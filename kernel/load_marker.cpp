// step 4 ruling 12: [LOAD_BEGIN] / [LOAD_DONE] / [LOAD_OFF] - the load screen's begin and end in every log profile.
// Contract in load_marker.hpp. Three observe overrides of the game's own load-screen functions (IDA, 2026-10-20).
#include "guest/ppc/ppc_recomp_shared.h"
#include "load_marker.hpp"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

extern "C" void __imp__sub_8214F1B0(PPCContext& __restrict ctx, uint8_t* base);   // FmuiEnterLoadingScreen
extern "C" void __imp__sub_8214E788(PPCContext& __restrict ctx, uint8_t* base);   // FmuiHideLoadingScreenWorker
extern "C" void __imp__sub_82317728(PPCContext& __restrict ctx, uint8_t* base);   // LoadScreenRenderObj vtable+0x24

namespace sr::kernel::loadmark {

namespace {

// The latest [LOAD_BEGIN]: its count and its time, published TOGETHER under g_beginMu (Astra P2 #1, 2026-10-20: as
// two atomics a [LOAD_DONE] / [LOAD_OFF] could name begin N with begin N-1's time, and concurrent begins could
// publish their times out of count order). The lock covers only the counter and the clock read - never a log call.
std::mutex g_beginMu;
BeginSnapshot g_beginSnap{0, 0};
std::atomic<uint32_t> g_done{0}, g_off{0};

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The backstop cap of each tag (the player profile keeps milestone tags whole): the first 512, then powers of two.
// Count 0 (a counter that wrapped past 2^32 events) is capped too (Astra P2 #4).
bool capped(uint32_t n) { return n == 0 || (n > 512 && (n & (n - 1)) != 0); }

double secondsSince(const BeginSnapshot& b) { return b.ns ? (nowNs() - b.ns) / 1e9 : -1.0; }

}  // namespace

const char* modeName(uint32_t mode) {
  switch (mode) {
    case 0: return "SHIELD";
    case 1: return "BIZARRO";
    case 2: return "ENGAGING";
    default: return "?";
  }
}

bool logged(uint32_t n) { return !capped(n); }

BeginSnapshot beginSnapshot() {
  std::lock_guard<std::mutex> lock(g_beginMu);
  return g_beginSnap;
}

uint32_t noteBegin(uint32_t mode, uint32_t lr) {
  BeginSnapshot b;
  {
    std::lock_guard<std::mutex> lock(g_beginMu);
    b = BeginSnapshot{g_beginSnap.id + 1, nowNs()};
    g_beginSnap = b;
  }
  if (logged(b.id)) SR_LOG_BOOT(sr::LogSeverity::Info, "[LOAD_BEGIN] #%u mode=%s lr=%08X", b.id, modeName(mode), lr);
  return b.id;
}

uint32_t noteDone(uint32_t lr) {
  const uint32_t n = g_done.fetch_add(1, std::memory_order_relaxed) + 1;
  const BeginSnapshot b = beginSnapshot();   // the id and the time of ONE [LOAD_BEGIN]
  if (logged(n))
    SR_LOG_BOOT(sr::LogSeverity::Info, "[LOAD_DONE] #%u hidden %.1f s after [LOAD_BEGIN] #%u lr=%08X", n,
                secondsSince(b), b.id, lr);
  return n;
}

uint32_t noteOff(uint32_t lr) {
  const uint32_t n = g_off.fetch_add(1, std::memory_order_relaxed) + 1;
  const BeginSnapshot b = beginSnapshot();
  if (logged(n))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[LOAD_OFF] #%u screen off %.1f s after [LOAD_BEGIN] #%u lr=%08X (the pause / START gate opens)", n,
                secondsSince(b), b.id, lr);
  return n;
}

// The transitions (each flag read before AND after the game's own call): only a change writes a line.
bool onShown(uint8_t before, uint8_t after, uint32_t mode, uint32_t lr) {
  if (before != 0 || after == 0) return false;
  noteBegin(mode, lr);
  return true;
}

