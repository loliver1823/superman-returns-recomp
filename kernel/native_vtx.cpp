// NATIVE vertex append (iter-467 guestcode round 5): AppendVertexData sub_8231AD20 as host code.
// ~2% self of the guest render thread in the ingame window (cm8_prof): the immediate-mode vertex
// writer (particles and other dynamic geometry). (r3 -> the stream cursor, r4 -> position,
// r5 -> packed colour word, r6 -> uv, r7 -> vec4): appends x y z, the colour word, u v and the
// four vec4 words after the cursor (the cursor points at the word BEFORE the next free one), 0x28
// bytes, and advances it by 0x28.
//
// The transcription makes the guest's loads and stores in its order, re-reading the cursor where
// the guest re-reads it (the stvewx stores of splatted elements are plain word stores of that
// element; u and v pass through lfs/stfs: fcopy). The red-zone stack copy of the uv vector is
// not written.
//
// GATES. SR_NATIVE_VTXAPPEND (baked default ON under the iter-467 per-call rule, =0 = the guest body: CHECK 0 bad, faster per call, live 3x3 + in-roll A/B no regression - PERFORMANCE 2.16). SR_NATIVE_VTXAPPEND_CHECK=1:
// per call, the 0x28 appended bytes + the cursor word compared against the guest original.
// SR_NATIVE_VTXAPPEND_AB=1: in-process timing A/B.
// C1: the CHECK and the A/B are development oracles, compiled out under SR_ORACLE=0 (kernel/sr_oracle.hpp): there
// both gates read as unset (no env read), so the release runs SR_NATIVE_VTXAPPEND's native path or the guest body only.

#include "native_vmx.hpp"
#include "ab_toggle.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void __imp__sub_8231AD20(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::native::vtx {
namespace {

using namespace sr::native::vmx;

bool vtxOn() {  // read at the point of use (SR_AB_TOGGLE, kernel/ab_toggle.hpp)
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_VTXAPPEND"); return !e || e[0] != '0'; }();
  static const int ab = sr::abt::reg("SR_NATIVE_VTXAPPEND");
  return v && (ab < 0 || sr::abt::on(ab));
}
#if SR_ORACLE
bool vtxCheckOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_VTXAPPEND_CHECK"); return e && e[0] == '1'; }();
  return v;
}
bool vtxAbOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_VTXAPPEND_AB"); return e && e[0] == '1'; }();
  return v;
}

std::atomic<uint64_t> g_calls{0}, g_checked{0}, g_bad{0}, g_unchk{0};
AbTimer g_ab;
#endif  // SR_ORACLE

void appendVertex(PPCContext& ctx, uint8_t* b) {
  const uint32_t r3 = ctx.r3.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
  const V p = lvx(b, ctx.r4.u32);
  stw(b, (lwz(b, r3) + 4) & ~3u, elem(p, 0));
  stw(b, (lwz(b, r3) + 8) & ~3u, elem(p, 1));
  stw(b, (lwz(b, r3) + 12) & ~3u, elem(p, 2));
  uint32_t r10 = lwz(b, r3) + 12;
  stw(b, r3, r10);
  const uint32_t c = lwz(b, r5);
  r10 += 4;
  stw(b, r10, c);  // stwu
  const uint32_t r11 = r10;
  r10 += 8;
  stw(b, r3, r11);
  const V uv = lvx(b, r6);
  stw(b, r11 + 4, fcopy(elem(uv, 0)));
  const V uv2 = lvx(b, r6);
  stw(b, r3, r10);
  stw(b, r11 + 8, fcopy(elem(uv2, 1)));
  const V q = lvx(b, r7);
  stw(b, (r10 + 4) & ~3u, elem(q, 0));
  stw(b, (lwz(b, r3) + 8) & ~3u, elem(q, 1));
  stw(b, (lwz(b, r3) + 12) & ~3u, elem(q, 2));
  stw(b, (lwz(b, r3) + 16) & ~3u, elem(q, 3));
  stw(b, r3, lwz(b, r3) + 16);
}

#if SR_ORACLE
void report() {
  double us[3];
  g_ab.means(us);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_VTXAPPEND] calls=%llu checked=%llu checkBad=%llu unchecked=%llu | AB us/call guest=%.3f native=%.3f",
              (unsigned long long)g_calls.load(std::memory_order_relaxed),
              (unsigned long long)g_checked.load(std::memory_order_relaxed),
              (unsigned long long)g_bad.load(std::memory_order_relaxed),
              (unsigned long long)g_unchk.load(std::memory_order_relaxed), us[0], us[1]);
}

void checkedCall(PPCContext& ctx, uint8_t* b) {
  const uint32_t r3 = ctx.r3.u32;
  const uint32_t cur = lwz(b, r3);
  const uint32_t lo = (cur + 4) & ~3u, len = 0x28;
  uint8_t* out = gp(b, lo);
  if (sr::kernel::aliasGuestVA(lo + len - 1) - sr::kernel::aliasGuestVA(lo) != len - 1 ||
      !sr::kernel::hostRangeReadable(out, len) || (lo <= r3 + 3 && r3 < lo + len)) {
    g_unchk.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_8231AD20(ctx, b);
    return;
  }
  uint8_t pre[0x28], guest[0x28], nat[0x28];
  std::memcpy(pre, out, len);
  const PPCContext in = ctx;
  __imp__sub_8231AD20(ctx, b);
  const PPCContext guestOut = ctx;
  std::memcpy(guest, out, len);
  const uint32_t gcur = lwz(b, r3);
  std::memcpy(out, pre, len);
  stw(b, r3, cur);
  ctx = in;
  appendVertex(ctx, b);
  std::memcpy(nat, out, len);
  const uint32_t ncur = lwz(b, r3);
  const bool bad = std::memcmp(nat, guest, len) != 0 || ncur != gcur;
  std::memcpy(out, guest, len);
  stw(b, r3, gcur);
  ctx = guestOut;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  g_checked.fetch_add(1, std::memory_order_relaxed);
  if (bad) {
    const uint64_t m = g_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 20) {
      int k = 0;
      while (k < int(len) && nat[k] == guest[k]) ++k;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_VTXAPPEND] MISMATCH #%llu cursor=%08X byte %d guest=%02X native=%02X cur guest=%08X native=%08X",
                  (unsigned long long)m, cur, k, k < int(len) ? guest[k] : 0, k < int(len) ? nat[k] : 0, gcur, ncur);
    }
  }
}
#endif  // SR_ORACLE

}  // namespace
}  // namespace sr::native::vtx

// AppendVertexData (NATIVE vertex append; see the top of this file).
void sub_8231AD20(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace sr::native::vtx;
#if SR_ORACLE
  if (vtxAbOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
      double t[3];
      g_ab.means(t);
    }
    const int mode = int((n >> 9) & 1);
    const uint64_t t0 = __rdtsc();
    if (mode == 0) __imp__sub_8231AD20(ctx, base);
    else appendVertex(ctx, base);
    g_ab.add(mode, __rdtsc() - t0);
    if ((n & 0xFFFFF) == 0) report();
    return;
  }
  if (vtxCheckOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    checkedCall(ctx, base);
    if ((n & 0xFFFFF) == 0) report();
    return;
  }
#endif  // SR_ORACLE
  if (vtxOn()) {
    appendVertex(ctx, base);
    return;
  }
  __imp__sub_8231AD20(ctx, base);
}
