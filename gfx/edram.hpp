#pragma once

// The EDRAM model for the PM4 backbone (docs/RENDERER_PM4_HYBRID.md, phase P1), split for the release composition
// (docs/RELEASE_COMPOSITION.md section 8, stage 3): gfx/surface_key.hpp (ship: the surface identity, the format math,
// the alias geometry) + gfx/edram_model.hpp (oracle: the tile map, the EDRAM word addressing, the CPU resolve from the
// mirror). This umbrella is what difftest --edram-test, tests/xenos_conformance and the oracle unit read; a shipping
// unit includes surface_key.hpp.

#include "surface_key.hpp"
#include "edram_model.hpp"
