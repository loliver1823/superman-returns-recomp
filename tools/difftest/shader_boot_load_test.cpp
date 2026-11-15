// render/boot-attach (docs/NATIVE_ROADMAP.md "Boot attach"): the shader cache's staged boot load and
// its wait points, offline. No guest, no game; T8 also needs a D3D12 device and a swap chain.
//
//   build/difftest.exe --shader-boot-load-test [--work <dir>] [--source <installed cache dir>]
//
// The fixtures are small installed caches copied from --source (default data/shader_cache/dxil, the
// git-tracked cache) into --work (default archive/shader_boot_load_test). A read hook
// (ShaderCache::setReadHookForTest) slows, fails or throws stage-1 reads; test runtime hooks stand in
// for the translator's stage 2 (a slow boot that admits one "persisted" translation).
//
// Checks
//   T1  probe: probeInstalledDir says yes only for a dir holding a file stage 1 admits (the loader's
//       own name / size filter): no for a missing / empty dir and for .vsf.dxil-only, zero-byte or
//       other files
//   T2  decoupling: with every stage-1 read slowed, beginBootLoad returns at once (the attach path),
//       installedPresent answers from the probe without waiting, and the load takes the slowed time
//   T3  WP2, installed hit: findVs / findPs on another thread during the slowed load block until stage
//       1 is published and return the entry built from that very file - never a miss
//   T4  WP2, translator stage (stage 2 slowed, then it admits a "persisted" VS): an installed hit
//       returns while stage 2 still runs; an installed miss waits for stage 2 and finds the persisted
//       entry; a genuine miss after it reaches the translator's onMiss
//   T5  failure: every read failing -> the probe said yes, installedPresent() turns false at stage 1
//       (the old "no cache" decision); a read that throws -> both stages still published, no waiter
//       hangs; the loader's own hooks looking up a miss on the loader thread do not wait on themselves
//   T6  the IM_LOAD wiring (the global cache + a real Pm4Engine, the CP's path): an IM_LOAD of a cached
//       VS during the slowed load blocks until stage 1 and counts a HIT (lastVsUcodeHash = its hash,
//       xlatVsHits 1, xlatMisses 0), and returns while stage 2 still runs (no wait for the translator)
//   T12 (step 4 lane C, 0005) a <hash>.ps.at.dxil alpha-test variant is attached to its PS (bytes + native code),
//       never admitted as an entry of its own (PS count, a variant without its PS, the probe), and
//       loadEntryFromDir reads it too
//   T10 stage 1's end time is sampled before its publication: with the loader stalled right after publishing,
//       a reader released by the publication never sees a later end (the evaluator's witness stays sound)
//   T7  hang watchdog in a host wait (CP phase 5), no timing heuristic: T7a a fence / present stall during the
//       wait still fires (H1, H2) whenever it began; T7b the CP's own stall trigger H3 is set aside (it fires in
//       phase 1, the control); T7c the escalation schedule (60 s, then every 5 min: 24 in 2 h); T7d the [GPU_HANG]
//       report fired in a host wait is annotated with the wait
//   T11 the CP's first command is recorded where it is consumed: a first ring packet / host item whose words
//       arrive late is recorded when they do (gpuCpTakeCommand, both command paths)
//   T9  the CP's wait for the live backend (gpuCpWaitForBackend), one check per branch of its policy:
//       T9a no attach yet, init not failed (a child process, fake clock): it waits 2 simulated hours - no
//           60 s limit, no exit - with a still-waiting Warn every period, in phase 5, and returns on the attach
//           with the total wait; the phase before it is restored
//       T9b the same on the real clock (default sleep / clock): returns on a 150 ms attach, no warning
//       T9c an explicit init failure (a child process): INIT FAILED, [FATAL], exit 3 (no message box: the
//           child sets SR_INSTANCE_ID)
//       T9d the real watchdog on a short schedule over a 2.6 s real-clock wait (a child process): it escalates
//           as [HOST_WAIT_STALL] blocks naming the backend init's step / thread and the loader's progress, never
//           as a [GPU_HANG], and the wait returns only on the attach
//       T9e SR_LOG=player (a child): 70 [HOST_WAIT_STALL] reports then 3 [GPU_HANG] reports keep every section of
//           every report (literal milestone tags), and the host reports call the run-time-tagged thread / CS dumps
//           once. The child registers no guest thread and holds no CS: it shows the dumps' summary line kept, not
//           populated thread / CS rows (those share the same per-site cap the once-per-process rule protects)
//       T9f a lock the report's helper needs held across many escalations (a child): one helper thread at most,
//           the later reports skip its queries and say so, the thread count stays bounded
//   T8  D3D12Backend::init with stage 1 slowed (when this machine gives a device + swap chain):
//       init returns before the slowed stage 1 is published and takes less than it - the load is off init's
//       path (the probe, a directory listing, and the thread's start stay on it)
//
// Exit code = failed checks; prints "N passed, M failed". Mutation check:
// tools/difftest/shader_boot_load_run.py (each mutant must fail this suite).
//
// What it does not cover: difftest is built with SR_ORACLE=1 in every preset (build-rel included; only the game
// target gets SR_ORACLE=0 from SR_RELEASE_COMPOSITION), so this suite runs the oracle build of these files. The
// boot-attach code has no SR_ORACLE conditional (docs/NATIVE_ROADMAP.md "Boot attach", "Offline proof").
#include "gfx/shader_cache.hpp"
#include "gfx/pm4_engine.hpp"
#include "gfx/d3d12_backend.hpp"
#include "gfx/xenos_ucode.hpp"
#include "kernel/gpu_hang_wd.hpp"
#include "kernel/gpu_submit.hpp"
#include "app/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace G = sr::gfx;
namespace K = sr::kernel;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
int g_pass = 0, g_fail = 0;

void check(bool ok, const std::string& what, const std::string& detail = "") {
  if (ok) {
    g_pass++;
    std::printf("  PASS %s%s%s\n", what.c_str(), detail.empty() ? "" : " ", detail.c_str());  // the measured values too
  } else {
    g_fail++;
    std::printf("  FAIL %s %s\n", what.c_str(), detail.c_str());
  }
  std::fflush(stdout);
}

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::string fmtMs(const char* f, double a, double b = 0.0) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), f, a, b);
  return buf;
}

