// iter-467 NATIVE strand A: the native executor's gates, CHECK and counters. Contract: native_exec.hpp.
// The executor thread itself lives beside the drain it runs (gfx/null_renderer.cpp).
// C1 (docs/RELEASE_COMPOSITION.md 2.2): this unit is out of the release configuration (CMakeLists.txt
// EMULATION_SOURCES); compiled with SR_ORACLE=0 it is empty, so a build that links it anyway (the composition
// audit's mutation) carries no bodies that could shadow the header's inline no-ops.
#include "native_exec.hpp"

#if SR_ORACLE

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "ab_toggle.hpp"
#include "guest_context.hpp"
#include "native_flush.hpp"
#include "phys_window.hpp"
#include "../app/log.hpp"
#include "../gfx/pm4_live_trace.hpp"
#include "../gfx/xenos_state.hpp"
#define XXH_INLINE_ALL
#include "../gfx/thirdparty/xxhash.h"

namespace sr::native::exec {

namespace {

bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
const bool g_gate = envOn("SR_NATIVE_EXEC");
const bool g_check = envOn("SR_NATIVE_EXEC_CHECK");
const uint32_t g_every = [] {
  const char* e = std::getenv("SR_NATIVE_EXEC_CHECK_EVERY");
  return e && *e ? std::max<uint32_t>(1u, uint32_t(std::strtoul(e, nullptr, 0))) : 8u;
}();
const uint32_t g_lag = [] {
  const char* e = std::getenv("SR_NATIVE_EXEC_LAG_US");
  return (e && *e) ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u;
}();

std::atomic<uint64_t> g_effects[3], g_stores{0}, g_syncs{0}, g_syncUs{0}, g_syncTimeouts{0}, g_drainCalls{0},
    g_drainItems{0};
std::atomic<uint64_t> g_ckEffects{0}, g_ckEffectBad{0}, g_ckDraws{0}, g_ckCb{0}, g_ckBad{0}, g_ckBadCb{0},
    g_ckSkipped{0}, g_ckBetween{0}, g_ckTex{0}, g_ckBadTex{0};
std::atomic<uint32_t> g_examples{0};

struct Range {
  uint32_t phys, len;
};
// Every range a draw's drain reads that the check covers: the index buffer and the used vertex fetch
// constants' buffers, from the command's own snapshot (push and execute see the same ranges). Each is
// capped at its first 64 KB. cb = a range touches a command-buffer page (physwin, the CP's IB windows).
uint32_t rangesOf(const sr::gfx::GuestDrawCommand& c, Range* r, bool& cb) {
  uint32_t n = 0;
  cb = false;
  auto add = [&](uint32_t phys, uint32_t len) {
    phys &= 0x1FFFFFFFu;
    len = std::min<uint32_t>(len, 64u << 10);
    if (!len || uint64_t(phys) + len > 0x20000000ull || n >= 97) return;
    if (sr::kernel::physwin::commandBufferWindow(phys, len)) cb = true;
    r[n++] = Range{phys, len};
  };
  if (c.idxBasePhys) add(c.idxBasePhys, c.vertexCount * (c.idxFmt32 ? 4u : 2u));
  for (uint32_t s = 0; s < 96; s++) {
    if (!((c.execChkFetch[s >> 5] >> (s & 31)) & 1u)) continue;
    const uint32_t d0 = c.fetchRaw[2 * s], d1 = c.fetchRaw[2 * s + 1];
    if ((d0 & 3u) == 3u) add(((d0 >> 2) & 0x3FFFFFFFu) * 4u, ((d1 >> 2) & 0xFFFFFFu) * 4u);
  }
  return n;
}

// The draw's textures: every texture fetch constant (type 2) with a base address, its first 16 KB
// (where a rewrite of a dynamic texture / movie plane / render-to-texture source shows first).
uint32_t texRangesOf(const sr::gfx::GuestDrawCommand& c, Range* r) {
  uint32_t n = 0;
  for (uint32_t g = 0; g < 32; g++) {
    const uint32_t d0 = c.fetchRaw[6 * g], d1 = c.fetchRaw[6 * g + 1];
    const uint32_t base = d1 & 0x1FFFF000u;
    if ((d0 & 3u) != 2u || !base) continue;
    if (uint64_t(base) + (16u << 10) > 0x20000000ull) continue;
    r[n++] = Range{base, 16u << 10};
  }
  return n;
}

// The bytes as the GPU sees them: the owning window's host view (physwin; 0xA when ambiguous, as
// physToHost). false = a range is not one contiguous host span (not hashed).
bool hashRanges(const Range* r, uint32_t n, uint64_t& out) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < n; i++) {
    const int w = sr::kernel::physwin::gpuWindowRange(r[i].phys, r[i].len);
    const uint32_t base = (w == 8) ? 0x80000000u : 0xA0000000u;
    const uint8_t* h0 = sr::kernel::guestVAToHost(base + r[i].phys);
    const uint8_t* h1 = sr::kernel::guestVAToHost(base + r[i].phys + r[i].len - 1u);
    if (!h0 || h1 - h0 != ptrdiff_t(r[i].len) - 1) return false;
    h = XXH3_64bits_withSeed(h0, r[i].len, h);  // chained: the ranges in order
  }
  out = h;
  return n != 0;
}

}  // namespace

