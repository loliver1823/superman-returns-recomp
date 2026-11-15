// iter-467 NATIVE strand A: the drain census. Contract: native_drain_census.hpp.
#include "native_drain_census.hpp"

#include <windows.h>
#include <intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "../app/log.hpp"

namespace sr::native::dcensus {

namespace {

const bool g_on = [] {
  const char* e = std::getenv("SR_NATIVE_DRAIN_CENSUS");
  return e && e[0] == '1';
}();

// Statistics only (one writer per side; a torn read only skews a report line).
struct alignas(64) Drain {
  uint64_t draws = 0, same = 0, prepSame = 0, prepDiff = 0, nPrepSame = 0, nPrepDiff = 0;
  uint64_t vpSame = 0, vpDiff = 0, nVpSame = 0, nVpDiff = 0, resolves = 0, drains = 0;
  uint32_t prev[64] = {};
  uint32_t prevN = 0;
};
Drain g_d;

constexpr uint32_t kOps = 6;
const uint32_t kOpCode[kOps] = {0x58u, 0x3Du, 0x3Eu, 0x54u, 0x64u, 0x46u};
const char* const kOpName[kOps] = {"evShd", "memWr", "regToMem", "intr", "swap", "evWrite"};
// Draws between two consecutive guest-visible effects: 0, 1-7, 8-31, 32-127, 128+.
constexpr uint32_t kBins = 5;
struct alignas(64) Cp {
  uint64_t op[kOps] = {}, waitsSat = 0, waitsBlocked = 0, draws = 0, runDraws = 0, swaps = 0;
  uint64_t gap[kBins] = {};
  uint64_t lastReportSwap = 0, windowSwap0 = 0;
};
Cp g_c;

double tscPerUs() {
  static double f = 0;
  if (f == 0) {
    LARGE_INTEGER q0, q1, fq;
    QueryPerformanceCounter(&q0);
    const uint64_t t0 = __rdtsc();
    Sleep(20);
    QueryPerformanceCounter(&q1);
    const uint64_t t1 = __rdtsc();
    QueryPerformanceFrequency(&fq);
    f = double(t1 - t0) / (double(q1.QuadPart - q0.QuadPart) / double(fq.QuadPart)) / 1e6;
  }
  return f;
}

// Per window (the counters restart after each line; the drain runs on the CP thread today, so one
// thread writes both blocks - a torn line is possible only if that changes).
void report() {
  const uint64_t wSwaps = g_c.swaps - g_c.windowSwap0;
  const double fr = wSwaps ? double(wSwaps) : 1.0;
  const double t = tscPerUs();
  auto us = [&](uint64_t tsc, uint64_t n) { return n ? double(tsc) / t / double(n) : 0.0; };
  char ops[256];
  int o = 0;
  for (uint32_t i = 0; i < kOps; i++)
    o += std::snprintf(ops + o, sizeof(ops) - size_t(o), " %s=%.1f", kOpName[i], double(g_c.op[i]) / fr);
  const uint64_t gaps = g_c.gap[0] + g_c.gap[1] + g_c.gap[2] + g_c.gap[3] + g_c.gap[4];
  auto pct = [&](uint64_t v, uint64_t n) { return n ? 100.0 * double(v) / double(n) : 0.0; };
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_DRAIN_CENSUS] swaps=%llu window=%llu | drain per frame: draws=%.0f sameKey=%.1f%% resolves=%.1f drains=%.1f | "
              "prepare us/draw same=%.3f diff=%.3f (ms/frame same=%.3f diff=%.3f) | viewport us/draw same=%.3f diff=%.3f "
              "(ms/frame same=%.3f diff=%.3f) | effects per frame:%s waits sat=%.1f blocked=%.1f | draws between effects: "
              "0=%.1f%% 1-7=%.1f%% 8-31=%.1f%% 32-127=%.1f%% 128+=%.1f%% (gaps/frame=%.1f, CP draws/frame=%.0f)",
              (unsigned long long)g_c.swaps, (unsigned long long)wSwaps, double(g_d.draws) / fr, pct(g_d.same, g_d.draws), double(g_d.resolves) / fr,
              double(g_d.drains) / fr, us(g_d.prepSame, g_d.nPrepSame), us(g_d.prepDiff, g_d.nPrepDiff),
              double(g_d.prepSame) / t / 1000.0 / fr, double(g_d.prepDiff) / t / 1000.0 / fr, us(g_d.vpSame, g_d.nVpSame),
              us(g_d.vpDiff, g_d.nVpDiff), double(g_d.vpSame) / t / 1000.0 / fr, double(g_d.vpDiff) / t / 1000.0 / fr, ops,
              double(g_c.waitsSat) / fr, double(g_c.waitsBlocked) / fr, pct(g_c.gap[0], gaps), pct(g_c.gap[1], gaps),
              pct(g_c.gap[2], gaps), pct(g_c.gap[3], gaps), pct(g_c.gap[4], gaps), double(gaps) / fr,
              double(g_c.draws) / fr);
  const uint32_t prevN = g_d.prevN;
  uint32_t prev[64];
  std::memcpy(prev, g_d.prev, sizeof(prev));
  g_d = Drain{};
  g_d.prevN = prevN;
  std::memcpy(g_d.prev, prev, sizeof(prev));
  const uint64_t swaps = g_c.swaps, runDraws = g_c.runDraws;
  g_c = Cp{};
  g_c.swaps = g_c.windowSwap0 = g_c.lastReportSwap = swaps;
  g_c.runDraws = runDraws;
}

}  // namespace

