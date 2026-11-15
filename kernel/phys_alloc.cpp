// iter-458 H-FREE: the physical allocator (contract in phys_alloc.hpp).
#include "phys_alloc.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <vector>

#include "../app/log.hpp"
#include "guest_context.hpp"

namespace sr {
namespace kernel {
namespace physalloc {
namespace {

struct State {
  std::mutex mu;
  uint32_t lo = kLo, hi = kHi;
  std::map<uint32_t, uint32_t> freeR;   // start -> end (exclusive), coalesced
  std::map<uint32_t, uint32_t> live;    // start -> end (exclusive)
  std::map<uint32_t, uint32_t> hist;    // PHYSICAL start -> end, coalesced
  std::map<uint32_t, uint32_t> quar;    // quarantined VA start -> end
  uint64_t quarantined = 0, quarantinedBytes = 0, historyPins = 0;
  uint64_t carvedBytes = 0, carves = 0;   // free space removed by a pin
  std::map<uint32_t, uint32_t> ever;      // VA extents ever allocated, coalesced
  uint64_t pinNoProvenanceBytes = 0;      // pinned free space never allocated
  uint64_t hostReservedBytes = 0;         // H-STACK: excluded host-owned ranges
  uint64_t hostOverlapAllocs = 0;         // H-STACK: allocations touching them (must stay 0)
  uint64_t allocs = 0, frees = 0, failedAllocs = 0, refusedFrees = 0, badProtect = 0;
  uint64_t liveBytes = 0, peakBytes = 0;
  uint32_t lowWater = kHi;
  bool init = false;
};
State& S() {
  static State s;
  return s;
}

void initLocked(State& s, uint32_t lo, uint32_t hi, bool carveHost = true) {
  s.lo = lo; s.hi = hi;
  s.freeR.clear(); s.live.clear(); s.hist.clear(); s.quar.clear();
  s.quarantined = s.quarantinedBytes = s.historyPins = 0;
  s.carvedBytes = s.carves = 0;
  s.ever.clear(); s.pinNoProvenanceBytes = 0;
  if (lo < hi) s.freeR[lo] = hi;
  // iter-458 H-STACK: the harness's main-thread stack lies inside the arena and
  // is never handed out (never allocatable; a free inside it is refused as not
  // a live block). Excluded at birth, so no ordering against host allocations.
  s.hostReservedBytes = 0;
  s.hostOverlapAllocs = 0;
  if (carveHost) {
    const uint64_t rLo = std::max<uint64_t>(lo, kMainStackBase);
    const uint64_t rHi = std::min<uint64_t>(hi, (uint64_t)kMainStackBase + kMainStackSize);
    if (lo < hi && rLo < rHi) {
      s.freeR.clear();
      if (lo < rLo) s.freeR[lo] = (uint32_t)rLo;
      if (rHi < hi) s.freeR[(uint32_t)rHi] = hi;
      s.hostReservedBytes = rHi - rLo;
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[PHYS_FREE] host range [%08llX,%08llX) (main-thread stack) excluded from the "
          "arena [%08X,%08X)",
          (unsigned long long)rLo, (unsigned long long)rHi, lo, hi);
    }
  }
  s.allocs = s.frees = s.failedAllocs = s.refusedFrees = s.badProtect = 0;
  s.liveBytes = s.peakBytes = 0;
  s.lowWater = hi;
  s.init = true;
}

void ensureInit(State& s) {
  if (!s.init) initLocked(s, kLo, kHi);
}

uint64_t roundUp(uint64_t v, uint64_t a) { return (v + a - 1u) & ~(a - 1u); }

bool histOverlapsLocked(const State& s, uint64_t lo, uint64_t hi) {
  if (lo >= hi) return false;
  auto it = s.hist.upper_bound((uint32_t)std::min<uint64_t>(lo, 0xFFFFFFFFu));
  if (it != s.hist.begin()) {
    auto pv = std::prev(it);
    if (pv->second > lo) return true;
  }
  return it != s.hist.end() && it->first < hi;
}

// ⛔ Astra 025: the invariant is freeR n history = {}. A pin whose extent
// overlaps space that is ALREADY FREE (free -> stale capture -> pin) moves that
// space out of the free list into quarantine NOW, under the same lock - so no
// later allocation can hand it out while the stale snapshot is published.
void carveFreeLocked(State& s, uint64_t pLo, uint64_t pHi) {
  const uint64_t vLo = std::max<uint64_t>((uint64_t)kLo + pLo, s.lo);
  const uint64_t vHi = std::min<uint64_t>((uint64_t)kLo + pHi, s.hi);
  if (vLo >= vHi) return;
  // PROVENANCE (Astra 027): a previous owner's data can only sit in space that
  // was ALLOCATED before. Collect free n pin n ever-allocated; free space that
  // was never handed out is left allocatable and counted.
  std::vector<std::pair<uint64_t, uint64_t>> cut;
  uint64_t freeOverlap = 0;
  auto it = s.freeR.upper_bound((uint32_t)vLo);
  if (it != s.freeR.begin()) --it;
  for (; it != s.freeR.end() && it->first < vHi; ++it) {
    const uint64_t fs = std::max<uint64_t>(it->first, vLo);
    const uint64_t fe = std::min<uint64_t>(it->second, vHi);
    if (fs >= fe) continue;
    freeOverlap += fe - fs;
    auto ev = s.ever.upper_bound((uint32_t)fs);
    if (ev != s.ever.begin()) --ev;
    for (; ev != s.ever.end() && ev->first < fe; ++ev) {
      const uint64_t a = std::max<uint64_t>(ev->first, fs);
      const uint64_t b = std::min<uint64_t>(ev->second, fe);
      if (a < b) cut.push_back({a, b});
    }
  }
  uint64_t carvedNow = 0;
  for (const auto& c : cut) {
    // [c.first, c.second) lies inside exactly one free range: split it out.
    auto fr = s.freeR.upper_bound((uint32_t)c.first);
    --fr;
    const uint64_t fs = fr->first, fe = fr->second;
    s.freeR.erase(fr);
    if (fs < c.first) s.freeR[(uint32_t)fs] = (uint32_t)c.first;
    if (c.second < fe) s.freeR[(uint32_t)c.second] = (uint32_t)fe;
    s.quar[(uint32_t)c.first] = (uint32_t)c.second;
    s.carves++;
    s.carvedBytes += c.second - c.first;
    s.quarantinedBytes += c.second - c.first;
    carvedNow += c.second - c.first;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[PHYS_FREE] CARVE #%llu free [%08llX,%08llX) previously allocated, pinned while "
        "free - quarantined | carved=%llu B quarantined=%llu B",
        (unsigned long long)s.carves, (unsigned long long)c.first,
        (unsigned long long)c.second, (unsigned long long)s.carvedBytes,
        (unsigned long long)s.quarantinedBytes);
  }
  if (freeOverlap > carvedNow) {
    s.pinNoProvenanceBytes += freeOverlap - carvedNow;
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[PHYS_FREE] pin over NEVER-allocated free space [%08llX,%08llX): %llu B left "
        "allocatable (no previous owner) | total=%llu B",
        (unsigned long long)vLo, (unsigned long long)vHi,
        (unsigned long long)(freeOverlap - carvedNow),
        (unsigned long long)s.pinNoProvenanceBytes);
  }
}
void freeTotalsLocked(const State& s, uint64_t& freeBytes, uint64_t& largest) {
  freeBytes = 0; largest = 0;
  for (const auto& r : s.freeR) {
    const uint64_t n = (uint64_t)r.second - r.first;
    freeBytes += n;
    if (n > largest) largest = n;
  }
}

void logEvent(const char* what, uint32_t a, uint64_t b, const State& s, uint64_t n) {
  if (n <= 16 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[PHYS_FREE] %s #%llu %08X size=0x%llX | live=%llu blocks %llu B peak=%llu "
        "lowWater=%08X failed=%llu refusedFrees=%llu",
        what, (unsigned long long)n, a, (unsigned long long)b,
        (unsigned long long)s.live.size(), (unsigned long long)s.liveBytes,
        (unsigned long long)s.peakBytes, s.lowWater,
        (unsigned long long)s.failedAllocs, (unsigned long long)s.refusedFrees);
}

// Provenance (Astra 027): every extent ever handed out, coalesced.
void noteEverLocked(State& s, uint32_t b, uint32_t e) {
  uint64_t lo = b, hi = e;
  auto it = s.ever.upper_bound(b);
  if (it != s.ever.begin()) {
    auto pv = std::prev(it);
    if (pv->second >= lo) {
      lo = pv->first;
      if (pv->second > hi) hi = pv->second;
      it = s.ever.erase(pv);
    }
  }
  while (it != s.ever.end() && it->first <= hi) {
    if (it->second > hi) hi = it->second;
    it = s.ever.erase(it);
  }
  s.ever[(uint32_t)lo] = (uint32_t)hi;
}

// Top-down first fit inside [minVa, maxVa] (inclusive upper bound on the LAST
// byte, as Xenia's AllocRange). size and align are already page-rounded; align
// is a power of two.
uint32_t allocLocked(State& s, uint64_t size, uint64_t align, uint64_t minVa,
                     uint64_t maxVa) {
  const uint64_t limitEnd = maxVa + 1u;   // exclusive
  for (auto it = s.freeR.end(); it != s.freeR.begin();) {
    --it;
    const uint64_t fs = std::max<uint64_t>(it->first, minVa);
    const uint64_t fe = std::min<uint64_t>(it->second, limitEnd);
    if (fe <= fs || fe - fs < size) continue;
    const uint64_t base = (fe - size) & ~(align - 1u);
    if (base < fs) continue;
    const uint32_t b = (uint32_t)base, e = (uint32_t)(base + size);
    // H-STACK (Astra 031): every allocation is checked against the host-owned
    // main stack, uncapped. Zero by construction; the counter proves it.
    if (b < (uint64_t)kMainStackBase + kMainStackSize && e > kMainStackBase) {
      s.hostOverlapAllocs++;
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[PHYS_FREE] HOST OVERLAP #%llu alloc [%08X,%08X) intersects the main-thread "
          "stack [%08X,%08X)",
          (unsigned long long)s.hostOverlapAllocs, b, e, kMainStackBase,
          kMainStackBase + kMainStackSize);
    }
    const uint32_t rs = it->first, re = it->second;
    s.freeR.erase(it);
    if (rs < b) s.freeR[rs] = b;
    if (e < re) s.freeR[e] = re;
    s.live[b] = e;
    noteEverLocked(s, b, e);
    s.allocs++;
    s.liveBytes += size;
    if (s.liveBytes > s.peakBytes) s.peakBytes = s.liveBytes;
    if (b < s.lowWater) s.lowWater = b;
    logEvent("alloc", b, size, s, s.allocs);
    return b;
  }
  s.failedAllocs++;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[PHYS_FREE] alloc FAILED size=0x%llX align=0x%llX range=[%08llX,%08llX] "
      "(live=%llu B, %llu blocks) - returns 0 as the console would",
      (unsigned long long)size, (unsigned long long)align,
      (unsigned long long)minVa, (unsigned long long)maxVa,
      (unsigned long long)s.liveBytes, (unsigned long long)s.live.size());
  return 0;
}

}  // namespace

