#pragma once
// iter-466 cp lane: CPU-write tracking for the PM4 shared memory (SR_SHM_WRITE_TRACK).
//
// Xenia's model (memory.cc PhysicalHeap::EnableAccessCallbacks / TriggerCallbacks,
// gpu/shared_memory.cc MakeRangeValid / MemoryInvalidationCallback; BSD-3, attribution in
// CREDITS.md): a page the GPU has read is write-protected; the first CPU write faults, the
// handler unprotects it and marks it invalid, and only invalid pages are uploaded again. Here
// the validation stays a hash compare (gfx/d3d12_pm4_shmem.cpp pm4ShmValidate) - what this
// adds is the proof that a page was NOT written since its last validation, so the hash can be
// skipped: a page is protected BEFORE it is hashed, so any write after the hash faults.
//
// Our flat guest arena does not fold the physical windows (kernel/phys_window.hpp): one
// physical page has a copy at 0x80000000|p and one at 0xA0000000|p, and the validation reads
// either. Both copies are protected; a write to either clears the page.
//
// A fault unprotects the whole 64-page (256 KB) block around it (Xenia: "a lot cheaper to
// upload some excess data than to catch access violations"); here the excess only costs a
// re-hash. A block that faults is not protected again for 1, 2, 4 .. 64 frames (doubling per
// fault), so a ring the guest rewrites every frame settles at one fault per 64 frames and is
// hashed per drain exactly as without tracking.
//
// Kernel-mode writers (ReadFile into guest memory) do not fault into the handler - they fail
// with ERROR_NOACCESS. They call hostWillWrite() first (kernel/stubs.cpp NtReadFile).
#include <cstdint>
#include <cstddef>
#include <vector>

namespace sr::kernel::gww {

constexpr uint32_t kPages = 1u << 17;  // 512 MiB of 4 KB physical pages
constexpr uint32_t kBlockLog2 = 6;     // 64 pages per fault block

bool enabled();      // SR_SHM_WRITE_TRACK (default ON since iter-466 cp; =0 off)
bool checkMode();    // SR_SHM_WT_CHECK=1: skipped pages are hashed anyway and compared

// Fresh CPU-store admission in the installed tracker's physical windows. Every
// page must be committed and writable, or read-only under this tracker's own
// protection. Does not install the handler, unprotect pages or invalidate data.
// Ordinary CPU stores still fault through the handler and release dirty blocks;
// kernel-mode writers must use HostWriteScope. The caller owns the mapping and
// external protection lifetime through its stores, as with VirtualQuery.
bool cpuWritableRange(const void* pointer, size_t bytes) noexcept;

// A page is skippable when it is protected, was validated after the protection, and that
// validation took the window `own` (8 / 0xA) the new request names.
bool clean(uint32_t page, uint8_t own);
// The window of the page's validation when it is clean (protected and validated since), else -1.
int cleanOwn(uint32_t page);
// The page's protection generation while it is protected, else 0. Content cannot change while
// protGen stays the same non-zero value, so a hash taken AFTER the protection began (protGen read
// before hashing, unchanged after) stays current for as long as protGen returns that value.
uint32_t protGen(uint32_t page);

// Before hashing: protect every page of pages[0..n) that is eligible (not protected yet, its
// block not cooling down). Thread-safe.
void protectForValidation(const uint32_t* pages, uint32_t n);
// After the upload decision: the page's validation used window `own`; it becomes skippable
// if it is still protected.
void markValidated(uint32_t page, uint8_t own);
// The same for a validation that read protGen(page) == gen0 right after its protectForValidation: counts a mark made
// under a later generation (another protector re-protected the page after a write since the hash) and, under
// SR_SHM_WT_GEN_MARK=1, undoes it. Returns whether the page is now skippable.
bool markValidated(uint32_t page, uint8_t own, uint32_t gen0);

// A host kernel-mode write into guest memory is about to happen: unprotect the watched pages
// of [guestVA, guestVA + len) (0x8 / 0xA windows only; anything else is a no-op) and keep them
// unprotected until the scope ends (a long ReadFile must not see the CP re-protect them).
class HostWriteScope {
 public:
  HostWriteScope(uint32_t guestVA, uint32_t len);
  ~HostWriteScope();
  HostWriteScope(const HostWriteScope&) = delete;
  HostWriteScope& operator=(const HostWriteScope&) = delete;

 private:
  int slot_ = -1;
  bool armed_ = false;
};

// Frame boundary (the CP's shared-memory frame): cooldowns count in frames.
void beginFrame();

// iter-467 native-b (the shared-memory range cache, gfx/d3d12_pm4_shmem.cpp SR_NB_SHM_RANGE_CACHE): every release
// of a 64-page block (a CPU write fault or a host write) takes the next value of a global release clock and stamps
// its block with it BEFORE any of its pages is unprotected. Releases are serialised (g_lock), so when the clock
// reads S every release stamped < S has finished clearing its pages and at most the one stamped S is still in
// flight: a range whose pages were all clean in a walk that began after the clock read S stays clean while none of
// its blocks carries a stamp >= S (blocksReleasedSince). Lock-free.
uint64_t releaseClock();
bool blocksReleasedSince(uint32_t block0, uint32_t block1, uint64_t stamp);

// The 64-page blocks that released protected pages (a fault or a host write) since the last
// call, cleared as they are taken (the texture cache's present-time invalidation).
void takeReleasedBlocks(std::vector<uint32_t>& out);

struct Stats {
  uint64_t faults, faultPages, protects, protectPages, hostWrites, skipped, staleChecks, stale;
  uint64_t genLate, genRefused;  // markValidated(.., gen0): marks under a later generation / undone
};
Stats stats();
void noteSkipped(uint32_t n);
void noteCheck(bool stale);

}  // namespace sr::kernel::gww
