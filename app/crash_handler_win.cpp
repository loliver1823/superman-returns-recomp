#include "crash_handler.hpp"
#include "log.hpp"
#include <Windows.h>
#include <DbgHelp.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

extern "C" uint8_t* g_debugGuestBase;  // kernel/guest_context.cpp: the guest arena's base (process-wide)

namespace sr {

#pragma comment(lib, "dbghelp.lib")

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
static char g_dumpPath[MAX_PATH] = "superman_returns_crash.dmp";
static std::mutex g_crashMutex;

// DbgHelpLock (crash_handler.hpp): the owning thread's id, 0 = free.
static std::atomic<unsigned long> g_dbgHelpOwner{0};

DbgHelpLock::DbgHelpLock(unsigned timeoutMs) {
  const unsigned long me = GetCurrentThreadId();
  if (g_dbgHelpOwner.load(std::memory_order_acquire) == me) return;  // re-entry: refuse
  const ULONGLONG deadline = timeoutMs == 0xFFFFFFFFu ? ~0ull : GetTickCount64() + timeoutMs;
  for (;;) {
    unsigned long expected = 0;
    if (g_dbgHelpOwner.compare_exchange_strong(expected, me, std::memory_order_acq_rel)) {
      held_ = true;
      return;
    }
    if (GetTickCount64() >= deadline) return;
    Sleep(1);
  }
}

DbgHelpLock::~DbgHelpLock() {
  if (held_) g_dbgHelpOwner.store(0, std::memory_order_release);
}

// step4/relcrash: SR_CRASH_DUMP_HEAP (crash_handler.hpp). Read once by installCrashHandler, never in a filter.
static bool g_heapDump = false;
bool heapDumpEnabled() { return g_heapDump; }

namespace {
// The MemoryCallback walk. Plain statics, no heap, VirtualQuery only: it runs inside the crash filter, possibly over
// a corrupted heap. One walk at a time (every caller holds DbgHelpLock).
struct HeapWalk {
  uint8_t* cursor;
  uint64_t bytes;
  uint32_t regions, skippedBig, skippedGpu, skippedCap;
};
HeapWalk g_walk;
constexpr uint64_t kWalkCapBytes = 0xE0000000ull;    // 3.5 GB: the memory list's RVAs are 32-bit
constexpr uint64_t kWalkMaxRegion = 0x40000000ull;   // 1 GB: no single region above it (no heap segment is)

bool walkWanted(const MEMORY_BASIC_INFORMATION& m) {
  if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
  const DWORD p = m.Protect & 0xFF;
  const bool readable = p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                        p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
  if (!readable) return false;
  if (m.Type == MEM_PRIVATE) return true;
  return m.Type == MEM_IMAGE && (p == PAGE_READWRITE || p == PAGE_WRITECOPY);  // .data / .bss of every module
}

BOOL CALLBACK heapDumpCallback(PVOID, PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
  if (in->CallbackType == CancelCallback) {
    out->CheckCancel = FALSE;
    out->Cancel = FALSE;
    return TRUE;
  }
  if (in->CallbackType != MemoryCallback) return TRUE;  // modules, threads: the default (include)
  const uint8_t* gb = g_debugGuestBase;
  const uintptr_t g0 = gb ? reinterpret_cast<uintptr_t>(gb) - 0x100000000ull : 0;
  const uintptr_t g1 = gb ? reinterpret_cast<uintptr_t>(gb) + 0x100010000ull : 0;
  MEMORY_BASIC_INFORMATION m{};
  while (VirtualQuery(g_walk.cursor, &m, sizeof(m)) == sizeof(m)) {
    uint8_t* b = static_cast<uint8_t*>(m.BaseAddress);
    const uint64_t n = m.RegionSize;
    g_walk.cursor = b + n;
    if (!walkWanted(m)) continue;
    const uintptr_t a = reinterpret_cast<uintptr_t>(b);
    if (gb && a < g1 && a + n > g0) continue;                       // the guest arena (+ its sign-extended mirror)
    if (m.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) { ++g_walk.skippedGpu; continue; }
    if (n > kWalkMaxRegion) { ++g_walk.skippedBig; continue; }
    if (g_walk.bytes + n > kWalkCapBytes) { ++g_walk.skippedCap; continue; }
    g_walk.bytes += n;
    ++g_walk.regions;
    out->MemoryBase = a;
    out->MemorySize = static_cast<ULONG>(n);
    return TRUE;
  }
  return FALSE;  // no more ranges
}

// The type and callback of one minidump: withHeap = false gives back `base` and no callback (the dump as before);
// true adds the heap walk (crash_handler.hpp). Each caller makes its own MiniDumpWriteDump call under its
// DbgHelpLock (tools/test_log_async_wiring.py W7).
MINIDUMP_CALLBACK_INFORMATION g_walkCallback{heapDumpCallback, nullptr};
MINIDUMP_TYPE dumpType(MINIDUMP_TYPE base, bool withHeap, PMINIDUMP_CALLBACK_INFORMATION* cb) {
  *cb = nullptr;
  if (!withHeap) return base;
  g_walk = HeapWalk{};
  *cb = &g_walkCallback;
  return static_cast<MINIDUMP_TYPE>(base | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemoryInfo |
                                    MiniDumpWithUnloadedModules | MiniDumpWithHandleData |
                                    MiniDumpIgnoreInaccessibleMemory);
}

void logHeapWalk(const char* what, const char* path) {
  Log::write(LogCategory::Boot, LogSeverity::Error,
             "[CRASH_DUMP] heap dump (%s, SR_CRASH_DUMP_HEAP) %s: %u regions %llu MB; skipped big %u gpu %u cap %u",
             what, path, g_walk.regions, (unsigned long long)(g_walk.bytes >> 20), g_walk.skippedBig,
             g_walk.skippedGpu, g_walk.skippedCap);
}
}  // namespace

static void writeLogTailToFile(const char* path) {
  char buf[Log::kMaxTailLines * 512];
  Log::getTailLines(buf, sizeof(buf));
  FILE* f = fopen(path, "w");
  if (f) {
    fputs("--- Last log lines before crash ---\n", f);
    fputs(buf, f);
    fclose(f);
  }
}

static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* ex) {
  std::lock_guard<std::mutex> lock(g_crashMutex);

  // app/log-async: from here on this thread's lines never wait for room and never spin on the log
  // lock (Log::EmergencyScope) - a full queue behind a stalled disk, or a fault inside the logger,
  // cannot stop this filter. The log is written by its own thread (app/log.cpp): hand everything
  // logged up to the crash to the OS first - the live-process minidump below can take seconds or
  // hang - and again after the [CRASH] line: this filter's return ends the process with no other
  // callback. Each flush is bounded by its timeout.
  Log::EmergencyScope emergency;
  Log::flush(2000);

  // Write minidump. DbgHelp is single-threaded: when another thread's DbgHelp call holds it for 2 s
  // (a hang dump in progress), the minidump is skipped rather than waited for.
  bool dbgBusy = false;
  {
    DbgHelpLock dbg(2000);
    dbgBusy = !dbg.held();
    HANDLE hFile = dbgBusy ? INVALID_HANDLE_VALUE
                           : CreateFileA(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                         nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION mei = {};
      mei.ThreadId = GetCurrentThreadId();
      mei.ExceptionPointers = ex;
      mei.ClientPointers = TRUE;
      PMINIDUMP_CALLBACK_INFORMATION cb = nullptr;
      const MINIDUMP_TYPE type = dumpType(MiniDumpWithThreadInfo, g_heapDump, &cb);
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                       type, &mei, nullptr, cb);
      CloseHandle(hFile);
    }
  }

  // Log exception so it appears in the tail we're about to write
  DWORD code = ex->ExceptionRecord->ExceptionCode;
  void* addr = ex->ExceptionRecord->ExceptionAddress;
  Log::write(LogCategory::Boot, LogSeverity::Error,
             "[CRASH] Exception 0x%08lX at %p (thread %lu)",
             code, addr, GetCurrentThreadId());
  if (!dbgBusy && g_heapDump) logHeapWalk("crash", g_dumpPath);
  if (dbgBusy)
    Log::write(LogCategory::Boot, LogSeverity::Error,
               "[CRASH] minidump skipped: DbgHelp was busy on another thread for 2 s");
  Log::flush(3000);

  // Write log tail to .log file
  char logPath[MAX_PATH];
  const char* dot = strrchr(g_dumpPath, '.');
  if (dot) {
    snprintf(logPath, sizeof(logPath), "%.*s.log", (int)(dot - g_dumpPath), g_dumpPath);
  } else {
    snprintf(logPath, sizeof(logPath), "%s.log", g_dumpPath);
  }
  writeLogTailToFile(logPath);

  if (g_prevFilter) {
    return g_prevFilter(ex);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashHandler() {
  const char* h = std::getenv("SR_CRASH_DUMP_HEAP");
  g_heapDump = h && h[0] == '1';
  g_prevFilter = SetUnhandledExceptionFilter(unhandledExceptionFilter);
  SR_LOG_BOOT(sr::LogSeverity::Info, "Crash handler installed: minidump -> %s", g_dumpPath);
  if (g_heapDump)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[CRASH_DUMP] SR_CRASH_DUMP_HEAP=1: crash / thread-fatal / diagnostic dumps carry the host heap "
                "(private + writable image pages, not the guest arena; up to 3.5 GB each)");
}

