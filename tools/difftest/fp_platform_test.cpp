// iter-463 PLATFORM FIXES (camera tilt investigation): host FP state the guest relies on.
//
//   --fp-platform-test          R*  guest/math_helpers.cpp roundevenf (vrfin's libm route) vs the
//                                   real ROUNDPS (fp_ref_sse41.cpp, the only TU built -msse4.1)
//                               S*  the WHOLE simde_mm_round_ps lowering (vrfin / vrfim / vrfiz,
//                                   exactly as the guest emits it, in a non-SSE4.1 TU) vs ROUNDPS,
//                                   swept over 1/251 of all float bit patterns, with MXCSR in the
//                                   default state and in the VMX flush state (FTZ|DAZ)
//                               M1  mutation witness: the same checker, run on a verbatim copy of
//                                   the pre-fix helper, must find the tie bug
//                               L*  the other libm symbols the guest objects import (floorf,
//                                   truncf, exp2f, log2f, sqrtf, sqrt): special values vs ROUNDPS /
//                                   the Power ISA (L-checks on exp2f/log2f/sqrt are INFO lines,
//                                   not gates: a different class, see the printed notes)
//                               A*  APC delivery restores the host MXCSR (guest/ppc_import_glue.cpp
//                                   callGuestAPCSafe) through the REAL queue + delivery path
//   --fp-roundeven-exhaustive   all 2^32 float patterns, helper vs ROUNDPS (~2.5 min)
//
// Mutation checks: restoring the old roundevenf fails R1 R3 R5 R7 R9 R10 R38 R13 S1 S1f (14 of 32 with A1..A4); removing the
// setcsr after `ctx = saved` fails A1..A4 (both recorded in the iter-463 commits).
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"

#include <cfloat>
#include <xmmintrin.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

void srRefRoundPs(const float* in, float* out, int mode);   // fp_ref_sse41.cpp (-msse4.1)
bool srRefBuiltWithSse41();
void srTestQueueGuestAPC(uint32_t threadHandle, uint32_t routine, uint32_t arg1,
                         uint32_t arg2, uint32_t arg3);      // guest/ppc_import_glue.cpp
void srTestDeliverGuestAPCs(PPCContext& ctx, uint32_t threadHandle);
extern "C" float roundevenf(float x);                        // guest/math_helpers.cpp

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

// Called through a volatile pointer so clang cannot constant-fold or pattern-match it.
float (*volatile g_helper)(float) = roundevenf;

// The PRE-FIX helper, verbatim (git show 4814d5c:guest/math_helpers.cpp): the M1 witness.
float oldRoundevenf(float x) {
  if (std::isnan(x) || std::isinf(x)) return x;
  float r = std::roundf(x);
  float d = x - r;
  if (std::abs(d) == 0.5f) {
    if (std::fmod(r, 2.0f) != 0.0f)
      r = (d > 0) ? r - 1.0f : r + 1.0f;
  }
  return r;
}
float (*volatile g_oldHelper)(float) = oldRoundevenf;

uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float fromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// Exactly the guest's vrfin / vrfim / vrfiz lowering (recompiler.cpp PPC_INST_VRFI*): this TU,
// like the guest lib, has no -msse4.1, so simde falls back per lane to libm / roundevenf.
void guestRoundPs(const float* in, float* out, int mode) {
  alignas(16) float a[4], r[4];
  std::memcpy(a, in, 16);
  simde__m128 v = simde_mm_load_ps(a);
  simde__m128 x;
  switch (mode) {
    case 0: x = simde_mm_round_ps(v, SIMDE_MM_FROUND_TO_NEAREST_INT | SIMDE_MM_FROUND_NO_EXC); break;
    case 1: x = simde_mm_round_ps(v, SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC); break;
    default: x = simde_mm_round_ps(v, SIMDE_MM_FROUND_TO_ZERO | SIMDE_MM_FROUND_NO_EXC); break;
  }
  simde_mm_store_ps(r, x);
  std::memcpy(out, r, 16);
}

const float kTargets[] = {
    0.5f, 1.5f, 2.5f, 3.5f, -0.5f, -1.5f, -2.5f, -3.5f, 1000000.5f, -1000000.5f, 8388607.5f,
    -8388607.5f, 0.25f, 0.75f, -0.75f, 0.49999997f, 0.50000006f, 1.4999999f, 2.5000002f,
    -0.49999997f, 1.0f, 2.0f, -3.0f, 0.0f, -0.0f, 1e-45f, -1e-45f, FLT_MIN, -FLT_MIN,
    8388608.0f, 16777216.0f, 1e30f, -1e30f, FLT_MAX, -FLT_MAX, INFINITY, -INFINITY,
    3.14159274f / 6.28318548f /* the sin/cos(pi) tie: 0.5 */};
