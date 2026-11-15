// iter-467 NATIVE DRAW BUILDER S0 census - contract and gates: native_builder_census.hpp.
// Observe-only: the three overrides read guest memory and time the guest body; they never write.
#include "native_builder_census.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include "../gfx/thirdparty/xxhash.h"
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "native_draw_ledger.hpp"
#include "../app/log.hpp"

namespace sr::native::census {

const bool g_on = [] {
  const char* e = std::getenv("SR_NATIVE_BUILDER_CENSUS");
  return e && e[0] == '1';
}();

namespace {

constexpr int kKinds = 5;
constexpr int kCuts = 5;
const char* const kKindName[kKinds] = {"indexed", "auto", "UP", "begin", "end"};
// 3, 4: found by bc1b (50% of XDK draws outside the first three): the RW null-PS-then-PS object
// draw and the render-context mesh-per-pass helper, both calling the C3 flush-and-draw directly.
const char* const kCutName[kCuts] = {"draw", "extxform", "instanced", "nullps", "ctxpass"};
constexpr uint32_t kDevGlobal = 0x8299ADF4u;  // the XDK device pointer the RW layer draws with

struct Counters {
  // [inCut][zpass][kind]
  std::atomic<uint64_t> draws[2][2][kKinds];
  std::atomic<uint64_t> cutCalls[kCuts], nested, cutTscBy[kCuts];
  std::atomic<uint64_t> cutTsc, xdkInCutTsc;
  std::atomic<uint64_t> objNew, objSame, objChanged, linesChanged, linesTotal, objFallback;
  // Redundancy (orchestrator 2026-10-07): sampled objects (1/4, chosen by address so the same
  // objects are sampled every frame), cut-call rdtsc by outcome. out* = the PM4 bytes the call
  // emitted vs the same object's same occurrence in its previous frame; ob* = the object's bytes.
  std::atomic<uint64_t> sampTsc, outSameTsc, outDiffTsc, outNewTsc, outKickTsc;
  std::atomic<uint64_t> obSameTsc, obChangedTsc, obNewTsc;
  std::atomic<uint64_t> outSame, outDiff, outNew, outKick, outBytes;
  // The same comparison with the ALU float constants (type-0 registers 0x4000..0x47FF) left out:
  // the work a per-object cache (S3) eliminates while still re-uploading changed constants.
  std::atomic<uint64_t> ncSameTsc, ncSame;
  // Order-independent: the device shadow's FULL state after the call (fetch constants, ALU
  // constants, render-state shadows: dev+0x400..0x2A00) vs the same occurrence's previous frame,
  // and the same without the ALU constants (dev+0x700..0x2700). The emitted PM4 above is a delta
  // against whatever was drawn before, so it differs whenever the draw order changes.
  std::atomic<uint64_t> dsSameTsc, dsncSameTsc;
  // Which dwords make the state differ: per dword of [0x400, 0x2A00), how often it changed vs the
  // same occurrence's previous frame; and the calls (ms-weighted) whose changes all fall in the
  // ALU constants [0x700, 0x2700) or fetch-constant slot 31 [0x6E8, 0x700) (dynamic data a
  // per-object cache re-applies each frame) - the "static except dynamic data" share.
  std::atomic<uint64_t> dwCompared, dwChanged[(0x2A00 - 0x400) / 4];
  std::atomic<uint64_t> dynOnlyTsc, dynOnly, constOnlyTsc;
};
Counters g_c;

struct Snap {
  uint64_t draws[2][2][kKinds];
  uint64_t cutCalls[kCuts], nested, cutTsc, xdkInCutTsc, cutTscBy[kCuts];
  uint64_t objNew, objSame, objChanged, linesChanged, linesTotal, objFallback;
  uint64_t sampTsc, outSameTsc, outDiffTsc, outNewTsc, outKickTsc, obSameTsc, obChangedTsc, obNewTsc;
  uint64_t outSame, outDiff, outNew, outKick, outBytes, ncSameTsc, ncSame, dsSameTsc, dsncSameTsc;
  uint64_t dynOnlyTsc, dynOnly, constOnlyTsc;
  uint64_t frames;
  int64_t qpc;
  uint64_t tsc;
};

uint64_t ld(const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); }

Snap take() {
  Snap s{};
  for (int i = 0; i < 2; i++)
    for (int z = 0; z < 2; z++)
      for (int k = 0; k < kKinds; k++) s.draws[i][z][k] = ld(g_c.draws[i][z][k]);
  for (int c = 0; c < kCuts; c++) {
    s.cutCalls[c] = ld(g_c.cutCalls[c]);
    s.cutTscBy[c] = ld(g_c.cutTscBy[c]);
  }
  s.nested = ld(g_c.nested);
  s.cutTsc = ld(g_c.cutTsc);
  s.xdkInCutTsc = ld(g_c.xdkInCutTsc);
  s.objNew = ld(g_c.objNew);
  s.objSame = ld(g_c.objSame);
  s.objChanged = ld(g_c.objChanged);
  s.linesChanged = ld(g_c.linesChanged);
  s.linesTotal = ld(g_c.linesTotal);
  s.objFallback = ld(g_c.objFallback);
  s.sampTsc = ld(g_c.sampTsc);
  s.outSameTsc = ld(g_c.outSameTsc);
  s.outDiffTsc = ld(g_c.outDiffTsc);
  s.outNewTsc = ld(g_c.outNewTsc);
  s.outKickTsc = ld(g_c.outKickTsc);
  s.obSameTsc = ld(g_c.obSameTsc);
  s.obChangedTsc = ld(g_c.obChangedTsc);
  s.obNewTsc = ld(g_c.obNewTsc);
  s.outSame = ld(g_c.outSame);
  s.outDiff = ld(g_c.outDiff);
  s.outNew = ld(g_c.outNew);
  s.outKick = ld(g_c.outKick);
  s.outBytes = ld(g_c.outBytes);
  s.ncSameTsc = ld(g_c.ncSameTsc);
  s.ncSame = ld(g_c.ncSame);
  s.dsSameTsc = ld(g_c.dsSameTsc);
  s.dsncSameTsc = ld(g_c.dsncSameTsc);
  s.dynOnlyTsc = ld(g_c.dynOnlyTsc);
  s.dynOnly = ld(g_c.dynOnly);
  s.constOnlyTsc = ld(g_c.constOnlyTsc);
  s.frames = sr::kernel::getVdSwapCount();
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  s.qpc = q.QuadPart;
  s.tsc = __rdtsc();
  return s;
}

double qpcFreq() {
  static const double f = [] {
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    return double(q.QuadPart);
  }();
  return f;
}

std::mutex g_repMu;
Snap g_start{}, g_win{};
bool g_haveStart = false;
int64_t g_lastCum = 0;

void line(const char* tag, const Snap& a, const Snap& b) {
  const double secs = double(b.qpc - a.qpc) / qpcFreq();
  const double tscPerMs = secs > 0 ? double(b.tsc - a.tsc) / (secs * 1000.0) : 1.0;
  const uint64_t fr = b.frames - a.frames;
  const double perF = fr ? 1.0 / double(fr) : 0.0;
  uint64_t in = 0, out = 0, inZ = 0, outZ = 0;
  char kinds[512];
  size_t o = 0;
  for (int k = 0; k < kKinds; k++) {
    const uint64_t i0 = b.draws[1][0][k] - a.draws[1][0][k], i1 = b.draws[1][1][k] - a.draws[1][1][k];
    const uint64_t o0 = b.draws[0][0][k] - a.draws[0][0][k], o1 = b.draws[0][1][k] - a.draws[0][1][k];
    in += i0 + i1;
    out += o0 + o1;
    inZ += i1;
    outZ += o1;
    o += std::snprintf(kinds + o, sizeof(kinds) - o, " %s=%llu/%llu(z%llu/%llu)", kKindName[k],
                       (unsigned long long)(i0 + i1), (unsigned long long)(o0 + o1),
                       (unsigned long long)i1, (unsigned long long)o1);
    if (o >= sizeof(kinds)) break;
  }
  const double cutMs = double(b.cutTsc - a.cutTsc) / tscPerMs;
  const double xdkMs = double(b.xdkInCutTsc - a.xdkInCutTsc) / tscPerMs;
  const uint64_t on = b.objNew - a.objNew, os = b.objSame - a.objSame, oc = b.objChanged - a.objChanged;
  const uint64_t samples = on + os + oc;
  char cuts[400];
  size_t co = 0;
  for (int c = 0; c < kCuts && co < sizeof(cuts); ++c)
    co += std::snprintf(cuts + co, sizeof(cuts) - co, " %s=%llu(%.3fms)", kCutName[c],
                        (unsigned long long)(b.cutCalls[c] - a.cutCalls[c]),
                        double(b.cutTscBy[c] - a.cutTscBy[c]) / tscPerMs * perF);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_BUILDER_CENSUS] %s %.1fs frames=%llu | draws in_cut=%llu out=%llu share=%.1f%% "
              "(zpass in=%llu out=%llu) per_frame in=%.0f out=%.0f | in/out by kind:%s | cut calls (outermost ms/frame)"
              "%s nested=%llu | ms/frame cut=%.3f xdk_draws_in_cut=%.3f plumbing=%.3f | "
              "objects sampled=%llu new=%llu same=%llu (%.1f%% of repeats) changed=%llu lines_changed=%.1f%% "
              "fallback_range=%llu",
              tag, secs, (unsigned long long)fr, (unsigned long long)in, (unsigned long long)out,
              (in + out) ? 100.0 * double(in) / double(in + out) : 0.0, (unsigned long long)inZ,
              (unsigned long long)outZ, double(in) * perF, double(out) * perF, kinds, cuts,
              (unsigned long long)(b.nested - a.nested),
              cutMs * perF, xdkMs * perF, (cutMs - xdkMs) * perF, (unsigned long long)samples,
              (unsigned long long)on, (unsigned long long)os,
              (os + oc) ? 100.0 * double(os) / double(os + oc) : 0.0, (unsigned long long)oc,
              (b.linesTotal - a.linesTotal)
                  ? 100.0 * double(b.linesChanged - a.linesChanged) / double(b.linesTotal - a.linesTotal)
                  : 0.0,
              (unsigned long long)(b.objFallback - a.objFallback));
  const double st = double(b.sampTsc - a.sampTsc);
  auto pc = [&](uint64_t bv, uint64_t av) { return st > 0 ? 100.0 * double(bv - av) / st : 0.0; };
  const double sameShare = st > 0 ? double(b.outSameTsc - a.outSameTsc) / st : 0.0;
  const uint64_t outN = (b.outSame - a.outSame) + (b.outDiff - a.outDiff) + (b.outNew - a.outNew);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_BUILDER_REDUNDANCY] %s frames=%llu | sampled cut ms/frame=%.3f (objects 1/4 by "
              "address) | emitted PM4 vs previous frame (ms-weighted): same=%.1f%% diff=%.1f%% new=%.1f%% "
              "kicked=%.1f%% (calls same=%llu diff=%llu new=%llu kicked=%llu, avg %.0f B/call) | object bytes "
              "(ms-weighted): same=%.1f%% changed=%.1f%% new=%.1f%% | eliminable_est ms/frame=%.3f "
              "(cut ms x output-same share) | without ALU constants: same=%.1f%% (calls %llu) "
              "cacheable_est ms/frame=%.3f | device state after the call (order-independent): same=%.1f%% "
              "state_same_est ms/frame=%.3f, without ALU constants same=%.1f%% state_nc_same_est ms/frame=%.3f | "
              "changes only in ALU constants=%.1f%% only in constants+fetch31=%.1f%% (calls %llu) static_est ms/frame=%.3f",
              tag, (unsigned long long)fr, st / tscPerMs * perF, pc(b.outSameTsc, a.outSameTsc),
              pc(b.outDiffTsc, a.outDiffTsc), pc(b.outNewTsc, a.outNewTsc), pc(b.outKickTsc, a.outKickTsc),
              (unsigned long long)(b.outSame - a.outSame), (unsigned long long)(b.outDiff - a.outDiff),
              (unsigned long long)(b.outNew - a.outNew), (unsigned long long)(b.outKick - a.outKick),
              outN ? double(b.outBytes - a.outBytes) / double(outN) : 0.0, pc(b.obSameTsc, a.obSameTsc),
              pc(b.obChangedTsc, a.obChangedTsc), pc(b.obNewTsc, a.obNewTsc), cutMs * perF * sameShare,
              pc(b.ncSameTsc, a.ncSameTsc), (unsigned long long)(b.ncSame - a.ncSame),
              st > 0 ? cutMs * perF * double(b.ncSameTsc - a.ncSameTsc) / st : 0.0,
              pc(b.dsSameTsc, a.dsSameTsc), st > 0 ? cutMs * perF * double(b.dsSameTsc - a.dsSameTsc) / st : 0.0,
              pc(b.dsncSameTsc, a.dsncSameTsc),
              st > 0 ? cutMs * perF * double(b.dsncSameTsc - a.dsncSameTsc) / st : 0.0,
              pc(b.constOnlyTsc, a.constOnlyTsc), pc(b.dynOnlyTsc, a.dynOnlyTsc),
              (unsigned long long)(b.dynOnly - a.dynOnly),
              st > 0 ? cutMs * perF * double(b.dynOnlyTsc - a.dynOnlyTsc) / st : 0.0);
  if (std::strcmp(tag, "cumulative") == 0) {
    const uint64_t cmp = ld(g_c.dwCompared);
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (uint32_t i = 0; i < uint32_t((0x2A00 - 0x400) / 4); i++) {
      const uint64_t c = ld(g_c.dwChanged[i]);
      if (c) top.push_back({c, 0x400u + 4u * i});
    }
    std::sort(top.begin(), top.end(), [](auto& x, auto& y) { return x.first > y.first; });
    char buf[1400];
    size_t o = 0;
    uint64_t nConst = 0, nOther = 0;
    for (auto& t : top) (t.second >= 0x700u && t.second < 0x2700u ? nConst : nOther)++;
    o += std::snprintf(buf + o, sizeof(buf) - o, "compared=%llu dwords_ever_changed const=%llu other=%llu | top other:",
                       (unsigned long long)cmp, (unsigned long long)nConst, (unsigned long long)nOther);
    int shown = 0;
    for (auto& t : top) {
      if (t.second >= 0x700u && t.second < 0x2700u) continue;
      if (o >= sizeof(buf) - 24 || shown++ >= 40) break;
      o += std::snprintf(buf + o, sizeof(buf) - o, " +%04X:%.1f%%", t.second, cmp ? 100.0 * t.first / cmp : 0.0);
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_BUILDER_REDUNDANCY] dwords %s", buf);
  }
}

