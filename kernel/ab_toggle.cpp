// iter-467 guestcode: in-roll A/B toggling - see ab_toggle.hpp.
#include "ab_toggle.hpp"

#include "highfps.hpp"
#include "../app/log.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sr::abt {

std::atomic<uint32_t> g_mask{0};

namespace {

struct Cfg {
  std::vector<std::string> names;
  uint64_t seed = 1;
};

uint64_t mix(uint64_t x) {  // splitmix64
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

const Cfg& cfg() {
  static const Cfg c = [] {
    Cfg r;
    const char* e = std::getenv("SR_AB_TOGGLE");
    if (e && *e) {
      std::string s(e);
      size_t a = 0;
      while (a <= s.size() && r.names.size() < 32) {
        size_t b = s.find_first_of(",;+ ", a);  // ';' / '+' too: perf_run --gates splits on ',
        if (b == std::string::npos) b = s.size();
        if (b > a) r.names.push_back(s.substr(a, b - a));
        a = b + 1;
      }
    }
    const char* sd = std::getenv("SR_AB_SEED");
    if (sd && *sd) r.seed = std::strtoull(sd, nullptr, 0);
    return r;
  }();
  return c;
}

// The schedule: window w is position (w & 1) of block w >> 1; feature i is ON in the position its
// hash bit names.
uint32_t maskFor(uint64_t w) {
  const Cfg& c = cfg();
  uint32_t m = 0;
  for (size_t i = 0; i < c.names.size(); i++) {
    const uint64_t pick = mix(c.seed * 0x100000001B3ull + (w >> 1) * 64u + i) & 1u;
    if ((w & 1u) == pick) m |= 1u << i;
  }
  return m;
}

std::once_flag g_logOnce;

void logConfig() {
  const Cfg& c = cfg();
  std::string list;
  for (size_t i = 0; i < c.names.size(); i++) list += " bit" + std::to_string(i) + "=" + c.names[i];
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[AB_TOGGLE] in-roll A/B: %zu feature(s)%s, block-paired per [HIGHFPS_PACE] window, seed=%llu",
              c.names.size(), list.c_str(), (unsigned long long)c.seed);
}

}  // namespace

bool active() { return !cfg().names.empty(); }

int reg(const char* name) {
  const Cfg& c = cfg();
  if (c.names.empty() || !name) return -1;
  std::call_once(g_logOnce, [] {
    g_mask.store(maskFor(1), std::memory_order_relaxed);  // the first pace window is w=1
    logConfig();
  });
  for (size_t i = 0; i < c.names.size(); i++)
    if (c.names[i] == name) return int(i);
  return -1;
}

static bool presentMode() {
  static const bool v = [] {
    const char* e = std::getenv("SR_AB_PRESENT");
    return e && e[0] == '1';
  }();
  return v;
}

uint32_t onWindowClose(uint64_t closed) {
  if (!active()) return 0;
  std::call_once(g_logOnce, [] {
    g_mask.store(maskFor(1), std::memory_order_relaxed);  // the first pace window is w=1
    logConfig();
  });
  const uint32_t held = g_mask.load(std::memory_order_relaxed);
  if (!presentMode()) g_mask.store(maskFor(closed + 1), std::memory_order_relaxed);
  return held;
}

bool drainedMode() {
  static const bool v = [] {
    const char* e = std::getenv("SR_AB_PRESENT_DRAINED");
    const bool on = e && e[0] == '1' && presentMode() && active();
    if (on)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[AB_DRAIN] SR_AB_PRESENT_DRAINED: the per-present flip commits after the next "
                                         "quiesced close (diagnostic)");
    return on;
  }();
  return v;
}

namespace {
// Drained mode only: the pending flip (bit 31 = pending) and the swap it belongs to.
std::atomic<uint32_t> g_pending{0};
std::atomic<uint64_t> g_pendingSwap{0};
struct DrainStats {
  std::mutex mu;
  uint64_t commits = 0, postponed = 0, overwritten = 0, pinned = 0;
  double waitSum = 0, waitMax = 0;
} g_drain;
}  // namespace

bool pendingFlip() { return (g_pending.load(std::memory_order_acquire) & 0x80000000u) != 0; }

