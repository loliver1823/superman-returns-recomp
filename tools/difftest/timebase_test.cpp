// iter-463 SR_TIMEBASE_50MHZ: one 50 MHz guest clock domain (kernel/sr_timebase.cpp) +
// the two stale-MXCSR restores in spliced bodies.
//
//   --timebase-test
//     S*  split scaling (d/den)*num + ((d%den)*num)/den vs an exact software 128/64 divide,
//         over overflow edges for every plausible host QPF, + the 128-bit product inequality
//     R*  rate: 50e6 +-0.1% over 2 s against GetSystemTimePreciseAsFileTime; legacy (=0) is
//         the raw TSC (the 74x defect, measured)
//     M*  monotonic: 8 threads x 10^7 reads, per thread AND across threads (happens-before)
//     B*  KeTimeStampBundle: ms == ticks/50000, interrupt == ticks/5, system == base+ticks/5
//         from one read; ms vs a later mftb/50000 within +-1; a live 1 ms ticker sampled 10^5
//         times; the tick-period measurement (sleep_for(1ms) vs the high-resolution timer)
//     K*  kernel exports: QPF = 50 MHz with NO guest write at r3; QPC inside [mftb before, after];
//         QPC/QPF == wall time; KeQuerySystemTime = base + ticks/5 and stored into bundle+8;
//         the import glue returns QPF as a full 64-bit r3
//     G*  generated code: sub_824680D0 (inlined QueryPerformanceCounter = mftb, a force-included
//         TU) and sub_825667C8 (ms = QPC/(QPF/1000)) read the 50 MHz clock; sub_824680C0 (the
//         GameKernelLoop pacing clock) reads bundle+0x10 with the gate on AND off (its original body
//         since 156b6f47 deleted the GuestClockMs splice; G4)
//     X*  MXCSR after the spliced context restores: Lua rawrunprotected (sub_82762400, longjmp
//         keeps the THROW-time FPSCR) and the CONDVAR autoSem splice (sub_825FC1A0, pre-call
//         FPSCR), cached csr == host MXCSR afterwards
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/sr_timebase.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(sub_824680C0);   // GameKernelLoop pacing clock (the original body: bundle+0x10 ms, either gate)
PPC_EXTERN_FUNC(sub_824680D0);   // QueryPerformanceCounter (inlined mftb), ppc_recomp.57
PPC_EXTERN_FUNC(sub_825667C8);   // ms clock: QPC / qword_829792F8 (= QPF/1000)
PPC_EXTERN_FUNC(sub_82762400);   // Lua rawrunprotected (iter-350 host setjmp splice)
PPC_EXTERN_FUNC(sub_825FC1A0);   // CONDVAR wait (autoSem splice)
PPC_EXTERN_FUNC(__imp__KeQueryPerformanceFrequency);
extern "C" bool srGuestLongjmpTryThrow(uint32_t bufVA, uint32_t code);  // ppc_recomp.106

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include <windows.h>
#include <xmmintrin.h>
#include <x86intrin.h>

namespace K = sr::kernel;
namespace TB = sr::timebase;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

