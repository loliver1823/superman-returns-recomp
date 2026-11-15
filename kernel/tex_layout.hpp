// iter-456n slice 5 (Class A): THE ONE texture layout / footprint helper.
//
// Director's bar (ledger §"DESIGN REVIEW VERDICT" R2, re-affirmed in the
// slice-3 stop verdict): "Extract one shared checked layout/footprint helper
// used by bind capture and Unlock. It must calculate the maximum byte actually
// touched by the tiled decode, not merely duplicate a padded rows*pitch
// estimate that may include a neighbour tail."
//
// TWO numbers, deliberately separate, because they answer different questions:
//
//   spanPadded  — the storage span the bind-capture site has always used:
//                 alignUp(rows,32) * blockPitch * blockBytes when tiled.
//                 REPRODUCED BIT-FOR-BIT here so the capture site can adopt
//                 this helper without changing one byte of default behaviour
//                 (the gate-OFF byte-identity bar). It is an UPPER BOUND and
//                 it can include a neighbour's bytes.
//   maxTouched  — the EXACT highest byte offset the decoder's own addressing
//                 reaches, +1. For the tiled case this is computed by walking
//                 the decoder's own (bx,by) grid through the SAME tiled2D
//                 transform the backend uses, and taking the maximum — not by
//                 a closed-form guess, because the Xenos swizzle interleaves
//                 bank/pipe bits that no simple corner argument bounds. The
//                 walk is memoised per (rowBlocks, rows, blockPitch,
//                 blockBytes), so a run pays it once per distinct layout.
//
// A snapshot sized at maxTouched is sufficient for the decode BY CONSTRUCTION
// (it covers every byte the decoder reads) and never claims a neighbour's tail
// beyond what the decoder actually addresses.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>

#include "../gfx/xenos_tiling.hpp"

namespace sr::kernel::texlayout {

// Block geometry per Xenos texture format. ⚠ This MUST agree with BOTH
// kernel/hle_api_observe.cpp's texBlockGeom AND the backend's decode switch in
// gfx/d3d12_backend.cpp (hleUploadHostTextureFmt). The self-test asserts the
// agreement rather than trusting the comment.
struct BlockGeom {
  uint32_t blockW = 1;       // texels per block edge (1 = uncompressed)
  uint32_t blockBytes = 4;   // bytes per block
  bool supported = false;    // false = the backend refuses it -> WHITE
};

inline BlockGeom blockGeomFor(uint32_t fmtCode) {
  switch (fmtCode) {
    case 2:            return {1, 1, true};    // k_8
    case 6: case 50:   return {1, 4, true};    // k_8_8_8_8
    case 18:           return {4, 8, true};    // k_DXT1  (BC1)
    case 19: case 20: case 49: return {4, 16, true};  // DXT2_3 / DXT4_5 / DXN
    case 24:           return {1, 2, true};    // k_16
    case 29:           return {1, 8, true};    // k_16_16_16_16_EXPAND (4x16F)
    case 31:           return {1, 4, true};    // k_16_16_FLOAT
    case 36:           return {1, 4, true};    // k_32_FLOAT
    // 22/23 are depth-as-texture: the CAPTURE site sizes them, but the
    // backend's decode switch has no arm, so Class A must NOT admit them.
    case 22: case 23:  return {1, 4, false};
    default:           return {1, 4, false};
  }
}

enum class LayoutRefuse : uint8_t {
  None = 0, UnsupportedFormat, BadDims, NonZeroMip, Overflow,
  // iter-456n rework (5th adjudication): the exact max-touched walk is O(blocks)
  // and runs on the GUEST thread. A cheap upper bound is checked BEFORE it, so a
  // layout that could never be snapshotted anyway is refused without walking a
  // grid up to 4096x4096.
  OverBudget,
};

inline const char* layoutRefuseName(LayoutRefuse r) {
  switch (r) {
    case LayoutRefuse::None:              return "none";
    case LayoutRefuse::UnsupportedFormat: return "unsupported-format";
    case LayoutRefuse::BadDims:           return "bad-dims";
    case LayoutRefuse::NonZeroMip:        return "nonzero-mip";
    case LayoutRefuse::Overflow:          return "overflow";
    case LayoutRefuse::OverBudget:        return "over-budget";
  }
  return "?";
}

struct Layout {
  bool valid = false;
  LayoutRefuse refuse = LayoutRefuse::None;
  uint32_t blockW = 0, blockBytes = 0;
  uint32_t rowBlocks = 0, rows = 0, blockPitch = 0;
  uint64_t spanPadded = 0;    // the capture site's historical span (upper bound)
  uint64_t maxTouched = 0;    // EXACT bytes the decode addresses
};

namespace detail {

// Exact maximum over the decoder's own grid, memoised per layout tuple.
// Mirrors gfx/d3d12_backend.cpp hleUploadHostTextureFmt's tiled loop exactly:
//   for by in [0,rows) for bx in [0,rowBlocks):
//       off = tiled2D(bx, by, blockPitch, log2(blockBytes)); read blockBytes
inline uint64_t tiledMaxTouched(uint32_t rowBlocks, uint32_t rows,
                                uint32_t blockPitch, uint32_t blockBytes) {
  using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
  static std::mutex mu;
  static std::map<Key, uint64_t> memo;
  const Key k{rowBlocks, rows, blockPitch, blockBytes};
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = memo.find(k);
    if (it != memo.end()) return it->second;
  }
  uint32_t bpbLog2 = 0;
  while ((1u << bpbLog2) < blockBytes) bpbLog2++;
  int64_t hi = -1;
  for (uint32_t by = 0; by < rows; by++) {
    for (uint32_t bx = 0; bx < rowBlocks; bx++) {
      const int32_t off = sr::gfx::xenos_tiling::tiled2D(
          int32_t(bx), int32_t(by), blockPitch, bpbLog2);
      if (off > hi) hi = off;
    }
  }
  const uint64_t v = (hi < 0) ? 0ull : (uint64_t)hi + blockBytes;
  {
    std::lock_guard<std::mutex> lk(mu);
    memo.emplace(k, v);
  }
  return v;
}

}  // namespace detail

