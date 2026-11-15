#pragma once
// iter-467 freezefx: SR_PM4_QUAD_STRIP (default ON; =0 keeps the old split for an A/B).
//
// How a Xenos kQuadList quad is split into two host triangles. Xenia's D3D12 backend (THE
// reference for PM4/Xenos semantics) never converts quad lists on the CPU: its primitive
// processor reports quad lists as supported and draws them through a geometry shader that emits
// the strip v0, v1, v3, v2 (reference/xenia-canary/src/xenia/gpu/d3d12/pipeline_cache.cc,
// PipelineGeometryShader::kQuadList) - triangles (0,1,3) and (3,1,2), the 1-3 diagonal. Our
// conversions used (0,1,2)(0,2,3), the 0-2 diagonal (Xenia's CPU fallback, unused on D3D12).
// The diagonal decides two things: the surface of a non-planar quad (cloth - the cape silhouette
// on the title screen), and the interpolation of attributes that are not affine across the quad.
// The game's soft-particle VS B341612F writes per-vertex 1/depth terms and collapses particle
// corners behind the camera to (0,0,0,0); with the 0-2 diagonal the frozen meteor's frost cloud
// faded out over the near buildings (trace 5794 draw #3309, ENGINE_MAP 6g.8b). The vertex order
// inside each triangle was measured not to matter (5 rotations, byte-identical replays).
#include <cstdlib>

#include "../kernel/sr_oracle.hpp"

inline bool pm4QuadStripOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_QUAD_STRIP");
    return !e || e[0] != '0';
  }();
  return v;
#else
  // C1 stage 3 (docs/RELEASE_COMPOSITION.md 3.1 rule 2): the =0 arm is the old 0-2 diagonal split - a fallback
  // component; the release configuration keeps Xenia's strip order only, the gate's name absent.
  return true;
#endif
}

// The 6 quad-relative indices of the quad's two triangles.
inline const int* pm4QuadSplit() {
  static const int k[2][6] = {{0, 1, 2, 0, 2, 3}, {0, 1, 3, 3, 1, 2}};
  return k[pm4QuadStripOn() ? 1 : 0];
}
