// Shared helpers for NATIVE transcriptions of recompiled guest bodies (iter-467 guestcode):
// guest memory accessors exactly as the recompiled ones (base + aliasGuestVA(va)), VMX register
// images (host lane i = PPC element 3-i) and every VMX op lowered to the SAME simde expression
// the recompiler emits. Include only from TUs compiled at the guest lib's ISA level (-mssse3, no
// SSE4.1 / FMA: CMakeLists.txt) so every op lowers exactly as in the guest lib. Everything is in
// an unnamed namespace: each TU owns its copy.
#pragma once

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "sr_oracle.hpp"  // C1: AbTimer (the *_AB timing oracle) is compiled out under SR_ORACLE=0

#include <atomic>
#include <cstring>
#include <intrin.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sr::native::vmx {
namespace {

using V = simde__m128;
using VI = simde__m128i;

// ---- guest memory, exactly as the recompiled accessors (base + aliasGuestVA(va)) ----------
inline uint8_t* gp(uint8_t* b, uint32_t va) { return b + sr::kernel::aliasGuestVA(va); }
inline uint32_t lwz(uint8_t* b, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, gp(b, va), 4);
  return _byteswap_ulong(v);
}
inline void stw(uint8_t* b, uint32_t va, uint32_t x) {
  x = _byteswap_ulong(x);
  std::memcpy(gp(b, va), &x, 4);
}
inline uint8_t lbz(uint8_t* b, uint32_t va) { return *gp(b, va); }
inline void stb(uint8_t* b, uint32_t va, uint8_t x) { *gp(b, va) = x; }
inline uint64_t ld(uint8_t* b, uint32_t va) {
  uint64_t v;
  std::memcpy(&v, gp(b, va), 8);
  return v;  // raw bytes: only ever stored back with sd
}
inline void sd(uint8_t* b, uint32_t va, uint64_t raw) { std::memcpy(gp(b, va), &raw, 8); }
inline VI maskL() { return simde_mm_load_si128((const VI*)VectorMaskL); }
inline V lvx(uint8_t* b, uint32_t va) {
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(
      simde_mm_load_si128((const VI*)(b + sr::kernel::aliasGuestVA(va & ~0xFu))), maskL()));
}
inline void stvx(uint8_t* b, uint32_t va, V v) {
  simde_mm_store_si128((VI*)(b + sr::kernel::aliasGuestVA(va & ~0xFu)),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(v), maskL()));
}

// ---- register images ---------------------------------------------------------------------
// A VMX register is held in the recompiler's image: the 16 guest bytes reversed, so host lane
// i = PPC element 3-i.
inline uint32_t elem(V v, int e) {
  alignas(16) uint32_t t[4];
  simde_mm_store_ps((float*)t, v);
  return t[3 - e];
}
// image of the 16 guest bytes {e0, e1, e2, e3} (PPC element order, host-endian values)
inline V image(uint32_t e0, uint32_t e1, uint32_t e2, uint32_t e3) {
  return simde_mm_castsi128_ps(simde_mm_set_epi32(int(e0), int(e1), int(e2), int(e3)));
}
inline V lvsl(uint32_t r) {
  return simde_mm_castsi128_ps(simde_mm_load_si128((const VI*)&VectorShiftTableL[(r & 0xF) * 16]));
}
inline V spltw0(V v) { return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v), 0xFF)); }
inline V spltw1(V v) { return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v), 0xAA)); }
inline V spltw2(V v) { return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v), 0x55)); }
inline V spltw3(V v) { return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v), 0x00)); }
inline V vadd(V a, V b) { return simde_mm_add_ps(a, b); }
inline V vsub(V a, V b) { return simde_mm_sub_ps(a, b); }
inline V vmul(V a, V b) { return simde_mm_mul_ps(a, b); }
inline V vmadd(V a, V c, V b) { return simde_mm_add_ps(simde_mm_mul_ps(a, c), b); }   // vmaddfp vD,vA,vC,vB
inline V vnmsub(V a, V c, V b) {                                                       // vnmsubfp vD,vA,vC,vB
  return simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(a, c), b),
                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))));
}
inline V vmin(V a, V b) { return simde_mm_min_ps(a, b); }
inline V vmax(V a, V b) { return simde_mm_max_ps(a, b); }
inline V dp3(V a, V b) { return simde_mm_dp_ps(a, b, 0xEF); }   // vmsum3fp128
inline V dp4(V a, V b) { return simde_mm_dp_ps(a, b, 0xFF); }   // vmsum4fp128
inline V vperm(V a, V b, V c) {
  return simde_mm_castsi128_ps(simde_mm_perm_epi8_(simde_mm_castps_si128(a), simde_mm_castps_si128(b),
                                                   simde_mm_castps_si128(c)));
}
inline V rlimi_1_1(V d, V b) { return simde_mm_blend_ps(d, simde_mm_permute_ps(b, 147), 1); }
inline V rlimi_2_0(V d, V b) { return simde_mm_blend_ps(d, simde_mm_permute_ps(b, 228), 2); }
inline V rlimi_1_0(V d, V b) { return simde_mm_blend_ps(d, simde_mm_permute_ps(b, 228), 1); }
inline V vcfsx0(V b) { return simde_mm_cvtepi32_ps(simde_mm_castps_si128(b)); }
inline V vcfsx1(V b) {
  return simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_castps_si128(b)),
                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x3F000000))));
}
inline V vrefp(V b) { return simde_mm_div_ps(simde_mm_set1_ps(1), b); }
inline V vrsqrte(V b) { return simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(b)); }
inline V vsel(V a, V b, V c) {  // vsel vD,vA,vB,vC
  return simde_mm_castsi128_ps(simde_mm_or_si128(
      simde_mm_andnot_si128(simde_mm_castps_si128(c), simde_mm_castps_si128(a)),
      simde_mm_and_si128(simde_mm_castps_si128(c), simde_mm_castps_si128(b))));
}
// vcmpgtfp. -> cr6 (setFromMask(mask, 0xF)): lt = all lanes, eq = no lane
inline int gtMask(V a, V b) { return simde_mm_movemask_ps(simde_mm_cmpgt_ps(a, b)); }

