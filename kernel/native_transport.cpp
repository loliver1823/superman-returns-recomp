// Native submission/park pair from retail 820FD800 / 820FCA80 / 82110768.
// SDK software-list metadata is not a hardware command stream. The remaining
// pump and underlying publishing dependencies are separate cutover obligations.
// SR_NATIVE_TRANSPORT, default ON since the transport bake (native/transport-bake,
// 2026-10-15; =0 keeps the SDK bodies); per-present A/B: SR_AB_TOGGLE=SR_NATIVE_TRANSPORT
// with SR_AB_PRESENT=1 (abWindowOn, decided once per call).
#include "native_transport.hpp"
#include "ab_toggle.hpp"
#include "guest_context.hpp"
#include "guest_protect.hpp"
#include "kpcr.hpp"
#include "guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"

#include <atomic>
#include <cstdlib>
#include <windows.h>

namespace sr::native::transport {
namespace {
namespace K = sr::kernel;
std::atomic<uint64_t> g_append{0}, g_chain{0}, g_direct{0}, g_park{0}, g_resume{0}, g_decline{0};
std::atomic<uint64_t> g_abOffAppend{0}, g_abOffEnd{0};

// Host time inside the native entries (QPC ticks; gate ON only): each admitted or declined call from its top to its
// return - lock waits included, the park's KeWaitForSingleObject excluded - and the part of an append's chain path
// spent on admission while it holds the guest spinlock dev+0x2A78 (the lock EndSync takes). Census: the totals and
// per-call maxima as us, and the tail - calls over 100 us and over 1 ms - so a live roll shows what the pair itself
// costs and how long it holds the pump off (tpl1, 2026-10-14: the OS permission queries stalled it for ms).
uint64_t qpcPerSecond() {
  static const uint64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return uint64_t(q.QuadPart); }();
  return f;
}
struct HostTime {
  std::atomic<uint64_t> ticks{0}, max{0}, over100us{0}, over1ms{0};
  void add(uint64_t t) {
    ticks.fetch_add(t, std::memory_order_relaxed);
    uint64_t m = max.load(std::memory_order_relaxed);
    while (t > m && !max.compare_exchange_weak(m, t, std::memory_order_relaxed)) {}
    if (t * 10000u > qpcPerSecond()) over100us.fetch_add(1, std::memory_order_relaxed);
    if (t * 1000u > qpcPerSecond()) over1ms.fetch_add(1, std::memory_order_relaxed);
  }
};
HostTime g_appendTime, g_endTime, g_lockedAdmission;
uint64_t qpcNow() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return uint64_t(q.QuadPart); }
double qpcUs(uint64_t ticks) { return double(ticks) * 1e6 / double(qpcPerSecond()); }
// Charges the enclosing native call's host time to one accumulator when it leaves scope (or at stop()).
class Timed {
 public:
  explicit Timed(HostTime& sink) : sink_(sink), t0_(qpcNow()) {}
  ~Timed() { stop(); }
  void stop() { if (t0_) { sink_.add(qpcNow() - t0_); t0_ = 0; } }
 private:
  HostTime& sink_;
  uint64_t t0_;
};
// Exclusion: a page-protecting diagnostic is requested or a protection excluding all stores was declared
// (kernel/guest_protect.hpp) - every entry declines. g_foreignRange: admissions refused because the range touches a
// declared foreign-protected range (each also counted under its entry's positional reason). Retired: the guest arena
// was retired for its release (kernel/guest_protect.hpp ArenaLease) - no lease, no guest access.
enum class DeclineReason { Thread, Append, Tail, State, Marker, Exclusion, Retired, Count };
std::atomic<uint64_t> g_declineReasons[size_t(DeclineReason::Count)]{};
std::atomic<uint64_t> g_foreignRange{0};
enum class RejectCheck { Shape, Device, List, Node, Stack, TailAlignment, TailMemory, Count };
std::atomic<uint32_t> g_rejectSamples[size_t(RejectCheck::Count)]{};