void maybeReport() {
  static std::atomic<int64_t> s_next{0};
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  const int64_t now = q.QuadPart;
  const int64_t nx = s_next.load(std::memory_order_relaxed);
  if (now < nx) return;
  std::unique_lock<std::mutex> lk(g_repMu, std::try_to_lock);
  if (!lk.owns_lock()) return;
  const int64_t step = int64_t(qpcFreq() * 5.0);
  s_next.store(now + step, std::memory_order_relaxed);
  const Snap s = take();
  if (!g_haveStart) {
    g_haveStart = true;
    g_start = g_win = s;
    g_lastCum = now;
    return;
  }
  line("window", g_win, s);
  g_win = s;
  if (now - g_lastCum >= 6 * step) {
    g_lastCum = now;
    line("cumulative", g_start, s);
  }
}

// ---- per thread: cut depth, timing, object sampling ----
struct Tls {
  uint32_t depth = 0;
  uint64_t t0 = 0, xdk0 = 0;
  uint32_t calls = 0;
};
thread_local Tls t_s;

std::mutex g_objMu;
std::unordered_map<uint32_t, std::vector<uint64_t>> g_obj;  // object VA -> 64-byte line hashes

uint32_t rdBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}

uint64_t hashLine(const uint8_t* p, size_t n) { return XXH3_64bits(p, n); }

