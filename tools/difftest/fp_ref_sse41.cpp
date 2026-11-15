// iter-463: the REFERENCE side of --fp-platform-test. This TU alone is compiled with
// -msse4.1 (CMakeLists.txt set_source_files_properties), so _mm_round_ps is the real
// ROUNDPS instruction: the semantics of PPC vrfin/vrfim/vrfiz (Xenia lowers them to
// vroundps). The guest lib and every other difftest TU are built WITHOUT -msse4.1, where
// simde_mm_round_ps falls back to libm / guest/math_helpers.cpp.
#if !defined(__SSE4_1__)
#error "fp_ref_sse41.cpp must be compiled with -msse4.1 (see CMakeLists.txt)"
#endif
#include <smmintrin.h>

void srRefRoundPs(const float* in, float* out, int mode) {
  __m128 v = _mm_loadu_ps(in);
  __m128 r;
  switch (mode) {
    case 0: r = _mm_round_ps(v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); break;
    case 1: r = _mm_round_ps(v, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); break;
    case 2: r = _mm_round_ps(v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC); break;
    default: r = _mm_round_ps(v, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC); break;
  }
  _mm_storeu_ps(out, r);
}

bool srRefBuiltWithSse41() { return true; }
