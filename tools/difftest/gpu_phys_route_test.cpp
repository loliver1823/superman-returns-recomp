// iter-458 HR1 (Astra 071/072): the SR_GPU_PHYS_ROUTE contract of the
// sub_82467F40 override (kernel/gpu_ring_alloc_fix.cpp), driven through the
// REAL override, the guest's OWN XMemAllocDispatch_sub_82467F40 /
// XMemAllocPhysical_sub_82468008 code, and the REAL MmAllocatePhysicalMemoryEx /
// MmFreePhysicalMemory exports (registerStubs).
//
//   A  gate off: the ring claim and today's routing (heap > 256 KB after the
//      claim, direct guestPhysAlloc <= 256 KB) - unchanged.
//   B  gate on, before the claim: the claim keeps its path (the boundary).
//   C  gate on, after the claim: every bit-31 size class runs the guest's own
//      path - a physical-window grant, registered exactly ONCE (the import
//      registers; the override registers nothing), never heap-routed.
//   D  the attrs alignment nibble reaches the import (and its page floor).
//   G  Astra 072/073: the capture snapshot at the REAL
//      recorder-close edge carries the unsampled tail exactly, is taken once,
//      counts a call outstanding at the close, and classifies a request that
//      spans the claim's publication by ONE admission observation.
//   E  zero-fill is the guest's (attrs bit 30), on reused (LIFO) memory.
//   F  exhaustion: the guest's own failure path (SetLastError(8), null), no
//      heap fallback, nothing registered; delegated grants below 0xA0000000
//      and the import's truncated / rounding-wrapped sizes are counted.
//
// The image is not loaded in difftest: the guest's protect table (0x82917E60)
// and flag word (0x82976A94) are seeded with the image's values (IDA).
//
// Run: build/difftest.exe --gpu-phys-route-test   (exit code = failed checks)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "gfx/hle_video.hpp"
#include "kernel/export_registry.hpp"
#include "kernel/gpu_phys_route.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/phys_window.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(sub_82467F40);   // the XMemAlloc override (gpu_ring_alloc_fix.cpp)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace sr { namespace kernel {
extern std::atomic<uint32_t> g_gpuRingAllocVA;
} }

namespace K = sr::kernel;
namespace W = sr::kernel::physwin;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
  std::fflush(stdout);
}

// A scratch PCR / thread block for the guest's SetLastError (r13), and a stack.
constexpr uint32_t kPcr = 0x7F000000u, kThr = 0x7F001000u, kStackTop = 0x7F0F0000u;
constexpr uint32_t kWinA = 0xA0000000u;   // the physical window's floor

uint32_t callOn(uint32_t size, uint32_t flags, uint32_t stackTop) {
  PPCContext c{};
  c.r1.u64 = stackTop;
  c.r3.u64 = size;
  c.r4.u64 = flags;
  c.r13.u64 = kPcr;
  c.lr = 0;
  sub_82467F40(c, K::getGuestBase());
  return c.r3.u32;
}
uint32_t call(uint32_t size, uint32_t flags) { return callOn(size, flags, kStackTop); }
std::string tmpPath(const char* name) {
  const char* t = std::getenv("TEMP");
  return std::string(t ? t : ".") + "\\" + name;
}
K::GpuPhysRouteSnapshot now() { return K::gpuPhysRouteNowForTest(); }
bool filled(uint32_t va, uint32_t n, uint8_t v) {
  const uint8_t* p = K::guestVAToHost(va);
  if (!p) return false;
  for (uint32_t i = 0; i < n; i++) if (p[i] != v) return false;
  return true;
}
}  // namespace