// 0 new, 1 same, 2 changed, -1 not readable.
int sampleObject(uint32_t obj) {
  const uint8_t* h = sr::kernel::guestVAToHost(obj);
  if (!h) return -1;
  uint32_t end = rdBE32(h + 8);
  uint32_t len = 512;
  if (end > obj + 16u && end - obj <= 0x10000u) len = end - obj;
  else g_c.objFallback.fetch_add(1, std::memory_order_relaxed);
  // The range must stay inside mapped guest memory: check the last byte maps contiguously.
  const uint8_t* hl = sr::kernel::guestVAToHost(obj + len - 1u);
  if (!hl || hl - h != ptrdiff_t(len - 1u)) return -1;
  std::vector<uint64_t> lines((len + 63u) / 64u);
  for (size_t i = 0; i < lines.size(); i++) {
    const size_t o = i * 64u;
    lines[i] = hashLine(h + o, std::min<size_t>(64u, len - o));
  }
  std::lock_guard<std::mutex> lk(g_objMu);
  auto it = g_obj.find(obj);
  if (it == g_obj.end()) {
    if (g_obj.size() < 200000) g_obj.emplace(obj, std::move(lines));
    g_c.objNew.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  std::vector<uint64_t>& prev = it->second;
  uint64_t diff = 0;
  const size_t n = std::max(prev.size(), lines.size());
  for (size_t i = 0; i < n; i++)
    if (i >= prev.size() || i >= lines.size() || prev[i] != lines[i]) diff++;
  g_c.linesTotal.fetch_add(n, std::memory_order_relaxed);
  if (diff) {
    g_c.objChanged.fetch_add(1, std::memory_order_relaxed);
    g_c.linesChanged.fetch_add(diff, std::memory_order_relaxed);
    prev = std::move(lines);
    return 2;
  }
  g_c.objSame.fetch_add(1, std::memory_order_relaxed);
  return 1;
}

bool sampledObject(uint32_t obj) { return ((((obj >> 4) * 0x9E3779B1u) >> 30) == 0u); }

// The sampled object's occurrence index within the current frame (VdSwap count).
std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> g_occ;
// (obj << 8 | occurrence) -> {emitted PM4 hash, the same without ALU float constants}
std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> g_out;
// (obj << 8 | occurrence) -> {device state hash, the same without ALU constants}
std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> g_ds;
// (obj << 8 | occurrence) -> the device state dwords [0x400, 0x2A00) after its previous-frame call
constexpr uint32_t kDw0 = 0x400, kDwN = (0x2A00 - 0x400) / 4;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_dw;

// bit0 = every changed dword is dynamic data (ALU constants or fetch slot 31), bit1 = every changed
// dword is an ALU constant. Only when a previous copy exists (else -1).
int sampleDevDwords(uint64_t key, const uint8_t* d) {
  std::lock_guard<std::mutex> lk(g_objMu);
  auto it = g_dw.find(key);
  if (it == g_dw.end()) {
    if (g_dw.size() < 100000) {
      std::vector<uint32_t> v(kDwN);
      std::memcpy(v.data(), d + kDw0, kDwN * 4u);
      g_dw.emplace(key, std::move(v));
    }
    return -1;
  }
  std::vector<uint32_t>& prev = it->second;
  bool dyn = true, cst = true;
  const uint32_t* cur = reinterpret_cast<const uint32_t*>(d + kDw0);
  for (uint32_t i = 0; i < kDwN; i++) {
    uint32_t v;
    std::memcpy(&v, cur + i, 4);
    if (v == prev[i]) continue;
    prev[i] = v;
    g_c.dwChanged[i].fetch_add(1, std::memory_order_relaxed);
    const uint32_t off = kDw0 + 4u * i;
    const bool isConst = off >= 0x700u && off < 0x2700u;
    if (!isConst) cst = false;
    if (!isConst && !(off >= 0x6E8u && off < 0x700u)) dyn = false;
  }
  g_c.dwCompared.fetch_add(1, std::memory_order_relaxed);
  return (dyn ? 1 : 0) | (cst ? 2 : 0);
}

// Device-state hashes after the call: [0x400, 0x2A00) and that minus [0x700, 0x2700).
// Returns bit0 = same, bit1 = same without constants (vs the previous frame's same occurrence).
int sampleDevState(uint32_t obj, uint32_t occ, uint32_t devVA) {
  const uint8_t* d = sr::kernel::guestVAToHost(devVA);
  const uint8_t* e = sr::kernel::guestVAToHost(devVA + 0x2A00u - 1u);
  if (!d || !e || e - d != ptrdiff_t(0x2A00u - 1u)) return 0;
  const uint64_t a = XXH3_64bits(d + 0x400, 0x300);
  const uint64_t c = XXH3_64bits(d + 0x700, 0x2000);
  const uint64_t r = XXH3_64bits(d + 0x2700, 0x300);
  const uint64_t all = a ^ (c * 0x9E3779B97F4A7C15ull) ^ (r * 0xC2B2AE3D27D4EB4Full);
  const uint64_t nc = a ^ (r * 0xC2B2AE3D27D4EB4Full);
  const uint64_t key = (uint64_t(obj) << 8) | std::min<uint32_t>(occ, 255u);
  std::lock_guard<std::mutex> lk(g_objMu);
  auto it = g_ds.find(key);
  if (it == g_ds.end()) {
    if (g_ds.size() < 400000) g_ds.emplace(key, std::make_pair(all, nc));
    return 0;
  }
  const int res = (it->second.first == all ? 1 : 0) | (it->second.second == nc ? 2 : 0);
  it->second = {all, nc};
  return res;
}

// The emitted stream walked as PM4 (XDK output, well-formed): hash everything except the payload
// of type-0 writes into the ALU float constants 0x4000..0x47FF (header kept, so the layout counts).
uint64_t hashNonConst(const uint8_t* h, uint32_t len) {
  XXH3_state_t st;
  XXH3_64bits_reset(&st);
  uint32_t off = 0, from = 0;
  while (off + 4u <= len) {
    const uint32_t hdr = rdBE32(h + off);
    const uint32_t type = hdr >> 30;
    uint32_t n = 1;
    if (type == 0u) {
      n = 1u + (((hdr >> 16) & 0x3FFFu) + 1u);
      const uint32_t reg = hdr & 0x7FFFu;
      if (reg >= 0x4000u && reg < 0x4800u && off + 4u * n <= len) {
        XXH3_64bits_update(&st, h + from, off + 4u - from);  // up to and including the header
        from = off + 4u * n;
      }
    } else if (type == 1u) {
      n = 3;
    } else if (type == 3u) {
      n = 1u + (((hdr >> 16) & 0x3FFFu) + 1u);
    }
    off += 4u * n;
  }
  if (from < len) XXH3_64bits_update(&st, h + from, len - from);
  return XXH3_64bits_digest(&st);
}

uint32_t occurrence(uint32_t obj) {
  const uint64_t f = sr::kernel::getVdSwapCount();
  std::lock_guard<std::mutex> lk(g_objMu);
  auto& e = g_occ[obj];
  if (e.first != f) e = {f, 0u};
  return e.second++;
}

// 0 new, 1 same, 2 diff, 3 kicked / not hashable; ncSame = equal without the ALU constants.
int sampleOutput(uint32_t obj, uint32_t occ, uint32_t c0, uint32_t c1, bool kicked, bool& ncSame) {
  ncSame = false;
  if (kicked || c1 < c0 || c1 - c0 > (1u << 20)) return 3;
  const uint32_t len = c1 - c0;
  uint64_t hsh = 0, nc = 0;
  if (len) {
    const uint8_t* h0 = sr::kernel::guestVAToHost(c0);
    const uint8_t* h1 = sr::kernel::guestVAToHost(c1 - 1u);
    if (!h0 || !h1 || h1 - h0 != ptrdiff_t(len - 1u)) return 3;
    hsh = XXH3_64bits(h0, len);
    nc = hashNonConst(h0, len);
  }
  g_c.outBytes.fetch_add(len, std::memory_order_relaxed);
  const uint64_t key = (uint64_t(obj) << 8) | std::min<uint32_t>(occ, 255u);
  std::lock_guard<std::mutex> lk(g_objMu);
  auto it = g_out.find(key);
  if (it == g_out.end()) {
    if (g_out.size() < 400000) g_out.emplace(key, std::make_pair(hsh, nc));
    return 0;
  }
  ncSame = it->second.second == nc;
  if (it->second.first == hsh) return 1;
  it->second = {hsh, nc};
  return 2;
}

uint32_t devPtr() {
  const uint8_t* h = sr::kernel::guestVAToHost(kDevGlobal);
  return h ? rdBE32(h) : 0u;
}

struct CutScope {
  bool outer = false, on = false, sampled = false;
  int cut = 0;
  uint32_t dev = 0, obj = 0, occ = 0, c0 = 0, seg0 = 0, devS = 0;
  int obStatus = -1;
  CutScope(int cut, uint32_t obj) {
    if (!g_on) return;
    on = true;
    this->cut = cut;
    g_c.cutCalls[cut].fetch_add(1, std::memory_order_relaxed);
    Tls& t = t_s;
    if (t.depth++) {
      g_c.nested.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    outer = true;
    this->obj = obj;
    sampled = sampledObject(obj);
    if (sampled) {
      obStatus = sampleObject(obj);
      occ = occurrence(obj);
      devS = devPtr();
      if (const uint8_t* d = devS ? sr::kernel::guestVAToHost(devS) : nullptr) {
        c0 = rdBE32(d + 0x28);
        seg0 = rdBE32(d + 13508);
      } else {
        sampled = false;
      }
    }
    if (sr::native::ledger::g_on) {
      dev = devPtr();
      if (dev) sr::native::ledger::before(dev, 5);
    }
    t.t0 = __rdtsc();
  }
  ~CutScope() {
    if (!on) return;
    Tls& t = t_s;
    t.depth--;
    if (!outer) return;
    const uint64_t dur = __rdtsc() - t.t0;
    g_c.cutTsc.fetch_add(dur, std::memory_order_relaxed);
    g_c.cutTscBy[cut].fetch_add(dur, std::memory_order_relaxed);
    if (sampled) {
      if (const uint8_t* d = sr::kernel::guestVAToHost(devS)) {
        const uint32_t c1 = rdBE32(d + 0x28), seg1 = rdBE32(d + 13508);
        bool ncSame = false;
        const int os = sampleOutput(obj, occ, c0, c1, seg1 != seg0, ncSame);
        if (ncSame) {
          g_c.ncSameTsc.fetch_add(dur, std::memory_order_relaxed);
          g_c.ncSame.fetch_add(1, std::memory_order_relaxed);
        }
        const int ds = sampleDevState(obj, occ, devS);
        if (const uint8_t* dd = sr::kernel::guestVAToHost(devS)) {
          const int dw = sampleDevDwords((uint64_t(obj) << 8) | std::min<uint32_t>(occ, 255u), dd);
          if (dw >= 0 && (dw & 1)) {
            g_c.dynOnlyTsc.fetch_add(dur, std::memory_order_relaxed);
            g_c.dynOnly.fetch_add(1, std::memory_order_relaxed);
          }
          if (dw >= 0 && (dw & 2)) g_c.constOnlyTsc.fetch_add(dur, std::memory_order_relaxed);
        }
        if (ds & 1) g_c.dsSameTsc.fetch_add(dur, std::memory_order_relaxed);
        if (ds & 2) g_c.dsncSameTsc.fetch_add(dur, std::memory_order_relaxed);
        g_c.sampTsc.fetch_add(dur, std::memory_order_relaxed);
        std::atomic<uint64_t>* const outT[4] = {&g_c.outNewTsc, &g_c.outSameTsc, &g_c.outDiffTsc,
                                                &g_c.outKickTsc};
        std::atomic<uint64_t>* const outC[4] = {&g_c.outNew, &g_c.outSame, &g_c.outDiff, &g_c.outKick};
        outT[os]->fetch_add(dur, std::memory_order_relaxed);
        outC[os]->fetch_add(1, std::memory_order_relaxed);
        if (obStatus >= 0) {
          std::atomic<uint64_t>* const obT[3] = {&g_c.obNewTsc, &g_c.obSameTsc, &g_c.obChangedTsc};
          obT[obStatus]->fetch_add(dur, std::memory_order_relaxed);
        }
      }
    }
    if (dev) sr::native::ledger::after(dev, 5);
    maybeReport();
  }
};

}  // namespace

void drawBefore(uint32_t devVA, int kind) {
  if (kind < 0 || kind >= kKinds) return;
  Tls& t = t_s;
  bool z = false;
  if (const uint8_t* h = sr::kernel::guestVAToHost(devVA + 10808u)) z = (h[0] & 0x42u) != 0;  // BeginZPass: byte |= 0x40
  g_c.draws[t.depth ? 1 : 0][z ? 1 : 0][kind].fetch_add(1, std::memory_order_relaxed);
  if (t.depth) t.xdk0 = __rdtsc();
}

void drawAfter(int kind) {
  Tls& t = t_s;
  if (kind < 0 || kind >= kKinds) return;
  if (t.depth) g_c.xdkInCutTsc.fetch_add(__rdtsc() - t.xdk0, std::memory_order_relaxed);
  else maybeReport();
}

}  // namespace sr::native::census

