// iter-463 D-AUDIO-CMDBUF recurrence (bc1f8fc roll1, SR_XMA_NEW on): observe probes and named
// stress diagnostics. Contract and gates: kernel/audio_cmdq_obs.hpp.
//
// WHAT THE RECURRENCE LOOKED LIKE (archive/v1_invest/collision/route/roll1/attempt1/run.log):
//   * 15:50:29.691 the DAC thread ("RWAudioCore Dac", guest tid 47) entered
//     EaStrm_DequeueReadyBlock_sub_825FC760 three times with a DEAD reader: [CS_GARBAGE]
//     cs=4, 4, 3 at caller_lr 825FC788 (CS = reader->owner + 4, so owner = 0, 0, 0xFFFFFFFF).
//   * It never waited on its wake event again; RwacSystem_ProcessCmdQueue_sub_8264C540 (the
//     ONLY consumer of the 1 MB RWAC command queue) stopped; producers overran the queue in
//     ~8.2 s into EA's objects; ICALL_FAIL at 823399D4 on object 87337770 (vt 8264CE28).
//   * 3 of the 4 healthy rolls of the same exe also took the dead-reader path (cs=4 only,
//     thousands per burst) and survived because the zeroed reader has reader+12 == 0.
//   * Xenia (the oracle) runs protect_zero = true: [0, 0x10000) is no-access
//     (memory.cc:235), so a null-page reader read would be fatal there; the oracle played
//     ~40 min of gameplay without one. The dead reader is a divergence UPSTREAM of the
//     queue; our writable null page only hides it.
//
// WHAT THIS FILE DOES (nothing here writes guest memory except S2, which is master-gated):
//   [RWAC_Q]        1 Hz from a host wall-clock thread: queue ticks/write/high-water/capacity,
//                   drains seen by the consumer override, DAC thread lr/r1/r3/ctr, XMA kick
//                   delta (total + the top 3 contexts), [EA_RDR] call/anomaly deltas.
//   [RWAC_Q_STALL]  ticks frozen >= 250 ms while XAudio render callbacks advance; the first
//                   stall and the first time the write offset passes 50% of capacity call
//                   dumpGuestThreadsNow.
//   [EA_RDR]        strong-def observe overrides on sub_82654BE0 (stream player: next block
//                   for request r4, reader = *(r4+64)) and sub_825FC760 (the dequeue): when
//                   the reader is in the null page or not a plausible object, or its owner
//                   (reader+4) is not, log the request, the reader words, the caller and the
//                   last time this request had a healthy reader. Both delegate unconditionally.
//   S1 SR_PROTECT_ZERO=1          Xenia's protect_zero: guest [0, 0x10000) no-access. The
//                                 first dead-reader read becomes a VEH crash with full guest
//                                 context; [EA_RDR] logs the request BEFORE the guest read.
//   S2 SR_DIAG_NULLPAGE_POISON=N  FORCING, roll it ALONE: SR_ALLOW_FORCING=1
//                                 SR_FORCING_ISOLATE=SR_DIAG_NULLPAGE_POISON (plain
//                                 SR_ALLOW_FORCING=1 also mutes sound via cfgSoundOffThread).
//                                 Fills guest [0, N) with 0xFF every
//                                 100 ms so every dead-reader call takes roll1's garbage branch.
//                                 Causal mutation test: the wedge + ICALL should then follow
//                                 the FIRST dead-reader burst instead of 1 roll in 5.
//   S3 SR_DIAG_DAC_WAKE_DELAY_US=N  host delay before each RWAC drain on the DAC thread
//                                 (thread-ordering / priority sensitivity). Control:
//                                 SR_DIAG_IO_DELAY_MS on sound.AST reads (disk-feed starvation).
#include "audio_cmdq_obs.hpp"
#include "ntavm_xenia_heap.hpp"

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "guest_protect.hpp"
#include "apu/audio_system.hpp"
#include "apu/xma_context_new.hpp"
#include "apu/xma_device.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// The all-guest-threads registry (kernel/guest_context.cpp, exported for Frida).
extern "C" sr::kernel::GuestThreadSlot g_debugThreadRegistry[64];

