// Step 6 I7 (render/gate3/FREEZE_I7_20261022.txt; Fable 30 / 45): native_flush's segment-record arena (SR_SEG_ARENA) -
// records bump-allocated in refcounted arenas and held by aliasing pointers, the arena replaced at each segment's close,
// its chunks released only when its LAST record goes (to a bounded free list, poisoned under ASan), the retention cap,
// the audit's recycled nodes, and the =0 arm (one allocation per record). Step 6 I12: the reclaim audit's pos-ordered
// ring (SR_AUDIT_RING, the default) against the map it replaces (=0), its counted fallbacks and its compaction (A11-A13).
//
// Run: build/difftest.exe --seg-arena-test               (exit = failed checks)
//      build/difftest.exe --seg-arena-test early-release (I7 (4)'s negative control: in an SR_ASAN_TESTS=ON build with
//      SR_SEG_ARENA_KEEP=0 it must STOP with an ASan heap-use-after-free report; reaching its end prints NOT CAUGHT, exit 3)
// The byte identity of =1 and =0 (condition 5) is checked across processes: the segment / bracket suites run under both
// gates must print the same [SEG_ARENA] final capture_hash / parts_hash.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/native_flush.hpp"
#include "kernel/guest_context.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace F = sr::native::flush;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
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
// test positions far above any real ring position (no collision with a live audit entry)
constexpr uint64_t kPos = 1ull << 60;

#if SR_ORACLE
int runEarlyRelease() {
  F::segArenaForceForTest(1, -1);
  std::printf("[SEG_ARENA_TEST] I7 negative control early-release (asan=%s, SR_SEG_ARENA_KEEP=%s): the read must stop "
              "with an ASan report\n", kAsan ? "on" : "off", std::getenv("SR_SEG_ARENA_KEEP") ? std::getenv("SR_SEG_ARENA_KEEP") : "unset");
  std::fflush(stdout);
  const int v = F::segArenaEarlyReleaseForTest();
  std::printf("[SEG_ARENA_TEST] NOT CAUGHT: early-release read a released record and returned (byte 0x%02x) - %s\n", v,
              kAsan ? "ASan did not stop it: I7 (4) FAILS" : "no ASan in this build (the control needs SR_ASAN_TESTS=ON)");
  return 3;
}
#endif
}  // namespace

