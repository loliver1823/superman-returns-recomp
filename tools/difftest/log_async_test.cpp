// app/log-async: --log-async-test, the offline proof of the asynchronous log writer (app/log.cpp;
// design and claims: docs/TOOLKIT.md 3.2).
//
// Every case runs THIS exe as a child (--log-async-child <case> <log path>) so the profile, the
// queue size, the stall injection and the crash mode start fresh; the parent reads the child's log
// file afterwards (and the child's own checks come back as its exit code / stdout).
//
//   O1  order        8 threads pass a token, so the enqueue order is fixed by the program: the file
//                    must hold seq 0..N*M-1 in that order.
//   S1  stress       16 free-running threads into a 64 KB queue behind a 300 ms writer stall: every
//                    line present exactly once, in its thread's order, byte-exact (no tear); each
//                    [LOG_STALL] producer wait follows a line of its own thread; the waits counted
//                    in the file = the census's waits=; census bytes= exact.
//   D1  slowdisk     the writer sleeps 5 s inside one write: 4 producers below capacity finish while
//                    it sleeps (max Log::write and total time far under 5 s, none of their lines on
//                    disk yet), then every line lands and [LOG_STALL] write ... injected=1 ms>=5000.
//   W1  fullwait     a 64 KB queue behind a 1.5 s stall: the producer waits (>= 1 s) and says so.
//   C1  crash        an unhandled access violation (the game's installCrashHandler filter) with a
//                    writer stall pending: every line before it and the [CRASH] line are in the file.
//   C2  abort        std::abort() (UCRT __fastfail after SIGABRT) with a stall pending: every line.
//   C3  exit         std::_Exit with a stall pending (the TLS process-detach drain): every line.
//   C4  midbatch     the writer ends with half a batch on disk, then std::_Exit: the drain resumes at
//                    the file position - every line exactly once, the file well-formed.
//   C5  midflush     the same, then Log::flush (a replacement writer resumes it) and Log::shutdown.
//   C6  crashfull    a crash while the queue is full behind a writer stuck in a 120 s write: the crash
//                    filter's lines take the emergency path (no wait for room) and the process ends in
//                    seconds (Astra round 1 P1: it waited for the writer).
//   C7  crashinlock  a fault inside the logger with the log lock held: the crash filter's [CRASH] line
//                    reaches stderr, the tail file is written, the process ends (it spun forever).
//   X1  takeexit     the writer ends mid-batch, its replacement ends mid-resume, then std::_Exit: the
//                    exit drain resumes the same journal - every line exactly once, no fragment.
//   X2  takeover     the writer ends mid-batch while 3 producers log, 4 threads flush in a loop and 2
//                    call Log::shutdown at once: one replacement, every file write by a writer thread,
//                    every line once in its thread's order, the final census exact.
//   R1-R2 recover    a dead writer, a 1 s-per-write disk and a full 16 KB queue, found dead first by the
//                    waiting producer (R1) or by a flush (R2): no file write on a producer or flusher
//                    thread, flush(200) returns within its budget, nothing lost.
//   C6b crashfull4   C6 with a 4 s write: the [CRASH] line queued in the reserve lands in the file.
//   F2  retry        two partial failed writes: the retry continues where the write stopped - every
//                    line once, no fragment, lost=0.
//   F3  lost         a batch that fails all its tries: counted lost (census lost=1), a [LOG_STALL]
//                    write FAILED line, the torn line ended, flush() false from then on, later lines land.
//   S2  sustained    every write 150 ms: the slow-write witness never feeds itself, it is aggregated to
//                    at most one line a second (count=), and Log::shutdown ends in < 4 s.
//   H2  dbghelp      the process-wide DbgHelp lock: exclusive, refuses re-entry, honours its timeout.
//   K1  kill         TerminateProcess 100 ms after 500 lines logged to an idle writer (Stop-Process -Force):
//                    all 500 there - the writer wakes on a line, it does not wait for a timer.
//   F1  flush        flush() true with the line in the file; false under a stall (line not there
//                    yet), then true; noteIoStall over / under 100 ms.
//   Q1-Q2 tryWriteLine graceful-quit's deadline line: tryWriteLine then TerminateProcess keeps it and every line
//                    before it; under a stall it returns false after its 1 s budget and the line lands later.
//   H1  hangdump     the hang watchdog's dump (writeDump as a trigger takes it) writes its files on the dump
//                    helper thread, never the calling thread; the sidecar is written; no minidump by default.
//   P1  player       the player profile keeps [IO_STALL] and the [LOG_STALL] census.
//   P2  periodic     an automated run (SR_INSTANCE_ID): a census line every 10 s, bytes= exact while two
//                    threads log multi-line records (the live graders' witness and completeness check).
//   B1-B2 same bytes the d71946e2 logger (tools/difftest/log_legacy_ref.cpp) and this one run the
//                    same calls (dev, player): log files and stderr identical once timestamps are
//                    masked and the [LOG_STALL] lines removed; every census bytes= exact.

#include "app/crash_handler.hpp"
#include "app/log.hpp"
#include "kernel/gpu_hang_wd.hpp"
#include "log_legacy_ref.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::printf("  %s ", ok ? "ok  " : "FAIL");
  std::vprintf(fmt, a);
  std::printf("\n");
  va_end(a);
  (ok ? g_pass : g_fail)++;
}

double msSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

std::string readAll(const std::string& p) {
  std::string s;
  HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return s;
  char buf[1 << 16];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) s.append(buf, got);
  CloseHandle(h);
  return s;
}

// Physical lines of a log file (text mode: "\r\n"); a last line without one is kept.
std::vector<std::string> fileLines(const std::string& d) {
  std::vector<std::string> v;
  size_t at = 0;
  while (at < d.size()) {
    size_t e = d.find("\r\n", at);
    if (e == std::string::npos) {
      v.push_back(d.substr(at));
      break;
    }
    v.push_back(d.substr(at, e - at));
    at = e + 2;
  }
  return v;
}

int countSub(const std::string& d, const char* s) {
  int n = 0;
  for (size_t at = d.find(s); at != std::string::npos; at = d.find(s, at + 1)) ++n;
  return n;
}

bool parseU64(const std::string& line, const char* key, unsigned long long* out) {
  const size_t k = line.find(key);
  if (k == std::string::npos) return false;
  *out = std::strtoull(line.c_str() + k + std::strlen(key), nullptr, 10);
  return true;
}

double parseD(const std::string& line, const char* key) {
  const size_t k = line.find(key);
  return k == std::string::npos ? -1.0 : std::atof(line.c_str() + k + std::strlen(key));
}

// Every [LOG_STALL] census line: bytes= must equal the raw line bytes before it (file bytes before
// its start minus the '\r' the text translation added per '\n'). Returns the failures.
int censusCheck(const std::string& d, int* nCensus, bool* haveFinal, unsigned long long* finalWaits,
                unsigned long long* finalLines, unsigned long long* finalUnwitnessed = nullptr,
                int* nPeriodic = nullptr) {
  if (nPeriodic) *nPeriodic = 0;
  int bad = 0;
  *nCensus = 0;
  *haveFinal = false;
  size_t lineStart = 0;
  unsigned long long newlines = 0;
  while (lineStart < d.size()) {
    size_t e = d.find('\n', lineStart);
    if (e == std::string::npos) e = d.size();
    const std::string line = d.substr(lineStart, e - lineStart);
    if (line.find("] [LOG_STALL] census ") != std::string::npos) {
      ++*nCensus;
      unsigned long long bytes = ~0ull;
      parseU64(line, " bytes=", &bytes);
      const unsigned long long raw = lineStart - newlines;
      if (bytes != raw) {
        ++bad;
        std::printf("    census at file offset %zu: bytes=%llu, raw bytes before it %llu\n", lineStart, bytes, raw);
      }
      if (line.find("] [LOG_STALL] census final ") != std::string::npos) {
        *haveFinal = true;
        parseU64(line, " waits=", finalWaits);
        parseU64(line, " lines=", finalLines);
        if (finalUnwitnessed) parseU64(line, " unwitnessed=", finalUnwitnessed);
      }
      if (nPeriodic && line.find("] [LOG_STALL] census periodic ") != std::string::npos) ++*nPeriodic;
    }
    if (e < d.size()) ++newlines;
    lineStart = e + 1;
  }
  return bad;
}

// ---------------------------------------------------------------------------------------- children
struct Env {
  const char* name;
  const char* value;  // nullptr = unset
};
const char* kEnvNames[] = {"SR_LOG", "SR_LOG_QUEUE_KB", "SR_LOG_INJECT_STALL", "SR_LOG_CONSOLE", "SR_INSTANCE_ID"};

