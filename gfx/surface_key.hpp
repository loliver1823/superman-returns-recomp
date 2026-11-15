#pragma once

// The surface key: the guest render target as the Xbox 360 names it - an EDRAM base and pitch in tiles, MSAA mode,
// colour / depth, format - with the register -> host translation math (draw targets, viewport, scissor, resolve
// info), the render-target FORMAT encodings (pack / unpack: the clear words, the kernels' texels) and the alias
// geometry (a tile run as pixel rectangles of a surface). Pure logic, no D3D12. Shipping code (docs/
// RELEASE_COMPOSITION.md section 8, stage 3): an Xbox 360 surface IS a base + pitch in EDRAM tiles (the XDK allocates
// surfaces by EDRAM offset and aliases them on purpose), so the tile coordinates are the guest's own surface
// identity, not an emulated word layout. The EDRAM WORD model - the tile ownership map, the sample addressing of the
// 10 MiB buffer, the CPU resolve from the mirror - is gfx/edram_model.hpp (oracle). gfx/edram.hpp includes both.
//
// Ported from Xenia (reference/xenia-canary/src/xenia/gpu/, BSD-3-Clause, Copyright Ben Vanik). The contract, with
// file:line citations, is docs/XENIA_EDRAM_CONTRACT.md. When this file disagrees with Xenia, this file is the suspect.
// The namespace keeps its name (edram::): every reader names the surface key through it.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "par.hpp"
#include "xenos_tiling.hpp"

