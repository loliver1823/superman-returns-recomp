#include "log.hpp"
#include "../kernel/sr_oracle.hpp"  // SR_ORACLE: oracle-only milestone tags (mem.resources step 2a's MEM_RES)
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
// WaitOnAddress / WakeByAddress* (Windows 8+): the producers' full-queue wait and the writer's idle wait.
#pragma comment(lib, "synchronization.lib")
#endif

// ---------------------------------------------------------------------------
// app/log-async (docs/TOOLKIT.md 3.2): THE ASYNCHRONOUS LOG WRITER.
//
// A producer (any thread that logs) formats its line exactly as before (the timestamp is taken at
// the call), then, under the log spinlock, copies one record into a byte ring and into the crash
// tail ring - no syscall, no I/O inside the lock - and wakes the writer if it sleeps. One writer
// thread ("SR log writer") takes whole records in ring order, translates '\n' to "\r\n" (the bytes
// the CRT text-mode stream used to write) and hands them to the OS with one WriteFile, then echoes
// the untranslated bytes to stderr. The ring order is the global enqueue sequence; nothing is
// dropped while a writer exists: a producer that finds the ring full waits in 100 ms slices and
// reports the wait ([LOG_STALL] producer wait). Writes over 100 ms are reported (aggregated, at most
// one [LOG_STALL] write line per second), and a [LOG_STALL] census line every 10 s is the activation
// witness and the byte-exact completeness check (bytes= = the raw line bytes before it). One thread
// owns the file at a time and every write is journaled (see "The queue and the writer"). Drain
// points: Log::flush, Log::shutdown, the crash filter (app/crash_handler_win.cpp), a SIGABRT handler,
// and a TLS process-detach callback that writes what the killed writer left at ExitProcess / _Exit /
// exit / quick_exit. Crash / abort context (Log::EmergencyScope) never waits and never spins
// unboundedly. A write that fails after its retries, or an emergency line that finds no room, counts
// lost (the census's lost=) and makes flush() false.
//
// History: until iter-365u every line was fflush()ed inside the lock on the guest's own threads
// (the FM file worker and the preload conveyor serialized against the disk - the provider-less
// deadlock, iter-365s); iter-365u flushed every 256 lines / 200 ms instead, still on whichever
// thread crossed the threshold, under the lock - which held the CP ~4.85 s in tcmp1 census0.
// ---------------------------------------------------------------------------

namespace sr {

// iter-464 perf: the crash handler's tail is a fixed ring (it was a vector<string>
// that erased its front - a 64-element shift plus a heap allocation per line, inside
// the global lock). Lines longer than a slot are truncated in the TAIL only.
static constexpr int kTailSlot = 512;
static char g_tail[Log::kMaxTailLines][kTailSlot];
static int g_tailHead = 0;   // next slot to write
static int g_tailCount = 0;
static std::atomic_flag g_logSpinLock = ATOMIC_FLAG_INIT;
static bool g_toConsole = true;

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
    // app/log-async (docs/TOOLKIT.md 3.2): the log writer's witnesses (producer waits, slow writes, the
    // 10 s census) and the hot-thread file writers' slow writes (Log::noteIoStall)
    "LOG_STALL", "IO_STALL",
    // fix/postgame-apt: the SR_DAS_WITNESS diagnostic (default off; a run that sets it keeps its findings)
    "DAS_WITNESS",
    // step4/audit: the logic audit's import fixes (kernel/import_fixes.cpp; written only while a batch gate is on)
    "IMPORT_FIXES",
    "IMPORT_AUDIT",   // step4/audit: the SR_AUDIT_IMPORTS census's boot line (audit builds only; kernel/import_audit.cpp)
    // perf telemetry
    "PERF", "HOST_PROFILE", "GUEST_PROFILE", "NATIVE_FRAMESPLIT", "GPU_CP_TIME", "NATIVE_SUBMIT_TIME", "PM4_SWAP_TIME",
    "PM4_BIND_CENSUS", "PM4_SHM_FRAME", "MIRROR_DEMAND", "HIGHFPS", "HIGHFPS_PACE", "PM4_GPU_TIME",
    "MOVIE_PACE", "SIM_FREEZE", "SIM_FREEZE_DUMP",   // iter-466 hfr: the clock-inventory diagnostic
    "AB_TOGGLE", "CONST_SKIP",                       // iter-467 guestcode: in-roll A/B bits, upload skip
    "AB_PRESENT",                                    // iter-467 native-a: the per-present flip (SR_AB_PRESENT) while the sim is held
    "AB_DRAIN",                                      // iter-467 native-a: the drained per-present flip (SR_AB_PRESENT_DRAINED) waits / postponements
    "SHTIME_OBS", "SHTIME_HOLD",                     // iter-467 native-a: the wall-clock shader time 0x29 (SR_TIMEBASE_OBS, 2 s) and its sim hold (per hold)
    "NATIVE_DEVCREATE",                              // iter-467 native-a: boot.device_create's ring-init line + census counters
    "P2_TRACE",                                      // iter-467 native-a: the fence / ring / answer trace census (SR_P2_TRACE, 10 s)
    "GPU_CP", "NATIVE_SUBMIT", "RINGLESS",           // Oracle/native submission census + P2-A line
    "GPU_COHER",                                     // native/coher-sentinel: the coherency requests the CP applied (beside each census)
    "NATIVE_ZPD", "NATIVE_ZPD_Q",                    // p2.zpd: the occlusion-query census (beside each [GPU_CP] census) + the first 64 queries
    "NATIVE_ZPD_T", "PM4_LIVE_TRACE",                // p2.zpd zpd7: the traced frames' queries (the Xenia oracle) + the live trace files
    "LEAF",                                          // iter-467 native-a: SR_NATIVE_LEAF_TRACE per-call lines of the row-3 / 5 / 3b sites (rate-limited)
    "AUDIO_TIMING", "AUDIO_OUT_SUM", "AUDIO_DEV",    // iter-467 sound lane: output health (5 s; device events)
    "AUDIO_OPEN",                                    // lane E E0: one line per audio device open (tools/audio_safety_check.py)
    "AUDIO_NATIVE",                                  // iter-467 N6: guest XMA-register access census (30 s)
    "CP_GATE", "SHM_WT",                             // iter-466 cp lane: gate witnesses, write tracking
    "VD_SYSBUF", "VD_SCANOUT", "VD_RING",            // iter-467 p2: VdGetSystemCommandBuffer / scan-out registers / ring from the argument
    "NATIVE_METER", "NATIVE_METER_RUN", "NATIVE_TRANSPORT",              // iter-467 the native meter (owner-facing number)
    "MEM_RES_NATIVE",                                // native resource generation capacity at terminal snapshot
#if SR_ORACLE  // (the census is an oracle unit: the release configuration has no MEM_RES line to keep)
    "MEM_RES",                                       // mem.resources step 2a: the registry's shadow census (SR_MEM_RESOURCES=census only)
    "MEM_RES_CHECK",                                 // native kept-page checker and its terminal coverage
    "MEM_RES_FETCH",                                 // bounded consumed-fetch diagnostics retain every sampled record
    "MEM_RES_TEX",                                   // one exact queued consumer snapshot and its completion
    "MEM_RES_AB",                                    // the frozen-pass A/B of the native route (gfx/resource_ab.hpp)
    "MEM_RES_TEXID",                                 // the texture-identity census of resolve output (gfx/resource_texid.hpp)
#endif
    "NATIVE_VBLANK",                                 // display.registers: SR_NATIVE_VBLANK's census / exit counters, refusal, trace BAD
    "NATIVE_BUILDER_CENSUS", "NATIVE_BUILDER_REDUNDANCY", "NATIVE_LEDGER", "CONST_DEDUP", "NATIVE_FLUSH", "NATIVE_FLUSH_BAD", "NATIVE_FLUSH_DROP", "NATIVE_EXEC_PROF", "DRAW_LEDGER", "NATIVE_DRAIN_CENSUS", "NATIVE_METER_KINDS", "NATIVE_EXEC", "NATIVE_TILE_CENSUS", "NATIVE_DRAIN_REUSE",  // iter-467 native A diagnostics
    "PM4_COV", "PM4_COVMAP", "NATIVE_BRACKETS", "NATIVE_CPTIME",  // iter-467 native A P0: the PM4 producer census (SR_NATIVE_BRACKETS)
    "NB_REC", "NB_REC_BAD", "NB_NANWATCH",           // iter-467 native-b P1 row 9: the record / dry-CHECK counters (SR_NB_REC, SR_NB_REC_CHECK)
    "VPK_F16_4",  // iter-467 p2: the FLOAT16_4 production-input census (kernel/observe_vpk_f16.cpp)
    "HITCH", "HITCH_SUM", "HITCH_IO", "ASYNC_READ", "DIAG_READ_LATENCY", "GAMMA_OBS",  // iter-466 stutter lane
    // port features / player build identity (docs/PLAYER_BUILD.md)
    "PORT", "RES_SCALE", "DISPLAY", "STDIO_GUARD", "MODMENU", "SHADER_CACHE",
    "SHADER_WAIT", "SHADER_DROP",                    // render/shader-coverage: the miss policy's waits and every draw without its shader
    // input receipts the scenario tools read (MOUSE_UI only exists under SR_MOUSE_UI_LOG=1)
    "MOUSE_UI", "INPUT", "PC_CONTROLS",
    "MENU_CENSUS",   // lane E E1: the menu census (SR_MENU_CENSUS=1, kernel/mouse_ui.cpp; docs/MENU_INVENTORY.md)
    // era markers (tools/perf/perf_report.py, sr_drive scenarios)
    "NAT_FE", "APT_LOADMOVIE", "APT_REQ", "APT_SCREEN", "FE_SETSCREEN", "XAM", "XAM_PROFILE",
    "KBPAD_OBS", "SR_FSCMD", "DEVTOOL", "OBJSTATE",
    "CS_INIT", "CS_REINIT_HELD",                     // step 6 I8: RtlInitializeCriticalSection's re-init counts (kernel/stubs.cpp)
    "ENTRY_POOL",                                    // step 6 I6: the Entry::Impl free list's counters (kernel/native_flush.cpp; tracer rolls run player)
    "LOAD_BEGIN", "LOAD_DONE", "LOAD_OFF",   // step 4 ruling 12: the load screen's begin / end (kernel/load_marker.hpp)
    // failure breadcrumbs
    "CRASH", "BUGCHECK", "KERNEL_CRASH", "ICALL_FAIL", "GPU_HANG", "GPU_CP_STALL", "NATIVE_SUBMIT_STALL", "FATAL",
    "CRASH_DUMP", "HEAP_SENTINEL", "ASAN",   // step4/relcrash: SR_CRASH_DUMP_HEAP + SR_HEAP_SENTINEL (app/heap_sentinel.cpp)
    "GUEST_ALLOC_FAIL",   // step4 stuck-load: a failed guest allocation (kernel/guest_alloc_fail.hpp; rate-limited there)
    "HOST_WAIT_STALL",   // render/boot-attach: the watchdog's host-wait escalation block (kernel/gpu_hang_wd.cpp)
    // step4/timing 7a / 4(c): the boot's one-time host-shader lines (one or two per boot; a runtime compile is the
    // A/B's validity witness, so a player-profile roll must keep it), the opt-in dump queue's census and the opt-in
    // contended-lock trace (SR_CS_WAIT_TRACE=1: past 4 lines the per-site Warn cap hid the waits the 7a A/B grades)
    "HOST_HLSL", "PM4_EARLY_INIT", "PRESENT_DUMP_Q", "CS_WAIT",
    "HOST_WORK_STALL",   // step4/timing 2k(4): the watchdog's host-work block (kernel/gpu_hang_wd.cpp, SR_GPU_HANG_HOST_WORK)
    "PSO_CREATE", "PSO_WARM", "PSO_MANIFEST",   // step4/timing 2k: the pipeline census / warm-up / manifest (gfx/pso_warm.cpp)
    "XLAT_HWM",   // step4/timing 7d: the opt-in xlat arena high-water (SR_XLAT_HWM=1; a port-config census roll dropped it)
    "MEM_SITE",   // step4/timing 7d: SR_MEM_SITES=1, the big host allocations named (kernel/mem_sites.cpp)
    // step4/timing G7: a texture the cache could not create, whatever the cause (HRESULT + size), and the
    // SR_TEX_CACHE_EVICT sweeps / checks (gfx/d3d12_backend.cpp texEvict*)
    "TEX_CREATE_FAIL", "TEX_CACHE_EVICT",
    // R1 band-aid hooks on generated code (kernel/r1_hooks.hpp): every hook shares ONE call
    // site, so the per-site Warn cap (4) would hide all but the first few fires in a player
    // roll; the fire log is already sparse (#1, #2, #4, ... per hook) and the burndown counts it
    "R1_FIRE",
    // capture tooling a player may switch on
    "PRESENT_DUMP", "FRAME_DUMP", "TRACE_HOTKEY", "HLE_TRACE",
    // tools/graceful-quit: the harness-requested quit and its terminal snapshot (kernel/quit.cpp)
    "QUIT",
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

// tools/graceful-quit (Log::setThreadCapture): the calling thread's capture, null for every thread but the quit's.
struct ThreadCapture {
  void (*sink)(void*, const char*) = nullptr;
  void* user = nullptr;
  bool suppress = false;
};
thread_local ThreadCapture t_capture;

}  // namespace