uint64_t preciseFt() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
uint64_t rdBE64(uint32_t va) {
  const uint8_t* p = K::guestVAToHost(va);
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}
void wrBE64(uint32_t va, uint64_t v) {
  uint8_t* p = K::guestVAToHost(va);
  for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (56 - 8 * i));
}
uint32_t rdBE32p(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t rdBE64p(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

// ------------------------------------------------------------------ S: scaling --------------
bool exactOk(uint64_t d, uint64_t num, uint64_t den, uint64_t r) {
  // floor(d*num/den) == r  <=>  r*den <= d*num < (r+1)*den   (128-bit products, no division)
  const unsigned __int128 p = (unsigned __int128)d * num;
  const unsigned __int128 lo = (unsigned __int128)r * den;
  const unsigned __int128 hi = lo + den;
  return lo <= p && p < hi;
}

void scalingTests() {
  std::printf("[S] split scaling vs exact 128/64 (no __udivti3)\n");
  const uint64_t qpfs[] = {10000000ull, 3579545ull, 14318180ull, 24000000ull, 19200000ull,
                           1000000000ull, 3700020649ull, 4294967296ull, 50000000ull, 1ull};
  uint64_t splitChecked = 0, splitBad = 0, exactChecked = 0, exactBad = 0, slowPairs = 0;
  std::mt19937_64 rng(463);
  for (uint64_t qpf : qpfs) {
    uint64_t num = TB::kGuestHz, den = qpf, a = num, b = den;
    while (b) { uint64_t t = a % b; a = b; b = t; }
    num /= a; den /= a;
    const bool splitSafe = !(den > 1 && (den - 1) > UINT64_MAX / num);
    if (!splitSafe) ++slowPairs;
    // largest d whose exact result still fits in 64 bits (keep split inputs in-domain too)
    const uint64_t dTop = (num >= den) ? (UINT64_MAX / num) * den : UINT64_MAX;   // result <= UINT64_MAX
    std::vector<uint64_t> ds = {0, 1, 2, den - 1, den, den + 1, 2 * den - 1, 0xFFFFFFFFull,
                                0x100000000ull, 1ull << 40, (1ull << 53) + 1, 1ull << 62,
                                dTop / 2, dTop - den, dTop - 1, dTop};
    for (int i = 0; i < 200000; ++i) ds.push_back(rng() % (dTop == UINT64_MAX ? UINT64_MAX : dTop + 1));
    for (uint64_t d : ds) {
      if (d > dTop) continue;
      const uint64_t e = TB::mulDivExact(d, num, den);
      ++exactChecked;
      if (!exactOk(d, num, den, e)) {
        if (exactBad++ < 3) std::printf("    exact mismatch qpf=%llu d=%llu\n", (unsigned long long)qpf, (unsigned long long)d);
      }
      if (splitSafe) {
        ++splitChecked;
        if (TB::scaleSplit(d, num, den) != e) {
          if (splitBad++ < 3) std::printf("    split mismatch qpf=%llu d=%llu\n", (unsigned long long)qpf, (unsigned long long)d);
        }
      }
    }
  }
  char buf[200];
  std::snprintf(buf, sizeof buf, "S1 mulDivExact == floor(d*num/den) by the 128-bit product inequality: %llu inputs, %llu bad",
                (unsigned long long)exactChecked, (unsigned long long)exactBad);
  check(exactBad == 0 && exactChecked > 0, buf);
  std::snprintf(buf, sizeof buf, "S2 split form == exact on every split-safe (num,den): %llu inputs, %llu bad (%llu pairs need the slow path)",
                (unsigned long long)splitChecked, (unsigned long long)splitBad, (unsigned long long)slowPairs);
  check(splitBad == 0 && splitChecked > 0, buf);
  check(TB::mulDivExact(UINT64_MAX, 50000000ull, 1ull) == UINT64_MAX && TB::mulDivExact(1, 1, 0) == UINT64_MAX,
        "S3 mulDivExact reports a non-fitting quotient / den=0 as UINT64_MAX (never wraps silently)");
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  std::snprintf(buf, sizeof buf, "S4 this host: QPF=%lld -> scale %llu/%llu (num*QPF == 50e6*den)", f.QuadPart,
                (unsigned long long)TB::scaleNum(), (unsigned long long)TB::scaleDen());
  check(TB::scaleNum() * uint64_t(f.QuadPart) == TB::kGuestHz * TB::scaleDen(), buf);
  // Horizon: at 5/1 the split form's first overflow is d/den*num > 2^64 -> years.
  std::printf("  info: low 32 bits of the 50 MHz clock wrap every %.1f s (legacy raw TSC: %.2f s at 3.7 GHz)\n",
              4294967296.0 / 50e6, 4294967296.0 / 3.7e9);
}

// ------------------------------------------------------------------ R: rate -----------------
void rateTests() {
  std::printf("[R] rate against GetSystemTimePreciseAsFileTime\n");
  TB::reinitForTest(true);
  const uint64_t ft0 = preciseFt(), t0 = sr_guest_mftb();
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const uint64_t t1 = sr_guest_mftb(), ft1 = preciseFt();
  const double secs = double(ft1 - ft0) * 1e-7;
  const double rate = double(t1 - t0) / secs;
  char buf[200];
  std::snprintf(buf, sizeof buf, "R1 gate on: %.0f ticks/s over %.3f s = %.5fx 50 MHz (want 1 +-0.001)", rate, secs, rate / 50e6);
  check(std::abs(rate / 50e6 - 1.0) < 1e-3, buf);

  TB::reinitForTest(false);
  const uint64_t lf0 = preciseFt(), l0 = sr_guest_mftb();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const uint64_t l1 = sr_guest_mftb(), lf1 = preciseFt();
  const double lrate = double(l1 - l0) / (double(lf1 - lf0) * 1e-7);
  std::snprintf(buf, sizeof buf, "R2 gate off (legacy A/B) is the raw TSC: %.0f ticks/s = %.2fx 50 MHz (the defect)", lrate, lrate / 50e6);
  check(lrate / 50e6 > 10.0, buf);
  TB::reinitForTest(true);
}

// ------------------------------------------------------------------ M: monotonic ------------
void monotonicTests() {
  std::printf("[M] monotonic: 8 threads x 10^7 reads\n");
  TB::reinitForTest(true);
  constexpr int kThreads = 8;
  constexpr uint64_t kReads = 10000000ull;
  std::atomic<uint64_t> published{0};
  std::atomic<uint64_t> selfBack{0}, crossBack{0};
  std::atomic<int> ready{0};
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> th;
  for (int i = 0; i < kThreads; ++i) {
    th.emplace_back([&] {
      ready.fetch_add(1);
      while (ready.load() < kThreads) {}
      uint64_t prev = 0, sb = 0, cb = 0;
      for (uint64_t n = 0; n < kReads; ++n) {
        const uint64_t seen = published.load(std::memory_order_acquire);  // published BEFORE our read
        const uint64_t t = sr_guest_mftb();
        if (t < prev) ++sb;
        if (t < seen) ++cb;
        prev = t;
        if ((n & 63) == 0) {
          uint64_t cur = published.load(std::memory_order_relaxed);
          while (cur < t && !published.compare_exchange_weak(cur, t, std::memory_order_release)) {}
        }
      }
      selfBack.fetch_add(sb);
      crossBack.fetch_add(cb);
    });
  }
  for (auto& t : th) t.join();
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  char buf[220];
  std::snprintf(buf, sizeof buf, "M1 per-thread non-decreasing: %llu backward steps in %llu reads (%.1f M reads/s total)",
                (unsigned long long)selfBack.load(), (unsigned long long)(kThreads * kReads), kThreads * kReads / s / 1e6);
  check(selfBack.load() == 0, buf);
  std::snprintf(buf, sizeof buf, "M2 cross-thread: a read never precedes a value another thread published before it: %llu violations",
                (unsigned long long)crossBack.load());
  check(crossBack.load() == 0, buf);
}

// ------------------------------------------------------------------ B: bundle ---------------
struct PeriodStats { double mean = 0, max = 0; };
template <class F> PeriodStats measurePeriod(F wait, int n) {
  LARGE_INTEGER f, a, prev;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&a);
  prev = a;
  PeriodStats st;
  for (int i = 0; i < n; ++i) {
    wait();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double dt = double(now.QuadPart - prev.QuadPart) * 1000.0 / double(f.QuadPart);
    if (dt > st.max) st.max = dt;
    prev = now;
  }
  st.mean = double(prev.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart) / n;
  return st;
}

void bundleTests() {
  std::printf("[B] KeTimeStampBundle in the same domain\n");
  TB::reinitForTest(true);
  alignas(8) uint8_t b[0x20] = {};
  const uint64_t t = TB::writeBundle(b);
  const uint64_t after = sr_guest_mftb();
  char buf[220];
  check(rdBE32p(b + 0x10) == uint32_t(t / 50000) && rdBE64p(b) == t / 5 &&
            rdBE64p(b + 8) == TB::systemTimeBase100ns() + t / 5,
        "B1 one tick read: ms = t/50000, interrupt = t/5, system = base + t/5 (big-endian)");
  const int64_t dms = int64_t(after / 50000) - int64_t(rdBE32p(b + 0x10));
  std::snprintf(buf, sizeof buf, "B2 bundle ms vs the next mftb/50000: diff %lld (want 0..1)", (long long)dms);
  check(dms >= 0 && dms <= 1, buf);

  // Tick granularity: what the legacy ticker's sleep_for(1ms) really gives on this host,
  // vs the high-resolution waitable timer the gated ticker uses.
  const PeriodStats sl = measurePeriod([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }, 300);
  TB::PeriodicMs pm;
  const PeriodStats hr = measurePeriod([&] { pm.wait(); }, 1000);
  std::printf("  info: sleep_for(1ms) period mean %.3f ms max %.3f ms (no timeBeginPeriod in this process)\n", sl.mean, sl.max);
  std::snprintf(buf, sizeof buf, "B3 high-resolution timer (%s): period mean %.3f ms max %.3f ms (want mean < 1.5)",
                pm.highResolution() ? "CREATE_WAITABLE_TIMER_HIGH_RESOLUTION" : "FALLBACK sleep_for", hr.mean, hr.max);
  check(pm.highResolution() && hr.mean < 1.5, buf);

  // Live: a 1 ms ticker (the gated kernel loop's shape) sampled by a reader for ~3 s.
  std::atomic<bool> stop{false};
  alignas(8) static uint8_t live[0x20];
  TB::writeBundle(live);
  std::thread ticker([&] {
    TB::PeriodicMs p;
    while (!stop.load(std::memory_order_relaxed)) {
      TB::writeBundle(live);
      p.wait();
    }
  });
  uint64_t samples = 0, within1 = 0, ahead = 0;
  int64_t worst = 0;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < end) {
    for (int i = 0; i < 100; ++i) {
      // one aligned 32-bit load, as the guest's lwz does (the ticker writes it concurrently)
      const uint32_t ms = __builtin_bswap32(__atomic_load_n(reinterpret_cast<uint32_t*>(live + 0x10), __ATOMIC_RELAXED));
      const int64_t d = int64_t(sr_guest_mftb() / 50000) - int64_t(ms);
      ++samples;
      if (d < 0) ++ahead;
      if (d >= -1 && d <= 1) ++within1;
      if (d > worst) worst = d;
    }
    std::this_thread::yield();
  }
  stop = true;
  ticker.join();
  std::snprintf(buf, sizeof buf, "B4 live ticker: %llu samples, %.4f%% within +-1 ms of mftb/50000, worst lag %lld ms, %llu ahead (want >=99%%, 0 ahead)",
                (unsigned long long)samples, 100.0 * within1 / samples, (long long)worst, (unsigned long long)ahead);
  check(samples > 1000 && within1 * 100 >= samples * 99 && ahead == 0, buf);
}

