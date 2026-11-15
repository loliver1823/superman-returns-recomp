// thread_mem_xenia.cpp - see thread_mem_xenia.hpp (iter-465 physmem).
// Placement ported from xenia-canary (BSD-3): XThread::AllocateStack / FreeStack
// (src/xenia/kernel/xthread.cc), Memory::SystemHeapAlloc + BaseHeap::AllocSystemHeap
// (src/xenia/memory.cc).
#include "thread_mem_xenia.hpp"

#include "guest_context.hpp"
#include "ntavm_xenia_heap.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace sr::kernel::threadmem {
namespace {
std::atomic<int> g_force{-1};
std::atomic<uint64_t> g_stacks{0}, g_releases{0}, g_failures{0}, g_system{0};
std::atomic<int64_t> g_live{0};
thread_local uint32_t t_allocBase = 0;

bool shouldLog(uint64_t n) { return n <= 16 || (n & (n - 1)) == 0; }

void zeroGuest(uint32_t va, uint32_t size) {
  if (uint8_t* p = guestVAToHost(va)) std::memset(p, 0, size);
}
}  // namespace

bool enabled() {
  const int f = g_force.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool v = [] {
    const char* e = std::getenv("SR_THREAD_MEM_XENIA");
    return !e || e[0] != '0';   // default ON since the iter-465 rolls; =0 keeps the A/B
  }();
  return v && xheap::enabled();
}
void forceEnabledForTest(int mode) { g_force.store(mode, std::memory_order_relaxed); }

uint32_t allocStack(uint32_t size, uint32_t* allocBase) {
  if (allocBase) *allocBase = 0;
  const uint32_t rounded = (size + kStackGuard - 1) & ~(kStackGuard - 1);
  uint32_t grant = 0;
  const uint32_t base = xheap::allocRange(kStackLo, kStackHi, rounded + 2 * kStackGuard,
                                          kStackGuard, /*large=*/true, /*topDown=*/false, &grant);
  if (!base) {
    const uint64_t k = g_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLog(k))
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[THREAD_MEM] stack 0x%X: no room in [%08X,%08X) (#%llu) - caller falls back",
                  size, kStackLo, kStackHi, static_cast<unsigned long long>(k));
    return 0;
  }
  zeroGuest(base, grant);
  if (allocBase) *allocBase = base;
  const uint64_t n = g_stacks.fetch_add(1, std::memory_order_relaxed) + 1;
  const int64_t live = g_live.fetch_add(1, std::memory_order_relaxed) + 1;
  if (shouldLog(n))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[THREAD_MEM] stack #%llu alloc=%08X limit=%08X top=%08X (0x%X + 2 guard pages) "
                "live=%lld",
                static_cast<unsigned long long>(n), base, base + kStackGuard,
                base + kStackGuard + rounded, rounded, static_cast<long long>(live));
  return base + kStackGuard;
}

void releaseStack(uint32_t allocBase) {
  if (!allocBase) return;
  const uint32_t sz = xheap::release(allocBase);
  if (!sz) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[THREAD_MEM] release of unknown stack %08X", allocBase);
    return;
  }
  const uint64_t n = g_releases.fetch_add(1, std::memory_order_relaxed) + 1;
  const int64_t live = g_live.fetch_sub(1, std::memory_order_relaxed) - 1;
  if (shouldLog(n))
    SR_LOG_BOOT(sr::LogSeverity::Info, "[THREAD_MEM] stack release #%llu alloc=%08X size=0x%X live=%lld",
                static_cast<unsigned long long>(n), allocBase, sz, static_cast<long long>(live));
}

void bindCurrentStack(uint32_t allocBase) { t_allocBase = allocBase; }

void releaseCurrentStack() {
  const uint32_t b = t_allocBase;
  t_allocBase = 0;
  releaseStack(b);
}

uint32_t allocSystem(uint32_t size) {
  uint32_t grant = 0;
  const uint32_t va = xheap::allocRange(kSystemLo, kSystemHi, size, 0x1000u, /*large=*/false,
                                        /*topDown=*/false, &grant);
  if (!va) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[THREAD_MEM] system block 0x%X: no room", size);
    return 0;
  }
  zeroGuest(va, grant);
  const uint64_t n = g_system.fetch_add(1, std::memory_order_relaxed) + 1;
  if (shouldLog(n))
    SR_LOG_BOOT(sr::LogSeverity::Info, "[THREAD_MEM] system block #%llu va=%08X size=0x%X",
                static_cast<unsigned long long>(n), va, grant);
  return va;
}

Stats stats() {
  Stats s;
  s.stacks = g_stacks.load(std::memory_order_relaxed);
  s.stackReleases = g_releases.load(std::memory_order_relaxed);
  s.stackFailures = g_failures.load(std::memory_order_relaxed);
  s.systemBlocks = g_system.load(std::memory_order_relaxed);
  const int64_t l = g_live.load(std::memory_order_relaxed);
  s.liveStacks = l > 0 ? static_cast<uint32_t>(l) : 0u;
  return s;
}

}  // namespace sr::kernel::threadmem
