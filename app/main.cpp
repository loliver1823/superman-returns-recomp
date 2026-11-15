#include <SDL3/SDL.h>
// SDL3 has no SDLmain library: SDL_main.h supplies main / WinMain (header-only, this TU only), which
// call SDL_main below through SDL_RunApp - what SDL2main.lib did.
#include <SDL3/SDL_main.h>

#include "crash_handler.hpp"
#include "heap_sentinel.hpp"
#include "log.hpp"
#include "platform_sdl.hpp"
#include "stdio_guard.hpp"         // iter-465 player: no thread blocks on a stdio sink
#include "kernel/devtools.hpp"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/harness.hpp"
#include "kernel/host_automation.hpp"     // iter-464 bgruns: background window + harness keys
#include "kernel/quit.hpp"                // tools/graceful-quit: the harness's quit request + terminal snapshot
#include "kernel/input_recorder.hpp"
#include "kernel/mod_menu.hpp"         // iter-464 modmenu: RESTART NOW relaunch on exit
#include "kernel/xam_content.hpp"
#include "kernel/player_instance_rule.hpp"  // shared host launch policy
#include "kernel/sr_oracle.hpp"
#include "guest/guest_harness.hpp"
#include "gfx/d3d12_backend.hpp"
#if SR_ORACLE
#include "gfx/gpu_trace_replay.hpp"
#include "gfx/hle_video.hpp"
#endif
#include "gfx/null_renderer.hpp"
#include "gfx/present_dump_writer.hpp"   // step 4 4(c) / Astra P2 3: the dump drain on the quick_exit path
#include "gfx/pso_warm.hpp"              // step 4 2k / Astra P2 1, 3: the warm-up join + manifest write at exit
#include "gfx/res_scale.hpp"
#include "gfx/shader_cache.hpp"   // render/boot-attach: the replays wait for the load thread before exiting
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if SR_ORACLE
// iter-455j: SR_H7_ENTRYWATCH host-side self-test entry (kernel/apt_h7_entrywatch.cpp).
namespace sr { namespace kernel { namespace h7 { int runSelfTestIfRequested(); } } }
namespace sr { namespace kernel { namespace isolation_test { int runSelfTestIfRequested(); } } }
namespace sr { namespace gfx { namespace prov { int runSelfTestIfRequested(); } } }
namespace sr { namespace wffsm { int runSelfTestIfRequested(); } }
// iter-455n: the texture Unlock-upload contract self-test (kernel/hle_api_observe.cpp).
namespace sr { namespace kernel { namespace texunlock {
int runSelfTestIfRequested();
int runGateOffSelfTestIfRequested();
} } }
namespace sr { namespace kernel { namespace texid { int runSelfTestIfRequested(); } } }
#endif

#include "sr_version.h"  // iter-465 window: SR_PORT_GIT_HASH (generated each build)
// iter-465 window: the window title ("Superman Returns - SR Recomp <build>" with an em dash, UTF-8).
static constexpr const char kPortWindowTitle[] = "Superman Returns — SR Recomp " SR_PORT_GIT_HASH;

#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>  // iter-465 perf2: __cpuid (the SSSE3 check in SDL_main)
#include <timeapi.h>
#include "kernel/perf_stats.hpp"
#include <tlhelp32.h>

// iter-465 polish: ONE PLAYER INSTANCE, and a player launch never kills anything.
// Until iter-465 a launch without SR_INSTANCE_ID terminated every other SupermanReturns.exe
// on the machine (a dev-era "zombie killer"), including a lane's background roll and a
// player's own running game. Now a player launch claims a per-session named mutex; when
// another PLAYER instance already holds it, the new launch brings that instance's window to
// the front and exits (the standard "already running" behaviour of a PC game). Automated
// rolls (SR_INSTANCE_ID) never take part, so a roll and a player game run side by side.
// The RESTART NOW child (kernel/mod_menu.cpp, SR_RESTART_PARENT_PID) waits for its parent to
// exit and then takes the mutex over. This runs before the log opens: a second launch must
// not truncate the running instance's superman_returns.log.
namespace {
constexpr wchar_t kPlayerMutexName[] = L"Local\\SupermanReturnsRecomp.PlayerInstance";
constexpr wchar_t kPlayerPidMapName[] = L"Local\\SupermanReturnsRecomp.PlayerInstancePid";
HANDLE g_playerMutex = nullptr;   // held for the process's lifetime (released by the OS)
HANDLE g_playerPidMap = nullptr;

void publishPlayerPid() {
  g_playerPidMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096,
                                      kPlayerPidMapName);
  if (!g_playerPidMap) return;
  if (void* v = MapViewOfFile(g_playerPidMap, FILE_MAP_WRITE, 0, 0, 4)) {
    *static_cast<volatile DWORD*>(v) = GetCurrentProcessId();
    UnmapViewOfFile(v);
  }
}

DWORD readPlayerPid() {
  HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, kPlayerPidMapName);
  if (!m) return 0;
  DWORD pid = 0;
  if (void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 4)) {
    pid = *static_cast<volatile DWORD*>(v);
    UnmapViewOfFile(v);
  }
  CloseHandle(m);
  return pid;
}

struct WinOfPid {
  DWORD pid;
  HWND hwnd;
};
BOOL CALLBACK findTopWindowOfPid(HWND h, LPARAM lp) {
  auto* w = reinterpret_cast<WinOfPid*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != w->pid || GetWindow(h, GW_OWNER) || !IsWindowVisible(h)) return TRUE;
  w->hwnd = h;
  return FALSE;
}
}  // namespace

