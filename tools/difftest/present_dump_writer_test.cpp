// Step 4 item 4(c): the present-dump writer queue (gfx/present_dump_writer.cpp) in isolation - no file, no log.
// Checks: every job is written once, in submission order; a slow write never blocks submit while the queue has room;
// a full queue blocks submit (counted) and never drops; drain() waits for the last job; the destructor drains.
#include "gfx/present_dump_writer.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sr::gfx::dumpq;

namespace {
int failures = 0;
void check(bool ok, const char* name) {
  std::printf("[PRESENT_DUMP_WRITER_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}

std::mutex g_mu;
std::vector<std::string> g_order;
std::atomic<int> g_writeMs{0};
std::atomic<int> g_logged{0};

bool slowWrite(const Job& j) {
  std::this_thread::sleep_for(std::chrono::milliseconds(g_writeMs.load()));
  std::lock_guard<std::mutex> lk(g_mu);
  g_order.push_back(j.path);
  return j.path.find("fail") == std::string::npos;
}
void written(const Job&, bool, double) { g_logged++; }

Job job(const std::string& name, size_t bytes = 16) {
  Job j;
  j.path = name;
  j.bgra.assign(bytes, 0x5A);
  j.w = 2;
  j.h = 2;
  return j;
}
double msSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
}  // namespace

int main() {
  // 1. A slow disk does not stall the presenting thread while the queue has room.
  {
    g_order.clear();
    g_writeMs = 200;
    Writer w(8, Hooks{slowWrite, written, nullptr});
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) w.submit(job("a" + std::to_string(i)));
    const double submitMs = msSince(t0);
    check(submitMs < 100.0, "five submits behind a 200 ms write return at once (room in the queue)");
    check(w.drain(5000) == 0, "drain waits for every job");
    const Stats s = w.stats();
    check(s.written == 5 && s.failed == 0 && s.blockedTimes == 0, "five written, none blocked");
    std::lock_guard<std::mutex> lk(g_mu);
    check(g_order == std::vector<std::string>({"a0", "a1", "a2", "a3", "a4"}), "written in submission order");
  }
  // 2. A full queue blocks submit (never drops), and the block is counted.
  {
    g_order.clear();
    g_writeMs = 100;
    Writer w(2, Hooks{slowWrite, written, nullptr});
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 6; ++i) w.submit(job("b" + std::to_string(i)));
    const double submitMs = msSince(t0);
    check(submitMs >= 150.0, "the full queue blocked the submitter (6 jobs, 2 slots, 100 ms writes)");
    w.drain(5000);
    const Stats s = w.stats();
    check(s.written == 6, "all six written: nothing dropped");
    check(s.blockedTimes >= 1 && s.blockedUs >= 50000, "the blocked submits are counted with their time");
    check(s.highWater <= 2, "never more than the capacity waiting");
  }
  // 3. A failed write is counted, the queue goes on.
  {
    g_writeMs = 0;
    Writer w(4, Hooks{slowWrite, written, nullptr});
    w.submit(job("c0"));
    w.submit(job("c1_fail"));
    w.submit(job("c2"));
    w.drain(5000);
    const Stats s = w.stats();
    check(s.written == 2 && s.failed == 1, "a failed write counts as failed, the next one still runs");
  }
  // 4. The buffer is moved, not copied: the job's bytes arrive intact.
  {
    static std::atomic<size_t> seen{0};
    Writer w(4, Hooks{[](const Job& j) {
                        seen = j.bgra.size() == (size_t(1) << 20) && j.bgra[12345] == 0x5A ? 1 : 2;
                        return true;
                      },
                      nullptr, nullptr});
    Job j = job("d0", size_t(1) << 20);
    const uint8_t* before = j.bgra.data();
    w.submit(std::move(j));
    w.drain(5000);
    check(seen == 1, "the writer saw the 1 MiB buffer intact");
    check(j.bgra.empty() && before != nullptr, "the submitter's buffer was moved out");
  }
  // 6. The census hook runs every kCensusEvery files, with the counts so far.
  {
    static std::vector<uint64_t> seen;
    seen.clear();
    g_writeMs = 0;
    {
      Writer w(16, Hooks{slowWrite, written, [](const Stats& s, const char*) { seen.push_back(s.written); }});
      for (uint64_t i = 0; i < 2 * kCensusEvery + 5; ++i) w.submit(job("c" + std::to_string(i)));
      w.drain(10000);
    }
    check(seen.size() == 2 && seen[0] == kCensusEvery && seen[1] == 2 * kCensusEvery,
          "the census runs every kCensusEvery files");
  }
  // 5. The destructor drains what is queued before it joins.
  {
    g_order.clear();
    g_writeMs = 50;
    {
      Writer w(8, Hooks{slowWrite, written, nullptr});
      for (int i = 0; i < 4; ++i) w.submit(job("e" + std::to_string(i)));
    }
    std::lock_guard<std::mutex> lk(g_mu);
    check(g_order.size() == 4, "the destructor wrote every queued job");
  }
  // 7. close() (the exit's quiesce, Astra 10:2x #6): a submitter blocked on the full queue wakes and is refused,
  //    every later submit is refused (counted), and the jobs already queued are still written by the drain.
  {
    g_order.clear();
    g_writeMs = 150;
    Writer w(1, Hooks{slowWrite, written, nullptr});
    w.submit(job("f0"));   // being written (150 ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    w.submit(job("f1"));   // queued: the queue is full
    std::atomic<int> blockedResult{-1};
    std::thread blocked([&] { blockedResult = w.submit(job("f2")) ? 1 : 0; });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    w.close();
    blocked.join();
    const bool late = w.submit(job("f3"));
    const uint64_t left = w.drain(5000);
    const Stats s = w.stats();
    check(blockedResult == 0, "close wakes the submitter blocked on a full queue and refuses its job");
    check(!late && s.refused == 2, "after close every submit is refused and counted");
    std::lock_guard<std::mutex> lk(g_mu);
    check(left == 0 && s.written == 2 && g_order == std::vector<std::string>({"f0", "f1"}),
          "the jobs queued before close are still written");
  }
  std::printf("[PRESENT_DUMP_WRITER_TEST] %s (%d failure(s))\n", failures ? "FAIL" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
