// D20 fixture: a FAKE RETURN in a kernel strong-def override of a guest function
// (skip the guest body, fabricate its result) was not a site at all - only the
// D16 primitive over tools/regen/hand saw register writes (round-4 verifier U1:
// 16 such branches ran with no gate or default ON).
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); } }
struct R { uint32_t u32; uint64_t u64; int64_t s64; };
struct PPCContext { R r3, r4, r5, ctr; uint64_t lr; };
extern "C" void __imp__sub_82000100(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82000200(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82000300(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82000400(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82000500(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82000600(PPCContext& ctx, uint8_t* base);
#define PPC_FUNC(x) void x(PPCContext& __restrict ctx, uint8_t* base)
static bool bad(uint32_t v) { return v < 0x10000u; }

// UNSAFE-direction case: the guard branch fabricates the return and skips the body.
void sub_82000100(PPCContext& __restrict ctx, uint8_t* base) {
  if (bad(ctx.r3.u32)) {
    ctx.r3.u32 = 0;  // EXPECT: UNGATED
    return;
  }
  __imp__sub_82000100(ctx, base);
}

// UNSAFE-direction case: PPC_FUNC spelling, a non-r3 register, one-line branch.
PPC_FUNC(sub_82000200) {
  if (bad(ctx.r4.u32)) { ctx.ctr.u32 = 0x82000000u; return; }  // EXPECT: UNGATED
  __imp__sub_82000200(ctx, base);
}

// UNSAFE-direction case: a default-ON own gate is DEFAULT-ON, not opt-in.
void sub_82000300(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool on = [] { const char* e = std::getenv("SR_FR_GUARD"); return !e || e[0] != '0'; }();
  if (on && bad(ctx.r3.u32)) {
    ctx.r3.u32 = ctx.r5.u32;  // EXPECT: OWNGATE_ON
    return;
  }
  __imp__sub_82000300(ctx, base);
}

// UNSAFE-direction case: a RESULT SUBSTITUTED after the guest body ran.
void sub_82000400(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82000400(ctx, base);
  if (ctx.r3.u32 == 0) ctx.r3.u32 = 0x40u;  // EXPECT: UNGATED
}

// Control: the same fabrication behind the master gate is MASTER.
void sub_82000500(PPCContext& __restrict ctx, uint8_t* base) {
  if (sr::kernel::forcingAllowed() && bad(ctx.r3.u32)) {
    ctx.r3.u32 = 0;  // EXPECT: MASTER
    return;
  }
  __imp__sub_82000500(ctx, base);
}

// Control: a register written only to feed the guest body (an argument set up
// before `__imp__`) is not a fake return.
void sub_82000600(PPCContext& __restrict ctx, uint8_t* base) {
  ctx.r4.u32 = ctx.r3.u32 + 4u;
  __imp__sub_82000600(ctx, base);
}

// Control: a host helper (not an override) that writes a struct register copy
// is not a site.
void hostHelper(PPCContext& ctx) { (void)ctx; }