// true = this process is the player instance (continue); false = another player instance
// owns the session and was brought to the front (this launch exits).
static bool claimPlayerInstance(bool backgroundLaunch) {
  g_playerMutex = CreateMutexW(nullptr, TRUE, kPlayerMutexName);
  if (!g_playerMutex) return true;   // cannot tell: never block the player's launch
  if (GetLastError() != ERROR_ALREADY_EXISTS) {
    publishPlayerPid();
    return true;
  }
  // A RESTART NOW child: its parent holds the mutex until it exits (it spawns us just before
  // quick_exit). Wait for that, then own the session.
  const char* parent = std::getenv("SR_RESTART_PARENT_PID");
  if (parent && *parent) {
    const DWORD ppid = DWORD(std::strtoul(parent, nullptr, 10));
    if (HANDLE ph = ppid ? OpenProcess(SYNCHRONIZE, FALSE, ppid) : nullptr) {
      WaitForSingleObject(ph, 30000);
      CloseHandle(ph);
    }
    const DWORD w = WaitForSingleObject(g_playerMutex, 30000);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {
      publishPlayerPid();
      return true;
    }
  }
  // Another player instance runs: bring it to the front (it may still be creating its window).
  const DWORD other = readPlayerPid();
  WinOfPid w{other, nullptr};
  for (int i = 0; other && !w.hwnd && i < 30; ++i) {
    EnumWindows(findTopWindowOfPid, reinterpret_cast<LPARAM>(&w));
    if (!w.hwnd) Sleep(100);
  }
  if (w.hwnd && !backgroundLaunch) {
    if (IsIconic(w.hwnd)) ShowWindow(w.hwnd, SW_RESTORE);
    AllowSetForegroundWindow(other);
    SetForegroundWindow(w.hwnd);
  } else if (!w.hwnd && !backgroundLaunch) {
    MessageBoxW(nullptr, L"Superman Returns is already running.", L"Superman Returns",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
  }
  fprintf(stderr,
          "[INSTANCE] another player instance is running (pid %lu, window %p): %s; this launch "
          "exits and stops nothing\n",
          static_cast<unsigned long>(other), static_cast<void*>(w.hwnd),
          backgroundLaunch ? "background launch, focus left alone"
          : w.hwnd         ? "switched to its window"
                           : "told the player it is already running");
  return false;
}
#endif

// iter-463 THE EXIT PATH (platform fix; SR_FAST_EXIT, default ON, =0 restores the old
// `return 0`). Closing the window and the custom front end's EXIT (fscommand SR_Quit ->
// kernel/mod_bridge.cpp -> SDL_EVENT_QUIT) both leave the event pump here. Returning from
// SDL_main runs the CRT's exit (static destructors, DLL detach) while the guest,
// render and audio threads are still executing, and that teardown crashed:
// archive/v1_frontend/menu/loadexit2 exited 0xC0000005 3.4 s after the loop ended.
// The reference host does not tear down under running guest threads either:
// UnleashedRecomp App::Exit() (reference/UnleashedRecomp/UnleashedRecomp/app.cpp:20,
// reached from its SDL_QUIT handler, ui/game_window.cpp:41) saves its config and calls
// std::_Exit(0). Here: finish the host-side files (input tape, log, every C stdio
// stream; guest saves are already flushed per NtWriteFile), then std::quick_exit, which
// runs the at_quick_exit handlers (host-side final dumps such as the guest profiler,
// kernel/guest_profile.cpp; register there, never in atexit alone) and then _Exit - no
// static destructors, no teardown under running guest threads. Both loops that run the
// game (the default event pump and SR_HLE_OWN_PRESENT=0) leave through it. The replay
// modes (SR_HLE_TRACE_REPLAY, SR_GPU_TRACE_REPLAY) keep their `return rc`: no guest
// thread runs there, so the crash mechanism does not apply, and replay_ab reads rc.
static bool fastExitEnabled() {
  const char* e = std::getenv("SR_FAST_EXIT");
  return !e || e[0] != '0';
}
// Step 4 (lane D; Astra 10:2x #5 / #6): the host's own producers are quiesced BEFORE their final drains, and every
// wait is bounded - a player's window close always finishes. Only while one of them runs (the present dumps,
// SR_PRESENT_DUMP / SR_PRESENT_DUMP_QUEUE; the pipeline census / manifest / warm-up, SR_PSO_*); otherwise the exit is
// exactly as before. (1) the dump queue stops admitting (a later frame is refused and counted; a presenter blocked on
// a full queue is released); (2) the presenter parks: the ring cut + the CP's drain point (kernel/quit.cpp
// parkPresenterForExit, at most 1 s) - no present, dump or pipeline create after it; (3) the queued dumps drain (at
// most 10 s - a diagnostic); (4) the pipeline workers stop, the last census line and manifest write on their own
// thread (at most 2 s). false = a wait timed out: a worker may still hold a C stream's lock or use the backend, so the
// caller ends the process without a global fflush and without tearing anything down.
static bool hostFinalDrains() {
  static const bool dumpOn = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP");
    return e && e[0] == '1';
  }();
  if (!dumpOn && !sr::gfx::dumpq::active() && !sr::gfx::psoAnyOn()) return true;
  const auto t0 = std::chrono::steady_clock::now();
  sr::gfx::dumpq::closeAdmission();
  const bool parked = sr::kernel::quit::parkPresenterForExit(1000);
  const bool dumps = sr::gfx::dumpq::drainAtExit(10000);
  const bool pso = sr::gfx::psoShutdown(2000);
  SR_LOG_BOOT(parked && dumps && pso ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
              "[EXIT] host drains: presenter parked %s, present dumps %s, pipelines %s - %.0f ms (step 4: the exit "
              "quiesce, each wait bounded)",
              parked ? "yes" : "NO (1 s deadline)", dumps ? "drained" : "TIMED OUT",
              pso ? "stopped" : "a worker still busy",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  return parked && dumps && pso;
}

// Astra (10:5x) #2: SR_FAST_EXIT=0 claims the exit too, before its drains (parkPresenterForExit runs only on a
// claimed exit) - and a graceful quit already in progress owns the exit, as in hostExitNow: this thread waits.
static void claimExitOrWait() {
  if (sr::kernel::quit::claimExit()) return;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[QUIT] SR_FAST_EXIT=0 teardown during a graceful quit: the quit ends the process");
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

// SR_FAST_EXIT=0 (the CRT-teardown A/B) after hostFinalDrains timed out: no teardown under a busy worker.
[[noreturn]] static void hostAbandonTeardown(int code) {
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[EXIT] SR_FAST_EXIT=0: a host worker is still busy after its deadline - std::quick_exit(%d) instead of "
              "the CRT teardown (nothing it uses is torn down)", code);
  sr::Log::shutdown();
  sr::stdio_guard::flush(500);
  std::quick_exit(code);
}

[[noreturn]] static void hostExitNow(int code) {
  // tools/graceful-quit: a harness-requested quit in progress owns the exit (it closes the log and quick_exits after
  // its terminal snapshot, or its hard deadline ends the process); this thread must not exit under it.
  if (!sr::kernel::quit::claimExit()) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[QUIT] window close / SR_Quit during a graceful quit: the quit ends the process");
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[EXIT] event pump ended (SDL_EVENT_QUIT: window close or SR_Quit) -> flush host files, "
      "std::quick_exit(%d) (SR_FAST_EXIT=0 restores the CRT teardown)", code);
  if (sr::kernel::modMenuRestartRequested())
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] relaunching the exe after this process's log is closed (RESTART NOW)");
  input_recorder::shutdown();
  // step 4 / Astra P2 3: quick_exit skips atexit - the queued present dumps and the pipeline manifest's last write
  // (and the warm-up's join) happen here, before the log closes (kernel/quit.cpp pExit does the same); quiesced and
  // bounded (hostFinalDrains, Astra 10:2x #5 / #6)
  const bool clean = hostFinalDrains();
  sr::Log::shutdown();
  if (clean) std::fflush(nullptr);   // not behind a writer that may still hold a stream
  sr::stdio_guard::flush(500);   // iter-465: hand the tail to stdout/stderr if they still listen
  sr::kernel::modMenuSpawnRestartIfRequested();   // iter-464 modmenu: no-op unless requested
  std::quick_exit(code);
}

