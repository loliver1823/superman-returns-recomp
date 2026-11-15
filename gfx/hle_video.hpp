#pragma once

// iter-441/442 MIGRATION PHASE 1-2 — the API-boundary HLE renderer.
//
// STRUCTURAL REFERENCE (docs/MIGRATION_API_HLE.md): UnleashedRecomp's
// gpu/video.cpp at reference/UnleashedRecomp/UnleashedRecomp/gpu/video.cpp.
// It is law for the QUEUE / THREAD / DISPATCH / PRESENT-RENDEZVOUS shape.
//
// ⚠ IT IS NOT LAW FOR RESOURCE OWNERSHIP (iter-442, decided with data). The
// reference REPLACES its D3D9 — its hooks never call __imp__, so the title's
// D3D9 never runs and the host owns pitch/staging/handles. WE DELEGATE: every
// hook calls __imp__, the guest's D3D9 keeps running and keeps owning resource
// memory, pitch, lock counts, refcounts and the swap fence. Measured contract:
// ENGINE_MAP §6g.2 (pitch is guest-fixed — a 640-wide chroma plane has pitch
// 768; planes are linear; pixels live ~15MB from the header so a VA->record map
// is mandatory; the guest reports NO dirty span, so Unlock uploads the full
// locked subresource).
//
// MEASURED LOAD (iter-442 census, swap=7500): ~905 draws/frame and ~4,250
// commands/frame. At 32 bytes/command that is ~136KB/frame.

#include <cstdint>
#include <type_traits>
#include <vector>

#include "../kernel/sr_oracle.hpp"

