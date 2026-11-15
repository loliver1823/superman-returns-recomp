#include "kernel/resource_reader_observe.hpp"
#include "host_hlsl.hpp"  // step 4 7a: SR_HOST_HLSL_AOT (host HLSL compiled ahead of time)
#include "../kernel/mem_sites.hpp"  // step 4 7d: SR_MEM_SITES allocation names
// iter-460 S2/S3 (SR_PM4_SHMEM): Xenia's shared memory + GPU resolves.
//
// Model (xenia gpu/shared_memory.cc, d3d12_shared_memory.cc,
// d3d12_render_target_cache.cc Resolve): one 512 MiB GPU buffer indexed by
// guest PHYSICAL address. Guest pages reach it on demand (RequestRange);
// resolves are Xenia's precompiled resolve_* compute shaders reading the EDRAM
// buffer (already in the edram.xesli layout, pm4GpuStore) and writing the
// shared memory directly - no GPU sync, no CPU pixel loop.
//
// CPU-write detection: Xenia write-protects watched pages. Here a page is
// re-hashed (XXH3) at most once per frame epoch when requested; a changed hash
// re-uploads it. A GPU-written page (resolve output) stays authoritative until
// the CPU copy of that page changes - Xenia's valid_and_gpu_written rule.
// (A Windows write-watch arena cost the guest ~47 ms/frame: guest_context.cpp.)
//
// Window rule: the texture cache's (gpuPhysToHost: the 0x8 alias; a page that
// is empty there but written through the 0xA window uploads from 0xA - the
// movie planes, iter-427d).

#include "d3d12_backend.hpp"
#include "cp_simd.hpp"
#include "cube_layout.hpp"
#include "native_owner_rects.hpp"
#include "gamma_ramp.hpp"
#include "pm4_half_gate.hpp"  // iter-467 freezefx: SR_PM4_HALF_SIGNED (native_resolve.inc)
#include "gfx_options.hpp"   // iter-466 gfx: the present chain's options
#include "surface_key.hpp"  // C1 stage 3: the surface identity, the format math (shipping; the word model is the oracle unit's)
#include "res_scale.hpp"
#include "shader_cache.hpp"
#include "xenos_tiling.hpp"
#include "xenos_state.hpp"  // GuestDrawCommand (drainCollectTextures)
#include "../app/log.hpp"
#include "../kernel/guest_context.hpp"
#include "../kernel/phys_window.hpp"
#if SR_ORACLE
#include "../kernel/gpu_write_watch.hpp"
#endif
#include "resource_memory.hpp"
#include "resource_ab.hpp"  // mem.resources frozen-pass A/B: resourceDecides() (the constant true under SR_ORACLE=0)
#include "resource_texid.hpp"  // mem.resources textures_from_resolves: the texture-identity census (no-op under SR_ORACLE=0)
#if SR_ORACLE
#include "resource_memory_check.hpp"
#endif
#include <stdexcept>
#include "../kernel/hitch_log.hpp"  // iter-466 stutter: SR_HITCH_LOG (observe-only)
#include "../kernel/ab_toggle.hpp"  // iter-467 native-b: in-roll A/B of the NB gates (TOOLKIT 1.8d)
#include "../kernel/native_meter.hpp"  // mem.resources step 1: the shm column = this model's page validations (inline no-op under SR_ORACLE=0)
#include "resource_census.hpp"  // mem.resources step 2a: the registry in shadow at the meter's decision events (SR_MEM_RESOURCES=census; no-op under SR_ORACLE=0)
#include "pm4_live_trace.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdarg>  // iter-467 native-b census (native_targets.inc)
#include <cstdio>
#include <cstring>
#include <thread>
#include <memory>
#include <filesystem>  // iter-467 native-b SR_NB_AOT
#include <fstream>

namespace sr {
namespace gfx {

namespace {
#if SR_ORACLE
// iter-466 cp round 2 (SR_CP_OWN_MEMO, see shmOwnMemo): hits / misses / check mismatches.
std::atomic<uint64_t> g_ownHit{0}, g_ownMiss{0}, g_ownMismatch{0};
// iter-467 native-b SR_NB_SHM_RANGE_CACHE (see pm4ShmRequest): hits / stores / misses / CHECK.
std::atomic<uint64_t> g_rcHits{0}, g_rcStores{0}, g_rcMissRel{0}, g_rcMissOwn{0}, g_rcChecked{0},
    g_rcMismatch{0};
// iter-467 native-b SR_NB_PREPASS_FLAT (see shmCollectTextures): the set generation; reused / new / CHECK.
std::atomic<uint32_t> g_prepGen{1};
std::atomic<uint64_t> g_prepHits{0}, g_prepNew{0}, g_prepMismatch{0};
#endif
}  // namespace

namespace xeshaders {
#if SR_ORACLE  // C1 stage 3: Xenia's resolve CS reads the EDRAM buffer - the oracle unit's path (pm4ShmResolve)
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_8bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_16bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_128bpp_cs.h"
#endif
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_8bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_16bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_32bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_64bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_128bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_depth_float_cs.h"
// iter-464 resscale: the resolution-scaled variants (Xenia builds them from the same
// .xesli sources with XE_RESOLVE_RESOLUTION_SCALED / XE_TEXTURE_LOAD_RESOLUTION_SCALED).
#if SR_ORACLE
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_8bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_16bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_128bpp_scaled_cs.h"
#endif
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_8bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_16bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_32bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_64bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_128bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/texture_load_depth_float_scaled_cs.h"
}  // namespace xeshaders

void pm4ShmBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                   D3D12_RESOURCE_STATES b);             // d3d12_pm4_edram.cpp
uint8_t* pm4ShmGuestWritable(uint32_t phys, size_t& avail);  // d3d12_pm4_edram.cpp (the 0x8 copy)
uint8_t* pm4ShmGuestWritableWin(uint32_t phys, size_t& avail, int& win);  // the owning window

bool pm4ShmEnabled() {
#if !SR_ORACLE
  return true;  // native resource storage has no model/CPU fallback selector
#else
  static const bool v = [] {
    // P3 (iter-460): default ON with the PM4 live path; =0 keeps the CPU
    // texture / vertex path for an A/B (too slow for the title city: CP stall).
    const char* e = std::getenv("SR_PM4_SHMEM");
    return !e || e[0] != '0';
  }();
  return v;
#endif
}

// Validation mode (SR_PM4_SHMEM_READBACK=1): copy each resolve's destination
// extent back into guest RAM (one GPU sync per resolve), Xenia's
// readback-resolve option. Default off: CPU consumers of resolve output sync
// only the GPU-written pages they read (pm4ShmSyncToGuest).
#if SR_ORACLE
static bool shmReadbackEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_SHMEM_READBACK");
    return e && e[0] == '1';
  }();
  return v;
}
#endif

namespace {
constexpr uint64_t kShmBytes = 1ull << 29;       // 512 MiB, xenia kBufferSize
constexpr uint32_t kShmPages = uint32_t(kShmBytes >> 12);
void natStartPipelines(ID3D12Device* dev, bool wait);  // gfx/native_resolve.inc (pm4ShmInit starts the build)
constexpr uint64_t kShmUploadThird = 64ull << 20;  // per-frame upload arena third (3 frames)
// The calling thread's request mode (pm4ShmSetDrainMode / the pre-pass).
#if SR_ORACLE
thread_local std::vector<sr::gfx::D3D12Backend::ShmCand>* t_shmCollectInto = nullptr;
#endif
thread_local bool t_shmDefer = false;    // queue the copies (the CP's drain)
#if SR_ORACLE
thread_local bool t_shmPrepass = false;  // the render's texture pre-pass (resolve mask applies)
#endif
constexpr uint64_t kShmReadbackBytes = 32ull << 20;

// xenia draw_util.cc:913-924 (group_size_x_log2, group_size_y_log2).
constexpr uint32_t kResolveGroupLog2[9][2] = {{6, 3}, {6, 3}, {5, 3}, {5, 3}, {6, 3},
                                              {5, 3}, {5, 3}, {5, 3}, {4, 3}};

struct XeResolveConsts {
  uint32_t edramInfo, coordInfo, destInfo, destCoordInfo, destBase;
};

// xenos.h:614-641 (IsColorResolveFormatBitwiseEquivalent), raw enum values.
bool bitwiseEquivalent(uint32_t rtFormat, uint32_t destFormat) {
  switch (rtFormat) {
    case 0: case 1: return destFormat == 6 || destFormat == 14 || destFormat == 50;
    case 2: case 10: return destFormat == 7 || destFormat == 54;
    case 6: return destFormat == 31;
    case 7: return destFormat == 32;
    case 14: return destFormat == 36;
    case 15: return destFormat == 37;
    default: return false;
  }
}

// texture_util.cc:450-477 GetTiledAddressUpperBound2D.
uint32_t tiledUpperBound2D(uint32_t right, uint32_t bottom, uint32_t pitch, uint32_t bpbLog2) {
  if (!right || !bottom) return 0;
  uint32_t ub = uint32_t(xenos_tiling::tiled2D(int32_t((right - 1) & ~31u),
                                               int32_t((bottom - 1) & ~31u), pitch, bpbLog2));
  switch (bpbLog2) {
    case 0: ub += 0xA00; break;
    case 1: ub += 0xC00; break;
    default: ub += 0x400u << bpbLog2; break;
  }
  return ub;
}

// GetResolveInfo + ResolveInfo::GetCopyShader (draw_util.cc:926-1391) from
// the fields computeResolve already derived. Returns the shader index, or -1.
// iter-464 resscale: sx/sy = the draw resolution scale (ResolveCoordinateInfo
// draw_resolution_scale_x/_y, bits 16-18 / 19-21; draw_util.cc:1076-1079), the
// half-pixel fill flag (ResolveEdramInfo bit 29, draw_util.cc:1228-1233) and the
// dispatch over the SCALED rectangle (GetCopyShader, draw_util.cc:1371-1382). The
// destination extent stays guest addresses. At 1x the constants are unchanged.
int buildResolve(const edram::ResolveInfo& ri, const edram::Regs& r, XeResolveConsts& c,
                 uint32_t& gx, uint32_t& gy, uint32_t& extStart, uint32_t& extLen,
                 uint32_t sx = 1, uint32_t sy = 1) {
  if (!ri.copy || ri.destArray) return -1;
  const uint32_t bpt = edram::destBytesPerTexel(ri.destFormat);
  if (!bpt) return -1;
  uint32_t bpbLog2 = 0;
  while ((1u << bpbLog2) < bpt) bpbLog2++;
  const uint32_t msaa = ri.msaa;
  const uint32_t lx = msaa >= edram::kMsaa4X ? 1u : 0u, ly = msaa >= edram::kMsaa2X ? 1u : 0u;
  const uint32_t x0s = ri.x0 << lx, y0s = ri.y0 << ly;
  const uint32_t pitchTiles32 = ((ri.surfacePitch << lx) + 79u) / 80u;
  const uint32_t baseOffTiles = (y0s / 16u) * pitchTiles32 + (x0s / 80u);
  const uint32_t offX8 = (x0s % 80u) >> (lx + 3u), offY8 = (y0s % 16u) >> (ly + 3u);
  const uint32_t widthDiv8 = (ri.x1 - ri.x0) >> 3, heightDiv8 = (ri.y1 - ri.y0) >> 3;
  if (!widthDiv8 || !heightDiv8) return -1;
  uint32_t rtFormat, is64 = 0, pitchTilesF, baseTiles;
  if (ri.isDepth) {
    rtFormat = (r.depthInfo >> 16) & 1u;
    pitchTilesF = pitchTiles32;
    baseTiles = (r.depthInfo & 0xFFFu) + baseOffTiles;
  } else {
    const uint32_t ci = r.colorInfo[ri.srcIndex & 3u];
    rtFormat = (ci >> 16) & 0xFu;
    is64 = ri.src64 ? 1u : 0u;
    pitchTilesF = pitchTiles32 << is64;
    baseTiles = (ci & 0xFFFu) + (baseOffTiles << is64);
  }
  c.edramInfo = (pitchTilesF & 0x3FFu) | ((msaa & 3u) << 10) | (uint32_t(ri.isDepth) << 12) |
                ((baseTiles & 0x7FFu) << 13) | ((rtFormat & 0xFu) << 24) | (is64 << 28);
  c.coordInfo = (offX8 & 0xFu) | ((offY8 & 1u) << 4) | ((widthDiv8 & 0x7FFu) << 5) |
                ((sx & 7u) << 16) | ((sy & 7u) << 19);
  // fill_half_pixel_offset: scaled, half_pixel_offset (default true), and
  // PA_SU_VTX_CNTL.pix_center == kD3DZero (resolve_resolution_scale_fill_half_pixel_offset
  // default true).
  if ((sx > 1 || sy > 1) && !(r.vtxCntl & 1u)) c.edramInfo |= 1u << 29;
  // RB_COPY_DEST_INFO, sanitized (draw_util.cc:1289-1303): the resolved
  // format; depth has no exp bias and no swap. Our EDRAM holds the true guest
  // encoding, so no +5 fixed16 bias (fixed_*_truncated = false).
  uint32_t di = r.copyDestInfo;
  di = (di & ~(0x3Fu << 7)) | ((ri.destFormat & 0x3Fu) << 7);
  if (ri.isDepth) di &= ~((0x3Fu << 16) | (1u << 24));
  c.destInfo = di;
  const uint32_t mask = (1u << (7u - std::min(2u, bpbLog2))) - 1u;  // 2D base granularity
  c.destCoordInfo = ((ri.destPitch >> 5) & 0x3FFu) | (((ri.destHeight >> 5) & 0x3FFu) << 10) |
                    (((ri.x0 & mask) >> 3) << 20) | (((ri.y0 & mask) >> 3) << 24) |
                    ((ri.sampleSelect & 7u) << 28);
  c.destBase = ri.destBase + uint32_t(xenos_tiling::tiled2D(int32_t(ri.x0 & ~mask),
                                                            int32_t(ri.y0 & ~mask),
                                                            ri.destPitch, bpbLog2));
  extStart = ri.destBase + uint32_t(xenos_tiling::tiled2D(int32_t(ri.x0 & ~31u),
                                                          int32_t(ri.y0 & ~31u), ri.destPitch,
                                                          bpbLog2));
  const uint32_t extEnd = ri.destBase + tiledUpperBound2D(ri.x1, ri.y1, ri.destPitch, bpbLog2);
  extLen = extEnd > extStart ? extEnd - extStart : 0u;
  int shader;
  const bool expBias = ((di >> 16) & 0x3Fu) != 0;
  if (ri.isDepth || (!expBias && ri.sampleSelect <= 3u && bitwiseEquivalent(rtFormat, ri.destFormat))) {
    shader = msaa >= edram::kMsaa4X ? (is64 ? 3 : 1) : (is64 ? 2 : 0);
  } else {
    switch (bpt) {
      case 1: shader = 4; break;
      case 2: shader = 5; break;
      case 4: shader = 6; break;
      case 8: shader = 7; break;
      case 16: shader = 8; break;
      default: return -1;
    }
  }
  const uint32_t w = (widthDiv8 << 3) * sx, h = (heightDiv8 << 3) * sy;
  gx = (w + (1u << kResolveGroupLog2[shader][0]) - 1u) >> kResolveGroupLog2[shader][0];
  gy = (h + (1u << kResolveGroupLog2[shader][1]) - 1u) >> kResolveGroupLog2[shader][1];
  return shader;
}

// Xenia texture load shaders (texture_cache.cc:72-108): {bytes per host block,
// guest x blocks per thread log2}; group = 4x32 threads.
struct LoadShaderInfo { const void* bc; size_t n; uint32_t hostBlockBytes, xLog2; };
enum ShmLoad { kL8, kL16, kL32, kL64, kL128, kLDepthUnorm, kLDepthFloat, kLCount };
const LoadShaderInfo kLoadShaders[kLCount] = {
    {xeshaders::texture_load_8bpb_cs, sizeof(xeshaders::texture_load_8bpb_cs), 1, 4},
    {xeshaders::texture_load_16bpb_cs, sizeof(xeshaders::texture_load_16bpb_cs), 2, 4},
    {xeshaders::texture_load_32bpb_cs, sizeof(xeshaders::texture_load_32bpb_cs), 4, 3},
    {xeshaders::texture_load_64bpb_cs, sizeof(xeshaders::texture_load_64bpb_cs), 8, 2},
    {xeshaders::texture_load_128bpb_cs, sizeof(xeshaders::texture_load_128bpb_cs), 16, 1},
    {xeshaders::texture_load_depth_unorm_cs, sizeof(xeshaders::texture_load_depth_unorm_cs), 4, 3},
    {xeshaders::texture_load_depth_float_cs, sizeof(xeshaders::texture_load_depth_float_cs), 4, 3},
};

// iter-464 resscale: the same loaders built with XE_TEXTURE_LOAD_RESOLUTION_SCALED
// (source = the scaled resolve memory, always tiled; texture_load.xesli:96-157).
const LoadShaderInfo kLoadShadersScaled[kLCount] = {
    {xeshaders::texture_load_8bpb_scaled_cs, sizeof(xeshaders::texture_load_8bpb_scaled_cs), 1, 4},
    {xeshaders::texture_load_16bpb_scaled_cs, sizeof(xeshaders::texture_load_16bpb_scaled_cs), 2, 4},
    {xeshaders::texture_load_32bpb_scaled_cs, sizeof(xeshaders::texture_load_32bpb_scaled_cs), 4, 3},
    {xeshaders::texture_load_64bpb_scaled_cs, sizeof(xeshaders::texture_load_64bpb_scaled_cs), 8, 2},
    {xeshaders::texture_load_128bpb_scaled_cs, sizeof(xeshaders::texture_load_128bpb_scaled_cs), 16, 1},
    {xeshaders::texture_load_depth_unorm_scaled_cs, sizeof(xeshaders::texture_load_depth_unorm_scaled_cs), 4, 3},
    {xeshaders::texture_load_depth_float_scaled_cs, sizeof(xeshaders::texture_load_depth_float_scaled_cs), 4, 3},
};

// The load shader for each format the texture cache decodes, with the SAME
// host DXGI format it creates (d3d12_texture_cache.h host_formats_ rows; the
// PM4 path maps _EXPAND to _FLOAT like Xenia's GetBaseFormat).
int loadShaderFor(uint32_t fmt) {
  switch (fmt) {
    case 2: return kL8;                                        // k_8 -> R8
    case 24: case 27: case 30: return kL16;                    // k_16, _EXPAND, _FLOAT
    case 6: case 50: case 25: case 28: case 31: case 36: return kL32;
    case 18: case 26: case 29: case 32: return kL64;           // DXT1 (BC1), 16x4
    case 19: case 20: case 49: return kL128;                   // DXT2_3, DXT4_5, DXN
    case 22: return kLDepthUnorm;                              // k_24_8 -> R32_FLOAT
    case 23: return kLDepthFloat;                              // k_24_8_FLOAT -> R32_FLOAT
    default: return -1;
  }
}

// iter-464 resscale: a format a resolve can write and a scaled loader exists for
// (Xenia IsScaledResolveSupportedForFormat: load_pipelines_scaled_; block-compressed
// formats have none, d3d12_texture_cache.cc:267-318, 1292-1297).
bool scaledLoadable(uint32_t fmt) {
  if (fmt == 18 || fmt == 19 || fmt == 20 || fmt == 49) return false;  // DXT1/3/5, DXN
  return loadShaderFor(fmt) >= 0;
}

// 16 MiB heaps behind the scaled resolve memory (kScaledResolveHeapSizeLog2 = 24).
constexpr uint64_t kSrHeapBytes = 16ull << 20;

#if SR_ORACLE
uint64_t zeroPageHash() {
  static const uint64_t h = [] {
    static const uint8_t z[4096] = {};
    return ShaderCache::hashUcode(z, sizeof(z));
  }();
  return h;
}
#endif
}  // namespace

bool D3D12Backend::pm4ShmInit() {
  if (m_shmReady) return true;
  if (m_shmFailed || !pm4GpuInit()) return false;
  sr::kernel::MemSiteScope memSite("pm4ShmInit (PM4_SHARED_MEMORY DEFAULT 512 MB, upload arena, readback)");   // step 4 7d: [MEM_SITE] names (SR_MEM_SITES, diagnostic)
  auto fail = [&](const char* what) {
    m_shmFailed = true;
#if SR_ORACLE
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PM4_SHM] init failed at %s - CPU resolves stay", what);
#else
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_RESOURCE_STORAGE] init failed at %s", what);
#endif
    return false;
  };
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = kShmBytes;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  m_shmState = D3D12_RESOURCE_STATE_COMMON;  // committed DEFAULT memory is zeroed
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, m_shmState, nullptr,
                                               IID_PPV_ARGS(&m_shm))))
    return fail("shared memory buffer");
  m_shm->SetName(L"PM4_SHARED_MEMORY");
  vsfShmSrvCreate();  // the in-shader vertex fetch reads it (SR_PM4_VSF_SHM)
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  bd.Width = kShmUploadThird * 3;
  bd.Flags = D3D12_RESOURCE_FLAG_NONE;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&m_shmUpload))))
    return fail("upload arena");
  m_shmUpload->SetName(L"PM4_SHM_UPLOAD");
  const D3D12_RANGE noRead{0, 0};
  void* mp = nullptr;
  if (FAILED(m_shmUpload->Map(0, &noRead, &mp)) || !mp) return fail("upload map");
  m_shmUploadPtr = static_cast<uint8_t*>(mp);
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  bd.Width = kShmReadbackBytes;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&m_shmReadback))))
    return fail("readback buffer");
#if SR_ORACLE  // C1 stage 3: the EDRAM-path resolve (Xenia's resolve CS over the EDRAM buffer) is the oracle unit's
  // Resolve root signature (d3d12_render_target_cache.cc:351-429, unscaled):
  // {0: 5 constants, 1: UAV u0 shared memory, 2: SRV t0 EDRAM}.
  {
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 5;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = p;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_shmResolveRs))))
      return fail("resolve root signature");
  }
  struct Bc { const void* p; size_t n; };
  const Bc bcs[9] = {
      {xeshaders::resolve_fast_32bpp_1x2xmsaa_cs, sizeof(xeshaders::resolve_fast_32bpp_1x2xmsaa_cs)},
      {xeshaders::resolve_fast_32bpp_4xmsaa_cs, sizeof(xeshaders::resolve_fast_32bpp_4xmsaa_cs)},
      {xeshaders::resolve_fast_64bpp_1x2xmsaa_cs, sizeof(xeshaders::resolve_fast_64bpp_1x2xmsaa_cs)},
      {xeshaders::resolve_fast_64bpp_4xmsaa_cs, sizeof(xeshaders::resolve_fast_64bpp_4xmsaa_cs)},
      {xeshaders::resolve_full_8bpp_cs, sizeof(xeshaders::resolve_full_8bpp_cs)},
      {xeshaders::resolve_full_16bpp_cs, sizeof(xeshaders::resolve_full_16bpp_cs)},
      {xeshaders::resolve_full_32bpp_cs, sizeof(xeshaders::resolve_full_32bpp_cs)},
      {xeshaders::resolve_full_64bpp_cs, sizeof(xeshaders::resolve_full_64bpp_cs)},
      {xeshaders::resolve_full_128bpp_cs, sizeof(xeshaders::resolve_full_128bpp_cs)},
  };
  for (int i = 0; i < 9; i++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = m_shmResolveRs.Get();
    cd.CS = {bcs[i].p, bcs[i].n};
    if (FAILED(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_shmResolvePso[i]))))
      return fail("resolve pso");
  }
#endif
  // Texture load root signature (d3d12_texture_cache.cc:150-181):
  // {0: 10 constants, 1: SRV t0 shared memory, 2: UAV u0 scratch}.
  {
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 10;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = p;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_shmLoadRs))))
      return fail("texture load root signature");
  }
  for (int i = 0; i < kLCount; i++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = m_shmLoadRs.Get();
    cd.CS = {kLoadShaders[i].bc, kLoadShaders[i].n};
    if (FAILED(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_shmLoadPso[i]))))
      return fail("texture load pso");
  }
  // iter-464 resscale: the scaled resolve memory and the scaled resolve / load
  // pipelines (only when the draw resolution is scaled).
  if (resscale::get().scaled() && !srInit())
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[RES_SCALE] scaled resolve memory init FAILED - resolves will be dropped");
  m_shmStamp.assign(kShmPages, 0u);
#if SR_ORACLE
  m_shmHash8.assign(kShmPages, zeroPageHash());
  m_shmHashA.assign(kShmPages, zeroPageHash());
#endif
  m_shmEpochOf.assign(kShmPages, 0u);
  m_shmGpuWritten.assign(kShmPages, 0u);
#if SR_ORACLE
  m_shmSrcA.assign(kShmPages, 0u);
  m_shmFresh8.assign(kShmPages, 0u);
  m_shmFreshA.assign(kShmPages, 0u);
#endif
  m_shmReady = true;
#if SR_ORACLE
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_SHM] ready: 512 MiB shared memory, 2x%llu MiB upload arena, xenia resolve CS x9 "
              "(readback %s)",
              (unsigned long long)(kShmUploadThird >> 20), shmReadbackEnabled() ? "ON" : "off");
  // mem.resources BAKED (lane C, 2026-10-17): the route this run decides resources by, and how it was chosen
  // (form default = SR_MEM_RESOURCES unset; off = =0, the page model; census = the page model + the shadow census)
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES_NATIVE] route %s form=%s",
              memres::resourceMode() ? "native" : "page_model", memres::resourceRouteForm());
#endif
  // release/emu-stage3 (Astra round 1 P2): the native resolve pipelines start here, not at the first resolve - the
  // shipped blobs inline (SR_NB_AOT), else compiled (the dev build on a thread / SR_NATIVE_RESOLVE_SYNC inline; the
  // release configuration inline: a missing or stale blob set compiles once at init, [NB_AOT] compiled).
  natStartPipelines(m_device.Get(), /*wait=*/false);
  return true;
}

void D3D12Backend::pm4ShmState(D3D12_RESOURCE_STATES s) {
  if (m_shmState == s) {
    if (s == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && m_shmUavDirty) {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      b.UAV.pResource = m_shm.Get();
      m_commandList->ResourceBarrier(1, &b);
      m_shmUavDirty = false;
    }
    return;
  }
  pm4ShmBarrier(m_commandList.Get(), m_shm.Get(), m_shmState, s);
  m_shmState = s;
  m_shmUavDirty = false;  // any transition out of UAV commits the writes
}

// Per frame: a new validation epoch and the other upload-arena half (the
// frame that used it two presents ago has completed: moveToNextFrame).
#if SR_ORACLE
void D3D12Backend::pm4ShmBeginFrame() {
  if (!m_shmReady) return;
  static uint32_t s_frames = 0;
  static uint64_t s_up = 0, s_res = 0, s_tex = 0, s_arr = 0;
  static double s_rb = 0;
  if (++s_frames <= 8 || s_frames % 100 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_SHM_FRAME] #%u since last: uploaded=%.2f MiB resolves=%llu texLoads=%llu "
                "arrayLoads=%llu readbackMs=%.1f | owner-window uploads total: 0xA=%llu 0x8=%llu | "
                "gpu-page clobbers total=%llu window conflicts total=%llu | last frame: pages "
                "requested=%u hashed=%u lookupMs=%.2f hashMs=%.2f clean=%u repeat=%u",
                s_frames, double(m_shmUploadBytes - s_up) / 1048576.0,
                (unsigned long long)(m_shmResolves - s_res),
                (unsigned long long)(m_shmTexLoads - s_tex),
                (unsigned long long)(m_shmArrayLoads - s_arr), m_shmMsReadback - s_rb,
                (unsigned long long)m_shmOwnerA, (unsigned long long)m_shmOwner8,
                (unsigned long long)m_shmClobbers, (unsigned long long)m_shmConflicts,
                m_shmFramePagesReq, m_shmFramePagesHashed, m_shmFrameMsLookup, m_shmFrameMsHash,
                m_shmFramePagesClean, m_shmFramePagesRepeat);
  // iter-466 cp lane: the write tracker's totals (kernel/gpu_write_watch.hpp), same cadence.
  if (sr::kernel::gww::enabled()) {
    sr::kernel::gww::beginFrame();
    if (s_frames <= 8 || s_frames % 100 == 0) {
      const sr::kernel::gww::Stats w = sr::kernel::gww::stats();
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[SHM_WT] #%u totals: faults=%llu (pages released %llu) protect runs=%llu (pages %llu) "
                  "host writes=%llu | check: %llu skipped pages hashed, %llu stale | rebaseline kept %llu | "
#if SR_ORACLE
                  "tex marks tracked=%llu frame=%llu block invalidations=%llu | ib cache hits=%llu "
                  "miss=%llu | fetch cb reused=%llu | const blocks reused=%llu | own memo hit=%llu miss=%llu "
#else
                  "tex marks tracked=%llu frame=%llu block invalidations=%llu | "
                  "fetch cb reused=%llu | const blocks reused=%llu | own memo hit=%llu miss=%llu "
#endif
                  "mismatch=%llu | const track uploads reused=%llu gaps=%llu mismatch=%llu | "
                  "range cache hits=%llu stores=%llu misses released=%llu owner=%llu check=%llu mismatch=%llu | "
                  "prepass flat reused=%llu new=%llu check mismatch=%llu | gen-mark late=%llu refused=%llu",
                  s_frames, (unsigned long long)w.faults, (unsigned long long)w.faultPages,
                  (unsigned long long)w.protects, (unsigned long long)w.protectPages,
                  (unsigned long long)w.hostWrites, (unsigned long long)w.staleChecks,
                  (unsigned long long)w.stale, (unsigned long long)m_shmWtRebaseKept,
                  (unsigned long long)m_texTrackedMarks, (unsigned long long)m_texFrameMarks,
                  (unsigned long long)m_texBlockInvals,
#if SR_ORACLE
                  (unsigned long long)m_ibConvHits, (unsigned long long)m_ibConvMiss,
#endif
                  (unsigned long long)m_vsfCbDedup,
                  (unsigned long long)m_cdHits, (unsigned long long)g_ownHit.load(),
                  (unsigned long long)g_ownMiss.load(), (unsigned long long)g_ownMismatch.load(),
                  (unsigned long long)m_cvUpReuse, (unsigned long long)m_cvGaps, (unsigned long long)m_cvMismatch,
                  (unsigned long long)g_rcHits.load(), (unsigned long long)g_rcStores.load(),
                  (unsigned long long)g_rcMissRel.load(), (unsigned long long)g_rcMissOwn.load(),
                  (unsigned long long)g_rcChecked.load(), (unsigned long long)g_rcMismatch.load(),
                  (unsigned long long)g_prepHits.load(), (unsigned long long)g_prepNew.load(),
                  (unsigned long long)g_prepMismatch.load(), (unsigned long long)w.genLate,
                  (unsigned long long)w.genRefused);
    }
  }
  m_shmFramePagesReq = m_shmFramePagesHashed = 0;
  m_shmFramePagesClean = m_shmFramePagesRepeat = 0;
  m_shmFrameMsHash = m_shmFrameMsLookup = 0;
  if (s_frames <= 8 || s_frames % 100 == 0) {
    s_up = m_shmUploadBytes;
    s_res = m_shmResolves;
    s_tex = m_shmTexLoads;
    s_arr = m_shmArrayLoads;
    s_rb = m_shmMsReadback;
  }
  // (No new epoch here: the render accepts every validation since
  // m_shmEpochFrameStart - the drains of this frame did the hashing.)
}

#endif
// End of a frame's render: the per-frame texture dedupe starts over and, on
// the synchronous path, the queued-copy list (every entry was recorded by its
// batch; the render thread recycles its frame's own list). The arena thirds
// rotate at the next set's first drain (pm4ShmBeginSet), not here.
void D3D12Backend::pm4ShmEndFrame() {
  if (!m_shmReady) return;
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  if (!m_rtOn) {  // (render thread: the CP's prepass owns the dedupe - iter-465 perf2)
#if SR_ORACLE
    m_shmTexCollected.clear();
    std::memset(m_shmTexPrev, 0, sizeof(m_shmTexPrev));
    g_prepGen.fetch_add(1, std::memory_order_relaxed);  // SR_NB_PREPASS_FLAT: a new set
#endif
    m_shmPending.clear();
  }
}

// iter-461: the first drain of a frame set. The set stages into `third`
// (its cursor and high-water mark start over; the frame that used it two
// presents ago has completed - the caller waited for its fence), the render
// of this set uploads into the same third (the synchronous path sets it here,
// the render thread when it takes the frame), and the set's first epoch is
// what its render accepts validations from.
void D3D12Backend::pm4ShmBeginSet(uint32_t third) {
  if (memres::resourceMode()) memres::resourceMemory().beginFrame();
#if SR_ORACLE
  memres::resourceCheckFrame();
  // mem.resources frozen-pass A/B (gfx/resource_ab.hpp): the set's decider, latched on the CP before its first decision.
  memres::resourceAbBeginSet();
  m_resArmSet = memres::resourceAbArm();
#endif
  m_setEpochFirst = m_shmEpoch;
  if (!m_shmReady) return;
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  m_shmDrainThird = third;
  if (!m_rtOn) m_shmRenderThird = third;
  m_shmUploadUsedT[third] = uint64_t(third) * kShmUploadThird;
  m_shmArenaHighT[third] = m_shmUploadUsedT[third];
  shmSpillRelease(third);
}

// A spill chunk with room for `bytes`, tagged with the third (32 MiB chunks,
// or the run's own size when larger). nullptr = allocation failed.
uint8_t* D3D12Backend::shmSpillAlloc(uint32_t third, uint64_t bytes, uint64_t& off,
                                     ID3D12Resource*& res) {
  for (ShmSpill& c : m_shmSpill) {
    if (c.third != third || c.used + bytes > c.size) continue;
    off = c.used;
    c.used += bytes;
    res = c.res.Get();
    return c.ptr + off;
  }
  ShmSpill c;
  c.third = third;
  c.size = std::max<uint64_t>(bytes, 32ull << 20);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = c.size;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(sr::hitch::timed(sr::hitch::kResCreate, [&] {
        return m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&c.res));
      })))
    return nullptr;
  c.res->SetName(L"PM4_SHM_SPILL");
  const D3D12_RANGE noRead{0, 0};
  void* mp = nullptr;
  if (FAILED(c.res->Map(0, &noRead, &mp)) || !mp) return nullptr;
  c.ptr = static_cast<uint8_t*>(mp);
  c.used = bytes;
  off = 0;
  res = c.res.Get();
  m_shmSpillBytes += c.size;
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1) + 1;
  if (n <= 8 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
#if SR_ORACLE
                "[PM4_SHM] #%u arena third %u full at drain time: %.1f MiB spill chunk (%.1f MiB live)",
#else
                "[NATIVE_RESOURCE_STORAGE] #%u arena third %u full at drain time: %.1f MiB spill chunk (%.1f MiB live)",
#endif
                n, third, double(c.size) / 1048576.0, double(m_shmSpillBytes) / 1048576.0);
  m_shmSpill.push_back(std::move(c));
  return m_shmSpill.back().ptr;
}

// The third is reused: its frame's fence completed, its spill chunks go.
void D3D12Backend::shmSpillRelease(uint32_t third) {
  for (size_t i = 0; i < m_shmSpill.size();) {
    if (m_shmSpill[i].third != third) {
      i++;
      continue;
    }
    m_shmSpillBytes -= m_shmSpill[i].size;
    m_shmSpill[i].res->Unmap(0, nullptr);
    m_shmSpill.erase(m_shmSpill.begin() + ptrdiff_t(i));
  }
}

void D3D12Backend::pm4ShmSetDrainMode(bool collect, bool defer) {
#if SR_ORACLE
  t_shmCollectInto = collect ? &m_shmCollect : nullptr;
  t_shmDefer = defer;
  t_shmPrepass = false;
#else
  (void)collect;  // release captures each native request immediately
  t_shmDefer = defer;
#endif
}

// The GPU is idle after pm4Flush: the render's third is free again, except
// the bytes of copies still queued for later batches (m_shmArenaHighT).
void D3D12Backend::pm4ShmAfterFlush() {
  if (!m_shmReady) return;
  const uint32_t t = m_shmRenderThird;
  m_shmUploadUsedT[t] = std::max(uint64_t(t) * kShmUploadThird, m_shmArenaHighT[t]);
  m_shmUavDirty = false;    // ExecuteCommandLists is a full UAV barrier
}

#if SR_ORACLE
bool D3D12Backend::replayOrderedRestores() const {
  return m_replayRestoreActive && !memres::resourceMode();
}

void D3D12Backend::replayRestoredRange(uint32_t phys, uint32_t len) {
  static const bool enabled = [] {
    const char* value = std::getenv("SR_REPLAY_ORDERED_RESTORES");
    return value && std::strcmp(value, "1") == 0;
  }();
  if (!enabled || memres::resourceMode() || !len) return;
  phys &= 0x1FFFFFFFu;
  if (uint64_t(phys) + len > kShmBytes)
    throw std::runtime_error("replay restore crosses physical storage");
  if (m_rtOn || pm4RenderThreadGate() || !pm4RtCacheEnabled() || !pm4ShmEnabled())
    throw std::runtime_error("ordered replay restores require synchronous shared storage");
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  m_replayRestoreActive = true;
  // Xenia invalidates whole host pages even for exact-range playback writes.
  // Preserve the CPU page image outside a partial MemoryRead, not older GPU
  // bytes. Repeated partial restores before a draw merge through actual RAM.
  const uint32_t first = phys >> 12;
  const uint32_t last = uint32_t((uint64_t(phys) + len - 1u) >> 12);
  for (uint32_t page = first; page <= last; ++page) {
    const uint32_t offset = page << 12;
    const auto* host = sr::kernel::guestVAToHost(0x80000000u | offset);
    const auto* alias = sr::kernel::guestVAToHost(0xA0000000u | offset);
    if (!host || !alias || !sr::kernel::isGuestVAInRange(0xA0000000u | (offset + 4095u)))
      throw std::runtime_error("replay restore has no complete CPU page");
    ReplayRestorePage capture;
    std::memcpy(capture.bytes.data(), host, capture.bytes.size());
    capture.hashA = ShaderCache::hashUcode(alias, 4096);
    m_replayRestores.insert_or_assign(page, std::move(capture));
  }
}

