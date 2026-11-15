#include "../kernel/sr_oracle.hpp"
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace sr::gfx {

enum class NativeImageKind : uint8_t { Color, DepthStencil };

// Native attachment properties only. Optimized clear metadata does not record
// a clear operation; recordClear is separate. The caller owns transition state.
struct NativeImageDesc {
  uint32_t width = 0, height = 0;
  DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
  NativeImageKind kind = NativeImageKind::Color;
  uint32_t samples = 1, sampleQuality = 0;
  D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
  D3D12_CLEAR_VALUE clear{};  // Format is the typed RTV/DSV format.
  D3D12_CLEAR_FLAGS depthClearFlags = D3D12_CLEAR_FLAG_DEPTH;
  bool nullColorView = false;  // Descriptor-only MRT hole, created by createNullColor.
};

struct NativeImageHandle {
  uint32_t slot = 0;  // zero is invalid
  uint64_t generation = 0;
  bool operator==(const NativeImageHandle&) const = default;
};

struct NativeImageAllocation;
struct NativeImageDescriptorHeap;

// Holds the allocation AND its RTV/DSV slot. A recording retains this lease
// until it is abandoned or transferred to submit. External SRVs and command
// allocator reuse remain the caller's responsibility.
class NativeImageLease {
 public:
  NativeImageHandle handle() const;
  ID3D12Resource* resource() const;
  ID3D12Device* device() const;
  bool isNullColor() const;
  std::weak_ptr<const void> ownership() const;
  D3D12_CPU_DESCRIPTOR_HANDLE view() const;
  const NativeImageDesc* description() const;
  explicit operator bool() const { return bool(m_image); }
  // Step 6 R1a F1 (SR_RENDER_KEEP_LEAN): the recording-retention stamp of this lease's ALLOCATION (a recording's
  // serial; relaxed atomic). A recording pushes a lease only when the allocation is not stamped with that recording's
  // serial, and stamps it after the push: a stale stamp can only cause a duplicate retain, never a miss.
  bool retainedIn(uint64_t recordingSerial) const;
  void markRetainedIn(uint64_t recordingSerial) const;

 private:
  std::shared_ptr<const NativeImageAllocation> m_image;
  friend class NativeImageStore;
};

struct NativeImageStats {
  size_t live = 0, pending = 0;
  uint32_t colorDescriptors = 0, depthDescriptors = 0;
};

// Owns native allocations, CPU attachment descriptors and a queue fence.
// No console addresses, tile identities, register files or emulation policy.
// Methods are thread-safe; recorded lists/allocators must not be reset until
// their returned successful submission completes. Call shutdown before destroy.
class NativeImageStore {
 public:
  ~NativeImageStore();
  HRESULT initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                     uint32_t colorCapacity = 512, uint32_t depthCapacity = 128);
  HRESULT create(const NativeImageDesc& desc, NativeImageLease& output);
  // Owns a typed null RTV descriptor without allocating a texture. The encoder
  // admits it only for a write-disabled MRT slot and never compacts slot indices.
  HRESULT createNullColor(DXGI_FORMAT format, DXGI_SAMPLE_DESC samples, NativeImageLease& output);
  HRESULT acquire(NativeImageHandle handle, NativeImageLease& output) const;
  HRESULT release(NativeImageHandle handle);
  HRESULT recordClear(ID3D12GraphicsCommandList* commands, const NativeImageLease& image) const;

  // Takes strong copies before execution. The caller supplies a CLOSED list
  // and every image it references, retaining leases while recording. Signal
  // failure after execution quarantines those copies; it never reports success.
  // Additional owners retain complete immutable draw packets through the same
  // checked fence. Empty owners are rejected before any queue execution.
  HRESULT submit(ID3D12CommandList* commands, ID3D12CommandAllocator* allocator,
                 std::span<const NativeImageLease> images, uint64_t& submission,
                 std::span<const std::shared_ptr<const void>> owners = {});
  HRESULT collect();
  // Checked marker after ALL prior work on the bound queue, including lists
  // submitted by development adapters. It does not execute an unsubmitted list.
  HRESULT checkpoint(uint64_t& submission);
  HRESULT wait(uint64_t submission, uint32_t timeoutMs) const;
  // Observation only: no signal, event, wait, collect or ownership mutation. (step 6 I3-B: the raw ring reads it in
  // release builds too.)
  HRESULT completionSnapshot(uint64_t& completed) const;
  // Stops admission, signals AFTER all prior work on the bound queue, waits
  // within the supplied bound and clears ownership only on observed completion.
  HRESULT shutdown(uint32_t timeoutMs = 10000);
  NativeImageStats stats() const;

 private:
  HRESULT publish(const std::shared_ptr<NativeImageAllocation>& image, NativeImageLease& output);
  struct Slot {
    uint64_t generation = 0;
    std::shared_ptr<const NativeImageAllocation> image;
  };
  struct Submission {
    uint64_t fence = 0;
    Microsoft::WRL::ComPtr<ID3D12CommandList> commands;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    std::vector<NativeImageLease> images;
    std::vector<std::shared_ptr<const void>> owners;
  };
  mutable std::mutex m_mutex;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
  Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
  std::shared_ptr<const uint8_t> m_identity;
  std::shared_ptr<NativeImageDescriptorHeap> m_colorHeap, m_depthHeap;
  std::vector<Slot> m_slots;
  std::vector<Submission> m_pending;
  uint64_t m_nextFence = 1, m_lastSubmitted = 0;
  bool m_accepting = false, m_closed = false;
};

}  // namespace sr::gfx
