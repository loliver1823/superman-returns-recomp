// iter-33 (roadmap Phase 1): per-pool host serialization for sub_82182700
// (the DOGMA top-level entry).
//
// Wave-10 witness empirically captured 20+ contention events at this entry
// (main thread + 2 ExCreateThread workers concurrently inside sub_82182700
// on the same pool). The guest CS at pool+1328 is broken in our recomp:
// sub_825FC108's TryEnter wall-clock-3s timeout returns -2 WITHOUT holding
// the CS, but every caller (sub_82182700, sub_82180788, sub_8278DCB8)
// discards the return and unconditionally Leaves → counter underflow.
// Walker reproducibility drops to ~1/3 as a result.
//
// Wave-10's prior "pool-VA std::mutex" attempt regressed (per RAG id
// 8f403077 — likely because std::mutex is non-recursive and same-thread
// re-entry from within sub_82182700's sub-dispatches deadlocks).
//
// This file: strong-override sub_82182700 to acquire a per-pool
// std::recursive_mutex around the lifted body. Re-entry from same thread
// is fine (recursive); concurrent THREADS serialize per-pool. Different
// pools don't block each other.
//
// Strong-override pattern: defining a non-weak `sub_82182700` here
// displaces the recomp's `PPC_WEAK_FUNC(sub_82182700)` weak alias to
// `__imp__sub_82182700`. Same pattern as kernel/splash_hooks.cpp,
// kernel/dogma_hash_hooks.cpp.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <mutex>
#include <unordered_map>

extern "C" void __imp__sub_82182700(PPCContext& __restrict ctx, uint8_t* base);

namespace {
std::mutex g_poolMapMutex;
std::unordered_map<uint32_t, std::recursive_mutex> g_poolMutexes;

std::recursive_mutex* getPoolMutex(uint32_t poolVA) {
  if (poolVA == 0) return nullptr;
  std::lock_guard<std::mutex> lk(g_poolMapMutex);
  return &g_poolMutexes[poolVA];  // creates on first access
}
}  // namespace

void sub_82182700(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t pool = ctx.r3.u32;

  std::recursive_mutex* poolMtx = getPoolMutex(pool);
  std::unique_lock<std::recursive_mutex> poolLock;
  if (poolMtx) {
    poolLock = std::unique_lock<std::recursive_mutex>(*poolMtx);
  }

  // First N entries: log so we can confirm serialization is happening.
  static std::atomic<unsigned> s_callLog{0};
  unsigned n = s_callLog.fetch_add(1, std::memory_order_relaxed);
  if (n < 10) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[POOL_SERIALIZE] sub_82182700 #%u pool=0x%08X (re-entrant lock acquired)",
      n + 1, pool);
  }

  __imp__sub_82182700(ctx, base);
}
