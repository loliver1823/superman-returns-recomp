#pragma once
// volume_layout.hpp - the GUEST memory layout of an Xbox 360 3D (volume) texture, every level.
//
// Ported from Xenia (reference/xenia-canary/src/xenia/gpu/texture_util.cc: GetGuestTextureLayout for
// xenos::DataDimension::k3D, GetPackedMipOffset, GetTiledAddressUpperBound3D; texture_util.h
// GetPackedMipLevel). Xenia is BSD-3-Clause, Copyright Ben Vanik (LICENSE-xenia.txt ships with the exe).
// Each function cites its source; the variable names follow Xenia's so the two read side by side.
//
// Why a separate header: gfx/cube_layout.hpp ports the same function for 2D / stacked / cube and REFUSES a
// volume on purpose - a volume's stride multiplies by align(depth, kTextureTileDepth) and its extent needs
// the 3D address bound. Both are here. The 2D header is not touched.
//
// Consumer: D3D12Backend::getOrCreateTexture3D under SR_TEX3D_MIPS (iter-467 p2, research/p2 5007e489; ported
// to dev by step-4 lane C on 2026-10-18). Before it, our volumes loaded the base level only (MipLevels 1,
// mip_address ignored) and read the base with the slice STRIDE rather than Xenia's data EXTENT, so a minified
// 64^3 noise volume (car paint, building windows) aliased.
//
// Units: x / y in BLOCKS (4x4 for DXT, 1x1 otherwise), z in texels (volumes are never block-compressed in
// z). Offsets in bytes.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "xenos_tiling.hpp"

