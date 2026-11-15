// NATIVE mode B stage 0: the guest-visible write ledger of the XDK draw entries.
// Contract and gates: native_draw_ledger.hpp. Diagnostic, observe-only (reads guest memory).

#include "native_draw_ledger.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../app/log.hpp"
#include "guest_context.hpp"
#include "native_brackets.hpp"

namespace sr::native::ledger {

namespace {
bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
uint32_t envU(const char* n, uint32_t d) {
  const char* e = std::getenv(n);
  return e && *e ? uint32_t(std::strtoul(e, nullptr, 0)) : d;
}

constexpr uint32_t kDevSpan = 0x4000u;  // the device struct and its XDK-maintained fields
constexpr int kKinds = 6;  // 5 = the draw builder's cut extent (native_builder_census)
constexpr uint32_t kWatchTop = 0xA0000000u;  // host offsets below: every real arena page
const char* const kKindName[kKinds] = {"indexed", "auto", "UP", "begin", "end", "cut"};

const uint32_t g_every = envU("SR_NATIVE_DRAW_LEDGER_EVERY", 256u);
const uint32_t g_wwEvery = envU("SR_NATIVE_DRAW_LEDGER_WW_EVERY", 16u);  // of the samples

struct Stats {
  std::atomic<uint64_t> samples{0}, kicks{0}, kickDraws{0}, zpass{0}, wwPages{0};
  std::atomic<uint32_t> devOff[kDevSpan / 4];
};
Stats g_stats[kKinds];
std::mutex g_pageMu;
std::unordered_map<uint32_t, uint64_t> g_pages[kKinds];  // page host offset -> samples
// Control: pages first written during one back-to-back GetWriteWatch pair on the same thread
// (no draw in between) - the other threads' background rate over a scan-length window.
std::unordered_map<uint32_t, uint64_t> g_noise;
std::atomic<uint64_t> g_noiseSamples{0}, g_wwSamples[kKinds];
std::atomic<uint64_t> g_totalSamples{0};

// SR_NATIVE_DRAW_LEDGER_PAGES=va,va,... (up to 8 guest pages): byte-exact diff of those pages
// around every sampled draw call - pins which dwords the XDK body writes there (the write-watch
// pages that beat the control windows). Counts per (page, dword offset).
constexpr uint32_t kMaxWatchPages = 8;
uint32_t g_watchVA[kMaxWatchPages];
uint32_t g_nWatch = [] {
  uint32_t n = 0;
  if (const char* e = std::getenv("SR_NATIVE_DRAW_LEDGER_PAGES")) {
    const char* q = e;
    while (*q && n < kMaxWatchPages) {
      char* end = nullptr;
      const unsigned long v = std::strtoul(q, &end, 0);
      if (end == q) break;
      g_watchVA[n++] = uint32_t(v) & ~0xFFFu;
      q = (*end == ',') ? end + 1 : end;
    }
  }
  return n;
}();
std::atomic<uint32_t> g_watchHits[kMaxWatchPages][1024];
// SR_NATIVE_DRAW_LEDGER_PAGES_KIND=k: only samples of kind k diff the pinned pages (5 = the draw
// builder's cut extent); default -1 = every kind.
const int g_watchKind = [] {
  const char* e = std::getenv("SR_NATIVE_DRAW_LEDGER_PAGES_KIND");
  return (e && *e) ? std::atoi(e) : -1;
}();
bool watchKind(int kind) { return g_watchKind < 0 || g_watchKind == kind; }
std::atomic<uint64_t> g_watchSamples{0};

struct Tls {
  uint8_t pageSnap[kMaxWatchPages][4096];
  uint32_t n = 0, sampled = 0;
  bool armed = false, ww = false;
  uint32_t kicks0 = 0, cur0 = 0;
  uint8_t snap[kDevSpan];
  std::vector<PVOID> ww0, ww1, ctl;
};
// Slot 1 = the cut extent, which contains draw entries (slot 0), so the two never share a sample.
Tls& tls(int kind) {
  thread_local Tls* t[2] = {new Tls(), new Tls()};
  return *t[kind == 5 ? 1 : 0];
}

uint8_t* devHost(uint32_t dev) {
  uint8_t* gb = sr::kernel::getGuestBase();
  if (!gb || !dev) return nullptr;
  const uint32_t a0 = sr::kernel::aliasGuestVA(dev), a1 = sr::kernel::aliasGuestVA(dev + kDevSpan - 1u);
  if (a1 < a0 || a1 - a0 != kDevSpan - 1u) return nullptr;
  return gb + a0;
}

uint32_t rdBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}

// Pages written since the last write-watch reset, host offsets in [0, kWatchTop), NO reset.
bool watchPages(std::vector<PVOID>& out) {
  if (!sr::kernel::guestWriteWatchActive()) return false;
  uint8_t* gb = sr::kernel::getGuestBase();
  if (!gb) return false;
  if (out.size() < (1u << 20)) out.resize(1u << 20);
  ULONG_PTR count = ULONG_PTR(out.size());
  DWORD gran = 0;
  if (GetWriteWatch(0, gb, kWatchTop, out.data(), &count, &gran) != 0) return false;
  out.resize(count);
  std::sort(out.begin(), out.end());
  return true;
}

void reportPages() {
  const uint64_t n = g_watchSamples.load();
  if (!g_nWatch || !n) return;
  // One line per page (a single line for all pages overflowed the logger's ~4 KB line), the
  // changed dwords sorted by rate, at most 60 shown.
  for (uint32_t i = 0; i < g_nWatch; i++) {
    std::vector<std::pair<uint32_t, uint32_t>> hits;
    for (uint32_t o = 0; o < 1024; o++) {
      const uint32_t c = g_watchHits[i][o].load(std::memory_order_relaxed);
      if (c) hits.push_back({c, o});
    }
    std::sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string line;
    for (size_t k = 0; k < hits.size() && k < 60; k++) {
      char b[48];
      std::snprintf(b, sizeof(b), " %08X:%.1f%%", g_watchVA[i] + 4u * hits[k].second, 100.0 * hits[k].first / double(n));
      line += b;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_LEDGER] watched page %08X: %zu dwords changed during sampled calls (of %llu):%s",
                g_watchVA[i], hits.size(), (unsigned long long)n, line.empty() ? " -" : line.c_str());
  }
}