// ------------------------------------------------------------------ K: kernel exports -------
constexpr uint32_t kScratch = 0x70700000u;
constexpr uint32_t kImageProbe = 0x82A00000u;   // in the old stray-write window [0x82000000,0x83000000)

void kernelTests() {
  std::printf("[K] kernel exports in the same domain\n");
  TB::reinitForTest(true);
  auto& reg = K::ExportRegistry::instance();
  auto qpf = reg.getExport("KeQueryPerformanceFrequency");
  auto qpc = reg.getExport("KeQueryPerformanceCounter");
  auto qst = reg.getExport("KeQuerySystemTime");
  check(qpf && qpc && qst, "K0 exports registered");
  if (!qpf || !qpc || !qst) return;
  char buf[220];

  wrBE64(kImageProbe, 0x1122334455667788ull);
  const uint32_t f = qpf(kImageProbe, 0, 0, 0, 0, 0);
  std::snprintf(buf, sizeof buf, "K1 QPF returns %u and leaves guest memory at r3 alone (read back %016llX)", f,
                (unsigned long long)rdBE64(kImageProbe));
  check(f == 50000000u && rdBE64(kImageProbe) == 0x1122334455667788ull, buf);

  const uint64_t a = sr_guest_mftb();
  qpc(kScratch, 0, 0, 0, 0, 0);
  const uint64_t c = rdBE64(kScratch), z = sr_guest_mftb();
  std::snprintf(buf, sizeof buf, "K2 QPC inside [mftb before, after]: %llu <= %llu <= %llu", (unsigned long long)a,
                (unsigned long long)c, (unsigned long long)z);
  check(a <= c && c <= z, buf);

  const uint64_t ft0 = preciseFt();
  qpc(kScratch, 0, 0, 0, 0, 0);
  const uint64_t c0 = rdBE64(kScratch);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  qpc(kScratch, 0, 0, 0, 0, 0);
  const uint64_t c1 = rdBE64(kScratch), ft1 = preciseFt();
  const double qs = double(c1 - c0) / double(f), ws = double(ft1 - ft0) * 1e-7;
  std::snprintf(buf, sizeof buf, "K3 QPC delta / QPF = %.4f s vs wall %.4f s (want within 0.5%%)", qs, ws);
  check(std::abs(qs / ws - 1.0) < 5e-3, buf);

  // KeQuerySystemTime: base + ticks/5, and stored into bundle+8 once the bundle is published.
  wrBE64(TB::kBundleVA + 8, 0);
  TB::setBundlePublished(true);
  const uint64_t s0 = sr_guest_mftb();
  qst(kScratch, 0, 0, 0, 0, 0);
  const uint64_t st = rdBE64(kScratch), s1 = sr_guest_mftb();
  const uint64_t lo = TB::systemTimeBase100ns() + s0 / 5, hi = TB::systemTimeBase100ns() + s1 / 5;
  const int64_t vsHost = int64_t(st) - int64_t(preciseFt());
  std::snprintf(buf, sizeof buf, "K4 KeQuerySystemTime = base + ticks/5 (in [%llu,%llu]: %llu), host FILETIME delta %.3f ms",
                (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)st, vsHost / 1e4);
  check(lo <= st && st <= hi && std::llabs(vsHost) < 20000000, buf);   // < 2 s (sanity)
  check(rdBE64(TB::kBundleVA + 8) == st, "K5 KeQuerySystemTime stored the same value into KeTimeStampBundle+8 (Xenia)");
  TB::setBundlePublished(false);

  PPCContext ctx{};
  ctx.r3.u64 = 0xDEADBEEF00000000ull | kImageProbe;
  __imp__KeQueryPerformanceFrequency(ctx, K::getGuestBase());
  std::snprintf(buf, sizeof buf, "K6 import glue: r3 = %016llX after QPF (full 64-bit 50e6; callers std/rldicl it)",
                (unsigned long long)ctx.r3.u64);
  check(ctx.r3.u64 == 50000000ull, buf);
}

