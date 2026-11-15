// TEST ORACLE ONLY (tools/difftest/log_async_test.cpp, app/log-async): app/log.cpp as of dev d71946e2 - the
// synchronous logger the asynchronous writer replaced - verbatim apart from the namespace (sr -> sr_logref)
// and the header name. The same-bytes case runs the same log calls through both and compares the files.
// Never linked into the game.
#include "log_legacy_ref.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <atomic>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#endif

namespace sr_logref {

static FILE* g_logFile = nullptr;
// iter-365u: lines written since the last flush (guarded by g_logSpinLock, so a
// plain int is fine) and how many we allow to sit in the 1MB stdio buffer.
static int g_sinceFlush = 0;
static constexpr int kFlushEvery = 256;
static constexpr int kFlushMs = 200;
static std::chrono::steady_clock::time_point g_lastFlush =
    std::chrono::steady_clock::now();
static std::atomic_flag g_logSpinLock = ATOMIC_FLAG_INIT;
static bool g_toConsole = true;

// iter-464 perf: the crash handler's tail is a fixed ring (it was a vector<string>
// that erased its front - a 64-element shift plus a heap allocation per line, inside
// the global lock). Lines longer than a slot are truncated in the TAIL only.
static constexpr int kTailSlot = 512;
static char g_tail[Log::kMaxTailLines][kTailSlot];
static int g_tailHead = 0;   // next slot to write
static int g_tailCount = 0;

// ---------------------------------------------------------------------------
// iter-464 perf lane: LOG PROFILES (docs/PERFORMANCE.md "Logging").
//
//   SR_LOG=dev     every line (the pre-464 behaviour; ~1.6M lines per 25 min).
//   SR_LOG=player  milestone lines only: the tags in kMilestoneTags (era markers the
//                  harnesses await, the run class, the gauges, perf, crash breadcrumbs),
//                  plus the FIRST few lines of every other Warn (4) / Error (64) call
//                  site so a player log still shows that something warned. Info and
//                  below are dropped BEFORE formatting (one cached lookup per line).
//   unset          dev when SR_INSTANCE_ID is set (every automated roll pins it, so all
//                  harness tooling keeps every line), player otherwise (a plain launch).
//
// Gate: SR_LOG. The decision per call site is cached by format-string pointer.
// ---------------------------------------------------------------------------
namespace {

enum class Profile : int { Dev = 0, Player = 1 };

Profile profileFromEnv() {
  const char* e = std::getenv("SR_LOG");
  if (e && e[0]) {
    if (std::strcmp(e, "player") == 0 || std::strcmp(e, "0") == 0) return Profile::Player;
    return Profile::Dev;  // "dev", "1", anything else: everything
  }
  const char* inst = std::getenv("SR_INSTANCE_ID");
  return (inst && inst[0]) ? Profile::Dev : Profile::Player;
}

Profile g_profile = Profile::Dev;  // set by Log::init (before any guest thread)

// Lines a PLAYER log keeps (and that the harnesses can await in a player-profile roll).
// Match = the format string starts with "[" + tag followed by ']' or ' ' or '#'.
const char* const kMilestoneTags[] = {
    // run identity / gates / build
    "LOG_PROFILE", "RUN_CLASS", "GATES", "BUILD", "TIMER_RES", "GAUGE", "MOD", "MODS", "MOD_OVERLAY",
    // perf telemetry
    "PERF", "HOST_PROFILE", "GUEST_PROFILE", "NATIVE_FRAMESPLIT", "GPU_CP_TIME", "PM4_SWAP_TIME",
    "PM4_BIND_CENSUS", "PM4_SHM_FRAME", "MIRROR_DEMAND", "HIGHFPS", "HIGHFPS_PACE", "PM4_GPU_TIME",
    "MOVIE_PACE", "SIM_FREEZE", "SIM_FREEZE_DUMP",   // iter-466 hfr: the clock-inventory diagnostic
    "AB_TOGGLE", "CONST_SKIP",                       // iter-467 guestcode: in-roll A/B bits, upload skip
    "AB_PRESENT",                                    // iter-467 native-a: the per-present flip (SR_AB_PRESENT) while the sim is held
    "AB_DRAIN",                                      // iter-467 native-a: the drained per-present flip (SR_AB_PRESENT_DRAINED) waits / postponements
    "SHTIME_OBS", "SHTIME_HOLD",                     // iter-467 native-a: the wall-clock shader time 0x29 (SR_TIMEBASE_OBS, 2 s) and its sim hold (per hold)
    "NATIVE_DEVCREATE",                              // iter-467 native-a: boot.device_create's ring-init line + census counters
    "P2_TRACE",                                      // iter-467 native-a: the fence / ring / answer trace census (SR_P2_TRACE, 10 s)
    "GPU_CP", "RINGLESS",                            // iter-467 native-a: the CP census (120 swaps; zpd / ext / ringless counts) + P2-A line
    "GPU_COHER",                                     // native/coher-sentinel: the coherency requests the CP applied (beside each census)
    "LEAF",                                          // iter-467 native-a: SR_NATIVE_LEAF_TRACE per-call lines of the row-3 / 5 / 3b sites (rate-limited)
    "AUDIO_TIMING", "AUDIO_OUT_SUM", "AUDIO_DEV",    // iter-467 sound lane: output health (5 s; device events)
    "AUDIO_NATIVE",                                  // iter-467 N6: guest XMA-register access census (30 s)
    "CP_GATE", "SHM_WT",                             // iter-466 cp lane: gate witnesses, write tracking
    "VD_SYSBUF", "VD_SCANOUT", "VD_RING",            // iter-467 p2: VdGetSystemCommandBuffer / scan-out registers / ring from the argument
    "NATIVE_METER", "NATIVE_METER_RUN", "NATIVE_TRANSPORT",              // iter-467 the native meter (owner-facing number)
    "NATIVE_BUILDER_CENSUS", "NATIVE_BUILDER_REDUNDANCY", "NATIVE_LEDGER", "CONST_DEDUP", "NATIVE_FLUSH", "NATIVE_FLUSH_BAD", "NATIVE_FLUSH_DROP", "NATIVE_EXEC_PROF", "DRAW_LEDGER", "NATIVE_DRAIN_CENSUS", "NATIVE_METER_KINDS", "NATIVE_EXEC", "NATIVE_TILE_CENSUS", "NATIVE_DRAIN_REUSE",  // iter-467 native A diagnostics
    "PM4_COV", "PM4_COVMAP", "NATIVE_BRACKETS", "NATIVE_CPTIME",  // iter-467 native A P0: the PM4 producer census (SR_NATIVE_BRACKETS)
    "NB_REC", "NB_REC_BAD", "NB_NANWATCH",           // iter-467 native-b P1 row 9: the record / dry-CHECK counters (SR_NB_REC, SR_NB_REC_CHECK)
    "VPK_F16_4",  // iter-467 p2: the FLOAT16_4 production-input census (kernel/observe_vpk_f16.cpp)
    "HITCH", "HITCH_SUM", "HITCH_IO", "ASYNC_READ", "DIAG_READ_LATENCY", "GAMMA_OBS",  // iter-466 stutter lane
    // port features / player build identity (docs/PLAYER_BUILD.md)
    "PORT", "RES_SCALE", "DISPLAY", "STDIO_GUARD", "MODMENU", "SHADER_CACHE",
    // input receipts the scenario tools read (MOUSE_UI only exists under SR_MOUSE_UI_LOG=1)
    "MOUSE_UI", "INPUT", "PC_CONTROLS",
    // era markers (tools/perf/perf_report.py, sr_drive scenarios)
    "NAT_FE", "APT_LOADMOVIE", "APT_REQ", "APT_SCREEN", "FE_SETSCREEN", "XAM", "XAM_PROFILE",
    "KBPAD_OBS", "SR_FSCMD", "DEVTOOL", "OBJSTATE",
    // failure breadcrumbs
    "CRASH", "BUGCHECK", "KERNEL_CRASH", "ICALL_FAIL", "GPU_HANG", "GPU_CP_STALL", "FATAL",
    "HOST_WAIT_STALL",   // render/boot-attach: the watchdog's host-wait escalation block (kernel/gpu_hang_wd.cpp)
    // R1 band-aid hooks on generated code (kernel/r1_hooks.hpp): every hook shares ONE call
    // site, so the per-site Warn cap (4) would hide all but the first few fires in a player
    // roll; the fire log is already sparse (#1, #2, #4, ... per hook) and the burndown counts it
    "R1_FIRE",
    // capture tooling a player may switch on
    "PRESENT_DUMP", "FRAME_DUMP", "TRACE_HOTKEY", "HLE_TRACE",
};

bool isMilestone(const char* fmt) {
  if (!fmt || fmt[0] != '[') return false;
  const char* t = fmt + 1;
  for (const char* m : kMilestoneTags) {
    const size_t n = std::strlen(m);
    if (std::strncmp(t, m, n) == 0) {
      const char c = t[n];
      if (c == ']' || c == ' ' || c == '#') return true;
    }
  }
  return false;
}

struct FmtSlot {
  std::atomic<const char*> key{nullptr};
  std::atomic<uint8_t> cls{0};  // 0 = not yet classified, 1 = normal, 2 = milestone
  std::atomic<uint32_t> emitted{0};
};
constexpr size_t kFmtSlots = 8192;
FmtSlot g_fmt[kFmtSlots];

FmtSlot* fmtSlot(const char* fmt) {
  const size_t h = size_t((uint64_t(uintptr_t(fmt)) * 0x9E3779B97F4A7C15ull) >> 51);
  for (size_t i = 0; i < 32; ++i) {
    FmtSlot& s = g_fmt[(h + i) & (kFmtSlots - 1)];
    const char* k = s.key.load(std::memory_order_acquire);
    if (k == fmt) return &s;
    if (!k) {
      const char* expected = nullptr;
      if (s.key.compare_exchange_strong(expected, fmt, std::memory_order_acq_rel)) return &s;
      if (expected == fmt) return &s;
    }
  }
  return nullptr;  // neighbourhood full: classify on every call (correct, just slower)
}

bool playerKeeps(LogSeverity sev, const char* fmt) {
  FmtSlot* s = fmtSlot(fmt);
  uint8_t c = s ? s->cls.load(std::memory_order_relaxed) : 0;
  if (!c) {
    c = isMilestone(fmt) ? 2 : 1;
    if (s) s->cls.store(c, std::memory_order_relaxed);
  }
  if (c == 2) return true;
  if (sev < LogSeverity::Warn) return false;
  const uint32_t cap = sev >= LogSeverity::Error ? 64u : 4u;
  if (!s) return false;  // uncached site: cannot count, drop non-milestones
  return s->emitted.fetch_add(1, std::memory_order_relaxed) < cap;
}

// iter-464 perf: the timestamp's date/time part is formatted once per second per
// thread (localtime_s on every line was the single biggest fixed cost of a line).
struct TsCache {
  int64_t sec = -1;
  char text[24] = {};
};
thread_local TsCache t_ts;

}  // namespace

bool Log::wouldLog(LogSeverity sev, const char* fmt) {
  return g_profile == Profile::Dev || playerKeeps(sev, fmt);
}

bool Log::playerProfile() { return g_profile == Profile::Player; }

// Simple spinlock guard to avoid std::mutex (SRWLOCK) which can crash
// when threads die via ExitThread while the CRT is in an indeterminate state.
// iter-464 perf: spin politely (pause) and yield the core after a while - the old
// tight test_and_set loop burned a whole core per waiter while the holder flushed.
struct LogSpinGuard {
  std::atomic_flag& f;
  LogSpinGuard(std::atomic_flag& flag) : f(flag) {
    int rounds = 0;
    while (f.test_and_set(std::memory_order_acquire)) {
      for (int i = 0; i < 64 && f.test(std::memory_order_relaxed); ++i) _mm_pause();
#ifdef _WIN32
      if (++rounds > 8) SwitchToThread();
#endif
    }
  }
  ~LogSpinGuard() { f.clear(std::memory_order_release); }
};

static const char* severityStr(LogSeverity s) {
  switch (s) {
    case LogSeverity::Trace: return "TRACE";
    case LogSeverity::Debug: return "DEBUG";
    case LogSeverity::Info:  return "INFO ";
    case LogSeverity::Warn:  return "WARN ";
    case LogSeverity::Error: return "ERROR";
    default: return "?????";
  }
}

static const char* categoryStr(LogCategory c) {
  switch (c) {
    case LogCategory::Boot:        return "BOOT";
    case LogCategory::KernelImport: return "KERNEL:IMPORT";
    case LogCategory::KernelThread: return "KERNEL:THREAD";
    case LogCategory::KernelFile:   return "KERNEL:FILE";
    case LogCategory::Mem:         return "MEM";
    case LogCategory::GfxCmd:      return "GFX:CMD";
    case LogCategory::GfxShader:   return "GFX:SHADER";
    case LogCategory::Audio:       return "AUDIO";
    case LogCategory::Perf:        return "PERF";
    default: return "????";
  }
}

void Log::init(const char* logFilePath) {
  g_profile = profileFromEnv();
  // iter-464 perf: SR_LOG_CONSOLE=0 stops the stderr echo (a WriteFile to the console per
  // line, inside the global log lock; a Windows Terminal console renders every one). Unset
  // or 1 keeps it (tools that redirect stderr - replay_ab - read their summary from it).
  if (const char* c = std::getenv("SR_LOG_CONSOLE")) g_toConsole = c[0] != '0';
  {
    LogSpinGuard lock(g_logSpinLock);
    if (logFilePath) {
      g_logFile = fopen(logFilePath, "w");
      // iter-365u: a big fully-buffered sink so a log line costs a memcpy, not a
      // write syscall. See the flush policy in Log::write.
      if (g_logFile) setvbuf(g_logFile, nullptr, _IOFBF, 1 << 20);
    }
  }
  // One line that says which profile this log is (a milestone tag: always kept).
  write(LogCategory::Boot, LogSeverity::Info,
        "[LOG_PROFILE] %s (SR_LOG=dev|player; unset = dev for automated rolls "
        "(SR_INSTANCE_ID), player otherwise)",
        g_profile == Profile::Dev ? "dev" : "player");
}

void Log::shutdown() {
  LogSpinGuard lock(g_logSpinLock);
  if (g_logFile) {
    fclose(g_logFile);
    g_logFile = nullptr;
  }
}

void Log::write(LogCategory cat, LogSeverity sev, const char* fmt, ...) {
  if (g_profile != Profile::Dev && !playerKeeps(sev, fmt)) return;
  std::va_list args;
  va_start(args, fmt);
  writeV(cat, sev, fmt, args);
  va_end(args);
}

void Log::writeV(LogCategory cat, LogSeverity sev, const char* fmt, std::va_list args) {
  // (Log::write has already applied the profile; a direct writeV caller is rare and
  // keeps its line.)
  const auto now = std::chrono::system_clock::now();
  const int64_t msTotal =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  const int64_t sec = msTotal / 1000;
  const int ms = int(msTotal % 1000);
  TsCache& ts = t_ts;
  if (ts.sec != sec) {
    const std::time_t t = std::time_t(sec);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    snprintf(ts.text, sizeof(ts.text), "%04d-%02d-%02d %02d:%02d:%02d", tmBuf.tm_year + 1900,
             tmBuf.tm_mon + 1, tmBuf.tm_mday, tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    ts.sec = sec;
  }

  char msgBuf[4096];
  vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);

  char lineBuf[4096 + 128];
  int len = snprintf(lineBuf, sizeof(lineBuf), "[%s.%03d] [%s] [%s] %s\n",
                     ts.text, ms, severityStr(sev), categoryStr(cat), msgBuf);
  if (len < 0) return;
  if (len >= int(sizeof(lineBuf))) len = int(sizeof(lineBuf)) - 1;

  LogSpinGuard lock(g_logSpinLock);
  {
    char* slot = g_tail[g_tailHead];
    int n = len < kTailSlot - 1 ? len : kTailSlot - 2;
    std::memcpy(slot, lineBuf, size_t(n));
    if (n < len) slot[n++] = '\n';
    slot[n] = '\0';
    g_tailHead = (g_tailHead + 1) % kMaxTailLines;
    if (g_tailCount < kMaxTailLines) ++g_tailCount;
  }

  if (g_toConsole) {
#ifdef _WIN32
    // Use WriteFile instead of fputs to avoid CRT stdio locks.
    // ExitThread in worker threads can corrupt CRT internal state.
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr && hErr != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(hErr, lineBuf, (DWORD)len, &written, nullptr);
    }
#else
    fputs(lineBuf, stderr);
#endif
  }
  if (g_logFile) {
    fwrite(lineBuf, 1, size_t(len), g_logFile);
    // iter-365u PLATFORM FIX: this used to fflush() EVERY line, i.e. a
    // synchronous disk flush inside the global log spinlock — executed on the
    // guest's own threads. The FM file worker and the preload conveyor both log
    // in their hot paths, so every line serialized them against each other and
    // against the disk: 4KB reads ended up ~15-20ms apart with sub-second
    // stretches of no I/O at all. That is exactly the conveyor-vs-stream-I/O
    // ratio inversion behind the provider-less deadlock (iter-365s) — our own
    // diagnostics were throttling the pipeline they were measuring.
    // Flush policy: errors immediately (never lose a crash breadcrumb), plus a
    // periodic safety flush; Log::shutdown() and the crash path fclose/flush.
    // Bounded by BOTH count and time: a hot phase flushes every kFlushEvery
    // lines, and a quiet phase (e.g. a frozen boot we are about to kill and
    // read the tail of) still flushes within kFlushMs. steady_clock::now() is a
    // QPC read (~20ns) — orders of magnitude below the flush it replaces.
    const auto snow = std::chrono::steady_clock::now();
    if (sev >= LogSeverity::Error || ++g_sinceFlush >= kFlushEvery ||
        snow - g_lastFlush >= std::chrono::milliseconds(kFlushMs)) {
      fflush(g_logFile);
      g_sinceFlush = 0;
      g_lastFlush = snow;
    }
  }
}

void Log::getTailLines(char* out, int maxLen) {
  LogSpinGuard lock(g_logSpinLock);
  out[0] = '\0';
  int written = 0;
  const int first = (g_tailHead - g_tailCount + kMaxTailLines) % kMaxTailLines;
  for (int i = 0; i < g_tailCount; ++i) {
    const char* line = g_tail[(first + i) % kMaxTailLines];
    const int need = int(std::strlen(line)) + 1;
    if (written + need > maxLen) break;
    memcpy(out + written, line, size_t(need));
    written += need - 1;
  }
  if (written > 0) out[written] = '\0';
}


}  // namespace sr_logref
