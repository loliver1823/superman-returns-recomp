#include <cstdlib>
#include "kernel/sr_oracle.hpp"  // C1: the legacy VdSwap window scan and the SR_NATIVE_EXEC executor are dev-only (SR_ORACLE)
#include "kernel/phys_alloc.hpp"
#include "kernel/phys_window.hpp"
#include "null_renderer.hpp"
#include "d3d12_backend.hpp"
#include "pso_warm.hpp"   // step 4 2k: psoAnyOn (cpEarlyInit)
#if SR_ORACLE  // C1 stage 1: the development live engine is the PM4 walker (its IB entry below is the oracle half)
#include "pm4_engine.hpp"
#else          // the release configuration: the record engine, the opaque record dispatch installed here
#include "record_engine.hpp"
#include "kernel/native_flush.hpp"
#endif
#include "pm4_live_trace.hpp"
#include "shader_cache.hpp"  // render/boot-attach: msSinceProcessStart (the attach line)
#include "xenos_state.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/gpu_hang_wd.hpp"
#include "kernel/native_exec.hpp"   // iter-467 native A: the executor
#include "kernel/native_drain_census.hpp"  // iter-467 native A: tile census (observe-only)
#include "native_zpd.hpp"                  // p2.zpd: the covering-fence check (SR_NATIVE_ZPD)
#include <intrin.h>
#include "../app/log.hpp"
#include <cinttypes>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <windows.h>

namespace sr {
namespace gfx {

// getXenosRegisters() (the device-state shadow) is defined by gfx/record_engine.cpp (C1 stage 2a).
DrawCommandQueue& getDrawCommandQueue() {
  static DrawCommandQueue s_queue;
  return s_queue;
}

namespace {
#if SR_ORACLE  // the legacy VdSwap window scan's state (onVdSwapFrame; dev-only, docs/RELEASE_COMPOSITION.md 2.2)
  constexpr int kFbWidth  = 1280;
  constexpr int kFbHeight = 720;
  constexpr int kFbBytesPerPixel = 4;
  constexpr size_t kFbSize = static_cast<size_t>(kFbWidth) * kFbHeight * kFbBytesPerPixel;

  static uint32_t s_resolveAddr = 0;    // Last resolved framebuffer address
  static uint32_t s_prevWritePtr = 0;   // Previous VdSwap r3 (= scan anchor)
#endif

  // The live instance of the shared PM4 engine (gfx/pm4_engine.hpp) — the same
  // golden-trace-proven parser the replay uses, fed from the game's real
  // command stream. Physical P is visible at guest VA 0x80000000 + P.
  // C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): in the release configuration (SR_ORACLE=0) the live instance is the
  // record engine (gfx/record_engine.hpp) - the walker is not linked; the CP applies items (records) only.
#if SR_ORACLE
  using LiveEngine = Pm4Engine;
#else
  using LiveEngine = RecordEngine;
#endif
  LiveEngine& livePm4() {
    static LiveEngine s_eng = [] {
      LiveEngine e;
#if !SR_ORACLE
      // The opaque record kinds' dispatch (record_engine.hpp opaqueKnown / opaqueApply): Pm4Engine's constructor
      // installs it in the development configuration; here the registry's handlers take the record engine itself
      // (kernel/native_flush.hpp CpEngine). Process-wide, like the registry.
      RecordEngine::opaqueKnown = [](uint32_t kind) { return sr::native::flush::opaqueHandler(kind) != nullptr; };
      RecordEngine::opaqueApply = [](RecordEngine& eng, uint32_t kind, const uint32_t* w, uint32_t n) {
        const sr::native::flush::OpaqueHandler h = sr::native::flush::opaqueHandler(kind);
        return h != nullptr && h(kind, w, n, eng);
      };
#endif
      // iter-404g PLATFORM FIX (SR_PM4_PHYS_A): resolve GPU physical addresses
      // through a PHYSICAL window, not 0x80000000.
      //
      // ENGINE_MAP 6e (cross-checked vs Xenia memory.h): the physical heaps are
      // 0xA0000000 / 0xC0000000 / 0xE0000000, all aliasing the same RAM;
      // 0x80000000 is a VIRTUAL heap. So this translation was not picking the
      // wrong alias - it was using an address that is not a physical window at
      // all, and in our harness (where the windows are distinct host regions,
      // not folded) it lands on memory the guest never wrote.
      //
      // MEASURED CONSEQUENCE (iter-404): the movie's shaders are submitted by
      // ADDRESS (IM_LOAD, not IM_LOAD_IMMEDIATE) at physical 0x0EC07130 - the
      // guest wrote them via 0xAEC07130, which is exactly the pointer at obj+32
      // of the bound shader object. We read the empty 0x8 alias, saw zeros, and
      // logged [PM4_REJECT] "payload is not Xenos ucode", so the GPU kept
      // executing the stale resident passthrough. Every layer above worked.
      //
      // SCOPE, deliberately narrow: iter-373 identified the SAME defect from the
      // fence side and fixed it inside aliasGuestVA - i.e. on every PPC
      // load/store - which regressed the trunk (feTick 2870 -> 9, 0/4 deep) and
      // is why SR_PHYS_ALIAS_A is default-OFF. This touches ONLY the PM4
      // engine's own phys->host resolution and leaves guest load/store
      // semantics alone. Gated so the A/B decides the default rather than an
      // assumption that "console-correct" implies "no regression" - iter-373 is
      // the standing counter-example.
      e.physToHost = [](void*, uint32_t phys) -> uint8_t* {
#if SR_ORACLE
        static const bool s_physA = [] {
          const char* v = std::getenv("SR_PM4_PHYS_A");
          return !v || v[0] != '0';
        }();
        const uint32_t off = phys & 0x1FFFFFFFu;
        // iter-460 (SR_PM4_ENGINE_OWNER, default ON): the allocators' records
        // first (kernel/phys_window.hpp gpuWindow), as every other GPU read now
        // does. Constant blocks (LOAD_ALU_CONSTANT), fences and polled words the
        // game keeps in its 0x8 heap were read and written through the 0xA alias.
        static const bool s_owner = [] {
          const char* v = std::getenv("SR_PM4_ENGINE_OWNER");
          return !v || v[0] != '0';
        }();
        // iter-460 (SR_CB_WINDOW): bytes inside a command buffer the CP just
        // parsed (resolve rectangles are inline in it) come from the window the
        // buffer itself was read from.
        if (sr::kernel::physwin::commandBufferWindowEnabled()) {
          const int cw = sr::kernel::physwin::commandBufferWindow(off, 1u);
          if (cw == 8) return sr::kernel::guestVAToHost(0x80000000u + off);
          if (cw == 0xA) return sr::kernel::guestVAToHost(0xA0000000u + off);
        }
        if (s_owner && sr::kernel::physwin::gpuOwnerWindowEnabled()) {
          const int w = sr::kernel::physwin::gpuWindow(off, 1u);
          if (w == 8) return sr::kernel::guestVAToHost(0x80000000u + off);
          if (w == 0xA) return sr::kernel::guestVAToHost(0xA0000000u + off);
        }
        return sr::kernel::guestVAToHost((s_physA ? 0xA0000000u : 0x80000000u) + off);
#else
        // Native fence/constant addresses follow allocator ownership. There is
        // no parsed command-buffer history or legacy window selector.
        const uint32_t off = phys & 0x1FFFFFFFu;
        const int owner = sr::kernel::physwin::gpuWindow(off, 1u);
        return sr::kernel::guestVAToHost((owner == 8 ? 0x80000000u : 0xA0000000u) + off);
#endif
      };
      // iter-441 MIGRATION STAGE-A: the live engine is command-processor
      // SERVICE ONLY (fence writeback, interrupts, health counters). Draws are
      // parsed and counted but never submitted — live rendering belongs to the
      // API-boundary HLE path (gfx/hle_video). The replay fixture's own
      // instance keeps submitDraws=true.
      e.submitDraws = pm4LiveEnabled();  // P2: the live game draws through PM4
      return e;
    }();
    return s_eng;
  }
}

// P2 (SR_PM4_LIVE): the live backend and the lock that serialises its use by
// the guest threads that submit (doorbell) and swap (VdSwap).
namespace {
D3D12Backend* s_liveBe = nullptr;
// Detached workers can keep taking this lock after backend retirement.
auto& s_liveMu = *new std::recursive_mutex();
enum DrainSite { kDsFence, kDsSwap, kDsIbEnd, kDsVdSwap, kDsAsync, kDsExec, kDsN };
void drainCounted(int site);

#if SR_ORACLE  // C1 stage 1: the IB-end async drain (SR_CP_DRAIN_ASYNC) serves only executeGuestIndirectBuffer (dev-only)
struct DrainWorker {
  std::mutex mu;
  std::condition_variable cv;
  uint64_t req = 0, done = 0;
  std::atomic<uint64_t> drains{0}, coalesced{0};
};
DrainWorker& drainWorker() {
  static DrainWorker* w = [] {
    auto* nw = new DrainWorker();
    // The guest-memory mapping is per thread: hand the CP thread's to the worker.
    uint8_t* base = sr::kernel::getGuestBase();
    const size_t size = sr::kernel::getGuestSize();
    std::thread([nw, base, size] {
      SetThreadDescription(GetCurrentThread(), L"SR CP drain");
      if (base) sr::kernel::setGuestMemory(base, size);
      for (;;) {
        uint64_t target;
        {
          std::unique_lock<std::mutex> lk(nw->mu);
          nw->cv.wait(lk, [&] { return nw->req != nw->done; });
          target = nw->req;
        }
        {
          std::lock_guard<std::recursive_mutex> lk(s_liveMu);
          if (s_liveBe) drainCounted(kDsAsync);
        }
        const uint64_t n = nw->drains.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0 && n >= 1024)
          SR_LOG_BOOT(sr::LogSeverity::Info, "[CP_DRAIN] async drains=%llu coalesced kicks=%llu",
                      (unsigned long long)n, (unsigned long long)nw->coalesced.load());
        std::lock_guard<std::mutex> lk(nw->mu);
        nw->done = target;
      }
    }).detach();
    return nw;
  }();
  return *w;
}
#endif  // SR_ORACLE (the IB-end async drain worker)
}  // namespace
namespace {
std::atomic<bool> s_liveAttached{false};
// render/boot-attach: main() reports a backend that will never attach (its init failed), so the CP's
// wait ends at once and loudly instead of after its limit (kernel/gpu_command_processor.cpp).
std::atomic<bool> s_liveFailed{false};
char s_liveFailReason[256] = {};
}  // namespace
bool pm4LiveBackendAttached() { return s_liveAttached.load(std::memory_order_acquire); }
void failRenderBackend(const char* reason) {
  std::snprintf(s_liveFailReason, sizeof(s_liveFailReason), "%s", reason ? reason : "unknown");
  s_liveFailed.store(true, std::memory_order_release);
  SR_LOG_BOOT(sr::LogSeverity::Error, "[DISPLAY] the PM4 live backend will not attach: %s", s_liveFailReason);
}
bool pm4LiveBackendFailed() { return s_liveFailed.load(std::memory_order_acquire); }

namespace {
std::atomic<const char*> s_initStep{nullptr};
std::atomic<uint32_t> s_initTid{0};
std::atomic<double> s_initStartMs{-1.0};
std::atomic<bool> s_initReturned{false};
}  // namespace
void noteBackendInitStep(const char* step) {
  if (!step) {
    s_initReturned.store(true, std::memory_order_release);
    return;
  }
  if (!s_initTid.load(std::memory_order_relaxed)) {
    s_initStartMs.store(msSinceProcessStart(), std::memory_order_relaxed);
    s_initTid.store(uint32_t(GetCurrentThreadId()), std::memory_order_relaxed);
  }
  s_initStep.store(step, std::memory_order_release);
}
BackendInitState backendInitState() {
  BackendInitState b;
  b.step = s_initStep.load(std::memory_order_acquire);
  b.tid = s_initTid.load(std::memory_order_relaxed);
  b.startMs = s_initStartMs.load(std::memory_order_relaxed);
  b.returned = s_initReturned.load(std::memory_order_acquire);
  return b;
}
const char* pm4LiveBackendFailReason() {
  return s_liveFailed.load(std::memory_order_acquire) ? s_liveFailReason : "";
}

void setRenderBackend(D3D12Backend* be) {
  {
    std::lock_guard<std::recursive_mutex> lk(s_liveMu);
    s_liveBe = be;
    s_liveAttached.store(be != nullptr, std::memory_order_release);
  }
  // iter-467 native-b: the race the black world rode on (job64: b64_2's first 16 traced swaps had no backend state).
  // render/boot-attach: the attach's own process-relative time (the bound the boot-attach evaluator reads) and
  // the installed shader cache's readiness sampled just after the attach (Astra round 2: the evaluator's "still
  // loading at the attach" witness reads this, not a timestamp comparison). Stages only advance, so "still loading"
  // here was true at the attach; "loaded" may have become true just after it.
  const bool cacheReady = getShaderCache().installedLoaded();
  const double attachAt = msSinceProcessStart();
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[DISPLAY] PM4 live backend attached: guest VdSwaps before it %llu, draw commands queued %u; "
              "process +%.1f ms; installed shader cache just after the attach: %s",
              (unsigned long long)sr::kernel::getVdSwapCount(), unsigned(getDrawCommandQueue().size()), attachAt,
              cacheReady ? "loaded" : "still loading");
  // iter-460 live trace: the trace's initial GPU state comes from this backend.
  livetrace::setInitProvider([](livetrace::InitState& out) {
    std::lock_guard<std::recursive_mutex> lk(s_liveMu);
    if (!s_liveBe) return;
    s_liveBe->pm4TraceInit(out);
  });
}