bool physFreeEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PHYS_FREE");
    return e && e[0] == '1';   // DEFAULT OFF
  }();
  return s_on;
}

uint32_t mmAllocate(uint32_t size, uint32_t protect, uint32_t minPhys,
                    uint32_t maxPhys, uint32_t alignment) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  ensureInit(s);
  if (!(protect & (kXPageReadOnly | kXPageReadWrite))) {
    s.badProtect++;
    s.failedAllocs++;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[PHYS_FREE] MmAllocatePhysicalMemoryEx refused: bad protection bits %08X",
        protect);
    return 0;
  }
  uint64_t page = kPage;
  if (protect & kXMemLargePages) page = 0x10000u;
  else if (protect & kXMem16MbPages) page = 0x1000000u;
  const uint64_t sz = roundUp(size ? size : 1u, page);
  uint64_t al = roundUp(alignment, page);
  if (al < page) al = page;
  if ((al & (al - 1u)) != 0u) {   // not a power of two: next power of two
    uint64_t p = page;
    while (p < al) p <<= 1;
    al = p;
  }
  // Physical bounds -> 0xA-window VA bounds, clamped to the arena.
  const uint64_t minVa = std::max<uint64_t>((uint64_t)kLo + minPhys, s.lo);
  const uint64_t maxVa = std::min<uint64_t>((uint64_t)kLo + maxPhys, (uint64_t)s.hi - 1u);
  if (minVa > maxVa) {
    s.failedAllocs++;
    return 0;
  }
  return allocLocked(s, sz, al, minVa, maxVa);
}

