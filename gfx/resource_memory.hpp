#pragma once
// Native resource-memory protocol. CPU invalidation comes from the guest's writes,
// never a byte hash or a write-protection fault. Upload completion is explicit:
// taking a ticket cannot make an allocation current before its bytes are copied.
#include "resource_registry.hpp"

#include <array>
#include <mutex>
#include <vector>

namespace sr::gfx::memres {

struct ResourceSpan { uint32_t phys = 0, len = 0; };
uint32_t resourceTextureSpans(const uint32_t* fetch, ResourceSpan out[2]) noexcept;
uint32_t resourcePhysOfVa(uint32_t va) noexcept;

class ResourceMemory {
 public:
  struct UploadTicket {
    uint32_t page = kPages, cpu = 0, gpu = 0;
    Decision decision = Decision::kKeep;
    uint32_t window = 8;
  };
  ResourceMemory();
  ResourceMemory(const ResourceMemory&) = delete;
  ResourceMemory& operator=(const ResourceMemory&) = delete;

  uint32_t noteCpu(uint32_t phys, uint32_t len, Src source) noexcept;
  uint32_t noteGpu(uint32_t phys, uint32_t len) noexcept;
  uint32_t reserveGpu() noexcept { return registry_.reserveGeneration(); }
  uint32_t noteGpuAt(uint32_t phys, uint32_t len, uint32_t generation) noexcept;
  void noteCoherency(uint32_t status, uint32_t phys, uint32_t len) noexcept;
  void noteFileRead(uint32_t va, uint32_t len) noexcept;
  void noteGrant(uint32_t va, uint32_t len) noexcept;
  // Completed inline vertex bytes, before EndVertices / UP publishes its cursor.
  void noteInlineWrite(uint32_t va, uint32_t len) noexcept;
  void noteTextureUnlock(const uint32_t* fetch, bool moviePlane = false) noexcept;
  // The known decoder writes registered planes without a Lock. Until its output
  // boundary is hooked, their pages are copied once per submitted frame.
  void beginFrame() noexcept;

  // EXACT CPU writes inside a page that no coherency record announces: the D3D
  // header fields the driver stores on the CPU (D3DDevice_SetStreamSource 820F2A50
  // stamping the outgoing vertex buffer's last-use fence at +8, SetIndices 820F2C08,
  // SetTexture 82100310, SetVertexShader 820F5218 / SetPixelShader 820F4E88 the
  // same for theirs). The console GPU reads unified memory, so the arena must
  // hold these bytes too - but a page-granular note would upload RAM over the
  // GPU-written bytes of the same page. A patch names dwords: it reserves no
  // generation and leaves the page's decision unchanged; the next request of the
  // page copies exactly those dwords from the window each was written through
  // (takePatch), and a whole-page upload, whose copy contains the dwords of its
  // own window, drops those (the caller clears them). Lock-free; any thread.
  struct Patch {
    uint32_t page = kPages;
    // [0] written through 0x8, [1] through 0xA (the arena keeps the two windows
    // apart; the dwords of each are read from their own); bit d = page offset 4 * d.
    std::array<std::array<uint64_t, 16>, 2> dwords{};
    static uint32_t window(uint32_t i) noexcept { return i ? 0xAu : 8u; }
    bool any() const noexcept {
      for (const auto& w : dwords)
        for (uint64_t m : w)
          if (m) return true;
      return false;
    }
  };
  void notePatch(uint32_t va, uint32_t len) noexcept;
  bool patchPending(uint32_t page) const noexcept {
    return page < kPages &&
        (patchPending_[page >> 6].load(std::memory_order_acquire) >> (page & 63u) & 1u) != 0;
  }
  bool patchPendingRange(uint32_t firstPage, uint32_t lastPage) const noexcept;
  // Moves each time a page's pending bit is set from clear (a take clears it): while it holds, no page gained a
  // pending patch - the draw path's texture-slot skip (perf parity round 2) keys on it.
  uint64_t patchArmed() const noexcept { return patchArmed_.load(std::memory_order_acquire); }
  // Take (and clear) the page's pending dwords: false when none. A patch noted
  // after the take stays pending.
  bool takePatch(uint32_t page, Patch& out) noexcept;
  // A GPU write the registry hears (noteGpu / noteGpuAt) supersedes the pending patched dwords it covers, in both
  // windows: the GPU's result is newer than the CPU bytes those patches would copy (Astra round 2, finding 1).
  // fast: the notes the lean path (SR_PATCH_LEAN) answered without a write - their dword and page were still pending.
  struct PatchStats { uint64_t notes = 0, dwords = 0, refused = 0, taken = 0, superseded = 0, fast = 0; };
  PatchStats patchStats() const noexcept;
  // step 6 G8 (SR_PATCH_LEAN, default on; =0 the A/B's off arm): a note of one whole dword whose bit and page are both
  // still pending writes nothing (resource_memory.cpp notePatch). Tests set it per object.
  void setPatchLean(bool on) noexcept { patchLean_ = on; }

