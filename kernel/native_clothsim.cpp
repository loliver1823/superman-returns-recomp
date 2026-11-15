// NATIVE cloth simulation (iter-467 guestcode round 4, after the cloth light mask): the cloth
// solver's hot leaves as host code. 0.74 ms of the 8.85 ms player-profile render tick is the
// cloth simulation (docs/PERFORMANCE.md 2.11).
//
// sub_822C0360 Cloth_SatisfyDistanceConstraints: one Jakobsen relaxation pass over the cloth's
// distance constraints (r3 = cloth; count +0x3CB0, 16-byte list {i0, i1, restLen, -} at +0x3CC0,
// particle positions at +0x3C8C, pinned bytes at +0x3CB8, rest-length scale +0x3D60).
// Sequential by nature (each constraint reads the positions the previous ones moved), so this is
// a single-thread transcription: every guest memory access outside the frame at the same program
// point, every VMX op the same simde expression the recompiler emits (native_vmx.hpp), the
// scalar fmuls in IEEE semantics (fmulsIEEE), the two fcmpu clamps as exact compares.
//
// GATES. SR_NATIVE_CLOTHSIM (default ON since iter-467; =0 runs the guest bodies): run natively.
// SR_NATIVE_CLOTHSIM_CHECK=1: per call, snapshot the particle positions the constraints index,
// run the guest original, keep its result, restore, run the native version, compare every byte,
// put the guest's result back; counts every call and logs mismatches.
// C1: the CHECK and the timing A/B (SR_NATIVE_CLOTHSIM_AB) are development oracles, compiled out under SR_ORACLE=0
// (kernel/sr_oracle.hpp): there both gates read as unset (no env read). The [NATIVE_CLOTHSIM] counter line the native
// path prints stays as it is (its CHECK counters then read 0, as with the CHECK unset).

#include "native_vmx.hpp"
#include "ab_toggle.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void __imp__sub_822C0360(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822C0520(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::native::clothsim {
namespace {

using namespace sr::native::vmx;

bool clothSimOn() {  // read at the point of use (SR_AB_TOGGLE, kernel/ab_toggle.hpp)
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHSIM"); return !e || e[0] != '0'; }();
  static const int ab = sr::abt::reg("SR_NATIVE_CLOTHSIM");
  return v && (ab < 0 || sr::abt::on(ab));
}
#if SR_ORACLE
bool clothSimCheckOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHSIM_CHECK"); return e && e[0] == '1'; }();
  return v;
}

bool clothSimAbOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHSIM_AB"); return e && e[0] == '1'; }();
  return v;
}
#endif  // SR_ORACLE

std::atomic<uint64_t> g_calls{0}, g_cons{0}, g_checked{0}, g_bad{0}, g_unchk{0};
#if SR_ORACLE
AbTimer g_abS, g_abC;  // SatisfyDistanceConstraints, CollideParticle
#endif
std::atomic<uint64_t> g_ccalls{0}, g_cfall{0}, g_cchecked{0}, g_cbad{0};

constexpr uint32_t kHalf = 0x820037C8u, kClampHi = 0x8201F56Cu, kClampLo = 0x8201F570u;

// sub_822C0360. The caller's MXCSR state is left in VMX flush mode (shadow kept in step).
void satisfyDistanceConstraints(PPCContext& ctx, uint8_t* b) {
  const uint32_t r3 = ctx.r3.u32;
  uint32_t n = lwz(b, r3 + 15536);
  uint32_t r8 = lwz(b, r3 + 15552);
  ctx.fpscr.enableFlushMode();
  const V v5 = spltw0(image(fcopy(lwz(b, kHalf)), 0, 0, 0));
  if (n == 0) return;
  const V v6 = simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1));
  const uint32_t f13 = lwz(b, kClampLo), f0 = lwz(b, kClampHi);
  const V hi = spltw0(image(fcopy(f0), 0, 0, 0)), lo = spltw0(image(fcopy(f13), 0, 0, 0));
  uint64_t cons = 0;
  do {
    const uint32_t r6 = lwz(b, r8);
    const uint32_t pos = lwz(b, r3 + 15500);
    const uint32_t rest = lwz(b, r8 + 8);
    const uint32_t r31 = lwz(b, r8 + 4);
    const uint32_t scale = lwz(b, r3 + 15712);
    const uint32_t r4 = (r6 << 4) + pos;
    const uint32_t r = fmulsIEEE(rest, scale);
    const uint32_t r9 = (r31 << 4) + pos;
    const V v8 = lvx(b, r4);
    const V v12a = lvx(b, r9);
    const V v10 = vsub(v12a, v8);
    const V v13a = spltw0(image(r, 0, 0, 0));
    V v0 = simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1));
    const V v9 = vmul(v13a, v13a);
    V v13 = dp3(v10, v10);
    v13 = vadd(v13, v9);
    const V v12 = vrefp(v13);
    v0 = vnmsub(v12, v13, v0);
    v0 = vmadd(v12, v0, v12);
    const V v11 = vnmsub(v0, v13, v6);
    v0 = vmadd(v0, v11, v0);
    v0 = vmul(v0, v9);
    v0 = vsub(v0, v5);
    if (fGT(elem(v0, 0), f0)) v0 = hi;   // fcmpu + bgt: unordered = not taken
    if (fLT(elem(v0, 0), f13)) v0 = lo;  // fcmpu + blt
    v0 = vmul(v10, v0);
    if (lbz(b, lwz(b, r3 + 15544) + r6) == 0) stvx(b, r4, vsub(v8, v0));
    const uint32_t r7 = lwz(b, r8 + 4);
    if (lbz(b, lwz(b, r3 + 15544) + r7) == 0) stvx(b, r9, vadd(lvx(b, r9), v0));
    r8 += 16;
    ++cons;
  } while (--n != 0);
  g_cons.fetch_add(cons, std::memory_order_relaxed);
}

