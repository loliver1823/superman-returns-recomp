// Step 4 item 4(c): the present-dump writer queue (see present_dump_writer.hpp). This unit is the queue alone (no
// logging, no file format): the process glue - app/log lines, writeBmp32, the atexit drain - is
// gfx/present_dump_queue.cpp, so tests/present_dump_writer_test.cpp links this file by itself.

#include "present_dump_writer.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace sr::gfx::dumpq {

struct Writer::Impl {
  uint32_t capacity = 1;
  Hooks hooks;
  mutable std::mutex mu;
  std::condition_variable notEmpty, notFull, idle;
  std::deque<Job> q;
  bool writing = false, stop = false, closed = false;
  Stats st;
  std::thread th;

  void run() {
    std::unique_lock<std::mutex> lk(mu);
    for (;;) {
      notEmpty.wait(lk, [&] { return stop || !q.empty(); });
      if (q.empty()) break;  // stop with nothing left
      Job job = std::move(q.front());
      q.pop_front();
      writing = true;
      notFull.notify_all();
      lk.unlock();
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = hooks.write ? hooks.write(job) : false;
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (hooks.written) hooks.written(job, ok, ms);
      lk.lock();
      writing = false;
      if (ok) st.written++;
      else st.failed++;
      // every 120 files: a 360-frame every-present window logs its blocked time 3 times before the run ends (a
      // perf_run title roll ends the process, so the atexit drain's line is not reached; fixab_4c 2026-10-20)
      const bool censusDue = hooks.census && (st.written + st.failed) % kCensusEvery == 0;
      const Stats snap = st;
      if (q.empty()) idle.notify_all();
      if (censusDue) {
        lk.unlock();
        hooks.census(snap, "every 120");
        lk.lock();
      }
    }
    idle.notify_all();
  }
};

Writer::Writer(uint32_t capacity, Hooks hooks) : m_impl(std::make_unique<Impl>()) {
  m_impl->capacity = capacity ? capacity : 1;
  m_impl->hooks = hooks;
  m_impl->th = std::thread([this] { m_impl->run(); });
}

Writer::~Writer() {
  {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->stop = true;
  }
  m_impl->notEmpty.notify_all();
  if (m_impl->th.joinable()) m_impl->th.join();
}

bool Writer::submit(Job&& job) {
  std::unique_lock<std::mutex> lk(m_impl->mu);
  if (!m_impl->closed && m_impl->q.size() >= m_impl->capacity) {
    const auto t0 = std::chrono::steady_clock::now();
    m_impl->notFull.wait(lk, [&] { return m_impl->closed || m_impl->q.size() < m_impl->capacity; });
    m_impl->st.blockedTimes++;
    m_impl->st.blockedUs += uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  if (m_impl->closed) {
    m_impl->st.refused++;
    return false;
  }
  m_impl->q.push_back(std::move(job));
  m_impl->st.queued++;
  if (m_impl->q.size() > m_impl->st.highWater) m_impl->st.highWater = m_impl->q.size();
  lk.unlock();
  m_impl->notEmpty.notify_one();
  return true;
}

void Writer::close() {
  {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->closed = true;
  }
  m_impl->notFull.notify_all();   // a submitter waiting on a full queue wakes and is refused
}

uint64_t Writer::drain(uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lk(m_impl->mu);
  m_impl->idle.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                        [&] { return m_impl->q.empty() && !m_impl->writing; });
  return uint64_t(m_impl->q.size()) + (m_impl->writing ? 1u : 0u);
}

Stats Writer::stats() const {
  std::lock_guard<std::mutex> lk(m_impl->mu);
  return m_impl->st;
}

}  // namespace sr::gfx::dumpq
