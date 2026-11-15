// iter-462 PLATFORM FIX: guest KTHREAD objects + ObReferenceObjectByHandle
// (kernel/kthread_objects.{hpp,cpp}, SR_OB_KTHREAD default ON).
//
// Drives the REAL recompiled GetExitCodeThread (sub_82469E98) through the
// import glue and the export registry, plus the real ExCreateThread /
// ExTerminateThread / NtDuplicateObject / NtClose exports.
//
// Run: build/difftest.exe --obref-test                  (exit code = failed checks)
//      SR_OB_KTHREAD=0 build/difftest.exe --obref-test  (today's generic stub:
//      the defect is visible, so T1.. FAIL - that is the attribution check)
//
// The defect (archive/v1_eapool/): the generic stub returned success without
// writing *obj, so sub_82469E98 read its stale stack slot as the KTHREAD
// (lwz r3,80(r1); lwz r11,4(r3); &0xFF; nonzero -> exit=[r3+0x140]) and
// reported LIVE threads as exited; EAThreadPool_FixThreads then cloned them.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/kthread_objects.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(sub_82469E98);   // GetExitCodeThread(h, lpExitCode) -> BOOL

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

uint8_t* g_base = nullptr;
constexpr size_t kGuestSize = 0x100000000ull;   // difftest_main's flat guest

// Scratch guest memory (difftest's flat 4 GB, all committed).
constexpr uint32_t kStackMain = 0x70100000u;    // r1 for calls from the main thread
constexpr uint32_t kStackSide = 0x70180000u;    // r1 for calls from a side thread
constexpr uint32_t kPcr = 0x70200000u;          // r13: SetLastError on the failure path
constexpr uint32_t kPcrThread = 0x70201000u;    //   writes [r13+256]+352
constexpr uint32_t kOutMain = 0x70300000u;      // *lpExitCode
constexpr uint32_t kOutSide = 0x70300040u;
constexpr uint32_t kHandleOut = 0x70300080u;    // ExCreateThread *pHandle
constexpr uint32_t kDupOut = 0x703000C0u;       // NtDuplicateObject *new_handle
constexpr uint32_t kObjOut = 0x70300100u;       // ObReferenceObjectByHandle *obj
constexpr uint32_t kGarbage = 0x70310000u;      // T1b: a KTHREAD lookalike left in the slot
constexpr uint32_t kGarbageExit = 0xDEAD0001u;
constexpr uint32_t kTypeThread = 0xD0016000u;   // ExThreadObjectType (SR_DATA_IMPORTS)
constexpr uint32_t kStillActive = 0x103u;

uint32_t slotFor(uint32_t stackTop) { return stackTop - 112u + 80u; }   // stwu r1,-112; r5=r1+80

struct ExitRes { uint32_t ret, code, slot; };

// GetExitCodeThread through the recompiled guest function. `slotInit` is the
// stale value sitting in its ObRef out-slot before the call.
ExitRes getExitCode(uint32_t h, uint32_t slotInit, uint32_t stackTop = kStackMain,
                    uint32_t outVA = kOutMain) {
  K::writeGuestBE32(slotFor(stackTop), slotInit);
  K::writeGuestBE32(outVA, 0xCCCCCCCCu);
  PPCContext c{};
  c.r1.u32 = stackTop;
  c.r13.u32 = kPcr;
  c.r14.u32 = 0x829C0000u;
  c.r3.u32 = h;
  c.r4.u32 = outVA;
  c.lr = 0;
  sub_82469E98(c, K::getGuestBase());
  return ExitRes{c.r3.u32, K::readGuestU32BE(outVA), K::readGuestU32BE(slotFor(stackTop))};
}

K::ExportFn getExp(const char* name) { return K::ExportRegistry::instance().getExport(name); }

// ExCreateThread with r7 = 0: the handle-only path (no host thread).
uint32_t createHandleOnly() {
  K::ExportRegistry::setCurrentExtraArgs(0, 0);
  K::writeGuestBE32(kHandleOut, 0);
  getExp("ExCreateThread")(kHandleOut, 0, 0, 0, 0, 0);
  return K::readGuestU32BE(kHandleOut);
}