void Log::setThreadCapture(void (*sink)(void* user, const char* message), void* user, bool suppress) {
  t_capture.sink = sink;
  t_capture.user = user;
  t_capture.suppress = sink && suppress;
}

bool Log::wouldLog(LogSeverity sev, const char* fmt) {
  return g_profile == Profile::Dev || playerKeeps(sev, fmt);
}

bool Log::playerProfile() { return g_profile == Profile::Player; }

// Simple spinlock guard to avoid std::mutex (SRWLOCK) which can crash
// when threads die via ExitThread while the CRT is in an indeterminate state.
// iter-464 perf: spin politely (pause) and yield the core after a while - the old
// tight test_and_set loop burned a whole core per waiter while the holder flushed.
// app/log-async: the holder now only copies a record (no I/O), so a waiter spins briefly. The holder's thread id is
// kept, so a thread that re-enters the logger while it holds the lock (a fault inside the logger reaching a filter
// that logs) is detected instead of spinning forever (emitLine's emergency path, getTailLines).
static std::atomic<unsigned long> g_lockOwner{0};
static void logSpinAcquire() {
  int rounds = 0;
  while (g_logSpinLock.test_and_set(std::memory_order_acquire)) {
    for (int i = 0; i < 64 && g_logSpinLock.test(std::memory_order_relaxed); ++i) _mm_pause();
#ifdef _WIN32
    if (++rounds > 8) SwitchToThread();
#endif
  }
  g_lockOwner.store(GetCurrentThreadId(), std::memory_order_relaxed);
}
// The bounded acquire (crash / abort context, the writer's own lines, tryWriteLine, the crash tail): false when the
// lock stays busy for maxMs, and at once when the calling thread already holds it.
static bool logSpinTryAcquire(unsigned maxMs) {
  const unsigned long me = GetCurrentThreadId();
  if (g_lockOwner.load(std::memory_order_relaxed) == me) return false;
  uint64_t deadline = 0;
  int rounds = 0;
  while (g_logSpinLock.test_and_set(std::memory_order_acquire)) {
    for (int i = 0; i < 64 && g_logSpinLock.test(std::memory_order_relaxed); ++i) _mm_pause();
    if (++rounds > 8) {
      const uint64_t now = GetTickCount64();
      if (!deadline)
        deadline = now + maxMs;
      else if (now >= deadline)
        return false;
      SwitchToThread();
    }
  }
  g_lockOwner.store(me, std::memory_order_relaxed);
  return true;
}
static void logSpinRelease() {
  g_lockOwner.store(0, std::memory_order_relaxed);
  g_logSpinLock.clear(std::memory_order_release);
}

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