// ------------------------------------------------------------------ G: generated code -------
void guestTests() {
  std::printf("[G] generated guest code reads the 50 MHz clock\n");
  uint8_t* base = K::getGuestBase();
  char buf[220];
  TB::reinitForTest(true);
  PPCContext ctx{};
  ctx.r3.u64 = kScratch;
  const uint64_t a = sr_guest_mftb();
  sub_824680D0(ctx, base);
  const uint64_t q = rdBE64(kScratch), z = sr_guest_mftb();
  std::snprintf(buf, sizeof buf, "G1 sub_824680D0 (inlined QPC = mftb, force-included ppc_recomp.57): %llu in [%llu,%llu]",
                (unsigned long long)q, (unsigned long long)a, (unsigned long long)z);
  check(a <= q && q <= z, buf);

  constexpr uint32_t kDiv = 0x829792F8u;   // qword_829792F8 = QPF/1000 (NetServiceInit_sub_82566A50)
  const uint64_t savedDiv = rdBE64(kDiv);
  wrBE64(kDiv, 50000);
  ctx = PPCContext{};
  ctx.r1.u64 = 0x70580000u;
  std::this_thread::sleep_for(std::chrono::milliseconds(40));   // so the ms value is not trivially 0
  const uint64_t m0 = sr_guest_mftb() / 50000;
  sub_825667C8(ctx, base);
  const uint64_t m1 = sr_guest_mftb() / 50000;
  std::snprintf(buf, sizeof buf, "G2 sub_825667C8 ms clock = %u in [%llu,%llu] (guest ms since origin)", ctx.r3.u32,
                (unsigned long long)m0, (unsigned long long)m1);
  check(m0 >= 40 && ctx.r3.u32 >= uint32_t(m0) && ctx.r3.u32 <= uint32_t(m1), buf);
  wrBE64(kDiv, savedDiv);

  // sub_824680C0: gate on + bundle published -> *(*(0x820007D0)+0x10).
  const uint32_t savedCell = K::readGuestU32BE(0x820007D0u);
  const uint32_t savedMs = K::readGuestU32BE(TB::kBundleVA + 0x10);
  K::writeGuestBE32(0x820007D0u, TB::kBundleVA);
  K::writeGuestBE32(TB::kBundleVA + 0x10, 0x00C0FFEEu);
  TB::setBundlePublished(true);
  ctx = PPCContext{};
  sub_824680C0(ctx, base);
  std::snprintf(buf, sizeof buf, "G3 sub_824680C0, gate on + bundle live: r3 = %08X (want bundle+0x10 = 00C0FFEE)", ctx.r3.u32);
  check(ctx.r3.u32 == 0x00C0FFEEu, buf);
  // G4, gate off. 156b6f47 (iter-467 r1 step 2: pristine XenonRecomp output) deleted the GuestClockMs splice
  // (docs/SPLICE_CONVERSION.md: GuestClockMs_KeTimeStampBundle_sub_824680C0 DEAD -> DELETE), so sub_824680C0 runs
  // its original body with either gate - lis/lwz/lwz, r3 = *(*(0x820007D0)+0x10) - and with SR_TIMEBASE_50MHZ=0 the
  // legacy bundle writer supplies the ms there. There is no steady_clock-from-first-call path any more (the old G4
  // asserted one and failed with 00C0FFEE twice). The bundle unpublished too, two cell values: r3 follows the cell.
  TB::reinitForTest(false);
  TB::setBundlePublished(false);
  const uint32_t g4want[2] = {0x00012345u, 0x0BADF00Du};
  uint32_t g4[2] = {};
  for (int i = 0; i < 2; i++) {
    K::writeGuestBE32(TB::kBundleVA + 0x10, g4want[i]);
    ctx = PPCContext{};
    sub_824680C0(ctx, base);
    g4[i] = ctx.r3.u32;
  }
  std::snprintf(buf, sizeof buf,
                "G4 sub_824680C0, gate off + bundle unpublished: r3 = %08X, %08X (want bundle+0x10 = %08X, %08X: the original body)",
                g4[0], g4[1], g4want[0], g4want[1]);
  check(g4[0] == g4want[0] && g4[1] == g4want[1], buf);
  ctx = PPCContext{};
  ctx.r3.u64 = kScratch;
  const uint64_t la = __rdtsc();
  sub_824680D0(ctx, base);
  const uint64_t lq = rdBE64(kScratch), lz = __rdtsc();
  check(la <= lq && lq <= lz, "G5 sub_824680D0, gate off: the raw TSC (legacy A/B unchanged)");
  K::writeGuestBE32(TB::kBundleVA + 0x10, savedMs);
  K::writeGuestBE32(0x820007D0u, savedCell);
  TB::reinitForTest(true);
}