bool onHidden(uint8_t before, uint8_t after, uint32_t lr) {
  if (before == 0 || after != 0) return false;
  noteDone(lr);
  return true;
}

bool onScreenOff(uint8_t before, uint8_t after, uint32_t lr) {
  if (before == 0 || after != 0) return false;
  noteOff(lr);
  return true;
}

void resetForTest() { setCountsForTest(0, 0, 0); }

void setCountsForTest(uint32_t begin, uint32_t done, uint32_t off) {
  {
    std::lock_guard<std::mutex> lock(g_beginMu);
    g_beginSnap = BeginSnapshot{begin, 0};
  }
  g_done.store(done);
  g_off.store(off);
}

}  // namespace sr::kernel::loadmark

namespace {

uint32_t rd32(uint32_t va) { return (va >= 0x10000u && va < 0xC0000000u) ? sr::kernel::readGuestU32BE(va) : 0u; }
uint8_t rd8(uint8_t* base, uint32_t va) { return (va >= 0x10000u && va < 0xC0000000u) ? base[va] : 0u; }

}  // namespace

// The holder (*(ui+0x54)) is read ONCE, before the game's call, and only its shown byte is read again after it - a
// documented assumption (Astra P2 #3, 2026-10-20): the call does not replace *(ui+0x54) or retire the holder.
//   - IDA: FmuiEnterLoadingScreen allocates the LoadScreenRenderObj at holder+0x14 (the holder itself stays), and
//     FmuiHideLoadingScreenWorker clears the shown byte and dismisses that object; neither body writes ui+0x54.
//     Indirect callbacks inside them are not proven not to; this is not a proof.
//   - The witness (2026-10-20 recovery run 1, step4/load-markers 438162b4, release exe 8b403d4af6d0): [LOAD_BEGIN] #1
//     was logged at the first (boot, ENGAGING) load, so the holder is already non-null at the first Show, and every
//     BEGIN of the recovery runs had its DONE and OFF.
// If the assumption ever failed, the after-read would be of the old holder: a missed or a spurious marker line, never
// a write the guest sees (rd8 reads a guest address in the numeric range only; the holder is the game's own heap).

// FmuiEnterLoadingScreen_sub_8214F1B0(ui, mode): the holder's shown byte (*(ui+0x54)+0x18) 0 -> non-zero = BEGIN.
PPC_FUNC(sub_8214F1B0) {
  const uint32_t ui = ctx.r3.u32, mode = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t holder = rd32(ui + 0x54u);
  const uint8_t before = holder ? rd8(base, holder + 0x18u) : 0;
  __imp__sub_8214F1B0(ctx, base);
  if (holder) sr::kernel::loadmark::onShown(before, rd8(base, holder + 0x18u), mode, lr);
}

// FmuiHideLoadingScreenWorker_sub_8214E788(ui): the shown byte non-zero -> 0 = DONE (with a load-screen object the
// worker also sets the sequencer's state 7: LoadScreenSetDismissState(obj, 0)).
PPC_FUNC(sub_8214E788) {
  const uint32_t ui = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t holder = rd32(ui + 0x54u);
  const uint8_t before = holder ? rd8(base, holder + 0x18u) : 0;
  __imp__sub_8214E788(ctx, base);
  if (holder) sr::kernel::loadmark::onHidden(before, rd8(base, holder + 0x18u), lr);
}

// LoadScreenRenderObj vtable+0x24 (sub_82317728): the active flag +0x44 (FrontendUpdateGate's term) set -> clear =
// OFF. The object (r3) is the game's own; this method does not free it (it clears +0x44, empties a vector, stops a
// stopwatch and calls the base class's method).
PPC_FUNC(sub_82317728) {
  const uint32_t obj = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint8_t before = rd8(base, obj + 0x44u);
  __imp__sub_82317728(ctx, base);
  sr::kernel::loadmark::onScreenOff(before, rd8(base, obj + 0x44u), lr);
}