void D3D12Backend::replayStageRestores() {
  if (!replayOrderedRestores() || m_replayRestores.empty()) return;
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  if (!m_shmReady || m_rtOn)
    throw std::runtime_error("replay restore staging has no synchronous storage set");
  while (!m_replayRestores.empty()) {
    auto begin = m_replayRestores.begin(), end = begin;
    const uint32_t first = begin->first;
    uint32_t count = 0;
    while (end != m_replayRestores.end() && end->first == first + count && count < 256) {
      ++count;
      ++end;
    }
    const uint64_t bytes = uint64_t(count) * 4096;
    uint64_t offset = 0;
    ID3D12Resource* source = nullptr;
    auto hashes = std::make_shared<std::vector<uint64_t>>();
    hashes->reserve(count);
    uint8_t* mapped = shmSpillAlloc(m_shmDrainThird, bytes, offset, source);
    if (!mapped) throw std::runtime_error("replay restore cannot allocate immutable staging");
    uint32_t index = 0;
    for (auto it = begin; it != end; ++it, ++index) {
      std::memcpy(mapped + uint64_t(index) * 4096, it->second.bytes.data(), 4096);
      hashes->push_back(it->second.hashA);
    }
    ShmPend entry{uint64_t(first) << 12, offset, bytes, false, source};
    entry.replayHashA = std::move(hashes);
    entry.replayBytes = mapped;
    m_shmPending.push_back(std::move(entry));
    for (uint32_t page = first; page < first + count; ++page)
      m_shmEpochOf[page] = m_shmEpoch;
    m_replayRestores.erase(begin, end);
  }
}

void D3D12Backend::replayPrepareTail() {
  if (!replayOrderedRestores()) return;
  if (m_rtOn) throw std::runtime_error("ordered replay tail cannot use a render worker");
  if (!pm4ShmInit()) throw std::runtime_error("replay tail cannot initialize storage");
  if (!m_pm4DrainOpen) {
    // A frame containing only restores still owns its upload third. No pending
    // copy may be allocated before this rotation frees the previous use.
    m_drawBatches.clear();
    m_xlatThird = (m_xlatThird + 1u) % 3u;
    pm4WaitThirdFree(m_xlatThird);
    pm4ShmBeginSet(m_xlatThird);
    // step 6 I10p: the CB third follows (no cursor is reset here, as before; no draw follows in this set); in check
    // mode its reuse is asserted / rechecked / poisoned like the drain's rotation (the shared helper; this empty set
    // does not own the cursor, so its own poison range is empty - Astra r2 P3).
    zcRotateCbThird(false);
    m_pm4DrainOpen = true;
  }
  // No batch cursor is raised: these copies execute after the last draw and
  // before scanout, via renderPm4Batches' existing pending tail.
  replayStageRestores();
}
#endif

void D3D12Backend::pm4RecordBatchPending(const DrawBatch& batch, std::vector<ShmPend>& list,
                                       uint32_t& cursor) {
  if (batch.shmPendEnd > cursor) {
    pm4ShmRecordPending(list, cursor, batch.shmPendEnd);
    cursor = batch.shmPendEnd;
  }
}

// Record the queued copies [begin, end) of `list` on the open frame list.
void D3D12Backend::pm4ShmRecordPending(std::vector<ShmPend>& list, uint32_t begin, uint32_t end) {
  if (!m_shmReady) return;
  end = std::min<uint32_t>(end, uint32_t(list.size()));
  bool any = false;
  for (uint32_t i = begin; i < end; i++)
    if (!list[i].done) { any = true; break; }
  if (!any) return;
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  pm4ShmState(D3D12_RESOURCE_STATE_COPY_DEST);
  for (uint32_t i = begin; i < end; i++) {
    ShmPend& e = list[i];
    if (e.done) continue;
    const uint32_t p0 = uint32_t(e.shmOff >> 12);
    const uint32_t n = uint32_t((e.shmOff + e.bytes + 4095u) >> 12) - p0;
    bool replayRestore = false;
#if SR_ORACLE
    replayRestore = e.replayHashA != nullptr;
    if (replayRestore && ((e.shmOff & 4095u) || (e.bytes & 4095u) ||
                          e.replayHashA->size() != n || !e.replayBytes))
      throw std::runtime_error("replay restore copy metadata is inconsistent");
#endif
    // A patch replaces a few CPU-written dwords only: the page's GPU-written
    // bytes stay the GPU's (resourcePatchScaled: a scaled copy cannot take them).
    if ((memres::resourceMode() || replayRestore) && m_srReady && !e.patch)
      srClearPages(p0, n);
    if (e.patch) resourcePatchScaled(p0);
    m_commandList->CopyBufferRegion(m_shm.Get(), e.shmOff, e.src ? e.src : m_shmUpload.Get(),
                                    e.arenaOff, e.bytes);
    e.done = true;
#if SR_ORACLE
    if (memres::resourceConsumerEnabled()) {
      if (m_resourceRecordedBaselines.empty()) m_resourceRecordedBaselines.resize(memres::kPages);
      const bool wholePages = !(e.shmOff & 4095u) && !(e.bytes & 4095u) && e.consumerCopies.size() == n;
      for (uint32_t k = 0; k < n; ++k)
        m_resourceRecordedBaselines[p0 + k] = wholePages ? std::move(e.consumerCopies[k]) : nullptr;
      std::vector<std::shared_ptr<const memres::ConsumerBaseline>>{}.swap(e.consumerCopies);
    }
#endif
    // iter-461: the pages' stamps move when the copy is RECORDED, not when the
    // drain queued it - a texture loaded earlier in command order (the previous
    // frame's list, on the render thread) read the old bytes, and its version
    // must differ from the stamp a later bind sees.
    ++m_shmStampClock;
    for (uint32_t k = 0; k < n; k++) {
      m_shmStamp[p0 + k] = m_shmStampClock;
      if ((memres::resourceMode() || replayRestore) && !e.patch) m_shmGpuWritten[p0 + k] = 0;
#if SR_ORACLE
      if (replayRestore) {
        m_shmHash8[p0 + k] = ShaderCache::hashUcode(e.replayBytes + uint64_t(k) * 4096, 4096);
        m_shmHashA[p0 + k] = (*e.replayHashA)[k];
        m_shmFresh8[p0 + k] = m_shmFreshA[p0 + k] = 0;
        m_shmSrcA[p0 + k] = 0;
      }
#endif
    }
    if (memres::resourceMode() || replayRestore) invalidateGuestRange(uint32_t(e.shmOff), e.bytes);
  }
}

// iter-460 (SR_PM4_SHM_GPU_AUTH, default ON): GPU-written pages stay valid until
// the CPU writes them (Xenia shared_memory.cc: a resolve marks its range valid;
// only a CPU write-watch hit invalidates it). Our CPU-change detector is the
// page hash, so a resolve re-baselines both windows' hashes, and on a page no
// allocator record assigns to one window, a change seen only through the 0xA
// alias does not displace GPU data: our 0x8/0xA windows are separate memory
// that can both claim one physical page (the game's 299 MB probe is never
// reclaimed, so the 224 MB pool wraps onto heap-routed grants such as the
// front buffer), and there the 0xA bytes belong to the OTHER allocation. That
// chain uploaded foreign 0xA bytes over the resolved front buffer each frame
// - the 32x32 noise squares on the title (one 4 KB page = one 32x32 tile).
#if SR_ORACLE
static bool shmGpuAuthEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_SHM_GPU_AUTH");
    return !e || e[0] != '0';
  }();
  return v;
}

// iter-463 D8 (SR_PM4_TRACE_FULL, default ON; =0 records only what the
// renderer read, as before): while a live trace records, the trace also gets
// (a) every page a request skips because it was validated earlier in the
// epoch, and (b) pm4TraceDrawBacking's IB / VB / full texture backing of
// every draw, rejected ones included. Tracing only reads memory.
static bool traceFullEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_TRACE_FULL");
    return !e || e[0] != '0';
  }();
  return v;
}

// One page into the trace, from the window pm4ShmValidate serves it from, by
// validation's own rule (iter-463 review): the owner is the request range's
// (commandBufferWindow, then gpuWindowRange), else the page's own record
// (commandBufferWindow, then gpuWindow); owner 0xA takes the 0xA copy, an
// ambiguous page takes it only when the 0x8 copy is all zero, everything else
// the 0x8 copy. The records sit at the draw that references the page, so
// they must be the bytes the renderer draws from, not a window guess.
// A GPU-written page is recorded only when the CPU rewrote it since the
// resolve (its CPU copy's hash moved off the resolve's re-baseline, the same
// test that makes validation upload it): otherwise it is in the trace's
// initial state (pm4ShmTraceGpuPages) or an earlier resolve of the traced
// frame produces it, and a stale CPU copy recorded here would overwrite either
// in the player.
void D3D12Backend::shmTracePage(uint32_t p, int rangeOwn) {
  if (p >= kShmPages) return;
  // Resolved earlier in the traced frame (queued by the drain, executed at the
  // render): the player's resolve produces it, and so does ours.
  if (m_traceResolvedPages.count(p)) return;
  const bool gpuWritten = m_shmReady && m_shmGpuWritten[p];
  // Without SR_PM4_SHM_GPU_AUTH a resolve does not re-baseline the hashes, so
  // "the CPU rewrote it" cannot be told apart: the GPU data stands.
  if (gpuWritten && !shmGpuAuthEnabled()) return;
  size_t avail8 = 0;
  const uint8_t* h8 = pm4ShmGuestWritable(p << 12, avail8);
  // The pages pm4ShmValidate would read, read the way it reads them (a page
  // with no 0x8 copy is not served, and the VEH makes any host AV fatal).
  if (!h8 || avail8 < 4096) return;
  const uint32_t va = 0xA0000000u + (p << 12);
  const uint8_t* hA = sr::kernel::guestVAToHost(va);
  const bool hasA = hA && sr::kernel::isGuestVAInRange(va + 4095u);
  int ownWin = rangeOwn;
  if (!ownWin) {  // the page's own record, as pm4ShmValidate
    const int cbWin = sr::kernel::physwin::commandBufferWindowEnabled()
                          ? sr::kernel::physwin::commandBufferWindow(p << 12, 4096u)
                          : 0;
    ownWin = cbWin ? cbWin
                   : sr::kernel::physwin::gpuOwnerWindowEnabled()
                         ? sr::kernel::physwin::gpuWindow(p << 12, 4096u)
                         : 0;
  }
  const bool fromA = ownWin == 0xA && hasA;
  uint64_t k8 = 0;
  bool k8Ok = false;
  auto hash8 = [&]() {
    if (!k8Ok) {
      k8 = ShaderCache::hashUcode(h8, 4096);
      k8Ok = true;
    }
    return k8;
  };
  const uint8_t* ts = h8;
  if (fromA) ts = hA;
  else if (ownWin == 0 && hasA && hash8() == zeroPageHash()) ts = hA;
  if (gpuWritten) {
    // pm4ShmValidate's upload decision for a GPU-written page under GPU_AUTH:
    // owner 0xA - the 0xA hash moved; owner 8 and ambiguous - the 0x8 hash
    // moved (a change seen only through the 0xA alias does not displace GPU
    // data).
    const bool cpuRewrote = fromA ? ShaderCache::hashUcode(hA, 4096) != m_shmHashA[p]
                                  : hash8() != m_shmHash8[p];
    if (!cpuRewrote) return;
  }
  livetrace::memRead(p << 12, ts, 4096u);
}

// The owner of [phys, +len) as pm4ShmRequest computes it for one request.
static int shmTraceRangeOwner(uint32_t phys, uint32_t len) {
  const int cb = sr::kernel::physwin::commandBufferWindowEnabled()
                     ? sr::kernel::physwin::commandBufferWindow(phys, len)
                     : 0;
  if (cb) return cb;
  return sr::kernel::physwin::gpuOwnerWindowEnabled()
             ? sr::kernel::physwin::gpuWindowRange(phys, len)
             : 0;
}

void D3D12Backend::pm4TraceRange(uint32_t phys, uint32_t len) {
  if (!len || !livetrace::recording() || !traceFullEnabled()) return;
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  phys &= 0x1FFFFFFFu;
  const uint64_t end = std::min<uint64_t>(uint64_t(phys) + len, kShmBytes);
  if (end <= phys) return;
  const int rangeOwn = shmTraceRangeOwner(phys, uint32_t(end - phys));
  for (uint32_t p = phys >> 12; (uint64_t(p) << 12) < end; p++) shmTracePage(p, rangeOwn);
}

void D3D12Backend::pm4TraceNoteResolve(const edram::Regs& regs) {
  if (!livetrace::recording() || !traceFullEnabled()) return;
  uint32_t s = 0, l = 0;
  if (!pm4ShmResolveExtent(regs, s, l)) return;
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  s &= 0x1FFFFFFFu;
  const uint64_t end = std::min<uint64_t>(uint64_t(s) + l, kShmBytes);
  for (uint32_t p = s >> 12; (uint64_t(p) << 12) < end; p++) m_traceResolvedPages.insert(p);
}

// Xenia GetSubresourcesFromFetchConstant + GetGuestTextureLayout
// (texture_util.cc:23-114, 208-448) for ONE texture fetch constant: its base
// and mip backing, as tools/xtr_draws.py derive_texture_backing computes it.
// 2D / stacked / cube through cube_layout.hpp (faces = array size); a volume
// records its base volume (z slices aligned to kTextureTileDepth), mips not
// ported for 3D; 1D is skipped.
// Block geometry of every Xenos TextureFormat for trace SIZING only - xenia
// texture_info_formats.inl (block width, block height, bytes per block =
// bw * bh * bpp / 8), row for row with tools/xtr_draws.py FORMAT_BLOCK_TABLE.
// Independent of what the decoder supports (cubeBlockGeom): a texture the
// renderer cannot decode still has bytes Xenia's render needs (iter-463 review).
struct TraceBlockGeom { uint8_t bw, bh, bpb; };
static constexpr TraceBlockGeom kTraceBlockGeom[64] = {
    {1, 1, 0}, {1, 1, 0}, {1, 1, 1}, {1, 1, 2}, {1, 1, 2}, {1, 1, 2}, {1, 1, 4}, {1, 1, 4},
    {1, 1, 1}, {1, 1, 1}, {1, 1, 2}, {2, 1, 4}, {2, 1, 4}, {1, 1, 4}, {1, 1, 4}, {1, 1, 2},
    {1, 1, 4}, {1, 1, 4}, {4, 4, 8}, {4, 4, 16}, {4, 4, 16}, {1, 1, 8}, {1, 1, 4}, {1, 1, 4},
    {1, 1, 2}, {1, 1, 4}, {1, 1, 8}, {1, 1, 2}, {1, 1, 4}, {1, 1, 8}, {1, 1, 2}, {1, 1, 4},
    {1, 1, 8}, {1, 1, 4}, {1, 1, 8}, {1, 1, 16}, {1, 1, 4}, {1, 1, 8}, {1, 1, 16}, {4, 1, 4},
    {2, 1, 4}, {1, 1, 2}, {1, 1, 4}, {1, 1, 1}, {4, 1, 4}, {1, 1, 2}, {1, 1, 2}, {1, 1, 2},
    {1, 1, 4}, {4, 4, 16}, {1, 1, 4}, {4, 4, 8}, {4, 4, 16}, {4, 4, 16}, {1, 1, 4}, {1, 1, 4},
    {1, 1, 4}, {1, 1, 12}, {4, 4, 8}, {4, 4, 8}, {4, 4, 8}, {4, 4, 8}, {1, 1, 4}, {1, 1, 4},
};

void D3D12Backend::pm4TraceFetchBacking(const uint32_t* f) {
  namespace CL = sr::gfx::cube;
  if ((f[0] & 3u) != 2u) return;  // not a texture fetch constant
  const uint32_t fmt = f[1] & 0x3Fu;
  const uint32_t blockW = kTraceBlockGeom[fmt].bw, blockH = kTraceBlockGeom[fmt].bh;
  const uint32_t bpb = kTraceBlockGeom[fmt].bpb;
  if (!bpb) return;  // k_1 / k_1_REVERSE: 0 bytes per block (xenia sizes nothing)
  const uint32_t dim = (f[5] >> 9) & 3u;
  const bool tiled = ((f[0] >> 31) & 1u) != 0;
  const bool stacked = ((f[1] >> 10) & 1u) != 0;
  uint32_t basePage = (f[1] >> 12) & 0x1FFFFu;  // xenia: & 0x1FFFF (512 MB)
  uint32_t mipPage = (f[5] >> 12) & 0x1FFFFu;
  uint32_t width, height, arr = 1, depth = 1;
  if (dim == 2u) {
    width = (f[2] & 0x7FFu) + 1u;
    height = ((f[2] >> 11) & 0x7FFu) + 1u;
    depth = ((f[2] >> 22) & 0x3FFu) + 1u;
  } else if (dim == 1u || dim == 3u) {
    width = (f[2] & 0x1FFFu) + 1u;
    height = ((f[2] >> 13) & 0x1FFFu) + 1u;
    arr = dim == 3u ? 6u : (stacked ? ((f[2] >> 26) & 0x3Fu) + 1u : 1u);
  } else {
    return;
  }
  uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  if (pitchTexels < width) pitchTexels = width;
  uint32_t longest = std::max(width, height);
  if (dim == 2u) longest = std::max(longest, depth);
  const uint32_t sizeMax = CL::detail::log2Floor(longest);
  uint32_t mipMin = 0, mipMax = 0;
  if (mipPage) {
    mipMin = std::min((f[4] >> 2) & 0xFu, sizeMax);
    mipMax = std::max(std::min((f[4] >> 6) & 0xFu, sizeMax), mipMin);
  }
  if (mipMax) {
    if (!basePage) mipMin = std::max(mipMin, 1u);
    if (mipMin) basePage = 0;
  } else {
    mipPage = 0;
  }
  if (dim == 2u) {
    const uint32_t pitchBlocks = CL::detail::alignUp(CL::detail::alignUp(pitchTexels, blockW) / blockW, 32u);
    const uint32_t rows = CL::detail::alignUp(CL::detail::alignUp(height, blockH) / blockH, 32u);
    const uint64_t span = uint64_t(pitchBlocks) * bpb * rows * CL::detail::alignUp(depth, 4u);
    if (basePage && span <= (64u << 20)) pm4TraceRange(basePage << 12, uint32_t(span));
    return;
  }
  const bool packed = ((f[5] >> 11) & 1u) != 0;
  CL::CubeLayout L = CL::computeGuestLayout(fmt, width, height, pitchTexels, tiled, mipMax + 1u, packed,
                                            blockW, blockH, bpb, arr);
  if (!L.ok) {
    // Out of cube_layout's range (more than 13 levels, a zero extent): the
    // base-level span still goes in, so the trace is at worst mip-incomplete.
    uint32_t pb = 0;
    const uint32_t stride = CL::detail::levelSliceStride(0, true, width, height, pitchTexels, blockW,
                                                         blockH, bpb, tiled, &pb);
    if (basePage) pm4TraceRange(basePage << 12, stride * arr);
    return;
  }
  if (basePage) pm4TraceRange(basePage << 12, L.baseSpanBytes);
  if (mipPage && L.mipsTotalExtentBytes) pm4TraceRange(mipPage << 12, L.mipsTotalExtentBytes);
}

// Everything a draw's registers point at: the index buffer, every vertex
// fetch constant (a rejected draw has no shader to say which ones it uses;
// recording the rest only adds bytes the guest memory held anyway) and every
// texture fetch constant's backing.
void D3D12Backend::pm4TraceDrawBacking(const GuestDrawCommand& c) {
  if (!livetrace::recording() || !traceFullEnabled()) return;
  if (c.idxBasePhys && c.vertexCount)
    pm4TraceRange(c.idxBasePhys, c.vertexCount * (c.idxFmt32 ? 4u : 2u));
  for (uint32_t g = 0; g < 32u; g++) {
    const uint32_t* f = c.fetchRaw + g * 6u;
    if ((f[0] & 3u) == 2u) {
      pm4TraceFetchBacking(f);
      continue;
    }
    for (uint32_t v = 0; v < 3u; v++) {
      const uint32_t d0 = f[v * 2u], d1 = f[v * 2u + 1u];
      if ((d0 & 3u) != 3u) continue;
      const uint32_t phys = d0 & 0x1FFFFFFCu;
      const uint32_t bytes = ((d1 >> 2) & 0x00FFFFFFu) * 4u;
      if (phys && bytes && bytes <= (16u << 20)) pm4TraceRange(phys, bytes);
    }
  }
}

// iter-466 cp round 2 (SR_CP_OWN_MEMO): a request's window answer (the command-buffer window,
// else gpuWindowRange - or gpuWindow for one page) memoised per thread across drain epochs,
// tagged with kernel/phys_window.hpp ownerVersion: the per-64 KB-page mutation counters of
// every registry change and command-buffer note. Each drain re-requests the same vertex
// buffers and textures; their answers only change when something touching their pages moves
// (the physwin memos checked the whole grant log, or rescanned the notes, per call: ~12% of
// the CP). SR_CP_OWN_MEMO_CHECK=1 (diagnostic) also computes the answer and counts mismatches.
namespace {
struct OwnMemoEnt {
  uint64_t sum = 0;
  uint32_t phys = 0, len = 0;
  int32_t res = 0;
  bool valid = false, page = false;
};
bool ownMemoCheck() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CP_OWN_MEMO_CHECK");
    return e && e[0] == '1';
  }();
  return v;
}
template <class F>
int shmOwnMemo(uint32_t phys, uint32_t len, bool page, F uncached) {
  thread_local std::unique_ptr<OwnMemoEnt[]> t_tab;
  thread_local uint32_t t_hit = 0, t_miss = 0;
  if (!t_tab) t_tab.reset(new OwnMemoEnt[1u << 14]);
  OwnMemoEnt& e = t_tab[((phys * 0x9E3779B1u) ^ (len * 0x85EBCA6Bu) ^ (page ? 0xC2B2AE35u : 0u)) >> 18];
  const sr::kernel::physwin::OwnerVer v = sr::kernel::physwin::ownerVersion(phys, len);
  if (v.ok && e.valid && e.phys == phys && e.len == len && e.page == page && e.sum == v.sum) {
    if (++t_hit >= 4096) {
      g_ownHit.fetch_add(t_hit, std::memory_order_relaxed);
      t_hit = 0;
    }
    if (ownMemoCheck()) {
      const int u = uncached();
      if (u != e.res && sr::kernel::physwin::ownerVersion(phys, len).sum == v.sum) {
        const uint64_t k = g_ownMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k <= 16)
          SR_LOG_BOOT(sr::LogSeverity::Error, "[CP_OWN_MEMO] MISMATCH #%llu phys=%08X len=%u page=%d memo=%X now=%X",
                      (unsigned long long)k, phys, len, page ? 1 : 0, e.res, u);
      }
    }
    return e.res;
  }
  if (++t_miss >= 4096) {
    g_ownMiss.fetch_add(t_miss, std::memory_order_relaxed);
    t_miss = 0;
  }
  const int r = uncached();
  if (v.ok) {
    const sr::kernel::physwin::OwnerVer v2 = sr::kernel::physwin::ownerVersion(phys, len);
    if (v2.ok && v2.sum == v.sum && v2.cbSeq == v.cbSeq) {  // nothing touched the range meanwhile
      e.sum = v.sum;
      e.phys = phys;
      e.len = len;
      e.res = r;
      e.page = page;
      e.valid = true;
    }
  }
  return r;
}
}  // namespace

// iter-467 native-b (SR_NB_SHM_RANGE_CACHE, baked ON; =0 off; SR_AB_TOGGLE): the CP's validated-range cache. A drain
// re-validates every page it touches once per drain epoch - and the live path drains after every indirect buffer,
// so each vertex buffer is re-walked (m_shmMu, then a write-tracking check per page) many times a frame although
// write tracking already proves its pages unchanged. A range whose pages were ALL clean under its window when the
// write tracker's release clock read S is remembered per thread with S and the range's owner version; while no
// 64-page block of it carries a release stamp >= S (gww::blocksReleasedSince) and its owner version holds, a request is
// answered from the cache: no lock, no page walk. Exactly what the walk would decide (every page clean: counted
// validated without a hash). SR_NB_SHM_RANGE_CHECK=1 re-walks every hit under the lock and reports a page the
// walk would not have passed ([NB_SHMRC] CHECK MISMATCH); SR_NB_SHM_RANGE_TEST_NOREL=1 is its mutation. Totals: the
// [SHM_WT] line ("range cache hits=..").
namespace {
using ShmRangeEnt = D3D12Backend::ShmRangeTicket;
bool nbShmRangeCacheOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_SHM_RANGE_CACHE");
    // iter-467 native-b: baked ON (=0 A/B). In-roll A/B (fr22 / fr24, uncapped, two rolls each): GPU_CP -0.32 +-0.27
    // (1x), -0.30 +-0.26 (3x), -0.37 +-0.26 (1x) ms per frame, fps neutral; live CHECK 27.3M hits, 0 mismatch, 0 stale.
    return !e || e[0] != '0';
  }();
  static const int ab = sr::abt::reg("SR_NB_SHM_RANGE_CACHE");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}
// SR_NB_SHM_RANGE_PREPASS (default OFF - no gain: job24 GPU_CP +0.10 +-0.26 ms/frame; SR_AB_TOGGLE): the cache also
// answers the texture prepass's requests
// (shmCollectTextures). The prepass walk skips the pages a resolve of this frame writes (validated at the bind);
// a hit skips them the same way, and every other page of a cached range is clean (no upload either way).
bool nbShmRangePrepassOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_SHM_RANGE_PREPASS");
    return e && e[0] == '1';
  }();
  static const int ab = sr::abt::reg("SR_NB_SHM_RANGE_PREPASS");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}
bool nbShmRangeCheckOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_SHM_RANGE_CHECK");
    return e && e[0] == '1';
  }();
  return v;
}
}  // namespace

#endif
bool D3D12Backend::pm4ShmRequest(uint32_t phys, uint32_t len) {
#if SR_ORACLE
  return pm4ShmRequestT(phys, len, nullptr);
#else
  return pm4ResourceRequest(phys, len);
#endif
}

#if SR_ORACLE
bool D3D12Backend::pm4ShmEnsure(uint32_t phys, uint32_t len, ShmRangeTicket& t) {
  return pm4ShmRequestT(phys, len, &t);
}

#endif
// The native upload path uses the same arena and ordered CopyBufferRegion
// commands as the development model. Its decisions use announced generations.
// Texture versions still move when copies are RECORDED, not when a future frame
// stages them on the CPU (pm4ShmRecordPending owns that publication).
void D3D12Backend::resourcePrepareResolveRange(uint32_t phys, uint32_t len, uint32_t generation) {
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  if (!generation) throw std::logic_error("native resolve has no producer generation");
  // A replay (and the first live resolve) can reach this before any texture or
  // vertex fetch has initialized storage. Preservation is the first consumer.
  if (!pm4ShmInit())
    throw std::runtime_error("native resource storage initialization failed before resolve");
  // A partial resolve preserves the destination's untouched bytes. Capture
  // those pages before publishing this queued GPU operation's ownership.
  if (!pm4ResourceRequest(phys, len))
    throw std::runtime_error("native resolve cannot preserve its destination");
  memres::resourceMemory().noteGpuAt(phys, len, generation);
}

#if SR_ORACLE
// mem.resources frozen-pass A/B (gfx/resource_ab.hpp), a model set in dual mode: the registry hears the resolve's GPU
// write at its drain with its producer generation, exactly as the native arm's preservation publishes it, so the next
// native set decides on the same generations a native-only roll would. The model preserves at execution itself
// (native_targets.inc's request) and rebaselines there.
void D3D12Backend::resourceAbNoteResolve(DrawBatch& batch) {
  if (!memres::resourceAbDual()) return;
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  if (!batch.resourceGpuGeneration) throw std::logic_error("native resolve has no producer generation");
  uint32_t start = 0, bytes = 0;
  if (pm4ShmResolveExtent(batch.pm4Regs, start, bytes) && bytes) {
    memres::resourceMemory().noteGpuAt(start, bytes, batch.resourceGpuGeneration);
    memres::resourceAbCount(memres::AbEvent::kGpuNoteModelArm);
  }
}
#endif

void D3D12Backend::resourcePrepareResolve(DrawBatch& batch) {
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  if (!batch.resourceGpuGeneration)
    throw std::logic_error("native resolve has no producer generation");
  uint32_t start = 0, bytes = 0;
  if (pm4ShmResolveExtent(batch.pm4Regs, start, bytes))
    resourcePrepareResolveRange(start, bytes, batch.resourceGpuGeneration);
  batch.shmPendEnd = uint32_t(m_shmPending.size());
}

#if SR_ORACLE
memres::ConsumerWitness D3D12Backend::resourceConsumerWitness(uint32_t page) const {
  memres::ConsumerWitness out;
  out.page = page;
  if (page >= memres::kPages) return out;
  const uint32_t phys = page << 12;
  auto& memory = memres::resourceMemory();
  const auto before = sr::kernel::physwin::ownerVersion(phys, 4096);
  const uint32_t cpuBefore = memory.cpuGeneration(page);
  const auto grants = sr::kernel::physwin::ownersOfSpan(phys, 4096);
  const int owner = sr::kernel::physwin::gpuWindow(phys, 4096);
  out.window = memory.sourceWindow(page, owner);
  out.cpu = memory.cpuGeneration(page);
  const auto after = sr::kernel::physwin::ownerVersion(phys, 4096);
  out.ownerSum = after.sum;
  out.ownerSeq = after.cbSeq;
  out.grantGeneration = grants.gen;
  out.grantDigest = grants.setDigest;
  out.grantCount = grants.nLive;
  std::copy_n(grants.serial, 2, out.grantSerial);
  out.known = before.ok && after.ok && before.sum == after.sum && before.cbSeq == after.cbSeq &&
      cpuBefore == out.cpu && grants.nLive != 0 && (out.window == 8 || out.window == 0xA);
  return out;
}
#endif

uint32_t* D3D12Backend::resEpochOf() {
#if SR_ORACLE
  if (memres::resourceAbDual()) {  // the frozen-pass A/B (gfx/resource_ab.hpp STAMPS): the native arm's own stamps
    if (m_resEpochOf.size() != m_shmEpochOf.size()) m_resEpochOf.assign(m_shmEpochOf.size(), 0u);
    return m_resEpochOf.data();
  }
#endif
  return m_shmEpochOf.data();
}

namespace {
// step 6 I4 (SR_CP_RES_REQ_LEAN, default ON; =0 or SR_AB_TOGGLE turns it off): pm4ResourceRequest's same-drain range
// memo answers before m_shmMu, and a draw's asked texture spans share one hold for their misses (its vertex streams
// stay one request each: pm4StageVsf asks inside the census's per-slot scope). Every input of the memo's check is
// lock-free or the drain's own (archive/step6_i3i11 I4 freeze note, 10-24).
bool cpResReqLeanOn() {
  static const bool v = cpsimd::gateLogged("SR_CP_RES_REQ_LEAN", true);
  static const int ab = sr::abt::reg("SR_CP_RES_REQ_LEAN");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}
#if SR_ORACLE
// SR_CP_RES_REQ_CHECK=1: every lock-free memo hit is re-decided under the lock. A page the walk would not keep while
// the range's blocks and the armed patches still hold is BAD; a note or patch landing meanwhile is RACED.
bool cpResReqCheckOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CP_RES_REQ_CHECK");
    return e && e[0] == '1';
  }();
  return v;
}
#endif
// The drain's own asks: the memo's thread and the only one that counts. The render thread's asks are not (its
// texture prepass collects too under SR_ORACLE).
bool drainAsk() {
#if SR_ORACLE
  return (t_shmCollectInto != nullptr || t_shmDefer) && !t_shmPrepass;
#else
  return t_shmDefer;
#endif
}
}  // namespace

uint32_t D3D12Backend::resourceRangeMemoSlot(uint32_t phys, uint32_t len) {
  return ((phys * 0x9E3779B1u) ^ (len * 0x85EBCA77u)) >> (32u - kResourceRangeMemoLog2);
}

// I4 (a): the memo's check, without m_shmMu. Its inputs: the thread_local drain mode, the latched checker flags,
// m_shmEpoch (written only by the drain, under the lock, at its start), the registry's acquire atomics (the note
// sequence per block, the pending-patch bits), and the memo itself (read and written only when memoOk: the drain).
bool D3D12Backend::resourceRangeMemoHit(uint32_t phys, uint32_t len, uint32_t first, uint32_t last) {
#if SR_ORACLE
  const bool memoOk = drainAsk() && !memres::resourceCheckEnabled() && !memres::resourceConsumerEnabled();
#else
  const bool memoOk = t_shmDefer;
#endif
  if (!memoOk || m_resourceRangeMemo.empty()) return false;
  const ResourceRangeMemo& memo = m_resourceRangeMemo[resourceRangeMemoSlot(phys, len)];
  auto& memory = memres::resourceMemory();
  // keepStamp of a collecting ask is the drain epoch (the locked body's m_rtOn branch is for the render's asks)
  if (memo.phys != phys || memo.len != len || memo.stamp != m_shmEpoch ||
      !memory.unchangedSince(first, last, memo.sequence) || memory.patchPendingRange(first, last))
    return false;
  ++m_resourceRangeMemoHits;
  ++m_cpResReq.hitsNoLock;
#if SR_ORACLE
  if (cpResReqCheckOn()) resourceRangeMemoCheck(first, last, memo.sequence);
#endif
  return true;
}

#if SR_ORACLE
void D3D12Backend::resourceRangeMemoCheck(uint32_t first, uint32_t last, uint64_t sequence) {
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  auto& memory = memres::resourceMemory();
  ++m_cpResReq.checked;
  uint32_t page = first;
  bool patched = false;
  for (; page <= last; ++page) {
    if (memory.patchPending(page)) {
      patched = true;
      break;
    }
    if (memres::isUpload(memory.decide(page))) break;
  }
  if (page > last) return;
  // Astra I4 r1 P2: a writer publishes in two stores. A pending patch the hit did not see was noted after it (the
  // hit's own patchPendingRange excludes one already pending): RACED. A note raises its page generation before its
  // block sequence (Registry::noteCpuWrite), so an upload decision with the blocks unchanged may be a note between the
  // two: a SUSPECT, decided a full drain later (resourceRangeMemoResolve).
  if (patched || !memory.unchangedSince(first, last, sequence)) {
    ++m_cpResReq.raced;
    return;
  }
  m_cpResReqSuspects.push_back({first, last, page, sequence, m_cpResReq.drains});
}

// At a drain's end: a suspect one full drain old is RACED when its blocks have moved since the hit (the note's
// sequence landed), else BAD - the hit served a page the walk would have uploaded.
void D3D12Backend::resourceRangeMemoResolve() {
  auto& memory = memres::resourceMemory();
  size_t kept = 0;
  for (size_t i = 0; i < m_cpResReqSuspects.size(); ++i) {
    const CpResReqSuspect s = m_cpResReqSuspects[i];
    if (s.drain + 2u > m_cpResReq.drains) {
      m_cpResReqSuspects[kept++] = s;
      continue;
    }
    if (!memory.unchangedSince(s.first, s.last, s.sequence)) {
      ++m_cpResReq.raced;
      continue;
    }
    if (++m_cpResReq.bad <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[CP_RES_REQ] BAD lock-free memo hit #%llu: page %u of [%u, %u] would not be kept (a drain later)",
                  (unsigned long long)m_cpResReq.bad, s.page, s.first, s.last);
  }
  m_cpResReqSuspects.resize(kept);
}
#endif

bool D3D12Backend::pm4ResourceRequest(uint32_t phys, uint32_t len) {
  if (!m_shmReady || !len) return m_shmReady;
  phys &= 0x1FFFFFFFu;
  uint32_t first, last;
  if (!memres::Registry::pageRange(phys, len, first, last)) return false;
  const bool lean = cpResReqLeanOn();
  if (lean) {
    memres::resourceAbDecision(memres::AbWho::kNative, last - first + 1u);  // lock-free: thread_local + atomics
    if (resourceRangeMemoHit(phys, len, first, last)) return true;
  }
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  if (drainAsk()) ++m_cpResReq.holds;
  return pm4ResourceRequestLocked(phys, len, first, last, lean);
}

void D3D12Backend::pm4ResourceRequestSite(const memres::ResourceSpan* spans, uint32_t count) {
  constexpr uint32_t kSiteMax = 32;  // a draw's 16 texture slots, <= 2 spans each
  if (!cpResReqLeanOn() || count > kSiteMax) {
    for (uint32_t i = 0; i < count; ++i) pm4ResourceRequest(spans[i].phys, spans[i].len);
    return;
  }
  if (!m_shmReady) return;
  struct Miss { uint32_t phys, len, first, last; };
  Miss misses[kSiteMax];
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!spans[i].len) continue;
    const uint32_t phys = spans[i].phys & 0x1FFFFFFFu;
    uint32_t first, last;
    if (!memres::Registry::pageRange(phys, spans[i].len, first, last)) continue;
    memres::resourceAbDecision(memres::AbWho::kNative, last - first + 1u);
    if (resourceRangeMemoHit(phys, spans[i].len, first, last)) continue;
    misses[n++] = {phys, spans[i].len, first, last};
  }
  if (!n) return;
  // In ask order; the locked body checks the memo again (an earlier miss of the site may have made a later one current)
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  if (drainAsk()) ++m_cpResReq.holds;
  for (uint32_t i = 0; i < n; ++i)
    pm4ResourceRequestLocked(misses[i].phys, misses[i].len, misses[i].first, misses[i].last, true);
}

// [CP_RES_REQ] at a drain's end, on the drain thread (every counter is its own): the first 4 drains, then every 1000th.
void D3D12Backend::cpResReqCensus() {
  const uint64_t d = ++m_cpResReq.drains;
  size_t pending = 0;  // the CHECK's undecided suspects (SR_ORACLE)
#if SR_ORACLE
  if (!m_cpResReqSuspects.empty()) resourceRangeMemoResolve();
  pending = m_cpResReqSuspects.size();
#endif
  if (d > 4 && d % 1000) return;
  const auto& c = m_cpResReq;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[CP_RES_REQ] lean=%d drains=%llu hits_nolock=%llu locked_ranges=%llu holds=%llu memo_hits=%llu "
              "checked=%llu bad=%llu raced=%llu pending=%zu",
              int(cpResReqLeanOn()), (unsigned long long)d, (unsigned long long)c.hitsNoLock,
              (unsigned long long)c.lockedRanges, (unsigned long long)c.holds,
              (unsigned long long)m_resourceRangeMemoHits, (unsigned long long)c.checked, (unsigned long long)c.bad,
              (unsigned long long)c.raced, pending);
}

