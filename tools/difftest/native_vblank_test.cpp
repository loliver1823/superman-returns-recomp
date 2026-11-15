// display.registers (SR_NATIVE_VBLANK; kernel/native_vblank.{hpp,cpp}; docs/NATIVE_ROADMAP.md "DISPLAY.REGISTERS BUILD
// NOTE"): the host forms of GpuPresentPacingUpdate_82111E38 and the vblank ISR 820FDBA0's source 0 against the generated
// guest bodies, offline. --native-vblank-test. Exit 0 = every assertion passed (the mutant sections pass when each
// mutant FAILS at least one row). No live pacing or display acceptance: that is native_proof.py's display_registers.
//   A  the pacing differential: __imp__sub_82111E38 with the MMIO seeded vs pacingCore with the same (V, H), over the
//      arithmetic's domain (vblanks, remaining > 0 / 0 / < 0 / wrap, line % 0 / mid / saturated / 32-bit wrap, H
//      720 / 1 / 0xFFF / 0 / high bits, pct 0..0xFFFF, the +0x3B20 wrap, pc+4 aliasing the device words); decide() too
//   B  linePercent's table; C nextVCounter against today's rule written out independently (V + 720, 0 under HIGHFPS)
//   D  event scripts: ticks and gates interleaved, the guest ISR + guest 82111E38 reading the registers a replica of
//      today's vdScanoutRegsTick writes, against the production host path (TickScope + trySource0 + the strong
//      sub_82111E38, the host counter, the registers poisoned the other way): counter, countdown, releases, +0x3B20,
//      the swap callback's blocks in order, after every event
//   W  walks across the wrap points of today's answer (the 32-bit V*100 product: line % <= 8 at tick 178,957 and
//      357,914, line % < 100 from tick 59,653; V's own 2^32 wrap at tick 5,965,233): the host decision equals the
//      guest arithmetic on every tick, and the rare release-now happens where the arithmetic says it does
//   S  the shadow check (mismatch= / early=) in both A/B arms: clean when the forms agree, counted when they do not,
//      and an extra pc+4 clear on WAIT / NEXT caught (Astra round 1)
//   E  the gate states, the decision helpers (planTick per registration), the refusal table, the env default, the
//      dispatch wiring, the ISR route
//   E2 the requested forms against a missing requirement, each in a FRESH process (the latch is decided once a process;
//      this exe's `--native-vblank-test latch`): =1 refused (no native registration, the scan-out answers as today), the
//      default standing down at WARN, the On line naming =1 or the default (native/dreg-bake, Astra round 1, P3)
//   F  the per-vblank check (TickScope) on the real source 0, on mutant ones (an early clear among them) and on live
//      registers that diverge; the CP's rearm racing a release, through a real Pm4Engine (Astra round 1)
//   R  the registration's eligibility through the production tick delivery (K::vdVblankDeliver on a GuestCaller
//      thread): a callback that is not the D3D ISR runs as registered, its context untouched (Astra round 1)
//   G  mutants of the pacing core and of the counter rule: each must fail rows of A / W
//   H  the dispreg breakdown fed by VdGetSystemCommandBuffer (and the meter's column)
//   T  dreg2's stall class (native/dreg-hang): the census line returns while a tick holds the CPU-2 lock; a production
//      tick makes no OS permission query; on a resident arena, ticks back to back cost < 250 us each and the CP's
//      pacing calls never wait 100 ms for the lock (tools/difftest/native_vblank_mutation_run.py: source0_os_query,
//      census_takes_lock)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "guest/guest_harness.hpp"
#include "gfx/pm4_engine.hpp"
#include "gfx/render_draw_command.hpp"
#include "kernel/native_exec.hpp"
#include "kernel/ab_toggle.hpp"
#include "kernel/export_registry.hpp"
#include "kernel/graphics_interrupt_registration.hpp"
#include "kernel/guest_call.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/guest_protect.hpp"
#include "kernel/kpcr.hpp"
#include "kernel/native_display.hpp"
#include "kernel/native_flush.hpp"
#include "kernel/native_meter.hpp"
#include "kernel/native_vblank.hpp"
#include "kernel/stubs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" void __imp__sub_82111E38(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820FDBA0(PPCContext& __restrict, uint8_t*);
// sub_82111E38 (declared by ppc_recomp_shared.h) links to kernel/native_vblank.cpp's strong definition.

namespace {
namespace K = sr::kernel;
namespace V = sr::native::vblank;
namespace D = sr::native::display;

constexpr uint32_t kDev = 0x706B0000u, kPc = 0x706B8000u, kGlob = 0x706B9000u;
constexpr uint32_t kStack = 0x706C0000u, kPcr = 0x706C8000u, kDesc = 0x706CA000u;
constexpr uint32_t kCell = 0x82000948u, kVCounter = 0x7FC86530u, kViewport = 0x7FC86584u, kEnable = 0x7FC86544u;
constexpr uint32_t kCallback = uint32_t(PPC_CODE_BASE) + 8u;
constexpr uint32_t kDevBytes = 0x5000u, kPcBytes = 0x40u;
constexpr uint32_t kMode720 = 0x050002D0u;  // what vdScanoutRegsTick answers today (Xenia's 1280 x 720)

int passed = 0, failed = 0;
void check(bool ok, const std::string& name) {
  std::printf("%s %s\n", ok ? "  ok " : "FAIL:", name.c_str());
  ok ? ++passed : ++failed;
}
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va, uint32_t v) { K::writeGuestBE32(va, v); }

// Today's rule, written out here independently of the production function (kernel/stubs.cpp vdScanoutRegsTick,
// iter-467): each console-path tick adds the display height, HIGHFPS answers scanline 0.
uint32_t todayNext(uint32_t v, bool highFps) { return highFps ? 0u : v + 720u; }