std::string g_dir;  // absolute, ends with '\'

std::string path(const char* leaf) { return g_dir + leaf; }

// Runs `difftest --log-async-child <mode> <log>` with only SR_LOG=dev and `env` set among kEnvNames; the child's
// stdout -> <log>.out, stderr -> <log>.err; prints the child's stdout. Returns its exit code
// (0xFFFFFFFF on a timeout, after killing it).
DWORD runChild(const char* mode, const std::string& log, std::initializer_list<Env> env, DWORD timeoutMs = 60000,
               const char* cwd = nullptr) {
  for (const char* n : kEnvNames) SetEnvironmentVariableA(n, nullptr);
  SetEnvironmentVariableA("SR_LOG", "dev");  // every line (unset + no SR_INSTANCE_ID = the player profile)
  for (const Env& e : env) SetEnvironmentVariableA(e.name, e.value);
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  const std::string outP = log + ".out", errP = log + ".err";
  HANDLE out = CreateFileA(outP.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
  HANDLE err = CreateFileA(errP.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
  char exe[MAX_PATH * 2];
  GetModuleFileNameA(nullptr, exe, sizeof exe);
  std::string cmd = std::string("\"") + exe + "\" --log-async-child " + mode + " \"" + log + "\"";
  STARTUPINFOA si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out;
  si.hStdError = err;
  PROCESS_INFORMATION pi{};
  // The child lives in a kill-on-close job: if this parent dies (a suite timeout kills it), the child goes too,
  // instead of lingering (a difftest process commits 4 GB of guest memory).
  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
  }
  const BOOL ok = CreateProcessA(exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                 nullptr, cwd, &si, &pi);
  for (const char* n : kEnvNames) SetEnvironmentVariableA(n, nullptr);
  CloseHandle(out);
  CloseHandle(err);
  if (!ok) {
    if (job) CloseHandle(job);
    return 0xFFFFFFFEu;
  }
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  DWORD code = 0xFFFFFFFFu;
  if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
    GetExitCodeProcess(pi.hProcess, &code);
  } else {
    TerminateProcess(pi.hProcess, 0xFFFFFFFFu);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (job) CloseHandle(job);
  const std::string o = readAll(outP);
  if (!o.empty()) std::printf("%s", o.c_str());
  return code;
}

using LogW = sr::Log;
#define LW(...) sr::Log::write(sr::LogCategory::Boot, sr::LogSeverity::Info, __VA_ARGS__)

// The deterministic per-line payload of the stress case: thread t, line i.
std::string stressPayload(int t, int i) {
  const int len = (t * 131 + i * 17) % 700;
  std::string s(size_t(len), 'a');
  for (int k = 0; k < len; ++k) s[size_t(k)] = char('a' + (t + i + k) % 26);
  return s;
}

int childOrder(const char* log) {
  LogW::init(log);
  constexpr int N = 8, M = 1500;
  std::atomic<int> turn{0};
  std::vector<std::thread> th;
  for (int t = 0; t < N; ++t)
    th.emplace_back([t, &turn] {
      for (int k = 0; k < M; ++k) {
        const int mine = k * N + t;
        while (turn.load(std::memory_order_acquire) != mine) std::this_thread::yield();
        LW("[LAT] order seq=%d t=%d k=%d", mine, t, k);
        turn.store(mine + 1, std::memory_order_release);
      }
    });
  for (auto& x : th) x.join();
  LogW::shutdown();
  return 0;
}

int childStress(const char* log) {
  LogW::init(log);
  constexpr int N = 16, M = 4000;
  std::vector<std::thread> th;
  for (int t = 0; t < N; ++t)
    th.emplace_back([t] {
      for (int i = 0; i < M; ++i) {
        const std::string p = stressPayload(t, i);
        LW("[LAT] stress tid=%lu t=%d i=%d p=%s end", GetCurrentThreadId(), t, i, p.c_str());
      }
    });
  for (auto& x : th) x.join();
  LogW::shutdown();
  return 0;
}

int childSlowDisk(const char* log) {
  LogW::init(log);
  LW("[LAT] slow first");  // the writer takes it and sleeps 5 s inside that write
  Sleep(300);
  const std::string before = readAll(log);
  constexpr int N = 4, M = 2000;
  std::atomic<long long> maxUs{0};
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> th;
  for (int t = 0; t < N; ++t)
    th.emplace_back([t, &maxUs] {
      for (int i = 0; i < M; ++i) {
        const auto a = std::chrono::steady_clock::now();
        LW("[LAT] slow t=%d i=%d payload=%0120d", t, i, i);
        const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - a).count();
        long long cur = maxUs.load();
        while (us > cur && !maxUs.compare_exchange_weak(cur, us)) {
        }
      }
    });
  for (auto& x : th) x.join();
  const double total = msSince(t0);
  const std::string during = readAll(log);
  int bad = 0;
  std::printf("    child: %d lines in %.1f ms, max Log::write %.2f ms, file %zu -> %zu bytes while the writer slept\n",
              N * M, total, double(maxUs.load()) / 1000.0, before.size(), during.size());
  if (total >= 2000.0) ++bad, std::printf("    child FAIL: producers took %.1f ms (>= 2000) - they waited for the disk\n", total);
  if (maxUs.load() >= 500000) ++bad, std::printf("    child FAIL: one Log::write took %.1f ms (>= 500)\n", maxUs.load() / 1000.0);
  if (countSub(during, "[LAT] slow t=") != 0)
    ++bad, std::printf("    child FAIL: producer lines reached the file while the writer slept (no stall injected?)\n");
  LogW::shutdown();
  return bad;
}

int childFullWait(const char* log) {
  LogW::init(log);
  LW("[LAT] full first");
  Sleep(100);  // the writer is now inside its 1.5 s write
  long long maxUs = 0;
  for (int i = 0; i < 1500; ++i) {  // ~1500 x 200 B = 300 KB through a 64 KB queue
    const auto a = std::chrono::steady_clock::now();
    LW("[LAT] full i=%d payload=%0160d", i, i);
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - a).count();
    if (us > maxUs) maxUs = us;
  }
  std::printf("    child: max Log::write %.1f ms\n", maxUs / 1000.0);
  LogW::shutdown();
  return maxUs >= 1000000 ? 0 : 1;  // it must have waited for the stalled writer
}

// Lines A (written), then B and C queued behind a writer that sleeps inside its write.
void pendingLines() {
  for (int i = 0; i < 100; ++i) LW("[LAT] pend A i=%d", i);
  Sleep(700);  // past the injection time (SR_LOG_INJECT_STALL=<ms>@0.5)
  for (int i = 0; i < 50; ++i) LW("[LAT] pend B i=%d", i);
  Sleep(50);   // the writer took B and sleeps
  for (int i = 0; i < 50; ++i) LW("[LAT] pend C i=%d", i);
}

int childCrash(const char* log) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  LogW::init(log);
  sr::installCrashHandler();
  pendingLines();
  LW("[LAT] pend LAST before the fault");
  *static_cast<volatile int*>(nullptr) = 1;
  return 0;
}

int childAbort(const char* log) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  LogW::init(log);
  pendingLines();
  LW("[LAT] pend LAST before abort");
  std::abort();
}

int childExit(const char* log) {
  LogW::init(log);
  pendingLines();
  LW("[LAT] pend LAST before _Exit");
  std::_Exit(7);
}

// Bursts so the writer forms several batches; it dies in its 3rd with half of it on disk.
void midBatchLines() {
  int n = 0;
  for (int burst = 0; burst < 6; ++burst) {
    for (int i = 0; i < 120; ++i, ++n) LW("[LAT] mid n=%d payload=%0200d", n, n);
    Sleep(60);
  }
}

// The writer ends with half of its `nth` file write on disk (Log::testSeams).
void exitMidWrite(unsigned mask, int respawnLimit = -1) {
  sr::Log::TestSeams s;
  s.exitMidWriteMask = mask;
  s.respawnLimit = respawnLimit;
  sr::Log::testSeams(s);
}

int childMidBatch(const char* log) {
  exitMidWrite(1u << 2);
  LogW::init(log);
  midBatchLines();
  std::_Exit(5);
}

int childMidFlush(const char* log) {
  exitMidWrite(1u << 2);
  LogW::init(log);
  midBatchLines();
  const bool ok = LogW::flush(5000);
  LogW::shutdown();
  return ok ? 0 : 1;
}