bool D3D12Backend::pm4ResourceRequestLocked(uint32_t phys, uint32_t len, uint32_t first, uint32_t last,
                                            bool decided) {
  if (!decided) memres::resourceAbDecision(memres::AbWho::kNative, last - first + 1u);  // the A/B's witness (dual only)
  if (drainAsk()) ++m_cpResReq.lockedRanges;
#if SR_ORACLE
  const bool collecting = t_shmCollectInto != nullptr || t_shmDefer;
  const bool deferred = t_shmDefer || (t_shmPrepass && m_rtOn);
#else
  const bool collecting = t_shmDefer;
  const bool deferred = t_shmDefer;
#endif
  auto& memory = memres::resourceMemory();
  struct Page {
    memres::ResourceMemory::UploadTicket ticket;
    const uint8_t* bytes;
#if SR_ORACLE
    memres::ConsumerWitness before;
#endif
  };
  // No reserve: a request whose pages are all current (each draw re-requests its
  // vertex and texture ranges) must not allocate.
  std::vector<Page> pages;
  std::vector<memres::ResourceMemory::Patch> patches;
#if SR_ORACLE
  // The checkers observe every kept page with its window; without them a keep
  // needs neither the allocation's window nor the metadata lock.
  const bool observeKeeps = memres::resourceCheckEnabled() || memres::resourceConsumerEnabled();
  const bool memoOk = collecting && !observeKeeps && !t_shmPrepass;
#else
  constexpr bool observeKeeps = false;
  const bool memoOk = collecting;
#endif
  const uint32_t keepStamp = m_rtOn && !collecting ? m_shmEpochFrameStart : m_shmEpoch;
  // The drain asks the same ranges many times (every draw re-requests its 16
  // texture slots and its vertex streams). A range this drain epoch found
  // current is current again while no note changed a page of its 64-page
  // blocks since the walk began (the registry's per-block change sequence: a
  // note elsewhere no longer invalidates it - prof2_ng_native) and no exact
  // CPU-write patch waits on its pages: the walk would decide keep for every
  // page and stamp the same epoch.
  ResourceRangeMemo* memo = nullptr;
  const uint64_t sequenceBefore = memory.sequence();
  if (memoOk) {
    if (m_resourceRangeMemo.empty()) m_resourceRangeMemo.resize(size_t{1} << kResourceRangeMemoLog2);
    memo = &m_resourceRangeMemo[resourceRangeMemoSlot(phys, len)];
    if (memo->phys == phys && memo->len == len && memo->stamp == keepStamp &&
        memory.unchangedSince(first, last, memo->sequence) && !memory.patchPendingRange(first, last)) {
      ++m_resourceRangeMemoHits;
      return true;
    }
  }
  const auto takeKeptPatch = [&](uint32_t page) {
    if (!memory.patchPending(page)) return;
    memres::ResourceMemory::Patch patch;
    if (memory.takePatch(page, patch)) patches.push_back(patch);
  };
  uint32_t* const epochOf = resEpochOf();  // the frozen-pass A/B: the native arm's own stamps in dual mode
  for (uint32_t page = first; page <= last; ++page) {
    // A render-side bind consumes this frame's captured bytes, even when the
    // guest/CP has already announced writes for a later frame.
    if (!collecting && m_rtOn && epochOf[page] >= m_shmEpochFrameStart) continue;
#if SR_ORACLE
    if (t_shmPrepass && m_shmResolveMask[page]) continue;
#endif
    // A keep decided on the announced generations without the lock is the same
    // answer prepare() gives: a note landing after this read stays pending for
    // the next request, exactly as one landing after prepare()'s unlock. The
    // allocation's window lookup (physwin, ~80% of the CP when asked per page,
    // archive/live/prof1_title_native) is only needed for an upload's source.
    if (!observeKeeps && !memres::isUpload(memory.decide(page))) {
      epochOf[page] = keepStamp;
      takeKeptPatch(page);
      continue;
    }
    const uint32_t address = page << 12;
    const int owner = sr::kernel::physwin::gpuWindow(address, 4096u);
    const auto ticket = memory.prepare(page, owner);
    if (!memres::isUpload(ticket.decision)) {
#if SR_ORACLE
      if (memres::resourceCheckEnabled()) {
        const uint32_t va = (ticket.window == 0xAu ? 0xA0000000u : 0x80000000u) + address;
        const uint8_t* source = sr::kernel::guestVAToHost(va);
        if (!source || !sr::kernel::isGuestVAInRange(va + 4095u))
          throw std::runtime_error("native resource checker found unmapped kept bytes");
        const size_t patchesBefore = patches.size();
        takeKeptPatch(page);
        if (patches.size() != patchesBefore) {
          // The announced dwords moved; the whole-page fingerprint is re-taken.
          memres::resourceCheckPatched(page, ticket.cpu, source);
        } else if (memres::resourceConsumerScopeActive()) {
          auto before = resourceConsumerWitness(page);
          before.known = before.known && before.cpu == ticket.cpu && before.window == ticket.window;
          std::array<uint8_t, 4096> frozen;
          std::memcpy(frozen.data(), source, frozen.size());
          const bool stable = std::memcmp(frozen.data(), source, frozen.size()) == 0;
          const auto after = resourceConsumerWitness(page);
          if (!memres::resourceConsumerKept(frozen.data(), before, after, stable))
            throw std::runtime_error("native consumer checker could not admit kept bytes");
        } else if (!memres::resourceCheckKept(page, ticket.cpu, source)) {
          throw std::runtime_error("native resource checker found stale kept bytes");
        }
      }
#endif
      epochOf[page] = m_rtOn && !collecting ? m_shmEpochFrameStart : m_shmEpoch;
      takeKeptPatch(page);
      continue;
    }
    const uint32_t va = (ticket.window == 0xAu ? 0xA0000000u : 0x80000000u) + address;
    const uint8_t* source = sr::kernel::guestVAToHost(va);
    if (!source || !sr::kernel::isGuestVAInRange(va + 4095u))
      throw std::runtime_error("native resource upload has no readable allocation");
    {
      // Taken before the page's bytes are copied: the copy holds every dword
      // patched until now through its own window; dwords patched through the
      // other window are copied after the page. A later patch stays pending.
      memres::ResourceMemory::Patch patch;
      if (memory.takePatch(page, patch)) {
        patch.dwords[ticket.window == 0xAu ? 1u : 0u] = {};
        if (patch.any()) patches.push_back(patch);
      }
    }
    pages.push_back({ticket, source});
#if SR_ORACLE
    if (memres::resourceConsumerEnabled()) {
      pages.back().before = resourceConsumerWitness(page);
      pages.back().before.known = pages.back().before.known &&
          pages.back().before.cpu == ticket.cpu && pages.back().before.window == ticket.window;
    }
#endif
  }
  const uint32_t third = deferred ? m_shmDrainThird : m_shmRenderThird;
  uint64_t& used = m_shmUploadUsedT[third];
  const uint64_t end = uint64_t(third + 1u) * kShmUploadThird;
  for (size_t begin = 0; begin < pages.size();) {
    size_t stop = begin + 1;
    while (stop < pages.size() && pages[stop].ticket.page == pages[stop - 1].ticket.page + 1u) ++stop;
    const uint32_t page = pages[begin].ticket.page, count = uint32_t(stop - begin);
    const uint64_t bytes = uint64_t(count) << 12;
    if (!deferred && used + bytes > end) pm4Flush();
    uint64_t sourceOffset = 0;
    ID3D12Resource* sourceResource = nullptr;
    uint8_t* destination;
    if (used + bytes > end) {
      destination = shmSpillAlloc(third, bytes, sourceOffset, sourceResource);
      if (!destination) throw std::runtime_error("native resource upload arena exhausted");
    } else {
      sourceOffset = used;
      destination = m_shmUploadPtr + used;
      used += bytes;
      if (deferred) m_shmArenaHighT[third] = used;
    }
#if SR_ORACLE
    std::vector<std::shared_ptr<const memres::ConsumerBaseline>> consumerCopies;
    if (memres::resourceConsumerEnabled()) consumerCopies.reserve(count);
#endif
    for (size_t i = begin; i < stop; ++i) {
      uint8_t* copied = destination + ((i - begin) << 12);
      std::memcpy(copied, pages[i].bytes, 4096u);
#if SR_ORACLE
      resourceAbNativeCopied(pages[i].ticket.page, pages[i].ticket.window, copied);  // INV-M (dual mode only)
      auto witness = pages[i].before;
      if (memres::resourceConsumerEnabled()) {
        const auto after = resourceConsumerWitness(pages[i].ticket.page);
        witness.known = witness.known && after.known && witness.cpu == after.cpu &&
            witness.window == after.window && witness.ownerSum == after.ownerSum &&
            witness.ownerSeq == after.ownerSeq && witness.grantGeneration == after.grantGeneration &&
            witness.grantDigest == after.grantDigest && witness.grantCount == after.grantCount &&
            std::equal(witness.grantSerial, witness.grantSerial + 2, after.grantSerial) &&
            std::memcmp(copied, pages[i].bytes, 4096u) == 0;
      }
      auto baseline = memres::resourceCheckCaptured(pages[i].ticket.page, pages[i].ticket.cpu,
          copied, memres::resourceConsumerEnabled() ? &witness : nullptr);
      if (memres::resourceConsumerEnabled()) consumerCopies.push_back(std::move(baseline));
#endif
    }
    if (deferred) {
      m_shmPending.push_back(ShmPend{uint64_t(page) << 12, sourceOffset, bytes, false, sourceResource});
#if SR_ORACLE
      m_shmPending.back().consumerCopies = std::move(consumerCopies);
#endif
    } else {
      if (m_srReady) srClearPages(page, count);
      pm4ShmState(D3D12_RESOURCE_STATE_COPY_DEST);
      m_commandList->CopyBufferRegion(m_shm.Get(), uint64_t(page) << 12,
                                     sourceResource ? sourceResource : m_shmUpload.Get(), sourceOffset, bytes);
      ++m_shmStampClock;
      for (uint32_t i = 0; i < count; ++i) {
        m_shmStamp[page + i] = m_shmStampClock;
        m_shmGpuWritten[page + i] = 0;
      }
      invalidateGuestRange(page << 12, bytes);
#if SR_ORACLE
      if (memres::resourceConsumerEnabled()) {
        if (m_resourceRecordedBaselines.empty()) m_resourceRecordedBaselines.resize(memres::kPages);
        for (uint32_t i = 0; i < count; ++i)
          m_resourceRecordedBaselines[page + i] = std::move(consumerCopies[i]);
      }
#endif
    }
    // Only copied, admitted bytes commit. A later CPU note stays pending.
    for (size_t i = begin; i < stop; ++i) {
      memory.commit(pages[i].ticket);
      epochOf[pages[i].ticket.page] = m_rtOn && !collecting ? m_shmEpochFrameStart : m_shmEpoch;
    }
    m_shmUploadBytes += bytes;
    begin = stop;
  }
  // Exact CPU-write patches: only the noted dwords, read now from the writer's
  // window, copied after the page's own copy in command order. The page's other
  // bytes - GPU-written ones included - are left as the arena holds them.
  for (const auto& patch : patches)
  for (uint32_t set = 0; set < 2u; ++set) {
    const auto& mask = patch.dwords[set];
    const uint32_t windowBase = memres::ResourceMemory::Patch::window(set) == 0xAu ? 0xA0000000u : 0x80000000u;
    for (uint32_t d = 0; d < 1024u;) {
      if (!(mask[d >> 6] >> (d & 63u) & 1u)) { ++d; continue; }
      uint32_t e = d + 1u;
      while (e < 1024u && (mask[e >> 6] >> (e & 63u) & 1u)) ++e;
      const uint32_t at = (patch.page << 12) + (d << 2);
      const uint64_t bytes = uint64_t(e - d) << 2;
      const uint8_t* source = sr::kernel::guestVAToHost(windowBase + at);
      if (!source || !sr::kernel::isGuestVAInRange(windowBase + at + uint32_t(bytes) - 1u))
        throw std::runtime_error("native resource patch has no readable allocation");
      if (!deferred && used + bytes > end) pm4Flush();
      uint64_t sourceOffset = 0;
      ID3D12Resource* sourceResource = nullptr;
      uint8_t* destination;
      if (used + bytes > end) {
        destination = shmSpillAlloc(third, bytes, sourceOffset, sourceResource);
        if (!destination) throw std::runtime_error("native resource upload arena exhausted");
      } else {
        sourceOffset = used;
        destination = m_shmUploadPtr + used;
        used += bytes;
        if (deferred) m_shmArenaHighT[third] = used;
      }
      std::memcpy(destination, source, size_t(bytes));
      if (deferred) {
        m_shmPending.push_back(ShmPend{at, sourceOffset, bytes, false, sourceResource});
        m_shmPending.back().patch = true;
      } else {
        resourcePatchScaled(patch.page);
        pm4ShmState(D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->CopyBufferRegion(m_shm.Get(), at, sourceResource ? sourceResource : m_shmUpload.Get(),
                                       sourceOffset, bytes);
        m_shmStamp[patch.page] = ++m_shmStampClock;
        invalidateGuestRange(at, bytes);
      }
      m_shmUploadBytes += bytes;
      ++m_resourcePatchCopies;
      d = e;
    }
  }
  if (memo) *memo = ResourceRangeMemo{phys, len, keepStamp, sequenceBefore};
  return true;
}

#if SR_ORACLE
// mem.resources frozen-pass A/B (gfx/resource_ab.hpp), dual mode only. INV-M: the native route just copied `bytes` from
// `window` into page `page` of the arena. The page model's stored state for the page describes what it believes the
// arena holds; when it describes other bytes, the model forgets the page (its next validation uploads). A native
// upload of exactly the bytes the model last validated from the same window changes nothing the model relies on.
namespace {
constexpr uint64_t kAbForgotHash = 0xF0CA11ED5EED0001ull;  // no 4 KiB page hashes to it (2^-64), so it never matches
}
void D3D12Backend::resourceAbNativeCopied(uint32_t page, uint32_t window, const uint8_t* bytes) {
  if (!memres::resourceAbDual() || page >= m_shmHash8.size()) return;
  memres::resourceAbCount(memres::AbEvent::kNativeUploadPages);
  const uint64_t h = ShaderCache::hashUcode(bytes, 4096);
  // The 0x8 copy all zero: the model's legacy branch served the 0xA copy there (its 0x8 hash proves nothing).
  const bool same = window == 0xAu ? (m_shmSrcA[page] != 0 && m_shmHashA[page] == h)
                                   : (m_shmSrcA[page] == 0 && m_shmHash8[page] == h && h != zeroPageHash());
  if (same) {
    memres::resourceAbCount(memres::AbEvent::kModelAdopted);
    return;
  }
  m_shmHash8[page] = m_shmHashA[page] = kAbForgotHash;
  m_shmFresh8[page] = m_shmFreshA[page] = 0u;
  if (m_abForgot.empty()) m_abForgot.assign(memres::kPages, 0u);
  if (m_abForgotHash.empty()) m_abForgotHash.assign(memres::kPages, 0u);
  m_abForgot[page] = 1u;  // the model's next upload of the page re-establishes it (with these bytes: no repair)
  m_abForgotHash[page] = h;
  // The write watch: release the page's block (both windows) so neither its clean keep nor the range cache keeps it.
  { sr::kernel::gww::HostWriteScope release8(0x80000000u + (page << 12), 4096u); }
  { sr::kernel::gww::HostWriteScope releaseA(0xA0000000u + (page << 12), 4096u); }
  memres::resourceAbCount(memres::AbEvent::kModelForgot);
}

// INV-R: the page model just queued an upload of page `page` from the 0xA copy (`fromA`) or the 0x8 copy (`src`: the
// guest bytes it copies). The registry's record of what the arena holds must not outlive it when the model put another
// window's bytes there.
void D3D12Backend::resourceAbModelCopied(uint32_t page, bool fromA, const uint8_t* src) {
  if (!memres::resourceAbDual() || page >= memres::kPages) return;
  memres::resourceAbCount(memres::AbEvent::kModelUploadPages);
  // Astra review1 P1-2: the model's own clobber rule fired for this upload - the page's GPU write had EXECUTED (the
  // flag is set at execution, in both arms). A write only announced at its drain has not.
  const bool executed = page < m_abClob.size() && m_abClob[page];
  if (executed) m_abClob[page] = 0u;
  // A page INV-M forgot, re-established. Only the very bytes the native route captured rebuild the model's metadata
  // and nothing else; other bytes repair the arena like any other upload (Astra review1 P2-5).
  bool reestablish = page < m_abForgot.size() && m_abForgot[page];
  if (reestablish) {
    m_abForgot[page] = 0u;
    reestablish = src && page < m_abForgotHash.size() && ShaderCache::hashUcode(src, 4096) == m_abForgotHash[page];
  }
  auto& memory = memres::resourceMemory();
  const uint32_t cpu = memory.cpuGeneration(page), gpu = memory.gpuGeneration(page);
  if (gpu != 0u && gpu >= cpu) {  // the registry holds the page as GPU data
    // executed: the model replaced the GPU's bytes with CPU bytes no note announced (unrestorable); pending: the
    // model preserving the CPU bytes around a partial resolve before it runs (the native arm's preparation does too)
    memres::resourceAbCount(executed ? memres::AbEvent::kCrossClobber : memres::AbEvent::kModelPreserve);
    return;
  }
  const int owner = sr::kernel::physwin::gpuWindow(page << 12, 4096u);
  if (memory.sourceWindow(page, owner) != (fromA ? 0xAu : 8u)) {
    memory.forgetUpload(page);
    memres::resourceAbCount(memres::AbEvent::kCrossWindow);
    return;
  }
  if (reestablish) {
    memres::resourceAbCount(memres::AbEvent::kModelReestablished);
    return;
  }
  // No pending note: an upper bound of the unannounced-write repairs (a model's first validation of a page the
  // registry already uploaded counts here too).
  if (memory.uploadedGeneration(page) != 0u && memory.uploadedGeneration(page) >= cpu)
    memres::resourceAbCount(memres::AbEvent::kCrossUnannounced);
}
#endif

#if SR_ORACLE
// mem.resources step 2a (SR_MEM_RESOURCES=census): a walk that skipped [p0, p1] as already disposed of - the range memo
// (`claim` = the drain epoch it claims for every page; `memo`: the range cache's keep of that epoch counts too) or the
// epoch pre-check (`claim` = the epoch every page carried at the check), or the lazy window (claim 0: each page's own
// stamp; a page neither stamped in time nor masked is checked too, and shows as unseen). The census checks each page's
// recorded stamp; the prepass's masked pages are deferrals.
static void mrSkips(uint32_t p0, uint32_t p1, const uint32_t* epochOf, uint32_t claim, bool memo, uint32_t epoch,
                    uint32_t frameStart, bool collect, const uint8_t* mask) {
  for (uint32_t p = p0; p <= p1; p++) {
    const bool done = claim == 0u && (collect ? epochOf[p] == epoch : epochOf[p] >= frameStart);
    if (!done && mask && mask[p]) memres::skipDeferred(p);
    else if (memo) memres::skipMemo(p, claim);
    else memres::skipReuse(p, claim ? claim : epochOf[p], collect);
  }
}
#endif

#if SR_ORACLE
bool D3D12Backend::pm4ShmRequestT(uint32_t phys, uint32_t len, ShmRangeTicket* ext) {
  if (memres::resourceDecides()) return pm4ResourceRequest(phys, len);
  if (!m_shmReady || !len) return m_shmReady;
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  memres::resourceAbDecision(memres::AbWho::kModel, p1 - p0 + 1u);  // the frozen-pass A/B's witness (dual mode only)
#if SR_ORACLE  // mem.resources step 2a: every page of the request disposed of exactly once (a census hook on each path)
  memres::WalkScope mrReq(p1 - p0 + 1u, false, phys, len);
#endif
  // iter-466 cp round 2 (SR_CP_SHM_LEAN): no steady_clock per request (two clock reads per
  // request were ~2% of the CP, for the [SHM] lookup-ms statistic only, which then reads 0),
  // and the "all pages done this epoch" pre-check reads the epoch stamps without m_shmMu: a
  // stamp equal to this drain's epoch was written by this thread (the render thread stamps
  // frame-start epochs, always older), so a lock-free read can only add a locked re-check.
  static const bool s_lean = cpsimd::gateLogged("SR_CP_SHM_LEAN", true);
  static const bool s_ownMemo = cpsimd::gateLogged("SR_CP_OWN_MEMO", true);
  // The whole request range's owning window ONCE, on this thread (the pages
  // of one fetch or texture share an allocation; the allocators' maps are
  // not for the pool's workers): kernel/phys_window.hpp gpuWindow, and
  // (SR_CB_WINDOW) a page inside a command buffer the CP parsed comes from
  // that buffer's window. 0 = ambiguous: the page decides for itself.
  const auto tLook = s_lean ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now();
  auto addLookupMs = [&] {
    if (!s_lean)
      m_shmFrameMsLookup +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tLook).count();
  };
  // iter-466 cp lane (SR_CP_SHM_LAZYWIN): the range's window is needed only when a page of it
  // is validated. A request whose pages were all validated in this epoch (the same vertex
  // buffer drawn again in one drain, a texture already collected) returns before the physwin
  // lookups below (~2% of the CP). The live trace keeps the full path (it records each page's
  // window even when the page is not re-validated).
  static const bool s_lazyWin = cpsimd::gateLogged("SR_CP_SHM_LAZYWIN", true);
  // The drain's range memo (CP thread, collect mode): a range this drain epoch already
  // requested in full - the same vertex buffer under many draws - returns without walking its
  // pages again. Every page of a collect-mode request ends the request stamped with the epoch.
  struct RangeMemo {
    uint32_t phys, len, epoch;
  };
  static thread_local RangeMemo t_rangeMemo[1024];
  RangeMemo* memo = nullptr;
  if (s_lazyWin && t_shmCollectInto != nullptr && !livetrace::recording()) {
    memo = &t_rangeMemo[((phys * 0x9E3779B1u) ^ (len * 0x85EBCA6Bu)) >> 22];
#if SR_ORACLE  // mem.resources step 2a: the memo's early return below disposes of the whole range (reuses / deferrals)
    if (memres::census() && memo->phys == phys && memo->len == len && memo->epoch == m_shmEpoch && m_shmEpoch)
      mrSkips(p0, p1, m_shmEpochOf.data(), m_shmEpoch, true, m_shmEpoch, m_shmEpochFrameStart, true,
              t_shmPrepass ? m_shmResolveMask.data() : nullptr);
#endif
    if (memo->phys == phys && memo->len == len && memo->epoch == m_shmEpoch && m_shmEpoch) return true;
  }
  struct MemoStore {
    RangeMemo* m;
    uint32_t phys, len;
    const uint32_t& epoch;
    bool armed = false;
    ~MemoStore() {
      if (m && armed) *m = RangeMemo{phys, len, epoch};
    }
  } memoStore{memo, phys, len, m_shmEpoch};
  // SR_NB_SHM_RANGE_CACHE: a drain's (collect mode, not the texture prepass) request of a range proven clean.
  const bool rcOn = t_shmCollectInto != nullptr && (!t_shmPrepass || nbShmRangePrepassOn()) &&
                    !livetrace::recording() && sr::kernel::gww::enabled() && nbShmRangeCacheOn();
  thread_local std::unique_ptr<ShmRangeEnt[]> t_rc;
  ShmRangeEnt* rce = nullptr;
  if (rcOn) {
    if (ext) {
      rce = ext;  // pm4ShmEnsure: the caller's ticket
    } else {
      if (!t_rc) t_rc.reset(new ShmRangeEnt[1u << 14]);
      rce = &t_rc[((phys * 0x9E3779B1u) ^ (len * 0x85EBCA6Bu)) >> 18];
    }
    if (rce->valid && rce->phys == phys && rce->len == len) {
      // The clock before the block stamps: with none of them >= the entry's stamp, none is >= clk's releases
      // either, and the stamp moves to clk (a release in flight at clk carries clk: the next check sees it).
      // (cr rc2_chk: an earlier "> stamp" test with a clock-unchanged shortcut kept ranges whose block a release
      // in flight at the walk was clearing - 998 CHECK mismatches.)
      const uint64_t clk = sr::kernel::gww::releaseClock();
      const bool rel = sr::kernel::gww::blocksReleasedSince(p0 >> sr::kernel::gww::kBlockLog2,
                                                            p1 >> sr::kernel::gww::kBlockLog2, rce->stamp);
      const sr::kernel::physwin::OwnerVer v =
          rel ? sr::kernel::physwin::OwnerVer{}
              : sr::kernel::physwin::ownerVersion(phys, std::min<uint64_t>(len, kShmBytes - phys));
      if (!rel && v.ok && v.sum == rce->ownSum) {
        rce->stamp = clk;  // none of its blocks released up to now
        g_rcHits.fetch_add(1, std::memory_order_relaxed);
        if (nbShmRangeCheckOn()) {
          std::lock_guard<std::recursive_mutex> lk(m_shmMu);
          for (uint32_t p = p0; p <= p1; p++) {
            const int co = sr::kernel::gww::cleanOwn(p);
            // (a release stamped >= clk is in flight now: the walk would race it the same way - not a cache error)
            if (co != rce->own &&
                !sr::kernel::gww::blocksReleasedSince(p >> sr::kernel::gww::kBlockLog2, p >> sr::kernel::gww::kBlockLog2,
                                                      clk)) {
              const uint64_t k = g_rcMismatch.fetch_add(1, std::memory_order_relaxed) + 1u;
              if (k <= 16u || (k & (k - 1u)) == 0u)
                SR_LOG_BOOT(sr::LogSeverity::Error,
                            "[NB_SHMRC] CHECK MISMATCH #%llu range=%08X+%u page=%08X clean-window=%d cached=%X",
                            (unsigned long long)k, phys, len, p << 12, co, rce->own);
#if SR_ORACLE
              memres::rcMismatch(p);  // mem.resources step 2a: the range cache's keep of this page, unverified
#endif
            } else if (sr::kernel::gww::checkMode()) {
              shmWtCheck(p, rce->own);
#if SR_ORACLE
              memres::rcChecked(p);  // mem.resources step 2a: the re-hash above verified this page's keep
#endif
            }
          }
          g_rcChecked.fetch_add(1, std::memory_order_relaxed);
        }
#if SR_ORACLE  // (the hooks of this unit are preprocessed out of the release configuration: its code stays the model's)
        if (memres::census())  // mem.resources step 2a: the decisions the meter feed below counts, the registry's beside them
          memres::modelKeepRange(p0, p1, t_shmPrepass ? memres::Site::kPrepass : memres::Site::kDrain,
                                 memres::provOfWindow(rce->own), m_shmEpoch);
#endif
        // Every page of the range decided current by the cached proof (across epochs: not a same-epoch dedup hit) -
        // page validations of the meter's shm column (Astra round 1 on release/mem-meter).
        sr::native::meter::add(sr::native::meter::kShm, p1 - p0 + 1u);
        memoStore.armed = true;  // this drain's later requests of the range: the epoch memo
        return true;
      }
      (rel ? g_rcMissRel : g_rcMissOwn).fetch_add(1, std::memory_order_relaxed);
      rce->valid = false;
    }
  }
  // SR_CP_SHM_LEAN: a collect-mode request whose pages all carry this drain's epoch (read
  // without the lock, see above) returns before taking m_shmMu.
  if (s_lean && s_lazyWin && t_shmCollectInto != nullptr && !livetrace::recording() && m_shmEpoch) {
    const uint32_t* eo = m_shmEpochOf.data();
    const uint32_t ep = m_shmEpoch;
    bool all = true;
    for (uint32_t p = p0; p <= p1 && all; p++) all = __atomic_load_n(eo + p, __ATOMIC_RELAXED) == ep;
    if (all) {
#if SR_ORACLE
      if (memres::census()) mrSkips(p0, p1, eo, ep, false, ep, m_shmEpochFrameStart, true, nullptr);  // step 2a: reuses
#endif
      memoStore.armed = true;
      return true;
    }
  }
  if (s_lazyWin && !livetrace::recording()) {
    std::lock_guard<std::recursive_mutex> lk(m_shmMu);
    const bool collectNow = t_shmCollectInto != nullptr;
    bool need = false;
    for (uint32_t p = p0; p <= p1 && !need; p++) {
      const bool done = collectNow ? m_shmEpochOf[p] == m_shmEpoch : m_shmEpochOf[p] >= m_shmEpochFrameStart;
      if (!done && !(t_shmPrepass && m_shmResolveMask[p])) need = true;
    }
    if (!need) {
#if SR_ORACLE
      if (memres::census())  // mem.resources step 2a: each page reused, or deferred by the prepass's mask
        mrSkips(p0, p1, m_shmEpochOf.data(), 0u, false, m_shmEpoch, m_shmEpochFrameStart, collectNow,
                t_shmPrepass ? m_shmResolveMask.data() : nullptr);
#endif
      addLookupMs();
      memoStore.armed = collectNow;
      return true;
    }
  }
  const bool cbOn = sr::kernel::physwin::commandBufferWindowEnabled();
  const bool ownOn = sr::kernel::physwin::gpuOwnerWindowEnabled();
  const uint32_t rangeLen = uint32_t(std::min<uint64_t>(len, kShmBytes - phys));
  // iter-461 FMV regression: the range answer must be one every page of the
  // range agrees with (gpuWindowRange); gpuWindow's per-record containment
  // returned 0xA for the movie planes' contested pages (physwin.cpp note).
  auto ownerOf = [&](uint32_t ph, uint32_t n, bool page) {
    auto uncached = [&] {
      const int cb = cbOn ? sr::kernel::physwin::commandBufferWindow(ph, n) : 0;
      return cb ? cb
                : ownOn ? (page ? sr::kernel::physwin::gpuWindow(ph, n) : sr::kernel::physwin::gpuWindowRange(ph, n))
                        : 0;
    };
    return (s_ownMemo && !livetrace::recording()) ? shmOwnMemo(ph, n, page, uncached) : uncached();
  };
  const int rangeOwn = ownerOf(phys, rangeLen, false);
  // SR_NB_SHM_RANGE_CACHE: the release clock and the owner version BEFORE the walk (a release during it stamps a
  // block later than rcClk; an owner change moves the version).
  bool rcClean = rce != nullptr && rangeOwn != 0;
  const uint64_t rcClk = rcClean ? sr::kernel::gww::releaseClock() : 0u;
  const sr::kernel::physwin::OwnerVer rcVer =
      rcClean ? sr::kernel::physwin::ownerVersion(phys, rangeLen) : sr::kernel::physwin::OwnerVer{};
  if (!rcVer.ok) rcClean = false;
  // iter-465 native lane: the range's window (physwin registries, thread-safe) is looked up
  // above, before m_shmMu is taken.
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  const bool collect = t_shmCollectInto != nullptr;
  std::vector<ShmCand>& cands = collect ? *t_shmCollectInto : m_shmCandsTmp;
  if (!collect) cands.clear();
  const bool wtOn = sr::kernel::gww::enabled() && !livetrace::recording();
  // mem.resources step 2a (SR_MEM_RESOURCES=census): the pages this request decides or makes candidates, tagged with
  // the request's fetch kind and site; the write-watch accepts are decisions here, the candidates' at pm4ShmValidate.
#if SR_ORACLE
  const bool mr = memres::census();
  const memres::Site mrSite = t_shmPrepass ? memres::Site::kPrepass
                              : collect     ? memres::Site::kDrain
                                            : memres::Site::kRender;
#endif
  for (uint32_t p = p0; p <= p1; p++) {
    if (rcClean && sr::kernel::gww::cleanOwn(p) != rangeOwn) rcClean = false;  // SR_NB_SHM_RANGE_CACHE
    // A drain re-validates per drain (the guest may rewrite a buffer between
    // two indirect buffers); the render accepts this frame's validations.
    if (collect ? m_shmEpochOf[p] == m_shmEpoch : m_shmEpochOf[p] >= m_shmEpochFrameStart) {
      // iter-463 (SR_PM4_TRACE_FULL): validated earlier this epoch - possibly
      // before the trace opened, so its bytes never reached the trace.
      if (livetrace::recording() && traceFullEnabled()) shmTracePage(p, rangeOwn);
#if SR_ORACLE
      if (mr) memres::skipReuse(p, m_shmEpochOf[p], collect);  // mem.resources step 2a: the stamp it relies on
#endif
      continue;
    }
#if SR_ORACLE
    if (mr && t_shmPrepass && m_shmResolveMask[p]) memres::skipDeferred(p);  // mem.resources step 2a: deferred below
#endif
    if (t_shmPrepass && m_shmResolveMask[p]) continue;  // resolved this frame: at the bind
    // iter-466 cp lane (SR_SHM_WRITE_TRACK): a page protected before its last validation and
    // not written since (kernel/gpu_write_watch.hpp), whose validation took this request's
    // window, would hash the same and decide the same - it counts as validated now.
    // An ambiguous range (rangeOwn 0) decides per page, as pm4ShmValidate does.
    const int wtOwn = wtOn ? sr::kernel::gww::cleanOwn(p) : -1;
    int pageOwn = rangeOwn;
    if (wtOwn >= 0 && !pageOwn) pageOwn = ownerOf(p << 12, 4096u, true);
    if (wtOwn >= 0 && wtOwn == pageOwn) {
      if (sr::kernel::gww::checkMode()) shmWtCheck(p, pageOwn);
      m_shmEpochOf[p] = (m_rtOn && !collect) ? m_shmEpochFrameStart : m_shmEpoch;
#if SR_ORACLE
      if (mr)  // mem.resources step 2a: the model keeps it (no CPU store since its last validation), stamped
        memres::acceptPage(p, mrSite, m_shmEpochOf[p], collect || !m_rtOn, memres::provOfWindow(pageOwn));
#endif
      m_shmFramePagesClean++;
      sr::native::meter::add(sr::native::meter::kShm);  // a page validation decided by the write watch (meter column shm)
      continue;
    }
    if (collect && m_shmEpochOf[p] >= m_shmEpochFrameStart) m_shmFramePagesRepeat++;
    // iter-465 perf2: under SR_PM4_RENDER_THREAD a render-side validation (scan-out, binds)
    // belongs to the frame being rendered, whose sets the CP has already drained past: stamping
    // it with the CP's live epoch made LATER frames accept it (>= their frame start), so a page
    // only the render side validates - the front buffer the scan-out reads - was validated
    // once and never again (flat brown boot intro). The frame's own first epoch keeps the
    // dedupe inside the frame and expires at the next one. Synchronous path unchanged.
    m_shmEpochOf[p] = (m_rtOn && !collect) ? m_shmEpochFrameStart : m_shmEpoch;
    m_shmFramePagesReq++;
#if SR_ORACLE
    if (mr) memres::candPage(p, mrSite, m_shmEpochOf[p], collect || !m_rtOn);  // step 2a: decided at pm4ShmValidate
#endif
    cands.push_back(ShmCand{p, rangeOwn});
  }
  addLookupMs();
  memoStore.armed = collect;
  if (rcClean) {  // SR_NB_SHM_RANGE_CACHE: every page clean under the range's window - remember the proof
    const sr::kernel::physwin::OwnerVer v2 = sr::kernel::physwin::ownerVersion(phys, rangeLen);
    if (v2.ok && v2.sum == rcVer.sum && v2.cbSeq == rcVer.cbSeq) {
      rce->phys = phys;
      rce->len = len;
      rce->stamp = rcClk;
      rce->ownSum = rcVer.sum;
      rce->own = rangeOwn;
      rce->valid = true;
      g_rcStores.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (collect || cands.empty()) return true;
  pm4ShmValidate(cands, t_shmDefer);
  return true;
}

void D3D12Backend::shmWtCheck(uint32_t p, int own) {
  // The hashes pm4ShmValidate's branch for `own` would compare: 0xA its 0xA copy, 8 its 0x8
  // copy, 0 (legacy) the 0x8 copy and, when that is all zero, the 0xA copy.
  const uint8_t* h8 = sr::kernel::guestVAToHost(0x80000000u + (p << 12));
  const uint8_t* hA = sr::kernel::guestVAToHost(0xA0000000u + (p << 12));
  if (!h8 || !hA) return;
  bool stale;
  if (own == 0xA) {
    stale = ShaderCache::hashUcode(hA, 4096) != m_shmHashA[p];
  } else {
    const uint64_t k8 = ShaderCache::hashUcode(h8, 4096);
    stale = k8 != m_shmHash8[p];
    if (!stale && own == 0 && k8 == zeroPageHash()) stale = ShaderCache::hashUcode(hA, 4096) != m_shmHashA[p];
  }
  sr::kernel::gww::noteCheck(stale);
#if SR_ORACLE
  memres::wtChecked(p, stale);  // mem.resources step 2a: the keep this re-hash verified (acceptPage / rcChecked)
#endif
  if (stale) {
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHM_WT] STALE #%u page=%08X own=%X: written while marked clean",
                  n, p << 12, own);
  }
}

void D3D12Backend::pm4ShmCollectFlush() {
  if (!m_shmReady || m_shmCollect.empty()) return;
#if SR_ORACLE
  // Collected draws have already left their admission scopes. This is a fresh
  // validation-batch decision, not evidence naming the draw that caused a miss.
  // Declare before every lock so origin finalization runs after those release.
  const memres::FetchAdmissionScope fetchValidation(0);
#endif
  // iter-465 native lane (CP costs; hfr line profile: the CP blocked ~16% on m_shmMu while the
  // render thread hashed under it, and vice versa): the drain's collected pages were stamped at
  // request time and only this flush validates them, so their hashes run unlocked; the
  // decisions / uploads / pending copies stay under m_shmMu. SR_SHM_FLUSH_UNLOCKED=0 = locked.
  static const bool s_unlocked = [] {
    const char* e = std::getenv("SR_SHM_FLUSH_UNLOCKED");
    return !e || e[0] != '0';
  }();
  if (!s_unlocked) {
    std::lock_guard<std::recursive_mutex> lk(m_shmMu);
    pm4ShmValidate(m_shmCollect, /*deferred=*/true);
    m_shmCollect.clear();
    return;
  }
  static thread_local std::vector<ShmCand> t_flush;
  {
    std::lock_guard<std::recursive_mutex> lk(m_shmMu);
    t_flush.swap(m_shmCollect);
    m_shmCollect.clear();
  }
  pm4ShmValidate(t_flush, /*deferred=*/true, /*hashUnlocked=*/true);
  t_flush.clear();
}

