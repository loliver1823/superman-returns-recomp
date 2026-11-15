// iter-458 H-VQ (Astra 035): the hostRangeReadable readable-interval cache
// (kernel/guest_context.{hpp,cpp}). Driven through the REAL hostRangeReadable over
// the difftest guest reservation (the same MEM_RESERVE|MEM_COMMIT 4 GB shape as the
// game). The cache is forced on/off through the test seam; the environment gate
// (SR_HRR_CACHE, default OFF) is not consulted.
//
// Run: build/difftest.exe --hrr-test   (exit code = failed checks)
#include "kernel/guest_context.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
uint64_t vq() { return K::hostRangeReadableStats().vqCalls; }
uint8_t* host(uint32_t va) { return K::getGuestBase() + va; }

// Ground truth without the cache: the seam forces the uncached walk.
bool uncached(const void* p, size_t n) {
  K::hostRangeReadableSetCacheForTest(0);
  const bool r = K::hostRangeReadable(p, n);
  K::hostRangeReadableSetCacheForTest(1);
  return r;
}

// Stale-refill seam state.
uint8_t* g_staleVictim = nullptr;

// Astra 040 interleaving: the remover runs ENTIRELY between the reader's
// lock-free generation sample and its lookup lock.
uint8_t* g_lateVictim = nullptr;
void lateRemoveFull() {
  K::hostRangeReadableBeginRemoval();
  VirtualFree(g_lateVictim, 0x1000, MEM_DECOMMIT);
  K::hostRangeReadableEndRemoval();
}
// ...and a remover caught mid-protocol: first bump + removal, second bump not yet.
void lateRemoveHalf() {
  K::hostRangeReadableBeginRemoval();
  VirtualFree(g_lateVictim, 0x1000, MEM_DECOMMIT);
}
// S9: the removal completes (decommit + End) between a walk that ran DURING the
// removal (page still readable) and that walk's insert.
uint8_t* g_s9Victim = nullptr;
void s9FinishRemoval() {
  VirtualFree(g_s9Victim, 0x1000, MEM_DECOMMIT);
  K::hostRangeReadableEndRemoval();
}
void staleHook() {
  // Runs between the walk (which saw the page readable) and the insert:
  // the page becomes unreadable under the before/after protocol.
  K::hostRangeReadableBeginRemoval();
  VirtualFree(g_staleVictim, 0x1000, MEM_DECOMMIT);
  K::hostRangeReadableEndRemoval();
}
}  // namespace

