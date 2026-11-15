// D1 fixture: an early-exit guard counts only if its branch ALWAYS exits.
// Round 1's _exit_guard accepted any block whose LAST statement was a return,
// so `if (!M || !own) { if (va == 0) return; }` (verifier probe P1) read as a
// master guard and the write after it as MASTER - it runs with forcing off.
// Every function is reached from the guest-called override at the bottom, so an
// ungated site reads UNGATED (not "dead code").
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }
struct PPCContext { struct { uint32_t u32; } r3; };
void logit();

bool ownOn() {
  const char* e = std::getenv("SR_FIXTURE_D1_OWN");
  return !e || e[0] != '0';
}

// UNSAFE-direction case: the block ends in a nested if's return, so with
// forcing off and va != 0 control falls through to the write.
void d1NestedReturn(uint32_t va) {
  if (!sr::kernel::forcingAllowed() || !ownOn()) { if (va == 0) return; }
  sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: UNGATED
}

// UNSAFE-direction case: only one branch of the inner if/else exits.
void d1HalfExit(uint32_t va) {
  if (!sr::kernel::forcingAllowed()) { if (va) { return; } else { logit(); } }
  sr::kernel::writeGuestBE32(va, 2u);  // EXPECT: UNGATED
}

// Controls: blocks that always exit are real guards.
void d1BothExit(uint32_t va) {
  if (!sr::kernel::forcingAllowed()) { if (va) return; else return; }
  sr::kernel::writeGuestBE32(va, 3u);  // EXPECT: MASTER
}

void d1ExitThenLog(uint32_t va) {
  if (!sr::kernel::forcingAllowed() || !ownOn()) { logit(); return; }
  sr::kernel::writeGuestBE32(va, 4u);  // EXPECT: MASTER
}

void d1LoopContinue(const uint32_t* vas, int n) {
  for (int i = 0; i < n; ++i) {
    if (!sr::kernel::forcingAllowed()) { logit(); continue; }
    sr::kernel::writeGuestBE32(vas[i], 5u);  // EXPECT: MASTER
  }
}

void sub_82000010(PPCContext& ctx, uint8_t* base) {
  d1NestedReturn(ctx.r3.u32);
  d1HalfExit(ctx.r3.u32);
  d1BothExit(ctx.r3.u32);
  d1ExitThenLog(ctx.r3.u32);
  d1LoopContinue(nullptr, 0);
}
