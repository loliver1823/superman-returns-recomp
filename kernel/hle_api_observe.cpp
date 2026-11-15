#include "kernel/resource_reader_observe.hpp"
// iter-440: API-BOUNDARY OBSERVE HOOKS (migration Phase 1→2 bridge).
//
// Phase 0 (docs/MIGRATION_API_HLE.md) named the full D3D9 API surface with
// verified contracts. This file strong-defs the FIRST-LIGHT boundary set as
// observe-only overrides — log + delegate to __imp__, behavior unchanged —
// to prove in a live run that the call stream matches the map before Phase 2
// translates it. Charter class: observe (registered in docs/OVERRIDES.json).
//
// The set (all named+contract-verified by iter-440 subagents, IDA IDB):
//   sub_82112050  D3DDeviceSwap(dev, frontBufSurf, srcRect) — THE Present
//                 choke point; every present path converges here.
//   sub_82101998  D3DDeviceClear(dev, count, rects, flags, color, z, stencil)
//   sub_82101A58  D3DDeviceClearF(dev, flags, rect, float4* color, z, stencil)
//   sub_820F2CA0  D3DDeviceSetRenderTarget(dev, index, surf)
//   sub_820F2FD0  D3DDeviceSetDepthStencilSurface(dev, surf)
//   sub_820F2A50  D3DDeviceSetStreamSource(dev, stream, vb, ofs, stride, mask)
//   sub_820F2C08  D3DDeviceSetIndices(dev, ib)
//   sub_820FFF08  D3DTextureCreateWithBackingMemory(w,h,d,levels,usage,fmt,…)
//   sub_820FFCB0  D3DTextureLockRect(tex, level, out{pitch,pBits}, pt, flags)
//   sub_820FFCC8  D3DTextureUnlockRect(tex)
//
// Log model: per-function atomic counters; first-N verbose lines per fn
// ([API_OBS] tag); a one-line counter summary on every 60th Swap (the frame
// boundary), so one grep answers "does the stream shape match Phase 0".

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "sr_oracle.hpp"   // C1: what SR_ORACLE=0 keeps - see the note after the includes
#if SR_ORACLE
#include "tex_layout.hpp"
#include "../gfx/cube_layout.hpp"  // iter-457 H1a: the ONE cube layout body
#include "cube_capture_rule.hpp"    // iter-457 H1a-r2: THE cube capture body        // iter-456n: THE shared layout/footprint helper
#include "tex_unlock_rule.hpp"   // iter-455n: the shared Unlock-upload rule
#include "phys_window.hpp"       // iter-458 H-WIN: read-window census (observe-only)
#include "tex_census.hpp"   // iter-458 (Astra 068): texture-path census
#include "phys_alloc.hpp"        // iter-458 Q: host-cache history pins (SR_PHYS_FREE)
#include "tex_identity_rule.hpp" // iter-455n Phase 2 v1: key-only identity registry
#include "tex_identity_domain.hpp" // iter-455n r3: production identity domain (capability boundary)
#endif
#include "../app/log.hpp"
#if SR_ORACLE
#include "../gfx/hle_video.hpp"
#include "../gfx/vb_publish.hpp"   // iter-456p Step 3A: VB publication rules
#include "../gfx/tex_ownership.hpp"
#endif
#include "../gfx/shader_cache.hpp"
#include "../gfx/resource_census.hpp"  // mem.resources step 2a: the texture Unlock note (SR_MEM_RESOURCES=census; no-op under SR_ORACLE=0)
#include "../gfx/resource_memory.hpp"
#include "native_resource_writes.hpp"
#include "native_resource_stamps.hpp"
#if SR_ORACLE
#include "../gfx/xenos_state.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>   // iter-449v: 3D/cube census
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // GetCurrentThreadId (producer-thread census)
#include "native_brackets.hpp"
#if SR_ORACLE
#include "native_draw_ledger.hpp"
#include "native_builder_census.hpp"   // iter-467 draw builder S0 census
#endif
#include "native_flush.hpp"   // iter-467 native A S1: the native draw flush
#include "native_rec_b.hpp"   // iter-467 native-b row 9: Resolve / ClearF as native records (CHECK first)
#if SR_ORACLE
#include "../gfx/native_device.hpp"  // NATIVE N1 mode A (SR_NATIVE_RECORD)
#endif
#include "native_leaves.hpp"         // NATIVE leaves (SR_NATIVE_CONST_LEAF)

namespace sr { namespace kernel {
uint32_t readGuestU32BE(uint32_t va);
}}

// ── C1 (kernel/sr_oracle.hpp; docs/release_composition.json) ───────────────────────────────────
// Around the XDK entries this file adds development oracles and the HLE renderer's A/B producer: the [API_OBS]
// observe log (SR_API_OBSERVE, default ON: a log, the calls are unchanged), the HLE mirror and its producers
// (SR_HLE_MIRROR, SR_S6_*, SR_HLE_TEX_*, SR_HLE_VB_PUBLISH, SR_API_PRESENT_BLOCK / SR_API_CLEAR), the SRHT record
// (SR_HLE_TRACE_REC), the probes and censuses, the native::dev record (SR_NATIVE_RECORD), the census / ledger taps,
// the leaf-caller tap (SR_NATIVE_LEAF_CALLERS), the NaN watch (SR_NB_NANWATCH) and the CHECK paths
// (SR_NATIVE_FLUSH_CHECK, SR_NB_REC_CHECK). SR_ORACLE=0 compiles all of it out, each gate reading as unset.
// What stays is the native routing of the shipping path: D3DDeviceSwap -> native::flush::swapLeaf, ClearF /
// Resolve -> native::recb, DrawIndexedVertices / BeginVertices / DrawVertices -> native::flush::draw,
// SetShaderConstantF -> native::leaf, plus s4HashShaderObjUncached (gfx/native_device) and the
// noteVertexBufferCreate entry (kernel/apt_render_watch.cpp). An override with nothing left is compiled out whole:
// the recompiled body runs in its place, as it did behind the pass-through. (The mirror is the HLE renderer's only
// input, so under SR_ORACLE=0 the SR_PM4_LIVE=0 A/B of gfx/hle_video has nothing to draw.)
#if SR_ORACLE
// iter-447 S6-UP: declared at GLOBAL scope, deliberately. The definition sits
// below (it needs s4HashShaderObjCached, which is defined later still), and the
// UP producer calls it from INSIDE the anonymous namespace. A forward
// declaration placed inside that namespace declares a DIFFERENT entity that is
// never defined — which compiles as a second overload and fails at the far call
// site with "ambiguous", not at the near one. Global scope makes both callers
// bind to the one definition.
void publishDrawState(uint32_t dev);
namespace sr { namespace gfx { bool pm4LiveEnabled(); } }  // gfx/d3d12_backend.hpp

namespace {

bool apiObserveEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_API_OBSERVE");
    return !e || e[0] != '0';   // default ON; =0 keeps the A/B
  }();
  return s_on;
}

// iter-442 S2: mirror the API stream into the HLE render queue (the HLE layer's own DRAW
// path: typed commands, constant / declaration / vertex / index snapshots, consumed by the
// hle_video render thread).
// iter-465 perf lane: default = ON only when the HLE layer draws. Under the PM4 draw path
// (SR_PM4_LIVE, the default since P3) nothing renders the mirror - its consumer only counts -
// yet producing it cost the guest's RENDER thread ~22% of its gameplay frame (host profile
// archive/v1_perf/g4_ffx_portdev: publishDrawState / idxGatherPublish / poolWrite / memcpy and
// a condition-variable wake per command, 24K context switches/s on the consumer).
// SR_HLE_MIRROR=1 forces it on (HLE tooling, SRHT traces), =0 off.
bool apiMirrorEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_MIRROR");
    if (e && e[0]) return e[0] != '0';
    // An SRHT recording (SR_HLE_TRACE_REC) records the mirror: keep it on for that.
    const char* rec = std::getenv("SR_HLE_TRACE_REC");
    if (rec && rec[0]) return true;
    return !sr::gfx::pm4LiveEnabled();
  }();
  return s_on;
}

// iter-442 S4.0: the shader-identity probe. Default ON while S4 is being built
// (it is read-only and bounded to ~16 draws), =0 silences it.
bool s4IdProbeEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_S4_ID_PROBE");
    return !e || e[0] != '0';
  }();
  return s_on;
}

struct ObsCounter {
  std::atomic<uint64_t> n{0};
  uint64_t bump() { return n.fetch_add(1, std::memory_order_relaxed) + 1; }
};

ObsCounter s_swap, s_clear, s_clearF, s_setRT, s_setDS, s_setStream, s_setIdx,
    s_texCreate, s_texLock, s_texUnlock;
// iter-456: the in-place header init sub_82616C40 — the UNHOOKED create the
// k1-r5 witness named (its products were the run's only identity-miss textures).
ObsCounter s_texInitHdr;
// iter-442 S1 census: the rest of the first-light set.
ObsCounter s_drawV, s_drawIV, s_drawUP, s_endV, s_setTex, s_setVp, s_setSc,
    s_setVS, s_setPS, s_vbCreate, s_ibCreate;
// iter-442 S4.2 ACTIVITY GATE: draws matching the movie quad's measured shape
// (prim=13 quadlist, count=4 — ENGINE_MAP §6g.3). A CANDIDATE count, not a
// proof: shape alone does not identify the shader. Paired with the arena's
// write count it answers the only question that makes a dark first-light frame
// interpretable — "did this roll have a movie frame to draw at all?"
ObsCounter s_movieQuad;
// S4.5: SetShaders has no guest API call of its own — it is derived state we
// publish per draw — so it needs its own counter to keep [HLE_MIRROR] 1:1.
ObsCounter s_setShadersC;
// iter-443 S6c/S6d: derived-state publishers, each with its own counter so
// [HLE_MIRROR] stays 1:1 (none of these has a guest API call of its own).
ObsCounter s_setDeclC, s_setConstC, s_setRsC;
// Xenos ALU constant files: VS 256 x float4 at dev+1792, PS 224 x float4 at
// dev+5888. Sized here rather than inline so the two capture sites and the
// executor cannot drift apart.
// X360 vertex-decl element: {stream<<16|byteOffset, packed D3DDECLTYPE,
// usage<<16|usageIndex} = 12 bytes. NOT the PC D3DVERTEXELEMENT9's 8.
constexpr uint32_t kDeclElemBytes = 12u;
constexpr uint32_t kVsConstBytes = 256u * 16u;
// iter-449v: WAS 224*16 — 33/208 cached PS declare and unclamped-read
// constants up to c231; the truncated tail read zeros from the adjacent
// shared block. Full 256-float4 file now, plus an 8-byte BOOL tail (VS/PS
// bool dword 0 from dev+9984/+10000 — reference GuestDevice layout, verified
// on the golden dump: VS bools read 0x3 at menu). psBytes >= kPsConstBytes+8
// is the executor's witness that the tail is present (old traces stay
// readable — they simply carry no bools).
constexpr uint32_t kPsConstBytes = 256u * 16u;
constexpr uint32_t kPsConstWithBools = kPsConstBytes + 8u;
// iter-443 S6b: per-frame unique vertex-buffer census (see the draw hook).
std::mutex s_uniqMu;
std::unordered_set<uint64_t> s_uniqKeys;
uint64_t s_uniqBytes = 0;

// ── iter-447 [S6_VBID] state (contract at the definition of vbIdTest, foot of
// this file). Declared HERE beside its census sibling because both the producer
// (noteVertexBufferCreate) and the reporter run long before that definition.
std::mutex s_vbIdMu;
std::unordered_set<uint32_t> s_vbCreatedHdr;    // returned VA, normalised
std::unordered_set<uint32_t> s_vbCreatedPtr;    // pointers found in the header
uint64_t s_vbIdTested = 0, s_vbIdHitHdr = 0, s_vbIdHitPtr = 0, s_vbIdMiss = 0;
uint32_t s_vbIdEgHdr[4] = {}, s_vbIdEgDrawn[4] = {};
uint32_t s_vbIdNEgHdr = 0, s_vbIdNEgDrawn = 0;

// ── iter-444 S6g-a: THE TEXTURE CENSUS ──────────────────────────────────────
// WHY AN INSTRUMENT BEFORE A DESIGN, again: S6b's vertex census killed an
// elaborate residency plan (content stamps, epochs, LRU vs a 256 MB budget) by
// measuring the working set at 13 MB / ~70 buffers. General textures are the
// same question one layer over, and NOTHING currently answers it: how many
// distinct textures does a frame bind, how many bytes would snapshotting them
// cost, which FORMATS actually occur, and - the one that decides the whole
// design - ARE THEY MUTATED after first sight?
//
// That last one is the difference between "snapshot once per key and cache
// for ever" and "every bind needs a revalidation hash". The LLE path already
// answers it render-side (m_texRefresh / s_skips vs s_refreshes, iter-408) but
// that path does not run live, so the number has to be produced here.
//
// Costs nothing when it changes no behaviour: this slice captures NOTHING and
// uploads NOTHING. It only measures, so a wrong design cannot ship on the back
// of it.
struct TexSeen {
  uint64_t sampleHash;   // cheap content fingerprint (3 x 64 B probes)
  uint32_t binds;
  uint32_t changes;      // how many times the fingerprint MOVED
};
std::mutex s_texCensusMu;
std::unordered_map<uint64_t, TexSeen> s_texSeen;     // key -> content state
std::unordered_set<uint64_t> s_texFrameKeys;         // distinct this window
uint64_t s_texFrameBytes = 0;                        // unique span this window
uint64_t s_texBinds = 0, s_texChanged = 0, s_texUnsized = 0;
// `sampled` is the ANTI-BLIND-PROBE counter: mutated=0 means nothing
// unless the fingerprints were actually read. The first version of this
// census hashed an unreadable address 7.8M times and reported "textures
// are immutable" — a design decision built on reading nothing.
uint64_t s_texSampled = 0, s_texUnreadable = 0;
uint32_t s_texProbes = 0;   // content probes issued this window (capped)
uint32_t s_texFmtHist[64] = {};
uint32_t s_texTiled = 0, s_texLinear = 0;
uint32_t s_texMaxBytes = 0;

// Block geometry per Xenos texture format — the SAME table the proven decoder
// uses (d3d12_backend.cpp getOrCreateTexture2D). Returns false for a format we
// do not decode, so "unsupported" is COUNTED rather than silently sized wrong.
// SR_S6_TEXCENSUS=1. DEFAULT OFF because this is a DIAGNOSTIC on a hook that
// runs millions of times a run: it takes a lock per bind and does a
// VirtualQuery-guarded content probe per newly-seen key. A measuring
// instrument must never be able to move the natural baseline it is measuring.
// ── iter-444 S6g: GENERAL TEXTURE CAPTURE ───────────────────────────────────
// SR_S6_TEX, DEFAULT OFF. Same discipline as SR_S6_WIDE: it changes what the
// screen shows, so it stays opt-in until a roll is recorded and eyeballed.
// ── iter-457 H1a: THE CUBE CAPTURE GATE ──────────────────────────────────────
// DEFAULT OFF. It changes what the producer RECORDS (a cube key stops being
// captured as one 2D face and becomes a two-region six-face snapshot), so a
// trace taken with it on is a different program from one taken with it off.
// Nothing consumes cube uploads yet - the executor refuses them by name until
// H1(b) lands the backend path - so switching this on today changes the trace
// and NOT the picture. That is deliberate: the recording path is proven before
// the one budgeted fresh capture is spent on it.
// The PARSE, split from the LATCH. `cubeCaptureEnabled()` caches its answer in
// a function-local static, which is correct for production (one env read per
// process) and made the gate untestable alongside the dispatch: C9 asserts the
// DEFAULT and C13 needs the gate ON, and whichever ran first won the latch
// forever. Testing this pure function tests the rule; the latch is plumbing.
bool cubeGateWantsOn(const char* e) { return e && e[0] == '1'; }

bool cubeCaptureEnabled() {
  static const bool v = cubeGateWantsOn(std::getenv("SR_HLE_CUBE_CAPTURE"));
  return v;
}

bool texCaptureEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_S6_TEX");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
  // iter-465 perf lane: the captures feed the HLE draw path's queue (TextureUpload commands
  // into the HLE arena), so the default follows the mirror: OFF under the PM4 draw path,
  // where nothing renders them. SR_S6_TEX=1 forces it on.
    if (e && e[0]) return e[0] != '0';
    // The texture-contract self-test processes (SR_HLE_TEX_UNLOCK_TEST / _IDENTITY_TEST, C13/C14)
    // exercise the capture with its pre-iter-465 default (ON).
    const char* t1 = std::getenv("SR_HLE_TEX_UNLOCK_TEST");
    const char* t2 = std::getenv("SR_HLE_TEX_IDENTITY_TEST");
    if ((t1 && t1[0] == '1') || (t2 && t2[0] == '1')) return true;
    return apiMirrorEnabled();
  }();
  return v;
}
// Keys already published. Capture is ONCE PER KEY: the census measured 883
// distinct textures across a run against ~100K binds per 60-frame window, so
// capturing per bind would move gigabytes to re-upload bytes that (98.1% of the
// time) did not change. ⚠ THE MEASURED GAP: 1.9% of keys DO mutate, and this
// slice does not re-capture them - it is counted, named here, and left to the
// invalidation slice rather than papered over.
std::mutex s_texCapMu;
std::unordered_set<uint64_t> s_texCaptured;
uint64_t s_texCapN = 0, s_texCapBytes = 0, s_texCapReject = 0, s_texCapBig = 0;
// ── iter-445 S1: THE ALIAS COUNTER ──────────────────────────────────────────
// The capture key is the full tuple (base, fmt, w, h) but the command — and
// therefore `s_texSlot` — carries only `base` (RenderCommand is at its 32-byte
// cap). So two DIFFERENT textures that share a 4 KB page base collapse onto one
// slot, last writer wins. That may be rare or may be routine; nothing measures
// it. This does, without changing any behaviour: if the count is non-zero,
// widening the key is a follow-on slice, and until then any "the wrong texture
// is bound" symptom has a named second suspect.
struct TexIdent { uint32_t fmt, w, h; };
std::unordered_map<uint32_t, TexIdent> s_texIdent;   // base -> first identity
uint64_t s_texAlias = 0;
uint32_t s_texUnsupported = 0;                        // formats texBlockGeom refuses
// iter-457 H1(a)-r3: the cube dispatch witness and its outcome.
std::atomic<uint64_t> s_cubeDispatches{0};
std::atomic<bool> s_delegateBypassForTest{false};
std::atomic<uint64_t> s_legacy2DEntries{0};
std::atomic<uint64_t> s_cubeCapPublished{0};
std::atomic<const char*> s_cubeCapLastRefuse{nullptr};

bool texCensusEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_S6_TEXCENSUS");
    return e && e[0] == '1';
  }();
  return v;
}

inline bool texBlockGeom(uint32_t fmt, uint32_t& blockW, uint32_t& blockBytes) {
  switch (fmt) {
    // ⛔ iter-447: this table claimed to be "the SAME table the proven decoder
    // uses" and was NOT — the decoder accepted k_8 (fmt 2) while this refused
    // it, so every k_8 general texture was dropped producer-side for a format
    // the consumer could already decode. The format census then named the whole
    // refused set exactly (f2+f29+f31 == unsizedFmt to the unit, per window):
    // k_8 was ~75% of all unsupportedFmt refusals.
    case 2:          blockW = 1; blockBytes = 1; return true;   // k_8
    case 6: case 50: blockW = 1; blockBytes = 4; return true;   // k_8_8_8_8
    case 18:         blockW = 4; blockBytes = 8; return true;   // k_DXT1
    case 19: case 20: case 49: blockW = 4; blockBytes = 16; return true;  // DXT2_3/4_5/DXN
    case 24:         blockW = 1; blockBytes = 2; return true;   // k_16
    // iter-447: ORACLE-BACKED, not guessed — Xenia's base-format table aliases
    // k_16_16_16_16_EXPAND to k_16_16_16_16_FLOAT (texture_info.h:32), i.e. the
    // texels ARE half-floats. 29 = 4x16F (8 B), 31 = k_16_16_FLOAT (4 B).
    case 29:         blockW = 1; blockBytes = 8; return true;   // k_16_16_16_16_EXPAND
    case 31:         blockW = 1; blockBytes = 4; return true;   // k_16_16_FLOAT
    case 36:         blockW = 1; blockBytes = 4; return true;   // k_32_FLOAT
    case 22: case 23: blockW = 1; blockBytes = 4; return true;  // depth-as-texture
    default:         blockW = 1; blockBytes = 4; return false;
  }
}

// ── iter-442 S1: THE RESOURCE-MODEL PROBE ───────────────────────────────────
// THE QUESTION THIS ANSWERS (design risk named by the Phase-2 critic): the
// UnleashedRecomp reference REPLACES the guest D3D9 outright, so its resource
// model is free to own the memory — host-chosen 0x100-aligned pitch, host
// staging shadow, "the host object IS the guest handle", no VA->record table.
// WE DELEGATE to a live guest D3D9 (every hook here calls __imp__), so those
// properties are NOT ours to assume. Adopting them blind produces WRONG PIXELS
// (wrong pitch / stale planes / tiling) that look exactly like translation bugs.
//
// Four facts decide the whole Phase-2a resource design, and all four are
// readable at the existing Lock/Unlock boundary:
//   1. PITCH — is it guest-fixed (we must upload at the guest's pitch) or
//      host-choosable? Compare the Lock-reported pitch against width*bpp.
//   2. TILED — does the fetch constant in the header say the surface is tiled
//      (then the untile path does NOT retire)?
//   3. OWNERSHIP — does pBits point into the texture's own backing allocation
//      (then a VA->record side table is mandatory)?
//   4. DIRTY RANGE — the guest tracks a per-lock dirty span in the header
//      (hdr[5]/hdr[6] as {HIWORD=start, LOWORD=end} in 128-byte units,
//      0xFFFF0000 = empty) which GpuResourceUnlockRestore_sub_820F3C18 flushes
//      on the lock-count 0x100->0 transition. If real, we upload exactly that
//      span and need NO hashing/scanning (retires the iter-408 revalidate hash).
// Log raw dwords, decode offline — a probe that pre-interprets can only confirm
// what it assumed. Fault-free per the VEH hard rule: every read range-checked.
// iter-442 S4.1: the lock result is LATCHED here. Unlock (sub_820FFCC8) is
// passed ONLY the texture handle, so at the one moment we must snapshot pixels
// the pitch and the pixel pointer exist nowhere else — they have to be carried
// forward from the matching Lock. (Under DELEGATE this is not optional the way
// it is for the reference, which owns the staging memory itself.)
struct TexRec {
  uint32_t w, h, depth, levels, usage, fmt;
  sr::kernel::texunlock::LockLatch latch;   // iter-455n r2: cleared per Lock,
                                            // consumed once per Unlock
};
std::mutex s_texMu;
std::unordered_map<uint32_t, TexRec> s_texMap;   // guest VA -> create args
// iter-455n: the movie-plane format dword and THE Unlock snapshot predicate
// now live in kernel/tex_unlock_rule.hpp so the production hook below and the
// self-test (kernel/tex_unlock_test.cpp) share ONE decision body.
constexpr uint32_t kMoviePlaneFmt = sr::kernel::texunlock::kMoviePlaneFmtDword;
using sr::kernel::texunlock::unlockSnapshotWanted;
constexpr size_t kTexMapCap = 4096;

// ── iter-455n Phase 2 v1: KEY-ONLY texture-identity domain (r4-frozen) ────────
// r3 (Dwight r2 REWORK): the identity registry + its mutex/cap/counters + the
// three Identity DomainOrderedLock sites + both registerTexIdentity calls now
// live ENTIRELY in kernel/tex_identity_domain.cpp — a TU that cannot see any
// behavior-domain symbol. This file keeps ONLY the behavior-create accounting
// (s_behCreateCounters), which names the existing s_texMap create semantics
// without changing them (D3, disjoint). The CREATE/LOCK hooks call the domain
// functions (sr::kernel::texid::domain::*). Because the identity seam has no
// access to s_texMap/kTexMapCap/s_texMu, the three frozen structural mutations
// are compile errors, not survivors (see tex_identity_domain.hpp).
sr::kernel::texid::BehaviorCreateCounters s_behCreateCounters;
// Cached once (never a per-hook getenv on the create/Lock hot path). The env
// reader runs exactly once; an observable init count makes that provable so the
// self-test can KILL a per-call-getenv mutant (r2 blocker 3). The gate itself is
// defined at GLOBAL sr::kernel::texid scope near the end of this file (this point
// is inside an anonymous namespace, so it cannot host the real symbol) and is
// declared in tex_identity_rule.hpp so the test TU drives THIS exact gate.
// Bring the real (global-scope) gate into hot-path scope so the hooks call it
// DIRECTLY as productionGateEnabled() — one zero-arg predicate the override
// manifest resolves to its getenv (keeps the SR_HLE_TEX_IDENTITY gate annotated).
using ::sr::kernel::texid::productionGateEnabled;

// iter-455n r2 (director blocker 4): the ONE checked whole-range guest read
// used by the Unlock upload and its window probe. Proves, without ever
// dereferencing: no 32-bit overflow, both ends translate, the translation is
// CONTIGUOUS across the span, and every host page in it is committed+readable.
// The checks PRECEDE access, which is required because the harness VEH makes a
// host AV fatal before any SEH filter runs ("try it and catch" is not an
// option). That ordering is a discipline, NOT a proof of fault-freedom.
const uint8_t* checkedGuestSpan(uint32_t va, uint32_t bytes) {
  if (!va || !bytes) return nullptr;
  if ((uint64_t)va + bytes > 0x100000000ull) return nullptr;
  uint8_t* h0 = sr::kernel::guestVAToHost(va);
  uint8_t* h1 = sr::kernel::guestVAToHost(va + bytes - 1u);
  if (!h0 || !h1 || h1 != h0 + (bytes - 1u)) return nullptr;   // contiguous
  if (!sr::kernel::hostRangeReadable(h0, bytes)) return nullptr;
  return h0;
}

bool texLookup(uint32_t va, TexRec& out) {
  std::lock_guard<std::mutex> lk(s_texMu);
  auto it = s_texMap.find(va);
  if (it == s_texMap.end()) return false;
  out = it->second;
  return true;
}

// Fault-free dword read: returns false rather than touching unmapped memory.
bool safeU32(uint32_t va, uint32_t& out);

// iter-456n (seventh adjudication): THE PIXEL IDENTITY OF A CREATED TEXTURE.
//
// The director required the real identity for the sub_820FFF08 create route
// "without guessing offsets". This does not guess one: a D3D texture header
// carries a GPUTEXTURE_FETCH_CONSTANT at +28, and `basePhys = (f[1] >> 12)
// << 12` is the pixel address. That decode is not new here - it is
// texlayout::decodeFetchConstant, the SAME decode the widened Class-A publish
// already reads at `tex + 28` (see the WidePlan build), covered by the R2-1/R2-2
// golden tests across all 11 admitted formats and four endian modes, and it is
// the identity every upload key in this file is already expressed in.
//
// Fault-free by construction (safeU32/VirtualQuery), because the harness VEH
// makes any host AV fatal before SEH filters run.
// Returns 0 when the header is unreadable or is not a 2D texture fetch, and the
// caller then raises NO lifecycle event - silence is correct, a guessed key
// would be a wrong generation bump on some unrelated object.
// slice 6 / Class B v2 gate. DEFAULT OFF and independent of Class A: with it
// off the Resolve command keeps its historical packing byte-for-byte and no
// resolve authority is ever taken.
// ── slice 6: THE TWO AXES ARE SEPARATE (director's control design) ────────
// DESCRIPTOR CARRIAGE — producer decode, bit packing, trace serialization,
// executor unpacking — is trace-only metadata and must be held CONSTANT in both
// arms of the control, so it gets its own gate. Ownership POLICY (legacy
// permanent set vs epoch-scoped authority) is then the sole OFF/ON variable.
//
// Default OFF and behaviour-inert: with policy off, a carried descriptor is
// written into the command and READ BY NOTHING, so enabling carriage alone
// cannot change a pixel. `SR_HLE_RESOLVE_OWNERSHIP_V2=1` implies carriage, so
// the public gate stays self-contained.
bool resolveDescCarryEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_RESOLVE_DESC_CARRY");
    return e && e[0] == '1';
  }();
  return s_on;
}

bool resolveOwnershipV2Enabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_RESOLVE_OWNERSHIP_V2");
    return e && e[0] == '1';
  }();
  return s_on;
}

uint32_t lifecyclePixelKeyFromHeader(uint32_t tex) {
  if (tex < 0x1000u) return 0;
  uint32_t f[6] = {};
  for (uint32_t i = 0; i < 6; i++)
    if (!safeU32(tex + 28u + 4u * i, f[i])) return 0;
  const sr::kernel::texlayout::FetchDesc fd =
      sr::kernel::texlayout::decodeFetchConstant(f);
  return fd.valid ? fd.basePhys : 0u;
}

bool safeU32(uint32_t va, uint32_t& out) {
  if (va < 0x1000u || !sr::kernel::isGuestVAInRange(va + 4)) return false;
  if (!sr::kernel::guestVAToHost(va)) return false;
  out = sr::kernel::readGuestU32BE(va);
  return true;
}

// Hex of the first `n` bytes at a guest VA (fault-free); "-" if unreadable.
void safeHex(uint32_t va, uint32_t n, char* buf, size_t cap) {
  buf[0] = '-'; buf[1] = 0;
  if (va < 0x1000u || !sr::kernel::isGuestVAInRange(va + n)) return;
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (!p) return;
  size_t o = 0;
  for (uint32_t i = 0; i < n && o + 3 < cap; i++) o += (size_t)snprintf(buf + o, cap - o, "%02X", p[i]);
}

// ── iter-450UD: THE ATLAS-PROVENANCE OBSERVER (SR_AUTOPSY_KEY, default OFF) ──
// Two live hypotheses about the all-zero PRESS START atlas (key 0BA5A000) share
// one measurement, so one observer kills both:
//   H2 (timing) — the first capture ran BEFORE the guest populated the atlas,
//                 and s_texCaptured then deduplicated every later bind away, so
//                 no re-read ever happened.
//   H3 (alias)  — the payload lives in the 0xA window, but the production
//                 heuristic below only tests the FIRST 64 BYTES; a font page
//                 with a blank leading region keeps the zero 0x8 alias.
// Both need the FULL span of BOTH aliases, at BOTH the bind and the pre-draw
// instant — the head-64 probe the production path uses cannot answer either.
//
// ⛔ OBSERVER ONLY. It never selects an alias, uploads, writes guest memory,
// touches draw state, or mutates the dedupe set: the dedupe answer is sampled
// with a `count` under the same mutex, never an insert or erase.
// ⚠ Fault-free by construction — hostRangeReadable gates every span walk,
// because the harness VEH makes a host AV fatal before any SEH filter runs
// (the iter-444 census faulted here and killed the roll).
uint32_t atlasProbeKey() {
  static const uint32_t k = [] {
    const char* e = std::getenv("SR_AUTOPSY_KEY");
    return (e && e[0]) ? (uint32_t)(std::strtoul(e, nullptr, 16) & 0x1FFFFFFFu)
                       : 0u;
  }();
  return k;
}

struct AtlasAliasView {
  bool readable = false;
  uint64_t hash = 0;        // FNV-1a over the WHOLE span, not 3x64 B
  uint64_t nonzero = 0;
  int64_t first = -1, last = -1;
  bool head64 = false;      // what the production heuristic actually tests
};

// ONE walk, shared by the observer and the iter-450UG refresh gate: digest +
// nonzero + first + last in a single pass over an ALREADY-VALIDATED pointer.
// ⚠ The caller owns the fault-free guarantee — every caller must have gated `p`
// through hostRangeReadable, because the harness VEH makes a host AV fatal.
// ⚠ The digest is this project's FNV-STYLE rolling digest, NOT standard FNV-1a
// (director correction 8e826b3). Equal digests CORROBORATE identity; they are
// not a literal byte-identity proof.
AtlasAliasView scanSpanBytes(const uint8_t* p, uint64_t span) {
  AtlasAliasView v;
  v.readable = true;
  uint64_t h = 1469598103934665603ull;
  for (uint64_t i = 0; i < span; i++) {
    const uint8_t b = p[i];
    h = (h ^ b) * 1099511628211ull;
    if (b) {
      v.nonzero++;
      if (v.first < 0) v.first = (int64_t)i;
      v.last = (int64_t)i;
      if (i < 64) v.head64 = true;
    }
  }
  v.hash = h;
  return v;
}

AtlasAliasView atlasScan(uint32_t va, uint64_t span) {
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (!p || !sr::kernel::hostRangeReadable(p, (size_t)span)) return AtlasAliasView{};
  return scanSpanBytes(p, span);
}

// stage = "bind" (right after the delegated SetTexture updated the fetch
// constant, before the capture dedupe) or "predraw" (last act of
// publishDrawState, i.e. immediately before the draw command).
// Defined here (ahead of the FIX A block that uses it) so the selector's return
// type is available to the whole file.
// ⚠ atlasProbe deliberately does NOT call pickAliasWindow — see the ATLASSNAP
// block: that function mutates counters and can emit records, so an observer
// must derive "which window would be selected" from its own scans instead.
struct AliasPick {
  const uint8_t* p = nullptr;   // snapshot `span` bytes here, or null
  uint8_t window = 0;           // 0 = none, 8 = 0x8 window, 0xA = 0xA window
  bool rescued = false;         // 0xA chosen where the head-64 rule kept 0x8
  bool ambiguous = false;       // both populated AND different -> kept 0x8
  bool truncated = false;       // span too large to scan -> head-64 fallback
  bool noneReadable = false;    // 0x8 unreadable (today's drop), counted
};
// PURE gate reader (a function-local `static const bool` over getenv), declared
// here so the ATLASSNAP reporting step can branch on the active selector rule
// WITHOUT calling the selector itself. It touches no counter and emits nothing.
bool aliasFullspanEnabled();

void atlasProbe(const char* stage, uint32_t dev, uint32_t slot) {
  const uint32_t want = atlasProbeKey();
  if (!want || slot >= 32u) return;
  // Cheap rejection first: the pre-draw site runs on EVERY draw, so read only
  // the two dwords that decide relevance before touching the other four.
  const uint32_t fc = dev + 1024u + 24u * slot;
  uint32_t f[6] = {};
  if (!safeU32(fc, f[0]) || !safeU32(fc + 4u, f[1])) return;
  if ((f[0] & 3u) != 2u) return;                    // not a texture fetch slot
  const uint32_t base = ((f[1] >> 12) & 0xFFFFFu) << 12;
  if (!base || (base & 0x1FFFFFFFu) != want) return;
  for (uint32_t i = 2; i < 6; i++)
    if (!safeU32(fc + 4u * i, f[i])) return;

  // Cap the WALKS, not just the prints — two full-span scans per observation is
  // real work and the pre-draw site fires once per qualifying draw.
  // ⚠ The operator's one-shot snapshot (iter-450UI) is UNCAPPED and overrides
  // this; the consume happens further down, once a valid span exists, so a
  // early return can never swallow the single edge.
  static std::atomic<uint64_t> s_probeN{0};
  const uint64_t n = s_probeN.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool capped = (n > 64 && (n & (n - 1u)) != 0);

  const uint32_t fmt = f[1] & 0x3Fu;
  const uint32_t endian = (f[1] >> 6) & 3u;
  const uint32_t width = (f[2] & 0x1FFFu) + 1u;
  const uint32_t height = ((f[2] >> 13) & 0x1FFFu) + 1u;
  const uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  const bool tiled = ((f[0] >> 31) & 1u) != 0;
  const uint32_t texDim = (f[5] >> 9) & 3u;

  // Span computed EXACTLY as the capture path computes it, so what gets
  // measured is "the bytes we would have uploaded" — not a differently-sized
  // window that could disagree for reasons of its own.
  uint32_t blockW = 1, blockBytes = 4;
  const bool known = texBlockGeom(fmt, blockW, blockBytes);
  const uint32_t rowBlocks = (width + blockW - 1u) / blockW;
  const uint32_t rows = (height + blockW - 1u) / blockW;
  uint32_t blockPitch = pitchTexels / blockW;
  if (tiled) blockPitch = (blockPitch + 31u) & ~31u;
  if (!blockPitch) blockPitch = rowBlocks;
  const uint32_t spanRows = tiled ? ((rows + 31u) & ~31u) : rows;
  const uint64_t span = (uint64_t)spanRows * blockPitch * blockBytes;
  if (!span || span > (16u << 20)) return;

  // iter-450UI: claim the operator's ONE-SHOT dual-alias snapshot at the next
  // QUALIFYING bind/predraw — i.e. here, where the key matched and the span is
  // valid. Consuming earlier could burn the edge on an observation that then
  // returned without measuring anything.
  const bool snapNow = sr::gfx::hle::consumeOperatorSnapshot();
  if (capped && !snapNow) return;

  // Dedupe state, READ-ONLY: the same tuple key the capture path inserts.
  const uint64_t key = ((uint64_t)base << 24) ^ ((uint64_t)fmt << 40) ^
                       ((uint64_t)width << 12) ^ (uint64_t)height;
  bool deduped = false;
  {
    std::lock_guard<std::mutex> lk(s_texCapMu);
    deduped = s_texCaptured.count(key) != 0;
  }

  const uint32_t va8 = 0x80000000u | (base & 0x1FFFFFFFu);
  const uint32_t vaA = 0xA0000000u | (base & 0x1FFFFFFFu);
  const AtlasAliasView a8 = atlasScan(va8, span);
  const AtlasAliasView aA = atlasScan(vaA, span);
  // The production heuristic REPLAYED, not applied: start at 0x8; switch to 0xA
  // only when 0x8's first 64 bytes are all zero and 0xA's are not.
  const char* pick = "0x8";
  if (!a8.readable) pick = aA.readable ? "none(0x8-unreadable)" : "none";
  else if (!a8.head64 && aA.readable && aA.head64) pick = "0xA";

  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ATLASPROBE] #%llu %s slot=%u tid=%lu base=%08X fetch=%08X %08X %08X "
      "%08X %08X %08X",
      (unsigned long long)n, stage, slot, GetCurrentThreadId(), base,
      f[0], f[1], f[2], f[3], f[4], f[5]);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ATLASPROBE] #%llu   fmt=%u(%s) %ux%u pitch=%u tiled=%u endian=%u dim=%u"
      " span=%llu dedupedAlready=%u",
      (unsigned long long)n, fmt, known ? "sized" : "UNSIZED", width, height,
      pitchTexels, tiled ? 1u : 0u, endian, texDim, (unsigned long long)span,
      deduped ? 1u : 0u);
  const AtlasAliasView* vv[2] = {&a8, &aA};
  const uint32_t vas[2] = {va8, vaA};
  const char* nm[2] = {"0x8", "0xA"};
  for (int i = 0; i < 2; i++)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[ATLASPROBE] #%llu   %s va=%08X readable=%u hash=%016llX nz=%llu "
        "first=%lld last=%lld head64=%u",
        (unsigned long long)n, nm[i], vas[i], vv[i]->readable ? 1u : 0u,
        (unsigned long long)vv[i]->hash, (unsigned long long)vv[i]->nonzero,
        (long long)vv[i]->first, (long long)vv[i]->last,
        vv[i]->head64 ? 1u : 0u);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ATLASPROBE] #%llu   heuristicWouldSelect=%s",
      (unsigned long long)n, pick);

  // ── iter-450UI: THE OPERATOR'S ONE-SHOT DUAL-ALIAS SNAPSHOT ───────────────
  // Banks the EXACT raw spans of both readable aliases at the instant the
  // operator saw the era on screen, so a later byte-identity claim can compare
  // BYTES instead of digests (the limitation the director named on C-450UHc).
  // ⛔ Observation only: it selects no alias, uploads nothing, mutates no guest
  // memory, no dedupe and no draw state. It reuses this function's own fetch
  // decode, capture-span calculation and hostRangeReadable-gated scans.
  if (snapNow) {
    const char* dir = std::getenv("SR_HLE_SNAP_DIR");
    // ⛔⛔ DO NOT CALL pickAliasWindow HERE. It is NOT a pure query: it bumps
    // s_aliasPick8/PickA, can bump s_aliasRescued / s_aliasNone /
    // s_aliasAmbiguous, runs the old-vs-new selector census, and can EMIT an
    // extra [S6_ALIAS] RESCUED record. For this atlas under full-span mode a
    // "would select" query would have counted a SECOND rescue and corrupted the
    // very terminal census H5 rests on — the reporting step must not be
    // observable in any counter but its own.
    //
    // So derive the answer purely from the a8/aA views ALREADY scanned above:
    // no counters, no logs, no guest reads, no selection, no state change.
    // Semantics reproduce the selector exactly (span > kAliasScanMax cannot
    // occur here — atlasProbe already returned on the same 16 MB bound, so the
    // selector's truncation branch is unreachable from this site).
    uint8_t selWindow;
    if (!a8.readable) {
      selWindow = 0;                                   // none: 0x8 unreadable
    } else if (aliasFullspanEnabled()) {
      selWindow = (a8.nonzero > 0) ? 8u
                : ((aA.readable && aA.nonzero > 0) ? 0xAu : 8u);
    } else {
      selWindow = (!a8.head64 && aA.readable && aA.head64) ? 0xAu : 8u;
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[ATLASSNAP] one-shot at %s slot=%u tid=%lu key=%08X fetch=%08X %08X "
        "%08X %08X %08X %08X fmt=%u %ux%u pitch=%u tiled=%u endian=%u dim=%u "
        "span=%llu selectedWindow=0x%X dedupedAlready=%u dir=%s",
        stage, slot, GetCurrentThreadId(), base, f[0], f[1], f[2], f[3], f[4],
        f[5], fmt, width, height, pitchTexels, tiled ? 1u : 0u, endian, texDim,
        (unsigned long long)span, selWindow, deduped ? 1u : 0u,
        dir ? dir : "(unset — measuring only, banking SKIPPED)");

    const AtlasAliasView* vv[2] = {&a8, &aA};
    const uint32_t vas[2] = {va8, vaA};
    const char* wn[2] = {"8", "A"};
    for (int i = 0; i < 2; i++) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[ATLASSNAP]   alias 0x%s va=%08X readable=%u bytes=%llu nonzero=%llu"
          " first=%lld last=%lld digest=%016llX",
          wn[i], vas[i], vv[i]->readable ? 1u : 0u,
          (unsigned long long)(vv[i]->readable ? span : 0ull),
          (unsigned long long)vv[i]->nonzero, (long long)vv[i]->first,
          (long long)vv[i]->last, (unsigned long long)vv[i]->hash);
      if (!dir || !vv[i]->readable) continue;
      const uint8_t* p = sr::kernel::guestVAToHost(vas[i]);
      if (!p || !sr::kernel::hostRangeReadable(p, (size_t)span)) continue;
      char path[512];
      std::snprintf(path, sizeof(path), "%s/%08X_win%s_%lluB_%016llX.bin", dir,
                    base, wn[i], (unsigned long long)span,
                    (unsigned long long)vv[i]->hash);
      FILE* fp = std::fopen(path, "wb");
      if (!fp) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[ATLASSNAP]   BANK FAILED (cannot open %s) — does the directory "
            "exist?", path);
        continue;
      }
      const size_t wrote = std::fwrite(p, 1, (size_t)span, fp);
      std::fclose(fp);
      // ⚠ PROVE THE FILE, do not assume it. An instrument whose success line
      // prints unconditionally is this project's most repeated defect class:
      // re-open, re-read and re-digest what actually landed on disk.
      uint64_t backHash = 0;
      size_t backBytes = 0;
      if (FILE* rf = std::fopen(path, "rb")) {
        std::vector<uint8_t> buf((size_t)span);
        backBytes = std::fread(buf.data(), 1, buf.size(), rf);
        std::fclose(rf);
        backHash = scanSpanBytes(buf.data(), backBytes).hash;
      }
      const bool ok = (wrote == (size_t)span) && (backBytes == (size_t)span) &&
                      (backHash == vv[i]->hash);
      SR_LOG_BOOT(ok ? sr::LogSeverity::Warn : sr::LogSeverity::Error,
          "[ATLASSNAP]   BANKED %s wrote=%zu readBack=%zu digestBack=%016llX "
          "%s", path, wrote, backBytes, (unsigned long long)backHash,
          ok ? "VERIFIED" : "MISMATCH");
    }
  }
}

// ── iter-450UE FIX A: THE ALIAS-WINDOW SELECTOR ─────────────────────────────
// (SR_HLE_TEX_ALIAS_FULLSPAN, default OFF)
//
// The 0x8 and 0xA guest windows alias one physical page on the console; our
// harness does NOT fold them (iter-425), so they are two distinct host buffers
// and a surface written through one reads all-zero at the other. The heuristic
// this replaces tested only the FIRST 64 BYTES of the 0x8 window — so a texture
// whose leading region is legitimately blank kept the zero 0x8 window while the
// payload sat further in. Measured (iter-450UD [ATLASPROBE], 71/71
// observations): 0x8 entirely zero, 0xA carrying 1685 bytes from offset 257.
//
// ⚠ Fault-free by construction: hostRangeReadable gates EVERY walk, because the
// harness VEH makes a host AV fatal before any SEH filter runs.
// ⛔ NEVER GUESSES: if BOTH windows carry data and they differ, it keeps 0x8 —
// today's answer — and counts the ambiguity. A coin-flip between two populated
// windows would be a new bug wearing a fix's clothes.
// ⚠ COST: the scan EARLY-OUTS at the first nonzero byte, so a data-bearing
// texture costs a handful of byte reads. The full O(span) walk happens only for
// a genuinely all-zero span — exactly the case this exists to catch — and only
// at CAPTURE time (once per distinct key), never per bind.
// Span cap: the same 16 MB bound atlasProbe uses. Above it we do NOT pretend to
// have scanned — we fall back and SAY SO (`truncated`), because a silent cap
// reads as "covered everything" when it did not.
constexpr uint64_t kAliasScanMax = 16ull << 20;

std::atomic<uint64_t> s_aliasPick8{0}, s_aliasPickA{0}, s_aliasRescued{0},
    s_aliasAmbiguous{0}, s_aliasTruncated{0}, s_aliasNone{0};

bool aliasFullspanEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_TEX_ALIAS_FULLSPAN");
    return e && e[0] == '1';   // DEFAULT OFF
  }();
  return v;
}

// First nonzero byte offset, or -1. Early-out is the whole performance story.
int64_t firstNonzeroByte(const uint8_t* p, uint64_t n) {
  for (uint64_t i = 0; i < n; i++)
    if (p[i]) return (int64_t)i;
  return -1;
}

// iter-458 (Astra 029 B): the INDEPENDENT check on an ownership-selected span -
// content only, never ownership. Implausible = the selected window is
// unreadable, or blank over the probe while the other window holds data (the
// class every legacy alias probe exists to catch).
bool resolvedBytesPlausible(const uint8_t* sel, const uint8_t* other, uint64_t n) {
  if (!sel || !sr::kernel::hostRangeReadable(sel, (size_t)n)) return false;
  if (firstNonzeroByte(sel, n) >= 0) return true;
  return !(other && sr::kernel::hostRangeReadable(other, (size_t)n) &&
           firstNonzeroByte(other, n) >= 0);
}

uint64_t fnv1aSpan(const uint8_t* p, uint64_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// ── iter-450UF: THE SIDE-BY-SIDE SELECTOR CENSUS ────────────────────────────
// (SR_HLE_TEX_ALIAS_CENSUS, default OFF)
//
// ⛔ WHY THIS EXISTS, in the director's words: `ambiguous=151` was NOT the
// relevant delta. When BOTH rules keep 0x8, selection did not change and the
// fix introduced no uncertainty at all. The class that actually matters — and
// that iter-450UE admitted but never counted — is where the OLD head-64 rule
// and the NEW full-span rule choose DIFFERENT windows. This measures exactly
// that, evaluating both selectors on every capture.
//
// ⚠ It NEVER alters the selected payload: the active gate alone decides what
// pickAliasWindow returns. This block only observes.
std::atomic<uint64_t> s_selSame{0}, s_selOld8NewA{0}, s_selOldANew8{0},
    s_selNone{0};

bool aliasCensusEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_TEX_ALIAS_CENSUS");
    return e && e[0] == '1';
  }();
  return v;
}

// ── iter-450UG: THE CONTENT-CHANGE REFRESH PROOF GATE ───────────────────────
// (SR_HLE_TEX_REFRESH, default OFF)
//
// ⛔ THIS IS A PROOF GATE, NOT THE PRODUCTION CACHE DESIGN. Neither reference
// hashes texture contents to decide freshness: Xenia arms an MMU page
// write-watch and flips two bools (gpu/shared_memory.cc:343, texture_cache.cc:
// 569); UnleashedRecomp re-uploads the whole slice on every guest Unlock with
// no dedupe at all (gpu/video.cpp:2180-2206). We hash only because we have
// NEITHER signal — we delegate to a live guest D3D9 and have no page watch.
// The deviation and the production candidates are written up in
// docs/MIGRATION_API_HLE.md ("Texture cache coherency — how we deviate").
//
// WHAT IT DOES: revisits an already-captured tuple at later binds, fingerprints
// the EXACT alias-selected capture span, and publishes another TextureUpload
// only when that span's content changed. Versioned per key. General — no atlas
// key, string, glyph or shader special case anywhere in this file.
//
// ⚠ THROTTLED once per key per 60-Swap window. Binds run ~7.8M/run against 883
// distinct textures; an unthrottled full-span digest is gigabytes of re-reads,
// which is precisely why capture chose one-shot (see :201-206).
struct TexRefreshState {
  uint64_t digest = 0;     // over the ALIAS-SELECTED span at last publish
  uint32_t version = 0;    // 0 = original capture, +1 per republish
  uint32_t bytes = 0;      // span at last publish — a size change IS a change
  uint8_t window = 0;      // 8 or 0xA — the window the digest was taken FROM
  bool publishing = false; // one publisher per key at a time
  // iter-456h: the seed capture was ALL-ZERO bytes. The DESIGN_TEXCONTENT_V2
  // causality bar needs the zero->nonzero transition EXHAUSTIVELY (the sampled
  // [S6_TEXREFRESH] log cannot carry it — director verdict b737759e); these
  // transitions are <= the zero-census size (~30/roll), so every one logs.
  bool zeroSeed = false;
  // iter-456j (design-review slice 1): the seed's own content stats + present
  // ordinal, so the FIRST-republish line carries the complete before/after
  // tuple rather than only the after side.
  uint64_t seedNonzero = 0;
  uint64_t seedSwap = 0;
  uint32_t publishes = 0;
};
std::unordered_map<uint64_t, TexRefreshState> s_texRefresh;      // s_texCapMu
std::set<uint64_t> s_texRefreshWindowKeys;                       // s_texCapMu
uint64_t s_trRevisits = 0, s_trProbed = 0, s_trThrottled = 0, s_trUnreadable = 0,
         s_trUnchanged = 0, s_trChanged = 0, s_trPublished = 0, s_trPubBytes = 0,
         s_trReject = 0, s_trTooBig = 0, s_trWindowFlip = 0, s_trRace = 0,
         s_trVolSkip = 0, s_trLogN = 0, s_trFiltered = 0, s_trZeroFill = 0,
         s_trPreN = 0, s_trFirstPub = 0;
// After this many republishes a key is treated as VOLATILE and left alone — the
// VB cache's own precedent for content that legitimately changes every frame.
constexpr uint32_t kTexRefreshMaxPublish = 8;

bool texRefreshEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_TEX_REFRESH");
    return e && e[0] == '1';   // DEFAULT OFF
  }();
  return v;
}

// ── iter-456n Class A gate (director's slice-5 instruction) ─────────────────
// SR_HLE_UNLOCK_UPLOAD_ALL=1 widens the Unlock publish beyond k_8 to every
// backend-supported level-0 2D format. DEFAULT OFF; gate OFF is byte-for-byte
// the iter-455n behaviour, and even gate ON leaves the k_8/atlas route
// untouched (see the routing note at the Unlock snapshot site).
bool unlockUploadAllEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_UNLOCK_UPLOAD_ALL");
    return e && e[0] == '1';   // DEFAULT OFF
  }();
  return v;
}

// iter-450UH: SR_HLE_TEX_REFRESH_KEY=<hex>, default UNSET. A NORMALIZED
// PHYSICAL key filter for the proof gate. Unset = today's global diagnostic
// behaviour, unchanged. Set = only that key's captured-tuple revisits may be
// hashed or republished, which is what makes the gate surgical enough to run
// alongside a boot without the per-frame-surface republish load measured in
// iter-450UG (103 publishes / 105.78 MB of submission traffic).
// ⛔ The key comes from the ENVIRONMENT — the same shape as SR_AUTOPSY_KEY and
// SR_HLE_TRACE_ARM_TEXKEY. No atlas key, menu state, glyph, string or shader is
// hardcoded anywhere in this file.
uint32_t texRefreshKeyFilter() {
  static const uint32_t k = [] {
    const char* e = std::getenv("SR_HLE_TEX_REFRESH_KEY");
    return (e && e[0]) ? (uint32_t)(std::strtoul(e, nullptr, 16) & 0x1FFFFFFFu)
                       : 0u;
  }();
  return k;
}

// ── iter-456j (design-review slices 1+2) ────────────────────────────────────
// SR_HLE_TEX_REFRESH_FIRST=1 — log EVERY key's FIRST successful republish once,
// with the complete before/after tuple. Bounded by distinct republished keys
// (~190/roll measured), so it is EXHAUSTIVE, not sampled; there is no cap, and
// the census carries the count so completeness is checkable rather than
// assumed (director: "any capacity cap needs an explicit overflow counter that
// invalidates completeness" — this has no cap to overflow).
bool texRefreshFirstEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_TEX_REFRESH_FIRST");
    return e && e[0] == '1';   // DEFAULT OFF
  }();
  return v;
}

// SR_HLE_TEX_REFRESH_FROM=<present ordinal> — THE WITHIN-ROLL A/B ARM.
// ⚠ It gates PUBLICATION ONLY (director's amended semantics). Probing and
// hashing run from boot on BOTH sides of N, so the hashing cost — the plausible
// cause of the observed FE-tick half-rate — is CONSTANT across the boundary and
// a content change at N cannot be an artifact of work starting at N. A
// suppressed change does NOT consume the stored digest, so the pending change
// is re-detected and published at/after N rather than lost.
uint64_t texRefreshFromPresent() {
  static const uint64_t v = [] {
    const char* e = std::getenv("SR_HLE_TEX_REFRESH_FROM");
    const long long n = (e && e[0]) ? std::atoll(e) : 0;
    return (uint64_t)(n > 0 ? n : 0);   // 0 = publish from boot (unset)
  }();
  return v;
}

// `physBase` is the UNMASKED base (basePage * 4096); masking happens inside.
// `checkAmbiguity` is true at CAPTURE sites (once per key, so the memcmp is
// affordable) and false anywhere hotter.
// iter-458 (Astra 030): the window pickAliasWindow's LEGACY rule selects for
// these pointers (null = unreadable) - no counters, no logging. It mirrors the
// gate-off / oversized head-64 rule and the SR_HLE_TEX_ALIAS_FULLSPAN rule
// below, so a resolved read can be compared with what the legacy rule would
// have read. 0 = nothing readable (the legacy drop).
int legacyAliasWindow(const uint8_t* p8, const uint8_t* pA, uint64_t span) {
  if (!p8) return 0;
  if (!aliasFullspanEnabled() || span > kAliasScanMax) {
    const uint64_t h = span < 64 ? span : 64;
    return (firstNonzeroByte(p8, h) < 0 && pA && firstNonzeroByte(pA, h) >= 0) ? 0xA : 8;
  }
  if (firstNonzeroByte(p8, span) >= 0) return 8;
  return (pA && firstNonzeroByte(pA, span) >= 0) ? 0xA : 8;
}

// `resolveSite` names the SR_PHYS_RESOLVE counter; kRsSites = the caller
// already resolved (no second decision, nothing counted).
AliasPick pickAliasWindow(uint32_t physBase, uint64_t span, bool checkAmbiguity,
                          uint32_t fmt, uint32_t width, uint32_t height,
                          sr::kernel::physwin::ResolveSite resolveSite =
                              sr::kernel::physwin::kRsTex) {
  AliasPick r;
  const uint32_t va8 = 0x80000000u | (physBase & 0x1FFFFFFFu);
  const uint32_t vaA = 0xA0000000u | (physBase & 0x1FFFFFFFu);
  const uint8_t* p8 = sr::kernel::guestVAToHost(va8);
  if (p8 && !sr::kernel::hostRangeReadable(p8, (size_t)span)) p8 = nullptr;
  const uint8_t* pA = sr::kernel::guestVAToHost(vaA);
  if (pA && !sr::kernel::hostRangeReadable(pA, (size_t)span)) pA = nullptr;

  // ── THE SELECTOR CENSUS (observation only; the active gate still decides) ──
  if (aliasCensusEnabled() && span && span <= kAliasScanMax) {
    const int64_t f8 = p8 ? firstNonzeroByte(p8, span) : -1;
    const int64_t fA = pA ? firstNonzeroByte(pA, span) : -1;
    // OLD rule: 0x8 unless its FIRST 64 BYTES are blank and 0xA's are not.
    int oldWin = 0, newWin = 0;
    if (p8) {
      const bool head8 = f8 >= 0 && f8 < 64;
      const bool headA = fA >= 0 && fA < 64;
      oldWin = (!head8 && pA && headA) ? 0xA : 8;
      // NEW rule: 0x8 unless its WHOLE span is blank and 0xA's is not.
      newWin = (f8 < 0 && fA >= 0) ? 0xA : 8;
    }
    if (!oldWin) {
      s_selNone.fetch_add(1, std::memory_order_relaxed);
    } else if (oldWin == newWin) {
      s_selSame.fetch_add(1, std::memory_order_relaxed);
    } else {
      const bool o8nA = (oldWin == 8 && newWin == 0xA);
      (o8nA ? s_selOld8NewA : s_selOldANew8)
          .fetch_add(1, std::memory_order_relaxed);
      // Every DISTINCT differing key is named once, in full. This is the H5
      // evidence: a divergence that is not named and reduced by key kills it.
      static std::mutex s_dmu;
      static std::set<uint32_t> s_dseen;
      bool fresh = false;
      {
        std::lock_guard<std::mutex> lk(s_dmu);
        if (s_dseen.size() < 4096) fresh = s_dseen.insert(physBase).second;
      }
      if (fresh)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_SELDIFF] %s key=%08X fmt=%u %ux%u span=%llu | 0x8 first=%lld "
            "hash=%016llX | 0xA first=%lld hash=%016llX",
            o8nA ? "old8/newA" : "oldA/new8", physBase, fmt, width, height,
            (unsigned long long)span, (long long)f8,
            (unsigned long long)(p8 ? fnv1aSpan(p8, span) : 0ull),
            (long long)fA,
            (unsigned long long)(pA ? fnv1aSpan(pA, span) : 0ull));
    }
  }

  // iter-458 (Astra 029 B): SR_PHYS_RESOLVE - the allocators' ownership picks
  // the window when it names exactly one; everything below is the legacy rule
  // for BOTH/unknown. An unreadable owned window is a counted FAILED read
  // (Astra 030), never a fall-through to the legacy guess.
  if (const int rw = sr::kernel::physwin::resolveOwned(resolveSite, physBase, span)) {
    const uint8_t* sel = rw == 0xA ? pA : p8;
    const uint8_t* oth = rw == 0xA ? p8 : pA;
    const uint64_t probe = span < kAliasScanMax ? span : 64u;
    const bool selOk = resolvedBytesPlausible(sel, oth, probe);
    // Astra 030: the SAME predicate on the window the legacy rule would pick.
    const int lw = legacyAliasWindow(p8, pA, span);
    const bool legOk = lw == rw ? selOk
                     : lw ? resolvedBytesPlausible(lw == 0xA ? pA : p8, lw == 0xA ? p8 : pA, probe)
                          : false;
    sr::kernel::physwin::resolveValidate(resolveSite, selOk);
    sr::kernel::physwin::resolveCompare(resolveSite, rw, lw, selOk, legOk);
    if (!sel) {
      r.noneReadable = true;
      s_aliasNone.fetch_add(1, std::memory_order_relaxed);
      return r;
    }
    r.p = sel; r.window = (uint8_t)rw;
    (rw == 8 ? s_aliasPick8 : s_aliasPickA).fetch_add(1, std::memory_order_relaxed);
    return r;
  }

  // An unreadable 0x8 is today's drop, and it stays one under the gate: the
  // brief is "start with the READABLE 0x8 window", and an absent window is not
  // the same fact as a zero one. Counted so the class can't hide.
  if (!p8) {
    r.noneReadable = true;
    s_aliasNone.fetch_add(1, std::memory_order_relaxed);
    return r;
  }

  const bool head8 = firstNonzeroByte(p8, span < 64 ? span : 64) >= 0;

  // ── GATE OFF (and the oversized fallback): BYTE-IDENTICAL to the production
  // heuristic, so the gate is a true A/B rather than a rewrite that merely
  // contains the old path.
  if (!aliasFullspanEnabled() || span > kAliasScanMax) {
    if (aliasFullspanEnabled()) {
      r.truncated = true;
      s_aliasTruncated.fetch_add(1, std::memory_order_relaxed);
    }
    r.p = p8; r.window = 8;
    if (!head8 && pA && firstNonzeroByte(pA, span < 64 ? span : 64) >= 0) {
      r.p = pA; r.window = 0xA;
    }
    (r.window == 8 ? s_aliasPick8 : s_aliasPickA)
        .fetch_add(1, std::memory_order_relaxed);
    return r;
  }

  // ── GATE ON: the exact capture span decides, not its first 64 bytes.
  const int64_t f8 = firstNonzeroByte(p8, span);
  if (f8 >= 0) {
    r.p = p8; r.window = 8;
    if (checkAmbiguity && pA && firstNonzeroByte(pA, span) >= 0 &&
        std::memcmp(p8, pA, (size_t)span) != 0) {
      r.ambiguous = true;
      const uint64_t na = s_aliasAmbiguous.fetch_add(1, std::memory_order_relaxed) + 1;
      // Named loudly: this is the ONE case where "keep 0x8" is a choice
      // between two populated windows rather than the only option. If this
      // ever fires, FIX A must not be baked until the class is understood.
      if (na <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_ALIAS] AMBIGUOUS #%llu base=%08X span=%llu — BOTH windows "
            "populated and DIFFERENT; keeping 0x8 (no guess)",
            (unsigned long long)na, physBase, (unsigned long long)span);
    }
    s_aliasPick8.fetch_add(1, std::memory_order_relaxed);
    return r;
  }

  // 0x8's WHOLE span is zero — only now is the alias worth consulting.
  const int64_t fA = pA ? firstNonzeroByte(pA, span) : -1;
  if (fA >= 0) {
    r.p = pA; r.window = 0xA;
    // The old rule would have found it only if it lay in the first 64 bytes;
    // anything at or beyond 64 is a texture this fix genuinely rescued.
    if (fA >= 64) {
      r.rescued = true;
      const uint64_t nr = s_aliasRescued.fetch_add(1, std::memory_order_relaxed) + 1;
      // THE VERDICT NUMBER for FIX A: a texture whose payload begins past byte
      // 64 of the 0xA window, which the head-64 rule uploaded as all-zero.
      if (nr <= 24 || (nr & (nr - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_ALIAS] RESCUED #%llu base=%08X span=%llu firstNonzero=%lld "
            "(0x8 span all zero) -> selected 0xA",
            (unsigned long long)nr, physBase, (unsigned long long)span,
            (long long)fA);
    }
    s_aliasPickA.fetch_add(1, std::memory_order_relaxed);
    return r;
  }

  r.p = p8; r.window = 8;   // both blank: nothing better exists
  s_aliasPick8.fetch_add(1, std::memory_order_relaxed);
  return r;
}

}  // namespace

namespace sr {
namespace kernel {
// iter-458 (Astra 029 B): the texture selector, driven by difftest
// --physib-test with distinguishable window contents. Returns the chosen
// window (0 = none readable).
int pickAliasWindowForTest(uint32_t physBase, uint64_t span, int resolveSite) {
  // resolveSite < 0 goes through the DEFAULT argument, so the default is tested.
  const AliasPick ap =
      resolveSite < 0
          ? pickAliasWindow(physBase, span, /*checkAmbiguity=*/false, 0, 0, 0)
          : pickAliasWindow(physBase, span, /*checkAmbiguity=*/false, 0, 0, 0,
                            (sr::kernel::physwin::ResolveSite)resolveSite);
  return ap.window;
}
}  // namespace kernel
}  // namespace sr

namespace {

// iter-450UG: the revisit. Called ONLY for tuples already in s_texCaptured, and
// only when SR_HLE_TEX_REFRESH=1 — the caller short-circuits on the gate before
// reaching here, so with the gate off this function is never entered and the
// capture block's emitted path is unchanged.
void texRefreshRevisit(uint64_t key, uint32_t base, uint64_t span, uint32_t fmt,
                       uint32_t width, uint32_t height, bool tiled,
                       uint32_t endian, uint32_t pitchTexels,
                       uint64_t tightBytes) {
  // (1) THROTTLE BEFORE ANY GUEST READ — once per key per 60-Swap window.
  uint8_t wantWindow = 0;
  uint64_t prevDigest = 0;
  uint32_t prevVer = 0, prevBytes = 0;
  {
    std::lock_guard<std::mutex> lk(s_texCapMu);
    s_trRevisits++;
    // (0) THE KEY FILTER, before any hash or republish can happen. Counted so
    // conservation still closes: revisits = filtered + probed + throttled +
    // volSkip + unreadable + race.
    const uint32_t filt = texRefreshKeyFilter();
    if (filt && (base & 0x1FFFFFFFu) != filt) { s_trFiltered++; return; }
    auto it = s_texRefresh.find(key);
    if (it == s_texRefresh.end()) return;     // never published: nothing to compare
    if (it->second.publishes >= kTexRefreshMaxPublish) { s_trVolSkip++; return; }
    if (it->second.publishing) { s_trRace++; return; }
    if (s_texRefreshWindowKeys.size() >= 100000 ||
        !s_texRefreshWindowKeys.insert(key).second) { s_trThrottled++; return; }
    wantWindow = it->second.window;
    prevDigest = it->second.digest;
    prevVer = it->second.version;
    prevBytes = it->second.bytes;
  }

  // (2) PROBE THE SAME WINDOW THE PAYLOAD WAS UPLOADED FROM. Probing the other
  // window MANUFACTURES mutations out of our unfolded alias views — the VB
  // cache learned this the hard way. Fall back to the selector only if the
  // stored window went unreadable, and COUNT the flip so a digest change with
  // flip=1 can be excluded from evidence.
  // iter-458 Q: pin BEFORE reading - a free racing this republish must see it.
  sr::kernel::physalloc::pinHistory(base, span);
  // iter-458 (Astra 029 B): SR_PHYS_RESOLVE - ownership overrides the stored
  // window when it names one (a change of window counts as a flip, exactly
  // like the unreadable fallback, so its digest change stays out of evidence).
  const int rfRes = sr::kernel::physwin::resolveOwned(
      sr::kernel::physwin::kRsRefresh, base, span);
  const uint8_t probeWindow = rfRes ? (uint8_t)rfRes : wantWindow;
  const uint32_t va = ((probeWindow == 0xA) ? 0xA0000000u : 0x80000000u) |
                      (base & 0x1FFFFFFFu);
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (p && !sr::kernel::hostRangeReadable(p, (size_t)span)) p = nullptr;
  uint8_t usedWindow = probeWindow;
  bool flipped = probeWindow != wantWindow;
  if (rfRes) {
    const uint32_t lowP = base & 0x1FFFFFFFu;
    const uint8_t* w8 = sr::kernel::guestVAToHost(0x80000000u | lowP);
    const uint8_t* wA = sr::kernel::guestVAToHost(0xA0000000u | lowP);
    if (w8 && !sr::kernel::hostRangeReadable(w8, (size_t)span)) w8 = nullptr;
    if (wA && !sr::kernel::hostRangeReadable(wA, (size_t)span)) wA = nullptr;
    const uint64_t probe = span < kAliasScanMax ? span : 64u;
    const bool selOk = resolvedBytesPlausible(p, probeWindow == 0xA ? w8 : wA, probe);
    // Astra 030: the legacy rule re-probes the STORED window and asks the
    // selector only when that window is unreadable.
    const int storedWin = wantWindow == 0xA ? 0xA : 8;
    const int lw = (storedWin == 0xA ? wA : w8) ? storedWin : legacyAliasWindow(w8, wA, span);
    const bool legOk = lw == rfRes ? selOk
                     : lw ? resolvedBytesPlausible(lw == 0xA ? wA : w8, lw == 0xA ? w8 : wA, probe)
                          : false;
    sr::kernel::physwin::resolveValidate(sr::kernel::physwin::kRsRefresh, selOk);
    sr::kernel::physwin::resolveCompare(sr::kernel::physwin::kRsRefresh, rfRes, lw, selOk, legOk);
  }
  if (!p && !rfRes) {
    // checkAmbiguity=false ALWAYS here: true costs a whole-span memcmp of both
    // windows, affordable once per key at capture and nowhere else.
    const AliasPick ap = pickAliasWindow(base, span, /*checkAmbiguity=*/false,
                                         fmt, width, height,
                                         sr::kernel::physwin::kRsSites);
    p = ap.p; usedWindow = ap.window; flipped = (ap.window != wantWindow);
  }
  if (!p) { std::lock_guard<std::mutex> lk(s_texCapMu); s_trUnreadable++; return; }

  // (3) FINGERPRINT OUTSIDE THE LOCK — one pass.
  const AtlasAliasView v = scanSpanBytes(p, span);

  // (4) COMPARE AND CLAIM.
  {
    std::lock_guard<std::mutex> lk(s_texCapMu);
    if (flipped) s_trWindowFlip++;
    auto it = s_texRefresh.find(key);
    if (it == s_texRefresh.end() || it->second.publishing) { s_trRace++; return; }
    s_trProbed++;
    if (v.hash == it->second.digest && span == it->second.bytes) {
      s_trUnchanged++; return;                       // the overwhelming case
    }
    s_trChanged++;
    if (span > sr::gfx::hle::kTexSlotBytes) { s_trTooBig++; return; }
    // ── iter-456j: WITHIN-ROLL A/B — publication gate (probe cost unchanged).
    // The digest is deliberately NOT updated here: the change stays pending so
    // the first probe at/after N publishes it. Counted, so "changes existed
    // before N" is provable rather than inferred from silence.
    if (const uint64_t fromN = texRefreshFromPresent()) {
      if (s_swap.n.load(std::memory_order_relaxed) < fromN) {
        s_trPreN++;
        return;
      }
    }
    it->second.publishing = true;                    // claimed
  }

  // (5) PUBLISH VERSION N — the same enqueue shape as the first capture.
  // ⚠ Always SAME DIMS by construction: width/height are part of the tuple key,
  // so a dims change is a different key and therefore a first capture, never a
  // refresh. That is why this can never drive the backend's resource-recreate
  // path or walk the SRV bump allocator toward exhaustion.
  const uint32_t off = sr::gfx::hle::texWrite(p, (uint32_t)span);
  std::lock_guard<std::mutex> lk(s_texCapMu);
  auto it = s_texRefresh.find(key);
  if (it != s_texRefresh.end()) it->second.publishing = false;
  if (off == sr::gfx::hle::kArenaFull) {
    // Mirrors the first capture's reject: do NOT bump the version and do NOT
    // store the new digest, so the next window re-probes and retries.
    s_trReject++;
    return;
  }
  const uint32_t newVer = prevVer + 1;
  bool zeroFill = false;
  uint64_t seedNz = 0, seedSw = 0;
  if (it != s_texRefresh.end()) {
    seedNz = it->second.seedNonzero;
    seedSw = it->second.seedSwap;
  }
  if (it != s_texRefresh.end()) {
    // iter-456h: THE ZERO->NONZERO TRANSITION, logged EXHAUSTIVELY (design
    // causality bar). This publish fired FROM a bind revisit, so one line
    // carries the whole join: zero-seeded capture -> content changed ->
    // republished -> bound (and sampled by the frames that follow).
    zeroFill = it->second.zeroSeed && v.nonzero > 0;
    if (zeroFill) { it->second.zeroSeed = false; s_trZeroFill++; }
    if (newVer == 1u) s_trFirstPub++;   // distinct keys ever republished
    it->second.digest = v.hash;
    it->second.bytes = (uint32_t)span;
    it->second.window = usedWindow;
    it->second.version = newVer;
    it->second.publishes++;
  }
  s_trPublished++;
  s_trPubBytes += span;
  if (zeroFill)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[TEXREFRESH_ZNZ] #%llu key=%08X ZERO->NONZERO v%u->v%u nz=%llu/%llu "
        "first=%lld last=%lld digest=%016llX at-bind (exhaustive)",
        (unsigned long long)s_trZeroFill, base, prevVer, newVer,
        (unsigned long long)v.nonzero, (unsigned long long)span,
        (long long)v.first, (long long)v.last, (unsigned long long)v.hash);
  // ── iter-456j slice 1: THE EXHAUSTIVE FIRST-REPUBLISH RECORD ──────────────
  // One line per KEY, at its FIRST successful republish (newVer==1), carrying
  // the complete before/after tuple the director required: both digests, both
  // nonzero counts, format/dims/pitch/tiled/endian, the alias window actually
  // read (and whether the selector flipped), the true footprint vs the padded
  // span, seed and publish present ordinals, and the publication outcome
  // (published here by construction — refusals are counted in the census's
  // rejected/tooBig/preN fields, which conserve against `changed`).
  if (newVer == 1u && texRefreshFirstEnabled())
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[TEXREFRESH_FIRST] #%llu key=%08X v0->v1 fmt=%u %ux%u pitch=%u "
        "tiled=%u endian=%u win=0x%X flip=%d span=%llu tight=%llu tail=%llu "
        "seedNz=%llu newNz=%llu seedSwap=%llu pubSwap=%llu "
        "digest=%016llX (was %016llX) outcome=published",
        (unsigned long long)s_trFirstPub, base, fmt, width, height, pitchTexels,
        tiled ? 1u : 0u, endian, usedWindow, flipped ? 1 : 0,
        (unsigned long long)span, (unsigned long long)tightBytes,
        (unsigned long long)((span > tightBytes) ? (span - tightBytes) : 0ull),
        (unsigned long long)seedNz, (unsigned long long)v.nonzero,
        (unsigned long long)seedSw,
        (unsigned long long)s_swap.n.load(std::memory_order_relaxed),
        (unsigned long long)v.hash, (unsigned long long)prevDigest);

  sr::gfx::hle::RenderCommand tc;
  tc.type = sr::gfx::hle::RenderCommandType::TextureUpload;
  tc.textureUpload.key = base;
  tc.textureUpload.arenaOffset = off;
  tc.textureUpload.bytes = (uint32_t)span;
  tc.textureUpload.dims = (width << 16) | (height & 0xFFFFu);
  tc.textureUpload.fmtBits =
      (fmt & 0xFFu) | ((tiled ? 1u : 0u) << 8) | ((endian & 3u) << 12);
  tc.textureUpload.pitch = pitchTexels;
  sr::gfx::hle::enqueue(tc);

  // `tail` = span - rows*blockPitch*blockBytes: the bytes a TILED span reads
  // past the real allocation. A change confined to offsets >= (span - tail) is
  // a neighbour's write, not this texture's — the "span overshoot" false
  // positive, now ONE subtraction to check instead of an investigation.
  // (iter-450UG claimed this was printed and it was not; corrected here.)
  const uint64_t tail = (span > tightBytes) ? (span - tightBytes) : 0ull;
  const uint64_t n = ++s_trLogN;
  // ⚠ Sampled, NOT exhaustive: first 24 then powers of two. A per-key
  // classification of every event is NOT recoverable from this log — an
  // iter-450UG claim the director correctly marked OVERSTATED. Under a key
  // filter the event count is small enough that every event is printed.
  if (n <= 24 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_TEXREFRESH] #%llu key=%08X v%u->v%u win=0x%X fmt=%u %ux%u "
        "span=%llu (was %u) tight=%llu tail=%llu nz=%llu first=%lld last=%lld "
        "digest=%016llX (was %016llX) flip=%d",
        (unsigned long long)n, base, prevVer, newVer, usedWindow, fmt, width,
        height, (unsigned long long)span, prevBytes,
        (unsigned long long)tightBytes, (unsigned long long)tail,
        (unsigned long long)v.nonzero, (long long)v.first, (long long)v.last,
        (unsigned long long)v.hash, (unsigned long long)prevDigest,
        flipped ? 1 : 0);
}

// One line carrying the raw header window + the Lock result. `tag` is LOCK or
// UNLOCK so a pair can be diffed (the dirty range is only meaningful pre-flush).
void logResModel(const char* tag, uint64_t n, uint32_t tex, uint32_t level,
                 const TexRec& r, uint32_t pitch, uint32_t bits) {
  uint32_t hdr[13] = {};
  for (int i = 0; i < 13; i++) safeU32(tex + 4u * (uint32_t)i, hdr[i]);
  char dump[80];
  safeHex(bits, 24, dump, sizeof(dump));
  // hdr[0] word0: type nibble (&0xF), lock count (&0xF00)
  // hdr[5]/hdr[6]: per-lock dirty range candidates
  // hdr[7..12]: the 24-byte texture fetch constant SetTextureFetchConstant
  //             (sub_82100310) lifts from tex+28 -> dev+1024+24*slot
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[RESMODEL] %s #%llu tex=%08X lv=%u %ux%u fmt=%08X | pitch=%u tight=%u "
      "pBits=%08X delta=%d | w0=%08X lockcnt=%u rc=%08X d5=%08X d6=%08X | "
      "fc=%08X %08X %08X %08X %08X %08X | b0=%s",
      tag, (unsigned long long)n, tex, level, r.w, r.h, r.fmt,
      pitch, r.w, bits, (int)(int64_t)((int64_t)bits - (int64_t)tex),
      hdr[0], (hdr[0] >> 8) & 0xFu, hdr[1], hdr[5], hdr[6],
      hdr[7], hdr[8], hdr[9], hdr[10], hdr[11], hdr[12], dump);
}

// iter-440c (migration Phase 2, opt-in until K-proven): block the guest inside
// Swap until the drain has consumed this frame's boundary marker — the
// reference's g_executedCommandList.wait (video.cpp:2811) and the console's own
// one-swap-in-flight throttle. Fail-open on timeout (render side stalled or
// not yet consuming) so a hook can never deadlock boot.
// iter-442 S3: BAKED DEFAULT-ON (=0 keeps the A/B) after the decisive pair.
// A/B on the same build, K=1 200s each: OFF -> present/swap 7200/7200,
// feFrames 5090, defExec 20336. ON -> identical 7200/7200 with the guest
// waiting 489us/frame on average, ZERO timeouts, feFrames 5014, defExec 20899
// (within roll noise, some metrics higher). Half a millisecond of backpressure
// per frame buys the console's own one-frame-in-flight throttle and a
// guest-driven boundary; the reference blocks here too (video.cpp:2811).
bool apiPresentBlockEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_API_PRESENT_BLOCK");
    if (e && e[0]) return e[0] != '0';
    // iter-465 perf lane: the rendezvous waits for the mirror's Present command; without the
    // mirror nothing would ever release it (every Swap would sit out the 200 ms fail-open).
    return apiMirrorEnabled();
  }();
  return s_on;
}

// iter-440c: forward the guest's own Clear/ClearF into the draw stream
// (drain executes at stream position; the invented drain-top clear stands
// down — see renderGuestGeometry). Opt-in until visually verified.
bool apiClearEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_API_CLEAR");
    return e && e[0] == '1';
  }();
  return s_on;
}

float guestF32(uint32_t va) {
  const uint32_t bits = sr::kernel::readGuestU32BE(va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void pushGuestClear(uint32_t flags, const float rgba[4], float z, uint32_t stencil) {
  sr::gfx::GuestDrawCommand cmd{};
  cmd.clearOp = true;
  cmd.clearFlags = flags;
  std::memcpy(cmd.clearColor, rgba, sizeof(cmd.clearColor));
  cmd.clearZ = z;
  cmd.clearStencil = stencil;
  if (!sr::gfx::getDrawCommandQueue().pushBlocking(cmd, 100)) {
    static std::atomic<uint32_t> s_drop{0};
    const uint32_t d = s_drop.fetch_add(1, std::memory_order_relaxed) + 1;
    if (d <= 4 || d % 1000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[API_CLEAR] push DROPPED #%u (queue full)", d);
  }
}

// ── iter-445: THE RENDER-TARGET / RESOLVE CENSUS ────────────────────────────
// SR_S6_RTCENSUS, DEFAULT OFF. Measures only; captures and changes nothing.
//
// WHY, and why NOW — ⚠ CORRECTED iter-445 C6, both halves of the original
// claim were wrong and the next session would have planned from them:
// the executor runs ELEVEN of the twenty mirrored command types, not six
// (case arms in gfx/hle_video.cpp for Present, TextureUnlock, SetRenderState,
// TextureUpload, SetTexture, SetShaders, SetVertexDecl, SetConstants and the
// three draw kinds). NINE have no implementing arm: Clear, ClearF,
// SetRenderTarget, SetDepthStencilSurface, SetViewport, SetScissorRect,
// SetStreamSource, SetIndices, EndVertices.
// ⚠ AND **Resolve IS NOT ONE OF THEM** — it is strictly MORE work than the
// other nine, because it has no RenderCommandType at all (absent from the enum
// in gfx/hle_video.hpp) and D3DDeviceResolve_sub_8210C5F8 below publishes
// NOTHING. It is blocked by a MISSING COMMAND TYPE, not by a missing handler.
// Every draw still lands on ONE backbuffer under a HOST-INVENTED depth clear —
// the "the host invents what the API states" pattern this migration exists to
// delete, which already cost iter-444 S6i a whole iteration. The S1 census measured
// setRT=308K/setDS=107K over 7,500 frames = the guest re-targets ~41x per
// frame, and NOTHING currently says what those targets are. This is the
// instrument-before-design rule that S6b and S6g-a both paid off with.
//
// THE FOUR NUMBERS IT MUST RETURN:
//  1. how many DISTINCT render targets a frame uses (how many host RTs)
//  2. their format/size (from the surface header — dumped raw, decoded offline)
//  3. resolves per frame + distinct destination textures (RT->texture traffic)
//  4. whether the frame is PREDICATED-TILED (BeginTiling/EndTiling counts) —
//     if it is, EndTiling emits a resolve PER TILE and the raw resolve rate is
//     tile-multiplied rather than pass-count.
bool rtCensusEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_S6_RTCENSUS");
    return e && e[0] != '0';
  }();
  return s_on;
}

std::mutex s_rtMu;
std::unordered_map<uint64_t, uint64_t> s_rtBinds;    // (idx<<32|surf) -> binds
std::unordered_map<uint32_t, uint64_t> s_dsBinds;    // surf -> binds
std::unordered_map<uint64_t, uint64_t> s_resolveTo;  // (srcIdx<<32|dest) -> n
std::unordered_map<uint32_t, uint64_t> s_resolveFlagHist;
uint64_t s_rtWin = 0, s_dsWin = 0, s_resWin = 0;     // per-60-frame windows
uint64_t s_tileBegin = 0, s_tileEnd = 0, s_resolveN = 0;
uint32_t s_hdrDumped = 0;
std::atomic<uint32_t> s_vpDumped{0}, s_scDumped{0};  // iter-446 R0b dump caps

// Reinterpret published float BITS. The producer publishes bits rather than a
// float so the value survives the uint32_t-based command union unrounded.
inline float bitsToFloat(uint32_t b) {
  float f;
  std::memcpy(&f, &b, sizeof f);
  return f;
}

// iter-445: DRAW -> RENDER TARGET ATTRIBUTION. The census above says the frame
// has 16 targets and ~13 resolves; this says WHICH TARGET each draw goes to and
// by which entry point. It is the measurement that orders the next two slices:
// if the era's draws are dominated by one target that only reaches the screen
// through a Resolve, the RT/resolve slice comes first; if they are dominated by
// DrawIndexedVertices, the indexed path does. RT0 surface = dev+12168 (proven
// by the Resolve prologue, §6g.4).
// ⚠ iter-445 S1: KEYED ON THE PRESENT WINDOW, NOT ON THE [S4_STAGE] GATE. That
// gate is computed from CUMULATIVE arena writes, so it latches at the first
// movie snapshot and reads MOVIE-ACTIVE for the entire remaining log — a
// histogram keyed on it yields exactly one bucket holding the whole run, which
// is precisely the era-blindness this instrument exists to remove. The window
// is the guest's own swap count / 60, read producer-side.
std::unordered_map<uint64_t, uint64_t> s_drawRt;   // (win<<40|kind<<36|surf) -> n
uint64_t s_rtCalls = 0, s_rtOverflow = 0, s_rtReadFail = 0;
constexpr size_t kDrawRtCap = 4096;
void rtAttribute(uint32_t dev, uint32_t kind) {
  if (!rtCensusEnabled()) return;
  uint32_t rt0 = 0;
  const uint64_t win = s_swap.n.load(std::memory_order_relaxed) / 60u;
  std::lock_guard<std::mutex> lk(s_rtMu);
  s_rtCalls++;
  // A read failure is NOT the same as "no target" and must never be silently
  // folded into a bucket: conservation (attributed + overflow + readFail ==
  // calls) is what makes this histogram trustworthy.
  if (!safeU32(dev + 12168u, rt0)) { s_rtReadFail++; return; }
  const uint64_t key = (win << 40) | ((uint64_t)(kind & 0xFu) << 36) | rt0;
  auto it = s_drawRt.find(key);
  if (it != s_drawRt.end()) { it->second++; return; }
  if (s_drawRt.size() >= kDrawRtCap) { s_rtOverflow++; return; }
  s_drawRt.emplace(key, 1ull);
}

// ── iter-445 S1 (J): THE UP/INLINE DRAW PROBE STATE ─────────────────────────
// SR_S6_UPPROBE, DEFAULT OFF. The hooks live further down with the other draw
// entry points; this is the state and the drain, declared here because the
// per-window report in the Swap summary above consumes them.
bool upProbeEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_S6_UPPROBE");
    return e && e[0] == '1';
  }();
  return v;
}
// iter-447 S6-UP. DEFAULT OFF: this publishes real draws with real state and so
// changes what reaches the screen. ⚠ It IMPLIES the probe — the producer reads
// the same latch the probe maintains, so arming it without upProbeEnabled()
// would silently publish nothing at all (t_up.live is only ever set under the
// probe gate). Folded in here rather than left as a footgun for a future roll.
// ⚠ COST NOTE (iter-447). Arming this together with SR_S6_TEX=1 measurably
// SLOWS a roll — present 6000 for the pair vs 7200 (TEX alone) / 8100 (UP alone)
// / 8400 (neither). Additive instrument cost on the guest thread; both gates are
// diagnostics, so pay it knowingly.
//
// ⛔⛔ AND A CORRECTION TO MY OWN COMMITTED CLAIM (8f0461f). That commit said the
// pair CRASHES, from ONE roll. IT DOES NOT REPRODUCE: the re-run is crash=0 with
// ZERO null-read AVs. A single K=1 roll is not a deterministic crash — the
// project rule says never conclude from one roll and I broke it. Three things
// were established while chasing it, all worth keeping:
//
//  1. `0x2A00000000` IS THE GUEST BASE (`kPreferredBase`,
//     guest/guest_harness.cpp:637), NOT a garbage pointer. The VEH only logs
//     dataAddr within [guestBase, guestBase+0x1000) (guest_harness.cpp:420), so
//     those were guest NULL reads at +0x80/+0x9C.
//  2. THEY ARE HARMLESS WHILE THE HARNESS LIVES. Guest memory is
//     MEM_RESERVE|MEM_COMMIT over the full 4 GB and zero-filled
//     (guest_harness.cpp:647), so a read of guest 0x80 SUCCEEDS and returns 0 —
//     a healthy roll logs spinStorm=624,000,001 of exactly this. They can only
//     FAULT after freeGuestMemory() releases the reservation with guest threads
//     still running. The "69,600 AVs" were a sub-second teardown death-rattle
//     (three host IPs cycling at ~200k/s), i.e. a PRE-EXISTING shutdown race
//     that any slow roll can expose — not a defect of this path.
//  3. REFUTED, by code and by measurement, so nobody re-chases them:
//     - the decl/stride overrun: the expander clamps TWICE
//       (gfx/d3d12_backend.cpp:2716 element-vs-srcStride -> BadFormat, :2728
//       srcCount*srcStride-vs-srcBytes -> Shape). It refuses, never over-reads.
//     - block-pool saturation: pair peak 2,461 vs 2,234 UP-alone, rejects=0
//       both, against kBlockCount=4096. The pool is not the bottleneck.
// iter-466 tick (SR_S6_FOLLOW_MIRROR, A/B gate): the S6 UP / IDX producers (and the
// publishDrawState they call) fill the HLE arena for the HLE draw path's queue only: every
// command they build goes through MIRROR1, which drops it when the mirror is off (the PM4
// draw path). Their per-draw hashing, de-indexing and arena copies were ~3% of the guest
// render thread for nothing (PERFORMANCE 2.8). Under the gate their default follows the
// mirror, as SR_S6_TEX's already does (iter-465); SR_S6_UP / SR_S6_IDX = 0 | 1 still win.
bool s6FollowMirror() {
  static const bool v = [] {
    const char* e = std::getenv("SR_S6_FOLLOW_MIRROR");
    return !e || e[0] != '0';  // iter-466 tick: baked default ON (=0 A/B)
  }();
  return v;
}
bool s6UpEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_S6_UP");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
    if (e && e[0]) return e[0] != '0';
    return s6FollowMirror() ? apiMirrorEnabled() : true;
  }();
  return v;
}
struct UpPending {
  uint32_t allocVA, prim, count, stride, dev, rt0;
  bool live;
};
thread_local UpPending t_up{};
std::mutex s_upMu;
uint64_t s_upBegins = 0, s_upNullAlloc = 0, s_upDrainEnd = 0, s_upDrainBBB0 = 0,
         s_upOverwritten = 0, s_upZeroSpan = 0, s_upUnreadable = 0,
         s_upRepeating = 0;
// iter-447: DO THE UP DRAWS DIFFER IN THEIR VERTICES AT ALL? The first 12
// samples all carried prim=13 count=4 stride=20 span=80 and the SAME hash
// across different alloc VAs — which, if it holds, means UI quads are a UNIT
// QUAD positioned by CONSTANTS, and a producer that snapshots vertices alone
// would draw every element on top of every other. That decides whether the
// slice is "snapshot 80 bytes" (cheap) or "snapshot + publish OWN state"
// (the real job). Counted, not eyeballed off 12 lines.
std::unordered_set<uint64_t> s_upHashes;
uint64_t s_upSpanMin = ~0ull, s_upSpanMax = 0;
// iter-447 S6-UP producer accounting. Every drain that the gate accepts must
// end in exactly one of these — an unattributed refusal is how 39,983 skipped
// draws once hid behind an empty reject line.
uint64_t s_upPublished = 0, s_upArenaFull = 0;
// ⛔ AND THE ONE THAT MATTERS FOR THE MOVIE GATE. `poolWrite` ROUTES BY SIZE
// (gfx/hle_video.cpp:1846-1852): <= kBlockBytes goes to the small-BLOCK pool,
// anything larger to the 1 MB ARENA. `snaps` is arenaStats().writes, so only
// the LARGE UP spans contaminate it. Subtracting all 467,518 publishes from an
// arena-only counter drove snaps to 0 and made a 739-movie-quad roll report
// NO-MOVIE — the gate broken the OTHER way, by a fix built on a wrong model of
// which pool the bytes went to. Count the arena ones specifically, by testing
// the tag poolWrite hands back.
uint64_t s_upArenaWrites = 0;
uint64_t s_upWinBegins = 0, s_upWinDrained = 0;

// Read the inline vertex bytes the caller has NOW written (it memcpys them
// after the emit returns — verified, see the hook) and answer the one design
// question: are they there at this point, or is this the wrong latch site?
// zeroSpan and repeating are the two failure shapes — nothing written yet, or
// the same 8 bytes repeating, which is exactly what reading the wrong buffer
// looked like in iter-444 S6j.
// iter-447: MIRROR1 and publishDrawState both used to live BELOW upDrain.
// The UP producer publishes from the drain, so the macro moved up here and
// the state publisher is forward-declared. Pure relocation — no expansion
// change; every existing MIRROR1 call site is textually after this point.
// ── iter-456p Step 3A: THE VERTEX-STREAM BINDING SHADOW ────────────────────
// SIXTEEN bindings, updated at the SetStreamSource hook and read at the draw
// boundary.
//
// WHY THIS STATE MUST EXIST AT ALL, given the guest already holds it: the VB
// object VA is recoverable from the device shadow (`dev[4*(3047+s)]`) and the
// stride from `dev+12256+s`, but the OFFSET is folded into the vertex fetch
// constant's base and cannot be recovered from it. Only the call carries it,
// and the reference's `size = dataSize - offset` needs it.
//
// ⚠ ATOMIC, NOT MUTEXED. This hook takes ~18K calls in 6 ms during a world
// load (the iter-449w storm) and a lock there would be a measurable tax on the
// guest thread. Per-field relaxed atomics mean a concurrent draw can in
// principle read a half-updated binding; that is acceptable HERE and nowhere
// else, because every field is independently validated at the draw (the object
// header is re-read and range-checked, the offset is checked against the
// decoded size) and a torn read refuses BY NAME rather than publishing wrong
// bytes.
struct VbBindSlot {
  std::atomic<uint32_t> objectVA{0};
  std::atomic<uint32_t> offset{0};
  std::atomic<uint32_t> stride{0};
};
VbBindSlot s_vbBind[sr::gfx::kVbMaxStreams];

// Default OFF, per the charter: new behavior arrives behind a named SR_* gate
// and is baked only after it is proven across sequential rolls.
bool vbPublishEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_VB_PUBLISH");
    return e && e[0] == '1';
  }();
  return v;
}

// One resource-state entry per key, producer-side. Bounded: a run that somehow
// produced more distinct buffers than this stops PUBLISHING rather than growing
// without limit, and says so.
constexpr size_t kVbPubMaxResources = 16384;
std::mutex s_vbPubMu;
std::unordered_map<uint64_t, sr::gfx::VbResourceState> s_vbPubState;
std::atomic<uint64_t> s_vbPubFirst{0}, s_vbPubChanged{0}, s_vbPubUnchanged{0};
std::atomic<uint64_t> s_vbPubBytes{0}, s_vbPubFull{0};
std::atomic<uint64_t> s_vbRefuse[(size_t)sr::gfx::VbRefuse::Count] = {};
std::atomic<uint64_t> s_vbAliasPrimary{0}, s_vbAliasAlt{0};
// The sparse probe's exposure, reported rather than assumed away: how often the
// cheap path served a buffer too large for the probe to see whole (a mid-buffer
// edit could hide there), and how often the probe fired on bytes the full
// digest then found identical.
std::atomic<uint64_t> s_vbProbeMissRisk{0}, s_vbProbeFalseFire{0};
// The two-alias proof, as counters. `differ` is the one that matters: it is the
// population where the alias CHOICE decided which bytes were published.
std::atomic<uint64_t> s_vbAliasAgree{0}, s_vbAliasDiffer{0}, s_vbAliasOnlyOne{0};

// ── iter-456p Step 3A: THE DRAW-BOUNDARY PUBLICATION ───────────────────────
// For each stream the ACTIVE DECLARATION references, resolve the bound buffer
// through its object header and publish the WHOLE logical buffer once per
// content generation.
//
// WHY THE WHOLE BUFFER and not `count * stride`: POSITION1 derives its count
// from the full remaining view (`indexCount = size / 4`), and an ordinary
// shader with a nonzero start offset addresses a non-linear subset. A
// draw-extent capture is correct for neither, and would make the H10 quotient
// unanswerable by construction.
//
// WHY ORIGINAL GUEST BYTE ORDER: the payload hash is the artifact's integrity
// check. Swapping first would hash our interpretation of the bytes rather than
// the bytes, and two readers that swap differently would then agree exactly
// where they differ most. The endian mode travels in the command; the consumer
// swaps.
//
// ⛔ OBSERVER + PUBLISHER ONLY. It reads guest memory (fault-free, range-checked
// before every access — the harness VEH makes a host AV fatal before any SEH
// filter runs), writes a payload into our own pool and enqueues a command. It
// writes NO guest memory, overrides NO guest decision, and calls NO guest
// function. Default OFF behind SR_HLE_VB_PUBLISH.
// ⚠ `apiPrim`/`apiCount`/`apiStart` are the DRAW's own arguments, carried in so
// the evidence row can be read without joining it to anything. H10's quotient is
// `instanceCount = apiCount / (stream1.dataSize / 4)`; a row without apiCount
// cannot answer it, which is exactly the gap the first refresh attempt left.
void vbPublishForDraw(uint32_t dev, uint32_t declStreamMask, uint32_t apiPrim,
                      uint32_t apiStart, uint32_t apiCount) {
  if (!vbPublishEnabled() || !declStreamMask) return;

  for (uint32_t stream = 0; stream < sr::gfx::kVbMaxStreams; stream++) {
    if (!(declStreamMask & (1u << stream))) continue;

    sr::gfx::VbBinding b;
    b.objectVA = s_vbBind[stream].objectVA.load(std::memory_order_relaxed);
    b.offset = s_vbBind[stream].offset.load(std::memory_order_relaxed);
    b.stride = s_vbBind[stream].stride.load(std::memory_order_relaxed);
    b.live = b.objectVA != 0;

    auto refuse = [&](sr::gfx::VbRefuse r) {
      s_vbRefuse[(size_t)r].fetch_add(1, std::memory_order_relaxed);
    };

    if (!b.live) { refuse(sr::gfx::VbRefuse::NotLive); continue; }

    // The object header. Fault-free: range-check the WHOLE pair before reading.
    if (!sr::kernel::isGuestVAInRange(b.objectVA + 0x20u)) {
      refuse(sr::gfx::VbRefuse::BadHeader);
      continue;
    }
    const uint32_t rawBase = sr::kernel::readGuestU32BE(b.objectVA + 0x18u);
    const uint32_t rawSize = sr::kernel::readGuestU32BE(b.objectVA + 0x1Cu);
    const sr::gfx::VbDesc d = sr::gfx::decodeVbHeader(rawBase, rawSize);

    const sr::gfx::VbRefuse chk =
        sr::gfx::vbCheckBinding(stream, b, d, sr::gfx::hle::kVbCacheEntryCap);
    if (chk != sr::gfx::VbRefuse::None) { refuse(chk); continue; }

    // ── THE HOT PATH ────────────────────────────────────────────────────────
    // This runs on EVERY draw. A whole-buffer alias scan plus a whole-buffer
    // digest here is ~2.25M full passes over buffers up to 16 MB in a run —
    // enough to make the recording roll unusable and so waste the one
    // authorized fixture refresh. The cheap probe comes FIRST and the expensive
    // work happens only when it says something changed.
    const uint32_t va8 = 0x80000000u | d.physBase;
    const uint32_t vaA = 0xA0000000u | d.physBase;
    const uint8_t* p8 = sr::kernel::isGuestVAInRange(va8 + d.dataSize)
                            ? sr::kernel::guestVAToHost(va8) : nullptr;
    const uint8_t* pA = sr::kernel::isGuestVAInRange(vaA + d.dataSize)
                            ? sr::kernel::guestVAToHost(vaA) : nullptr;
    if (!p8 && !pA) { refuse(sr::gfx::VbRefuse::Unreadable); continue; }

    const uint64_t key = ((uint64_t)d.physBase << 32) | (uint64_t)d.dataSize;

    // Step 1: known key + matching sparse signature on the LOCKED alias = done.
    // No scan, no digest, no allocation.
    {
      std::lock_guard<std::mutex> lk(s_vbPubMu);
      auto it = s_vbPubState.find(key);
      if (it != s_vbPubState.end() && it->second.known &&
          it->second.endian == d.endian) {
        const uint8_t* cur =
            (it->second.alias == sr::gfx::VbAlias::Primary) ? p8 : pA;
        if (cur && sr::gfx::vbSparseSigMatches(it->second.sig, cur,
                                               d.dataSize)) {
          s_vbPubUnchanged.fetch_add(1, std::memory_order_relaxed);
          if (sr::gfx::vbProbeMissRisk(d.dataSize))
            s_vbProbeMissRisk.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
      }
    }

    // Step 2: the expensive path — first sight, or the probe saw a change.
    // ⛔ The vbc3 lesson: the alias is chosen by WHOLE-BUFFER scan, once, and
    // LOCKED. A head-only probe once chose the window holding 4 MB of zeros for
    // every 0xA-written buffer and blacked out an entire era.
    const sr::gfx::VbAliasChoice ac = sr::gfx::vbChooseAlias(p8, pA, d.dataSize);
    if (ac.refuse != sr::gfx::VbRefuse::None || !ac.bytes) {
      refuse(ac.refuse != sr::gfx::VbRefuse::None ? ac.refuse
                                                  : sr::gfx::VbRefuse::Unreadable);
      continue;
    }
    if (ac.alias == sr::gfx::VbAlias::Primary)
      s_vbAliasPrimary.fetch_add(1, std::memory_order_relaxed);
    else
      s_vbAliasAlt.fetch_add(1, std::memory_order_relaxed);

    const uint64_t digest = sr::gfx::vbDigest(ac.bytes, d.dataSize);
    // ⚠ BOTH CANDIDATE DIGESTS, on the EXPENSIVE PATH ONLY (~3.5K times a run,
    // never the ~390K-draw hot path). The director's point: `c8`/`cA` booleans
    // plus the selected digest cannot prove an alias choice was right — 397
    // successful rows had content in BOTH windows, and for those the booleans
    // say nothing at all about whether the two windows AGREE. Two digests turn
    // that into a checkable statement: equal digests mean the choice could not
    // have mattered, unequal digests mean it decided the bytes and the run must
    // say which it took.
    const uint64_t digest8 = p8 ? sr::gfx::vbDigest(p8, d.dataSize) : 0;
    const uint64_t digestA = pA ? sr::gfx::vbDigest(pA, d.dataSize) : 0;
    const bool aliasesAgree = p8 && pA && digest8 == digestA;
    if (p8 && pA) {
      if (aliasesAgree) s_vbAliasAgree.fetch_add(1, std::memory_order_relaxed);
      else s_vbAliasDiffer.fetch_add(1, std::memory_order_relaxed);
    } else {
      s_vbAliasOnlyOne.fetch_add(1, std::memory_order_relaxed);
    }

    // ⚠ PEEK ONLY. Nothing is mutated until the payload is safely parked. The
    // first cut decided-and-mutated here and rolled back on a pool refusal, but
    // the rollback restored digest and generation and NOT the sparse
    // signature — so a failed publication left the NEW signature installed, the
    // next draw's cheap probe matched it, and that buffer took the unchanged
    // fast path FOREVER. 490 changed attempts against 252 successful rows on
    // the measured roll means 238 buffers were poisoned exactly that way.
    sr::gfx::VbPublishAction act;
    {
      std::lock_guard<std::mutex> lk(s_vbPubMu);
      auto it = s_vbPubState.find(key);
      if (it == s_vbPubState.end()) {
        if (s_vbPubState.size() >= kVbPubMaxResources) {
          // Bounded, and it SAYS SO. Growing without limit would trade a
          // renderer bug for an allocator one; skipping silently would make
          // coverage look complete when it is not.
          s_vbPubFull.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        it = s_vbPubState.emplace(key, sr::gfx::VbResourceState{}).first;
      }
      act = sr::gfx::vbPeekPublish(it->second, d, digest);
    }

    if (act == sr::gfx::VbPublishAction::Unchanged) {
      // The sparse probe disagreed with the full digest — i.e. the probe fired
      // on bytes that turned out identical. Harmless, but counted, because a
      // rising count here would mean the signature is being captured from a
      // different window than it is probed against.
      s_vbPubUnchanged.fetch_add(1, std::memory_order_relaxed);
      s_vbProbeFalseFire.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // ⚠ THE PUBLICATION COUNTERS BUMP AFTER THE POOL WRITE, not before. The
    // first cut bumped here and the pool then refused 416 of them, so
    // firstSight+changed counted ATTEMPTS while the log carried SUCCESSES —
    // 3531 vs 3115. The gap reconciled exactly to poolFull, but a census whose
    // terms mean different things is a census that will be misread.
    // Park the ORIGINAL bytes. poolWrite routes by size and tags the offset, so
    // the consumer knows which pool to read and release.
    const uint32_t off = sr::gfx::hle::poolWrite(ac.bytes, d.dataSize);
    if (off == sr::gfx::hle::kArenaFull) {
      // The pool refused. NOTHING TO ROLL BACK — the state was never mutated,
      // so the next draw peeks the same answer and retries cleanly. This is the
      // whole point of the peek/commit split: a partial rollback is a class of
      // bug, and not mutating until commit removes the class.
      s_vbPubFull.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // COMMITTED: digest, generation, alias and signature move together, so no
    // later probe can read a state that half-describes a publication which
    // never reached the trace.
    uint32_t generation = 0;
    {
      std::lock_guard<std::mutex> lk(s_vbPubMu);
      auto it = s_vbPubState.find(key);
      if (it == s_vbPubState.end()) {
        // The entry was created above under the same lock and is never erased,
        // so this cannot happen; refusing loudly beats publishing a generation
        // nobody recorded.
        sr::gfx::hle::poolRelease(off);
        s_vbPubFull.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      generation = sr::gfx::vbCommitPublish(
          it->second, d, digest, ac.alias,
          sr::gfx::vbCaptureSparseSig(ac.bytes, d.dataSize));
    }
    if (act == sr::gfx::VbPublishAction::FirstSight)
      s_vbPubFirst.fetch_add(1, std::memory_order_relaxed);
    else
      s_vbPubChanged.fetch_add(1, std::memory_order_relaxed);
    s_vbPubBytes.fetch_add(d.dataSize, std::memory_order_relaxed);

    sr::gfx::hle::RenderCommand c;
    c.type = sr::gfx::hle::RenderCommandType::VertexBufferPublish;
    c.vertexBufferPublish.objectVA = b.objectVA;
    c.vertexBufferPublish.key = d.physBase;
    c.vertexBufferPublish.sizeBytes = d.dataSize;
    c.vertexBufferPublish.endAliasGen =
        sr::gfx::vbPackEndAliasGen(d.endian, ac.alias, generation);
    c.vertexBufferPublish.arenaOffset = off;
    c.vertexBufferPublish.digestLo = (uint32_t)digest;
    c.vertexBufferPublish.digestHi = (uint32_t)(digest >> 32);
    sr::gfx::hle::enqueue(c);

    // ⚠ THE TARGET-FAMILY EVIDENCE ROW. The director requires, for every one of
    // the 35 target-family occurrences: raw object header dwords, decoded
    // size/endian, offset/stride, selected alias, payload size, first/last
    // nonzero, and the original-byte digest. Every one of those is on this line
    // so the artifact is auditable without re-running anything.
    uint32_t firstNz = 0xFFFFFFFFu, lastNz = 0xFFFFFFFFu;
    for (uint32_t i2 = 0; i2 < d.dataSize; i2++)
      if (ac.bytes[i2]) { firstNz = i2; break; }
    if (firstNz != 0xFFFFFFFFu)
      for (uint32_t i2 = d.dataSize; i2-- > 0; )
        if (ac.bytes[i2]) { lastNz = i2; break; }

    static std::atomic<uint64_t> s_logged{0};
    const uint64_t ln = s_logged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ln <= 4096 || ln % 5000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[VB_PUB] #%llu %s stream=%u obj=%08X raw=%08X,%08X base=%08X "
          "size=%u endian=%u offset=%u stride=%u view=%u alias=%s gen=%u "
          "payload=%u firstNz=%d lastNz=%d selDigest=%016llX c8=%d cA=%d | "
          "dig8=%016llX digA=%016llX aliasesAgree=%s | "
          "prim=%u start=%u apiCount=%u declMask=%04X",
          (unsigned long long)ln, sr::gfx::vbPublishActionName(act), stream,
          b.objectVA, rawBase, rawSize, d.physBase, d.dataSize, d.endian,
          b.offset, b.stride, sr::gfx::vbViewSize(b, d),
          ac.alias == sr::gfx::VbAlias::Primary ? "0x8" : "0xA", generation,
          d.dataSize, (int)(int32_t)firstNz, (int)(int32_t)lastNz,
          (unsigned long long)digest,
          ac.primaryHadContent ? 1 : 0, ac.altHadContent ? 1 : 0,
          (unsigned long long)digest8, (unsigned long long)digestA,
          (p8 && pA) ? (aliasesAgree ? "yes" : "NO") : "onlyOneMapped",
          apiPrim, apiStart, apiCount, declStreamMask);
  }
}

#define MIRROR1(CMDTYPE, ASSIGN)                                  \
  do {                                                            \
    if (apiMirrorEnabled()) {                                     \
      sr::gfx::hle::RenderCommand c;                              \
      c.type = sr::gfx::hle::RenderCommandType::CMDTYPE;          \
      c.ASSIGN;                                                   \
      sr::gfx::hle::enqueue(c);                                   \
    }                                                             \
  } while (0)

void upDrain(const char* where) {
  // iter-447: the PRODUCER gate implies the latch, so SR_S6_UP=1 alone works.
  if ((!upProbeEnabled() && !s6UpEnabled()) || !t_up.live) return;
  const uint64_t span = (uint64_t)t_up.stride * (uint64_t)t_up.count;
  uint64_t h = 1469598103934665603ull;
  bool allZero = true, repeating = true, readable = false;
  if (t_up.allocVA >= 0x1000u && span && span <= (1u << 20) &&
      sr::kernel::isGuestVAInRange(t_up.allocVA + (uint32_t)span)) {
    if (const uint8_t* p = sr::kernel::guestVAToHost(t_up.allocVA)) {
      readable = true;
      const uint32_t nb = (uint32_t)(span < 4096 ? span : 4096);
      for (uint32_t i = 0; i < nb; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
        if (p[i]) allZero = false;
        if (i >= 8 && p[i] != p[i - 8]) repeating = false;
      }
      if (nb < 16) repeating = false;
    }
  }
  std::lock_guard<std::mutex> lk(s_upMu);
  if (!readable) s_upUnreadable++;
  else {
    if (allZero) s_upZeroSpan++;
    if (repeating) s_upRepeating++;
    if (s_upHashes.size() < 100000) s_upHashes.insert(h);
    if (span < s_upSpanMin) s_upSpanMin = span;
    if (span > s_upSpanMax) s_upSpanMax = span;
  }
  s_upWinDrained++;
  if (where[0] == 'E') s_upDrainEnd++; else s_upDrainBBB0++;
  // ── iter-447 S6-UP: THE PRODUCER ──────────────────────────────────────────
  // SR_S6_UP, DEFAULT OFF. Everything above this line is the measure-only probe
  // and stays byte-for-byte unchanged; this is the arm that actually publishes.
  //
  // WHY HERE AND NOT AT BeginVertices: sub_820FB6E8 returns the ring VA and the
  // caller memcpys the vertices in AFTERWARDS, so a Begin-time snapshot would
  // capture the PREVIOUS contents — a fresh instance of the iter-444 S6j
  // wrong-buffer bug. The probe proved this site sound: conservation exact
  // (drainEnd+drainBBB0 == begins), overwritten=0, pending=0, and
  // zero/repeating/unreadable all 0 across 449,602 drains.
  if (s6UpEnabled() && readable && span) {
    // The 1 MB-slot POOL, not the small-block pool: measured span runs to
    // 207,360 B, so the 4 KB kMaxVertSnapshot class cannot hold the large end.
    if (const uint8_t* src = sr::kernel::guestVAToHost(t_up.allocVA)) {
      const uint32_t va = sr::gfx::hle::poolWrite(src, (uint32_t)span);
      if (va == sr::gfx::hle::kArenaFull) {
        s_upArenaFull++;
      } else {
        // ── iter-450k THE UP ENDIAN FIX (SR_S6_UPSWAP, default ON) ────────
        // This producer published the guest's BYTES RAW since iter-447 —
        // no endian pass anywhere on the UP path, while DrawVertices swaps
        // on fetch endian 2 and the indexed gather swaps its host copy.
        // Guest-CPU-written UP vertices are BIG-ENDIAN floats; unswapped
        // they decode as denormals (±256.0 reads as ~4.6e-41) and every UP
        // quad rasterizes DEGENERATE — proven by [PSDRAW] vertex dumps of
        // the exposure-measure quad and the all-zero post-chain surfaces
        // ([WB_PROBE]/surf dumps). Same 8in32 treatment as the DrawVertices
        // endian-2 arm: swap the ARENA copy, never guest memory.
        static const bool s_upSwap = [] {
          const char* e = std::getenv("SR_S6_UPSWAP");
          return !e || e[0] != '0';
        }();
        if (s_upSwap) {
          if (uint8_t* dst = const_cast<uint8_t*>(sr::gfx::hle::poolRead(va))) {
            for (uint32_t i = 0; i + 4 <= (uint32_t)span; i += 4) {
              std::swap(dst[i + 0], dst[i + 3]);
              std::swap(dst[i + 1], dst[i + 2]);
            }
          }
        }
        // STATE FIRST, then the draw — the publish-order contract. Without
        // this the UP draw would inherit the last DrawVertices' decl/
        // constants/registers/shaders, i.e. the movie quad's in the FE era.
        publishDrawState(t_up.dev);
        s_upPublished++;
        // NO tag bit set => it went to the 1 MB arena, which is what `snaps`
        // counts. iter-449c: geo-tagged (bit30) writes are NOT arena writes —
        // testing kPoolBlockFlag alone would subtract them from snaps and
        // break the movie gate a third time.
        if (!(va & (sr::gfx::hle::kPoolBlockFlag | sr::gfx::hle::kPoolGeoFlag)))
          s_upArenaWrites++;
        MIRROR1(DrawVerticesUP,
                draw.device = t_up.dev;
                c.draw.primType = t_up.prim;
                // Honestly named, unlike the shared macro: the snapshot IS the
                // whole stream, so the first vertex is index 0.
                c.draw.start = 0u;
                c.draw.count = t_up.count;
                c.draw.vertArena = va;
                c.draw.vertBytes = (uint32_t)span;
                c.draw.stride = t_up.stride);
      }
    }
  }
  static uint64_t s_shown = 0;
  if (++s_shown <= 12)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_UP] drain@%s #%llu alloc=%08X prim=%u count=%u stride=%u span=%llu"
        " rt0=%08X | readable=%d zero=%d repeating=%d hash=%016llX",
        where, (unsigned long long)s_shown, t_up.allocVA, t_up.prim, t_up.count,
        t_up.stride, (unsigned long long)span, t_up.rt0, readable ? 1 : 0,
        readable && allZero ? 1 : 0, readable && repeating ? 1 : 0,
        (unsigned long long)h);
  t_up.live = false;
}

// ── iter-449: THE INDEXED PRODUCER'S GATE + ACCOUNTING ──────────────────────
// SR_S6_IDX, DEFAULT OFF. The producer itself lives beside the sub_820FC000
// hook; the counters live here because the frame reporter (textually above the
// hook) prints them. Every gate-on, mirror-on hook call ends in EXACTLY ONE of
// {published, one refusal counter} — seen == published + Σrefusals is the
// conservation the [S6_IDX] line lets a reader check, because an unattributed
// refusal is how 39,983 skipped draws once hid behind an empty reject line.
bool s6IdxEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_S6_IDX");
  // iter-449o WAVE-2 BAKE: default-ON after wave 1 + two clean soak rolls
  // (movie gate exact, rejects=0 on all three). `=0` keeps the A/B.
  // iter-466 tick: follows the mirror under SR_S6_FOLLOW_MIRROR (see s6UpEnabled).
    if (e && e[0]) return e[0] != '0';
    return s6FollowMirror() ? apiMirrorEnabled() : true;
  }();
  return v;
}
std::atomic<uint64_t> s_idxSeen{0}, s_idxPublished{0}, s_idxZeroCount{0},
    s_idxNoDecl{0}, s_idxSlotMiss{0}, s_idxIbBad{0}, s_idxEndianOdd{0},
    s_idxRangeBad{0}, s_idxOverCap{0}, s_idxPoolFull{0},
    s_idxFmt32{0}, s_idxMaxCount{0},
    // iter-450i: draws rescued by the 0xA-window retry (would have refused
    // as "range" under the 0x8-window misread) + VBs read through the
    // coupled window. Informational — NOT part of the conservation sum.
    s_idxWinFixed{0}, s_idxVbFromA{0},
    // iter-449d: restart-strips converted to triangle lists at gather (the
    // former `cut` refusal class — now served for strips, and for LISTS the
    // all-ones value is a REAL INDEX, settled by the convRem kill switch;
    // see the model note in idxGatherPublish). `ambig` counts the one silent
    // case: a >=65536-vertex strip whose 0xFFFF could be either reading.
    s_idxStripConv{0}, s_idxCutAmbig{0};
// ⛔ THE MOVIE-GATE TERM, third producer to learn it (movie planes, then UP,
// now this). `snaps` is arenaStats().writes and gathered spans > kBlockBytes
// route to the SAME 1 MB arena, so raw writes stop meaning "plane uploads" the
// moment this gate arms. Counted by testing the tag poolWrite hands back,
// exactly like s_upArenaWrites — and subtracted beside it in [S4_STAGE].
std::atomic<uint64_t> s_idxArenaWrites{0};

// ── iter-449f: THE RANGE-CLASS RECHECK LEDGER ───────────────────────────────
// The refused text/UI draws read ASCII where indices should be. Two live
// readings: (a) ring-timing — the FE writes the real indices AFTER the emit
// returns (the UP lesson), so a Swap-time re-read shows VALID indices and a
// deferred latch wins; (b) the draws are stale on console too (credits
// screens rendered before their data exists) and invisible — the re-read
// still shows strings and the refusal is FINAL. The last 4 refusals are
// re-read at Swap and both snapshots logged side by side.
struct IdxRecheck {
  uint32_t dev, ibVA, count, prim;
  uint64_t vbSizeAtEmit;
  uint32_t slotVA;          // the stream's vertex fetch constant VA
  uint16_t emitWords[8];    // first index words AT EMIT
  bool live;
};
std::mutex s_idxRecheckMu;
IdxRecheck s_idxRecheck[4] = {};
uint32_t s_idxRecheckNext = 0;
uint64_t s_idxRecheckLogged = 0;

// Read the three header dwords the render target subsystem needs. Fault-free;
// any field that will not read comes back 0, which `decodeSurface` reports as
// invalid rather than as a plausible-but-wrong size.
//   +24 RB_SURFACE_INFO   +28 RB_COLOR_INFO   +44 byte size
// Contract and proof: gfx/hle_video.hpp `SurfaceDesc`, ENGINE_MAP §6g.9.
void readSurfaceHeader(uint32_t surf, uint32_t& surfInfo, uint32_t& colorInfo,
                       uint32_t& byteSize, uint32_t& sizeWord) {
  surfInfo = colorInfo = byteSize = sizeWord = 0;
  if (surf < 0x1000u) return;
  // ⛔ REFUSE A TEXTURE-LEVEL SURFACE. D3DSurface_GetDesc_sub_82100190 branches
  // on flags bit 0x40000000 FIRST: when it is set, +24 is a PARENT TEXTURE
  // POINTER and +28 is level<<28, so reading them as RB_SURFACE_INFO /
  // RB_COLOR_INFO decodes a pointer as a pitch. Refused HERE, producer-side, so
  // the condition never has to cross the queue.
  uint32_t flags = 0;
  safeU32(surf + 0u, flags);
  if (flags & 0x40000000u) return;
  safeU32(surf + 24u, surfInfo);
  safeU32(surf + 28u, colorInfo);
  safeU32(surf + 44u, byteSize);
  // THE TRUE EXTENT — the field §6g.9 wrongly said did not exist.
  // ⚠ ZERO IS A LEGAL VALUE: a 1x1 surface encodes (w-1)<<18 | (h-1)<<3 == 0,
  // and this game HAS one — the last step of the 64x64 -> 16x16 -> 4x4 -> 1x1
  // auto-exposure luminance chain. Inferring "unreadable" from a zero VALUE
  // therefore refuses a real surface. Unreadability gets its own sentinel:
  // 0xFFFFFFFF decodes to 16384x32768, which no real surface can be.
  if (!safeU32(surf + 36u, sizeWord)) sizeWord = 0xFFFFFFFFu;
}

// iter-446 R0: dump the header RAW **and** decoded, side by side, for the first
// distinct surfaces. Raw so a wrong decode cannot masquerade as a wrong
// measurement; decoded so the run itself grades the hypothesis instead of
// deferring it to an offline read that may never happen. Widened 13 -> 16
// dwords: if a height IS stored somewhere we have not looked, a literal 720
// beside a 1280 pitch will say so.
void dumpSurfaceHeader(const char* what, uint32_t idx, uint32_t surf) {
  if (s_hdrDumped >= 16u) return;
  s_hdrDumped++;
  uint32_t hdr[16] = {};
  for (int i = 0; i < 16; i++) safeU32(surf + 4u * (uint32_t)i, hdr[i]);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[S6_RTHDR] %s idx=%u surf=%08X | w0=%08X w1=%08X w2=%08X w3=%08X "
      "w4=%08X w5=%08X w6=%08X w7=%08X w8=%08X w9=%08X w10=%08X w11=%08X "
      "w12=%08X w13=%08X w14=%08X w15=%08X",
      what, idx, surf, hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6],
      hdr[7], hdr[8], hdr[9], hdr[10], hdr[11], hdr[12], hdr[13], hdr[14],
      hdr[15]);
  const bool isDepth = (what[0] == 'D');
  const sr::gfx::hle::SurfaceDesc d =
      sr::gfx::hle::decodeSurface(hdr[6], hdr[7], hdr[11], hdr[9], isDepth);
  // `padded=` is printed beside the true extent precisely so the EDRAM padding
  // is visible rather than mistaken for the size (§6g.12).
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[S6_RTDESC] %s idx=%u surf=%08X | TRUE %ux%u | padded %ux%u | pitch=%u "
      "msaa=%u samples=%u fmt=%u bpp=%u edram=0x%X bytes=0x%X flags=%08X valid=%d",
      what, idx, surf, d.width, d.height, d.pitch, d.paddedRows, d.pitch,
      d.msaa, d.samples, d.format, d.bpp, d.edramBase, d.bytes, hdr[0],
      d.valid ? 1 : 0);
}

}  // namespace

#define API_OBS_PROLOGUE(counter)                       \
  const uint64_t n = (counter).bump();                  \
  const bool loud = apiObserveEnabled() && (n <= 16 || n % 2000 == 0);
#endif

// iter-442 S2: publish one typed RenderCommand. `ASSIGN` is written without the
// leading `c.` for the first field (e.g. `setTexture.device = ctx.r3.u32`) and
// with it for any further fields, separated by `;` — never `,`, which would
// split the macro argument.

// --- NATIVE B2 emitter: D3DDeviceEmitPm4Packet (SET_CONSTANT the caller fills) ----
// The override of sub_82113010 lives in kernel/native_flush.cpp since P2-B item 5 (2026-10-10,
// SR_NATIVE_SETCONSTMEM, default ON since 2026-10-14, =0 the guest body: the packet as a kRkSetConstMem record);
// it still calls the native device's record-only observer (gfx/native_device onConstantPacket, SR_NATIVE_RECORD)
// on either path.

// --- Present: D3DDeviceSwap --------------------------------------------------
extern "C" void __imp__sub_82112050(PPCContext& __restrict ctx, uint8_t* base);
void sub_82112050(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x82112050);  // NATIVE N0 (scripts/gen_native_brackets.py)
#if SR_ORACLE
  API_OBS_PROLOGUE(s_swap);
  const uint32_t dev = ctx.r3.u32, front = ctx.r4.u32, rect = ctx.r5.u32;
  // ── iter-449o bake-audit item: THE EXE STATES ITS OWN GATE SET ────────────
  // Archived logs could not be attributed to a gate configuration (the runner
  // prints env to stdout, not the log), so per-roll attribution was narrative.
  // One line, once, at the first Swap: every render-stack gate and its state.
  {
    static std::atomic<bool> s_gatesLogged{false};
    bool expect = false;
    if (s_gatesLogged.compare_exchange_strong(expect, true)) {
      // iter-449o wave 2: print EFFECTIVE state, not the raw env — after the
      // bake, unset means ON for the baked set, and a line full of "-" told a
      // reader nothing (the wave-1 audit tripped on exactly that). Polarity
      // table mirrors the gate definitions; VERTCAP/TRACE_REC are values.
      struct G { const char* name; bool defaultOn; };
      static const G kGates[] = {
          {"SR_S6_WIDE", true}, {"SR_S6_STATE", true}, {"SR_S6_TEX", true},
          {"SR_S6_UP", true}, {"SR_S6_IDX", true}, {"SR_S6_VBCACHE", false},
          {"SR_HLE_RT_POOL", true}, {"SR_HLE_DRAW_TO_POOL", true},
          {"SR_HLE_DRAW_INORDER", true}, {"SR_HLE_RESOLVE", true},
          {"SR_HLE_RESOLVE_DEPTH", true}, {"SR_HLE_PRESENT_FRONT", true}};
      char l[440]; size_t o = 0;
      for (const G& g : kGates) {
        const char* v = std::getenv(g.name);
        bool eff = g.defaultOn ? (!v || v[0] != '0') : (v && v[0] == '1');
        // iter-466 tick: TEX (iter-465) and UP / IDX (SR_S6_FOLLOW_MIRROR) can follow the
        // mirror: print what the gate functions decided, not the bake polarity.
        if (!std::strcmp(g.name, "SR_S6_TEX")) eff = texCaptureEnabled();
        else if (!std::strcmp(g.name, "SR_S6_UP")) eff = s6UpEnabled();
        else if (!std::strcmp(g.name, "SR_S6_IDX")) eff = s6IdxEnabled();
        if (o + 44 < sizeof(l))
          o += (size_t)snprintf(l + o, sizeof(l) - o, " %s=%s", g.name + 3,
                                eff ? "ON" : "off");
      }
      const char* vc = std::getenv("SR_S6_VERTCAP");
      const char* tr = std::getenv("SR_HLE_TRACE_REC");
      if (o + 60 < sizeof(l))
        o += (size_t)snprintf(l + o, sizeof(l) - o, " VERTCAP=%s TRACE=%s",
                              vc && *vc ? vc : "-", tr && *tr ? "set" : "-");
      SR_LOG_BOOT(sr::LogSeverity::Info, "[GATES]%s", o ? l : " -");
    }
  }
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] Swap #%llu dev=%08X front=%08X rect=%08X",
        (unsigned long long)n, dev, front, rect);
  }
  if (apiObserveEnabled() && n % 60 == 0) {
    const unsigned long long sw = (unsigned long long)s_swap.n.load();
    const unsigned long long dv = (unsigned long long)s_drawV.n.load();
    const unsigned long long div_ = (unsigned long long)s_drawIV.n.load();
    const unsigned long long dup = (unsigned long long)s_drawUP.n.load();
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] SUMMARY swap=%llu clear=%llu clearF=%llu setRT=%llu "
        "setDS=%llu setStream=%llu setIdx=%llu texCreate=%llu texLock=%llu "
        "texUnlock=%llu",
        sw, (unsigned long long)s_clear.n.load(),
        (unsigned long long)s_clearF.n.load(), (unsigned long long)s_setRT.n.load(),
        (unsigned long long)s_setDS.n.load(),
        (unsigned long long)s_setStream.n.load(),
        (unsigned long long)s_setIdx.n.load(),
        (unsigned long long)s_texCreate.n.load(),
        (unsigned long long)s_texLock.n.load(),
        (unsigned long long)s_texUnlock.n.load());
    // iter-442 S1: the numbers that size Phase 2 — commands per frame decides
    // the queue shape; the draw split decides which emits must be hooked.
    SR_LOG_BOOT(sr::LogSeverity::Info,
        // ⚠ iter-447: setVS/setPS are DEAD COUNTERS — their hooks are owned by
        // cs_timeout_underflow_fix.cpp and never bump these, so they read 0 by
        // construction. Labelled in the output rather than printed bare: a bare
        // `setVS=0` is indistinguishable from a measurement, and `vbCreate=0`
        // (same defect, now fixed) was quoted as evidence for a whole session.
        "[API_OBS] SUMMARY2 drawV=%llu drawIV=%llu drawUP=%llu endV=%llu "
        "setTex=%llu setVp=%llu setSc=%llu setVS=%llu(unhooked) "
        "setPS=%llu(unhooked) vbCreate=%llu "
        "ibCreate=%llu | draws/frame=%.1f",
        dv, div_, dup, (unsigned long long)s_endV.n.load(),
        (unsigned long long)s_setTex.n.load(),
        (unsigned long long)s_setVp.n.load(),
        (unsigned long long)s_setSc.n.load(),
        (unsigned long long)s_setVS.n.load(),
        (unsigned long long)s_setPS.n.load(),
        (unsigned long long)s_vbCreate.n.load(),
        (unsigned long long)s_ibCreate.n.load(),
        sw ? double(dv + div_ + dup) / double(sw) : 0.0);
    // iter-456p Step 3A: the publication census. ⚠ EVERY TERM IS A REAL
    // COUNTER with an increment site — no `vbCreate=0` repeats here. When the
    // gate is OFF every term reads 0 BY CONSTRUCTION, so the line is labelled
    // with the gate state rather than left to look like a measurement.
    if (vbPublishEnabled()) {
      uint64_t refuseTotal = 0;
      char rb[256]; int ro = 0;
      for (size_t i = 1; i < (size_t)sr::gfx::VbRefuse::Count; i++) {
        const uint64_t v = s_vbRefuse[i].load(std::memory_order_relaxed);
        refuseTotal += v;
        if (v && ro < 200)
          ro += snprintf(rb + ro, sizeof(rb) - ro, " %s=%llu",
                         sr::gfx::vbRefuseName((sr::gfx::VbRefuse)i),
                         (unsigned long long)v);
      }
      if (!ro) snprintf(rb, sizeof(rb), " none");
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[VB_PUB] SUMMARY firstSight=%llu changed=%llu unchanged=%llu "
          "bytes=%llu poolFull=%llu alias8=%llu aliasA=%llu "
          "probeMissRisk=%llu probeFalseFire=%llu "
          "aliasAgree=%llu aliasDIFFER=%llu aliasOnlyOne=%llu | refusals=%llu:%s",
          (unsigned long long)s_vbPubFirst.load(),
          (unsigned long long)s_vbPubChanged.load(),
          (unsigned long long)s_vbPubUnchanged.load(),
          (unsigned long long)s_vbPubBytes.load(),
          (unsigned long long)s_vbPubFull.load(),
          (unsigned long long)s_vbAliasPrimary.load(),
          (unsigned long long)s_vbAliasAlt.load(),
          (unsigned long long)s_vbProbeMissRisk.load(),
          (unsigned long long)s_vbProbeFalseFire.load(),
          (unsigned long long)s_vbAliasAgree.load(),
          (unsigned long long)s_vbAliasDiffer.load(),
          (unsigned long long)s_vbAliasOnlyOne.load(),
          (unsigned long long)refuseTotal, rb);
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[VB_PUB] SUMMARY gate SR_HLE_VB_PUBLISH is OFF - every term would "
          "read 0 by construction, so none are printed");
    }
    // iter-442 S2 ACCEPTANCE TEST: the mirror is a pure refactor, so every
    // per-type count on the HLE side must EQUAL its [API_OBS] counter. Any
    // drift means a command is being dropped or double-published. Printed
    // side by side so one grep settles it.
    using RCT = sr::gfx::hle::RenderCommandType;
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[HLE_MIRROR] enq=%llu drained=%llu depth=%u | present=%llu/%llu "
        "clear=%llu/%llu clearF=%llu/%llu setRT=%llu/%llu setDS=%llu/%llu "
        "setStream=%llu/%llu setIdx=%llu/%llu setTex=%llu/%llu "
        "drawV=%llu/%llu drawIV=%llu/%llu drawUP=%llu/%llu endV=%llu/%llu "
        "texUnlock=%llu/%llu  (hle/api)",
        (unsigned long long)sr::gfx::hle::enqueuedTotal(),
        (unsigned long long)sr::gfx::hle::drainedTotal(),
        sr::gfx::hle::queueDepth(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::Present), sw,
        (unsigned long long)sr::gfx::hle::typeCount(RCT::Clear),
        (unsigned long long)s_clear.n.load(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::ClearF),
        (unsigned long long)s_clearF.n.load(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::SetRenderTarget),
        (unsigned long long)s_setRT.n.load(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::SetDepthStencilSurface),
        (unsigned long long)s_setDS.n.load(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::SetStreamSource),
        (unsigned long long)s_setStream.n.load(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::SetIndices),
        (unsigned long long)s_setIdx.n.load(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::SetTexture),
        (unsigned long long)s_setTex.n.load(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::DrawVertices), dv,
        (unsigned long long)sr::gfx::hle::typeCount(RCT::DrawIndexedVertices), div_,
        (unsigned long long)sr::gfx::hle::typeCount(RCT::BeginVertices), dup,
        (unsigned long long)sr::gfx::hle::typeCount(RCT::EndVertices),
        (unsigned long long)s_endV.n.load(),
        (unsigned long long)sr::gfx::hle::typeCount(RCT::TextureUnlock),
        (unsigned long long)s_texUnlock.n.load());
    // iter-443 S6b: the sizing number. Averaged over the 60 swaps since the
    // last report, so it is per-FRAME residency demand, not a run total.
    {
      std::lock_guard<std::mutex> lk(s_uniqMu);
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_UNIQ] over last 60 frames: distinctVBs=%zu uniqueBytes=%llu "
          "(%.2f MB) => per frame ~%.1f buffers, ~%.2f MB",
          s_uniqKeys.size(), (unsigned long long)s_uniqBytes,
          double(s_uniqBytes) / 1048576.0, double(s_uniqKeys.size()) / 60.0,
          double(s_uniqBytes) / 1048576.0 / 60.0);
      s_uniqKeys.clear();
      s_uniqBytes = 0;
    }
    // iter-447 [S6_VBID]: the decisive line for the VB/IB design. If `hitHdr`
    // and `hitPtr` are both 0 with `creates` > 0, the drawn geometry does NOT
    // come from D3D9's buffer API and the reference's per-buffer model has no
    // purchase on our vertex path — the same answer PROGRESS.md:1047-1053
    // already recorded for INDEX buffers. Examples are printed from BOTH sides
    // so a miss can be read (address ranges), not just counted.
    {
      std::lock_guard<std::mutex> lk(s_vbIdMu);
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_VBID] creates: hdrVAs=%zu ptrsInHdr=%zu | drawn distinct tested="
          "%llu hitHdr=%llu hitPtr=%llu MISS=%llu | eg hdr=%08X,%08X drawn=%08X,%08X",
          s_vbCreatedHdr.size(), s_vbCreatedPtr.size(),
          (unsigned long long)s_vbIdTested, (unsigned long long)s_vbIdHitHdr,
          (unsigned long long)s_vbIdHitPtr, (unsigned long long)s_vbIdMiss,
          s_vbIdEgHdr[0], s_vbIdEgHdr[1], s_vbIdEgDrawn[0], s_vbIdEgDrawn[1]);
    }
    // ── iter-445: the RT/resolve census ───────────────────────────────────
    // Window counts are per-60-frames (the rate that sizes the design);
    // the distinct maps are cumulative (the inventory that sizes the pool).
    if (rtCensusEnabled()) {
      std::lock_guard<std::mutex> lk(s_rtMu);
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_RT] last 60 frames: rtBinds=%llu (%.1f/frame) dsBinds=%llu "
          "(%.1f/frame) resolves=%llu (%.1f/frame) | CUMULATIVE distinct: "
          "rt(idx,surf)=%zu ds=%zu resolveDests=%zu flagKinds=%zu | tiling "
          "begin=%llu end=%llu",
          (unsigned long long)s_rtWin, double(s_rtWin) / 60.0,
          (unsigned long long)s_dsWin, double(s_dsWin) / 60.0,
          (unsigned long long)s_resWin, double(s_resWin) / 60.0,
          s_rtBinds.size(), s_dsBinds.size(), s_resolveTo.size(),
          s_resolveFlagHist.size(), (unsigned long long)s_tileBegin,
          (unsigned long long)s_tileEnd);
      // The inventory itself, capped — "how many distinct RTs" is the number
      // that decides the design, and the surfaces name themselves here.
      {
        char l[420]; int o = 0; uint32_t k = 0;
        for (const auto& kv : s_rtBinds) {
          if (k++ >= 10 || o > 360) break;
          o += snprintf(l + o, sizeof(l) - o, " [%u]%08X:%llu",
                        (uint32_t)(kv.first >> 32), (uint32_t)kv.first,
                        (unsigned long long)kv.second);
        }
        if (o) SR_LOG_BOOT(sr::LogSeverity::Info, "[S6_RTLIST]%s", l);
      }
      {
        char l[420]; int o = 0; uint32_t k = 0;
        for (const auto& kv : s_resolveFlagHist) {
          if (k++ >= 12 || o > 360) break;
          o += snprintf(l + o, sizeof(l) - o, " %08X:%llu", kv.first,
                        (unsigned long long)kv.second);
        }
        if (o) SR_LOG_BOOT(sr::LogSeverity::Info, "[S6_RESFLAGS]%s", l);
      }
      // WHICH TARGET the draws actually go to, by entry point.
      // ⚠ The first cut printed the first 12 entries of an unordered_map, which
      // is an ARBITRARY subset — it showed only BeginVertices and read as "UP
      // draws dominate". Rank by count and report per-kind totals, so the line
      // says what it appears to say.
      // iter-445 S1: report THIS WINDOW ONLY (the previous 60 swaps). That is
      // the branch number for the whole plan: in the front-end era, do the UP
      // draws target the primary surface or an offscreen one?
      {
        const uint64_t win = (n / 60u) - 1u;   // the window that just closed
        std::vector<std::pair<uint64_t, uint64_t>> v;
        for (const auto& kv : s_drawRt)
          if ((kv.first >> 40) == win) v.push_back(kv);
        std::sort(v.begin(), v.end(),
                  [](const std::pair<uint64_t, uint64_t>& a,
                     const std::pair<uint64_t, uint64_t>& b) {
                    return a.second > b.second;
                  });
        uint64_t byKind[3] = {0, 0, 0};
        for (const auto& kv : v) {
          const uint32_t k = (uint32_t)((kv.first >> 36) & 0xFu);
          if (k >= 10u && k <= 12u) byKind[k - 10u] += kv.second;
        }
        char l[460]; int o = 0;
        for (size_t i = 0; i < v.size() && i < 10 && o < 380; i++)
          o += snprintf(l + o, sizeof(l) - o, " %s@%08X:%llu",
                        sr::gfx::hle::renderCommandName(
                            (sr::gfx::hle::RenderCommandType)(uint32_t)((v[i].first >> 36) & 0xFu)),
                        (uint32_t)v[i].first, (unsigned long long)v[i].second);
        // CONSERVATION: attributed + overflow + readFail == calls, all three
        // counted on the same (producer) thread. A histogram that silently
        // drops keys past a cap is a histogram that lies about its own shape.
        uint64_t attributed = 0;
        for (const auto& kv : s_drawRt) attributed += kv.second;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_DRAWRT] win=%llu(swaps %llu-%llu) drawV=%llu drawIV=%llu "
            "drawUP=%llu targets=%zu | calls=%llu attributed=%llu overflow=%llu "
            "readFail=%llu keys=%zu | top:%s",
            (unsigned long long)win, (unsigned long long)(win * 60),
            (unsigned long long)(win * 60 + 59),
            (unsigned long long)byKind[0], (unsigned long long)byKind[1],
            (unsigned long long)byKind[2], v.size(),
            (unsigned long long)s_rtCalls, (unsigned long long)attributed,
            (unsigned long long)s_rtOverflow, (unsigned long long)s_rtReadFail,
            s_drawRt.size(), o ? l : " -");
      }
      s_rtWin = s_dsWin = s_resWin = 0;
    }
    // ── iter-445 S1 (J): the UP/inline drain accounting ───────────────────
    // CONSERVATION: begins == nullAlloc + drained(End) + drained(BBB0) +
    // overwritten + stillPending. A large zeroSpan or repeating fraction KILLS
    // the End-latch design before the next slice is written on top of it.
    // iter-447: the PRODUCER's own accounting lives on this line, so it must
    // print when the producer is on — not only under the probe gate. Without
    // this, an SR_S6_UP roll publishes draws and reports nothing about how many
    // or why the rest were refused, which is the unattributed-refusal shape.
    if (upProbeEnabled() || s6UpEnabled()) {
      std::lock_guard<std::mutex> lk(s_upMu);
      const uint64_t accounted =
          s_upNullAlloc + s_upDrainEnd + s_upDrainBBB0 + s_upOverwritten;
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_UP] begins=%llu (win +%llu) nullAlloc=%llu drainEnd=%llu "
          "drainBBB0=%llu overwritten=%llu pending=%lld | drained win +%llu | "
          "zeroSpan=%llu repeating=%llu unreadable=%llu | distinctVertHash=%zu "
          "span=%llu..%llu | published=%llu arenaFull=%llu",
          (unsigned long long)s_upBegins, (unsigned long long)s_upWinBegins,
          (unsigned long long)s_upNullAlloc, (unsigned long long)s_upDrainEnd,
          (unsigned long long)s_upDrainBBB0,
          (unsigned long long)s_upOverwritten,
          (long long)((int64_t)s_upBegins - (int64_t)accounted),
          (unsigned long long)s_upWinDrained, (unsigned long long)s_upZeroSpan,
          (unsigned long long)s_upRepeating, (unsigned long long)s_upUnreadable,
          // iter-447: distinctVertHash ~1 against hundreds of thousands of
          // drains would mean the UI is a UNIT QUAD placed by CONSTANTS, so a
          // vertex-only producer would stack every element at one position.
          s_upHashes.size(),
          (unsigned long long)(s_upSpanMin == ~0ull ? 0 : s_upSpanMin),
          (unsigned long long)s_upSpanMax,
          (unsigned long long)s_upPublished,
          (unsigned long long)s_upArenaFull);
      s_upWinBegins = 0;
      s_upWinDrained = 0;
    }
    // ── iter-449: the indexed producer's conservation line ────────────────
    // seen == published + Σrefusals, by construction; a reader can check it
    // from this one line. `arenaSnaps` is the movie-gate term ([S4_STAGE]).
    if (s6IdxEnabled()) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_IDX] seen=%llu published=%llu conv=%llu ambig=%llu arenaSnaps=%llu | refused: "
          "zero=%llu decl=%llu slot=%llu ib=%llu endian=%llu "
          "range=%llu overCap=%llu poolFull=%llu | fmt32=%llu maxCount=%llu"
          " | winFix=%llu vbA=%llu",
          (unsigned long long)s_idxSeen.load(std::memory_order_relaxed),
          (unsigned long long)s_idxPublished.load(std::memory_order_relaxed),
          (unsigned long long)s_idxStripConv.load(std::memory_order_relaxed),
          (unsigned long long)s_idxCutAmbig.load(std::memory_order_relaxed),
          (unsigned long long)s_idxArenaWrites.load(std::memory_order_relaxed),
          (unsigned long long)s_idxZeroCount.load(std::memory_order_relaxed),
          (unsigned long long)s_idxNoDecl.load(std::memory_order_relaxed),
          (unsigned long long)s_idxSlotMiss.load(std::memory_order_relaxed),
          (unsigned long long)s_idxIbBad.load(std::memory_order_relaxed),
          (unsigned long long)s_idxEndianOdd.load(std::memory_order_relaxed),
          (unsigned long long)s_idxRangeBad.load(std::memory_order_relaxed),
          (unsigned long long)s_idxOverCap.load(std::memory_order_relaxed),
          (unsigned long long)s_idxPoolFull.load(std::memory_order_relaxed),
          (unsigned long long)s_idxFmt32.load(std::memory_order_relaxed),
          (unsigned long long)s_idxMaxCount.load(std::memory_order_relaxed),
          (unsigned long long)s_idxWinFixed.load(std::memory_order_relaxed),
          (unsigned long long)s_idxVbFromA.load(std::memory_order_relaxed));
    }
    // ── iter-449L: the VB residency cache line ────────────────────────────
    // hits/uploads is the residency ratio (high = the model fits);
    // mutations/volatiles decompose the dynamic class; deadMB is the price
    // of never-freeing (orphaned pre-mutation entries).
    {
      const sr::gfx::hle::VbCacheStats vc = sr::gfx::hle::vbCacheStats();
      if (vc.uploads || vc.rejects)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_VBC] entries=%u liveMB=%llu deadMB=%llu | hits=%llu "
            "uploads=%llu mutations=%llu volatiles=%llu rejects=%llu",
            vc.entries, (unsigned long long)(vc.liveBytes >> 20),
            (unsigned long long)(vc.deadBytes >> 20),
            (unsigned long long)vc.hits, (unsigned long long)vc.uploads,
            (unsigned long long)vc.mutations,
            (unsigned long long)vc.volatiles,
            (unsigned long long)vc.rejects);
    }
    // ── iter-449f: the range-class Swap-time re-read ──────────────────────
    // First 12 verdicts only. SAME-WORDS = the data never became indices this
    // frame (refusal final for that draw); CHANGED = ring-timing confirmed,
    // and a deferred latch would have served it.
    if (s6IdxEnabled() && s_idxRecheckLogged < 12) {
      std::lock_guard<std::mutex> lk(s_idxRecheckMu);
      for (auto& r : s_idxRecheck) {
        if (!r.live || s_idxRecheckLogged >= 12) continue;
        r.live = false;
        uint16_t now[8] = {};
        if (const uint8_t* p = sr::kernel::guestVAToHost(r.ibVA))
          for (uint32_t j = 0; j < 8u; j++)
            now[j] = (uint16_t)((p[2 * j] << 8) | p[2 * j + 1]);
        uint32_t sd0 = 0, sd1 = 0;
        safeU32(r.slotVA, sd0);
        safeU32(r.slotVA + 4u, sd1);
        const uint64_t vbNow = (uint64_t)((sd1 >> 2) & 0xFFFFFFu) * 4u;
        bool same = true;
        for (uint32_t j = 0; j < 8u; j++)
          if (now[j] != r.emitWords[j]) { same = false; break; }
        s_idxRecheckLogged++;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_RECHECK] #%llu ib=%08X prim=%u count=%u | emit: %04X %04X "
            "%04X %04X %04X %04X | swap: %04X %04X %04X %04X %04X %04X | "
            "vbSize emit=%llu swap=%llu | %s",
            (unsigned long long)s_idxRecheckLogged, r.ibVA, r.prim, r.count,
            r.emitWords[0], r.emitWords[1], r.emitWords[2], r.emitWords[3],
            r.emitWords[4], r.emitWords[5], now[0], now[1], now[2], now[3],
            now[4], now[5], (unsigned long long)r.vbSizeAtEmit,
            (unsigned long long)vbNow,
            same ? "SAME-WORDS (refusal final)" : "CHANGED (ring-timing)");
      }
    }
    // ── iter-445: the producer-sequence probe ─────────────────────────────
    // splits>0 means a draw's state and the draw itself were separated by
    // another thread — i.e. the executor has been applying the wrong state,
    // and every transform verdict since S6c is suspect.
    {
      const sr::gfx::hle::SeqStats q = sr::gfx::hle::seqStats();
      if (q.opens || q.publishes) {
        char l[420]; int o = 0;
        for (uint32_t i = 0; i < q.tidN && o < 340; i++)
          o += snprintf(l + o, sizeof(l) - o, " tid%lu:pub=%llu,draw=%llu",
                        (unsigned long)q.tid[i],
                        (unsigned long long)q.tidPub[i],
                        (unsigned long long)q.tidDraw[i]);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[SEQ] opens=%llu concurrent=%llu SPLITS=%llu publishes=%llu "
            "producers=%u |%s",
            (unsigned long long)q.opens, (unsigned long long)q.concurrent,
            (unsigned long long)q.splits, (unsigned long long)q.publishes,
            q.tidN, o ? l : " -");
      }
    }
    // iter-444 S6g-a: THE TEXTURE CENSUS — the number that chooses the general
    // texture design. `distinct`/`MB` size the capture; `mutated` decides
    // whether a one-shot snapshot per key is legitimate or whether every bind
    // needs revalidation; the format histogram names which decoders matter.
    if (texCaptureEnabled()) {
      const sr::gfx::hle::ArenaStats t = sr::gfx::hle::texStats();
      std::lock_guard<std::mutex> lk(s_texCapMu);
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_TEXCAP] captured=%llu (%.2f MB) keys=%zu | rejects=%llu "
          "tooBig=%llu | pool writes=%llu inflight=%u peak=%u rejects=%llu | "
          "alias=%llu bases=%zu unsupportedFmt=%u",
          (unsigned long long)s_texCapN, double(s_texCapBytes) / 1048576.0,
          s_texCaptured.size(), (unsigned long long)s_texCapReject,
          (unsigned long long)s_texCapBig, (unsigned long long)t.writes,
          t.inFlight, t.peak, (unsigned long long)t.rejects,
          (unsigned long long)s_texAlias, s_texIdent.size(), s_texUnsupported);
      // iter-450UE FIX A. `rescued` is the verdict number — captures the
      // head-64 rule would have uploaded as all-zero. `ambiguous` must stay 0;
      // any nonzero value blocks a bake until that class is understood.
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_ALIAS] fullspan=%d pick8=%llu pickA=%llu rescued=%llu "
          "ambiguous=%llu truncated=%llu noneReadable=%llu",
          aliasFullspanEnabled() ? 1 : 0,
          (unsigned long long)s_aliasPick8.load(std::memory_order_relaxed),
          (unsigned long long)s_aliasPickA.load(std::memory_order_relaxed),
          (unsigned long long)s_aliasRescued.load(std::memory_order_relaxed),
          (unsigned long long)s_aliasAmbiguous.load(std::memory_order_relaxed),
          (unsigned long long)s_aliasTruncated.load(std::memory_order_relaxed),
          (unsigned long long)s_aliasNone.load(std::memory_order_relaxed));
      // iter-450UF H5: the OLD-vs-NEW selector delta. This — not `ambiguous` —
      // is the bake criterion: `old8/newA` + `oldA/new8` is the complete set of
      // captures whose selected window this change actually moved.
      if (aliasCensusEnabled())
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_SELCENSUS] same=%llu old8/newA=%llu oldA/new8=%llu "
            "unreadable8=%llu",
            (unsigned long long)s_selSame.load(std::memory_order_relaxed),
            (unsigned long long)s_selOld8NewA.load(std::memory_order_relaxed),
            (unsigned long long)s_selOldANew8.load(std::memory_order_relaxed),
            (unsigned long long)s_selNone.load(std::memory_order_relaxed));
      // ── iter-450UG refresh census. Conservation is checkable from this ONE
      // line:  revisits = probed + throttled + volSkip + unreadable + race
      //        probed   = unchanged + changed
      //        changed  = published + rejected + tooBig
      // `probed=0` with `revisits>0` means the fingerprints were never read —
      // "0 changes" would then be a blind probe, not a finding.
      // `scope=2D` is printed literally: the 3D/volume path is deliberately NOT
      // refreshed (static LUT class, ~1 MB each against an 8-slot pool), so
      // nobody reads "no volume refreshes" as "volumes do not mutate".
      // iter-456n bar 4: the Class-A pool's own conservation line. `rejects`
      // MUST be 0 — a nonzero value means a supported guest Unlock was DROPPED,
      // which the fifth adjudication rules an acceptance failure (not a
      // harmless fallback). doubleFree/badHandle prove once-only release.
      if (unlockUploadAllEnabled()) {
        const sr::gfx::hle::ClassAStats ca = sr::gfx::hle::classAStats();
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_CLASSA] writes=%llu (%.2f MB) rejects=%llu doubleFree=%llu "
            "badHandle=%llu | live=%llu B / budget=%llu B | bufs=%u inFlight=%u "
            "peak=%u",
            (unsigned long long)ca.writes, double(ca.bytes) / 1048576.0,
            (unsigned long long)ca.rejects, (unsigned long long)ca.doubleFree,
            (unsigned long long)ca.badHandle, (unsigned long long)ca.liveBytes,
            (unsigned long long)ca.budget, ca.bufs, ca.inFlight, ca.peak);
      }
      // iter-456n, sixth adjudication: the OWNERSHIP machine's own activity.
      // UNCONDITIONAL, not gated on SR_HLE_UNLOCK_UPLOAD_ALL: the legacy k_8
      // Unlock route claims through this same machine, so the DEFAULT gate-OFF
      // path is governed by it too. Without this the packet cannot say whether
      // the repaired decision is even EXERCISED on a roll - and "the replay
      // bars are unchanged" would read as evidence the fix does nothing, when
      // it may simply mean the fixture never re-captures a claimed key.
      // refusals>0 is the positive witness that a stale same-epoch capture was
      // actually blocked.
      {
        auto& own = sr::gfx::hle::unlockOwnership();
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_OWNERSHIP] keys=%llu claims: new=%llu sameEpoch=%llu "
            "replaced=%llu | refusals=%llu | events=%llu (create=%llu "
            "destroy=%llu) | unattributedFirstClaims=%llu | verdicts=%llu "
            "digest=%016llX (noOwner=%llu recreated=%llu staleGen=%llu "
            "refuse=%llu)",
            (unsigned long long)own.trackedKeys(),
            (unsigned long long)own.claimsNew(),
            (unsigned long long)own.claimsSame(),
            (unsigned long long)own.claimsReplaced(),
            (unsigned long long)own.refusals(),
            (unsigned long long)own.objectEvents(),
            (unsigned long long)own.createEvents(),
            (unsigned long long)own.destroyEvents(),
            (unsigned long long)own.unattributedFirstClaims(),
            (unsigned long long)own.verdictCount(),
            (unsigned long long)own.verdictDigest(),
            (unsigned long long)own.verdictNoOwner(),
            (unsigned long long)own.verdictRecreated(),
            (unsigned long long)own.verdictStaleGen(),
            (unsigned long long)own.verdictRefuse());
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_RESOLVE_V2] gate=%d claims=%llu legacyAdopted=%llu "
            "successNoEpoch=%llu capturesRefusedByResolve=%llu",
            sr::gfx::hle::resolveOwnershipV2Enabled() ? 1 : 0,
            (unsigned long long)sr::gfx::hle::resolveV2Claims(),
            (unsigned long long)sr::gfx::hle::resolveV2LegacyAdopted(),
            (unsigned long long)sr::gfx::hle::resolveV2SuccessNoEpoch(),
            (unsigned long long)sr::gfx::hle::resolveV2CapturesRefused());
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_DISPOSITION_SUM] n=%llu digest=%016llX | applied=%llu "
            "refusedLegacySet=%llu refusedResolveEpoch=%llu "
            "refusedUnlockEpoch=%llu refusedOther=%llu uploadFailed=%llu",
            (unsigned long long)sr::gfx::hle::dispositionCount(),
            (unsigned long long)sr::gfx::hle::dispositionDigest(),
            (unsigned long long)sr::gfx::hle::dispositionOfKind(0),
            (unsigned long long)sr::gfx::hle::dispositionOfKind(1),
            (unsigned long long)sr::gfx::hle::dispositionOfKind(2),
            (unsigned long long)sr::gfx::hle::dispositionOfKind(3),
            (unsigned long long)sr::gfx::hle::dispositionOfKind(4),
            (unsigned long long)sr::gfx::hle::dispositionOfKind(5));
      }
      if (texRefreshEnabled())
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_TEXREFRESH] scope=2D DIAGNOSTIC(hash-at-bind, not a production "
            "cache) filter=%08X keys=%zu | revisits=%llu = filtered=%llu + "
            "probed=%llu + "
            "throttled=%llu + volSkip=%llu + unreadable=%llu + race=%llu | "
            "probed = unchanged=%llu + changed=%llu | changed = published=%llu "
            "(%.2f MB) + rejected=%llu + tooBig=%llu + preN=%llu | winFlip=%llu "
            "windowKeys=%zu zeroFill=%llu firstPub=%llu fromN=%llu",
            texRefreshKeyFilter(), s_texRefresh.size(),
            (unsigned long long)s_trRevisits,
            (unsigned long long)s_trFiltered,
            (unsigned long long)s_trProbed, (unsigned long long)s_trThrottled,
            (unsigned long long)s_trVolSkip, (unsigned long long)s_trUnreadable,
            (unsigned long long)s_trRace, (unsigned long long)s_trUnchanged,
            (unsigned long long)s_trChanged, (unsigned long long)s_trPublished,
            double(s_trPubBytes) / 1048576.0, (unsigned long long)s_trReject,
            (unsigned long long)s_trTooBig, (unsigned long long)s_trPreN,
            (unsigned long long)s_trWindowFlip,
            s_texRefreshWindowKeys.size(), (unsigned long long)s_trZeroFill,
            (unsigned long long)s_trFirstPub,
            (unsigned long long)texRefreshFromPresent());
      // ⚠ THE RESET LIVES IN THE SAME BLOCK AS THE PRINT. The content census's
      // throttle went permanently dead because its insert ran under a wider
      // gate than its reset. Do not repeat that.
      if (texRefreshEnabled()) s_texRefreshWindowKeys.clear();
    }
    if (texCensusEnabled()) {
      std::lock_guard<std::mutex> lk(s_texCensusMu);
      char fh[256]; int fo = 0;
      for (uint32_t i = 0; i < 64 && fo < 200; i++)
        if (s_texFmtHist[i])
          fo += snprintf(fh + fo, sizeof(fh) - fo, " f%u=%u", i, s_texFmtHist[i]);
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_TEX] over last 60 frames: distinct=%zu uniqueMB=%.2f "
          "(per frame ~%.1f tex, ~%.2f MB) | binds=%llu tracked=%zu "
          "mutated=%llu/%llu sampled unreadable=%llu unsizedFmt=%llu | "
          "tiled=%u linear=%u maxMB=%.2f |%s",
          s_texFrameKeys.size(), double(s_texFrameBytes) / 1048576.0,
          double(s_texFrameKeys.size()) / 60.0,
          double(s_texFrameBytes) / 1048576.0 / 60.0,
          (unsigned long long)s_texBinds, s_texSeen.size(),
          (unsigned long long)s_texChanged, (unsigned long long)s_texSampled,
          (unsigned long long)s_texUnreadable, (unsigned long long)s_texUnsized,
          s_texTiled, s_texLinear, double(s_texMaxBytes) / 1048576.0,
          fo ? fh : " -");
      s_texFrameKeys.clear();
      s_texFrameBytes = 0;
      s_texTiled = s_texLinear = 0;
      s_texProbes = 0;
      for (uint32_t i = 0; i < 64; i++) s_texFmtHist[i] = 0;
    }
    // iter-442 S4.2 — THE ACTIVITY GATE. A first-light verdict is only
    // attributable if this roll actually had a movie frame to draw: movie
    // decode is still roll-dependent (the open no-movie class, iter-440q), and
    // on a roll that decoded nothing there are no Locks, so a CORRECT renderer
    // still draws black. snaps==0 means "nothing to render", never "broken".
    // rejects>0 means the arena lapped — pixel data was dropped, and any
    // darkness after that IS ours.
    const sr::gfx::hle::ArenaStats a = sr::gfx::hle::arenaStats();
    // iter-444 S6e: the small-block pool reports alongside it. The pools fail
    // the same way and the arena's own numbers looked healthy right up until
    // iter-443 started routing 36-byte declarations through 1 MB slots — an
    // unreported pool is a pool whose saturation is discovered a session late.
    const sr::gfx::hle::ArenaStats b = sr::gfx::hle::blockStats();
    // iter-449c: the geometry pool reports beside them. Its `rejects` are
    // OVERFLOWS to the arena (poolWrite falls back), not drops — a real drop
    // shows up as an arena reject.
    const sr::gfx::hle::ArenaStats g = sr::gfx::hle::geoStats();
    SR_LOG_BOOT(sr::LogSeverity::Info,
        // ⛔ iter-447: `snaps` MUST STAY PLANE-ONLY. It is the movie activity
        // gate, and the whole point of `snaps == 3 x movieQuads` is that it
        // says "this roll had a movie frame to draw". The UP producer writes
        // its vertex snapshots into the SAME 1 MB pool, so raw arena writes
        // stopped meaning "plane uploads" the moment SR_S6_UP armed — the first
        // producer roll read snaps=23431 against movieQuads=736 and broke the
        // invariant by construction. Subtracting the UP writes restores the
        // number's MEANING rather than papering over it, and `upSnaps` is
        // printed beside it so the arena's true total is still recoverable.
        // iter-449: the INDEXED producer is the third arena tenant — gathered
        // spans > kBlockBytes land in the same pool, so it subtracts the same
        // way (its own count printed as idxSnaps, same recoverability rule).
        "[S4_STAGE] snaps=%llu upSnaps=%llu idxSnaps=%llu MB=%llu inflight=%u peak=%u rejects=%llu | "
        "blocks=%llu inflight=%u peak=%u rejects=%llu | "
        "geo=%llu inflight=%u peak=%u overflow=%llu | "
        "movieQuads=%llu texLock=%llu texUnlock=%llu | GATE=%s",
        (unsigned long long)(a.writes >= s_upArenaWrites + s_idxArenaWrites.load(std::memory_order_relaxed)
                                 ? a.writes - s_upArenaWrites - s_idxArenaWrites.load(std::memory_order_relaxed) : 0),
        (unsigned long long)s_upArenaWrites,
        (unsigned long long)s_idxArenaWrites.load(std::memory_order_relaxed),
        (unsigned long long)(a.bytes >> 20),
        a.inFlight, a.peak, (unsigned long long)a.rejects,
        (unsigned long long)b.writes, b.inFlight, b.peak,
        (unsigned long long)b.rejects,
        (unsigned long long)g.writes, g.inFlight, g.peak,
        (unsigned long long)g.rejects,
        (unsigned long long)s_movieQuad.n.load(),
        (unsigned long long)s_texLock.n.load(),
        (unsigned long long)s_texUnlock.n.load(),
        // Plane-only here too: a roll that decoded NO movie but published UP
        // or gathered-index vertex snapshots would otherwise report
        // MOVIE-ACTIVE off those writes alone — the gate would confirm exactly
        // what it exists to rule out, on a roll with nothing to draw.
        (a.writes > s_upArenaWrites +
                        s_idxArenaWrites.load(std::memory_order_relaxed))
            ? ((a.rejects || b.rejects) ? "MOVIE-BUT-ARENA-LAPPED"
                                        : "MOVIE-ACTIVE")
            : "NO-MOVIE-THIS-ROLL");
  }
#endif
  if (!sr::native::flush::swapLeaf(ctx, base)) __imp__sub_82112050(ctx, base);
#if SR_ORACLE
  // iter-441 PHASE 1: first producer into the HLE video queue — the guest's
  // own Present, at its own cadence (the migration's guest-driven frame
  // boundary). Phase 2 adds the rest of the named surface as producers and
  // moves translation/present into the hle_video render thread.
  // ── iter-448: THE FRONT BUFFER NAMES ITS OWN PIXELS ───────────────────────
  // `front` is a texture-header OBJECT, not pixel memory: at +28 it embeds a
  // 6-dword fetch constant (proven on the golden dump — f[1] basePage names
  // the pixel allocation, f[2] decodes 1280x720, fmt 6). The guest is telling
  // us, every Swap, which memory this present scans out. Read it HERE (the
  // render thread never touches guest memory), fault-free; 0 = unreadable.
  uint32_t frontKey = 0, frontDims = 0;
  {
    uint32_t f1 = 0, f2 = 0;
    if (front >= 0x1000u && safeU32(front + 32u, f1) && safeU32(front + 36u, f2)) {
      const uint32_t basePage = (f1 >> 12) & 0xFFFFFu;
      if (basePage) {
        frontKey = (basePage * 4096u) & 0x1FFFFFFFu;
        frontDims = f2;
      }
    }
  }
  MIRROR1(Present, present.device = dev; c.present.front = front;
                   c.present.rect = rect; c.present.frontKey = frontKey;
                   c.present.frontDims = frontDims);
  // iter-442 S3: THE PRESENT RENDEZVOUS.
  // Replaces the iter-440c poll, which waited on the RETIRED DrawCommandQueue's
  // boundary counters (that queue no longer receives draws — Pm4Engine::
  // submitDraws is false live — so the old wait had become a no-op that merely
  // looked like backpressure). Now the guest blocks until the render thread has
  // actually executed THIS frame's Present: the reference's
  // g_executedCommandList.wait (video.cpp:2811) and the console's own
  // one-frame-in-flight throttle. Fails OPEN on timeout — a stalled renderer
  // must never wedge the guest.
  if (apiPresentBlockEnabled()) {
    static std::atomic<uint64_t> s_blockUs{0}, s_timeouts{0}, s_blocked{0};
    const uint64_t target = n;  // this hook's own swap ordinal
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = sr::gfx::hle::waitForPresent(target, 200);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    s_blockUs.fetch_add(uint64_t(us), std::memory_order_relaxed);
    const uint64_t b = s_blocked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!ok) s_timeouts.fetch_add(1, std::memory_order_relaxed);
    if (b <= 8 || b % 300 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[API_PRESENT] rendezvous #%llu waited=%lldus timeouts=%llu "
                  "avgUs=%llu presented=%llu submitted=%llu",
                  (unsigned long long)b, (long long)us,
                  (unsigned long long)s_timeouts.load(std::memory_order_relaxed),
                  (unsigned long long)(s_blockUs.load(std::memory_order_relaxed) / b),
                  (unsigned long long)sr::gfx::hle::presentCount(),
                  (unsigned long long)sr::gfx::hle::presentsSubmitted());
  }
#endif
}

#if SR_ORACLE
// --- Clear (D3DCOLOR form) ---------------------------------------------------
// (dev, Count, pRects, Flags, D3DCOLOR, Z=f1, Stencil) — XDK signature,
// verified iter-440. XDK flags: 0x1=TARGET, 0x10=ZBUFFER, 0x20=STENCIL.
extern "C" void __imp__sub_82101998(PPCContext& __restrict ctx, uint8_t* base);
void sub_82101998(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x82101998);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_clear);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] Clear #%llu dev=%08X count=%u rects=%08X flags=%08X "
        "color=%08X",
        (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        ctx.r7.u32);
  }
  if (apiClearEnabled()) {
    const uint32_t argb = ctx.r7.u32;
    const float rgba[4] = {((argb >> 16) & 0xFFu) / 255.0f,
                           ((argb >> 8) & 0xFFu) / 255.0f,
                           (argb & 0xFFu) / 255.0f,
                           ((argb >> 24) & 0xFFu) / 255.0f};
    uint32_t f = 0;
    if (ctx.r6.u32 & 0x1u) f |= 0xFu;          // TARGET -> all color slots
    f |= (ctx.r6.u32 & 0x30u);                 // Z / STENCIL pass through
    if (ctx.r4.u32 != 0 && ctx.r5.u32 != 0) {  // rect list present: v1 clears full RT
      static std::atomic<uint32_t> s_rectN{0};
      const uint32_t rn = s_rectN.fetch_add(1, std::memory_order_relaxed) + 1;
      if (rn <= 6 || rn % 2000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[API_CLEAR] Clear with %u rect(s) — v1 clears FULL RT", ctx.r4.u32);
    }
    pushGuestClear(f, rgba, float(ctx.f1.f64), ctx.r8.u32);
  }
  // iter-445: PUBLISH THE TRUTH. `z` was a literal 0 while the guest's actual
  // depth arrived in f1 and was decoded two lines above for pushGuestClear —
  // whose sink is the DELETED model's queue and is gated OFF. `color` is fine
  // as-is: for Clear (unlike ClearF) r7 is a packed D3DCOLOR scalar, not a VA.
  rtAttribute(ctx.r3.u32, (uint32_t)sr::gfx::hle::RenderCommandType::Clear);
  {
    const float zf = float(ctx.f1.f64);
    uint32_t zbits = 0;
    std::memcpy(&zbits, &zf, 4);
    // ⛔⛔ iter-446 RE-CORRECTION — iter-445 got this HALF right and the wrong
    // half was published as GOLD. §6g.8 said "D3DDeviceClear has NO stencil
    // argument" because r8 held a guest pointer (0x829A165C). r8 is indeed not
    // the stencil, but the conclusion does not follow: under the PPC64 ABI the
    // `Z` argument travelling in f1 STILL CONSUMES ITS INTEGER SLOT, so r8 is
    // the SKIPPED slot and the stencil is simply one register further along.
    //   (r3 dev, r4 Count, r5 pRects, r6 Flags, r7 Color, [r8 SKIPPED],
    //    r9 Stencil, f1 Z)
    // PROVEN from the recompiled prologue: this wrapper does `mr r27,r9` and
    // then `mr r8,r27` before EACH tail call into ClearF — i.e. it moves its
    // own r9 into ClearF's stencil register, and ClearF's prologue latches r8.
    // The same ABI rule is why ClearF skips r7. See the note on sub_82101A58.
    MIRROR1(Clear, clear.device = ctx.r3.u32; c.clear.count = ctx.r4.u32;
                   c.clear.rects = ctx.r5.u32; c.clear.flags = ctx.r6.u32;
                   c.clear.color = ctx.r7.u32; c.clear.z = zbits;
                   c.clear.stencil = ctx.r9.u32);
  }
  __imp__sub_82101998(ctx, base);
}
#endif

// --- ClearF (float4 color form; title calls this directly too) ---------------
// ⚠⚠ AND EVERY D3DDeviceClear ARRIVES HERE TOO. sub_82101998 is a WRAPPER, not
// a peer: it vupkd3d128-unpacks its D3DCOLOR to a float4 and tail-calls this
// function once per rect (or once with rect=0). Our hooks DELEGATE, so the
// guest's own Clear re-enters this hook — which means an implementation must
// live HERE and ONLY here. Executing both arms would DOUBLE-CLEAR. See
// ENGINE_MAP §6g.8; the Phase-0 note "Clear needs BOTH hooks" is true for
// OBSERVATION and false for IMPLEMENTATION.
// ⛔⛔ SIGNATURE CORRECTED iter-446 — verified against the recompiled PPC
// prologue, not the decompiler and not the XDK header:
//   (r3 dev, r4 Flags, r5 pRect, r6 pColorFloat4, [r7 SKIPPED], r8 Stencil,
//    f1 Z)
// THE FLOAT ARGUMENT CONSUMES A GPR SLOT. Under the PPC64 ABI a double passed
// in f1 still occupies its position in the integer parameter sequence, so r7 is
// DEAD and the stencil is r8. The prologue proves it: it latches r8 into the
// callee-saved r20 (`mr r20,r8`) and never touches r7 at all.
// ⚠ We published r7 for the whole of iter-445, and it read as 0 and 42 — the
// exact "plausible small integer" trap b62f493 named while fixing the SAME bug
// one register over in the Clear wrapper. b62f493 concluded from r8 holding a
// pointer that "Clear has NO stencil argument"; the truth is that Clear skips
// r8 for its own f1 and its stencil is r9 (`mr r27,r9` -> passed here as r8).
//
// FLAGS ARE THE 360 LAYOUT, and this function is what proves it: it splits
// `Flags & 0xF` for the target clear and `Flags & ~0xF` for the depth/stencil
// clear, calling D3DDeviceClearImpl_sub_821013D0 separately for each.
//   bits 0-3 = TARGET0..TARGET3   bit 4 (0x10) = ZBUFFER   bit 5 (0x20) = STENCIL
// ⚠ NOT the PC D3D9 values. Measured flags 0x3F (all targets + Z + stencil) and
// 0x30 (depth+stencil ONLY, no target) — under PC semantics 0x30 would AND to
// zero and every depth clear would be silently dropped.
extern "C" void __imp__sub_82101A58(PPCContext& __restrict ctx, uint8_t* base);
void sub_82101A58(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x82101A58);  // NATIVE N0 (scripts/gen_native_brackets.py)
#if SR_ORACLE
  API_OBS_PROLOGUE(s_clearF);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] ClearF #%llu dev=%08X flags=%08X rect=%08X colorVec=%08X",
        (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  if (apiClearEnabled()) {
    float rgba[4] = {0, 0, 0, 1};
    if (ctx.r6.u32 >= 0x10000u) {
      for (int i = 0; i < 4; i++) rgba[i] = guestF32(ctx.r6.u32 + 4u * i);
    }
    if (ctx.r5.u32 != 0) {
      static std::atomic<uint32_t> s_rectN{0};
      const uint32_t rn = s_rectN.fetch_add(1, std::memory_order_relaxed) + 1;
      if (rn <= 6 || rn % 2000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[API_CLEAR] ClearF with rect — v1 clears FULL RT");
    }
    pushGuestClear(ctx.r4.u32 & 0x3Fu, rgba, float(ctx.f1.f64), ctx.r7.u32);
  }
  // iter-445: PUBLISH THE TRUTH. `color` was a RAW GUEST VA — which the render
  // thread may never dereference — and `z` a literal 0 while the guest's depth
  // arrived in f1. Both are decoded producer-side here, on the guest thread
  // where reading guest memory is legal, exactly as the reference's ProcClear
  // takes `be<float>* color, double z`.
  // ⚠ The colour goes through the SMALL-BLOCK POOL rather than being packed to
  // a D3DCOLOR: the measured render targets are HDR (k_2_10_10_10_FLOAT,
  // k_16_16_FLOAT, k_32_FLOAT — ENGINE_MAP §6g.7), so a clear value can exceed
  // [0,1] and an 8-bit pack would be lossy in a way that matters.
  rtAttribute(ctx.r3.u32, (uint32_t)sr::gfx::hle::RenderCommandType::ClearF);
  {
    float rgbaP[4] = {0, 0, 0, 1};
    if (ctx.r6.u32 >= 0x10000u)
      for (int i = 0; i < 4; i++) rgbaP[i] = guestF32(ctx.r6.u32 + 4u * i);
    const float zf = float(ctx.f1.f64);
    uint32_t zbits = 0;
    std::memcpy(&zbits, &zf, 4);
    const uint32_t cb = sr::gfx::hle::blockWrite(rgbaP, sizeof(rgbaP));
    // ⚠ PUBLISH EVEN ON FAILURE (bytes=0). The executor's arm is the ONLY thing
    // that releases this block, so a command that never publishes leaks a slot —
    // the exact iter-443 failure (rejects=2,320,314, arena pinned at 48/48).
    MIRROR1(ClearF, clearF.device = ctx.r3.u32; c.clearF.flags = ctx.r4.u32;
                    c.clearF.rect = ctx.r5.u32;
                    c.clearF.colorBlock = cb;
                    c.clearF.colorBytes =
                        (cb == sr::gfx::hle::kBlockFull) ? 0u : (uint32_t)sizeof(rgbaP);
                    c.clearF.z = zbits; c.clearF.stencil = ctx.r8.u32);
  }
#endif
  // iter-467 native-b row 9 (kernel/native_rec_b.hpp): the native record (SR_NB_REC), or the dry port compared with
  // the guest body (SR_NB_REC_CHECK).
  namespace nb = sr::native::recb;
  if (nb::on() && nb::entryOn(nb::kClearF) && nb::clearF(ctx, base)) return;
#if SR_ORACLE
  if (nb::checkOn()) nb::checkBefore(nb::kClearF, ctx, base);
#endif
  __imp__sub_82101A58(ctx, base);
#if SR_ORACLE
  if (nb::checkOn()) nb::checkAfter(nb::kClearF, ctx, base);
#endif
}

#if SR_ORACLE
// --- SetRenderTarget ---------------------------------------------------------
extern "C" void __imp__sub_820F2CA0(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F2CA0(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F2CA0);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_setRT);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] SetRenderTarget #%llu dev=%08X idx=%u surf=%08X",
        (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  // Latch the arguments: delegating clobbers the volatile registers.
  const uint32_t dev = ctx.r3.u32, idx = ctx.r4.u32, surf = ctx.r5.u32;
  bool firstBind = false;
  if (rtCensusEnabled()) {
    std::lock_guard<std::mutex> lk(s_rtMu);
    s_rtWin++;
    const uint64_t key = ((uint64_t)idx << 32) | surf;
    firstBind = (++s_rtBinds[key] == 1) && surf >= 0x1000u;
  }
  __imp__sub_820F2CA0(ctx, base);
  // ⚠ The header is read AFTER delegating, and that ordering is load-bearing:
  // this very function READ-MODIFY-WRITES the format nibble at surface+28 for
  // the 2_10_10_10 family (swapping the base and _AS_ variants), and it is the
  // POST-write value it leaves in the register shadow that the GPU uses.
  // Reading first would publish a format the hardware never sees.
  if (firstBind) {
    std::lock_guard<std::mutex> lk(s_rtMu);
    dumpSurfaceHeader("RT", idx, surf);
  }
  // ── iter-449b: THE NULL-BIND PROBE ─────────────────────────────────────────
  // MEASURED (full3): binds null=151,880 (~40/frame) and noRt=642,213 draws —
  // HALF of everything recorded — arrive with no captured target. Three
  // readings, one probe: after the delegate, what does the guest's OWN device
  // shadow (dev+12168+4*idx, ENGINE_MAP §6d) say RT idx is?
  //   arg==0, shadow==0       -> genuine unbind; targetless draws are honest
  //   arg==0, shadow!=0       -> NULL means "keep/restore" (or a second bind
  //                              path writes the shadow) — the executor's null
  //                              mirror is then OUR gap, not the guest's state
  //   arg!=0, shadow!=arg     -> the shadow resolves binds to a DIFFERENT
  //                              surface (wrapper indirection)
  // Counters always on (one guest read per bind); first 8 of each named class.
  {
    uint32_t shadow = 0;
    safeU32(dev + 12168u + 4u * (idx & 3u), shadow);
    static std::atomic<uint64_t> s_nbBoth0{0}, s_nbShadowLive{0}, s_nbMis{0};
    const char* cls = nullptr;
    std::atomic<uint64_t>* ctr = nullptr;
    if (surf < 0x1000u) {
      if (shadow < 0x1000u) { cls = "null-and-null"; ctr = &s_nbBoth0; }
      else { cls = "NULL-BUT-SHADOW-LIVE"; ctr = &s_nbShadowLive; }
    } else if (shadow != surf) { cls = "BIND-SHADOW-MISMATCH"; ctr = &s_nbMis; }
    if (ctr) {
      const uint64_t k = ctr->fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_NULLBIND] %s #%llu idx=%u arg=%08X shadow=%08X (both0=%llu "
            "shadowLive=%llu mismatch=%llu)",
            cls, (unsigned long long)k, idx, surf, shadow,
            (unsigned long long)s_nbBoth0.load(std::memory_order_relaxed),
            (unsigned long long)s_nbShadowLive.load(std::memory_order_relaxed),
            (unsigned long long)s_nbMis.load(std::memory_order_relaxed));
    }
    // The tallies ride the existing 60th-swap cadence via a slow drip instead:
    // every 4096th null bind restates the totals so a long roll's last word is
    // never just the first 8.
    static std::atomic<uint64_t> s_nbAll{0};
    if (surf < 0x1000u &&
        (s_nbAll.fetch_add(1, std::memory_order_relaxed) + 1) % 4096 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_NULLBIND] totals: both0=%llu shadowLive=%llu mismatch=%llu",
          (unsigned long long)s_nbBoth0.load(std::memory_order_relaxed),
          (unsigned long long)s_nbShadowLive.load(std::memory_order_relaxed),
          (unsigned long long)s_nbMis.load(std::memory_order_relaxed));
  }
  uint32_t si = 0, ci = 0, bs = 0, sw = 0;
  if (apiMirrorEnabled()) readSurfaceHeader(surf, si, ci, bs, sw);
  MIRROR1(SetRenderTarget, setRenderTarget.device = dev;
                           c.setRenderTarget.index = idx;
                           c.setRenderTarget.surface = surf;
                           c.setRenderTarget.surfInfo = si;
                           c.setRenderTarget.colorInfo = ci;
                           c.setRenderTarget.byteSize = bs;
                           c.setRenderTarget.sizeWord = sw);
}

// --- SetDepthStencilSurface --------------------------------------------------
extern "C" void __imp__sub_820F2FD0(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F2FD0(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F2FD0);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_setDS);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] SetDepthStencil #%llu dev=%08X surf=%08X",
        (unsigned long long)n, ctx.r3.u32, ctx.r4.u32);
  }
  const uint32_t dev = ctx.r3.u32, surf = ctx.r4.u32;
  bool firstBind = false;
  if (rtCensusEnabled()) {
    std::lock_guard<std::mutex> lk(s_rtMu);
    s_dsWin++;
    firstBind = (++s_dsBinds[surf] == 1) && surf >= 0x1000u;
  }
  __imp__sub_820F2FD0(ctx, base);
  if (firstBind) {
    std::lock_guard<std::mutex> lk(s_rtMu);
    dumpSurfaceHeader("DS", 4, surf);
  }
  uint32_t si = 0, ci = 0, bs = 0, sw = 0;
  if (apiMirrorEnabled()) readSurfaceHeader(surf, si, ci, bs, sw);
  MIRROR1(SetDepthStencilSurface, setDepthStencilSurface.device = dev;
                                  c.setDepthStencilSurface.surface = surf;
                                  c.setDepthStencilSurface.surfInfo = si;
                                  c.setDepthStencilSurface.colorInfo = ci;
                                  c.setDepthStencilSurface.byteSize = bs;
                                  c.setDepthStencilSurface.sizeWord = sw);
}
#endif

// --- Resolve (iter-445) ------------------------------------------------------
// D3DDevice_Resolve — 360 has NO StretchRect; Resolve IS the RT->texture copy,
// so this is the API-level function every offscreen pass ends with.
//
// ARGS VERIFIED against the recompiled PPC (guest/ppc/ppc_recomp.2.cpp,
// __imp__sub_8210C5F8 prologue), NOT assumed from the XDK header:
//   r3 dev (r31)   r4 Flags (r25)   r5 pSourceRect (r26)   r6 pDestTexture
//   (r19)   r7 pDestPoint (r27)   r8 DestLevel   r9 DestSliceOrFace (r22)
//   r10 pClearColor (r21, NULL -> a default float from .rdata)   f1 ClearZ.
// The prologue also PROVES the surface slot layout: `flags & 7` selects the
// source, 4 reads dev+12184 and anything else reads dev[4*(3042+idx)] —
// i.e. RT0..3 at dev+12168+4*idx and the depth surface at dev+12184, which is
// simply slot 4 of the same array. Flags 0x100/0x200 = clear-on-resolve.
extern "C" void __imp__sub_8210C5F8(PPCContext& __restrict ctx, uint8_t* base);
void sub_8210C5F8(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x8210C5F8);  // NATIVE N0 (scripts/gen_native_brackets.py)
#if SR_ORACLE
  if (rtCensusEnabled()) {
    const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, dest = ctx.r6.u32;
    const uint32_t srcIdx = flags & 7u;
    uint32_t srcSurf = 0;
    safeU32(dev + 12168u + 4u * (srcIdx & 7u), srcSurf);
    const uint64_t k = ++s_resolveN;
    {
      std::lock_guard<std::mutex> lk(s_rtMu);
      s_resWin++;
      s_resolveFlagHist[flags]++;
      s_resolveTo[((uint64_t)srcIdx << 32) | dest]++;
    }
    if (k <= 16 || k % 20000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_RESOLVE] #%llu flags=%08X src=%u(surf=%08X) dest=%08X level=%u "
          "slice=%u rect=%08X point=%08X clearCol=%08X",
          (unsigned long long)k, flags, srcIdx, srcSurf, dest, ctx.r8.u32,
          ctx.r9.u32, ctx.r5.u32, ctx.r7.u32, ctx.r10.u32);
  }
  // iter-446 R5: PUBLISH IT. Until now this hook counted and published nothing,
  // so Resolve was the one D3D9 entry point with no command type at all.
  //
  // Both ends are resolved to keys HERE, on the guest thread, because that is
  // the only place guest memory may be read: the source surface comes out of
  // the device's own slot array, and the destination becomes the same
  // fetch-constant base page that SetTexture binds by.
  uint32_t srcSurface = 0, destKey = 0, rectLT = 0, rectRB = 0;
  uint32_t destPoint = 0, levelSliceFlags = 0, destSize = 0;
  const uint32_t rFlags = ctx.r4.u32;
  if (apiMirrorEnabled()) {
    const uint32_t dev = ctx.r3.u32, rectVA = ctx.r5.u32;
    const uint32_t destTex = ctx.r6.u32, pointVA = ctx.r7.u32;
    // ⛔ §6g.12: the DESTINATION'S OWN DECLARED SIZE. The source surface cannot
    // supply it — its byte size is EDRAM-tile-padded, which makes every
    // 360-tall target derive as 368. This is the Xenos size_2d field of the
    // dest texture's fetch constant, read exactly as Resolve itself reads it
    // (`v46 = *(a4+36)`, then the 13/13 split).
    if (destTex >= 0x1000u) safeU32(destTex + 36u, destSize);
    // `Flags & 7` == 4 means the depth surface, which is simply slot 4 of the
    // same array — so ONE formula covers both (12168 + 4*4 == 12184).
    safeU32(dev + 12168u + 4u * (rFlags & 7u), srcSurface);
    uint32_t fetchW1 = 0;
    if (destTex >= 0x1000u) safeU32(destTex + 32u, fetchW1);
    destKey = fetchW1 & 0xFFFFF000u;   // ⚠ same mask as the bind path
    if (rectVA >= 0x1000u) {
      uint32_t l = 0, t = 0, r = 0, b = 0;
      safeU32(rectVA + 0u, l);  safeU32(rectVA + 4u, t);
      safeU32(rectVA + 8u, r);  safeU32(rectVA + 12u, b);
      rectLT = (l & 0xFFFFu) | ((t & 0xFFFFu) << 16);
      rectRB = (r & 0xFFFFu) | ((b & 0xFFFFu) << 16);
    }
    if (pointVA >= 0x1000u) {
      uint32_t x = 0, y = 0;
      safeU32(pointVA + 0u, x);  safeU32(pointVA + 4u, y);
      destPoint = (x & 0xFFFFu) | ((y & 0xFFFFu) << 16);
    }
    // Packed to free a dword for `destSize` — see the union's contract.
    //   bits 0-3 level | 4-11 slice | 12-14 Flags&7 | 15 clear-RT | 16 clear-Z
    levelSliceFlags = (ctx.r8.u32 & 0xFu) | ((ctx.r9.u32 & 0xFFu) << 4) |
                      ((rFlags & 7u) << 12) |
                      ((rFlags & 0x100u) ? (1u << 15) : 0u) |
                      ((rFlags & 0x200u) ? (1u << 16) : 0u);
    // ── Class B v2: carry the destination's REAL descriptor ────────────────
    // Decoded with the shared helper from the destination's own fetch constant
    // at +28 — the same authority the bind path and the widened unlock publish
    // read. Nothing here is inferred from executor-side geometry.
    // Gate-OFF leaves every bit above 16 zero, so the historical packing is
    // preserved byte-for-byte and a gate-OFF trace is unchanged.
    if ((resolveDescCarryEnabled() || resolveOwnershipV2Enabled()) &&
        destTex >= 0x1000u) {
      uint32_t f[6] = {};
      bool okF = true;
      for (uint32_t i = 0; i < 6 && okF; i++)
        okF = safeU32(destTex + 28u + 4u * i, f[i]);
      if (okF) {
        const sr::kernel::texlayout::FetchDesc fd =
            sr::kernel::texlayout::decodeFetchConstant(f);
        if (fd.valid) {
          namespace RC = sr::gfx::hle;
          destSize = (destSize & RC::kResolveSizeMask) |
                     ((fd.fmt & RC::kResolveFmtMask) << RC::kResolveFmtShift);
          levelSliceFlags |=
              (((fd.pitchTexels >> 5) & RC::kResolvePitchMask)
                   << RC::kResolvePitchShift) |
              (fd.tiled ? RC::kResolveTiledBit : 0u) |
              ((fd.endian & RC::kResolveEndianMask) << RC::kResolveEndianShift) |
              RC::kResolveDescValidBit;
        }
      }
    }
  }
  // iter-458 Q: the executor keys the resolve product (s_texSlot,
  // s_resolveOwned) by destKey. The format is not known here, so pin a
  // CONSERVATIVE extent: tile-aligned width x height x 16 bytes/texel.
  if (destKey && destSize) {
    const uint64_t rw = ((uint64_t)(destSize & 0x1FFFu) + 1u + 31u) & ~31ull;
    const uint64_t rh = ((uint64_t)((destSize >> 13) & 0x1FFFu) + 1u + 31u) & ~31ull;
    sr::kernel::physalloc::pinHistory(destKey, rw * rh * 16u);
  }
  MIRROR1(Resolve, resolve.srcSurface = srcSurface; c.resolve.destKey = destKey;
                   c.resolve.destSize = destSize; c.resolve.rectLT = rectLT;
                   c.resolve.rectRB = rectRB; c.resolve.destPoint = destPoint;
                   c.resolve.levelSliceFlags = levelSliceFlags);
#endif
  // iter-467 native-b row 9 (kernel/native_rec_b.hpp): the native record (SR_NB_REC), or the dry port compared with
  // the guest body (SR_NB_REC_CHECK).
  namespace nb = sr::native::recb;
#if SR_ORACLE
  nb::nanWatchTick();  // SR_NB_NANWATCH (observe-only diagnostic; no-op unless set)
#endif
  if (nb::on() && nb::entryOn(nb::kResolve) && nb::resolve(ctx, base)) return;
#if SR_ORACLE
  if (nb::checkOn()) nb::checkBefore(nb::kResolve, ctx, base);
#endif
  __imp__sub_8210C5F8(ctx, base);
#if SR_ORACLE
  if (nb::checkOn()) nb::checkAfter(nb::kResolve, ctx, base);
#endif
}

#if SR_ORACLE
// --- BeginTiling / EndTiling (iter-445, counters only) -----------------------
// If the frame is predicated-tiled, EndTiling emits a Resolve PER TILE, so the
// raw resolve rate above counts tiles, not passes. One counter each settles it.
extern "C" void __imp__sub_8210D588(PPCContext& __restrict ctx, uint8_t* base);
void sub_8210D588(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x8210D588);  // NATIVE N0 (scripts/gen_native_brackets.py)
  if (rtCensusEnabled()) {
    std::lock_guard<std::mutex> lk(s_rtMu);
    if (++s_tileBegin <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_TILING] BeginTiling #%llu dev=%08X flags=%08X count=%u "
          "rects=%08X", (unsigned long long)s_tileBegin, ctx.r3.u32,
          ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_8210D588(ctx, base);
}

extern "C" void __imp__sub_8210DA98(PPCContext& __restrict ctx, uint8_t* base);
void sub_8210DA98(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x8210DA98);  // NATIVE N0 (scripts/gen_native_brackets.py)
  if (rtCensusEnabled()) {
    std::lock_guard<std::mutex> lk(s_rtMu);
    s_tileEnd++;
  }
  __imp__sub_8210DA98(ctx, base);
}

// --- SetStreamSource (iter-440 misname correction: was labeled
// "SetRenderTargetSurfaceTile" pre-migration) --------------------------------
extern "C" void __imp__sub_820F2A50(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F2A50(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F2A50);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_setStream);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] SetStreamSource #%llu dev=%08X stream=%u vb=%08X ofs=%u "
        "stride=%u mask=%08X",
        (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        ctx.r7.u32, ctx.r8.u32);
  }
  // iter-449w TRIPWIRE (observe, always-on): the first attract-demo roll hit
  // an ~18K-call/6ms SetStreamSource storm with garbage args (dev=0, stream in
  // the thousands, vb=F9400000-class) during the demo world load, then wedged
  // permanently inside D3DXPreshaderExecute_sub_8239CAC0 — the whole FE froze
  // (feTick stopped) while presents kept flowing. The storm lines carried no
  // caller, so the corrupt walker was never named, and the wedge is
  // roll-dependent (1-in-3 observed) so live-Frida repros are expensive.
  // Capture lr + the back-chain for the first garbage calls: any future storm
  // roll becomes a self-contained autopsy.
  if (ctx.r3.u32 == 0 || ctx.r4.u32 >= 16u) {
    static std::atomic<uint32_t> s_junkStream{0};
    const uint32_t j = s_junkStream.fetch_add(1, std::memory_order_relaxed) + 1;
    if (j <= 10u || (j & 0x3FFFu) == 0u) {
      uint32_t f[6] = {};
      int n = 0;
      f[n++] = static_cast<uint32_t>(ctx.lr);
      uint32_t sp = ctx.r1.u32;
      for (int i = 0; i < 5 && sp >= 0x1000u && sp < 0xC0000000u; ++i) {
        f[n++] = sr::kernel::readGuestU32BE(sp + 4);
        const uint32_t next = sr::kernel::readGuestU32BE(sp);
        if (next <= sp || next >= 0xC0000000u) break;
        sp = next;
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[SSS_JUNK] #%u dev=%08X stream=%u vb=%08X stride=%u r1=%08X "
          "lr-chain: %08X %08X %08X %08X %08X %08X",
          j, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r7.u32, ctx.r1.u32,
          f[0], f[1], f[2], f[3], f[4], f[5]);
    }
  }
  // iter-456p Step 3A: shadow the binding for the draw-boundary resolution.
  // ⚠ Written for EVERY call including the garbage the tripwire above counts —
  // the range check is what keeps a stream index in the thousands out of the
  // array, and a vb of 0 is a real NULL BIND that must unbind rather than leave
  // the previous buffer standing.
  if (ctx.r4.u32 < sr::gfx::kVbMaxStreams) {
    VbBindSlot& b = s_vbBind[ctx.r4.u32];
    b.objectVA.store(ctx.r5.u32, std::memory_order_relaxed);
    b.offset.store(ctx.r6.u32, std::memory_order_relaxed);
    b.stride.store(ctx.r7.u32, std::memory_order_relaxed);
  }
  MIRROR1(SetStreamSource, setStreamSource.device = ctx.r3.u32;
                           c.setStreamSource.stream = ctx.r4.u32;
                           c.setStreamSource.buffer = ctx.r5.u32;
                           c.setStreamSource.offset = ctx.r6.u32;
                           c.setStreamSource.stride = ctx.r7.u32;
                           c.setStreamSource.dirty = ctx.r8.u32);
  // mem.resources: the outgoing vertex buffer's header fence stamp (kernel/native_resource_stamps.hpp).
  const uint32_t resourceStamp = sr::kernel::resourceStreamStampTarget(ctx.r3.u32, ctx.r4.u32);
  __imp__sub_820F2A50(ctx, base);
  sr::kernel::resourceFenceStampDone(resourceStamp);
}

// --- SetIndices --------------------------------------------------------------
extern "C" void __imp__sub_820F2C08(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F2C08(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F2C08);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_setIdx);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] SetIndices #%llu dev=%08X ib=%08X",
        (unsigned long long)n, ctx.r3.u32, ctx.r4.u32);
  }
  MIRROR1(SetIndices, setIndices.device = ctx.r3.u32;
                      c.setIndices.buffer = ctx.r4.u32);
  const uint32_t resourceStamp = sr::kernel::resourceFenceStampTarget(ctx.r3.u32, sr::kernel::kDevIndexSlot);
  __imp__sub_820F2C08(ctx, base);
  sr::kernel::resourceFenceStampDone(resourceStamp);
}

// --- sub_8261C018 is NOT a create route (tenth adjudication) ----------------
// It was hooked, measured, and the hook is DELETED rather than kept as dead
// code. Unconditional accounting showed it IS called and its pointer shape
// holds (entry r3 == return r3), but the constructed object's first dword is
// 0x38 — type nibble 8, on every call. The director's decode identifies it as a
// 56-byte texture-LAYOUT descriptor, not a D3D texture resource, so the
// nibble-3 guard was right to refuse and there is no generation to raise here.
// sub_82616FB0 is likewise a layout query, not a create route.
// The real branch-A create is finalized by sub_827B9E68, hooked below.

// --- sub_827B9E68: BRANCH A's CREATE, at its finalizer ----------------------
//
// iter-456n (tenth adjudication). The director's static decode settles the
// route the `unattributedFirstClaims` gauge could not: sub_827B9B08 builds and
// copies a nibble-3, 52-byte texture resource header, and sub_827B9E68
// FINALIZES its fetch addresses. sub_827B9E68 has exactly ONE generated call
// site — sub_8277B0A8 branch A — so hooking it covers branch A precisely and
// cannot double-bump branch B, which reaches sub_820FFF08 and is already
// covered there.
//
// r3 at entry is the header (branch A loads it from [r10+20] immediately
// before the call, with r4 the page-aligned pixel base). The header pointer is
// therefore statically established; the type marker and fetch decode only
// VALIDATE it. The key is read AFTER delegating, because finalizing the fetch
// addresses is exactly what makes the fetch constant readable.
extern "C" void __imp__sub_827B9E68(PPCContext& __restrict ctx, uint8_t* base);
void sub_827B9E68(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t hdr = ctx.r3.u32;
  __imp__sub_827B9E68(ctx, base);
  static std::atomic<uint64_t> s_seen{0}, s_type{0}, s_nokey{0};
  const uint64_t seen = s_seen.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t flags = 0;
  if (!hdr || !safeU32(hdr + 0u, flags) || (flags & 0xFu) != 3u) {
    s_type++;
    if (seen <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[TEXLIFE] 827B9E68 #%llu REJECT type hdr=%08X flags=%08X nibble=%u",
          (unsigned long long)seen, hdr, flags, flags & 0xFu);
    return;
  }
  const uint32_t lifeKey = lifecyclePixelKeyFromHeader(hdr);
  if (!lifeKey) {
    s_nokey++;
    if (seen <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[TEXLIFE] 827B9E68 #%llu REJECT nokey hdr=%08X",
          (unsigned long long)seen, hdr);
    return;
  }
  if (seen <= 16)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[TEXLIFE] create#%llu route=827B9E68 hdr=%08X pixelKey=%08X",
        (unsigned long long)seen, hdr, lifeKey);
  sr::gfx::hle::noteTextureObjectEvent(lifeKey, /*reason=*/0u);
}

// --- D3DResourceRelease sub_820F3DA0: THE FINAL-RELEASE LIFECYCLE EDGE ------
//
// iter-456n (eighth adjudication). Ownership is generation-scoped, so a
// destroyed texture's Unlock claim must not survive to be inherited by whatever
// lands at that pixel address next. The mechanism is fully visible in the
// generated recomp source (guest/ppc/ppc_recomp.0.cpp) - IDA is corroboration,
// not the source of truth for it:
//
//   * atomic decrement of the refcount at resource+4 (lwarx/stwcx. loop),
//   * the NEW count is returned in r3,
//   * the destroy dispatch (sub_820F3788) runs ONLY on the zero arm.
//
// "Prove it is a texture" uses the guest's OWN type marker, not a range check:
// D3DResource_GetType_sub_820F3E48 reads `*(u32*)res & 0xF` and treats
//   nibble 3 = TEXTURE   (then res+48 bits 23-24 pick 2D / CUBE / VOLUME)
//   nibble 4 = SURFACE   (0x40000000 = surface OF a texture level, parent at +24)
// so nibble 3 is the texture discriminator. The pixel identity then comes from
// the same fetch-constant decode every other site here uses.
//
// ⚠ The key is read BEFORE delegating: after the destructor has run the header
// is free memory and decoding it would be reading a corpse. The event is
// published AFTER, and only when the delegate reports the count reached zero.
extern "C" void __imp__sub_820F3DA0(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F3DA0(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F3DA0);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const uint32_t res = ctx.r3.u32;
  uint32_t preKey = 0;
  if (res >= 0x1000u) {
    uint32_t flags = 0;
    if (safeU32(res + 0u, flags) && (flags & 0xFu) == 3u)   // nibble 3 = texture
      preKey = lifecyclePixelKeyFromHeader(res);
  }
  __imp__sub_820F3DA0(ctx, base);
  if (preKey && ctx.r3.u32 == 0u) {           // final release: refcount hit 0
    static std::atomic<uint64_t> s_relN{0};
    const uint64_t rn = s_relN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rn <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[TEXLIFE] destroy#%llu route=820F3DA0 res=%08X pixelKey=%08X",
          (unsigned long long)rn, res, preKey);
    sr::gfx::hle::noteTextureObjectEvent(preKey, /*reason=*/1u);
  }
}

// --- Texture create/lock/unlock (THE movie pixel path) -----------------------
extern "C" void __imp__sub_820FFF08(PPCContext& __restrict ctx, uint8_t* base);
void sub_820FFF08(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FFF08);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_texCreate);
  const uint32_t w = ctx.r3.u32, h = ctx.r4.u32, depth = ctx.r5.u32,
                 levels = ctx.r6.u32, usage = ctx.r7.u32, fmt = ctx.r8.u32;
  // iter-442: r9/r10 were never captured. Phase 0 lists the tail as (?, resType)
  // and D3DRESOURCETYPE decides whether this is a 2D texture, cube, or volume —
  // the translation layer cannot pick a view type without it.
  const uint32_t a7 = ctx.r9.u32, resType = ctx.r10.u32;
  __imp__sub_820FFF08(ctx, base);
  const uint32_t tex = ctx.r3.u32;
  // iter-456n (seventh adjudication, lifecycle coverage): the SECOND create
  // route now raises an ordered lifecycle event, so an object created here
  // cannot inherit a dead object's Unlock claim at the same pixel address.
  // Identity comes from the header's own fetch constant, never a guessed
  // offset; an unreadable header raises nothing. The first hits are logged so
  // the correspondence to upload keys is VERIFIABLE rather than assumed.
  if (const uint32_t lifeKey = lifecyclePixelKeyFromHeader(tex)) {
    static std::atomic<uint64_t> s_lifeN{0};
    const uint64_t ln = s_lifeN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ln <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[TEXLIFE] create#%llu route=820FFF08 hdr=%08X pixelKey=%08X",
          (unsigned long long)ln, tex, lifeKey);
    sr::gfx::hle::noteTextureObjectEvent(lifeKey, /*reason=*/0u);
  }
  // Minimal VA->record table. It exists to ANSWER the ownership question (is a
  // side table mandatory?), and doubles as the prototype if the answer is yes.
  namespace tid = sr::kernel::texid;
  if (tex >= 0x1000u) {
    // The pre-existing behavior upsert. Factored so the gate-OFF path is
    // byte-for-byte the legacy `{ lock_guard(s_texMu); if(size<cap) upsert; }`
    // and the gate-ON path runs the SAME upsert under the shared domain-ordered
    // lock (r2 blocker 1: a nested acquisition can never reach s_texMu).
    auto behaviorUpsert = [&]() {
      const size_t sz = s_texMap.size();
      if (sz < kTexMapCap)
        s_texMap[tex] = TexRec{w, h, depth, levels, usage, fmt, 0, 0, 0};
      // Behavior-create accounting (gate-on only): names the EXACT current
      // semantics (Upserted when the assignment ran, CapFull when the size guard
      // blocked it). Observation only — the assignment above is unchanged.
      if (productionGateEnabled())
        tid::classifyBehaviorCreate(s_behCreateCounters, tex, sz, kTexMapCap);
    };
    if (productionGateEnabled()) {
      tid::DomainOrderedLock<std::mutex> bg(tid::LockDomain::Behavior, s_texMu);
      if (bg.locked()) behaviorUpsert();          // !ok never reaches s_texMu
    } else {
      std::lock_guard<std::mutex> lk(s_texMu);     // gate-off: legacy path exactly
      behaviorUpsert();
    }
  } else if (productionGateEnabled()) {
    tid::DomainOrderedLock<std::mutex> bg(tid::LockDomain::Behavior, s_texMu);
    if (bg.locked())
      tid::classifyBehaviorCreate(s_behCreateCounters, tex, s_texMap.size(), kTexMapCap);  // InvalidKey
  }
  // Identity registration (D2): the SEPARATE identity domain, entered AFTER
  // s_texMu is released, via the capability-boundary TU. It runs regardless of
  // the behavior outcome and never conditions it. CREATE is an identity attempt
  // for EVERY key, invalid ones included (§5/§6) — domain::registerCreate ->
  // registerTexIdentity classifies key<0x1000 as InvalidKey before any field is
  // read. The identity mutex/cap/registry are UNREACHABLE from this behavior TU.
  if (productionGateEnabled())
    tid::domain::registerCreate(tex, w, h, fmt, resType);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] TexCreate #%llu %ux%ux%u lv=%u usage=%08X fmt=%08X a7=%08X "
        "resType=%u -> tex=%08X",
        (unsigned long long)n, w, h, depth, levels, usage, fmt, a7, resType, tex);
  }
}

extern "C" void __imp__sub_820FFCB0(PPCContext& __restrict ctx, uint8_t* base);
void sub_820FFCB0(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FFCB0);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_texLock);
  const uint32_t tex = ctx.r3.u32, level = ctx.r4.u32, out = ctx.r5.u32;
#if SR_ORACLE
  const uint32_t readerFlags = ctx.r7.u32;
#endif
  __imp__sub_820FFCB0(ctx, base);
#if SR_ORACLE
  sr::kernel::reader_observe::textureLock(tex, level, out, readerFlags);
#endif
  uint32_t pitch = 0, bits = 0;
  if (out) { safeU32(out, pitch); safeU32(out + 4, bits); }
  // mem.resources step 2a (SR_MEM_RESOURCES=census): a successful Lock opens a guest write window over the texture
  // (its fetch constant's extent) until its Unlock - a page model read inside it is a host race with the guest's
  // writes, not a missed boundary (the Unlock's note follows). Read-only; no-op otherwise.
  if (sr::gfx::memres::census() && bits && tex >= 0x1000u) {
    uint32_t mf[6] = {};
    bool okF = true;
    for (uint32_t i = 0; i < 6 && okF; i++) okF = safeU32(tex + 28u + 4u * i, mf[i]);
    sr::gfx::memres::noteLock(tex, okF ? mf : nullptr);
  }
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] TexLock #%llu tex=%08X lv=%u -> pitch=%u pBits=%08X",
        (unsigned long long)n, tex, level, pitch, bits);
  }
  // iter-442 S4.1 / iter-455n r2 (director blocker 3): LATCH pitch/pBits so
  // Unlock can snapshot. The comment always CLAIMED a failed lock leaves no
  // stale pointer, but the fields were only ever overwritten on success — so
  // success -> FAILED lock -> Unlock re-uploaded the old pointer's bytes. The
  // latch is now cleared on EVERY lock attempt and only then re-armed, and
  // Unlock CONSUMES it (one Unlock per Lock, by construction).
  if (tex >= 0x1000u) {
    std::lock_guard<std::mutex> lk(s_texMu);
    auto it = s_texMap.find(tex);
    if (it != s_texMap.end()) {
      it->second.latch.begin();                     // clears unconditionally
      if (bits && pitch) it->second.latch.succeed(pitch, bits, level);
    }
  }
  // ── iter-455n Phase 2 v1: Lock-late identity registration (r4-frozen) ───────
  // `tex`(=pre-delegate r3) is the identity key; `out`(=pre-delegate r5) is the
  // out-struct. Post-delegate r3 is a computed lock address and is NEVER read
  // here. The ONE checked 8-byte BE span read happens in THIS behavior TU,
  // OUTSIDE every lock (§3/§8), and only the decoded AdmitResult crosses the
  // capability boundary into the identity domain. The identity registry/mutex/
  // cap are UNREACHABLE here, so this seam CANNOT write s_texMap (K3, r3 kill 1).
  if (productionGateEnabled()) {
    namespace tid = sr::kernel::texid;
    if (tex < 0x1000u) {
      tid::domain::noteLockInvalidKey();                 // InvalidKey, no read
    } else {
      const tid::AdmitResult a = tid::lockLateAdmit(
          out, [](uint32_t va, uint32_t bytes) { return checkedGuestSpan(va, bytes); });
      tid::domain::registerLockLate(tex, a);             // admit ? LockLate : LockOutputInvalid
    }
  }
  // RESMODEL: movie planes only (the known first-light surface), first 24 locks.
  TexRec r{};
  if (texLookup(tex, r) && r.fmt == kMoviePlaneFmt) {
    static std::atomic<uint64_t> s_rmL{0};
    const uint64_t k = s_rmL.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 24) logResModel("LOCK", k, tex, level, r, pitch, bits);
  }
}

extern "C" void __imp__sub_820FFCC8(PPCContext& __restrict ctx, uint8_t* base);
void sub_820FFCC8(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FFCC8);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_texUnlock);
  const uint32_t tex = ctx.r3.u32;
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] TexUnlock #%llu tex=%08X", (unsigned long long)n, tex);
  }
  // RESMODEL: sample BEFORE delegating — Unlock's cache-restore path resets the
  // dirty range to the 0xFFFF0000 empty sentinel, so reading after the delegate
  // would show an erased range and "prove" the guest tracks nothing.
  TexRec r{};
  const bool known = texLookup(tex, r);
  const bool isPlane = known && r.fmt == kMoviePlaneFmt;
  // mem.resources step 2a (SR_MEM_RESOURCES=census): the Unlock as a boundary - the texture's extents from its own
  // fetch constant (header dwords 7..12), noted before the delegate (the CPU's writes into the lock are done here), and
  // the guest write window its Lock opened closed; a movie plane's range is also remembered as the decoder's (a
  // boundary-less writer). Read-only; no-op otherwise.
  if (sr::gfx::memres::census() || sr::gfx::memres::resourceMode()) {
    uint32_t mf[6] = {};
    bool okF = true;
    for (uint32_t i = 0; i < 6 && okF; i++) okF = safeU32(tex + 28u + 4u * i, mf[i]);
    sr::gfx::memres::noteUnlock(tex, okF ? mf : nullptr, isPlane);
    if (sr::gfx::memres::resourceMode())
      sr::gfx::memres::resourceMemory().noteTextureUnlock(okF ? mf : nullptr, isPlane);
  }
  // ── iter-455n: THE UNLOCK IS THE FRESHNESS SIGNAL (director order) ─────────
  // UnleashedRecomp uploads the ENTIRE texture unconditionally on every
  // UnlockTextureRect and never permanently dedupes by (address, fmt, w, h).
  // We did the opposite: only movie planes snapshotted here, everything else
  // left to the BIND-time capture, which guesses an alias window and then
  // permanently suppresses the tuple (s_texCaptured.insert) — so a font atlas
  // captured while still empty or sparse stayed that way forever. That is the
  // glyph regression: dialog text renders as solid placeholder bars.
  //
  // SCOPE, deliberately narrow: the single-channel k_8 class only — the class
  // the atlas belongs to, where the latched lock pitch is exact (bytes ==
  // texels) and pitch*height is the true span. Other formats keep today's
  // bind-time path (the director's item 5 fallback) rather than inherit an
  // unproven pitch conversion; they are COUNTED, never silently dropped.
  const uint32_t fmtCode = known ? (r.fmt & 0xFFu) : 0u;
  const bool snapWanted = known && unlockSnapshotWanted(r.fmt);
  if (known && !snapWanted) {
    static std::atomic<uint64_t> s_uSkipFmt{0};
    const uint64_t sk = s_uSkipFmt.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sk <= 8 || sk % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[TEXUNLOCK_SKIP] #%llu tex=%08X fmt=%08X (code=%u) not k_8 - "
          "bind-time capture remains this texture's path",
          (unsigned long long)sk, tex, r.fmt, fmtCode);
  }
  if (isPlane) {
    static std::atomic<uint64_t> s_rmU{0};
    const uint64_t k = s_rmU.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 24) logResModel("UNLOCK-PRE", k, tex, 0, r, 0, 0);
  }

  // ── iter-442 S4.2: THE PRODUCER-SIDE SNAPSHOT ─────────────────────────────
  // This is THE upload point (ENGINE_MAP §6g.2). It must happen HERE, on the
  // guest's own thread, before the delegate: the guest reports no dirty span
  // (so we take the whole locked subresource) and the FMV decoder rewrites the
  // SAME allocation every frame, so a render thread dereferencing r.lockBits
  // later would render a LATER frame's bytes. Planes are linear, pitch is
  // guest-fixed, so the snapshot is one memcpy of pitch*height.
  //
  // Gated on apiMirrorEnabled(): the queue is the only thing that frees an
  // arena slot, so allocating without publishing a command would leak it.
  // iter-455n r2: the decision, sizing, identity normalization, refusals and
  // command-field construction all live in ONE body (tex_unlock_rule.hpp) that
  // the self-test drives with fakes. Production injects the three real steps.
  uint32_t arenaOfs = sr::gfx::hle::kArenaFull, snapBytes = 0;
  sr::kernel::texunlock::UnlockResult ur{};
  // ── iter-456n CLASS A (SR_HLE_UNLOCK_UPLOAD_ALL, default OFF) ─────────────
  // The reference publishes EVERY Unlock unconditionally (video.cpp:2190). We
  // widen toward that: formats the legacy k_8 predicate refuses now publish
  // too, sized by the SHARED layout helper's maxTouched and described by the
  // texture header's OWN fetch constant (pitch in TEXELS — the Lock out-struct
  // reports BYTES, and the two coincide only for k_8).
  //
  // ROUTING, chosen so both gate states leave the proven path untouched:
  //   legacy-eligible (k_8)  -> arena + TextureUnlock, EXACTLY as before, in
  //                             both gate states. The atlas cannot move.
  //   widened (non-k_8)      -> texture pool + TextureUpload, whose executor
  //                             arm already calls hleUploadHostTextureFmt (the
  //                             format-aware decoder). TextureUnlock's arm is
  //                             R8-only, which is why widened data cannot ride
  //                             it (director's R2 note).
  // The TextureUnlock mirror is still published for EVERY unlock so
  // [HLE_MIRROR] stays hle==api; on the widened branch it carries bytes=0, so
  // the executor performs no upload, takes no ownership and releases nothing —
  // the arena/texture slot is owned by exactly one command, never both.
  const bool wideGate = unlockUploadAllEnabled();
  const bool goWide = wideGate && known && !snapWanted;
  uint32_t wideUpOfs = sr::gfx::hle::kArenaFull, wideUpBytes = 0;
  sr::kernel::texunlock::WidePlan widePlan{};
  if (goWide) {
    // The header's own 24-byte fetch constant (ENGINE_MAP §6g.2: hdr[7..12]).
    uint32_t f[6] = {};
    bool okF = true;
    for (uint32_t i = 0; i < 6 && okF; i++) okF = safeU32(tex + 28u + 4u * i, f[i]);
    if (okF) {
      widePlan.fd = sr::kernel::texlayout::decodeFetchConstant(f);
      // ── iter-456p Slice A.2: THE WIDENED PUBLISH IS PLAIN-2D ONLY ─────────
      // Correcting the decoder's metadata is NOT authorization to publish 1D,
      // stacked, 3D, cube or mip data (director ruling). Before the dimension
      // was decoded at all, a 3D or cube header arrived here wearing garbage 2D
      // extents and computeLayout sized against them; now the shape is known,
      // anything that is not an unstacked 2D texture is refused outright rather
      // than published at a corrected-but-unsupported size.
      if (widePlan.fd.valid && !widePlan.fd.isPlain2D()) {
        static uint64_t s_nonPlain2D = 0;
        if (++s_nonPlain2D <= 8)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[TEXWIDE] REFUSED non-plain-2D widened publish: tex=%08X dim=%s "
              "stacked=%d %ux%ux%u fmt=%u",
              tex, sr::kernel::texlayout::texDimName(widePlan.fd.dim),
              widePlan.fd.stacked ? 1 : 0, widePlan.fd.width,
              widePlan.fd.height, widePlan.fd.depth, widePlan.fd.fmt);
        widePlan.fd.valid = false;
      }
      widePlan.haveFetch = widePlan.fd.valid;
      if (widePlan.fd.valid)
        widePlan.layout = sr::kernel::texlayout::computeLayout(
            widePlan.fd.fmt, widePlan.fd.width, widePlan.fd.height,
            widePlan.fd.pitchTexels, widePlan.fd.tiled, /*level=*/0,
            /*budgetBytes=*/sr::gfx::hle::kClassAMaxBytes);
    }
  }
  if (apiMirrorEnabled()) {
    sr::kernel::texunlock::LockLatch latch{};
    {  // consume the latch under the map lock, then run the body lock-free
      std::lock_guard<std::mutex> lk(s_texMu);
      auto it = s_texMap.find(tex);
      if (it != s_texMap.end()) { latch = it->second.latch; it->second.latch.begin(); }
    }
    if (goWide) {
      // Widened branch: snapshot into the TEXTURE pool (texWrite) because the
      // TextureUpload arm reads/releases that pool — mixing pools would read
      // garbage and free a slot in the wrong allocator.
      ur = sr::kernel::texunlock::runUnlock(
          sr::kernel::texunlock::TexInfo{r.fmt, r.w, r.h}, latch,
          (uint32_t)sr::gfx::hle::kClassAMaxBytes, sr::gfx::hle::kArenaFull,
          [](uint32_t va, uint32_t bytes) -> const uint8_t* {
            sr::kernel::physalloc::pinHistory(va, bytes);   // iter-458 Q: before the snapshot
            return checkedGuestSpan(va, bytes);
          },
          // ⛔ ISOLATED POOL (fifth adjudication, bar 4): widened snapshots use
          // the bounded variable-size Class-A pool, NOT the 8-slot texture pool
          // bind capture competes for. A 2 KB icon costs 2 KB here.
          [](const uint8_t* p, uint32_t bytes) {
            return sr::gfx::hle::classAWrite(p, bytes);
          },
          [&](const sr::kernel::texunlock::UnlockResult& res, uint32_t ofs) {
            wideUpOfs = ofs; wideUpBytes = res.bytes;
          },
          &widePlan);
      if (ur.published && wideUpBytes) {
        sr::gfx::hle::RenderCommand tu;
        tu.type = sr::gfx::hle::RenderCommandType::TextureUpload;
        tu.textureUpload.key = ur.key;
        tu.textureUpload.arenaOffset = wideUpOfs;
        tu.textureUpload.bytes = wideUpBytes;
        tu.textureUpload.dims = (ur.width << 16) | (ur.height & 0xFFFFu);
        // Same packing the capture site uses: fmt | tiled<<8 | endian<<12,
        // dimension bits 14-15 left 0 = 2D.
        // ⛔ BIT 26 = UNLOCK-ORIGIN, and it is load-bearing. MEASURED without
        // it (iter-456n live1): the widened unlock upload for 0BC22000 landed
        // at upload #6 with real bytes, and a LATER BIND-TIME CAPTURE of the
        // same key re-uploaded it ALL-ZERO at #15 — the stale capture clobbered
        // the fresh guest data. The legacy TextureUnlock arm has always been
        // protected by `s_unlockOwned`; routing widened data through
        // TextureUpload bypassed that protection. This bit tells the executor
        // "this came from a guest Unlock": it supersedes existing ownership
        // (an explicit Unlock IS newer than an older capture or resolve — the
        // director's R2 rule) and TAKES ownership so later captures cannot
        // overwrite it.
        tu.textureUpload.fmtBits = (ur.fmtCode & 0xFFu) |
                                   ((ur.tiled ? 1u : 0u) << 8) |
                                   ((ur.endian & 3u) << 12) |
                                   sr::gfx::hle::kUploadUnlockOriginBit;
        tu.textureUpload.pitch = ur.pitch;
        sr::gfx::hle::enqueue(tu);
        static std::atomic<uint64_t> s_wideN{0};
        const uint64_t wn = s_wideN.fetch_add(1, std::memory_order_relaxed) + 1;
        if (wn <= 16 || wn % 500 == 0)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[TEXUNLOCK_WIDE] #%llu key=%08X fmt=%u %ux%u pitch=%u tiled=%u "
              "endian=%u bytes=%u (maxTouched; padded=%llu)",
              (unsigned long long)wn, ur.key, ur.fmtCode, ur.width, ur.height,
              ur.pitch, ur.tiled ? 1u : 0u, ur.endian, wideUpBytes,
              (unsigned long long)widePlan.layout.spanPadded);
      }
    } else
    ur = sr::kernel::texunlock::runUnlock(
        sr::kernel::texunlock::TexInfo{r.fmt, r.w, r.h}, latch,
        (uint32_t)sr::gfx::hle::kArenaSlotBytes, sr::gfx::hle::kArenaFull,
        // CHECKED whole-range translation (blocker 4): proves no overflow,
        // contiguous translation, and committed/readable host pages across the
        // ENTIRE span BEFORE any access. That ordering is required because the
        // harness VEH makes a host AV fatal before any SEH filter runs — but it
        // is a discipline, not a demonstration of fault-freedom (director
        // correction 2026-08-14).
        [](uint32_t va, uint32_t bytes) -> const uint8_t* {
          sr::kernel::physalloc::pinHistory(va, bytes);   // iter-458 Q: before the snapshot
          return checkedGuestSpan(va, bytes);
        },
        [](const uint8_t* p, uint32_t bytes) {
          return sr::gfx::hle::arenaWrite(p, bytes);
        },
        [&](const sr::kernel::texunlock::UnlockResult& res, uint32_t ofs) {
          arenaOfs = ofs; snapBytes = res.bytes;
        });
    if (!ur.published && ur.refuse != sr::kernel::texunlock::Refuse::NotEligibleFormat) {
      static std::atomic<uint64_t> s_ref{0};
      const uint64_t rn = s_ref.fetch_add(1, std::memory_order_relaxed) + 1;
      if (rn <= 16 || rn % 500 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TEXUNLOCK_REFUSE] #%llu tex=%08X reason=%s", (unsigned long long)rn,
            tex, sr::kernel::texunlock::refuseName(ur.refuse));
    }
  }
  if (snapWanted && snapBytes) {
    static std::atomic<uint64_t> s_snap{0};
    const uint64_t k = s_snap.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 12 || k % 600 == 0) {
      const sr::gfx::hle::ArenaStats a = sr::gfx::hle::arenaStats();
      // END-TO-END CONTENT CHECK: hex the ARENA copy, not the guest memory —
      // this is literally the buffer the render thread will read, so it proves
      // the offset and the memcpy together. It also closes the §6g sampling
      // trap: a plane read before the decoder wrote is all zeros, and "all
      // zeros" is indistinguishable from a black frame downstream. Real video
      // is Y ~0x21 / U ~0x7C / V ~0x78 (ENGINE_MAP §6g.2). Safe to read here:
      // the slot is held until MIRROR1 below hands it to the queue.
      char pix[40] = {'-', 0};
      if (snapBytes) {
        if (const uint8_t* q = sr::gfx::hle::arenaRead(arenaOfs)) {
          size_t o = 0;
          for (int i = 0; i < 12 && o + 3 < sizeof(pix); i++)
            o += (size_t)snprintf(pix + o, sizeof(pix) - o, "%02X", q[i]);
        }
      }
      // iter-442: WINDOW-DISAGREEMENT PROBE. The green cast is proven to be in
      // the plane DATA (our render path matches an independent CPU reference),
      // and one live candidate is that the guest writes a plane through an
      // 0xA-class VA while we snapshot the 0x8 window — the exact iter-424/425
      // class that bit the vertex buffer this same iteration. Unlike the VB the
      // planes are NOT zero here, so a stale-but-plausible read would be
      // invisible; compare the two windows directly instead. Sums over the real
      // pixels only (row padding excluded — it is zero and would dilute both).
      // iter-455n r2 (blocker 4): both windows are now named EXPLICITLY as
      // 0x8|phys and 0xA|phys and read through the SAME checked whole-span
      // translation as the upload. Previously this labelled whichever window
      // lockBits happened to carry as "mean8" — wrong whenever the guest
      // locked through 0xA, which is exactly the atlas case.
      uint64_t sum8 = 0, sumA = 0;
      {
        const uint32_t phys = ur.srcVA & sr::kernel::texunlock::kKeyMask;
        const uint32_t span = ur.bytes;
        const uint8_t* p8 = checkedGuestSpan(0x80000000u | phys, span);
        const uint8_t* pA = checkedGuestSpan(0xA0000000u | phys, span);
        for (uint32_t row = 0; row < r.h; row++) {
          const uint32_t off = row * ur.pitch;
          if (p8) for (uint32_t x = 0; x < r.w; x++) sum8 += p8[off + x];
          if (pA) for (uint32_t x = 0; x < r.w; x++) sumA += pA[off + x];
        }
      }
      const uint64_t px = (uint64_t)r.w * r.h;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S4_WIN] tex=%08X %ux%u mean8=%.1f meanA=%.1f %s",
          tex, r.w, r.h, px ? double(sum8) / double(px) : 0.0,
          px ? double(sumA) / double(px) : 0.0,
          sum8 == sumA ? "IDENTICAL" : "*** WINDOWS DISAGREE ***");
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S4_SNAP] #%llu tex=%08X %ux%u pitch=%u lv=%u src=%08X key=%08X -> "
          "ofs=%08X bytes=%u pix=%s | arena writes=%llu MB=%llu inflight=%u "
          "peak=%u rejects=%llu",
          (unsigned long long)k, tex, r.w, r.h, ur.pitch, ur.level,
          ur.srcVA, ur.key, arenaOfs, snapBytes, pix, (unsigned long long)a.writes,
          (unsigned long long)(a.bytes >> 20), a.inFlight, a.peak,
          (unsigned long long)a.rejects);
    }
  }
  // Published for EVERY unlock (bytes=0 when there was nothing to snapshot) so
  // the [HLE_MIRROR] acceptance test stays hle==api.
  // ⚠ The key is the PIXEL VA (r.lockBits), NOT the header handle `tex`. The
  // sampler side can only name a texture by the fetch constant's base, which is
  // the pixel VA — so both sides must agree on that identity or every bind
  // resolves to the white fallback and the quad paints flat.
  // ⛔ iter-455n r2 (director blocker 1) — THE IDENTITY. `ur.key` is the
  // NORMALIZED VA (srcVA & 0x1FFFFFFF). The atlas locks at ABA5A000 while the
  // shader binds 0BA5A000; publishing the raw pointer made the backend store
  // the fresh resource under one name while s_texSlot and the swizzled-view
  // lookup both used the other — binding fresh pixels with a stale alpha view,
  // which defeats the glyph fix. Source pointer and identity are now separate:
  // ur.srcVA was the memcpy source, ur.key is what every consumer names.
  MIRROR1(TextureUnlock, textureUnlock.texture = ur.key;
                         c.textureUnlock.arenaOffset = arenaOfs;
                         c.textureUnlock.bytes = snapBytes;
                         c.textureUnlock.pitch = ur.pitch;
                         c.textureUnlock.width = ur.width;
                         c.textureUnlock.height = ur.height;
                         c.textureUnlock.level = ur.level);
  __imp__sub_820FFCC8(ctx, base);
}

// ── iter-456: THE UNHOOKED CREATE — in-place texture-header init ─────────────
// D3DSurfaceInitFetchConstant_sub_82616C40(holder, w:u16, h:u16, levels:u8,
// usageFlags, fmtDword) memsets and fills the 56-byte texture header at
// holder[0] (pixel allocation at holder[2]) and returns the header VA — WITHOUT
// ever passing through TexCreate sub_820FFF08. The k1-r5 exact-object witness
// proved every unknown-identity Lock/Unlock texture in a natural boot (six TGA
// UI loads via TgaTextureCreate sub_822F9918, fmt dword 0x18280186 = k_8_8_8_8)
// is a product of THIS function, so its creations were invisible to s_texMap:
// their Locks latched nothing and their Unlocks refused at NotEligibleFormat.
// PLATFORM FIX (charter class): the console's D3D9 knew every texture it
// constructed; registering the returned header exactly the way the TexCreate
// hook does gives the HLE the same knowledge. The guest decides everything —
// this observes a create it already performed.
// Gate SR_HLE_TEX1640_REG — BAKED DEFAULT-ON iter-456 after two clean gated
// K=1 rolls (r1 mechanism roll: registration + honest TEXUNLOCK_SKIP for the
// six TGA textures, atlas 88ED7E10/0BA5A000 snapshot+upload live, aggregate
// healthy; r2 recorded roll: press-start + legal text render as real glyphs on
// BOTH captures, GPU readback authoritative). =0 keeps the A/B: gate-off
// reproduces the pre-iter-456 identity gap (creates skip registration; the
// [TEX1640] census line still prints so the sites stay named).
bool tex1640RegEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_TEX1640_REG");
    return !e || e[0] != '0';
  }();
  return s_on;
}

extern "C" void __imp__sub_82616C40(PPCContext& __restrict ctx, uint8_t* base);
void sub_82616C40(PPCContext& __restrict ctx, uint8_t* base) {
  API_OBS_PROLOGUE(s_texInitHdr);
  const uint32_t holder = ctx.r3.u32;
  const uint32_t w = ctx.r4.u32 & 0xFFFFu, h = ctx.r5.u32 & 0xFFFFu,
                 levels = ctx.r6.u32 & 0xFFu, usage = ctx.r7.u32,
                 fmt = ctx.r8.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82616C40(ctx, base);
  const uint32_t tex = ctx.r3.u32;
  // holder[2] = the pixel allocation — the fetch-constant base the bind path
  // keys on ([S6_TEXUP]/s_texSlot), i.e. the join key from this create to its
  // binds. The k1-r5 witness had no dims and no pixel VA; this line is that gap.
  uint32_t pixVA = 0;
  if (holder) safeU32(holder + 8u, pixVA);
  if (n <= 16 || loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[TEX1640] #%llu %ux%u lv=%u usage=%08X fmt=%08X lr=%08X -> hdr=%08X "
        "pix=%08X reg=%d",
        (unsigned long long)n, w, h, levels, usage, fmt, lr, tex, pixVA,
        tex1640RegEnabled() ? 1 : 0);
  }
  namespace tid = sr::kernel::texid;
  // iter-456n (sixth adjudication, bar d): a CREATE at this pixel address ends
  // whatever object used to live there. Ownership is generation-scoped, so this
  // is what stops a destroyed texture's Unlock claim being inherited by the next
  // object at the same physical address — including the case a descriptor
  // comparison alone cannot see, where the newcomer has the SAME descriptor.
  // Raised from the pixel VA this site already reads, normalized owner-side.
  // The 1640 route's holder already exposes the pixel VA directly; prefer the
  // header's own fetch constant when it is readable so BOTH create routes
  // express identity through exactly one decode, and fall back to the holder
  // field otherwise.
  if (tex1640RegEnabled()) {
    const uint32_t hdrKey = lifecyclePixelKeyFromHeader(tex);
    const uint32_t lifeKey = hdrKey ? hdrKey : pixVA;
    if (lifeKey) {
      static std::atomic<uint64_t> s_life1640{0};
      const uint64_t ln = s_life1640.fetch_add(1, std::memory_order_relaxed) + 1;
      if (ln <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[TEXLIFE] create#%llu route=82616C40 hdr=%08X pixelKey=%08X "
            "(fromHeader=%d holderPix=%08X)",
            (unsigned long long)ln, tex, lifeKey, hdrKey ? 1 : 0, pixVA);
      sr::gfx::hle::noteTextureObjectEvent(lifeKey, /*reason=*/0u);
    }
  }
  if (tex1640RegEnabled() && tex >= 0x1000u) {
    // Registration mirrors the TexCreate hook exactly (same upsert body, same
    // domain-ordered locking discipline) so the two create entries stay one
    // semantics. depth=1: this entry point constructs 2D headers only.
    auto behaviorUpsert = [&]() {
      const size_t sz = s_texMap.size();
      if (sz < kTexMapCap)
        s_texMap[tex] = TexRec{w, h, 1u, levels, usage, fmt, {}};
      if (productionGateEnabled())
        tid::classifyBehaviorCreate(s_behCreateCounters, tex, sz, kTexMapCap);
    };
    if (productionGateEnabled()) {
      tid::DomainOrderedLock<std::mutex> bg(tid::LockDomain::Behavior, s_texMu);
      if (bg.locked()) behaviorUpsert();
    } else {
      std::lock_guard<std::mutex> lk(s_texMu);
      behaviorUpsert();
    }
    if (productionGateEnabled())
      tid::domain::registerCreate(tex, w, h, fmt, usage & 0xFu);
  }
}

// ── iter-442 S1 CENSUS: the rest of the first-light API set ──────────────────
// Observe-only (log + delegate). Purpose: measure the REAL call stream before
// writing any translation — commands/frame sizes the queue, the distinct
// producer thread set decides whether per-draw batching is mandatory, and the
// Begin/End-vs-UP split decides which draw entry points must be hooked at all.
// Every one of these was unhooked when kernel/hle_d3d9.cpp was deleted.
// REGISTRY CONTRACT (iter-442, learned the hard way): the first macro argument
// MUST be the full `sub_XXXXXXXX` symbol. scripts/gen_override_manifest.py DOES
// expand macro-defined overrides, but its MACRO_CALL_RE only matches a call
// whose first argument is that symbol. A macro taking a bare address and pasting
// `sub_##ADDR` compiles and links fine while being INVISIBLE to the override
// registry — 11 overrides silently unregistered, which is precisely the blind
// spot the registry exists to prevent (iter-395). Verified: with this form,
// gen_override_manifest.py lists every one.
// MIRROR (iter-442 S2): each hook now also publishes a typed RenderCommand.
// Behaviour is still unchanged — every hook delegates to __imp__ and the render
// thread only counts — so this is a pure refactor whose acceptance test is that
// [HLE_VIDEO] per-type counts EQUAL the [API_OBS] counters.
// ── iter-449: THE INDEXED PRODUCER (SR_S6_IDX, default OFF) ──────────────────
// sub_820FC000 is DrawIndexedVertices(dev, primType, baseVertexIndex,
// startIndex, indexCount) — r5 SIGNED, r6 in INDEX UNITS, r7 in indices. All
// three were proven from the binary's own packet builder, not cited
// (MIGRATION_API_HLE §"THE REFERENCE'S BUFFER MODEL"): r5 lands in
// VGT_INDX_OFFSET, r6 scales by the index size onto ib[+24], r7 becomes
// VGT_DRAW_INITIATOR num_indices. The guest SPLITS r7 > 0xFFFF into multiple
// hardware draws inside this function — the hook sees the caller's FULL r7 and
// publishes ONE draw; replicating the split is explicitly forbidden.
//
// WHY GATHER, NOT A HOST INDEX BUFFER: the backend CPU-expands packed guest
// vertices into float4 attributes per draw (hleSubmitDraw), so a D3D12 IBV
// would index into a stream that is rebuilt per draw anyway. Resolving the
// indices HERE — vertex (baseVertex + idx) copied per index, in draw order —
// turns the draw into exactly the shape the executor already trusts (start=0,
// count = genuine vertex count) with topology preserved 1:1: an indexed
// list/strip of N indices IS the non-indexed list/strip of the N gathered
// vertices, winding parity included. The per-draw gather is this slice's
// measuring instrument; the (physBase,size)-keyed residency cache
// (MIGRATION §3) is the named successor that removes the copy.
//
// Refusal discipline: every gate-on attempt ends in exactly one counter
// (conservation printed on [S6_IDX]). Refused draws still publish the census
// mirror below, so [HLE_MIRROR] hle==api stays exact either way.
namespace {
bool idxGatherPublish(PPCContext& ctx) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32;
  const int32_t baseVertex = (int32_t)ctx.r5.u32;   // SIGNED — reference video.cpp:961
  const uint32_t startIndex = ctx.r6.u32, idxCount = ctx.r7.u32;
  s_idxSeen.fetch_add(1, std::memory_order_relaxed);
  if (!idxCount) { s_idxZeroCount++; return false; }

  // 1) The declaration names the stream (element 0, same rule as the
  //    DrawVertices producer — the backend itself refuses a CONSUMED
  //    second-stream element as MultiStream, and it is the only side that
  //    knows which elements the VS consumes).
  uint32_t declStream = 0;
  {
    const uint32_t decl = sr::kernel::readGuestU32BE(dev + 11536u);
    uint32_t nE = 0, d0 = 0;
    if (decl >= 0x1000u) safeU32(decl + 0x18u, nE);
    if (!nE || nE > 32u || !safeU32(decl + 0x34u, d0)) {
      s_idxNoDecl++;
      return false;
    }
    declStream = (d0 >> 16) & 3u;
  }
  // Stride is PER STREAM: dev+12256 is a byte array indexed by stream (BE).
  const uint32_t stride = ((sr::kernel::readGuestU32BE(dev + 12256u) >>
                            (24u - 8u * declStream)) & 0xFFu) * 4u;
  if (!stride) { s_idxZeroCount++; return false; }
  // The gather size is known BEFORE any index is read, so the cap check runs
  // first: it bounds the pass-1 scan cost by construction, and with
  // stride >= 4 it caps idxCount at 256K — which is also what keeps every
  // byte-count below a uint32 wrap. Refused and counted, never truncated.
  const uint64_t want = (uint64_t)idxCount * stride;
  if (want > sr::gfx::hle::kArenaSlotBytes) { s_idxOverCap++; return false; }

  // 2) The vertex fetch constant, slot = 95 - stream (iter-435): d0 = physBase
  //    | type (3 = kVertex), d1 = endian | sizeDwords<<2.
  uint32_t vbPhys = 0, vbEndian = 0;
  uint64_t vbSize = 0;
  {
    const uint32_t slotVA = dev + 1024u + 8u * (95u - declStream);
    uint32_t d0 = 0, d1 = 0;
    if (!safeU32(slotVA, d0) || !safeU32(slotVA + 4u, d1) || (d0 & 3u) != 3u) {
      s_idxSlotMiss++;
      return false;
    }
    vbPhys = d0 & ~3u;
    vbEndian = d1 & 3u;
    vbSize = (uint64_t)((d1 >> 2) & 0xFFFFFFu) * 4u;
  }

  // 3) The index buffer, from the object SetIndices stored at dev+12164
  //    (ENGINE_MAP: format = ib[0] bit31 [set = 32-bit], endian = bits 29-30,
  //    base = ib+24, a preformatted fetch-constant dword0 — mask the low
  //    type bits before using it as an address).
  const uint32_t ib = sr::kernel::readGuestU32BE(dev + 12164u);
  uint32_t ib0 = 0, ib24 = 0;
  if (ib < 0x1000u || !safeU32(ib + 0u, ib0) || !safeU32(ib + 24u, ib24)) {
    s_idxIbBad++;
    return false;
  }
  const bool fmt32 = (ib0 >> 31) != 0;
  const uint32_t idxEndian = (ib0 >> 29) & 3u;
  const uint32_t idxSize = fmt32 ? 4u : 2u;
  if (fmt32) s_idxFmt32.fetch_add(1, std::memory_order_relaxed);
  // Guest-CPU-written indices are BE, carried by endian op 1 (8in16) for
  // 16-bit and 2 (8in32) for 32-bit; endian 0 means the memory is already
  // little-endian. Any other combination is one this title has not shown —
  // refuse and COUNT rather than guess a byte order (a wrong guess renders
  // shredded geometry that still counts as `recorded`).
  if (!(idxEndian == 0u || (!fmt32 && idxEndian == 1u) ||
        (fmt32 && idxEndian == 2u))) {
    s_idxEndianOdd++;
    return false;
  }
  const uint32_t idxVA =
      0x80000000u | (((ib24 & ~3u) + startIndex * idxSize) & 0x1FFFFFFFu);
  const uint64_t idxBytes = (uint64_t)idxCount * idxSize;
  if (!sr::kernel::isGuestVAInRange(idxVA + (uint32_t)idxBytes)) {
    s_idxIbBad++;
    return false;
  }
  const uint8_t* ip = sr::kernel::guestVAToHost(idxVA);
  if (!ip) { s_idxIbBad++; return false; }
  // iter-458 (Astra 029 B): SR_PHYS_RESOLVE - ownership picks the window when
  // it names one; the head-64 probe and the range rescue below are the legacy
  // rule for the rest.
  const int ibRes = sr::kernel::physwin::resolveOwned(
      sr::kernel::physwin::kRsIdxIb, idxVA, idxBytes);
  if (ibRes == 0xA)
    if (const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u | (idxVA & 0x1FFFFFFFu)))
      ip = alt;
  // iter-425 platform accommodation, same as the vertex path: the harness does
  // not fold the physical windows, so data written through an 0xA-class VA
  // reads all-zero at the 0x8 window. Read-side only.
  if (!ibRes) {
    bool zero8 = true;
    for (uint64_t b = 0; b < 64u && b < idxBytes; b++)
      if (ip[b]) { zero8 = false; break; }
    if (zero8) {
      if (const uint8_t* alt = sr::kernel::guestVAToHost(
              0xA0000000u | (idxVA & 0x1FFFFFFFu))) {
        bool zeroA = true;
        for (uint64_t b = 0; b < 64u && b < idxBytes; b++)
          if (alt[b]) { zeroA = false; break; }
        if (!zeroA) ip = alt;
      }
    }
  }

  // 4) Pass 1: min/max + the cut sentinel. All-ones is the Xenos strip-restart
  //    index; a gathered stream cannot express a restart, so refuse and count
  //    rather than draw a phantom triangle across the cut.
  const uint32_t cut = fmt32 ? 0xFFFFFFFFu : 0xFFFFu;
  auto readIdx = [&](uint32_t i) -> uint32_t {
    const uint8_t* p = ip + (size_t)i * idxSize;
    if (fmt32)
      return idxEndian ? ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                          (uint32_t(p[2]) << 8) | p[3])
                       : ((uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) |
                          (uint32_t(p[1]) << 8) | p[0]);
    return idxEndian ? ((uint32_t(p[0]) << 8) | p[1])
                     : ((uint32_t(p[1]) << 8) | p[0]);
  };
  // ── iter-449d: RESTART STRIPS, PROVEN then CONVERTED ─────────────────────
  // The cut forensics settled the premise from the guest's own register
  // file: reg 0x2103 (VGT_MULTI_PRIM_IB_RESET_INDX, 0x2100-run shadow at
  // dev+10328) reads 0xFFFF on every cut draw, every one is prim 6, and the
  // index stream is textbook restart-separated substrips (mostly 4-index
  // quads-as-strips sharing one large IB). Restarts are REAL — and a
  // restart-strip converts EXACTLY to a triangle list: substrip triangle t
  // is (t,t+1,t+2) with the winding swap on odd t (the D3D strip parity
  // rule), parity resetting per substrip. No approximation anywhere; the
  // command publishes as prim 4 with the converted count. A cut on any
  // NON-strip prim has never been observed (p4=0 across the forensics roll)
  // and stays REFUSED and logged — on a list a cut index is a decode
  // anomaly, not a topology.
  // iter-449d, model CORRECTED TWICE by its own instruments:
  //  * conv1: 53,728 prim-4 LISTS carry the all-ones value too — the
  //    forensics' "p4=0" was first-8 sampling bias, my misread of my own
  //    instrument.
  //  * conv2: the "list segments assemble floor(len/3) triangles" reading
  //    was REFUTED by its own kill switch (convRem = 3,092,169 dropped
  //    indices against 240,043 conversions), while every logged p4 cut draw
  //    has count %% 3 == 0 — the WHOLE draw is triangle-aligned, which
  //    segmentation would destroy.
  // THE SETTLED MODEL: in a LIST, 0xFFFF is a REAL VERTEX INDEX into a
  // >=65536-vertex mesh (the range check remains the honest guard); only
  // STRIPS have restarts — proven by the armed reg 0x2103 shadow AND the
  // 4-index substrip data shape. The one silent ambiguity — a >=65536-vertex
  // STRIP whose 0xFFFF could be either — is counted (`ambig`), not guessed
  // silently.
  const bool stripCuts = (prim == 6u);
  uint32_t minIdx = ~0u, maxIdx = 0, nCuts = 0, outTris = 0;
  const auto pass1 = [&] {
    minIdx = ~0u; maxIdx = 0; nCuts = 0; outTris = 0;
    uint32_t runLen = 0;
    for (uint32_t i = 0; i < idxCount; i++) {
      const uint32_t v = readIdx(i);
      if (stripCuts && v == cut) {
        nCuts++;
        runLen = 0;
        continue;
      }
      runLen++;
      if (prim == 6u && runLen >= 3u) outTris++;
      if (v < minIdx) minIdx = v;
      if (v > maxIdx) maxIdx = v;
    }
  };
  pass1();

  // ── iter-450i WINDOW FIX (SR_S6_WINFIX, default ON, =0 keeps the A/B) ────
  // PROVEN by [S6_RANGEA] (14/14 samples, boot era through FE): the IB's GPU
  // address is PHYSICAL; when the buffer was written through the 0xA window,
  // the 0x8-window pages hold unrelated VIRTUAL data (heap pointers, ASCII)
  // and pass 1 computes garbage ranges — 99.96% of every "range" refusal.
  // The zero8 probe above only rescues ALL-ZERO misreads. On a failed range
  // check, rerun pass 1 through the 0xA window and accept it ONLY if the
  // recomputed range self-validates against the declared VB size (the
  // structural check garbage cannot pass). Conservation stays exact: a
  // rescued draw publishes; both-windows-fail refuses once, as before.
  bool ibFromA = false;
  static const bool s_winFix = [] {
    const char* e = std::getenv("SR_S6_WINFIX");
    return !e || e[0] != '0';
  }();
  {
    const int64_t lo0 = (int64_t)baseVertex + (int64_t)minIdx;
    const int64_t hi0 = (int64_t)baseVertex + (int64_t)maxIdx;
    if (s_winFix && !ibRes &&
        (minIdx > maxIdx || lo0 < 0 ||
         (uint64_t)(hi0 + 1) * stride > vbSize)) {
      if (const uint8_t* alt = sr::kernel::guestVAToHost(
              0xA0000000u | (idxVA & 0x1FFFFFFFu))) {
        if (alt != ip) {
          const uint8_t* saved = ip;
          ip = alt;
          pass1();
          const int64_t loA = (int64_t)baseVertex + (int64_t)minIdx;
          const int64_t hiA = (int64_t)baseVertex + (int64_t)maxIdx;
          if (minIdx <= maxIdx && loA >= 0 &&
              (uint64_t)(hiA + 1) * stride <= vbSize) {
            ibFromA = true;
            s_idxWinFixed.fetch_add(1, std::memory_order_relaxed);
          } else {
            ip = saved;    // restore: forensics below must show the 0x8 read
            pass1();
          }
        }
      }
    }
  }

  // iter-458 (Astra 029 B): the independent check on a resolved index read -
  // the structural range test the legacy rescue uses, never ownership.
  bool legacyIbFromA = ibFromA;   // what follow-the-IB sees under the legacy rule
  if (ibRes) {
    const auto rangeOk = [&] {
      const int64_t loR = (int64_t)baseVertex + (int64_t)minIdx;
      const int64_t hiR = (int64_t)baseVertex + (int64_t)maxIdx;
      return minIdx <= maxIdx && loR >= 0 && (uint64_t)(hiR + 1) * stride <= vbSize;
    };
    const bool selOk = rangeOk();
    sr::kernel::physwin::resolveValidate(sr::kernel::physwin::kRsIdxIb, selOk);
    // Astra 030: the SAME range predicate on the window the legacy rule would
    // have read (head-64 probe, then the range rescue). pass1 rewrites only its
    // own locals; the selected window's pass is restored below.
    const uint8_t* const selP = ip;
    const uint8_t* p8i = sr::kernel::guestVAToHost(0x80000000u | (idxVA & 0x1FFFFFFFu));
    const uint8_t* pAi = sr::kernel::guestVAToHost(0xA0000000u | (idxVA & 0x1FFFFFFFu));
    const auto okAt = [&](const uint8_t* p) {
      if (!p) return false;
      ip = p;
      pass1();
      return rangeOk();
    };
    int lw = 8;
    if (p8i && pAi) {
      bool z8 = true, zA = true;
      for (uint64_t b = 0; b < 64u && b < idxBytes; b++) {
        if (p8i[b]) z8 = false;
        if (pAi[b]) zA = false;
      }
      if (z8 && !zA) lw = 0xA;
    }
    const bool ok8 = okAt(p8i), okA = okAt(pAi);
    if (lw == 8 && s_winFix && !ok8 && okA) { lw = 0xA; legacyIbFromA = true; }
    ip = selP;
    pass1();
    sr::kernel::physwin::resolveCompare(sr::kernel::physwin::kRsIdxIb, ibRes, lw, selOk,
                                        lw == 0xA ? okA : ok8);
  }

  // iter-458 H-WIN (observe-only): which window did the index read use?
  // Astra 043: the draw context rides on every sample of this draw (idx-ib,
  // then idx-vb below); observe-only, never changes the read.
  if (sr::kernel::physwin::censusEnabled()) {
    sr::kernel::physwin::DrawCtx dc;
    dc.presentId = sr::gfx::hle::presentCount();
    dc.prim = prim; dc.stride = stride; dc.count = idxCount; dc.startIndex = startIndex;
    dc.minIdx = minIdx; dc.maxIdx = maxIdx; dc.baseVertex = baseVertex;
    dc.idx32 = fmt32 ? 1 : 0; dc.idxEndian = (uint8_t)idxEndian;
    dc.vbEndian = (uint8_t)vbEndian;
    sr::kernel::physwin::setDrawContext(dc, /*newDraw=*/true);
  }
  if (sr::kernel::physwin::censusEnabled())
    sr::kernel::physwin::census(sr::kernel::physwin::kIdxIb, idxVA,
                                (uint32_t)idxBytes,
                                ip == sr::kernel::guestVAToHost(idxVA));

  const bool convert = (nCuts != 0);
  if (convert &&
      ((int64_t)baseVertex + 0x10000) * (int64_t)stride <= (int64_t)vbSize)
    s_idxCutAmbig.fetch_add(1, std::memory_order_relaxed);
  if (convert && (!outTris || minIdx > maxIdx)) { s_idxZeroCount++; return false; }
  // The gathered stream's OWN shape: a converted strip becomes a triangle
  // LIST of 3*outTris vertices; everything else keeps its count and prim.
  const uint32_t outPrim = convert ? 4u : prim;
  const uint32_t outVerts = convert ? 3u * outTris : idxCount;

  // 5) The referenced vertex range must sit inside the fetch constant's own
  //    declared size — the slot rule the DrawVertices producer enforces, plus
  //    the signed base: lo < 0 would gather from before the buffer.
  const int64_t lo = (int64_t)baseVertex + (int64_t)minIdx;
  const int64_t hi = (int64_t)baseVertex + (int64_t)maxIdx;
  if (lo < 0 || (uint64_t)(hi + 1) * stride > vbSize) {
    s_idxRangeBad++;
    // ── iter-449e RANGE FORENSICS: the 565K/32% class names its shape ──────
    // The refused draw's referenced span vs the stream-0 slot it was judged
    // against, PLUS the full decl stream map and the neighbouring fetch
    // slots — if a failing draw's decl spans streams and a NEIGHBOUR slot
    // covers the span, "multi-stream" is confirmed and the fix is
    // stream-aware gathering; if NO slot covers it, the slot-size reading
    // itself is the suspect.
    static std::atomic<uint64_t> s_rgShown{0};
    const uint64_t rk = s_rgShown.fetch_add(1, std::memory_order_relaxed) + 1;
    // iter-450i: first-8 burns out ~90 s in (pre-FE); the modulo keeps
    // samples flowing through the FE era's own refusals.
    if (rk <= 8 || (rk % 30000u) == 0u) {
      char dm[120]; size_t dmo = 0;
      {
        const uint32_t decl = sr::kernel::readGuestU32BE(dev + 11536u);
        uint32_t nE = 0;
        safeU32(decl + 0x18u, nE);
        for (uint32_t e = 0; e < nE && e < 12u && dmo + 8 < sizeof(dm); e++) {
          uint32_t d0 = 0;
          safeU32(decl + 0x34u + kDeclElemBytes * e, d0);
          dmo += (size_t)snprintf(dm + dmo, sizeof(dm) - dmo, " s%u@%u",
                                  (d0 >> 16) & 3u, d0 & 0xFFFFu);
        }
      }
      char sl[160]; size_t slo = 0;
      for (uint32_t s = 95; s >= 92 && slo + 24 < sizeof(sl); s--) {
        uint32_t d0 = 0, d1 = 0;
        safeU32(dev + 1024u + 8u * s, d0);
        safeU32(dev + 1024u + 8u * s + 4u, d1);
        slo += (size_t)snprintf(sl + slo, sizeof(sl) - slo,
                                " s%u{t%u sz=%u}", s, d0 & 3u,
                                ((d1 >> 2) & 0xFFFFFFu) * 4u);
      }
      // The raw stream head discriminates every candidate mechanism at once:
      // garbage looks scattered, 32-bit-read-as-16 alternates 0000/value,
      // section padding shows runs of FFFF between plausible values.
      char rw[110]; size_t rwo = 0;
      for (uint32_t j = 0; j < idxCount && j < 12u && rwo + 5 < sizeof(rw); j++)
        rwo += (size_t)snprintf(rw + rwo, sizeof(rw) - rwo, " %04X",
                                readIdx(j) & 0xFFFFu);
      // iter-449g: the FULL 8-dword IB object header + the caller's LR.
      // The ..C50/..950 allocator tell says two OBJECT LAYOUTS; the header
      // dump shows where the real data pointer lives in the failing one, and
      // the LR names which of the six call sites feeds it.
      char hd[90]; size_t hdo = 0;
      for (uint32_t j = 0; j < 8u && hdo + 10 < sizeof(hd); j++) {
        uint32_t w = 0;
        safeU32(ib + 4u * j, w);
        hdo += (size_t)snprintf(hd + hdo, sizeof(hd) - hdo, " %08X", w);
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_RANGE] #%llu ibObj=%08X hdr:%s | callerLR=%08X",
          (unsigned long long)rk, ib, hdo ? hd : " -",
          (uint32_t)ctx.lr);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_RANGE] #%llu prim=%u count=%u baseVtx=%d startIdx=%u "
          "ib0=%08X ibPhys=%08X idx=[%u..%u] stride=%u NEED=%lld B stream=%u "
          "vbSize=%llu | decl:%s | slots:%s | raw:%s",
          (unsigned long long)rk, prim, idxCount, baseVertex, startIndex,
          ib0, ib24 & ~3u, minIdx, maxIdx, stride,
          (long long)((hi + 1) * (int64_t)stride), declStream,
          (unsigned long long)vbSize, dmo ? dm : " -", slo ? sl : " -",
          rwo ? rw : " -");
      // ── iter-450i 0xA-WINDOW DISCRIMINATOR ─────────────────────────────
      // The zero8 alias probe (iter-425, above) rescues only ALL-ZERO
      // 0x8-window misreads. A physically-addressed IB whose 0x8-window
      // page is occupied by unrelated VIRTUAL data reads as foreign bytes
      // (heap pointers, ASCII) and lands here as a "range" refusal. Read
      // the SAME span through the 0xA window and recompute the range —
      // sane indices there convict the WINDOW, not the game.
      {
        const uint8_t* savedIp = ip;
        uint32_t minA = ~0u, maxA = 0;
        char rwA[110]; size_t rwoA = 0;
        int haveA = 0;
        if (const uint8_t* alt = sr::kernel::guestVAToHost(
                0xA0000000u | (idxVA & 0x1FFFFFFFu))) {
          haveA = 1;
          ip = alt;   // readIdx captures ip by reference
          for (uint32_t i = 0; i < idxCount; i++) {
            const uint32_t v = readIdx(i);
            if (stripCuts && v == cut) continue;
            if (v < minA) minA = v;
            if (v > maxA) maxA = v;
          }
          for (uint32_t j = 0; j < idxCount && j < 12u && rwoA + 5 < sizeof(rwA); j++)
            rwoA += (size_t)snprintf(rwA + rwoA, sizeof(rwA) - rwoA, " %04X",
                                     readIdx(j) & 0xFFFFu);
          ip = savedIp;
        }
        const int64_t hiA = (int64_t)baseVertex + (int64_t)maxA;
        const int fitsA =
            haveA && minA <= maxA && (int64_t)baseVertex + (int64_t)minA >= 0 &&
            (uint64_t)(hiA + 1) * stride <= vbSize;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_RANGEA] #%llu haveA=%d idxA=[%u..%u] needA=%lld B "
            "vbSize=%llu fitsA=%d | rawA:%s",
            (unsigned long long)rk, haveA, minA, maxA,
            (long long)((hiA + 1) * (int64_t)stride),
            (unsigned long long)vbSize, fitsA, rwoA ? rwA : " -");
      }
    }
    // iter-449f: park this refusal for the Swap-time re-read (cheap ledger,
    // overwrites round-robin — the QUESTION is per-class, not per-draw).
    {
      std::lock_guard<std::mutex> lk(s_idxRecheckMu);
      IdxRecheck& r = s_idxRecheck[s_idxRecheckNext];
      s_idxRecheckNext = (s_idxRecheckNext + 1u) % 4u;
      r.dev = dev;
      r.ibVA = idxVA;
      r.count = idxCount;
      r.prim = prim;
      r.vbSizeAtEmit = vbSize;
      r.slotVA = dev + 1024u + 8u * (95u - declStream);
      for (uint32_t j = 0; j < 8u; j++)
        r.emitWords[j] = (uint16_t)(j < idxCount ? readIdx(j) & 0xFFFFu : 0u);
      r.live = true;
    }
    return false;
  }
  const uint32_t vbVA = 0x80000000u | (vbPhys & 0x1FFFFFFFu);
  if (!sr::kernel::isGuestVAInRange(
          vbVA + (uint32_t)((uint64_t)(hi + 1) * stride))) {
    s_idxRangeBad++;
    return false;
  }
  const uint8_t* vp = sr::kernel::guestVAToHost(vbVA);
  if (!vp) { s_idxRangeBad++; return false; }
  // iter-450i: the IB and VB of one mesh share an allocation regime — when
  // the INDEX read was only valid through the 0xA window (ibFromA), the
  // vertex data is physical too, and its 0x8-window bytes are the same
  // foreign-data class the zero8 probe below cannot catch. Follow the IB's
  // winning window; the zero8 probe stays as the fallback for the rest.
  // iter-458 (Astra 029 B): SR_PHYS_RESOLVE - ownership of the consumed span
  // decides when it names one window; follow-the-IB and the zero8 probe are
  // the legacy rule for the rest.
  const int vbRes = sr::kernel::physwin::resolveOwned(
      sr::kernel::physwin::kRsIdxVb, (uint32_t)(vbPhys + (uint64_t)lo * stride),
      (uint64_t)(hi - lo + 1) * stride);
  if (vbRes) {
    if (vbRes == 0xA)
      if (const uint8_t* alt = sr::kernel::guestVAToHost(
              0xA0000000u | (vbVA & 0x1FFFFFFFu)))
        vp = alt;
    const uint8_t* v8 = sr::kernel::guestVAToHost(0x80000000u | (vbVA & 0x1FFFFFFFu));
    const uint8_t* vA = sr::kernel::guestVAToHost(0xA0000000u | (vbVA & 0x1FFFFFFFu));
    const uint64_t off = (uint64_t)lo * stride;
    const uint64_t n0 = (uint64_t)(hi - lo + 1) * stride;
    const uint64_t n = n0 < 256u ? n0 : 256u;
    const auto plausibleAt = [&](int w) {
      const uint8_t* s = w == 0xA ? vA : v8;
      const uint8_t* o = w == 0xA ? v8 : vA;
      return resolvedBytesPlausible(s ? s + off : nullptr, o ? o + off : nullptr, n);
    };
    const bool selOk = plausibleAt(vbRes);
    sr::kernel::physwin::resolveValidate(sr::kernel::physwin::kRsIdxVb, selOk);
    // Astra 030: the legacy rule - follow the IB's legacy rescue, else the
    // zero8 probe on the first referenced vertex.
    int lw = 8;
    if (legacyIbFromA) {
      if (vA) lw = 0xA;
    } else if (v8 && vA) {
      bool z8 = true, zA = true;
      for (uint32_t b = 0; b < 64u && b < stride; b++) {
        if (v8[off + b]) z8 = false;
        if (vA[off + b]) zA = false;
      }
      if (z8 && !zA) lw = 0xA;
    }
    sr::kernel::physwin::resolveCompare(sr::kernel::physwin::kRsIdxVb, vbRes, lw, selOk,
                                        lw == vbRes ? selOk : plausibleAt(lw));
  } else if (ibFromA) {
    if (const uint8_t* alt = sr::kernel::guestVAToHost(
            0xA0000000u | (vbVA & 0x1FFFFFFFu))) {
      vp = alt;
      s_idxVbFromA.fetch_add(1, std::memory_order_relaxed);
    }
  } else {  // 0xA-window alias, judged on the first referenced vertex.
    const uint8_t* first = vp + (uint64_t)lo * stride;
    bool zero8 = true;
    for (uint32_t b = 0; b < 64u && b < stride; b++)
      if (first[b]) { zero8 = false; break; }
    if (zero8) {
      if (const uint8_t* alt = sr::kernel::guestVAToHost(
              0xA0000000u | (vbVA & 0x1FFFFFFFu))) {
        const uint8_t* af = alt + (uint64_t)lo * stride;
        bool zeroA = true;
        for (uint32_t b = 0; b < 64u && b < stride; b++)
          if (af[b]) { zeroA = false; break; }
        if (!zeroA) vp = alt;
      }
    }
  }
  // iter-458 H-WIN (observe-only): the consumed vertex span [lo, hi].
  if (sr::kernel::physwin::censusEnabled())
    sr::kernel::physwin::census(
        sr::kernel::physwin::kIdxVb,
        (uint32_t)(vbPhys + (uint64_t)lo * stride),
        (uint32_t)((uint64_t)(hi - lo + 1) * stride),
        vp == sr::kernel::guestVAToHost(vbVA));

  // 6) Pass 2: gather in draw order, then ONE endian pass over the host copy
  //    (never in guest memory — the guest still owns and reads it). Endian 2
  //    (8IN32) is the only class the DrawVertices path swaps; identical here.
  //    The converted-strip output can EXCEED the input span (a 4-index quad
  //    substrip emits 6 list vertices, a 3-index one 3-for-4), so the cap is
  //    re-checked against the OUTPUT size — refused and counted, never
  //    truncated, same as everywhere else.
  const uint64_t wantOut = (uint64_t)outVerts * stride;
  if (wantOut > sr::gfx::hle::kArenaSlotBytes) { s_idxOverCap++; return false; }
  static thread_local std::vector<uint8_t> t_gather;
  if (t_gather.size() < wantOut) t_gather.resize((size_t)wantOut);
  uint8_t* dst = t_gather.data();
  auto fetchVert = [&](uint32_t idx, uint32_t outSlot) {
    std::memcpy(dst + (size_t)outSlot * stride,
                vp + (size_t)((int64_t)baseVertex + (int64_t)idx) * stride,
                stride);
  };
  if (convert) {
    // Substrip walk (strips only): triangle t of a substrip is (t,t+1,t+2)
    // even / (t+1,t,t+2) odd (the D3D parity rule), parity resetting at each
    // cut.
    uint32_t o = 0, rl = 0, p2 = 0, p1 = 0;
    for (uint32_t i = 0; i < idxCount; i++) {
      const uint32_t v = readIdx(i);
      if (v == cut) { rl = 0; continue; }
      rl++;
      if (rl >= 3u) {
        const uint32_t t = rl - 3u;
        fetchVert((t & 1u) ? p1 : p2, o++);
        fetchVert((t & 1u) ? p2 : p1, o++);
        fetchVert(v, o++);
      }
      p2 = p1;
      p1 = v;
    }
  } else {
    for (uint32_t i = 0; i < idxCount; i++) fetchVert(readIdx(i), i);
  }
  if (vbEndian == 2u) {
    for (uint64_t i = 0; i + 4 <= wantOut; i += 4) {
      std::swap(dst[i + 0], dst[i + 3]);
      std::swap(dst[i + 1], dst[i + 2]);
    }
  }
  const uint32_t va = sr::gfx::hle::poolWrite(dst, (uint32_t)wantOut);
  if (va == sr::gfx::hle::kArenaFull) { s_idxPoolFull++; return false; }
  // iter-449c: only a genuinely ARENA-routed write (no tag bit at all) joins
  // the movie-gate subtraction; geo-pool writes (bit30) must not.
  if (!(va & (sr::gfx::hle::kPoolBlockFlag | sr::gfx::hle::kPoolGeoFlag)))
    s_idxArenaWrites.fetch_add(1, std::memory_order_relaxed);

  {
    // STATE FIRST, then the draw — the publish-order contract (without it the
    // draw inherits the previous draw's decl/constants/registers/shaders).
    sr::gfx::hle::SeqScope seq;
    publishDrawState(dev);
    MIRROR1(DrawIndexedVertices,
            draw.device = dev;
            // Honestly named: the gathered stream IS the draw order, so the
            // first vertex is index 0 and `count` is a genuine vertex count.
            // A converted restart-strip publishes ITS OWN shape — prim 4 with
            // the triangle-list count — because that is what the stream now
            // is. vertBytes > 0 on this type is the executor's witness that
            // THIS producer wrote the command (the census mirror keeps
            // vertBytes=0).
            // iter-450O: the strip-conversion MARKER rides bit 8 so the
            // replayed fixture can A/B "skip/only converted strips" — the
            // cheapest KILLS-IT for the angular-shard fracture suspect.
            c.draw.primType =
                outPrim |
                (convert ? sr::gfx::hle::kPrimStripConvertedBit : 0u);
            c.draw.start = 0u;
            c.draw.count = outVerts;
            c.draw.vertArena = va;
            c.draw.vertBytes = (uint32_t)wantOut;
            c.draw.stride = stride);
  }
  s_idxPublished.fetch_add(1, std::memory_order_relaxed);
  if (convert) s_idxStripConv.fetch_add(1, std::memory_order_relaxed);
  {
    uint64_t prev = s_idxMaxCount.load(std::memory_order_relaxed);
    while (idxCount > prev &&
           !s_idxMaxCount.compare_exchange_weak(prev, idxCount)) {}
  }
  static std::atomic<uint64_t> s_shown{0};
  const uint64_t k = s_shown.fetch_add(1, std::memory_order_relaxed) + 1;
  if (k <= 6) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_IDX] gather #%llu prim=%u->%u count=%u->%u baseVtx=%d "
        "startIdx=%u fmt=%u endian=%u ibPhys=%08X vbPhys=%08X stride=%u "
        "idx=[%u..%u] bytes=%llu -> va=%08X%s",
        (unsigned long long)k, prim, outPrim, idxCount, outVerts, baseVertex,
        startIndex, fmt32 ? 32u : 16u, idxEndian, ib24 & ~3u, vbPhys, stride,
        minIdx, maxIdx, (unsigned long long)wantOut, va,
        convert ? " (restart-strip converted)" : "");
    // iter-449g: the WORKING contrast for the ..C50/..950 layout question —
    // same 8-dword header + caller LR the [S6_RANGE] probe prints.
    char hd[90]; size_t hdo = 0;
    for (uint32_t j = 0; j < 8u && hdo + 10 < sizeof(hd); j++) {
      uint32_t w = 0;
      safeU32(ib + 4u * j, w);
      hdo += (size_t)snprintf(hd + hdo, sizeof(hd) - hdo, " %08X", w);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_IDX] gather #%llu ibObj=%08X hdr:%s | callerLR=%08X",
        (unsigned long long)k, ib, hdo ? hd : " -", (uint32_t)ctx.lr);
  }
  return true;
}

// ── iter-458: THE INDEX VERTEX STREAM (reference video.cpp:4585-4650, 4913) ──
// A declaration carrying POSITION with usageIndex 1 names an INDEX STREAM: the
// game puts R32 indices in a second vertex stream and calls DrawVertices, and
// the console VS fetches the index from that stream and then fetches the mesh
// attributes BY THAT INDEX (Xenos manual indexing — the oracle draw #1017 in
// data/gpu_traces/menu_pressstart_454107ED_9415.xtr: slot 94 = k_32 stride 1,
// 900 B = 225 indices, slot 95 = the 36-B mesh; VGT auto-index, no DMA IB).
// The translated VS is input-assembler fed, so on the host the draw MUST be
// indexed with that stream as the index buffer — which is exactly what the
// reference does (CheckInstancing -> g_indexBufferView = the stream,
// drawIndexedInstanced(indexCount, primitiveCount / indexCount)). Our
// DrawVertices producer instead snapshotted stream 0 LINEARLY from start, so
// every such mesh rendered its vertex array in storage order: correct
// vertices, wrong triangles — the title backdrop's spikes and slabs. (iter-450O
// "POSITION1 acquitted" only checked that the DXIL never LOADS the input; the
// input is not the point, the topology is.)
//
// Same shape as idxGatherPublish: gather vertex[idx_i] in draw order into a
// pool span, publish start=0/count=apiCount. No restart handling — manual
// indexing has no cut semantics (VGT_MULTI_PRIM_IB_RESET_INDX applies to the
// DMA index path only), an all-ones index would just fetch out of range and
// is refused by the range check like any other. Instancing (count a multiple
// of the stream's index count) is carried by wrapping the index stream — see
// the block below; the per-instance transform is the VS's own arithmetic on
// the vertex index, which the translated shader now receives as SV_VertexID.
struct IdxStreamStats {
  std::atomic<uint64_t> seen{0}, published{0}, noSlot{0}, tooBig{0},
      instanced{0}, range{0}, unmapped{0}, poolFull{0}, instTotal{0},
      startNonZero{0};
};
IdxStreamStats s_idxStream;

bool idxStreamEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("SR_S6_IDXSTREAM");
    return e && e[0] == '1';
  }();
  return on;
}

// Returns true and fills vArena/vBytes when the gather published a span.
bool idxStreamGather(uint32_t dev, uint32_t prim, uint32_t start,
                     uint32_t count, uint32_t meshStream, uint32_t idxStream,
                     uint32_t meshStride, uint32_t& vArena, uint32_t& vBytes) {
  s_idxStream.seen.fetch_add(1, std::memory_order_relaxed);
  if (!count || !meshStride) return false;
  const uint8_t* fetchFile = sr::kernel::isGuestVAInRange(dev + 1024u + 768u)
                                 ? sr::kernel::guestVAToHost(dev + 1024u)
                                 : nullptr;
  if (!fetchFile) { s_idxStream.noSlot++; return false; }
  auto slotOf = [&](uint32_t stream, uint32_t& phys, uint32_t& endian,
                    uint64_t& size) {
    const uint8_t* e = fetchFile + 8u * (95u - (stream & 3u));
    const uint32_t d0 = (uint32_t(e[0]) << 24) | (uint32_t(e[1]) << 16) |
                        (uint32_t(e[2]) << 8) | uint32_t(e[3]);
    const uint32_t d1 = (uint32_t(e[4]) << 24) | (uint32_t(e[5]) << 16) |
                        (uint32_t(e[6]) << 8) | uint32_t(e[7]);
    if ((d0 & 3u) != 3u) return false;
    phys = (d0 & ~3u) & 0x1FFFFFFFu;
    endian = d1 & 3u;
    size = (uint64_t)((d1 >> 2) & 0xFFFFFFu) * 4u;
    return true;
  };
  uint32_t ibPhys = 0, ibEndian = 0, vbPhys = 0, vbEndian = 0;
  uint64_t ibSize = 0, vbSize = 0;
  if (!slotOf(idxStream, ibPhys, ibEndian, ibSize) ||
      !slotOf(meshStream, vbPhys, vbEndian, vbSize)) {
    s_idxStream.noSlot++;
    return false;
  }
  const uint64_t wantOut = (uint64_t)count * meshStride;
  if (wantOut > sr::gfx::hle::kArenaSlotBytes) { s_idxStream.tooBig++; return false; }
  // INSTANCING (reference video.cpp:4648 `drawIndexedInstanced(indexCount,
  // primitiveCount / indexCount)`; the console VS, oracle draw #1021 ucode:
  //   inst = trunc((vi + 0.5) / c4.x);  idx = vf1[vi - inst*c4.x + c4.y]
  //   a0 = 2*inst;  world = c[100+a0], c[101+a0]
  // with c4.x == ibSize/4 and c4.y == 0 on all 1,955 oracle index-stream
  // draws, 1,285 of them multi-instance). indexCount is the STREAM SIZE; the
  // vertex index runs 0..count-1 and wraps through the index stream every
  // indexCount vertices. The gather reproduces exactly that fetch sequence —
  // vertex i = mesh[idx[i % n]] — and the translated VS recovers `inst` from
  // SV_VertexID (Xenos r0.x), so the per-instance constants are its own
  // business. `start` is ignored as the reference ignores it; it reads 0 on
  // every draw so far and is counted if it ever does not.
  const uint32_t n = (uint32_t)(ibSize / 4u);
  if (!n) { s_idxStream.noSlot++; return false; }
  uint32_t wrapN = n;
  if (count > n) {
    if (count % n != 0) {
      // A ragged tail cannot be an instance count. Refused and named.
      s_idxStream.instanced++;
      return false;
    }
    s_idxStream.instTotal += count / n;
  } else {
    wrapN = count;   // one (possibly partial) instance
  }
  if (start) s_idxStream.startNonZero++;
  const uint64_t ibStart = 0;
  // Index bytes: physical, read through 0x8 with the 0xA alias probe used by
  // every other producer (the harness does not fold the windows).
  auto resolveWindow = [&](uint32_t phys, uint64_t off, uint64_t bytes)
      -> const uint8_t* {
    const uint32_t va8 = 0x80000000u | (uint32_t)((phys + off) & 0x1FFFFFFFu);
    if (!sr::kernel::isGuestVAInRange(va8 + (uint32_t)bytes)) return nullptr;
    const uint8_t* p = sr::kernel::guestVAToHost(va8);
    if (!p) return nullptr;
    bool zero8 = true;
    for (uint64_t b = 0; b < 64u && b < bytes; b++)
      if (p[b]) { zero8 = false; break; }
    if (zero8) {
      if (const uint8_t* alt = sr::kernel::guestVAToHost(
              0xA0000000u | (va8 & 0x1FFFFFFFu))) {
        bool zeroA = true;
        for (uint64_t b = 0; b < 64u && b < bytes; b++)
          if (alt[b]) { zeroA = false; break; }
        if (!zeroA) p = alt;
      }
    }
    return p;
  };
  const uint8_t* ip = resolveWindow(ibPhys, ibStart, (uint64_t)wrapN * 4u);
  if (!ip) { s_idxStream.unmapped++; return false; }
  // endian 2 (8in32) = guest-CPU-written BE dwords; 0 = already host order.
  auto readIdx = [&](uint32_t i) -> uint32_t {
    const uint8_t* p = ip + (size_t)i * 4u;
    return ibEndian ? ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8) | p[3])
                    : ((uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) |
                       (uint32_t(p[1]) << 8) | p[0]);
  };
  uint32_t maxIdx = 0;
  for (uint32_t i = 0; i < wrapN; i++) {
    const uint32_t v = readIdx(i);
    if (v > maxIdx) maxIdx = v;
  }
  if ((uint64_t)(maxIdx + 1u) * meshStride > vbSize) {
    s_idxStream.range++;
    static std::atomic<uint32_t> s_rl{0};
    const uint32_t rn = s_rl.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rn <= 6)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_IDXSTREAM] range REFUSED #%u prim=%u start=%u count=%u n=%u "
          "maxIdx=%u stride=%u vbSize=%llu ibSize=%llu ibEndian=%u | "
          "idx0..3=%u %u %u %u",
          rn, prim, start, count, wrapN, maxIdx, meshStride,
          (unsigned long long)vbSize, (unsigned long long)ibSize, ibEndian,
          readIdx(0), wrapN > 1 ? readIdx(1) : 0u, wrapN > 2 ? readIdx(2) : 0u,
          wrapN > 3 ? readIdx(3) : 0u);
    return false;
  }
  // The mesh is judged on the FIRST referenced vertex's window, like the
  // indexed producer; the whole span up to maxIdx must be mapped.
  const uint8_t* vp = resolveWindow(vbPhys, (uint64_t)readIdx(0) * meshStride,
                                    meshStride);
  if (!vp) { s_idxStream.unmapped++; return false; }
  vp -= (size_t)readIdx(0) * meshStride;   // back to the buffer base
  if (!sr::kernel::isGuestVAInRange(
          0x80000000u | (uint32_t)((vbPhys + (uint64_t)(maxIdx + 1u) * meshStride) &
                                   0x1FFFFFFFu))) {
    s_idxStream.unmapped++;
    return false;
  }
  static thread_local std::vector<uint8_t> t_gather;
  if (t_gather.size() < wantOut) t_gather.resize((size_t)wantOut);
  uint8_t* dst = t_gather.data();
  for (uint32_t i = 0; i < count; i++)
    std::memcpy(dst + (size_t)i * meshStride,
                vp + (size_t)readIdx(i % wrapN) * meshStride, meshStride);
  if (vbEndian == 2u) {
    for (uint64_t i = 0; i + 4 <= wantOut; i += 4) {
      std::swap(dst[i + 0], dst[i + 3]);
      std::swap(dst[i + 1], dst[i + 2]);
    }
  }
  const uint32_t va = sr::gfx::hle::poolWrite(dst, (uint32_t)wantOut);
  if (va == sr::gfx::hle::kArenaFull) { s_idxStream.poolFull++; return false; }
  vArena = va;
  vBytes = (uint32_t)wantOut;
  const uint64_t k = s_idxStream.published.fetch_add(1, std::memory_order_relaxed) + 1;
  if (k <= 6 || k % 100000 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_IDXSTREAM] gather #%llu prim=%u start=%u count=%u n=%u inst=%u "
        "meshStream=%u idxStream=%u stride=%u ibPhys=%08X ibSize=%llu "
        "ibEndian=%u vbPhys=%08X vbSize=%llu vbEndian=%u maxIdx=%u -> va=%08X | "
        "seen=%llu published=%llu instancesTotal=%llu startNonZero=%llu | "
        "refused: noSlot=%llu tooBig=%llu ragged=%llu range=%llu unmapped=%llu "
        "poolFull=%llu",
        (unsigned long long)k, prim, start, count, wrapN, count / wrapN,
        meshStream, idxStream, meshStride, ibPhys, (unsigned long long)ibSize,
        ibEndian, vbPhys, (unsigned long long)vbSize, vbEndian, maxIdx, va,
        (unsigned long long)s_idxStream.seen.load(),
        (unsigned long long)s_idxStream.published.load(),
        (unsigned long long)s_idxStream.instTotal.load(),
        (unsigned long long)s_idxStream.startNonZero.load(),
        (unsigned long long)s_idxStream.noSlot.load(),
        (unsigned long long)s_idxStream.tooBig.load(),
        (unsigned long long)s_idxStream.instanced.load(),
        (unsigned long long)s_idxStream.range.load(),
        (unsigned long long)s_idxStream.unmapped.load(),
        (unsigned long long)s_idxStream.poolFull.load());
  return true;
}
}  // namespace
#endif

extern "C" void __imp__sub_820FC000(PPCContext& __restrict ctx, uint8_t* base);
void sub_820FC000(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FC000);  // NATIVE N0 (scripts/gen_native_brackets.py)
#if SR_ORACLE
  API_OBS_PROLOGUE(s_drawIV);
  const uint32_t dev = ctx.r3.u32;
  rtAttribute(dev,
              (uint32_t)sr::gfx::hle::RenderCommandType::DrawIndexedVertices);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] DrawIndexedVertices #%llu dev=%08X prim=%u baseVtx=%d "
        "startIdx=%u idxCount=%u tid=%lu",
        (unsigned long long)n, dev, ctx.r4.u32, (int32_t)ctx.r5.u32,
        ctx.r6.u32, ctx.r7.u32, GetCurrentThreadId());
  }
  bool published = false;
  if (s6IdxEnabled() && apiMirrorEnabled()) published = idxGatherPublish(ctx);
  if (!published && apiMirrorEnabled()) {
    // The pre-iter-449 census mirror, bit-identical: raw r5/r6 land in
    // start/count (baseVertex/startIndex — NOT what the names say) with
    // vertBytes=0, which is exactly what keeps the executor's field-trust
    // guard refusing them. One command per call either way, so the
    // [HLE_MIRROR] hle==api conservation holds with the gate on or off.
    sr::gfx::hle::RenderCommand c;
    c.type = sr::gfx::hle::RenderCommandType::DrawIndexedVertices;
    c.draw.device = dev;
    c.draw.primType = ctx.r4.u32;
    c.draw.start = ctx.r5.u32;
    c.draw.count = ctx.r6.u32;
    // iter-442 S4.3: written explicitly — RenderCommand has no default member
    // initializers (a union cannot carry them), so leaving these alone would
    // publish STACK GARBAGE as a vertBytes/arena offset, and the executor
    // would release an arena slot it never owned.
    c.draw.vertArena = sr::gfx::hle::kArenaFull;
    c.draw.vertBytes = 0;
    c.draw.stride = 0;
    sr::gfx::hle::enqueue(c);
  }
  const bool nrec = sr::native::dev::on();
  if (nrec) sr::native::dev::onDrawBefore(dev, sr::native::dev::kDrawIndexed, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  if (sr::native::census::g_on) sr::native::census::drawBefore(dev, sr::native::dev::kDrawIndexed);
  if (sr::native::ledger::g_on) sr::native::ledger::before(dev, sr::native::dev::kDrawIndexed);
#endif
  // NATIVE strand A S1 (kernel/native_flush.hpp): the flush as a native record (SR_NATIVE_FLUSH), or
  // the dry native flush compared with the guest body (SR_NATIVE_FLUSH_CHECK).
  if (!sr::native::flush::draw(ctx, base, 0)) {
#if SR_ORACLE
    if (sr::native::flush::checkOn()) sr::native::flush::checkBefore(ctx, base, 0);
#endif
    __imp__sub_820FC000(ctx, base);
#if SR_ORACLE
    if (sr::native::flush::checkOn()) sr::native::flush::checkAfter(ctx, base, 0);
#endif
  }
#if SR_ORACLE
  if (sr::native::ledger::g_on) sr::native::ledger::after(dev, sr::native::dev::kDrawIndexed);
  if (sr::native::census::g_on) sr::native::census::drawAfter(sr::native::dev::kDrawIndexed);
  if (nrec) sr::native::dev::onDrawAfter(dev);
#endif
}

// ── iter-450O: SetVertexShaderConstantF OBSERVE ─────────────────────────────
// Settles ONE question for the constants arc: does the guest EVER set a VS
// float register >= 96 (the c100+ per-node parameter table / the c200 rows
// the golden dump shows populated)? IDA-verified contract (sub_820F4920,
// GpuCopyVectorConstantsVMX): args (dev r3, startReg r4, src r5, count r6);
// it writes dev+1792+16*reg — dev is typed __int64* so IDA's "result[2*reg
// + 224]" is BYTES 16*reg+1792, the SAME shadow the draw path emits from
// (EmitDrawIndexedVertices' "a1 + 224" = dev+1792 for the same reason) and
// the same region publishDrawState captures. So a zero in our capture means
// the guest never wrote it — this hook attributes that: high-reg sets seen
// here but zero at capture would instead indict the recompiled VMX copy.
// Pure observe: counters + capped logs, always on, then delegate.
extern "C" void __imp__sub_820F4920(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F4920(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F4920);  // NATIVE N0 (scripts/gen_native_brackets.py)
#if SR_ORACLE
  static std::atomic<uint64_t> s_n{0}, s_hi{0};
  // iter-450O roll 1: 442K high sets in one roll, EVERY capped sample
  // reg=100 cnt=1 from ONE site (lr=8260B310) in per-draw bursts — the
  // immediate-mode shape. The histogram exists to make "only c100" (or not)
  // a measured claim instead of a 24-sample extrapolation: c200 is what the
  // golden dump shows populated and what the fixture capture reads as zero.
  static std::atomic<uint32_t> s_regHits[160];   // regs 96..255
  // iter-465: the histogram was ~0.9% of the gameplay render thread (p1_prof self time of
  // this wrapper) for an HLE-era question that is settled; now opt-in.
  static const bool s_obs = [] {
    const char* e = std::getenv("SR_S6_SETVSF_OBS");
    return e && e[0] == '1';
  }();
  if (!s_obs) {
    (sr::native::leaf::g_leafCallersOn ? (void)(sr::native::leaf::t_leafCaller = _ReturnAddress()) : (void)0), sr::native::leaf::setShaderConstantF(ctx, base, true, [](PPCContext& c, uint8_t* b) { __imp__sub_820F4920(c, b); });
    return;
  }
  const uint32_t reg = ctx.r4.u32, cnt = ctx.r6.u32;
  const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (reg + cnt > 96u) {
    const uint64_t h = s_hi.fetch_add(1, std::memory_order_relaxed) + 1;
    for (uint32_t r = reg < 96u ? 96u : reg; r < reg + cnt && r < 256u; r++)
      s_regHits[r - 96u].fetch_add(1, std::memory_order_relaxed);
    if (h <= 24)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_SETVSF] HIGH set #%llu reg=%u cnt=%u lr=%08X (call #%llu)",
          (unsigned long long)h, reg, cnt, (uint32_t)ctx.lr,
          (unsigned long long)n);
  }
  if ((n & 0x3FFFF) == 0) {
    char hist[200]; size_t o = 0;
    for (uint32_t r = 0; r < 160u && o + 20 < sizeof(hist); r++) {
      const uint32_t v = s_regHits[r].load(std::memory_order_relaxed);
      if (v)
        o += (size_t)snprintf(hist + o, sizeof(hist) - o, " c%u:%u", r + 96u, v);
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[S6_SETVSF] total=%llu highSets=%llu | hist:%s",
        (unsigned long long)n,
        (unsigned long long)s_hi.load(std::memory_order_relaxed),
        o ? hist : " -");
  }
  // iter-465 NATIVE leaf (kernel/native_leaves.hpp): the copy + dirty OR run as
  // host code (SR_NATIVE_CONST_LEAF=0 = the recompiled body, for A/B).
  (sr::native::leaf::g_leafCallersOn ? (void)(sr::native::leaf::t_leafCaller = _ReturnAddress()) : (void)0),
#endif
  sr::native::leaf::setShaderConstantF(ctx, base, true, [](PPCContext& c, uint8_t* b) { __imp__sub_820F4920(c, b); });
}

// ── iter-445 S1 (J): THE UP/INLINE DRAW PROBE ───────────────────────────────
// SR_S6_UPPROBE, DEFAULT OFF. Measures only: no enqueue beyond the mirror this
// hook already published, no snapshot, no staging, no pixels.
//
// WHY THIS PATH IS THE ONE THAT MATTERS: the whole Apt/front-end module
// (0x826D0000-0x826F0000) has exactly ONE function that calls D3D9 and it calls
// only BeginVertices/EndVertices (three sites, guest/ppc/ppc_recomp.92.cpp
// :23360, :23515, :23887). The natural roll has a ~3.8 s window where drawV is
// FROZEN and drawUP climbs by thousands — the guest draws the front end through
// this entry point and we render literally nothing.
//
// ARG CONTRACT — VERIFIED, not assumed, from the deleted renderer's own hook
// (git show d651b04^:kernel/hle_d3d9.cpp): D3DDeviceEmitDrawIndexedGen
// sub_820FB6E8(dev, prim, count, strideBytes), and it RETURNS THE RING ALLOC VA
// IN r3 (0 on failure).
// ⚠⚠ THE CALLER MEMCPYS THE VERTICES IN **AFTER** THIS RETURNS. A snapshot
// taken in this hook would capture the PREVIOUS frame's ring contents — a fresh
// instance of the iter-444 S6j wrong-buffer bug. The bytes are only valid at the
// matching End, which is what this probe exists to locate.
// ⚠ Note the shared macro has been publishing start=r5 (really COUNT) and
// count=r6 (really STRIDE) for this entry point. That is inert only because it
// also forces vertBytes=0. Preserved BYTE-FOR-BYTE below so this probe changes
// nothing; the correct fields are the next slice's job.
extern "C" void __imp__sub_820FB6E8(PPCContext& __restrict ctx, uint8_t* base);
void sub_820FB6E8(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FB6E8);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const uint32_t resourceDevice = ctx.r3.u32;
  const uint64_t resourceBytes = uint64_t(ctx.r5.u32) * ctx.r6.u32;
#if SR_ORACLE
  API_OBS_PROLOGUE(s_drawUP);
  rtAttribute(ctx.r3.u32,
              (uint32_t)sr::gfx::hle::RenderCommandType::BeginVertices);
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] BeginVertices/UP #%llu r3=%08X r4=%08X r5=%08X r6=%08X "
        "r7=%08X tid=%lu",
        (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        ctx.r7.u32, GetCurrentThreadId());
  }
  // Preserved byte-for-byte from the shared macro — same fields, same values,
  // same order. This hook must remain a pure refactor of it.
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, cnt = ctx.r5.u32,
                 strideB = ctx.r6.u32;
  if (apiMirrorEnabled()) {
    sr::gfx::hle::RenderCommand c;
    c.type = sr::gfx::hle::RenderCommandType::BeginVertices;
    c.draw.device = ctx.r3.u32;
    c.draw.primType = ctx.r4.u32;
    c.draw.start = ctx.r5.u32;
    c.draw.count = ctx.r6.u32;
    c.draw.vertArena = sr::gfx::hle::kArenaFull;
    c.draw.vertBytes = 0;
    c.draw.stride = 0;
    sr::gfx::hle::enqueue(c);
  }
  const bool nrec = sr::native::dev::on();
  if (nrec) sr::native::dev::onDrawBefore(dev, sr::native::dev::kBeginVertices, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  if (sr::native::census::g_on) sr::native::census::drawBefore(dev, sr::native::dev::kBeginVertices);
  if (sr::native::ledger::g_on) sr::native::ledger::before(dev, sr::native::dev::kBeginVertices);
#endif
  // NATIVE strand A P1 rows 4+6+7 (kernel/native_flush.hpp): the third draw flush as a native record
  // (SR_NATIVE_BEGINV), or the dry native flush compared with the guest body (SR_NATIVE_FLUSH_CHECK).
  if (!sr::native::flush::draw(ctx, base, 2)) {
#if SR_ORACLE
    if (sr::native::flush::checkOn()) sr::native::flush::checkBefore(ctx, base, 2);
#endif
    __imp__sub_820FB6E8(ctx, base);
#if SR_ORACLE
    if (sr::native::flush::checkOn()) sr::native::flush::checkAfter(ctx, base, 2);
#endif
  }
  sr::kernel::resourceInlineBegin(resourceDevice, ctx.r3.u32, resourceBytes);
#if SR_ORACLE
  if (sr::native::ledger::g_on) sr::native::ledger::after(dev, sr::native::dev::kBeginVertices);
  if (sr::native::census::g_on) sr::native::census::drawAfter(sr::native::dev::kBeginVertices);
  if (nrec) sr::native::dev::onDrawAfter(dev);
  // iter-447: the producer reads this latch, so it must arm it too.
  if (!upProbeEnabled() && !s6UpEnabled()) return;
  const uint32_t allocVA = ctx.r3.u32;   // the emit's RETURN value
  std::unique_lock<std::mutex> lk(s_upMu);
  s_upBegins++;
  s_upWinBegins++;
  if (!allocVA) { s_upNullAlloc++; lk.unlock(); return; }
  // A second Begin before the previous one drained means the End-latch model is
  // wrong for this caller — count it rather than silently overwrite.
  if (t_up.live) s_upOverwritten++;
  lk.unlock();
  uint32_t rt0 = 0;
  safeU32(dev + 12168u, rt0);
  t_up = UpPending{allocVA, prim, cnt, strideB, dev, rt0, true};
#endif
}

// sub_820FBBB0 — VERIFIED UNHOOKED before this (grep over kernel/ and gfx/
// returned nothing) and a real recompiled function (ppc_func_mapping.cpp:372,
// callers in ppc_recomp.37/109). The pre-FE era shows 13,966 BeginVertices with
// ZERO EndVertices, so a second drain point must exist; this is the candidate.
extern "C" void __imp__sub_820FBBB0(PPCContext& __restrict ctx, uint8_t* base);
void sub_820FBBB0(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FBBB0);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const uint32_t dev = ctx.r3.u32;
  sr::kernel::ResourceInlineUpScope resourceScope(dev);
#if SR_ORACLE
  const bool nrec = sr::native::dev::on();
  if (nrec) sr::native::dev::onDrawBefore(dev, sr::native::dev::kDrawUP, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  if (sr::native::census::g_on) sr::native::census::drawBefore(dev, sr::native::dev::kDrawUP);
  if (sr::native::ledger::g_on) sr::native::ledger::before(dev, sr::native::dev::kDrawUP);
#endif
  __imp__sub_820FBBB0(ctx, base);
  resourceScope.finish();
#if SR_ORACLE
  if (sr::native::ledger::g_on) sr::native::ledger::after(dev, sr::native::dev::kDrawUP);
  if (sr::native::census::g_on) sr::native::census::drawAfter(sr::native::dev::kDrawUP);
  if (nrec) sr::native::dev::onDrawAfter(dev);
  upDrain("B");
#endif
}

// ── iter-442 S4.0: SHADER IDENTITY AT THE API BOUNDARY (probe only) ──────────
// THE decisive question for the whole S4 slice: does the shader identity we can
// compute from the bound shader OBJECTS resolve to the DXIL already on disk?
// Everything downstream assumes getXlatPso(vsHash, psHash, ...) finds a real
// pipeline in the existing cache. If the VS misses, the backend caches a
// nullptr under that key for the life of the process and the draw is silently
// skipped — indistinguishable from an upload or bind bug, because every stage
// of this chain fails into the same black screen.
//
// So this runs FIRST, alone: nothing is enqueued, nothing uploaded, no D3D12
// object touched. Ported from the deleted kernel/hle_d3d9.cpp (d651b04^:101-145).
// iter-465: also used by gfx/native_device (N1 shader identity per draw).
uint64_t s4HashShaderObjUncached(uint32_t obj, bool isVS, bool* usedTable) {
  if (usedTable) *usedTable = false;
  if (obj < 0x10000u) return 0;
  const uint32_t hdr = obj + (isVS ? 0x368u : 0x28u);
  const uint32_t ucodeVA = sr::kernel::readGuestU32BE(obj + (isVS ? 0x20u : 0x18u));
  // The cache key is XXH3 over the UCODE SECTION located by the container's
  // shaderOffset table (+0x18 -> {physOfs, size}) — NOT the physicalSize at +8
  // (that gave 70A2EA59B3006903 for a shader stored as 20A5E5204FD23BE2).
  // ⚠ CORRECTED vs the original: the old guard was `soff < 0x1000`, but real
  // containers carry shaderOffset values into the thousands, and on guard
  // failure the code fell back SILENTLY to the wrong +8 span. Widened, and the
  // caller logs which path was taken so a miss is diagnosable, not invisible.
  const uint32_t soff = sr::kernel::readGuestU32BE(hdr + 0x18);
  uint32_t physOfs = 0, physSize = sr::kernel::readGuestU32BE(hdr + 8);
  if (soff && soff < 0x10000u) {
    const uint32_t o = sr::kernel::readGuestU32BE(hdr + soff);
    const uint32_t s = sr::kernel::readGuestU32BE(hdr + soff + 4);
    if (s && s <= 0x40000u) {
      physOfs = o;
      physSize = s;
      if (usedTable) *usedTable = true;
    }
  }
  if (ucodeVA < 0x10000u || !physSize || physSize > 0x40000u) return 0;
  if (!sr::kernel::isGuestVAInRange(ucodeVA + physOfs + physSize)) return 0;
  const uint8_t* p = sr::kernel::guestVAToHost(ucodeVA + physOfs);
  if (!p) return 0;
  return sr::gfx::ShaderCache::hashUcode(p, physSize);
}

#if SR_ORACLE
// iter-442 S4.5: cached shader identity. s4HashShaderObjUncached walks guest
// memory and XXH3s the whole ucode span, which is far too expensive to run on
// every draw. Key on (obj, ucodePtr) — NOT obj alone: shader objects are pooled
// and reused, so a stale hash for a recycled VA would bind the WRONG shader,
// which renders a plausible-looking wrong picture rather than an error.
uint64_t s4HashShaderObjCached(uint32_t obj, bool isVS) {
  if (obj < 0x1000u) return 0;
  const uint32_t ucode = sr::kernel::readGuestU32BE(obj + (isVS ? 0x20u : 0x18u));
  const uint64_t key = ((uint64_t)obj << 32) | ucode;
  static std::mutex mu;
  static std::unordered_map<uint64_t, uint64_t> cache;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
  }
  const uint64_t h = s4HashShaderObjUncached(obj, isVS, nullptr);
  std::lock_guard<std::mutex> lk(mu);
  if (cache.size() < 4096) cache[key] = h;
  return h;
}

// ── iter-447 S6-UP: THE DRAW STATE, FACTORED OUT ────────────────────────────
// Extracted VERBATIM from sub_820FBBF8 so the UP/inline path can publish the
// SAME state. It was inline under `if (vBytes)`, which is exactly why a UP draw
// inherited the LAST DrawVertices' decl/constants/registers/shaders — in the
// front-end era, the frozen movie quad's (PROGRESS iter-445 predicted this;
// iter-447 measured the gate that causes it).
//
// ⚠ PUBLISH ORDER IS PART OF THE CONTRACT: every one of these lands IMMEDIATELY
// BEFORE the draw it belongs to, so "whatever is current when a draw executes"
// is exactly that draw's state. Call this directly before the draw MIRROR1.
// ⚠ A failed capture PUBLISHES WITH bytes=0 rather than staying silent, so the
// executor INVALIDATES instead of silently reusing the previous draw's state.
void publishDrawState(uint32_t dev) {
  // ── iter-443 S6c: THE VERTEX DECLARATION ────────────────────────────────
  // Format decoded in iter-435: object at dev+11536, element count at +0x18,
  // elements at +0x34, 8 bytes each — {stream<<16|byteOffset, packed X360
  // D3DDECLTYPE whose low 6 bits are the Xenos format, usage<<16|usageIdx}.
  // Snapshotted whole (tens of bytes) so the render thread can map guest
  // attributes onto the translated VS's inputs without reading guest memory.
  {
    const uint32_t decl = sr::kernel::readGuestU32BE(dev + 11536u);
    uint32_t nElem = 0;
    if (decl >= 0x1000u) safeU32(decl + 0x18u, nElem);
    // ELEMENT STRIDE IS 12, NOT 8 — measured, and it cost nothing because the
    // probe ran before any expansion code existed. D3DVERTEXELEMENT9 is 8
    // bytes on PC, which is where the wrong number came from; the 360 form is
    // three dwords: {stream<<16|byteOffset, packed D3DDECLTYPE, usage<<16|
    // usageIndex}. Live: {0, 0x2A23B9 FLOAT3, 0 POSITION} /
    // {12, 0x2C23A5 FLOAT2, 5 TEXCOORD} / {20, 0x1A23A6 FLOAT4, 10 COLOR}
    // = stride 36, which independently reproduces the movie quad's measured
    // layout. At 8 the third element was silently truncated away.
    if (nElem && nElem <= 32u) {
      const uint32_t dBytes = nElem * kDeclElemBytes;
      if (sr::kernel::isGuestVAInRange(decl + 0x34u + dBytes)) {
        if (const uint8_t* ds = sr::kernel::guestVAToHost(decl + 0x34u)) {
          // iter-444 S6e: the SMALL-BLOCK pool, not the 1 MB-slot pixel
          // arena. A 36-byte declaration in a 1 MB slot is 29,000x internal
          // fragmentation — that, plus the missing executor release, pinned
          // the arena at 48/48 and rejected 2.3M pixel/vertex snapshots.
          const uint32_t dA = sr::gfx::hle::blockWrite(ds, dBytes);
          // ⚠ PUBLISH EVEN ON FAILURE, with bytes=0. Skipping the command
          // leaves the executor holding the PREVIOUS draw's declaration and
          // silently expanding this draw's vertices with the wrong attribute
          // map — a garbage draw that still counts as recorded. bytes=0 tells
          // the executor to INVALIDATE, so the draw is refused and named.
          if (dA == sr::gfx::hle::kBlockFull) {
            s_setDeclC.bump();
            MIRROR1(SetVertexDecl, setVertexDecl.device = dev;
                                   c.setVertexDecl.arenaOffset = dA;
                                   c.setVertexDecl.bytes = 0;
                                   c.setVertexDecl.elemCount = 0);
          } else {
            // S6c FORMAT PROBE: the element stride is recorded three ways in
            // our own notes (8 B D3DVERTEXELEMENT9 vs a 12 B {streamOffset,
            // packedType, usage} triple). Dump the raw bytes and decode from
            // them — guessing a layout is what produced a degenerate quad
            // earlier this iteration.
            static std::atomic<uint64_t> s_dn{0};
            const uint64_t dn = s_dn.fetch_add(1, std::memory_order_relaxed) + 1;
            if (dn <= 6) {
              char hx[200]; size_t o = 0;
              for (uint32_t i = 0; i < dBytes && i < 64 && o + 3 < sizeof(hx); i++)
                o += (size_t)snprintf(hx + o, sizeof(hx) - o, "%02X", ds[i]);
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[S6_DECL] #%llu decl=%08X nElem=%u bytes=%u stride?=%.1f | %s",
                  (unsigned long long)dn, decl, nElem, dBytes,
                  double(dBytes) / double(nElem), hx);
            }
            s_setDeclC.bump();
            MIRROR1(SetVertexDecl, setVertexDecl.device = dev;
                                   c.setVertexDecl.arenaOffset = dA;
                                   c.setVertexDecl.bytes = dBytes;
                                   c.setVertexDecl.elemCount = nElem);
          }
        }
      }
    }
  }
  // ── iter-443 S6d: THE ALU CONSTANTS ─────────────────────────────────────
  // VS floats at dev+1792 (Xenos reg 0x4000), PS floats at dev+5888
  // (0x4400) — the device shadow the guest flushes at draw. Captured only
  // for draws that carry vertices, i.e. only the ones we could render.
  {
    const uint32_t vsCB = dev + 1792u, psCB = dev + 5888u;
    uint32_t va = sr::gfx::hle::kBlockFull, pa = sr::gfx::hle::kBlockFull;
    uint32_t vb = 0, pb = 0;
    if (sr::kernel::isGuestVAInRange(vsCB + kVsConstBytes)) {
      if (const uint8_t* p = sr::kernel::guestVAToHost(vsCB)) {
        va = sr::gfx::hle::blockWrite(p, kVsConstBytes);
        if (va != sr::gfx::hle::kBlockFull) vb = kVsConstBytes;
      }
    }
    if (sr::kernel::isGuestVAInRange(psCB + kPsConstBytes) &&
        sr::kernel::isGuestVAInRange(dev + 10004u)) {
      if (const uint8_t* p = sr::kernel::guestVAToHost(psCB)) {
        // iter-449v: PS floats + the 8-byte bool tail in one block. The bool
        // shadows sit directly after the PS float file (reference GuestDevice:
        // vertexShaderBoolConstants[4] at +9984, pixelShaderBoolConstants[4]
        // at +10000; dword 0 of each carries the live mask).
        uint8_t stage[kPsConstWithBools];
        std::memcpy(stage, p, kPsConstBytes);
        const uint32_t vsB = sr::kernel::readGuestU32BE(dev + 9984u);
        const uint32_t psB = sr::kernel::readGuestU32BE(dev + 10000u);
        std::memcpy(stage + kPsConstBytes, &vsB, 4);
        std::memcpy(stage + kPsConstBytes + 4, &psB, 4);
        pa = sr::gfx::hle::blockWrite(stage, kPsConstWithBools);
        if (pa != sr::gfx::hle::kBlockFull) pb = kPsConstWithBools;
      }
    }
    // Same rule as the declaration: a failed capture must INVALIDATE rather
    // than leave the previous draw's constants silently in force.
    if (!vb && !pb) {
      s_setConstC.bump();
      MIRROR1(SetConstants, setConstants.device = dev;
                            c.setConstants.vsArena = va;
                            c.setConstants.vsBytes = 0;
                            c.setConstants.psArena = pa;
                            c.setConstants.psBytes = 0);
    } else {
      s_setConstC.bump();
      MIRROR1(SetConstants, setConstants.device = dev;
                            c.setConstants.vsArena = va;
                            c.setConstants.vsBytes = vb;
                            c.setConstants.psArena = pa;
                            c.setConstants.psBytes = pb);
    }
  }
  // ── iter-444 S6h: THE RENDER-STATE REGISTER RUNS ────────────────────────
  // The 0x2100 run (dev+10316, 26 dwords) and the 0x2200 run (dev+10420, 33
  // dwords) are CONTIGUOUS, so ONE 236-byte block carries everything the
  // transform needs: RB_COLOR_MASK 0x2104, PA_CL_VPORT_* 0x210F-0x2114,
  // RB_DEPTHCONTROL 0x2200, RB_BLENDCONTROL0 0x2201, PA_CL_VTE_CNTL 0x2206.
  // Offsets verified against the recompiled PPC (ENGINE_MAP §iter-444 S6f).
  {
    constexpr uint32_t kRsBase = 10316u, kRsBytes = 236u;
    // iter-450T SAMPLER CAPTURE: the texture-fetch file at dev+1024 is the
    // IDA-verified home of LIVE sampler state — the ~20 SetSamplerState
    // setters (table 0x8295C3A0) RMW dev+1024+24*slot in place, and
    // SetTexture (sub_82100310) preserves the sampler-owned bits with
    // keep-masks dw0 0x3FFC00 / dw3 0x7FF80000. Appending it to the SAME
    // block as the render-state run costs zero extra pool traffic (1004 B
    // in an 8 KB block); bytes tells the executor whether it is present,
    // so pre-450T traces (bytes=236) replay unchanged.
    constexpr uint32_t kTfBase = 1024u, kTfBytes = 768u;
    if (sr::kernel::isGuestVAInRange(dev + kRsBase + kRsBytes)) {
      if (const uint8_t* rs = sr::kernel::guestVAToHost(dev + kRsBase)) {
        uint8_t stage[kRsBytes + kTfBytes];
        std::memcpy(stage, rs, kRsBytes);
        uint32_t total = kRsBytes;
        if (sr::kernel::isGuestVAInRange(dev + kTfBase + kTfBytes)) {
          if (const uint8_t* tf = sr::kernel::guestVAToHost(dev + kTfBase)) {
            std::memcpy(stage + kRsBytes, tf, kTfBytes);
            total += kTfBytes;
          }
        }
        const uint32_t ra = sr::gfx::hle::blockWrite(stage, total);
        // Publish even on failure (bytes=0) so the executor INVALIDATES
        // rather than silently reusing the previous draw's transform.
        s_setRsC.bump();
        MIRROR1(SetRenderState,
                setRenderState.device = dev;
                c.setRenderState.blockOffset = ra;
                c.setRenderState.bytes =
                    (ra == sr::gfx::hle::kBlockFull) ? 0u : total);
      }
    }
  }
  const uint64_t vsH = s4HashShaderObjCached(
      sr::kernel::readGuestU32BE(dev + 12420u), true);
  const uint64_t psH = s4HashShaderObjCached(
      sr::kernel::readGuestU32BE(dev + 12416u), false);
  s_setShadersC.bump();
  MIRROR1(SetShaders, setShaders.device = dev;
                      c.setShaders.vsLo = (uint32_t)vsH;
                      c.setShaders.vsHi = (uint32_t)(vsH >> 32);
                      c.setShaders.psLo = (uint32_t)psH;
                      c.setShaders.psHi = (uint32_t)(psH >> 32));
  // iter-450UD H2/H3: the last act before the draw command is mirrored, so this
  // is the pre-draw instant. Fires only when sampler 0 names SR_AUTOPSY_KEY.
  atlasProbe("predraw", dev, 0u);
}
#endif

extern "C" void __imp__sub_820FBBF8(PPCContext& __restrict ctx, uint8_t* base);
void sub_820FBBF8(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FBBF8);  // NATIVE N0 (scripts/gen_native_brackets.py)
#if SR_ORACLE
  API_OBS_PROLOGUE(s_drawV);
  const uint32_t dev = ctx.r3.u32;
  if (ctx.r4.u32 == 13u && ctx.r6.u32 == 4u) s_movieQuad.bump();
  rtAttribute(dev, (uint32_t)sr::gfx::hle::RenderCommandType::DrawVertices);
  // ── iter-445 S6k: THE DECLARATION IS READ FIRST, BECAUSE IT NAMES THE
  //    STREAM ─────────────────────────────────────────────────────────────
  // iter-444 S6j proved we were snapshotting the WRONG BUFFER: decoded
  // positions alternated between exactly two values, an 8-byte pattern read as
  // float3 at stride 36. The cause was iter-443's slot heuristic — "any kVertex
  // fetch slot whose declared size CONTAINS this draw's range, tightest fit
  // wins". With ~96 slots, many buffers trivially contain a small range, so the
  // tiebreak picked an unrelated one; its celebrated 740 -> 437,739 capture jump
  // was mostly draws reading the wrong bytes (coverage up, correctness down).
  //
  // THE RULE, no heuristic left: the declaration carries a STREAM index per
  // element (d0 >> 16) and iter-435 proved `slot = 95 - stream` (movie: stream
  // 0 -> slot 95). The xlat path has always used exactly that. The declaration
  // must therefore be read BEFORE the snapshot — which is why this block moved
  // up here from inside the publish block below.
  //
  // ⚠ MULTI-STREAM DRAWS ARE REFUSED, NOT GUESSED. hleSubmitDraw pairs the
  // first N decl elements positionally with the VS inputs and applies ONE
  // srcStride, so a decl spanning two streams would decode stream-1 attributes
  // at stream-0 offsets out of the stream-0 buffer. Counted and named.
  uint32_t declN = 0, declStream = 0;
  bool declMulti = false;
  // iter-456p Step 3A: the set of streams this declaration REFERENCES, as a
  // bitmask. The publication considers only these — capturing every bound
  // stream would publish the guest's null housekeeping binds on streams 2-15
  // and, worse, whatever a stale live binding still names.
  uint32_t declStreamMask = 0;
  // iter-458: the INDEX VERTEX STREAM — POSITION usageIndex 1 in a stream other
  // than element 0's (reference video.cpp:4913). ~0u = none declared.
  uint32_t idxStream = ~0u;
  // NATIVE strand A (iter-467): the declaration's consumers are the HLE mirror / publication
  // (apiMirror, SR_HLE_VB_PUBLISH) and their census; under SR_PM4_LIVE none runs, and the
  // range-checked element reads were ~1% of the guest render tick (gc3 p0 profile).
  static const bool s_declUsed = apiMirrorEnabled() || vbPublishEnabled();
  if (s_declUsed) {
    const uint32_t decl = sr::kernel::readGuestU32BE(dev + 11536u);
    uint32_t nE = 0;
    if (decl >= 0x1000u) safeU32(decl + 0x18u, nE);
    if (nE && nE <= 32u) {
      bool ok = true;
      for (uint32_t i = 0; i < nE; i++) {
        uint32_t d0 = 0, d2 = 0;
        if (!safeU32(decl + 0x34u + kDeclElemBytes * i, d0)) { ok = false; break; }
        const uint32_t st = (d0 >> 16) & 3u;
        if (i == 0) declStream = st;
        else if (st != declStream) declMulti = true;
        if (st < sr::gfx::kVbMaxStreams) declStreamMask |= (1u << st);
        // usage = byte 2, usageIndex = byte 1 of the third dword (the layout
        // tools/srht_census.py decodes: NORMAL0 = 00030000, POSITION1 = 00000100).
        if (i && safeU32(decl + 0x34u + kDeclElemBytes * i + 8u, d2) &&
            ((d2 >> 16) & 0xFFu) == 0u && ((d2 >> 8) & 0xFFu) == 1u &&
            st != declStream)
          idxStream = st;
      }
      if (ok) declN = nE;
      else declStreamMask = 0;   // a partial scan names no streams
    }
  }
  // ⚠ BEFORE the snapshot below, so a publication always precedes the draw that
  // consumes it in the command stream — replay must be able to rebuild the
  // resource before it applies the binding.
  vbPublishForDraw(dev, declStreamMask, ctx.r4.u32, ctx.r5.u32,
                   ctx.r6.u32);
  if (declMulti) {
    static std::atomic<uint64_t> s_multi{0};
    const uint64_t m = s_multi.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m == 1 || m % 500000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_STREAM] %llu draws have a MULTI-STREAM declaration (census, not "
          "a refusal — the backend refuses only if a CONSUMED element uses the "
          "second stream). Per-stream snapshots are the next slice.",
          (unsigned long long)m);
    // ⚠ A refusal rate near 100% is a claim about the GAME's vertex layout, and
    // that claim is far too strong to accept from a one-line derivation. Dump
    // the raw elements for the first few so the reason NAMES itself: genuine
    // multi-stream geometry, or a D3DDECL_END-style terminator being read as a
    // real element (stream 0xFF & 3 = 3, which would look exactly like this).
    if (m <= 4) {
      const uint32_t decl = sr::kernel::readGuestU32BE(dev + 11536u);
      uint32_t nE = 0;
      safeU32(decl + 0x18u, nE);
      char l[440]; int o = 0;
      for (uint32_t i = 0; i < nE && i < 12u && o < 380; i++) {
        uint32_t d0 = 0, d1 = 0, d2 = 0;
        safeU32(decl + 0x34u + kDeclElemBytes * i, d0);
        safeU32(decl + 0x34u + kDeclElemBytes * i + 4u, d1);
        safeU32(decl + 0x34u + kDeclElemBytes * i + 8u, d2);
        o += snprintf(l + o, sizeof(l) - o, " e%u{%08X %08X %08X s=%u@%u}", i,
                      d0, d1, d2, d0 >> 16, d0 & 0xFFFFu);
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_DECL] multi-stream #%llu decl=%08X nElem=%u |%s",
          (unsigned long long)m, decl, nE, l);
    }
  }
  if (loud) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[API_OBS] DrawVertices #%llu dev=%08X prim=%u start=%u count=%u tid=%lu",
        (unsigned long long)n, dev, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        GetCurrentThreadId());
  }
  // S4.0 probe: first 16 draws only, then every 5000th so a later era can be
  // spot-checked without flooding.
  if (s4IdProbeEnabled() && (n <= 16 || n % 5000 == 0)) {
    // Device shadow: PS object at dev+12416, VS at dev+12420 (ENGINE_MAP §6d).
    const uint32_t psObj = sr::kernel::readGuestU32BE(dev + 12416);
    const uint32_t vsObj = sr::kernel::readGuestU32BE(dev + 12420);
    bool vsTable = false, psTable = false;
    const uint64_t vsH = s4HashShaderObjUncached(vsObj, true, &vsTable);
    const uint64_t psH = s4HashShaderObjUncached(psObj, false, &psTable);
    auto& sc = sr::gfx::getShaderCache();
    sc.ensureLoaded();
    const bool vsHit = vsH && sc.findVs(vsH) != nullptr;
    const bool psHit = psH && sc.findPs(psH) != nullptr;
    // iter-442 S4.3 DISAMBIGUATION: the movie quad's vertex STRIDE is recorded
    // two different ways and they disagree — ENGINE_MAP L951 has a live
    // SetStreamSource with stride 0x14 (20B = pos float3 + uv float2), while
    // iter-427/435 recorded 36 (pos + uv + a trailing const float4). Those came
    // from the PM4 ring, where "the true stride lives in runtime-patched vfetch
    // ucode" — a different view of the same draw. Read it AT the movie draw from
    // the device shadow, which is the authority for the API path we now render
    // from: VB pointer at dev[4*(3047+s)], stride/4 byte at dev+12256+s.
    const uint32_t vb0 = sr::kernel::readGuestU32BE(dev + 4u * 3047u);
    const uint32_t strideWord = sr::kernel::readGuestU32BE(dev + 12256u);
    const uint32_t stride0 = ((strideWord >> 24) & 0xFFu) * 4u;   // s=0, BE MSB
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S4_ID] #%llu prim=%u count=%u vsObj=%08X vs=%016llX hit=%d tbl=%d | "
        "psObj=%08X ps=%016llX hit=%d tbl=%d | vb0=%08X stride0=%u (raw=%08X)",
        (unsigned long long)n, ctx.r4.u32, ctx.r6.u32, vsObj,
        (unsigned long long)vsH, vsHit ? 1 : 0, vsTable ? 1 : 0,
        psObj, (unsigned long long)psH, psHit ? 1 : 0, psTable ? 1 : 0,
        vb0, stride0, strideWord);
  }
  // ── iter-442 S4.3: THE PRODUCER-SIDE VERTEX SNAPSHOT ──────────────────────
  // Same contract as the plane pixels (S4.2): the render thread must never read
  // guest memory, so the vertices are copied here, on the guest's thread, and
  // the command carries an arena offset. The quad's own bytes are the source of
  // truth — hardcoding a known-good NDC quad would render something the guest
  // did not ask for, and would silently stop tracking it if the guest changed.
  //
  // Device shadow (ENGINE_MAP §6g.2): VB pointer dev[4*(3047+s)], stride/4 byte
  // at dev+12256+s. MEASURED stride 36 = pos float3@0, uv float2@12, const
  // float4@20, so the movie quad is 4*36 = 144 B.
  uint32_t vArena = sr::gfx::hle::kArenaFull, vBytes = 0, vStride = 0;
  // iter-458: an index-stream declaration is an INDEXED draw in disguise —
  // gather vertex[idx] in draw order (idxStreamGather). On refusal the draw
  // falls through to the linear snapshot exactly as before, so the A/B
  // (SR_S6_IDXSTREAM=0) and every refusal share the one legacy path.
  if (apiMirrorEnabled() && idxStream != ~0u && idxStreamEnabled()) {
    const uint32_t ms = ((sr::kernel::readGuestU32BE(dev + 12256u) >>
                         (24u - 8u * (declStream & 3u))) & 0xFFu) * 4u;
    idxStreamGather(dev, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, declStream,
                    idxStream, ms, vArena, vBytes);
  }
  if (apiMirrorEnabled()) {
    // ⚠ dev[4*(3047+s)] is the vertex-buffer OBJECT HEADER, NOT the vertices —
    // measured: its +20 dword reads the 0xFFFF0000 dirty sentinel and its +8
    // dword counts up per frame. Exactly the texture split of §6g.2. The real
    // bytes come from the VERTEX FETCH CONSTANT, 2 dwords at dev+1024+8*slot:
    //   d0 = physBase | type(3=kVertex)   d1 = endian(2) | sizeDwords(24) | ...
    // MEASURED live for the movie quad: s95 {0BC868B3,10000092} = phys
    // 0x0BC868B0, 36 dwords = 144 B = 4 verts x 36, endian 2 (8IN32).
    // iter-445 S6k: the stride is PER STREAM — dev+12256 is a byte array
    // indexed by stream, so stream s sits at shift (24 - 8*s) of the BE dword.
    // Reading byte 0 for every draw was correct only for stream-0 geometry.
    vStride = ((sr::kernel::readGuestU32BE(dev + 12256u) >>
                (24u - 8u * (declStream & 3u))) & 0xFFu) * 4u;
    // iter-443 S6a: SR_S6_VERTCAP raises the per-draw snapshot cap. Purely a
    // MEASURING instrument for the S6b design decision — it quantifies what
    // "just snapshot every draw" actually costs (arena rejects, throughput)
    // instead of asserting it is impossible.
    // iter-458 director ruling: preserve the legacy 4 KB default while the
    // POSITION1/index-stream lane is parked. A positive explicit value remains
    // diagnostic; unset, empty, zero, invalid and over-arena values all keep
    // the pre-change cap.
    constexpr uint32_t kMaxVertSnapshot = 4096u;
    static const uint32_t s_cap = [] {
      const char* e = std::getenv("SR_S6_VERTCAP");
      const long v = e ? std::atol(e) : 0;
      return (v > 0 && v <= (long)sr::gfx::hle::kArenaSlotBytes)
                 ? (uint32_t)v : kMaxVertSnapshot;
    }();
    const uint64_t want = (uint64_t)vStride * (uint64_t)ctx.r6.u32;
    // iter-445 S6k: RESOLVE BY STREAM, NOT BY SIZE. `slot = 95 - stream`
    // (iter-435, and what the xlat path has always used). One slot, no scan,
    // no tiebreak — and it costs one fetch-constant read instead of 96.
    uint32_t vbVA = 0, vbEndian = 0, vbBase = 0;
    uint64_t bestSz = ~0ull;
    // ⚠ iter-445 S6k, CORRECTED BY THE [S6_DECL] DUMP: the first cut refused
    // any draw whose DECLARATION spanned two streams, and that refused ~100% of
    // DrawVertices (2.2 M of 2.19 M) — general geometry declares 7 elements
    // with six in stream 0 and one in stream 1. But `hleSubmitDraw` only pairs
    // the first N elements the VS actually CONSUMES, and it is the only side
    // that knows N. So snapshot element 0's stream here and let the backend
    // refuse (HleReject::MultiStream) only when a CONSUMED element disagrees.
    // `declMulti` stays as a census number, not a gate.
    //
    // ⚠ iter-445 S1: THE SIZE CAP IS NO LONGER PART OF THIS CONDITION. The
    // [S6_UNIQ] working-set census lives inside this block, and gating it on
    // `want <= s_cap` meant it censused only the buffers the 4 KB cap already
    // admits — i.e. it excluded BY CONSTRUCTION the ~1.4M over-cap draws that a
    // vertex-buffer cache exists to carry, and any cache sized from it would be
    // sized from the wrong population. Resolve and census every draw; apply the
    // cap only to the SNAPSHOT below. Cost: one fetch-constant read per draw.
    if (want && declN) {
      const uint64_t byteStart = (uint64_t)ctx.r5.u32 * (uint64_t)vStride;
      // COST NOTE (measured, iter-443): range-checked per-slot guest reads cost
      // ~4x throughput at a few million draws a run. Resolve the fetch file to
      // a HOST pointer once and read it directly.
      const uint8_t* fetchFile =
          sr::kernel::isGuestVAInRange(dev + 1024u + 768u)
              ? sr::kernel::guestVAToHost(dev + 1024u)
              : nullptr;
      const uint32_t slot = 95u - (declStream & 3u);
      if (fetchFile) {
        const uint8_t* e = fetchFile + 8u * slot;
        const uint32_t d0 = (uint32_t(e[0]) << 24) | (uint32_t(e[1]) << 16) |
                            (uint32_t(e[2]) << 8) | uint32_t(e[3]);
        const uint32_t d1 = (uint32_t(e[4]) << 24) | (uint32_t(e[5]) << 16) |
                            (uint32_t(e[6]) << 8) | uint32_t(e[7]);
        const uint64_t sz = (uint64_t)((d1 >> 2) & 0xFFFFFFu) * 4u;
        // The slot must actually be a VERTEX fetch constant AND must hold this
        // draw's whole range. A slot that fails either test means the stream
        // rule does not describe this draw — refuse and count it rather than
        // fall back to a guess, which is what produced the wrong buffer before.
        if ((d0 & 3u) == 3u && sz >= byteStart + want) {
          bestSz = sz;
          vbBase = 0x80000000u | ((d0 & ~3u) & 0x1FFFFFFFu);
          vbVA = vbBase + (uint32_t)byteStart;
          vbEndian = d1 & 3u;
        } else {
          static std::atomic<uint64_t> s_slotMiss{0};
          const uint64_t sm = s_slotMiss.fetch_add(1, std::memory_order_relaxed) + 1;
          if (sm <= 8 || sm % 100000 == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[S6_STREAM] slot miss #%llu stream=%u slot=%u d0=%08X d1=%08X "
                "(type=%u size=%llu need=%llu) stride=%u count=%u",
                (unsigned long long)sm, declStream, slot, d0, d1, d0 & 3u,
                (unsigned long long)sz, (unsigned long long)(byteStart + want),
                vStride, ctx.r6.u32);
          // ── iter-449m SLOT-MISS FORENSICS: the 700K/33%-of-noVerts class ──
          // The example that prompted this: need=154,224 from a slot declaring
          // 46,592 B — which is NOT an integral vertex count at the declared
          // stride. Same shape of probe that unmasked the range class: the
          // full decl (streams+offsets) and the neighbouring slots' sizes
          // discriminate wrong-stream / wrong-stride / wrong-size-decode in
          // one line each.
          if (sm <= 8) {
            char dm[130]; size_t dmo = 0;
            {
              const uint32_t decl = sr::kernel::readGuestU32BE(dev + 11536u);
              uint32_t nE = 0;
              safeU32(decl + 0x18u, nE);
              for (uint32_t e2 = 0; e2 < nE && e2 < 12u && dmo + 14 < sizeof(dm); e2++) {
                uint32_t dd0 = 0, dd1 = 0;
                safeU32(decl + 0x34u + kDeclElemBytes * e2, dd0);
                safeU32(decl + 0x34u + kDeclElemBytes * e2 + 4u, dd1);
                dmo += (size_t)snprintf(dm + dmo, sizeof(dm) - dmo,
                                        " s%u@%u:t%02X", (dd0 >> 16) & 3u,
                                        dd0 & 0xFFFFu, dd1 & 0x3Fu);
              }
            }
            char sl[170]; size_t slo = 0;
            for (uint32_t s2 = 95; s2 >= 92 && slo + 26 < sizeof(sl); s2--) {
              uint32_t e0 = 0, e1 = 0;
              safeU32(dev + 1024u + 8u * s2, e0);
              safeU32(dev + 1024u + 8u * s2 + 4u, e1);
              slo += (size_t)snprintf(sl + slo, sizeof(sl) - slo,
                                      " s%u{t%u sz=%u st?=%u}", s2, e0 & 3u,
                                      ((e1 >> 2) & 0xFFFFFFu) * 4u,
                                      (sr::kernel::readGuestU32BE(dev + 12256u) >>
                                       (24u - 8u * (95u - s2))) & 0xFFu);
            }
            // iter-449n: the SHADER names the fetch model — a VS that indexes
            // stream 0 manually (via the 4-byte stream-1 element) is the lead,
            // and its hash is the key into our own translated-shader cache.
            const uint64_t smVs = s4HashShaderObjCached(
                sr::kernel::readGuestU32BE(dev + 12420u), true);
            const uint64_t smPs = s4HashShaderObjCached(
                sr::kernel::readGuestU32BE(dev + 12416u), false);
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[S6_SLOTMISS] #%llu start=%u count=%u vs=%016llX ps=%016llX "
                "| decl:%s | slots:%s",
                (unsigned long long)sm, ctx.r5.u32, ctx.r6.u32,
                (unsigned long long)smVs, (unsigned long long)smPs,
                dmo ? dm : " -", slo ? sl : " -");
          }
        }
      }
      // iter-443 S6b SIZING MEASUREMENT: how many DISTINCT vertex buffers, and
      // how many unique bytes, does one frame actually touch? This decides the
      // whole residency design and nothing else can answer it — at ~5 MB/frame
      // a buffer cache is a luxury; at ~200 MB/frame invalidation policy IS the
      // problem. Counting unique KEYS (physBase|size), not per-draw bytes, is
      // the point: many draws share one buffer, which is exactly why per-draw
      // snapshotting was the wrong model.
      if (vbBase && bestSz != ~0ull) {
        const uint64_t key =
            ((uint64_t)(vbBase & 0x1FFFFFFFu) << 32) | (uint64_t)(uint32_t)bestSz;
        // ⚠ iter-445 S1 COST FIX: hoisting this census above the size cap put a
        // MUTEX ON EVERY DRAW (the cap had been suppressing ~99% of them), and
        // the roll that measured it ran at ~40% of normal throughput. An
        // instrument that moves what it measures is not an instrument. Most
        // consecutive draws share one buffer, so a thread-local last-key filter
        // removes nearly all of the lock traffic while keeping the census
        // exact — it can only skip a key this thread just counted.
        thread_local uint64_t t_lastUniq = ~0ull;
        if (key != t_lastUniq) {
          t_lastUniq = key;
          bool fresh = false;
          {
            std::lock_guard<std::mutex> lk(s_uniqMu);
            if (s_uniqKeys.size() < 200000 && s_uniqKeys.insert(key).second) {
              s_uniqBytes += bestSz;
              fresh = true;
            }
          }
          // iter-447 [S6_VBID]: test ONLY newly-distinct buffers against the
          // D3D9 create path. Defined at the foot of this file.
          void vbIdTest(uint32_t drawnBase);
          if (fresh) vbIdTest(vbBase);
        }
      }
    }
    // ── iter-449L: THE RESIDENCY ATTEMPT, before any per-draw copy ─────────
    // Whole buffer, keyed (physBase,size) — the [S6_UNIQ] key — uploaded once
    // and referenced by start/count. Falls back to the pool path on any
    // refusal (volatile key, caps, unreadable), so coverage degrades
    // honestly. ⚠ Disabled while TRACING: resident draws carry no payload,
    // and a trace recorded with residency on would replay to refusals.
    static const bool s_vbcacheOn = [] {
      const char* e = std::getenv("SR_S6_VBCACHE");
      return e && e[0] == '1' && !std::getenv("SR_HLE_TRACE_REC");
    }();
    if (s_vbcacheOn && vArena == sr::gfx::hle::kArenaFull && vbBase &&
        bestSz != ~0ull && bestSz <= sr::gfx::hle::kVbCacheEntryCap &&
        sr::kernel::isGuestVAInRange(vbBase + (uint32_t)bestSz)) {
      // ⛔ vbc3: BOTH physical windows go to the cache; IT chooses at upload
      // by whole-buffer scan (a head-only probe here uploaded 4 MB of zeros
      // for 0xA-written buffers and blacked out the whole FE era, starfield
      // included).
      const uint8_t* whole = sr::kernel::guestVAToHost(vbBase);
      const uint8_t* alt = sr::kernel::guestVAToHost(
          0xA0000000u | (vbBase & 0x1FFFFFFFu));
      if (whole) {
        const uint64_t rkey =
            ((uint64_t)(vbBase & 0x1FFFFFFFu) << 32) | (uint64_t)(uint32_t)bestSz;
        const uint32_t tag = sr::gfx::hle::vbCacheAcquire(
            rkey, whole, alt, (uint32_t)bestSz, vbEndian);
        if (tag != sr::gfx::hle::kArenaFull) {
          vArena = tag;
          vBytes = (uint32_t)want;   // the DRAW's extent; buildDraw offsets
                                     // by start*stride within the entry
        }
      }
    }
    // iter-445 S1: the cap moved HERE — it bounds the copy, not the census.
    // iter-449L: only reached when residency did not serve the draw.
    if (vArena == sr::gfx::hle::kArenaFull && want <= s_cap && vbVA >= 0x1000u &&
        sr::kernel::isGuestVAInRange(vbVA + (uint32_t)want)) {
      const uint8_t* src = sr::kernel::guestVAToHost(vbVA);
      // iter-425 PLATFORM ACCOMMODATION, reused verbatim (SR_VB_ALIAS_PROBE,
      // default ON): our harness does NOT fold the physical windows, so a VB
      // the guest wrote through an 0xA-class VA reads ALL ZERO at the 0x8
      // window — which is exactly what this probe measured before the fix
      // (a degenerate quad: correct shaders painting nothing). On console every
      // window holds identical bytes, so preferring whichever one actually has
      // data is console-consistent when only one was written. READ-SIDE ONLY:
      // no write folding, so none of iter-373's allocator-collision risk.
      static const bool s_alias = [] {
        const char* e2 = std::getenv("SR_VB_ALIAS_PROBE");
        return !e2 || e2[0] != '0';
      }();
      // iter-458 (Astra 029 B): SR_PHYS_RESOLVE - ownership decides when it
      // names one window; the alias probe below is the legacy rule for the rest.
      const int s4Res = sr::kernel::physwin::resolveOwned(
          sr::kernel::physwin::kRsS4Vb, vbVA, want);
      if (s4Res && src) {
        const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u | (vbVA & 0x1FFFFFFFu));
        const uint8_t* sel = s4Res == 0xA ? alt : src;
        const uint64_t n = want < 256u ? want : 256u;
        const bool selOk = resolvedBytesPlausible(sel, s4Res == 0xA ? src : alt, n);
        sr::kernel::physwin::resolveValidate(sr::kernel::physwin::kRsS4Vb, selOk);
        // Astra 030: the legacy alias probe (SR_VB_ALIAS_PROBE) on the same bytes.
        int lw = 8;
        if (s_alias && alt) {
          bool z8 = true, zA = true;
          for (uint32_t b = 0; b < 64 && b < want; b++) {
            if (src[b]) z8 = false;
            if (alt[b]) zA = false;
          }
          if (z8 && !zA) lw = 0xA;
        }
        sr::kernel::physwin::resolveCompare(
            sr::kernel::physwin::kRsS4Vb, s4Res, lw, selOk,
            lw == s4Res ? selOk
                        : resolvedBytesPlausible(lw == 0xA ? alt : src, lw == 0xA ? src : alt, n));
        if (sel) src = sel;
      }
      if (s_alias && src && !s4Res) {
        bool zero8 = true;
        for (uint32_t b = 0; b < 64 && b < want; b++) if (src[b]) { zero8 = false; break; }
        if (zero8) {
          const uint32_t altVA = 0xA0000000u | (vbVA & 0x1FFFFFFFu);
          if (const uint8_t* alt = sr::kernel::guestVAToHost(altVA)) {
            bool zeroA = true;
            for (uint32_t b = 0; b < 64 && b < want; b++) if (alt[b]) { zeroA = false; break; }
            if (!zeroA) {
              src = alt;
              static std::atomic<uint32_t> s_an{0};
              const uint32_t an = s_an.fetch_add(1, std::memory_order_relaxed) + 1;
              if (an <= 4 || an % 5000 == 0)
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[S4_VERT] #%u VB alias: 0x8-window zero at %08X, using "
                    "0xA-window %08X", an, vbVA, altVA);
            }
          }
        }
      }
      // iter-458 H-WIN (observe-only): which window did the vertex copy use?
      if (src && sr::kernel::physwin::censusEnabled()) {
        sr::kernel::physwin::DrawCtx dc;   // Astra 043: sample context
        dc.presentId = sr::gfx::hle::presentCount();
        dc.prim = ctx.r4.u32; dc.stride = vStride; dc.count = ctx.r6.u32;
        dc.startIndex = ctx.r5.u32; dc.vbEndian = (uint8_t)vbEndian;
        sr::kernel::physwin::setDrawContext(dc, /*newDraw=*/true);
      }
      if (src && sr::kernel::physwin::censusEnabled())
        sr::kernel::physwin::census(sr::kernel::physwin::kS4Vb, vbVA,
                                    (uint32_t)want,
                                    src == sr::kernel::guestVAToHost(vbVA));
      if (src) {
        // iter-450k [QUAD_ENDIAN]: the post-chain screen quads reach the GPU
        // as UNSWAPPED BE floats (denormals ±256 decoded LE) — every post
        // surface paints zero pixels. Name whether the fetch slot declares
        // endian!=2 for these dynamic quad VBs or a path skipped the swap.
        if (ctx.r4.u32 == 13u && ctx.r6.u32 == 4u && vStride == 20u) {
          static std::atomic<uint32_t> s_qeN{0};
          const uint32_t qn = s_qeN.fetch_add(1, std::memory_order_relaxed) + 1;
          if (qn <= 10)
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[QUAD_ENDIAN] #%u vbEndian=%u stride=%u want=%llu vbVA=%08X "
                "first8: %02X %02X %02X %02X %02X %02X %02X %02X",
                qn, vbEndian, vStride, (unsigned long long)want, vbVA,
                src[0], src[1], src[2], src[3], src[4], src[5], src[6],
                src[7]);
        }
        vArena = sr::gfx::hle::poolWrite(src, (uint32_t)want);
        if (vArena != sr::gfx::hle::kArenaFull) {
          vBytes = (uint32_t)want;
          // ENDIAN 2 = 8IN32: guest floats are big-endian, D3D12 needs little.
          // Swap in the ARENA copy (host-owned) — never in guest memory, which
          // the guest still owns and reads.
          if (vbEndian == 2u) {
            if (uint8_t* dst = const_cast<uint8_t*>(sr::gfx::hle::poolRead(vArena))) {
              for (uint32_t i = 0; i + 4 <= vBytes; i += 4) {
                std::swap(dst[i + 0], dst[i + 3]);
                std::swap(dst[i + 1], dst[i + 2]);
              }
            }
          }
        }
      }
    } else if (vArena == sr::gfx::hle::kArenaFull && want > s_cap) {
      // NO SILENT CAPS: say how much geometry this slice cannot carry.
      // iter-449L: a draw the residency cache SERVED must not count here —
      // this arm is only for draws nothing carried.
      static std::atomic<uint64_t> s_skip{0};
      const uint64_t k = s_skip.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k == 1 || k % 20000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S4_VERT] skipped %llu draws over the %u B snapshot cap "
            "(latest %u verts x %u B) — S6 replaces this with a VB cache",
            (unsigned long long)k, s_cap,
            ctx.r6.u32, vStride);
    }
  }
  if (s4IdProbeEnabled() && ctx.r4.u32 == 13u && ctx.r6.u32 == 4u) {
    static std::atomic<uint64_t> s_vq{0};
    const uint64_t k = s_vq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 4) {
      // dev[4*3047] turned out to be a RESOURCE HEADER, not the vertices: its
      // +20 dword reads the 0xFFFF0000 dirty sentinel and its +8 dword counts
      // up per frame. Same shape as the texture header (§6g.2), where the real
      // bytes live ~15 MB away and the fetch constant at +28 carries the VA.
      // Dump the object so the data pointer can be READ rather than guessed.
      // THE DOCUMENTED CONTRACT (iter-423c, ENGINE_MAP §6d): the movie VS
      // 8C029BAD1E4F75CD reads VERTEX fetch slots 95/94 (group 31) of the
      // device fetch file at dev+1024. Vertex fetch constants are 2 dwords
      // {base|type, size}, so slot s sits at dev+1024+8*s. base is PHYSICAL;
      // host VA = 0x80000000 | (phys & 0x1FFFFFFF) (guest_context.hpp:124).
      // THE CONTENT CHECK: decode the ARENA copy as the documented layout
      // (pos float3 @0, uv float2 @12, const float4 @20). A screen quad must
      // read as NDC corners with UVs in [0,1]. Garbage here means the fetch
      // slot or the endian swap is wrong — and garbage vertices render a
      // degenerate quad, i.e. a BLACK SCREEN with no other symptom.
      char line[240]; size_t o = 0;
      const uint8_t* q = vBytes ? sr::gfx::hle::arenaRead(vArena) : nullptr;
      for (uint32_t v = 0; q && v < 4 && o + 52 < sizeof(line); v++) {
        float f[5];
        std::memcpy(f, q + v * vStride, 12);          // pos.xyz
        std::memcpy(f + 3, q + v * vStride + 12, 8);  // uv
        o += (size_t)snprintf(line + o, sizeof(line) - o,
              "(%.2f,%.2f,%.2f|%.2f,%.2f) ", f[0], f[1], f[2], f[3], f[4]);
      }
      if (!q) { line[0] = '-'; line[1] = 0; }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S4_VERT] quad #%llu stride=%u bytes=%u ofs=%08X | %s",
          (unsigned long long)k, vStride, vBytes, vArena, line);
    }
  }
  // S4.5: publish shader identity IMMEDIATELY BEFORE the draw it belongs to, so
  // the executor's "current shaders" are unambiguous. Only for draws that
  // carry vertices — those are the only ones this slice can render, and hashing
  // every draw would cost far more than it buys.
  //
  // ── iter-445: THE SEQUENCE WINDOW ───────────────────────────────────────
  // Everything from here to the DrawVertices publish is ONE logical unit: five
  // commands (decl, constants, register block, shaders, draw) that the executor
  // reads as "the state of THIS draw". They go through five separate lock
  // acquisitions, so a second guest thread publishing in between would hand its
  // state to our draw. The scope measures that (SR_HLE_SEQPROBE, default OFF)
  // and costs nothing when the gate is off — it does NOT fix it; the fix is
  // hle_video.hpp's LocalCommandBatch, which is written and unused.
  {
    sr::gfx::hle::SeqScope seq;

  if (vBytes) publishDrawState(dev);
  MIRROR1(DrawVertices, draw.device = dev; c.draw.primType = ctx.r4.u32;
                        c.draw.start = ctx.r5.u32; c.draw.count = ctx.r6.u32;
                        c.draw.vertArena = vArena; c.draw.vertBytes = vBytes;
                        c.draw.stride = vStride);
  }   // iter-445: end of the sequence window
  const bool nrec = sr::native::dev::on();
  if (nrec) sr::native::dev::onDrawBefore(dev, sr::native::dev::kDrawAuto, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  if (sr::native::census::g_on) sr::native::census::drawBefore(dev, sr::native::dev::kDrawAuto);
  if (sr::native::ledger::g_on) sr::native::ledger::before(dev, sr::native::dev::kDrawAuto);
#endif
  // NATIVE strand A S1 (kernel/native_flush.hpp): the flush as a native record (SR_NATIVE_FLUSH), or
  // the dry native flush compared with the guest body (SR_NATIVE_FLUSH_CHECK).
  if (!sr::native::flush::draw(ctx, base, 1)) {
#if SR_ORACLE
    if (sr::native::flush::checkOn()) sr::native::flush::checkBefore(ctx, base, 1);
#endif
    __imp__sub_820FBBF8(ctx, base);
#if SR_ORACLE
    if (sr::native::flush::checkOn()) sr::native::flush::checkAfter(ctx, base, 1);
#endif
  }
#if SR_ORACLE
  if (sr::native::ledger::g_on) sr::native::ledger::after(dev, sr::native::dev::kDrawAuto);
  if (sr::native::census::g_on) sr::native::census::drawAfter(sr::native::dev::kDrawAuto);
  if (nrec) sr::native::dev::onDrawAfter(dev);
#endif
}

extern "C" void __imp__sub_820FBBA0(PPCContext& __restrict ctx, uint8_t* base);
void sub_820FBBA0(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FBBA0);  // NATIVE N0 (scripts/gen_native_brackets.py)
#if SR_ORACLE
  API_OBS_PROLOGUE(s_endV);
  if (loud)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[API_OBS] EndVertices #%llu dev=%08X tid=%lu",
                (unsigned long long)n, ctx.r3.u32, GetCurrentThreadId());
  MIRROR1(EndVertices, endVertices.device = ctx.r3.u32);
  const uint32_t nrecDev = ctx.r3.u32;
  const bool nrec = sr::native::dev::on();
  if (nrec) sr::native::dev::onDrawBefore(nrecDev, sr::native::dev::kEndVertices, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  if (sr::native::census::g_on) sr::native::census::drawBefore(nrecDev, sr::native::dev::kEndVertices);
  if (sr::native::ledger::g_on) sr::native::ledger::before(nrecDev, sr::native::dev::kEndVertices);
#endif
  sr::kernel::resourceInlineEnd(ctx.r3.u32);
  __imp__sub_820FBBA0(ctx, base);
#if SR_ORACLE
  if (sr::native::ledger::g_on) sr::native::ledger::after(nrecDev, sr::native::dev::kEndVertices);
  if (sr::native::census::g_on) sr::native::census::drawAfter(sr::native::dev::kEndVertices);
  if (nrec) sr::native::dev::onDrawAfter(nrecDev);
  upDrain("End");   // iter-445 S1 (J)
#endif
}

#if SR_ORACLE
extern "C" void __imp__sub_82100310(PPCContext& __restrict ctx, uint8_t* base);
// ── iter-457 H1(a)-r3: THE OBSERVER, SPLIT OUT SO ITS DISPATCH IS TESTABLE ─
// ⛔ WHY THIS SPLIT EXISTS, and it is the third time this arc. r2 extracted
// the cube capture RULE into kernel/cube_capture_rule.hpp and tested it. The
// director then deleted the production DISPATCH
// (`if (false && cubeCaptureEnabled() && ...)`) and the suite still reported
// 399/399. Testing the body is not testing the wiring.
//
// Everything after the guest delegate now lives HERE, in a function the suite
// can call directly with guest memory installed. The dispatch condition the
// director mutated is INSIDE this body, so deleting it now fails a normal
// assertion instead of passing silently.
//
// It takes only `dev` and `slot`: the block references neither `base`, `loud`
// nor the call counter - verified by search before the move, not assumed. It
// RETURNS `fetchBase` because the MIRROR1 tail needs it, and that tail stays
// in the guest override since it is the one line that reads `ctx`.
uint32_t observeSetTextureFetch(uint32_t dev, uint32_t slot) {
  // ⚠ r5 IS NOT A TEXTURE. This is SetTextureFetchConstant: measured live, r5
  // is 0 on every call. The binding it performs is writing the 24-byte fetch
  // constant into dev+1024+24*slot, and dword1 of THAT is `base | 2` — the same
  // pixel VA the matching Lock returned (§6g.2). So the sampler's texture
  // identity is read from the fetch constant AFTER delegating, and it is the
  // PIXEL VA — which is why S4 keys its slot map on the pixel VA too, not on
  // the header handle (the header is a separate allocation ~15 MB away).
  uint32_t fetchBase = 0;
  if (slot < 32u) safeU32(dev + 1024u + 24u * slot + 4u, fetchBase);
  // ⛔⛔ iter-445 S1 — THE TEXTURE-IDENTITY BUG. This mask must match the one
  // the UPLOAD path keys on, and for a whole iteration it did not:
  //   upload  (this file, the census block below): key = basePage*4096
  //           = (f[1] >> 12) * 4096 = f[1] & 0xFFFFF000
  //   bind    (here, before this fix):             fetchBase = f[1] & ~3u
  // `s_texSlot` is INSERTED with the first and LOOKED UP with the second
  // (gfx/hle_video.cpp:362 vs :519). dword1's low bits are NOT address bits —
  // `fmt = f[1] & 0x3F` and endian = bits 6-7 — so the two keys agree ONLY when
  // fmt < 4 and endian == 0. That is the movie planes (fmt 2) and nothing else:
  // every general texture is DXT1(18)/DXN(49)/DXT4_5(20)/8888(6)/k_16(24), all
  // >= 4, so every one of them was stored under a key no bind could ever find.
  // ⚠ It also re-reads iter-444 S6g-b: "the capture->upload->bind->sample chain
  // works end to end" was concluded from shards carrying texture detail and warm
  // colour — which was general geometry sampling the MOVIE's YUV planes, the one
  // texture class whose two keys happen to agree.
  fetchBase &= 0xFFFFF000u;
  // iter-450UD H2/H3: observe HERE — the fetch constant is live and the capture
  // dedupe below has not yet had a chance to hide a later guest write. Default
  // OFF (SR_AUTOPSY_KEY); reads only, changes nothing.
  atlasProbe("bind", dev, slot);
  // ── iter-444 S6g-a: the census (contract at the TexSeen declaration) ───────
  // Reads the WHOLE 6-dword fetch constant, not just dword1, because the
  // design question needs format/dims/tiling, not just the address. Measures
  // only; captures nothing.
  if (slot < 32u && (texCensusEnabled() || texCaptureEnabled())) {
    const uint32_t fc = dev + 1024u + 24u * slot;
    uint32_t f[6] = {};
    bool ok = true;
    for (uint32_t i = 0; i < 6 && ok; i++) ok = safeU32(fc + 4u * i, f[i]);
    // Fetch slots are a UNION: texture slot N shares its six dwords with
    // vertex slots 3N..3N+2, so a non-kTexture type is normal hardware
    // behaviour, not corruption (xenos.h:1333) — skip, do not count.
    if (ok && (f[0] & 3u) == 2u) {
      const uint32_t basePage = (f[1] >> 12) & 0xFFFFFu;
      const uint32_t fmt = f[1] & 0x3Fu;
      const uint32_t width = (f[2] & 0x1FFFu) + 1u;
      const uint32_t height = ((f[2] >> 13) & 0x1FFFu) + 1u;
      const uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
      const bool tiled = ((f[0] >> 31) & 1u) != 0;
      // ── iter-449v: THE 3D/CUBE CENSUS (design input for the volume slice).
      // dimension = fetch dword 5 bits 9-10 (xenos.h:1325); 3D size packs
      // w:11 h:11 d:10 in dword 2 (xenos.h:1290-1294). Two dominant scene PS
      // do tfetch3D — those slots resolve WHITE until Texture3D lands. This
      // block only names the shapes: how many volumes, their sizes, formats,
      // and whether they are tiled (tiled 3D needs its own untiler).
      {
        const uint32_t texDim = (f[5] >> 9) & 3u;
        if (texDim == 2u || texDim == 3u) {
          static std::mutex s_dimMu;
          static std::set<uint32_t> s_dimSeen;
          std::lock_guard<std::mutex> lk(s_dimMu);
          const uint32_t vbase = (f[1] >> 12) << 12;
          if (s_dimSeen.size() < 64 && s_dimSeen.insert(vbase).second) {
            // Cube maps pack size_2d (w:13 h:13, xenos.h:1281-1284) with 6
            // implied faces — the first census run decoded size_3d for both
            // and printed impossible 128x509x81 "cubes".
            const bool isCube = texDim == 3u;
            const uint32_t w3 = isCube ? ((f[2] & 0x1FFFu) + 1u)
                                       : ((f[2] & 0x7FFu) + 1u);
            const uint32_t h3 = isCube ? (((f[2] >> 13) & 0x1FFFu) + 1u)
                                       : (((f[2] >> 11) & 0x7FFu) + 1u);
            const uint32_t d3 = isCube ? 6u : (((f[2] >> 22) & 0x3FFu) + 1u);
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[S6_TEX3D] %s slot=%u base=%08X fmt=%u %ux%ux%u tiled=%u "
                "endian=%u pitch=%u (distinct #%zu)",
                texDim == 2u ? "3D" : "CUBE", slot, vbase, fmt, w3, h3, d3,
                ((f[0] >> 31) & 1u), (f[1] >> 6) & 3u,
                ((f[0] >> 22) & 0x1FFu) << 5, s_dimSeen.size());
          }
        }
      }
      // ── iter-449x: THE VOLUME CAPTURE (Texture3D — the LUT class) ─────────
      // The 2D guard below never admits a volume (its size_3d word misdecodes
      // to width > 4096), so this is the ONLY 3D path. Census: every live
      // volume is 64x64x64 k_8_8_8_8 tiled (~1 MB — the 8 MB tex slots fit).
      // Cubes stay census-only; their slots keep the white cube dummy.
      if (basePage && texCaptureEnabled() && fmt == 6u &&
          ((f[5] >> 9) & 3u) == 2u) {
        const uint32_t w3 = (f[2] & 0x7FFu) + 1u;
        const uint32_t h3 = ((f[2] >> 11) & 0x7FFu) + 1u;
        const uint32_t d3 = ((f[2] >> 22) & 0x3FFu) + 1u;
        if (w3 <= 256u && h3 <= 256u && d3 <= 256u) {
          uint32_t pitch3 = pitchTexels ? pitchTexels : w3;
          const uint32_t base = basePage * 4096u;
          uint64_t span3;
          if (tiled) {
            pitch3 = (pitch3 + 31u) & ~31u;
            const uint32_t hA = (h3 + 31u) & ~31u;
            const uint32_t dA = (d3 + 3u) & ~3u;
            span3 = (uint64_t)pitch3 * hA * dA * 4u;
          } else {
            span3 = (uint64_t)pitch3 * h3 * d3 * 4u;
          }
          // Same dedupe set as 2D — the key mixes depth so it cannot collide
          // with a 2D key of the same base.
          const uint64_t key3 = ((uint64_t)base << 24) ^ ((uint64_t)fmt << 40) ^
                                ((uint64_t)w3 << 12) ^
                                ((uint64_t)h3 * 131u + d3) ^ (1ull << 63);
          bool doIt = false;
          {
            std::lock_guard<std::mutex> lk(s_texCapMu);
            if (s_texCaptured.size() < 100000)
              doIt = s_texCaptured.insert(key3).second;
          }
          if (doIt) {
            if (span3 > sr::gfx::hle::kTexSlotBytes) {
              std::lock_guard<std::mutex> lk(s_texCapMu);
              s_texCapBig++;
            } else {
              // iter-450UE FIX A: the alias choice is SHARED with the 2D path
              // below (SR_HLE_TEX_ALIAS_FULLSPAN). Gate OFF reproduces the
              // head-64 heuristic byte for byte.
              sr::kernel::physalloc::pinHistory(base, span3);   // iter-458 Q
              const AliasPick ap = pickAliasWindow(base, span3,
                                                   /*checkAmbiguity=*/true,
                                                   fmt, w3, h3);
              const uint8_t* p = ap.p;
              const uint32_t off =
                  p ? sr::gfx::hle::texWrite(p, (uint32_t)span3)
                    : sr::gfx::hle::kArenaFull;
              std::lock_guard<std::mutex> lk(s_texCapMu);
              if (off == sr::gfx::hle::kArenaFull) {
                s_texCapReject++;
                s_texCaptured.erase(key3);
              } else {
                s_texCapN++;
                s_texCapBytes += span3;
                sr::gfx::hle::RenderCommand tc;
                tc.type = sr::gfx::hle::RenderCommandType::TextureUpload;
                tc.textureUpload.key = base;
                tc.textureUpload.arenaOffset = off;
                tc.textureUpload.bytes = (uint32_t)span3;
                tc.textureUpload.dims = (w3 << 16) | (h3 & 0xFFFFu);
                tc.textureUpload.fmtBits =
                    (fmt & 0xFFu) | ((tiled ? 1u : 0u) << 8) |
                    (((f[1] >> 6) & 3u) << 12) | (2u << 14) |
                    ((d3 - 1u) << 16);
                tc.textureUpload.pitch = pitch3;
                sr::gfx::hle::enqueue(tc);
              }
            }
          }
        }
      }
      // ── iter-457 H1(a)-r2: THE CUBE CAPTURE, via THE SHARED BODY ────────
      // SR_HLE_CUBE_CAPTURE, DEFAULT OFF.
      //
      // ⛔ The rule itself lives in kernel/cube_capture_rule.hpp and this is
      // its ONLY production call site. It was inline here in H1(a)-r1, and the
      // director's verdict found the consequence: nothing could drive it, so
      // the mutant "the producer does not verify stack_depth == 5" survived
      // with the suite green. The self-test now drives the SAME body with
      // injected reader/arena/publisher, exactly as tex_unlock_rule.hpp does.
      //
      // Placed BEFORE the 2D guard below on purpose: that guard's
      // `width <= 4096 && height <= 4096` admits a 128x128 cube and captures
      // ONE face of six as a plain 2D image, which is how all four observed
      // cube keys came to be recorded at exactly 1/6 of their real size.
      bool cubeHandled = false;
      if (cubeCaptureEnabled() && texCaptureEnabled() &&
          ((f[5] >> 9) & 3u) == 3u) {
        cubeHandled = true;               // a cube NEVER falls to the 2D guard
        // ⛔ THE DISPATCH WITNESS (iter-457 H1(a)-r3). r2 tested the extracted
        // rule body and the director then DELETED THIS CALL SITE - the suite
        // still reported 399/399. This counter moves on every dispatch,
        // whatever the outcome, so deleting the dispatch is now a normal
        // assertion failure rather than a silent pass.
        s_cubeDispatches++;
        static std::vector<uint8_t> s_cubeBuf;
        s_cubeBuf.resize(sr::gfx::hle::kTexSlotBytes);
        uint64_t pendingKey = 0;
        const auto res = sr::kernel::cubecap::runCubeCapture(
            f, sr::gfx::hle::kTexSlotBytes, sr::gfx::hle::kArenaFull,
            [&](uint64_t k64) {
              pendingKey = k64;
              std::lock_guard<std::mutex> lk(s_texCapMu);
              if (s_texCaptured.size() >= 100000) return false;
              return s_texCaptured.insert(k64).second;
            },
            [&](uint32_t addr, uint32_t bytes) -> const uint8_t* {
              sr::kernel::physalloc::pinHistory(addr, bytes);   // iter-458 Q (base + mips)
              const AliasPick ap = pickAliasWindow(addr, bytes,
                                                   /*checkAmbiguity=*/true,
                                                   fmt, width, height);
              return ap.p;
            },
            [&](const uint8_t* p, uint32_t bytes) {
              return sr::gfx::hle::texWrite(p, bytes);
            },
            [&](const sr::kernel::cubecap::CubeCapResult& cr) {
              sr::gfx::hle::RenderCommand tc;
              tc.type = sr::gfx::hle::RenderCommandType::TextureUpload;
              tc.textureUpload.key = cr.key;
              tc.textureUpload.arenaOffset = cr.arenaOffset;
              tc.textureUpload.bytes = cr.baseBytes;
              tc.textureUpload.mipBytes = cr.mipBytes;
              tc.textureUpload.dims = cr.dims;
              tc.textureUpload.fmtBits = cr.fmtBits;
              tc.textureUpload.pitch = cr.pitch;
              sr::gfx::hle::enqueue(tc);
            },
            s_cubeBuf.data(), (uint32_t)s_cubeBuf.size());
        std::lock_guard<std::mutex> lk(s_texCapMu);
        if (res.published) {
          s_cubeCapPublished.fetch_add(1, std::memory_order_relaxed);
          s_texCapN++;
          s_texCapBytes += res.totalBytes;
          static uint64_t s_cubeCapN = 0;
          if (++s_cubeCapN <= 8)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[S6_CUBECAP] base=%08X mip=%08X %ux%u fmt=%u faces=6 "
                "faceStride=%u baseSpan=%u mipsTotal=%u total=%u mips=%u "
                "packed=%u", res.key, res.mipAddr, res.dims >> 16,
                res.dims & 0xFFFFu, res.fmtBits & 0xFFu, res.faceStride,
                res.baseBytes, res.mipBytes, res.totalBytes, res.mipLevels,
                res.packedMips ? 1u : 0u);
        } else {
          // A refusal that already consumed the dedupe slot must give it back,
          // or a transient read failure would permanently blacklist the key.
          using R = sr::kernel::cubecap::CubeCapRefuse;
          s_cubeCapLastRefuse.store(
              sr::kernel::cubecap::cubeCapRefuseName(res.refuse),
              std::memory_order_relaxed);
          if (pendingKey && (res.refuse == R::BaseUnreadable ||
                             res.refuse == R::MipUnreadable ||
                             res.refuse == R::ArenaFull)) {
            s_texCapReject++;
            s_texCaptured.erase(pendingKey);
          }
          static uint64_t s_cubeRefuseN = 0;
          if (res.refuse != R::Duplicate && ++s_cubeRefuseN <= 8)
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[S6_CUBECAP] REFUSED (%s) base=%08X %ux%u fmt=%u pitch=%u",
                sr::kernel::cubecap::cubeCapRefuseName(res.refuse),
                (f[1] >> 12) << 12, width, height, fmt, pitchTexels);
        }
      }
      if (!cubeHandled && basePage && width <= 4096u && height <= 4096u) {
        // ⛔ THE LEGACY-2D ENTRY WITNESS (iter-457 H1(a)-r3). A cube that fails
        // to set `cubeHandled` would be captured TWICE: once correctly as six
        // faces, and again here as ONE 2D face under the same base address -
        // which is exactly how all four observed cube keys came to be recorded
        // at 1/6 size in the first place. Counting entry makes that observable.
        s_legacy2DEntries.fetch_add(1, std::memory_order_relaxed);
        // ── iter-456n rework: THE ONE SHARED LAYOUT HELPER ──────────────────
        // Fifth adjudication, failed bar 1: this site used to call
        // `texBlockGeom` and duplicate its own span math, so tex_layout.hpp was
        // "shared" in name only. It now derives geometry and span from
        // texlayout::computeLayout — the SAME body the Unlock path uses.
        // `spanPadded` reproduces this site's historical expression bit-for-bit
        // (self-tested over a format/layout matrix), so adopting the helper
        // changes NOTHING about default behaviour; only the duplication dies.
        // ⚠ `known` keeps its old meaning: geometry the CAPTURE path can size.
        // The helper's `supported` flag is the BACKEND-decode predicate and is
        // deliberately stricter (it excludes depth-as-texture 22/23), so the
        // capture site must not adopt it — that would silently drop captures
        // this path has always made.
        uint32_t blockW = 1, blockBytes = 4;
        const bool known = texBlockGeom(fmt, blockW, blockBytes);
        const sr::kernel::texlayout::Layout capL =
            sr::kernel::texlayout::computeLayout(fmt, width, height, pitchTexels,
                                                 tiled, /*level=*/0);
        const uint32_t rowBlocks = capL.valid ? capL.rowBlocks
                                              : (width + blockW - 1u) / blockW;
        const uint32_t rows = capL.valid ? capL.rows
                                         : (height + blockW - 1u) / blockW;
        uint32_t blockPitch;
        if (capL.valid) {
          blockPitch = capL.blockPitch;
        } else {
          blockPitch = pitchTexels / blockW;
          if (tiled) blockPitch = (blockPitch + 31u) & ~31u;
          if (!blockPitch) blockPitch = rowBlocks;
        }
        // ⚠ A TILED SURFACE IS STORED AS IF IT WERE alignUp(rows,32) TALL, so
        // the untile READ SPAN exceeds rows*pitch. Sizing the snapshot at
        // rows*blockPitch*blockBytes under-reads: measured live as
        // "[S6_TEXUP] snapshot SHORT: 640x360 tiled=1 srcBytes=921600
        // needed>=921600" — needed EQUALLING srcBytes and still short, because
        // the tiler addresses row 383 of a 360-row surface. Caught by the
        // consumer's bounds check rather than by an out-of-bounds read, which
        // under the harness VEH would have been a fatal AV.
        const uint32_t spanRows = tiled ? ((rows + 31u) & ~31u) : rows;
        // Identical value, now produced by the shared helper when it applies.
        const uint64_t span = capL.valid
            ? capL.spanPadded
            : (uint64_t)spanRows * blockPitch * blockBytes;
        const uint32_t base = basePage * 4096u;
        // The key first — the CONTENT probe is deliberately deferred, see below.
        const uint64_t key = ((uint64_t)base << 24) ^ ((uint64_t)fmt << 40) ^
                             ((uint64_t)width << 12) ^ (uint64_t)height;
        // iter-445 S1: does this full-tuple key collapse onto `base` (all the
        // command can carry)? Count the collisions rather than assume either way.
        {
          std::lock_guard<std::mutex> lk(s_texCapMu);
          if (!known) s_texUnsupported++;
          auto ins = s_texIdent.emplace(base, TexIdent{fmt, width, height});
          if (!ins.second) {
            const TexIdent& p = ins.first->second;
            if (p.fmt != fmt || p.w != width || p.h != height) {
              const uint64_t a = ++s_texAlias;
              if (a <= 8)
                SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[S6_TEXALIAS] #%llu base=%08X was f%u %ux%u, now f%u %ux%u"
                    " — one slot, last writer wins",
                    (unsigned long long)a, base, p.fmt, p.w, p.h, fmt, width,
                    height);
            }
          }
        }

        // ── THE CAPTURE ────────────────────────────────────────────────────
        // Producer-side, because the render thread may never read guest memory.
        // At BIND, because 883 distinct textures are bound in a run against 104
        // creates and 133 non-movie Unlocks — most never touch D3D9's lock path
        // at all (PROGRESS §iter-444 S6g-a).
        if (texCaptureEnabled() && known && span >= 64) {
          // iter-458 (Astra 069): the census observation starts BEFORE any
          // snapshot - its FIFO-clock lower bound and the registry generation.
          const sr::kernel::txc::BindToken txcTok = sr::kernel::txc::bindBegin();
          bool doIt = false, revisit = false, txcIssued = false;
          uint32_t txcWindow = 0;   // the alias the snapshot was taken from
          {
            std::lock_guard<std::mutex> lk(s_texCapMu);
            if (s_texCaptured.size() < 100000)
              doIt = s_texCaptured.insert(key).second;
            // ── iter-450UG PROOF GATE: the ONE new statement in this critical
            // section. It short-circuits on a function-local `static const
            // bool`, so with SR_HLE_TEX_REFRESH unset the emitted path is
            // exactly today's: insert -> false -> nothing. No guest read, no
            // hash, no allocation, no ordering change.
            // ⚠ `count(key)` rather than merely `!doIt`, so a key skipped by the
            // 100000 cap is not mistaken for a previously-captured one.
            if (!doIt && texRefreshEnabled() && s_texCaptured.count(key))
              revisit = true;
          }
          if (revisit)
            texRefreshRevisit(key, base, span, fmt, width, height, tiled,
                              (f[1] >> 6) & 3u, pitchTexels,
                              (uint64_t)rows * blockPitch * blockBytes);
          if (doIt) {
            if (span > sr::gfx::hle::kTexSlotBytes) {
              std::lock_guard<std::mutex> lk(s_texCapMu);
              s_texCapBig++;   // NO SILENT CAPS: too big to snapshot, counted
            } else {
              // iter-450UE FIX A: SHARED with the 3D path above
              // (SR_HLE_TEX_ALIAS_FULLSPAN). The head-64 probe this replaces
              // could not see a payload that starts past byte 64 — which is
              // exactly the PRESS START atlas (iter-450UD).
              sr::kernel::physalloc::pinHistory(base, span);   // iter-458 Q
              const AliasPick ap = pickAliasWindow(base, span,
                                                   /*checkAmbiguity=*/true,
                                                   fmt, width, height);
              const uint8_t* p = ap.p;
              const uint32_t off =
                  p ? sr::gfx::hle::texWrite(p, (uint32_t)span)
                    : sr::gfx::hle::kArenaFull;
              std::lock_guard<std::mutex> lk(s_texCapMu);
              if (off == sr::gfx::hle::kArenaFull) {
                s_texCapReject++;
                // Un-mark so a later bind can retry — a texture dropped for a
                // transient pool-full must not be lost for the whole run.
                s_texCaptured.erase(key);
              } else {
                s_texCapN++;
                s_texCapBytes += span;
                // iter-450UG: seed version 0 from the EXACT pointer and window
                // that were uploaded, so the stored digest provably describes
                // the bytes we published. Gate OFF: not evaluated past the bool.
                // Seeded only for keys the filter admits, so `keys=` in the
                // census is provably the filtered set (keys=1 when filtered).
                if (texRefreshEnabled() && p &&
                    (!texRefreshKeyFilter() ||
                     (base & 0x1FFFFFFFu) == texRefreshKeyFilter())) {
                  const AtlasAliasView sv = scanSpanBytes(p, span);
                  TexRefreshState st;
                  st.digest = sv.hash;
                  st.zeroSeed = (sv.nonzero == 0);   // iter-456h: ZNZ join arm
                  st.seedNonzero = sv.nonzero;       // iter-456j: before-side
                  st.seedSwap = s_swap.n.load(std::memory_order_relaxed);
                  st.bytes = (uint32_t)span;
                  st.window = ap.window;
                  st.version = 0;
                  s_texRefresh.emplace(key, st);
                }
                sr::gfx::hle::RenderCommand tc;
                tc.type = sr::gfx::hle::RenderCommandType::TextureUpload;
                tc.textureUpload.key = base;
                tc.textureUpload.arenaOffset = off;
                tc.textureUpload.bytes = (uint32_t)span;
                tc.textureUpload.dims = (width << 16) | (height & 0xFFFFu);
                tc.textureUpload.fmtBits =
                    (fmt & 0xFFu) | ((tiled ? 1u : 0u) << 8) |
                    (((f[1] >> 6) & 3u) << 12);
                tc.textureUpload.pitch = pitchTexels;
                sr::gfx::hle::enqueue(tc);
                txcIssued = true;
                txcWindow = ap.window;
              }
            }
          }
          // iter-458 (Astra 068/069): the texture-path census - an ISSUED upload
          // is the descriptor's baseline (its own payload, its own alias); a
          // SUPPRESSED bind (descriptor already uploaded) is checked for new
          // owners / new bytes in that alias. A doIt that did not publish (too
          // big, arena full) records nothing: the next bind retries.
          if (sr::kernel::txc::enabled() && (txcIssued || !doIt))
            sr::kernel::txc::onTexDecode(
                txcTok, base, (uint32_t)span,
                (fmt & 0xFFu) | ((tiled ? 1u : 0u) << 8) | (((f[1] >> 6) & 3u) << 12),
                (width << 16) | (height & 0xFFFFu), txcIssued, txcWindow);
        }
        bool probeThis = false;
        {
          std::lock_guard<std::mutex> lk(s_texCensusMu);
          s_texBinds++;
          if (fmt < 64) s_texFmtHist[fmt]++;
          if (tiled) s_texTiled++; else s_texLinear++;
          if (!known) s_texUnsized++;
          if (span > s_texMaxBytes && span < (64u << 20)) s_texMaxBytes = (uint32_t)span;
          // FIRST SIGHT THIS WINDOW is the only time we probe content. Binds
          // run ~7.8M/run; probing each one would mean 7.8M VirtualQuery walks
          // AND 1.5 GB of hashing. Once per distinct key per 60-frame window is
          // ~800 probes and still answers the mutability question, because the
          // comparison is against the SAME key's hash from a previous window.
          if (s_texFrameKeys.size() < 100000 && s_texFrameKeys.insert(key).second) {
            s_texFrameBytes += span;
            probeThis = s_texProbes < 4096;
            if (probeThis) s_texProbes++;
          }
        }
        if (probeThis) {
        // ── The CONTENT fingerprint (3 x 64 B head/middle/tail) ─────────────
        // ⚠ THE FETCH CONSTANT'S BASE IS PHYSICAL. guestVAToHost indexes by the
        // VA ITSELF (base + va, after aliasing), NOT by va & 0x1FFFFFFF — so
        // handing it the raw 0x0F807000 reads a different, normally
        // uncommitted, address. The first version did exactly that and reported
        // mutated=0 across 7.8M binds: not a finding, an artifact of hashing an
        // unreadable location. Normalise the way the S4 vertex path does.
        //
        // ⚠ AND isGuestVAInRange IS NOT ENOUGH. It bounds the address space, not
        // the COMMIT state; the second version passed it, faulted, and the
        // harness VEH killed the roll (crash=1). hostRangeReadable is THE
        // fault-free predicate (guest_context.hpp).
        const uint32_t readVA = 0x80000000u | (base & 0x1FFFFFFFu);
        uint64_t h = 1469598103934665603ull;
        bool sampled = false;
        if (span >= 64 && span < (64u << 20)) {
          const uint8_t* p = sr::kernel::guestVAToHost(readVA);
          if (p && !sr::kernel::hostRangeReadable(p, (size_t)span)) p = nullptr;
          // iter-425 alias class: our harness does not fold the physical
          // windows, so a surface written through an 0xA-class VA reads ALL
          // ZERO at the 0x8 window. Prefer whichever window actually has data.
          if (p) {
            bool zero8 = true;
            for (uint32_t bb = 0; bb < 64; bb++) if (p[bb]) { zero8 = false; break; }
            if (zero8) {
              const uint32_t altVA = 0xA0000000u | (base & 0x1FFFFFFFu);
              if (const uint8_t* alt = sr::kernel::guestVAToHost(altVA))
                if (sr::kernel::hostRangeReadable(alt, (size_t)span))
                  for (uint32_t bb = 0; bb < 64; bb++)
                    if (alt[bb]) { p = alt; break; }
            }
          }
          if (p) {
            const uint64_t offs[3] = {0, span / 2u, span - 64u};
            for (int k = 0; k < 3; k++)
              for (uint32_t bb = 0; bb < 64; bb++)
                h = (h ^ p[offs[k] + bb]) * 1099511628211ull;
            sampled = true;
          }
        }
        {
          std::lock_guard<std::mutex> lk(s_texCensusMu);
          if (sampled) s_texSampled++; else s_texUnreadable++;
          if (sampled && s_texSeen.size() < 100000) {
            auto it = s_texSeen.find(key);
            if (it == s_texSeen.end()) {
              s_texSeen.emplace(key, TexSeen{h, 1, 0});
            } else {
              it->second.binds++;
              if (it->second.sampleHash != h) {
                it->second.sampleHash = h;
                it->second.changes++;
                s_texChanged++;
              }
            }
          }
        }
        }  // probeThis
      }
    }
  }
  return fetchBase;
}

void sub_82100310(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x82100310);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_setTex);
  if (loud)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[API_OBS] SetTexture #%llu dev=%08X slot=%u tex=%08X dirty=%08X tid=%lu",
                (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                GetCurrentThreadId());
  const uint32_t dev = ctx.r3.u32, slot = ctx.r4.u32;
  // ── iter-457 H1(a)-r4: A BOUNDED TEST-ONLY DELEGATE BYPASS ────────────────
  // The director required C13 to enter THIS function so the wrapper-to-observer
  // call is covered (replacing it with 0 previously built and left 410/410
  // green). Executing the real 94-instruction guest body offline is possible
  // but carries real hazards the recon enumerated: an uninitialised r1+80 read,
  // save/restore-GPR helpers, and indirect calls through a dispatch table the
  // suite does not populate - and the harness VEH makes any host AV FATAL.
  //
  // The director explicitly allowed "an equivalently bounded test-only delegate
  // seam". This is it: one relaxed atomic load, default false, and the arming
  // setter is STRUCTURALLY gated on SR_HLE_TEX_UNLOCK_TEST, so the bypass
  // cannot be armed outside the self-test process.
  //
  // ⚠ NOT "production is byte-identical" - that was overstated and the
  // director corrected it (iter-457 r5). The accurate statement: DEFAULT
  // PRODUCTION BEHAVIOUR IS UNCHANGED - the guest body runs unconditionally,
  // because the flag is false and can never be set outside the suite. The
  // CHECK ITSELF IS ALWAYS PRESENT: one relaxed atomic load and a branch on
  // every SetTexture, in every build, including the shipping default. That is
  // a real (if small) cost, and it is named here rather than absorbed into a
  // word that means something stronger than what was measured.
  //
  // With the flag set, the fixture writes the fetch constant itself - exactly
  // what the delegate would have written - and everything AFTER this line (the
  // dev/slot capture, the observer call, MIRROR1) runs for real.
  //
  // ⚠ WHAT THIS DOES NOT COVER, stated rather than implied: the delegate's own
  // header->fetch-constant transform. That is generated guest code, not ours.
  if (!s_delegateBypassForTest.load(std::memory_order_relaxed)) {
    // mem.resources: the outgoing texture's header fence stamp (kernel/native_resource_stamps.hpp).
    const uint32_t resourceStamp = sr::kernel::resourceTextureStampTarget(dev, slot);
    __imp__sub_82100310(ctx, base);
    sr::kernel::resourceFenceStampDone(resourceStamp);
  }
  // The observer reads the fetch constant the delegate just wrote. It is a
  // separate function so the suite can drive it; see its header comment.
  const uint32_t fetchBase = observeSetTextureFetch(dev, slot);
  MIRROR1(SetTexture, setTexture.device = dev; c.setTexture.sampler = slot;
                      c.setTexture.texture = fetchBase;
                      c.setTexture.dirty = ctx.r6.u32);
}

extern "C" void __imp__sub_820F2810(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F2810(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F2810);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_setVp);
  if (loud)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[API_OBS] SetViewport #%llu dev=%08X vp=%08X tid=%lu",
                (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, GetCurrentThreadId());
  // iter-446 R0b: DECODE, don't forward the pointer. D3DVIEWPORT9 is
  // {DWORD X,Y,Width,Height; float MinZ,MaxZ}; MinZ/MaxZ are published as raw
  // FLOAT BITS (readGuestU32BE already byte-swaps them). An unreadable or NULL
  // pViewport leaves width==height==0, which the executor treats as "no
  // viewport" — never as a zero-area one it should honour.
  const uint32_t dev = ctx.r3.u32, vpVA = ctx.r4.u32;
  uint32_t vx = 0, vy = 0, vw = 0, vh = 0, vzn = 0, vzf = 0;
  if (apiMirrorEnabled() && vpVA >= 0x1000u) {
    safeU32(vpVA + 0u, vx);  safeU32(vpVA + 4u, vy);
    safeU32(vpVA + 8u, vw);  safeU32(vpVA + 12u, vh);
    safeU32(vpVA + 16u, vzn); safeU32(vpVA + 20u, vzf);
  }
  if (rtCensusEnabled() && s_vpDumped < 64u) {
    s_vpDumped++;
    // ⚠ iter-446: CORRELATE THE VIEWPORT WITH THE SURFACE IT IS SET ON. The
    // surface header has no height field, so we derive it from
    // bytes/(pitch*bpp*samples) — and the derived values (720, 368, 192, 96)
    // are every one of them ceil(h/16)*16 of the pyramid the guest should have
    // (720, 360, 180, 90). That is the EDRAM TILE-PADDING signature: a tile is
    // 80x16 pixels, so `bytes` may describe the PADDED footprint rather than
    // the live extent. If so, every non-primary pooled surface is ~2% too tall,
    // carries garbage rows, and scales normalised sampling wrong — exactly the
    // "subtly wrong and invisible" class. The viewport is the INDEPENDENT
    // witness: it is set immediately after the tile config on every RT bind.
    uint32_t rt0 = 0, si = 0, ci = 0, bs = 0, sw = 0;
    safeU32(dev + 12168u, rt0);
    readSurfaceHeader(rt0, si, ci, bs, sw);
    const sr::gfx::hle::SurfaceDesc d =
        sr::gfx::hle::decodeSurface(si, ci, bs, sw, /*isDepth=*/false);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_VP] #%llu vp=%08X | x=%d y=%d %ux%u minZ=%.3f maxZ=%.3f "
        "|| rt0=%08X derived=%ux%u pitch=%u msaa=%u bytes=0x%X %s",
        (unsigned long long)n, vpVA, (int)vx, (int)vy, vw, vh,
        (double)bitsToFloat(vzn), (double)bitsToFloat(vzf), rt0, d.width,
        d.height, d.pitch, d.msaa, d.bytes,
        (d.valid && vw && vh && d.width == vw && d.height == vh) ? "MATCH"
        : (d.valid && vw && vh) ? "*** MISMATCH ***" : "-");
  }
  MIRROR1(SetViewport, setViewport.device = dev; c.setViewport.x = vx;
                       c.setViewport.y = vy; c.setViewport.width = vw;
                       c.setViewport.height = vh; c.setViewport.minZ = vzn;
                       c.setViewport.maxZ = vzf);
  __imp__sub_820F2810(ctx, base);
}

extern "C" void __imp__sub_820F2720(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F2720(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F2720);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_setSc);
  if (loud)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[API_OBS] SetScissorRect #%llu dev=%08X rect=%08X tid=%lu",
                (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, GetCurrentThreadId());
  // iter-446 R0b: same defect, same fix — RECT is {LONG left, top, right,
  // bottom}. A NULL pRect is D3D9 for "scissoring disabled"; it lands here as
  // all-zeros, which the executor must read as "no scissor", not as an empty
  // one that would reject every pixel.
  const uint32_t dev = ctx.r3.u32, rcVA = ctx.r4.u32;
  uint32_t sl = 0, st = 0, sr_ = 0, sb = 0;
  if (apiMirrorEnabled() && rcVA >= 0x1000u) {
    safeU32(rcVA + 0u, sl);   safeU32(rcVA + 4u, st);
    safeU32(rcVA + 8u, sr_);  safeU32(rcVA + 12u, sb);
  }
  if (rtCensusEnabled() && s_scDumped < 16u) {
    s_scDumped++;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_SC] #%llu rect=%08X | l=%d t=%d r=%d b=%d",
        (unsigned long long)n, rcVA, (int)sl, (int)st, (int)sr_, (int)sb);
  }
  MIRROR1(SetScissorRect, setScissorRect.device = dev; c.setScissorRect.left = sl;
                          c.setScissorRect.top = st; c.setScissorRect.right = sr_;
                          c.setScissorRect.bottom = sb);
  __imp__sub_820F2720(ctx, base);
}

// CreateIndexBuffer stays observe-only: resource CREATION is synchronous under
// delegation (the guest allocates and owns it) — nothing to order against draws.
extern "C" void __imp__sub_820F4040(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F4040(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F4040);  // NATIVE N0 (scripts/gen_native_brackets.py)
  API_OBS_PROLOGUE(s_ibCreate);
  const uint32_t sizeBytes = ctx.r3.u32, usage = ctx.r4.u32, fmt = ctx.r5.u32;
  __imp__sub_820F4040(ctx, base);
  if (loud)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[API_OBS] CreateIndexBuffer #%llu size=%u usage=%08X fmt=%08X -> ib=%08X",
                (unsigned long long)n, sizeBytes, usage, fmt, ctx.r3.u32);
}
#endif
// NOT hooked here — already strong-def'd elsewhere; adding a second definition
// is a link error, and these are exactly the sites Phase 2 must eventually take
// over, so the existing owner is where that conversation happens:
//   SetVertexShader   sub_820F5218 -> kernel/cs_timeout_underflow_fix.cpp:3624
//   SetPixelShader    sub_820F4E88 -> kernel/cs_timeout_underflow_fix.cpp:3736
//   CreateVertexBuffer sub_820F3ED8 -> kernel/apt_render_watch.cpp:541
//
// ⛔ iter-447: `vbCreate=0` WAS AN INSTRUMENT THAT COULD NOT FAIL. The counter
// had no increment site anywhere — it appeared only in its declaration and in
// the SUMMARY2 format string — so it read 0 BY CONSTRUCTION, and a reader (me,
// this session) quoted it as evidence that the guest creates no vertex buffers.
// It carried zero bits. That is the same defect class as `nonBlackPx`
// (iter-446) and the retired `aptTick`/`defBuilt` gauges: a number that reads
// the same whether the thing happens or not.
//
// The honest options were DELETE or MAKE REAL. Made real — whether the guest
// uses D3D9's buffer API at all is load-bearing for the VB/IB slice (the
// reference's whole Create/Lock/Unlock model assumes it does, and `ibCreate=7`
// against ~3.06M indexed draws says ours largely does not). The owning hook now
// calls the notifier below. ⚠ `s_setVS`/`s_setPS` are STILL dead by the same
// mechanism — they are labelled `(unhooked)` in SUMMARY2 rather than printed as
// if measured. Do not read them; give them notifiers or delete them.
// ⚠ Wrapped explicitly: line 46's `namespace sr { namespace kernel {` closes at
// line 48 (it is a forward-declaration block), so the REST OF THIS FILE is at
// global scope. Defining this bare would produce `::noteVertexBufferCreate` and
// a link error against the `sr::kernel::` declaration in apt_render_watch.cpp.
// ── iter-447 [S6_VBID]: DOES THE D3D9 CREATE PATH PRODUCE THE BUFFERS WE DRAW? ─
// THE question the reference read left open. UnleashedRecomp's whole per-buffer
// model assumes geometry flows through CreateVertexBuffer/Lock/Unlock. For INDEX
// buffers ours measurably does not (ibCreate=7 vs ~2.8M indexed draws, and
// PROGRESS.md:1047-1053 found the BOUND IBs in a different address range from
// what the create path returns). The vertex side was never tested — it rode on
// `vbCreate=0`, a counter with no increment site (fixed a039f78, reads 3506).
//
// ⚠ WHY TWO SETS. The create call returns the D3D9 VB *HEADER* VA, while draws
// resolve a *DATA* base from the vertex fetch constant — the exact mismatch the
// IB probe found. Comparing only the returned VA would report a miss and prove
// nothing. So we also harvest the plausible pointers INSIDE the header and test
// against those too. A hit on either set answers the question; a miss on BOTH,
// with creates > 0, is the strong result (the drawn geometry does not come from
// D3D9's buffer API at all).
// State lives beside its [S6_UNIQ] sibling at the head of this file — the
// producer and the reporter both run long before this point.
namespace sr { namespace kernel {
// Called by the OWNING hook (apt_render_watch.cpp). vbVA is the returned header.
void noteVertexBufferCreate(uint32_t vbVA) {
#if SR_ORACLE
  s_vbCreate.bump();
  if (!vbVA) return;
  std::lock_guard<std::mutex> lk(s_vbIdMu);
  if (s_vbCreatedHdr.size() >= 65536) return;   // bounded; 3,506/run measured
  if (s_vbCreatedHdr.insert(vbVA & 0x1FFFFFFFu).second) {
    if (s_vbIdNEgHdr < 4) s_vbIdEgHdr[s_vbIdNEgHdr++] = vbVA;
  }
  // Fault-free: VirtualQuery-backed range check then a host read. The harness
  // VEH makes any host AV fatal before SEH filters run, so this must never
  // fault-and-catch (docs/TOOLKIT + the thread-registry rule).
  for (uint32_t off = 0; off < 32u; off += 4u) {
    if (!sr::kernel::isGuestVAInRange(vbVA + off + 4u)) break;
    const uint32_t v = sr::kernel::readGuestU32BE(vbVA + off);
    // A plausible guest pointer, not a size/flag word.
    if (v >= 0x10000u && v < 0xC0000000u)
      s_vbCreatedPtr.insert(v & 0x1FFFFFFFu);
  }
#else
  (void)vbVA;   // SR_ORACLE=0: the [API_OBS] / [S6_VBID] census it fed is compiled out
#endif
}
}}  // namespace sr::kernel

#if SR_ORACLE
// ── iter-455n Phase 2 v1 r2: the PRODUCTION feature gate, at GLOBAL scope so its
// symbol IS sr::kernel::texid::productionGateEnabled (declared in
// tex_identity_rule.hpp; the hooks call it via the anon-namespace alias above).
// Cached once — the env reader runs exactly once; productionGateInitCount() makes
// that observable so the self-test kills a per-call-getenv mutant (r2 blocker 3).
namespace sr { namespace kernel {

// Readable by the suite so the DEFAULT can be asserted, not assumed.
bool cubeCaptureEnabledForTest() { return cubeCaptureEnabled(); }
bool cubeGateWantsOnForTest(const char* e) { return cubeGateWantsOn(e); }
// The STORED state, not the setter's return value. The director found that
// asserting only the return proves nothing about a process where the gate is
// off: deleting the refusal left the in-suite assertion green. A gate-off
// process reads this to prove the flag never moved.
bool setTextureDelegateBypassStateForTest() {
  return s_delegateBypassForTest.load(std::memory_order_relaxed);
}
// ⛔ STRUCTURALLY GATED (director, iter-457 r5). The bypass can only be armed
// inside the self-test process. Guarding it on the caller's discipline is not a
// guard at all: a seam that CAN be reached in production is a production path,
// whatever its name says. The env read is latched once, so this costs nothing
// after the first call and cannot be flipped mid-run.
bool setSetTextureDelegateBypassForTest(bool on) {
  static const bool inSuite = [] {
    const char* e = std::getenv("SR_HLE_TEX_UNLOCK_TEST");
    return e && e[0] == '1';
  }();
  if (!inSuite) {
    s_delegateBypassForTest.store(false, std::memory_order_relaxed);
    return false;                       // refused, and it says so
  }
  s_delegateBypassForTest.store(on, std::memory_order_relaxed);
  return true;
}

// iter-457 H1(a)-r3: the PRODUCTION dispatch, published and refused counts,
// readable by the suite. Uncapped and log-free.
uint64_t cubeDispatchesForTest() { return s_cubeDispatches.load(std::memory_order_relaxed); }
uint64_t legacy2DEntriesForTest() { return s_legacy2DEntries.load(std::memory_order_relaxed); }
uint64_t cubeCapPublishedForTest() { return s_cubeCapPublished.load(std::memory_order_relaxed); }
const char* cubeCapLastRefuseForTest() {
  const char* p = s_cubeCapLastRefuse.load(std::memory_order_relaxed);
  return p ? p : "";
}
void resetCubeCapCountersForTest() {
  s_cubeDispatches.store(0, std::memory_order_relaxed);
  s_legacy2DEntries.store(0, std::memory_order_relaxed);
  s_cubeCapPublished.store(0, std::memory_order_relaxed);
  s_cubeCapLastRefuse.store(nullptr, std::memory_order_relaxed);
}
// The observer body, so the suite can drive the REAL dispatch.
uint32_t observeSetTextureFetchForTest(uint32_t dev, uint32_t slot) {
  return observeSetTextureFetch(dev, slot);
} namespace texid {
namespace { std::atomic<int> s_gateInitCount{0}; }
bool productionGateEnabled() {
  static const bool s_on = [] {
    s_gateInitCount.fetch_add(1, std::memory_order_relaxed);   // once, at first use
    const char* e = std::getenv("SR_HLE_TEX_IDENTITY");
    return e && e[0] == '1';
  }();
  return s_on;
}
int productionGateInitCount() { return s_gateInitCount.load(std::memory_order_relaxed); }
}}}  // namespace sr::kernel::texid

// Tested at the DISTINCT-KEY point of the [S6_UNIQ] census (~117 keys / 60
// frames), never per draw — the per-draw path is 2.25M/run and iter-445 already
// measured that putting a lock there costs ~60% of throughput.
void vbIdTest(uint32_t drawnBase) {
  const uint32_t norm = drawnBase & 0x1FFFFFFFu;
  std::lock_guard<std::mutex> lk(s_vbIdMu);
  s_vbIdTested++;
  const bool h = s_vbCreatedHdr.count(norm) != 0;
  const bool p = s_vbCreatedPtr.count(norm) != 0;
  if (h) s_vbIdHitHdr++;
  if (p) s_vbIdHitPtr++;
  if (!h && !p) {
    s_vbIdMiss++;
    if (s_vbIdNEgDrawn < 4) s_vbIdEgDrawn[s_vbIdNEgDrawn++] = drawnBase;
  }
}

// ── iter-449z: THE 0x1E PROBE (observe, always-on) ──────────────────────────
// Render-script opcode 0x1E (RenderScriptRunTechnique_sub_8277B998:0x8277BE30)
// performs String assignment 826057D8, then optionally Locks entry+12.
// IDA 2026-10-17: the output is unused at 8277BE64. This is not an established
// CPU exposure read. This historical hook's LR attribution is also unavailable
// in skip_lr builds. Actual row-copy observation uses exact 823A1194 hooks.
extern "C" void __imp__sub_826057D8(PPCContext& __restrict ctx, uint8_t* base);
void sub_826057D8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint64_t> s_n{0}, s_null{0};
  const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t tex = 0;
  if (ctx.r3.u32 >= 0x10010u) tex = sr::kernel::readGuestU32BE(ctx.r3.u32 - 4u);
  if (!tex) s_null.fetch_add(1, std::memory_order_relaxed);
  // Historical call-site label only; not evidence of a pixel read.
  const bool isLockSite = (uint32_t)ctx.lr == 0x8277BE44u;
  if (isLockSite || n <= 12 || (n & 0x3FFu) == 0u)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[SCRIPT_1E]%s #%llu entry=%08X tex(+12)=%08X lr=%08X (null=%llu)",
        isLockSite ? " LOCKSITE" : "",
        (unsigned long long)n, ctx.r3.u32 - 16u, tex,
        (uint32_t)ctx.lr,
        (unsigned long long)s_null.load(std::memory_order_relaxed));
  __imp__sub_826057D8(ctx, base);
}
#endif