namespace {

// The largest line writeV produces (its lineBuf) - one record's payload never exceeds it.
constexpr size_t kLineMax = 4096 + 128;

// One finished line, formatted the way writeV always has: "[date time.ms] [SEV  ] [CAT] msg\n",
// cut at kLineMax - 1 bytes (a cut line loses its '\n'). Returns the length (< 0: nothing).
int formatLineV(char* lineBuf, LogCategory cat, LogSeverity sev, const char* fmt, std::va_list args) {
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

  int len = snprintf(lineBuf, kLineMax, "[%s.%03d] [%s] [%s] %s\n",
                     ts.text, ms, severityStr(sev), categoryStr(cat), msgBuf);
  if (len < 0) return -1;
  if (len >= int(kLineMax)) len = int(kLineMax) - 1;
  return len;
}

int formatLine(char* lineBuf, LogCategory cat, LogSeverity sev, const char* fmt, ...) {
  std::va_list args;
  va_start(args, fmt);
  const int len = formatLineV(lineBuf, cat, sev, fmt, args);
  va_end(args);
  return len;
}

// ---------------------------------------------------------------------------
// The queue and the writer.
//
// OWNERSHIP (docs/TOOLKIT.md 3.2.4): one thread at a time writes the file, and every write is journaled.
//   - The writer thread. A writer that ended (the test seam; nothing in the game ends it) is REPLACED by
//     whichever thread notices first (ensureWriter: one CreateThread, never file I/O on that thread), one
//     replacement at a time; the replacement first finishes the ended writer's journaled batch.
//   - The writer thread itself, in its own crash filter or SIGABRT handler (Log::flush on the writer).
//   - The exit drain (the TLS process-detach callback): the OS has ended every other thread.
// A producer or a flusher never writes the file: it waits on the writer's progress, within its own deadline.
// THE JOURNAL: inflight = 1 while stage[0, stagedLen) is being written at file offset inflightAt; the file's
// real position then says how much of it landed. The commit stores tail (the batch leaves the ring) BEFORE
// inflight = 0, so whichever owner comes next finishes an interrupted batch exactly: nothing lost, nothing
// written twice.
// ---------------------------------------------------------------------------
constexpr size_t kReserveBytes = 64u << 10;     // ring bytes only the logger's own lines and emergency lines may use
constexpr size_t kDefaultQueueKB = 16u << 10;   // SR_LOG_QUEUE_KB default: 16 MB for ordinary lines
constexpr size_t kMinQueueKB = 16;              // > the largest record (4 + kLineMax bytes)
constexpr size_t kBatchRaw = 256u << 10;        // line bytes per WriteFile
constexpr double kStallMs = 100.0;              // a write / wait longer than this is reported
constexpr DWORD kWaitSliceMs = 100;             // a producer's full-queue wait slice
constexpr DWORD kIdleWaitMs = 100;              // the writer's longest sleep
constexpr uint64_t kCensusPeriodMs = 10000;     // [LOG_STALL] census period (automated runs: SR_INSTANCE_ID)
constexpr uint64_t kCensusPeriodPlayerMs = 300000;  // a plain player launch: every 5 min (a player log stays small)
constexpr DWORD kShutdownWaitMs = 10000;        // Log::shutdown's bound on the writer's drain
constexpr uint32_t kInternalBit = 0x80000000u;  // record header flag: a line the logger wrote about itself
constexpr int kWriteTries = 3;                  // a failed file write is retried before its lines count lost
constexpr DWORD kRetryMs = 50;                  //   ... this far apart, from where it stopped
constexpr uint64_t kWitnessPeriodMs = 1000;     // at most one [LOG_STALL] write line per op per second (aggregated)
constexpr unsigned kEmergencyLockMs = 50;       // an emergency line's (and the crash tail's) bound on the log lock
constexpr unsigned kWriterLockMs = 20;          // the writer's bound on the log lock for its own lines
constexpr unsigned kTryWriteLockMs = 20;        // Log::tryWriteLine's bound on the log lock

enum : int { kClosed = 0, kOpen = 1, kClosing = 2 };

// Slow writes not yet reported by a [LOG_STALL] write line (one per op: file, stderr). Writer-owned.
struct SlowAgg {
  uint32_t count = 0;
  uint64_t maxUs = 0;
  uint64_t bytes = 0;       // the slowest write's bytes
  bool injected = false;
  uint64_t lastLineMs = 0;  // when this op's last line was enqueued (0: never)
};

struct Queue {
  // Set by Log::init (no thread reads them while the state is kClosed).
  char* ring = nullptr;         // records: u32 header (payload length | kInternalBit) + payload (the line's bytes)
  size_t cap = 0;               // ring bytes = normalCap + kReserveBytes
  size_t normalCap = 0;         // what ordinary lines may occupy
  HANDLE file = nullptr;        // nullptr: no file (Log::init(nullptr) or the open failed)
  char* stage = nullptr;        // the batch in flight, translated (\n -> \r\n)
  char* rawStage = nullptr;     // the same batch untranslated (the stderr echo)
  uint64_t t0Ms = 0;            // GetTickCount64 at init
  unsigned injectMs = 0;        // SR_LOG_INJECT_STALL=<ms>[@<s>]: the writer sleeps <ms> inside one timed write,
  uint64_t injectAtMs = 0;      //   once, <s> seconds after init (a named diagnostic, default off)

  // The writer. Its handles are never closed: a thread checking writerDead() may still hold the value.
  std::atomic<HANDLE> writerThread{nullptr};
  std::atomic<unsigned long> writerTid{0};
  std::atomic<int> respawning{0};  // ensureWriter: one replacement at a time
  int respawns = 0;                // replacements started (guarded by respawning)

  // Producer side: guarded by g_logSpinLock.
  bool accepting = false;
  uint64_t head = 0;            // ring bytes enqueued (monotonic; offset = head % cap)
  uint64_t lines = 0;           // records enqueued
  uint64_t rawBytes = 0;        // line bytes enqueued (the census's bytes=)
  uint64_t peak = 0;            // largest ring occupancy seen at an enqueue

  // Published.
  std::atomic<int> state{kClosed};
  std::atomic<uint64_t> headPub{0};   // head, published after the record is in the ring
  std::atomic<uint64_t> tail{0};      // ring bytes whose lines are done with (in the file, or counted lost)
  std::atomic<uint64_t> echoed{0};    // ring bytes also echoed to stderr
  std::atomic<uint32_t> progress{0};  // bumped after every batch: what waiters sleep on
  std::atomic<uint32_t> waiters{0};   // producers / flushers sleeping on progress
  std::atomic<uint32_t> writerIdle{0};
  std::atomic<bool> stop{false};
  std::atomic<int> exited{0};         // the exit drain ran: later lines take the stderr-only path

  // The journal and the owner's state (only the current owner touches these; ownership passes through a thread's
  // end - observed by WaitForSingleObject - and a CreateThread, or through the OS ending every other thread).
  std::atomic<int> inflight{0};
  uint64_t inflightAt = 0;      // the file offset the batch starts at
  size_t stagedLen = 0;         // stage bytes of the batch
  size_t rawLen = 0;            // rawStage bytes of the batch
  uint64_t inflightEnd = 0;     // the ring position after the batch
  uint64_t inflightRaw = 0;     // the batch's line bytes
  uint64_t inflightLines = 0;
  bool inflightPrefix = false;  // the batch starts with the "\r\n" that ends a torn line
  bool inflightExternal = false;  // the batch holds a line that is not the logger's own
  bool tornLine = false;        // a lost batch left part of a line at the file's end
  uint64_t filePos = 0;         // bytes the file holds (owner-tracked)
  uint64_t fileWrites = 0;      // file writes since init (the exit seam's ordinal)
  unsigned long lastGle = 0;    // the last failed write's GetLastError
  SlowAgg slow[2];              // [0] file writes, [1] the stderr echo

  // Statistics (the census line).
  std::atomic<uint64_t> linesWritten{0}, rawWritten{0}, writes{0}, slowWrites{0}, maxWriteUs{0};
  std::atomic<uint64_t> waits{0}, waitUs{0}, maxWaitUs{0}, lostLines{0}, lostBytes{0};
  std::atomic<uint64_t> unwitnessed{0};  // producer waits whose witness line found no room (counted, not seen)
  uint64_t censusPeriodMs = kCensusPeriodMs;
};
Queue g_q;

// Log::testSeams (difftest only).
struct Seams {
  std::atomic<unsigned> exitMidWriteMask{0};
  std::atomic<int> failWrites{0};
  std::atomic<unsigned> slowWriteMs{0};
  std::atomic<int> respawnLimit{-1};
  std::atomic<bool> faultInLock{false};
  std::atomic<void (*)(unsigned long)> onFileWrite{nullptr};
};
Seams g_seams;

// Log::EmergencyScope: depth and the lines it could not queue, per thread.
thread_local int t_emergency = 0;
thread_local unsigned t_emergencyFailed = 0;

uint64_t nowUs() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return uint64_t(double(c.QuadPart) * 1e6 / double(f.QuadPart));
}

void atomicMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

void ringPut(uint64_t pos, const void* src, size_t n) {
  const size_t off = size_t(pos % g_q.cap);
  const size_t first = n < g_q.cap - off ? n : g_q.cap - off;
  std::memcpy(g_q.ring + off, src, first);
  if (n > first) std::memcpy(g_q.ring, static_cast<const char*>(src) + first, n - first);
}

void ringGet(uint64_t pos, void* dst, size_t n) {
  const size_t off = size_t(pos % g_q.cap);
  const size_t first = n < g_q.cap - off ? n : g_q.cap - off;
  std::memcpy(dst, g_q.ring + off, first);
  if (n > first) std::memcpy(static_cast<char*>(dst) + first, g_q.ring, n - first);
}

// The crash tail ring (lock held).
void tailPutLocked(const char* lineBuf, int len) {
  char* slot = g_tail[g_tailHead];
  int n = len < kTailSlot - 1 ? len : kTailSlot - 2;
  std::memcpy(slot, lineBuf, size_t(n));
  if (n < len) slot[n++] = '\n';
  slot[n] = '\0';
  g_tailHead = (g_tailHead + 1) % Log::kMaxTailLines;
  if (g_tailCount < Log::kMaxTailLines) ++g_tailCount;
}

// One record into the ring (lock held, room checked by the caller). internal: a line the logger wrote about
// itself ([LOG_STALL] census / write / producer wait) - a batch of only those raises no new write witness.
void putRecordLocked(const char* line, size_t n, bool internal) {
  const uint32_t hdr = uint32_t(n) | (internal ? kInternalBit : 0u);
  ringPut(g_q.head, &hdr, 4);
  ringPut(g_q.head + 4, line, n);
  g_q.head += 4 + n;
  g_q.lines++;
  g_q.rawBytes += n;
  const uint64_t used = g_q.head - g_q.tail.load(std::memory_order_acquire);
  if (used > g_q.peak) g_q.peak = used;
  g_q.headPub.store(g_q.head, std::memory_order_seq_cst);
}

bool roomLocked(size_t n, bool reserve) {
  const uint64_t used = g_q.head - g_q.tail.load(std::memory_order_acquire);
  return used + 4 + n <= (reserve ? g_q.cap : g_q.normalCap);
}

void wakeWriter() {
  if (g_q.writerIdle.load(std::memory_order_seq_cst)) WakeByAddressSingle(&g_q.headPub);
}

void wakeWaiters() {
  g_q.progress.fetch_add(1, std::memory_order_seq_cst);
  if (g_q.waiters.load(std::memory_order_seq_cst)) WakeByAddressAll(&g_q.progress);
}

bool writeAll(HANDLE h, const char* p, size_t n, size_t* written) {
  size_t done = 0;
  while (done < n) {
    const DWORD chunk = DWORD((n - done) < (1u << 30) ? (n - done) : (1u << 30));
    DWORD w = 0;
    if (!WriteFile(h, p + done, chunk, &w, nullptr) || w == 0) break;
    done += w;
  }
  if (written) *written = done;
  return done == n;
}

void echoStderr(const char* p, size_t n) {
  HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
  if (hErr && hErr != INVALID_HANDLE_VALUE) writeAll(hErr, p, n, nullptr);
}

// The thread's description (SetThreadDescription), ASCII, or "-".
void threadName(char* out, size_t cap) {
  std::snprintf(out, cap, "-");
  using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const GetDesc fn = reinterpret_cast<GetDesc>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!fn) return;
  PWSTR w = nullptr;
  if (FAILED(fn(GetCurrentThread(), &w)) || !w) return;
  size_t i = 0;
  for (; w[i] && i + 1 < cap; ++i) out[i] = (w[i] >= 0x20 && w[i] < 0x7F) ? char(w[i]) : '?';
  if (i) out[i] = '\0';
  LocalFree(w);
}

// The census line, enqueued under the lock (its bytes= is the raw line bytes before it).
void enqueueCensusLocked(const char* kind) {
  char line[kLineMax];
  const uint64_t ms = GetTickCount64() - g_q.t0Ms;
  const int n = formatLine(
      line, LogCategory::Boot, LogSeverity::Warn,
      "[LOG_STALL] census %s t=%.1fs lines=%llu bytes=%llu written=%llu writes=%llu slow=%llu max_write_ms=%.1f "
      "waits=%llu unwitnessed=%llu wait_ms=%.1f max_wait_ms=%.1f peak=%llu cap=%llu lost=%llu (the log writer, "
      "docs/TOOLKIT.md 3.2)",
      kind, double(ms) / 1000.0, (unsigned long long)g_q.lines, (unsigned long long)g_q.rawBytes,
      (unsigned long long)g_q.rawWritten.load(), (unsigned long long)g_q.writes.load(),
      (unsigned long long)g_q.slowWrites.load(), double(g_q.maxWriteUs.load()) / 1000.0,
      (unsigned long long)g_q.waits.load(), (unsigned long long)g_q.unwitnessed.load(),
      double(g_q.waitUs.load()) / 1000.0, double(g_q.maxWaitUs.load()) / 1000.0, (unsigned long long)g_q.peak,
      (unsigned long long)g_q.normalCap, (unsigned long long)g_q.lostLines.load());
  if (n > 0 && roomLocked(size_t(n), true)) putRecordLocked(line, size_t(n), true);
}

// The writer's own lines ([LOG_STALL] write, write FAILED). They never wait: the lock is tried for at most
// kWriterLockMs, and false when even the reserve is full (the caller keeps the line's counts and retries
// later). Accepted while Log::shutdown drains too (the writer writes everything queued before it ends).
bool tryEnqueueInternal(const char* line, size_t n) {
  if (!logSpinTryAcquire(kWriterLockMs)) return false;
  const bool ok = g_q.state.load(std::memory_order_acquire) != kClosed && roomLocked(n, true);
  if (ok) putRecordLocked(line, n, true);
  logSpinRelease();
  return ok;
}

// ----------------------------------------------------------------------------- the owner's write path
// Stages whole records from `from` (at most kBatchRaw line bytes, at least one record) into stage / rawStage and
// opens the journal for them. *external: the batch holds a line that is not the logger's own. Returns the ring
// position after the batch.
uint64_t openJournal(uint64_t from, uint64_t to, bool* external) {
  uint64_t pos = from;
  size_t st = 0, raw = 0;
  uint64_t n = 0;
  bool ext = false;
  const bool prefix = g_q.tornLine;
  if (prefix) {  // the last lost batch left part of a line: end it, so the next record starts a line
    g_q.stage[st++] = '\r';
    g_q.stage[st++] = '\n';
  }
  while (pos < to) {
    uint32_t hdr = 0;
    ringGet(pos, &hdr, 4);
    const uint32_t len = hdr & ~kInternalBit;
    if (n && raw + len > kBatchRaw) break;
    if (!(hdr & kInternalBit)) ext = true;
    char* r = g_q.rawStage + raw;
    ringGet(pos + 4, r, len);
    // The CRT text-mode stream's bytes: every '\n' becomes "\r\n".
    for (uint32_t i = 0; i < len; ++i) {
      if (r[i] == '\n') g_q.stage[st++] = '\r';
      g_q.stage[st++] = r[i];
    }
    raw += len;
    pos += 4 + len;
    ++n;
  }
  g_q.inflightAt = g_q.filePos;
  g_q.stagedLen = st;
  g_q.rawLen = raw;
  g_q.inflightEnd = pos;
  g_q.inflightRaw = raw;
  g_q.inflightLines = n;
  g_q.inflightPrefix = prefix;
  g_q.inflightExternal = ext;
  g_q.inflight.store(1, std::memory_order_release);
  if (external) *external = ext;
  return pos;
}