thread_local bool t_executor = false;
void markExecutorThread() { t_executor = true; }
bool onExecutorThread() { return t_executor; }

bool gateOn() { return g_gate; }
bool checkOn() { return g_check; }
uint32_t lagUs() { return g_check ? g_lag : 0u; }

void lagSpin() {
  const uint32_t us = lagUs();
  if (!us) return;
  static const double ticksPerUs = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart) / 1e6;
  }();
  LARGE_INTEGER t0, t;
  QueryPerformanceCounter(&t0);
  const int64_t end = t0.QuadPart + int64_t(double(us) * ticksPerUs);
  do {
    _mm_pause();
    QueryPerformanceCounter(&t);
  } while (t.QuadPart < end);
}

bool deferring() {
  if (!g_gate) return false;
  static const int s_ab = sr::abt::reg("SR_NATIVE_EXEC");
  if (s_ab >= 0 && !sr::abt::on(s_ab)) return false;
  return sr::native::flush::on() && !sr::gfx::livetrace::armed();
}

bool frontend() {
  static const bool v = envOn("SR_NATIVE_EXEC_FRONTEND");
  return v && deferring();
}

void chkAtPush(sr::gfx::GuestDrawCommand& c, const uint32_t usedFetch[3]) {
  if (!g_check) return;
  std::memcpy(c.execChkFetch, usedFetch, sizeof(c.execChkFetch));
  thread_local uint32_t t_n = 0;
  Range r[97];
  bool cb = false;
  const uint32_t n = rangesOf(c, r, cb);
  const bool sampled = (++t_n % g_every) == 0;
  if (!sampled && !cb) return;
  uint64_t h = 0;
  if (!hashRanges(r, n, h)) return;
  c.execChk = cb ? 2u : 1u;
  c.execChkHash = h;
  Range t[32];
  const uint32_t nt = texRangesOf(c, t);
  uint64_t ht = 0;
  c.execChkTex = (nt && hashRanges(t, nt, ht)) ? (ht | 1ull) : 0ull;  // 0 = no texture hashed
}

