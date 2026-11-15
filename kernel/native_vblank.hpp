#pragma once
// display.registers (docs/NATIVE_ROADMAP.md, p2.transport plan item 2 "DISPLAY.REGISTERS BUILD NOTE"; ledger
// display.registers; the orchestrator's DECISIONS (2) of 2026-10-10 and its pacing decision of 2026-10-14): GATE (b)
// WITH (c), behind SR_NATIVE_VBLANK - default ON since the display-registers bake (native/dreg-bake, 2026-10-15;
// NATIVE_ROADMAP "DISPLAY-REGISTERS BAKE PLAN"); =0 keeps the guest 82111E38, the guest ISR's source 0, the scan-out
// answers and the vsync-enable bit (the A/B's OFF arm and the bake's =0 arms).
//   (b) the pacing decision GpuPresentPacingUpdate_82111E38 as host code: its exact arithmetic on the guest's words
//       dev+0x3B14 / 0x3B18 / 0x3B1C / 0x3B20, with line % computed from TODAY's scan-out answers held by host code
//       instead of read from the emulated D1MODE_V_COUNTER / VIEWPORT_SIZE registers: a host counter advanced on every
//       vblank tick by the same rule vdScanoutRegsTick (kernel/stubs.cpp) writes the register with (nextVCounter:
//       V += 720, 0 under HIGHFPS, the 32-bit wrap included) and the same 1280 x 720 viewport - so the gate outcome is
//       today's by construction, wrap points included. A strong sub_82111E38 (kernel/native_vblank.cpp): every route to
//       the callback - the GATE record's CALLBACK apply, the PM4 form (CPPM4, a declined GATE leaf) - runs the host form.
//   (c) the host vblank service performs the vblank ISR 820FDBA0's source-0 effects (the counter, the countdown and
//       its release, the swap callback) with sr::native::display::trySource0 instead of the guest ISR, so the
//       vsync-enable MMIO bit 0x7FC86544 is not armed at VdSetGraphicsInterruptCallback.
// Under the gate the two emulated scan-out answers per vblank are not written to the MMIO page.
// Requires SR_NATIVE_GATE, the native flush, segmentless, SR_KPCR (all default ON), today's scan-out answers
// (SR_VD_SCANOUT_REGS on, not A/B-toggled) and no page-protecting diagnostic. Unmet: an explicit SR_NATIVE_VBLANK=1 is
// REFUSED (one Error line, state=refused in every [NATIVE_VBLANK] line); the default (unset) STANDS DOWN - one WARN line
// naming what is missing (greppable: a default build emulating the display registers), then state Off, as =0 (no
// [NATIVE_VBLANK] census line) - so an arm that turns a prerequisite off (the transport's =0 arms: SR_NATIVE_GATE=0)
// runs today's path without an error. Either way the emulated path runs. The stand-down is a dev-build behaviour: the
// release configuration has no emulated answers to fall back to and must refuse instead (docs/RELEASE_COMPOSITION.md,
// stage 4). In SR_ORACLE=0 the service is mandatory: its selector and A/B are absent,
// unmet native prerequisites stop with NATIVE_DISPLAY_BAD, and no guest pacing or
// register-answer fallback is compiled. SR_AB_TOGGLE=SR_NATIVE_VBLANK: the dev in-roll A/B (OFF windows = the guest 82111E38 and the guest ISR source
// 0, their inputs kept live in both arms); every pacing call in either arm is shadow-checked against the other form's
// decision (mismatch=, must read 0).
// The CPU-2 lock: on the console both functions run as DPCs on CPU 2 and never interleave; under the gate the host
// forms (and a toggled roll's guest forms) run under one host mutex. Gate OFF: nothing here runs but a latched test.
#include "sr_oracle.hpp"
#include <cstdint>

struct PPCContext;