// One file write (WriteFile until n bytes or an error), through the test seams.
bool fileWrite(const char* p, size_t n, size_t* w) {
  *w = 0;
  const uint64_t ordinal = ++g_q.fileWrites;
  const unsigned long me = GetCurrentThreadId();
  if (auto cb = g_seams.onFileWrite.load(std::memory_order_acquire)) cb(me);
  if (const unsigned ms = g_seams.slowWriteMs.load(std::memory_order_relaxed)) Sleep(ms);
  if (ordinal <= 32 && ((g_seams.exitMidWriteMask.load(std::memory_order_relaxed) >> (ordinal - 1)) & 1u) &&
      me == g_q.writerTid.load(std::memory_order_acquire)) {
    writeAll(g_q.file, p, n / 2, w);  // half of it lands, then the writer thread ends with the journal open
    g_q.writerTid.store(0, std::memory_order_release);
    ExitThread(0);
  }
  if (g_seams.failWrites.load(std::memory_order_relaxed) > 0 && g_seams.failWrites.fetch_sub(1) > 0) {
    writeAll(g_q.file, p, n / 2, w);  // a partial write that then fails
    SetLastError(ERROR_DISK_FULL);
    return false;
  }
  return writeAll(g_q.file, p, n, w);
}

// Writes stage[*done, stagedLen) of the journaled batch, advancing *done by what lands. A failed write is retried
// (kWriteTries in all, kRetryMs apart) from where it stopped. True when the whole batch is in the file.
bool writeRest(size_t* done) {
  if (!g_q.file) {
    *done = g_q.stagedLen;
    return true;
  }
  for (int attempt = 1;; ++attempt) {
    size_t w = 0;
    const bool ok = fileWrite(g_q.stage + *done, g_q.stagedLen - *done, &w);
    *done += w;
    if (ok) return true;
    g_q.lastGle = GetLastError();
    if (attempt >= kWriteTries) return false;
    Sleep(kRetryMs);
  }
}

// Closes the journal: the batch's records leave the ring (tail) BEFORE inflight = 0, so a resume after a kill
// between the two finds tail == inflightEnd and writes nothing. ok = false: the rest of the batch could not be
// written after its retries - its lines count lost (the census's lost=; flush() false from then on), stderr says
// so at once, and the file says so as soon as the disk takes writes again (a [LOG_STALL] write FAILED line; none
// for a batch of only the logger's own lines, so a disk that stays dead cannot feed the queue forever).
void closeJournal(bool ok, size_t done) {
  g_q.filePos = g_q.inflightAt + done;
  if (ok) {
    g_q.linesWritten.fetch_add(g_q.inflightLines, std::memory_order_relaxed);
    g_q.rawWritten.fetch_add(g_q.inflightRaw, std::memory_order_relaxed);
    g_q.tornLine = false;
  } else {
    g_q.lostLines.fetch_add(g_q.inflightLines, std::memory_order_relaxed);
    g_q.lostBytes.fetch_add(g_q.stagedLen - done, std::memory_order_relaxed);
    g_q.tornLine = done > 0 || g_q.inflightPrefix;  // the file now ends inside a line
    char msg[kLineMax];
    const int m = formatLine(msg, LogCategory::Boot, LogSeverity::Warn,
                             "[LOG_STALL] write FAILED tid=%lu gle=%lu lines=%llu bytes=%llu tries=%d (these lines "
                             "are not in the log file; the census lost= counts them, docs/TOOLKIT.md 3.2)",
                             GetCurrentThreadId(), g_q.lastGle, (unsigned long long)g_q.inflightLines,
                             (unsigned long long)(g_q.stagedLen - done), kWriteTries);
    if (m > 0) {
      echoStderr(msg, size_t(m));
      if (g_q.inflightExternal) tryEnqueueInternal(msg, size_t(m));
    }
  }
  g_q.tail.store(g_q.inflightEnd, std::memory_order_release);
  g_q.inflight.store(0, std::memory_order_release);
  wakeWaiters();
}

// The owner's first act (a replacement writer, the writer's own crash drain, the exit drain): finish the batch an
// ended owner had in flight, at the file's real position - nothing lost, nothing written twice. echo: also echo
// it to stderr (a live writer; the drains write the file only).
void resumeJournal(bool echo) {
  if (g_q.inflight.load(std::memory_order_acquire) != 1) return;
  if (g_q.tail.load(std::memory_order_acquire) == g_q.inflightEnd) {  // committed; only the flag was left
    g_q.inflight.store(0, std::memory_order_release);
    return;
  }
  size_t done = 0;
  LARGE_INTEGER zero{}, cur{};
  if (g_q.file && SetFilePointerEx(g_q.file, zero, &cur, FILE_CURRENT) && uint64_t(cur.QuadPart) >= g_q.inflightAt)
    done = uint64_t(cur.QuadPart) - g_q.inflightAt;
  if (done > g_q.stagedLen) done = g_q.stagedLen;
  const bool ok = writeRest(&done);
  const uint64_t end = g_q.inflightEnd;
  const size_t raw = g_q.rawLen;
  closeJournal(ok, done);
  if (echo && g_toConsole) echoStderr(g_q.rawStage, raw);
  g_q.echoed.store(end, std::memory_order_release);
  wakeWaiters();
}

// The owner drains in place of a writer that will not: the writer thread itself in its crash filter / SIGABRT
// handler (Log::flush on the writer), or the exit drain after the OS ended every other thread. The journaled
// batch first, then whole batches until the queue is empty or the deadline passes (checked between batches; a
// write in progress is not interrupted). File only: the stderr forwarders may be gone. Its only callers are
// logTlsCallback and Log::flush (on the writer thread).
void drainAsOwner(uint64_t deadlineMs) {
  resumeJournal(false);
  for (;;) {
    const uint64_t t = g_q.tail.load(std::memory_order_acquire);
    const uint64_t h = g_q.headPub.load(std::memory_order_acquire);
    if (t >= h || GetTickCount64() >= deadlineMs) break;
    const uint64_t end = openJournal(t, h, nullptr);
    size_t done = 0;
    const bool ok = writeRest(&done);
    closeJournal(ok, done);
    g_q.echoed.store(end, std::memory_order_release);
  }
  wakeWaiters();
}

// ------------------------------------------------------------------------------------- the writer thread
void aggregateSlow(int op, uint64_t bytes, uint64_t us, bool injected) {
  SlowAgg& a = g_q.slow[op];
  a.count++;
  if (us > a.maxUs) {
    a.maxUs = us;
    a.bytes = bytes;
  }
  a.injected |= injected;
}

// The aggregated [LOG_STALL] write lines: per op at most one per kWitnessPeriodMs (force: now - the writer's
// end). ms= is the slowest write since the op's last line, count= how many were slow. True when a line was
// enqueued. A line that finds no room stays aggregated (the census's slow= counts every slow write anyway).
bool emitWitnesses(bool force) {
  bool any = false;
  const uint64_t now = GetTickCount64();
  for (int op = 0; op < 2; ++op) {
    SlowAgg& a = g_q.slow[op];
    if (!a.count) continue;
    if (!force && a.lastLineMs && now - a.lastLineMs < kWitnessPeriodMs) continue;
    char line[kLineMax];
    const int len = formatLine(
        line, LogCategory::Boot, LogSeverity::Warn,
        "[LOG_STALL] write tid=%lu name=SR log writer op=%s bytes=%llu ms=%.1f queued=%llu count=%u%s",
        GetCurrentThreadId(), op ? "stderr" : "file", (unsigned long long)a.bytes, double(a.maxUs) / 1000.0,
        (unsigned long long)(g_q.headPub.load() - g_q.tail.load()), a.count, a.injected ? " injected=1" : "");
    if (len > 0 && tryEnqueueInternal(line, size_t(len))) {
      a = SlowAgg{};
      a.lastLineMs = now;
      any = true;
    }
  }
  return any;
}