namespace sr {
namespace kernel {
namespace cmdq {

namespace {

// ── gates (each predicate names its own getenv so the override manifest sees it) ─────────────
uint32_t parseU32(const char* e, uint32_t dflt) {
  return (e && e[0]) ? (uint32_t)std::strtoul(e, nullptr, 0) : dflt;
}
bool obsOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_RWAC_Q_OBS");
    return !e || e[0] != '0';                              // observe only: default ON
  }();
  return v;
}
bool protectZeroRequested() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PROTECT_ZERO");
    return e && e[0] == '1';                               // S1: default OFF
  }();
  return v;
}
uint32_t poisonBytesRequested() {
  static const uint32_t v = [] {
    uint32_t n = parseU32(std::getenv("SR_DIAG_NULLPAGE_POISON"), 0);   // S2: default OFF
    if (n == 1) n = 0x40;
    return std::min<uint32_t>(n, kNullPageEnd);
  }();
  return v;
}
uint32_t dacDelayUs() {
  static const uint32_t v = parseU32(std::getenv("SR_DIAG_DAC_WAKE_DELAY_US"), 0);   // S3: OFF
  return v;
}
uint32_t ioDelayMs() {
  static const uint32_t v = parseU32(std::getenv("SR_DIAG_IO_DELAY_MS"), 0);   // S3 control: OFF
  return v;
}
uint32_t protectZeroDelayMs() {
  static const uint32_t v = parseU32(std::getenv("SR_PROTECT_ZERO_DELAY_S"), 0) * 1000u;
  return v;
}

std::atomic<bool> g_zeroArmed{false};        // S1 applied: never touch [0, 0x10000) from here
std::atomic<uint32_t> g_dacHostTid{0};       // the thread that runs the RWAC drain
std::atomic<uint64_t> g_drains{0};
std::atomic<uint64_t> g_rdrCalls{0}, g_rdrBad{0}, g_deqCalls{0}, g_deqBadOther{0};
thread_local uint32_t t_curReq = 0;          // request of the enclosing sub_82654BE0 call

uint64_t nowMs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Guest reads. Every address read here is one the guest itself reads in the same call
// (request+64/+68, reader+4) or is checked with hostRangeReadableCached before a log line;
// the null page is skipped once S1 has made it no-access.
bool inNullPage(uint32_t va) { return aliasGuestVA(va) < kNullPageEnd; }
bool rd32(uint32_t va, uint32_t& out) {
  if (g_zeroArmed.load(std::memory_order_relaxed) && (inNullPage(va) || inNullPage(va + 3)))
    return false;
  uint8_t* h = guestVAToHost(va);
  if (!h) return false;
  out = __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(h));
  return true;
}
bool rd32Checked(uint32_t va, uint32_t& out) {
  if (g_zeroArmed.load(std::memory_order_relaxed) && (inNullPage(va) || inNullPage(va + 3)))
    return false;
  uint8_t* h = guestVAToHost(va);
  if (!h || !hostRangeReadableCached(h, 4)) return false;
  out = __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(h));
  return true;
}
bool rdBytesChecked(uint32_t va, uint8_t* out, uint32_t n) {
  if (g_zeroArmed.load(std::memory_order_relaxed) && (inNullPage(va) || inNullPage(va + n - 1)))
    return false;
  uint8_t* h0 = guestVAToHost(va);
  uint8_t* h1 = guestVAToHost(va + n - 1);
  if (!h0 || !h1 || h1 - h0 != (ptrdiff_t)(n - 1) || !hostRangeReadableCached(h0, n)) return false;
  std::memcpy(out, h0, n);
  return true;
}

