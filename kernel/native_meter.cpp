// iter-467 NATIVE METER - contract: native_meter.hpp.
#include "native_meter.hpp"

#include "../app/log.hpp"

#if __has_include("audio_native_census.hpp")
#include "audio_native_census.hpp"
#define SR_METER_HAS_AUDIO 1
#else
#define SR_METER_HAS_AUDIO 0
#endif

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace sr::native::meter {

std::atomic<uint64_t> g_count[kIds];
std::atomic<uint64_t> g_kind[kKinds];

namespace {

// Columns: the pushed ids in Id order, then xma (pulled from the audio census).
constexpr int kCols = int(kIds) + 1;
constexpr int kXma = int(kIds);
const char* const kName[kCols] = {"pm4", "edram_res", "edram_xfer", "edram_load", "nrec", "dispreg", "shm", "xma"};
static_assert(kDisplayReg == 5, "the dispreg column sits between nrec and shm in the log line");
static_assert(kShm == 6, "the shm column (the shared-memory page model's validations) sits between dispreg and xma");
const char* const kKindName[kKinds] = {"t0",    "t1",    "t2nop", "drawDma", "drawAuto", "drawImm", "shader",
                                       "const", "ib",    "sync",  "event",   "bin",      "nop3",    "other"};
void kindSnap(uint64_t out[kKinds]) {
  for (int i = 0; i < int(kKinds); ++i) out[i] = g_kind[i].load(std::memory_order_relaxed);
}
// " t0=12.3 t1=0.0 ..." per frame over [a, b) frames
size_t kindsFmt(char* buf, size_t n, const uint64_t* a, const uint64_t* b, uint64_t frames) {
  size_t o = 0;
  for (int i = 0; i < int(kKinds) && o < n; ++i)
    o += std::snprintf(buf + o, n - o, " %s=%.1f", kKindName[i], frames ? double(b[i] - a[i]) / double(frames) : 0.0);
  return o;
}

// pm4 / nrec: fed by this lane's CP hooks; dispreg: fed by kernel/stubs.cpp (the display stubs are
// unconditional code in every build, so the column is live from the first frame); shm: fed by
// gfx/d3d12_pm4_shmem.cpp's page validations (unconditional code in every build that has this meter).
std::atomic<uint32_t> g_wired{(1u << kPm4) | (1u << kNativeRec) | (1u << kDisplayReg) | (1u << kShm)};

bool enabled() {
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_METER");
    return !e || e[0] != '0';
  }();
  return on;
}

bool windowLines() {
  static const bool on = [] {
    const char* m = std::getenv("SR_NATIVE_METER");
    const char* p = std::getenv("SR_HIGHFPS_PACE");
    return (m && m[0] == '1') || (p && p[0] && p[0] != '0');
  }();
  return on;
}

bool wired(int c) {
  if (c == kXma) return SR_METER_HAS_AUDIO != 0;
  return (g_wired.load(std::memory_order_relaxed) >> c) & 1u;
}

void snapshot(uint64_t out[kCols]) {
  for (int i = 0; i < int(kIds); ++i) out[i] = g_count[i].load(std::memory_order_relaxed);
#if SR_METER_HAS_AUDIO
  const sr::kernel::AudioNativeCensus a = sr::kernel::audioNativeCensus();
  out[kXma] = a.guest_mmio_fns + a.xma_regfile_writes;
#else
  out[kXma] = 0;
#endif
}

int64_t qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
double qpcSec(int64_t d) {
  static const double f = [] {
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    return double(q.QuadPart);
  }();
  return double(d) / f;
}

struct State {
  std::mutex mu;
  bool init = false;
  int64_t t0 = 0, winStart = 0, lastRun = 0;
  uint64_t frames = 0, winFrames = 0, window = 0;
  uint64_t prev[kCols] = {}, winStartVal[kCols] = {}, winMax[kCols] = {};
  uint64_t kindStart[kKinds] = {}, kindWin[kKinds] = {};
};
State& st() {
  static State s;
  return s;
}

void fmtVal(char* b, size_t n, int c, uint64_t v) {
  if (wired(c)) std::snprintf(b, n, "%llu", (unsigned long long)v);
  else std::snprintf(b, n, "-");
}

void runLineLocked(State& s, const char* why) {
  uint64_t cur[kCols];
  snapshot(cur);
  char buf[640];
  size_t o = 0;
  o += std::snprintf(buf + o, sizeof(buf) - o, "%s %.1fs frames=%llu |", why, qpcSec(qpc() - s.t0),
                     (unsigned long long)s.frames);
  for (int c = 0; c < kCols && o < sizeof(buf); ++c) {
    char v[32];
    fmtVal(v, sizeof v, c, cur[c]);
    o += std::snprintf(buf + o, sizeof(buf) - o, " %s=%s", kName[c], v);
  }
  if (o < sizeof(buf)) o += std::snprintf(buf + o, sizeof(buf) - o, " | per_frame");
  for (int c = 0; c < kCols && o < sizeof(buf); ++c) {
    if (wired(c) && s.frames)
      o += std::snprintf(buf + o, sizeof(buf) - o, " %s=%.1f", kName[c], double(cur[c]) / double(s.frames));
    else
      o += std::snprintf(buf + o, sizeof(buf) - o, " %s=-", kName[c]);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_METER_RUN] %s", buf);
  uint64_t k[kKinds];
  kindSnap(k);
  char kb[512];
  kindsFmt(kb, sizeof kb, s.kindStart, k, s.frames);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_METER_KINDS] run %s frames=%llu | pm4 per_frame by kind:%s", why,
              (unsigned long long)s.frames, kb);
}