namespace sr {
namespace gfx {
namespace edram {

constexpr uint32_t kTileWidthSamples = 80;
constexpr uint32_t kTileHeightSamples = 16;
constexpr uint32_t kTileCount = 2048;
constexpr uint32_t kMsaa1X = 0, kMsaa2X = 1, kMsaa4X = 2;
constexpr uint32_t kResolveAlign = 8;
constexpr uint32_t kMaxResolveSize = 16384 - 8;

// Register snapshot of one draw / resolve: only what the EDRAM model reads.
struct Regs {
  uint32_t surfaceInfo = 0;                      // 0x2000 RB_SURFACE_INFO
  uint32_t colorInfo[4] = {};                    // 0x2001, 0x2003..0x2005
  uint32_t depthInfo = 0;                        // 0x2002
  uint32_t colorMask = 0;                        // 0x2104
  uint32_t depthControl = 0;                     // 0x2200
  uint32_t stencilRefMask = 0;                   // 0x210D RB_STENCILREFMASK (ref|mask<<8|write<<16)
  uint32_t stencilRefMaskBF = 0;                 // 0x210C RB_STENCILREFMASK_BF
  uint32_t modeControl = 0;                      // 0x2208
  uint32_t windowOffset = 0;                     // 0x2080
  uint32_t windowScissorTL = 0, windowScissorBR = 0;  // 0x2081/0x2082
  uint32_t screenScissorTL = 0, screenScissorBR = 0;  // 0x200E/0x200F
  uint32_t suScModeCntl = 0;                     // 0x2205
  uint32_t clipCntl = 0;                         // 0x2204
  uint32_t vteCntl = 0;                          // 0x2206
  uint32_t vtxCntl = 0;                          // 0x2302 PA_SU_VTX_CNTL
  float vpScale[3] = {}, vpOffset[3] = {};       // 0x210F..0x2114
  uint32_t copyControl = 0, copyDestBase = 0, copyDestPitch = 0, copyDestInfo = 0;
  uint32_t depthClear = 0, colorClear = 0, colorClearLo = 0;  // 0x231D..0x231F
  // A resolve draw's rectangle: vertex fetch 0, three xy pairs, byte-swapped.
  float resolveVerts[6] = {};
  bool resolveVertsValid = false;
};

// step 6 R6 CHECK (Astra round 1 P2): field-wise equality of two register snapshots - every field; floats compared
// bitwise (a recomputation is bit-exact: == would call one NaN unequal to itself and +0 equal to -0). The
// static_assert is a layout-size TRIPWIRE, not a completeness proof (Astra r3 P3): a member that grows the struct
// fails the build, but a small one could sit in existing padding with the size unchanged - a new member must be
// added to this compare and to its mutant test (tools/difftest/edram_test.cpp testSameRegs) by review.
inline bool sameRegs(const Regs& a, const Regs& b) {
  auto f = [](const float* x, const float* y, size_t n) { return std::memcmp(x, y, n * sizeof(float)) == 0; };
  return a.surfaceInfo == b.surfaceInfo && std::equal(a.colorInfo, a.colorInfo + 4, b.colorInfo) &&
         a.depthInfo == b.depthInfo && a.colorMask == b.colorMask && a.depthControl == b.depthControl &&
         a.stencilRefMask == b.stencilRefMask && a.stencilRefMaskBF == b.stencilRefMaskBF &&
         a.modeControl == b.modeControl && a.windowOffset == b.windowOffset &&
         a.windowScissorTL == b.windowScissorTL && a.windowScissorBR == b.windowScissorBR &&
         a.screenScissorTL == b.screenScissorTL && a.screenScissorBR == b.screenScissorBR &&
         a.suScModeCntl == b.suScModeCntl && a.clipCntl == b.clipCntl && a.vteCntl == b.vteCntl &&
         a.vtxCntl == b.vtxCntl && f(a.vpScale, b.vpScale, 3) && f(a.vpOffset, b.vpOffset, 3) &&
         a.copyControl == b.copyControl && a.copyDestBase == b.copyDestBase && a.copyDestPitch == b.copyDestPitch &&
         a.copyDestInfo == b.copyDestInfo && a.depthClear == b.depthClear && a.colorClear == b.colorClear &&
         a.colorClearLo == b.colorClearLo && a.resolveVertsValid == b.resolveVertsValid &&
         f(a.resolveVerts, b.resolveVerts, 6);
}
static_assert(sizeof(Regs) == 160, "edram::Regs changed: extend sameRegs (step 6 R6 CHECK) and testSameRegs");

inline int32_t sext(uint32_t v, int bits) {
  return int32_t(v << (32 - bits)) >> (32 - bits);
}
inline uint32_t surfacePitch(const Regs& r) { return r.surfaceInfo & 0x3FFFu; }
inline uint32_t surfaceMsaa(const Regs& r) { return (r.surfaceInfo >> 16) & 3u; }
inline uint32_t edramMode(const Regs& r) { return r.modeControl & 7u; }
inline int32_t windowOffsetX(const Regs& r) { return sext(r.windowOffset & 0x7FFFu, 15); }
inline int32_t windowOffsetY(const Regs& r) {
  return sext((r.windowOffset >> 16) & 0x7FFFu, 15);
}

// ── Formats ────────────────────────────────────────────────────────────────
// ColorRenderTargetFormat: 0 8888, 1 8888_GAMMA, 2 2_10_10_10, 3 2_10_10_10_FLOAT,
// 4 16_16, 5 16_16_16_16, 6 16_16_FLOAT, 7 16_16_16_16_FLOAT,
// 10 2_10_10_10_AS_10_10_10_10, 12 2_10_10_10_FLOAT_AS_16_16_16_16,
// 14 32_FLOAT, 15 32_32_FLOAT. DepthRenderTargetFormat: 0 D24S8, 1 D24FS8.
inline bool colorIs64bpp(uint32_t f) { return f == 5 || f == 7 || f == 15; }
// xenos.h:392-402 GetStorageColorFormat.
inline uint32_t colorStorageFormat(uint32_t f) {
  return f == 10 ? 2u : (f == 12 ? 3u : f);
}
// render_target_cache.h:679-686 with gamma NOT stored separately (8888_GAMMA
// shares 8888 storage - gamma conversion is a later refinement).
inline uint32_t colorResourceFormat(uint32_t f) {
  f = colorStorageFormat(f);
  return f == 1 ? 0u : f;
}
// Component count of a color RT format (for the normalized write mask).
inline uint32_t colorComponentCount(uint32_t f) {
  switch (colorStorageFormat(f)) {
    case 4: case 6: case 15: return 2;
    case 14: return 1;
    default: return 4;
  }
}

// Host storage chosen for each resource format. Floats are kept at their TRUE
// guest value (16_16 fixed -32..32 in a half float), so no shader-side scaling
// is needed; the EDRAM pack clamps to the guest range.
enum class HostFmt : uint8_t { None, RGBA8, RGB10A2, RGBA16F, RG16F, R32F, RG32F, D32F };
inline HostFmt hostColorFormat(uint32_t resourceFmt) {
  switch (resourceFmt) {
    case 0: return HostFmt::RGBA8;
    case 2: return HostFmt::RGB10A2;
    case 3: case 5: case 7: return HostFmt::RGBA16F;
    case 4: case 6: return HostFmt::RG16F;
    case 14: return HostFmt::R32F;
    case 15: return HostFmt::RG32F;
    default: return HostFmt::None;
  }
}
inline uint32_t hostBytesPerTexel(HostFmt f) {
  switch (f) {
    case HostFmt::RGBA16F: case HostFmt::RG32F: return 8;
    case HostFmt::None: return 0;
    default: return 4;
  }
}

// iter-464 resscale: the draw resolution scale along Y, for the host-limit clamp
// of a render target's height (set once by gfx/res_scale.cpp before any RT
// exists; 1 = the unscaled path, the pre-resscale heights bit for bit).
inline uint32_t g_rtHeightScaleY = 1;
constexpr uint32_t kMaxHostRtHeight = 16384;  // D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION

// ── Render-target key (render_target_cache.h:251-262) ──────────────────────
struct RtKey {
  uint32_t v = 0;
  RtKey() = default;
  explicit RtKey(uint32_t raw) : v(raw) {}
  static RtKey make(uint32_t baseTiles, uint32_t pitchTiles32, uint32_t msaa,
                    bool depth, uint32_t format) {
    return RtKey((baseTiles & 0x7FFu) | ((pitchTiles32 & 0xFFu) << 11) |
                 ((msaa & 3u) << 19) | (uint32_t(depth) << 21) |
                 ((format & 0xFu) << 22));
  }
  bool empty() const { return v == 0; }
  uint32_t base() const { return v & 0x7FFu; }
  uint32_t pitchTiles32() const { return (v >> 11) & 0xFFu; }
  uint32_t msaa() const { return (v >> 19) & 3u; }
  bool isDepth() const { return ((v >> 21) & 1u) != 0; }
  uint32_t format() const { return (v >> 22) & 0xFu; }
  bool is64bpp() const { return !isDepth() && colorIs64bpp(format()); }
  uint32_t intsLog2() const { return is64bpp() ? 1u : 0u; }
  uint32_t pitchTiles() const { return pitchTiles32() << intsLog2(); }
  // Pixel width (:306-311): 32 and 64bpp share it; 64bpp spans twice the tiles.
  uint32_t width() const {
    return pitchTiles32() * (kTileWidthSamples >> (msaa() >= kMsaa4X ? 1 : 0));
  }
  // GetRenderTargetHeight (render_target_cache.cc:988-1014): guest pixels, the
  // tile rows clamped to the guest limit (8192 * scale) and the host limit
  // (16384) in SCALED samples - at 3x a small-pitch surface stops at 5456 rows.
  uint32_t height() const {
    const uint32_t p = pitchTiles32();
    if (!p) return 0;
    uint32_t tileRows = (kTileCount + p - 1) / p;
    const uint32_t yShift = msaa() >= kMsaa2X ? 1u : 0u;
    const uint32_t sy = g_rtHeightScaleY ? g_rtHeightScaleY : 1u;
    const uint32_t maxScaled = std::min(8192u * sy, kMaxHostRtHeight);
    tileRows = std::min(tileRows, (maxScaled << yShift) / (kTileHeightSamples * sy));
    return tileRows * (kTileHeightSamples >> yShift);
  }
  uint32_t tileWidthPixels() const {
    return kTileWidthSamples >> ((msaa() >= kMsaa4X ? 1u : 0u) + intsLog2());
  }
  uint32_t tileHeightPixels() const {
    return kTileHeightSamples >> (msaa() >= kMsaa2X ? 1u : 0u);
  }
  HostFmt hostFormat() const {
    return isDepth() ? HostFmt::D32F : hostColorFormat(format());
  }
  bool operator==(const RtKey& o) const { return v == o.v; }
  bool operator!=(const RtKey& o) const { return v != o.v; }
};

// pitch_tiles_at_32bpp (render_target_cache.cc:585-602).
inline uint32_t pitchTiles32(uint32_t surfacePitchPx, uint32_t msaa) {
  return ((surfacePitchPx << (msaa >= kMsaa4X ? 1u : 0u)) + 79u) / 80u;
}

// ── Per-draw bound targets ─────────────────────────────────────────────────
struct DrawTargets {
  bool rasterize = false;
  RtKey color[4];
  uint8_t colorWriteMask[4] = {};  // normalized RGBA mask; 0 = not bound
  RtKey depth;
  bool depthBound = false;
  bool zEnable = false, zWrite = false, stencilEnable = false;
  uint32_t zFunc = 7;
  // Ownership claim length per target (tiles), 0..3 color, 4 depth.
  uint32_t claimTiles[5] = {};
  // iter-467 native-b B1 (SR_NB_CLEAR_ELIDE, gfx/d3d12_pm4_edram.cpp): the draw is an XDK EDRAM clear
  // that rewrites every sample of fullRect (surface pixels, [x0, y0, x1, y1)) in the bindings of
  // fullWrite (bit i = colour i, bit 4 = depth + stencil); 0 = unknown. Set by the D3D12 drain only.
  uint8_t fullWrite = 0;
  int32_t fullRect[4] = {};
  // iter-467 native-b B1c (SR_NB_CLEAR_PROP): for an XDK clear, fullRect is its exact raster rectangle even when no
  // binding is fully written (rectExact = 1), and the values it writes when they are uniform: colour 0 (clearColor,
  // clearValid bit 0) and depth 0 + stencil (clearStencil, clearValid bit 4).
  uint8_t rectExact = 0;
  uint8_t clearValid = 0;
  uint8_t clearStencil = 0;
  float clearColor[4] = {};
};

// step 6 R6 CHECK (Astra round 2 P2b): field-wise equality of two DrawTargets - every field; floats compared bitwise
// (a recomputation is bit-exact). The static_assert is a layout-size TRIPWIRE, not a completeness proof (Astra r3
// P3): a member that grows the struct fails the build, but a small one could sit in existing padding with the size
// unchanged - a new member must be added to this compare and to its mutant test (tools/difftest/edram_test.cpp
// testSameTargets) by review.
inline bool sameTargets(const DrawTargets& a, const DrawTargets& b) {
  auto same = [](const void* x, const void* y, size_t n) { return std::memcmp(x, y, n) == 0; };
  if (a.rasterize != b.rasterize || a.depthBound != b.depthBound || a.zEnable != b.zEnable || a.zWrite != b.zWrite ||
      a.stencilEnable != b.stencilEnable || a.zFunc != b.zFunc || a.fullWrite != b.fullWrite ||
      a.rectExact != b.rectExact || a.clearValid != b.clearValid || a.clearStencil != b.clearStencil)
    return false;
  for (int i = 0; i < 4; i++)
    if (a.color[i] != b.color[i]) return false;
  return a.depth == b.depth && same(a.colorWriteMask, b.colorWriteMask, sizeof(a.colorWriteMask)) &&
         same(a.claimTiles, b.claimTiles, sizeof(a.claimTiles)) && same(a.fullRect, b.fullRect, sizeof(a.fullRect)) &&
         same(a.clearColor, b.clearColor, sizeof(a.clearColor));
}
static_assert(sizeof(DrawTargets) == 96, "edram::DrawTargets changed: extend sameTargets (step 6 R6 CHECK) and testSameTargets");

// Scissor (draw_util.cc:640-725). Offsets/extents in surface pixels.
struct Scissor { int32_t x = 0, y = 0, w = 0, h = 0; };
inline Scissor computeScissor(const Regs& r, bool clampToSurfacePitch) {
  int32_t tlx = int32_t(r.windowScissorTL & 0x3FFFu);
  int32_t tly = int32_t((r.windowScissorTL >> 16) & 0x3FFFu);
  int32_t brx = int32_t(r.windowScissorBR & 0x3FFFu);
  int32_t bry = int32_t((r.windowScissorBR >> 16) & 0x3FFFu);
  if (!(r.windowScissorTL >> 31)) {
    tlx += windowOffsetX(r); tly += windowOffsetY(r);
    brx += windowOffsetX(r); bry += windowOffsetY(r);
  }
  tlx = std::max(tlx, sext(r.screenScissorTL & 0x7FFFu, 15));
  tly = std::max(tly, sext((r.screenScissorTL >> 16) & 0x7FFFu, 15));
  brx = std::min(brx, sext(r.screenScissorBR & 0x7FFFu, 15));
  bry = std::min(bry, sext((r.screenScissorBR >> 16) & 0x7FFFu, 15));
  if (clampToSurfacePitch) {
    const int32_t pitch = int32_t(surfacePitch(r));
    tlx = std::min(tlx, pitch);
    brx = std::min(brx, pitch);
  }
  tlx = std::max(tlx, 0); tly = std::max(tly, 0);
  brx = std::max(brx, tlx); bry = std::max(bry, tly);
  return Scissor{tlx, tly, brx - tlx, bry - tly};
}

// DrawExtentEstimator::EstimateMaxY, without the CPU vertex-shader pass
// (draw_extent_estimator.cc:269-354).
inline uint32_t estimateMaxY(const Regs& r) {
  int32_t bry = int32_t((r.windowScissorBR >> 16) & 0x3FFFu);
  if (!(r.windowScissorTL >> 31)) bry += windowOffsetY(r);
  bry = std::min(bry, sext((r.screenScissorBR >> 16) & 0x7FFFu, 15));
  bry = std::max(bry, 0);
  uint32_t maxY = uint32_t(bry);
  if (!((r.clipCntl >> 16) & 1u)) {
    float add = ((r.suScModeCntl >> 16) & 1u) ? float(windowOffsetY(r)) : 0.0f;
    if (!(r.vtxCntl & 1u)) add += 0.5f;
    const float yOff = ((r.vteCntl >> 3) & 1u) ? r.vpOffset[1] : 0.0f;
    const float yScale = ((r.vteCntl >> 2) & 1u) ? std::fabs(r.vpScale[1]) : 1.0f;
    const float bottom = std::max(0.0f, add + yOff + yScale);
    maxY = std::min(maxY, uint32_t(bottom));
  }
  return maxY;
}

// draw_util.cc:35-79, 698-712, 727-768 and render_target_cache.cc:632-846.
// psColorOutMask: bit i = the pixel shader exports oC_i (static info).
// vertexMaxY >= 0 bounds the claimed height by the draw's actual vertices:
// Xenia gets that bound for clip-disabled draws by running the VS on the CPU
// (draw_extent_estimator.cc); without it a screen-space clear rect claims the
// whole scissor (8192 rows) and steals every other surface's tiles.
inline DrawTargets computeDrawTargets(const Regs& r, uint32_t psColorOutMask,
                                      bool polygonal, int32_t vertexMaxY = -1) {
  DrawTargets t;
  const uint32_t mode = edramMode(r);
  if (mode != 4u && mode != 5u) return t;
  const uint32_t pitch = surfacePitch(r);
  if (!pitch) return t;
  if (polygonal && (r.suScModeCntl & 3u) == 3u) return t;
  const uint32_t msaa = surfaceMsaa(r);
  if (msaa > kMsaa4X) return t;
  const uint32_t pt = pitchTiles32(pitch, msaa);
  if (!pt || pt > 255u) return t;
  t.rasterize = true;

  bool zEnable = (r.depthControl & 2u) != 0;
  const bool zWrite = (r.depthControl & 4u) != 0;
  const uint32_t zFunc = (r.depthControl >> 4) & 7u;
  const bool stencil = (r.depthControl & 1u) != 0;
  if (zEnable && !zWrite && zFunc == 7u) zEnable = false;
  t.zEnable = zEnable;
  t.zWrite = zEnable && zWrite;
  t.zFunc = zFunc;
  t.stencilEnable = stencil;
  if (zEnable || stencil) {
    t.depthBound = true;
    t.depth = RtKey::make(r.depthInfo & 0x7FFu, pt, msaa, true, (r.depthInfo >> 16) & 1u);
  }
  if (mode == 4u) {
    for (uint32_t i = 0; i < 4; i++) {
      if (!(psColorOutMask & (1u << i))) continue;
      const uint32_t fmt = (r.colorInfo[i] >> 16) & 0xFu;
      const uint32_t comps = colorComponentCount(fmt);
      const uint32_t have = (1u << comps) - 1u;
      uint32_t m = (r.colorMask >> (4 * i)) & have;
      if (!m) continue;
      m |= ~have & 0xFu;
      t.colorWriteMask[i] = uint8_t(m);
      t.color[i] = RtKey::make(r.colorInfo[i] & 0x7FFu, pt, msaa, false,
                               colorResourceFormat(fmt));
    }
  }
  // Same-base elimination: lower color index wins; depth loses to color.
  for (uint32_t i = 0; i < 4; i++) {
    if (!t.colorWriteMask[i]) continue;
    for (uint32_t j = i + 1; j < 4; j++)
      if (t.colorWriteMask[j] && t.color[j].base() == t.color[i].base())
        t.colorWriteMask[j] = 0;
    if (t.depthBound && t.depth.base() == t.color[i].base()) t.depthBound = false;
  }

  // Claim lengths (render_target_cache.cc:749-846).
  struct B { uint32_t base; bool is64; int slot; };
  B bound[5]; int nb = 0;
  for (int i = 0; i < 4; i++)
    if (t.colorWriteMask[i]) bound[nb++] = {t.color[i].base(), t.color[i].is64bpp(), i};
  if (t.depthBound) bound[nb++] = {t.depth.base(), false, 4};
  if (!nb) return t;
  std::sort(bound, bound + nb, [](const B& a, const B& b) { return a.base < b.base; });
  RtKey probe = RtKey::make(0, pt, msaa, false, 0);
  uint32_t heightUsed = std::min(probe.height(), estimateMaxY(r));
  if (vertexMaxY >= 0) heightUsed = std::min(heightUsed, uint32_t(vertexMaxY));
  const uint32_t yShift = msaa >= kMsaa2X ? 1u : 0u;
  const uint32_t lengthUsed32 =
      ((heightUsed << yShift) + kTileHeightSamples - 1) / kTileHeightSamples * pt;
  uint32_t maxDist64 = kTileCount * 2u;
  for (int i = 0; i < nb; i++) {
    const uint32_t next = (i + 1 < nb) ? bound[i + 1].base : bound[0].base + kTileCount;
    uint32_t gap = next - bound[i].base;
    if (nb == 1) gap = kTileCount;
    maxDist64 = std::min(maxDist64, bound[i].is64 ? gap : (gap << 1));
  }
  for (int i = 0; i < nb; i++) {
    const uint32_t is64 = bound[i].is64 ? 1u : 0u;
    const uint32_t next = (i + 1 < nb) ? bound[i + 1].base : bound[0].base + kTileCount;
    uint32_t len = std::min(lengthUsed32 << is64, maxDist64 >> (is64 ^ 1u));
    len = std::min(len, next - bound[i].base);
    t.claimTiles[bound[i].slot] = len;
  }
  return t;
}

// -- The alias table's unit: a run of absolute tiles and the surface that owns it (nb_surfaces.hpp; the tile map of
// edram_model.hpp reports the same) ---------------------------------------------------------------------------
struct Transfer {
  uint32_t start = 0, end = 0;  // absolute tiles, start < end <= kTileCount
  RtKey source;
};

// Pixel rectangles (in `k`'s own coordinates) covering the absolute tile
// range [start, end) (GetRangeRectangles, render_target_cache.cc:332-416).
// release/emu-stage3 (Astra re-review 2): the EDRAM is a ring of 2048 tiles, so a range may cross the owner's BASE
// going around it (an owner whose pitch does not divide 2048: pitch 20, base 1536, the span [1528,1548) of its row
// 102 crosses tile 1536). Relative to the base such a range runs past 2048; before, rel1 = rel0 + length placed the
// part past the base on rows beyond the surface's last tile row (host pixels the owner's rows 0.. alias - or, with a
// pitch dividing 2048, rows past the host RT, dropped) while the native kernel's canonicalisation (NatOwnerPixel)
// reads a tile at (tile - base) mod 2048: its own row / column. The range is split at the base now: the part before
// it keeps its rows, the part from the base on starts at row 0 - the host location every reader of a tile agrees
// on. Up to kRangeRectsMax rectangles (3 per part). g_rangeRectSplits counts the splits (the census prints it).
struct Rect { uint32_t x = 0, y = 0, w = 0, h = 0; };
constexpr int kRangeRectsMax = 6;
inline std::atomic<uint64_t> g_rangeRectSplits{0};
inline int rangeRectsPart(RtKey k, uint32_t rel0, uint32_t rel1, Rect* out) {
  const uint32_t pt = k.pitchTiles();
  const uint32_t tw = k.tileWidthPixels(), th = k.tileHeightPixels();
  const uint32_t row0 = rel0 / pt, col0 = rel0 % pt;
  const uint32_t row1 = (rel1 - 1) / pt, col1 = (rel1 - 1) % pt + 1;
  int n = 0;
  auto push = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const uint32_t W = k.width(), H = k.height();
    if (x >= W || y >= H) return;
    w = std::min(w, W - x);
    h = std::min(h, H - y);
    if (w && h) out[n++] = Rect{x, y, w, h};
  };
  if (row0 == row1) {
    push(col0 * tw, row0 * th, (col1 - col0) * tw, th);
    return n;
  }
  uint32_t fullStart = row0, fullEnd = row1 + 1;
  if (col0 != 0) {
    push(col0 * tw, row0 * th, (pt - col0) * tw, th);
    fullStart = row0 + 1;
  }
  if (col1 != pt) fullEnd = row1;
  if (fullEnd > fullStart) push(0, fullStart * th, pt * tw, (fullEnd - fullStart) * th);
  if (col1 != pt) push(0, row1 * th, col1 * tw, th);
  return n;
}
inline int rangeRects(RtKey k, uint32_t start, uint32_t end, Rect out[kRangeRectsMax]) {
  if (start >= end) return 0;
  if (!k.pitchTiles()) return 0;
  const uint32_t rel0 = (start + kTileCount - k.base()) % kTileCount;
  const uint32_t rel1 = rel0 + (end - start);  // exclusive; <= rel0 + kTileCount
  if (rel1 <= kTileCount) return rangeRectsPart(k, rel0, rel1, out);
  g_rangeRectSplits.fetch_add(1, std::memory_order_relaxed);
  int n = rangeRectsPart(k, rel0, kTileCount, out);
  n += rangeRectsPart(k, 0, rel1 - kTileCount, out + n);
  return n;
}

inline uint32_t msaaSampleCount(uint32_t msaa) { return 1u << msaa; }

// ── Scalar conversions ─────────────────────────────────────────────────────
inline uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline float saturate(float f) { return f != f ? 0.0f : std::min(std::max(f, 0.0f), 1.0f); }

inline uint16_t floatToHalf(float f) {
  const uint32_t x = f2u(f);
  const uint32_t sign = (x >> 16) & 0x8000u;
  uint32_t mant = x & 0x7FFFFFu;
  const int32_t exp = int32_t((x >> 23) & 0xFFu);
  if (exp == 0xFF) return uint16_t(sign | 0x7C00u | (mant ? 0x200u : 0u));
  int32_t e = exp - 127 + 15;
  if (e >= 0x1F) return uint16_t(sign | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return uint16_t(sign);
    mant |= 0x800000u;
    const uint32_t shift = uint32_t(14 - e);
    uint32_t h = mant >> shift;
    const uint32_t rem = mant & ((1u << shift) - 1u), halfway = 1u << (shift - 1);
    if (rem > halfway || (rem == halfway && (h & 1u))) h++;
    return uint16_t(sign | h);
  }
  uint32_t h = (uint32_t(e) << 10) | (mant >> 13);
  const uint32_t rem = mant & 0x1FFFu;
  if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) h++;
  return uint16_t(sign | h);
}
inline float halfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
  if (exp == 0) {
    if (!mant) return u2f(sign);
    exp = 1;
    while (!(mant & 0x400u)) { mant <<= 1; exp--; }
    mant &= 0x3FFu;
    return u2f(sign | ((exp + 127 - 15) << 23) | (mant << 13));
  }
  if (exp == 0x1F) return u2f(sign | 0x7F800000u | (mant << 13));
  return u2f(sign | ((exp + 127 - 15) << 23) | (mant << 13));
}