int runSegArenaTest(const char* mode) {
#if SR_ORACLE
  if (mode) {
    if (std::strcmp(mode, "early-release") == 0) return runEarlyRelease();
    std::printf("[SEG_ARENA_TEST] usage: --seg-arena-test [early-release]\n");
    return 2;
  }
  std::printf("[SEG_ARENA_TEST] native_flush segment-record arena (step 6 I7, SR_SEG_ARENA) asan=%s\n", kAsan ? "on" : "off");
  F::segArenaForceForTest(1, 1);
  F::segArenaCloseForTest();  // a fresh open arena for the checks below

  // A1: records go into the open arena (one arena, no per-record allocation) and come back intact
  auto c0 = F::segArenaCounters();
  std::vector<uint64_t> t;
  for (int i = 0; i < 8; i++) t.push_back(F::segArenaCaptureForTest(60u + i, kPos + i, uint8_t(0x30 + i)));
  auto c1 = F::segArenaCounters();
  check(c1.records - c0.records == 8 && c1.solo == c0.solo && c1.arenas - c0.arenas == 1,
        "A1 8 records in ONE new arena, none on the per-record path");
  bool intact = true;
  for (int i = 0; i < 8; i++) intact = intact && F::segArenaRecordIsForTest(t[i], 60u + i, uint8_t(0x30 + i));
  check(intact, "A1b every record's bytes as captured, the pad bytes zero");

  // A2: the segment's close retires the open arena; it LIVES while any record (here the test's and the audit's) is held
  F::segArenaCloseForTest();
  auto c2 = F::segArenaCounters();
  check(c2.arenas == c1.arenas && c2.retained > c1.retained, "A2 the close retires the arena (counted), it stays alive");
  for (int i = 0; i < 8; i++) F::segArenaReleaseForTest(t[i]);
  auto c3 = F::segArenaCounters();
  check(c3.arenas == c2.arenas, "A2b the test's references gone, the audit still holds the records: the arena lives");
  for (int i = 0; i < 7; i++) F::segArenaReclaimForTest(kPos + i);
  check(F::segArenaCounters().arenas == c2.arenas, "A2c 7 of 8 reclaimed: the arena lives (its last record is held)");
  F::segArenaReclaimForTest(kPos + 7);
  auto c4 = F::segArenaCounters();
  check(c4.arenas == c2.arenas - 1 && c4.retained < c2.retained && c4.freeChunks > c2.freeChunks,
        "A2d the LAST record goes: the arena dies, its chunk goes to the free list, the retained total drops");

  // A3: the next arena takes its chunk from the free list; the audit's nodes are recycled
  F::segArenaCloseForTest();
  auto c5 = F::segArenaCounters();
  uint64_t u = F::segArenaCaptureForTest(100u, kPos + 100, 0x77);
  auto c6 = F::segArenaCounters();
  check(c6.chunkReuse > c5.chunkReuse && c6.nodeReuse > c5.nodeReuse, "A3 a recycled chunk and a recycled audit node");
  check(F::segArenaRecordIsForTest(u, 100u, 0x77), "A3b a record in a recycled chunk reads as captured");
  F::segArenaReleaseForTest(u);
  F::segArenaReclaimForTest(kPos + 100);

  // A4: an oversized record (> a 64 KiB chunk) gets an exact-size chunk, never kept
  F::segArenaCloseForTest();
  auto c7 = F::segArenaCounters();
  u = F::segArenaCaptureForTest(70u << 10, kPos + 200, 0x11);
  check(F::segArenaCounters().oversized == c7.oversized + 1 && F::segArenaRecordIsForTest(u, 70u << 10, 0x11),
        "A4 a 70 KiB record: one exact-size chunk, intact");
  F::segArenaReleaseForTest(u);
  F::segArenaReclaimForTest(kPos + 200);
  F::segArenaCloseForTest();
  check(F::segArenaCounters().chunkFreed > c7.chunkFreed, "A4b the exact-size chunk is freed, not kept");

  // A5: the free list is bounded (64 chunks / 4 MiB): 80 arenas of one 64 KiB chunk each released at once
  {
    std::vector<uint64_t> big;
    for (int i = 0; i < 80; i++) {
      big.push_back(F::segArenaCaptureForTest(40u << 10, kPos + 1000 + i, 0x22));  // 40 KiB: a 64 KiB chunk each
      F::segArenaCloseForTest();
    }
    for (int i = 0; i < 80; i++) {
      F::segArenaReleaseForTest(big[i]);
      F::segArenaReclaimForTest(kPos + 1000 + i);
    }
    auto c8 = F::segArenaCounters();
    check(c8.freeChunks <= 64 && c8.freeBytes <= (4u << 20), "A5 the free list holds at most 64 chunks / 4 MiB");
  }

  // A6: the retention cap (32 MiB): 600 retired arenas of 64 KiB held = 37.5 MiB > the cap -> a reported fault and the
  // per-record path; released -> back under the cap, arena records again
  {
    auto c9 = F::segArenaCounters();
    std::vector<uint64_t> held;
    for (int i = 0; i < 600; i++) {
      held.push_back(F::segArenaCaptureForTest(40u << 10, kPos + 5000 + i, 0x33));
      F::segArenaCloseForTest();
    }
    auto c10 = F::segArenaCounters();
    check(c10.capFaults > c9.capFaults && c10.solo > c9.solo && c10.retained <= (32ull << 20),
          "A6 over the 32 MiB cap: a reported fault, records on the per-record path, retention never over the cap");
    for (size_t i = 0; i < held.size(); i++) {
      F::segArenaReleaseForTest(held[i]);
      F::segArenaReclaimForTest(kPos + 5000 + i);
    }
    F::segArenaCloseForTest();
    auto c11 = F::segArenaCounters();
    u = F::segArenaCaptureForTest(64u, kPos + 9000, 0x44);
    auto c12 = F::segArenaCounters();
    check(c11.retained < (32ull << 20) && c12.solo == c11.solo, "A6b released: under the cap, records in arenas again");
    F::segArenaReleaseForTest(u);
    F::segArenaReclaimForTest(kPos + 9000);
  }

  // A9 (Astra I7 P1-2, failing on 20cc67a8): records filling one OPEN arena up to the cap must not strand it - a small
  // first record, then 64 KiB-chunk records without a close until the cap sends them to the per-record path; then a
  // close, everything released: the arena retires and dies, and the next record is in an arena again.
  {
    F::segArenaCapForTest(1u << 20);  // a 1 MiB cap: the same arithmetic as 32 MiB, sixteen chunks
    F::segArenaCloseForTest();
    auto a0 = F::segArenaCounters();
    std::vector<uint64_t> held;
    held.push_back(F::segArenaCaptureForTest(64u, kPos + 20000, 0x61));
    for (int i = 0; i < 20; i++) held.push_back(F::segArenaCaptureForTest(65520u, kPos + 20001 + i, 0x62));
    auto a1 = F::segArenaCounters();
    check(a1.solo > a0.solo && a1.capFaults > a0.capFaults && a1.bytes - a0.bytes <= (1u << 20),
          "A9 the open arena stops at the cap (per-record path, a fault), never past it");
    F::segArenaCloseForTest();
    for (size_t i = 0; i < held.size(); i++) {
      F::segArenaReleaseForTest(held[i]);
      F::segArenaReclaimForTest(i == 0 ? kPos + 20000 : kPos + 20000 + i);
    }
    F::segArenaCloseForTest();
    auto a2 = F::segArenaCounters();
    u = F::segArenaCaptureForTest(64u, kPos + 20100, 0x63);
    auto a3 = F::segArenaCounters();
    check(a2.arenas == a0.arenas && a3.solo == a2.solo && a3.arenas == a2.arenas + 1,
          "A9b everything released: the full arena retired and died (not stranded), the next record is in a new arena");
    F::segArenaReleaseForTest(u);
    F::segArenaReclaimForTest(kPos + 20100);
    F::segArenaCloseForTest();
    // A10 (Astra I7 P1-3, failing on 20cc67a8): every SR_ORACLE cap entry is reported - 17 entries, 17 lines
    auto b0 = F::segArenaCounters();
    for (int round = 0; round < 17; round++) {
      std::vector<uint64_t> h2;
      for (int i = 0; i < 20; i++) {
        h2.push_back(F::segArenaCaptureForTest(40u << 10, kPos + 30000 + round * 100 + i, 0x64));
        F::segArenaCloseForTest();
      }
      for (int i = 0; i < 20; i++) {
        F::segArenaReleaseForTest(h2[i]);
        F::segArenaReclaimForTest(kPos + 30000 + round * 100 + i);
      }
      F::segArenaCloseForTest();
      F::segArenaReleaseForTest(F::segArenaCaptureForTest(64u, kPos + 39000 + round, 0x65));  // under the cap: reset
      F::segArenaReclaimForTest(kPos + 39000 + round);
      F::segArenaCloseForTest();
    }
    auto b1 = F::segArenaCounters();
    check(b1.capFaults - b0.capFaults == 17 && b1.capLogged - b0.capLogged == 17,
          "A10 17 cap entries: 17 faults, 17 ERROR lines (none silent)");
    F::segArenaCapForTest(0);
  }

  // A7: SR_SEG_ARENA=0 - one allocation per record, no arena, the same bytes
  F::segArenaForceForTest(0, 1);
  {
    auto a = F::segArenaCounters();
    u = F::segArenaCaptureForTest(60u, kPos + 9100, 0x55);
    auto b = F::segArenaCounters();
    check(b.arenas == a.arenas && b.records == a.records + 1 && F::segArenaRecordIsForTest(u, 60u, 0x55),
          "A7 =0: a record with no arena, intact");
    F::segArenaReleaseForTest(u);
    F::segArenaReclaimForTest(kPos + 9100);
  }
  F::segArenaForceForTest(-1, -1);

  // I12 (SR_AUDIT_RING): the reclaim audit's ring against the map it replaces - the same script under both finds the
  // same entries (a reclaim's return is the record's CP flag - false for both a hit and a miss here - so the observable
  // is the audit's hit counter, either arm) and ends holding the same; and the ring's counted fallbacks
  auto hit = [](uint64_t pos) {
    const uint64_t h0 = F::segArenaCounters().auditHits;
    F::segArenaReclaimForTest(pos);
    return int(F::segArenaCounters().auditHits - h0);
  };
  auto script = [&hit](int ring, std::vector<int>& seq, F::SegArenaCounters& after) {
    F::auditRingForceForTest(ring);
    F::segArenaForceForTest(1, 1);
    const uint64_t b = kPos + 50000 + uint64_t(ring) * 10000;
    std::vector<uint64_t> tok;
    for (int i = 0; i < 40; i++) tok.push_back(F::segArenaCaptureForTest(48u, b + 8u * i, uint8_t(0x70 + (i & 7))));
    for (uint64_t t : tok) F::segArenaReleaseForTest(t);   // only the audit holds them now
    seq.clear();
    for (int i = 0; i < 10; i++) seq.push_back(hit(b + 8u * i));   // in order
    seq.push_back(hit(b + 8u * 20));   // out of order (a tombstone in the ring)
    seq.push_back(hit(b + 8u * 20));   // again: a miss now
    seq.push_back(hit(b + 3));         // never captured: a miss
    for (int i = 10; i < 40; i++)
      if (i != 20) seq.push_back(hit(b + 8u * i));
    after = F::segArenaCounters();
    F::segArenaForceForTest(-1, -1);
    F::auditRingForceForTest(-1);
  };
  {
    std::vector<int> sm, sr;
    F::SegArenaCounters am, ar;
    const auto r0 = F::segArenaCounters();
    script(0, sm, am);
    script(1, sr, ar);
    int ones = 0;
    for (int v : sr) ones += v;
    check(sm == sr && sm.size() == 42 && ones == 40 && sr[11] == 0 && sr[12] == 0,
          "A11 the same capture / reclaim script: the ring finds what the map finds (40 hits, the 2 misses in place)");
    check(ar.auditMisses - am.auditMisses == 2 && ar.ringTombs - r0.ringTombs == 1 && am.ringTombs == r0.ringTombs,
          "A11b the ring's out-of-order reclaim: one tombstone (the map arm none), 2 misses");
    check(ar.ringN == r0.ringN && am.mapN == r0.mapN, "A11c every entry released: the ring and the map back to their size");
    // the counted fallbacks: an out-of-order insert and a duplicate pos (the map's emplace no-op: the first stays)
    F::auditRingForceForTest(1);
    F::segArenaForceForTest(1, 1);
    const auto s0 = F::segArenaCounters();
    const uint64_t b = kPos + 80000;
    uint64_t t1 = F::segArenaCaptureForTest(32u, b + 16, 0x01);
    uint64_t t2 = F::segArenaCaptureForTest(32u, b + 8, 0x02);    // below the newest: a sorted insert
    uint64_t t3 = F::segArenaCaptureForTest(32u, b + 16, 0x03);   // the same pos again: no-op
    const auto s1 = F::segArenaCounters();
    check(s1.ringSlow - s0.ringSlow == 2 && s1.ringDupes - s0.ringDupes == 1 && s1.ringN - s0.ringN == 2,
          "A12 an out-of-order insert and a duplicate pos: counted, the first record of a pos stays");
    F::segArenaReleaseForTest(t1);
    F::segArenaReleaseForTest(t2);
    F::segArenaReleaseForTest(t3);
    const bool h8 = F::segArenaReclaimForTest(b + 8), h16 = F::segArenaReclaimForTest(b + 16);
    const auto s2 = F::segArenaCounters();
    check(s2.auditHits - s1.auditHits == 2 && s2.ringN == s0.ringN && !h8 && !h16,
          "A12b both reclaimed in pos order (never executed by a CP: false, as the map), the ring back to its size");
    F::segArenaForceForTest(-1, -1);
    F::auditRingForceForTest(-1);
  }
  // A13: an entry the reclaim walks past (a stuck front, as after a header desync) - every later reclaim a tombstone:
  // the ring COMPACTS, stays bounded by its live entries, finds every entry after a compaction, and still holds the
  // stuck one (found when it is reclaimed), as the map did
  auto stuck = [&hit](int ring, std::vector<int>& seq, F::SegArenaCounters& mid, F::SegArenaCounters& after) {
    F::auditRingForceForTest(ring);
    F::segArenaForceForTest(1, 1);
    const uint64_t b = kPos + 100000 + uint64_t(ring) * 100000;
    std::vector<uint64_t> tok;
    for (int i = 0; i < 600; i++) tok.push_back(F::segArenaCaptureForTest(32u, b + 8u * i, uint8_t(0x90 + (i & 7))));
    for (uint64_t t : tok) F::segArenaReleaseForTest(t);
    seq.clear();
    for (int i = 1; i < 600; i++) seq.push_back(hit(b + 8u * i));   // the walk passed b + 0
    mid = F::segArenaCounters();
    seq.push_back(hit(b));
    after = F::segArenaCounters();
    F::segArenaForceForTest(-1, -1);
    F::auditRingForceForTest(-1);
  };
  {
    std::vector<int> sm, sr;
    F::SegArenaCounters m0, m1, r1, r2;
    const auto q0 = F::segArenaCounters();
    stuck(0, sm, m0, m1);
    stuck(1, sr, r1, r2);
    int ones = 0;
    for (int v : sr) ones += v;
    check(sm == sr && ones == 600, "A13 a stuck front: the ring finds all 600 entries, as the map");
    check(r1.ringCompacts > q0.ringCompacts && r1.ringSlots <= q0.ringSlots + 64 && r1.ringN == q0.ringN + 1 &&
              m0.mapN == q0.mapN + 1,
          "A13b the tombstones compacted: the ring bounded (<= 64 slots), both holding the stuck entry only");
    check(r2.ringN == q0.ringN && m1.mapN == q0.mapN, "A13c the stuck entry reclaimed at last: both back to their size");
  }

  const auto e = F::segArenaCounters();
  std::printf("[SEG_ARENA_TEST] counters records=%llu solo=%llu arenas=%llu chunks=%llu reuse=%llu freed=%llu bytes=%llu "
              "retained=%llu oversized=%llu node_reuse=%llu cap_faults=%llu bad=%llu free=%llu/%llu hash=%s checks=%llu "
              "verifies=%llu fills=%llu\n",
              (unsigned long long)e.records, (unsigned long long)e.solo, (unsigned long long)e.arenas,
              (unsigned long long)e.chunks, (unsigned long long)e.chunkReuse, (unsigned long long)e.chunkFreed,
              (unsigned long long)e.bytes, (unsigned long long)e.retained, (unsigned long long)e.oversized,
              (unsigned long long)e.nodeReuse, (unsigned long long)e.capFaults, (unsigned long long)e.bad,
              (unsigned long long)e.freeChunks, (unsigned long long)e.freeBytes, F::segArenaHashOnForTest() ? "on" : "off",
              (unsigned long long)e.checks, (unsigned long long)e.verifies, (unsigned long long)e.fills);
  check(e.bad == 0, "A8 no record changed after its capture (SR_ORACLE checksums)");
  // L1 (step 6 I15 P2, J:/sr_lanec_drawthread/FREEZE_I15_20261024.txt): the known-mapped plain read's GUARD - a
  // readable page copies; a page made NOACCESS faults INSIDE the guard (caught: false), never raised to the process.
  // Failing-first: a bare memcpy in its place stops the process here.
  {
    uint8_t* pg = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    bool okRead = false, caught = false;
    if (pg) {
      std::memset(pg, 0x5C, 64);
      uint8_t out[64] = {};
      okRead = F::guardedCopyForTest(out, pg, 64u) && out[0] == 0x5C && out[63] == 0x5C;
      DWORD old = 0;
      if (VirtualProtect(pg, 4096, PAGE_NOACCESS, &old)) caught = !F::guardedCopyForTest(out, pg, 64u);
      VirtualFree(pg, 0, MEM_RELEASE);
    }
    check(okRead && caught, "L1 I15's guarded copy: a readable page copies, a NOACCESS page faults inside the guard");
  }
  // L2 (the coordinator 10-25 00:2x): gateReadMapped's fault -> counter -> gateRead fallback, driven directly. A guest
  // page is cached readable, then made NOACCESS WITHOUT the invalidation protocol (G3's raw-NOACCESS case), so the
  // cached check still answers "readable": the plain copy faults inside the guard, the fault is counted (lean on) and
  // the call returns what the OS copy (ReadProcessMemory) returns - false - with no crash. Under SR_GUESTMEM_LEAN=0 the
  // same call is gateRead: the same result, no fault counted. Then the page is restored: a plain read (lean on).
  {
    const uint32_t va = 0x45000000u;
    uint8_t* h = sr::kernel::guestVAToHost(va);
    const bool lean = F::guestMemLeanOnForTest();
    bool pre = false, same = false, okBack = false;
    uint64_t faults = 99, mapped = 99, faults2 = 99, mapped2 = 99, checks = 99;
    if (h) {
      sr::kernel::hostRangeReadableSetCacheForTest(1);
      sr::kernel::hostRangeReadableInvalidate();
      std::memset(h, 0x6D, 64);
      DWORD old = 0;
      if (sr::kernel::hostRangeReadableCached(h, 64) && VirtualProtect(h, 4096, PAGE_NOACCESS, &old)) {
        pre = sr::kernel::hostRangeReadableCached(h, 64);  // stale: still "readable"
        uint8_t viaOs[64] = {}, out[64] = {};
        SIZE_T n = 0;
        const bool os = ReadProcessMemory(GetCurrentProcess(), h, viaOs, 64, &n) && n == 64;
        // Astra I15 fix-check P3: the fallback is OBSERVED, not inferred from the result - gateRead's own gateSpan is
        // one more readability check, so the call makes 2 checks when it falls back (lean on) and 1 under =0.
        const uint64_t q0 = sr::kernel::hostRangeReadableStats().calls;
        const bool got = F::gateReadMappedForTest(va, out, 64u, &faults, &mapped);
        checks = sr::kernel::hostRangeReadableStats().calls - q0;
        same = got == os;
        DWORD tmp = 0;
        VirtualProtect(h, 4096, old, &tmp);
        sr::kernel::hostRangeReadableInvalidate();
        uint8_t back[64] = {};
        okBack = F::gateReadMappedForTest(va, back, 64u, &faults2, &mapped2) && back[0] == 0x6D && back[63] == 0x6D;
        std::printf("  L2 lean=%s stale_readable=%d os=%d got=%d faults=%llu mapped=%llu checks=%llu | restored "
                    "faults=%llu mapped=%llu\n", lean ? "on" : "off", int(pre), int(os), int(got),
                    (unsigned long long)faults, (unsigned long long)mapped, (unsigned long long)checks,
                    (unsigned long long)faults2, (unsigned long long)mapped2);
      }
      sr::kernel::hostRangeReadableSetCacheForTest(-1);
      sr::kernel::hostRangeReadableInvalidate();
    }
    check(pre && same && faults == (lean ? 1u : 0u) && mapped == 0 && checks == (lean ? 2u : 1u) && okBack &&
              faults2 == 0 &&
              mapped2 == (lean ? 1u : 0u),
          "L2 I15's gateReadMapped on a stale-readable NOACCESS page: the OS copy's result, faults=1 (lean on; 0 "
          "off), no crash; restored: a plain read");
  }
  // H1 (the coordinator 10-24 17:2x): the checksum work is opt-in - SR_SEG_ARENA_HASH=1 checksums every record at its
  // capture and verifies it at the reclaim; unset, none at all (records were captured above in both cases)
  if (F::segArenaHashOnForTest())
    check(e.records > 0 && e.checks >= e.records && e.verifies > 0,
          "H1 SR_SEG_ARENA_HASH=1: every record checksummed at capture, verified at the reclaim");
  else
    check(e.records > 0 && e.checks == 0 && e.verifies == 0,
          "H1 SR_SEG_ARENA_HASH unset: no checksum work (checks = verifies = 0, records > 0)");
  // H2 (the coordinator 10-24 19:3x): the 0xDB fill of a released chunk is opt-in too - chunks were released above
  // (freed or kept) in both cases
  if (F::segArenaHashOnForTest())
    check(e.chunkFreed + e.freeChunks > 0 && e.fills > 0, "H2 SR_SEG_ARENA_HASH=1: released chunks filled with 0xDB");
  else
    check(e.chunkFreed + e.freeChunks > 0 && e.fills == 0, "H2 SR_SEG_ARENA_HASH unset: no 0xDB fill (fills = 0)");
  std::printf("[SEG_ARENA_TEST] passed=%d failed=%d\n", g_pass, g_fail);
  return g_fail;
#else
  (void)mode;
  std::printf("[SEG_ARENA_TEST] needs the SR_ORACLE seams (a release build has none)\n");
  return 0;
#endif
}
