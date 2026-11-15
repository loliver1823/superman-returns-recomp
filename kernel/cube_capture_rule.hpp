#pragma once
// iter-457 H1(a)-r2: THE cube capture body — one implementation, driven by
// production AND by the self-test.
//
// WHY THIS FILE EXISTS. The H1(a) cube capture was first written inline inside
// `sub_82100310` (the guest SetTexture override). The director's verdict on
// f7421e9 found the consequence: nothing could drive it, so the mutant "the
// producer does not verify stack_depth == 5" SURVIVED with the suite still
// green. A refusal nobody can witness is a comment, not a rule.
//
// This follows `tex_unlock_rule.hpp` exactly, and for the same reason its own
// header records: a "shared body" that the test does not actually call is
// shared in name only. The three side-effecting steps — reading guest bytes,
// snapshotting into the arena, publishing the command — are injected, so the
// fixture drives THIS body and production drives THIS body.
//
// ⛔ SIX FACES IS VERIFIED, NEVER ASSUMED. Xenos requires
// `size_2d.stack_depth == 5` for a cube and Xenia asserts it
// (reference/xenia-canary/src/xenia/gpu/texture_util.cc:52). The older census
// printed "d=6 by construction"; that is the bug, not the fix.
#include "../gfx/cube_layout.hpp"
#include "../gfx/hle_video.hpp"   // the fmtBits allocation this packs into

#include <cstdint>

