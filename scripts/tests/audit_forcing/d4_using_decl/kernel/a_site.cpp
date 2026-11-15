// D4 fixture: a using-DECLARATION (`using sr::diag::hackOn;`, not a using-
// directive) brings one gate into this file. Round 1 ignored it, so the gate was
// lost (verifier P8: a default-ON own gate read as dead-code opt-in).
#include <cstdint>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }
namespace sr { namespace diag { bool hackOn(); bool traceOn(); } }
struct PPCContext { struct { uint32_t u32; } r3; };

using sr::diag::hackOn;
using ::sr::diag::traceOn;

// UNSAFE-direction case: default-ON gate -> ships.
void d4Hack(uint32_t va) {
  if (hackOn()) sr::kernel::writeGuestBE32(va, 8u);  // EXPECT: OWNGATE_ON
}

// Control: a default-OFF gate brought in the same way stays opt-in.
void d4Trace(uint32_t va) {
  if (traceOn()) sr::kernel::writeGuestBE32(va, 9u);  // EXPECT: OWNGATE
}

void sub_82000040(PPCContext& ctx, uint8_t* base) { d4Hack(ctx.r3.u32); d4Trace(ctx.r3.u32); }