// Exit during the takeover: the writer ends in write 3, its replacement ends in write 4 (its resume of that
// batch), no further replacement is allowed; then std::_Exit - the exit drain finishes the same journal.
int childTakeExit(const char* log) {
  exitMidWrite((1u << 2) | (1u << 3), 1);
  LogW::init(log);
  midBatchLines();
  const bool ok = LogW::flush(1500);
  std::printf("    child: flush with both writers ended returned %d\n", ok);
  std::fflush(stdout);
  std::_Exit(ok ? 1 : 6);
}

// Every file write's thread (Log::TestSeams::onFileWrite).
std::atomic<int> g_nWrites{0};
unsigned long g_writeTid[8192];
void recordWriteTid(unsigned long tid) {
  const int i = g_nWrites.fetch_add(1);
  if (i < 8192) g_writeTid[i] = tid;
}

// The game-side threads of a case (no file write may happen on one).
struct GameTids {
  std::atomic<int> n{0};
  unsigned long tid[64] = {};
  void mark() {
    const int i = n.fetch_add(1);
    if (i < 64) tid[i] = GetCurrentThreadId();
  }
  bool has(unsigned long t) const {
    for (int i = 0; i < n.load() && i < 64; ++i)
      if (tid[i] == t) return true;
    return false;
  }
};

// {file writes on a game thread, distinct writing threads}
std::pair<int, int> writeThreads(const GameTids& game) {
  int foreign = 0;
  std::vector<unsigned long> seen;
  const int n = g_nWrites.load() < 8192 ? g_nWrites.load() : 8192;
  for (int i = 0; i < n; ++i) {
    if (game.has(g_writeTid[i])) ++foreign;
    bool dup = false;
    for (unsigned long s : seen) dup |= s == g_writeTid[i];
    if (!dup) seen.push_back(g_writeTid[i]);
  }
  return {foreign, int(seen.size())};
}

// Concurrent takeover and shutdown: the writer ends in write 3 while 3 producers log; 4 threads flush in a loop
// and 2 threads call Log::shutdown at once.
int childTakeover(const char* log) {
  sr::Log::TestSeams s;
  s.exitMidWriteMask = 1u << 2;
  s.onFileWrite = &recordWriteTid;
  sr::Log::testSeams(s);
  LogW::init(log);
  GameTids game;
  game.mark();
  std::atomic<bool> stopFlush{false};
  std::vector<std::thread> producers, flushers, closers;
  for (int t = 0; t < 3; ++t)
    producers.emplace_back([t, &game] {
      game.mark();
      for (int i = 0; i < 400; ++i) {
        LW("[LAT] tko t=%d i=%d payload=%0150d", t, i, i);
        if (i % 50 == 49) Sleep(5);
      }
    });
  for (int f = 0; f < 4; ++f)
    flushers.emplace_back([&game, &stopFlush] {
      game.mark();
      while (!stopFlush.load()) LogW::flush(50);
    });
  for (auto& x : producers) x.join();
  for (int c = 0; c < 2; ++c)
    closers.emplace_back([&game] {
      game.mark();
      LogW::shutdown();
    });
  for (auto& x : closers) x.join();
  stopFlush.store(true);
  for (auto& x : flushers) x.join();
  const auto wt = writeThreads(game);
  std::printf("    child: %d file writes by %d writer threads, %d of them on a game thread\n", g_nWrites.load(),
              wt.second, wt.first);
  return wt.first == 0 && wt.second == 2 ? 0 : 1;
}

// Recovery off the game threads: the writer ends in write 2; then every write takes 1 s while a producer pushes
// 63 KB through a 16 KB queue (it waits, and a replacement writer does the writes) and the main thread asks
// flush(200), which must return within its budget. flushFirst: the main thread's flush is the first to find the
// writer dead; otherwise the waiting producer is.
int childRecover(const char* log, bool flushFirst) {
  int bad = 0;
  sr::Log::TestSeams s;
  s.exitMidWriteMask = 1u << 1;
  s.onFileWrite = &recordWriteTid;
  sr::Log::testSeams(s);
  LogW::init(log);
  GameTids game;
  game.mark();
  Sleep(200);
  LW("[LAT] rec first");  // write 2: the writer ends with half of it on disk
  Sleep(300);
  s.slowWriteMs = 1000;
  sr::Log::testSeams(s);
  bool f1 = true;
  double ms = 0;
  auto timedFlush = [&] {
    const auto t0 = std::chrono::steady_clock::now();
    f1 = LogW::flush(200);
    ms = msSince(t0);
  };
  if (flushFirst) timedFlush();
  std::thread producer([&game] {
    game.mark();
    for (int i = 0; i < 300; ++i) LW("[LAT] rec i=%d payload=%0160d", i, i);
  });
  if (!flushFirst) {
    Sleep(150);
    timedFlush();
  }
  producer.join();
  const bool f2 = LogW::flush(20000);
  LogW::shutdown();
  const auto wt = writeThreads(game);
  std::printf("    child: flush(200) with a dead writer and a 1 s disk returned %d after %.1f ms; %d file writes by %d "
              "writer threads, %d on a game thread; final flush %d\n",
              f1, ms, g_nWrites.load(), wt.second, wt.first, f2);
  if (f1 || ms > 600.0) ++bad, std::printf("    child FAIL: flush(200) did not keep its budget\n");
  if (wt.first != 0) ++bad, std::printf("    child FAIL: a game thread wrote the file\n");
  if (wt.second != 2) ++bad, std::printf("    child FAIL: want 2 writer threads (the first and its replacement)\n");
  if (!f2) ++bad, std::printf("    child FAIL: the replacement never drained the queue\n");
  return bad;
}

// Two partial failed writes, then the retry lands the rest.
int childRetry(const char* log) {
  LogW::init(log);
  for (int i = 0; i < 50; ++i) LW("[LAT] retry a i=%d", i);
  const bool f1 = LogW::flush(5000);
  sr::Log::TestSeams s;
  s.failWrites = 2;
  sr::Log::testSeams(s);
  for (int i = 0; i < 50; ++i) LW("[LAT] retry b i=%d payload=%0100d", i, i);
  const bool f2 = LogW::flush(5000);
  LogW::shutdown();
  std::printf("    child: flush before %d, after two failed writes %d\n", f1, f2);
  return f1 && f2 ? 0 : 1;
}

// A batch that fails all its tries: lost, said so, and flush() is false from then on.
int childLost(const char* log) {
  int bad = 0;
  LogW::init(log);
  LW("[LAT] lost A");
  const bool f1 = LogW::flush(5000);
  sr::Log::TestSeams s;
  s.failWrites = 3;
  sr::Log::testSeams(s);
  LW("[LAT] lost B the line the disk refuses");
  const bool f2 = LogW::flush(5000);
  LW("[LAT] lost C");
  const bool f3 = LogW::flush(5000);
  LogW::shutdown();
  const bool f4 = LogW::flush(10);
  std::printf("    child: flush before %d, after the lost batch %d, after a later line %d, after shutdown %d\n", f1, f2,
              f3, f4);
  if (!f1 || f2 || f3 || f4) ++bad, std::printf("    child FAIL: want 1 0 0 0\n");
  return bad;
}

// Every write takes 150 ms.
int childSustained(const char* log) {
  int bad = 0;
  sr::Log::TestSeams s;
  s.slowWriteMs = 150;
  sr::Log::testSeams(s);
  LogW::init(log);
  for (int i = 0; i < 5; ++i) LW("[LAT] sus a i=%d", i);
  Sleep(2500);
  const int n1 = countSub(readAll(log), "] [LOG_STALL] write tid=");
  Sleep(3000);  // nothing logged: no witness may follow its own (slow) write
  const int n2 = countSub(readAll(log), "] [LOG_STALL] write tid=");
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; msSince(t0) < 3000.0; ++i) {
    LW("[LAT] sus b i=%d", i);
    Sleep(20);
  }
  const auto s0 = std::chrono::steady_clock::now();
  LogW::shutdown();
  const double shutMs = msSince(s0);
  std::printf("    child: write lines %d at 2.5 s, %d at 5.5 s (nothing logged between); Log::shutdown %.1f ms\n", n1,
              n2, shutMs);
  if (n1 < 1) ++bad, std::printf("    child FAIL: no [LOG_STALL] write line for the slow writes\n");
  if (n2 != n1) ++bad, std::printf("    child FAIL: the slow-write witness fed itself\n");
  if (shutMs > 4000.0) ++bad, std::printf("    child FAIL: Log::shutdown took %.1f ms under a slow disk\n", shutMs);
  return bad;
}