void report() {
  reportPages();
  for (int k = 0; k < kKinds; k++) {
    Stats& s = g_stats[k];
    const uint64_t n = s.samples.load(std::memory_order_relaxed);
    if (!n) continue;
    std::vector<std::pair<uint32_t, uint32_t>> offs;
    for (uint32_t i = 0; i < kDevSpan / 4; i++) {
      const uint32_t c = s.devOff[i].load(std::memory_order_relaxed);
      if (c) offs.push_back({c, i * 4u});
    }
    std::sort(offs.begin(), offs.end(), [](auto& a, auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    std::string line;
    for (size_t i = 0; i < offs.size() && i < 64; i++) {
      char b[40];
      std::snprintf(b, sizeof(b), " +%04X:%.1f%%", offs[i].second, 100.0 * offs[i].first / double(n));
      line += b;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_LEDGER] %s samples=%llu zpass=%llu kickDraws=%llu kicks=%llu wwPagesOutside=%llu "
                "devDwordsChanged=%zu:%s",
                kKindName[k], (unsigned long long)n, (unsigned long long)s.zpass.load(),
                (unsigned long long)s.kickDraws.load(), (unsigned long long)s.kicks.load(),
                (unsigned long long)s.wwPages.load(), offs.size(), line.c_str());
    std::vector<std::pair<uint64_t, uint32_t>> pages;
    {
      std::lock_guard<std::mutex> lk(g_pageMu);
      for (auto& [pg, c] : g_pages[k]) pages.push_back({c, pg});
    }
    if (pages.empty()) continue;
    std::sort(pages.begin(), pages.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string pl;
    const uint64_t wn = g_wwSamples[k].load(), nn = g_noiseSamples.load();
    for (size_t i = 0; i < pages.size() && i < 32; i++) {
      uint64_t nz = 0;
      {
        std::lock_guard<std::mutex> lk(g_pageMu);
        auto it = g_noise.find(pages[i].second);
        if (it != g_noise.end()) nz = it->second;
      }
      char b[64];
      std::snprintf(b, sizeof(b), " %08X:%.1f%%/ctl%.1f%%", pages[i].second, wn ? 100.0 * pages[i].first / double(wn) : 0.0,
                    nn ? 100.0 * nz / double(nn) : 0.0);
      pl += b;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_LEDGER] %s pages(distinct=%zu, wwSamples=%llu, ctlSamples=%llu; %% of draw windows / %% of "
                "control windows):%s",
                kKindName[k], pages.size(), (unsigned long long)wn, (unsigned long long)nn, pl.c_str());
  }
}
}  // namespace

const bool g_on = envOn("SR_NATIVE_DRAW_LEDGER");

void before(uint32_t devVA, int kind) {
  if (kind < 0 || kind >= kKinds) return;
  Tls& t = tls(kind);
  t.armed = false;
  if (++t.n % (g_every ? g_every : 1u)) return;
  uint8_t* d = devHost(devVA);
  if (!d) return;
  t.ww = (++t.sampled % (g_wwEvery ? g_wwEvery : 1u)) == 0 && watchPages(t.ctl) && watchPages(t.ww0);
  if (t.ww) {
    std::vector<uint32_t> noise;
    uint8_t* gb = sr::kernel::getGuestBase();
    for (PVOID p : t.ww0)
      if (!std::binary_search(t.ctl.begin(), t.ctl.end(), p)) noise.push_back(uint32_t(static_cast<uint8_t*>(p) - gb));
    g_noiseSamples.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_pageMu);
    for (uint32_t pg : noise) g_noise[pg]++;
  }
  // The device snapshot is taken AFTER the scans, so its diff covers the draw call only.
  if (watchKind(kind))
    for (uint32_t i = 0; i < g_nWatch; i++)
      if (const uint8_t* h = sr::kernel::guestVAToHost(g_watchVA[i])) std::memcpy(t.pageSnap[i], h, 4096);
  std::memcpy(t.snap, d, kDevSpan);
  t.kicks0 = sr::native::threadKicks();
  t.cur0 = rdBE32(d + 0x28);
  t.armed = true;
}