void chkAtExecute(const sr::gfx::GuestDrawCommand& c, uint64_t itemsBetween) {
  if (!g_check || !c.execChk) return;
  Range r[97];
  bool cb = false;
  const uint32_t n = rangesOf(c, r, cb);
  uint64_t h = 0;
  g_ckDraws.fetch_add(1, std::memory_order_relaxed);
  if (c.execChk == 2u) g_ckCb.fetch_add(1, std::memory_order_relaxed);
  g_ckBetween.fetch_add(itemsBetween, std::memory_order_relaxed);
  if (!hashRanges(r, n, h)) {
    g_ckSkipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (c.execChkTex) {
    Range t[32];
    const uint32_t nt = texRangesOf(c, t);
    uint64_t ht = 0;
    g_ckTex.fetch_add(1, std::memory_order_relaxed);
    if (nt && hashRanges(t, nt, ht) && (ht | 1ull) != c.execChkTex) {
      g_ckBadTex.fetch_add(1, std::memory_order_relaxed);
      if (g_examples.fetch_add(1, std::memory_order_relaxed) < 24)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[NATIVE_EXEC] BAD texture changed between push and execute: draw#%u tex0=%08X prim=%u items "
                    "between=%llu",
                    c.drawOrdinal, nt ? t[0].phys : 0u, c.primType, (unsigned long long)itemsBetween);
    }
  }
  if (h == c.execChkHash) return;
  g_ckBad.fetch_add(1, std::memory_order_relaxed);
  if (c.execChk == 2u) g_ckBadCb.fetch_add(1, std::memory_order_relaxed);
  if (g_examples.fetch_add(1, std::memory_order_relaxed) < 24)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NATIVE_EXEC] BAD data changed between push and execute: draw#%u class=%s idx=%08X prim=%u "
                "items between=%llu",
                c.drawOrdinal, c.execChk == 2u ? "cb" : "vb/ib", c.idxBasePhys, c.primType,
                (unsigned long long)itemsBetween);
}

void chkEffect(uint64_t seq, uint64_t consumed, uint8_t kind) {
  if (!g_check) return;
  g_ckEffects.fetch_add(1, std::memory_order_relaxed);
  if (seq == consumed) return;
  g_ckEffectBad.fetch_add(1, std::memory_order_relaxed);
  if (g_examples.fetch_add(1, std::memory_order_relaxed) < 24)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NATIVE_EXEC] BAD effect kind=%u pushed as item %llu executed with %llu items consumed", kind,
                (unsigned long long)seq, (unsigned long long)consumed);
}

void noteEffectPushed(uint8_t kind, uint32_t stores) {
  if (kind < 3) g_effects[kind].fetch_add(1, std::memory_order_relaxed);
  g_stores.fetch_add(stores, std::memory_order_relaxed);
}

thread_local uint32_t t_reason = 0;
std::atomic<uint64_t> g_syncBy[4], g_syncUsBy[4];
void syncReason(uint32_t r) { t_reason = r < 4 ? r : 0; }

void noteSync(uint64_t waitUs, bool timedOut) {
  g_syncs.fetch_add(1, std::memory_order_relaxed);
  g_syncBy[t_reason].fetch_add(1, std::memory_order_relaxed);
  g_syncUsBy[t_reason].fetch_add(waitUs, std::memory_order_relaxed);
  t_reason = 0;
  g_syncUs.fetch_add(waitUs, std::memory_order_relaxed);
  if (timedOut) g_syncTimeouts.fetch_add(1, std::memory_order_relaxed);
}

void noteDrainCall(uint32_t items) {
  g_drainCalls.fetch_add(1, std::memory_order_relaxed);
  g_drainItems.fetch_add(items, std::memory_order_relaxed);
}

// ---- SR_NATIVE_EXEC_PROF ----
const bool g_prof = envOn("SR_NATIVE_EXEC_PROF");
std::atomic<uint64_t> g_pBusy{0}, g_pLock{0}, g_pSpin{0}, g_pSleep{0}, g_pSpinHit{0}, g_pSleeps{0}, g_pIters{0},
    g_pRes{0}, g_pResN{0};