// A crash while the queue is full behind a writer stuck in a long write (SR_LOG_INJECT_STALL=<ms>@0.3: 120 s in
// C6, 4 s in C6b).
int childCrashFull(const char* log) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  LogW::init(log);
  sr::installCrashHandler();
  Sleep(400);
  LW("[LAT] crashfull first");  // the writer takes it and sleeps 120 s inside the write
  Sleep(100);
  // Fills the 16 KB with lines SHORTER than the crash filter's [CRASH] line (~100 bytes), then waits: the gap a
  // waiting filler leaves is under one of its records, so the [CRASH] line cannot fit in the ordinary part - only
  // the emergency path's reserve takes it (a 200-byte filler could leave a gap the [CRASH] line fits in).
  std::thread([] {
    for (int i = 0; i < 5000; ++i) LW("[LAT] cf i=%d", i);
  }).detach();
  Sleep(500);
  std::printf("    child: faulting with the queue full\n");
  std::fflush(stdout);
  *static_cast<volatile int*>(nullptr) = 1;
  return 0;
}

// A vectored handler that logs, with no Log::EmergencyScope (kernel/image_write_watchdog.cpp's VEH does): on the
// faulting thread, before the crash filter.
LONG CALLBACK loggingVeh(EXCEPTION_POINTERS* ep) {
  static std::atomic<bool> s_once{false};  // the first access violation only (not one DbgHelp may raise later)
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !s_once.exchange(true))
    LW("[LAT] veh saw the access violation");
  return EXCEPTION_CONTINUE_SEARCH;
}

// A fault inside the logger, the log lock held (Log::TestSeams::faultInLock): a VEH logs (ordinary path), then the
// crash filter (emergency path).
int childCrashInLock(const char* log) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  LogW::init(log);
  sr::installCrashHandler();
  AddVectoredExceptionHandler(1, &loggingVeh);
  for (int i = 0; i < 100; ++i) LW("[LAT] inlock i=%d", i);
  LogW::flush(5000);
  sr::Log::TestSeams s;
  s.faultInLock = true;
  sr::Log::testSeams(s);
  std::printf("    child: faulting inside the log lock\n");
  std::fflush(stdout);
  LW("[LAT] inlock the faulting line");
  return 0;
}

int childKill(const char* log) {
  LogW::init(log);
  LW("[LAT] kill first");
  Sleep(250);  // the writer has written it and sleeps: the burst below must wake it
  for (int i = 0; i < 500; ++i) LW("[LAT] kill i=%d", i);
  Sleep(100);
  LW("[LAT] kill LAST");
  TerminateProcess(GetCurrentProcess(), 9);
  return 0;
}

// graceful-quit's hard deadline (kernel/quit.cpp deadlineThread): tryWriteLine, then TerminateProcess at once.
int childDeadline(const char* log) {
  LogW::init(log);
  for (int i = 0; i < 300; ++i) LW("[LAT] deadline i=%d", i);
  const bool ok = LogW::tryWriteLine(sr::LogSeverity::Error, "[QUIT] terminal snapshot FAILED stage=deadline (test)");
  TerminateProcess(GetCurrentProcess(), ok ? 11u : 12u);
  return 0;
}

// tryWriteLine never waits long: under a 3 s writer stall it gives up after its 1 s flush budget, the line stays
// queued and lands later; after shutdown it refuses.
int childTryWrite(const char* log) {
  int bad = 0;
  LogW::init(log);
  Sleep(400);  // past SR_LOG_INJECT_STALL=3000@0.3
  LW("[LAT] try A");  // the writer takes it and sleeps 3 s inside the write
  Sleep(50);
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = LogW::tryWriteLine(sr::LogSeverity::Error, "[LAT] try B");
  const double ms = msSince(t0);
  std::printf("    child: tryWriteLine under the stall returned %d after %.1f ms\n", ok, ms);
  if (ok || ms > 2000.0 || ms < 900.0) ++bad, std::printf("    child FAIL: want false after ~1 s\n");
  LogW::shutdown();
  if (LogW::tryWriteLine(sr::LogSeverity::Error, "[LAT] try C after shutdown"))
    ++bad, std::printf("    child FAIL: tryWriteLine after shutdown returned true\n");
  return bad;
}

int childFlush(const char* log) {
  int bad = 0;
  LogW::init(log);
  LW("[LAT] flush X");
  const bool f1 = LogW::flush(2000);
  if (!f1 || countSub(readAll(log), "[LAT] flush X") != 1) ++bad, std::printf("    child FAIL: flush() before the stall\n");
  sr::Log::noteIoStall("unit_over", 4096, 150.0);
  sr::Log::noteIoStall("unit_under", 4096, 50.0);
  Sleep(600);  // past SR_LOG_INJECT_STALL=1500@0.4
  LW("[LAT] flush Y");
  const bool f2 = LogW::flush(200);
  const int yNow = countSub(readAll(log), "[LAT] flush Y");
  if (f2 || yNow != 0) ++bad, std::printf("    child FAIL: flush(200) under a 1.5 s stall returned %d with Y %d times in the file\n", f2, yNow);
  const bool f3 = LogW::flush(5000);
  if (!f3 || countSub(readAll(log), "[LAT] flush Y") != 1) ++bad, std::printf("    child FAIL: flush(5000) after the stall\n");
  LogW::shutdown();
  if (!LogW::flush(10)) ++bad, std::printf("    child FAIL: flush() after shutdown\n");
  return bad;
}

// The hang watchdog's dump (kernel/gpu_hang_wd.cpp writeDump, as a trigger takes it): its files are written
// by the dump helper thread, not the calling (watchdog) thread; the minidump is off by default.
int childHangDump(const char* log) {
  // The dump files are named by pid, and pids are reused: a file a previous run left (the M15 mutant writes a real
  // .dmp) must not be read as this run's.
  char bin[96], dmp[96];
  std::snprintf(bin, sizeof bin, "archive\\gpu_hang_%lu_1.bin", GetCurrentProcessId());
  std::snprintf(dmp, sizeof dmp, "archive\\gpu_hang_%lu_1.dmp", GetCurrentProcessId());
  DeleteFileA(bin);
  DeleteFileA(dmp);
  LogW::init(log);
  const unsigned long me = GetCurrentThreadId();
  const double ms = sr::kernel::gpuHangWdDumpForTest();
  std::string line;
  for (int i = 0; i < 200 && line.empty(); ++i) {
    LogW::flush(100);
    for (const auto& l : fileLines(readAll(log)))
      if (l.find("] [GPU_HANG] dump #1 (test) -> ") != std::string::npos) line = l;
    if (line.empty()) Sleep(50);
  }
  LogW::shutdown();
  unsigned long long tid = 0;
  parseU64(line, "helper thread tid=", &tid);
  const std::string b = readAll(bin);
  const bool dmpExists = GetFileAttributesA(dmp) != INVALID_FILE_ATTRIBUTES;
  std::printf("    child: writeDump returned in %.1f ms on tid %lu; the dump line came from tid %llu; sidecar %zu "
              "bytes; minidump %s\n", ms, me, tid, b.size(), dmpExists ? "PRESENT" : "absent");
  int bad = 0;
  if (line.empty()) ++bad, std::printf("    child FAIL: no [GPU_HANG] dump line within 10 s\n");
  if (!tid || tid == me) ++bad, std::printf("    child FAIL: the dump files were written on the calling thread\n");
  if (line.find("no minidump (off;") == std::string::npos || dmpExists)
    ++bad, std::printf("    child FAIL: the minidump is not off by default\n");
  if (b.size() < 16 || b.compare(0, 4, "GHW1") != 0) ++bad, std::printf("    child FAIL: no GHW1 sidecar\n");
  return bad;
}

// An automated run (SR_INSTANCE_ID): the census every 10 s while two threads log.
int childPeriodic(const char* log) {
  LogW::init(log);
  std::atomic<bool> stop{false};
  std::thread other([&stop] {
    for (int i = 0; !stop.load(); ++i) {
      LW("[LAT] periodic other i=%d", i);
      Sleep(7);
    }
  });
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; msSince(t0) < 21500.0; ++i) {
    LW("[LAT] periodic main i=%d\nwith a second line", i);
    Sleep(5);
  }
  stop.store(true);
  other.join();
  LogW::shutdown();
  return 0;
}

int childPlayer(const char* log) {
  LogW::init(log);
  LW("[LAT_DROPPED] an Info line the player profile drops");
  sr::Log::noteIoStall("player_over", 1, 250.0);
  LogW::shutdown();
  return 0;
}

// The same-bytes script, for either logger (L = the class, C / S its enums).
template <class L, class C, class S>
void writeVia(C c, S s, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  L::writeV(c, s, fmt, a);
  va_end(a);
}

