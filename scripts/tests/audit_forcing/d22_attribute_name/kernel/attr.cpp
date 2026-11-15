// D22 fixture: func_name named a function after a leading attribute specifier
// (`__attribute__((noinline)) void attrWriter(..) {` was "__attribute__"), so the
// site below was attributed to "__attribute__", and "__attribute__" joined the
// host-function set: every generated `__attribute__((alias(..))) PPC_WEAK_FUNC`
// line (guest/ppc/ppc_recomp.0.cpp here) then read as a host call, and as a
// FAILING one because the misnamed function writes guest memory.
#include <cstdint>
namespace sr { namespace kernel {
void writeGuestBE32(uint32_t, uint32_t);
__attribute__((noinline)) void attrWriter(uint32_t va) {
  writeGuestBE32(va, 1u);  // EXPECT: OWNGATE fn=attrWriter  (no reference: dead code)
}
} }