bool pm4RenderThreadIdleForExit(uint32_t ms) {
  D3D12Backend* be;
  {
    std::lock_guard<std::recursive_mutex> lk(s_liveMu);
    be = s_liveBe;
  }
  // the backend outlives this wait: the exit ends the process (quick_exit) or tears it down only after the drains
  return !be || be->pm4RenderThreadIdle(ms);
}

void retirePm4LiveBackend(D3D12Backend* expected) {
  std::lock_guard<std::recursive_mutex> lk(s_liveMu);
  if (s_liveBe != expected) return;
  s_liveBe = nullptr;
  s_liveAttached.store(false, std::memory_order_release);
}

// NATIVE strand A (SR_CP_DRAIN_ASYNC, default OFF): the drain at the end of an indirect buffer runs on
// its own host thread ("SR CP drain") while the CP parses the next one. Every drain the guest can
// observe stays synchronous: cpFlushDraws (before any fence / interrupt / memory write / swap the CP
// executes) and the swap take s_liveMu, wait for the worker's drain in flight, then drain the rest
// themselves - so no guest-visible fence is written before every draw ahead of it has been drained,
// exactly as before. (Segment memory is reused by the XDK only behind those fences.) A live trace
// recording keeps the synchronous drain (its draw records are ordered on the CP thread).
// C1 stage 1: an indirect buffer is the development PM4 walker's input only, so this is dev-only (SR_ORACLE).
#if SR_ORACLE
namespace {
bool drainAsyncOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CP_DRAIN_ASYNC");
    const bool on = e && e[0] == '1';
    if (on) SR_LOG_BOOT(sr::LogSeverity::Info, "[CP_DRAIN] SR_CP_DRAIN_ASYNC=1: indirect-buffer drains on their own thread");
    return on;
  }();
  return v;
}
struct DrainWorker;
DrainWorker& drainWorker();
}  // namespace
#endif  // SR_ORACLE

// NATIVE strand A (SR_CP_DRAIN_STATS=1, diagnostic): drains by site - calls, empty calls, commands
// drained, wall time - logged every 4096 drains as [CP_DRAIN] stats.
namespace {
void drainCounted(int site) {
  if (!s_liveBe) return;  // s_liveMu held; retirement may follow an earlier admission check
#if SR_ORACLE
  static const bool s_on = [] {
    const char* e = std::getenv("SR_CP_DRAIN_STATS");
    return e && e[0] == '1';
  }();
  if (!s_on) {
    s_liveBe->pm4DrainNow();
    return;
  }
  static uint64_t s_calls[kDsN], s_empty[kDsN], s_cmds[kDsN], s_total = 0;
  static double s_us[kDsN];
  const uint32_t pending = getDrawCommandQueue().size();
  const auto t0 = std::chrono::steady_clock::now();
  s_liveBe->pm4DrainNow();
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  s_calls[site]++;
  if (!pending) s_empty[site]++;
  s_cmds[site] += pending;
  s_us[site] += us;
  if ((++s_total & 4095u) == 0) {
    static const char* const kName[kDsN] = {"fence", "swap", "ibEnd", "vdSwap", "async", "exec"};
    char line[600];
    int o = 0;
    for (int i = 0; i < kDsN; i++)
      if (s_calls[i])
        o += std::snprintf(line + o, sizeof(line) - size_t(o), " %s calls=%llu empty=%llu cmds=%llu us=%.0f",
                           kName[i], (unsigned long long)s_calls[i], (unsigned long long)s_empty[i],
                           (unsigned long long)s_cmds[i], s_us[i]);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[CP_DRAIN] stats after %llu drains:%s", (unsigned long long)s_total, line);
  }
#else
  (void)site;
  s_liveBe->pm4DrainNow();
#endif
}
}  // namespace