template <class L, class C, class S>
void sameBytesScript() {
  for (int c = 0; c <= 8; ++c)
    for (int s = 0; s <= 4; ++s) L::write(static_cast<C>(c), static_cast<S>(s), "[LAT] sevcat c=%d s=%d", c, s);
  for (int i = 0; i < 10; ++i) L::write(C::Boot, S::Warn, "[LAT_WARN] one site, warn #%d", i);
  for (int i = 0; i < 70; ++i) L::write(C::Boot, S::Error, "[LAT_ERR] one site, error #%d", i);
  for (int i = 0; i < 5; ++i) L::write(C::Perf, S::Info, "[PERF] t=%ds milestone %d", i, i);
  L::write(C::Boot, S::Info, "[LAT_INFO] a non-milestone Info line");
  L::write(C::Boot, S::Error, "[GPU_HANG] a milestone Error line");
  const std::string big(5000, 'q');
  L::write(C::Boot, S::Info, "[PERF] long %s", big.c_str());
  L::write(C::KernelImport, S::Error, "[PERF] long import %s", big.c_str());
  L::write(C::Boot, S::Info, "[PERF] embedded\nnewline");
  L::write(C::Boot, S::Info, "[PERF] embedded crlf\r\nend");
  L::write(C::Boot, S::Info, "[PERF] lone cr\rend");
  L::write(C::Boot, S::Info, "[PERF] trailing newline\n");
  L::write(C::Boot, S::Info, "\n");
  L::write(C::Boot, S::Info, "%s", "");
  L::write(C::Boot, S::Info, "[PERF] utf8 \xC3\xA9 \xE2\x9A\xA0 \xE2\x9C\x93");
  L::write(C::Boot, S::Info, "[PERF] nul=%c|after", 0);
  L::write(C::Boot, S::Info, "[PERF] percent %% and %5.2f and %08X", 3.14159, 0xBEEFu);
  for (int i = 0; i < 2000; ++i) L::write(C::GfxCmd, S::Info, "[PM4_COV] F frame=%d pk=%d", i, i * 3);
  writeVia<L, C, S>(C::Audio, S::Debug, "[LAT] writeV direct %d", 42);
  L::shutdown();
}

int childSameBytes(const char* which, const char* log) {
  if (std::strcmp(which, "legacy") == 0) {
    sr_logref::Log::init(log);
    sameBytesScript<sr_logref::Log, sr_logref::LogCategory, sr_logref::LogSeverity>();
  } else {
    sr::Log::init(log);
    sameBytesScript<sr::Log, sr::LogCategory, sr::LogSeverity>();
  }
  return 0;
}

// ------------------------------------------------------------------------------------- the checks
// "[YYYY-MM-DD HH:MM:SS.mmm]" at a line start -> "[TS]"; then drop the [LOG_STALL] lines.
std::string maskedNoStall(const std::string& d) {
  static const std::regex ts(R"(^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}\])");
  std::string out;
  size_t at = 0;
  while (at < d.size()) {
    size_t e = d.find('\n', at);
    const size_t end = e == std::string::npos ? d.size() : e + 1;
    std::string line = d.substr(at, end - at);
    line = std::regex_replace(line, ts, "[TS]", std::regex_constants::format_first_only);
    if (line.rfind("[TS] [WARN ] [BOOT] [LOG_STALL] ", 0) != 0) out += line;
    at = end;
  }
  return out;
}

void firstDiff(const std::string& a, const std::string& b) {
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  std::printf("    first difference at byte %zu (sizes %zu / %zu): legacy '%s' / async '%s'\n", i, a.size(), b.size(),
              a.substr(i, 40).c_str(), b.substr(i, 40).c_str());
}

void caseOrder() {
  const std::string log = path("order.log");
  const DWORD rc = runChild("order", log, {});
  const auto lines = fileLines(readAll(log));
  int next = 0, bad = 0;
  for (const auto& l : lines) {
    const size_t k = l.find("[LAT] order seq=");
    if (k == std::string::npos) continue;
    const int seq = std::atoi(l.c_str() + k + 16);
    if (seq != next && bad++ < 3) std::printf("    expected seq=%d, found seq=%d\n", next, seq);
    next = seq + 1;
  }
  check(rc == 0 && bad == 0 && next == 8 * 1500,
        "O1 order: 8 token-passing threads, the file holds seq 0..11999 in enqueue order (rc=%lu, out of order %d, last %d)",
        rc, bad, next - 1);
}

void caseStress() {
  const std::string log = path("stress.log");
  const DWORD rc = runChild("stress", log, {{"SR_LOG_QUEUE_KB", "64"}, {"SR_LOG_INJECT_STALL", "300"}}, 120000);
  const std::string d = readAll(log);
  const auto lines = fileLines(d);
  std::map<int, int> nextI;
  std::map<int, unsigned long> tidOf;
  int torn = 0, order = 0, waitsInFile = 0, waitWrongThread = 0, total = 0;
  unsigned long prevTid = 0;
  bool prevWasStress = false;
  for (const auto& l : lines) {
    const size_t k = l.find("[LAT] stress tid=");
    if (k != std::string::npos) {
      unsigned long tid = 0;
      int t = -1, i = -1;
      char p[1024] = {};
      const int got = std::sscanf(l.c_str() + k, "[LAT] stress tid=%lu t=%d i=%d p=%1000s", &tid, &t, &i, p);
      const std::string expect = "[LAT] stress tid=" + std::to_string(tid) + " t=" + std::to_string(t) +
                                 " i=" + std::to_string(i) + " p=" + stressPayload(t, i) + " end";
      if (got < 3 || l.compare(k, std::string::npos, expect) != 0) {
        if (torn++ < 3) std::printf("    torn / wrong line: %.120s\n", l.c_str());
      } else {
        if (i != nextI[t] && order++ < 3) std::printf("    thread %d: expected i=%d, found %d\n", t, nextI[t], i);
        nextI[t] = i + 1;
        tidOf[t] = tid;
        ++total;
      }
      prevTid = tid;
      prevWasStress = true;
      continue;
    }
    if (l.find("] [LOG_STALL] producer wait tid=") != std::string::npos) {
      ++waitsInFile;
      unsigned long long tid = 0;
      parseU64(l, "producer wait tid=", &tid);
      if (!prevWasStress || tid != prevTid) ++waitWrongThread;
    }
    prevWasStress = false;
  }
  int complete = 0;
  for (int t = 0; t < 16; ++t) complete += nextI[t] == 4000;
  int nCensus = 0;
  bool final = false;
  unsigned long long finalWaits = ~0ull, finalLines = 0, unwitnessed = ~0ull;
  const int censusBad = censusCheck(d, &nCensus, &final, &finalWaits, &finalLines, &unwitnessed);
  check(rc == 0 && torn == 0 && order == 0 && complete == 16 && total == 16 * 4000,
        "S1 stress: 16 threads x 4000 lines through a 64 KB queue - every line once, byte-exact, in its thread's "
        "order (rc=%lu, lines %d, torn %d, out of order %d, threads complete %d)",
        rc, total, torn, order, complete);
  check(waitsInFile > 0 && waitWrongThread == 0 && final && unwitnessed != ~0ull &&
            finalWaits == (unsigned long long)waitsInFile + unwitnessed,
        "S1 wait accounting: %d [LOG_STALL] producer wait lines, each right after a line of its own thread "
        "(%d not); census final waits=%llu = lines + unwitnessed=%llu",
        waitsInFile, waitWrongThread, finalWaits, unwitnessed);
  check(censusBad == 0 && nCensus >= 1 && final,
        "S1 completeness: %d census line(s), every bytes= equals the raw bytes before it (%d wrong)", nCensus, censusBad);
  check(countSub(d, "op=file") >= 1 && countSub(d, "injected=1") == 1,
        "S1 the injected 300 ms write is reported once as [LOG_STALL] write ... injected=1");
}

void caseSlowDisk() {
  const std::string log = path("slowdisk.log");
  const DWORD rc = runChild("slowdisk", log, {{"SR_LOG_INJECT_STALL", "5000"}}, 60000);
  const std::string d = readAll(log);
  int ms5000 = 0;
  for (const auto& l : fileLines(d))
    if (l.find("] [LOG_STALL] write ") != std::string::npos && l.find("injected=1") != std::string::npos &&
        parseD(l, " ms=") >= 5000.0)
      ++ms5000;
  check(rc == 0, "D1 slow disk: while the writer sleeps 5 s, 4 producers below capacity finish 8000 lines without "
                 "waiting (child checks: total < 2 s, each Log::write < 500 ms, none on disk yet; rc=%lu)", rc);
  check(countSub(d, "[LAT] slow t=") == 8000 && ms5000 == 1 && countSub(d, "producer wait") == 0,
        "D1 then all 8000 land, [LOG_STALL] write ... injected=1 ms>=5000 once (%d), no producer waited",
        ms5000);
}

