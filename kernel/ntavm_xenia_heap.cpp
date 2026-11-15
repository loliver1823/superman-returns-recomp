// ntavm_xenia_heap.cpp - see ntavm_xenia_heap.hpp. Port of the placement rules of
// Xenia's BaseHeap::Alloc / AllocRange (memory.cc) for the two virtual heaps that
// NtAllocateVirtualMemory uses when the title passes BaseAddress == 0.
// Reference: reference/xenia-canary-oracle (BSD-3).
#include "ntavm_xenia_heap.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>

namespace sr::kernel::xheap {
namespace {

struct Heap {
  uint32_t lo, hi, page;
  std::map<uint32_t, uint32_t> live;   // base -> size (rounded)
};

std::mutex g_mu;
Heap g_v00{kV00Base + kZeroGuard, kV00End, kV00Page, {}};
Heap g_v40{kV40Base, kV40End, kV40Page, {}};
std::atomic<int> g_force{-1};

// Lock-free ownership map for owns(): one bit per 4 KB page of [0, kV40End).
// owns() runs on hot host paths (every guest memcpy through the null guard's
// plausibility test), so it must not take g_mu. Bits are set after a region is
// recorded and cleared before it is erased, both under g_mu.
constexpr uint32_t kOwnPage = 0x1000u;
constexpr uint32_t kOwnWords = (kV40End / kOwnPage + 63) / 64;
std::atomic<uint64_t> g_ownBits[kOwnWords];

void markRange(uint32_t va, uint32_t size, bool set) {
  const uint32_t first = va / kOwnPage;
  const uint32_t last = (va + size - 1) / kOwnPage;
  for (uint32_t pg = first; pg <= last && pg / 64 < kOwnWords; ++pg) {
    const uint64_t bit = uint64_t(1) << (pg % 64);
    if (set) g_ownBits[pg / 64].fetch_or(bit, std::memory_order_release);
    else     g_ownBits[pg / 64].fetch_and(~bit, std::memory_order_release);
  }
}

inline uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
inline uint32_t alignDown(uint32_t v, uint32_t a) { return v & ~(a - 1); }

// First fit in the gaps between live regions, bottom-up (or top-down).
uint32_t findGap(const Heap& h, uint32_t size, bool topDown) {
  if (!topDown) {
    uint32_t cur = h.lo;
    for (const auto& [b, s] : h.live) {
      const uint32_t start = alignUp(cur, h.page);
      if (b >= start && b - start >= size) return start;
      if (b + s > cur) cur = b + s;
    }
    const uint32_t start = alignUp(cur, h.page);
    if (start < h.hi && h.hi - start >= size) return start;
    return 0;
  }
  uint32_t cur = h.hi;   // exclusive top of the current gap
  for (auto it = h.live.rbegin(); it != h.live.rend(); ++it) {
    const uint32_t end = it->first + it->second;
    if (cur >= end && cur - end >= size) {
      const uint32_t start = alignDown(cur - size, h.page);
      if (start >= end) return start;
    }
    if (it->first < cur) cur = it->first;
  }
  if (cur > h.lo && cur - h.lo >= size) {
    const uint32_t start = alignDown(cur - size, h.page);
    if (start >= h.lo) return start;
  }
  return 0;
}

}  // namespace

bool enabled() {
  const int f = g_force.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool s_on = [] {
    const char* e = std::getenv("SR_NTAVM_XENIA_HEAP");
    return !e || e[0] != '0';   // iter-463: default ON after the cmdbuf2 rolls; =0 keeps the A/B
  }();
  return s_on;
}

uint32_t alloc(uint32_t size, bool large, bool topDown, uint32_t* grantOut) {
  if (!size) return 0;
  const uint64_t rounded = (static_cast<uint64_t>(size) + kSizeRound - 1) & ~uint64_t(kSizeRound - 1);
  if (rounded > 0x40000000ull) return 0;
  const uint32_t sz = static_cast<uint32_t>(rounded);
  std::lock_guard<std::mutex> lk(g_mu);
  Heap& h = large ? g_v40 : g_v00;
  const uint32_t va = findGap(h, sz, topDown);
  if (!va) return 0;
  h.live.emplace(va, sz);
  markRange(va, sz, true);
  if (grantOut) *grantOut = sz;
  return va;
}

namespace {
// First fit for [size] at a multiple of `align` inside [lo, hi), in the gaps
// between h.live (base -> size, disjoint). 64-bit math: hi may be 0x7F000000 and
// the sums must not wrap.
uint32_t findGapIn(const Heap& h, uint64_t lo, uint64_t hi, uint64_t size, uint64_t align,
                   bool topDown) {
  if (lo >= hi || size == 0 || size > hi - lo) return 0;
  if (!topDown) {
    uint64_t cur = lo;
    auto it = h.live.upper_bound(static_cast<uint32_t>(lo));
    if (it != h.live.begin()) {
      const auto p = std::prev(it);
      cur = std::max<uint64_t>(cur, uint64_t(p->first) + p->second);
    }
    for (;; ++it) {
      const bool last = it == h.live.end() || it->first >= hi;
      const uint64_t gapEnd = last ? hi : it->first;
      const uint64_t start = (cur + align - 1) & ~(align - 1);
      if (start + size <= gapEnd) return static_cast<uint32_t>(start);
      if (last) return 0;
      cur = std::max<uint64_t>(cur, uint64_t(it->first) + it->second);
    }
  }
  uint64_t cur = hi;   // exclusive top of the current gap
  auto it = h.live.lower_bound(static_cast<uint32_t>(std::min<uint64_t>(hi, 0xFFFFFFFFull)));
  for (;;) {
    // the region below `it` (if any) bounds this gap from below
    const bool first = it == h.live.begin();
    const auto p = first ? h.live.end() : std::prev(it);
    const uint64_t pEnd = first ? 0 : uint64_t(p->first) + p->second;
    const uint64_t gapLo = std::max<uint64_t>(lo, pEnd);
    if (cur >= gapLo + size) {
      const uint64_t start = (cur - size) & ~(align - 1);
      if (start >= gapLo) return static_cast<uint32_t>(start);
    }
    if (first || pEnd <= lo) return 0;
    cur = std::min<uint64_t>(cur, p->first);
    it = p;
  }
}
}  // namespace

uint32_t allocRange(uint32_t lo, uint32_t hi, uint32_t size, uint32_t align, bool large,
                    bool topDown, uint32_t* grantOut) {
  if (!size) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  Heap& h = large ? g_v40 : g_v00;
  const uint64_t page = h.page;
  const uint64_t sz = (static_cast<uint64_t>(size) + page - 1) & ~(page - 1);
  uint64_t al = align < page ? page : align;
  if (al & (al - 1)) return 0;   // alignment must be a power of two
  const uint64_t rlo = std::max<uint64_t>(lo, h.lo);
  const uint64_t rhi = std::min<uint64_t>(hi, h.hi);
  const uint32_t va = findGapIn(h, rlo, rhi, sz, al, topDown);
  if (!va) return 0;
  h.live.emplace(va, static_cast<uint32_t>(sz));
  markRange(va, static_cast<uint32_t>(sz), true);
  if (grantOut) *grantOut = static_cast<uint32_t>(sz);
  return va;
}

uint32_t release(uint32_t base) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (Heap* h : {&g_v00, &g_v40}) {
    auto it = h->live.find(base);
    if (it != h->live.end()) {
      const uint32_t sz = it->second;
      markRange(base, sz, false);
      h->live.erase(it);
      return sz;
    }
  }
  return 0;
}

bool owns(uint32_t va) {
  if (va >= kV40End) return false;
  const uint32_t pg = va / kOwnPage;
  return (g_ownBits[pg / 64].load(std::memory_order_acquire) >> (pg % 64)) & 1u;
}

void resetForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_v00.live.clear();
  g_v40.live.clear();
  for (auto& w : g_ownBits) w.store(0, std::memory_order_relaxed);
}
void forceEnabledForTest(int mode) { g_force.store(mode, std::memory_order_relaxed); }
uint32_t liveRegionsForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  return static_cast<uint32_t>(g_v00.live.size() + g_v40.live.size());
}

}  // namespace sr::kernel::xheap
