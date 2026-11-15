// DOGMA Pool Witness — runtime instrumentation for sub_82182700 (the real
// DOGMA top-level entry, per wave-7/8 forensics).
//
// Goal: capture WHICH host threads are concurrently calling into DOGMA at
// hang time. Wave-9 (thread_local→global allocator demote) brought baseline
// to ~80% deterministic but cumulative case (FMShadows re-enabled) is still
// 1/3 success. The remaining contention source is unknown — this file is
// pure observation, no behavior change.
//
// Pattern: strong-override sub_82182700 by defining a non-weak symbol here
// (the recomp emits a weak alias to __imp__sub_82182700). On entry we log
// host TID, guest LR, pool address (r3 == r31 of sub_82182700 == pool
// object), an in-flight counter, and a per-thread last-entry timestamp.
//
// Invariants we DO NOT touch:
//   - we always call __imp__sub_82182700 with original ctx, no pre/post
//     mutation of register state.
//   - we do not introduce locks; the in-flight counter is a relaxed atomic.
//   - we do not return early under any circumstance.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

extern "C" void __imp__sub_82182700(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::dogma_witness {

namespace {

using Clock = std::chrono::steady_clock;

// Concurrent in-flight count across ALL threads.
std::atomic<int>     g_inFlight{0};

// Total entries observed.
std::atomic<uint64_t> g_totalEntries{0};

// Total contention events (entries observed when in-flight > 1).
std::atomic<uint64_t> g_contentionEvents{0};

// Last entry telemetry, used for "rapid handoff vs simultaneous" + cross
// thread cross-reference on contention.
struct LastEntry {
  std::atomic<uint32_t> tid{0};
  std::atomic<uint32_t> lr{0};
  std::atomic<uint32_t> pool{0};
  std::atomic<uint64_t> tNs{0};         // steady_clock ns since process boot
  std::atomic<uint64_t> tEnterNs{0};    // when most recent entry started
  std::atomic<int>      inUse{0};       // 1 if this thread is currently inside
};

// Track up to N distinct host threads. 16 is more than enough — harness +
// render thread + 4 ExCreateThread workers + slop.
constexpr size_t kMaxThreads = 16;
LastEntry g_lastByThread[kMaxThreads];

// Thread-table lookup: linear-scan, atomic-CAS to claim a slot first time.
// Returns slot index or kMaxThreads if full.
size_t getThreadSlot(uint32_t tid) {
  for (size_t i = 0; i < kMaxThreads; ++i) {
    uint32_t expected = g_lastByThread[i].tid.load(std::memory_order_acquire);
    if (expected == tid) return i;
    if (expected == 0) {
      uint32_t z = 0;
      if (g_lastByThread[i].tid.compare_exchange_strong(z, tid, std::memory_order_acq_rel)) {
        return i;
      }
      // Lost the CAS — re-check if it became our tid (impossible normally,
      // but lets us fall through to a re-scan iteration).
      if (g_lastByThread[i].tid.load(std::memory_order_acquire) == tid) return i;
    }
  }
  return kMaxThreads;
}

// Per-thread entry counter for the "first 100 per-thread" cap.
struct PerThreadInfoCount {
  std::atomic<uint32_t> n{0};
};
PerThreadInfoCount g_infoCount[kMaxThreads];

inline uint64_t nowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch()).count());
}

// Dump every other thread's most-recent entry data — used when one thread
// has been holding DOGMA > 1000 ms.
void dumpAllThreads(const char* reason, uint32_t selfTid) {
  for (size_t i = 0; i < kMaxThreads; ++i) {
    uint32_t tid  = g_lastByThread[i].tid.load(std::memory_order_acquire);
    if (tid == 0) continue;
    uint32_t lr   = g_lastByThread[i].lr.load(std::memory_order_relaxed);
    uint32_t pool = g_lastByThread[i].pool.load(std::memory_order_relaxed);
    uint64_t tNs  = g_lastByThread[i].tNs.load(std::memory_order_relaxed);
    int      use  = g_lastByThread[i].inUse.load(std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[DOGMA_WIT] DUMP %s self=%u peer_slot=%zu tid=%u in_use=%d lr=0x%08X pool=0x%08X last_t_ns=%llu",
      reason, selfTid, i, tid, use, lr, pool, (unsigned long long)tNs);
  }
}

}  // namespace

// Public reset (not currently called from anywhere; left for future use).
void reset() {
  g_inFlight.store(0);
  g_totalEntries.store(0);
  g_contentionEvents.store(0);
  for (size_t i = 0; i < kMaxThreads; ++i) {
    g_lastByThread[i].tid.store(0);
    g_lastByThread[i].lr.store(0);
    g_lastByThread[i].pool.store(0);
    g_lastByThread[i].tNs.store(0);
    g_lastByThread[i].tEnterNs.store(0);
    g_lastByThread[i].inUse.store(0);
    g_infoCount[i].n.store(0);
  }
}

}  // namespace sr::dogma_witness