void caseFullWait() {
  const std::string log = path("fullwait.log");
  const DWORD rc = runChild("fullwait", log, {{"SR_LOG_QUEUE_KB", "64"}, {"SR_LOG_INJECT_STALL", "1500"}}, 60000);
  const std::string d = readAll(log);
  double maxWait = 0;
  int waits = 0;
  for (const auto& l : fileLines(d))
    if (l.find("] [LOG_STALL] producer wait ") != std::string::npos) {
      ++waits;
      const double ms = parseD(l, " ms=");
      if (ms > maxWait) maxWait = ms;
    }
  check(rc == 0 && countSub(d, "[LAT] full i=") == 1500 && waits >= 1 && maxWait >= 1000.0,
        "W1 full queue: the producer waits for the stalled writer (>= 1 s) and says so; nothing dropped "
        "(rc=%lu, %d wait lines, max %.1f ms)",
        rc, waits, maxWait);
}

// The mid-batch cases' file: "[LAT] mid n=" 0..719 each exactly once, in order, byte-exact, and every physical line
// a whole record. Returns the next n expected (720 when complete).
int midCheck(const std::string& d, int* bad, int* malformed) {
  int next = 0;
  for (const auto& l : fileLines(d)) {
    if (l.rfind("[20", 0) != 0) ++*malformed;  // every physical line is a whole record
    const size_t k = l.find("[LAT] mid n=");
    if (k == std::string::npos) continue;
    const int n = std::atoi(l.c_str() + k + 12);
    char want[300];
    std::snprintf(want, sizeof want, "[LAT] mid n=%d payload=%0200d", n, n);
    if (n != next || l.compare(k, std::string::npos, want) != 0) ++*bad;
    next = n + 1;
  }
  return next;
}

bool allPending(const std::string& d) {
  return countSub(d, "[LAT] pend A ") == 100 && countSub(d, "[LAT] pend B ") == 50 && countSub(d, "[LAT] pend C ") == 50 &&
         countSub(d, "[LAT] pend LAST") == 1;
}

void caseCrashPaths() {
  {
    const std::string log = path("crash.log");
    const DWORD rc = runChild("crash", log, {{"SR_LOG_INJECT_STALL", "800@0.5"}}, 60000, g_dir.c_str());
    const std::string d = readAll(log);
    check(rc == 0xC0000005u && allPending(d) && countSub(d, "] [CRASH] Exception 0xC0000005") == 1,
          "C1 crash: an access violation through the game's crash filter, a writer stall pending - every line and "
          "the [CRASH] line are in the file (rc=0x%08lX)",
          rc);
  }
  {
    const std::string log = path("abort.log");
    const DWORD rc = runChild("abort", log, {{"SR_LOG_INJECT_STALL", "800@0.5"}}, 60000);
    const std::string d = readAll(log);
    check(rc != 0 && rc != 0xFFFFFFFFu && allPending(d),
          "C2 abort: std::abort() (SIGABRT handler, then __fastfail) with a stall pending - every line (rc=0x%08lX)", rc);
  }
  {
    const std::string log = path("exit.log");
    const DWORD rc = runChild("exit", log, {{"SR_LOG_INJECT_STALL", "800@0.5"}}, 60000);
    const std::string d = readAll(log);
    check(rc == 7 && allPending(d),
          "C3 exit: std::_Exit with a stall pending - the TLS process-detach drain writes every line (rc=%lu)", rc);
  }
  for (int flush = 0; flush < 2; ++flush) {
    const std::string log = path(flush ? "midflush.log" : "midbatch.log");
    const DWORD rc = runChild(flush ? "midflush" : "midbatch", log, {}, 60000);
    int bad = 0, malformed = 0;
    const int next = midCheck(readAll(log), &bad, &malformed);
    check(rc == (flush ? 0u : 5u) && bad == 0 && malformed == 0 && next == 720,
          "%s: the writer died with half of write 3 on disk; %s resumes at the file position - 720 lines exactly "
          "once, in order, no fragment (rc=%lu, bad %d, malformed %d, last %d)",
          flush ? "C5 midflush" : "C4 midbatch",
          flush ? "the replacement writer Log::flush starts" : "the exit drain", rc, bad, malformed, next - 1);
  }
  {
    const std::string log = path("crashfull.log");
    const std::string tailLog = g_dir + "superman_returns_crash.log";
    DeleteFileA(tailLog.c_str());
    const auto t0 = std::chrono::steady_clock::now();
    const DWORD rc = runChild("crashfull", log, {{"SR_LOG_QUEUE_KB", "16"}, {"SR_LOG_INJECT_STALL", "120000@0.3"}},
                              30000, g_dir.c_str());
    const double ms = msSince(t0);
    const std::string tail = readAll(tailLog);
    check(rc == 0xC0000005u && countSub(tail, "] [CRASH] Exception 0xC0000005") == 1,
          "C6 crash with a full queue: the writer is stuck in a 120 s write and a producer waits on a full 16 KB "
          "queue; the crash filter's lines take the emergency path, its flushes time out, the process ends "
          "(rc=0x%08lX after %.1f s; the [CRASH] line %s the crash tail)",
          rc, ms / 1000.0, countSub(tail, "] [CRASH] Exception") ? "in" : "NOT in");
  }
  {
    const std::string log = path("crashfull4.log");
    const DWORD rc = runChild("crashfull", log, {{"SR_LOG_QUEUE_KB", "16"}, {"SR_LOG_INJECT_STALL", "4000@0.3"}},
                              30000, g_dir.c_str());
    const std::string d = readAll(log);
    // The [CRASH] line must come before the filler line that was waiting when the fault came: it was queued in
    // the reserve at once, not after the writer made room.
    const size_t crashAt = d.find("] [CRASH] Exception 0xC0000005");
    const size_t waitAt = d.find("] [LOG_STALL] producer wait ");
    check(rc == 0xC0000005u && countSub(d, "] [CRASH] Exception 0xC0000005") == 1 &&
              countSub(d, "[LAT] crashfull first") == 1 && crashAt != std::string::npos &&
              (waitAt == std::string::npos || crashAt < waitAt),
          "C6b the same with a 4 s write: the [CRASH] line, queued in the reserve while the 16 KB queue was full (ahead "
          "of the waiting producer's line), is in the file once the writer is back (rc=0x%08lX)",
          rc);
  }
  {
    const std::string log = path("crashinlock.log");
    const std::string tailLog = g_dir + "superman_returns_crash.log";
    DeleteFileA(tailLog.c_str());
    const DWORD rc = runChild("crashinlock", log, {}, 30000, g_dir.c_str());
    const std::string d = readAll(log), err = readAll(log + ".err"), tail = readAll(tailLog);
    check(rc == 0xC0000005u && countSub(d, "[LAT] inlock i=") == 100 &&
              countSub(err, "] [LAT] veh saw the access violation") == 1 &&
              countSub(err, "] [CRASH] Exception 0xC0000005") == 1 &&
              countSub(tail, "[LAT] inlock the faulting line") == 1,
          "C7 fault inside the logger (the log lock held): neither a VEH that logs (ordinary path) nor the crash "
          "filter (emergency path) spins on it - both lines reach stderr, the crash tail is written, the 100 lines "
          "before are in the file (rc=0x%08lX)",
          rc);
  }
  {
    const std::string log = path("takeexit.log");
    const DWORD rc = runChild("takeexit", log, {}, 60000);
    int bad = 0, malformed = 0;
    const int next = midCheck(readAll(log), &bad, &malformed);
    check(rc == 6 && bad == 0 && malformed == 0 && next == 720,
          "X1 exit during takeover: the writer died in write 3, its replacement died resuming it (write 4), then "
          "std::_Exit - the exit drain finishes the same journal: 720 lines exactly once, in order, no fragment "
          "(rc=%lu, bad %d, malformed %d, last %d)",
          rc, bad, malformed, next - 1);
  }
  {
    const std::string log = path("kill.log");
    const DWORD rc = runChild("kill", log, {}, 60000);
    const std::string d = readAll(log);
    check(rc == 9 && countSub(d, "[LAT] kill i=") == 500,
          "K1 kill: TerminateProcess (Stop-Process -Force) 100 ms after 500 lines - all 500 were already written "
          "(the last line, logged just before the kill, %s) (rc=%lu)",
          countSub(d, "[LAT] kill LAST") ? "too" : "not", rc);
  }
}