// ---- sub_822C0520 Cloth_CollideParticlePositionSphereCapsule ------------------------------
// r3 cloth, r4 -> particle position (in/out), r7 collider shape, r8 -> collider motion. Kind
// +0x40: 1 capsule (centre +0x10, axis +0x4C, length +0x48, radius +0x44), 0 sphere (radius
// +0x44), else nothing. Writes only the particle position. A one-sided plane (+0x20 set) calls
// sub_822E0E78: that case returns false BEFORE any guest write, and the guest body runs.
inline V one4() { return simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1)); }
inline V half4() { return vcfsx1(simde_mm_castsi128_ps(simde_mm_set1_epi32(1))); }
// the guest's rsqrte + two Newton steps on d (all lanes), as lowered
inline V nrRsqrt(V d) {
  V v10 = one4(), v9 = half4();
  const V v8 = one4(), v7 = half4();
  V v12 = vrsqrte(d);
  V v6 = vmul(v12, v12);
  v9 = vmul(v12, v9);
  v6 = vmul(d, v6);
  v10 = vsub(v10, v6);
  v10 = vmul(v9, v10);
  v12 = vadd(v12, v10);
  v10 = vmul(v12, v12);
  v9 = vmul(v12, v7);
  V v11 = vmul(d, v10);
  v11 = vsub(v8, v11);
  v11 = vmul(v9, v11);
  return vadd(v12, v11);
}

bool collideParticle(PPCContext& ctx, uint8_t* b) {
  const uint32_t r3 = ctx.r3.u32, r31 = ctx.r4.u32, r7 = ctx.r7.u32, r8 = ctx.r8.u32;
  const uint32_t kind = lwz(b, r7 + 64);
  ctx.fpscr.enableFlushMode();
  const V v5 = spltw0(image(fcopy(lwz(b, 0x820013B8u)), 0, 0, 0));
  const V p = lvx(b, r31);
  V c = lvx(b, r7 + 16);
  V v127, n;
  if (kind == 1) {
    const uint32_t len = fcopy(lwz(b, r7 + 72));
    V v13 = vsub(p, c);
    const uint32_t rad = fcopy(lwz(b, r7 + 68));
    const V axis = image(fcopy(lwz(b, r7 + 76)), fcopy(lwz(b, r7 + 80)), fcopy(lwz(b, r7 + 84)), 0);
    v13 = dp3(v13, axis);
    const V v11 = spltw0(image(len, 0, 0, 0));
    v127 = spltw0(image(rad, 0, 0, 0));
    const V v10 = vmul(v127, v127);
    v13 = vmax(v5, v13);
    v13 = vmin(v11, v13);
    v13 = vmul(axis, v13);
    c = vadd(c, v13);
    v13 = vsub(p, c);
    V v0 = dp3(v13, v13);
    v0 = vsub(v0, v10);
    if (gtMask(v5, v0) != 0xF) return true;  // outside: nothing written
    n = vmul(v13, nrRsqrt(dp3(v13, v13)));
  } else if (kind == 0) {
    const uint32_t rad = fcopy(lwz(b, r7 + 68));
    const V v12 = vsub(p, c);
    v127 = spltw0(image(rad, 0, 0, 0));
    V v0 = dp3(v12, v12);
    const V v13 = vmul(v127, v127);
    v0 = vsub(v0, v13);
    if (gtMask(v5, v0) != 0xF) return true;
    n = vmul(v12, nrRsqrt(dp3(v12, v12)));
  } else {
    return true;
  }
  if (int32_t(lwz(b, r7 + 32)) != 0) return false;  // one-sided plane: the guest body (sub_822E0E78)
  if (gtMask(dp3(lvx(b, r8), n), v5) != 0) {
    // bias n by the collider motion: n = normalize(n + motion * cloth+0x3D64)
    const V k = spltw0(image(fcopy(lwz(b, r3 + 15716)), 0, 0, 0));
    const V m = lvx(b, r8);
    V v13 = vmadd(m, k, n);
    const V d = dp3(v13, v13);
    // the 0A00 refinement (same steps, its own register order)
    V v11 = one4(), v10 = half4();
    const V v9 = one4(), v8 = half4();
    V v0 = vrsqrte(d);
    V v7 = vmul(v0, v0);
    v10 = vmul(v0, v10);
    v7 = vmul(d, v7);
    v11 = vsub(v11, v7);
    v11 = vmul(v10, v11);
    v0 = vadd(v0, v11);
    v11 = vmul(v0, v0);
    V v12 = vmul(d, v11);
    v11 = vmul(v0, v8);
    v12 = vsub(v9, v12);
    v12 = vmul(v11, v12);
    v0 = vadd(v0, v12);
    n = vmul(v13, v0);
  }
  stvx(b, r31, vadd(c, vmul(n, v127)));
  return true;
}

