// B2 fixture: the FIRST gate helper found in a body won, so an own-gate
// helper used for logging beat the master-gate helper that guards the write
// (kernel/apt_as_watch.cpp: watchEnabled() vs autoFrameEnabled()).
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }

bool logOn() {
  const char* e = std::getenv("SR_FIXTURE_B2_LOG");
  return !e || e[0] != '0';
}

bool autoFrameOn() {
  if (!sr::kernel::forcingAllowed()) return false;
  static const bool v = [] {
    const char* e = std::getenv("SR_FIXTURE_B2_FRAME");
    return !e || e[0] != '0';
  }();
  return v;
}

void hook(uint32_t va) {
  if (logOn()) { /* log only */ }
  if (autoFrameOn()) {
    sr::kernel::writeGuestBE32(va, 8u);  // EXPECT: MASTER
  }
}

// Site-level form (iter-463 round 2): both gates guard the write; the master
// gate is the stronger requirement. Reverted, the first operand (logOn) won.
void hookBoth(uint32_t va) {
  if (logOn() && autoFrameOn()) sr::kernel::writeGuestBE32(va, 9u);  // EXPECT: MASTER
}

// Control: an own gate alone is still an own gate.
void hookOwnOnly(uint32_t va) {
  if (logOn()) sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: OWNGATE_ON
}
