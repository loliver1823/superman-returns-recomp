#pragma once

// iter-467 flicker hunt: the PER-DRAW LEDGER over consecutive presents of a sim freeze
// (SR_DRAW_LEDGER=1, diagnostic, default OFF; pairs with SR_DIAG_SIM_FREEZE).
//
// While the sim loop is held, the render loop re-walks the same frame every pass, so every
// present must emit the same draws with the same state. The ledger records, on the CP thread at
// Pm4Engine::emitDraw, one entry per draw: the draw arguments, the bound shader hashes, the bin
// predication state, the native record (S1) the draw came from, a hash of the register-file
// subset a draw consumes (0x2000-0x23FF render state, 0x4000-0x497F constants / fetch / bool-loop,
// with the vertex-fetch ADDRESSES masked: dynamic vertex buffers are ring-allocated per present)
// and a hash of the vertex data the bound vertex-fetch slots point at (first 4 KB per slot), so a
// draw that reads a different or stale buffer shows as a data difference.
// At the CP swap (XE_SWAP) a present emitted wholly inside a freeze (>= 400 ms held at its first
// draw, still held at the swap) is compared with the previous such present: draws are aligned by
// their arguments (a draw missing from either side is reported), then every aligned draw whose
// state or data differs is examined: the registers that differ in a large share of the differing
// draws are the per-present volatile set (reported once); a draw whose difference includes any
// OTHER register, or whose vertex data differs, is the defect candidate and is reported with its
// registers and values.
//
// Log: [DRAW_LEDGER] (12 presents in full per freeze, 300 overall; summaries every 200 compares).

#include <cstdint>

#include "../kernel/sr_oracle.hpp"

namespace sr::gfx {
class XenosRegisterFile;
namespace ledger {

#if SR_ORACLE
bool on();
#endif
using PhysToHost = uint8_t* (*)(void* user, uint32_t phys);  // Pm4Engine::physToHost
#if SR_ORACLE
// CP thread, at the top of emitDraw (every draw the CP emits, predication already applied).
void noteDraw(const XenosRegisterFile& xr, uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize,
              uint64_t vsHash, uint64_t psHash, uint64_t binSelect, uint64_t binMask, uint64_t nativeRec,
              PhysToHost phys, void* user);
// The draw just noted was not submitted (1 = zero indices, 2 = no vertex fetch, 3 = queue full).
void noteDropped(uint32_t reason);
// CP thread, at XE_SWAP (swaps = the swap ordinal).
void onSwap(uint64_t swaps);
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): the per-draw ledger is compiled out (an unset SR_DRAW_LEDGER: off, no-ops).
inline bool on() { return false; }
inline void noteDraw(const XenosRegisterFile&, uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                     uint64_t, PhysToHost, void*) {}
inline void noteDropped(uint32_t) {}
inline void onSwap(uint64_t) {}
#endif

}  // namespace ledger
}  // namespace sr::gfx