// SDL_main.h (above) provides main / WinMain and calls SDL_main - so we must define SDL_main.
extern "C" int SDL_main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  // SR_INSTANCE_ID: enables parallel soak instances for A/B testing. When set, the
  // process takes no part in the single-player-instance claim (claimPlayerInstance) and
  // writes a per-instance log so runs don't clobber each other. Assets are read-only.
  const char* srInstanceId = std::getenv("SR_INSTANCE_ID");

  // iter-465 player: FIRST, before anything writes: a player's double-click console goes away,
  // and a stdout/stderr pipe or console that stalls can never freeze a game thread (the
  // RESTART NOW relaunch death of 2026-10-02; app/stdio_guard.hpp).
  const bool consoleDetached =
      sr::stdio_guard::detachOwnConsoleForPlayer(srInstanceId != nullptr && *srInstanceId != '\0');
  sr::stdio_guard::install();

#if defined(SR_GUEST_NEEDS_SSSE3)
  // iter-465 perf2: the guest lib is compiled with -mssse3 (CMake SR_GUEST_ISA). Say so
  // instead of dying on the first pshufb on a CPU without it (pre-2011 AMD).
  int cpuInfo[4] = {};
  __cpuid(cpuInfo, 1);
  if (!(cpuInfo[2] & (1 << 9))) {  // CPUID.1:ECX bit 9 = SSSE3
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Superman Returns",
                             "This build needs a CPU with SSSE3 (any Intel since 2006, AMD since 2011).",
                             nullptr);
    return 1;
  }
#endif

#if SR_ORACLE
  // iter-455j item 7 (director): SR_H7_ENTRYWATCH_TEST=1 turns this exe into a PURE
  // host-side test runner for the SR_H7_ENTRYWATCH probe and returns without booting
  // the guest. It MUST be decided before the player-instance claim below: a "pure host-side
  // test runner" that touches somebody else's running game is not host-side, and
  // the first version of this shipped exactly that way. Nothing EARLIER IN THIS
  // FUNCTION touches another process, the filesystem, or the log — which is the whole
  // guarantee. Static initializers and SDL's own WinMain run before main entirely and
  // are outside any gate placed here.
  const bool h7TestMode = [] {
    const char* e = std::getenv("SR_H7_ENTRYWATCH_TEST");
    return e && e[0] == '1';
  }();
  // iter-455l retirement: the H7C gate is DELETED (director D1 adjudication); the
  // retained named-isolation infrastructure keeps its own independent test mode.
  const bool isoTestMode = [] {
    const char* e = std::getenv("SR_ISOLATION_TEST");
    return e && e[0] == '1';
  }();
  // iter-455l r3: the provenance ring's host-side test runner (same contract).
  const bool provTestMode = [] {
    const char* e = std::getenv("SR_PROV_RING_TEST");
    return e && e[0] == '1';
  }();
  // iter-455l: the [WFFSM] load-completion observer's test runner.
  const bool wffsmTestMode = [] {
    const char* e = std::getenv("SR_WFFSM_OBS_TEST");
    return e && e[0] == '1';
  }();
  // iter-455n: the texture Unlock-upload contract test (glyph regression).
  const bool texUnlockTestMode = [] {
    const char* e = std::getenv("SR_HLE_TEX_UNLOCK_TEST");
    return e && e[0] == '1';
  }();
  // iter-455n Phase 2 v1: the key-only texture-identity registry test runner.
  const bool texIdentityTestMode = [] {
    const char* e = std::getenv("SR_HLE_TEX_IDENTITY_TEST");
    return e && e[0] == '1';
  }();
  // iter-457 r6: the GATE-OFF process. Deliberately a different env var from
  // SR_HLE_TEX_UNLOCK_TEST, because its whole purpose is to run where that gate
  // is closed and prove the test-only delegate bypass cannot arm.
  const bool texGateOffTestMode = [] {
    const char* e = std::getenv("SR_HLE_TEX_GATEOFF_TEST");
    return e && e[0] == '1';
  }();
#else
  // SR_ORACLE=0 (kernel/sr_oracle.hpp): the host-side self-test runners are compiled out, so no SR_*_TEST variable
  // turns this exe into a test runner (or exempts it from the player-instance claim).
  constexpr bool h7TestMode = false, isoTestMode = false, provTestMode = false, wffsmTestMode = false,
                 texUnlockTestMode = false, texIdentityTestMode = false, texGateOffTestMode = false;
#endif

#if defined(_WIN32)
  // The single-player-instance decision goes through two testable predicates
  // (kernel/player_instance_rule.hpp) so a self-test proves texIdentityTestMode is
  // in the exclusion set — dropping it from anySelfTestModeActive is a mutant the
  // suite catches (r2 blocker 3). iter-465 polish: it no longer kills anything
  // (claimPlayerInstance above).
  if (sr::kernel::texid::isPlayerInstanceLaunch(
          sr::kernel::texid::anySelfTestModeActive(
              h7TestMode, isoTestMode, provTestMode, wffsmTestMode,
              texUnlockTestMode, texIdentityTestMode)
              // OR-ed outside the predicate so its own self-test keeps the
              // arity and behaviour it was written against.
              || texGateOffTestMode,
          srInstanceId != nullptr && *srInstanceId != '\0') &&
      !claimPlayerInstance(sr::kernel::automation::policy().windowBackground))
    return 0;
#endif

  char srLogName[160] = "superman_returns.log";
  // iter-464 modmenu: a process relaunched by the MODS menu's RESTART NOW carries
  // SR_RESTART_GENERATION; in an automated roll it logs to its own file, so the parent's log
  // (which the harness copies) is never overwritten.
  const char* srRestartGen = std::getenv("SR_RESTART_GENERATION");
  if (srInstanceId && *srInstanceId && srRestartGen && *srRestartGen)
    snprintf(srLogName, sizeof(srLogName), "superman_returns_%s_r%s.log", srInstanceId, srRestartGen);
  else if (srInstanceId && *srInstanceId)
    snprintf(srLogName, sizeof(srLogName), "superman_returns_%s.log", srInstanceId);
  sr::Log::init(srLogName);
  SR_LOG_BOOT(sr::LogSeverity::Info, "Superman Returns - starting");
  {
    const sr::stdio_guard::Stats sg = sr::stdio_guard::stats();
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[STDIO_GUARD] console=%s stdout=%s(type %u) stderr=%s(type %u) (SR_STDIO_GUARD=0 disables)",
        consoleDetached ? "detached(player double-click)" : "kept",
        sg.stdoutGuarded ? "guarded" : "direct", sg.stdoutType,
        sg.stderrGuarded ? "guarded" : "direct", sg.stderrType);
  }
  if (srRestartGen && *srRestartGen) {
    const char* parent = std::getenv("SR_RESTART_PARENT_PID");
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODMENU] restarted by the MODS menu (generation %s, parent pid %s, pid %lu)",
                srRestartGen, parent ? parent : "?", static_cast<unsigned long>(GetCurrentProcessId()));
  }
  // iter-463 phase 2: the dev-tool gate is read EAGERLY, so the run tag ([RUN_CLASS]) sits in
  // the log header, before the first [GAUGE] (docs/DEVTOOLS_DESIGN.md 3.1). Off by default.
  sr::kernel::devToolsInitEager();
  sr::kernel::devToolsScanContentRoot(sr::kernel::xamContentRoot());
  // iter-464 perf lane: SR_PERF_STATS / SR_HOST_PROFILE_HZ telemetry (observe-only, off
  // unless set; docs/PERFORMANCE.md).
  sr::perf::start();