uint32_t obRef(uint32_t h, uint32_t type, uint32_t outInit) {
  K::writeGuestBE32(kObjOut, outInit);
  return getExp("ObReferenceObjectByHandle")(h, type, kObjOut, 0, 0, 0);
}

uint32_t arenaCursor() {
  const uint32_t c = K::kthreadArenaCursorForTest(0);
  K::kthreadArenaCursorForTest(c);
  return c;
}

bool inArena(uint32_t kt) {
  return kt >= K::kKThreadArenaLo && kt + K::kKThreadSize <= K::kKThreadArenaHi;
}

#ifdef _WIN32
// Poll (bounded) until a KTHREAD's signal_state low byte is 1.
bool waitSignalled(uint32_t kt, DWORD ms) {
  for (DWORD t = 0; t < ms; t += 5) {
    if (kt && (K::readGuestU32BE(kt + K::kKtOffSignalState) & 0xFFu) == 1u) return true;
    Sleep(5);
  }
  return false;
}

// ── T2: ExTerminateThread on a raw thread bound to a handle ──────────────────
struct TermArgs { uint32_t h; };
DWORD WINAPI termThread(LPVOID p) {
  auto* a = static_cast<TermArgs*>(p);
  K::setGuestMemory(g_base, kGuestSize);
  K::setCurrentThreadHandle(a->h);
  K::kthreadBindCurrent(a->h);
  K::ExportRegistry::setCurrentExtraArgs(0, 0);
  getExp("ExTerminateThread")(0x1234u, 0, 0, 0, 0, 0);   // never returns (ExitThread)
  return 0xFFFFFFFFu;
}

// ── T2n: the real runner path (ExCreateThread -> guestThreadFunc) ───────────
std::atomic<uint32_t> g_runnerHandle{0};
std::atomic<int> g_runnerBound{-1};
std::atomic<bool> g_runnerStarted{false};
HANDLE g_runnerGo = nullptr;
void testRunner(uint32_t, uint32_t, uint32_t, uint8_t* base, size_t size) {
  K::setGuestMemory(base, size);
  const uint32_t h = K::getCurrentThreadHandle();
  g_runnerHandle.store(h);
  g_runnerBound.store(K::kthreadCurrent(false) != 0 &&
                      K::kthreadCurrent(false) == K::kthreadLookup(h) ? 1 : 0);
  g_runnerStarted.store(true);
  WaitForSingleObject(g_runnerGo, 5000);
  // return normally -> guestThreadFunc marks exit code 0
}

// ── T3 / T4 / T4b side threads ───────────────────────────────────────────────
struct SideArgs {
  uint32_t bindHandle;        // 0 = fresh thread with no handle
  std::atomic<uint32_t> obStatus{0xFFFFFFFFu}, obObj{0}, exitRet{0}, exitCode{0};
  std::atomic<uint32_t> dupHandle{0}, opensAfterDup{0}, opensAfterClose{0};
  std::atomic<uint32_t> exitAfterClose{0};
  HANDLE ready = nullptr, done = nullptr;
  int mode = 0;               // 3 = pseudo-handle, 4 = dup on fresh thread, 5 = dup+close of own handle
};
DWORD WINAPI sideThread(LPVOID p) {
  auto* a = static_cast<SideArgs*>(p);
  K::setGuestMemory(g_base, kGuestSize);
  if (a->bindHandle) {
    K::setCurrentThreadHandle(a->bindHandle);
    K::kthreadBindCurrent(a->bindHandle);
  }
  K::ExportRegistry::setCurrentExtraArgs(0, 0);
  if (a->mode == 3) {
    a->obStatus.store(obRef(K::kCurrentThreadPseudoHandle, kTypeThread, 0x77777777u));
    a->obObj.store(K::readGuestU32BE(kObjOut));
    const ExitRes r = getExitCode(K::kCurrentThreadPseudoHandle, 0, kStackSide, kOutSide);
    a->exitRet.store(r.ret);
    a->exitCode.store(r.code);
  } else if (a->mode == 4 || a->mode == 5) {
    K::writeGuestBE32(kDupOut, 0);
    getExp("NtDuplicateObject")(K::kCurrentThreadPseudoHandle, kDupOut, 2u, 0, 0, 0);
    const uint32_t d = K::readGuestU32BE(kDupOut);
    a->dupHandle.store(d);
    a->opensAfterDup.store(K::kthreadOpenCount(d));
    if (a->mode == 5) {
      getExp("NtClose")(d, 0, 0, 0, 0, 0);   // close the "duplicate" only
      a->opensAfterClose.store(K::kthreadOpenCount(d));
    }
  }
  SetEvent(a->ready);
  WaitForSingleObject(a->done, 5000);   // stay alive while the main thread checks
  return 0;
}