bool pm4CpAsyncEnabled() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_CP");
    if (e && e[0] == '0') return false;
    if (e && e[0] == '1') return true;
    return pm4LiveEnabled();
  }();
  return v;
#else
  // C1 (docs/RELEASE_COMPOSITION.md 3.1 rule 2): SR_PM4_CP=0 is the synchronous legacy path (the VdSwap window scan),
  // a fallback component. The release configuration has no selector: the asynchronous CP is the one consumer
  // (pm4LiveEnabled() is constant true there as well, gfx/d3d12_pm4_edram.cpp).
  return true;
#endif
}

RecordEngine& getLiveRecordEngine() { return livePm4(); }
#if SR_ORACLE
Pm4Engine& getLivePm4() { return livePm4(); }
#endif

// ---- NATIVE strand A executor (kernel/native_exec.hpp, SR_NATIVE_EXEC) ----
// One thread ("SR native exec") drains the queue (drainDrawQueue stops at an effect item) and performs
// the effect items in stream order. The CP thread only pushes; cp.flush (WAIT_REG_MEM, INTERRUPT,
// COND_WRITE, any synchronous caller) waits for the executor to finish everything pushed so far.
// C1 (docs/RELEASE_COMPOSITION.md 2.2): a default-OFF experiment on the emulated CP's drain; the release
// configuration (SR_ORACLE=0) compiles it out, with kernel/native_exec.cpp (CMakeLists.txt EMULATION_SOURCES).
// The nativeExec* entries are then null_renderer.hpp's inline no-ops and every exec:: gate reads false.
#if SR_ORACLE
namespace {
struct Exec {
  std::once_flag once;
  std::atomic<bool> running{false}, sleeping{false}, syncReq{false};
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<uint64_t> effectsPushed{0}, effectsDone{0}, swaps{0};
  std::atomic<uint64_t> curSeq{0};
  std::atomic<uint32_t> curKind{0};
  bool capSet = false;  // CP thread only
  // The host word each queued store will write (CP thread only): a memory poll waits for the
  // executor only when a store still queued targets its word - the synchronous model's answer.
  struct Pending {
    const uint8_t* p;
    uint64_t effect;  // effectsPushed when the store's item was pushed
  };
  static constexpr uint32_t kPend = 1024;
  Pending pend[kPend] = {};
  uint64_t pendN = 0;
};
Exec& ex() {
  static Exec* e = new Exec();
  return *e;
}
void presentSwapLocked(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t d0, uint32_t d2,
                       const sr::gfx::zpd::FrameStamp& zst = {});
// The deferred swap item's frame stamp (Astra round 2 P2-1, gfx/native_zpd FrameStamp): drawParams[5..8], written by
// cpPresentSwap (zero unless the freeze dump runs), read back where the executor presents the frame.
constexpr uint32_t kSwapStampAt = 5;
sr::gfx::zpd::FrameStamp swapItemStamp(const GuestDrawCommand& f) {
  sr::gfx::zpd::FrameStamp s;
  s.begins = f.drawParams[kSwapStampAt + 0];
  s.rec = f.drawParams[kSwapStampAt + 1];
  s.pm4 = f.drawParams[kSwapStampAt + 2];
  s.frame = f.drawParams[kSwapStampAt + 3];
  return s;
}
void execWake() {
  Exec& e = ex();
  if (e.sleeping.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lk(e.mu);
    e.cv.notify_one();
  }
}
bool execWant() { return sr::native::exec::deferring() || ex().syncReq.load(std::memory_order_acquire); }
// s_liveMu held: perform the effect items at the front of the queue, in order.
void execPerformFrontEffects() {
  Exec& e = ex();
  auto& q = getDrawCommandQueue();
  const GuestDrawCommand* f;
  while ((f = q.front()) != nullptr && f->effectKind) {
    e.curSeq.store(f->effectSeq, std::memory_order_relaxed);
    e.curKind.store(f->effectKind, std::memory_order_relaxed);
    sr::native::exec::chkEffect(f->effectSeq, q.consumed(), f->effectKind);
    if (f->effectKind == sr::native::exec::kStores) {
      livePm4().execPerformStores(*f);
    } else if (f->effectKind == sr::native::exec::kSwap) {
      presentSwapLocked(f->drawParams[0], f->drawParams[1], f->drawParams[2], f->drawParams[3], f->drawParams[4],
                        swapItemStamp(*f));
      if ((e.swaps.fetch_add(1, std::memory_order_relaxed) + 1) % 1024u == 0) sr::native::exec::report("periodic");
    }
    q.popFront();
    e.effectsDone.fetch_add(1, std::memory_order_release);
  }
  e.curKind.store(0, std::memory_order_relaxed);
}
// s_liveMu held, CP thread, an A/B OFF window: everything queued (an ON window's leftovers included),
// draws and effects in order, on this thread - the synchronous path's behaviour.
void execDrainAllLocked(int site) {
  auto& q = getDrawCommandQueue();
  for (const GuestDrawCommand* f = q.front(); f != nullptr; f = q.front()) {
    if (!f->effectKind) {
      const uint64_t c0 = q.consumed();
      drainCounted(site);
      if (q.consumed() == c0 && q.front() == f) break;  // the drain is not ready (boot)
    }
    execPerformFrontEffects();
  }
}
void execLoop(uint8_t* base, size_t size) {
  // SR_NATIVE_EXEC_SPIN: pause iterations before the executor sleeps on an empty queue (default 4000).
  static const int s_spin = [] {
    const char* v = std::getenv("SR_NATIVE_EXEC_SPIN");
    return v ? std::max(0, std::atoi(v)) : 4000;
  }();
  SetThreadDescription(GetCurrentThread(), L"SR native exec");
  if (base) sr::kernel::setGuestMemory(base, size);
  sr::native::exec::markExecutorThread();
  Exec& e = ex();
  auto& q = getDrawCommandQueue();
  for (;;) {
    const GuestDrawCommand* f = q.front();
    if (!f || !execWant()) {
      // Deferring with an empty queue: spin briefly (the CP pushes again within microseconds), then
      // sleep until kicked. Not deferring (gate / A/B OFF window): sleep without spinning - an idle
      // executor must not take CPU from the synchronous CP it is being compared with.
      const bool want = execWant();
      const bool prof = want && sr::native::exec::profOn();
      const uint64_t s0 = prof ? sr::native::exec::profNow() : 0;
      bool got = false;
      for (int i = 0; want && i < s_spin && !got; i++) {
        _mm_pause();
        got = q.front() != nullptr;
      }
      const uint64_t s1 = prof ? sr::native::exec::profNow() : 0;
      if (!got) {
        std::unique_lock<std::mutex> lk(e.mu);
        e.sleeping.store(true, std::memory_order_release);
        e.cv.wait_for(lk, std::chrono::milliseconds(want ? 1 : 20),
                      [&] { return q.front() != nullptr && execWant(); });
        e.sleeping.store(false, std::memory_order_relaxed);
      }
      if (prof) sr::native::exec::profExecIdle(s1 - s0, got ? 0 : sr::native::exec::profNow() - s1, got);
      continue;
    }
    uint64_t moved = 0;
    {
      const bool prof = sr::native::exec::profOn();
      const uint64_t l0 = prof ? sr::native::exec::profNow() : 0;
      std::lock_guard<std::recursive_mutex> lk(s_liveMu);
      if (!s_liveBe) continue;
      const uint64_t c0 = q.consumed();
      const uint64_t l1 = prof ? sr::native::exec::profNow() : 0;
      if (prof) sr::native::exec::profPlace(sr::native::exec::kPlExec);
      struct ProfIter {
        bool on;
        uint64_t l0, l1, depth;
        ~ProfIter() {
          if (on) sr::native::exec::profExecIter(l1 - l0, sr::native::exec::profNow() - l1, uint32_t(depth));
        }
      } profIter{prof, l0, l1, q.committed() - c0};
      if (!f->effectKind) {
        drainCounted(kDsExec);  // up to the next effect item, or the end of the queue
        sr::native::exec::noteDrainCall(uint32_t(q.consumed() - c0));
      }
      execPerformFrontEffects();  // the effect items at the front, in stream order
      moved = q.consumed() - c0;
    }
    if (!moved) std::this_thread::sleep_for(std::chrono::microseconds(50));  // drain not ready yet (boot)
  }
}
void execEnsureStarted() {
  Exec& e = ex();
  if (e.running.load(std::memory_order_acquire)) return;
  std::call_once(e.once, [&] {
    // The guest-memory mapping is per thread: hand the CP thread's to the executor.
    uint8_t* base = sr::kernel::getGuestBase();
    const size_t size = sr::kernel::getGuestSize();
    std::thread([base, size] { execLoop(base, size); }).detach();
    e.running.store(true, std::memory_order_release);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_EXEC] executor started (SR_NATIVE_EXEC=1%s)",
                sr::native::exec::checkOn() ? ", CHECK" : "");
  });
}
// CP thread: wait until the executor has consumed every item committed so far.
void execSync() {
  Exec& e = ex();
  if (!e.running.load(std::memory_order_acquire)) return;
  auto& q = getDrawCommandQueue();
  const uint64_t target = q.committed();
  if (q.consumed() < target) {
    const auto t0 = std::chrono::steady_clock::now();
    auto nextLog = t0 + std::chrono::seconds(2);
    e.syncReq.store(true, std::memory_order_release);
    execWake();
    bool timedOut = false;
    for (uint32_t i = 0; q.consumed() < target; i++) {
      if (i < 2000) {
        _mm_pause();
        continue;
      }
      std::this_thread::yield();
      const auto now = std::chrono::steady_clock::now();
      if (now >= nextLog) {  // the deadlock watchdog: what the executor is on
        timedOut = true;
        nextLog = now + std::chrono::seconds(10);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[NATIVE_EXEC] WATCHDOG CP waited %.1fs: target=%llu consumed=%llu queue=%u executor item seq=%llu "
                    "kind=%u effects pushed=%llu done=%llu",
                    std::chrono::duration<double>(now - t0).count(), (unsigned long long)target,
                    (unsigned long long)q.consumed(), q.size(), (unsigned long long)e.curSeq.load(),
                    e.curKind.load(), (unsigned long long)e.effectsPushed.load(),
                    (unsigned long long)e.effectsDone.load());
      }
    }
    e.syncReq.store(false, std::memory_order_release);
    sr::native::exec::noteSync(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - t0).count()),
                               timedOut);
  }
  // A 1K ring for a concurrent producer and consumer (set once, here: the producer, on an empty queue).
  if (!e.capSet && q.consumed() == q.committed()) {
    static const uint32_t s_cap = [] {
      const char* v = std::getenv("SR_NATIVE_EXEC_QCAP");
      // Default: the full ring. A smaller one keeps the producer's slots cache-warm, but an A/B OFF
      // window (the CP drains only at IB ends) could overflow it and drop draws.
      return (v && *v) ? uint32_t(std::strtoul(v, nullptr, 0)) : 0u;
    }();
    e.capSet = true;
    if (s_cap) q.setCapacity(s_cap);
  }
}
GuestDrawCommand* execReserveSlot() {
  auto& q = getDrawCommandQueue();
  GuestDrawCommand* s = q.reserve();
  if (s) return s;
  execEnsureStarted();
  const auto t0 = std::chrono::steady_clock::now();
  struct ProfRes {
    uint64_t p0;
    ~ProfRes() {
      if (p0) sr::native::exec::profReserveWait(sr::native::exec::profNow() - p0);
    }
  } profRes{sr::native::exec::profOn() ? sr::native::exec::profNow() : 0};
  auto nextLog = t0 + std::chrono::seconds(2);
  for (uint32_t i = 0; (s = q.reserve()) == nullptr; i++) {
    execWake();
    if (i < 2000) {
      _mm_pause();
      continue;
    }
    std::this_thread::yield();
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextLog) {
      nextLog = now + std::chrono::seconds(10);
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_EXEC] WATCHDOG CP waited %.1fs for a queue slot (queue=%u)",
                  std::chrono::duration<double>(now - t0).count(), q.size());
    }
  }
  return s;
}
void execPush(GuestDrawCommand* s, uint8_t kind) {
  Exec& e = ex();
  auto& q = getDrawCommandQueue();
  s->effectSeq = q.committed();
  e.effectsPushed.fetch_add(1, std::memory_order_release);
  q.commit();
  sr::native::exec::noteEffectPushed(kind, kind == sr::native::exec::kStores ? s->effectN : 0u);
  execWake();
}
}  // namespace

