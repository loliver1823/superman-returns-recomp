#pragma once
// iter-467 freezefx: SR_PM4_HALF_SIGNED (default ON; =0 keeps the old compare for an A/B).
//
// The GPU float32 -> float16 packers in the EDRAM store kernel (d3d12_pm4_edram.cpp kPm4EdramHlsl
// FloatToHalf) and the native resolve kernel (native_resolve.inc NatFloatToHalf) are compiled at
// run time with FXC (D3DCompile, cs_5_1). FXC types an unsuffixed HEX literal as uint, so their
// overflow test `int e ...; if (e >= 0x1F)` compiled to an UNSIGNED compare (uge): every half
// with a biased exponent e <= 0 - zero and every |f| < 2^-14 - was packed as +-inf. The
// k_16_16_FLOAT / k_16_16_16_16_FLOAT render targets therefore reached EDRAM (and every resolve
// and transfer through it) with +inf where the host RT held 0. Seen: the frozen meteor's frost
// cloud (soft particles fading against a half-res max/min depth RT whose sky = 0) vanished over
// the sky. The fix is a decimal bound (`e >= 31`, a signed compare); =0 compiles SR_HALF_HEXCMP,
// the old hex compare.
#include <cstdlib>

#include "../kernel/sr_oracle.hpp"

inline bool pm4HalfSignedOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_HALF_SIGNED");
    return !e || e[0] != '0';
  }();
  return v;
#else
  // C1 stage 3 (docs/RELEASE_COMPOSITION.md 3.1 rule 2): the =0 arm is the old unsigned compare - a fallback
  // component (and a wrong one); the release configuration compiles the signed compare only, the gate's name absent.
  return true;
#endif
}