#if SR_ORACLE
void collideChecked(PPCContext& ctx, uint8_t* b) {
  const uint32_t r4 = ctx.r4.u32;
  uint8_t* p = gp(b, r4 & ~0xFu);
  if (!sr::kernel::hostRangeReadable(p, 16)) {
    g_unchk.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_822C0520(ctx, b);
    return;
  }
  uint8_t pre[16], guest[16];
  std::memcpy(pre, p, 16);
  const PPCContext in = ctx;
  __imp__sub_822C0520(ctx, b);
  const PPCContext guestOut = ctx;
  std::memcpy(guest, p, 16);
  std::memcpy(p, pre, 16);
  ctx = in;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  const bool ran = collideParticle(ctx, b);
  const bool bad = ran && (std::memcmp(p, guest, 16) != 0 || ctx.r3.u64 != guestOut.r3.u64);
  uint8_t nat[16];
  std::memcpy(nat, p, 16);
  std::memcpy(p, guest, 16);
  ctx = guestOut;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  if (!ran) {
    g_cfall.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_cchecked.fetch_add(1, std::memory_order_relaxed);
  if (bad) {
    const uint64_t m = g_cbad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 20)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_CLOTHSIM] MISMATCH #%llu CollideParticle p=%08X kind=%u guest=%08X %08X %08X native=%08X %08X %08X",
                  (unsigned long long)m, r4, lwz(b, in.r7.u32 + 64), lwz(b, r4), lwz(b, r4 + 4), lwz(b, r4 + 8),
                  _byteswap_ulong(*(uint32_t*)nat), _byteswap_ulong(*(uint32_t*)(nat + 4)),
                  _byteswap_ulong(*(uint32_t*)(nat + 8)));
  }
}
#endif  // SR_ORACLE

void report() {
#if SR_ORACLE
  if (clothSimAbOn()) {
    double s3[3], c3[3];
    g_abS.means(s3);
    g_abC.means(c3);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_CLOTHSIM] AB us/call satisfy guest=%.3f native=%.3f (%llu/%llu) collide guest=%.3f "
                "native=%.3f (%llu/%llu)",
                s3[0], s3[1], (unsigned long long)g_abS.calls[0].load(), (unsigned long long)g_abS.calls[1].load(),
                c3[0], c3[1], (unsigned long long)g_abC.calls[0].load(), (unsigned long long)g_abC.calls[1].load());
  }
#endif
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_CLOTHSIM] calls=%llu constraints=%llu checked=%llu checkBad=%llu unchecked=%llu | "
              "collide calls=%llu fallback=%llu checked=%llu checkBad=%llu",
              (unsigned long long)g_calls.load(std::memory_order_relaxed),
              (unsigned long long)g_cons.load(std::memory_order_relaxed),
              (unsigned long long)g_checked.load(std::memory_order_relaxed),
              (unsigned long long)g_bad.load(std::memory_order_relaxed),
              (unsigned long long)g_unchk.load(std::memory_order_relaxed),
              (unsigned long long)g_ccalls.load(std::memory_order_relaxed),
              (unsigned long long)g_cfall.load(std::memory_order_relaxed),
              (unsigned long long)g_cchecked.load(std::memory_order_relaxed),
              (unsigned long long)g_cbad.load(std::memory_order_relaxed));
}

