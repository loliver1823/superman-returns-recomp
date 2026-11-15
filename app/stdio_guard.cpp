// iter-465 player: the stdio guard. Contract and the death it fixes: app/stdio_guard.hpp.
#include "stdio_guard.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace sr::stdio_guard {

#ifdef _WIN32
namespace {

constexpr DWORD kPrivatePipeBytes = 1u << 20;   // what a burst can queue before the reader runs
constexpr size_t kRingCapBytes = 16u << 20;      // what a stalled sink can queue before drops

struct Forwarder {
  DWORD stdId = 0;
  int crtFd = -1;
  HANDLE readEnd = nullptr;   // private pipe, read side (reader thread only)
  HANDLE writeEnd = nullptr;  // private pipe, write side (the std handle now)
  HANDLE sink = nullptr;      // our duplicate of the original stdout/stderr
  CRITICAL_SECTION cs;
  CONDITION_VARIABLE cv;
  std::string pending;        // guarded by cs
  bool writerBusy = false;    // guarded by cs
  uint64_t droppedNote = 0;   // guarded by cs: dropped bytes not yet reported in-stream
  volatile LONG64 forwarded = 0;
  volatile LONG64 dropped = 0;
  volatile LONG sinkFailed = 0;
  volatile LONG64 reads = 0;  // ReadFile returns with data (flush's progress signal)
};

Forwarder g_fw[2];
Stats g_stats;
bool g_installed = false;
bool g_consoleDetached = false;

void nameThread(HANDLE th, const wchar_t* name) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static Fn fn = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
  if (fn) fn(th, name);
}

DWORD WINAPI readerMain(LPVOID arg) {
  auto* f = static_cast<Forwarder*>(arg);
  constexpr DWORD kChunk = 64 * 1024;
  char* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, kChunk));
  if (!buf) return 0;
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(f->readEnd, buf, kChunk, &got, nullptr)) {
      const DWORD e = GetLastError();
      if (e == ERROR_BROKEN_PIPE) return 0;   // every write end closed (never: we keep one)
      Sleep(10);                              // never stop draining on a transient error
      continue;
    }
    if (got == 0) continue;                   // a zero-byte write
    InterlockedIncrement64(&f->reads);
    EnterCriticalSection(&f->cs);
    if (f->pending.size() + got > kRingCapBytes) {
      // The sink has stalled for 16 MB of output: drop rather than ever block a game thread.
      InterlockedAdd64(&f->dropped, got);
      f->droppedNote += got;
    } else {
      f->pending.append(buf, got);
    }
    WakeAllConditionVariable(&f->cv);
    LeaveCriticalSection(&f->cs);
  }
}

bool writeAll(HANDLE h, const char* p, size_t n) {
  while (n > 0) {
    DWORD put = 0;
    const DWORD chunk = n > (1u << 20) ? (1u << 20) : static_cast<DWORD>(n);
    if (!WriteFile(h, p, chunk, &put, nullptr) || put == 0) return false;
    p += put;
    n -= put;
  }
  return true;
}

DWORD WINAPI writerMain(LPVOID arg) {
  auto* f = static_cast<Forwarder*>(arg);
  std::string local;
  for (;;) {
    uint64_t note = 0;
    EnterCriticalSection(&f->cs);
    while (f->pending.empty())
      SleepConditionVariableCS(&f->cv, &f->cs, INFINITE);
    local.clear();
    local.swap(f->pending);
    note = f->droppedNote;
    f->droppedNote = 0;
    f->writerBusy = true;
    LeaveCriticalSection(&f->cs);

    if (!f->sinkFailed) {
      bool ok = true;
      if (note) {
        char line[160];
        const int len = std::snprintf(line, sizeof line,
            "[STDIO_GUARD] %llu bytes of output dropped while this sink was stalled\n",
            static_cast<unsigned long long>(note));
        ok = writeAll(f->sink, line, static_cast<size_t>(len));
      }
      if (ok) ok = writeAll(f->sink, local.data(), local.size());
      if (ok) InterlockedAdd64(&f->forwarded, static_cast<LONG64>(local.size()));
      else InterlockedExchange(&f->sinkFailed, 1);   // reader gone: discard from now on
    }

    EnterCriticalSection(&f->cs);
    f->writerBusy = false;
    WakeAllConditionVariable(&f->cv);
    LeaveCriticalSection(&f->cs);
  }
}