  // A completed CPU writer names its actual write window, then a live owner,
  // then the most recent physical grant, then 0x8.
  // The remembered grant survives free because queued draws can outlive it.
  uint32_t sourceWindow(uint32_t page, int liveOwner) const noexcept;
  UploadTicket prepare(uint32_t page, int liveOwner = 0) noexcept;
  // The registry's decision on the announced generations, lock-free (relaxed
  // reads). An upload must still take prepare()'s ticket: its window and
  // generations are frozen together under the metadata lock.
  Decision decide(uint32_t page) const noexcept { return registry_.decide(page); }
  bool commit(const UploadTicket& ticket) noexcept;
  uint32_t version(uint32_t phys, uint32_t len) const noexcept;
  uint32_t cpuGeneration(uint32_t page) const noexcept { return registry_.cpuGen(page); }
  uint32_t gpuGeneration(uint32_t page) const noexcept { return registry_.gpuGen(page); }
  uint32_t uploadedGeneration(uint32_t page) const noexcept { return registry_.uploadedGen(page); }
  // The page's last CPU-write note source (the texture-identity census's cpu_after_gpu samples).
  Src lastSrc(uint32_t page) const noexcept { return registry_.lastSrc(page); }
  // The frozen-pass A/B (gfx/resource_ab.hpp INV-R): the arena no longer holds this registry's bytes for `page`.
  void forgetUpload(uint32_t page) noexcept;
  uint32_t generation() const noexcept { return registry_.clock(); }
  uint64_t sequence() const noexcept { return registry_.sequence(); }
  bool unchangedSince(uint32_t firstPage, uint32_t lastPage, uint64_t since) const noexcept {
    return registry_.unchangedSince(firstPage, lastPage, since);
  }

  static constexpr uint32_t kDirtyBlockLog2 = 6;
  void takeDirtyBlocks(std::vector<uint32_t>& out);

 private:
  // A ticket's generation and CPU alias must describe the same completed
  // writer. Notes publish both under this lock; copying uses the frozen ticket.
  mutable std::recursive_mutex metadataMutex_;
  Registry registry_;
  std::unique_ptr<std::atomic<uint8_t>[]> grantWindow_;
  // Actual completed CPU write window (0 = unknown), guarded by metadataMutex_.
  // GPU ownership never changes CPU bytes or their source alias.
  std::unique_ptr<uint8_t[]> cpuWindow_;
  uint32_t noteCpuWindow(uint32_t phys, uint32_t len, Src source, uint8_t window) noexcept;
  std::array<std::atomic<uint64_t>, kPages / (64u << kDirtyBlockLog2)> dirty_{};
  std::array<std::atomic<uint64_t>, kPages / 64u> movie_{};
  // notePatch's dwords: a block per page allocated at its first patch (never
  // freed: the pages holding driver-stamped headers), and a pending bit per page.
  struct PatchBlock {
    std::array<std::array<std::atomic<uint64_t>, 16>, 2> dwords{};  // [window 8, window 0xA]
  };
  std::unique_ptr<std::atomic<PatchBlock*>[]> patchBlocks_;
  std::array<std::atomic<uint64_t>, kPages / 64u> patchPending_{};
  std::atomic<uint64_t> patchRefused_{0}, patchTaken_{0}, patchSuperseded_{0};
  std::atomic<uint64_t> patchArmed_{0};
  // notePatch's per-call counters, one cache line per shard (a thread keeps its shard): the draw thread's stamps
  // never write a line the CP's take or another thread writes (step 6 G8).
  struct alignas(64) PatchShard { std::atomic<uint64_t> notes{0}, dwords{0}, fast{0}; };
  static constexpr uint32_t kPatchShards = 16;
  std::array<PatchShard, kPatchShards> patchShards_{};
  bool patchLean_;
  // step 6 G8c: notePatch's full path (everything after the lean early-out), out of line, so a fast note pays none of
  // its register saves.
#if defined(__clang__)
  __attribute__((noinline))
#endif
  void notePatchSlow(uint32_t va, uint32_t len, uint32_t set, uint8_t window, PatchShard& shard) noexcept;
  void supersedePatches(uint32_t phys, uint32_t len) noexcept;
};

// The native resource route. Release (SR_ORACLE=0): always. Dev / Oracle build: SR_MEM_RESOURCES, BAKED default ON
// since 2026-10-17 (lane C, native/mem-native): unset or empty = the native route; "0" = the page model (the A/B and
// oracle arm); "census" = the page model with the registry in shadow (gfx/resource_census.hpp); any other value
// ("1" the documented one) = the native route, explicitly.
bool resourceMode() noexcept;
// step 6 G8: SR_PATCH_LEAN as read at startup (default on). Off is the whole pre-G8 path: no lean note, one counter
// line, the stamp bracket's per-call route reads (kernel/native_resource_stamps.cpp).
bool patchLeanOn() noexcept;
// How resourceMode() was decided: "default" (unset), "explicit", "off" (=0), "census", "release" (SR_ORACLE=0).
const char* resourceRouteForm() noexcept;
ResourceMemory& resourceMemory();
uint64_t resourceElapsedMilliseconds() noexcept;

}  // namespace sr::gfx::memres
