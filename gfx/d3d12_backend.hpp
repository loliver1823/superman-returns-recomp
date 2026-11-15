#pragma once
#include <chrono>

#include <cstdint>
#include <atomic>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#endif

#include "surface_key.hpp"     // P1 / C1 stage 3: the surface identity, the format math, the alias geometry (shipping)
#include "native_image_store.hpp"
#include "native_pipeline.hpp"
#include "native_draw.hpp"
#include "native_zpd.hpp"
#include "translated_constants.hpp"
#include "xlat_cb_gpu.hpp"  // step 6 I10p: the GPU-visible, write-only twin of the constant scratch
#include "translated_samplers.hpp"
#include "translated_textures.hpp"
#include "tex_evict_policy.hpp"  // step 4 G7 (lane D): SR_TEX_CACHE_EVICT's policy
#include "../kernel/sr_oracle.hpp"
#if SR_ORACLE
#include <array>
#include <map>
#include "hle_prov_ring.hpp"   // provenance payload carried on HleBatch (r3)
#endif
#if SR_ORACLE
#include "cube_bind_rule.hpp"  // iter-457 H1(b): the cube decisions
#include "cube_transaction.hpp"  // iter-457 H1(b)-r5: THE upload transaction
#endif

namespace sr {
namespace gfx {

struct GuestDrawCommand;
struct CachedShader;
namespace cube { struct CubeLayout; }
namespace native_owner { struct Plan; }
namespace livetrace { struct InitState; }
namespace gfxopt { struct Settings; }   // iter-466 gfx: gfx_options.hpp (the present chain's options)
#if SR_ORACLE
namespace memres { struct FetchCapture; struct FetchCaptureInput; struct FetchAnalysis; struct FetchPagePayload; struct FetchPayloadIntersection; }
namespace memres { struct ConsumerBaseline; struct ConsumerCheck; struct ConsumerWitness; struct ConsumerFetchLease; }
#endif
namespace memres { struct ResourceSpan; }  // step 6 I4: pm4ResourceRequestSite (both builds)

// Step 4 item 7d(b) (lane D, 2026-10-20; SR_XLAT_SCRATCH_COMMIT=1 opts in - the default is the zero-filled
// std::vector as before. fix12c, orchestrator 12:5x: NOT baked in step 4. Any read of a not-yet-committed range is an
// access violation, so it waits for step 6, with item 1, behind a first-touch fault safety net: a VEH limited to this
// reservation that commits the page and logs [XLAT_SCRATCH_FAULT]): the translated-draw CPU scratch (kXlatVBBytes /
// kXlatCBBytes, 3 thirds) as ONE address reservation whose
// pages are committed by high-water: each third's committed end only grows, in 1 MB steps, and commit() runs BEFORE
// any write into a new range (D3D12Backend::xlatCommit at the four cursor checks of the drain and pm4StageVsf). The
// release census (lane D 7d, NOTES 2026-10-20 11:17): 512 + 192 MB committed and resident from boot vs a measured
// per-third use of 4.8 + 28.6 MB. size() / data() are the reservation's, so every bounds check reads as before.
class XlatScratch {
 public:
  XlatScratch() = default;
  XlatScratch(const XlatScratch&) = delete;
  XlatScratch& operator=(const XlatScratch&) = delete;
  ~XlatScratch();
  // reserveOnly: the reservation (nothing committed yet); else a zero-filled vector (all committed). false: no memory.
  // A second init replaces the first (the difftests size a backend's scratch directly: init(n, false)).
  bool init(uint64_t bytes, bool reserveOnly);
  uint8_t* data() const { return m_base; }
  uint64_t size() const { return m_bytes; }
  // Commits up to `end` (an offset into the buffer) inside the third that holds end - 1. false: the commit failed
  // (the caller rejects the draw as when the third is full). Always true for the vector form.
  bool commit(uint64_t end, bool* grew = nullptr);
  uint64_t committedBytes() const;   // the reservation's committed total (the vector form: its size)

 private:
  uint8_t* m_base = nullptr;
  uint64_t m_bytes = 0;
  bool m_reserved = false;
  std::vector<uint8_t> m_vec;
  std::atomic<uint64_t> m_end[3] = {};   // per third: the committed end (an absolute offset; 0 = none yet)
};

// iter-460 S4: one 2D texture for the shared-memory GPU loader
// (D3D12Backend::pm4ShmLoadTexture). layout == nullptr: base level only.
struct ShmTexDesc {
  uint32_t fmt = 0, width = 0, height = 0, pitchTexels = 0, blockW = 1, blockBytes = 4;
  uint32_t endian = 0, basePhys = 0, mipPhys = 0, levels = 1;
  bool tiled = false;
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  const cube::CubeLayout* layout = nullptr;
  uint32_t subresource = 0;  // first destination subresource (an array layer)
  // iter-463 (SR_PM4_ARRAY_MIPS): the array slice this load reads, placed by
  // `layout` (placeSubresource face): basePhys/mipPhys are then the ARRAY's
  // base and mip addresses, and level m goes to subresource + m.
  uint32_t face = 0;
  // iter-464 resscale: > 1 = a resolution-scaled texture (Xenia texture key
  // scaled_resolve): the resource is scale x the guest size and loads from the
  // scaled resolve memory with the texture_load_*_scaled shaders.
  uint32_t scaleX = 1, scaleY = 1;
};
bool pm4ShmEnabled();
bool pm4ShmLoadable(uint32_t fmt);
// iter-466 res4k round 2 (SR_TEX_LOAD_DIRECT, gfx/tex_load_direct.inc)
bool pm4TexLoadDirectOn();
// iter-467 native-b B2: SR_NB_RESOLVE_TEX - textures that may become native targets are created typeless + UAV
// (gfx/native_targets.inc), the 16bpp ones too.
bool pm4NbTargetsOn();
bool pm4NbScaledRemapOn();  // iter-467 native-b SR_NB_SCALED_REMAP (gfx/native_resolve.inc)
bool pm4NbTargets1xOn();    // iter-467 native-b SR_NB_TARGETS_1X (gfx/native_targets.inc)
// iter-467 native-b: the EDRAM model's work per frame, for the NATIVE METER (strand A; owner 2026-10-07: "a native
// PC port, not some glorified emulator" - the shipping goal is every field but resolvesTarget at zero).
// Counted on the render thread, published at the end of each PM4 frame (gfx/d3d12_pm4_edram.cpp).
struct Pm4EdramMeter {
  uint64_t frame = 0;              // PM4 frames rendered so far (this record's frame)
  uint32_t resolvesTarget = 0;     // B2: copied straight into the texture that samples it (native)
  uint32_t resolvesGuestMem = 0;   // host RT -> guest-layout (tiled, guest-format) memory, native kernel
  uint32_t resolvesEdram = 0;      // through the EDRAM buffer or the CPU mirror
  uint32_t transfers = 0;          // pixel moves between surface keys sharing EDRAM tiles (aliasing)
  uint32_t transferTiles = 0;
  uint32_t elidedTiles = 0;        // B1a: tiles a transfer would have moved, proven overwritten
  uint32_t edramLoads = 0;         // unowned tiles loaded from the EDRAM mirror
  uint32_t edramLoadTiles = 0;
  uint32_t resolvedTexLoads = 0;   // textures decoded back out of resolve output (guest layout)
  uint32_t materializations = 0;   // B2 targets written back to guest layout for another reader
  uint32_t surfaceOps = 0;         // EDRAM zero step 3 (SR_NB_SURF_TABLE): the same moves, decided by the surface table
  uint32_t surfaceOpTiles = 0;
  // C1 stage 3 (release/emu-stage3): moves EXECUTED through the EDRAM word model - the buffer's store + load or the
  // CPU mirror (the oracle unit), whatever decided them; a move the direct passes took is not one. Feeds edram_xfer.
  uint32_t bufferMoves = 0;
  uint32_t bufferMoveTiles = 0;
};
bool pm4EdramMeterLast(Pm4EdramMeter* out);  // the last completed frame; false before the first
bool pm4TexDirectFormat(DXGI_FORMAT dxgi, DXGI_FORMAT* typeless, DXGI_FORMAT* uav, uint32_t* log2Bpb,
                        uint32_t* unpack,
                        bool any16 = false);

// Thread-safe draw command tracking.
struct DrawStats {
  std::atomic<uint32_t> drawSubmits{0};
  std::atomic<uint32_t> stateUpdates{0};
  std::atomic<uint32_t> presents{0};
  std::atomic<uint32_t> beginBatch{0};

  void resetFrame() {
    drawSubmits.store(0, std::memory_order_relaxed);
    stateUpdates.store(0, std::memory_order_relaxed);
  }
};

DrawStats& getDrawStats();

// Splash overlay state. Splash hook (kernel/splash_hooks.cpp) calls
// setSplashOverlayColor() when sub_822B7F38 fires for a recognised logo;
// the D3D12 default-path clear uses this color instead of dark blue, so
// the splash sequence is visible on screen even though the guest's logo
// decoders aren't present in this build.
//
// Thread-safe: writer = main guest thread (in sub_822B7F38 hook),
// reader = render thread (presentFramebuffer). Single 32-bit atomic.
void setSplashOverlayColor(uint32_t rgbaPacked);
uint32_t getSplashOverlayColor();
void clearSplashOverlayColor();

// Host present-image override (iter-273). When set, the present path blits this
// RGBA8888 image (1280x720, row-major) to the screen instead of the guest
// framebuffer / placeholder — used to display the host-rendered REAL press-start
// screen (decoded from ScrPressStart.AST) since the guest Apt content-load path
// is not yet wired. Set by kernel/splash_hooks.cpp after the intro sequence.
// Thread-safe: copies into an internal buffer under a lock; present reads it.
void setHostPresentImage(const uint8_t* rgba, int width, int height);
void clearHostPresentImage();
bool getHostPresentImage(std::vector<uint8_t>& out, int& width, int& height);

// P1 (docs/RENDERER_PM4_HYBRID.md): SR_PM4_RTCACHE=1 renders the PM4 draw
// path through the EDRAM model (gfx/edram.hpp) instead of the two fixed RTs.
bool pm4RtCacheEnabled();
bool pm4LiveEnabled();
// iter-460: EDRAM transfers on the GPU (SR_PM4_EDRAM_GPU, default ON under the
// RT cache; =0 keeps the CPU-mirror path as the A/B reference).
bool pm4GpuEdramEnabled();
// P1: the PM4 path draws with each VS's in-shader vertex-fetch variant when it
// has one (SR_PM4_VSF, default ON; =0 keeps the CPU pre-decode for A/B).
bool pm4VsfEnabled();

#ifdef _WIN32

class D3D12Backend {
public:
  bool init(void* hwnd, int width, int height);
  void shutdown();

  // tiled: data points at a Xenos 32x32-tiled 32bpp surface (pitch = storage
  // pitch in bytes, multiple of 32 texels); it is untiled into a scratch
  // buffer before upload (gfx/xenos_tiling.hpp).
  void presentFramebuffer(const uint8_t* data, int fbWidth, int fbHeight, int fbPitchBytes,
                          bool tiled = false);
  void presentPlaceholder(uint64_t frameCount);

  bool isInitialized() const { return m_initialized; }

  // ── iter-442 S4.4/S4.6: the API-HLE movie path ────────────────────────────
  // Called ONLY from the hle_video render thread, which exclusively owns this
  // device. Deliberately separate from getOrCreateTexture2D: that one reads
  // GUEST memory through a fetch constant, hashes 1.38 MB per bind to detect
  // CPU writes, and shares m_texEntries with the resolve path (which would
  // evict our planes). Here the bytes are already host-owned and already known
  // fresh — they were snapshotted at the guest's own Unlock.
  // Returns an SRV heap slot, or 0 (the 1x1 white fallback) on failure.
  uint32_t hleUploadHostTexture(uint32_t key, const uint8_t* bytes,
                                uint32_t pitch, uint32_t width, uint32_t height);

  // ── iter-444 S6g: the GENERAL texture upload ──────────────────────────────
  // Same contract (HOST bytes only — the render thread never reads guest
  // memory) but for arbitrary Xenos formats: does the untile, the endian swap
  // and the depth unpack that the movie planes never needed, reusing the
  // algorithms proven in getOrCreateTexture2D rather than re-deriving them.
  // `srcBytes` is the size of the snapshot and is ENFORCED — every source read
  // is bounds-checked against it, which getOrCreateTexture2D notably does not
  // do (its tiled read span exceeds the span it validates).
  // Returns an SRV heap slot, or 0 on failure (the 1x1 white fallback).
  uint32_t hleUploadHostTextureFmt(uint32_t key, const uint8_t* src,
                                   uint32_t srcBytes, uint32_t fmt,
                                   uint32_t width, uint32_t height,
                                   uint32_t pitchTexels, bool tiled,
                                   uint32_t endian);
  // iter-449x: the VOLUME (Texture3D) upload — the LUT class. Returns the
  // space1 RANGE index (1..7; heap slot kTex2DSlots+idx), or 0 = white 3D
  // dummy. k_8_8_8_8 only (census: every live volume is 64^3 fmt 6).
  uint32_t hleUploadHostVolume(uint32_t key, const uint8_t* src,
                               uint32_t srcBytes, uint32_t fmt, uint32_t width,
                               uint32_t height, uint32_t depth,
                               uint32_t pitchTexels, bool tiled,
                               uint32_t endian);
  // iter-450UE FIX B: the SRV heap slot for `key` viewed through the host
  // component mapping composed from the guest fetch swizzle. Returns 0 for
  // "keep the view you already had" (identity mapping, resolve product,
  // unlisted format, mode gating, or heap exhaustion) — never a white dummy.
  uint32_t hleEnsureTexView(uint32_t key, uint16_t guestSwiz, int mode);

  // The guest's own movie quad, fully resolved producer-side. Latched here and
  // drawn inside presentFramebuffer, because the frame command list rests
  // CLOSED between presents — a standalone draw method would silently record
  // nothing.
  //
  // ⚠ iter-444 S6e: this now carries the guest's RAW vertex record plus its own
  // vertex DECLARATION, and the expansion to the translated VS's float4-per-
  // attribute form happens inside hleSubmitDraw. The expansion cannot live on
  // the caller's side: it needs the VS vfetch layout (ShaderCache) and
  // xeDecodeAttr, both private to d3d12_backend.cpp, and it must happen at
  // SUBMIT time because the source bytes live in an arena slot the executor
  // releases as soon as the command is handled.
  struct HleDraw {
    uint64_t vsHash, psHash;
    uint32_t primType;         // guest primitive type (13 = quadlist)
    uint32_t srcCount;         // guest vertex count (4 for the movie quad)
    uint32_t srcStride;        // guest vertex stride in bytes (36 for the quad)
    const uint8_t* srcVerts;   // HOST bytes — valid ONLY for this call
    uint32_t srcBytes;
    const uint8_t* declRaw;    // HOST bytes, 12 per element (S6c), or null
    uint32_t declCount;
    // Host-endian ALU float constants, or null to keep the previous set. The
    // shared CB's own fields (texture indices, sampler config, c18) are filled
    // in by the backend — these are only the c0..cN files the shaders read.
    const uint8_t* vsConst;    // 4096 bytes (256 float4) or null
    const uint8_t* psConst;    // 4096 bytes (256 float4, iter-449v) or null
    uint32_t boolBits;         // iter-449v: (vsBool0&0xFF)|((psBool0&0xFF)<<16) -> g_Booleans
    // iter-449u: WAS [3] — the guest binds fetch slots up to 15 (terrain PS
    // read 10: splat layers + lightRamp + depth), and every truncated slot
    // sampled the 1x1 WHITE fallback. All 16 travel with the draw now.
    uint32_t texSlot[16];
    // iter-450UA [DRAWID] diagnostic: the guest texture KEY bound to each
    // sampler (physical-normalised, i.e. & 0x1FFFFFFF), carried beside the
    // resolved descriptor index in texSlot[] so a single record can state
    // "slot s: key K -> descriptor D". Observation only — nothing outside the
    // [DRAWID] log reads it, and it never influences a bind.
    // ⚠ iter-450UE: NO LONGER OBSERVATION-ONLY. FIX B reads texKey[] to look up
    // the resource a swizzled view is built over, and to prove the fetch entry
    // it decoded describes the texture actually bound to that sampler.
    uint32_t texKey[16] = {};
    // iter-450UE FIX B: the guest texture-fetch swizzle (12 bits) for each
    // sampler, decoded from the draw-associated fetch file, and that entry's
    // own base. 0xFFFF = no data (pre-450T trace, non-texture entry, or a
    // capture miss) -> the backend keeps D3D12's default component mapping.
    // ⛔ Initialised to the SENTINEL, not to 0: an unfilled producer must
    // degrade to "no data", never to a silent (R,R,R,R) mapping.
    uint16_t texSwiz[16] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu,
                            0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu,
                            0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu,
                            0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
    uint32_t texSwizBase[16] = {};
    // iter-449x: per-slot Texture3D RANGE index (space1 table, 0 = white 3D).
    // A slot's key lives in exactly one map, so 2D and 3D indices coexist:
    // the shader's tfetch dimension picks which range it reads. Default-zero
    // so every producer that doesn't fill it keeps today's white-dummy read.
    uint8_t texSlot3D[16] = {};
    // ── iter-457 H1(b): per-slot TextureCube RANGE index (space2 table) ────
    // 0 = the white 6-face dummy, which is what every cube-fetching draw has
    // been reading since the renderer was built, because nothing ever wrote
    // this band. Default-zero keeps exactly that for any producer that does
    // not fill it.
    uint8_t texSlotCube[16] = {};
    // ── S6h: the guest's OWN render state, decoded from its register shadow ──
    // Until now these were hardcoded (depth off, opaque blend, all channels,
    // c18 identity, full-RT viewport), which is right for a clip-space movie
    // quad and wrong for everything else — measured: general geometry rendered
    // as a shard pile because nothing depth-tested and every VTE-off draw was
    // mapped as if it emitted clip space.
    // iter-449c: WHICH PRODUCER fed this draw (0=DrawVertices, 1=Indexed,
    // 2=UP/inline). Diagnostic attribution only — the menu UI draws exclusively
    // through the UP path, so "does UI land on the pool primary" is only
    // answerable if the landing counters can tell the types apart.
    uint32_t srcType;
    // iter-450O: this list was converted from a restart strip by the gather
    // (carried as bit 8 of the trace's primType; masked off in buildDraw).
    // Powers the SR_S6_STRIPAB replay A/B for the fracture census.
    bool     fromStrip = false;
    // iter-450T: per-slot sampler-table index ((clampU?4)|(clampV?2)|
    // (linear?1)) decoded from the guest's texture-fetch shadow, or 0xFF =
    // no data (old traces / capture miss) -> the fixed clamp+linear stands.
    // Consumed only under SR_HLE_SAMPSTATE>=1.
    uint8_t  sampSel[16];
    // iter-450Q: the vertex bytes were SYNTHESIZED BY THE HOST (movie quad:
    // pos@0 float3 + uv@12 float2, stride 20 — our layout by construction).
    // The vfetch sidecar describes the GUEST's fetch offsets for that VS, so
    // offset pairing must not apply — the OFFPAIR bake paired the movie
    // quad's position from its UV bytes and every movie frame rendered as a
    // quarter-size 180-degree-rotated top-right-quadrant image (regression
    // window proven by the iter450k_upswap vs iter450M_offpair archives).
    bool     hostVerts = false;
    bool     stateValid;
    uint32_t depthControl;   // RB_DEPTHCONTROL   0x2200
    uint32_t blendControl;   // RB_BLENDCONTROL0  0x2201
    uint32_t colorMask;      // RB_COLOR_MASK     0x2104
    uint32_t vteCntl;        // PA_CL_VTE_CNTL    0x2206
    float    vpScale[3];     // PA_CL_VPORT_{X,Y,Z}SCALE  0x210F/0x2111/0x2113
    float    vpOffset[3];    // PA_CL_VPORT_{X,Y,Z}OFFSET 0x2110/0x2112/0x2114
  };
  // Returns false when the draw could not be staged (no VS layout, decl too
  // short for the VS's input count, unsupported shape, ring full) — the caller
  // counts it as a coverage miss rather than claiming a draw it cannot record.
  // ⚠ Appends to the PER-FRAME DRAW LIST; nothing is recorded until Present.
  bool hleSubmitDraw(const HleDraw& d);
  // iter-448: blit the texture under `key` (the guest front buffer's pixel
  // memory, from its embedded fetch constant) onto the open frame's backbuffer.
  // Exact-match CopyResource only — dims/format mismatches REFUSE by name, and
  // the refusal is counted in [S6_FRONT]'s shown column staying behind HIT.
  bool hlePresentFront(uint32_t key, uint32_t dims);
  // Why a submit was refused, for the coverage instrument's skip breakdown.
  enum class HleReject : uint32_t {
    None = 0, Shape, NoVs, NoDecl, BadFormat, NoPso, VbFull, CbFull, TooManyDraws,
    NoConst,
    // iter-445 S6k: the CONSUMED elements span more than one vertex stream.
    // Measured: general geometry declares 7 elements, six in stream 0 and one
    // in stream 1 — the producer snapshots element 0's stream only, so a draw
    // whose VS actually reads the second stream cannot be expanded yet. Refused
    // and named rather than fetched from the wrong buffer (which is exactly the
    // iter-443 heuristic failure S6j diagnosed).
    MultiStream
  };
  HleReject hleLastReject() const { return m_hleLastReject; }
  static const char* hleRejectName(HleReject r);
  // THE HONEST TRIPLE. `recorded` is produced by the code that actually issues
  // DrawInstanced, so the coverage instrument can never again report a staged
  // draw as a rendered one. `dropped` = staged but retired without recording
  // (a present that took an early return or the upload branch); it must stay 0
  // and names the bug if it does not. `held` = re-issues of the guest's own
  // last list for the 2:1 movie hazard, reported separately and NEVER folded
  // into `recorded`.
  uint64_t hleDrawsRecorded() const { return m_hleDrawn; }
  // iter-445 C4: called from the head of hle::execute() — lazy and idempotent,
  // so whichever of execute()/presentFramebuffer reaches it first opens the
  // frame's command list. No-op unless SR_HLE_FRAME_LIST=1.
  void ensureFrameOpen();
  // Counts a command observed while the window was open. The ONLY positive
  // evidence that the window actually widened: it reads exactly 0 with the gate
  // off (the list is opened from inside execute()'s own Present arm, so no
  // execute() call ever observes Recording at its head) and the trace's real
  // commands-per-frame with it on.
  void noteWindowCommand() { if (m_clState == ClState::Recording) m_windowCmds++; }
  // iter-445 C1: the frame-list ledger's report line. Public because the
  // executor owns the reporting cadence (it is the only place that knows the
  // present index, and shutdown() has NO CALL SITES anywhere in the repo, so a
  // teardown-time report would never print).
  void frameListReport(uint64_t presents);
  uint64_t hleDrawsHeld() const { return m_hleHeld; }
  uint64_t hleDrawsDropped() const { return m_hleDropped; }
  uint64_t hleQuadsDrawn() const { return m_hleDrawn; }
  uint64_t hleQuadsHeld() const { return m_hleHeld; }