// 7e3 (DirectXTexConvert FloatTo7e3 / pixel_formats.xesli XeUnpackR10FloatX4).
inline uint32_t floatTo7e3(float value) {
  uint32_t i = f2u(value);
  if (i & 0x80000000u) return 0;              // positive only (drops -0, NaN sign)
  if (value != value) return 0;
  if (i > 0x41FF73FFu) return 0x3FFu;         // saturate at 31.875
  if (i < 0x3E800000u) {                      // below 0.25: denormalized
    const uint32_t shift = 125u - (i >> 23);
    i = shift >= 32 ? 0u : ((0x800000u | (i & 0x7FFFFFu)) >> shift);
  } else {
    i += 0xC2000000u;                         // rebias the exponent
  }
  return ((i + 0x7FFFu + ((i >> 16) & 1u)) >> 16) & 0x3FFu;
}
inline float f7e3ToFloat(uint32_t f10) {
  f10 &= 0x3FFu;
  if (!f10) return 0.0f;
  uint32_t mantissa = f10 & 0x7Fu, exponent = f10 >> 7;
  if (!exponent) {
    // Denormal: normalize so the hidden bit lands at bit 7; exponent = 1 - lz
    // (may go negative - the uint wrap plus the +124 bias is exact).
    uint32_t lz = 0;
    while (!(mantissa & 0x80u)) { mantissa <<= 1; lz++; }
    mantissa &= 0x7Fu;
    return u2f(((uint32_t(int32_t(1) - int32_t(lz)) + 124u) << 23) | (mantissa << 16));
  }
  return u2f(((exponent + 124u) << 23) | (mantissa << 16));
}