namespace sr::native::vblank {

enum class State : uint8_t { Off = 0, On = 1, Refused = 2 };
// The env's three forms: =0 Off (the guest bodies); unset (or any value but 0 / 1) the bake's Default; =1 Explicit.
enum class Request : uint8_t { Off = 0, Default = 1, Explicit = 2 };
Request requestOf(const char* env);  // pure: the form of SR_NATIVE_VBLANK's value (nullptr = unset)
bool vblankGateRequested();   // SR_NATIVE_VBLANK not =0 (or the difftest's forced state): sub_82111E38's first test
State state();                // latched at the first query
bool on();                    // state() == On
bool toggled();               // on() and SR_AB_TOGGLE lists SR_NATIVE_VBLANK
bool active();                // on() and (untoggled, or the toggle's ON window) - read at the point of use
bool nativeRegistration();    // VdSetGraphicsInterruptCallback: on() and untoggled (the vsync bit is not armed)
bool scanoutAnswers();        // the vblank tick writes the emulated scan-out answers: !on() || toggled()
// The leased registration is the retail D3D vblank ISR 820FDBA0 with a device context: the only registration whose
// source 0 the host form may perform (sr::native::display::trySource0's identity) and whose device the per-vblank
// check may read.
bool d3dIsrRegistration(uint32_t callbackVA, uint32_t contextVA);
// One vblank tick's plan for the leased registration (stubs.cpp vdVblankDeliver; Astra round 1, P1):
//   native   - the host source 0 instead of the registered callback: a native registration (SR_NATIVE_DISPLAY_SOURCE0
//              or this gate untoggled; VdSetGraphicsInterruptCallback admits only the D3D ISR there) every tick, or
//              the D3D ISR's registration in the toggle's ON windows. Any other registration runs its own callback.
//   scoped   - a TickScope (the CPU-2 lock, the host counter's advance, the tick record): the gate on, whatever the
//              registration, so the host counter advances on exactly the ticks today's path writes the register.
//   checkDev - the device the per-vblank check reads: the D3D ISR's context, else 0 (no check, no device read).
struct TickPlan {
  bool native = false;
  bool scoped = false;
  uint32_t checkDev = 0;
};
TickPlan planTick(bool registrationNative, uint32_t callbackVA, uint32_t contextVA);
// The latch's decision (pure): Off for =0; On with every requirement; else Refused for an explicit =1 and Off (the
// default standing down) when unset. protectFree: no page-protecting diagnostic requested
// (kernel/guest_protect.hpp diagnosticRequested) - the host source 0 admits guest ranges by arithmetic
// (native/dreg-hang), so under such a diagnostic every tick would decline, which is fatal for a native registration;
// the gate refuses (or stands down) up front instead.
State decideState(Request request, bool gate, bool flush, bool segmentless, bool kpcr, bool scanoutRegs,
                  bool protectFree);

// TODAY's scan-out answers (SR_VD_SCANOUT_REGS, iter-467; Xenia's ReadRegister 0x1961 constant): one vblank tick's
// D1MODE_V_COUNTER from the previous one - V + 720 on the console path, the 32-bit wrap included, 0 under HIGHFPS - and
// the constant D1MODE_VIEWPORT_SIZE. vdScanoutRegsTick writes the register with it; under the gate the host counter
// advances with it. One function, so the two cannot diverge.
constexpr uint32_t kViewportAnswer = 0x050002D0u;  // 1280 x 720
constexpr uint32_t kHeight = kViewportAnswer & 0xFFFu;
uint32_t nextVCounter(uint32_t v, bool highFps);
// 82111E7C-82111E98 in the generated form: min(100, uint32(V * 100) / height + 1), the 32-bit product of mulli / divwu;
// height 0 -> quotient 0 (the recompiler dropped the twllei at 82111E84 and guards the divide; the console traps).
uint32_t linePercent(uint32_t v, uint32_t height);

// 82111E38's body with its two MMIO loads (82111E74 / 82111E78) replaced by `line`; every other load and store as the
// asm, through the generated code's own PPC_LOAD_U32 / PPC_STORE_U32. The caller holds the CPU-2 lock.
struct Line {
  uint32_t v = 0;
  uint32_t height = 0;
};
enum class Outcome : uint8_t { Wait = 0, Now = 1, Next = 2 };  // remaining > 0 / release now / the next vblank
using LineFn = Line (*)(void* user);
Outcome pacingCore(uint32_t arg, LineFn line, void* user);
// The same decision as a pure function of its inputs (no guest access): the shadow check's prediction.
struct Decision {
  Outcome outcome = Outcome::Wait;
  uint32_t countdown = 0, gates = 0;
  bool release = false;
};
Decision decide(uint32_t arg, uint32_t count, uint32_t last, uint32_t gates, Line line);

// The strong sub_82111E38's ON path: the CPU-2 lock; the host form in an active window, else the guest body; either
// way the call is shadow-checked against the other form's decision on the same pre-state (its stores, and pc+4:
// cleared on a release, unchanged otherwise).
void pacingEntry(PPCContext& ctx, uint8_t* base);

// The CP's SCRATCH_REG1 write-back (gfx/record_engine.cpp RecordEngine::writeReg, register 0x579; the executor's
// deferred perform gfx/pm4_engine.cpp Pm4Engine::execPerformStores): pc+4's only writer
// besides the two releases - the GATE's rearm (pc+4 = 1, presentGateApply / the PM4 form's SET 0x579), which the CP
// performs outside the CPU-2 lock once a release lets it past its hold. BRACKETED (native/dreg-hang): Begin before the
// store is performed or queued, End after it (the executor brackets a deferred perform the same way); each bumps the
// generation, and between them the write-back is in flight. A check excuses a pc+4 change when the generation moved
// during it or a write-back was in flight at either end. (Astra round 1's single count before the store left a hole: a
// tick that began between that count and the store read the new generation with the old pc+4 and flagged the CP's own
// rearm - fix1's traceBad=3 once the ticks ran at 1000 Hz; --native-vblank-test F, the three interleavings.)
// Begin records the write-back's own target - its address (SCRATCH_ADDR + 4) and value - and only a change of pc+4 TO
// that value at that address is explained (Astra round 2, P2: a write-back of 1 in flight cannot explain a clear to 0).
void noteScratch1Begin(uint32_t addr, uint32_t value);
void noteScratch1End();
uint64_t scratch1Writes();
uint32_t scratch1InFlight();
// For a check that read `gen` and `flight` (scratch1Writes / scratch1InFlight, in that order) before its first read of
// pc+4 and has since read `observed` there: true when a CP write-back moved the generation or was in flight at either
// end AND the latest write-back targets pc+4 (guest physical, the 29-bit window offset) with exactly `observed`.
bool scratch1Explains(uint64_t gen, uint32_t flight, uint32_t pcVA, uint32_t observed);

// The VD_ISR thread's tick under the gate (stubs.cpp vdVblankDeliver), from before the scan-out write through the
// source-0 delivery: the CPU-2 lock, the host counter's advance, the tick time, and the per-vblank check of the ISR's
// effects (either form): counter + 1; countdown - 1 iff positive; at 1 -> 0 pc+4 == 0 and +0x3B18 == +0x3B14, else
// +0x3B18 unchanged and pc+4 unchanged (no clear but the release's); +0x3B20 unchanged; a swap callback counted;
// where the registers are live (a toggled roll) they equal the host counter and the viewport answer. pc+4 is the CP's
// too: a SCRATCH_REG1 write-back during the tick (the rearm that follows a release) excuses its value.
class TickScope {
 public:
  TickScope(uint32_t dev, bool highFps, bool native);
  ~TickScope();
  TickScope(const TickScope&) = delete;
  TickScope& operator=(const TickScope&) = delete;