// ---- exact scalar FPU helpers (mode independent) -----------------------------------------
// lfs then stfs of the same value in the guest's IEEE mode: a bit copy, except that the
// double round trip quiets a signalling NaN.
inline uint32_t fcopy(uint32_t b) {
  return ((b & 0x7F800000u) == 0x7F800000u && (b & 0x003FFFFFu) && !(b & 0x00400000u)) ? (b | 0x00400000u) : b;
}
inline bool fnan(uint32_t b) { return (b & 0x7FFFFFFFu) > 0x7F800000u; }
inline int32_t fkey(uint32_t b) {
  const int32_t m = int32_t(b & 0x7FFFFFFFu);
  return (b & 0x80000000u) ? -m : m;  // -0 and +0 both 0
}
// fcmpu of two lfs-loaded values: exact (lfs widening is exact in IEEE mode); NaN = unordered,
// i.e. not less (blt not taken, bge taken)
inline bool fLT(uint32_t a, uint32_t b) { return !fnan(a) && !fnan(b) && fkey(a) < fkey(b); }


// the float product of two float bit patterns via the exact double product (the guest's fmuls).
// noinline: an opaque call orders it against the caller's MXCSR switches.
__declspec(noinline) uint32_t fmulsBits(uint32_t a, uint32_t b) {
  float fa, fb;
  std::memcpy(&fa, &a, 4);
  std::memcpy(&fb, &b, 4);
  const float r = float(double(fa) * double(fb));
  uint32_t u;
  std::memcpy(&u, &r, 4);
  return u;
}
// fmuls in the guest's IEEE (non-flush) mode, from a caller running in VMX flush mode
// (FTZ+DAZ): the double product of two floats is exact, so only a denormal input (DAZ would zero
// it on the widen) or a result near the float underflow threshold (FTZ) needs the IEEE MXCSR.
inline uint32_t fmulsIEEE(uint32_t a, uint32_t b) {
  auto den = [](uint32_t x) { return (x & 0x7F800000u) == 0 && (x & 0x007FFFFFu) != 0; };
  if (!den(a) && !den(b)) {
    float fa, fb;
    std::memcpy(&fa, &a, 4);
    std::memcpy(&fb, &b, 4);
    const double p = double(fa) * double(fb);
    const double m = p < 0 ? -p : p;
    if (m == 0 || m >= 0x1p-125 || m != m) {
      const float r = float(p);
      uint32_t u;
      std::memcpy(&u, &r, 4);
      return u;
    }
  }
  const unsigned csr = _mm_getcsr();
  _mm_setcsr(csr & ~0x8040u);
  const uint32_t u = fmulsBits(a, b);
  _mm_setcsr(csr);
  return u;
}
inline bool fGT(uint32_t a, uint32_t b) { return fLT(b, a); }

#if SR_ORACLE
// In-process A/B timing (the *_AB gates): calls alternate between implementations in blocks,
// each call's TSC span is summed per mode, and report() prints the mean us per call. One roll
// gives the per-call cost of the guest body and the native one under the same frames, without
// the roll-to-roll noise of an fps A/B.
struct AbTimer {
  std::atomic<uint64_t> ticks[3]{}, calls[3]{};
  uint64_t tsc0 = 0;
  long long qpc0 = 0;
  void add(int mode, uint64_t dt) {
    ticks[mode].fetch_add(dt, std::memory_order_relaxed);
    calls[mode].fetch_add(1, std::memory_order_relaxed);
  }
  // mean microseconds per call of each mode (TSC calibrated against QPC since the first call)
  void means(double out[3]) {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    const uint64_t t = __rdtsc();
    if (!tsc0) {
      tsc0 = t;
      qpc0 = q.QuadPart;
    }
    const double secs = double(q.QuadPart - qpc0) / double(f.QuadPart);
    const double hz = secs > 0.5 ? double(t - tsc0) / secs : 3.0e9;
    for (int m = 0; m < 3; ++m) {
      const uint64_t c = calls[m].load(std::memory_order_relaxed);
      out[m] = c ? double(ticks[m].load(std::memory_order_relaxed)) / double(c) / hz * 1e6 : 0.0;
    }
  }
};
#endif  // SR_ORACLE

}  // namespace
}  // namespace sr::native::vmx