// Validate the candidate pages: host pointers + the 4 KB hashes in parallel,
// then the (ordered) decisions, upload runs and copies.
void D3D12Backend::pm4ShmValidate(std::vector<ShmCand>& cands, bool deferred, bool hashUnlocked) {
  std::unique_lock<std::recursive_mutex> lk(m_shmMu, std::defer_lock);
  if (!hashUnlocked) lk.lock();
#if SR_ORACLE  // mem.resources step 2a (SR_MEM_RESOURCES=census): each candidate decided or failed exactly once
  const bool mr = memres::census();
  memres::WalkScope mrVal(uint32_t(cands.size()), true);
#endif
  const uint64_t zh = zeroPageHash();
  const bool gpuAuth = shmGpuAuthEnabled();
  // Runs of pages to upload: (first page, count, source window per page).
  struct Run { uint32_t page, count; };
  std::vector<Run> runs;
  std::vector<const uint8_t*> srcs;
  // iter-460: a CPU-copy upload over a page a resolve wrote replaces GPU data
  // (the noise-square chain). Counted; the first ones and a sample are logged.
  auto clobber = [&](uint32_t p, const char* rule, int ownWin, bool zero8) {
    if (!m_shmGpuWritten[p]) return;
#if SR_ORACLE
    if (memres::resourceAbDual()) {  // the frozen-pass A/B's INV-R: this upload replaces a GPU write that EXECUTED
      if (m_abClob.size() != m_shmGpuWritten.size()) m_abClob.assign(m_shmGpuWritten.size(), 0u);
      m_abClob[p] = 1u;
    }
#endif
    const uint64_t n = ++m_shmClobbers;
    if (n <= 48 || n % 16 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_SHM_CLOBBER] #%llu ep=%u page=%08X rule=%s owner=%d zero8=%d",
                  (unsigned long long)n, m_shmEpoch, p << 12, rule, ownWin, zero8 ? 1 : 0);
  };
  struct Ev {
    const uint8_t* h8;
    const uint8_t* hA;
    uint64_t k8, kA;
    bool ok, hasA, kAOk;
    uint32_t gen0;  // gww::protGen before the hashes (SR_SHM_WT_REBASE freshness)
  };
  static thread_local std::vector<Ev> s_ev;
  std::vector<Ev>& ev = s_ev;  // (a lambda cannot capture a thread_local by name)
  ev.assign(cands.size(), Ev{});
  // Phase A (this thread): the host pointers - the guest-memory mapping is
  // per thread (a pool worker resolves to nothing in the replay process).
  const auto tLookA = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < cands.size(); i++) {
    const uint32_t p = cands[i].p;
    Ev& e = ev[i];
    size_t avail8 = 0;
    e.h8 = pm4ShmGuestWritable(p << 12, avail8);
    e.ok = e.h8 && avail8 >= 4096;
    if (!e.ok) continue;
    e.hA = sr::kernel::guestVAToHost(0xA0000000u + (p << 12));
    e.hasA = e.hA && sr::kernel::isGuestVAInRange(0xA0000000u + (p << 12) + 4095u);
#if SR_ORACLE
    if (mr) memres::snapPage(p);  // mem.resources step 2a: the registry's generations BEFORE the bytes are hashed
#endif
  }
  // iter-466 cp lane (SR_SHM_WRITE_TRACK): protect before hashing - a write after this point
  // faults and clears the page, so the hash below is the content of every page still clean.
  const bool wtOn = sr::kernel::gww::enabled() && !livetrace::recording();
  if (wtOn) {
    static thread_local std::vector<uint32_t> s_wtPages;
    s_wtPages.clear();
    for (uint32_t i = 0; i < cands.size(); i++)
      if (ev[i].ok) s_wtPages.push_back(cands[i].p);
    sr::kernel::gww::protectForValidation(s_wtPages.data(), uint32_t(s_wtPages.size()));
    for (uint32_t i = 0; i < cands.size(); i++)
      if (ev[i].ok) ev[i].gen0 = sr::kernel::gww::protGen(cands[i].p);
  }
  const auto tHash = std::chrono::steady_clock::now();
  const double msLookupA = std::chrono::duration<double, std::milli>(tHash - tLookA).count();
  // Phase B (parallel): the hashes the range's window needs; an ambiguous
  // page hashes both.
  auto work = [&ev, &cands, zh](uint32_t i) {
    const ShmCand& c = cands[i];
    Ev& e = ev[i];
    if (!e.ok) return;
    if (c.rangeOwn == 0xA && e.hasA) {
      e.kA = ShaderCache::hashUcode(e.hA, 4096);
      e.kAOk = true;
      return;
    }
    e.k8 = ShaderCache::hashUcode(e.h8, 4096);
    // The 0xA window only matters when the 0x8 alias holds nothing.
    if (c.rangeOwn != 8 && e.k8 == zh && e.hasA) {
      e.kA = ShaderCache::hashUcode(e.hA, 4096);
      e.kAOk = true;
    }
  };
  if (cands.size() >= 32) par::parallelFor(0u, uint32_t(cands.size()), work, 8);
  else for (uint32_t i = 0; i < cands.size(); i++) work(i);
  const double msHashB =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tHash).count();
  if (hashUnlocked) lk.lock();  // the decisions below read / write the page state
  m_shmFrameMsLookup += msLookupA;
  m_shmFrameMsHash += msHashB;
  m_shmFramePagesHashed += uint32_t(cands.size());
  sr::native::meter::add(sr::native::meter::kShm, cands.size());  // page validations decided by a hash (meter column shm)
  const bool cbOn = sr::kernel::physwin::commandBufferWindowEnabled();
  const bool ownOn = sr::kernel::physwin::gpuOwnerWindowEnabled();
  // mem.resources step 2a (SR_MEM_RESOURCES=census): each decision below, handed to the registry's shadow as values
  // (upload or keep, why, the window it serves) BEFORE the stored hash is replaced; nothing here reads an answer back.
  for (uint32_t i = 0; i < cands.size(); i++) {
    const ShmCand& c = cands[i];
    Ev& e = ev[i];
    if (!e.ok) {
#if SR_ORACLE
      if (mr) memres::failedPage(c.p);  // mem.resources step 2a: no host pointer - nothing decided or uploaded
#endif
      continue;
    }
    const uint32_t p = c.p;
    const uint8_t* h8 = e.h8;
    const uint8_t* hA = e.hA;
    int ownWin = c.rangeOwn;
    if (!ownWin) {  // the page's own record (this thread)
      const int cbWin = cbOn ? sr::kernel::physwin::commandBufferWindow(p << 12, 4096u) : 0;
      ownWin = cbWin ? cbWin : ownOn ? sr::kernel::physwin::gpuWindow(p << 12, 4096u) : 0;
    }
    if (ownWin == 0xA && e.hasA && !e.kAOk) {
      e.kA = ShaderCache::hashUcode(hA, 4096);
      e.kAOk = true;
    }
    // iter-466 cp lane: the window this validation decides by (only 8 / 0xA pages are ever
    // skipped; the decision below runs under m_shmMu, as do the skip checks).
    // iter-467 native-b: with the generation the hashes were taken under (SR_SHM_WT_GEN_MARK; late marks counted).
    if (wtOn)
      sr::kernel::gww::markValidated(p, uint8_t(ownWin == 0xA && e.hasA ? 0xA : ownWin == 8 ? 8 : 0), e.gen0);
    // The hashes below were taken after the protection began: current while it lasts.
    const uint32_t freshG = (wtOn && e.gen0 && sr::kernel::gww::protGen(p) == e.gen0) ? e.gen0 : 0u;

    // iter-460 live trace: the bytes this page is served from (GPU-written
    // pages are the trace's initial state or an earlier resolve in it).
    // iter-463 review: a GPU-written page the CPU rewrote since is recorded
    // below, where the decision uploads it (the flag is cleared there).
    const bool traceGpuPage = livetrace::recording() && traceFullEnabled() && m_shmGpuWritten[p];
    if (livetrace::recording() && !m_shmGpuWritten[p]) {
      const uint8_t* ts = h8;
      if (ownWin == 0xA && e.hasA) ts = hA;
      else if (ownWin == 0 && e.hasA && e.k8 == zh) ts = hA;
      livetrace::memRead(p << 12, ts, 4096u);
    }
    if (ownWin == 0xA && e.hasA) {
      const uint64_t k = e.kA;
      const bool ch = k != m_shmHashA[p] ||
                      (m_shmSrcA[p] == 0 && !(gpuAuth && m_shmGpuWritten[p]));
#if SR_ORACLE
      if (mr)
        memres::modelPage(p, ch, k != m_shmHashA[p] ? memres::Why::kContent : memres::Why::kWindow, memres::Prov::kA);
#endif
      m_shmHashA[p] = k;
      m_shmFreshA[p] = freshG;
      m_shmSrcA[p] = 1;
      if (!ch) continue;
      clobber(p, "ownerA", ownWin, false);
      m_shmGpuWritten[p] = 0;
      if (!runs.empty() && runs.back().page + runs.back().count == p) runs.back().count++;
      else runs.push_back({p, 1});
      srcs.push_back(hA);
      if (traceGpuPage) livetrace::memRead(p << 12, srcs.back(), 4096u);  // iter-463: CPU rewrote a resolved page
      m_shmOwnerA++;
      continue;
    }
    if (ownWin == 8) {
      const uint64_t k = e.k8;
      const bool ch = k != m_shmHash8[p] ||
                      (m_shmSrcA[p] != 0 && !(gpuAuth && m_shmGpuWritten[p]));
#if SR_ORACLE
      if (mr)
        memres::modelPage(p, ch, k != m_shmHash8[p] ? memres::Why::kContent : memres::Why::kWindow, memres::Prov::k8);
#endif
      m_shmHash8[p] = k;
      m_shmFresh8[p] = freshG;
      m_shmSrcA[p] = 0;
      if (!ch) continue;
      clobber(p, "owner8", ownWin, false);
      m_shmGpuWritten[p] = 0;
      if (!runs.empty() && runs.back().page + runs.back().count == p) runs.back().count++;
      else runs.push_back({p, 1});
      srcs.push_back(h8);
      if (traceGpuPage) livetrace::memRead(p << 12, srcs.back(), 4096u);  // iter-463: CPU rewrote a resolved page
      m_shmOwner8++;
      continue;
    }
    const uint64_t k8 = e.k8;
    uint64_t kA = m_shmHashA[p];
    // The 0xA window only matters when the 0x8 alias holds nothing.
    if (k8 == zh && e.hasA) kA = e.kA;
    const bool ch8 = k8 != m_shmHash8[p], chA = kA != m_shmHashA[p];
#if SR_ORACLE
    if (mr)  // the two skips below, else the upload (from 0xA when the 0x8 copy is all zero and the 0xA one moved)
      memres::modelPage(p, (ch8 || chA) && !(gpuAuth && m_shmGpuWritten[p] && !ch8), memres::Why::kContent,
                        (k8 == zh && chA && hA) ? memres::Prov::kLegacyA : memres::Prov::kLegacy8);
#endif
    m_shmHash8[p] = k8;
    m_shmHashA[p] = kA;
    m_shmFresh8[p] = freshG;
    if (k8 == zh && e.hasA) m_shmFreshA[p] = freshG;
    if (!ch8 && !chA) continue;  // CPU copy unchanged: GPU data (uploaded or resolved) stands
    if (gpuAuth && m_shmGpuWritten[p] && !ch8) continue;  // a foreign 0xA write (see above)
    clobber(p, ch8 ? "legacy8" : "legacyA", ownWin, k8 == zh);
    // iter-460 census: an upload decided by the content rule where BOTH windows
    // hold different non-zero bytes - one physical page, two live contents (the
    // double-booked windows). The rule takes 0x8; whether that is right is
    // exactly what the census exists to question.
    if (k8 != zh && e.hasA) {
      const uint64_t kA2 = ShaderCache::hashUcode(hA, 4096);
      if (kA2 != zh && kA2 != k8) {
        const uint64_t n = ++m_shmConflicts;
        const int gf = sr::kernel::physwin::gpuGrantFlags(p << 12, 4096u);
        if (n <= 24 || (n & (n - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[PM4_SHM_CONFLICT] #%llu ep=%u page=%08X owner=%d took=0x8 "
                      "8:[heap=%d phys=%d] A:[phys=%d]",
                      (unsigned long long)n, m_shmEpoch, p << 12, ownWin,
                      (gf & 1) ? 1 : 0, (gf & 2) ? 1 : 0, (gf & 4) ? 1 : 0);
      }
    }
    m_shmGpuWritten[p] = 0;
    const uint8_t* src = (k8 == zh && chA && hA) ? hA : h8;
    if (!runs.empty() && runs.back().page + runs.back().count == p) {
      runs.back().count++;
    } else {
      runs.push_back({p, 1});
    }
    srcs.push_back(src);
    if (traceGpuPage) livetrace::memRead(p << 12, srcs.back(), 4096u);  // iter-463: CPU rewrote a resolved page
  }
  if (runs.empty()) return;
#if SR_ORACLE
  if (memres::resourceAbDual()) {  // the frozen-pass A/B's INV-R: each page this validation uploads, by its window
    size_t at = 0;
    for (const Run& r : runs)
      for (uint32_t i = 0; i < r.count; i++, at++)
        resourceAbModelCopied(r.page + i,
                              srcs[at] == sr::kernel::guestVAToHost(0xA0000000u + ((r.page + i) << 12)), srcs[at]);
  }
#endif
  if (!deferred) pm4ShmState(D3D12_RESOURCE_STATE_COPY_DEST);
  // The drain stages into its third, the render into its own.
  const uint32_t third = deferred ? m_shmDrainThird : m_shmRenderThird;
  uint64_t& used = m_shmUploadUsedT[third];
  const uint64_t thirdEnd = uint64_t(third + 1u) * kShmUploadThird;
  size_t si = 0;
  for (const Run& r : runs) {
    const uint64_t bytes = uint64_t(r.count) << 12;
    // iter-464 resscale: the CPU rewrote these pages - their truth is guest RAM
    // again, not a scaled resolve (Xenia ScaledResolveGlobalWatchCallback,
    // texture_cache.cc:1052-1098).
    if (m_srReady) srClearPages(r.page, r.count);
    // The render's third filled: the GPU has finished with everything recorded
    // so far, the cursor rewinds past the copies still queued (pm4ShmAfterFlush).
    if (!deferred && used + bytes > thirdEnd) {
      pm4Flush();
      pm4ShmState(D3D12_RESOURCE_STATE_COPY_DEST);
    }
    uint8_t* dst = nullptr;
    uint64_t srcOff = 0;
    ID3D12Resource* srcRes = nullptr;  // nullptr = the arena
    if (used + bytes > thirdEnd) {
      // iter-461: no room in the third - a spill chunk of this third. At drain
      // time the frame list belongs to the render thread (no flush); the chunk
      // is released when the third is reused, after its frame's fence.
      dst = shmSpillAlloc(third, bytes, srcOff, srcRes);
      if (!dst) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[PM4_SHM] spill of %llu bytes failed: %u pages at %08X stay stale",
                    (unsigned long long)bytes, r.count, r.page << 12);
        si += r.count;
        continue;
      }
    } else {
      dst = m_shmUploadPtr + used;
      srcOff = used;
      used += bytes;
      if (deferred) m_shmArenaHighT[third] = used;
    }
    for (uint32_t i = 0; i < r.count; i++) std::memcpy(dst + (size_t(i) << 12), srcs[si + i], 4096);
    if (deferred)
      m_shmPending.push_back(ShmPend{uint64_t(r.page) << 12, srcOff, bytes, false, srcRes});
    else
      m_commandList->CopyBufferRegion(m_shm.Get(), uint64_t(r.page) << 12,
                                      srcRes ? srcRes : m_shmUpload.Get(), srcOff, bytes);
    if (!deferred) {  // a queued copy stamps its pages when it is recorded
      ++m_shmStampClock;
      for (uint32_t i = 0; i < r.count; i++) m_shmStamp[r.page + i] = m_shmStampClock;
    }
    m_shmUploadBytes += bytes;
    si += r.count;
  }
}

// iter-460 live trace: every GPU-written page as the GPU holds it, read back
// into buffers (guest memory untouched). Needs an open command list.
void D3D12Backend::pm4ShmTraceGpuPages(std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& out) {
  if (!m_shmReady) return;
  const uint32_t maxPages = uint32_t(kShmReadbackBytes >> 12);
  for (uint32_t p = 0; p < kShmPages;) {
    if (!m_shmGpuWritten[p]) {
      p++;
      continue;
    }
    uint32_t n = 1;
    while (p + n < kShmPages && n < maxPages && m_shmGpuWritten[p + n]) n++;
    const uint32_t bytes = n << 12;
    pm4ShmState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->CopyBufferRegion(m_shmReadback.Get(), 0, m_shm.Get(), uint64_t(p) << 12, bytes);
    pm4Flush();
    uint8_t* mapped = nullptr;
    const D3D12_RANGE rr{0, SIZE_T(bytes)};
    if (SUCCEEDED(m_shmReadback->Map(0, &rr, reinterpret_cast<void**>(&mapped))) && mapped) {
      out.emplace_back(p << 12, std::vector<uint8_t>(mapped, mapped + bytes));
      const D3D12_RANGE wr{0, 0};
      m_shmReadback->Unmap(0, &wr);
    }
    p += n;
  }
}

#endif
void D3D12Backend::pm4ShmRangeWrittenByGpu(uint32_t phys, uint32_t len) {
  if (!m_shmReady || !len) return;
  if (memres::resourceMode()) {
    std::lock_guard<std::recursive_mutex> lock(m_shmMu);
    phys &= 0x1FFFFFFFu;
    auto& memory = memres::resourceMemory();
    const uint32_t generation = m_resourceGpuGeneration;
    if (!generation) throw std::logic_error("native GPU write has no producer generation");
    memory.noteGpuAt(phys, len, generation);
#if SR_ORACLE
    sr::kernel::reader_observe::gpuWrite(m_readerRecording, phys, len, generation);
#endif
    uint32_t first, last;
    if (!memres::Registry::pageRange(phys, len, first, last)) return;
    ++m_shmStampClock;
    for (uint32_t page = first; page <= last; ++page) {
      m_shmGpuWritten[page] = 1;
      m_shmStamp[page] = m_shmStampClock;
    }
    invalidateGuestRange(phys, len);
    // The frozen-pass A/B (gfx/resource_ab.hpp): the page model rebaselines too, so its next present keeps the GPU data.
    if (!memres::resourceAbDual()) return;
  }
#if SR_ORACLE
  if (memres::census()) memres::noteGpuWrite(phys, len);  // mem.resources step 2a: the registry's noteGpuWrite
#endif
#if SR_ORACLE
  // iter-466 hfr (SR_SHM_REBASE_UNLOCKED, default ON; =0 the old order): the rebaseline
  // hashes (~200 pages x 2 windows per shadow resolve, XXH3 in the par pool) used to run
  // UNDER m_shmMu on the render thread, and the CP - the frame's wall - sat blocked on that
  // mutex ~16% of its time in pm4ShmRequest / drainDrawQueue (archive/v1_hfr/b2_unc). The
  // hashes only read guest memory, so they are taken first, without the lock, and the
  // gpu-written flags, stamps and baselines are committed together under it - the same
  // state a validate-then-rebaseline order leaves. Guest CPU writes racing the hash were
  // never ordered by this lock (guest threads do not take it).
  if (pm4ShmRebaseUnlockedEnabled()) {
    pm4ShmRangeWrittenByGpuUnlocked(phys, len);
    return;
  }
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  ++m_shmStampClock;
  const bool gpuAuth = shmGpuAuthEnabled();
  const uint32_t stamp = m_shmStampClock;
  // Re-baseline the CPU copies: only a CPU write AFTER this resolve counts.
  // iter-460 perf: two 4 KB hashes per page, in parallel (each page writes
  // only its own records); ~200 pages per shadow-map resolve.
  struct Rb { const uint8_t* h8; const uint8_t* hA; };
  thread_local std::vector<Rb> ptrs;
  ptrs.assign(size_t(p1 - p0 + 1u), Rb{nullptr, nullptr});
  for (uint32_t p = p0; p <= p1; p++) {  // lookups on this thread
    m_shmGpuWritten[p] = 1;
    m_shmStamp[p] = stamp;
    if (!gpuAuth) continue;
    size_t avail8 = 0;
    const uint8_t* h8 = pm4ShmGuestWritable(p << 12, avail8);
    if (h8 && avail8 >= 4096) ptrs[p - p0].h8 = h8;
    const uint32_t va = 0xA0000000u + (p << 12);
    const uint8_t* hA = sr::kernel::guestVAToHost(va);
    if (hA && sr::kernel::isGuestVAInRange(va + 4095u)) ptrs[p - p0].hA = hA;
  }
  if (!gpuAuth) return;
  std::vector<Rb>& pv = ptrs;  // thread_local: bind the reference before the lambda
  auto rebase = [&](uint32_t p) {  // hashes in parallel (pv: a plain reference, capturable)
    const Rb& r = pv[p - p0];
    sr::native::meter::add(sr::native::meter::kShm);  // a re-baseline decision, by a hash (meter column shm)
    if (r.h8) m_shmHash8[p] = ShaderCache::hashUcode(r.h8, 4096);
    if (r.hA) m_shmHashA[p] = ShaderCache::hashUcode(r.hA, 4096);
    m_shmFresh8[p] = m_shmFreshA[p] = 0u;  // (parallel per page: own records only)
  };
  if (p1 - p0 + 1u >= 64u) par::parallelFor(p0, p1 + 1u, rebase, 16);
  else for (uint32_t p = p0; p <= p1; p++) rebase(p);
#endif
}

#if SR_ORACLE
// iter-466 hfr: pm4ShmRangeWrittenByGpu with the hashing outside m_shmMu (see there).
// pm4ShmResolve holds m_shmMu across its whole body (request, dispatch, rebaseline), so it
// pre-hashes the extent BEFORE taking the lock (pm4ShmRebaseHash); the rebaseline then only
// commits. Any other caller hashes here (outside the lock unless its caller holds it).
namespace {
struct RebaseHash {
  uint64_t k8, kA;
  bool has8, hasA;
  bool fresh;     // iter-466 cp lane: both stored hashes current (SR_SHM_WT_REBASE) - not hashed
  uint32_t gen0;  // gww::protGen before hashing (0 = not protected)
};
// SR_SHM_WT_REBASE: the stored hashes a rebaseline may keep (both current under the page's
// protection generation), read unlocked here and re-checked under m_shmMu at the commit.
struct RebaseFresh {
  const uint32_t* f8 = nullptr;
  const uint32_t* fA = nullptr;
};
thread_local RebaseFresh t_rebaseFresh;
struct RebasePre {
  uint32_t phys = 0, len = 0;
  bool valid = false;
  std::vector<RebaseHash> hs;
};
thread_local RebasePre t_rebasePre;

// iter-467 native-b B-CPU (SR_NB_REBASE_PROTECT, baked ON after cr2; =0 off): a resolve's rebaseline hashes its destination's
// guest pages (2 x 4 KB per page, every resolve, every frame: strand A's pr_off profile - rebaseHashPages 0.81 of
// the render thread's 8.19 ms) because nothing proves the CPU left them alone: GPU-written pages are never
// validated, so write tracking never protects them and SR_SHM_WT_REBASE's keep finds no protection generation.
// With the gate the rebaseline protects them first (gww::protectForValidation, as the validation does before its
// hashes): the hash is then the content for as long as the protection generation holds, and the next resolve into
// the page keeps it instead of hashing again. A CPU write faults and clears the page (one re-hash).
// SR_NB_NATIVE_PREHASH (iter-467 native-b, baked ON; =0 off): the native resolves (path N, and the native targets)
// hash their extent's rebaseline before taking m_shmMu (pm4ShmResolve's order), not under it. Job16b (in-roll A/B,
// uncapped, 2 rolls each): +7.4 +-2.7 fps at 3x, +4.4 +-2.5 at 1x; replay frames 9/9 at 1x and 3x, 0 stale kept
// hashes under SR_SHM_WT_CHECK=1.
bool nbNativePrehashOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_NATIVE_PREHASH");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON after job16b (=0 A/B)
  }();
  static const int ab = sr::abt::reg("SR_NB_NATIVE_PREHASH");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}

bool nbRebaseProtectOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_REBASE_PROTECT");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  // SR_AB_TOGGLE: state-leaving (the protection outlives an OFF window) - toggle it alone
  static const int ab = sr::abt::reg("SR_NB_REBASE_PROTECT");
  return v && (ab < 0 || sr::abt::on(ab));
}

// SR_NB_REBASE_FRESH_FIRST (iter-467 native-b, baked ON; =0 off; SR_AB_TOGGLE): the kept pages are found serially first (a
// protGen read per page) and only the pages that need a hash go to the pool. The 3x render-thread profile (nb_prof,
// fr14) had rebaseHashPages at 1.01 ms per frame inclusive, 0.98 of it in par::Pool::run, while the same roll's
// [SHM_WT] totals keep ~10,400 pages a frame (every page of the 26 resolves' extents): the pool was woken for
// pages that hash nothing. Same hashes, same generations (gen0 is read before the hash, after the protection).
// Job16b: +14.1 +-3.0 fps at 3x, +12.6 +-2.4 at 1x, process CPU -0.47 / -0.53 s per s; replay 9/9 x 2, 0 stale.
bool nbRebaseFreshFirstOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_REBASE_FRESH_FIRST");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON after job16b (=0 A/B)
  }();
  static const int ab = sr::abt::reg("SR_NB_REBASE_FRESH_FIRST");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}

void rebaseHashPages(uint32_t p0, uint32_t p1, std::vector<RebaseHash>& out) {
  out.assign(size_t(p1 - p0 + 1u), RebaseHash{0, 0, false, false, false, 0u});
  std::vector<RebaseHash>& hv = out;
  const RebaseFresh fr = t_rebaseFresh;  // (the pool's workers read the caller's arrays)
  if (nbRebaseFreshFirstOn()) {
    thread_local std::vector<uint32_t> s_need;
    s_need.clear();
    for (uint32_t p = p0; p <= p1; p++) {
      if (fr.f8) {
        const uint32_t g = sr::kernel::gww::protGen(p);
        if (g && fr.f8[p] == g && fr.fA[p] == g) {
          hv[p - p0].fresh = true;
          continue;
        }
      }
      s_need.push_back(p);
    }
    if (s_need.empty()) return;
    if (fr.f8 && nbRebaseProtectOn() && !livetrace::recording())
      sr::kernel::gww::protectForValidation(s_need.data(), uint32_t(s_need.size()));
    const std::vector<uint32_t>& need = s_need;  // (bind before the lambda: thread_local)
    auto hashNeeded = [&hv, &need, p0, fr](uint32_t i) {
      const uint32_t p = need[i];
      RebaseHash& r = hv[p - p0];
      sr::native::meter::add(sr::native::meter::kShm);  // a re-baseline hash: a page validation (meter column shm)
      if (fr.f8) r.gen0 = sr::kernel::gww::protGen(p);  // after the protection, before the hash
      size_t avail8 = 0;
      const uint8_t* h8 = pm4ShmGuestWritable(p << 12, avail8);
      if (h8 && avail8 >= 4096) {
        r.k8 = ShaderCache::hashUcode(h8, 4096);
        r.has8 = true;
      }
      const uint32_t va = 0xA0000000u + (p << 12);
      const uint8_t* hA = sr::kernel::guestVAToHost(va);
      if (hA && sr::kernel::isGuestVAInRange(va + 4095u)) {
        r.kA = ShaderCache::hashUcode(hA, 4096);
        r.hasA = true;
      }
    };
    const uint32_t n = uint32_t(need.size());
    if (n >= 64u) par::parallelFor(0u, n, hashNeeded, 16);
    else for (uint32_t i = 0; i < n; i++) hashNeeded(i);
    return;
  }
  if (fr.f8 && nbRebaseProtectOn() && !livetrace::recording()) {
    thread_local std::vector<uint32_t> s_pages;
    s_pages.clear();
    for (uint32_t p = p0; p <= p1; p++) {
      const uint32_t g = sr::kernel::gww::protGen(p);
      if (!(g && fr.f8[p] == g && fr.fA[p] == g)) s_pages.push_back(p);  // not kept as it stands: protect
    }
    if (!s_pages.empty()) sr::kernel::gww::protectForValidation(s_pages.data(), uint32_t(s_pages.size()));
  }
  auto hashPage = [&hv, p0, fr](uint32_t p) {  // guest memory reads only: no shm state touched
    RebaseHash& r = hv[p - p0];
    if (fr.f8) {
      const uint32_t g = sr::kernel::gww::protGen(p);
      if (g && fr.f8[p] == g && fr.fA[p] == g) {
        r.fresh = true;
        return;
      }
      r.gen0 = g;
    }
    sr::native::meter::add(sr::native::meter::kShm);  // a re-baseline hash: a page validation (meter column shm)
    size_t avail8 = 0;
    const uint8_t* h8 = pm4ShmGuestWritable(p << 12, avail8);
    if (h8 && avail8 >= 4096) {
      r.k8 = ShaderCache::hashUcode(h8, 4096);
      r.has8 = true;
    }
    const uint32_t va = 0xA0000000u + (p << 12);
    const uint8_t* hA = sr::kernel::guestVAToHost(va);
    if (hA && sr::kernel::isGuestVAInRange(va + 4095u)) {
      r.kA = ShaderCache::hashUcode(hA, 4096);
      r.hasA = true;
    }
  };
  if (p1 - p0 + 1u >= 64u) par::parallelFor(p0, p1 + 1u, hashPage, 16);
  else for (uint32_t p = p0; p <= p1; p++) hashPage(p);
}
}  // namespace

bool D3D12Backend::pm4ShmRebaseUnlockedEnabled() {
  if (memres::resourceMode() && !memres::resourceAbDual()) return false;  // dual: the model's rebaseline stays its own
  static const bool on = [] {
    const char* e = std::getenv("SR_SHM_REBASE_UNLOCKED");
    return !e || e[0] != '0';
  }();
  return on;
}

bool D3D12Backend::shmWtRebaseOn() const {
  static const bool on = sr::kernel::gww::enabled() && cpsimd::gateLogged("SR_SHM_WT_REBASE", true);
  return on;
}

void D3D12Backend::pm4ShmRebaseHash(uint32_t phys, uint32_t len) {
  t_rebasePre.valid = false;
  if (!m_shmReady || !len || !shmGpuAuthEnabled()) return;
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  if (shmWtRebaseOn()) t_rebaseFresh = RebaseFresh{m_shmFresh8.data(), m_shmFreshA.data()};
  rebaseHashPages(p0, p1, t_rebasePre.hs);
  t_rebaseFresh = RebaseFresh{};
  t_rebasePre.phys = phys;
  t_rebasePre.len = len;
  t_rebasePre.valid = true;
}

void D3D12Backend::pm4ShmRebaseDrop() { t_rebasePre.valid = false; }

void D3D12Backend::pm4ShmRangeWrittenByGpuUnlocked(uint32_t phys, uint32_t len) {
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  const bool gpuAuth = shmGpuAuthEnabled();
  thread_local std::vector<RebaseHash> local;
  const std::vector<RebaseHash>* hs = nullptr;
  if (gpuAuth) {
    if (t_rebasePre.valid && t_rebasePre.phys == phys && t_rebasePre.len == len) {
      hs = &t_rebasePre.hs;   // pre-hashed by pm4ShmResolve before it took m_shmMu
    } else {
      if (shmWtRebaseOn()) t_rebaseFresh = RebaseFresh{m_shmFresh8.data(), m_shmFreshA.data()};
      rebaseHashPages(p0, p1, local);
      t_rebaseFresh = RebaseFresh{};
      hs = &local;
    }
  }
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  const uint32_t stamp = ++m_shmStampClock;
  for (uint32_t p = p0; p <= p1; p++) {
    m_shmGpuWritten[p] = 1;
    m_shmStamp[p] = stamp;
    if (!hs) continue;
    const RebaseHash& r = (*hs)[p - p0];
    if (r.fresh) {
      // A page the pre-hash found fresh: decided here - kept under its generation, or hashed now after losing the
      // protection. Either way one re-baseline decision of the meter's shm column (the pre-hash counted it nowhere).
      sr::native::meter::add(sr::native::meter::kShm);
      // Still under the generation both hashes were taken in: they are the content - keep.
      const uint32_t g = sr::kernel::gww::protGen(p);
      if (g && m_shmFresh8[p] == g && m_shmFreshA[p] == g) {
        m_shmWtRebaseKept++;
        if (sr::kernel::gww::checkMode()) {  // SR_SHM_WT_CHECK: the kept hashes, recomputed
          const uint8_t* c8 = sr::kernel::guestVAToHost(0x80000000u + (p << 12));
          const uint8_t* cA = sr::kernel::guestVAToHost(0xA0000000u + (p << 12));
          const bool stale = (c8 && ShaderCache::hashUcode(c8, 4096) != m_shmHash8[p]) ||
                             (cA && ShaderCache::hashUcode(cA, 4096) != m_shmHashA[p]);
          sr::kernel::gww::noteCheck(stale);
          if (stale) {
            static std::atomic<uint32_t> s_rn{0};
            if (s_rn.fetch_add(1, std::memory_order_relaxed) < 16)
              SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHM_WT] STALE rebaseline keep page=%08X", p << 12);
          }
        }
        continue;
      }
      // Lost the protection since the unlocked check: hash now, as the rebaseline always did.
      const uint32_t g0 = g;
      size_t avail8 = 0;
      const uint8_t* h8 = pm4ShmGuestWritable(p << 12, avail8);
      if (h8 && avail8 >= 4096) m_shmHash8[p] = ShaderCache::hashUcode(h8, 4096);
      const uint32_t va = 0xA0000000u + (p << 12);
      const uint8_t* hA = sr::kernel::guestVAToHost(va);
      if (hA && sr::kernel::isGuestVAInRange(va + 4095u)) m_shmHashA[p] = ShaderCache::hashUcode(hA, 4096);
      const uint32_t f = (g0 && sr::kernel::gww::protGen(p) == g0) ? g0 : 0u;
      m_shmFresh8[p] = (h8 && avail8 >= 4096) ? f : 0u;
      m_shmFreshA[p] = hA ? f : 0u;
      continue;
    }
    if (r.has8) m_shmHash8[p] = r.k8;
    if (r.hasA) m_shmHashA[p] = r.kA;
    const uint32_t f = (r.gen0 && sr::kernel::gww::protGen(p) == r.gen0) ? r.gen0 : 0u;
    m_shmFresh8[p] = r.has8 ? f : 0u;
    m_shmFreshA[p] = r.hasA ? f : 0u;
  }
  t_rebasePre.valid = false;
}

#endif
#if SR_ORACLE
// mem.resources textures_from_resolves (gfx/resource_texid.hpp): the pages of [phys, +len) holding resolve output now -
// in the deciding arm's own terms: the registry's (a GPU generation not older than the newest CPU note) when the native
// route decides, the page model's GPU-written / scaled-resolve flags when it does.
uint32_t D3D12Backend::texIdGpuPages(uint32_t phys, uint32_t len, uint32_t* cpuAfter, uint32_t* src) const {
  if (!m_shmReady || !len) return 0;
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  const bool registry = memres::resourceDecides();
  auto& memory = memres::resourceMemory();
  uint32_t n = 0;
  for (uint32_t p = p0; p <= p1; ++p) {
    bool gpu;
    if (registry) {
      const uint32_t g = memory.gpuGeneration(p), c = memory.cpuGeneration(p);
      gpu = g != 0u && g >= c;
      if (cpuAfter && g != 0u && c > g) {  // a resolve destination the CPU wrote since (the orchestrator's rule)
        if (!*cpuAfter && src) *src = uint32_t(memory.lastSrc(p));
        ++*cpuAfter;
      }
    } else {
      gpu = (p < m_shmGpuWritten.size() && m_shmGpuWritten[p]) || (m_srReady && p < m_srPages.size() && m_srPages[p]);
    }
    n += gpu ? 1u : 0u;
  }
  return n;
}

// A 2D clean-mark hit: nothing the texture reads changed since its last full check, so it binds the content that check
// classified (the cache entry's recorded class).
void D3D12Backend::texIdCleanHit(uint64_t key, const uint32_t* f) {
  auto it = m_texRefresh.find(key);
  const memres::TexIdClass cls =
      it != m_texRefresh.end() ? memres::TexIdClass(it->second.texIdClass) : memres::TexIdClass::kUnknown;
  memres::texIdBind(memres::TexIdKind::k2DClean, cls, 0u, 0u, ((f[1] >> 12) & 0xFFFFFu) * 4096u, f[1] & 0x3Fu);
}
#endif

// The newest change stamp over [phys, +len): a texture loaded at stamp S is
// current while this stays <= S.
uint32_t D3D12Backend::pm4ShmVersion(uint32_t phys, uint32_t len) {
  if (!m_shmReady || !len) return 0;
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  uint32_t v = 0;
  for (uint32_t p = p0; p <= p1; p++) v = std::max(v, m_shmStamp[p]);
  return v;
}

bool pm4ShmLoadable(uint32_t fmt) { return loadShaderFor(fmt) >= 0; }
// iter-466 res4k round 2: SR_TEX_LOAD_DIRECT state accessors (gfx/tex_load_direct.inc)
uint32_t pm4TldUavSlot();
ID3D12RootSignature* pm4TldRs();
ID3D12PipelineState* pm4TldPso();
uint64_t pm4TldNoteLoad();
bool pm4ShmScaledLoadable(uint32_t fmt) { return scaledLoadable(fmt); }

