// C23 math used by recompiled guest code; not in MSVC CRT.
#include <cmath>

// roundevenf: round to nearest, ties to EVEN -- the semantics of PPC vrfin, of SSE4.1
// roundps(_MM_FROUND_TO_NEAREST_INT) and of Xenia's vroundps lowering. The guest lib is built
// without -msse4.1, so simde_mm_round_ps(TO_NEAREST_INT) (vrfin, 6 sites) falls back to
// __builtin_roundevenf, which links HERE.
//
// iter-463 platform fix: the tie branch used to step AWAY from x (r - 1 when d > 0), so
// 0.5 -> 2, 2.5 -> 4, -0.5 -> -2. With x = x_in, r = roundf(x) (ties away from zero) and
// d = x - r, a tie with an odd r must move r one step TOWARD x: d > 0 means x > r, so r + 1.
// That broke VmxSinCosPoly_sub_8213BD10 at exactly +-pi (x/2pi = 0.5): sin/cos(pi) came out
// (0.13, -1.34), so every camera view matrix built from an Euler with a +-pi lane was rolled
// and scaled. Also: the result keeps x's sign (roundps(-0.5) = -0.0, not +0.0), and a NaN is
// returned QUIETED (x + x), as roundps / vrfin do. Proven bit-exact against SSE4.1 roundps by
// build/difftest.exe --fp-platform-test.
extern "C" float roundevenf(float x) {
  if (std::isnan(x)) return x + x;   // quiet an SNaN, keep the payload
  if (std::isinf(x)) return x;
  float r = std::roundf(x);
  float d = x - r;
  if (std::abs(d) == 0.5f) {
    if (std::fmod(r, 2.0f) != 0.0f)
      r = (d > 0) ? r + 1.0f : r - 1.0f;
  }
  return std::copysign(r, x);
}