// One batch: stage + journal, write (timed, retried), commit, echo. False if nothing was queued.
// A write over 100 ms counts in the census's slow=; it gets a [LOG_STALL] write line (aggregated, emitWitnesses)
// only when its batch held a line that is not the logger's own, or the injected stall - so the witness of a slow
// write can never make its own successor (a disk that stays slow does not keep the queue busy forever).
bool writerBatch() {
  const uint64_t t = g_q.tail.load(std::memory_order_relaxed);
  const uint64_t h = g_q.headPub.load(std::memory_order_acquire);
  if (t >= h) return false;
  bool external = false;
  const uint64_t end = openJournal(t, h, &external);
  const size_t st = g_q.stagedLen, raw = g_q.rawLen;
  bool injected = false;
  const uint64_t t0 = nowUs();
  if (g_q.injectMs && GetTickCount64() - g_q.t0Ms >= g_q.injectAtMs) {
    Sleep(g_q.injectMs);  // SR_LOG_INJECT_STALL: a slow disk, inside the timed write
    g_q.injectMs = 0;
    injected = true;
  }
  size_t done = 0;
  const bool ok = writeRest(&done);
  const uint64_t us = nowUs() - t0;
  closeJournal(ok, done);
  g_q.writes.fetch_add(1, std::memory_order_relaxed);
  atomicMax(g_q.maxWriteUs, us);
  if (double(us) / 1000.0 > kStallMs) {
    g_q.slowWrites.fetch_add(1, std::memory_order_relaxed);
    if (external || injected) aggregateSlow(0, st, us, injected);
  }
  if (g_toConsole) {
    const uint64_t e0 = nowUs();
    echoStderr(g_q.rawStage, raw);
    const uint64_t eus = nowUs() - e0;
    if (double(eus) / 1000.0 > kStallMs) {
      g_q.slowWrites.fetch_add(1, std::memory_order_relaxed);
      if (external) aggregateSlow(1, raw, eus, false);
    }
  }
  g_q.echoed.store(end, std::memory_order_release);
  wakeWaiters();
  return true;
}