// ── [EA_RDR] request table: the last HEALTHY reader each request was seen with ──────────────
struct ReqSlot {
  uint32_t req = 0, reader = 0, owner = 0, lr = 0;
  uint64_t okMs = 0, okCount = 0;
  bool dumped = false;
};
constexpr size_t kReqSlots = 1024;
std::mutex g_reqMu;
std::array<ReqSlot, kReqSlots> g_req;

ReqSlot* reqFindOrClaim(uint32_t req) {   // g_reqMu held
  size_t h = (size_t)(((req >> 3) * 2654435761u) >> 22) & (kReqSlots - 1);
  for (size_t i = 0; i < 16; ++i) {
    ReqSlot& s = g_req[(h + i) & (kReqSlots - 1)];
    if (s.req == req) return &s;
    if (s.req == 0) { s = ReqSlot{}; s.req = req; return &s; }
  }
  ReqSlot& s = g_req[h];   // full neighbourhood: recycle the home slot
  s = ReqSlot{};
  s.req = req;
  return &s;
}

const char* siteName(uint32_t lr) {
  switch (lr) {
    case 0x82654D24u: case 0x82654DA8u: return "CB0";     // sub_82654CB0 (end of resident head)
    case 0x82654EBCu: case 0x82654FB4u: return "DD0";     // sub_82654DD0 (streamed refill)
    case 0x826550B8u: return "FF0";                       // sub_82654FF0 (request start)
    case 0x82655564u: return "PLAY";                      // player sub_82655170
    case 0x82654C60u: return "BE0";                       // the dequeue inside sub_82654BE0
    case 0x8235C47Cu: return "FMV";                       // FmvDecodeFrameSubmit_sub_8235C460
    case 0x826578E4u: return "SCHL";                      // SCHl feeder sub_826577F8
    default: return "?";
  }
}

bool logGate(std::atomic<uint64_t>& counter, uint64_t& nOut) {
  const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  nOut = n;
  return n <= 64 || (n & (n - 1)) == 0;
}

void logReader(const char* tag, uint64_t n, uint32_t req, uint32_t lr, uint32_t reader,
               uint32_t owner, bool ownerKnown, bool firstForReq, const ReqSlot* last) {
  uint32_t w[5] = {};
  bool wOk = rdBytesChecked(reader, reinterpret_cast<uint8_t*>(w), 20);
  for (uint32_t& x : w) x = __builtin_bswap32(x);
  char reqBuf[200] = "-";
  if (req) {
    uint32_t f28 = 0, f36 = 0, f40 = 0, f44 = 0, f48 = 0, f64 = 0, f68 = 0;
    uint8_t b[8] = {};
    const bool ok = rd32Checked(req + 28, f28) && rd32Checked(req + 36, f36) &&
                    rd32Checked(req + 40, f40) && rd32Checked(req + 44, f44) &&
                    rd32Checked(req + 48, f48) && rd32Checked(req + 64, f64) &&
                    rd32Checked(req + 68, f68) && rdBytesChecked(req + 72, b, 8);
    if (ok)
      std::snprintf(reqBuf, sizeof(reqBuf),
                    "+28=%08X +36=%08X +40=%08X +44=%08X +48=%08X +64=%08X +68=%08X "
                    "b72..79=%02X%02X%02X%02X%02X%02X%02X%02X(mode=%u)",
                    f28, f36, f40, f44, f48, f64, f68, b[0], b[1], b[2], b[3], b[4], b[5], b[6],
                    b[7], b[2]);
    else
      std::snprintf(reqBuf, sizeof(reqBuf), "(request unreadable)");
  }
  char lastBuf[128] = "none";
  if (last && last->okCount)
    std::snprintf(lastBuf, sizeof(lastBuf), "reader=%08X owner=%08X age_ms=%llu n=%llu lr=%08X",
                  last->reader, last->owner, (unsigned long long)(nowMs() - last->okMs),
                  (unsigned long long)last->okCount, last->lr);
  // CsEnterWithDeadline_sub_825FC108's deadline globals (-1 = blocking enter; any other value
  // can return -2 without the +32 increment the caller then undoes - hypothesis H-DL).
  uint32_t dl0 = 0, dl1 = 0;
  const bool dlOk = rd32(0x82074190u, dl0) && rd32(0x8207FDE0u, dl1);
  char ownBuf[16];
  if (ownerKnown) std::snprintf(ownBuf, sizeof(ownBuf), "%08X", owner);
  else std::snprintf(ownBuf, sizeof(ownBuf), "unread");
  char wBuf[64] = "unreadable";
  if (wOk) std::snprintf(wBuf, sizeof(wBuf), "%08X %08X %08X %08X %08X", w[0], w[1], w[2], w[3], w[4]);
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[%s] #%llu%s site=%s caller_lr=%08X tid=%u htid=%lu req=%08X reader=%08X owner=%s "
              "rdr[0..4]=%s req{%s} last_ok{%s} deadline{%08X %08X}%s zero_armed=%d",
              tag, (unsigned long long)n, firstForReq ? " FIRST" : "", siteName(lr), lr,
              getCurrentThreadHandle(), (unsigned long)GetCurrentThreadId(), req, reader, ownBuf,
              wBuf, reqBuf, lastBuf, dl0, dl1, dlOk ? "" : "(unread)", g_zeroArmed.load() ? 1 : 0);
  if (firstForReq && req) {   // the whole request object once: 0x60 bytes
    uint8_t d[0x60];
    if (rdBytesChecked(req, d, sizeof(d))) {
      char hex[0x60 * 2 + 8];
      for (size_t i = 0; i < sizeof(d); ++i) std::snprintf(hex + 2 * i, 3, "%02X", d[i]);
      SR_LOG_BOOT(sr::LogSeverity::Error, "[EA_RDR_REQ] req=%08X bytes[0..5F]=%s", req, hex);
    }
  }
}

