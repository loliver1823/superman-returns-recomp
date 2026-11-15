// display.registers: GATE (b) with (c) - see native_vblank.hpp and docs/NATIVE_ROADMAP.md "DISPLAY.REGISTERS BUILD NOTE".
// SR_NATIVE_VBLANK: default ON since the display-registers bake (native/dreg-bake, 2026-10-15), =0 the guest bodies;
// requires SR_NATIVE_GATE, the native flush, segmentless, SR_KPCR, today's scan-out answers and no page-protecting
// diagnostic (an explicit =1 without them is refused, the default stands down).
//
// Contract pins (IDA default.xex sha256 c8f243acd99de9a91f5ae4f409721c0e954e3d5eb96861419d3da07b8106db2b, read-only):
//   GpuPresentPacingUpdate_82111E38 (a leaf; its only reference is the callback address 82111FCC in 82111EC8):
//     E3C srwi r10,r3,16 / E40 clrlwi r7,r3,16 / E44-E48 dev = *(*(0x82000948)) / E4C count = +0x3B14 /
//     E50 last = +0x3B18 / E54 subf / E58-E5C gates = +0x3B20 + 1 / E60 add. remaining = last + vblanks - count /
//     E64 stw +0x3B20 / E68 bgt -> EC0 / E74 V = MMIO 0x7FC86530 / E78 MMIO 0x7FC86584 / E7C mulli V*100 /
//     E80 H = & 0xFFF / E84 twllei H,0 (dropped by the recompiler) / E88 divwu / E8C +1 / E90-E98 cap 100 /
//     E9C-EA0 line % > pct -> EBC r8 = 1 / EA4-EAC pc+4 = 0 / EB0-EB4 +0x3B18 = +0x3B14 / EC0 stw r8,+0x3B1C / EC4 blr.
//   The vblank ISR 820FDBA0 source 0 (820FDC30-820FDCB0) is kernel/native_display.cpp's (trySource0, its own pin).
//   Today's scan-out answers: kernel/stubs.cpp vdScanoutRegsTick (SR_VD_SCANOUT_REGS) through nextVCounter below.
#include "native_vblank.hpp"
#include "sr_oracle.hpp"
#include "native_display.hpp"
#include "native_flush.hpp"
#include "ab_toggle.hpp"
#include "guest_context.hpp"
#include "guest_protect.hpp"
#include "kpcr.hpp"
#include "guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

#if SR_ORACLE
extern "C" void __imp__sub_82111E38(PPCContext& __restrict ctx, uint8_t* base);
#endif

