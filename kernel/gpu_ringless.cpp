// iter-467 native-a P2-A: the host IB queue (see gpu_ringless.hpp). Host code written from the asm of
// GpuRingUpdateWritePointer_820FD218 and GpuRingEmitBinSelectLo_82110140 (IDA, 2026-10-09).

#include "gpu_ringless.hpp"
#include "sr_oracle.hpp"

#include <atomic>
#include <cstdlib>

#include "guest/ppc/ppc_recomp_shared.h"
#include "ab_toggle.hpp"
#include "native_brackets.hpp"
#include "gpu_submit.hpp"
#include "guest_context.hpp"
#include "p2_trace.hpp"
#include "native_flush.hpp"  // P2-B: segmentPublishId (the close's hand-off / the bracket replay's lookup)
#include "quit.hpp"          // tools/graceful-quit: no item is queued once the ring is cut
#include "../app/log.hpp"

#if SR_ORACLE
extern "C" void __imp__sub_820FD218(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82110140(PPCContext& __restrict ctx, uint8_t* base);
namespace sr::kernel { void gpuRingLegacyDoorbell(); }
#endif

namespace sr::kernel::ringless {

namespace {

constexpr uint32_t kVdGlobalDeviceCell = 0x82000948u;
constexpr uint32_t kDevWptr = 0x2A44u;      // ring write index (dwords) = dev+10820
constexpr uint32_t kDevRingBase = 0x34A4u;  // = dev+13476
constexpr uint32_t kDevRingMask = 0x34A8u;  // = dev+13480
constexpr uint32_t kDevModeDword = 0x2A38u; // byte +0x2A39 bit 1 = no-GPU (PIX) mode
constexpr uint32_t kDevPixObject = 0x4DF4u, kDevPixCallback = 0x4E84u;

// Default ON since the P2-A proof (2026-10-09, p2b_*: per-present ring/queue A/B pixel-exact 12/12, desync 0,
// gate PASS, speed neutral); =0 keeps the ring path for the A/B. SR_NATIVE_SEGMENTLESS (P2-B, default ON since
// the 2026-10-13 re-bake) implies it: the ring path needs SR_NATIVE_SEGMENTLESS=0 as well.
bool ringlessGate() {  // the gate as a zero-arg helper (the override manifest generator follows it)
#if SR_ORACLE
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_RINGLESS");
    const char* s = std::getenv("SR_NATIVE_SEGMENTLESS");
    return (!e || e[0] != '0') || (!s || s[0] != '0');
  }();
  return on;
#else
  return true;
#endif
}
const bool g_gate = ringlessGate();

std::atomic<uint64_t> g_ib{0}, g_bs{0}, g_bsSame{0}, g_guestMode{0}, g_notTaken{0};

uint32_t rd(uint32_t va) { return sr::kernel::readGuestU32BE(va); }
uint32_t deviceVA() { return rd(rd(kVdGlobalDeviceCell)); }

bool takeNow() {
#if SR_ORACLE
  if (!g_gate) return false;
  // The in-roll A/B of ringless needs segmentless off: an OFF window under segmentless would publish its markerless
  // records through ring memory (Astra, 2026-10-13). With segmentless on, the toggle is ignored - once, loudly.
  static const int s_ab = [] {
    const int ab = sr::abt::reg("SR_NATIVE_RINGLESS");
    const char* s = std::getenv("SR_NATIVE_SEGMENTLESS");
    if (ab >= 0 && (!s || s[0] != '0')) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[RINGLESS] SR_AB_TOGGLE=SR_NATIVE_RINGLESS ignored: segmentless is on and its records have no ring "
                  "form - A/B ringless with SR_NATIVE_SEGMENTLESS=0");
      return -1;
    }
    return ab;
  }();
  if (s_ab >= 0 && !sr::abt::on(s_ab)) {
    g_notTaken.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!sr::kernel::gpuCpActive()) {  // the legacy synchronous drain parses ring memory: no queue there
    static std::atomic<bool> s_once{false};
    if (!s_once.exchange(true))
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[RINGLESS] the async CP is off (SR_PM4_CP): the ring path stays");
    return false;
  }
#else
  if (!sr::kernel::gpuCpActive()) return false;
#endif
  return true;
}

#if !SR_ORACLE
[[noreturn]] void submissionRefused(const char* reason, uint32_t a, uint32_t b) {
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[NATIVE_FLUSH_BAD] native submission refused %s (%08X, %u); no guest ring fallback",
              reason, a, b);
  sr::Log::shutdown();
  std::_Exit(3);
}
#endif