// Shared by both overrides. Returns true when the call was anomalous.
void checkReader(uint32_t req, uint32_t reader, uint32_t lr, bool fromBE0) {
  uint32_t owner = 0;
  bool ownerKnown = false;
  // The guest reads reader+4 next (sub_825F82B8 / sub_825FC760). Read it ourselves only
  // where that read cannot fault: a plausible object, or the (still writable) null page.
  if (guestObjectPlausible(reader) || (inNullPage(reader) && !g_zeroArmed.load()))
    ownerKnown = rd32(reader + 4, owner);
  const bool bad = readerAnomalous(reader, owner, ownerKnown);
  if (!bad) {
    if (req) {
      std::lock_guard<std::mutex> g(g_reqMu);
      ReqSlot* s = reqFindOrClaim(req);
      s->reader = reader; s->owner = owner; s->lr = lr; s->okMs = nowMs(); s->okCount++;
    }
    return;
  }
  if (fromBE0) {
    static std::atomic<uint64_t> s_n{0};
    uint64_t n = 0;
    bool first = false;
    ReqSlot last{};
    {
      std::lock_guard<std::mutex> g(g_reqMu);
      ReqSlot* s = reqFindOrClaim(req);
      last = *s;
      if (!s->dumped) { s->dumped = true; first = true; }
    }
    g_rdrBad.fetch_add(1, std::memory_order_relaxed);
    if (logGate(s_n, n) || first)
      logReader("EA_RDR", n, req, lr, reader, owner, ownerKnown, first, &last);
  } else {
    static std::atomic<uint64_t> s_n{0};
    uint64_t n = 0;
    g_deqBadOther.fetch_add(1, std::memory_order_relaxed);
    if (logGate(s_n, n))
      logReader("EA_RDR_DEQ", n, 0, lr, reader, owner, ownerKnown, false, nullptr);
  }
}

// sub_82654BE0 entry (request r4): the reader is *(request+64).
void onGetBlock(uint32_t req, uint32_t lr) {
  g_rdrCalls.fetch_add(1, std::memory_order_relaxed);
  t_curReq = req;
  uint32_t reader = 0;
  if (rd32(req + 64, reader)) {
    checkReader(req, reader, lr, true);
  } else if (g_zeroArmed.load() && inNullPage(req + 64)) {
    // S1: the guest's own read of request+64 is about to fault; name it first.
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[EA_RDR] request itself in the null page: req=%08X caller_lr=%08X site=%s tid=%u",
                req, lr, siteName(lr), getCurrentThreadHandle());
  }
}

