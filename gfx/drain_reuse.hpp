#pragma once
// step 6 R6 fix round 2 (Astra round 2 on 2d0b546b; the coordinator 10-22): the parts of the CP drain's reuse
// (gfx/d3d12_backend.cpp drainDrawQueue, SR_NATIVE_DRAIN_REUSE) that are pure logic, shared by the drain and by their
// known-answer tests (tools/difftest/edram_test.cpp testDrainReuse / testSameTargets). No D3D12, no state.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "surface_key.hpp"

namespace sr::gfx::drainreuse {

// The adjacency guard (Astra r2 P1). One monotonic sequence per draining thread (thread_local in the drain). The
// drain's ENTRY bumps it, before any early return, and so does every command the drain consumes. An entry stored by
// the command at sequence s may be reused only by the command at s + 1: never by a command of a later drain (that
// drain's entry bump sits in between, even when the drain returned early), never across an intervening command.
// No drain counter takes part (the former t_reuse.drain == g_drainCalls guard read a telemetry counter the render
// thread reset: a data race and a cross-drain collision).
struct Seq {
  uint64_t v = 0;
  void drainEntry() { ++v; }
  void command() { ++v; }
};
inline bool adjacent(uint64_t stored, uint64_t now) { return stored + 1 == now; }

// Fix rounds 3 + 4 (Astra r3 P2, r4 P2): only a COMPLETED prepare is cached. pm4PrepareDraw can fail transiently - a
// missing alpha-test variant that ShaderCache::admitRuntimeAlphaTest admits later, with no key word changing - and it
// returns true even when a pipeline acquisition failed: the main PSO null (getPm4Pso failed), or the coverage-mark
// PSO null although the mark applies (markWanted). Either can succeed on the next command; a cached incomplete
// result would outlive that. So the entry is stored only when the prepare succeeded AND every pipeline it needs was
// acquired; anything else empties it (the drain stores n = 0) and is never reused.
inline bool cacheable(bool prepared, bool havePso, bool markWanted, bool haveMark) {
  return prepared && havePso && (!markWanted || haveMark);
}

// pm4RegsFromDraw's mapping: the command fields that become the draw's edram::Regs (D3D12Backend::pm4RegsFromDraw
// returns exactly this; pm4PrepareDraw writes nothing into the Regs after it).
template <class Cmd>
inline edram::Regs regsFromCommand(const Cmd& c) {
  edram::Regs r;
  r.surfaceInfo = c.surfaceInfo;
  r.colorInfo[0] = c.colorInfo;
  for (int i = 0; i < 3; i++) r.colorInfo[i + 1] = c.colorInfo123[i];
  r.depthInfo = c.depthInfo;
  r.colorMask = c.colorMask;
  r.depthControl = c.depthControl;
  r.stencilRefMask = c.stencilRefMask;
  r.stencilRefMaskBF = c.stencilRefMaskBF;
  r.modeControl = c.modeControl;
  r.windowOffset = c.windowOffset;
  r.windowScissorTL = c.windowScissorTL;
  r.windowScissorBR = c.windowScissorBR;
  r.screenScissorTL = c.screenScissorTL;
  r.screenScissorBR = c.screenScissorBR;
  r.suScModeCntl = c.suScModeCntl;
  r.clipCntl = c.clipCntl;
  r.vteCntl = c.vteCntl;
  r.vtxCntl = c.vtxCntl;
  for (int i = 0; i < 3; i++) {
    r.vpScale[i] = c.vpScale[i];
    r.vpOffset[i] = c.vpOffset[i];
  }
  r.copyControl = c.copyControl;
  r.copyDestBase = c.copyDestBase;
  r.copyDestPitch = c.copyDestPitch;
  r.copyDestInfo = c.copyDestInfo;
  r.depthClear = c.depthClear;
  r.colorClear = c.colorClear;
  r.colorClearLo = c.colorClearLo;
  std::memcpy(r.resolveVerts, c.resolveVerts, sizeof(r.resolveVerts));
  r.resolveVertsValid = c.resolveVertsValid;
  return r;
}

// Astra r2 P2a: the Regs fields regsFromCommand copies that the reuse key does not carry (the copy / clear / resolve
// registers). On a reuse hit they are refreshed from the CURRENT command, so the reused Regs equal a fresh
// regsFromCommand(c) whenever the keyed fields are equal (every other Regs field is keyed verbatim).
template <class Cmd>
inline void refreshNonKey(edram::Regs& r, const Cmd& c) {
  r.copyControl = c.copyControl;
  r.copyDestBase = c.copyDestBase;
  r.copyDestPitch = c.copyDestPitch;
  r.copyDestInfo = c.copyDestInfo;
  r.depthClear = c.depthClear;
  r.colorClear = c.colorClear;
  r.colorClearLo = c.colorClearLo;
  std::memcpy(r.resolveVerts, c.resolveVerts, sizeof(r.resolveVerts));
  r.resolveVertsValid = c.resolveVertsValid;
}

// The drain census counters (Astra r2 P1, the coordinator's single-writer rule): written by the draining thread only,
// read by the thread that prints [PM4_BIND_CENSUS]. A relaxed load + store (a plain mov on x64: no lock prefix, no
// RMW on the per-draw path); nothing resets them - the reader keeps its last snapshot and reports the delta.
template <class T>
inline void add(std::atomic<T>& a, T v) {
  a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

}  // namespace sr::gfx::drainreuse