bool on() { return g_on; }

void drainBegin() {
  if (!g_on) return;
  g_d.prevN = 0;
  g_d.drains++;
}

bool keySame(const uint32_t* key, uint32_t n) {
  if (!g_on || n > 64) return false;
  g_d.draws++;
  const bool same = g_d.prevN == n && std::memcmp(g_d.prev, key, 4u * n) == 0;
  if (!same) {
    std::memcpy(g_d.prev, key, 4u * n);
    g_d.prevN = n;
  } else {
    g_d.same++;
  }
  return same;
}

void notePrep(bool same, uint64_t prepTsc) {
  if (!g_on) return;
  if (same) {
    g_d.prepSame += prepTsc;
    g_d.nPrepSame++;
  } else {
    g_d.prepDiff += prepTsc;
    g_d.nPrepDiff++;
  }
}

void noteViewport(bool same, uint64_t vpTsc) {
  if (!g_on) return;
  if (same) {
    g_d.vpSame += vpTsc;
    g_d.nVpSame++;
  } else {
    g_d.vpDiff += vpTsc;
    g_d.nVpDiff++;
  }
}

void noteResolveDraw() {
  if (g_on) g_d.resolves++;
}

void noteEffect(uint32_t op) {
  if (!g_on) return;
  for (uint32_t i = 0; i < kOps; i++)
    if (kOpCode[i] == op) {
      g_c.op[i]++;
      break;
    }
  const uint64_t r = g_c.runDraws;
  g_c.gap[r == 0 ? 0 : r < 8 ? 1 : r < 32 ? 2 : r < 128 ? 3 : 4]++;
  g_c.runDraws = 0;
  if (op == 0x64u) {
    g_c.swaps++;
    if (g_c.swaps - g_c.lastReportSwap >= 256) {
      g_c.lastReportSwap = g_c.swaps;
      report();
    }
  }
}

void noteWait(bool blocked) {
  if (!g_on) return;
  if (blocked) g_c.waitsBlocked++;
  else g_c.waitsSat++;
}

void noteDraw() {
  if (!g_on) return;
  g_c.draws++;
  g_c.runDraws++;
}

// ---- predicated tiling census ----
namespace {
const bool g_tile = [] {
  const char* e = std::getenv("SR_NATIVE_TILE_CENSUS");
  return e && e[0] == '1';
}();
struct IbFirst {
  uint32_t execs = 0;
  uint32_t regs[kTileRegs] = {};
};
struct TileWin {
  uint64_t frames = 0, ibExecs = 0, replayExecs = 0, replayedIbs = 0, binNotAll = 0;
  uint64_t drawsFirst = 0, drawsReplay = 0, skipFirst = 0, skipReplay = 0, resFirst = 0, resReplay = 0;
  uint64_t cpFirst = 0, cpReplay = 0, drainFirst = 0, drainReplay = 0;
  uint64_t diff[kTileRegs] = {};
  uint64_t tilesHist[4] = {};  // execs of a replayed IB: 2, 3, 4, 5+
  uint64_t maxTiles = 0;
  uint64_t predRun = 0, predSkipAll = 0, predBlend = 0;
};
TileWin g_tw;
std::unordered_map<uint32_t, IbFirst> g_frameIbs;
struct Cur {
  bool open = false, replay = false;
  uint64_t draws = 0, skip = 0, res = 0;
} g_cur;
}  // namespace

bool tileOn() { return g_tile; }