// ------------------------------------------------------------------------------------------- guest-state snapshots
struct Snap {
  std::vector<uint8_t> dev, pc;
  uint32_t vcounter = 0, viewport = 0, cell = 0, glob = 0;
  bool operator==(const Snap& o) const {
    return dev == o.dev && pc == o.pc && vcounter == o.vcounter && viewport == o.viewport && cell == o.cell &&
           glob == o.glob;
  }
};
Snap snap() {
  Snap s;
  const uint8_t* d = K::guestVAToHost(kDev);
  const uint8_t* p = K::guestVAToHost(kPc);
  s.dev.assign(d, d + kDevBytes);
  s.pc.assign(p, p + kPcBytes);
  s.vcounter = rd(kVCounter);
  s.viewport = rd(kViewport);
  s.cell = rd(kCell);
  s.glob = rd(kGlob);
  return s;
}
uint32_t word(const std::vector<uint8_t>& b, uint32_t off) {
  const uint8_t* p = b.data() + off;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// ------------------------------------------------------------------------------------------------- A: the pacing
struct Row {
  uint32_t arg, count, last, gates, v, viewport, pcVA;
};
void seed(const Row& r) {
  std::memset(K::guestVAToHost(kDev), 0xA5, kDevBytes);
  std::memset(K::guestVAToHost(kPc), 0x3C, kPcBytes);
  wr(kCell, kGlob);
  wr(kGlob, kDev);
  wr(kDev + 0x2A14u, r.pcVA);
  wr(kDev + 0x3B10u, 0u);
  wr(kDev + 0x3B14u, r.count);
  wr(kDev + 0x3B18u, r.last);
  wr(kDev + 0x3B1Cu, 0xDEADBEEFu);  // overwritten on every path
  wr(kDev + 0x3B20u, r.gates);
  wr(r.pcVA + 4u, 1u);              // the gate closed (the GATE's SCRATCH_REG1 write-back)
  wr(kVCounter, r.v);
  wr(kViewport, r.viewport);
}
struct Fixed {
  uint32_t v, height;
};
V::Line fixedLine(void* user) {
  const auto& f = *static_cast<const Fixed*>(user);
  return V::Line{f.v, f.height};
}
using Core = V::Outcome (*)(uint32_t, V::LineFn, void*);

std::vector<Row> pacingRows() {
  std::vector<Row> rows;
  const uint32_t vblanks[] = {0u, 1u, 2u, 3u, 0xFFFFu};
  const uint32_t pcts[] = {0u, 1u, 8u, 50u, 99u, 100u, 101u, 0xFFFFu};
  const uint32_t counts[][2] = {{100u, 99u}, {100u, 100u}, {100u, 98u}, {5u, 0xFFFFFFF0u},
                                {0x80000000u, 0u}, {0u, 0x7FFFFFFFu}, {0xFFFFFFFFu, 0xFFFFFFFEu}};
  // V: scanline 0 / 1, the pct-8 edge 57 / 58, mid, the frame, past it, today's wrap-point answers (tick 178,957:
  // 128,849,040 -> 3; tick 59,653: 42,950,160 -> 68), the product's first wrap (42,949,672 / 42,949,673) and ~0
  const uint32_t vs[] = {0u, 1u, 57u, 58u, 359u, 360u, 719u, 720u, 721u, 42949672u, 42949673u, 42950160u,
                         128849040u, 0xFFFFFFFFu};
  const uint32_t viewports[] = {kMode720, 0x05000001u, 0x00000FFFu, 0x05000000u, 0xFFFFF2D0u};
  const uint32_t gates[] = {7u, 0xFFFFFFFFu};
  for (uint32_t vb : vblanks)
    for (uint32_t pct : pcts)
      for (const auto& cl : counts)
        for (uint32_t v : vs)
          for (uint32_t vp : viewports)
            for (uint32_t g : gates) rows.push_back(Row{(vb << 16) | pct, cl[0], cl[1], g, v, vp, kPc});
  // pc+4 aliasing each device word the release path touches (the store order and the reload after it)
  for (uint32_t alias : {kDev + 0x3B10u, kDev + 0x3B14u, kDev + 0x3B18u, kDev + 0x3B1Cu})
    for (uint32_t v : {0u, 720u})
      for (const auto& cl : counts) rows.push_back(Row{(1u << 16) | 8u, cl[0], cl[1], 7u, v, kMode720, alias});
  return rows;
}

// Every row: the guest body against `core`. Returns the failing rows (and prints the first few when `print`).
// decideBad: rows (unaliased, equal effects) where decide() on the pre-state predicts other effects than the guest's.
size_t pacingTable(Core core, bool print, size_t* total = nullptr, size_t* outcomeBad = nullptr,
                   size_t* decideBad = nullptr) {
  const auto rows = pacingRows();
  size_t bad = 0, obad = 0, dbad = 0;
  for (const Row& r : rows) {
    seed(r);
    PPCContext guest{};
    guest.r1.u64 = kStack;
    guest.r3.u64 = r.arg;
    guest.r13.u64 = kPcr;
    __imp__sub_82111E38(guest, K::getGuestBase());
    const Snap want = snap();
    seed(r);
    Fixed f{r.v, r.viewport & 0xFFFu};
    const V::Outcome got = core(r.arg, fixedLine, &f);
    const Snap have = snap();
    const bool same = want == have;
    if (r.pcVA == kPc && same) {
      // the outcome agrees with the effects: NOW opened the gate (pc+4 1 -> 0)
      const bool opened = word(have.pc, 4u) == 0u;
      const uint32_t cd = word(have.dev, 0x3B1Cu);
      const bool ok = got == V::Outcome::Now ? opened : !opened && (got == V::Outcome::Next ? cd == 1u : int32_t(cd) > 0);
      if (!ok) ++obad;
      // decide() predicts the same effects from the pre-state alone
      const V::Decision d = V::decide(r.arg, r.count, r.last, r.gates, V::Line{r.v, r.viewport & 0xFFFu});
      const bool dok = d.outcome == got && d.countdown == cd && d.gates == word(have.dev, 0x3B20u) &&
                       d.release == opened && (!d.release || word(have.dev, 0x3B18u) == r.count);
      if (!dok) ++dbad;
    }
    if (!same) {
      if (print && bad < 6)
        std::printf("  row arg=%08X count=%08X last=%08X gates=%08X V=%08X vp=%08X pc=%08X: guest cd=%08X last=%08X "
                    "gates=%08X pc4=%08X | host cd=%08X last=%08X gates=%08X pc4=%08X\n",
                    r.arg, r.count, r.last, r.gates, r.v, r.viewport, r.pcVA, word(want.dev, 0x3B1Cu),
                    word(want.dev, 0x3B18u), word(want.dev, 0x3B20u), word(want.pc, 4u), word(have.dev, 0x3B1Cu),
                    word(have.dev, 0x3B18u), word(have.dev, 0x3B20u), word(have.pc, 4u));
      ++bad;
    }
  }
  if (total) *total = rows.size();
  if (outcomeBad) *outcomeBad = obad;
  if (decideBad) *decideBad = dbad;
  return bad;
}

// ------------------------------------------------------------------------------------- G: the pacing core's mutants
enum Defect { kUnsigned, kNoGates, kNoCap, kGe, kStaleLast, kZeroCountdown, kWide, kHeightZero, kCap99, kDefects };
const char* const kDefectName[kDefects] = {
    "remaining compared unsigned", "no +0x3B20 store", "line % not capped", "line % >= pct releases",
    "+0x3B18 from the count read before the pc+4 store", "release now stores countdown 0",
    "V*100 in 64 bits (no 32-bit wrap)", "height 0 read as 720", "line % capped at 99"};
int g_defect = 0;
V::Outcome mutantCore(uint32_t arg, V::LineFn line, void* user) {
  const int m = g_defect;
  const uint32_t dev = PPC_LOAD_U32(PPC_LOAD_U32(0x82000948u));
  const uint32_t vblanks = arg >> 16, pct = arg & 0xFFFFu;
  const uint32_t count = PPC_LOAD_U32(dev + 0x3B14u), last = PPC_LOAD_U32(dev + 0x3B18u);
  const uint32_t gates = PPC_LOAD_U32(dev + 0x3B20u) + 1u;
  const uint32_t remaining = (vblanks - count) + last;
  if (m != kNoGates) PPC_STORE_U32(dev + 0x3B20u, gates);
  if (m == kUnsigned ? remaining > 0u : int32_t(remaining) > 0) {
    PPC_STORE_U32(dev + 0x3B1Cu, remaining);
    return V::Outcome::Wait;
  }
  const V::Line l = line(user);
  const uint32_t height = (m == kHeightZero && !l.height) ? 720u : l.height;
  const uint64_t wide = uint64_t(l.v) * 100u;
  uint64_t q = height ? (m == kWide ? wide : uint64_t(uint32_t(wide))) / height : 0u;
  q += 1u;
  if (m == kCap99) q = q < 99u ? q : 99u;
  else if (m != kNoCap) q = q < 100u ? q : 100u;
  const bool waitNext = m == kGe ? q >= pct : q > pct;  // kGe: off by one - line % == pct waits a vblank
  if (waitNext) {
    PPC_STORE_U32(dev + 0x3B1Cu, 1u);
    return V::Outcome::Next;
  }
  const uint32_t pc = PPC_LOAD_U32(dev + 0x2A14u);
  PPC_STORE_U32(pc + 4u, 0u);
  PPC_STORE_U32(dev + 0x3B18u, m == kStaleLast ? count : PPC_LOAD_U32(dev + 0x3B14u));
  PPC_STORE_U32(dev + 0x3B1Cu, m == kZeroCountdown ? 0u : remaining);
  return V::Outcome::Now;
}

// ------------------------------------------------------------------------------------- D / W: event scripts
struct CbRecord {
  uint32_t a0, a1, a2, count, gates;
  bool operator==(const CbRecord&) const = default;
};
thread_local std::vector<CbRecord> g_cbRecords;
void recordCallback(PPCContext& ctx, uint8_t*) {
  const uint32_t arg = ctx.r3.u32;
  g_cbRecords.push_back({rd(arg), rd(arg + 4u), rd(arg + 8u), rd(kDev + 0x3B14u), rd(kDev + 0x3B20u)});
}

enum class Ev { Tick, TickHighFps, Gate };
struct Event {
  Ev kind;
  uint32_t arg = 0;
};
struct State6 {
  uint32_t count, last, countdown, gates, pc4, v;
  size_t callbacks;
  bool operator==(const State6&) const = default;
};
void seedScript(uint32_t count, uint32_t last, uint32_t countdown, uint32_t callback) {
  std::memset(K::guestVAToHost(kDev), 0x5A, kDevBytes);
  std::memset(K::guestVAToHost(kPc), 0, kPcBytes);
  std::memset(K::guestVAToHost(kPcr), 0, 0x154u);
  *K::guestVAToHost(kPcr + K::kKpcrOffCurrentCpu) = 2u;
  wr(kCell, kGlob);
  wr(kGlob, kDev);
  wr(kDev + 0x2A14u, kPc);
  wr(kDev + 0x2A18u, 0u);  // the ISR's source-1 spinlock word: free
  wr(kDev + 0x3B10u, callback);
  wr(kDev + 0x3B14u, count);
  wr(kDev + 0x3B18u, last);
  wr(kDev + 0x3B1Cu, countdown);
  wr(kDev + 0x3B20u, 40u);
  g_cbRecords.clear();
}
PPCContext isrContext(uint32_t source) {
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = source;
  ctx.r4.u64 = kDev;
  ctx.r13.u64 = kPcr;
  ctx.r14.u64 = 0x829C0000u;
  return ctx;
}
State6 state6(uint32_t v) {
  return {rd(kDev + 0x3B14u), rd(kDev + 0x3B18u), rd(kDev + 0x3B1Cu), rd(kDev + 0x3B20u), rd(kPc + 4u), v,
          g_cbRecords.size()};
}
// The guest forms: the registers written per tick by today's rule (todayNext, the viewport constant), the guest ISR
// with the enable bit armed, the guest body reading the registers.
std::vector<State6> runGuestScript(const std::vector<Event>& script, uint32_t startV) {
  std::vector<State6> out;
  uint32_t v = startV;
  wr(kVCounter, v);
  wr(kViewport, kMode720);
  for (const Event& e : script) {
    if (e.kind == Ev::Gate) {
      wr(kPc + 4u, 1u);  // the GATE's SCRATCH_REG1 write-back precedes its callback
      PPCContext ctx = isrContext(0u);
      ctx.r3.u64 = e.arg;
      __imp__sub_82111E38(ctx, K::getGuestBase());
    } else {
      v = todayNext(v, e.kind == Ev::TickHighFps);
      wr(kVCounter, v);
      wr(kViewport, kMode720);
      wr(kEnable, 1u);  // the oracle's registration armed it
      PPCContext ctx = isrContext(0u);
      K::KpcrDpcScope dpc(kPcr);
      K::ExportRegistry::setGuestCallContext(true);
      __imp__sub_820FDBA0(ctx, K::getGuestBase());
      K::ExportRegistry::setGuestCallContext(false);
    }
    out.push_back(state6(v));
  }
  return out;
}
bool g_hostScriptExact = true;
// The production host path: TickScope (the host counter, the lock, the check) + trySource0, the strong sub_82111E38;
// the registers poisoned with an answer that would decide the other way, the enable bit never armed.
std::vector<State6> runHostScript(const std::vector<Event>& script, uint32_t startV) {
  std::vector<State6> out;
  V::test::setVCount(startV);
  for (const Event& e : script) {
    if (e.kind == Ev::Gate) {
      wr(kPc + 4u, 1u);
      const uint32_t truth = V::linePercent(V::test::vcount(), V::kHeight);
      wr(kVCounter, truth <= (e.arg & 0xFFFFu) ? 720u : 0u);  // the opposite decision (pct 0 excepted: both NEXT)
      wr(kViewport, kMode720);
      PPCContext ctx = isrContext(0u);
      ctx.r3.u64 = e.arg;
      sub_82111E38(ctx, K::getGuestBase());  // the strong definition: the production entry
    } else {
      wr(kEnable, 0u);  // a native registration never arms it
      PPCContext ctx = isrContext(0u);
      uint32_t exc = 0xFFFFFFFFu;
      D::Source0Result r;
      {
        V::TickScope scope(kDev, e.kind == Ev::TickHighFps, true);
        K::KpcrDpcScope dpc(kPcr);
        K::ExportRegistry::setGuestCallContext(true);
        r = D::trySource0(ctx, K::getGuestBase(), D::kD3DIsr, kDev, &exc);
        K::ExportRegistry::setGuestCallContext(false);
      }
      g_hostScriptExact &= r == D::Source0Result::Completed && exc == 0u && ctx.r1.u64 == kStack;
    }
    out.push_back(state6(V::test::vcount()));
  }
  return out;
}

// One script both ways; true when the host equals the guest after every event. `nowAt` collects the event indices
// where the guest released at once (pc+4 cleared by a gate).
bool script(const char* name, const std::vector<Event>& events, uint32_t count, uint32_t last, uint32_t countdown,
            bool callback, uint32_t startV = 0u, std::vector<size_t>* nowAt = nullptr, bool report = true) {
  seedScript(count, last, countdown, callback ? kCallback : 0u);
  const auto want = runGuestScript(events, startV);
  const auto wantCb = g_cbRecords;
  seedScript(count, last, countdown, callback ? kCallback : 0u);
  V::test::reset();
  V::test::force(1, 0, 1);
  g_hostScriptExact = true;
  const auto have = runHostScript(events, startV);
  const auto haveCb = g_cbRecords;
  size_t firstBad = events.size();
  for (size_t i = 0; i < events.size() && i < have.size(); ++i)
    if (!(want[i] == have[i])) { firstBad = i; break; }
  if (firstBad < events.size())
    std::printf("  %s event %zu: guest count=%u last=%u cd=%d gates=%u pc4=%u V=%u cbs=%zu | host count=%u last=%u "
                "cd=%d gates=%u pc4=%u V=%u cbs=%zu\n", name, firstBad, want[firstBad].count, want[firstBad].last,
                int32_t(want[firstBad].countdown), want[firstBad].gates, want[firstBad].pc4, want[firstBad].v,
                want[firstBad].callbacks, have[firstBad].count, have[firstBad].last, int32_t(have[firstBad].countdown),
                have[firstBad].gates, have[firstBad].pc4, have[firstBad].v, have[firstBad].callbacks);
  if (nowAt)
    for (size_t i = 0; i < events.size(); ++i)
      if (events[i].kind == Ev::Gate && want[i].pc4 == 0u) nowAt->push_back(i);
  const auto c = V::test::counters();
  size_t gatesN = 0, ticks = 0;
  for (const Event& e : events) (e.kind == Ev::Gate ? gatesN : ticks) += 1;
  const bool ok = firstBad == events.size() && want.size() == have.size() && wantCb == haveCb && g_hostScriptExact &&
                  c.traceBad == 0u && c.mismatch == 0u && c.early == 0u && c.host == gatesN && c.ticks == ticks &&
                  c.native == ticks;
  if (report)
    check(ok, std::string("script ") + name + ": host == guest after every event (counter, countdown, last release, "
                                              "gates, pc+4, V, callback blocks in order); check / mismatch / early 0");
  return ok;
}

// The ticks n where today's answer V = 720 n (mod 2^32) gives line % <= limit: V*100 = 72000 n (mod 2^32), and 72000 =
// 64 * 1125 with 1125 odd, so n = k * 1125^-1 (mod 2^26) for each residue 64 k below 720 * limit (line % =
// 64 k / 720 + 1 <= limit). Ascending, the first `want` after tick 0; *perPeriod = such ticks per 2^26 (k = 0 included).
std::vector<uint32_t> wrapTicks(uint32_t limit, size_t want, size_t* perPeriod = nullptr) {
  uint32_t inv = 1u;  // 1125^-1 mod 2^32 by Newton's iteration (1 -> 2 -> ... -> 32 bits), then mod 2^26
  for (int i = 0; i < 6; ++i) inv *= 2u - 1125u * inv;
  inv &= (1u << 26) - 1u;
  std::vector<uint32_t> n;
  const uint32_t kmax = (720u * limit + 63u) / 64u;  // k < kmax <=> 64 k < 720 limit
  for (uint32_t k = 1; k < kmax; ++k) n.push_back(uint32_t((uint64_t(k) * inv) & ((1u << 26) - 1u)));
  if (perPeriod) *perPeriod = n.size() + 1u;
  std::sort(n.begin(), n.end());
  if (n.size() > want) n.resize(want);
  return n;
}

// ------------------------------------------------------------------------------------- F: source-0 mutants
enum S0Defect { kS0Real, kS0NoCount, kS0DecNonPositive, kS0NoPcClear, kS0NoLast, kS0DecTwice, kS0TouchGates,
                kS0EarlyClear, kS0Defects };
const char* const kS0Name[kS0Defects] = {"the real trySource0", "no counter increment", "countdown decremented at 0",
                                         "release without pc+4 = 0", "release without +0x3B18",
                                         "countdown decremented twice", "+0x3B20 touched",
                                         "pc+4 cleared early (2 -> 1, the countdown itself right)"};
void mutantSource0(int m) {
  const uint32_t count = rd(kDev + 0x3B14u) + (m == kS0NoCount ? 0u : 1u);
  wr(kDev + 0x3B14u, count);
  const uint32_t cd = rd(kDev + 0x3B1Cu);
  if (int32_t(cd) > 0 || m == kS0DecNonPositive) {
    const uint32_t next = cd - (m == kS0DecTwice ? 2u : 1u);
    wr(kDev + 0x3B1Cu, next);
    if (next == 0u || (m == kS0DecTwice && cd == 1u)) {
      if (m != kS0NoPcClear) wr(rd(kDev + 0x2A14u) + 4u, 0u);
      if (m != kS0NoLast) wr(kDev + 0x3B18u, rd(kDev + 0x3B14u));
    } else if (m == kS0EarlyClear) {
      wr(rd(kDev + 0x2A14u) + 4u, 0u);  // the gate opened a vblank (or more) early
    }
  }
  if (m == kS0TouchGates) wr(kDev + 0x3B20u, rd(kDev + 0x3B20u) + 1u);
}

// The CP's side of pc+4 (Astra round 1, P2): a real Pm4Engine in CP mode with SCRATCH_UMSK bit 1 and SCRATCH_ADDR =
// the pc block, so writeReg(0x579, 1) is the GATE's rearm exactly as presentGateApply / the PM4 form perform it - the
// SCRATCH_REG1 write-back to pc+4 and its generation (gfx/record_engine.cpp RecordEngine::writeReg since the record
// engine split; the executor's deferred perform stays gfx/pm4_engine.cpp's). SCRATCH_ADDR holds the guest VA here and
// the map resolves it as one.
// native/dreg-hang: hooks into the write-back's own steps - physToHost runs after the bracket's Begin and before the
// store, the P2 trace hook (what 1 = performed) after the store and before End - so a test can open or close a vblank
// tick at exactly those points of the production writeReg.
void (*g_onScratchMap)() = nullptr;
void (*g_onScratchStored)() = nullptr;
struct CpRearm {
  std::unique_ptr<sr::gfx::Pm4Engine> engine = std::make_unique<sr::gfx::Pm4Engine>();
  static uint8_t* map(void*, uint32_t addr) {
    if (addr == kPc + 4u && g_onScratchMap) g_onScratchMap();
    return K::guestVAToHost(addr);
  }
  static void trace(void*, uint32_t what, uint32_t addr, uint32_t) {
    if (what == 1u && addr == kPc + 4u && g_onScratchStored) g_onScratchStored();
  }
  CpRearm() {
    engine->submitDraws = false;
    engine->cpMode = true;
    engine->physToHost = map;
    engine->cp.trace = trace;
    engine->writeReg(0x1DCu, 0x2u);  // SCRATCH_UMSK: REG1 writes back
    engine->writeReg(0x1DDu, kPc);   // SCRATCH_ADDR: REG1 -> kPc + 4
  }
  void rearm() { engine->writeReg(0x579u, 1u); }
};
// A non-release vblank tick (countdown 0: the source 0's effect is the counter + 1) opened and closed by the hooks.
std::optional<V::TickScope> g_raceTick;
int g_raceOpens = 0, g_raceCloses = 0;
void raceOpen() {
  ++g_raceOpens;
  g_raceTick.emplace(kDev, false, true);
  wr(kDev + 0x3B14u, rd(kDev + 0x3B14u) + 1u);
}
void raceClose() {
  if (g_raceTick) ++g_raceCloses;
  g_raceTick.reset();
}
// Astra round 2 (P2): a whole tick inside an open bracket - after the CP's store of 1, before its End - during which
// a foreign write clears pc+4. A write-back of 1 cannot explain a 0.
void raceOpenClearClose() {
  raceOpen();
  wr(kPc + 4u, 0u);
  raceClose();
}

// R: a registered vblank callback that is NOT the D3D ISR (another title's, or a debug hook's): it records its calls.
constexpr uint32_t kIsrCb = uint32_t(PPC_CODE_BASE) + 12u;
std::vector<std::array<uint32_t, 2>> g_isrCalls;
void recordIsr(PPCContext& ctx, uint8_t*) { g_isrCalls.push_back({ctx.r3.u32, ctx.r4.u32}); }
void noopRunner(uint32_t, uint32_t, uint32_t, uint8_t*, size_t) {}

// A host-arm pacing core with an extra clear: the decision and every store right, then pc+4 = 0 on WAIT / NEXT.
V::Outcome extraClearCore(uint32_t arg, V::LineFn line, void* user) {
  const V::Outcome o = V::pacingCore(arg, line, user);
  if (o != V::Outcome::Now) PPC_STORE_U32(PPC_LOAD_U32(PPC_LOAD_U32(PPC_LOAD_U32(kCell)) + 0x2A14u) + 4u, 0u);
  return o;
}

// ------------------------------------------------------------------------------------- G: the counter rule's mutants
enum CounterDefect { kCnSaturate, kCnHighFps, kCn721, kCnWideProduct, kCnDefects };
const char* const kCnName[kCnDefects] = {"the counter saturating at 2^32 (no wrap)", "HIGHFPS not answering 0",
                                         "+721 a tick", "line % from a 64-bit V*100 (no product wrap)"};
uint32_t mutantNext(int m, uint32_t v, bool hf) {
  if (hf && m != kCnHighFps) return 0u;
  if (m == kCnSaturate) return v > 0xFFFFFFFFu - 720u ? 0xFFFFFFFFu : v + 720u;
  return v + (m == kCn721 ? 721u : 720u);
}
uint32_t mutantLinePercent(int m, uint32_t v) {
  if (m != kCnWideProduct) return V::linePercent(v, 720u);
  const uint64_t q = uint64_t(v) * 100u / 720u + 1u;
  return q < 100u ? uint32_t(q) : 100u;
}

// E2's child (native/dreg-bake, Astra round 1 on the branch, P3): the latch is decided once a process, so a requested
// form with a missing requirement is exercised in a FRESH process - this exe's `--native-vblank-test latch` under the
// env the parent set. It prints the latch's outcome; the boot line computeState writes (On / REFUSED / stand-down)
// reaches the same output, which the parent reads.
int runNativeVblankLatch() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  K::registerStubs();
  const V::State s = V::state();
  const V::TickPlan p = V::planTick(false, D::kD3DIsr, kDev);
  std::printf("[NATIVE_VBLANK_LATCH] state=%d requested=%d on=%d nativeReg=%d scanout=%d planNative=%d planScoped=%d\n",
              int(s), int(V::vblankGateRequested()), int(V::on()), int(V::nativeRegistration()),
              int(V::scanoutAnswers()), int(p.native), int(p.scoped));
  return 0;
}

}  // namespace

