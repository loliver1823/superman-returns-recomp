#pragma once

// A small persistent worker pool for data-parallel CPU loops in the renderer
// (EDRAM mirror store/load, resolves). parallelFor(begin, end, fn) runs
// fn(i) for every i in [begin, end), split into contiguous chunks; the caller
// thread takes a chunk too and returns when all are done. fn must only write
// memory no other index writes (the EDRAM callers: every (x, y) maps to its
// own tiled / sample offset).

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Test seam (tools/difftest/par_pool_test.cpp defines it before including this header to force interleavings):
// an empty statement everywhere else.
#ifndef SR_PAR_TEST_HOOK
#define SR_PAR_TEST_HOOK(point, value) ((void)0)
#endif

namespace sr::gfx::par {

enum HookPoint : int { kHookAfterLoad = 1, kHookAfterCas = 2, kHookWorkExit = 3, kHookRunBeforeCursor = 4 };

class Pool {
 public:
  static Pool& get() {
    static Pool p;
    return p;
  }
  unsigned workers() const { return unsigned(threads_.size()); }

  // Runs job(chunk) for chunk in [0, chunks); the caller runs chunks too.
  //
  // iter-461: the chunk cursor carries the GENERATION. A worker that woke late
  // for region G (all of G's chunks already taken) used to fetch_add the bare
  // counter AFTER region G+1 had reset it: it took one of G+1's chunk indices,
  // ran it (G+1 also handed that index out), and its `done_ += mine` pushed
  // done_ past chunks_, so run() waited forever - the CP thread hung in
  // pm4ShmRequest's parallel hashing (archive/gpu_cp_stall.dmp, [GPU_CP_STALL]
  // phase=swap). A worker now takes a chunk only under its own generation.
  //
  // step4/relcrash (2026-10-20): the chunk COUNT is in the cursor too: (generation << 32) | (chunks << 16) | next.
  // A worker that loaded generation G's final cursor (G, N_G) used to compare it with the plain chunks_ - already the
  // NEXT region's larger count if run() had stored job_ / chunks_ but not yet the new cursor - and its CAS on the
  // still-unchanged (G, N_G) then succeeded: it ran chunk N_G of the NEW job, uncounted (gen_ != G), so run() could
  // return - and the caller free the buffer the job writes - while that worker still wrote it. Now the bound comes
  // from the same atomic value the CAS checks (tools/difftest/par_pool_test.cpp forces that interleaving).
  static constexpr uint32_t kMaxChunks = 0xFFFFu;
  void run(uint32_t chunks, const std::function<void(uint32_t)>& job) {
    if (chunks > kMaxChunks) {  // parallelFor never asks for more than lanes * 4 (<= 64); keep any caller correct
      for (uint32_t c = 0; c < chunks; c++) job(c);
      return;
    }
    std::lock_guard<std::mutex> one(runMu_);  // one parallel region at a time
    std::unique_lock<std::mutex> lk(mu_);
    job_ = &job;
    chunks_ = chunks;
    done_ = 0;
    gen_++;
    const uint64_t g = gen_;
    SR_PAR_TEST_HOOK(kHookRunBeforeCursor, g);
    cursor_.store((g << 32) | (uint64_t(chunks) << 16), std::memory_order_release);  // (generation, count, next)
    cv_.notify_all();
    lk.unlock();
    work(g);
    lk.lock();
    doneCv_.wait(lk, [&] { return done_ >= chunks_; });
    job_ = nullptr;
  }

 private:
  Pool() {
    unsigned n = std::thread::hardware_concurrency();
    n = n > 1 ? std::min(n - 1, 15u) : 0u;
    for (unsigned i = 0; i < n; i++) threads_.emplace_back([this] { loop(); });
  }
  ~Pool() {
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
  // Takes chunks of generation g only: the cursor's high half is the
  // generation, so a late worker sees a newer generation and takes nothing;
  // the count beside it is that generation's own (never the next run's).
  void work(uint64_t g) {
    uint32_t mine = 0;
    for (;;) {
      uint64_t cur = cursor_.load(std::memory_order_acquire);
      SR_PAR_TEST_HOOK(kHookAfterLoad, cur);
      if ((cur >> 32) != g) break;
      const uint32_t n = uint32_t(cur >> 16) & kMaxChunks;
      const uint32_t c = uint32_t(cur) & kMaxChunks;
      if (c >= n) break;
      const bool took = cursor_.compare_exchange_weak(cur, cur + 1u, std::memory_order_acq_rel);
      SR_PAR_TEST_HOOK(kHookAfterCas, took ? 1u : 0u);
      if (!took) continue;
      (*job_)(c);
      mine++;
    }
    SR_PAR_TEST_HOOK(kHookWorkExit, mine);
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
  std::atomic<uint64_t> cursor_{0};  // (generation << 32) | (chunk count << 16) | next chunk
  uint64_t gen_ = 0;
  bool stop_ = false;
};

template <class F>
void parallelFor(uint32_t begin, uint32_t end, F&& fn, uint32_t minPerChunk = 8) {
  if (end <= begin) return;
  const uint32_t n = end - begin;
  Pool& pool = Pool::get();
  const uint32_t lanes = pool.workers() + 1u;
  uint32_t chunks = std::min(n / std::max(minPerChunk, 1u), lanes * 4u);
  if (chunks <= 1u || lanes <= 1u) {
    for (uint32_t i = begin; i < end; i++) fn(i);
    return;
  }
  const uint32_t per = (n + chunks - 1u) / chunks;
  const std::function<void(uint32_t)> job = [&](uint32_t c) {
    const uint32_t a = begin + c * per, b = std::min(end, a + per);
    for (uint32_t i = a; i < b; i++) fn(i);
  };
  pool.run(chunks, job);
}

}  // namespace sr::gfx::par
