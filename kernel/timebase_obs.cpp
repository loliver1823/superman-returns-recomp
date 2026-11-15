// iter-463 SR_TIMEBASE_OBS (default OFF): OBSERVE-ONLY witnesses of the guest clock rate,
// for the SR_TIMEBASE_50MHZ A/B. Both overrides call the guest body unchanged and only READ
// guest memory afterwards; they never write guest state.
//
//  [LOADSEQ_OBS]  TimedSequencerStateTick_sub_82304930(seq, dt_ms)  (called by
//                 TimedSequencerAdvanceRandomState_sub_8230F580 with dt = Stopwatch elapsed ms,
//                 an mftb clock). The load-screen item sequencer: state 5 shows an item for
//                 12000 ms, state 4 cross-fades, state 1 waits 4000 ms. Logged: every STATE
//                 CHANGE (first 400) with the wall ms spent in the old state and the guest dt
//                 summed over it, plus a rate line (sum dt / wall) every 2 s of wall while ticking.
//                 On a correct 50 MHz clock rate = 1.00 and state 5 lasts ~12 s of wall.
//  [SHTIME_OBS]   ShaderParam_WriteTimeSeconds_sub_8260DDC8 (semantic constant 0x29: float4
//                 {seconds since render-device init, 0, 0, 1}). Every 2 s of wall: the value the
//                 guest wrote, and its advance / wall advance since the last line (= the rate).
//
// Registry: docs/OVERRIDES.json classifies sub_82304930 as `observe`; sub_8260DDC8 and sub_824680D0 are
// `diagnostic` since the shader-time hold below.
//
// iter-467 native-a (save_gate_drift, the orchestrator's go 2026-10-11): THE SHADER TIME HELD WITH THE SIM.
// Under SR_DIAG_SIM_FREEZE (the freeze oracle of the flicker gate; never in a player's run) the semantic
// constant 0x29 - the only wall-clock input of a gameplay picture (docs/HIGH_FRAMERATE.md; the sea's surface
// moved through every sgb1 hold) - follows the sim-held guest clock: during a hold it stays at the hold's
// start, after it the clock continues with the held time removed (no jump). Mechanism: the guest body runs
// unchanged and computes .x itself, (QPC - qword 8299AEA0) / QPF as float; only the QPC it reads
// (QueryPerformanceCounter sub_824680D0, = mftb) is substituted for the duration of that one call, through a
// thread-local that nothing else sets. So .y / .z / .w, the dirty bits and the arithmetic are the guest's;
// a hold writes the identical float on every pass. SR_DIAG_SIM_FREEZE_SHTIME=0 lets the time run (the A/B).
// With SR_DIAG_SIM_FREEZE unset the thread-local is never set: both overrides are the guest bodies, bit for
// bit (difftest --shtime-hold-test checks both paths).
#include "guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"
#include "highfps.hpp"
#include "shtime_hold.hpp"
#include "sr_timebase.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

bool obsOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_TIMEBASE_OBS");
    return e && e[0] == '1';
  }();
  return on;
}

double wallMs() {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return duration<double, std::milli>(steady_clock::now() - t0).count();
}

