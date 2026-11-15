// iter-358: NATURAL GAUGES — see natural_gauges.hpp for why this exists.
//
// This file owns:
//   1. The uncapped atomic counters + the 10s [GAUGE] census emitter.
//   2. Pass-through strong-defs for the two natural signals nobody observed:
//      sub_821494C0 (FE master tick) and sub_826D2718 (Apt executor pump).
//
// Both strong-defs are PURE OBSERVERS: bump a counter, delegate to __imp__.
// They make no decision for the guest and are not gated — measuring a natural
// signal is not forcing. (Charter: platform/observation vs. forcing.)
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "natural_gauges.hpp"
#include "devtools.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <thread>
#include <chrono>

namespace sr { namespace kernel {

static std::atomic<uint32_t> s_gauges[static_cast<size_t>(Gauge::Count)];

static const char* gaugeName(Gauge g) {
  switch (g) {
    case Gauge::FeTick:       return "feTick";
    case Gauge::AptPump:      return "aptPump";
    case Gauge::ClipCtorAll:  return "clipCtorAll";
    case Gauge::ClipCtorReal: return "clipCtorReal";
    case Gauge::DefExec:      return "defExec";
    case Gauge::AptReq:       return "aptReq";
    default:                  return "?";
  }
}

uint32_t gaugeGet(Gauge g) {
  return s_gauges[static_cast<size_t>(g)].load(std::memory_order_relaxed);
}

void gaugeCensus(const char* why) {
  // ONE authoritative line. The K-run aggregate parses the LAST of these —
  // never log-line counts, which the verbose caps would silently truncate.
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[GAUGE] %s | feTick=%u aptPump=%u clipCtorAll=%u clipCtorReal=%u "
    "defExec=%u aptReq=%u devtools=0x%X devtoolActs=%llu",
    why ? why : "tick",
    gaugeGet(Gauge::FeTick), gaugeGet(Gauge::AptPump),
    gaugeGet(Gauge::ClipCtorAll), gaugeGet(Gauge::ClipCtorReal),
    gaugeGet(Gauge::DefExec), gaugeGet(Gauge::AptReq),
    // iter-463 phase 2 (DEVTOOLS_DESIGN 3.1 A7): the dev-tool tag rides EVERY census line, so
    // it cannot be lost to capped or early logging. 0x0 / 0 on a natural run.
    devToolMask(), static_cast<unsigned long long>(devToolActs()));
}

// Wall-clock census thread. Started lazily on the first bump so a run that
// never reaches guest code doesn't spawn it. Detached + never joined: the
// process exits when the run ends; the census is diagnostic-only.
// (Project rule: clock off a dedicated wall-clock thread, never off pump ticks —
// the pump stalls for minutes once managers exist.)
static void ensureCensusThread() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true, std::memory_order_acq_rel)) return;
  std::thread([]() {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      gaugeCensus("10s");
      // iter-463 phase 2: the level-skip host check while no tick runs (an FMV, a modal popup):
      // the deadline and the idle-pad path (fault-free guest reads only); a no-op unless
      // levelskip is on.
      devToolsPoll();
    }
  }).detach();
}

void gaugeBump(Gauge g) {
  s_gauges[static_cast<size_t>(g)].fetch_add(1, std::memory_order_relaxed);
  ensureCensusThread();
}

} }  // namespace sr::kernel

// ---------------------------------------------------------------------------
// Natural observers
// ---------------------------------------------------------------------------

// NOTE: the Apt executor pump sub_826D2718 is NOT hooked here — apt_parse_trace.cpp
// already strong-defs it ([PARSE_TRACE]) and now bumps Gauge::AptPump from there.
// (Project rule: grep kernel/ for an existing hook before adding a strong-def;
// two strong-defs of the same guest function is a duplicate-symbol link error.)
extern "C" void __imp__sub_821494C0(PPCContext& __restrict ctx, uint8_t* base);

// sub_821494C0 = the FE master tick (iter-295e: "THE NATURAL FE TICK (the
// trunk)"). splash_hooks.cpp's SR_DRIVE_FE_TICK driver hand-calls __imp__
// directly, so it BYPASSES this observer — meaning this counter reports
// natural ticks even when the forcing driver is armed. That separation is the
// whole point: feFrames used to count the driver's own log lines.
void sub_821494C0(PPCContext& __restrict ctx, uint8_t* base) {
  sr::kernel::gaugeBump(sr::kernel::Gauge::FeTick);
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 4 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[NAT_FE] #%u natural FE master tick sub_821494C0 this=0x%08X lr=0x%08X",
      n, ctx.r3.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_821494C0(ctx, base);
}

// Chain-address footnote (cost an hour of link errors, so it is written down):
// iter-357f's watchpoint chain reported the pump as "0x826d27c8". That is a
// RETURN address 176 bytes INTO AptExecutorPump_sub_826D2718 (body
// 826d2718-826d2927) — not a function entry, so there is no __imp__ symbol for
// it and hooking it fails to link. Chain addresses from a backtrace name a
// call SITE; you hook the containing function. Same for the rest of that chain:
// 0x826d26cc is inside sub_826D2690 (load-by-name request, hooked in
// fe_flow_watch.cpp) and 0x821496fc is inside sub_821494C0 (the FE tick above).
// Resolve with mcp__ghidra__get_function_by_address or va_to_source first.
