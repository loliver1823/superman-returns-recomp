#pragma once

// iter-467 NATIVE strand A: the drain census (SR_NATIVE_DRAIN_CENSUS=1, observe-only diagnostic).
//
// Two questions before the CP drain is reworked (NATIVE_ROADMAP "Strand A: the CP drain"):
//   1. dirty-tracked reuse: how often does a drained draw carry the SAME pipeline / target /
//      viewport inputs as the draw before it in the same drain, and what do pm4PrepareDraw and
//      pm4ViewportScissor cost on those draws (the most a reuse can save)?
//   2. pipelined fences: how often does the stream carry a guest-visible effect (EVENT_WRITE_SHD,
//      MEM_WRITE, REG_TO_MEM, INTERRUPT, SWAP, EVENT_WRITE), how many draws lie between two of them,
//      and how many WAIT_REG_MEM packets actually block?
// One [NATIVE_DRAIN_CENSUS] line every 256 swaps, per-frame figures.

#include <cstdint>

#include "sr_oracle.hpp"

namespace sr::native::dcensus {

#if SR_ORACLE
bool on();

// Drain side (the thread that runs drainDrawQueue).
void drainBegin();
// true when key[0..n) equals the previous draw's key in this drain (then it becomes the previous).
bool keySame(const uint32_t* key, uint32_t n);
void notePrep(bool same, uint64_t prepTsc);
void noteViewport(bool same, uint64_t vpTsc);
void noteResolveDraw();

// CP side (Pm4Engine).
void noteEffect(uint32_t op);  // 0x58 0x3D 0x3E 0x54 0x64 0x46
void noteWait(bool blocked);
void noteDraw();               // emitDraw

// ---- predicated tiling census (SR_NATIVE_TILE_CENSUS=1, observe-only) ----
// Per top-level indirect buffer: an IB executed again in the same frame is a tile replay. Counts the
// replays per frame, the tiles per replayed IB, the draws / predicated skips / resolves in first
// executions vs replays, the CP time of replays and of their drains, and which tile-state registers
// differ from the IB's first execution (bin select, window offset, window / screen scissor).
// One [NATIVE_TILE_CENSUS] line per 256 swaps.
bool tileOn();
#endif
constexpr uint32_t kTileRegs = 7;  // binSelect lo, binSelect hi, 0x2080, 0x2081, 0x2082, 0x200E, 0x200F
#if SR_ORACLE
void ibBegin(uint32_t ibPhys, uint64_t draws, uint64_t predSkipped, uint64_t resolves, const uint32_t* regs);
void ibEnd(uint64_t draws, uint64_t predSkipped, uint64_t resolves, uint64_t cpTsc, uint64_t drainTsc);
void tileFrame(uint64_t predDrawsRun, uint64_t predSkipped, uint64_t predBlend);  // at the swap (engine totals)
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): both censuses are compiled out (unset SR_NATIVE_DRAIN_CENSUS /
// SR_NATIVE_TILE_CENSUS: off, every tap a no-op).
inline bool on() { return false; }
inline void drainBegin() {}
inline bool keySame(const uint32_t*, uint32_t) { return false; }
inline void notePrep(bool, uint64_t) {}
inline void noteViewport(bool, uint64_t) {}
inline void noteResolveDraw() {}
inline void noteEffect(uint32_t) {}
inline void noteWait(bool) {}
inline void noteDraw() {}
inline bool tileOn() { return false; }
inline void ibBegin(uint32_t, uint64_t, uint64_t, uint64_t, const uint32_t*) {}
inline void ibEnd(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
inline void tileFrame(uint64_t, uint64_t, uint64_t) {}
#endif

}  // namespace sr::native::dcensus
