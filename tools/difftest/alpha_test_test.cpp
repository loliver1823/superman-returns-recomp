// Step 4 lane C (gfx/alpha_test.hpp, tools/xenosrecomp/0005 + 0006): the Xenos alpha test.
// Built twice (CMakeLists.txt): SR_ORACLE=1 (dev: SR_XLAT_ALPHA_TEST read, default ON since the bake, =0 the A/B) and
// SR_ORACLE=0 (release: the constant). Checks
//   word()       RB_COLORCONTROL -> the variant's c17.y word for every enable / function, high bits ignored
//   passes()     the reference semantics against an independent transcription of Xenia's DXBC sequence
//                (DxbcShaderTranslator::CompletePixelShader, dxbc_shader_translator_om.cc 3282-3375,
//                use_fuzzy_alpha_epsilon false): UBFE the mask, skip on ALWAYS, NOT_EQUAL as NE, else the LESS / EQUAL /
//                GREATER mask with OR ~bit / AND, discard when the mask is 0; each DXBC float compare flushes a
//                denormal input - over every function and a grid of alphas / references with NaN (quiet, signalling,
//                negative), +-0, +-denormals, +-FLT_MIN, +-inf, FLT_MAX and the reference itself
//   passesBits() (round 4, Astra P2 3: the 0006 epilogue's integer compare, transcribed) equals passes() on that grid
//                and on 4M pseudo-random bit-pattern pairs (half of them near-equal / special-heavy)
//   applies()    (round 4, Astra P2 1) only kColorDepth, only a PS writing colour 0, only an enabled non-ALWAYS test
//   use()        (round 4, Astra P2 2) none / the variant / drop
//   mutants      GEQUAL as clip(a - ref) for every function; NOT_EQUAL as LESS | GREATER (false for NaN); the integer
//                compare without the denormal flush; the integer compare on raw unsigned bits (no sign-magnitude);
//                applies() ignoring edram_mode (the 0005 wiring); applies() ignoring colour 0 - each must differ
//   gate         dev default ON (baked 2026-10-20), =0 OFF; release constant ON
// The wiring (pm4PrepareDraw / nbClearCoverage using applies(), the drop, the runtime variant, the drain writing
// c17.x / c17.y) is proven by the replays and tools/difftest/shader_xlat_test.cpp T8; the COMPILED epilogue (no float
// compare on the reference) by tools/difftest/alpha_test_dxil_check.py.
#include "gfx/alpha_test.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[ALPHA_TEST_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}

uint32_t bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
float flt(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Xenia's DXBC, op by op (non-fuzzy). colorControl -> the system flag's 3-bit function (alpha_test_enable ? func :
// ALWAYS, d3d12_command_processor.cc 3801-3805). true = the pixel survives. Each float compare op (NE / LT / EQ) reads
// its inputs through the DXBC float32 rule: a denormal is flushed to a zero of its sign.
bool xeniaSurvives(uint32_t colorControl, float alpha0, float ref0) {
  const auto dx = [](float v) { return std::fpclassify(v) == FP_SUBNORMAL ? std::copysign(0.0f, v) : v; };
  const float alpha = dx(alpha0), ref = dx(ref0);
  const uint32_t func = (colorControl & 8u) ? (colorControl & 7u) : 7u;
  uint32_t mask = func;                      // OpUBFE(mask, 3, shift, flags)
  const bool notAlways = mask != 7u;         // OpINE(op, mask, kAlways)
  if (!notAlways) return true;               // OpIf(op) ... the test is skipped
  if (mask == 5u) {                          // OpIEq(op, mask, kNotEqual); OpIf
    mask = (alpha != ref) ? 0xFFFFFFFFu : 0u;  // OpNE(mask, alpha, ref)
  } else {
    uint32_t op = (alpha < ref) ? 0xFFFFFFFFu : 0u;  // OpLT(op, alpha, ref)
    op |= ~uint32_t(1u << 0);                         // OpOr(op, op, ~(1 << 0))
    mask &= op;                                       // OpAnd(mask, mask, op)
    op = (alpha == ref) ? 0xFFFFFFFFu : 0u;           // OpEq
    op |= ~uint32_t(1u << 1);
    mask &= op;
    op = (ref < alpha) ? 0xFFFFFFFFu : 0u;            // OpLT(op, ref, alpha)
    op |= ~uint32_t(1u << 2);
    mask &= op;
  }
  return mask != 0u;                         // OpDiscard(false, mask): discard when mask == 0
}

bool clipGequal(uint32_t, float a, float ref) { return !(a - ref < 0.0f); }  // upstream's clip(oC0.w - ref)
bool neAsLessGreater(uint32_t w, float a, float ref) {
  if (!(w & 8u) || (w & 7u) == 7u) return true;
  const uint32_t m = (w & 7u) == 5u ? 5u : (w & 7u);
  return ((m & 1u) && a < ref) || ((m & 2u) && a == ref) || ((m & 4u) && a > ref);
}
// The integer compare without the denormal flush (denormals compared exactly).
bool bitsNoFtz(uint32_t w, uint32_t a, uint32_t r) {
  if (!(w & 8u) || (w & 7u) == 7u) return true;
  const uint32_t m = w & 7u, am = a & 0x7FFFFFFFu, rm = r & 0x7FFFFFFFu;
  const bool unord = am > 0x7F800000u || rm > 0x7F800000u;
  const int32_t ka = (a >> 31) ? -int32_t(am) : int32_t(am), kr = (r >> 31) ? -int32_t(rm) : int32_t(rm);
  const bool eq = !unord && ka == kr;
  if (m == 5u) return !eq;
  return ((m & 1u) && !unord && ka < kr) || ((m & 2u) && eq) || ((m & 4u) && !unord && ka > kr);
}
// The integer compare on the raw bits as unsigned numbers (no sign-magnitude key).
bool bitsUnsigned(uint32_t w, uint32_t a, uint32_t r) {
  if (!(w & 8u) || (w & 7u) == 7u) return true;
  const uint32_t m = w & 7u;
  const bool unord = (a & 0x7FFFFFFFu) > 0x7F800000u || (r & 0x7FFFFFFFu) > 0x7F800000u;
  const bool eq = !unord && a == r;
  if (m == 5u) return !eq;
  return ((m & 1u) && !unord && a < r) || ((m & 2u) && eq) || ((m & 4u) && !unord && a > r);
}
// applies() as 0005 wired it (no edram_mode check), and one that ignores colour 0.
constexpr bool appliesNoMode(uint32_t cc, uint32_t, uint32_t ps) { return sr::gfx::alphatest::word(cc) && (ps & 1u); }
constexpr bool appliesNoColor0(uint32_t cc, uint32_t mode, uint32_t) {
  return sr::gfx::alphatest::word(cc) && (mode & 7u) == 4u;
}

uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint32_t rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return uint32_t(g_rng >> 16);
}
}  // namespace