uint32_t rdU32(const uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return _byteswap_ulong(v);
}
double rdF64(const uint8_t* base, uint32_t va) {
  uint64_t v;
  std::memcpy(&v, base + va, 8);
  v = _byteswap_uint64(v);
  double d;
  std::memcpy(&d, &v, 8);
  return d;
}
float rdF32(const uint8_t* base, uint32_t va) {
  uint32_t v = rdU32(base, va);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// One tracked sequencer (the load screen owns one at a time; a new object resets it).
struct SeqTrack {
  uint32_t obj = 0;
  uint32_t state = 0xFFFFFFFFu;
  double stateWall0 = 0;   // wall ms at entry to `state`
  double stateDt = 0;      // guest dt ms summed since entry to `state`
  double winWall0 = 0;     // rate window
  double winDt = 0;
  uint32_t winCalls = 0;
};
std::mutex g_seqMu;
SeqTrack g_seq;
std::atomic<uint32_t> g_seqLines{0};

// ---- the shader-time hold (see the header) ----
// The gate as a zero-arg helper (scripts/audit_forcing.py and the override manifest read it): the freeze
// diagnostic SR_DIAG_SIM_FREEZE (default OFF, never in a player's run), unless SR_DIAG_SIM_FREEZE_SHTIME=0.
bool shtimeHoldGate() {
  static const bool on = [] {
    const char* e = std::getenv("SR_DIAG_SIM_FREEZE");
    if (!(e && *e && e[0] != '0')) return false;
    const char* s = std::getenv("SR_DIAG_SIM_FREEZE_SHTIME");
    return !(s && s[0] == '0');
  }();
  return on;
}
bool shtimeHoldOn() {
  static const bool on = [] {
    if (!sr::kernel::highfps::simFreezeOn()) return false;
    const bool v = shtimeHoldGate();
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SIM_FREEZE] shader time 0x29 %s (diagnostic; SR_DIAG_SIM_FREEZE_SHTIME=%s)",
                v ? "HELD with the sim: still during a hold, the held time removed after it" : "runs on the wall clock",
                v ? "0 lets it run" : "0");
    return v;
  }();
  return on;
}
// The substitution for ONE QueryPerformanceCounter call inside ShaderParam_WriteTimeSeconds: kFixed = the
// hold's start (minus the earlier holds), kShift = the live clock minus every completed hold.
enum QpcMode : uint8_t { kQpcGuest = 0, kQpcFixed, kQpcShift };
thread_local QpcMode t_qpcMode = kQpcGuest;
thread_local uint64_t t_qpcValue = 0;
// Per-hold witness: the 0x29 writes inside a hold and the value they held.
std::mutex g_holdMu;
uint32_t g_holdSeenId = 0, g_holdWrites = 0;
float g_holdVal = 0.f;
bool g_holdDiffer = false;
std::atomic<uint32_t> g_holdLines{0};
std::atomic<uint64_t> g_substitutions{0};  // QPC reads the hold substituted (0 in every player's run)

}  // namespace

namespace sr::shtime {
void armFixedForTest(uint64_t qpc) noexcept {  // takes effect only where the gate is on (the -on test)
  t_qpcMode = kQpcFixed;
  t_qpcValue = qpc;
}
void disarmForTest() noexcept { t_qpcMode = kQpcGuest; }
uint64_t substitutions() noexcept { return g_substitutions.load(std::memory_order_relaxed); }
}  // namespace sr::shtime

// QueryPerformanceCounter (sub_824680D0: r11 = mftb, re-read when the low word is 0, *r3 = r11, r3 = 1). The
// guest body unless the shader-time hold armed this thread for the current ShaderParam_WriteTimeSeconds call.
extern "C" void __imp__sub_824680D0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824680D0(PPCContext& __restrict ctx, uint8_t* base) {
  if (!shtimeHoldGate() || t_qpcMode == kQpcGuest) {
    __imp__sub_824680D0(ctx, base);  // the guest body
    return;
  }
  g_substitutions.fetch_add(1, std::memory_order_relaxed);
  uint64_t v = t_qpcMode == kQpcFixed ? t_qpcValue : sr_guest_mftb() - t_qpcValue;
  if (t_qpcMode == kQpcShift && uint32_t(v) == 0) v = sr_guest_mftb() - t_qpcValue;  // the guest's re-read
  (void)base;
  PPC_STORE_U64(ctx.r3.u32, v);  // std r11, 0(r10)
  ctx.r3.u64 = 1;
}

extern "C" void __imp__sub_82304930(PPCContext& __restrict ctx, uint8_t* base);
void sub_82304930(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  __imp__sub_82304930(ctx, base);  // the guest body always runs, unchanged
  if (!obsOn()) return;
  const uint32_t st = rdU32(base, obj + 152);
  const double now = wallMs();
  std::lock_guard<std::mutex> lk(g_seqMu);
  if (g_seq.obj != obj) {
    g_seq = SeqTrack{};
    g_seq.obj = obj;
    g_seq.state = st;
    g_seq.stateWall0 = now;
    g_seq.winWall0 = now;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[LOADSEQ_OBS] new seq=%08X state=%u wall=%.0fms", obj, st, now);
    return;
  }
  g_seq.stateDt += dt;
  g_seq.winDt += dt;
  g_seq.winCalls++;
  if (st != g_seq.state) {
    if (g_seqLines.fetch_add(1, std::memory_order_relaxed) < 400) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[LOADSEQ_OBS] seq=%08X state %u->%u after wall=%.1fms guestDt=%.1fms item=%u",
                  obj, g_seq.state, st, now - g_seq.stateWall0, g_seq.stateDt, rdU32(base, obj + 268));
    }
    g_seq.state = st;
    g_seq.stateWall0 = now;
    g_seq.stateDt = 0;
  }
  if (now - g_seq.winWall0 >= 2000.0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[LOADSEQ_OBS] rate seq=%08X calls=%u guestDt=%.1fms wall=%.1fms rate=%.3f state=%u",
                obj, g_seq.winCalls, g_seq.winDt, now - g_seq.winWall0,
                g_seq.winDt / (now - g_seq.winWall0), st);
    g_seq.winWall0 = now;
    g_seq.winDt = 0;
    g_seq.winCalls = 0;
  }
}

