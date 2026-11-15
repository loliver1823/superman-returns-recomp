#pragma once
#include "native_image_store.hpp"
#include "native_pipeline.hpp"
#include "native_upload.hpp"

namespace sr::gfx {
struct NativeRootAllocation;
struct NativeDescriptorAllocation;
struct NativeDescriptorArenaState;
struct NativeDrawPayload;

// Owns the root object and the binding rules derived from the exact native
// description used to create it. No console register ABI is inferred here.
class NativeRootSignature {
 public:
  // Buffer extents are an explicit native shader/ABI contract, indexed by root
  // parameter. CBV/SRV entries must be nonzero; other entries must be zero.
  // The caller proves dynamic shader addresses; root descriptors have no GPU
  // bounds. This service checks owned storage against the declared extents.
  static HRESULT create(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
                        NativeRootSignature& output,
                        std::span<const uint32_t> requiredBufferBytes = {});
  ID3D12RootSignature* root() const;
  ID3D12Device* device() const;
  std::weak_ptr<const void> ownership() const;
  explicit operator bool() const { return bool(m_root); }
 private:
  std::shared_ptr<const NativeRootAllocation> m_root;
  friend class NativeDrawRecording;
};

struct NativeViewRecipe {
  // SRV recipes use resource for native GPU-owned resources, or buffer for
  // immutable upload data. Null resource/buffer explicitly creates a null SRV.
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  NativeBufferSlice buffer;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  D3D12_SAMPLER_DESC sampler{};
};

// An immutable shader-visible descriptor version. Every referenced allocation
// is owned with the heap; changes require a new version, never a slot overwrite.
class NativeDescriptorVersion {
 public:
  static HRESULT create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                        std::span<const NativeViewRecipe> recipes, NativeDescriptorVersion& output);
  ID3D12DescriptorHeap* heap() const;
  ID3D12Device* device() const;
  D3D12_DESCRIPTOR_HEAP_TYPE type() const;
  uint32_t count() const;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t first = 0) const;
  std::weak_ptr<const void> ownership() const;
  // Arena page serial (1-based, increasing with each page the arena opens);
  // 0 for a version that owns its own heap.
  uint64_t pageSerial() const;
  explicit operator bool() const { return bool(m_descriptors); }
 private:
  std::shared_ptr<const NativeDescriptorAllocation> m_descriptors;
  friend class NativeDescriptorArena;
  friend class NativeDrawRecording;
};

// Append-only SRV pages. Versions keep their page and referenced resources
// alive independently of the arena; a page is recycled only after its last
// version retires. Changing a binding never rewrites a published descriptor.
class NativeDescriptorArena {
 public:
  HRESULT initialize(ID3D12Device* device, uint32_t pageDescriptors = 16384,
                     uint32_t maxPages = 32);
  HRESULT create(std::span<const NativeViewRecipe> recipes, NativeDescriptorVersion& output);
  size_t livePages() const;
  // Serial of the page new versions go to (0 before the first allocation).
  uint64_t tailSerial() const;
  void discardTail();
 private:
  mutable std::mutex m_mutex;
  std::shared_ptr<NativeDescriptorArenaState> m_state;
};

struct NativeRootBinding {
  uint32_t parameter = 0;
  NativeBufferSlice buffer;
  NativeDescriptorVersion table;
  uint32_t firstDescriptor = 0, descriptorCount = 0;
  std::vector<uint32_t> constants;
};

enum class NativeDrawKind : uint8_t { DecodedVertices, FetchVertices, FetchIndexedVertices };
struct PreparedNativeDraw {
  NativePipelineLease pipeline;
  NativeRootSignature signature;
  std::vector<NativeRootBinding> bindings;
  std::array<NativeImageLease, 8> colors;
  NativeImageLease depth;
  D3D12_VIEWPORT viewport{};
  D3D12_RECT scissor{};
  uint32_t stencilReference = 0;
  D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  NativeDrawKind kind = NativeDrawKind::DecodedVertices;
  NativeBufferSlice vertices, indices;
  uint32_t vertexStride = 0, count = 0, first = 0;
  DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
};

// Owns one DIRECT list and its allocator. Validate all draw inputs and retain
// the complete immutable packet BEFORE recording any draw state. Submission
// transfers those owners to the checked native queue fence; frame rotation,
// cache clearing or a mid-frame flush cannot release the submitted packet.
class NativeDrawRecording {
 public:
  NativeDrawRecording() = default;
  NativeDrawRecording(const NativeDrawRecording&) = delete;
  NativeDrawRecording& operator=(const NativeDrawRecording&) = delete;
  HRESULT begin(ID3D12Device* device);
  ID3D12GraphicsCommandList* commands() const;
  // Keep other native resources before recording clears, copies or transitions
  // on commands(). Callers never Reset this list or allocator themselves.
  HRESULT keep(ID3D12Resource* resource);
  HRESULT keep(const NativeBufferSlice& buffer);
  HRESULT keep(const NativeImageLease& image);
  HRESULT keep(const NativeDescriptorVersion& descriptors);
  HRESULT draw(const PreparedNativeDraw& draw);
  HRESULT close();
  HRESULT submit(NativeImageStore& queue, uint64_t& submission);
  size_t drawCount() const;
  // Step 6 R1a (FREEZE_R1A_20261025): this recording's serial (process-wide, monotonic, never reused; 0 before
  // begin), and the payload's retained image / upload counts (diagnostics: with SR_RENDER_KEEP_LEAN on, one per
  // distinct image allocation / upload page; =0, one per keep as before).
  uint64_t serial() const { return m_serial; }
  size_t retainedImageCount() const;
  size_t retainedUploadCount() const;
  // Whether the recording payload `payload` (ownership()) retains an upload whose owner is `owner` (a slice's
  // ownership(): its page with SR_RENDER_KEEP_LEAN on, the slice itself with =0). Holds no strong reference.
  static bool payloadRetainsUpload(const std::weak_ptr<const void>& payload, const std::weak_ptr<const void>& owner);
#if SR_ORACLE
  // R1a failing controls (tests only): 0 = none; 1 = skip-page-keep (the page is stamped as retained but the slice is
  // not pushed); 2 = release-at-slice-death (an upload keep retains nothing: the page lives only as long as slices).
  static void uploadKeepMutantForTest(int mutant);
#endif
#if SR_ORACLE
  // Actual make_shared combined object/control-block allocation on this STL.
  // Unknown implementations refuse diagnostic admission rather than guess.
  static size_t ownershipMetadataBytes();
#endif
  std::weak_ptr<const void> ownership() const;
 private:
  enum class Phase : uint8_t { Empty, Recording, Closed, Spent };
  Phase m_phase = Phase::Empty;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commands;
  std::shared_ptr<NativeDrawPayload> m_payload;
  uint64_t m_serial = 0;
};
}  // namespace sr::gfx