// Xenia LoadTextureDataFromResidentMemoryImpl (d3d12_texture_cache.cc:1513-1953)
// for one 2D texture: texture_load_* dispatches from the shared memory into a
// placed-footprint scratch buffer, then CopyTextureRegion per level (a packed
// tail is loaded once and each sublevel copied out with a box). Recorded on
// the frame list in draw order. The texture ends in PIXEL_SHADER_RESOURCE.
bool D3D12Backend::pm4ShmLoadTexture(ID3D12Resource* tex, bool fromSR, const ShmTexDesc& d) {
#if SR_ORACLE
  holdNoteTex('T', d.basePhys, d.fmt, d.width, d.height);  // 2i diagnostic (SR_DIAG_HOLD_LOG)
#endif
  namespace CL = sr::gfx::cube;
  sr::hitch::Scope hitchScope(sr::hitch::kTexLoad);
  const int ls = loadShaderFor(d.fmt);
  if (ls < 0 || !m_shmReady) return false;
  // iter-464 resscale: a resolution-scaled texture (d3d12_texture_cache.cc:1528-1771):
  // the *_scaled loader, the source bound at guest address * area per job
  // (guest_offset 0), size_blocks and the host footprint times the scale, pitch
  // and z stride unscaled.
  const bool scaled = d.scaleX > 1 || d.scaleY > 1;
  const uint32_t tsx = scaled ? d.scaleX : 1u, tsy = scaled ? d.scaleY : 1u;
  if (scaled && (!m_srReady || !scaledLoadable(d.fmt) || d.blockW != 1u)) return false;
  // iter-467 native-b NATIVE METER: a texture decoded back out of resolve output (the scaled resolve memory,
  // or at 1x a base page the GPU wrote).
  {
    const uint32_t bp = (d.basePhys & 0x1FFFFFFFu) >> 12;
    if (fromSR || scaled || (bp < m_shmGpuWritten.size() && m_shmGpuWritten[bp])) m_meterCur.resolvedTexLoads++;
  }
  const LoadShaderInfo& li = scaled ? kLoadShadersScaled[ls] : kLoadShaders[ls];
  const uint32_t bw = d.blockW;  // host block == guest block (BC stays compressed)
  struct Job {
    uint32_t guestOffset, pitchBlocks, wBlocks, hBlocks, heightTexels, fpW, fpH, rowPitch;
    uint64_t hostOff;
    int key;
    uint32_t hBlocksGuest;  // iter-464: unscaled block rows (z stride, scaled extent)
  };
  struct Copy {
    uint32_t sub, job;
    bool box;
    D3D12_BOX b;
  };
  std::vector<Job> jobs;
  std::vector<Copy> copies;
  uint64_t total = 0;
  auto addJob = [&](uint32_t guestOffset, uint32_t pitchBlocks, uint32_t w, uint32_t h,
                    int key) -> uint32_t {
    for (uint32_t i = 0; i < jobs.size(); i++)
      if (key >= 0 && jobs[i].key == key) return i;
    Job j{};
    j.guestOffset = guestOffset;
    j.pitchBlocks = pitchBlocks;
    j.hBlocksGuest = (h + bw - 1u) / bw;
    j.wBlocks = (w + bw - 1u) / bw * tsx;
    j.hBlocks = j.hBlocksGuest * tsy;
    j.heightTexels = h;
    j.fpW = j.wBlocks * bw;
    j.fpH = j.hBlocks * bw;
    const uint32_t xPerThread = 1u << li.xLog2;
    j.rowPitch =
        (((j.wBlocks + xPerThread - 1u) / xPerThread * xPerThread) * li.hostBlockBytes + 255u) &
        ~255u;
    j.hostOff = (total + 511u) & ~uint64_t(511u);
    total = j.hostOff + uint64_t(j.rowPitch) * j.hBlocks;
    j.key = key;
    jobs.push_back(j);
    return uint32_t(jobs.size() - 1u);
  };
  auto pitchOf = [&](uint32_t level, bool isBase) {
    uint32_t pb = 0;
    CL::detail::levelSliceStride(level, isBase, d.width, d.height, d.pitchTexels, bw, bw,
                                 d.blockBytes, d.tiled, &pb);
    if (!d.tiled && !isBase) pb = ((pb * d.blockBytes + 255u) & ~255u) / d.blockBytes;
    return pb;
  };
  if (!d.layout) {
    const uint32_t j = addJob(d.basePhys, pitchOf(0, true), d.width, d.height, -1);
    copies.push_back({0u, j, false, {}});
  } else {
    for (uint32_t m = 0; m < d.levels; m++) {
      const CL::SubResPlacement p =
          CL::placeSubresource(*d.layout, d.width, d.height, bw, bw, d.face, m);
      if (!p.ok) return false;
      uint32_t j;
      bool box = false;
      switch (p.region) {
        case CL::SubResRegion::Base:
          j = addJob(d.basePhys + uint32_t(p.byteOffset), pitchOf(0, true), p.width, p.height, -1);
          break;
        case CL::SubResRegion::Mip:
          j = addJob(d.mipPhys + uint32_t(p.byteOffset), pitchOf(m, false), p.width, p.height, -1);
          break;
        case CL::SubResRegion::BasePackedTail:
          j = addJob(d.basePhys + uint32_t(p.byteOffset), pitchOf(0, true), p.tailWidth,
                     p.tailHeight, 1);
          box = true;
          break;
        default:  // PackedTail
          j = addJob(d.mipPhys + uint32_t(p.byteOffset), pitchOf(d.layout->packedLevel, false),
                     p.tailWidth, p.tailHeight, 2);
          box = true;
          break;
      }
      Copy c{m, j, box, {}};
      if (box) {
        // iter-464 resscale: the tail's host footprint is scaled (bw == 1 there).
        c.b.left = p.xBlocks * bw * tsx;
        c.b.top = p.yBlocks * bw * tsy;
        c.b.front = 0;
        c.b.right = c.b.left + (p.width + bw - 1u) / bw * bw * tsx;
        c.b.bottom = c.b.top + (p.height + bw - 1u) / bw * bw * tsy;
        c.b.back = 1;
      }
      copies.push_back(c);
    }
  }
  if (!total) return false;
  // iter-467 native-b B2: memory a native target holds goes back before the loader reads it.
  if (scaled)
    for (const Job& j : jobs)
      nbMaterialize(j.guestOffset, uint32_t(std::min<uint64_t>(uint64_t((j.pitchBlocks + 31u) & ~31u) * d.blockBytes *
                                                                    ((j.hBlocksGuest + 31u) & ~31u), kShmBytes)));
  // iter-466 res4k round 2 (SR_TEX_LOAD_DIRECT): one scaled level written straight into the texture
  // (created typeless + UAV for this), no scratch buffer and no copy (gfx/tex_load_direct.inc).
  if (scaled && pm4TexLoadDirectOn() && jobs.size() == 1u && copies.size() == 1u && !copies[0].box &&
      d.levels == 1u && (tex->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
    DXGI_FORMAT tlFmt = DXGI_FORMAT_UNKNOWN, uavFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t lb = 0, um = 0;
    const uint32_t wantLb = ls == kL16 ? 1u : ls == kL64 ? 3u : (ls == kL32 || ls == kLDepthUnorm || ls == kLDepthFloat) ? 2u : 0u;
    if (pm4TexDirectFormat(d.dxgi, &tlFmt, &uavFmt, &lb, &um) && lb == wantLb && pm4TexLoadDirectInit()) {
      const Job& j = jobs[0];
      const uint64_t span = uint64_t((j.pitchBlocks + 31u) & ~31u) * d.blockBytes * ((j.hBlocksGuest + 31u) & ~31u);
      if (!srEnsure(j.guestOffset & 0x1FFFFFFFu, uint32_t(std::min<uint64_t>(span, kShmBytes)))) return false;
      srState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      const uint32_t slot = pm4TldUavSlot();
      D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
      ud.Format = uavFmt;
      ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;  // a 2D texture or one array slice
      ud.Texture2DArray.FirstArraySlice = d.subresource;  // levels == 1: subresource == slice
      ud.Texture2DArray.ArraySize = 1;
      m_device->CreateUnorderedAccessView(tex, nullptr, &ud, pm4RtSrvCpu(slot));
      pm4ShmBarrier(m_commandList.Get(), tex,
                    fromSR ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gtMark(GtDraw);
      nbTexLoadBegin(d, true);  // iter-467 native-b census (current until nbEnd below)
      pm4XlatHeaps();  // step 4 lane C 2i: this pass's tables are m_xlatSrvHeap's (ERROR 708 / GBV 935 without)
      m_commandList->SetComputeRootSignature(pm4TldRs());
      m_commandList->SetPipelineState(pm4TldPso());
      m_commandList->SetComputeRootShaderResourceView(
          1, m_srBuf->GetGPUVirtualAddress() + uint64_t(j.guestOffset & 0x1FFFFFFFu) * (uint64_t(tsx) * tsy));
      m_commandList->SetComputeRootDescriptorTable(2, pm4RtSrvGpu(slot));
      const uint32_t c[10] = {uint32_t(d.tiled) | ((d.endian & 3u) << 2) | (tsx << 4) | (tsy << 7), 0u,
                              j.pitchBlocks, (j.hBlocksGuest + 31u) & ~31u, j.wBlocks, j.hBlocks, 1u,
                              uint32_t(j.hostOff), j.rowPitch, j.heightTexels};
      m_commandList->SetComputeRoot32BitConstants(0, 10, c, 0);
      const uint32_t c4[4] = {lb, um, ls == kLDepthUnorm ? 1u : ls == kLDepthFloat ? 2u : 0u, 0u};
      m_commandList->SetComputeRoot32BitConstants(3, 4, c4, 0);
      const uint32_t gxBlocks = 1u << (2u + li.xLog2);
      m_commandList->Dispatch((j.wBlocks + gxBlocks - 1u) / gxBlocks, (j.hBlocks + 31u) / 32u, 1u);
      pm4ShmBarrier(m_commandList.Get(), tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      gtMark(GtTexLoadScaled);
      nbEnd();
      m_shmTexLoads++;
      m_srTexLoads++;
      const uint64_t nld = pm4TldNoteLoad();
      if (nld <= 8u || (nld % 4096u) == 0u)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[TEX_LOAD_DIRECT] #%llu fmt=%u %ux%u x%ux%u base=%08X lb=%u unpack=%u",
                    (unsigned long long)nld, d.fmt, d.width, d.height, tsx, tsy, d.basePhys, lb, um);
      return true;
    }
  }
  if (scaled) {  // iter-466 res4k (SR_RES_TEXLOG, diagnostic): why a scaled load took the scratch path
    static const bool s_tl = std::getenv("SR_RES_TEXLOG") != nullptr;
    static std::atomic<uint32_t> s_sn{0};
    if (s_tl && s_sn.fetch_add(1, std::memory_order_relaxed) < 64)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[RES_TEXLOG] scratch path: fmt=%u %ux%u dxgi=%u levels=%u jobs=%zu copies=%zu box=%d uav=%d",
                  d.fmt, d.width, d.height, unsigned(d.dxgi), d.levels, jobs.size(), copies.size(),
                  copies.empty() ? -1 : int(copies[0].box),
                  int((tex->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0));
  }
  // Scratch (xenia RequestScratchGPUBuffer): grown in 16 MiB steps; a replaced
  // buffer may still be referenced by this frame's list, so it retires.
  if (total > m_shmScratchBytes) {
    if (m_shmScratch) retireResource(m_shmScratch);
    m_shmScratch.Reset();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (total + (16ull << 20) - 1u) & ~((16ull << 20) - 1u);
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    m_shmScratchState = D3D12_RESOURCE_STATE_COMMON;
    if (FAILED(sr::hitch::timed(sr::hitch::kResCreate, [&] {
          return m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, m_shmScratchState,
                                                   nullptr, IID_PPV_ARGS(&m_shmScratch));
        }))) {
      m_shmScratchBytes = 0;
      return false;
    }
    m_shmScratch->SetName(L"PM4_TEXLOAD_SCRATCH");
    m_shmScratchBytes = bd.Width;
  }
  pm4ShmState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if (m_shmScratchState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    pm4ShmBarrier(m_commandList.Get(), m_shmScratch.Get(), m_shmScratchState,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_shmScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  if (scaled) {
    // Every job's guest range mapped before the loader reads it (Xenia
    // EnsureScaledResolveMemoryCommitted for the base and the mips; unmapped parts
    // of a partly resolved texture read zero).
    for (const Job& j : jobs) {
      const uint64_t span = uint64_t((j.pitchBlocks + 31u) & ~31u) * d.blockBytes *
                            ((j.hBlocksGuest + 31u) & ~31u);
      if (!srEnsure(j.guestOffset & 0x1FFFFFFFu, uint32_t(std::min<uint64_t>(span, kShmBytes))))
        return false;
    }
    srState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  gtMark(GtDraw);  // iter-466 res4k: SR_PM4_GPU_TIME per-pass attribution
  nbTexLoadBegin(d, false);  // iter-467 native-b census (current until nbEnd below)
  m_commandList->SetComputeRootSignature(m_shmLoadRs.Get());
  m_commandList->SetPipelineState(scaled ? m_srLoadPso[ls].Get() : m_shmLoadPso[ls].Get());
  if (!scaled) m_commandList->SetComputeRootShaderResourceView(1, m_shm->GetGPUVirtualAddress());
  m_commandList->SetComputeRootUnorderedAccessView(2, m_shmScratch->GetGPUVirtualAddress());
  for (const Job& j : jobs) {
    if (scaled)
      m_commandList->SetComputeRootShaderResourceView(
          1, m_srBuf->GetGPUVirtualAddress() +
                 uint64_t(j.guestOffset & 0x1FFFFFFFu) * (uint64_t(tsx) * tsy));
    // LoadConstants (texture_cache.h:417-438): 2D tiling; the scale in bits 4-9
    // (1x1 when unscaled, as before).
    const uint32_t c[10] = {uint32_t(d.tiled) | ((d.endian & 3u) << 2) | (tsx << 4) | (tsy << 7),
                            scaled ? 0u : (j.guestOffset & 0x1FFFFFFFu),
                            j.pitchBlocks,
                            (j.hBlocksGuest + 31u) & ~31u,
                            j.wBlocks,
                            j.hBlocks,
                            1u,
                            uint32_t(j.hostOff),
                            j.rowPitch,
                            j.heightTexels};
    m_commandList->SetComputeRoot32BitConstants(0, 10, c, 0);
    const uint32_t gxBlocks = 1u << (2u + li.xLog2);
    m_commandList->Dispatch((j.wBlocks + gxBlocks - 1u) / gxBlocks, (j.hBlocks + 31u) / 32u, 1u);
  }
  pm4ShmBarrier(m_commandList.Get(), m_shmScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_shmScratchState = D3D12_RESOURCE_STATE_COPY_SOURCE;
  gtMark(scaled ? GtTexLoadScaled : GtTexLoad);  // iter-466 res4k: the load CS (the copy follows)
  if (fromSR)
    pm4ShmBarrier(m_commandList.Get(), tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COPY_DEST);
  for (const Copy& c : copies) {
    const Job& j = jobs[c.job];
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = d.subresource + c.sub;
    src.pResource = m_shmScratch.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = j.hostOff;
    src.PlacedFootprint.Footprint.Format = d.dxgi;
    src.PlacedFootprint.Footprint.Width = j.fpW;
    src.PlacedFootprint.Footprint.Height = j.fpH;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = j.rowPitch;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, c.box ? &c.b : nullptr);
  }
  pm4ShmBarrier(m_commandList.Get(), tex, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  gtMark(GtTexCopy);  // iter-466 res4k: scratch -> texture
  nbEnd();
  m_shmTexLoads++;
  if (scaled) {
    m_srTexLoads++;
    static std::atomic<uint32_t> s_log{0};
    const uint32_t n = s_log.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 24 || n % 2000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[RES_SCALE] #%u scaled texture load fmt=%u %ux%u x%ux%u base=%08X levels=%u",
                  n, d.fmt, d.width, d.height, tsx, tsy, d.basePhys, d.levels);
  }
  return true;
}

// iter-460 perf: the host format the loader writes for each guest format - the
// same rows getOrCreateTexture2D creates on the PM4 path (_EXPAND as FLOAT).
static bool pm4LoaderHostFormat(uint32_t fmt, DXGI_FORMAT& dxgi, uint32_t& blockW,
                                uint32_t& blockBytes) {
  blockW = 1;
  blockBytes = 4;
  switch (fmt) {
    case 6: case 50: dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
    case 18: dxgi = DXGI_FORMAT_BC1_UNORM; blockW = 4; blockBytes = 8; return true;
    case 19: dxgi = DXGI_FORMAT_BC2_UNORM; blockW = 4; blockBytes = 16; return true;
    case 20: dxgi = DXGI_FORMAT_BC3_UNORM; blockW = 4; blockBytes = 16; return true;
    case 49: dxgi = DXGI_FORMAT_BC5_UNORM; blockW = 4; blockBytes = 16; return true;
    case 2: dxgi = DXGI_FORMAT_R8_UNORM; blockBytes = 1; return true;
    case 24: dxgi = DXGI_FORMAT_R16_UNORM; blockBytes = 2; return true;
    case 27: case 30: dxgi = DXGI_FORMAT_R16_FLOAT; blockBytes = 2; return true;
    case 25: dxgi = DXGI_FORMAT_R16G16_UNORM; blockBytes = 4; return true;
    case 28: case 31: dxgi = DXGI_FORMAT_R16G16_FLOAT; blockBytes = 4; return true;
    case 36: dxgi = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; return true;
    case 26: dxgi = DXGI_FORMAT_R16G16B16A16_UNORM; blockBytes = 8; return true;
    case 29: case 32: dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; blockBytes = 8; return true;
    case 22: case 23: dxgi = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; return true;
    default: dxgi = DXGI_FORMAT_UNKNOWN; return false;
  }
}

// iter-463 D4b: a stacked / cube fetch's level layout (defined with pm4ShmLoadArray).
static uint32_t arrayMipLayout(const uint32_t* f, uint32_t fmt, uint32_t width, uint32_t height,
                               uint32_t pitchTexels, bool tiled, uint32_t blockW,
                               uint32_t blockBytes, uint32_t layers,
                               sr::gfx::cube::CubeLayout& L);

// The shared-memory ranges a 2D texture fetch reads: the base level (or the
// whole face when the mips pack) and the mip tail - the same layout math as
// getOrCreateTexture2D's request.
bool D3D12Backend::texShmRanges(const uint32_t* f, uint32_t* base, uint32_t* baseLen,
                                uint32_t* mipPhys, uint32_t* mipLen) {
  namespace CL = sr::gfx::cube;
  *mipPhys = 0;
  *mipLen = 0;
  const uint32_t basePage = (f[1] >> 12) & 0xFFFFFu;
  const uint32_t fmt = f[1] & 0x3Fu;
  const uint32_t width = (f[2] & 0x1FFFu) + 1u, height = ((f[2] >> 13) & 0x1FFFu) + 1u;
  uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  const bool tiled = ((f[0] >> 31) & 1u) != 0;
  if (!basePage || width > 4096u || height > 4096u) return false;
  if (pitchTexels < width) pitchTexels = width;
  DXGI_FORMAT dxgi;
  uint32_t blockW, blockBytes;
  if (!pm4LoaderHostFormat(fmt, dxgi, blockW, blockBytes) || !pm4ShmLoadable(fmt)) return false;
  uint32_t pb = 0;
  uint32_t bl = CL::detail::levelSliceStride(0, true, width, height, pitchTexels, blockW, blockW,
                                             blockBytes, tiled, &pb);
  const uint32_t mipPage = (f[5] >> 12) & 0xFFFFFu;
  const bool packed = ((f[5] >> 11) & 1u) != 0;
  const uint32_t sizeMax = CL::detail::log2Floor(width > height ? width : height);
  const uint32_t mipMax = mipPage ? std::min((f[4] >> 6) & 0xFu, sizeMax) : 0u;
  const bool baseTail = packed && CL::detail::packedMipLevel(width, height) == 0u;
  if (mipMax > 0u || baseTail) {
    const CL::CubeLayout L = CL::computeGuestLayout(fmt, width, height, pitchTexels, tiled,
                                                    mipMax + 1u, packed, blockW, blockW, blockBytes, 1u);
    if (L.ok) {
      bl = std::max(bl, L.faceStrideBytes);
      if (mipMax > 0u && L.mipsTotalExtentBytes) {
        *mipPhys = mipPage * 4096u;
        *mipLen = L.mipsTotalExtentBytes;
      }
    }
  }
  *base = basePage * 4096u;
  *baseLen = bl;
  return true;
}

// iter-464 resscale: Xenia's TextureKey.scaled_resolve (texture_cache.cc:597-620):
// the draw resolution is scaled, the texture is tiled, a scaled loader exists for
// its format, and a scaled resolve wrote ANY page of its base or mip range (the
// whole texture then loads from the scaled memory - Xenia's "load unscaled parts"
// TODO). Cheap upper bounds stand in for the exact layout: the base slice
// (pitch and rows aligned to 32 blocks, times the layers) and as much again for
// the mips. The scaled size must fit the 16384 texture limit.
bool D3D12Backend::texResolutionScaled(const uint32_t* f, uint32_t layers) const {
  if (!m_srReady || (f[0] & 3u) != 2u || !((f[0] >> 31) & 1u)) return false;
  const uint32_t fmt = f[1] & 0x3Fu;
  if (!scaledLoadable(fmt)) return false;
  DXGI_FORMAT dxgi;
  uint32_t blockW, blockBytes;
  if (!pm4LoaderHostFormat(fmt, dxgi, blockW, blockBytes) || blockW != 1u) return false;
  const uint32_t width = (f[2] & 0x1FFFu) + 1u, height = ((f[2] >> 13) & 0x1FFFu) + 1u;
  const resscale::Scale& rs = resscale::get();
  if (width * rs.x > 16384u || height * rs.y > 16384u) return false;
  uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  if (pitchTexels < width) pitchTexels = width;
  const uint64_t slice = uint64_t((pitchTexels + 31u) & ~31u) * blockBytes * ((height + 31u) & ~31u);
  const uint64_t span = std::min<uint64_t>(((slice + 4095u) & ~4095ull) * std::max(layers, 1u), kShmBytes);
  const uint32_t basePhys = ((f[1] >> 12) & 0xFFFFFu) << 12;
  if (basePhys && srAnyScaled(basePhys, uint32_t(span))) return true;
  const uint32_t mipPhys = ((f[5] >> 12) & 0xFFFFFu) << 12;
  return mipPhys && srAnyScaled(mipPhys, uint32_t(span));
}

#if SR_ORACLE
// iter-467 native-b (SR_NB_PREPASS_FLAT, baked ON; =0 off; SR_AB_TOGGLE): the prepass's per-set dedupe and each fetch's
// request ranges in one flat per-thread table keyed by the 24-byte fetch constant (compared whole). cp_prof21 (1x):
// shmCollectTextures 0.84 ms per frame - a std::unordered_set insert per new fetch (0.14), its clear per set, and
// texShmRanges / the array layout (0.10) recomputed every frame for the same fetches. With the gate a fetch already
// seen in an earlier set is a probe + generation compare and its ranges are reused (pure in the fetch constant);
// the set's dedupe is a generation bump. SR_NB_PREPASS_FLAT_CHECK=1 recomputes every reused entry and reports a
// difference ([NB_PREPASS] CHECK MISMATCH).
namespace {
struct PrepEnt {
  uint32_t f[6];
  uint32_t gen;      // the set generation it was last collected in (0 = empty)
  uint8_t dim;       // (tf[5] >> 9) & 3
  bool arr, tex;     // the layer requests / the 2D (or layered) base + mip requests apply
  uint32_t aBase, aStride, aLayers, aMip, aMipLen;
  uint32_t tBase, tLen, tMip, tMipLen;
};
bool nbPrepassFlatOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_PREPASS_FLAT");
    // iter-467 native-b: baked ON (=0 A/B). Four in-roll A/B rolls (fr26, 1x, 234 windows): GPU_CP -0.14 +-0.17,
    // render thread -0.17 +-0.13 ms per frame (the prepass holds m_shmMu for its whole run); live CHECK 17.1M reuses,
    // 0 mismatch; replays 9/9 at 1x and 3x.
    return !e || e[0] != '0';
  }();
  static const int ab = sr::abt::reg("SR_NB_PREPASS_FLAT");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}
bool nbPrepassFlatCheckOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_PREPASS_FLAT_CHECK");
    return e && e[0] == '1';
  }();
  return v;
}
}  // namespace

// The request ranges of one texture fetch (shmCollectTextures' rule): false = nothing to request.
bool D3D12Backend::shmPrepRanges(const uint32_t* tf, void* out) {
  PrepEnt& e = *static_cast<PrepEnt*>(out);
  e.dim = uint8_t((tf[5] >> 9) & 3u);
  e.arr = e.tex = false;
  e.aBase = e.aStride = e.aLayers = e.aMip = e.aMipLen = 0;
  e.tBase = e.tLen = e.tMip = e.tMipLen = 0;
  if (e.dim == 2u) return false;  // volumes decode on the CPU (pm4ShmSyncToGuest)
  const uint32_t layers = e.dim == 3u ? 6u : ((tf[2] >> 26) & 0x3Fu) + 1u;
  if (layers > 1u) {
    DXGI_FORMAT dxgi;
    uint32_t blockW, blockBytes;
    const uint32_t fmt = tf[1] & 0x3Fu;
    if (!pm4LoaderHostFormat(fmt, dxgi, blockW, blockBytes) || !pm4ShmLoadable(fmt)) return false;
    const uint32_t width = (tf[2] & 0x1FFFu) + 1u, height = ((tf[2] >> 13) & 0x1FFFu) + 1u;
    uint32_t pitchTexels = ((tf[0] >> 22) & 0x1FFu) << 5;
    if (pitchTexels < width) pitchTexels = width;
    const uint32_t pitchBlocks = (pitchTexels + blockW - 1u) / blockW;
    const uint32_t rowPitch = ((pitchBlocks + 31u) & ~31u) * blockBytes;
    const uint32_t blockRows = ((((height + blockW - 1u) / blockW) + 31u) & ~31u);
    const uint32_t layerStride = (rowPitch * blockRows + 4095u) & ~4095u;
    const uint32_t basePhys = ((tf[1] >> 12) & 0xFFFFFu) * 4096u;
    if (!basePhys || uint64_t(basePhys) + uint64_t(layerStride) * layers > kShmBytes) return false;
    e.arr = true;
    e.aBase = basePhys;
    e.aStride = layerStride;
    e.aLayers = layers;
    sr::gfx::cube::CubeLayout L;
    const bool tiled = ((tf[0] >> 31) & 1u) != 0;
    if (arrayMipLayout(tf, fmt, width, height, pitchTexels, tiled, blockW, blockBytes, layers, L) > 1u) {
      e.aMip = ((tf[5] >> 12) & 0xFFFFFu) * 4096u;
      e.aMipLen = L.mipsTotalExtentBytes;
    }
    if (e.dim == 3u) return true;
  }
  uint32_t base, baseLen, mipPhys, mipLen;
  if (texShmRanges(tf, &base, &baseLen, &mipPhys, &mipLen)) {
    e.tex = true;
    e.tBase = base;
    e.tLen = baseLen;
    e.tMip = mipPhys;
    e.tMipLen = mipLen;
  }
  return e.arr || e.tex;
}

void D3D12Backend::shmPrepRequest(const void* ent) {
  const memres::KindScope mrKind(memres::Kind::kTex);  // mem.resources step 2a: the census's fetch kind
  const PrepEnt& e = *static_cast<const PrepEnt*>(ent);
  if (e.arr) {
    for (uint32_t i = 0; i < e.aLayers; i++) pm4ShmRequest(e.aBase + i * e.aStride, e.aStride);
    if (e.aMipLen) pm4ShmRequest(e.aMip, e.aMipLen);
  }
  if (e.tex) {
    pm4ShmRequest(e.tBase, e.tLen);
    if (e.tMipLen) pm4ShmRequest(e.tMip, e.tMipLen);
  }
}

// Collect the textures one batch binds (the pre-pass then validates them
// together). Slots equal to the previous batch's and fetches already
// collected this frame cost a compare / one lookup.
void D3D12Backend::shmCollectTextures(const uint32_t* fetchRaw) {
  if (!m_shmReady || !t_shmCollectInto) return;
  const memres::KindScope mrKind(memres::Kind::kTex);  // mem.resources step 2a: the census's fetch kind
  for (uint32_t t = 0; t < 16; t++) {
    const uint32_t* tf = &fetchRaw[t * 6];
    if ((tf[0] & 3u) != 2u) continue;
    if (std::memcmp(tf, &m_shmTexPrev[t * 6], 24) == 0) continue;
    std::memcpy(&m_shmTexPrev[t * 6], tf, 24);
    if (nbPrepassFlatOn()) {  // SR_NB_PREPASS_FLAT
      thread_local std::unique_ptr<PrepEnt[]> t_prep;
      constexpr uint32_t kBits = 13u, kProbe = 8u;
      if (!t_prep) t_prep.reset(new PrepEnt[1u << kBits]());
      const uint32_t gen = g_prepGen.load(std::memory_order_relaxed);
      const uint64_t h = ShaderCache::hashUcode(tf, 24);
      const uint32_t i0 = uint32_t(h >> (64u - kBits));
      PrepEnt* hit = nullptr;
      PrepEnt* slot = nullptr;
      for (uint32_t k = 0; k < kProbe; k++) {
        PrepEnt& c = t_prep[(i0 + k) & ((1u << kBits) - 1u)];
        if (c.gen && std::memcmp(c.f, tf, 24) == 0) {
          hit = &c;
          break;
        }
        if (!slot && (!c.gen || c.gen != gen)) slot = &c;  // empty, or not collected in this set
      }
      if (hit) {
        if (hit->gen == gen) continue;  // collected in this set
        hit->gen = gen;
        g_prepHits.fetch_add(1, std::memory_order_relaxed);
        if (nbPrepassFlatCheckOn()) {
          PrepEnt c{};
          shmPrepRanges(tf, &c);
          // SR_NB_PREPASS_FLAT_TEST_MUT=1 (checker mutation): the reference is perturbed, every compare must fail.
          static const bool s_mut = [] {
            const char* e = std::getenv("SR_NB_PREPASS_FLAT_TEST_MUT");
            return e && e[0] == '1';
          }();
          if (s_mut) c.tLen += 4096u;
          if (c.dim != hit->dim || c.arr != hit->arr || c.tex != hit->tex || c.aBase != hit->aBase ||
              c.aStride != hit->aStride || c.aLayers != hit->aLayers || c.aMip != hit->aMip ||
              c.aMipLen != hit->aMipLen || c.tBase != hit->tBase || c.tLen != hit->tLen || c.tMip != hit->tMip ||
              c.tMipLen != hit->tMipLen) {
            const uint64_t m = g_prepMismatch.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (m <= 16u)
              SR_LOG_BOOT(sr::LogSeverity::Error, "[NB_PREPASS] CHECK MISMATCH #%llu fetch=%08X %08X %08X",
                          (unsigned long long)m, tf[0], tf[1], tf[2]);
          }
        }
        shmPrepRequest(hit);
        continue;
      }
      PrepEnt fresh{};
      std::memcpy(fresh.f, tf, 24);
      fresh.gen = gen;
      const bool any = shmPrepRanges(tf, &fresh);
      g_prepNew.fetch_add(1, std::memory_order_relaxed);
      if (!slot) slot = &t_prep[i0];  // the window is full: replace its first entry
      *slot = fresh;
      if (any) shmPrepRequest(slot);
      continue;
    }
    if (!m_shmTexCollected.insert(ShaderCache::hashUcode(tf, 24)).second) continue;
    const uint32_t dim = (tf[5] >> 9) & 3u;
    if (dim == 2u) continue;  // volumes decode on the CPU (pm4ShmSyncToGuest)
    const uint32_t layers = dim == 3u ? 6u : ((tf[2] >> 26) & 0x3Fu) + 1u;
    if (layers > 1u) {  // a stack or cube: the layers (as pm4ShmLoadArray)
      DXGI_FORMAT dxgi;
      uint32_t blockW, blockBytes;
      const uint32_t fmt = tf[1] & 0x3Fu;
      if (!pm4LoaderHostFormat(fmt, dxgi, blockW, blockBytes) || !pm4ShmLoadable(fmt)) continue;
      const uint32_t width = (tf[2] & 0x1FFFu) + 1u, height = ((tf[2] >> 13) & 0x1FFFu) + 1u;
      uint32_t pitchTexels = ((tf[0] >> 22) & 0x1FFu) << 5;
      if (pitchTexels < width) pitchTexels = width;
      const uint32_t pitchBlocks = (pitchTexels + blockW - 1u) / blockW;
      const uint32_t rowPitch = ((pitchBlocks + 31u) & ~31u) * blockBytes;
      const uint32_t blockRows = ((((height + blockW - 1u) / blockW) + 31u) & ~31u);
      const uint32_t layerStride = (rowPitch * blockRows + 4095u) & ~4095u;
      const uint32_t basePhys = ((tf[1] >> 12) & 0xFFFFFu) * 4096u;
      if (!basePhys || uint64_t(basePhys) + uint64_t(layerStride) * layers > kShmBytes) continue;
      for (uint32_t i = 0; i < layers; i++) pm4ShmRequest(basePhys + i * layerStride, layerStride);
      {  // iter-463 D4b: and the array's mip region (pm4ShmLoadArray's layout)
        sr::gfx::cube::CubeLayout L;
        const bool tiled = ((tf[0] >> 31) & 1u) != 0;
        if (arrayMipLayout(tf, fmt, width, height, pitchTexels, tiled, blockW, blockBytes, layers, L) > 1u)
          pm4ShmRequest(((tf[5] >> 12) & 0xFFFFFu) * 4096u, L.mipsTotalExtentBytes);
      }
      if (dim == 3u) continue;
    }
    uint32_t base, baseLen, mipPhys, mipLen;
    if (!texShmRanges(tf, &base, &baseLen, &mipPhys, &mipLen)) continue;
    pm4ShmRequest(base, baseLen);
    if (mipLen) pm4ShmRequest(mipPhys, mipLen);
  }
}

// The guest range a resolve batch writes (the same math pm4ShmResolve uses).
#endif
bool D3D12Backend::pm4ShmResolveExtent(const edram::Regs& regs, uint32_t& extStart, uint32_t& extLen) {
  const edram::ResolveInfo ri = edram::computeResolve(regs);
  if (!ri.valid || !ri.copy) return false;
  XeResolveConsts c{};
  uint32_t gx = 0, gy = 0;
  extStart = extLen = 0;
  return buildResolve(ri, regs, c, gx, gy, extStart, extLen) >= 0 && extLen != 0;
}

#if SR_ORACLE
static bool shmPrepassEnabled() {
  static const bool v = [] {  // SR_PM4_SHM_PREPASS=0: textures validate one by one at the bind
    const char* e = std::getenv("SR_PM4_SHM_PREPASS");
    return !e || e[0] != '0';
  }();
  return v;
}

// Frame start (the list is open): mask this frame's resolve extents, collect
// every batch's textures, validate them in one parallel flush with the copies
// recorded now - before any draw, which is where a page no resolve of this
// frame touches may legitimately land.
void D3D12Backend::pm4ShmTexturePrepass(const std::vector<DrawBatch>& batches, bool deferred) {
  if (memres::resourceDecides()) return;  // native texture bytes were captured at their draw
  if (!m_shmReady || !shmPrepassEnabled() || batches.empty()) return;
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  if (deferred) {  // iter-465 perf2: the CP's prepass dedupes per set (pm4ShmEndFrame runs on the RT)
    m_shmTexCollected.clear();
    std::memset(m_shmTexPrev, 0, sizeof(m_shmTexPrev));
    g_prepGen.fetch_add(1, std::memory_order_relaxed);  // SR_NB_PREPASS_FLAT: a new set
  }
  if (m_shmResolveMask.size() != kShmPages) m_shmResolveMask.assign(kShmPages, 0);
  std::vector<uint32_t> masked;
  for (const DrawBatch& b : batches) {
    if (!b.resolve || !b.pm4) continue;
    uint32_t s = 0, l = 0;
    if (!pm4ShmResolveExtent(b.pm4Regs, s, l)) continue;
    const uint32_t p0 = (s & 0x1FFFFFFFu) >> 12;
    const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(s & 0x1FFFFFFFu) + l - 1u, kShmBytes - 1u) >> 12);
    for (uint32_t p = p0; p <= p1; p++) {
      if (!m_shmResolveMask[p]) masked.push_back(p);
      m_shmResolveMask[p] = 1;
    }
  }
  t_shmCollectInto = &m_shmPrepassCollect;
  t_shmPrepass = true;
  t_shmDefer = false;
  for (const DrawBatch& b : batches)
    if (!b.resolve && b.pm4 && !b.texSame) shmCollectTextures(b.texFetch);  // texSame: SR_CP_TEX_SKIPSAME
  t_shmCollectInto = nullptr;
  t_shmPrepass = false;
  if (!m_shmPrepassCollect.empty()) {
    pm4ShmValidate(m_shmPrepassCollect, deferred);
    m_shmPrepassCollect.clear();
  }
  for (const uint32_t p : masked) m_shmResolveMask[p] = 0;
}

// iter-463 D4b (SR_PM4_ARRAY_MIPS, default ON; =0: base level only, as
// before): stacked and cube textures carry their mip chain like the 2D path -
// mip_max_level + 1 levels (xenia d3d12_texture_cache.cc:1361, :1557), placed
// by GetGuestTextureLayout with array_size = the layer count (cube_layout.hpp
// computeGuestLayout, faces = layers). Levels as the 2D path (P1): mip_address
// != 0 gives min(mip_max_level, log2(longest axis)); a packed base tail uses
// the layout even without mips.
#endif
static bool arrayMipsEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_ARRAY_MIPS");
    return !e || e[0] != '0';
  }();
  return v;
}

// The layout of a stacked / cube fetch's levels. Returns the level count
// (1 = base only); L.ok = the layout places the levels (else the per-layer
// base-only load).
static uint32_t arrayMipLayout(const uint32_t* f, uint32_t fmt, uint32_t width, uint32_t height,
                               uint32_t pitchTexels, bool tiled, uint32_t blockW,
                               uint32_t blockBytes, uint32_t layers,
                               sr::gfx::cube::CubeLayout& L) {
  namespace CL = sr::gfx::cube;
  L = CL::CubeLayout{};
  if (!arrayMipsEnabled()) return 1u;
  const uint32_t mipPage = (f[5] >> 12) & 0xFFFFFu;
  const bool packed = ((f[5] >> 11) & 1u) != 0;
  const uint32_t sizeMax = CL::detail::log2Floor(width > height ? width : height);
  const uint32_t mipMax = mipPage ? std::min((f[4] >> 6) & 0xFu, sizeMax) : 0u;
  const bool baseTail = packed && CL::detail::packedMipLevel(width, height) == 0u;
  if (mipMax == 0u && !baseTail) return 1u;
  L = CL::computeGuestLayout(fmt, width, height, pitchTexels, tiled, mipMax + 1u, packed, blockW,
                             blockW, blockBytes, layers);
  if (L.ok && mipMax > 0u &&
      (!L.mipsTotalExtentBytes || uint64_t(mipPage) * 4096u + L.mipsTotalExtentBytes > kShmBytes))
    L.ok = false;
  if (!L.ok) {
    static std::atomic<uint32_t> s_refuse{0};
    if (s_refuse.fetch_add(1, std::memory_order_relaxed) < 16)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_ARRAY_MIPS] refused (%s) fmt=%u %ux%u x%u levels=%u packed=%d mips=0x%08X "
                  "-> base level only",
                  CL::cubeRefuseName(L.refuse), fmt, width, height, layers, mipMax + 1u,
                  packed ? 1 : 0, mipPage * 4096u);
    L = CL::CubeLayout{};
    return 1u;
  }
  return mipMax + 1u;
}

// iter-463 review (SR_REPLAY_MIPS_SUPPLIED, default ON; =0: a replay loads
// array mips whether or not the trace supplied them). Outside a replay the
// map is empty and every range counts as supplied.
static bool replayMipsSuppliedEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_REPLAY_MIPS_SUPPLIED");
    return !e || e[0] != '0';
  }();
  return v;
}