void writeThreadFatalMinidump(_EXCEPTION_POINTERS* ex) {
  static volatile LONG s_written = 0;
  if (InterlockedCompareExchange(&s_written, 1, 0) != 0) return;  // first only
  // app/log-async: an SEH filter on a dying thread - its lines never wait (Log::EmergencyScope).
  Log::EmergencyScope emergency;
  const char* path = "superman_returns_threadfatal.dmp";
  bool busy = false, dumped = false;
  {
    DbgHelpLock dbg(5000);  // DbgHelp is single-threaded (crash_handler.hpp)
    busy = !dbg.held();
    HANDLE hFile = busy ? INVALID_HANDLE_VALUE
                        : CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION mei = {};
      mei.ThreadId = GetCurrentThreadId();
      mei.ExceptionPointers = ex;
      mei.ClientPointers = FALSE;
      PMINIDUMP_CALLBACK_INFORMATION cb = nullptr;
      const MINIDUMP_TYPE type = dumpType(
          (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), g_heapDump, &cb);
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, type, &mei, nullptr, cb);
      CloseHandle(hFile);
      dumped = true;
    }
  }
  if (busy)
    Log::write(LogCategory::Boot, LogSeverity::Error,
               "[THREAD_FATAL] minidump skipped: DbgHelp was busy on another thread for 5 s");
  else if (dumped) {
    Log::write(LogCategory::Boot, LogSeverity::Error,
               "[THREAD_FATAL] minidump -> %s", path);
    if (g_heapDump) logHeapWalk("thread-fatal", path);
  }
  else
    return;
  Log::flush(2000);  // app/log-async: the dying thread's breadcrumbs reach the file before it goes
}

bool writeDiagnosticMinidump(const char* path, bool withHeap) {
  DbgHelpLock dbg(10000);  // DbgHelp is single-threaded (crash_handler.hpp)
  if (!dbg.held()) return false;
  HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return false;
  const bool heap = withHeap || g_heapDump;
  PMINIDUMP_CALLBACK_INFORMATION cb = nullptr;
  const MINIDUMP_TYPE type =
      dumpType((MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), heap, &cb);
  const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, type, nullptr, nullptr, cb);
  CloseHandle(hFile);
  if (heap) logHeapWalk("diagnostic", path);
  return ok != FALSE;
}

const char* getCrashDumpPath() {
  return g_dumpPath;
}

}  // namespace sr
