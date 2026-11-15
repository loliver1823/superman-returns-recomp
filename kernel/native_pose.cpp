// NATIVE pose leaf (iter-467 guestcode round 4): PoseSnapshot_GetWorldMatrix sub_822F5990 as host
// code. Virtual slot +48 of about nine frame-snapshot vtables (cloth, pose, ...): (snapshot r3,
// out r4) writes the 4x4 world matrix - rows 0-2 the rotation of the quaternion at +0x720 with
// the 2.0 prescale (flt_820BD0EC), row 3 the position at +0x730. 0.29 ms self of the 8.85 ms
// player-profile render tick (docs/PERFORMANCE.md 2.11): the recompiled body assembles the
// rows element by element through the stack (stvx, lfs, stfs, lvx) with ~20 MXCSR switches.
//
// The transcription computes the same VMX products / sums in the same operand order (flush
// mode), and gives every element that the guest routes through an lfs/stfs pair the IEEE-mode
// round trip (fcopy: a bit copy that quiets a signalling NaN). It also makes the one store the
// guest makes outside its red zone: r4 into the caller's frame at r1+28.
//
// GATES. SR_NATIVE_POSE (default ON since iter-467; =0 runs the guest body). SR_NATIVE_POSE_CHECK=1: per
// call, run the guest original, keep its result, restore, run the native version, compare the
// 64 output bytes and the r1+28 word. SR_NATIVE_POSE_AB=1: in-process timing A/B.
// C1: the CHECK and the A/B are development oracles, compiled out under SR_ORACLE=0 (kernel/sr_oracle.hpp): there
// both gates read as unset (no env read), so the release runs SR_NATIVE_POSE's native path or the guest body only.

#include "native_vmx.hpp"
#include "ab_toggle.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void __imp__sub_822F5990(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::native::pose {
namespace {

using namespace sr::native::vmx;

bool poseOn() {  // read at the point of use (SR_AB_TOGGLE, kernel/ab_toggle.hpp)
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_POSE"); return !e || e[0] != '0'; }();
  static const int ab = sr::abt::reg("SR_NATIVE_POSE");
  return v && (ab < 0 || sr::abt::on(ab));
}
#if SR_ORACLE
bool poseCheckOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_POSE_CHECK"); return e && e[0] == '1'; }();
  return v;
}
bool poseAbOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_POSE_AB"); return e && e[0] == '1'; }();
  return v;
}

std::atomic<uint64_t> g_calls{0}, g_checked{0}, g_bad{0}, g_unchk{0};
AbTimer g_ab;
#endif  // SR_ORACLE

constexpr uint32_t kZero = 0x820013B8u, kOne = 0x820013C0u, kTwo = 0x820BD0ECu;

void getWorldMatrix(PPCContext& ctx, uint8_t* b) {
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r1 = ctx.r1.u32;
  stw(b, r1 + 28, r4);
  ctx.fpscr.enableFlushMode();
  const V q = lvx(b, r3 + 1824);
  const uint32_t c0 = fcopy(lwz(b, kZero)), c1 = fcopy(lwz(b, kOne)), two = fcopy(lwz(b, kTwo));
  const V Y = spltw1(q), Z = spltw2(q), X = spltw0(q), W = spltw3(q);
  const V y2 = vmul(Y, Y), yz = vmul(Y, Z), z2 = vmul(Z, Z);
  const V xz = vmul(X, Z), x2 = vmul(X, X), xy = vmul(X, Y);
  const V wx = vmul(W, X), wy = vmul(W, Y), wz = vmul(W, Z);
  const V one = spltw0(image(c1, 0, 0, 0)), t = spltw0(image(two, 0, 0, 0));
  const V p = lvx(b, r3 + 1840);
  const V x2y2 = vadd(x2, y2), x2z2 = vadd(x2, z2);
  const V yzmwx = vsub(yz, wx), yzpwx = vadd(yz, wx);
  const V xzpwy = vadd(xz, wy);
  const V D = vmul(t, yzpwx);
  const V Bp = vmul(t, x2y2);
  const V A = vmul(t, yzmwx);
  const V C = vmul(t, xzpwy);
  const V B = vsub(one, Bp);
  const V Ep = vmul(t, x2z2);
  const V y2z2 = vadd(y2, z2), xymwz = vsub(xy, wz), xzmwy = vsub(xz, wy), xypwz = vadd(xy, wz);
  const V E = vsub(one, Ep);
  const V Ip = vmul(t, y2z2);
  const V F = vmul(t, xymwz), G = vmul(t, xzmwy), H = vmul(t, xypwz);
  const V I = vsub(one, Ip);
  auto e0 = [](V v) { return fcopy(elem(v, 0)); };
  stvx(b, r4, image(e0(I), e0(H), e0(G), c0));
  stvx(b, r4 + 16, image(e0(F), e0(E), e0(D), c0));
  stvx(b, r4 + 32, image(e0(C), e0(A), e0(B), c0));
  stvx(b, r4 + 48, image(fcopy(elem(p, 0)), fcopy(elem(p, 1)), fcopy(elem(p, 2)), c1));
}

