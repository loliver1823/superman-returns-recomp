#pragma once
// iter-463: the LIFO physical allocator's reuse, floor and diagnostics
// (guestPhysAlloc / guestPhysLifoFree in kernel/guest_context.cpp). Declared
// here, not in guest_context.hpp, which every guest TU includes.
//
// THE DEFECT (archive/v1_gpuhang): the LIFO free reclaimed only the most recent
// grant; any other freed grant stayed reserved forever (state 1), so the cursor
// crept down by every non-top free. After ~30 min the Kryptonian arena's
// 0x2B30000 grant landed at 0x9E820000 - below the 0xA0000000 physical window -
// where its physical alias 0x1E820000.. double-booked the live D3D fence block,
// command buffer and ring at 0xBE82xxxx. gpuWindow() saw both windows owned,
// the IB fetch read the arena's file bytes as commands, the fence packets never
// ran and D3DDeviceSwap spun in GpuBlockOnFence_sub_820FD9A8. On the console
// (Xenia PhysicalHeap::Alloc / BaseHeap::Release) a freed physical page is free
// for the next allocation, and physical memory ends where 512 MB ends.
//
// SR_PHYS_LIFO_REUSE (default ON, =0 A/B): freed non-top grants become FREE
// EXTENTS - offset intervals [lo, hi) counted down from kPhysTop, coalesced -
// which the next grants reuse first-fit top-down before bumping; a top free
// cascades through the extents directly below it. Invariant (checked by
// physLifoCheckForTest): [origin, physNext) is the disjoint union of the live
// reservations and the free extents, no extent ends at physNext, and extents
// never touch each other (always coalesced).
// SR_PHYS_FLOOR (default ON, =0 A/B): no LIFO grant lies below 0xA0000000 (a
// reservation's alignment padding is clipped at the floor) - otherwise the
// request fails ([PHYS_CAP]) and MmAllocatePhysicalMemoryEx returns 0, as a
// console out of physical memory does.
#include <cstddef>
#include <cstdint>
#include <string>