// ── the wall-clock watcher ─────────────────────────────────────────────────────────────────
bool sampleDac(uint32_t out[4]) {
  const uint32_t tid = g_dacHostTid.load(std::memory_order_relaxed);
  if (!tid) return false;
  for (int i = 0; i < 64; ++i) {
    const GuestThreadSlot& s = g_debugThreadRegistry[i];
    if (s.tid != tid) continue;
    void* ctx = s.ctx;
    if (!ctx) return false;
    const uint32_t offs[4] = {s.lrOff, s.r1Off, s.r3Off, s.ctrOff};
    uint32_t maxOff = 0;
    for (uint32_t o : offs) maxOff = std::max(maxOff, o);
    if (!hostRangeReadable(ctx, size_t(maxOff) + 4)) return false;
    for (int k = 0; k < 4; ++k)
      out[k] = offs[k] ? *reinterpret_cast<volatile uint32_t*>(static_cast<char*>(ctx) + offs[k]) : 0;
    return true;
  }
  return false;
}

struct QSnap {
  bool ok = false;
  uint32_t q = 0, ticks = 0, buf = 0, cap = 0, wr = 0, hw = 0;
};
QSnap readQueue() {
  QSnap s;
  if (!rd32(kRwacSingletonVA, s.q) || !guestObjectPlausible(s.q)) return s;
  uint8_t* h = guestVAToHost(s.q);
  if (!h || !hostRangeReadableCached(h, kQHighWater + 4)) return s;
  s.ok = rd32(s.q + kQTicks, s.ticks) && rd32(s.q + kQBuffer, s.buf) &&
         rd32(s.q + kQCapacity, s.cap) && rd32(s.q + kQWrite, s.wr) &&
         rd32(s.q + kQHighWater, s.hw);
  return s;
}

