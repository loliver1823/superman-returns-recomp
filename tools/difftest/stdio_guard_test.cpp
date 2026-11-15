// iter-465 player: the stdio guard (app/stdio_guard.hpp) - no game thread blocks on a stalled
// stdout/stderr sink (the 2026-10-02 RESTART NOW relaunch death).
// Run: build/difftest.exe --stdio-guard-test     (exit = failed checks)
//
// Each case runs THIS exe as a child (--stdio-guard-child <mode>) whose stdout/stderr are
// anonymous pipes with a 4 KB buffer, and watches whether the child can finish writing 33 MB:
//   G1  separate pipes, NOBODY reads them, guard on   -> the child exits 0 (it dropped bytes)
//   G2  ONE pipe handle for both streams (Python's stderr=STDOUT shape), unread, guard on -> exits 0
//   G3  NEGATIVE CONTROL: G1 with SR_STDIO_GUARD=0 -> the child is still blocked 5 s later
//       (proves the cases can fail: without the guard the first full pipe freezes the writer)
//   G4  both pipes drained by the parent, guard on -> every byte arrives (lossless when read)
// The child writes like the game does: WriteFile on GetStdHandle(STD_ERROR_HANDLE) (sr::Log) and
// the CRT's fprintf(stderr) / printf (guest bodies, kernel prints).
#include "app/stdio_guard.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", name);
  (ok ? g_pass : g_fail)++;
}

constexpr size_t kWriteFileBytes = 32u << 20;   // via WriteFile(GetStdHandle(STD_ERROR_HANDLE))
constexpr size_t kCrtErrBytes = 512u << 10;     // via fprintf(stderr)
constexpr size_t kCrtOutBytes = 512u << 10;     // via printf (stdout)
}  // namespace

#ifdef _WIN32
int runStdioGuardChild(const char* mode) {
  (void)mode;
  sr::stdio_guard::install();
  static char chunk[4096];
  std::memset(chunk, 'x', sizeof chunk);   // no '\n': the CRT's text mode leaves it unchanged
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  for (size_t done = 0; done < kWriteFileBytes; done += sizeof chunk) {
    DWORD put = 0;
    WriteFile(err, chunk, sizeof chunk, &put, nullptr);
  }
  char line[1025];
  std::memset(line, 'y', 1024);
  line[1024] = 0;
  for (size_t done = 0; done < kCrtErrBytes; done += 1024) std::fputs(line, stderr);
  for (size_t done = 0; done < kCrtOutBytes; done += 1024) std::fputs(line, stdout);
  std::fflush(stdout);
  std::fflush(stderr);
  sr::stdio_guard::flush(3000);
  const sr::stdio_guard::Stats st = sr::stdio_guard::stats();
  // exit 0 = finished; the parent reads the guard state from the code's high bits
  return (st.stderrGuarded ? 0x10 : 0) | (st.stdoutGuarded ? 0x20 : 0) | (st.droppedBytes ? 0x40 : 0);
}

namespace {
struct Child {
  PROCESS_INFORMATION pi{};
  HANDLE outRead = nullptr, errRead = nullptr;
};

// shared: one pipe handle for both streams. guardOff: SR_STDIO_GUARD=0 in the child.
bool spawn(bool shared, bool guardOff, Child* c) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE outWrite = nullptr, errWrite = nullptr;
  if (!CreatePipe(&c->outRead, &outWrite, &sa, 4096)) return false;
  SetHandleInformation(c->outRead, HANDLE_FLAG_INHERIT, 0);
  if (!shared) {
    if (!CreatePipe(&c->errRead, &errWrite, &sa, 4096)) return false;
    SetHandleInformation(c->errRead, HANDLE_FLAG_INHERIT, 0);
  }
  wchar_t exe[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
  std::wstring cmd = std::wstring(L"\"") + exe + L"\" --stdio-guard-child x";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nullptr;
  si.hStdOutput = outWrite;
  si.hStdError = shared ? outWrite : errWrite;
  SetEnvironmentVariableW(L"SR_STDIO_GUARD", guardOff ? L"0" : nullptr);
  const BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &si, &c->pi);
  SetEnvironmentVariableW(L"SR_STDIO_GUARD", nullptr);
  CloseHandle(outWrite);
  if (errWrite) CloseHandle(errWrite);
  return ok != FALSE;
}

void finish(Child* c) {
  if (c->pi.hProcess) {
    if (WaitForSingleObject(c->pi.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(c->pi.hProcess, 99);
    WaitForSingleObject(c->pi.hProcess, 5000);
    CloseHandle(c->pi.hProcess);
    CloseHandle(c->pi.hThread);
  }
  if (c->outRead) CloseHandle(c->outRead);
  if (c->errRead) CloseHandle(c->errRead);
}

DWORD exitCodeAfter(Child* c, DWORD ms) {
  if (WaitForSingleObject(c->pi.hProcess, ms) != WAIT_OBJECT_0) return STILL_ACTIVE;
  DWORD code = 0;
  GetExitCodeProcess(c->pi.hProcess, &code);
  return code;
}

size_t drain(HANDLE h) {
  size_t total = 0;
  static thread_local char buf[65536];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) total += got;
  return total;
}
}  // namespace

int runStdioGuardTest() {
  std::printf("--stdio-guard-test (app/stdio_guard.cpp)\n");
  {
    Child c;
    const bool ok = spawn(false, false, &c);
    const DWORD code = ok ? exitCodeAfter(&c, 30000) : STILL_ACTIVE;
    check(ok && code != STILL_ACTIVE && (code & 0x0F) == 0 && (code & 0x30) == 0x30 && (code & 0x40),
          "G1 unread separate pipes: the child finishes (both streams guarded, bytes dropped)");
    if (code == STILL_ACTIVE) std::printf("    child still blocked after 30 s\n");
    finish(&c);
  }
  {
    Child c;
    const bool ok = spawn(true, false, &c);
    const DWORD code = ok ? exitCodeAfter(&c, 30000) : STILL_ACTIVE;
    check(ok && code != STILL_ACTIVE && (code & 0x0F) == 0 && (code & 0x30) == 0x30,
          "G2 one unread pipe for both streams: the child finishes");
    finish(&c);
  }
  {
    Child c;
    const bool ok = spawn(false, true, &c);
    const DWORD code = ok ? exitCodeAfter(&c, 5000) : 0;
    check(ok && code == STILL_ACTIVE, "G3 NEGATIVE CONTROL: SR_STDIO_GUARD=0 -> the child blocks on the full pipe");
    finish(&c);
  }
  {
    Child c;
    const bool ok = spawn(false, false, &c);
    size_t outBytes = 0, errBytes = 0;
    std::thread to([&] { outBytes = drain(c.outRead); });
    std::thread te([&] { errBytes = drain(c.errRead); });
    const DWORD code = ok ? exitCodeAfter(&c, 60000) : STILL_ACTIVE;
    to.join();
    te.join();
    check(ok && code != STILL_ACTIVE && (code & 0x40) == 0 && errBytes == kWriteFileBytes + kCrtErrBytes &&
              outBytes == kCrtOutBytes,
          "G4 drained pipes: every byte arrives, none dropped");
    std::printf("    stderr %zu / %zu bytes, stdout %zu / %zu bytes, code 0x%lX\n", errBytes,
                kWriteFileBytes + kCrtErrBytes, outBytes, kCrtOutBytes, static_cast<unsigned long>(code));
    finish(&c);
  }
  std::printf("stdio-guard-test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
#else
int runStdioGuardChild(const char*) { return 0; }
int runStdioGuardTest() { return 0; }
#endif