// `level` is the mip level the caller intends to read. Class A refuses nonzero
// levels rather than guessing their dimensions (the iter-455n blocker-4 rule).
// `budgetBytes` (0 = unbounded) is the caller's snapshot capacity. The PADDED
// span is a proven upper bound of maxTouched, so `spanPadded > budget` refuses
// BEFORE the exact walk — the walk only ever runs for layouts that could still
// be accepted. This is the fifth adjudication's required pre-walk guard.
inline Layout computeLayout(uint32_t fmtCode, uint32_t width, uint32_t height,
                            uint32_t pitchTexels, bool tiled, uint32_t level,
                            uint64_t budgetBytes = 0) {
  Layout L{};
  const BlockGeom g = blockGeomFor(fmtCode);
  L.blockW = g.blockW;
  L.blockBytes = g.blockBytes;
  if (!g.supported) { L.refuse = LayoutRefuse::UnsupportedFormat; return L; }
  if (level != 0)   { L.refuse = LayoutRefuse::NonZeroMip;        return L; }
  if (!width || !height || width > 4096u || height > 4096u) {
    L.refuse = LayoutRefuse::BadDims; return L;
  }

  // ⚠ Reproduces kernel/hle_api_observe.cpp's capture math EXACTLY, including
  // its use of blockW as the VERTICAL divisor (Xenos blocks are square) and
  // its `if (!blockPitch) blockPitch = rowBlocks` fallback.
  L.rowBlocks  = (width  + g.blockW - 1u) / g.blockW;
  L.rows       = (height + g.blockW - 1u) / g.blockW;
  L.blockPitch = pitchTexels / g.blockW;
  if (tiled) L.blockPitch = (L.blockPitch + 31u) & ~31u;
  if (!L.blockPitch) L.blockPitch = L.rowBlocks;
  if (!L.rowBlocks || !L.rows) { L.refuse = LayoutRefuse::BadDims; return L; }

  const uint32_t spanRows = tiled ? ((L.rows + 31u) & ~31u) : L.rows;
  L.spanPadded = (uint64_t)spanRows * L.blockPitch * L.blockBytes;

  // ⛔ PRE-WALK BUDGET GUARD — cheap, and it precedes the O(blocks) walk.
  if (budgetBytes && L.spanPadded > budgetBytes) {
    L.refuse = LayoutRefuse::OverBudget; return L;
  }
  if (tiled) {
    L.maxTouched = detail::tiledMaxTouched(L.rowBlocks, L.rows, L.blockPitch,
                                           L.blockBytes);
  } else {
    // Linear: the decoder reads rowBytes at y*blockPitch*blockBytes.
    const uint64_t srcPitch = (uint64_t)L.blockPitch * L.blockBytes;
    const uint64_t rowBytes = (uint64_t)L.rowBlocks * L.blockBytes;
    L.maxTouched = (uint64_t)(L.rows - 1u) * srcPitch + rowBytes;
  }

  if (L.spanPadded == 0 || L.spanPadded > 0xFFFFFFFFull ||
      L.maxTouched == 0 || L.maxTouched > 0xFFFFFFFFull) {
    L.refuse = LayoutRefuse::Overflow; return L;
  }
  L.valid = true;
  return L;
}

// ── The texture header's own 24-byte fetch constant (ENGINE_MAP §6g.2) ──────
// hdr[7..12] (byte offset +28) is the fetch constant the GPU samples through,
// and it is the AUTHORITATIVE source of pitch/tiling/format/dims.
//
// ⛔ WHY THIS EXISTS (director's R2 bar): "Do not confuse Lock's byte pitch
// with backend pitch texels." The Lock out-struct reports a BYTE pitch; the
// backend's decode wants pitch in TEXELS. They coincide only for k_8, which is
// exactly the class the legacy path was scoped to — so a widened Unlock that
// reused the Lock pitch would mis-size every other format.
// ── iter-456p: THE DATA DIMENSION ────────────────────────────────────────────
// Values are Xenia's `DataDimension` (xenos.h): the field lives in dword_5 bits
// 9-10, NOT in dword_0. Measured on the golden title frame, 9,253 of 38,380
// texture fetches (7,431 3D + 1,822 cube) are NOT 2D, and the old decoder read
// every one of them through the 2D size layout - producing extents such as
// "6208x7696 pitch=64".
enum class TexDim : uint32_t { k1D = 0, k2DOrStacked = 1, k3D = 2, kCube = 3 };

