#pragma once

// Native submission queue and its synchronization/diagnostic interface.
// The Oracle build additionally accepts packet-backed work. Function names and
// snapshot fields retain their existing ABI for callers and evidence readers.

#include <cstdint>

namespace sr::kernel {

bool gpuCpActive();
// CP_RB_WPTR store (GpuRingUpdateWritePointer_sub_820FD218): publish the ring
// geometry and the new write index (dwords). Never executes anything itself.
void gpuCpDoorbell(uint32_t ringBaseVA, uint32_t indexMask, uint32_t writeIndex);
// VdEnableRingBufferRPtrWriteBack: where the CP writes its read index back.
void gpuCpSetRptrWriteback(uint32_t phys, uint32_t blockSizeLog2);
// iter-467 native-a P2-A (kernel/gpu_ringless.cpp, SR_NATIVE_RINGLESS): a host item that occupies
// ring index space [index, index + n) instead of ring memory: kind 1 = INDIRECT_BUFFER {a = phys,
// b = dwords} (n = 3), kind 2 = SET_BIN_SELECT_LO {a = value} (n = 2). The CP loop takes it when its
// read index reaches `index`; a later gpuCpDoorbell publishes the index space it covers.
void gpuCpPushItem(uint32_t kind, uint32_t index, uint32_t a, uint32_t b);
// iter-467 native-a P2-B: kind 3 = a native SEGMENT {a = phys, b = dwords} whose parts the CP applies
// by `id` (kernel/native_flush.hpp cpSegment; the id, never the phys, names the parts). n = 3.
void gpuCpPushSegment(uint32_t index, uint32_t phys, uint32_t dwords, uint64_t id);
// Vblank: Xenia GraphicsSystem::MarkVblank increments the CP counter_.
void gpuCpVblank();
void gpuCpFinalLine();
// tools/graceful-quit (kernel/quit.hpp): the census with any why token (the terminal snapshot, after the CP parked),
// the wake for the drain request, and the ring items still queued (pushed for index space no doorbell published).
void gpuCpCensusLine(const char* why);
void gpuCpQuitWake();
uint32_t gpuCpRlPending();
// The ring cut: sets quit::g_ringCut under the CP's publish mutex - when it returns, no doorbell publishes again and
// the published write index is final.
void gpuCpCutRing();

// iter-463 (kernel/gpu_hang_wd.cpp): the CP state from atomics only - never
// takes the CP mutex, so the hang watchdog can read it while the CP is wedged.
// render/boot-attach: the CP is blocked on the host, not on the GPU - waiting for the live backend to attach
// (gpuCpWaitForBackend) or for the boot shader cache (WP1 / WP2, docs/NATIVE_ROADMAP.md "Boot attach"). The hang
// watchdog does not read the CP's own stall (H3) as a GPU hang, fires every other trigger annotated with the wait,
// and escalates a long host wait on its own schedule as a [HOST_WAIT_STALL] (kernel/gpu_hang_wd.cpp);
// the [DISPLAY] / [SHADER_CACHE] wait lines name it (a Warn every 10 s while it lasts).
constexpr uint32_t kCpPhaseHostWait = 5;
// What a phase-5 CP waits for (GpuCpSnapshot::hostWait).
// kCpHostWaitTranslation (render/shader-coverage): an IM_LOAD waiting for its runtime shader translation (the miss
// policy, gfx/shader_runtime_xlat.cpp).
enum : uint32_t {
  kCpHostWaitNone = 0, kCpHostWaitBackend = 1, kCpHostWaitInstalled = 2, kCpHostWaitTranslator = 3,
  kCpHostWaitTranslation = 4
};
const char* gpuCpHostWaitName(uint32_t what);
struct GpuCpSnapshot {
  bool started = false, published = false;
  uint32_t phase = 0;            // 0 idle, 1 ring packet, 2 IB, 3 swap, 4 ISR, 5 host wait
  uint32_t hostWait = 0;         // kCpHostWait*: what a phase-5 CP waits for
  uint64_t phaseSinceNs = 0;     // steady_clock ns
  uint32_t ringBase = 0, rptr = 0, wptr = 0;
  uint64_t doorbells = 0, packets = 0, swaps = 0, isrCalls = 0, isrFaults = 0;
};
GpuCpSnapshot gpuCpSnapshot();

// render/boot-attach (SR_PM4_WAIT_BACKEND, docs/NATIVE_ROADMAP.md "Boot attach"): the CP's wait for the live
// backend before its first command. The CP never runs the GPU command stream without the backend (the black
// world, ENGINE_MAP 6g.8d), so the wait has two ends only:
//   - attached: logs "[DISPLAY] CP waited N ms for the live backend (attached), W still-waiting warnings" and
//     returns the ns waited;
//   - failed() (main() reported a failed D3D12 init): logs "... (INIT FAILED)", then [FATAL], a message box
//     (not under SR_INSTANCE_ID) and exit 3 - it does not return.
// No time limit: with the init not failed it keeps waiting, a Warn every warnEveryMs naming the time so far, in
// CP phase kCpPhaseHostWait (the watchdog sets aside the CP's own stall and escalates a long wait as a
// [HOST_WAIT_STALL]; the phase before it is restored on attach). A null hook
// is the real one (the backend state, steady_clock, a sleep); the tests pass a fake clock.
struct BackendWaitProbe {
  bool (*attached)() = nullptr;
  bool (*failed)() = nullptr;
  const char* (*reason)() = nullptr;
  uint64_t (*nowNs)() = nullptr;
  void (*sleepMs)(uint32_t) = nullptr;
  uint32_t warnEveryMs = 10000;
};
uint64_t gpuCpWaitForBackend(const BackendWaitProbe& p);

// render/boot-attach (Astra round 3): the CP's first CONSUMED command. Both command paths - a host item and a ring
// packet - call gpuCpTakeCommand once they know how many dwords the command needs (`need`) and how many are
// published (`avail`): the command is taken only when all of them are (else nothing is consumed and the CP waits
// for the rest), and the first command taken is recorded then (nowMs() is read only for that one) - never when the
// ring merely became nonempty.
struct GpuCpFirstCommand {
  bool taken = false;
  double atMs = -1.0;  // process-relative (msSinceProcessStart)
  uint32_t kind = 0;   // 1 a host item, 2 a ring packet
};
bool gpuCpTakeCommand(uint32_t need, uint32_t avail, uint32_t kind, double (*nowMs)(), GpuCpFirstCommand& first);
uint64_t gpuCpBackendWaitWarnsForTest();  // still-waiting Warns logged by the last gpuCpWaitForBackend

// step 6 G6a DIAG (dev builds only; no-ops in release):
// - diagSwapNoteGuest: the guest VdSwap call (kernel/stubs.cpp stub_VdSwap) for [DIAG_SWAP], the latency of a swap
//   from the guest's call to the CP's XE_SWAP arrival and on to the next guest ISR call, per 120 swaps.
// - diagCpCallback: the CP callback the calling thread is inside (3 = the swap's present, 4 = a guest ISR, 0 = none),
//   [DIAG_WAIT]'s signaller class.
// - diagSetSwapCensusHook: a census the CP runs every 120 swaps, on its thread (guest/ppc_import_glue.cpp's
//   [APC_STUCK]; a hook, so this unit needs no link to the guest glue).
#if SR_ORACLE
void diagSwapNoteGuest() noexcept;
uint32_t diagCpCallback() noexcept;
void diagSetSwapCensusHook(void (*hook)(uint64_t swaps)) noexcept;
#else
inline void diagSwapNoteGuest() noexcept {}
inline uint32_t diagCpCallback() noexcept { return 0; }
inline void diagSetSwapCensusHook(void (*)(uint64_t)) noexcept {}
#endif

}  // namespace sr::kernel
