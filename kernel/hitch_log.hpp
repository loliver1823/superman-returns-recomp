#pragma once

// iter-466 stutter lane (docs/PERFORMANCE.md section 7): the HITCH recorder, SR_HITCH_LOG=1
// (default OFF, observe-only: it reads clocks and bumps its own counters, never guest state).
//
// Every subsystem that can stall a frame bumps a slot here (count, bytes, microseconds, the
// longest single event). At every host Present the recorder takes the interval since the
// previous one; when it exceeds SR_HITCH_RATIO (default 1.5) x the rolling median of the
// last 120 intervals it writes one [HITCH] line: the interval, the median, the guest swap
// intervals inside it, and every slot that moved since the previous Present. With the host
// profiler on (SR_HOST_PROFILE_HZ) it also asks the profiler for that window's stack samples
// of every thread (hitch_samples.txt in the perf dir; tools/perf/hitch_report.py joins them).
// A [HITCH_SUM] line every 10 s carries the same slots over all frames, so a cause that is
// as common in smooth frames as in hitches does not look like a culprit.
//
// Off: every hook is one predicted-not-taken bool test (no clock read).

#include <atomic>
#include <cstdint>

namespace sr::hitch {

enum Ev : int {
  kPso = 0,      // CreateGraphicsPipelineState on a PSO cache miss (pm4, xlat, edram paths)
  kResCreate,    // CreateCommittedResource for textures / RTs / staging during frames
  kTexLoad,      // shared-memory texture loads (untile/convert dispatches recorded)
  kResolve,      // EDRAM resolves (pm4ShmResolve)
  kShaderXlat,   // runtime shader translations finished (worker thread, off the frame)
  kWtProtect,    // write tracking: protect runs before validation (CP thread)
  kWtFault,      // write tracking: guest write faults (VEH), bytes = pages released
  kFileRead,     // guest NtReadFile (bytes)
  kFileWrite,    // guest NtWriteFile (saves)
  kFileOpen,     // guest NtCreateFile / NtOpenFile
  kVmAlloc,      // guest NtAllocateVirtualMemory (bytes)
  kGpuWait,      // render thread blocked on the frame fence (moveToNextFrame)
  kPresentCall,  // time inside IDXGISwapChain::Present
  kRtIdle,       // render thread waiting for the CP's next frame set
  kCpSubmitWait, // CP blocked handing a frame to a busy render thread
  kCount
};

struct Slot {
  std::atomic<uint64_t> n{0}, bytes{0}, us{0}, maxUs{0};
};

extern bool g_on;              // SR_HITCH_LOG=1, latched at static init
extern Slot g_slot[kCount];

uint64_t qpcNow();             // raw QPC ticks
void noteSlow(Ev e, uint64_t t0, uint64_t bytes);

inline uint64_t begin() { return g_on ? qpcNow() : 0; }
inline void end(Ev e, uint64_t t0, uint64_t bytes = 0) {
  if (t0) noteSlow(e, t0, bytes);
}
// Count-only event (no duration).
inline void count(Ev e, uint64_t bytes = 0) {
  if (!g_on) return;
  g_slot[e].n.fetch_add(1, std::memory_order_relaxed);
  if (bytes) g_slot[e].bytes.fetch_add(bytes, std::memory_order_relaxed);
}

struct Scope {
  Ev e;
  uint64_t t0;
  uint64_t bytes = 0;
  explicit Scope(Ev ev) : e(ev), t0(begin()) {}
  ~Scope() { end(e, t0, bytes); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

// Times one call: auto hr = sr::hitch::timed(sr::hitch::kPso, [&] { return dev->Create...; });
template <class F>
inline auto timed(Ev e, F&& f) -> decltype(f()) {
  Scope s(e);
  return f();
}

// Frame boundaries. onHostPresent: right after Present returns (the presenting thread).
void onHostPresent();
void onGuestSwap();
// The host thread id of the guest render loop (the last VdSwap caller); 0 before the first.
uint32_t renderLoopTid();
// A guest file read: lock wait [l0, l1), ReadFile [r0, r1) in QPC ticks (0 = not reached).
// Writes one [HITCH_IO] line when it took > SR_HITCH_IO_MS (default 20): the issuing thread
// (and whether it is the render loop), the host file, offset, length, lock vs read time.
void slowIo(uint32_t handle, uint32_t len, uint64_t off, uint64_t l0, uint64_t l1, uint64_t r0,
            uint64_t r1, void* file);

}  // namespace sr::hitch