namespace sr::native::vblank {
namespace {

constexpr uint32_t kVdGlobalDeviceCell = 0x82000948u;  // 82111E38 lis / 82111E44: VdGlobalDevice's import cell
constexpr uint32_t kVCounterVA = 0x7FC86530u, kViewportVA = 0x7FC86584u;  // D1MODE_V_COUNTER / VIEWPORT_SIZE
constexpr uint32_t kDevCallback = 0x3B10u, kDevCount = 0x3B14u, kDevLast = 0x3B18u, kDevCountdown = 0x3B1Cu,
                   kDevGates = 0x3B20u, kDevPc = 0x2A14u;

// The forced state of the difftest (test::force): -1 = the env latch.
#if SR_ORACLE
std::atomic<int> g_forceState{-1}, g_forceToggled{0}, g_forceWindow{1};
#endif

// The CPU-2 lock and what it guards: the host scan-out counter (today's D1MODE_V_COUNTER answer) and the tick record.
// g_vcount is written only under the lock; it is atomic so the census line (finalLine, on the CP thread) reads it
// without taking the lock (native/dreg-hang).
std::mutex g_cpu2;
std::atomic<uint32_t> g_vcount{0};  // as vdScanoutRegsTick's s_vcount: 0 before the first tick (the zero page reads 0)
uint64_t g_tickNs = 0;
bool g_ticked = false, g_tickHighFps = false;

std::atomic<uint64_t> g_ticks{0}, g_nativeTicks{0}, g_guestTicks{0}, g_released{0}, g_callbacks{0}, g_traceBad{0},
    g_late{0}, g_maxGapNs{0};
std::atomic<uint64_t> g_calls{0}, g_host{0}, g_guestCalls{0}, g_wait{0}, g_now{0}, g_next{0}, g_early{0}, g_mismatch{0};
std::atomic<uint64_t> g_dispreg[3] = {};
// The CPU-2 lock's witness (finalLine's `| cpu2` tail): a tick's hold, a pacing call's wait.
std::atomic<uint64_t> g_tickHoldMaxNs{0}, g_tickSlow{0}, g_pacingWaitMaxNs{0}, g_pacingWaitSlow{0};
std::atomic<uint64_t> g_scratch1{0};  // the CP's SCRATCH_REG1 write-backs (pc+4's rearm), a generation: +1 at each end
std::atomic<uint32_t> g_scratch1Flight{0};  // write-backs between their Begin and End (the store in flight)
std::atomic<uint64_t> g_scratch1Target{0};  // the latest write-back's target: address << 32 | value (set at Begin)
#if SR_ORACLE
std::atomic<void (*)()> g_hookAfterBegin{nullptr}, g_hookBeforeEnd{nullptr};  // difftest only (test::setBracketHooks)
std::atomic<Outcome (*)(uint32_t, LineFn, void*)> g_testCore{nullptr};
#endif

uint64_t nowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool envSegmentless() {
  const char* e = std::getenv("SR_NATIVE_SEGMENTLESS");
  return !e || e[0] != '0';
}
// Today's answers are SR_VD_SCANOUT_REGS's default form: on, and not in an A/B (its OFF windows answer zeros).
bool envScanoutRegs() {
#if SR_ORACLE
  const char* e = std::getenv("SR_VD_SCANOUT_REGS");
  return (!e || e[0] != '0') && sr::abt::reg("SR_VD_SCANOUT_REGS") < 0;
#else
  return true;  // the host service has no register-answer A/B
#endif
}

State computeState() {
#if SR_ORACLE
  const Request req = requestOf(std::getenv("SR_NATIVE_VBLANK"));
  if (req == Request::Off) return State::Off;
#else
  constexpr Request req = Request::Explicit;
#endif
  const bool gate = sr::native::flush::presentGateOn(), flush = sr::native::flush::on(), seg = envSegmentless(),
             kpcr = sr::kernel::kpcrEnabled(), scan = envScanoutRegs(),
             protectFree = !sr::kernel::guest_protect::diagnosticRequested();
  const State s = decideState(req, gate, flush, seg, kpcr, scan, protectFree);
#if !SR_ORACLE
  if (s != State::On) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[NATIVE_DISPLAY_BAD] release requires native gate=%d flush=%d segmentless=%d KPCR=%d and no protecting diagnostic=%d",
      int(gate), int(flush), int(seg), int(kpcr), int(protectFree));
    sr::Log::shutdown();
    std::_Exit(3);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_VBLANK] release host pacing and source 0; hardware registers inaccessible");
  return State::On;
#else
  if (s == State::On) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_VBLANK] SR_NATIVE_VBLANK on (%s): the pacing 82111E38 and the vblank ISR's source 0 as host "
                "code, today's scan-out answers held by host code (no MMIO answer, no vsync-enable bit; "
                "display.registers)", req == Request::Explicit ? "=1" : "the baked default; =0 keeps the guest bodies");
    return State::On;
  }
  if (s == State::Refused) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NATIVE_VBLANK] REFUSED: SR_NATIVE_VBLANK=1 requires SR_NATIVE_GATE=1 (%d), the native flush (%d), "
                "segmentless (%d), SR_KPCR (%d), today's scan-out answers (SR_VD_SCANOUT_REGS on, not A/B-toggled: "
                "%d) and no page-protecting diagnostic (kernel/guest_protect.hpp kDiagnosticGates: %d); the emulated "
                "path runs (scan-out answers, the vsync-enable bit, the guest 82111E38 and ISR source 0)",
                int(gate), int(flush), int(seg), int(kpcr), int(scan), int(protectFree));
    return State::Refused;
  }
  // The default standing down (native/dreg-bake): an arm that turned a prerequisite off asked for today's path. WARN,
  // not Info (the orchestrator, 2026-10-15): a default build that emulates the display registers must be greppable. A
  // dev-build behaviour - the release configuration has no emulated answers to fall back to (RELEASE_COMPOSITION stage 4).
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[NATIVE_VBLANK] default stands down: SR_NATIVE_GATE (%d), the native flush (%d), segmentless (%d), "
              "SR_KPCR (%d), today's scan-out answers (%d), no page-protecting diagnostic (%d) - the emulated path "
              "runs, as SR_NATIVE_VBLANK=0 (=1 would refuse)",
              int(gate), int(flush), int(seg), int(kpcr), int(scan), int(protectFree));
  return State::Off;