// ---- the cut: strong-def overrides, observe-only (always delegate to the guest body) ----

extern "C" void __imp__sub_8261D0A0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8261D0A0(PPCContext& __restrict ctx, uint8_t* base) {
  sr::native::census::CutScope s(0, ctx.r3.u32);  // SR_NATIVE_BUILDER_CENSUS; no-op when off
  __imp__sub_8261D0A0(ctx, base);
}

extern "C" void __imp__sub_8261CD90(PPCContext& __restrict ctx, uint8_t* base);
void sub_8261CD90(PPCContext& __restrict ctx, uint8_t* base) {
  sr::native::census::CutScope s(1, ctx.r3.u32);  // SR_NATIVE_BUILDER_CENSUS; no-op when off
  __imp__sub_8261CD90(ctx, base);
}

extern "C" void __imp__sub_8261CED0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8261CED0(PPCContext& __restrict ctx, uint8_t* base) {
  sr::native::census::CutScope s(2, ctx.r3.u32);  // SR_NATIVE_BUILDER_CENSUS; no-op when off
  __imp__sub_8261CED0(ctx, base);
}

extern "C" void __imp__sub_82617798(PPCContext& __restrict ctx, uint8_t* base);
void sub_82617798(PPCContext& __restrict ctx, uint8_t* base) {
  sr::native::census::CutScope s(3, ctx.r3.u32);  // RwRenderObjectDrawNullPsThenPs; no-op when off
  __imp__sub_82617798(ctx, base);
}

extern "C" void __imp__sub_822F8710(PPCContext& __restrict ctx, uint8_t* base);
void sub_822F8710(PPCContext& __restrict ctx, uint8_t* base) {
  sr::native::census::CutScope s(4, ctx.r3.u32);  // RenderCtxDrawMeshForPass; no-op when off
  __imp__sub_822F8710(ctx, base);
}
