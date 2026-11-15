// iter-466 cp lane: CPU-write tracking for the PM4 shared memory - see gpu_write_watch.hpp.
// Ported in spirit from Xenia (memory.cc PhysicalHeap::EnableAccessCallbacks /
// TriggerCallbacks, gpu/shared_memory.cc MemoryInvalidationCallback; BSD-3, CREDITS.md).
#include "gpu_write_watch.hpp"

#include "guest_context.hpp"
#include "hitch_log.hpp"  // iter-466 stutter: SR_HITCH_LOG (observe-only)
#include "sr_oracle.hpp"
#include "../app/log.hpp"
#include "../app/crash_handler.hpp"  // sr::kVehFirst (step4/relcrash: VEH order in ASan builds)
#include "../gfx/resource_census.hpp"  // mem.resources 2a (memres2): a census trap released before a kernel-mode write
#include "../gfx/resource_memory.hpp"
#include "../gfx/resource_ab.hpp"  // mem.resources frozen-pass A/B (resourceDecides / dual mode)

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include "native_memory.hpp"
#endif

namespace sr::kernel::gww {
namespace {

constexpr uint32_t kBlocks = kPages >> kBlockLog2;
constexpr uint8_t kProt = 1, kValid = 2;
constexpr uint64_t kWin8 = 0x80000000ull, kWinA = 0xA0000000ull;

std::atomic<uint8_t> g_st[kPages];
std::atomic<uint8_t> g_own[kPages];
std::atomic<uint32_t> g_gen[kPages];  // protection generation: +1 per protect (protGen)
std::atomic<uint64_t> g_relBits[(kPages >> kBlockLog2) / 64];  // blocks released since takeReleasedBlocks
std::atomic<uint32_t> g_coolUntil[kBlocks];
std::atomic<uint64_t> g_relClock{0};      // iter-467 native-b: the release clock (releaseClock)
std::atomic<uint64_t> g_blockRel[kBlocks];  // the clock value of each block's last release
uint8_t g_faultsOf[kBlocks];
std::atomic<uint32_t> g_frame{0};
std::atomic<uint64_t> g_faults{0}, g_faultPages{0}, g_protects{0}, g_protectPages{0}, g_hostWrites{0},
    g_skipped{0}, g_staleChecks{0}, g_stale{0}, g_genLate{0}, g_genRefused{0};

// Host kernel-mode writes in flight (HostWriteScope): physical page ranges the CP must not
// protect. bit 63 = used, [62:32] = last page, [31:0] = first page. An overflow (more scopes
// than slots) pins everything until it ends.
constexpr int kPinSlots = 16;
std::atomic<uint64_t> g_pins[kPinSlots];
std::atomic<int> g_pinsActive{0};
std::atomic<uint64_t> g_pinOverflow{0};

bool pinned(uint32_t p) {
  if (g_pinsActive.load(std::memory_order_acquire) == 0) return false;
  int seen = 0;
  for (int i = 0; i < kPinSlots; i++) {
    const uint64_t v = g_pins[i].load(std::memory_order_acquire);
    if (!v) continue;
    seen++;
    const uint32_t lo = uint32_t(v), hi = uint32_t(v >> 32) & 0x7FFFFFFFu;
    if (p >= lo && p <= hi) return true;
  }
  return seen < g_pinsActive.load(std::memory_order_acquire);  // an overflowed scope: all pinned
}

uint8_t* g_base = nullptr;
size_t g_size = 0;
std::atomic<bool> g_installed{false};
#if defined(_WIN32)
SRWLOCK g_lock = SRWLOCK_INIT;
#endif

// SR_NB_SHM_RANGE_TEST_NOREL=1 (checker mutation for the range cache's CHECK): releases do not stamp the clock.
bool testRangeNoRel() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_SHM_RANGE_TEST_NOREL");
    return e && e[0] == '1';
  }();
  return v;
}

bool testDrop() {  // SR_SHM_WT_TEST_DROP=1: a fault unprotects but keeps the page "clean" (checker mutation)
  static const bool v = [] {
    const char* e = std::getenv("SR_SHM_WT_TEST_DROP");
    return e && e[0] == '1';
  }();
  return v;
}

#if defined(_WIN32)
bool protectRun(uint32_t p, uint32_t n, DWORD prot) {
  DWORD old = 0;
  const bool a = VirtualProtect(g_base + kWin8 + (uint64_t(p) << 12), size_t(n) << 12, prot, &old) != 0;
  const bool b = VirtualProtect(g_base + kWinA + (uint64_t(p) << 12), size_t(n) << 12, prot, &old) != 0;
  return a && b;
}

