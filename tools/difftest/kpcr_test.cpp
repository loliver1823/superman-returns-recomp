// iter-463 PLATFORM FIX: per-thread guest KPCR (SR_KPCR) + Ke/Kf spinlocks
// (SR_KE_SPINLOCK). kernel/kpcr.{hpp,cpp}.
//
// Drives the REAL recompiled readers of r13 (RtlSetLastError_KpcrThread_sub_82469A70,
// RtlGetLastError_KpcrThread_sub_82469AC0, KeGetCurrentProcessorNumber_Kpcr_sub_82469FD8)
// and the REAL import glue (__imp__KfAcquireSpinLock & co.: the glue publishes
// the caller's r13 to the export), plus the real ExCreateThread export.
//
// Run: build/difftest.exe --kpcr-test                (exit code = failed checks)
//      SR_KPCR=0 build/difftest.exe --kpcr-test      (r13 stays 0: the KPCR and
//      spinlock checks FAIL - that is the attribution check)
//
// Mutation checks (in-process): each defect is re-created and the test's own
// predicate must SEE it - r13 = 0 on two threads must make their last errors
// collide (T2m), and the generic-stub spinlocks must lose updates (T3m).
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_call.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/kpcr.hpp"
#include "kernel/kthread_objects.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(sub_82469A70);   // RtlSetLastError(r3)       [[r13+0x100]+0x160] unless [r13+0x150]
PPC_EXTERN_FUNC(sub_82469AC0);   // RtlGetLastError() -> r3
PPC_EXTERN_FUNC(sub_82469FD8);   // KeGetCurrentProcessorNumber() -> r3 = [r13+0x10C]
PPC_EXTERN_FUNC(__imp__KfAcquireSpinLock);
PPC_EXTERN_FUNC(__imp__KfReleaseSpinLock);
PPC_EXTERN_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql);
PPC_EXTERN_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql);
PPC_EXTERN_FUNC(__imp__KeRaiseIrqlToDpcLevel);
PPC_EXTERN_FUNC(__imp__KfLowerIrql);
PPC_EXTERN_FUNC(__imp__KeSetAffinityThread);

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#endif

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

uint8_t* g_base = nullptr;
constexpr size_t kGuestSize = 0x100000000ull;

constexpr uint32_t kHandleOut = 0x70400000u;
constexpr uint32_t kPrevOut = 0x70400040u;
constexpr uint32_t kLock = 0x70400100u;
constexpr uint32_t kCounter = 0x70400200u;
constexpr uint32_t kStackBase = 0x70500000u;   // 4 x 64 KB guest stacks for side threads

K::ExportFn getExp(const char* name) { return K::ExportRegistry::instance().getExport(name); }

uint8_t rd8(uint32_t va) { return *K::guestVAToHost(va); }
uint32_t bswap(uint32_t v) { return _byteswap_ulong(v); }

bool inArena(uint32_t pcr) { return pcr >= K::kKpcrArenaLo && pcr + K::kKpcrSize <= K::kKpcrArenaHi; }

// ExCreateThread, handle-only path (r7 = 0), creation flags in r9.
uint32_t createHandleOnly(uint32_t flags) {
  K::ExportRegistry::setCurrentExtraArgs(flags, 0);
  K::writeGuestBE32(kHandleOut, 0);
  getExp("ExCreateThread")(kHandleOut, 0, 0, 0, 0, 0);
  K::ExportRegistry::setCurrentExtraArgs(0, 0);
  return K::readGuestU32BE(kHandleOut);
}

PPCContext mkCtx(uint32_t r13, uint32_t stackTop) {
  PPCContext c{};
  c.r1.u32 = stackTop;
  c.r13.u32 = r13;
  c.r14.u32 = 0x829C0000u;
  c.lr = 0;
  return c;
}