void D3D12Backend::replayNoteSupplied(uint32_t phys, uint32_t len) {
  // Sized once, before the first record: the render thread reads it while the
  // replay thread marks pages, so it never reallocates.
  if (m_replaySupplied.empty()) m_replaySupplied.assign(kShmPages, 0);
  if (!len) return;
  phys &= 0x1FFFFFFFu;
  const uint64_t end = std::min<uint64_t>(uint64_t(phys) + len, kShmBytes);
  for (uint32_t p = phys >> 12; (uint64_t(p) << 12) < end; p++) m_replaySupplied[p] = 1;
}

// Every page of [phys, +len) supplied by the replayed trace or written by a
// resolve the replay executed (those are the player's own output).
bool D3D12Backend::replaySupplied(uint32_t phys, uint32_t len) const {
  if (m_replaySupplied.empty() || !replayMipsSuppliedEnabled() || !len) return true;
  phys &= 0x1FFFFFFFu;
  const uint64_t end = std::min<uint64_t>(uint64_t(phys) + len, kShmBytes);
  for (uint32_t p = phys >> 12; (uint64_t(p) << 12) < end; p++) {
    if (m_replaySupplied[p]) continue;
    if (m_shmReady && p < m_shmGpuWritten.size() && m_shmGpuWritten[p]) continue;
    return false;
  }
  return true;
}

static bool shmArrayEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_SHM_ARRAY");
    return !e || e[0] != '0';
  }();
  return v;
}

// iter-460 perf: the title's shadow map (k_16 640x640 x4 layers, 3.2 MB) was
// re-read from the GPU, hashed, untiled and re-uploaded with two fence waits at
// EVERY bind (the bind census: 3 array binds = 5-12 ms of a title frame; the
// gameplay frame binds it in far more batches). This is the Xenia way: the
// texture is loaded from the shared memory by the load shaders, one dispatch
// per layer into subresource i, and a bind whose layers' page stamps did not
// move costs the version scan only.
uint32_t D3D12Backend::pm4ShmLoadArray(const uint32_t* f, bool cube) {
  if (!shmArrayEnabled() || !m_texInline || (f[0] & 3u) != 2u) return 0u;
  const uint32_t layers = cube ? 6u : ((f[2] >> 26) & 0x3Fu) + 1u;
  if (layers <= 1u) return 0u;
  const uint32_t fmt = f[1] & 0x3Fu;
  DXGI_FORMAT dxgi;
  uint32_t blockW, blockBytes;
  if (!pm4LoaderHostFormat(fmt, dxgi, blockW, blockBytes) || !pm4ShmLoadable(fmt) || !pm4ShmInit())
    return 0u;
  const uint32_t width = (f[2] & 0x1FFFu) + 1u, height = ((f[2] >> 13) & 0x1FFFu) + 1u;
  uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  if (pitchTexels < width) pitchTexels = width;
  const bool tiled = ((f[0] >> 31) & 1u) != 0;
  const uint32_t endian = (f[1] >> 6) & 3u;
  const uint32_t basePhys = ((f[1] >> 12) & 0xFFFFFu) * 4096u;
  if (!basePhys || width > 4096u || height > 4096u) return 0u;
  // Layer stride as getOrCreateTexture2DArray (texture_util.cc:312-352): the
  // base row pitch in blocks and the block rows aligned to 32, the slice to 4 KB.
  const uint32_t pitchBlocks = (pitchTexels + blockW - 1u) / blockW;
  const uint32_t rowPitch = ((pitchBlocks + 31u) & ~31u) * blockBytes;
  const uint32_t blockRows = ((((height + blockW - 1u) / blockW) + 31u) & ~31u);
  const uint32_t layerStride = (rowPitch * blockRows + 4095u) & ~4095u;
  if (uint64_t(basePhys) + uint64_t(layerStride) * layers > kShmBytes) return 0u;
  // iter-463 D4b: the level chain (cube_layout's layer stride is layerStride).
  namespace CL = sr::gfx::cube;
  CL::CubeLayout L;
  uint32_t levels = 0;
  // iter-467 native-b B-CPU (SR_NB_BIND_MEMO): the layout is a pure function of the fetch constant and the layer
  // count, recomputed at every bind before the epoch / clock short-cut below (strand A's pr_off profile:
  // arrayMipLayout + computeGuestLayout 0.58 ms / frame on the render thread) - kept per fetch instead.
  static const bool s_layoutMemoEnv = [] {
    const char* e = std::getenv("SR_NB_BIND_MEMO");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  static const int s_layoutMemoAb = sr::abt::reg("SR_NB_BIND_MEMO");  // SR_AB_TOGGLE (the bind memo's bit)
  if (s_layoutMemoEnv && (s_layoutMemoAb < 0 || sr::abt::on(s_layoutMemoAb))) {
    struct LayoutMemo {
      uint32_t f[6];
      uint32_t layers, levels;
      CL::CubeLayout L;
    };
    thread_local std::unordered_map<uint64_t, LayoutMemo> t_layouts;
    const uint64_t hk = ShaderCache::hashUcode(f, 24) ^ (uint64_t(layers) << 56);
    auto lit = t_layouts.find(hk);
    if (lit != t_layouts.end() && lit->second.layers == layers && std::memcmp(lit->second.f, f, 24) == 0) {
      L = lit->second.L;
      levels = lit->second.levels;
    } else {
      levels = arrayMipLayout(f, fmt, width, height, pitchTexels, tiled, blockW, blockBytes, layers, L);
      if (t_layouts.size() >= 4096u) t_layouts.clear();
      LayoutMemo& m = t_layouts[hk];
      std::memcpy(m.f, f, 24);
      m.layers = layers;
      m.levels = levels;
      m.L = L;
    }
  } else {
    levels = arrayMipLayout(f, fmt, width, height, pitchTexels, tiled, blockW, blockBytes, layers, L);
  }
  // A replayed trace that never supplied the mip region (every trace recorded
  // before iter-463 D8): the pages there are the replay's never-written zeros,
  // so the base level alone is the closest to what the live game drew.
  if (levels > 1u && !replaySupplied(((f[5] >> 12) & 0xFFFFFu) * 4096u, L.mipsTotalExtentBytes)) {
    static std::atomic<uint32_t> s_unsup{0};
    if (s_unsup.fetch_add(1, std::memory_order_relaxed) < 16)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_ARRAY_MIPS] replay: mips 0x%08X+0x%X not in the trace -> base level only "
                  "(%s fmt=%u %ux%u levels=%u; SR_REPLAY_MIPS_SUPPLIED=0 loads them anyway)",
                  ((f[5] >> 12) & 0xFFFFFu) * 4096u, L.mipsTotalExtentBytes, cube ? "cube" : "stacked",
                  fmt, width, height, levels);
    L = CL::CubeLayout{};
    levels = 1u;
  }
  const uint32_t mipPhys = levels > 1u ? ((f[5] >> 12) & 0xFFFFFu) * 4096u : 0u;
  const uint32_t mipLen = levels > 1u ? L.mipsTotalExtentBytes : 0u;
  // A laid-out array is a different resource from the base-only one the CPU
  // layer path (getOrCreateTexture2DArray) keys by the same fetch constant: its
  // own key, so an N-level array never aliases a 1-level one.
  // iter-464 resscale: a stacked / cube texture a scaled resolve wrote (e.g. the
  // shadow cascades) is a separate, scaled resource (Xenia key.scaled_resolve).
  uint32_t tsx = 1, tsy = 1;
  if (resscale::get().scaled() && texResolutionScaled(f, layers)) {
    tsx = resscale::get().x;
    tsy = resscale::get().y;
  }
  const uint64_t key = (ShaderCache::hashUcode(f, 24) ^ (cube ? 0x437562654D6170ull   // "CubeMap"
                                                               : 0x5374616B6564ull))  // "Staked"
                       ^ (L.ok ? 0x4D69704368616E00ull + levels : 0ull)              // "MipChan"
                       ^ (tsx > 1 || tsy > 1 ? 0x5265735363616C65ull : 0ull);        // "ResScale"
  TexArray& a = m_texArrays[key];
  // Bound again in the same epoch with no upload or GPU write since (the stamp
  // clock moves on every one): the versions cannot have changed.
  if (a.res && a.gpu && a.checkEpoch == m_shmEpoch && a.checkClock == m_shmStampClock) {
#if SR_ORACLE
    if (memres::texIdOn())  // textures_from_resolves: the same content its last full check classified
      memres::texIdBind(memres::TexIdKind::kArray, memres::TexIdClass(a.texIdClass), 0u, 0u, basePhys, fmt);
#endif
    return a.slot;
  }
  // Every layer's pages current in the shared memory, then its version.
  const memres::KindScope mrKind(memres::Kind::kTex);  // mem.resources step 2a: the census's fetch kind
  std::vector<uint32_t> vers(layers + (mipLen ? 1u : 0u));
  for (uint32_t i = 0; i < layers; i++) {
    const uint32_t lp = basePhys + i * layerStride;
    pm4ShmRequest(lp, layerStride);
    vers[i] = pm4ShmVersion(lp, layerStride);
  }
  if (mipLen) {  // iter-463 D4b: the mip region versions the array too
    pm4ShmRequest(mipPhys, mipLen);
    vers[layers] = pm4ShmVersion(mipPhys, mipLen);
  }
  a.checkEpoch = m_shmEpoch;
  a.checkClock = m_shmStampClock;
#if SR_ORACLE
  // textures_from_resolves (gfx/resource_texid.hpp): the class of this bind from each layer's resolve output and its
  // content's origin (a native target's stamp, else the loader's load) - the worst over the layers that hold any.
  auto texIdArray = [&](bool failed) {
    if (!memres::texIdOn()) return;
    if (a.layerOrigin.size() != layers) a.layerOrigin.assign(layers, 0u);
    uint32_t pages = 0, gpuPages = 0, cpuAfter = 0, src = 0;
    memres::TexIdClass cls = memres::TexIdClass::kPlain;
    auto rank = [](memres::TexIdClass c) {
      switch (c) {
        case memres::TexIdClass::kIdentity: return 1;
        case memres::TexIdClass::kArena: return 2;
        case memres::TexIdClass::kCpuSnapshot: return 3;
        case memres::TexIdClass::kGuestBytes: return 4;
        case memres::TexIdClass::kStale: return 5;
        case memres::TexIdClass::kUnknown: return 6;
        case memres::TexIdClass::kCpuAfterGpu: return 7;
        default: return 0;
      }
    };
    for (uint32_t i = 0; i < layers; i++) {
      uint32_t ca = 0;
      const uint32_t g = texIdGpuPages(basePhys + i * layerStride, layerStride, &ca, src ? nullptr : &src);
      pages += (layerStride + 4095u) / 4096u;
      gpuPages += g;
      cpuAfter += ca;
      const memres::TexIdClass c = failed && (g || ca) ? memres::TexIdClass::kStale
                                                        : memres::texIdClassOf(memres::TexOrigin(a.layerOrigin[i]), g, ca);
      if (rank(c) > rank(cls)) cls = c;
    }
    a.texIdClass = uint8_t(cls);
    memres::texIdBind(memres::TexIdKind::kArray, cls, pages, gpuPages, basePhys, fmt, cpuAfter, src);
  };
#endif
  if (a.res && a.gpu && a.layerVer == vers) {  // no layer moved
#if SR_ORACLE
    texIdArray(false);
#endif
    return a.slot;
  }
  // iter-467 native-b (SR_NB_ARRAY_LAYERS): only the layers whose version moved are loaded again. A
  // stacked texture bound between the resolves that write its layers one by one (the 4 shadow cascades:
  // 6 resolves, bound between them) reloaded EVERY layer at every bind - 22 layer loads per gameplay frame
  // for 6 written layers (B0 census). Single-level arrays with a known previous load only; the content is
  // identical (an unchanged version = unchanged memory under the layer).
  static const bool s_layersOnly = [] {
    const char* e = std::getenv("SR_NB_ARRAY_LAYERS");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  const bool partial = s_layersOnly && a.res && a.gpu && a.levels == 1u && !mipLen &&
                       a.layerVer.size() == vers.size();
  if (!a.res && !pm4TexArrayCreate(a, dxgi, width * tsx, height * tsy, layers, cube, f[3], levels,
                                    tsx > 1u || tsy > 1u))
    return 0u;
  for (uint32_t i = 0; i < layers; i++) {
    if (partial && a.layerVer[i] == vers[i]) continue;
    ShmTexDesc d;
    d.fmt = fmt;
    d.width = width;
    d.height = height;
    d.pitchTexels = pitchTexels;
    d.blockW = blockW;
    d.blockBytes = blockBytes;
    d.endian = endian;
    // iter-463 D4b: with a layout, the level placement (placeSubresource,
    // face i) finds layer i in the array's base and mip regions, and level m
    // goes to D3D12 subresource m + i * levels.
    d.basePhys = L.ok ? basePhys : basePhys + i * layerStride;
    d.mipPhys = mipPhys;
    d.levels = a.levels;
    d.tiled = tiled;
    d.dxgi = dxgi;
    d.layout = L.ok ? &L : nullptr;
    d.face = L.ok ? i : 0u;
    d.subresource = i * a.levels;
    d.scaleX = tsx;  // iter-464 resscale
    d.scaleY = tsy;
    if (!pm4ShmLoadTexture(a.res.Get(), /*fromSR=*/true, d)) {
      a.gpu = false;
      a.layerVer.clear();
#if SR_ORACLE
      texIdArray(true);
#endif
      return 0u;
    }
#if SR_ORACLE
    if (memres::texIdOn()) {
      if (a.layerOrigin.size() != layers) a.layerOrigin.assign(layers, 0u);
      a.layerOrigin[i] = uint8_t(memres::TexOrigin::kArena);
    }
#endif
  }
  a.gpu = true;
  a.layerVer = vers;
#if SR_ORACLE
  texIdArray(false);
#endif
  a.layerSrc.clear();
  a.layerHash.clear();
  m_shmArrayLoads++;
#if SR_ORACLE
  holdNoteTex(cube ? 'C' : 'A', basePhys, fmt, width, height);  // 2i diagnostic (SR_DIAG_HOLD_LOG)
#endif
  // iter-467 native-b B2: a single-level, unlaid-out stacked texture's layers can be native targets.
  if (!a.nbIndexed && !L.ok && a.levels == 1u && !cube) {
    a.nbIndexed = true;
    a.base = basePhys;
    a.layerStride = layerStride;
    a.pitch = pitchTexels;
    a.fmt = fmt;
    a.endian = endian;
    a.w = width;
    a.h = height;
    a.tsx = tsx;
    a.tsy = tsy;
    a.dxgi = dxgi;
    a.tiled = tiled;
    a.uav = (a.res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    for (uint32_t i = 0; i < layers; i++) m_nbArrayByBase.emplace(basePhys + i * layerStride, std::make_pair(key, i));
  }
  static std::atomic<uint32_t> s_log{0};
  const uint32_t n = s_log.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16 || n % 2000 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[TEX_ARRAY] #%u slot %u: %s fmt=%u %ux%u x%u layers levels=%u stride=0x%X "
                "base=0x%08X mips=0x%08X+0x%X swz=%03X (shared-memory load, ver %u)",
                n, a.slot, cube ? "cube" : "stacked", fmt, width, height, layers, a.levels,
                layerStride, basePhys, mipPhys, mipLen, (f[3] >> 1) & 0xFFFu, vers[0]);
  return a.slot;
}

// shared memory [phys, +len) -> guest RAM (the texture cache's window), one
// GPU sync. The CPU copies of those pages are then current again.
#if SR_ORACLE
void D3D12Backend::pm4ShmReadback(uint32_t phys, uint32_t len) {
  if (memres::resourceDecides())
    throw std::logic_error("native resource readers must use an owned CPU snapshot");
  if (!m_shmReady || !len) return;
  phys &= 0x1FFFFFFFu;
  const uint32_t start = phys & ~4095u;
  const uint32_t end = uint32_t(std::min<uint64_t>((uint64_t(phys) + len + 4095u) & ~4095ull, kShmBytes));
  const uint32_t bytes = end - start;
  if (bytes > kShmReadbackBytes) {
    static std::atomic<uint32_t> s_big{0};
    if (s_big.fetch_add(1) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PM4_SHM] readback of %u bytes exceeds the buffer - skipped",
                  bytes);
    return;
  }
  pm4ShmState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_commandList->CopyBufferRegion(m_shmReadback.Get(), 0, m_shm.Get(), start, bytes);
  const auto tf = std::chrono::steady_clock::now();
  pm4Flush();
  m_shmMsReadback +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tf).count();
  uint8_t* mapped = nullptr;
  const D3D12_RANGE rr{0, SIZE_T(bytes)};
  if (FAILED(m_shmReadback->Map(0, &rr, reinterpret_cast<void**>(&mapped))) || !mapped) return;
  size_t avail = 0;
  int win = 8;
  uint8_t* dst = pm4ShmGuestWritableWin(start, avail, win);
  if (dst && avail >= bytes) {
    std::memcpy(dst, mapped, bytes);
    // The frozen-pass A/B: a model-arm host reader made guest RAM hold GPU data (the native route never writes RAM).
    memres::resourceAbCount(memres::AbEvent::kModelReadbackPages, bytes >> 12);
    for (uint32_t p = start >> 12; p < (end >> 12); p++) {
      // The bytes written back are the guest's to read (a platform fact mem.resources keeps); the baseline the model
      // takes of them is a re-baseline decision of the meter's shm column.
      sr::native::meter::add(sr::native::meter::kShm);
      const uint64_t h = ShaderCache::hashUcode(dst + (size_t(p - (start >> 12)) << 12), 4096);
      // The window the bytes went to is the one whose baseline they now are.
      if (win == 0xA) {
        m_shmHashA[p] = h;
        m_shmSrcA[p] = 1;
      } else {
        m_shmHash8[p] = h;
      }
      m_shmFresh8[p] = m_shmFreshA[p] = 0u;  // iter-466 cp lane: written here, not under a protection
      m_shmEpochOf[p] = m_rtOn ? m_shmEpochFrameStart : m_shmEpoch;  // iter-465 perf2 (see pm4ShmRequest)
#if SR_ORACLE
      if (memres::census()) memres::rebasePage(p, m_shmEpochOf[p], !m_rtOn);  // mem.resources step 2a: its stamp
#endif
      m_shmGpuWritten[p] = 0;
    }
  }
  const D3D12_RANGE wr{0, 0};
  m_shmReadback->Unmap(0, &wr);
}

// A CPU reader of guest memory (a texture the GPU loader does not handle, the
// scan-out, the replay's frame dump) is about to read [phys, +len): bring the
// GPU-written pages in it back into guest RAM first. No-op when none are.
void D3D12Backend::pm4ShmSyncToGuest(uint32_t phys, uint32_t len) {
  if (memres::resourceDecides())
    throw std::logic_error("native resource readers must use an owned CPU snapshot");
  if (!m_shmReady || !len) return;
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  uint32_t first = UINT32_MAX, last = 0;
  for (uint32_t p = p0; p <= p1; p++)
    // iter-464 resscale: a page a SCALED resolve wrote holds no GPU data in the
    // 1x shared memory (Xenia skips the readback when scaled,
    // d3d12_command_processor.cc:3096) - guest RAM keeps the CPU's bytes.
    if (m_shmGpuWritten[p] && !(m_srReady && m_srPages[p])) {
      first = std::min(first, p);
      last = p;
    }
  if (first == UINT32_MAX) return;
  nbCpuRead("sync", phys, len, last - first + 1u);  // iter-467 native-b census
  // Outside a frame (the replay dumps after the present) the list is closed:
  // open it on this frame's allocator (moveToNextFrame waited for it) and
  // leave it closed again, as found.
  const bool wasClosed = m_clState != ClState::Recording;
  if (wasClosed) clReset(Pm4Reset);
  const uint32_t maxPages = uint32_t(kShmReadbackBytes >> 12);
  if (!m_srReady) {
    for (uint32_t p = first; p <= last; p += maxPages) {
      const uint32_t n = std::min(maxPages, last - p + 1u);
      pm4ShmReadback(p << 12, n << 12);
    }
  } else {
    // iter-464 resscale: runs of read-back-able pages only (never a scaled page).
    for (uint32_t p = first; p <= last;) {
      if (!m_shmGpuWritten[p] || m_srPages[p]) { p++; continue; }
      uint32_t n = 1;
      while (p + n <= last && n < maxPages && m_shmGpuWritten[p + n] && !m_srPages[p + n]) n++;
      pm4ShmReadback(p << 12, n << 12);
      p += n;
    }
  }
  if (wasClosed) clClose(Pm4Close);
  m_shmSyncs++;
}

#endif
void D3D12Backend::resourceCpuSnapshot(uint32_t phys, uint32_t len, std::vector<uint8_t>& out) {
  if (!memres::resourceDecides())
    throw std::runtime_error("native CPU snapshot has no valid resource range");
  resourcePrivateSnapshot(phys, len, out);
}

// Shared byte-copy primitive: the native consumer and an explicitly enabled
// Oracle observation both read the recorded GPU bytes into private storage.
// Callers retain their admission policy; no guest CPU bytes are overwritten.
void D3D12Backend::resourcePrivateSnapshot(uint32_t phys, uint32_t len, std::vector<uint8_t>& out) {
  if (!m_shmReady || !len || uint64_t(phys) + len > kShmBytes)
    throw std::runtime_error("native CPU snapshot has no valid resource range");
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  const uint32_t first = phys >> 12, last = (phys + len - 1u) >> 12;
  for (uint32_t page = first; page <= last; ++page)
    if (m_srReady && m_srPages[page])
      throw std::runtime_error("native CPU snapshot of scaled storage needs a typed readback");
  // Read the arena in recorded draw order, not the guest's potentially newer
  // bytes. The caller retains its own result through decoding or frame export.
  std::vector<uint8_t> captured(len);
  const bool wasClosed = m_clState != ClState::Recording;
  if (wasClosed) clReset(Pm4Reset);
  for (uint32_t offset = 0; offset < len;) {
    const uint32_t bytes = std::min<uint32_t>(uint32_t(kShmReadbackBytes), len - offset);
    pm4ShmState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->CopyBufferRegion(m_shmReadback.Get(), 0, m_shm.Get(), uint64_t(phys) + offset, bytes);
    pm4Flush();
    uint8_t* mapped = nullptr;
    const D3D12_RANGE read{0, SIZE_T(bytes)};
    if (FAILED(m_shmReadback->Map(0, &read, reinterpret_cast<void**>(&mapped))) || !mapped)
      throw std::runtime_error("native CPU snapshot readback mapping failed");
    std::memcpy(captured.data() + offset, mapped, bytes);
    const D3D12_RANGE written{0, 0};
    m_shmReadback->Unmap(0, &written);
    offset += bytes;
  }
  if (wasClosed) clClose(Pm4Close);
  out.swap(captured);
}

#if SR_ORACLE
// One exact shader/texture observation, queued beside the real draw. This
// copies existing resources only: no upload, materialization, flush or wait.
bool D3D12Backend::resourceTextureProbe(const DrawBatch& b, const TranslatedConstantSnapshot& constants) {
  static const bool enabled = [] {
    const char* e = std::getenv("SR_MEM_RES_TEXTURE_PROBE");
    return e && std::strcmp(e, "1") == 0;
  }();
  if (!enabled || m_resourceTextureProbeUsed || b.vsHash != 0xCE2D505A0E7B9767ull ||
      b.psHash != 0x38392DB754F93075ull) return false;
  if (b.drawOrdinal != 41u) nativeRecordingFail("resource-texture-probe-ordinal", E_INVALIDARG);
  constexpr uint32_t expectedFetch[6] = {0x85024802, 0x0B5E1458, 0x0C4FE27F, 0x01001400, 0, 0x00000218};
  if (std::memcmp(b.texFetch, expectedFetch, sizeof(expectedFetch)) != 0)
    nativeRecordingFail("resource-texture-probe-fetch", E_INVALIDARG);
  if (!constants || !m_shmReady || !m_xlatCBPtr || b.sharedCbOff > m_xlatConstants.size() ||
      512u > m_xlatConstants.size() - b.sharedCbOff)
    nativeRecordingFail("resource-texture-probe-input", E_INVALIDARG);
  // step 6 I3-B (bindless): the bound shared block is the planner's own bytes, so the staged word equals it.
  uint32_t planning = 0, bound = 0;
  std::memcpy(&planning, m_xlatCBPtr + b.sharedCbOff + 16u * 4u, 4);
  std::memcpy(&bound, constants.storage().bytes().data() + constants.offset(2) + 16u * 4u, 4);
  const uint32_t slot = planning & 0x1FFFFFFFu;
  if ((planning & 0xE0000000u) != 0xC0000000u || bound != planning ||
      slot >= m_xlatViews.size()) nativeRecordingFail("resource-texture-probe-selected-index", E_INVALIDARG);
  const auto& view = m_xlatViews[slot];
  if (!view.resource || view.buffer || view.srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY ||
      view.srv.Format != DXGI_FORMAT_R16_UNORM || view.srv.Texture2DArray.FirstArraySlice != 0 ||
      view.srv.Texture2DArray.ArraySize != 4 || view.srv.Texture2DArray.MostDetailedMip != 0 ||
      view.srv.Texture2DArray.MipLevels != 1)
    nativeRecordingFail("resource-texture-probe-selected-view", E_INVALIDARG);
  const auto desc = view.resource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != 640 || desc.Height != 640 ||
      desc.DepthOrArraySize != 4 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
      (desc.Format != DXGI_FORMAT_R16_UNORM && desc.Format != DXGI_FORMAT_R16_TYPELESS))
    nativeRecordingFail("resource-texture-probe-shape", E_INVALIDARG);
  const char* dir = std::getenv("SR_MEM_RES_TEXTURE_PROBE_DIR");
  const DWORD attrs = dir ? GetFileAttributesA(dir) : INVALID_FILE_ATTRIBUTES;
  if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    nativeRecordingFail("resource-texture-probe-directory", E_INVALIDARG);
  auto p = std::make_unique<ResourceTextureProbe>();
  p->path = std::string(dir) + (memres::resourceDecides() ? "/consumer41_native.bin" : "/consumer41_default.bin");
  if (GetFileAttributesA(p->path.c_str()) != INVALID_FILE_ATTRIBUTES)
    nativeRecordingFail("resource-texture-probe-existing-output", E_INVALIDARG);
  p->texture = view.resource; p->arena = m_shm; p->constants = constants.storage();
  p->ordinal = b.drawOrdinal; p->slot = slot; p->mapping = view.srv.Shader4ComponentMapping;
  p->format = uint32_t(desc.Format);
  constexpr uint32_t span = 4u * 640u * 640u * 2u;
  UINT rows[4]{}; UINT64 rowBytes[4]{}, footprintBytes = 0;
  m_device->GetCopyableFootprints(&desc, 0, 4, span, p->footprints.data(), rows, rowBytes, &footprintBytes);
  uint64_t end = span;
  for (uint32_t i = 0; i < 4; ++i) {
    if (rows[i] != 640 || rowBytes[i] != 1280 || p->footprints[i].Footprint.Depth != 1)
      nativeRecordingFail("resource-texture-probe-footprint", E_INVALIDARG);
    end = std::max(end, p->footprints[i].Offset + uint64_t(p->footprints[i].Footprint.RowPitch) * rows[i]);
  }
  p->constantOffset = (end + 511u) & ~511ull;
  if (!p->constants.size() || p->constants.size() > 16384)
    nativeRecordingFail("resource-texture-probe-constants", E_INVALIDARG);
  p->total = p->constantOffset + p->constants.size();
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = p->total; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const HRESULT created = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p->readback));
  if (FAILED(created)) nativeRecordingFail("resource-texture-probe-readback", created);
  const auto arenaState = m_shmState;
  pm4ShmState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_commandList->CopyBufferRegion(p->readback.Get(), 0, p->arena.Get(), 0x0B5E1000u, span);
  pm4ShmState(arenaState);
  pm4ShmBarrier(m_commandList.Get(), p->texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (uint32_t layer = 0; layer < 4; ++layer) {
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = p->readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = p->footprints[layer];
    src.pResource = p->texture.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = layer;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  pm4ShmBarrier(m_commandList.Get(), p->texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_commandList->CopyBufferRegion(p->readback.Get(), p->constantOffset, p->constants.resource(),
                                  p->constants.offset(), p->constants.size());
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_TEX] queued=%u slot=%u bound=%08X mapping=%u format=%u raw_span=%u "
      "constant_bytes=%llu constant_offsets=%u,%u,%u,%u completion=unconfirmed",
      p->ordinal, slot, bound, p->mapping, p->format, span, (unsigned long long)p->constants.size(),
      constants.offset(0), constants.offset(1), constants.offset(2), constants.offset(3));
  m_resourceTextureProbe = std::move(p); m_resourceTextureProbeUsed = true;
  return true;
}
void D3D12Backend::resourceTextureProbeSubmitted(uint64_t ticket) {
  if (!m_resourceTextureProbe || m_resourceTextureProbe->ticket) return;
  if (!m_resourceTextureProbe->recorded || !ticket)
    nativeRecordingFail("resource-texture-probe-unrecorded-submit", E_UNEXPECTED);
  m_resourceTextureProbe->ticket = ticket;
}
void D3D12Backend::resourceTextureProbeDiscard() {
  if (m_resourceTextureProbe && !m_resourceTextureProbe->ticket) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES_TEX] unconfirmed=%u reason=discarded",
                m_resourceTextureProbe->ordinal);
    m_resourceTextureProbe.reset();
  }
}
void D3D12Backend::resourceTextureProbeCollect() {
  if (!m_resourceTextureProbe || !m_resourceTextureProbe->ticket) return;
  const auto& p = *m_resourceTextureProbe;
  const HRESULT complete = m_nativeImages.wait(p.ticket, 0);
  if (complete == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) return;
  if (FAILED(complete)) nativeRecordingFail("resource-texture-probe-completion", complete);
  uint8_t* data = nullptr;
  const D3D12_RANGE read{0, SIZE_T(p.total)};
  const HRESULT mapped = p.readback->Map(0, &read, reinterpret_cast<void**>(&data));
  if (FAILED(mapped) || !data) nativeRecordingFail("resource-texture-probe-map", FAILED(mapped) ? mapped : E_POINTER);
  constexpr uint32_t span = 4u * 640u * 640u * 2u;
  std::vector<uint8_t> output(size_t(2u * span + p.constants.size()));
  std::memcpy(output.data(), data, span);
  for (uint32_t layer = 0; layer < 4; ++layer)
    for (uint32_t row = 0; row < 640; ++row)
      std::memcpy(output.data() + span + layer * 819200u + row * 1280u,
          data + p.footprints[layer].Offset + uint64_t(row) * p.footprints[layer].Footprint.RowPitch, 1280);
  std::memcpy(output.data() + 2u * span, data + p.constantOffset, size_t(p.constants.size()));
  const D3D12_RANGE noWrite{0, 0}; p.readback->Unmap(0, &noWrite);
  HANDLE file = CreateFileA(p.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) nativeRecordingFail("resource-texture-probe-file", HRESULT_FROM_WIN32(GetLastError()));
  DWORD written = 0;
  const BOOL wrote = WriteFile(file, output.data(), DWORD(output.size()), &written, nullptr);
  const DWORD error = wrote ? ERROR_WRITE_FAULT : GetLastError();
  const BOOL closed = CloseHandle(file);
  const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
  if (!wrote || written != output.size() || !closed)
    nativeRecordingFail("resource-texture-probe-write", HRESULT_FROM_WIN32(!closed ? closeError : error));
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_TEX] completed=%u ticket=%llu recorded=1 arena_offset=0 arena_bytes=%u "
      "array_offset=%u array_bytes=%u constants_offset=%u constant_bytes=%llu file=%s",
      p.ordinal, (unsigned long long)p.ticket, span, span, span, 2u * span,
      (unsigned long long)p.constants.size(), p.path.c_str());
  m_resourceTextureProbe.reset();
}

bool D3D12Backend::resourceValidateConsumer(const DrawBatch& batch) {
  if (batch.resourceConsumerChecks.empty()) return false;
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  memres::FetchAnalysis analysis;
  analysis.reason = "consumer_inputs_missing";
  bool flushed = false;
  if (batch.resourceConsumerFetch && batch.vsfShm) {
    const auto span = memres::requestedFetchF1(*batch.resourceConsumerFetch);
    memres::FetchFrozenSource source;
    source.drawId = batch.resourceConsumerId;
    source.phys = span.phys;
    std::vector<uint8_t> frozen;
    bool valid = m_shmReady && span.len <= 1024u * 1024u && uint64_t(span.phys) + span.len <= kShmBytes;
    if (valid && span.len) {
      for (uint32_t p = span.phys >> 12; p <= (span.phys + span.len - 1u) >> 12; ++p)
        if (m_srReady && m_srPages[p]) valid = false;
      if (valid) {
        source.recordedBefore = uint64_t(m_shmStampClock) + 1u;
        resourcePrivateSnapshot(span.phys, span.len, frozen);
        flushed = true;
        source.recordedAfter = uint64_t(m_shmStampClock) + 1u;
        source.bytes = frozen.data();
        source.byteCount = frozen.size();
        source.complete = true;
      }
    }
    analysis = memres::analyzeFetchDraw(*batch.resourceConsumerFetch, source);
    auto actual = resourceCurrentFetchInput(batch);
    actual.drawId = batch.resourceConsumerId;
    actual.constantBlocks = batch.resourceConsumerConstantBlocks;
    if (!valid || !memres::confirmFetchDraw(*batch.resourceConsumerFetch, actual)) {
      analysis.complete = false;
      analysis.reason = valid ? "consumer_submitted_inputs_changed" : "consumer_f1_storage_unknown";
      analysis.uses.clear();
    }
  }
  bool passed = true;
  for (const auto& check : batch.resourceConsumerChecks) {
    const uint32_t page = check->observed.page;
    const auto recorded = page < m_resourceRecordedBaselines.size() ? m_resourceRecordedBaselines[page] : nullptr;
    const auto result = memres::resourceConsumerValidate(check, analysis, recorded, resourceConsumerWitness(page));
    if (!result.passed()) {
      passed = false;
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[MEM_RES_CHECK] consumer_rejected=1 draw=%llu page=%08X reason=%s changed=%u matched=%u",
          (unsigned long long)batch.resourceConsumerId, page << 12, result.reason,
          result.changedWords, result.matchedWords);
    }
  }
  if (!passed) throw std::runtime_error("native consumer check refused incomplete or stale draw");
  return flushed;
}

bool D3D12Backend::resourceInspectFetch(const DrawBatch& batch, memres::FetchAnalysis* output,
                                      memres::FetchPayloadIntersection* payloadOutput) {
  if (!batch.resourceFetchCapture) {
    if (payloadOutput) { *payloadOutput = {}; payloadOutput->reason = "payload_not_captured"; }
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  const auto ownerBefore = sr::kernel::physwin::ownerVersion(batch.resourceFetchTarget, 4);
  const auto pageOwnerBefore = batch.resourceFetchPayload
      ? sr::kernel::physwin::ownerVersion(batch.resourceFetchPayload->page, 4096)
      : decltype(ownerBefore){};
  const auto span = memres::requestedFetchF1(*batch.resourceFetchCapture);
  memres::FetchFrozenSource source;
  source.drawId = batch.resourceFetchId;
  source.phys = span.phys;
  std::vector<uint8_t> frozen;
  bool flushed = false;
  std::string unavailable;
  if (!batch.vsfShm) unavailable = "staged_vertex_source_unsupported";
  else if (span.len) {
    bool scaled = false;
    if (uint64_t(span.phys) + span.len <= kShmBytes)
      for (uint32_t page = span.phys >> 12; page <= (span.phys + span.len - 1u) >> 12; ++page)
        if (m_srReady && m_srPages[page]) scaled = true;
    if (!m_shmReady || span.len > 1024u * 1024u || uint64_t(span.phys) + span.len > kShmBytes)
      unavailable = "invalid_f1_storage";
    else if (scaled) unavailable = "scaled_f1_storage";
    else {
      source.recordedBefore = uint64_t(m_shmStampClock) + 1u;
      resourcePrivateSnapshot(span.phys, span.len, frozen);
      flushed = true;
      source.recordedAfter = uint64_t(m_shmStampClock) + 1u;
      source.bytes = frozen.data();
      source.byteCount = frozen.size();
      source.complete = true;
    }
  }
  auto result = memres::analyzeFetchDraw(*batch.resourceFetchCapture, source);
  if (result.complete && !memres::confirmFetchDraw(*batch.resourceFetchCapture, resourceCurrentFetchInput(batch)))
    unavailable = "submitted_inputs_changed";
  const auto grants = sr::kernel::physwin::ownersOfSpan(batch.resourceFetchTarget, 4);
  const auto ownerAfter = sr::kernel::physwin::ownerVersion(batch.resourceFetchTarget, 4);
  if (!batch.resourceFetchOwnerKnown || !ownerBefore.ok || !ownerAfter.ok ||
      batch.resourceFetchOwnerSum != ownerBefore.sum || batch.resourceFetchOwnerSeq != ownerBefore.cbSeq ||
      ownerBefore.sum != ownerAfter.sum || ownerBefore.cbSeq != ownerAfter.cbSeq)
    unavailable = "target_owner_changed_or_unknown";
  if (grants.nLive != batch.resourceFetchGrantCount || grants.setDigest != batch.resourceFetchGrantDigest ||
      grants.gen != batch.resourceFetchGrantGeneration ||
      !std::equal(grants.serial, grants.serial + 2, batch.resourceFetchGrantSerial))
    unavailable = "target_grants_changed";
  if (!batch.resourceFetchOriginMatched) unavailable = "selected_miss_origin_changed_or_unknown";
  if (!unavailable.empty()) {
    result.complete = false;
    result.reason = unavailable;
    result.uses.clear();
  }
  uint32_t matches = 0, firstInstruction = 0, firstSlot = 0, firstClass = 0;
  for (const auto& use : result.uses) {
    if (uint64_t(batch.resourceFetchTarget) + 4u <= use.phys ||
        uint64_t(use.phys) + use.len <= batch.resourceFetchTarget) continue;
    if (!matches) { firstInstruction = use.instruction; firstSlot = use.slot; firstClass = use.cls; }
    ++matches;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_FETCH] analyzed=%llu ordinal=%u vs=%016llX target=%08X complete=%u reason=%s "
      "candidate_matches=%u first_instruction=%u first_slot=%u first_class=%u spans=%zu "
      "f1=%08X+%X version=%llu/%llu owner=%llu/%u/%u scope=sampled_draw_candidates epoch_coverage=unknown",
      (unsigned long long)batch.resourceFetchId, batch.drawOrdinal, (unsigned long long)batch.vsHash,
      batch.resourceFetchTarget, result.complete ? 1u : 0u, result.reason.empty() ? "none" : result.reason.c_str(),
      matches, firstInstruction, firstSlot, firstClass, result.uses.size(), span.phys, span.len,
      (unsigned long long)source.recordedBefore, (unsigned long long)source.recordedAfter,
      (unsigned long long)ownerAfter.sum, ownerAfter.cbSeq, ownerAfter.ok ? 1u : 0u);
  if (batch.resourceFetchPayload) {
    const auto& payload = *batch.resourceFetchPayload;
    const auto pageGrants = sr::kernel::physwin::ownersOfSpan(payload.page, 4096);
    const auto pageOwnerAfter = sr::kernel::physwin::ownerVersion(payload.page, 4096);
    auto joined = memres::intersectFetchPayload(payload, result);
    const bool payloadPageIdentityChanged = !pageOwnerBefore.ok || !pageOwnerAfter.ok ||
        pageOwnerBefore.sum != payload.current.ownerSum || pageOwnerAfter.sum != payload.current.ownerSum ||
        pageOwnerBefore.cbSeq != payload.current.ownerSeq || pageOwnerAfter.cbSeq != payload.current.ownerSeq ||
        pageGrants.nLive != payload.grantCount || pageGrants.setDigest != payload.grantDigest ||
        pageGrants.gen != payload.current.grantGeneration ||
        !std::equal(pageGrants.serial, pageGrants.serial + 2, payload.grantSerial);
    if (payloadPageIdentityChanged) {
      joined.complete = false;
      joined.reason = "payload_page_identity_changed";
      joined.candidateWords = 0;
      joined.candidateMask.fill(0);
      for (auto& word : joined.changedWords) word.candidate = false;
    }
    // One full-page membership bitmap per sampled draw joins EVERY changed
    // word from the frozen origin. It cannot establish the causing draw's
    // membership or turn missing/changed provenance into an exclusion claim.
    char mask[16 * 16 + 1]{};
    for (size_t i = 0; i < joined.candidateMask.size(); ++i)
      std::snprintf(mask + i * 16, 17, "%016llX", (unsigned long long)joined.candidateMask[i]);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_FETCH] payload_join=%llu origin_sequence=%llu page=%08X complete=%u reason=%s "
        "changed_words=%zu candidate_words=%u candidate_mask=%s mask_order=ascending_64_dwords_lsb_first "
        "scope=origin_words_in_later_draw_candidates epoch_coverage=unknown",
        (unsigned long long)batch.resourceFetchId, (unsigned long long)batch.resourceFetchOriginSequence,
        payload.page, joined.complete ? 1u : 0u, joined.reason.c_str(), joined.changedWords.size(),
        joined.candidateWords, mask);
    if (payloadOutput) *payloadOutput = std::move(joined);
  } else if (payloadOutput) {
    *payloadOutput = {};
    payloadOutput->reason = "payload_not_captured";
  }
  if (output) *output = std::move(result);
  return flushed;
}

