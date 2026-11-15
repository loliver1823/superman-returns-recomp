#pragma once

// iter-467 native-a P2 trace (docs/NATIVE_ROADMAP.md "P2.7 (d)" item 1): the observe-only fence /
// ring / answer trace of the XDK ring path, gate SR_P2_TRACE=<file> (or =1 for
// archive/p2_trace_<pid>.bin). It is the BASELINE the ringless path (P2-A, P2-B) is proven against:
// per event it records the kind, the open submission id S (dev+0x2A1C), the completed fence C
// (fb+0), the queried F, the answer / reason, the host thread and the present number.
//
// Hooks (all observe: guest state is read, never written; every override delegates once):
//   close      GpuBuildFenceWritePacket_820FD580 (override here)      kClose {f = S written, a = cursor}
//   ring       GpuRingUpdateWritePointer_820FD218 (table hook)        kIb per descriptor {a = phys, b = dwords}
//              GpuRingEmitBinSelectLo_82110140 (override here)        kBs {a = value}
//   answers    D3DDeviceIsFencePending_820FDD90 (table hook)          kPending {f, aux = result}
//              GpuBlockOnFence_820FD9A8 (table hook)                  kWaitBegin / kWaitEnd {f, aux = reason}
//              D3DQueryGetData_82102DC8 (table hook)                  kQuery {f = q+0x14, aux = result}
//              D3DVsPatchFetchesInPlaceIfIdle_82108520 (native_flush) kPatch {f = cache fence, aux = result}
//              GpuRingWaitForSpace_820FCB30 / WaitReadPtrWrap_820FD150 (table hooks)
//              D3DDeviceInsertFence_820FDD30 (table hook)             kInsertFence {a = S returned}
//   consumer   the CP's EVENT_WRITE_SHD to fb+0 / fb+4 (CpHooks::trace)  kComplete / kProgress {a = value}
//              the CP's scratch write-back to pc+4n                   kScratch {aux = n, a = value}
//   ISR        kernel/gpu_command_processor.cpp onInterrupt           kIsr {aux = cpuMask, a = pc+0, b = pc+0x10}
//   present    stub_VdSwap                                            kPresent {a = dev+0x34D0, b = fb+0x3C}
// Summariser: tools/perf/p2_trace.py.

#include <cstdint>

#include "sr_oracle.hpp"

namespace sr::native::p2trace {

#if SR_ORACLE
extern bool g_on;  // SR_P2_TRACE set (latched at load)
#else
inline constexpr bool g_on = false;  // SR_ORACLE=0 (kernel/sr_oracle.hpp): the trace is compiled out
#endif

enum Kind : uint16_t {
  kClose = 1, kIb = 2, kBs = 3, kWaitBegin = 4, kWaitEnd = 5, kPending = 6, kQuery = 7, kPatch = 8,
  kComplete = 9, kProgress = 10, kScratch = 11, kIsr = 12, kPresent = 13, kRingSpaceBegin = 14,
  kRingSpaceEnd = 15, kRptrWrapBegin = 16, kRptrWrapEnd = 17, kInsertFence = 18, kFlushBegin = 19,
  kFlushEnd = 20,
  // 2026-10-10 native-a (the segmentless wedge's CP-side evidence): the CP's writes of SCRATCH_UMSK / SCRATCH_ADDR
  // {aux = reg, a = value}; a scratch write-back the CP could NOT perform (physToHost null) {aux = n, a = addr, b =
  // value}; one it deferred to the executor {aux = n, a = addr, b = value}; a ringless item the CP takes {aux = kind
  // 1 IB / 3 SEGMENT, f = id low, a = phys, b = dwords}; segmentPublishId's answer {aux = 0 none / 1 hand-off / 2
  // replay hit / 3 hand-off mismatch, f = id low, a = phys, b = dwords}.
  // kCpItem aux 4 (p2.transport BRACKET): a bound segment applied inside its BRACKET_END {f = id low, a = phys,
  // b = dwords}, once per pass; aux 5 (2026-10-14, the tcmp1 diagnosis; the producer thread, just before the
  // BRACKET_END is published): the closes that BRACKET_END binds {f = first id low, a = count, b = generation low}, one
  // record per contiguous id run. aux 6 / 7 (the CP thread, Astra round 1): a BRACKET_END segment's replay begins /
  // completed every step {f = its segment id low, a = steps planned / applied, b = generation low}.
  kRegw = 21, kScratchSkip = 22, kScratchDefer = 23, kCpItem = 24, kSegLookup = 25,
  // 2026-10-10 native-a: the WHOLE D3DDeviceBlockUntilIdle_820FDCB8 (its fence wait AND the dev+0x2A74 drain spin at
  // 0x820FDD10-18, which the release/forcing lane un-hooks: the console's ISR / pump must drain it) {f = dev+0x2A74 at
  // entry, a = lr} / {f = dev+0x2A74 at exit, a, b = the QPC duration}. The proof check: every entry completes, none
  // longer than a frame.
  kIdleBegin = 26, kIdleEnd = 27,
  // 2026-10-13: SR_NATIVE_SEG_TILE's forced drained point (native_flush.cpp segTileForcedDrain) {aux = 1 registered ==
  // first-applied / 0 not, f = registered segments, a = first-applied segments, b = registered - first-applied records}.
  kDrained = 28,
  // 2026-10-14 (transport-composed lane, p2.transport's `pacing` unknown): the pacing words at every present, written
  // right after kPresent {f = dev+0x3B14 (the vblank counter the source-0 ISR increments), a = dev+0x3B20 (the gate
  // count 82111E38 increments), b = dev+0x4E4C (the throttle wait SwapEpilogStats snapshots)}. Observe-only reads.
  kPacing = 29,
};

// One record (40 bytes, host little-endian). S / C / present / thread / time are filled by event().
struct Rec {
  uint64_t qpc;
  uint32_t present;
  uint32_t tid;
  uint16_t kind;
  uint16_t aux;
  uint32_t s;   // dev+0x2A1C (the open submission id)
  uint32_t c;   // fb+0 (the completed fence)
  uint32_t f;   // the queried / written fence
  uint32_t a, b;
};
static_assert(sizeof(Rec) == 40, "p2 trace record");

#if SR_ORACLE
void event(uint16_t kind, uint16_t aux, uint32_t f, uint32_t a, uint32_t b);

// Pm4Engine::CpHooks::trace: what 0 = EVENT_WRITE_SHD store {addr, value}, 1 = scratch write-back.
void cpEvent(void* user, uint32_t what, uint32_t addr, uint32_t value);
void onIsr(uint32_t cpuMask, uint32_t ctxVA);
void onPresent();
void onPatch(uint32_t fence, uint32_t result);
// P2-A (gpu_ringless.cpp): the 820FD218 override took the queue path and recorded the IB itself; the
// table hook must not scan the (stale) ring for this call.
void markRingless();
void finalLine(const char* why);
#else
// SR_ORACLE=0: every hook is a no-op (an unset SR_P2_TRACE).
inline void event(uint16_t, uint16_t, uint32_t, uint32_t, uint32_t) {}
inline void cpEvent(void*, uint32_t, uint32_t, uint32_t) {}
inline void onIsr(uint32_t, uint32_t) {}
inline void onPresent() {}
inline void onPatch(uint32_t, uint32_t) {}
inline void markRingless() {}
inline void finalLine(const char*) {}
#endif

}  // namespace sr::native::p2trace