// Whole-body guest modes: no-GPU (byte dev+0x2A39 & 2) and PIX attached (dev+0x4DF4 / +0x4E84).
bool guestMode(uint32_t dev) {
  if ((rd(dev + kDevModeDword) >> 16) & 2u) return true;
  return rd(dev + kDevPixObject) != 0 || rd(dev + kDevPixCallback) != 0;
}

}  // namespace

bool gateOn() { return g_gate; }

// GpuRingUpdateWritePointer_820FD218(dev r3, desc r4 = {dwords, phys}).
bool updateWritePointer(PPCContext& ctx, uint8_t* base) {
  if (!takeNow()) return false;
  const uint32_t dev = ctx.r3.u32, desc = ctx.r4.u32;
  if (!dev || !desc || guestMode(dev)) {
    g_guestMode.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t dwords = rd(desc), phys = rd(desc + 4u);
  const uint32_t mask = rd(dev + kDevRingMask), ringBase = rd(dev + kDevRingBase);
  const uint32_t wptr = rd(dev + kDevWptr);
  if (!ringBase || !mask || (mask & (mask + 1u)) != 0u) {
    g_guestMode.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // tools/graceful-quit (kernel/quit.hpp): once the ring is cut nothing is queued for the CP either - the item is
  // never pushed and the doorbell's cut rule applies here, first (park, or a lock holder before the thread cut runs
  // on). Without it a producer after the cut queued items the drained CP never takes (smoke1: rl_pending 0->2).
  if (sr::kernel::quit::g_ringCut.load(std::memory_order_relaxed)) {
    // native/quit-drain round 2: the fence this dropped IB carries (a PM4 segment close's tail; none otherwise), the
    // only kind of fence a STRANDED wait may target - recorded before the cut rule, which may park this thread.
    // Round 3 (Astra round 2 P1): read from a PLAIN IB's own bytes only. A native segmentless publication's guest bytes
    // are not its packets (record mode leaves them untouched: a reused segment can still end in an older close's
    // EVENT_WRITE_SHD ... F) - the close's publishSegment records the S it carries; a pump replay of a deferred entry or
    // a hand-off pending for another block records nothing (fail closed: a waiter on it stays FAILED).
#if SR_ORACLE
    if (sr::native::flush::cutPublicationKind(phys, dwords) == 0) sr::kernel::quit::recordDroppedIb(dev, phys, dwords);
#endif
    sr::kernel::quit::doorbellCut();
    ctx.r3.u64 = 0;
    return true;
  }
  // The XDK's own ring-space wait: GpuRingWaitReadPtrWrap(dev, wptr, 3) spins on the read index
  // fb+0x3C, which the CP writes as it takes items (the same index arithmetic as the ring).
  ctx.r3.u64 = dev;
  ctx.r4.u64 = wptr;
  ctx.r5.u64 = 3u;
  sub_820FD150(ctx, base);
  // P2-B: the segment the segmentless close is publishing through this very call (handed over on this
  // thread), or a bracket segment the pump is replaying, goes as its part list (kind 3, by id); every
  // other block - whoever wrote it - is the plain INDIRECT_BUFFER it is.
  if (const uint64_t sid = sr::native::flush::segmentPublishId(phys, dwords))
    sr::kernel::gpuCpPushSegment(wptr, phys, dwords, sid);
  else
#if SR_ORACLE
    sr::kernel::gpuCpPushItem(1u, wptr, phys, dwords);
#else
    submissionRefused("an unconverted indirect buffer", phys, dwords);
#endif
  const uint32_t w1 = (wptr + 3u) & mask;
  sr::kernel::writeGuestBE32(dev + kDevWptr, w1);
  if (sr::native::p2trace::g_on) {
    sr::native::p2trace::markRingless();
    sr::native::p2trace::event(sr::native::p2trace::kIb, 0, wptr, phys, dwords);
  }
  sr::kernel::gpuCpDoorbell(ringBase, mask, w1);  // the publish, in place of the CP_RB_WPTR store
  g_ib.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = 0;  // the guest body leaves r3 = dev+0x4DF4 (0 without PIX)
  return true;
}

uint64_t itemsTotal() { return g_ib.load(std::memory_order_relaxed) + g_bs.load(std::memory_order_relaxed); }

void finalLine(const char* why) {
  if (!g_gate) return;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[RINGLESS] %s ib=%llu bs=%llu bsSame=%llu guestMode=%llu abOff=%llu", why,
              (unsigned long long)g_ib.load(), (unsigned long long)g_bs.load(), (unsigned long long)g_bsSame.load(),
              (unsigned long long)g_guestMode.load(), (unsigned long long)g_notTaken.load());
}

}  // namespace sr::kernel::ringless