void D3D12Backend::replayPoisonResource(uint32_t phys, uint32_t len) {
  if ((!memres::resourceMode() && !replayOrderedRestores()) || !len) return;
  phys &= 0x1FFFFFFFu;
  if (uint64_t(phys) + len > kShmBytes || (phys & 3u) || (len & 3u))
    throw std::runtime_error("replay resource poison has an invalid framebuffer range");
  std::lock_guard<std::recursive_mutex> lock(m_shmMu);
  // The replay is synchronous; an asynchronous submitted frame owns different
  // uploads and cannot be safely rewritten by this oracle operation.
  if (m_rtOn) throw std::runtime_error("native replay poison requires synchronous rendering");
  const auto poison = [](uint8_t* dst, uint64_t bytes) {
    for (uint64_t i = 0; i < bytes; i += 4u) {
      dst[i] = 0xFF; dst[i + 1u] = 0; dst[i + 2u] = 0xFF; dst[i + 3u] = 0xFF;
    }
  };
  // Replay restores and consumes the 0x8 window synchronously. Validate the
  // pre-write bytes before poisoning, so a partial write cannot conceal an
  // unrelated missing boundary elsewhere on that page.
  auto* guestPhys = sr::kernel::guestVAToHost(0x80000000u);
  if (memres::resourceMode()) {
    for (uint32_t page = phys >> 12; page <= (uint64_t(phys) + len - 1u) >> 12; ++page)
      if (memres::resourceMemory().sourceWindow(page, 8) != 8u)
        throw std::runtime_error("replay resource poison has a conflicting CPU source window");
    if (!guestPhys || !memres::resourceCheckReplayPoison(phys, len, guestPhys))
      throw std::runtime_error("replay resource poison lacks valid checker coverage");
  }
  if (!guestPhys)
    throw std::runtime_error("replay resource poison lacks valid checker coverage");
  poison(guestPhys + phys, len);
  // Also cover immutable images that have not reached their upload third yet.
  for (auto& [page, capture] : m_replayRestores) {
    const uint64_t begin = std::max<uint64_t>(phys, uint64_t(page) << 12);
    const uint64_t end = std::min<uint64_t>(uint64_t(phys) + len, uint64_t(page + 1u) << 12);
    if (begin < end) poison(capture.bytes.data() + begin - (uint64_t(page) << 12), end - begin);
  }
  if (!m_shmReady) return;  // Later drain captures the already poisoned guest bytes.
  // CP drains may already have copied Xenia's final pixels into staging.
  // Poison their overlapping bytes too, or those later uploads undo the
  // initial poison and make a missing/partial resolve look correct.
  for (const ShmPend& entry : m_shmPending) {
    const uint64_t begin = std::max<uint64_t>(phys, entry.shmOff);
    const uint64_t end = std::min<uint64_t>(uint64_t(phys) + len, entry.shmOff + entry.bytes);
    if (begin >= end) continue;
    if (entry.done) throw std::runtime_error("replay resource poison found an already recorded upload");
    uint8_t* mapped = m_shmUploadPtr;
    if (entry.src) {
      mapped = nullptr;
      for (const ShmSpill& spill : m_shmSpill)
        if (spill.res.Get() == entry.src) { mapped = spill.ptr; break; }
      if (!mapped) throw std::runtime_error("replay resource poison cannot locate an owned spill");
    }
    poison(mapped + entry.arenaOff + begin - entry.shmOff, end - begin);
  }
  uint64_t offset = 0;
  ID3D12Resource* source = nullptr;
  uint8_t* mapped = shmSpillAlloc(m_shmDrainThird, len, offset, source);
  if (!mapped) throw std::runtime_error("replay resource poison cannot allocate staging");
  poison(mapped, len);
  m_shmPending.insert(m_shmPending.begin(), ShmPend{phys, offset, len, false, source});
  for (DrawBatch& batch : m_drawBatches) {
    ++batch.shmPendBegin;
    ++batch.shmPendEnd;
  }
}
#endif

// Returns false when the resolve is not one the GPU path handles (the caller
// runs the CPU resolve). extStart/extLen: the destination range written.
std::string nbNatStatus();  // below; the census summary of native_targets.inc prints it
#include "native_resolve.inc"
#include "tex_load_direct.inc"  // iter-466 res4k round 2
#include "native_targets.inc"   // iter-467 native-b (strand B): census, native targets

// iter-467 native-b (EDRAM zero): the native resolve path's state for [NB_EDRAM_RESOLVE] (d3d12_frame_render.cpp) and
// [NATIVE_RESOLVE_BAD]; release/emu-stage3: the refusals by reason and the last one (Astra round 1 P2).
std::string nbNatStatus() {
  char b[400];
  int n = std::snprintf(b, sizeof b,
                        "native ready=%d started=%d failed=%d done=%llu fallback warm=%llu other=%llu depth=%llu | "
                        "refused",
                        g_nat.ready.load() ? 1 : 0, g_nat.started.load() ? 1 : 0, g_nat.failed.load() ? 1 : 0,
                        (unsigned long long)g_nat.done, (unsigned long long)g_nat.fallbackWarm,
                        (unsigned long long)g_nat.fallbackOther, (unsigned long long)g_nat.fallbackDepth);
  for (uint32_t r = 0; r < kNatRefuseCount && n > 0 && size_t(n) < sizeof b; r++)
    n += std::snprintf(b + n, sizeof b - size_t(n), " %s=%llu", kNatRefusalName[r],
                       (unsigned long long)g_nat.refused[r]);
  if (n > 0 && size_t(n) < sizeof b)
    std::snprintf(b + n, sizeof b - size_t(n), " last=%s | declined owned=%llu owners=%llu",
                  g_nat.lastRefusal < kNatRefuseCount ? kNatRefusalName[g_nat.lastRefusal] : "none",
                  (unsigned long long)g_nat.declinedOwned, (unsigned long long)g_nat.declinedOwners);
  return b;
}

// The frame renderer's census of the resolves the native kernel did not take (release/emu-stage3, Astra round 1 P1):
// owned = the kernel was asked for the span and refused (nbNatStatus names why; the development build's model took it, the
// release stops), else the owner selection kept the span from the kernel.
void natNoteDeclined(bool owned) {
  if (owned) g_nat.declinedOwned++;
  else g_nat.declinedOwners++;
}
uint64_t natDeclinedOwned() { return g_nat.declinedOwned; }
uint64_t natDeclinedOwners() { return g_nat.declinedOwners; }

bool D3D12Backend::pm4NativeResolve(const edram::ResolveInfo& ri, const edram::Regs& regs,
                                    const Pm4NatOwners& owners, uint32_t& extStart, uint32_t& extLen) {
  const edram::RtKey owner = owners.a, ownerB = owners.b;
  const bool mixed = !ownerB.empty();
  // The colour owner reads through t1, the depth owner's planes through t2.
  const edram::RtKey colourOwner = !owner.isDepth() ? owner : (mixed && !ownerB.isDepth() ? ownerB : edram::RtKey());
  const edram::RtKey depthOwner = owner.isDepth() ? owner : (mixed && ownerB.isDepth() ? ownerB : edram::RtKey());
  const resscale::Scale& rsc = resscale::get();
  const bool scaled = rsc.scaled();  // iter-466 res4k: SR_NATIVE_RESOLVE_SCALED
  // (iter-467 native-b: at a scale SR_NATIVE_RESOLVE_SCALED alone decides - with SR_NATIVE_RESOLVE default ON its =0
  // would otherwise no longer reach the scaled path)
  if (g_nat.failed.load(std::memory_order_acquire)) return natRefuse(kNatRefuseFailed);
  if (!(scaled ? natResolveScaledEnabled() : natResolveEnabled()) || !ri.copy) return natRefuse(kNatRefuseOff);
  if (ri.destArray) {  // copy_dest_array = 1: see kNatRefuseArrayDest (named, so a [NATIVE_RESOLVE_BAD] says what it was)
    static std::atomic<uint32_t> s_alog{0};
    if (s_alog.fetch_add(1) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_RESOLVE] array / volume destination (copy_dest_array=1, slice %u) key=%08X dest=%08X "
                  "rect=(%u,%u)-(%u,%u): not implemented in any resolve path (the development build's CPU resolve drops it "
                  "too); never seen from the title",
                  ri.destSlice, ri.srcKey.v, ri.destBase, ri.x0, ri.y0, ri.x1, ri.y1);
    return natRefuse(kNatRefuseArrayDest);
  }
#if SR_ORACLE  // C1 stage 3 (3.1 rule 2): the =0 arm is the EDRAM-path depth resolve of the oracle unit
  static const bool s_natDepth = [] {  // SR_NATIVE_RESOLVE_DEPTH=0 keeps depth on the EDRAM path
    const char* e = std::getenv("SR_NATIVE_RESOLVE_DEPTH");
    return !e || e[0] != '0';
  }();
  if ((ri.isDepth || !depthOwner.empty()) && !s_natDepth) return natRefuse(kNatRefuseDepth);
#endif
  // iter-466 res4k: scaled - only a same-shape single owner (the source key, or a surface with the
  // same sample grid and depth-ness), so every host RT pixel maps 1:1 to the scaled EDRAM pixel the
  // EDRAM path would read; the scaled resolve memory must exist.
  // iter-467 native-b (SR_NB_SCALED_REMAP): with the scaled tile dimensions in the kernel the 1x remap holds at
  // a scale too (natScaledRemapOn) - any owner shape and mixed spans, as at 1x.
  if (!pm4ShmInit()) return natRefuse(kNatRefuseFailed);
  if (scaled && (!m_srReady || (!natScaledRemapOn() &&
                                (mixed || owner.isDepth() != ri.srcKey.isDepth() ||
                                 (owner.msaa() >= edram::kMsaa4X) != (ri.srcKey.msaa() >= edram::kMsaa4X) ||
                                 (owner.msaa() >= edram::kMsaa2X) != (ri.srcKey.msaa() >= edram::kMsaa2X)))))
    return natRefuse(kNatRefuseScaled);
  // The 7 pipelines (natStartPipelines: pm4ShmInit started their build - the shipped blobs inline, else a compile).
  // Dev build: until they are ready every resolve takes the EDRAM path (SR_NATIVE_RESOLVE_SYNC=1, which the replay
  // sets, compiled inline). Release configuration (release/emu-stage3, Astra round 1 P2): nothing can take a resolve
  // meanwhile - wait for the build; only a failed build refuses ("failed", a [NATIVE_RESOLVE_BAD] that names it).
  if (!g_nat.ready.load(std::memory_order_acquire)) {
#if SR_ORACLE
    natStartPipelines(m_device.Get(), /*wait=*/false);
#else
    natStartPipelines(m_device.Get(), /*wait=*/true);
#endif
    if (g_nat.failed.load(std::memory_order_acquire)) return natRefuse(kNatRefuseFailed);
    if (!g_nat.ready.load(std::memory_order_acquire)) return natRefuse(kNatRefuseWarm);
  }
  // iter-467 native-b (SR_NB_NATIVE_PREHASH): the resolve constants are pure (buildResolve reads only ri / regs), so
  // they and the extent's rebaseline hashes come BEFORE m_shmMu, as pm4ShmResolve does (iter-466 hfr). Hashed under
  // the lock, ~200 pages x 2 windows per resolve kept the CP blocked in pm4ShmRequest: the live 1x in-roll A/B
  // (nb_n1 / nb_n2) had the native resolve at -13.7 fps, its guest render tick +1.0 ms, the CP's ib share up.
  XeResolveConsts c{};
  uint32_t gx = 0, gy = 0;
#if SR_ORACLE
  const bool prehash = nbNativePrehashOn();
  std::unique_lock<std::recursive_mutex> lkEarly(m_shmMu, std::defer_lock);
  if (!prehash) lkEarly.lock();
#endif
  const int shader = buildResolve(ri, regs, c, gx, gy, extStart, extLen, scaled ? rsc.x : 1u,
                                  scaled ? rsc.y : 1u);
#if SR_ORACLE
  struct PreDrop {
    D3D12Backend* b;
    ~PreDrop() { b->pm4ShmRebaseDrop(); }
  } preDrop{this};
  if (prehash && shader >= 0 && extLen && pm4ShmRebaseUnlockedEnabled()) pm4ShmRebaseHash(extStart, extLen);
#endif
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  const int kind = natKindForShader(shader);
#if SR_ORACLE  // C1 stage 3: a diagnostic refusal (the EDRAM path took the resolve) - the oracle unit's
  static const uint32_t s_kindMask = [] {  // diagnostic: SR_NATIVE_RESOLVE_KINDS=bitmask of NAT_KIND
    const char* e = std::getenv("SR_NATIVE_RESOLVE_KINDS");
    return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0x7Fu;
  }();
  if (kind >= 0 && !((s_kindMask >> kind) & 1u)) return natRefuse(kNatRefuseKind);
#endif
  // release/emu-stage3 (Astra round 1 P1): a rectangle whose origin lies past the first tile row / column
  // (x0 >= 80 samples, y0 >= 16 sample rows). buildResolve folds the whole tiles before the origin into the EDRAM
  // base tile (edram_offset_scaled keeps the in-tile offset only), which a host-RT read must add back - the kernel
  // reads the owner's RT at the true origin (gNatSrcFold, as the NAT_TEX path did). iter-466 res4k refused these
  // (the 2026-10-06 live 3x Fortress COSTUME VIEWER backdrop: a seam at the half-height line, read without the
  // fold) and the EDRAM path took them; the release configuration has no such path. The replay compare at 1x / 3x
  // and the difftest (SR_NAT_DIFFTEST, gfx/d3d12_edram_model.cpp) prove the fold against the model.
  const uint32_t lx = ri.msaa >= edram::kMsaa4X ? 1u : 0u, ly = ri.msaa >= edram::kMsaa2X ? 1u : 0u;
  const uint32_t foldX = (((ri.x0 << lx) / 80u) * 80u) >> lx, foldY = (((ri.y0 << ly) / 16u) * 16u) >> ly;
  static std::atomic<uint32_t> s_olog{0};
  if ((foldX || foldY) && s_olog.fetch_add(1) < 16)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_RESOLVE] rect origin (%u,%u)-(%u,%u) key=%08X past the first tile: native, source fold "
                "(%u,%u) px",
                ri.x0, ri.y0, ri.x1, ri.y1, ri.srcKey.v, foldX, foldY);
  // The owner's host RT holds the span's pixels; a source pixel reads the owner pixel under the
  // same sample (Xenos 2x = rows doubled, 4x = rows and columns; MSAA keys render 1x, replicated).
  const auto shX = [](edram::RtKey k) { return k.msaa() >= edram::kMsaa4X ? 1u : 0u; };
  const auto shY = [](edram::RtKey k) { return k.msaa() >= edram::kMsaa2X ? 1u : 0u; };
  const auto remapOf = [&](edram::RtKey o) {
    return shX(ri.srcKey) | (shY(ri.srcKey) << 4) | (shX(o) << 8) | (shY(o) << 12) |
           (o.isDepth() ? 0x10000u : 0u) | (o.isDepth() != ri.srcKey.isDepth() ? 0x20000u : 0u);
  };
  Pm4HostRt* rtColour = (kind >= 0 && !colourOwner.empty()) ? pm4Rt(colourOwner) : nullptr;
  Pm4HostRt* rt = (kind >= 0 && !depthOwner.empty()) ? pm4Rt(depthOwner) : nullptr;  // the depth owner
  if (kind < 0 || !gx || !gy || !extLen) return natRefuse(kNatRefuseShape);
  if ((!colourOwner.empty() && (!rtColour || rtColour->srv < 0)) || (!depthOwner.empty() && !rt))
    return natRefuse(kNatRefuseNoRt);
  // Depth: the surface's two copy planes into the scratch buffer (as the EDRAM store does).
  uint32_t depthC[24] = {};
  depthC[20] = scaled ? rsc.x : 1u;  // SR_NB_SCALED_REMAP: the kernel's scaled tile dimensions
  depthC[21] = scaled ? rsc.y : 1u;
  depthC[22] = foldX * (scaled ? rsc.x : 1u);  // gNatSrcFold: the folded origin, in scaled source pixels
  depthC[23] = foldY * (scaled ? rsc.y : 1u);
  depthC[6] = owner.format();
  depthC[7] = remapOf(owner);
  // The retained alias-pixel correspondence uses pitch, never the source base.
  depthC[11] = ri.srcKey.pitchTiles();
  if (mixed) {
    depthC[8] = ownerB.format();
    depthC[9] = remapOf(ownerB);
  }
  native_owner::Plan ownerPlan;
  if (!natBuildOwnerPlan(ri, owners.ranges, mixed ? owners.nRanges : 0u, c,
                         depthC[22], depthC[23], gy, scaled, ownerPlan))
    return natRefuse(kNatRefuseShape);
  // D24 UNORM sampling need not preserve every stored integer: the ROG probe
  // read 0x3ED9B476 for raw 0x006CDA3A, which reconstructs as 0x006CDA3B.
  // Read D24's exact copy planes instead. D24FS8 keeps the float SRV path;
  // select by the actual owner format, including a depth owner of a color alias.
  // gNatS1Bpt (depthC[4]) = 0 selects the SRVs (t3/t4) in the unchanged kernel.
#if SR_ORACLE
  static const bool s_depthSrv = [] {
    const char* e = std::getenv("SR_NB_DEPTH_SRV");
    return !e || e[0] != '0';  // float-depth copy/SRV diagnostic only
  }();
#else
  constexpr bool s_depthSrv = true;
#endif
  const bool depthViaSrv = !depthOwner.empty() && depthOwner.format() == 1u && s_depthSrv && rt->dsrv >= 0;
  if (depthViaSrv) {
    pm4Transition(*rt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  } else if (!depthOwner.empty()) {
    D3D12_RESOURCE_DESC desc = rt->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
    UINT rows[2] = {};
    UINT64 rowBytes[2] = {}, total = 0;
    m_device->GetCopyableFootprints(&desc, 0, 2, 0, fp, rows, rowBytes, &total);
    const uint32_t bpt0 = fp[0].Footprint.Width ? uint32_t(rowBytes[0] / fp[0].Footprint.Width) : 0u;
    const uint32_t bpt1 = fp[1].Footprint.Width ? uint32_t(rowBytes[1] / fp[1].Footprint.Width) : 0u;
    if (bpt0 != 4u || (bpt1 != 1u && bpt1 != 4u && bpt1 != 8u)) return natRefuse(kNatRefuseDepth);
    if (total > g_nat.depthScratchBytes) {
      if (g_nat.depthScratch) g_nat.graveyard.push_back(g_nat.depthScratch);
      g_nat.depthScratch.Reset();
      D3D12_HEAP_PROPERTIES hp{};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC bd{};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = total;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&g_nat.depthScratch)))) {
        g_nat.depthScratchBytes = 0;
        return natRefuse(kNatRefuseDepth);
      }
      g_nat.depthScratchBytes = total;
      g_nat.depthScratchState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (g_nat.depthScratchState != D3D12_RESOURCE_STATE_COPY_DEST) {
      pm4ShmBarrier(m_commandList.Get(), g_nat.depthScratch.Get(), g_nat.depthScratchState,
                    D3D12_RESOURCE_STATE_COPY_DEST);
      g_nat.depthScratchState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    pm4Transition(*rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT p = 0; p < 2; p++) {
      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
      dst.pResource = g_nat.depthScratch.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp[p];
      src.pResource = rt->res;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = p;
      m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    pm4ShmBarrier(m_commandList.Get(), g_nat.depthScratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_nat.depthScratchState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    depthC[0] = uint32_t(fp[0].Offset);
    depthC[1] = fp[0].Footprint.RowPitch;
    depthC[2] = uint32_t(fp[1].Offset);
    depthC[3] = fp[1].Footprint.RowPitch;
    depthC[4] = bpt1;
    depthC[5] = bpt1 == 1u ? 0u : (bpt1 == 4u ? 3u : 4u);  // the stencil byte (R8 / X24G8 / X32G8X24)
  }
  // NATIVE present: a fast 32bpp resolve of at least 320 pixels from the origin also
  // fills a linear front-buffer candidate (4 slots keyed by destination, LRU).
  natFrontInvalidate(extStart, extLen);
  NatFront* front = nullptr;
  if (!scaled && natPresentEnabled() && kind == 0 && !ri.destArray && ri.x0 == 0 && ri.y0 == 0 &&
      ri.x1 >= 320u && ri.destPitch && ri.destHeight) {
    const uint32_t dest = ri.destBase & 0x1FFFFFFFu;
    const uint64_t bytes = uint64_t(ri.destPitch) * ri.destHeight * 4u;
    for (NatFront& f : g_nat.front)
      if (f.buf && f.dest == dest) front = &f;
    if (!front) {
      front = &g_nat.front[0];
      for (NatFront& f : g_nat.front)
        if (f.lastUse < front->lastUse) front = &f;
    }
    if (bytes > (32ull << 20)) {
      front = nullptr;
    } else if (!front->buf || front->bytes < bytes) {
      if (front->buf) g_nat.graveyard.push_back(front->buf);
      front->buf.Reset();
      D3D12_HEAP_PROPERTIES hp{};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC bd{};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = bytes;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&front->buf)))) {
        front->buf.Reset();
        front = nullptr;
      } else {
        front->bytes = bytes;
        front->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      }
    }
    if (front) {
      if (front->state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        pm4ShmBarrier(m_commandList.Get(), front->buf.Get(), front->state,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        front->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      }
      front->dest = dest;
      front->span = uint32_t(bytes);
      front->pitch = ri.destPitch;
      front->rows = ri.destHeight;
      front->epoch = m_frameEpoch;
      front->lastUse = ++g_nat.frontUse;
      front->valid = true;
    }
  }
  // iter-466 res4k: scaled - the destination is the scaled resolve memory bound at
  // copy_dest_base * area (DestRelative constants), exactly as the scaled EDRAM resolve binds it;
  // the 1x shared memory is not written.
  const uint32_t extG = extStart & 0x1FFFFFFFu;
  D3D12_GPU_VIRTUAL_ADDRESS destVa = 0;
  if (scaled) {
    nbMaterialize(extG, extLen);  // iter-467 native-b B2: held memory back before the resolve writes over it
    const uint32_t destBase = c.destBase & 0x1FFFFFFFu;
    const uint32_t lo = std::min(destBase, extG);
    if (!srEnsure(lo, extG + extLen - lo)) return natRefuse(kNatRefuseMemory);
    srState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    destVa = m_srBuf->GetGPUVirtualAddress() + uint64_t(destBase) * rsc.area();
  } else {
    const memres::KindScope mrKind(memres::Kind::kResolve);  // mem.resources step 2a: the census's fetch kind
    pm4ShmRequest(extStart, extLen);
    pm4ShmState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    destVa = m_shm->GetGPUVirtualAddress();
  }
  D3D12_GPU_VIRTUAL_ADDRESS ownerAddress = 0;
  if (!uploadNativeOwnerPlan(ownerPlan, ownerAddress)) return natRefuse(kNatRefuseMemory);
  if (rtColour) pm4Transition(*rtColour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (depthViaSrv) keepNativeImage(rt->image);  // preparation above may flush/reopen the list
  pm4XlatHeaps();  // step 4 lane C 2i: this pass's tables are m_xlatSrvHeap's (ERROR 708 / GBV 935 without)
  m_commandList->SetComputeRootSignature(g_nat.rs.Get());
  m_commandList->SetPipelineState(g_nat.pso[kind].Get());
  m_commandList->SetComputeRoot32BitConstants(0, 5, &c, 0);
  m_commandList->SetComputeRootUnorderedAccessView(1, destVa);
  // A depth source reads t2; t1 still needs a valid descriptor (slot 0, as the store binds it).
  m_commandList->SetComputeRootDescriptorTable(2, pm4RtSrvGpu(rtColour ? uint32_t(rtColour->srv) : 0u));
  m_commandList->SetComputeRootShaderResourceView(
      5, (!depthOwner.empty() && !depthViaSrv) ? g_nat.depthScratch->GetGPUVirtualAddress()
                                               : m_shmUpload->GetGPUVirtualAddress());
  m_commandList->SetComputeRoot32BitConstants(6, 24, depthC, 0);
  m_commandList->SetComputeRootConstantBufferView(8, ownerAddress);
  // t3/t4: the depth owner's plane SRVs (or any two valid descriptors when unused)
  m_commandList->SetComputeRootDescriptorTable(7, pm4RtSrvGpu(depthViaSrv ? uint32_t(rt->dsrv) : 0u));
  if (!depthOwner.empty()) g_nat.depthDone++;
  m_commandList->SetComputeRootUnorderedAccessView(
      3, front ? front->buf->GetGPUVirtualAddress() : destVa);
  const uint32_t frontPitch = front ? front->pitch : 0u;
  m_commandList->SetComputeRoot32BitConstants(4, 1, &frontPitch, 0);
  m_commandList->Dispatch(gx, gy, 1);
  nbRemapWitness("N", ri, owner, ownerB);
  if (scaled) {
    // As the scaled EDRAM resolve: mark the pages scaled; texture versions move.
    m_srUavDirty = true;
    srMark(extG, extLen);
    pm4ShmRangeWrittenByGpu(extG, extLen);
    m_srResolves++;
    g_nat.scaledDone++;
  } else {
    m_shmUavDirty = true;
    pm4ShmRangeWrittenByGpu(extStart, extLen);
  }
  m_shmResolves++;
  const uint64_t n = ++g_nat.done;
  static bool s_depthLogged = false;
  if (!depthOwner.empty() && !s_depthLogged) {
    s_depthLogged = true;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_RESOLVE] first native DEPTH resolve: key=%08X cs=%d planes d0=+%u/%u s1=+%u/%u bpt=%u",
                ri.srcKey.v, shader, depthC[0], depthC[1], depthC[2], depthC[3], depthC[4]);
  }
  if (n <= 24 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_RESOLVE] #%llu cs=%d kind=%d key=%08X groups=%ux%u extent=%08X+%u (depth %llu; fallback depth=%llu "
                "other=%llu warm=%llu; scaled %llu)",
                (unsigned long long)n, shader, kind, ri.srcKey.v, gx, gy, extStart, extLen,
                (unsigned long long)g_nat.depthDone, (unsigned long long)g_nat.fallbackDepth,
                (unsigned long long)g_nat.fallbackOther, (unsigned long long)g_nat.fallbackWarm,
                (unsigned long long)g_nat.scaledDone);
  return true;
}

void D3D12Backend::pm4NativeFrontInvalidate(uint32_t start, uint32_t len) {
  natFrontInvalidate(start, len);
}

uint64_t D3D12Backend::pm4ShmHash(uint32_t start, uint32_t len, uint32_t* nonZero, bool* ok) {
  if (ok) *ok = false;
  start &= 0x1FFFFFFFu;
  if (!m_shmReady || !len || uint64_t(start) + len > kShmBytes) return 0;
  // release/emu-stage3 (Astra re-review 2): at a draw resolution scale a resolve writes the SCALED resolve memory
  // (m_srBuf, bound at guest address x area), never the 1x shared memory - the hash reads that range instead, so
  // SR_NATIVE_RESOLVE_VERIFY and the difftest compare the bytes a scaled resolve wrote (the 32 MiB readback bounds
  // the range: a larger extent is "not readable", which the verify counts as a MISMATCH).
  const resscale::Scale& rs = resscale::get();
  const uint64_t area = rs.scaled() ? uint64_t(rs.area()) : 1u;
  if (uint64_t(len) * area > kShmReadbackBytes) return 0;
  if (rs.scaled()) {
    if (!m_srReady || !srEnsure(start, len)) return 0;
    srState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->CopyBufferRegion(m_shmReadback.Get(), 0, m_srBuf.Get(), uint64_t(start) * area,
                                    uint64_t(len) * area);
  } else {
    pm4ShmState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->CopyBufferRegion(m_shmReadback.Get(), 0, m_shm.Get(), start, len);
  }
  pm4Flush();
  len = uint32_t(uint64_t(len) * area);
  uint8_t* h = nullptr;
  const D3D12_RANGE rr{0, SIZE_T(len)};
  if (FAILED(m_shmReadback->Map(0, &rr, reinterpret_cast<void**>(&h))) || !h) return 0;
  uint64_t x = 0xCBF29CE484222325ull;
  uint32_t nz = 0;
  for (uint32_t i = 0; i < len; i++) {
    x = (x ^ h[i]) * 0x100000001B3ull;
    nz += h[i] != 0;
  }
  const D3D12_RANGE wr{0, 0};
  m_shmReadback->Unmap(0, &wr);
  if (nonZero) *nonZero = nz;
  if (ok) *ok = true;
  return x;
}

#if SR_ORACLE  // C1 stage 3: SR_NATIVE_RESOLVE_HASH (a diagnostic) and the EDRAM-path resolve (the oracle unit's)
void D3D12Backend::pm4ResolveHashDebug(const edram::ResolveInfo& ri, uint32_t extStart, uint32_t extLen,
                                       bool native) {
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_RESOLVE_HASH");
    return e && e[0] == '1';
  }();
  if (!on || !extLen) return;
  const uint32_t start = extStart & 0x1FFFFFFFu;
  if (!m_shmReady || uint64_t(start) + extLen > kShmBytes || extLen > kShmReadbackBytes) return;
  pm4ShmState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_commandList->CopyBufferRegion(m_shmReadback.Get(), 0, m_shm.Get(), start, extLen);
  pm4Flush();
  uint8_t* h = nullptr;
  const D3D12_RANGE rr{0, SIZE_T(extLen)};
  if (FAILED(m_shmReadback->Map(0, &rr, reinterpret_cast<void**>(&h))) || !h) return;
  uint64_t x = 0xCBF29CE484222325ull;
  uint32_t nz = 0;
  for (uint32_t i = 0; i < extLen; i++) {
    x = (x ^ h[i]) * 0x100000001B3ull;
    nz += h[i] != 0;
  }
  const D3D12_RANGE wr{0, 0};
  m_shmReadback->Unmap(0, &wr);
  static uint32_t n = 0;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[RESOLVE_HASH] #%u %s key=%08X dest=%08X fmt=%u rect=(%u,%u)-(%u,%u) extent=%08X+%u nz=%u hash=%016llX",
              ++n, native ? "native" : "edram", ri.srcKey.v, ri.destBase, ri.destFormat, ri.x0, ri.y0, ri.x1, ri.y1,
              extStart, extLen, nz, (unsigned long long)x);
}

bool D3D12Backend::pm4ShmResolve(const edram::ResolveInfo& ri, const edram::Regs& regs,
                                 uint32_t& extStart, uint32_t& extLen) {
  if (!pm4ShmInit()) return false;
  sr::hitch::Scope hitchScope(sr::hitch::kResolve);
  // iter-466 hfr: the resolve constants are pure (buildResolve reads only ri / regs), so they
  // and the extent's rebaseline hashes (guest memory reads) come before m_shmMu: the CP's
  // drain blocked on this lock while the render thread hashed ~200 pages per resolve.
  XeResolveConsts c{};
  uint32_t gx = 0, gy = 0;
  const resscale::Scale& rs = resscale::get();
  const int shader = buildResolve(ri, regs, c, gx, gy, extStart, extLen, rs.x, rs.y);
  if (shader < 0 || !gx || !gy || !extLen) return false;
  struct PreDrop {
    D3D12Backend* b;
    ~PreDrop() { b->pm4ShmRebaseDrop(); }
  } preDrop{this};
  if (pm4ShmRebaseUnlockedEnabled()) pm4ShmRebaseHash(extStart, extLen);
  std::lock_guard<std::recursive_mutex> lk(m_shmMu);
  if (rs.scaled()) {
    // iter-464 resscale (d3d12_render_target_cache.cc:1351-1416): the *_scaled
    // resolve writes the scaled resolve memory, bound at copy_dest_base * area
    // (DestRelative constants, no dest_base); the 1x shared memory is NOT written.
    if (!m_srReady) return false;
    const uint32_t destBase = c.destBase & 0x1FFFFFFFu;
    const uint32_t ext = extStart & 0x1FFFFFFFu;
    nbMaterialize(ext, extLen);  // iter-467 native-b B2: held memory back before the resolve writes over it
    const uint32_t lo = std::min(destBase, ext);
    if (!srEnsure(lo, ext + extLen - lo)) return false;
    pm4GpuEdramState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    srState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_commandList->SetComputeRootSignature(m_srResolveRs.Get());
    m_commandList->SetPipelineState(m_srResolvePso[shader].Get());
    m_commandList->SetComputeRoot32BitConstants(0, 4, &c, 0);
    m_commandList->SetComputeRootUnorderedAccessView(
        1, m_srBuf->GetGPUVirtualAddress() + uint64_t(destBase) * rs.area());
    m_commandList->SetComputeRootShaderResourceView(2, m_pm4EdramGpu->GetGPUVirtualAddress());
    m_commandList->Dispatch(gx, gy, 1);
    m_srUavDirty = true;
    srMark(ext, extLen);
    // Texture versions move and the CPU copies are re-baselined (Xenia
    // MarkRangeAsResolved -> RangeWrittenByGpu).
    pm4ShmRangeWrittenByGpu(ext, extLen);
    m_shmResolves++;
    m_srResolves++;
    static std::atomic<uint32_t> s_slog{0};
    if (s_slog.fetch_add(1) < 24)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_SHM] SCALED resolve %ux%u cs=%d groups=%ux%u consts={%08X %08X %08X %08X} "
                  "base=%08X extent=%08X+%u",
                  rs.x, rs.y, shader, gx, gy, c.edramInfo, c.coordInfo, c.destInfo,
                  c.destCoordInfo, destBase, ext, extLen);
    return true;
  }
  // Partially covered pages must hold the CPU bytes first (xenia RequestRange).
  {
    const memres::KindScope mrKind(memres::Kind::kResolve);  // mem.resources step 2a: the census's fetch kind
    pm4ShmRequest(extStart, extLen);
  }
  pm4GpuEdramState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  pm4ShmState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  m_commandList->SetComputeRootSignature(m_shmResolveRs.Get());
  m_commandList->SetPipelineState(m_shmResolvePso[shader].Get());
  m_commandList->SetComputeRoot32BitConstants(0, 5, &c, 0);
  m_commandList->SetComputeRootUnorderedAccessView(1, m_shm->GetGPUVirtualAddress());
  m_commandList->SetComputeRootShaderResourceView(2, m_pm4EdramGpu->GetGPUVirtualAddress());
  m_commandList->Dispatch(gx, gy, 1);
  m_shmUavDirty = true;
  pm4ShmRangeWrittenByGpu(extStart, extLen);
  m_shmResolves++;
  static std::atomic<uint32_t> s_log{0};
  if (s_log.fetch_add(1) < 24)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_SHM] resolve cs=%d groups=%ux%u consts={%08X %08X %08X %08X %08X} "
                "extent=%08X+%u",
                shader, gx, gy, c.edramInfo, c.coordInfo, c.destInfo, c.destCoordInfo, c.destBase,
                extStart, extLen);
  if (shmReadbackEnabled()) pm4ShmReadback(extStart, extLen);
  return true;
}
#endif  // SR_ORACLE

