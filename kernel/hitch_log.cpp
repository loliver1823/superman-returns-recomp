// iter-466 stutter lane: the HITCH recorder (SR_HITCH_LOG=1). Contract in hitch_log.hpp.
#include "hitch_log.hpp"

#include "perf_stats.hpp"
#include "../app/log.hpp"

#include <windows.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace sr::hitch {

bool g_on = [] {
  const char* e = std::getenv("SR_HITCH_LOG");
  return e && e[0] == '1';
}();
Slot g_slot[kCount];

namespace {

const char* const kName[kCount] = {"pso",   "res",   "texload", "resolve",  "xlat",
                                   "wtprot", "wtfault", "fread",  "fwrite",   "fopen",
                                   "vmalloc", "gpuwait", "present", "rtidle", "cpwait"};

double g_usPerTick = [] {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return 1e6 / double(f.QuadPart);
}();

double ratio() {
  static const double r = [] {
    const char* e = std::getenv("SR_HITCH_RATIO");
    const double v = e ? std::atof(e) : 0.0;
    return v > 1.0 ? v : 1.5;
  }();
  return r;
}

// Guest swaps (the guest render loop's VdSwap): a small ring of QPC stamps.
constexpr uint32_t kGs = 256;
std::atomic<uint64_t> g_gsT[kGs];
std::atomic<uint32_t> g_gsN{0};

struct Snap {
  uint64_t n, bytes, us;
};

struct State {
  uint64_t lastPresent = 0;
  float ring[120] = {};
  uint32_t ringN = 0, ringPos = 0;
  Snap prev[kCount] = {};
  // [HITCH_SUM] accumulators
  uint64_t sumT0 = 0;
  uint32_t presents = 0, hitches = 0, h2 = 0, h4 = 0;  // > ratio, > 2x, > 4x median
  double hitchMs = 0, worstMs = 0;
  Snap sum0[kCount] = {};
  uint64_t sumMaxUs[kCount] = {};
  uint32_t id = 0;
};
State g_st;

uint64_t maxTake(Ev e) { return g_slot[e].maxUs.exchange(0, std::memory_order_relaxed); }

}  // namespace

uint64_t qpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(t.QuadPart);
}

void noteSlow(Ev e, uint64_t t0, uint64_t bytes) {
  const uint64_t us = uint64_t(double(qpcNow() - t0) * g_usPerTick);
  Slot& s = g_slot[e];
  s.n.fetch_add(1, std::memory_order_relaxed);
  if (bytes) s.bytes.fetch_add(bytes, std::memory_order_relaxed);
  s.us.fetch_add(us, std::memory_order_relaxed);
  uint64_t m = s.maxUs.load(std::memory_order_relaxed);
  while (us > m && !s.maxUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {
  }
}

std::atomic<uint32_t> g_swapTid{0};

uint32_t renderLoopTid() { return g_swapTid.load(std::memory_order_relaxed); }

void slowIo(uint32_t handle, uint32_t len, uint64_t off, uint64_t l0, uint64_t l1, uint64_t r0,
            uint64_t r1, void* file) {
  if (!g_on || !l0) return;
  static const double thrMs = [] {
    const char* e = std::getenv("SR_HITCH_IO_MS");
    return e ? std::atof(e) : 20.0;
  }();
  const uint64_t end = qpcNow();
  const double totalMs = double(end - l0) * g_usPerTick * 1e-3;
  if (totalMs < thrMs) return;
  const double lockMs = l1 > l0 ? double(l1 - l0) * g_usPerTick * 1e-3 : 0.0;
  const double readMs = (r0 && r1 > r0) ? double(r1 - r0) * g_usPerTick * 1e-3 : 0.0;
  char path[260] = "?";
  if (file) {
    HANDLE os = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(static_cast<std::FILE*>(file))));
    wchar_t w[260];
    const DWORD n = os != INVALID_HANDLE_VALUE ? GetFinalPathNameByHandleW(os, w, 260, 0) : 0;
    if (n && n < 260) {
      const wchar_t* base = w;
      for (const wchar_t* p = w; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
      WideCharToMultiByte(CP_UTF8, 0, base, -1, path, int(sizeof(path)), nullptr, nullptr);
    }
  }
  const uint32_t tid = GetCurrentThreadId();
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HITCH_IO] t_us=%llu tid=%u render_loop=%d handle=%u file=%s off=%llu len=%u total=%.2fms "
              "lock=%.2fms read=%.2fms",
              (unsigned long long)sr::perf::nowUs(), tid, int(tid == renderLoopTid()), handle, path,
              (unsigned long long)off, len, totalMs, lockMs, readMs);
}

void onGuestSwap() {
  if (!g_on) return;
  g_swapTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  const uint32_t i = g_gsN.load(std::memory_order_relaxed);
  g_gsT[i % kGs].store(qpcNow(), std::memory_order_relaxed);
  g_gsN.store(i + 1, std::memory_order_release);
}

