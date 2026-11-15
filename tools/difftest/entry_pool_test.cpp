// Step 6 I6 (joint plan I6, Fable ruling 21 (8); render/gate3/FREEZE_I6_I7_20261022.txt + addendum 1): native_flush's
// Entry::Impl free list (SR_ENTRY_POOL) - bounded per thread, a give on another thread or out of order safe by
// construction, freed at thread exit, and a reused Impl reset to EXACTLY a value-initialised one.
//
// Run: build/difftest.exe --entry-pool-test     (exit = failed checks; < 1 s)
// The leaves' bytes with reused Impls are checked by the native suites (they run every Entry through the pool now) and
// the live SR_NATIVE_FLUSH_CHECK roll; this test checks the list itself and the reset's completeness, with the MUTANT:
// each reset statement skipped in turn (entryPoolSkipResetForTest 0..6) must be caught by the SR_ORACLE state check.
// Mutation / attribution: SR_ENTRY_POOL=0 (or entryPoolForceForTest(0)) - every take a miss, every give a delete.
// N4 (NEWPAIR_CHECKS section 4; a CMake SR_ASAN_TESTS=ON build): a parked Impl is poisoned, so the negative controls
//   build/difftest.exe --entry-pool-test touch-after-give         (a use after a give on this thread)
//   build/difftest.exe --entry-pool-test touch-after-cross-give   (a use after a give on another thread)
// must each STOP with an ASan use-after-poison report; one that reaches its end prints NOT CAUGHT and exits 3.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/native_flush.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace F = sr::native::flush;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

// `n` device-less Entries alive at once on this thread (the constructor takes an Impl and only stores ctx / base /
// dev; nothing is opened), released innermost first.
void nest(int n) {
  PPCContext c{};
  std::vector<std::unique_ptr<F::Entry>> es;
  for (int i = 0; i < n; i++) es.push_back(std::make_unique<F::Entry>(c, nullptr, 0u));
  while (!es.empty()) es.pop_back();
}

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAsan = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAsan = true;
#else
constexpr bool kAsan = false;
#endif
#else
constexpr bool kAsan = false;
#endif

// N4's negative controls, one per process (an ASan report ends it): 3 = the touch returned (NOT CAUGHT), 2 = usage or
// not set up (nothing touched).
int runTouchControl(const char* mode) {
  const bool cross = std::strcmp(mode, "touch-after-cross-give") == 0;
  if (!cross && std::strcmp(mode, "touch-after-give") != 0) {
    std::printf("[ENTRY_POOL_TEST] usage: --entry-pool-test [touch-after-give | touch-after-cross-give]\n");
    return 2;
  }
#if SR_ORACLE
  F::entryPoolForceForTest(1);
  std::printf("[ENTRY_POOL_TEST] N4 negative control %s (asan=%s): the touch must stop with an ASan use-after-poison "
              "report\n", mode, kAsan ? "on" : "off");
  std::fflush(stdout);
  const int v = cross ? F::entryPoolTouchAfterCrossGiveForTest() : F::entryPoolTouchAfterGiveForTest();
  if (v < 0) {
    std::printf("[ENTRY_POOL_TEST] %s: SETUP FAILED - the Impl was not parked (nothing touched)\n", mode);
    return 2;
  }
  std::printf("[ENTRY_POOL_TEST] NOT CAUGHT: %s touched a parked Impl and returned (byte 0x%02x) - %s\n", mode, v,
              kAsan ? "ASan did not stop it: N4 FAILS" : "no ASan in this build (the control needs SR_ASAN_TESTS=ON)");
  return 3;
#else
  std::printf("[ENTRY_POOL_TEST] %s needs the SR_ORACLE seams (a release build has none)\n", mode);
  return 2;
#endif
}
}  // namespace

