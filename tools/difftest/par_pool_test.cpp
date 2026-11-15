// step4/relcrash (2026-10-20): gfx/par.hpp's late-worker race, forced deterministically through the header's
// SR_PAR_TEST_HOOK seam, on the real Pool (the fix) and on LegacyPool below (the pre-fix class verbatim plus the
// same four hook calls: the mutant, which must show the defect).
//
// The interleaving: region A (2 chunks, each 100 ms) runs; a worker that took no chunk of A loads A's final cursor
// and is held right after the load. A ends. Region B (8 chunks) starts; inside run(B), after job_ / chunks_ are
// stored and before the new cursor, the held worker is released and the test waits for its decision.
//   pre-fix: it compares A's cursor with B's chunks_ (8), its CAS on the unchanged A cursor succeeds, and it runs
//            chunk 2 of B's job UNCOUNTED - chunk 2 runs twice and the extra run is still writing after run(B)
//            has returned (the caller could have freed what the job writes);
//   fixed:   the bound comes from the cursor itself (A's count, 2): it takes nothing.
// A run in which no worker could be held is a FAIL (no vacuous pass). Exit 0 = the fix holds AND the mutant shows
// the defect.
//
//   build: cmake --build build --target sr_par_pool_test   (or any C++20 compiler: one TU, the repo root on -I)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

void parTestHook(int point, uint64_t value);
#define SR_PAR_TEST_HOOK(point, value) parTestHook(int(point), uint64_t(value))
#include "gfx/par.hpp"

namespace {
using namespace std::chrono_literals;
namespace P = sr::gfx::par;

// ---- the controller the hook drives ------------------------------------------------------------------------------
struct Ctl {
  std::mutex m;
  std::condition_variable cv;
  std::thread::id mainId;
  std::atomic<bool> armA{false};       // hold the first eligible worker that loads A's final cursor
  std::atomic<bool> expectA{false};    // the next run() is region A (record its generation)
  std::atomic<bool> expectB{false};    // the next run() is region B (release the held worker there)
  uint64_t genA = 0;
  int state = 0;                       // 0 none held, 1 held, 2 released, 3 decided
  int decision = -1;                   // 1 its CAS took a chunk, 0 its CAS failed, 2 it left work() taking nothing
  std::thread::id held;
} g_ctl;
thread_local bool t_ranA = false;      // this thread ran a chunk of A (holding it would stall A)

}  // namespace

void parTestHook(int point, uint64_t v) {
  Ctl& c = g_ctl;
  const auto me = std::this_thread::get_id();
  if (point == P::kHookRunBeforeCursor) {
    if (c.expectA.exchange(false)) {
      std::lock_guard<std::mutex> lk(c.m);
      c.genA = v;
    } else if (c.expectB.exchange(false)) {
      std::unique_lock<std::mutex> lk(c.m);
      if (c.state == 1) {
        c.state = 2;
        c.cv.notify_all();
        c.cv.wait_for(lk, 2s, [&] { return c.state == 3; });
      }
    }
    return;
  }
  if (point == P::kHookAfterLoad) {
    if (me == c.mainId || t_ranA || !c.armA.load()) return;
    uint64_t genA;
    {
      std::lock_guard<std::mutex> lk(c.m);
      genA = c.genA;
    }
    if ((v >> 32) != genA || (v & 0xFFFFu) < 2u) return;  // not A's final cursor (both encodings: next is the low bits)
    if (!c.armA.exchange(false)) return;
    std::unique_lock<std::mutex> lk(c.m);
    c.held = me;
    c.state = 1;
    c.cv.notify_all();
    c.cv.wait(lk, [&] { return c.state >= 2; });
    return;
  }
  if (point == P::kHookAfterCas || point == P::kHookWorkExit) {
    std::lock_guard<std::mutex> lk(c.m);
    if (c.state == 2 && me == c.held) {
      c.decision = point == P::kHookAfterCas ? (v ? 1 : 0) : 2;
      c.state = 3;
      c.cv.notify_all();
    }
  }
}

namespace {
// ---- the mutant: gfx/par.hpp's Pool before the fix (be965e4b..34e8eea8), verbatim but for the hook calls ---------
class LegacyPool {
 public:
  static LegacyPool& get() {
    static LegacyPool p;
    return p;
  }
  unsigned workers() const { return unsigned(threads_.size()); }
  void run(uint32_t chunks, const std::function<void(uint32_t)>& job) {
    std::lock_guard<std::mutex> one(runMu_);
    std::unique_lock<std::mutex> lk(mu_);
    job_ = &job;
    chunks_ = chunks;
    done_ = 0;
    gen_++;
    const uint64_t g = gen_;
    parTestHook(P::kHookRunBeforeCursor, g);
    cursor_.store(g << 32, std::memory_order_release);
    cv_.notify_all();
    lk.unlock();
    work(g);
    lk.lock();
    doneCv_.wait(lk, [&] { return done_ >= chunks_; });
    job_ = nullptr;
  }

