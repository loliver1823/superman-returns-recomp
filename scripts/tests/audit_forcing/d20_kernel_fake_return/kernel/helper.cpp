// D20 fixture: host helpers the indirect-call macro calls with ctx.
#include <cstdint>
namespace sr { namespace kernel { bool forcingAllowed(); } }
struct R { uint32_t u32; };
struct PPCContext { R r3; };

namespace sr { namespace kernel {
// UNSAFE-direction case: the helper fabricates r3 for an unresolvable call
// with no gate (the pre-iter-463 PPC_CALL_INDIRECT_FUNC branches, moved host-side).
void fixtureUnresolved(PPCContext& ctx, uint32_t target) {
  (void)target;
  ctx.r3.u32 = 0;  // EXPECT: UNGATED
}
// Control: a PPCContext& helper the macro does NOT call is not a D20 site.
void notCalledFromMacro(PPCContext& ctx) {
  ctx.r3.u32 = 1;
}
}}