#if SR_ORACLE
  if (h7TestMode) {
    const int h7Fails = sr::kernel::h7::runSelfTestIfRequested();   // exit code = failures
    sr::Log::shutdown();
    return h7Fails < 0 ? 0 : h7Fails;
  }
  if (provTestMode) {
    const int provFails = sr::gfx::prov::runSelfTestIfRequested();  // exit code = failures
    sr::Log::shutdown();
    return provFails < 0 ? 0 : provFails;
  }
  if (isoTestMode) {
    const int isoFails = sr::kernel::isolation_test::runSelfTestIfRequested();  // exit code = failures
    sr::Log::shutdown();
    return isoFails < 0 ? 0 : isoFails;
  }
  if (wffsmTestMode) {
    const int wfFails = sr::wffsm::runSelfTestIfRequested();  // exit code = failures
    sr::Log::shutdown();
    return wfFails < 0 ? 0 : wfFails;
  }
  if (texUnlockTestMode) {
    const int tuFails = sr::kernel::texunlock::runSelfTestIfRequested();
    sr::Log::shutdown();
    return tuFails < 0 ? 0 : tuFails;
  }
  if (texIdentityTestMode) {
    const int idFails = sr::kernel::texid::runSelfTestIfRequested();
    sr::Log::shutdown();
    return idFails < 0 ? 0 : idFails;
  }
  if (texGateOffTestMode) {
    const int goFails = sr::kernel::texunlock::runGateOffSelfTestIfRequested();
    sr::Log::shutdown();
    return goFails < 0 ? 0 : goFails;
  }
#endif

#if defined(_WIN32)
  // SR_TIMER_RES_1MS (opt-in =1; iter-365f): 1 ms scheduler quantum. The
  // console's resource pipeline drains items back-to-back; our sub-ms sleeps
  // (scheduler empty-poll backoff, wall-clock retries) otherwise round up to
  // the default 15.6 ms quantum, throttling the whole load pipeline ~16x.
  // With this + SR_SCHED_FAST_POLL the metropol.AST world-preload chain runs
  // open→manifest-parse in ~48s (vs 170s+/never) and the FE-tick death
  // becomes DETERMINISTIC at ~+50s (the preload executor's sync EFFECT
  // request self-deadlocks against the vxd-definition load queued behind it —
  // head-of-line block). Default-OFF for now: the accelerated pipeline fires
  // that fatal request EARLY every roll, which closes the lucky-roll window
  // the deep-Apt menu build currently escapes through (iter-365 malloc0
  // precedent: console-correct but trunk-regressing → opt-in until the
  // ordering wall is root-fixed; then bake both ON with a K=8 A/B).
  {
    const char* e = std::getenv("SR_TIMER_RES_1MS");
    // iter-464 perf lane: SR_TIMER_RES=xenia sets the timer resolution exactly as Xenia's
    // main does (base/main_win.cc: NtSetTimerResolution(maximum resolution), 0.5 ms on
    // Windows 10/11), so a guest Sleep/KeDelayExecutionThread of N ms sleeps ~N ms instead of
    // rounding up to the 15.6 ms default quantum. =1ms is timeBeginPeriod(1) (the iter-365
    // SR_TIMER_RES_1MS behaviour); unset/0 keeps the Windows default.
    const char* r = std::getenv("SR_TIMER_RES");
    if (r && std::strcmp(r, "xenia") == 0) {
      typedef LONG(NTAPI * QueryRes)(PULONG, PULONG, PULONG);
      typedef LONG(NTAPI * SetRes)(ULONG, BOOLEAN, PULONG);
      HMODULE nt = GetModuleHandleA("ntdll.dll");
      auto q = nt ? reinterpret_cast<QueryRes>(GetProcAddress(nt, "NtQueryTimerResolution")) : nullptr;
      auto s = nt ? reinterpret_cast<SetRes>(GetProcAddress(nt, "NtSetTimerResolution")) : nullptr;
      ULONG mn = 0, mx = 0, cur = 0;
      if (q && s && q(&mn, &mx, &cur) == 0) {
        s(mx, TRUE, &cur);
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[TIMER_RES] NtSetTimerResolution(max=%lu x100ns) -> current %lu x100ns (Xenia main_win.cc)",
                    mx, cur);
      }
    } else if ((r && std::strcmp(r, "1ms") == 0) || (e && e[0] == '1')) {
      timeBeginPeriod(1);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[TIMER_RES] timeBeginPeriod(1) — 1ms scheduler quantum");
    }
  }
#endif
  input_recorder::init();

#if defined(_WIN32) && defined(BUILD_GUEST)
#if SR_ORACLE
  // Reserve mirror VA (base - 4GB) at process start; ~2.26 GB so sign-extended reads (0x86800002 etc.) are covered.
  void* mirrorVA = reinterpret_cast<void*>(0x2900000000ULL);
  size_t mirrorReserveSize = 0x87000000ULL;  // covers 0x2986800002 (offset 0x86800002)
  if (VirtualAlloc(mirrorVA, mirrorReserveSize, MEM_RESERVE, PAGE_READWRITE))
    SR_LOG_BOOT(sr::LogSeverity::Info, "Reserved mirror VA at 0x%p for guest EDRAM workaround", mirrorVA);
  else
    SR_LOG_BOOT(sr::LogSeverity::Error, "Failed to reserve mirror VA at 0x%p", mirrorVA);
#endif

  // BANDAID: Commit a page at host address 0x100000000 (4GB). Game code derives
  // this address through pointer arithmetic and reads from it. Without a mapped
  // page, this causes an AV in ntdll. Zeroed page returns safe "null" values.
  void* lowGuard = VirtualAlloc(reinterpret_cast<LPVOID>(0x100000000ULL), 0x10000,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (lowGuard)
    SR_LOG_BOOT(sr::LogSeverity::Info, "Mapped guard page at 0x100000000 for pointer arithmetic safety");
  else
    SR_LOG_BOOT(sr::LogSeverity::Error, "Failed to map guard page at 0x100000000");
#endif

  sr::installCrashHandler();
  sr::startHeapSentinel();  // step4/relcrash: SR_HEAP_SENTINEL (default OFF; app/heap_sentinel.hpp)
#if defined(SR_ASAN_BUILD)
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[ASAN] DIAGNOSTIC build: AddressSanitizer on the harness sources "
              "(CMake ASAN=ON; never a player package). ASAN_OPTIONS=%s",
              std::getenv("ASAN_OPTIONS") ? std::getenv("ASAN_OPTIONS") : "(unset)");
