#pragma once

// Xenos 2D texture tiling — address math ported from Xenia
// (reference/xenia-canary/src/xenia/gpu/texture_address.h, BSD license,
// Copyright 2026 Ben Vanik. All rights reserved.) See that header for the full
// derivation (AMD AddrLib macro/micro tile structure; 2D tiling originally from
// BinomialLLC/crunch crn_decomp.h).
//
// The 360 system front buffer (and most render-target textures) is TILED:
// 32x32-texel macro tiles, 8x16 micro tiles, bank/pipe interleave. A linear
// CPU read of that memory is scrambled; presenting it requires this address
// transform. Xenia does the equivalent in its texture-load shaders.

#include <cstdint>

namespace sr {
namespace gfx {
namespace xenos_tiling {

constexpr unsigned int kMacroTileWidthLog2 = 5;
constexpr unsigned int kMacroTileHeight2DLog2 = 5;

inline int32_t tiledCombine(int32_t outerInnerBytes, uint32_t bank,
                            uint32_t pipe, uint32_t yLsb) {
  return int32_t((yLsb << 4) | (pipe << 6) | (bank << 11)) |
         (outerInnerBytes & 0b1111) |
         (((outerInnerBytes >> 4) & 0b1) << 5) |
         (((outerInnerBytes >> 5) & 0b111) << 8) |
         (outerInnerBytes >> 8 << 12);
}

// Byte offset of texel (x, y) in a 2D-tiled surface. pitchAligned is the
// storage pitch in texels and MUST be a multiple of 32 (the fetch-constant
// pitch field is texels>>5, so pitch<<5 always satisfies this).
inline int32_t tiled2D(int32_t x, int32_t y, uint32_t pitchAligned,
                       unsigned int bytesPerBlockLog2) {
  const int32_t outerBlocks =
      ((y >> kMacroTileHeight2DLog2) *
           int32_t(pitchAligned >> kMacroTileWidthLog2) +
       (x >> kMacroTileWidthLog2))
      << 6;
  const int32_t innerBlocks = (((y >> 1) & 0b111) << 3) | (x & 0b111);
  const int32_t outerInnerBytes = (outerBlocks | innerBlocks)
                                  << bytesPerBlockLog2;
  const uint32_t bank = (y >> 4) & 0b1;
  const uint32_t pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1);
  return tiledCombine(outerInnerBytes, bank, pipe, y & 1);
}

// ── 3D (volume) tiling — ported from the same Xenia header's Tiled3D ──
// Macro tiles are 32x16x4 for volumes; bank/pipe mix in z. Census (iter-449w):
// every live guest volume is 64x64x64 k_8_8_8_8, so offsets stay well inside
// int32, but the math is kept 64-bit-faithful to the source.
constexpr unsigned int kMacroTileHeight3DLog2 = 4;
constexpr unsigned int kMacroTileDepthLog2 = 2;

inline int64_t tiledCombine64(int64_t outerInnerBytes, uint32_t bank,
                              uint32_t pipe, uint32_t yLsb) {
  return int64_t(int32_t((yLsb << 4) | (pipe << 6) | (bank << 11))) |
         (outerInnerBytes & 0b1111) |
         (((outerInnerBytes >> 4) & 0b1) << 5) |
         (((outerInnerBytes >> 5) & 0b111) << 8) |
         (outerInnerBytes >> 8 << 12);
}

// Byte offset of texel (x, y, z) in a 3D-tiled volume. pitchAligned and
// heightAligned in texels, multiples of 32.
inline int64_t tiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitchAligned,
                       uint32_t heightAligned, unsigned int bytesPerBlockLog2) {
  const int32_t outerBlocks =
      ((((z >> kMacroTileDepthLog2) *
             int32_t(heightAligned >> kMacroTileHeight3DLog2) +
         (y >> kMacroTileHeight3DLog2)) *
        int32_t(pitchAligned >> kMacroTileWidthLog2)) +
       (x >> kMacroTileWidthLog2))
      << 7;
  const int32_t innerBlocks =
      ((z & 0b11) << 5) | (((y >> 1) & 0b11) << 3) | (x & 0b111);
  const int64_t outerInnerBytes = int64_t(outerBlocks | innerBlocks)
                                  << bytesPerBlockLog2;
  const uint32_t bank = ((y >> 3) ^ (z >> 2)) & 0b1;
  const uint32_t pipe = ((x >> 3) & 0b11) ^ (bank << 1);
  return tiledCombine64(outerInnerBytes, bank, pipe, y & 1);
}

// Storage span in bytes of a 32bpp 2D-tiled surface (full macro-tile rows).
inline size_t tiledSpanBytes32(uint32_t pitchTexels, uint32_t height) {
  const uint32_t hAligned = (height + 31u) & ~31u;
  return static_cast<size_t>(pitchTexels) * hAligned * 4u;
}

// Untile a 32bpp 2D surface into tightly-packed rows (dstPitch = width*4).
// src must cover tiledSpanBytes32(pitchTexels, height) — caller validates.
inline void untile32(uint8_t* dst, const uint8_t* src, uint32_t pitchTexels,
                     uint32_t width, uint32_t height) {
  const uint32_t w = (width <= pitchTexels) ? width : pitchTexels;
  for (uint32_t y = 0; y < height; y++) {
    uint8_t* drow = dst + static_cast<size_t>(y) * width * 4u;
    for (uint32_t x = 0; x < w; x++) {
      const int32_t off = tiled2D(static_cast<int32_t>(x),
                                  static_cast<int32_t>(y), pitchTexels, 2);
      const uint8_t* s = src + off;
      drow[x * 4 + 0] = s[0];
      drow[x * 4 + 1] = s[1];
      drow[x * 4 + 2] = s[2];
      drow[x * 4 + 3] = s[3];
    }
  }
}

}  // namespace xenos_tiling
}  // namespace gfx
}  // namespace sr