// ── iter-464 resscale: scaled resolve memory ─────────────────────────────────
// Xenia d3d12_texture_cache.cc:926-1191 (scaled_resolve_buffers_, 16 MiB heaps,
// UpdateTileMappings) and texture_cache.cc:126-136, 268-298, 993-1042 (the page
// marks). One reserved buffer covers 512 MiB * area: root SRV/UAV bindings take a
// GPU virtual address, so no view has to fit a 4 GiB window (Xenia's 2 GiB
// per-GB buffers exist for its descriptor views).
bool D3D12Backend::srInit() {
  if (m_srReady) return true;
  if (m_srFailed) return false;
  auto fail = [&](const char* what, HRESULT hr) {
    m_srFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[RES_SCALE] scaled resolve memory: %s failed (0x%08X)",
                what, unsigned(hr));
    return false;
  };
  const resscale::Scale& rs = resscale::get();
  m_srBytes = kShmBytes * rs.area();
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = m_srBytes;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;  // buffers are row-major
  bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  m_srState = D3D12_RESOURCE_STATE_COMMON;
  HRESULT hr = m_device->CreateReservedResource(&bd, m_srState, nullptr, IID_PPV_ARGS(&m_srBuf));
  if (FAILED(hr)) return fail("CreateReservedResource", hr);
  m_srBuf->SetName(L"PM4_SCALED_RESOLVE");
  m_srHeaps.assign(size_t((m_srBytes + kSrHeapBytes - 1u) / kSrHeapBytes), nullptr);
  m_srPages.assign(kShmPages, 0u);
  m_srBlock.assign(kShmPages / 64u, 0u);
#if SR_ORACLE  // C1 stage 3: the scaled EDRAM-path resolve is the oracle unit's
  // Resolve root signature, scaled (d3d12_render_target_cache.cc:351-381):
  // {0: 4 constants (DestRelative), 1: UAV u0 scaled memory, 2: SRV t0 EDRAM}.
  {
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 4;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = p;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) return fail("resolve root signature (serialize)", hr);
    hr = m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                       IID_PPV_ARGS(&m_srResolveRs));
    if (FAILED(hr)) return fail("resolve root signature", hr);
  }
  struct Bc { const void* p; size_t n; };
  const Bc bcs[9] = {
      {xeshaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs, sizeof(xeshaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
      {xeshaders::resolve_fast_32bpp_4xmsaa_scaled_cs, sizeof(xeshaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
      {xeshaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs, sizeof(xeshaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
      {xeshaders::resolve_fast_64bpp_4xmsaa_scaled_cs, sizeof(xeshaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
      {xeshaders::resolve_full_8bpp_scaled_cs, sizeof(xeshaders::resolve_full_8bpp_scaled_cs)},
      {xeshaders::resolve_full_16bpp_scaled_cs, sizeof(xeshaders::resolve_full_16bpp_scaled_cs)},
      {xeshaders::resolve_full_32bpp_scaled_cs, sizeof(xeshaders::resolve_full_32bpp_scaled_cs)},
      {xeshaders::resolve_full_64bpp_scaled_cs, sizeof(xeshaders::resolve_full_64bpp_scaled_cs)},
      {xeshaders::resolve_full_128bpp_scaled_cs, sizeof(xeshaders::resolve_full_128bpp_scaled_cs)},
  };
  for (int i = 0; i < 9; i++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = m_srResolveRs.Get();
    cd.CS = {bcs[i].p, bcs[i].n};
    hr = m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_srResolvePso[i]));
    if (FAILED(hr)) return fail("scaled resolve pso", hr);
  }
#endif
  for (int i = 0; i < kLCount; i++) {  // the load root signature is the unscaled one
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = m_shmLoadRs.Get();
    cd.CS = {kLoadShadersScaled[i].bc, kLoadShadersScaled[i].n};
    hr = m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_srLoadPso[i]));
    if (FAILED(hr)) return fail("scaled texture load pso", hr);
  }
  m_srReady = true;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[RES_SCALE] scaled resolve memory ready: %llu MiB reserved (%ux%u), 16 MiB heaps on "
              "demand, resolve CS x9 + texture load CS x7 (_scaled)",
              (unsigned long long)(m_srBytes >> 20), rs.x, rs.y);
  return true;
}

bool D3D12Backend::srEnsure(uint32_t phys, uint32_t len) {
  if (!m_srReady || !len) return m_srReady;
  const uint64_t area = resscale::get().area();
  phys &= 0x1FFFFFFFu;
  const uint64_t first = uint64_t(phys) * area;
  const uint64_t last = std::min<uint64_t>((uint64_t(phys) + len) * area, m_srBytes) - 1u;
  const size_t h0 = size_t(first / kSrHeapBytes), h1 = size_t(last / kSrHeapBytes);
  for (size_t h = h0; h <= h1 && h < m_srHeaps.size(); h++) {
    if (m_srHeaps[h]) continue;
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = kSrHeapBytes;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    const HRESULT hr = m_device->CreateHeap(&hd, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) {
      static std::atomic<uint32_t> s_n{0};
      if (s_n.fetch_add(1) < 8)
        SR_LOG_BOOT(sr::LogSeverity::Error, "[RES_SCALE] scaled resolve heap %zu: CreateHeap 0x%08X",
                    h, unsigned(hr));
      return false;
    }
    // 64 KiB tiles: heap h backs tiles [h * 256, +256) of the buffer.
    constexpr UINT kTiles = UINT(kSrHeapBytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
    D3D12_TILED_RESOURCE_COORDINATE coord{};
    coord.X = UINT(h) * kTiles;
    D3D12_TILE_REGION_SIZE size{};
    size.NumTiles = kTiles;
    const D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;
    const UINT heapStart = 0, count = kTiles;
    m_commandQueue->UpdateTileMappings(m_srBuf.Get(), 1, &coord, &size, heap.Get(), 1, &flag,
                                       &heapStart, &count, D3D12_TILE_MAPPING_FLAG_NONE);
    m_srHeaps[h] = heap;
    m_srMappedBytes += kSrHeapBytes;
    static std::atomic<uint32_t> s_log{0};
    const uint32_t n = s_log.fetch_add(1) + 1;
    if (n <= 16 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[RES_SCALE] scaled resolve heap %zu mapped (guest %08X+%u), %llu MiB resident", h,
                  phys, len, (unsigned long long)(m_srMappedBytes >> 20));
  }
  return true;
}

void D3D12Backend::srState(D3D12_RESOURCE_STATES s) {
  if (!m_srBuf) return;
  if (m_srState == s) {
    if (s == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && m_srUavDirty) {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      b.UAV.pResource = m_srBuf.Get();
      m_commandList->ResourceBarrier(1, &b);
      m_srUavDirty = false;
    }
    return;
  }
  pm4ShmBarrier(m_commandList.Get(), m_srBuf.Get(), m_srState, s);
  m_srState = s;
  m_srUavDirty = false;
}

void D3D12Backend::srMark(uint32_t phys, uint32_t len) {
  if (!m_srReady || !len) return;
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  for (uint32_t p = p0; p <= p1; p++)
    if (!m_srPages[p]) {
      m_srPages[p] = 1;
      m_srBlock[p >> 6]++;
      m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO: a texture over it is a scaled one now
    }
}

void D3D12Backend::srClearPages(uint32_t page, uint32_t count) {
  if (!m_srReady) return;
  for (uint32_t p = page; p < page + count && p < kShmPages; p++)
    if (m_srPages[p]) {
      m_srPages[p] = 0;
      m_srBlock[p >> 6]--;
      m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO
    }
}

// An exact CPU-write patch on a page whose resolved bytes live in the scaled
// resolve memory: the scaled copy cannot take the patched dwords (its layout is
// the resolve's at the scale), so the page falls back to the arena's 1x bytes,
// which now hold them - the GPU's view stays the CPU's latest write. Counted:
// the header stamps are not expected on resolve destinations.
void D3D12Backend::resourcePatchScaled(uint32_t page) {
  if (!m_srReady || page >= kShmPages || !m_srPages[page]) return;
  srClearPages(page, 1);
  ++m_resourcePatchScaledDrops;
}

bool D3D12Backend::srAnyScaled(uint32_t phys, uint32_t len) const {
  if (!m_srReady || !len) return false;
  phys &= 0x1FFFFFFFu;
  const uint32_t p0 = phys >> 12;
  const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);
  for (uint32_t p = p0; p <= p1;) {
    if ((p & 63u) == 0u && p + 63u <= p1) {  // whole blocks: one counter
      if (m_srBlock[p >> 6]) return true;
      p += 64u;
      continue;
    }
    if (m_srPages[p]) return true;
    p++;
  }
  return false;
}

// ── Scan-out from the shared memory (xenia IssueSwap: the front buffer is a
// texture in shared memory, gamma-ramped into the host backbuffer) ────────────
// The resolved front buffer never has to come back to guest RAM. The PS reads
// the k_8_8_8_8 front buffer (tiled or linear) straight from the shared memory
// and applies the game's display gamma ramp (a 256-entry LUT built on the CPU
// from gamma::displayRamp - the same table the CPU scan-out used).
//
// iter-464 resscale (PORT FEATURE, docs/RESOLUTION.md): two passes.
//   1. PSOut: the front buffer at the INTERNAL resolution (guest size x scale)
//      into m_outTex - from the scaled resolve memory when a scaled resolve wrote
//      it (texture_address.xesli resolution-scaled addressing, 4 B/block), else
//      from the 1x shared memory (each guest pixel repeated scale x scale).
//   2. PSFit: m_outTex into the backbuffer, aspect preserved (letterbox /
//      pillarbox on black), 1:1 copy when the sizes match, bilinear when
//      enlarging, an exact area (box) filter when shrinking (e.g. 2x -> 1080p).
//      Xenia presenter.cc fits the same way (present_letterbox, default on); its
//      CAS/FSR sharpeners are not ported.
// At 1x in a 1280x720 window pass 2 is an exact copy: the presented pixels are
// the single-pass scan-out's.
namespace {
const char kShmPresentHlsl[] = R"HLSL(
cbuffer C : register(b0) {
  uint gFbOff; uint gPitch; uint gTiled; uint gW; uint gH;
  uint gScaleX; uint gScaleY; uint gScaledSrc;
};
ByteAddressBuffer gShm : register(t0);
ByteAddressBuffer gLut : register(t1);
Texture2D<uint> gFrontTex : register(t2);  // iter-467 native-b B4: a front target (the raw dwords, at the scale)
float4 VSFull(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2(float((id << 1) & 2u), float(id & 2u));
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
// xenos_tiling::tiled2D (Xenia texture_address Tiled2D), 32bpp.
uint Tiled2D32(uint x, uint y, uint pitch) {
  int outerBlocks = int(((y >> 5) * (pitch >> 5) + (x >> 5)) << 6);
  int innerBlocks = int((((y >> 1) & 7u) << 3) | (x & 7u));
  int oib = (outerBlocks | innerBlocks) << 2;
  uint bank = (y >> 4) & 1u;
  uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
  return uint(int(((y & 1u) << 4) | (pipe << 6) | (bank << 11)) | (oib & 15) |
              (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12));
}
// texture_address.xesli XeniaTextureGetResolutionScaledAddressing, 4 bytes per
// block: groups of 16x2 blocks; host groups column-major in a guest group,
// blocks row-major in a host group; guest group address = tiled address * area.
uint ScaledAddress32(uint2 pos) {
  const uint2 gl = uint2(4u, 1u);
  uint2 scale = uint2(gScaleX, gScaleY);
  uint2 hostGroup = pos >> gl;
  uint2 guestGroup = hostGroup / scale;
  uint2 sub = hostGroup - scale * guestGroup;
  uint2 origin = guestGroup << gl;
  uint idx = sub.x * scale.y + sub.y;
  uint2 inGroup = pos & ((uint2(1u, 1u) << gl) - 1u);
  uint hostOff = (idx << (6u + gl.y)) | (inGroup.y << 6u) | (inGroup.x << 2u);
  return Tiled2D32(origin.x, origin.y, gPitch) * (gScaleX * gScaleY) + hostOff;
}
// Guest bytes B, G, R, A (the CPU scan-out's s[0..3]); LUT entry = r | g<<8 | b<<16.
float4 ScanColour(uint v) {
  uint r = gLut.Load(((v >> 16) & 255u) * 4u) & 255u;
  uint g = (gLut.Load(((v >> 8) & 255u) * 4u) >> 8) & 255u;
  uint b = (gLut.Load((v & 255u) * 4u) >> 16) & 255u;
  return float4(float(r), float(g), float(b), 255.0) / 255.0;
}
float4 PSPresent(float4 pos : SV_Position) : SV_Target {
  uint x = uint(pos.x), y = uint(pos.y);
  if (x >= gW * gScaleX || y >= gH * gScaleY) return float4(0.0, 0.0, 0.0, 1.0);
  uint off;
  if (gScaledSrc != 0u) {
    off = ScaledAddress32(uint2(x, y));
  } else {
    uint gx = x / gScaleX, gy = y / gScaleY;
    off = gTiled != 0u ? Tiled2D32(gx, gy, gPitch) : (gy * gPitch + gx) * 4u;
  }
  return ScanColour(gShm.Load(gFbOff + off));
}
// iter-467 native-b B4 (SR_NB_FRONT): the front target's texel (x, y) is the word ScaledAddress32(x, y) reads.
float4 PSPresentTex(float4 pos : SV_Position) : SV_Target {
  uint x = uint(pos.x), y = uint(pos.y);
  if (x >= gW * gScaleX || y >= gH * gScaleY) return float4(0.0, 0.0, 0.0, 1.0);
  return ScanColour(gFrontTex.Load(int3(int(x), int(y), 0)));
}

cbuffer F : register(b0) {
  float gSrcW; float gSrcH; float gDstX; float gDstY; float gDstW; float gDstH; uint gMode; uint gPad;
};
Texture2D<float4> gSrc : register(t0);
SamplerState gLinear : register(s0);
float4 PSFit(float4 pos : SV_Position) : SV_Target {
  float2 p = pos.xy - float2(gDstX, gDstY);
  if (p.x < 0.0 || p.y < 0.0 || p.x >= gDstW || p.y >= gDstH) return float4(0.0, 0.0, 0.0, 1.0);
  if (gMode == 0u) return float4(gSrc.Load(int3(int2(p), 0)).rgb, 1.0);  // 1:1
  if (gMode == 1u)                                                      // enlarge
    return float4(gSrc.SampleLevel(gLinear, p / float2(gDstW, gDstH), 0.0).rgb, 1.0);
  // shrink: the exact area (box) average of the source pixels this one covers
  float2 ratio = float2(gSrcW / gDstW, gSrcH / gDstH);
  float2 d0 = floor(p);
  float2 s0 = d0 * ratio, s1 = (d0 + 1.0) * ratio;
  int2 i0 = int2(floor(s0)), i1 = int2(ceil(s1 - 1e-4));
  float3 acc = float3(0.0, 0.0, 0.0);
  float wsum = 0.0;
  [loop] for (int yy = i0.y; yy < i1.y; yy++) {
    float wy = min(s1.y, float(yy) + 1.0) - max(s0.y, float(yy));
    [loop] for (int xx = i0.x; xx < i1.x; xx++) {
      float wx = min(s1.x, float(xx) + 1.0) - max(s0.x, float(xx));
      float w = max(wx, 0.0) * max(wy, 0.0);
      acc += gSrc.Load(int3(xx, yy, 0)).rgb * w;
      wsum += w;
    }
  }
  return float4(acc / max(wsum, 1e-6), 1.0);
}
)HLSL";

Microsoft::WRL::ComPtr<ID3DBlob> shmCompile(const char* entry, const char* target) {
  Microsoft::WRL::ComPtr<ID3DBlob> code, err;
  const HRESULT hr = hostHlslCompile(kShmPresentHlsl, sizeof(kShmPresentHlsl) - 1, "pm4_shm_present.hlsl",
                                nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &code, &err);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
#if SR_ORACLE
                "[PM4_SHM] %s (%s) compile failed: %s",
#else
                "[NATIVE_RESOURCE_STORAGE] %s (%s) compile failed: %s",
#endif
                entry, target,
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return nullptr;
  }
  return code;
}

// The letterbox rectangle of a srcW x srcH frame in a dstW x dstH output.
void fitRect(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, uint32_t& x, uint32_t& y,
             uint32_t& w, uint32_t& h) {
  if (!srcW || !srcH || !dstW || !dstH) {
    x = y = 0;
    w = dstW;
    h = dstH;
    return;
  }
  // width-limited or height-limited, whichever keeps the frame inside
  if (uint64_t(dstW) * srcH <= uint64_t(dstH) * srcW) {
    w = dstW;
    h = uint32_t((uint64_t(dstW) * srcH + srcW / 2u) / srcW);
  } else {
    h = dstH;
    w = uint32_t((uint64_t(dstH) * srcW + srcH / 2u) / srcH);
  }
  w = std::max(1u, std::min(w, dstW));
  h = std::max(1u, std::min(h, dstH));
  // iter-466 res4k: an output within 2 pixels of the frame's aspect fills edge to edge (1366x768
  // is 16:9 to 1/3 of a pixel: the exact fit left a 1-pixel black column; the stretch is <= 0.15%).
  if (dstW - w <= 2u) w = dstW;
  if (dstH - h <= 2u) h = dstH;
  x = (dstW - w) / 2u;
  y = (dstH - h) / 2u;
}
}  // namespace

// iter-466 gfx: the letterbox rectangle for the present chain (gfx/d3d12_present_chain.cpp).
void presentFitRect(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, uint32_t& x,
                    uint32_t& y, uint32_t& w, uint32_t& h) {
  fitRect(srcW, srcH, dstW, dstH, x, y, w, h);
}

static bool shmPresentEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_SHMEM_PRESENT");
    return !e || e[0] != '0';
  }();
  return v;
}

// The internal-resolution output texture (RTV + SRV in the RT SRV band).
bool D3D12Backend::pm4OutputEnsure(uint32_t w, uint32_t h) {
  if (m_outTex && m_outW == w && m_outH == h) return true;
  if (!m_pm4GpuReady || m_pm4RtSrvUsed >= pm4RtSrvCapacity()) return false;
  if (!m_outRtvHeap) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_outRtvHeap)))) return false;
  }
  if (m_outTex) retireResource(m_outTex);  // an in-flight frame may still read it
  m_outTex.Reset();
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE cv{};
  cv.Format = rd.Format;
  m_outState = D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, m_outState, &cv,
                                               IID_PPV_ARGS(&m_outTex))))
    return false;
  m_outTex->SetName(L"PM4_OUTPUT");
  m_device->CreateRenderTargetView(m_outTex.Get(), nullptr,
                                   m_outRtvHeap->GetCPUDescriptorHandleForHeapStart());
  // A new slot per (rare) size change: the old descriptor may still be read by a
  // frame in flight.
  m_outSrv = int32_t(m_pm4RtSrvUsed++);
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = rd.Format;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  publishXlatSrv(m_outTex.Get(), &sd, pm4RtSrvCpu(uint32_t(m_outSrv)));
  m_outW = w;
  m_outH = h;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[RES_SCALE] output texture %ux%u (srv slot %d)", w, h, m_outSrv);
  return true;
}

bool D3D12Backend::pm4ShmPresent(uint32_t fbPhys, uint32_t width, uint32_t height,
                                 uint32_t pitchTexels, bool tiled, ID3D12Resource* backbuffer) {
  gtMark(GtDraw);  // iter-466 res4k: SR_PM4_GPU_TIME per-pass attribution
  if (!pm4ShmScanOut(fbPhys, width, height, pitchTexels, tiled, backbuffer->GetDesc().Format))
    return false;
  gtMark(GtScanOut);
  pm4PresentChain(backbuffer);  // iter-466 gfx: post, colour, scale, sharpen, then fit (pass 2)
  gtMark(GtFit);  // iter-466 res4k: the whole chain (post .. fit)
  static std::atomic<uint32_t> s_log{0};
  if (s_log.fetch_add(1) < 4)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RES_SCALE] present fb=%08X %ux%u -> internal %ux%u -> backbuffer %llux%u", fbPhys,
                width, height, m_outW, m_outH, (unsigned long long)backbuffer->GetDesc().Width,
                backbuffer->GetDesc().Height);
  return true;
}

// The replay's frame dump at a scale: pass 1 alone, on a list it opens, runs and
// closes (the replay dumps after the frame's own list executed).
bool D3D12Backend::pm4ScanOutForDump(uint32_t fbPhys, uint32_t width, uint32_t height,
                                     uint32_t pitchTexels, bool tiled) {
  if (!m_shmReady) return false;
  const bool wasClosed = m_clState != ClState::Recording;
  if (wasClosed) clReset(Pm4Reset);
  ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
  m_commandList->SetDescriptorHeaps(2, heaps);
  const bool ok = pm4ShmScanOut(fbPhys, width, height, pitchTexels, tiled,
                                m_renderTargets[m_frameIndex]->GetDesc().Format);
  if (ok) {
    // iter-466 gfx: the internal-resolution present-chain stages (post, colour), as presented;
    // pm4ReadOutput dumps their result. At their defaults nothing runs (the scan-out is dumped).
    uint32_t ran = 0;
    m_chainInternalTgt =
        pm4ChainInternal(gfxopt::current(), PresentSurface{m_outTex.Get(), m_outSrv, m_outW, m_outH}, ran).tgt;
  }
  pm4Flush();  // execute + wait (the list reopens)
  if (wasClosed) clClose(Pm4Close);
  return ok;
}

// Pass 1: the front buffer at the internal resolution into m_outTex.
bool D3D12Backend::pm4ShmScanOut(uint32_t fbPhys, uint32_t width, uint32_t height,
                                 uint32_t pitchTexels, bool tiled, DXGI_FORMAT backbufferFmt) {
  if (!m_shmReady || !shmPresentEnabled() || m_shmPresentFailed) return false;
  if (!m_shmPresentPso) {
    auto fail = [&](const char* what) {
      m_shmPresentFailed = true;
      SR_LOG_BOOT(sr::LogSeverity::Error,
#if SR_ORACLE
                  "[PM4_SHM] present init failed at %s - CPU scan-out stays",
#else
                  "[NATIVE_RESOURCE_STORAGE] present init failed at %s - CPU scan-out stays",
#endif
                  what);
      return false;
    };
    auto makeRs = [&](const D3D12_ROOT_SIGNATURE_DESC& rd,
                      Microsoft::WRL::ComPtr<ID3D12RootSignature>& out) -> bool {
      Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
      return SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) &&
             SUCCEEDED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&out)));
    };
    {  // scan-out: {0: 8 constants, 1: SRV t0 front buffer memory, 2: SRV t1 LUT}
      D3D12_ROOT_PARAMETER p[3]{};
      p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      p[0].Constants.Num32BitValues = 8;
      p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      p[1].Descriptor.ShaderRegister = 0;
      p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      p[2].Descriptor.ShaderRegister = 1;
      for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC rd{};
      rd.NumParameters = 3;
      rd.pParameters = p;
      if (!makeRs(rd, m_shmPresentRs)) return fail("root signature");
    }
    {  // fit: {0: 8 constants, 1: table SRV t0 (the output texture)} + a linear clamp sampler
      D3D12_DESCRIPTOR_RANGE range{};
      range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 1;
      D3D12_ROOT_PARAMETER p[2]{};
      p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      p[0].Constants.Num32BitValues = 8;
      p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      p[1].DescriptorTable.NumDescriptorRanges = 1;
      p[1].DescriptorTable.pDescriptorRanges = &range;
      p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_STATIC_SAMPLER_DESC ss{};
      ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      ss.MaxLOD = D3D12_FLOAT32_MAX;
      ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_ROOT_SIGNATURE_DESC rd{};
      rd.NumParameters = 2;
      rd.pParameters = p;
      rd.NumStaticSamplers = 1;
      rd.pStaticSamplers = &ss;
      if (!makeRs(rd, m_fitRs)) return fail("fit root signature");
    }
    auto vs = shmCompile("VSFull", "vs_5_1");
    auto ps = shmCompile("PSPresent", "ps_5_1");
    auto psFit = shmCompile("PSFit", "ps_5_1");
    if (!vs || !ps || !psFit) return fail("shader compile");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
    gd.pRootSignature = m_shmPresentRs.Get();
    gd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    gd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = UINT_MAX;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;  // the output texture
    gd.SampleDesc.Count = 1;
    if (FAILED(m_device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&m_shmPresentPso))))
      return fail("pso");
    if (nbFrontOn()) {  // iter-467 native-b B4: {0: 8 constants, 1: SRV t1 LUT, 2: table SRV t2 the front target}
      D3D12_DESCRIPTOR_RANGE range{};
      range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 1;
      range.BaseShaderRegister = 2;
      D3D12_ROOT_PARAMETER p[3]{};
      p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      p[0].Constants.Num32BitValues = 8;
      p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      p[1].Descriptor.ShaderRegister = 1;
      p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      p[2].DescriptorTable.NumDescriptorRanges = 1;
      p[2].DescriptorTable.pDescriptorRanges = &range;
      for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC rd{};
      rd.NumParameters = 3;
      rd.pParameters = p;
      auto psTex = shmCompile("PSPresentTex", "ps_5_1");
      D3D12_GRAPHICS_PIPELINE_STATE_DESC gt = gd;
      if (psTex && makeRs(rd, m_nbFrontRs)) {
        gt.pRootSignature = m_nbFrontRs.Get();
        gt.PS = {psTex->GetBufferPointer(), psTex->GetBufferSize()};
        if (FAILED(m_device->CreateGraphicsPipelineState(&gt, IID_PPV_ARGS(&m_nbFrontPso)))) m_nbFrontPso.Reset();
      }
      if (!m_nbFrontPso)
        SR_LOG_BOOT(sr::LogSeverity::Error, "[NB_FRONT] scan-out pipeline failed - front buffers stay in memory");
    }
    gd.pRootSignature = m_fitRs.Get();
    gd.PS = {psFit->GetBufferPointer(), psFit->GetBufferSize()};
    gd.RTVFormats[0] = backbufferFmt;
    if (FAILED(m_device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&m_fitPso))))
      return fail("fit pso");
    SR_LOG_BOOT(sr::LogSeverity::Info,
#if SR_ORACLE
                "[PM4_SHM] scan-out from shared memory ready (output texture + fit to rtv fmt %u)",
#else
                "[NATIVE_RESOURCE_STORAGE] scan-out from resource storage ready (output texture + fit to rtv fmt %u)",
#endif
                unsigned(gd.RTVFormats[0]));
  }
  if (pitchTexels < width) pitchTexels = width;
  const uint32_t tp = tiled ? ((pitchTexels + 31u) & ~31u) : pitchTexels;
  const uint32_t span = tiled ? uint32_t(xenos_tiling::tiledSpanBytes32(tp, height))
                              : pitchTexels * height * 4u;
  const resscale::Scale& rs = resscale::get();
  const uint32_t outW = width * rs.x, outH = height * rs.y;
  if (!pm4OutputEnsure(outW, outH)) return false;
  // Resolution-scaled front buffer (Xenia RequestSwapTexture: the texture key is
  // scaled_resolve when a scaled resolve wrote any of its pages).
  // iter-467 native-b B4 (SR_NB_FRONT): the front target a resolve wrote (the memory is held, never de-tiled).
  const int32_t frontSrv = (rs.scaled() && tiled) ? nbFrontScanSrv(fbPhys, width, height, tp) : -1;
  const bool scaledSrc = frontSrv < 0 && rs.scaled() && tiled && srAnyScaled(fbPhys, span);
  // NATIVE present (SR_NATIVE_PRESENT=1): the front buffer this frame's native resolve
  // copied linearly; anything else (an older or CPU-written front buffer) scans out of
  // the shared memory as before.
  NatFront* nf = nullptr;
  if (!scaledSrc && frontSrv < 0 && natPresentEnabled()) {
    const uint32_t fb = fbPhys & 0x1FFFFFFFu;
    for (NatFront& f : g_nat.front)
      if (f.valid && f.dest == fb && f.epoch == m_frameEpoch && width <= f.pitch && height <= f.rows)
        nf = &f;
    (nf ? g_nat.presentsNative : g_nat.presentsShm)++;
    const uint64_t np = g_nat.presentsNative + g_nat.presentsShm;
    if (np <= 4 || (np & (np - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_PRESENT] fb=%08X %ux%u: %s (native %llu, shm %llu)", fbPhys,
                  width, height, nf ? "front-buffer copy" : "shared memory",
                  (unsigned long long)g_nat.presentsNative, (unsigned long long)g_nat.presentsShm);
  }
  // A front buffer the CPU wrote (not our resolve) reaches the shared memory.
  if (!scaledSrc && !nf && frontSrv < 0) {
    const memres::KindScope mrKind(memres::Kind::kScanout);  // mem.resources step 2a: the census's fetch kind
    pm4ShmRequest(fbPhys, span);
  }
  // The LUT: 256 x (r | g << 8 | b << 16) from the game's display ramp.
  const uint32_t third = m_shmRenderThird;
  if (m_shmUploadUsedT[third] + 1024u > uint64_t(third + 1u) * kShmUploadThird) pm4Flush();
  const uint64_t lutOff = (m_shmUploadUsedT[third] + 255u) & ~uint64_t(255u);
  {
    const auto& ramp = sr::gfx::gamma::displayRamp();
    uint32_t* lut = reinterpret_cast<uint32_t*>(m_shmUploadPtr + lutOff);
    for (uint32_t i = 0; i < 256; i++)
      lut[i] = uint32_t(ramp.to8(ramp.table10(i, 0))) |
               (uint32_t(ramp.to8(ramp.table10(i, 1))) << 8) |
               (uint32_t(ramp.to8(ramp.table10(i, 2))) << 16);
#if SR_ORACLE
    // iter-466 stutter lane, SR_GAMMA_OBS=1 (observe-only): one line whenever the scan-out
    // ramp this present uses changes (green at 64 / 128 / 255, the ramp version), to tell a
    // gamma-ramp fade from a dark frame.
    static const bool s_gammaObs = [] {
      const char* e = std::getenv("SR_GAMMA_OBS");
      return e && e[0] == '1';
    }();
    if (s_gammaObs) {
      static uint32_t s_last = ~0u, s_lines = 0, s_presents = 0;
      ++s_presents;
      const uint32_t key = (lut[64] >> 8 & 0xFFu) | (lut[128] >> 8 & 0xFFu) << 8 | (lut[255] >> 8 & 0xFFu) << 16;
      if (key != s_last && s_lines < 400) {
        ++s_lines;
        SR_LOG_BOOT(sr::LogSeverity::Info, "[GAMMA_OBS] present=%u green@64=%u @128=%u @255=%u version=%llu",
                    s_presents, lut[64] >> 8 & 0xFFu, lut[128] >> 8 & 0xFFu, lut[255] >> 8 & 0xFFu,
                    (unsigned long long)ramp.version);
      }
      s_last = key;
    }
#endif  // SR_ORACLE
  }
  m_shmUploadUsedT[third] = lutOff + 1024u;
  // ── pass 1: the front buffer at the internal resolution ──
  D3D12_GPU_VIRTUAL_ADDRESS srcVa;
  uint32_t fbOff;
  uint32_t scanPitch = tp, scanTiled = tiled ? 1u : 0u;
  if (frontSrv >= 0) {
    srcVa = 0;  // the texture, through the descriptor table
    fbOff = 0;
    scanTiled = 0;
  } else if (nf) {
    const D3D12_RESOURCE_STATES rd =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (nf->state != rd) {
      pm4ShmBarrier(m_commandList.Get(), nf->buf.Get(), nf->state, rd);
      nf->state = rd;
    }
    srcVa = nf->buf->GetGPUVirtualAddress();
    fbOff = 0;
    scanPitch = nf->pitch;
    scanTiled = 0;
  } else if (scaledSrc) {
    nbMaterialize(fbPhys, uint32_t(span));  // iter-467 native-b B2
    srEnsure(fbPhys, span);
    srState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    srcVa = m_srBuf->GetGPUVirtualAddress() + uint64_t(fbPhys & 0x1FFFFFFFu) * rs.area();
    fbOff = 0;
  } else {
    pm4ShmState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    srcVa = m_shm->GetGPUVirtualAddress();
    fbOff = fbPhys & 0x1FFFFFFFu;
  }
  if (m_outState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    pm4ShmBarrier(m_commandList.Get(), m_outTex.Get(), m_outState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_outState = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE outRtv = m_outRtvHeap->GetCPUDescriptorHandleForHeapStart();
  m_commandList->OMSetRenderTargets(1, &outRtv, FALSE, nullptr);
  {
    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(outW), float(outH), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, LONG(outW), LONG(outH)};
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &sc);
  }
  const uint32_t c[8] = {fbOff, scanPitch, scanTiled, width, height, rs.x, rs.y, scaledSrc ? 1u : 0u};
  if (frontSrv >= 0) {
    ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
    m_commandList->SetDescriptorHeaps(2, heaps);
    m_commandList->SetGraphicsRootSignature(m_nbFrontRs.Get());
    m_commandList->SetPipelineState(m_nbFrontPso.Get());
    m_commandList->SetGraphicsRoot32BitConstants(0, 8, c, 0);
    m_commandList->SetGraphicsRootShaderResourceView(1, m_shmUpload->GetGPUVirtualAddress() + lutOff);
    m_commandList->SetGraphicsRootDescriptorTable(2, pm4RtSrvGpu(uint32_t(frontSrv)));
  } else {
    m_commandList->SetGraphicsRootSignature(m_shmPresentRs.Get());
    m_commandList->SetPipelineState(m_shmPresentPso.Get());
    m_commandList->SetGraphicsRoot32BitConstants(0, 8, c, 0);
    m_commandList->SetGraphicsRootShaderResourceView(1, srcVa);
    m_commandList->SetGraphicsRootShaderResourceView(2, m_shmUpload->GetGPUVirtualAddress() + lutOff);
  }
  m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_commandList->DrawInstanced(3, 1, 0, 0);
  pm4ShmBarrier(m_commandList.Get(), m_outTex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_outState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  static std::atomic<uint32_t> s_log{0};
  if (s_log.fetch_add(1) < 4)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[RES_SCALE] scan-out fb=%08X %ux%u -> internal %ux%u from %s",
                fbPhys, width, height, outW, outH,
                frontSrv >= 0 ? "its front target" : scaledSrc ? "the scaled resolve memory" : "1x memory");
  return true;
}

void D3D12Backend::pm4FitToBackbuffer(ID3D12Resource* backbuffer, const PresentSurface& src) {
  const D3D12_RESOURCE_DESC bd = backbuffer->GetDesc();
  const uint32_t bw = uint32_t(bd.Width), bh = bd.Height;
  uint32_t x, y, w, h;
  fitRect(src.w, src.h, bw, bh, x, y, w, h);
  pm4ShmBarrier(m_commandList.Get(), backbuffer, D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(m_frameIndex) * m_rtvDescriptorSize;
  m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0.0f, 0.0f, float(bw), float(bh), 0.0f, 1.0f};
  const D3D12_RECT sc{0, 0, LONG(bw), LONG(bh)};
  m_commandList->RSSetViewports(1, &vp);
  m_commandList->RSSetScissorRects(1, &sc);
  ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
  m_commandList->SetDescriptorHeaps(2, heaps);
  m_commandList->SetGraphicsRootSignature(m_fitRs.Get());
  m_commandList->SetPipelineState(m_fitPso.Get());
  // mode 0: 1:1, 1: enlarge (bilinear), 2: shrink (area)
  const uint32_t mode = (w == src.w && h == src.h) ? 0u : (w >= src.w && h >= src.h) ? 1u : 2u;
  struct { float sw, sh, dx, dy, dw, dh; uint32_t mode, pad; } f{
      float(src.w), float(src.h), float(x), float(y), float(w), float(h), mode, 0u};
  m_commandList->SetGraphicsRoot32BitConstants(0, 8, &f, 0);
  m_commandList->SetGraphicsRootDescriptorTable(1, pm4RtSrvGpu(uint32_t(src.srv)));
  m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_commandList->DrawInstanced(3, 1, 0, 0);
  pm4ShmBarrier(m_commandList.Get(), backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT);
  static uint32_t s_lastMode = ~0u, s_lastW = 0, s_lastH = 0;
  if (mode != s_lastMode || bw != s_lastW || bh != s_lastH) {
    s_lastMode = mode;
    s_lastW = bw;
    s_lastH = bh;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RES_SCALE] fit %ux%u -> backbuffer %ux%u at (%u,%u) %ux%u, %s", src.w, src.h, bw,
                bh, x, y, w, h, mode == 0 ? "1:1" : mode == 1 ? "bilinear enlarge" : "area shrink");
  }
}

bool D3D12Backend::pm4ReadOutput(std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h) {
  if (!m_outTex || !m_texUploadList || !m_texUploadAlloc || !m_texFence) return false;
  // iter-466 gfx: the present chain's internal-resolution result when a post / colour stage ran
  // (else the scan-out itself).
  ID3D12Resource* srcTex = m_outTex.Get();
  D3D12_RESOURCE_STATES srcState = m_outState;
  w = m_outW;
  h = m_outH;
  if (m_chainInternalTgt >= 0 && m_chainInternalTgt < kChainTargets && m_chainTgt[m_chainInternalTgt].tex) {
    const ChainTarget& ct = m_chainTgt[m_chainInternalTgt];
    srcTex = ct.tex.Get();
    srcState = ct.state;
    w = ct.w;
    h = ct.h;
  }
  const uint32_t rowPitch = (w * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
  const uint64_t bytes = uint64_t(rowPitch) * h;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = bytes;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> rb;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb))))
    return false;
  if (FAILED(m_texUploadAlloc->Reset())) return false;
  if (FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) return false;
  pm4ShmBarrier(m_texUploadList.Get(), srcTex, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = rb.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  dst.PlacedFootprint.Footprint.Width = w;
  dst.PlacedFootprint.Footprint.Height = h;
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
  src.pResource = srcTex;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  m_texUploadList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  pm4ShmBarrier(m_texUploadList.Get(), srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
  m_texUploadList->Close();
  ID3D12CommandList* l[] = {m_texUploadList.Get()};
  m_commandQueue->ExecuteCommandLists(1, l);
  m_texFenceValue++;
  m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
  if (m_texFence->GetCompletedValue() < m_texFenceValue) {
    m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
    WaitForSingleObject(m_texFenceEvent, 10000);
  }
  uint8_t* mp = nullptr;
  const D3D12_RANGE rr{0, SIZE_T(bytes)};
  if (FAILED(rb->Map(0, &rr, reinterpret_cast<void**>(&mp))) || !mp) return false;
  bgra.resize(size_t(w) * h * 4u);
  for (uint32_t yy = 0; yy < h; yy++) {
    const uint8_t* s = mp + size_t(yy) * rowPitch;
    uint8_t* d = bgra.data() + size_t(yy) * w * 4u;
    for (uint32_t xx = 0; xx < w; xx++) {  // RGBA -> BGRA
      d[xx * 4 + 0] = s[xx * 4 + 2];
      d[xx * 4 + 1] = s[xx * 4 + 1];
      d[xx * 4 + 2] = s[xx * 4 + 0];
      d[xx * 4 + 3] = 255;
    }
  }
  const D3D12_RANGE wr{0, 0};
  rb->Unmap(0, &wr);
  return true;
}

}  // namespace gfx
}  // namespace sr