extern "C" void __imp__sub_8260DDC8(PPCContext& __restrict ctx, uint8_t* base);
void sub_8260DDC8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t reg = ctx.r3.u32;
  const bool vs = ctx.r5.u32 != 0;
  // One guest-body call on every path; the substitution is armed around it only when the hold is on.
  const bool hold = shtimeHoldOn();
  sr::kernel::highfps::FreezeClock c{};
  if (hold) {
    c = sr::kernel::highfps::freezeClock();
    if (c.holdStartTicks) {
      t_qpcMode = kQpcFixed;
      t_qpcValue = c.holdStartTicks - c.heldTicks;
    } else {
      t_qpcMode = kQpcShift;  // heldTicks 0 (before the first hold): the guest's own clock, bit for bit
      t_qpcValue = c.heldTicks;
    }
  }
  __imp__sub_8260DDC8(ctx, base);  // the guest body computes .x itself (from the substituted QPC when armed)
  if (hold) {
    t_qpcMode = kQpcGuest;
    auto summary = [] {  // under g_holdMu: one line per hold that had 0x29 writes
      if (g_holdSeenId && g_holdWrites && g_holdLines.fetch_add(1, std::memory_order_relaxed) < 400)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[SHTIME_HOLD] hold #%u: %u writes of 0x29 held at t=%.6fs%s", g_holdSeenId,
                    g_holdWrites, g_holdVal, g_holdDiffer ? " - DIFFERING VALUES (must not happen)" : "");
      g_holdWrites = 0;
    };
    if (!c.holdStartTicks) {  // the first write after a hold closes its witness line
      std::lock_guard<std::mutex> lk(g_holdMu);
      if (g_holdWrites) summary();
    } else {  // the per-hold witness: every write of a hold must be the same float
      const uint32_t dev = rdU32(base, 0x8299ADF4u);
      const float val = dev ? rdF32(base, dev + 16u * (reg + (vs ? 112u : 368u))) : 0.f;
      std::lock_guard<std::mutex> lk(g_holdMu);
      if (g_holdSeenId != c.holdId) {
        summary();
        g_holdSeenId = c.holdId;
        g_holdWrites = 0;
        g_holdVal = val;
        g_holdDiffer = false;
      }
      if (g_holdWrites && std::memcmp(&val, &g_holdVal, 4) != 0) g_holdDiffer = true;
      g_holdWrites++;
    }
  }
  if (!obsOn()) return;
  static std::mutex mu;
  static double lastWall = -1, lastVal = 0;
  static uint32_t calls = 0;
  const double now = wallMs();
  std::lock_guard<std::mutex> lk(mu);
  ++calls;
  if (lastWall >= 0 && now - lastWall < 2000.0) return;
  const uint32_t dev = rdU32(base, 0x8299ADF4u);
  if (!dev) return;  // the guest just wrote through this pointer
  const float val = rdF32(base, dev + 16u * (reg + (vs ? 112u : 368u)));
  if (lastWall >= 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SHTIME_OBS] t=%.3fs dT=%.3fs dWall=%.3fs rate=%.3f calls=%u",
                val, val - lastVal, (now - lastWall) / 1000.0,
                (val - lastVal) / ((now - lastWall) / 1000.0), calls);
  } else {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SHTIME_OBS] first t=%.3fs reg=%u %s", val, reg, vs ? "VS" : "PS");
  }
  lastWall = now;
  lastVal = val;
  calls = 0;
}
