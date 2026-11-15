#pragma once
// cube_layout.hpp — the GUEST memory layout of an Xbox 360 cube texture.
//
// ONE BODY, four callers: the producer (what to snapshot), the recorder (how
// long the payload is), the replayer (whether a record's payload agrees with
// its own geometry), and the executor (how many subresources to upload). They
// MUST agree, and the only way to guarantee that is for there to be one
// function. iter-456n's fifth adjudication failed on exactly this — a helper
// that was "shared" in name while a call site duplicated its span math.
//
// ⛔ THE AUTHORITY IS XENIA, NOT INTUITION.
// The six-face span is NOT `6 * w * h * bpp`. It is
// `array_slice_stride_bytes * 6`, where the stride is the tile-aligned row
// pitch times the tile-aligned block-row count, aligned up to 4096
// (reference/xenia-canary/src/xenia/gpu/texture_util.cc:312-352). For the
// 128x128 k_8_8_8_8 cubes this fixture actually contains those two happen to
// agree, which is precisely why deriving it from intuition would look correct
// and generalise wrong — at 33x33 the authority says 32,768 per face and the
// intuition says 4,356.
//
// Every function here is a port of a named Xenia function, cited at its
// definition, and is checked against the independent Python port in
// tools/check_cube_layout.py (40 fixtures, 21 mutants).
//
// Scope: 2D/stacked/cube only. A 3D dimension REFUSES — its stride multiplies
// by align(depth, kTextureTileDepth) and its extent needs a 3D address bound,
// neither of which is ported here, and silently returning a 2D-shaped answer
// for a volume is the exact class of error this header exists to prevent.
#include "xenos_tiling.hpp"
// ⚠ ONE block-geometry table, not two. kernel/tex_layout.hpp's `blockGeomFor`
// is already designated the single source that the capture site, this header
// and the backend's decode switch must all agree on, and gfx/pm4_engine.cpp
// already reaches for it the same way ("the ONE canonical fetch decoder").
// Duplicating the switch here is how the two silently diverge.
#include "../kernel/tex_layout.hpp"

#include <cstdint>