#endif

  sr::kernel::runHarness();

  // iter-464 bgruns: the automation policy (background QoS opt-out) + harness
  // keyboard channel, created BEFORE the window so a harness that has found the
  // HWND finds the channel too (kernel/host_automation.hpp; one [AUTOMATION] line).
  sr::kernel::automation::initAutomation();
  // tools/graceful-quit (kernel/quit.hpp, docs/TOOLKIT.md 1.8m): the quit channel of an automated run
  // (Local\SR_QUIT_<id>_<pid> + its ack), before the guest starts; one [QUIT] line.
  sr::kernel::quit::init();

  sr::PlatformSDL platform;
  // Skip SDL renderer when D3D12 is available (they'd conflict on the same HWND).
#ifdef _WIN32
  constexpr bool kTryD3D12 = true;
#else
  constexpr bool kTryD3D12 = false;
#endif
  // iter-464 PORT FEATURE (docs/RESOLUTION.md): the window follows the display
  // settings (settings/display.ini, SR_DISPLAY_MODE / SR_WINDOW_SIZE /
  // SR_OUTPUT_SIZE). The replay fixtures keep the fixed 1280x720 window
  // (deterministic captures); an automated roll (SR_INSTANCE_ID) never goes
  // fullscreen unless SR_DISPLAY_MODE asks - the user can see windows on this PC.
  // The window is resizable when the PM4 present (which fits any size) presents.
  bool platformOk;
  {
#if SR_ORACLE
    const bool replayMode = (std::getenv("SR_GPU_TRACE_REPLAY") && *std::getenv("SR_GPU_TRACE_REPLAY")) ||
                            (std::getenv("SR_HLE_TRACE_REPLAY") && *std::getenv("SR_HLE_TRACE_REPLAY"));
#else
    constexpr bool replayMode = false;  // SR_ORACLE=0: no replay mode (SR_GPU_TRACE_REPLAY / SR_HLE_TRACE_REPLAY compiled out)
#endif
    const auto& disp = sr::gfx::resscale::display();
    int mode = int(disp.mode);
    const char* inst = std::getenv("SR_INSTANCE_ID");
    if (inst && *inst && disp.modeSource != "SR_DISPLAY_MODE") mode = 0;
    if (replayMode) {
      platformOk = platform.init(1280, 720, kTryD3D12);
    } else {
      const bool resizable = sr::gfx::pm4LiveEnabled();
      platformOk = platform.initDisplay(mode, resizable ? int(disp.windowW) : 1280,
                                        resizable ? int(disp.windowH) : 720, int(disp.outputW),
                                        int(disp.outputH), resizable, kTryD3D12);
    }
    if (platformOk)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[DISPLAY] %s window %dx%d (client pixels)",
                  replayMode ? "replay" : sr::gfx::resscale::modeName(sr::gfx::resscale::DisplayMode(mode)),
                  platform.getWindowWidth(), platform.getWindowHeight());
  }
  if (!platformOk) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "Platform init failed: %s", SDL_GetError());
    sr::Log::shutdown();
    return 1;
  }

  // iter-465 window: Xenia-style title - the game's name plus the port and its build.
  platform.setWindowTitle(kPortWindowTitle);
  SR_LOG_BOOT(sr::LogSeverity::Info, "Host app ready - window open, entering main loop");

#if SR_ORACLE
  // SR_GPU_TRACE_REPLAY=<file.xtr>: replay a Xenia GPU trace through our
  // PM4->D3D12 backend instead of booting the game (docs/TOOLKIT.md section 1.6).
  const char* gpuTraceReplayPath = std::getenv("SR_GPU_TRACE_REPLAY");
  if (gpuTraceReplayPath && !*gpuTraceReplayPath) gpuTraceReplayPath = nullptr;
#endif

#if SR_ORACLE
  // iter-442 S5: SR_HLE_TRACE_REPLAY=<file.srht> replays a recorded HLE command
  // stream (with its plane payloads) through the SAME hle::execute() the render
  // thread runs — the seconds-long loop that S4.4-S4.7 are developed against
  // instead of 200 s boots. Like the GPU-trace mode it skips the guest entirely.
  const char* hleTraceReplayPath = std::getenv("SR_HLE_TRACE_REPLAY");
  if (hleTraceReplayPath && !*hleTraceReplayPath) hleTraceReplayPath = nullptr;
  const bool skipGuest = gpuTraceReplayPath || hleTraceReplayPath;
#else
  // SR_ORACLE=0: no replay mode - the GPU-trace replay and the HLE-trace replay (gfx/hle_video.cpp) are compiled out,
  // so SR_GPU_TRACE_REPLAY / SR_HLE_TRACE_REPLAY read as unset and the guest always boots.
  constexpr bool skipGuest = false;
#endif

#if BUILD_GUEST
  // Run guest in background so window appears immediately; guest may never return (game loop).
  // Use Windows CreateThread with large stack - std::thread default (~1 MB) causes stack overflow.
  // Skipped entirely in GPU-trace-replay mode: the replay owns guest memory and no game runs.
  if (!skipGuest) {
    auto guestThreadProc = [](LPVOID) -> DWORD {
      SR_LOG_BOOT(sr::LogSeverity::Info, "Invoking guest entry point (background thread)...");
      sr::guest::runGuestHarness();
      SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness returned (background thread)");
      return 0;
    };
    constexpr SIZE_T kGuestStackSize = 1024 * 1024 * 1024;  // 1 GB stack (std::thread default ~1 MB causes overflow)
    HANDLE hGuestThread = CreateThread(nullptr, kGuestStackSize, guestThreadProc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (hGuestThread) CloseHandle(hGuestThread);  // detach
  }
#endif

  bool running = true;
#if SR_ORACLE
  int frameCount = 0;
  auto mainLoopStart = std::chrono::steady_clock::now();
#endif

  // Initialize D3D12 backend using the SDL window's HWND (SDL3: the window's
  // SDL_PROP_WINDOW_WIN32_HWND_POINTER property).
#ifdef _WIN32
  sr::gfx::D3D12Backend d3d12;
  {
    if (HWND hwnd = static_cast<HWND>(platform.nativeWindowHandle())) {
      // iter-464 resscale: the scale ("auto" follows the output size) is fixed
      // before the backend renders anything (gfx/res_scale.hpp).
      sr::gfx::resscale::configure(uint32_t(platform.getWindowWidth()),
                                   uint32_t(platform.getWindowHeight()));
      if (d3d12.init(hwnd, platform.getWindowWidth(), platform.getWindowHeight())) {
        SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12] Backend ready – will present via D3D12");
      } else {
#if SR_ORACLE
        SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] Init failed, falling back to SDL renderer");
#else
        SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] Native render backend initialization failed");
#endif
      }
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] Could not get HWND from SDL: %s", SDL_GetError());
    }
  }
  bool useD3D12 = d3d12.isInitialized();