std::atomic<uint64_t> g_pDepth[6];
struct Topo {
  int core[64];
  int l3[64];
  int nCore = 0, nL3 = 0;
};
const Topo& topo() {
  static const Topo t = [] {
    Topo r;
    for (int i = 0; i < 64; i++) r.core[i] = r.l3[i] = -1;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    std::unique_ptr<uint8_t[]> buf(new uint8_t[len ? len : 1]);
    if (len &&
        GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.get(), &len)) {
      for (DWORD o = 0; o < len;) {
        auto* p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.get() + o);
        if (p->Relationship == RelationProcessorCore && p->Processor.GroupCount &&
            p->Processor.GroupMask[0].Group == 0) {
          for (int i = 0; i < 64; i++)
            if (p->Processor.GroupMask[0].Mask & (KAFFINITY(1) << i)) r.core[i] = r.nCore;
          r.nCore++;
        } else if (p->Relationship == RelationCache && p->Cache.Level == 3 && p->Cache.GroupMask.Group == 0) {
          for (int i = 0; i < 64; i++)
            if (p->Cache.GroupMask.Mask & (KAFFINITY(1) << i)) r.l3[i] = r.nL3;
          r.nL3++;
        }
        o += p->Size;
      }
    }
    return r;
  }();
  return t;
}
constexpr int kRoles = 4;
std::atomic<uint32_t> g_pLast[kRoles];        // last logical CPU + 1
std::atomic<uint64_t> g_pCpu[2][kRoles][64];  // [deferring][role][cpu]
std::atomic<uint64_t> g_pN[2][kRoles], g_pSameCore[2][kRoles], g_pSameL3[2][kRoles];  // vs the guest swap thread
uint64_t qpcFreq() {
  static const uint64_t f = [] {
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    return uint64_t(q.QuadPart);
  }();
  return f;
}
double tms(uint64_t t) { return double(t) * 1000.0 / double(qpcFreq()); }
void profReport() {
  const Topo& t = topo();
  char line[2400];
  int o = std::snprintf(
      line, sizeof(line),
      "[NATIVE_EXEC_PROF] exec iters=%llu busy=%.1fms lock=%.1fms spin=%.1fms sleep=%.1fms spinHits=%llu sleeps=%llu "
      "| depth 1:%llu 2-7:%llu 8-31:%llu 32-127:%llu 128-511:%llu 512+:%llu | reserveWait=%.1fms n=%llu | syncMs "
      "wait=%.1f intr=%.1f cond=%.1f other=%.1f | topo cores=%d l3=%d",
      (unsigned long long)g_pIters.load(), tms(g_pBusy.load()), tms(g_pLock.load()), tms(g_pSpin.load()),
      tms(g_pSleep.load()), (unsigned long long)g_pSpinHit.load(), (unsigned long long)g_pSleeps.load(),
      (unsigned long long)g_pDepth[0].load(), (unsigned long long)g_pDepth[1].load(),
      (unsigned long long)g_pDepth[2].load(), (unsigned long long)g_pDepth[3].load(),
      (unsigned long long)g_pDepth[4].load(), (unsigned long long)g_pDepth[5].load(), tms(g_pRes.load()),
      (unsigned long long)g_pResN.load(), double(g_syncUsBy[1].load()) / 1000.0,
      double(g_syncUsBy[2].load()) / 1000.0, double(g_syncUsBy[3].load()) / 1000.0,
      double(g_syncUsBy[0].load()) / 1000.0, t.nCore, t.nL3);
  static const char* const kName[kRoles] = {"cp", "exec", "guest", "render"};
  for (int d = 0; d < 2; d++) {
    for (int r = 0; r < kRoles; r++) {
      const uint64_t n = g_pN[d][r].load();
      uint64_t tot = 0;
      for (int c = 0; c < 64; c++) tot += g_pCpu[d][r][c].load();
      if (!tot) continue;
      o += std::snprintf(line + o, sizeof(line) - size_t(o), " | %s %s n=%llu", d ? "ON" : "OFF", kName[r],
                         (unsigned long long)tot);
      if (r != kPlGuestSwap && n)
        o += std::snprintf(line + o, sizeof(line) - size_t(o), " sameCoreAsGuest=%.1f%% sameL3=%.1f%%",
                           100.0 * double(g_pSameCore[d][r].load()) / double(n),
                           100.0 * double(g_pSameL3[d][r].load()) / double(n));
      bool used[64] = {};
      o += std::snprintf(line + o, sizeof(line) - size_t(o), " top");
      for (int k = 0; k < 4; k++) {
        int best = -1;
        uint64_t bv = 0;
        for (int c = 0; c < 64; c++)
          if (!used[c] && g_pCpu[d][r][c].load() > bv) bv = g_pCpu[d][r][c].load(), best = c;
        if (best < 0) break;
        used[best] = true;
        o += std::snprintf(line + o, sizeof(line) - size_t(o), " %d(c%d,L%d)=%.0f%%", best, t.core[best],
                           t.l3[best], 100.0 * double(bv) / double(tot));
      }
      if (o > int(sizeof(line)) - 200) break;
    }
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "%s", line);
}

