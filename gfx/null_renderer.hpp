#pragma once

#include <cstdint>

#include "kernel/sr_oracle.hpp"

namespace sr {
namespace gfx {

// Called from the VdSwap stub each time the guest presents a frame.
// ringBufWritePtr: VdSwap r3 = current ring buffer write pointer (guest VA in physical region).
// commandBufferVA: guest VA of the GPU command buffer (from VdGetSystemCommandBuffer).
// frontBufferVA: guest VA of the front buffer we're displaying (or test-pattern VA).
// frameNumber: current VdSwap count.
void onVdSwapFrame(uint32_t ringBufWritePtr, uint32_t commandBufferVA,
                   uint32_t frontBufferVA, uint64_t frameNumber);

// Publish the D3D12 backend to the serialized native submission/render service.
class D3D12Backend;
void setRenderBackend(D3D12Backend* be);
void failRenderBackend(const char* reason);
#if SR_ORACLE
inline void setPm4LiveBackend(D3D12Backend* be) { setRenderBackend(be); }
inline void failPm4LiveBackend(const char* reason) { failRenderBackend(reason); }
#endif
// Unpublish only this backend and wait for its serialized callers to finish.
void retirePm4LiveBackend(D3D12Backend* expected);
// iter-467 native-b (the black world): true once main() attached the live backend. Until then every CP draw and resolve
// is parsed but not performed, and the queued draws are drained LATE, against memory the guest has since reused.
bool pm4LiveBackendAttached();
// render/boot-attach: main() says the live backend will never attach (its init failed); the CP then fails
// loudly at once instead of running the command stream without a backend (the black-world path).
bool pm4LiveBackendFailed();
const char* pm4LiveBackendFailReason();  // "" until failPm4LiveBackend
// render/boot-attach (Astra round 2): D3D12Backend::init's progress, for the host-wait stall report
// (kernel/gpu_hang_wd.cpp). The step it is in (a static string; the first call records its thread and start, in
// ms since process start), nullptr = init returned ready. Lock-free.
void noteBackendInitStep(const char* step);
struct BackendInitState {
  const char* step = nullptr;  // nullptr = never started
  uint32_t tid = 0;            // the init thread (host)
  double startMs = -1.0;       // process-relative
  bool returned = false;       // init returned ready
};
BackendInitState backendInitState();
bool pm4LiveEnabled();  // gfx/d3d12_pm4_edram.cpp (also declared in d3d12_backend.hpp)

// P2 async command processor (kernel/gpu_command_processor.cpp). SR_PM4_CP:
// default = SR_PM4_LIVE; =1 forces it on (HLE rendering + real CP), =0 off.
// When on, the CP thread owns the live engine: the doorbell only publishes the
// write pointer, and the VdSwap window scan / chain drain / VD_ISR source-1
// delivery / swap-time fence fakes all stand down.
bool pm4CpAsyncEnabled();
// The CP thread's live engine. C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): the release configuration (SR_ORACLE=0)
// links no PM4 walker - the live engine is the record engine (gfx/record_engine.hpp) and getLivePm4() does not exist
// there; the development configuration's live engine is the walker derived from it (gfx/pm4_engine.hpp), the same
// instance under both names.
struct RecordEngine;
RecordEngine& getLiveRecordEngine();
#if SR_ORACLE
struct Pm4Engine;
Pm4Engine& getLivePm4();
#endif
// CP-thread services (no-ops unless SR_PM4_LIVE has a backend): translate the
// queued draws now; present the swap the CP just reached. fbPhys/w/h come from
// the PM4_XE_SWAP packet, pitch/tiling from fetch constant 0 (VdSwap wrote it
// immediately before the swap packet — Xenia IssueSwap reads the same slot).
void cpFlushDraws();
// Step 4 (Astra 10:5x #1): the window-close exit, after the CP parked drained (kernel/quit.cpp parkPresenterForExit):
// waits at most `ms` for the PM4 render thread to finish the frames it was handed. true = idle (or no live backend).
bool pm4RenderThreadIdleForExit(uint32_t ms);
void cpIdleFlush();  // the CP reached wptr: drain now, or kick the native executor
// p2.zpd (SR_NATIVE_ZPD): the covering-fence check before an EVENT_WRITE_SHD store, on the drain owner (the CP, or
// the executor performing its store item): strict mode renders the set so far and writes the drained queries' slot
// words first, and whatever that could not write gets the strict fallback before the store returns; the latency
// model marks them covered (written later = late).
void cpZpdFence();
void cpPresentSwap(uint32_t fbPhys, uint32_t width, uint32_t height);
// Step 4 item 7a: SR_PM4_EARLY_INIT (default ON since the 7a A/B; =0 = the lazy path) - the CP (kernel/gpu_submit.cpp cpMain, after WP1, before its first
// command) builds the frame renderer's one-time GPU setup now instead of inside the guest's first swap
// (D3D12Backend::pm4EarlyInit). No-op with the gate off or no live backend.
bool pm4EarlyInitOn();
void cpEarlyInit();

// NATIVE strand A executor (kernel/native_exec.hpp, SR_NATIVE_EXEC): the drain on its own thread, the
// CP's guest-visible effects as stream items it performs in order.
// C1 (docs/RELEASE_COMPOSITION.md 2.2): a default-OFF experiment on the emulated CP's drain. The release
// configuration (SR_ORACLE=0) compiles the executor out of null_renderer.cpp (with kernel/native_exec.cpp,
// CMakeLists.txt EMULATION_SOURCES); these entries then answer what the shipping code sees with the gate unset.
struct GuestDrawCommand;
#if SR_ORACLE
void nativeExecKick();                                        // CP: items were committed
void nativeExecPushStores(const uint32_t* quads, uint32_t n);  // CP: one store item (n <= kStoresPerItem)
GuestDrawCommand* nativeExecReserve();                        // CP: a slot, waiting for the executor
uint64_t nativeExecPendingEffects();                          // effect items not yet performed
bool nativeExecPendingStoreAt(const uint8_t* word);           // CP: a queued store targets this host word
#else
inline void nativeExecKick() {}
inline void nativeExecPushStores(const uint32_t*, uint32_t) {}
inline GuestDrawCommand* nativeExecReserve() { return nullptr; }
inline uint64_t nativeExecPendingEffects() { return 0u; }
inline bool nativeExecPendingStoreAt(const uint8_t*) { return false; }
#endif

// iter-397q: PM4_INTERRUPT (0x54) packets consumed by the live engine. The
// kernel VD_ISR thread polls this and delivers source-1 graphics interrupts
// (SR_CP_INT) — on hardware every 0x54 the CP consumes raises one.
uint64_t getLiveCpInterruptCount();

// iter-458 (Astra 029 A): which window executeGuestIndirectBuffer read each IB
// through, decided by physwin::ownerWindow. Counted under SR_PHYS_FREE only (all
// zero with the gate off). `both` and `unknown` kept the legacy 0x8 read.
// C1 stage 1: executeGuestIndirectBuffer is null_renderer.cpp's oracle half (the development PM4 walker's IB entry),
// compiled out under SR_ORACLE=0 with these counts.
#if SR_ORACLE
struct IbWindowCounts { uint64_t a, w8, both, unknown; };
IbWindowCounts getLiveIbWindowCounts();
#endif

// D3D cmd[46] (present/resolve) — kernel sets when guest calls vtable[0] with r3=46.
// Backend checks and clears in present path to trigger copy of render target to guest front buffer.
void setResolveRequested();
bool getResolveRequested();  // Peek without clearing (for diagnostics).
bool getAndClearResolveRequested();

// Called by D3D12Backend::moveToNextFrame() after each successful Present.
// Increments a counter that the GPU IRQ thread checks to know when to signal
// guest GPU sync events (simulating GPU-done DPC after frame completion).
void notifyGpuFramePresented();

// Returns true (and resets the flag) if a new D3D12 Present has occurred since
// the last call.  Single-consumer: only call from the GPU IRQ thread.
bool consumeGpuFramePresented();

}  // namespace gfx
}  // namespace sr