const uint32_t kNanBits[] = {0x7FC00000u, 0xFFC00000u, 0x7F800001u /*SNaN*/, 0xFF800123u /*-SNaN*/,
                             0x7FC01234u};

int mismatchesVsRef(float (*fn)(float), const float* xs, int n, int* firstIdx) {
  int bad = 0;
  for (int i = 0; i < n; i += 4) {
    float in[4] = {0, 0, 0, 0}, ref[4];
    for (int k = 0; k < 4 && i + k < n; ++k) in[k] = xs[i + k];
    srRefRoundPs(in, ref, 0);
    for (int k = 0; k < 4 && i + k < n; ++k)
      if (bits(fn(in[k])) != bits(ref[k])) { if (!bad && firstIdx) *firstIdx = i + k; ++bad; }
  }
  return bad;
}

// Strided sweep of the float bit space: guest lowering (mode) vs ROUNDPS.
uint64_t sweep(int mode, uint32_t stride, uint32_t* firstBad, bool helperOnly) {
  uint64_t bad = 0;
  alignas(16) float in[4], ref[4], got[4];
  int k = 0;
  uint64_t u = 0;
  for (; u <= 0xFFFFFFFFull; u += stride) {
    in[k++] = fromBits((uint32_t)u);
    if (k == 4) {
      srRefRoundPs(in, ref, mode);
      if (helperOnly) for (int j = 0; j < 4; ++j) got[j] = g_helper(in[j]);
      else guestRoundPs(in, got, mode);
      for (int j = 0; j < 4; ++j)
        if (bits(got[j]) != bits(ref[j])) { if (!bad && firstBad) *firstBad = bits(in[j]); ++bad; }
      k = 0;
    }
  }
  return bad;
}

// ---------------------------------------------------------------- APC MXCSR restore --------
constexpr uint32_t kApcVa = (uint32_t)PPC_CODE_BASE + 0x10;
constexpr uint32_t kApcHandle = 0x00F0A5C1u;
constexpr uint32_t kFlush = 0x8040;   // MXCSR FTZ | DAZ

void apcSetFlush(PPCContext& ctx, uint8_t*) { ctx.fpscr.enableFlushMode(); }
void apcClearFlush(PPCContext& ctx, uint8_t*) { ctx.fpscr.disableFlushMode(); }
void apcRoundTowardZero(PPCContext& ctx, uint8_t*) { ctx.fpscr.storeFromGuest(1); }   // mtfsf RN=01
void apcFlushThenFault(PPCContext& ctx, uint8_t*) {
  ctx.fpscr.enableFlushMode();
  RaiseException(0xE0463001u, 0, 0, nullptr);   // -> callGuestAPCSafe's __except path
}

// Run one APC on a guest context whose cached csr == live MXCSR == `start`; return the live
// MXCSR after delivery (with the exception-mask bits the harness always forces).
uint32_t deliverOne(PPCFunc* routine, uint32_t start, uint32_t* cachedAfter) {
  PPCFunc** table = sr::kernel::g_dispatchTable;
  table[(kApcVa - PPC_CODE_BASE) >> 2] = routine;
  PPCContext ctx;
  std::memset(&ctx, 0, sizeof ctx);
  ctx.fpscr.csr = start;
  ctx.fpscr.setcsr(start);
  srTestQueueGuestAPC(kApcHandle, kApcVa, 1, 2, 3);
  srTestDeliverGuestAPCs(ctx, kApcHandle);
  uint32_t live = _mm_getcsr();
  if (cachedAfter) *cachedAfter = ctx.fpscr.csr;
  _mm_setcsr(0x1F80);
  table[(kApcVa - PPC_CODE_BASE) >> 2] = nullptr;
  return live;
}