#endif
}

#if SR_ORACLE
int abBit() {
  static const int bit = sr::abt::reg("SR_NATIVE_VBLANK");
  return bit;
}
#endif

void maxStore(std::atomic<uint64_t>& m, uint64_t v) {
  uint64_t cur = m.load(std::memory_order_relaxed);
  while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

uint32_t rd(uint32_t va) { return sr::kernel::readGuestU32BE(va); }

Line hostLine(void*) { return Line{g_vcount.load(std::memory_order_relaxed), kHeight}; }  // under the CPU-2 lock

// A pacing call's effects against a decision on the same pre-state (count, last and pc+4 read before the call; gen /
// flight = scratch1Writes() / scratch1InFlight() then). WAIT / NEXT leave pc+4 as it was - an extra clear is a premature
// release (Astra round 1, P2) - unless a CP write-back may explain it (the call runs inside the CP's own callback, so
// the CP cannot; the bracket keeps the check honest regardless) or pc+4 aliases a word the call stores.
bool effectsMatch(uint32_t dev, const Decision& want, uint32_t count, uint32_t last, uint32_t pc4, uint64_t gen,
                  uint32_t flight) {
  if (rd(dev + kDevCountdown) != want.countdown || rd(dev + kDevGates) != want.gates) return false;
  const uint32_t pc = rd(dev + kDevPc);
  const bool havePc = pc >= 0x10000u;
  if (!want.release) {
    const bool aliased = havePc && pc + 4u >= dev + kDevLast && pc + 4u <= dev + kDevGates;
    const uint32_t pc4Now = havePc ? rd(pc + 4u) : 0u;
    const bool pcSame = !havePc || aliased || pc4Now == pc4 || scratch1Explains(gen, flight, pc, pc4Now);
    return rd(dev + kDevLast) == last && pcSame;
  }
  return rd(dev + kDevLast) == count && (!havePc || rd(pc + 4u) == 0u);
}

}  // namespace

// The gate's env as a zero-arg helper (scripts/gen_override_manifest.py follows it from sub_82111E38): SR_NATIVE_VBLANK
// not =0 - default ON since the display-registers bake (native/dreg-bake, 2026-10-15) - or the difftest's forced
// state. A default that stands down (computeState) still passes this first test; pacingEntry then runs the guest body.
bool vblankGateRequested() {
#if SR_ORACLE
  static const bool env = [] {
    const char* e = std::getenv("SR_NATIVE_VBLANK");  // `e`: scripts/audit_forcing.py reads `!e ||` as default ON
    return !e || e[0] != '0';
  }();
  return env || g_forceState.load(std::memory_order_relaxed) >= 0;
#else
  return true;
#endif
}

State state() {
#if SR_ORACLE
  const int forced = g_forceState.load(std::memory_order_relaxed);
  if (forced >= 0) return State(forced);
#endif
  static const State latched = computeState();
  return latched;
}
bool on() { return state() == State::On; }
bool toggled() {
#if SR_ORACLE
  if (!on()) return false;
  if (g_forceState.load(std::memory_order_relaxed) >= 0) return g_forceToggled.load(std::memory_order_relaxed) != 0;
  return abBit() >= 0;
#else
  return false;
#endif
}
bool active() {
#if SR_ORACLE
  if (!on()) return false;
  if (!toggled()) return true;
  if (g_forceState.load(std::memory_order_relaxed) >= 0) return g_forceWindow.load(std::memory_order_relaxed) != 0;
  return sr::abt::on(abBit());
#else
  return on();
#endif
}
bool nativeRegistration() { return on() && !toggled(); }
#if SR_ORACLE
bool scanoutAnswers() { return !on() || toggled(); }
#else
bool scanoutAnswers() { return false; }
#endif
bool d3dIsrRegistration(uint32_t callbackVA, uint32_t contextVA) {
  return callbackVA == sr::native::display::kD3DIsr && contextVA >= 0x10000u;
}
TickPlan planTick(bool registrationNative, uint32_t callbackVA, uint32_t contextVA) {
  TickPlan p;
  const bool d3d = d3dIsrRegistration(callbackVA, contextVA);
  p.native = registrationNative || (d3d && active());
  p.scoped = on();
  p.checkDev = d3d ? contextVA : 0u;
  return p;
}