void traceReject(RejectCheck check, const PPCContext& ctx, uint32_t address, uint32_t bytes) {
  static const bool on = [] {
    const char* value = std::getenv("SR_NATIVE_TRANSPORT_DIAG");
    return value && value[0] == '1';
  }();
  if (!on || g_rejectSamples[size_t(check)].fetch_add(1, std::memory_order_relaxed) >= 8u) return;
  const char* const names[] = {"shape", "device", "list", "node", "stack", "tail-alignment", "tail-memory"};
  MEMORY_BASIC_INFORMATION first{}, last{};
  const uint8_t* firstHost = K::guestVAToHost(address);
  if (firstHost) VirtualQuery(firstHost, &first, sizeof(first));
  if (bytes && address <= UINT32_MAX - (bytes - 1u)) {
    const uint8_t* lastHost = K::guestVAToHost(address + bytes - 1u);
    if (lastHost) VirtualQuery(lastHost, &last, sizeof(last));
  }
  // The failed predicate and its range are exact. These optional permission
  // snapshots occur afterward and do not establish permissions at rejection.
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[NATIVE_TRANSPORT] reject check=%s va=%08X bytes=%u firstState=%X firstProtect=%X lastState=%X lastProtect=%X"
      " dev=%08X at=%08X phys=%08X dw=%u increment=%u list=%08X stack=%08X owner=%08X",
      names[size_t(check)], address, bytes, first.State, first.Protect, last.State, last.Protect,
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r1.u32, ctx.r13.u32);
}