void onHostPresent() {
  if (!g_on) return;
  State& st = g_st;
  const uint64_t now = qpcNow();
  Snap cur[kCount];
  for (int k = 0; k < kCount; ++k)
    cur[k] = {g_slot[k].n.load(std::memory_order_relaxed),
              g_slot[k].bytes.load(std::memory_order_relaxed),
              g_slot[k].us.load(std::memory_order_relaxed)};
  uint64_t mx[kCount];
  for (int k = 0; k < kCount; ++k) {
    mx[k] = maxTake(Ev(k));
    st.sumMaxUs[k] = std::max(st.sumMaxUs[k], mx[k]);
  }
  if (!st.sumT0) {
    st.sumT0 = now;
    std::memcpy(st.sum0, cur, sizeof(cur));
  }
  if (st.lastPresent) {
    const double ms = double(now - st.lastPresent) * g_usPerTick * 1e-3;
    float med = 0.f;
    if (st.ringN >= 30) {
      float tmp[120];
      std::memcpy(tmp, st.ring, sizeof(float) * st.ringN);
      std::nth_element(tmp, tmp + st.ringN / 2, tmp + st.ringN);
      med = tmp[st.ringN / 2];
    }
    st.presents++;
    if (med > 0.f && ms > ratio() * med) {
      st.hitches++;
      if (ms > 2.0 * med) st.h2++;
      if (ms > 4.0 * med) st.h4++;
      st.hitchMs += ms - med;
      st.worstMs = std::max(st.worstMs, ms);
      const uint32_t id = ++st.id;
      // Guest swaps inside the window, and the longest guest interval that ends inside it.
      const uint32_t gn = g_gsN.load(std::memory_order_acquire);
      uint32_t gIn = 0;
      double gMax = 0.0, gLast = 0.0;
      for (uint32_t j = gn > kGs - 1 ? gn - (kGs - 1) : 0; j < gn; ++j) {
        const uint64_t t = g_gsT[j % kGs].load(std::memory_order_relaxed);
        const uint64_t tp = j ? g_gsT[(j - 1) % kGs].load(std::memory_order_relaxed) : 0;
        if (t > st.lastPresent && t <= now) {
          gIn++;
          if (tp && t > tp) gMax = std::max(gMax, double(t - tp) * g_usPerTick * 1e-3);
        }
        if (t <= now && t > 0) gLast = double(now - t) * g_usPerTick * 1e-3;
      }
      char buf[1400];
      int o = std::snprintf(buf, sizeof(buf),
                            "#%u t_us=%llu ms=%.2f med=%.2f x%.1f | guest swaps=%u gmax=%.2f "
                            "since_last_swap=%.2f |",
                            id, (unsigned long long)sr::perf::nowUs(), ms, double(med), ms / med, gIn,
                            gMax, gLast);
      for (int k = 0; k < kCount && o > 0 && o < int(sizeof(buf)) - 64; ++k) {
        const uint64_t dn = cur[k].n - st.prev[k].n;
        if (!dn) continue;
        const uint64_t db = cur[k].bytes - st.prev[k].bytes;
        const uint64_t du = cur[k].us - st.prev[k].us;
        o += std::snprintf(buf + o, sizeof(buf) - size_t(o), " %s=%llu/%.2fms/max%.2f", kName[k],
                           (unsigned long long)dn, double(du) * 1e-3, double(mx[k]) * 1e-3);
        if (db && o < int(sizeof(buf)) - 32)
          o += std::snprintf(buf + o, sizeof(buf) - size_t(o), "/%lluKB", (unsigned long long)(db >> 10));
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[HITCH] %s", buf);
      const uint64_t t1 = sr::perf::nowUs();
      const uint64_t span = uint64_t(ms * 1000.0);
      sr::perf::hostProfileHitch(id, t1 > span ? t1 - span : 0, t1);
    }
    st.ring[st.ringPos] = float(ms);
    st.ringPos = (st.ringPos + 1) % 120u;
    if (st.ringN < 120) st.ringN++;
  }
  std::memcpy(st.prev, cur, sizeof(cur));
  st.lastPresent = now;

  const double sumMs = double(now - st.sumT0) * g_usPerTick * 1e-3;
  if (sumMs >= 10000.0) {
    char buf[1400];
    int o = std::snprintf(buf, sizeof(buf),
                          "%.1fs presents=%u hitches=%u (>2x %u, >4x %u) excess_ms=%.1f "
                          "worst=%.1f |",
                          sumMs / 1000.0, st.presents, st.hitches, st.h2, st.h4, st.hitchMs, st.worstMs);
    for (int k = 0; k < kCount && o > 0 && o < int(sizeof(buf)) - 64; ++k) {
      const uint64_t dn = cur[k].n - st.sum0[k].n;
      if (!dn) continue;
      o += std::snprintf(buf + o, sizeof(buf) - size_t(o), " %s=%llu/%.1fms/max%.2f", kName[k],
                         (unsigned long long)dn, double(cur[k].us - st.sum0[k].us) * 1e-3,
                         double(st.sumMaxUs[k]) * 1e-3);
      const uint64_t db = cur[k].bytes - st.sum0[k].bytes;
      if (db && o < int(sizeof(buf)) - 32)
        o += std::snprintf(buf + o, sizeof(buf) - size_t(o), "/%lluKB", (unsigned long long)(db >> 10));
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[HITCH_SUM] %s", buf);
    st.sumT0 = now;
    std::memcpy(st.sum0, cur, sizeof(cur));
    std::memset(st.sumMaxUs, 0, sizeof(st.sumMaxUs));
    st.presents = st.hitches = st.h2 = st.h4 = 0;
    st.hitchMs = st.worstMs = 0;
  }
}

}  // namespace sr::hitch