namespace sr {
namespace gfx {

class D3D12Backend;

namespace hle {

// Mirrors the reference's RenderCommandType (video.cpp:811) restricted to the
// Phase-0-named 360 surface. Values are stable log/stat indices — APPEND ONLY.
enum class RenderCommandType : uint32_t {
  Present = 0,            // D3DDeviceSwap_sub_82112050 (S3: becomes ExecuteCommandList)
  Clear,                  // D3DDeviceClear_sub_82101998
  ClearF,                 // D3DDeviceClearF_sub_82101A58
  SetRenderTarget,        // sub_820F2CA0
  SetDepthStencilSurface, // sub_820F2FD0
  SetViewport,            // sub_820F2810
  SetScissorRect,         // sub_820F2720
  SetStreamSource,        // sub_820F2A50
  SetIndices,             // sub_820F2C08
  SetTexture,             // SetTextureFetchConstant sub_82100310
  DrawVertices,           // sub_820FBBF8
  DrawIndexedVertices,    // sub_820FC000
  BeginVertices,          // sub_820FB6E8 (inline/UP)
  EndVertices,            // sub_820FBBA0
  TextureUnlock,          // sub_820FFCC8 — THE upload point (S4 attaches bytes)
  SetShaders,             // S4.5: the draw's VS/PS identity, resolved producer-side
  SetVertexDecl,          // S6c: the guest vdecl element array (attribute map)
  SetConstants,           // S6d: VS/PS ALU float constants from the device shadow
  TextureUpload,          // S6g: general texture pixels, captured AT BIND
  SetRenderState,         // S6h: the packed Xenos register runs (VTE/viewport/
                          //      depth/blend/mask) from the device shadow
  // iter-446 R5: D3DDeviceResolve_sub_8210C5F8. 360 has NO StretchRect, so
  // Resolve IS the RT->texture copy that ends every offscreen pass — and until
  // now it was the ONE D3D9 entry point with no command type at all, i.e.
  // strictly worse off than the nine that merely lacked handlers. ~13/frame
  // across 16 destinations, EVERY ONE of which the guest later SAMPLES.
  Resolve,
  // ── iter-447 S6-UP: the inline/UP draw, published AT ITS DRAIN ─────────────
  // A SEPARATE type from DrawVertices, deliberately, for two reasons that both
  // bite otherwise:
  //  1. THE MOVIE GATE. `isMovieQuad` is `primType==13 && count==4`, and the UP
  //     path emits exactly that shape constantly (the first 12 probe samples
  //     were all prim=13 count=4). Publishing UP as DrawVertices would inflate
  //     mQuads, and `snaps == 3 x mQuads` is the load-bearing invariant that
  //     tells us whether a roll had a movie frame at all. A separate type keeps
  //     that gate honest by construction rather than by a careful predicate.
  //  2. CENSUS SEPARABILITY — drawV vs drawUP stay distinguishable downstream.
  // ⚠ APPENDED, never inserted: every existing type keeps its value, so v5
  // traces remain correctly interpretable and kTraceVersion does NOT need to
  // move (a bump would invalidate every banked .srht fixture for nothing).
  DrawVerticesUP,
  // ── iter-456n (seventh adjudication): TEXTURE OBJECT LIFECYCLE ────────────
  // A texture object was created, destroyed or released at a pixel address.
  // It travels as a COMMAND, not as a direct call, for two reasons and the
  // second is the load-bearing one:
  //  1. THREAD SAFETY. The ownership maps live on the render thread; raising
  //     the event from the guest/API hook was an unsynchronized mutation of a
  //     std::unordered_map that execute() reads concurrently.
  //  2. ORDER. A mutex would fix the race and NOT the semantics: the render
  //     thread lags the producer, so an event raised immediately can overtake
  //     TextureUnlock/TextureUpload commands still queued for the OLD object,
  //     which would then execute under the NEW generation. Serializing it in
  //     the stream is what makes "old object's uploads, then the create, then
  //     the new object's capture" execute in exactly that order — live AND on
  //     replay, because the recorder preserves stream order for free.
  // ⚠ APPENDED, never inserted: every existing type keeps its value.
  TextureObjectEvent,
  // iter-456n (seventh adjudication): PREAMBLE-ONLY. Restores one unlock
  // ownership claim so a replay starts from the same ownership state the live
  // run held when the window opened. Without it the replay's accept/refuse
  // decisions diverge from live on an identical command stream, which makes any
  // ownership-sensitive replay A/B unsound. Never produced by a live hook —
  // only written into a trace preamble and consumed on replay.
  // ⚠ APPENDED, never inserted.
  OwnershipRestore,
  // ── iter-456p Step 3A: THE VERTEX-BUFFER PUBLICATION ─────────────────────
  // A persistent vertex-buffer resource, published ONCE per content generation
  // and thereafter referenced by cheap (resource, offset, stride) binds. This
  // is the reference's resource/view split (video.cpp ProcSetStreamSource binds
  // a persistent host buffer and derives `size = dataSize - offset`), adapted
  // to delegate+mirror.
  //
  // WHY THE WHOLE LOGICAL BUFFER and not `count*stride`: POSITION1 derives its
  // count from the FULL remaining view, and ordinary shaders with a nonzero
  // start offset address a non-linear subset. A draw-extent capture would be
  // correct for neither.
  //
  // WHY ORIGINAL GUEST BYTE ORDER: the payload hash is the artifact's integrity
  // check, and a swapped payload would hash our interpretation of the bytes
  // rather than the bytes. The endian mode travels in the command so the
  // consumer swaps; the record keeps what the guest wrote.
  //
  // ⚠ APPENDED, never inserted: every existing type keeps its value, so a v5
  // trace still decodes. v5 carries NO publications and must replay with
  // streams UNAVAILABLE — never with a fabricated empty buffer.
  VertexBufferPublish,
  Count
};

// ── The trace version, and the v5 contract it encodes ─────────────────────
// Defined HERE rather than in the .cpp so the self-test asserts the same values
// the reader uses; a private copy would let the two drift and the
// v5-compatibility assertion would then pass against nothing.
//
// v6 (iter-456p Step 3A) adds VertexBufferPublish AND makes SetStreamSource a
// consumed binding rather than a dropped record. The bump is about the second
// change: a v5 trace's binds are real and correctly encoded, but the buffers
// they name were never published.
//
// ⛔ v5 MUST STILL REPLAY — with streams UNAVAILABLE, never with a fabricated
// empty buffer. An empty bind is not a degraded version of the real thing; it
// is a different program that draws from zeros and reports success.
// v7 (iter-457 H1a) adds CUBE TextureUpload: a second payload region for the
// mip chain, plus mipLevels / packedMips / mipBytes metadata. The bump is a
// NARROW SEMANTIC GATE, not an envelope change (director ruling, cube r5):
//
//  - the record envelope is UNCHANGED, so v5/v6 keep replaying their
//    representable program byte-identically;
//  - a v5/v6 record CLAIMING a cube upload is REFUSED BY NAME rather than
//    interpreted, because the fields needed to interpret it do not exist in
//    those versions and MUST NOT be guessed;
//  - in v7, absence of a cube upload means ABSENCE. Never synthesize one;
//  - non-cube v7 uploads keep the existing semantics and must never acquire an
//    accidental mip segment.
//
// ⛔ WHY THE GATE IS LOAD-BEARING AND NOT BOOKKEEPING. `mipBytes` occupies the
// textureUpload union's seventh dword, which no previous version wrote — and
// the producer constructs its command as `RenderCommand tc;`, NOT `tc{}`, so
// that dword held INDETERMINATE STACK BYTES and every pre-v7 trace serialized
// them to disk. Reading mipBytes from a v5/v6 record would read that garbage.
// The version check is what makes it unreadable.
//
// v8 (Astra 086) adds the ClearF COLOUR PAYLOAD: a ClearF record now carries
// its kClearFColorBytes float4 as the record payload. The bump is a SAFETY
// BOUNDARY, not bookkeeping. A v7 reader has no ClearF payload arm: it drops
// the payload ("payload DROPPED") and then EXECUTES the record with the
// recorded live-session colorBlock still in it - reading and releasing
// whatever that offset holds in the replay process. A v7 reader must refuse a
// v8 trace outright, before executing any record, and the version check does
// exactly that.
//  - v5..v7 still replay: their ClearF records have no payload, and the v8
//    reader normalises them to "colour MISSING" (never read, never released,
//    never substituted).
//  - The record envelope is unchanged.
constexpr uint32_t kTraceVersion = 8;
constexpr uint32_t kTraceVersionMin = 5;
// Aliases the self-test asserts against, so the contract cannot be edited in
// one place only.
constexpr uint32_t kTraceVersionForTest = kTraceVersion;
constexpr uint32_t kTraceVersionMinForTest = kTraceVersionMin;

// True only when the replayed trace actually carries VertexBufferPublish
// records. Read by the executor before it acts on any stream binding.
#if SR_ORACLE
bool traceStreamsAvailable();

const char* renderCommandName(RenderCommandType t);
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): hle_video.cpp - the HLE renderer, the A/B that draws only under SR_PM4_LIVE=0,
// its render thread, arenas, ownership executor and SRHT recorder / replay - is compiled out (CMakeLists.txt
// ORACLE_SOURCES); the PM4 path draws and presents. The entries the shipping code still calls (the API hooks in
// kernel/hle_api_observe.cpp, the F9 hotkey, phys_window's trace-window probes, the backend's shutdown) are inline
// no-ops: no command is queued, no arena / block slot is granted (kArenaFull / kBlockFull), no recorder opens, every
// count reads 0, ownPresentEnabled() stays true. The test seams are declared only with SR_ORACLE=1.
//
// ⚠ THIS IS NOT THE DEVELOPMENT BUILD'S DEFAULT FOR THE OBSERVER ITSELF. Under the default SR_PM4_LIVE, with every HLE gate
// unset, the dev build still runs observer bookkeeping that the release drops:
//   - each texture create / final release (hle_api_observe.cpp sub_827B9E68, sub_820FFF08, sub_82616C40 under
//     SR_HLE_TEX1640_REG default ON, sub_820F3DA0) calls noteTextureObjectEvent, which enqueues a TextureObjectEvent,
//     starts the render thread (the consumer) on the first one and bumps the queue counters (enqueued / drained /
//     depth / max depth / per type); the consumer applies the event to the UnlockOwnership machine
//     (generation, event counts);
//   - each ClearF (sub_82101A58) takes a small-block slot (blockWrite) that nothing releases while the mirror is off,
//     until the 4096-slot pool is full.
// Nothing in the render or gameplay path reads any of it. With no backend published (main.cpp publishes one only
// under SR_PM4_LIVE=0) the consumer records no GPU work; the ownership machine is read only by the HLE executor's own
// capture / unlock / resolve arms (mirrored commands only) and the SRHT recorder (SR_HLE_TRACE_REC), both compiled out
// here, and by the [S6_OWNERSHIP] log line; the queue counters only by the [HLE_MIRROR] log line and the
// SR_HLE_ARENA_WAIT backpressure (default OFF); the block pool only by the [S4_STAGE] log line (its ClearF handle feeds
// only the mirror's command); and the hooks run the guest body first and ignore the event. So the release matches
// the default for gameplay and rendering, not for the observer's own counters and log lines.
inline const char* renderCommandName(RenderCommandType) { return "?"; }
#endif


// ── The command ─────────────────────────────────────────────────────────────
// POD TAGGED UNION, exactly the reference's shape (video.cpp:844-1004): a type
// tag plus an anonymous union of NAMED per-command structs. Producers write
// `cmd.setStreamSource.stride = s;`, handlers recover with
// `const auto& args = cmd.setStreamSource;`.
//
// ⚠ NO DEFAULT MEMBER INITIALIZERS anywhere in this struct. Two reasons, both
// load-bearing: a union member cannot carry them, and defaulting the tag would
// make any partially-filled command read as Present — i.e. fabricate a frame
// boundary out of a corrupt command. The struct must stay trivially copyable so
// it can be memcpy'd in bulk.
// iter-446 R0 — THE SURFACE HEADER, DECODED.
//
// A D3DSurface is NOT a texture: it carries Xenos REGISTER VALUES, not a
// GPUTEXTURE_FETCH_CONSTANT.
//
//   surface+0  = flags. ⛔ BIT 0x40000000 MEANS "SURFACE OF A TEXTURE LEVEL",
//                and then +24 is a PARENT POINTER and +28 is level<<28 — the
//                whole register decode below is meaningless. REFUSE those.
//   surface+24 = RB_SURFACE_INFO (reg 0x2000): pitch bits 0-13, msaa bits 16-17
//   surface+28 = RB_COLOR_INFO   (reg 0x2001 + index): fmt 16-19, edram 0-11
//   surface+36 = THE TRUE EXTENT: width-1 in bits 18-31, height-1 in bits 3-17
//   surface+44 = the EDRAM TILE FOOTPRINT in bytes (tiles * 5120)
//
// PROVEN by the guest's own accessor, D3DSurface_GetDesc_sub_82100190:
//     if (*(u32*)surf & 0x40000000)  -> defer to the parent texture + level
//     else { desc.Width  = (*(u32*)(surf+36) >> 18) + 1;
//            desc.Height = ((*(u32*)(surf+36) >> 3) & 0x7FFF) + 1;
//            desc.MultiSample = *(u16*)(surf+24) & 3; }
// and by the writer TextureSurfaceFetchConstantBuild_sub_820FF978, which builds
// +36 as `(8*(h-1)) & 0x3FFF8 | ((w-1) << 18)` and +44 as `tiles * 5120`.
// The +24/+28 register identities are proven separately by the shadow indices
// the guest mirrors them into (2560 / 2561) — see ENGINE_MAP §6g.9.
//
// ⛔⛔ iter-446 CORRECTION — §6g.9 SAID "THERE IS NO HEIGHT FIELD" AND THAT WAS
// WRONG. Height was derived as bytes/(pitch*bpp*samples), which returns the
// EDRAM-TILE-PADDED extent (tiles are 80x16) in BOTH axes. It agreed with the
// truth only on surfaces already tile-aligned — which is exactly why a 15/15
// "valid" census never caught it. MEASURED: a 4x4 luminance target decoded as
// 80x16, and 640x360 targets as 640x368. §6g.12.
// ⚠ §6g.6's "a 1280x720 primary decodes as 5753x8161" was the RIGHT FIELD read
// with the TEXTURE 13/13 split instead of the surface 14/15 split.
struct SurfaceDesc {
  uint32_t width = 0;      // TRUE extent, surface+36 bits 18-31 (+1)
  uint32_t height = 0;     // TRUE extent, surface+36 bits 3-17  (+1)
  uint32_t pitch = 0;      // RB_SURFACE_INFO bits 0-13 — the EDRAM stride,
                           // align80(width) (align40 at >1X), NOT the width
  uint32_t paddedRows = 0; // what the old byte-size derivation returns; kept
                           // ONLY so the padding can be reported, never used
                           // as an extent
  uint32_t msaa = 0;       // RB_SURFACE_INFO bits 16-17 (0=1X, 1=2X, 2=4X)
  uint32_t format = 0;     // RB_COLOR_INFO bits 16-19 (colour OR depth enum)
  uint32_t edramBase = 0;  // RB_COLOR_INFO bits 0-11, in tiles
  uint32_t bytes = 0;      // surface+44 — the EDRAM tile footprint
  uint32_t bpp = 0;        // bytes per pixel implied by `format`
  uint32_t samples = 1;    // 1 << msaa
  bool isDepth = false;
  bool valid = false;
};

// Bytes per pixel for a Xenos render-target format. ⚠ Depth surfaces index a
// DIFFERENT enum (0 k_24_8, 1 k_24_8_FLOAT) — never read one with the colour
// table.
inline uint32_t surfaceBytesPerPixel(uint32_t format, bool isDepth) {
  if (isDepth) return 4u;  // both depth formats are 32-bit
  switch (format) {
    case 5:   // k_16_16_16_16
    case 7:   // k_16_16_16_16_FLOAT
    case 15:  // k_32_32_FLOAT
      return 8u;
    default:  // 0/1 8_8_8_8(_GAMMA), 2/3 2_10_10_10(_FLOAT), 4 16_16,
      return 4u;  // 6 16_16_FLOAT, 10/12 the _AS_ variants, 14 32_FLOAT
  }
}

// Pure arithmetic on published scalars — legal on either side of the queue.
// `sizeWord` is surface+36; pass 0 only if it could not be read, and the result
// is then INVALID rather than falling back to the padded derivation.
inline SurfaceDesc decodeSurface(uint32_t surfInfo, uint32_t colorInfo,
                                 uint32_t byteSize, uint32_t sizeWord,
                                 bool isDepth) {
  SurfaceDesc d;
  d.isDepth = isDepth;
  d.pitch = surfInfo & 0x3FFFu;
  d.msaa = (surfInfo >> 16) & 3u;
  d.format = (colorInfo >> 16) & 0xFu;
  d.edramBase = colorInfo & 0xFFFu;
  d.bytes = byteSize;
  d.bpp = surfaceBytesPerPixel(d.format, isDepth);
  d.samples = 1u << d.msaa;
  // The padded derivation, kept ONLY to report how much padding there is.
  const uint32_t row = d.pitch * d.bpp * d.samples;
  if (row && byteSize && (byteSize % row) == 0) d.paddedRows = byteSize / row;
  // THE TRUE EXTENT.
  d.width = (sizeWord >> 18) + 1u;
  d.height = ((sizeWord >> 3) & 0x7FFFu) + 1u;
  // ⚠ sizeWord == 0 is a LEGAL 1x1 surface (this title has one, the last step
  // of its auto-exposure luminance chain), so validity is judged on the DECODED
  // extent, never on the raw word being non-zero. The producer signals a failed
  // READ with 0xFFFFFFFF, which decodes to 16384x32768 and fails the bounds
  // test below on its own.
  d.valid = d.width <= 8192u && d.height <= 8192u && d.pitch != 0u &&
            d.pitch >= d.width;
  return d;
}

// iter-450O: bit 8 of draw.primType marks "this list was CONVERTED from a
// restart strip" (the SR_S6_IDX gather sets it at publish; buildDraw strips
// it into HleDraw::fromStrip). It rides inside the existing field so the
// trace format is unchanged — pre-450O fixtures simply read as unmarked.
// Guest prim types are < 32, so the bit can never collide.
constexpr uint32_t kPrimStripConvertedBit = 0x100u;

constexpr uint32_t kResolveFmtShift      = 26;   // in destSize
constexpr uint32_t kResolveFmtMask       = 0x3Fu;
constexpr uint32_t kResolveSizeMask      = 0x03FFFFFFu;
constexpr uint32_t kResolvePitchShift    = 17;   // in levelSliceFlags
constexpr uint32_t kResolvePitchMask     = 0x1FFu;
constexpr uint32_t kResolveTiledBit      = 1u << 26;
constexpr uint32_t kResolveEndianShift   = 27;
constexpr uint32_t kResolveEndianMask    = 0x3u;
constexpr uint32_t kResolveDescValidBit  = 1u << 29;

struct RenderCommand {
  RenderCommandType type;
  union {
    // iter-448: frontKey/frontDims — the guest's OWN answer to "which pixels
    // does this Swap present". The front-buffer object (front=82998720/839F5CF0)
    // embeds a texture fetch constant at +28: f[1] basePage names the pixel
    // memory, f[2] the dims (proven on the golden dump: fmt 6, 1280x720).
    // Read kernel-side at the hook (the render thread never touches guest
    // memory) and published normalized; 0 = header unreadable.
    struct { uint32_t device, front, rect, frontKey, frontDims; } present;
    struct { uint32_t device, count, rects, flags, color, z, stencil; } clear;
    // iter-445: `color` was a RAW GUEST VA and `z` a literal 0. Both are now
    // decoded PRODUCER-side: the float4 goes through the small-block pool
    // (HDR render targets mean a clear value can exceed [0,1], so packing to
    // D3DCOLOR would be lossy), and z carries the float bits from f1.
    // 7 dwords — joint-widest with `clear`, so RenderCommand is still 32 bytes.
    // Astra 085: colorBytes is EXACTLY kClearFColorBytes (one float4, owned
    // by this command) or 0 (no colour). The recorder serializes those 16
    // bytes as the record payload and the replayer restages them into a block
    // it owns; every other shape is refused by name.
    struct {
      uint32_t device, flags, rect, colorBlock, colorBytes, z, stencil;
    } clearF;
    // iter-446 R0: `surface` stays — it is the surface's IDENTITY (the key the
    // host resource map is built on), not a pointer anyone dereferences. What
    // is new is the three header dwords beside it, read producer-side where
    // guest memory is legal, so the render thread can size and format the host
    // resource without ever touching the guest. RAW on purpose: `decodeSurface`
    // above turns them into a SurfaceDesc, and keeping the raw values in the
    // stream means the fixture replays real headers and the decode stays
    // testable offline. 6 dwords.
    struct {
      uint32_t device, index, surface, surfInfo, colorInfo, byteSize, sizeWord;
    } setRenderTarget;                              // 7 dwords — at the cap
    struct {
      uint32_t device, surface, surfInfo, colorInfo, byteSize, sizeWord;
    } setDepthStencilSurface;
    // iter-446 R0b: these carried a RAW GUEST VA — the same defect class as
    // `clear.z = 0`, and unusable by construction on the render thread.
    // D3DVIEWPORT9 is {DWORD X,Y,Width,Height; float MinZ,MaxZ}; minZ/maxZ
    // carry FLOAT BITS. 7 dwords — at the union cap.
    struct {
      uint32_t device, x, y, width, height, minZ, maxZ;
    } setViewport;
    struct { uint32_t device, left, top, right, bottom; } setScissorRect;
    // iter-446 R5. Args verified against the recompiled PPC prologue:
    // (r3 dev, r4 Flags, r5 pSourceRect, r6 pDestTexture, r7 pDestPoint,
    //  r8 DestLevel, r9 DestSliceOrFace, r10 pClearColor, f1 ClearZ).
    //
    // Both ENDS are resolved to KEYS producer-side, which is the whole point:
    //  * `srcSurface` — `Flags & 7` selects the source from the device's own
    //    surface array (RT0..3 at dev+12168+4*idx, depth at slot 4 = dev+12184),
    //    so the producer reads the ACTUAL surface VA and the render thread never
    //    needs the device.
    //  * `destKey` — `*(pDestTexture+32) & 0xFFFFF000`, the fetch-constant base
    //    page. ⚠ THIS MASK MUST MATCH THE BIND KEY EXACTLY. iter-445 lost an
    //    iteration to bind/upload keying the same texture two different ways;
    //    a resolve stored under a key nothing samples is that bug again with a
    //    longer fuse.
    // 7 dwords — at the union cap, so the rect and point are PACKED 16:16 and
    // clamped to [0,65535]; a resolve rect is never negative and never exceeds
    // a 8192-pixel surface, so the clamp is not reachable in practice and is
    // there to make a corrupt value obvious rather than wrap.
    // ⛔ iter-446 §6g.12: `flags` became `destSize` because the DESTINATION's
    // true extent is the one thing the source surface cannot supply. The
    // surface's byte size is EDRAM-TILE-PADDED (tiles are 80x16), so deriving
    // the destination from it makes every 360-tall target 368 tall — 8 garbage
    // rows and a 2.2% vertical stretch on everything that samples it. The dest
    // texture's own fetch constant states the real size:
    //   width = (destSize & 0x1FFF) + 1,  height = ((destSize >> 13) & 0x1FFF) + 1
    // The flag bits the executor actually needs are folded into `levelSlice`,
    // which had 16 bits going spare (level is <= 15, slice <= 255):
    //   bits 0-3 level | 4-11 slice | 12-14 Flags&7 | 15 clear-RT | 16 clear-Z
    struct {
      uint32_t srcSurface, destKey, destSize, rectLT, rectRB, destPoint,
               levelSliceFlags;
    } resolve;
    // ── slice 6 / Class B v2: THE RESOLVE DESTINATION'S REAL DESCRIPTOR ──────
    // Resolve authority must be scoped to the same (key, descriptor epoch,
    // generation) a capture is judged against, or every post-resolve capture
    // reads as a recreation and silently overwrites the render product. The
    // executor cannot compute that epoch — it never sees the guest header — but
    // the PRODUCER can, exactly as the widened unlock publish does. The
    // destination's fetch constant lives at `pDestTexture+28`, and this hook
    // already reads two of its dwords (+32 for the base page, +36 for the
    // size). The remaining fields ride in bits that were free:
    //   destSize[25:0]        the existing 13/13 size split (unchanged)
    //   destSize[31:26]       format
    //   levelSliceFlags[25:17] pitchTexels >> 5
    //   levelSliceFlags[26]    tiled
    //   levelSliceFlags[28:27] endian
    //   levelSliceFlags[29]    DESCRIPTOR VALID
    // ⚠ Written ONLY when SR_HLE_RESOLVE_OWNERSHIP_V2 is on, so the gate-OFF
    // stream keeps its historical packing byte-for-byte. A legacy trace has the
    // valid bit clear and is handled by its own fallback, never by inventing a
    // descriptor.
    struct { uint32_t device, stream, buffer, offset, stride, dirty; } setStreamSource;
    struct { uint32_t device, buffer; } setIndices;
    struct { uint32_t device, sampler, texture, dirty; } setTexture;
    // iter-456n: `key` is the PIXEL address (normalized owner-side, so either
    // guest alias window is accepted). `reason` names the route so a census can
    // tell creation from destruction: 0 = create, 1 = destroy/release.
    struct { uint32_t key, reason; } textureObjectEvent;
    // 6 dwords: the 64-bit descriptor signature is carried as two halves.
    struct {
      uint32_t key, generation, sigLo, sigHi, publishes, reserved;
    } ownershipRestore;
    // iter-456p Step 3A. Seven dwords is the whole union budget, so every
    // field here earns its place:
    //   objectVA    the guest D3DVertexBuffer OBJECT VA a binding names. Replay
    //               maps objectVA -> resource, because bindings are recorded at
    //               the SetStreamSource call and carry only the object.
    //   key         the NORMALIZED physical base (low 29 bits), the resource
    //               identity shared with the [S6_UNIQ]/vbCache key.
    //   sizeBytes   the LOGICAL buffer size, `be32(vb+0x1C) & 0x03FFFFFC`.
    //   endAliasGen THREE fields packed into one dword, because the union has
    //               exactly seven and the payload's arena offset must have one:
    //                 bits 0-1   endian mode
    //                 bits 8-9   WHICH guest alias window supplied the bytes
    //                            (0 = 0x8, 1 = 0xA). ⛔ RECORDED, never
    //                            inferred: our harness does not fold the
    //                            windows (iter-425), a buffer written through
    //                            0xA reads all zero at 0x8, and a silent choice
    //                            there blacked out a whole era (the vbc3
    //                            lesson).
    //                 bits 16-31 generation, which bumps on every content
    //                            change at the same key so a reused address
    //                            cannot alias two different buffers. 16 bits
    //                            SATURATES rather than wrapping — a wrap would
    //                            make a changed buffer read as unchanged.
    //   arenaOffset where the producer parked the bytes; the recorder reads it
    //               to attach the payload, exactly as TextureUpload does.
    //   digestLo/Hi the ORIGINAL-BYTE content digest that decided publication.
    //               ⚠ Deliberately a DIFFERENT function from the record tail's
    //               XXH3 payload hash, so a corrupt payload cannot agree with
    //               its own digest by construction.
    // Payload = the whole logical buffer, original guest byte order.
    struct {
      uint32_t objectVA, key, sizeBytes, endAliasGen, arenaOffset,
               digestLo, digestHi;
    } vertexBufferPublish;
    // The three draw emits share a shape: (primType, start/base, count).
    // S4.3 appends the producer-side VERTEX snapshot, same contract as the
    // plane pixels: an arena offset, never a guest VA. vertBytes==0 means the
    // draw was not snapshotted (too large — see kMaxVertSnapshot) and so cannot
    // be rendered from the trace. 7 dwords: at the union cap, zero slack.
    struct {
      uint32_t device, primType, start, count, vertArena, vertBytes, stride;
    } draw;
    struct { uint32_t device; } endVertices;
    // S4.5: shader identity, hashed on the PRODUCER side. It cannot be resolved
    // later — the hash is computed from ucode in GUEST memory, which the render
    // thread must never touch. Carried as two halves because the union is
    // uint32_t-based; reassembled with (uint64_t)hi<<32 | lo.
    struct { uint32_t device, vsLo, vsHi, psLo, psHi; } setShaders;
    // S6c: the guest's vertex declaration, snapshotted producer-side. Without
    // it a packed guest vertex cannot be mapped onto the translated VS's
    // float4-per-attribute input layout — the movie quad only worked because
    // its layout was measured and hardcoded.
    struct { uint32_t device, arenaOffset, bytes, elemCount; } setVertexDecl;
    // S6d: VS/PS ALU float constants. These are ZERO today, which is fine for
    // the movie quad (its vertices are already in clip space) and fatal for
    // everything else — every other draw reads them for its transform.
    struct {
      uint32_t device, vsArena, vsBytes, psArena, psBytes;
    } setConstants;
    // S6h: the guest's packed register runs, snapshotted producer-side. The
    // 0x2100 run (dev+10316) and the 0x2200 run (dev+10420) are CONTIGUOUS, so
    // one 236-byte block carries RB_COLOR_MASK, the whole PA_CL_VPORT_* set,
    // RB_DEPTHCONTROL, RB_BLENDCONTROL0 and PA_CL_VTE_CNTL together.
    struct { uint32_t device, blockOffset, bytes; } setRenderState;
    // S6g: a GENERAL texture's pixels, snapshotted producer-side at BIND.
    // WHY AT BIND and not at Unlock — measured, and it is iter-443's
    // vertex-buffer answer one layer over: 883 distinct textures are BOUND in a
    // run against texCreate=104 and 133 non-movie Unlocks, so ~85% of them never
    // pass through D3D9's lock path at all (RW4 arena assets are used in place).
    // Unlock can only ever be an INVALIDATION HINT for textures.
    // Packed to 6 dwords: `dims` = width<<16|height (both <=4096 so 13 bits
    // suffice), `fmtBits` = fmt | tiled<<8 | endian<<12. The render thread needs
    // every one of these to untile/swap/decode without touching guest memory.
    // iter-457 H1a: `mipBytes` is the SEVENTH and last union dword (`draw`
    // already sits at that cap). It is the length of the SECOND payload
    // region — the mip chain from the guest's `mip_address` — and is nonzero
    // ONLY for a v7 CUBE upload. `bytes` remains the BASE region's length, so
    // the serialized payload is `bytes + mipBytes` and the two segments split
    // back on those recorded counts, exactly as SetConstants splits VS from PS.
    struct {
      uint32_t key, arenaOffset, bytes, dims, fmtBits, pitch, mipBytes;
    } textureUpload;
    // S4.2: the pixels are SNAPSHOTTED producer-side (see the arena below), so
    // this carries an arena offset, never a guest VA. `bytes` is 0 when there
    // was nothing to snapshot (not a movie plane, or the arena rejected it) —
    // the command still publishes so [HLE_MIRROR] counts stay 1:1 with the API.
    // EXACTLY 7 dwords: this is joint-widest with `clear`, so RenderCommand is
    // still 32 bytes. There is ZERO slack — an 8th field costs 4KB/frame.
    struct {
      uint32_t texture, arenaOffset, bytes, pitch, width, height, level;
    } textureUnlock;
  };
};

static_assert(std::is_trivially_copyable<RenderCommand>::value,
              "RenderCommand must be trivially copyable for bulk transfer");
static_assert(sizeof(RenderCommand) <= 32,
              "RenderCommand grew — at ~4,250 cmds/frame every byte is 4KB/frame");

// ── S4.2: THE PRODUCER-SIDE PIXEL ARENA ─────────────────────────────────────
// Enforces this file's HARD RULE (the render thread never reads guest memory).
// The FMV decoder rewrites the SAME plane allocation every frame (ENGINE_MAP
// §6g.2), and S3 measured a queue depth around 1,900 commands — so a render
// thread that dereferenced the guest pointer later would legitimately read a
// LATER frame's bytes. The producer snapshots the locked subresource at Unlock
// into this host-owned arena; the command carries an offset into it.
//
// FIXED-SIZE SLOTS, not a bump ring. A movie plane is at most 921,600 bytes, so
// slots make reclamation one counter with no straddle/padding bookkeeping, and
// a slot address is stable for the render thread's whole execute() call. The
// cost is internal fragmentation on the 276,480-byte chroma planes — 48 MB of
// host RAM buys ~16 movie frames in flight, against a present rendezvous that
// keeps the guest at most ONE frame ahead.
//
// MEASURED (iter-442 S4.2, 200 s roll): 2,184 snapshots / 1,023 MB moved, peak
// 3 slots in flight — exactly one movie frame, i.e. 16x headroom — and ZERO
// rejects. The invariant that validates the whole model: snaps == 3 x movie
// quad draws, exactly, over the entire run.
//
// A request that does not fit (too large, or every slot in flight) is REJECTED
// and counted — never truncated. Silently short pixel data is indistinguishable
// from a translation bug, which is exactly the failure this slice must not have.
constexpr uint32_t kArenaSlotBytes = 1u << 20;   // 1 MB
constexpr uint32_t kArenaSlots = 48;             // 48 MB total
constexpr uint32_t kArenaFull = 0xFFFFFFFFu;     // allocation failed

// S4.3: the largest vertex block a draw may snapshot. The movie quad is 4 x 36
// = 144 B; general geometry runs to thousands of vertices and snapshotting all
// ~905 draws/frame would move gigabytes a second. This is a RESOURCE bound, not
// a content special-case, and skipped draws are COUNTED and logged rather than
// silently dropped (a silent cap reads as "covered everything" when it did not).
// S6 replaces this with a real vertex-buffer cache.
constexpr uint32_t kMaxVertSnapshot = 4096;

// Producer side: copy `bytes` from host memory into a free slot. Returns the
// arena offset, or kArenaFull. The caller MUST publish a command carrying the
// offset, or the slot leaks — the queue is the only thing that frees it.
#if SR_ORACLE
uint32_t arenaWrite(const void* src, uint32_t bytes);
#else
inline uint32_t arenaWrite(const void*, uint32_t) { return kArenaFull; }
#endif

// ── iter-444 S6e: THE SMALL-BLOCK POOL ──────────────────────────────────────
// WHY A SECOND POOL RATHER THAN MORE ARENA SLOTS (measured, not stylistic).
// iter-443 S6c/S6d put the vertex DECL (36 bytes) and the ALU CONSTANTS
// (4,096 + 3,584 bytes) through the 1 MB-slot pixel arena, one allocation each
// per draw-that-carries-vertices. Two independent faults, and the first hid
// the second:
//   1. execute() never released them (they fell through to `default:`), so
//      three slots leaked per qualifying draw.
//   2. Even released, a 36-byte decl in a 1 MB slot is 29,000x internal
//      fragmentation — 48 MB of arena buys 16 draws in flight.
// MEASURED on the HEAD before this fix (natural K=1, 200 s): peak=48/48,
// inFlight pinned at 44-47, rejects=2,320,314, GATE=MOVIE-BUT-ARENA-LAPPED,
// coverage 732/5,606,612 = 0.013%. The iter-442 baseline for the SAME roll
// shape was peak=3, rejects=0. Pixel data was being dropped wholesale.
//
// Small blocks are a different size class, so they get their own pool sized
// from the demand: the producer runs at most ~1,900 commands ahead of the
// render thread (S3), and a draw publishes at most 3 blocks, so 1,024 blocks
// is comfortable headroom. 8 KB covers the largest block (the 4,096-byte VS
// constant file) with room for the whole float4 file to grow.
//
// Same hard contract as the arena: every published offset MUST be released by
// the executor, a request that does not fit is REJECTED and counted, never
// truncated, and the render thread reads only these HOST bytes — never guest
// memory.
constexpr uint32_t kBlockBytes = 8192;
// 1,024 was sized from "the producer runs ~1,900 commands ahead". That held
// until S6f widened the shape gate: recording ~300 draws/frame slows the render
// thread, the producer gets further ahead, and a wide roll MEASURED peak
// 1024/1024 with 45 rejects (earlier wide rolls peaked at 193 and 333 — it is
// roll-dependent, which is exactly why headroom rather than the observed max).
constexpr uint32_t kBlockCount = 4096;           // 32 MB total
constexpr uint32_t kBlockFull = 0xFFFFFFFFu;
// Astra 085: the ClearF colour is exactly one float4 (HDR-capable, never
// packed). The one size the recorder, the replayer and the executor accept.
constexpr uint32_t kClearFColorBytes = 16u;

#if SR_ORACLE
uint32_t blockWrite(const void* src, uint32_t bytes);
const uint8_t* blockRead(uint32_t offset);
void blockRelease(uint32_t offset);
#else
inline uint32_t blockWrite(const void*, uint32_t) { return kBlockFull; }
#endif

// ── iter-444 S6f: SIZE-ROUTED SNAPSHOTS ─────────────────────────────────────
// A draw's VERTEX snapshot is capped at kMaxVertSnapshot (4 KB) yet used a
// whole 1 MB arena slot — the same 250x internal fragmentation that exhausted
// the pool for the 36-byte declaration, just less extreme. It did not show
// until S6f widened the shape gate: recording ~300 draws/frame instead of 1
// slows the render thread, the producer runs further ahead, and 48 slots ran
// out (MEASURED: rejects=4,092, peak=48/48, GATE=ARENA-LAPPED — while the
// block pool sat at peak 144 of 1,024).
//
// So route by SIZE and TAG the offset, because the consumer must know which
// pool to read and release. The tag is a high bit: both pools' offsets are far
// below 2^31 (48 MB and 8 MB), and kArenaFull is checked first so the all-ones
// sentinel is never mistaken for a tagged offset.
// Movie planes (up to 921,600 B) still take the 1 MB arena, which is what it
// was built for.
constexpr uint32_t kPoolBlockFlag = 0x80000000u;
#if SR_ORACLE
uint32_t poolWrite(const void* src, uint32_t bytes);
const uint8_t* poolRead(uint32_t tagged);
void poolRelease(uint32_t tagged);
#else
inline uint32_t poolWrite(const void*, uint32_t) { return kArenaFull; }
inline const uint8_t* poolRead(uint32_t) { return nullptr; }
inline void poolRelease(uint32_t) {}
#endif

// ── iter-449c: THE GEOMETRY POOL — the fourth size class ────────────────────
// The UP producer (spans to 207,360 B) and the indexed gather (measured max
// ~245 KB) both routed >8 KB spans into the 1 MB-slot MOVIE-PLANE arena, and
// under the full armed stack the two producers plus the planes overran its 48
// slots every roll (rejects 330→1,189, flagged ARENA-LAPPED). A dropped
// gather is a missing draw, which presents as FRAME-TO-FRAME FLICKER — the
// land1 roll's "one bright menu frame in eight" shape. Geometry gets its own
// slots so producer traffic can never again evict a movie plane (or vice
// versa); spans over 256 KB (rare, none measured) still take the arena and
// stay counted by the producers' arena-write terms.
// Tag bit: block=bit31, geo=bit30 — arena (48 MB), geo (64 MB) and block
// (32 MB) offsets all stay far below either bit, and kArenaFull is checked
// before any flag test as before.
constexpr uint32_t kGeoSlotBytes = 256u << 10;  // 256 KB
constexpr uint32_t kGeoSlots = 256;             // 64 MB total
constexpr uint32_t kPoolGeoFlag = 0x40000000u;
#if SR_ORACLE
uint32_t geoWrite(const void* src, uint32_t bytes);
const uint8_t* geoRead(uint32_t offset);
void geoRelease(uint32_t offset);
#endif

// ── iter-444 S6g: THE TEXTURE POOL ──────────────────────────────────────────
// A THIRD size class, and by now that is the established answer rather than a
// new idea: a 36-byte decl, a 4 KB vertex snapshot, a 900 KB movie plane and a
// 3.5 MB texture do not belong in the same slot size (three pool exhaustions
// this iteration, all from getting that wrong).
//
// SIZED FROM THE CENSUS: maxMB=3.52 measured, and the TILED source span is
// larger than width*height*bpp because untiling reads
// blockPitch * alignUp(rows,32) * blockBytes — so 8 MB slots, not 4. Captures
// are RARE (883 distinct keys across a whole run, ~4 textures live per frame),
// so 8 slots is ample; anything larger is REJECTED and COUNTED, never truncated.
constexpr uint32_t kTexSlotBytes = 8u << 20;    // 8 MB
constexpr uint32_t kTexSlotCount = 8;           // 64 MB
// ── iter-456n: Class-A snapshot pool (widened Unlock publishes) ─────────────
// Bounded, VARIABLE-SIZE and strictly isolated from the bind-capture texture
// pool, so a widened Unlock can never be refused because bind capture filled a
// fixed slot table (fifth adjudication, failed bar 4). Handles carry
// kClassAHandleFlag and are NOT byte offsets — read/release them only through
// the classA* calls.
constexpr uint32_t kClassAHandleFlag = 0x40000000u;   // distinct from kPool* tags
// iter-456n: TextureUpload.fmtBits bit 26 = UNLOCK-ORIGIN. It is not a format
// bit; it says the pixels came from a guest Unlock (Class A's widened publish)
// rather than from a bind capture, and it is what the executor, the recorder
// and the replay all route on. Named once here because it was a bare
// `1u << 26` in three files, which is precisely how the recorder came to read
// the wrong pool for exactly these records.
constexpr uint32_t kUploadUnlockOriginBit = 1u << 26;

// ── iter-457 H1a: THE CUBE FIELDS OF fmtBits ─────────────────────────────────
// Existing allocation: bits 0-7 format, 8 tiled, 12-13 endian, 14-15 texDim,
// 16-25 depth-1, 26 unlock-origin. The cube slice claims the HIGH group:
//
//   bits 27-29  mipLevels - 1   (0..7; 0 means base only)
//   bit  30     packedMips      (the Xenos f[5] bit 11)
//
// ⛔ NOT bits 9-11, which an earlier design named: mipLevels-1 needs three bits
// and packedMips a fourth, which would have run into the endian field at 12.
//
// ⛔ packedMips IS A NAMED FIELD AND IS NEVER INFERRED. It decides how the
// recorded mip REGION is decoded — with it set, every level at or past
// GetPackedMipLevel shares one tail; without it each level stands alone. Two
// textures with identical mipBytes can need different decodes, so deriving it
// from a byte count is unsound. It belongs in descriptor and ownership identity
// beside format, dims and tiling, or a re-created texture that flips only this
// bit silently inherits the previous decode.
constexpr uint32_t kUploadMipLevelsShift = 27;
constexpr uint32_t kUploadMipLevelsMask  = 7u;
constexpr uint32_t kUploadPackedMipsBit  = 1u << 30;
constexpr uint32_t kUploadTexDimShift    = 14;
constexpr uint32_t kUploadTexDimCube     = 3u;

inline uint32_t uploadTexDim(uint32_t fmtBits) {
  return (fmtBits >> kUploadTexDimShift) & 3u;
}
inline bool uploadIsCube(uint32_t fmtBits) {
  return uploadTexDim(fmtBits) == kUploadTexDimCube;
}
// COUNT, not the stored index: 1 = base only.
inline uint32_t uploadMipLevels(uint32_t fmtBits) {
  return ((fmtBits >> kUploadMipLevelsShift) & kUploadMipLevelsMask) + 1u;
}
inline bool uploadPackedMips(uint32_t fmtBits) {
  return (fmtBits & kUploadPackedMipsBit) != 0;
}

// Which POOL holds a TextureUpload's bytes. Named once and used by the
// executor, the recorder and the replay staging, because the recorder used to
// answer it privately (texRead unconditionally) and got it wrong for every
// Class-A record - they were written payloadless and silently de-fanged.
// The HANDLE is the ground truth: it is what the allocator issued. Bit 26 is
// provenance carried alongside, so a disagreement between the two is a real
// defect and is worth reporting rather than silently resolving.
inline bool uploadIsClassA(uint32_t handle) {
  return handle != kArenaFull && (handle & kClassAHandleFlag) != 0;
}
inline bool uploadFlagAgrees(uint32_t handle, uint32_t fmtBits) {
  return uploadIsClassA(handle) == ((fmtBits & kUploadUnlockOriginBit) != 0);
}
constexpr uint64_t kClassABudgetBytes = 64ull << 20;  // 64 MB total live
constexpr uint32_t kClassAMaxBytes = 16u << 20;       // 16 MB per snapshot
constexpr size_t   kClassAMaxBufs = 4096;             // hard buffer-count bound

struct ClassAStats {
  uint64_t writes, bytes, rejects, doubleFree, badHandle, liveBytes, budget;
  uint32_t bufs, inFlight, peak;
};

#if SR_ORACLE
uint32_t classAWrite(const void* src, uint32_t bytes);
// iter-458: the single-attempt writers (never wait); the public writers wrap
// them with SR_HLE_ARENA_WAIT backpressure.
uint32_t classAWriteOnce(const void* src, uint32_t bytes);
uint32_t texWriteOnce(const void* src, uint32_t bytes);
uint32_t arenaWriteOnce(const void* src, uint32_t bytes);
uint32_t blockWriteOnce(const void* src, uint32_t bytes);
uint32_t geoWriteOnce(const void* src, uint32_t bytes);
uint32_t poolWriteOnce(const void* src, uint32_t bytes);
#else
inline uint32_t classAWrite(const void*, uint32_t) { return kArenaFull; }
#endif
// iter-458 (Astra 017): staging backpressure, SR_HLE_ARENA_WAIT (default OFF).
// Pools: 0 block, 1 geo, 2 arena, 3 tex, 4 classA. `exhausted` = capacity
// failures of single attempts, `invalid` = bad input; both cumulative from boot
// and counted with the gate on or off.
struct ArenaWaitStats {
  uint64_t exhausted[5], invalid[5];
  uint64_t waits, waitsOk, waitUsTotal, waitUsMax, timeouts, noProgress,
      renderRefusals, fails;
};
#if SR_ORACLE
bool arenaWaitEnabled();
ArenaWaitStats arenaWaitStats();
void arenaWaitResetForTest();
void arenaWaitSetEnabledForTest(int v);              // -1 = use the env gate
void arenaWaitSetPendingProbeForTest(bool (*fn)());  // null = queue depth
void arenaWaitSetFailHookForTest(void (*fn)(const char* reason, const char* pool,
                                            uint32_t bytes, uint64_t waitedUs));
void arenaWaitSetTimeoutMsForTest(uint32_t ms);
void arenaWaitMarkRenderThreadForTest(bool on);
const uint8_t* classARead(uint32_t handle);
void classARelease(uint32_t handle, uint32_t bytes);
ClassAStats classAStats();
#else
inline ClassAStats classAStats() { return {}; }
#endif

#if SR_ORACLE
uint32_t texWrite(const void* src, uint32_t bytes);
const uint8_t* texRead(uint32_t offset);
void texRelease(uint32_t offset);
#else
inline uint32_t texWrite(const void*, uint32_t) { return kArenaFull; }
#endif

// ── iter-449L: THE VB RESIDENCY CACHE (SR_S6_VBCACHE, default OFF) ─────────
// The reference's buffer model, adapted to delegate+mirror: ONE host-resident
// copy of each guest vertex buffer, keyed (physBase,size) — the key
// [S6_UNIQ] has computed since iter-443 — uploaded ONCE and referenced by
// every draw over it. Retires kMaxVertSnapshot (which suppressed the
// 1.44M-draw big-vertex world-mesh class — proven by the vcap1 building
// frame) AND the per-draw copy volume that lapped the arena when the cap was
// raised (rejects=970). This slice covers plain DrawVertices only: the
// indexed gather's output is index-dependent and UP data is inline — both
// stay on the pools, which are the right tool for per-draw-unique bytes.
//
// STATIC vs DYNAMIC is decided by OBSERVED BEHAVIOR, not guessed: every
// reuse runs a sparse content probe (first/mid/last 32 B — full compare for
// small buffers) against guest memory. A mismatch re-uploads as a NEW entry;
// a key that mutates more than kVbCacheMaxReuploads times is demoted
// VOLATILE and permanently falls back to the pool-snapshot path. Counted,
// never silent.
//
// LIFETIME SAFETY (the reason this is append-only): the executor consumes
// raw pointers into entries while the queue holds up to ~1,900 commands.
// Entries are NEVER freed, NEVER moved, NEVER rewritten in place — a
// mutation allocates a fresh entry and orphans the old one (orphaned bytes
// are the price of not double-buffering; `deadMB` reports it). Capacity is
// a hard cap with NAMED rejects; a rejected acquire falls back to the pool
// path, so coverage degrades honestly instead of dangling.
//
// The tag: BOTH pool flag bits set (block|geo cannot co-occur), entry id in
// the low bits. kArenaFull (all-ones) is checked before any tag test as
// always. Producers publish it in draw.vertArena; poolRead/poolRelease
// route on it (release is a no-op — residency is the point).
constexpr uint32_t kPoolResidentFlag = 0xC0000000u;
constexpr uint32_t kVbCacheEntries = 1024;
constexpr uint64_t kVbCacheEntryCap = 16ull << 20;   // per-buffer
constexpr uint64_t kVbCacheBudget = 384ull << 20;    // total resident bytes
constexpr uint32_t kVbCacheMaxReuploads = 4;

// Producer side (guest thread). Looks up key, probes content on hit,
// uploads (endian-swapped per `endian`, same 8IN32 rule as the pools) on
// miss or mutation. Returns the RESIDENT TAG (kPoolResidentFlag|id), or
// kArenaFull when the key is volatile / caps are hit / src unreadable —
// caller falls back to the pool-snapshot path.
// ⛔ vbc3 lesson (a BLACK FE era, starfield included): `src` and `srcAlt`
// are the SAME buffer through the 0x8 and 0xA physical windows, which our
// harness does NOT fold (iter-425). The pool path chose per DRAW RANGE; a
// head-only probe here uploaded 4 MB of zeros for every 0xA-written buffer
// and every draw over it went degenerate. The cache chooses at UPLOAD by
// whole-buffer scan (early-exit, ~330 uploads/run), locks the choice into
// the key, and probes reuse against the CHOSEN window.
#if SR_ORACLE
uint32_t vbCacheAcquire(uint64_t key, const uint8_t* src,
                        const uint8_t* srcAlt, uint32_t bytes,
                        uint32_t endian);
// Render-thread side: stable pointer + size for a resident tag's entry, or
// null if the id is invalid (refused and counted by the caller).
const uint8_t* vbCacheData(uint32_t tagged, uint32_t* sizeOut);
#else
inline uint32_t vbCacheAcquire(uint64_t, const uint8_t*, const uint8_t*, uint32_t, uint32_t) {
  return kArenaFull;
}
#endif

struct VbCacheStats {
  uint64_t hits, uploads, mutations, volatiles, rejects;
  uint64_t liveBytes, deadBytes;
  uint32_t entries;
};
#if SR_ORACLE
VbCacheStats vbCacheStats();
#else
inline VbCacheStats vbCacheStats() { return {}; }
#endif

// Render-thread side: the snapshot, valid until arenaRelease(offset).
#if SR_ORACLE
const uint8_t* arenaRead(uint32_t offset);
void arenaRelease(uint32_t offset);
#else
inline const uint8_t* arenaRead(uint32_t) { return nullptr; }
#endif

// `writes` is THE MOVIE-ACTIVITY GATE for a first-light verdict. Movie decode
// is still roll-dependent (the open no-movie class, iter-440q): on a roll that
// decoded nothing there are no Locks at all, so a perfectly correct renderer
// draws black. Darkness only means something when writes > 0 on the SAME roll.
struct ArenaStats {
  uint64_t writes;    // successful snapshots
  uint64_t bytes;     // total snapshotted
  uint64_t rejects;   // too large, or all slots in flight (must stay 0)
  uint32_t inFlight;  // slots held right now
  uint32_t peak;      // high-water slot count
};
#if SR_ORACLE
ArenaStats arenaStats();

// Same shape for the small-block pool. `rejects` must stay 0 for the same
// reason: a dropped constant block silently renders the PREVIOUS draw's
// transform, which looks like a geometry bug rather than a resource bug.
ArenaStats blockStats();

// And for the texture pool (S6g). A reject here means a texture was never
// captured, so every draw sampling it gets the 1x1 white fallback — visible as
// flat geometry, which is exactly the symptom this slice exists to remove.
ArenaStats texStats();

// And for the geometry pool (iter-449c). A reject here is a dropped draw —
// visible as frame flicker, the exact symptom the pool exists to remove.
ArenaStats geoStats();
#else
inline ArenaStats arenaStats() { return {}; }
inline ArenaStats blockStats() { return {}; }
inline ArenaStats texStats() { return {}; }
inline ArenaStats geoStats() { return {}; }
#endif

// ── Producer API ────────────────────────────────────────────────────────────
// Unbounded: never blocks, never drops (the reference property the old capped
// DrawCommandQueue could not provide — it dropped 33K commands/run under load).
// Whether the RENDER thread owns Present (SR_HLE_OWN_PRESENT, default ON). The
// backend's frame-list model requires it: with it off the MAIN thread presents
// while the render thread still drains commands, so a lazily-opened frame list
// on the render thread would collide with the main thread's recording.
#if SR_ORACLE
bool ownPresentEnabled();
#else
inline bool ownPresentEnabled() { return true; }   // SR_HLE_OWN_PRESENT=0 is the compiled-out A/B
#endif

// ── iter-457 H1a: THE ONE CUBE RECORD VALIDATOR ──────────────────────────────
// A cube upload is accepted ONLY when its two independently sized regions, its
// total payload and its layout equations all agree. Every stage that touches a
// cube record calls THIS body, so the recorder, the replayer and the executor
// cannot drift apart.
//
// `payloadBytes` is the SRHT envelope's own TraceRecord.payloadBytes. A caller
// with no envelope (the live producer path) passes bytes + mipBytes.
struct CubeCheck {
  bool ok = false;
  const char* why = "notChecked";   // always names the failure
  uint32_t baseBytes = 0, mipBytes = 0;
};
#if SR_ORACLE
CubeCheck checkCubeUpload(const RenderCommand& c, uint64_t payloadBytes);

void enqueue(const RenderCommand& cmd);
void enqueueBulk(const RenderCommand* cmds, uint32_t count);
#else
inline void enqueue(const RenderCommand&) {}
inline void enqueueBulk(const RenderCommand*, uint32_t) {}
#endif

// ── iter-450UI: THE OPERATOR TRACE HOTKEY (SR_HLE_TRACE_HOTKEY, default OFF) ─
// An era we cannot arm on programmatically: iter-450UF proved the broken menu is
// visibly composed at p3480-3840 while WorldFlow 17->2 happens ~2 s LATER and
// p3960 is already black, so no guest state transition we have is a sound
// menu-entry signal. Arming on atlas version 2 is worse still — it makes fixture
// acquisition conditional on H6 already being true, so a no-growth run could
// never produce the fixture that would kill H6. The operator's eye is the only
// trustworthy trigger we have for "the broken menu is on screen RIGHT NOW".
//
// ⛔ HOST-DIAGNOSTIC ONLY. F9 never reaches guest pad state, never writes guest
// memory, never calls guest code, and changes no rendering. It sets one atomic.
#if SR_ORACLE
void operatorArmRequest();      // SDL thread, on the F9 keydown edge
bool operatorArmRequested();    // recorder: has the operator armed?
bool consumeOperatorSnapshot(); // one-shot; returns true to exactly one caller
#else
inline void operatorArmRequest() {}   // SR_HLE_TRACE_HOTKEY: no recorder to arm
inline bool consumeOperatorSnapshot() { return false; }
#endif

// ── iter-445: THE PRODUCER-SEQUENCE PROBE (SR_HLE_SEQPROBE, default OFF) ────
// Settles the question this header has carried UNVERIFIED since S2 (see the
// LocalCommandBatch note below): a draw publishes FIVE commands — decl,
// constants, register block, shaders, then the draw itself — through five
// separate lock acquisitions. If a second guest thread publishes in between,
// the render thread applies that thread's state to this thread's draw. The
// symptom would be "the transform is sometimes wrong", which is exactly the
// class S6 is debugging, so it must be ruled in or out BEFORE any more
// transform work — otherwise two causes are confounded.
//
// TWO DISTINCT SIGNALS, and only the second one is a live bug:
//   concurrent — two draw sequences open at once (the begin CAS failed). Proves
//                the guest does NOT serialise its device across threads.
//   splits     — a publish from ANOTHER thread landed INSIDE an open sequence.
//                This is the actual corruption event.
// A run with concurrent>0 and splits==0 means the race exists but has not been
// observed to fire; splits>0 means it fires and the batch must land first.
#if SR_ORACLE
void seqBegin();
void seqEnd();
#else
inline void seqBegin() {}   // SR_HLE_SEQPROBE unset: the scope costs nothing
inline void seqEnd() {}
#endif

struct SeqStats {
  uint64_t opens;        // sequences begun
  uint64_t concurrent;   // begin found another thread's sequence open
  uint64_t splits;       // a foreign publish landed inside an open sequence
  uint64_t publishes;    // publish() calls seen while the probe was on
  uint32_t tidN;         // distinct producer threads observed (cap 16)
  uint32_t tid[16];
  uint64_t tidPub[16];   // commands published by that thread
  uint64_t tidDraw[16];  // draw commands published by that thread
};
#if SR_ORACLE
SeqStats seqStats();
#else
inline SeqStats seqStats() { return {}; }
#endif

// RAII form so an early return in a hook can never leave a sequence open.
struct SeqScope {
  SeqScope() { seqBegin(); }
  ~SeqScope() { seqEnd(); }
};

// Per-draw batch: fill on the stack, publish ONCE — the reference's
// LocalRenderCommandQueue (video.cpp:4280-4295).
//
// ⚠ NOT YET USED (honest note): at S2 each hook mirrors its own call
// independently, because under DELEGATE+MIRROR the state setters are separate
// guest calls rather than a draw-time state flush. This becomes live at S4,
// when a draw flushes its own captured state — then a draw's state and the draw
// itself publish atomically in one lock acquisition (~4,250 -> ~1,000 lock
// ops/frame). Kept here because the shape is settled and S4 is the next slice.
//
// It is an OPTIMISATION plus per-draw atomicity, NOT a proven correctness fix:
// our hooks run inside each guest API call, so publish order already equals the
// guest's call order.
//
// ✅ iter-445 SETTLED IT — the probe above ran, and the S1 census's conclusion
// that per-draw bulk publish is "MANDATORY" is CORRECTED to optional. One
// natural 200 s roll: `concurrent=0` across 1.92 M draw sequences (two draw
// sequences are never open at once), ONE thread owns all 5.39 M draw commands,
// and `splits=30` out of 27 M publishes — every one of them a `Present` from
// the presenter thread, which carries no draw state. So the guest DOES
// effectively serialise its drawing, and this batch buys ~5x fewer lock
// acquisitions per draw, nothing more. Do not spend a correctness argument on
// it; spend a throughput one when the draw rate needs it.
struct LocalCommandBatch {
  static constexpr uint32_t kMax = 24;
  RenderCommand cmds[kMax];
  uint32_t count;