inline const char* texDimName(TexDim d) {
  switch (d) {
    case TexDim::k1D:          return "1D";
    case TexDim::k2DOrStacked: return "2D";
    case TexDim::k3D:          return "3D";
    case TexDim::kCube:        return "CUBE";
    default:                   return "?";
  }
}

struct FetchDesc {
  bool valid = false;
  uint32_t fmt = 0, width = 0, height = 0, pitchTexels = 0, endian = 0;
  bool tiled = false;
  uint32_t basePhys = 0;      // dword1 base page << 12 (the bind key's source)
  // iter-456p additions. `depth` is the array/stack/volume extent: 1 for plain
  // 2D and 1D, stack_depth for a STACKED 2D, the volume depth for 3D, and
  // exactly 6 for a cube.
  TexDim   dim = TexDim::k2DOrStacked;
  bool     stacked = false;
  uint32_t depth = 1;
  uint32_t mipMin = 0, mipMax = 0;

  // The ONLY shape the widened-Unlock publish path may touch. Correcting the
  // metadata is not authorization to publish 1D, stacked, 3D, cube or mip data
  // (director ruling, Slice A.2).
  bool isPlain2D() const { return dim == TexDim::k2DOrStacked && !stacked; }
};

// `f` = the six fetch dwords, ALREADY host-endian (callers read them BE).
// Field positions are Xenia's `xe_gpu_texture_fetch_t` (xenos.h), read from the
// reference rather than re-derived:
//   f[0] bits 0-1 type (2 = kTexture), bits 22-30 pitch in 32-texel units,
//        bit 31 tiled
//   f[1] bits 0-5 format, bits 6-7 endian, bit 10 STACKED, bits 12+ base page
//   f[2] the size union, SELECTED BY DIMENSION - every extent stored minus one:
//        1D          width 24
//        2D/stacked  width 13 | height 13 | stack_depth 6
//        3D          width 11 | height 11 | depth 10
//        cube        as 2D; depth is always the 6 faces
//   f[4] bits 2-5 mip_min_level, bits 6-9 mip_max_level
//   f[5] bits 9-10 DIMENSION
//
// ⚠ 2D RESULTS ARE BYTE-FOR-BYTE UNCHANGED. The 13+13 width/height decode for
// dimension == k2DOrStacked is exactly the previous one; only the non-2D cases,
// which were previously garbage, differ.
inline FetchDesc decodeFetchConstant(const uint32_t f[6]) {
  FetchDesc d{};
  if ((f[0] & 3u) != 2u) return d;                 // not a texture fetch
  d.pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  d.tiled       = ((f[0] >> 31) & 1u) != 0;
  d.fmt         = f[1] & 0x3Fu;
  d.endian      = (f[1] >> 6) & 3u;
  d.stacked     = ((f[1] >> 10) & 1u) != 0;
  d.basePhys    = (f[1] >> 12) << 12;
  d.dim         = (TexDim)((f[5] >> 9) & 3u);
  d.mipMin      = (f[4] >> 2) & 0xFu;
  d.mipMax      = (f[4] >> 6) & 0xFu;

  switch (d.dim) {
    case TexDim::k1D:
      d.width  = (f[2] & 0xFFFFFFu) + 1u;
      d.height = 1u;
      d.depth  = 1u;
      break;
    case TexDim::k2DOrStacked:
      d.width  = (f[2] & 0x1FFFu) + 1u;
      d.height = ((f[2] >> 13) & 0x1FFFu) + 1u;
      // stack_depth is "not very meaningful" for a non-stacked 2D (Xenia's own
      // comment), so it is read ONLY when the stacked bit says it means
      // something.
      d.depth  = d.stacked ? (((f[2] >> 26) & 0x3Fu) + 1u) : 1u;
      break;
    case TexDim::k3D:
      d.width  = (f[2] & 0x7FFu) + 1u;
      d.height = ((f[2] >> 11) & 0x7FFu) + 1u;
      d.depth  = ((f[2] >> 22) & 0x3FFu) + 1u;
      break;
    case TexDim::kCube:
      d.width  = (f[2] & 0x1FFFu) + 1u;
      d.height = ((f[2] >> 13) & 0x1FFFu) + 1u;
      d.depth  = 6u;                                // always the six faces
      break;
  }
  if (!d.width || !d.height || !d.depth) return d;
  d.valid = true;
  return d;
}

}  // namespace sr::kernel::texlayout
