// iter-463 charter (burndown item 10): the ONE place an unresolvable guest
// indirect call (bctrl / bcctrl) ends up.
//
// PPC_CALL_INDIRECT_FUNC (tools/regen/hand/ppc_context.h) dispatches every
// target that resolves to a recompiled function. Before this change, anything
// else - a NULL pointer, a target outside the image's code range, an in-range
// VA with no dispatch entry, and six hard-coded VAs - got a FABRICATED
// `r3 = 0` and execution continued. On the console such a call executes
// whatever is at the target (NULL / a data page / garbage: an exception), and
// Xenia either JIT-compiles from the address or fails to resolve it
// (x64_emitter.cc ResolveFunction: assert_not_null(fn)); neither returns 0.
// The fabricated return hid real bugs: the out-of-range calls at lr
// 0x82722E34 (InputContext_UpdateCommands, cmd->vt[+0x1C]) in 21/81 run logs
// before the heap-corruption fix 60a90bf were garbage vtables of freed
// command objects, not a missing function.
//
// Default exe: a NAMED LOG-AND-FAIL. The first calls are logged with the call
// site, the target, and the object the call was made on (r3, its vtable word
// and the slots a virtual call loads), then the process stops with a
// [ICALL_FAIL] marker (scripts/crash_markers.py counts it as fatal), because
// continuing would run the caller on a result the guest never produced.
//
// SR_ALLOW_FORCING=1: the legacy `r3 = 0` continue (a band-aid, still logged).
// SR_ICALL_DIAG=<N> (observe-only, any mode): log the first N unresolved
// calls instead of the default 8 (under the master gate every one of them is
// survivable, so a roll can collect the whole population).
#include "../guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"
#include "guest_context.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace sr { namespace kernel {

namespace {

unsigned icallDiagBudget() {
  static const unsigned v = [] {
    const char* e = std::getenv("SR_ICALL_DIAG");
    if (!e || !e[0]) return 8u;
    const unsigned long n = std::strtoul(e, nullptr, 0);
    return n > 100000ul ? 100000u : static_cast<unsigned>(n);
  }();
  return v;
}

// Fault-free guest word read for diagnostics (the harness VEH makes a host AV
// fatal, so never fault-and-catch): only VAs whose host bytes are readable.
bool peekGuestU32(uint32_t va, uint32_t& out) {
  if (va < 0x10000u || (va & 3u) != 0) return false;
  uint8_t* p = guestVAToHost(va);
  if (!p || !hostRangeReadable(p, 4)) return false;
  out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  return true;
}

const char* kindOf(uint32_t target) {
  if (target == 0) return "null";
  if (target < PPC_CODE_BASE || target >= PPC_CODE_BASE + PPC_CODE_SIZE) return "out-of-range";
  return "unmapped";   // inside the code range, no recompiled function starts there
}

}  // namespace

void indirectCallUnresolved(PPCContext& ctx, uint32_t target) {
  static std::atomic<uint64_t> s_count{0};
  const uint64_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool cont = forcingAllowed();        // legacy fabricated-return path
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);   // generated code sets lr = call site + 4
  const unsigned budget = icallDiagBudget();
  if (n <= budget || (!cont) || (n & (n - 1)) == 0) {
    const uint32_t obj = ctx.r3.u32;
    uint32_t vt = 0, s8 = 0, s18 = 0, s1c = 0;
    const bool haveVt = peekGuestU32(obj, vt);
    if (haveVt) {
      peekGuestU32(vt + 0x08u, s8);
      peekGuestU32(vt + 0x18u, s18);
      peekGuestU32(vt + 0x1Cu, s1c);
    }
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[ICALL_UNRESOLVED] #%llu kind=%s target=%08X call_site=%08X (lr=%08X) "
      "r3(obj)=%08X vt=%s%08X vt[+8]=%08X vt[+0x18]=%08X vt[+0x1C]=%08X "
      "r4=%08X r5=%08X r1=%08X ctr=%08X tid=%lu -> %s",
      (unsigned long long)n, kindOf(target), target, lr ? lr - 4u : 0u, lr,
      obj, haveVt ? "" : "(unreadable)", vt, s8, s18, s1c,
      ctx.r4.u32, ctx.r5.u32, ctx.r1.u32, ctx.ctr.u32,
#if defined(_WIN32)
      (unsigned long)GetCurrentThreadId(),
#else
      0ul,
#endif
      cont ? "r3=0, continue (SR_ALLOW_FORCING legacy band-aid)" : "FAIL");
    // The registers a loop / member call usually holds its source pointer in,
    // and a re-read of the words r28/r31 point at (was the pointer read from a
    // location whose value changed under us?).
    uint32_t m28 = 0, m31 = 0;
    const bool h28 = peekGuestU32(ctx.r28.u32, m28), h31 = peekGuestU32(ctx.r31.u32, m31);
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[ICALL_UNRESOLVED] #%llu regs r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X "
      "*r28=%s%08X *r31=%s%08X r11=%08X r12=%08X",
      (unsigned long long)n, ctx.r27.u32, ctx.r28.u32, ctx.r29.u32, ctx.r30.u32, ctx.r31.u32,
      h28 ? "" : "?", m28, h31 ? "" : "?", m31, ctx.r11.u32, ctx.r12.u32);
  }
  if (cont) {
    ctx.r3.u32 = 0;
    return;
  }
  // The named failure. Console/Xenia would fault or run garbage here; the
  // default exe stops with the evidence above instead of fabricating a result.
  SR_LOG_BOOT(sr::LogSeverity::Error,
    "[ICALL_FAIL] unresolvable guest indirect call: target=%08X call_site=%08X - "
    "stopping (SR_ALLOW_FORCING=1 restores the legacy r3=0 continue)",
    target, lr ? lr - 4u : 0u);
  std::fprintf(stderr, "[ICALL_FAIL] target=%08X call_site=%08X\n", target, lr ? lr - 4u : 0u);
  std::fflush(stderr);
  sr::Log::shutdown();
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), 0xC1CA11FAu);
#endif
  std::_Exit(3);
}

}}  // namespace sr::kernel
