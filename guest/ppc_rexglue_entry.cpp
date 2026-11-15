// ReXGlue entry point: Windows COFF does not reliably support weak alias, so provide
// sub_820F0000 by forwarding to __imp__sub_820F0000 (defined in recomp.0.cpp).
// Harness uses PPC_EXTERN_FUNC (C++ linkage with __restrict), so we must match.
#include "ppc/ppc_context.h"

extern "C" void __imp__sub_820F0000(PPCContext& ctx, uint8_t* base);

void sub_820F0000(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_820F0000(ctx, base);
}