 private:
  LegacyPool() {
    unsigned n = std::thread::hardware_concurrency();
    n = n > 1 ? std::min(n - 1, 15u) : 0u;
    for (unsigned i = 0; i < n; i++) threads_.emplace_back([this] { loop(); });
  }
  ~LegacyPool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
      cv_.notify_all();
    }
    for (auto& t : threads_) t.join();
  }
  void loop() {
    uint64_t seen = 0;
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
      cv_.wait(lk, [&] { return stop_ || (gen_ != seen && job_); });
      if (stop_) return;
      seen = gen_;
      lk.unlock();
      work(seen);
      lk.lock();
    }
  }
  void work(uint64_t g) {
    uint32_t mine = 0;
    for (;;) {
      uint64_t cur = cursor_.load(std::memory_order_acquire);
      parTestHook(P::kHookAfterLoad, cur);
      if ((cur >> 32) != g) break;
      const uint32_t c = uint32_t(cur);
      if (c >= chunks_) break;
      const bool took = cursor_.compare_exchange_weak(cur, cur + 1u, std::memory_order_acq_rel);
      parTestHook(P::kHookAfterCas, took ? 1u : 0u);
      if (!took) continue;
      (*job_)(c);
      mine++;
    }
    parTestHook(P::kHookWorkExit, mine);
    if (mine) {
      std::lock_guard<std::mutex> lk(mu_);
      if (gen_ == g) {
        done_ += mine;
        if (done_ >= chunks_) doneCv_.notify_all();
      }
    }
  }
  std::vector<std::thread> threads_;
  std::mutex mu_, runMu_;
  std::condition_variable cv_, doneCv_;
  const std::function<void(uint32_t)>* job_ = nullptr;
  uint32_t chunks_ = 0, done_ = 0;
  std::atomic<uint64_t> cursor_{0};
  uint64_t gen_ = 0;
  bool stop_ = false;
};

struct Outcome {
  bool held = false;          // the scenario was reached: a worker sat on A's final cursor
  int decision = -1;
  bool heldRanB = false;      // the held worker ran a chunk of B (legitimate in the fixed pool once released)
  bool lateWrite = false;     // ... and was still in it after run(B) returned
  unsigned maxRuns = 0;       // the most times any chunk of B ran
};

template <class PoolT>
Outcome scenario(PoolT& pool) {
  Ctl& c = g_ctl;
  {
    std::lock_guard<std::mutex> lk(c.m);
    c.state = 0;
    c.decision = -1;
    c.held = {};
  }
  Outcome o;
  // region A: 2 slow chunks, so idle workers wake while A is live and load its cursor
  c.armA = true;
  c.expectA = true;
  const std::function<void(uint32_t)> jobA = [](uint32_t) {
    t_ranA = true;
    std::this_thread::sleep_for(100ms);
  };
  pool.run(2, jobA);
  {
    std::unique_lock<std::mutex> lk(c.m);
    o.held = c.cv.wait_for(lk, 2s, [&] { return c.state == 1; });
  }
  c.armA = false;
  if (!o.held) return o;
  // region B: 8 chunks; the held worker is released inside run(B), before B's cursor is stored
  std::atomic<unsigned> runs[8] = {};
  std::atomic<bool> runReturned{false}, heldRan{false}, late{false};
  const auto heldId = [&] {
    std::lock_guard<std::mutex> lk(c.m);
    return c.held;
  }();
  const std::function<void(uint32_t)> jobB = [&](uint32_t k) {
    runs[k].fetch_add(1);
    if (std::this_thread::get_id() == heldId) {
      heldRan = true;
      std::this_thread::sleep_for(200ms);  // a slow chunk: the write still to come
      if (runReturned.load()) late = true;
    }
  };
  c.expectB = true;
  pool.run(8, jobB);
  runReturned = true;
  std::this_thread::sleep_for(400ms);  // let a stray chunk finish before reading the counts
  {
    std::lock_guard<std::mutex> lk(c.m);
    o.decision = c.decision;
  }
  o.heldRanB = heldRan.load();
  o.lateWrite = late.load();
  for (auto& r : runs) o.maxRuns = std::max(o.maxRuns, r.load());
  return o;
}

unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[PAR_POOL_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
void show(const char* who, const Outcome& o) {
  std::printf("[PAR_POOL_TEST] %s: held=%d decision=%d heldRanB=%d lateWrite=%d maxRunsPerChunk=%u\n", who,
              int(o.held), o.decision, int(o.heldRanB), int(o.lateWrite), o.maxRuns);
}
}  // namespace

int main() {
  g_ctl.mainId = std::this_thread::get_id();
  P::Pool& fixed = P::Pool::get();
  LegacyPool& legacy = LegacyPool::get();
  std::printf("[PAR_POOL_TEST] workers: fixed %u, legacy %u\n", fixed.workers(), legacy.workers());
  if (fixed.workers() < 2 || legacy.workers() < 2) {
    check(false, "needs >= 2 pool workers (hardware_concurrency >= 3): scenario not reachable here");
    return 1;
  }
  const Outcome f = scenario(fixed);
  show("fixed", f);
  check(f.held, "fixed: a worker was held on region A's final cursor (scenario reached)");
  check(f.held && f.decision == 2, "fixed: released inside run(B), it leaves work() without a CAS");
  // (released, the worker may legitimately join B through the pool's own wake-up: counted, so run(B) waits for it)
  check(f.held && f.maxRuns == 1 && !f.lateWrite, "fixed: every chunk of B ran exactly once, none after run(B) returned");

  const Outcome m = scenario(legacy);
  show("legacy (mutant)", m);
  check(m.held, "mutant: a worker was held on region A's final cursor (scenario reached)");
  check(m.held && m.decision == 1 && m.heldRanB && m.maxRuns == 2,
        "mutant: its CAS on A's cursor takes chunk 2 of B's job, which then runs twice");
  check(m.held && m.lateWrite, "mutant: that uncounted chunk was still running after run(B) returned");

  std::printf("[PAR_POOL_TEST] %u passed, %u failed\n", passed, failed);
  return failed ? 1 : 0;
}
