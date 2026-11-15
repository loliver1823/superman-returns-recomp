// D17 fixture (round-3 verifier probe tree v3/p6): a host function called ONLY
// from a hand edit in generated guest code (a splice body) read "dead code: no
// reference" = OWNGATE - the reference index read the scanned .cpp files only.
#include <cstdint>
namespace sr { namespace kernel {
void writeGuestBE32(uint32_t, uint32_t);
bool forcingAllowed();

// UNSAFE-direction case: called from the splice body, ungated.
void forceFlag(uint32_t va) {
  writeGuestBE32(va, 1u);  // EXPECT: UNGATED
}
// UNSAFE-direction case: called WITHOUT sr::kernel:: from the splice
// (walkStaticCtors_impl shape), under an inline `!e || e[0] != '0'` there -
// which a CALLER-side reading treats as a kill switch (D10: indistinguishable
// from `if (!testMode) run();`), so it reads UNGATED: the safe direction.
void walkLike(uint32_t va) {
  writeGuestBE32(va, 2u);  // EXPECT: UNGATED
}
// UNSAFE-direction case: called only from an inline function in a header.
void headerOnly(uint32_t va) {
  writeGuestBE32(va, 3u);  // EXPECT: UNGATED
}
// Control: the splice calls it only under the master gate.
void masterOnly(uint32_t va) {
  writeGuestBE32(va, 4u);  // EXPECT: MASTER
}
// Control: nothing references it anywhere (a declaration in the header does
// not count) - dead code stays dead.
void neverCalled(uint32_t va) {
  writeGuestBE32(va, 5u);  // EXPECT: OWNGATE
}
} }