// 20e4 (xenos.cc Float32To20e4 / pixel_formats.xesli XeFloat20e4To32).
inline uint32_t floatTo20e4(float f32) {
  uint32_t u = f2u(f32);
  u = std::min((u <= 0x7FFFFFFFu) ? u : 0u, 0x3FFFFFF8u);
  const uint32_t shift = std::min(113u - std::min(113u, u >> 23), 24u);
  const uint32_t denorm = ((u & 0x7FFFFFu) | 0x800000u) >> shift;
  uint32_t f24 = (u < 0x38800000u) ? denorm : (u + 0xC8000000u);
  f24 += 3u + ((f24 >> 3) & 1u);  // round to nearest even
  return (f24 >> 3) & 0xFFFFFFu;
}
inline float f20e4ToFloat(uint32_t f24) {
  f24 &= 0xFFFFFFu;
  if (!f24) return 0.0f;
  uint32_t mantissa = f24 & 0xFFFFFu, exponent = f24 >> 20;
  if (!exponent) {
    uint32_t lz = 0;
    while (!(mantissa & 0x100000u)) { mantissa <<= 1; lz++; }
    mantissa &= 0xFFFFFu;
    return u2f(((uint32_t(1 - int32_t(lz)) + 112u) << 23) | (mantissa << 3));
  }
  return u2f(((exponent + 112u) << 23) | (mantissa << 3));
}