namespace sr::gfx::cube {

// ── Xenos constants (reference/.../gpu/xenos.h) ─────────────────────────────
constexpr uint32_t kTileWH = 32;          // :1198-1199 kTextureTileWidthHeight
constexpr uint32_t kSubresAlign = 4096;   // :1177-1178, also the base/mip shift
constexpr uint32_t kLinearRowAlign = 256; // :1225
constexpr uint32_t kCubeFaces = 6;        // texture_util.cc:229

// Why a cube snapshot was refused. A refusal always names itself — a silent
// fallback to the 2D path is the fail-open this whole slice exists to close.
enum class CubeRefuse : uint32_t {
  None = 0,
  NotCube,          // the fetch dimension is not kCube
  StackDepth,       // size_2d.stack_depth != 5 (texture_util.cc:52)
  Format,           // no block geometry for this format
  Extent,           // zero or implausible width/height
  Pitch,            // pitch cannot address the width
  MipLevels,        // mip level count outside 0..7
  Span,             // the computed span does not fit the capture budget
};

inline const char* cubeRefuseName(CubeRefuse r) {
  switch (r) {
    case CubeRefuse::None:       return "none";
    case CubeRefuse::NotCube:    return "notCube";
    case CubeRefuse::StackDepth: return "stackDepth";
    case CubeRefuse::Format:     return "format";
    case CubeRefuse::Extent:     return "extent";
    case CubeRefuse::Pitch:      return "pitch";
    case CubeRefuse::MipLevels:  return "mipLevels";
    case CubeRefuse::Span:       return "span";
    default:                     return "unknown";
  }
}

namespace detail {

inline uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

inline uint32_t log2Floor(uint32_t v) {
  uint32_t n = 0;
  while (v > 1u) { v >>= 1; n++; }
  return n;
}

inline uint32_t log2Ceil(uint32_t v) {
  const uint32_t n = log2Floor(v);
  return (1u << n) == v ? n : n + 1u;
}

inline uint32_t nextPow2(uint32_t v) { return v ? (1u << log2Ceil(v)) : 1u; }

// texture_util.cc:450-477 GetTiledAddressUpperBound2D.
inline uint32_t tiledUpperBound2D(uint32_t right, uint32_t bottom,
                                  uint32_t pitchAligned, uint32_t bpbLog2) {
  if (!right || !bottom) return 0;
  uint32_t ub = (uint32_t)sr::gfx::xenos_tiling::tiled2D(
      (int32_t)((right - 1u) & ~(kTileWH - 1u)),
      (int32_t)((bottom - 1u) & ~(kTileWH - 1u)), pitchAligned, bpbLog2);
  if (bpbLog2 == 0)      ub += 0xA00u;
  else if (bpbLog2 == 1) ub += 0xC00u;
  else                   ub += 0x400u << bpbLog2;
  return ub;
}

// texture_util.h:41-44 GetPackedMipLevel.
inline uint32_t packedMipLevel(uint32_t w, uint32_t h) {
  const uint32_t s = log2Ceil(w < h ? w : h);
  return s > 4u ? s - 4u : 0u;
}

// texture_util.cc:116-205 GetPackedMipOffset. Returns false when `mip` is not
// inside the packed tail. Offsets come back in BLOCKS.
inline bool packedMipOffset(uint32_t width, uint32_t height, uint32_t blockW,
                            uint32_t blockH, uint32_t mip, uint32_t& xBlocks,
                            uint32_t& yBlocks) {
  const uint32_t log2W = log2Ceil(width), log2H = log2Ceil(height);
  const uint32_t log2Size = log2W < log2H ? log2W : log2H;
  if (log2Size > 4u + mip) { xBlocks = yBlocks = 0; return false; }
  const uint32_t base = log2Size > 4u ? log2Size - 4u : 0u;
  const uint32_t pm = mip - base;
  if (pm < 3u) {
    // ⚠ A SQUARE takes the ELSE arm. Getting this backwards is the easiest
    // error in the function and is invisible on any square texture.
    if (log2W > log2H) { xBlocks = 0; yBlocks = 16u >> pm; }
    else               { xBlocks = 16u >> pm; yBlocks = 0; }
  } else {
    if (log2W > log2H) { xBlocks = (1u << (log2W - base)) >> (pm - 2u); yBlocks = 0; }
    else               { xBlocks = 0; yBlocks = (1u << (log2H - base)) >> (pm - 2u); }
  }
  // texture_util.cc:203-204 — the shifts are log2 of the block dimensions
  // (texture_info_formats.cc:45-53 `GetShift(block_width)`).
  xBlocks >>= log2Floor(blockW);
  yBlocks >>= log2Floor(blockH);
  return true;
}

// The per-level array-slice stride, texture_util.cc:312-352.
// ⛔ `isBase` is a SEPARATE input from `level`. When packed_level == 0 the
// mip tail is stored as mips[0] (level is 0) but still uses the mip formula
// (next_pow2 of the base size), not the fetch-constant pitch. Collapsing
// "level == 0" with "is the base" is the r2 defect: it made the mip region
// vanish for every packed_level == 0 cube.
inline uint32_t levelSliceStride(uint32_t level, bool isBase, uint32_t width,
                                 uint32_t height, uint32_t pitchTexels,
                                 uint32_t blockW, uint32_t blockH, uint32_t bpb,
                                 bool tiled,
                                 uint32_t* rowPitchBlocksOut = nullptr) {
  uint32_t rowPitchTexels, zRows;
  if (isBase) {
    rowPitchTexels = pitchTexels;
    zRows = height;
  } else {
    const uint32_t w = nextPow2(width) >> level, h = nextPow2(height) >> level;
    rowPitchTexels = w ? w : 1u;
    zRows = h ? h : 1u;
  }
  const uint32_t rowPitchBlocks =
      alignUp(alignUp(rowPitchTexels, blockW) / blockW, kTileWH);
  if (rowPitchBlocksOut) *rowPitchBlocksOut = rowPitchBlocks;
  uint32_t rowPitchBytes = rowPitchBlocks * bpb;
  if (!tiled && !isBase)
    rowPitchBytes = alignUp(rowPitchBytes, kLinearRowAlign);
  const uint32_t zBlockRows = alignUp(alignUp(zRows, blockH) / blockH, kTileWH);
  return alignUp(rowPitchBytes * zBlockRows, kSubresAlign);
}

}  // namespace detail

// Block geometry for a cube's format, from the shared table. `blockH == blockW`
// for every Xenos format in it (DXT is 4x4, everything else 1x1); a format the
// backend cannot decode returns false so the caller REFUSES rather than
// capturing pixels nothing can read.
inline bool cubeBlockGeom(uint32_t fmt, uint32_t& blockW, uint32_t& blockH,
                          uint32_t& bpb) {
  const auto g = sr::kernel::texlayout::blockGeomFor(fmt);
  blockW = g.blockW;
  blockH = g.blockW;
  bpb = g.blockBytes;
  return g.supported;
}

// The fully decoded guest layout of one cube texture.
// Xenos stores at most 8 mip levels (computeCubeLayout refuses more), so the
// per-level arrays are fixed size - no allocation in a header used by a
// hot-path uploader.
inline constexpr uint32_t kMaxMipLevels = 14;  // levels 0..13 (8192), [0] unused for
                                               // a packed_level > 0 texture

struct CubeLayout {
  bool     ok = false;
  CubeRefuse refuse = CubeRefuse::NotCube;
  uint32_t faces = kCubeFaces;
  uint32_t faceStrideBytes = 0;    // one face of the BASE level
  uint32_t baseSpanBytes = 0;      // faceStrideBytes * 6
  uint32_t mipLevels = 1;          // 1 = base only
  uint32_t packedLevel = 0;
  bool     packedMips = false;
  uint32_t mipsTotalExtentBytes = 0;
  uint32_t totalBytes = 0;         // baseSpanBytes + mipsTotalExtentBytes
  // ── iter-457 H1(b): WHERE EACH STORED MIP ACTUALLY LIVES ─────────────────
  // The loop below already computes both of these; until H1(b) it discarded
  // them and reported only the total. An uploader cannot place a mip from a
  // total - it needs the offset of the level and the per-face stride within
  // it, which is what `check_cube_layout.py` has been PRINTING all along
  // ("mip offsets {1: 0, 2: 98304, 3: 122880} strides {1: 16384, ...}").
  // Recording them here is what lets the C++ body and the Python port be
  // checked against each other instead of trusted separately.
  //
  // Indexed by LEVEL. For a packed_level > 0 cube, [0] is unused - the base
  // lives in the base span. For packed_level == 0, Xenia stores the MIP tail
  // as mips[0] (texture_util.cc:284-299): a SECOND packed tail, separate
  // from the base, and [0] is the only stored mip-region entry.
  uint32_t mipOffsetBytes[kMaxMipLevels] = {};
  uint32_t mipSliceStride[kMaxMipLevels] = {};
  uint32_t lastStoredLevel = 0;
  // ── iter-457 H1(b)-r2: THE PACKED TAIL'S REAL EXTENT, IN BLOCKS ──────────
  // The tail slice is WIDER than the level stored in it. For 128x128 the tail
  // level is 16x16 but its sublevels sit beside it - mip 3 at x=16 - so the
  // extent is 32x16 blocks. Decoding the tail at the LEVEL's size would read a
  // 16x16 window and miss every sublevel, which is a cube whose distant mips
  // are garbage. The loop below already computes this max and used to discard
  // it.
  uint32_t packedTailExtentXBlocks = 0;
  uint32_t packedTailExtentYBlocks = 0;
  // ── iter-457 H1(b)-r3: THE BASE PACKED TAIL (packed_level == 0 only) ─────
  // Xenia treats packed_level == 0 as TWO tails (texture_util.cc:284-289):
  // loop iteration 0 is the BASE packed tail (mip 0 only), iteration 1 is
  // the MIPS packed tail (sublevels 0..max_level). They have independent
  // extents: a 1x17 has base y=17 and mips[0] y=18.
  uint32_t basePackedTailExtentXBlocks = 0;
  uint32_t basePackedTailExtentYBlocks = 0;
};

// `mipLevels` is the COUNT (1 = base only, 8 = levels 0..7). `packedMips` is
// the Xenos f[5] bit 11 — it is a NAMED input and is never inferred from a byte
// count, because two textures with equal mip bytes can need different decodes.
// `faces` is the array size: 6 for a cube (computeCubeLayout), 1 for a plain 2D
// texture, N for a stacked one (P1: the PM4 path's 2D mip chains) - Xenia's
// GetGuestTextureLayout is the same body for all three.
inline CubeLayout computeGuestLayout(uint32_t fmt, uint32_t width, uint32_t height,
                                     uint32_t pitchTexels, bool tiled,
                                     uint32_t mipLevels, bool packedMips,
                                     uint32_t blockW, uint32_t blockH,
                                     uint32_t bpb, uint32_t faces) {
  CubeLayout L{};
  if (!blockW || !blockH || !bpb) { L.refuse = CubeRefuse::Format; return L; }
  if (!width || !height || width > 8192u || height > 8192u) {
    L.refuse = CubeRefuse::Extent; return L;
  }
  if (!pitchTexels || pitchTexels < width) { L.refuse = CubeRefuse::Pitch; return L; }
  // iter-463 D4b: the 8-level cap is computeCubeLayout's (the H1(b) capture
  // contract); Xenia's layout has no such limit - a 256x256 cube carries 9
  // levels (xenia GetGuestTextureLayout, texture_util.cc:244-248).
  const uint32_t maxCount = kMaxMipLevels - 1u;
  if (!mipLevels || mipLevels > maxCount) { L.refuse = CubeRefuse::MipLevels; return L; }
  if (!faces) { L.refuse = CubeRefuse::Extent; return L; }

  L.faces = faces;
  L.mipLevels = mipLevels;
  L.packedMips = packedMips;
  L.packedLevel = packedMips ? detail::packedMipLevel(width, height) : 0xFFFFFFFFu;
  // texture_util.cc:258-294. The loop counter means TWO different things
  // when packed_level == 0: iteration 0 is the BASE packed tail, iteration 1
  // is the MIPS packed tail, and both are "level 0" stored in different
  // regions. r2 collapsed that into a loop starting at level 1, so a cube
  // whose packed tail IS mip 0 recorded no mip region and then refused
  // every mip 1+ because mipSliceStride[0] stayed 0.
  const uint32_t maxLevel = mipLevels - 1u;
  const uint32_t lastStored =
      packedMips ? (maxLevel < L.packedLevel ? maxLevel : L.packedLevel) : maxLevel;
  L.lastStoredLevel = lastStored;
  uint32_t loopLevelLast = lastStored;
  if (packedMips && L.packedLevel == 0)
    loopLevelLast = (maxLevel != 0u) ? 1u : 0u;

  uint32_t mipOffset = 0, total = 0;
  for (uint32_t loopLevel = 0; loopLevel <= loopLevelLast; loopLevel++) {
    const bool isBase = loopLevel == 0;
    const uint32_t level =
        (packedMips && L.packedLevel == 0) ? 0u : loopLevel;
    uint32_t rowPitchBlocks = 0;
    const uint32_t stride = detail::levelSliceStride(
        level, isBase, width, height, pitchTexels, blockW, blockH, bpb, tiled,
        &rowPitchBlocks);
    uint32_t xExtent = 0, yExtent = 0;
    if (packedMips && level == L.packedLevel) {
      // THE PACKED TAIL — the used region is the max over EVERY sublevel that
      // lives in it, not just this level. ⛔ Not a simplification: a skinny
      // 1x17 has its maximum at sublevel 3, not at the packed level.
      // texture_util.cc:374: the BASE tail sums only sublevel 0; the MIPS
      // tail sums 0..max_level.
      const uint32_t subLast = isBase ? 0u : maxLevel;
      for (uint32_t sub = L.packedLevel; sub <= subLast; sub++) {
        uint32_t px = 0, py = 0;
        detail::packedMipOffset(width, height, blockW, blockH, sub, px, py);
        const uint32_t ow = width >> sub, oh = height >> sub;
        const uint32_t x = px + detail::alignUp(ow ? ow : 1u, blockW) / blockW;
        const uint32_t y = py + detail::alignUp(oh ? oh : 1u, blockH) / blockH;
        if (x > xExtent) xExtent = x;
        if (y > yExtent) yExtent = y;
      }
      if (isBase) {
        L.basePackedTailExtentXBlocks = xExtent;
        L.basePackedTailExtentYBlocks = yExtent;
      } else {
        L.packedTailExtentXBlocks = xExtent;
        L.packedTailExtentYBlocks = yExtent;
      }
    } else {
      const uint32_t ow = width >> level, oh = height >> level;
      xExtent = detail::alignUp(ow ? ow : 1u, blockW) / blockW;
      yExtent = detail::alignUp(oh ? oh : 1u, blockH) / blockH;
    }
    uint32_t sliceExtent;
    if (tiled) {
      sliceExtent = detail::tiledUpperBound2D(xExtent, yExtent, rowPitchBlocks,
                                              detail::log2Floor(bpb));
    } else {
      const uint32_t rowPitchBytes = rowPitchBlocks * bpb;
      sliceExtent = rowPitchBytes * (yExtent ? yExtent - 1u : 0u) + bpb * xExtent;
    }
    if (isBase) {
      L.faceStrideBytes = stride;
      L.baseSpanBytes = stride * faces;
    } else {
      const uint32_t levelExtent = stride * (faces - 1u) + sliceExtent;
      const uint32_t end = mipOffset + levelExtent;
      if (end > total) total = end;
      if (level < kMaxMipLevels) {
        L.mipOffsetBytes[level] = mipOffset;
        L.mipSliceStride[level] = stride;
      }
      mipOffset += stride * faces;
    }
  }
  L.mipsTotalExtentBytes = total;
  L.totalBytes = L.baseSpanBytes + L.mipsTotalExtentBytes;
  L.ok = true;
  L.refuse = CubeRefuse::None;
  return L;
}

inline CubeLayout computeCubeLayout(uint32_t fmt, uint32_t width, uint32_t height,
                                    uint32_t pitchTexels, bool tiled,
                                    uint32_t mipLevels, bool packedMips,
                                    uint32_t blockW, uint32_t blockH,
                                    uint32_t bpb) {
  if (mipLevels > 8u) {  // the H1(b) capture contract (kept as it was before iter-463)
    CubeLayout L{};
    L.refuse = CubeRefuse::MipLevels;
    return L;
  }
  return computeGuestLayout(fmt, width, height, pitchTexels, tiled, mipLevels,
                            packedMips, blockW, blockH, bpb, kCubeFaces);
}


// ── iter-457 H1(b)-r2: WHERE ONE SUBRESOURCE'S BYTES ACTUALLY ARE ──────────
//
// \u26d4 THE DEFECT THIS REPLACES. r1's backend looped mip 0..7 and treated every
// level above `lastStoredLevel` as MipDataMissing - so the ONLY cube shape the
// guest actually creates (128x128, 8 levels, packed, lastStoredLevel 3) was
// refused on mip 4, every single time. The director reproduced it on a device:
// "created TEXTURECUBE idx=1, then backend REFUSED reason=MipDataMissing".
// The feature was 100% non-functional on real data while every synthetic test
// passed, because the tests stopped at layout totals and never drove the loop.
//
// Packed levels do not lack storage. Levels `packedLevel..maxLevel` SHARE the
// one stored tail slice, each occupying a sub-rectangle at its own Xenos
// (x, y). Extracting them is the whole job.
enum class SubResRegion : uint8_t {
  Base,            // the base level, in the base span, at the origin
  Mip,             // a separately-stored mip level, in the mip region
  PackedTail,      // inside the shared mip-region tail - read the TAIL, then sub-rect it
  BasePackedTail,  // packed_level == 0: mip 0 lives in the BASE tail, not at the origin
};

struct SubResPlacement {
  bool ok = false;
  CubeRefuse refuse = CubeRefuse::MipLevels;
  SubResRegion region = SubResRegion::Base;
  uint64_t byteOffset = 0;   // within the region's buffer
  uint32_t availBytes = 0;   // the slice stride at that level
  uint32_t width = 0;        // THIS subresource's dimensions
  uint32_t height = 0;
  // For PackedTail: the tail slice to decode, and where inside it this
  // sublevel sits. Offsets are in BLOCKS, exactly as GetPackedMipOffset
  // reports them.
  uint32_t tailWidth = 0, tailHeight = 0;
  uint32_t xBlocks = 0, yBlocks = 0;
};

// `face` 0..5, `mip` 0..mipLevels-1. Pure: no reads, no device, no allocation.
inline SubResPlacement placeSubresource(const CubeLayout& L, uint32_t width,
                                        uint32_t height, uint32_t blockW,
                                        uint32_t blockH, uint32_t face,
                                        uint32_t mip) {
  SubResPlacement p;
  if (!L.ok || face >= L.faces || mip >= L.mipLevels) {
    p.refuse = CubeRefuse::MipLevels; return p; }

  p.width = width >> mip ? width >> mip : 1u;
  p.height = height >> mip ? height >> mip : 1u;

  if (mip == 0 && !(L.packedMips && L.packedLevel == 0)) {
    p.region = SubResRegion::Base;
    p.byteOffset = uint64_t(L.faceStrideBytes) * face;
    p.availBytes = L.faceStrideBytes;
    p.ok = true;
    p.refuse = CubeRefuse::None;
    return p;
  }

  const bool packedHere = L.packedMips && mip >= L.packedLevel;
  if (!packedHere) {
    if (mip >= kMaxMipLevels || !L.mipSliceStride[mip]) {
      p.refuse = CubeRefuse::MipLevels; return p; }
    p.region = SubResRegion::Mip;
    p.byteOffset = uint64_t(L.mipOffsetBytes[mip]) +
                   uint64_t(L.mipSliceStride[mip]) * face;
    p.availBytes = L.mipSliceStride[mip];
    p.ok = true;
    p.refuse = CubeRefuse::None;
    return p;
  }

  // ── packed_level == 0: mip 0 lives in the BASE packed tail ──────────────
  // texture_util.cc:284-289. The base tail holds ONLY mip 0, at its packed
  // (x, y) - for an 8x8 that is (16, 0), NOT the origin. Reading it as an
  // ordinary image at the origin is the r2 defect the director reproduced
  // against the Python port (mips_total_extent_bytes=24576, C++ said 0).
  if (mip == 0 && L.packedLevel == 0) {
    p.region = SubResRegion::BasePackedTail;
    p.byteOffset = uint64_t(L.faceStrideBytes) * face;
    p.availBytes = L.faceStrideBytes;
    p.tailWidth = L.basePackedTailExtentXBlocks * blockW;
    p.tailHeight = L.basePackedTailExtentYBlocks * blockH;
    if (!p.tailWidth || !p.tailHeight) {
      p.refuse = CubeRefuse::MipLevels; return p; }
    if (!detail::packedMipOffset(width, height, blockW, blockH, 0,
                                 p.xBlocks, p.yBlocks)) {
      p.refuse = CubeRefuse::MipLevels; return p; }
    p.ok = true;
    p.refuse = CubeRefuse::None;
    return p;
  }

  // ── THE PACKED TAIL (mip region) ────────────────────────────────────────
  // Every level from packedLevel up lives in the tail slice STORED AT
  // packedLevel. Its byte offset is that level's, not this one's.
  const uint32_t tail = L.packedLevel;
  if (tail >= kMaxMipLevels || !L.mipSliceStride[tail]) {
    p.refuse = CubeRefuse::MipLevels; return p; }
  p.region = SubResRegion::PackedTail;
  p.byteOffset = uint64_t(L.mipOffsetBytes[tail]) +
                 uint64_t(L.mipSliceStride[tail]) * face;
  p.availBytes = L.mipSliceStride[tail];
  // The EXTENT, not the level's own size - see packedTailExtentXBlocks.
  p.tailWidth = L.packedTailExtentXBlocks * blockW;
  p.tailHeight = L.packedTailExtentYBlocks * blockH;
  if (!p.tailWidth || !p.tailHeight) {
    p.refuse = CubeRefuse::MipLevels; return p; }
  // \u26a0 The sub-rect is relative to the tail image. detail::packedMipOffset
  // returns FALSE when the level is not in the tail at all, which for a level
  // we already know is >= packedLevel means the layout disagrees with itself -
  // refuse rather than place at (0,0), which would silently overwrite the
  // tail's own base sublevel.
  if (!detail::packedMipOffset(width, height, blockW, blockH, mip,
                               p.xBlocks, p.yBlocks)) {
    p.refuse = CubeRefuse::MipLevels; return p; }
  p.ok = true;
  p.refuse = CubeRefuse::None;
  return p;
}

}  // namespace sr::gfx::cube