void watcherMain(uint8_t* guestBase, size_t guestSize) {
  if (guestBase) setGuestMemory(guestBase, guestSize);
  const uint64_t t0 = nowMs();
  const uint32_t pzDelayMs = protectZeroDelayMs();
  bool pzDone = !protectZeroRequested();
  uint64_t lastSumMs = t0;
  uint32_t lastTicksSum = 0;
  uint64_t lastDrains = 0, lastCb = 0, lastKicks = 0, lastRdrCalls = 0, lastRdrBad = 0,
           lastDeq = 0, lastDeqBad = 0;
  std::array<uint64_t, apu::kXmaContextCount> lastCtxKick{};
  StallDetector det;
  uint64_t stallN = 0;
  bool dumpedStall = false, dumpedHalf = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t now = nowMs();

    // S1: arm protect_zero (once).
    if (!pzDone && now - t0 >= pzDelayMs) {
      pzDone = true;
      const bool ok = protectRange(guestBase, kNullPageEnd);
      if (ok) g_zeroArmed.store(true);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[PROTECT_ZERO] guest [0, 0x10000) -> no-access %s (Xenia protect_zero; S1 "
                  "diagnostic, delay %u ms after the first RWAC drain)",
                  ok ? "ARMED" : "FAILED", pzDelayMs);
    }
    if (!obsOn()) continue;

    const QSnap q = readQueue();
    const uint64_t cb = apu::AudioSystem::instance().stats().callbacks;
    if (q.ok) {
      const StallDetector::Event ev = det.feed(now, q.ticks, cb);
      if (ev == StallDetector::kStart) {
        ++stallN;
        uint32_t r[4] = {};
        const bool rOk = sampleDac(r);
        SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[RWAC_Q_STALL] START #%llu ticks=%u frozen_ms=%llu cb_during=%llu wr=%X cap=%X "
                    "dac_htid=%u dac{lr=%08X r1=%08X r3=%08X ctr=%08X}%s",
                    (unsigned long long)stallN, q.ticks,
                    (unsigned long long)(now - det.lastTickChangeMs),
                    (unsigned long long)(cb - det.cbAtLastTickChange), q.wr, q.cap,
                    g_dacHostTid.load(), r[0], r[1], r[2], r[3], rOk ? "" : " (dac unsampled)");
        if (!dumpedStall) { dumpedStall = true; dumpGuestThreadsNow("RWAC_Q_STALL_THREADS"); }
      } else if (ev == StallDetector::kEnd) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[RWAC_Q_STALL] END #%llu after %llu ms ticks=%u wr=%X",
                    (unsigned long long)stallN, (unsigned long long)(now - det.stallStartMs),
                    q.ticks, q.wr);
      }
      if (!dumpedHalf && q.cap && q.wr > q.cap / 2) {
        dumpedHalf = true;
        SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[RWAC_Q_STALL] HALF write offset %X > 50%% of capacity %X (overrun follows)",
                    q.wr, q.cap);
        dumpGuestThreadsNow("RWAC_Q_HALF_THREADS");
      }
    }

    if (now - lastSumMs < 1000) continue;
    const uint64_t dtMs = now - lastSumMs;
    lastSumMs = now;
    uint32_t r[4] = {};
    const bool rOk = sampleDac(r);
    // XMA kicks: total + the top 3 contexts by delta.
    char xbuf[160] = "xma=off";
    if (apu::xmaNewEnabled()) {
      apu::XmaDevice& d = apu::XmaDevice::instance();
      const uint64_t kicks = d.stats().kicks;
      std::array<std::pair<uint64_t, uint32_t>, 3> top{};
      for (uint32_t i = 0; i < apu::kXmaContextCount; ++i) {
        apu::XmaContextNew* c = d.context(i);
        const uint64_t k = c ? c->kick_seq() : 0;
        const uint64_t dk = k - lastCtxKick[i];
        lastCtxKick[i] = k;
        if (dk > top[2].first) {
          top[2] = {dk, i};
          std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        }
      }
      std::snprintf(xbuf, sizeof(xbuf), "xma_kicks=+%llu top{%u:+%llu %u:+%llu %u:+%llu}",
                    (unsigned long long)(kicks - lastKicks), top[0].second,
                    (unsigned long long)top[0].first, top[1].second,
                    (unsigned long long)top[1].first, top[2].second,
                    (unsigned long long)top[2].first);
      lastKicks = kicks;
    }
    const uint64_t drains = g_drains.load(), rc = g_rdrCalls.load(), rb = g_rdrBad.load(),
                   dq = g_deqCalls.load(), db = g_deqBadOther.load();
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RWAC_Q] dt_ms=%llu q=%08X ticks=%u(+%u) drains=+%llu wr=%X hw=%X cap=%X buf=%08X "
                "cb=+%llu dac_htid=%u dac{lr=%08X r1=%08X r3=%08X ctr=%08X}%s %s "
                "ea_rdr=+%llu bad=+%llu deq=+%llu deq_bad_other=+%llu stalls=%llu zero_armed=%d",
                (unsigned long long)dtMs, q.q, q.ticks, q.ticks - lastTicksSum,
                (unsigned long long)(drains - lastDrains), q.wr, q.hw, q.cap, q.buf,
                (unsigned long long)(cb - lastCb), g_dacHostTid.load(), r[0], r[1], r[2], r[3],
                rOk ? "" : "(unsampled)", xbuf, (unsigned long long)(rc - lastRdrCalls),
                (unsigned long long)(rb - lastRdrBad), (unsigned long long)(dq - lastDeq),
                (unsigned long long)(db - lastDeqBad), (unsigned long long)stallN,
                g_zeroArmed.load() ? 1 : 0);
    lastTicksSum = q.ticks; lastDrains = drains; lastCb = cb; lastRdrCalls = rc; lastRdrBad = rb;
    lastDeq = dq; lastDeqBad = db;
  }
}

