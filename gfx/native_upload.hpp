#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace sr::gfx {
struct NativeUploadAllocation;
struct NativeUploadPage;
struct NativeUploadBudget;
struct NativeUploadPool;

// Step 6 R1a (J:/sr_lanec_drawthread/FREEZE_R1A_20261025.txt): SR_RENDER_KEEP_LEAN, read once, default on; =0 is the
// per-slice / per-bind retention of before. On: a recording retains each image allocation and each upload PAGE once
// (F1 / F2b), and a staged slice owns its page directly (no per-slice heap object). The test seam forces the mode
// (-1 = the environment, 0 = off, 1 = on) for a process that compares both arms.
bool renderKeepLean();
void renderKeepLeanForceForTest(int mode);

// Published bytes never change. A slice retains its page, including its mapped
// memory, independently of the arena. Keep it through actual GPU completion.
// R1a: the slice is (page owner, offset, size). With SR_RENDER_KEEP_LEAN=0 the owner is an aliasing pointer into a
// per-slice object holding the page (ownership() per slice, as before); with it on, the page itself (ownership() per
// PAGE: every slice staged on one page shares it, and a recording that retains one of them retains them all).
class NativeBufferSlice {
 public:
  ID3D12Resource* resource() const;
  ID3D12Device* device() const;
  uint64_t offset() const;
  uint64_t size() const;
  D3D12_GPU_VIRTUAL_ADDRESS address() const;
  std::span<const uint8_t> bytes() const;
  // Immutable staging metadata; no CPU read of mapped upload memory is needed.
  bool hasZeroPadding(uint64_t begin, uint64_t end) const;
  // Non-owning diagnostics used to verify recording/submission retention.
  std::weak_ptr<const void> ownership() const;
  // Both name the same staged range (one page, one append-only offset).
  bool sameStorage(const NativeBufferSlice& other) const {
    return m_page.get() == other.m_page.get() && m_offset == other.m_offset;
  }
  // R1a F2b: the recording-retention stamp of this slice's PAGE (a recording's serial; relaxed atomic). A recording
  // pushes a slice only when its page is not stamped with that recording's serial, and stamps it after the push.
  bool pageRetainedIn(uint64_t recordingSerial) const;
  void markPageRetainedIn(uint64_t recordingSerial) const;
  explicit operator bool() const { return bool(m_page); }
 private:
  std::shared_ptr<const NativeUploadPage> m_page;
  uint64_t m_offset = 0, m_size = 0, m_meaningful = 0;
  friend class NativeUploadArena;
};

// Append-only native upload pages. There is no range overwrite/recycle API.
// Released pages return budget only after the final slice/recording/submission
// owner disappears; a frame index cannot establish that a range is reusable.
// A released standard-size page is kept mapped in a small pool and reused whole
// for a later page (up to budget / page / 2 pages), instead of being destroyed.
class NativeUploadArena {
 public:
  HRESULT initialize(ID3D12Device* device, uint64_t pageBytes = 4u << 20,
                     uint64_t budgetBytes = 128u << 20);
  HRESULT stage(std::span<const uint8_t> bytes, uint32_t alignment, NativeBufferSlice& output);
  HRESULT stageConstantBuffer(std::span<const uint8_t> bytes, NativeBufferSlice& output);
  // One constant buffer gathered from parts (offset, bytes) written straight into the
  // page - no intermediate packed copy. Parts must not overlap; gaps and the tail up
  // to the 256-byte padding are zeroed. `bytes` is the meaningful size.
  // reserveBytes (0 = none): the slice is placed so that its page resource also spans
  // reserveBytes from the slice's start. The bytes past the slice are not the slice's
  // (later slices may own them); a read there stays inside the resource.
  struct Part { uint64_t offset; std::span<const uint8_t> bytes; };
  HRESULT stageConstantParts(std::span<const Part> parts, uint64_t bytes, NativeBufferSlice& output,
                             uint64_t reserveBytes = 0);
  uint64_t liveBytes() const;
  // Abandons the current tail. Outstanding slices retain their entire pages.
  void discardTail();
 private:
  HRESULT stagePadded(std::span<const uint8_t> bytes, uint32_t alignment,
                      uint64_t paddedBytes, NativeBufferSlice& output);
  HRESULT stageWrite(std::span<const Part> parts, uint64_t meaningful, uint32_t alignment,
                     uint64_t paddedBytes, NativeBufferSlice& output, uint64_t reserveBytes = 0);
  mutable std::mutex m_mutex;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  std::shared_ptr<NativeUploadBudget> m_budget;
  std::shared_ptr<NativeUploadPool> m_pool;
  std::shared_ptr<NativeUploadPage> m_tail;
  uint64_t m_pageBytes = 0;
};
}  // namespace sr::gfx