#if SR_ORACLE
  // render/boot-attach: the CP waits for this backend (SR_PM4_WAIT_BACKEND), with no time limit. A backend that
  // failed to init is said at once: the CP stops loudly (exit 3) instead of running the ring without a backend
  // (the black-world path, docs/NATIVE_ROADMAP.md "Boot attach"). Every other path below attaches.
  if (!useD3D12 && sr::gfx::pm4LiveEnabled())
    sr::gfx::failPm4LiveBackend("Direct3D 12 initialisation failed (the [D3D12] lines before this one name the step)");
  // iter-442 S3: hand the device to the HLE render thread, which becomes its
  // SOLE owner. main() created it because it holds the HWND; from here the main
  // loop must never present again (see the pump-only loop below) — one thread
  // touching D3D12 is the reference's model and the precondition for a
  // guest-driven frame boundary.
  // P2 (SR_PM4_LIVE=1): the PM4 path draws and presents on the guest's swap;
  // the HLE layer keeps observing with NO backend (bookkeeping only).
#if SR_ORACLE
  if (useD3D12 && !sr::gfx::pm4LiveEnabled()) sr::gfx::hle::setBackend(&d3d12);
#endif  // SR_ORACLE=0: the HLE renderer A/B (SR_PM4_LIVE=0) is compiled out; pm4LiveEnabled() is always true
  if (useD3D12 && sr::gfx::pm4LiveEnabled()) {
    sr::gfx::setPm4LiveBackend(&d3d12);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_LIVE] the live game draws through the PM4 path (HLE layer: observers only)");
  }
#else
  if (!useD3D12) {
    sr::gfx::failRenderBackend("Direct3D 12 initialization failed (see the preceding D3D12 log)");
  } else {
    sr::gfx::setRenderBackend(&d3d12);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_RENDER] native records draw and present through Direct3D 12");
  }
#endif

#else
  bool useD3D12 = false;
#endif

#ifdef _WIN32
#if SR_ORACLE
  if (hleTraceReplayPath) {
    // Replay owns this thread AND the device (no guest, so no render thread
    // starts — nothing publishes). Loops so the window shows a live picture the
    // moment S4.4-S4.7 make the executor draw; SR_HLE_TRACE_LOOPS=1 for a
    // one-shot batch check.
    int loops = -1;
    if (const char* n = std::getenv("SR_HLE_TRACE_LOOPS")) {
      const int v = std::atoi(n);
      if (v > 0) loops = v;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[HLE_REPLAY] replay mode: %s (loops=%d)",
                hleTraceReplayPath, loops);
    sr::gfx::hle::TraceStats st{};
    int rc = 0;
    if (loops > 0) {
      rc = sr::gfx::hle::replayTrace(hleTraceReplayPath, loops, &st);
    } else {
      // Unbounded: one pass per pumped frame, so the window stays responsive.
      while (platform.pumpEvents()) {
        rc = sr::gfx::hle::replayTrace(hleTraceReplayPath, 1, &st);
        if (rc == 1) break;            // unreadable trace — do not spin on it
        SDL_Delay(16);
      }
    }
    // iter-457 gate 3: a replay that applied a suppression treatment is only
    // valid if the treatment was EXACT. Fold the manifest status into the exit
    // code so requested != matched != suppressed can never look like a pass.
    const int manRc = sr::gfx::hleDrawManifestStatus();
    if (manRc != 0 && rc == 0) rc = manRc;
    sr::gfx::getShaderCache().ensureLoaded();  // render/boot-attach: the load thread ends before the CRT exit
    sr::Log::shutdown();
    return rc;
  }

  if (gpuTraceReplayPath) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[GPU:REPLAY] replay mode: %s", gpuTraceReplayPath);
    int rc = sr::gfx::runGpuTraceReplay(gpuTraceReplayPath, useD3D12 ? &d3d12 : nullptr);
    const char* hold = std::getenv("SR_GPU_TRACE_REPLAY_HOLD");
    if (hold && hold[0] == '1') {
      // Keep the window up re-presenting the final frame until the user closes it.
      uint32_t rfVA = 0; int rfW = 0, rfH = 0, rfPitch = 0; bool rfTiled = false;
      bool haveFb = sr::kernel::getGuestFramebufferInfo(&rfVA, &rfW, &rfH, &rfPitch, &rfTiled);
      while (platform.pumpEvents()) {
        uint8_t* fb = haveFb ? sr::kernel::guestVAToHost(rfVA) : nullptr;
        if (useD3D12) {
          if (fb) d3d12.presentFramebuffer(fb, rfW, rfH, rfPitch, rfTiled);
          else d3d12.presentPlaceholder(0);
        }
        SDL_Delay(16);
      }
    }
    sr::gfx::getShaderCache().ensureLoaded();  // render/boot-attach: the load thread ends before the CRT exit
    sr::Log::shutdown();
    return rc;
  }
#endif  // SR_ORACLE
#endif

  // iter-442 S3: THE MAIN LOOP IS AN EVENT PUMP.
  // It used to present every iteration by scraping the guest framebuffer
  // (presentFramebuffer/presentPlaceholder) on its OWN cadence — which is why
  // `present` counted 11,700 while the guest swapped 7,500 times. That is the
  // "fb-present guessing" the migration retires: the host inventing frames the
  // guest never asked for. The guest's own Swap is now the frame boundary and
  // the render thread issues the present (gfx/hle_video.cpp).
  // SR_HLE_OWN_PRESENT=0 restores the old loop for A/B.
#if SR_ORACLE
  const bool hleOwnsPresent = [] {
    const char* e = std::getenv("SR_HLE_OWN_PRESENT");
    return !e || e[0] != '0';
  }();
  if (hleOwnsPresent) {
#else
  // SR_ORACLE=0: the legacy host-driven present loop (SR_HLE_OWN_PRESENT=0: guest-framebuffer scrape, placeholder,
  // SDL fallback) is an A/B and is compiled out; SR_HLE_OWN_PRESENT reads as unset. The PM4 path presents on the
  // guest's swap and this thread only pumps events.
  {
#endif
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[D3D12] main loop = EVENT PUMP ONLY; the HLE render thread presents on "
        "the guest's Swap (SR_HLE_OWN_PRESENT=0 restores host-driven present)");
    // iter-463 review: the VdSwap title updates below live in the legacy loop only, so a tagged
    // run never showed its tag on this default path. A run with no dev tool keeps its title.
    if (const char* sfx = sr::kernel::devToolsTitleSuffix(); sfx && *sfx) {
      char title[160];
      snprintf(title, sizeof(title), "%s%s", kPortWindowTitle, sfx);
      platform.setWindowTitle(title);
    }
    while (running) {
      running = platform.pumpEvents();
#ifdef _WIN32
      // iter-464: a new client size reaches the presenter (it resizes the
      // swapchain before its next present).
      int rw = 0, rh = 0;
      if (useD3D12 && platform.consumeResize(rw, rh)) d3d12.requestResize(rw, rh);
#endif
      SDL_Delay(4);
    }
    if (fastExitEnabled()) hostExitNow(0);
    // SR_FAST_EXIT=0: the same quiesce + bounded drains, before the log closes (Astra 10:2x #6), on a claimed exit
    claimExitOrWait();
    if (!hostFinalDrains()) hostAbandonTeardown(0);
    sr::Log::shutdown();
    return 0;
  }