int runEntryPoolTest(const char* mode) {
  if (mode) return runTouchControl(mode);
  std::printf("[ENTRY_POOL_TEST] native_flush Entry::Impl free list (step 6 I6, SR_ENTRY_POOL)\n");
  std::printf("[ENTRY_POOL_TEST] asan=%s (step 6 N4: a parked Impl is %s)\n", kAsan ? "on" : "off",
              kAsan ? "poisoned" : "not poisoned");
#if SR_ORACLE
  // the env's =0 arm is honoured (the A/B step: P0 only); otherwise the pool is forced on for P1-P8
  const char* env = std::getenv("SR_ENTRY_POOL");
  if (!(env && env[0] == '0')) F::entryPoolForceForTest(1);
#endif
  if (!F::entryPoolOn()) {
    std::printf("[ENTRY_POOL_TEST] SR_ENTRY_POOL=0: the A/B arm - every take a miss, every give a delete\n");
    const auto a0 = F::entryPoolCounters();
    nest(3);
    const auto a1 = F::entryPoolCounters();
    check(a1.takes - a0.takes == 3 && a1.misses - a0.misses == 3 && a1.hits == a0.hits && a1.pushes == a0.pushes,
          "P0 pool off: 3 takes = 3 misses, no hit, no push (today's heap path)");
    std::printf("[ENTRY_POOL_TEST] passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail;
  }

  // P1: the first Entry on a fresh thread state misses; the next reuses it (a hit, no allocation)
  nest(1);  // warm: at least one Impl cached on this thread
  auto c0 = F::entryPoolCounters();
  nest(1);
  auto c1 = F::entryPoolCounters();
  check(c1.takes - c0.takes == 1 && c1.hits - c0.hits == 1 && c1.misses == c0.misses && c1.pushes - c0.pushes == 1,
        "P1 an Entry after another on the same thread reuses the Impl (hit, no miss)");

  // P2: bounded - 5 Entries alive at once: the list holds kCap (4) afterwards; the 5th give deletes (overflow)
  nest(4);  // fill the list to 4
  c0 = F::entryPoolCounters();
  nest(5);
  c1 = F::entryPoolCounters();
  check(c1.hits - c0.hits == 4 && c1.misses - c0.misses == 1 && c1.overflowDeletes - c0.overflowDeletes == 1 &&
            c1.pushes - c0.pushes == 4,
        "P2 5 nested Entries: 4 hits + 1 miss, then 4 pushes + 1 overflow delete (bounded at kCap 4)");
#if SR_ORACLE
  check(F::entryPoolCachedForTest() == 4, "P2b the list holds exactly kCap (4)");

  // P3: cross-thread - an Impl taken here and given on another thread lands on THAT thread's list, counted; this
  // thread's list is untouched by the give
  int other = -9;
  uint64_t cross = 0;
  const bool mineSame = F::entryPoolCrossThreadForTest(other, cross);
  check(mineSame && other == 1 && cross == 1,
        "P3 a give on another thread: on that thread's list (1), counted once, this list unchanged");

  // P4: thread exit - a thread that cached 4 Impls frees them in its list's destructor
  const uint64_t freed = F::entryPoolThreadExitForTest(6);
  check(freed == 4, "P4 a thread with 4 cached Impls frees 4 at exit (6 nested: 2 overflow deletes, 4 cached)");

  // P5: the reset is complete - a used, poisoned Impl comes back exactly value-initialised
  bool complete = false;
  const bool reused = F::entryPoolResetCheckForTest(-1, complete);
  check(reused && complete, "P5 a reused (poisoned) Impl is reset to the value-initialised state");

  // P6: the MUTANT - each reset statement skipped in turn must be caught by the state check
  static const char* const kField[7] = {"dr", "opened", "locked", "atMode", "atCursor0", "payload", "span"};
  int caught = 0;
  for (int k = 0; k < 7; k++) {
    bool done = true;
    const bool r = F::entryPoolResetCheckForTest(k, done);
    const bool ok = r && !done;
    caught += ok ? 1 : 0;
    std::printf("  %s  P6 mutant: reset() skips %s -> %s\n", ok ? "ok " : "FAIL", kField[k],
                ok ? "caught (state differs from value-initialised)" : (r ? "NOT CAUGHT" : "Impl not reused"));
  }
  check(caught == 7, "P6 every skipped reset statement is caught (7 / 7)");

  // P7: the gate - with the pool forced off, takes miss and gives delete (the A/B arm's path)
  F::entryPoolForceForTest(0);
  c0 = F::entryPoolCounters();
  nest(3);
  c1 = F::entryPoolCounters();
  F::entryPoolForceForTest(1);
  check(c1.misses - c0.misses == 3 && c1.hits == c0.hits && c1.pushes == c0.pushes,
        "P7 SR_ENTRY_POOL off: 3 takes = 3 misses, no hit, no push");
  F::entryPoolForceForTest(-1);
#else
  std::printf("  --  P2b-P7 need the SR_ORACLE seams (release build: the list's counters only)\n");
#endif
  const auto e = F::entryPoolCounters();
  std::printf("[ENTRY_POOL_TEST] counters takes=%llu hits=%llu misses=%llu pushes=%llu overflow=%llu cross=%llu "
              "exit_frees=%llu threads=%llu reset_bad=%llu\n",
              (unsigned long long)e.takes, (unsigned long long)e.hits, (unsigned long long)e.misses,
              (unsigned long long)e.pushes, (unsigned long long)e.overflowDeletes, (unsigned long long)e.crossThreadGives,
              (unsigned long long)e.exitFrees, (unsigned long long)e.threads, (unsigned long long)e.resetBad);
  check(e.hits + e.misses == e.takes, "P8 hits + misses = takes");
  std::printf("[ENTRY_POOL_TEST] passed=%d failed=%d\n", g_pass, g_fail);
  return g_fail;
}