namespace sr::kernel::cubecap {

enum class CubeCapRefuse : uint32_t {
  None = 0,
  NotCube,        // fetch dimension is not kCube
  NoBasePage,     // base_address is zero
  StackDepth,     // size_2d.stack_depth != 5 — the Xenos invariant
  MipLevels,      // mip level count outside 1..8
  Format,         // the backend has no decode for this format
  Layout,         // computeCubeLayout refused (its own reason is carried)
  OverBudget,     // the two regions do not fit one texture slot
  Duplicate,      // already captured this identity
  BaseUnreadable, // the base region could not be translated
  MipUnreadable,  // the mip region could not be translated
  ArenaFull,      // no texture slot available
};

inline const char* cubeCapRefuseName(CubeCapRefuse r) {
  switch (r) {
    case CubeCapRefuse::None:           return "none";
    case CubeCapRefuse::NotCube:        return "notCube";
    case CubeCapRefuse::NoBasePage:     return "noBasePage";
    case CubeCapRefuse::StackDepth:     return "stackDepth";
    case CubeCapRefuse::MipLevels:      return "mipLevels";
    case CubeCapRefuse::Format:         return "format";
    case CubeCapRefuse::Layout:         return "layout";
    case CubeCapRefuse::OverBudget:     return "overBudget";
    case CubeCapRefuse::Duplicate:      return "duplicate";
    case CubeCapRefuse::BaseUnreadable: return "baseUnreadable";
    case CubeCapRefuse::MipUnreadable:  return "mipUnreadable";
    case CubeCapRefuse::ArenaFull:      return "arenaFull";
    default:                            return "unknown";
  }
}

struct CubeCapResult {
  bool published = false;
  CubeCapRefuse refuse = CubeCapRefuse::NotCube;
  // Everything the published command carries, so a fixture can assert the
  // TWO-REGION shape rather than just "it published".
  uint32_t key = 0;            // normalized base address
  uint32_t mipAddr = 0;
  uint32_t baseBytes = 0;      // region A: six faces of the base level
  uint32_t mipBytes = 0;       // region B: the mip chain
  uint32_t totalBytes = 0;     // what the arena actually holds
  uint32_t arenaOffset = 0;
  uint32_t dims = 0;
  uint32_t fmtBits = 0;
  uint32_t pitch = 0;
  uint32_t mipLevels = 1;
  bool     packedMips = false;
  uint32_t faceStride = 0;
};

// `f`      the six fetch dwords, already host-endian.
// `maxBytes`/`arenaFull` the texture-slot budget and its failure sentinel.
// `dedupe(key64)` -> true when this identity is NEW (false = already captured).
// `rd(addr, bytes)` -> const uint8_t*, or nullptr when the range is not a
//          readable, contiguous guest window. Called ONCE PER REGION, because
//          Xenia treats base_address and mip_address as independent
//          (texture_util.cc:76-99 lets base_page go to 0 while mip_page stays);
//          adjacency is an observation about our four keys, not a contract.
// `snap(ptr, bytes)` -> arena offset, or `arenaFull`.
// `pub(res)` publish the TextureUpload command.
template <typename Dedupe, typename Reader, typename Snapshotter,
          typename Publisher>
CubeCapResult runCubeCapture(const uint32_t f[6], uint32_t maxBytes,
                             uint32_t arenaFull, Dedupe dedupe, Reader rd,
                             Snapshotter snap, Publisher pub,
                             uint8_t* scratch, uint32_t scratchBytes) {
  CubeCapResult r{};
  if (((f[5] >> 9) & 3u) != 3u) { r.refuse = CubeCapRefuse::NotCube; return r; }

  const uint32_t basePage = (f[1] >> 12) & 0xFFFFFu;
  if (!basePage) { r.refuse = CubeCapRefuse::NoBasePage; return r; }

  // ⛔ THE XENOS INVARIANT, checked before anything is sized or read.
  const uint32_t stackDepth = (f[2] >> 26) & 0x3Fu;
  if (stackDepth != 5u) { r.refuse = CubeCapRefuse::StackDepth; return r; }

  const uint32_t fmt = f[1] & 0x3Fu;
  const uint32_t width = (f[2] & 0x1FFFu) + 1u;
  const uint32_t height = ((f[2] >> 13) & 0x1FFFu) + 1u;
  const uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  const bool tiled = ((f[0] >> 31) & 1u) != 0;
  const uint32_t endian = (f[1] >> 6) & 3u;
  const uint32_t mipPage = (f[5] >> 12) & 0xFFFFFu;
  const uint32_t mipMax = (f[4] >> 6) & 0xFu;
  const bool packedMips = ((f[5] >> 11) & 1u) != 0;
  // texture_util.cc:82-90 — a zero mip_address means NO mips, whatever the
  // level field says.
  const uint32_t mipLevels = (mipPage && mipMax) ? (mipMax + 1u) : 1u;
  if (!mipLevels || mipLevels > 8u) {
    r.refuse = CubeCapRefuse::MipLevels; return r;
  }

  uint32_t bw = 1, bh = 1, bpb = 4;
  if (!sr::gfx::cube::cubeBlockGeom(fmt, bw, bh, bpb)) {
    r.refuse = CubeCapRefuse::Format; return r;
  }
  const auto L = sr::gfx::cube::computeCubeLayout(fmt, width, height,
                                                  pitchTexels, tiled, mipLevels,
                                                  packedMips, bw, bh, bpb);
  if (!L.ok) { r.refuse = CubeCapRefuse::Layout; return r; }
  if (L.totalBytes > maxBytes || L.totalBytes > scratchBytes) {
    r.refuse = CubeCapRefuse::OverBudget; return r;
  }

  const uint32_t base = basePage << 12;
  const uint32_t mipAddr = mipPage << 12;
  r.key = base;
  r.mipAddr = mipAddr;
  r.baseBytes = L.baseSpanBytes;
  r.mipBytes = L.mipsTotalExtentBytes;
  r.totalBytes = L.totalBytes;
  r.faceStride = L.faceStrideBytes;
  r.mipLevels = mipLevels;
  r.packedMips = packedMips;
  r.dims = (width << 16) | (height & 0xFFFFu);
  r.pitch = pitchTexels;
  // The cube marker is mixed into the identity so it can never collide with a
  // 2D key at the same base — the same reasoning as the volume path's key3.
  const uint64_t key64 = ((uint64_t)base << 24) ^ ((uint64_t)fmt << 40) ^
                         ((uint64_t)width << 12) ^ (uint64_t)height ^
                         (3ull << 61);
  if (!dedupe(key64)) { r.refuse = CubeCapRefuse::Duplicate; return r; }

  const uint8_t* pB = rd(base, L.baseSpanBytes);
  if (!pB) { r.refuse = CubeCapRefuse::BaseUnreadable; return r; }
  const uint8_t* pM = nullptr;
  if (L.mipsTotalExtentBytes) {
    pM = rd(mipAddr, L.mipsTotalExtentBytes);
    if (!pM) { r.refuse = CubeCapRefuse::MipUnreadable; return r; }
  }

  // BOTH regions, concatenated: base faces, then the mip chain. The recorder
  // serializes bytes + mipBytes and the replayer splits on those same counts.
  for (uint32_t i = 0; i < L.baseSpanBytes; i++) scratch[i] = pB[i];
  for (uint32_t i = 0; i < L.mipsTotalExtentBytes; i++)
    scratch[L.baseSpanBytes + i] = pM[i];

  const uint32_t off = snap(scratch, L.totalBytes);
  if (off == arenaFull) { r.refuse = CubeCapRefuse::ArenaFull; return r; }
  r.arenaOffset = off;

  // texDim=3 CUBE, depth-1=5 (six faces), mipLevels-1 in 27-29, packedMips in
  // 30 — the allocation in gfx/hle_video.hpp.
  r.fmtBits = (fmt & 0xFFu) | ((tiled ? 1u : 0u) << 8) | ((endian & 3u) << 12) |
              (sr::gfx::hle::kUploadTexDimCube << sr::gfx::hle::kUploadTexDimShift) |
              (5u << 16) |
              ((mipLevels - 1u) << sr::gfx::hle::kUploadMipLevelsShift) |
              (packedMips ? sr::gfx::hle::kUploadPackedMipsBit : 0u);
  r.refuse = CubeCapRefuse::None;
  r.published = true;
  pub(r);
  return r;
}

}  // namespace sr::kernel::cubecap