std::vector<uint8_t> readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string hx(uint64_t h) {
  char b[24];
  std::snprintf(b, sizeof(b), "%016" PRIx64, h);
  return b;
}

// A blocking call that must end: run it on a thread; a hang fails the suite and ends the process.
template <class F>
void mustEnd(const char* what, int timeoutMs, F&& fn) {
  auto fut = std::async(std::launch::async, std::forward<F>(fn));
  if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
    check(false, what, "(HUNG - the process ends here)");
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    std::fflush(stdout);
    std::_Exit(99);
  }
  fut.get();
}

// ── the stage-1 read hook ────────────────────────────────────────────────────
std::atomic<int> g_readDelayMs{0};
std::atomic<int> g_failReads{0};
std::atomic<int> g_throwAtRead{0};
std::atomic<int> g_reads{0};
bool readHook(const fs::path&) {
  const int n = g_reads.fetch_add(1) + 1;
  if (const int d = g_readDelayMs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(d));
  if (g_throwAtRead.load() == n) throw std::runtime_error("test: the read hook throws");
  return g_failReads.load() == 0;
}
std::atomic<int> g_publishDelayMs{0};
void publishHook(int stage) {  // T10: the loader descheduled right after it published stage 1
  if (stage == G::ShaderCache::kStageInstalled)
    if (const int d = g_publishDelayMs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(d));
}
void hookReset() {
  g_readDelayMs = 0;
  g_failReads = 0;
  g_throwAtRead = 0;
  g_reads = 0;
}

// ── the translator's slot: a stage 2 that sleeps, then admits a "persisted" translation ──
std::atomic<int> g_stage2SleepMs{0};
std::atomic<int> g_onMiss{0};
std::atomic<int> g_selfLookup{0}, g_selfLookupDone{0};
fs::path g_persistSrcDir;
uint64_t g_persistSrcHash = 0, g_persistHash = 0;
void testOnLoaded(G::ShaderCache& c) {
  if (const int s = g_stage2SleepMs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(s));
  if (g_selfLookup.load()) {
    (void)c.findVs(0x5EF1005EF1005EF1ull);  // a miss looked up on the loader thread itself
    g_selfLookupDone = 1;
  }
  if (g_persistHash) {
    G::CachedShader e;
    if (G::ShaderCache::loadEntryFromDir(g_persistSrcDir, g_persistSrcHash, true, e))
      c.admitRuntime(g_persistHash, true, std::move(e));
  }
}
void testOnMiss(G::ShaderCache&, uint64_t, bool, const void*, size_t) { g_onMiss++; }
const G::ShaderCacheRuntimeHooks kTestHooks{testOnLoaded, testOnMiss, nullptr, nullptr, nullptr};

// ── fixtures ─────────────────────────────────────────────────────────────────
struct Fixture {
  std::vector<uint64_t> vs, ps;  // the copied entries
  int files = 0;                 // stage-1 reads it costs (every .dxil / .vfetch / .vsf.dxil)
};