// The native producer owns this entry in both configurations. Only the Oracle
// build retains the guest body and its legacy doorbell consumer.
PPC_FUNC(sub_820FD218) {
  SR_NATIVE_SCOPE(0x820FD218);
  if (sr::kernel::ringless::updateWritePointer(ctx, base)) return;
#if SR_ORACLE
  __imp__sub_820FD218(ctx, base);
  sr::kernel::gpuRingLegacyDoorbell();
#else
  sr::kernel::ringless::submissionRefused("820FD218 admission", ctx.r3.u32, ctx.r4.u32);
#endif
}

// GpuRingEmitBinSelectLo_sub_82110140(S r3): the pump thread's bin select. Under the gate the body is
// host code and the packet becomes a BS item; otherwise the guest body runs (once) and the P2 trace
// reads what it wrote.
PPC_FUNC(sub_82110140) {
  using namespace sr::kernel::ringless;
  namespace tr = sr::native::p2trace;
  const uint32_t S = ctx.r3.u32;
  const bool trace = tr::g_on;
#if !SR_ORACLE
  const uint32_t nativeDev = deviceVA();
  if (!S || !nativeDev)
    submissionRefused("82110140 null state/device", S, nativeDev);
  const uint32_t nativeMask = rd(nativeDev + kDevRingMask);
  if (!rd(nativeDev + kDevRingBase) || !nativeMask || (nativeMask & (nativeMask + 1u)) != 0u)
    submissionRefused("82110140 queue geometry", nativeDev, nativeMask);
#endif
  if (!ringlessGate() || !takeNow() || guestMode(deviceVA())) {
#if SR_ORACLE
    const uint32_t dev = trace ? deviceVA() : 0u;
    const uint32_t w0 = trace ? rd(dev + kDevWptr) : 0u;
    __imp__sub_82110140(ctx, base);  // exactly once
    if (!trace) return;
    const uint32_t ringBase = rd(dev + kDevRingBase), mask = rd(dev + kDevRingMask);
    const uint32_t w1 = rd(dev + kDevWptr);
    uint32_t value = 0xFFFFFFFFu;
    if (ringBase && mask && ((w1 - w0) & mask) == 2u) value = rd(ringBase + ((w0 + 1u) & mask) * 4u);
    if (w1 != w0) tr::event(tr::kBs, 0, w0, value, S);
    return;
#else
    submissionRefused("82110140 admission", S, deviceVA());
#endif
  }
  const uint32_t flags = rd(S + 0x28u);
  uint32_t value = (flags & 0x80000000u) ? 0x2AAAAAAAu : (flags & 0x40000000u) ? 0x15555555u : 0xFFFFFFFFu;
  if (flags & 0x20000000u) {  // the tile loop (never active on this title)
    const uint32_t tile = rd(S + 0x2Cu);
    value &= 3u << (2u * tile);
    if (tile == 0u && !(flags & 0x80000000u)) value |= 0x80000000u;
  }
  if (value == rd(S + 0x16Cu)) {  // the cache: nothing written
    g_bsSame.fetch_add(1, std::memory_order_relaxed);
    return;                         // r3 = S, as the guest leaves it
  }
  if (sr::kernel::quit::g_ringCut.load(std::memory_order_relaxed)) {  // tools/graceful-quit: as updateWritePointer
    sr::kernel::quit::doorbellCut();
    ctx.r3.u64 = 0;
    return;
  }
  sr::kernel::writeGuestBE32(S + 0x16Cu, value);
  const uint32_t dev = deviceVA();
  const uint32_t mask = rd(dev + kDevRingMask), wptr = rd(dev + kDevWptr);
  ctx.r3.u64 = dev;
  ctx.r4.u64 = wptr;
  ctx.r5.u64 = 2u;
  sub_820FD150(ctx, base);  // GpuRingWaitReadPtrWrap(dev, wptr, 2)
  sr::kernel::gpuCpPushItem(2u, wptr, value, 0u);
  sr::kernel::writeGuestBE32(dev + kDevWptr, (wptr + 2u) & mask);  // no doorbell, as the XDK
  if (trace) tr::event(tr::kBs, 0, wptr, value, S);
  g_bs.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = 0;  // the guest body leaves r3 = dev+0x4DF4 (0 without PIX)
}
