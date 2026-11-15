// iter-467 flicker hunt: the per-draw ledger over consecutive presents of a sim freeze. Contract:
// draw_ledger.hpp.
#include "draw_ledger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenos_state.hpp"
#include "../app/log.hpp"
#include "../kernel/highfps.hpp"
#include "../kernel/host_automation.hpp"  // iter-467 forcing: the windowed ledger opens at the harness dump mark

namespace sr::gfx::ledger {

namespace {

const bool g_on = [] {
  const char* e = std::getenv("SR_DRAW_LEDGER");
  return e && e[0] == '1';
}();

// The register-file subset a draw consumes (xenos_state.hpp GuestDrawCommand: viewport, render
// state, ALU constants, fetch constants, bool / loop constants).
constexpr uint32_t kR1Lo = 0x2000u, kR1N = 0x400u;
constexpr uint32_t kR2Lo = 0x4000u, kR2N = 0x980u;
constexpr uint32_t kSnapN = kR1N + kR2N;
constexpr uint32_t kMaxDraws = 16384u;
constexpr double kFrozenMs = 400.0;  // highfps::freezeDumpWanted's threshold: the render clock caught up
constexpr uint32_t kDataBytesPerSlot = 4096u;
constexpr uint32_t kFullPerFreeze = 12, kFullTotal = 300, kDrawsPerPresent = 10;

struct Entry {
  uint64_t hash = 0;  // register subset (0 = not hashed: the present was not frozen at its first draw)
  uint64_t data = 0;  // vertex data of the bound vertex-fetch slots
  uint32_t initiator = 0, dmaBase = 0, dmaSize = 0, dropped = 0, nSlots = 0;
  uint64_t vs = 0, ps = 0, binSel = 0, binMask = 0, rec = 0;
};

struct Present {
  std::vector<Entry> e;
  std::vector<uint32_t> snap;  // kSnapN dwords per entry while frozen
  bool frozen = false;         // held >= kFrozenMs at the first draw (and at the swap once closed)
  double held0 = 0.0;
  uint64_t swap = 0;
};

Present g_cur, g_prev;
uint64_t g_compared = 0, g_differ = 0, g_full = 0, g_fullFreeze = 0, g_missing = 0, g_content = 0, g_dataDiff = 0;
double g_lastHeld = 0.0;

uint64_t mix(const uint32_t* d, size_t n, uint64_t h) {
  for (size_t i = 0; i < n; i++) {
    h ^= d[i];
    h *= 0x100000001B3ull;
    h ^= h >> 29;
  }
  return h;
}

bool sameArgs(const Entry& a, const Entry& b) {
  return a.initiator == b.initiator && a.dmaBase == b.dmaBase && a.dmaSize == b.dmaSize && a.vs == b.vs &&
         a.ps == b.ps && a.dropped == b.dropped;
}
bool sameAll(const Entry& a, const Entry& b) { return sameArgs(a, b) && a.hash == b.hash && a.data == b.data; }

uint32_t regOf(uint32_t i) { return i < kR1N ? kR1Lo + i : kR2Lo + (i - kR1N); }
const char* rangeName(uint32_t reg) {
  if (reg < 0x2400u) return "state";
  if (reg < 0x4400u) return "aluVS";
  if (reg < 0x4800u) return "aluPS";
  if (reg < 0x48C0u) return "fetch";
  return "bool";
}

void describe(char* out, size_t n, const char* who, const Entry& e) {
  std::snprintf(out, n, "%s{init=%08X dma=%08X/%08X vs=%016llX ps=%016llX bin=%llX&%llX rec=%llu drop=%u slots=%u h=%016llX d=%016llX}",
                who, e.initiator, e.dmaBase, e.dmaSize, (unsigned long long)e.vs, (unsigned long long)e.ps,
                (unsigned long long)e.binSel, (unsigned long long)e.binMask, (unsigned long long)e.rec, e.dropped,
                e.nSlots, (unsigned long long)e.hash, (unsigned long long)e.data);
}

struct Pair {
  uint32_t i, j;  // cur index, prev index
};

void compare(const Present& p, const Present& c) {
  g_compared++;
  if (c.held0 < g_lastHeld) g_fullFreeze = 0;  // a new freeze: its own full-detail budget
  g_lastHeld = c.held0;
  const bool full = g_fullFreeze < kFullPerFreeze && g_full < kFullTotal;
  char a[400], b[400];
  // 1. Alignment by draw arguments (one missing draw on either side is skipped over; more than 3
  //    unaligned in a row = the sequences diverged, stop aligning there).
  std::vector<Pair> pairs;
  pairs.reserve(c.e.size());
  uint32_t i = 0, j = 0, missing = 0, reported = 0;
  while (i < c.e.size() && j < p.e.size()) {
    if (sameArgs(c.e[i], p.e[j])) {
      pairs.push_back({i, j});
      i++;
      j++;
      continue;
    }
    const bool curExtra = i + 1 < c.e.size() && sameArgs(c.e[i + 1], p.e[j]);
    const bool prevExtra = j + 1 < p.e.size() && sameArgs(c.e[i], p.e[j + 1]);
    missing++;
    if (full && reported < kDrawsPerPresent) {
      reported++;
      describe(a, sizeof(a), "cur", c.e[i]);
      describe(b, sizeof(b), "prev", p.e[j]);
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[DRAW_LEDGER]   %s at cur %u / prev %u: %s %s",
                  curExtra && !prevExtra ? "EXTRA DRAW IN CUR (prev lacks it)"
                  : prevExtra && !curExtra ? "MISSING DRAW IN CUR (prev has it)"
                                           : "ARGS DIFFER",
                  i, j, a, b);
    }
    if (curExtra && !prevExtra) i++;
    else if (prevExtra && !curExtra) j++;
    else {
      i++;
      j++;
    }
    if (missing > 64) break;
  }
  const uint32_t tailCur = uint32_t(c.e.size()) - i, tailPrev = uint32_t(p.e.size()) - j;
  // 2. Content: per aligned pair, the differing registers; the histogram over the differing draws.
  std::unordered_map<uint32_t, uint32_t> hist;
  std::vector<uint32_t> diffPairs;
  uint32_t dataDiffs = 0;
  const bool snaps = p.snap.size() >= p.e.size() * size_t(kSnapN) && c.snap.size() >= c.e.size() * size_t(kSnapN);
  for (size_t k = 0; k < pairs.size(); k++) {
    const Entry& ec = c.e[pairs[k].i];
    const Entry& ep = p.e[pairs[k].j];
    if (ec.hash == ep.hash && ec.data == ep.data) continue;
    diffPairs.push_back(uint32_t(k));
    if (ec.data != ep.data) dataDiffs++;
    if (!snaps || ec.hash == ep.hash) continue;
    const uint32_t* sc = c.snap.data() + size_t(pairs[k].i) * kSnapN;
    const uint32_t* sp = p.snap.data() + size_t(pairs[k].j) * kSnapN;
    for (uint32_t r = 0; r < kSnapN; r++)
      if (sc[r] != sp[r]) hist[r]++;
  }
  if (!missing && !tailCur && !tailPrev && diffPairs.empty()) {
    if (g_compared <= 8 || g_compared % 200 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[DRAW_LEDGER] swap %llu frozen (held %.0f ms): %zu draws identical to the previous present | "
                  "compared=%llu differ=%llu (missing=%llu content=%llu data=%llu)",
                  (unsigned long long)c.swap, c.held0, c.e.size(), (unsigned long long)g_compared,
                  (unsigned long long)g_differ, (unsigned long long)g_missing, (unsigned long long)g_content,
                  (unsigned long long)g_dataDiff);
    return;
  }
  g_differ++;
  if (missing || tailCur || tailPrev) g_missing++;
  if (dataDiffs) g_dataDiff++;
  // The volatile set: registers differing in >= 25% of the differing draws (dynamic vertex-buffer
  // sizes, per-present constants). A draw is a candidate when it differs elsewhere or in its data.
  const uint32_t nd = uint32_t(diffPairs.size());
  std::vector<uint32_t> common;
  for (const auto& kv : hist)
    if (kv.second * 4u >= nd && nd >= 4u) common.push_back(kv.first);
  std::sort(common.begin(), common.end());
  uint32_t candidates = 0;
  std::vector<uint32_t> candList;
  for (uint32_t k : diffPairs) {
    const Entry& ec = c.e[pairs[k].i];
    const Entry& ep = p.e[pairs[k].j];
    bool extra = ec.data != ep.data;
    if (!extra && snaps && ec.hash != ep.hash) {
      const uint32_t* sc = c.snap.data() + size_t(pairs[k].i) * kSnapN;
      const uint32_t* sp = p.snap.data() + size_t(pairs[k].j) * kSnapN;
      for (uint32_t r = 0; r < kSnapN && !extra; r++)
        if (sc[r] != sp[r] && !std::binary_search(common.begin(), common.end(), r)) extra = true;
    }
    if (extra) {
      candidates++;
      if (candList.size() < kDrawsPerPresent) candList.push_back(k);
    }
  }
  if (candidates) g_content++;
  if (!full) {
    if (g_differ <= 16 || (g_differ & (g_differ - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[DRAW_LEDGER] #%llu swap %llu: missing=%u tailCur=%u tailPrev=%u differing=%u (data %u) candidates=%u "
                  "volatile=%zu regs (cur %zu / prev %zu draws)",
                  (unsigned long long)g_differ, (unsigned long long)c.swap, missing, tailCur, tailPrev, nd, dataDiffs,
                  candidates, common.size(), c.e.size(), p.e.size());
    return;
  }
  g_full++;
  g_fullFreeze++;
  char vol[512];
  int o = 0;
  for (size_t q = 0; q < common.size() && q < 24; q++)
    o += std::snprintf(vol + o, sizeof(vol) - size_t(o), " %04X(%u)", regOf(common[q]), hist[common[q]]);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[DRAW_LEDGER] #%llu swap %llu (held %.0f ms): cur %zu / prev %zu draws, aligned %zu, missing %u, tail cur %u "
              "prev %u | differing aligned draws %u (data %u) | volatile regs (>=25%% of them):%s | CANDIDATES %u",
              (unsigned long long)g_differ, (unsigned long long)c.swap, c.held0, c.e.size(), p.e.size(), pairs.size(),
              missing, tailCur, tailPrev, nd, dataDiffs, common.size() ? vol : " none", candidates);
  for (uint32_t k : candList) {
    const Entry& ec = c.e[pairs[k].i];
    const Entry& ep = p.e[pairs[k].j];
    describe(a, sizeof(a), "cur", ec);
    describe(b, sizeof(b), "prev", ep);
    char line[1024];
    int n = 0;
    uint32_t cnt[5] = {}, shown = 0;
    if (snaps && ec.hash != ep.hash) {
      const uint32_t* sc = c.snap.data() + size_t(pairs[k].i) * kSnapN;
      const uint32_t* sp = p.snap.data() + size_t(pairs[k].j) * kSnapN;
      for (uint32_t r = 0; r < kSnapN; r++) {
        if (sc[r] == sp[r]) continue;
        const uint32_t reg = regOf(r);
        cnt[reg < 0x2400u ? 0 : reg < 0x4400u ? 1 : reg < 0x4800u ? 2 : reg < 0x48C0u ? 3 : 4]++;
        const bool vol2 = std::binary_search(common.begin(), common.end(), r);
        if (shown < 14 && !vol2) {
          n += std::snprintf(line + n, sizeof(line) - size_t(n), " %04X:%08X/%08X", reg, sc[r], sp[r]);
          shown++;
        }
      }
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[DRAW_LEDGER]   candidate draw cur %u / prev %u: %s | diff state=%u aluVS=%u aluPS=%u fetch=%u bool=%u (cur/prev, "
                "volatile omitted):%s | %s %s",
                pairs[k].i, pairs[k].j, ec.data != ep.data ? "VERTEX DATA DIFFERS" : "registers differ", cnt[0], cnt[1],
                cnt[2], cnt[3], cnt[4], n ? line : " -", a, b);
    // Neighbours: which object the draw belongs to.
    for (uint32_t q = (k >= 1 ? k - 1 : 0); q <= k + 1 && q < pairs.size(); q++) {
      if (q == k) continue;
      describe(a, sizeof(a), "cur", c.e[pairs[q].i]);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[DRAW_LEDGER]     neighbour cur %u: %s", pairs[q].i, a);
    }
  }
  (void)rangeName;
}

// ── iter-467 forcing (flash 13b, orchestrator 2026-10-11): THE WINDOWED LEDGER ───────────────────────────────
// SR_DRAW_LEDGER_WINDOW=<seconds> (diagnostic, default OFF, independent of SR_DRAW_LEDGER): no sim freeze. It
// opens at the first CP swap after the harness DUMP MARK (tools/controls/harness_play.py `mark dump`, the same
// mark SR_PRESENT_DUMP_FROM_MARK uses) and stays open that many seconds. Per present it aggregates the constant
// file 0x4000-0x497F (vertex-fetch addresses masked) of the SELECTED draws (SR_DRAW_LEDGER_WINDOW_PS=<hex>[+<hex>
// ...], up to 8 PS ucode hashes; unset = every draw) and logs:
//   [DRAW_LEDGER_W] swap=<n> draws=<all> sel=<selected> uni=<registers every selected draw agreed on>
//     chg=<uniform registers that changed since the previous present> h=<hash of the selected snapshots> |
//     <reg>=<dwords at the present's first selected draw, '*' = not uniform> ... (SR_DRAW_LEDGER_WINDOW_REGS=
//     <hexreg>:<dwords>[:<name>][+...]; continuation lines when long)
//   [DRAW_LEDGER_W] ABA swap=<the middle present> ...: registers uniform in three consecutive presents whose
//     value in the middle one differs from the outer two, which agree (the guest wrote B between two A's).
struct WinRange {
  uint32_t reg = 0, n = 0;
};
struct Win {
  double sec = 0.0;
  uint64_t ps[8] = {};
  int nPs = 0;
  std::vector<WinRange> regs;
  bool open = false, closed = false;
  std::chrono::steady_clock::time_point t0;
  uint64_t openSwap = 0, presents = 0, abaPresents = 0;
  // the present being drawn
  uint32_t draws = 0, sel = 0;
  uint64_t hash = 0;
  std::vector<uint32_t> first, tmp;
  std::vector<uint8_t> uni;
  // the two previous presents (1 = N-1, 2 = N-2); have = how many are valid in a row
  std::vector<uint32_t> f1, f2;
  std::vector<uint8_t> u1, u2;
  uint64_t s1 = 0;
  int have = 0;
};

Win& win() {
  static Win w = [] {
    Win x;
    const char* e = std::getenv("SR_DRAW_LEDGER_WINDOW");
    x.sec = e ? std::atof(e) : 0.0;
    if (x.sec <= 0.0) return x;
    if (const char* p = std::getenv("SR_DRAW_LEDGER_WINDOW_PS")) {
      for (const char* q = p; *q && x.nPs < 8;) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(q, &end, 16);
        if (end == q) break;
        x.ps[x.nPs++] = uint64_t(v);
        q = (*end == '+' || *end == ',') ? end + 1 : end;
        if (*end != '+' && *end != ',') break;
      }
    }
    if (const char* p = std::getenv("SR_DRAW_LEDGER_WINDOW_REGS")) {
      std::string s(p);
      size_t a = 0;
      while (a < s.size()) {
        size_t b = s.find_first_of("+,", a);
        if (b == std::string::npos) b = s.size();
        const std::string item = s.substr(a, b - a);
        a = b + 1;
        WinRange r;
        r.reg = uint32_t(std::strtoul(item.c_str(), nullptr, 16));
        const size_t c = item.find(':');
        r.n = c == std::string::npos ? 1u : uint32_t(std::strtoul(item.c_str() + c + 1, nullptr, 10));
        if (r.reg >= kR2Lo && r.n && r.reg + r.n <= kR2Lo + kR2N) x.regs.push_back(r);
      }
    }
    x.first.assign(kR2N, 0u);
    x.tmp.assign(kR2N, 0u);
    x.uni.assign(kR2N, 0u);
    x.f1 = x.f2 = x.first;
    x.u1 = x.u2 = x.uni;
    return x;
  }();
  return w;
}

const bool g_winOn = win().sec > 0.0;

void winNoteDraw(const XenosRegisterFile& xr, uint64_t ps) {
  Win& w = win();
  if (!w.open) return;
  w.draws++;
  if (w.nPs) {
    bool hit = false;
    for (int i = 0; i < w.nPs; i++) hit |= w.ps[i] == ps;
    if (!hit) return;
  }
  uint32_t* t = w.tmp.data();
  std::memcpy(t, xr.data() + kR2Lo, kR2N * 4u);
  for (uint32_t slot = 0; slot < XenosRegisterFile::kVertexFetchSlots; slot++) {
    uint32_t* w0 = t + (XenosRegisterFile::kFetchConstBase + slot * 2u - kR2Lo);
    if ((w0[0] & 3u) == 3u) w0[0] = 3u;  // ring-allocated vertex buffers move every present
  }
  w.hash = mix(t, kR2N, w.hash ^ ps);
  if (w.sel == 0) {
    std::memcpy(w.first.data(), t, kR2N * 4u);
    std::fill(w.uni.begin(), w.uni.end(), uint8_t(1));
  } else if (std::memcmp(w.first.data(), t, kR2N * 4u) != 0) {
    for (uint32_t r = 0; r < kR2N; r++)
      if (w.uni[r] && t[r] != w.first[r]) w.uni[r] = 0;
  }
  w.sel++;
}

void winEmit(const std::string& s, uint64_t swap) {
  // the log line holds 4096 bytes: continuation lines carry the present's swap
  const size_t kChunk = 3600;
  for (size_t a = 0; a < s.size(); a += kChunk)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[DRAW_LEDGER_W] %s%s", a ? ("swap=" + std::to_string(swap) + " cont |").c_str() : "",
                s.substr(a, kChunk).c_str());
}

void winOnSwap(uint64_t swaps) {
  Win& w = win();
  if (!w.open) {
    if (w.closed || !(sr::kernel::automation::harnessFlags() & sr::kernel::automation::kHarnessFlagDumpMark)) return;
    w.open = true;
    w.t0 = std::chrono::steady_clock::now();
    w.openSwap = swaps;
    w.draws = w.sel = 0;
    w.hash = 0x9E3779B97F4A7C15ull;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[DRAW_LEDGER_W] window OPEN at swap %llu by the harness dump mark: %.1f s, PS filter %d hash(es)%s, "
                "%zu register range(s)",
                (unsigned long long)swaps, w.sec, w.nPs, w.nPs ? "" : " (every draw)", w.regs.size());
    return;
  }
  // The present that just ended.
  w.presents++;
  char buf[160];
  uint32_t nUni = 0, nChg = 0;
  const bool cur = w.sel > 0;
  if (cur)
    for (uint32_t r = 0; r < kR2N; r++) {
      nUni += w.uni[r];
      if (w.have >= 1 && w.uni[r] && w.u1[r] && w.first[r] != w.f1[r]) nChg++;
    }
  std::snprintf(buf, sizeof(buf), "swap=%llu draws=%u sel=%u uni=%u chg=%u h=%016llX |", (unsigned long long)swaps,
                w.draws, w.sel, nUni, nChg, (unsigned long long)w.hash);
  std::string line(buf);
  if (cur)
    for (const WinRange& r : w.regs) {
      std::snprintf(buf, sizeof(buf), " %04X=", r.reg);
      line += buf;
      for (uint32_t i = 0; i < r.n; i++) {
        const uint32_t k = r.reg - kR2Lo + i;
        std::snprintf(buf, sizeof(buf), "%s%08X%s", i ? "," : "", w.first[k], w.uni[k] ? "" : "*");
        line += buf;
      }
    }
  winEmit(line, swaps);
  // A-B-A: N-2 = A, N-1 = B, N = A.
  if (cur && w.have >= 2) {
    std::string aba;
    uint32_t n = 0;
    for (uint32_t r = 0; r < kR2N; r++) {
      if (!(w.u2[r] && w.u1[r] && w.uni[r])) continue;
      if (w.f2[r] != w.first[r] || w.f1[r] == w.first[r]) continue;
      if (n++ < 48) {
        std::snprintf(buf, sizeof(buf), " %04X:%08X/%08X", kR2Lo + r, w.first[r], w.f1[r]);
        aba += buf;
      }
    }
    if (n) {
      w.abaPresents++;
      std::snprintf(buf, sizeof(buf), "ABA swap=%llu (between %llu and %llu): %u register(s) A/B:",
                    (unsigned long long)w.s1, (unsigned long long)(w.s1 - 1), (unsigned long long)swaps, n);
      winEmit(std::string(buf) + aba, swaps);
    }
  }
  if (cur) {
    std::swap(w.f2, w.f1);
    std::swap(w.u2, w.u1);
    std::swap(w.f1, w.first);
    std::swap(w.u1, w.uni);
    w.s1 = swaps;
    w.have = w.have < 2 ? w.have + 1 : 2;
  } else {
    w.have = 0;  // no selected draw: the chain restarts
  }
  w.draws = w.sel = 0;
  w.hash = 0x9E3779B97F4A7C15ull;
  const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - w.t0).count();
  if (el >= w.sec) {
    w.open = false;
    w.closed = true;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[DRAW_LEDGER_W] window CLOSED at swap %llu after %.1f s: %llu presents, %llu with an A-B-A register",
                (unsigned long long)swaps, el, (unsigned long long)w.presents, (unsigned long long)w.abaPresents);
  }
}

}  // namespace

