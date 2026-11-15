// D21 fixture: an override of a guest function that ALSO has a gated HOST
// reference (gpu_ring_alloc_fix.cpp: a master-gated isolation probe takes
// &sub_82467F40) read "caller-gated: MASTER" - the weakest-caller walk counted
// only host references and the B4 lift raised it - although generated guest
// code calls the override on every allocation, with no gate.
#include <cstdint>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }
struct R { uint32_t u32; };
struct PPCContext { R r3; };
extern "C" void __imp__sub_82000700(PPCContext& ctx, uint8_t* base);
using GuestFn = void(PPCContext&, uint8_t*);

// UNSAFE-direction case: the override writes guest memory on its default path.
void sub_82000700(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82000700(ctx, base);
  sr::kernel::writeGuestBE32(ctx.r3.u32, 7u);  // EXPECT: UNGATED
}

// The only HOST reference is master-gated.
static void runProbe() {
  if (!sr::kernel::forcingAllowed()) return;
  GuestFn* fn = &sub_82000700;
  PPCContext c{};
  fn(c, nullptr);
}
void kickProbe() { runProbe(); }

// Control: a plain host helper reached only through the master gate stays MASTER.
static void hostOnly() { sr::kernel::writeGuestBE32(0x20u, 1u); }  // EXPECT: MASTER
void kickHost() { if (sr::kernel::forcingAllowed()) hostOnly(); }