void report(const char* why) {
  if (!g_gate) return;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_EXEC] %s effects stores=%llu (items %llu) swaps=%llu | syncs=%llu (wait %llu intr %llu cond %llu other %llu) "
              "wait=%.1fms timeouts=%llu | "
              "executor drains=%llu items=%llu | CHECK effects=%llu bad=%llu draws=%llu cb=%llu BAD=%llu badCb=%llu "
              "unhashable=%llu between=%.1f lagUs=%u | textures checked=%llu BADTEX=%llu",
              why, (unsigned long long)g_stores.load(), (unsigned long long)g_effects[kStores].load(),
              (unsigned long long)g_effects[kSwap].load(), (unsigned long long)g_syncs.load(),
              (unsigned long long)g_syncBy[1].load(), (unsigned long long)g_syncBy[2].load(),
              (unsigned long long)g_syncBy[3].load(), (unsigned long long)g_syncBy[0].load(),
              double(g_syncUs.load()) / 1000.0, (unsigned long long)g_syncTimeouts.load(),
              (unsigned long long)g_drainCalls.load(), (unsigned long long)g_drainItems.load(),
              (unsigned long long)g_ckEffects.load(), (unsigned long long)g_ckEffectBad.load(),
              (unsigned long long)g_ckDraws.load(), (unsigned long long)g_ckCb.load(), (unsigned long long)g_ckBad.load(),
              (unsigned long long)g_ckBadCb.load(), (unsigned long long)g_ckSkipped.load(),
              g_ckDraws.load() ? double(g_ckBetween.load()) / double(g_ckDraws.load()) : 0.0, lagUs(),
              (unsigned long long)g_ckTex.load(), (unsigned long long)g_ckBadTex.load());
}

bool profOn() { return g_prof; }
uint64_t profNow() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return uint64_t(q.QuadPart);
}
void profExecIter(uint64_t lockTicks, uint64_t busyTicks, uint32_t depth) {
  g_pIters.fetch_add(1, std::memory_order_relaxed);
  g_pLock.fetch_add(lockTicks, std::memory_order_relaxed);
  g_pBusy.fetch_add(busyTicks, std::memory_order_relaxed);
  const int b = depth <= 1 ? 0 : depth < 8 ? 1 : depth < 32 ? 2 : depth < 128 ? 3 : depth < 512 ? 4 : 5;
  g_pDepth[b].fetch_add(1, std::memory_order_relaxed);
}
void profExecIdle(uint64_t spinTicks, uint64_t sleepTicks, bool spinHit) {
  g_pSpin.fetch_add(spinTicks, std::memory_order_relaxed);
  g_pSleep.fetch_add(sleepTicks, std::memory_order_relaxed);
  (spinHit ? g_pSpinHit : g_pSleeps).fetch_add(1, std::memory_order_relaxed);
}
void profReserveWait(uint64_t ticks) {
  g_pRes.fetch_add(ticks, std::memory_order_relaxed);
  g_pResN.fetch_add(1, std::memory_order_relaxed);
}
void profPlace(int role) {
  if (!g_prof || role < 0 || role >= kRoles) return;
  const uint32_t cpu = GetCurrentProcessorNumber() & 63u;
  const int d = deferring() ? 1 : 0;
  g_pCpu[d][role][cpu].fetch_add(1, std::memory_order_relaxed);
  g_pLast[role].store(cpu + 1u, std::memory_order_relaxed);
  if (role != kPlGuestSwap) {
    const uint32_t g = g_pLast[kPlGuestSwap].load(std::memory_order_relaxed);
    if (g) {
      const Topo& t = topo();
      g_pN[d][role].fetch_add(1, std::memory_order_relaxed);
      if (t.core[cpu] >= 0 && t.core[cpu] == t.core[g - 1])
        g_pSameCore[d][role].fetch_add(1, std::memory_order_relaxed);
      if (t.l3[cpu] >= 0 && t.l3[cpu] == t.l3[g - 1]) g_pSameL3[d][role].fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    static std::atomic<uint64_t> s_n{0};
    if ((s_n.fetch_add(1, std::memory_order_relaxed) + 1) % 2048u == 0) profReport();
  }
}

}  // namespace sr::native::exec

#endif  // SR_ORACLE
