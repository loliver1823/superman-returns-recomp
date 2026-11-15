#pragma once

// Internal to the PM4 frame renderer (gfx/d3d12_frame_render.cpp, shipping) and its oracle half, the EDRAM word model
// (gfx/d3d12_edram_model.cpp; docs/RELEASE_COMPOSITION.md section 8, stage 3): the helpers both units use, which were
// file-local to gfx/d3d12_pm4_edram.cpp before the split. Nothing here is the model - the depth copy-plane helpers,
// the staging sizes, the host-pixel rectangle, the transfer constants, the shared HLSL pack / unpack text and the
// per-frame counters are the renderer's. Readers: the two units only.

#include <chrono>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <wrl/client.h>

#include "nb_surfaces.hpp"
#include "surface_key.hpp"

namespace sr {
namespace gfx {

// The resolve executor's phase timers (iter-460 perf census; both units' phases).
using Pm4Clk = std::chrono::steady_clock;
inline double pm4Ms(Pm4Clk::time_point t0) {
  return std::chrono::duration<double, std::milli>(Pm4Clk::now() - t0).count();
}

// Staging (readback / upload) buffer size shared by the frame renderer and the model's CPU paths.
constexpr uint64_t kPm4StagingBytes = 128ull << 20;

// A depth surface's two copy planes (0 = depth, 1 = stencil) as placed in a staging buffer, with each plane's bytes
// per texel.
struct Pm4DepthPlanes {
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
  uint32_t bpt[2]{};
  uint64_t total = 0;
};
bool pm4DepthFootprints(ID3D12Device* dev, ID3D12Resource* res, uint64_t base, Pm4DepthPlanes& pl);
// The stencil byte's position inside a plane-1 texel (R8 / X24G8 / X32G8X24).
inline uint32_t pm4StencilByte(uint32_t bpt1) { return bpt1 == 1u ? 0u : (bpt1 == 4u ? 3u : 4u); }
// Guest EDRAM word of one depth texel: D24S8 keeps the host unorm24 bits exactly; D24FS8 converts the host float to
// 20e4 (the render-target format's encoding, surface_key.hpp packDepth).
uint32_t pm4DepthGuestBits(uint32_t depthFmt, const uint8_t* mapped, const Pm4DepthPlanes& pl, uint32_t x,
                           uint32_t y);
void pm4DepthPutGuestBits(uint32_t depthFmt, uint8_t* staged, const Pm4DepthPlanes& pl, uint32_t x, uint32_t y,
                          uint32_t word);
void pm4Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b);
inline uint64_t pm4AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// Host pointer for guest physical `phys` through the owning window (gfx/d3d12_frame_render.cpp).
uint8_t* pm4GuestWritableWin(uint32_t phys, size_t& avail, int& win);
uint8_t* pm4GuestWritable(uint32_t phys, size_t& avail);

// SR_PM4_DUMP_FROM / _TO / _FRAME / _EVERY / _DIR: the per-batch RT dumps (a development diagnostic of the renderer).
struct Pm4DumpCfg {
  long from = -1, to = -1, frame = -1, every = 1;
  std::string dir;
  bool hit(long bi, long epoch) const {
    return from >= 0 && bi >= from && bi <= to && (frame < 0 || epoch == frame) &&
           (every <= 1 || (bi - from) % every == 0);
  }
};
const Pm4DumpCfg& pm4DumpCfg();
void pm4WriteBmp(const std::string& path, const uint8_t* bgra, uint32_t w, uint32_t h);
// One destination texel (guest bytes with the endian swap undone) as float4.
void pm4DecodeDest(uint32_t fmt, const uint8_t* p, float c[4]);

// Per-frame counters (reported once per rendered batch set).
struct Pm4Stats {
  uint32_t draws = 0, skipped = 0, noRt = 0, transfers = 0, tilesMoved = 0, resolves = 0,
           resolveTexels = 0, clears = 0, flushes = 0, newRts = 0;
  double msFlush = 0, msStoreCpu = 0, msLoadCpu = 0, msResolveCpu = 0;  // where the time goes
};
extern Pm4Stats g_pm4Stats;
// iter-460 perf census: the resolve executor's phases per frame.
extern double g_resMsStore, g_resMsShm, g_resMsInval, g_resMsClear, g_resMsCpu;

// The transfer constants (16 root constants) of the direct transfer passes and of the model's store / load passes.
struct Pm4XferConsts {
  uint32_t base, pitchTiles, msaa, flags;
  uint32_t rectX, rectY, rectW, rectH;
  uint32_t samples, bit, d0Off, d0Pitch;
  uint32_t s1Off, s1Pitch, s1Bpt, s1Byte;
};
static_assert(sizeof(Pm4XferConsts) == 64, "16 root constants");
// r: HOST pixels (pm4HostRect). The scale rides in flags bits 16-21 (0 at 1x).
Pm4XferConsts pm4XferConsts(edram::RtKey k, const edram::Rect& r);
// iter-464 resscale: a guest-pixel rectangle (edram::rangeRects) in host pixels.
edram::Rect pm4HostRect(const edram::Rect& r);

// The pack / unpack HLSL both units' passes share (the render-target format encodings and the alias geometry of a
// transfer: the constants, PackColor / UnpackColor, SampleOffset, the full-RT triangle VSQuad, the direct passes'
// source lookup). The frame renderer appends the direct transfer passes, the model its store / load passes.
extern const char kPm4PackHlsl[];
// D3DCompile of one entry of `src` (SR_PM4_HALF_SIGNED decides SR_HALF_HEXCMP); null on failure (logged as `name`).
Microsoft::WRL::ComPtr<ID3DBlob> pm4CompileHlsl(const char* src, size_t srcLen, const char* name, const char* entry,
                                                 const char* target);

// The surface table (gfx/nb_surfaces.hpp; EDRAM zero step 3): the alias table the shipping path decides with.
nbsurf::Table& nbSurfTable();
bool nbSurfTableOn();   // SR_NB_SURF_TABLE (=0: the tile map decides - the oracle unit's arm)
bool nbClaimFastOn();   // SR_NB_CLAIM_FAST: a claim of tiles the claimer owns whole is a no-op (a performance A/B)
bool pm4GpuEdramEnabled();  // SR_PM4_EDRAM_GPU (=0: the CPU mirror - the oracle unit's arm)
// The oracle unit's tile map: its range count, for the frame report (SR_ORACLE builds only; declared for both).
size_t edramOwnRanges();

}  // namespace gfx
}  // namespace sr
