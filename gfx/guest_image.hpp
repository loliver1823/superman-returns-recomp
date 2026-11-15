#pragma once
// guest_image.hpp — decode one 2D guest image into linear host rows.
//
// Extracted from D3D12Backend::hleUploadHostTextureFmt (iter-457 H1(b) step 2a)
// so the cube uploader and the 2D uploader share ONE body, and so a test can
// drive it without a D3D12 device. The pre-extract inlined copy is frozen in
// kernel/tex_unlock_test.cpp and compared byte-for-byte against this function;
// a drift here is a picture that looks like an art bug.
#include "xenos_tiling.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace sr {
namespace gfx {

inline uint32_t guestImageSwap32(uint32_t v, uint32_t endian) {
  switch (endian & 3u) {
    case 1: return ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
    case 2: return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
                   ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
    case 3: return (v >> 16) | (v << 16);
    default: return v;
  }
}

struct DecodedImage {
  std::vector<uint8_t> lin;
  uint32_t rows = 0;
  uint32_t rowBytes = 0;
  bool truncated = false;
  uint64_t needed = 0;
};

inline bool decodeGuestImage(const uint8_t* src, uint64_t srcBytes,
                             uint32_t blockW, uint32_t blockBytes,
                             uint32_t width, uint32_t height,
                             uint32_t pitchTexels, bool tiled, uint32_t endian,
                             DecodedImage& out) {
  const uint32_t rowBlocks = (width + blockW - 1u) / blockW;
  const uint32_t rows = (height + blockW - 1u) / blockW;
  const uint32_t rowBytes = rowBlocks * blockBytes;
  if (!rowBytes || !rows) return false;
  uint32_t blockPitch = pitchTexels / blockW;
  if (tiled) blockPitch = (blockPitch + 31u) & ~31u;
  if (!blockPitch) blockPitch = rowBlocks;

  out.rows = rows;
  out.rowBytes = rowBytes;
  out.needed = uint64_t(rows) * blockPitch * blockBytes;
  out.truncated = false;
  out.lin.assign(size_t(rows) * rowBytes, 0);

  if (tiled) {
    uint32_t bpbLog2 = 0;
    while ((1u << bpbLog2) < blockBytes) bpbLog2++;
    for (uint32_t by = 0; by < rows; by++) {
      uint8_t* drow = out.lin.data() + size_t(by) * rowBytes;
      for (uint32_t bx = 0; bx < rowBlocks; bx++) {
        const int32_t off = xenos_tiling::tiled2D(int32_t(bx), int32_t(by),
                                                  blockPitch, bpbLog2);
        if (off < 0 || uint64_t(off) + blockBytes > srcBytes) {
          out.truncated = true;
          continue;
        }
        std::memcpy(drow + size_t(bx) * blockBytes, src + off, blockBytes);
      }
    }
  } else {
    const uint32_t srcPitch = blockPitch * blockBytes;
    for (uint32_t y = 0; y < rows; y++) {
      const uint64_t off = uint64_t(y) * srcPitch;
      if (off + rowBytes > srcBytes) {
        out.truncated = true;
        break;
      }
      std::memcpy(out.lin.data() + size_t(y) * rowBytes, src + off, rowBytes);
    }
  }
  if (endian) {
    uint32_t* p = reinterpret_cast<uint32_t*>(out.lin.data());
    for (size_t i = 0; i < out.lin.size() / 4; i++)
      p[i] = guestImageSwap32(p[i], endian);
  }
  return true;
}

}  // namespace gfx
}  // namespace sr
