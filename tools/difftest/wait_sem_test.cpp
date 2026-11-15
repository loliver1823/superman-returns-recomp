// iter-463 PLATFORM FIXES: the wait / semaphore / file-read layer against Xenia
// (xenia-canary src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc, xobject.cc,
// xboxkrnl_io.cc, xfile.cc, vfs/devices/disc_image_file.cc).
//
// Drives the REAL exports through the export registry (NtCreateSemaphore,
// NtReleaseSemaphore, NtWaitForSingleObjectEx, KeInitializeSemaphore,
// KeReleaseSemaphore, KeInitializeEvent, KeSetEvent, KeWaitForSingleObject,
// ExCreateThread -> guestThreadFunc, NtReadFile), so the wiring is tested, not
// just a helper body.
//
// Run: build/difftest.exe --wait-sem-test     (exit = failed checks; ~10 s)
//      build/difftest.exe --atomic-read-test  (exit = failed checks)
// Mutation / attribution (each gate =0 restores the old path; its checks FAIL):
//   SR_SEM_STATUS=0          -> S2 S3 S4 S5
//   SR_WAIT_FULL_TIMEOUT=0   -> T1 T2 T3 T4
//   SR_ALLOW_FORCING=1 SR_NTWAIT_ESCAPE_SCOPE=0 -> T5 (the escape is forcing:
//                               it never runs without the master gate)
//   SR_KESEM_STATUS=0        -> K1 K2 K3 K4 K5
//   SR_READ_PENDING=0        -> P1 P2 (the test sets =1 itself when unset; the
//                               exe default is OFF until its own A/B;
//                               SR_NTSTATUS_DOSERR, which it needs, baked 2026-10-18)
//   SR_WAIT_ALERTABLE=0      -> A1 A3
//   SR_THREAD_WAIT_STATUS=0  -> H1 H2 H3
//   SR_ATOMIC_READ=0         -> R1 (and the EOF checks E1 E3, which need it)
//   SR_READ_EOF_STATUS=0     -> E1 E3 (and P3, which reads at EOF)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/stubs.hpp"
#include "kernel/file_io.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace K = sr::kernel;
namespace fs = std::filesystem;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

uint8_t* g_base = nullptr;
constexpr size_t kGuestSize = 0x100000000ull;

// Scratch guest memory (difftest's flat 4 GB). Above kGuestVABase so
// NtReadFile's real-convention detector accepts the pointers.
constexpr uint32_t kHandleOut = 0x90500000u;
constexpr uint32_t kPrev = 0x90500010u;
constexpr uint32_t kTimeouts = 0x90500100u;   // 8-byte LARGE_INTEGERs, one per slot
constexpr uint32_t kKeSemA = 0x90500200u;      // KSEMAPHORE (0x14 bytes)
constexpr uint32_t kKeEvtA = 0x90500240u;      // KEVENT (0x10 bytes)
constexpr uint32_t kThreadOut = 0x90500300u;
constexpr uint32_t kReadIo = 0x90510000u;      // per-reader: IOSB at +0, offset at +0x10 (0x40 stride)
constexpr uint32_t kReadBuf = 0x90600000u;     // per-reader 128 KB buffers (0x40000 stride)
constexpr uint32_t kStatusTimeout = 0x102u, kStatusUserApc = 0xC0u;
constexpr uint32_t kLimitExceeded = 0xC0000047u, kInvalidHandle = 0xC0000008u;
constexpr uint32_t kEndOfFile = 0xC0000011u;
constexpr uint32_t kInvalidParameter = 0xC000000Du, kPending = 0x103u;
constexpr uint32_t kKeSemK = 0x90500280u;      // KSEMAPHORE for the K checks

K::ExportFn getExp(const char* name) { return K::ExportRegistry::instance().getExport(name); }

struct Ctx {
  PPCContext c{};
  explicit Ctx(uint32_t stack) {
    K::setGuestMemory(g_base, kGuestSize);
    c.r1.u32 = stack;
    c.r14.u32 = 0x829C0000u;
    K::setCurrentGuestContext(&c, offsetof(PPCContext, lr), offsetof(PPCContext, ctr),
                              offsetof(PPCContext, r3), offsetof(PPCContext, r1),
                              offsetof(PPCContext, r31));
  }
  ~Ctx() { K::setCurrentGuestContext(nullptr, 0); }
};

uint32_t call(const char* name, uint32_t a3, uint32_t a4 = 0, uint32_t a5 = 0, uint32_t a6 = 0,
              uint32_t a7 = 0, uint32_t a8 = 0, uint32_t a9 = 0, uint32_t a10 = 0) {
  K::ExportRegistry::setCurrentExtraArgs(a9, a10);
  return getExp(name)(a3, a4, a5, a6, a7, a8);
}