// The bracket's order (each seq_cst): Begin = the target, flight + 1, then generation + 1; [the store]; End =
// generation + 1, then flight - 1. A check reads generation then flight before its first pc+4 read, and pc+4 then both
// again at its end: a store it saw only at the end either moved the generation after the start read, or was in flight
// at the start (its Begin seen by the start's generation read implies its flight seen by the later flight read,
// unless its End was too - and then the store precedes the start's pc+4 read). The target precedes both counts, so a
// check that sees the bracket sees its target (or a later write-back's: the latest store decides pc+4).
void noteScratch1Begin(uint32_t addr, uint32_t value) {
  g_scratch1Target.store(uint64_t(addr) << 32 | value, std::memory_order_seq_cst);
  g_scratch1Flight.fetch_add(1, std::memory_order_seq_cst);
  g_scratch1.fetch_add(1, std::memory_order_seq_cst);
#if SR_ORACLE
  if (auto h = g_hookAfterBegin.load(std::memory_order_relaxed)) h();
#endif
}
void noteScratch1End() {
#if SR_ORACLE
  if (auto h = g_hookBeforeEnd.load(std::memory_order_relaxed)) h();
#endif
  g_scratch1.fetch_add(1, std::memory_order_seq_cst);
  g_scratch1Flight.fetch_sub(1, std::memory_order_seq_cst);
}
uint64_t scratch1Writes() { return g_scratch1.load(std::memory_order_seq_cst); }
uint32_t scratch1InFlight() { return g_scratch1Flight.load(std::memory_order_seq_cst); }
bool scratch1Explains(uint64_t gen, uint32_t flight, uint32_t pcVA, uint32_t observed) {
  std::atomic_thread_fence(std::memory_order_seq_cst);  // after the caller's last pc+4 read
  const uint64_t genNow = g_scratch1.load(std::memory_order_seq_cst);
  const uint32_t flightNow = g_scratch1Flight.load(std::memory_order_seq_cst);
  if (!(genNow != gen || flight != 0u || flightNow != 0u)) return false;  // no write-back in the window
  const uint64_t t = g_scratch1Target.load(std::memory_order_seq_cst);
  // Its address (SCRATCH_ADDR + 4, a GPU physical with endian bits; pc a guest VA: compared as the 29-bit window
  // offset, word-aligned) and its value: a write-back of 1 explains pc+4 reading 1, never 0.
  return ((uint32_t(t >> 32) ^ (pcVA + 4u)) & 0x1FFFFFFCu) == 0u && uint32_t(t) == observed;  // its address and value
}
Request requestOf(const char* env) {
  if (env && env[0] == '0') return Request::Off;
  return env && env[0] == '1' ? Request::Explicit : Request::Default;
}
State decideState(Request request, bool gate, bool flush, bool segmentless, bool kpcr, bool scanoutRegs,
                  bool protectFree) {
  if (request == Request::Off) return State::Off;
  if (gate && flush && segmentless && kpcr && scanoutRegs && protectFree) return State::On;
  return request == Request::Explicit ? State::Refused : State::Off;  // the default stands down
}

uint32_t nextVCounter(uint32_t v, bool highFps) {
  return highFps ? 0u : v + kHeight;  // HIGHFPS: scanline 0; the console path: + the display height, wrapping at 2^32
}

