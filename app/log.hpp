#pragma once

#include <string>
#include <string_view>
#include <cstdarg>

namespace sr {

enum class LogSeverity : int {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
};

// Log categories per PLAN.md
enum class LogCategory : int {
  Boot,
  KernelImport,
  KernelThread,
  KernelFile,
  Mem,
  GfxCmd,
  GfxShader,
  Audio,
  Perf,
};

class Log {
public:
  static void init(const char* logFilePath = nullptr);
  static void shutdown();

  static void write(LogCategory cat, LogSeverity sev, const char* fmt, ...);
  // iter-464 perf (SR_LOG profiles, app/log.cpp): would a line from this call site be
  // written? Lets a hot site skip building its arguments. Always true in the dev profile.
  static bool wouldLog(LogSeverity sev, const char* fmt);
  static bool playerProfile();
  static void writeV(LogCategory cat, LogSeverity sev, const char* fmt, std::va_list args);
  // tools/graceful-quit: one finished line (no format) for kernel/quit.cpp's hard-deadline thread, which must not
  // block on a lock a hung thread holds. app/log-async: try the log lock for at most 20 ms (never when this thread
  // holds it), enqueue the line only if the queue has room (never waits for room), then Log::flush(1000) - the line
  // and everything logged before it reach the file unless the writer is stuck on the disk. True only when flush()
  // is; false when the lock was busy, the log is closed, the queue is full, the flush timed out or a line of this
  // log was lost. Always kept (no profile filter).
  static bool tryWriteLine(LogSeverity sev, const char* text);
  // tools/graceful-quit: while a capture is set on the CALLING thread, every line it logs through write() is also
  // handed to sink(user, message) - the formatted message, no timestamp, before the player-profile filter; with
  // suppress it is not written to the log at all. sink = nullptr clears it. Other threads are unaffected.
  static void setThreadCapture(void (*sink)(void* user, const char* message), void* user, bool suppress);

  // app/log-async (docs/TOOLKIT.md 3.2): the file is written by one writer thread; logging threads
  // only format and enqueue. flush() returns true once every line enqueued by any thread before the
  // call is in the file (handed to the OS: it survives a process kill). False if the writer did not
  // get there within timeoutMs (a stalled disk: those lines stay queued and are still written), or if
  // any line of this log was lost (a write that failed after its retries, an emergency line that found
  // no room: the census's lost=) - from the first loss on, flush() never returns true again. It waits
  // on the writer's progress only, never on a lock a game thread holds, and never writes the file
  // itself (except on the writer thread, in its own crash filter / SIGABRT handler); any thread may
  // call it (the crash filter, the SIGABRT handler and the exits do). shutdown() = flush, the final
  // [LOG_STALL] census line, stop the writer (it writes what is queued), close the file.
  static bool flush(unsigned timeoutMs = 2000);
  // For the host file writers that stay synchronous on a hot thread (the present dump's BMP, the
  // saves' NtWriteFile; TOOLKIT 3.2.3): one [IO_STALL] line when one write took > 100 ms.
  static void noteIoStall(const char* what, unsigned long long bytes, double ms);

  // Crash / abort context on the calling thread (the crash filter, the SIGABRT handler,
  // writeThreadFatalMinidump; TOOLKIT 3.2.4). While a scope is alive, this thread's lines never
  // wait for room and never spin unboundedly on the log lock: the lock is tried for at most 50 ms
  // (never when this thread already holds it - a fault inside the logger), the line may use the
  // 64 KB reserve, and a line that cannot be queued goes to stderr and is counted lost (the
  // census's lost=; flush() false from then on). failed() = this scope's lines not queued.
  class EmergencyScope {
  public:
    EmergencyScope();
    ~EmergencyScope();
    EmergencyScope(const EmergencyScope&) = delete;
    EmergencyScope& operator=(const EmergencyScope&) = delete;
    unsigned failed() const;

  private:
    unsigned failedAtStart_;
  };

  // Test seams (tools/difftest/log_async_test.cpp only; every field 0 / -1 / null = off).
  struct TestSeams {
    unsigned exitMidWriteMask = 0;  // bit k: the (k+1)-th file write since init (any writer thread, resumes included)
                                    // writes half its bytes, then the writer thread ends - a kill mid-WriteFile
    int failWrites = 0;             // the next n file writes write half their bytes and fail (ERROR_DISK_FULL)
    unsigned slowWriteMs = 0;       // every file write first sleeps this long (a sustained slow disk)
    int respawnLimit = -1;          // how many ended writers may be replaced (-1 = no limit)
    bool faultInLock = false;       // the next ordinary line faults (a null write) while holding the log lock
    void (*onFileWrite)(unsigned long tid) = nullptr;  // called with the writing thread's id before each file write
  };
  static void testSeams(const TestSeams& seams);

  // Ring buffer for crash handler: last N lines. app/log-async: it waits at most 50 ms for the log
  // lock and not at all when the calling thread holds it (a crash inside the logger); without the
  // lock it reads the slots as they are (a slot being rewritten can come out mixed, never longer).
  static constexpr int kMaxTailLines = 64;
  static void getTailLines(char* out, int maxLen);

};

// Convenience macros
#define SR_LOG_BOOT(sev, ...)    sr::Log::write(sr::LogCategory::Boot, sev, __VA_ARGS__)
#define SR_LOG_KERNEL_IMPORT(...) sr::Log::write(sr::LogCategory::KernelImport, sr::LogSeverity::Info, __VA_ARGS__)
#define SR_LOG_KERNEL_THREAD(...) sr::Log::write(sr::LogCategory::KernelThread, sr::LogSeverity::Info, __VA_ARGS__)
#define SR_LOG_KERNEL_FILE(...)   sr::Log::write(sr::LogCategory::KernelFile, sr::LogSeverity::Info, __VA_ARGS__)
#define SR_LOG_MEM(...)           sr::Log::write(sr::LogCategory::Mem, sr::LogSeverity::Info, __VA_ARGS__)
#define SR_LOG_GFX_CMD(...)       sr::Log::write(sr::LogCategory::GfxCmd, sr::LogSeverity::Info, __VA_ARGS__)
#define SR_LOG_GFX_SHADER(...)    sr::Log::write(sr::LogCategory::GfxShader, sr::LogSeverity::Info, __VA_ARGS__)
#define SR_LOG_AUDIO(...)         sr::Log::write(sr::LogCategory::Audio, sr::LogSeverity::Info, __VA_ARGS__)
#define SR_LOG_PERF(...)          sr::Log::write(sr::LogCategory::Perf, sr::LogSeverity::Info, __VA_ARGS__)

}  // namespace sr