bool copyFile(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  fs::copy_file(a, b, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

// Complete VS entries (.vs.dxil + .vs.vfetch + .vs.vsf.dxil) and PS entries from src, in name order.
bool makeFixture(const fs::path& src, const fs::path& dst, size_t nVs, size_t nPs, Fixture& f) {
  std::error_code ec;
  fs::remove_all(dst, ec);
  fs::create_directories(dst, ec);
  std::vector<std::string> names;
  for (auto& e : fs::directory_iterator(src, ec)) names.push_back(e.path().filename().string());
  std::sort(names.begin(), names.end());
  for (const auto& n : names) {
    if (n.size() != 24) continue;  // <16 hex>.vs.dxil / .ps.dxil
    const uint64_t h = std::strtoull(n.substr(0, 16).c_str(), nullptr, 16);
    const std::string stem = n.substr(0, 16);
    if (n.compare(16, 8, ".vs.dxil") == 0 && f.vs.size() < nVs) {
      if (!fs::exists(src / (stem + ".vs.vfetch")) || !fs::exists(src / (stem + ".vs.vsf.dxil"))) continue;
      for (const char* ext : {".vs.dxil", ".vs.vfetch", ".vs.vsf.dxil"})
        if (!copyFile(src / (stem + ext), dst / (stem + ext))) return false;
      f.vs.push_back(h);
      f.files += 3;
    } else if (n.compare(16, 8, ".ps.dxil") == 0 && f.ps.size() < nPs) {
      if (!copyFile(src / n, dst / n)) return false;
      f.ps.push_back(h);
      f.files += 1;
    }
  }
  return f.vs.size() == nVs && f.ps.size() == nPs;
}

// A VS entry of src copied under another hash (the synthetic IM_LOAD's).
bool copyVsAs(const fs::path& src, uint64_t from, const fs::path& dst, uint64_t to) {
  for (const char* ext : {".vs.dxil", ".vs.vfetch", ".vs.vsf.dxil"})
    if (!copyFile(src / (hx(from) + ext), dst / (hx(to) + ext))) return false;
  return true;
}

void setCacheDir(const fs::path& d) { _putenv_s("SR_SHADER_CACHE_DIR", d.string().c_str()); }

// A minimal Xenos ucode the IM_LOAD structure gate accepts: control-flow pair 0 = one EXEC_END at
// address 1, count 1 (then a NOP); pair 1 = one ALU instruction whose first dword is `salt`.
std::vector<uint32_t> syntheticUcode(uint32_t salt) { return {0x00001001u, 0x00002000u, 0u, salt, 0u, 0u}; }
std::vector<uint8_t> beBytes(const std::vector<uint32_t>& w) {
  std::vector<uint8_t> b;
  for (uint32_t v : w) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
  }
  return b;
}

// ── T9: the CP's backend wait ───────────────────────────────────────────────
std::atomic<uint64_t> g_fakeNs{0};
std::atomic<uint32_t> g_phaseSeen{~0u};
uint64_t fakeNow() { return g_fakeNs.load(); }
void fakeSleep(uint32_t) {  // one simulated second per poll
  uint32_t none = ~0u;
  g_phaseSeen.compare_exchange_strong(none, K::gpuCpSnapshot().phase);
  g_fakeNs += 1000000000ull;
}
constexpr uint64_t kT9aAttachNs = 7200ull * 1000000000ull;  // two hours
bool fakeAttached() { return g_fakeNs.load() >= kT9aAttachNs; }
std::atomic<bool> g_t9bAttached{false};
bool t9bAttached() { return g_t9bAttached.load(); }
bool never() { return false; }
bool always() { return true; }
const char* childReason() { return "test: the D3D12 init failed (shader-boot-load-test child)"; }

// This process's thread count (T9f).
int processThreads() {
#ifdef _WIN32
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return -1;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  int n = 0;
  const DWORD me = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    if (te.th32OwnerProcessID == me) n++;
  CloseHandle(snap);
  return n;
#else
  return -1;
#endif
}

// The child half of T9a / T9c-f: `--shader-boot-load-test --child long|fatal|stall|player|helper`. Prints one
// CHILD line, exits.
int runChild(const char* mode) {
  _putenv_s("SR_INSTANCE_ID", "shader-boot-load-test-child");  // an automated run: never a message box
  K::BackendWaitProbe p;
  if (!std::strcmp(mode, "long")) {
    p.attached = fakeAttached;
    p.failed = never;
    p.reason = childReason;
    p.nowNs = fakeNow;
    p.sleepMs = fakeSleep;
    p.warnEveryMs = 600000;  // every 10 simulated minutes
    const uint32_t before = K::gpuCpSnapshot().phase;
    const uint64_t ns = K::gpuCpWaitForBackend(p);
    std::printf("CHILD long: waited %.0f s, warns %llu, phase before %u during %u after %u\n", double(ns) / 1e9,
                (unsigned long long)K::gpuCpBackendWaitWarnsForTest(), before, g_phaseSeen.load(),
                K::gpuCpSnapshot().phase);
  } else if (!std::strcmp(mode, "stall")) {
    // T9d: the real watchdog on a short escalation schedule (300 ms, then every 700 ms) over a real-clock backend
    // wait that the attach ends after 2.6 s. It must escalate as a host-wait stall, never as a guest hang, and the
    // wait must not proceed before the attach.
    K::gpuHangWdHostWaitScheduleForTest(300000000ull, 700000000ull);
    K::gpuHangWdStart();
    std::thread attacher([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2600));
      g_t9bAttached = true;
    });
    p.attached = t9bAttached;
    p.failed = never;
    p.reason = childReason;
    const uint64_t ns = K::gpuCpWaitForBackend(p);
    attacher.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // the watchdog's ticks after the wait: no more
    std::printf("CHILD stall: waited %.0f ms, host-wait stalls %llu\n", double(ns) / 1e6,
                (unsigned long long)K::gpuHangWdHostWaitStallsForTest());
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);  // the watchdog thread runs on: no static teardown under it
  } else if (!std::strcmp(mode, "player")) {
    // T9e: the player log profile. 70 host-wait reports (more than the 64-line Error cap a non-milestone format
    // site gets), then 3 hang reports (an episode's start, +30 s, +120 s): every section of every report must be
    // kept - both tags are literal milestones, and the host reports must not spend the hang report's budget.
    _putenv_s("SR_LOG", "player");
    sr::Log::init(nullptr);
    for (int i = 0; i < 70; i++) K::gpuHangWdReportForTest(true);
    for (int i = 0; i < 3; i++) K::gpuHangWdReportForTest(false);
    std::printf("CHILD player: %d host-wait reports, %d hang reports\n", 70, 3);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
  } else if (!std::strcmp(mode, "helper")) {
    // T9f: a lock the report's helper needs is held across many escalations: at most one helper thread may be
    // started (the later reports skip its queries and say so), and the process's thread count stays bounded.
    static std::mutex s_held;
    s_held.lock();
    K::gpuHangWdHelperBodyForTest(+[] {
      std::lock_guard<std::mutex> lk(s_held);  // blocks while the test holds it
    });
    const int threads0 = processThreads();
    K::gpuHangWdHostWaitScheduleForTest(100000000ull, 200000000ull);
    K::gpuHangWdStart();
    std::atomic<int> peak{threads0};
    std::thread attacher([&peak] {
      for (int i = 0; i < 30; i++) {  // 3 s, sampling the thread count
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        peak = std::max(peak.load(), processThreads());
      }
      g_t9bAttached = true;
    });
    p.attached = t9bAttached;
    p.failed = never;
    p.reason = childReason;
    (void)K::gpuCpWaitForBackend(p);
    attacher.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // a report in flight at the attach finishes
    uint64_t started = 0, skipped = 0;
    K::gpuHangWdHelperStatsForTest(&started, &skipped);
    std::printf("CHILD helper: escalations %llu, helpers started %llu, skipped %llu, threads %d -> peak %d\n",
                (unsigned long long)K::gpuHangWdHostWaitStallsForTest(), (unsigned long long)started,
                (unsigned long long)skipped, threads0, peak.load());
    std::fflush(stdout);
    std::fflush(stderr);
    s_held.unlock();
    std::_Exit(0);
  } else if (!std::strcmp(mode, "fatal")) {
    p.attached = never;
    p.failed = always;
    p.reason = childReason;
    (void)K::gpuCpWaitForBackend(p);
    std::printf("CHILD fatal: RETURNED (the init failure did not end the process)\n");
  } else {
    std::printf("CHILD %s: unknown mode\n", mode);
    return 2;
  }
  std::fflush(stdout);
  return 0;
}