int runNativeVblankTest(const char* mode) {
  if (mode && !std::strcmp(mode, "latch")) return runNativeVblankLatch();
  if (mode) return 2;
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // every row reaches the log even if a later section stops the process
  passed = failed = 0;
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  // The host source 0 admits guest ranges against the process's registered arena (native/dreg-hang; the game registers
  // it at guest init): the whole difftest guest memory here.
  uint8_t* arenaBase0 = nullptr;
  size_t arenaSize0 = 0;
  const K::ExCreateThreadRunnerFn arenaRunner0 = K::getExCreateThreadRunner(&arenaBase0, &arenaSize0);
  const size_t arenaSize = K::getGuestSize() ? K::getGuestSize() : size_t(0x100000000ull);
  K::registerExCreateThreadRunner(noopRunner, K::getGuestBase(), arenaSize);
  const uint32_t savedCell = rd(kCell), savedVc = rd(kVCounter), savedVp = rd(kViewport), savedEn = rd(kEnable);
  auto*& slot = PPC_LOOKUP_FUNC(K::getGuestBase(), kCallback);
  PPCFunc* const savedSlot = slot;
  slot = recordCallback;  // the swap callback of script D's callback case (dev+0x3B10 = kCallback)

  // ---- A: the pacing differential
  {
    size_t total = 0, obad = 0, dbad = 0;
    const size_t bad = pacingTable(V::pacingCore, true, &total, &obad, &dbad);
    check(bad == 0u && total > 30000u,
          "A: pacingCore == __imp__sub_82111E38 on " + std::to_string(total) + " rows (device, pc block, MMIO, cell; " +
              std::to_string(bad) + " differ)");
    check(obad == 0u, "A: the outcome (WAIT / NOW / NEXT) agrees with the effects on every unaliased row");
    check(dbad == 0u, "A: decide() on the pre-state predicts the guest's effects on every unaliased row (the shadow)");
  }

  // ---- B: linePercent
  {
    struct Lp { uint32_t v, h, want; };
    const Lp lp[] = {{0u, 720u, 1u}, {57u, 720u, 8u}, {58u, 720u, 9u}, {359u, 720u, 50u}, {360u, 720u, 51u},
                     {712u, 720u, 99u}, {713u, 720u, 100u}, {719u, 720u, 100u}, {720u, 720u, 100u},
                     {42949673u, 720u, 1u}, {42949672u, 720u, 100u}, {42950160u, 720u, 68u}, {128849040u, 720u, 3u},
                     {5u, 0u, 1u}, {0xFFFFFFFFu, 1u, 100u}, {1u, 0xFFFu, 1u}, {41u, 0xFFFu, 2u}};
    bool ok = true;
    for (const Lp& c : lp) {
      const uint32_t got = V::linePercent(c.v, c.h);
      if (got != c.want) {
        ok = false;
        std::printf("  linePercent(%u, %u) = %u, want %u\n", c.v, c.h, got, c.want);
      }
    }
    check(ok, "B: linePercent's table (1 at scanline 0, the pct-8 edge 57 / 58, the 32-bit product wrap, today's "
              "wrap-point answers 68 / 3, height 0 -> 1, cap 100)");
  }

  // ---- C: nextVCounter is today's rule
  {
    bool ok = V::kViewportAnswer == kMode720 && V::kHeight == 720u;
    uint32_t v = 0u, seedv = 0x13579BDFu;
    for (int i = 0; i < 200000 && ok; ++i) {
      const bool hf = (i % 97) == 0;
      ok &= V::nextVCounter(v, hf) == todayNext(v, hf);
      v = (i % 1000) == 0 ? (seedv = seedv * 1664525u + 1013904223u) : V::nextVCounter(v, hf);
    }
    ok &= V::nextVCounter(0xFFFFFFFFu - 719u, false) == 0u && V::nextVCounter(0xFFFFFFFFu - 100u, false) == 619u &&
          V::nextVCounter(123456u, true) == 0u;
    check(ok, "C: nextVCounter == today's rule (V + 720 with the 32-bit wrap, 0 under HIGHFPS) on 200,000 steps and "
              "the 2^32 edge; the viewport answer 0x050002D0");
  }

  // ---- D: event scripts (guest forms vs the production host path)
  const uint32_t i1p8 = (1u << 16) | 8u, i2p8 = (2u << 16) | 8u, i3p8 = (3u << 16) | 8u, i0p8 = 8u;
  using E = Event;
  const E t{Ev::Tick}, th{Ev::TickHighFps};
  {
    script("interval 1, pct 8: NOW at V 0, then today's saturated answer (NEXT, released by the next tick)",
           {E{Ev::Gate, i1p8}, t, t, E{Ev::Gate, i1p8}, t, t, E{Ev::Gate, i1p8}, t, t}, 1000u, 998u, 0u, false);
    script("before the first tick: V 0 answers line % 1 (NOW), as the zero page and the host counter both do",
           {E{Ev::Gate, i0p8}, t, E{Ev::Gate, i0p8}, t, t}, 1000u, 998u, 0u, false);
    script("interval 2 / 4: remaining > 0 -> a countdown of 1 / 2 / 3 released by the ticks",
           {t, E{Ev::Gate, i2p8}, t, t, E{Ev::Gate, i3p8}, t, t, t, t, E{Ev::Gate, i3p8}, t, t, t, t},
           50u, 50u, 0u, false);
    script("pct 0 never releases at once; pct 100 / 0xFFFF always",
           {t, t, E{Ev::Gate, 1u << 16}, t, t, E{Ev::Gate, (1u << 16) | 100u}, t, t,
            E{Ev::Gate, (1u << 16) | 0xFFFFu}, t},
           7u, 5u, 0u, false);
    script("the countdown's signed edges (a negative countdown, the 0x7FFFFFFF counter wrap)",
           {t, t, E{Ev::Gate, i1p8}, t, t, t}, 0x7FFFFFFEu, 0x7FFFFFFCu, 0x80000000u, false);
    script("the swap callback (dev+0x3B10 set): its {counter, +0x3B20, 0} blocks in order",
           {t, E{Ev::Gate, i1p8}, t, t, E{Ev::Gate, i2p8}, t, t, t}, 300u, 297u, 0u, true);
    script("HIGHFPS ticks answer scanline 0 (release at once), and the console path restarts from 0 after them",
           {th, E{Ev::Gate, i0p8}, th, th, E{Ev::Gate, i0p8}, t, E{Ev::Gate, i0p8}, t, E{Ev::Gate, i0p8}}, 90u, 88u, 0u,
           false, 5000u);
  }

  // ---- W: across today's wrap points
  {
    size_t perLe8 = 0, perLt100 = 0;
    const auto le8 = wrapTicks(8u, 3u, &perLe8), lt100 = wrapTicks(99u, 2u, &perLt100);
    // the same points by brute force over the first 2.3 million ticks: today's rule and linePercent, no algebra
    std::vector<uint32_t> bruteLe8, bruteLt100;
    {
      uint32_t v = 0u;
      for (uint32_t n = 1; n <= 2300000u; ++n) {
        v = todayNext(v, false);
        const uint32_t lp = V::linePercent(v, 720u);
        if (lp <= 8u && bruteLe8.size() < 3u) bruteLe8.push_back(n);
        if (lp < 100u && bruteLt100.size() < 2u) bruteLt100.push_back(n);
      }
    }
    check(le8.size() == 3u && le8[0] == 178957u && le8[1] == 357914u && le8[2] == 2207136u && lt100.size() == 2u &&
              lt100[0] == 59653u && lt100[1] == 119305u && perLe8 == 90u && perLt100 == 1114u && bruteLe8 == le8 &&
              bruteLt100 == lt100,
          "W: today's wrap points from the arithmetic - line % <= 8 first at ticks 178,957 / 357,914 / 2,207,136, "
          "line % < 100 first at 59,653 / 119,305 (90 / 1,114 per 2^26 ticks), the modular inverse and a brute-force "
          "walk agreeing");
    // A gate (vblanks 0: always a line % test) after every tick from 4 ticks before the point to 4 after.
    auto walk = [&](const char* what, uint32_t n, uint32_t arg, bool expectNow) {
      std::vector<Event> ev;
      for (int i = 0; i < 9; ++i) { ev.push_back(t); ev.push_back(E{Ev::Gate, arg}); }
      std::vector<size_t> nowAt;
      const uint32_t start = uint32_t(uint64_t(720u) * (n - 5u));  // ticks n-4 .. n+4 follow
      const bool same = script(what, ev, 1000u, 1000u, 0u, false, start, &nowAt, false);
      // the event index of the gate after tick n: tick n is the 5th tick (index 8), its gate index 9
      const bool atPoint = std::find(nowAt.begin(), nowAt.end(), size_t(9)) != nowAt.end();
      bool elsewhere = false;
      for (size_t i : nowAt) elsewhere |= i != 9u;
      check(same && atPoint == expectNow && !elsewhere,
            std::string("W: ") + what + ": host == guest on all 9 ticks; release-now " +
                (expectNow ? "exactly at the point" : "nowhere"));
    };
    char name[160];
    for (uint32_t n : le8) {
      std::snprintf(name, sizeof(name), "tick %u (V %u, line %% %u), pct 8", n, uint32_t(uint64_t(720u) * n),
                    V::linePercent(uint32_t(uint64_t(720u) * n), 720u));
      walk(name, n, i0p8, true);
    }
    for (uint32_t n : lt100) {
      const uint32_t lp = V::linePercent(uint32_t(uint64_t(720u) * n), 720u);
      std::snprintf(name, sizeof(name), "tick %u (V %u, line %% %u), pct 8: still NEXT", n,
                    uint32_t(uint64_t(720u) * n), lp);
      walk(name, n, i0p8, false);
      std::snprintf(name, sizeof(name), "tick %u, pct %u: NOW at the point only", n, lp);
      walk(name, n, lp, true);
    }
    // V's own 2^32 wrap: tick 5,965,233 is the first whose V (= 720 n - 2^32 = 464) is past it: line % 65
    walk("V's own 2^32 wrap (tick 5,965,233: V 464, line % 65), pct 65", 5965233u, 65u, true);
  }

  // ---- S: the shadow check in both arms
  {
    // pre-state: a line % test (vblanks 0); the host counter answers V = 0 (line % 1, NOW at pct 8)
    auto call = [](int toggled, int window, uint32_t hostV, uint32_t mmioV) {
      seedScript(1000u, 998u, 0u, 0u);
      V::test::reset();
      V::test::force(1, toggled, window);
      V::test::setVCount(hostV);
      wr(kVCounter, mmioV);
      wr(kViewport, kMode720);
      wr(kPc + 4u, 1u);
      PPCContext ctx = isrContext(0u);
      ctx.r3.u64 = 8u;
      sub_82111E38(ctx, K::getGuestBase());
      return V::test::counters();
    };
    const auto onSame = call(1, 1, 0u, 0u), onDiff = call(1, 1, 0u, 720u), offSame = call(1, 0, 720u, 720u),
               offDiff = call(1, 0, 0u, 720u), untoggled = call(0, 1, 0u, 720u);
    check(onSame.mismatch == 0u && onSame.early == 0u && onSame.host == 1u,
          "S: ON arm, the registers answer as the host counter: mismatch 0, early 0");
    check(onDiff.mismatch == 1u && onDiff.early == 1u && onDiff.now == 1u,
          "S: ON arm, the registers say NEXT where the host releases: mismatch 1, early 1");
    check(offSame.mismatch == 0u && offSame.guestCalls == 1u,
          "S: OFF arm, the guest body decides as the host counter predicts: mismatch 0");
    check(offDiff.mismatch == 1u && offDiff.guestCalls == 1u,
          "S: OFF arm, the guest body decides otherwise than the host prediction: mismatch 1");
    check(untoggled.mismatch == 0u && untoggled.early == 0u,
          "S: untoggled (no live registers): the host form against its own prediction, mismatch 0 whatever the page");
    // Astra round 1, P2 (premature clears): a host core whose decision and stores are right but which also clears
    // pc+4 on WAIT / NEXT. Host V 720 (NEXT at pct 8); arg 3 << 16 | 8 on count 1000 / last 998 = remaining 1 (WAIT).
    auto extra = [](int toggled, uint32_t arg, bool defective) {
      seedScript(1000u, 998u, 0u, 0u);
      V::test::reset();
      V::test::force(1, toggled, 1);
      V::test::setVCount(720u);
      wr(kVCounter, 720u);
      wr(kViewport, kMode720);
      wr(kPc + 4u, 1u);
      V::test::setCore(defective ? extraClearCore : nullptr);
      PPCContext ctx = isrContext(0u);
      ctx.r3.u64 = arg;
      sub_82111E38(ctx, K::getGuestBase());
      V::test::setCore(nullptr);
      const auto c = V::test::counters();
      return std::array<uint64_t, 4>{c.mismatch, c.early, c.next, c.wait};
    };
    const auto nextBad = extra(0, 8u, true), waitBad = extra(0, (3u << 16) | 8u, true),
               nextBadAb = extra(1, 8u, true), nextOk = extra(0, 8u, false), waitOk = extra(0, (3u << 16) | 8u, false);
    check(nextBad[0] == 1u && nextBad[1] == 0u && nextBad[2] == 1u && nextBadAb[0] == 1u,
          "S: an extra pc+4 clear on NEXT (outcome and stores right) counts mismatch 1 with early 0, untoggled and in "
          "the A/B's ON arm");
    check(waitBad[0] == 1u && waitBad[3] == 1u, "S: an extra pc+4 clear on WAIT counts mismatch 1");
    check(nextOk[0] == 0u && waitOk[0] == 0u && rd(kPc + 4u) == 1u,
          "S: the production core on the same inputs: mismatch 0, pc+4 left at 1 (the gate stays closed)");
  }

  // ---- E: gate states, helpers, wiring
  {
    struct S { int state, toggled, window; bool on, nativeReg, scanout, active; };
    const S states[] = {{0, 0, 1, false, false, true, false}, {2, 0, 1, false, false, true, false},
                        {1, 0, 1, true, true, false, true}, {1, 0, 0, true, true, false, true},
                        {1, 1, 1, true, false, true, true}, {1, 1, 0, true, false, true, false}};
    bool ok = true, plans = true;
    const uint32_t d3d = D::kD3DIsr;
    for (const S& s : states) {
      V::test::force(s.state, s.toggled, s.window);
      ok &= V::on() == s.on && V::nativeRegistration() == s.nativeReg && V::scanoutAnswers() == s.scanout &&
            V::active() == s.active;
      // the plan per registration: the D3D ISR with a device - native in an active window or on a native
      // registration; any other callback, or no context, never native and never device-checked
      const V::TickPlan pd = V::planTick(false, d3d, kDev), pn = V::planTick(true, d3d, kDev),
                        po = V::planTick(false, kCallback, kDev), pz = V::planTick(false, d3d, 0u);
      plans &= pd.native == s.active && pd.scoped == s.on && pd.checkDev == kDev && pn.native && pn.checkDev == kDev &&
               !po.native && po.scoped == s.on && po.checkDev == 0u && !pz.native && pz.checkDev == 0u;
    }
    check(ok, "E: on / nativeRegistration / scanoutAnswers / active per state (Off, Refused, On, toggled ON / OFF)");
    check(plans, "E: planTick per state - the host source 0 and the device check only for the D3D ISR 820FDBA0 with a "
                 "context; another callback or a null context runs its own callback, unchecked; scoped iff the gate is on");
    // native/dreg-bake: three requests (=0 Off, unset the baked Default, =1 Explicit) x the six requirements
    bool refusal = true;
    for (int r = 0; r < 3; ++r) {
      for (int m = 0; m < 64; ++m) {
        const V::Request req = V::Request(r);
        const bool gate = m & 1, flush = m & 2, seg = m & 4, kp = m & 8, scan = m & 16, pf = m & 32;
        const bool all = gate && flush && seg && kp && scan && pf;
        const V::State want = req == V::Request::Off ? V::State::Off
                              : all                  ? V::State::On
                              : req == V::Request::Explicit ? V::State::Refused
                                                            : V::State::Off;  // the default stands down
        refusal &= V::decideState(req, gate, flush, seg, kp, scan, pf) == want;
      }
    }
    check(refusal, "E: the latch's decision: =0 Off; On only with SR_NATIVE_GATE + flush + segmentless + SR_KPCR + "
                   "today's scan-out answers + no page-protecting diagnostic (the host source 0's arithmetic "
                   "admission); else an explicit =1 Refused and the default (unset) standing down to Off (192 "
                   "combinations)");
    check(V::requestOf(nullptr) == V::Request::Default && V::requestOf("0") == V::Request::Off &&
              V::requestOf("00") == V::Request::Off && V::requestOf("1") == V::Request::Explicit &&
              V::requestOf("") == V::Request::Default && V::requestOf("2") == V::Request::Default &&
              V::requestOf("on") == V::Request::Default,
          "E: the env's forms - unset (and any value but 0 / 1) the baked default, =0 off, =1 explicit");
    V::test::force(-1, 0, 1);
    // The process's own env, read as production reads it: the requirements' own helpers and env forms.
    const char* env = std::getenv("SR_NATIVE_VBLANK");
    const char* segEnv = std::getenv("SR_NATIVE_SEGMENTLESS");
    const char* scanEnv = std::getenv("SR_VD_SCANOUT_REGS");
    const V::Request envReq = V::requestOf(env);
    const V::State envWant = V::decideState(
        envReq, sr::native::flush::presentGateOn(), sr::native::flush::on(), !segEnv || segEnv[0] != '0',
        K::kpcrEnabled(), (!scanEnv || scanEnv[0] != '0') && sr::abt::reg("SR_VD_SCANOUT_REGS") < 0,
        !K::guest_protect::diagnosticRequested());
    const bool latchedOn = V::state() == V::State::On;
    // every form is checked (Astra round 1 on native/dreg-bake, P3: an explicit =1 is no longer waved through)
    check(V::state() == envWant && V::vblankGateRequested() == (envReq != V::Request::Off) &&
              V::on() == latchedOn && V::scanoutAnswers() == !latchedOn && V::nativeRegistration() == latchedOn &&
              V::planTick(false, D::kD3DIsr, kDev).native == latchedOn &&
              V::planTick(false, D::kD3DIsr, kDev).scoped == latchedOn &&
              (envReq != V::Request::Off || !latchedOn) &&
              (V::state() != V::State::Refused || envReq == V::Request::Explicit),
          "E: the process's env, whatever its form (unset = the baked default ON, =0 off, =1 explicit) - the latch is "
          "the decision on the live requirements (Refused only for =1), the override's first test passes unless =0, "
          "the tick and the registration follow the latch");
    std::printf("E: env SR_NATIVE_VBLANK=%s -> latched %s\n", env ? env : "(unset)",
                latchedOn ? "On" : V::state() == V::State::Refused ? "Refused" : "Off");

    // E2 (native/dreg-bake, Astra round 1 on the branch, P3): each requested form against a missing requirement in a
    // FRESH process. A missing requirement makes =1 REFUSED - no native registration (the guest ISR keeps source 0 and
    // the vsync-enable bit), the scan-out answers written as today, the tick unscoped - and the default stand down at
    // WARN; with every requirement the On line names the form (=1 or the baked default); =0 prints no boot line.
    {
      struct Case {
        const char* what;
        const char* vblank;  // SR_NATIVE_VBLANK (nullptr = unset)
        const char* key;     // the requirement turned off (nullptr = none)
        const char* value;
        int state, requested;
        const char* line;     // in the child's output
        const char* notLine;  // never in it
      };
      const Case cases[] = {
          {"=1, SR_NATIVE_GATE=0", "1", "SR_NATIVE_GATE", "0", 2, 1,
           "[NATIVE_VBLANK] REFUSED: SR_NATIVE_VBLANK=1 requires", "SR_NATIVE_VBLANK on ("},
          {"=1, SR_NATIVE_SEGMENTLESS=0", "1", "SR_NATIVE_SEGMENTLESS", "0", 2, 1, "[NATIVE_VBLANK] REFUSED:",
           "SR_NATIVE_VBLANK on ("},
          {"=1, a page-protecting diagnostic (SR_PROTECT_ZERO=1)", "1", "SR_PROTECT_ZERO", "1", 2, 1,
           "[NATIVE_VBLANK] REFUSED:", "SR_NATIVE_VBLANK on ("},
          {"unset, SR_NATIVE_GATE=0", nullptr, "SR_NATIVE_GATE", "0", 0, 1,
           "[WARN ] [BOOT] [NATIVE_VBLANK] default stands down: SR_NATIVE_GATE (0)", "[NATIVE_VBLANK] REFUSED"},
          {"=1, every requirement", "1", nullptr, nullptr, 1, 1, "[NATIVE_VBLANK] SR_NATIVE_VBLANK on (=1)",
           "SR_NATIVE_VBLANK on (the baked default"},
          {"unset, every requirement", nullptr, nullptr, nullptr, 1, 1,
           "[NATIVE_VBLANK] SR_NATIVE_VBLANK on (the baked default;", "SR_NATIVE_VBLANK on (=1)"},
          {"=0", "0", nullptr, nullptr, 0, 0, "[NATIVE_VBLANK_LATCH] state=0", "[NATIVE_VBLANK] "}};
      char* self = nullptr;
      const bool haveSelf = _get_pgmptr(&self) == 0 && self && *self;
      auto saved = [](const char* name) {
        const char* v = std::getenv(name);
        return std::pair<bool, std::string>(v != nullptr, v ? v : "");
      };
      for (const Case& c : cases) {
        const auto oldVblank = saved("SR_NATIVE_VBLANK");
        const auto oldKey = c.key ? saved(c.key) : std::pair<bool, std::string>(false, "");
        _putenv_s("SR_NATIVE_VBLANK", c.vblank ? c.vblank : "");  // "" removes the variable
        if (c.key) _putenv_s(c.key, c.value);
        std::string out;
        int rc = -1;
        if (haveSelf) {
          const std::string cmd = std::string("\"\"") + self + "\" --native-vblank-test latch 2>&1\"";
          if (FILE* f = _popen(cmd.c_str(), "r")) {
            char buf[4096];
            while (std::fgets(buf, sizeof buf, f)) out += buf;
            rc = _pclose(f);
          }
        }
        _putenv_s("SR_NATIVE_VBLANK", oldVblank.first ? oldVblank.second.c_str() : "");
        if (c.key) _putenv_s(c.key, oldKey.first ? oldKey.second.c_str() : "");
        int st = -1, rq = -1, on = -1, nr = -1, sc = -1, pn = -1, ps = -1;
        const size_t at = out.find("[NATIVE_VBLANK_LATCH] ");
        const bool parsed =
            at != std::string::npos &&
            std::sscanf(out.c_str() + at,
                        "[NATIVE_VBLANK_LATCH] state=%d requested=%d on=%d nativeReg=%d scanout=%d planNative=%d "
                        "planScoped=%d", &st, &rq, &on, &nr, &sc, &pn, &ps) == 7;
        const int wantOn = c.state == 1 ? 1 : 0;
        const bool ok = rc == 0 && parsed && st == c.state && rq == c.requested && on == wantOn && nr == wantOn &&
                        sc == 1 - wantOn && pn == wantOn && ps == wantOn && out.find(c.line) != std::string::npos &&
                        out.find(c.notLine) == std::string::npos;
        check(ok, std::string("E2: a fresh process, SR_NATIVE_VBLANK ") + c.what + " -> " +
                      (c.state == 2   ? "REFUSED, no native registration, the scan-out answers as today, unscoped"
                       : c.state == 1 ? "On, its line naming the form"
                       : c.requested  ? "the default stands down at WARN, today's path"
                                      : "Off, no boot line"));
        if (!ok)
          std::printf("  child rc=%d parsed=%d state=%d requested=%d on=%d nativeReg=%d scanout=%d plan=%d/%d; "
                      "output: %.400s\n", rc, int(parsed), st, rq, on, nr, sc, pn, ps, out.c_str());
      }
    }
    check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82111E38u) == sub_82111E38 &&
              PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82111E38u) != __imp__sub_82111E38,
          "E: the dispatch slot of 0x82111E38 (the ISR's bctrl) is the strong sub_82111E38");

    // The override per state on one input: the host counter says V 0 (NOW), the registers say 720 (NEXT).
    auto decideVia = [](int st, int tg, int win, bool viaIsr) {
      seedScript(1000u, 998u, 0u, 0u);
      V::test::reset();
      V::test::force(st, tg, win);
      V::test::setVCount(0u);
      wr(kPc + 4u, 1u);
      wr(kVCounter, 720u);
      wr(kViewport, kMode720);
      if (viaIsr) {
        // the CALLBACK block's write-backs, then the guest ISR's source 1 (its bctrl through the dispatch table)
        wr(kPc + 0x10u, 0x82111E38u);
        wr(kPc + 0x14u, (1u << 16) | 8u);
        wr(kPc + 0u, 4u);
        PPCContext ctx = isrContext(1u);
        K::KpcrDpcScope dpc(kPcr);
        K::ExportRegistry::setGuestCallContext(true);
        __imp__sub_820FDBA0(ctx, K::getGuestBase());
        K::ExportRegistry::setGuestCallContext(false);
      } else {
        PPCContext ctx = isrContext(0u);
        ctx.r3.u64 = (1u << 16) | 8u;
        sub_82111E38(ctx, K::getGuestBase());
      }
      return std::array<uint32_t, 3>{rd(kPc + 4u), rd(kDev + 0x3B1Cu), rd(kPc + 0u)};
    };
    auto host = [](const std::array<uint32_t, 3>& r) { return r[0] == 0u && r[1] == 0xFFFFFFFFu; };  // NOW, remaining -1
    auto guest = [](const std::array<uint32_t, 3>& r) { return r[0] == 1u && r[1] == 1u; };          // NEXT
    const auto off = decideVia(0, 0, 1, false), refused = decideVia(2, 0, 1, false), onUn = decideVia(1, 0, 1, false),
               onTogOn = decideVia(1, 1, 1, false), onTogOff = decideVia(1, 1, 0, false);
    check(guest(off) && guest(refused) && host(onUn) && host(onTogOn) && guest(onTogOff),
          "E: sub_82111E38 runs the guest body Off / Refused / in the A/B's OFF window and the host form On (the "
          "registers read only by the guest body)");
    const auto isr = decideVia(1, 0, 1, true);
    const auto cnt = V::test::counters();
    check(host(isr) && isr[2] == 0u && cnt.calls == 1u && cnt.host == 1u,
          "E: the ISR's source 1 (cb = pc+0x10 = 82111E38) reaches the host form, then clears its pending bit");
    const auto isrOff = decideVia(0, 0, 1, true);
    check(guest(isrOff) && isrOff[2] == 0u, "E: the same ISR route with the gate Off runs the guest body");
  }

  // ---- F: the per-vblank check
  {
    CpRearm cp;
    // after: what reaches pc+4 between the ISR's effects and the check - 0 nothing, 1 the CP's rearm (its
    // SCRATCH_REG1 write-back through a real Pm4Engine), 2 a write of 1 that is NOT the CP's (no write-back)
    auto run = [&cp](int m, uint32_t countdown, int toggled = 0, bool liveOk = true, int after = 0,
                     uint32_t pc4Init = 1u) {
      V::test::force(1, toggled, 1);
      seedScript(10u, 5u, countdown, 0u);
      wr(kPc + 4u, pc4Init);
      V::test::reset();
      V::test::setVCount(1440u);
      {
        V::TickScope scope(kDev, false, true);
        if (toggled) {  // the scan-out write a toggled roll makes inside the scope (the lock is held: no vcount())
          const uint32_t today = todayNext(1440u, false);
          wr(kVCounter, liveOk ? today : today + 1u);
          wr(kViewport, kMode720);
        }
        if (m == kS0Real) {
          PPCContext ctx = isrContext(0u);
          uint32_t exc = 0;
          K::KpcrDpcScope dpc(kPcr);
          K::ExportRegistry::setGuestCallContext(true);
          D::trySource0(ctx, K::getGuestBase(), D::kD3DIsr, kDev, &exc);
          K::ExportRegistry::setGuestCallContext(false);
        } else {
          mutantSource0(m);
        }
        // the interleaving Astra round 1 names: the CP sees the release, passes its hold and rearms the next gate
        // (outside the CPU-2 lock) before this tick's check reads pc+4
        if (after == 1) cp.rearm();
        if (after == 2) wr(kPc + 4u, 1u);
      }
      return V::test::counters().traceBad;
    };
    bool real = true;
    for (uint32_t cd : {0u, 1u, 2u, 0x80000000u}) real &= run(kS0Real, cd) == 0u && run(kS0Real, cd, 1) == 0u;
    check(real && V::test::vcount() == 2160u,
          "F: the real source 0 passes the per-vblank check at countdown 0 / 1 / 2 / negative, untoggled and with live "
          "registers equal to the host counter (which advanced 1440 -> 2160)");
    for (int m = kS0NoCount; m < kS0Defects; ++m) {
      uint64_t flagged = 0;
      for (uint32_t cd : {0u, 1u, 2u}) flagged += run(m, cd);
      check(flagged > 0u, std::string("F: the per-vblank check flags a source 0 with ") + kS0Name[m]);
    }
    check(run(kS0Real, 0u, 1, false) == 1u, "F: the per-vblank check flags live registers that differ from the host counter");
    // Astra round 1, P2 (the release post-check races the CP's next gate), deterministically:
    const uint64_t gen0 = V::scratch1Writes();
    const uint64_t raced = run(kS0Real, 1u, 0, true, 1);
    check(raced == 0u && rd(kPc + 4u) == 1u && V::scratch1Writes() == gen0 + 2u && V::scratch1InFlight() == 0u,
          "F: a correct release whose pc+4 the CP has already rearmed (SCRATCH_REG1 write-back, a real Pm4Engine) "
          "before the check: not flagged");
    check(run(kS0Real, 1u, 1, true, 1) == 0u, "F: ... the same in a toggled roll (live registers as the host counter)");
    check(run(kS0Real, 1u, 0, true, 2) == 1u,
          "F: a released pc+4 set back to 1 by anything but the CP's write-back: flagged (the release undone)");
    // native/dreg-hang (fix1's traceBad=3 at 1000 ticks/s): a non-release tick that overlaps the CP's rearm (pc+4
    // 0 -> 1 through the production writeReg) at each point of the write-back's bracket. Astra round 1's single count
    // before the store flags (a) and (c): the tick reads the new generation with the old pc+4.
    {
      auto overlap = [&cp](void (*open)(), void (*close)(), bool openFirst, bool closeAfter) {
        V::test::force(1, 0, 1);
        seedScript(10u, 5u, 0u, 0u);
        wr(kPc + 4u, 0u);  // the gate open (released): the CP's next GATE rearms it
        V::test::reset();
        g_onScratchMap = open;
        g_onScratchStored = close;
        if (openFirst) raceOpen();
        cp.rearm();
        if (closeAfter) raceClose();
        g_onScratchMap = g_onScratchStored = nullptr;
        return std::array<uint64_t, 3>{V::test::counters().traceBad, rd(kPc + 4u), rd(kDev + 0x3B14u)};
      };
      const auto a = overlap(raceOpen, nullptr, false, true);   // opens between Begin and the store, closes after End
      const auto b = overlap(nullptr, raceClose, true, false);  // opens before Begin, closes between the store and End
      const auto c = overlap(raceOpen, raceClose, false, false);  // opens and closes inside the bracket
      const auto ok = [](const std::array<uint64_t, 3>& r) { return r[0] == 0u && r[1] == 1u && r[2] == 11u; };
      check(ok(a), "F: a tick opened between the CP's count and its pc+4 store (closed after): not flagged");
      check(ok(b), "F: a tick closed between the CP's pc+4 store and its end count (opened before): not flagged");
      check(ok(c), "F: a tick wholly inside the CP's write-back bracket: not flagged");
      check(V::scratch1InFlight() == 0u, "F: the bracket closes (no write-back left in flight)");

      // Astra round 2, P2 (1): the bracket explains only its own value at its own address. Negative controls inside
      // and across an open bracket (the first is Astra's schedule; round 1's in-flight excuse passed all three):
      const auto neg = overlap(nullptr, raceOpenClearClose, false, false);  // CP stored 1, End pending: cleared to 0
      check(neg[0] == 1u && neg[1] == 0u,
            "F: inside an open bracket (the CP's 1 stored, End pending) a foreign clear of pc+4 to 0: flagged");
      auto across = [&cp](uint32_t foreign) {  // a tick across the whole write-back, a foreign value landing after it
        V::test::force(1, 0, 1);
        seedScript(10u, 5u, 0u, 0u);
        wr(kPc + 4u, 0u);
        V::test::reset();
        raceOpen();
        cp.rearm();
        wr(kPc + 4u, foreign);
        raceClose();
        return V::test::counters().traceBad;
      };
      check(across(7u) == 1u, "F: a write-back of 1 during the tick, then a foreign 7: flagged (not its value)");
      {
        // A bracket whose target is another address (pc+0x44, value 1) while a foreign write sets pc+4 = 1 inside it.
        V::test::force(1, 0, 1);
        seedScript(10u, 5u, 0u, 0u);
        wr(kPc + 4u, 0u);
        V::test::reset();
        raceOpen();
        V::noteScratch1Begin(kPc + 0x44u, 1u);
        wr(kPc + 4u, 1u);
        V::noteScratch1End();
        raceClose();
        check(V::test::counters().traceBad == 1u,
              "F: a write-back to another address cannot explain pc+4's change, even to the same value: flagged");
      }

      // Astra round 2, P2 (2): the executor's deferred perform (Pm4Engine::execPerformStores, SR_NATIVE_EXEC) - the
      // same three interleavings and the negative control, at the bracket's own points (test::setBracketHooks:
      // after Begin, before End), on a real store item {SCRATCH_REG1, pc+4, 1}.
      auto perform = [&cp](void (*afterBegin)(), void (*beforeEnd)(), bool openFirst, bool closeAfter) {
        V::test::force(1, 0, 1);
        seedScript(10u, 5u, 0u, 0u);
        wr(kPc + 4u, 0u);
        V::test::reset();
        g_raceOpens = g_raceCloses = 0;
        auto item = std::make_unique<sr::gfx::GuestDrawCommand>();
        item->effectKind = sr::native::exec::kStores;
        item->effectN = 1;
        item->drawParams[0] = sr::native::exec::kOpScratch + 1u;
        item->drawParams[1] = kPc + 4u;
        item->drawParams[2] = 1u;
        V::test::setBracketHooks(afterBegin, beforeEnd);
        if (openFirst) raceOpen();
        cp.engine->execPerformStores(*item);
        V::test::setBracketHooks(nullptr, nullptr);
        if (closeAfter) raceClose();
        const bool tidy = !g_raceTick && V::scratch1InFlight() == 0u;
        raceClose();  // a hook that never fired leaves the tick open
        return std::array<uint64_t, 5>{V::test::counters().traceBad, rd(kPc + 4u), rd(kDev + 0x3B14u),
                                       uint64_t(g_raceOpens * 10 + g_raceCloses), uint64_t(tidy)};
      };
      const auto ea = perform(raceOpen, nullptr, false, true);
      const auto eb = perform(nullptr, raceClose, true, false);
      const auto ec = perform(raceOpen, raceClose, false, false);
      const auto en = perform(nullptr, raceOpenClearClose, false, false);
      const auto eok = [](const std::array<uint64_t, 5>& r) {
        return r[0] == 0u && r[1] == 1u && r[2] == 11u && r[3] == 11u && r[4] == 1u;
      };
      check(eok(ea), "F: executor perform - a tick opened after its Begin, before its store (closed after): not flagged");
      check(eok(eb), "F: executor perform - a tick closed after its store, before its End (opened before): not flagged");
      check(eok(ec), "F: executor perform - a tick wholly inside its bracket: not flagged");
      check(en[0] == 1u && en[1] == 0u && en[3] == 11u && en[4] == 1u,
            "F: executor perform - inside its open bracket a foreign clear of pc+4 to 0: flagged");
    }
    check(run(kS0NoPcClear, 1u, 0, true, 0) == 1u,
          "F: a release without the clear, the CP never rearming (it holds): flagged");
    // Astra round 1, P2 (unintended clears), and non-release ticks with the CP's ownership accounted for:
    check(run(kS0EarlyClear, 2u) == 1u && run(kS0EarlyClear, 3u) == 1u,
          "F: a source 0 that counts 2 -> 1 (3 -> 2) right and clears pc+4 early: flagged");
    check(run(kS0Real, 0u, 0, true, 1, 0u) == 0u && run(kS0Real, 2u, 0, true, 1, 0u) == 0u,
          "F: a non-release tick during which the CP rearms an open gate (pc+4 0 -> 1 by write-back): not flagged");
    check(run(kS0Real, 0u, 0, true, 2, 0u) == 1u,
          "F: a non-release tick whose pc+4 changes without the CP's write-back: flagged");
  }

  // ---- R: the registration's eligibility, through the production tick delivery (Astra round 1, P1):
  // K::vdVblankDeliver - the vblank thread's body after the lease - on a GuestCaller thread of its own, as VD_ISR runs
  // it, for a registration that is not the D3D ISR and for the D3D ISR's (the positive control).
  {
    auto*& isrSlot = PPC_LOOKUP_FUNC(K::getGuestBase(), kIsrCb);
    PPCFunc* const savedIsr = isrSlot;
    isrSlot = recordIsr;
    uint8_t* runnerBase = nullptr;
    size_t runnerSize = 0;
    const K::ExCreateThreadRunnerFn savedRunner = K::getExCreateThreadRunner(&runnerBase, &runnerSize);
    const size_t guestSize = K::getGuestSize() ? K::getGuestSize() : size_t(0x100000000ull);
    K::registerExCreateThreadRunner(noopRunner, K::getGuestBase(), guestSize);  // GuestCaller's global base
    struct Out {
      bool ok = false, ownPcr = false;
      K::VdTickDelivery d;
      uint32_t count = 0, countdown = 0, last = 0, pc4 = 0;
      std::vector<std::array<uint32_t, 2>> isr;
      V::test::Counters c{};
    };
    // countdown 1 on the device: a D3D source 0 run against it would release (count + 1, pc+4 = 0, +0x3B18)
    auto tick = [guestSize](int st, int tg, int win, uint32_t cb, bool nativeReg) {
      Out o;
      seedScript(10u, 5u, 1u, 0u);
      wr(kPc + 4u, 1u);
      V::test::reset();
      V::test::force(st, tg, win);
      g_isrCalls.clear();
      std::thread th([&o, cb, nativeReg, guestSize] {
        K::setGuestMemory(K::getGuestBase(), guestSize);
        K::GuestCaller gc("VBLANK_TEST_GC");
        o.ok = gc.ok();
        if (!o.ok) return;
        o.ownPcr = K::kpcrEnabled() && gc.ctx().r13.u32 != 0;
        if (o.ownPcr) K::kpcrSetActiveCpu(gc.ctx().r13.u32, 2u);
        const sr::kernel::graphics_interrupt::Registration reg{cb, kDev, nativeReg};
        o.d = K::vdVblankDeliver(&gc, reg, o.ownPcr, false);
      });
      th.join();
      o.count = rd(kDev + 0x3B14u);
      o.countdown = rd(kDev + 0x3B1Cu);
      o.last = rd(kDev + 0x3B18u);
      o.pc4 = rd(kPc + 4u);
      o.isr = g_isrCalls;
      o.c = V::test::counters();
      return o;
    };
    auto untouched = [](const Out& o) {
      return o.ok && !o.d.refused && !o.d.native && o.d.okc && o.isr.size() == 1u && o.isr[0][0] == 0u &&
             o.isr[0][1] == kDev && o.count == 10u && o.countdown == 1u && o.last == 5u && o.pc4 == 1u &&
             o.c.native == 0u && o.c.traceBad == 0u;
    };
    const Out on = tick(1, 0, 1, kIsrCb, false), ab = tick(1, 1, 1, kIsrCb, false), off = tick(0, 0, 1, kIsrCb, false);
    check(untouched(on) && on.c.ticks == 1u && on.c.guest == 1u,
          "R: gate on, a registration that is not the D3D ISR: its own callback runs (r3 0, r4 its context), no host "
          "source 0, its context's D3D words untouched, the tick scoped but unchecked");
    check(untouched(ab) && ab.c.ticks == 1u, "R: ... the same in a toggled roll's ON window");
    check(untouched(off) && off.c.ticks == 0u, "R: ... and with the gate off (no tick scope)");
    const Out d3d = tick(1, 0, 1, D::kD3DIsr, true);
    const bool hostRan = d3d.d.native && d3d.d.okc && d3d.d.source0Completed && d3d.isr.empty() && d3d.count == 11u &&
                         d3d.countdown == 0u && d3d.last == 11u && d3d.pc4 == 0u && d3d.c.native == 1u &&
                         d3d.c.released == 1u && d3d.c.traceBad == 0u;
    check(d3d.ok && (d3d.ownPcr ? hostRan : d3d.d.refused),
          d3d.ownPcr ? "R: the D3D ISR's native registration: the host source 0 runs (count + 1, the release), checked "
                       "clean"
                     : "R: the D3D ISR's native registration without an owned KPCR: refused (the caller stops)");
    isrSlot = savedIsr;
    K::registerExCreateThreadRunner(savedRunner, runnerBase, runnerSize);
  }

  // ---- G: mutants (each must FAIL rows)
  {
    for (int m = 0; m < kDefects; ++m) {
      g_defect = m;
      const size_t bad = pacingTable(mutantCore, false);
      check(bad > 0u, std::string("G: the pacing table catches the mutant '") + kDefectName[m] + "' (" +
                          std::to_string(bad) + " rows)");
    }
    // The counter rule's mutants on the wrap walks (today's rule and the guest arithmetic as the truth: the answer V
    // and its line % on each of the 9 ticks around every point) and on a HIGHFPS tick.
    std::vector<uint32_t> points = wrapTicks(8u, 3u);
    for (uint32_t n : wrapTicks(99u, 2u)) points.push_back(n);
    points.push_back(5965233u);
    for (int m = 0; m < kCnDefects; ++m) {
      size_t bad = 0;
      for (uint32_t n : points) {
        uint32_t tv = uint32_t(uint64_t(720u) * (n - 5u)), mv = tv;
        for (int i = 0; i < 9; ++i) {
          tv = todayNext(tv, false);
          mv = mutantNext(m, mv, false);
          bad += tv != mv || V::linePercent(tv, 720u) != mutantLinePercent(m, mv);
        }
      }
      bad += mutantNext(m, 777u, true) != todayNext(777u, true);
      check(bad > 0u, std::string("G: the wrap walks catch the counter mutant '") + kCnName[m] + "' (" +
                          std::to_string(bad) + " ticks)");
    }
  }

  // ---- H: the dispreg breakdown
  {
    V::test::reset();
    const int cols = sr::native::meter::columnCount();
    int col = -1;
    for (int c = 0; c < cols; ++c)
      if (!std::strcmp(sr::native::meter::columnName(c), "dispreg")) col = c;
    std::vector<uint64_t> before(size_t(cols > 0 ? cols : 1)), after(before.size());
    if (cols > 0) sr::native::meter::snapshotColumns(before.data(), cols);
    const auto fn = K::ExportRegistry::instance().getExport("VdGetSystemCommandBuffer");
    if (fn) fn(kDesc, kDesc + 0x100u, 0u, 0u, 0u, 0u);
    if (cols > 0) sr::native::meter::snapshotColumns(after.data(), cols);
    const auto c = V::test::counters();
    check(fn && col >= 0 && c.sysbuf == 1u && c.scanout == 0u && c.vsyncBit == 0u &&
              after[size_t(col)] == before[size_t(col)] + 1u && rd(kDesc) == 0xBEEF0000u &&
              rd(kDesc + 0x100u) == 0xBEEF0001u,
          "H: VdGetSystemCommandBuffer feeds the meter's dispreg column and the breakdown's sysbuf (one per call)");
  }

  // ---- T: dreg2's stall class (native/dreg-hang). Live (SR_NATIVE_VBLANK=1, HIGHFPS 1000 Hz): ~46 vblank ticks/s,
  // each holding the CPU-2 lock ~21 ms (trySource0's VirtualQuery walks of the resident arena), back to back (the
  // clock re-bases a late tick and returns at once); the CP thread waited 0.9 s for it in a pacing call (ISR #2) and
  // 9.7 s in its census line ([NATIVE_VBLANK] printed 9.7 s after [NATIVE_TRANSPORT] in one census) - the GPU hang
  // trigger H1 at 17.3 s with rptr 114 / wptr 144.
  {
    using Clock = std::chrono::steady_clock;
    const auto us = [](Clock::duration d) {
      return double(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()) / 1000.0;
    };
    const size_t guestSize = arenaSize;
    uint8_t* const gb = K::getGuestBase();  // the guest base is per thread (TLS): each thread below binds this one
    // A production vblank tick, as vdVblankDeliver performs it for the native registration: the CPU-2 lock with the
    // host counter and the per-vblank check (TickScope) around the host source 0 in a CPU-2 DPC.
    auto tickOnce = [](bool highFps) {
      PPCContext ctx = isrContext(0u);
      uint32_t exc = 0xFFFFFFFFu;
      D::Source0Result r;
      {
        V::TickScope scope(kDev, highFps, true);
        K::KpcrDpcScope dpc(kPcr);
        K::ExportRegistry::setGuestCallContext(true);
        r = D::trySource0(ctx, K::getGuestBase(), D::kD3DIsr, kDev, &exc);
        K::ExportRegistry::setGuestCallContext(false);
      }
      return r == D::Source0Result::Completed && exc == 0u;
    };

    // T1: the census line never waits for the CPU-2 lock (deterministic): a tick in progress holds it on one thread,
    // the [NATIVE_VBLANK] line must come back on another before that tick ends.
    {
      V::test::force(1, 0, 1);
      V::test::reset();
      std::atomic<bool> held{false}, releaseTick{false}, censusDone{false};
      std::thread holder([&] {
        K::setGuestMemory(gb, guestSize);
        V::TickScope scope(0u, true, true);  // no device: the lock and the host counter only
        held = true;
        const auto until = Clock::now() + std::chrono::seconds(3);
        while (!releaseTick && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      });
      while (!held) std::this_thread::yield();
      std::thread census([&] {
        V::finalLine("vblank-test-T1");
        censusDone = true;
      });
      const auto t0 = Clock::now();
      while (!censusDone && Clock::now() - t0 < std::chrono::milliseconds(1500))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const bool doneWhileHeld = censusDone.load();
      releaseTick = true;
      holder.join();
      census.join();
      check(doneWhileHeld, "T1: the [NATIVE_VBLANK] census line returns while a vblank tick holds the CPU-2 lock (dreg2: "
                           "the CP's census sat 9.7 s behind the ticks)");
    }

    // T2: no OS permission query on a production tick (deterministic): countdown 0 / 1 (the release: the scratch cell
    // and pc+4 admitted too) / 2, the swap callback null as in this title.
    {
      V::test::force(1, 0, 1);
      bool ok = true;
      const uint64_t q0 = D::test::osQueries();
      for (uint32_t cd : {0u, 1u, 2u}) {
        seedScript(10u, 5u, cd, 0u);
        wr(kPc + 4u, 1u);
        V::test::reset();
        ok &= tickOnce(false);
        ok &= rd(kDev + 0x3B14u) == 11u && V::test::counters().traceBad == 0u;
      }
      const uint64_t q1 = D::test::osQueries();
      check(ok && q1 == q0, "T2: production ticks (countdown 0 / 1 / 2) complete clean with 0 OS permission queries (" +
                                std::to_string(q1 - q0) + " made; dreg2's trySource0 made five VirtualQuery walks a tick)");
    }

    // T3: the live mechanism under the live condition: the guest arena resident around the device, stack and KPCR
    // (VirtualQuery walks a region's resident pages: the transport's cost table measured 367 us a walk at 256 MB),
    // ticks back to back on a vblank thread, and the CP's pacing calls through the strong sub_82111E38 (each takes
    // the CPU-2 lock) on another. Bounds: the tick's mean cost, and the CP's longest wait for the lock.
    {
      uint8_t* from = K::guestVAToHost(kDev & ~0xFFFFu);
      uint8_t* to = K::guestVAToHost(0x7F000000u);  // ~233 MB below the MMIO page, made resident (values kept)
      std::printf("  T3: making %llu MB of the arena resident\n", (unsigned long long)((to - from) >> 20));
      for (uint8_t* p = from; p < to; p += 0x1000) {
        volatile uint8_t* b = p;
        *b = *b;
      }
      std::printf("  T3: resident; ticks back to back against the CP's pacing calls\n");
      V::test::force(1, 0, 1);
      seedScript(1000u, 999u, 0u, 0u);
      wr(kPc + 4u, 1u);
      V::test::reset();
      std::atomic<bool> stop{false}, cpDone{false}, tickBad{false};
      std::atomic<uint64_t> ticks{0}, tickNs{0};
      std::thread vblank([&] {
        K::setGuestMemory(gb, guestSize);
        while (!stop) {
          const auto t0 = Clock::now();
          if (!tickOnce(true)) tickBad = true;
          tickNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
          ++ticks;
        }
      });
      std::vector<double> waits;
      std::thread cp([&] {
        K::setGuestMemory(gb, guestSize);
        for (int i = 0; i < 150; ++i) {
          PPCContext ctx = isrContext(0u);
          ctx.r3.u64 = (1u << 16) | 8u;  // interval 1, pct 8: gameplay's gate
          const auto t0 = Clock::now();
          sub_82111E38(ctx, K::getGuestBase());
          waits.push_back(us(Clock::now() - t0));
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        cpDone = true;
      });
      const auto start = Clock::now();
      while (!cpDone && Clock::now() - start < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      stop = true;  // a starved CP gets the lock once the ticks stop
      vblank.join();
      cp.join();
      const double meanTickUs = ticks ? double(tickNs) / 1000.0 / double(ticks) : 0.0;
      const double maxWaitUs = waits.empty() ? 0.0 : *std::max_element(waits.begin(), waits.end());
      const auto c = V::test::counters();
      std::printf("  T3: %llu ticks back to back, mean %.1f us a tick; the CP's 150 pacing calls: longest %.1f us "
                  "(witness: tickMaxUs %llu tickSlow %llu waitMaxUs %llu waitSlow %llu)\n",
                  (unsigned long long)ticks.load(), meanTickUs, maxWaitUs,
                  (unsigned long long)(c.tickHoldMaxNs / 1000u), (unsigned long long)c.tickSlow,
                  (unsigned long long)(c.pacingWaitMaxNs / 1000u), (unsigned long long)c.pacingWaitSlow);
      check(!tickBad && ticks > 0u && c.traceBad == 0u && c.mismatch == 0u && c.calls == 150u,
            "T3: ticks and pacing calls interleaved under the lock: every tick completed, the per-vblank check and "
            "the shadow clean");
      check(meanTickUs < 250.0, "T3: a vblank tick costs < 250 us on a resident arena (" + std::to_string(meanTickUs) +
                                    " us; dreg2 ~21,000)");
      check(maxWaitUs < 100000.0, "T3: the CP's pacing call never waits 100 ms for the CPU-2 lock (" +
                                      std::to_string(maxWaitUs) + " us; dreg2 0.9 s, then 3-33 s)");
      check(c.tickHoldMaxNs > 0u && c.pacingWaitMaxNs > 0u && double(c.pacingWaitMaxNs) / 1000.0 <= maxWaitUs + 1.0,
            "T3: the [NATIVE_VBLANK] cpu2 witness is fed (a tick's hold, a pacing call's wait within the measured "
            "call)");
    }
  }

  V::test::force(-1, 0, 1);
  V::test::reset();
  K::registerExCreateThreadRunner(arenaRunner0, arenaBase0, arenaSize0);
  slot = savedSlot;
  wr(kCell, savedCell);
  wr(kVCounter, savedVc);
  wr(kViewport, savedVp);
  wr(kEnable, savedEn);
  std::printf("[NATIVE_VBLANK_TEST] %d passed, %d failed (offline host-vs-guest differential; no live pacing)\n",
              passed, failed);
  return failed ? 1 : 0;
}
