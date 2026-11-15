#pragma once

// iter-464 perf lane (docs/PERFORMANCE.md): observe-only performance telemetry.
//
//   SR_PERF_STATS=1          frame events + per-thread CPU, written to SR_PERF_DIR
//                            (default archive/v1_perf/run_<pid>):
//                              frames.csv   S|P,<n>,<t_us>   one row per guest VdSwap (S)
//                                                            and per host Present (P)
//                              threads.csv  <t_us>,<tid>,<user_ms>,<kernel_ms>,<cycles>,<name>
//                                           cumulative, every second, every thread
//                              meta.txt     pid, t0 (epoch ms), the SR_* environment
//                            plus one [PERF] log line per 10 s (fps, frame p50/p95/p99).
//   SR_HOST_PROFILE_HZ=<hz>  the HOST sampling profiler (kernel/host_profile.cpp):
//                            suspends each busy thread at <hz>, reads RIP, unwinds the host
//                            stack (.pdata, no locks, no allocation while suspended) and
//                            writes per-thread self/inclusive/stack histograms every
//                            SR_HOST_PROFILE_SECS (default 10) to hprof_<n>.txt in the same
//                            directory. Recompiled guest functions ARE host functions named
//                            sub_XXXXXXXX, so the host symbol names the guest VA.
//                            tools/perf/host_profile.py symbolizes (DbgHelp + the exe's PDB).
//
// Neither gate changes guest state or timing beyond its own sampling cost; with both
// off every hook below is a single predicted-not-taken bool test.

#include <atomic>
#include <cstdint>
#include <vector>

#include "sr_oracle.hpp"   // SR_ORACLE for the census declarations below (defined 1 unless the build sets 0)

namespace sr::perf {

extern bool g_statsOn;  // SR_PERF_STATS=1 (latched at static init)

// Guest frame boundary: the guest's VdSwap (kernel/stubs.cpp stub_VdSwap).
void onGuestSwapSlow();
inline void onGuestSwap() {
  if (g_statsOn) onGuestSwapSlow();
}
// Host frame boundary: right after IDXGISwapChain::Present returns.
void onHostPresentSlow();
inline void onHostPresent() {
  if (g_statsOn) onHostPresentSlow();
}

// Start the telemetry thread(s) (idempotent; app/main.cpp right after Log::init).
void start();

#if SR_ORACLE
// Step 4 G7 option (c) (lane D, NOTES 2026-10-20 15:34; Fable ruling 11): SR_ALLOC_CENSUS=1 (diagnostic, default OFF,
// absent from release) with SR_PERF_STATS=1 - every SR_ALLOC_CENSUS_SECS (default 30) a host thread writes
// <perf dir>/alloc_census.csv (t_us,group,name,value): (1) every process heap (HeapSummary; LFH heaps also walked by
// size class), (2) the private committed memory no walked heap owns (VirtualQuery, the guest arena and its mirror
// excluded), (3) + (4) the registered sources (the backend's caches, video memory), (5) handles / GDI / USER objects.
using AllocCensusEmit = void (*)(const char* group, const char* name, double value);
using AllocCensusSource = void (*)(void* ctx, AllocCensusEmit emit);
void allocCensusRegister(const char* group, AllocCensusSource fn, void* ctx);
#endif
// Output directory (created on first use); "" when neither gate is on.
const char* outDir();
// Microseconds since process start on the telemetry clock (QPC).
uint64_t nowUs();

// kernel/host_profile.cpp
void startHostProfiler();
// iter-466 stutter (SR_HITCH_LOG + SR_HOST_PROFILE_HZ): ask the profiler to write every stack
// sample it took in [t0Us, t1Us] (nowUs clock) to hitch_samples.txt under hitch <id>. Lock-free
// (a single-producer ring read by the profiler thread); a no-op while the profiler is off.
void hostProfileHitch(uint32_t id, uint64_t t0Us, uint64_t t1Us);

// Every thread of THIS process with its CPU times and context switches, from one
// NtQuerySystemInformation(SystemProcessInformation) call (Toolhelp's Thread32Next maps and
// unmaps its snapshot per thread: ~15% of a core at a 1 s cadence). false on failure.
struct SysThread {
  uint32_t tid = 0;
  uint64_t user100ns = 0, kernel100ns = 0;
  uint32_t ctxSwitches = 0, state = 0, waitReason = 0;
};
bool ownThreads(std::vector<SysThread>& out);

// SR_GUEST_MIRROR=lazy (guest/guest_harness.cpp): demand commits of the base-4GB mirror and
// the RIPs of the first touches. Written by the VEH (relaxed counters), read by the census.
constexpr int kMirrorFaultRips = 16;
extern std::atomic<uint32_t> g_mirrorFaults;
extern uint64_t g_mirrorFaultRip[kMirrorFaultRips];

}  // namespace sr::perf