namespace sr {
namespace gfx {
namespace volume {

constexpr uint32_t kTileWH = 32;          // xenos::kTextureTileWidthHeight
constexpr uint32_t kTileDepth = 4;        // xenos::kTextureTileDepth
constexpr uint32_t kSubresAlign = 4096;   // xenos::kTextureSubresourceAlignmentBytes
constexpr uint32_t kLinearRowAlign = 256; // xenos::kTextureLinearRowAlignmentBytes
constexpr uint32_t kMaxLevels = 14;       // TextureGuestLayout::mips[]

inline uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1u) / a * a; }
inline uint32_t log2Floor(uint32_t v) { uint32_t r = 0; while (v >>= 1) ++r; return r; }
inline uint32_t log2Ceil(uint32_t v) { return v <= 1u ? 0u : log2Floor(v - 1u) + 1u; }
inline uint32_t nextPow2(uint32_t v) { return v <= 1u ? 1u : (1u << log2Ceil(v)); }

// texture_util.h:41 GetPackedMipLevel (2D size only, as in Xenia).
inline uint32_t packedMipLevel(uint32_t width, uint32_t height) {
  const uint32_t s = log2Ceil(std::min(width, height));
  return s > 4u ? s - 4u : 0u;
}

// texture_util.cc:116-205 GetPackedMipOffset. x / y come back in BLOCKS (shifted by the block size, as
// Xenia's GetWidthShift / GetHeightShift do), z in texels. Returns false when `mip` is not in the tail.
inline bool packedMipOffset(uint32_t width, uint32_t height, uint32_t depth, uint32_t blockLog2, uint32_t mip,
                            uint32_t& xBlocks, uint32_t& yBlocks, uint32_t& zBlocks) {
  const uint32_t log2W = log2Ceil(width), log2H = log2Ceil(height);
  const uint32_t log2Size = std::min(log2W, log2H);
  if (log2Size > 4u + mip) {
    xBlocks = yBlocks = zBlocks = 0;
    return false;
  }
  const uint32_t packedMipBase = log2Size > 4u ? log2Size - 4u : 0u;
  const uint32_t packedMip = mip - packedMipBase;
  if (packedMip < 3u) {
    if (log2W > log2H) {
      xBlocks = 0;
      yBlocks = 16u >> packedMip;
    } else {
      xBlocks = 16u >> packedMip;
      yBlocks = 0;
    }
    zBlocks = 0;
  } else {
    uint32_t offset;
    if (log2W > log2H) {
      offset = (1u << (log2W - packedMipBase)) >> (packedMip - 2u);
      xBlocks = offset;
      yBlocks = 0;
    } else {
      xBlocks = 0;
      offset = (1u << (log2H - packedMipBase)) >> (packedMip - 2u);
      yBlocks = offset;
    }
    if (offset < 4u) {
      // Pack 1x1 Z mipmaps along Z.
      const uint32_t log2D = log2Ceil(depth);
      zBlocks = log2D > 1u + packedMip ? (log2D - packedMip) * 4u : 4u;
    } else {
      zBlocks = 0;
    }
  }
  xBlocks >>= blockLog2;
  yBlocks >>= blockLog2;
  return true;
}

// texture_util.cc:479-510 GetTiledAddressUpperBound3D. pitch / height in blocks (multiples of 32).
inline uint64_t tiledUpperBound3D(uint32_t right, uint32_t bottom, uint32_t back, uint32_t pitchAligned,
                                  uint32_t heightAligned, uint32_t bpbLog2) {
  if (!right || !bottom || !back) return 0;
  uint64_t ub = uint64_t(xenos_tiling::tiled3D(int32_t((right - 1u) & ~(kTileWH - 1u)),
                                               int32_t((bottom - 1u) & ~(kTileWH - 1u)),
                                               int32_t((back - 1u) & ~(kTileDepth - 1u)), pitchAligned,
                                               heightAligned, bpbLog2));
  if (bpbLog2 == 0u)
    ub += ((pitchAligned >> 6) << 12) + 0xC00u + ((pitchAligned & (1u << 5)) << (10 - 5));
  else
    ub += uint64_t((pitchAligned << (12 - 5 - 1)) + (0x1000u >> 1)) << bpbLog2;
  return ub;
}

struct Level {
  uint32_t rowPitchBytes = 0;        // row_pitch_bytes
  uint32_t rowPitchBlocks = 0;       // row_pitch_blocks_tile_aligned (the Tiled3D pitch)
  uint32_t zSliceRows = 0;           // z_slice_stride_block_rows (the Tiled3D height)
  uint32_t sliceStrideBytes = 0;     // array_slice_stride_bytes (x align(depth, 4), 4 KB aligned)
  uint32_t zStrideBytes = 0;         // one z slice (linear addressing)
  uint32_t extentBytes = 0;          // level_data_extent_bytes (array size 1)
};

struct Layout {
  bool ok = false;
  uint32_t maxLevel = 0;             // after the dimension clamp
  uint32_t packedLevel = 0xFFFFFFFFu;
  Level base;
  Level mips[kMaxLevels];
  uint32_t mipOffsets[kMaxLevels] = {};
  uint32_t mipsTotalExtentBytes = 0;
};

// texture_util.cc:208-438 GetGuestTextureLayout, k3D, array size 1, has_base = true.
// basePitchTexels = the fetch constant's pitch field << 5. blockLog2: 0 (1x1) or 2 (4x4 DXT).
inline Layout computeLayout(uint32_t width, uint32_t height, uint32_t depth, uint32_t basePitchTexels, bool tiled,
                            uint32_t blockLog2, uint32_t bytesPerBlock, bool hasPacked, uint32_t maxLevel) {
  Layout L;
  if (!width || !height || !depth || !bytesPerBlock) return L;
  const uint32_t block = 1u << blockLog2;
  maxLevel = std::min(maxLevel, log2Floor(std::max(std::max(width, height), depth)));
  if (maxLevel >= kMaxLevels) return L;
  L.maxLevel = maxLevel;
  L.packedLevel = hasPacked ? packedMipLevel(width, height) : 0xFFFFFFFFu;
  const uint32_t maxStoredLevel = std::min(maxLevel, L.packedLevel);
  const uint32_t bpbLog2 = log2Floor(bytesPerBlock);
  const uint32_t loopLast = L.packedLevel == 0u ? uint32_t(maxLevel != 0u) : maxStoredLevel;
  uint32_t mipOffset = 0;
  for (uint32_t loop = 0; loop <= loopLast; ++loop) {
    const bool isBase = loop == 0u;
    const uint32_t level = L.packedLevel == 0u ? 0u : loop;
    Level& lv = isBase ? L.base : L.mips[level];
    uint32_t rowPitchTexels, zRowsTexels;
    if (isBase) {
      rowPitchTexels = basePitchTexels;
      zRowsTexels = height;
    } else {
      rowPitchTexels = std::max(nextPow2(width) >> level, 1u);
      zRowsTexels = std::max(nextPow2(height) >> level, 1u);
    }
    lv.rowPitchBlocks = alignUp(alignUp(rowPitchTexels, block) / block, kTileWH);
    lv.rowPitchBytes = lv.rowPitchBlocks * bytesPerBlock;
    if (!tiled && !isBase) lv.rowPitchBytes = alignUp(lv.rowPitchBytes, kLinearRowAlign);
    lv.zSliceRows = alignUp(alignUp(zRowsTexels, block) / block, kTileWH);
    lv.zStrideBytes = lv.rowPitchBytes * lv.zSliceRows;
    lv.sliceStrideBytes = alignUp(lv.zStrideBytes * alignUp(depth, kTileDepth), kSubresAlign);
    uint32_t xExt = 0, yExt = 0, zExt = 0;
    if (level == L.packedLevel) {
      const uint32_t subLast = isBase ? 0u : maxLevel;
      for (uint32_t sub = L.packedLevel; sub <= subLast; ++sub) {
        uint32_t xb, yb, zb;
        packedMipOffset(width, height, depth, blockLog2, sub, xb, yb, zb);
        xExt = std::max(xExt, xb + alignUp(std::max(width >> sub, 1u), block) / block);
        yExt = std::max(yExt, yb + alignUp(std::max(height >> sub, 1u), block) / block);
        zExt = std::max(zExt, zb + std::max(depth >> sub, 1u));
      }
    } else {
      xExt = alignUp(std::max(width >> level, 1u), block) / block;
      yExt = alignUp(std::max(height >> level, 1u), block) / block;
      zExt = std::max(depth >> level, 1u);
    }
    if (tiled) {
      const uint64_t e = tiledUpperBound3D(xExt, yExt, zExt, lv.rowPitchBlocks, lv.zSliceRows, bpbLog2);
      if (e > 0xFFFFFFFFull) return Layout{};
      lv.extentBytes = uint32_t(e);
    } else {
      lv.extentBytes = lv.zStrideBytes * (zExt - 1u) + lv.rowPitchBytes * (yExt - 1u) + bytesPerBlock * xExt;
    }
    if (!isBase) {
      L.mipOffsets[level] = mipOffset;
      L.mipsTotalExtentBytes = std::max(L.mipsTotalExtentBytes, mipOffset + lv.extentBytes);
      mipOffset += lv.sliceStrideBytes;
    }
  }
  L.ok = true;
  return L;
}

// Where level `m` of the volume lives, as Xenia's loader reads it: the region (base page or mip page),
// the byte offset of its storage level in that region, the storage level's Tiled3D pitch / height, and
// the block / z origin of `m` inside a packed tail.
struct Placement {
  bool ok = false;
  bool inBasePage = false;           // the base page (level 0, or a base packed tail) vs the mip page
  uint32_t byteOffset = 0;           // from the region's start
  uint32_t x0 = 0, y0 = 0, z0 = 0;   // block / z origin inside a packed tail
  uint32_t width = 1, height = 1, depth = 1;  // the level's texel size
  const Level* storage = nullptr;
};

inline Placement placeLevel(const Layout& L, uint32_t width, uint32_t height, uint32_t depth, uint32_t blockLog2,
                            uint32_t m) {
  Placement p;
  if (!L.ok || m > L.maxLevel) return p;
  p.width = std::max(width >> m, 1u);
  p.height = std::max(height >> m, 1u);
  p.depth = std::max(depth >> m, 1u);
  if (m == 0u) {
    p.inBasePage = true;
    p.storage = &L.base;
    if (L.packedLevel == 0u) packedMipOffset(width, height, depth, blockLog2, 0u, p.x0, p.y0, p.z0);
  } else if (m >= L.packedLevel) {
    const uint32_t s = L.packedLevel;  // the tail is stored as level packedLevel (mips[0] when it is 0)
    p.storage = &L.mips[s];
    p.byteOffset = L.mipOffsets[s];
    packedMipOffset(width, height, depth, blockLog2, m, p.x0, p.y0, p.z0);
  } else {
    p.storage = &L.mips[m];
    p.byteOffset = L.mipOffsets[m];
  }
  p.ok = p.storage->rowPitchBytes != 0u;
  return p;
}

// The byte offset (from the storage level's start) of block (x, y) of z slice z.
inline int64_t blockOffset(const Level& s, bool tiled, uint32_t bpbLog2, uint32_t x, uint32_t y, uint32_t z) {
  if (tiled)
    return xenos_tiling::tiled3D(int32_t(x), int32_t(y), int32_t(z), s.rowPitchBlocks, s.zSliceRows, bpbLog2);
  return int64_t(z) * s.zStrideBytes + int64_t(y) * s.rowPitchBytes + (int64_t(x) << bpbLog2);
}

// Step-4 lane C round 2 (SR_TEX3D_V2, P2-1): the guest ranges a volume fetch can read - the base to its EXTENT
// and the mip region to the last level texture_util.cc GetSubresourcesFromFetchConstant allows (min(mip_max_level,
// log2 of the longest axis, depth included), not below mip_min_level; kBaseMap ignored, as there: a fetch
// instruction may override it). The volume loader reads within these; capture and texture-unlock invalidation
// use them. mipField / mipMaxField = the fetch constant's dword_4 mip_min_level / mip_max_level.
struct Spans {
  bool ok = false;
  uint32_t baseBytes = 0, mipBytes = 0, maxLevel = 0;
};
inline Spans fetchSpans(uint32_t width, uint32_t height, uint32_t depth, uint32_t basePitchTexels, bool tiled,
                        uint32_t blockLog2, uint32_t bytesPerBlock, bool hasMipPage, bool packed,
                        uint32_t mipMinField, uint32_t mipMaxField) {
  Spans s;
  const uint32_t sizeMax = log2Floor(std::max(std::max(width, height), depth));
  const uint32_t mipMin = std::min(mipMinField, sizeMax);
  const uint32_t maxLevel = hasMipPage ? std::max(std::min(mipMaxField, sizeMax), mipMin) : 0u;
  const Layout L = computeLayout(width, height, depth, basePitchTexels, tiled, blockLog2, bytesPerBlock, packed,
                                 maxLevel);
  if (!L.ok) return s;
  s.ok = true;
  s.maxLevel = L.maxLevel;
  s.baseBytes = L.base.extentBytes;
  s.mipBytes = (L.maxLevel && hasMipPage) ? L.mipsTotalExtentBytes : 0u;
  return s;
}

// Step-4 lane C round 2 (SR_TEX3D_V2, P2-2): a guest region with the fetch constant's endian swap applied to its
// GUEST 32-bit words (Xenia's texture load shaders swap the words they read, then pick texels), the last partial
// word included (zero-padded). out.size() = span rounded up to 4. endian: 1 8in16, 2 8in32, 3 16in32.
inline void swapGuestWords(const uint8_t* src, uint32_t span, uint32_t endian, std::vector<uint8_t>& out) {
  out.assign((size_t(span) + 3u) & ~size_t(3), 0u);
  if (span) std::memcpy(out.data(), src, span);
  for (size_t i = 0; i < out.size(); i += 4) {
    uint32_t v;
    std::memcpy(&v, out.data() + i, 4);
    switch (endian & 3u) {
      case 1: v = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8); break;
      case 2: v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); break;
      case 3: v = (v >> 16) | (v << 16); break;
      default: break;
    }
    std::memcpy(out.data() + i, &v, 4);
  }
}

