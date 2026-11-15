// iter-463 charter (burndown item 10): PPC_CALL_INDIRECT_FUNC wiring test.
//
// Runs the REAL macro (tools/regen/hand/ppc_context.h, as deployed in
// guest/ppc) against a private dispatch table, through the REAL
// sr::kernel::indirectCallUnresolved (kernel/indirect_call.cpp).
//
//   SR_ALLOW_FORCING=1 difftest --icall-test continue
//       mapped target dispatches; NULL / unmapped / out-of-range / the six
//       formerly special-cased VAs all reach the handler, which returns r3=0
//       (the legacy band-aid, master-gated) - prints "N passed, 0 failed".
//   difftest --icall-test fail       (no forcing)
//       a mapped call works, then an unresolvable call must STOP the process
//       with exit code 0xC1CA11FA and an [ICALL_FAIL] line; reaching the end
//       of this mode is itself the failure (returns 1).
// tools/difftest/icall_test_run.py runs both modes and checks the exit codes.
#include "ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

constexpr uint32_t kMappedVA = 0x82110040u;   // any in-range VA; the table is private

void mappedTarget(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u32 = 0x1234ABCDu;
}

// One guest-style call site: `ctx.lr = site + 4; bctrl` with ctr = target.
void callSite(PPCContext& ctx, uint8_t* base, uint32_t target) {
  ctx.ctr.u32 = target;
  ctx.lr = 0x82722E34u;
  PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

struct Table {
  std::unique_ptr<PPCFunc*[]> storage;
  PPCFunc** saved = nullptr;
  Table() {
    const size_t n = (size_t)(PPC_CODE_SIZE >> 2) + 1;
    storage = std::make_unique<PPCFunc*[]>(n);
    std::memset(storage.get(), 0, n * sizeof(PPCFunc*));
    storage[(kMappedVA - PPC_CODE_BASE) >> 2] = &mappedTarget;
    saved = sr::kernel::g_dispatchTable;
    sr::kernel::g_dispatchTable = storage.get();
  }
  ~Table() { sr::kernel::g_dispatchTable = saved; }
};

}  // namespace

int runIcallTest(const char* mode) {
  const bool cont = mode && std::strcmp(mode, "continue") == 0;
  if (cont && !sr::kernel::forcingAllowed()) {
    std::printf("  FAIL continue mode needs SR_ALLOW_FORCING=1 in the environment\n");
    std::printf("[ICALL_TEST] 0 passed, 1 failed\n");
    return 1;
  }
  uint8_t* base = PPC_GUEST_BASE();
  Table t;
  PPCContext ctx{};
  ctx.r1.u32 = 0x70000000u;
  int ok = 0, bad = 0;
  auto check = [&](bool c, const char* what) {
    std::printf("  %s %s\n", c ? "ok  " : "FAIL", what);
    (c ? ok : bad)++;
  };

  ctx.r3.u32 = 0xDEADBEEFu;
  callSite(ctx, base, kMappedVA);
  check(ctx.r3.u32 == 0x1234ABCDu, "mapped target dispatches (r3 from the callee)");
  ctx.r3.u32 = 0xDEADBEEFu;
  callSite(ctx, base, kMappedVA | 2u);
  check(ctx.r3.u32 == 0x1234ABCDu, "low two target bits ignored (bcctr semantics)");

  if (!cont) {
    std::printf("  ...an unresolvable call must now stop the process (exit 0xC1CA11FA)\n");
    std::fflush(stdout);
    ctx.r3.u32 = 0x82123456u;
    callSite(ctx, base, 0x01010000u);          // the lr 0x82722E34 garbage-vtable shape
    std::printf("  FAIL the unresolvable call RETURNED on the default path\n");
    std::printf("[ICALL_TEST] %d passed, %d failed\n", ok, bad + 1);
    return 1;
  }

  const struct { uint32_t va; const char* what; } kUnresolved[] = {
    {0x00000000u, "NULL target -> handler, r3=0 under SR_ALLOW_FORCING"},
    {0x01010000u, "out-of-range (lr 0x82722E34 shape) -> handler"},
    {0xFFFFFFFEu, "out-of-range 0xFFFFFFFE -> handler"},
    {0x82110080u, "in-range, unmapped -> handler"},
    {0x822979C4u, "ex-special-case 0x822979C4 (not in this table) -> handler"},
    {0x8276A520u, "ex-special-case 0x8276A520 -> handler"},
    {0x827648B4u, "ex-special-case 0x827648B4 -> handler"},
  };
  for (const auto& u : kUnresolved) {
    ctx.r3.u32 = 0x5A5A5A5Au;
    callSite(ctx, base, u.va);
    check(ctx.r3.u32 == 0u, u.what);
  }
  std::printf("[ICALL_TEST] %d passed, %d failed\n", ok, bad);
  return bad ? 1 : 0;
}
