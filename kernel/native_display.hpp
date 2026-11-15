#pragma once

#include <cstdint>

struct PPCContext;

namespace sr::native::display {

constexpr uint32_t kD3DIsr = 0x820FDBA0u;

enum class Source0Result { Declined, Completed, CallbackFault };

// Bounded replacement of the registered retail D3D ISR's source-0 operation.
// The caller establishes CPU 2, a DPC and the guest-call/TLS scope. It owns the
// device, stack, dispatch mapping and their protection lifetimes for this call;
// the scratch pointer and callback publication must not change concurrently.
// Only Declined guarantees no changes to ctx, guest bytes or exceptionOut.
// CallbackFault follows committed effects and must never trigger oracle replay.
// The native registration supplies arming; this function reads no MMIO flag.
//
// Admission is arithmetic (native/dreg-hang, 2026-10-15; the transport pair's ADMISSION A, kernel/guest_protect.hpp):
// every guest range it touches (the device words, the ISR frame, the KPCR, the release scratch) must map contiguously
// into the registered guest arena and touch no range a foreign protector declared; it declines while a page-protecting
// diagnostic is requested or an all-excluding protection was declared, and when the arena is retired (an ArenaLease
// is held across every guest access, the callback included). No OS permission query runs on a tick with a null swap
// callback: dreg2 (SR_NATIVE_VBLANK=1, HIGHFPS 1000 Hz) measured ~46 ticks/s, each tick's five VirtualQuery walks of
// the resident guest arena (~4 ms apiece) holding the CPU-2 lock the CP's pacing call and census need - the 3-33 s
// CP stalls (GPU hang trigger H1). exceptionOut is the caller's host word: overlap-checked, never queried.
Source0Result trySource0(PPCContext& ctx, uint8_t* base,
                         uint32_t registeredCallbackVA, uint32_t registeredContextVA,
                         uint32_t* exceptionOut = nullptr);

namespace test {
// OS permission queries (VirtualQuery) trySource0 has made in this process: only a non-null swap callback's
// dispatch-slot / host-function check makes any (null in this title). --native-vblank-test T reads it.
uint64_t osQueries();
}  // namespace test

}  // namespace sr::native::display