// Step-4 lane C round 3 (SR_TEX3D_V3, Astra round-2 P2 on volume_layout.hpp swapGuestWords): the conversion depends
// on the format's block size, as Xenia's texture load shaders do it (third_party/xenia-shaders endian.xesli and
// texture_load_*bpb.xesli): an 8-bit block loader (texture_load_8bpb) never swaps; a 16-bit block loader
// (texture_load_16bpb) applies XeEndianSwap16 - only 8in16 swaps (the bytes of each 16-bit unit), 8in32 and 16in32
// leave the data as it is; 32 / 64 / 128-bit block loaders apply XeEndianSwap32 to every 32-bit guest word
// (8in16, 8in32 = full byte reversal, 16in32 = halves). swapGuestWords above applies the 32-bit rule to every format
// (an R8 '7F' with 8in16 became 00, R16 '12 34' with 8in32 / 16in32 became 00 00). out.size() = span rounded up to
// 4, the tail zero-padded. blockBytes = guest bytes per block (1, 2, 4, 8, 16).
inline void swapGuestUnits(const uint8_t* src, uint32_t span, uint32_t endian, uint32_t blockBytes,
                           std::vector<uint8_t>& out) {
  endian &= 3u;
  if (blockBytes >= 4u) {
    swapGuestWords(src, span, endian, out);
    return;
  }
  out.assign((size_t(span) + 3u) & ~size_t(3), 0u);
  if (span) std::memcpy(out.data(), src, span);
  if (blockBytes != 2u || endian != 1u) return;  // 8-bit blocks: no swap; 16-bit blocks: only 8in16
  for (size_t i = 0; i < out.size(); i += 2) std::swap(out[i], out[i + 1]);
}

}  // namespace volume
}  // namespace gfx
}  // namespace sr
