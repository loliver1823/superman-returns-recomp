#pragma once
// mem.resources step 2 (docs/RELEASE_COMPOSITION.md section 9.2; ledger `mem.resources`): THE RESOURCE REGISTRY - the
// title's own resource protocol in place of the shared-memory page model's byte comparisons. A page is stale when the
// guest SAID so (a coherency record, a host write such as NtReadFile, a texture Unlock), not when its bytes changed.
// A coherency record is a CPU-write note only for the statuses that announce one (Src below): the Resolve's 0x03000100
// follows the GPU's own write into the destination and notes nothing (the GPU write is noteGpuWrite).
//
// Per 4 KiB guest physical page (512 MiB = 131,072 pages) three generations of one global clock:
//   cpuGen       the clock at the newest CPU-write note covering the page (noteCpuWrite);
//   gpuGen       the clock at the newest GPU write into the page (noteGpuWrite: a resolve's destination extent);
//   uploadedGen  what the arena holds: the cpuGen of the last upload (kFirstUseGen for a never-noted page's first one).
// ensure(page): gpuGen != 0 && gpuGen >= cpuGen -> the arena holds the GPU's data (Xenia's valid_and_gpu_written as a
// comparison of generations instead of hashes): keep. Else uploadedGen == 0 -> the page's first use: upload (a FIRST-
// USE upload when cpuGen == 0: a never-noted page - a static resource written before any boundary, or a writer with no
// boundary - is then current until a note arrives; a noted upload otherwise). Else cpuGen > uploadedGen -> a note
// since the last upload: upload. Else keep.
// version(range) = the newest of cpuGen / gpuGen over the range (pm4ShmVersion's contract).
//
// Step 2a (this unit's first use) runs the registry in SHADOW beside the page model (gfx/resource_census.hpp,
// SR_MEM_RESOURCES=census): it decides, the model decides, the decisions are compared per page. Nothing reads the
// registry's answers to draw. Class native (docs/release_composition.json): it holds no emulation - generations of
// the guest's own announcements - and step 2b makes it the default path's answer. The window a page is uploaded from
// (the allocation's provenance) is the caller's, not the registry's: the census compares it with the model's.
//
// Threads: notes arrive on the CP thread (the coherency records, applied in stream order), the guest threads
// (NtReadFile, the Unlock hook) and the render thread (a resolve's GPU write); ensure runs on the CP thread (the drain)
// and the render thread (the texture prepass). Every table entry is a relaxed atomic; a note raises a page's
// generation with a compare-exchange max, so a page's generation never moves backwards when two notes race. A caller
// that reads the bytes after deciding binds the decision to the generations it read first (ensureAt): a note landing
// between that read and the copy stays pending rather than being claimed by bytes that may predate its write.
#include <atomic>
#include <cstdint>
#include <memory>

namespace sr::gfx::memres {

constexpr uint32_t kPageLog2 = 12;
constexpr uint32_t kPhysBytes = 0x20000000u;  // 512 MiB of guest physical memory
constexpr uint32_t kPages = kPhysBytes >> kPageLog2;
constexpr uint32_t kFirstUseGen = 1;  // uploadedGen of a never-noted page after its first-use upload (the clock starts above it)

// Who announced the newest CPU write of a page (the census attributes a missed boundary to it). The coherency records
// are told apart by their COHER_STATUS_HOST value (memres1's classification, 2026-10-16: the guest issues exactly four
// - 0x03000000, 0x02000000, 0x01000000 and the Resolve's 0x03000100, which is a GPU write's request, never a CPU note).
enum class Src : uint8_t {
  kNone = 0,   // never noted
  kInval,      // COHER_STATUS_HOST 0x03000000: a CPU-write range - the VB lock (820F3FA0 -> the lock core 820F3988),
               // GpuInvalidateMemoryRange 820F4280, the close's flush of the deferred range dev+0x2D18
  kTexLock,    // 0x02000000: the texture lock's request (TextureRangeGpuRingCommand 820FF468, kRkCoherHost)
  kCmdBuf,     // 0x01000000: the close's vertex-cache invalidate over the WHOLE command buffer [dev+0x34AC, dev+0x34B0)
               // (GpuBuildFlushMemoryPackets 820FD3B8 once byte dev+0x2A3A bit 2 - set at the buffer's first wrap - is
               // set): the console's only boundary for the inline vertices BeginVertices / DrawVerticesUP carve from it
  kOther,      // any other status (none expected: counted and sampled; noted as a CPU write, the conservative side)
  kReadFile,   // NtReadFile into guest memory (kernel/stubs.cpp: the synchronous read and the async read worker)
  kUnlock,     // the texture Unlock hook (sub_820FFCC8, kernel/hle_api_observe.cpp)
  kGpu,        // a GPU write (noteGpuWrite) - the newest generation of the page is the GPU's
  kCount
};
const char* srcName(Src s) noexcept;

enum class Decision : uint8_t {
  kKeep = 0,      // current: no note since the last upload
  kKeepGpu,       // the arena holds the GPU's data (gpuGen >= cpuGen)
  kUploadNoted,   // a note since the last upload
  kUploadFirst,   // the page's first use (never uploaded)
};
inline bool isUpload(Decision d) noexcept { return d == Decision::kUploadNoted || d == Decision::kUploadFirst; }

class Registry {
 public:
  Registry();
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // [phys, phys + len) in guest physical bytes (bit 29 and up must be clear: a range at or past 512 MiB - the
  // all-ones sentinel {0xFFFFF000, 0x1000} - is refused and counted; a range crossing the end is clamped).
  // Returns the pages noted (0 = refused or empty).
  uint32_t noteCpuWrite(uint32_t phys, uint32_t len, Src src) noexcept;
  uint32_t noteGpuWrite(uint32_t phys, uint32_t len) noexcept;
  // Reserve at command production; publish the same generation when a deferred
  // resolve's extent becomes known. Execution time must not reorder guest writes.
  uint32_t reserveGeneration() noexcept;
  // Refuses exhaustion without changing the clock or any page. Production
  // reservation stops deterministically rather than wrapping ownership to zero.
  bool tryReserveGeneration(uint32_t& generation) noexcept;
  uint32_t noteGpuWriteAt(uint32_t phys, uint32_t len, uint32_t generation) noexcept;

