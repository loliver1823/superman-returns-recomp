// iter-458 H-LIFO + H-UNIQ (Astra 058): the LIFO physical free
// (guestPhysLifoFree, SR_PHYS_LIFO_FREE) and the physical-uniqueness census
// (physwin::uniqSnapshot, SR_PHYS_UNIQ_CENSUS) - both through the REAL
// allocator, and the free through the REAL MmFreePhysicalMemory export.
//
// Run: build/difftest.exe --phys-lifo-test   (exit code = failed checks)
//      build/difftest.exe --phys-uniq-test
// Both require SR_PHYS_FREE unset (H-LIFO is inert under H-FREE).
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/phys_alloc.hpp"
#include "kernel/phys_lifo.hpp"   // iter-463 reuse + floor
#include "kernel/phys_window.hpp"
#include "kernel/stubs.hpp"
#include "gfx/hle_video.hpp"   // Astra 063: the recorder-close path (seal endpoint)

PPC_EXTERN_FUNC(sub_82468290);   // the SIMD memcpy guard override (kernel/simd_memcpy_null_guard.cpp)
namespace sr { namespace kernel { void setMemcpyGuardTopFixForTest(int mode); } }

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sr { namespace kernel { void guestPhysReserveAlias(uint32_t va, uint32_t size); } }  // iter-462
namespace K = sr::kernel;
namespace W = sr::kernel::physwin;
using LF = sr::kernel::PhysLifoFree;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
  // One whole line per write: the kernel logger shares this pipe, and a
  // buffered flush boundary could otherwise split a FAIL line around its output.
  std::fflush(stdout);
}
struct Req { size_t size; uint32_t align; };
// guestPhysAlloc's own normalisation, for the expected reservation.
size_t reserveOf(const Req& r) {
  uint32_t a = r.align;
  if (a < 16u || (a & (a - 1u)) != 0u) a = 16u;
  return ((r.size + 15) & ~size_t(15)) + a;
}
// R8: the post-free hook re-grants through the real export, once.
sr::kernel::ExportFn g_hookAlloc = nullptr;
uint32_t g_hookSize = 0, g_hookGot = 0;
void regrantHook(uint32_t) {
  if (!g_hookAlloc || !g_hookSize) return;
  const uint32_t sz = g_hookSize;
  g_hookSize = 0;
  g_hookGot = g_hookAlloc(0, sz, 4u, 0u, 0xFFFFFFFFu, 0u);
}
}  // namespace