 private:
  uint32_t dev_ = 0;
  uint32_t pre_[6] = {};
  uint64_t gen_ = 0;  // scratch1Writes() at the start of the tick
  uint32_t flight_ = 0;  // scratch1InFlight() at the start of the tick (read after gen_)
  uint64_t lockedNs_ = 0;  // when the CPU-2 lock was taken (the hold witness)
  bool havePc_ = false;
};

// The dispreg breakdown (native_meter.hpp's counting rule): the meter's three emulated-answer sources, counted where
// the meter counts them (kernel/stubs.cpp). Host counters only; printed in the [NATIVE_VBLANK] line.
enum class Dispreg : uint8_t { Scanout = 0, Sysbuf = 1, VsyncBit = 2 };
void countDispreg(Dispreg source, uint64_t n);

// [NATIVE_VBLANK] <why> ... (with the CP census and at exit); nothing when the state is Off (=0, or the default
// standing down). Never takes the
// CPU-2 lock (native/dreg-hang): the CP thread prints it from its census, and dreg2's CP sat 9.7 s in this call behind
// the vblank thread's ticks. Its tail `| cpu2 tickMaxUs= tickSlow= waitMaxUs= waitSlow=` is the lock's own witness:
// the longest hold of a vblank tick (and the ticks holding it over 1 ms), the longest wait of a pacing call for it
// (and the waits over 1 ms) - dreg2 held ~21 ms per tick and made the CP's pacing call wait 0.9-33 s.
void finalLine(const char* why);
constexpr uint64_t kCpu2SlowNs = 1000000u;  // 1 ms: a tick holding, or a pacing call waiting, longer counts as slow

// The vblank tick period of the console path (60 Hz): a gap over 1.5 periods counts as late.
constexpr uint64_t kPeriodNs = 16667000u;

// difftest only (--native-vblank-test).
#if SR_ORACLE
namespace test {
struct Counters {
  uint64_t ticks, native, guest, released, callbacks, traceBad, late;
  uint64_t calls, host, guestCalls, wait, now, next, early, mismatch;
  uint64_t scanout, sysbuf, vsyncBit;
  uint64_t tickHoldMaxNs, tickSlow, pacingWaitMaxNs, pacingWaitSlow;  // the CPU-2 lock's witness (finalLine's tail)
};
// state: -1 = the env latch, 0 Off, 1 On, 2 Refused; toggled 0 / 1; window 1 = the toggle's ON window, 0 = OFF.
void force(int state, int toggled, int window);
void reset();                    // counters, the host counter (0) and the tick record
void setVCount(uint32_t v);      // the host counter (a walk across a wrap point starts near it)
uint32_t vcount();
Counters counters();
// The host arm's pacing core (nullptr = pacingCore): a defective core for the shadow check's negative controls.
void setCore(Outcome (*core)(uint32_t arg, LineFn line, void* user));
// The bracket's own points, for interleavings on either production path (writeReg, the executor's perform):
// afterBegin runs at the end of noteScratch1Begin (before the store), beforeEnd at the start of noteScratch1End (after
// it). nullptr = none (production).
void setBracketHooks(void (*afterBegin)(), void (*beforeEnd)());
}  // namespace test
#endif

}  // namespace sr::native::vblank