void nativeExecKick() {
  // Only while deferring: in an A/B OFF window the CP drains itself, and waking the idle executor for
  // every draw would charge the OFF windows with lock + notify + context-switch work they never do.
  if (!sr::native::exec::deferring()) return;
  if (!ex().running.load(std::memory_order_relaxed)) execEnsureStarted();
  execWake();
}

void nativeExecPushStores(const uint32_t* quads, uint32_t n) {
  execEnsureStarted();
  {
    Exec& e = ex();
    Pm4Engine& eng = livePm4();
    const uint64_t idx = e.effectsPushed.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < sr::native::exec::kStoresPerItem; i++) {
      const uint32_t op = quads[4 * i], a = quads[4 * i + 1], aux = quads[4 * i + 3];
      const uint8_t* p = (op == sr::native::exec::kOpEvShd && aux && eng.cp.virtToHost)
                             ? eng.cp.virtToHost(eng.cp.user, aux)
                             : eng.physToHost(eng.user, a & ~3u);
      e.pend[e.pendN++ % Exec::kPend] = Exec::Pending{p, idx};
    }
  }
  GuestDrawCommand* s = execReserveSlot();
  std::memset(s, 0, offsetof(GuestDrawCommand, aluVs));
  s->effectKind = sr::native::exec::kStores;
  s->effectN = uint8_t(std::min<uint32_t>(n, sr::native::exec::kStoresPerItem));
  std::memcpy(s->drawParams, quads, 16u * s->effectN);
  execPush(s, sr::native::exec::kStores);
}

GuestDrawCommand* nativeExecReserve() { return execReserveSlot(); }

uint64_t nativeExecPendingEffects() {
  Exec& e = ex();
  return e.effectsPushed.load(std::memory_order_acquire) - e.effectsDone.load(std::memory_order_acquire);
}

bool nativeExecPendingStoreAt(const uint8_t* word) {
  Exec& e = ex();
  const uint64_t done = e.effectsDone.load(std::memory_order_acquire);
  const uint64_t pushed = e.effectsPushed.load(std::memory_order_acquire);
  if (pushed == done) return false;
  if (pushed - done >= Exec::kPend / 2) return true;  // more queued than the record holds: be safe
  for (uint64_t k = e.pendN; k > 0 && e.pendN - k < Exec::kPend; k--) {
    const Exec::Pending& q = e.pend[(k - 1) % Exec::kPend];
    if (q.effect < done) break;  // performed (stores are performed in push order)
    if (q.p == word) return true;
  }
  return false;
}
#endif  // SR_ORACLE (the SR_NATIVE_EXEC executor)

void cpIdleFlush() {
  if (sr::native::exec::deferring()) {
    nativeExecKick();
    return;
  }
  cpFlushDraws();
}

void cpFlushDraws() {
  if (!pm4LiveBackendAttached() || !pm4LiveEnabled()) return;
#if SR_ORACLE
  if (sr::native::exec::gateOn() && ex().running.load(std::memory_order_acquire)) {
    if (sr::native::exec::deferring()) {
      execSync();  // the executor drained (and performed) everything pushed so far
      sr::native::exec::syncReason(0);
      return;
    }
    sr::native::exec::syncReason(0);
    std::lock_guard<std::recursive_mutex> lk(s_liveMu);  // an A/B OFF window: this thread, as before
    execDrainAllLocked(kDsFence);
    return;
  }
#endif
  std::lock_guard<std::recursive_mutex> lk(s_liveMu);
  drainCounted(kDsFence);
}

void cpZpdFence() {
  namespace zpd = sr::gfx::zpd;
  if (!zpd::fenceNeedsFlush()) return;
  if (zpd::mode() != zpd::kStrict) {
    zpd::fenceCovered();  // the latency model: written when the GPU finishes (after this fence = late)
    return;
  }
  if (pm4LiveBackendAttached() && pm4LiveEnabled()) {
    std::lock_guard<std::recursive_mutex> lk(s_liveMu);  // recursive: the executor's store item already holds it
    if (s_liveBe) {
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = s_liveBe->pm4ZpdFlushPartial();
      zpd::noteFlush(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - t0).count()),
                     ok);
    }
  }
  // Strict never publishes the fence over an unwritten slot (Astra round 1 P1): whatever the partial frame did not
  // write - it failed, timed out, or there is no backend - gets Xenia's strict fallback now, before the caller's
  // store (the slot's last count, else 1; the real count is dropped when it arrives).
  if (zpd::fenceNeedsFlush()) zpd::fenceFallback();
}