int runPhysLifoTest() {
  std::printf("[PHYS_LIFO_TEST] LIFO physical free\n");
  g_pass = g_fail = 0;
  K::setPhysXeniaPlaceForTest(0);   // iter-465: these pin the legacy size+align placement
  check(!K::physalloc::physFreeEnabled(), "L00 SR_PHYS_FREE is off (H-LIFO is inert under it)");

  // ── gate: default ON since iter-460 (1f0813e), and OFF means no effect ────
  K::setPhysLifoForTest(-1);
  check(K::physLifoEnabled(), "L01 SR_PHYS_LIFO_FREE defaults ON");
  K::setPhysLifoForTest(0);
  {
    const uint32_t a = K::guestPhysAlloc(0x3000, 0x1000);
    const size_t c = K::guestPhysNextForTest();
    check(K::guestPhysLifoFree(a) == LF::kNotEnabled && K::guestPhysNextForTest() == c,
          "L02 gate OFF: a free is not-enabled and the cursor does not move");
  }

  // ── E: gate-OFF equivalence + exact rollback ─────────────────────────────
  const std::vector<Req> S = {{0x3000, 0x1000}, {0x90, 16},   {0x5000, 0x10000},
                              {0x1234, 0x2000}, {0x800, 0x1000}, {0x10, 3}};
  {
    K::setPhysLifoForTest(1);
    const size_t c0 = K::guestPhysNextForTest();
    std::vector<uint32_t> v1;
    size_t sumRes = 0;
    for (const Req& r : S) { v1.push_back(K::guestPhysAlloc(r.size, r.align)); sumRes += reserveOf(r); }
    check(K::guestPhysNextForTest() == c0 + sumRes, "E1 gate ON reserves exactly size+align per grant");
    bool allRec = true;
    for (size_t i = v1.size(); i-- > 0;) allRec &= K::guestPhysLifoFree(v1[i]) == LF::kReclaimed;
    check(allRec, "E2 freeing in reverse order reclaims every grant");
    check(K::guestPhysNextForTest() == c0, "E3 cursor back to the pre-reservation value (padding included)");
    K::setPhysLifoForTest(0);
    std::vector<uint32_t> v2;
    for (const Req& r : S) v2.push_back(K::guestPhysAlloc(r.size, r.align));
    check(v1 == v2, "E4 the gate-OFF path grants the SAME VAs from the same cursor");
    check(K::guestPhysNextForTest() == c0 + sumRes, "E5 gate-OFF reservation arithmetic identical");
  }

  // ── T: top free reclaims; the identical request gets the identical VA ─────
  K::setPhysLifoForTest(1);
  {
    const size_t c0 = K::guestPhysNextForTest();
    const uint32_t a = K::guestPhysAlloc(0x4000, 0x10000);
    check(K::guestPhysNextForTest() == c0 + 0x4000 + 0x10000, "T1 reservation includes the 64 KB padding");
    check(K::guestPhysLifoFree(a) == LF::kReclaimed && K::guestPhysNextForTest() == c0,
          "T2 top free reclaimed to the recorded cursor");
    const uint32_t a2 = K::guestPhysAlloc(0x4000, 0x10000);
    check(a2 == a, "T3 reclaimed capacity is reused: same request -> same VA");
    check(K::guestPhysLifoFree(a2) == LF::kReclaimed, "T4 a re-granted VA carries a fresh record");
    check(K::guestPhysLifoFree(a2) == LF::kDuplicate && K::guestPhysNextForTest() == c0,
          "T5 a second free of a reclaimed grant is a duplicate; cursor unchanged");
  }

  // ── N: intervening grant -> non-top free rejected and RETAINED ────────────
  // The iter-458 contract, with the iter-463 reuse gate OFF (SR_PHYS_LIFO_REUSE=0);
  // the reuse contract is --phys-lifo-reuse-test.
  K::setPhysLifoReuseForTest(0);
  {
    const size_t c0 = K::guestPhysNextForTest();
    const uint32_t a = K::guestPhysAlloc(0x2000, 0x1000);
    const size_t cA = K::guestPhysNextForTest();
    const uint32_t b = K::guestPhysAlloc(0x3000, 0x1000);
    const size_t cB = K::guestPhysNextForTest();
    check(K::guestPhysLifoFree(a) == LF::kNotTop && K::guestPhysNextForTest() == cB,
          "N1 a non-top free is rejected; the cursor does not move");
    check(K::guestPhysLifoFree(a) == LF::kDuplicate, "N2 freeing the retained grant again is a duplicate");
    const uint32_t c = K::guestPhysAlloc(0x1000, 0x1000);
    check(c + 0x1000 <= b && !(c < a + 0x2000 && a < c + 0x1000),
          "N3 the retained grant is not re-granted");
    check(K::guestPhysLifoFree(c) == LF::kReclaimed && K::guestPhysLifoFree(b) == LF::kReclaimed,
          "N4 the grants above it still reclaim in LIFO order");
    check(K::guestPhysNextForTest() == cA,
          "N5 no cascade: the cursor stops at the retained grant's reservation");
    (void)c0;
  }
  K::setPhysLifoReuseForTest(-1);
  K::setPhysLifoForTest(1);   // iter-463: re-origin (the N records left a gap reuse-OFF never lists)

  // ── U: unknown frees ──────────────────────────────────────────────────────
  {
    const uint32_t a = K::guestPhysAlloc(0x2000, 0x1000);
    const size_t c = K::guestPhysNextForTest();
    check(K::guestPhysLifoFree(a + 0x1000) == LF::kUnknown, "U1 an interior pointer is unknown");
    check(K::guestPhysLifoFree(0x82B00000u) == LF::kUnknown, "U2 a heap VA is unknown");
    check(K::guestPhysLifoFree(0) == LF::kUnknown, "U3 null is unknown");
    check(K::guestPhysNextForTest() == c, "U4 unknown frees never move the cursor");
    check(K::guestPhysLifoFree(a) == LF::kReclaimed, "U5 ...and the real top still reclaims after them");
  }

  // ── C: concurrency - live grants stay pairwise disjoint ──────────────────
  {
    constexpr int kThreads = 8, kIters = 4000;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> live(kThreads);
    std::atomic<uint64_t> reclaimed{0}, notTop{0}, bad{0};
    std::vector<std::thread> th;
    uint8_t* const base = K::getGuestBase();
    // C0 (Astra 059): a section that never completes is a NAMED failure, not a
    // silent hang - the watchdog reports it and ends the process.
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
      for (int ms = 0; ms < 120000 && !done.load(); ms += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (!done.load()) {
        std::printf("FAIL: C0 the concurrency section completes within 120 s\n");
        std::printf("[PHYS_LIFO_TEST] %d passed, %d failed\n", g_pass, g_fail + 1);
        std::fflush(stdout);
        std::_Exit(3);
      }
    });
    for (int t = 0; t < kThreads; t++) {
      th.emplace_back([&, t] {
        K::setGuestMemory(base, 0x100000000ull);   // the guest base is per-thread (TLS)
        std::mt19937 rng(1234u + t);
        std::vector<std::pair<uint32_t, uint32_t>> mine;
        for (int i = 0; i < kIters; i++) {
          if (!mine.empty() && (rng() & 1u)) {
            const auto g = mine.back();
            mine.pop_back();
            const LF r = K::guestPhysLifoFree(g.first);
            if (r == LF::kReclaimed) reclaimed++;
            // iter-463: with SR_PHYS_LIFO_REUSE a retained grant's pages are free
            // for the next grant - only a reuse-OFF retained grant still holds them.
            else if (r == LF::kNotTop) { notTop++; if (!K::physLifoReuseEnabled()) live[t].push_back(g); }
            else bad++;
          } else {
            const uint32_t sz = 0x10u << (rng() % 10);
            const uint32_t al = 16u << (rng() % 9);
            const uint32_t va = K::guestPhysAlloc(sz, al);
            if (!va) { bad++; continue; }
            if (va & (al - 1u)) bad++;
            mine.push_back({va, sz});
          }
        }
        for (const auto& g : mine) live[t].push_back(g);
      });
    }
    for (auto& x : th) x.join();
    done = true;
    watchdog.join();
    check(true, "C0 the concurrency section completes within 120 s");
    std::vector<std::pair<uint32_t, uint32_t>> all;
    for (auto& v : live) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    bool disjoint = true;
    for (size_t i = 1; i < all.size(); i++)
      if (all[i - 1].first + all[i - 1].second > all[i].first) disjoint = false;
    uint32_t plo = 0, phi = 0;
    K::guestPhysRegion(plo, phi);
    const bool inRegion = all.empty() || all.front().first >= plo;
    std::printf("      (C: %llu reclaimed, %llu not-top, %zu live)\n",
                (unsigned long long)reclaimed.load(), (unsigned long long)notTop.load(), all.size());
    check(bad == 0, "C1 no failed grant, misaligned grant, unknown or duplicate free");
    check(reclaimed > 0 && notTop > 0, "C2 both outcomes exercised under contention");
    check(disjoint, "C3 every live/retained grant is pairwise disjoint after concurrent LIFO frees");
    check(inRegion, "C4 every live grant lies inside the cursor's region");
    if (K::physLifoReuseEnabled()) {   // reuse OFF: retained grants are gaps by design
      std::string why;
      const bool inv = K::physLifoCheckForTest(&why);
      if (!inv) std::printf("      (C5: %s)\n", why.c_str());
      check(inv, "C5 iter-463: after 32000 concurrent grants/frees the live reservations and free extents "
                 "still partition [origin, physNext) exactly");
    }
  }

  // ── R: the REAL exports - the overcommit pair (Xenia oracle sequence) ────
  K::registerStubs();
  auto& reg = K::ExportRegistry::instance();
  auto mmAlloc = reg.getExport("MmAllocatePhysicalMemoryEx");
  auto mmFree = reg.getExport("MmFreePhysicalMemory");
  check(mmAlloc && mmFree, "R0 both exports registered");
  if (mmAlloc && mmFree) {
    W::resetForTest();
    W::setUniqCensusForTest(1);
    // ReserveMemoryWithFallback_sub_82167FA8 (0x12B00000), freed by
    // GpuCreateBufferResourcePool_sub_82183DC8; then the 224 MB world pool.
    const size_t c0 = K::guestPhysNextForTest();
    const uint32_t R = mmAlloc(0, 0x12B00000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    check(R >= 0xA0000000u, "R1 the 299 MB reservation is granted in the 0xA window");
    mmFree(0, R, 0, 0, 0, 0);
    check(K::guestPhysNextForTest() == c0, "R2 MmFreePhysicalMemory reclaimed it (cursor restored)");
    const uint32_t Wp = mmAlloc(0, 0x0DFF0000u, 0x20000404u, 0u, 0xFFFFFFFFu, 0x8000u);
    check(Wp >= R && Wp + 0x0DFF0000u <= R + 0x12B00000u,
          "R3 the world pool lands INSIDE the reclaimed reservation (as on Xenia)");
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.pairsLiveLive == 0 && u.physGrantsBelowA == 0 && u.grantsLive == 1 && u.grantsReleased == 0,
          "R4 census: the reclaimed grant left the registry; one live grant, no wrap, no overlap");
    check(u.physRegionLo >= 0xA0000000u && u.physRegionLo <= Wp,
          "R4b the bump region itself stays above 0xA0000000 (no wrap on any route)");

    // R8 (Astra 059): the race point. The hook runs inside MmFreePhysicalMemory
    // right after the allocator reclaimed X - where another thread could re-grant
    // X and register it. The re-grant must survive the old free's bookkeeping.
    W::resetForTest();
    const uint32_t X = mmAlloc(0, 0x10000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    g_hookAlloc = mmAlloc;
    g_hookSize = 0x10000u;
    g_hookGot = 0;
    K::setPhysLifoPostFreeHookForTest(&regrantHook);
    mmFree(0, X, 0, 0, 0, 0);
    K::setPhysLifoPostFreeHookForTest(nullptr);
    check(g_hookGot == X, "R8a the hook re-granted the reclaimed VA at the race point");
    check(W::ownerOfGrant(X, 0x10000u) == W::kOwnPhysRegion && W::uniqSnapshot().grantsLive == 1,
          "R8 a re-grant registered at the race point survives the old free's bookkeeping");
    mmFree(0, X, 0, 0, 0, 0);
    check(W::uniqSnapshot().grantsLive == 0, "R8b ...and its own free then removes it");

    // R9: a non-top free through the export is RELEASED (retained) in the registry.
    const uint32_t A1 = mmAlloc(0, 0x10000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    const uint32_t B1 = mmAlloc(0, 0x10000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    mmFree(0, A1, 0, 0, 0, 0);
    {
      const W::UniqSnapshot u9 = W::uniqSnapshot();
      check(u9.grantsLive == 1 && u9.grantsReleased == 1 && W::ownerOfGrant(A1, 0x1000) == W::kOwnPhysRegion,
            "R9 a non-top export free leaves the grant RELEASED (still the owner of its retained pages)");
    }
    mmFree(0, B1, 0, 0, 0, 0);

    // The same sequence with the gate OFF: the stub keeps the reservation.
    K::setPhysLifoForTest(0);
    W::resetForTest();
    const uint32_t R2 = mmAlloc(0, 0x12B00000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    const size_t c1 = K::guestPhysNextForTest();
    mmFree(0, R2, 0, 0, 0, 0);
    check(K::guestPhysNextForTest() == c1, "R5 gate OFF: the free keeps the reservation");
    const uint32_t W2 = mmAlloc(0, 0x0DFF0000u, 0x20000404u, 0u, 0xFFFFFFFFu, 0x8000u);
    check(W2 + 0x0DFF0000u <= R2, "R6 gate OFF: the world pool lands BELOW the kept reservation");
    check(W::uniqSnapshot().physRegionLo <= W2, "R6b physRegionLo tracks the bump cursor");
    const W::UniqSnapshot u2 = W::uniqSnapshot();
    check(u2.grantsReleased == 1 && u2.grantsLive == 1,
          "R7 census-only: the freed reservation is RELEASED (retained), not forgotten");
    W::setUniqCensusForTest(-1);
    W::resetForTest();
  }
  K::setPhysLifoForTest(-1);
  std::printf("[PHYS_LIFO_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

int runPhysUniqTest() {
  std::printf("[PHYS_UNIQ_TEST] physical uniqueness census\n");
  g_pass = g_fail = 0;
  K::setPhysXeniaPlaceForTest(0);   // iter-465: these pin the legacy size+align placement
  W::setUniqCensusForTest(-1);
  check(!W::uniqCensusEnabled(), "Q00 SR_PHYS_UNIQ_CENSUS defaults OFF");
  W::setUniqCensusForTest(1);
  W::resetForTest();

  // Disjoint grants on every route: exact zero.
  W::notePhysGrant(0xB0000000u, 0x2000);
  W::noteHeapRoutedGrant(0x80001000u, 0x1000);      // p 0x00001000
  W::noteStackGrant(0xB0100000u, 0x40000);
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.grantsLive == 3 && u.pairsLiveLive == 0 && u.bytesLiveLive == 0 &&
              u.pairsLiveReleased == 0 && u.pairsReleasedReleased == 0,
          "Q01 disjoint grants on all three routes: exactly zero overlap");
  }

  // Heap-routed vs GPU physical on the same physical page.
  W::noteHeapRoutedGrant(0x90000000u, 0x3000);      // p 0x10000000..0x10003000
  W::notePhysGrant(0xB0001000u, 0x1000);            // p 0x10001000 - inside the phys grant at B0000000 too
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.pairsLiveLive == 3 && u.bytesLiveLive == 0x2000 + 0x1000 + 0x1000,
          "Q02 double-booked physical pages counted per pair with exact byte overlap");
    check((u.exKindA == 1 || u.exKindB == 1) && (u.exKindA == 2 || u.exKindB == 2),
          "Q03 the example pair names the heap-routed and the GPU physical owner");
  }

  // Lifetimes: a reclaimed grant leaves; a released one is reported apart.
  W::forgetPhysGrant(0xB0001000u);
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.pairsLiveLive == 1 && u.bytesLiveLive == 0x2000, "Q04 forget removes the grant's overlaps");
  }
  check(W::markPhysGrantReleased(0xB0000000u), "Q05 marking a GPU physical grant released finds it");
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.pairsLiveLive == 0 && u.pairsLiveReleased == 1 && u.grantsReleased == 1,
          "Q06 a released grant's overlap is live-released, not live-live");
  }
  check(W::markPhysGrantReleased(0x90000000u), "Q07 a heap-routed grant can be released too");
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.pairsReleasedReleased == 1 && u.pairsLiveReleased == 0 && u.pairsLiveLive == 0,
          "Q08 both released: released-released");
  }
  check(!W::markPhysGrantReleased(0xB7770000u), "Q09 releasing an unregistered VA reports not-found");

  // Window-boundary crossing: a grant over 0xA0000000 splits into
  // p [0x1FFFF000, 0x20000000) and p [0, 0x1000).
  W::resetForTest();
  W::notePhysGrant(0x9FFFF000u, 0x2000);
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.physGrantsBelowA == 1 && u.lowestPhysGrantVA == 0x9FFFF000u,
          "Q10 a GPU physical grant below 0xA0000000 is counted as wrapped");
  }
  W::noteHeapRoutedGrant(0x80000800u, 0x100);       // p 0x800 - hits the upper split
  W::noteStackGrant(0xBFFFF800u, 0x400);            // p 0x1FFFF800 - hits the lower split
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.pairsLiveLive == 2 && u.bytesLiveLive == 0x100 + 0x400,
          "Q11 both halves of a boundary-crossing grant are compared");
  }

  // Heap bulk: a GPU physical grant aliasing the heap bump's own span.
  W::resetForTest();
  const uint32_t h = K::guestHeapAlloc(0x1000);
  uint32_t hlo = 0, hhi = 0;
  K::guestHeapRegion(hlo, hhi);
  check(h >= hlo && h + 0x1000 <= hhi, "Q12 guestHeapRegion covers a fresh heap grant");
  W::notePhysGrant(0xA0000000u | (h & 0x1FFFFFFFu), 0x1000);
  W::noteHeapRoutedGrant(h, 0x1000);
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.heapBulkPairs == 1 && u.heapBulkBytes == 0x1000,
          "Q13 a physical grant aliasing the heap span is counted (heap-routed ones are not)");
  }

  // ── S: sticky violation tracking + per-kind totals (Astra 059) ────────────
  W::resetForTest();
  check(W::uniqPairIndex(1, 2) == 1 && W::uniqPairIndex(2, 1) == 1 && W::uniqPairIndex(3, 2) == 4 &&
            W::uniqPairIndex(3, 3) == 5 && W::uniqPairIndex(1, 1) == 0,
        "S0 the kind-pair index is symmetric");
  // The existing grant starts BELOW the new one (found only via the predecessor)
  // and lives at another window's alias.
  W::notePhysGrant(0xA0100000u, 0x3000);            // p 0x00100000..0x00103000
  W::noteHeapRoutedGrant(0x80101000u, 0x1000);      // p 0x00101000
  {
    const W::UniqSticky s = W::uniqSticky();
    check(s.llPairs[1] == 1 && s.llBytes[1] == 0x1000 && s.exNew[1].va == 0x80101000u &&
              s.exOld[1].va == 0xA0100000u,
          "S1 a live-live pair is counted when the later grant registers, across window aliases");
  }
  W::forgetPhysGrant(0xA0100000u);
  check(W::uniqSnapshot().pairsLiveLive == 0 && W::uniqSticky().llPairs[1] == 1,
        "S2 a transient overlap stays counted after it disappears");
  W::notePhysGrant(0xA0200000u, 0x1000);
  W::markPhysGrantReleased(0xA0200000u);
  W::noteHeapRoutedGrant(0x80200000u, 0x1000);
  check(W::uniqSticky().llPairs[1] == 1, "S3 a released grant forms no live-live pair");

  W::resetForTest();
  W::noteHeapRoutedGrant(0x80300000u, 0x1000);      // kind 1, p 0x00300000
  W::notePhysGrant(0xA0300000u, 0x1000);            // kind 2, same p       -> 12
  W::noteStackGrant(0xB0400000u, 0x2000);           // kind 3, p 0x10400000
  W::notePhysGrant(0xB0401000u, 0x1000);            // kind 2 inside it     -> 23
  {
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.pairsLL[1] == 1 && u.bytesLL[1] == 0x1000 && u.pairsLL[4] == 1 && u.bytesLL[4] == 0x1000 &&
              u.pairsLL[0] + u.pairsLL[2] + u.pairsLL[3] + u.pairsLL[5] == 0,
          "S4 snapshot totals per kind pair (one 12, one 23, nothing else)");
    const W::UniqSticky s = W::uniqSticky();
    check(s.llPairs[1] == 1 && s.llPairs[4] == 1 && s.registrations == 4 &&
              s.llPairs[0] + s.llPairs[2] + s.llPairs[3] + s.llPairs[5] == 0,
          "S5 sticky totals per kind pair");
  }
  W::noteHeapRoutedGrant(0x80300000u, 0x1000);      // the same key again (replaces the entry)
  check(W::uniqSticky().llPairs[0] == 0 && W::uniqSticky().llPairs[1] == 2,
        "S6 a re-registered key never pairs with the entry it replaces");

  W::notePhysGrant(0xA0000000u, 0x1000);            // exactly AT the boundary
  check(W::uniqSticky().physBelowA == 0, "S7 a grant at 0xA0000000 is not below it");
  W::notePhysGrant(0x9FFF0000u, 0x1000);
  W::noteStackGrant(0x9FF00000u, 0x1000);
  {
    const W::UniqSticky s = W::uniqSticky();
    check(s.physBelowA == 1 && s.stackBelowA == 1 && s.firstBelowAVA == 0x9FFF0000u,
          "S8 grants below 0xA0000000 are counted, by route");
  }
  W::forgetPhysGrant(0x9FFF0000u);
  check(W::uniqSticky().physBelowA == 1, "S8b ...and stay counted after they are gone");

  K::setPhysLifoForTest(1);
  {
    const uint32_t a = K::guestPhysAlloc(0x100000, 0x1000);
    W::notePhysGrant(a, 0x100000);
    uint32_t lo1 = 0, hi1 = 0;
    K::guestPhysRegion(lo1, hi1);
    const uint32_t min1 = W::uniqSticky().minPhysRegionLo;
    check(K::guestPhysLifoFree(a) == LF::kReclaimed, "S9a the big grant is reclaimed");
    const uint32_t b = K::guestPhysAlloc(0x1000, 0x1000);
    W::notePhysGrant(b, 0x1000);
    check(min1 == lo1 && W::uniqSticky().minPhysRegionLo == lo1,
          "S9 minPhysRegionLo is the lowest bound ever, sticky across a reclaim");
    K::guestPhysLifoFree(b);
  }
  K::setPhysLifoForTest(-1);

  W::resetForTest();
  const uint32_t h2 = K::guestHeapAlloc(0x1000);
  W::notePhysGrant(0xA0000000u | (h2 & 0x1FFFFFFFu), 0x1000);
  check(W::uniqSticky().heapSpanPairs == 1 && W::uniqSticky().heapSpanBytes == 0x1000,
        "S10 a phys grant registered into the heap span's physical range is counted at registration");

  W::resetForTest();
  W::noteHeapRoutedGrant(0x80000800u, 0x100);       // p 0x800
  W::noteStackGrant(0xBFFFF800u, 0x400);            // p 0x1FFFF800
  W::notePhysGrant(0x9FFFF000u, 0x2000);            // crosses 0xA0000000
  {
    const W::UniqSticky s = W::uniqSticky();
    check(s.llPairs[1] == 1 && s.llBytes[1] == 0x100 && s.llPairs[4] == 1 && s.llBytes[4] == 0x400,
          "S11 both halves of a boundary-crossing registration are checked");
  }

  // S12 (Astra 060 counterexample): a SHORTER intervening entry of the same
  // registry is the nearer predecessor and would hide an earlier enclosing one.
  W::resetForTest();
  W::notePhysGrant(0xA0500000u, 0x10000);           // A: p 0x00500000..0x00510000
  W::notePhysGrant(0xA0502000u, 0x1000);            // B: inside A (registry not VA-disjoint)
  check(W::uniqSticky().registryOverlaps[1] == 1 && W::uniqSticky().llPairs[3] == 1,
        "S12a an insert overlapping its own registry is counted (and is a 22 pair)");
  W::noteHeapRoutedGrant(0x80508000u, 0x1000);      // overlaps A only; B is the nearer predecessor
  {
    const W::UniqSticky s = W::uniqSticky();
    check(s.llPairs[1] == 1 && s.llBytes[1] == 0x1000 && s.exOld[1].va == 0xA0500000u,
          "S12 an enclosing grant behind a shorter intervening entry is still found");
  }

  // ── D: the durable terminal record (Astra 060) ────────────────────────────
  {
    const char* tmp = std::getenv("TEMP");
    const std::string path = std::string(tmp ? tmp : ".") + "\\sr_uniq_sticky_test.bin";
    std::remove(path.c_str());
    W::resetForTest();
    W::setUniqStickyFileForTest(path.c_str());
    W::notePhysGrant(0xA0600000u, 0x1000);
    W::noteHeapRoutedGrant(0x80600000u, 0x1000);    // one 12 pair
    W::noteStackGrant(0xB0700000u, 0x1000);
    uint8_t live[W::kUniqStickyRecBytes], disk[W::kUniqStickyRecBytes];
    W::serializeUniqSticky(W::uniqSticky(), live);
    W::UniqSticky r;
    uint64_t seq = 0;
    int complete = 0;
    const bool ok = W::readUniqStickyFile(path.c_str(), &r, &seq, &complete);
    W::serializeUniqSticky(r, disk);
    check(ok && seq == 3 && complete == 2 && r.registrations == 3 && r.llPairs[1] == 1 &&
              std::memcmp(live, disk, sizeof(live)) == 0,
          "D1 the durable record equals the live sticky state after the last registration");
    W::tearUniqStickyForTest();
    W::UniqSticky r2;
    uint64_t seq2 = 0;
    int complete2 = 0;
    const bool ok2 = W::readUniqStickyFile(path.c_str(), &r2, &seq2, &complete2);
    W::serializeUniqSticky(r2, disk);
    check(ok2 && seq2 == 3 && complete2 == 1 && std::memcmp(live, disk, sizeof(live)) == 0,
          "D2 a torn (incomplete) newest slot is ignored: the last COMPLETE state is returned");
    W::UniqSticky x;
    x.registrations = 7;
    x.llPairs[5] = 3; x.llBytes[2] = 0x1234;
    x.exNew[4] = W::UniqEx{0xA1u, 0xB2u, 0xC3u}; x.exOld[1] = W::UniqEx{0xD4u, 0xE5u, 0xF6u};
    x.physBelowA = 9; x.stackBelowA = 8; x.firstBelowAVA = 0x9FFF0000u; x.minPhysRegionLo = 0xA0001000u;
    x.heapSpanPairs = 5; x.heapSpanBytes = 0x5000; x.heapSpanBytesMaxTick = 0x6000;
    x.registryOverlaps[2] = 4;
    uint8_t a1[W::kUniqStickyRecBytes], a2[W::kUniqStickyRecBytes];
    W::serializeUniqSticky(x, a1);
    W::serializeUniqSticky(W::deserializeUniqSticky(a1), a2);
    const W::UniqSticky y = W::deserializeUniqSticky(a1);
    check(std::memcmp(a1, a2, sizeof(a1)) == 0 && y.exNew[4].serial == 0xC3u && y.registryOverlaps[2] == 4 &&
              y.minPhysRegionLo == 0xA0001000u && y.llBytes[2] == 0x1234,
          "D3 serialize/deserialize round-trip is exact, every field");
    W::setUniqStickyFileForTest(nullptr);
    std::remove(path.c_str());
  }

  // ── F: fail-closed publication (Astra 061) ────────────────────────────────
  {
    const char* tmp = std::getenv("TEMP");
    const std::string pf = std::string(tmp ? tmp : ".") + "\\sr_uniq_sticky_fail.bin";
    // F3's file is KEPT: tools/uniq_sticky_read.py --integration checks it
    // against this process's own log.
    const std::string p3 = std::string(tmp ? tmp : ".") + "\\sr_uniq_sticky_f3.bin";
    W::UniqSticky r;
    uint64_t seq = 0, fs = 0, ps = 0;
    bool poisoned = false;
    W::resetForTest();
    W::setUniqStickyFileForTest(pf.c_str());
    W::notePhysGrant(0xA0800000u, 0x1000);
    W::notePhysGrant(0xA0900000u, 0x1000);
    check(W::readUniqStickyFile(pf.c_str(), &r, &seq, nullptr) && seq == 2,
          "F0 two publications before the fault");
    W::setUniqStickyFailInjectForTest(1);
    W::notePhysGrant(0xA0A00000u, 0x1000);            // publication 3 fails
    W::setUniqStickyFailInjectForTest(0);
    W::notePhysGrant(0xA0B00000u, 0x1000);            // must NOT publish (fail-closed)
    const bool got = W::readUniqStickyFile(pf.c_str(), &r, &seq, nullptr, &poisoned);
    check(!got && poisoned,
          "F1 a failed publication poisons the record: the older complete slot is NOT returned");
    check(W::uniqStickyPublicationFailed(&fs, &ps) && fs == 3 && ps == 2,
          "F2 the failure is latched at seq 3 and publication stopped at seq 2 (a later success cannot re-validate)");
    // Double fault: the poison write fails too. The file ALONE still reads a
    // stale complete seq-2 record - acceptance must consult the log as well.
    W::resetForTest();
    W::setUniqStickyNonceForTest(0xF3F3F3F3u);        // --integration expects this nonce
    W::setUniqStickyFileForTest(p3.c_str());
    W::setUniqStickyNonceForTest(0);
    W::notePhysGrant(0xA0800000u, 0x1000);
    W::notePhysGrant(0xA0900000u, 0x1000);
    W::setUniqStickyFailInjectForTest(2);
    W::notePhysGrant(0xA0A00000u, 0x1000);
    W::setUniqStickyFailInjectForTest(0);
    W::notePhysGrant(0xA0B00000u, 0x1000);
    const bool got3 = W::readUniqStickyFile(p3.c_str(), &r, &seq, nullptr, &poisoned);
    check(got3 && !poisoned && seq == 2 && W::uniqStickyPublicationFailed(&fs, &ps) && fs == 3,
          "F3 double fault: the file alone reads stale seq 2, the latch holds seq 3 (the log carries it)");
    // Astra 062: the acquisition's seal records the failure - positive evidence
    // that survives a truncated log. The seal + record stay for --integration.
    W::uniqSealForTest("f3");
    std::string why3;
    check(!W::acceptUniqSticky(p3.c_str(), W::uniqStickyNonceForTest(), &why3) && why3.find("S-ok") != std::string::npos,
          "F3s the double-fault acquisition is REJECTED from its seal alone (no log needed)");
    // F4: a REAL write failure (the record reopened read-only): the checked
    // fwrite must detect it; the poison write then fails for real as well.
    const std::string p4 = std::string(tmp ? tmp : ".") + "\\sr_uniq_sticky_f4.bin";
    W::resetForTest();
    W::setUniqStickyFileForTest(p4.c_str());
    W::notePhysGrant(0xA0800000u, 0x1000);
    W::notePhysGrant(0xA0900000u, 0x1000);
    W::reopenUniqStickyReadOnlyForTest(p4.c_str());
    W::notePhysGrant(0xA0A00000u, 0x1000);
    const bool latched = W::uniqStickyPublicationFailed(&fs, &ps);
    W::setUniqStickyFileForTest(nullptr);
    const bool got4 = W::readUniqStickyFile(p4.c_str(), &r, &seq, nullptr, &poisoned);
    check(latched && fs == 3 && ps == 2 && got4 && seq == 2 && !poisoned,
          "F4 a real fwrite failure is detected and latched (poison failed too: only the log carries it)");
    std::remove(p4.c_str());
    std::remove(pf.c_str());
  }

  // ── G: the seal - positive, acquisition-bound terminal evidence (Astra 062)
  {
    const char* tmp = std::getenv("TEMP");
    const std::string pg = std::string(tmp ? tmp : ".") + "\\sr_uniq_sticky_seal.bin";
    const std::string sg = pg + ".seal";
    std::remove(pg.c_str());
    std::remove(sg.c_str());
    std::string why;
    W::UniqSticky r;
    uint64_t seq = 0;
    W::resetForTest();
    W::setUniqStickyFileForTest(pg.c_str());
    W::notePhysGrant(0xA0800000u, 0x1000);
    W::noteHeapRoutedGrant(0x80800000u, 0x1000);
    W::noteStackGrant(0xB0800000u, 0x1000);
    check(!W::acceptUniqSticky(pg.c_str(), W::uniqStickyNonceForTest(), &why) && why.find("S-present: no seal") != std::string::npos,
          "G0 no seal yet: REJECTED, named as missing (missing evidence is not acceptance)");
    W::uniqSealForTest("g1");
    W::UniqSeal seal;
    const bool sealOk = W::readUniqSeal(sg.c_str(), &seal);
    check(sealOk && seal.seq == 3 && seal.failed == 0 && seal.nonce != 0 &&
              seal.nonce == W::readUniqStickyNonce(pg.c_str()) && W::acceptUniqSticky(pg.c_str(), W::uniqStickyNonceForTest(), &why),
          "G1 a clean acquisition's seal matches its record (nonce, seq 3, bytes): ACCEPTED");
    // A clean sealed acquisition KEPT for --integration (Python must accept it too).
    {
      const std::string pc = std::string(tmp ? tmp : ".") + "\\sr_uniq_sticky_clean.bin";
      std::string whyC;
      W::setUniqStickyNonceForTest(0xC1EA0001u);      // --integration expects this nonce
      W::setUniqStickyFileForTest(pc.c_str());
      W::setUniqStickyNonceForTest(0);
      W::notePhysGrant(0xA0C00000u, 0x1000);
      W::noteHeapRoutedGrant(0x80C00000u, 0x1000);
      W::uniqSealForTest("clean");
      check(W::acceptUniqSticky(pc.c_str(), W::uniqStickyNonceForTest(), &whyC), "G1c a second clean sealed acquisition is ACCEPTED");
      W::setUniqStickyFileForTest(pg.c_str());   // back to G's own record (a new acquisition)
      W::notePhysGrant(0xA0800000u, 0x1000);
      W::noteHeapRoutedGrant(0x80800000u, 0x1000);
      W::noteStackGrant(0xB0800000u, 0x1000);
      W::uniqSealForTest("g1-again");
    }
    W::notePhysGrant(0xA0900000u, 0x1000);            // after the seal
    check(W::readUniqStickyFile(pg.c_str(), &r, &seq, nullptr) && seq == 3 && W::acceptUniqSticky(pg.c_str(), W::uniqStickyNonceForTest(), &why),
          "G2 the seal freezes publication: a later registration leaves the sealed state terminal");
    // G5: a corrupted seal.
    {
      std::FILE* f = std::fopen(sg.c_str(), "r+b");
      std::fseek(f, 60, SEEK_SET);
      const int c = std::fgetc(f);
      std::fseek(f, 60, SEEK_SET);
      std::fputc(c ^ 0x5A, f);
      std::fclose(f);
    }
    check(!W::acceptUniqSticky(pg.c_str(), W::uniqStickyNonceForTest(), &why) && why.find("S-present") != std::string::npos,
          "G5 a corrupted seal (one byte flipped) is REJECTED by its checksum");
    // G6: a valid seal from ANOTHER acquisition (a new nonce for the same path).
    W::uniqSealForTest("unused");                     // no-op: already sealed
    std::remove(sg.c_str());
    W::resetForTest();
    W::setUniqStickyFileForTest(pg.c_str());          // new acquisition, new nonce
    W::notePhysGrant(0xA0800000u, 0x1000);
    W::uniqSealForTest("g6a");
    const std::string keep = sg + ".keep";
    std::remove(keep.c_str());
    std::rename(sg.c_str(), keep.c_str());            // this acquisition's seal, set aside
    W::resetForTest();
    W::setUniqStickyFileForTest(pg.c_str());          // a THIRD acquisition, same path
    W::notePhysGrant(0xA0800000u, 0x1000);
    std::rename(keep.c_str(), sg.c_str());            // the stale seal in place
    check(!W::acceptUniqSticky(pg.c_str(), W::uniqStickyNonceForTest(), &why) && why.find("S-nonce") != std::string::npos,
          "G6 a seal from another acquisition is REJECTED (nonce mismatch)");
    // G7: a record newer than its seal.
    std::remove(sg.c_str());
    W::uniqSealForTest("g7");
    {
      // Make the OTHER slot a complete, newer seq: the reader now picks it.
      std::FILE* f = std::fopen(pg.c_str(), "r+b");
      const long slot = 16;   // slot 0 - empty here, since seq 1 lives in slot 1
      const uint64_t s9 = 9;
      std::fseek(f, slot, SEEK_SET);
      std::fwrite(&s9, 8, 1, f);
      std::fseek(f, slot + 8 + (long)W::kUniqStickyRecBytes, SEEK_SET);
      std::fwrite(&s9, 8, 1, f);
      std::fclose(f);
    }
    check(!W::acceptUniqSticky(pg.c_str(), W::uniqStickyNonceForTest(), &why) && why.find("S-match") != std::string::npos,
          "G7 a record whose terminal slot is not the sealed state is REJECTED");
    // G8 (Astra 063): acquisition identity. A sealed pair from acquisition A
    // stays at the path; acquisition B (a different supplied nonce) never
    // manages to write. Only A's own expected nonce can accept that pair.
    std::remove(pg.c_str());
    std::remove(sg.c_str());
    W::resetForTest();
    W::setUniqStickyNonceForTest(0xAAAA0001u);
    W::setUniqStickyFileForTest(pg.c_str());
    W::setUniqStickyNonceForTest(0);
    W::notePhysGrant(0xA0800000u, 0x1000);
    W::uniqSealForTest("g8-A");
    W::setUniqStickyFileForTest(nullptr);             // B fails to open: no output at all
    const bool asB = W::acceptUniqSticky(pg.c_str(), 0xBBBB0001u, &why);
    const std::string whyB = why;
    const bool asNone = W::acceptUniqSticky(pg.c_str(), 0, &why);
    const std::string whyNone = why;
    const bool asA = W::acceptUniqSticky(pg.c_str(), 0xAAAA0001u, &why);
    check(!asB && whyB.find("S-expected") != std::string::npos,
          "G8 a stale sealed pair at a reused path is REJECTED for another acquisition (expected nonce)");
    check(!asNone && whyNone.find("S-expected: no expected") != std::string::npos && asA,
          "G8b no expected nonce: rejected; the pair's own acquisition: accepted");
    std::remove(pg.c_str());
    std::remove(sg.c_str());

    // G9 (Astra 063): the endpoint is SYNCHRONOUS with the recorder's close.
    // The close path (gfx noteRecorderClosed) runs the seal hook under the
    // registry lock; a registration after it is not counted.
    sr::gfx::hle::resetTraceCloseCountForTest();
    W::armUniqCloseHookForTest(true);
    W::resetForTest();
    W::setUniqStickyFileForTest(pg.c_str());
    W::notePhysGrant(0xA0800000u, 0x1000);
    W::noteHeapRoutedGrant(0x80800000u, 0x1000);      // one 12 pair
    sr::gfx::hle::noteRecorderClosedForTest();        // the recorder closes
    W::UniqSeal s9;
    const bool sealed9 = W::readUniqSeal(sg.c_str(), &s9);   // BEFORE any later registration
    W::noteHeapRoutedGrant(0x80800800u, 0x100);       // post-capture: would be a second 12 pair
    const W::UniqSticky st9 = W::uniqSticky();
    check(sealed9 && s9.seq == 2 && st9.registrations == 2 && st9.llPairs[1] == 1 &&
              W::acceptUniqSticky(pg.c_str(), W::uniqStickyNonceForTest(), &why),
          "G9 the close seals synchronously: a post-capture registration never reaches the census");
    W::armUniqCloseHookForTest(false);
    std::remove(pg.c_str());
    std::remove(sg.c_str());

    // G10: with no hook, the first registration that OBSERVES the close seals
    // before it is counted.
    sr::gfx::hle::resetTraceCloseCountForTest();
    W::resetForTest();
    W::setUniqStickyFileForTest(pg.c_str());
    W::notePhysGrant(0xA0800000u, 0x1000);
    W::noteHeapRoutedGrant(0x80800000u, 0x1000);
    sr::gfx::hle::noteRecorderClosedForTest();        // no hook armed
    W::noteHeapRoutedGrant(0x80800800u, 0x100);       // observes the close
    W::UniqSeal s10;
    const bool sealed10 = W::readUniqSeal(sg.c_str(), &s10);
    const W::UniqSticky st10 = W::uniqSticky();
    check(sealed10 && s10.seq == 2 && st10.registrations == 2 && st10.llPairs[1] == 1,
          "G10 without the hook, the registration that observes the close seals first and is not counted");
    sr::gfx::hle::resetTraceCloseCountForTest();
    W::setUniqStickyFileForTest(nullptr);
    std::remove(pg.c_str());
    std::remove(sg.c_str());

    // G11: the launcher-supplied nonce (SR_PHYS_UNIQ_NONCE, hex) is the one the
    // acquisition records - the value --accept is later given.
    _putenv_s("SR_PHYS_UNIQ_NONCE", "5EED0001");
    W::setUniqStickyFileForTest(pg.c_str());
    const uint32_t n11 = W::uniqStickyNonceForTest();
    const uint32_t h11 = W::readUniqStickyNonce(pg.c_str());
    _putenv_s("SR_PHYS_UNIQ_NONCE", "");
    check(n11 == 0x5EED0001u && h11 == 0x5EED0001u,
          "G11 SR_PHYS_UNIQ_NONCE is the acquisition's nonce (header and seal identity)");
    W::setUniqStickyFileForTest(nullptr);
    std::remove(pg.c_str());
  }
  W::setUniqCensusForTest(-1);
  W::resetForTest();
  std::printf("[PHYS_UNIQ_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// --phys-lifo-stack-test (Astra 065, the hlifo1 failure): a FRESH process, so
// the cursor starts at 0 exactly as in the game. With H-LIFO a reclaimed span is
// handed out again; nothing may ever land on the harness main stack.
int runPhysLifoStackTest() {
  std::printf("[PHYS_LIFO_STACK_TEST] H-LIFO never re-grants the main stack\n");
  g_pass = g_fail = 0;
  K::setPhysXeniaPlaceForTest(0);   // iter-465: these pin the legacy size+align placement
  check(!K::physalloc::physFreeEnabled(), "K00 SR_PHYS_FREE is off");
  K::setPhysLifoForTest(1);
  const uint32_t sLo = K::kMainStackBase, sHi = K::kMainStackBase + K::kMainStackSize;
  auto hits = [&](uint32_t va, uint32_t n) { return va < sHi && va + n > sLo; };
  const size_t c0 = K::guestPhysNextForTest();
  check(c0 == 0, "K0 fresh process: the cursor starts at 0 (the game's state at the probe)");
  const uint32_t a = K::guestPhysAlloc(0x12B00000u, 0x1000);
  check(a != 0 && !hits(a, 0x12B00000u) && a + 0x12B00000u <= sLo,
        "K1 the 0x12B00000 probe, which would straddle the main stack, starts below it");
  check(K::guestPhysLifoFree(a) == LF::kReclaimed && K::guestPhysNextForTest() == c0,
        "K2 the skipped gap is part of the reservation: the reclaim restores the cursor exactly");
  std::vector<uint32_t> v;
  bool any = false, above = false, below = false;
  for (int i = 0; i < 400; i++) {
    const uint32_t va = K::guestPhysAlloc(0x10000, 0x1000);
    v.push_back(va);
    any |= hits(va, 0x10000);
    above |= va >= sHi;
    below |= va + 0x10000 <= sLo;
  }
  check(!any, "K3 400 x 64 KB grants swept across the stack zone: none overlaps the main stack");
  check(above && below, "K3b ...they land both above and below it");
  bool allRec = true;
  for (size_t i = v.size(); i-- > 0;) allRec &= K::guestPhysLifoFree(v[i]) == LF::kReclaimed;
  check(allRec && K::guestPhysNextForTest() == c0, "K4 reverse frees reclaim across the skip exactly");
  K::registerStubs();
  auto& reg = K::ExportRegistry::instance();
  auto mmAlloc = reg.getExport("MmAllocatePhysicalMemoryEx");
  auto mmFree = reg.getExport("MmFreePhysicalMemory");
  if (mmAlloc && mmFree) {
    const uint32_t R = mmAlloc(0, 0x12B00000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    mmFree(0, R, 0, 0, 0, 0);
    const uint32_t Wp = mmAlloc(0, 0x0DFF0000u, 0x20000404u, 0u, 0xFFFFFFFFu, 0x8000u);
    check(R && Wp && !hits(R, 0x12B00000u) && !hits(Wp, 0x0DFF0000u) && Wp >= R &&
              Wp + 0x0DFF0000u <= R + 0x12B00000u,
          "K5 the hlifo1 sequence through the real exports: probe and pool avoid the main stack; pool inside the probe");
  } else {
    check(false, "K5 the Mm exports are registered");
  }
  // K6: the census registers the main stack, so a grant ON it is a live-live
  // 23 pair (the gate-OFF probe is exactly such a grant).
  W::resetForTest();
  W::setUniqCensusForTest(1);
  W::setUniqNoteMainStackForTest(true);
  W::notePhysGrant(0xA0100000u, 0x1000);            // first registration notes the main stack
  W::notePhysGrant(sLo + 0x80000u, 0x1000);         // a grant on the main stack
  {
    const W::UniqSticky s = W::uniqSticky();
    check(s.llPairs[4] == 1 && s.llBytes[4] == 0x1000 && s.exOld[4].va == sLo && s.registrations == 3,
          "K6 the census registers the main stack: a grant on it is a live-live 23 pair");
  }
  W::setUniqNoteMainStackForTest(false);
  W::setUniqCensusForTest(-1);
  W::resetForTest();
  // K7-K9 (iter-462): a heap grant's 0xA alias is a hole the bump steps below,
  // exactly like the main stack (guestPhysReserveAlias; the GPU ring's pages).
  {
    const size_t c1 = K::guestPhysNextForTest();
    const uint32_t holeHi = uint32_t(size_t(K::kMinGuestSizeToCoverAllUsedVAs) - c1 - 0x10000u);  // kPhysTop
    const uint32_t holeLo = holeHi - 0x20000u;
    K::guestPhysReserveAlias(0x80000000u | (holeLo & 0x1FFFFFFFu), 0x20000u);
    auto inHole = [&](uint32_t va, uint32_t n) { return va < holeHi && va + n > holeLo; };
    std::vector<uint32_t> g;
    bool anyHole = false, belowHole = false;
    for (int i = 0; i < 4; i++) {
      const uint32_t va = K::guestPhysAlloc(0x10000, 0x1000);
      g.push_back(va);
      anyHole |= va == 0 || inHole(va, 0x10000);
      belowHole |= va + 0x10000 <= holeLo;
    }
    check(!anyHole, "K7 4 x 64 KB grants across an alias hole: none lands on the heap grant's pages");
    check(belowHole, "K8 ...the bump steps below the hole");
    bool rec = true;
    for (size_t i = g.size(); i-- > 0;) rec &= K::guestPhysLifoFree(g[i]) == LF::kReclaimed;
    check(rec && K::guestPhysNextForTest() == c1, "K9 reverse frees reclaim across the hole exactly");
  }
  K::setPhysLifoForTest(-1);
  std::printf("[PHYS_LIFO_STACK_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// --memcpy-guard-test (Astra 066): the SIMD memcpy guard (sub_82468290 override)
// through the real entry. Its legal range ended at the pre-iter-173 top
// 0xB4000000, silently dropping copies to/from live memory above it.
int runMemcpyGuardTest() {
  std::printf("[MEMCPY_GUARD_TEST] SIMD memcpy guard range\n");
  g_pass = g_fail = 0;
  K::setPhysXeniaPlaceForTest(0);   // iter-465: these pin the legacy size+align placement
  auto fillv = [](uint32_t va, uint8_t v, uint32_t n) {
    if (uint8_t* p = K::guestVAToHost(va)) std::memset(p, v, n);
  };
  auto allv = [](uint32_t va, uint8_t v, uint32_t n) {
    const uint8_t* p = K::guestVAToHost(va);
    if (!p) return false;
    for (uint32_t i = 0; i < n; i++) if (p[i] != v) return false;
    return true;
  };
  auto memcpyGuest = [](uint32_t dst, uint32_t src, uint32_t n) {
    PPCContext c{};
    c.r3.u64 = dst;
    c.r4.u64 = src;
    c.r5.u64 = n;
    c.r1.u64 = 0x8FF00000u;   // a scratch guest stack for the lifted body
    sub_82468290(c, K::getGuestBase());
    return c.r3.u32;
  };
  constexpr uint32_t kHiSrc = 0xB9270000u, kHiDst = 0xBD000000u, kLo = 0x8FA30000u;
  constexpr uint32_t kN = 0x440;
  // iter-463 charter (burndown U1): the reject is a band-aid and now acts only
  // under SR_ALLOW_FORCING; the default exe runs the guest's own memcpy for
  // every input. The top fix (SR_MEMCPY_GUARD_TOP_FIX) only moves the reject
  // window, so its cases below are judged against the forcing state.
  const bool guardActs = K::forcingAllowed();
  // Env default: SR_MEMCPY_GUARD_TOP_FIX is baked ON, and without the master
  // gate nothing is rejected - either way the copy from 0xB9270000 happens.
  K::setMemcpyGuardTopFixForTest(-1);
  fillv(kHiSrc, 0x5A, kN); fillv(kLo, 0x00, kN);
  memcpyGuest(kLo, kHiSrc, kN);
  check(allv(kLo, 0x5A, kN), "M0 env default: a copy FROM 0xB9270000 happens (no hlifo2 reject)");
  K::setMemcpyGuardTopFixForTest(1);
  fillv(kLo, 0x00, kN);
  const uint32_t ret = memcpyGuest(kLo, kHiSrc, kN);
  check(allv(kLo, 0x5A, kN) && ret == kLo, "M1 gate ON: a copy FROM live memory above 0xB4000000 happens");
  fillv(kLo, 0x3C, kN); fillv(kHiDst, 0x00, kN);
  memcpyGuest(kHiDst, kLo, kN);
  check(allv(kHiDst, 0x3C, kN), "M2 gate ON: a copy TO memory above 0xB4000000 happens");
  fillv(0xBEFFF000u, 0x77, 0x100); fillv(kLo, 0x00, 0x100);
  memcpyGuest(kLo, 0xBEFFF000u, 0x100);
  check(allv(kLo, 0x77, 0x100), "M3 gate ON: the last page below 0xBF000000 is legal");
  fillv(0xBF000000u, 0x99, 0x100);   // the test world maps it, so a wrong accept would copy 0x99
  fillv(kLo, 0x22, 0x100);
  memcpyGuest(kLo, 0xBF000000u, 0x100);
  check(allv(kLo, guardActs ? 0x22 : 0x99, 0x100),
        guardActs ? "M4 gate ON + SR_ALLOW_FORCING: 0xBF000000 (beyond mapped guest RAM) is rejected"
                  : "M4 gate ON, forcing off: 0xBF000000 is copied by the guest memcpy (no reject)");
  fillv(kLo, 0x11, 0x40);
  memcpyGuest(kLo, 0, 0x40);
  const bool nullCopied = !allv(kLo, 0x11, 0x40);   // VA 0 holds no 0x11 run in this world
  check(guardActs ? !nullCopied : nullCopied,
        guardActs ? "M5 gate ON + SR_ALLOW_FORCING: a NULL source is rejected"
                  : "M5 gate ON, forcing off: a NULL source reaches the guest memcpy (no reject)");
  fillv(0x8FA40000u, 0x66, 0x80); fillv(kLo, 0x00, 0x80);
  memcpyGuest(kLo, 0x8FA40000u, 0x80);
  check(allv(kLo, 0x66, 0x80), "M6 gate ON: an ordinary low copy is unchanged");
  K::setMemcpyGuardTopFixForTest(-1);
  std::printf("[MEMCPY_GUARD_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// --uniq-sticky-dump <file>: the C++ decoding of a durable record, in the same
// key=value format as tools/uniq_sticky_read.py (the two must agree).
int runUniqStickyDump(const char* path) {
  W::UniqSticky s;
  uint64_t seq = 0;
  int complete = 0;
  if (!W::readUniqStickyFile(path, &s, &seq, &complete)) {
    std::printf("error=no complete slot\n");
    return 1;
  }
  static const char* const kP[6] = {"11", "12", "13", "22", "23", "33"};
  std::printf("seq=%llu complete=%d\n", (unsigned long long)seq, complete);
  std::printf("registrations=%llu\n", (unsigned long long)s.registrations);
  for (int i = 0; i < 6; i++)
    std::printf("ll%s=%llu/0x%llX new=%08X+0x%X/s%u old=%08X+0x%X/s%u\n", kP[i],
                (unsigned long long)s.llPairs[i], (unsigned long long)s.llBytes[i], s.exNew[i].va,
                s.exNew[i].size, s.exNew[i].serial, s.exOld[i].va, s.exOld[i].size, s.exOld[i].serial);
  std::printf("physBelowA=%llu stackBelowA=%llu firstBelowAVA=%08X\n", (unsigned long long)s.physBelowA,
              (unsigned long long)s.stackBelowA, s.firstBelowAVA);
  std::printf("minPhysRegionLo=%08X\n", s.minPhysRegionLo);
  std::printf("heapSpanPairs=%llu heapSpanBytes=0x%llX heapSpanBytesMaxTick=0x%llX\n",
              (unsigned long long)s.heapSpanPairs, (unsigned long long)s.heapSpanBytes,
              (unsigned long long)s.heapSpanBytesMaxTick);
  std::printf("registryOverlaps=%llu,%llu,%llu\n", (unsigned long long)s.registryOverlaps[0],
              (unsigned long long)s.registryOverlaps[1], (unsigned long long)s.registryOverlaps[2]);
  return 0;
}

// iter-462 --heap-align-test: guestHeapAlloc keeps every grant 16-byte
// aligned after a large (page-aligned) grant, as the console heap does. The
// odd 0xFFF pad left the bump cursor at 15 mod 16 (SR_HEAP_CURSOR_ALIGN=0
// restores it, and A3/A5 must then fail - the mutation check).
int runHeapAlignTest() {
  std::printf("[HEAP_ALIGN_TEST] guestHeapAlloc alignment after large grants\n");
  g_pass = g_fail = 0;
  K::setPhysXeniaPlaceForTest(0);   // iter-465: these pin the legacy size+align placement
  const uint32_t a = K::guestHeapAlloc(0x20);
  check(a && (a & 15u) == 0, "A1 a small grant is 16-byte aligned");
  const uint32_t big = K::guestHeapAlloc(0x10000);
  check(big && (big & 0xFFFu) == 0, "A2 a >=64KB grant starts on a 4KB page");
  const uint32_t s = K::guestHeapAlloc(0x20);
  check(s && (s & 15u) == 0, "A3 the grant after a large one is 16-byte aligned");
  check(s >= big + 0x10000u || s + 0x20u <= big, "A4 it does not overlap the large grant");
  bool all = true;
  uint32_t prevEnd = s + 0x20u;
  static const size_t kSizes[] = {0x12345, 0x30, 0x10001, 0x7, 0x20000, 0x44};
  for (size_t sz : kSizes) {
    const uint32_t v = K::guestHeapAlloc(sz);
    all &= v != 0 && (v & 15u) == 0 && (sz < 0x10000 || (v & 0xFFFu) == 0) && v >= prevEnd;
    prevEnd = v + static_cast<uint32_t>(sz);
  }
  check(all, "A5 a mixed sequence stays 16-aligned, page-aligned when large, and ascending");
  std::printf("[HEAP_ALIGN_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// ── iter-463 F1: --phys-lifo-reuse-test ─────────────────────────────────────
// A FRESH process (the cursor starts at 0, as in the game). The v1_gpuhang leak
// (a non-top free was retained forever, the cursor crept 30 minutes until the
// arena's 0x2B30000 grant landed at 0x9E820000, double-booking the D3D command
// buffer's physical pages) and its fix: free extents reused first-fit top-down,
// the top free cascading through them, and no grant below 0xA0000000.
// Mutation-validated: no cascade -> K4/C3/F4/R3 fail; no floor -> G1 fails; no
// extent listing -> K1/C1/F1/F2 fail.
namespace {
constexpr uint32_t kTopVA = 0xBF000000u;
constexpr uint32_t kPMask = 0x1FFFFFFFu;
bool lifoInv(const char* tag) {
  std::string why;
  const bool ok = K::physLifoCheckForTest(&why);
  if (!ok) std::printf("      (%s: invariant broken: %s)\n", tag, why.c_str());
  return ok;
}
// The VA span [lo, hi) of a grant's recorded reservation.
bool resSpan(uint32_t va, uint32_t& lo, uint32_t& hi) {
  size_t p = 0, r = 0;
  int st = 0;
  if (!K::physLifoGrantForTest(va, &p, &r, &st)) return false;
  lo = uint32_t(kTopVA - (p + r));
  hi = uint32_t(kTopVA - p);
  return true;
}
size_t retained() { return K::physLifoRetainedBytes(); }
bool onStack(uint32_t va, uint32_t n) {
  return va < K::kMainStackBase + K::kMainStackSize && va + n > K::kMainStackBase;
}
}  // namespace

int runPhysLifoReuseTest() {
  std::printf("[PHYS_LIFO_REUSE_TEST] LIFO reuse, cascade and the 0xA0000000 floor\n");
  g_pass = g_fail = 0;
  K::setPhysXeniaPlaceForTest(0);   // iter-465: these pin the legacy size+align placement
  check(!K::physalloc::physFreeEnabled(), "Z00 SR_PHYS_FREE is off (H-LIFO is inert under it)");
  K::setPhysLifoForTest(1);
  K::setPhysLifoReuseForTest(-1);
  K::setPhysFloorForTest(-1);
  const char* eR = std::getenv("SR_PHYS_LIFO_REUSE");
  const char* eF = std::getenv("SR_PHYS_FLOOR");
  if (!eR && !eF)
    check(K::physLifoReuseEnabled() && K::physFloorEnabled(),
          "Z01 SR_PHYS_LIFO_REUSE and SR_PHYS_FLOOR default ON");
  K::setPhysLifoReuseForTest(1);
  K::setPhysFloorForTest(1);
  const size_t c0 = K::guestPhysNextForTest();
  check(c0 == 0, "Z02 fresh process: the cursor starts at 0");
  auto A = [](size_t s, uint32_t al) { return K::guestPhysAlloc(s, al); };
  auto F = [](uint32_t va) { return K::guestPhysLifoFree(va); };

  // ── K: the main stack inside a free extent ───────────────────────────────
  {
    const uint32_t a = A(0x2000000, 0x1000);   // straddles the stack: its reservation holds it
    const uint32_t g = A(0x1000, 0x1000);      // above it, so a's free is not the top
    uint32_t lo = 0, hi = 0;
    check(a && g && resSpan(a, lo, hi) && lo <= K::kMainStackBase &&
              hi >= K::kMainStackBase + K::kMainStackSize,
          "K0 precondition: the grant's reservation contains the main stack");
    check(F(a) == LF::kNotTop && retained() == size_t(hi - lo) && K::physLifoRetainedExtents() == 1 &&
              lifoInv("K1"),
          "K1 a non-top free lists its whole reservation as ONE free extent");
    const uint32_t s = A(0x1800000, 0x1000);
    check(s && !onStack(s, 0x1800000) && s >= lo && s + 0x1800000u <= hi,
          "K2 a 24 MB grant whose top placement covers the stack lands below it, inside the extent");
    check(s == ((K::kMainStackBase - 0x1800000u) & ~0xFFFu), "K2b ...as high as it can: directly below the stack");
    check(lifoInv("K3"), "K3 invariant: live + free partition [origin, physNext) exactly");
    check(F(s) == LF::kNotTop && F(g) == LF::kReclaimed && K::guestPhysNextForTest() == c0 &&
              retained() == 0 && lifoInv("K4"),
          "K4 the top free cascades through every freed grant below it, back to the origin");
  }

  // ── H: an alias hole inside a free extent ────────────────────────────────
  {
    const uint32_t a = A(0x100000, 0x1000);
    const uint32_t g = A(0x1000, 0x1000);
    uint32_t lo = 0, hi = 0;
    resSpan(a, lo, hi);
    check(F(a) == LF::kNotTop, "H0 the grant is freed (retained as an extent)");
    const uint32_t hLo = hi - 0x30000u, hHi = hi - 0x10000u;   // the heap grant's 0xA alias
    K::guestPhysReserveAlias(0x80000000u | (hLo & kPMask), 0x20000u);
    const uint32_t s = A(0x40000, 0x1000);
    check(s && !(s < hHi && s + 0x40000u > hLo) && s >= lo && s + 0x40000u <= hLo,
          "H1 a grant whose top placement covers the hole steps below it, inside the extent");
    const uint32_t t = A(0x8000, 0x1000);
    check(t == hi - 0x8000u, "H2 a grant that fits above the hole takes that space (first fit, top-down)");
    check(lifoInv("H3"), "H3 invariant");
    check(F(s) == LF::kNotTop && F(t) == LF::kNotTop && F(g) == LF::kReclaimed &&
              K::guestPhysNextForTest() == c0 && retained() == 0 && lifoInv("H4"),
          "H4 all freed: the cascade returns the cursor to the origin");
    K::resetPhysHolesForTest();
  }

  // ── C / S / L: coalesce, split, alignment ────────────────────────────────
  {
    const uint32_t a = A(0x40000, 0x1000), b = A(0x40000, 0x1000), c = A(0x40000, 0x1000);
    const uint32_t g = A(0x1000, 0x1000);
    uint32_t aLo = 0, aHi = 0, cLo = 0, cHi = 0;
    resSpan(a, aLo, aHi);
    resSpan(c, cLo, cHi);
    check(F(a) == LF::kNotTop && F(c) == LF::kNotTop && K::physLifoRetainedExtents() == 2,
          "C1 two separated frees are two extents");
    check(F(b) == LF::kNotTop && K::physLifoRetainedExtents() == 1 && retained() == size_t(aHi - cLo),
          "C2 freeing the grant between them coalesces all three into one extent");
    const size_t r0 = retained();
    const uint32_t s1 = A(0x10000, 0x1000);
    check(s1 == aHi - 0x10000u && K::physLifoRetainedExtents() == 1 && retained() == r0 - 0x10000u,
          "S1 carved from the extent's top: one extent remains, exactly 0x10000 fewer bytes");
    const uint32_t s2 = A(0x1000, 0x10000);
    check(s2 && (s2 & 0xFFFFu) == 0 && s2 + 0x1000u <= s1 && s2 >= cLo && K::physLifoRetainedExtents() == 2 &&
              retained() == r0 - 0x11000u,
          "S2 a 64 KB-aligned grant inside the extent splits it in two; alignment honoured");
    const uint32_t s3 = A(0x3000, 0x8000);
    check(s3 && (s3 & 0x7FFFu) == 0 && lifoInv("L1"), "L1 an 0x8000-aligned grant from an extent: aligned, invariant");
    size_t p = 0, r = 0;
    int st = -1;
    check(K::physLifoGrantForTest(s2, &p, &r, &st) && st == 0 && r == 0x1000u &&
              uint32_t(kTopVA - p - r) == s2,
          "L2 a reused grant records its EXACT span as its reservation");
    check(F(s1) == LF::kNotTop && F(s3) == LF::kNotTop && F(s2) == LF::kNotTop && F(g) == LF::kReclaimed &&
              K::guestPhysNextForTest() == c0 && retained() == 0 && lifoInv("C3"),
          "C3 everything freed: the cascade returns the cursor to the origin");
  }

  // ── F: the v1_gpuhang sequence, fixed ───────────────────────────────────
  W::resetForTest();
  W::setUniqCensusForTest(1);
  {
    const uint32_t big = A(0x2B30000, 0x8000);
    W::notePhysGrant(big, 0x2B30000u);
    const uint32_t small = A(0x10000, 0x1000);
    W::notePhysGrant(small, 0x10000u);
    uint32_t lo = 0, hi = 0;
    resSpan(big, lo, hi);
    size_t sp = 0, sr = 0;
    K::physLifoGrantForTest(small, &sp, &sr, nullptr);
    check(F(big) == LF::kNotTop && retained() == size_t(hi - lo),
          "F1 the first arena grant is freed under a newer grant: its reservation is listed free");
    check(W::uniqSnapshot().grantsReleased == 1, "F1r ...and its registry record is RELEASED");
    const uint32_t pre = K::physLifoPreadvanceNowForTest(0x1DCAC480u);
    check(pre != 0 && K::guestPhysNextForTest() == 0x1DCAC480u,
          "F1p the cursor is where v1_gpuhang had crept to (0x1DCAC480)");
    const uint32_t big2 = A(0x2B30000, 0x8000);
    check(big2 >= 0xA0000000u && big2 >= lo && big2 + 0x2B30000u <= hi && !onStack(big2, 0x2B30000u),
          "F2 the reload's grant REUSES the retained extent (not 0x9E820000)");
    check(K::guestPhysNextForTest() == 0x1DCAC480u, "F2b ...and the cursor does not move");
    W::notePhysGrant(big2, 0x2B30000u);
    const W::UniqSnapshot u = W::uniqSnapshot();
    check(u.grantsReleased == 0 && u.pairsLiveLive == 0 && u.physGrantsBelowA == 0 &&
              W::ownerOfGrant(big2, 0x1000) == W::kOwnPhysRegion &&
              W::gpuWindow(big2 & kPMask, 0x1000) == 0xA,
          "F2r the stale released record is dropped on reuse: one live owner, gpuWindow = 0xA");
    check(lifoInv("F3"), "F3 invariant");
    check(F(big2) == LF::kNotTop && F(pre) == LF::kReclaimed && K::guestPhysNextForTest() == sp + sr,
          "F4a freeing the reload grant, then the top: the cursor rolls back to the small grant");
    check(F(small) == LF::kReclaimed && K::guestPhysNextForTest() == c0 && retained() == 0 &&
              lifoInv("F4"),
          "F4 the small grant's free cascades through the arena extent: the cursor is back at its origin");
  }
  // ── F5: the forget on the BUMP path (review: mutation B survived) ─────────
  // A released record whose pages the cascade gave back to the cursor, then a
  // larger BUMPED grant starting below it: the stale record would sit inside
  // the new one and make its pages read "unowned" (inMapLive takes the
  // predecessor record, the released one).
  W::resetForTest();   // F4 leaves the reload grant's released record (nothing re-covered it)
  W::setUniqCensusForTest(1);
  {
    const uint32_t x = A(0x100000, 0x1000);
    W::notePhysGrant(x, 0x100000u);
    const uint32_t y = A(0x1000, 0x1000);
    W::notePhysGrant(y, 0x1000u);
    check(F(x) == LF::kNotTop && W::uniqSnapshot().grantsReleased == 1,
          "F5a the non-top free leaves a RELEASED registry record");
    check(F(y) == LF::kReclaimed && K::guestPhysNextForTest() == c0 && retained() == 0,
          "F5b the top free cascades through it: the cursor is back at its origin, nothing listed");
    const uint64_t reuse0 = K::physLifoStats().reuses;
    const uint32_t z = A(0x200000, 0x1000);
    check(z && z < x && z + 0x200000u >= x + 0x100000u && K::physLifoStats().reuses == reuse0,
          "F5c a larger grant is BUMPED over the released record's pages (no extent reuse)");
    W::notePhysGrant(z, 0x200000u);
    check(W::uniqSnapshot().grantsReleased == 0 && W::gpuWindow((x + 0x1000u) & kPMask, 0x1000) == 0xA &&
              W::gpuWindow((z + 0x1000u) & kPMask, 0x1000) == 0xA,
          "F5d the stale record is dropped on the bump: the old record's pages are the new grant's (0xA)");
    check(F(z) == LF::kReclaimed && K::guestPhysNextForTest() == c0 && lifoInv("F5"), "F5e cleanup");
  }
  W::setUniqCensusForTest(-1);
  W::resetForTest();

  // ── G: the 0xA0000000 floor ──────────────────────────────────────────────
  {
    const uint64_t r0 = K::physLifoStats().floorRefusals;
    const uint32_t pre = K::physLifoPreadvanceNowForTest(0x1DCAC480u);
    const uint32_t g = A(0x2B30000, 0x8000);
    check(pre && g == 0 && K::guestPhysNextForTest() == 0x1DCAC480u &&
              K::physLifoStats().floorRefusals == r0 + 1,
          "G1 with no extent to reuse, a grant that would reach below 0xA0000000 is REFUSED ([PHYS_CAP])");
    const uint32_t s = A(0x10000, 0x1000);
    check(s >= 0xA0000000u, "G2 a grant that fits above the floor is still granted");
    check(F(s) == LF::kReclaimed && F(pre) == LF::kReclaimed && K::guestPhysNextForTest() == c0,
          "G3 cleanup back to the origin");
  }
  {
    // Review: the floor tests the GRANT, not the reservation's padding. At
    // cursor 0x1EFF9000 a 0x1000 grant aligned 0x8000 reserves [0x9FFFE000,
    // 0xA0007000); its aligned VA 0xA0000000 is at the floor - granted, with
    // the padding below the floor clipped off the reservation.
    const uint32_t pre = K::physLifoPreadvanceNowForTest(0x1EFF9000u);
    const uint32_t g = A(0x1000, 0x8000);
    check(pre && g == 0xA0000000u && K::guestPhysNextForTest() == 0x1F000000u && lifoInv("G4"),
          "G4 a grant whose aligned VA is AT the floor is granted; the padding is clipped (cursor 0x1F000000)");
    const uint64_t r0 = K::physLifoStats().floorRefusals;
    check(A(0x10, 16) == 0 && K::physLifoStats().floorRefusals == r0 + 1, "G5 nothing fits below it");
    check(F(g) == LF::kReclaimed && F(pre) == LF::kReclaimed && K::guestPhysNextForTest() == c0,
          "G6 cleanup back to the origin");
  }

  // ── R: randomized stress (thousands of alloc/free in random order) ────────
  {
    K::setPhysLifoForTest(1);
    const size_t origin = K::guestPhysNextForTest();
    // Two alias holes inside the region the stress uses (the main stack is in it too).
    const uint32_t h1 = 0xBEC00000u, h2 = 0xBD400000u;
    K::guestPhysReserveAlias(0x80000000u | (h1 & kPMask), 0x20000u);
    K::guestPhysReserveAlias(0x80000000u | (h2 & kPMask), 0x40000u);
    auto onHole = [&](uint32_t va, uint32_t n) {
      return (va < h1 + 0x20000u && va + n > h1) || (va < h2 + 0x40000u && va + n > h2);
    };
    struct Live { uint32_t va, size; };
    std::vector<Live> live;
    std::mt19937 rng(463u);
    uint64_t ops = 0, allocs = 0, frees = 0, refused = 0, notTop = 0, rec = 0;
    bool below = false, misaligned = false, blocked = false, overlap = false, inv = true, badFree = false;
    const uint64_t reuse0 = K::physLifoStats().reuses, casc0 = K::physLifoStats().cascades;
    auto disjoint = [&]() {
      std::vector<Live> v = live;
      std::sort(v.begin(), v.end(), [](const Live& x, const Live& y) { return x.va < y.va; });
      for (size_t i = 1; i < v.size(); i++)
        if (v[i - 1].va + v[i - 1].size > v[i].va) return false;
      return true;
    };
    for (int i = 0; i < 30000; i++, ops++) {
      const bool doFree = !live.empty() && ((rng() % 100u) < 47u || live.size() > 400);
      if (doFree) {
        const size_t idx = rng() % live.size();
        const LF r = F(live[idx].va);
        if (r == LF::kReclaimed) rec++;
        else if (r == LF::kNotTop) notTop++;
        else badFree = true;
        live[idx] = live.back();
        live.pop_back();
        frees++;
      } else {
        const uint32_t cap = 0x10u << (rng() % 17u);   // up to 1 MB
        const uint32_t size = 1u + rng() % cap;
        const uint32_t al = 16u << (rng() % 13u);      // 16 B .. 64 KB
        const uint32_t va = A(size, al);
        allocs++;
        if (!va) { refused++; continue; }
        const uint32_t n = (size + 15u) & ~15u;
        below |= va < 0xA0000000u;
        misaligned |= (va & (al - 1u)) != 0;
        blocked |= onStack(va, n) || onHole(va, n);
        live.push_back({va, n});
      }
      if (inv && !lifoInv("R")) inv = false;
      if ((i % 64) == 0 && !disjoint()) overlap = true;
    }
    if (!disjoint()) overlap = true;
    std::printf("      (R: %llu ops, %llu allocs, %llu frees [%llu reclaimed, %llu retained], %llu refused, "
                "%llu reuses, %llu cascades, %zu live at the end)\n",
                (unsigned long long)ops, (unsigned long long)allocs, (unsigned long long)frees,
                (unsigned long long)rec, (unsigned long long)notTop, (unsigned long long)refused,
                (unsigned long long)(K::physLifoStats().reuses - reuse0),
                (unsigned long long)(K::physLifoStats().cascades - casc0), live.size());
    check(!badFree, "R0 every free of a live grant is reclaimed or retained (never unknown/duplicate)");
    check(inv, "R1 invariant after EVERY op: live + free extents partition [origin, physNext), coalesced, "
               "no extent ends at physNext (physNext = the lowest live reservation edge)");
    check(!overlap, "R2 no two live grants overlap");
    check(!below && refused == 0, "R3 no grant below 0xA0000000, none refused");
    check(!misaligned, "R4 every grant honours its alignment");
    check(!blocked, "R5 no grant covers the main stack or an alias hole");
    check(K::physLifoStats().reuses > reuse0 && K::physLifoStats().cascades > casc0 && notTop > 0,
          "R6 reuse, retention and cascades were all exercised");
    std::shuffle(live.begin(), live.end(), rng);
    for (const Live& l : live) F(l.va);
    check(K::guestPhysNextForTest() == origin && retained() == 0 && lifoInv("R7"),
          "R7 freeing the rest in random order returns the cursor to its origin, nothing retained");
    K::resetPhysHolesForTest();
  }

  // ── X: the leak itself - reuse and floor OFF reproduce v1_gpuhang ────────
  {
    K::setPhysLifoReuseForTest(0);
    K::setPhysFloorForTest(0);
    K::setPhysLifoForTest(1);
    const size_t x0 = K::guestPhysNextForTest();
    const uint32_t big = A(0x2B30000, 0x8000);
    const uint32_t small = A(0x10000, 0x1000);
    check(F(big) == LF::kNotTop && retained() == 0, "X1 gate OFF: the non-top free is retained, never listed");
    const size_t cur = K::guestPhysNextForTest();
    const uint32_t pre = K::physLifoPreadvanceNowForTest(0x1DCAC480u);
    const uint32_t big2 = A(0x2B30000, 0x8000);
    std::printf("      (X: origin 0x%zX, cursor after the frees 0x%zX, reload grant %08X, cursor now 0x%zX)\n",
                x0, cur, big2, K::guestPhysNextForTest());
    check(pre && big2 == 0x9E820000u && K::guestPhysNextForTest() == 0x207E4480u,
          "X2 the reload's grant lands at 0x9E820000, cursor 0x207E4480 - the v1_gpuhang log, reproduced");
    check(F(big2) == LF::kReclaimed && F(pre) == LF::kReclaimed && K::guestPhysNextForTest() == cur,
          "X3 cleanup: the reload grant and the dummy roll back as tops");
    (void)small;
  }

  // ── P: the E3 env trigger (review: it fired at the FIRST grant, before the
  // D3D objects existed, so they were placed below the dummy - a false negative)
  // SR_PHYS_LIFO_PREADVANCE=1DCAC480 SR_PHYS_LIFO_PREADVANCE_AT=2B30000 with
  // reuse and floor off: armed by the rptr write-back, applied at the arena grant.
  {
    K::setPhysPreadvanceForTest(0x1DCAC480u, 0x2B30000u);
    const size_t p0 = K::guestPhysNextForTest();
    const uint32_t d3d = A(0x700000, 0x1000);   // boot: the D3D command buffer, before arming
    check(d3d > 0xB0000000u && K::guestPhysNextForTest() > p0 && K::guestPhysNextForTest() < 0x10000000u,
          "P1 not armed: boot grants keep their places at the top (no dummy)");
    K::physLifoPreadvanceArm();   // gpuCpSetRptrWriteback: the fence block exists
    const uint32_t other = A(0x10000, 0x1000);
    check(other > 0xB0000000u && K::guestPhysNextForTest() < 0x10000000u,
          "P2 armed, but a grant of another size: still no dummy");
    const uint32_t arena = A(0x2B30000, 0x8000);
    check(arena == 0x9E820000u && K::guestPhysNextForTest() == 0x207E4480u,
          "P3 the first 0x2B30000 grant after arming: dummy to 0x1DCAC480, the grant lands at 0x9E820000");
    const uint32_t again = A(0x10000, 0x1000);
    check(again == 0x9E80B000u, "P4 the dummy is applied once");
    K::setPhysPreadvanceForTest(0, 0);
    (void)d3d;
  }
  K::setPhysLifoReuseForTest(-1);
  K::setPhysFloorForTest(-1);
  K::setPhysLifoForTest(-1);
  std::printf("[PHYS_LIFO_REUSE_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// ── iter-465 physmem: thread memory where Xenia puts it (kernel/thread_mem_xenia.hpp) ──
// X: xheap::allocRange (Xenia BaseHeap::AllocRange placement inside a range).
// T: thread stacks in v40000000 [0x70000000, 0x7F000000), zeroed, released once.
// W: the Warworld -> Metropolis return in miniature on the LIFO allocator: with the
//    loader threads' stacks in physical memory the freed 224 MB world pool is split
//    and the re-request is refused (the [PHYS_CAP] -> [ICALL_FAIL] 8260527C crash);
//    with the stacks in the Xenia stack range the pool fits back where it was.
// Mutation-validated: stacks from guestPhysAlloc in the W4 block -> W4 fails; no
// release in releaseCurrentStack -> T4 fails; findGapIn top-down returning the
// lowest fit -> X5 fails.
#include "kernel/ntavm_xenia_heap.hpp"
#include "kernel/thread_mem_xenia.hpp"
int runThreadMemTest() {
  std::printf("[THREAD_MEM_TEST] thread stacks / system blocks in the Xenia heaps\n");
  g_pass = g_fail = 0;
  K::setPhysXeniaPlaceForTest(0);   // iter-465: these pin the legacy size+align placement
  namespace X = sr::kernel::xheap;
  namespace T = sr::kernel::threadmem;
  X::resetForTest();
  uint32_t g = 0;
  // ── X ──
  const uint32_t a = X::allocRange(0x70000000u, 0x7F000000u, 0x60000u, 0x10000u, true, false, &g);
  check(a == 0x70000000u && g == 0x60000u, "X1 first stack-range grant at 0x70000000 (bottom-up)");
  const uint32_t b = X::allocRange(0x70000000u, 0x7F000000u, 0x58000u, 0x10000u, true, false, &g);
  check(b == 0x70060000u && g == 0x60000u, "X2 next grant directly above, size rounded to the 64 KB page");
  check(X::owns(a) && X::owns(b + 0x5FFFFu) && !X::owns(b + 0x60000u), "X3 owns() covers the grants");
  check(X::release(a) == 0x60000u, "X4 release returns the size");
  const uint32_t d = X::allocRange(0x70000000u, 0x7F000000u, 0x10000u, 0x10000u, true, true, &g);
  check(d == 0x7EFF0000u, "X5 top-down: the highest fit in the range");
  const uint32_t c = X::allocRange(0x70000000u, 0x7F000000u, 0x30000u, 0x10000u, true, false, &g);
  check(c == 0x70000000u, "X6 a released range is reused first-fit");
  const uint32_t e = X::allocRange(0x70000000u, 0x7F000000u, 0x10000u, 0x100000u, true, false, &g);
  check(e == 0x70100000u, "X7 alignment honoured (1 MB): the first aligned gap above the live grants");
  const uint32_t s1 = X::allocRange(0x30000000u, 0x40000000u, 0xAB0u, 0x1000u, false, false, &g);
  const uint32_t s2 = X::allocRange(0x30000000u, 0x40000000u, 0x2D8u, 0x1000u, false, false, &g);
  check(s1 == 0x30000000u && s2 == 0x30001000u && g == 0x1000u,
        "X8 system region: 4 KB pages from 0x30000000 (KTHREAD then KPCR)");
  check(X::allocRange(0x70000000u, 0x70010000u, 0x20000u, 0x10000u, true, false, &g) == 0,
        "X9 a request larger than the range fails");
  check(X::allocRange(0x7EFF0000u, 0x7F000000u, 0x10000u, 0x10000u, true, false, &g) == 0,
        "X10 a range fully covered by a live grant fails");
  for (uint32_t v : {b, c, d, e, s1, s2}) X::release(v);
  check(X::liveRegionsForTest() == 0, "X11 all released");

  // ── T ──
  T::forceEnabledForTest(1);
  uint32_t base = 0;
  const uint32_t lim = T::allocStack(0x40000u, &base);
  check(base == 0x70000000u && lim == 0x70010000u, "T1 stack: limit = base + one 64 KB guard page");
  if (uint8_t* p = K::guestVAToHost(lim)) std::memset(p, 0xCD, 0x40000u);
  T::releaseStack(base);
  uint32_t base2 = 0;
  const uint32_t lim2 = T::allocStack(0x40000u, &base2);
  const uint8_t* q = K::guestVAToHost(lim2);
  bool zero = q != nullptr;
  for (uint32_t i = 0; zero && i < 0x40000u; i += 0x1000u) zero = q[i] == 0 && q[i + 0xFFFu] == 0;
  check(base2 == base && lim2 == lim && zero, "T2 a released stack is reused and comes back zeroed");
  const uint32_t lim3 = T::allocStack(0x40000u, &g);
  check(lim3 == 0x70070000u, "T3 the next stack sits above (0x40000 + 2 guard pages = 0x60000 each)");
  T::bindCurrentStack(base2);
  T::releaseCurrentStack();
  check(!X::owns(lim2) && X::owns(lim3), "T4 releaseCurrentStack gives back the bound stack only");
  T::releaseCurrentStack();
  check(X::liveRegionsForTest() == 1, "T5 a second releaseCurrentStack is a no-op");
  T::releaseStack(g);
  check(X::liveRegionsForTest() == 0, "T6 all released");

  // ── W ──
  K::setPhysLifoForTest(1);
  K::setPhysLifoReuseForTest(1);
  K::setPhysFloorForTest(1);
  auto A = [](size_t s, uint32_t al) { return K::guestPhysAlloc(s, al); };
  auto F = [](uint32_t va) { return K::guestPhysLifoFree(va); };
  const size_t c0 = K::guestPhysNextForTest();
  const uint32_t boot = A(0x2000000u, 0x1000u);   // D3D objects etc. above the pool
  const uint32_t pool = A(0xDFF0000u, 0x8000u);   // the 224 MB world pool
  const uint32_t city = A(0x6000000u, 0x1000u);   // level data below it: < 224 MB left below
  check(boot && pool && city && (pool & 0x7FFFu) == 0, "W0 boot grants, the world pool, the city below");
  uint32_t plo = 0, phi = 0;
  resSpan(pool, plo, phi);
  check(F(pool) == LF::kNotTop, "W1 leaving for Warworld: the pool is freed (a free extent)");
  uint32_t stk[4] = {};
  {
    K::PhysKindScope pk(K::PhysKind::kThreadStack);
    for (auto& s : stk) s = A(0x40000u, 16u);   // the old placement: physical, top-down
  }
  bool inHole = true;
  for (uint32_t s : stk) inHole = inHole && s < pool + 0xDFF0000u && s + 0x40000u > pool;
  check(inHole, "W2 old placement: the four loader stacks land inside the freed pool's range");
  check(A(0xDFF0000u, 0x8000u) == 0, "W3 ...so the world pool request is refused (the Warworld return crash)");
  for (uint32_t s : stk) F(s);
  uint32_t xs[4] = {}, xb[4] = {};
  for (int i = 0; i < 4; i++) xs[i] = T::allocStack(0x40000u, &xb[i]);
  bool outside = true;
  for (uint32_t s : xs) outside = outside && s >= 0x70000000u && s < 0x7F000000u;
  const uint32_t again = A(0xDFF0000u, 0x8000u);
  check(outside && again && again >= plo && again + 0xDFF0000u <= phi,
        "W4 Xenia placement: stacks in 0x7xxxxxxx, the pool fits back into its old reservation");
  check(lifoInv("W5"), "W5 invariant");
  for (uint32_t v : xb) T::releaseStack(v);
  F(again);
  F(city);
  F(boot);
  check(K::guestPhysNextForTest() == c0 && retained() == 0 && lifoInv("W6"),
        "W6 all freed: the cursor is back at the origin");
  T::forceEnabledForTest(-1);
  K::setPhysLifoReuseForTest(-1);
  K::setPhysFloorForTest(-1);
  K::setPhysLifoForTest(-1);
  std::printf("[THREAD_MEM_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// ── iter-465 physmem: SR_PHYS_XENIA_PLACE (Xenia BaseHeap::AllocRange top-down) ──
// P1 a 64 KB-aligned bump grant takes the highest aligned position; the pages between it
// and the cursor stay free; P2 a later 4 KB grant fills that gap (Xenia packs it there,
// the old reservation would have held it as padding); P3 the invariant; P4 frees cascade
// back to the origin. Mutation: skipping the gap insert -> P2 fails.
int runPhysXeniaPlaceTest() {
  std::printf("[PHYS_XENIA_PLACE_TEST] exact top-down placement\n");
  g_pass = g_fail = 0;
  K::setPhysLifoForTest(1);
  K::setPhysLifoReuseForTest(1);
  K::setPhysFloorForTest(1);
  K::setPhysXeniaPlaceForTest(1);
  const size_t c0 = K::guestPhysNextForTest();
  auto A = [](size_t s, uint32_t al) { return K::guestPhysAlloc(s, al); };
  auto F = [](uint32_t va) { return K::guestPhysLifoFree(va); };
  const uint32_t a = A(0x3000, 0x1000);     // cursor now 0x3000 below the top (above the main stack)
  const uint32_t b = A(0x20000, 0x10000);   // 64 KB aligned: the highest aligned spot below a
  check(a == kTopVA - 0x3000u && b == ((a - 0x20000u) & ~0xFFFFu) && b + 0x20000u < a,
        "P1 the 64 KB grant sits at the highest aligned position below the cursor");
  const uint32_t gap = a - (b + 0x20000u);
  check(gap > 0 && retained() == gap, "P1b the alignment gap above it is a free extent");
  const uint32_t c = A(0x1000, 0x1000);
  check(c == a - 0x1000u && c >= b + 0x20000u, "P2 a later 4 KB grant fills the gap (top-down, highest fit)");
  check(lifoInv("P3"), "P3 invariant");
  F(c);
  F(b);
  F(a);
  check(K::guestPhysNextForTest() == c0 && retained() == 0 && lifoInv("P4"),
        "P4 all freed: the cursor is back at the origin");
  // SR_PHYS_BIG_GAP: a >= 16 MB cursor grant leaves 32 MB free above it; a smaller
  // grant made later lands in that gap (top-down), not below the big one.
  K::setPhysBigGapForTest(1);
  const uint32_t s0 = A(0x3000, 0x1000);
  const uint32_t big = A(0x1000000, 0x10000);
  check(big && big + 0x1000000u + 0x2000000u <= s0 && retained() >= 0x2000000u,
        "G1 the 16 MB grant sits 32 MB below the cursor; the gap is a free extent");
  const uint32_t later = A(0x10000, 0x10000);
  check(later > big && later < s0, "G2 a later 64 KB grant lands in the gap above the big grant");
  check(lifoInv("G3"), "G3 invariant");
  F(later);
  F(big);
  F(s0);
  check(K::guestPhysNextForTest() == c0 && retained() == 0 && lifoInv("G4"),
        "G4 all freed: the cursor is back at the origin");
  K::setPhysBigGapForTest(-1);
  K::setPhysXeniaPlaceForTest(-1);
  K::setPhysLifoReuseForTest(-1);
  K::setPhysFloorForTest(-1);
  K::setPhysLifoForTest(-1);
  std::printf("[PHYS_XENIA_PLACE_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
