// TEST ORACLE ONLY (tools/difftest/log_async_test.cpp, app/log-async): app/log.hpp as of dev d71946e2, verbatim
// apart from the namespace (sr -> sr_logref) and the SR_LOG_* macros (dropped). Never linked into the game.
#pragma once

#include <string>
#include <string_view>
#include <cstdarg>

namespace sr_logref {

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

  // Ring buffer for crash handler: last N lines
  static constexpr int kMaxTailLines = 64;
  static void getTailLines(char* out, int maxLen);

};

}  // namespace sr_logref
