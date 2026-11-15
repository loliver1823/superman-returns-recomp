#pragma once
// ============================================================================
// xenos_conformance - Xenia REFERENCE ports: tiled texture addressing.
//
// The oracle for the Xenos 2D (32x32 macro tile) and 3D (32x16x4) tiled
// address math used by texture untiling and resolve destinations. Checked
// against our gfx/xenos_tiling.hpp by conformance_main.cpp. Never edit a
// function here to match our code.
//
// Ported from xenia-canary, snapshot reference/xenia-canary @ 0b4e6c44
// (2026-06-24):
//   src/xenia/gpu/texture_address.h        Copyright 2026 Ben Vanik. All rights reserved.
//     kMacroTile* (texture_address.h:164-171), TiledCombine (:173-184),
//     Tiled2D (:190-208), Tiled3D (:213-238); the asserts are dropped (the
//     suite only calls with 32-aligned pitch / height).
//
// Licence: BSD-3-Clause, reference/xenia-canary/LICENSE
//   "Copyright (c) 2015, Ben Vanik. All rights reserved." - the full licence
//   text (conditions + disclaimer) is reproduced in xenia_ref_common.hpp,
//   which this header includes; it applies to every function below.
// ============================================================================

#include <cstdint>

#include "xenia_ref_common.hpp"

namespace xref {
namespace texture_address {

constexpr unsigned int kMacroTileWidthLog2 = 5;
constexpr unsigned int kMacroTileHeight2DLog2 = 5;
constexpr unsigned int kMacroTileHeight3DLog2 = 4;
constexpr unsigned int kMacroTileDepthLog2 = 2;

// texture_address.h:173-184
template <typename Address>
Address TiledCombine(const Address outer_inner_bytes, const uint32_t bank,
                     const uint32_t pipe, const uint32_t y_lsb) {
  return int32_t((y_lsb << 4) | (pipe << 6) | (bank << 11)) |
         (outer_inner_bytes & 0b1111) |
         (((outer_inner_bytes >> 4) & 0b1) << 5) |
         (((outer_inner_bytes >> 5) & 0b111) << 8) |
         (outer_inner_bytes >> 8 << 12);
}

// texture_address.h:190-208
inline int32_t Tiled2D(const int32_t x, const int32_t y,
                       const uint32_t pitch_aligned,
                       const unsigned int bytes_per_block_log2) {
  const int32_t outer_blocks =
      ((y >> kMacroTileHeight2DLog2) *
           int32_t(pitch_aligned >> kMacroTileWidthLog2) +
       (x >> kMacroTileWidthLog2))
      << 6;
  const int32_t inner_blocks = (((y >> 1) & 0b111) << 3) | (x & 0b111);
  const int32_t outer_inner_bytes = (outer_blocks | inner_blocks)
                                    << bytes_per_block_log2;
  const uint32_t bank = (y >> 4) & 0b1;
  const uint32_t pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1);
  return TiledCombine(outer_inner_bytes, bank, pipe, y & 1);
}

// texture_address.h:213-238
inline int64_t Tiled3D(const int32_t x, const int32_t y, const int32_t z,
                       const uint32_t pitch_aligned,
                       const uint32_t height_aligned,
                       const unsigned int bytes_per_block_log2) {
  const int32_t outer_blocks =
      ((((z >> kMacroTileDepthLog2) *
             (height_aligned >> kMacroTileHeight3DLog2) +
         (y >> kMacroTileHeight3DLog2)) *
        int32_t(pitch_aligned >> kMacroTileWidthLog2)) +
       (x >> kMacroTileWidthLog2))
      << 7;
  const int32_t inner_blocks =
      ((z & 0b11) << 5) | (((y >> 1) & 0b11) << 3) | (x & 0b111);
  const int64_t outer_inner_bytes = int64_t(outer_blocks | inner_blocks)
                                    << bytes_per_block_log2;
  const uint32_t bank = ((y >> 3) ^ (z >> 2)) & 0b1;
  const uint32_t pipe = ((x >> 3) & 0b11) ^ (bank << 1);
  return TiledCombine(outer_inner_bytes, bank, pipe, y & 1);
}

}  // namespace texture_address
}  // namespace xref