// S2 (FORCING): its own thread, LAUNCHED only inside the master gate, so the forcing audit
// sees a master-gated launch rather than a forcing branch inside an observe thread.
void poisonMain(uint8_t* guestBase, size_t guestSize, uint32_t n) {
  if (guestBase) setGuestMemory(guestBase, guestSize);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[NULLPAGE_POISON] FORCED (diagnostic): guest [0, 0x%X) := 0xFF every 100 ms", n);
  for (;;) {
    std::memset(guestBase + 0, 0xFF, n);   // guest VA [0, n)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void ensureWatcher() {
  static std::atomic<bool> s_started{false};
  if (s_started.load(std::memory_order_relaxed)) return;
  if (!obsOn() && !protectZeroRequested() && !poisonBytesRequested()) return;
  if (s_started.exchange(true)) return;
  uint8_t* gb = getGuestBase();
  const size_t gs = getGuestSize();
  std::thread([gb, gs] { watcherMain(gb, gs); }).detach();
  if (const uint32_t n = poisonBytesRequested()) {
    if (protectZeroRequested()) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[NULLPAGE_POISON] ignored: SR_PROTECT_ZERO is set");
    } else if (gb && sr::kernel::forcingAllowedFor("SR_DIAG_NULLPAGE_POISON")) {
      std::thread(poisonMain, gb, gs, n).detach();
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NULLPAGE_POISON] requested (%u bytes) but DENIED: it writes guest memory "
                  "(forcing) - needs SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_DIAG_NULLPAGE_POISON", n);
    }
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[RWAC_Q] watcher started (obs=%d protect_zero=%d poison=%u dac_delay_us=%u "
              "io_delay_ms=%u)", obsOn() ? 1 : 0, protectZeroRequested() ? 1 : 0,
              poisonBytesRequested(), dacDelayUs(), ioDelayMs());
}

void delayUs(uint32_t us) {
  if (us >= 2000) { Sleep(us / 1000); return; }
  LARGE_INTEGER f, s, n;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&s);
  do {
    YieldProcessor();
    QueryPerformanceCounter(&n);
  } while ((uint64_t)(n.QuadPart - s.QuadPart) * 1000000ull / (uint64_t)f.QuadPart < us);
}

// S3 control: handles opened on a matching path.
std::mutex g_ioMu;
std::map<uint32_t, bool> g_ioHandles;

}  // namespace

// ── public pure pieces (difftest --cmdq-test) ───────────────────────────────────────────────
bool guestObjectPlausible(uint32_t va) {
  // Image statics + every heap / physical window the game's objects live in
  // ([0x82000000, 0xC0000000): image, fallback + primary heap, phys arena, thread stacks).
  // iter-463 SR_NTAVM_XENIA_HEAP: kernel-placed title arenas live in Xenia's
  // v00000000 / v40000000 heaps; a live region there holds real objects.
  if (va >= 0x82000000u && va < 0xC0000000u) return true;
  return xheap::enabled() && xheap::inRange(va) && xheap::owns(va);
}

bool readerAnomalous(uint32_t reader, uint32_t owner, bool ownerKnown) {
  if (reader < kNullPageEnd) return true;
  if (!guestObjectPlausible(reader)) return true;
  return ownerKnown && !guestObjectPlausible(owner);
}

StallDetector::Event StallDetector::feed(uint64_t nowMs, uint32_t ticks, uint64_t callbacks) {
  if (!primed) {
    primed = true;
    lastTicks = ticks;
    lastTickChangeMs = nowMs;
    cbAtLastTickChange = callbacks;
    return kNone;
  }
  if (ticks != lastTicks) {
    lastTicks = ticks;
    lastTickChangeMs = nowMs;
    cbAtLastTickChange = callbacks;
    if (stalled) { stalled = false; return kEnd; }
    return kNone;
  }
  if (!stalled && nowMs - lastTickChangeMs >= thresholdMs && callbacks >= cbAtLastTickChange + 2) {
    stalled = true;
    stallStartMs = lastTickChangeMs;
    return kStart;
  }
  return kNone;
}

