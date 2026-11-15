#pragma once

// Step 4 item 4(c) (lane D, 2026-10-20): the present dump's BMP writes off the presenting thread.
//
// Why: dumpBackBuffer wrote each BMP synchronously on the presenting thread (the PM4 render thread; the CP for the
// synchronous first frame). On the D: HDD single writes took up to 23 s (4(b), archive/step4_claude_20261018/timing
// NOTES 2026-10-20 02:32): the render thread stalled, the CP blocked handing it the next frame, presents stopped and the
// hang watchdog fired H1 with CP phase=swap. J: (SSD) and no-dump rolls never stalled.
//
// SR_PRESENT_DUMP_QUEUE=<n> (default unset / 0 = the synchronous write, unchanged): the presenting thread keeps the
// readback, the scan and [PRESENT_SIG]; the BGRA buffer (moved) and the file name go to ONE writer thread through a
// queue of at most n frames. A full queue BLOCKS the presenting thread until a slot frees (frames are never dropped:
// the every-present windows need every file) and the blocked time is counted. The writer logs the
// `[PRESENT_DUMP] <file> nonBlackPx=...` line only after the file exists (iter-447) and keeps the [IO_STALL] line for a
// slow write (it now names a writer-thread stall, not a frame stall). `[PRESENT_DUMP_Q]` census: every 120 files
// (kCensusEvery) and at the drain. Exit: drain() waits for the queue (bounded), at the graceful quit and at atexit.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sr::gfx::dumpq {

// The writer's census cadence (files written or failed between two `[PRESENT_DUMP_Q]` lines).
constexpr uint64_t kCensusEvery = 120;

// The queue depth from SR_PRESENT_DUMP_QUEUE (latched); 0 = synchronous.
uint32_t capacity();

struct Job {
  std::string path;
  std::vector<uint8_t> bgra;  // top-down BGRA, w * 4 bytes per row
  uint32_t w = 0, h = 0;
  uint64_t nonBlack = 0;      // for the success line (kept identical to the synchronous line)
};

struct Stats {
  uint64_t queued = 0, written = 0, failed = 0;
  uint64_t blockedTimes = 0, blockedUs = 0;  // presenting thread waits on a full queue
  uint64_t highWater = 0;                    // most jobs ever waiting
  uint64_t refused = 0;                      // submits after close() (the exit quiesce): dropped, counted
};

// The writer's two effects, injectable for the unit test (tests/present_dump_writer_test.cpp).
struct Hooks {
  bool (*write)(const Job&) = nullptr;                     // writes the file; true = it exists
  void (*written)(const Job&, bool ok, double ms) = nullptr;  // logs after the write
  void (*census)(const Stats&, const char* why) = nullptr;
};

class Writer {
 public:
  Writer(uint32_t capacity, Hooks hooks);
  ~Writer();  // drains (unbounded) and joins
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  // Blocks while `capacity` jobs are waiting; then queues the job (moved) and returns true. After close() (or when
  // close() wakes it from a full-queue wait) it queues nothing and returns false (Stats::refused).
  bool submit(Job&& job);
  // The exit's quiesce (Astra 10:2x #6): no job is admitted any more, so the drain that follows is final - nothing
  // can arrive after it and be lost at quick_exit. The queued jobs are still written.
  void close();
  // Waits until every queued job is written or timeoutMs passes; returns the jobs still pending.
  uint64_t drain(uint32_t timeoutMs);
  Stats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// The process's writer (created on first use with capacity() and `write`, the caller's file writer; logs through
// app/log; drain registered at atexit). Call only when capacity() > 0.
void submit(Job&& job, bool (*write)(const Job&));
// The exit (app/main.cpp hostFinalDrains, the graceful quit, atexit): true when a writer was created.
bool active();
// Closes the writer's admission (Writer::close; no-op without a writer) - before drainAtExit.
void closeAdmission();
// Waits (bounded) for the queued jobs; true when none is left (or no writer was created). A second call returns the
// first one's outcome.
bool drainAtExit(uint32_t timeoutMs);

}  // namespace sr::gfx::dumpq