  LocalCommandBatch() : count(0) {}
  ~LocalCommandBatch() { submit(); }

  RenderCommand& add(RenderCommandType t) {
    if (count >= kMax) submit();          // never overflow; publish and continue
    RenderCommand& c = cmds[count++];
    c.type = t;
    return c;
  }
  void submit() {
    if (!count) return;
    enqueueBulk(cmds, count);
    count = 0;
  }
};

// ── S3: DEVICE OWNERSHIP ────────────────────────────────────────────────────
// The render thread owns the D3D12 device and is the ONLY thread that touches
// it. main() still creates it (it holds the HWND) and publishes it here; from
// that point the main loop must never present again — it becomes an event pump.
//
// Publishing is atomic and the render thread tolerates a null backend (it just
// counts, the S2 behaviour), so there is no init-order race with guest threads
// that may start producing before the window exists.
#if SR_ORACLE
void setBackend(sr::gfx::D3D12Backend* backend);
void retireBackend(sr::gfx::D3D12Backend* expected);

// Blocks the CALLING (guest) thread until the render thread has executed the
// Present command with index >= target, or the timeout expires. This is the
// reference's g_executedCommandList.wait (video.cpp:2811) and the console's
// own one-frame-in-flight throttle: a guest-driven frame boundary with real
// backpressure. Returns false on timeout (fails OPEN — never wedge the guest).
bool waitForPresent(uint64_t target, uint32_t timeoutMs);
#else
inline void retireBackend(sr::gfx::D3D12Backend*) {}   // never published: main.cpp's setBackend is the A/B
inline bool waitForPresent(uint64_t, uint32_t) { return false; }   // no render thread presents: fails open at once
#endif

// ── S5: THE TRACE / REPLAY FIXTURE ──────────────────────────────────────────
// WHY THIS EXISTS, AND WHY IT COMES BEFORE THE REST OF S4: S4.4-S4.7 (upload,
// PSO, render-target binding, vertex data, present copy) are five independent
// failure points that ALL fail to an identical black screen. Debugged against
// 200 s boots plus contact sheets, that is 5+ rolls each reporting only "still
// black" — the exact cost iter-408 already paid once (feedback_use_the_replay
// _loop). A trace recorded before S4.2 would have been useless: guest pointers
// with no pixels behind them. With the arena it carries the real planes, so it
// replays into the backend headlessly in seconds with no game boot.
//
// THE PROPERTY THAT MAKES IT TRUSTWORTHY: replay feeds the SAME execute()
// the render thread uses. A fixture with its own parallel executor would drift
// from production and start certifying code that does not ship.
//
// SR_HLE_TRACE_REC=<path>   record (starts at the first Present AFTER movie
//                           activity begins, so a trace always HAS a movie
//                           frame — a no-movie roll would record nothing usable)
// SR_HLE_TRACE_FRAMES=<n>   how many Presents to capture (default 4)
// SR_HLE_TRACE_REPLAY=<path> replay instead of booting the guest (app/main.cpp)
//
// Format: header, then fixed-size records each optionally followed by its raw
// payload. Payloads carry a hash recorded at capture and re-checked at replay,
// so "the pixels survived the round trip" is proven rather than assumed.
struct TraceStats {
  uint64_t records;
  uint64_t payloadBytes;
  uint64_t hashMismatches;
  uint32_t presents;
  // Astra 086: THIS CALL's ClearF colour accounting (all passes of this call,
  // like `records`). The five outcomes CONSERVE the record count:
  //   clearFRecords == restaged + missing + noColour + malformed + allocFail
  // restaged  - payload present, well-formed, copied into a replay-owned block
  // missing   - no payload but the command claimed a colour (the hr1b shape)
  // noColour  - no payload and the command claimed none (colorBytes == 0)
  // malformed - payload present but command/payload sizes are not both 16
  // allocFail - well-formed payload, but the block pool refused the restage
  uint64_t clearFRecords, clearFRestaged, clearFMissing, clearFNoColour,
      clearFMalformed, clearFAllocFail;
};

// Returns 0 on success. `loops` < 0 repeats until the window closes.
#if SR_ORACLE
int replayTrace(const char* path, int loops, TraceStats* out);
#endif

// ── Stats (also the S2 refactor proof: these must equal the [API_OBS] counts) ─
#if SR_ORACLE
uint64_t enqueuedTotal();
// ── iter-456n: THE APPLIED-STATE SHADOW, exposed for its own test ─────────
// The preamble must be applied-state EXACT: one record per normalized key,
// holding whichever command class wrote it LAST, emitted in apply order. The
// eighth adjudication's predeclared killer is a same-key two-order test, so the
// production writer and a read-back are declared here and the test drives the
// REAL shadow rather than a reimplementation of it.
// A thin pass-through to the internal production writer (it lives in this
// translation unit's anonymous namespace). The test calls THIS and therefore
// exercises the real retention rule, not a copy of it.
void shadowNoteAppliedPublic(const RenderCommand& c, const uint8_t* px,
                             uint32_t bytes, uint32_t key);
// Returns false when the key has no applied record. `type` is the command class
// that wrote it last, `digest` its payload hash, `seq` its apply order.
bool shadowPeek(uint32_t key, uint32_t& type, uint64_t& digest, uint64_t& seq);
// iter-457 H1(b)-P1: the FULL retained record - command AND bytes - so a test
// can assert byte identity of a two-region cube payload, not only its digest.
bool shadowPeekFull(uint32_t key, RenderCommand& cmd, std::vector<uint8_t>& bytes,
                    uint64_t& seq);
uint64_t shadowKeyCount();
void shadowResetForTest();
#else
inline uint64_t enqueuedTotal() { return 0; }
#endif

// slice 6 Class B v2 accounting, exposed for the live census.
#if SR_ORACLE
bool resolveOwnershipV2Enabled();
// slice 6: the final capture-disposition witness (default OFF).
// Bounded recorder test hook: drives the REAL window-boundary writer.
bool recorderBoundarySelfTest(const char* path, bool preTreatArm,
                              RenderCommandType triggerType,
                              uint32_t triggerDestKey, uint32_t out[2],
                              uint32_t& outKey, uint64_t& outRecords);
#else
inline bool resolveOwnershipV2Enabled() { return false; }
#endif
// Defined in tex_ownership.hpp; forward-declared so this header need not
// include it just for one signature.
enum class CaptureDisposition : uint32_t;
#if SR_ORACLE
void noteDispositionForTest(uint32_t key, uint64_t sig,
                            CaptureDisposition d);
int productionArmCount();   // drives the REAL env lookup
// Drivers that run the REAL recorder and executor dispatchers, so a test can
// bind those paths rather than extracted helpers.
void resetRecorderForTest(const char* path);
void traceRecordForTest(const RenderCommand& c);
void closeRecorderForTest();
void releaseTracePathForTest();   // clears the override, writes nothing
// Publishes the recorder-open flag traceRecorderOpen() reads (the census's
// capture-window predicate) without the arm machinery or a file.
void setTraceOpenPubForTest(bool open);
// iter-458 (Astra 068) texture census FIFO clock: commands published so far
// (advanced inside the queue lock; SR_TEX_OWN_CENSUS only), and test seams.
uint64_t txcFifoPublished();
void txcSetFifoForTest(uint64_t published, uint64_t executed);
uint64_t txcFifoExecutedForTest();
// Astra 069: a draw command's census path without a backend: the executor's
// census prologue for THIS command (the one execute() runs), then the real
// buildDraw (otherwise reached only through an initialized D3D12Backend), then
// the vertex release the draw arm performs. And the renderer's own
// recorder-window counters {drawCmds, drawsAtTap, bindingsSeen, whiteoutSkips}.
bool executeDrawThroughTapForTest(const RenderCommand& c);
void txcWindowCountersForTest(uint64_t out[4]);
bool recorderOpenedForTest();
// iter-458: published recorder-open state (thread-safe; the present-dump
// path reads it for SR_PRESENT_DUMP_DURING_TRACE).
bool traceRecorderOpen();
// iter-458 (Astra 062): the live recorder's close count (sticky; H-UNIQ seal trigger).
uint32_t traceRecorderCloseCount();
// Astra 063: a hook the recorder's close calls SYNCHRONOUSLY (before the count
// publishes) - the kernel's H-UNIQ seal. Test seams drive the same close path.
void setTraceCloseHook(void (*hook)());
void noteRecorderClosedForTest();
void resetTraceCloseCountForTest();
uint64_t recorderRecordsForTest();
void executeForTest(const RenderCommand& c);
#else
inline uint64_t txcFifoPublished() { return 0; }
inline bool traceRecorderOpen() { return false; }   // SR_HLE_TRACE_REC unset
inline uint32_t traceRecorderCloseCount() { return 0; }
inline void setTraceCloseHook(void (*)()) {}   // no recorder ever closes
#endif
// iter-457 H1a: how many cube uploads the EXECUTOR refused. Exposed so the
// fail-closed arm can be ASSERTED rather than assumed to run - a counter the
// test can read is the difference between "the branch exists" and "the branch
// executed". Without it the arm is unwired-by-default and nobody would know.
#if SR_ORACLE
uint64_t cubeExecRefusedForTest();
// iter-457 H1a-r2: POOL OWNERSHIP, readable by the suite. The cube refusal must
// return its payload's slot to the pool that ALLOCATED it. Releasing the wrong
// pool leaks a texture slot AND frees an unrelated arena slot, and no test in
// this suite could see that until these existed.
uint32_t texPoolInFlightForTest();
uint32_t arenaInFlightForTest();
// iter-457 H1a-r2: how many uploads entered the NON-CUBE handling path. The
// witness is placed BEFORE any backend availability test, so "a cube fell
// through the fail-closed arm" is observable with no backend at all - which is
// exactly what the surviving fall-through mutant exploited.
uint64_t nonCubeUploadEntriesForTest();
// iter-457 H1a-r2: the REPLAY-side cube refusals, and the reason the last one
// gave. UNCAPPED and log-free on purpose - the director's rule is that
// assertions read counters/state, never grep capped log lines (506 of 674 tags
// in this project are count-unsafe).
uint64_t cubeReplayRefusedForTest();
const char* cubeReplayLastReasonForTest();
uint64_t cubeReplayAcceptedForTest();
void resetCubeCountersForTest();
#endif
// Astra 085: the ClearF colour contract, readable by the suite (counters, not
// capped log lines). Executor side: what the last ClearF decoded and the
// effective flags it would hand to hleClearBound (computed whether or not
// SR_HLE_GUEST_CLEAR executes it). Replay side: how each record's colour was
// handled. Plus the small-block pool's occupancy, so ownership is observable.
// (Replay-side accounting is per call, in TraceStats.)
struct ClearFStatsForTest {
  uint64_t seen, colourMissing, colourMalformed, targetsMasked;
  uint32_t lastFlags;
  bool lastColourOk;
  float lastRgba[4];
};
#if SR_ORACLE
ClearFStatsForTest clearFStatsForTest();
void resetClearFStatsForTest();
uint32_t blockInFlightForTest();
#endif
// Astra 086: THE SUBMISSION BOUNDARY. Every executed guest clear passes
// through one function on its way to D3D12Backend::hleClearBound; with a tap
// installed, the tap receives EXACTLY the arguments that call would have
// received (and the backend is not called). Null restores production.
using ClearSubmitTapFn = void (*)(uint32_t flags, const float rgba[4], float z,
                                  uint8_t stencil);
#if SR_ORACLE
void setClearSubmitTapForTest(ClearSubmitTapFn fn);
// SR_HLE_GUEST_CLEAR for the suite: 0/1 force it, -1 = the env gate.
void guestClearSetEnabledForTest(int v);
#endif
// ── iter-457 H1(a)-r4: THE ENQUEUE TAP ───────────────────────────────────────
// A fixture cannot peek the command queue: `publish` starts the detached render
// thread on the first ever call, and the executor RELEASES the payload slot on
// its way out - so any arena read after enqueue() returns is racing a live
// consumer that may already have freed (or reused) those bytes. The capture has
// to be SYNCHRONOUS, on the producer's thread, before the queue insert.
//
// One relaxed atomic load per publish CALL (not per command) on a path the file
// itself measures at ~27M commands/run. Default null: production loads null and
// branches away, and nothing else changes.
using EnqueueTapFn = void (*)(const RenderCommand&);
#if SR_ORACLE
void setEnqueueTapForTest(EnqueueTapFn fn);
#endif
// ── iter-457 H1(b)-P1: THE CUBE BACKEND STAND-IN ─────────────────────────
// The self-test process has no D3D12Backend (app/main.cpp constructs it after
// the suite returns), so the executor's cube SUCCESS arm - the one that must
// retain the applied cube for the recorder preamble - could not be reached by
// any assertion, and the K=1 acquisition c57b7ed found it unwired (a valid v7
// title trace with five live cubes and zero cube records). This stands in for
// the backend CALL only: the SR_HLE_CUBE_UPLOAD gate, the pool reads, the
// success and refusal arms, the shadow retention and the pool releases stay
// production. Default null: production loads null and calls the backend.
struct CubeUploadRequest {
  uint32_t key = 0;
  const uint8_t* base = nullptr;
  uint64_t baseBytes = 0;
  const uint8_t* mip = nullptr;
  uint64_t mipBytes = 0;
  uint32_t fmt = 0, width = 0, height = 0, pitch = 0;
  bool tiled = false;
  uint32_t endian = 0, mipLevels = 1;
  bool packed = false;
};
// Returns the cube index (nonzero = success). On 0 it may name the refusal by
// writing a cubebind::CubeRefuse enumerator value into `refuseCode`.
using CubeUploadStandInFn = uint32_t (*)(const CubeUploadRequest&,
                                         uint32_t& refuseCode);
#if SR_ORACLE
void setCubeUploadStandInForTest(CubeUploadStandInFn fn);
void seedResolveOwnedForTest(uint32_t key);
void clearResolveOwnedForTest();
void beginWitnessWindow();   // THE window-start operation, both witnesses
void resetDispositionWindow();
uint64_t dispositionDigest();
uint64_t dispositionCount();
uint64_t dispositionOfKind(uint32_t kind);
bool resolveDescCarryEnabled();

uint64_t resolveV2Claims();
uint64_t resolveV2LegacyAdopted();
uint64_t resolveV2SuccessNoEpoch();
uint64_t resolveV2CapturesRefused();

uint64_t drainedTotal();
uint64_t presentCount();      // Present COMMANDS executed by the render thread
uint64_t presentsSubmitted();  // actual swapchain presents issued
uint64_t typeCount(RenderCommandType t);
uint32_t queueDepth();
#else
inline uint64_t dispositionDigest() { return 0; }
inline uint64_t dispositionCount() { return 0; }
inline uint64_t dispositionOfKind(uint32_t) { return 0; }
inline uint64_t resolveV2Claims() { return 0; }
inline uint64_t resolveV2LegacyAdopted() { return 0; }
inline uint64_t resolveV2SuccessNoEpoch() { return 0; }
inline uint64_t resolveV2CapturesRefused() { return 0; }
inline uint64_t drainedTotal() { return 0; }
inline uint64_t presentCount() { return 0; }
inline uint64_t presentsSubmitted() { return 0; }
inline uint64_t typeCount(RenderCommandType) { return 0; }
inline uint32_t queueDepth() { return 0; }
#endif

}  // namespace hle

// ── iter-457 gate 3 [DRAWMAN]: the suppression manifest's completion status ──
// 0 when no manifest was supplied or the treatment was EXACT; nonzero when the
// manifest was refused (2) or requested != matched != suppressed (3). The replay
// driver uses it as its exit code, so a treatment that diverged from what was
// requested can never be reported as a clean run.
#if SR_ORACLE
int hleDrawManifestStatus();
#endif

}  // namespace gfx
}  // namespace sr