bool serviceAllocate(uint32_t size, uint32_t protect, uint32_t minPhys,
                     uint32_t maxPhys, uint32_t alignment, uint32_t& out) {
  if (!physFreeEnabled()) return false;
  out = mmAllocate(size, protect, minPhys, maxPhys, alignment);
  return true;
}

void serviceFree(uint32_t base) {
  if (physFreeEnabled()) free(base);
}

uint32_t alloc(uint32_t size, uint32_t align) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  ensureInit(s);
  uint64_t al = align < kPage ? kPage : align;
  if ((al & (al - 1u)) != 0u) al = kPage;
  return allocLocked(s, roundUp(size ? size : 1u, kPage), al, s.lo, (uint64_t)s.hi - 1u);
}

bool free(uint32_t base) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  ensureInit(s);
  auto it = s.live.find(base);
  if (it == s.live.end()) {
    s.refusedFrees++;
    if (s.refusedFrees <= 16 || (s.refusedFrees & (s.refusedFrees - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[PHYS_FREE] free REFUSED #%llu base=%08X (not a live block start)",
          (unsigned long long)s.refusedFrees, base);
    return false;
  }
  uint32_t b = it->first, e = it->second;
  const uint32_t origB = b, origE = e;
  s.live.erase(it);
  s.liveBytes -= (uint64_t)e - b;
  s.frees++;
  // The block's ORIGINAL extent, in physical terms (before any coalescing).
  const uint64_t pLo = b & 0x1FFFFFFFu, pHi = pLo + ((uint64_t)e - b);
  uint64_t freeBytes = 0, largest = 0;
  if (histOverlapsLocked(s, pLo, pHi)) {
    s.quar[b] = e;
    s.quarantined++;
    s.quarantinedBytes += (uint64_t)e - b;
    freeTotalsLocked(s, freeBytes, largest);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[PHYS_FREE] QUARANTINE #%llu %08X size=0x%X phys=[%08llX,%08llX) - host-cache "
        "history overlaps, never reused | quarantined=%llu (%llu B) free=%llu B "
        "largestFree=%llu B live=%llu B pins=%llu extents=%llu",
        (unsigned long long)s.quarantined, b, e - b, (unsigned long long)pLo,
        (unsigned long long)pHi, (unsigned long long)s.quarantined,
        (unsigned long long)s.quarantinedBytes, (unsigned long long)freeBytes,
        (unsigned long long)largest, (unsigned long long)s.liveBytes,
        (unsigned long long)s.historyPins, (unsigned long long)s.hist.size());
    return true;
  }
  auto nx = s.freeR.lower_bound(b);
  if (nx != s.freeR.end() && nx->first == e) {
    e = nx->second;
    nx = s.freeR.erase(nx);
  }
  if (nx != s.freeR.begin()) {
    auto pv = std::prev(nx);
    if (pv->second == b) {
      b = pv->first;
      s.freeR.erase(pv);
    }
  }
  s.freeR[b] = e;
  freeTotalsLocked(s, freeBytes, largest);
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[PHYS_FREE] freed #%llu %08X size=0x%llX | free=%llu B largestFree=%llu B live=%llu B "
      "quarantined=%llu (%llu B) pins=%llu extents=%llu",
      (unsigned long long)s.frees, origB, (unsigned long long)(origE - origB),
      (unsigned long long)freeBytes,
      (unsigned long long)largest, (unsigned long long)s.liveBytes,
      (unsigned long long)s.quarantined, (unsigned long long)s.quarantinedBytes,
      (unsigned long long)s.historyPins, (unsigned long long)s.hist.size());
  return true;
}

