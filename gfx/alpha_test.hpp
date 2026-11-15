#pragma once
// alpha_test.hpp - the Xenos ALPHA TEST for translated pixel shaders (step-4 lane C, 2026-10-20; the Warworld arena
// floor: the grate PS F6E8327B draws with RB_COLORCONTROL alpha test GREATER 0.502, and without the test its slots
// covered the glowing lava layer under it - archive/step4_claude_20261018/render/NOTES.md).
//
// The console's output merger compares oC0.a with RB_ALPHA_REF under RB_COLORCONTROL.alpha_func when
// RB_COLORCONTROL.alpha_test_enable is set; D3D12 has no fixed-function alpha test, so Xenia does it at the end of
// every pixel shader that writes colour 0 (DxbcShaderTranslator::CompletePixelShader, dxbc_shader_translator_om.cc;
// the function from the system flags, the reference from alpha_test_reference). XenosRecomp's own test is a
// spec-constant clip that our DXIL build compiles out, so until this gate no translated draw was ever alpha tested.
//
// tools/xenosrecomp/0005 translates every colour-0 pixel shader a second time with Xenia's test at its returns:
// <hash>.ps.at.dxil (gfx/shader_cache.hpp CachedShader::dxilAlphaTest). The plain translation is unchanged, so a draw
// without the test keeps its pipeline. SR_XLAT_ALPHA_TEST (baked ON 2026-10-20 after the r7set replay matrix: off
// 11/11 identical, on the arena trace 3840 frame 16 vs Xenia 9.86 -> 2.39; =0 keeps the old picture for an A/B in the
// dev build, the release has no such arm): a draw whose RB_COLORCONTROL enables the test (with a function other than
// ALWAYS) draws with the variant and passes the test in its shared constants - c17.x = RB_ALPHA_REF (the float
// bits), c17.y = word() below.
//
// Round 4 (Astra on 0005, four P2): the test acts only in kColorDepth (applies()); a draw the test applies to whose
// PS has no variant first gets one from the runtime translator at the PS's IM_LOAD and is DROPPED only if that fails
// too (use()); the epilogue compares in integer arithmetic (tools/xenosrecomp/0006: DXC marks float compares `fast`,
// which may assume no NaN - passesBits() is its transcription); and the XDK-clear bookkeeping no longer calls a draw's
// depth fully written when its test can reject pixels (d3d12_frame_render.cpp nbClearCoverage).
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "../kernel/sr_oracle.hpp"

namespace sr {
namespace gfx {
namespace alphatest {

inline bool enabled() {
#if SR_ORACLE
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_ALPHA_TEST");
    return !e || e[0] != '0';
  }();
  return on;
#else
  return true;
#endif
}

// RB_COLORCONTROL -> the variant's g_AlphaTest word (c17.y): 8 | alpha_func (bits 0-2) when alpha_test_enable
// (bit 3) is set and the function is not ALWAYS (7: Xenia skips the test, so NaN passes); 0 = no test. NEVER (0)
// keeps the word 8: every pixel is discarded, as Xenia's empty mask does.
constexpr uint32_t word(uint32_t colorControl) {
  return ((colorControl & 8u) != 0u && (colorControl & 7u) != 7u) ? (8u | (colorControl & 7u)) : 0u;
}

// RB_MODECONTROL.edram_mode kColorDepth.
constexpr uint32_t kModeColorDepth = 4u;

// Does the test act on this draw? Only in kColorDepth: Xenia binds NO pixel shader in kDepth (5) - so neither the PS
// nor its alpha test runs there (d3d12_command_processor.cc 2541-2552, draw_util.cc
// IsPixelShaderNeededWithRasterization) - and only for a PS that writes colour 0 (CompletePixelShader tests oC0.a).
// Colour writes masked off do not stop it: in kColorDepth Xenia keeps the PS whenever colour 0 is written and the test
// is on (its effect on depth / stencil and sample counting).
constexpr bool applies(uint32_t colorControl, uint32_t modeControl, uint32_t psColorOutMask) {
  return word(colorControl) != 0u && (modeControl & 7u) == kModeColorDepth && (psColorOutMask & 1u) != 0u;
}

// What a draw does about the test. kNone: it does not apply. kVariant: draw with the alpha-test variant - the PS's
// own <hash>.ps.at.dxil, or one the runtime translator made at the PS's IM_LOAD (and persisted). kDrop: it applies
// and no variant exists after the runtime translator tried - the draw is dropped ([XLAT_ALPHA] Error, a
// [SHADER_DROP] census line) rather than drawn without a test that may reject every pixel (NEVER) or the parts a
// cut-out hides. A whole-game run must count 0 of them.
enum class Use { kNone, kVariant, kDrop };
constexpr Use use(bool appliesToDraw, bool haveVariant) {
  return !appliesToDraw ? Use::kNone : haveVariant ? Use::kVariant : Use::kDrop;
}

// A DXBC float compare flushes a denormal input to a zero of its sign (the D3D10+ float32 rules: denorms are flushed
// on input to every floating-point operation) - Xenia's epilogue is DXBC, so its test sees a denormal as 0.
inline float ftz(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? std::copysign(0.0f, v) : v; }

// The reference semantics, for the unit test: does a pixel with alpha `a` pass the test `w` against `ref`? (Xenia:
// NOT_EQUAL is "not equal" - true for NaN; LESS / EQUAL / GREATER a mask; exact compares, use_fuzzy_alpha_epsilon
// false; denormal inputs flushed.)
inline bool passes(uint32_t w, float a0, float ref0) {
  if (!(w & 8u) || (w & 7u) == 7u) return true;
  const float a = ftz(a0), ref = ftz(ref0);
  const uint32_t m = w & 7u;
  if (m == 5u) return a != ref;
  return ((m & 1u) && a < ref) || ((m & 2u) && a == ref) || ((m & 4u) && a > ref);
}

// The 0006 epilogue, transcribed line for line (tools/xenosrecomp/0006-step4-alpha-test-strict-compare.patch): the
// same test in integer arithmetic on the float bits, so no float compare is left for fast-math to bend. NaN (either
// side) is unordered; a denormal magnitude is 0; the rest compare as sign-magnitude keys (-0 == +0, +-inf ordered).
inline bool passesBits(uint32_t w, uint32_t a, uint32_t r) {
  if (!(w & 8u) || (w & 7u) == 7u) return true;
  const uint32_t mask = w & 7u;
  const uint32_t am = a & 0x7FFFFFFFu, rm = r & 0x7FFFFFFFu;
  const bool unord = am > 0x7F800000u || rm > 0x7F800000u;
  int32_t ka = int32_t(am < 0x00800000u ? 0u : am);
  int32_t kr = int32_t(rm < 0x00800000u ? 0u : rm);
  ka = (a & 0x80000000u) != 0u ? -ka : ka;
  kr = (r & 0x80000000u) != 0u ? -kr : kr;
  const bool eq = !unord && ka == kr;
  if (mask == 5u) return !eq;
  return ((mask & 1u) != 0u && !unord && ka < kr) || ((mask & 2u) != 0u && eq) ||
         ((mask & 4u) != 0u && !unord && ka > kr);
}

}  // namespace alphatest
}  // namespace gfx
}  // namespace sr