uint32_t linePercent(uint32_t v, uint32_t height) {
  const uint32_t product = uint32_t(uint64_t(v) * 100u);  // 82111E7C mulli; 82111E88 divwu reads its low word
  uint32_t q = height ? product / height : 0u;            // 82111E84 twllei: the console traps on 0, the generated code guards
  q += 1u;                                                 // 82111E8C
  return q < 100u ? q : 100u;                              // 82111E90-82111E98
}

Outcome pacingCore(uint32_t arg, LineFn line, void* user) {
  const uint32_t cell = PPC_LOAD_U32(kVdGlobalDeviceCell);       // 82111E38 / 82111E44
  const uint32_t dev = PPC_LOAD_U32(cell);                       // 82111E48
  const uint32_t vblanks = arg >> 16, pct = arg & 0xFFFFu;       // 82111E3C / 82111E40
  const uint32_t count = PPC_LOAD_U32(dev + kDevCount);          // 82111E4C
  const uint32_t last = PPC_LOAD_U32(dev + kDevLast);            // 82111E50
  const uint32_t gates = PPC_LOAD_U32(dev + kDevGates) + 1u;     // 82111E58 / 82111E5C
  const uint32_t remaining = (vblanks - count) + last;           // 82111E54 / 82111E60
  PPC_STORE_U32(dev + kDevGates, gates);                         // 82111E64
  if (int32_t(remaining) > 0) {                                  // 82111E68
    PPC_STORE_U32(dev + kDevCountdown, remaining);               // 82111EC0
    return Outcome::Wait;
  }
  const Line l = line(user);                                     // 82111E6C-82111E80: today's answers, held by host code
  if (linePercent(l.v, l.height) > pct) {                        // 82111E7C-82111EA0
    PPC_STORE_U32(dev + kDevCountdown, 1u);                      // 82111EBC / 82111EC0
    return Outcome::Next;
  }
  const uint32_t pc = PPC_LOAD_U32(dev + kDevPc);                // 82111EA4
  PPC_STORE_U32(pc + 4u, 0u);                                    // 82111EA8 / 82111EAC: the gate opens
  PPC_STORE_U32(dev + kDevLast, PPC_LOAD_U32(dev + kDevCount));  // 82111EB0 / 82111EB4 (reloaded after that store)
  PPC_STORE_U32(dev + kDevCountdown, remaining);                 // 82111EC0
  return Outcome::Now;
}

Decision decide(uint32_t arg, uint32_t count, uint32_t last, uint32_t gates, Line line) {
  Decision d;
  const uint32_t pct = arg & 0xFFFFu, remaining = ((arg >> 16) - count) + last;
  d.gates = gates + 1u;
  if (int32_t(remaining) > 0) {
    d.outcome = Outcome::Wait;
    d.countdown = remaining;
  } else if (linePercent(line.v, line.height) > pct) {
    d.outcome = Outcome::Next;
    d.countdown = 1u;
  } else {
    d.outcome = Outcome::Now;
    d.countdown = remaining;
    d.release = true;
  }
  return d;
}

