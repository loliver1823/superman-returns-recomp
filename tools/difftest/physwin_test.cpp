// iter-458 H-WIN: the read-window census (kernel/phys_window.{hpp,cpp}) driven
// through the REAL allocators - guestPhysAlloc for the physical bump region and
// noteHeapRoutedGrant for the override's heap-routed physical grants.
//
// Run: build/difftest.exe --physwin-test   (exit code = failed checks)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/guest_context.hpp"
#include "kernel/phys_window.hpp"

#include <cstdio>
#include <cstring>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace W = sr::kernel::physwin;
namespace sr { namespace kernel {
int pickAliasWindowForTest(uint32_t physBase, uint64_t span, int resolveSite);   // hle_api_observe.cpp
} }

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
void fill(uint32_t va, uint8_t v, uint32_t n) {
  if (uint8_t* p = sr::kernel::guestVAToHost(va)) std::memset(p, v, n);
}
W::Counts delta(const W::Counts& a, const W::Counts& b) {
  return W::Counts{b.calls - a.calls,           b.ok - a.ok,
                   b.wrongDiffer - a.wrongDiffer, b.wrongSame - a.wrongSame,
                   b.bothDiffer - a.bothDiffer,   b.bothSame - a.bothSame,
                   b.none - a.none,             b.unreadable - a.unreadable};
}
// iter-463 E1: the double-book hook's witness.
int g_dbHookCalls = 0;
uint32_t g_dbHookNew = 0, g_dbHookOld = 0;
void dbHook(uint32_t newVa, uint32_t oldVa) {
  g_dbHookCalls++;
  g_dbHookNew = newVa;
  g_dbHookOld = oldVa;
}
bool only(const W::Counts& d, uint64_t W::Counts::*field) {
  W::Counts want{};
  want.calls = 1;
  want.*field = 1;
  return std::memcmp(&d, &want, sizeof(d)) == 0;
}
}  // namespace