bool protectRange(uint8_t* hostBase, uint32_t bytes) {
  if (!hostBase || !bytes) return false;
  // 2026-10-14: a diagnostic protection, declared first, so arithmetic store admission (kernel/guest_protect.hpp: the
  // transport pair) refuses every store from here on.
  guest_protect::declare(hostBase, bytes, guest_protect::Scope::All);
  hostRangeReadableBeginRemoval();
  DWORD old = 0;
  const BOOL ok = VirtualProtect(hostBase, bytes, PAGE_NOACCESS, &old);
  hostRangeReadableEndRemoval();
  return ok != 0;
}

void ioDiagNoteOpen(uint32_t handle, const std::string& path) {
  if (!ioDelayMs() || !handle) return;
  static const std::string match = [] {
    const char* e = std::getenv("SR_DIAG_IO_DELAY_MATCH");
    return std::string(e && e[0] ? e : "sound.AST");
  }();
  if (path.find(match) == std::string::npos) return;
  {
    std::lock_guard<std::mutex> g(g_ioMu);
    g_ioHandles[handle] = true;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[IO_DELAY] diagnostic: handle %u (%s) reads delayed %u ms each", handle,
              path.c_str(), ioDelayMs());
}

void ioDiagNoteClose(uint32_t handle) {
  if (!ioDelayMs()) return;
  std::lock_guard<std::mutex> g(g_ioMu);
  g_ioHandles.erase(handle);
}

void ioDiagMaybeDelay(uint32_t handle) {
  const uint32_t ms = ioDelayMs();
  if (!ms) return;
  {
    std::lock_guard<std::mutex> g(g_ioMu);
    if (g_ioHandles.find(handle) == g_ioHandles.end()) return;
  }
  Sleep(ms);
}

}  // namespace cmdq
}  // namespace kernel
}  // namespace sr

// ── the overrides (all delegate unconditionally) ────────────────────────────────────────────
namespace C = sr::kernel::cmdq;

// RwacSystem_ProcessCmdQueue_sub_8264C540: the RWAC queue's only consumer, on the
// "RWAudioCore Dac" thread once per XAudio frame. Observe: who runs it, how often; starts
// the watcher. S3 (SR_DIAG_DAC_WAKE_DELAY_US): host delay before the drain.
extern "C" void __imp__sub_8264C540(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8264C540) {
  C::ensureWatcher();
  C::g_dacHostTid.store((uint32_t)GetCurrentThreadId(), std::memory_order_relaxed);
  C::g_drains.fetch_add(1, std::memory_order_relaxed);
  if (const uint32_t us = C::dacDelayUs()) C::delayUs(us);
  __imp__sub_8264C540(ctx, base);
}

// sub_82654BE0(player r3, request r4, flag r5): the stream player's next-block fetch; reads
// request+68 / request+64 (the reader) and dequeues from the reader.
extern "C" void __imp__sub_82654BE0(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82654BE0) {
  const uint32_t savedReq = C::t_curReq;
  if (C::obsOn()) C::onGetBlock(ctx.r4.u32, (uint32_t)ctx.lr);   // sets t_curReq
  __imp__sub_82654BE0(ctx, base);
  C::t_curReq = savedReq;
}

// EaStrm_DequeueReadyBlock_sub_825FC760(reader r3). Callers: sub_82654BE0 (checked above),
// FmvDecodeFrameSubmit_sub_8235C460, the SCHl feeder sub_826577F8.
extern "C" void __imp__sub_825FC760(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_825FC760) {
  if (C::obsOn()) {
    C::g_deqCalls.fetch_add(1, std::memory_order_relaxed);
    if (C::t_curReq == 0) C::checkReader(0, ctx.r3.u32, (uint32_t)ctx.lr, false);
  }
  __imp__sub_825FC760(ctx, base);
}