// ------------------------------------------------------------------ X: MXCSR splices --------
constexpr uint32_t kFnVa = (uint32_t)PPC_CODE_BASE + 0x20;
constexpr uint32_t kFlush = 0x8040;       // MXCSR FTZ | DAZ
constexpr uint32_t kBase = 0x1F80, kRtz = 0x1F80 | 0x6000;
constexpr uint32_t kLuaState = 0x70410000u, kStackTop = 0x70580000u;

enum class LuaAct { SetFlushThrow, ClearFlushThrow, RtzThrow, SetFlushReturn };
LuaAct g_luaAct;
void luaProtected(PPCContext& ctx, uint8_t*) {
  switch (g_luaAct) {
    case LuaAct::SetFlushThrow:   ctx.fpscr.enableFlushMode(); break;
    case LuaAct::ClearFlushThrow: ctx.fpscr.disableFlushMode(); break;
    case LuaAct::RtzThrow:        ctx.fpscr.storeFromGuest(1); break;   // mtfsf RN=01
    case LuaAct::SetFlushReturn:  ctx.fpscr.enableFlushMode(); return;
  }
  ctx.r31.u64 = 0xBADull;   // clobber a GPR the snapshot must restore
  srGuestLongjmpTryThrow(ctx.r1.u32 + 96, 2);   // rawrunprotected's registered buffer
}

