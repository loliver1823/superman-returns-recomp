// D40 fixture (iter-463 phase 2, docs/DEVTOOLS_DESIGN.md 2.1): a dev-tool gate is a level of
// its own, DEVTOOL, recognized AT THE SITE - devToolAllowed("<tool>") and the hook aggregate
// devToolHookNeeded_sub_X(). With D40 reverted every gated write below reads UNGATED. An OR
// with anything, or the negated gate, gates nothing.
#include <cstdint>
struct PPCContext { struct { uint32_t u32; } r3; };
namespace sr { namespace kernel {
bool devToolAllowed(const char* tool);
bool devToolHookNeeded_sub_82000040();
void writeGuestBE32(uint32_t, uint32_t);
} }

void d40AfterGuard(uint32_t va) {
  if (!sr::kernel::devToolAllowed("levelskip")) return;
  sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: DEVTOOL
}

void d40InBranch(uint32_t va) {
  if (sr::kernel::devToolAllowed("levelskip")) {
    sr::kernel::writeGuestBE32(va, 2u);  // EXPECT: DEVTOOL
  }
}

void d40Throw(uint32_t va) {
  if (!sr::kernel::devToolAllowed("levelskip")) throw 1;
  sr::kernel::writeGuestBE32(va, 3u);  // EXPECT: DEVTOOL
}

void d40Hook(uint32_t va) {
  if (!sr::kernel::devToolHookNeeded_sub_82000040()) return;
  sr::kernel::writeGuestBE32(va, 4u);  // EXPECT: DEVTOOL
}

// Unsafe-direction controls: an OR bypass and the negated gate run by default.
void d40Or(uint32_t va, bool x) {
  if (sr::kernel::devToolAllowed("levelskip") || x) sr::kernel::writeGuestBE32(va, 5u);  // EXPECT: UNGATED
}

void d40Negated(uint32_t va) {
  if (!sr::kernel::devToolAllowed("levelskip")) sr::kernel::writeGuestBE32(va, 6u);  // EXPECT: UNGATED
}

void sub_82000040(PPCContext& ctx, uint8_t* base) {
  d40AfterGuard(ctx.r3.u32);
  d40InBranch(ctx.r3.u32);
  d40Throw(ctx.r3.u32);
  d40Hook(ctx.r3.u32);
  d40Or(ctx.r3.u32, ctx.r3.u32 != 0);
  d40Negated(ctx.r3.u32);
}