#ifdef _WIN32
// ── T2: last error / processor number per thread through the real readers ──
struct LeArgs {
  int idx;
  bool zeroR13;                 // mutation: the pre-fix world (r13 = 0)
  uint32_t err;
  HANDLE wrote, go, done;
  uint32_t pcr = 0, readBack = 0, cpu = 0, ktErr = 0;
};
DWORD WINAPI leThread(LPVOID p) {
  auto* a = static_cast<LeArgs*>(p);
  K::setGuestMemory(g_base, kGuestSize);
  a->pcr = a->zeroR13 ? 0u : K::kpcrBindCurrent(0, 0, a->idx == 0 ? 1 : 4);
  const uint32_t top = kStackBase + 0x10000u * uint32_t(a->idx + 1) - 0x100u;
  PPCContext c = mkCtx(a->pcr, top);
  c.r3.u32 = a->err;
  sub_82469A70(c, g_base);                       // SetLastError(err)
  SetEvent(a->wrote);
  WaitForSingleObject(a->go, 5000);              // both have written before either reads
  c = mkCtx(a->pcr, top);
  sub_82469AC0(c, g_base);
  a->readBack = c.r3.u32;
  c = mkCtx(a->pcr, top);
  sub_82469FD8(c, g_base);
  a->cpu = c.r3.u32;
  if (a->pcr) a->ktErr = K::readGuestU32BE(K::readGuestU32BE(a->pcr + 0x100u) + K::kKtOffLastError);
  SetEvent(a->done);
  return 0;
}