void after(uint32_t devVA, int kind) {
  if (kind < 0 || kind >= kKinds) return;
  Tls& t = tls(kind);
  if (!t.armed) return;
  t.armed = false;
  uint8_t* d = devHost(devVA);
  if (!d) return;
  Stats& s = g_stats[kind];
  s.samples.fetch_add(1, std::memory_order_relaxed);
  if (sr::native::zPassActive()) s.zpass.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < kDevSpan; i += 4)
    if (std::memcmp(t.snap + i, d + i, 4) != 0) s.devOff[i / 4].fetch_add(1, std::memory_order_relaxed);
  if (g_nWatch && watchKind(kind)) {
    g_watchSamples.fetch_add(1, std::memory_order_relaxed);
    for (uint32_t i = 0; i < g_nWatch; i++)
      if (const uint8_t* h = sr::kernel::guestVAToHost(g_watchVA[i]))
        for (uint32_t o = 0; o < 4096; o += 4)
          if (std::memcmp(t.pageSnap[i] + o, h + o, 4) != 0) g_watchHits[i][o / 4].fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t k = sr::native::threadKicks() - t.kicks0;
  if (k) {
    s.kicks.fetch_add(k, std::memory_order_relaxed);
    s.kickDraws.fetch_add(1, std::memory_order_relaxed);
  }
  if (t.ww) {
    g_wwSamples[kind].fetch_add(1, std::memory_order_relaxed);
    if (watchPages(t.ww1)) {
      uint8_t* gb = sr::kernel::getGuestBase();
      const uint32_t devOff = uint32_t(d - gb);
      const uint32_t c0 = sr::kernel::aliasGuestVA(t.cur0), c1 = sr::kernel::aliasGuestVA(rdBE32(d + 0x28));
      std::vector<uint32_t> fresh;
      for (PVOID p : t.ww1) {
        if (std::binary_search(t.ww0.begin(), t.ww0.end(), p)) continue;
        const uint32_t pg = uint32_t(static_cast<uint8_t*>(p) - gb);
        if (pg + 4096u > devOff && pg < devOff + kDevSpan) continue;          // the device struct
        if ((pg >> 12) == (c0 >> 12) || (pg >> 12) == (c1 >> 12)) continue;  // the segment written
        if (c1 > c0 && pg >= c0 && pg <= c1) continue;
        fresh.push_back(pg);
      }
      if (!fresh.empty()) {
        s.wwPages.fetch_add(fresh.size(), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_pageMu);
        for (uint32_t pg : fresh) g_pages[kind][pg]++;
      }
    }
  }
  const uint64_t n = g_totalSamples.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((n & 4095u) == 0) report();
}

}  // namespace sr::native::ledger