// Under g_lock: unprotect every protected page of block b; returns the pages released.
uint32_t releaseBlockLocked(uint32_t b, bool escalate) {
  const uint32_t p0 = b << kBlockLog2, p1 = p0 + (1u << kBlockLog2);
  // iter-467 native-b: stamp the block before any page of it becomes writable (releaseClock).
  if (!testRangeNoRel()) g_blockRel[b].store(g_relClock.fetch_add(1, std::memory_order_acq_rel) + 1u,
                                             std::memory_order_release);
  uint32_t released = 0, runStart = 0, runLen = 0;
  const bool keep = testDrop();
  for (uint32_t p = p0; p <= p1; p++) {
    const bool prot = p < p1 && (g_st[p].load(std::memory_order_relaxed) & kProt);
    if (prot) {
      if (!runLen) runStart = p;
      runLen++;
      if (!keep) g_st[p].store(0, std::memory_order_release);
      released++;
      continue;
    }
    if (runLen) {
      protectRun(runStart, runLen, PAGE_READWRITE);
      runLen = 0;
    }
  }
  if (released) g_relBits[b >> 6].fetch_or(1ull << (b & 63), std::memory_order_acq_rel);
  if (released && escalate) {
    g_faultsOf[b] = uint8_t(std::min<uint32_t>(g_faultsOf[b] + 1u, 6u));
    g_coolUntil[b].store(g_frame.load(std::memory_order_relaxed) + (1u << g_faultsOf[b]),
                         std::memory_order_release);
  }
  return released;
}

// The physical page of a host address inside the 0x8 / 0xA window copies, or ~0u.
uint32_t pageOfHost(uintptr_t addr) {
  const uintptr_t b = uintptr_t(g_base);
  if (addr < b + kWin8 || addr >= b + kWinA + 0x20000000ull) return ~0u;
  return uint32_t(((addr - b) & 0x1FFFFFFFull) >> 12);
}

LONG WINAPI gwwVeh(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD* er = info->ExceptionRecord;
  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 ||
      er->ExceptionInformation[0] != 1 || !g_base)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t addr = er->ExceptionInformation[1];
  const uint32_t p = pageOfHost(addr);
  if (p == ~0u) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t hitchT0 = sr::hitch::begin();  // (the VEH dispatch before here is not counted)
  AcquireSRWLockExclusive(&g_lock);
  const uint32_t n = releaseBlockLocked(p >> kBlockLog2, /*escalate=*/true);
  ReleaseSRWLockExclusive(&g_lock);
  if (n) {
    sr::hitch::end(sr::hitch::kWtFault, hitchT0, n);
    g_faults.fetch_add(1, std::memory_order_relaxed);
    g_faultPages.fetch_add(n, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  // Nothing of ours protected there: another thread released it first (retry the store), or
  // the page is read-only for someone else's reason (not ours to handle). Fault-free query.
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) &&
      mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
      !(mbi.Protect & PAGE_GUARD))
    return EXCEPTION_CONTINUE_EXECUTION;
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

bool install() {
  if (g_installed.load(std::memory_order_acquire)) return g_base != nullptr;
#if defined(_WIN32)
  AcquireSRWLockExclusive(&g_lock);
  if (!g_installed.load(std::memory_order_relaxed)) {
    uint8_t* b = nullptr;
    size_t s = 0;
    getGlobalGuestMemory(&b, &s);
    if (b && s >= kWinA + 0x20000000ull && AddVectoredExceptionHandler(sr::kVehFirst, gwwVeh)) {
      g_base = b;
      g_size = s;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[SHM_WT] write tracking on: guest arena %p (+%llu MB), 0x8 + 0xA copies, blocks of %u "
                  "pages, check=%d testDrop=%d",
                  b, (unsigned long long)(s >> 20), 1u << kBlockLog2, checkMode() ? 1 : 0, testDrop() ? 1 : 0);
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHM_WT] write tracking OFF: guest arena %p size %llu or handler unavailable", b,
                  (unsigned long long)s);
    }
    g_installed.store(true, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&g_lock);
#endif
  return g_base != nullptr;
}

}  // namespace