void pinHistory(uint32_t key, uint64_t bytes) {
  if (!physFreeEnabled()) return;
  if (bytes == 0) bytes = 1;
  uint64_t lo = key & 0x1FFFFFFFu;
  uint64_t hi = lo + bytes;
  if (hi > 0x20000000u) hi = 0x20000000u;   // physical space is 512 MB
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  ensureInit(s);
  s.historyPins++;
  // Insert [lo, hi) and coalesce with every overlapping or adjacent extent.
  auto it = s.hist.upper_bound((uint32_t)lo);
  if (it != s.hist.begin()) {
    auto pv = std::prev(it);
    if (pv->second >= lo) {
      lo = pv->first;
      if (pv->second > hi) hi = pv->second;
      it = s.hist.erase(pv);
    }
  }
  while (it != s.hist.end() && it->first <= hi) {
    if (it->second > hi) hi = it->second;
    it = s.hist.erase(it);
  }
  s.hist[(uint32_t)lo] = (uint32_t)hi;
  carveFreeLocked(s, key & 0x1FFFFFFFu,
                  std::min<uint64_t>((uint64_t)(key & 0x1FFFFFFFu) + bytes, 0x20000000u));
  const uint64_t np = s.historyPins;
  if (np <= 8 || (np & (np - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[PHYS_FREE] pin #%llu phys=[%08X,+0x%llX) extents=%llu",
        (unsigned long long)np, key & 0x1FFFFFFFu, (unsigned long long)bytes,
        (unsigned long long)s.hist.size());
}

bool historyOverlaps(uint32_t physLo, uint64_t bytes) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  const uint64_t lo = physLo & 0x1FFFFFFFu;
  return histOverlapsLocked(s, lo, lo + (bytes ? bytes : 1u));
}

uint32_t liveSize(uint32_t base) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  auto it = s.live.find(base);
  return it == s.live.end() ? 0u : it->second - it->first;
}

bool insideLive(uint32_t va, uint32_t size) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  auto it = s.live.upper_bound(va);
  if (it == s.live.begin()) return false;
  --it;
  return it->first <= va && (uint64_t)va + size <= it->second;
}

Stats stats() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  ensureInit(s);
  uint64_t freeBytes = 0, largest = 0;
  freeTotalsLocked(s, freeBytes, largest);
  return Stats{s.allocs,    s.frees,     s.failedAllocs,         s.refusedFrees,
               s.badProtect, s.liveBytes, s.peakBytes, (uint64_t)s.live.size(),
               s.lowWater,   s.quarantined, s.quarantinedBytes, s.historyPins,
               (uint64_t)s.hist.size(), freeBytes, largest, s.carves, s.carvedBytes,
               s.pinNoProvenanceBytes, s.hostReservedBytes, s.hostOverlapAllocs};
}

void resetForTest(uint32_t lo, uint32_t hi) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  initLocked(s, lo, hi);
}

void resetForTestNoHostCarve(uint32_t lo, uint32_t hi) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  initLocked(s, lo, hi, /*carveHost=*/false);
}

}  // namespace physalloc
}  // namespace kernel
}  // namespace sr
