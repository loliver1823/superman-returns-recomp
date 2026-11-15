#pragma once
// iter-467 guestcode: in-roll A/B toggling (perf method; docs/PERFORMANCE.md 2.16, TOOLKIT 1.8d).
//
// SR_AB_TOGGLE=NAME[,NAME...] (separators , ; + or space; perf diagnostic, default unset): every listed feature gets a runtime
// flag that flips at each [HIGHFPS_PACE] window close (~2 s of wall time, kernel/highfps.cpp) on a
// block-paired schedule - in every pair of windows (2b, 2b+1) each feature is ON in exactly one,
// which one drawn per feature from a seeded hash (SR_AB_SEED, default 1) - so one roll carries a
// paired A/B for every listed feature at once, and features toggled together stay separable (their
// assignments are independent). The pace line records the mask that held during its window
// (" | ab=0x.."); tools/perf/ab_windows.py turns the lines into per-feature tick / fps / thread-CPU
// deltas with confidence intervals. Requires the pace lines (SR_HIGHFPS_PACE=1).
//
// A feature reads its flag AT THE POINT OF USE, never a startup latch:
//   bool fooOn() {
//     static const bool v = [] { const char* e = std::getenv("SR_FOO"); return !e || e[0] != '0'; }();
//     static const int ab = sr::abt::reg("SR_FOO");
//     return v && (ab < 0 || sr::abt::on(ab));
//   }
// The toggle only switches a feature OFF in its OFF windows: it never turns on what the feature's
// own env gate leaves off (a default-OFF feature is A/B'd with SR_FOO=1 + SR_AB_TOGGLE=SR_FOO).
// Unset (every player's and every natural run), reg() returns -1 and the feature keeps its env
// gate. Only features whose both states are exact (proven by their CHECK) may be listed: the
// toggle changes host speed, never guest-visible results.
#include <atomic>
#include <cstdint>

#include "sr_oracle.hpp"

namespace sr::abt {

#if SR_ORACLE
extern std::atomic<uint32_t> g_mask;

// The feature's bit (its position in SR_AB_TOGGLE), or -1 when it is not toggled.
int reg(const char* name);
inline bool on(int bit) { return ((g_mask.load(std::memory_order_relaxed) >> bit) & 1u) != 0; }

bool active();                 // SR_AB_TOGGLE names at least one feature
// Called by the pace logger when window `closed` ends: returns the mask that held during it and
// switches to the schedule's mask for window closed + 1.
uint32_t onWindowClose(uint64_t closed);
// SR_AB_PRESENT=1 (iter-467 native-a, the frozen-pass A/B): every listed feature flips at EVERY
// guest present instead (VdSwap, on the presenting thread: even presents ON, odd OFF), so that under
// the dense sim freeze consecutive dumped presents come from alternate paths and freeze_diff
// --exact compares them directly. [AB_PRESENT] swap=<n> ab=0x<mask> is logged for the first 16 and
// for every present while the sim is held. The pace windows then report the mask that held last.
void onGuestSwap();

// SR_AB_PRESENT_DRAINED=1 (iter-467 native-a, with SR_AB_PRESENT=1; diagnostic, the orchestrator 2026-10-11): the
// per-present flip no longer lands inside VdSwap - in the middle of the XDK's Swap emission, where the segmentless
// toggle dropped records (NEVER-EXECUTED, ledger p2.segmentless / abp_toggle_drops). onGuestSwap only records the
// next mask as PENDING; the close wrapper (GpuCommandBufferFlush 820FD8C0, kernel/native_flush.cpp) commits it after
// the next close once the stream is quiesced (outside a Z-pass bracket, no open footprint, no incomplete deferred
// segment, the ring read index at the write index, every record executed), logging [AB_DRAIN]. Unset (every other
// roll): drainedMode() is a latched false, onGuestSwap stores the mask exactly as before and the wrapper has no
// pending-mask path.
bool drainedMode();
bool pendingFlip();                                     // a committed-later flip is waiting
void commitPending(uint64_t closeNo, double waitedUs);  // the wrapper: the stream is quiesced - apply it now
void postponePending(const char* why);                  // the wrapper: not quiesced within the bound - next close
// tools/graceful-quit: "[AB_DRAIN] <why> stats ..." (drained mode only) - the terminal snapshot's family.
void drainStatsLine(const char* why);
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): the A/B toggle is compiled out. Every call answers what an unset SR_AB_TOGGLE /
// SR_AB_PRESENT gives: no feature is toggled (reg -1), so each feature keeps its own env gate.
inline int reg(const char*) { return -1; }
inline bool on(int) { return true; }
inline bool active() { return false; }
inline uint32_t onWindowClose(uint64_t) { return 0; }
inline void onGuestSwap() {}
inline bool drainedMode() { return false; }
inline bool pendingFlip() { return false; }
inline void commitPending(uint64_t, double) {}
inline void postponePending(const char*) {}
inline void drainStatsLine(const char*) {}
#endif

}  // namespace sr::abt