bool enabled() {
  // The frozen-pass A/B (gfx/resource_ab.hpp dual mode) keeps the page model's write watch in every present.
  if (sr::gfx::memres::resourceMode() && !sr::gfx::memres::resourceAbDual()) return false;
  static const bool v = [] {
    const char* e = std::getenv("SR_SHM_WRITE_TRACK");  // default ON (iter-466 cp); =0 hashes every page
    return !e || e[0] != '0';
  }();
  return v;
}

bool checkMode() {
  static const bool v = [] {
    const char* e = std::getenv("SR_SHM_WT_CHECK");
    return e && e[0] == '1';
  }();
  return v;
}

bool cpuWritableRange(const void* pointer, size_t bytes) noexcept {
#if defined(_WIN32)
  const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
  if (!begin || !bytes || bytes > std::numeric_limits<uintptr_t>::max() - begin ||
      !enabled() || !g_installed.load(std::memory_order_acquire) || !g_base) return false;
  uint8_t* currentBase = nullptr;
  size_t currentSize = 0;
  getGlobalGuestMemory(&currentBase, &currentSize);
  if (currentBase != g_base || currentSize != g_size) return false;
  const uintptr_t end = begin + bytes, base = reinterpret_cast<uintptr_t>(g_base);
  if (begin < base + kWin8 || end > base + kWinA + 0x20000000ull) return false;
  // Keep both the ownership flags and OS permissions stable for the entire
  // query. Release the lock before any CPU store can invoke the exclusive VEH.
  AcquireSRWLockShared(&g_lock);
  bool admitted = true;
  for (uintptr_t at = begin; at < end;) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
      admitted = false; break;
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (next <= at) { admitted = false; break; }
    if (!sr::native::memory::writableProtection(region.Protect)) {
      // Only PAGE_READONLY is installed by protectForValidation. Readability
      // alone is insufficient: every covered page must still belong to us.
      if ((region.Protect & 0xFFu) != PAGE_READONLY) { admitted = false; break; }
      const uintptr_t last = std::min(next, end) - 1u;
      for (uintptr_t page = at & ~uintptr_t(0xFFFu); page <= last; page += 0x1000u) {
        const uint32_t p = pageOfHost(page);
        if (p == ~0u || !(g_st[p].load(std::memory_order_relaxed) & kProt)) {
          admitted = false; break;
        }
      }
      if (!admitted) break;
    }
    at = next;
  }
  ReleaseSRWLockShared(&g_lock);
  return admitted;
#else
  (void)pointer; (void)bytes;
  return false;
#endif
}

bool clean(uint32_t page, uint8_t own) {
  if (page >= kPages) return false;
  return g_st[page].load(std::memory_order_acquire) == (kProt | kValid) &&
         g_own[page].load(std::memory_order_relaxed) == own;
}

uint32_t protGen(uint32_t page) {
  if (page >= kPages || !(g_st[page].load(std::memory_order_acquire) & kProt)) return 0;
  const uint32_t g = g_gen[page].load(std::memory_order_acquire);
  return g ? g : 1u;  // (a wrapped generation never reads as "unprotected")
}

int cleanOwn(uint32_t page) {
  if (page >= kPages || g_st[page].load(std::memory_order_acquire) != (kProt | kValid)) return -1;
  return g_own[page].load(std::memory_order_relaxed);
}