void pacingEntry(PPCContext& ctx, uint8_t* base) {
#if SR_ORACLE
  if (!on()) {  // refused (or a forced Off): the emulated path, unchanged
    __imp__sub_82111E38(ctx, base);
    return;
  }
#else
  (void)base;
  (void)state();  // validate release prerequisites; refusal exits, never falls back
#endif
  const uint64_t waitFrom = nowNs();
  std::lock_guard<std::mutex> lock(g_cpu2);
  const uint64_t waited = nowNs() - waitFrom;  // the CP's wait for the vblank tick (the witness)
  maxStore(g_pacingWaitMaxNs, waited);
  if (waited > kCpu2SlowNs) g_pacingWaitSlow.fetch_add(1, std::memory_order_relaxed);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t arg = ctx.r3.u32;
  // The shadow's pre-state (fault-free reads; the device the guest body loads through the same cell).
  const uint32_t dev = rd(rd(kVdGlobalDeviceCell));
  const uint32_t count = rd(dev + kDevCount), last = rd(dev + kDevLast), gates = rd(dev + kDevGates);
  const uint32_t pc = rd(dev + kDevPc);
  const uint64_t gen = g_scratch1.load(std::memory_order_seq_cst);
  const uint32_t flight = g_scratch1Flight.load(std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const uint32_t pc4 = pc >= 0x10000u ? rd(pc + 4u) : 0u;
  const Line host = hostLine(nullptr);
  // Today's registers where they are live (a toggled roll writes them every tick); else the host's own answer.
#if SR_ORACLE
  const Line today = scanoutAnswers() ? Line{rd(kVCounterVA), rd(kViewportVA) & 0xFFFu} : host;
#else
  const Line today = host;
#endif
  bool same;
#if SR_ORACLE
  if (!active()) {
    // The A/B's OFF window: the guest body reads the registers; the shadow is the host form's prediction.
    g_guestCalls.fetch_add(1, std::memory_order_relaxed);
    const Decision want = decide(arg, count, last, gates, host);
    __imp__sub_82111E38(ctx, base);
    same = effectsMatch(dev, want, count, last, pc4, gen, flight);
  } else
#endif
  {
    // The host form; the shadow is the decision today's register answers give on the same pre-state.
    const Decision want = decide(arg, count, last, gates, today);
#if SR_ORACLE
    const auto core = g_testCore.load(std::memory_order_relaxed);
    const Outcome outcome = (core ? core : pacingCore)(arg, hostLine, nullptr);
#else
    const Outcome outcome = pacingCore(arg, hostLine, nullptr);
#endif
    g_host.fetch_add(1, std::memory_order_relaxed);
    switch (outcome) {
      case Outcome::Wait: g_wait.fetch_add(1, std::memory_order_relaxed); break;
      case Outcome::Next: g_next.fetch_add(1, std::memory_order_relaxed); break;
      case Outcome::Now: g_now.fetch_add(1, std::memory_order_relaxed); break;
    }
    if (outcome == Outcome::Now && want.outcome == Outcome::Next) g_early.fetch_add(1, std::memory_order_relaxed);
    same = outcome == want.outcome && effectsMatch(dev, want, count, last, pc4, gen, flight);
  }
  if (!same && g_mismatch.fetch_add(1, std::memory_order_relaxed) < 8u)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NATIVE_VBLANK] pacing MISMATCH arm=%s arg=%08X count=%u last=%u gates=%u V host=%u today=%u H today=%u "
                "-> countdown %d last %u gates %u", active() ? "host" : "guest", arg, count, last, gates, host.v, today.v,
                today.height, int32_t(rd(dev + kDevCountdown)), rd(dev + kDevLast), rd(dev + kDevGates));
}

