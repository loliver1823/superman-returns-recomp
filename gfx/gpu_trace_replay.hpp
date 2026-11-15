#pragma once

// GPU trace replay (SR_GPU_TRACE_REPLAY) — replays a Xenia .xtr GPU stream/frame
// trace through our PM4->D3D12 backend, so renderer work runs against a golden
// menu trace instead of waiting for the game to reach the scene naturally.
// Capture side: instrumented xenia_canary (tools/xenia-trace/
// xenia_gpu_trace_release.patch) with trace_gpu_stream=true, or a single-frame
// trace via xenia's "GPU Trace Frame". Pipeline doc: docs/TOOLKIT.md section 1.6.
//
// The replayer is deliberately self-contained: it allocates its own flat guest
// memory (no XEX, no guest threads), maps trace physical addresses at
// VA = 0x80000000 + (phys & 0x1FFFFFFF) — the same convention the live PM4
// parser uses — and drives the SAME XenosRegisterFile / DrawCommandQueue /
// D3D12Backend the live path uses. kEvent(kSwap) records mark frame boundaries.

#include <cstdint>

namespace sr {
namespace gfx {

class D3D12Backend;

struct GpuTraceReplayStats {
  uint64_t packets = 0;
  uint64_t draws = 0;
  uint64_t regWrites = 0;
  uint64_t memReadBytes = 0;
  uint64_t memReadCmds = 0;
  uint32_t frames = 0;
  uint32_t unknownOpcodes = 0;   // distinct type-3 opcodes we did not handle
  uint32_t snappyFailures = 0;
  // iter-358g: shader microcode loads (PM4 IM_LOAD 0x27 / IM_LOAD_IMMEDIATE 0x2b).
  // Non-zero vertexShaderLoads is the precondition for correct vertex decode: the
  // vertex format lives in the VS's vfetch instructions, not in the fetch constant.
  uint64_t vertexShaderLoads = 0;
  uint64_t pixelShaderLoads = 0;
};

// Replays the trace at tracePath. d3d12 may be null (stats-only replay).
// Returns 0 on success (trace parsed to EOF), nonzero on open/format errors.
// Env knobs: SR_GPU_TRACE_REPLAY_MAXFRAMES (0=all),
//            SR_GPU_TRACE_REPLAY_DUMP_EVERY (BMP cadence, default 60; first 3 always).
int runGpuTraceReplay(const char* tracePath, D3D12Backend* d3d12,
                      GpuTraceReplayStats* outStats = nullptr);

}  // namespace gfx
}  // namespace sr
