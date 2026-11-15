// guest_call.hpp — one-stop ergonomics for calling guest functions from host
// driver threads (iter-295g). Collapses the ~60-line ceremony every drive used
// to hand-roll, and bakes in the hard rules that repeatedly bit us:
//   - setGuestMemory TLS on the calling thread (silent no-ops otherwise)
//   - own guest stack (never share another drive's stack window)
//   - r14 = 0x829C0000; r13 = this thread's KPCR (SR_KPCR, kernel/kpcr.hpp)
//   - ExportRegistry::setGuestCallContext around the call
//   - SEH __try (the harness VEH handles guest faults; host-side faults in
//     YOUR code are still fatal — never probe dead memory, see ENGINE_MAP §7)
//   - thread registry + name => [THREAD_SAMPLER] / frida_threads visibility
//
// Usage (from any host thread):
//   sr::kernel::GuestCaller gc("MY_DRIVER");            // once per thread
//   sr::kernel::GuestArgs a{}; a.r3 = ctxVA; a.r4 = slotVA;
//   uint32_t exc = 0;
//   if (gc.call(__imp__sub_826AECC8, a, &exc)) { /* returned */ }
//   uint32_t ret = gc.ctx().r3.u32;                     // guest return value
//
// The PPCContext is REUSED across calls on this thread (that's what real guest
// threads do) and registered with the sampler, so a hung call shows its live
// lr/r1 in [THREAD_SAMPLER] and frida_threads.
#pragma once

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "export_registry.hpp"
#include "kpcr.hpp"
#include "quit.hpp"  // tools/graceful-quit: a host loop's guest calls are a park point
#include <cstdint>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace sr::kernel {

struct GuestArgs {
  uint32_t r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0;
  double   f1 = 0.0;          // first float arg (set has_f1)
  bool     has_f1 = false;
  uint32_t lr = 0;            // synthetic caller LR (0 = leave default)
};

using GuestFn = void(PPCContext& __restrict, uint8_t*);

namespace detail {
// Plain function so the SEH __try never mixes with C++ unwinding.
inline bool sehCall(GuestFn* fn, PPCContext& ctx, uint8_t* base, uint32_t* excOut) noexcept {
#if defined(_WIN32)
  __try {
    fn(ctx, base);
    return true;
  } __except (*excOut = (uint32_t)GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#else
  (void)excOut;
  fn(ctx, base);
  return true;
#endif
}
}  // namespace detail

class GuestCaller {
public:
  // stackBytes: per-thread guest stack (drives that recurse deep may want more).
  explicit GuestCaller(const char* threadName, uint32_t stackBytes = 0x20000) {
    uint8_t* sb = nullptr; size_t ss = 0;
    getGlobalGuestMemory(&sb, &ss);
    if (sb) setGuestMemory(sb, ss);
    base_ = sb;
    stackVA_ = guestHeapAlloc(stackBytes);
    stackTop_ = stackVA_ ? stackVA_ + stackBytes - 0x100u : 0;
    // iter-463 SR_KPCR: this host thread's own KPCR in r13 (0 when off).
    ctx_.r13.u32 = kpcrBindCurrent(stackVA_ ? stackVA_ + stackBytes : 0u, stackVA_, -1);
    setCurrentGuestContext(&ctx_, offsetof(PPCContext, lr), offsetof(PPCContext, ctr),
                           offsetof(PPCContext, r3), offsetof(PPCContext, r1),
                           offsetof(PPCContext, r31));
    if (threadName) setGuestThreadName(threadName);
    quit::markHostLoop();  // tools/graceful-quit: between calls this thread is host-idle (acknowledged)
  }
  ~GuestCaller() { setCurrentGuestContext(nullptr, 0); }
  GuestCaller(const GuestCaller&) = delete;
  GuestCaller& operator=(const GuestCaller&) = delete;

  bool ok() const { return base_ != nullptr && stackVA_ != 0; }
  PPCContext& ctx() { return ctx_; }
  uint32_t stackVA() const { return stackVA_; }

  // Returns true if the call returned; false on a caught exception (*excOut set).
  bool call(GuestFn* fn, const GuestArgs& a, uint32_t* excOut = nullptr) {
    if (!ok()) { if (excOut) *excOut = 0xDEAD0000u; return false; }
    ctx_.r1.u32 = stackTop_;
    ctx_.r3.u32 = a.r3;  ctx_.r4.u32 = a.r4;  ctx_.r5.u32 = a.r5;  ctx_.r6.u32 = a.r6;
    ctx_.r7.u32 = a.r7;  ctx_.r8.u32 = a.r8;  ctx_.r9.u32 = a.r9;  ctx_.r10.u32 = a.r10;
    ctx_.r14.u32 = 0x829C0000u;
    if (a.has_f1) ctx_.f1.f64 = a.f1;
    if (a.lr) ctx_.lr = a.lr;
    uint32_t exc = 0;
    // tools/graceful-quit (kernel/quit.hpp): after a quit's thread cut a host loop parks here, before any guest code.
    quit::guestCallEnter();
    ExportRegistry::setGuestCallContext(true);
    const bool ret = detail::sehCall(fn, ctx_, base_, &exc);
    ExportRegistry::setGuestCallContext(false);
    quit::guestCallLeave();
    if (excOut) *excOut = exc;
    return ret;
  }

private:
  PPCContext ctx_{};
  uint8_t*   base_ = nullptr;
  uint32_t   stackVA_ = 0;
  uint32_t   stackTop_ = 0;
};

}  // namespace sr::kernel