TickScope::TickScope(uint32_t dev, bool highFps, bool native) : dev_(dev) {
  g_cpu2.lock();
  const uint64_t now = nowNs();
  lockedNs_ = now;
  if (g_ticked && !g_tickHighFps && !highFps && now > g_tickNs) {
    const uint64_t gap = now - g_tickNs;
    if (gap > kPeriodNs + kPeriodNs / 2u) g_late.fetch_add(1, std::memory_order_relaxed);
    maxStore(g_maxGapNs, gap);
  }
  g_tickNs = now;
  g_ticked = true;
  g_tickHighFps = highFps;
  g_vcount.store(nextVCounter(g_vcount.load(std::memory_order_relaxed), highFps), std::memory_order_relaxed);  // today's answer for this tick, held by host code
  g_ticks.fetch_add(1, std::memory_order_relaxed);
  (native ? g_nativeTicks : g_guestTicks).fetch_add(1, std::memory_order_relaxed);
  if (dev_ < 0x10000u) return;
  pre_[0] = rd(dev_ + kDevCallback);
  pre_[1] = rd(dev_ + kDevCount);
  pre_[2] = rd(dev_ + kDevLast);
  pre_[3] = rd(dev_ + kDevCountdown);
  pre_[4] = rd(dev_ + kDevGates);
  const uint32_t pc = rd(dev_ + kDevPc);
  havePc_ = pc >= 0x10000u;
  gen_ = g_scratch1.load(std::memory_order_seq_cst);  // generation, then flight, then pc+4 (noteScratch1Begin's order)
  flight_ = g_scratch1Flight.load(std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  pre_[5] = havePc_ ? rd(pc + 4u) : 0u;
}

TickScope::~TickScope() {
  if (dev_ >= 0x10000u) {
    const uint32_t count = rd(dev_ + kDevCount), last = rd(dev_ + kDevLast), countdown = rd(dev_ + kDevCountdown),
                   gates = rd(dev_ + kDevGates);
    const uint32_t pc = rd(dev_ + kDevPc);
    const uint32_t pc4 = pc >= 0x10000u ? rd(pc + 4u) : 0u;
    // The CP's SCRATCH_REG1 write-back (the GATE's rearm, outside this lock) since the tick began, or in flight at
    // either end: read after pc+4, so a rearmed pc+4 seen above is explained (the bracket, noteScratch1Begin / End).
    const bool rearmed = havePc_ && scratch1Explains(gen_, flight_, pc, pc4);
    const bool release = pre_[3] == 1u;
    bool bad = false;
    if (pre_[0]) {
      // A swap callback (null in this title) may change any of the words: counted, not checked.
      g_callbacks.fetch_add(1, std::memory_order_relaxed);
    } else {
      bad |= count != pre_[1] + 1u;
      if (int32_t(pre_[3]) > 0) {
        bad |= countdown != pre_[3] - 1u;
        if (release) {
          // pc+4 cleared - or already rearmed: the CP passes its hold only after the clear (Astra round 1, P2)
          bad |= last != count || (havePc_ && pc4 != 0u && !rearmed);
          g_released.fetch_add(1, std::memory_order_relaxed);
        } else {
          bad |= last != pre_[2];
        }
      } else {
        bad |= countdown != pre_[3] || last != pre_[2];
      }
      // No release this tick: pc+4 as it was - no early clear (nor any other write) unless the CP rearmed it.
      if (!release) bad |= havePc_ && pc4 != pre_[5] && !rearmed;
      bad |= gates != pre_[4];
    }
    // Where the registers are live (a toggled roll: vdScanoutRegsTick wrote them inside this scope), they are the
    // host counter's answer.
    const uint32_t vcount = g_vcount.load(std::memory_order_relaxed);
#if SR_ORACLE
    const auto diagnosticVcount = [&] { return rd(kVCounterVA); };
    const bool liveBad = scanoutAnswers() && (rd(kVCounterVA) != vcount || rd(kViewportVA) != kViewportAnswer);
#else
    const auto diagnosticVcount = [vcount] { return vcount; };
    constexpr bool liveBad = false;
#endif
    bad |= liveBad;
    if (bad && g_traceBad.fetch_add(1, std::memory_order_relaxed) < 8u)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_VBLANK] trace BAD dev=%08X count %u->%u countdown %d->%d last %u->%u gates %u->%u pc4 %u->%u%s "
                  "vcount host %u mmio %u%s", dev_, pre_[1], count, int32_t(pre_[3]), int32_t(countdown), pre_[2], last,
                  pre_[4], gates, pre_[5], pc4, rearmed ? " (CP rearm)" : "", vcount, diagnosticVcount(),
                  liveBad ? " (live registers differ)" : "");
  }
  // The hold witness: this tick's time under the CPU-2 lock (the CP's pacing call and nothing else waits for it).
  const uint64_t held = nowNs() - lockedNs_;
  maxStore(g_tickHoldMaxNs, held);
  if (held > kCpu2SlowNs) g_tickSlow.fetch_add(1, std::memory_order_relaxed);
  g_cpu2.unlock();
}

void countDispreg(Dispreg source, uint64_t n) {
  g_dispreg[size_t(source)].fetch_add(n, std::memory_order_relaxed);
}