  // ── iter-446 R1/R2: THE GUEST SURFACE POOL ─────────────────────────────────
  // ONE host resource per guest surface VA, created LAZILY on first bind from
  // the header dwords the producer publishes (ENGINE_MAP §6g.9). Measured: 16
  // distinct render targets and 3 depth surfaces per run, forming a downsample
  // pyramid, with ~13 resolves/frame between them.
  //
  // ⚠ WHY ITS OWN DESCRIPTOR HEAPS, rather than growing the existing ones: the
  // shared m_rtvHeap is exactly kFrameCount+2 and its slots are OWNED — two by
  // the swapchain, one each by m_geoRT/m_sceneRT — and m_dsvHeap is a single
  // descriptor created CONDITIONALLY. Resizing either means renumbering live
  // slots in code paths the movie currently depends on. RTV/DSV descriptors are
  // CPU-only and cost nothing, so a dedicated heap buys total isolation for
  // free: nothing in the pool can perturb a path that works today.
  struct HleSurface {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    uint32_t width = 0, height = 0;
    uint32_t xenosFormat = 0;    // RB_COLOR_INFO bits 16-19 (colour OR depth)
    uint32_t samples = 1;        // the CONSOLE's sample count — see below
    uint32_t descIndex = 0;      // slot in m_hleRtvHeap / m_hleDsvHeap
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;   // RESOURCE format (typeless for depth)
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;  // typed depth view
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool isDepth = false;
    // iter-446 audit finding 3: has REAL GEOMETRY ever been rendered into
    // this surface? False for every surface today.
    bool everDrawn = false;
  };
  // Look up (creating on first sight) the host resource for a guest surface.
  // Returns nullptr when the header did not decode or the pool is exhausted —
  // NEVER a plausible-but-wrong surface. The caller counts the refusal.
  HleSurface* hleSurfaceGet(uint32_t surfVA, uint32_t surfInfo,
                            uint32_t colorInfo, uint32_t byteSize,
                            uint32_t sizeWord, bool isDepth);
  // D3D9 render-target binding is STATE: these only update the mirror, and the
  // binding is realised at draw/clear time. `s` may be null (MRT teardown —
  // measured 65K times per run for slots 1/2/3).
  void hleBindRenderTarget(uint32_t index, HleSurface* s);
  void hleBindDepthStencil(HleSurface* s);
  // LOOKUP ONLY — never creates, never touches the refusal counters. Resolve
  // needs to know whether a surface is already pooled, and asking through
  // hleSurfaceGet would charge every miss to badHeader and corrupt the pool's
  // own honesty metric.
  HleSurface* hleSurfaceFind(uint32_t surfVA);
  // iter-446 R5b: execute one resolve — RT -> the texture the guest samples.
  // Creates the destination if needed (it can arrive NO other way: a render
  // product's pixels come from the GPU, so it never passes through Lock/Unlock
  // or the bind-capture path) and records the copy into the OPEN frame list.
  // Returns the SRV slot, or 0 if refused; `why` names the refusal.
  enum class ResolveSkip : uint32_t {
    None = 0, Disabled, NoList, NoSource, DepthSource, SubRect, Sliced,
    CreateFailed, NoSlot,
    // The dest texture's declared extent was unreadable or absurd.
    BadDestSize,
    // The frame-list interlock: without it we would record into a list the MAIN
    // thread owns (SR_HLE_OWN_PRESENT=0). Named rather than silent so the
    // refusal is visible in the report.
    NoFrameList
  };
  // `destW`/`destH` are the DESTINATION's own declared extent (Xenos size_2d
  // from its fetch constant), NOT the source's. The source surface's byte size
  // is EDRAM-tile-padded, so deriving the destination from it makes every
  // 360-tall target 368 tall — see ENGINE_MAP §6g.12. Pass 0/0 when the
  // producer could not read the dest header: the call is then REFUSED rather
  // than falling back to the padded size.
  // iter-457: resolve into ONE SLICE of a STACKED destination, registered in
  // the 3D descriptor range. The 2D sibling below cannot serve these: its
  // destination is a Texture2D in the 2D range, and every consumer of a
  // stacked product reads the 3D range instead (measured: 672 admitted
  // draws, zero 2D readers), so it samples the white 3D dummy forever.
  // `depth` MUST come from the guest's own fetch entry; 0 means unknown and
  // the caller must refuse rather than pick one.
  // ── iter-457 H1(b): upload a guest CUBE, all six faces, fail-closed ──────
  // Returns the cube descriptor index (1..7) or 0. Zero means REFUSED, and a
  // refusal degrades to the white dummy at index 0 - today's picture - never
  // to a partially-written cube, which renders wrongly in one direction only
  // and reads as a bug in the scene rather than in the upload.
  // `why` always receives the named reason.
  // r5: staging, submission, publication and every lifetime decision live
  // in gfx/cube_transaction.hpp (m_cubeTx below). This function decodes the
  // guest bytes and hands the transaction ONE request; it never touches an
  // allocator, a fence or a descriptor itself.
#if SR_ORACLE
  uint32_t hleUploadCube(uint32_t key, const uint8_t* base, uint64_t baseBytes,
                         const uint8_t* mipSrc, uint64_t mipBytes,
                         uint32_t fmt, uint32_t width, uint32_t height,
                         uint32_t pitchTexels, bool tiled, uint32_t endian,
                         uint32_t mipLevels, bool packedMips,
                         sr::gfx::cubebind::CubeRefuse& why);
  // The cube descriptor index for a key, or 0 when we hold no cube for it.
  uint32_t hleCubeSlotForKey(uint32_t key) const;
  // Uncapped cube summary [S6_CUBE_SUM]. `site` names the emission path:
  // the executor calls it as "periodic" from the live report and exactly
  // once as "replay-final" at the end of a replay. hleSurfaceReport does
  // NOT emit it - r4 emitted it from both, which duplicated the replay line.
  void hleCubeSumReport(const char* site);
#endif

  uint32_t hleResolveToVolume(HleSurface* src, uint32_t destKey,
                              uint32_t destW, uint32_t destH,
                              uint32_t depth, uint32_t slice,
                              ResolveSkip& why);
  uint32_t hleResolveToTexture(HleSurface* src, uint32_t destKey,
                               uint32_t destW, uint32_t destH,
                               ResolveSkip& why);
  // iter-446 R6: execute the guest's OWN clear against the surfaces it has
  // bound. ⚠ Call this from the ClearF arm and ONLY there — D3DDeviceClear is a
  // WRAPPER that tail-calls ClearF once per rect, and our hooks delegate, so
  // arming both would double-clear every wrapper call (ENGINE_MAP §6g.8).
  // `flags` is the 360 layout: bits 0-3 = TARGET0..3, 0x10 = ZBUFFER,
  // 0x20 = STENCIL — NOT the PC D3D9 values.
  void hleClearBound(uint32_t flags, const float rgba[4], float z,
                     uint8_t stencil);
  // Marks a pooled surface as having had real geometry rendered into it.
  // ⚠ NO CALL SITE YET — the draw path still renders into the legacy geo
  // target. Declared here so the gap is measurable rather than assumed.
  void hleNotePoolDraw(HleSurface* s) { if (s) s->everDrawn = true; }
  void hleSurfaceReport(uint64_t presents);
  // iter-449s AUTOPSY: read back every live pool COLOR surface and write BMPs
  // into `dir` (replay-fixture diagnostic; the presented backbuffer only shows
  // the front-blit result, so surface truth needs its own oracle).
  void hleDumpSurfaces(const char* dir);

  ID3D12Device* getDevice() const { return m_device.Get(); }
  ID3D12CommandQueue* getCommandQueue() const { return m_commandQueue.Get(); }

  // P1 (SR_PM4_RTCACHE): translate every queued draw NOW, appending to the
  // frame's batch set. The command processor calls this after each executed
  // packet so a draw's vertex/index bytes are read at draw time (Xenia reads
  // shared memory at draw time); renderPm4Batches closes the frame's set.
  // P1: guest memory [phys, phys+bytes) changed behind the texture cache (a
  // trace memory restore, a resolve, a CPU write): textures reading it must be
  // re-validated at their next bind. Everything else binds without hashing.
  void invalidateGuestRange(uint32_t phys, uint64_t bytes);
  // Live, until a page write-watch exists: everything re-validates once.
  void invalidateAllTextures() {
    m_texClean.clear();
    m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO
  }
  // P2 (SR_PM4_LIVE): the guest's swap. Renders the open frame set (its
  // resolves write the guest front buffer), then scans the front buffer out:
  // untile, the game's display gamma ramp, the backbuffer, Present.
  void pm4PresentFrame(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t pitchTexels,
                       bool tiled);
  // Step 4 item 7a (SR_PM4_EARLY_INIT=1, default OFF; gfx/d3d12_frame_render.cpp): the frame renderer's one-time GPU
  // setup (pm4Init, pm4GpuInit, the direct-transfer root signature + shaders, pm4ShmInit, the oracle unit's EDRAM
  // passes), called by the CP before its first command instead of inside the guest's first swap. Same thread as the
  // synchronous first frame (the CP); logs [PM4_EARLY_INIT]. true = every part that is enabled came up.
  bool pm4EarlyInit();
  // Step 4 item 2k (SR_PSO_CENSUS / SR_PSO_RECORD / SR_PSO_WARM, default OFF; gfx/pso_warm.cpp): the native
  // pipeline cache's census, manifest recorder and boot warm-up, attached once by the CP (cpEarlyInit).
  void psoAttach();
#if SR_ORACLE
  // Step 4 G7 option (c) (SR_ALLOC_CENSUS, kernel/perf_stats.hpp): this backend's cache sizes and video memory.
  void allocCensusReport(void (*emit)(const char* group, const char* name, double value)) const;
#endif
  // Step 4 (Astra 10:5x #1): the exit's render-thread quiescence - true once no frame is queued for the PM4 render
  // thread and none is being rendered (or the thread stopped), within `ms`.
  bool pm4RenderThreadIdle(uint32_t ms);
  // p2.zpd strict covering fence (the drain owner: the CP, or the SR_NATIVE_EXEC executor; everything ahead is
  // drained): the open set rendered as a partial frame and the GPU drained, so the occlusion queries it ends are
  // written to guest memory before the caller performs its fence store. The partial CONSUMES the set (a swap with no
  // draw after the fence renders nothing of it again). false = it could not (not ready / stop / the render thread
  // did not finish within m_zpdFlushTimeoutMs): the caller then writes the strict fallback (null_renderer
  // cpZpdFence).
  bool pm4ZpdFlushPartial();
#if defined(SR_DIFFTEST)
  // ── p2.zpd difftest seam (tools/difftest/native_zpd_test.cpp; the cloud worker has no GPU) ──
  // `on` takes the device checks out of pm4ZpdFlushPartial / pm4PresentFrame and replaces pm4RenderFrame's GPU body
  // with zpdTestFrame: the same zpdMarker / zpdDrawBegin / zpdDrawEnd / zpdCollectNow calls the render loop makes,
  // each draw's host count from `samplesPerDraw`, `delayMs` of GPU time before the frame's queries complete, the
  // render loop's end-of-set bookkeeping. Every frame and every query command is recorded.
  struct ZpdTestFrame {
    bool partial = false;
    uint32_t batches = 0, markers = 0, draws = 0, helpers = 0;
    zpd::FrameStamp stamp{};
    uint32_t threadId = 0;
  };
  struct ZpdTestSeam {
    bool on = false;
    uint32_t delayMs = 0;
    uint64_t samplesPerDraw = 0;
    uint64_t refSamples = 0, rasterSamples = 0;   // the probe draws' host counts
    std::vector<uint64_t> readback;               // the resolved counts (the readback buffer's stand-in)
    std::mutex mu;                                // frames / rec (the render thread records too)
    std::vector<ZpdTestFrame> frames;
    // 'B' BeginQuery, 'E' EndQuery + Resolve, 'H' a helper pass, 'r' / 'R' a ref / raster probe draw
    std::vector<std::pair<char, uint32_t>> rec;
    void (*onFrame)(void* user, bool partial) = nullptr;  // after the frame's queries were collected
    void* user = nullptr;
  };
  ZpdTestSeam& zpdTest() { return m_zpdTest; }
  void zpdTestEnable();
  void zpdTestPushMarker(uint8_t op, uint32_t id) {  // the drain's marker branch (and the drain's open set)
    zpdDrainMarker(op, id);
    m_pm4DrainOpen = true;
  }
  // A guest draw (RB_SURFACE_INFO MSAA) or a helper batch; probe = it carries the probe pipelines (stand-ins: the
  // seam records 'r' / 'R' instead of drawing).
  void zpdTestPushDraw(uint32_t msaa, bool helper, bool probe = false);
  size_t zpdTestSetSize() const { return m_drawBatches.size(); }
  bool zpdTestDrainOpen() const { return m_pm4DrainOpen; }
  void zpdTestRenderThread(bool on);
  void zpdTestFlushTimeout(uint32_t ms) { m_zpdFlushTimeoutMs = ms; }
#endif
  void pm4DrainNow() {
    if (!m_geoPipelineReady) return;
    const auto t0 = std::chrono::steady_clock::now();
    drainDrawQueue();
    m_pm4DrainNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - t0).count());
  }
  // Trace replay: Xenia's EDRAM snapshot (edram::kEdramBytes, tile-major guest
  // layout, d3d12_render_target_cache.cc RestoreEdramSnapshot) becomes the
  // mirror's content; ownership resets so tiles load from it when claimed.
  void pm4RestoreEdram(const uint8_t* snapshot, size_t bytes);
  // iter-460 live trace (pm4_live_trace.hpp): the EDRAM and the GPU-written
  // shared-memory pages as the trace's initial state (Xenia InitializeTrace).
  void pm4TraceInit(livetrace::InitState& out);
#if SR_ORACLE
  void pm4ShmTraceGpuPages(std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& out);
  // iter-463 (SR_PM4_TRACE_FULL, default ON; only while a trace records):
  // record the guest memory a draw's registers reference - its index buffer,
  // every vertex fetch constant and the full base + mip backing of every
  // texture fetch constant (Xenia GetGuestTextureLayout) - whether or not the
  // renderer goes on to read it (a draw rejected for a missing shader reads
  // nothing). Reads memory only: what is rendered does not change.
  void pm4TraceDrawBacking(const GuestDrawCommand& c);
  void pm4TraceFetchBacking(const uint32_t* fetch6);
  void pm4TraceRange(uint32_t phys, uint32_t len);
  // One 4 KB page, from the window pm4ShmValidate serves it from: rangeOwn is
  // the request range's owner (commandBufferWindow, then gpuWindowRange; 0 =
  // the page decides, as validation does).
  void shmTracePage(uint32_t page, int rangeOwn);
  // A resolve the traced frame's drain queued: its destination pages are the
  // player's own output from then on, so pm4TraceRange no longer records the
  // (stale) CPU copy over them. Cleared when a trace opens (pm4TraceInit).
  void pm4TraceNoteResolve(const edram::Regs& regs);
  std::unordered_set<uint32_t> m_traceResolvedPages;
#endif
  // iter-463 replay (SR_REPLAY_MIPS_SUPPLIED, default ON): the pages the trace
  // being replayed supplied (MemoryRead records, applied in stream order). An
  // array whose mip region the trace never supplied loads its base level only:
  // traces recorded before iter-463 D8 carry no cube / stacked mip bytes, and
  // the zero pages there would blend every LOD > 0 lookup toward black.
  void replayNoteSupplied(uint32_t phys, uint32_t len);
  bool replaySupplied(uint32_t phys, uint32_t len) const;  // true outside a replay
  std::vector<uint8_t> m_replaySupplied;  // per 4 KB page; empty = not a replay
#if SR_ORACLE
  // release/emu-stage3 (Astra round 1 P1): SR_NAT_DIFFTEST=1 - synthetic resolves of the shapes the native memory
  // path must take (origins past the first tile row / column, depth at 1x, colour without a cached target, MSAA,
  // 64bpp), each compared against the EDRAM word model's bytes ([NAT_DIFFTEST]; gfx/d3d12_edram_model.cpp). The
  // trace replay runs it after its frames (gfx/gpu_trace_replay.cpp). A no-op without the gate.
  void natResolveDifftest();
  // Replay-only census closure: preserve the resolved frame's metadata before
  // diagnostic readback reuses timing scratch, then drain after real completion.
  void nbReplayBegin();
  bool nbReplayFinish();
#endif
  // step 6 I3-B: the [I3_BINDLESS] census line (the cadence's, and once at shutdown / a replay's end: `why`).
  void i3bCensusLine(const char* why);

  ~D3D12Backend() { shutdown(); }

private:
#if SR_ORACLE
  friend struct ResourceRoutingTestAccess;
  friend struct NativeCensusTestAccess;
  friend struct XlatCbZeroCopyTestAccess;   // step 6 I10p: tools/difftest/xlat_cb_zerocopy_test.cpp (Astra r2)
  friend struct NativeBindlessTestAccess;  // step 6 I3-B: tools/difftest/native_bindless_test.cpp
#endif
  friend struct NativeDrawAdmissionTestAccess;
  // step 6 I3-B: the SRV heap layout as the fixture sees it (d3d12_backend.cpp asserts they match its constants)
  static constexpr uint32_t kXlat2DSlots = 8192, kXlatRawRingSlots = 65536;
  static constexpr int kFrameCount = 2;
#if SR_ORACLE
  // iter-358k: real menu frames carry hundreds of draws with thousands of
  // vertices (golden trace: 3,501 draws). Vertex = pos float3 + color float4.
  static constexpr uint32_t kMaxGuestVerts = 262144;
  static constexpr uint32_t kGeoVertexFloats = 7;
  static constexpr uint32_t kGeoVertexStride = kGeoVertexFloats * 4;

#endif  // SR_ORACLE
  void waitForGpu();
  void moveToNextFrame();
  // iter-464 PORT FEATURE (SR_HIGHFPS, kernel/highfps.hpp): every swap-chain Present goes
  // through here - sync interval 1 (the old fixed call) unless the high-frame-rate
  // config asks for 0, with ALLOW_TEARING when the swap chain was created with it.
  HRESULT presentSwap();
  bool m_allowTearing = false;
  uint32_t m_swapChainFlags = 0;  // Creation flags survive a present-mode fallback.
  bool createGuestGeoPipeline();
#if SR_ORACLE
  void renderGuestGeometry();
#endif
  void drainDrawQueue();

  bool m_initialized = false;
  bool m_geoPipelineReady = false;
  int m_width = 0;
  int m_height = 0;

  Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
  UINT m_rtvDescriptorSize = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_renderTargets[kFrameCount];

  std::unique_ptr<NativeDrawRecording> m_nativeRecording;
  uint64_t m_nativeSubmission = 0;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
  size_t m_uploadBufferSize = 0;

  // Readback buffer for RESOLVE: copy render target to guest framebuffer (Phase 4).
  Microsoft::WRL::ComPtr<ID3D12Resource> m_readbackBuffer;
  UINT m_readbackRowPitch = 0;
#if SR_ORACLE
  // destInfo = RB_COPY_DEST_INFO: selects dest texel format (6=8888 4B,
  // 24=k_16 2B, 36=k_32_FLOAT 4B), red/blue swap, and endian permutation.
  // destInfo 0 = legacy 8888 no-swap no-endian (bytes R,G,B,A).
  void copyRenderTargetToGuestFramebuffer(uint8_t* hostDest, int destPitchBytes,
                                          int maxRows = 720, uint32_t destInfo = 0);

  // iter-437c: DEFERRED resolve execution. The iter-437 HLE Resolve hook
  // surfaces the game's real traffic (~100 resolves/drain incl. the 440-cycle
  // helper pass) and a per-resolve submit+wait+convert ran the drain at
  // 149-274ms/present ([DRAIN_PROF]). Instead: the GPU copy is recorded into
  // the OPEN frame list at the resolve's stream position (RT content captured
  // exactly as the console would), while the fence wait + CPU convert are
  // deferred to the first point that needs the guest bytes — an overlapping
  // texture bind, end of drain, or readback-buffer exhaustion. A pending
  // same-dest resolve with no intervening overlapping bind is DEAD (its
  // output would be overwritten unread) and skips its convert — the helper
  // pass collapses to one convert. Guest-visible memory semantics identical.
  // The replay fixture path (SR_XLAT_RTT=1) keeps the immediate path so RTT
  // snapshots capture the RT at the resolve's stream position.
  struct PendingResolve {
    uint32_t destPhys = 0, pitchTexels = 0, rows = 0, destInfo = 0;
    uint64_t rbOffset = 0;
    UINT rbRowPitch = 0;
    int copyW = 0, copyH = 0;
    // iter-439 P1: placement of this (possibly sub-rect) resolve inside the
    // dest surface — guest write-back and texsnap both land here.
    int dstX = 0, dstY = 0;
    bool dead = false;
  };
  std::vector<PendingResolve> m_pendingResolves;
  uint64_t m_resolveRbCursor = 0;
  // iter-438c H1 probe: ring of recently CONVERTED resolve dests (guest memory
  // written) so a later texture bind landing on one can be logged with the
  // format/path/content it was actually served.
  struct RecentResolveDest { uint32_t phys = 0, bytes = 0, destInfo = 0; bool dead = false; };
  RecentResolveDest m_recentResolveDests[16];
  uint32_t m_recentResolveCursor = 0;
#endif  // SR_ORACLE
  // iter-437e: boundaries consumed by frame-coherent drains (drainDrawQueue).
  uint64_t m_framesDrained = 0;
  // iter-439d MID-FRAME CONTINUATION: a guest frame with >kCapacity/2 draws
  // (the render-script eras) trips the half-full valve and gets split across
  // drains. The unconditional clear at renderGuestGeometry's top then ERASED
  // the frame's first half before the second half drew — measured as the
  // x~648 half-screen seams, the half-black titles, and the mid-movie
  // solid-color flashes (fb dump vs window, roll D: zero tiled resolves — the
  // seams were never resolve placement). A continuation drain now keeps the
  // RT (console EDRAM persists across the whole frame) and its batch set
  // renders exactly once (re-render without clear would double-blend).
  bool m_thisDrainContinuesFrame = false;   // this drain's set continues a frame
  bool m_nextDrainContinuesFrame = false;   // last drain ended mid-frame
  bool m_contBatchesRendered = false;       // continuation set already drawn
#if SR_ORACLE
  // iter-439f PRESENT-ONLY-COMPLETE-FRAMES: a mid-frame drain leaves half a
  // picture on the RT; copying THAT to the backbuffer is the half-screen-seam
  // / maroon-flash presenter. Completed frames snapshot into m_lastGoodRT and
  // mid-frame presents re-show the snapshot — the console never scans out
  // mid-frame state either.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_lastGoodRT;
  bool m_lastGoodRTValid = false;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_resolveReadback;
  // iter-439 P2: ASYNC resolve readback. The drain-end flush used to
  // Close+Execute+waitForGpu every drain — [DRAIN_PROF] measured that wait at
  // ~40% of drain time, the drain fell behind the game's ~8.5K draws/s during
  // the ident render script, the 8192-slot queue pinned full, and every guest
  // pushBlocking stalled inside a game-held critical section (the boot wedge).
  // Now the drain-end flush signals m_resolveFence and returns; the flushed
  // items park here and are converted (guest write-back) on a LATER drain once
  // the fence has passed — one drain of latency, the console's own
  // resolve-to-scanout distance. Sync paths remain for rb-full and
  // bind-overlap (a bind needs post-resolve guest bytes NOW).
  struct InFlightResolveFlush {
    std::vector<PendingResolve> items;
    UINT64 fenceValue = 0;
    uint64_t rbBegin = 0, rbEnd = 0;  // region of m_resolveReadback still owned
  };
  std::deque<InFlightResolveFlush> m_inFlightResolves;
  Microsoft::WRL::ComPtr<ID3D12Fence> m_resolveFence;
  UINT64 m_resolveFenceNext = 0;
  HANDLE m_resolveFenceEvent = nullptr;
  uint64_t m_resolveRbBase = 0;  // start of the CURRENT recording region
  // Poll (or block on) m_resolveFence and convert every completed flush's
  // items to guest memory; rewinds the readback ring when fully drained.
  // Returns true if anything was converted. Never touches the command list.
  bool completeInFlightResolves(bool waitAll);
  void convertResolveItems(const std::vector<PendingResolve>& items,
                           const uint8_t* mappedBase);
  struct DrawBatch;  // declared below
  // Returns true if a buffer-full flush happened inside — the caller's draw
  // state (RTV/PSO) died with the reset list and MUST be rebound.
  bool recordPendingResolve(const DrawBatch& batch);
  bool flushPendingResolves(const char* why, bool allowAsync = false);
  bool pendingResolveOverlaps(uint64_t lo, uint64_t hi) const;

#endif  // SR_ORACLE
  // iter-358f: the static test-triangle pipeline (createTestPipeline/renderTestPattern
  // + m_rootSignature/m_testPSO/m_testVertexBuffer/m_testVBView/m_pipelineReady) was
  // removed — it faked visible output. See docs/NATURAL_BASELINE.md 2026-07-17.

#if SR_ORACLE
  // Guest geometry pipeline (iter-358k: pos+color, per-draw viewport root
  // constants, offscreen RT).
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_geoRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_geoPSO;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_geoVertexBuffer;  // dynamic upload
  D3D12_VERTEX_BUFFER_VIEW m_geoVBView{};
#endif  // SR_ORACLE
  // Offscreen guest render target (RTV heap slot kFrameCount); real draws land
  // here, the resolve reads it back, and a CopyResource mirrors it on screen.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_geoRT;
  // iter-359m: HDR scene color surface (R16G16B16A16_FLOAT), RTV heap slot
  // kFrameCount+1. Scene passes (RB_COLOR_INFO float formats) render here so
  // the tonemap gets unclamped input; a resolve from it snapshots an HDR
  // texture the composite samples. Gated with SR_XLAT_RTT (the surface model).
  Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneHdrRT;

