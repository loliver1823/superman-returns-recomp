#include "crash_handler.hpp"
#include "log.hpp"

namespace sr {

void installCrashHandler() {
  // Stub for non-Windows; Windows impl in crash_handler_win.cpp
  SR_LOG_BOOT(sr::LogSeverity::Info, "Crash handler: stub (Windows impl in crash_handler_win.cpp)");
}

bool heapDumpEnabled() { return false; }  // step4/relcrash: no DbgHelp off Windows

const char* getCrashDumpPath() {
  return "crash_dump.dmp";
}

// No DbgHelp off Windows: the lock is always granted.
DbgHelpLock::DbgHelpLock(unsigned) : held_(true) {}
DbgHelpLock::~DbgHelpLock() = default;

}  // namespace sr