void protectForValidation(const uint32_t* pages, uint32_t n) {
#if defined(_WIN32)
  if (!n || !install()) return;
  sr::hitch::Scope hitchScope(sr::hitch::kWtProtect);
  thread_local std::vector<uint32_t> ps;
  ps.clear();
  const uint32_t frame = g_frame.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t p = pages[i];
    if (p >= kPages || (g_st[p].load(std::memory_order_relaxed) & kProt) || pinned(p)) continue;
    if (int32_t(frame - g_coolUntil[p >> kBlockLog2].load(std::memory_order_relaxed)) < 0) continue;
    ps.push_back(p);
  }
  if (ps.empty()) return;
  std::sort(ps.begin(), ps.end());
  ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
  uint64_t runs = 0, done = 0;
  AcquireSRWLockExclusive(&g_lock);
  size_t i = 0;
  while (i < ps.size()) {
    size_t j = i;
    // (pinned again under the lock: a HostWriteScope pins before it takes the lock to release)
    while (j < ps.size() && !(g_st[ps[j]].load(std::memory_order_relaxed) & kProt) && !pinned(ps[j]) &&
           (j == i || ps[j] == ps[j - 1] + 1u))
      j++;
    if (j == i) {  // protected meanwhile
      i++;
      continue;
    }
    for (size_t k = i; k < j; k++) {
      g_gen[ps[k]].fetch_add(1, std::memory_order_relaxed);
      g_st[ps[k]].store(kProt, std::memory_order_release);
    }
    if (!protectRun(ps[i], uint32_t(j - i), PAGE_READONLY)) {
      // Not protected (both copies or neither): the pages stay untracked (hashed as before).
      protectRun(ps[i], uint32_t(j - i), PAGE_READWRITE);
      for (size_t k = i; k < j; k++) g_st[ps[k]].store(0, std::memory_order_release);
      static std::atomic<uint32_t> s_fail{0};
      if (s_fail.fetch_add(1, std::memory_order_relaxed) < 8)
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHM_WT] VirtualProtect failed for pages %08X+%u (gle=%lu)",
                    ps[i] << 12, uint32_t(j - i), (unsigned long)GetLastError());
      i = j;
      continue;
    }
    runs++;
    done += j - i;
    i = j;
  }
  ReleaseSRWLockExclusive(&g_lock);
  g_protects.fetch_add(runs, std::memory_order_relaxed);
  g_protectPages.fetch_add(done, std::memory_order_relaxed);
#else
  (void)pages;
  (void)n;
#endif
}

void markValidated(uint32_t page, uint8_t own) {
  if (page >= kPages) return;
  g_own[page].store(own, std::memory_order_relaxed);
  uint8_t cur = kProt;
  // Only a page still protected since protectForValidation becomes skippable.
  g_st[page].compare_exchange_strong(cur, uint8_t(kProt | kValid), std::memory_order_acq_rel);
}

// iter-467 native-b (SR_SHM_WT_GEN_MARK, default OFF; the late-mark count is always on): markValidated's compare-
// exchange only asks "protected now", not "protected since the hash". A validation that protected the page at
// generation G1 and hashed it with m_shmMu released can see a CPU write release it and ANOTHER protector (the
// rebaseline's REBASE_PROTECT, the IB cache, the constant skip - all outside m_shmMu) protect it again at G2 before
// the decision marks it: the page then reads clean under a hash of the content before the write, and when that hash
// equals the stored one nothing is uploaded - the GPU copy misses the write until the next one. The generation read
// after the exchange tells: a later generation counts as a late mark; with the gate the mark is undone (the page
// stays protected, not skippable: the next request hashes it). The exchange reads the state the later protector
// stored after bumping the generation, so the generation read after it sees that bump.
bool markValidated(uint32_t page, uint8_t own, uint32_t gen0) {
  static const bool s_refuse = [] {
    const char* e = std::getenv("SR_SHM_WT_GEN_MARK");
    return e && e[0] == '1';
  }();
  if (page >= kPages) return false;
  g_own[page].store(own, std::memory_order_relaxed);
  uint8_t cur = kProt;
  if (!g_st[page].compare_exchange_strong(cur, uint8_t(kProt | kValid), std::memory_order_acq_rel)) return false;
  const uint32_t g = g_gen[page].load(std::memory_order_acquire);
  if (gen0 && (g ? g : 1u) == gen0) return true;
  g_genLate.fetch_add(1, std::memory_order_relaxed);
  if (!s_refuse) return true;
  uint8_t v = uint8_t(kProt | kValid);
  g_st[page].compare_exchange_strong(v, kProt, std::memory_order_acq_rel);  // (a release meanwhile cleared it)
  g_genRefused.fetch_add(1, std::memory_order_relaxed);
  return false;
}