bool sinkCanStall(HANDLE h, DWORD* typeOut) {
  *typeOut = 0;
  if (!h || h == INVALID_HANDLE_VALUE) return false;
  const DWORD t = GetFileType(h);
  *typeOut = t;
  return t == FILE_TYPE_PIPE || t == FILE_TYPE_CHAR;
}

}  // namespace

bool detachOwnConsoleForPlayer(bool automated) {
  if (automated) return false;
  if (const char* e = std::getenv("SR_KEEP_CONSOLE"); e && e[0] == '1') return false;
  DWORD pids[2] = {};
  if (GetConsoleProcessList(pids, 2) != 1) return false;   // 0: none; >1: a terminal owns it
  // Started on its own console (a double-click): the player gets the game window only. The CRT
  // streams are pointed at NUL first, so no later write can reach a dead or reused handle.
  std::fflush(stdout);
  std::fflush(stderr);
  FILE* f = nullptr;
  freopen_s(&f, "NUL", "w", stdout);
  freopen_s(&f, "NUL", "w", stderr);
  freopen_s(&f, "NUL", "r", stdin);
  FreeConsole();
  SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
  SetStdHandle(STD_ERROR_HANDLE, nullptr);
  SetStdHandle(STD_INPUT_HANDLE, nullptr);
  g_consoleDetached = true;
  return true;
}

void install() {
  if (g_installed) return;
  g_installed = true;
  if (g_consoleDetached) return;
  if (const char* e = std::getenv("SR_STDIO_GUARD"); e && e[0] == '0') return;

  const DWORD ids[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  HANDLE originals[2] = {GetStdHandle(ids[0]), GetStdHandle(ids[1])};
  bool guard[2] = {};
  DWORD types[2] = {};
  for (int i = 0; i < 2; ++i) guard[i] = sinkCanStall(originals[i], &types[i]);
  g_stats.stdoutType = types[0];
  g_stats.stderrType = types[1];
  // A launcher may hand ONE handle value to both streams (Python subprocess stderr=STDOUT does).
  // Then one forwarder (stderr's) serves both, and fd 1 is re-pointed without closing twice.
  const bool shared = guard[0] && guard[1] && originals[0] == originals[1];
  if (shared) guard[0] = false;

  // Phase 1: everything that creates a handle - our duplicate of each sink, the private pipes -
  // BEFORE any CRT fd is replaced (replacing an fd closes its old handle, and Windows hands a
  // freed handle value to the next handle created).
  for (int i = 0; i < 2; ++i) {
    if (!guard[i]) continue;
    Forwarder& f = g_fw[i];
    f.stdId = ids[i];
    f.crtFd = i == 0 ? 1 : 2;
    if (!DuplicateHandle(GetCurrentProcess(), originals[i], GetCurrentProcess(), &f.sink, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
      guard[i] = false;
      continue;
    }
    if (!CreatePipe(&f.readEnd, &f.writeEnd, nullptr, kPrivatePipeBytes)) {
      CloseHandle(f.sink);
      f.sink = nullptr;
      guard[i] = false;
      continue;
    }
    InitializeCriticalSection(&f.cs);
    InitializeConditionVariable(&f.cv);
  }
  int crtFds[2] = {-1, -1};
  for (int i = 0; i < 2; ++i) {
    if (!guard[i]) continue;
    HANDLE crtCopy = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), g_fw[i].writeEnd, GetCurrentProcess(), &crtCopy, 0,
                        FALSE, DUPLICATE_SAME_ACCESS)) {
      crtFds[i] = _open_osfhandle(reinterpret_cast<intptr_t>(crtCopy), _O_WRONLY | _O_TEXT);
      if (crtFds[i] < 0) CloseHandle(crtCopy);
    }
  }
  // Phase 2: the CRT's fds and the Win32 std handles move to the private pipes.
  std::fflush(stdout);
  std::fflush(stderr);
  for (int i = 0; i < 2; ++i) {
    if (!guard[i]) continue;
    Forwarder& f = g_fw[i];
    if (crtFds[i] >= 0) {
      _dup2(crtFds[i], f.crtFd);
      _close(crtFds[i]);
    }
    SetStdHandle(f.stdId, f.writeEnd);
  }
  if (shared && g_fw[1].writeEnd) {
    // fd 1 still names the shared handle that fd 2's replacement just closed. If the freed value
    // went to fd 2's new handle, fd 1 already aliases it (leave it: closing it would close fd 2's);
    // otherwise re-point fd 1 at fd 2's pipe (closing the stale value is a no-op).
    if (_get_osfhandle(1) != _get_osfhandle(2)) _dup2(2, 1);
    SetStdHandle(STD_OUTPUT_HANDLE, g_fw[1].writeEnd);
    g_stats.stdoutGuarded = true;
  }
  // Phase 3: the forwarding threads.
  for (int i = 0; i < 2; ++i) {
    if (!guard[i]) continue;
    Forwarder& f = g_fw[i];
    HANDLE r = CreateThread(nullptr, 64 * 1024, readerMain, &f, 0, nullptr);
    HANDLE w = CreateThread(nullptr, 64 * 1024, writerMain, &f, 0, nullptr);
    if (r) { nameThread(r, i == 0 ? L"sr_stdout_reader" : L"sr_stderr_reader"); CloseHandle(r); }
    if (w) { nameThread(w, i == 0 ? L"sr_stdout_writer" : L"sr_stderr_writer"); CloseHandle(w); }
    (i == 0 ? g_stats.stdoutGuarded : g_stats.stderrGuarded) = true;
  }
}