int runHrrTest() {
  std::printf("[HRR_TEST] hostRangeReadable interval cache\n");
  K::hostRangeReadableSetCacheForTest(1);
  K::hostRangeReadableInvalidate();

  // ── hits cost zero VirtualQuery calls ─────────────────────────────────────
  uint8_t* a = host(0x40000000u);
  check(K::hostRangeReadable(a, 0x1000), "C1 a guest span is readable");
  const uint64_t v0 = vq();
  bool all = true;
  for (int i = 0; i < 1000; i++) all &= K::hostRangeReadable(a + (i * 64) % 0x800, 256);
  check(all && vq() == v0, "C2 1000 warmed hits: ZERO additional VirtualQuery calls");
  const K::HostRangeReadableStats s1 = K::hostRangeReadableStats();
  check(s1.hits >= 1000, "C3 hits counted");

  // ── decommit / NOACCESS / GUARD with the before/after protocol ────────────
  uint8_t* d = host(0x41000000u);
  check(K::hostRangeReadable(d, 0x1000), "D0 page readable (and now cached)");
  K::hostRangeReadableBeginRemoval();
  VirtualFree(d, 0x1000, MEM_DECOMMIT);
  K::hostRangeReadableEndRemoval();
  check(!K::hostRangeReadable(d, 0x1000), "D1 decommitted page -> unreadable");
  check(!K::hostRangeReadable(d + 0x800, 16), "D2 its interior -> unreadable");
  check(K::hostRangeReadable(d - 0x1000, 0x1000) && K::hostRangeReadable(d + 0x1000, 0x1000),
        "D3 neighbours on both sides stay readable");
  check(!K::hostRangeReadable(d - 8, 16) && !K::hostRangeReadable(d + 0xFF8, 16),
        "D4 spans straddling either edge -> unreadable");
  check(K::hostRangeReadable(d - 0x1000, 0x1000) && !K::hostRangeReadable(d - 0x1000, 0x1001),
        "D4b a warmed interval ending at the hole: one byte past its end -> unreadable");
  check(K::hostRangeReadable(d + 0x1000, 0x10) && !K::hostRangeReadable(d + 0xFFF, 0x10),
        "D4c a warmed interval starting after the hole: one byte before its start -> unreadable");
  K::hostRangeReadableBeginRemoval();
  VirtualAlloc(d, 0x1000, MEM_COMMIT, PAGE_READWRITE);
  K::hostRangeReadableEndRemoval();
  check(K::hostRangeReadable(d, 0x1000), "D5 recommitted -> readable again");

  DWORD old = 0;
  uint8_t* n = host(0x42000000u);
  check(K::hostRangeReadable(n, 0x1000), "N0 page cached readable");
  K::hostRangeReadableBeginRemoval();
  VirtualProtect(n, 0x1000, PAGE_NOACCESS, &old);
  K::hostRangeReadableEndRemoval();
  check(!K::hostRangeReadable(n, 0x1000), "N1 PAGE_NOACCESS -> unreadable");
  K::hostRangeReadableBeginRemoval();
  VirtualProtect(n, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
  K::hostRangeReadableEndRemoval();
  check(!K::hostRangeReadable(n, 0x1000), "N2 PAGE_GUARD -> unreadable (and not touched)");
  K::hostRangeReadableBeginRemoval();
  VirtualProtect(n, 0x1000, PAGE_READONLY, &old);
  K::hostRangeReadableEndRemoval();
  check(K::hostRangeReadable(n, 0x1000), "N3 PAGE_READONLY -> readable");
  K::hostRangeReadableBeginRemoval();
  VirtualProtect(n, 0x1000, PAGE_READWRITE, &old);
  K::hostRangeReadableEndRemoval();

  // ── a stale in-flight walk must not repopulate the cache ──────────────────
  g_staleVictim = host(0x43000000u);
  K::hostRangeReadableInvalidate();
  const uint64_t st0 = K::hostRangeReadableStats().staleDrops;
  K::hostRangeReadableSetBeforeInsertHookForTest(staleHook);
  const bool sawReadable = K::hostRangeReadable(g_staleVictim, 0x1000);
  K::hostRangeReadableSetBeforeInsertHookForTest(nullptr);
  check(sawReadable, "S0 the in-flight walk saw the page readable (before the change)");
  check(K::hostRangeReadableStats().staleDrops == st0 + 1, "S1 its insert was dropped as stale");
  check(!K::hostRangeReadable(g_staleVictim, 0x1000), "S2 the next check sees the truth (unreadable)");
  K::hostRangeReadableBeginRemoval();
  VirtualAlloc(g_staleVictim, 0x1000, MEM_COMMIT, PAGE_READWRITE);
  K::hostRangeReadableEndRemoval();

  // ── Astra 040: a stale generation SAMPLE must not serve a stale HIT ───────
  g_lateVictim = host(0x46000000u);
  K::hostRangeReadableInvalidate();
  check(K::hostRangeReadable(g_lateVictim, 0x1000) && K::hostRangeReadable(g_lateVictim, 0x1000),
        "S3a page warmed readable (second call is a cache hit)");
  K::hostRangeReadableSetAfterGenSampleHookForTest(lateRemoveFull);
  const bool lateFull = K::hostRangeReadable(g_lateVictim, 0x1000);
  K::hostRangeReadableSetAfterGenSampleHookForTest(nullptr);
  check(!lateFull, "S3 invalidate+decommit+invalidate between the gen sample and the lock -> unreadable, not a stale hit");
  K::hostRangeReadableBeginRemoval();
  VirtualAlloc(g_lateVictim, 0x1000, MEM_COMMIT, PAGE_READWRITE);
  K::hostRangeReadableEndRemoval();
  check(K::hostRangeReadable(g_lateVictim, 0x1000) && K::hostRangeReadable(g_lateVictim, 0x1000),
        "S4a recommitted and re-warmed");
  K::hostRangeReadableSetAfterGenSampleHookForTest(lateRemoveHalf);
  const bool lateHalf = K::hostRangeReadable(g_lateVictim, 0x1000);
  K::hostRangeReadableSetAfterGenSampleHookForTest(nullptr);
  K::hostRangeReadableEndRemoval();   // the remover finishes
  check(!lateHalf, "S4 a remover mid-protocol (first bump + decommit) between sample and lock -> unreadable");
  check(!K::hostRangeReadable(g_lateVictim, 0x1000), "S5 and the next call agrees (unreadable)");
  K::hostRangeReadableBeginRemoval();
  VirtualAlloc(g_lateVictim, 0x1000, MEM_COMMIT, PAGE_READWRITE);
  K::hostRangeReadableEndRemoval();

  // ── Astra 041: first bump -> a reader REFILLS while the range is still
  //    readable -> removal -> a NEW reader must not hit that refill ───────────
  uint8_t* rv = host(0x47000000u);
  check(K::hostRangeReadable(rv, 0x1000) && K::hostRangeReadable(rv, 0x1000),
        "S6a page warmed readable");
  K::hostRangeReadableBeginRemoval();
  const uint64_t h6 = K::hostRangeReadableStats().hits;
  const bool r1 = K::hostRangeReadable(rv, 0x1000);   // still committed: a would-be refill
  VirtualFree(rv, 0x1000, MEM_DECOMMIT);
  const bool r2 = K::hostRangeReadable(rv, 0x1000);   // the new reader, BEFORE End
  const uint64_t h6b = K::hostRangeReadableStats().hits;
  K::hostRangeReadableEndRemoval();
  check(r1, "S6b a reader during the removal, before the decommit, saw the page readable");
  check(!r2, "S6 a NEW reader after the decommit but before End -> unreadable (no refill was served)");
  check(h6b == h6, "S7 zero cache hits while the removal was active");
  check(!K::hostRangeReadable(rv, 0x1000), "S7b after End: unreadable");
  K::hostRangeReadableBeginRemoval();
  VirtualAlloc(rv, 0x1000, MEM_COMMIT, PAGE_READWRITE);
  K::hostRangeReadableEndRemoval();
  {
    K::hostRangeReadable(rv, 0x1000);
    const uint64_t v8 = vq();
    const bool again = K::hostRangeReadable(rv, 0x1000);
    check(again && vq() == v8, "S8 after End, caching resumes (a warmed hit costs zero VirtualQuery)");
  }
  g_s9Victim = host(0x48000000u);
  K::hostRangeReadableBeginRemoval();
  K::hostRangeReadableSetBeforeInsertHookForTest(s9FinishRemoval);
  const bool w9 = K::hostRangeReadable(g_s9Victim, 0x1000);   // walks while readable; hook ends the removal
  K::hostRangeReadableSetBeforeInsertHookForTest(nullptr);
  check(w9, "S9a the walk during the removal saw the page readable");
  check(!K::hostRangeReadable(g_s9Victim, 0x1000),
        "S9 its insert AFTER End was refused (End's bump) -> the next call is unreadable, not a stale hit");
  K::hostRangeReadableBeginRemoval();
  VirtualAlloc(g_s9Victim, 0x1000, MEM_COMMIT, PAGE_READWRITE);
  K::hostRangeReadableEndRemoval();

  // ── host pointers and reservation boundaries are never cached ─────────────
  std::vector<uint8_t> heapBuf(4096, 1);
  K::hostRangeReadable(heapBuf.data(), heapBuf.size());
  const uint64_t hv = vq();
  K::hostRangeReadable(heapBuf.data(), heapBuf.size());
  check(vq() == hv + 1, "B1 a host (non-guest) pointer always queries");
  uint8_t* top = K::getGuestBase() + K::getGuestSize();
  const bool straddle = K::hostRangeReadable(top - 8, 16);
  check(straddle == uncached(top - 8, 16), "B2 a span straddling the reservation end = the uncached answer");
  const uint64_t sv = vq();
  K::hostRangeReadable(top - 8, 16);
  check(vq() > sv, "B3 ...and it is not answered from the cache");
  check(!K::hostRangeReadable(reinterpret_cast<void*>(UINTPTR_MAX - 16), 64),
        "B4 a span that would wrap the address space -> false");
  check(!K::hostRangeReadable(nullptr, 16) && !K::hostRangeReadable(a, 0),
        "B5 null / zero-length -> false");

  // ── equivalence: 1000 random guest spans, cached == uncached ──────────────
  uint8_t* hole = host(0x44010000u);
  K::hostRangeReadableBeginRemoval();
  VirtualFree(hole, 0x1000, MEM_DECOMMIT);
  K::hostRangeReadableEndRemoval();
  std::mt19937 rng(458);
  int mismatch = 0;
  for (int i = 0; i < 1000; i++) {
    const uint32_t off = 0x44000000u + (rng() % 0x20000u);
    const uint32_t len = 1 + rng() % 0x3000u;
    const bool c = K::hostRangeReadable(host(off), len);
    if (c != uncached(host(off), len)) mismatch++;
  }
  check(mismatch == 0, "E1 1000 random spans around a hole: cached == uncached");
  K::hostRangeReadableBeginRemoval();
  VirtualAlloc(hole, 0x1000, MEM_COMMIT, PAGE_READWRITE);
  K::hostRangeReadableEndRemoval();

  // ── 4 threads racing a flipping page ──────────────────────────────────────
  uint8_t* flip = host(0x45000000u);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> checks{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 4; t++)
    th.emplace_back([&, t] {
      std::mt19937 r(100 + t);
      while (!stop.load()) {
        K::hostRangeReadable(flip + (r() % 0x3000u), 64);
        checks++;
      }
    });
  // The readers must actually overlap the flips: start flipping only once they
  // run, and keep flipping until they have made thousands of checks.
  while (checks.load() < 1000) std::this_thread::yield();
  const uint64_t c0 = checks.load();
  int k = 0;
  for (; k < 200 || checks.load() < c0 + 20000; k++) {
    K::hostRangeReadableBeginRemoval();
    if (k & 1) VirtualAlloc(flip + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    else VirtualFree(flip + 0x1000, 0x1000, MEM_DECOMMIT);
    K::hostRangeReadableEndRemoval();
  }
  const bool committedAtEnd = ((k - 1) & 1) != 0;
  stop = true;
  for (auto& x : th) x.join();
  check(checks.load() >= c0 + 20000, "R0 the readers overlapped the flips (>= 20000 concurrent checks)");
  check(K::hostRangeReadable(flip + 0x1000, 0x1000) == committedAtEnd &&
            uncached(flip + 0x1000, 0x1000) == committedAtEnd,
        "R1 4 threads x flipping page: the settled answer matches the final state");
  if (!committedAtEnd) {
    K::hostRangeReadableBeginRemoval();
    VirtualAlloc(flip + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    K::hostRangeReadableEndRemoval();
  }

  // ── the window predicate ──────────────────────────────────────────────────
  check(!K::hrrWindowActive(119.9, 120, 240) && K::hrrWindowActive(120, 120, 240) &&
            K::hrrWindowActive(239.9, 120, 240) && !K::hrrWindowActive(240, 120, 240),
        "W1 window [on, off): half-open");
  {
    double a = 0, b = 0;
    check(K::hrrParseWindow("90-120", a, b) && a == 90 && b == 120, "W2 '90-120' parses");
    check(K::hrrParseWindow("90:120", a, b) && K::hrrParseWindow("90,120", a, b),
          "W3 ':' and ',' separators parse too");
    check(!K::hrrParseWindow("120-90", a, b) && !K::hrrParseWindow("90", a, b) &&
              !K::hrrParseWindow("abc", a, b) && !K::hrrParseWindow("90-120x", a, b) &&
              !K::hrrParseWindow("", a, b) && !K::hrrParseWindow(nullptr, a, b),
          "W4 reversed / single / junk / trailing / empty / null -> rejected");
    check(!K::hrrParseWindow("0-inf", a, b) && !K::hrrParseWindow("inf-inf", a, b) &&
              !K::hrrParseWindow("0-1e999", a, b) && !K::hrrParseWindow("nan-5", a, b) &&
              !K::hrrParseWindow("5-nan", a, b) && !K::hrrParseWindow("-5-10", a, b),
          "W5 infinite / overflowing / NaN / negative endpoints -> rejected (Astra 037)");
  }
  {
    const K::HrrConfig unsetW = K::hrrConfigFromEnv("1", nullptr);
    const K::HrrConfig emptyW = K::hrrConfigFromEnv("1", "");
    const K::HrrConfig badW = K::hrrConfigFromEnv("1", "90,120x");
    const K::HrrConfig goodW = K::hrrConfigFromEnv("1", "90-120");
    const K::HrrConfig offW = K::hrrConfigFromEnv(nullptr, "90-120");
    check(unsetW.enabled && !unsetW.windowed && !unsetW.badWindow,
          "W6 cache=1, window unset -> enabled, unwindowed");
    check(!emptyW.enabled && emptyW.badWindow && !badW.enabled && badW.badWindow,
          "W7 window SET but empty / unparsable -> fail CLOSED (Astra 037)");
    check(goodW.enabled && goodW.windowed && !goodW.badWindow && goodW.onAt == 90 &&
              goodW.offAt == 120,
          "W8 cache=1, window '90-120' -> enabled, windowed [90,120)");
    // iter-464: the cache is DEFAULT ON (movie-pace lane); only '0' turns it off.
    const K::HrrConfig dflt = K::hrrConfigFromEnv(nullptr, nullptr);
    check(offW.enabled && offW.windowed && !K::hrrConfigFromEnv("0", nullptr).enabled &&
              !K::hrrConfigFromEnv("0", "90-120").enabled && dflt.enabled &&
              !dflt.windowed && !dflt.badWindow,
          "W9 cache unset -> ON (default ON since iter-464), '0' -> OFF");
  }

  K::hostRangeReadableSetCacheForTest(-1);
  // One call in env mode so the [HRR] config line (SR_HRR_CACHE / _WINDOW glue)
  // is exercised and printed; its answer must still be the truth.
  check(K::hostRangeReadable(a, 64), "G1 env-mode call answers correctly");
  std::printf("[HRR_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
