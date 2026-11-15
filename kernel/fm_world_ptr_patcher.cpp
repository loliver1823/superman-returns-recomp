// iter-43: patch FM module worldPtr (module+4) to fix DRAW_INDX=0.
//
// Root cause traced in iter-42d:
//   sub_82720650 (FM tick) reads *(module+4) as worldPtr, then calls
//   sub_825FC2D0 which reads *(worldPtr+0) as taskPtr (the worker's
//   guest thread handle) and passes it to NtQueueApcThread.
//
//   Problem A (fixed in ppc_recomp.57.cpp): sub_82468EF8 read condvar
//   handle via r13 (Xenon TEB) which is 0 in our build, so workers
//   stored 0 at context+0. The bandaid returns getCurrentThreadHandle().
//
//   Problem B (fixed here): module+4 holds garbage (0x073480BF etc.)
//   because sub_825F73E0 (the setter) has a guard that no-ops when
//   *(module+4)==0, creating a chicken-and-egg situation. The field
//   never gets initialized to a valid context VA.
//
// Fix: capture ExCreateThread contextVA for each startVA=0x827A7420
// worker (via notifyFMWorkerCreated called from the ExCreateThread stub),
// wait for workers to set context+0 (condvar handle), then write context
// VAs to module+4. Poll every 50 ms and re-patch if the field becomes
// invalid (the game's broken init path overwrites it to garbage).
//
// Context-to-module mapping: creation order matches globals array order
// [D, B, C, A] at 0x8296D8D8/DC/E0/E4. Workers are always created in
// the same order as their owning modules are iterated.
//
// iter-188 (2026-06-11): OBSERVE-ONLY. The readGuestU32BE/writeGuestBE32
// thread_local g_guestSize bug made this patcher a silent no-op since
// iter-43 (reads returned 0, writes no-opped). With that fixed, the first
// real patch run showed module+4 already holds REAL game-initialized
// worldPtrs (0x8297AB68/ABF0/ABAC/AC34) — iter-43's "garbage" premise is
// stale — and clobbering them with worker context VAs crashed the run at
// ~19 s. Until proven needed again, this patcher only observes and logs
// module+4 transitions (kObserveOnly).

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace sr {
namespace kernel {

namespace {

// Module globals array: [D, B, C, A] at consecutive 4-byte slots.
constexpr uint32_t kModGlobAddrs[4] = {
  0x8296D8D8u,  // modD
  0x8296D8DCu,  // modB
  0x8296D8E0u,  // modC
  0x8296D8E4u,  // modA
};
static const char* const kModNames[4] = { "modD", "modB", "modC", "modA" };

inline bool isDogmaHeapAddr(uint32_t va) {
  return va >= 0x88000000u && va < 0xBF000000u;
}

struct FMWorkerInfo {
  uint32_t contextVA;
  uint32_t guestHandle;
};

std::mutex g_workerMutex;
std::condition_variable g_workerCV;
std::vector<FMWorkerInfo> g_workers;

void patcherMain(uint8_t* /*base*/) {
  // Step 1: Wait for all 4 FM workers to be captured (up to 10 s).
  std::vector<FMWorkerInfo> workers;
  {
    std::unique_lock<std::mutex> lk(g_workerMutex);
    g_workerCV.wait_for(lk, std::chrono::seconds(10),
                        [] { return (int)g_workers.size() >= 4; });
    workers = g_workers;
  }

  if (workers.empty()) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[FM_WORLD_PATCHER] no FM workers captured after 10 s, exiting");
    return;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FM_WORLD_PATCHER] captured %zu workers, waiting for context+0...",
    workers.size());

  // Step 2: Wait for each worker to set context+0 (the condvar handle).
  // The sub_82468EF8 bandaid stores getCurrentThreadHandle() there.
  for (int i = 0; i < (int)workers.size(); ++i) {
    uint32_t ctxVA = workers[i].contextVA;
    for (int t = 0; t < 500; ++t) {
      uint32_t condvar = readGuestU32BE(ctxVA);
      if (condvar != 0) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[FM_WORLD_PATCHER] worker[%d] ctx=0x%08X condvar=%u ready after %d ms",
          i, ctxVA, condvar, t * 10);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (t == 499) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[FM_WORLD_PATCHER] worker[%d] ctx=0x%08X condvar still 0 after 5 s",
          i, ctxVA);
      }
    }
  }

  // Step 3: Wait for module globals to be non-zero (up to 5 s).
  for (int t = 0; t < 500; ++t) {
    bool allSet = true;
    for (int i = 0; i < 4; ++i) {
      if (readGuestU32BE(kModGlobAddrs[i]) == 0) { allSet = false; break; }
    }
    if (allSet) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  static std::atomic<uint32_t> s_patchCount{0};

  // iter-188: observe-only — log module+4 transitions, never write.
  constexpr bool kObserveOnly = true;
  uint32_t lastSeen[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};

  auto patchAll = [&]() {
    for (int i = 0; i < (int)workers.size() && i < 4; ++i) {
      uint32_t modVA = readGuestU32BE(kModGlobAddrs[i]);
      if (modVA == 0) continue;

      uint32_t fieldAddr = modVA + 4;
      uint32_t current   = readGuestU32BE(fieldAddr);
      uint32_t target    = workers[i].contextVA;

      if (kObserveOnly) {
        if (current != lastSeen[i]) {
          uint32_t n = s_patchCount.fetch_add(1, std::memory_order_relaxed) + 1;
          if (n <= 40 || (n & (n - 1)) == 0) {
            SR_LOG_BOOT(sr::LogSeverity::Info,
              "[FM_WORLD_PATCHER] observe#%u %s(0x%08X)+4: 0x%08X (was 0x%08X) workerCtx=0x%08X",
              n, kModNames[i], modVA, current, lastSeen[i], target);
          }
          lastSeen[i] = current;
        }
        continue;
      }

      if (current == target) continue;

      writeGuestBE32(fieldAddr, target);
      uint32_t condvar = readGuestU32BE(target);  // context+0
      uint32_t n = s_patchCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 20 || (n & (n - 1)) == 0) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[FM_WORLD_PATCHER] patch#%u %s(0x%08X)+4: 0x%08X->0x%08X condvar=%u",
          n, kModNames[i], modVA, current, target, condvar);
      }
    }
  };

  patchAll();

  // Poll every 50 ms to re-patch if the game overwrites module+4.
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    patchAll();
  }
}

}  // namespace

void notifyFMWorkerCreated(uint32_t contextVA, uint32_t guestHandle) {
  {
    std::lock_guard<std::mutex> lk(g_workerMutex);
    if ((int)g_workers.size() >= 4) return;
    g_workers.push_back({contextVA, guestHandle});
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FM_WORLD_PATCHER] captured worker[%zu] contextVA=0x%08X handle=%u",
      g_workers.size() - 1, contextVA, guestHandle);
  }
  g_workerCV.notify_all();
}

void startFMWorldPtrPatcher() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;

  uint8_t* base = nullptr;
  size_t size = 0;
  getGlobalGuestMemory(&base, &size);
  if (!base) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[FM_WORLD_PATCHER] cannot start: guest memory not registered");
    return;
  }

  std::thread(patcherMain, base).detach();
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FM_WORLD_PATCHER] patcher thread spawned");
}

}  // namespace kernel
}  // namespace sr
