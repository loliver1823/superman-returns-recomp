// D16 fixture: hand-written guest code (compiled as guest/ppc/, committed under
// tools/regen/hand/) was never classified - the classifier skipped guest/ppc,
// scan_generated only looks behind inject markers, and the registry scan did
// not read this file. Its strong defs REPLACE guest functions.
#include "ppc_config.h"
#include "ppc_context.h"
#define PPC_MISSING_STUB(name) PPC_FUNC(name) { (void)ctx; (void)base; }

// UNSAFE-direction case: a fabricated return value (the real function at that
// VA does something else).
PPC_FUNC(sub_824B9560) { ctx.r3.u32 = 0; }  // EXPECT: UNGATED

// UNSAFE-direction case: a hand store that skips the guest's own check.
PPC_FUNC(sub_822F6F28) {
  uint32_t destVA = ctx.r3.u32 + 0x18;
  uint8_t* srcHost = sr::kernel::guestVAToHost(ctx.r4.u32);
  if (srcHost) {
    for (;;) {
      uint8_t c = *srcHost++;
      PPC_STORE_U8(destVA, c);  // EXPECT: UNGATED
      if (c == 0) break;
      destVA++;
    }
  }
}

// Control: a fabrication behind the master gate is MASTER.
PPC_FUNC(sub_82001230) {
  if (sr::kernel::forcingAllowed()) ctx.r3.u32 = 0;  // EXPECT: MASTER
}

// Controls: an empty no-op stub and a pure read have no write site (the
// registry classifies them - gen_override_manifest scans this file).
PPC_MISSING_STUB(sub_828D0EC8)
PPC_FUNC(sub_82765994) { (void)ctx; (void)base; }
