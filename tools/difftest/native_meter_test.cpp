// iter-467 native-a: the NATIVE METER's columns (kernel/native_meter.{hpp,cpp}) - every Id has a
// column with a name, the display-register column `dispreg` (RELEASE_PLAN C1's fourth class) is
// wired and counts what its feeders push, and the column list the tools parse (tools/perf/
// native_meter.py COLS) is the one the kernel prints.
//
// Run: build/difftest.exe --native-meter-test   (exit code = failed checks)
#include "kernel/native_meter.hpp"

#include <cstdio>
#include <cstring>

namespace M = sr::native::meter;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
}  // namespace

int runNativeMeterTest() {
  std::printf("native meter: columns and the dispreg feed\n");
  // The printed column list: the pushed ids in Id order, then xma. tools/perf/native_meter.py COLS
  // must list exactly these names in this order (test_native_meter.py checks the Python side).
  const char* const want[] = {"pm4", "edram_res", "edram_xfer", "edram_load", "nrec", "dispreg", "shm", "xma"};
  const int n = int(sizeof(want) / sizeof(want[0]));
  check(M::columnCount() == n, "columnCount == 8 (the seven pushed ids + xma)");
  check(int(M::kIds) + 1 == M::columnCount(), "every Id has a column, plus the pulled xma");
  bool names = true;
  for (int c = 0; c < n && c < M::columnCount(); ++c) names = names && std::strcmp(M::columnName(c), want[c]) == 0;
  check(names, "column names in order: pm4 edram_res edram_xfer edram_load nrec dispreg shm xma");
  check(std::strcmp(M::columnName(int(M::kDisplayReg)), "dispreg") == 0, "kDisplayReg prints as dispreg");
  // mem.resources step 1 (release/mem-meter): the shared-memory page model's validations, wired from the start (its
  // feeder gfx/d3d12_pm4_shmem.cpp is unconditional code in every build that has the meter).
  check(std::strcmp(M::columnName(int(M::kShm)), "shm") == 0, "kShm prints as shm, between dispreg and xma");
  check(M::columnWired(int(M::kShm)), "shm is wired (prints a number, never '-')");
  check(std::strcmp(M::columnName(-1), "") == 0 && std::strcmp(M::columnName(99), "") == 0,
        "out-of-range columns print as empty, never alias another column");
  // dispreg is wired from the start (its feeders in kernel/stubs.cpp are unconditional code), so a
  // roll's line never shows "-" for it - a 0 there is a measured 0.
  check(M::columnWired(int(M::kDisplayReg)), "dispreg is wired (prints a number, never '-')");
  check(M::columnWired(int(M::kPm4)) && M::columnWired(int(M::kNativeRec)), "pm4 and nrec wired as before");
  // The feed: what the display stubs push arrives in the column, by the exact count.
  uint64_t a[8] = {}, b[8] = {};
  M::snapshotColumns(a, 8);
  M::add(M::kDisplayReg, 2);   // one vblank tick: V_COUNTER + VIEWPORT_SIZE
  M::add(M::kDisplayReg);      // one VdGetSystemCommandBuffer call
  M::snapshotColumns(b, 8);
  check(b[M::kDisplayReg] - a[M::kDisplayReg] == 3, "add(kDisplayReg, 2) + add(kDisplayReg) counts 3");
  bool others = true;
  for (int c = 0; c < M::columnCount(); ++c)
    if (c != int(M::kDisplayReg)) others = others && (a[c] == b[c]);
  check(others, "no other column moved");
  // The shm feed: a hash-decided validation of a 3-page range and one write-watch-decided page arrive by the exact count.
  M::snapshotColumns(a, 8);
  M::add(M::kShm, 3);   // pm4ShmValidate: three candidate pages hashed
  M::add(M::kShm);      // pm4ShmRequest: one page accepted from the write watch
  M::snapshotColumns(b, 8);
  check(b[M::kShm] - a[M::kShm] == 4, "add(kShm, 3) + add(kShm) counts 4");
  bool othersShm = true;
  for (int c = 0; c < M::columnCount(); ++c)
    if (c != int(M::kShm)) othersShm = othersShm && (a[c] == b[c]);
  check(othersShm, "no other column moved on the shm feed");
  // snapshotColumns never writes past n.
  uint64_t guard[3] = {0xDEADBEEFull, 0xDEADBEEFull, 0xDEADBEEFull};
  M::snapshotColumns(guard, 2);
  check(guard[2] == 0xDEADBEEFull, "snapshotColumns honours n");
  std::printf("native meter test: %d ok, %d failed\n", g_pass, g_fail);
  return g_fail;
}
