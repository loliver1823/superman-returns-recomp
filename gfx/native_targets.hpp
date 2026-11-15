// iter-467 native-b (strand B), dependency D1 (docs/NATIVE_ROADMAP.md "Dependency points"):
// the render targets a native draw record carries once strand A stops producing a register file
// (stage 5). Until then strand B derives the same thing from the register file
// (edram::computeDrawTargets); fromDrawTargets / toDrawTargets keep the two interchangeable, so the
// draw drain consumes either.
//
// A target is the game's SURFACE, keyed as edram::RtKey: EDRAM base (tiles), pitch (32-pixel tiles),
// MSAA mode, depth or colour, format. Strand B keeps one persistent host render target per key and
// moves pixels between keys only where two surfaces alias at the same EDRAM base (B1).
#pragma once
#include <cstdint>
#include "surface_key.hpp"  // C1 stage 3: edram::DrawTargets / RtKey are the surface identity (shipping)

namespace sr {
namespace gfx {

struct NativeTargets {
  uint32_t color[4] = {};       // edram::RtKey values; 0 = slot unbound
  uint8_t colorMask[4] = {};    // normalized RGBA write mask per slot (0 = unbound)
  uint32_t depth = 0;           // edram::RtKey value; 0 = no depth / stencil
  uint16_t extentTiles[5] = {}; // tiles each binding covers from its base: colour 0..3, depth 4
  uint8_t flags = 0;            // bit 0 z enable, 1 z write, 2 stencil, bits 4..6 z func
};

inline NativeTargets fromDrawTargets(const edram::DrawTargets& t) {
  NativeTargets n;
  for (int i = 0; i < 4; i++) {
    n.colorMask[i] = t.colorWriteMask[i];
    n.color[i] = t.colorWriteMask[i] ? t.color[i].v : 0u;
  }
  n.depth = t.depthBound ? t.depth.v : 0u;
  for (int i = 0; i < 5; i++) n.extentTiles[i] = uint16_t(t.claimTiles[i]);
  n.flags = uint8_t((t.zEnable ? 1u : 0u) | (t.zWrite ? 2u : 0u) | (t.stencilEnable ? 4u : 0u) |
                    ((t.zFunc & 7u) << 4));
  return n;
}

inline edram::DrawTargets toDrawTargets(const NativeTargets& n) {
  edram::DrawTargets t;
  t.rasterize = n.depth != 0u || n.colorMask[0] || n.colorMask[1] || n.colorMask[2] || n.colorMask[3];
  for (int i = 0; i < 4; i++) {
    t.colorWriteMask[i] = n.colorMask[i];
    t.color[i] = edram::RtKey(n.colorMask[i] ? n.color[i] : 0u);
  }
  t.depthBound = n.depth != 0u;
  t.depth = edram::RtKey(n.depth);
  t.zEnable = (n.flags & 1u) != 0;
  t.zWrite = (n.flags & 2u) != 0;
  t.stencilEnable = (n.flags & 4u) != 0;
  t.zFunc = (n.flags >> 4) & 7u;
  for (int i = 0; i < 5; i++) t.claimTiles[i] = n.extentTiles[i];
  return t;
}

}  // namespace gfx
}  // namespace sr