int main() {
  using namespace sr::gfx::alphatest;
  bool wordOk = true;
  for (uint32_t cc = 0; cc < 16; ++cc) {
    const uint32_t hi = 0xAA000000u | 0x10u;  // alpha-to-mask offsets / enable must not reach the word
    const uint32_t expect = (cc & 8u) && (cc & 7u) != 7u ? (8u | (cc & 7u)) : 0u;
    wordOk = wordOk && word(cc) == expect && word(cc | hi) == expect;
  }
  check(wordOk, "word: 8 | func when enabled and not ALWAYS, else 0, for all 16 low nibbles (high bits ignored)");
  check(word(0xAA00000Cu) == 0xCu, "word: the arena grate's RB_COLORCONTROL 0xAA00000C (GREATER, on) -> 0xC");
  check(word(0xAA000004u) == 0u, "word: the lava's 0xAA000004 (GREATER, off) -> 0 (no test)");

  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const float dmin = std::numeric_limits<float>::denorm_min(), nmin = std::numeric_limits<float>::min();
  const float dmax = flt(0x007FFFFFu), fmax = std::numeric_limits<float>::max();
  const float snan = flt(0x7F800001u), nnan = flt(0xFFC00000u);
  const float refs[] = {0.0f, 0.50196081f, 1.0f, -0.0f, 0.25f, dmin, -dmin, nmin, -1.0f, inf, nan};
  const float alphas[] = {0.0f, -0.0f, 0.50196081f, 0.5f, 0.502f, 1.0f, 0.25f, -1.0f, 2.0f, inf, -inf, nan,
                          snan, nnan, dmin, -dmin, dmax, -dmax, nmin, -nmin, fmax, -fmax,
                          std::nextafter(0.50196081f, 1.0f), std::nextafter(0.50196081f, 0.0f)};
  int cases = 0, bad = 0, badBits = 0, badClip = 0, badNe = 0, badNoFtz = 0, badUns = 0;
  for (uint32_t cc = 0; cc < 16; ++cc)
    for (float ref : refs)
      for (float a : alphas) {
        const bool x = xeniaSurvives(cc, a, ref);
        const uint32_t w = word(cc);
        ++cases;
        bad += passes(w, a, ref) != x;
        badBits += passesBits(w, bits(a), bits(ref)) != x;
        badClip += ((cc & 8u) ? clipGequal(w, a, ref) : true) != x;
        badNe += neAsLessGreater(w, a, ref) != x;
        badNoFtz += bitsNoFtz(w, bits(a), bits(ref)) != x;
        badUns += bitsUnsigned(w, bits(a), bits(ref)) != x;
      }
  std::printf("[ALPHA_TEST_TEST] %d grid cases: passes() mismatches %d, passesBits() %d; mutants clip(a-ref) %d, "
              "NE-as-LT|GT %d, integer without the denormal flush %d, unsigned raw bits %d\n",
              cases, bad, badBits, badClip, badNe, badNoFtz, badUns);
  check(bad == 0, "passes() equals Xenia's DXBC sequence on every function x reference x alpha (NaN, +-0, denormals, "
                  "+-inf, FLT_MAX)");
  check(badBits == 0, "passesBits() (the 0006 integer epilogue) equals Xenia's DXBC sequence on the same grid");
  check(badClip > 0, "mutant: upstream's clip(oC0.w - ref) for every function is caught");
  check(badNe > 0, "mutant: NOT_EQUAL as LESS | GREATER (false for NaN) is caught");
  check(badNoFtz > 0, "mutant: the integer compare without the denormal flush is caught");
  check(badUns > 0, "mutant: the integer compare on raw unsigned bits (negative alphas) is caught");

  // 4M random pairs: half raw bit patterns, half special-heavy (exponent 0 / 255, near-equal magnitudes).
  int64_t sweep = 0, sweepBad = 0;
  for (int i = 0; i < 4000000; ++i) {
    uint32_t a = rnd(), r = rnd();
    if (i & 1) {
      const uint32_t pick = rnd() & 7u;
      if (pick == 0) a &= 0x807FFFFFu;                       // a denormal / zero
      if (pick == 1) a |= 0x7F800000u;                       // inf / NaN
      if (pick == 2) r = a ^ (rnd() & 3u);                   // a near-equal reference
      if (pick == 3) r = a ^ 0x80000000u;                    // the negated value (+-0 included)
      if (pick == 4) { a &= 0x807FFFFFu; r &= 0x807FFFFFu; } // both denormal / zero
    }
    const uint32_t w = 8u | (rnd() & 7u);
    ++sweep;
    sweepBad += passesBits(w, a, r) != xeniaSurvives(w, flt(a), flt(r));
  }
  std::printf("[ALPHA_TEST_TEST] random sweep: %lld pairs, passesBits() mismatches %lld\n", (long long)sweep,
              (long long)sweepBad);
  check(sweepBad == 0, "passesBits() equals Xenia's DXBC sequence on 4M random bit-pattern pairs");
  check(!passes(word(0x8u), 1.0f, 0.0f) && !passes(word(0x8u), nan, 0.0f) && !passesBits(word(0x8u), bits(nan), 0u),
        "NEVER (enabled) discards every pixel");
  check(passes(word(0x5u), 0.0f, 1.0f) && passesBits(word(0x5u), 0u, bits(1.0f)),
        "a disabled test passes everything (the word is 0)");

  // applies(): edram_mode x colour-0 output x RB_COLORCONTROL.
  int appliesBad = 0, noModeBad = 0, noC0Bad = 0;
  for (uint32_t cc = 0; cc < 16; ++cc)
    for (uint32_t mode = 0; mode < 8; ++mode)
      for (uint32_t ps = 0; ps < 16; ++ps) {
        const bool expect = (cc & 8u) && (cc & 7u) != 7u && mode == 4u && (ps & 1u);
        const uint32_t m = mode | 0xFFFFFFF0u;  // high RB_MODECONTROL bits ignored
        appliesBad += applies(cc, m, ps) != expect;
        noModeBad += appliesNoMode(cc, m, ps) != expect;
        noC0Bad += appliesNoColor0(cc, m, ps) != expect;
      }
  check(appliesBad == 0, "applies(): only kColorDepth (4), only a PS writing colour 0, only an enabled non-ALWAYS test");
  check(!applies(0xAA00000Cu, 5u, 1u) && applies(0xAA00000Cu, 4u, 1u),
        "applies(): the grate's test in kDepthOnly (5) does not apply (Xenia binds no PS there), in kColorDepth it does");
  check(applies(0xAA00000Cu, 4u, 1u | 2u) && !applies(0xAA00000Cu, 4u, 2u),
        "applies(): MRT - colour 0 decides (a PS writing only colour 1 is not tested)");
  check(noModeBad > 0, "mutant: applies() ignoring edram_mode (the 0005 wiring) is caught");
  check(noC0Bad > 0, "mutant: applies() ignoring colour 0 is caught");
  check(use(false, false) == Use::kNone && use(false, true) == Use::kNone && use(true, true) == Use::kVariant &&
            use(true, false) == Use::kDrop,
        "use(): no test -> none; a variant (own or runtime-made) -> the variant; none at all -> drop");

  const char* env = std::getenv("SR_XLAT_ALPHA_TEST");
#if SR_ORACLE
  const bool expect = !env || env[0] != '0';
  check(enabled() == expect, expect ? "dev default ON (baked)" : "dev SR_XLAT_ALPHA_TEST=0 keeps the old picture");
#else
  check(enabled(), env ? "release ignores the gate" : "release constant ON (baked)");
#endif
  std::printf("[ALPHA_TEST_TEST] RESULT checks=%d passed=%d failed=%d\n", passed + failed, passed, failed);
  return failed ? 1 : 0;
}
