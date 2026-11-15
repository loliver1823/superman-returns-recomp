// step 6 G14b: the guest APC queue's spin guard (guest/ppc_import_glue.cpp ApcSpinGuard), through its test seams - the
// REAL enqueue and lock. One mode per run: a PASS line, or a FAIL line (exit 1).
//   difftest --apc-guard-test <mode>   (tools/difftest/apc_guard_run.py runs every mode)
// The old guard broke out after 100000 spins and entered unlocked; holding the lock from the test for 300 ms (far past
// that budget) forces the break-out, so guard-waits FAILS on the old guard.
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string>

void srTestQueueGuestAPC(uint32_t threadHandle, uint32_t routine, uint32_t arg1, uint32_t arg2, uint32_t arg3);
void srTestApcLock(bool take);
bool srTestApcLockHeld();
uint32_t srTestApcQueued(uint32_t threadHandle);

namespace {
constexpr uint32_t kH1 = 0x00F0B101u, kH2 = 0x00F0B102u;
constexpr uint32_t kRoutine = 0x82000020u;   // never delivered here: the tests only queue

void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}
// `f` on another thread: true when it returned within `ms`.
template <class F>
bool returnsWithin(F f, int ms, std::future<void>& out) {
  out = std::async(std::launch::async, f);
  return out.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
}

void run(const std::string& mode) {
  if (mode == "uncontended") {
    // With nobody holding it, the guard takes the lock at once: the enqueue completes and the lock is free after.
    srTestQueueGuestAPC(kH1, kRoutine, 1, 0, 0);
    require(srTestApcQueued(kH1) == 1, "the enqueue completed");
    require(!srTestApcLockHeld(), "the lock was released");
  } else if (mode == "guard-waits") {
    // The guard never enters without the lock: an enqueue while another holder keeps g_apcLock for 300 ms (far past
    // the old 100000-spin budget) returns only after the release, and the holder's lock stays held meanwhile.
    srTestApcLock(true);
    std::future<void> done;
    const bool early = returnsWithin([] { srTestQueueGuestAPC(kH2, kRoutine, 11, 0, 0); }, 300, done);
    const bool stillHeld = srTestApcLockHeld();
    srTestApcLock(false);
    done.get();
    require(!early, "the enqueue waited for the holder's release");
    require(stillHeld, "the holder's lock was never released by the waiter");
    require(srTestApcQueued(kH2) == 1, "the enqueue completed after the release");
    require(!srTestApcLockHeld(), "the lock was released after the enqueue");
  } else {
    throw std::runtime_error("unknown mode");
  }
  std::printf("[APC_GUARD_TEST] PASS %s\n", mode.c_str());
}
}  // namespace

int runApcGuardTest(const char* mode) {
  if (!mode) {
    std::printf("[APC_GUARD_TEST] FAIL no mode (tools/difftest/apc_guard_run.py lists them)\n");
    return 2;
  }
  try {
    run(mode);
  } catch (const std::exception& e) {
    std::printf("[APC_GUARD_TEST] FAIL %s\n", e.what());
    std::fflush(stdout);
    std::_Exit(1);
  }
  std::fflush(stdout);
  return 0;
}
