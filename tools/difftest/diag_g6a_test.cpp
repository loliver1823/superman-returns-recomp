// step 6 G6a (SR_ORACLE): the DIAG's own arithmetic and census, through its test seams - the REAL code paths.
// One mode per run: a PASS line, or a FAIL line (exit 1).
//   difftest --diag-g6a-test <mode>   (tools/difftest/diag_g6a_run.py runs every mode)
// - sig-record: [DIAG_WAIT]'s signaller record (obj / class / tid in one 64-bit word) round-trips.
// - wake-ticks: [DIAG_WAKE]'s set -> wake ticks: a stamp inside the wait, before it, another object's, across a wrap of
//   the QPC's low 32 bits, and a wait too long to tell.
// - wake-bucket: [DIAG_WAKE]'s log2 buckets.
// - apc-census: [APC_STUCK]: a queue that holds an APC across two censuses with no delivery is stuck; a delivery
//   clears it.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/sr_oracle.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if SR_ORACLE && defined(_WIN32)
namespace sr::kernel {
uint64_t srTestDiagSigRecord(uint32_t obj, uint32_t cls, uint32_t tid);
int64_t srTestDiagWakeTicks(uint32_t stampObj, uint64_t stampQpc, uint32_t obj, uint64_t t0, uint64_t t1);
int srTestDiagWakeBucket(uint32_t us);
}  // namespace sr::kernel
void srTestQueueGuestAPC(uint32_t threadHandle, uint32_t routine, uint32_t arg1, uint32_t arg2, uint32_t arg3);
void srTestDeliverGuestAPCs(PPCContext& ctx, uint32_t threadHandle);
uint32_t srTestApcStuckCensus(uint64_t swaps);

namespace {
constexpr uint32_t kRecordVa = (uint32_t)PPC_CODE_BASE + 0x20;   // records its arg1
constexpr uint32_t kH1 = 0x00F0C101u;
std::vector<uint32_t> g_ran;
void apcRecord(PPCContext& ctx, uint8_t*) { g_ran.push_back(ctx.r3.u32); }

void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}

void run(const std::string& mode) {
  using namespace sr::kernel;
  if (mode == "sig-record") {
    const uint64_t r = srTestDiagSigRecord(0x829AC0B8u, 4u, 31900u);
    require(uint32_t(r >> 32) == 0x829AC0B8u, "the record keeps the object");
    require((uint32_t(r >> 29) & 7u) == 4u, "the record keeps the class");
    require((uint32_t(r) & 0x1FFFFFFFu) == 31900u, "the record keeps the tid");
    const uint64_t g = srTestDiagSigRecord(0x400054CCu, 0u, 0x3FFFFFFFu);   // a tid past 29 bits is cut, not spilled
    require(uint32_t(g >> 32) == 0x400054CCu && (uint32_t(g >> 29) & 7u) == 0u, "a wide tid leaves the class alone");
    require((uint32_t(g) & 0x1FFFFFFFu) == 0x1FFFFFFFu, "a wide tid keeps its low 29 bits");
  } else if (mode == "wake-ticks") {
    const uint64_t t0 = 5000000000ull, t1 = t0 + 1000u;
    require(srTestDiagWakeTicks(0x10u, t0 + 400u, 0x10u, t0, t1) == 600, "a stamp inside the wait: t1 - set");
    require(srTestDiagWakeTicks(0x10u, t0 - 1u, 0x10u, t0, t1) == -1, "a stamp before the wait is not its wake");
    require(srTestDiagWakeTicks(0x11u, t0 + 400u, 0x10u, t0, t1) == -1, "another object's stamp is not its wake");
    require(srTestDiagWakeTicks(0x10u, t1, 0x10u, t0, t1) == 0, "a stamp at t1 is a wake of 0");
    const uint64_t w0 = (7ull << 32) + 0xFFFFFF00u;   // the low 32 bits wrap inside this wait
    require(srTestDiagWakeTicks(0x10u, w0 + 0x200u, 0x10u, w0, w0 + 0x400u) == 0x200, "a wake across the wrap");
    require(srTestDiagWakeTicks(0x10u, t0 + 1u, 0x10u, t0, t0 + (1ull << 31)) == -1, "a wait too long to tell");
  } else if (mode == "wake-bucket") {
    require(srTestDiagWakeBucket(0) == 0, "0 us is bucket 0");
    require(srTestDiagWakeBucket(1) == 1 && srTestDiagWakeBucket(2) == 1, "1-2 us is bucket 1");
    require(srTestDiagWakeBucket(3) == 2, "3 us is bucket 2");
    require(srTestDiagWakeBucket(1000) == 9, "1000 us is bucket 9");
    require(srTestDiagWakeBucket(0xFFFFFFFFu) == 19, "the last bucket is open-ended");
  } else if (mode == "apc-census") {
    PPCFunc** table = sr::kernel::g_dispatchTable;
    std::vector<PPCFunc*> own;
    if (!table) {
      own.assign(sr::kernel::kDispatchTableEntryCount, nullptr);
      sr::kernel::g_dispatchTable = table = own.data();
    }
    table[(kRecordVa - PPC_CODE_BASE) >> 2] = apcRecord;
    PPCContext ctx;
    std::memset(&ctx, 0, sizeof ctx);
    srTestQueueGuestAPC(kH1, kRecordVa, 3, 0, 0);
    require(srTestApcStuckCensus(120) == 0, "a queue is not stuck at the first census that sees it");
    require(srTestApcStuckCensus(240) == 1, "held across two censuses with no delivery: stuck");
    srTestDeliverGuestAPCs(ctx, kH1);
    srTestQueueGuestAPC(kH1, kRecordVa, 4, 0, 0);
    require(g_ran == std::vector<uint32_t>({3}), "the APC was delivered");
    require(srTestApcStuckCensus(360) == 0, "a delivery between two censuses is no stuck queue, though a new APC waits");
    require(srTestApcStuckCensus(480) == 1, "the new APC, undelivered across two censuses: stuck");
    srTestDeliverGuestAPCs(ctx, kH1);
    require(g_ran == std::vector<uint32_t>({3, 4}), "the second APC was delivered");
    require(srTestApcStuckCensus(600) == 0, "an empty queue is not stuck");
    table[(kRecordVa - PPC_CODE_BASE) >> 2] = nullptr;
  } else {
    throw std::runtime_error("unknown mode");
  }
  std::printf("[DIAG_G6A_TEST] PASS %s\n", mode.c_str());
}
}  // namespace

int runDiagG6aTest(const char* mode) {
  if (!mode) {
    std::printf("[DIAG_G6A_TEST] FAIL no mode (tools/difftest/diag_g6a_run.py lists them)\n");
    return 2;
  }
  try {
    run(mode);
  } catch (const std::exception& e) {
    std::printf("[DIAG_G6A_TEST] FAIL %s\n", e.what());
    std::fflush(stdout);
    std::_Exit(1);
  }
  std::fflush(stdout);
  return 0;
}
#else
int runDiagG6aTest(const char*) {
  std::printf("[DIAG_G6A_TEST] not compiled (SR_ORACLE=0: the DIAG is absent from this build)\n");
  return 3;
}
#endif