// ── Guest EDRAM pixel packing (pixel_formats.xesli) ────────────────────────
inline uint32_t packUNorm(float f, float scale) { return uint32_t(saturate(f) * scale + 0.5f); }
inline uint32_t packFixed16Edram(float f) {  // XePackR16G16Edram: [-32,32] fixed
  f = std::min(std::max(f != f ? 0.0f : f, -32.0f), 32.0f);
  return uint32_t(int32_t(f * (32767.0f / 32.0f))) & 0xFFFFu;
}
inline float unpackFixed16Edram(uint32_t v) {  // XeUnpackR16G16Edram
  return std::max(-32.0f, float(int32_t(v << 16) >> 16) * (32.0f / 32767.0f));
}

// Host value (true guest value) -> EDRAM bits of one sample. 64bpp formats use
// both halves (lo = first dword).
inline uint64_t packColor(uint32_t resourceFmt, const float c[4]) {
  switch (resourceFmt) {
    case 0:
      return packUNorm(c[0], 255.f) | (packUNorm(c[1], 255.f) << 8) |
             (packUNorm(c[2], 255.f) << 16) | (uint64_t(packUNorm(c[3], 255.f)) << 24);
    case 2:
      return packUNorm(c[0], 1023.f) | (packUNorm(c[1], 1023.f) << 10) |
             (packUNorm(c[2], 1023.f) << 20) | (uint64_t(packUNorm(c[3], 3.f)) << 30);
    case 3:
      return floatTo7e3(c[0]) | (floatTo7e3(c[1]) << 10) | (floatTo7e3(c[2]) << 20) |
             (uint64_t(packUNorm(c[3], 3.f)) << 30);
    case 4:
      return packFixed16Edram(c[0]) | (uint64_t(packFixed16Edram(c[1])) << 16);
    case 5:
      return uint64_t(packFixed16Edram(c[0]) | (packFixed16Edram(c[1]) << 16)) |
             (uint64_t(packFixed16Edram(c[2]) | (packFixed16Edram(c[3]) << 16)) << 32);
    case 6:
      return floatToHalf(c[0]) | (uint64_t(floatToHalf(c[1])) << 16);
    case 7:
      return uint64_t(floatToHalf(c[0]) | (uint32_t(floatToHalf(c[1])) << 16)) |
             (uint64_t(floatToHalf(c[2]) | (uint32_t(floatToHalf(c[3])) << 16)) << 32);
    case 14:
      return f2u(c[0]);
    case 15:
      return uint64_t(f2u(c[0])) | (uint64_t(f2u(c[1])) << 32);
    default:
      return 0;
  }
}
inline void unpackColor(uint32_t resourceFmt, uint64_t bits, float c[4]) {
  const uint32_t lo = uint32_t(bits), hi = uint32_t(bits >> 32);
  c[0] = c[1] = c[2] = 0.0f;
  c[3] = 1.0f;
  switch (resourceFmt) {
    case 0:
      for (int i = 0; i < 4; i++) c[i] = float((lo >> (8 * i)) & 0xFFu) * (1.0f / 255.0f);
      break;
    case 2:
      for (int i = 0; i < 3; i++) c[i] = float((lo >> (10 * i)) & 0x3FFu) * (1.0f / 1023.0f);
      c[3] = float(lo >> 30) * (1.0f / 3.0f);
      break;
    case 3:
      for (int i = 0; i < 3; i++) c[i] = f7e3ToFloat(lo >> (10 * i));
      c[3] = float(lo >> 30) * (1.0f / 3.0f);
      break;
    case 4:
      c[0] = unpackFixed16Edram(lo & 0xFFFFu);
      c[1] = unpackFixed16Edram(lo >> 16);
      break;
    case 5:
      c[0] = unpackFixed16Edram(lo & 0xFFFFu); c[1] = unpackFixed16Edram(lo >> 16);
      c[2] = unpackFixed16Edram(hi & 0xFFFFu); c[3] = unpackFixed16Edram(hi >> 16);
      break;
    case 6:
      c[0] = halfToFloat(uint16_t(lo)); c[1] = halfToFloat(uint16_t(lo >> 16));
      break;
    case 7:
      c[0] = halfToFloat(uint16_t(lo)); c[1] = halfToFloat(uint16_t(lo >> 16));
      c[2] = halfToFloat(uint16_t(hi)); c[3] = halfToFloat(uint16_t(hi >> 16));
      break;
    case 14:
      c[0] = u2f(lo);
      break;
    case 15:
      c[0] = u2f(lo); c[1] = u2f(hi);
      break;
    default:
      break;
  }
}
// Depth: host value is D32 float in [0,1]. D24S8 stores unorm24; D24FS8
// stores 20e4 of the GUEST value, and the host holds guest * 0.5
// (draw_util.cc:517-537 host-RT convention).
inline uint32_t packDepth(uint32_t depthFmt, float hostDepth, uint32_t stencil) {
  uint32_t d24;
  if (depthFmt == 1) {
    d24 = floatTo20e4(hostDepth * 2.0f);
  } else {
    // Exact inverse of XeUNorm24To32 ((n + (n >> 23)) / 2^24): load -> store
    // -> load must be bit-exact, which a float32 `* 16777215 + 0.5` is not.
    const uint32_t m = uint32_t(double(saturate(hostDepth)) * 16777216.0 + 0.5);
    d24 = std::min(m >= 0x800001u ? m - 1u : m, 0xFFFFFFu);
  }
  return (d24 << 8) | (stencil & 0xFFu);
}
inline float unpackDepth(uint32_t depthFmt, uint32_t bits) {
  const uint32_t d24 = bits >> 8;
  if (depthFmt == 1) return f20e4ToFloat(d24) * 0.5f;
  return float(d24 + (d24 >> 23)) * (1.0f / 16777216.0f);  // XeUNorm24To32
}