#if SR_ORACLE
void report() {
  double us[3];
  g_ab.means(us);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_POSE] calls=%llu checked=%llu checkBad=%llu unchecked=%llu | AB us/call guest=%.3f native=%.3f",
              (unsigned long long)g_calls.load(std::memory_order_relaxed),
              (unsigned long long)g_checked.load(std::memory_order_relaxed),
              (unsigned long long)g_bad.load(std::memory_order_relaxed),
              (unsigned long long)g_unchk.load(std::memory_order_relaxed), us[0], us[1]);
}

void checkedCall(PPCContext& ctx, uint8_t* b) {
  const uint32_t r4 = ctx.r4.u32 & ~0xFu, slot = ctx.r1.u32 + 28;
  uint8_t* out = gp(b, r4);
  uint8_t* sp = gp(b, slot);
  if (sr::kernel::aliasGuestVA(r4 + 63) - sr::kernel::aliasGuestVA(r4) != 63 || !sr::kernel::hostRangeReadable(out, 64) ||
      !sr::kernel::hostRangeReadable(sp, 4)) {
    g_unchk.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_822F5990(ctx, b);
    return;
  }
  uint8_t pre[68], guest[68], nat[68];
  std::memcpy(pre, out, 64);
  std::memcpy(pre + 64, sp, 4);
  const PPCContext in = ctx;
  __imp__sub_822F5990(ctx, b);
  const PPCContext guestOut = ctx;
  std::memcpy(guest, out, 64);
  std::memcpy(guest + 64, sp, 4);
  std::memcpy(out, pre, 64);
  std::memcpy(sp, pre + 64, 4);
  ctx = in;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  getWorldMatrix(ctx, b);
  std::memcpy(nat, out, 64);
  std::memcpy(nat + 64, sp, 4);
  const bool bad = std::memcmp(nat, guest, 68) != 0 || ctx.r3.u64 != guestOut.r3.u64;
  std::memcpy(out, guest, 64);
  std::memcpy(sp, guest + 64, 4);
  ctx = guestOut;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  g_checked.fetch_add(1, std::memory_order_relaxed);
  if (bad) {
    const uint64_t m = g_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 20) {
      int k = 0;
      while (k < 68 && nat[k] == guest[k]) ++k;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_POSE] MISMATCH #%llu snapshot=%08X out=%08X byte %d guest=%02X native=%02X", (unsigned long long)m,
                  in.r3.u32, in.r4.u32, k, k < 68 ? guest[k] : 0, k < 68 ? nat[k] : 0);
    }
  }
}
#endif  // SR_ORACLE

}  // namespace
}  // namespace sr::native::pose

// PoseSnapshot_GetWorldMatrix (NATIVE pose leaf; see the top of this file).
void sub_822F5990(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace sr::native::pose;
#if SR_ORACLE
  if (poseAbOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
      double t[3];
      g_ab.means(t);
    }
    const int mode = int((n >> 8) & 1);
    const uint64_t t0 = __rdtsc();
    if (mode == 0) __imp__sub_822F5990(ctx, base);
    else getWorldMatrix(ctx, base);
    g_ab.add(mode, __rdtsc() - t0);
    if ((n & 0xFFFF) == 0) report();
    return;
  }
  if (poseCheckOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    checkedCall(ctx, base);
    if ((n & 0xFFFF) == 0) report();
    return;
  }
#endif  // SR_ORACLE
  if (poseOn()) {
    getWorldMatrix(ctx, base);
    return;
  }
  __imp__sub_822F5990(ctx, base);
}
