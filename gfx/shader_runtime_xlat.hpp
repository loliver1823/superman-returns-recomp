#pragma once

// iter-463: runtime shader translation on a cache miss (SR_SHADER_RUNTIME_XLAT, default ON;
// =0 = the old behaviour, a missing shader drops its draws for the whole run).
//
// Xenia translates every shader on first use and skips a draw only until its pipeline exists
// (gpu_flags.cc async_shader_compilation, pipeline_cache.cc, d3d12_command_processor.cc
// "pipeline not ready"). This is the same contract for our PM4 path:
//
//   IM_LOAD miss (pm4_engine) -> ShaderCache::requestTranslation -> a job on ONE host worker
//   thread -> gfx/shader_xlat_core translate() (XenosRecomp linked in + dxcompiler.dll) ->
//   persisted to the runtime cache dir -> ShaderCache::admitRuntime -> the next draw finds it.
//
// THE MISS POLICY (render/shader-coverage, 2026-10-17; SR_SHADER_RUNTIME_XLAT_WAIT, default ON): the IM_LOAD
// that missed WAITS on the CP thread for its translation (bounded: SR_SHADER_RUNTIME_XLAT_WAIT_MS, default
// 30000; a Warn every 5 s; [SHADER_WAIT] names every wait and its outcome), so the draws behind it find the
// shader. Neither build drops or exits for a shader that is merely not translated yet. A draw that still has
// no shader - no template container, refused, failed, the translator unavailable, or the budget passed - is
// a genuine failure: dev drops it ([SHADER_DROP] names it), release stops with the state named
// ([NATIVE_RECORDING] FAILED stage=untranslated-draw ... xlat=<state>). =0 restores dropping a draw until its
// admission (the old async behaviour, the A/B).
//
// Containers come from the game itself: kernel/shader_container_dump.cpp hands every
// CreateVertexShader / CreatePixelShader container to ShaderCache::noteContainer (observe-only
// capture at entry). A replay has no Create calls, so SR_SHADER_CONTAINER_DIRS names corpus
// directories (';'-separated; a stage_missing_vs.py corpus.txt works as an entry).
//
// Environment:
//   SR_SHADER_RUNTIME_XLAT=0        off (A/B)
//   SR_SHADER_RUNTIME_XLAT_WAIT=0   do not wait: the IM_LOAD returns at once and the draws are dropped
//                                   until the admission (the pre-2026-10-17 default; A/B only)
//   SR_SHADER_RUNTIME_XLAT_WAIT_MS  the bound of one wait (default 30000)
//   SR_SHADER_RUNTIME_DIR=<dir>     cache root (default below)
//   SR_SHADER_CONTAINER_DIRS=<list> extra container corpora (replays)
//   SR_DXC_DIR=<dir>                where dxcompiler.dll/dxil.dll live (default: the exe's dir)
//
// Cache root: SR_SHADER_RUNTIME_DIR; else with SR_INSTANCE_ID (every automated roll) a fresh
// %TEMP%/SupermanRecomp/shader_cache-<id>-<pid> so rolls never share or inherit translations
// (the kernel/xam_content.cpp rule); else <exe>/shader_cache when <exe>/portable.txt exists;
// else %LOCALAPPDATA%/SupermanRecomp/shader_cache. Entries live under <root>/xr-<translator id>/
// (the id hashes the translator sources - the clone's and ours - shader_common.h and the DXC
// DLLs), named exactly like data/shader_cache/dxil plus <hash>.<vs|ps>.xlat (provenance) and
// containers/. A per-process %TEMP% dir whose process is gone is pruned when the next one opens.
// Every path variable is read as UTF-16 (_wgetenv).

#include <cstdint>
#include <string>

namespace sr {
namespace gfx {
namespace rtxlat {

bool enabled();  // SR_SHADER_RUNTIME_XLAT (default ON)

// Blocks until no job is queued or running (or the timeout passes); true when idle. The GPU
// trace replay calls it between SR_GPU_TRACE_REPLAY_LOOPS passes, so a later pass sees every
// translation the earlier one requested (deterministic async proofs).
bool waitIdle(uint32_t timeoutMs);

// The worker takes no new job; waits (up to the timeout) for the one in flight to end. Call it
// BEFORE the process starts exiting (the GPU replay does, at its end): a job left running into
// exit() was observed never to finish there (the exit waited out the whole 30 s drain; likely
// the CRT exit lock, which a function-local static's destructor registration takes - not
// proven by a stack). An atexit handler is only the fallback. Returns true when parked.
bool stop(uint32_t timeoutMs);

// The miss policy's census since process start: IM_LOAD waits for a translation, by outcome.
struct WaitStats {
  uint64_t waits = 0, admitted = 0, timeouts = 0, other = 0;  // other: no template / refused / failed
  double totalMs = 0, maxMs = 0;
};
WaitStats waitStats();

// ── test support (tools/difftest/shader_xlat_test.cpp) ──
void setWaitModeForTest(int mode);        // -1 = env, 0 = async, 1 = wait
void setWaitBudgetForTest(int64_t ms);    // -1 = env / default
void setJobDelayForTest(uint32_t ms);     // the worker sleeps this long before each translation
bool waitIdleForTest(uint32_t timeoutMs); // true once no job is queued or running
std::string runtimeDirForTest();          // <root>/xr-<id> of the last load
// Status of one request: "none", "queued", "waiting-template", "admitted", "refused", "failed".
std::string statusForTest(uint64_t hash, bool isVs);
// The same for a cached PS's alpha-test variant request (step 4 lane C round 4, ShaderCache::requestAlphaTestVariant).
std::string alphaTestStatusForTest(uint64_t hash);

}  // namespace rtxlat
}  // namespace gfx
}  // namespace sr
