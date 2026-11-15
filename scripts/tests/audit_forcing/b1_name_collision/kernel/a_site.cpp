// B1 fixture: gate helpers were looked up by BARE NAME across the tree.
// z_other.cpp (scanned after this file) defines its own watchEnabled(), and
// 0_txc.cpp a txc::enabled(), both default-ON own gates. The old lookup let the last
// definition win for every caller and matched member calls (cfg.enabled()).
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }
namespace sr { namespace kernel { namespace txc { bool enabled(); } } }

namespace {
// This file's watchEnabled(): false unless the master gate is on.
bool watchEnabled() {
  if (!sr::kernel::forcingAllowed()) return false;
  const char* e = std::getenv("SR_FIXTURE_B1_WATCH");
  return !e || e[0] != '0';
}
}  // namespace

void patchA(uint32_t va) {
  if (watchEnabled()) {
    sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: MASTER
  }
}

struct Cfg { bool enabled() const { return flag; } bool flag; };
bool diagOn();

// A member call is not the free gate helper txc::enabled(); the real gate is
// diagOn() (default OFF, z_other.cpp).
void patchC(const Cfg& cfg, uint32_t va) {
  if (cfg.enabled() && diagOn()) {
    sr::kernel::writeGuestBE32(va, 3u);  // EXPECT: OWNGATE
  }
}

// Control: a qualified call resolves to the helper in that namespace.
void patchD(uint32_t va) {
  if (sr::kernel::txc::enabled()) {
    sr::kernel::writeGuestBE32(va, 4u);  // EXPECT: OWNGATE_ON
  }
}