// A relative guest timeout (negative 100 ns units) in slot i; returns its VA.
uint32_t relTimeout(int slot, int64_t ms) {
  const uint32_t va = kTimeouts + uint32_t(slot) * 8u;
  const int64_t t = -ms * 10000;
  K::writeGuestBE32(va, uint32_t(uint64_t(t) >> 32));
  K::writeGuestBE32(va + 4, uint32_t(uint64_t(t)));
  return va;
}

uint32_t ntCreateSem(int32_t init, int32_t max) {
  K::writeGuestBE32(kHandleOut, 0);
  call("NtCreateSemaphore", kHandleOut, 0, uint32_t(init), uint32_t(max));
  return K::readGuestU32BE(kHandleOut);
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

#ifdef _WIN32
std::atomic<int> g_apcRan{0};
void CALLBACK countApc(ULONG_PTR) { g_apcRan.fetch_add(1); }

// ── thread-handle wait (H): a guest thread through ExCreateThread's runner ──
HANDLE g_runnerGo = nullptr;
std::atomic<bool> g_runnerStarted{false};
void testRunner(uint32_t, uint32_t, uint32_t, uint8_t* base, size_t size) {
  K::setGuestMemory(base, size);
  g_runnerStarted.store(true);
  WaitForSingleObject(g_runnerGo, 10000);
}
#endif
}  // namespace