namespace {
// The swap's present (s_liveMu held): fbPhys / w / h from PM4_XE_SWAP, pitch / tiling / size from
// fetch constant 0 as it stood at the swap packet.
void presentSwapLocked(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t d0, uint32_t d2,
                       const sr::gfx::zpd::FrameStamp& zst) {
  if (!s_liveBe) return;
  uint32_t w = width, h = height;
  if ((d0 & 3u) == 2u) {
    w = (d2 & 0x1FFFu) + 1u;
    h = ((d2 >> 13) & 0x1FFFu) + 1u;
  }
  uint32_t pitch = ((d0 >> 22) & 0x1FFu) << 5;
  if (pitch < w) pitch = w;
  const bool tiled = ((d0 >> 31) & 1u) != 0;
  if (!fbPhys || !w || !h || w > 8192 || h > 8192) return;
  drainCounted(kDsSwap);
  // The backend captures this stamp with the frame when it hands work to its render thread.
  sr::gfx::zpd::setPresentingStamp(zst);
  s_liveBe->pm4PresentFrame(fbPhys & 0x1FFFFFFFu, w, h, pitch, tiled);
  sr::gfx::zpd::setPresentingStamp({});
}
}  // namespace

bool pm4EarlyInitOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_PM4_EARLY_INIT");
    return !e || e[0] != '0';   // baked default ON (step 4 7a A/B, lane D NOTES); =0 = the lazy first-frame setup
  }();
  return on;
}

void cpEarlyInit() {
  const bool early = pm4EarlyInitOn(), pso = psoAnyOn();
  if ((!early && !pso) || !pm4LiveBackendAttached() || !pm4LiveEnabled()) return;
  std::lock_guard<std::recursive_mutex> lk(s_liveMu);
  if (!s_liveBe) return;
  if (early) s_liveBe->pm4EarlyInit();
  if (pso) s_liveBe->psoAttach();   // step 4 2k: the pipeline census / manifest / warm-up (gfx/pso_warm.hpp)
}

void cpPresentSwap(uint32_t fbPhys, uint32_t width, uint32_t height) {
  // Astra round 2 P2-1: this frame's zpd events, taken at its swap packet (stream order) on every swap - before any
  // early return, so each stamp holds its own frame's only. Empty unless the freeze dump runs (zpd::frameStampOn).
  const sr::gfx::zpd::FrameStamp zst =
      sr::gfx::zpd::frameStampOn() ? sr::gfx::zpd::takeFrameStamp() : sr::gfx::zpd::FrameStamp{};
  if (!pm4LiveBackendAttached() || !pm4LiveEnabled()) return;
  if (sr::native::dcensus::tileOn())  // NATIVE strand A tile census (observe-only)
    sr::native::dcensus::tileFrame(livePm4().predDrawsRun, livePm4().predSkipped, livePm4().predDrawsBlend);
  // Fetch constant 0 (0x4800..0x4805): dword0 pitch bits 22-30 (texels >> 5),
  // tiled bit 31; dword2 size_2d. VdSwap wrote it right before PM4_XE_SWAP.
  auto& regs = getXenosRegisters();
  const uint32_t d0 = regs.read(0x4800u), d2 = regs.read(0x4802u);
#if SR_ORACLE
  if (sr::native::exec::deferring()) {  // NATIVE strand A executor: the swap is a stream item
    execEnsureStarted();
    GuestDrawCommand* s = execReserveSlot();
    std::memset(s, 0, offsetof(GuestDrawCommand, aluVs));
    s->effectKind = sr::native::exec::kSwap;
    s->drawParams[0] = fbPhys;
    s->drawParams[1] = width;
    s->drawParams[2] = height;
    s->drawParams[3] = d0;
    s->drawParams[4] = d2;
    s->drawParams[kSwapStampAt + 0] = zst.begins;  // the frame stamp travels with the deferred swap
    s->drawParams[kSwapStampAt + 1] = zst.rec;
    s->drawParams[kSwapStampAt + 2] = zst.pm4;
    s->drawParams[kSwapStampAt + 3] = zst.frame;
    execPush(s, sr::native::exec::kSwap);
    return;
  }
#endif
  std::lock_guard<std::recursive_mutex> lk(s_liveMu);
#if SR_ORACLE
  if (sr::native::exec::gateOn() && ex().running.load(std::memory_order_acquire)) execDrainAllLocked(kDsSwap);
#endif
  presentSwapLocked(fbPhys, width, height, d0, d2, zst);
}


// iter-397q: PM4_INTERRUPT (0x54) packets consumed by the live engine, for the
// kernel's graphics-interrupt service. On hardware every 0x54 the CP consumes
// raises the graphics interrupt (source=1); the kernel VD_ISR thread polls this
// count and delivers the difference to the guest ISR.
uint64_t getLiveCpInterruptCount() { return livePm4().cpInterrupts; }

// C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): null_renderer's ORACLE HALF - the development PM4 walker's indirect-buffer
// entry (a kind-1 ring item, a ring IB descriptor, the replay of a plain IB) with its window rules, census and the
// IB-end drain. The release configuration (SR_ORACLE=0) links no walker: there the CP stops on a kind-1 item or a ring
// packet ([NATIVE_FLUSH_BAD], kernel/gpu_command_processor.cpp) and this block is compiled out.
#if SR_ORACLE
// iter-458 (Astra 029 A): per-window IB read counts (SR_PHYS_FREE only).
namespace {
std::atomic<uint64_t> s_ibWinA{0}, s_ibWin8{0}, s_ibWinBoth{0}, s_ibWinUnknown{0};
}
IbWindowCounts getLiveIbWindowCounts() {
  return IbWindowCounts{s_ibWinA.load(), s_ibWin8.load(), s_ibWinBoth.load(),
                        s_ibWinUnknown.load()};
}

