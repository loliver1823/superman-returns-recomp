// iter-458 (Astra 017): SR_HLE_ARENA_WAIT staging backpressure - exhaustion,
// release, no-progress, timeout, invalid input and the render-thread refusal,
// driven through the REAL public writers and release functions the producers
// call (poolWrite/poolRelease, texWrite/texRelease, classAWrite/classARelease).
//
// Run: build/difftest.exe --arena-wait-test   (exit code = failed checks)
//
// Test seams (hle_video.hpp *ForTest): the gate override, the fail hook (the
// production fail action terminates the process, which a test cannot observe
// in-process) and the pending-work probe (no render thread runs here, so the
// test states whether consumer work is pending). Everything else is production.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "gfx/hle_video.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace H = sr::gfx::hle;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
bool g_pending = false;
bool pendingProbe() { return g_pending; }
std::string g_failReason;
int g_failCalls = 0;
void failHook(const char* reason, const char*, uint32_t, uint64_t) {
  g_failCalls++;
  g_failReason = reason;
}
using Clock = std::chrono::steady_clock;
long long msSince(Clock::time_point t0) {
  return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}
}  // namespace

int runArenaWaitTest() {
  std::printf("[ARENA_WAIT_TEST] SR_HLE_ARENA_WAIT backpressure\n");
  H::arenaWaitSetFailHookForTest(failHook);
  H::arenaWaitSetPendingProbeForTest(pendingProbe);
  H::arenaWaitSetTimeoutMsForTest(5000);
  H::arenaWaitResetForTest();

  // Arena route: spans larger than a geo slot and at most one arena slot.
  const uint32_t span = H::kGeoSlotBytes + 4096;
  std::vector<uint8_t> src(H::kArenaSlotBytes + 16, 0x5A);
  std::vector<uint32_t> held;

  // Fill the arena with the gate OFF.
  H::arenaWaitSetEnabledForTest(0);
  for (int i = 0; i < 1000; i++) {
    const uint32_t o = H::poolWrite(src.data(), span);
    if (o == H::kArenaFull) break;
    held.push_back(o);
  }
  check(held.size() == H::kArenaSlots, "A0 the arena fills to exactly kArenaSlots via poolWrite");

  // A1 gate OFF: an immediate reject, counted as EXHAUSTED, no wait.
  {
    const auto s0 = H::arenaWaitStats();
    const auto t0 = Clock::now();
    const uint32_t o = H::poolWrite(src.data(), span);
    const long long ms = msSince(t0);
    const auto s1 = H::arenaWaitStats();
    check(o == H::kArenaFull && ms < 50, "A1a gate OFF: a full pool rejects immediately");
    check(s1.exhausted[2] == s0.exhausted[2] + 1 && s1.invalid[2] == s0.invalid[2],
          "A1b ...counted as arena EXHAUSTED, not invalid");
    check(s1.waits == s0.waits && g_failCalls == 0, "A1c ...no wait, no failure");
  }

  H::arenaWaitSetEnabledForTest(1);

  // A2 invalid input with the gate ON: immediate, counted INVALID, never waits.
  {
    const auto s0 = H::arenaWaitStats();
    const auto t0 = Clock::now();
    const uint32_t o = H::poolWrite(src.data(), H::kArenaSlotBytes + 1);
    const long long ms = msSince(t0);
    const auto s1 = H::arenaWaitStats();
    check(o == H::kArenaFull && ms < 50, "A2a gate ON: an oversize request rejects immediately");
    check(s1.invalid[2] == s0.invalid[2] + 1 && s1.waits == s0.waits && g_failCalls == 0,
          "A2b ...counted INVALID, no wait, no run invalidation");
  }

  // A3 gate ON, pending work, a release arrives: the producer waits and gets it.
  {
    g_pending = true;
    const auto s0 = H::arenaWaitStats();
    const uint32_t victim = held.back(); held.pop_back();
    std::thread rel([victim] {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      H::poolRelease(victim);
    });
    const auto t0 = Clock::now();
    const uint32_t o = H::poolWrite(src.data(), span);
    const long long ms = msSince(t0);
    rel.join();
    const auto s1 = H::arenaWaitStats();
    check(o != H::kArenaFull, "A3a gate ON: the producer is granted the released slot");
    check(ms >= 50, "A3b ...only after the release (it WAITED, did not reject)");
    check(s1.waits == s0.waits + 1 && s1.waitsOk == s0.waitsOk + 1 && g_failCalls == 0,
          "A3c ...counted as one successful wait, no failure");
    if (o != H::kArenaFull) held.push_back(o);
  }

  // A4 gate ON, pool full, NOTHING pending: fail at once (no-progress), never hang.
  {
    g_pending = false;
    g_failCalls = 0; g_failReason.clear();
    const auto t0 = Clock::now();
    const uint32_t o = H::poolWrite(src.data(), span);
    const long long ms = msSince(t0);
    check(o == H::kArenaFull && g_failCalls == 1 && g_failReason == "no-progress",
          "A4a no pending work: the wait ends as no-progress and invalidates the run");
    check(ms < 1000, "A4b ...promptly (well before the 5 s timeout)");
  }

  // A5 gate ON, pending work that never releases: the hard timeout fires.
  {
    g_pending = true;
    g_failCalls = 0; g_failReason.clear();
    H::arenaWaitSetTimeoutMsForTest(300);
    const auto t0 = Clock::now();
    const uint32_t o = H::poolWrite(src.data(), span);
    const long long ms = msSince(t0);
    H::arenaWaitSetTimeoutMsForTest(5000);
    check(o == H::kArenaFull && g_failCalls == 1 && g_failReason == "timeout",
          "A5a pending but never released: the timeout invalidates the run");
    check(ms >= 290 && ms < 3000, "A5b ...after the configured timeout, not before");
  }

  // A6 the render thread (the only consumer) never waits.
  {
    g_pending = true;
    g_failCalls = 0;
    const auto s0 = H::arenaWaitStats();
    H::arenaWaitMarkRenderThreadForTest(true);
    const auto t0 = Clock::now();
    const uint32_t o = H::poolWrite(src.data(), span);
    const long long ms = msSince(t0);
    H::arenaWaitMarkRenderThreadForTest(false);
    const auto s1 = H::arenaWaitStats();
    check(o == H::kArenaFull && ms < 50, "A6a the render thread gets the legacy reject at once");
    check(s1.renderRefusals == s0.renderRefusals + 1 && s1.waits == s0.waits && g_failCalls == 0,
          "A6b ...counted as a render-thread refusal, no wait");
  }

  for (uint32_t o : held) H::poolRelease(o);
  held.clear();

  // A7 the texture pool through texWrite/texRelease.
  {
    H::arenaWaitSetEnabledForTest(0);
    std::vector<uint8_t> tsrc(4096, 0x33);
    std::vector<uint32_t> th;
    for (int i = 0; i < 100; i++) {
      const uint32_t o = H::texWrite(tsrc.data(), (uint32_t)tsrc.size());
      if (o == H::kArenaFull) break;
      th.push_back(o);
    }
    check(th.size() == H::kTexSlotCount, "A7a the texture pool fills to kTexSlotCount");
    H::arenaWaitSetEnabledForTest(1);
    g_pending = true;
    g_failCalls = 0;
    const uint32_t victim = th.back(); th.pop_back();
    std::thread rel([victim] {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      H::texRelease(victim);
    });
    const auto t0 = Clock::now();
    const uint32_t o = H::texWrite(tsrc.data(), (uint32_t)tsrc.size());
    const long long ms = msSince(t0);
    rel.join();
    check(o != H::kArenaFull && ms >= 50 && g_failCalls == 0,
          "A7b texWrite waits for texRelease and is granted the slot");
    if (o != H::kArenaFull) th.push_back(o);
    for (uint32_t t : th) H::texRelease(t);
  }

  // A8 the Class-A pool (byte budget) through classAWrite/classARelease.
  {
    H::arenaWaitSetEnabledForTest(0);
    const uint32_t nb = H::kClassAMaxBytes;
    std::vector<uint8_t> csrc(nb, 0x44);
    std::vector<uint32_t> ch;
    for (int i = 0; i < 64; i++) {
      const uint32_t o = H::classAWrite(csrc.data(), nb);
      if (o == H::kArenaFull) break;
      ch.push_back(o);
    }
    const auto s0 = H::arenaWaitStats();
    check(!ch.empty() && s0.exhausted[4] >= 1, "A8a the Class-A budget exhausts (counted EXHAUSTED)");
    H::arenaWaitSetEnabledForTest(1);
    g_pending = true;
    g_failCalls = 0;
    const uint32_t victim = ch.back(); ch.pop_back();
    std::thread rel([victim, nb] {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      H::classARelease(victim, nb);
    });
    const auto t0 = Clock::now();
    const uint32_t o = H::classAWrite(csrc.data(), nb);
    const long long ms = msSince(t0);
    rel.join();
    check(o != H::kArenaFull && ms >= 50 && g_failCalls == 0,
          "A8b classAWrite waits for classARelease and is granted the budget");
    if (o != H::kArenaFull) ch.push_back(o);
    for (uint32_t h : ch) H::classARelease(h, nb);
  }

  // A9 (Astra 018) the SMALL-BLOCK pool through poolWrite/poolRelease: spans at
  // most kBlockBytes route to blocks; exhaustion is counted per pool and waits.
  {
    H::arenaWaitSetEnabledForTest(0);
    std::vector<uint8_t> bsrc(1024, 0x66);
    std::vector<uint32_t> bh;
    for (uint32_t i = 0; i < H::kBlockCount + 16; i++) {
      const uint32_t o = H::poolWrite(bsrc.data(), (uint32_t)bsrc.size());
      if (o == H::kArenaFull) break;
      bh.push_back(o);
    }
    const auto s0 = H::arenaWaitStats();
    check(bh.size() == H::kBlockCount && s0.exhausted[0] >= 1,
          "A9a the block pool fills to kBlockCount; the next is BLOCK-exhausted");
    H::arenaWaitSetEnabledForTest(1);
    g_pending = true;
    g_failCalls = 0;
    const uint32_t victim = bh.back(); bh.pop_back();
    std::thread rel([victim] {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      H::poolRelease(victim);
    });
    const auto t0 = Clock::now();
    const uint32_t o = H::poolWrite(bsrc.data(), (uint32_t)bsrc.size());
    const long long ms = msSince(t0);
    rel.join();
    check(o != H::kArenaFull && ms >= 50 && g_failCalls == 0,
          "A9b a block-route poolWrite waits for the block release and is granted");
    if (o != H::kArenaFull) bh.push_back(o);
    for (uint32_t b : bh) H::poolRelease(b);
  }

  // A10 (Astra 018) the GEOMETRY route: a full geo pool FALLS BACK to the arena
  // (no wait); only when both are full does the producer wait, and a geo release
  // satisfies it.
  {
    H::arenaWaitSetEnabledForTest(0);
    const uint32_t gspan = H::kBlockBytes + 512;   // geo-sized
    std::vector<uint8_t> gsrc(gspan, 0x77);
    std::vector<uint32_t> gh, ah;
    for (uint32_t i = 0; i < H::kGeoSlots + H::kArenaSlots + 8; i++) {
      const uint32_t o = H::poolWrite(gsrc.data(), gspan);
      if (o == H::kArenaFull) break;
      if (o & H::kPoolGeoFlag) gh.push_back(o); else ah.push_back(o);
    }
    check(gh.size() == H::kGeoSlots && ah.size() == H::kArenaSlots,
          "A10a geo-sized spans fill geo, then FALL BACK to the arena until both are full");
    H::arenaWaitSetEnabledForTest(1);
    g_pending = true;
    g_failCalls = 0;
    const uint32_t victim = gh.back(); gh.pop_back();
    std::thread rel([victim] {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      H::poolRelease(victim);
    });
    const auto t0 = Clock::now();
    const uint32_t o = H::poolWrite(gsrc.data(), gspan);
    const long long ms = msSince(t0);
    rel.join();
    check(o != H::kArenaFull && (o & H::kPoolGeoFlag) && ms >= 50 && g_failCalls == 0,
          "A10b with both full the producer waits; the geo release grants a GEO slot");
    if (o != H::kArenaFull) gh.push_back(o);
    for (uint32_t g : gh) H::poolRelease(g);
    for (uint32_t a : ah) H::poolRelease(a);
  }

  H::arenaWaitSetEnabledForTest(-1);
  H::arenaWaitSetFailHookForTest(nullptr);
  H::arenaWaitSetPendingProbeForTest(nullptr);
  std::printf("[ARENA_WAIT_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