// Per-thread order of "[LAT] <tag> t=<t> i=<i> " lines: every i of every thread 0..threads-1 exactly once, in order.
int perThreadOrder(const std::string& d, const char* tag, int threads, int perThread, int* total) {
  std::map<int, int> nextI;
  int bad = 0;
  *total = 0;
  const std::string key = std::string("[LAT] ") + tag + " t=";
  for (const auto& l : fileLines(d)) {
    if (l.rfind("[20", 0) != 0) ++bad;  // a fragment
    const size_t k = l.find(key);
    if (k == std::string::npos) continue;
    int t = -1, i = -1;
    if (std::sscanf(l.c_str() + k + key.size(), "%d i=%d", &t, &i) != 2 || t < 0 || t >= threads || i != nextI[t])
      ++bad;
    nextI[t] = i + 1;
    ++*total;
  }
  for (int t = 0; t < threads; ++t) bad += nextI[t] != perThread;
  return bad;
}

void caseOwnership() {
  {
    const std::string log = path("takeover.log");
    const DWORD rc = runChild("takeover", log, {}, 90000);
    const std::string d = readAll(log);
    int total = 0, nCensus = 0;
    bool final = false;
    unsigned long long w = 0, l = 0;
    const int bad = perThreadOrder(d, "tko", 3, 400, &total);
    const int censusBad = censusCheck(d, &nCensus, &final, &w, &l);
    check(rc == 0 && bad == 0 && total == 1200 && final && censusBad == 0 &&
              countSub(d, "] [LOG_STALL] census final ") == 1,
          "X2 concurrent takeover and shutdown: the writer died in write 3 while 3 producers logged, 4 threads "
          "flushed in a loop and 2 called Log::shutdown at once - one replacement, every file write on a writer "
          "thread (child), 1200 lines once in their threads' order, one final census, bytes= exact (rc=%lu, bad %d, "
          "lines %d)",
          rc, bad, total);
  }
  for (int flushFirst = 0; flushFirst < 2; ++flushFirst) {
    const std::string log = path(flushFirst ? "recoverflush.log" : "recover.log");
    const DWORD rc = runChild(flushFirst ? "recoverflush" : "recover", log, {{"SR_LOG_QUEUE_KB", "16"}}, 90000);
    const std::string d = readAll(log);
    int malformed = 0, next = 0, badLine = 0;
    for (const auto& l : fileLines(d)) {
      if (l.rfind("[20", 0) != 0 || countSub(l, "] [BOOT] ") != 1) ++malformed;
      const size_t k = l.find("[LAT] rec i=");
      if (k == std::string::npos) continue;
      const int i = std::atoi(l.c_str() + k + 12);
      badLine += i != next;
      next = i + 1;
    }
    check(rc == 0 && malformed == 0 && badLine == 0 && next == 300 && countSub(d, "[LAT] rec first") == 1,
          "R%d recovery off the game threads: a dead writer, a 1 s-per-write disk, a producer on a full 16 KB queue; "
          "%s finds the writer dead first - a replacement writer does every write (none on the producer or the "
          "flusher, child), flush(200) keeps its budget, every line once in order (rc=%lu, malformed %d, out of "
          "order %d, last %d)",
          flushFirst ? 2 : 1, flushFirst ? "a flush(200)" : "the waiting producer", rc, malformed, badLine, next - 1);
  }
}

void caseWriteFailures() {
  {
    const std::string log = path("retry.log");
    const DWORD rc = runChild("retry", log, {}, 60000);
    const std::string d = readAll(log);
    int malformed = 0, a = 0, b = 0;
    for (const auto& l : fileLines(d)) {
      if (l.rfind("[20", 0) != 0 || countSub(l, "] [BOOT] ") != 1) ++malformed;  // one whole record per line
      a += l.find("[LAT] retry a i=") != std::string::npos;
      b += l.find("[LAT] retry b i=") != std::string::npos;
    }
    int nCensus = 0;
    bool final = false;
    unsigned long long w = 0, l = 0;
    const int censusBad = censusCheck(d, &nCensus, &final, &w, &l);
    check(rc == 0 && malformed == 0 && a == 50 && b == 50 && countSub(d, "write FAILED") == 0 && final &&
              censusBad == 0 && countSub(d, " lost=0 ") >= 1,
          "F2 retry: two partial failed writes, then the retry continues where the write stopped - 100 lines once, "
          "no fragment, no loss, census exact (rc=%lu, malformed %d, a %d, b %d)",
          rc, malformed, a, b);
  }
  {
    const std::string log = path("lost.log");
    const DWORD rc = runChild("lost", log, {}, 60000);
    const std::string d = readAll(log), err = readAll(log + ".err");
    const auto lines = fileLines(d);
    // The lost batch (B alone) left 7/8 of its line in the file (three half-writes); the next write must start
    // with "\r\n" so nothing is glued onto that fragment.
    int bLines = 0, wholeB = 0, glued = 0, cAtStart = 0, failedAtStart = 0;
    for (const auto& l : lines) {
      if (l.find("[LAT] lost B") != std::string::npos) {
        ++bLines;
        wholeB += l.find("the line the disk refuses") != std::string::npos;
        glued += countSub(l, "] [BOOT] ") != 1 || l.find("[LOG_STALL]") != std::string::npos ||
                 l.find("lost C") != std::string::npos;
      }
      if (l.rfind("[20", 0) != 0) continue;
      cAtStart += l.find("] [LAT] lost C") != std::string::npos;
      failedAtStart += l.find("] [LOG_STALL] write FAILED ") != std::string::npos;
    }
    unsigned long long lost = 0;
    for (const auto& l : lines)
      if (l.find("] [LOG_STALL] census final ") != std::string::npos) parseU64(l, " lost=", &lost);
    check(rc == 0 && countSub(d, "] [LAT] lost A") == 1 && bLines == 1 && wholeB == 0 && glued == 0 &&
              cAtStart == 1 && failedAtStart == 1 && lost == 1 && countSub(err, "write FAILED") >= 1,
          "F3 lost: a batch that fails all its tries counts lost (census final lost=%llu), says so in the file and "
          "on stderr ([LOG_STALL] write FAILED), its torn line is ended (%d glued), later lines land, flush() is false "
          "from then on (child) (rc=%lu)",
          lost, glued, rc);
  }
}

void caseSustained() {
  const std::string log = path("sustained.log");
  const DWORD rc = runChild("sustained", log, {}, 90000);
  const std::string d = readAll(log), err = readAll(log + ".err");
  int witness = 0;
  unsigned long long counted = 0;
  for (const auto& l : fileLines(d))
    if (l.find("] [LOG_STALL] write tid=") != std::string::npos) {
      ++witness;
      unsigned long long c = 0;
      parseU64(l, " count=", &c);
      counted += c;
    }
  int nCensus = 0;
  bool final = false;
  unsigned long long w = 0, l = 0;
  censusCheck(d, &nCensus, &final, &w, &l);
  check(rc == 0 && witness >= 2 && witness <= 9 && counted >= 12 && final &&
            countSub(err, "did not finish") == 0 && countSub(d, "[LAT] sus a ") == 5,
        "S2 sustained slow disk (every write 150 ms): the write witness never feeds itself, it is aggregated "
        "(%d [LOG_STALL] write lines covering %llu slow writes, at most one a second), Log::shutdown ends in < 4 s "
        "with its final census (rc=%lu)",
        witness, counted, rc);
}

void caseDbgHelpLock() {
  int bad = 0;
  // exclusive: 4 threads x 2000 critical sections; the inside count never exceeds 1
  std::atomic<int> inside{0}, maxInside{0};
  long long plain = 0;
  std::vector<std::thread> th;
  for (int t = 0; t < 4; ++t)
    th.emplace_back([&] {
      for (int i = 0; i < 2000; ++i) {
        sr::DbgHelpLock lk(0xFFFFFFFFu);
        const int now = inside.fetch_add(1) + 1;
        int m = maxInside.load();
        while (now > m && !maxInside.compare_exchange_weak(m, now)) {
        }
        ++plain;
        inside.fetch_sub(1);
      }
    });
  for (auto& x : th) x.join();
  if (maxInside.load() != 1 || plain != 8000) ++bad;
  // re-entry refused at once; a timeout honoured; granted once released
  double reMs = 0, toMs = 0, grantMs = 0;
  bool reHeld = true, toHeld = true, grantHeld = false;
  {
    sr::DbgHelpLock outer(0xFFFFFFFFu);
    const auto t0 = std::chrono::steady_clock::now();
    sr::DbgHelpLock inner(1000);
    reMs = msSince(t0);
    reHeld = inner.held();
  }
  std::atomic<bool> taken{false};
  std::thread holder([&taken] {
    sr::DbgHelpLock lk(0xFFFFFFFFu);
    taken.store(true);
    Sleep(400);
  });
  while (!taken.load()) Sleep(1);
  {
    const auto t0 = std::chrono::steady_clock::now();
    sr::DbgHelpLock lk(60);
    toMs = msSince(t0);
    toHeld = lk.held();
  }
  {
    const auto t0 = std::chrono::steady_clock::now();
    sr::DbgHelpLock lk(5000);
    grantMs = msSince(t0);
    grantHeld = lk.held();
  }
  holder.join();
  if (reHeld || reMs > 50.0 || toHeld || toMs < 50.0 || toMs > 300.0 || !grantHeld) ++bad;
  check(bad == 0,
        "H2 DbgHelp lock (every DbgHelp call in the host): exclusive across 4 threads (max inside %d, %lld sections), "
        "re-entry refused (%s in %.1f ms), a 60 ms timeout honoured (%s after %.1f ms), granted once released "
        "(%s after %.1f ms)",
        maxInside.load(), plain, reHeld ? "HELD" : "refused", reMs, toHeld ? "HELD" : "refused", toMs,
        grantHeld ? "held" : "REFUSED", grantMs);
}