HostWriteScope::HostWriteScope(uint32_t guestVA, uint32_t len) {
#if SR_ORACLE
  // mem.resources 2a (memres2, SR_MEM_RESOURCES=census): a census trap (a write-protected page whose writer the census
  // names) over the range is released first - a kernel-mode write into it would fail. Whether or not this tracker runs.
  if (sr::gfx::memres::census()) sr::gfx::memres::hostWillWrite(guestVA, len);
#endif
#if defined(_WIN32)
  if (!len || !g_installed.load(std::memory_order_acquire) || !g_base) return;
  const uint64_t lo = guestVA, hi = uint64_t(guestVA) + len;
  if (hi <= kWin8 || lo >= kWinA + 0x20000000ull) return;
  // Pin the physical page range first (protectForValidation skips pinned pages), then release
  // what is protected now.
  const uint64_t pa = std::max<uint64_t>(lo, kWin8), pz = std::min<uint64_t>(hi, kWinA + 0x20000000ull);
  uint32_t pinLo = 0, pinHi = 0;  // physical pages [pinLo, pinHi]
  if (pa < kWinA && pz > kWinA) {  // spans both windows: pin everything (rare)
    pinLo = 0;
    pinHi = kPages - 1;
  } else {
    pinLo = uint32_t((pa & 0x1FFFFFFFull) >> 12);
    pinHi = uint32_t(((pz - 1u) & 0x1FFFFFFFull) >> 12);
  }
  for (int i = 0; i < kPinSlots; i++) {
    uint64_t expect = 0;
    const uint64_t v = (uint64_t(pinHi) << 32) | uint64_t(pinLo) | (1ull << 63);
    if (g_pins[i].compare_exchange_strong(expect, v, std::memory_order_acq_rel)) {
      slot_ = i;
      break;
    }
  }
  if (slot_ < 0) g_pinOverflow.fetch_add(1, std::memory_order_relaxed);
  g_pinsActive.fetch_add(1, std::memory_order_acq_rel);
  const uint64_t a = std::max<uint64_t>(lo, kWin8), z = std::min<uint64_t>(hi, kWinA + 0x20000000ull);
  // Per window part: the physical pages it covers.
  auto pagesOf = [](uint64_t s, uint64_t e, uint32_t& p0, uint32_t& p1) {
    p0 = uint32_t((s & 0x1FFFFFFFull) >> 12);
    p1 = uint32_t(((e - 1u) & 0x1FFFFFFFull) >> 12);
  };
  uint32_t released = 0;
  AcquireSRWLockExclusive(&g_lock);
  for (uint64_t s = a; s < z;) {
    const uint64_t winEnd = (s < kWinA) ? kWinA : kWinA + 0x20000000ull;
    const uint64_t e = std::min<uint64_t>(z, winEnd);
    uint32_t p0 = 0, p1 = 0;
    pagesOf(s, e, p0, p1);
    for (uint32_t b = p0 >> kBlockLog2; b <= (p1 >> kBlockLog2); b++) released += releaseBlockLocked(b, false);
    s = e;
  }
  ReleaseSRWLockExclusive(&g_lock);
  if (released) g_hostWrites.fetch_add(1, std::memory_order_relaxed);
  armed_ = true;
#else
  (void)guestVA;
  (void)len;
#endif
}

HostWriteScope::~HostWriteScope() {
  if (!armed_) return;
  if (slot_ >= 0) g_pins[slot_].store(0, std::memory_order_release);
  g_pinsActive.fetch_sub(1, std::memory_order_acq_rel);
}

void beginFrame() { g_frame.fetch_add(1, std::memory_order_relaxed); }

uint64_t releaseClock() { return g_relClock.load(std::memory_order_acquire); }

bool blocksReleasedSince(uint32_t block0, uint32_t block1, uint64_t stamp) {
  if (block1 >= kBlocks) block1 = kBlocks - 1u;
  for (uint32_t b = block0; b <= block1; b++) {
    const uint64_t s = g_blockRel[b].load(std::memory_order_acquire);
    if (s != 0 && s >= stamp) return true;  // (0: never released)
  }
  return false;
}

void takeReleasedBlocks(std::vector<uint32_t>& out) {
  out.clear();
  for (uint32_t w = 0; w < (kPages >> kBlockLog2) / 64; w++) {
    if (!g_relBits[w].load(std::memory_order_relaxed)) continue;
    uint64_t bits = g_relBits[w].exchange(0, std::memory_order_acq_rel);
    while (bits) {
      const uint32_t b = uint32_t(__builtin_ctzll(bits));
      out.push_back(w * 64u + b);
      bits &= bits - 1u;
    }
  }
}

Stats stats() {
  return Stats{g_faults.load(), g_faultPages.load(), g_protects.load(), g_protectPages.load(),
               g_hostWrites.load(), g_skipped.load(), g_staleChecks.load(), g_stale.load(),
               g_genLate.load(),    g_genRefused.load()};
}
void noteSkipped(uint32_t n) { g_skipped.fetch_add(n, std::memory_order_relaxed); }
void noteCheck(bool stale) {
  g_staleChecks.fetch_add(1, std::memory_order_relaxed);
  if (stale) g_stale.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace sr::kernel::gww