#if SR_ORACLE
// CHECK: the only guest memory the pass writes is the positions its constraints index.
void checkedCall(PPCContext& ctx, uint8_t* b) {
  const uint32_t r3 = ctx.r3.u32;
  const uint32_t n = lwz(b, r3 + 15536), list = lwz(b, r3 + 15552), pos = lwz(b, r3 + 15500);
  uint32_t maxIdx = 0;
  bool ok = n <= 0x100000u;
  for (uint32_t k = 0; ok && k < n; ++k) {
    const uint32_t i0 = lwz(b, list + 16 * k), i1 = lwz(b, list + 16 * k + 4);
    if (i0 > 0xFFFFFu || i1 > 0xFFFFFu) ok = false;
    maxIdx = i0 > maxIdx ? i0 : maxIdx;
    maxIdx = i1 > maxIdx ? i1 : maxIdx;
  }
  const uint32_t len = n ? (maxIdx + 1) * 16 : 0;
  uint8_t* p = gp(b, pos);
  if (ok && len) {
    ok = sr::kernel::aliasGuestVA(pos + len - 1) - sr::kernel::aliasGuestVA(pos) == len - 1 &&
         sr::kernel::hostRangeReadable(p, len);
  }
  if (!ok) {
    g_unchk.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_822C0360(ctx, b);
    return;
  }
  std::vector<uint8_t> pre(p, p + len);
  const PPCContext in = ctx;
  __imp__sub_822C0360(ctx, b);
  const PPCContext guestOut = ctx;
  std::vector<uint8_t> guest(p, p + len);
  for (uint32_t k = 0; k < len; ++k)
    if (p[k] != pre[k]) p[k] = pre[k];
  ctx = in;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  satisfyDistanceConstraints(ctx, b);
  uint32_t diffs = 0, first = 0;
  uint8_t nb = 0;
  for (uint32_t k = 0; k < len; ++k)
    if (p[k] != guest[k]) {
      if (!diffs++) {
        first = k;
        nb = p[k];
      }
      p[k] = guest[k];
    }
  ctx = guestOut;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  g_checked.fetch_add(1, std::memory_order_relaxed);
  if (diffs) {
    const uint64_t m = g_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 20)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_CLOTHSIM] MISMATCH #%llu SatisfyDistanceConstraints cloth=%08X pos=%08X+%X (particle %u) "
                  "guest=%02X native=%02X diffs=%u n=%u",
                  (unsigned long long)m, r3, pos, first, first / 16, guest[first], nb, diffs, n);
  }
}
#endif  // SR_ORACLE

}  // namespace
}  // namespace sr::native::clothsim

// Cloth_SatisfyDistanceConstraints (NATIVE cloth sim; see the top of this file).
void sub_822C0360(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace sr::native::clothsim;
#if SR_ORACLE
  if (clothSimAbOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
      double t[3];
      g_abS.means(t);
      g_abC.means(t);
    }
    const int mode = int((n >> 6) & 1);
    const uint64_t t0 = __rdtsc();
    if (mode == 0) __imp__sub_822C0360(ctx, base);
    else satisfyDistanceConstraints(ctx, base);
    g_abS.add(mode, __rdtsc() - t0);
    if ((n & 0xFFF) == 0) report();
    return;
  }
  if (clothSimCheckOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    checkedCall(ctx, base);
    if ((n & 0xFFF) == 0) report();
    return;
  }
#endif  // SR_ORACLE
  if (clothSimOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    satisfyDistanceConstraints(ctx, base);
    if ((n & 0xFFF) == 0) report();
    return;
  }
  __imp__sub_822C0360(ctx, base);
}

// Cloth_CollideParticlePositionSphereCapsule (NATIVE cloth sim; see collideParticle).
void sub_822C0520(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace sr::native::clothsim;
#if SR_ORACLE
  if (clothSimAbOn()) {
    const uint64_t n = g_ccalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const int mode = int((n >> 8) & 1);
    const uint64_t t0 = __rdtsc();
    if (mode == 0) {
      __imp__sub_822C0520(ctx, base);
    } else if (!collideParticle(ctx, base)) {
      g_cfall.fetch_add(1, std::memory_order_relaxed);
      __imp__sub_822C0520(ctx, base);
    }
    g_abC.add(mode, __rdtsc() - t0);
    return;
  }
  if (clothSimCheckOn()) {
    g_ccalls.fetch_add(1, std::memory_order_relaxed);
    collideChecked(ctx, base);
    return;
  }
#endif  // SR_ORACLE
  if (clothSimOn()) {
    g_ccalls.fetch_add(1, std::memory_order_relaxed);
    if (!collideParticle(ctx, base)) {
      g_cfall.fetch_add(1, std::memory_order_relaxed);
      __imp__sub_822C0520(ctx, base);
    }
    return;
  }
  __imp__sub_822C0520(ctx, base);
}