// Host texel <-> float4 (the host storage chosen by hostColorFormat).
inline void hostToFloat4(HostFmt f, const uint8_t* p, float c[4]) {
  uint32_t a, b;
  c[0] = c[1] = c[2] = 0.0f;
  c[3] = 1.0f;
  switch (f) {
    case HostFmt::RGBA8:
      for (int i = 0; i < 4; i++) c[i] = float(p[i]) * (1.0f / 255.0f);
      break;
    case HostFmt::RGB10A2:
      std::memcpy(&a, p, 4);
      for (int i = 0; i < 3; i++) c[i] = float((a >> (10 * i)) & 0x3FFu) * (1.0f / 1023.0f);
      c[3] = float(a >> 30) * (1.0f / 3.0f);
      break;
    case HostFmt::RGBA16F: {
      uint16_t h[4];
      std::memcpy(h, p, 8);
      for (int i = 0; i < 4; i++) c[i] = halfToFloat(h[i]);
    } break;
    case HostFmt::RG16F: {
      uint16_t h[2];
      std::memcpy(h, p, 4);
      c[0] = halfToFloat(h[0]); c[1] = halfToFloat(h[1]);
    } break;
    case HostFmt::R32F:
    case HostFmt::D32F:
      std::memcpy(&a, p, 4);
      c[0] = u2f(a);
      break;
    case HostFmt::RG32F:
      std::memcpy(&a, p, 4); std::memcpy(&b, p + 4, 4);
      c[0] = u2f(a); c[1] = u2f(b);
      break;
    default:
      break;
  }
}
inline void float4ToHost(HostFmt f, const float c[4], uint8_t* p) {
  uint32_t a;
  switch (f) {
    case HostFmt::RGBA8:
      for (int i = 0; i < 4; i++) p[i] = uint8_t(packUNorm(c[i], 255.f));
      break;
    case HostFmt::RGB10A2:
      a = packUNorm(c[0], 1023.f) | (packUNorm(c[1], 1023.f) << 10) |
          (packUNorm(c[2], 1023.f) << 20) | (packUNorm(c[3], 3.f) << 30);
      std::memcpy(p, &a, 4);
      break;
    case HostFmt::RGBA16F: {
      const uint16_t h[4] = {floatToHalf(c[0]), floatToHalf(c[1]), floatToHalf(c[2]),
                             floatToHalf(c[3])};
      std::memcpy(p, h, 8);
    } break;
    case HostFmt::RG16F: {
      const uint16_t h[2] = {floatToHalf(c[0]), floatToHalf(c[1])};
      std::memcpy(p, h, 4);
    } break;
    case HostFmt::R32F:
    case HostFmt::D32F:
      a = f2u(c[0]);
      std::memcpy(p, &a, 4);
      break;
    case HostFmt::RG32F:
      a = f2u(c[0]); std::memcpy(p, &a, 4);
      a = f2u(c[1]); std::memcpy(p + 4, &a, 4);
      break;
    default:
      break;
  }
}

// ── Point sprites (iter-370 kPointList; p2.zpd: the HUD threat query's draw is a 1-vertex point list) ───────
// c19.xy of the shared block, the NDC radius tools/xenosrecomp/point_sprite.gs.hlsl expands a point by (times w).
// PA_SU_POINT_SIZE holds the HALF width (bits 31:16) / height (15:0) in 12.4 fixed point (Xenia registers.h:444), so
// the screen DIAMETER is field * 2/16 guest pixels; over the viewport extent (host pixels) that is the NDC radius -
// the 0.5 diameter->radius and 2.0 NDC-span factors cancel (d3d12_command_processor.cc:3950) - times the resolution
// scale sx / sy (point_screen_diameter_to_ndc_radius, :3983-3988). 0x00080008 = a 1-pixel point.
inline void pointSpriteNdcRadius(uint32_t pointSize, float extX, float extY, uint32_t sx, uint32_t sy,
                                 float out[2]) {
  const float diamX = float((pointSize >> 16) & 0xFFFFu) * (2.0f / 16.0f);
  const float diamY = float(pointSize & 0xFFFFu) * (2.0f / 16.0f);
  out[0] = extX > 0.0f ? diamX * float(sx) / extX : 0.0f;
  out[1] = extY > 0.0f ? diamY * float(sy) / extY : 0.0f;
}

// ── Host viewport (draw_util.cc:171-549, D3D12 conventions) ────────────────
struct HostViewport {
  float x = 0, y = 0, w = 0, h = 0, minZ = 0, maxZ = 1;
  float ndcScale[3] = {1, 1, 1}, ndcOffset[3] = {0, 0, 0};
  bool empty = false;
};
// iter-464 resscale: sx/sy = the draw resolution scale. As in Xenia
// (draw_util.cc:339-428), all the guest-pixel math stays in guest pixels - the
// limit is x_max / scale, the 0.5 half-pixel offset lives in ndcOffset - and only
// the host viewport's offset and extent are multiplied by the scale. 1x is the
// unscaled function bit for bit.
inline HostViewport computeViewport(const Regs& r, bool psWritesDepth, bool depthFloat24,
                                    uint32_t sx = 1, uint32_t sy = 1) {
  const uint32_t vte = r.vteCntl;
  const float scale[3] = {(vte & 1u) ? r.vpScale[0] : 1.0f, (vte & 4u) ? r.vpScale[1] : 1.0f,
                          (vte & 16u) ? r.vpScale[2] : 1.0f};
  const float offBase[3] = {(vte & 2u) ? r.vpOffset[0] : 0.0f,
                            (vte & 8u) ? r.vpOffset[1] : 0.0f,
                            (vte & 32u) ? r.vpOffset[2] : 0.0f};
  float add[2] = {0.0f, 0.0f};
  if ((r.suScModeCntl >> 16) & 1u) {
    add[0] = float(windowOffsetX(r));
    add[1] = float(windowOffsetY(r));
  }
  if (!(r.vtxCntl & 1u)) { add[0] += 0.5f; add[1] += 0.5f; }
  // D3D12_VIEWPORT_BOUNDS_MAX in guest pixels (draw_util.cc:339-341 MagicDiv).
  const uint32_t axisScale[2] = {std::max(sx, 1u), std::max(sy, 1u)};
  const float xyMaxAxis[2] = {float(32767u / axisScale[0]), float(32767u / axisScale[1])};
  HostViewport v;
  if ((r.clipCntl >> 16) & 1u) {
    float ext[2];
    for (int a = 0; a < 2; a++) {
      const float E = std::min(8192.0f, xyMaxAxis[a]);
      ext[a] = E * float(axisScale[a]);
      v.ndcScale[a] = scale[a] * (2.0f / E);
      v.ndcOffset[a] = (offBase[a] - E * 0.5f + add[a]) * (2.0f / E);
    }
    v.x = v.y = 0.0f;
    v.w = ext[0];
    v.h = ext[1];
    v.minZ = 0.0f;
    v.maxZ = 1.0f;
    v.ndcScale[2] = scale[2];
    v.ndcOffset[2] = offBase[2];
  } else {
    float o[2], e[2];
    for (int a = 0; a < 2; a++) {
      const float offset = offBase[a] + add[a];
      const float s = scale[a], sa = std::fabs(s);
      const uint32_t a0 = uint32_t(std::min(std::max(offset - sa, 0.0f), xyMaxAxis[a]));
      const uint32_t a1 = uint32_t(std::min(std::max(offset + sa, 0.0f), xyMaxAxis[a]));
      o[a] = float(a0 * axisScale[a]);
      e[a] = float((a1 - a0) * axisScale[a]);
      if (a1 > a0) {
        v.ndcScale[a] = s * 2.0f / float(a1 - a0);
        v.ndcOffset[a] = (offset - (float(a0) + float(a1 - a0) * 0.5f)) * 2.0f / float(a1 - a0);
      } else {
        v.ndcScale[a] = 1.0f;
        v.ndcOffset[a] = 0.0f;
        v.empty = true;
      }
    }
    v.x = o[0]; v.y = o[1]; v.w = e[0]; v.h = e[1];
    float hostOff, hostScale;
    if ((r.clipCntl >> 19) & 1u) {
      hostOff = offBase[2]; hostScale = scale[2];
      v.ndcScale[2] = 1.0f; v.ndcOffset[2] = 0.0f;
    } else {
      hostOff = offBase[2] - scale[2]; hostScale = 2.0f * scale[2];
      v.ndcScale[2] = 0.5f; v.ndcOffset[2] = 0.5f;
    }
    if (psWritesDepth) {
      v.minZ = 0.0f; v.maxZ = 1.0f;
    } else {
      v.minZ = saturate(hostOff);
      v.maxZ = saturate(hostOff + hostScale);
      if (v.minZ > v.maxZ) {
        std::swap(v.minZ, v.maxZ);
        v.ndcScale[2] = -v.ndcScale[2];
        v.ndcOffset[2] = 1.0f - v.ndcOffset[2];
      }
    }
  }
  if (depthFloat24) { v.minZ *= 0.5f; v.maxZ *= 0.5f; }
  v.ndcScale[1] = -v.ndcScale[1];   // origin_bottom_left (D3D12)
  v.ndcOffset[1] = -v.ndcOffset[1];
  return v;
}

