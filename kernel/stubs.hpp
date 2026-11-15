#pragma once

#include "export_registry.hpp"

namespace sr {
namespace kernel {

// Register all stub exports with the registry.
void registerStubs();

// step4/audit (SR_XMP_APP_XENIA's overlapped completion, kernel/import_fixes.cpp): set the Nt-level guest event
// `guestHandle` under the event table's lock, as Xenia's CompleteOverlappedEx does through LookupObject<XEvent>.
// False when the handle is not an event in the table (with SR_NT_EVENT_TYPE on: not one NtCreateEvent made).
bool ntEventSetByHandle(uint32_t guestHandle);

// step4/audit SR_XNOTIFY_XENIA (Astra batch-2 P2): broadcast one notification to every listener through the
// filtered queue (Xenia KernelState::BroadcastNotification). A no-op while the gate is off.
void xnotifyBroadcastXenia(uint32_t id, uint32_t param);
// Test seam (difftest --import-fixes-test): drop every listener, the first-listener flags and the UI state.
void xnotifyResetForTest();
// step4/audit SR_XNOTIFY_XENIA (Astra merge review P2): run `mutate(ctx)` - a state change that returns the
// notification's param - under the queue lock and broadcast `id` with its result in the same step (lock order
// queue -> the caller's state lock). Gate off: mutate(ctx) alone.
void xnotifyPublishXenia(uint32_t id, uint32_t (*mutate)(void*), void* ctx);
// Test seams (Astra merge review): the next Xenia-path listener gets no wait event (its creation must fail); the
// host event behind a listener handle while it is in the table (nullptr otherwise).
void xnotifyFailNextEventForTest();
void* xnotifyListenerEventForTest(uint32_t handle);
// Test seam (difftest N11): `hook(r3)` runs inside NtWaitForSingleObjectEx once it holds r3's host HANDLE, before
// the wait. nullptr clears it.
void ntWaitAcquiredHookForTest(void (*hook)(uint32_t handle));

// step4/audit SR_XAPI_THREAD_STARTUP test seam: ExCreateThread's entry plan under the gate's current answer.
uint32_t exThreadEntryPlanForTest(uint32_t r6, uint32_t r7, uint32_t r8, uint32_t* r3, uint32_t* r4);

// step4/audit SR_XAPI_THREAD_STARTUP: the r4 of a guest thread's first call (thread-local, kernel/guest_context.cpp).
// ExCreateThread's guestThreadFunc sets it before it runs the entry; the runner (guest/guest_harness.cpp) takes it
// (0 when nothing set it: the old entry(r3 = context) call). Declared here, not in guest_context.hpp, so the
// generated guest TUs do not rebuild.
void setThreadStartR4(uint32_t r4);
uint32_t takeThreadStartR4();

// Individual stubs (PowerPC args r3-r8). Each logs and returns a safe default.
uint32_t stub_NtAllocateVirtualMemory(uint32_t r3, uint32_t r4, uint32_t r5,
                                      uint32_t r6, uint32_t r7, uint32_t r8);
uint32_t stub_KeQueryPerformanceCounter(uint32_t r3, uint32_t r4, uint32_t r5,
                                        uint32_t r6, uint32_t r7, uint32_t r8);
uint32_t stub_KeInitializeEvent(uint32_t r3, uint32_t r4, uint32_t r5,
                                uint32_t r6, uint32_t r7, uint32_t r8);
uint32_t stub_RtlAllocateHeap(uint32_t r3, uint32_t r4, uint32_t r5,
                              uint32_t r6, uint32_t r7, uint32_t r8);

// iter-464 SR_NTQDIR_XENIA test seam (difftest --mod-overlay-test): -1 = the env gate,
// 0 = the old packing listing, 1 = Xenia's one-record-per-call listing.
void ntQueryDirXeniaSetForTest(int mode);
uint32_t ntQueryDirStateCountForTest();

// One VD_ISR vblank tick's delivery for a leased registration (kernel/stubs.cpp, the vblank thread's body after the
// lease, the callback lookup and the CPU-2 / KPCR setup; display.registers, Astra round 1, P1): the scan-out answer,
// the SR_NATIVE_VBLANK tick scope and either the registered callback or - for the retail D3D ISR only - the host
// source 0 (sr::native::vblank::planTick). refused = the host source 0 was due without an owned KPCR (the caller
// stops). Driven directly by difftest --native-vblank-test on a GuestCaller thread.
class GuestCaller;
namespace graphics_interrupt { struct Registration; }
struct VdTickDelivery {
  bool refused = false;
  bool native = false;            // the host source 0 ran instead of the registered callback
  bool okc = false;               // the guest call returned
  uint32_t exc = 0;
  bool source0Completed = false;  // native: trySource0 completed
  unsigned source0Result = 0;
  uint32_t source0Exception = 0;
};
VdTickDelivery vdVblankDeliver(GuestCaller* gc, const graphics_interrupt::Registration& registration, bool ownPcr,
                               bool highFps);

}  // namespace kernel
}  // namespace sr