bool runSide(SideArgs& a, HANDLE& th) {
  a.ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  a.done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  th = CreateThread(nullptr, 8u << 20, sideThread, &a, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  return th && WaitForSingleObject(a.ready, 5000) == WAIT_OBJECT_0;
}
void finishSide(SideArgs& a, HANDLE th) {
  SetEvent(a.done);
  if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
  CloseHandle(a.ready);
  CloseHandle(a.done);
}
#endif
}  // namespace

int runObRefTest() {
  std::printf("[OBREF_TEST] guest KTHREADs + ObReferenceObjectByHandle (SR_OB_KTHREAD)\n");
  g_base = K::getGuestBase();
  K::registerStubs();
#ifdef _WIN32
  K::registerExCreateThreadRunner(testRunner, g_base, kGuestSize);
#endif
  const bool gateOn = K::obKThreadEnabled();
  std::printf("  gate SR_OB_KTHREAD = %s\n",
              gateOn ? "ON" : "OFF (today's generic stub: failures below ARE the defect)");

  // The live-run world: the ExThreadObjectType cell resolved (SR_DATA_IMPORTS),
  // and the null page poisoned as in poolw1 (BE32 @4 = 1 -> byte 7 = 1, @0x140 = 0).
  K::writeGuestBE32(K::kExThreadObjectTypeCell, kTypeThread);
  K::writeGuestBE32(0x4u, 0x00000001u);
  K::writeGuestBE32(0x140u, 0u);
  K::writeGuestBE32(kPcr + 336u, 0u);
  K::writeGuestBE32(kPcr + 256u, kPcrThread);
  // T1b's stack garbage: a nonzero pointer to bytes whose +7 is 1.
  K::writeGuestBE32(kGarbage + 4u, 0x00000001u);
  K::writeGuestBE32(kGarbage + 0x140u, kGarbageExit);

  // ── T0: the KTHREAD ExCreateThread builds (Xenia InitializeGuestObject) ────
  const uint32_t h0 = createHandleOnly();
  const uint32_t kt0 = K::kthreadLookup(h0);
  check(h0 != 0 && inArena(kt0), "T0a ExCreateThread maps its handle to a KTHREAD in [D0020000,D00E0000)");
  check(kt0 && (K::readGuestU32BE(kt0) >> 24) == 6u && K::readGuestU32BE(kt0 + 4) == 0u,
        "T0b type=6 (DISPATCHER_THREAD), signal_state=0");
  check(kt0 && K::readGuestU32BE(kt0 + 0x08) == kt0 + 0x08 && K::readGuestU32BE(kt0 + 0x0C) == kt0 + 0x08 &&
            K::readGuestU32BE(kt0 + 0x10) == kt0 + 0x10 && K::readGuestU32BE(kt0 + 0x14) == kt0 + 0x10,
        "T0c wait list and mutants list are empty self-links");
  check(kt0 && K::readGuestU32BE(kt0 + 0x60) != 0 &&
            K::readGuestU32BE(kt0 + 0x5C) - K::readGuestU32BE(kt0 + 0x60) == 256u * 1024u &&
            K::readGuestU32BE(kt0 + 0x14C) == h0 && K::readGuestU32BE(kt0 + 0x16C) == 0u,
        "T0d stack_base/limit span the 256 KB stack, thread_id = handle, creation_flags");
  check(K::kthreadOpenCount(h0) == 1u, "T0e the creator's handle is one open");

  // ── T1: a live thread reads STILL_ACTIVE (was: stale slot 0 -> null page -> 0)
  const uint32_t h1 = createHandleOnly();
  const uint32_t kt1 = K::kthreadLookup(h1);
  {
    const ExitRes r = getExitCode(h1, 0u);
    std::printf("       T1: ret=%u code=%08X slot=%08X\n", r.ret, r.code, r.slot);
    check(r.ret == 1u && r.code == kStillActive, "T1 GetExitCodeThread(live) = TRUE, 0x103 (stale slot 0)");
  }
  // ── T1b: the stale slot holds a nonzero pointer to garbage whose +7 is 1 ──
  {
    const ExitRes r = getExitCode(h1, kGarbage);
    std::printf("       T1b: ret=%u code=%08X slot=%08X\n", r.ret, r.code, r.slot);
    check(r.ret == 1u && r.code == kStillActive && r.slot == kt1 && kt1 != 0,
          "T1b stack-garbage slot (independent of the null page): still 0x103, slot = the KTHREAD");
  }

  // ── T1m: MUTATION - the same calls with the gate OFF show the defect ───────
  {
    K::obKThreadSetEnabledForTest(0);
    const uint32_t curBefore = arenaCursor();
    const uint32_t hm = createHandleOnly();
    const bool noAlloc = arenaCursor() == curBefore && K::kthreadLookup(hm) == 0;
    const ExitRes r0 = getExitCode(hm, 0u);
    const ExitRes rg = getExitCode(hm, kGarbage);
    const ExitRes rl = getExitCode(h1, 0u);   // even a thread that HAS a KTHREAD
    const uint32_t st = obRef(h1, kTypeThread, 0x55555555u);
    const uint32_t obj = K::readGuestU32BE(kObjOut);
    K::obKThreadSetEnabledForTest(-1);
    std::printf("       T1m (gate OFF): slot0 -> code=%08X, garbage slot -> code=%08X, "
                "live h1 -> code=%08X, ObRef st=%08X obj=%08X\n",
                r0.code, rg.code, rl.code, st, obj);
    check(noAlloc, "T1m-a gate OFF: ExCreateThread allocates no KTHREAD (today's behaviour)");
    check(st == 0u && obj == 0x55555555u, "T1m-b gate OFF: ObRef = generic stub (success, *obj NOT written)");
    check(r0.ret == 1u && r0.code == 0u,
          "T1m-c gate OFF: live thread reported EXITED with code 0 (the defect, stale slot 0)");
    check(rg.ret == 1u && rg.code == kGarbageExit,
          "T1m-d gate OFF: live thread reported EXITED with the garbage's code (the defect, T1b shape)");
    check(rl.code != kStillActive, "T1m-e gate OFF: the test cannot see STILL_ACTIVE without the fix");
  }

#ifdef _WIN32
  // ── T2: ExTerminateThread(0x1234) is an exit site ──────────────────────────
  {
    const uint32_t h2 = createHandleOnly();
    const ExitRes before = getExitCode(h2, 0u);
    TermArgs ta{h2};
    HANDLE th = CreateThread(nullptr, 8u << 20, termThread, &ta, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    DWORD hostExit = 0;
    const bool joined = th && WaitForSingleObject(th, 5000) == WAIT_OBJECT_0;
    if (th) { GetExitCodeThread(th, &hostExit); CloseHandle(th); }
    const ExitRes r = getExitCode(h2, 0u);
    const uint32_t kt2 = K::kthreadLookup(h2);
    std::printf("       T2: before=%08X after ret=%u code=%08X hostExit=%08lX\n",
                before.code, r.ret, r.code, (unsigned long)hostExit);
    check(joined && before.code == kStillActive && r.ret == 1u && r.code == 0x1234u,
          "T2 ExTerminateThread(0x1234): GetExitCodeThread = TRUE, 0x1234");
    check(kt2 && (K::readGuestU32BE(kt2 + 0xBC) & 0x0000FF00u) == 0x100u &&
              (K::readGuestU32BE(kt2 + 4) & 0xFFu) == 1u,
          "T2b terminated=1 (+0xBE) and signal_state=1");
  }

  // ── T2n: the real runner path - live while running, exit 0 on normal return
  {
    g_runnerGo = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    K::ExportRegistry::setCurrentExtraArgs(0, 0);
    K::writeGuestBE32(kHandleOut, 0);
    getExp("ExCreateThread")(kHandleOut, 0, 0, 0, 0x82469E98u /*in range; the test runner ignores it*/, 0x1111u);
    const uint32_t hn = K::readGuestU32BE(kHandleOut);
    bool started = false;
    for (int i = 0; i < 1000 && !(started = g_runnerStarted.load()); i++) Sleep(5);
    const ExitRes live = getExitCode(hn, 0u);
    SetEvent(g_runnerGo);
    const uint32_t ktn = K::kthreadLookup(hn);
    const bool exited = waitSignalled(ktn, 5000);
    const ExitRes r = getExitCode(hn, 0u);
    std::printf("       T2n: handle=%u runnerHandle=%u bound=%d live=%08X after=%08X\n",
                hn, g_runnerHandle.load(), g_runnerBound.load(), live.code, r.code);
    check(started && g_runnerHandle.load() == hn && g_runnerBound.load() == 1,
          "T2n-a guestThreadFunc binds the host thread to its KTHREAD");
    check(live.ret == 1u && live.code == kStillActive, "T2n-b running thread: 0x103");
    check(exited && r.ret == 1u && r.code == 0u, "T2n-c normal return: exit code 0 via guestThreadFunc");
    CloseHandle(g_runnerGo);
  }

  // ── T3: the pseudo-handle on a bound thread ────────────────────────────────
  {
    const uint32_t h3 = createHandleOnly();
    SideArgs a; a.bindHandle = h3; a.mode = 3;
    HANDLE th = nullptr;
    const bool ok = runSide(a, th);
    std::printf("       T3: st=%08X obj=%08X kt=%08X exit=%08X\n",
                a.obStatus.load(), a.obObj.load(), K::kthreadLookup(h3), a.exitCode.load());
    check(ok && a.obStatus.load() == 0u && a.obObj.load() == K::kthreadLookup(h3) && a.obObj.load() != 0,
          "T3a ObRef(0xFFFFFFFE) = this thread's KTHREAD");
    check(a.exitRet.load() == 1u && a.exitCode.load() == kStillActive,
          "T3b GetExitCodeThread(0xFFFFFFFE) on a bound thread = 0x103");
    finishSide(a, th);
  }

  // ── T4: NtDuplicateObject(-2) on a fresh thread gives it a live handle ─────
  {
    SideArgs a; a.bindHandle = 0; a.mode = 4;
    HANDLE th = nullptr;
    const bool ok = runSide(a, th);
    const uint32_t hd = a.dupHandle.load();
    const ExitRes r = getExitCode(hd, 0u);   // from ANOTHER thread (this one)
    std::printf("       T4: dup=%u opens=%u ret=%u code=%08X kt=%08X\n",
                hd, a.opensAfterDup.load(), r.ret, r.code, K::kthreadLookup(hd));
    check(ok && hd != 0 && a.opensAfterDup.load() == 1u && inArena(K::kthreadLookup(hd)),
          "T4a fresh thread: NtDuplicateObject(-2) -> a new handle with an anonymous KTHREAD");
    check(r.ret == 1u && r.code == kStillActive,
          "T4b another thread's GetExitCodeThread on it = 0x103 (the main thread's handle-1 case)");
    finishSide(a, th);
  }

  // ── T4c: a closed DUPLICATE never drops a live thread's KTHREAD ────────────
  {
    const uint32_t h4 = createHandleOnly();
    SideArgs a; a.bindHandle = h4; a.mode = 5;
    HANDLE th = nullptr;
    const bool ok = runSide(a, th);
    const ExitRes r1 = getExitCode(h4, 0u);
    std::printf("       T4c: dup=%u (own=%u) opens dup=%u afterClose=%u; after dup-close code=%08X\n",
                a.dupHandle.load(), h4, a.opensAfterDup.load(), a.opensAfterClose.load(), r1.code);
    check(ok && a.dupHandle.load() == h4 && a.opensAfterDup.load() == 2u && a.opensAfterClose.load() == 1u,
          "T4c-a -2 duplicate of a handled thread = one more open of the SAME handle");
    check(r1.ret == 1u && r1.code == kStillActive,
          "T4c-b after the duplicate is closed the creator's handle still reads 0x103");
    getExp("NtClose")(h4, 0, 0, 0, 0, 0);   // the creator's close: last open
    const ExitRes r2 = getExitCode(h4, 0u);
    check(K::kthreadOpenCount(h4) == 0u && r2.ret == 0u,
          "T4c-c the last close erases the mapping (GetExitCodeThread fails)");
    finishSide(a, th);
  }
#endif

  // ── T5: after NtClose the handle is invalid and the out-slot is pre-zeroed ─
  {
    const uint32_t h5 = createHandleOnly();
    getExp("NtClose")(h5, 0, 0, 0, 0, 0);
    const ExitRes r = getExitCode(h5, 0xBAADF00Du);
    const uint32_t st = obRef(h5, kTypeThread, 0x66666666u);
    std::printf("       T5: ret=%u slot=%08X ObRef st=%08X obj=%08X\n", r.ret, r.slot, st,
                K::readGuestU32BE(kObjOut));
    check(r.ret == 0u && r.slot == 0u, "T5a closed handle: GetExitCodeThread = FALSE, ObRef out-slot zeroed");
    check(st == K::kObStatusInvalidHandle && K::readGuestU32BE(kObjOut) == 0u,
          "T5b ObRef(closed) = 0xC0000008, *obj = 0");
  }

  // ── T6: type check against the live ExThreadObjectType cell ────────────────
  {
    const uint32_t st = obRef(h1, 0xD0017000u, 0x11111111u);
    check(st == K::kObStatusObjectTypeMismatch && K::readGuestU32BE(kObjOut) == 0u,
          "T6a ObRef(h, VdHSIOCalibrationLock type) = 0xC0000024, *obj = 0");
    const uint32_t st0 = obRef(h1, 0u, 0x11111111u);
    check(st0 == 0u && K::readGuestU32BE(kObjOut) == kt1, "T6b type 0 accepted");
    K::writeGuestBE32(K::kExThreadObjectTypeCell, 0u);   // unresolved cell
    const uint32_t stU = obRef(h1, 0xD0017000u, 0x11111111u);
    check(stU == 0u && K::readGuestU32BE(kObjOut) == kt1, "T6c cell unresolved (0): any type accepted");
    K::writeGuestBE32(K::kExThreadObjectTypeCell, 0x0000ABCDu);   // SR_DATA_IMPORTS=0 raw token
    const ExitRes r = getExitCode(h1, 0u);   // the guest loads the same token itself
    check(r.ret == 1u && r.code == kStillActive, "T6d raw-token cell: the guest's own type still matches");
    K::writeGuestBE32(K::kExThreadObjectTypeCell, kTypeThread);
  }

  // ── T7: unknown handle ─────────────────────────────────────────────────────
  {
    const uint32_t st = obRef(0x7FFFFFF0u, kTypeThread, 0x22222222u);
    check(st == K::kObStatusInvalidHandle && K::readGuestU32BE(kObjOut) == 0u,
          "T7 ObRef(unknown handle) = 0xC0000008, *obj = 0");
  }

  // ── T8: arena exhaustion falls back to guestPhysAlloc (last: moves the cursor)
  {
    K::kthreadArenaCursorForTest(K::kKThreadArenaHi - K::kKThreadStride);
    const uint32_t ha = createHandleOnly();
    const uint32_t hb = createHandleOnly();
    const uint32_t kta = K::kthreadLookup(ha), ktb = K::kthreadLookup(hb);
    const ExitRes r = getExitCode(hb, 0u);
    std::printf("       T8: last arena kt=%08X fallback kt=%08X code=%08X\n", kta, ktb, r.code);
    check(kta == K::kKThreadArenaHi - K::kKThreadStride, "T8a the last arena slot is used");
    check(ktb != 0 && !inArena(ktb) && r.ret == 1u && r.code == kStillActive,
          "T8b exhausted arena: guestPhysAlloc block, still a working KTHREAD");
  }

  std::printf("[OBREF_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