// ── Resolve (draw_util.cc:926-1318) ────────────────────────────────────────
// ui/graphics_util.cc FloatToD3D11Fixed16p8.
inline int32_t floatToFixed16p8(float f32) {
  if (!(std::fabs(f32) >= 1.0f / 512.0f)) return 0;
  if (f32 >= 32768.0f - 1.0f / 256.0f) return (1 << 23) - 1;
  if (f32 <= -32768.0f) return -32768 * 256;
  const uint32_t bits = f2u(f32);
  union { int32_t s; uint32_t u; } res;
  res.u = (bits & ((1u << 23) - 1u)) | (1u << 23);
  if (bits >> 31) res.s = -res.s;
  const int32_t exponent = int32_t((bits >> 23) & 255u) - 127;
  const uint32_t extra = uint32_t(15 - exponent);
  if (extra) {
    res.u += (1u << (extra - 1)) - 1u + ((res.u >> extra) & 1u);
    res.s >>= extra;
  }
  return res.s;
}

struct ResolveInfo {
  bool valid = false;         // a resolve to execute (possibly with no copy)
  bool copy = false;          // copy_command raw/convert with a non-empty rect
  bool raw = false;           // copy_command == kRaw (bitwise)
  bool isDepth = false;
  uint32_t srcIndex = 0, sampleSelect = 0, msaa = 0, surfacePitch = 0;
  uint32_t srcBase = 0, srcFormat = 0;  // color resource format or depth format
  bool src64 = false;
  RtKey srcKey;                          // the EDRAM surface the copy reads
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // surface pixels, 8-aligned
  uint32_t destBase = 0, destPitch = 0, destHeight = 0;  // pitch/height 32-aligned
  uint32_t destFormat = 0, destEndian = 0, destSlice = 0;
  bool destSwap = false, destArray = false;
  float expBiasFactor = 1.0f;
  bool clearColor = false, clearDepth = false;
  RtKey colorClearKey, depthClearKey;
};

// copy_sample_select sanitizing (draw_util.cc:839-880). k01 = 4, k23 = 5, k0123 = 6.
inline uint32_t sanitizeSampleSelect(uint32_t s, uint32_t msaa, bool depth) {
  if (msaa == kMsaa1X) return 0;
  if (msaa == kMsaa2X) {
    if (s == 2) s = 0;
    else if (s == 3) s = 1;
    else if (s > 4) s = 4;
    if (depth && s == 4) s = 0;
    return s;
  }
  if (s > 6) s = 6;
  if (depth) {
    if (s == 4 || s == 6) s = 0;
    else if (s == 5) s = 2;
  }
  return s;
}

inline ResolveInfo computeResolve(const Regs& r) {
  ResolveInfo ri;
  const uint32_t cc = r.copyControl;
  const uint32_t cmd = (cc >> 20) & 3u;
  ri.srcIndex = cc & 7u;
  ri.isDepth = ri.srcIndex >= 4u;
  ri.msaa = surfaceMsaa(r);
  ri.surfacePitch = surfacePitch(r);
  if (ri.msaa > kMsaa4X) return ri;
  ri.valid = true;
  const uint32_t pt = pitchTiles32(ri.surfacePitch, ri.msaa);
  // Clear targets (draw_util.h:688-716).
  ri.clearDepth = ((cc >> 9) & 1u) != 0;
  ri.clearColor = !ri.isDepth && ((cc >> 8) & 1u) != 0;
  ri.depthClearKey = RtKey::make(r.depthInfo & 0x7FFu, pt, ri.msaa, true, (r.depthInfo >> 16) & 1u);
  if (!ri.isDepth) {
    const uint32_t ci = r.colorInfo[ri.srcIndex & 3u];
    ri.srcBase = ci & 0x7FFu;
    ri.srcFormat = colorResourceFormat((ci >> 16) & 0xFu);
    ri.src64 = colorIs64bpp(ri.srcFormat);
    ri.srcKey = RtKey::make(ri.srcBase, pt, ri.msaa, false, ri.srcFormat);
    ri.colorClearKey = ri.srcKey;
  } else {
    ri.srcBase = r.depthInfo & 0x7FFu;
    ri.srcFormat = (r.depthInfo >> 16) & 1u;
    ri.srcKey = ri.depthClearKey;
  }
  ri.sampleSelect = sanitizeSampleSelect((cc >> 4) & 7u, ri.msaa, ri.isDepth);
  if (!r.resolveVertsValid || (cmd != 0u && cmd != 1u)) return ri;
  ri.raw = cmd == 0u;

  const float hp = (r.vtxCntl & 1u) ? 0.0f : 0.5f;
  int32_t fx[6];
  for (int i = 0; i < 6; i++) fx[i] = floatToFixed16p8(r.resolveVerts[i] + hp);
  int32_t x0 = std::min(std::min(fx[0], fx[2]), fx[4]);
  int32_t y0 = std::min(std::min(fx[1], fx[3]), fx[5]);
  int32_t x1 = std::max(std::max(fx[0], fx[2]), fx[4]);
  int32_t y1 = std::max(std::max(fx[1], fx[3]), fx[5]);
  x0 = (x0 + 127) >> 8; y0 = (y0 + 127) >> 8;
  x1 = (x1 + 127) >> 8; y1 = (y1 + 127) >> 8;
  if ((r.suScModeCntl >> 16) & 1u) {
    x0 += windowOffsetX(r); x1 += windowOffsetX(r);
    y0 += windowOffsetY(r); y1 += windowOffsetY(r);
  }
  const Scissor s = computeScissor(r, false);
  x0 = std::clamp(x0, s.x, s.x + s.w); x1 = std::clamp(x1, s.x, s.x + s.w);
  y0 = std::clamp(y0, s.y, s.y + s.h); y1 = std::clamp(y1, s.y, s.y + s.h);
  x0 &= ~int32_t(kResolveAlign - 1); y0 &= ~int32_t(kResolveAlign - 1);
  x1 = (x1 + int32_t(kResolveAlign) - 1) & ~int32_t(kResolveAlign - 1);
  y1 = (y1 + int32_t(kResolveAlign) - 1) & ~int32_t(kResolveAlign - 1);
  const int32_t pitchAligned = int32_t(ri.surfacePitch & ~(kResolveAlign - 1));
  x0 = std::min(x0, pitchAligned); x1 = std::min(x1, pitchAligned);
  if (y1 - y0 > int32_t(kMaxResolveSize)) y1 = y0 + int32_t(kMaxResolveSize);
  if (x0 >= x1 || y0 >= y1) return ri;
  ri.x0 = uint32_t(x0); ri.y0 = uint32_t(y0);
  ri.x1 = uint32_t(x1); ri.y1 = uint32_t(y1);

  const uint32_t di = r.copyDestInfo;
  ri.destBase = r.copyDestBase;
  ri.destPitch = ((r.copyDestPitch & 0x3FFFu) + 31u) & ~31u;
  ri.destHeight = (((r.copyDestPitch >> 16) & 0x3FFFu) + 31u) & ~31u;
  ri.destEndian = di & 7u;
  ri.destArray = ((di >> 3) & 1u) != 0;
  ri.destSlice = (di >> 4) & 7u;
  if (ri.isDepth) {
    ri.destFormat = ri.srcFormat == 1u ? 23u : 22u;  // k_24_8_FLOAT : k_24_8
    ri.destSwap = false;
    ri.expBiasFactor = 1.0f;
  } else {
    ri.destFormat = (di >> 7) & 0x3Fu;
    ri.destSwap = ((di >> 24) & 1u) != 0;
    ri.expBiasFactor = std::ldexp(1.0f, sext((di >> 16) & 0x3Fu, 6));
  }
  ri.copy = true;
  return ri;
}