// Strong override of sub_82182700 — DOGMA top-level entry.
void sub_82182700(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace sr::dogma_witness;

#if defined(_WIN32)
  const uint32_t tid = static_cast<uint32_t>(::GetCurrentThreadId());
#else
  const uint32_t tid = 0;
#endif

  const uint32_t lr   = ctx.lr;
  const uint32_t pool = ctx.r3.u32;
  const uint64_t t    = nowNs();

  // Per-thread book-keeping.
  size_t slot = getThreadSlot(tid);
  uint32_t prevTid = 0;
  uint32_t prevPool = 0;
  uint64_t prevTNs = 0;
  if (slot < kMaxThreads) {
    prevTNs = g_lastByThread[slot].tNs.load(std::memory_order_relaxed);
    g_lastByThread[slot].lr.store(lr, std::memory_order_relaxed);
    g_lastByThread[slot].pool.store(pool, std::memory_order_relaxed);
    g_lastByThread[slot].tNs.store(t, std::memory_order_relaxed);
    g_lastByThread[slot].tEnterNs.store(t, std::memory_order_relaxed);
    g_lastByThread[slot].inUse.store(1, std::memory_order_relaxed);
  }

  // Increment in-flight; capture pre-increment value to detect contention.
  const int inFlightAfter = g_inFlight.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint64_t totalEntry = g_totalEntries.fetch_add(1, std::memory_order_relaxed) + 1;

  // INFO log: first 100 per-thread.
  if (slot < kMaxThreads) {
    uint32_t n = g_infoCount[slot].n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 100) {
      uint64_t deltaSinceLast = (prevTNs == 0) ? 0 : (t - prevTNs);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[DOGMA_WIT] ENTER tid=%u slot=%zu n=%u total=%llu inflight=%d lr=0x%08X pool=0x%08X dt_self_ns=%llu",
        tid, slot, n, (unsigned long long)totalEntry, inFlightAfter, lr, pool,
        (unsigned long long)deltaSinceLast);
    }
  }

  // CONTENTION event: in-flight > 1. Always log, no cap. Identify the
  // peer thread(s) currently inside.
  if (inFlightAfter > 1) {
    uint64_t cev = g_contentionEvents.fetch_add(1, std::memory_order_relaxed) + 1;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[DOGMA_WIT] CONTENTION#%llu tid=%u inflight=%d lr=0x%08X pool=0x%08X total=%llu",
      (unsigned long long)cev, tid, inFlightAfter, lr, pool, (unsigned long long)totalEntry);
    // Identify peers (any other thread with inUse=1).
    for (size_t i = 0; i < kMaxThreads; ++i) {
      if (i == slot) continue;
      uint32_t peerTid = g_lastByThread[i].tid.load(std::memory_order_acquire);
      if (peerTid == 0) continue;
      int peerUse = g_lastByThread[i].inUse.load(std::memory_order_relaxed);
      if (peerUse) {
        uint32_t peerLr   = g_lastByThread[i].lr.load(std::memory_order_relaxed);
        uint32_t peerPool = g_lastByThread[i].pool.load(std::memory_order_relaxed);
        uint64_t peerTNs  = g_lastByThread[i].tEnterNs.load(std::memory_order_relaxed);
        const char* sameOrDiff = (peerPool == pool) ? "SAME_POOL" : "DIFF_POOL";
        uint64_t ageNs = (peerTNs == 0 || t < peerTNs) ? 0 : (t - peerTNs);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[DOGMA_WIT]   peer tid=%u (slot=%zu) inflight%s lr=0x%08X pool=0x%08X (%s) age_ns=%llu",
          peerTid, i, peerUse ? "_active" : "_idle",
          peerLr, peerPool, sameOrDiff, (unsigned long long)ageNs);
        prevTid = peerTid;
        prevPool = peerPool;
      }
    }
  }
  (void)prevTid; (void)prevPool;

  // Delegate to the lifted body. NOTE: DO NOT mutate ctx before/after.
  // Snapshot our own enter time for hang detection on the way back out.
  const uint64_t tEnterForHang = t;
  __imp__sub_82182700(ctx, base);

  // Decrement in-flight; mark slot idle.
  g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
  if (slot < kMaxThreads) {
    g_lastByThread[slot].inUse.store(0, std::memory_order_relaxed);
  }

  // Hang detection: if we were inside > 1000 ms, dump every other thread.
  const uint64_t tExit = nowNs();
  const uint64_t heldNs = (tExit > tEnterForHang) ? (tExit - tEnterForHang) : 0;
  if (heldNs > 1'000'000'000ull) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[DOGMA_WIT] LONG_HOLD tid=%u held_ms=%llu lr=0x%08X pool=0x%08X — dumping all peers",
      tid, (unsigned long long)(heldNs / 1'000'000ull), lr, pool);
    dumpAllThreads("LONG_HOLD", tid);
  }
}