DWORD WINAPI writerMain(LPVOID) {
  using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  if (auto fn = reinterpret_cast<SetDesc>(
          GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
    fn(GetCurrentThread(), L"SR log writer");
  resumeJournal(true);  // a replacement first finishes what the ended writer had in flight
  uint64_t lastCensus = GetTickCount64();
  for (;;) {
    const bool wrote = writerBatch();
    emitWitnesses(false);
    if (GetTickCount64() - lastCensus >= g_q.censusPeriodMs && logSpinTryAcquire(kWriterLockMs)) {
      lastCensus = GetTickCount64();
      if (g_q.accepting) enqueueCensusLocked("periodic");
      logSpinRelease();
    }
    if (wrote) continue;
    if (g_q.stop.load(std::memory_order_acquire)) {
      if (emitWitnesses(true)) continue;  // the last aggregated slow writes, then their (unwitnessed) write
      if (g_q.tail.load(std::memory_order_acquire) >= g_q.headPub.load(std::memory_order_acquire)) break;
      continue;
    }
    // Sleep until a producer publishes (Dekker with putRecordLocked / wakeWriter) or 100 ms.
    g_q.writerIdle.store(1, std::memory_order_seq_cst);
    uint64_t h = g_q.headPub.load(std::memory_order_seq_cst);
    if (h == g_q.tail.load(std::memory_order_relaxed) && !g_q.stop.load())
      WaitOnAddress(&g_q.headPub, &h, sizeof(h), kIdleWaitMs);
    g_q.writerIdle.store(0, std::memory_order_seq_cst);
  }
  g_q.writerTid.store(0, std::memory_order_release);  // an ended writer's id must not match a later thread
  return 0;
}

// Starts a writer thread (suspended until its id is published, so the thread can recognise itself).
bool startWriter() {
  DWORD tid = 0;
  HANDLE h = CreateThread(nullptr, 0, &writerMain, nullptr, CREATE_SUSPENDED, &tid);
  if (!h) return false;
  SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
  g_q.writerTid.store(tid, std::memory_order_release);
  g_q.writerThread.store(h, std::memory_order_release);
  ResumeThread(h);
  return true;
}

bool writerDead() {
  HANDLE h = g_q.writerThread.load(std::memory_order_acquire);
  return h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
}

enum class WriterState { Alive, None };

// A writer that ended (only the test seam ends one) is replaced: one CreateThread by whichever thread notices
// first, never file I/O on the caller's thread; the replacement finishes the ended writer's journaled batch
// before anything else. None: there is no writer and none will come - nothing left to write after
// Log::shutdown's stop, the log closed, the respawn-limit seam, or CreateThread failed (the process is exiting).
WriterState ensureWriter() {
  if (!writerDead()) return WriterState::Alive;
  int expected = 0;
  if (!g_q.respawning.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    return WriterState::Alive;  // another thread is starting the replacement
  WriterState r = WriterState::Alive;
  if (writerDead()) {  // re-checked under the flag: the replacement may already run
    const int limit = g_seams.respawnLimit.load(std::memory_order_relaxed);
    const bool nothingLeft = g_q.stop.load(std::memory_order_acquire) &&
                             g_q.inflight.load(std::memory_order_acquire) == 0 &&
                             g_q.tail.load(std::memory_order_acquire) >= g_q.headPub.load(std::memory_order_acquire);
    if (nothingLeft || g_q.state.load(std::memory_order_acquire) == kClosed || (limit >= 0 && g_q.respawns >= limit) ||
        !startWriter())
      r = WriterState::None;
    else
      ++g_q.respawns;
  }
  g_q.respawning.store(0, std::memory_order_release);
  return r;
}

// The synchronous path before Log::init / after Log::shutdown (today's behaviour without a file).
void fallbackEcho(const char* line, int len) {
  if (g_toConsole) echoStderr(line, size_t(len));
}

// A line in crash / abort context (Log::EmergencyScope), from a thread that already holds the log lock (it
// faulted inside the logger and its filter logs), or from the writer thread itself: bounded, it never waits for
// room. The lock is tried for at most kEmergencyLockMs; the line may use the reserve. True when it is queued;
// otherwise it goes to stderr and, while the log is open, counts lost.
bool emitEmergency(const char* line, int len) {
  bool queued = false, open = false;
  if (logSpinTryAcquire(kEmergencyLockMs)) {
    tailPutLocked(line, len);
    open = g_q.accepting;
    if (open && roomLocked(size_t(len), true)) {
      putRecordLocked(line, size_t(len), false);
      queued = true;
    }
    logSpinRelease();
  } else {
    open = g_q.state.load(std::memory_order_acquire) != kClosed;
  }
  if (queued) {
    wakeWriter();
    return true;
  }
  fallbackEcho(line, len);
  if (open) {
    g_q.lostLines.fetch_add(1, std::memory_order_relaxed);
    g_q.lostBytes.fetch_add(uint64_t(len), std::memory_order_relaxed);
    ++t_emergencyFailed;
  }
  return false;
}

// A producer's line: the tail ring, then the queue (waiting while it is full; never dropped while a writer
// exists or can be started).
void emitLine(const char* line, int len) {
  if (g_q.exited.load(std::memory_order_relaxed)) {  // the exit drain ran: no writer, no ring
    fallbackEcho(line, len);
    return;
  }
  const unsigned long me = GetCurrentThreadId();
  if (t_emergency || g_lockOwner.load(std::memory_order_relaxed) == me ||
      g_q.writerTid.load(std::memory_order_relaxed) == me) {
    emitEmergency(line, len);
    return;
  }
  bool waited = false;
  uint64_t waitStartUs = 0;
  unsigned slices = 0;
  char name[64] = "-";
  logSpinAcquire();
  tailPutLocked(line, len);
  if (g_seams.faultInLock.load(std::memory_order_relaxed) && g_seams.faultInLock.exchange(false))
    *static_cast<volatile int*>(nullptr) = 0;  // Log::testSeams: a fault inside the logger, the lock held
  for (;;) {
    if (!g_q.accepting) {
      logSpinRelease();
      fallbackEcho(line, len);
      return;
    }
    if (roomLocked(size_t(len), false)) break;
    // Full: register as a waiter, re-check after reading progress (a batch may have just landed),
    // then sleep one slice with the lock released.
    g_q.waiters.fetch_add(1, std::memory_order_seq_cst);
    uint32_t seen = g_q.progress.load(std::memory_order_seq_cst);
    const bool room = roomLocked(size_t(len), false);
    logSpinRelease();
    if (!waited) {
      waited = true;
      waitStartUs = nowUs();
      threadName(name, sizeof(name));
    }
    if (!room) {
      if (ensureWriter() == WriterState::None) {
        // No writer and none can start: nothing will ever make room. Rather than hang this thread, the line
        // goes to stderr and counts lost.
        g_q.waiters.fetch_sub(1, std::memory_order_seq_cst);
        g_q.lostLines.fetch_add(1, std::memory_order_relaxed);
        g_q.lostBytes.fetch_add(uint64_t(len), std::memory_order_relaxed);
        fallbackEcho(line, len);
        return;
      }
      wakeWriter();
      WaitOnAddress(&g_q.progress, &seen, sizeof(seen), kWaitSliceMs);
      ++slices;
    }
    g_q.waiters.fetch_sub(1, std::memory_order_seq_cst);
    logSpinAcquire();
  }
  putRecordLocked(line, size_t(len), false);
  if (waited) {
    const uint64_t us = nowUs() - waitStartUs;
    g_q.waits.fetch_add(1, std::memory_order_relaxed);
    g_q.waitUs.fetch_add(us, std::memory_order_relaxed);
    atomicMax(g_q.maxWaitUs, us);
    // The witness right after the line that waited (the reserve takes it; if even the reserve
    // is full the wait stays counted in the census).
    char w[kLineMax];
    const int n = formatLine(w, LogCategory::Boot, LogSeverity::Warn,
                             "[LOG_STALL] producer wait tid=%lu name=%s ms=%.1f slices=%u bytes=%d queued=%llu "
                             "cap=%llu (the log queue was full; the line above waited, nothing was dropped)",
                             me, name, double(us) / 1000.0, slices, len,
                             (unsigned long long)(g_q.head - g_q.tail.load()), (unsigned long long)g_q.normalCap);
    if (n > 0 && roomLocked(size_t(n), true))
      putRecordLocked(w, size_t(n), true);
    else
      g_q.unwitnessed.fetch_add(1, std::memory_order_relaxed);
  }
  logSpinRelease();
  wakeWriter();
}

// SIGABRT: abort() / std::terminate raise it before the UCRT's __fastfail when a handler exists.
using SigFn = void (*)(int);
SigFn g_prevAbrt = SIG_DFL;
void onSigAbrt(int sig) {
  Log::EmergencyScope emergency;
  Log::flush(3000);
  if (g_prevAbrt != SIG_DFL && g_prevAbrt != SIG_IGN && g_prevAbrt != SIG_ERR && g_prevAbrt != &onSigAbrt)
    g_prevAbrt(sig);
}

}  // namespace

// The process-exit drain (ExitProcess, _Exit, exit, quick_exit, main returning): runs after the OS has
// terminated every other thread, so the queue and the journal are frozen; this thread is the owner.
static void NTAPI logTlsCallback(PVOID, DWORD reason, PVOID) {
  if (reason != DLL_PROCESS_DETACH) return;
  if (g_q.state.load(std::memory_order_acquire) == kClosed) return;
  g_q.exited.store(1, std::memory_order_relaxed);  // a line logged after this takes the stderr-only path
  drainAsOwner(~0ull);
}

Log::EmergencyScope::EmergencyScope() : failedAtStart_(t_emergencyFailed) { ++t_emergency; }
Log::EmergencyScope::~EmergencyScope() { --t_emergency; }
unsigned Log::EmergencyScope::failed() const { return t_emergencyFailed - failedAtStart_; }

void Log::testSeams(const TestSeams& s) {
  g_seams.exitMidWriteMask.store(s.exitMidWriteMask);
  g_seams.failWrites.store(s.failWrites);
  g_seams.slowWriteMs.store(s.slowWriteMs);
  g_seams.respawnLimit.store(s.respawnLimit);
  g_seams.faultInLock.store(s.faultInLock);
  g_seams.onFileWrite.store(s.onFileWrite, std::memory_order_release);
}

void Log::init(const char* logFilePath) {
  if (g_q.state.load() != kClosed) {
    shutdown();
    if (g_q.state.load() != kClosed) return;  // the old writer is stuck in a write: keep it, open nothing new
  }
  g_profile = profileFromEnv();
  // iter-464 perf: SR_LOG_CONSOLE=0 stops the stderr echo (a WriteFile to the console per
  // line; a Windows Terminal console renders every one). Unset or 1 keeps it (tools that
  // redirect stderr - replay_ab - read their summary from it). app/log-async: the writer
  // thread echoes, not the logging thread.
  if (const char* c = std::getenv("SR_LOG_CONSOLE")) g_toConsole = c[0] != '0';
  size_t queueKB = kDefaultQueueKB;
  if (const char* q = std::getenv("SR_LOG_QUEUE_KB")) {
    const long long v = std::atoll(q);
    if (v > 0) queueKB = size_t(v) < kMinQueueKB ? kMinQueueKB : size_t(v);
  }
  g_q.injectMs = 0;
  g_q.injectAtMs = 0;
  if (const char* s = std::getenv("SR_LOG_INJECT_STALL")) {
    g_q.injectMs = unsigned(std::atoi(s));
    if (const char* at = std::strchr(s, '@')) g_q.injectAtMs = uint64_t(std::atof(at + 1) * 1000.0);
  }
  const size_t cap = queueKB * 1024u + kReserveBytes;
  // The ring is never released (no VirtualFree: a writer stuck in a write may still read it, and the
  // hostRangeReadable surface rule stays trivially met). A re-init that wants a bigger ring - tests only -
  // allocates a new one and leaves the old one mapped.
  static size_t s_ringAlloc = 0;
  if (!g_q.ring || s_ringAlloc < cap) {
    g_q.ring = static_cast<char*>(VirtualAlloc(nullptr, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    s_ringAlloc = g_q.ring ? cap : 0;
  }
  if (!g_q.stage) {
    g_q.stage = static_cast<char*>(VirtualAlloc(nullptr, 2 * (kBatchRaw + kLineMax) + 2, MEM_RESERVE | MEM_COMMIT,
                                                PAGE_READWRITE));
    g_q.rawStage = static_cast<char*>(VirtualAlloc(nullptr, kBatchRaw + kLineMax, MEM_RESERVE | MEM_COMMIT,
                                                   PAGE_READWRITE));
  }
  g_q.cap = cap;
  g_q.normalCap = queueKB * 1024u;
  g_q.file = nullptr;
  if (logFilePath) {
    // fopen(path, "w")'s open: create / truncate, shared read + write.
    HANDLE f = CreateFileA(logFilePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) g_q.file = f;
  }
  g_q.head = g_q.lines = g_q.rawBytes = g_q.peak = 0;
  g_q.headPub.store(0);
  g_q.tail.store(0);
  g_q.echoed.store(0);
  g_q.stop.store(false);
  g_q.inflight.store(0);
  g_q.filePos = 0;
  g_q.fileWrites = 0;
  g_q.tornLine = false;
  g_q.slow[0] = g_q.slow[1] = SlowAgg{};
  g_q.respawns = 0;
  g_q.linesWritten = g_q.rawWritten = g_q.writes = g_q.slowWrites = g_q.maxWriteUs = 0;
  g_q.waits = g_q.waitUs = g_q.maxWaitUs = g_q.lostLines = g_q.lostBytes = g_q.unwitnessed = 0;
  g_q.t0Ms = GetTickCount64();
  {
    const char* inst = std::getenv("SR_INSTANCE_ID");  // an automated run: the census is the graders' witness
    g_q.censusPeriodMs = (inst && inst[0]) ? kCensusPeriodMs : kCensusPeriodPlayerMs;
  }
  const bool ready = g_q.ring && g_q.stage && g_q.rawStage;
  const bool started = ready && startWriter();
  if (started) {
    logSpinAcquire();
    g_q.accepting = true;
    g_q.state.store(kOpen);
    logSpinRelease();
  } else {
    char msg[160];
    const int m = std::snprintf(msg, sizeof(msg),
                                "[LOG_STALL] the log writer could not start (gle=%lu): no log file this run\n",
                                GetLastError());
    if (m > 0) echoStderr(msg, size_t(m));
    if (g_q.file) CloseHandle(g_q.file);
    g_q.file = nullptr;
  }
  static bool s_abrt = false;
  if (!s_abrt) {
    s_abrt = true;
    g_prevAbrt = std::signal(SIGABRT, &onSigAbrt);
    if (g_prevAbrt == SIG_ERR) g_prevAbrt = SIG_DFL;
  }
  // One line that says which profile this log is (a milestone tag: always kept).
  write(LogCategory::Boot, LogSeverity::Info,
        "[LOG_PROFILE] %s (SR_LOG=dev|player; unset = dev for automated rolls "
        "(SR_INSTANCE_ID), player otherwise)",
        g_profile == Profile::Dev ? "dev" : "player");
}

bool Log::flush(unsigned timeoutMs) {
  if (g_q.state.load(std::memory_order_acquire) == kClosed) return g_q.lostLines.load() == 0;
  const uint64_t target = g_q.headPub.load(std::memory_order_seq_cst);
  const uint64_t deadline = GetTickCount64() + timeoutMs;
  // On the writer thread itself (its crash filter or SIGABRT handler) nobody else will write: it drains as the
  // owner. Every other thread only waits (a dead writer is replaced, never stood in for).
  if (GetCurrentThreadId() == g_q.writerTid.load(std::memory_order_acquire) &&
      g_q.tail.load(std::memory_order_acquire) < target)
    drainAsOwner(deadline);
  // Wait for the file, then (best effort, same deadline) for the stderr echo.
  for (int phase = 0; phase < 2; ++phase) {
    std::atomic<uint64_t>& done = phase == 0 ? g_q.tail : g_q.echoed;
    if (done.load(std::memory_order_acquire) >= target) continue;
    g_q.waiters.fetch_add(1, std::memory_order_seq_cst);
    for (;;) {
      uint32_t seen = g_q.progress.load(std::memory_order_seq_cst);
      if (done.load(std::memory_order_acquire) >= target) break;
      const uint64_t now = GetTickCount64();
      if (now >= deadline) break;
      if (ensureWriter() == WriterState::None) break;  // no writer will come: waiting cannot help
      wakeWriter();
      const uint64_t left = deadline - now;
      WaitOnAddress(&g_q.progress, &seen, sizeof(seen), DWORD(left < kWaitSliceMs ? left : kWaitSliceMs));
    }
    g_q.waiters.fetch_sub(1, std::memory_order_seq_cst);
  }
  return g_q.tail.load(std::memory_order_acquire) >= target && g_q.lostLines.load() == 0;
}

void Log::shutdown() {
  int expected = kOpen;
  if (!g_q.state.compare_exchange_strong(expected, kClosing)) return;  // never opened, or already closing
  // Everything logged so far (and the writer's witness lines about writing it), then the final
  // census, then no more lines (later ones take the synchronous stderr-only path, as after the old
  // fclose); the writer writes what is queued and ends.
  flush(kShutdownWaitMs);
  logSpinAcquire();
  enqueueCensusLocked("final");
  g_q.accepting = false;
  logSpinRelease();
  g_q.stop.store(true, std::memory_order_seq_cst);
  WakeByAddressSingle(&g_q.headPub);
  // Wait for the writer to write what is queued and end. A writer that ended early (the test seam) is replaced
  // and the replacement drains and ends (stop is set): this thread never writes the file.
  const uint64_t deadline = GetTickCount64() + kShutdownWaitMs;
  bool ended = false;
  for (;;) {
    HANDLE h = g_q.writerThread.load(std::memory_order_acquire);
    const uint64_t now = GetTickCount64();
    const DWORD slice = DWORD(now >= deadline ? 0 : (deadline - now < 100 ? deadline - now : 100));
    if (!h || WaitForSingleObject(h, slice) == WAIT_OBJECT_0) {
      if (g_q.inflight.load(std::memory_order_acquire) == 0 &&
          g_q.tail.load(std::memory_order_acquire) >= g_q.headPub.load(std::memory_order_acquire)) {
        ended = true;
        break;
      }
      if (GetTickCount64() >= deadline || ensureWriter() == WriterState::None) break;
      continue;
    }
    if (GetTickCount64() >= deadline) break;
  }
  if (ended) {
    if (g_q.file) CloseHandle(g_q.file);
    g_q.file = nullptr;
    g_q.state.store(kClosed);
  } else {
    // The writer is stuck in a write (a stalled disk), or none could finish: leave the handle; the exit drain
    // writes the rest when the process ends.
    static const char kMsg[] = "[LOG_STALL] Log::shutdown: the log writer did not finish within 10 s\n";
    echoStderr(kMsg, sizeof(kMsg) - 1);
  }
}

void Log::write(LogCategory cat, LogSeverity sev, const char* fmt, ...) {
  if (t_capture.sink) {  // tools/graceful-quit: the terminal snapshot's capture (one thread, at the quit only)
    char msg[4096];
    std::va_list cap;
    va_start(cap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, cap);
    va_end(cap);
    t_capture.sink(t_capture.user, msg);
    if (t_capture.suppress) return;
  }
  if (g_profile != Profile::Dev && !playerKeeps(sev, fmt)) return;
  std::va_list args;
  va_start(args, fmt);
  writeV(cat, sev, fmt, args);
  va_end(args);
}

void Log::writeV(LogCategory cat, LogSeverity sev, const char* fmt, std::va_list args) {
  // (Log::write has already applied the profile; a direct writeV caller is rare and
  // keeps its line.)
  char lineBuf[kLineMax];
  const int len = formatLineV(lineBuf, cat, sev, fmt, args);
  if (len < 0) return;
  emitLine(lineBuf, len);
}

void Log::noteIoStall(const char* what, unsigned long long bytes, double ms) {
  if (!(ms > kStallMs)) return;
  char name[64];
  threadName(name, sizeof(name));
  write(LogCategory::Boot, LogSeverity::Warn,
        "[IO_STALL] what=%s tid=%lu name=%s bytes=%llu ms=%.1f (one synchronous host file write on this "
        "thread; docs/TOOLKIT.md 3.2.3)",
        what ? what : "?", GetCurrentThreadId(), name, bytes, ms);
}

// tools/graceful-quit's hard-deadline line (kernel/quit.cpp deadlineThread, just before TerminateProcess), on the
// asynchronous writer (docs/TOOLKIT.md 3.2.4): try the lock for at most kTryWriteLockMs (never when this thread
// holds it - the lock guards a record copy only, never I/O), enqueue the line if the queue has room (never wait
// for room), then give the writer kTryWriteFlushMs to put it - and everything logged before it - in the file.
// Never waits on a lock a game thread holds. True only when flush() is.
bool Log::tryWriteLine(LogSeverity sev, const char* text) {
  constexpr unsigned kTryWriteFlushMs = 1000;
  if (!text) return false;
  char lineBuf[kLineMax];
  const int len = formatLine(lineBuf, LogCategory::Boot, sev, "%s", text);
  if (len < 0) return false;
  if (g_q.exited.load(std::memory_order_relaxed)) return false;
  if (!logSpinTryAcquire(kTryWriteLockMs)) return false;  // busy: never wait
  bool queued = false;
  if (g_q.accepting && roomLocked(size_t(len), false)) {
    tailPutLocked(lineBuf, len);
    putRecordLocked(lineBuf, size_t(len), false);
    queued = true;
  }
  logSpinRelease();
  if (!queued) return false;  // the log is closed, or the queue is full
  wakeWriter();
  return flush(kTryWriteFlushMs);
}

void Log::getTailLines(char* out, int maxLen) {
  if (maxLen <= 0) return;
  // Crash diagnostics: a bounded wait for the lock, none when this thread holds it (a fault inside the logger).
  // Without the lock the slots are read as they are: a slot being rewritten can come out mixed, never longer
  // than its slot (strnlen).
  const bool locked = logSpinTryAcquire(kEmergencyLockMs);
  out[0] = '\0';
  int written = 0;
  const int count = g_tailCount;
  const int first = (g_tailHead - count + kMaxTailLines) % kMaxTailLines;
  for (int i = 0; i < count; ++i) {
    const char* line = g_tail[(first + i) % kMaxTailLines];
    const int need = int(strnlen(line, kTailSlot - 1)) + 1;
    if (written + need > maxLen) break;
    memcpy(out + written, line, size_t(need - 1));
    written += need - 1;
    out[written] = '\0';
  }
  if (locked) logSpinRelease();
}


}  // namespace sr

// The TLS callback that drains the log at process exit (see logTlsCallback). _tls_used is the
// CRT's TLS directory (tlssup); .CRT$XL? entries between XLA and XLZ are its callback array.
#ifdef _WIN32
#pragma section(".CRT$XLY", long, read)
extern "C" __declspec(allocate(".CRT$XLY")) const PIMAGE_TLS_CALLBACK sr_log_tls_callback = &sr::logTlsCallback;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:sr_log_tls_callback")
#endif