#ifdef _WIN32
// Runs this exe as `--shader-boot-load-test --child <mode>` with stdout + stderr into `out`; returns its exit code
// (-1 = not started, -2 = still running after 60 s, then killed) and its output in `text`.
int spawnChild(const char* mode, const fs::path& out, std::string& text) {
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return -1;
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --shader-boot-load-test --child ";
  for (const char* m = mode; *m; m++) cmd += wchar_t(*m);
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE h = CreateFileW(out.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = h;
  si.hStdError = h;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> line(cmd.begin(), cmd.end());
  line.push_back(0);
  const BOOL ok = CreateProcessW(exe, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(h);
  if (!ok) return -1;
  int rc = 0;
  if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 98);
    WaitForSingleObject(pi.hProcess, 5000);
    rc = -2;
  } else {
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    rc = int(code);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  const auto bytes = readAll(out);
  text.assign(bytes.begin(), bytes.end());
  return rc;
}

HWND hiddenWindow() {  // never shown: no focus, nothing on screen
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SrShaderBootLoadTest";
  RegisterClassW(&wc);
  return CreateWindowExW(0, wc.lpszClassName, L"shader boot load test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                         nullptr, nullptr, wc.hInstance, nullptr);
}
#endif
}  // namespace

int runShaderBootLoadTest(int argc, char** argv) {
  fs::path work = "archive/shader_boot_load_test", source = "data/shader_cache/dxil";
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--child")) return runChild(argv[i + 1]);  // T9a / T9c's other process
    if (!std::strcmp(argv[i], "--work")) work = argv[i + 1];
    else if (!std::strcmp(argv[i], "--source")) source = argv[i + 1];
  }
  std::printf("[shader-boot-load-test] source %s, work %s, SR_SHADER_LOAD_ASYNC=%d\n", source.string().c_str(),
              work.string().c_str(), G::ShaderCache::bootLoadAsync() ? 1 : 0);
  std::error_code ec;
  fs::create_directories(work, ec);
  // T8's backend init reads / first-start-writes the player's settings (display.ini) under the content root:
  // keep them in the work dir, never in %LOCALAPPDATA%.
  _putenv_s("SR_CONTENT_DIR", fs::absolute(work / "content", ec).string().c_str());
  G::ShaderCache::setReadHookForTest(readHook);
  G::ShaderCache::setRuntimeHooks(&kTestHooks);
  // Caches are never freed: a freed cache's address could come back for the next one, and the
  // per-thread find memo is keyed by the cache's address.

  // ── T1 ──
  std::printf("T1 the installed-dir probe\n");
  {
    const fs::path d = work / "t1";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    check(!G::ShaderCache::probeInstalledDir(work / "t1_missing"), "a missing dir: no cache");
    check(!G::ShaderCache::probeInstalledDir(d), "an empty dir: no cache");
    auto put = [&](const char* name, size_t bytes) {
      std::ofstream o(d / name, std::ios::binary);
      const std::string s(bytes, 'x');
      o.write(s.data(), std::streamsize(s.size()));
    };
    put("0000000000000001.vs.vsf.dxil", 64);
    put("0000000000000002.vs.vfetch", 64);
    put("0000000000000003.ps.dxil", 0);
    put("readme.dxil", 64);
    check(!G::ShaderCache::probeInstalledDir(d), "only .vsf.dxil / .vfetch / zero-byte / short names: no cache");
    put("0000000000000004.ps.dxil", 64);
    check(G::ShaderCache::probeInstalledDir(d), "one non-empty <hash>.ps.dxil: a cache");
    check(G::ShaderCache::probeInstalledDir(source), "the source cache: a cache");
  }

  Fixture base;
  const fs::path fx = work / "fx";
  const bool haveFx = makeFixture(source, fx, 8, 8, base);
  check(haveFx, "fixture: 8 VS + 8 PS copied from the source", fx.string());
  if (!haveFx) {
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? g_fail : 1;
  }

  // ── T2 ──
  std::printf("T2 decoupling: beginBootLoad does not wait for a slow disk\n");
  {
    hookReset();
    g_readDelayMs = 40;
    setCacheDir(fx);
    auto* c = new G::ShaderCache();
    const auto t0 = Clock::now();
    c->beginBootLoad();
    const double begin = msSince(t0);
    const auto t1 = Clock::now();
    const bool present = c->installedPresent();
    const double ask = msSince(t1);
    const bool loadedEarly = c->installedLoaded();
    check(begin < 250.0, "beginBootLoad returns at once", fmtMs("%.1f ms", begin));
    check(present && ask < 50.0 && !loadedEarly, "installedPresent answers from the probe while stage 1 runs",
          fmtMs("present %.0f, %.1f ms", present ? 1.0 : 0.0, ask));
    c->ensureInstalledLoaded();
    const double load = msSince(t0);
    const double expect = 0.8 * base.files * 40.0;
    check(load >= expect && c->vsCount() == base.vs.size() && c->psCount() == base.ps.size(),
          "stage 1 takes the slowed time and loads the fixture", fmtMs("%.0f ms (>= %.0f expected)", load, expect));
    c->ensureLoaded();
  }

  // ── T3 ──
  std::printf("T3 WP2: a lookup during the load blocks, then answers from the complete cache\n");
  {
    hookReset();
    g_readDelayMs = 40;
    setCacheDir(fx);
    auto* c = new G::ShaderCache();
    c->startLoadAsync();
    const G::CachedShader* v = nullptr;
    const G::CachedShader* p = nullptr;
    bool stage1AtReturn = false;
    double waited = 0.0;
    mustEnd("T3 reader", 30000, [&] {
      const auto t = Clock::now();
      v = c->findVs(base.vs.back());
      p = c->findPs(base.ps.back());
      waited = msSince(t);
      stage1AtReturn = c->installedLoaded();
    });
    check(waited >= 200.0 && stage1AtReturn, "the reader blocked until stage 1 was published",
          fmtMs("%.0f ms", waited));
    check(v && v->dxil == readAll(fx / (hx(base.vs.back()) + ".vs.dxil")) &&
              v->dxilFetch == readAll(fx / (hx(base.vs.back()) + ".vs.vsf.dxil")) && !v->layout.empty(),
          "findVs answers the entry built from that file (DXIL + vsf + layout), not a miss");
    check(p && p->dxil == readAll(fx / (hx(base.ps.back()) + ".ps.dxil")), "findPs answers the entry of that file");
    c->ensureLoaded();
  }

  // ── T12 (step 4 lane C, tools/xenosrecomp/0005) ──
  std::printf("T12 the alpha-test variant <hash>.ps.at.dxil: attached to its PS, never an entry of its own\n");
  {
    hookReset();
    const fs::path fa = work / "fx_at";
    Fixture fb;
    const bool ok = makeFixture(source, fa, 8, 8, fb);
    // Stand-in variant bytes: another PS's DXIL (the loader keeps the bytes; it does not parse a variant).
    const std::vector<uint8_t> variant = readAll(fa / (hx(fb.ps[1]) + ".ps.dxil"));
    {
      std::ofstream o(fa / (hx(fb.ps[0]) + ".ps.at.dxil"), std::ios::binary);
      o.write(reinterpret_cast<const char*>(variant.data()), std::streamsize(variant.size()));
    }
    {
      std::ofstream o(fa / "00000000000000ab.ps.at.dxil", std::ios::binary);  // a variant without its PS
      o.write(reinterpret_cast<const char*>(variant.data()), std::streamsize(variant.size()));
    }
    setCacheDir(fa);
    auto* c = new G::ShaderCache();
    c->ensureLoaded();
    const G::CachedShader* p0 = c->findPs(fb.ps[0]);
    const G::CachedShader* p1 = c->findPs(fb.ps[1]);
    check(ok && !variant.empty() && p0 && p0->dxil == readAll(fa / (hx(fb.ps[0]) + ".ps.dxil")) &&
              p0->dxilAlphaTest == variant && bool(p0->nativeAlphaTestCode),
          "a PS with a .ps.at.dxil carries it (bytes + native code) beside its own DXIL");
    check(p1 && p1->dxilAlphaTest.empty() && !p1->nativeAlphaTestCode, "a PS without one carries none");
    check(c->psCount() == fb.ps.size() && !c->findPs(0xabull),
          "the variants are not entries: the PS count is unchanged, a variant without its PS is no shader");
    G::CachedShader e;
    check(G::ShaderCache::loadEntryFromDir(fa, fb.ps[0], false, e) && e.dxilAlphaTest == variant,
          "loadEntryFromDir (the runtime translator's persisted entries) reads the variant too");
    const fs::path d = work / "t12_only_variant";
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    {
      std::ofstream o(d / "0000000000000005.ps.at.dxil", std::ios::binary);
      o.write(reinterpret_cast<const char*>(variant.data()), std::streamsize(variant.size()));
    }
    check(!G::ShaderCache::probeInstalledDir(d), "a dir holding only .ps.at.dxil files: no cache (the probe agrees)");
  }

  // ── T4 ──
  std::printf("T4 WP2: the translator stage - hits do not wait for it, misses do\n");
  {
    hookReset();
    setCacheDir(fx);
    g_persistSrcDir = fx;
    g_persistSrcHash = base.vs[0];
    g_persistHash = 0x0123456789ABCDEFull;  // in no installed cache
    g_stage2SleepMs = 1500;
    g_onMiss = 0;
    auto* c = new G::ShaderCache();
    c->startLoadAsync();
    c->ensureInstalledLoaded();
    const auto t0 = Clock::now();
    const G::CachedShader* hit = c->findVs(base.vs[1]);
    const double hitMs = msSince(t0);
    const bool stage2Running = !c->fullyLoaded();
    check(hit && hitMs < 500.0 && stage2Running, "an installed hit returns while stage 2 still runs",
          fmtMs("%.1f ms, stage 2 running %.0f", hitMs, stage2Running ? 1.0 : 0.0));
    const auto t1 = Clock::now();
    const G::CachedShader* persisted = c->findVs(g_persistHash);
    const double missMs = msSince(t1);
    check(persisted && persisted->dxil == readAll(fx / (hx(base.vs[0]) + ".vs.dxil")) && c->fullyLoaded(),
          "an installed miss waits for stage 2 and finds the persisted translation",
          fmtMs("%.0f ms", missMs));
    check(missMs >= 500.0, "that wait is stage 2's (the miss blocked)", fmtMs("%.0f ms", missMs));
    const uint64_t genuine = 0xDEADBEEF00C0FFEEull;
    const auto u = beBytes(syntheticUcode(7));
    const bool none = c->findVs(genuine) == nullptr;
    c->requestTranslation(genuine, true, u.data(), u.size());
    check(none && g_onMiss.load() == 1, "a genuine miss after stage 2 reaches the translator's onMiss");
    g_persistHash = 0;
    g_stage2SleepMs = 0;
    c->ensureLoaded();
  }

  // ── T5 ──
  std::printf("T5 failures: unreadable cache, a throwing read, the loader's own lookups\n");
  {
    hookReset();
    g_failReads = 1;
    setCacheDir(fx);
    auto* c = new G::ShaderCache();
    c->beginBootLoad();
    const bool before = c->installedPresent();
    mustEnd("T5 unreadable: stage 1", 30000, [&] { c->ensureInstalledLoaded(); });
    const bool after = c->installedPresent();
    check(before && !after && c->vsCount() == 0 && c->psCount() == 0,
          "every read failing: the probe said yes, installedPresent() turns false at stage 1");
    mustEnd("T5 unreadable: stage 2", 30000, [&] { c->ensureLoaded(); });

    hookReset();
    g_throwAtRead = 3;
    auto* t = new G::ShaderCache();
    t->startLoadAsync();
    mustEnd("T5 throwing read", 30000, [&] { t->ensureLoaded(); });
    check(t->installedLoaded() && t->fullyLoaded(), "a read that throws: both stages still published, no hang");

    hookReset();
    g_selfLookup = 1;
    g_selfLookupDone = 0;
    auto* s = new G::ShaderCache();
    s->startLoadAsync();
    mustEnd("T5 loader self-lookup", 30000, [&] { s->ensureLoaded(); });
    check(g_selfLookupDone.load() == 1, "the loader's own hook looks up a miss without waiting on itself");
    g_selfLookup = 0;
  }

  // ── T10 ──
  std::printf("T10 stage 1's end time is sampled before its publication (the loader descheduled after it)\n");
  {
    hookReset();
    g_readDelayMs = 20;
    setCacheDir(fx);
    G::ShaderCache::setPublishHookForTest(publishHook);
    g_publishDelayMs = 400;  // the loader stalls 400 ms right after publishing stage 1
    auto* c = new G::ShaderCache();
    c->startLoadAsync();
    double woke = -1.0;
    mustEnd("T10 reader", 30000, [&] {
      c->ensureInstalledLoaded();
      woke = G::msSinceProcessStart();  // released by the publication
    });
    mustEnd("T10 whole load", 30000, [&] { c->ensureLoaded(); });
    G::ShaderCache::setPublishHookForTest(nullptr);
    g_publishDelayMs = 0;
    const double end = c->installedEndMs();
    check(end > 0.0 && woke > 0.0 && end <= woke,
          "T10 a reader released by stage 1's publication never sees a later stage-1 end (the witness stays sound)",
          fmtMs("end +%.1f ms, reader woke +%.1f ms", end, woke));
  }

  // ── T7 ──
  std::printf("T7 the hang watchdog in a host wait: H0-H2 fire (annotated), only the CP's own stall is set aside\n");
  {
    constexpr uint64_t S = 1000000000ull;
    K::GpuHangSnap s{};
    s.nowNs = 1000 * S;
    s.cpStarted = true;
    s.cpPhase = 1;
    s.cpPhaseSinceNs = s.nowNs - 6 * S;
    s.published = true;
    s.rptr = 0;
    s.wptr = 4;
    s.fenceValid = true;
    s.completed = 5;
    s.completedSinceNs = s.nowNs - 6 * S;
    s.issuedValid = true;
    s.lastIssued = 7;
    const uint32_t busy = K::gpuHangEvaluate(s);
    check((busy & K::kGhH1) && (busy & K::kGhH3), "T7b phase 1: H1 + H3 fire (the control)");
    // Astra round 3's example: the fence stops 2 s before WP2 begins; 10 s into the wait, H1 must fire. So must a
    // fence stall that begins after the wait, and a present stall - no timing decides attribution.
    s.cpPhase = K::kCpPhaseHostWait;
    s.cpPhaseSinceNs = s.nowNs - 10 * S;
    s.completedSinceNs = s.nowNs - 12 * S;
    const uint32_t before = K::gpuHangEvaluate(s);
    s.completedSinceNs = s.nowNs - 4 * S;  // flat since 6 s INTO the wait
    s.presents = 40;
    s.presentsSinceNs = s.nowNs - 11 * S;
    s.threadInGpuWait = true;
    const uint32_t during = K::gpuHangEvaluate(s);
    check((before & K::kGhH1) && (during & K::kGhH1) && (during & K::kGhH2),
          "T7a a fence / present stall during a host wait still fires (H1, H2), whenever it began",
          fmtMs("masks %.0f / %.0f", double(before), double(during)));
    check(!(before & K::kGhH3) && !(during & K::kGhH3), "T7b ... and the CP's own stall trigger H3 is set aside");
    char note[256] = "";
    const bool annotated = K::gpuHangHostWaitNote(K::kCpPhaseHostWait, K::kCpHostWaitInstalled, s.cpPhaseSinceNs,
                                                  s.nowNs, note, sizeof(note));
    char none[64] = "";
    const bool outside = K::gpuHangHostWaitNote(1, 0, s.cpPhaseSinceNs, s.nowNs, none, sizeof(none));
    check(annotated && !outside && std::strstr(note, "10.0 s for the installed shader cache") &&
              std::strstr(note, "host wait"),
          "T7d the [GPU_HANG] report fired in a host wait is annotated with the wait (which, how long)", note);
    // The escalation schedule over 2 hours of one host wait, polled every 250 ms as the watchdog does.
    uint32_t fired = 0, count = 0;
    double firstAt = -1.0, lastAt = -1.0, minGap = 1e9;
    for (uint64_t t = 0; t <= 7200 * S; t += S / 4) {
      if (K::gpuHangHostWaitDue(t, fired)) {
        const double at = double(t) / double(S);
        if (firstAt < 0) firstAt = at;
        if (lastAt >= 0) minGap = std::min(minGap, at - lastAt);
        lastAt = at;
        count++;
      }
    }
    check(firstAt >= 60.0 && firstAt < 60.5 && count == 24 && minGap >= 299.9 && fired == 24,
          "T7c a host wait escalates at 60 s, then every 5 min (24 in 2 h), the wait itself unchanged",
          fmtMs("first %.2f s, count", firstAt) + " " + std::to_string(count));
  }

  // ── T11 ──
  std::printf("T11 the CP's first command is recorded where it is consumed (its words may arrive late)\n");
  {
    static double s_fakeMs = 0.0;
    const auto fakeMs = +[] { return s_fakeMs; };
    K::GpuCpFirstCommand first;
    s_fakeMs = 100.0;  // a packet needing 5 dwords, 2 published: nothing is consumed
    const bool early = K::gpuCpTakeCommand(5, 2, 2, fakeMs, first);
    const bool earlyTaken = first.taken;
    s_fakeMs = 250.0;  // the rest arrives
    const bool late = K::gpuCpTakeCommand(5, 5, 2, fakeMs, first);
    s_fakeMs = 400.0;
    const bool next = K::gpuCpTakeCommand(1, 9, 2, fakeMs, first);
    check(!early && !earlyTaken && late && next && first.taken && first.atMs == 250.0 && first.kind == 2,
          "T11 a first ring packet whose words arrive late is recorded when they do, once",
          fmtMs("taken at %.0f ms (words complete at 250)", first.atMs));
    K::GpuCpFirstCommand item;
    s_fakeMs = 50.0;  // a host item whose index space (3) is not yet published
    const bool itemEarly = K::gpuCpTakeCommand(3, 0, 1, fakeMs, item);
    s_fakeMs = 80.0;
    const bool itemLate = K::gpuCpTakeCommand(3, 3, 1, fakeMs, item);
    check(!itemEarly && itemLate && item.atMs == 80.0 && item.kind == 1,
          "T11 ... and so is a first host item", fmtMs("taken at %.0f ms (published at 80)", item.atMs));
  }

  // ── T9 ──
  std::printf("T9 the CP's wait for the live backend: no time limit; an init failure is fatal\n");
  {
#ifdef _WIN32
    std::string out;
    const int rcLong = spawnChild("long", work / "t9a_child.txt", out);
    double secs = -1.0;
    unsigned long long warns = ~0ull;
    unsigned before = ~0u, during = ~0u, after = ~0u;
    const size_t at = out.find("CHILD long:");
    const bool parsed = at != std::string::npos &&
                        std::sscanf(out.c_str() + at, "CHILD long: waited %lf s, warns %llu, phase before %u during %u after %u",
                                    &secs, &warns, &before, &during, &after) == 5;
    check(rcLong == 0 && parsed && secs >= 7199.0 && secs <= 7201.0,
          "T9a no attach yet, init not failed: waits 2 simulated hours (no 60 s limit, no exit), returns on the attach",
          fmtMs("child exit %.0f, waited %.0f s", double(rcLong), secs));
    check(parsed && warns == 11 && out.find("CP still waiting for the live backend after 600 s") != std::string::npos &&
              out.find("(attached), 11 still-waiting warnings") != std::string::npos,
          "T9a ... a still-waiting Warn every period (11 in 2 h at 10 min), and the total wait logged on the attach",
          fmtMs("%.0f warns", double(warns)));
    check(parsed && during == K::kCpPhaseHostWait && after == before,
          "T9a ... in CP phase 5 (host wait) while waiting, the phase before it restored after",
          fmtMs("during %.0f, after %.0f", double(during), double(after)));
    if (!parsed) std::printf("    child output (%s):\n%s\n", (work / "t9a_child.txt").string().c_str(), out.c_str());

    const int rcFatal = spawnChild("fatal", work / "t9c_child.txt", out);
    const bool fatalLines = out.find("(INIT FAILED)") != std::string::npos &&
                            out.find("[FATAL] the live D3D12 backend is not there: test: the D3D12 init failed") !=
                                std::string::npos;
    check(rcFatal == 3 && fatalLines && out.find("RETURNED") == std::string::npos,
          "T9c an explicit D3D12 init failure: INIT FAILED, [FATAL] with its reason, exit 3",
          fmtMs("child exit %.0f, lines %.0f", double(rcFatal), fatalLines ? 1.0 : 0.0));
    if (rcFatal != 3) std::printf("    child output (%s):\n%s\n", (work / "t9c_child.txt").string().c_str(), out.c_str());

    const int rcStall = spawnChild("stall", work / "t9d_child.txt", out);
    double stallMs = -1.0;
    unsigned long long stalls = 0;
    const size_t sat = out.find("CHILD stall:");
    const bool sparsed = sat != std::string::npos &&
                         std::sscanf(out.c_str() + sat, "CHILD stall: waited %lf ms, host-wait stalls %llu", &stallMs,
                                     &stalls) == 2;
    const bool stallLines = out.find("[HOST_WAIT_STALL] ==== the CP has waited") != std::string::npos &&
                            out.find("for the live backend's attach") != std::string::npos &&
                            out.find("[HOST_WAIT_STALL] (h) backend init:") != std::string::npos &&
                            out.find("[HOST_WAIT_STALL] ==== end") != std::string::npos;
    check(rcStall == 0 && sparsed && stalls >= 2 && stallLines,
          "T9d a long host wait escalates on schedule: [HOST_WAIT_STALL] blocks with the init and loader state",
          fmtMs("child exit %.0f, escalations %.0f", double(rcStall), double(stalls)));
    check(sparsed && stallMs >= 2500.0 && out.find("[GPU_HANG]") == std::string::npos &&
              out.find("(attached)") != std::string::npos,
          "T9d ... never as a guest hang ([GPU_HANG]), and the wait never proceeds before the attach",
          fmtMs("waited %.0f ms", stallMs));
    if (!sparsed || rcStall != 0)
      std::printf("    child output (%s):\n%s\n", (work / "t9d_child.txt").string().c_str(), out.c_str());

    // T9e: the player profile keeps every line of repeated reports.
    const int rcPlayer = spawnChild("player", work / "t9e_child.txt", out);
    const auto count = [&](const char* needle) {
      size_t n = 0;
      for (size_t at = out.find(needle); at != std::string::npos; at = out.find(needle, at + 1)) n++;
      return n;
    };
    const size_t hEnd = count("[HOST_WAIT_STALL] ==== end"), hHead = count("[HOST_WAIT_STALL] ==== the CP has waited"),
                 hCp = count("[HOST_WAIT_STALL] (e) CP phase="), hInit = count("[HOST_WAIT_STALL] (h) backend init"),
                 hFence = count("[HOST_WAIT_STALL] (c) fence block"), hCs = count("[HOST_WAIT_STALL_CS]");
    const size_t gHead = count("[GPU_HANG] ==== trigger"), gEnd = count("[GPU_HANG] ==== end"),
                 gCp = count("[GPU_HANG] (e) CP phase="), gFence = count("[GPU_HANG] (c) fence block"),
                 gCs = count("[GPU_HANG_CS]");
    char counts[256];
    std::snprintf(counts, sizeof(counts),
                  "host: %zu heads / %zu (e) / %zu (h) / %zu (c) / %zu ends / %zu CS; hang: %zu heads / %zu (e) / %zu "
                  "(c) / %zu ends / %zu CS",
                  hHead, hCp, hInit, hFence, hEnd, hCs, gHead, gCp, gFence, gEnd, gCs);
    check(rcPlayer == 0 && out.find("[LOG_PROFILE] player") != std::string::npos && hHead == 70 && hCp == 70 &&
              hInit == 70 && hFence == 70 && hEnd == 70 && hCs == 1,
          "T9e SR_LOG=player: 70 [HOST_WAIT_STALL] reports keep every section (literal milestone tags)", counts);
    check(gHead == 3 && gCp == 3 && gFence == 3 && gEnd == 3 && gCs == 3,
          "T9e ... and the [GPU_HANG] reports after them lose nothing, the CS dump's summary line included (no "
          "thread / CS rows exist in this child)");

    // T9f: a held lock across many escalations: one helper thread at most.
    const int rcHelper = spawnChild("helper", work / "t9f_child.txt", out);
    unsigned long long esc = 0, hStarted = 0, hSkipped = 0;
    int thr0 = 0, thrPeak = 0;
    const size_t hat = out.find("CHILD helper:");
    const bool hparsed =
        hat != std::string::npos &&
        std::sscanf(out.c_str() + hat, "CHILD helper: escalations %llu, helpers started %llu, skipped %llu, threads %d -> peak %d",
                    &esc, &hStarted, &hSkipped, &thr0, &thrPeak) == 5;
    char hs[160];
    std::snprintf(hs, sizeof(hs), "escalations %llu, helpers started %llu, skipped %llu, threads %d -> peak %d", esc,
                  hStarted, hSkipped, thr0, thrPeak);
    check(rcHelper == 0 && hparsed && esc >= 4 && hStarted == 1 && hSkipped + 1 == esc &&
              out.find("helper SKIPPED") != std::string::npos,
          "T9f a lock held across escalations: one helper thread at most, the later reports skip it and say so", hs);
    check(hparsed && thr0 > 0 && thrPeak - thr0 <= 4,
          "T9f ... and the process's thread count stays bounded (watchdog, one helper, the test's attacher)", hs);
    if (!hparsed || rcHelper != 0)
      std::printf("    child output (%s):\n%s\n", (work / "t9f_child.txt").string().c_str(), out.c_str());
#endif
    // T9b, in this process on the real clock: the backend attaches 150 ms in.
    g_t9bAttached = false;
    const uint32_t phaseBefore = K::gpuCpSnapshot().phase;
    std::atomic<uint32_t> phaseDuring{~0u};
    std::thread attacher([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      phaseDuring = K::gpuCpSnapshot().phase;
      g_t9bAttached = true;
    });
    K::BackendWaitProbe p;
    p.attached = t9bAttached;
    p.failed = never;
    p.reason = childReason;
    uint64_t ns = 0;
    mustEnd("T9b real-clock wait", 30000, [&] { ns = K::gpuCpWaitForBackend(p); });
    attacher.join();
    const double ms = double(ns) / 1e6;
    check(ms >= 120.0 && ms < 5000.0 && K::gpuCpBackendWaitWarnsForTest() == 0,
          "T9b on the real clock: returns on the attach with the time waited, no warning", fmtMs("%.0f ms", ms));
    check(phaseDuring.load() == K::kCpPhaseHostWait && K::gpuCpSnapshot().phase == phaseBefore,
          "T9b ... phase 5 while waiting, restored after");
  }

  // ── T8 + T6: the global cache, as the game uses it ──
  std::printf("T8 D3D12Backend::init returns before the slowed load; T6 the IM_LOAD wiring\n");
  bool backendMade = false;
  {
    hookReset();
    const fs::path f6 = work / "fx6";
    Fixture b6;
    const auto ucode = syntheticUcode(0x5EED1234u);
    const auto be = beBytes(ucode);
    const uint64_t hIm = G::ShaderCache::hashUcode(be.data(), be.size());
    const bool ok6 = makeFixture(source, f6, 8, 8, b6) && copyVsAs(f6, b6.vs[0], f6, hIm);
    check(ok6 && G::ucode::hasValidControlFlow(ucode.data(), ucode.size()),
          "T6 fixture: a synthetic VS the structure gate accepts, cached under its ucode hash", hx(hIm));
    setCacheDir(f6);
    g_readDelayMs = 100;  // stage 1 >= ~3.5 s
    g_stage2SleepMs = 1500;
    auto& cache = G::getShaderCache();
    double initMs = 0.0;
    bool stage1AtInit = true;
#ifdef _WIN32
    HWND hwnd = hiddenWindow();
    auto* backend = new G::D3D12Backend();  // never freed (no teardown at exit)
    const auto ti = Clock::now();
    const bool inited = hwnd && backend->init(hwnd, 320, 240);
    initMs = msSince(ti);
    stage1AtInit = cache.installedLoaded();
    backendMade = inited;
    if (inited) {
      check(!stage1AtInit, "T8 init returned before the slowed stage 1 was published (the load is off init's path)",
            fmtMs("init %.0f ms", initMs));
    } else {
      std::printf("  T8 NOT RUN: D3D12Backend::init failed on this machine (%.0f ms, stage 1 %s at its return; no "
                  "device or swap chain - see the [D3D12] lines; a swap chain needs an interactive session); T6 "
                  "starts the load as init would\n", initMs, stage1AtInit ? "published" : "still loading");
    }
#endif
    cache.beginBootLoad();  // what init does first (a no-op for the load when init already started it)
    auto eng = std::make_unique<G::Pm4Engine>();
    double imMs = 0.0;
    bool stage1AtIm = false, stage2AtIm = true;
    mustEnd("T6 IM_LOAD", 60000, [&] {
      const auto t = Clock::now();
      eng->applyImLoadImmediate(0u, uint32_t(ucode.size()), be.data(), uint32_t(ucode.size()) + 2u);
      imMs = msSince(t);
      stage1AtIm = cache.installedLoaded();
      stage2AtIm = cache.fullyLoaded();
    });
    const double whole1 = 0.8 * (b6.files + 3) * 100.0;
    check(stage1AtIm && imMs + initMs >= whole1 * 0.5, "T6 the IM_LOAD blocked until stage 1 was published",
          fmtMs("%.0f ms (+ init %.0f ms)", imMs, initMs));
    check(eng->lastVsUcodeHash == hIm && eng->xlatVsHits == 1 && eng->xlatMisses == 0,
          "T6 ... and counted a HIT for its hash (no miss, no translation request)",
          fmtMs("hits %.0f misses %.0f", double(eng->xlatVsHits), double(eng->xlatMisses)));
    check(!stage2AtIm, "T6 ... and returned while stage 2 still ran (the IM_LOAD does not wait for the translator)");
    const G::CachedShader* v = cache.findVs(hIm);
    check(v && v->dxil == readAll(f6 / (hx(hIm) + ".vs.dxil")), "T6 the draw's VS lookup answers that entry");
    if (backendMade)
      check(initMs < whole1, "T8 init took less than the slowed stage 1",
            fmtMs("init %.0f ms, stage 1 >= %.0f ms", initMs, whole1));
    mustEnd("T6 whole load", 60000, [&] { cache.ensureLoaded(); });
    g_stage2SleepMs = 0;
    hookReset();
  }

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  std::fflush(stdout);
  if (backendMade) std::_Exit(g_fail);  // a live D3D12 backend: no teardown at exit
  return g_fail;
}
