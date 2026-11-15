// iter-458 HR1 (Astra 071/072/073): SR_GPU_PHYS_ROUTE, default OFF - the
// sub_82467F40 override (kernel/gpu_ring_alloc_fix.cpp) stops intercepting the
// guest's own physical allocation path after the ring claim. Contract and
// evidence: see the block above gpuPhysRouteOn() in gpu_ring_alloc_fix.cpp.
#pragma once

#include <cstdint>
#include <mutex>

namespace sr {
namespace kernel {

// The capture-bound witness. Every counter changes only under the accounting
// mutex; the snapshot is taken once, at the recorder's first close, INSIDE the
// same critical section as the H-UNIQ seal (registry lock, then this mutex).
struct GpuPhysRouteSnapshot {
  bool taken = false;
  int gate = 0;
  int atSeal = 0;          // 1 = taken in the critical section that sealed H-UNIQ
  uint32_t nonce = 0;      // that seal's acquisition nonce (0 = no seal)
  uint64_t sealSeq = 0;    // that seal's record seq
  uint64_t admitted = 0, settled = 0;                  // every bit-31 request
  uint64_t started = 0, completed = 0, null = 0, belowA = 0;   // the delegated subset
  uint64_t heapAfterClaim = 0, heapSpanning = 0;       // intercepted heap routes
  uint64_t mmTruncated = 0, mmWrapped = 0;             // the import's size rules
};

// The accounting mutex. Lock order: physwin's registry lock, THEN this one.
// Nothing holding it calls into the registry.
std::mutex& gpuPhysRouteAccountingMutex();
// The caller holds gpuPhysRouteAccountingMutex(). First call wins.
void gpuPhysRouteCaptureSnapshotLocked(const char* why, bool atSeal, uint32_t nonce,
                                       uint64_t sealSeq);

// stubs.cpp (MmAllocatePhysicalMemoryEx, every caller reaching the legacy size
// rules - SR_PHYS_FREE's service returns earlier): [0] requests truncated to
// the per-call cap, [1] requests whose 32-bit page rounding wrapped.
void mmAllocSizeAnomalies(uint64_t out[2]);

// Test seams.
GpuPhysRouteSnapshot gpuPhysRouteSnapshotForTest();   // the capture snapshot
GpuPhysRouteSnapshot gpuPhysRouteNowForTest();        // the live counters
void gpuPhysRouteResetForTest();
void setGpuPhysRouteForTest(int mode);                // -1 env, 0 off, 1 on
// phase 1: admitted (counted, lock released); 2: delegated, before the guest
// call; 3: the guest call returned, before settling.
void setGpuPhysRoutePhaseHookForTest(void (*fn)(int phase));

}  // namespace kernel
}  // namespace sr
