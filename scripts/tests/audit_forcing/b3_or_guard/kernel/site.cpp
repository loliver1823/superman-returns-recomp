// B3 fixture: `if (!forcingAllowed() || !own) <exit>` means master AND own,
// but OR_BYPASS_RE read every `forcingAllowed() ||` as a bypass
// (crt_file_lock_init_fix.cpp, dogma_hash_hooks.cpp, gpu_ring_alloc_fix.cpp).
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }
void passthrough(uint32_t);

bool fabOn() {
  const char* e = std::getenv("SR_FIXTURE_B3_FAB");
  return !e || e[0] != '0';
}

void hookGuardBlock(uint32_t va) {
  if (!sr::kernel::forcingAllowed() || !fabOn()) { passthrough(va); return; }
  sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: MASTER
}

void hookGuardReturn(uint32_t va) {
  if (!sr::kernel::forcingAllowed() || !fabOn()) return;
  sr::kernel::writeGuestBE32(va, 2u);  // EXPECT: MASTER
}

// A predicate built on the same guard form is a master predicate.
bool fabGate() {
  if (!sr::kernel::forcingAllowed() || !fabOn()) return false;
  return true;
}

void hookViaPredicate(uint32_t va) {
  if (fabGate()) sr::kernel::writeGuestBE32(va, 5u);  // EXPECT: MASTER
}

// Controls: real bypasses stay DEFAULT-ON.
void hookBypass(uint32_t va) {
  if (fabOn() || sr::kernel::forcingAllowed()) sr::kernel::writeGuestBE32(va, 3u);  // EXPECT: OWNGATE_ON
}

// (iter-463 round 2: judged at the site, `!M || fabOn()` holds whenever forcing
// is off, whatever SR_FIXTURE_B3_FAB says - the write is UNGATED, not merely
// default-ON. The guest-called override below keeps it from reading dead code.)
void hookNegatedOrBody(uint32_t va) {
  if (!sr::kernel::forcingAllowed() || fabOn()) { sr::kernel::writeGuestBE32(va, 4u); }  // EXPECT: UNGATED
}
struct PPCContext { struct { uint32_t u32; } r3; };
void sub_82000030(PPCContext& ctx, uint8_t* base) { hookNegatedOrBody(ctx.r3.u32); }