int runGpuPhysRouteTest() {
  std::printf("[GPU_PHYS_ROUTE_TEST] SR_GPU_PHYS_ROUTE (HR1) routing contract\n");
  K::registerStubs();
  auto& reg = K::ExportRegistry::instance();
  auto mmFree = reg.getExport("MmFreePhysicalMemory");
  check(mmFree != nullptr, "S0 the MmFreePhysicalMemory export is registered");
  // The image's protect table and flag word (IDA 0x82917E60, 0x82976A94).
  const uint32_t prot[4] = {0x20000404u, 0x00000204u, 0x00000004u, 0x00000404u};
  for (int i = 0; i < 4; i++) K::writeGuestBE32(0x82917E60u + 4u * i, prot[i]);
  K::writeGuestBE32(0x82976A94u, 0u);
  K::writeGuestBE32(kPcr + 336u, 0u);
  K::writeGuestBE32(kPcr + 256u, kThr);
  K::writeGuestBE32(kThr + 352u, 0u);
  W::resetForTest();
  K::setPhysLifoForTest(0);
  K::gpuPhysRouteResetForTest();
  char name[256];

  // ── G0: the ENV gate arms the recorder-close snapshot by itself (no census,
  //     no test arming): the production wiring of a gate-only run.
  {
    _putenv_s("SR_GPU_PHYS_ROUTE", "1");
    K::setGpuPhysRouteForTest(-1);
    K::g_gpuRingAllocVA.store(0xA0100000u);   // a claim, so the call delegates
    const uint32_t va = call(0x1000u, 0x8CC00000u);
    sr::gfx::hle::noteRecorderClosedForTest();
    const K::GpuPhysRouteSnapshot s = K::gpuPhysRouteSnapshotForTest();
    check(va >= kWinA && s.taken && s.gate == 1 && s.completed == 1,
          "G0 the env gate alone arms the close hook: the recorder's close takes the snapshot");
    K::gpuPhysRouteResetForTest();
  }

  // ── A: gate off - the boundary and today's routing, unchanged ────────────
  K::setGpuPhysRouteForTest(0);
  K::g_gpuRingAllocVA.store(0);
  {
    const K::GpuPhysRouteSnapshot s0 = now();
    const uint32_t ring = call(0xC0000u, 0x8FC00000u);
    check(ring != 0 && ring < kWinA && K::g_gpuRingAllocVA.load() == ring,
          "A1 gate off: the ring claim is a heap grant and publishes the ring VA");
    const uint32_t big = call(0x40001u, 0x8FC00000u);
    check(big != 0 && big < kWinA && W::ownerOfGrant(big, 0x40001u) == W::kOwnHeapGrant,
          "A2 gate off: > 256 KB after the claim is heap-routed (today's route)");
    const uint32_t mid = call(0x40000u, 0x8FC00000u);
    check(mid >= kWinA && W::ownerOfGrant(mid, 0x40000u) == W::kOwnPhysRegion,
          "A3 gate off: <= 256 KB after the claim -> guestPhysAlloc (today's route)");
    const K::GpuPhysRouteSnapshot s1 = now();
    check(s1.started == s0.started && s1.completed == s0.completed &&
              s1.heapAfterClaim == s0.heapAfterClaim + 1 && s1.heapSpanning == s0.heapSpanning,
          "A4 gate off: nothing delegated; the witness counts the one heap-routed grant admitted "
          "after the claim");
  }

  // ── B: gate on, before the claim - the claim keeps its path ──────────────
  K::setGpuPhysRouteForTest(1);
  K::g_gpuRingAllocVA.store(0);
  {
    const K::GpuPhysRouteSnapshot s0 = now();
    const uint32_t ring = call(0xC0000u, 0x8FC00000u);
    const K::GpuPhysRouteSnapshot s1 = now();
    check(ring != 0 && ring < kWinA && K::g_gpuRingAllocVA.load() == ring &&
              s1.started == s0.started && s1.heapAfterClaim == s0.heapAfterClaim &&
              s1.heapSpanning == s0.heapSpanning,
          "B1 gate on, before the claim: the claim is intercepted as before (heap), publishes "
          "the ring VA, and is counted as neither after nor spanning the claim");
  }

  // ── C: gate on, after the claim - every size class runs the guest's path ──
  {
    const uint32_t sizes[] = {1u, 0xFFFu, 0x1000u, 0x40000u, 0x40001u, 0x398000u,
                              0x4000000u, 0x4000001u};
    for (uint32_t sz : sizes) {
      const K::GpuPhysRouteSnapshot s0 = now();
      const uint64_t g0 = W::registryGeneration();
      const uint32_t va = call(sz, 0x8CC00000u);
      const K::GpuPhysRouteSnapshot s1 = now();
      const uint64_t g1 = W::registryGeneration();
      std::snprintf(name, sizeof name,
                    "C size 0x%X: the guest's own path -> a page-aligned physical-window grant "
                    "(0x%08X), registered exactly once, not heap-routed", sz, va);
      check(va >= kWinA && (va & 0xFFFu) == 0 && g1 - g0 == 1 &&
                W::ownerOfGrant(va, sz) == W::kOwnPhysRegion &&
                s1.started == s0.started + 1 && s1.completed == s0.completed + 1 &&
                s1.null == s0.null && s1.heapAfterClaim == s0.heapAfterClaim,
            name);
    }
    const K::GpuPhysRouteSnapshot s0 = now();
    const uint64_t g0 = W::registryGeneration();
    const uint32_t z = call(0u, 0x8CC00000u);
    const K::GpuPhysRouteSnapshot s1 = now();
    check(z >= kWinA && W::registryGeneration() - g0 == 1 && s1.completed == s0.completed + 1,
          "C0 size 0: delegated too; the import's existing rule grants one page");
  }

  // ── D: the attrs alignment nibble reaches the import ─────────────────────
  {
    const uint32_t a32 = call(0x1000u, 0x8FC00000u);   // nibble 0xF: 32 KB
    check(a32 >= kWinA && (a32 & 0x7FFFu) == 0, "D1 the 32 KB class is honoured");
    const uint32_t a16 = call(0x10u, 0x84C00000u);     // nibble 0x4: 16 B -> page floor
    check(a16 >= kWinA && (a16 & 0xFFFu) == 0, "D2 a 16-byte class is floored to the page");
    const uint32_t d = call(0x1000u, 0x80C00000u);     // nibble 0: the guest's default (4 KB)
    check(d >= kWinA && (d & 0xFFFu) == 0, "D3 no class: the guest's default page class");
  }

  // ── G: the capture-bound evidence (Astra 072) ────────────────────────────
  W::armRecorderCloseHook();   // the production close edge -> uniqOnRecorderClose
  {
    // G1: nine delegations - the sampled log lines stop at #8, the snapshot
    // at the REAL close edge carries all nine.
    K::gpuPhysRouteResetForTest();
    for (int i = 0; i < 9; i++) call(0x1000u, 0x8CC00000u);
    sr::gfx::hle::noteRecorderClosedForTest();
    const K::GpuPhysRouteSnapshot s = K::gpuPhysRouteSnapshotForTest();
    check(s.taken && s.gate == 1 && s.admitted == 9 && s.settled == 9 && s.started == 9 &&
              s.completed == 9 && s.null == 0 && s.heapAfterClaim == 0 && s.atSeal == 0,
          "G1 the recorder's close snapshots the witness: all 9 delegations (the sampled log "
          "lines stop at #8), none outstanding; no seal in this configuration (atSeal 0)");
    call(0x1000u, 0x8CC00000u);
    sr::gfx::hle::noteRecorderClosedForTest();
    const K::GpuPhysRouteSnapshot s2 = K::gpuPhysRouteSnapshotForTest();
    check(s2.completed == 9 && now().completed == 10,
          "G1b the FIRST close is the capture boundary: a later close changes nothing");
  }
  {
    // G2: the capture closes while a delegated call is inside the guest.
    K::gpuPhysRouteResetForTest();
    K::setGpuPhysRoutePhaseHookForTest([](int phase) {
      if (phase == 2) sr::gfx::hle::noteRecorderClosedForTest();
    });
    const uint32_t va = call(0x1000u, 0x8CC00000u);
    K::setGpuPhysRoutePhaseHookForTest(nullptr);
    const K::GpuPhysRouteSnapshot s = K::gpuPhysRouteSnapshotForTest();
    check(va >= kWinA && s.taken && s.admitted == 1 && s.settled == 0 && s.started == 1 &&
              s.completed == 0 && now().completed == 1 && now().settled == 1,
          "G2 a call still inside the guest at the close is OUTSTANDING in the snapshot "
          "(admitted 1, settled 0) - never silently absent");
  }
  {
    // G2b (Astra 073): the close lands right after ADMISSION (phase 1), before
    // the delegation point. The admitted request is outstanding, not absent.
    K::gpuPhysRouteResetForTest();
    K::setGpuPhysRoutePhaseHookForTest([](int phase) {
      if (phase == 1) sr::gfx::hle::noteRecorderClosedForTest();
    });
    call(0x1000u, 0x8CC00000u);
    K::setGpuPhysRoutePhaseHookForTest(nullptr);
    const K::GpuPhysRouteSnapshot s = K::gpuPhysRouteSnapshotForTest();
    check(s.taken && s.admitted == 1 && s.settled == 0 && s.started == 1 && s.completed == 0,
          "G2b a close right after admission (phase 1) snapshots the request as OUTSTANDING "
          "(admitted and started 1, settled 0)");
  }
  {
    // G3: another thread publishes the claim between this request's admission
    // and its routing. One admission observation classifies it: SPANNING.
    K::gpuPhysRouteResetForTest();
    const uint32_t realClaim = K::g_gpuRingAllocVA.load();
    K::g_gpuRingAllocVA.store(0);
    K::setGpuPhysRoutePhaseHookForTest([](int phase) {
      if (phase == 1) {
        uint32_t expected = 0;
        K::g_gpuRingAllocVA.compare_exchange_strong(expected, 0xA1234000u);
      }
    });
    const uint32_t va = call(0x40001u, 0x8FC00000u);
    K::setGpuPhysRoutePhaseHookForTest(nullptr);
    const K::GpuPhysRouteSnapshot s = now();
    check(va != 0 && va < kWinA && s.heapSpanning == 1 && s.heapAfterClaim == 0 &&
              s.started == 0 && s.admitted == 1 && s.settled == 1,
          "G3 a request admitted before the claim and routed after it is SPANNING (heap, not "
          "delegated), never counted as heap-routed after the claim");
    K::g_gpuRingAllocVA.store(realClaim);
  }
  {
    // G4 (Astra 073): the seal and the snapshot are ONE observation. This thread
    // is delegated; at the delegation point (phase 2) a closer thread C runs
    // the recorder's close (no guest code) and, inside the endpoint - sealed,
    // both locks held - lets this thread continue and waits 60 ms. This
    // delegation's grant registration and settlement must wait for the
    // endpoint, so the snapshot shows it OUTSTANDING (never a clean snapshot
    // beside a seal taken while it was in flight).
    const std::string rec = tmpPath("sr_hr1_g4_uniq.bin");
    sr::gfx::hle::resetTraceCloseCountForTest();
    W::setUniqCensusForTest(1);
    W::setUniqStickyNonceForTest(0x6A11C0DEu);
    W::setUniqStickyFileForTest(rec.c_str());
    call(0x1000u, 0x8CC00000u);                   // one sealed registration first
    K::gpuPhysRouteResetForTest();
    static std::atomic<int> s_g4{0};              // 1 = C is inside the endpoint
    static std::thread* s_closer = nullptr;
    s_g4 = 0;
    W::setUniqCloseMidHookForTest([] {
      s_g4 = 1;
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    });
    K::setGpuPhysRoutePhaseHookForTest([](int phase) {
      if (phase != 2) return;
      s_closer = new std::thread([] { sr::gfx::hle::noteRecorderClosedForTest(); });
      while (s_g4.load() != 1) std::this_thread::yield();   // C sealed; locks held
    });
    call(0x1000u, 0x8CC00000u);
    K::setGpuPhysRoutePhaseHookForTest(nullptr);
    if (s_closer) {
      s_closer->join();
      delete s_closer;
      s_closer = nullptr;
    }
    W::setUniqCloseMidHookForTest(nullptr);
    const K::GpuPhysRouteSnapshot s = K::gpuPhysRouteSnapshotForTest();
    check(s.taken && s.atSeal == 1 && s.admitted == 1 && s.settled == 0 && s.started == 1 &&
              s.completed == 0 && now().settled == 1 && now().completed == 1,
          "G4 a delegation released INSIDE the endpoint (after the seal, before the snapshot) "
          "cannot register or settle until it ends: the sealed snapshot shows it OUTSTANDING");
    // G5: the snapshot names the seal it was taken with (the seal file itself).
    uint32_t sealNonce = 0;
    uint64_t sealSeq = 0;
    if (std::FILE* f = std::fopen((rec + ".seal").c_str(), "rb")) {
      uint8_t hdr[24] = {};
      if (std::fread(hdr, 1, sizeof hdr, f) == sizeof hdr) {
        std::memcpy(&sealNonce, hdr + 8, 4);
        std::memcpy(&sealSeq, hdr + 16, 8);
      }
      std::fclose(f);
    }
    check(s.nonce == 0x6A11C0DEu && sealNonce == s.nonce && sealSeq == s.sealSeq && sealSeq > 0,
          "G5 the snapshot names the seal it was taken with (nonce and seq equal the seal "
          "file's)");
    W::setUniqStickyFileForTest(nullptr);
  }
  {
    // G6: a clean sealed capture, KEPT for tools/hr1_witness.py --integration.
    const std::string rec = tmpPath("sr_hr1_uniq.bin");
    sr::gfx::hle::resetTraceCloseCountForTest();
    W::setUniqStickyNonceForTest(0x6A11C0DFu);
    W::setUniqStickyFileForTest(rec.c_str());
    K::gpuPhysRouteResetForTest();
    for (int i = 0; i < 3; i++) call(0x1000u, 0x8CC00000u);
    sr::gfx::hle::noteRecorderClosedForTest();
    const K::GpuPhysRouteSnapshot s = K::gpuPhysRouteSnapshotForTest();
    check(s.taken && s.atSeal == 1 && s.nonce == 0x6A11C0DFu && s.sealSeq > 0 &&
              s.admitted == 3 && s.settled == 3 && s.completed == 3,
          "G6 a clean sealed capture: 3 delegations settled, bound to its seal (kept for the "
          "witness integration)");
    W::setUniqStickyFileForTest(nullptr);
    W::setUniqCensusForTest(-1);
  }

  // ── E: zero-fill is the guest's (attrs bit 30), on reused memory ─────────
  K::setPhysLifoForTest(1);
  if (mmFree) {
    const uint32_t x = call(0x2000u, 0x8CC00000u);
    if (uint8_t* p = K::guestVAToHost(x)) std::memset(p, 0xAB, 0x2000);
    mmFree(0, x, 0, 0, 0, 0);
    const uint32_t y = call(0x2000u, 0xCCC00000u);     // bit 30: zero-fill
    check(y == x && filled(y, 0x2000u, 0x00),
          "E1 attrs bit 30: the guest's own zero-fill ran on the reused block");
    if (uint8_t* p = K::guestVAToHost(y)) std::memset(p, 0xCD, 0x2000);
    mmFree(0, y, 0, 0, 0, 0);
    const uint32_t z = call(0x2000u, 0x8CC00000u);     // no bit 30
    check(z == x && filled(z, 0x2000u, 0xCD),
          "E2 without bit 30 nothing zero-fills: the override no longer memsets the guest's "
          "physical requests");
    mmFree(0, z, 0, 0, 0, 0);
  }
  K::setPhysLifoForTest(0);

  // ── F: exhaustion - the guest's failure path, no heap fallback ───────────
  {
    K::gpuPhysRouteResetForTest();
    uint32_t last = 1;
    int attempts = 0, below = 0;
    for (; attempts < 16 && last != 0; attempts++) {   // 256 MB each
      last = call(0x10000000u, 0x8CC00000u);
      if (last && last < kWinA) below++;
    }
    const K::GpuPhysRouteSnapshot s0 = now();
    const uint64_t g0 = W::registryGeneration();
    K::writeGuestBE32(kThr + 352u, 0u);
    const uint32_t f = call(0x10000000u, 0x8CC00000u);
    const K::GpuPhysRouteSnapshot s1 = now();
    std::snprintf(name, sizeof name,
                  "F1 exhausted (%d grants, then a null on attempt %d): null, counted as a "
                  "completed delegation with a null", attempts - 1, attempts);
    check(last == 0 && f == 0 && s1.completed == s0.completed + 1 && s1.null == s0.null + 1,
          name);
    check(K::readGuestU32BE(kThr + 352u) == 8u,
          "F2 the guest's own error path ran: SetLastError(ERROR_NOT_ENOUGH_MEMORY)");
    check(W::registryGeneration() == g0 && s1.heapAfterClaim == s0.heapAfterClaim,
          "F3 no heap fallback and nothing registered");
    std::snprintf(name, sizeof name,
                  "F4 delegated grants below 0xA0000000 are counted exactly (%d here): a non-null "
                  "return is not adequate provisioning", below);
    check(below >= 1 && s1.belowA == (uint64_t)below, name);
    // The import's existing size rules, counted (not fixed): a request above
    // the 512 MB cap is truncated, one near 4 GB wraps in 32-bit rounding.
    call(0x20000001u, 0x8CC00000u);
    const K::GpuPhysRouteSnapshot s2 = now();
    check(s2.mmTruncated == s1.mmTruncated + 1 && s2.mmWrapped == s1.mmWrapped,
          "F5 a request above the import's 512 MB cap is counted as TRUNCATED");
    call(0xFFFFF001u, 0x8CC00000u);
    const K::GpuPhysRouteSnapshot s3 = now();
    check(s3.mmWrapped == s2.mmWrapped + 1 && s3.mmTruncated == s2.mmTruncated,
          "F6 a request whose 32-bit page rounding wraps is counted as WRAPPED");
  }

  K::setGpuPhysRouteForTest(-1);
  std::printf("[GPU_PHYS_ROUTE_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