void finalLine(const char* why) {
  const State s = state();
  if (s == State::Off) return;
  auto v = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
  // Never the CPU-2 lock here: the CP thread prints this line from its census (native/dreg-hang).
  const uint32_t vcount = g_vcount.load(std::memory_order_relaxed);  // the census: lock-free
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_VBLANK] %s state=%s toggled=%d ticks=%llu native=%llu guest=%llu released=%llu callbacks=%llu "
              "traceBad=%llu late=%llu maxGapUs=%llu vcount=%u | pacing calls=%llu host=%llu guest=%llu wait=%llu "
              "now=%llu next=%llu early=%llu mismatch=%llu | dispreg scanout=%llu sysbuf=%llu vsyncBit=%llu "
              "| cpu2 tickMaxUs=%llu tickSlow=%llu waitMaxUs=%llu waitSlow=%llu",
              why, s == State::On ? "on" : "refused", int(toggled()), v(g_ticks), v(g_nativeTicks), v(g_guestTicks),
              v(g_released), v(g_callbacks), v(g_traceBad), v(g_late), v(g_maxGapNs) / 1000ull, vcount, v(g_calls),
              v(g_host), v(g_guestCalls), v(g_wait), v(g_now), v(g_next), v(g_early), v(g_mismatch), v(g_dispreg[0]),
              v(g_dispreg[1]), v(g_dispreg[2]), v(g_tickHoldMaxNs) / 1000ull, v(g_tickSlow),
              v(g_pacingWaitMaxNs) / 1000ull, v(g_pacingWaitSlow));
}

#if SR_ORACLE
namespace test {
void force(int s, int toggledFlag, int window) {
  g_forceToggled.store(toggledFlag, std::memory_order_relaxed);
  g_forceWindow.store(window, std::memory_order_relaxed);
  g_forceState.store(s, std::memory_order_relaxed);
}
void reset() {
  std::lock_guard<std::mutex> lock(g_cpu2);
  g_vcount.store(0, std::memory_order_relaxed);
  g_tickNs = 0;
  g_ticked = g_tickHighFps = false;
  for (auto* a : {&g_ticks, &g_nativeTicks, &g_guestTicks, &g_released, &g_callbacks, &g_traceBad, &g_late, &g_maxGapNs,
                  &g_calls, &g_host, &g_guestCalls, &g_wait, &g_now, &g_next, &g_early, &g_mismatch, &g_dispreg[0],
                  &g_dispreg[1], &g_dispreg[2], &g_tickHoldMaxNs, &g_tickSlow, &g_pacingWaitMaxNs, &g_pacingWaitSlow})
    a->store(0, std::memory_order_relaxed);
}
void setVCount(uint32_t v) {
  std::lock_guard<std::mutex> lock(g_cpu2);
  g_vcount.store(v, std::memory_order_relaxed);
}
uint32_t vcount() { return g_vcount.load(std::memory_order_relaxed); }
Counters counters() {
  auto v = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  return Counters{v(g_ticks), v(g_nativeTicks), v(g_guestTicks), v(g_released), v(g_callbacks), v(g_traceBad),
                  v(g_late), v(g_calls), v(g_host), v(g_guestCalls), v(g_wait), v(g_now), v(g_next), v(g_early),
                  v(g_mismatch), v(g_dispreg[0]), v(g_dispreg[1]), v(g_dispreg[2]), v(g_tickHoldMaxNs), v(g_tickSlow),
                  v(g_pacingWaitMaxNs), v(g_pacingWaitSlow)};
}
void setCore(Outcome (*core)(uint32_t, LineFn, void*)) { g_testCore.store(core, std::memory_order_relaxed); }
void setBracketHooks(void (*afterBegin)(), void (*beforeEnd)()) {
  g_hookAfterBegin.store(afterBegin, std::memory_order_relaxed);
  g_hookBeforeEnd.store(beforeEnd, std::memory_order_relaxed);
}
}  // namespace test
#endif

}  // namespace sr::native::vblank

// GpuPresentPacingUpdate_82111E38 (display.registers (b)). Gate OFF (SR_NATIVE_VBLANK=0): the guest body with the same
// context, nothing else. Requested (unset - the baked default - or =1): pacingEntry - refused or the default standing
// down -> the guest body; ON -> the host form under the CPU-2 lock in
// an active window, the guest body in the A/B's OFF window; every ON call shadow-checked.
PPC_FUNC(sub_82111E38) {
#if SR_ORACLE
  if (!sr::native::vblank::vblankGateRequested()) {
    __imp__sub_82111E38(ctx, base);
    return;
  }
#endif
  sr::native::vblank::pacingEntry(ctx, base);
}