void atExit() { report("exit"); }

}  // namespace

void wire(Id id) noexcept {
  if (id < kIds) g_wired.fetch_or(1u << id, std::memory_order_relaxed);
}

int columnCount() noexcept { return kCols; }
const char* columnName(int c) noexcept { return (c >= 0 && c < kCols) ? kName[c] : ""; }
bool columnWired(int c) noexcept { return c >= 0 && c < kCols && wired(c); }
void snapshotColumns(uint64_t* out, int n) noexcept {
  uint64_t cur[kCols];
  snapshot(cur);
  for (int c = 0; c < n && c < kCols; ++c) out[c] = cur[c];
}

void report(const char* why) noexcept {
  if (!enabled()) return;
  State& s = st();
  // try_lock: at exit another (terminated) thread may hold it; skip rather than hang.
  std::unique_lock<std::mutex> lk(s.mu, std::try_to_lock);
  if (!lk.owns_lock() || !s.init) return;
  runLineLocked(s, why);
}

void onFrame() noexcept {
  if (!enabled()) return;
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  const int64_t now = qpc();
  uint64_t cur[kCols];
  snapshot(cur);
  if (!s.init) {
    s.init = true;
    s.t0 = s.winStart = s.lastRun = now;
    for (int c = 0; c < kCols; ++c) s.prev[c] = s.winStartVal[c] = cur[c];
    kindSnap(s.kindStart);
    std::memcpy(s.kindWin, s.kindStart, sizeof(s.kindWin));
    std::atexit(atExit);
    return;
  }
  s.frames++;
  s.winFrames++;
  for (int c = 0; c < kCols; ++c) {
    const uint64_t d = cur[c] - s.prev[c];
    if (d > s.winMax[c]) s.winMax[c] = d;
    s.prev[c] = cur[c];
  }
  const double winS = qpcSec(now - s.winStart);
  if (winS >= 2.0) {
    if (windowLines()) {
      char buf[768];
      size_t o = 0;
      o += std::snprintf(buf + o, sizeof(buf) - o, "w=%llu %.2fs frames=%llu | per_frame avg/max",
                         (unsigned long long)++s.window, winS, (unsigned long long)s.winFrames);
      for (int c = 0; c < kCols && o < sizeof(buf); ++c) {
        if (wired(c))
          o += std::snprintf(buf + o, sizeof(buf) - o, " %s=%.1f/%llu", kName[c],
                             double(cur[c] - s.winStartVal[c]) / double(s.winFrames),
                             (unsigned long long)s.winMax[c]);
        else
          o += std::snprintf(buf + o, sizeof(buf) - o, " %s=-", kName[c]);
      }
      if (o < sizeof(buf)) o += std::snprintf(buf + o, sizeof(buf) - o, " | window");
      for (int c = 0; c < kCols && o < sizeof(buf); ++c) {
        char v[32];
        fmtVal(v, sizeof v, c, cur[c] - s.winStartVal[c]);
        o += std::snprintf(buf + o, sizeof(buf) - o, " %s=%s", kName[c], v);
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_METER] %s", buf);
      uint64_t k[kKinds];
      kindSnap(k);
      char kb[512];
      kindsFmt(kb, sizeof kb, s.kindWin, k, s.winFrames);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_METER_KINDS] w=%llu frames=%llu | pm4 per_frame by kind:%s",
                  (unsigned long long)s.window, (unsigned long long)s.winFrames, kb);
    }
    kindSnap(s.kindWin);
    s.winStart = now;
    s.winFrames = 0;
    for (int c = 0; c < kCols; ++c) {
      s.winStartVal[c] = cur[c];
      s.winMax[c] = 0;
    }
  }
  if (qpcSec(now - s.lastRun) >= 30.0) {
    s.lastRun = now;
    runLineLocked(s, "periodic");
  }
}

}  // namespace sr::native::meter