int runWaitSemTest() {
  std::printf("[WAIT_SEM_TEST] wait / semaphore status vs Xenia (iter-463 platform fixes)\n");
  g_base = K::getGuestBase();
  K::registerStubs();
#ifdef _WIN32
  K::registerExCreateThreadRunner(testRunner, g_base, kGuestSize);
  Ctx mainCtx(0x90100000u);

  // ── S: NtReleaseSemaphore status codes (xboxkrnl_threading.cc:807-832) ────
  {
    const uint32_t h = ntCreateSem(0, 2);
    check(h != 0, "S0 NtCreateSemaphore(init 0, max 2) -> handle");
    K::writeGuestBE32(kPrev, 0xCCCCCCCCu);
    uint32_t st = call("NtReleaseSemaphore", h, 1, kPrev);
    std::printf("       S1: st=%08X prev=%u\n", st, K::readGuestU32BE(kPrev));
    check(st == 0 && K::readGuestU32BE(kPrev) == 0u, "S1 release 1 of max 2: SUCCESS, prev 0");
    K::writeGuestBE32(kPrev, 0xCCCCCCCCu);
    st = call("NtReleaseSemaphore", h, 2, kPrev);
    std::printf("       S2: st=%08X prev=%08X\n", st, K::readGuestU32BE(kPrev));
    check(st == kLimitExceeded, "S2 count 1 + 2 > max 2: STATUS_SEMAPHORE_LIMIT_EXCEEDED");
    st = call("NtReleaseSemaphore", h, 0, kPrev);
    std::printf("       S3: st=%08X\n", st);
    check(st == kLimitExceeded, "S3 release count 0 is refused (not coerced to 1)");
    K::writeGuestBE32(kPrev, 0xCCCCCCCCu);
    st = call("NtReleaseSemaphore", h, 1, kPrev);
    std::printf("       S4: st=%08X prev=%u\n", st, K::readGuestU32BE(kPrev));
    check(st == 0 && K::readGuestU32BE(kPrev) == 1u,
          "S4 the failed releases changed nothing: release 1 -> SUCCESS, prev 1");
    st = call("NtReleaseSemaphore", 0x0007FFF0u, 1, kPrev);
    std::printf("       S5: st=%08X\n", st);
    check(st == kInvalidHandle, "S5 unknown handle: STATUS_INVALID_HANDLE");
    call("NtClose", h);
  }

  // ── K: KeReleaseSemaphore / NtCreateSemaphore (xboxkrnl_threading.cc:743-792) ──
  {
    call("KeInitializeSemaphore", kKeSemK, 0, 4);
    uint32_t r1 = call("KeReleaseSemaphore", kKeSemK, 0, 1, 0);
    uint32_t r2 = call("KeReleaseSemaphore", kKeSemK, 0, 2, 0);
    std::printf("       K1: prev=%u then %u\n", r1, r2);
    check(r1 == 0 && r2 == 1, "K1 KeReleaseSemaphore returns the previous count (0, then 1)");
    // count is 3 of limit 4: adjustment 0 and adjustment 2 (past the limit) both add nothing.
    const uint32_t r3 = call("KeReleaseSemaphore", kKeSemK, 0, 0, 0);
    const uint32_t r4 = call("KeReleaseSemaphore", kKeSemK, 0, 2, 0);
    int permits = 0;
    while (permits < 8 &&
           call("KeWaitForSingleObject", kKeSemK, 0, 0, 0, relTimeout(6, 0)) == 0) ++permits;
    std::printf("       K2/K3: adj0 ret=%u adj2 ret=%u permits=%d\n", r3, r4, permits);
    check(r3 == 0 && permits == 3, "K2 adjustment 0 adds no permit (not coerced to 1)");
    check(r4 == 0 && permits == 3, "K3 adjustment past the limit fails and adds nothing");
    K::writeGuestBE32(kHandleOut, 0xCCCCCCCCu);
    uint32_t st = call("NtCreateSemaphore", kHandleOut, 0, 0, 0);
    std::printf("       K4: st=%08X handle=%08X\n", st, K::readGuestU32BE(kHandleOut));
    check(st == kInvalidParameter && K::readGuestU32BE(kHandleOut) == 0,
          "K4 NtCreateSemaphore(limit 0): INVALID_PARAMETER, handle 0 (not coerced to 1024)");
    st = call("NtCreateSemaphore", kHandleOut, 0, 5, 2);
    std::printf("       K5: st=%08X\n", st);
    check(st == kInvalidParameter, "K5 NtCreateSemaphore(count 5 > limit 2): INVALID_PARAMETER");
  }

  // ── A: alertable vs non-alertable (xobject.cc:204-219) ───────────────────
  {
    const uint32_t h = ntCreateSem(0, 4);
    g_apcRan.store(0);
    QueueUserAPC(countApc, GetCurrentThread(), 0);
    uint32_t st = call("NtWaitForSingleObjectEx", h, 1, 0 /*non-alertable*/, relTimeout(0, 200));
    const int ranDuring = g_apcRan.load();
    SleepEx(0, TRUE);   // drain the still-pending APC
    std::printf("       A1: st=%08X apcRanDuringWait=%d\n", st, ranDuring);
    check(st == kStatusTimeout && ranDuring == 0,
          "A1 non-alertable timed wait with a pending host APC: TIMEOUT, APC not run");
    g_apcRan.store(0);
    QueueUserAPC(countApc, GetCurrentThread(), 0);
    st = call("NtWaitForSingleObjectEx", h, 1, 1 /*alertable*/, relTimeout(0, 2000));
    std::printf("       A2: st=%08X apcRan=%d\n", st, g_apcRan.load());
    check(st == kStatusUserApc && g_apcRan.load() == 1, "A2 alertable timed wait: STATUS_USER_APC");
    // INFINITE non-alertable wait with a pending APC: blocks until the release.
    g_apcRan.store(0);
    QueueUserAPC(countApc, GetCurrentThread(), 0);
    std::thread rel([h] {
      Ctx c(0x90180000u);
      Sleep(400);
      call("NtReleaseSemaphore", h, 1, 0);
    });
    const auto t0 = std::chrono::steady_clock::now();
    st = call("NtWaitForSingleObjectEx", h, 1, 0, 0 /*INFINITE*/);
    const double el = secondsSince(t0);
    rel.join();
    const int ranDuring3 = g_apcRan.load();
    SleepEx(0, TRUE);
    std::printf("       A3: st=%08X elapsed=%.3fs apcRanDuringWait=%d\n", st, el, ranDuring3);
    check(st == 0 && el > 0.3 && ranDuring3 == 0,
          "A3 non-alertable INFINITE wait with a pending APC: SUCCESS on the release, never USER_APC");
    call("NtClose", h);
  }

  // ── H: thread-handle waits (xobject.cc:207-216) ────────────────────────────
  {
    g_runnerGo = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    K::writeGuestBE32(kThreadOut, 0);
    call("ExCreateThread", kThreadOut, 0, 0, 0, 0x82469E98u, 0x2222u);
    const uint32_t th = K::readGuestU32BE(kThreadOut);
    for (int i = 0; i < 1000 && !g_runnerStarted.load(); i++) Sleep(5);
    auto t0 = std::chrono::steady_clock::now();
    uint32_t st = call("NtWaitForSingleObjectEx", th, 1, 0, relTimeout(1, 300));
    double el = secondsSince(t0);
    std::printf("       H1: running thread h=%u st=%08X elapsed=%.3fs\n", th, st, el);
    check(th != 0 && st == kStatusTimeout && el < 1.5,
          "H1 running thread, 300 ms timeout: TIMEOUT after ~300 ms (not 5 s)");
    SetEvent(g_runnerGo);
    st = call("NtWaitForSingleObjectEx", th, 1, 0, relTimeout(1, 3000));
    std::printf("       H2: exited thread st=%08X\n", st);
    check(st == 0, "H2 wait for the exit: STATUS_SUCCESS (not USER_APC 0xC0)");
    t0 = std::chrono::steady_clock::now();
    st = call("NtWaitForSingleObjectEx", th, 1, 0, relTimeout(1, 300));
    el = secondsSince(t0);
    std::printf("       H3: second wait st=%08X elapsed=%.3fs\n", st, el);
    check(st == 0 && el < 0.2, "H3 an exited thread stays signalled: a second waiter gets SUCCESS at once");
    CloseHandle(g_runnerGo);
  }

  // ── T: timeouts past 5 s, and the 8 s escape (xobject.cc:199-205) ──────────
  {
    const uint32_t semA = ntCreateSem(0, 4), semC = ntCreateSem(0, 4), semD = ntCreateSem(0, 4);
    call("KeInitializeSemaphore", kKeSemA, 0, 4);
    call("KeInitializeEvent", kKeEvtA, 1 /*synchronization*/, 0);
    struct Res { std::atomic<uint32_t> st{0xFFFFFFFFu}; std::atomic<double> el{0}; };
    Res rA, rB, rC, rD, rE;
    const auto t0 = std::chrono::steady_clock::now();
    auto waiter = [&](Res& r, int stackSlot, auto fn) {
      return std::thread([&r, stackSlot, fn, t0] {
        Ctx c(0x90200000u + uint32_t(stackSlot) * 0x10000u);
        const uint32_t st = fn();
        r.el.store(secondsSince(t0));
        r.st.store(st);
      });
    };
    std::vector<std::thread> ws;
    ws.push_back(waiter(rA, 0, [&] { return call("NtWaitForSingleObjectEx", semA, 1, 0, relTimeout(2, 5800)); }));
    ws.push_back(waiter(rB, 1, [&] { return call("KeWaitForSingleObject", kKeSemA, 0, 0, 0, relTimeout(3, 5800)); }));
    ws.push_back(waiter(rC, 2, [&] { return call("NtWaitForSingleObjectEx", semC, 1, 0, relTimeout(4, 6000)); }));
    ws.push_back(waiter(rD, 3, [&] { return call("NtWaitForSingleObjectEx", semD, 1, 0, 0 /*INFINITE*/); }));
    ws.push_back(waiter(rE, 4, [&] { return call("KeWaitForSingleObject", kKeEvtA, 0, 0, 0, relTimeout(5, 5800)); }));
    std::this_thread::sleep_until(t0 + std::chrono::milliseconds(5400));
    call("NtReleaseSemaphore", semA, 1, 0);
    call("KeReleaseSemaphore", kKeSemA, 0, 1, 0);
    call("KeSetEvent", kKeEvtA, 0, 0);
    std::this_thread::sleep_until(t0 + std::chrono::milliseconds(9000));
    call("NtReleaseSemaphore", semD, 1, 0);
    for (auto& w : ws) w.join();
    std::printf("       T1 Nt  sem  -5.8s, released @5.4s: st=%08X at %.2fs\n", rA.st.load(), rA.el.load());
    std::printf("       T2 Ke  sem  -5.8s, released @5.4s: st=%08X at %.2fs\n", rB.st.load(), rB.el.load());
    std::printf("       T3 Nt  sem  -6.0s, never released: st=%08X at %.2fs\n", rC.st.load(), rC.el.load());
    std::printf("       T4 Ke  evt  -5.8s, set @5.4s:      st=%08X at %.2fs\n", rE.st.load(), rE.el.load());
    std::printf("       T5 Nt  sem  INFINITE on a non-main thread, released @9.0s: st=%08X at %.2fs\n",
                rD.st.load(), rD.el.load());
    check(rA.st.load() == 0 && rA.el.load() > 5.3, "T1 NtWait -5.8 s: SUCCESS on the 5.4 s release (no 5 s cap)");
    check(rB.st.load() == 0 && rB.el.load() > 5.3, "T2 KeWait (semaphore) -5.8 s: SUCCESS on the 5.4 s release");
    check(rC.st.load() == kStatusTimeout && rC.el.load() > 5.9, "T3 NtWait -6.0 s unsignalled: TIMEOUT at >= 5.9 s");
    check(rE.st.load() == 0 && rE.el.load() > 5.3, "T4 KeWait (guest-truth event) -5.8 s: SUCCESS on the 5.4 s set");
    check(rD.st.load() == 0 && rD.el.load() > 8.9,
          "T5 INFINITE wait on a non-main thread: no 8 s USER_APC escape, SUCCESS on the 9 s release");
    call("NtClose", semA); call("NtClose", semC); call("NtClose", semD);
  }
#else
  std::printf("  (Windows only)\n");
#endif
  std::printf("[WAIT_SEM_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// ── R / E: NtReadFile positioned reads + EOF (xfile.cc:102-190) ───────────────
int runAtomicReadTest() {
  std::printf("[ATOMIC_READ_TEST] NtReadFile positioned reads + EOF vs Xenia (iter-463)\n");
  // SR_READ_PENDING is default OFF in the exe (it needs SR_NTSTATUS_DOSERR); the
  // P checks test the behaviour itself, so turn it on unless the caller set it.
  if (!std::getenv("SR_READ_PENDING")) _putenv("SR_READ_PENDING=1");
  // iter-466: SR_ASYNC_READ (default ON) completes a non-synchronous read on the worker; these
  // checks are the SYNCHRONOUS contract (P1 reads the IOSB at return), --async-read-test covers it.
  if (!std::getenv("SR_ASYNC_READ")) _putenv("SR_ASYNC_READ=0");
  g_base = K::getGuestBase();
  K::registerStubs();
#ifdef _WIN32
  Ctx mainCtx(0x90100000u);
  std::error_code ec;
  const fs::path file = fs::temp_directory_path(ec) /
                        ("sr_atomic_read_" + std::to_string(GetCurrentProcessId()) + ".bin");
  constexpr uint32_t kFileSize = 8u << 20;   // 8 MB of self-describing words
  {
    std::vector<uint8_t> data(kFileSize);
    for (uint32_t o = 0; o < kFileSize; o += 4) {
      data[o] = uint8_t(o >> 24); data[o + 1] = uint8_t(o >> 16);
      data[o + 2] = uint8_t(o >> 8); data[o + 3] = uint8_t(o);
    }
    std::FILE* w = _wfopen(file.c_str(), L"wb");
    if (w) { std::fwrite(data.data(), 1, data.size(), w); std::fclose(w); }
  }
  std::FILE* f = _wfopen(file.c_str(), L"rb");
  const uint32_t h = f ? K::fileHandleAlloc(f) : 0;
  check(h != 0, "R0 the 8 MB pattern file is open as a kernel file handle");

  auto readAt = [](uint32_t handle, int slot, uint32_t len, bool positioned, uint64_t off,
                   uint32_t* outInfo, uint32_t* outIosbSt) -> uint32_t {
    const uint32_t io = kReadIo + uint32_t(slot) * 0x40u;
    const uint32_t buf = kReadBuf + uint32_t(slot) * 0x40000u;
    K::writeGuestBE32(io + 0, 0xCCCCCCCCu);
    K::writeGuestBE32(io + 4, 0xCCCCCCCCu);
    K::writeGuestBE32(io + 0x10, uint32_t(off >> 32));
    K::writeGuestBE32(io + 0x14, uint32_t(off));
    const uint32_t st = call("NtReadFile", handle, 0, 0, 0, io, buf, len, positioned ? io + 0x10 : 0);
    if (outIosbSt) *outIosbSt = K::readGuestU32BE(io + 0);
    if (outInfo) *outInfo = K::readGuestU32BE(io + 4);
    return st;
  };
  auto verify = [](int slot, uint64_t off, uint32_t n) -> bool {
    const uint32_t buf = kReadBuf + uint32_t(slot) * 0x40000u;
    for (uint32_t i = 0; i < n; i += 4)
      if (K::readGuestU32BE(buf + i) != uint32_t(off + i)) return false;
    return true;
  };

  // R1: 4 threads x 1500 positioned 64 KB reads on ONE handle.
  {
    constexpr int kThreads = 4, kReads = 1500;
    constexpr uint32_t kLen = 64u << 10;
    std::atomic<int> bad{0}, badStatus{0}, done{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
      ts.emplace_back([&, t] {
        Ctx c(0x90200000u + uint32_t(t) * 0x10000u);
        std::mt19937 rng(1234u + unsigned(t));
        for (int i = 0; i < kReads; ++i) {
          const uint64_t off = (uint64_t(rng()) % ((kFileSize - kLen) / 4)) * 4;
          uint32_t info = 0, iost = 0;
          const uint32_t st = readAt(h, t, kLen, true, off, &info, &iost);
          if (st != 0 || iost != 0 || info != kLen) badStatus.fetch_add(1);
          else if (!verify(t, off, kLen)) bad.fetch_add(1);
          done.fetch_add(1);
        }
      });
    }
    for (auto& t : ts) t.join();
    std::printf("       R1: %d reads, %d wrong-bytes, %d wrong-status\n", done.load(), bad.load(), badStatus.load());
    check(done.load() == kThreads * kReads && bad.load() == 0 && badStatus.load() == 0,
          "R1 4 threads x 1500 concurrent 64 KB positioned reads: every buffer = the file's bytes at its own offset");
  }
  // R2: a positioned read leaves the position at offset + n (xfile.cc:176-179).
  {
    uint32_t info = 0, iost = 0;
    uint32_t st1 = readAt(h, 0, 16, true, 1000, &info, &iost);
    uint32_t st2 = readAt(h, 1, 16, false, 0, &info, &iost);
    std::printf("       R2: st=%08X/%08X seq info=%u first=%08X\n", st1, st2, info,
                K::readGuestU32BE(kReadBuf + 0x40000u));
    check(st1 == 0 && st2 == 0 && info == 16 && verify(1, 1016, 16),
          "R2 a sequential read after a positioned read continues at offset + n");
  }
  // E: end of file (disc_image_file.cc:26-27; xboxkrnl_io.cc:151-153, 205-208).
  {
    uint32_t info = 0xFFFF, iost = 0;
    uint32_t st = readAt(h, 0, 64, true, kFileSize, &info, &iost);
    std::printf("       E1: st=%08X iosb=%08X info=%u\n", st, iost, info);
    check(st == kEndOfFile && iost == kEndOfFile && info == 0,
          "E1 read at offset == size: STATUS_END_OF_FILE, IOSB {END_OF_FILE, 0}");
    st = readAt(h, 0, 64, true, kFileSize - 8, &info, &iost);
    std::printf("       E2: st=%08X iosb=%08X info=%u\n", st, iost, info);
    check(st == 0 && iost == 0 && info == 8 && verify(0, kFileSize - 8, 8),
          "E2 read straddling the end: SUCCESS with the 8 remaining bytes");
    st = readAt(h, 0, 64, false, 0, &info, &iost);
    std::printf("       E3: st=%08X iosb=%08X info=%u\n", st, iost, info);
    check(st == kEndOfFile && iost == kEndOfFile && info == 0,
          "E3 sequential read at the end (position = size): STATUS_END_OF_FILE");
    st = readAt(h, 0, 0, true, kFileSize, &info, &iost);
    std::printf("       E4: st=%08X info=%u\n", st, info);
    check(st == 0 && info == 0, "E4 zero-length read at the end: SUCCESS (xfile.cc:121-123)");
  }
  // P: a handle opened WITHOUT FILE_SYNCHRONOUS_IO_* (xboxkrnl_io.cc:94-96, 167-169):
  // STATUS_PENDING with the IOSB holding the real status + count, the file object
  // is waitable (async_event_ set by the read), and END_OF_FILE is not PENDING.
  {
    std::FILE* fa = _wfopen(file.c_str(), L"rb");
    const uint32_t ha = fa ? K::fileHandleAlloc(fa) : 0;
    K::fileHandleSetSynchronous(ha, false);
    uint32_t info = 0, iost = 0;
    uint32_t st = readAt(ha, 2, 4096, true, 8192, &info, &iost);
    std::printf("       P1: st=%08X iosb=%08X info=%u\n", st, iost, info);
    check(ha != 0 && st == kPending && iost == 0 && info == 4096 && verify(2, 8192, 4096),
          "P1 non-synchronous handle: STATUS_PENDING, IOSB {SUCCESS, 4096}, bytes delivered");
    const uint32_t wst = call("NtWaitForSingleObjectEx", ha, 1, 0, relTimeout(7, 500));
    std::printf("       P2: wait on the file handle st=%08X\n", wst);
    check(wst == 0, "P2 the file object is signalled after the read: NtWait(file) -> SUCCESS");
    st = readAt(ha, 2, 64, true, kFileSize, &info, &iost);
    std::printf("       P3: st=%08X iosb=%08X\n", st, iost);
    check(st == kEndOfFile && iost == kEndOfFile, "P3 END_OF_FILE on a non-synchronous handle is returned, not PENDING");
    call("NtClose", ha);
    std::FILE* fb = _wfopen(file.c_str(), L"rb");
    const uint32_t hb = fb ? K::fileHandleAlloc(fb) : 0;
    K::fileHandleSetSynchronous(hb, true);
    st = readAt(hb, 2, 64, true, 0, &info, &iost);
    check(st == 0 && info == 64, "P4 synchronous handle: STATUS_SUCCESS");
    call("NtClose", hb);
  }
  // C: close waits for the handle's I/O lock; a read on the closed handle is INVALID_HANDLE.
  {
    call("NtClose", h);
    uint32_t info = 0, iost = 0;
    const uint32_t st = readAt(h, 0, 16, true, 0, &info, &iost);
    check(st == kInvalidHandle, "C1 read after NtClose: STATUS_INVALID_HANDLE");
  }
  fs::remove(file, ec);
#else
  std::printf("  (Windows only)\n");
#endif
  std::printf("[ATOMIC_READ_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// ── A: iter-466 stutter lane, SR_ASYNC_READ - an overlapped read completes asynchronously ──
// Run: build/difftest.exe --async-read-test (exit = failed checks). Mutation: SR_ASYNC_READ=0
// -> A1 A3 A4 fail (the read completes on the calling thread, no PENDING window).
int runAsyncReadTest() {
  std::printf("[ASYNC_READ_TEST] overlapped NtReadFile completes on the async worker (iter-466)\n");
  if (!std::getenv("SR_ASYNC_READ")) _putenv("SR_ASYNC_READ=1");
  g_base = K::getGuestBase();
  K::registerStubs();
#ifdef _WIN32
  Ctx mainCtx(0x90100000u);
  std::error_code ec;
  const fs::path file = fs::temp_directory_path(ec) /
                        ("sr_async_read_" + std::to_string(GetCurrentProcessId()) + ".bin");
  constexpr uint32_t kFileSize = 4u << 20;
  {
    std::vector<uint8_t> data(kFileSize);
    for (uint32_t o = 0; o < kFileSize; o += 4) {
      data[o] = uint8_t(o >> 24); data[o + 1] = uint8_t(o >> 16);
      data[o + 2] = uint8_t(o >> 8); data[o + 3] = uint8_t(o);
    }
    std::FILE* w = _wfopen(file.c_str(), L"wb");
    if (w) { std::fwrite(data.data(), 1, data.size(), w); std::fclose(w); }
  }
  std::FILE* fa = _wfopen(file.c_str(), L"rb");
  const uint32_t h = fa ? K::fileHandleAlloc(fa) : 0;
  K::fileHandleSetSynchronous(h, false);
  check(h != 0, "A0 the pattern file is open as a non-synchronous (overlapped) handle");
  auto newEvent = [] {
    K::writeGuestBE32(kHandleOut, 0);
    call("NtCreateEvent", kHandleOut, 0, 0, 0);
    return K::readGuestU32BE(kHandleOut);
  };
  auto issue = [](uint32_t handle, uint32_t ev, int slot, uint32_t len, uint64_t off) -> uint32_t {
    const uint32_t io = kReadIo + uint32_t(slot) * 0x40u;
    const uint32_t buf = kReadBuf + uint32_t(slot) * 0x40000u;
    K::writeGuestBE32(io + 0, 0xCCCCCCCCu);
    K::writeGuestBE32(io + 4, 0xCCCCCCCCu);
    K::writeGuestBE32(io + 0x10, uint32_t(off >> 32));
    K::writeGuestBE32(io + 0x14, uint32_t(off));
    return call("NtReadFile", handle, ev, 0, 0, io, buf, len, io + 0x10);
  };
  auto iosb = [](int slot, int word) { return K::readGuestU32BE(kReadIo + uint32_t(slot) * 0x40u + 4u * word); };
  auto verify = [](int slot, uint64_t off, uint32_t n) -> bool {
    const uint32_t buf = kReadBuf + uint32_t(slot) * 0x40000u;
    for (uint32_t i = 0; i < n; i += 4)
      if (K::readGuestU32BE(buf + i) != uint32_t(off + i)) return false;
    return true;
  };
  // A1/A2: with the handle's I/O lock held the worker cannot finish: the caller already has
  // STATUS_PENDING, the IOSB says PENDING and the event is NOT signalled; after the release
  // the event is set and the IOSB holds {SUCCESS, len} with the file's bytes.
  {
    const uint32_t ev = newEvent();
    std::shared_ptr<std::mutex> io = K::fileHandleIoLock(h);
    uint32_t st = 0, iost = 0, w0 = 0;
    {
      // Issued from a second thread: with the gate off the read blocks on the held lock (the
      // mutation must fail A1, not deadlock the test).
      std::unique_lock<std::mutex> hold(*io);
      std::atomic<bool> returned{false};
      std::atomic<uint32_t> ast{0xFFFFFFFFu};
      std::thread issuer([&] {
        Ctx c(0x90300000u);
        ast.store(issue(h, ev, 0, 64u << 10, 128u << 10));
        returned.store(true);
      });
      for (int i = 0; i < 40 && !returned.load(); ++i) Sleep(5);
      st = returned.load() ? ast.load() : 0xFFFFFFFFu;  // still blocked = not asynchronous
      iost = iosb(0, 0);
      w0 = call("NtWaitForSingleObjectEx", ev, 1, 0, relTimeout(0, 50));
      hold.unlock();
      issuer.join();
    }
    const uint32_t w1 = call("NtWaitForSingleObjectEx", ev, 1, 0, relTimeout(1, 2000));
    std::printf("       A1: st=%08X iosb@submit=%08X wait(stalled)=%08X wait=%08X iosb={%08X,%u}\n", st,
                iost, w0, w1, iosb(0, 0), iosb(0, 1));
    check(st == kPending && iost == kPending && w0 == kStatusTimeout,
          "A1 the caller returns STATUS_PENDING before the read is done (IOSB PENDING, event clear)");
    check(w1 == 0 && iosb(0, 0) == 0 && iosb(0, 1) == (64u << 10) && verify(0, 128u << 10, 64u << 10),
          "A2 completion: event set, IOSB {SUCCESS, 65536}, the buffer holds the file's bytes");
    call("NtClose", ev);
  }
  // A3: 4 threads x 200 overlapped 32 KB reads, each waits on its own event: every one PENDING,
  // completed with the right bytes (FIFO worker).
  {
    std::atomic<int> bad{0}, notPending{0}, done{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
      ts.emplace_back([&, t] {
        Ctx c(0x90200000u + uint32_t(t) * 0x10000u);
        K::writeGuestBE32(kHandleOut + 0x40u + 4u * uint32_t(t), 0);
        call("NtCreateEvent", kHandleOut + 0x40u + 4u * uint32_t(t), 0, 0, 0);
        const uint32_t ev = K::readGuestU32BE(kHandleOut + 0x40u + 4u * uint32_t(t));
        std::mt19937 rng(77u + unsigned(t));
        for (int i = 0; i < 200; ++i) {
          const uint64_t off = (uint64_t(rng()) % ((kFileSize - (32u << 10)) / 4)) * 4;
          const uint32_t st = issue(h, ev, 1 + t, 32u << 10, off);
          if (st != kPending) notPending.fetch_add(1);
          const uint32_t w = call("NtWaitForSingleObjectEx", ev, 1, 0, relTimeout(2 + t, 5000));
          if (w != 0 || iosb(1 + t, 0) != 0 || iosb(1 + t, 1) != (32u << 10) || !verify(1 + t, off, 32u << 10))
            bad.fetch_add(1);
          done.fetch_add(1);
        }
        call("NtClose", ev);
      });
    }
    for (auto& t : ts) t.join();
    std::printf("       A3: %d reads, %d not pending, %d wrong\n", done.load(), notPending.load(), bad.load());
    check(done.load() == 800 && notPending.load() == 0 && bad.load() == 0,
          "A3 4 threads x 200 overlapped 32 KB reads: all PENDING, all complete with their own bytes");
  }
  // A4: the file object is signalled by the completion (no event passed).
  {
    const uint32_t st = issue(h, 0, 0, 4096, 0);
    const uint32_t w = call("NtWaitForSingleObjectEx", h, 1, 0, relTimeout(6, 2000));
    std::printf("       A4: st=%08X wait(file)=%08X iosb={%08X,%u}\n", st, w, iosb(0, 0), iosb(0, 1));
    check(w == 0 && iosb(0, 0) == 0 && iosb(0, 1) == 4096 && verify(0, 0, 4096),
          "A4 no event: the file object is signalled when the read completes");
  }
  // A5: a read at the end is answered at once with END_OF_FILE (not PENDING).
  {
    const uint32_t st = issue(h, 0, 0, 64, kFileSize);
    std::printf("       A5: st=%08X iosb=%08X\n", st, iosb(0, 0));
    check(st == kEndOfFile && iosb(0, 0) == kEndOfFile, "A5 at the end: END_OF_FILE returned, not PENDING");
  }
  // A6: a synchronous handle is unchanged.
  {
    std::FILE* fb = _wfopen(file.c_str(), L"rb");
    const uint32_t hb = fb ? K::fileHandleAlloc(fb) : 0;
    K::fileHandleSetSynchronous(hb, true);
    const uint32_t st = issue(hb, 0, 0, 64, 4);
    check(st == 0 && iosb(0, 1) == 64 && verify(0, 4, 64), "A6 synchronous handle: STATUS_SUCCESS at once");
    call("NtClose", hb);
  }
  call("NtClose", h);
  fs::remove(file, ec);
#else
  std::printf("  (Windows only)\n");
#endif
  std::printf("[ASYNC_READ_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