int runPhysWinTest() {
  std::printf("[PHYSWIN_TEST] read-window census\n");
  W::resetForTest();
  const W::Site S = W::kIdxIb;

  // A physical-region allocation (grows down from 0xBF000000, so 0xA window).
  const uint32_t A = sr::kernel::guestPhysAlloc(0x20000, 0x1000);
  check(A >= 0xA0000000u && A < 0xBF000000u, "P0 guestPhysAlloc lands in the 0xA window");
  const uint32_t pA = A & 0x1FFFFFFFu;
  check(W::ownerOf(A, 0x1000) == W::kOwnPhysRegion, "P1 ownerOf(phys grant) = physRegion");
  check(W::ownerOf(0x80000000u | pA, 0x1000) == W::kOwnNone,
        "P2 the 0x8 alias of a physical grant is not owned");

  fill(A, 0xAB, 0x1000);
  fill(0x80000000u | pA, 0x11, 0x1000);
  W::Counts c0 = W::countsForTest(S);
  W::census(S, pA, 0x1000, /*chose8=*/false);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::ok), "C1 chose the owned 0xA window -> ok");

  c0 = W::countsForTest(S);
  W::census(S, pA, 0x1000, /*chose8=*/true);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::wrongDiffer),
        "C2 chose the unowned 0x8 alias with different bytes -> WRONG differ");

  fill(0x80000000u | pA, 0xAB, 0x1000);
  c0 = W::countsForTest(S);
  W::census(S, pA, 0x1000, /*chose8=*/true);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::wrongSame),
        "C3 same bytes in both windows -> WRONG same (harmless)");

  // A heap-routed physical grant in the 0x8 window.
  const uint32_t G = 0x8A000000u;
  W::noteHeapRoutedGrant(G, 0x10000);
  const uint32_t pG = G & 0x1FFFFFFFu;
  check(W::ownerOf(G, 0x10000) == W::kOwnHeapGrant, "P3 ownerOf(heap grant) = heapGrant");
  fill(G, 0x22, 0x10000);
  fill(0xA0000000u | pG, 0x33, 0x10000);
  c0 = W::countsForTest(S);
  W::census(S, pG, 0x1000, /*chose8=*/true);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::ok), "C4 heap grant read through 0x8 -> ok");
  c0 = W::countsForTest(S);
  W::census(S, pG, 0x1000, /*chose8=*/false);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::wrongDiffer),
        "C5 heap grant read through 0xA -> WRONG differ");

  // A span that runs past the end of the grant is not owned by it.
  c0 = W::countsForTest(S);
  W::census(S, pG + 0xF000u, 0x2000, /*chose8=*/true);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::none),
        "C6 a span straddling the end of a grant -> none (containment, not overlap)");

  // Neither window owned.
  c0 = W::countsForTest(S);
  W::census(S, 0x05000000u, 0x100, /*chose8=*/true);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::none), "C7 unregistered memory -> none");

  // Both windows owned: an ambiguous physical address.
  W::noteHeapRoutedGrant(0x80000000u | pA, 0x1000);
  fill(0x80000000u | pA, 0x44, 0x1000);
  c0 = W::countsForTest(S);
  W::census(S, pA, 0x1000, /*chose8=*/true);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::bothDiffer),
        "C8 both windows owned, bytes differ -> BOTH differ");
  fill(0x80000000u | pA, 0xAB, 0x1000);
  c0 = W::countsForTest(S);
  W::census(S, pA, 0x1000, /*chose8=*/false);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::bothSame),
        "C9 both windows owned, bytes equal -> BOTH same");

  // Window bits on the input are masked: a 0xA-form address classifies alike.
  c0 = W::countsForTest(S);
  W::census(S, 0xA0000000u | pG, 0x1000, /*chose8=*/true);
  check(only(delta(c0, W::countsForTest(S)), &W::Counts::ok), "C10 input window bits are masked");

  // Per-site counters are independent.
  const W::Counts vb = W::countsForTest(W::kIdxVb);
  check(vb.calls == 0, "C11 other sites untouched");

  check(!W::censusEnabled(), "C12 census is DEFAULT OFF (no SR_PHYSWIN_CENSUS)");

  // ── Astra 043: the PRECISE (grant) model + the payload sampler ──────────────
  {
    W::resetForTest();
    const W::Site G = W::kIdxVb;
    // An UNREGISTERED physical-region allocation (a thread stack uses this path).
    const uint32_t st = sr::kernel::guestPhysAlloc(0x10000, 0x1000);
    const uint32_t pS = st & 0x1FFFFFFFu;
    check(W::ownerOf(st, 0x1000) == W::kOwnPhysRegion && W::ownerOfGrant(st, 0x1000) == W::kOwnNone,
          "G1 an unregistered phys-region block: legacy owned, grant model NOT owned");
    const uint32_t gp = sr::kernel::guestPhysAlloc(0x10000, 0x1000);
    const uint32_t pP = gp & 0x1FFFFFFFu;
    W::notePhysGrant(gp, 0x10000);
    check(W::ownerOfGrant(gp, 0x1000) == W::kOwnPhysRegion &&
              W::ownerOfGrant(gp + 0xF000, 0x1001) == W::kOwnNone,
          "G2 a registered GPU grant is owned; a span past its end is not");

    fill(gp, 0x5A, 0x1000);
    fill(0x80000000u | pP, 0x3C, 0x1000);
    fill(0x80000000u | pS, 0x77, 0x1000);
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::strcat(tmp, "physwin_sample_test.bin");
    W::setSampleFileForTest(tmp);
    W::DrawCtx dc;
    dc.presentId = 4242; dc.prim = 4; dc.stride = 32; dc.count = 99;
    W::setDrawContext(dc, /*newDraw=*/true);
    W::census(G, pP, 0x1000, /*chose8=*/false);   // grant ok
    W::census(G, pP, 0x1000, /*chose8=*/true);    // grant WRONG
    W::census(G, pS, 0x1000, /*chose8=*/true);    // legacy WRONG, grant none
    W::GrantCounts gc = W::grantCountsForTest(G);
    check(gc.ok == 1 && gc.wrong == 1 && gc.none == 1 && gc.both == 0,
          "G3 grant categories: registered+chosen ok, registered+other WRONG, stack-only none");
    W::noteHeapRoutedGrant(0x80000000u | pP, 0x1000);
    W::census(G, pP, 0x1000, /*chose8=*/true);    // grant BOTH
    gc = W::grantCountsForTest(G);
    check(gc.both == 1, "G4 heap grant + phys grant at one physical address -> grant BOTH");

    // Budget: the first 2000 per stratum, then 1 in 1024 (seen 2048 -> +1).
    for (int i = 0; i < 2100; i++) W::census(G, pP, 0x1000, /*chose8=*/true);   // BOTH stratum
    const W::SampleStats ss = W::sampleStatsForTest();
    check(ss.seen[G][W::kGBoth] == 2101 && ss.sampled[G][W::kGBoth] == 2001,
          "G5 budget: 2101 seen in a stratum -> 2000 + one 1/1024 sample = 2001");
    W::setSampleFileForTest(nullptr);   // closes + flushes

    FILE* f = std::fopen(tmp, "rb");
    std::vector<W::SampleRec> recs;
    if (f) {
      W::SampleRec r;
      while (std::fread(&r, sizeof(r), 1, f) == 1) recs.push_back(r);
      std::fclose(f);
    }
    std::remove(tmp);
    check(recs.size() == 2004, "G6 file holds exactly the sampled records (3 + 2001), 220 B each");
    bool magicOk = !recs.empty();
    for (const auto& r : recs) magicOk &= std::memcmp(r.magic, "PWS1", 4) == 0;
    check(magicOk, "G7 every record carries the PWS1 magic");
    if (recs.size() >= 3) {
      const W::SampleRec& r0 = recs[0];
      const uint8_t* h8 = sr::kernel::guestVAToHost(0x80000000u | pP);
      const uint8_t* hA = sr::kernel::guestVAToHost(gp);
      check(r0.site == G && r0.catGrant == W::kGOk && (r0.flags & 1) == 0 && (r0.flags & 6) == 6 &&
                r0.phys == pP && r0.size == 0x1000 && r0.sampledLen == 64,
            "G8 record fields: site, grant category, chose8 flag, both readable, phys, size, sampledLen");
      check(r0.bytes8[0] == 0x3C && r0.bytes8[63] == 0x3C && r0.bytesA[0] == 0x5A && r0.bytesA[63] == 0x5A,
            "G9 both candidates' first 64 B captured from the right windows");
      check(r0.hash8 == W::fnv1a64ForTest(h8, 0x1000) && r0.hashA == W::fnv1a64ForTest(hA, 0x1000) &&
                r0.hash8 != r0.hashA,
            "G10 full-span FNV-1a-64 hashes of both candidates, taken at sampling time");
      check(r0.presentId == 4242 && r0.prim == 4 && r0.stride == 32 && r0.count == 99 &&
                r0.drawId == recs[1].drawId && r0.drawId != 0,
            "G11 draw context rides on the records (one draw id per setDrawContext(newDraw))");
      check(recs[2].catGrant == W::kGNone && recs[2].catLegacy == 2 && (recs[2].flags & 1) == 1,
            "G12 the stack-only read: legacy WRONG-differ, grant none - the refinement is recorded");
    }
    // An unreadable candidate never faults and records readable=0, hash 0.
    const uint32_t hole = 0xA6000000u;
    sr::kernel::hostRangeReadableBeginRemoval();
    VirtualFree(sr::kernel::guestVAToHost(hole), 0x1000, MEM_DECOMMIT);
    sr::kernel::hostRangeReadableEndRemoval();
    W::setSampleFileForTest(tmp);
    W::census(W::kS4Vb, hole & 0x1FFFFFFFu, 0x100, /*chose8=*/true);
    W::setSampleFileForTest(nullptr);
    f = std::fopen(tmp, "rb");
    W::SampleRec u{};
    const bool got = f && std::fread(&u, sizeof(u), 1, f) == 1;
    if (f) std::fclose(f);
    std::remove(tmp);
    check(got && (u.flags & 4) == 0 && u.hashA == 0 && (u.flags & 2) == 2,
          "G13 an unreadable candidate is flagged (readableA=0, hashA=0), no fault");
    sr::kernel::hostRangeReadableBeginRemoval();
    VirtualAlloc(sr::kernel::guestVAToHost(hole), 0x1000, MEM_COMMIT, PAGE_READWRITE);
    sr::kernel::hostRangeReadableEndRemoval();
    W::resetForTest();
  }

  // ── E1 (iter-463): the v1_gpuhang ambiguity, as a unit ────────────────────
  // The D3D command buffer (a 0xA physical grant) and the arena grant that
  // crossed below 0xA0000000 share physical pages 0x1E82B000..: the IB the CP
  // stalled on (phys 0x1E8B1A00, 296 bytes) has two owners. Mutation-validated:
  // the old 0x8 IB default -> E1d fails; no F2 check -> E1b fails.
  // SR_PM4_ENGINE_OWNER=0 (the pre-iter-460 A/B arm, latched per process):
  // ownership is NOT consulted - the legacy 0x8 fetch, never "unowned".
  const char* eOwner = std::getenv("SR_PM4_ENGINE_OWNER");
  if (eOwner && eOwner[0] == '0') {
    W::resetForTest();
    W::notePhysGrant(0xBE82B000u, 0x700000u);
    int v = 0;
    const uint64_t u0 = W::ibUnownedCount(), c0 = W::ibContestedCount();
    check(W::resolveIbWindow(0x1E8B1A00u, 296, &v) == 0x80000000u && v == W::kIbNotConsulted,
          "N1 owner gate off: not consulted - the legacy 0x8 IB fetch");
    W::noteIbWindow(v, 0x1E8B1A00u, 296, 0x80000000u);
    check(W::ibUnownedCount() == u0 && W::ibContestedCount() == c0, "N2 ...and never counted as unowned");
    W::resetForTest();
  } else {
    W::resetForTest();
    g_dbHookCalls = 0;
    W::setPhysDoubleBookHook(&dbHook);
    W::notePhysGrant(0xBE82B000u, 0x700000u);   // the D3D command buffer
    check(W::physDoubleBookCount() == 0 && g_dbHookCalls == 0, "E1a one grant: no double-book");
    W::notePhysGrant(0x9E820000u, 0x2B30000u);  // the reload's arena grant
    const W::PhysDoubleBook d = W::physDoubleBookLast();
    check(W::physDoubleBookCount() == 1 && W::physDoubleBookCountPhysPhys() == 1 && d.newVa == 0x9E820000u &&
              d.oldVa == 0xBE82B000u && d.newKind == 2 && d.oldKind == 2 && g_dbHookCalls == 1 &&
              g_dbHookNew == 0x9E820000u && g_dbHookOld == 0xBE82B000u,
          "E1b the second registration raises PHYS_DOUBLE_BOOK (count, record, hook)");
    check(W::gpuWindow(0x1E8B1A00u, 296) == 0, "E1c gpuWindow(0x1E8B1A00, 296) = 0: both windows own it");
    int v = 0;
    W::setIbWindowUnifiedForTest(1);
    check(W::resolveIbWindow(0x1E8B1A00u, 296, &v) == 0xA0000000u && v == W::kWinBoth,
          "E1d SR_IB_WINDOW_UNIFIED on: the contested IB is parsed from 0xA (physToHost's rule)");
    W::setIbWindowUnifiedForTest(0);
    check(W::resolveIbWindow(0x1E8B1A00u, 296, &v) == 0x80000000u && v == W::kWinBoth,
          "E1e gate off: the legacy 0x8 default - the arena's bytes (the hang)");
    W::setIbWindowUnifiedForTest(-1);
    if (!std::getenv("SR_IB_WINDOW_UNIFIED"))
      check(W::ibWindowUnifiedEnabled(), "E1f SR_IB_WINDOW_UNIFIED defaults ON");
    const uint64_t c0 = W::ibContestedCount(), u0 = W::ibUnownedCount();
    W::noteIbWindow(v, 0x1E8B1A00u, 296, 0xA0000000u);
    check(W::ibContestedCount() == c0 + 1 && W::ibUnownedCount() == u0, "E1g a contested IB is counted");
    // Owned and unowned IBs.
    W::notePhysGrant(0xB0000000u, 0x10000u);
    check(W::resolveIbWindow(0x10000100u, 64, &v) == 0xA0000000u && v == 0xA,
          "E1h an IB only a 0xA grant holds: 0xA");
    check(W::resolveIbWindow(0x05000000u, 64, &v) == 0xA0000000u && v == 0,
          "E1i an unowned IB: physToHost's default (0xA)");
    W::noteIbWindow(v, 0x05000000u, 64, 0xA0000000u);
    check(W::ibUnownedCount() == u0 + 1, "E1j an unowned IB is counted apart");
    W::noteHeapRoutedGrant(0x85000000u, 0x1000u);
    check(W::resolveIbWindow(0x05000000u, 64, &v) == 0x80000000u && v == 8,
          "E1k an IB a heap-routed grant holds: 0x8");
    // Reverse order, a released owner, a heap-routed owner, disjoint grants.
    W::resetForTest();
    W::notePhysGrant(0x9E820000u, 0x2B30000u);
    W::notePhysGrant(0xBE82B000u, 0x700000u);
    check(W::physDoubleBookCount() == 1 && W::physDoubleBookLast().oldVa == 0x9E820000u,
          "E1l either registration order is caught");
    W::resetForTest();
    W::notePhysGrant(0x9E820000u, 0x2B30000u);
    W::markPhysGrantReleased(0x9E820000u);
    W::notePhysGrant(0xBE82B000u, 0x700000u);
    check(W::physDoubleBookCount() == 0, "E1m a RELEASED owner is not a double-book");
    W::noteHeapRoutedGrant(0x9E8B0000u, 0x1000u);   // p 0x1E8B0000: the command buffer's page
    check(W::physDoubleBookCount() == 1 && W::physDoubleBookCountPhysPhys() == 0 &&
              W::physDoubleBookLast().newKind == 1,
          "E1n a heap-routed grant on a live physical grant's pages is a double-book (kind 1 vs 2)");
    W::notePhysGrant(0xB0000000u, 0x10000u);
    W::notePhysGrant(0xB0010000u, 0x10000u);
    check(W::physDoubleBookCount() == 1, "E1o adjacent disjoint grants are not");
    // forgetReleasedPhysGrantsIn: only released records overlapping the range go.
    W::markPhysGrantReleased(0xB0000000u);
    check(W::forgetReleasedPhysGrantsIn(0xB0008000u, 0x10000u) == 1 &&
              W::ownerOfGrant(0xB0000000u, 0x1000u) == W::kOwnNone &&
              W::ownerOfGrant(0xB0010000u, 0x1000u) == W::kOwnPhysRegion,
          "E1p forgetReleasedPhysGrantsIn drops the released record it overlaps, never a live one");
    // Review: the same VA granted twice while the first grant lives. The
    // registration used to skip its own key, so this counted nothing.
    W::resetForTest();
    g_dbHookCalls = 0;
    W::notePhysGrant(0xB2000000u, 0x10000u);
    W::notePhysGrant(0xB2000000u, 0x10000u);
    check(W::physDoubleBookCount() == 1 && W::physDoubleBookCountPhysPhys() == 1 &&
              W::physDoubleBookLast().sameVa == 1 && W::physDoubleBookLast().oldVa == 0xB2000000u &&
              g_dbHookCalls == 1,
          "E1q a live grant registered AGAIN at the same VA is a double-book (SAME-VA)");
    W::markPhysGrantReleased(0xB2000000u);
    W::notePhysGrant(0xB2000000u, 0x10000u);
    check(W::physDoubleBookCount() == 1, "E1r ...a released one at the same VA is not");
    // Review: the scan's predecessor shortcut after an overlap, census OFF.
    W::resetForTest();
    W::setUniqCensusForTest(0);
    W::notePhysGrant(0xB0000000u, 0x100000u);   // A
    W::notePhysGrant(0xB0020000u, 0x10000u);    // B inside A: counted
    check(W::physDoubleBookCount() == 1, "E1s an overlapping registration is counted (census off)");
    W::notePhysGrant(0xB0050000u, 0x1000u);     // C: predecessor B ends before it; A covers it
    check(W::physDoubleBookCount() == 2 && W::physDoubleBookLast().oldVa == 0xB0000000u,
          "E1t after an overlap the scan is exhaustive: C vs A (behind B) is counted");
    W::setUniqCensusForTest(-1);
    W::setPhysDoubleBookHook(nullptr);
    W::resetForTest();
  }

  std::printf("[PHYSWIN_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// ── Astra 044: GRANT-MODE resolver (SR_PHYS_RESOLVE_GRANTS=1, SR_PHYS_FREE off) ──
// Run: SR_PHYS_RESOLVE_GRANTS=1 build/difftest.exe --physresolve-grant-test
int runPhysResolveGrantTest() {
  std::printf("[PHYSRESOLVE_GRANT_TEST] grant-mode ownership resolver\n");
  g_pass = g_fail = 0;
  check(W::physResolveGrantMode() && W::physResolveEnabled(),
        "GR0 grant mode ACTIVE (run with SR_PHYS_RESOLVE_GRANTS=1, SR_PHYS_FREE unset)");
  W::resetForTest();
  using RC = W::ResolveCounts;
  auto rc = [](W::ResolveSite s) { return W::resolveCountsForTest(s); };
  auto fillW = [](uint32_t va, uint8_t v, uint32_t n) {
    std::memset(sr::kernel::guestVAToHost(va), v, n);
  };
  constexpr uint32_t kSpan = 0x1000u;
  {
    // GR1: the hwin1 WRONG case - a registered GPU physical grant (0xA), with
    // non-zero foreign bytes at 0x8|p. At an IN-SCOPE site (idx-vb) the resolver
    // names 0xA; legacy would read the non-zero 0x8.
    const uint32_t g = sr::kernel::guestPhysAlloc(kSpan, 0x1000);
    const uint32_t p = g & 0x1FFFFFFFu;
    W::notePhysGrant(g, kSpan);
    fillW(0x80000000u | p, 0x18, kSpan);
    fillW(0xA0000000u | p, 0x5A, kSpan);
    check(W::ownerWindow(p, kSpan) == W::kWinA, "GR1w ownerWindow = 0xA (the sole registered owner)");
    const RC v0 = rc(W::kRsIdxVb);
    check(W::resolveOwned(W::kRsIdxVb, p, 0x200) == 0xA && rc(W::kRsIdxVb).a == v0.a + 1,
          "GR1 WRONG case at idx-vb: the resolver names 0xA and counts it");
    const RC s0 = rc(W::kRsS4Vb), b0 = rc(W::kRsIdxIb);
    check(W::resolveOwned(W::kRsS4Vb, p, 0x200) == 0xA && W::resolveOwned(W::kRsIdxIb, p, 0x200) == 0xA &&
              rc(W::kRsS4Vb).a == s0.a + 1 && rc(W::kRsIdxIb).a == b0.a + 1,
          "GR2 ...and at s4-vb and idx-ib (all three in-scope sites)");
    // Astra 045 SCOPE: the texture selector keeps the legacy rule, counted.
    const RC c0 = rc(W::kRsTex);
    const int w = sr::kernel::pickAliasWindowForTest(p, kSpan, -1);
    const RC c1 = rc(W::kRsTex);
    check(w == 8 && c1.scopeSkip == c0.scopeSkip + 1 && c1.a == c0.a && c1.disagree == c0.disagree,
          "GR1t tex site OUT OF SCOPE: legacy choice (0x8), scopeSkip counted, no decision");
    const RC r0 = rc(W::kRsRefresh);
    check(W::resolveOwned(W::kRsRefresh, p, 0x200) == 0 && rc(W::kRsRefresh).scopeSkip == r0.scopeSkip + 1,
          "GR11 refresh site OUT OF SCOPE: no decision, scopeSkip counted");
    check(W::ownerWindow(p + kSpan - 4, 4) == W::kWinA && W::ownerWindow(p + kSpan - 4, 8) == W::kWinUnknown,
          "GR6 exact grant end owned; one dword past it -> unknown");
  }
  {
    // GR3: a heap-routed grant only (the 0xA side unregistered - e.g. a stack) -> 0x8.
    const uint32_t s = sr::kernel::guestPhysAlloc(kSpan, 0x1000);   // NOT registered
    const uint32_t p = s & 0x1FFFFFFFu;
    W::noteHeapRoutedGrant(0x80000000u | p, kSpan);
    check(W::ownerWindow(p, kSpan) == W::kWin8 && W::resolveOwned(W::kRsIdxVb, p, kSpan) == 8,
          "GR3 heap grant only: the resolver names 0x8");
  }
  {
    // GR4: both registered - a genuine double booking: legacy, counted BOTH.
    const uint32_t g = sr::kernel::guestPhysAlloc(kSpan, 0x1000);
    const uint32_t p = g & 0x1FFFFFFFu;
    W::notePhysGrant(g, kSpan);
    W::noteHeapRoutedGrant(0x80000000u | p, kSpan);
    const RC c0 = rc(W::kRsIdxVb);
    check(W::ownerWindow(p, kSpan) == W::kWinBoth && W::resolveOwned(W::kRsIdxVb, p, kSpan) == 0 &&
              rc(W::kRsIdxVb).both == c0.both + 1,
          "GR4 heap grant + phys grant: BOTH -> legacy (0), counted - never silently 0xA");
  }
  {
    // GR10 (Astra 045): the 0x8 side is itself a GPU PHYSICAL grant (the bump
    // region spills below 0xA0000000) and 0xA|p is another -> BOTH -> legacy.
    const uint32_t g = sr::kernel::guestPhysAlloc(kSpan, 0x1000);
    const uint32_t p = g & 0x1FFFFFFFu;
    W::notePhysGrant(g, kSpan);
    W::notePhysGrant(0x80000000u | p, kSpan);   // a physical grant at the 0x8 alias
    const RC c0 = rc(W::kRsS4Vb);
    check(W::ownerWindow(p, kSpan) == W::kWinBoth && W::resolveOwned(W::kRsS4Vb, p, kSpan) == 0 &&
              rc(W::kRsS4Vb).both == c0.both + 1,
          "GR10 physical grants at BOTH aliases: BOTH -> legacy (the allocator's own double booking)");
  }
  {
    // GR5: neither registered -> unknown -> legacy.
    const uint32_t s = sr::kernel::guestPhysAlloc(kSpan, 0x1000);   // NOT registered
    const uint32_t p = s & 0x1FFFFFFFu;
    const RC c0 = rc(W::kRsIdxVb);
    check(W::ownerWindow(p, kSpan) == W::kWinUnknown && W::resolveOwned(W::kRsIdxVb, p, kSpan) == 0 &&
              rc(W::kRsIdxVb).unknown == c0.unknown + 1,
          "GR5 neither registered: unknown -> legacy (0), counted unknown");
  }
  {
    // GR7: the tex selector is out of scope - even with the registered 0xA owner
    // unreadable it keeps the legacy read (no failed read, no decision).
    const uint32_t g = sr::kernel::guestPhysAlloc(kSpan, 0x1000);
    const uint32_t p = g & 0x1FFFFFFFu;
    W::notePhysGrant(g, kSpan);
    fillW(0x80000000u | p, 0x3C, kSpan);
    uint8_t* hA = sr::kernel::guestVAToHost(0xA0000000u | p);
    sr::kernel::hostRangeReadableBeginRemoval();
    VirtualFree(hA, kSpan, MEM_DECOMMIT);
    sr::kernel::hostRangeReadableEndRemoval();
    const RC c0 = rc(W::kRsTex);
    const int w = sr::kernel::pickAliasWindowForTest(p, kSpan, -1);
    check(w == 8 && rc(W::kRsTex).validBad == c0.validBad,
          "GR7 tex out of scope: the legacy 0x8 read even with the 0xA owner unreadable");
    sr::kernel::hostRangeReadableBeginRemoval();
    VirtualAlloc(hA, kSpan, MEM_COMMIT, PAGE_READWRITE);
    sr::kernel::hostRangeReadableEndRemoval();
  }
  {
    // GR8/GR9: the sampler carries THIS read's legacy choice, never a stale one.
    const uint32_t g = sr::kernel::guestPhysAlloc(kSpan, 0x1000);
    const uint32_t p = g & 0x1FFFFFFFu;
    W::notePhysGrant(g, kSpan);
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::strcat(tmp, "physresolve_grant_test.bin");
    W::setSampleFileForTest(tmp);
    W::resolveCompare(W::kRsIdxVb, 0xA, 8, true, true);
    W::census(W::kIdxVb, p, 0x100, /*chose8=*/false);   // decided read: flags 16|32
    W::census(W::kIdxVb, p, 0x100, /*chose8=*/false);   // no decision: no 16
    W::resolveCompare(W::kRsIdxIb, 0xA, 8, true, true);
    W::census(W::kS4Vb, p, 0x100, /*chose8=*/false);    // other site: consumed, not applied
    W::census(W::kIdxIb, p, 0x100, /*chose8=*/false);   // ...and not carried to a later read
    W::setSampleFileForTest(nullptr);
    std::vector<W::SampleRec> recs;
    if (FILE* f = std::fopen(tmp, "rb")) {
      W::SampleRec r;
      while (std::fread(&r, sizeof(r), 1, f) == 1) recs.push_back(r);
      std::fclose(f);
    }
    std::remove(tmp);
    check(recs.size() == 4, "GR8a four sampled reads");
    if (recs.size() == 4) {
      check((recs[0].flags & 48) == 48 && (recs[0].flags & 1) == 0,
            "GR8 a decided read records resolver-decided + legacy 0x8, selected 0xA");
      check((recs[1].flags & 16) == 0, "GR9a the next read at the site carries no stale decision");
      check((recs[2].flags & 16) == 0 && (recs[3].flags & 16) == 0,
            "GR9b a decision is consumed by the next census, never applied to another site or carried");
    }
  }
  W::resetForTest();
  std::printf("[PHYSRESOLVE_GRANT_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// ── iter-458 (Astra 054): the PWS2 per-draw window capture ─────────────────
// Run: build/difftest.exe --physwin-win-test   (exit code = failed checks)
namespace {
bool g_winOpen = false;
bool winProbe() { return g_winOpen; }
long fileSize(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return -1;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fclose(f);
  return n;
}
std::vector<W::WinRec> readWin(const char* path) {
  std::vector<W::WinRec> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) return v;
  W::WinRec r;
  while (std::fread(&r, sizeof(r), 1, f) == 1) v.push_back(r);
  std::fclose(f);
  return v;
}
}  // namespace

int runPhysWinCaptureTest() {
  std::printf("[PHYSWIN_WIN_TEST] PWS2 per-draw window capture\n");
  g_pass = g_fail = 0;
  char dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  const std::string f1 = std::string(dir) + "sr_pws2_test1.bin";
  const std::string f2 = std::string(dir) + "sr_pws2_test2.bin";
  const std::string f0 = std::string(dir) + "sr_pws2_test0.bin";
  std::remove(f0.c_str()); std::remove(f1.c_str()); std::remove(f2.c_str());
  check(sizeof(W::WinRec) == 384, "WC0 WinRec is the 384-byte offline contract");

  W::resetForTest();
  constexpr uint32_t kSpan = 0x1000u;
  const uint32_t g = sr::kernel::guestPhysAlloc(kSpan, 0x1000);
  const uint32_t p = g & 0x1FFFFFFFu;
  // A double-booked range: a heap-routed grant on the 0x8 side registered
  // FIRST, a GPU physical grant on the 0xA side registered SECOND.
  W::noteHeapRoutedGrant(0x80000000u | p, kSpan);
  W::notePhysGrant(0xA0000000u | p, kSpan);
  fill(0x80000000u | p, 0x11, kSpan);
  fill(0xA0000000u | p, 0x22, kSpan);

  // WC1: gate off -> nothing captured, nothing written.
  W::setWinCaptureForTest(nullptr, nullptr);
  for (int i = 0; i < 4; i++) W::census(W::kS4Vb, p, 0x200, true);
  check(W::winFlushedForTest() == 0 && fileSize(f0.c_str()) < 0,
        "WC1 gate off: no capture, no file");

  // WC2: 3 reads before the window, 5 inside, then the first read after close
  // flushes exactly the 8 appended records; later reads never rewrite.
  W::setWinCaptureForTest(f1.c_str(), &winProbe);
  g_winOpen = false;
  W::DrawCtx dc;
  dc.stride = 36; dc.count = 96; dc.prim = 4;
  W::setDrawContext(dc, /*newDraw=*/true);
  for (int i = 0; i < 3; i++) W::census(W::kS4Vb, p, 0x200, true);
  g_winOpen = true;
  for (int i = 0; i < 5; i++) W::census(W::kIdxVb, p, 0x200, false);
  check(W::winFlushedForTest() == 0 && fileSize(f1.c_str()) < 0,
        "WC2a nothing is written while the window is open");
  g_winOpen = false;
  W::census(W::kS4Vb, p, 0x200, true);
  check(W::winFlushedForTest() == 8 && fileSize(f1.c_str()) == 8 * 384,
        "WC2b the first read after close flushes exactly the 8 appended records");
  for (int i = 0; i < 4; i++) W::census(W::kS4Vb, p, 0x200, true);
  // A re-open that would APPEND and a second close that would RE-FLUSH: a
  // not-once implementation rewrites the file with more than 8 records here
  // (an identical rewrite of the same 8 would otherwise be invisible).
  g_winOpen = true;
  W::census(W::kS4Vb, p, 0x200, true);
  W::census(W::kS4Vb, p, 0x200, true);
  g_winOpen = false;
  W::census(W::kS4Vb, p, 0x200, true);
  check(W::winFlushedForTest() == 8 && fileSize(f1.c_str()) == 8 * 384,
        "WC2c flush happens ONCE: a later re-open + close never rewrites");

  // WC3: record contents.
  const std::vector<W::WinRec> v = readWin(f1.c_str());
  bool magic = v.size() == 8, seqOk = v.size() == 8, ctxOk = true, sites = true;
  for (size_t i = 0; i < v.size(); i++) {
    magic = magic && std::memcmp(v[i].magic, "PWS2", 4) == 0;
    seqOk = seqOk && v[i].seq == i + 1;
    ctxOk = ctxOk && v[i].stride == 36 && v[i].count == 96 && v[i].prim == 4 &&
            v[i].phys == p && v[i].size == 0x200;
    sites = sites && v[i].site == (i < 3 ? W::kS4Vb : W::kIdxVb) &&
            ((v[i].flags & 1) != 0) == (i < 3);
  }
  check(magic && seqOk, "WC3a magic PWS2 and seq 1..8 in capture order");
  check(ctxOk, "WC3b draw context (prim/stride/count) and phys/size recorded");
  check(sites, "WC3c site and the chose-0x8 flag recorded per read");
  if (!v.empty()) {
    const W::WinRec& r = v[3];
    check(r.catGrant == W::kGBoth, "WC3d a double-booked read is classified BOTH");
    check(r.g8.kind == 1 && r.gA.kind == 2 && r.g8.start == (0x80000000u | p) &&
              r.gA.start == (0xA0000000u | p) && r.g8.coversSpan == 1 && r.gA.coversSpan == 1,
          "WC3e each window names its own grant: heap-routed on 0x8, physical on 0xA");
    check(r.g8.serial != 0 && r.gA.serial == r.g8.serial + 1,
          "WC3f allocation serials record registration order across both registries");
    bool b8 = true, bA = true;
    for (int k = 0; k < 128; k++) { b8 = b8 && r.bytes8[k] == 0x11; bA = bA && r.bytesA[k] == 0x22; }
    uint8_t ref8[512];
    std::memset(ref8, 0x11, sizeof(ref8));
    check(b8 && bA && r.hash8 == W::fnv1a64ForTest(ref8, 512) && (r.flags & 6) == 6,
          "WC3g both candidates' bytes and the 512-byte hash are captured");
    check(r.drawId != 0, "WC3h the producer draw serial is attached");
  }

  // WC4: ring overflow keeps the NEWEST kWinRing records.
  W::setWinCaptureForTest(f2.c_str(), &winProbe);
  g_winOpen = true;
  const size_t over = W::kWinRing + 10;
  for (size_t i = 0; i < over; i++) W::census(W::kS4Vb, p, 64, true);
  g_winOpen = false;
  W::census(W::kS4Vb, p, 64, true);
  const std::vector<W::WinRec> v2 = readWin(f2.c_str());
  check(W::winFlushedForTest() == W::kWinRing && v2.size() == W::kWinRing &&
            v2.front().seq == 11 && v2.back().seq == over,
        "WC4 overflow: exactly the newest kWinRing records, oldest first");

  // WC5: the unregistered side of a read is kind 0 (no owner), not a guess.
  W::setWinCaptureForTest(f0.c_str(), &winProbe);
  const uint32_t g2 = sr::kernel::guestPhysAlloc(kSpan, 0x1000);
  const uint32_t p2 = g2 & 0x1FFFFFFFu;
  W::notePhysGrant(0xA0000000u | p2, kSpan);
  g_winOpen = true;
  W::census(W::kS4Vb, p2, 0x100, false);
  g_winOpen = false;
  W::census(W::kS4Vb, p2, 0x100, false);
  const std::vector<W::WinRec> v3 = readWin(f0.c_str());
  check(v3.size() == 1 && v3[0].g8.kind == 0 && v3[0].g8.serial == 0 && v3[0].gA.kind == 2,
        "WC5 an unregistered window records kind 0, serial 0");

  W::setWinCaptureForTest(nullptr, nullptr);
  W::resetForTest();
  std::remove(f0.c_str()); std::remove(f1.c_str()); std::remove(f2.c_str());
  std::printf("[PHYSWIN_WIN_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