struct XResult { uint32_t live, cached, r1; };
XResult runLua(LuaAct act, uint32_t start) {
  g_luaAct = act;
  PPCFunc** table = K::g_dispatchTable;
  table[(kFnVa - PPC_CODE_BASE) >> 2] = luaProtected;
  std::memset(K::guestVAToHost(kLuaState), 0, 0x100);
  PPCContext ctx{};
  ctx.r1.u64 = kStackTop;
  ctx.r3.u64 = kLuaState;
  ctx.r4.u64 = kFnVa;
  ctx.r5.u64 = 0;
  ctx.fpscr.csr = start;
  ctx.fpscr.setcsr(start);
  sub_82762400(ctx, K::getGuestBase());
  XResult r{_mm_getcsr(), ctx.fpscr.csr, ctx.r1.u32};
  _mm_setcsr(0x1F80);
  table[(kFnVa - PPC_CODE_BASE) >> 2] = nullptr;
  return r;
}

uint32_t g_injectCsr = 0;
uint32_t ntCreateSemaphoreInject(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  _mm_setcsr(g_injectCsr);   // the inserted call leaves the host MXCSR changed
  return 0xC0000017u;          // STATUS_NO_MEMORY: no handle, the guest path carries on
}

XResult runCondvar(bool syncPath, uint32_t start, uint32_t inject) {
  g_injectCsr = inject;
  constexpr uint32_t kCv = 0x70420000u, kTimeout = 0x70420100u;
  std::memset(K::guestVAToHost(kCv), 0, 0x20);
  K::writeGuestBE32(kCv + 0, 0);                         // no semaphore yet -> autoSem
  *K::guestVAToHost(kCv + 12) = syncPath ? 1 : 0;        // +12 selects site 1 / site 2
  K::writeGuestBE32(kCv + 4, 0);                         // counter 0 -> -1 < 0 on site 1
  K::writeGuestBE32(kTimeout, 0);                        // timeout 0
  PPCContext ctx{};
  ctx.r1.u64 = kStackTop;
  ctx.r3.u64 = kCv;
  ctx.r4.u64 = kTimeout;
  ctx.fpscr.csr = start;
  ctx.fpscr.setcsr(start);
  sub_825FC1A0(ctx, K::getGuestBase());
  XResult r{_mm_getcsr(), ctx.fpscr.csr, ctx.r1.u32};
  _mm_setcsr(0x1F80);
  return r;
}