// Bytes per texel of a resolvable destination TextureFormat (0 = unsupported).
inline uint32_t destBytesPerTexel(uint32_t fmt) {
  switch (fmt) {
    case 2: return 1;                                            // k_8
    case 3: case 4: case 5: case 10: case 15: case 24: case 30:  // 16bpp
      return 2;
    case 6: case 7: case 16: case 17: case 22: case 23: case 25: case 31:
    case 33: case 36:
      return 4;
    case 26: case 32: case 34: case 37: return 8;
    case 35: case 38: return 16;
    default: return 0;
  }
}

// Pack one resolved pixel into the destination TextureFormat (little-endian
// GPU value, pixel_formats.xesli XePack*). Returns bytes written.
inline uint32_t packDest(uint32_t fmt, const float c[4], uint8_t* out) {
  auto put32 = [&](uint32_t v) { std::memcpy(out, &v, 4); return 4u; };
  auto put16 = [&](uint32_t v) { const uint16_t h = uint16_t(v); std::memcpy(out, &h, 2); return 2u; };
  switch (fmt) {
    case 2: out[0] = uint8_t(packUNorm(c[0], 255.f)); return 1;
    case 3: return put16(packUNorm(c[0], 31.f) | (packUNorm(c[1], 31.f) << 5) |
                         (packUNorm(c[2], 31.f) << 10) | (packUNorm(c[3], 1.f) << 15));
    case 4: return put16(packUNorm(c[0], 31.f) | (packUNorm(c[1], 63.f) << 5) |
                         (packUNorm(c[2], 31.f) << 11));
    case 5: return put16(packUNorm(c[0], 31.f) | (packUNorm(c[1], 31.f) << 5) |
                         (packUNorm(c[2], 63.f) << 10));
    case 10: return put16(packUNorm(c[0], 255.f) | (packUNorm(c[1], 255.f) << 8));
    case 15: return put16(packUNorm(c[0], 15.f) | (packUNorm(c[1], 15.f) << 4) |
                          (packUNorm(c[2], 15.f) << 8) | (packUNorm(c[3], 15.f) << 12));
    case 24: return put16(packUNorm(c[0], 65535.f));
    case 30: return put16(floatToHalf(c[0]));
    case 6:
      return put32(packUNorm(c[0], 255.f) | (packUNorm(c[1], 255.f) << 8) |
                   (packUNorm(c[2], 255.f) << 16) | (packUNorm(c[3], 255.f) << 24));
    case 7:
      return put32(packUNorm(c[0], 1023.f) | (packUNorm(c[1], 1023.f) << 10) |
                   (packUNorm(c[2], 1023.f) << 20) | (packUNorm(c[3], 3.f) << 30));
    case 16:  // 10_11_11: XePackR11G11B10UNorm
      return put32(packUNorm(c[0], 2047.f) | (packUNorm(c[1], 2047.f) << 11) |
                   (packUNorm(c[2], 1023.f) << 22));
    case 17:  // 11_11_10: XePackR10G11B11UNorm
      return put32(packUNorm(c[0], 1023.f) | (packUNorm(c[1], 2047.f) << 10) |
                   (packUNorm(c[2], 2047.f) << 21));
    case 25:
      return put32(packUNorm(c[0], 65535.f) | (packUNorm(c[1], 65535.f) << 16));
    case 31:
      return put32(floatToHalf(c[0]) | (uint32_t(floatToHalf(c[1])) << 16));
    case 26: {
      const uint32_t a = packUNorm(c[0], 65535.f) | (packUNorm(c[1], 65535.f) << 16);
      const uint32_t b = packUNorm(c[2], 65535.f) | (packUNorm(c[3], 65535.f) << 16);
      std::memcpy(out, &a, 4); std::memcpy(out + 4, &b, 4);
      return 8;
    }
    case 32: {
      const uint32_t a = floatToHalf(c[0]) | (uint32_t(floatToHalf(c[1])) << 16);
      const uint32_t b = floatToHalf(c[2]) | (uint32_t(floatToHalf(c[3])) << 16);
      std::memcpy(out, &a, 4); std::memcpy(out + 4, &b, 4);
      return 8;
    }
    case 34: case 37: {
      const uint32_t a = f2u(c[0]), b = f2u(c[1]);
      std::memcpy(out, &a, 4); std::memcpy(out + 4, &b, 4);
      return 8;
    }
    case 35: case 38:
      for (int i = 0; i < 4; i++) { const uint32_t a = f2u(c[i]); std::memcpy(out + 4 * i, &a, 4); }
      return 16;
    default:  // 32bpp "treat as 32_FLOAT" (k_32, k_32_FLOAT, 24_8 via convert)
      return put32(f2u(c[0]));
  }
}

// Endian swap of one unit in place (shaders/endian.xesli). 1 = 8in16,
// 2 = 8in32, 3 = 16in32, 4 = 8in64, 5 = 8in128. Unit size per mode: 2/4/4/8/16.
inline uint32_t endianUnitBytes(uint32_t endian) {
  switch (endian) { case 1: return 2; case 2: case 3: return 4; case 4: return 8; case 5: return 16; default: return 1; }
}
inline void endianSwapUnit(uint32_t endian, uint8_t* p) {
  switch (endian) {
    case 1: std::swap(p[0], p[1]); break;
    case 2: std::reverse(p, p + 4); break;
    case 3: { uint8_t t[4]; std::memcpy(t, p, 4); p[0] = t[2]; p[1] = t[3]; p[2] = t[0]; p[3] = t[1]; } break;
    case 4: { uint8_t t[8]; std::memcpy(t, p, 8); std::memcpy(p, t + 4, 4); std::memcpy(p + 4, t, 4);
              std::reverse(p, p + 4); std::reverse(p + 4, p + 8); } break;
    case 5: { uint8_t t[16]; std::memcpy(t, p, 16);
              for (int d = 0; d < 4; d++) std::memcpy(p + 4 * d, t + 4 * (3 - d), 4);
              for (int d = 0; d < 4; d++) std::reverse(p + 4 * d, p + 4 * d + 4); } break;
    default: break;
  }
}

}  // namespace edram
}  // namespace gfx
}  // namespace sr