// Returns true when each thread read back its OWN error.
bool runLastErrorPair(bool zeroR13, LeArgs out[2]) {
  HANDLE go = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE th[2];
  for (int i = 0; i < 2; i++) {
    out[i].idx = i; out[i].zeroR13 = zeroR13; out[i].err = 0x1111u * uint32_t(i + 1);
    out[i].wrote = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    out[i].done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    out[i].go = go;
    th[i] = CreateThread(nullptr, 8u << 20, leThread, &out[i], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(out[i].wrote, 5000);    // sequential writes: 0 then 1
  }
  SetEvent(go);
  for (int i = 0; i < 2; i++) {
    WaitForSingleObject(out[i].done, 5000);
    WaitForSingleObject(th[i], 5000);
    CloseHandle(th[i]); CloseHandle(out[i].wrote); CloseHandle(out[i].done);
  }
  CloseHandle(go);
  return out[0].readBack == out[0].err && out[1].readBack == out[1].err;
}

// ── T3: spinlock mutual exclusion across 4 host threads with distinct r13 ──
constexpr int kSpinThreads = 4;
constexpr int kSpinIters = 20000;
struct SpinArgs {
  int idx;
  HANDLE start;
  uint32_t pcr = 0;
  std::atomic<uint32_t> ownerViolations{0}, irqlViolations{0};
};
DWORD WINAPI spinThread(LPVOID p) {
  auto* a = static_cast<SpinArgs*>(p);
  K::setGuestMemory(g_base, kGuestSize);
  a->pcr = K::kpcrBindCurrent(0, 0, -1);
  const uint32_t top = kStackBase + 0x10000u * uint32_t(a->idx + 3) - 0x100u;
  const bool atRaised = (a->idx & 1) != 0;       // two threads use the Ke* pair
  WaitForSingleObject(a->start, 5000);
  for (int i = 0; i < kSpinIters; i++) {
    PPCContext c = mkCtx(a->pcr, top);
    uint32_t oldIrql = 0;
    if (atRaised) {
      __imp__KeRaiseIrqlToDpcLevel(c, g_base);
      oldIrql = c.r3.u32;
      c.r3.u32 = kLock;
      __imp__KeAcquireSpinLockAtRaisedIrql(c, g_base);
    } else {
      c.r3.u32 = kLock;
      __imp__KfAcquireSpinLock(c, g_base);
      oldIrql = c.r3.u32;
    }
    // Critical section: owner must be us; a torn read-modify-write of the
    // counter with a pause in the middle loses updates without exclusion.
    if (a->pcr && K::readGuestU32BE(kLock) != a->pcr) a->ownerViolations.fetch_add(1);
    if (a->pcr && rd8(a->pcr + K::kKpcrOffIrql) != 2u) a->irqlViolations.fetch_add(1);
    const uint32_t v = K::readGuestU32BE(kCounter);
    for (int k = 0; k < 40; k++) _mm_pause();
    if ((i & 255) == 0) SwitchToThread();
    K::writeGuestBE32(kCounter, v + 1u);
    c = mkCtx(a->pcr, top);
    c.r3.u32 = kLock;
    if (atRaised) {
      __imp__KeReleaseSpinLockFromRaisedIrql(c, g_base);
      c = mkCtx(a->pcr, top);
      c.r3.u32 = oldIrql;
      __imp__KfLowerIrql(c, g_base);
    } else {
      c.r4.u32 = oldIrql;
      __imp__KfReleaseSpinLock(c, g_base);
    }
  }
  return 0;
}

struct SpinResult { uint32_t counter, ownerViolations, irqlViolations; bool distinct; };
SpinResult runSpinRace() {
  K::writeGuestBE32(kLock, 0);
  K::writeGuestBE32(kCounter, 0);
  HANDLE start = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  SpinArgs args[kSpinThreads];
  HANDLE th[kSpinThreads];
  for (int i = 0; i < kSpinThreads; i++) {
    args[i].idx = i; args[i].start = start;
    th[i] = CreateThread(nullptr, 8u << 20, spinThread, &args[i], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  }
  Sleep(50);
  SetEvent(start);
  WaitForMultipleObjects(kSpinThreads, th, TRUE, 60000);
  SpinResult r{K::readGuestU32BE(kCounter), 0, 0, true};
  for (int i = 0; i < kSpinThreads; i++) {
    CloseHandle(th[i]);
    r.ownerViolations += args[i].ownerViolations.load();
    r.irqlViolations += args[i].irqlViolations.load();
    for (int j = 0; j < i; j++) if (!args[i].pcr || args[i].pcr == args[j].pcr) r.distinct = false;
  }
  CloseHandle(start);
  return r;
}

// ── T6: a GuestCaller host thread gets its own KPCR ─────────────────────────
struct GcArgs { uint32_t r13 = 0, kt = 0, curThread = 0; };
DWORD WINAPI gcThread(LPVOID p) {
  auto* a = static_cast<GcArgs*>(p);
  K::setGuestMemory(g_base, kGuestSize);
  K::GuestCaller gc("KPCR_TEST_GC");
  a->r13 = gc.ctx().r13.u32;
  a->kt = K::kthreadCurrent(false);
  a->curThread = a->r13 ? K::readGuestU32BE(a->r13 + 0x100u) : 0u;
  return 0;
}
void noopRunner(uint32_t, uint32_t, uint32_t, uint8_t*, size_t) {}

// ── T7: the thread binds to ITS KTHREAD/KPCR even when the creator closed the
// only handle before the thread started (review fix, SR_KT_BIND_VA). The real
// guestThreadFunc binds, then calls this runner - which binds r13 exactly as
// runGuestThreadEntry does.
std::atomic<uint32_t> g_t7R13{0}, g_t7Kt{0};
HANDLE g_t7Done = nullptr;
void recordRunner(uint32_t, uint32_t, uint32_t, uint8_t*, size_t) {
  g_t7R13.store(K::kpcrBindCurrent(0, 0, -1));
  g_t7Kt.store(K::kthreadCurrent(false));
  SetEvent(g_t7Done);
}
struct T7Result { uint32_t kt, pcr, r13, curKt; uint8_t cpu; bool ran; };
T7Result runT7(bool bindByVa) {
  T7Result r{};
  K::kthreadBindByVASetForTest(bindByVa ? 1 : 0);
  K::registerExCreateThreadRunner(recordRunner, g_base, kGuestSize);
  g_t7Done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_t7R13.store(0); g_t7Kt.store(0);
  // A running (not suspended) worker in the XNet shape; created SUSPENDED here
  // only so the close provably lands before the thread binds.
  K::ExportRegistry::setCurrentExtraArgs(0x1u, 0);
  K::writeGuestBE32(kHandleOut, 0);
  getExp("ExCreateThread")(kHandleOut, 0, 0, 0, 0x82566760u, 0);
  K::ExportRegistry::setCurrentExtraArgs(0, 0);
  const uint32_t h = K::readGuestU32BE(kHandleOut);
  r.kt = K::kthreadLookup(h);
  r.pcr = K::kpcrForKThread(r.kt);
  K::kthreadCloseHandle(h);            // the creator's NtClose: last handle of a LIVE thread
  getExp("NtResumeThread")(h, 0, 0, 0, 0, 0);
  r.ran = WaitForSingleObject(g_t7Done, 5000) == WAIT_OBJECT_0;
  r.r13 = g_t7R13.load();
  r.curKt = r.r13 ? K::readGuestU32BE(r.r13 + 0x100u) : 0u;
  r.cpu = r.r13 ? rd8(r.r13 + 0x10Cu) : 0xFFu;
  CloseHandle(g_t7Done);
  K::registerExCreateThreadRunner(noopRunner, g_base, kGuestSize);
  K::kthreadBindByVASetForTest(-1);
  return r;
}
#endif
}  // namespace

int runKpcrTest() {
  std::printf("[KPCR_TEST] per-thread KPCR (SR_KPCR) + Ke/Kf spinlocks (SR_KE_SPINLOCK)\n");
  g_base = K::getGuestBase();
  K::registerStubs();
#ifdef _WIN32
  K::registerExCreateThreadRunner(noopRunner, g_base, kGuestSize);   // GuestCaller's global base
#endif
  const bool kpcrOn = K::kpcrEnabled();
  const bool spinOn = K::keSpinlockEnabled();
  std::printf("  gates SR_KPCR=%s SR_KE_SPINLOCK=%s%s\n", kpcrOn ? "ON" : "OFF", spinOn ? "ON" : "OFF",
              kpcrOn ? "" : " (r13 = 0 world: failures below ARE the defect)");

#ifdef _WIN32
  // The creating ("parent") thread: bind a KPCR on CPU 3.
  const uint32_t mainPcr = K::kpcrBindCurrent(0x70600000u, 0x70500000u, 3);
  check(inArena(mainPcr), "T0a the test thread's KPCR sits in [D00E0000,D0100000)");
  check(mainPcr && K::kpcrCurrent() == mainPcr && K::kpcrBindCurrent(0, 0, -1) == mainPcr,
        "T0b binding is idempotent per host thread");

  // ── T1: ExCreateThread builds the KPCR next to the KTHREAD (Xenia Create) ──
  {
    const uint32_t h = createHandleOnly(0x04000000u);   // processor mask 4 -> CPU 2
    const uint32_t kt = K::kthreadLookup(h);
    const uint32_t pcr = K::kpcrForKThread(kt);
    check(kt && inArena(pcr) && pcr != mainPcr, "T1a ExCreateThread maps the KTHREAD to its own KPCR");
    check(pcr && K::readGuestU32BE(pcr + 0x100u) == kt, "T1b +0x100 prcb.current_thread = the thread's KTHREAD");
    check(pcr && rd8(pcr + 0x10Cu) == 2u && rd8(kt + 0xBFu) == 2u,
          "T1c mask 0x04 -> current_cpu 2 in KPCR+0x10C and KTHREAD+0xBF");
    check(pcr && K::readGuestU32BE(pcr + 0x150u) == 0u && rd8(pcr + 0x18u) == 0u,
          "T1d +0x150 dpc_active = 0, +0x18 irql = 0");
    check(pcr && K::readGuestU32BE(pcr + 0x2A8u) == pcr + 0x100u && K::readGuestU32BE(pcr + 0x34u) == pcr &&
              K::readGuestU32BE(pcr + 0x30u) == 0u,
          "T1e prcb pointer (+0x2A8) and be64 pcr_ptr (+0x30)");
    check(pcr && K::readGuestU32BE(pcr + 0x70u) - K::readGuestU32BE(pcr + 0x74u) == 256u * 1024u &&
              K::readGuestU32BE(pcr + 0x74u) == K::readGuestU32BE(kt + 0x60u),
          "T1f stack_base/stack_end span the thread's 256 KB stack");
    const uint32_t h2 = createHandleOnly(0u);             // no mask -> the parent's CPU
    const uint32_t pcr2 = K::kpcrForKThread(K::kthreadLookup(h2));
    check(pcr2 && rd8(pcr2 + 0x10Cu) == 3u, "T1g mask 0 inherits the creating thread's CPU (3)");
    const uint32_t h3 = createHandleOnly(0x20000000u);    // mask 0x20 -> CPU 5
    const uint32_t pcr3 = K::kpcrForKThread(K::kthreadLookup(h3));
    check(pcr3 && rd8(pcr3 + 0x10Cu) == 5u, "T1h mask 0x20 -> CPU 5");

    // ── T5: KeSetAffinityThread through the glue ──
    PPCContext c = mkCtx(mainPcr, 0x705FFF00u);
    c.r3.u32 = kt; c.r4.u32 = 0x10u; c.r5.u32 = kPrevOut;
    K::writeGuestBE32(kPrevOut, 0xCCCCCCCCu);
    __imp__KeSetAffinityThread(c, g_base);
    check(c.r3.u32 == 0u && K::readGuestU32BE(kPrevOut) == (1u << 2) && rd8(pcr + 0x10Cu) == 4u &&
              rd8(kt + 0xBFu) == 4u,
          "T5a KeSetAffinityThread(kt, 0x10): prev = 1<<2, CPU 4 in KPCR and KTHREAD");
    c = mkCtx(mainPcr, 0x705FFF00u);
    c.r3.u32 = kt; c.r4.u32 = 0u; c.r5.u32 = 0u;
    __imp__KeSetAffinityThread(c, g_base);
    check(c.r3.u32 == K::kKeStatusInvalidParameter, "T5b affinity 0 -> STATUS_INVALID_PARAMETER");
    c = mkCtx(mainPcr, 0x705FFF00u);
    c.r3.u32 = 0x70400800u; c.r4.u32 = 1u; c.r5.u32 = 0u;
    __imp__KeSetAffinityThread(c, g_base);
    check(c.r3.u32 == K::kStatusInvalidHandleKe, "T5c a non-thread pointer -> STATUS_INVALID_HANDLE");
  }

  // ── T2: the real guest readers see per-thread state ──
  {
    LeArgs le[2];
    const bool own = runLastErrorPair(false, le);
    check(own, "T2a RtlSetLastError/RtlGetLastError are per-thread (each thread reads its own)");
    check(le[0].pcr && le[0].ktErr == le[0].err && le[1].ktErr == le[1].err,
          "T2b the value lives in the thread's KTHREAD+0x160");
    check(le[0].cpu == 1u && le[1].cpu == 4u, "T2c KeGetCurrentProcessorNumber reads KPCR+0x10C");
    LeArgs m[2];
    const bool mutantOwn = runLastErrorPair(true, m);
    check(!mutantOwn, "T2m MUTATION: with r13 = 0 on both threads the errors collide (test sees the defect)");
    // DPC: last error is not written while dpc_active.
    PPCContext c = mkCtx(mainPcr, 0x705FFF00u);
    c.r3.u32 = 0x5555u;
    sub_82469A70(c, g_base);
    {
      K::KpcrDpcScope dpc(mainPcr);
      const bool inDpc = mainPcr && K::readGuestU32BE(mainPcr + 0x150u) == 1u &&
                         rd8(mainPcr + 0x18u) == 2u && rd8(mainPcr + 0x0Cu) == 1u;
      check(inDpc, "T2d DPC scope: dpc_active 1, irql 2, processtype_in_dpc TITLE");
      c = mkCtx(mainPcr, 0x705FFF00u);
      c.r3.u32 = 0x6666u;
      sub_82469A70(c, g_base);
    }
    c = mkCtx(mainPcr, 0x705FFF00u);
    sub_82469AC0(c, g_base);
    check(c.r3.u32 == 0x5555u && mainPcr && K::readGuestU32BE(mainPcr + 0x150u) == 0u &&
              rd8(mainPcr + 0x18u) == 0u && rd8(mainPcr + 0x0Cu) == 0u,
          "T2e SetLastError inside a DPC is skipped; the scope restores irql/dpc/IDLE");
  }

  // ── T4: IRQL bookkeeping ──
  {
    PPCContext c = mkCtx(mainPcr, 0x705FFF00u);
    __imp__KeRaiseIrqlToDpcLevel(c, g_base);
    const bool raised = c.r3.u32 == 0u && mainPcr && rd8(mainPcr + 0x18u) == 2u;
    c = mkCtx(mainPcr, 0x705FFF00u);
    c.r3.u32 = 0u;
    __imp__KfLowerIrql(c, g_base);
    check(raised && rd8(mainPcr + 0x18u) == 0u, "T4a KeRaiseIrqlToDpcLevel -> old 0, irql 2; KfLowerIrql(0) -> 0");
    K::writeGuestBE32(kLock, 0);
    c = mkCtx(mainPcr, 0x705FFF00u);
    c.r3.u32 = kLock;
    __imp__KfAcquireSpinLock(c, g_base);
    const uint32_t old = c.r3.u32;
    const bool held = K::readGuestU32BE(kLock) == mainPcr && rd8(mainPcr + 0x18u) == 2u;
    c = mkCtx(mainPcr, 0x705FFF00u);
    c.r3.u32 = kLock; c.r4.u32 = old;
    __imp__KfReleaseSpinLock(c, g_base);
    check(old == 0u && held && K::readGuestU32BE(kLock) == 0u && rd8(mainPcr + 0x18u) == 0u,
          "T4b KfAcquireSpinLock: lock word = BE r13, irql 2; KfReleaseSpinLock(old) -> 0 / irql 0");
  }

  // ── T3: mutual exclusion across 4 host threads with distinct r13 ──
  {
    const K::KeSpinStats s0 = K::keSpinStats();
    const SpinResult r = runSpinRace();
    const K::KeSpinStats s1 = K::keSpinStats();
    std::printf("  T3 counter=%u/%u ownerViolations=%u irqlViolations=%u contended=%llu\n", r.counter,
                kSpinThreads * kSpinIters, r.ownerViolations, r.irqlViolations,
                (unsigned long long)(s1.contended - s0.contended));
    check(r.distinct, "T3a the 4 threads hold 4 distinct KPCRs");
    check(r.counter == uint32_t(kSpinThreads * kSpinIters), "T3b no lost update: exclusion held");
    check(r.ownerViolations == 0u && r.irqlViolations == 0u,
          "T3c inside the lock: owner word = own r13, irql 2 (Kf and Ke-at-raised mixed)");
    check(s1.recursions == s0.recursions && s1.foreignReleases == s0.foreignReleases && s1.noPcr == s0.noPcr,
          "T3d no recursion / foreign release / r13=0 fallback");

    // Mutation: the generic stubs (gate off) must lose updates.
    K::keSpinlockSetEnabledForTest(0);
    const SpinResult m = runSpinRace();
    K::keSpinlockSetEnabledForTest(-1);
    std::printf("  T3m counter=%u/%u with the generic stubs\n", m.counter, kSpinThreads * kSpinIters);
    check(m.counter < uint32_t(kSpinThreads * kSpinIters),
          "T3m MUTATION: generic-stub spinlocks lose updates (test sees the defect)");
  }

  // ── T6: GuestCaller threads get a KPCR ──
  {
    GcArgs g;
    HANDLE th = CreateThread(nullptr, 8u << 20, gcThread, &g, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    check(inArena(g.r13) && g.kt && g.curThread == g.kt,
          "T6 GuestCaller: r13 = its own KPCR whose current_thread = its KTHREAD");
  }

  // ── T7: handle closed before the thread starts ──
  {
    const T7Result r = runT7(true);
    check(r.ran && r.kt && r.pcr && r.r13 == r.pcr && r.curKt == r.kt && g_t7Kt.load() == r.kt,
          "T7a handle closed before start: the thread still binds its own KTHREAD + KPCR");
    check(r.ran && r.cpu == 3u, "T7b ... and keeps the CPU inherited from its creator (3)");
    const T7Result m = runT7(false);
    std::printf("  T7m old handle bind: r13=%08X (built %08X) kt=%08X (built %08X)\n", m.r13, m.pcr,
                m.curKt, m.kt);
    check(m.ran && m.r13 != m.pcr,
          "T7m MUTATION: bind-by-handle runs the thread on another KPCR (test sees the defect)");
  }
#else
  check(false, "kpcr test needs Win32");
#endif

  std::printf("[KPCR_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