void ibBegin(uint32_t ibPhys, uint64_t draws, uint64_t predSkipped, uint64_t resolves, const uint32_t* regs) {
  if (!g_tile) return;
  IbFirst& f = g_frameIbs[ibPhys];
  g_cur.replay = f.execs > 0;
  if (!f.execs) std::memcpy(f.regs, regs, sizeof(f.regs));
  else
    for (uint32_t i = 0; i < kTileRegs; i++)
      if (regs[i] != f.regs[i]) g_tw.diff[i]++;
  f.execs++;
  g_tw.ibExecs++;
  if (g_cur.replay) g_tw.replayExecs++;
  if (regs[0] != 0xFFFFFFFFu || regs[1] != 0u) g_tw.binNotAll++;
  g_cur.open = true;
  g_cur.draws = draws;
  g_cur.skip = predSkipped;
  g_cur.res = resolves;
}

void ibEnd(uint64_t draws, uint64_t predSkipped, uint64_t resolves, uint64_t cpTsc, uint64_t drainTsc) {
  if (!g_tile || !g_cur.open) return;
  g_cur.open = false;
  const uint64_t d = draws - g_cur.draws, s = predSkipped - g_cur.skip, r = resolves - g_cur.res;
  if (g_cur.replay) {
    g_tw.drawsReplay += d;
    g_tw.skipReplay += s;
    g_tw.resReplay += r;
    g_tw.cpReplay += cpTsc;
    g_tw.drainReplay += drainTsc;
  } else {
    g_tw.drawsFirst += d;
    g_tw.skipFirst += s;
    g_tw.resFirst += r;
    g_tw.cpFirst += cpTsc;
    g_tw.drainFirst += drainTsc;
  }
}

void tileFrame(uint64_t predDrawsRun, uint64_t predSkipped, uint64_t predBlend) {
  if (!g_tile) return;
  static uint64_t s_prevRun = 0, s_prevSkip = 0, s_prevBlend = 0;
  g_tw.predBlend += predBlend - s_prevBlend;
  s_prevBlend = predBlend;
  g_tw.predRun += predDrawsRun - s_prevRun;
  g_tw.predSkipAll += predSkipped - s_prevSkip;
  s_prevRun = predDrawsRun;
  s_prevSkip = predSkipped;
  for (const auto& kv : g_frameIbs) {
    const uint32_t e = kv.second.execs;
    if (e < 2) continue;
    g_tw.replayedIbs++;
    g_tw.tilesHist[e >= 5 ? 3 : e - 2]++;
    if (e > g_tw.maxTiles) g_tw.maxTiles = e;
  }
  g_frameIbs.clear();
  if (++g_tw.frames < 256) return;
  const double fr = double(g_tw.frames), t = tscPerUs();
  auto ms = [&](uint64_t tsc) { return double(tsc) / t / 1000.0 / fr; };
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_TILE_CENSUS] window=%llu frames | per frame: IB execs=%.1f replays=%.1f replayed IBs=%.2f "
              "(tiles 2=%llu 3=%llu 4=%llu 5+=%llu max=%llu) binSelect!=all=%.1f | predicated draws run=%.0f (blending %.0f) "
              "skipped=%.0f | "
              "draws first=%.0f replay=%.0f | "
              "predicated skips first=%.0f replay=%.0f | resolves first=%.1f replay=%.1f | CP ms first=%.3f replay=%.3f | "
              "IB-end drain ms first=%.3f replay=%.3f | replays whose tile state differs from the first execution: "
              "binSel=%llu/%llu winOff=%llu winScisTL=%llu winScisBR=%llu scrScisTL=%llu scrScisBR=%llu (of %llu)",
              (unsigned long long)g_tw.frames, double(g_tw.ibExecs) / fr, double(g_tw.replayExecs) / fr,
              double(g_tw.replayedIbs) / fr, (unsigned long long)g_tw.tilesHist[0], (unsigned long long)g_tw.tilesHist[1],
              (unsigned long long)g_tw.tilesHist[2], (unsigned long long)g_tw.tilesHist[3],
              (unsigned long long)g_tw.maxTiles, double(g_tw.binNotAll) / fr, double(g_tw.predRun) / fr,
              double(g_tw.predBlend) / fr, double(g_tw.predSkipAll) / fr, double(g_tw.drawsFirst) / fr,
              double(g_tw.drawsReplay) / fr, double(g_tw.skipFirst) / fr, double(g_tw.skipReplay) / fr,
              double(g_tw.resFirst) / fr, double(g_tw.resReplay) / fr, ms(g_tw.cpFirst), ms(g_tw.cpReplay),
              ms(g_tw.drainFirst), ms(g_tw.drainReplay), (unsigned long long)g_tw.diff[0],
              (unsigned long long)g_tw.diff[1], (unsigned long long)g_tw.diff[2], (unsigned long long)g_tw.diff[3],
              (unsigned long long)g_tw.diff[4], (unsigned long long)g_tw.diff[5], (unsigned long long)g_tw.diff[6],
              (unsigned long long)g_tw.replayExecs);
  g_tw = TileWin{};
}

}  // namespace sr::native::dcensus
