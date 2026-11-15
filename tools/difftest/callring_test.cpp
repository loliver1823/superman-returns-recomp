// fix/postgame-apt (lane K, 2026-10-17): --callring-test - the import call ring's slot never leaves the ring.
//
// guest/ppc_import_glue.cpp records every kernel import in a per-thread 8-entry ring. Its counter was an
// `int` and the slot `idx % 8`: after 2^31 imports of one thread (~14 minutes of a game-kernel runner) the
// slot went -7..-1 and the writes landed up to 112 bytes in front of the ring - the R3 post-game TLS
// corruption (dev exe) and heap-header corruption 0xC0000374 (release exe). The fix makes the counter
// unsigned and the slot a mask.
//
//   C1  callKernel's writer (recordCallRingTo) on test-owned guarded storage from 2^31 - 32 through the 2^31
//       crossing: 0 guard bytes changed, the counter ends at seed + calls, and the ring holds the last 8 records
//       newest-first at the slots the counter names (a writer that stores nothing or elsewhere fails)
//   C2  the same through the 2^32 wrap (0xFFFFFFE0 -> 0x40)
//   C3  the slot function over [2^31 - 64, 2^31 + 64), [2^32 - 64, 2^32) and [0, 64): always < 8 and == n & 7
//   C4  the WIRING: 40 real callKernel calls (an import no export has) from a counter seeded at 2^31 - 16 leave
//       their records, newest-first, in the real t_callRing (read in bounds only)
//   C5  the same for the MAIN guest thread's static g_mainCallRing (its own counter; the pre-fix overflow wrote
//       the image .data in front of it - the APC queue map in dev f1379269fc82): this thread marked main
//   M1  mutation witness: the pre-fix arithmetic (SR_DIAG_CALLRING_LEGACY's path) through the same crossing on the
//       same guarded storage MUST change guard bytes in front of the ring
//   M2  the pre-fix slot function at 2^31 + 1 is negative
// (Astra review 1, P2: the first version read live TLS in front of the real ring and could pass with no store.)
#include <cstdint>
#include <cstdio>

uint32_t srTestCallRingOwned(uint32_t seed, uint32_t calls, bool legacy, uint32_t* endIdx, bool* entriesOk);
bool srTestCallKernelRing(uint32_t seed, uint32_t calls, uint32_t* endIdx);   // guest/ppc_import_glue.cpp
bool srTestCallKernelMainRing(uint32_t seed, uint32_t calls, uint32_t* endIdx);
uint32_t srTestCallRingSlot(uint32_t n);
int srTestCallRingSlotLegacy(uint32_t n);

int runCallRingTest() {
  int bad = 0;
  auto check = [&](const char* id, bool ok, const char* what) {
    std::printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", id, what);
    if (!ok) ++bad;
  };
  char buf[256];

  uint32_t end = 0;
  bool entries = false;
  uint32_t changed = srTestCallRingOwned(0x7FFFFFE0u, 64, false, &end, &entries);
  std::snprintf(buf, sizeof(buf), "writer across 2^31: guard bytes changed=%u, end=0x%08X, entries %s (want 0, 0x80000020, ok)",
                changed, end, entries ? "ok" : "WRONG");
  check("C1", changed == 0 && end == 0x80000020u && entries, buf);

  changed = srTestCallRingOwned(0xFFFFFFE0u, 96, false, &end, &entries);
  std::snprintf(buf, sizeof(buf), "writer across 2^32: changed=%u, end=0x%08X, entries %s (want 0, 0x00000040, ok)",
                changed, end, entries ? "ok" : "WRONG");
  check("C2", changed == 0 && end == 0x40u && entries, buf);

  bool slotsOk = true;
  uint32_t firstBad = 0;
  const uint32_t starts[3] = {0x80000000u - 64u, 0xFFFFFFC0u, 0u};
  for (uint32_t s : starts)
    for (uint32_t k = 0; k < 128; ++k) {
      if (s == 0xFFFFFFC0u && k >= 64) break;
      const uint32_t n = s + k;
      const uint32_t slot = srTestCallRingSlot(n);
      if (slot >= 8u || slot != (n & 7u)) {
        if (slotsOk) firstBad = n;
        slotsOk = false;
      }
    }
  check("C3", slotsOk, "slot(n) in [0,8) and == n&7 around 2^31, 2^32 and 0");
  if (!slotsOk) std::printf("        first bad n=0x%08X\n", firstBad);

  const bool wired = srTestCallKernelRing(0x7FFFFFF0u, 40, &end);
  std::snprintf(buf, sizeof(buf), "callKernel records into t_callRing across 2^31: %s, end=0x%08X (want ok, 0x80000018)",
                wired ? "ok" : "WRONG", end);
  check("C4", wired && end == 0x80000018u, buf);

  const bool mainWired = srTestCallKernelMainRing(0x7FFFFFF0u, 40, &end);
  std::snprintf(buf, sizeof(buf), "callKernel records into g_mainCallRing across 2^31: %s, end=0x%08X (want ok, 0x80000018)",
                mainWired ? "ok" : "WRONG", end);
  check("C5", mainWired && end == 0x80000018u, buf);

  changed = srTestCallRingOwned(0x7FFFFFE0u, 64, true, &end, &entries);
  std::snprintf(buf, sizeof(buf), "mutation witness: the pre-fix slot across 2^31 changed %u guard bytes (want > 0)", changed);
  check("M1", changed > 0, buf);

  const int legacySlot = srTestCallRingSlotLegacy(0x80000001u);
  std::snprintf(buf, sizeof(buf), "mutation witness: pre-fix slot(2^31 + 1) = %d (want < 0)", legacySlot);
  check("M2", legacySlot < 0, buf);

  std::printf("callring-test: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
  return bad ? 1 : 0;
}