// Arithmetic admission (2026-10-14, the orchestrator's option A after tpl1; NATIVE_ROADMAP 'TRANSPORT PAIR ADMISSION
// A'). No OS query: live, QueryWorkingSetEx / VirtualQuery stalled the pump and the render thread for milliseconds
// (VirtualQuery walks the resident pages of the marker's whole same-protection region; both contend with the write
// tracker's protection changes). A guest range is admitted when it is non-empty, does not wrap, maps contiguously
// into the registered guest arena - one committed read-write allocation in both memory models; nothing decommits
// inside it (kernel/guest_protect.hpp) - and touches no range a foreign protector declared. What can still protect an
// admitted page is the GPU write tracker (PAGE_READONLY on the 0x8 / 0xA windows): the pair's stores are plain host
// stores (writeTransportWord), so the first one into such a page faults into the tracker's handler exactly as a guest
// store does, and the handler releases the block and publishes its invalidation before the store retries.
bool admit(uint32_t va, uint32_t bytes) {
  if (!va || !bytes || va > UINT32_MAX - (bytes - 1u)) return false;
  const uint8_t* first = K::guestVAToHost(va);
  const uint8_t* last = K::guestVAToHost(va + bytes - 1u);
  if (!first || !last || last - first != ptrdiff_t(bytes - 1u)) return false;
  uint8_t* arena = nullptr;
  size_t arenaSize = 0;
  K::getGlobalGuestMemory(&arena, &arenaSize);
  if (!arena || first < arena || size_t(last - arena) >= arenaSize) return false;
  if (K::guest_protect::intersects(first, bytes)) {
    g_foreignRange.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}
// A page-protecting diagnostic was requested, or a protection that excludes all arithmetic admission was declared.
bool excluded() {
  return K::guest_protect::diagnosticRequested() || K::guest_protect::excludesAll();
}
uint32_t readTransportWord(uint32_t va) { return K::readGuestU32BE(va); }
void writeTransportWord(uint32_t va, uint32_t value) { K::writeGuestBE32(va, value); }
uint32_t softwareAddress(uint32_t va) {
  return ((((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu)) - 0x40000000u;
}
bool ready(const PPCContext& ctx) {
  return K::keSpinlockEnabled() && ctx.r13.u32 &&
         K::kpcrCurrent() == ctx.r13.u32;
}
bool decline(DeclineReason reason) {
  g_declineReasons[size_t(reason)].fetch_add(1, std::memory_order_relaxed);
  g_decline.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// The per-present A/B (SR_AB_TOGGLE=SR_NATIVE_TRANSPORT with SR_AB_PRESENT=1; kernel/ab_toggle.hpp), read once per
// call at the top of each entry, before any read, import or write: one call is wholly native or wholly the SDK body.
// Both bodies leave the same guest-visible state and import sequence (the actual-SDK differential), so calls from
// either arm interleave on the shared Kf lock: dev+0x2A74, the software list and the pump's park state move as the
// SDK moves them in both. Unset (every other run), reg() is -1 and the gate alone decides.
bool abWindowOn() {
  static const int ab = sr::abt::reg("SR_NATIVE_TRANSPORT");
  return ab < 0 || sr::abt::on(ab);
}
// An OFF window: counted (the census shows both arms ran), not a decline.
bool abOff(std::atomic<uint64_t>& counter) {
  counter.fetch_add(1, std::memory_order_relaxed);
  return false;  // the SDK body runs
}

// Use the actual guest import path: it publishes r13 and preserves the same
// IRQL/owner protocol as the still-recompiled producer and consumer.
class ChainLock {
 public:
  ChainLock(PPCContext& work, uint8_t* base, uint32_t address)
      : work_(work), base_(base), address_(address) {
    work_.r3.u64 = address_;
    __imp__KfAcquireSpinLock(work_, base_);
    oldIrql_ = work_.r3.u32;
  }
  ~ChainLock() { release(); }
  void release() {
    if (!held_) return;
    work_.r3.u64 = address_; work_.r4.u64 = oldIrql_;
    __imp__KfReleaseSpinLock(work_, base_);
    held_ = false;
  }
 private:
  PPCContext& work_;
  uint8_t* base_;
  uint32_t address_, oldIrql_ = 0;
  bool held_ = true;
};
}

bool gateOn() {  // default ON since the transport bake; =0 off (the A/B and the opt-out)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_TRANSPORT");  // `e`: scripts/audit_forcing.py reads `!e ||` as default ON
    return !e || e[0] != '0';
  }();
  return on;
}

bool appendJumpSegment(PPCContext& ctx, uint8_t* base) {
  if (!gateOn()) return false;
  if (!abWindowOn()) return abOff(g_abOffAppend);
  Timed timed(g_appendTime);
  // Held across every guest access below, sub_820FD218 and the lock imports included (Astra round A, P1). Scope
  // (Astra round B): a retired decline returns false, and the production dispatch then runs the SDK body, which still
  // touches guest memory. The lease protects this native arithmetic admission only; the pre-existing teardown race of
  // guest threads that survive the arena's release (SDK bodies included) is unchanged.
  K::guest_protect::ArenaLease appendLease;
  if (!appendLease) return decline(DeclineReason::Retired);
  if (excluded()) return decline(DeclineReason::Exclusion);
  if (!ready(ctx)) return decline(DeclineReason::Thread);
  const uint32_t dev = ctx.r3.u32, at = ctx.r4.u32, phys = ctx.r5.u32;
  const uint32_t dwords = ctx.r6.u32, increment = ctx.r7.u32, list = ctx.r8.u32;
  const uint64_t returned = ctx.r4.u64;
  // Both possible branches must be admitted before changing device/list state.
  // Storage is owned by the caller until its submitted work completes.
  if (!dev || ((dev | at | list | ctx.r1.u32) & 3u) || dev > UINT32_MAX - 0x2A7Fu) {
    traceReject(RejectCheck::Shape, ctx, dev, 0u); return decline(DeclineReason::Append);
  }
  if (!admit(dev + 0x2A74u, 8u)) {
    traceReject(RejectCheck::Device, ctx, dev + 0x2A74u, 8u); return decline(DeclineReason::Append);
  }
  if (!admit(list, 16u)) {
    traceReject(RejectCheck::List, ctx, list, 16u); return decline(DeclineReason::Append);
  }
  if (at > UINT32_MAX - 16u || !admit(at, 16u)) {
    traceReject(RejectCheck::Node, ctx, at, 16u); return decline(DeclineReason::Append);
  }
  if (ctx.r1.u32 < 0x1000u || !admit(ctx.r1.u32 - 0x1000u, 0x1000u)) {
    traceReject(RejectCheck::Stack, ctx, ctx.r1.u32 - 0x1000u, 0x1000u); return decline(DeclineReason::Append);
  }
  PPCContext work = ctx;
  bool chained = false;
  if (readTransportWord(dev + 0x2A74u)) {
    ChainLock lock(work, base, dev + 0x2A78u);
    // EndSync may have parked between the initial read and lock acquisition.
    if (readTransportWord(dev + 0x2A74u)) {
      const uint32_t previousTail = readTransportWord(list + 8u);
      const uint64_t admit0 = qpcNow();
      if ((previousTail & 3u)) {
        g_lockedAdmission.add(qpcNow() - admit0);
        lock.release(); traceReject(RejectCheck::TailAlignment, ctx, previousTail, 4u);
        return decline(DeclineReason::Tail);
      }
      const bool tailAdmitted = admit(previousTail, 4u);
      g_lockedAdmission.add(qpcNow() - admit0);
      if (!tailAdmitted) {
        lock.release(); traceReject(RejectCheck::TailMemory, ctx, previousTail, 4u);
        return decline(DeclineReason::Tail);
      }
      const uint32_t address = softwareAddress(at);
      writeTransportWord(at + 8u, phys);
      writeTransportWord(at + 12u, 0xC0000000u);
      writeTransportWord(at + 4u, 0x82000000u | dwords);
      writeTransportWord(at, address + 16u);
      writeTransportWord(list + 8u, at + 12u);
      writeTransportWord(list + 4u, at);
      // Publish initialized contents before linking the old parked tail. Host
      // coherent memory needs no console cache flush or MMIO operation.
      MemoryBarrier();
      writeTransportWord(previousTail, address);
      MemoryBarrier();
      writeTransportWord(dev + 0x2A74u, readTransportWord(dev + 0x2A74u) + increment);
      chained = true;
    }
  }
  if (!chained) {
    // Retail intentionally increments outside the chain lock on this branch;
    // keep that ordering, including when the locked recheck found zero.
    const uint32_t frame = ctx.r1.u32 - 0xB0u;
    writeTransportWord(frame, ctx.r1.u32);
    writeTransportWord(frame + 0x50u, dwords); writeTransportWord(frame + 0x54u, phys);
    writeTransportWord(dev + 0x2A74u, readTransportWord(dev + 0x2A74u) + increment);
    work.r1.u64 = frame; work.r3.u64 = dev;
    work.r4.u64 = frame + 0x50u; work.r5.u64 = 1u;
    sub_820FD218(work, base);
    g_direct.fetch_add(1, std::memory_order_relaxed);
  } else g_chain.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = returned + (chained ? 16u : 0u);
  g_append.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool endSync(PPCContext& ctx, uint8_t* base) {
  if (!gateOn()) return false;
  if (!abWindowOn()) return abOff(g_abOffEnd);
  Timed timed(g_endTime);
  // Held across every guest access below, the park's KeWaitForSingleObject on the guest event included: a pump
  // parked for good keeps the arena mapped at teardown (Astra round A, P1). A retired decline falls back to the SDK
  // body like any decline: native admission only, not whole-process teardown safety (see appendJumpSegment).
  K::guest_protect::ArenaLease endLease;
  if (!endLease) return decline(DeclineReason::Retired);
  if (excluded()) return decline(DeclineReason::Exclusion);
  if (!ready(ctx)) return decline(DeclineReason::Thread);
  const uint32_t state = ctx.r3.u32, at = ctx.r4.u32;
  if (((state | at) & 3u) || state > UINT32_MAX - 0x5Fu ||
      !admit(state, 0x60u)) return decline(DeclineReason::State);
  if (!admit(at, 4u)) return decline(DeclineReason::Marker);
  PPCContext work = ctx;
  ChainLock lock(work, base, state + 0x3Cu);
  // Re-read under the very same lock as the tail-link store. Never use a marker
  // sampled before acquiring it, or a producer can strand newly linked work.
  const uint32_t marker = readTransportWord(at);
  if (marker != 0xC0000000u) {
    lock.release();
    ctx.r3.u64 = uint64_t(marker) + 4u;
    g_resume.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  const uint32_t nesting = readTransportWord(state + 0x30u) - 1u;
  writeTransportWord(state + 0x30u, nesting);
  work.r3.u64 = state + 0x40u;
  if (!nesting) {
    if (!readTransportWord(state + 0x50u)) writeTransportWord(state + 0x38u, readTransportWord(state + 0x38u) - 1u);
    work.r4.u64 = 1u; work.r5.u64 = 0u;
    __imp__KeSetEvent(work, base);
    writeTransportWord(state + 0x34u, 0u);
  } else __imp__KeResetEvent(work, base);
  writeTransportWord(state + 0x24u, at);
  lock.release();
  writeTransportWord(state, 0u);
  work.r3.u64 = state + 0x40u; work.r4.u64 = 3u;
  work.r5.u64 = 1u; work.r6.u64 = 0u; work.r7.u64 = 0u;
  timed.stop();  // the park's wait is the pump's idle time, not the pair's
  __imp__KeWaitForSingleObject(work, base);
  ctx.r3.u64 = 0u;
  g_park.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void finalLine(const char* why) {
  if (!gateOn()) return;
  // abOffAppend / abOffEnd: the calls an A/B OFF window sent to the SDK body (0 without SR_AB_TOGGLE); appended
  // after the decline reasons so the earlier fields keep their positions. Then (2026-10-14) the exclusion declines
  // and the declared-range refusals, and the host-time census with its tails (calls over 100 us / 1 ms).
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[NATIVE_TRANSPORT] %s append=%llu chain=%llu direct=%llu park=%llu resume=%llu decline=%llu"
      " | owner=%llu appendInput=%llu tailInput=%llu endState=%llu endMarker=%llu"
      " | abOffAppend=%llu abOffEnd=%llu | exclusion=%llu foreignRange=%llu retired=%llu"
      " | appendUs=%.0f appendMaxUs=%.1f appendOver100us=%llu appendOver1ms=%llu"
      " endUs=%.0f endMaxUs=%.1f endOver100us=%llu endOver1ms=%llu lockedAdmUs=%.0f lockedAdmMaxUs=%.1f", why,
      (unsigned long long)g_append.load(), (unsigned long long)g_chain.load(),
      (unsigned long long)g_direct.load(), (unsigned long long)g_park.load(),
      (unsigned long long)g_resume.load(), (unsigned long long)g_decline.load(),
      (unsigned long long)g_declineReasons[size_t(DeclineReason::Thread)].load(),
      (unsigned long long)g_declineReasons[size_t(DeclineReason::Append)].load(),
      (unsigned long long)g_declineReasons[size_t(DeclineReason::Tail)].load(),
      (unsigned long long)g_declineReasons[size_t(DeclineReason::State)].load(),
      (unsigned long long)g_declineReasons[size_t(DeclineReason::Marker)].load(),
      (unsigned long long)g_abOffAppend.load(), (unsigned long long)g_abOffEnd.load(),
      (unsigned long long)g_declineReasons[size_t(DeclineReason::Exclusion)].load(),
      (unsigned long long)g_foreignRange.load(),
      (unsigned long long)g_declineReasons[size_t(DeclineReason::Retired)].load(),
      qpcUs(g_appendTime.ticks.load()), qpcUs(g_appendTime.max.load()),
      (unsigned long long)g_appendTime.over100us.load(), (unsigned long long)g_appendTime.over1ms.load(),
      qpcUs(g_endTime.ticks.load()), qpcUs(g_endTime.max.load()),
      (unsigned long long)g_endTime.over100us.load(), (unsigned long long)g_endTime.over1ms.load(),
      qpcUs(g_lockedAdmission.ticks.load()), qpcUs(g_lockedAdmission.max.load()));
}

Counters counters() {
  return {g_append.load(), g_chain.load(), g_direct.load(), g_park.load(), g_resume.load(), g_decline.load(),
          g_abOffAppend.load(), g_abOffEnd.load(),
          g_declineReasons[size_t(DeclineReason::Exclusion)].load(), g_foreignRange.load(),
          g_declineReasons[size_t(DeclineReason::Append)].load(), g_declineReasons[size_t(DeclineReason::Tail)].load(),
          g_declineReasons[size_t(DeclineReason::State)].load(), g_declineReasons[size_t(DeclineReason::Marker)].load(),
          g_declineReasons[size_t(DeclineReason::Retired)].load()};
}
}

extern "C" void __imp__sub_820FD800(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82110768(PPCContext& __restrict, uint8_t*);
PPC_FUNC(sub_820FD800) {
  if (!(sr::native::transport::gateOn() && sr::native::transport::appendJumpSegment(ctx, base)))
    __imp__sub_820FD800(ctx, base);
}
PPC_FUNC(sub_82110768) {
  if (!(sr::native::transport::gateOn() && sr::native::transport::endSync(ctx, base)))
    __imp__sub_82110768(ctx, base);
}