// iter-372: execute one INDIRECT_BUFFER published by the guest on the GPU ring
// (kernel/gpu_ring_doorbell.cpp). Routed through the SAME engine instance the
// VdSwap window scan uses, so counters, shader state and the resolve latch stay
// coherent no matter which path delivered the packets.
//
// The size bound mirrors the one in Pm4Engine's own 0x3F/0x37 handler
// (iter-371): a descriptor is not evidence, and an unbounded one sends the
// parser into unrelated guest memory.
void executeGuestIndirectBuffer(uint32_t ibPhys, uint32_t ibSizeDwords) {
  constexpr uint32_t kMaxIbDwords = 0x100000u / 4u;   // 1 MB
  auto& eng = livePm4();
  if (ibSizeDwords == 0 || ibSizeDwords > kMaxIbDwords) {
    eng.badIndirectBuffers++;
    return;
  }
  // The window the IB is parsed from (physwin::resolveIbWindow, one pure rule):
  //  * iter-458 (Astra 029 A): under SR_PHYS_FREE indirect buffers are physical-
  //    allocator blocks in the 0xA window; reading the 0x8 alias parsed zeros and
  //    boot hung (pf2: IB#0 347 dwords, draws=0 regWrites=0) - ownerWindow decides.
  //  * iter-460 (SR_PM4_ENGINE_OWNER): otherwise the allocators' live records
  //    (gpuWindow), as every other GPU read. The 0x8 default only worked because
  //    small GPU grants sat at VAs just below 0xA0000000, where the 0x8 alias of
  //    their physical address IS their own VA; with the 299 MB probe reclaimed
  //    (SR_PHYS_LIFO_FREE) they move to 0xB... and the 0x8 read parsed foreign
  //    bytes (176K zero-address IM_LOADs, no resolves, black frames).
  //  * iter-463 F3 (SR_IB_WINDOW_UNIFIED, default ON): contested and unowned IBs
  //    take physToHost's default (0xA), no longer a separate 0x8 one - on a
  //    double-booked page the CP parsed the arena's file bytes at 0x9E8xxxxx
  //    while the IB's fences resolved to 0xBE8xxxxx (v1_gpuhang). Counted and
  //    logged as [PM4_IB_CONTESTED] / [PM4_IB_UNOWNED].
  const uint32_t ibBytes = ibSizeDwords * 4u;
  int ibVerdict = 0;
  const uint32_t ibWin = sr::kernel::physwin::resolveIbWindow(ibPhys, ibBytes, &ibVerdict);
  sr::kernel::physwin::noteIbWindow(ibVerdict, ibPhys, ibBytes, ibWin);
  if (sr::kernel::physalloc::physFreeEnabled()) {
    std::atomic<uint64_t>& ctr = ibVerdict == 0xA ? s_ibWinA
                               : ibVerdict == 8 ? s_ibWin8
                               : ibVerdict == sr::kernel::physwin::kWinBoth ? s_ibWinBoth
                                                                            : s_ibWinUnknown;
    const uint64_t cn = ctr.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((cn & (cn - 1)) == 0)
      SR_LOG_GFX_CMD("[PHYS_IB] ownerWindow: A=%llu 8=%llu BOTH=%llu unknown=%llu | last ib=%08X "
                     "dwords=%u -> %08X",
                     (unsigned long long)s_ibWinA.load(), (unsigned long long)s_ibWin8.load(),
                     (unsigned long long)s_ibWinBoth.load(),
                     (unsigned long long)s_ibWinUnknown.load(), ibPhys, ibSizeDwords, ibWin);
  }
  uint8_t* host = sr::kernel::guestVAToHost(ibWin + (ibPhys & 0x1FFFFFFFu));
  if (!host) {
    eng.badIndirectBuffers++;
    return;
  }
  sr::kernel::gpuHangWdNoteIb(ibPhys, ibSizeDwords, ibWin, host);   // iter-463 IB ring
  sr::kernel::physwin::noteCommandBuffer(ibPhys, ibSizeDwords * 4u,
                                         ibWin == 0xA0000000u ? 0xA : 8);
  // NATIVE strand A tile census (SR_NATIVE_TILE_CENSUS, observe-only): this IB's tile state and work.
  const bool tileCensus = sr::native::dcensus::tileOn();
  uint64_t tc0 = 0, tc1 = 0;
  if (tileCensus) {
    auto& xr = getXenosRegisters();
    const uint32_t r[sr::native::dcensus::kTileRegs] = {uint32_t(eng.binSelect), uint32_t(eng.binSelect >> 32),
                                                       xr.read(0x2080u), xr.read(0x2081u), xr.read(0x2082u),
                                                       xr.read(0x200Eu), xr.read(0x200Fu)};
    sr::native::dcensus::ibBegin(ibPhys, eng.draws, eng.predSkipped, eng.resolveDraws, r);
    tc0 = __rdtsc();
  }
  eng.execute(host, static_cast<size_t>(ibSizeDwords) * 4, 1);
  if (tileCensus) tc1 = __rdtsc();
  // P2: translate this IB's draws NOW - the guest may reuse their memory as
  // soon as it sees the IB's fences, which our CP has just written.
  if (pm4LiveBackendAttached() && pm4LiveEnabled()) {
#if SR_ORACLE
    if (sr::native::exec::deferring()) {
      // NATIVE strand A executor: the drain runs behind on its own thread; the guest sees the IB's
      // fences only when the executor performs them, after these draws.
      execEnsureStarted();
      execWake();
    } else if (sr::native::exec::gateOn() && ex().running.load(std::memory_order_acquire)) {
      std::lock_guard<std::recursive_mutex> lk(s_liveMu);  // an A/B OFF window: drain here as before
      execDrainAllLocked(kDsIbEnd);
    } else
#endif
    if (drainAsyncOn() && !livetrace::armed()) {
      DrainWorker& w = drainWorker();
      {
        std::lock_guard<std::mutex> lk(w.mu);
        if (w.req != w.done) w.coalesced.fetch_add(1, std::memory_order_relaxed);
        w.req++;
      }
      w.cv.notify_one();
    } else {
      std::lock_guard<std::recursive_mutex> lk(s_liveMu);
      drainCounted(kDsIbEnd);
    }
  }
  if (tileCensus)
    sr::native::dcensus::ibEnd(eng.draws, eng.predSkipped, eng.resolveDraws, tc1 - tc0, __rdtsc() - tc1);
    sr::native::exec::profPlace(sr::native::exec::kPlCp);

  // Census on THIS path. The equivalent print lived in the VdSwap window scan,
  // which stands down when the doorbell consumer executes — without this the
  // ring path would be unmeasured, and an unmeasured path is how iter-362's
  // artifact numbers survived for weeks.
  // iter-373: sample on every 256th IB *and* whenever the draw count MOVES.
  // The fixed-interval-only version missed the thing it existed for: in the
  // first deep roll of the session the last sample landed 11 s before the
  // DEF_EXEC transition, so the deep phase — the whole question — went
  // unmeasured. A submission change is exactly what must never be throttled away.
  static std::atomic<uint64_t> s_n{0};
  static std::atomic<uint64_t> s_lastDraws{~0ull};
  const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  const bool drawsMoved =
      s_lastDraws.exchange(eng.draws, std::memory_order_relaxed) != eng.draws;
  // iter-461: the draws move on EVERY IB once the game renders (~250 IBs/s);
  // after the first 2000 moved samples, one in 64 (SR_LOG_VERBOSE=1: all).
  static std::atomic<uint64_t> s_moved{0};
  const bool sampleMoved =
      drawsMoved && (s_moved.fetch_add(1, std::memory_order_relaxed) < 2000 || (n % 64) == 0 ||
                     sr::kernel::logVerbose());
  if (n < 8 || (n % 256) == 0 || sampleMoved) {
    SR_LOG_GFX_CMD("[PM4_RING_EXEC] IB#%llu %u dwords | totals draws=%llu "
                   "regWrites=%llu vsLoads=%llu psLoads=%llu resolves=%llu "
                   "badIB=%llu bogusShader=%llu",
                   (unsigned long long)n, ibSizeDwords,
                   (unsigned long long)eng.draws,
                   (unsigned long long)eng.regWrites,
                   (unsigned long long)eng.vertexShaderLoads,
                   (unsigned long long)eng.pixelShaderLoads,
                   (unsigned long long)eng.resolveEvents,
                   (unsigned long long)eng.badIndirectBuffers,
                   (unsigned long long)eng.bogusShaderLoads);
    char hist[320];
    int hn = 0;
    for (uint32_t op = 0; op < 256 && hn < int(sizeof(hist)) - 16; op++)
      if (eng.opcodeHist[op])
        hn += snprintf(hist + hn, sizeof(hist) - hn, " %02X:%u", op,
                       eng.opcodeHist[op]);
    if (hn) SR_LOG_GFX_CMD("[PM4_RING_EXEC]   opcodes:%s", hist);
  }
}
#endif  // SR_ORACLE (null_renderer's oracle half: executeGuestIndirectBuffer)

// Set by kernel when the guest requests a present/resolve.
static std::atomic<bool> s_resolveRequested{false};

// iter-369g: the guest's frame boundary. Bumped once per VdSwap AFTER that
// swap's command window is parsed. See getSwapFrameSeq in xenos_state.hpp.
static std::atomic<uint64_t> s_swapFrameSeq{0};
uint64_t getSwapFrameSeq() { return s_swapFrameSeq.load(std::memory_order_acquire); }
void bumpSwapFrameSeq() { s_swapFrameSeq.fetch_add(1, std::memory_order_release); }

