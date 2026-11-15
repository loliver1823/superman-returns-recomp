#pragma once
// Step 4 item 2k(4) (lane D, 2026-10-20): HOST-WORK SCOPES - so the GPU-hang watchdog tells "the CP waits for host
// work on the presenting path" from "the GPU stopped".
//
// The 4(b) / 4(c) sets: every H1 in an every-present capture was the render thread inside a synchronous BMP write
// to the D: HDD (CP phase swap for 3-5 s, the frame's fence written only after it); the 2k first-run stall is the
// same shape with driver pipeline compiles inside frames. A presenting thread (the CP thread, the PM4 render
// thread - each marks itself once at its start) opens a HostWorkScope around such work; the watchdog reads the
// oldest open scope on a presenting thread (hostWorkOldest) and classifies by STATE, never by time (Astra round 3):
// gpuHangHostWorkExplains (kernel/gpu_hang_wd.hpp). A scope on any other thread (the async dump writer, a loader, a
// future warm-up thread) records nothing - it cannot hold a frame by itself.
//
// Gate SR_GPU_HANG_HOST_WORK=1 (default OFF): off, a scope is one cached bool test and records nothing, so the
// watchdog sees no host work and judges exactly as before.
//
// Header-only (C++17 inline variables): the units that open scopes (gfx/native_pipeline.cpp, gfx/host_hlsl.cpp)
// are also linked into standalone tests, which then need no extra source. Lock-free: a fixed table of slots, each
// claimed by compare-exchange; the watchdog reads it with acquire loads and never waits.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#if defined(_WIN32)
// kernel32's own declaration (the same as <processthreadsapi.h>'s), so this header pulls in no <windows.h> and
// leaves each unit's WIN32_LEAN_AND_MEAN / NOMINMAX choice alone
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif

namespace sr::kernel {

enum HostWorkKind : uint32_t {
  kHostWorkNone = 0,
  kHostWorkDumpWrite = 1,      // a present-dump BMP write, or the presenting thread waiting for room in the queue
  kHostWorkShaderCompile = 2,  // a runtime D3DCompile of host HLSL (gfx/host_hlsl.cpp)
  kHostWorkPsoCompile = 3,     // a pipeline-state compile on a cache miss (gfx/native_pipeline.cpp)
};

inline const char* hostWorkName(uint32_t kind) {
  switch (kind) {
    case kHostWorkDumpWrite: return "dump-write";
    case kHostWorkShaderCompile: return "shader-compile";
    case kHostWorkPsoCompile: return "pso-compile";
    default: return "none";
  }
}

namespace hostwork_detail {
constexpr int kSlots = 32;
struct Slot {
  std::atomic<uint32_t> owned{0};   // 0 free, 1 claimed by an open scope
  std::atomic<uint32_t> seq{0};     // seqlock: odd while the fields change (Astra P2 5: no ABA across a reuse)
  std::atomic<uint32_t> kind{kHostWorkNone};
  std::atomic<uint32_t> tid{0};
  std::atomic<uint32_t> onCp{0};
  std::atomic<uint64_t> sinceNs{0};
};
inline Slot g_slots[kSlots];
inline std::atomic<int> g_force{-1};           // tests: -1 = the environment, 0 / 1 = forced
inline thread_local int t_presenting = 0;      // 0 no, 1 a presenting thread, 2 the CP thread
inline uint64_t steadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
inline uint32_t hostTid() {
#if defined(_WIN32)
  return uint32_t(GetCurrentThreadId());
#else
  return 0;
#endif
}
}  // namespace hostwork_detail

// SR_GPU_HANG_HOST_WORK=1 (latched; a test may force it).
inline bool hostWorkEnabled() {
  const int f = hostwork_detail::g_force.load(std::memory_order_relaxed);
  if (f >= 0) return f != 0;
  static const bool on = [] {
    const char* e = std::getenv("SR_GPU_HANG_HOST_WORK");
    return e && e[0] == '1';
  }();
  return on;
}

// Marks the calling thread as a presenting thread (once, at its start). cp = the GPU CP thread.
inline void hostWorkMarkPresentingThread(bool cp) { hostwork_detail::t_presenting = cp ? 2 : 1; }

struct HostWorkView {
  uint32_t kind = kHostWorkNone;
  uint32_t hostTid = 0;
  bool onCp = false;
  uint64_t sinceNs = 0;          // steady_clock ns (the watchdog's clock)
};

// The oldest open scope on a presenting thread; false when none (always, with the gate off).
inline bool hostWorkOldest(HostWorkView* out) {
  using namespace hostwork_detail;
  bool any = false;
  HostWorkView best;
  for (Slot& s : g_slots) {
    if (!s.owned.load(std::memory_order_acquire)) continue;
    for (int tries = 0; tries < 4; ++tries) {
      const uint32_t s1 = s.seq.load(std::memory_order_acquire);
      if (s1 & 1u) continue;   // being written
      HostWorkView v;
      v.kind = s.kind.load(std::memory_order_relaxed);
      v.hostTid = s.tid.load(std::memory_order_relaxed);
      v.onCp = s.onCp.load(std::memory_order_relaxed) != 0;
      v.sinceNs = s.sinceNs.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (s.seq.load(std::memory_order_relaxed) != s1) continue;   // changed while read: retry
      if (v.kind != kHostWorkNone && (!any || v.sinceNs < best.sinceNs)) {
        best = v;
        any = true;
      }
      break;
    }
  }
  if (any && out) *out = best;
  return any;
}

class HostWorkScope {
 public:
  explicit HostWorkScope(uint32_t kind) {
    using namespace hostwork_detail;
    if (!t_presenting || kind == kHostWorkNone || !hostWorkEnabled()) return;
    for (int i = 0; i < kSlots; ++i) {
      Slot& s = g_slots[i];
      uint32_t expected = 0;
      if (s.owned.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel)) {
        s.seq.fetch_add(1, std::memory_order_acq_rel);   // odd: the fields change
        s.tid.store(hostTid(), std::memory_order_relaxed);
        s.onCp.store(t_presenting == 2 ? 1u : 0u, std::memory_order_relaxed);
        s.sinceNs.store(steadyNs(), std::memory_order_relaxed);
        s.kind.store(kind, std::memory_order_relaxed);
        s.seq.fetch_add(1, std::memory_order_release);   // even: stable
        m_slot = i;
        return;
      }
    }
    // table full (32 open scopes on presenting threads): not recorded - the watchdog judges as before
  }
  ~HostWorkScope() {
    if (m_slot < 0) return;
    hostwork_detail::Slot& s = hostwork_detail::g_slots[m_slot];
    s.seq.fetch_add(1, std::memory_order_acq_rel);
    s.kind.store(kHostWorkNone, std::memory_order_relaxed);
    s.seq.fetch_add(1, std::memory_order_release);
    s.owned.store(0, std::memory_order_release);
  }
  HostWorkScope(const HostWorkScope&) = delete;
  HostWorkScope& operator=(const HostWorkScope&) = delete;

 private:
  int m_slot = -1;
};

// Tests: force the gate (-1 = back to the environment) and count the open scopes.
inline void hostWorkForceEnableForTest(int on) { hostwork_detail::g_force.store(on, std::memory_order_relaxed); }
inline int hostWorkOpenForTest() {
  int n = 0;
  for (const hostwork_detail::Slot& s : hostwork_detail::g_slots)
    if (s.owned.load(std::memory_order_acquire) && s.kind.load(std::memory_order_acquire) != kHostWorkNone) ++n;
  return n;
}

}  // namespace sr::kernel
