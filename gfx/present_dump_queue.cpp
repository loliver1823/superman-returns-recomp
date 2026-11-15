// Step 4 item 4(c): the process's present-dump writer (gfx/present_dump_writer.hpp) - SR_PRESENT_DUMP_QUEUE, the
// app/log lines and the exit drain. The queue itself is present_dump_writer.cpp.

#include "present_dump_writer.hpp"

#include "../app/log.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sr::gfx::dumpq {

uint32_t capacity() {
  static const uint32_t n = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_QUEUE");
    const long v = e ? std::strtol(e, nullptr, 10) : 0;
    return uint32_t(v > 0 ? (v > 4096 ? 4096 : v) : 0);
  }();
  return n;
}

namespace {

bool (*g_write)(const Job&) = nullptr;  // the BMP writer of d3d12_backend.cpp (file-local there), set by the first submit

bool writeJob(const Job& j) {
#if defined(_WIN32)
  static thread_local bool named = false;
  if (!named) {
    SetThreadDescription(GetCurrentThread(), L"SR present dump writer");
    named = true;
  }
#endif
  return g_write && g_write(j);
}

void logWritten(const Job& j, bool ok, double ms) {
  sr::Log::noteIoStall("present_dump_bmp", 54ull + uint64_t(j.w) * j.h * 4u, ms);
  // iter-447: the success line only when the file exists (writeBmp32 logs its own WRITE FAILED line)
  if (ok)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PRESENT_DUMP] %s nonBlackPx=%llu/%u (⚠ constant — see the note)",
                j.path.c_str(), (unsigned long long)j.nonBlack, j.w * j.h);
}

void logCensus(const Stats& s, const char* why) {
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PRESENT_DUMP_Q] %s: queued %llu written %llu failed %llu | queue high-water %llu of %u | presenting "
              "thread blocked %llu times, %.1f ms | refused after the exit closed the queue %llu "
              "(SR_PRESENT_DUMP_QUEUE, step 4 item 4(c))",
              why, (unsigned long long)s.queued, (unsigned long long)s.written, (unsigned long long)s.failed,
              (unsigned long long)s.highWater, capacity(), (unsigned long long)s.blockedTimes,
              double(s.blockedUs) * 1e-3, (unsigned long long)s.refused);
}

std::mutex g_mu;
Writer* g_writer = nullptr;  // never deleted: the atexit drain flushes it, the process exit ends its thread
std::atomic<bool> g_drained{false};
std::atomic<bool> g_drainClean{true};   // the first drainAtExit's outcome
// Astra (10:5x) #1: the exit's closure persists - set before the first writer exists, a later submit is refused too
std::atomic<bool> g_admissionClosed{false};
std::atomic<uint64_t> g_refusedNoWriter{0};

void atExitDrain() { (void)drainAtExit(30000); }

Writer& writer(bool (*write)(const Job&)) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_writer) {
    g_write = write;
    g_writer = new Writer(capacity(), Hooks{writeJob, logWritten, logCensus});
    std::atexit(atExitDrain);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PRESENT_DUMP_Q] on: the BMP writes go to one writer thread, at most %u frames queued (a full queue "
                "blocks the presenting thread; nothing is dropped)",
                capacity());
  }
  // a submit that passed the admission check while closeAdmission ran: its writer is closed from birth
  if (g_admissionClosed.load(std::memory_order_acquire)) g_writer->close();
  return *g_writer;
}

}  // namespace

void submit(Job&& job, bool (*write)(const Job&)) {
  if (g_admissionClosed.load(std::memory_order_acquire)) {   // closed at exit: never a writer created after it
    g_refusedNoWriter.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  writer(write).submit(std::move(job));
}

bool active() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_writer != nullptr;
}

void closeAdmission() {
  Writer* w;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_admissionClosed.store(true, std::memory_order_release);   // under g_mu: writer() cannot race past it
    w = g_writer;
  }
  if (w) w->close();
}

bool drainAtExit(uint32_t timeoutMs) {
  Writer* w;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    w = g_writer;
  }
  if (!w) return true;
  if (g_drained.exchange(true)) return g_drainClean.load();
  const uint64_t left = w->drain(timeoutMs);
  g_drainClean.store(left == 0);
  logCensus(w->stats(), left ? "exit drain TIMED OUT" : "exit drain");
  if (left)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PRESENT_DUMP_Q] %llu dump(s) still unwritten after %u ms at exit",
                (unsigned long long)left, timeoutMs);
  sr::Log::flush(2000);
  return left == 0;
}

}  // namespace sr::gfx::dumpq