bool on() { return g_on || g_winOn; }  // iter-467 forcing: or the windowed ledger

void noteDraw(const XenosRegisterFile& xr, uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize, uint64_t vsHash,
              uint64_t psHash, uint64_t binSelect, uint64_t binMask, uint64_t nativeRec, PhysToHost phys, void* user) {
  if (g_winOn) winNoteDraw(xr, psHash);  // iter-467 forcing: the windowed ledger (no freeze)
  if (!g_on) return;
  Present& p = g_cur;
  if (p.e.empty()) {
    p.held0 = sr::kernel::highfps::freezeHeldMs();
    p.frozen = p.held0 >= kFrozenMs;
    p.snap.clear();
  }
  if (p.e.size() >= kMaxDraws) return;
  Entry en;
  en.initiator = initiator;
  en.dmaBase = dmaBase;
  en.dmaSize = dmaSize;
  en.vs = vsHash;
  en.ps = psHash;
  en.binSel = binSelect;
  en.binMask = binMask;
  en.rec = nativeRec;
  if (p.frozen) {
    const uint32_t* d = xr.data();
    const size_t o = p.snap.size();
    p.snap.resize(o + kSnapN);
    uint32_t* s = p.snap.data() + o;
    std::memcpy(s, d + kR1Lo, kR1N * 4u);
    std::memcpy(s + kR1N, d + kR2Lo, kR2N * 4u);
    // Vertex fetch slots (type 3 in dword 0): the address is masked out of the snapshot, the data
    // it points at is hashed instead (dynamic vertex buffers move every present).
    uint64_t dh = 0x9E3779B97F4A7C15ull;
    for (uint32_t slot = 0; slot < XenosRegisterFile::kVertexFetchSlots; slot++) {
      const uint32_t r = XenosRegisterFile::kFetchConstBase + slot * 2u;
      uint32_t* w0 = s + kR1N + (r - kR2Lo);
      if ((w0[0] & 3u) != 3u) continue;
      const uint32_t addr = (w0[0] >> 2) & 0x3FFFFFFFu;
      const uint32_t sizeWords = (w0[1] >> 2) & 0x00FFFFFFu;
      w0[0] = 3u;
      en.nSlots++;
      const uint8_t* h = phys ? phys(user, addr * 4u) : nullptr;
      const uint32_t bytes = std::min<uint32_t>(sizeWords * 4u, kDataBytesPerSlot) & ~3u;
      dh ^= uint64_t(slot) * 0x9E3779B97F4A7C15ull;
      if (h && bytes) dh = mix(reinterpret_cast<const uint32_t*>(h), bytes / 4u, dh);
      else dh = mix(&sizeWords, 1, dh ^ 0xA5A5A5A5ull);
    }
    en.data = dh;
    en.hash = mix(s, kSnapN, 0x9E3779B97F4A7C15ull) | 1ull;
  }
  p.e.push_back(en);
}

void noteDropped(uint32_t reason) {
  if (!g_on || g_cur.e.empty()) return;
  g_cur.e.back().dropped = reason;
}

void onSwap(uint64_t swaps) {
  if (g_winOn) winOnSwap(swaps);  // iter-467 forcing: the windowed ledger (no freeze)
  if (!g_on) return;
  Present& c = g_cur;
  c.swap = swaps;
  const double held1 = sr::kernel::highfps::freezeHeldMs();
  const bool frozen = c.frozen && held1 > c.held0;  // still held: every draw ran inside the freeze
  if (frozen && g_prev.frozen && g_prev.swap + 1 == swaps && !c.e.empty()) compare(g_prev, c);
  std::swap(g_prev, g_cur);
  g_prev.frozen = frozen;
  g_cur.e.clear();
  g_cur.snap.clear();
  g_cur.frozen = false;
}

}  // namespace sr::gfx::ledger