  // Per-frame draw batch from queue. kind selects the pipeline:
  //   FixedFn — the iter-358k pos+color passthrough (fallback);
  //   Xlat    — XenosRecomp-translated real shaders (iter-359c).
  struct DrawBatch {
    uint32_t vertexCount;
    uint32_t vertexOffset;   // FixedFn: vertex index; Xlat: BYTE offset in m_xlatVB
    uint32_t flags;
    D3D_PRIMITIVE_TOPOLOGY topology;
    uint32_t consts[10];  // b0: vpScale xyz, vpOffset xyz, vteCntl, rtW, rtH, pad
    // ── translated path ──
    bool xlat = false;
    ID3D12PipelineState* xlatPso = nullptr;  // borrowed from m_xlatPsoCache
    // iter-467 native-b B1b (SR_NB_COVER_FILL): the same draw marking its coverage in a scratch stencil (PM4 path;
    // set by pm4PrepareDraw for a draw without depth / stencil whose every colour binding is fully overwritten
    // where it rasterizes - no blending, every channel written), else null.
    ID3D12PipelineState* xlatPsoMark = nullptr;
    uint32_t xlatStride = 0;                 // canonical float4-per-attr stride
    uint64_t xlatVsCbOff = 0, xlatPsCbOff = 0;  // CPU staging offsets, never GPU addresses
    // Float-constant extents the shaders read (c0 to the last used 16-register block;
    // TranslatedConstantSnapshot), from the draw's vsConstUse / psConstUse.
    uint16_t xlatVsBytes = 4096, xlatPsBytes = 4096;
    D3D12_VIEWPORT xlatViewport{};
    // iter-359j: texture binding happens at RENDER time, not drain time —
    // a draw sampling a range an earlier resolve wrote THIS frame must see
    // the post-resolve content (drain-time creation read pre-frame memory:
    // the tonemap chain sampled stale/trace bytes — the attribution bug).
    // First 16 texture fetch groups (96 dwords) + CPU offset of the shared
    // CB block whose dwords [0..15] receive the descriptor indices.
    uint32_t texFetch[96] = {};
    uint64_t sharedCbOff = 0;
    // step 6 I10p (SR_XLAT_CB_ZEROCOPY): a view batch - xlatVsCbOff / xlatPsCbOff / vsfFetchCbOff are offsets into
    // m_xlatCBGpu (the CB third's views), sharedCbOff is the CP's shared block in the compact scratch, and
    // zcSharedOff is the draw's shared slot in m_xlatCBGpu (the render thread writes it on change).
    bool zcView = false;
    uint64_t zcSharedOff = 0;
    uint64_t zcSet = 0;   // the CP set generation that allocated the views (m_zcCpSet): the shared slot's write-once key
    // iter-420: carried purely so bind-time logs can name WHICH shader a
    // fetch-slot dump belongs to. Without it, a capped sample of bind
    // events cannot be attributed to a draw, and iter-417's dump landed on
    // the wrong draws entirely.
    uint64_t vsHash = 0, psHash = 0;
    // ── resolve (iter-359g: RB_MODECONTROL edram_mode==6 — the pass wiring) ──
    bool resolve = false;
    uint32_t resolveDestPhys = 0;     // RB_COPY_DEST_BASE
    uint32_t resourceGpuGeneration = 0;
    uint32_t resolvePitchTexels = 0;  // RB_COPY_DEST_PITCH
    uint32_t resolveRows = 0;
    // iter-439 P1: source rect from the emit (tile sub-rect resolves).
    // All-zero = full surface.
    uint16_t resolveSrcX = 0, resolveSrcY = 0, resolveSrcW = 0, resolveSrcH = 0;
    // RB_COPY_DEST_INFO (iter-359h, Xenia registers.h:870): [2:0] endian128,
    // [12:7] dest format, [24] red/blue swap. The write-back must honor all
    // three — the frame's resolves target k_8_8_8_8, k_16 AND k_32_FLOAT.
    uint32_t resolveDestInfo = 0;
    // iter-359m: which EDRAM color surface this batch targets — RB_COLOR_INFO
    // float formats route to the HDR scene RT; everything else to the LDR RT.
    uint32_t colorInfo = 0;
    bool hdrTarget = false;
    // ── iter-440c API clear (migration Phase 2): the guest's own Clear/ClearF
    // executed at stream position (SR_API_CLEAR).
    bool clearOp = false;
    uint32_t clearFlags = 0;
    float clearColor[4] = {};
    float clearZ = 1.0f;
    uint8_t clearStencil = 0;
    // ── p2.zpd (SR_NATIVE_ZPD, gfx/native_zpd.hpp): an occlusion-query marker at stream position (no draw):
    // 1 = BEGIN, 2 = END of logical query zpdId. renderPm4Batches records BeginQuery / EndQuery here.
    uint8_t zpdOp = 0;
    uint32_t zpdId = 0;
    // p2.zpd probe (zpd::probeOn, pm4PrepareDraw): a guest draw drained inside an open query carries the probe
    // pipelines it is drawn again with right after its measured draw, each on a segment that is not the answer
    // (gfx/native_zpd.hpp segmentOpenProbe): zpdRefPso = depth ALWAYS, stencil off, nothing written, its own pixel
    // shader; zpdRasterPso = the same without a pixel shader. zpdPointSize / zpdColorCtl (PA_SU_POINT_SIZE /
    // RB_COLORCONTROL) and the first vertex as the guest wrote it (zpdVtxAddr: the VS's first vertex stream,
    // physical; zpdVtx: its first 16 bytes as big-endian words, read at drain time) feed the [NATIVE_ZPD] draw lines.
    ID3D12PipelineState* zpdRefPso = nullptr;
    ID3D12PipelineState* zpdRasterPso = nullptr;
    uint32_t zpdPointSize = 0, zpdColorCtl = 0;
    uint32_t zpdVtxAddr = 0, zpdVtx[4] = {};
    // ── P1 EDRAM path (SR_PM4_RTCACHE=1, gfx/d3d12_pm4_edram.cpp) ─────────────
    // Built by the drain from the draw's register snapshot; the render loop
    // binds the per-key host RTs, runs ownership transfers and executes
    // resolves through the EDRAM mirror.
    bool pm4 = false;
    // iter-466 cp round 2 (SR_CP_TEX_SKIPSAME): texFetch equals the previous pm4 draw batch's
    // (non-resolve) in this vector - the texture prepass has nothing new to collect from it.
    bool texSame = false;
    edram::Regs pm4Regs;
    edram::DrawTargets pm4Targets;
    D3D12_RECT pm4Scissor{};
    // P1 in-shader vertex fetch: the VS reads staged guest memory (root SRV
    // 10 = immutable native descriptor) through patched fetch constants (root CBV
    // 9); indexed draws bind an owned native snapshot of the planned u32 indices.
    bool vsf = false;
    uint64_t vsfFetchCbOff = 0;
    uint64_t vsfIbOffset = 0;
    NativeBufferSlice vsfRawVertices;  // immutable staged shader-fetch bytes
    // iter-460 perf (SR_PM4_VSF_SHM): the VS fetches from the shared memory
    // (fetch address = guest physical), and the shared-memory uploads its
    // drain queued are recorded right before this batch.
    bool vsfShm = false;
    uint32_t shmPendBegin = 0, shmPendEnd = 0;
    // iter-461: an auto-indexed draw whose index remap is the identity draws
    // without an index list (SV_VertexID = vsfFirstVertex + i).
    bool vsfNoIb = false;
    uint32_t vsfFirstVertex = 0;
    uint32_t pm4Blend0 = 0;  // RB_BLENDCONTROL0 (diagnostics)
    uint32_t drawOrdinal = 0;  // the stream's 1-based draw packet ordinal (EDRAM oracle)
#if SR_ORACLE
    std::shared_ptr<const memres::FetchCapture> resourceFetchCapture;
    std::shared_ptr<const memres::FetchPagePayload> resourceFetchPayload;
    uint64_t resourceFetchId = 0, resourceFetchOwnerSum = 0;
    uint64_t resourceFetchGrantDigest = 0, resourceFetchGrantGeneration = 0;
    uint32_t resourceFetchGrantSerial[2] = {}, resourceFetchGrantCount = 0;
    uint32_t resourceFetchTarget = 0, resourceFetchOwnerSeq = 0, resourceFetchConstantBlocks = 0;
    bool resourceFetchOwnerKnown = false, resourceFetchDeclaredDword = false;
    uint64_t resourceFetchOriginSequence = 0;
    bool resourceFetchOriginMatched = true;
    std::vector<std::shared_ptr<const memres::ConsumerCheck>> resourceConsumerChecks;
    mutable std::shared_ptr<const memres::FetchCapture> resourceConsumerFetch;
    mutable std::shared_ptr<const memres::ConsumerFetchLease> resourceConsumerFetchLease;
    uint64_t resourceConsumerId = 0;
    uint32_t resourceConsumerConstantBlocks = 0;
    // step 6 I10p check mode (SR_XLAT_CB_ZEROCOPY_CHECK): the CP's block-use masks and the hashes of the bytes this
    // draw's VS / PS / fetch views must hold (from the cacheable sources), and the CB third + generation they live in.
    uint32_t zcVsUse = 0, zcPsUse = 0, zcThird = 0, zcGen = 0;
    uint64_t zcVsHash = 0, zcPsHash = 0, zcFetchHash = 0;
#endif
  };
#if SR_ORACLE
  struct ResourceFetchCounters {
    std::atomic<uint64_t> considered{0}, awaitingOrigin{0}, noTargetPage{0};
    std::atomic<uint64_t> pageCandidates{0}, declaredDword{0}, captured{0}, capped{0};
  };
  static ResourceFetchCounters& resourceFetchCounters();
  static void resourceFetchReport(const char* phase);
  void resourceCaptureFetch(DrawBatch&, const GuestDrawCommand&, const CachedShader&);
  void resourceCaptureFetchInputs(DrawBatch&, uint64_t id, uint32_t target, uint32_t constantBlocks);
  memres::FetchCaptureInput resourceCurrentFetchInput(const DrawBatch&) const;
  bool resourceInspectFetch(const DrawBatch&, memres::FetchAnalysis* output = nullptr,
                            memres::FetchPayloadIntersection* payloadOutput = nullptr);
  memres::ConsumerWitness resourceConsumerWitness(uint32_t page) const;
  void resourceCaptureConsumer(DrawBatch&, const GuestDrawCommand&);
  bool resourceValidateConsumer(const DrawBatch&);
  std::vector<std::shared_ptr<const memres::ConsumerBaseline>> m_resourceRecordedBaselines;
  struct ResourceTextureProbe {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback, texture, arena;
    NativeBufferSlice constants;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints{};
    uint64_t ticket = 0, total = 0, constantOffset = 0;
    uint32_t ordinal = 0, slot = 0, mapping = 0, format = 0;
    bool recorded = false;
    std::string path;
  };
  std::unique_ptr<ResourceTextureProbe> m_resourceTextureProbe;
  bool m_resourceTextureProbeUsed = false;
  bool resourceTextureProbe(const DrawBatch&, const TranslatedConstantSnapshot&);
  void resourceTextureProbeSubmitted(uint64_t);
  void resourceTextureProbeCollect();
  void resourceTextureProbeDiscard();
#endif
  std::vector<DrawBatch> m_drawBatches;
  uint32_t m_totalGeoVerts = 0;