namespace sr {
namespace kernel {

// iter-465 physmem: who asked for a LIFO grant (recorded with it; observe-only). The
// title's own requests (MmAllocatePhysicalMemoryEx) are kGuest; host-side callers name
// themselves with a PhysKindScope around their guestPhysAlloc.
enum class PhysKind : uint8_t {
  kGuest = 0,        // MmAllocatePhysicalMemoryEx (the title)
  kThreadStack = 1,  // ExCreateThread stack (SR_THREAD_MEM_XENIA=0 / fallback)
  kKThread = 2,      // KTHREAD arena overflow
  kKpcr = 3,         // KPCR arena overflow
  kGpuRoute = 4,     // the sub_82467F40 override's physical route
  kHost = 5,         // any other host caller (XMA contexts, ...)
};
const char* physKindName(PhysKind k);
// Sets the calling thread's kind for the grants made inside the scope.
class PhysKindScope {
 public:
  explicit PhysKindScope(PhysKind k);
  ~PhysKindScope();
  PhysKindScope(const PhysKindScope&) = delete;
  PhysKindScope& operator=(const PhysKindScope&) = delete;
 private:
  PhysKind prev_;
};

// iter-465 physmem (observe-only): at a [PHYS_CAP] refusal of `size` bytes, log
// [PHYS_PINS]: the live grants per kind, the largest free extents and the best window
// (the size-long VA range holding the fewest live bytes) with every live grant inside
// it - what keeps that range from being one free run. Logged at the first refusals
// only (and always under SR_PHYS_TRACE). Takes the allocator lock.
void physLifoLogPins(size_t size, uint32_t align, const char* why);
// SR_PHYS_TRACE=1 (default OFF, diagnostic): every LIFO grant and free logged
// ([PHYS_TRACE] grant/free va size align kind lr) - the allocation timeline.
bool physTraceEnabled();

// iter-465 physmem SR_PHYS_XENIA_PLACE (default ON since the iter-465 rolls; =0 A/B): Xenia's physical
// placement - MmAllocatePhysicalMemoryEx rounds size AND alignment to the page size its
// protect names (4 KB; 64 KB with X_MEM_LARGE_PAGES; 16 MB with X_MEM_16MB_PAGES;
// xboxkrnl_memory.cc xeMmAllocatePhysicalMemoryEx), and a bump grant takes the highest
// aligned position with the pages above it left free (BaseHeap::AllocRange top-down)
// rather than reserving size + align. Needs SR_PHYS_LIFO_REUSE.
bool physXeniaPlaceEnabled();
// iter-465 physmem SR_PHYS_BIG_GAP (default ON since the iter-465 rolls, =0 A/B; needs SR_PHYS_XENIA_PLACE):
// a cursor (bump) grant of >= 16 MB leaves 32 MB free above it - the hole Xenia's layout
// gets above the world pool from boot timing, made deterministic (guest_context.cpp).
bool physBigGapEnabled();
void setPhysBigGapForTest(int mode);    // -1 env, 0 off, 1 on
void setPhysXeniaPlaceForTest(int mode); // -1 env, 0 off, 1 on
bool physLifoReuseEnabled();            // SR_PHYS_LIFO_REUSE
bool physFloorEnabled();                // SR_PHYS_FLOOR
void setPhysLifoReuseForTest(int mode); // -1 env, 0 off, 1 on
void setPhysFloorForTest(int mode);     // -1 env, 0 off, 1 on

struct PhysLifoStats {
  size_t physNext = 0;
  size_t retainedBytes = 0;      // bytes in free extents (unsampled, exact)
  uint32_t retainedExtents = 0;
  uint64_t reuses = 0;           // grants carved from a free extent
  uint64_t cascades = 0;         // extents consumed by top-free cascades
  uint64_t floorRefusals = 0;    // [PHYS_CAP] floor refusals
  uint32_t holes = 0;            // alias holes registered
  uint64_t holeDrops = 0;        // alias holes DROPPED (table full)
  uint32_t lowestLiveVA = 0;     // the lowest live LIFO grant (0 = none)
};
PhysLifoStats physLifoStats();   // takes the allocator lock; scans the grant records
// Lock-free: bytes / extents currently free for reuse.
size_t physLifoRetainedBytes();
uint32_t physLifoRetainedExtents();
// GPU_HANG_WD (f) provider (kernel/gpu_hang_wd.hpp setGpuHangAllocProvider):
// try_lock only - false when the allocator lock is busy.
struct GpuHangAllocInfo;
bool physLifoHangAllocInfo(GpuHangAllocInfo* out);

// E3 (DIAGNOSTIC ONLY, never baked): SR_PHYS_LIFO_PREADVANCE=<hex offset>. The
// cursor is pre-consumed to that offset as ONE dummy reservation nobody frees,
// reproducing the 30-minute creep in seconds. WHEN: armed only once the D3D
// fence block exists (physLifoPreadvanceArm, from gpuCpSetRptrWriteback - so
// the ring, command buffer and fence block keep their places at the top), then
// applied at the first LIFO grant of exactly SR_PHYS_LIFO_PREADVANCE_AT bytes
// (hex; unset = the first grant after arming), just before that grant is
// placed. E3 = PREADVANCE=1DCAC480 PREADVANCE_AT=2B30000 with
// SR_PHYS_LIFO_REUSE=0 SR_PHYS_FLOOR=0: the arena's first grant lands at
// 0x9E820000 exactly as in v1_gpuhang.
void physLifoPreadvanceArm();
// Test seams: the env trigger with explicit values (disarmed; 0 target = off);
// and the dummy applied now (from the current cursor) - returns the dummy's VA
// (0 = nothing to do: the cursor is already at or past the offset).
void setPhysPreadvanceForTest(size_t target, size_t atSize);
uint32_t physLifoPreadvanceNowForTest(size_t offset);

// Test seams. The record of the grant at va (false = none): its reservation
// [prev, prev+reserve) in cursor offsets and its state (0 live, 1 released,
// 2 reclaimed).
bool physLifoGrantForTest(uint32_t va, size_t* prev, size_t* reserve, int* state);
// Verifies the invariant above from the allocator's own records (under its
// lock); on failure *why names the first violation.
bool physLifoCheckForTest(std::string* why);
// Drops every alias hole (the table is process-global).
void resetPhysHolesForTest();

}  // namespace kernel
}  // namespace sr
