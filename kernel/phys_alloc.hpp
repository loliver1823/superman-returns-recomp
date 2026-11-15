#pragma once
// iter-458 H-FREE: A REAL PHYSICAL ALLOCATOR (SR_PHYS_FREE=1, default OFF).
//
// The console kernel's MmAllocatePhysicalMemoryEx / MmFreePhysicalMemory manage
// ONE 512 MB physical RAM: a freed block is returned and later allocations reuse
// it. The game depends on that: ReserveMemoryWithFallback_sub_82167FA8 holds
// 0x12B00000 bytes through boot and GpuCreateBufferResourcePool_sub_82183DC8
// releases it (sub_824680A8 -> MmFreePhysicalMemory) for the GPU pools that
// follow; sub_827C0908 frees pool-page chains through XMemFree. Our kernel
// registered MmFreePhysicalMemory as a no-op stub and guestPhysAlloc was a
// bump pointer, so nothing was ever reused.
//
// Semantics follow Xenia xboxkrnl_memory.cc xeMmAllocatePhysicalMemoryEx:
//   * protect must carry PAGE_READONLY or PAGE_READWRITE, else fail;
//   * page size 4 KB, 64 KB with X_MEM_LARGE_PAGES, 16 MB with X_MEM_16MB_PAGES;
//     size AND alignment round up to the page size;
//   * min/max_addr_range bound the PHYSICAL address;
//   * top-down first fit; one physical space shared by every page size.
// DEVIATION (documented): every block is returned in the 0xA window
// (VA = 0xA0000000 | physical). Xenia returns 4 KB-page blocks in 0xE0000000
// and 16 MB-page blocks in 0xC0000000, all aliasing one RAM; our flat arena does
// not alias those windows (aliasGuestVA folds 0xC/0xE onto 0x8), so a single
// window is the only one in which a physical address names exactly one byte.
// The arena is [0xA0000000, 0xBF000000) - 0xBF000000+ stays clear of the
// 0xC0000000 fold.
#include <cstdint>

namespace sr {
namespace kernel {
namespace physalloc {

constexpr uint32_t kLo = 0xA0000000u;
constexpr uint32_t kHi = 0xBF000000u;
constexpr uint32_t kPage = 0x1000u;

constexpr uint32_t kXPageReadOnly = 0x02u;
constexpr uint32_t kXPageReadWrite = 0x04u;
constexpr uint32_t kXMemLargePages = 0x20000000u;
constexpr uint32_t kXMem16MbPages = 0x80000000u;

bool physFreeEnabled();   // SR_PHYS_FREE=1 (unique name: scripts/audit_forcing.py keys gate predicates by bare function name)

// Kernel-export entry points. They check the gate THEMSELVES so the export
// file never names the gate predicate (scripts/audit_forcing.py attributes
// every site in stubs.cpp's namespace block to any predicate named there).
// serviceAllocate: false = gate off (caller keeps its legacy path); true =
// handled, `out` holds the result (0 on failure).
bool serviceAllocate(uint32_t size, uint32_t protect, uint32_t minPhys,
                     uint32_t maxPhys, uint32_t alignment, uint32_t& out);
// serviceFree: no-op with the gate off.
void serviceFree(uint32_t base);

// The MmAllocatePhysicalMemoryEx service. Returns 0 on failure.
uint32_t mmAllocate(uint32_t size, uint32_t protect, uint32_t minPhys,
                    uint32_t maxPhys, uint32_t alignment);

// Page-granular allocation for host-side callers (thread stacks, the
// sub_82467F40 override's small GPU route): 4 KB pages, any physical address.
uint32_t alloc(uint32_t size, uint32_t align);

// Frees the LIVE block starting exactly at `base`. Anything else (unknown base,
// interior pointer, double free, a heap-routed grant) is refused and counted.
// ⛔ QUARANTINE (Astra 024): if the block's ORIGINAL physical extent overlaps
// the host-cache history (pinHistory), it leaves `live` but is NEVER returned
// to the free list - no byte that ever backed a host-cached resource is handed
// out again, so no host cache can serve a previous owner's data (no executor
// invalidation needed). Every free and quarantine is logged uncapped.
bool free(uint32_t base);

// Record that host caches may now hold data keyed inside the PHYSICAL extent
// [key & 0x1FFFFFFF, +bytes). Publishers call it BEFORE they snapshot guest
// memory, under the allocator lock, so a free racing a capture either sees the
// pin (and quarantines) or happened first (and the capture reads the new
// owner's memory). This closes the capture -> free -> realloc -> publish ABA.
// No-op with SR_PHYS_FREE off.
void pinHistory(uint32_t key, uint64_t bytes);
bool historyOverlaps(uint32_t physLo, uint64_t bytes);   // test/inspection

uint32_t liveSize(uint32_t base);            // 0 if `base` starts no live block
bool insideLive(uint32_t va, uint32_t size); // inside ONE live block

struct Stats {
  uint64_t allocs, frees, failedAllocs, refusedFrees, badProtect;
  uint64_t liveBytes, peakBytes, liveBlocks;
  uint32_t lowWater;   // lowest address ever handed out (kHi if none)
  uint64_t quarantined, quarantinedBytes, historyPins, historyExtents;
  uint64_t freeBytes, largestFree;
  uint64_t carves, carvedBytes, pinNoProvenanceBytes;
  uint64_t hostReservedBytes;   // H-STACK: arena bytes owned by the host (main stack)
  uint64_t hostOverlapAllocs;   // H-STACK: allocations that touched them (must be 0)
};
Stats stats();

// Test seam: re-initialise the allocator over [lo, hi). Like the production
// arena, it excludes the host-owned main-thread stack where it intersects.
void resetForTest(uint32_t lo, uint32_t hi);
// Test seam for the overlap counter ONLY: the same arena WITHOUT the host carve,
// so a test can prove hostOverlapAllocs fires. Never used outside difftest.
void resetForTestNoHostCarve(uint32_t lo, uint32_t hi);

}  // namespace physalloc
}  // namespace kernel
}  // namespace sr