void commitPending(uint64_t closeNo, double waitedUs) {
  const uint32_t p = g_pending.exchange(0, std::memory_order_acq_rel);
  if (!(p & 0x80000000u)) return;
  const uint32_t m = p & 0x7FFFFFFFu;
  g_mask.store(m, std::memory_order_relaxed);
  const uint64_t n = g_pendingSwap.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(g_drain.mu);
  g_drain.commits++;
  g_drain.waitSum += waitedUs;
  if (waitedUs > g_drain.waitMax) g_drain.waitMax = waitedUs;
  if (n <= 16 || sr::kernel::highfps::freezeHeldMs() > 0.0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[AB_DRAIN] swap=%llu ab=0x%X committed after close %llu, waited %.0f us",
                (unsigned long long)n, m, (unsigned long long)closeNo, waitedUs);
  if ((g_drain.commits & 1023u) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[AB_DRAIN] stats commits=%llu postponed=%llu overwritten=%llu pinned=%llu "
                "wait mean %.0f us max %.0f us", (unsigned long long)g_drain.commits,
                (unsigned long long)g_drain.postponed, (unsigned long long)g_drain.overwritten,
                (unsigned long long)g_drain.pinned, g_drain.waitSum / double(g_drain.commits), g_drain.waitMax);
}

void postponePending(const char* why) {
  std::lock_guard<std::mutex> lk(g_drain.mu);
  if (why && why[0] == 'p') g_drain.pinned++;
  else g_drain.postponed++;
  const uint64_t k = g_drain.postponed + g_drain.pinned;
  if (k <= 16 || (k & (k - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[AB_DRAIN] swap=%llu flip postponed (%s) - pinned %llu, not drained %llu",
                (unsigned long long)g_pendingSwap.load(std::memory_order_relaxed), why ? why : "?",
                (unsigned long long)g_drain.pinned, (unsigned long long)g_drain.postponed);
}

// tools/graceful-quit (kernel/quit.hpp): the stats line with a why token - the terminal snapshot's whole-run twin of
// the every-1024-commits "[AB_DRAIN] stats" line (drained mode only).
void drainStatsLine(const char* why) {
  if (!drainedMode()) return;
  std::lock_guard<std::mutex> lk(g_drain.mu);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[AB_DRAIN] %s stats commits=%llu postponed=%llu overwritten=%llu pinned=%llu "
              "wait mean %.0f us max %.0f us", why, (unsigned long long)g_drain.commits,
              (unsigned long long)g_drain.postponed, (unsigned long long)g_drain.overwritten,
              (unsigned long long)g_drain.pinned, g_drain.commits ? g_drain.waitSum / double(g_drain.commits) : 0.0,
              g_drain.waitMax);
}

void onGuestSwap() {
  if (!active() || !presentMode()) return;
  std::call_once(g_logOnce, [] {
    g_mask.store(maskFor(1), std::memory_order_relaxed);
    logConfig();
  });
  static uint64_t s_n = 0;  // the presenting thread only
  const uint64_t n = ++s_n;
  const uint32_t all = uint32_t((1ull << cfg().names.size()) - 1ull);
  const uint32_t m = (n & 1ull) ? 0u : all;
  if (drainedMode()) {  // SR_AB_PRESENT_DRAINED: pending until the close wrapper finds the stream quiesced
    g_pendingSwap.store(n, std::memory_order_relaxed);
    if (g_pending.exchange(m | 0x80000000u, std::memory_order_acq_rel) & 0x80000000u) {
      std::lock_guard<std::mutex> lk(g_drain.mu);
      g_drain.overwritten++;  // the previous present's flip never committed: that present ran on the old mask
    }
    if (n <= 16 || sr::kernel::highfps::freezeHeldMs() > 0.0)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[AB_PRESENT] swap=%llu ab=0x%X (pending, drained)", (unsigned long long)n, m);
    return;
  }
  g_mask.store(m, std::memory_order_relaxed);
  if (n <= 16 || sr::kernel::highfps::freezeHeldMs() > 0.0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[AB_PRESENT] swap=%llu ab=0x%X", (unsigned long long)n, m);
}

}  // namespace sr::abt