void apcTests() {
  std::printf("[A] APC delivery restores the host MXCSR (callGuestAPCSafe)\n");
  std::vector<PPCFunc*> own;
  PPCFunc** saved = sr::kernel::g_dispatchTable;
  if (!saved) { own.assign(sr::kernel::kDispatchTableEntryCount, nullptr); sr::kernel::g_dispatchTable = own.data(); }
  const uint32_t kBase = 0x1F80, kRtz = 0x1F80 | 0x6000;
  uint32_t cached = 0, live;
  char buf[160];

  live = deliverOne(apcSetFlush, kBase, &cached);
  std::snprintf(buf, sizeof buf, "A1 APC sets FTZ|DAZ on a non-flush thread: MXCSR after = %04X (want %04X, cached %04X)", live, kBase, cached);
  check(live == kBase && (cached | 0x1F80) == live, buf);

  live = deliverOne(apcClearFlush, kBase | kFlush, &cached);
  std::snprintf(buf, sizeof buf, "A2 APC clears FTZ|DAZ on a flush thread:  MXCSR after = %04X (want %04X, cached %04X)", live, kBase | kFlush, cached);
  check(live == (kBase | kFlush) && (cached | 0x1F80) == live, buf);

  live = deliverOne(apcRoundTowardZero, kBase, &cached);
  std::snprintf(buf, sizeof buf, "A3 APC sets round-toward-zero (mtfsf):     MXCSR after = %04X (want %04X, APC had %04X)", live, kBase, kRtz);
  check(live == kBase && (cached | 0x1F80) == live, buf);

  live = deliverOne(apcFlushThenFault, kBase, &cached);
  std::snprintf(buf, sizeof buf, "A4 APC sets FTZ|DAZ then faults (__except): MXCSR after = %04X (want %04X, cached %04X)", live, kBase, cached);
  check(live == kBase && (cached | 0x1F80) == live, buf);

  if (!saved) sr::kernel::g_dispatchTable = nullptr;
}

void libmNotes() {
  std::printf("[L] other libm routes of the guest objects (llvm-nm -u: sqrtf sqrt floorf truncf exp2f log2f roundevenf)\n");
  // floorf / truncf: vrfim / vrfiz via simde (swept above as S2/S3). exp2f / log2f: vexptefp /
  // vlogefp (recompiler emits the libm call per lane). The PPC ops are ESTIMATES (1/2^12
  // relative) and Xenia also uses the host's exact exp2 / log2, so precision is not the bug
  // class here; the checks below are the special values the ISA defines.
  volatile float ninf = -INFINITY, pinf = INFINITY, qnan = NAN, zero = 0.0f, nzero = -0.0f,
                 neg = -1.0f, den = 1e-45f;
  check(exp2f(ninf) == 0.0f && !std::signbit(exp2f(ninf)), "L1 exp2f(-inf) = +0 (vexptefp ISA)");
  check(std::isinf(exp2f(pinf)) && exp2f(pinf) > 0, "L2 exp2f(+inf) = +inf");
  check(std::isnan(exp2f(qnan)), "L3 exp2f(NaN) = NaN");
  check(std::isinf(log2f(zero)) && log2f(zero) < 0, "L4 log2f(+0) = -inf (vlogefp ISA)");
  check(std::isinf(log2f(nzero)) && log2f(nzero) < 0, "L5 log2f(-0) = -inf");
  check(std::isnan(log2f(neg)), "L6 log2f(-1) = NaN");
  check(std::isinf(log2f(pinf)) && log2f(pinf) > 0, "L7 log2f(+inf) = +inf");
  // INFO (not gated): VMX non-Java mode treats a denormal INPUT as zero, so vlogefp(denormal)
  // is -inf on hardware; libm computes it exactly. The guest runs vlogefp under
  // printSetFlushMode(true), i.e. with MXCSR DAZ, which a software libm may ignore.
  _mm_setcsr(0x1F80 | kFlush);
  float l = log2f(den), e = exp2f(den);
  _mm_setcsr(0x1F80);
  std::printf("  info log2f(1e-45) under DAZ = %g (ISA non-Java: -inf); exp2f(1e-45) = %g (ISA: 1)\n", l, e);
  volatile double dneg = -1.0;
  volatile float fneg = -4.0f;
  std::printf("  info sqrt(-1) bits = %016llX, sqrtf(-4) bits = %08X (PPC default NaN is positive "
              "7FF8.../7FC0...; x86 gives the negative default NaN - Xenia's host has the same)\n",
              (unsigned long long)[](double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }(std::sqrt(dneg)),
              bits(std::sqrt(fneg)));
}
}  // namespace