void onVdSwapFrame(uint32_t ringBufWritePtr, uint32_t commandBufferVA,
                   uint32_t frontBufferVA, uint64_t frameNumber) {
#if !SR_ORACLE
  // Submission owns native presents; this export notification only advances the guest swap sequence.
  s_swapFrameSeq.fetch_add(1, std::memory_order_release);
  (void)ringBufWritePtr; (void)commandBufferVA; (void)frontBufferVA; (void)frameNumber;
#else
  (void)commandBufferVA;
  // VdSwap r3 points INTO the command stream at a 64-dword block the CALLER
  // reserves for VdSwap itself to write swap packets into (Xenia oracle:
  // xboxkrnl_video.cc VdSwap_entry). stub_VdSwap writes that block, so
  // [prev r3, curr r3) is a contiguous big-endian PM4 stream starting on a
  // packet boundary (iter-358h RING_DUMP proof: valid type-0/type-3 chain at
  // prevR3+0x100, zeroed reserved block before it). Parse ONLY windows
  // anchored at a previous r3 — on any reanchor (first frame, wrap, >1MB gap)
  // skip one window rather than enter the stream mid-packet.
  // iter-372: when the DOORBELL consumer is executing (SR_PM4_RING_EXEC=1,
  // kernel/gpu_ring_doorbell.cpp), it delivers the guest's submissions from the
  // ring where the guest actually publishes them. This window-inference path
  // must then stand down: the IB descriptors point INTO the same command buffer
  // this window covers, so running both would execute overlapping commands
  // twice. SR_PM4_WINDOW_LEGACY=1 forces the old path back on for an A/B.
  // C1 (docs/RELEASE_COMPOSITION.md 2.2): the window scan and the segment-chain consumer below are the synchronous
  // legacy path (SR_PM4_CP=0), a fallback component the release configuration compiles out: there
  // pm4CpAsyncEnabled() is constant true and this function only publishes the frame boundary and presents.
#if SR_ORACLE
  static const bool s_windowScan = [] {
    const char* legacy = std::getenv("SR_PM4_WINDOW_LEGACY");
    if (legacy && legacy[0] == '1') return true;
    const char* exec = std::getenv("SR_PM4_RING_EXEC");
    return !(exec && exec[0] == '1');
  }();
#endif
  // P2: with the async CP the ring is the ONLY stream source, the CP presents
  // at PM4_XE_SWAP, and the game's pump thread follows the segment chain.
  if (pm4CpAsyncEnabled()) {
    s_swapFrameSeq.fetch_add(1, std::memory_order_release);
    if (pm4LiveBackendAttached() && pm4LiveEnabled()) return;
  }
#if SR_ORACLE
  else if (s_windowScan &&
      ringBufWritePtr >= 0x84000000u && ringBufWritePtr < 0xC0000000u) {
    // iter-369e: anchor at prevR3 + 0x100, NOT prevR3.
    //
    // The comment above records the iter-358h RING_DUMP proof: the valid
    // type-0/type-3 chain begins at prevR3 + 0x100, and the 64-dword block
    // BEFORE it is the reserved area VdSwap itself fills. We were feeding that
    // reserved block to the parser as if it were packets, so every window
    // entered the stream 0x100 bytes early. Combined with an unvalidated
    // payload size (below) that let the parser walk out of the command buffer
    // entirely and decode guest STRING TABLES and PE image data as GPU packets
    // — captured in the wild as 15KB..242KB "pixel shaders" whose first dwords
    // spell "!This program cannot be run in DOS mode", "Manager\0AttachLi",
    // "rBreath_Hover".
    constexpr uint32_t kVdSwapReservedBytes = 0x100;
    const uint32_t scanStart = (s_prevWritePtr != 0)
                                   ? (s_prevWritePtr + kVdSwapReservedBytes)
                                   : 0;
    const bool anchored = scanStart != 0 && scanStart < ringBufWritePtr &&
                          (ringBufWritePtr - scanStart) <= 0x100000;
    if (anchored) {
      uint8_t* h0 = sr::kernel::guestVAToHost(scanStart);
      uint8_t* h1 = sr::kernel::guestVAToHost(ringBufWritePtr - 4u);
      if (h0 && h1 && h1 > h0) {
        auto& eng = livePm4();
        const uint64_t resolvesBefore = eng.resolveEvents;
        size_t winBytes = ringBufWritePtr - scanStart;
        // iter-369l: bound the window to the guest's WRITTEN extent. Applied
        // ONLY when the trailing fence sits dramatically short of the r3-delta
        // window (idle frames: 424 bytes written into a 229,248-byte window).
        // Busy frames have the fence at the very end and are left untouched, so
        // this cannot truncate real work. SR_PM4_FENCE_BOUND=0 disables for A/B.
        static const bool s_fenceBound = [] {
          const char* e = std::getenv("SR_PM4_FENCE_BOUND");
          return !e || e[0] != '0';
        }();
        if (s_fenceBound) {
          size_t validEnd = 0;
          sr::gfx::Pm4Engine::ScanStop stop;
          const size_t fence = eng.scanExtent(h0, winBytes, &validEnd, &stop);
          // iter-370 (SR_PM4_EXTENT_DEBUG=1, default OFF — per-frame render
          // diagnostics must never tax the default path): a truncation to
          // near-nothing is ambiguous in the extent number alone. An idle frame
          // that genuinely wrote 40 bytes and a scan that WRONGLY rejected a
          // real packet at offset 40 both print "guest wrote 0.0%". Name the
          // stopping dword and dump what follows it, so the two are separable.
          static const bool s_extentDebug = [] {
            const char* e = std::getenv("SR_PM4_EXTENT_DEBUG");
            return e && e[0] == '1';
          }();
          if (s_extentDebug) {
            static std::atomic<uint32_t> s_dLog{0};
            const uint32_t dn = s_dLog.fetch_add(1, std::memory_order_relaxed);
            if (dn < 24) {
              static const char* kReason[] = {"WINDOW_END", "TYPE_ONE",
                                              "OVERRUN", "UNKNOWN_OP"};
              SR_LOG_GFX_CMD("[PM4_EXTENT_DBG] win=%zu fence=%zu validEnd=%zu "
                             "stop=%s @0x%zX hdr=0x%08X (type=%u op=0x%02X cnt=%u)",
                             winBytes, fence, validEnd,
                             kReason[stop.reason], stop.offset, stop.header,
                             (stop.header >> 30) & 3u, (stop.header >> 8) & 0xFFu,
                             ((stop.header >> 16) & 0x3FFFu) + 1u);
              char hex[256];
              int hn = 0;
              for (size_t i = 0; i < 16 && stop.offset + i * 4 + 4 <= winBytes; i++) {
                const uint8_t* p = h0 + stop.offset + i * 4;
                hn += snprintf(hex + hn, sizeof(hex) - hn, " %02X%02X%02X%02X",
                               p[0], p[1], p[2], p[3]);
                if (hn >= int(sizeof(hex)) - 12) break;
              }
              SR_LOG_GFX_CMD("[PM4_EXTENT_DBG]   at stop:%s", hex);
            }
          }
          // iter-369n: prefer the trailing fence; with no fence, fall back to
          // where the header walk stopped looking like PM4. Never parse past
          // that point — beyond it we are, by construction, decoding whatever
          // memory happens to follow (369d caught it decoding guest string
          // tables and PE headers as shader ucode).
          const size_t ext = fence ? fence : validEnd;
          // iter-369m: apply the bound WHENEVER a trailing fence exists, not
          // only when the extent is dramatically short. The quarter-window
          // trigger was my own caution and it was letting PARTIALLY-written
          // windows through — the source of the residual garbage (216 distinct
          // opcodes vs a real stream's ~25). 369k measured busy frames at
          // fenceEnd == consumed, i.e. their fence already sits at the window
          // end, so bounding them there is a no-op beyond the sub-packet tail.
          // No fence found (ext == 0) => no bound, window parsed as before.
          if (ext != 0 && ext < winBytes) {
            eng.windowsTruncated++;
            static std::atomic<uint32_t> s_tLog{0};
            const uint32_t tn = s_tLog.fetch_add(1, std::memory_order_relaxed);
            if (tn < 6 || (tn & (tn - 1)) == 0)
              SR_LOG_GFX_CMD("[PM4_EXTENT] window %zu -> %zu bytes (guest wrote "
                             "%.1f%%; the rest is stale) total=%" PRIu64,
                             winBytes, ext, 100.0 * double(ext) / double(winBytes),
                             eng.windowsTruncated);
            winBytes = ext;
          }
        }
        eng.execute(h0, winBytes, 0);

        // EVENT_WRITE_EXT fired: RB_COPY_DEST_BASE names the resolve target.
        if (eng.resolveEvents != resolvesBefore && eng.resolveBaseAtLastEvent != 0) {
          const uint32_t b = eng.resolveBaseAtLastEvent;
          const uint32_t a12 = b << 12, a8 = b << 8;
          const uint32_t resolved =
              (b >= 0x82000000u && b < 0xF0000000u) ? b :
              (a12 >= 0x82000000u && a12 < 0xF0000000u) ? a12 :
              (a8 >= 0x82000000u && a8 < 0xF0000000u) ? a8 : b;
          s_resolveAddr = resolved;
          sr::kernel::setGuestFramebufferVA(resolved, kFbWidth, kFbHeight,
              kFbWidth * kFbBytesPerPixel);
          // The game's own EVENT_WRITE_EXT is the natural resolve trigger for
          // the D3D12 geometry-resolve path (was previously raised only by the
          // deleted fake cmd[46] dispatcher).
          setResolveRequested();
        }

        static uint64_t s_regionN = 0;
        ++s_regionN;
        if (s_regionN <= 5 || s_regionN % 60 == 0) {
          SR_LOG_GFX_CMD("[GPU:PM4] region#%" PRIu64 " [0x%08X,0x%08X) totals:"
                         " draws=%" PRIu64 " regWrites=%" PRIu64
                         " vsLoads=%" PRIu64 " psLoads=%" PRIu64
                         " resolves=%" PRIu64 " predSkip=%" PRIu64
                         " cpInt=%" PRIu64 " badShaderLoads=%" PRIu64
                         " consumed=%zu/%zu fenceEnd=%zu short=%" PRIu64
                         " desyncStops=%" PRIu64,
                         s_regionN, scanStart, ringBufWritePtr, eng.draws,
                         eng.regWrites, eng.vertexShaderLoads,
                         eng.pixelShaderLoads, eng.resolveEvents,
                         eng.predSkipped, eng.cpInterrupts, eng.badShaderLoads,
                         eng.lastConsumed, eng.lastWindowBytes, eng.lastFenceEnd,
                         eng.windowsShort, eng.desyncStops);
          // iter-369f: dump the cumulative opcode histogram at boot AND later.
          // Dumping only at region#5 made it evidence about BOOT only — the
          // question it must answer is whether INDIRECT_BUFFER (0x3F/0x37) ever
          // appears live, since the console frame's bulk lives inside IBs and
          // we currently see ~45 draws/swap against the trace's 3501. A
          // boot-only sample cannot answer that (same sampling blind spot that
          // produced two wrong conclusions earlier in this arc).
          if (s_regionN == 5 || s_regionN == 600 || s_regionN == 1800) {
            for (int i = 0; i < 256; i++)
              if (eng.opcodeHist[i])
                SR_LOG_GFX_CMD("[GPU:PM4]   opcode 0x%02X x%u%s", i,
                               eng.opcodeHist[i],
                               eng.opcodeUnknown[i] ? "  (UNHANDLED)" : "");
            // iter-369j: THE CULPRIT LIST. Which opcode was last consumed before
            // a window lost alignment — i.e. whose payload size we size wrong.
            for (int i = 0; i < 256; i++)
              if (eng.desyncPrevOp[i])
                SR_LOG_GFX_CMD("[GPU:PM4]   DESYNC-AFTER opcode 0x%02X x%u", i,
                               eng.desyncPrevOp[i]);
          }
        }
      }
    } else if (s_prevWritePtr != 0) {
      static int s_reanchorLog = 0;
      if (++s_reanchorLog <= 10)
        SR_LOG_GFX_CMD("[GPU:PM4] reanchor: prev=0x%08X curr=0x%08X frame=%" PRIu64
                       " — skipping one window",
                       s_prevWritePtr, ringBufWritePtr, frameNumber);
    }

    // iter-397q: run the persistent segment-chain consumer (the software-CP
    // park-cursor drain — see Pm4Engine::drainSegmentChain). During FMV the
    // driver jump-chains segments through a rotating pool instead of
    // publishing ring descriptors; the swap window only carries the ~0x28-byte
    // swap stub + one tail block, and the frame's real content (including the
    // movie's YUV pixel-shader IM_LOAD) lives in the chained segments. The
    // consumption budget comes from the driver's own fence counter at
    // dev+10780 (+2 per closed segment, iter-371) — without it the drain
    // chased stale tail blocks around the rotating pool in a cycle.
    {
      uint32_t newSegs = 0;
      const uint32_t devPP = sr::kernel::readGuestU32BE(0x82000948u);
      const uint32_t dev = devPP ? sr::kernel::readGuestU32BE(devPP) : 0u;
      if (dev) {
        const uint32_t fence = sr::kernel::readGuestU32BE(dev + 10780u);
        static uint32_t s_lastFence = 0;
        if (fence >= s_lastFence) newSegs = (fence - s_lastFence) / 2u;
        s_lastFence = fence;
      }
      livePm4().drainSegmentChain(newSegs);
    }

    // iter-369g: this swap's command window has now been parsed, so one frame's
    // worth of draws is queued. Publish the frame boundary (see getSwapFrameSeq).
    s_swapFrameSeq.fetch_add(1, std::memory_order_release);

    static bool s_everRealRing = false;
    if (!s_everRealRing) {
      s_everRealRing = true;
      SR_LOG_GFX_CMD("[GPU:RING] *** SWITCHED TO REAL RING *** wptr=0x%08X frame=%" PRIu64,
        ringBufWritePtr, frameNumber);
    }
    if (frameNumber <= 10 || frameNumber % 30 == 0) {
      SR_LOG_GFX_CMD("[GPU] ringWritePtr=0x%08X anchored=%d resolveAddr=0x%08X frame=%" PRIu64,
        ringBufWritePtr, anchored ? 1 : 0, s_resolveAddr, frameNumber);
    }
    s_prevWritePtr = ringBufWritePtr;
  }
#endif  // SR_ORACLE (the legacy VdSwap window scan)

  // P2: the guest's swap renders and presents the frame the PM4 path built.
  if (pm4LiveBackendAttached() && pm4LiveEnabled()) {
    uint32_t va = 0;
    int w = 0, h = 0, pitch = 0;
    bool tiled = false;
    std::lock_guard<std::recursive_mutex> lk(s_liveMu);
    if (!s_liveBe) return;
    drainCounted(kDsVdSwap);
    if (sr::kernel::getGuestFramebufferInfo(&va, &w, &h, &pitch, &tiled) && va && w > 0 && h > 0)
      s_liveBe->pm4PresentFrame(va & 0x1FFFFFFFu, uint32_t(w), uint32_t(h), uint32_t(pitch) / 4u, tiled);
    (void)frontBufferVA;
    (void)frameNumber;
    return;
  }

  // iter-437e: publish the frame boundary INTO the draw queue. All producers
  // (HLE draw/resolve hooks + this parse) run on the game's render thread, so
  // the marker lands strictly after frame N's content — the drain consumes
  // only complete frames (console semantics: scanout shows completed swaps
  // only). Root fix for the 437d black-flash class: 39-presents/s drains
  // caught the queue mid-frame and rendered a movie frame without its quad.
  {
    sr::gfx::GuestDrawCommand marker{};
    marker.frameBoundary = true;
    auto& q = sr::gfx::getDrawCommandQueue();
    if (q.pushBlocking(marker, 100)) {  // iter-437e backpressure
      q.notifyBoundaryPushed();
    } else {
      static std::atomic<uint32_t> s_bDropN{0};
      const uint32_t bd = s_bDropN.fetch_add(1, std::memory_order_relaxed) + 1;
      if (bd <= 4 || bd % 1000 == 0)
        SR_LOG_GFX_CMD("[FRAME_MARK] DROPPED (queue full) #%u — frame merges into next", bd);
    }
  }

  // iter-358j: the null-renderer color-cycle clear is DELETED. It wrote 3.6 MB
  // of solid color into frontBufferVA every frame — harmless while that was a
  // test buffer, but since iter-358i the registered front buffer is the game's
  // REAL display surface (0x8AB7F000, created by the FMV player): clearing it
  // is writing state the game owns. The "harness alive" signal is the
  // placeholder present path.
  (void)frontBufferVA;
  (void)frameNumber;
#endif  // SR_ORACLE: legacy swap window / framebuffer fallback
}