void flush(unsigned timeoutMs) {
  // No PeekNamedPipe here: the reader thread always sits in a synchronous ReadFile on the read
  // end, and a second call on that file object waits for it - forever when the producers are
  // done (the first version hung the exit path; difftest --stdio-guard-test G1/G4 caught it).
  // Instead: done once the ring is empty, the writer is idle, and the reader has taken nothing
  // new for kQuietMs (bytes still in the private pipe wake the reader within a scheduler tick).
  constexpr ULONGLONG kQuietMs = 30;
  std::fflush(stdout);
  std::fflush(stderr);
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  for (Forwarder& f : g_fw) {
    if (!f.readEnd) continue;
    LONG64 lastReads = -1;
    ULONGLONG quietSince = GetTickCount64();
    for (;;) {
      const ULONGLONG now = GetTickCount64();
      const LONG64 reads = InterlockedCompareExchange64(&f.reads, 0, 0);
      EnterCriticalSection(&f.cs);
      const bool idle = f.pending.empty() && !f.writerBusy;
      LeaveCriticalSection(&f.cs);
      if (reads != lastReads || !idle) {
        lastReads = reads;
        quietSince = now;
      }
      if ((idle && now - quietSince >= kQuietMs) || f.sinkFailed || now >= deadline) break;
      Sleep(1);
    }
  }
}

Stats stats() {
  Stats s = g_stats;
  s.forwardedBytes = static_cast<uint64_t>(g_fw[0].forwarded + g_fw[1].forwarded);
  s.droppedBytes = static_cast<uint64_t>(g_fw[0].dropped + g_fw[1].dropped);
  s.sinkFailed = g_fw[0].sinkFailed || g_fw[1].sinkFailed;
  return s;
}

#else   // !_WIN32: nothing to interpose (the host is Windows-only today).

void install() {}
void flush(unsigned) {}
Stats stats() { return {}; }
bool detachOwnConsoleForPlayer(bool) { return false; }

#endif

}  // namespace sr::stdio_guard
