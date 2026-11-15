#pragma once
// volume_gates.hpp - step-4 lane C round 2 switches for the volume (k3D) texture path, from Astra's review of the
// SR_TEX3D_MIPS / SR_XLAT_SAMPLER_W bake (dev 7af4a541, archive/step4_claude_20261018/render/astra_merge.txt).
// Baked ON after the round's replay matrix (lane C NOTES 2026-10-18 round 2); =0 keeps the earlier behaviour for an
// A/B in the dev build, the release has no such arm (constant true).
//
// SR_TEX3D_V2          the volume resource: capture / invalidation spans from gfx/volume_layout.hpp (P2-1), the
//                      endian swap by guest byte lanes including sub-dword tails (P2-2), and a volume whose level
//                      count changes (a base-only fallback that later finds its mip pages) is recreated (P2-6).
// SR_XLAT_SAMPLER_V2   a k3D fetch samples through a full Xenia sampler (d3d12_texture_cache.cc
//                      GetSamplerParameters + WriteSampler): U / V / W clamp modes incl. mirror and border, the
//                      border colour, min / mag / mip filters, anisotropy, MinLOD = mip_min_level and kBaseMap as
//                      MaxLOD = MinLOD (+0.25) with the mip chain kept (P2-3, P2-4, P2-5).
//
// Round 3 (Astra's review of round 2, archive/step4_claude_20261018/render/astra_round2.txt), default OFF until the
// round's replay matrix:
// SR_TEX3D_V3          the endian conversion by the format's block size, as Xenia's loaders (8-bit: none, 16-bit:
//                      8in16 only, 32-bit and wider: the 32-bit word swap; volume_layout.hpp swapGuestUnits), and a
//                      volume loaded base-only because a replay never supplied its mip pages keeps the mip range
//                      registered for invalidation, so the pages arriving later reload it with its chain.
// SR_XLAT_SAMPLER_V3   the 48 volume sampler entries are reclaimed least-recently-used once full (an entry whose
//                      last use the GPU has finished is rewritten in place) instead of the 49th distinct sampler
//                      falling back to a legacy recipe.
#include <cstdlib>

#include "../kernel/sr_oracle.hpp"

namespace sr {
namespace gfx {
namespace volume {

inline bool tex3dV2On() {
#if SR_ORACLE
  static const bool on = [] {
    const char* e = std::getenv("SR_TEX3D_V2");
    return !e || e[0] != '0';
  }();
  return on;
#else
  return true;
#endif
}

inline bool samplerV2On() {
#if SR_ORACLE
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_SAMPLER_V2");
    return !e || e[0] != '0';
  }();
  return on;
#else
  return true;
#endif
}

inline bool tex3dV3On() {
#if SR_ORACLE
  static const bool on = [] {
    const char* e = std::getenv("SR_TEX3D_V3");
    return e && e[0] == '1';
  }();
  return on;
#else
  return false;
#endif
}

inline bool samplerV3On() {
#if SR_ORACLE
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_SAMPLER_V3");
    return e && e[0] == '1';
  }();
  return on;
#else
  return false;
#endif
}

}  // namespace volume
}  // namespace gfx
}  // namespace sr