  // ── Translated-shader pipeline (iter-359c) ────────────────────────────────
  // Root CBVs b0-b4 space4 (b3/b4 alias b0/b1 for dynamic constant indexing),
  // bindless SRV tables t0 spaces 0-2, sampler table s0 space3 (contract:
  // tools/xenosrecomp/README.md + ENGINE_MAP §6d "Shaders"). v1 textures =
  // 1×1 white fallback at descriptor index 0.
  // Sized for a full golden-trace frame per half: 3,501 draws of canonical
  // float4-per-attr vertices ran the old 32MB half dry at draw ~1,060
  // (census: vbFull=2207) — every later draw fell back to the fixed-function
  // white painter, which WAS the white flood.
  // iter-461: THREE thirds (a frame on the GPU, one recording, one draining -
  // the render thread), rotated at the first drain of a frame set.
  static constexpr uint64_t kXlatVBBytes = 512ull << 20;  // staged indices / decoded verts, 3 thirds
  static constexpr uint64_t kXlatCBBytes = 192ull << 20;  // CPU planning scratch, 3 thirds
  uint32_t m_xlatThird = 0;
  bool m_xlatReady = false;  // the init-time decision; read it through xlatReady()
  // render/boot-attach: m_xlatReady && the shader cache's installedPresent() (the probe until the
  // background load's stage 1 ends, then its answer). Never blocks.
  bool xlatReady() const;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_xlatRootSig;  // development HLE consumer
  NativeRootSignature m_nativeXlatRoot;
  std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_xlatPsoCache, m_nativeXlatPsoCache;
  XlatScratch m_xlatVertices, m_xlatConstants;  // cacheable CPU planning scratch (step 4 7d(b): committed by high-water)
  // Step 4 7d(b): commits the scratch BEFORE a write up to these ends (0 = that buffer not written); false = the
  // commit failed - the caller rejects the draw as when the third is full. Logs each new 16 MB of committed scratch.
  bool xlatCommit(uint64_t vbEnd, uint64_t cbEnd);
  NativeUploadArena m_nativeConstants, m_nativeGeometry, m_nativeRawGeometry;
  // step 6 I10p (SR_XLAT_CB_ZEROCOPY, gfx/xlat_cb_gpu.hpp; Fable 40 + the coordinator's variant): the view path.
  // m_xlatCBGpu = the write-only upload twin of the CB thirds: the CP writes each draw's changed VS / PS blocks (M1)
  // and fetch block (M2) there, at the cursor offsets the scratch used, and reserves the draw's 512-byte shared slot
  // there, which the RENDER thread fills (texture indices patched) only when the shared bytes change. The batch's
  // offsets are the views; the CB third and its fence own them. m_xlatConstants then holds only the CP's shared blocks
  // (the render's read-modify-write source) at a compact cursor (kXlatShBytes, 3 thirds by m_xlatThird).
  static constexpr uint64_t kXlatShBytes = 48ull << 20;   // the view path's scratch: shared blocks, 3 thirds
  XlatCBGpu m_xlatCBGpu;
  bool m_zcOn = false;                      // the view path: xlatCbZeroCopyOn() and the twin initialized
  uint64_t m_xlatShUsed = 0;                // the view path: the scratch's shared-block cursor
  uint32_t m_xlatCbThird = 0;               // the third whose CB range the set writes (= m_xlatThird; the mutant differs)
  uint32_t m_zcPsTmp[1024] = {};            // CP: a PS upload the host options patch, composed here before its write
  // CP: the set generation (every rotation into a set, both rotation sites; batches carry it as zcSet).
  uint64_t m_zcCpSet = 0;
  // CP, at both rotation sites (the drain's !pm4Append rotation: drainSite; replayPrepareTail), after the fence wait
  // for m_xlatThird: the set's CB third and generation; in check mode the finished set's poison range, the TEST-ONLY
  // mutant (drain site) and the new third's retire assert / recheck / poison (zcBeginThird).
  void zcRotateCbThird(bool drainSite);
  // Render (Astra I10p r1 P1: a retained set can be RENDERED AGAIN - the synchronous present of an empty drain - while
  // its earlier submission may still read the slots): per CB third, the set generation whose slots were last written
  // and the highest slot written for it; a slot at or below it is never rewritten in that generation (staged instead).
  // The shared bytes last bound, their generation and address (reused only within the same generation).
  uint64_t m_zcSlotSet[3] = {}, m_zcSlotHigh[3] = {};
  bool m_zcSlotAny[3] = {};
  std::array<uint8_t, TranslatedConstantSnapshot::kSharedBytes> m_zcSharedBytes{};
  uint64_t m_zcSharedSet = 0;
  bool m_zcSharedValid = false;
  D3D12_GPU_VIRTUAL_ADDRESS m_zcSharedAddr = 0;
  NativeBufferSlice m_zcSharedSlice;
  uint64_t m_zcSharedKeptIn = 0;            // the m_nativeRecordingSerial m_zcSharedSlice was kept in (0 = none)
  uint64_t m_nativeRecordingSerial = 0;     // openNativeRecording's count (a recording's identity)
  // [XLAT_CB_ZEROCOPY] counters: render (binds, shared writes / reuses / fallbacks), CP (blocks and bytes written,
  // fetch dedups, shared-scratch-full rejects)
  uint64_t m_zcBinds = 0, m_zcSharedWrites = 0, m_zcSharedReuses = 0, m_zcSharedFallbacks = 0;   // render thread
  // CP thread (single writer: zcBump's relaxed load + store, a plain add - read by the render thread's line)
  std::atomic<uint64_t> m_zcVsWrites{0}, m_zcPsWrites{0}, m_zcFetchWrites{0}, m_zcFetchDedups{0}, m_zcBytes{0},
      m_zcShFull{0}, m_zcCbPeak{0}, m_zcShPeak{0};   // peaks: a set's use of its third (twin / compact scratch)
  static void zcBump(std::atomic<uint64_t>& a, uint64_t n = 1) {
    a.store(a.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
  void zcCounterLine(const char* when);     // the [XLAT_CB_ZEROCOPY] counters (beside [NATIVE_RECORDING])
  // The view path's bind (bindXlatConstants): root CBVs from the twin; the shared slot written on change.
  void zcBindConstants(const DrawBatch& b);
#if SR_ORACLE
  // I10p check mode (SR_XLAT_CB_ZEROCOPY_CHECK=1): m_zcShadow = the cacheable shadow of the twin (every twin write
  // lands there too; every check reads it, never the twin). Per CB third, under m_zcMu: the generation the render
  // thread may consume, the ranges it bound (compare-on-retire), the open recording that bound them and the last
  // submission ticket carrying them (the retire assert: every recording that bound a third's views has been
  // submitted and has completed before the CP reuses the third).
  bool m_zcCheck = false, m_zcMutant = false;
  XlatScratch m_zcShadow;
  uint64_t m_zcSetSerial = 0;               // CP: rotations (the mutant's cadence)
  uint32_t m_zcGen[3] = {};                 // CP: each CB third's generation (its rotations into it)
  uint64_t m_zcThirdHigh[3] = {};           // CP: each CB third's used end (the poison range at its reuse)
  struct ZcRange { uint64_t off; uint32_t use; uint32_t bytes; uint64_t hash; };
  struct ZcThirdLive {
    uint32_t gen = 0;                       // the generation whose views may be consumed
    uint64_t openRec = 0;                   // the open recording that bound them (0 = none)
    uint64_t ticket = 0;                    // the last submission that carried them
    std::vector<ZcRange> ranges;
  };
  ZcThirdLive m_zcLive[3];
  std::mutex m_zcMu;
  std::mutex m_zcShadowMu;                  // the shadow's writes (XlatCBGpu), the consume / retire hashes
  // The deterministic fence assert (Astra r1 P2): pm4WaitThirdFree's last third and its count, checked at every
  // rotation into a CB third (a CB third is written only after its own fence wait in that rotation).
  uint32_t m_zcWaitedThird = ~0u;
  uint64_t m_zcWaits = 0, m_zcWaitsSeen = 0;
  void zcNoteThirdHigh(uint32_t third, uint64_t used);   // the finished set's used end, if inside that third
  // Astra r2 P3: the generation that owns m_xlatCBUsed (set where the drain resets the cursor into its set's third);
  // a replay tail's empty set keeps an older generation's cursor, so its own poison range is recorded as empty.
  uint64_t m_zcCursorSet = 0;
  // checked_xframe (the freeze's test bar): view checks of a generation that an EARLIER recording already bound and
  // SUBMITTED (the first consumer outstanding or complete: a zpd partial's continuation, a retained set rendered again)
  std::atomic<uint64_t> m_zcChecked{0}, m_zcCheckedXframe{0}, m_zcBad{0}, m_zcRetireChecked{0}, m_zcRetireBad{0},
      m_zcFenceBad{0}, m_zcRetired{0}, m_zcStaleGen{0};
  std::vector<ZcRange> m_zcRetireScratch;   // CP: the retired ranges (capacity kept)
  bool zcShadowInit();                      // the shadow + its mutex installed in the twin (init; the fixture)
  void zcConsume(const DrawBatch& b, const uint8_t* shared, bool sharedWritten);   // render, at bind
  void zcSubmitted();                       // render, after a recording's submit (clExecute)
  void zcBeginThird(uint32_t third);        // CP, at the rotation into a CB third (after its fence wait)
#endif
  TranslatedSamplerCache m_nativeSamplers;
  TranslatedTextureCache m_nativeTextures;
  uint64_t m_nativeSamplerBindings = 0;
  uint64_t m_nativeTextureDraws = 0, m_nativeTextureBindings = 0;
  uint64_t m_nativeConstantDraws = 0;
  // The previous draw's staged constants and a cacheable copy of their bytes. A draw
  // whose four blocks equal them binds the same immutable slice (bindXlatConstants).
  TranslatedConstantSnapshot m_lastConstants;
  std::array<uint8_t, 2 * TranslatedConstantSnapshot::kFloatBytes + TranslatedConstantSnapshot::kSharedBytes +
                          TranslatedConstantSnapshot::kFetchBytes>
      m_lastConstantBytes{};
  bool m_lastConstantFetch = false;
  uint64_t m_nativeConstantReuses = 0;
  uint64_t m_nativeVertexBindings = 0, m_nativeIndexBindings = 0;
  uint8_t* m_xlatVBPtr = nullptr;
  uint8_t* m_xlatCBPtr = nullptr;
  uint64_t m_xlatVBUsed = 0, m_xlatCBUsed = 0;       // per-frame cursors
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_xlatSrvHeap;      // shader-visible
  // Native recipes own every resource published into this development heap.
  // Native draws select immutable compact tables from these recipes. The older
  // HLE / development utility consumers still require conversion from this heap.
  std::vector<NativeViewRecipe> m_xlatViews;
  // View identity per slot, set when publishXlatSrv changes a slot's recipe: equal
  // views (same resource or staged range, same SRV) get the same id in any slot, and
  // ids are never reused. A draw whose 48 selected ids equal a remembered key's reuses
  // that key's texture table without a content lookup (bindXlatTextures). A key's
  // table keeps its views' resources alive, so no other resource can take their
  // addresses while the key can match. Direct-mapped. Keys hold a table (not its raw
  // view) and are dropped at the cache's page ages, so a reuse never skips a migration
  // or keeps a page the cache would drain.
  std::vector<uint32_t> m_xlatViewId;
  struct ViewIdentity {
    const void* storage = nullptr;  // resource, or the staged range's upload resource
    uint64_t offset = 0, size = 0;  // staged range (buffer views)
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  };
  std::unordered_map<uint64_t, std::vector<std::pair<ViewIdentity, uint32_t>>> m_viewIds;
  size_t m_viewIdCount = 0;
  uint32_t m_nextViewId = 1;  // 0 = a slot's initial empty recipe
  uint32_t viewIdOf(const NativeViewRecipe& recipe);
  struct TexKey {
    std::array<uint32_t, 48> ids{};
    TranslatedTextureSnapshot snapshot;
    uint32_t refs = 0;  // selected slots holding a resource or buffer (stats)
    bool valid = false;
  };
  NativeDescriptorVersion m_drawSamplers;  // for gfxopt version m_drawSamplersVersion
  uint64_t m_drawSamplersVersion = 0;
  uint64_t m_drawSamplersVolGen = 0;       // ... and volume-sampler generation m_drawSamplersVolGen
  static constexpr uint32_t kTexKeys = 8192;
  std::vector<TexKey> m_texKeys;
  uint64_t m_texKeyTail = 0;
  // Drop keys whose table is at least `age` pages behind `tail` (default: all).
  void dropTexKeys(uint64_t tail = UINT64_MAX, uint64_t age = 0);
  uint64_t m_nativeTextureReuses = 0;
  // step 6 I3-B (bindless, Fable ruling 44). Each slot's view dimension (set only by publishXlatSrv): a draw's
  // selection is checked against it, never against the recipe.
  std::vector<uint8_t> m_xlatSlotDim;
  UINT m_xlatSrvInc = 0;
  // The raw ring: absolute write counter, the open recording's first write, and each submitted recording's writes
  // with its native submission (a slot is rewritten only after the submission that read it completed).
  struct RawRingSegment { uint64_t start = 0, end = 0, submission = 0; };
  std::deque<RawRingSegment> m_rawRingSegments;
  uint64_t m_rawRingWrites = 0, m_rawRingRecStart = 0, m_rawRingCheckedTo = 0;
  // The last raw view (reused within the same recording only).
  uint64_t m_rawLastRec = 0, m_rawLastOffset = 0, m_rawLastSize = 0;
  const void* m_rawLastResource = nullptr;
  uint32_t m_rawLastSlot = 0;
  // The sampler version the open recording retains (once per recording and per sampler version).
  uint64_t m_samplersKeptRec = 0, m_samplersKeptVersion = 0, m_samplersKeptVolGen = 0;
  uint64_t m_i3bDirectDraws = 0, m_i3bRawWaits = 0, m_i3bSlotCow = 0, m_i3bExhaustWaits = 0;
  void i3bWaitLine(const char* kind, uint64_t detail);  // every raw-ring / exhaustion wait: a HITCH frame for Q
  // A slot whose view must change becomes a NEW slot; the old one and its resource (moved from oldResource) retire
  // with the frame fence (copy-on-write). 0 = no slot: nothing changed, oldResource untouched.
  uint32_t texSlotCow(uint32_t oldSlot, Microsoft::WRL::ComPtr<ID3D12Resource>& oldResource, const char* site);
  // Slot exhaustion's victim, waited for until the GPU finished the frames that bound its slot; false = none.
  bool texExhaustVictim(uint64_t& victim);
#if SR_ORACLE
  // The I3-B closing check: per slot its write generation, the last native submission that read it, and the open
  // recording that bound it; the open recording's (slot, generation) binds, checked at its submission.
  std::vector<uint32_t> m_i3bSlotGen;
  std::vector<uint64_t> m_i3bSlotSubmitted, m_i3bSlotBoundRec;
  std::vector<std::pair<uint32_t, uint32_t>> m_i3bRecBinds;
  uint64_t m_i3bChecked = 0, m_i3bBadInFlight = 0, m_i3bBadOpen = 0, m_i3bLegacyRefs = 0;
  void i3bSlotBound(uint32_t slot);
  void i3bSlotWrite(uint32_t slot, const char* what);
  void i3bSubmitted(uint64_t submission);
#endif
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_xlatSamplerHeap;  // shader-visible
  Microsoft::WRL::ComPtr<ID3D12Resource> m_xlatWhite2D, m_xlatWhite3D, m_xlatWhiteCube;
  bool createXlatPipeline();
  bool createXlatSrvHeap();  // step 6 I3-B: the SRV heap + its per-slot state (createXlatPipeline's first step)
  // PSO keyed by shader pair + per-draw render state (iter-359e: depth/blend/mask
  // from RB_DEPTHCONTROL / RB_BLENDCONTROL0 / RB_COLOR_MASK draw-time snapshots).
  // pointSprite (iter-370) selects the kPointList variant: sprite-expansion GS
  // plus POINT topology — a different pipeline for the same VS/PS pair.
  // iter-447: rtvFmt/dsvFmt make the TARGET FORMATS explicit key fields, which is
  // the reference's shape (video.cpp:144-145 carries renderTargetFormat/
  // depthStencilFormat as PipelineState members set from the BOUND surface at
  // :3339/:3361, and hashes the whole struct at :4139). The hdrTarget/poolDepth
  // BOOLEANS below are approximations of exactly this, and they are why the pool
  // could serve only ONE format pair and refused 723,398 draws/run.
  // ⚠ DXGI_FORMAT_UNKNOWN means "derive from the booleans" — the legacy movie
  // path passes UNKNOWN and therefore hashes and builds BYTE-IDENTICALLY to
  // before. Do not collapse the booleans away until that path is re-verified.
  ID3D12PipelineState* getXlatPso(uint64_t vsHash, uint64_t psHash,
                                  uint32_t depthControl, uint32_t blendControl,
                                  uint32_t colorMask, bool depthOnly, bool hdrTarget,
                                  bool pointSprite = false,
                                  bool poolDepth = false,
                                  DXGI_FORMAT rtvFmt = DXGI_FORMAT_UNKNOWN,
                                  DXGI_FORMAT dsvFmt = DXGI_FORMAT_UNKNOWN,
                                  bool noDsv = false, bool nativeTextures = false);
  // ── P1: EDRAM render-target cache for the PM4 path (SR_PM4_RTCACHE=1) ──────
  // docs/RENDERER_PM4_HYBRID.md P1. Logic: gfx/edram.hpp (tested by
  // difftest --edram-test). D3D12 side: gfx/d3d12_pm4_edram.cpp. One host RT
  // per edram::RtKey; a CPU mirror of the 10 MiB EDRAM is the shared truth
  // that ownership transfers and resolves go through (slow, checkable; the
  // reference a later GPU path is tested against).
  struct Pm4HostRt {
    NativeImageLease image;  // owns the native resource and its RTV/DSV descriptor
    ID3D12Resource* res = nullptr;  // cached borrow from image, valid for the lease
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;  // RTV / DSV format
    D3D12_CPU_DESCRIPTOR_HANDLE view{};      // RTV or DSV
    uint32_t w = 0, h = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    int32_t srv = -1;  // GPU EDRAM: slot in the xlat heap's RT SRV band (color only)
    int32_t dsrv = -1;  // iter-466 res4k: a depth RT's depth-plane SRV slot; the stencil plane is +1
  };
  struct Pm4PsoDesc {
    uint64_t vsHash = 0, psHash = 0;
    bool pointSprite = false;
    bool vsf = false;       // the VS's in-shader vertex-fetch variant, no input layout
    bool stripCut = false;  // index 0xFFFFFFFF restarts the strip (primitive reset)
    bool depthClip = true;  // !PA_CL_CLIP_CNTL.clip_disable
    uint32_t cull = 0;      // 0 none, 1 front, 2 back (polygonal primitives only)
    bool frontCcw = true;   // PA_SU_SC_MODE_CNTL.face == 0
    int32_t depthBias = 0;  // polygon offset, D3D10 integer units
    float depthBiasSlope = 0.0f;
    uint32_t numRt = 0;           // slots 0..numRt-1 (holes use the null RTV)
    DXGI_FORMAT rtv[4] = {};
    uint8_t writeMask[4] = {};
    uint32_t blend[4] = {};
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    bool zEnable = false, zWrite = false;
    uint32_t zFunc = 7;
    // Stencil (Xenia pipeline_cache): RB_DEPTHCONTROL's stencil fields and
    // RB_STENCILREFMASK's read/write masks (bits 8..23).
    bool stencil = false;
    uint32_t stencilCtl = 0, stencilMasks = 0;
    // p2.zpd raster probe only (zpdProbeDesc): no pixel shader. Every guest pipeline has one (getPm4Pso refuses a
    // missing translation); only the probe deliberately leaves it out.
    bool noPixel = false;
    // step 4 lane C (gfx/alpha_test.hpp, SR_XLAT_ALPHA_TEST): draw with the PS's alpha-test variant
    // (<hash>.ps.at.dxil, tools/xenosrecomp/0005) - RB_COLORCONTROL enables the Xenos alpha test.
    bool alphaTest = false;
  };
  // p2.zpd: a query draw's probe pipeline (DrawBatch::zpdRefPso / zpdRasterPso) from its own description: depth
  // tested ALWAYS (kept enabled, so the depth block that counts samples stays on), no depth / stencil / colour
  // write, stencil off, the formats and everything else unchanged (the bound targets must match); raster = also no
  // pixel shader.
  static Pm4PsoDesc zpdProbeDesc(const Pm4PsoDesc& d, bool raster);
  ID3D12PipelineState* m_pm4ZpdRefPso = nullptr;     // pm4PrepareDraw's probe variants of the PSO it returns
  ID3D12PipelineState* m_pm4ZpdRasterPso = nullptr;  // (null outside an open query / with the probe off)
  uint32_t m_pm4ZpdVtxAddr = 0, m_pm4ZpdVtx[4] = {};  // ... and the query draw's first vertex (DrawBatch::zpdVtx)
  NativeImageStore m_nativeImages;
  std::unordered_map<uint32_t, Pm4HostRt> m_pm4Rts;  // development address-to-lease adapter
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_pm4RtvHeap, m_pm4DsvHeap;
  uint32_t m_pm4RtvUsed = 0, m_pm4DsvUsed = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE m_pm4NullRtv{};
  // (C1 stage 3: the CPU EDRAM mirror and the tile ownership map are the oracle unit's - m_pm4Edram below under
  // SR_ORACLE, the map a static of gfx/d3d12_edram_model.cpp.)
  Microsoft::WRL::ComPtr<ID3D12Resource> m_pm4Readback, m_pm4Upload;
  // step 4 7d(c): the readback's creation (pm4Init, or deferred to its first user in release: SR_PM4_READBACK_LAZY)
  static bool pm4ReadbackLazy();
  bool pm4EnsureReadback();
  uint8_t* m_pm4UploadPtr = nullptr;
  uint64_t m_pm4UploadUsed = 0;
  NativePipelineCache m_nativePipelines;
  bool m_pm4Ready = false;
  // iter-467 native-b (SR_NB_ZERO_INIT): no EDRAM snapshot was restored (a live run, a scaled replay): every tile no
  // surface has claimed still holds the power-on zero.
  bool m_pm4EdramPristine = true;
  // A frame's batch set is open: drains append instead of starting a new set.
  bool m_pm4DrainOpen = false;
  uint64_t m_pm4DrainNs = 0;  // draw-time translation cost of the open frame set
  // iter-460: while a PM4 batch binds its textures, uploads are recorded on the
  // FRAME list in draw order (Xenia's texture loads) instead of a dedicated list
  // + full GPU wait each; staging buffers retire with the frame fence.
  bool m_texInline = false;
  // iter-460 S2/S3 (SR_PM4_SHMEM, d3d12_pm4_shmem.cpp): Xenia's shared memory
  // (512 MiB by guest physical address) and GPU resolves into it.
  bool pm4ShmInit();
  void pm4ShmState(D3D12_RESOURCE_STATES s);
#if SR_ORACLE
  void pm4ShmBeginFrame();
#endif
  void pm4ShmAfterFlush();
  bool pm4ShmRequest(uint32_t phys, uint32_t len);
#if SR_ORACLE
 public:
  // iter-467 native-b: a validated range's proof (SR_NB_SHM_RANGE_CACHE) - the release clock value and the owner
  // version under which every page of [phys, phys + len) was clean in the range's window. A pre-resolved draw keeps
  // one per stream; pm4ShmRequest keeps a per-thread table of them.
  struct ShmRangeTicket {
    uint64_t stamp = 0, ownSum = 0;
    uint32_t phys = 0, len = 0;
    int32_t own = 0;
    bool valid = false;
  };
  // A drain-time request with the caller's ticket (strand A's pre-resolved draws): answered from the ticket alone
  // (no m_shmMu, no page walk) while it is valid for [phys, phys + len); else the full request, which renews the
  // ticket when every page is clean. Same contract as pm4ShmRequest; thread-safe (call from the drain thread).
  bool pm4ShmEnsure(uint32_t phys, uint32_t len, ShmRangeTicket& t);
 private:
  bool pm4ShmRequestT(uint32_t phys, uint32_t len, ShmRangeTicket* ext);
#endif
  // iter-460 perf (SR_PM4_VSF_SHM): a drain-time request (the frame list is
  // closed between presents, and a page's upload must land in draw order)
  // stages the pages into the arena and QUEUES the GPU copy; renderPm4Batches
  // records a batch's queued copies right before it. The arena half flips at
  // the END of a frame (pm4ShmEndFrame) so the next frame's drains stage into
  // the half whose GPU work moveToNextFrame has waited for.
  // iter-460 perf: a drain COLLECTS the pages its draws fetch and the
  // textures they bind (drainCollectTextures), then validates them all at
  // once - the 4 KB hashes on the pool (one request's pages are too few to
  // spread). A drain is its own epoch; the render accepts any validation of
  // this frame (m_shmEpochFrameStart) so its binds re-hash nothing.
#if SR_ORACLE
 public:
  struct ShmCand { uint32_t p; int rangeOwn; };  // (public: the thread-local collect target names it)
 private:
  std::vector<ShmCand> m_shmCollect, m_shmPrepassCollect, m_shmCandsTmp;
#endif
  // The calling thread's request mode (thread-local: the CP drains while the
  // render thread binds): collect into which list / queue the copies.
  void pm4ShmSetDrainMode(bool collect, bool defer);
  // Everything that reads or writes the page tables, the stamps or the arena
  // cursors (the drain and the render thread both do).
  std::recursive_mutex m_shmMu;
  uint32_t m_shmEpochFrameStart = 1;
#if SR_ORACLE
  std::unordered_set<uint64_t> m_shmTexCollected;  // fetch keys collected this frame
  bool shmPrepRanges(const uint32_t* tf, void* ent);  // iter-467 native-b SR_NB_PREPASS_FLAT (PrepEnt)
  void shmPrepRequest(const void* ent);
  uint32_t m_shmTexPrev[96] = {};                  // the previous draw's texture fetches
  // hashUnlocked: the host pointers + page hashes run BEFORE m_shmMu is taken (the CP's
  // collected pages: their epochs were stamped at request time); decisions stay locked.
  void pm4ShmValidate(std::vector<ShmCand>& cands, bool deferred, bool hashUnlocked = false);
  void pm4ShmCollectFlush();
  // iter-466 cp lane (SR_SHM_WT_CHECK=1): a page skipped as clean, hashed anyway and compared
  // with its validation's hash (m_shmMu held).
  void shmWtCheck(uint32_t page, int own);
#endif
  bool texShmRanges(const uint32_t* fetch6, uint32_t* base, uint32_t* baseLen, uint32_t* mipPhys,
                    uint32_t* mipLen);
  // The render's texture pre-pass (SR_PM4_SHM_PREPASS): every texture the
  // frame's batches bind is validated in ONE parallel flush at frame start,
  // except pages inside this frame's resolve extents (m_shmResolveMask) -
  // those are GPU-written mid-frame and keep the bind-time check, in draw
  // order after their resolve. (Validating textures at DRAIN time uploaded a
  // resolve destination's stale CPU copy over the resolve: the gate broke.)
#if SR_ORACLE
  void shmCollectTextures(const uint32_t* fetchRaw96);
#endif
  bool pm4ShmResolveExtent(const edram::Regs& regs, uint32_t& extStart, uint32_t& extLen);
  // iter-465 perf2: deferred = the CP runs it at the set's submit under SR_PM4_RENDER_THREAD
  // (copies queued, recorded by the render thread before the first batch), so every
  // validation - drain and prepass - happens on one thread in set order.
#if SR_ORACLE
  void pm4ShmTexturePrepass(const std::vector<DrawBatch>& batches, bool deferred = false);
  std::vector<uint8_t> m_shmResolveMask;  // per page: resolved this frame
#endif
  // A queued copy into the shared memory: from the arena (src = nullptr) or a
  // spill chunk (iter-461: the drain third filled).
  struct ShmPend {
    uint64_t shmOff, arenaOff, bytes;
    bool done;
    ID3D12Resource* src = nullptr;
    // An exact CPU-write patch (ResourceMemory::notePatch): a few dwords inside a
    // page. It neither makes the page CPU-owned nor clears its scaled copy.
    bool patch = false;
#if SR_ORACLE
    // Exactly the CPU images in this copy. Admission is not recording: publish
    // these handles only at this entry's real CopyBufferRegion command cursor.
    std::vector<std::shared_ptr<const memres::ConsumerBaseline>> consumerCopies;
#endif
#if SR_ORACLE
    // Non-null only for an explicit trace MemoryRead. The upload bytes and
    // alternate-window baseline belong to that command, not current guest RAM.
    std::shared_ptr<const std::vector<uint64_t>> replayHashA;
    const uint8_t* replayBytes = nullptr;
#endif
  };
#if SR_ORACLE
  struct ReplayRestorePage {
    std::array<uint8_t, 4096> bytes;
    uint64_t hashA;
  };
  std::map<uint32_t, ReplayRestorePage> m_replayRestores;
  bool m_replayRestoreActive = false;
  void replayStageRestores();
#endif
  bool pm4ResourceRequest(uint32_t phys, uint32_t len);
  // step 6 I4 (SR_CP_RES_REQ_LEAN): one site's ranges (a draw's asked texture spans). The same-drain memo answers its
  // hits before m_shmMu; the misses share one hold. Without the gate: one request each. (The vertex streams stay one
  // request each: pm4StageVsf asks inside the census's per-slot scope.)
  void pm4ResourceRequestSite(const memres::ResourceSpan* spans, uint32_t count);
  // The request's body under m_shmMu (page range already taken; `decided`: its resourceAbDecision already counted).
  bool pm4ResourceRequestLocked(uint32_t phys, uint32_t len, uint32_t first, uint32_t last, bool decided);
  // The same-drain memo before m_shmMu: true = current (the request is done). Drain asks only.
  bool resourceRangeMemoHit(uint32_t phys, uint32_t len, uint32_t first, uint32_t last);
  static uint32_t resourceRangeMemoSlot(uint32_t phys, uint32_t len);
#if SR_ORACLE
  void resourceRangeMemoCheck(uint32_t first, uint32_t last, uint64_t sequence);
  void resourceRangeMemoResolve();  // the CHECK's suspects a drain later
#endif
  void cpResReqCensus();  // [CP_RES_REQ] at a drain's end
  void resourcePrepareResolve(DrawBatch& batch);
  void resourcePrepareResolveRange(uint32_t phys, uint32_t len, uint32_t generation);
#if SR_ORACLE
  // mem.resources frozen-pass A/B (gfx/resource_ab.hpp), dual mode only: the cross-arm consistency rules and the
  // model arm's GPU note at a resolve's drain.
  void resourceAbNativeCopied(uint32_t page, uint32_t window, const uint8_t* bytes);  // INV-M
  void resourceAbModelCopied(uint32_t page, bool fromA, const uint8_t* src);         // INV-R
  void resourceAbNoteResolve(DrawBatch& batch);
  int8_t m_resArmSet = -1;            // the CP's open set's arm (0 model, 1 native; -1 not dual)
  // mem.resources textures_from_resolves (gfx/resource_texid.hpp): pages of [phys, +len) holding resolve output now
  // (the deciding arm's view), and a 2D clean-mark hit (its cache entry's recorded class).
  // ... and, given cpuAfter, the pages a CPU note reached after their GPU write (src: the first one's note source).
  uint32_t texIdGpuPages(uint32_t phys, uint32_t len, uint32_t* cpuAfter = nullptr, uint32_t* src = nullptr) const;
  void texIdCleanHit(uint64_t key, const uint32_t* f);
  std::vector<uint8_t> m_abForgot;    // pages INV-M made the model forget (its next upload re-establishes them)
  std::vector<uint64_t> m_abForgotHash;  // ... and the hash of the bytes the native route captured there
  std::vector<uint8_t> m_abClob;      // pages whose model upload replaces an EXECUTED GPU write (its clobber rule fired)
  std::vector<uint32_t> m_resEpochOf;  // dual mode: the native arm's own validation stamps (Astra review1 P1-1)
#endif
  // The validation stamps the native route reads and writes: m_shmEpochOf, or its own array in dual mode (the page
  // model's stamps then never satisfy a native request, nor the native route's a model one). Under m_shmMu.
  uint32_t* resEpochOf();
  uint32_t m_resourceGpuGeneration = 0;  // currently executing resolve's producer generation
  std::vector<ShmPend> m_shmPending;
  // Arena thirds: the render's third (immediate uploads and this frame's
  // queued copies) and the drain's third (the next frame's staging); equal on
  // the synchronous path, rotated at pm4ShmEndFrame. Cursor and the end still
  // owed to queued copies, per third.
  uint32_t m_shmRenderThird = 0, m_shmDrainThird = 0;
  uint64_t m_shmUploadUsedT[3] = {}, m_shmArenaHighT[3] = {};
  void pm4ShmRecordPending(std::vector<ShmPend>& list, uint32_t begin, uint32_t end);
  void pm4RecordBatchPending(const DrawBatch& batch, std::vector<ShmPend>& list, uint32_t& cursor);
  // iter-461: a frame set's first drain rotates BOTH arenas (xlat VB / CB and
  // the shm upload arena) into the next third - the frame that used it two
  // presents ago has completed (the render thread waits for its fence). The
  // set's first epoch is what its render accepts validations from.
  void pm4ShmBeginSet(uint32_t third);
  void pm4ShmEndFrame();
  // iter-461: a third that fills at drain time spills into a chunk tagged with
  // the third (the frame list belongs to the render thread, so no flush); the
  // chunks are released when the third is reused.
  struct ShmSpill {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    uint8_t* ptr = nullptr;
    uint64_t size = 0, used = 0;
    uint32_t third = 0;
  };
  std::vector<ShmSpill> m_shmSpill;
  uint64_t m_shmSpillBytes = 0;
  uint8_t* shmSpillAlloc(uint32_t third, uint64_t bytes, uint64_t& off, ID3D12Resource*& res);
  void shmSpillRelease(uint32_t third);
  bool vsfShmSrvCreate();       // raw view of the shared memory for t0 space5
  D3D12_GPU_DESCRIPTOR_HANDLE m_vsfShmSrv{};
  bool m_vsfShmSrvReady = false;

  // ── iter-461 render thread (SR_PM4_RENDER_THREAD) ──────────────────────
  // The command processor hands each frame's batch set to a render thread at
  // the swap and keeps executing the next indirect buffers: frame =
  // max(ib, swap) instead of ib + swap. Ownership - CP thread: the engine,
  // the draw queue, the drain (translation, PSO cache, the drain third of
  // the arenas, shm collect / validate); render thread: the frame list, the
  // texture / RT caches, EDRAM, resolves, present, the fence wait, the
  // presented notification. Shared under m_shmMu: the shared-memory page
  // tables. Frames in flight: N on the GPU, N+1 recording, N+2 draining -
  // the arenas' three thirds; a third is reused only once its frame's fence
  // completed (m_thirdFence).
  struct Pm4Frame {
    std::vector<DrawBatch> batches;
    std::vector<ShmPend> pending;
    uint32_t third = 0;       // the arena third the set was drained into
    uint32_t epochFirst = 0;  // the set's first drain epoch
#if SR_ORACLE
    std::vector<ShmPend> prepass;  // iter-465 perf2: the CP's texture prepass copies
    int8_t resArm = -1;            // mem.resources frozen-pass A/B: the set's decider (gfx/resource_ab.hpp)
#endif
    uint64_t drainNs = 0;
    uint32_t fbPhys = 0, width = 0, height = 0, pitchTexels = 0;
    bool tiled = false;
    uint64_t zpdSeq = 0;  // p2.zpd: non-zero = a PARTIAL set (strict covering fence): no scan-out / present
    zpd::FrameStamp zpdStamp{};  // immutable diagnostic identity of this frame, carried across threads
  };
  static bool pm4RenderThreadGate();   // env + live + RT cache + no live trace
  void pm4RenderThreadStart();
  void pm4RenderThreadMain(uint8_t* guestBase, size_t guestSize);
#if SR_ORACLE
  std::vector<ShmPend> m_rtFramePrepass;  // render thread: the taken frame's prepass copies
#endif
  void pm4RenderThreadStop();
  // zpdSeqOut (p2.zpd): the set goes as a partial frame; *zpdSeqOut = its sequence (m_zpdPartialDone reaches it).
  void pm4SubmitFrame(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t pitchTexels,
                      bool tiled, uint64_t* zpdSeqOut = nullptr);
  // The swap body (either thread): frame list open, render, scan-out,
  // present, the fence, the notification. zpdPartial (p2.zpd): pm4ZpdPartialFrame instead.
  void pm4RenderFrame(std::vector<DrawBatch>& batches, std::vector<ShmPend>& pending,
                      uint32_t third, uint32_t fbPhys, uint32_t width, uint32_t height,
                      uint32_t pitchTexels, bool tiled, bool zpdPartial = false,
                      const zpd::FrameStamp& stamp = {});
  // ── p2.zpd (SR_NATIVE_ZPD, gfx/native_zpd.hpp, docs/NATIVE_ROADMAP.md P2.12) ──
  // The partial frame (strict covering fence): the set drained so far rendered on the frame list (no scan-out,
  // present, frame-epoch bump or swapchain fence), then the GPU drained (waitForGpu) and the queries collected.
  void pm4ZpdPartialFrame(std::vector<DrawBatch>& batches, std::vector<ShmPend>& pending, uint32_t third);
  bool pm4FramesReady();                    // device + PM4 + translator ready (pm4PresentFrame's admission)
  void zpdDrainMarker(uint8_t op, uint32_t id);  // the drain: a marker DrawBatch at stream position
  uint32_t m_zpdDrainQuery = 0;  // the drain's open query (its BEGIN marker drained, its END not yet): 0 = none
  bool zpdEnsure();                         // the query heap + persistently mapped readback (lazy, once)
  void zpdMarker(const DrawBatch& b);       // render loop: a BEGIN / END marker batch (opens no host query)
  // Render loop, immediately around a GUEST draw call while a query is open (the only work a query counts: helper
  // passes, transfers, resolves and clears run outside, and no segment spans a list close): BeginQuery before the
  // draw, EndQuery + ResolveQueryData right after it. zpdDrawBegin returns the segment (zpd::kNoSeg = none).
  uint32_t zpdDrawBegin(const DrawBatch& b);
  void zpdDrawEnd(uint32_t seg);
  // The probe draws (DrawBatch::zpdRefPso / zpdRasterPso) right after the measured draw: kind = zpd::kRef /
  // kRaster; zpd::kNoSeg = not probed (the probe off, this kind already taken by the query's first draw, no heap).
  uint32_t zpdProbeBegin(uint8_t kind);
  // [NATIVE_ZPD] draw #n: the measured draw's state (the first kZpdDrawLines query draws), for the 0-sample hunt.
  void zpdLogDraw(const DrawBatch& b, const D3D12_RECT& scissor, bool depthBound, bool probed, size_t bi);
  static constexpr uint32_t kZpdDrawLines = 16;
  uint32_t m_zpdDrawLines = 0;
  // ... and where its depth came from (zpd5: the depth test killed ~91 %): the moves its depth claim produced (the
  // previous owners of the tiles it tests, " none" = its own surface already held them), and the last draw this
  // frame that rewrote a whole depth binding (an XDK clear: key, batch, uniform-zero) - before or after the HUD's
  // depth clear.
  char m_zpdDepthMoves[200] = {};
  uint32_t m_zpdClearKey = 0;
  uint64_t m_zpdDrawSerial = 0, m_zpdClearSerial = 0, m_zpdClearEpoch = UINT64_MAX;
  bool m_zpdClearZero = false;
  void zpdRecord(bool begin, uint32_t seg);  // the query commands on the list (the difftest seam logs them)
  void zpdCollectNow(bool gpuIdle);         // stamp the closed segments and write the completed queries
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_zpdHeap;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_zpdReadback;
  const uint64_t* m_zpdReadbackPtr = nullptr;
  bool m_zpdInitFailed = false;
  uint32_t m_zpdOpenId = 0;                 // the list owner's open query (BEGIN marker passed, END not yet)
  uint64_t m_zpdPartialReq = 0, m_zpdPartialDone = 0;     // under m_rtMu (render-thread partial handshake)
  uint32_t m_zpdFlushTimeoutMs = 5000;      // the render thread's partial frame, then the strict fallback
#if defined(SR_DIFFTEST)
  ZpdTestSeam m_zpdTest;
  void zpdTestFrame(std::vector<DrawBatch>& batches, bool partial);
#endif
  void pm4WaitThirdFree(uint32_t third);  // CP: the third's last frame completed on the GPU
  bool m_rtOn = false;                    // the thread runs (set before it starts)
  std::thread m_rtThread;
  std::mutex m_rtMu;
  std::condition_variable m_rtCv;
  Pm4Frame m_rtQueued;                    // depth-1 queue
  bool m_rtHasFrame = false, m_rtBusy = false, m_rtStop = false;
  std::vector<DrawBatch> m_rtFreeBatches; // recycled vectors (SR_BATCH_KEEP=0: the old one-slot recycle)
  std::vector<ShmPend> m_rtFreePending;
  // step 6 I5 (joint plan I5 / I8(b), SR_BATCH_KEEP, default ON): every recycled batch vector keeps its capacity. The
  // old one-slot recycle dropped a vector whenever the render thread returned one while the slot was still full, and
  // handed the CP a moved-from (capacity 0) vector whenever the render thread had not returned one yet - the set then
  // regrew from nothing (gate 3: 4,578 regrowths / 2.96 GB per reload on the PC, 9,670 / 6.2 GB on the Ally, the 64 KB -
  // 1 MB band). Now: a small pool (m_rtMu) the render thread returns to and the CP takes the largest from; when it has
  // none, the CP reserves its set to the high-water mark once (one allocation, no regrowth chain).
  static constexpr size_t kBatchPool = 3;
  std::vector<std::vector<DrawBatch>> m_rtBatchPool;   // guarded by m_rtMu; reserved to kBatchPool once
  size_t m_batchHighWater = 0;                         // CP: the largest set handed off
  struct BatchKeepCounts {                             // guarded by m_rtMu
    uint64_t handoffs = 0, poolTakes = 0, reserves = 0, returns = 0, drops = 0;
  } m_batchKeep;
  static bool batchKeepOn();
  void batchPoolTakeLocked(std::vector<DrawBatch>& out);     // CP, under m_rtMu
  void batchPoolReturnLocked(std::vector<DrawBatch>& in);    // either thread, under m_rtMu
  std::atomic<uint64_t> m_thirdFence[3] = {};
  uint64_t m_lastSignaledFence = 0;       // moveToNextFrame's signal (render thread)
  HANDLE m_cpFenceEvent = nullptr;
  uint32_t m_setEpochFirst = 0;           // CP: the open set's first drain epoch
  uint64_t m_rtDrainNsShown = 0;          // render: the frame's drain cost (reported)
  std::atomic<uint64_t> m_rtWaitThirdUs{0}, m_rtWaitSubmitUs{0};  // CP stalls (census)
  void pm4ShmRangeWrittenByGpu(uint32_t phys, uint32_t len);
#if SR_ORACLE
  void pm4ShmRangeWrittenByGpuUnlocked(uint32_t phys, uint32_t len);  // iter-466 hfr: hashes outside m_shmMu
  void pm4ShmRebaseHash(uint32_t phys, uint32_t len);   // iter-466 hfr: pre-hash before m_shmMu
  void pm4ShmRebaseDrop();
  static bool pm4ShmRebaseUnlockedEnabled();            // SR_SHM_REBASE_UNLOCKED (default ON)
  void pm4ShmReadback(uint32_t phys, uint32_t len);
#endif
  // NATIVE N4a (SR_NATIVE_RESOLVE=1): the resolve reads the source host RT directly
  // (gfx/native_resolve.inc); false = not taken (depth, scaled, failed) -> EDRAM path.
  // The surfaces owning the span: a (ri.srcKey or a same-base/pitch/bpp surface), and for a
  // mixed span b (one colour + one depth) with the absolute tile ranges b owns.
  struct Pm4NatOwners {
    edram::RtKey a, b;
    uint32_t nRanges = 0;
    uint32_t ranges[8] = {};
  };
  bool pm4NativeResolve(const edram::ResolveInfo& ri, const edram::Regs& regs, const Pm4NatOwners& owners,
                        uint32_t& extStart, uint32_t& extLen);
  bool uploadNativeOwnerPlan(const native_owner::Plan& plan, D3D12_GPU_VIRTUAL_ADDRESS& address);
  // FNV-1a of a shared-memory range (GPU sync; diagnostics only). ok (Astra re-review 1): whether the range was read
  // back at all - a failed Map returned 0 like an empty range, and two of them compared equal.
  uint64_t pm4ShmHash(uint32_t start, uint32_t len, uint32_t* nonZero = nullptr, bool* ok = nullptr);
  // NATIVE present: a resolve that did not refresh a front-buffer copy drops overlapping ones.
  void pm4NativeFrontInvalidate(uint32_t start, uint32_t len);
#if SR_ORACLE  // C1 stage 3: the EDRAM-path resolve (Xenia's resolve CS over the EDRAM buffer) is the oracle unit's
  // SR_NATIVE_RESOLVE_HASH=1 (diagnostic): read each resolve's destination back and log its hash.
  void pm4ResolveHashDebug(const edram::ResolveInfo& ri, uint32_t extStart, uint32_t extLen, bool native);
  bool pm4ShmResolve(const edram::ResolveInfo& ri, const edram::Regs& regs, uint32_t& extStart,
                     uint32_t& extLen);
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shmResolveRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shmResolvePso[9];
#endif
  Microsoft::WRL::ComPtr<ID3D12Resource> m_shm, m_shmUpload, m_shmReadback;
  uint8_t* m_shmUploadPtr = nullptr;
  uint64_t m_shmUploadBytes = 0, m_shmResolves = 0;
  uint32_t m_shmEpoch = 1;
  D3D12_RESOURCE_STATES m_shmState = D3D12_RESOURCE_STATE_COMMON;
  bool m_shmReady = false, m_shmFailed = false, m_shmUavDirty = false;
#if SR_ORACLE
  double m_shmMsReadback = 0;
  std::vector<uint64_t> m_shmHash8, m_shmHashA;
#endif
  std::vector<uint32_t> m_shmEpochOf;
  // pm4ResourceRequest's same-drain memo: a range found current at drain epoch
  // `stamp` while the registry's note sequence held (under m_shmMu).
  struct ResourceRangeMemo { uint32_t phys = 0, len = 0, stamp = 0; uint64_t sequence = 0; };
  static constexpr uint32_t kResourceRangeMemoLog2 = 12;
  std::vector<ResourceRangeMemo> m_resourceRangeMemo;
  uint64_t m_resourcePatchCopies = 0;  // exact CPU-write patch copies recorded or queued
  uint64_t m_resourceRangeMemoHits = 0;  // requests the same-drain range memo answered without a walk
  // step 6 I4: the drain's own request counters (incremented and read on the drain thread only: [CP_RES_REQ]).
  struct CpResReqCounters {
    uint64_t drains = 0, hitsNoLock = 0, lockedRanges = 0, holds = 0, checked = 0, bad = 0, raced = 0;
  } m_cpResReq;
#if SR_ORACLE
  // SR_CP_RES_REQ_CHECK: hits whose page would upload while the blocks still hold - decided a drain later (a note's
  // block sequence lands after its generation). Drain thread only; empty without the CHECK.
  struct CpResReqSuspect { uint32_t first = 0, last = 0, page = 0; uint64_t sequence = 0, drain = 0; };
  std::vector<CpResReqSuspect> m_cpResReqSuspects;
#endif
  // perf parity round 2: the previous pm4 draw's texture slots as the draw path requested them, and the state they
  // were requested in (registry sequence, patch arming count, the stamps); a slot equal to it is not asked again.
  uint32_t m_resourceTexSlot[16][6] = {};
  uint16_t m_resourceTexSlotValid = 0;
  uint64_t m_resourceTexSlotSeq = 0, m_resourceTexSlotArmed = 0;
  uint32_t m_resourceTexSlotEpoch = 0, m_resourceTexSlotFrameStart = 0;
  bool m_resourceTexSlotRt = false;
  uint64_t m_resourceTexSlotSkips = 0, m_resourceTexSlotAsks = 0;
  uint64_t m_resourcePatchScaledDrops = 0;  // patched pages whose scaled resolve copy was dropped
  void resourcePatchScaled(uint32_t page);
#if SR_ORACLE
  uint32_t m_shmFramePagesReq = 0, m_shmFramePagesHashed = 0;  // iter-460 perf census
  // iter-466 cp lane: pages skipped as write-tracked clean; pages a drain re-validated that an
  // earlier drain of the same frame had validated.
  uint32_t m_shmFramePagesClean = 0, m_shmFramePagesRepeat = 0;
  double m_shmFrameMsHash = 0, m_shmFrameMsLookup = 0;
#endif
  std::vector<uint8_t> m_shmGpuWritten;
#if SR_ORACLE
  std::vector<uint8_t> m_shmSrcA;  // iter-460: page last uploaded from the 0xA window
  // iter-466 cp lane (SR_SHM_WT_REBASE): the write tracker's protection generation (gww::protGen)
  // under which m_shmHash8 / m_shmHashA were last taken; while protGen still returns it the hash
  // is the page's content and the post-resolve rebaseline need not hash it again. 0 = unknown.
  std::vector<uint32_t> m_shmFresh8, m_shmFreshA;
  uint64_t m_shmWtRebaseKept = 0;  // rebaseline pages kept (not re-hashed)
  bool shmWtRebaseOn() const;
  uint64_t m_shmOwnerA = 0, m_shmOwner8 = 0;
  uint64_t m_shmClobbers = 0;  // iter-460: CPU-copy uploads over GPU-written pages
  uint64_t m_shmConflicts = 0;  // iter-460: uploads where 0x8 and 0xA hold different data
#endif
  // iter-460 S4: GPU texture loads (Xenia texture_load_* from shared memory).
  uint32_t pm4ShmVersion(uint32_t phys, uint32_t len);
 public:
  // A CPU reader is about to read guest [phys, +len): GPU-written pages first.
#if SR_ORACLE
  void pm4ShmSyncToGuest(uint32_t phys, uint32_t len);
#endif
  // An immutable copy for a renderer/replay CPU consumer. Never writes guest RAM.
  void resourceCpuSnapshot(uint32_t phys, uint32_t len, std::vector<uint8_t>& out);
  void resourcePrivateSnapshot(uint32_t phys, uint32_t len, std::vector<uint8_t>& out);
#if SR_ORACLE
  // Opt-in playback contract; native resource ownership is unchanged.
  bool replayOrderedRestores() const;
  void replayRestoredRange(uint32_t phys, uint32_t len);
  void replayPrepareTail();
  // The replay oracle poisons both guest RAM and already captured uploads.
  // A missing resolve must never expose the trace's final framebuffer pixels.
  void replayPoisonResource(uint32_t phys, uint32_t len);
#endif
 private:
#if SR_ORACLE
  uint64_t m_shmSyncs = 0;
#endif
  bool pm4ShmLoadTexture(ID3D12Resource* tex, bool fromSR, const ShmTexDesc& d);
  // iter-460 perf (SR_PM4_SHM_ARRAY, default ON): a stacked / cube fetch is
  // loaded layer by layer from the shared memory straight into the array
  // resource (texture_load_* into subresource i), versioned by the layers'
  // page stamps - no CPU readback, hash, untile or fence per bind. 0 = not
  // loadable this way (the caller falls back to the CPU layer path).
  uint32_t pm4ShmLoadArray(const uint32_t* fetch6, bool cube);
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shmLoadRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shmLoadPso[7];
  Microsoft::WRL::ComPtr<ID3D12Resource> m_shmScratch;
  uint64_t m_shmScratchBytes = 0, m_shmTexLoads = 0, m_shmArrayLoads = 0;
  D3D12_RESOURCE_STATES m_shmScratchState = D3D12_RESOURCE_STATE_COMMON;
  std::vector<uint32_t> m_shmStamp;
  uint32_t m_shmStampClock = 0;
  // Scan-out straight from the shared memory (SR_PM4_SHMEM_PRESENT, default ON
  // under SR_PM4_SHMEM): false = not done, the CPU scan-out runs.
  bool pm4ShmPresent(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t pitchTexels,
                     bool tiled, ID3D12Resource* backbuffer);
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shmPresentRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shmPresentPso;
  bool m_shmPresentFailed = false;
  // ── iter-464 resscale: scaled resolve memory (Xenia d3d12_texture_cache.cc
  // scaled_resolve_buffers_, texture_cache.cc scaled_resolve_pages_) ──────────
  // A reserved buffer of 512 MiB * scale_x * scale_y: guest physical byte A of a
  // resolve destination lives at about A * area (texture_address.xesli groups).
  // 16 MiB heaps are mapped on first use. Pages a scaled resolve wrote are
  // marked; a CPU write (a changed page hash at validation) clears the mark.
 public:
  bool srAnyScaled(uint32_t phys, uint32_t len) const;  // any page of the range scaled
  // Xenia TextureKey.scaled_resolve for a texture fetch constant (6 dwords).
  bool texResolutionScaled(const uint32_t* fetch6, uint32_t layers = 1) const;
 private:
  bool srInit();
  bool srEnsure(uint32_t phys, uint32_t len);           // map the heaps under [phys, +len)
  void srState(D3D12_RESOURCE_STATES s);
  void srMark(uint32_t phys, uint32_t len);
  void srClearPages(uint32_t page, uint32_t count);
  Microsoft::WRL::ComPtr<ID3D12Resource> m_srBuf;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> m_srHeaps;  // per 16 MiB of the scaled space
  uint64_t m_srBytes = 0, m_srMappedBytes = 0;
  D3D12_RESOURCE_STATES m_srState = D3D12_RESOURCE_STATE_COMMON;
  bool m_srReady = false, m_srFailed = false, m_srUavDirty = false;
  std::vector<uint8_t> m_srPages;     // per 4 KiB guest page: written by a scaled resolve
  std::vector<uint16_t> m_srBlock;    // per 64 pages: how many are marked (fast range scan)
#if SR_ORACLE  // C1 stage 3: the scaled EDRAM-path resolve is the oracle unit's
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_srResolveRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_srResolvePso[9];
#endif
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_srLoadPso[7];
  uint64_t m_srResolves = 0, m_srTexLoads = 0;
  // The presented frame at the internal resolution (scan-out + gamma ramp), then
  // fitted to the window (pm4ShmPresent). Recreated when the size changes.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_outTex;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_outRtvHeap;
  uint32_t m_outW = 0, m_outH = 0;
  D3D12_RESOURCE_STATES m_outState = D3D12_RESOURCE_STATE_COMMON;
  int32_t m_outSrv = -1;  // slot in the xlat heap's RT SRV band
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_fitRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_outPso, m_fitPso;
  bool pm4OutputEnsure(uint32_t w, uint32_t h);
  // ── iter-466 gfx: THE PRESENT CHAIN (gfx/d3d12_present_chain.cpp, docs/GRAPHICS_SETTINGS.md):
  // scan-out -> post -> colour -> scale -> sharpen -> fit, the host stages between pass 1 and the
  // swap chain. A stage at its default records nothing (byte-identical presents). ─────────────
 public:
  enum class PresentStage : uint8_t { Post = 0, Colour, Scale, Sharpen, Fit, Count };
  static const char* presentStageName(PresentStage s);
  // An image a stage reads: an SRV in the RT SRV band, in a shader-read state. tgt = the chain
  // target that holds it (-1 = the scan-out's output texture).
  struct PresentSurface {
    ID3D12Resource* res = nullptr;
    int32_t srv = -1;
    uint32_t w = 0, h = 0;
    int tgt = -1;
  };
 private:
  // Chain targets by stage: 0-1 post, 2 colour, 3-4 scale / sharpen, 5 spare.
  static constexpr int kChainTargets = 6;
  static constexpr uint32_t kChainConsts = 16;   // root constants b0 of the chain root signature
  struct ChainTarget {
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    int32_t srv = -1, uav = -1;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  };
  ChainTarget m_chainTgt[kChainTargets];
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_chainRs;
  Microsoft::WRL::ComPtr<ID3DBlob> m_chainVs;
  std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_chainPsos;
  bool m_chainFailed = false;
  uint32_t m_chainLastRan = ~0u;
  // post stage: Xenia's FXAA compute pipelines (root signature b0 / u0 / t0 / s0)
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_fxaaRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_fxaaPso, m_fxaaExtremePso;
  bool m_fxaaFailed = false;
  // iter-466 gfx: the xlat sampler table in blocks of 16 (d3d12_backend.cpp xlatSamplerTable):
  // block 0 = the game's own filtering, 1..kSampBlocks-1 = a ring for the anisotropic / LOD bias
  // options. Every draw binds xlatSamplerTable().
  static constexpr uint32_t kSampBlocks = 8;
  UINT m_sampInc = 0;
  uint64_t m_sampVersion = 0;
  uint32_t m_sampBlock = 0, m_sampRing = 0;
  void xlatWriteSamplerBlock(uint32_t block, uint32_t aniso, float lodBias);
  D3D12_GPU_DESCRIPTOR_HANDLE xlatSamplerTable();
  // Step-4 lane C round 2 (SR_XLAT_SAMPLER_V2): the volume samplers of table indices 16+ (translated_samplers.hpp),
  // in first-use order; a new one bumps m_volSamplerGen (the native path's sampler version follows it; the legacy
  // heap gets the new entry written into block 0 and the block in use - an index no recorded draw has used yet).
  std::vector<VolumeSamplerParams> m_volSamplers;
  std::unordered_map<uint32_t, uint32_t> m_volSamplerIndex;  // VolumeSamplerParams::key() -> table index
  uint64_t m_volSamplerGen = 0;
  // Round 3 (SR_XLAT_SAMPLER_V3): per entry, the frame fence value of the frame that last bound it; a full table
  // reclaims the least recently used entry whose last frame the GPU has finished (rewritten in place: no list in
  // flight reads that index any more), instead of the legacy-recipe fallback.
  std::vector<uint64_t> m_volSamplerLastUse;
  uint64_t m_volSamplerReclaims = 0, m_volSamplerFallbacks = 0;
  uint32_t m_sampAniso = 0;     // the options of block m_sampBlock (m_sampBlock 0: none)
  float m_sampLodBias = 0.0f;
  uint32_t xlatSamplerIndex(const uint32_t* fetch);
  uint32_t xlatVolumeSamplerIndex(const uint32_t* fetch);
  int m_chainInternalTgt = -1;   // the last internal-resolution result (pm4ReadOutput); -1 = m_outTex
  bool pm4ChainInit();
  // iter-466 res4k: a chain PSO from precompiled pixel-shader bytecode (Xenia guest-output effects).
  ID3D12PipelineState* pm4ChainPsoBytecode(const char* key, const void* ps, size_t n);
  ID3D12PipelineState* pm4ChainPso(const char* key, const char* hlsl, size_t len, const char* entry,
                                   DXGI_FORMAT rtvFormat);
  bool pm4ChainTargetEnsure(int idx, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav);
  void pm4ChainToRead(ChainTarget& t);
  bool pm4ChainPass(ID3D12PipelineState* pso, const PresentSurface& in, int idx, uint32_t w, uint32_t h,
                    DXGI_FORMAT fmt, const void* consts, uint32_t nConsts, PresentSurface& out);
  bool pm4StagePost(const gfxopt::Settings& s, const PresentSurface& in, PresentSurface& out);
  bool pm4StageColour(const gfxopt::Settings& s, const PresentSurface& in, PresentSurface& out);
  bool pm4StageScale(const gfxopt::Settings& s, const PresentSurface& in, uint32_t fitW, uint32_t fitH,
                     PresentSurface& out);
  bool pm4StageSharpen(const gfxopt::Settings& s, const PresentSurface& in, PresentSurface& out);
  PresentSurface pm4ChainInternal(const gfxopt::Settings& s, PresentSurface cur, uint32_t& ran);
  void pm4PresentChain(ID3D12Resource* backbuffer);
  // SR_PRESENT_CHAIN_TIME=1: per-stage GPU timestamps (d3d12_present_chain.cpp)
  static constexpr uint32_t kCtStamps = 6;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_ctHeap;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_ctReadback;
  uint64_t m_ctFreq = 0;
  bool m_ctFailed = false;
  bool m_ctValid[8] = {};
  double m_ctAcc[kCtStamps - 1] = {};
  uint32_t m_ctN = 0;
  void pm4ChainStamp(uint32_t i);
  void pm4ChainTimeFrame();
  // fit (pass 2): `src` into the backbuffer, letterboxed (1:1 / bilinear enlarge / area shrink).
  void pm4FitToBackbuffer(ID3D12Resource* backbuffer, const PresentSurface& src);
  bool pm4ShmScanOut(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t pitchTexels,
                     bool tiled, DXGI_FORMAT backbufferFmt);
 public:
  // The replay at a scale: scan the front buffer out into the output texture
  // (opens, executes and closes its own list).
  bool pm4ScanOutForDump(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t pitchTexels,
                         bool tiled);
  // The last presented internal-resolution frame as BGRA rows (replay frame dump
  // at a scale; diagnostics). False when there is none. Needs no open list.
  bool pm4ReadOutput(std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h);
 private:
  // Decoded-VB reuse for the open batch set (drainDrawQueue).
  std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> m_xlatVbDedup;
  bool pm4Init();
  Pm4HostRt* pm4Rt(edram::RtKey k);
  void pm4Transition(Pm4HostRt& rt, D3D12_RESOURCE_STATES s);
  // Development HDR observations use existing successful submission tickets.
  // Recording copies never flushes, signals or waits on the queue.
  struct HdrProbeImage {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint32_t slot = 0, owner = 0, key = 0, mapping = 0, resourceFormat = 0;
    uint64_t cacheKey = 0, rowBytes = 0, rawOffset = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
  };
  struct HdrProbeCapture {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    NativeBufferSlice constants;
    std::vector<HdrProbeImage> images;
    uint64_t ticket = 0, frame = 0, vsHash = 0, psHash = 0, readbackBytes = 0, rawBytes = 0;
    uint64_t vsOffset = 0, psOffset = 0, sharedOffset = 0;
    uint32_t vsBytes = 0, psBytes = 0;
    uint32_t freeze = 0, sequence = 0, sameHold = 0, stage = 0;
    size_t batch = 0;
    double held = 0;
    std::string path;
  };
  std::deque<HdrProbeCapture> m_hdrProbePending;
  uint32_t m_hdrProbeCounts[19][6]{};  // render-thread only, 48 captures/stage/hold
  void hdrFreezeProbe(const DrawBatch& b, Pm4HostRt& output, size_t batch,
                      const D3D12_RECT& scissor, const TranslatedConstantSnapshot& constants);
  void hdrProbeSubmitted(uint64_t ticket);
  void hdrProbeCollect();
  void hdrProbeDiscardUnsubmitted();
  // Close + execute + wait + reopen the frame list; restores descriptor heaps.
  void pm4Flush();
  // A move (C1 stage 3, docs/RELEASE_COMPOSITION.md section 8): tiles [start, end) of the source surface's pixels
  // into the destination surface - the alias geometry of gfx/surface_key.hpp rangeRects; src empty = a load of tiles
  // nobody owns (SR_NB_ZERO_INIT: a host clear).
  struct Pm4Move { edram::RtKey src, dst; uint32_t start = 0, end = 0; };
  // Claim [start, start+len) for k: moves from the other owners, loads of the unowned tiles - decided by the surface
  // table (pm4ClaimTable, gfx/nb_surfaces.hpp); the development build's oracle unit (edramClaim) runs the tile map beside it.
  void pm4Claim(edram::RtKey k, uint32_t start, uint32_t len, std::vector<Pm4Move>& moves);
  void pm4ClaimTable(edram::RtKey k, uint32_t start, uint32_t end, std::vector<Pm4Move>& out);
  // The owners of a resolve's source span (whole tile rows covering its rectangle), from the surface table.
  void pm4SpanOwners(edram::RtKey k, const edram::ResolveInfo& ri, std::vector<edram::Transfer>& out);
  // Execute a claim's moves: the direct transfer passes (pm4GpuLoad); the development build's oracle unit takes what they
  // cannot (the EDRAM buffer / mirror), the release configuration stops on such a move ([SURFACE_MOVE_BAD]).
  void pm4MoveTiles(const std::vector<Pm4Move>& moves);
  // iter-467 native-b B1c (SR_NB_CLEAR_PROP): moves whose source tiles hold an XDK clear's uniform value become
  // host clears of the destination (gfx/d3d12_frame_render.cpp); returns the tiles no longer moved.
  uint32_t nbPropagateClears(std::vector<Pm4Move>& moves, size_t bi);
  uint32_t nbZeroLoads(std::vector<Pm4Move>& moves, size_t bi);  // SR_NB_ZERO_INIT: loads of never-claimed tiles
#if SR_ORACLE
  void nbPropVerify(const std::vector<Pm4Move>& moves, size_t bi);  // SR_NB_CLEAR_PROP_VERIFY (a CHECK)
  // ── The EDRAM word model (gfx/d3d12_edram_model.cpp: the development build's oracle unit, EMULATION_SOURCES) ──
  // The 10 MiB EDRAM lives in a GPU buffer (SR_PM4_EDRAM_GPU; =0: the CPU mirror alone) with a CPU mirror refreshed
  // per resolve span (edramSyncMirror). A move the direct passes do not take stores its source into the buffer
  // (compute: host RT -> guest words, every sample) and loads the destination from it (quad: words -> host RT); the
  // EDRAM-path resolve reads the buffer (Xenia's resolve CS, pm4ShmResolve) or the mirror (edram::resolveToGuest).
  // The tile ownership map (edram::Ownership) is a static of the unit; the trace snapshot and SR_PM4_EDRAM_DUMP read
  // the mirror.
  std::vector<uint8_t> m_pm4Edram;  // CPU EDRAM mirror, edram::kEdramBytes
  Microsoft::WRL::ComPtr<ID3D12Resource> m_pm4EdramGpu, m_pm4DepthScratch;
  D3D12_RESOURCE_STATES m_pm4EdramGpuState = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES m_pm4DepthScratchState = D3D12_RESOURCE_STATE_COMMON;
  uint64_t m_pm4DepthScratchBytes = 0;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pm4StoreRs, m_pm4LoadRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pm4StoreColorPso, m_pm4StoreDepthPso;
  Microsoft::WRL::ComPtr<ID3DBlob> m_pm4LoadPsColor, m_pm4LoadPsDepth, m_pm4LoadPsStencil;
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_pm4LoadPsos;
  bool m_edramGpuReady = false, m_edramGpuFailed = false, m_pm4GpuUploadPending = false;
  bool m_natVerifyForce = false;  // natResolveDifftest: every native resolve verified against the model
  void edramModelInit();  // the mirror and the map (pm4Init)
  bool edramGpuInit();    // the buffer, the store / load root signatures, shaders and PSOs (lazy; needs pm4GpuInit)
  void pm4GpuEdramState(D3D12_RESOURCE_STATES s);
  void edramStoreTiles(const std::vector<Pm4Move>& moves);  // the sources -> the buffer (GPU) or the mirror (CPU)
  void edramLoadTiles(const std::vector<Pm4Move>& moves);   // the mirror -> the destinations (the CPU path)
  void edramGpuStore(const std::vector<Pm4Move>& moves);
  void edramGpuUploadPending();  // a restored snapshot reaches the buffer before a transfer reads it
  void edramGpuLoadMove(const Pm4Move& mv, Pm4HostRt& rt, const edram::Rect* rects, int n, bool& bound);
  ID3D12PipelineState* pm4GpuLoadPso(DXGI_FORMAT fmt, uint32_t kind, uint32_t bit);
  // Copy tiles [start, start+len) (mod 2048) of the GPU EDRAM into the CPU mirror.
  void edramSyncMirror(uint32_t start, uint32_t len);
  void edramClaim(edram::RtKey k, uint32_t start, uint32_t end, std::vector<Pm4Move>& moves);
  void edramSpanOwners(edram::RtKey k, const edram::ResolveInfo& ri, std::vector<edram::Transfer>& out);
  // The EDRAM path of a resolve (and SR_NATIVE_RESOLVE_DUAL / _VERIFY beside the native one).
  void edramResolve(const edram::ResolveInfo& ri, const edram::Regs& regs, size_t bi, bool natDone, bool nbTex,
                    edram::RtKey natOwner, bool viaOwner, uint32_t natStart, uint32_t natLen, char& nbPath,
                    uint32_t& nbExtS, uint32_t& nbExtL);
  bool edramDumpAfterDraw(const DrawBatch& db);  // SR_PM4_EDRAM_DUMP; true = the store's flush reset the list
  // SR_PM4_ORACLE: diff one resolve's guest bytes against Xenia's dump of it.
  void pm4OracleCompare(const edram::ResolveInfo& ri, const uint8_t* dest, size_t bi);
#endif
  // ── The frame renderer's GPU side: the render-target SRV band and the transfer passes' vertex shader ──
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_pm4Graveyard;  // freed after the next flush
  Microsoft::WRL::ComPtr<ID3DBlob> m_pm4LoadVs;  // VSQuad: the full-RT triangle of every transfer pass
  bool m_pm4GpuReady = false, m_pm4GpuFailed = false;  // the band and the VS exist
  uint32_t m_pm4RtSrvUsed = 0;
  bool pm4GpuInit();
  bool pm4XferDirectOk(const Pm4Move& mv);             // iter-466 res4k SR_PM4_XFER_DIRECT
  ID3D12PipelineState* pm4XferDirectPso(DXGI_FORMAT fmt, bool masked = false);
  // iter-467 native-b B1b (SR_NB_COVER_FILL): the scratch stencil a draw marks its coverage in, and the moves run
  // after the draw into the pixels it did not write (stencil 0).
  struct NbFillScratch {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  };
  std::unordered_map<uint64_t, NbFillScratch> m_nbFillScratch;
  ID3D12PipelineState* m_pm4MarkPso = nullptr;  // pm4PrepareDraw's B1b variant of the PSO it returns
  bool m_pm4MarkWanted = false;  // step 6 R6 fix round 4: pm4PrepareDraw's B1b branch applied (a null m_pm4MarkPso = a failed acquisition)
  NbFillScratch* nbFillScratch(uint32_t w, uint32_t h);
  void nbFillLoad(const std::vector<Pm4Move>& moves, D3D12_CPU_DESCRIPTOR_HANDLE dsv);
  // iter-466 res4k round 2 (SR_PM4_XFER_DEPTH): depth sources / destinations in direct transfers
  void pm4DepthSrvs(Pm4HostRt& rt, uint32_t depthFmt);
  ID3D12PipelineState* pm4XferDirectDepthPso(DXGI_FORMAT dsvFmt, uint32_t kind, uint32_t bit);
  bool pm4XferDwEnsure(uint32_t w, uint32_t h);
  bool pm4XferPredEnsure();  // iter-466 res4k: SR_XFER_DEPTH_PRED
  bool pm4TexLoadDirectInit();  // iter-466 res4k round 2
  void pm4GpuLoad(const std::vector<Pm4Move>& moves);  // the moves' direct passes (the buffer loads in the dev build)
  // The xlat heap's RT SRV band (defined next to kTex2DSlots in d3d12_backend.cpp).
  D3D12_CPU_DESCRIPTOR_HANDLE pm4RtSrvCpu(uint32_t slot) const;
  D3D12_GPU_DESCRIPTOR_HANDLE pm4RtSrvGpu(uint32_t slot) const;
  // step 4 lane C 2i (SR_PM4_XLAT_HEAPS): bind m_xlatSrvHeap + m_xlatSamplerHeap on m_commandList before a pass
  // binds tables from them (pm4RtSrvGpu) - a translated draw's texture bind sets the native arena's heaps on the
  // same list. Baked ON; no-op only with SR_PM4_XLAT_HEAPS=0 (a dev A/B arm).
  void pm4XlatHeaps();
  uint32_t pm4RtSrvCapacity() const;
  void pm4ExecResolve(const DrawBatch& b, size_t bi);
  void pm4ExecResolveRegs(const edram::Regs& regs, size_t bi);  // iter-467 native-b: the record-driven entry
  void renderPm4Batches(std::vector<DrawBatch>& batches, std::vector<ShmPend>& pending);
  ID3D12PipelineState* getPm4Pso(const Pm4PsoDesc& d);
  // Drain side: register snapshot + bound targets + PSO for one draw. false =
  // the draw rasterizes into nothing (consume it, draw nothing).
  bool pm4PrepareDraw(const GuestDrawCommand& c, bool pointList, bool vsf, bool stripCut,
                      edram::Regs& regs, edram::DrawTargets& t, ID3D12PipelineState*& pso);
  // In-shader vertex fetch staging for one draw: copies every fetch range the
  // VS's own vfetches use into an owned snapshot, writes patched constants and the
  // host index list (Xenia's index remap, strip cuts, fan/quad expansion).
  struct Pm4VsfOut {
    uint64_t fetchCbOff = 0;
    uint64_t ibOffset = 0;
    NativeBufferSlice rawVertices;
    uint32_t indexCount = 0;
    D3D_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool shm = false;  // the vertex data stays in the shared memory
    bool noIb = false;   // identity remap: DrawInstanced from firstVertex
    uint32_t firstVertex = 0;
#if SR_ORACLE
    uint64_t zcFetchHash = 0;   // step 6 I10p check mode: the hash of the fetch block this draw's view must hold
#endif
  };
  bool pm4StageVsf(const GuestDrawCommand& c, const CachedShader& vs, bool stripCut,
                   uint64_t vbEnd, uint64_t cbEnd, Pm4VsfOut& out);
  // iter-466 cp lane (SR_CP_VSF_CBDEDUP): the previous draw's fetch-constant buffer in this set's
  // CB third - an identical one is bound again instead of copied (cleared with the frame set).
  uint32_t m_vsfCbLast[192] = {};
  uint64_t m_vsfCbLastOff = ~0ull;
  // iter-466 cp round 2 (SR_CP_CONST_DEDUP): this set's latest VS / PS float-constant upload.
  uint32_t m_cdVs[1024] = {}, m_cdPs[1024] = {};
  uint32_t m_cdVsUse = 0, m_cdPsUse = 0;
  uint64_t m_cdVsOff = ~0ull, m_cdPsOff = ~0ull, m_cdHits = 0;
  // iter-466 cp round 2 (SR_CP_CONST_TRACK): every constant block's latest values in tracked
  // command order (cvMerge), the last tracked sequence, this set's latest VS / PS upload (its
  // offset and blocks) and the blocks merged since it; counters for the [SHM_WT] line.
  void cvMerge(const GuestDrawCommand& cmd);
  uint32_t m_cvCons[2048] = {};
  uint32_t m_cvSeq = 0;
  uint64_t m_cvUpVsOff = ~0ull, m_cvUpPsOff = ~0ull;
  uint32_t m_cvUpVsMask = 0, m_cvUpPsMask = 0, m_cvUpDirty = 0;
#if SR_ORACLE
  // step 6 drop fix (the coordinator 10-23; Astra I10p r1 P3): the EXACT offsets of the VS / PS blocks that draws dropped
  // after their writes had written in this set (a reuse binds an upload's own offset; reset with the set; the newest
  // kDropOffsets kept, older ones counted as lost), and the set count - the drop-then-reuse census (xlatDropLine).
  static constexpr uint32_t kDropOffsets = 64;
  uint64_t m_dropOff[kDropOffsets] = {};
  uint32_t m_dropN = 0;
  uint64_t m_cpSetSerial = 0;
  bool droppedBlock(uint64_t off) const {
    const uint32_t n = m_dropN < kDropOffsets ? m_dropN : kDropOffsets;
    for (uint32_t i = 0; i < n; ++i)
      if (m_dropOff[i] == off) return true;
    return false;
  }
#endif
  uint64_t m_cvGaps = 0, m_cvMismatch = 0, m_cvUpReuse = 0;
#if SR_ORACLE
  // iter-466 cp lane (SR_CP_IB_CACHE, needs SR_SHM_WRITE_TRACK): this set's converted index lists
  // by (guest index buffer, remap parameters); a hit needs every page of the guest buffer still
  // under the write-protection generation it was converted in (cleared with the frame set).
  struct IbConv {
    uint64_t ibOffset;
    uint32_t indexCount, p0, nPages;
    D3D_PRIMITIVE_TOPOLOGY topo;
    uint32_t gens[8];
  };
  std::unordered_map<uint64_t, IbConv> m_ibConv;
  uint64_t m_ibConvHits = 0, m_ibConvMiss = 0, m_vsfCbDedup = 0;
#else
  uint64_t m_vsfCbDedup = 0;
#endif  // SR_ORACLE
  // Host viewport, g_NdcScaleOffset (c18) and scissor. false = empty viewport.
  bool pm4ViewportScissor(const edram::Regs& regs, const edram::DrawTargets& t,
                          uint64_t psHash, D3D12_VIEWPORT& vp, float ndc[4],
                          D3D12_RECT& scissor);
  static edram::Regs pm4RegsFromDraw(const GuestDrawCommand& c);
  // Shared with the xlat render loop's conventions (d3d12_backend.cpp owns
  // the heap layout constants): root signature + bindless tables, and the
  // render-time texture/sampler index write into the batch's shared CB.
 public:
  // iter-464 PORT FEATURE: the window's client size changed (main thread). The
  // presenter resizes the swapchain before its next present (PM4 path only; the
  // legacy HLE presents copy a fixed 1280x720 frame and keep the initial size).
  void requestResize(int w, int h);
 private:
  void applyPendingResize();
  std::atomic<uint64_t> m_pendingResize{0};  // (w << 32) | h, 0 = none
  // iter-464 resscale: SR_PM4_GPU_TIME=1 (diagnostic, default OFF) - the GPU time of
  // each PM4 frame from D3D12 timestamp queries: a begin/end pair around every
  // command list the frame submits (the frame list + each pm4Flush), summed = busy
  // time; first begin -> last end = span. One [PM4_GPU_TIME] line per 120 frames.
  // iter-466 res4k: per-pass attribution - every stamp carries the category of the
  // GPU work that ended at it (gtMark at each pass boundary; the work between
  // passes is the game's draws). One [PM4_GPU_PASS] line beside [PM4_GPU_TIME].
  enum GtTag : uint8_t { GtListStart = 0, GtDraw, GtStore, GtLoad, GtResolve, GtTexLoad,
                         GtScanOut, GtFit, GtResStore, GtTexLoadScaled, GtTexCopy,
                         GtNbMat,  // iter-467 native-b B2: materializing memory a native target held
                         GtTagCount };
  static constexpr uint32_t kGtPerSlot = 4096;  // stamps per frame (lists + pass marks)
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_gtHeap;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gtReadback;
  uint64_t m_gtFreq = 0;
  uint32_t m_gtN = 0;                     // stamps this frame
  uint32_t m_gtSlotN[kFrameCount] = {};   // stamps resolved into each slot
  uint8_t m_gtTag[kFrameCount][kGtPerSlot] = {};  // the category ending at each stamp
  bool m_gtOpen = false;                  // a list is being stamped
  bool m_gtReady = false, m_gtFailed = false;
  bool gtInit();
  void gtBegin();                         // a list starts
  void gtEnd();                           // a list ends
  void gtMark(GtTag endedWork);           // a pass boundary: the work since the last stamp
  void gtFrameStart();                    // read the slot's last frame, start this one
  void gtFrameEnd();                      // resolve this frame's stamps into the slot
  // ── iter-467 native-b (strand B) B0: the resolve / texture-load / transfer census
  // (SR_NB_CENSUS=<frames>, observe only; gfx/native_targets.inc). Each record's GPU time is
  // the sum of the SR_PM4_GPU_TIME segments that END while it is current (the census turns the
  // stamps on). Records print one frame later, when the slot's stamps are read back.
#if SR_ORACLE
  struct NbRec {
    std::string line;
    double ms = 0;
  };
  struct NbDest {  // a resolve destination, for matching the textures that sample it
    uint64_t frame = 0;
    uint32_t ord = 0, destBase = 0, destPitch = 0, destHeight = 0, destFormat = 0, endian = 0;
    uint32_t srcKey = 0, extStart = 0, extLen = 0;
  };
  std::vector<NbRec> m_nbRecs[kFrameCount];
  uint64_t m_nbRecFrame[kFrameCount] = {};
  uint16_t m_gtItem[kFrameCount][kGtPerSlot] = {};  // the census record current at each stamp
  uint32_t m_nbCur = 0;      // 1-based record (this slot) the next stamps belong to
  bool m_nbFrameOn = false;  // this frame is recorded
  uint32_t m_nbResOrd = 0;   // resolves this frame
  std::vector<NbDest> m_nbDests;      // ring of the last resolve destinations
  std::vector<uint32_t> m_nbPageDest;  // per 4 KiB page: 1 + index in m_nbDests (0 = none)
  uint32_t m_nbDestNext = 0;
  void nbDumpCompletedSlot(uint32_t slot);
#if SR_ORACLE
  struct NbReplaySlot {
    uint32_t count = 0, records = 0;
    uint8_t tag[kGtPerSlot] = {};
    uint16_t item[kGtPerSlot] = {};
    std::shared_ptr<const void> recording;  // immutable NativeDrawRecording payload identity
    uint64_t submission = 0;
    bool sealed = false, attributed = false;
  };
  NbReplaySlot m_nbReplaySlots[kFrameCount];
  bool m_nbReplay = false, m_nbReplayFailed = false, m_nbReplayFinished = false;
  uint64_t m_nbReplayFrames = 0, m_nbReplayDumpedFrames = 0, m_nbReplayDumpedRecords = 0;
  uint64_t m_nbReplayLastDumpedFrame = 0;
  void nbReplaySeal(uint32_t slot);
  void nbReplaySubmitted();
  bool nbReplayFail(const char* reason);
#endif
  void nbFrameStart(uint32_t slot);   // print + clear the slot's records, decide this frame
  void nbAttribute(uint32_t slot, const uint64_t* t, uint32_t n);  // stamp times -> records
  uint32_t nbAdd(const char* fmt, ...);                     // a record of this frame (1-based id)
  void nbAppend(uint32_t id, const char* fmt, ...);
  void nbBegin(uint32_t id) { m_nbCur = id; }
  void nbEnd() { m_nbCur = 0; }
  uint32_t nbResolveBegin(const edram::ResolveInfo& ri);    // record + current; 0 = off
  void nbResolveEnd(uint32_t id, const edram::ResolveInfo& ri, char path, uint32_t extStart,
                    uint32_t extLen);
  uint32_t nbTexLoadBegin(const ShmTexDesc& d, bool direct);
  void nbCpuRead(const char* who, uint32_t phys, uint32_t len, uint32_t pages);
  // SR_NB_RHASH=1 (diagnostic, GPU sync per resolve): the SAMPLER-VISIBLE texels of each resolve's
  // destination rectangle (the destination loaded as its own format by the texture loader, or the native
  // target that holds it), hashed: [NB_RHASH] lines compare a PM4-path run with a native run resolve by
  // resolve (tools/nb_census.py rhash A B).
  void nbResolveHash(const edram::ResolveInfo& ri, char path, uint32_t extStart, uint32_t extLen);
  struct NbTmpTex {
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool uav = false;
  };
  std::vector<NbTmpTex> m_nbTmpTex;
#else
  // Native submission keeps timing, without census records or readback diagnostics.
  void nbFrameStart(uint32_t) {}
  void nbAttribute(uint32_t, const uint64_t*, uint32_t) {}
  void nbBegin(uint32_t) {}
  void nbEnd() {}
  uint32_t nbResolveBegin(const edram::ResolveInfo&) { return 0; }
  void nbResolveEnd(uint32_t, const edram::ResolveInfo&, char, uint32_t, uint32_t) {}
  uint32_t nbTexLoadBegin(const ShmTexDesc&, bool) { return 0; }
  void nbCpuRead(const char*, uint32_t, uint32_t, uint32_t) {}
  void nbResolveHash(const edram::ResolveInfo&, char, uint32_t, uint32_t) {}
#endif
  // ── iter-467 native-b B2 (SR_NB_RESOLVE_TEX, scaled): native targets. A resolve whose destination is
  // exactly a texture's identity writes THAT texture (NAT_TEX kernels) and not the scaled resolve memory;
  // the texture is stamped current, so its bind loads nothing. The memory it did not write is HELD: a
  // page map names the target that holds it, and every reader of that memory (a texture load, the
  // scan-out, a later resolve into it) first MATERIALIZES the held rectangle from the target
  // (the loader run backwards) - so everything else sees exactly what the PM4 path wrote. ──
  struct TexShared;  // below (the texture cache)
  // A native target: a shared 2D texture (kind 0, key = share key) or one layer of a GPU-loaded stacked
  // texture (kind 1, key = the array's cache key, slice = the layer).
  struct NbTgt {
    ID3D12Resource* res = nullptr;
    uint8_t kind = 0;
    uint64_t key = 0;
    uint32_t slice = 0;
    uint32_t base = 0, baseLen = 0, pitch = 0, fmt = 0, endian = 0, w = 0, h = 0, tsx = 1, tsy = 1;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    bool current = false;  // its version is the newest page stamp of its range
  };
  bool nbTgtInfo(uint8_t kind, uint64_t key, uint32_t slice, NbTgt& t);
  void nbTgtStamp(const NbTgt& t);  // current at the range's version now
  struct NbHeld {
    uint8_t kind = 0;
    uint64_t key = 0;
    uint32_t slice = 0;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the union of the resolve rectangles (guest pixels)
    bool on = false;
    // SR_NB_OVERHANG: guest rows [auxY0, auxY1) of a resolve whose rectangle runs past the texture's last
    // row (the 8-row resolve granularity: 184 rows into a 180-row texture) - kept here, not in memory.
    Microsoft::WRL::ComPtr<ID3D12Resource> aux;
    uint32_t auxY0 = 0, auxY1 = 0, auxW = 0, auxH = 0;  // guest rows; host texels
    uint32_t auxX1 = 0;  // guest columns [0, auxX1) (past the texture's width: the luminance 8x8 into 4x4 / 1x1)
    DXGI_FORMAT auxFmt = DXGI_FORMAT_UNKNOWN;
    bool auxOn = false;
    // SR_NB_DEPTH_TARGET: a depth target's stencil bytes (R8_UINT, the texture's host size).
    Microsoft::WRL::ComPtr<ID3D12Resource> stencil;
    uint32_t stencilW = 0, stencilH = 0;
    bool depth = false;
  };
  // Layers of GPU-loaded single-level stacked textures by guest base (the B2 array targets).
  std::unordered_multimap<uint32_t, std::pair<uint64_t, uint32_t>> m_nbArrayByBase;
  std::vector<NbHeld> m_nbHeld;          // index + 1 = the page map's value
  std::vector<uint16_t> m_nbHeldPage;    // per 4 KiB guest page: the holder (0 = the memory is current)
  uint64_t m_nbTexResolves = 0, m_nbMaterializes = 0, m_nbNoTarget = 0, m_nbStale = 0, m_nbOverhangs = 0, m_nbDepthTargets = 0;
  uint64_t m_nbFanouts = 0;  // SR_NB_TEX_FANOUT: extra targets written by the resolve kernel
  Pm4EdramMeter m_meterCur;              // this frame's EDRAM-model work (published by pm4MeterPublish)
  void pm4MeterMoves(const std::vector<Pm4Move>& moves);
  void pm4MeterPublish();
  bool nbResolveToTarget(const edram::ResolveInfo& ri, const edram::Regs& regs, const Pm4NatOwners& owners,
                         uint32_t& extStart, uint32_t& extLen);
  // Held pages -> memory, except the holder (kind, key, slice) given.
  void nbMaterialize(uint32_t phys, uint32_t len, uint8_t exceptKind = 0xFF, uint64_t exceptKey = 0,
                     uint32_t exceptSlice = 0);
  void nbMaterializeHeld(uint32_t idx);
  int32_t nbRingSlot();  // a descriptor slot of strand B's ring in the RT SRV band (-1 = none)
#if SR_ORACLE
  bool nbHeldTarget(uint32_t phys, NbTgt& t);  // the target holding the page (SR_NB_RHASH)
#endif
  // iter-467 native-b B-CPU (SR_NB_BIND_MEMO): a texture slot whose fetch constant equals the one this slot bound
  // last time, with nothing a bind's answer depends on changed since - the clean set and the scaled-page marks
  // (m_texBindGen), the shared-memory stamp clock and the validation epoch - reuses that bind's answer instead of
  // hashing the fetch and looking it up again. Only answers the lookups' own fast paths gave are kept.
  struct BindMemo {
    uint32_t f[6] = {};
    uint32_t raw2d = 0, arr = 0, s3 = 0;  // getOrCreateTexture2D (pre-glyph-remap) / array / 3D-or-cube answers
    uint64_t gen = ~0ull;
    uint32_t clock = 0, epoch = 0;
    uint8_t kind = 0;  // 1: 2D (+ array), 2: 3D / cube
  };
  BindMemo m_bindMemo[16];
  std::atomic<uint64_t> m_texBindGen{0};  // bumped on any thread (the drain clears scaled marks too)
  uint64_t m_bindMemoHits = 0, m_bindMemoMiss = 0;
  bool m_texBindFast = false;  // the last getOrCreateTexture2D / 3D answered from the clean set
  // iter-467 native-b B4 (SR_NB_FRONT): the front buffers' native targets (kind 2, key = guest base): a resolve
  // into a buffer the game has presented writes its raw dwords into an R32 texture at the draw scale, the memory
  // is held, and the scan-out reads the texture.
  struct NbFront {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    uint32_t base = 0, pitch = 0, w = 0, h = 0, tsx = 1, tsy = 1, span = 0;
    uint64_t ver = ~0ull, lastUse = 0;
  };
  struct NbFrontSeen {
    uint32_t base = 0, w = 0, h = 0, pitch = 0;
    uint64_t use = 0;
  };
  std::vector<NbFront> m_nbFronts;
  NbFrontSeen m_nbFrontSeen[4];
  uint64_t m_nbFrontUse = 0, m_nbFrontResolves = 0, m_nbFrontScans = 0, m_nbFrontFallbacks = 0;
  bool nbFrontTarget(const edram::ResolveInfo& ri, uint32_t destBase);  // find / make the target (false = none)
  int32_t nbFrontScanSrv(uint32_t fbPhys, uint32_t width, uint32_t height, uint32_t pitch);  // -1 = memory
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_nbFrontRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_nbFrontPso;
  bool m_nbFrontPsoFailed = false;
#if SR_ORACLE
  Microsoft::WRL::ComPtr<ID3D12Resource> m_nbReadback;
  uint64_t m_nbReadbackBytes = 0;
#endif
  uint32_t m_bbW = 0, m_bbH = 0;             // current backbuffer size
  Microsoft::WRL::ComPtr<ID3D12Resource> m_dumpReadback;  // SR_PRESENT_DUMP of any size
  uint64_t m_dumpReadbackBytes = 0;
  void publishXlatSrv(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* view,
                      D3D12_CPU_DESCRIPTOR_HANDLE destination);
  // step 6 I3-B (bindless, Fable ruling 44): validates the draw's selection words and binds m_xlatSrvHeap's tables.
  void bindXlatTextures(const DrawBatch& batch);
  // The raw vertex-fetch view of a staged range: a ring slot of m_xlatSrvHeap (reused only once the native submission
  // that read it completed); its GPU handle.
  D3D12_GPU_DESCRIPTOR_HANDLE rawRingView(const NativeBufferSlice& slice);
  void xlatBindRootTables();
  // iter-464 resscale: root param 14 (b6 space4) = 1/scale - 1 per axis.
  void xlatSetPixelPosScale();
  void xlatBindBatchTextures(const DrawBatch& b);
  TranslatedConstantSnapshot bindXlatConstants(const DrawBatch& b);
  void bindXlatGeometry(const DrawBatch& b);

  // Is this format usable as an RTV on THIS device? Cached per format. The pool
  // must refuse an unrenderable format BY NAME rather than hand D3D12 a desc it
  // will reject (a failed PSO create is a silent nullptr and a dropped draw).
  bool formatRenderable(DXGI_FORMAT f);
  std::unordered_map<uint32_t, bool> m_fmtRenderable;
  // iter-447: the DISTINCT (rtv,dsv) format pairs the pool actually served. The
  // point of the slice is that this is >1; if it reports 1 the widening did
  // nothing and the fmt refusals moved for some other reason. Reported, so the
  // claim is falsifiable rather than inferred from a refusal count falling.
  std::unordered_set<uint64_t> m_poolFmtVariants;
  // Depth buffer for the offscreen guest RT (D32_FLOAT, cleared per frame).
  Microsoft::WRL::ComPtr<ID3D12Resource> m_geoDepth;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

  // ── iter-446 R1/R2: surface-pool state (contract on hleSurfaceGet above) ───
  // 64/16 against a MEASURED 16 render targets and 3 depth surfaces — 4x and 5x
  // headroom. Sized generously because these are CPU-only descriptors, and
  // because a pool that silently runs out would present as "some passes are
  // missing", the hardest possible symptom to attribute.
  static constexpr uint32_t kHleRtvSlots = 64;
  static constexpr uint32_t kHleDsvSlots = 16;
  bool ensureSurfaceHeaps();
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_hleRtvHeap, m_hleDsvHeap;
  UINT m_hleRtvSize = 0, m_hleDsvSize = 0;
  uint32_t m_hleRtvNext = 0, m_hleDsvNext = 0;
  // unordered_map: reference/pointer stability across rehash is guaranteed, so
  // the bind mirror below can hold raw pointers into it.
  // ⛔ NOTHING MAY ERASE FROM THIS MAP. HleBatch (and the bind mirror below)
  // hold RAW POINTERS into it; unordered_map guarantees reference stability
  // across rehash but not across erase. If eviction is ever wanted, the batches
  // must hold keys instead.
  std::unordered_map<uint32_t, HleSurface> m_hleSurfaces;
  HleSurface* m_hleBoundRT[4] = {nullptr, nullptr, nullptr, nullptr};
  HleSurface* m_hleBoundDS = nullptr;
  bool m_surfPoolOn = false;
  // Every one of these is a number the report prints; none is inferred.
  uint64_t m_surfCreated = 0;     // resources actually created
  uint64_t m_surfCreateFail = 0;  // CreateCommittedResource said no
  uint64_t m_surfBadHeader = 0;   // header did not decode — refused, not guessed
  uint64_t m_surfNoSlot = 0;      // descriptor heap exhausted
  uint64_t m_surfMsaaDowngrade = 0;  // console >1x, created 1x (see the .cpp)
  uint64_t m_surfRtBinds = 0, m_surfDsBinds = 0, m_surfNullBinds = 0;
  uint64_t m_resolveCopies = 0, m_resolveTexCreated = 0;  // iter-446 R5b
  // ⛔ AUDIT FIX: re-creates are the input that fires the lifetime bug, so
  // they are COUNTED. Measured 0 over 80,637 copies before the fix, which
  // is exactly why it was latent rather than observed.
  uint64_t m_resolveRecreates = 0;
  // ⛔ §6g.12: destinations refused because their declared extent was
  // unreadable or disagreed with the source. Counted, never approximated.
  uint64_t m_resolveBadDest = 0;
  // ⛔ AUDIT FIX (finding 3): the resolve SOURCE has never been written by a
  // draw — no draw path targets the pool yet — so a copy moves clear values,
  // not a render product. `hleNotePoolDraw` is the setter that path will
  // call; it has NO call site today AND THAT IS THE POINT. This counter
  // reports the gap instead of letting `copies=` imply content.
  uint64_t m_resolveSrcNeverDrawn = 0;
  // iter-446 R6. `unbound` is the honest one: a clear whose target slot holds
  // no pooled surface is DROPPED, and that must be visible rather than folded
  // into the executed count.
  uint64_t m_clearRt = 0, m_clearDs = 0, m_clearUnboundRt = 0;
  uint64_t m_clearUnboundDs = 0, m_clearNoList = 0;
  void hleTransition(HleSurface* s, D3D12_RESOURCE_STATES to);
  // ⛔ AUDIT FIX (iter-446 pre-bake): a displaced ID3D12Resource may NOT be
  // released at the point of replacement. D3D12 command lists hold no reference
  // to the resources they read, presentation is pipelined (kFrameCount=2, and
  // moveToNextFrame waits on the frame BEFORE last), and the whole 2D SRV table
  // is bound for every translated draw — so a frame still in flight can be
  // sampling exactly the resource being freed and the descriptor being
  // rewritten. Park it here and drop it once the fence proves the GPU is done.
  // The same release-in-place pattern exists in both pre-existing upload paths,
  // so this is deliberately shared rather than resolve-only.
  void retireResource(Microsoft::WRL::ComPtr<ID3D12Resource> res);
  void drainRetired(uint64_t completedFence);
  struct RetiredRes {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    uint64_t fence = 0;
  };
  std::vector<RetiredRes> m_retired;
  std::mutex m_retiredMu;
  // The back buffer's ACTUAL clear tuple this frame. dumpBackBuffer needs it
  // to distinguish a cleared frame from a drawn one — the thing nonBlackPx
  // could never do, having been quoted as evidence all through iter-446.
  uint8_t m_lastClearRgba8[4] = {10, 10, 26, 255};
  uint64_t m_retiredPeak = 0, m_retiredFreed = 0;

  // Texture cache (iter-359d): guest texture fetch constant (6 dwords) → 2D SRV
  // heap slot. Slot 0 = white fallback; heap layout [0..255] 2D, [256..263] 3D
  // white, [264..271] cube white. Uploads run on a dedicated list + waitForGpu.
  std::unordered_map<uint64_t, uint32_t> m_texCache;
  // Per-cached-texture guest range for resolve eviction (iter-359g): a mode-6
  // resolve rewrites guest memory that cached textures were built from.
  // iter-442 S4.4/S4.6 state. A tiny dedicated pool: 3 movie planes, keyed on
  // the guest texture handle so the SAME slot+resource is reused every frame.
  // Allocating slots per frame would burn the 8192-slot heap in ~3 minutes.
  struct HleTex {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    uint32_t slot = 0, w = 0, h = 0;
    bool uploaded = false;   // drives uploadTexture2D's fromShaderResource
    // iter-446 R5b: set on entries a RESOLVE produced. Their pixels come from
    // the GPU, so `uploaded`'s copy-path state machine does not describe them
    // and `fmt` must be remembered to know when a re-create is needed.
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool fromResolve = false;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    // ── iter-450UE FIX B: EXTRA VIEWS OVER THE SAME RESOURCE ────────────────
    // One guest texture can be sampled by different draws through different
    // component mappings, so the resource needs N views, not one. `slot` above
    // stays THE IDENTITY VIEW — hlePresentFront and the resolve re-create logic
    // read it, so its meaning must not change.
    // ⚠ The list lives INSIDE the entry on purpose: the code that replaces
    // `res` is then the same code that invalidates the views. A side map would
    // drift, and a view outliving its resource is a stale descriptor sampled by
    // an in-flight frame. Expected size is 0-2.
    struct SwizView { uint16_t swiz; uint32_t slot; };
    std::vector<SwizView> swizViews;
  };
  std::unordered_map<uint32_t, HleTex> m_hleTextures;

  // ── iter-444 S6f: THE PER-FRAME DRAW LIST ─────────────────────────────────
  // WHY A LIST AND NOT A LATCH: S6e latched ONE expanded draw and recorded it
  // at Present. Widening the shape gate past the movie quad would then submit
  // ~900 draws/frame into a one-slot latch, and the coverage instrument counts
  // "rendered" = draws recorded into a command list — so the number would start
  // LYING (899 counted, 1 recorded). The list makes the count true again.
  //
  // ── iter-445 C6: THE OPEN-LIST CONTRACT (SR_HLE_FRAME_LIST) ──────────────
  // The paragraph that used to sit here explained why the list is CLOSED during
  // execute(). That is no longer true under the gate, and a stale rationale is
  // how the next session plans from a model that does not exist.
  //
  // THE MODEL: ONE open window per frame, opened LAZILY by ensureFrameOpen()
  // from whichever of hle::execute() / presentFramebuffer reaches it first;
  // exactly ONE Close+Execute per present (the ledger's delta equality);
  // dumpBackBuffer AFTER Close+Execute and BEFORE Present; one frame in flight
  // via the present rendezvous. Draws are still STAGED and recorded in one pass
  // by renderHleDraws — that has not changed and is what keeps the counters
  // below meaning what they say.
  //
  // THE HONEST TRIPLE, restated because a counter whose meaning quietly changes
  // under a refactor is worse than no counter (this project has already been
  // burned once — feedback_measure_dont_assert):
  //   recorded — DrawInstanced calls actually issued by renderHleDraws.
  //              UNCHANGED: renderHleDraws is still the single record point.
  //   dropped  — staged then retired without ever being recorded
  //              (hleRetireFrame's else-arm). UNCHANGED for the same reason,
  //              and it now has a SECOND source worth knowing about:
  //              m_clDiscards counts a frame recorded into a list that was
  //              never submitted — the failure the open window newly makes
  //              possible, which C3's FrameGuard turns into a counted event
  //              instead of a silent one.
  //   held     — unchanged.
  // MEASURED BASELINE to compare against (present 3900): recorded=750/24836,
  // dropped=0, held=579. Quote numbers someone produced, not invented absolutes.
  //
  // kHleMaxDraws / HleReject::TooManyDraws STAY. Under an open list they no
  // longer bound a fixed vector, but they remain the only thing that names a
  // runaway frame, and an unnamed runaway is indistinguishable from a hang.
  //
  // WHY DEDICATED RINGS and not the xlat path's m_xlatVB/m_xlatCB: that path is
  // the PM4/LLE fixture, kept only for offline replay and slated for removal.
  // Sharing its rings would couple the new renderer to the old one — exactly
  // the drift the rebuild exists to stop.
  struct HleBatch {
    ID3D12PipelineState* pso;
    D3D12_GPU_VIRTUAL_ADDRESS vsCbv, psCbv, sharedCbv;
    uint64_t vbOffset;          // BYTE offset into m_hleVB
    uint32_t vertexCount;       // AFTER quad/rect expansion
    uint32_t stride;            // nAttrs*16
    D3D12_PRIMITIVE_TOPOLOGY topology;
    D3D12_VIEWPORT viewport;    // S6h: per-draw, from the guest's viewport regs
    // ── iter-446 S1: THE DRAW'S OWN RENDER-TARGET IDENTITY ──────────────────
    // ⚠ CAPTURED PER DRAW, AT SUBMIT, AND THAT IS NOT AN OPTIMISATION. The bind
    // mirror (m_hleBoundRT/m_hleBoundDS) is rewritten 33-35 times per frame
    // (ENGINE_MAP §6g.4), so reading it later — inside renderHleDraws, at
    // Present — would give EVERY batch the guest's LAST bind of the frame
    // rather than the target it was actually drawn to. That is the whole
    // difference between a pass graph and a pile.
    // Raw pointers into m_hleSurfaces are safe ONLY because that map is
    // emplace/find and NOTHING ERASES FROM IT — see the note at its declaration.
    HleSurface* rt[4];
    HleSurface* ds;
    // iter-446 S5: the PSO built for the POOL target's formats. Null when this
    // draw cannot be re-pointed (no bound surface, or a colour format the PSO
    // cache was not asked for). Separate from `pso` because the PSO BAKES its
    // target formats - one pipeline cannot serve two different RTV/DSV pairs.
    ID3D12PipelineState* poolPso;
    // Shader identity, so a later step can carve out the movie quad by hash
    // instead of by guessing at a draw's shape.
    uint64_t vsHash, psHash;
    // iter-455l r3 (ruling ad4621a defect 1): the provenance payload, precomputed
    // at SUBMIT (the only moment the draw's CPU constant blocks are alive) so the
    // post-success chokepoint inside recordHleBatchToPool can feed the ring from
    // EITHER caller — in-order submit or the renderHleDraws fallback — including
    // held-list re-records presents later. Filled only when SR_PROV_RING=1.
#if SR_ORACLE
    prov::PoolDrawFeed provFeed;
#endif
  };
  std::vector<HleBatch> m_hleBatches;   // built this frame by hleSubmitDraw
  std::vector<HleBatch> m_hleHeldList;  // last NON-EMPTY frame (the 2:1 hazard)
  bool renderHleDraws();
public:
  // iter-455l provenance ring (observe-only readers; no state change):
  uint64_t drawOrdinalNow() const {
    return m_drawIdOrdinal.load(std::memory_order_relaxed);
  }
  const void* hleTexHostPtr(uint32_t key, uint32_t* fmt, uint32_t* w, uint32_t* h,
                            uint32_t* slot) const {
    auto it = m_hleTextures.find(key);
    if (it == m_hleTextures.end() || !it->second.res) return nullptr;
    if (fmt)  *fmt = (uint32_t)it->second.fmt;
    if (w)    *w = it->second.w;
    if (h)    *h = it->second.h;
    if (slot) *slot = it->second.slot;
    return (const void*)it->second.res.Get();
  }
private:
  // iter-446 S2: the frame-global head and the per-batch record, split out so a
  // draw can be recorded at STREAM POSITION (inside hle::execute) instead of
  // only at Present. The reference translates every command 1:1 as it drains
  // (video.cpp ProcDrawPrimitive calls drawInstanced directly in the dispatch
  // switch, beside ProcClear and ProcStretchRect) - our staging-until-Present
  // is the deviation, and it puts every Resolve of frame N BEFORE the draws it
  // is meant to be resolving.
  bool hleFrameStateReady() const;
  void flushHleFrameState();
  void recordHleBatch(const HleBatch& b, ID3D12PipelineState*& curPso,
                      D3D12_PRIMITIVE_TOPOLOGY& curTopo);
  bool recordHleBatchToPool(const HleBatch& b, ID3D12PipelineState*& curPso,
                            D3D12_PRIMITIVE_TOPOLOGY& curTopo);
  void rebindFrameTarget();
  bool m_hleFrameStateSet = false;        // reset per frame in beginFrame
  bool m_hleFrameRecordedInOrder = false; // reset per frame in beginFrame
  uint64_t m_hleInOrderRecorded = 0, m_hleInOrderFrames = 0;
  uint64_t m_depthClearZfuncLate = 0;
  uint32_t m_hleDepthClearLast = 0;
  bool m_hleDepthClearSeen = false;
  // Exactly one back-buffer colour clear per frame, from one of two sites.
  // Both counted so a frame that cleared twice - or never - is visible.
  uint64_t m_frameOpenClears = 0, m_frameTailClears = 0;
  void drawRtCensus();   // iter-446 S1 — proves the capture is per-DRAW
  uint64_t m_drawRtFrames = 0, m_drawRtMultiFrames = 0;
  uint64_t m_drawRtNullTarget = 0, m_drawRtMrt = 0;
  // The per-draw PROOF: batches whose captured target differs from the
  // mirror's value at Present. Zero forever = the capture is frame-global.
  uint64_t m_drawRtVsMirror = 0;
  uint64_t m_drawRtBindsAtLastSubmit = 0, m_drawRtDiscriminable = 0;
  // The distinct (rtvFormat, dsvFormat) pairs staged draws would demand of
  // the PSO cache if they were re-pointed at their bound pool surface.
  uint64_t m_drawFmtPairs[16] = {};
  uint32_t m_drawFmtN = 0;
  // iter-446 S5. `poolDraws` is the number that makes srcNeverDrawn fall;
  // `poolRefusedFmt` is every draw whose bound surface the PSO cache was
  // not asked to serve - counted, never silently sent to the wrong target.
  uint64_t m_poolDraws = 0, m_poolRefusedFmt = 0, m_poolRefusedNoRt = 0;
  // iter-449b: depth-LESS pool draws (RT0 bound, DS NULL — a census, not a
  // refusal; they render with a null DSV). The [S6_NULLBIND] probe proved the
  // old `noRt` union was almost entirely this class.
  uint64_t m_poolNoDsDraws = 0;
  // iter-449c: landings split by producer (index = HleDraw::srcType 0/1/2;
  // [0]=pool target, [1]=legacy backbuffer). The UI question in one line.
  uint64_t m_landByType[2][3] = {};
  // Retires the staged frame EXACTLY ONCE per present, whatever path present
  // took. presentFramebuffer has four early returns (m_initialized, the guest
  // fb range checks, the degenerate copy rect) and an upload branch that never
  // reaches renderHleDraws — on any of those the staged batches would otherwise
  // accumulate for ever and the ring cursors would never reset. Driven by an
  // RAII guard so a future sixth exit gets it for free.
  void hleRetireFrame(bool recorded);

  // Sized from measurement, not guessed. ~900 draws/frame; per draw the CB
  // costs 4096 (VS) + 3584 (PS) + 512 (shared), 256-aligned => ~8.4 KB, so a
  // frame needs ~7.6 MB; the VB costs at most kMaxVertSnapshot expanded, which
  // at the 1.5x quadlist growth is ~6 KB => ~5.4 MB. Two halves each, so the
  // half still referenced by the in-flight frame is never overwritten.
  // iter-449e: that VB arithmetic predates the GATHER producers — a single
  // indexed gather now delivers up to 1 MB of packed vertices that expand
  // ~3x to float4 attributes, and conv3 MEASURED vbFull=67,116 refusals/roll
  // (3.9% of recorded) against the 32 MB half.
  // iter-449o BAKE-AUDIT CORRECTION: the 4x (256 MB) answer commits EAGERLY
  // at init — even in the gate-off default exe, which has no consumer for it
  // (+192 MB default-build regression, found by the audit). And the demand
  // that motivated it has since been absorbed by the VB residency cache
  // (vbc4: vbFull=0). 2x covers the measured cache-less worst case
  // (67K refusals were 3.9% at 1x; 2x clears the observed peak) and the
  // refusal stays counted either way — vbFull says if this is ever wrong.
  static constexpr uint64_t kHleVBBytes = 128ull << 20;   // 64 MB per half
  static constexpr uint64_t kHleCBBytes = 96ull << 20;   // 48 MB per half
  static constexpr uint32_t kHleMaxDraws = 8192;
  static constexpr uint32_t kHleSharedBytes = 512;       // 128 dwords, c0..c31
  Microsoft::WRL::ComPtr<ID3D12Resource> m_hleVB, m_hleCB;
  uint8_t* m_hleVBPtr = nullptr;
  uint8_t* m_hleCBPtr = nullptr;
  uint64_t m_hleVBUsed = 0, m_hleCBUsed = 0;
  uint32_t m_hleHalf = 0;
  // iter-444 S6i: the depth CLEAR value is forced by the guest's own compare
  // function, not by convention. Measured: RB_DEPTHCONTROL zfunc=6 (GEQUAL), so
  // clearing to the D3D-habitual 1.0 rejects EVERY fragment — general geometry
  // then renders nothing at all, which is easily mistaken for "the shard pile
  // is fixed". Far plane under GREATER/GEQUAL is 0.0; under LESS/LEQUAL it is 1.0.
  float m_hleDepthClear = 1.0f;
  bool m_hleRingsReady = false;
  bool hleEnsureRings();
  HleReject m_hleLastReject = HleReject::None;
  // The constants are DEVICE state: a draw with no SetConstants before it
  // legitimately inherits the previous draw's. Held host-side, host-endian.
  std::vector<uint8_t> m_hleVsConst, m_hlePsConst;
  bool m_hleHaveConst = false;

  uint32_t m_hleQuadAge = 0;
  uint64_t m_hleDrawn = 0, m_hleHeld = 0, m_hleSerial = 0;
  // iter-450UA [DRAWID]: monotonic submit ordinal, assigned at hleSubmitDraw
  // entry. Deterministic under replay; the handle SR_DRAWID_SUPPRESS targets.
  std::atomic<uint64_t> m_drawIdOrdinal{0};
  // iter-450UE FIX B census. Every refusal reason is counted separately so a
  // "nothing happened" result names WHICH gate stopped it, rather than being a
  // single number that could mean five different things.
  std::atomic<uint64_t> m_swzBindings{0};      // swizzle decoded + composed
  std::atomic<uint64_t> m_swzIdentity{0};      // composed == D3D12 default
  std::atomic<uint64_t> m_swzCensusOnly{0};    // mode 1: would apply, didn't
  std::atomic<uint64_t> m_swzSkipMode{0};      // mode 2: not single-channel
  std::atomic<uint64_t> m_swzSkipResolve{0};   // resolve product, out of scope
  std::atomic<uint64_t> m_swzUnknownFmt{0};    // DXGI not in the host table
  std::atomic<uint64_t> m_swzBaseMismatch{0};  // fetch entry != bound key
  std::atomic<uint64_t> m_swzExhausted{0};     // SRV heap full -> identity
  std::atomic<uint64_t> m_swzViews{0};         // distinct views created
  uint64_t m_hleDropped = 0;   // staged, retired without ever being recorded
  uint64_t m_hleVbFull = 0, m_hleCbFull = 0, m_hleListFull = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_hleUpload;   // 64KB: legacy scratch
  uint8_t* m_hleUploadPtr = nullptr;
  unsigned long m_hleThread = 0;   // first submitter; a change is a real bug

  // ── iter-445 C1: THE FRAME-LIST LEDGER ────────────────────────────────────
  // The open-command-list refactor changes WHEN a D3D12 command list is legally
  // open. Before touching that, make the CURRENT lifecycle's invariants visible
  // so the refactor has a baseline it can be caught violating. This commit adds
  // no behaviour — it only counts, checks HRESULTs nobody was checking, and
  // names each site.
  //
  // ⚠ SEEDED `Recording`, NOT `Closed`. A D3D12 command list is born RECORDING
  // (CreateCommandList at :221 has no Reset and is followed by an explicit
  // Close at :228). Seeding Closed would make that first Close a
  // close-while-closed violation on every run from process start, and an
  // instrument that reports a false positive on unchanged code is worthless.
  enum class ClState : uint8_t { Closed, Recording };
  enum ClSite : uint32_t {
    Create = 0, InitClose, XlatReset, XlatClose, XlatExec, GeoReset,
    CopyRtClose, CopyRtExec, ResolveAsyncClose, ResolveAsyncExec,
    ResolveSyncClose, ResolveSyncExec, ResolveReset, FrameOpenAlloc, FrameOpen,
    HostImgClose, HostImgExec, GeoBranchClose, GeoBranchExec, FrameTailClose,
    FrameTailExec, GuardDiscard, Probe, Pm4Close, Pm4Exec, Pm4Reset, SiteCount
  };
  static const char* clSiteName(uint32_t s);
  ClState m_clState = ClState::Recording;
  uint64_t m_clOpens = 0, m_clCloses = 0, m_clExecutes = 0;
  uint64_t m_clViolations = 0, m_clResetFails = 0, m_clCloseFails = 0;
  uint64_t m_clDiscards = 0, m_clTidMismatches = 0;
  // The init epoch runs ONCE per process before any present (CreateCommandList
  // + createXlatPipeline's reset/close/execute). Reporting raw totals would
  // fold it into every frame's delta and hide the equality that matters.
  uint32_t m_clInitOpens = 0, m_clInitCloses = 0, m_clInitExecutes = 0;
  bool m_clInitLatched = false;
  uint64_t m_clSiteHits[SiteCount] = {};
  unsigned long m_clTid = 0;
  HRESULT openNativeRecording();
  [[noreturn]] void nativeRecordingFail(const char* stage, HRESULT hr);
  void keepNativeImage(const NativeImageLease& image);
  void waitNativeFence(ID3D12Fence* fence, uint64_t value, HANDLE event, const char* stage);
  void clReset(ClSite s);
  void clClose(ClSite s);
  void clExecute(ClSite s);
#if SR_ORACLE
  uint32_t m_readerStream = 0;
  uint64_t m_readerRecording = 0;
  void readerObserveCompletion();
#endif
  void clNoteTid(int site = -1);  // iter-465 perf2: the site is named in a mismatch
  // Called at every point that RECORDS into m_commandList. Recording into a
  // closed list is the exact failure the refactor can introduce, and D3D12
  // reports it only via the debug layer (compiled out today) or a failed
  // Close() — so count it ourselves.
  void recordGuard(ClSite s);
  // iter-445 C2: the debug layer, compiled in and gated at runtime.
  // dbgActive is the HONEST flag — level was requested, layer actually came up.
  // Reported on the ledger line so no verdict can quote "no layer errors" while
  // the layer was silently unavailable.
  uint32_t m_dbgLevel = 0;
  bool m_dbgActive = false;
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
  uint64_t m_dbgMessages = 0, m_dbgErrors = 0, m_dbgWarnings = 0;
  std::unordered_set<uint32_t> m_dbgSeen;
  void drainDebugLayer();
  // iter-460: one-shot device-removal report - the removal reason plus, under
  // SR_D3D12_DRED=1, the in-flight command list's auto-breadcrumbs and the
  // page-fault VA with the allocations around it. Returns true once removed.
  bool checkDeviceRemoved(const char* where);
  bool m_dredOn = false, m_removalReported = false;
  // iter-460 GPU breadcrumbs (SR_D3D12_DRED=1): WriteBufferImmediate stamps
  // {frame, batch} into CPU-visible memory before (MARKER_IN) and after
  // (MARKER_OUT) every PM4 draw, so a hang names its batch even when DRED's
  // own marker contexts are empty. crumbs[0..1] = started, [2..3] = finished.
  void pm4Crumb(uint32_t batch, bool after);
  Microsoft::WRL::ComPtr<ID3D12Resource> m_crumbBuf;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> m_commandList2;
  volatile uint32_t* m_crumbs = nullptr;
  bool m_crumbFailed = false;
  // The GPU runs frames behind the CPU: keep the last frames' batch summaries
  // so a hang report can name a batch of an older frame.
  struct CrumbBatch {
    uint64_t vs, ps;
    uint32_t count, topo, vbOff, ibOff;
    bool vsf, point;
  };
  struct CrumbFrame {
    uint32_t frame = 0xFFFFFFFFu;
    std::vector<CrumbBatch> b;
  };
  CrumbFrame m_crumbHist[4];
  // iter-445 C3: bumped at every frame open. FrameGuard captures the epoch it
  // expects and acts only if that epoch is still current, so a guard can never
  // close a list that some other path already closed and reopened.
  uint64_t m_frameEpoch = 0;
  // iter-445 C4: the open-command-list model. beginFrame/endFrame split the
  // lifecycle out of presentFramebuffer; ensureFrameOpen is the lazy,
  // idempotent entry both hle::execute() and presentFramebuffer call.
  ID3D12Resource* m_frameRT = nullptr;   // this frame's backbuffer, cached
  uint64_t m_windowCmds = 0;             // commands seen with the window OPEN
  uint64_t m_legacyExecutes = 0;         // a legacy branch executed under the gate
  bool m_deviceRemoved = false;          // sticky: DXGI_ERROR_DEVICE_REMOVED
  // iter-445 C7 pre-bake fixes (audit)
  uint64_t m_openEpoch = 0;              // stamped by beginFrame; FrameGuard's key
  bool m_clTidRebased = false;           // re-latch tid at the render-thread handoff
  bool m_clLastCloseOk = true;           // clExecute refuses a poisoned list
  uint64_t m_clSkippedSubmits = 0;
  uint64_t m_geoBatchesDropped = 0, m_geoVertsDropped = 0;
  void beginFrame();
  // iter-445 S6k: the stream element 0 declares — every other consumed element
  // must match it, because the producer snapshots exactly one stream's buffer.
  uint32_t m_hleDrawStream = 0;

  struct TexEntry { uint64_t key; uint32_t basePhys; uint32_t bytes; };
  std::vector<TexEntry> m_texEntries;
  // P1 dirty tracking (invalidateGuestRange): 64 KB guest page -> texture keys
  // reading it, and the keys validated since their pages last changed.
  std::unordered_map<uint32_t, std::vector<uint64_t>> m_texPages;
  std::unordered_set<uint64_t> m_texTracked;
  // key -> slot for every texture validated since its pages last changed
  // (iter-460 perf: the bind fast path is this one lookup).
  std::unordered_map<uint64_t, uint32_t> m_texClean;
  void texMarkClean(uint64_t key, uint32_t slot, uint32_t phys, uint64_t bytes);
  // iter-466 cp lane (SR_SHM_WT_TEXCLEAN, needs SR_SHM_WRITE_TRACK): a clean mark survives the
  // present when every page the GPU loader read is write-protected and unchanged since (the
  // released blocks invalidate at the present instead of every texture). The shared-memory
  // 2D path arms m_texTrack* for its one texture; every other mark is frame-clean as before.
#if SR_ORACLE
  struct TexTrack {
    bool armed = false;
    uint32_t base = 0, baseLen = 0, mip = 0, mipLen = 0, version = 0;
  } m_texTrack;
  std::unordered_set<uint64_t> m_texCleanFrame;  // clean marks to drop at the present
  uint64_t m_texTrackLast = 0;  // the key the last armed mark tracked (its mip mark follows)
#endif
  std::vector<uint32_t> m_texReleasedTmp;
#if SR_ORACLE
  uint64_t m_texTrackedMarks = 0, m_texFrameMarks = 0, m_texBlockInvals = 0;
#else
  uint64_t m_texBlockInvals = 0;
#endif
#if SR_ORACLE
  bool texCleanTrackedOn() const;
  bool texRangeProtected(uint32_t phys, uint32_t len) const;
#endif
  void invalidateTexturesAtPresent();
  uint32_t m_texNextSlot = 1;
  // iter-464 input lane: DEVICE-MATCHED BUTTON PROMPTS (gfx/glyph_subst.hpp). Per 2D
  // slot: the prompt icon the texture in it was identified as (0 = none) and the guest
  // base page it was identified at (a slot later reused by another producer fails the
  // page check). Replacements live in a slot band reserved at init, never evicted.
  std::vector<uint16_t> m_glyphIcon;
  std::vector<uint32_t> m_glyphBasePage;
  std::unordered_map<uint64_t, uint32_t> m_glyphRepl;  // (icon, set, variant) -> slot, 0 = keep
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_glyphRes;
  uint32_t m_glyphNextSlot = 0, m_glyphSlotEnd = 0;
  bool m_glyphOn = false;
  void glyphNote(uint32_t slot, uint32_t basePage, uint32_t fmt, uint32_t w, uint32_t h,
                 const uint8_t* src, size_t avail, uint32_t pitchTexels, bool tiled,
                 uint32_t blockW, uint32_t blockBytes);
  uint32_t glyphRemap(uint32_t slot, const uint32_t* fetch);
  // iter-449x: the volume table (space1, heap [kTex2DSlots..+7], idx 0=white).
  struct HleVol {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    uint32_t idx = 0, w = 0, h = 0, d = 0;
    bool uploaded = false;
    // iter-457: this volume is a RESOLVE PRODUCT, not a guest upload.
    // The two must not be confused: an upload owns whole-volume pixels,
    // a resolve fills one slice at a time and the rest stay undefined
    // until their own resolve lands. `sliceMask` records which slices
    // have actually been written, so "partially populated" is visible
    // rather than looking identical to a finished texture.
    bool fromResolve = false;
    uint32_t sliceMask = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  };
  std::unordered_map<uint32_t, HleVol> m_hleVolumes;   // key -> volume
  uint32_t m_volNextIdx = 1;                           // 1..7

  // ── iter-457 H1(b): GUEST CUBE MAPS ──────────────────────────────────────
  // A cube is a TEXTURE2D with SIX ARRAY SLICES viewed through a
  // D3D12_SRV_DIMENSION_TEXTURECUBE SRV. D3D12 has no cube resource
  // dimension, so creating a TEXTURE3D of depth 6 here would compile, bind,
  // and sample the wrong thing.
  //
  // The descriptor range is [kTex2DSlots+8 .. +15]: index 0 is the white
  // 6-face dummy created at init, so a real cube may only take 1..7.
  //
  // r5: the key table, the FREE LIST of indices 1..7, the fence-retired old
  // (resource, slot) pairs, the dedicated upload-context pool and the
  // counters are ONE unit - gfx/cube_transaction.hpp - so that
  // kernel/tex_unlock_test.cpp drives the identical body on a WARP device.
#if SR_ORACLE
  sr::gfx::cubetx::CubeTransactor m_cubeTx;
#endif
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_texResources;
  // step 4 G7 (lane D): every 2D answer passes here, so SR_TEX_CACHE_EVICT stamps the slot's last bind whatever path
  // answered (the clean-set fast path included; the bind memo stamps its own hits)
  uint32_t getOrCreateTexture2D(const uint32_t* fetch6) {
    const uint32_t slot = getOrCreateTexture2DLookup(fetch6);
    if (m_texEvictOn) texEvictUse(slot);
    return slot;
  }
  uint32_t getOrCreateTexture2DLookup(const uint32_t* fetch6);
  // P1: a stacked 2D texture (fetch dimension 2D, size_2d.stack_depth > 0) as
  // a Texture2DArray SRV slot (0 = not stacked / failed). Each layer is decoded
  // by getOrCreateTexture2D at base + i * layer stride (Xenia texture_util
  // array_slice_stride_bytes) and copied in; a changed layer re-copies.
  // cube = true: fetch dimension kCube - six faces at the same slice stride,
  // viewed as a TEXTURECUBE SRV (the t0 space8 table over the 2D band).
  uint32_t getOrCreateTexture2DArray(const uint32_t* fetch6, bool cube = false);
  struct TexArray {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    uint32_t slot = 0, layers = 0;
    std::vector<ID3D12Resource*> layerSrc;  // the layer textures last copied in
    std::vector<uint64_t> layerHash;        // and their content hashes then
    std::vector<uint32_t> layerVer;         // GPU path: shared-memory versions per layer
                                            // (iter-463: + the mip region's, last)
    uint32_t levels = 1;                    // iter-463: mip levels of the resource
    bool gpu = false;                       // last filled by pm4ShmLoadArray
    uint32_t checkEpoch = ~0u, checkClock = ~0u;  // when the versions were last scanned
#if SR_ORACLE
    std::vector<uint8_t> layerOrigin;  // mem.resources textures_from_resolves: each layer's content origin (TexOrigin)
    uint8_t texIdClass = 0;            // ... and the class of the last full check
#endif
    // iter-467 native-b B2: a layer's identity for native-target resolves (single level, no layout)
    uint32_t base = 0, layerStride = 0, pitch = 0, fmt = 0, endian = 0, w = 0, h = 0, tsx = 1, tsy = 1;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    bool nbIndexed = false, uav = false, tiled = false;
  };
  std::unordered_map<uint64_t, TexArray> m_texArrays;
  // The array resource + its SRV (cube or 2D array) in the 2D slot band.
  bool pm4TexArrayCreate(TexArray& a, DXGI_FORMAT dxgi, uint32_t width, uint32_t height,
                         uint32_t layers, bool cube, uint32_t fetchDword3, uint32_t levels = 1, bool scaled = false);
  // P1: a real volume (fetch dimension k3D, size_3d) as a Texture3D SRV in the
  // 2D slot band (the t0 space7 table). Base level only.
  uint32_t getOrCreateTexture3D(const uint32_t* fetch6);
  struct TexVolume {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    uint32_t slot = 0;
    uint64_t hash = 0;  // source-span content hash at the last upload
    uint32_t levels = 1;  // MipLevels of res (SR_TEX3D_MIPS creates the whole chain)
  };
  std::unordered_map<uint64_t, TexVolume> m_texVolumes;

  // iter-408 PLATFORM FIX: CPU-write revalidation. The console's GPU sampled
  // guest RAM directly, so a CPU write to a sampled surface was visible on the
  // very next draw with no invalidation protocol at all. Our host cache is an
  // optimisation the console never had, and it was keyed ONLY on the fetch
  // constant (address/format/size) — so a surface the guest rewrites in place,
  // like a video frame, uploaded once and then froze forever. Keeping the
  // uploaded content's source hash lets a changed surface re-upload into the
  // SAME slot and resource, so descriptors stay valid and the 1024-slot pool
  // is not consumed one frame at a time.
  struct TexRefresh {
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    uint64_t contentHash = 0;
    uint32_t slot = 0;
    uint64_t lastUse = 0;  // m_texUseClock at last bind; drives LRU eviction
    uint64_t shareKey = 0;  // iter-466 res4k (SR_TEX_SHARE): the identity whose resource it binds
    UINT componentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;  // exact published SRV recipe
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t allocBytes = 0;  // step 4 G7 (SR_TEX_CACHE_EVICT): its own resource's size (a shared one: TexShared's)
#if SR_ORACLE
    // mem.resources textures_from_resolves (gfx/resource_texid.hpp): where the bound content came from (TexOrigin)
    // and the class of the last full check (a clean-mark hit repeats it).
    uint8_t texIdOrigin = 0, texIdClass = 0;
#endif
  };
  std::unordered_map<uint64_t, TexRefresh> m_texRefresh;
  // iter-466 res4k (SR_TEX_SHARE): GPU-loaded textures shared by IDENTITY (Xenia TextureKey: base,
  // mips, format, size, pitch, tiling, endian, levels, scale) - fetch constants that differ only in
  // sampler state / swizzle keep their own slot + SRV but bind one resource, loaded once per version.
  struct TexShared {
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    uint64_t contentHash = 0;  // the version the resource holds
    uint32_t users = 0;        // m_texRefresh entries binding it
    uint64_t allocBytes = 0;   // step 4 G7 (SR_TEX_CACHE_EVICT): the resource's size
    // iter-467 native-b B2: the identity a resolve must match to write it as its native target
    // (gfx/native_targets.inc nbResolveToTarget): guest base / version range, pitch, format, endian,
    // guest size, scale, one level, tiled, typeless + UAV.
    uint32_t base = 0, baseLen = 0, pitch = 0, fmt = 0, endian = 0, w = 0, h = 0, tsx = 1, tsy = 1;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    bool single = false, tiled = false, uav = false;
#if SR_ORACLE
    uint8_t texIdOrigin = 0;  // mem.resources textures_from_resolves: the content's origin (TexOrigin; kTarget = nbTgtStamp)
#endif
  };
  std::unordered_map<uint64_t, TexShared> m_texShared;
  std::unordered_multimap<uint32_t, uint64_t> m_texSharedByBase;  // iter-467 native-b: guest base -> share key
  void texSharedErase(uint64_t shareKey);  // the last binder left: index + native-target hold (B2)
  uint64_t m_texShareSkips = 0, m_texShareLoads = 0;
  // iter-408c: the slot pool never reclaimed anything, so `creates` hit the
  // 1024 ceiling on EVERY run and every texture after that was silently white.
  // The console had no such pool; a bounded descriptor heap is our constraint,
  // so it needs the eviction policy the console never had to express.
  uint64_t m_texUseClock = 0;
  // An evicted texture may still be referenced by a command list already
  // submitted on the queue, so it is retired behind a fence value rather than
  // released at eviction. The queue is serial and every upload signals
  // m_texFence and waits, so once the fence passes, prior work has completed.
  std::vector<std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>> m_texRetired;
  // step 4 G7 (lane D): SR_TEX_CACHE_EVICT (baked ON; =0 = the cache as before). Without it a key leaves the
  // 2D cache only when the 8192-slot pool is full, so every reload added keys, resources and video memory (G7 run
  // A: tex_cache 1,691 -> 2,095 over six reloads, video local +37.5 MB). With it, ONE erase path (texEvictKey) takes
  // an entry out of every map that knows it - m_texCache / m_texRefresh / m_texClean / m_texCleanFrame /
  // m_texPages / m_texTracked / m_texEntries / m_texShared, and the bind memo - and retires its resource and slot
  // behind the frame fence; the slot is reused once the GPU passed it (texEvictDrain). Sweeps (texEvictTick: every
  // kTexSweepFrames frames, and at the next frame after a burst of creates or a create over the budget) evict (a)
  // entries unused for m_texEvictAge frames, (b) the least recently used rest over the byte budget. The policy and
  // the retire list: gfx/tex_evict_policy.hpp (tools/difftest/tex_evict_policy_test.cpp).
  enum TexEvictWhy : uint8_t { kTexEvAge, kTexEvBudget, kTexEvExhaust, kTexEvRecreate, kTexEvResolve, kTexEvWhyN };
  bool m_texEvictOn = false;
  uint64_t m_texEvictAge = 0, m_texEvictBudget = 0;  // frames; bytes (0 = no budget)
  std::vector<uint64_t> m_texSlotUse;                // per 2D slot: the frame fence of its last bind (gate on)
  struct TexReg {
    uint32_t phys = 0, len = 0;
    bool tracked = false;  // the SR_SHM_WT_TEXCLEAN registration (its m_texTracked mark carries ^ 0x7472)
  };
  std::unordered_map<uint64_t, std::vector<TexReg>> m_texRegs;  // key -> its m_texPages / m_texTracked registrations
  sr::gfx::texevict::SlotRetire<Microsoft::WRL::ComPtr<ID3D12Resource>> m_texSlotRetire;
  std::vector<uint32_t> m_texFreeSlots;     // retired slots the GPU is done with
  std::vector<uint64_t> m_texEvictedKeys;   // evicted since the last m_texEntries compaction
  uint64_t m_texLiveBytes = 0, m_texLivePeak = 0, m_texEvictCount = 0, m_texEvictBytes = 0, m_texSlotReuses = 0;
  uint64_t m_texEvictWhyN[kTexEvWhyN] = {};
  uint64_t m_texPageRefs = 0;               // keys held in m_texPages (counted with the gate off too)
  uint64_t m_texSweepFence = 0, m_texTickFence = 0, m_texSweeps = 0;
  uint32_t m_texCreatesSinceSweep = 0;
  bool m_texSweepWanted = false;
  // fix2 (Astra 5d0c2a63 P1): the slots the draw being assembled selected (xlatBindBatchTextures keeps slot numbers
  // across its lookups and builds its tables after them): never an exhaustion victim. Reset per draw.
  uint32_t m_texPinned[64] = {};
  size_t m_texPinnedN = 0;
  uint64_t m_texTransientFails = 0;              // fix2 (P2): exhaustion with no eligible victim, not cached
  uint64_t m_texTransientSuppressed = 0;         // fix3 (P2): ... of which retries skipped (the pool unchanged)
  sr::gfx::texevict::RetryGate m_texRetry;       // fix3 (Astra 0a384c24 P2): retry a transient exhaustion only on change
  uint64_t m_texSweepFrames = 300, m_texKeepFrames = 30;  // fix3: the sweep period / the budget's keep window
                                                          // (SR_TEX_CACHE_SWEEP_FRAMES / _KEEP_FRAMES, oracle only)
  uint64_t m_texSweepUsMax = 0, m_texDrainUsMax = 0;  // fix2: the worst sweep / drain (the hitch check reads them)
  std::vector<uint32_t> m_texEvictPages;         // fix2: pages to clean of the keys evicted since texEvictFinish
  D3D12_SHADER_RESOURCE_VIEW_DESC m_texNullSrv{};  // the view a freed slot goes back to (the init's 2D null)
#if SR_ORACLE
  std::vector<uint8_t> m_texSlotEvicted;  // debug check: 1 = the slot's entry was evicted (binding it is stale)
  uint64_t m_texStaleBinds = 0;
  void texEvictStaleBind(uint32_t slot);
#endif
  void texEvictInit();
  void texEvictKey(uint64_t key, TexEvictWhy why, uint32_t* reuseSlotNow = nullptr);
  void texEvictTick();
  void texEvictSweep();
  void texEvictDrain(size_t maxN);
  void texEvictFinish();  // m_texEntries compaction + the batched m_texPages cleanup of the keys just evicted
  // fix2 (Astra P2): ONE allocator for every consumer of the 2D slot pool - a retired slot the GPU is done with
  // (gate on), then a fresh one; 0 = none
  uint32_t texSlotAlloc();
  bool texSlotAvailable() const;
  void texRegNote(uint64_t key, uint32_t phys, uint64_t len, bool tracked);
  void texCreateFailLog(const char* what, HRESULT hr, const D3D12_RESOURCE_DESC* td, uint32_t base, uint32_t fmt,
                        uint32_t w, uint32_t h);
  // The fence of the frame being recorded, plus one frame of margin: a stamp (and a retire fence) never under-counts
  // the frame a bind's work is submitted in.
  uint64_t texFenceNow() const { return m_fenceValues[m_frameIndex] + 1u; }
  // A bind of a slot: its last-use stamp, and a pin for the draw being assembled.
  void texEvictUse(uint32_t slot) {
    if (slot && slot < m_texSlotUse.size()) {
      m_texSlotUse[slot] = texFenceNow();
      if (m_texPinnedN < 64) m_texPinned[m_texPinnedN++] = slot;
#if SR_ORACLE
      if (m_texSlotEvicted[slot]) texEvictStaleBind(slot);
#endif
    }
  }
  // iter-449y: RESOLVE WRITE-BACK (SR_HLE_RESOLVE_WB) — the console's resolve
  // writes GUEST RAM; the guest CPU-reads the 1x1 luminance resolve to compute
  // adapted exposure, and with no write-back it reads 0 forever and multiplies
  // every frame to black. Small R32F destinations only (the exposure chain).
  struct WbPending { uint32_t key, w, h; DXGI_FORMAT fmt; uint8_t probeOnly = 0; };
  std::vector<WbPending> m_wbPending;
  void serviceResolveWriteback();
  // iter-450c LUMINANCE PROBE (SR_LUM_PROBE=1, diagnostic): snapshot the HDR
  // source's 64x64 center AT EACH IN-FRAME RESOLVE (the only observation
  // point where world content can exist — the composite-back overwrites it
  // before any present boundary). Serviced at present; logs per-resolve
  // means so live mid-frame scene content becomes measurable.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_lumProbeTex[2];
  uint32_t m_lumProbeCount = 0;
  void serviceLumProbe();
  // iter-450e: keys whose LAST upload snapshot was all-zero bytes (the
  // capture-at-bind staleness suspects). Feeds the SR_TEXSLOT_WHITE=zero
  // diagnostic via hleTexWasZeroUpload.
  std::set<uint32_t> m_zeroUploadKeys;
  // iter-450e: EVERY key that ever came through the upload path (resolves
  // never do). Membership = "this is an uploaded texture, not a resolve" —
  // feeds the SR_TEXSLOT_WHITE=u diagnostic (whiten uploads only, keep
  // resolve textures real = the replay's exact texture condition).
  std::set<uint32_t> m_uploadKeys;
 public:
  bool hleTexWasZeroUpload(uint32_t key) const {
    return m_zeroUploadKeys.count(key & 0x1FFFFFFFu) != 0;
  }
  bool hleTexWasUpload(uint32_t key) const {
    return m_uploadKeys.count(key & 0x1FFFFFFFu) != 0;
  }
 private:
  // depth > 1 uploads a volume: `rows` is the TOTAL staged row count across
  // all slices (height*depth for non-block formats); the placed footprint's
  // implied slice pitch (RowPitch*Height) matches the staging layout.
  bool uploadTexture2D(ID3D12Resource* tex, const uint8_t* lin, uint32_t rows,
                       uint32_t rowBytes, uint32_t width, uint32_t height,
                       DXGI_FORMAT dxgi, bool fromShaderResource,
                       uint32_t depth = 1, uint32_t subresource = 0);

  // iter-408b: SR_PRESENT_DUMP — capture the swapchain back buffer as it is
  // handed to Present. The one measurement that says what the screen shows.
  void dumpBackBuffer(ID3D12Resource* rt);
#if SR_ORACLE
  // Step-4 lane C 2i diagnostic (SR_DIAG_HOLD_LOG=1, default off, observe only): while SR_DIAG_SIM_FREEZE holds the
  // sim, every dumped present logs one [HOLD_FRAME] line - its present number and content hash (the dump's fnv), the
  // frame's draw batches, texture / array / volume loads, resolves, shared-memory upload bytes, block invalidations
  // and write-watch faults since the previous present, and the first textures loaded (kind, base, format, size) - so
  // the one-present facade change of 2i can be matched to what the renderer did differently in that frame.
  struct HoldTex {
    char kind;
    uint32_t base, fmt, w, h;
  };
  std::mutex m_holdMu;
  std::vector<HoldTex> m_holdTex;
  uint32_t m_holdTexCount = 0;
  void holdNoteTex(char kind, uint32_t base, uint32_t fmt, uint32_t w, uint32_t h);
  void holdLogPresent(uint64_t pn, uint64_t fnv, uint32_t w, uint32_t h);
#endif
  // iter-464: the host overlay (the mouse cursor, docs/MOUSE_CURSOR.md). Called after
  // dumpBackBuffer() and before Present at every present site, in its own command list,
  // so frame dumps and replay scores never contain it. Records nothing while hidden.
  void drawHostOverlay(ID3D12Resource* rt);
  // dumpBackBuffer(rt) then drawHostOverlay(rt): the order every present site uses.
  // SR_PRESENT_DUMP_WITH_CURSOR=1 (default OFF, review captures only) swaps it so the
  // SR_PRESENT_DUMP frames show the cursor.
  void dumpThenOverlay(ID3D12Resource* rt);
  // iter-465 modlib: SR_MOD_LIBRARY_SHOTS (F12) - writes the composited frame (overlays
  // included) from a readback buffer the overlay list filled.
  void writeOverlayShot(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                        DXGI_FORMAT fmt);
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_overlayRs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_overlayPso;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_overlayAlloc[kFrameCount];
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_overlayList;
  bool m_overlayFailed = false;
  // iter-410 (Xenia parity): blit the guest FRONT BUFFER into the back buffer.
  // Xenia's IssueSwap presents the front buffer as a guest texture, never the
  // render target; ours showed m_geoRT, i.e. only the most recent drain.
  bool blitGuestFramebufferToBackBuffer(const uint8_t* data, int w, int h,
                                        int pitchBytes, bool srcIsHostScratch,
                                        ID3D12Resource* rt);
  // Physical address of the guest front buffer, latched each present so the
  // resolve tally can say whether ANY resolve targets the surface we display.
  uint32_t m_guestFrontPhys = 0;
  // Whether the last present uploaded the guest framebuffer (vs cleared).
  bool m_lastPresentUploaded = false;
  // iter-435d: did any non-resolve batch draw into the LDR geo RT this frame?
  // Present-source selector: movies/direct passes draw to LDR (present the RT,
  // live); UI-era frames route everything through HDR+resolves and the LDR RT
  // stays clear-navy (present the guest FRONT BUFFER the resolves filled —
  // the console's scanout surface). Full-fb A/B stays on SR_PRESENT_FRONTBUF.
  bool m_frameDrewToLdr = false;

#if SR_ORACLE
  // iter-359l: RESOLVE-PRODUCES-TEXTURE (minimal render-to-texture / the EDRAM
  // surface bridge). A mode-6 resolve snapshots the RT into a persistent GPU
  // texture registered by dest base; a later draw sampling that guest address
  // in an EDRAM format the memory decoder can't handle (k_16_16_16_16_EXPAND
  // fmt 29 = the HDR scene color, k_24_8_FLOAT fmt 23 = scene depth) binds
  // THIS texture instead of the white fallback — the fix for the tonemap/
  // composite reading white where the scene should be.
  struct ResolveTex {
    uint32_t basePhys = 0;
    uint32_t slot = 0;
    uint32_t w = 0, h = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
  };
  std::vector<ResolveTex> m_resolveTextures;
  // hdrSource: read the HDR scene RT (R16F texture) vs the LDR RT (RGBA8).
  void snapshotResolveTexture(uint32_t destPhys, uint32_t w, uint32_t h, bool hdrSource);
  int findResolveTexSlot(uint32_t basePhys) const;
  // iter-438d: find-or-create the persistent GPU texture for a resolve dest
  // (creation only, no copy; resource at rest in PIXEL_SHADER_RESOURCE).
  ResolveTex* ensureResolveTex(uint32_t destPhys, uint32_t w, uint32_t h,
                               DXGI_FORMAT fmt);
#endif  // SR_ORACLE
  // Dedicated texture-upload submission path (iter-359f): uploads happen
  // mid-drain while the FRAME command list is recording — resetting that list
  // failed E_FAIL and silently white-cached every texture (recon wf_3fc645cd).
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_texUploadAlloc;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_texUploadList;
  Microsoft::WRL::ComPtr<ID3D12Fence> m_texFence;
  UINT64 m_texFenceValue = 0;
  HANDLE m_texFenceEvent = nullptr;
  // iter-358k: the PendingDraw probe machinery (deferred 128MB pool scans) is
  // DELETED — real draws decode from their own packet + register snapshot.

  // Synchronization.
  Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
  UINT64 m_fenceValues[kFrameCount] = {};
  HANDLE m_fenceEvent = nullptr;
  UINT m_frameIndex = 0;
};

#endif  // _WIN32

}  // namespace gfx
}  // namespace sr