// iter-358h: processGpuCommandsBackground() is DELETED. It scanned 32 KB PAST
// the write pointer in the fake device's 8-byte slot format — on the real
// contiguous stream that misreads headers at random offsets, and its
// EVENT_WRITE_SHD handler then wrote fence values to addresses decoded from
// the WRONG dwords: an arbitrary-guest-memory scribbler. Real fences now
// execute in-order through the shared PM4 engine at swap time, and the
// 0x9480C000 liveness fence is written by stub_VdSwap itself every frame.

void setResolveRequested() {
  s_resolveRequested.store(true, std::memory_order_release);
}

bool getResolveRequested() {
  return s_resolveRequested.load(std::memory_order_acquire);
}

bool getAndClearResolveRequested() {
  return s_resolveRequested.exchange(false, std::memory_order_acq_rel);
}

// ========== GPU frame-presented notification ==========
// Bridges D3D12Backend::moveToNextFrame() → GPU IRQ thread signal path.
// After each D3D12 Present, we increment a counter so the GPU IRQ thread
// knows it's safe to signal the guest GPU sync events exactly once.
static std::atomic<uint64_t> s_gpuPresentCount{0};

void notifyGpuFramePresented() {
  s_gpuPresentCount.fetch_add(1, std::memory_order_release);
}

bool consumeGpuFramePresented() {
  // Single-consumer (GPU IRQ thread only) — static local is safe.
  static uint64_t s_lastConsumed = 0;
  uint64_t cur = s_gpuPresentCount.load(std::memory_order_acquire);
  if (cur > s_lastConsumed) {
    s_lastConsumed = cur;
    return true;
  }
  return false;
}

}  // namespace gfx
}  // namespace sr