  // The rule on three generations (cpu, gpu, uploaded) - decide() and ensureAt() apply it.
  static Decision decideGens(uint32_t cpu, uint32_t gpu, uint32_t uploaded) noexcept;
  // The registry's decision for one page, without changing anything.
  Decision decide(uint32_t page) const noexcept;
  // decide + commit: an upload records the generation the arena now holds.
  Decision ensurePage(uint32_t page) noexcept;
  // ensurePage BOUND TO THE BYTES COPIED: the decision on the generations the caller read BEFORE it took the bytes
  // (cpu, gpu), and an upload commits that cpu generation - a note that lands after the read stays pending (its write
  // may not be in the bytes), so the next ensure uploads again. ensurePage(p) = ensureAt(p, cpuGen(p), gpuGen(p)).
  Decision ensureAt(uint32_t page, uint32_t cpu, uint32_t gpu) noexcept;
  uint32_t version(uint32_t phys, uint32_t len) const noexcept;

  uint32_t cpuGen(uint32_t page) const noexcept { return page < kPages ? cpu_[page].load(std::memory_order_relaxed) : 0u; }
  uint32_t gpuGen(uint32_t page) const noexcept { return page < kPages ? gpu_[page].load(std::memory_order_relaxed) : 0u; }
  uint32_t uploadedGen(uint32_t page) const noexcept {
    return page < kPages ? up_[page].load(std::memory_order_relaxed) : 0u;
  }
  // Forget what the arena holds for `page` (its next ensure uploads): the frozen-pass A/B (gfx/resource_ab.hpp INV-R),
  // when the page model's upload put another window's bytes into the arena. Callers serialise with the commits.
  void forgetUpload(uint32_t page) noexcept {
    if (page >= kPages) return;
    up_[page].store(0, std::memory_order_relaxed);
    markChanged(page, page);  // an upload reset moves the sequence (the request memo re-walks)
  }
  Src lastSrc(uint32_t page) const noexcept {
    return page < kPages ? Src(src_[page].load(std::memory_order_relaxed)) : Src::kNone;
  }
  uint32_t clock() const noexcept { return clock_.load(std::memory_order_relaxed); }
  // Moves after every note that changed a page's cpu / gpu generation or reset its upload (a deferred GPU
  // write publishes an already-reserved generation without moving the clock). A caller that found a range
  // current may reuse that answer while this sequence holds (the native request memo).
  uint64_t sequence() const noexcept { return seq_.load(std::memory_order_acquire); }
  // The per-block form (perf parity, prof2_ng_native): true while no note changed a page of [p0, p1]'s 64-page
  // blocks after `since` (a sequence() read before the caller's walk of the range). Every note raises its blocks'
  // change sequence to its own (an atomic max: a slower older note never lowers a newer one).
  static constexpr uint32_t kSeqBlockLog2 = 6;
  bool unchangedSince(uint32_t p0, uint32_t p1, uint64_t since) const noexcept {
    if (p0 > p1 || p0 >= kPages) return false;
    p1 = p1 < kPages ? p1 : kPages - 1u;
    for (uint32_t b = p0 >> kSeqBlockLog2; b <= (p1 >> kSeqBlockLog2); ++b)
      if (blockSeq_[b].load(std::memory_order_acquire) > since) return false;
    return true;
  }
  uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }
  void reset(uint32_t initialGeneration = kFirstUseGen) noexcept;  // tests; caller owns all registry use

  // The page range [p0, p1] of a byte range, clamped to the 512 MiB table; false = refused (out of range / empty).
  static bool pageRange(uint32_t phys, uint32_t len, uint32_t& p0, uint32_t& p1) noexcept;

 private:
  std::unique_ptr<std::atomic<uint32_t>[]> cpu_, gpu_, up_;
  std::unique_ptr<std::atomic<uint8_t>[]> src_;
  std::atomic<uint32_t> clock_{kFirstUseGen};
  std::atomic<uint64_t> refused_{0};
  std::atomic<uint64_t> seq_{0};
  std::unique_ptr<std::atomic<uint64_t>[]> blockSeq_;
  // After the pages' raises: the global sequence moves, then each touched block's change sequence to it.
  void markChanged(uint32_t p0, uint32_t p1) noexcept;
};

}  // namespace sr::gfx::memres