void caseFlush() {
  const std::string log = path("flush.log");
  const DWORD rc = runChild("flush", log, {{"SR_LOG_INJECT_STALL", "1500@0.4"}}, 60000);
  const std::string d = readAll(log);
  check(rc == 0, "F1 flush: true with the line in the file; false within 200 ms under a 1.5 s stall (line not yet "
                 "there); true after it; true after shutdown (child checks, rc=%lu)", rc);
  check(countSub(d, "] [IO_STALL] what=unit_over ") == 1 && countSub(d, "what=unit_under") == 0,
        "F1 noteIoStall: a 150 ms write gives one [IO_STALL] line, a 50 ms one none");
}

void caseTryWrite() {
  {
    const std::string log = path("deadline.log");
    const DWORD rc = runChild("deadline", log, {}, 60000);
    const std::string d = readAll(log);
    check(rc == 11 && countSub(d, "[LAT] deadline i=") == 300 &&
              countSub(d, "] [ERROR] [BOOT] [QUIT] terminal snapshot FAILED stage=deadline (test)") == 1,
          "Q1 graceful-quit deadline: tryWriteLine then TerminateProcess - the line and the 300 before it are in "
          "the file (rc=%lu)", rc);
  }
  {
    const std::string log = path("trywrite.log");
    const DWORD rc = runChild("trywrite", log, {{"SR_LOG_INJECT_STALL", "3000@0.3"}}, 60000);
    const std::string d = readAll(log);
    check(rc == 0 && countSub(d, "[LAT] try B") == 1 && countSub(d, "try C after shutdown") == 0,
          "Q2 tryWriteLine under a 3 s stall: false after its 1 s budget (never longer), the line still lands; "
          "refused after shutdown (rc=%lu)", rc);
  }
}

void caseHangDump() {
  const std::string log = path("hangdump.log");
  const DWORD rc = runChild("hangdump", log, {}, 60000, g_dir.c_str());
  check(rc == 0, "H1 hang dump: writeDump hands its files to the dump helper thread (the line names another tid), "
                 "the .bin sidecar is written, no minidump without SR_GPU_HANG_DUMP=1 (rc=%lu)", rc);
}

void casePeriodic() {
  const std::string log = path("periodic.log");
  const DWORD rc = runChild("periodic", log, {{"SR_INSTANCE_ID", "logtest"}}, 90000);
  const std::string d = readAll(log);
  int nCensus = 0, nPeriodic = 0;
  bool final = false;
  unsigned long long w = 0, l = 0, u = 0;
  const int bad = censusCheck(d, &nCensus, &final, &w, &l, &u, &nPeriodic);
  check(rc == 0 && nPeriodic == 2 && final && bad == 0,
        "P2 automated run: a [LOG_STALL] census every 10 s (%d periodic in 21.5 s) and the final one, every bytes= "
        "exact while two threads log multi-line records (rc=%lu, %d wrong)",
        nPeriodic, rc, bad);
}

void casePlayer() {
  const std::string log = path("player.log");
  const DWORD rc = runChild("player", log, {{"SR_LOG", "player"}}, 60000);
  const std::string d = readAll(log);
  int nCensus = 0;
  bool final = false;
  unsigned long long w = 0, l = 0;
  const int bad = censusCheck(d, &nCensus, &final, &w, &l);
  check(rc == 0 && countSub(d, "[LOG_PROFILE] player") == 1 && countSub(d, "[LAT_DROPPED]") == 0 &&
            countSub(d, "] [IO_STALL] what=player_over ") == 1 && final && bad == 0,
        "P1 player profile: [IO_STALL] and the [LOG_STALL] census are kept, an Info line is dropped (rc=%lu)", rc);
}

void caseSameBytes() {
  for (const char* profile : {"dev", "player"}) {
    const std::string a = path((std::string("same_legacy_") + profile + ".log").c_str());
    const std::string b = path((std::string("same_async_") + profile + ".log").c_str());
    const DWORD ra = runChild("same-legacy", a, {{"SR_LOG", profile}}, 60000);
    const DWORD rb = runChild("same-async", b, {{"SR_LOG", profile}}, 60000);
    const std::string fa = readAll(a), fb = readAll(b);
    const std::string ma = maskedNoStall(fa), mb = maskedNoStall(fb);
    const bool fileSame = !ma.empty() && ma == mb;
    if (!fileSame) firstDiff(ma, mb);
    const std::string ea = maskedNoStall(readAll(a + ".err")), eb = maskedNoStall(readAll(b + ".err"));
    const bool errSame = !ea.empty() && ea == eb;
    if (!errSame) firstDiff(ea, eb);
    int nCensus = 0;
    bool final = false;
    unsigned long long w = 0, l = 0;
    const int bad = censusCheck(fb, &nCensus, &final, &w, &l);
    check(ra == 0 && rb == 0 && fileSame && errSame && final && bad == 0,
          "B %s profile: the d71946e2 logger and the async writer give the same log file (%zu bytes) and the same "
          "stderr (%zu bytes) for the same calls, timestamps masked and [LOG_STALL] lines removed; census exact",
          profile, ma.size(), ea.size());
  }
}

}  // namespace

int runLogAsyncChild(int argc, char** argv) {
  if (argc < 4) return 2;
  const std::string mode = argv[2];
  const char* log = argv[3];
  if (mode == "order") return childOrder(log);
  if (mode == "stress") return childStress(log);
  if (mode == "slowdisk") return childSlowDisk(log);
  if (mode == "fullwait") return childFullWait(log);
  if (mode == "crash") return childCrash(log);
  if (mode == "abort") return childAbort(log);
  if (mode == "exit") return childExit(log);
  if (mode == "midbatch") return childMidBatch(log);
  if (mode == "midflush") return childMidFlush(log);
  if (mode == "takeexit") return childTakeExit(log);
  if (mode == "takeover") return childTakeover(log);
  if (mode == "recover") return childRecover(log, false);
  if (mode == "recoverflush") return childRecover(log, true);
  if (mode == "retry") return childRetry(log);
  if (mode == "lost") return childLost(log);
  if (mode == "sustained") return childSustained(log);
  if (mode == "crashfull") return childCrashFull(log);
  if (mode == "crashinlock") return childCrashInLock(log);
  if (mode == "kill") return childKill(log);
  if (mode == "flush") return childFlush(log);
  if (mode == "player") return childPlayer(log);
  if (mode == "periodic") return childPeriodic(log);
  if (mode == "hangdump") return childHangDump(log);
  if (mode == "deadline") return childDeadline(log);
  if (mode == "trywrite") return childTryWrite(log);
  if (mode == "same-legacy") return childSameBytes("legacy", log);
  if (mode == "same-async") return childSameBytes("async", log);
  return 2;
}

int runLogAsyncTest() {
  std::printf("--log-async-test (app/log.cpp, docs/TOOLKIT.md 3.2)\n");
  char full[MAX_PATH * 2];
  CreateDirectoryA("archive", nullptr);
  CreateDirectoryA("archive\\log_async_test", nullptr);
  GetFullPathNameA("archive\\log_async_test\\", sizeof full, full, nullptr);
  g_dir = full;
  caseOrder();
  caseStress();
  caseSlowDisk();
  caseFullWait();
  caseCrashPaths();
  caseOwnership();
  caseWriteFailures();
  caseSustained();
  caseFlush();
  caseTryWrite();
  caseHangDump();
  caseDbgHelpLock();
  casePeriodic();
  casePlayer();
  caseSameBytes();
  std::printf("[LOG_ASYNC_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