#if SR_ORACLE

  while (running) {
    running = platform.pumpEvents();
    // iter-358f: the fake-GPU-device setup + host-driven render thread that used to
    // run here were removed with the KeCertMonitorData-based fake D3D subsystem.
    // --- Present ---
    uint64_t swapCount = sr::kernel::getVdSwapCount();
    uint32_t fbVA = 0;
    int fbW = 0, fbH = 0, fbPitch = 0;
    bool fbTiled = false;

    if (useD3D12) {
      if (swapCount > 0 && sr::kernel::getGuestFramebufferInfo(&fbVA, &fbW, &fbH, &fbPitch, &fbTiled)) {
        uint8_t* hostFb = sr::kernel::guestVAToHost(fbVA);
        if (hostFb && fbW > 0 && fbH > 0) {
          static bool s_loggedFirstD3D12 = false;
          if (!s_loggedFirstD3D12) {
            s_loggedFirstD3D12 = true;
            SR_LOG_BOOT(sr::LogSeverity::Info,
              "[D3D12] First present: fbVA=0x%08X %dx%d pitch=%d", fbVA, fbW, fbH, fbPitch);
          }
          d3d12.presentFramebuffer(hostFb, fbW, fbH, fbPitch, fbTiled);
        } else {
          d3d12.presentPlaceholder(swapCount);
        }
      } else {
        d3d12.presentPlaceholder(swapCount);
      }
    } else {
      // SDL fallback path.
      if (swapCount > 0 && sr::kernel::getGuestFramebufferInfo(&fbVA, &fbW, &fbH, &fbPitch)) {
        uint8_t* hostFb = sr::kernel::guestVAToHost(fbVA);
        bool hasContent = false;
        if (hostFb && fbW > 0 && fbH > 0) {
          for (int probe = 0; probe < 16 && !hasContent; probe++) {
            int y = (fbH * probe) / 16;
            int x = (fbW * probe) / 16;
            const uint8_t* px = hostFb + y * fbPitch + x * 4;
            if (px[0] | px[1] | px[2]) hasContent = true;
          }
        }
        if (hasContent && hostFb) {
          platform.presentWithFramebuffer(hostFb, fbW, fbH, fbPitch);
        } else {
          platform.presentVdSwapActive(swapCount);
        }
      } else if (swapCount > 0) {
        platform.presentVdSwapActive(swapCount);
      } else {
        platform.present();
      }
    }

    // Update window title with VdSwap info (throttled to ~1/sec to avoid flicker).
    {
      static uint64_t s_lastTitleSwapCount = 0;
      if (swapCount > 0 && swapCount >= s_lastTitleSwapCount + 60) {
        s_lastTitleSwapCount = swapCount;
        char title[128];
        snprintf(title, sizeof(title), "Superman Returns - VdSwap #%" PRIu64 " (1280x720)%s", swapCount,
                 sr::kernel::devToolsTitleSuffix());
        platform.setWindowTitle(title);
      } else if (swapCount > 0 && s_lastTitleSwapCount == 0) {
        s_lastTitleSwapCount = swapCount;
        char title[160];
        snprintf(title, sizeof(title), "Superman Returns - VdSwap active (1280x720)%s", sr::kernel::devToolsTitleSuffix());
        platform.setWindowTitle(title);
      }
    }

    frameCount++;
    if (frameCount == 60) {
      SR_LOG_BOOT(sr::LogSeverity::Debug, "Main loop running (60 frames)");
      frameCount = 0;

      // Periodic main-thread PC sample (every ~10s) — independent of guest call activity.
      // Worker threads make constant NtWait calls, keeping hasGuestCallInLastMs() true,
      // so the idle detector below never fires. This samples the main guest thread's LR
      // unconditionally so we can find where the main thread is stuck.
      {
        static uint64_t s_lastPCSampleMs = 0;
        static bool s_r1CorruptionDetected = false;
        static bool s_lrCorruptionDetected = false;
        static uint32_t s_lastGoodLR = 0;
        static uint32_t s_lastGoodR1 = 0;
        uint64_t nowPC = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        uint32_t r1Now = sr::kernel::getCurrentGuestR1();
        // Fast corruption check every frame (not just every 10s)
        {
          uint32_t lrNow = sr::kernel::getCurrentGuestPC();
          if (!s_r1CorruptionDetected && r1Now != 0 && (r1Now < 0x82000000u || r1Now > 0x86000000u)) {
            s_r1CorruptionDetected = true;
            uint32_t r3 = sr::kernel::getCurrentGuestR3();
            uint32_t r31 = sr::kernel::getCurrentGuestR31();
            uint32_t totalCalls = sr::kernel::ExportRegistry::getGuestCallCount();
            SR_LOG_BOOT(sr::LogSeverity::Error,
              "[BOOT] [R1_CORRUPT] FIRST detection: r1=0x%08X lr=0x%08X r3=0x%08X r31=0x%08X totalCalls=%u prevGoodLR=0x%08X prevGoodR1=0x%08X",
              r1Now, lrNow, r3, r31, totalCalls, s_lastGoodLR, s_lastGoodR1);
          }
          if (!s_lrCorruptionDetected && lrNow == 0xFFFFFFFFu) {
            s_lrCorruptionDetected = true;
            uint32_t r3 = sr::kernel::getCurrentGuestR3();
            uint32_t ctr = sr::kernel::getCurrentGuestCtr();
            uint32_t r31 = sr::kernel::getCurrentGuestR31();
            uint32_t totalCalls = sr::kernel::ExportRegistry::getGuestCallCount();
            SR_LOG_BOOT(sr::LogSeverity::Error,
              "[BOOT] [LR_CORRUPT] lr=0xFFFFFFFF r1=0x%08X r3=0x%08X ctr=0x%08X r31=0x%08X totalCalls=%u prevGoodLR=0x%08X prevGoodR1=0x%08X",
              r1Now, r3, ctr, r31, totalCalls, s_lastGoodLR, s_lastGoodR1);
          }
          if (lrNow != 0 && lrNow != 0xFFFFFFFFu && lrNow >= 0x82000000u && lrNow <= 0x86000000u) {
            s_lastGoodLR = lrNow;
          }
          if (r1Now >= 0x82000000u && r1Now <= 0x86000000u) {
            s_lastGoodR1 = r1Now;
          }
        }
        if (s_lastPCSampleMs == 0 || (nowPC - s_lastPCSampleMs) >= 3000) {
          uint32_t pc = sr::kernel::getCurrentGuestPC();
          uint32_t ctr = sr::kernel::getCurrentGuestCtr();
          uint32_t r3 = sr::kernel::getCurrentGuestR3();
          uint32_t r31 = sr::kernel::getCurrentGuestR31();
          uint32_t totalCalls = sr::kernel::ExportRegistry::getGuestCallCount();
          uint32_t mainCalls = sr::kernel::getMainThreadCallCount();
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[BOOT] [PC_SAMPLE] main guest lr=0x%08X ctr=0x%08X r3=0x%08X r1=0x%08X r31=0x%08X totalCalls=%u mainThreadCalls=%u",
            pc, ctr, r3, r1Now, r31, totalCalls, mainCalls);

          // Stack walk using guestVAToHost (has global fallback for host thread)
          {
            auto readBE32 = [](uint32_t va) -> uint32_t {
              uint8_t* p = sr::kernel::guestVAToHost(va);
              if (!p || !sr::kernel::guestVAToHost(va + 3)) return 0;
              return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                     (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            };
            bool r1Valid = (sr::kernel::guestVAToHost(r1Now) != nullptr);
            if (r1Valid && r1Now != 0) {
              static int s_stackWalkCount = 0;
              if (s_stackWalkCount++ < 8) {
                SR_LOG_BOOT(sr::LogSeverity::Info, "[BOOT] [STACK_WALK] starting at r1=0x%08X:", r1Now);
                uint32_t frame = r1Now;
                for (int depth = 0; depth < 16; ++depth) {
                  if (!sr::kernel::guestVAToHost(frame)) break;
                  uint32_t backChain = readBE32(frame);
                  uint32_t savedLR = 0;
                  if (backChain >= 8 && sr::kernel::guestVAToHost(backChain - 8))
                    savedLR = readBE32(backChain - 8);
                  SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[BOOT] [STACK_WALK]   [%d] frame=0x%08X backchain=0x%08X savedLR=0x%08X",
                    depth, frame, backChain, savedLR);
                  if (backChain == 0 || backChain <= frame) break;
                  frame = backChain;
                }
              }
            }
          }

          // Dump main-thread kernel call histogram
          sr::kernel::dumpMainThreadCallHistogram();

          s_lastPCSampleMs = nowPC;
        }
      }

      // Guest idle heartbeat: if guest has made kernel calls before but none in 5s, log once per 30s
      if (sr::kernel::ExportRegistry::getLastGuestCallTimeMs() != 0 &&
          !sr::kernel::ExportRegistry::hasGuestCallInLastMs(5000)) {
        static uint64_t s_lastIdleLogMs = 0;
        uint64_t now = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (s_lastIdleLogMs == 0 || (now - s_lastIdleLogMs) >= 30000) {
          uint32_t totalCalls = 0;
          if (sr::kernel::ExportRegistry::getLastGuestCallTimeMs() != 0) {
            totalCalls = sr::kernel::ExportRegistry::getGuestCallCount();
          }
          uint32_t guestPC = sr::kernel::getCurrentGuestPC();
          uint32_t guestCtr = sr::kernel::getCurrentGuestCtr();
          uint32_t guestR3 = sr::kernel::getCurrentGuestR3();
          uint32_t guestR1 = sr::kernel::getCurrentGuestR1();
          uint32_t guestR31 = sr::kernel::getCurrentGuestR31();
          SR_LOG_BOOT(sr::LogSeverity::Info, "[BOOT] Guest idle: no kernel calls for 5s (total guest calls=%u; guest PC=0x%08X ctr=0x%08X r3=0x%08X; may be spinning in recomp code)", totalCalls, guestPC, guestCtr, guestR3);
          if (guestPC == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info, "[BOOT] Guest PC=0 means guest lr=0 (return-to-zero or invalid); \"Guest exit\" is only logged when the guest thread returns to the harness.");
          if (guestPC != 0 && (guestPC < 0x82000000u || guestPC >= 0x83000000u)) {
            SR_LOG_BOOT(sr::LogSeverity::Info, "[BOOT] Guest PC outside code range: r1=0x%08X r31=0x%08X (saved LR often at r1+8)", guestR1, guestR31);
            // One-time stack dump: read 16 words from r1 to find corrupt saved LR
            static bool s_stackDumped = false;
            if (!s_stackDumped && guestR1 != 0) {
              s_stackDumped = true;
              uint8_t* stackHost = sr::kernel::guestVAToHost(guestR1);
              if (stackHost) {
                for (int i = 0; i < 16; i++) {
                  uint32_t off = (uint32_t)(i * 4);
                  uint32_t val = (stackHost[off] << 24) | (stackHost[off+1] << 16) | (stackHost[off+2] << 8) | stackHost[off+3];
                  SR_LOG_BOOT(sr::LogSeverity::Info, "[BOOT] Stack dump: r1+%u = 0x%08X", off, val);
                }
                // Also walk back: saved r1 at r1+0 is previous frame pointer
                uint32_t prevR1 = (stackHost[0] << 24) | (stackHost[1] << 16) | (stackHost[2] << 8) | stackHost[3];
                if (prevR1 != 0 && prevR1 != guestR1) {
                  uint8_t* prevHost = sr::kernel::guestVAToHost(prevR1);
                  if (prevHost) {
                    SR_LOG_BOOT(sr::LogSeverity::Info, "[BOOT] Previous frame at 0x%08X:", prevR1);
                    for (int i = 0; i < 8; i++) {
                      uint32_t off = (uint32_t)(i * 4);
                      uint32_t val = (prevHost[off] << 24) | (prevHost[off+1] << 16) | (prevHost[off+2] << 8) | prevHost[off+3];
                      SR_LOG_BOOT(sr::LogSeverity::Info, "[BOOT] Stack dump: prev_r1+%u = 0x%08X", off, val);
                    }
                  }
                }
              }
            }
          }
          s_lastIdleLogMs = now;
        }
      }
    }

    // iter-437d JUDDER FIX: the fixed 16ms sleep stacked on ~30ms of present
    // work capped the loop at ~21/s against 30fps movie content — every other
    // frame repeated ([DRAIN_PROF] timestamps + recording frame diffs). The
    // D3D12 path is already vsync-paced by Present(1,0); the sleep only
    // belongs to the SDL-fallback/no-present paths.
    if (!useD3D12) SDL_Delay(16);
  }

  SR_LOG_BOOT(sr::LogSeverity::Info, "Shutting down");
  if (fastExitEnabled()) hostExitNow(0);   // the same exit as the event-pump loop above
  input_recorder::shutdown();
  // step 4 / Astra P2 1 + 10:2x #5 / #6: quiesced, bounded, before the backend's teardown, on a claimed exit
  claimExitOrWait();
  if (!hostFinalDrains()) hostAbandonTeardown(0);
  platform.shutdown();
  sr::Log::shutdown();
  sr::kernel::modMenuSpawnRestartIfRequested();   // iter-464 modmenu: no-op unless requested
  return 0;
#endif  // SR_ORACLE (the legacy host-driven present loop)
}
