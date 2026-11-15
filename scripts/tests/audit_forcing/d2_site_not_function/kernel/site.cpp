// D2 fixture: gates are judged AT THE WRITE, not per function. Round 1 let the
// master gate anywhere in the body win (B2) and read the B3 guard form wherever
// it sat, so a guard AFTER the write (verifier P2) and a master helper used in an
// unrelated branch (P3) both made shipping writes read MASTER.
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }
struct PPCContext { struct { uint32_t u32; } r3; };
void logit();

bool ownOn() {
  const char* e = std::getenv("SR_FIXTURE_D2_OWN");
  return !e || e[0] != '0';
}

bool masterHelper() {
  if (!sr::kernel::forcingAllowed()) return false;
  return true;
}

// UNSAFE-direction case P2: the guard sits after the write.
void d2GuardAfter(uint32_t va) {
  sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: UNGATED
  if (!sr::kernel::forcingAllowed() || !ownOn()) return;
  logit();
}

// UNSAFE-direction case P3: master helper in another branch, the write sits
// under a default-ON own gate.
void d2UnrelatedMaster(uint32_t va) {
  if (masterHelper()) logit();
  if (ownOn()) sr::kernel::writeGuestBE32(va, 2u);  // EXPECT: OWNGATE_ON
}

// UNSAFE-direction case: the else branch of the master check.
void d2ElseOfMaster(uint32_t va) {
  if (sr::kernel::forcingAllowed()) {
    logit();
  } else {
    sr::kernel::writeGuestBE32(va, 3u);  // EXPECT: UNGATED
  }
}

// Controls: the write inside the master branch / after a master guard.
void d2InMaster(uint32_t va) {
  if (masterHelper()) {
    logit();
    sr::kernel::writeGuestBE32(va, 4u);  // EXPECT: MASTER
  }
}

void d2AfterGuard(uint32_t va) {
  if (!masterHelper()) return;
  if (va) sr::kernel::writeGuestBE32(va, 5u);  // EXPECT: MASTER
}

// Control: a preprocessor line in front of the master check (stubs.cpp VdSwap:
// `#ifdef _WIN32` before `if (forcingAllowed()) {`).
void d2Ifdef(uint32_t va) {
#ifdef _WIN32
  if (sr::kernel::forcingAllowed()) {
    sr::kernel::writeGuestBE32(va, 6u);  // EXPECT: MASTER
  }
#endif
}

void sub_82000020(PPCContext& ctx, uint8_t* base) {
  d2Ifdef(ctx.r3.u32);
  d2GuardAfter(ctx.r3.u32);
  d2UnrelatedMaster(ctx.r3.u32);
  d2ElseOfMaster(ctx.r3.u32);
  d2InMaster(ctx.r3.u32);
  d2AfterGuard(ctx.r3.u32);
}