void mxcsrSpliceTests() {
  std::printf("[X] MXCSR after the spliced context restores\n");
  std::vector<PPCFunc*> own;
  if (!K::g_dispatchTable) { own.assign(K::kDispatchTableEntryCount, nullptr); K::g_dispatchTable = own.data(); }
  char buf[240];
  auto agree = [](const XResult& r) { return (r.cached | 0x1F80) == r.live; };

  XResult r = runLua(LuaAct::SetFlushThrow, kBase);
  std::snprintf(buf, sizeof buf, "X1 Lua longjmp after FTZ|DAZ on: MXCSR %04X cached %04X (want both %04X: throw-time FPSCR), r1 %08X",
                r.live, r.cached | 0x1F80, kBase | kFlush, r.r1);
  check(r.live == (kBase | kFlush) && agree(r) && r.r1 == kStackTop, buf);

  r = runLua(LuaAct::ClearFlushThrow, kBase | kFlush);
  std::snprintf(buf, sizeof buf, "X2 Lua longjmp after FTZ|DAZ off: MXCSR %04X cached %04X (want both %04X)", r.live,
                r.cached | 0x1F80, kBase);
  check(r.live == kBase && agree(r), buf);

  r = runLua(LuaAct::RtzThrow, kBase);
  std::snprintf(buf, sizeof buf, "X3 Lua longjmp after mtfsf RN=01: MXCSR %04X cached %04X (want both %04X)", r.live,
                r.cached | 0x1F80, kRtz);
  check(r.live == kRtz && agree(r), buf);

  r = runLua(LuaAct::SetFlushReturn, kBase);
  std::snprintf(buf, sizeof buf, "X4 Lua normal return (no longjmp): MXCSR %04X cached %04X (want both %04X)", r.live,
                r.cached | 0x1F80, kBase | kFlush);
  check(r.live == (kBase | kFlush) && agree(r), buf);

  K::ExportRegistry::instance().registerExport("NtCreateSemaphore", ntCreateSemaphoreInject);
  r = runCondvar(true, kBase, kBase | kFlush);
  std::snprintf(buf, sizeof buf, "X5 CONDVAR site 1 (sync), inserted call left FTZ|DAZ: MXCSR %04X cached %04X (want both %04X: pre-call)",
                r.live, r.cached | 0x1F80, kBase);
  check(r.live == kBase && agree(r) && r.r1 == kStackTop, buf);
  r = runCondvar(false, kBase | kFlush, kBase);
  std::snprintf(buf, sizeof buf, "X6 CONDVAR site 2 (noSync), inserted call cleared FTZ|DAZ: MXCSR %04X cached %04X (want both %04X)",
                r.live, r.cached | 0x1F80, kBase | kFlush);
  check(r.live == (kBase | kFlush) && agree(r) && r.r1 == kStackTop, buf);
  if (!own.empty()) K::g_dispatchTable = nullptr;
}
}  // namespace

int runTimebaseTest() {
  std::printf("[TIMEBASE_TEST] SR_TIMEBASE_50MHZ one 50 MHz guest clock + spliced MXCSR restores\n");
  K::registerStubs();
  scalingTests();
  rateTests();
  monotonicTests();
  bundleTests();
  kernelTests();
  guestTests();
  mxcsrSpliceTests();
  std::printf("[TIMEBASE_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