int runFpPlatformTest() {
  std::printf("[FP_PLATFORM_TEST] reference TU built -msse4.1: %s\n", srRefBuiltWithSse41() ? "yes" : "no");
  _mm_setcsr(0x1F80);
  char buf[200];

  // R: targeted values, helper vs ROUNDPS, bit-exact.
  std::printf("[R] guest/math_helpers.cpp roundevenf vs ROUNDPS(TO_NEAREST_INT)\n");
  const int nT = sizeof kTargets / sizeof kTargets[0];
  for (int i = 0; i < nT; ++i) {
    float in[4] = {kTargets[i], 0, 0, 0}, ref[4];
    srRefRoundPs(in, ref, 0);
    float got = g_helper(kTargets[i]);
    const bool tie = i < 11;
    if (tie || i == nT - 1) {
      std::snprintf(buf, sizeof buf, "R%d roundevenf(%.9g) = %.9g [%08X], ROUNDPS = %.9g [%08X]", i + 1,
                    kTargets[i], got, bits(got), ref[0], bits(ref[0]));
      check(bits(got) == bits(ref[0]), buf);
    }
  }
  int firstBad = -1;
  int badNonTie = mismatchesVsRef(g_helper, kTargets + 11, nT - 12, &firstBad);
  std::snprintf(buf, sizeof buf, "R12 non-ties, +-0, denormals, FLT_MIN/MAX, 2^23, 2^24, 1e30, +-inf: %d mismatches", badNonTie);
  check(badNonTie == 0, buf);
  int badNan = 0;
  for (uint32_t nb : kNanBits) {
    float in[4] = {fromBits(nb), 0, 0, 0}, ref[4];
    srRefRoundPs(in, ref, 0);
    if (bits(g_helper(in[0])) != bits(ref[0])) {
      std::printf("  nan %08X: helper %08X ROUNDPS %08X\n", nb, bits(g_helper(in[0])), bits(ref[0]));
      ++badNan;
    }
  }
  std::snprintf(buf, sizeof buf, "R13 NaNs (QNaN, -QNaN, SNaN, -SNaN, payload) quieted bit-exact: %d mismatches", badNan);
  check(badNan == 0, buf);

  // M1: the checker must catch the pre-fix helper.
  int oldBad = mismatchesVsRef(g_oldHelper, kTargets, nT, nullptr);
  std::snprintf(buf, sizeof buf, "M1 mutation witness: pre-fix helper fails %d of %d targeted values (must be > 0)", oldBad, nT);
  check(oldBad > 0, buf);
  {
    float in[4] = {0.5f, 2.5f, -0.5f, 1000000.5f}, ref[4];
    srRefRoundPs(in, ref, 0);
    std::printf("  old helper: 0.5->%g 2.5->%g -0.5->%g 1000000.5->%.1f | ROUNDPS: %g %g %g %.1f\n",
                g_oldHelper(0.5f), g_oldHelper(2.5f), g_oldHelper(-0.5f), g_oldHelper(1000000.5f),
                ref[0], ref[1], ref[2], ref[3]);
  }

  // S: the whole simde lowering, swept, in both MXCSR states the guest uses.
  std::printf("[S] simde_mm_round_ps (non-SSE4.1 guest lowering) vs ROUNDPS, stride 251 (~17.1M patterns)\n");
  const char* modeName[3] = {"vrfin TO_NEAREST_INT (roundevenf)", "vrfim TO_NEG_INF (floorf)", "vrfiz TO_ZERO (truncf)"};
  for (int flush = 0; flush < 2; ++flush) {
    _mm_setcsr(0x1F80 | (flush ? kFlush : 0));
    for (int m = 0; m < 3; ++m) {
      uint32_t fb = 0;
      uint64_t bad = sweep(m, 251, &fb, false);
      _mm_setcsr(0x1F80 | (flush ? kFlush : 0));
      int len = std::snprintf(buf, sizeof buf, "S%d%s %s, MXCSR %s: %llu mismatches", m + 1, flush ? "f" : "",
                              modeName[m], flush ? "FTZ|DAZ" : "default", (unsigned long long)bad);
      if (bad) std::snprintf(buf + len, sizeof buf - len, ", first input bits %08X", fb);
      check(bad == 0, buf);
    }
  }
  _mm_setcsr(0x1F80);

  libmNotes();
  apcTests();

  std::printf("[FP_PLATFORM_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

int runFpRoundevenExhaustive() {
  _mm_setcsr(0x1F80);
  uint32_t fb = 0;
  uint64_t bad = sweep(0, 1, &fb, true);
  std::printf("[FP_ROUNDEVEN_EXHAUSTIVE] 4294967296 patterns, roundevenf vs ROUNDPS: %llu mismatches (first bad input %08X)\n",
              (unsigned long long)bad, bad ? fb : 0u);
  _mm_setcsr(0x1F80 | kFlush);
  uint64_t badF = sweep(0, 1, &fb, false);
  _mm_setcsr(0x1F80);
  std::printf("[FP_ROUNDEVEN_EXHAUSTIVE] 4294967296 patterns, guest vrfin lowering under FTZ|DAZ vs ROUNDPS: %llu mismatches (first bad input %08X)\n",
              (unsigned long long)badF, badF ? fb : 0u);
  return (bad || badF) ? 1 : 0;
}
