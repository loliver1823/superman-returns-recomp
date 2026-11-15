// The PM4 frame renderer (shipping; docs/RENDERER_PM4_HYBRID.md P1, docs/RELEASE_COMPOSITION.md section 8 stage 3):
//   - one host render target per surface key (gfx/surface_key.hpp edram::RtKey {base, pitch, msaa, depth, format}:
//     the Xbox 360 surface as the XDK names it, by its EDRAM base and pitch in tiles);
//   - the surface table (gfx/nb_surfaces.hpp): which surface owns which tile run; a draw's or a resolve's claim of a
//     run another surface owns is a MOVE of those pixels, performed by the direct transfer passes (source RT ->
//     destination RT, the alias geometry of gfx/surface_key.hpp rangeRects / the shared pack HLSL) - Xenia's host-RT
//     path transfers the same way (d3d12_render_target_cache.cc PerformTransfersAndResolveClears);
//   - resolves run the native kernels (gfx/native_resolve.inc, gfx/native_targets.inc) from the owning surfaces'
//     host RTs straight into guest memory (tiled, endian-swapped) or into the texture that samples them;
//   - the frame's render thread, the draw batches, the clears, the present hand-off.
// The EDRAM WORD model - the tile ownership map, the 10 MiB EDRAM buffer with its store / load passes, the CPU mirror,
// the EDRAM-path resolve, the trace snapshot and the oracles over them - is gfx/d3d12_edram_model.cpp (the development build's
// oracle unit, EMULATION_SOURCES). The release configuration (SR_ORACLE=0) has no fallback into it: a move the direct
// passes cannot take stops the renderer ([SURFACE_MOVE_BAD]), as does a resolve the native kernel does not take
// ([NATIVE_RESOLVE_BAD]) - 3.1 rule 4. The helpers both units share are gfx/pm4_render_detail.hpp.
//
// Known simplifications (logged, not hidden): MSAA surfaces render at 1x (sample 0); culling is off; 8_8_8_8_GAMMA
// stores as 8_8_8_8.

#include "gamma_ramp.hpp"
#include "host_hlsl.hpp"  // step 4 7a: SR_HOST_HLSL_AOT (host HLSL compiled ahead of time)
#include "../kernel/host_work.hpp"  // step 4 2k(4): the render thread is a presenting thread
#include "../kernel/mem_sites.hpp"  // step 4 7d: SR_MEM_SITES allocation names
#include "native_owner_rects.hpp"
#include "native_failure_diagnostics.hpp"
#include "par.hpp"
#include "cp_simd.hpp"
#include "../kernel/sr_oracle.hpp"
#if SR_ORACLE
#include "../kernel/gpu_write_watch.hpp"
#endif
#include "../kernel/hitch_log.hpp"  // iter-466 stutter: SR_HITCH_LOG (observe-only)
#include "drain_reuse.hpp"  // step 6 R6 fix round 2: regsFromCommand, the single-writer census counters
#include "../kernel/native_meter.hpp"  // iter-467 native meter: strand B's EDRAM-model counts
#include "../kernel/ab_toggle.hpp"    // iter-467 native-b: in-roll A/B of the NB gates (TOOLKIT 1.8d)
#include "resource_census.hpp"  // mem.resources step 2a: the census's fetch kinds and IB arm (no-op under SR_ORACLE=0)
#include "resource_memory.hpp"
#include "resource_ab.hpp"  // mem.resources frozen-pass A/B (resourceDecides / dual mode)
#if SR_ORACLE
#include "resource_memory_check.hpp"
#endif
#include <array>
#include <chrono>
#include <d3dcompiler.h>
#include <algorithm>
#include <emmintrin.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../app/log.hpp"
#include "d3d12_backend.hpp"
#include "surface_key.hpp"      // C1 stage 3: the surface identity, the format math, the alias geometry (ship)
#include "pm4_render_detail.hpp"  // C1 stage 3: the helpers shared with the oracle half (gfx/d3d12_edram_model.cpp)
#include "nb_surfaces.hpp"  // iter-467 native-b EDRAM zero step 2: the surface table (SR_NB_SURF_VERIFY)
#include "native_targets.hpp"  // iter-467 native-b: D1 NativeTargets
#include "kernel/guest_context.hpp"
#include "kernel/phys_window.hpp"
#include "native_zpd.hpp"  // p2.zpd: EVENT_WRITE_ZPD as host occlusion queries (SR_NATIVE_ZPD)
#include "alpha_test.hpp"  // step-4 lane C: the Xenos alpha test (SR_XLAT_ALPHA_TEST, tools/xenosrecomp/0005)
#include "res_scale.hpp"
#include "shader_cache.hpp"
#include "xenos_state.hpp"
#include "pm4_live_trace.hpp"
#include "pm4_half_gate.hpp"  // iter-467 freezefx: SR_PM4_HALF_SIGNED
#include "pm4_quad_gate.hpp"  // iter-467 freezefx: SR_PM4_QUAD_STRIP
#include "pm4_frame_gate.hpp"  // step-4 lane C (M6): SR_PM4_FRAME_FAILCLOSED
#include "../kernel/highfps.hpp"
#include "native_draw.hpp"
#include "../kernel/native_exec.hpp"  // iter-467 native A: SR_NATIVE_EXEC_PROF placement

namespace sr { namespace kernel { uint8_t* guestVAToHost(uint32_t guestVA); } }

namespace sr {
namespace gfx {
// iter-460 perf census: where the drain (CP-thread draw translation) spends
// its time per frame - PSO/targets prep, vertex-fetch staging (hash + copy of
// the fetched ranges), index conversion, CPU vertex decode (non-vsf draws).
// step 6 R6 fix round 2 (Astra r2 P1): written by the draining thread only (drainreuse::add), never reset;
// [PM4_BIND_CENSUS] reports the delta since its previous pass.
std::atomic<double> g_drainMsPrep{0}, g_drainMsVsfFetch{0}, g_drainMsVsfIb{0}, g_drainMsDecode{0};
// iter-465 perf lane: the census TIMERS (steady_clock::now = QueryPerformanceCounter, ~40 calls
// per draw: prep, vsf fetch / index, decode, 2 per texture slot per batch) cost the CP 2-4 ms per
// gameplay frame (host profile: QPC 7% of the CP). SR_PM4_DIAG_TIMING=1 turns them back on; off,
// [PM4_BIND_CENSUS] keeps its counts and reports 0.0 ms.
extern const bool g_pm4DiagTiming = [] {
  const char* e = std::getenv("SR_PM4_DIAG_TIMING");
  return e && e[0] == '1';
}();
std::atomic<uint32_t> g_drainN{0}, g_drainCalls{0};  // step 6 R6 fix round 2: single-writer (the draining thread)
// iter-466 cp lane: an SR_CP_* gate read once, logged once (the activation witness).
bool cpsimd::gateLogged(const char* name, bool def) {
  const bool v = cpsimd::gate(name, def);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[CP_GATE] %s=%d (default %d, sse4.1 %d)", name, v ? 1 : 0,
              def ? 1 : 0, cpsimd::available() ? 1 : 0);
  return v;
}
bool cpsimd::vsfNoWin() {
  static const bool v = cpsimd::gateLogged("SR_CP_VSF_NOWIN", true);
  return v;
}
// Draw classes: vsf without an index list / with one; CPU-decoded rect lists,
// clip-disabled pixel draws, other.
std::atomic<uint32_t> g_drainNoIb{0}, g_drainIb{0}, g_drainDecRect{0}, g_drainDecClip{0}, g_drainDecOther{0};  // single-writer
// iter-460 perf census: the resolve executor's phases per frame.
double g_resMsStore = 0, g_resMsShm = 0, g_resMsInval = 0, g_resMsClear = 0, g_resMsCpu = 0;

bool pm4RtCacheEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_RTCACHE");
    return (e && e[0] == '1') || pm4LiveEnabled();
  }();
  return v;
}

// P2: the LIVE game draws through the PM4 path (default OFF). The live engine
// submits draws, each indirect buffer's draws are translated right after it
// executes, the guest's VdSwap renders + presents the frame, and the HLE layer
// runs with no backend (observers and bookkeeping only).
bool pm4LiveEnabled() {
#if SR_ORACLE
  static const bool v = [] {
    // P3 (iter-460): the PM4 command stream is the default draw path; =0 keeps
    // the HLE draw path for an A/B (NATURAL_BASELINE 2026-09-29: gameplay renders).
    const char* e = std::getenv("SR_PM4_LIVE");
    return !e || e[0] != '0';
  }();
  return v;
#else
  // SR_ORACLE=0 (kernel/sr_oracle.hpp): the HLE draw path, the SR_PM4_LIVE=0 A/B, is compiled out with
  // gfx/hle_video.cpp, so SR_PM4_LIVE reads as unset: the PM4 path always draws.
  return true;
#endif
}

// C1 stage 3: the helpers below were file-local; the oracle half (gfx/d3d12_edram_model.cpp) shares them through
// gfx/pm4_render_detail.hpp (kPm4StagingBytes lives there).
constexpr uint32_t kPm4RtvSlots = 512, kPm4DsvSlots = 128;

// Xenia's host depth formats: kD24S8 as D24_UNORM_S8_UINT - the rasterizer's
// own float->unorm24 conversion makes the guest bits exact (the shadow-map
// clear crafts 0x7FFF7F|stencil 0xFF so the aliased k_16_16 reads +32,+32);
// kD24FS8 as D32_FLOAT_S8X24_UINT. Both keep the stencil plane.
DXGI_FORMAT pm4DepthDsv(uint32_t depthFmt) {
  return depthFmt == 1u ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
}
DXGI_FORMAT pm4DepthTypeless(uint32_t depthFmt) {
  return depthFmt == 1u ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
}

// A depth surface's two copy planes (0 = depth, 1 = stencil) as placed in a
// staging buffer, with each plane's bytes per texel (Pm4DepthPlanes: pm4_render_detail.hpp).
bool pm4DepthFootprints(ID3D12Device* dev, ID3D12Resource* res, uint64_t base, Pm4DepthPlanes& pl) {
  D3D12_RESOURCE_DESC desc = res->GetDesc();
  UINT rows[2] = {};
  UINT64 rowBytes[2] = {};
  dev->GetCopyableFootprints(&desc, 0, 2, base, pl.fp, rows, rowBytes, &pl.total);
  for (int p = 0; p < 2; p++) {
    if (!pl.fp[p].Footprint.Width) return false;
    pl.bpt[p] = uint32_t(rowBytes[p] / pl.fp[p].Footprint.Width);
  }
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_DEPTH] copy planes: p0 fmt=%u bpt=%u  p1 fmt=%u bpt=%u (resource fmt=%u)",
                pl.fp[0].Footprint.Format, pl.bpt[0], pl.fp[1].Footprint.Format, pl.bpt[1],
                desc.Format);
  }
  return pl.bpt[0] == 4u && (pl.bpt[1] == 1u || pl.bpt[1] == 4u || pl.bpt[1] == 8u);
}
// (pm4StencilByte: pm4_render_detail.hpp)
// Guest EDRAM word of one depth texel: D24S8 keeps the host unorm24 bits
// exactly; D24FS8 converts the host float to 20e4.
uint32_t pm4DepthGuestBits(uint32_t depthFmt, const uint8_t* mapped, const Pm4DepthPlanes& pl,
                           uint32_t x, uint32_t y) {
  uint32_t v0;
  std::memcpy(&v0, mapped + pl.fp[0].Offset + uint64_t(y) * pl.fp[0].Footprint.RowPitch + x * 4u, 4);
  const uint32_t s = mapped[pl.fp[1].Offset + uint64_t(y) * pl.fp[1].Footprint.RowPitch +
                            uint64_t(x) * pl.bpt[1] + pm4StencilByte(pl.bpt[1])];
  if (depthFmt == 1u) {
    float f;
    std::memcpy(&f, &v0, 4);
    return uint32_t(edram::packDepth(1u, f, s));
  }
  return ((v0 & 0xFFFFFFu) << 8) | s;
}
void pm4DepthPutGuestBits(uint32_t depthFmt, uint8_t* staged, const Pm4DepthPlanes& pl, uint32_t x,
                          uint32_t y, uint32_t word) {
  uint32_t v0;
  if (depthFmt == 1u) {
    const float f = edram::unpackDepth(1u, word);
    std::memcpy(&v0, &f, 4);
  } else {
    v0 = word >> 8;
  }
  std::memcpy(staged + pl.fp[0].Offset + uint64_t(y) * pl.fp[0].Footprint.RowPitch + x * 4u, &v0, 4);
  staged[pl.fp[1].Offset + uint64_t(y) * pl.fp[1].Footprint.RowPitch + uint64_t(x) * pl.bpt[1] +
         pm4StencilByte(pl.bpt[1])] = uint8_t(word & 0xFFu);
}

DXGI_FORMAT pm4ColorDxgi(edram::HostFmt f) {
  switch (f) {
    case edram::HostFmt::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case edram::HostFmt::RGB10A2: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case edram::HostFmt::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case edram::HostFmt::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
    case edram::HostFmt::R32F: return DXGI_FORMAT_R32_FLOAT;
    case edram::HostFmt::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

void pm4Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER br{};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition.pResource = r;
  br.Transition.StateBefore = a;
  br.Transition.StateAfter = b;
  br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &br);
}

// Host pointer for guest physical `phys`, through the SAME window the texture
// cache reads (gpuPhysToHost: 0x8 alias unless SR_GPU_PHYS_A=1), bounded by
// the guest allocation - fault-free (the harness VEH makes a host AV fatal).
// iter-460: the window is the allocators' answer (the command buffers first,
// then the live grants: kernel/phys_window.hpp), as every GPU read chooses.
// This is also the WRITE side (pm4ShmReadback, the CPU resolve): with the 299 MB
// probe reclaimed (SR_PHYS_LIFO_FREE) physical grants start at phys 0, and a
// readback of a GPU-written page at phys p written through 0x80000000|p landed
// in the low virtual region instead of the grant at 0xA0000000|p.
uint8_t* pm4GuestWritableWin(uint32_t phys, size_t& avail, int& win) {
  static const bool s_physA = [] {
    const char* v = std::getenv("SR_GPU_PHYS_A");
    return v && v[0] == '1';
  }();
  static const bool s_owner = [] {
    const char* v = std::getenv("SR_PM4_WRITABLE_OWNER");
    return !v || v[0] != '0';
  }();
  avail = 0;
  win = s_physA ? 0xA : 8;
  const uint32_t p32 = phys & 0x1FFFFFFFu;
  if (s_owner) {
    int w = sr::kernel::physwin::commandBufferWindowEnabled()
                ? sr::kernel::physwin::commandBufferWindow(p32, 1u)
                : 0;
    if (!w && sr::kernel::physwin::gpuOwnerWindowEnabled()) w = sr::kernel::physwin::gpuWindow(p32, 1u);
    if (w == 8 || w == 0xA) win = w;
  }
  uint8_t* gbase = nullptr;
  size_t gsz = 0;
  sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
  uint8_t* p = sr::kernel::guestVAToHost((win == 0xA ? 0xA0000000u : 0x80000000u) + p32);
  if (!gbase || !p || p < gbase || p >= gbase + gsz) return nullptr;
  avail = size_t((gbase + gsz) - p);
  return p;
}

// The legacy accessor: the 0x8 window (SR_GPU_PHYS_A: 0xA). The vertex-fetch
// staging and pm4ShmRequest keep it as their explicit "0x8 copy"; the writers
// (pm4ShmReadback, the CPU resolve) and the scan-out use the Win variant.
uint8_t* pm4GuestWritable(uint32_t phys, size_t& avail) {
  static const bool s_physA = [] {
    const char* v = std::getenv("SR_GPU_PHYS_A");
    return v && v[0] == '1';
  }();
  avail = 0;
  uint8_t* gbase = nullptr;
  size_t gsz = 0;
  sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
  uint8_t* p = sr::kernel::guestVAToHost((s_physA ? 0xA0000000u : 0x80000000u) + (phys & 0x1FFFFFFFu));
  if (!gbase || !p || p < gbase || p >= gbase + gsz) return nullptr;
  avail = size_t((gbase + gsz) - p);
  return p;
}

// (pm4AlignUp: pm4_render_detail.hpp)

// SR_PM4_DUMP_FROM / SR_PM4_DUMP_TO (batch indices, inclusive): after each
// such draw batch, dump its RT0 as a BMP to SR_PM4_DUMP_DIR (default
// archive/pm4_dump). Floats are clamped to [0,1]; one GPU sync per dump.
// iter-460: SR_PM4_DUMP_FRAME=<frame epoch> limits the dumps to one frame
// (live runs: every frame would otherwise dump and flush); SR_PM4_DUMP_EVERY=n
// keeps every n-th batch of the range. (Pm4DumpCfg: pm4_render_detail.hpp)
const Pm4DumpCfg& pm4DumpCfg() {
  static const Pm4DumpCfg c = [] {
    Pm4DumpCfg d;
    if (const char* e = std::getenv("SR_PM4_DUMP_FROM")) d.from = std::strtol(e, nullptr, 10);
    if (const char* e = std::getenv("SR_PM4_DUMP_TO")) d.to = std::strtol(e, nullptr, 10);
    if (d.to < d.from) d.to = d.from;
    if (const char* e = std::getenv("SR_PM4_DUMP_FRAME")) d.frame = std::strtol(e, nullptr, 10);
    if (const char* e = std::getenv("SR_PM4_DUMP_EVERY")) d.every = std::strtol(e, nullptr, 10);
    const char* dir = std::getenv("SR_PM4_DUMP_DIR");
    d.dir = (dir && *dir) ? dir : "archive/pm4_dump";
    return d;
  }();
  return c;
}
void pm4WriteBmp(const std::string& path, const uint8_t* bgra, uint32_t w, uint32_t h) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t imgBytes = w * h * 4u, fileBytes = 54u + imgBytes;
  uint8_t hdr[54] = {'B', 'M'};
  std::memcpy(hdr + 2, &fileBytes, 4);
  const uint32_t off = 54, dib = 40, planesBpp = 1u | (32u << 16);
  std::memcpy(hdr + 10, &off, 4);
  std::memcpy(hdr + 14, &dib, 4);
  const int32_t sw = int32_t(w), sh = -int32_t(h);  // top-down
  std::memcpy(hdr + 18, &sw, 4);
  std::memcpy(hdr + 22, &sh, 4);
  std::memcpy(hdr + 26, &planesBpp, 4);
  std::memcpy(hdr + 34, &imgBytes, 4);
  std::fwrite(hdr, 1, 54, f);
  std::fwrite(bgra, 1, imgBytes, f);
  std::fclose(f);
}

// (SR_PM4_ORACLE, pm4OracleEnabled: the oracle unit, gfx/d3d12_edram_model.cpp)

// One destination texel (guest bytes with the endian swap undone) as float4.
// Unlisted formats read their first dword as unorm8x4.
void pm4DecodeDest(uint32_t fmt, const uint8_t* p, float c[4]) {
  uint32_t v0 = 0, v1 = 0;
  std::memcpy(&v0, p, 4);
  std::memcpy(&v1, p + 4, 4);
  c[0] = c[1] = c[2] = 0.0f;
  c[3] = 1.0f;
  auto h = [](uint32_t v) { return edram::halfToFloat(uint16_t(v & 0xFFFFu)); };
  switch (fmt) {
    case 7:
      c[0] = float(v0 & 0x3FFu) / 1023.f; c[1] = float((v0 >> 10) & 0x3FFu) / 1023.f;
      c[2] = float((v0 >> 20) & 0x3FFu) / 1023.f; c[3] = float(v0 >> 30) / 3.f;
      break;
    case 22: case 23:
      c[0] = float(v0 >> 8) / 16777215.f; c[1] = float(v0 & 0xFFu) / 255.f;
      break;
    case 24: c[0] = float(v0 & 0xFFFFu) / 65535.f; break;
    case 25: c[0] = float(v0 & 0xFFFFu) / 65535.f; c[1] = float(v0 >> 16) / 65535.f; break;
    case 30: c[0] = h(v0); break;
    case 31: c[0] = h(v0); c[1] = h(v0 >> 16); break;
    case 32: c[0] = h(v0); c[1] = h(v0 >> 16); c[2] = h(v1); c[3] = h(v1 >> 16); break;
    case 36: c[0] = edram::u2f(v0); break;
    case 37: c[0] = edram::u2f(v0); c[1] = edram::u2f(v1); break;
    default:
      for (int k = 0; k < 4; k++) c[k] = float((v0 >> (8 * k)) & 0xFFu) / 255.f;
      break;
  }
}

// Per-frame counters (reported once per rendered batch set; Pm4Stats: pm4_render_detail.hpp).
Pm4Stats g_pm4Stats;

// iter-460: the shared-memory path (d3d12_pm4_shmem.cpp) uses these.
void pm4ShmBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                   D3D12_RESOURCE_STATES b) {
  pm4Barrier(cl, r, a, b);
}
uint8_t* pm4ShmGuestWritable(uint32_t phys, size_t& avail) { return pm4GuestWritable(phys, avail); }
uint8_t* pm4ShmGuestWritableWin(uint32_t phys, size_t& avail, int& win) {
  return pm4GuestWritableWin(phys, avail, win);
}
bool pm4ShmEnabled();

edram::Regs D3D12Backend::pm4RegsFromDraw(const GuestDrawCommand& c) {
  return drainreuse::regsFromCommand(c);  // step 6 R6 fix round 2: the one mapping (gfx/drain_reuse.hpp), shared with its test
}

bool pm4VsfEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_VSF");
    return !e || e[0] != '0';
  }();
  return v;
}

// iter-461 (SR_PM4_VSF_NOIB, default ON): identity-remap auto-indexed draws
// go without an index list.
static bool vsfNoIbEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_VSF_NOIB");
    return !e || e[0] != '0';
  }();
  return v;
}

// iter-460 diagnostic ([IB_ZERO]): an all-zero index buffer names the window
// decision that produced it (the title city's sky strip, draw 2075 of
// archive/p2_cbw1/live_454107ED_2400.xtr, read 591 zero indices). `win` is the
// caller's ownership answer (-1 = the caller did not ask).
void pm4NoteZeroIb(const GuestDrawCommand& c, const uint8_t* ib, int win) {
  const uint32_t n = c.vertexCount;
  if (!ib || n < 16) return;
  const uint32_t span = n * (c.idxFmt32 ? 4u : 2u);
  const uint32_t chk = std::min<uint32_t>(64u, span);
  for (uint32_t b = 0; b < chk; b++)
    if (ib[b]) return;
  static std::atomic<uint32_t> s_z{0};
  const uint32_t k = s_z.fetch_add(1, std::memory_order_relaxed) + 1;
  if (k > 32 && (k & (k - 1)) != 0) return;
  auto zeroAt = [&](uint32_t va) {
    const uint8_t* q = sr::kernel::guestVAToHost(va);
    if (!q || !sr::kernel::isGuestVAInRange(va + span)) return -1;
    for (uint32_t b = 0; b < chk; b++)
      if (q[b]) return 0;
    return 1;
  };
  const uint32_t p = c.idxBasePhys & 0x1FFFFFFFu;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[IB_ZERO] #%u phys=%08X n=%u fmt32=%d win=%d flags=%d pageOwner=%d cbWin=%d "
              "zero8=%d zeroA=%d vs=%016llX",
              k, p, n, c.idxFmt32 ? 1 : 0, win, sr::kernel::physwin::gpuGrantFlags(p, span),
              sr::kernel::physwin::physPageOwner(p), sr::kernel::physwin::commandBufferWindow(p, span),
              zeroAt(0x80000000u + p), zeroAt(0xA0000000u + p), (unsigned long long)c.vsUcodeHash);
}

// iter-460 diagnostic ([IB_RANGE]): an index buffer whose indices reach past
// every vertex buffer the VS fetches is not this draw's index buffer (the
// title's street props read 5555 0001 0000 ... patterns and drew screen-sized
// slabs). Logs the window decision and whether the OTHER window's bytes would
// have been in range.
void pm4NoteIbRange(const GuestDrawCommand& c, const CachedShader& vs, const uint8_t* ib, int win) {
  // A diagnostic ([IB_RANGE]: implausible index ranges) that scanned up to 4096 indices of EVERY
  // indexed draw on the command processor (3.1% of it, perf2 r_wc_prof): opt-in, SR_PM4_IB_NOTES=1.
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PM4_IB_NOTES");
    return e && e[0] == '1';
  }();
  if (!s_on) return;
  const uint32_t n = c.vertexCount;
  if (!ib || n < 4) return;
  uint32_t minBytes = UINT32_MAX;
  for (const uint32_t slot : vs.fetchSlots) {
    if (slot >= 96) continue;
    const uint32_t d1 = c.fetchRaw[slot * 2 + 1];
    minBytes = std::min(minBytes, ((d1 >> 2) & 0xFFFFFFu) * 4u);
  }
  if (minBytes == UINT32_MAX || minBytes == 0) return;
  const uint32_t reset = c.idxFmt32 ? 0xFFFFFFFFu : 0xFFFFu;
  auto maxIndex = [&](const uint8_t* p) {
    uint32_t mx = 0;
    const uint32_t cnt = std::min<uint32_t>(n, 4096u);
    for (uint32_t i = 0; i < cnt; i++) {
      uint32_t v;
      if (c.idxFmt32) {
        std::memcpy(&v, p + size_t(i) * 4u, 4);
        if (c.idxEndian == 2) v = _byteswap_ulong(v);
      } else {
        uint16_t h;
        std::memcpy(&h, p + size_t(i) * 2u, 2);
        v = (c.idxEndian == 1 || c.idxEndian == 2) ? _byteswap_ushort(h) : h;
      }
      if (v == reset) continue;
      mx = std::max(mx, v & 0xFFFFFFu);
    }
    return mx;
  };
  const uint32_t mx = maxIndex(ib);
  if (uint64_t(mx) * 4u < minBytes) return;  // plausible
  static std::atomic<uint32_t> s_k{0};
  const uint32_t k = s_k.fetch_add(1, std::memory_order_relaxed) + 1;
  if (k > 64 && (k & (k - 1)) != 0) return;
  const uint32_t p = c.idxBasePhys & 0x1FFFFFFFu;
  const uint32_t span = n * (c.idxFmt32 ? 4u : 2u);
  auto otherMax = [&](uint32_t va) -> long {
    const uint8_t* q = sr::kernel::guestVAToHost(va);
    if (!q || !sr::kernel::isGuestVAInRange(va + span)) return -1;
    return long(maxIndex(q));
  };
  uint32_t w0 = 0, w1 = 0;
  std::memcpy(&w0, ib, 4);
  std::memcpy(&w1, ib + 4, 4);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[IB_RANGE] #%u phys=%08X n=%u fmt32=%d endian=%u win=%d maxIdx=%u vbMin=%u | "
              "max8=%ld maxA=%ld | flags=%d pageOwner=%d cbWin=%d head=%08X %08X vs=%016llX",
              k, p, n, c.idxFmt32 ? 1 : 0, c.idxEndian, win, mx, minBytes / 4u,
              otherMax(0x80000000u + p), otherMax(0xA0000000u + p),
              sr::kernel::physwin::gpuGrantFlags(p, span), sr::kernel::physwin::physPageOwner(p),
              sr::kernel::physwin::commandBufferWindow(p, span), w0, w1,
              (unsigned long long)c.vsUcodeHash);
}

bool D3D12Backend::pm4StageVsf(const GuestDrawCommand& c, const CachedShader& vs, bool stripCut,
                               uint64_t vbEnd, uint64_t cbEnd, Pm4VsfOut& out) {
  using DClk = std::chrono::steady_clock;
  const auto tFetch0 = g_pm4DiagTiming ? DClk::now() : DClk::time_point{};
  const memres::KindScope mrKind(memres::Kind::kVb);  // mem.resources step 2a: the census's fetch kind (vertex fetches)
#if SR_ORACLE
  // mem.resources memres5 (SR_MEM_RESOURCES=census): the draw's index stream, the bytes the conversion below reads,
  // handed to the census before the fetches below request their bytes - it marks fetched only the vertices those
  // indices address (index-bounded; it falls back to the declared range itself where it cannot). Nothing here changes a
  // byte the draw reads or uploads, and nothing the census computes comes back.
  const uint8_t* mib = nullptr;
  if (memres::census()) {
    const uint32_t ibBytes = c.vertexCount * (c.idxFmt32 ? 4u : 2u);
    if (c.idxBasePhys) {
      size_t avail = 0;
      mib = pm4GuestWritable(c.idxBasePhys, avail);
      if (mib && avail < size_t(ibBytes)) mib = nullptr;
      const int w = sr::kernel::physwin::gpuOwnerWindowEnabled()
                        ? sr::kernel::physwin::gpuWindow(c.idxBasePhys, ibBytes)
                        : 0;
      uint8_t* gbase = nullptr;
      size_t gsz = 0;
      sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
      const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u + c.idxBasePhys);
      const bool altOk = alt && gbase && alt >= gbase && alt + size_t(c.vertexCount) * 4u <= gbase + gsz;
      if (mib && w == 0xA && altOk) mib = alt;
      if (mib && w == 0) {
        bool zero = true;
        for (uint32_t b = 0; b < std::min<uint32_t>(64u, ibBytes); b++)
          if (mib[b]) { zero = false; break; }
        if (zero && altOk) mib = alt;
      }
    }
  }
  // memres6: the VS constants as the constant upload holds them (a tracked command's live file) and the guest fetch
  // constants - the census bounds a computed index (the world family) from them; read only, nothing returns.
  const memres::DrawScope mrDraw(c.vsUcodeHash, c.idxBasePhys != 0u, mib, c.vertexCount, c.idxFmt32, c.idxEndian,
                                 c.indxOffset, c.maxVtxIndx >= c.minVtxIndx, c.minVtxIndx, c.maxVtxIndx,
                                 c.primType == 6u && stripCut, c.primResetIndex, c.constTracked ? m_cvCons : c.aluVs,
                                 c.fetchRaw);
#endif
  // iter-460 perf (SR_PM4_VSF_SHM, default ON): the VS reads its vertices from
  // the shared memory at the guest physical address (Xenia's model) - a fetch
  // costs the page validation (once per epoch per page) and no copy; the
  // staged path below hashed and copied every fetched range per draw (12-15
  // ms of a gameplay frame's drain).
  static const bool s_vsfShm = [] {
    const char* e = std::getenv("SR_PM4_VSF_SHM");
    return !e || e[0] != '0';
  }();
  const bool shmVsf = s_vsfShm && pm4ShmEnabled() && pm4ShmInit() && m_vsfShmSrvReady;
  out.shm = shmVsf;
  // The staged variant owns a packed immutable buffer. Offset zero stays a
  // zero sentinel; neither later draws nor frame-ring reuse can overwrite it.
  std::vector<uint8_t> rawBytes;
  if (!shmVsf) {
    try { rawBytes.assign(64, 0); }
    catch (const std::bad_alloc&) { return false; }
  }
  // Fetch addresses are relative to the owned raw view, or remain physical
  // addresses on the separate development shared-memory path.
  uint32_t fetch[192];
  std::memcpy(fetch, c.fetchRaw, sizeof(fetch));
  for (const uint32_t slot : vs.fetchSlots) {
    if (slot >= 96) continue;
#if SR_ORACLE
    const memres::SlotScope mrSlot(slot);  // mem.resources memres5: the vertex fetch slot the request below serves
#endif
    const uint32_t d0 = c.fetchRaw[slot * 2], d1 = c.fetchRaw[slot * 2 + 1];
    fetch[slot * 2] = d0 & 3u;  // unpopulated / unreadable: reads staged offset 0
    if ((d0 & 3u) != 3u) continue;
    const uint32_t phys = d0 & 0x1FFFFFFCu;
    uint32_t size = ((d1 >> 2) & 0xFFFFFFu) * 4u;
    size_t avail = 0;
    const uint8_t* src = pm4GuestWritable(phys, avail);
    if (!src || !size) continue;
    // iter-466 cp lane (SR_CP_VSF_NOWIN): on the shared-memory path the bytes are read by the
    // VS from the shared memory, where the page validation picks each page's window itself
    // (pm4ShmRequest's range answer). The window chosen below only fed the host pointer's
    // avail clamp - which cannot bite: a fetch is at most 64 MiB and both windows' copies of
    // a 512 MiB physical address lie that far inside the 4 GiB arena - and the live trace.
    // Skipped unless a trace records the bytes (the physwin lookups were ~3% of the CP).
    if (cpsimd::vsfNoWin() && shmVsf && !livetrace::recording()) {
      size = uint32_t(std::min<size_t>(size, avail));
      pm4ShmRequest(phys, size);
      fetch[slot * 2] = phys | 3u;
      continue;
    }
    // iter-460: the allocators' records first (kernel/phys_window.hpp gpuWindow).
    const int ownWin = sr::kernel::physwin::gpuOwnerWindowEnabled()
                           ? sr::kernel::physwin::gpuWindow(phys, size)
                           : 0;
    if (ownWin == 0xA) {
      uint8_t* gbase = nullptr;
      size_t gsz = 0;
      sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
      const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u + phys);
      if (alt && gbase && alt >= gbase && alt < gbase + gsz) {
        src = alt;
        avail = size_t((gbase + gsz) - alt);
      }
    }
    // The movie/UI buffers written through the 0xA window read as zero here
    // under the harness's unfolded windows (SR_VB_ALIAS_PROBE, iter-425).
    if (ownWin == 0) {
      bool zero = true;
      for (uint32_t b = 0; b < std::min<uint32_t>(64u, size) && b < avail; b++)
        if (src[b]) { zero = false; break; }
      if (zero) {
        uint8_t* gbase = nullptr;
        size_t gsz = 0;
        sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
        const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u + phys);
        if (alt && gbase && alt >= gbase && alt < gbase + gsz) {
          bool zeroA = true;
          for (uint32_t b = 0; b < std::min<uint32_t>(64u, size); b++)
            if (alt[b]) { zeroA = false; break; }
          if (!zeroA) {
            src = alt;
            avail = size_t((gbase + gsz) - alt);
          }
        }
      }
    }
    size = uint32_t(std::min<size_t>(size, avail));
    livetrace::memRead(phys, src, size);
    if (shmVsf) {
      pm4ShmRequest(phys, size);  // queued copies, recorded before this draw
      fetch[slot * 2] = phys | 3u;
      continue;
    }
    const uint64_t off = pm4AlignUp(rawBytes.size(), 16);
    // Load4 may include the last fetch's alignment tail. Publish explicit zero
    // padding as part of the snapshot, bounded by the native arena's budget.
    const uint64_t end = pm4AlignUp(off + size, 16) + 16;
    if (end > (256u << 20)) return false;
    try { rawBytes.resize(size_t(end), 0); }
    catch (const std::bad_alloc&) { return false; }
    std::memcpy(rawBytes.data() + off, src, size);
    fetch[slot * 2] = uint32_t(off) | 3u;
  }
  if (!shmVsf && FAILED(m_nativeRawGeometry.stage(rawBytes, 16, out.rawVertices))) return false;
  // iter-466 cp lane (SR_CP_VSF_CBDEDUP): consecutive draws of one mesh / pass bind the same
  // fetch constants - the previous draw's buffer (same third, same bytes) is bound again.
  static const bool s_cbDedup = cpsimd::gateLogged("SR_CP_VSF_CBDEDUP", false);
  // step 6 I10p (M2, REQUIRED by Fable 40): on the view path the dedup is always on - the 768-byte compare against
  // the previous draw's block (the cacheable m_vsfCbLast) before the write; the block itself is written once into the
  // write-only twin (m_xlatCBGpu), never into the scratch.
  const bool cbDedup = s_cbDedup || m_zcOn;
#if SR_ORACLE
  if (m_zcCheck) out.zcFetchHash = xlatCbHash(reinterpret_cast<const uint8_t*>(fetch), ~0u, sizeof(fetch));
#endif
  if (cbDedup && m_vsfCbLastOff != ~0ull && std::memcmp(fetch, m_vsfCbLast, sizeof(fetch)) == 0) {
    out.fetchCbOff = m_vsfCbLastOff;
    m_vsfCbDedup++;
    if (m_zcOn) zcBump(m_zcFetchDedups);
  } else {
    const uint64_t cbOff = pm4AlignUp(m_xlatCBUsed, 256);
    if (cbOff + sizeof(fetch) > cbEnd) return false;
    if (m_zcOn) {
      m_xlatCBGpu.write(cbOff, fetch, sizeof(fetch));
      zcBump(m_zcFetchWrites);
      zcBump(m_zcBytes, sizeof(fetch));
    } else {
      if (!xlatCommit(0, cbOff + sizeof(fetch))) return false;   // step 4 7d(b): commit before the write
      std::memcpy(m_xlatCBPtr + cbOff, fetch, sizeof(fetch));
    }
    m_xlatCBUsed = cbOff + sizeof(fetch);
    out.fetchCbOff = cbOff;
    if (cbDedup) {
      std::memcpy(m_vsfCbLast, fetch, sizeof(fetch));
      m_vsfCbLastOff = cbOff;
    }
  }
  const auto tIb0 = g_pm4DiagTiming ? DClk::now() : DClk::time_point{};
  if (g_pm4DiagTiming) drainreuse::add(g_drainMsVsfFetch, std::chrono::duration<double, std::milli>(tIb0 - tFetch0).count());
  struct IbTimer {
    DClk::time_point t0;
    ~IbTimer() {
      if (g_pm4DiagTiming) drainreuse::add(g_drainMsVsfIb, std::chrono::duration<double, std::milli>(DClk::now() - t0).count());
    }
  } ibTimer{tIb0};

  // Host indices = the VS's SV_VertexID = r0.x: Xenia's RemapAndConvert
  // VertexIndices (offset, 24-bit mask, min/max clamp) on the guest index.
  const uint32_t n = c.vertexCount;
  const uint8_t* ib = nullptr;
  if (c.idxBasePhys) {
    size_t avail = 0;
    ib = pm4GuestWritable(c.idxBasePhys, avail);
    if (ib && avail < size_t(n) * (c.idxFmt32 ? 4u : 2u)) ib = nullptr;
    const int ibWin = sr::kernel::physwin::gpuOwnerWindowEnabled()
                          ? sr::kernel::physwin::gpuWindow(c.idxBasePhys, n * (c.idxFmt32 ? 4u : 2u))
                          : 0;
    if (ib && ibWin == 0xA) {
      uint8_t* gbase = nullptr;
      size_t gsz = 0;
      sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
      const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u + c.idxBasePhys);
      if (alt && gbase && alt >= gbase && alt + size_t(n) * 4u <= gbase + gsz) ib = alt;
    }
    if (ib && ibWin == 0) {
      bool zero = true;
      const uint32_t chk = std::min<uint32_t>(64u, n * (c.idxFmt32 ? 4u : 2u));
      for (uint32_t b = 0; b < chk; b++)
        if (ib[b]) { zero = false; break; }
      if (zero) {  // iter-435: IBs share the 0xA-window blindness
        uint8_t* gbase = nullptr;
        size_t gsz = 0;
        sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
        const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u + c.idxBasePhys);
        if (alt && gbase && alt >= gbase && alt + size_t(n) * 4u <= gbase + gsz) ib = alt;
      }
    }
    if (!ib) return false;
    pm4NoteZeroIb(c, ib, ibWin);
    pm4NoteIbRange(c, vs, ib, ibWin);
    livetrace::memRead(c.idxBasePhys, ib, n * (c.idxFmt32 ? 4u : 2u));
    // mem.resources step 2a (SR_MEM_RESOURCES=census): the conversion's read graded as a would-be IB cache keyed on
    // the registry (the census hashes the bytes itself; nothing here changes which bytes are converted).
    if (memres::census()) {
      const bool fromA = ib == sr::kernel::guestVAToHost(0xA0000000u + c.idxBasePhys);
      memres::ibRead(c.idxBasePhys, n * (c.idxFmt32 ? 4u : 2u), ib,
                     ibWin == 0xA ? memres::Prov::kA
                     : ibWin == 8 ? memres::Prov::k8
                     : fromA      ? memres::Prov::kLegacyA
                                  : memres::Prov::kLegacy8);
    }
  }
  auto raw = [&](uint32_t i) -> uint32_t {
    if (!ib) return i;
    if (c.idxFmt32) {
      uint32_t v;
      std::memcpy(&v, ib + size_t(i) * 4u, 4);
      if (c.idxEndian == 2) v = _byteswap_ulong(v);
      else if (c.idxEndian == 3) v = (v >> 16) | (v << 16);
      else if (c.idxEndian == 1) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      return v;
    }
    uint16_t v;
    std::memcpy(&v, ib + size_t(i) * 2u, 2);
    if (c.idxEndian == 1 || c.idxEndian == 2) v = _byteswap_ushort(v);
    return v;
  };
  const bool clamp = c.maxVtxIndx >= c.minVtxIndx;
  auto remap = [&](uint32_t r) -> uint32_t {
    uint32_t v = (r + c.indxOffset) & 0xFFFFFFu;
    if (clamp) v = std::min(std::max(v, c.minVtxIndx), c.maxVtxIndx);
    return v;
  };
  // iter-461: an auto-indexed list / strip / point draw whose remap is the
  // identity (no reset, the offset range inside the clamp) draws WITHOUT an
  // index list - SV_VertexID = StartVertexLocation + i IS remap(i). 60% of a
  // gameplay frame's draws are auto-indexed (gp1: 2314 of 3854).
  if (!ib && !stripCut && n && (c.primType == 4u || c.primType == 6u || c.primType == 1u)) {
    const uint32_t first = c.indxOffset & 0xFFFFFFu;
    const bool identity = uint64_t(first) + n <= 0x1000000ull &&
                          (!clamp || (first >= c.minVtxIndx && first + n - 1u <= c.maxVtxIndx));
    if (identity && vsfNoIbEnabled()) {
      out.noIb = true;
      out.firstVertex = first;
      out.ibOffset = 0;
      out.indexCount = n;
      out.topo = c.primType == 6u   ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                 : c.primType == 1u ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                                    : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      drainreuse::add(g_drainNoIb, 1u);
      return true;
    }
  }
  drainreuse::add(g_drainIb, 1u);
#if SR_ORACLE
  // iter-466 cp lane (SR_CP_IB_CACHE, needs SR_SHM_WRITE_TRACK): the depth prepass, the colour
  // pass and the shadow passes draw the same meshes - a guest index buffer converted earlier in
  // this set, whose pages are still under the write-protection generation they were converted
  // in (Xenia's primitive_processor conversion cache, invalidated by its memory watch), reuses
  // that host index list. The pages are protected BEFORE they are read.
  static const bool s_ibCache = sr::kernel::gww::enabled() && cpsimd::gateLogged("SR_CP_IB_CACHE", false);
  uint64_t ibKey = 0;
  uint32_t ibP0 = 0, ibNP = 0, ibGens[8] = {};
  bool ibCacheable = false;
  if (s_ibCache && ib && !livetrace::recording()) {
    const uint32_t bytes = n * (c.idxFmt32 ? 4u : 2u);
    const uint32_t a = c.idxBasePhys & 0x1FFFFFFFu;
    ibP0 = a >> 12;
    ibNP = uint32_t(((uint64_t(a) + bytes - 1u) >> 12) - ibP0 + 1u);
    if (bytes && ibNP <= 8u) {
      struct {
        uint64_t host;
        uint32_t n, fmt, endian, off, mn, mx, reset, cut, prim;
      } k{uint64_t(reinterpret_cast<uintptr_t>(ib)), n, c.idxFmt32 ? 1u : 0u, c.idxEndian, c.indxOffset,
          c.minVtxIndx, c.maxVtxIndx, c.primResetIndex, stripCut ? 1u : 0u, c.primType};
      ibKey = ShaderCache::hashUcode(&k, sizeof(k));
      auto hit = m_ibConv.find(ibKey);
      if (hit != m_ibConv.end() && hit->second.p0 == ibP0 && hit->second.nPages == ibNP) {
        bool same = true;
        for (uint32_t i = 0; i < ibNP && same; i++)
          same = hit->second.gens[i] && sr::kernel::gww::protGen(ibP0 + i) == hit->second.gens[i];
        if (same) {
          out.ibOffset = hit->second.ibOffset;
          out.indexCount = hit->second.indexCount;
          out.topo = hit->second.topo;
          m_ibConvHits++;
          // mem-native: the reused list against the census's used-vertex bitmap (vtx_stale)
          memres::censusCheckUsed(reinterpret_cast<const uint32_t*>(m_xlatVBPtr + hit->second.ibOffset),
                                  hit->second.indexCount);
          return true;
        }
      }
      uint32_t pages[8];
      for (uint32_t i = 0; i < ibNP; i++) pages[i] = ibP0 + i;
      sr::kernel::gww::protectForValidation(pages, ibNP);
      ibCacheable = true;
      for (uint32_t i = 0; i < ibNP; i++) {
        ibGens[i] = sr::kernel::gww::protGen(ibP0 + i);
        if (!ibGens[i]) ibCacheable = false;
      }
    }
  }
#endif  // SR_ORACLE
  const uint64_t ibOff = pm4AlignUp(m_xlatVBUsed, 16);
  const uint64_t cap = n >= 3 ? uint64_t(n) * 3u : uint64_t(n);  // fan/quad expansion bound
  if (ibOff + cap * 4u > vbEnd) return false;
  if (!xlatCommit(ibOff + cap * 4u, 0)) return false;   // step 4 7d(b): commit before the writes
  uint32_t* dst = reinterpret_cast<uint32_t*>(m_xlatVBPtr + ibOff);
  uint32_t cnt = 0;
  // iter-466 cp lane (SR_CP_IB_SSE41): the strip / list / point remaps as SSE4.1 kernels -
  // swap, reset-cut, offset, 24-bit mask and clamp exactly as raw() / remap() below (the
  // strip loop alone was ~4% of the CP, archive/v1_hfr/b4_g line profile).
  static const bool s_ibSse41 = cpsimd::gateLogged("SR_CP_IB_SSE41", true) && cpsimd::available();
  const uint32_t simdPrim = c.primType;
  if (s_ibSse41 && ib && (simdPrim == 6u || simdPrim == 1u || simdPrim == 4u)) {
    cpsimd::RemapParams rp{};
    rp.offset = c.indxOffset;
    rp.minV = c.minVtxIndx;
    rp.maxV = c.maxVtxIndx;
    rp.resetIndex = c.primResetIndex;
    rp.endian = c.idxEndian;
    rp.clamp = clamp;
    rp.cut = simdPrim == 6u && stripCut;
    rp.idx32 = c.idxFmt32;
    cnt = cpsimd::remapIndices(dst, ib, n, rp);
    out.topo = simdPrim == 6u   ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
               : simdPrim == 1u ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  } else
  switch (c.primType) {
    case 5:  // kTriangleFan -> list
      for (uint32_t i = 2; i < n; i++) {
        dst[cnt++] = remap(raw(0));
        dst[cnt++] = remap(raw(i - 1));
        dst[cnt++] = remap(raw(i));
      }
      out.topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    case 13: {  // kQuadList -> list: Xenia's GS split (0,1,3)(3,1,2); SR_PM4_QUAD_STRIP=0: (0,1,2)(0,2,3)
      const int* sp = pm4QuadSplit();  // iter-467 freezefx (gfx/pm4_quad_gate.hpp)
      for (uint32_t q = 0; q + 4 <= n; q += 4) {
        const uint32_t vv[4] = {remap(raw(q)), remap(raw(q + 1)), remap(raw(q + 2)), remap(raw(q + 3))};
        for (int k = 0; k < 6; k++) dst[cnt++] = vv[sp[k]];
      }
      out.topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    }
    case 6:  // kTriangleStrip; primitive reset -> strip cut
      for (uint32_t i = 0; i < n; i++) {
        const uint32_t r = raw(i);
        dst[cnt++] = (stripCut && r == c.primResetIndex) ? 0xFFFFFFFFu : remap(r);
      }
      out.topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      break;
    case 1:
      for (uint32_t i = 0; i < n; i++) dst[cnt++] = remap(raw(i));
      out.topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      break;
    default: {  // kTriangleList
      // iter-465 native lane (CP costs; perf2 r_wc_prof: this loop ~7% of the CP): 16-bit
      // indices with a byte swap (endian 1 / 2) and a clamp that cannot bite (the full 24-bit
      // range) go 8 at a time - (swap(r) + offset) & 0xFFFFFF, the scalar remap exactly.
      static const bool s_simd = [] {
        const char* e = std::getenv("SR_PM4_IB_SIMD");
        return !e || e[0] != '0';
      }();
      uint32_t i = 0;
      if (s_simd && ib && !c.idxFmt32 && (c.idxEndian == 1 || c.idxEndian == 2) &&
          (!clamp || (c.minVtxIndx == 0 && c.maxVtxIndx >= 0xFFFFFFu))) {
        const __m128i off = _mm_set1_epi32(int(c.indxOffset)), m24 = _mm_set1_epi32(0xFFFFFF);
        const __m128i zero = _mm_setzero_si128();
        for (; i + 8 <= n; i += 8) {
          __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ib + size_t(i) * 2u));
          v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
          const __m128i lo = _mm_and_si128(_mm_add_epi32(_mm_unpacklo_epi16(v, zero), off), m24);
          const __m128i hi = _mm_and_si128(_mm_add_epi32(_mm_unpackhi_epi16(v, zero), off), m24);
          _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + cnt), lo);
          _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + cnt + 4), hi);
          cnt += 8;
        }
      }
      for (; i < n; i++) dst[cnt++] = remap(raw(i));
      out.topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    }
  }
#if SR_ORACLE
  memres::censusCheckUsed(dst, cnt);  // mem-native: the converted list against the census's used-vertex bitmap
#endif
  m_xlatVBUsed = ibOff + uint64_t(cnt) * 4u;
  out.ibOffset = ibOff;
  out.indexCount = cnt;
#if SR_ORACLE
  if (ibCacheable) {  // still the generations the conversion read under: remember it
    bool same = true;
    for (uint32_t i = 0; i < ibNP && same; i++) same = sr::kernel::gww::protGen(ibP0 + i) == ibGens[i];
    if (same) {
      IbConv e{};
      e.ibOffset = ibOff;
      e.indexCount = cnt;
      e.p0 = ibP0;
      e.nPages = ibNP;
      e.topo = out.topo;
      std::memcpy(e.gens, ibGens, sizeof(ibGens));
      m_ibConv[ibKey] = e;
    }
    m_ibConvMiss++;
  }
#endif  // SR_ORACLE
  // iter-460 hang hunt (SR_PM4_VSF_TRACE=<vs hash hex>): the draw's index and
  // fetch data, and the largest vertex address it can make the VS read.
  static const uint64_t s_traceVs = [] {
    const char* e = std::getenv("SR_PM4_VSF_TRACE");
    return e ? std::strtoull(e, nullptr, 16) : 0ull;
  }();
  auto fbits = [](uint32_t u) { float f; std::memcpy(&f, &u, 4); return double(f); };
  if (s_traceVs && c.vsUcodeHash == s_traceVs) {
    static std::atomic<uint32_t> s_tn{0};
    if (s_tn.fetch_add(1) < 400) {
      uint32_t cuts = 0, maxIdx = 0, minIdx = UINT32_MAX;
      for (uint32_t i = 0; i < cnt; i++) {
        if (dst[i] == 0xFFFFFFFFu) { cuts++; continue; }
        maxIdx = std::max(maxIdx, dst[i]);
        minIdx = std::min(minIdx, dst[i]);
      }
      char rawb[200];
      int rp = 0;
      for (uint32_t i = 0; i < std::min<uint32_t>(n, 16u) && rp < 180; i++)
        rp += std::snprintf(rawb + rp, sizeof(rawb) - rp, " %X", raw(i));
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[VSF_TRACE] prim=%u n=%u out=%u idx32=%d endian=%u reset=%08X stripCut=%d "
                  "min=%u max=%u offs=%u clamp=%d -> host idx [%u..%u] cuts=%u c4=(%g,%g,%g,%g) | raw:%s",
                  c.primType, n, cnt, c.idxFmt32 ? 1 : 0, c.idxEndian, c.primResetIndex,
                  stripCut ? 1 : 0, c.minVtxIndx, c.maxVtxIndx, c.indxOffset, clamp ? 1 : 0,
                  minIdx, maxIdx, cuts, fbits((c.constTracked ? m_cvCons : c.aluVs)[16]),
                  fbits((c.constTracked ? m_cvCons : c.aluVs)[17]), fbits((c.constTracked ? m_cvCons : c.aluVs)[18]),
                  fbits((c.constTracked ? m_cvCons : c.aluVs)[19]), rawb);
      for (const uint32_t slot : vs.fetchSlots) {
        if (slot >= 96) continue;
        const uint32_t d0 = c.fetchRaw[slot * 2], d1 = c.fetchRaw[slot * 2 + 1];
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[VSF_TRACE]   slot %u: guest phys=%08X size=%u bytes type=%u -> staged off=%u",
                    slot, d0 & 0x1FFFFFFCu, ((d1 >> 2) & 0xFFFFFFu) * 4u, d0 & 3u,
                    fetch[slot * 2] & ~3u);
      }
    }
  }
  return cnt != 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// iter-467 native-b B1 (SR_NB_CLEAR_ELIDE, baked ON 2026-10-07, =0 A/B): a transfer into a surface that the claiming
// draw rewrites completely is not needed. The draws known to rewrite everything they cover are the
// XDK's own EDRAM clears: D3DDeviceClear_sub_821013D0 loads a fixed shader pair by IM_LOAD_IMMEDIATE
// (VS = the 27 dwords at 0x820BA0D0, PS = the 9 dwords at 0x820B59DC: no kill, no depth export) and
// D3DDeviceEmitClearRectVertices_sub_82100A28 draws an auto-indexed rect list whose vertices it writes
// into the command buffer (vertex fetch 0, 7 floats a vertex, screen-space x / y, minus 0.5 when
// PA_SU_VTX_CNTL.pix_center = 0). With colour mask F, blending ONE / ZERO and no alpha test it rewrites
// every colour sample inside the rectangle; with z ALWAYS + write and stencil ALWAYS / REPLACE / write
// mask FF every depth and stencil sample (RB_DEPTHCONTROL 0x8777, the XDK's own value). This records the
// rectangle (surface pixels) and the bindings it rewrites; renderPm4Batches drops the moved tiles inside
// it. The shaders are identified by hashing the XDK's bytes in the image, not by a constant.
// iter-467 native-b: the NATIVE METER's EDRAM side (Pm4EdramMeter, gfx/d3d12_backend.hpp). The render thread
// counts into m_meterCur; pm4MeterPublish copies it here at the end of each PM4 frame.
namespace {
std::mutex g_meterMu;
Pm4EdramMeter g_meterLast;
bool g_meterValid = false;
}  // namespace

bool pm4EdramMeterLast(Pm4EdramMeter* out) {
  std::lock_guard<std::mutex> lk(g_meterMu);
  if (!g_meterValid) return false;
  if (out) *out = g_meterLast;
  return true;
}

bool nbSurfTableOn();  // EDRAM zero step 3 (below)

void D3D12Backend::pm4MeterMoves(const std::vector<Pm4Move>& moves) {
  for (const Pm4Move& mv : moves) {
    const uint32_t n = mv.end - mv.start;
    if (mv.src.empty()) {
      m_meterCur.edramLoads++;
      m_meterCur.edramLoadTiles += n;
    } else if (nbSurfTableOn()) {  // EDRAM zero step 3: decided by the surface table, not the tile map
      m_meterCur.surfaceOps++;
      m_meterCur.surfaceOpTiles += n;
    } else {
      m_meterCur.transfers++;
      m_meterCur.transferTiles += n;
    }
  }
}

void D3D12Backend::pm4MeterPublish() {
  static uint64_t s_frames = 0;
  m_meterCur.frame = ++s_frames;
  // iter-467 NATIVE METER (kernel/native_meter.hpp): only what still went through the EDRAM model this frame -
  // resolves via the EDRAM buffer or the CPU mirror (E / C; native and target resolves are not counted),
  // ownership transfers between surface keys (moves performed: B1a's elided ones are not), loads of unowned
  // tiles from the EDRAM buffer.
  namespace nm = sr::native::meter;
  if (m_meterCur.resolvesEdram) nm::add(nm::kEdramResolve, m_meterCur.resolvesEdram);
  // C1 stage 3: edram_xfer = the moves EXECUTED through the word model (bufferMoves), whatever decided them - the
  // map-decided count (transfers) stays in the [NB_METER] line (it is 0 with the surface table deciding).
  if (m_meterCur.bufferMoves) nm::add(nm::kEdramTransfer, m_meterCur.bufferMoves);
  if (m_meterCur.edramLoads) nm::add(nm::kEdramLoad, m_meterCur.edramLoads);
  {
    std::lock_guard<std::mutex> lk(g_meterMu);
    g_meterLast = m_meterCur;
    g_meterValid = true;
  }
  // [NB_METER]: per-frame averages over 120 frames, for the logs (readers use pm4EdramMeterLast).
  static double s_sum[14] = {};
  static uint32_t s_n = 0;
  const uint32_t v[14] = {m_meterCur.resolvesTarget, m_meterCur.resolvesGuestMem, m_meterCur.resolvesEdram,
                          m_meterCur.transfers,      m_meterCur.transferTiles,    m_meterCur.elidedTiles,
                          m_meterCur.edramLoads,     m_meterCur.edramLoadTiles,   m_meterCur.resolvedTexLoads,
                          m_meterCur.materializations, m_meterCur.surfaceOps,   m_meterCur.surfaceOpTiles,
                          m_meterCur.bufferMoves,    m_meterCur.bufferMoveTiles};
  for (int i = 0; i < 14; i++) s_sum[i] += v[i];
  if (s_frames <= 8)  // the first frames one by one (a trace replay renders a handful)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NB_METER] frame %llu: resolves target %u guest-mem %u edram %u | transfers %u (%u tiles), elided %u "
                "tiles | edram loads %u (%u tiles) | resolved-texture loads %u | materializations %u | surface ops %u "
                "(%u tiles) | buffer moves %u (%u tiles)",
                (unsigned long long)s_frames, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
                v[12], v[13]);
  if (++s_n == 120) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NB_METER] per frame (120-frame avg, frame %llu): resolves target %.1f guest-mem %.1f edram %.1f | "
                "transfers %.1f (%.0f tiles), elided %.0f tiles | edram loads %.1f (%.0f tiles) | resolved-texture "
                "loads %.1f | materializations %.1f | surface ops %.1f (%.0f tiles) | buffer moves %.1f (%.0f tiles)",
                (unsigned long long)m_meterCur.frame, s_sum[0] / 120, s_sum[1] / 120, s_sum[2] / 120, s_sum[3] / 120,
                s_sum[4] / 120, s_sum[5] / 120, s_sum[6] / 120, s_sum[7] / 120, s_sum[8] / 120, s_sum[9] / 120,
                s_sum[10] / 120, s_sum[11] / 120, s_sum[12] / 120, s_sum[13] / 120);
    for (double& s : s_sum) s = 0;
    s_n = 0;
    if (m_bindMemoHits + m_bindMemoMiss)  // SR_NB_BIND_MEMO
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_BIND_MEMO] slot binds reused %llu, looked up %llu (%.1f%% reused)",
                  (unsigned long long)m_bindMemoHits, (unsigned long long)m_bindMemoMiss,
                  100.0 * double(m_bindMemoHits) / double(m_bindMemoHits + m_bindMemoMiss));
  }
  m_meterCur = Pm4EdramMeter{};
}

static bool pm4NbClearElideOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_CLEAR_ELIDE");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  static const int ab = sr::abt::reg("SR_NB_CLEAR_ELIDE");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}
// step 6 R6 fix round 2 (Astra r2 P2a): the drain reuse keys this gate (gfx/d3d12_backend.cpp).
bool pm4NbClearElideOnForKey() { return pm4NbClearElideOn(); }

// iter-467 native-b B1c (SR_NB_CLEAR_PROP, baked ON after check roll cr4_nb; =0 off): the XDK clears the console's way - a 4x-MSAA surface or a
// depth surface aliasing the target's tiles, then the real surface takes the tiles over - so the EDRAM model moves
// the cleared tiles into the real surface (C3m>C3 + D1m>D1, D0m>C4m / C0 / C6: 0.5 ms of the 0.78 ms of transfers in
// a 3x gameplay frame). With this gate the uniform value the clear left is known (its vertex colour / depth 0 and
// stencil reference), so such a move is a host ClearRenderTargetView / ClearDepthStencilView of the destination
// with the value the transfer would compute (the store's pack, the load's unpack; gfx/edram.hpp ports of both), and
// a clear claims only the rows it covers. A value the destination format cannot hold exactly keeps the transfer
// (SR_NB_CLEAR_PROP_ROUND=1, diagnostic: clear with the round-to-nearest-even value instead).
// SR_NB_CLEAR_PROP_VERIFY=1 (diagnostic): run the transfers, then read the destination back and compare.
// iter-467 native-b B1b (SR_NB_COVER_FILL, baked ON after check roll cr4_nb; =0 off): the coverage proof done by the GPU. A draw with no depth /
// stencil binding whose colour bindings are overwritten wherever it rasterizes (no blending, every channel of the
// host format written) does not need the previous owners' pixels it covers - only the ones it leaves. So it is
// drawn FIRST, marking its coverage in a scratch stencil (stencil REPLACE 1, a cached PSO variant), and the moves
// run AFTER it with a stencil test (EQUAL 0): only the pixels the draw did not write receive the old content. A
// full-screen post-process quad then moves nothing (the opaque quads that claim a reinterpreted surface: C0>C0,
// C3>C0, C0>C6, C14>C0 ...); a partial or discarding draw stays exact. Moves that are not direct host-RT copies,
// into depth, or MRT bindings of different sizes keep the order transfer -> draw.
bool nbCoverFillOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_COVER_FILL");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON after check roll cr4_nb (=0 A/B)
  }();
  static const int ab = sr::abt::reg("SR_NB_COVER_FILL");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}

// iter-467 native-b EDRAM zero (SR_NB_ZERO_INIT, baked ON; =0 off; SR_AB_TOGGLE): a tile no surface has claimed since the
// renderer started holds what the EDRAM held at power-on - zero in a live run (the GPU EDRAM buffer and the mirror
// start zeroed and only owners' tiles are ever stored into them), the trace's snapshot in a 1x replay (then the
// gate does nothing: m_pm4EdramPristine). So (T5) a "load" of such tiles is a host clear of the destination to the
// value zero bits unpack to - 0 in every colour format, depth 0 / stencil 0 - and (R0) a resolve whose span is
// unclaimed tiles (plus the source's own) reads zeros: the source's host RT is cleared there and the resolve takes
// the native path. Before this, job34 (fr32): the run's only EDRAM-model resolve was the very first one (meter
// frame 0, 00C08000 1280x720, never drawn) and its 2 EDRAM loads were T5.
bool nbZeroInitOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_ZERO_INIT");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON after job35 / job37 (=0 A/B)
  }();
  static const int ab = sr::abt::reg("SR_NB_ZERO_INIT");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
#else
  return true;  // C1 stage 3 (3.1 rule 2): the =0 arm loads unclaimed tiles from the EDRAM buffer - a fallback component
#endif
}
uint64_t g_nbZeroTiles = 0, g_nbZeroMoves = 0, g_nbZeroRes = 0;
// iter-467 native-b EDRAM zero (SR_NB_RESOLVE_CLAIM, baked ON; =0 off; SR_AB_TOGGLE): see pm4ExecResolve.
bool nbResolveClaimOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_RESOLVE_CLAIM");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON after job37 (=0 A/B)
  }();
  static const int ab = sr::abt::reg("SR_NB_RESOLVE_CLAIM");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
#else
  return true;  // C1 stage 3 (3.1 rule 2): the =0 arm resolves such spans through the EDRAM model - a fallback component
#endif
}
std::string nbNatStatus();  // gfx/d3d12_pm4_shmem.cpp: the native resolve path's readiness / refusal counts
void natNoteDeclined(bool owned);  // gfx/d3d12_pm4_shmem.cpp: the census of the resolves the kernel did not take
// iter-467 native-b EDRAM zero step 2 (SR_NB_SURF_VERIFY=1, diagnostic): the surface table (gfx/nb_surfaces.hpp) decides
// every claim and every owner query beside the tile map; the answers must be identical ([NB_SURF] lines).
// C1 stage 3: the table is the alias table the shipping path decides with; the map and the verify live in the oracle
// unit (gfx/d3d12_edram_model.cpp).
nbsurf::Table& nbSurfTable() {
  static nbsurf::Table t;
  return t;
}
// iter-467 native-b B-CPU (SR_NB_CLAIM_FAST, baked ON after cr2; =0 off): nearly every batch claims tiles its own
// binding already owns - owned whole by the claimer: no unowned tile, no transfer, no table change (a performance A/B of
// native work; shared with the oracle unit's map decision).
bool nbClaimFastOn() {
  static const bool s_fast = [] {
    const char* e = std::getenv("SR_NB_CLAIM_FAST");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  static const int s_fastAb = sr::abt::reg("SR_NB_CLAIM_FAST");  // SR_AB_TOGGLE
  return s_fast && (s_fastAb < 0 || sr::abt::on(s_fastAb));
}
// SR_NB_SURF_TABLE (EDRAM zero step 3, baked ON; =0 off - the tile map decides again; SR_AB_TOGGLE is NOT offered: the
// two structures diverge once one stops being updated): the surface table decides every claim and resolve span; the
// tile map is not consulted (kept up to date only under SR_NB_SURF_VERIFY, as the oracle). job40 (fr38): replays 9/9
// identical to the map deciding at 1x and 3x, 78/78 resolve hashes x 6 at 3x, 0 mismatches against the map in every
// claim / owner query (replays and a live roll); live 1x / 3x whole runs edram_res 0, edram_xfer 0, edram_load 0 (the
// moves count as surface ops); flicker gate fg40 PASS. A native D3D12 renderer tracks exactly this for aliased
// resources: which surface last wrote each range.
bool nbSurfTableOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_SURF_TABLE");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;  // C1 stage 3 (3.1 rule 2): the =0 arm (the tile map decides) is the oracle unit's - a fallback component
#endif
}

bool nbClearPropOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_CLEAR_PROP");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON after check roll cr4_nb (=0 A/B)
  }();
  static const int ab = sr::abt::reg("SR_NB_CLEAR_PROP");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
}

namespace {
// One key's tiles that an XDK clear rewrote uniformly: its surface-pixel rect and the host value it holds.
struct NbUniform {
  int32_t rect[4] = {};  // [x0, y0, x1, y1), the key's surface pixels
  bool depth = false;
  float host[4] = {};    // colour: the host texel (exact in the key's host format)
  uint32_t stencil = 0;  // depth: host depth 0 and this stencil
  uint32_t draw = 0;
};
std::unordered_map<uint32_t, NbUniform> g_nbUni;
uint64_t g_nbPropTiles = 0, g_nbPropMoves = 0, g_nbPropKept = 0;

// After the clear's draw: what it left in each fully written binding (colour 0, depth). sc = the host scissor the
// draw was bound with (already clamped to the bound targets).
void nbUniRecord(const edram::DrawTargets& t, const D3D12_RECT& sc, uint32_t draw) {
  const resscale::Scale& rs = resscale::get();
  const int32_t sx = int32_t(rs.x), sy = int32_t(rs.y);
  NbUniform u;
  u.rect[0] = std::max(t.fullRect[0], int32_t((sc.left + sx - 1) / sx));
  u.rect[1] = std::max(t.fullRect[1], int32_t((sc.top + sy - 1) / sy));
  u.rect[2] = std::min(t.fullRect[2], int32_t(sc.right / sx));
  u.rect[3] = std::min(t.fullRect[3], int32_t(sc.bottom / sy));
  u.draw = draw;
  if (u.rect[2] <= u.rect[0] || u.rect[3] <= u.rect[1]) return;
  if ((t.fullWrite & 1u) && (t.clearValid & 1u) && t.colorWriteMask[0]) {
    // the host texel the clear wrote: its colour through the key's host format, kept only when exact
    const edram::HostFmt hf = t.color[0].hostFormat();
    uint8_t hb[8] = {};
    edram::float4ToHost(hf, t.clearColor, hb);
    float h[4];
    edram::hostToFloat4(hf, hb, h);
    bool exact = true;
    for (int k = 0; k < 4; k++) exact = exact && edram::f2u(h[k]) == edram::f2u(t.clearColor[k]);
    if (exact) {
      NbUniform c = u;
      std::memcpy(c.host, h, sizeof(h));
      g_nbUni[t.color[0].v] = c;
    }
  }
  if ((t.fullWrite & 0x10u) && (t.clearValid & 0x10u) && t.depthBound) {
    NbUniform d = u;
    d.depth = true;
    d.stencil = t.clearStencil;
    g_nbUni[t.depth.v] = d;
  }
}

// pm4HostRect (defined further down): a guest-pixel rect in host pixels.
edram::Rect nbHostRect(const edram::Rect& r) {
  const resscale::Scale& rs = resscale::get();
  return edram::Rect{r.x * rs.x, r.y * rs.y, r.w * rs.x, r.h * rs.y};
}

// Every tile of [start, end) of `k` lies inside the record's rect.
bool nbUniCovers(edram::RtKey k, const NbUniform& u, uint32_t start, uint32_t end) {
  const uint32_t pitch = k.pitchTiles();
  if (!pitch || start >= end) return false;
  const int32_t tw = int32_t(k.tileWidthPixels()), th = int32_t(k.tileHeightPixels());
  for (uint32_t tt = start; tt < end; tt++) {
    const uint32_t rel = (tt + edram::kTileCount - k.base()) % edram::kTileCount;
    const int32_t col = int32_t(rel % pitch), row = int32_t(rel / pitch);
    if (col * tw < u.rect[0] || (col + 1) * tw > u.rect[2] || row * th < u.rect[1] || (row + 1) * th > u.rect[3])
      return false;
  }
  return true;
}

// The destination value of a move out of uniform tiles: what the transfer computes (PackColor / the depth dword of
// the source, UnpackColor / the depth load of the destination), as the host value a clear writes. false = keep the
// transfer (the 64 -> 32 bpp halves differ, or the value is not exact in the destination format).
bool nbPropValue(edram::RtKey src, edram::RtKey dst, const NbUniform& u, float col[4], float& depth,
                 uint32_t& stencil, uint64_t& bitsOut) {
  uint64_t bits = u.depth ? uint64_t(edram::packDepth(src.format(), 0.0f, u.stencil))
                          : edram::packColor(src.format(), u.host);
  const bool s64 = src.is64bpp(), d64 = dst.is64bpp();
  if (!s64 && d64) {
    bits = (bits & 0xFFFFFFFFull) | (bits << 32);  // a 64bpp sample spans two uniform 32bpp ones
  } else if (s64 && !d64) {
    if (uint32_t(bits) != uint32_t(bits >> 32)) return false;  // alternate halves: not uniform at 32bpp
    bits &= 0xFFFFFFFFull;
  }
  bitsOut = bits;
  static const bool s_round = [] {
    const char* e = std::getenv("SR_NB_CLEAR_PROP_ROUND");
    return e && e[0] == '1';
  }();
  if (dst.isDepth()) {
    const uint32_t dw = uint32_t(bits), d24 = dw >> 8;
    // D24S8's float -> unorm24 conversion of a clear is the driver's: only the two end points are certain
    if (dst.format() != 1u && d24 != 0u && d24 != 0xFFFFFFu && !s_round) return false;
    depth = edram::unpackDepth(dst.format(), dw);
    stencil = dw & 0xFFu;
    return true;
  }
  float v[4];
  edram::unpackColor(dst.format(), bits, v);
  const edram::HostFmt hf = dst.hostFormat();
  uint8_t hb[8] = {};
  edram::float4ToHost(hf, v, hb);
  edram::hostToFloat4(hf, hb, col);
  if (hf == edram::HostFmt::RGBA16F || hf == edram::HostFmt::RG16F) {
    // a half the transfer's float would round: equal only if the float is a half already
    bool exact = true;
    for (int k = 0; k < 4; k++) exact = exact && edram::f2u(col[k]) == edram::f2u(v[k]);
    if (!exact && !s_round) return false;
  }
  return true;
}

uint64_t nbImageUcodeHash(uint32_t va, uint32_t bytes) {
  uint8_t* gbase = nullptr;
  size_t gsz = 0;
  sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (!gbase || !p || p < gbase || p + bytes > gbase + gsz) return 0;
  return ShaderCache::hashUcode(p, bytes);
}

// atApplies (step 4 lane C round 4, Astra P2 4): the draw's alpha test can reject pixels (gfx/alpha_test.hpp
// applies()) - then neither its depth / stencil is fully written nor is it a uniform depth clear.
void nbClearCoverage(const GuestDrawCommand& c, edram::DrawTargets& t, bool atApplies) {
  t.fullWrite = 0;
  // The first rect-list draws with the clear's VS: every input the decision reads (one line each).
  static uint32_t s_dbg = 0;
  if (c.primType == 8u && c.vsUcodeHash == 0x0A6D1DD7767FDF27ull && s_dbg < 24) {
    s_dbg++;
    size_t av = 0;
    int w = 0;
    const uint8_t* p = pm4GuestWritableWin(c.fetchRaw[0] & 0x1FFFFFFCu, av, w);
    uint32_t raw0 = 0, raw1 = 0;
    if (p && av >= 8) {
      std::memcpy(&raw0, p, 4);
      std::memcpy(&raw1, p + 4, 4);
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NB_ELIDE] cand #%u draw=%u verts=%u idx=%08X ps=%016llX vte=%08X scmode=%08X winoff=%08X "
                "f0=%08X f1=%08X vb=%p avail=%zu raw=%08X %08X vtxCntl=%08X colorCtl=%08X blend0=%08X mask=%08X "
                "cwm=%X depth=%d dc=%08X sref=%08X srefBF=%08X",
                s_dbg, c.drawOrdinal, c.vertexCount, c.idxBasePhys, (unsigned long long)c.psUcodeHash, c.vteCntl,
                c.suScModeCntl, c.windowOffset, c.fetchRaw[0], c.fetchRaw[1], (const void*)p, av, raw0, raw1,
                c.vtxCntl, c.colorControl, c.blendControl, c.colorMask, unsigned(t.colorWriteMask[0]),
                t.depthBound ? 1 : 0, c.depthControl, c.stencilRefMask, c.stencilRefMaskBF);
  }
  if (c.primType != 8u || c.vertexCount != 3u || c.idxBasePhys != 0u) return;
  // XXH3-64 of the XDK's clear VS (27 dwords at 0x820BA0D0) and PS (9 dwords at 0x820B59DC), the same
  // hash the CP gives every IM_LOAD_IMMEDIATE (ENGINE_MAP 6g.8a). A trace replay has no XEX image in
  // guest memory, so the identity is the hash itself; with the image present (live) it is checked
  // against the image bytes once.
  constexpr uint64_t kXdkClearVs = 0x0A6D1DD7767FDF27ull, kXdkClearPs = 0x2E372EA28CC404B7ull;
  static const bool s_checked = [] {
    const uint64_t vs = nbImageUcodeHash(0x820BA0D0u, 27u * 4u), ps = nbImageUcodeHash(0x820B59DCu, 9u * 4u);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_ELIDE] XDK clear shader pair %016llX / %016llX: image %s",
                (unsigned long long)kXdkClearVs, (unsigned long long)kXdkClearPs,
                vs == kXdkClearVs && ps == kXdkClearPs ? "matches" : "absent or different (trace replay?)");
    return true;
  }();
  (void)s_checked;
  if (c.vsUcodeHash != kXdkClearVs || c.psUcodeHash != kXdkClearPs) return;
  // Screen-space positions only: no viewport scale / offset, no window offset added to them.
  if ((c.vteCntl & 0x3Fu) != 0u) return;
  if (((c.suScModeCntl >> 16) & 1u) && c.windowOffset != 0u) return;
  const uint32_t d0 = c.fetchRaw[0], d1 = c.fetchRaw[1];
  const uint32_t sizeDw = (d1 >> 2) & 0xFFFFFFu, endian = d1 & 3u;
  if ((d0 & 3u) != 3u || sizeDw < 21u) return;
  size_t avail = 0;
  int win = 0;
  const uint8_t* vb = pm4GuestWritableWin(d0 & 0x1FFFFFFCu, avail, win);
  if (!vb || avail < 84u) return;
  float x[3], y[3];
  uint32_t vw[3][7];  // B1c: each vertex's 7 dwords (x, y, z, then the colour the clear PS writes)
  for (int i = 0; i < 3; i++) {
    for (int k = 0; k < 7; k++) {
      uint32_t v;
      std::memcpy(&v, vb + i * 28 + k * 4, 4);
      if (endian == 1u) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      else if (endian == 2u) v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
      else if (endian == 3u) v = (v >> 16) | (v << 16);
      vw[i][k] = v;
      if (k >= 2) continue;
      float f;
      std::memcpy(&f, &v, 4);
      if (!(f > -16384.0f && f < 16384.0f)) return;  // NaN / out of range
      (k ? y : x)[i] = f;
    }
  }
  // A rect list's three vertices are three corners: the rectangle is their bounding box. The pixels
  // covered are those whose centre (p + 0.5 with pix_center = 1, p with 0) lies in [lo, hi).
  const float ctr = (c.vtxCntl & 1u) ? 0.5f : 0.0f;
  const float xl = std::min({x[0], x[1], x[2]}), xh = std::max({x[0], x[1], x[2]});
  const float yl = std::min({y[0], y[1], y[2]}), yh = std::max({y[0], y[1], y[2]});
  const int32_t px0 = int32_t(std::ceil(xl - ctr)), px1 = int32_t(std::ceil(xh - ctr));
  const int32_t py0 = int32_t(std::ceil(yl - ctr)), py1 = int32_t(std::ceil(yh - ctr));
  if (px1 <= px0 || py1 <= py0) return;
  // B1c: the rect is exact whatever is written (a rect list in screen space rasterizes exactly this).
  t.rectExact = 1;
  t.fullRect[0] = px0;
  t.fullRect[1] = py0;
  t.fullRect[2] = px1;
  t.fullRect[3] = py1;
  uint8_t full = 0;
  const bool noAlphaTest = (c.colorControl & 0x18u) == 0u;  // alpha test, alpha to mask
  const uint32_t blend[4] = {c.blendControl, c.blendControl123[0], c.blendControl123[1], c.blendControl123[2]};
  for (uint32_t i = 0; i < 4; i++)
    if (t.colorWriteMask[i] && noAlphaTest && blend[i] == 0x00010001u && ((c.colorMask >> (4u * i)) & 0xFu) == 0xFu)
      full |= uint8_t(1u << i);
  if (t.depthBound && !atApplies) {
    const uint32_t dc = c.depthControl;
    const bool zAll = ((dc >> 1) & 1u) && ((dc >> 2) & 1u) && ((dc >> 4) & 7u) == 7u;
    const bool sFront = (dc & 1u) && ((dc >> 8) & 7u) == 7u && ((dc >> 14) & 7u) == 2u &&
                        ((c.stencilRefMask >> 16) & 0xFFu) == 0xFFu;
    const bool sBack = !((dc >> 7) & 1u) || (((dc >> 20) & 7u) == 7u && ((dc >> 26) & 7u) == 2u &&
                                             ((c.stencilRefMaskBF >> 16) & 0xFFu) == 0xFFu);
    if (zAll && sFront && sBack) full |= 0x10u;
  }
  // B1c: the uniform values. Colour: the vertex colour (dwords 3..6) when all three vertices carry the same bits
  // (no NaN). Depth: z = 0 on all three, no polygon offset (host depth 0 for both depth formats), and the stencil
  // the clear REPLACEs (B1a's front-face check above; a back face with another reference is not uniform).
  {
    bool sameC = true;
    for (int k = 3; k < 7; k++) {
      sameC = sameC && vw[0][k] == vw[1][k] && vw[0][k] == vw[2][k] && (vw[0][k] & 0x7FFFFFFFu) <= 0x7F800000u;
    }
    if (sameC) {
      for (int k = 0; k < 4; k++) std::memcpy(&t.clearColor[k], &vw[0][3 + k], 4);
      t.clearValid |= 1u;
    }
    const uint32_t st = c.stencilRefMask & 0xFFu;
    const bool zeroZ = (vw[0][2] | vw[1][2] | vw[2][2]) == 0u;
    const bool noOffset = ((c.suScModeCntl >> 11) & 7u) == 0u;
    const bool bfSame = !((c.depthControl >> 7) & 1u) || (c.stencilRefMaskBF & 0xFFu) == st;
    if ((full & 0x10u) && zeroZ && noOffset && bfSame) {
      t.clearStencil = uint8_t(st);
      t.clearValid |= 0x10u;
    }
  }
  if (!full) return;
  t.fullWrite = full;
}
}  // namespace

bool D3D12Backend::pm4PrepareDraw(const GuestDrawCommand& c, bool pointList, bool vsf,
                                  bool stripCut, edram::Regs& regs, edram::DrawTargets& t,
                                  ID3D12PipelineState*& pso) {
  regs = pm4RegsFromDraw(c);
  m_pm4ZpdRefPso = m_pm4ZpdRasterPso = nullptr;
  if (m_zpdDrainQuery) {
    // p2.zpd: the query draw's first vertex as the guest wrote it, now (the command-buffer space BeginVertices filled
    // is reused later), for the [NATIVE_ZPD] draw line: the in-shader-fetch path decodes nothing on the CPU.
    m_pm4ZpdVtxAddr = c.drawParams[4];
    size_t avail = 0;
    int win = 0;
    const uint8_t* p = pm4GuestWritableWin(c.drawParams[4] & 0x1FFFFFFCu, avail, win);
    for (uint32_t i = 0; i < 4; i++)
      m_pm4ZpdVtx[i] = p && avail >= 16u ? uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 |
                                               uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3]
                                         : 0u;
  }
  const CachedShader* ps = getShaderCache().findPs(c.psUcodeHash);
  const uint32_t psOut = ps ? ps->colorOutMask() : 0u;
  // kPointList / kLineList / kLineStrip are not polygonal (cull does not apply).
  const bool polygonal = !(c.primType == 1u || c.primType == 2u || c.primType == 3u);
  t = edram::computeDrawTargets(regs, psOut, polygonal);
  // step 4 lane C (gfx/alpha_test.hpp): does the Xenos alpha test act on this draw? Round 4 (Astra on 0005): only in
  // kColorDepth (Xenia binds no PS in kDepthOnly) and only for a PS writing colour 0.
  const bool atApplies = alphatest::enabled() && alphatest::applies(c.colorControl, c.modeControl, psOut);
  if (pm4NbClearElideOn()) nbClearCoverage(c, t, atApplies);  // iter-467 native-b B1
  const bool anyColor = t.colorWriteMask[0] | t.colorWriteMask[1] | t.colorWriteMask[2] |
                        t.colorWriteMask[3];
  if (!t.rasterize || (!anyColor && !t.depthBound)) {
    g_pm4Stats.skipped++;
    // p2.zpd: a draw that writes nothing still counts its samples on the console; skipped here, the query it is in
    // gets no segment for it and answers "visible" (gfx/native_zpd segmentMissed: missed= in the census).
    if (m_zpdDrainQuery && t.rasterize) zpd::segmentMissed(m_zpdDrainQuery);
    static const bool s_skipLog = [] {
      const char* e = std::getenv("SR_PM4_SKIPLOG");
      return e && e[0] == '1';
    }();
    static std::atomic<uint32_t> s_n{0};
    if (s_skipLog && s_n.fetch_add(1) < 64)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_SKIP] vs=%08X ps=%08X prim=%u rasterize=%d color=%X depth=%d mode=%u "
                  "colorMask=%08X depthCtl=%08X psOut=%X colorInfo=%08X surf=%08X",
                  uint32_t(c.vsUcodeHash >> 32), uint32_t(c.psUcodeHash >> 32), c.primType,
                  t.rasterize ? 1 : 0,
                  (t.colorWriteMask[0] ? 1u : 0u) | (t.colorWriteMask[1] ? 2u : 0u) |
                      (t.colorWriteMask[2] ? 4u : 0u) | (t.colorWriteMask[3] ? 8u : 0u),
                  t.depthBound ? 1 : 0, c.modeControl & 7u, c.colorMask, c.depthControl, psOut,
                  c.colorInfo, c.surfaceInfo);
    return false;
  }
  Pm4PsoDesc d;
  d.vsHash = c.vsUcodeHash;
  d.psHash = c.psUcodeHash;
  d.pointSprite = pointList;
  d.vsf = vsf;
  d.stripCut = stripCut;
  // step 4 lane C (gfx/alpha_test.hpp, SR_XLAT_ALPHA_TEST): the Xenos alpha test, drawn with the PS's alpha-test
  // variant. The drain passes RB_ALPHA_REF and the function in c17.x / c17.y. Round 4 (Astra P2 2, the orchestrator's
  // order): (a) the PS's own .ps.at.dxil, (b) the variant the runtime translator made at the PS's IM_LOAD
  // (ShaderCache::requestAlphaTestVariant), (c) neither: the draw is DROPPED - never drawn without a test that may
  // reject every pixel - with an Error line and a [SHADER_DROP] census line (a whole-game run must count 0).
  if (ps && atApplies) {
    auto& sc = getShaderCache();
    if (alphatest::use(true, sc.hasAlphaTestVariant(c.psUcodeHash, *ps)) == alphatest::Use::kVariant) {
      d.alphaTest = true;
      static std::atomic<uint32_t> s_tested{0};
      if (s_tested.fetch_add(1, std::memory_order_relaxed) < 4) {
        float ref;
        std::memcpy(&ref, &c.alphaRef, 4);
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[XLAT_ALPHA] alpha-tested draw ps=%016llX func=%u ref=%.4f (RB_COLORCONTROL=%08X): the variant%s",
                    (unsigned long long)c.psUcodeHash, c.colorControl & 7u, ref, c.colorControl,
                    ps->dxilAlphaTest.empty() ? " (made at runtime)" : "");
      }
    } else {
      const int st = sc.noteAlphaTestDrop(c.psUcodeHash);
      const uint64_t n = sc.alphaTestDrops();
      if (n <= 8 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[XLAT_ALPHA] DROP #%llu ps=%016llX: the alpha test applies (RB_COLORCONTROL=%08X) and the PS has "
                    "no alpha-test variant - %s; the draw is DROPPED",
                    (unsigned long long)n, (unsigned long long)c.psUcodeHash, c.colorControl, dropStateName(st));
      g_pm4Stats.skipped++;
      if (m_zpdDrainQuery && t.rasterize) zpd::segmentMissed(m_zpdDrainQuery);
      return false;
    }
  }
  // Xenia pipeline_cache.cc:1435: depth clip follows PA_CL_CLIP_CNTL.clip_disable.
  // The game's EDRAM clears draw screen-space quads at z = -0.0 with clipping
  // disabled; a clipping host drops them and the surface keeps stale bits.
  d.depthClip = ((c.clipCntl >> 16) & 1u) == 0u;
  // Culling, winding and polygon offset (Xenia pipeline_cache.cc:1383-1425,
  // draw_util.cc:86 GetPreferredFacePolygonOffset). Points, lines and rect
  // lists are "front" only: no culling, the para offset.
  {
    const uint32_t p = c.primType;
    const bool polygonal = p == 4u || p == 5u || p == 6u || p == 7u || p == 13u || p == 14u || p == 15u;
    const uint32_t sc = c.suScModeCntl;
    const bool cullFront = polygonal && (sc & 1u), cullBack = polygonal && ((sc >> 1) & 1u);
    d.cull = cullFront ? 1u : cullBack ? 2u : 0u;
    d.frontCcw = ((sc >> 2) & 1u) == 0u;
    auto f32 = [&](uint32_t i) { float v; std::memcpy(&v, &c.polyOffset[i], 4); return v; };
    auto roundMagnitude = [](float v) {
      if (v == 0.0f) return v;
      const double order = std::pow(10.0, std::floor(std::log10(std::fabs(double(v)))));
      return float(std::round(double(v) / order) * order);
    };
    float scale = 0.0f, offset = 0.0f;
    if (polygonal) {
      if (((sc >> 11) & 1u) && !cullFront) {
        scale = roundMagnitude(f32(0));
        offset = f32(1);
      }
      if (((sc >> 12) & 1u) && !cullBack && scale == 0.0f && offset == 0.0f) {
        scale = f32(2);
        offset = f32(3);
      }
    } else if ((sc >> 13) & 1u) {
      scale = f32(0);
      offset = f32(1);
    }
    if (t.depthBound && (scale != 0.0f || offset != 0.0f)) {
      // GetD3D10IntegerPolygonOffset: D24FS8 in float24 ulps (x8), else unorm24.
      const bool f24 = t.depth.format() == 1u;
      int32_t bias = int32_t(std::ceil(std::fabs(offset) * (f24 ? float(1u << 24) * (1.0f / 8.0f)
                                                               : float((1u << 24) - 1u))));
      if (f24) bias <<= 3;
      d.depthBias = offset < 0.0f ? -bias : bias;
      d.depthBiasSlope = scale * (1.0f / 16.0f);  // kPolygonOffsetScaleSubpixelUnit
      // iter-464 resscale: the slope is per HOST pixel - Xenia pipeline_cache.cc:3036
      // multiplies by max(scale_x, scale_y) (worst case: more bias, less Z fighting).
      d.depthBiasSlope *= float(resscale::get().maxAxis());
    }
  }
  const uint32_t blend[4] = {c.blendControl, c.blendControl123[0], c.blendControl123[1],
                             c.blendControl123[2]};
  for (uint32_t i = 0; i < 4; i++) {
    if (!t.colorWriteMask[i]) continue;
    d.numRt = i + 1;
    d.rtv[i] = pm4ColorDxgi(t.color[i].hostFormat());
    d.writeMask[i] = t.colorWriteMask[i];
    d.blend[i] = blend[i];
  }
  if (t.depthBound) {
    d.dsv = pm4DepthDsv(t.depth.format());
    d.zEnable = t.zEnable;
    d.zWrite = t.zWrite;
    d.zFunc = t.zFunc;
    d.stencil = t.stencilEnable;
    d.stencilCtl = c.depthControl;
    d.stencilMasks = (c.stencilRefMask >> 8) & 0xFFFFu;
  }
  pso = getPm4Pso(d);
  // p2.zpd probe: the query draw's probe pipelines (DrawBatch::zpdRefPso / zpdRasterPso).
  if (pso && m_zpdDrainQuery && zpd::probeOn()) {
    m_pm4ZpdRefPso = getPm4Pso(zpdProbeDesc(d, false));
    m_pm4ZpdRasterPso = getPm4Pso(zpdProbeDesc(d, true));
  }
  // B1b (SR_NB_COVER_FILL): the coverage-marking variant for a draw whose colour bindings it overwrites wherever it
  // rasterizes - no depth / stencil binding, no blending (ONE / ZERO), every channel of the host format written.
  m_pm4MarkPso = nullptr;
  m_pm4MarkWanted = false;  // step 6 R6 fix round 4 (Astra r4 P2): "no mark needed" vs "the mark's acquisition failed"
  if (pso && nbCoverFillOn() && !t.depthBound && d.numRt) {
    bool ok = true;
    for (uint32_t i = 0; i < d.numRt && ok; i++) {
      if (!t.colorWriteMask[i]) continue;
      const edram::HostFmt hf = t.color[i].hostFormat();
      const uint32_t chans = (hf == edram::HostFmt::RG16F || hf == edram::HostFmt::RG32F) ? 0x3u
                             : hf == edram::HostFmt::R32F ? 0x1u : 0xFu;
      ok = d.blend[i] == 0x00010001u && (t.colorWriteMask[i] & chans) == chans;
    }
    if (ok) {
      m_pm4MarkWanted = true;
      Pm4PsoDesc m = d;
      m.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT;
      m.zEnable = m.zWrite = false;
      m.zFunc = 7;
      m.stencil = true;
      m.stencilCtl = (7u << 8) | (2u << 14);  // front = back: ALWAYS, pass REPLACE, fail / z-fail KEEP
      m.stencilMasks = 0xFFFFu;               // read 0xFF, write 0xFF
      m_pm4MarkPso = getPm4Pso(m);
    }
  }
  return true;
}

bool D3D12Backend::pm4ViewportScissor(const edram::Regs& regs, const edram::DrawTargets& t,
                                      uint64_t psHash, D3D12_VIEWPORT& vp, float ndc[4],
                                      D3D12_RECT& scissor) {
  const CachedShader* ps = getShaderCache().findPs(psHash);
  const bool psDepth = ps && ps->writesDepth();
  const bool float24 = t.depthBound && t.zEnable && t.depth.format() == 1u;
  // iter-464 resscale: the host viewport and scissor are in scaled host pixels
  // (draw_util.cc GetHostViewportInfo + d3d12_command_processor.cc:2713-2724).
  const resscale::Scale& rs = resscale::get();
  const edram::HostViewport hv = edram::computeViewport(regs, psDepth, float24, rs.x, rs.y);
  if (hv.empty) return false;
  vp.TopLeftX = hv.x;
  vp.TopLeftY = hv.y;
  vp.Width = hv.w;
  vp.Height = hv.h;
  vp.MinDepth = hv.minZ;
  vp.MaxDepth = hv.maxZ;
  ndc[0] = hv.ndcScale[0];
  ndc[1] = hv.ndcScale[1];
  ndc[2] = hv.ndcOffset[0];
  ndc[3] = hv.ndcOffset[1];
  const edram::Scissor s = edram::computeScissor(regs, true);
  scissor = D3D12_RECT{LONG(s.x * int32_t(rs.x)), LONG(s.y * int32_t(rs.y)),
                       LONG((s.x + s.w) * int32_t(rs.x)), LONG((s.y + s.h) * int32_t(rs.y))};
  return true;
}

bool D3D12Backend::pm4Init() {
  if (m_pm4Ready) return true;
  sr::kernel::MemSiteScope memSite("pm4Init");   // step 4 7d: [MEM_SITE] names (SR_MEM_SITES, diagnostic)
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kPm4RtvSlots;
  if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_pm4RtvHeap)))) return false;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = kPm4DsvSlots;
  if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_pm4DsvHeap)))) return false;
  // Slot 0: a null RTV for unbound MRT holes (write mask 0 there anyway).
  D3D12_RENDER_TARGET_VIEW_DESC nd{};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  m_pm4NullRtv = m_pm4RtvHeap->GetCPUDescriptorHandleForHeapStart();
  m_device->CreateRenderTargetView(nullptr, &nd, m_pm4NullRtv);
  m_pm4RtvUsed = 1;
  m_pm4DsvUsed = 0;

  D3D12_HEAP_PROPERTIES hp{};
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = kPm4StagingBytes;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  // Step 4 item 7d(c) (lane D; SR_PM4_READBACK_LAZY, default ON - =0 creates it here as before): in the release build
  // the 128 MB readback's only user is the RT0 frame dump (pm4DumpCfg, a diagnostic), so it is created at that dump's
  // first use (pm4EnsureReadback); the development build's EDRAM model and nbPropVerify use it, so it stays eager there.
  if (!pm4ReadbackLazy() && !pm4EnsureReadback()) return false;
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&m_pm4Upload))))
    return false;
  D3D12_RANGE none{0, 0};
  if (FAILED(m_pm4Upload->Map(0, &none, reinterpret_cast<void**>(&m_pm4UploadPtr)))) return false;
  m_pm4UploadUsed = 0;
  nbSurfTable().reset();  // EDRAM zero step 2: the surface table (C1 stage 3: the alias table the shipping path decides with)
#if SR_ORACLE
  edramModelInit();  // the oracle half: the CPU mirror and the tile map (gfx/d3d12_edram_model.cpp)
#endif
  m_pm4Ready = true;
  // iter-467 NATIVE METER: the PM4 renderer exists, so its EDRAM-model counts are real numbers from here on
  // (pm4MeterPublish feeds them once per rendered frame).
  sr::native::meter::wire(sr::native::meter::kEdramResolve);
  sr::native::meter::wire(sr::native::meter::kEdramTransfer);
  sr::native::meter::wire(sr::native::meter::kEdramLoad);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_RT] ready: staging %llu MB x2, RTV %u DSV %u%s",
              (unsigned long long)(kPm4StagingBytes >> 20), kPm4RtvSlots, kPm4DsvSlots,
              m_pm4Readback ? "" : " (the readback created at its first use, SR_PM4_READBACK_LAZY)");
  return true;
}

// step 4 7d(c): the release build defers the 128 MB staging readback to its first user (the RT0 frame dump)
bool D3D12Backend::pm4ReadbackLazy() {
#if SR_ORACLE
  return false;   // the EDRAM model (d3d12_edram_model.cpp) and nbPropVerify read through it from the first frame
#else
  static const bool lazy = [] {
    const char* e = std::getenv("SR_PM4_READBACK_LAZY");
    return !e || e[0] != '0';
  }();
  return lazy;
#endif
}

bool D3D12Backend::pm4EnsureReadback() {
  if (m_pm4Readback) return true;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = kPm4StagingBytes;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&m_pm4Readback)))) {
    m_pm4Readback.Reset();
    return false;
  }
  return true;
}

D3D12Backend::Pm4HostRt* D3D12Backend::pm4Rt(edram::RtKey k) {
  auto it = m_pm4Rts.find(k.v);
  if (it != m_pm4Rts.end()) return &it->second;
  if (!pm4Init()) return nullptr;
  const edram::HostFmt hf = k.hostFormat();
  const bool depth = k.isDepth();
  const DXGI_FORMAT fmt = depth ? pm4DepthDsv(k.format()) : pm4ColorDxgi(hf);
  if (fmt == DXGI_FORMAT_UNKNOWN || !k.width() || !k.height()) {
    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1) < 16)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PM4_RT] unsupported key 0x%08X fmt=%u depth=%d", k.v,
                  k.format(), depth ? 1 : 0);
    return nullptr;
  }
  if ((depth && m_pm4DsvUsed >= kPm4DsvSlots) || (!depth && m_pm4RtvUsed >= kPm4RtvSlots)) {
    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1) < 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PM4_RT] descriptor heap full (rtv=%u dsv=%u)",
                  m_pm4RtvUsed, m_pm4DsvUsed);
    return nullptr;
  }
  Pm4HostRt rt;
  // iter-464 resscale: host RTs are scale x the guest surface (Xenia
  // d3d12_render_target_cache.cc:1844-1847); rt.w / rt.h are HOST pixels.
  rt.w = k.width() * resscale::get().x;
  rt.h = k.height() * resscale::get().y;
  rt.fmt = fmt;
  rt.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  NativeImageDesc imageDesc;
  imageDesc.width = rt.w;
  imageDesc.height = rt.h;
  imageDesc.resourceFormat = depth ? pm4DepthTypeless(k.format()) : fmt;
  imageDesc.kind = depth ? NativeImageKind::DepthStencil : NativeImageKind::Color;
  imageDesc.initialState = rt.state;
  imageDesc.clear.Format = fmt;
  imageDesc.depthClearFlags = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
  // The development adapter maps dimensions/formats only. Existing targets
  // remain single-sample; console MSAA/tile policy never enters the image store.
  const HRESULT createHr = sr::hitch::timed(sr::hitch::kResCreate, [&] {
    return m_nativeImages.create(imageDesc, rt.image);
  });
  if (FAILED(createHr)) {
    const NativeImageStats images = m_nativeImages.stats();
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PM4_RT] create FAILED key 0x%08X %ux%u hr=%08X device=%08X "
                "images=%zu pending=%zu rtv=%u dsv=%u",
                k.v, rt.w, rt.h, unsigned(createHr), unsigned(m_device->GetDeviceRemovedReason()),
                images.live, images.pending, images.colorDescriptors, images.depthDescriptors);
    // Failure-only size query for the descriptor NativeImageStore::create uses.
    // The store can also refuse before CreateCommittedResource (for example,
    // descriptor/CPU allocation), so these are requested bytes, not a claim
    // that this particular API was the origin of createHr.
    D3D12_RESOURCE_DESC requested{};
    requested.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    requested.Width = imageDesc.width;
    requested.Height = imageDesc.height;
    requested.DepthOrArraySize = 1;
    requested.MipLevels = 1;
    requested.Format = imageDesc.resourceFormat;
    requested.SampleDesc = {imageDesc.samples, imageDesc.sampleQuality};
    requested.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto allocation = m_device->GetResourceAllocationInfo(0, 1, &requested);
    const bool allocationKnown = allocation.SizeInBytes != UINT64_MAX && allocation.SizeInBytes != 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PM4_RT] failed request resource_fmt=%u view_fmt=%u width=%llu height=%u samples=%u "
                "quality=%u flags=%u allocation_known=%u requested_bytes=%llu alignment=%llu",
                unsigned(requested.Format), unsigned(imageDesc.clear.Format),
                static_cast<unsigned long long>(requested.Width), requested.Height,
                requested.SampleDesc.Count, requested.SampleDesc.Quality, unsigned(requested.Flags),
                unsigned(allocationKnown), static_cast<unsigned long long>(allocationKnown ? allocation.SizeInBytes : 0),
                static_cast<unsigned long long>(allocationKnown ? allocation.Alignment : 0));
    reportNativeGpuFailure(m_device.Get(), m_factory.Get(), "native-image-create", createHr);
    return nullptr;
  }
  rt.res = rt.image.resource();
  rt.view = rt.image.view();
  {
    wchar_t nm[48];
    swprintf(nm, 48, L"PM4_RT %08X %ux%u", k.v, rt.w, rt.h);
    rt.res->SetName(nm);
  }
  keepNativeImage(rt.image);
  if (FAILED(m_nativeImages.recordClear(m_commandList.Get(), rt.image))) {
    m_nativeImages.release(rt.image.handle());
    return nullptr;
  }
  if (depth) {
    ++m_pm4DsvUsed;  // preserve the development adapter's total view budget
    if (m_pm4GpuReady) pm4DepthSrvs(rt, k.format());  // iter-466 res4k: direct depth transfers
  } else {
    ++m_pm4RtvUsed;
    if (m_pm4GpuReady && m_pm4RtSrvUsed < pm4RtSrvCapacity()) {
      D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = fmt;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.Texture2D.MipLevels = 1;
      rt.srv = int32_t(m_pm4RtSrvUsed++);
      publishXlatSrv(rt.res, &sd, pm4RtSrvCpu(uint32_t(rt.srv)));
    }
  }
  g_pm4Stats.newRts++;
  static std::atomic<uint32_t> s_log{0};
  if (s_log.fetch_add(1) < 64)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_RT] new key=0x%08X base=0x%03X pitchTiles=%u msaa=%u %s fmt=%u -> %ux%u dxgi=%u",
                k.v, k.base(), k.pitchTiles32(), k.msaa(), depth ? "DEPTH" : "color", k.format(), rt.w,
                rt.h, fmt);
  return &m_pm4Rts.emplace(k.v, std::move(rt)).first->second;
}

void D3D12Backend::pm4Transition(Pm4HostRt& rt, D3D12_RESOURCE_STATES s) {
  keepNativeImage(rt.image);  // same-state use still needs ownership in this recording
  if (rt.state == s) return;
  pm4Barrier(m_commandList.Get(), rt.res, rt.state, s);
  rt.state = s;
}

// --- iter-464 resscale: SR_PM4_GPU_TIME=1 - GPU time per PM4 frame ---
// (diagnostic, default OFF; docs/RESOLUTION.md "Measuring"). Timestamp queries
// around every command list of the frame; read back one frame later per slot
// (the slot's fence has passed when its next frame starts).
namespace {
bool gpuTimeEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_GPU_TIME");
#if SR_ORACLE
    const char* c = std::getenv("SR_NB_CENSUS");  // iter-467 native-b: the census needs the stamps
    return (e && e[0] == '1') || (c && c[0] && c[0] != '0');
#else
    return e && e[0] == '1';  // C1 stage 3: the census is the development build's (gfx/native_targets.inc)
#endif
  }();
  return v;
}
}  // namespace

bool D3D12Backend::gtInit() {
  if (m_gtReady) return true;
  if (m_gtFailed || !m_device || !m_commandQueue) return false;
  m_gtFailed = true;
  D3D12_QUERY_HEAP_DESC qd{};
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qd.Count = kGtPerSlot * kFrameCount;
  if (FAILED(m_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_gtHeap)))) return false;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = uint64_t(qd.Count) * 8u;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&m_gtReadback))))
    return false;
  if (FAILED(m_commandQueue->GetTimestampFrequency(&m_gtFreq)) || !m_gtFreq) return false;
  m_gtFailed = false;
  m_gtReady = true;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_GPU_TIME] on: timestamp queries, %llu Hz",
              (unsigned long long)m_gtFreq);
  return true;
}

void D3D12Backend::gtBegin() {
  // One stamp is kept free for the closing gtEnd.
  if (!m_gtReady || m_gtOpen || m_gtN + 2u > kGtPerSlot) return;
  m_gtTag[m_frameIndex][m_gtN] = GtListStart;
#if SR_ORACLE
  m_gtItem[m_frameIndex][m_gtN] = 0;
#endif
  m_commandList->EndQuery(m_gtHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          uint32_t(m_frameIndex) * kGtPerSlot + m_gtN++);
  m_gtOpen = true;
}

void D3D12Backend::gtEnd() {
  if (!m_gtReady || !m_gtOpen) return;
  m_gtTag[m_frameIndex][m_gtN] = GtDraw;
#if SR_ORACLE
  m_gtItem[m_frameIndex][m_gtN] = 0;
#endif
  m_commandList->EndQuery(m_gtHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          uint32_t(m_frameIndex) * kGtPerSlot + m_gtN++);
  m_gtOpen = false;
}

void D3D12Backend::gtMark(GtTag endedWork) {
  if (!m_gtReady || !m_gtOpen || m_gtN + 2u > kGtPerSlot) return;
  m_gtTag[m_frameIndex][m_gtN] = endedWork;
#if SR_ORACLE
  m_gtItem[m_frameIndex][m_gtN] = uint16_t(m_nbCur);  // iter-467 native-b census
#endif
  m_commandList->EndQuery(m_gtHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          uint32_t(m_frameIndex) * kGtPerSlot + m_gtN++);
}

void D3D12Backend::gtFrameStart() {
  if (!gpuTimeEnabled() || !gtInit()) return;
  const uint32_t slot = uint32_t(m_frameIndex);
  const uint32_t n = m_gtSlotN[slot];
  static double s_busy = 0, s_span = 0, s_max = 0, s_lists = 0, s_wall = 0;
  static double s_pass[GtTagCount] = {}, s_passN[GtTagCount] = {};
  static uint32_t s_cnt = 0;
  static std::chrono::steady_clock::time_point s_last{};
  const auto now = std::chrono::steady_clock::now();
  const double wall = s_last.time_since_epoch().count()
                          ? std::chrono::duration<double, std::milli>(now - s_last).count()
                          : 0.0;
  s_last = now;
  if (n >= 2) {
    const size_t off = size_t(slot) * kGtPerSlot * 8u;
    D3D12_RANGE rr{off, off + size_t(n) * 8u};
    void* p = nullptr;
    if (SUCCEEDED(m_gtReadback->Map(0, &rr, &p)) && p) {
      const uint64_t* t = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(p) + off);
      const uint8_t* tag = m_gtTag[slot];
      // iter-466 res4k: each stamp closes the segment since the previous one; a
      // ListStart stamp closes the idle gap between two lists (not counted).
      uint64_t busy = 0, pass[GtTagCount] = {};
      uint32_t lists = 0, passN[GtTagCount] = {};
      for (uint32_t i = 0; i < n; i++) {
        if (tag[i] == GtListStart) {
          lists++;
          continue;
        }
        if (i && t[i] > t[i - 1] && tag[i] < GtTagCount) {
          busy += t[i] - t[i - 1];
          pass[tag[i]] += t[i] - t[i - 1];
        }
        if (tag[i] < GtTagCount) passN[tag[i]]++;
      }
      const uint64_t span = t[n - 1] > t[0] ? t[n - 1] - t[0] : 0;
      nbAttribute(slot, t, n);  // iter-467 native-b census (no-op without records)
      const D3D12_RANGE nw{0, 0};
      m_gtReadback->Unmap(0, &nw);
      const double toMs = 1000.0 / double(m_gtFreq);
      const double busyMs = double(busy) * toMs;
      s_busy += busyMs;
      s_span += double(span) * toMs;
      s_max = std::max(s_max, busyMs);
      s_lists += double(lists);
      s_wall += wall;
      for (uint32_t k = 0; k < GtTagCount; k++) {
        s_pass[k] += double(pass[k]) * toMs;
        s_passN[k] += double(passN[k]);
      }
      if (++s_cnt == 120) {
        const resscale::Scale& rs = resscale::get();
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PM4_GPU_TIME] avg over 120 frames: gpu busy %.2f ms (max %.2f), span %.2f ms, "
                    "%.1f lists/frame, present interval %.2f ms (%.1f fps); internal %ux%u (scale %ux%u) "
                    "-> backbuffer %ux%u",
                    s_busy / 120, s_max, s_span / 120, s_lists / 120, s_wall / 120,
                    s_wall > 0 ? 120000.0 / s_wall : 0.0, 1280u * rs.x, 720u * rs.y, rs.x, rs.y,
                    m_bbW, m_bbH);
        // Per pass (ms per frame, and how many passes per frame): the game's draws, the
        // EDRAM store / load transfers, resolves, texture loads, the two present passes.
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PM4_GPU_PASS] avg ms/frame: draw %.2f | edram store %.2f (%.1f) | edram load %.2f "
                    "(%.1f) | resolve %.2f (%.1f; its edram store %.2f) | tex load %.2f (%.1f; scaled "
                    "%.2f (%.1f); copy %.2f) | scan-out %.2f | fit %.2f; scale %ux%u | materialize %.2f (%.1f)",
                    s_pass[GtDraw] / 120, s_pass[GtStore] / 120, s_passN[GtStore] / 120,
                    s_pass[GtLoad] / 120, s_passN[GtLoad] / 120,
                    (s_pass[GtResolve] + s_pass[GtResStore]) / 120, s_passN[GtResolve] / 120,
                    s_pass[GtResStore] / 120,
                    (s_pass[GtTexLoad] + s_pass[GtTexLoadScaled] + s_pass[GtTexCopy]) / 120,
                    (s_passN[GtTexLoad] + s_passN[GtTexLoadScaled]) / 120,
                    s_pass[GtTexLoadScaled] / 120, s_passN[GtTexLoadScaled] / 120,
                    s_pass[GtTexCopy] / 120, s_pass[GtScanOut] / 120, s_pass[GtFit] / 120, rs.x, rs.y,
                    s_pass[GtNbMat] / 120, s_passN[GtNbMat] / 120);
        s_busy = s_span = s_max = s_lists = s_wall = 0;
        for (uint32_t k = 0; k < GtTagCount; k++) s_pass[k] = s_passN[k] = 0;
        s_cnt = 0;
      }
    }
  }
  m_gtSlotN[slot] = 0;
  m_gtN = 0;
  m_gtOpen = false;
  nbFrameStart(slot);  // iter-467 native-b census: print the slot's records, decide this frame
  gtBegin();
}

void D3D12Backend::gtFrameEnd() {
  if (!m_gtReady) return;
  gtEnd();
  const uint32_t slot = uint32_t(m_frameIndex);
  if (m_gtN >= 2)
    m_commandList->ResolveQueryData(m_gtHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kGtPerSlot,
                                    m_gtN, m_gtReadback.Get(), uint64_t(slot) * kGtPerSlot * 8u);
  m_gtSlotN[slot] = m_gtN >= 2 ? m_gtN : 0;
#if SR_ORACLE
  nbReplaySeal(slot);
#endif
  m_gtN = 0;
}

namespace {
struct HdrProbeStage { const char* name; uint64_t shader; uint32_t inputs; };
constexpr HdrProbeStage kHdrProbeStages[] = {
    {"adapt", 0x040D6633B0AA5D6Dull, 2},
    {"tonemap", 0x2679C172F83DE85Dull, 3},
    {"extract", 0x287160844A17DEA4ull, 2},
    {"blur", 0xB8B3F881054E85FBull, 1},
    {"reduce", 0x1276813A772FDBFCull, 1},
    {"blurfinal", 0x7407441295D9968Full, 1},
};
}  // namespace

// Copy the identity-selected exposure/bloom chain at its actual draws.
// Completion is polled at existing lifecycle points; no added flush/signal/wait.
// Bounded development evidence only: GPU copies and disk I/O still cost time.
void D3D12Backend::hdrFreezeProbe(const DrawBatch& b, Pm4HostRt& output, size_t batch,
                                  const D3D12_RECT& scissor, const TranslatedConstantSnapshot& constants) {
  static const bool enabled = [] {
    const char* e = std::getenv("SR_HDR_ASYNC_PROBE");
    return e && e[0] == '1';
  }();
  if (!enabled) return;
  uint32_t stage = 0;
  while (stage < 6 && b.psHash != kHdrProbeStages[stage].shader) ++stage;
  if (stage == 6) return;
  const auto freeze = sr::kernel::highfps::freezeClock();
  const double held = sr::kernel::highfps::freezeHeldMs();
  if (!freeze.holdStartTicks || freeze.holdId < 4 || freeze.holdId > 18 ||
      held < 400 || held > 1000 || m_hdrProbeCounts[freeze.holdId][stage] >= 48) return;
  static const std::string dir = [] {
    const char* e = std::getenv("SR_HDR_ASYNC_PROBE_DIR");
    return e ? std::string(e) : std::string();
  }();
  const DWORD attrs = dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesA(dir.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    nativeRecordingFail("hdr-async-directory", E_INVALIDARG);
  if (!constants || !m_nativeRecording || !output.res)
    nativeRecordingFail("hdr-async-buffer", E_INVALIDARG);
  HdrProbeCapture capture;
  capture.constants = constants.storage();  // exact immutable bytes bound by this draw
  capture.stage = stage; capture.freeze = freeze.holdId; capture.frame = m_frameEpoch;
  capture.batch = batch; capture.sequence = m_hdrProbeCounts[freeze.holdId][stage];
  capture.held = held; capture.vsHash = b.vsHash; capture.psHash = b.psHash;
  capture.vsOffset = capture.constants.offset() + constants.offset(0);
  capture.psOffset = capture.constants.offset() + constants.offset(1);
  capture.sharedOffset = capture.constants.offset() + constants.offset(2);
  capture.vsBytes = constants.size(0); capture.psBytes = constants.size(1);  // staged extents
  const uint32_t inputCount = kHdrProbeStages[stage].inputs;
  capture.images.resize(inputCount + 1);
  uint32_t slots[3]{};
  std::memcpy(slots, capture.constants.bytes().data() + constants.offset(2),
              inputCount * sizeof(uint32_t));
  // step 6 I3-B (bindless): the bound shared block is the planner's own bytes, so each staged selection word IS
  // its heap slot - equal to the planning snapshot's, which identifies the selected recipe.
  uint32_t planningSlots[3]{};
  if (!m_xlatCBPtr || b.sharedCbOff > m_xlatConstants.size() ||
      inputCount * sizeof(uint32_t) > m_xlatConstants.size() - b.sharedCbOff)
    nativeRecordingFail("hdr-async-planning-range", E_INVALIDARG);
  std::memcpy(planningSlots, m_xlatCBPtr + b.sharedCbOff, inputCount * sizeof(uint32_t));
  for (uint32_t i = 0; i < inputCount; ++i) {
    if (slots[i] != planningSlots[i] || planningSlots[i] >= m_xlatViews.size())
      nativeRecordingFail("hdr-async-bound-input", E_INVALIDARG);
    const auto& view = m_xlatViews[planningSlots[i]];
    if (!view.resource || view.buffer || view.srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D)
      nativeRecordingFail("hdr-async-selected-view", E_INVALIDARG);
  }
  auto bind = [&](uint32_t i, ID3D12Resource* resource, uint32_t key,
                  uint32_t mapping, uint32_t owner, uint64_t cacheKey, DXGI_FORMAT viewFormat) {
    auto& image = capture.images[i];
    if (image.resource) nativeRecordingFail("hdr-async-ambiguous-slot", E_INVALIDARG);
    const auto& view = m_xlatViews[planningSlots[i]];
    if (view.resource.Get() != resource || view.srv.Format != viewFormat ||
        view.srv.Shader4ComponentMapping != mapping)
      nativeRecordingFail("hdr-async-selected-identity", E_INVALIDARG);
    image.resource = view.resource; image.key = key; image.mapping = mapping; image.owner = owner;
    image.slot = slots[i]; image.cacheKey = cacheKey; image.viewFormat = viewFormat;
  };
  for (const auto& entry : m_texRefresh)
    for (uint32_t i = 0; i < inputCount; ++i)
      if (entry.second.slot == planningSlots[i])
        bind(i, entry.second.tex.Get(), ((b.texFetch[i * 6 + 1] >> 12) & 0xFFFFFu) * 4096u,
             entry.second.componentMapping, 1u, entry.first, entry.second.viewFormat);
#if SR_ORACLE
  for (const auto& texture : m_resolveTextures)
    for (uint32_t i = 0; i < inputCount; ++i)
      if (texture.slot == planningSlots[i])
        bind(i, texture.res.Get(), texture.basePhys, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
             2u, texture.basePhys, texture.res ? texture.res->GetDesc().Format : DXGI_FORMAT_UNKNOWN);
#endif  // SR_ORACLE
  auto& target = capture.images.back();
  target.resource = output.res; target.state = output.state; target.owner = 3;
  target.mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  target.viewFormat = output.res->GetDesc().Format;
  if (scissor.left < 0 || scissor.top < 0 || scissor.right <= scissor.left || scissor.bottom <= scissor.top)
    nativeRecordingFail("hdr-async-output-region", E_INVALIDARG);
  target.x = uint32_t(scissor.left); target.y = uint32_t(scissor.top);
  target.width = uint32_t(scissor.right - scissor.left);
  target.height = uint32_t(scissor.bottom - scissor.top);
  uint64_t gpuOffset = 8704, rawOffset = 8704;
  for (uint32_t i = 0; i <= inputCount; ++i) {
    auto& image = capture.images[i];
    if (!image.resource) nativeRecordingFail("hdr-async-missing-input", E_INVALIDARG);
    const auto desc = image.resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !desc.Width || !desc.Height ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        desc.Width > 16384 || desc.Height > 16384)
      nativeRecordingFail("hdr-async-image-shape", E_INVALIDARG);
    image.resourceFormat = uint32_t(desc.Format);
    // Only the typed views actually encountered by these identity-selected
    // shaders. Compatible typeless families copy without reinterpretation.
    const bool compatible = desc.Format == image.viewFormat ||
        (desc.Format == DXGI_FORMAT_R32_TYPELESS && image.viewFormat == DXGI_FORMAT_R32_FLOAT) ||
        (desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS && image.viewFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
         (image.viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM || image.viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
    if (!compatible || (image.viewFormat != DXGI_FORMAT_R32_FLOAT &&
        image.viewFormat != DXGI_FORMAT_R16G16B16A16_FLOAT &&
        image.viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM && image.viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
      nativeRecordingFail("hdr-async-image-format", E_INVALIDARG);
    if (i < inputCount) { image.width = uint32_t(desc.Width); image.height = desc.Height; }
    if (uint64_t(image.x) + image.width > desc.Width || uint64_t(image.y) + image.height > desc.Height)
      nativeRecordingFail("hdr-async-image-region", E_INVALIDARG);
    if (stage == 0 && (image.width != 1 || image.height != 1 || image.viewFormat != DXGI_FORMAT_R32_FLOAT ||
        (i < inputCount && D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(0, image.mapping) != 0)))
      nativeRecordingFail("hdr-async-adapt-view", E_INVALIDARG);
    if (i < inputCount && image.resource.Get() == target.resource.Get())
      nativeRecordingFail("hdr-async-input-output-alias", E_INVALIDARG);
    auto copyDesc = desc;
    copyDesc.Width = image.width; copyDesc.Height = image.height; copyDesc.MipLevels = 1;
    copyDesc.Format = image.viewFormat;
    UINT rows = 0; UINT64 total = 0;
    m_device->GetCopyableFootprints(&copyDesc, 0, 1, gpuOffset,
                                  &image.footprint, &rows, &image.rowBytes, &total);
    if (image.footprint.Offset != gpuOffset || rows != image.height || !image.rowBytes ||
        image.rowBytes > image.footprint.Footprint.RowPitch)
      nativeRecordingFail("hdr-async-footprint", E_INVALIDARG);
    gpuOffset = (image.footprint.Offset + uint64_t(image.footprint.Footprint.RowPitch) * rows + 511) & ~511ull;
    image.rawOffset = rawOffset; rawOffset += image.rowBytes * rows;
    const HRESULT retained = m_nativeRecording->keep(image.resource.Get());
    if (FAILED(retained)) nativeRecordingFail("hdr-async-retain-image", retained);
  }
  capture.readbackBytes = gpuOffset; capture.rawBytes = rawOffset;
  uint64_t pendingBytes = capture.readbackBytes;
  for (const auto& pending : m_hdrProbePending) pendingBytes += pending.readbackBytes;
  if (capture.readbackBytes > 32ull * 1024 * 1024 || pendingBytes > 128ull * 1024 * 1024 ||
      m_hdrProbePending.size() >= 48)
    nativeRecordingFail("hdr-async-capacity", E_OUTOFMEMORY);
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = gpuOffset;
  buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&capture.readback));
  if (FAILED(hr)) nativeRecordingFail("hdr-async-create-readback", hr);
  hr = m_nativeRecording->keep(capture.readback.Get());
  if (FAILED(hr)) nativeRecordingFail("hdr-async-retain-readback", hr);
  keepNativeImage(output.image);
  for (const auto& image : capture.images) {
    if (image.state != D3D12_RESOURCE_STATE_COPY_SOURCE)
      pm4Barrier(m_commandList.Get(), image.resource.Get(), image.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = capture.readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = image.footprint;
    src.pResource = image.resource.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX region{image.x, image.y, 0, image.x + image.width, image.y + image.height, 1};
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &region);
    if (image.state != D3D12_RESOURCE_STATE_COPY_SOURCE)
      pm4Barrier(m_commandList.Get(), image.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, image.state);
  }
  // Only the staged extents: the bytes past them are not this draw's constants.
  m_commandList->CopyBufferRegion(capture.readback.Get(), 0, capture.constants.resource(), capture.vsOffset,
                                  capture.vsBytes);
  m_commandList->CopyBufferRegion(capture.readback.Get(), 4096, capture.constants.resource(), capture.psOffset,
                                  capture.psBytes);
  m_commandList->CopyBufferRegion(capture.readback.Get(), 8192, capture.constants.resource(), capture.sharedOffset, 512);
  const auto after = sr::kernel::highfps::freezeClock();
  capture.sameHold = (after.holdStartTicks == freeze.holdStartTicks && after.holdId == freeze.holdId) ? 1u : 0u;
  char name[160];
  std::snprintf(name, sizeof(name), "hdr_async_%s_fr%02u_f%06llu_b%05zu_n%03u.bin",
                kHdrProbeStages[stage].name, capture.freeze,
                (unsigned long long)capture.frame, capture.batch, capture.sequence);
  capture.path = dir + "\\" + name;
  m_hdrProbePending.push_back(std::move(capture));
  ++m_hdrProbeCounts[freeze.holdId][stage];
}

void D3D12Backend::hdrProbeSubmitted(uint64_t ticket) {
  size_t captures = 0;
  for (auto& capture : m_hdrProbePending)
    if (!capture.ticket) { capture.ticket = ticket; ++captures; }
  if (captures)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[HDR_ASYNC_SUBMITTED] ticket=%llu captures=%zu",
                (unsigned long long)ticket, captures);  // after actual successful submit+signal
}

void D3D12Backend::hdrProbeDiscardUnsubmitted() {
  for (const auto& capture : m_hdrProbePending)
    if (!capture.ticket)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[HDR_ASYNC_REFUSED] abandoned stage=%u freeze=%u frame=%llu batch=%zu",
                  capture.stage, capture.freeze, (unsigned long long)capture.frame, capture.batch);
  std::erase_if(m_hdrProbePending, [](const HdrProbeCapture& capture) { return !capture.ticket; });
}

void D3D12Backend::hdrProbeCollect() {
  while (!m_hdrProbePending.empty()) {
    const auto& capture = m_hdrProbePending.front();
    if (!capture.ticket) break;
    const HRESULT completed = m_nativeImages.wait(capture.ticket, 0);  // one nonblocking fence poll
    if (completed == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) break;
    if (FAILED(completed)) nativeRecordingFail("hdr-async-completion", completed);
    uint8_t* mapped = nullptr;
    const D3D12_RANGE read{0, SIZE_T(capture.readbackBytes)};
    const HRESULT mappedResult = capture.readback->Map(0, &read, reinterpret_cast<void**>(&mapped));
    if (FAILED(mappedResult) || !mapped)
      nativeRecordingFail("hdr-async-map", FAILED(mappedResult) ? mappedResult : E_POINTER);
    std::vector<uint8_t> bytes(size_t(capture.rawBytes));
    // Float blocks past their staged extents stay zero in the record (never read by the shaders).
    std::memcpy(bytes.data(), mapped, capture.vsBytes);
    std::memcpy(bytes.data() + 4096, mapped + 4096, capture.psBytes);
    std::memcpy(bytes.data() + 8192, mapped + 8192, 512);
    for (const auto& image : capture.images)
      for (uint32_t row = 0; row < image.height; ++row)
        std::memcpy(bytes.data() + image.rawOffset + row * image.rowBytes,
                    mapped + image.footprint.Offset + uint64_t(row) * image.footprint.Footprint.RowPitch,
                    size_t(image.rowBytes));
    const D3D12_RANGE noWrite{0, 0}; capture.readback->Unmap(0, &noWrite);
    HANDLE file = CreateFileA(capture.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      nativeRecordingFail("hdr-async-file-create", HRESULT_FROM_WIN32(GetLastError()));
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    const DWORD error = wrote ? ERROR_WRITE_FAULT : GetLastError();
    const BOOL closed = CloseHandle(file);
    const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
    if (!wrote || written != bytes.size()) nativeRecordingFail("hdr-async-write", HRESULT_FROM_WIN32(error));
    if (!closed) nativeRecordingFail("hdr-async-file-close", HRESULT_FROM_WIN32(closeError));
    const char* stage = kHdrProbeStages[capture.stage].name;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[HDR_ASYNC_PROBE] stage=%s freeze=%u frame=%llu batch=%zu sequence=%u held=%.3f "
                "same_hold=%u ticket=%llu vs=%016llX ps=%016llX raw_bytes=%llu "
                "source_offsets=%llu,%llu,%llu float_extents=%u,%u raw=%s",
                stage, capture.freeze, (unsigned long long)capture.frame, capture.batch, capture.sequence,
                capture.held, capture.sameHold, (unsigned long long)capture.ticket,
                (unsigned long long)capture.vsHash, (unsigned long long)capture.psHash,
                (unsigned long long)capture.rawBytes, (unsigned long long)capture.vsOffset,
                (unsigned long long)capture.psOffset, (unsigned long long)capture.sharedOffset, capture.vsBytes,
                capture.psBytes, capture.path.c_str());
    for (size_t i = 0; i < capture.images.size(); ++i) {
      const auto& image = capture.images[i];
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[HDR_ASYNC_IMAGE] stage=%s freeze=%u frame=%llu batch=%zu sequence=%u role=%zu "
                  "slot=%u owner=%u key=%08X cache_key=%016llX mapping=%08X "
                  "resource_format=%u view_format=%u region=%u,%u,%u,%u "
                  "raw_offset=%llu row_bytes=%llu gpu_offset=%llu gpu_pitch=%u",
                  stage, capture.freeze, (unsigned long long)capture.frame, capture.batch, capture.sequence, i,
                  image.slot, image.owner, image.key, (unsigned long long)image.cacheKey, image.mapping,
                  image.resourceFormat, uint32_t(image.viewFormat), image.x, image.y, image.width, image.height,
                  (unsigned long long)image.rawOffset, (unsigned long long)image.rowBytes,
                  (unsigned long long)image.footprint.Offset, image.footprint.Footprint.RowPitch);
    }
    m_hdrProbePending.pop_front();
  }
}

bool D3D12Backend::uploadNativeOwnerPlan(const native_owner::Plan& plan, D3D12_GPU_VIRTUAL_ADDRESS& address) {
  address = 0;
  if (!m_pm4Upload || !m_pm4UploadPtr || plan.count > native_owner::kMaxRects) return false;
  // This allocator is reset only after pm4Flush has waited for the GPU. Keep
  // the Oracle probe's reserved tail untouched. Call before binding any state:
  // a full upload arena closes and resets the current command list.
  constexpr uint64_t limit = kPm4StagingBytes - (4ull << 20);
  uint64_t offset = pm4AlignUp(m_pm4UploadUsed, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  if (offset > limit || sizeof(plan) > limit - offset) {
    pm4Flush();
    offset = 0;
  }
  std::memcpy(m_pm4UploadPtr + offset, &plan, sizeof(plan));
  m_pm4UploadUsed = offset + sizeof(plan);
  address = m_pm4Upload->GetGPUVirtualAddress() + offset;
  return true;
}

void D3D12Backend::pm4Flush() {
  gtEnd();  // iter-464 SR_PM4_GPU_TIME (no-op when off)
  clClose(Pm4Close);
  clExecute(Pm4Exec);
  waitForGpu();
  checkDeviceRemoved("pm4Flush");
  pm4ShmAfterFlush();
  if (m_dbgActive) drainDebugLayer();  // SR_D3D12_DEBUG: name the call, not the corpse
  m_pm4Graveyard.clear();              // the GPU is done with retired scratch buffers
  clReset(Pm4Reset);
  gtBegin();
  if (xlatReady()) {
    ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
    m_commandList->SetDescriptorHeaps(2, heaps);
  }
  m_pm4UploadUsed = 0;
  g_pm4Stats.flushes++;
}


// (Load: mirror -> each destination's moved tiles - edramLoadTiles, the oracle unit, below the claim.)
uint32_t D3D12Backend::nbPropagateClears(std::vector<Pm4Move>& moves, size_t bi) {
  if (g_nbUni.empty()) return 0;
  thread_local std::vector<Pm4Move> t_keep;
  t_keep.clear();
  uint32_t tiles = 0;
  for (const Pm4Move& mv : moves) {
    const auto it = mv.src.empty() ? g_nbUni.end() : g_nbUni.find(mv.src.v);
    float col[4] = {}, depth = 0.0f;
    uint32_t stencil = 0;
    uint64_t bits = 0;
    Pm4HostRt* rt = nullptr;
    edram::Rect rects[edram::kRangeRectsMax];
    int n = 0;
    if (it == g_nbUni.end() || !nbUniCovers(mv.src, it->second, mv.start, mv.end) ||
        !nbPropValue(mv.src, mv.dst, it->second, col, depth, stencil, bits) || !(rt = pm4Rt(mv.dst)) ||
        !(n = edram::rangeRects(mv.dst, mv.start, mv.end, rects))) {
      if (it != g_nbUni.end()) g_nbPropKept++;
      t_keep.push_back(mv);
      continue;
    }
    D3D12_RECT dr[edram::kRangeRectsMax];
    for (int i = 0; i < n; i++) {
      const edram::Rect r = nbHostRect(rects[i]);
      dr[i] = D3D12_RECT{LONG(r.x), LONG(r.y), LONG(std::min(r.x + r.w, rt->w)), LONG(std::min(r.y + r.h, rt->h))};
    }
    if (mv.dst.isDepth()) {
      pm4Transition(*rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      m_commandList->ClearDepthStencilView(rt->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth,
                                           UINT8(stencil), UINT(n), dr);
    } else {
      pm4Transition(*rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
      m_commandList->ClearRenderTargetView(rt->view, col, UINT(n), dr);
    }
    tiles += mv.end - mv.start;
    g_nbPropTiles += mv.end - mv.start;
#if SR_ORACLE
    if (m_nbFrameOn) {  // census: the move this clear replaced (one record per move, as the elided sets)
      const uint32_t pid = nbAdd("P batch=%zu [%08X(%c%u%s)->%08X(%c%u%s) t%u+%u]", bi, mv.src.v,
                                 mv.src.isDepth() ? 'D' : 'C', mv.src.format(), mv.src.msaa() ? "m" : "", mv.dst.v,
                                 mv.dst.isDepth() ? 'D' : 'C', mv.dst.format(), mv.dst.msaa() ? "m" : "", mv.start,
                                 mv.end - mv.start);
      nbBegin(pid);
      nbEnd();
    }
#endif
    if (++g_nbPropMoves <= 24 || (g_nbPropMoves & (g_nbPropMoves - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NB_CLEAR_PROP] batch=%zu %08X -> %08X tiles [%u,%u) from the clear at draw %u: bits %016llX -> "
                  "%s (%.9g %.9g %.9g %.9g | d=%.9g s=%u) (moves %llu, tiles %llu, kept %llu)",
                  bi, mv.src.v, mv.dst.v, mv.start, mv.end, it->second.draw, (unsigned long long)bits,
                  mv.dst.isDepth() ? "depth clear" : "colour clear", col[0], col[1], col[2], col[3], depth, stencil,
                  (unsigned long long)g_nbPropMoves, (unsigned long long)g_nbPropTiles,
                  (unsigned long long)g_nbPropKept);
  }
  moves.swap(t_keep);
  return tiles;
}

// SR_NB_ZERO_INIT (T5): each load of never-claimed tiles (no source) becomes a host clear of its destination to zero;
// returns the tiles cleared. The moves left keep the EDRAM path.
uint32_t D3D12Backend::nbZeroLoads(std::vector<Pm4Move>& moves, size_t bi) {
  if (!m_pm4EdramPristine || !nbZeroInitOn()) return 0;
  bool any = false;
  for (const Pm4Move& mv : moves) any = any || mv.src.empty();
  if (!any) return 0;
  thread_local std::vector<Pm4Move> t_keep;
  t_keep.clear();
  uint32_t tiles = 0;
  for (const Pm4Move& mv : moves) {
    Pm4HostRt* rt = nullptr;
    edram::Rect rects[edram::kRangeRectsMax];
    int n = 0;
    if (!mv.src.empty() || !(rt = pm4Rt(mv.dst)) || !(n = edram::rangeRects(mv.dst, mv.start, mv.end, rects))) {
      t_keep.push_back(mv);
      continue;
    }
    D3D12_RECT dr[edram::kRangeRectsMax];
    for (int i = 0; i < n; i++) {
      const edram::Rect r = nbHostRect(rects[i]);
      dr[i] = D3D12_RECT{LONG(r.x), LONG(r.y), LONG(std::min(r.x + r.w, rt->w)), LONG(std::min(r.y + r.h, rt->h))};
    }
    if (mv.dst.isDepth()) {
      pm4Transition(*rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      m_commandList->ClearDepthStencilView(rt->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0,
                                           UINT(n), dr);
    } else {
      static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      pm4Transition(*rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
      m_commandList->ClearRenderTargetView(rt->view, kZero, UINT(n), dr);
    }
    g_nbUni.erase(mv.dst.v);  // B1c: the key's tiles were rewritten (as pm4LoadTiles)
    tiles += mv.end - mv.start;
    g_nbZeroTiles += mv.end - mv.start;
#if SR_ORACLE
    if (m_nbFrameOn) {  // census: the load this clear replaced
      const uint32_t pid = nbAdd("Z batch=%zu [->%08X(%c%u%s) t%u+%u]", bi, mv.dst.v, mv.dst.isDepth() ? 'D' : 'C',
                                 mv.dst.format(), mv.dst.msaa() ? "m" : "", mv.start, mv.end - mv.start);
      nbBegin(pid);
      nbEnd();
    }
#endif
    if (++g_nbZeroMoves <= 16 || (g_nbZeroMoves & (g_nbZeroMoves - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NB_ZERO_INIT] batch=%zu load of never-claimed tiles [%u,%u) -> %08X: host clear to zero (moves %llu, "
                  "tiles %llu; resolves %llu)",
                  bi, mv.start, mv.end, mv.dst.v, (unsigned long long)g_nbZeroMoves, (unsigned long long)g_nbZeroTiles,
                  (unsigned long long)g_nbZeroRes);
  }
  moves.swap(t_keep);
  return tiles;
}

#if SR_ORACLE
// SR_NB_CLEAR_PROP_VERIFY (diagnostic): after the real transfers of this set, read back the first and the last
// texel each propagatable move wrote and compare them with the value nbPropagateClears would clear with.
void D3D12Backend::nbPropVerify(const std::vector<Pm4Move>& moves, size_t bi) {
  static uint64_t s_checked = 0, s_bad = 0;
  struct Probe { const Pm4Move* mv; uint32_t x, y; uint64_t off[2]; uint32_t bpt[2]; float col[4]; float depth;
                 uint32_t stencil; };
  std::vector<Probe> probes;
  const uint64_t region = kPm4StagingBytes - (4ull << 20);  // the tail: no other user mid-list
  uint64_t cur = region;
  for (const Pm4Move& mv : moves) {
    const auto it = mv.src.empty() ? g_nbUni.end() : g_nbUni.find(mv.src.v);
    float col[4] = {}, depth = 0.0f;
    uint32_t stencil = 0;
    uint64_t bits = 0;
    edram::Rect rects[edram::kRangeRectsMax];
    int n = 0;
    Pm4HostRt* rt = nullptr;
    if (it == g_nbUni.end() || !nbUniCovers(mv.src, it->second, mv.start, mv.end) ||
        !nbPropValue(mv.src, mv.dst, it->second, col, depth, stencil, bits) || !(rt = pm4Rt(mv.dst)) ||
        !(n = edram::rangeRects(mv.dst, mv.start, mv.end, rects)))
      continue;
    const edram::Rect a = nbHostRect(rects[0]), z = nbHostRect(rects[n - 1]);
    const uint32_t px[2][2] = {{a.x, a.y}, {std::min(z.x + z.w, rt->w) - 1u, std::min(z.y + z.h, rt->h) - 1u}};
    D3D12_RESOURCE_DESC desc = rt->res->GetDesc();
    for (int p = 0; p < 2; p++) {
      Probe pr{&mv, px[p][0], px[p][1], {}, {}, {col[0], col[1], col[2], col[3]}, depth, stencil};
      const UINT planes = mv.dst.isDepth() ? 2u : 1u;
      for (UINT pl = 0; pl < planes; pl++) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        m_device->GetCopyableFootprints(&desc, pl, 1, 0, &fp, &rows, &rowBytes, &total);
        if (!fp.Footprint.Width) continue;
        pr.bpt[pl] = uint32_t(rowBytes / fp.Footprint.Width);
        cur = (cur + 511u) & ~uint64_t(511u);
        fp.Offset = cur;
        fp.Footprint.Width = 1;
        fp.Footprint.Height = 1;
        fp.Footprint.Depth = 1;
        fp.Footprint.RowPitch = 256;
        pr.off[pl] = cur;
        cur += 512;
        pm4Transition(*rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m_pm4Readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = rt->res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = pl;
        const D3D12_BOX box{pr.x, pr.y, 0, pr.x + 1, pr.y + 1, 1};
        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
      }
      probes.push_back(pr);
    }
  }
  if (probes.empty()) return;
  pm4Flush();
  uint8_t* mp = nullptr;
  const D3D12_RANGE rr{SIZE_T(region), SIZE_T(cur)};
  if (FAILED(m_pm4Readback->Map(0, &rr, reinterpret_cast<void**>(&mp))) || !mp) return;
  for (const Probe& pr : probes) {
    const Pm4Move& mv = *pr.mv;
    bool ok;
    char got[96];
    if (mv.dst.isDepth()) {
      uint32_t d0 = 0;
      std::memcpy(&d0, mp + pr.off[0], 4);
      const uint32_t st = mp[pr.off[1] + pm4StencilByte(pr.bpt[1])];
      uint32_t want0;
      if (mv.dst.format() == 1u) {
        want0 = edram::f2u(pr.depth);
      } else {  // D24S8 plane 0: the unorm24 in the low 24 bits
        want0 = edram::packDepth(0u, pr.depth, 0u) >> 8;
        d0 &= 0xFFFFFFu;
      }
      ok = d0 == want0 && st == pr.stencil;
      std::snprintf(got, sizeof(got), "d=%08X s=%u (want %08X s=%u)", d0, st, want0, pr.stencil);
    } else {
      const edram::HostFmt hf = mv.dst.hostFormat();
      const uint32_t bpt = edram::hostBytesPerTexel(hf);
      uint8_t want[8] = {};
      edram::float4ToHost(hf, pr.col, want);
      ok = std::memcmp(mp + pr.off[0], want, bpt) == 0;
      uint64_t g = 0, w = 0;
      std::memcpy(&g, mp + pr.off[0], bpt);
      std::memcpy(&w, want, bpt);
      std::snprintf(got, sizeof(got), "%016llX (want %016llX)", (unsigned long long)g, (unsigned long long)w);
    }
    s_checked++;
    if (!ok) s_bad++;
    if (!ok || s_checked <= 16 || (s_checked & (s_checked - 1)) == 0)
      SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                  "[NB_CLEAR_PROP] VERIFY %s batch=%zu %08X -> %08X tiles [%u,%u) at (%u,%u): %s (checked %llu, "
                  "mismatches %llu)",
                  ok ? "ok" : "MISMATCH", bi, mv.src.v, mv.dst.v, mv.start, mv.end, pr.x, pr.y, got,
                  (unsigned long long)s_checked, (unsigned long long)s_bad);
  }
  const D3D12_RANGE wr{0, 0};
  m_pm4Readback->Unmap(0, &wr);
}
#endif  // SR_ORACLE (nbPropVerify)

// The claim (C1 stage 3, docs/RELEASE_COMPOSITION.md 8.2): [start, end) of the EDRAM tiles for k - the runs nobody
// owns first (loads from nowhere: the power-on zero, which SR_NB_ZERO_INIT turns into host clears; the trace's snapshot
// in the oracle build), then the previous owners' runs as moves into k - decided by the surface table
// (gfx/nb_surfaces.hpp, SR_NB_SURF_TABLE: the alias table the shipping path decides with). The development build's oracle unit
// (edramClaim) runs the tile map beside it under SR_NB_SURF_VERIFY, or instead of it with SR_NB_SURF_TABLE=0.
// iter-467 native-b B-CPU (SR_NB_CLAIM_FAST, baked ON after cr2; =0 off): nearly every batch claims tiles its own
// binding already owns (strand A's pr_off profile: pm4Claim 0.92 ms / frame on the render thread, most of it the two
// vectors below and the split / merge churn). Owned whole by `k`: no unowned tile, no transfer, no table change -
// exactly what the full path below computes, without it.
void D3D12Backend::pm4ClaimTable(edram::RtKey k, uint32_t start, uint32_t end, std::vector<Pm4Move>& out) {
  nbsurf::Table& st = nbSurfTable();
  if (nbClaimFastOn() && (end <= edram::kTileCount ? st.ownedBy(k, start, end)
                                                   : st.ownedBy(k, start, edram::kTileCount) &&
                                                         st.ownedBy(k, 0, end - edram::kTileCount)))
    return;
  thread_local std::vector<edram::Transfer> t_o, t_tr;
  t_o.clear();
  t_tr.clear();
  if (end <= edram::kTileCount) {
    st.owners(start, end, t_o);
  } else {
    st.owners(start, edram::kTileCount, t_o);
    st.owners(0, end - edram::kTileCount, t_o);
  }
  for (const auto& p : t_o)
    if (p.source.empty()) out.push_back(Pm4Move{edram::RtKey(), k, p.start, p.end});
  if (end <= edram::kTileCount) {
    st.claimExtent(k, start, end, t_tr);
  } else {
    st.claimExtent(k, start, edram::kTileCount, t_tr);
    st.claimExtent(k, 0, end - edram::kTileCount, t_tr);
  }
  for (const auto& x : t_tr) out.push_back(Pm4Move{x.source, k, x.start, x.end});
}

void D3D12Backend::pm4Claim(edram::RtKey k, uint32_t start, uint32_t len,
                            std::vector<Pm4Move>& moves) {
  if (!len) return;
  len = std::min(len, edram::kTileCount);
  start %= edram::kTileCount;
  const uint32_t end = start + len;
#if SR_ORACLE
  edramClaim(k, start, end, moves);  // the oracle unit: the table, with the tile map beside / instead of it
#else
  pm4ClaimTable(k, start, end, moves);
#endif
}

#if !SR_ORACLE
// The release configuration has no EDRAM word model (docs/RELEASE_COMPOSITION.md 8.2): a live trace carries no EDRAM
// snapshot and a replay restores none - what a scaled trace / replay already does (Xenia parity: no 1:1 mapping).
void D3D12Backend::pm4TraceInit(livetrace::InitState& out) {
  (void)out;
  if (!pm4Init()) return;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_RT] live trace: no EDRAM snapshot in the release composition");
}
void D3D12Backend::pm4RestoreEdram(const uint8_t* snapshot, size_t bytes) {
  (void)snapshot;
  if (!pm4Init()) return;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_RT] snapshot (%zu bytes) NOT restored: no EDRAM word model in the release composition", bytes);
}
#endif


// The source surface's tile span of a resolve (whole tile rows covering [y0, y1)) and its owners from the surface
// table (C1 stage 3: the alias table; the oracle unit's edramSpanOwners runs the tile map beside / instead of it).
void D3D12Backend::pm4SpanOwners(edram::RtKey k, const edram::ResolveInfo& ri, std::vector<edram::Transfer>& out) {
  const uint32_t yShift = k.msaa() >= edram::kMsaa2X ? 1u : 0u;
  const uint32_t pt = k.pitchTiles();
  const uint32_t row0 = (ri.y0 << yShift) / edram::kTileHeightSamples;
  const uint32_t row1 = ((ri.y1 << yShift) + edram::kTileHeightSamples - 1) / edram::kTileHeightSamples;
  const uint32_t len = std::min((row1 - row0) * pt, edram::kTileCount);
  const uint32_t start = (k.base() + row0 * pt) % edram::kTileCount;
  const uint32_t end = start + len;
  if (end <= edram::kTileCount) {
    nbSurfTable().owners(start, end, out);
  } else {
    nbSurfTable().owners(start, edram::kTileCount, out);
    nbSurfTable().owners(0, end - edram::kTileCount, out);
  }
}

// iter-467 native-b (EDRAM zero; strand A's Resolve / Clear RECORD, coordinator D3): a resolve - and the clears it
// carries - is driven by the register snapshot alone (edram::Regs: surface / colour / depth info, the copy and clear
// registers, the window offset and scissors, PA_SU_VTX_CNTL and the resolve rectangle), so the record stream can call
// pm4ExecResolveRegs with its record's snapshot exactly as the PM4 path does with a batch's.
void D3D12Backend::pm4ExecResolve(const DrawBatch& b, size_t bi) {
  struct GenerationScope {
    uint32_t& current;
    uint32_t previous;
    ~GenerationScope() { current = previous; }
  } scope{m_resourceGpuGeneration, m_resourceGpuGeneration};
  m_resourceGpuGeneration = b.resourceGpuGeneration;
  pm4ExecResolveRegs(b.pm4Regs, bi);
}

void D3D12Backend::pm4ExecResolveRegs(const edram::Regs& regs, size_t bi) {
  const edram::ResolveInfo ri = edram::computeResolve(regs);
  if (!ri.valid) return;
  g_pm4Stats.resolves++;
  // iter-467 native-b B0 census (SR_NB_CENSUS): this resolve's record, current until the caller's nbEnd.
  const uint32_t nbId = nbResolveBegin(ri);
  char nbPath = ri.copy ? '?' : '-';
  uint32_t nbExtS = 0, nbExtL = 0;
  // The source surface's tile span (whole tile rows covering [y0, y1)): its owners from the alias table (C1 stage 3;
  // the oracle unit's edramSpanOwners adds the tile map under SR_NB_SURF_VERIFY / SR_NB_SURF_TABLE=0).
  auto spanOwners = [&](edram::RtKey k, std::vector<edram::Transfer>& out) {
#if SR_ORACLE
    edramSpanOwners(k, ri, out);
#else
    pm4SpanOwners(k, ri, out);
#endif
  };
  // NATIVE N4a (SR_NATIVE_RESOLVE=1): resolve straight from the source host RT; no store of
  // the span's owners into the EDRAM buffer. Depth / scaled resolves keep the path below.
  uint32_t natStart = 0, natLen = 0;
  // Native resolves read the SOURCE key's host RT, so they are taken only when that key
  // owns every tile of the span. A span another key owns (same EDRAM base, another
  // format or MSAA mode - roadmap Q5) is a reinterpretation: it keeps the EDRAM path.
  // N4a step 2: a span ONE other surface owns at the same base, pitch and bpp (the 4x / 1x
  // depth pair 007082D0 / 006082D0) resolves natively from that surface; anything else
  // (several owners, another bpp or pitch, depth <-> colour) keeps the EDRAM path.
  // N4a: the span's owners. ONE owner at the same base, pitch and bpp (the source itself, or
  // a surface with another format / MSAA mode / depth-ness) resolves natively from it; a MIXED
  // span of exactly one colour + one depth owner (32bpp) picks the owner per tile in the shader.
  // Anything else keeps the EDRAM path.
  bool natOwned = false;
  Pm4NatOwners natOwners;
  natOwners.a = ri.srcKey;
  uint32_t nbRuns = 0, nbNone = 0, nbKeys = 0;  // [NB_EDRAM_RESOLVE]: the span's owner runs, unowned runs, keys
  if (ri.copy) {
    std::vector<edram::Transfer> own;
    spanOwners(ri.srcKey, own);
    nbRuns = uint32_t(own.size());
    for (const auto& o : own) nbNone += o.source.empty() ? 1u : 0u;
    const edram::RtKey s = ri.srcKey;
    // SR_NB_ZERO_INIT (R0): a span of never-claimed tiles (plus the source's own) reads the power-on zero - clear the
    // source's host RT there (pixels of tiles it does not own: nobody else reads them, a later claim loads them) and
    // resolve natively from it. Ownership is unchanged.
    if (m_pm4EdramPristine && nbZeroInitOn() && !own.empty()) {
      bool anyNone = false, selfOrNone = true;
      for (const auto& o : own) {
        if (o.source.empty()) anyNone = true;
        else if (o.source.v != s.v) selfOrNone = false;
      }
      Pm4HostRt* srt = (anyNone && selfOrNone) ? pm4Rt(s) : nullptr;
      if (srt) {
        uint32_t zt = 0;
        for (auto& o : own) {
          if (!o.source.empty()) continue;
          edram::Rect rects[edram::kRangeRectsMax];
          const int n = edram::rangeRects(s, o.start, o.end, rects);
          D3D12_RECT dr[edram::kRangeRectsMax];
          for (int i = 0; i < n; i++) {
            const edram::Rect r = nbHostRect(rects[i]);
            dr[i] = D3D12_RECT{LONG(r.x), LONG(r.y), LONG(std::min(r.x + r.w, srt->w)),
                               LONG(std::min(r.y + r.h, srt->h))};
          }
          if (n && s.isDepth()) {
            pm4Transition(*srt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            m_commandList->ClearDepthStencilView(srt->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f,
                                                 0, UINT(n), dr);
          } else if (n) {
            static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            pm4Transition(*srt, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_commandList->ClearRenderTargetView(srt->view, kZero, UINT(n), dr);
          }
          zt += o.end - o.start;
          o.source = s;
        }
        g_nbUni.erase(s.v);  // B1c: the key's pixels changed
        if (++g_nbZeroRes <= 16 || (g_nbZeroRes & (g_nbZeroRes - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[NB_ZERO_INIT] resolve of %08X over %u never-claimed tiles: the source cleared to zero there, "
                      "native path (resolves %llu)",
                      s.v, zt, (unsigned long long)g_nbZeroRes);
      }
    }
    edram::RtKey keys[2];
    int nk = 0;
    natOwned = !own.empty();
    for (const auto& o : own) {
      if (o.source.empty()) {
        natOwned = false;
        continue;
      }
      if ((nk >= 1 && keys[0].v == o.source.v) || (nk >= 2 && keys[1].v == o.source.v)) continue;
      if (nk == 2) {
        natOwned = false;
        continue;
      }
      keys[nk++] = o.source;
    }
    auto shapeOk = [&](edram::RtKey o) {
      // Exact only when each source pixel lands on ONE owner pixel (source samples <= owner's).
      const bool gridOk = (s.msaa() >= edram::kMsaa4X ? 1u : 0u) <= (o.msaa() >= edram::kMsaa4X ? 1u : 0u) &&
                          (s.msaa() >= edram::kMsaa2X ? 1u : 0u) <= (o.msaa() >= edram::kMsaa2X ? 1u : 0u);
      return o.base() == s.base() && o.pitchTiles32() == s.pitchTiles32() && o.intsLog2() == s.intsLog2() &&
             gridOk;
    };
    for (int k = 0; k < nk; k++) natOwned = natOwned && shapeOk(keys[k]);
    nbKeys = uint32_t(nk);
    if (natOwned && nk == 2) {
      natOwned = keys[0].isDepth() != keys[1].isDepth() && s.intsLog2() == 0;
      for (const auto& o : own) {
        if (!natOwned || o.source.v != keys[1].v) continue;
        if (natOwners.nRanges >= 4) {
          natOwned = false;
          break;
        }
        natOwners.ranges[2 * natOwners.nRanges] = o.start;
        natOwners.ranges[2 * natOwners.nRanges + 1] = o.end;
        natOwners.nRanges++;
      }
    }
    // SR_NB_RESOLVE_CLAIM (EDRAM zero): a span the native kernel cannot read from its owners (owners of another base /
    // pitch / bpp, two colour owners) is first moved into the source surface - the claim a draw binding the source
    // would make, the same exact transfers - and then resolved natively from it. The EDRAM content is unchanged (the
    // source's host RT holds those tiles now). job36 (fr34): the run's one EDRAM-model resolve, at boot - 00C08000
    // (k_2_10_10_10_FLOAT 1280x720) over 03800800 (k_32_FLOAT, pitch 1 tile) and a second colour key.
    if (!natOwned && nbResolveClaimOn() && !own.empty() && pm4Rt(s)) {
      std::vector<Pm4Move> cm;
      for (const auto& o : own)
        if (o.source.v != s.v) pm4Claim(s, o.start, o.end - o.start, cm);
      const uint32_t zt = cm.empty() ? 0u : nbZeroLoads(cm, bi);
      m_meterCur.elidedTiles += zt;
      pm4MeterMoves(cm);  // NATIVE METER: transfers (moves decided by the tile map)
      pm4MoveTiles(cm);
      for (auto& o : own) o.source = s;
      keys[0] = s;
      nk = 1;
      natOwners.nRanges = 0;
      natOwned = shapeOk(s);
      static uint64_t s_rc = 0;
      if (++s_rc <= 16 || (s_rc & (s_rc - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[NB_RESOLVE_CLAIM] #%llu resolve of %08X: span moved into the source (%zu moves, %u zero-cleared "
                    "tiles), native path",
                    (unsigned long long)s_rc, s.v, cm.size(), zt);
    }
    if (natOwned) {
      natOwners.a = keys[0];
      if (nk == 2) natOwners.b = keys[1];
    }
    static uint64_t s_alias = 0, s_aliasNative = 0, s_mixedNative = 0;
    if (natOwned && nk == 2) {
      if (++s_mixedNative <= 8 || (s_mixedNative & (s_mixedNative - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[NATIVE_RESOLVE] span of key %08X owned by %08X + %08X (%u runs of the second): native, owner per tile (%llu so far)",
                    s.v, keys[0].v, keys[1].v, natOwners.nRanges, (unsigned long long)s_mixedNative);
    } else if (natOwned && keys[0].v != s.v) {
      if (++s_aliasNative <= 8 || (s_aliasNative & (s_aliasNative - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[NATIVE_RESOLVE] span of key %08X owned by %08X (same base/pitch/bpp): native from the owner (%llu so far)",
                    s.v, keys[0].v, (unsigned long long)s_aliasNative);
    }
    if (!natOwned && (++s_alias <= 8 || (s_alias & (s_alias - 1)) == 0))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NATIVE_RESOLVE] span of key %08X owned by %08X (%zu owner runs, %d keys): EDRAM path (%llu so far)",
                  s.v, own.empty() ? 0u : own[0].source.v, own.size(), nk, (unsigned long long)s_alias);
  }
  const edram::RtKey natOwner = natOwners.a;
  // iter-467 native-b B2 (SR_NB_RESOLVE_TEX): a destination that is exactly a cached texture's identity is
  // resolved INTO that texture; the memory is held (gfx/native_targets.inc). Single owners only.
  const bool nbTex = ri.copy && natOwned && (natOwners.b.empty() || pm4NbScaledRemapOn()) &&
                     nbResolveToTarget(ri, regs, natOwners, natStart, natLen);
  const bool natDone =
      nbTex || (ri.copy && natOwned && pm4NativeResolve(ri, regs, natOwners, natStart, natLen));
  const bool viaOwner = natDone && (natOwner.v != ri.srcKey.v || !natOwners.b.empty());
  // release/emu-stage3 (Astra round 1 P1): the census of the copies the native kernel did not take - a span it was
  // asked for and refused (owned; nbNatStatus names why), or one the owner selection kept from it ([NB_CENSUS]
  // "native declined", [NB_EDRAM_RESOLVE]). The development build's model takes them below; the release stops.
  if (ri.copy && !natDone) natNoteDeclined(natOwned);
  if (natDone) {
    nbPath = nbTex ? 'T' : viaOwner ? (natOwners.b.empty() ? 'O' : 'M') : 'N';
    nbExtS = natStart;
    nbExtL = natLen;
    g_pm4Stats.resolveTexels += (ri.x1 - ri.x0) * (ri.y1 - ri.y0);
    invalidateGuestRange(natStart & 0x1FFFFFFFu, natLen);
#if SR_ORACLE
    pm4ResolveHashDebug(ri, natStart, natLen, true);  // SR_NATIVE_RESOLVE_HASH (diagnostic)
#endif
  }
#if SR_ORACLE
  // The EDRAM path (the oracle unit, gfx/d3d12_edram_model.cpp): a copy the native kernel did not take, and beside a
  // native one under SR_NATIVE_RESOLVE_DUAL / SR_NATIVE_RESOLVE_VERIFY.
  if (ri.copy)
    edramResolve(ri, regs, bi, natDone, nbTex, natOwner, viaOwner, natStart, natLen, nbPath, nbExtS, nbExtL);
#else
  if (ri.copy && !natDone) {
    // C1 stage 3 (docs/RELEASE_COMPOSITION.md 3.1 rule 4): the release configuration has no EDRAM path to fall back
    // to - a copy the native kernel refused stops the renderer, as [NATIVE_FLUSH_BAD] stops the consumer.
    // The status names the kernel's last refusal ("last="): warm / failed = the pipelines (not ready / not built),
    // array-dest = copy_dest_array=1 (implemented in no path, never seen from the title), the rest a shape or owner
    // the kernel does not read; natOwned=0 = the owner selection kept the span from the kernel (another base /
    // pitch / bpp, two colour owners).
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NATIVE_RESOLVE_BAD] a resolve the native kernel did not take: src=%08X fmt=%u msaa=%u depth=%d "
                "rect=(%u,%u)-(%u,%u) dest=%08X dfmt=%u arr=%d | owner runs=%u unowned=%u keys=%u natOwned=%d | %s",
                ri.srcKey.v, ri.srcFormat, ri.msaa, ri.isDepth ? 1 : 0, ri.x0, ri.y0, ri.x1, ri.y1, ri.destBase,
                ri.destFormat, ri.destArray ? 1 : 0, nbRuns, nbNone, nbKeys, natOwned ? 1 : 0, nbNatStatus().c_str());
    sr::Log::shutdown();
    std::_Exit(3);
  }
#endif
  // Clear-after-copy (draw_util.h:688-716): the same 8-aligned rect in each
  // target's coordinates, after ownership is claimed (partial tiles keep the
  // previous owner's pixels through the transfer).
  if ((ri.clearColor || ri.clearDepth) && ri.x1 > ri.x0 && ri.y1 > ri.y0) {
    const auto tc = Pm4Clk::now();
    struct ClearTimer { Pm4Clk::time_point t0; ~ClearTimer() { g_resMsClear += std::chrono::duration<double, std::milli>(Pm4Clk::now() - t0).count(); } } clearTimer{tc};
    auto clearKey = [&](edram::RtKey k, bool depth) {
      Pm4HostRt* rt = pm4Rt(k);
      if (!rt) return;
      g_nbUni.erase(k.v);  // B1c (SR_NB_CLEAR_PROP): the resolve's clear rewrites the key
      const uint32_t yShift = k.msaa() >= edram::kMsaa2X ? 1u : 0u;
      const uint32_t row0 = (ri.y0 << yShift) / edram::kTileHeightSamples;
      const uint32_t row1 = ((ri.y1 << yShift) + edram::kTileHeightSamples - 1) / edram::kTileHeightSamples;
      std::vector<Pm4Move> moves;
      pm4Claim(k, (k.base() + row0 * k.pitchTiles()) % edram::kTileCount,
               (row1 - row0) * k.pitchTiles(), moves);
      if (!moves.empty()) m_meterCur.elidedTiles += nbZeroLoads(moves, bi);  // SR_NB_ZERO_INIT (T5)
      pm4MeterMoves(moves);  // NATIVE METER
      pm4MoveTiles(moves);
      // iter-464 resscale: the guest-pixel rectangle times the scale
      // (d3d12_render_target_cache.cc:4526-4535); rt->w / rt->h are host pixels.
      const resscale::Scale& rs = resscale::get();
      const D3D12_RECT rect{LONG(ri.x0 * rs.x), LONG(ri.y0 * rs.y),
                            LONG(std::min(ri.x1 * rs.x, rt->w)),
                            LONG(std::min(ri.y1 * rs.y, rt->h))};
      if (depth) {
        pm4Transition(*rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        const float d = edram::unpackDepth(k.format(), regs.depthClear);
        // RB_DEPTH_CLEAR = the guest word: depth << 8 | stencil.
        m_commandList->ClearDepthStencilView(rt->view,
                                             D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, d,
                                             UINT8(regs.depthClear & 0xFFu), 1, &rect);
      } else {
        pm4Transition(*rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const uint64_t bits = k.is64bpp() ? (uint64_t(regs.colorClear) |
                                            (uint64_t(regs.colorClearLo) << 32))
                                          : uint64_t(regs.colorClear);
        float c[4];
        edram::unpackColor(k.format(), bits, c);
        m_commandList->ClearRenderTargetView(rt->view, c, 1, &rect);
      }
      g_pm4Stats.clears++;
    };
    if (ri.clearColor) clearKey(ri.colorClearKey, false);
    if (ri.clearDepth) clearKey(ri.depthClearKey, true);
  }
  // NATIVE METER: T = into the sampling texture; N / O / M = host RT -> guest-layout memory; E / C = the EDRAM model.
  if (nbPath == 'T') m_meterCur.resolvesTarget++;
  else if (nbPath == 'N' || nbPath == 'O' || nbPath == 'M') m_meterCur.resolvesGuestMem++;
  else if (nbPath == 'E' || nbPath == 'C') {
    m_meterCur.resolvesEdram++;
    // iter-467 native-b (EDRAM zero): with the shipped kernels the default build resolves nothing through the EDRAM
    // model (job33 aot_live: 1 of a whole run, in the first window) - name the ones left (Warn: a player log keeps
    // the first 4).
    static std::atomic<uint32_t> s_edramRes{0};
    const uint32_t n = s_edramRes.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (n <= 16u)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NB_EDRAM_RESOLVE] #%u path=%c meter-frame=%llu src=%08X fmt=%u msaa=%u depth=%d copy=%d raw=%d "
                  "rect=(%u,%u)-(%u,%u) dest=%08X pitch=%u h=%u dfmt=%u | owner runs=%u unowned=%u keys=%u "
                  "natOwned=%d pristine=%d | %s",
                  n, nbPath, (unsigned long long)m_meterCur.frame, ri.srcKey.v, ri.srcFormat, ri.msaa,
                  ri.isDepth ? 1 : 0, ri.copy ? 1 : 0, ri.raw ? 1 : 0, ri.x0, ri.y0, ri.x1, ri.y1, ri.destBase,
                  ri.destPitch, ri.destHeight, ri.destFormat, nbRuns, nbNone, nbKeys, natOwned ? 1 : 0,
                  m_pm4EdramPristine ? 1 : 0, nbNatStatus().c_str());
  }
  nbResolveEnd(nbId, ri, nbPath, nbExtS, nbExtL);  // registers the destination even when not recorded
  nbResolveHash(ri, nbPath, nbExtS, nbExtL);       // SR_NB_RHASH (diagnostic)
}

void D3D12Backend::pm4PresentFrame(uint32_t fbPhys, uint32_t width, uint32_t height,
                                   uint32_t pitchTexels, bool tiled) {
  if (!pm4FramesReady()) return;
  // iter-465 perf2: the FIRST PM4 frame renders synchronously; the render thread takes over
  // from the second present (SR_PM4_RT_AFTER=<n>: after the n-th, default 1; 0 = the first).
  // Measured (archive/v1_perf2, boot-intro contact sheets at a 40-present stride): with the
  // render thread rendering the very first frame - the set that carries every command since
  // boot (56 batches, 16 new render targets, a 424 ms drain) - every boot-intro frame came
  // out flat brown until the 3D front end, whatever the shm/window/readback/array gates said;
  // started at present 2, 31, 401 or 1201 the whole intro renders correctly (r_t_after1,
  // r_t_after30, r_t_after400, r_t_after1200). Which first-frame state the render thread
  // mishandles is still open; this keeps that frame on the path that handles it.
  static const uint32_t s_rtAfter = [] {
    const char* e = std::getenv("SR_PM4_RT_AFTER");
    return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 1u;
  }();
  static uint32_t s_presents = 0;
  ++s_presents;
  if (!m_rtOn && s_presents > s_rtAfter && pm4RenderThreadGate()) pm4RenderThreadStart();
  drainDrawQueue();  // the set's last drain (the swap's own indirect buffer)
#if SR_ORACLE
  // mem.resources frozen-pass A/B (gfx/resource_ab.hpp EMPTY; Astra review1 P2-6): a present no drain began a set for
  // (no new command since the last present) is a presenting set of its own - it takes the next arm.
  if (memres::resourceAbDual()) m_resArmSet = memres::resourceAbPresentingSet();
#endif
  if (m_rtOn) {
    // iter-461: the render thread takes the set; this thread goes back to the ring.
    pm4SubmitFrame(fbPhys, width, height, pitchTexels, tiled);
    memres::resourceAbPresented();  // the frozen-pass A/B: the next presenting set takes the other arm
    return;
  }
  m_rtDrainNsShown = 0;
  pm4RenderFrame(m_drawBatches, m_shmPending, m_xlatThird, fbPhys, width, height, pitchTexels,
                 tiled, false, zpd::presentingStamp());
  memres::resourceAbPresented();
}

// ── iter-461 render thread ──────────────────────────────────────────────────
// SR_PM4_RENDER_THREAD (default ON since iter-465 perf2; =0 = the synchronous path): the live game
// with the RT cache, and no live trace (the trace writer serializes the
// engine and the renderer on one thread).
bool D3D12Backend::pm4RenderThreadGate() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_RENDER_THREAD");
    // iter-465 perf2: default ON (=0 keeps the synchronous A/B path).
    const bool on = !e || e[0] != '0';
    return on && pm4LiveEnabled() && pm4RtCacheEnabled() && !livetrace::armed();
  }();
  return v;
}

void D3D12Backend::pm4RenderThreadStart() {
  if (m_rtOn) return;
  if (pm4ShmEnabled()) pm4ShmInit();  // created on this thread, before the render thread exists
  m_rtStop = false;
  m_rtOn = true;  // (before the thread starts: it reads the flag)
  // The guest-memory mapping is per thread: hand this thread's to the new one.
  uint8_t* base = sr::kernel::getGuestBase();
  const size_t size = sr::kernel::getGuestSize();
  m_rtThread = std::thread([this, base, size] { pm4RenderThreadMain(base, size); });
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_RT] render thread started (SR_PM4_RENDER_THREAD)");
}

void D3D12Backend::pm4RenderThreadStop() {
  {
    std::lock_guard<std::mutex> lk(m_rtMu);
    m_rtStop = true;
  }
  m_rtCv.notify_all();
  if (m_rtThread.joinable()) m_rtThread.join();
  m_rtOn = false;
}

// step 6 I5 (SR_BATCH_KEEP, default ON; =0 the old one-slot recycle): see d3d12_backend.hpp m_rtBatchPool.
bool D3D12Backend::batchKeepOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_BATCH_KEEP");
    return !e || e[0] != '0';
  }();
  return v;
}

// Under m_rtMu (CP): `out` (the moved-from queue slot's vector) becomes the pool's largest vector; `out`'s own
// storage, if any, goes back to the pool.
void D3D12Backend::batchPoolTakeLocked(std::vector<DrawBatch>& out) {
  if (m_rtBatchPool.empty()) return;
  size_t best = 0;
  for (size_t i = 1; i < m_rtBatchPool.size(); ++i)
    if (m_rtBatchPool[i].capacity() > m_rtBatchPool[best].capacity()) best = i;
  out.swap(m_rtBatchPool[best]);
  ++m_batchKeep.poolTakes;
  if (m_rtBatchPool[best].capacity() == 0) {   // the slot now holds out's old (empty) vector: remove it
    m_rtBatchPool[best].swap(m_rtBatchPool.back());
    m_rtBatchPool.pop_back();
  }
}

// Under m_rtMu: a cleared vector back to the pool. The pool is bounded (kBatchPool): when full, the smallest of the
// pool and `in` is the one dropped (freed with `in`'s owner), so the pool only ever keeps the largest vectors.
void D3D12Backend::batchPoolReturnLocked(std::vector<DrawBatch>& in) {
  in.clear();
  if (in.capacity() == 0) return;
  ++m_batchKeep.returns;
  if (m_rtBatchPool.capacity() < kBatchPool) m_rtBatchPool.reserve(kBatchPool);   // once (the pool's own storage)
  if (m_rtBatchPool.size() < kBatchPool) {
    m_rtBatchPool.push_back(std::move(in));
    return;
  }
  size_t smallest = 0;
  for (size_t i = 1; i < m_rtBatchPool.size(); ++i)
    if (m_rtBatchPool[i].capacity() < m_rtBatchPool[smallest].capacity()) smallest = i;
  if (in.capacity() > m_rtBatchPool[smallest].capacity()) in.swap(m_rtBatchPool[smallest]);
  ++m_batchKeep.drops;   // `in` now holds the smallest: freed by its owner
}

// CP: the frame set goes to the render thread. Blocks only while the render
// thread has not TAKEN the previous frame yet (frames in flight: one on the
// GPU, one recording, this one).
void D3D12Backend::pm4SubmitFrame(uint32_t fbPhys, uint32_t width, uint32_t height,
                                  uint32_t pitchTexels, bool tiled, uint64_t* zpdSeqOut) {
  const auto t0 = std::chrono::steady_clock::now();
  // iter-465 perf2: the set's texture prepass runs HERE, on the CP, with its copies queued
  // for the render thread to record before the first batch. Run on the render thread it
  // shared the page stamps / hashes with the CP's concurrent drains of the NEXT set (its
  // collect check compares with the CP's live drain epoch), so pages the CPU rewrote
  // between frames - the boot movies' planes - were not re-validated: every boot frame
  // presented flat brown (archive/v1_perf2/sheet_movie_b4rt_first60.png).
#if SR_ORACLE
  std::vector<ShmPend> prepass;
  if (m_shmReady) {
    std::lock_guard<std::recursive_mutex> slk(m_shmMu);
    std::vector<ShmPend> setPending;
    setPending.swap(m_shmPending);
    pm4ShmTexturePrepass(m_drawBatches, /*deferred=*/true);
    prepass.swap(m_shmPending);
    m_shmPending.swap(setPending);
  }
#endif
  {
    std::unique_lock<std::mutex> lk(m_rtMu);
    {
      sr::hitch::Scope cpw(sr::hitch::kCpSubmitWait);
      m_rtCv.wait(lk, [&] { return !m_rtHasFrame || m_rtStop; });
    }
    if (m_rtStop) return;
    Pm4Frame& f = m_rtQueued;
    f.batches.swap(m_drawBatches);
    f.pending.swap(m_shmPending);
    f.third = m_xlatThird;
    f.epochFirst = m_setEpochFirst;
#if SR_ORACLE
    f.prepass.swap(prepass);
    f.resArm = m_resArmSet;  // mem.resources frozen-pass A/B: the render thread decides as the set's arm
#endif
    f.drainNs = m_pm4DrainNs;
    f.fbPhys = fbPhys;
    f.width = width;
    f.height = height;
    f.pitchTexels = pitchTexels;
    f.tiled = tiled;
    f.zpdSeq = zpdSeqOut ? ++m_zpdPartialReq : 0u;  // p2.zpd: a partial set (no present)
    f.zpdStamp = zpdSeqOut ? zpd::FrameStamp{} : zpd::presentingStamp();
    if (zpdSeqOut) *zpdSeqOut = f.zpdSeq;
    m_rtHasFrame = true;
    // The recycled vectors keep their capacity for the next set.
    if (batchKeepOn()) {
      // I5: the pool's largest vector for the next set (m_drawBatches holds the moved-from queue slot's vector here)
      ++m_batchKeep.handoffs;
      m_batchHighWater = std::max(m_batchHighWater, f.batches.size());
      batchPoolTakeLocked(m_drawBatches);
      const auto& k = m_batchKeep;
      if (k.handoffs == 1 || k.handoffs == 100 || k.handoffs % 10000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[BATCH_KEEP] handoffs=%llu pool_takes=%llu reserves=%llu returns=%llu drops=%llu high_water=%zu "
                    "pool=%zu",
                    (unsigned long long)k.handoffs, (unsigned long long)k.poolTakes, (unsigned long long)k.reserves,
                    (unsigned long long)k.returns, (unsigned long long)k.drops, m_batchHighWater, m_rtBatchPool.size());
    } else {
      m_drawBatches.swap(m_rtFreeBatches);
    }
    m_shmPending.swap(m_rtFreePending);
  }
  m_rtCv.notify_all();
  m_rtWaitSubmitUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - t0).count());
  m_drawBatches.clear();
  m_shmPending.clear();
  if (batchKeepOn() && m_drawBatches.capacity() < m_batchHighWater) {
    // I5: no pooled vector large enough (the render thread still holds them): one allocation to the high-water
    // mark, outside the lock, instead of the set's regrowth chain
    m_drawBatches.reserve(m_batchHighWater + m_batchHighWater / 8);
    std::lock_guard<std::mutex> lk(m_rtMu);
    ++m_batchKeep.reserves;
  }
  m_pm4DrainNs = 0;
  m_pm4DrainOpen = false;  // the next drain starts the next set (rotates the thirds)
}

// iter-465 perf2: true on the render thread (its frames' texture prepass ran on the CP).
static thread_local bool t_pm4RenderThread = false;

void D3D12Backend::pm4RenderThreadMain(uint8_t* guestBase, size_t guestSize) {
  t_pm4RenderThread = true;
  sr::kernel::hostWorkMarkPresentingThread(/*cp=*/false);   // step 4 2k(4): host work here can hold a frame
  // iter-466 stutter: a name for the host profiler and debuggers (hitch windows watch it).
  SetThreadDescription(GetCurrentThread(), L"SR PM4 render");
  // iter-465 perf2: the frame list changes hands here (the CP rendered the first frame):
  // re-base the thread-affinity ledger on this thread, so a TID MISMATCH after this
  // point is a real cross-thread touch, not the handover.
  m_clTid = 0;
  // Guest memory for this host thread (texture and vertex reads).
  if (guestBase) sr::kernel::setGuestMemory(guestBase, guestSize);
  for (;;) {
    Pm4Frame f;
    {
      std::unique_lock<std::mutex> lk(m_rtMu);
      sr::hitch::Scope idle(sr::hitch::kRtIdle);
      m_rtCv.wait(lk, [&] { return m_rtHasFrame || m_rtStop; });
      if (m_rtStop) return;
      f = std::move(m_rtQueued);
#if SR_ORACLE
      memres::resourceAbEnter(f.resArm);
#endif
      m_rtHasFrame = false;
      m_rtBusy = true;
    }
    m_rtCv.notify_all();  // the CP may queue the next frame
    {
      std::lock_guard<std::recursive_mutex> lk(m_shmMu);
      m_shmRenderThird = f.third;
      m_shmEpochFrameStart = f.epochFirst;
    }
    m_rtDrainNsShown = f.drainNs;
#if SR_ORACLE
    m_rtFramePrepass.swap(f.prepass);
#endif
    sr::native::exec::profPlace(sr::native::exec::kPlRender);  // SR_NATIVE_EXEC_PROF (observe-only)
    pm4RenderFrame(f.batches, f.pending, f.third, f.fbPhys, f.width, f.height, f.pitchTexels,
                   f.tiled, f.zpdSeq != 0, f.zpdStamp);
    {
      std::lock_guard<std::mutex> lk(m_rtMu);
      f.batches.clear();
      f.pending.clear();
      if (batchKeepOn()) batchPoolReturnLocked(f.batches);   // I5: kept in the pool, never dropped while it has room
      else m_rtFreeBatches.swap(f.batches);
      m_rtFreePending.swap(f.pending);
      m_rtBusy = false;
      if (f.zpdSeq) m_zpdPartialDone = f.zpdSeq;  // p2.zpd: the CP waiting in pm4ZpdFlushPartial goes on
    }
    m_rtCv.notify_all();
  }
}

bool D3D12Backend::pm4RenderThreadIdle(uint32_t ms) {
  std::unique_lock<std::mutex> lk(m_rtMu);
  return m_rtCv.wait_for(lk, std::chrono::milliseconds(ms),
                         [&] { return (!m_rtHasFrame && !m_rtBusy) || m_rtStop; });
}

// ── p2.zpd: EVENT_WRITE_ZPD as host D3D12 occlusion queries (docs/NATIVE_ROADMAP.md P2.12) ─────────────────────
// The CP queues a marker per BEGIN / END (Pm4Engine::applyZpd); the drain makes it a DrawBatch at its stream
// position; here the list owner (render thread, or the CP when it renders) brackets every GUEST draw between the
// two markers with its own BeginQuery / EndQuery + ResolveQueryData (Astra round 1 P1: a query open from the BEGIN
// marker also counted the helper passes - pm4LoadTiles' transfer triangles, resolves, clears - so a fully occluded
// draw could answer non-zero), and once the submission that carries a segment has completed hands the counts to
// gfx/native_zpd, which writes the guest slot.

bool D3D12Backend::pm4FramesReady() {
#if defined(SR_DIFFTEST)
  if (m_zpdTest.on) return true;  // the seam: no device on the worker
#endif
  return m_initialized && pm4Init() && xlatReady();
}

void D3D12Backend::zpdDrainMarker(uint8_t op, uint32_t id) {
  DrawBatch zb{};
  zb.zpdOp = op;
  zb.zpdId = id;
  m_drawBatches.push_back(zb);
  if (op == zpd::kBegin) m_zpdDrainQuery = id;  // the draws drained from here to its END carry the probe
  else if (op == zpd::kEnd && m_zpdDrainQuery == id) m_zpdDrainQuery = 0;
  zpd::markerDrained(zpd::Op(op), id);  // a drained END makes the next EVENT_WRITE_SHD its covering-fence check
}

bool D3D12Backend::zpdEnsure() {
#if defined(SR_DIFFTEST)
  if (m_zpdTest.on) return true;
#endif
  if (m_zpdHeap && m_zpdReadbackPtr) return true;
  if (m_zpdInitFailed || !m_device) return false;
  D3D12_QUERY_HEAP_DESC qd{};
  qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  qd.Count = zpd::kHeapCapacity;
  HRESULT hr = m_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_zpdHeap));
  if (SUCCEEDED(hr)) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = uint64_t(zpd::kHeapCapacity) * sizeof(uint64_t);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&m_zpdReadback));
  }
  void* mapped = nullptr;
  if (SUCCEEDED(hr)) {
    const D3D12_RANGE all{0, size_t(zpd::kHeapCapacity) * sizeof(uint64_t)};
    hr = m_zpdReadback->Map(0, &all, &mapped);  // persistently mapped; read only after the segment's fence
  }
  if (FAILED(hr) || !mapped) {
    m_zpdInitFailed = true;
    m_zpdHeap.Reset();
    m_zpdReadback.Reset();
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NATIVE_ZPD] host occlusion query heap / readback FAILED hr=0x%08X - queries answer 'visible' (1)",
                unsigned(hr));
    return false;
  }
  m_zpdReadbackPtr = static_cast<const uint64_t*>(mapped);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_ZPD] host occlusion queries ready: %u-query heap + readback (mode=%s)",
              zpd::kHeapCapacity, zpd::modeName(zpd::mode()));
  return true;
}

void D3D12Backend::zpdRecord(bool begin, uint32_t seg) {
#if defined(SR_DIFFTEST)
  if (m_zpdTest.on) {
    std::lock_guard<std::mutex> lk(m_zpdTest.mu);
    m_zpdTest.rec.emplace_back(begin ? 'B' : 'E', seg);
    return;
  }
#endif
  if (begin) {
    m_commandList->BeginQuery(m_zpdHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, seg);
    return;
  }
  m_commandList->EndQuery(m_zpdHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, seg);
  m_commandList->ResolveQueryData(m_zpdHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, seg, 1, m_zpdReadback.Get(),
                                  uint64_t(seg) * sizeof(uint64_t));
}

void D3D12Backend::zpdMarker(const DrawBatch& b) {
  if (b.zpdOp == zpd::kBegin) {
    // The guest began again without an END: the old one was abandoned (gfx/native_zpd marked it dead).
    if (m_zpdOpenId) zpd::endRecorded(m_zpdOpenId, true);
    m_zpdOpenId = b.zpdId;  // the draws from here to its END are measured, each on its own (zpdDrawBegin)
  } else if (b.zpdOp == zpd::kEnd) {
    zpd::endRecorded(b.zpdId, m_zpdOpenId == b.zpdId);  // BEGIN unseen here: "visible"
    if (m_zpdOpenId == b.zpdId) m_zpdOpenId = 0;
  }
}

D3D12Backend::Pm4PsoDesc D3D12Backend::zpdProbeDesc(const Pm4PsoDesc& d, bool raster) {
  Pm4PsoDesc p = d;
  for (uint32_t i = 0; i < 4; i++) p.writeMask[i] = 0;  // no colour written (the RTV formats stay: they are bound)
  if (p.dsv != DXGI_FORMAT_UNKNOWN) {
    p.zEnable = true;  // tested ALWAYS: every rasterized sample passes and is counted
    p.zFunc = 7;
  }
  p.zWrite = false;
  p.stencil = false;
  p.stencilCtl = p.stencilMasks = 0;
  p.depthBias = 0;
  p.depthBiasSlope = 0.0f;
  p.noPixel = raster;
  return p;
}

uint32_t D3D12Backend::zpdProbeBegin(uint8_t kind) {
  if (!m_zpdOpenId || !zpdEnsure()) return zpd::kNoSeg;
  const uint32_t seg = zpd::segmentOpenProbe(m_zpdOpenId, zpd::Probe(kind));
  if (seg != zpd::kNoSeg) zpdRecord(true, seg);
  return seg;
}

void D3D12Backend::zpdLogDraw(const DrawBatch& b, const D3D12_RECT& sc, bool depthBound, bool probed, size_t bi) {
  if (m_zpdDrawLines >= kZpdDrawLines) return;
  m_zpdDrawLines++;
  // c19.xy of the shared block: the point sprite's NDC radius (d3d12_backend.cpp tryXlatDraw, point_sprite.gs.hlsl).
  float ndcR[2] = {0.0f, 0.0f};
  if (m_xlatCBPtr && b.sharedCbOff + 78u * 4u <= kXlatCBBytes) std::memcpy(ndcR, m_xlatCBPtr + b.sharedCbOff + 76u * 4u, 8);
  // The first vertex as decoded on the CPU (float4 per attribute; the in-shader-fetch path has none here).
  float v[8] = {};
  const bool haveV = !b.vsf && m_xlatVBPtr && b.xlatStride >= 16u && b.vertexOffset + 32u <= kXlatVBBytes;
  if (haveV) std::memcpy(v, m_xlatVBPtr + b.vertexOffset, b.xlatStride >= 32u ? 32u : 16u);
  const edram::DrawTargets& t = b.pm4Targets;
  const edram::Regs& r = b.pm4Regs;
  const auto asFloat = [](uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return double(f);
  };
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_ZPD] draw #%u id=%u topo=%u verts=%u vsf=%d vs=%016llX ps=%016llX vp=(%.1f,%.1f %.1fx%.1f "
              "z%.3f..%.3f) sc=(%ld,%ld)-(%ld,%ld) surf=%08X msaa=%u rt0=%08X/%X depth=%08X bound=%d z=%d/%d f%u "
              "stencil=%d dc=%08X sref=%08X colorCtl=%08X vte=%08X clip=%08X scmode=%08X pointSize=%08X "
              "ndcR=(%.5f,%.5f) v0=%s(%.3f,%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f,%.3f) vb=%08X guest=(%g,%g,%g %08X) "
              "probe=%d",
              m_zpdDrawLines, m_zpdOpenId, unsigned(b.topology), b.vertexCount, b.vsf ? 1 : 0,
              (unsigned long long)b.vsHash, (unsigned long long)b.psHash, b.xlatViewport.TopLeftX,
              b.xlatViewport.TopLeftY, b.xlatViewport.Width, b.xlatViewport.Height, b.xlatViewport.MinDepth,
              b.xlatViewport.MaxDepth, long(sc.left), long(sc.top), long(sc.right), long(sc.bottom), r.surfaceInfo,
              edram::surfaceMsaa(r), t.colorWriteMask[0] ? t.color[0].v : 0u, unsigned(t.colorWriteMask[0]),
              t.depthBound ? t.depth.v : 0u, depthBound ? 1 : 0, t.zEnable ? 1 : 0, t.zWrite ? 1 : 0,
              unsigned(t.zFunc), t.stencilEnable ? 1 : 0, r.depthControl, r.stencilRefMask, b.zpdColorCtl, r.vteCntl,
              r.clipCntl, r.suScModeCntl, b.zpdPointSize, ndcR[0], ndcR[1], haveV ? "" : "-", v[0], v[1], v[2], v[3],
              v[4], v[5], v[6], v[7], b.zpdVtxAddr, asFloat(b.zpdVtx[0]), asFloat(b.zpdVtx[1]), asFloat(b.zpdVtx[2]),
              b.zpdVtx[3], probed ? 1 : 0);
  // Where its depth came from: the claim's moves into the depth it tests (src key [tiles), " none" = that surface
  // already held them) and the frame's last whole-depth rewrite (an XDK clear) before it - zpd5's depthKilled split
  // between "tested against the scene" (no clear since the scene's depth) and "tested after the HUD's depth clear".
  char clr[96];
  if (m_zpdClearEpoch == m_frameEpoch)
    std::snprintf(clr, sizeof clr, "%08X %llu draws before (uniform %s)", m_zpdClearKey,
                  (unsigned long long)(m_zpdDrawSerial - m_zpdClearSerial), m_zpdClearZero ? "zero" : "no");
  else
    std::snprintf(clr, sizeof clr, "none this frame");
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_ZPD] draw #%u depth frame=%llu batch=%zu moves:%s lastDepthClear=%s",
              m_zpdDrawLines, (unsigned long long)m_frameEpoch, bi, m_zpdDepthMoves[0] ? m_zpdDepthMoves : " -", clr);
  m_zpdDepthMoves[0] = 0;
  // The first few: the vertex shader's float constants c0..c7 as staged (a world-to-clip transform reads the point
  // through them; the line lets the clip position be computed offline).
  // step 6 I10p: a view batch's VS constants live only in the write-only twin - this dump is refused there (said once).
  if (m_zpdDrawLines <= 4 && b.zcView) {
    static std::atomic<bool> s_zcRefused{false};
    if (!s_zcRefused.exchange(true))
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_ZPD] vs consts dump REFUSED: SR_XLAT_CB_ZEROCOPY (the VS block is a "
                                         "view into the write-only upload twin; never read back)");
  } else if (m_zpdDrawLines <= 4 && m_xlatCBPtr && b.xlatVsBytes >= 128u && b.xlatVsCbOff + 128u <= kXlatCBBytes) {
    float c[32];
    std::memcpy(c, m_xlatCBPtr + b.xlatVsCbOff, sizeof c);
    char txt[1024];
    int n = 0;
    for (int i = 0; i < 8 && n < int(sizeof txt) - 64; i++)
      n += std::snprintf(txt + n, sizeof txt - n, " c%d=(%g,%g,%g,%g)", i, c[4 * i], c[4 * i + 1], c[4 * i + 2],
                         c[4 * i + 3]);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_ZPD] draw #%u vs consts%s", m_zpdDrawLines, txt);
  }
}

uint32_t D3D12Backend::zpdDrawBegin(const DrawBatch& b) {
  if (!m_zpdOpenId) return zpd::kNoSeg;
  if (!zpdEnsure()) {
    zpd::segmentMissed(m_zpdOpenId);  // no heap: the query answers "visible"
    return zpd::kNoSeg;
  }
  // The console counts samples: the target's MSAA (RB_SURFACE_INFO bits 16-17: 1X / 2X / 4X) is the guest samples
  // per pixel; the host target is single-sampled at the guest pixel size times the resolution scale (Astra round 1
  // P2), so a host sample stands for samplesPerPixel / area guest samples (gfx/native_zpd normalize).
  const uint32_t msaa = std::min(edram::surfaceMsaa(b.pm4Regs), uint32_t(edram::kMsaa4X));
  const uint32_t seg = zpd::segmentOpen(m_zpdOpenId, edram::msaaSampleCount(msaa));
  if (seg != zpd::kNoSeg) zpdRecord(true, seg);
  return seg;
}

void D3D12Backend::zpdDrawEnd(uint32_t seg) {
  if (seg == zpd::kNoSeg) return;
  zpdRecord(false, seg);
  zpd::segmentClosed(m_zpdOpenId, seg);
}

void D3D12Backend::zpdCollectNow(bool gpuIdle) {
  // Segments closed so far ran on lists executed before the fence just signaled (or, gpuIdle, before the GPU went
  // idle); the queries whose every segment completed get their counts.
  zpd::stamp(gpuIdle ? 0u : m_lastSignaledFence);
  const uint64_t completed = gpuIdle ? UINT64_MAX : (m_fence ? m_fence->GetCompletedValue() : 0u);
  zpd::collect(completed, m_zpdReadbackPtr, sr::gfx::resscale::get().area(), gpuIdle);
}

void D3D12Backend::pm4ZpdPartialFrame(std::vector<DrawBatch>& batches, std::vector<ShmPend>& pending,
                                      uint32_t third) {
  // The frame list's own steps without the scan-out / present / swapchain fence and without the frame-epoch bump
  // (the rest of this guest frame - e.g. the front-buffer record the scan-out matches by epoch - is the same frame).
  clReset(FrameOpen);
  gtFrameStart();
  renderPm4Batches(batches, pending);
  gtFrameEnd();
  clClose(Pm4Close);
  clExecute(Pm4Exec);
  if (m_dbgActive) drainDebugLayer();
  waitForGpu();
  checkDeviceRemoved("zpdPartialFrame");
  m_thirdFence[third].store(0, std::memory_order_release);  // the GPU is idle: the set's third is free
  zpdCollectNow(true);
}

bool D3D12Backend::pm4ZpdFlushPartial() {
  if (!pm4FramesReady()) return false;
  if (m_rtOn) {
    uint64_t seq = 0;
    pm4SubmitFrame(0, 0, 0, 0, false, &seq);  // the render thread takes the set (consumed here)
    if (!seq) return false;                   // the render thread is stopping
    std::unique_lock<std::mutex> lk(m_rtMu);
    const bool done = m_rtCv.wait_for(lk, std::chrono::milliseconds(m_zpdFlushTimeoutMs),
                                      [&] { return m_zpdPartialDone >= seq || m_rtStop; });
    return done && !m_rtStop;
  }
  // Synchronous path (the first frame, SR_PM4_RENDER_THREAD=0): this thread owns the list. No set open since the
  // last render: nothing to render, only the GPU to drain.
  if (!m_pm4DrainOpen) {
    waitForGpu();
    checkDeviceRemoved("zpdPartialIdle");
    zpdCollectNow(true);
    return true;
  }
  // Astra round 1 P1: the partial CONSUMES the set, as the render thread's handoff does. Rendered in place it stayed
  // in m_drawBatches, and a swap with no draw after the fence (drainDrawQueue keeps the batches when the queue is
  // empty) rendered it again at present - repeated blends and resolves.
  std::vector<DrawBatch> set;
  set.swap(m_drawBatches);
  pm4RenderFrame(set, m_shmPending, m_xlatThird, 0, 0, 0, 0, false, true);
  m_totalGeoVerts = 0;
  set.clear();
  if (m_drawBatches.empty()) {
    m_drawBatches.swap(set);  // the vector's capacity for the next set
  } else if (batchKeepOn() && set.capacity() > m_drawBatches.capacity()) {
    // I5 (the synchronous path: no render thread draws from the pool): keep the LARGER vector - the batches recorded
    // meanwhile move into it, in order (a move, as any regrowth of m_drawBatches would make)
    set.insert(set.end(), std::make_move_iterator(m_drawBatches.begin()), std::make_move_iterator(m_drawBatches.end()));
    m_drawBatches.swap(set);
  }
  return true;
}

#if defined(SR_DIFFTEST)
// ── p2.zpd difftest seam (d3d12_backend.hpp ZpdTestSeam) ──────────────────────────────────────────────────────────
void D3D12Backend::zpdTestEnable() {
  m_zpdTest.on = true;
  m_zpdTest.readback.assign(zpd::kHeapCapacity, 0u);
  m_zpdReadbackPtr = m_zpdTest.readback.data();
}

void D3D12Backend::zpdTestPushDraw(uint32_t msaa, bool helper, bool probe) {
  DrawBatch b{};
  b.pm4 = true;
  b.vertexCount = 3;
  b.resolve = helper;  // a resolve: helper work the render loop does between draws
  b.pm4Regs.surfaceInfo = (msaa & 3u) << 16;
  if (probe && !helper && zpd::probeOn()) {  // stand-ins: the seam never binds them (pm4PrepareDraw's real ones)
    b.zpdRefPso = reinterpret_cast<ID3D12PipelineState*>(uintptr_t(0x10));
    b.zpdRasterPso = reinterpret_cast<ID3D12PipelineState*>(uintptr_t(0x20));
  }
  m_drawBatches.push_back(b);
  m_pm4DrainOpen = true;  // what the drain that queued it leaves
}

void D3D12Backend::zpdTestRenderThread(bool on) {
  if (on) {
    if (m_rtOn) return;
    m_rtStop = false;
    m_rtOn = true;
    uint8_t* base = sr::kernel::getGuestBase();
    const size_t size = sr::kernel::getGuestSize();
    m_rtThread = std::thread([this, base, size] { pm4RenderThreadMain(base, size); });
    return;
  }
  pm4RenderThreadStop();
}

// pm4RenderFrame's body under the seam: the render loop's query calls in batch order (renderPm4Batches: markers at
// zpdMarker; zpdDrawBegin / zpdDrawEnd immediately around each guest draw call; helper batches outside any query),
// the GPU time, then the frame's collect (partial: pm4ZpdPartialFrame's waitForGpu + zpdCollectNow(true)).
void D3D12Backend::zpdTestFrame(std::vector<DrawBatch>& batches, bool partial) {
  ZpdTestFrame f;
  f.partial = partial;
  f.stamp = zpd::presentingStamp();
  f.threadId = GetCurrentThreadId();
  f.batches = uint32_t(batches.size());
  for (const DrawBatch& b : batches) {
    if (b.zpdOp) {
      f.markers++;
      zpdMarker(b);
      continue;
    }
    if (b.resolve) {
      f.helpers++;
      std::lock_guard<std::mutex> lk(m_zpdTest.mu);
      m_zpdTest.rec.emplace_back('H', 0u);
      continue;
    }
    f.draws++;
    const uint32_t seg = zpdDrawBegin(b);
    if (seg != zpd::kNoSeg && seg < m_zpdTest.readback.size()) m_zpdTest.readback[seg] = m_zpdTest.samplesPerDraw;
    if (seg == zpd::kNoSeg) continue;
    zpdDrawEnd(seg);
    // The render loop's probe draws (renderPm4Batches): after the measured draw, each on a segment of its own.
    const std::pair<ID3D12PipelineState*, uint8_t> probes[2] = {{b.zpdRefPso, zpd::kRef},
                                                                {b.zpdRasterPso, zpd::kRaster}};
    for (const auto& [probePso, kind] : probes) {
      if (!probePso) continue;
      const uint32_t ps = zpdProbeBegin(kind);
      if (ps == zpd::kNoSeg) continue;
      {
        std::lock_guard<std::mutex> lk(m_zpdTest.mu);
        m_zpdTest.rec.emplace_back(kind == zpd::kRef ? 'r' : 'R', ps);
      }
      if (ps < m_zpdTest.readback.size())
        m_zpdTest.readback[ps] = kind == zpd::kRef ? m_zpdTest.refSamples : m_zpdTest.rasterSamples;
      zpdDrawEnd(ps);
    }
  }
  if (m_zpdTest.delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(m_zpdTest.delayMs));
  zpdCollectNow(true);
  {
    std::lock_guard<std::mutex> lk(m_zpdTest.mu);
    m_zpdTest.frames.push_back(f);
  }
  if (m_zpdTest.onFrame) m_zpdTest.onFrame(m_zpdTest.user, partial);
  if (!m_rtOn) m_pm4DrainOpen = false;  // renderPm4Batches' end: the next drain starts the next set
}
#endif

// CP, before a set drains into `third`: the frame that used it last has
// completed on the GPU (its present's fence).
void D3D12Backend::pm4WaitThirdFree(uint32_t third) {
#if SR_ORACLE
  m_zcWaitedThird = third;   // step 6 I10p check mode: the deterministic fence assert's record (zcBeginThird)
  ++m_zcWaits;
#endif
  const uint64_t need = m_thirdFence[third].load(std::memory_order_acquire);
  if (!need || !m_fence) return;
  if (m_fence->GetCompletedValue() >= need) return;
  const auto t0 = std::chrono::steady_clock::now();
  if (!m_cpFenceEvent) m_cpFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (m_cpFenceEvent && SUCCEEDED(m_fence->SetEventOnCompletion(need, m_cpFenceEvent))) {
    WaitForSingleObject(m_cpFenceEvent, INFINITE);
  } else {
    while (m_fence->GetCompletedValue() < need) Sleep(0);
  }
  m_rtWaitThirdUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - t0).count());
}

// The swap body: open the frame list, render the set, scan the front buffer
// out, present, wait for the frame before last, notify the guest.
void D3D12Backend::pm4RenderFrame(std::vector<DrawBatch>& batches, std::vector<ShmPend>& pending,
                                  uint32_t third, uint32_t fbPhys, uint32_t width,
                                  uint32_t height, uint32_t pitchTexels, bool tiled, bool zpdPartial,
                                  const zpd::FrameStamp& stamp) {
  // A swap may only enqueue here: the freeze dump runs on the render thread, where
  // the submitting thread's TLS is absent. Keep the stamp with the queued frame;
  // partial sets carry no present stamp. Restore TLS on every return path.
  struct RestoreStamp {
    zpd::FrameStamp previous;
    ~RestoreStamp() { zpd::setPresentingStamp(previous); }
  } restoreStamp{zpd::presentingStamp()};
  zpd::setPresentingStamp(stamp);
#if defined(SR_DIFFTEST)
  if (m_zpdTest.on) {  // p2.zpd difftest seam: the query side of the frame, no device
    zpdTestFrame(batches, zpdPartial);
    return;
  }
#endif
  if (zpdPartial) {  // p2.zpd: the strict covering fence's partial set (no scan-out / present)
    pm4ZpdPartialFrame(batches, pending, third);
    return;
  }
  // Per-phase wall time of the swap (ms), reported every 120 presents.
  using PClock = std::chrono::steady_clock;
  auto pms = [](PClock::time_point a, PClock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto p0 = PClock::now();
  applyPendingResize();  // iter-464: a resized window's swapchain, before this frame's list
  m_frameEpoch++;
  clReset(FrameOpen);
  gtFrameStart();  // iter-464 SR_PM4_GPU_TIME (no-op when off)
  renderPm4Batches(batches, pending);  // resolves land in guest memory before it returns
  const auto p1 = PClock::now();

  auto* rt = m_renderTargets[m_frameIndex].Get();
  // iter-460: scan-out straight from the shared memory - the resolved front
  // buffer never returns to guest RAM (xenia IssueSwap).
  const bool shmPresented =
      pm4ShmEnabled() && pm4ShmPresent(fbPhys, width, height, pitchTexels, tiled, rt);
  if (!shmPresented) {
  // Scan-out on the CPU: the resolved front buffer is in guest memory.
  const uint32_t cw = std::min<uint32_t>(width, uint32_t(m_width));
  const uint32_t ch = std::min<uint32_t>(height, uint32_t(m_height));
  const uint32_t upPitch = (uint32_t(m_width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
  if (pitchTexels < width) pitchTexels = width;
  size_t avail = 0;
  int fbWin = 8;
  const uint8_t* fb = pm4GuestWritableWin(fbPhys, avail, fbWin);  // iter-460: the owning window
  const size_t span = tiled ? xenos_tiling::tiledSpanBytes32((pitchTexels + 31u) & ~31u, height)
                            : size_t(pitchTexels) * height * 4u;
  // iter-460: the resolved front buffer lives in the shared memory.
  std::vector<uint8_t> nativeFront;
  if (pm4ShmEnabled()) {
    if (memres::resourceDecides()) {
      resourceCpuSnapshot(fbPhys, uint32_t(span), nativeFront);
      fb = nativeFront.data();
      avail = nativeFront.size();
#if SR_ORACLE
    } else {
      pm4ShmSyncToGuest(fbPhys, uint32_t(span));
#endif
    }
  }
  void* mapped = nullptr;
  const D3D12_RANGE noRead{0, 0};
  if (SUCCEEDED(m_uploadBuffer->Map(0, &noRead, &mapped)) && mapped) {
    uint8_t* dst = static_cast<uint8_t*>(mapped);
    const auto& ramp = sr::gfx::gamma::displayRamp();
    if (fb && span <= avail) {
      const uint32_t tp = (pitchTexels + 31u) & ~31u;
      par::parallelFor(0u, ch, [&](uint32_t y) {
        uint8_t* d = dst + size_t(y) * upPitch;
        for (uint32_t x = 0; x < cw; x++) {
          const uint8_t* s = tiled ? fb + xenos_tiling::tiled2D(int32_t(x), int32_t(y), tp, 2)
                                   : fb + (size_t(y) * pitchTexels + x) * 4u;
          // Guest bytes are B, G, R, A (the replay's BMP order); the
          // backbuffer is R8G8B8A8. 8_8_8_8 front buffer -> 256-entry table.
          d[x * 4 + 0] = ramp.to8(ramp.table10(s[2], 0));
          d[x * 4 + 1] = ramp.to8(ramp.table10(s[1], 1));
          d[x * 4 + 2] = ramp.to8(ramp.table10(s[0], 2));
          d[x * 4 + 3] = 0xFF;
        }
      });
    } else {
      for (uint32_t y = 0; y < uint32_t(m_height); y++) std::memset(dst + size_t(y) * upPitch, 0, upPitch);
      static std::atomic<uint32_t> s_miss{0};
      if (s_miss.fetch_add(1) < 8)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[PM4_LIVE] front buffer 0x%08X %ux%u pitch=%u tiled=%d not readable (avail=%zu span=%zu)",
                    fbPhys, width, height, pitchTexels, tiled ? 1 : 0, avail, span);
    }
    m_uploadBuffer->Unmap(0, nullptr);
  }
  pm4Barrier(m_commandList.Get(), rt, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION srcLoc{}, dstLoc{};
  srcLoc.pResource = m_uploadBuffer.Get();
  srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srcLoc.PlacedFootprint.Footprint.Width = UINT(m_width);
  srcLoc.PlacedFootprint.Footprint.Height = UINT(m_height);
  srcLoc.PlacedFootprint.Footprint.Depth = 1;
  srcLoc.PlacedFootprint.Footprint.RowPitch = upPitch;
  dstLoc.pResource = rt;
  dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  // iter-464: the backbuffer may be smaller than the 1280x720 upload (a resized
  // window) - copy what fits (this fallback does not fit-scale).
  const D3D12_RESOURCE_DESC bbd = rt->GetDesc();
  const D3D12_BOX box{0, 0, 0, UINT(std::min<UINT64>(UINT64(m_width), bbd.Width)),
                      std::min<UINT>(UINT(m_height), bbd.Height), 1};
  m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
  pm4Barrier(m_commandList.Get(), rt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  }  // CPU scan-out
  const auto p2 = PClock::now();
  gtFrameEnd();  // iter-464 SR_PM4_GPU_TIME (no-op when off)
  clClose(Pm4Close);
  clExecute(Pm4Exec);
  if (m_dbgActive) drainDebugLayer();
  dumpThenOverlay(rt);   // iter-464: the dump, then the host overlay, then Present
  const auto p3 = PClock::now();
  presentSwap();
  checkDeviceRemoved("pm4PresentFrame");
  const auto p4 = PClock::now();
  moveToNextFrame();
  // iter-461: the set's arena third is free once this frame's fence completes.
  m_thirdFence[third].store(m_lastSignaledFence, std::memory_order_release);
  if (sr::gfx::zpd::on()) zpdCollectNow(false);  // p2.zpd: this frame's query segments + the completed queries
  const auto p5 = PClock::now();
  // iter-460 S1: only textures on pages written since the last present re-validate
  // (Windows write watch on the arena); without it, all of them.
  if (memres::resourceMode()) {
    invalidateTexturesAtPresent();
#if SR_ORACLE
  } else if (sr::kernel::guestWriteWatchActive()) {
    const auto tw = std::chrono::steady_clock::now();
    static std::vector<uint32_t> pages;
    size_t n = 0;
    for (uint32_t win : {0x80000000u, 0xA0000000u}) {
      pages.clear();
      n += sr::kernel::guestCollectWrittenPages(win, 0x20000000u, pages);
      for (uint32_t va : pages) invalidateGuestRange(va & 0x1FFFFFFFu, 4096);
    }
    static std::atomic<uint32_t> s_wwLog{0};
    const uint32_t wl = s_wwLog.fetch_add(1) + 1;
    if (wl <= 8 || wl % 600 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_WW] present #%u: %zu written pages -> texture invalidation (%.2f ms)",
                  wl, n, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tw).count());
  } else if (texCleanTrackedOn()) {
    invalidateTexturesAtPresent();  // iter-466 cp lane: SR_SHM_WT_TEXCLEAN
  } else {
    invalidateAllTextures();
#endif
  }
  const auto p6 = PClock::now();
  static double s_acc[6] = {};
  static uint32_t s_n = 0;
  const double ph[6] = {pms(p0, p1), pms(p1, p2), pms(p2, p3), pms(p3, p4), pms(p4, p5),
                        pms(p5, p6)};
  for (int i = 0; i < 6; i++) s_acc[i] += ph[i];
  if (++s_n == 120) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_SWAP_TIME] avg over 120: render=%.1f scanout=%.1f submit=%.1f present=%.1f "
                "frameWait=%.1f ww=%.1f (ms)%s | CP stalls over 120: thirdWait=%.1f submitWait=%.1f ms",
                s_acc[0] / 120, s_acc[1] / 120, s_acc[2] / 120, s_acc[3] / 120, s_acc[4] / 120,
                s_acc[5] / 120, m_rtOn ? " [render thread]" : "",
                double(m_rtWaitThirdUs.exchange(0)) * 1e-3,
                double(m_rtWaitSubmitUs.exchange(0)) * 1e-3);
    s_n = 0;
    for (double& a : s_acc) a = 0;
  }
}

extern double g_bindMs2D, g_bindMs3D, g_bindMsArr;
extern uint32_t g_bindN2D, g_bindN3D, g_bindNArr;

void D3D12Backend::renderPm4Batches(std::vector<DrawBatch>& batches, std::vector<ShmPend>& pending) {
  if (!pm4Init() || !xlatReady()) {
#if SR_ORACLE
    for (const auto& batch : batches)
      if (batch.resourceFetchCapture)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[MEM_RES_FETCH] unconfirmed=%llu reason=frame_unavailable epoch_coverage=unknown",
            (unsigned long long)batch.resourceFetchId);
#endif
    // M6 (gfx/pm4_frame_gate.hpp): a frame with draws or pending copies stops through the native failure
    // diagnostic instead of vanishing while the caller presents.
    const char* stage = pm4FrameUnavailableStage(m_pm4Ready, xlatReady(), batches.size(), pending.size());
    if (stage && pm4FrameFailClosedOn()) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[PM4_FRAME] cannot render: pm4 renderer %s, translated pipeline %s, shader cache %s; %zu batches, "
                  "%zu pending copies would be dropped",
                  m_pm4Ready ? "ready" : "NOT READY", m_xlatReady ? "ready" : "NOT CREATED",
                  getShaderCache().installedPresent() ? "installed" : "NOT INSTALLED", batches.size(), pending.size());
      nativeRecordingFail(stage, E_FAIL);
    }
    return;
  }
  if (!m_rtOn && m_shmReady) {
    // The render accepts this set's validations (the render thread sets it
    // from the frame it took).
    std::lock_guard<std::recursive_mutex> lk(m_shmMu);
    m_shmEpochFrameStart = m_setEpochFirst;
  }
#if SR_ORACLE
  pm4ShmBeginFrame();
  if (t_pm4RenderThread) {  // iter-465 perf2: the CP validated them at submit; record its copies first
    pm4ShmRecordPending(m_rtFramePrepass, 0, uint32_t(m_rtFramePrepass.size()));
    m_rtFramePrepass.clear();
  } else {
    pm4ShmTexturePrepass(batches);  // iter-461: the frame's textures, one parallel validation
  }
#endif
  {
    // step 6 R6 fix round 2 (Astra r2 P1): the drain counters are the draining thread's (single-writer, never reset):
    // this block reports their delta since its previous pass; the bind / resolve counters stay this thread's own.
    struct DrainSnap {
      uint32_t calls, n, noIb, ib, rect, clip, other;
      double prep, fetch, ibMs, decode;
    };
    const auto rd = [](const auto& a) { return a.load(std::memory_order_relaxed); };
    const DrainSnap now{rd(g_drainCalls), rd(g_drainN), rd(g_drainNoIb), rd(g_drainIb), rd(g_drainDecRect),
                        rd(g_drainDecClip), rd(g_drainDecOther), rd(g_drainMsPrep), rd(g_drainMsVsfFetch),
                        rd(g_drainMsVsfIb), rd(g_drainMsDecode)};
    static DrainSnap s_last{};
    const DrainSnap d{now.calls - s_last.calls, now.n - s_last.n, now.noIb - s_last.noIb, now.ib - s_last.ib,
                      now.rect - s_last.rect, now.clip - s_last.clip, now.other - s_last.other,
                      now.prep - s_last.prep, now.fetch - s_last.fetch, now.ibMs - s_last.ibMs,
                      now.decode - s_last.decode};
    s_last = now;
    static uint32_t s_bf = 0;
    if (++s_bf <= 8 || s_bf % 100 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_BIND_CENSUS] last frame: 2D %u binds %.1f ms | 3D %u binds %.1f ms | array %u binds %.1f ms"
                  " | drain: %u calls %u draws prep %.1f vsfFetch %.1f vsfIb %.1f decode %.1f ms"
                  " (vsf noIb %u ib %u | decode rect %u clip %u other %u)"
                  " | resolve: store %.1f shm %.1f inval %.1f clear %.1f cpu %.1f ms",
                  g_bindN2D, g_bindMs2D, g_bindN3D, g_bindMs3D, g_bindNArr, g_bindMsArr,
                  d.calls, d.n, d.prep, d.fetch, d.ibMs, d.decode,
                  d.noIb, d.ib, d.rect, d.clip, d.other,
                  g_resMsStore, g_resMsShm, g_resMsInval, g_resMsClear, g_resMsCpu);
    g_bindMs2D = g_bindMs3D = g_bindMsArr = 0;
    g_bindN2D = g_bindN3D = g_bindNArr = 0;
    g_resMsStore = g_resMsShm = g_resMsInval = g_resMsClear = g_resMsCpu = 0;
  }
  const auto frameT0 = std::chrono::steady_clock::now();
  // Per-phase cost of this frame (reported in [PM4_FRAME]).
  using Clock = std::chrono::steady_clock;
  double msXfer = 0, msBind = 0, msResolve = 0;
  // iter-466 hfr: the per-batch phase timers (bind / resolve / transfer, ~3,600 QPC reads per
  // gameplay frame on the render thread) only feed [PM4_FRAME]: SR_PM4_DIAG_TIMING=1 keeps them.
  auto since = [](Clock::time_point t) {
    return g_pm4DiagTiming ? std::chrono::duration<double, std::milli>(Clock::now() - t).count() : 0.0;
  };
  {
    ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
    m_commandList->SetDescriptorHeaps(2, heaps);
  }
  bool rootBound = false;
  const Pm4DumpCfg& dcfg = pm4DumpCfg();
  if (m_dredOn) {
    CrumbFrame& cf = m_crumbHist[m_frameEpoch & 3u];
    cf.frame = uint32_t(m_frameEpoch);
    cf.b.clear();
    for (const DrawBatch& b : batches)
      cf.b.push_back(CrumbBatch{b.vsHash, b.psHash, b.vertexCount, unsigned(b.topology),
                                b.vertexOffset, uint32_t(b.vsfIbOffset), b.vsf,
                                b.topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST});
  }
  uint32_t shmPendCursor = 0;
  for (size_t bi = 0; bi < batches.size(); bi++) {
#if SR_ORACLE
    // SR_PM4_EDRAM_DUMP (the oracle unit): the whole EDRAM mirror after the named draws; its store flushed the list.
    if (bi && edramDumpAfterDraw(batches[bi - 1])) rootBound = false;
#endif
    DrawBatch& b = batches[bi];
#if SR_ORACLE
    struct FetchObservation {
      const DrawBatch& batch;
      bool recorded = false;
      ~FetchObservation() {
        if (batch.resourceFetchCapture && !recorded)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[MEM_RES_FETCH] unconfirmed=%llu reason=draw_skipped_or_aborted epoch_coverage=unknown",
              (unsigned long long)batch.resourceFetchId);
      }
    } fetchObservation{b};
#endif
    livetrace::setDrawContext(b.drawOrdinal);
    // The shared-memory uploads queued up to this batch's drain, in draw order
    // (a running cursor: a draw rejected after its requests still uploaded).
    pm4RecordBatchPending(b, pending, shmPendCursor);
    if (b.zpdOp) {  // p2.zpd: an occlusion-query marker (after the copies queued up to it, before later draws)
      zpdMarker(b);
      continue;
    }
    if (dcfg.hit(long(bi), long(m_frameEpoch)) && !b.resolve) {
      char tex[640];
      int tn = 0;
      for (uint32_t t = 0; t < 16 && tn < 590; t++) {
        const uint32_t* tf = &b.texFetch[t * 6];
        if ((tf[0] & 3u) == 2u)
          tn += std::snprintf(tex + tn, sizeof(tex) - tn, " t%u=%08X/f%u/d%u/s%08X/z%03X/e%d", t,
                              tf[1] & 0xFFFFF000u, tf[1] & 0x3Fu, (tf[5] >> 9) & 3u, tf[2],
                              (tf[3] >> 1) & 0xFFFu, int32_t(tf[3] << 13) >> 26);
      }
      tex[tn] = 0;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_BATCHTEX] #%zu%s", bi, tex);
    }
    if (dcfg.hit(long(bi), long(m_frameEpoch)))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_BATCH] #%zu draw=%u pm4=%d resolve=%d xlat=%d vsf=%d vs=%08X ps=%08X verts=%u "
                  "c0=%08X depth=%08X mask=%X blend0=%08X depthCtl=%08X mode=%u",
                  bi, b.drawOrdinal, b.pm4 ? 1 : 0, b.resolve ? 1 : 0, b.xlat ? 1 : 0, b.vsf ? 1 : 0,
                  uint32_t(b.vsHash >> 32), uint32_t(b.psHash >> 32), b.vertexCount,
                  b.pm4Targets.color[0].v, b.pm4Targets.depthBound ? b.pm4Targets.depth.v : 0u,
                  b.pm4Targets.colorWriteMask[0], b.pm4Blend0, b.pm4Regs.depthControl,
                  b.pm4Regs.modeControl & 7u);
    if (!b.pm4) continue;
    if (b.resolve) {
      const auto tr = g_pm4DiagTiming ? Clock::now() : Clock::time_point{};
      gtMark(GtDraw);  // iter-466 res4k: SR_PM4_GPU_TIME per-pass attribution
      pm4ExecResolve(b, bi);
      gtMark(GtResolve);
      nbEnd();  // iter-467 native-b census: pm4ExecResolve made its record current
      msResolve += since(tr);
      rootBound = false;
      continue;
    }
    if (!b.xlat || !b.xlatPso) continue;
    const edram::DrawTargets& t = b.pm4Targets;
    // Make every bound surface exist first (creation clears to the zeroed mirror).
    Pm4HostRt* rts[4] = {};
    Pm4HostRt* ds = nullptr;
    bool ok = true;
    uint32_t numRt = 0;
    for (uint32_t i = 0; i < 4; i++) {
      if (!t.colorWriteMask[i]) continue;
      rts[i] = pm4Rt(t.color[i]);
      ok = ok && rts[i];
      numRt = i + 1;
    }
    if (t.depthBound) {
      ds = pm4Rt(t.depth);
      ok = ok && ds;
    }
    if (!ok) {
      g_pm4Stats.noRt++;
      continue;
    }
    // Claim the tiles this draw covers; move the previous owners' pixels in.
    std::vector<Pm4Move> moves;
    // iter-467 native-b B1c (SR_NB_CLEAR_PROP): an XDK clear claims only the tile rows its exact rect touches (the
    // P1 extent bounds the bottom only), so a strip clear does not take the rows above it from their owner.
    const bool nbProp = nbClearPropOn();
    auto nbClaim = [&](edram::RtKey k, uint32_t len) {
      uint32_t s = k.base();
      if (nbProp && t.rectExact && len && k.pitchTiles()) {
        const uint32_t th = k.tileHeightPixels(), pt = k.pitchTiles();
        const uint32_t r0 = uint32_t(std::max(t.fullRect[1], 0)) / th;
        const uint32_t r1 = (uint32_t(std::max(t.fullRect[3], 0)) + th - 1) / th;
        const uint32_t e = std::min(len, r1 * pt), b0 = std::min(r0 * pt, e);
        s = (k.base() + b0) % edram::kTileCount;
        len = e - b0;
      }
      pm4Claim(k, s, len, moves);
    };
    for (uint32_t i = 0; i < 4; i++)
      if (t.colorWriteMask[i]) nbClaim(t.color[i], t.claimTiles[i]);
    if (t.depthBound) nbClaim(t.depth, t.claimTiles[4]);
    // p2.zpd (zpd5: the depth test killed ~91 %): which surface held the depth tiles a query draw tests, and the frame's
    // last draw that rewrote a whole depth binding (an XDK clear) - the [NATIVE_ZPD] draw line. Before the claim's
    // moves are elided or performed: the claim's own answer.
    m_zpdDrawSerial++;  // every draw batch, across the frame's partial sets (bi restarts in each set)
    if (t.depthBound && t.fullWrite & 0x10u) {
      m_zpdClearKey = t.depth.v;
      m_zpdClearSerial = m_zpdDrawSerial;
      m_zpdClearEpoch = m_frameEpoch;
      m_zpdClearZero = (t.clearValid & 0x10u) != 0;
    }
    if (m_zpdOpenId && m_zpdDrawLines < kZpdDrawLines && t.depthBound) {
      int n = 0;
      uint32_t cnt = 0;
      for (const Pm4Move& mv : moves) {
        if (!(mv.dst == t.depth)) continue;
        cnt++;
        if (n < int(sizeof(m_zpdDepthMoves)) - 40)
          n += std::snprintf(m_zpdDepthMoves + n, sizeof(m_zpdDepthMoves) - n, " %08X[%u,%u)", mv.src.v, mv.start,
                             mv.end);
      }
      if (!cnt) std::snprintf(m_zpdDepthMoves, sizeof(m_zpdDepthMoves), " none");
    }
    // NATIVE N4a (SR_NATIVE_RT=1, default OFF, replay/diagnostic): native surfaces - every
    // RtKey keeps its own host render target and NO pixels move between keys that alias
    // EDRAM tiles (a PC renderer has no EDRAM to reinterpret). Only the first load of a
    // key's tiles from the EDRAM snapshot (source = nobody) is kept. Scoring this against
    // the golden answers whether a fixture needs EDRAM reinterpretation (roadmap Q5).
    // SR_NATIVE_RT=2: keep only transfers between keys at the SAME EDRAM base (a surface
    // re-created over the same memory with another format / MSAA mode inherits its pixels);
    // drop the ones between partially overlapping surfaces at different bases.
    static const int s_nativeRt = [] {
      const char* e = std::getenv("SR_NATIVE_RT");
      return (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
    }();
    if (s_nativeRt && !moves.empty()) {
      static uint64_t s_dropped = 0, s_kept = 0;
      std::vector<Pm4Move> keep;
      for (const Pm4Move& mv : moves) {
        if (mv.src.empty() || (s_nativeRt == 2 && mv.src.base() == mv.dst.base())) keep.push_back(mv);
        else s_dropped += mv.end - mv.start;
      }
      s_kept += keep.size();
      if ((s_dropped & 0xFFF) == 0 || keep.size() != moves.size()) {
        static uint32_t s_log = 0;
        if (s_log++ < 16)
          SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_RT] dropped EDRAM transfers: tiles=%llu (kept initial loads %llu)",
                      (unsigned long long)s_dropped, (unsigned long long)s_kept);
      }
      moves.swap(keep);
    }
    // iter-467 native-b B1 (diagnostic, SR_NB_XFER_DROP=<classes, '+'-separated>): drop the transfers of
    // the named classes - "C6>D0m" = a colour k_16_16_FLOAT source to a 4x-MSAA k_24_8 depth destination
    // (kind C/D, the key's format, m = MSAA) - to learn which ones any output observes (replay frames +
    // SR_NB_RHASH). Initial loads (no source) are kept.
    static const std::vector<std::string> s_xferDrop = [] {
      std::vector<std::string> v;
      if (const char* e = std::getenv("SR_NB_XFER_DROP")) {
        std::string s(e), cur;
        for (char ch : s + "+") {
          if (ch == '+') {
            if (!cur.empty()) v.push_back(cur);
            cur.clear();
          } else {
            cur += ch;
          }
        }
      }
      return v;
    }();
    if (!s_xferDrop.empty() && !moves.empty()) {
      auto cls = [](edram::RtKey k) {
        char b[8];
        std::snprintf(b, sizeof(b), "%c%u%s", k.isDepth() ? 'D' : 'C', k.format(), k.msaa() ? "m" : "");
        return std::string(b);
      };
      std::vector<Pm4Move> keep;
      for (const Pm4Move& mv : moves) {
        const bool drop = !mv.src.empty() &&
                          std::find(s_xferDrop.begin(), s_xferDrop.end(), cls(mv.src) + ">" + cls(mv.dst)) != s_xferDrop.end();
        if (!drop) keep.push_back(mv);
      }
      moves.swap(keep);
    }
    // iter-467 native-b B1 (SR_NB_CLEAR_ELIDE): the tiles of a move that this XDK clear rewrites
    // completely (pm4PrepareDraw's fullRect, inside the host scissor) are not moved; the rest of the
    // move stays, split into its uncovered runs.
    uint32_t nbElided = 0;
    if (t.fullWrite && !moves.empty()) {
      const resscale::Scale& rs = resscale::get();
      const int32_t sx = int32_t(rs.x), sy = int32_t(rs.y);
      // The scissor the draw is bound with below: the batch's, clamped to the smallest bound target.
      LONG scR = b.pm4Scissor.right, scB = b.pm4Scissor.bottom;
      for (uint32_t i = 0; i < 4; i++)
        if (rts[i]) {
          scR = std::min<LONG>(scR, LONG(rts[i]->w));
          scB = std::min<LONG>(scB, LONG(rts[i]->h));
        }
      if (ds) {
        scR = std::min<LONG>(scR, LONG(ds->w));
        scB = std::min<LONG>(scB, LONG(ds->h));
      }
      const int32_t rx0 = std::max(t.fullRect[0], int32_t((b.pm4Scissor.left + sx - 1) / sx));
      const int32_t ry0 = std::max(t.fullRect[1], int32_t((b.pm4Scissor.top + sy - 1) / sy));
      const int32_t rx1 = std::min(t.fullRect[2], int32_t(scR / sx));
      const int32_t ry1 = std::min(t.fullRect[3], int32_t(scB / sy));
      if (rx1 > rx0 && ry1 > ry0) {
        std::vector<Pm4Move> keep;
        for (const Pm4Move& mv : moves) {
          bool full = t.depthBound && mv.dst == t.depth && (t.fullWrite & 0x10u);
          for (uint32_t i = 0; i < 4 && !full; i++)
            full = t.colorWriteMask[i] && mv.dst == t.color[i] && (t.fullWrite & (1u << i));
          const uint32_t pitch = mv.dst.pitchTiles();
          if (!full || !pitch) {
            keep.push_back(mv);
            continue;
          }
          const int32_t tw = int32_t(mv.dst.tileWidthPixels()), th = int32_t(mv.dst.tileHeightPixels());
          uint32_t run = UINT32_MAX;
          for (uint32_t tt = mv.start; tt < mv.end; tt++) {
            const uint32_t rel = (tt + edram::kTileCount - mv.dst.base()) % edram::kTileCount;
            const int32_t col = int32_t(rel % pitch), row = int32_t(rel / pitch);
            const bool cov = col * tw >= rx0 && (col + 1) * tw <= rx1 && row * th >= ry0 && (row + 1) * th <= ry1;
            if (cov) {
              nbElided++;
              if (run != UINT32_MAX) keep.push_back(Pm4Move{mv.src, mv.dst, run, tt});
              run = UINT32_MAX;
            } else if (run == UINT32_MAX) {
              run = tt;
            }
          }
          if (run != UINT32_MAX) keep.push_back(Pm4Move{mv.src, mv.dst, run, mv.end});
        }
        if (nbElided) {
          static uint64_t s_tiles = 0, s_sets = 0;
          s_tiles += nbElided;
          s_sets++;
          if (s_sets <= 12 || (s_sets & (s_sets - 1)) == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[NB_ELIDE] batch=%zu draw=%u clear rect (%d,%d)-(%d,%d) full=%X: %u tiles not moved, "
                        "%zu -> %zu moves (total %llu tiles in %llu sets)",
                        bi, b.drawOrdinal, rx0, ry0, rx1, ry1, unsigned(t.fullWrite), nbElided, moves.size(),
                        keep.size(), (unsigned long long)s_tiles, (unsigned long long)s_sets);
          moves.swap(keep);
        }
      }
    }
    // iter-467 native-b B1c (SR_NB_CLEAR_PROP): moves out of an XDK clear's uniform tiles become host clears.
    uint32_t nbPropT = 0;
#if SR_ORACLE
    static const bool s_propVerify = [] {
      const char* e = std::getenv("SR_NB_CLEAR_PROP_VERIFY");
      return e && e[0] == '1';
    }();
    std::vector<Pm4Move> nbVerifyMoves;
    if (nbProp && !moves.empty()) {
      if (s_propVerify) nbVerifyMoves = moves;  // VERIFY: the real transfer runs, then its result is compared
      else nbPropT = nbPropagateClears(moves, bi);
    }
#else
    if (nbProp && !moves.empty()) nbPropT = nbPropagateClears(moves, bi);
#endif
    const uint32_t nbZeroT = moves.empty() ? 0u : nbZeroLoads(moves, bi);  // SR_NB_ZERO_INIT (T5)
    m_meterCur.elidedTiles += nbElided + nbPropT + nbZeroT;  // NATIVE METER
    pm4MeterMoves(moves);
    // B1b (SR_NB_COVER_FILL): moves into this draw's colour bindings run after it, into the pixels it leaves.
    std::vector<Pm4Move> nbFillMoves;
    NbFillScratch* nbScratch = nullptr;
    if (b.xlatPsoMark && !moves.empty() && nbCoverFillOn()) {
      bool ok = true;
      uint32_t fw = 0, fh = 0;
      for (uint32_t i = 0; i < 4 && ok; i++) {
        if (!rts[i]) continue;
        if (!fw) {
          fw = rts[i]->w;
          fh = rts[i]->h;
        }
        ok = rts[i]->w == fw && rts[i]->h == fh && pm4XferDirectPso(rts[i]->fmt, true) != nullptr;
      }
      for (const Pm4Move& mv : moves) {
        bool bound = false;
        for (uint32_t i = 0; i < 4; i++) bound = bound || (t.colorWriteMask[i] && mv.dst == t.color[i]);
        ok = ok && bound && !mv.dst.isDepth() && !mv.src.empty() && pm4XferDirectOk(mv);
      }
      if (ok && fw) nbScratch = nbFillScratch(fw, fh);
      if (nbScratch) nbFillMoves.swap(moves);
    }
#if SR_ORACLE
    if (m_nbFrameOn && (nbElided || nbPropT || nbZeroT) && moves.empty()) {  // census: the set that no longer moves anything
      const uint32_t id = nbAdd("X batch=%zu n=0 elided=%u", bi, nbElided);
      if (nbPropT) nbAppend(id, " prop=%u", nbPropT);
      if (nbZeroT) nbAppend(id, " zero=%u", nbZeroT);
      nbBegin(id);
      nbEnd();
    }
#endif
    if (!moves.empty()) {
      static std::atomic<uint32_t> s_xl{0};
      for (const Pm4Move& mv : moves)
        if (s_xl.fetch_add(1) < 200)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[PM4_XFER] batch=%zu tiles [%u,%u) %08X -> %08X (vs=%08X ps=%08X)", bi,
                      mv.start, mv.end, mv.src.v, mv.dst.v, uint32_t(b.vsHash >> 32),
                      uint32_t(b.psHash >> 32));
      const auto tx = g_pm4DiagTiming ? Clock::now() : Clock::time_point{};
      gtMark(GtDraw);  // iter-466 res4k: SR_PM4_GPU_TIME per-pass attribution
#if SR_ORACLE
      if (m_nbFrameOn) {  // iter-467 native-b census: one record per batch's transfer set
        // D1 (NativeTargets, gfx/native_targets.hpp): the record form round-trips the drain's targets.
        {
          const edram::DrawTargets rt2 = toDrawTargets(fromDrawTargets(t));
          bool same = rt2.depthBound == t.depthBound && (!t.depthBound || rt2.depth.v == t.depth.v) &&
                      rt2.zEnable == t.zEnable && rt2.zWrite == t.zWrite && rt2.stencilEnable == t.stencilEnable &&
                      rt2.zFunc == t.zFunc;
          for (int i = 0; i < 4; i++)
            same = same && rt2.colorWriteMask[i] == t.colorWriteMask[i] &&
                   (!t.colorWriteMask[i] || rt2.color[i].v == t.color[i].v);
          for (int i = 0; i < 5; i++) same = same && rt2.claimTiles[i] == t.claimTiles[i];
          static uint32_t s_ntBad = 0;
          if (!same && s_ntBad++ < 8)
            SR_LOG_BOOT(sr::LogSeverity::Warn, "[NB_TARGETS] NativeTargets round trip MISMATCH at batch %zu", bi);
        }
        const uint32_t id = nbAdd("X batch=%zu n=%zu draw[c0=%08X/%X c1=%08X/%X d=%08X z%d%d f%u s%d blend0=%08X depthCtl=%08X]",
                                  bi, moves.size(), t.colorWriteMask[0] ? t.color[0].v : 0u, t.colorWriteMask[0],
                                  t.colorWriteMask[1] ? t.color[1].v : 0u, t.colorWriteMask[1],
                                  t.depthBound ? t.depth.v : 0u, t.zEnable ? 1 : 0, t.zWrite ? 1 : 0, t.zFunc,
                                  t.stencilEnable ? 1 : 0, b.pm4Blend0, b.pm4Regs.depthControl);
        nbAppend(id, " sc=(%ld,%ld)-(%ld,%ld) vp=(%.0f,%.0f %.0fx%.0f) verts=%u topo=%u stencilRef=%08X claim=[%u %u %u %u|%u]",
                 b.pm4Scissor.left, b.pm4Scissor.top, b.pm4Scissor.right, b.pm4Scissor.bottom, b.xlatViewport.TopLeftX,
                 b.xlatViewport.TopLeftY, b.xlatViewport.Width, b.xlatViewport.Height, b.vertexCount,
                 unsigned(b.topology), b.pm4Regs.stencilRefMask, t.claimTiles[0], t.claimTiles[1], t.claimTiles[2],
                 t.claimTiles[3], t.claimTiles[4]);
        if (nbElided) nbAppend(id, " elided=%u", nbElided);
        if (nbPropT) nbAppend(id, " prop=%u", nbPropT);
        for (const Pm4Move& mv : moves)
          nbAppend(id, " [%08X(%c%u%s)->%08X(%c%u%s) t%u+%u]", mv.src.v, mv.src.empty() ? '-' : mv.src.isDepth() ? 'D' : 'C',
                   mv.src.format(), mv.src.empty() ? "" : (mv.src.msaa() ? "m" : ""), mv.dst.v, mv.dst.isDepth() ? 'D' : 'C',
                   mv.dst.format(), mv.dst.msaa() ? "m" : "", mv.start, mv.end - mv.start);
        nbBegin(id);
      }
#endif
      pm4MoveTiles(moves);  // C1 stage 3: the direct passes (the oracle unit's buffer for a move they cannot take)
      gtMark(GtLoad);       // (one segment for the set: the buffer store, when the oracle takes one, is inside it)
      nbEnd();
#if SR_ORACLE
      if (!nbVerifyMoves.empty()) nbPropVerify(nbVerifyMoves, bi);  // SR_NB_CLEAR_PROP_VERIFY (diagnostic)
#endif
      msXfer += since(tx);
      rootBound = false;
    }
    // B1c: this draw rewrites its bindings - their uniform records end (a clear records anew after its draw).
    if (!g_nbUni.empty()) {
      for (uint32_t i = 0; i < 4; i++)
        if (t.colorWriteMask[i]) g_nbUni.erase(t.color[i].v);
      if (t.depthBound) g_nbUni.erase(t.depth.v);
    }
    // Bind.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    uint32_t minW = UINT32_MAX, minH = UINT32_MAX;
    for (uint32_t i = 0; i < numRt; i++) {
      if (rts[i]) {
        pm4Transition(*rts[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv[i] = rts[i]->view;
        minW = std::min(minW, rts[i]->w);
        minH = std::min(minH, rts[i]->h);
      } else {
        rtv[i] = m_pm4NullRtv;
      }
    }
    if (ds) {
      pm4Transition(*ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      minW = std::min(minW, ds->w);
      minH = std::min(minH, ds->h);
    }
    D3D12_RECT sc = b.pm4Scissor;
    sc.right = std::min<LONG>(sc.right, LONG(minW));
    sc.bottom = std::min<LONG>(sc.bottom, LONG(minH));
    if (sc.right <= sc.left || sc.bottom <= sc.top) {
      if (!nbFillMoves.empty()) {  // B1b: no draw - the moves run as they would have
        pm4MoveTiles(nbFillMoves);
        rootBound = false;
      }
      g_pm4Stats.skipped++;
      continue;
    }
    const auto tb = g_pm4DiagTiming ? Clock::now() : Clock::time_point{};
    static const bool s_texInline = [] {
      const char* e = std::getenv("SR_TEX_INLINE_UPLOAD");
      return !e || e[0] != '0';
    }();
    m_texInline = s_texInline;
    const uint32_t flushesBefore = g_pm4Stats.flushes;
    xlatBindBatchTextures(b);
    m_texInline = false;
    // A shared-memory upload that filled its arena flushed (and reset) the
    // command list mid-bind: the graphics root state is gone.
    if (g_pm4Stats.flushes != flushesBefore) rootBound = false;
    msBind += since(tb);
    // iter-460 (SR_PM4_FRAMELOG=<frame>): every batch of one frame - shaders,
    // topology, count, viewport, RT0, and each bound 2D texture with the slot
    // the cache gave it (0 = the white fallback).
    static const long s_frameLog = [] {
      const char* e = std::getenv("SR_PM4_FRAMELOG");
      return e ? std::atol(e) : -1L;
    }();
    if (s_frameLog >= 0 && long(m_frameEpoch) == s_frameLog) {
      const uint32_t* sharedTex = reinterpret_cast<const uint32_t*>(m_xlatCBPtr + b.sharedCbOff);
      char tx[512];
      int tp = 0;
      for (uint32_t ti = 0; ti < 16 && tp < 480; ti++) {
        const uint32_t* tf = &b.texFetch[ti * 6];
        if ((tf[0] & 3u) != 2u) continue;
        tp += std::snprintf(tx + tp, sizeof(tx) - tp, " t%u:%08X/f%u/%ux%u/%s/s%u", ti,
                            ((tf[1] >> 12) & 0xFFFFFu) << 12, tf[1] & 0x3Fu, (tf[2] & 0x1FFFu) + 1u,
                            ((tf[2] >> 13) & 0x1FFFu) + 1u, (tf[0] >> 31) ? "T" : "L",
                            sharedTex[ti]);
      }
      tx[tp] = 0;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_FRAMELOG] f%u b%zu vs=%016llX ps=%016llX topo=%u n=%u vsf=%d vp=(%.0f,%.0f %.0fx%.0f) "
                  "rt0=%08X ds=%d%s",
                  uint32_t(m_frameEpoch), bi, (unsigned long long)b.vsHash,
                  (unsigned long long)b.psHash, unsigned(b.topology), b.vertexCount, b.vsf ? 1 : 0,
                  b.xlatViewport.TopLeftX, b.xlatViewport.TopLeftY, b.xlatViewport.Width,
                  b.xlatViewport.Height, t.colorWriteMask[0] ? t.color[0].v : 0u, ds ? 1 : 0, tx);
    }
#if SR_ORACLE
    // Observe the actual resource contents at this draw cursor. A private
    // readback flush resets root state; the normal bindings below restore it.
    if (b.resourceFetchCapture) {
      if (resourceInspectFetch(b)) rootBound = false;
    }
    if (!b.resourceConsumerChecks.empty() && resourceValidateConsumer(b)) rootBound = false;
#endif
    // Output state after the texture bind: a bind that flushed (shared-memory
    // arena full) reset the list, and with it everything recorded before.
    for (uint32_t i = 0; i < numRt; ++i) if (rts[i]) keepNativeImage(rts[i]->image);
    if (ds) keepNativeImage(ds->image);
    const bool nbFill = !nbFillMoves.empty();
    if (nbFill)  // B1b: the scratch stencil starts at 0 (a fast clear of the whole surface)
      m_commandList->ClearDepthStencilView(nbScratch->dsv, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
    m_commandList->OMSetRenderTargets(numRt, numRt ? rtv : nullptr, FALSE,
                                      ds ? &ds->view : (nbFill ? &nbScratch->dsv : nullptr));
    m_commandList->RSSetScissorRects(1, &sc);
    if (!rootBound) {
      xlatBindRootTables();
      rootBound = true;
    }
    m_commandList->SetPipelineState(nbFill ? b.xlatPsoMark : b.xlatPso);
    if (ds) m_commandList->OMSetStencilRef(b.pm4Regs.stencilRefMask & 0xFFu);
    else if (nbFill) m_commandList->OMSetStencilRef(1);
    m_commandList->RSSetViewports(1, &b.xlatViewport);
    const auto constants = bindXlatConstants(b);  // post-bind snapshot owns float/shared/fetch bytes
    bindXlatGeometry(b);  // post-flush owned vertices or fetch indices
    m_commandList->IASetPrimitiveTopology(b.topology);
#if SR_ORACLE
    const bool textureProbed = resourceTextureProbe(b, constants);
#endif
    if (m_dredOn) {
      // DRED breadcrumb context: which draw a hang/fault names.
      wchar_t mk[112];
      const int mn = swprintf(mk, 112, L"b%zu vs%016llX ps%016llX n%u vsf%d prim%u", bi,
                              (unsigned long long)b.vsHash, (unsigned long long)b.psHash,
                              b.vertexCount, b.vsf ? 1 : 0, unsigned(b.topology));
      if (mn > 0) m_commandList->SetMarker(0, mk, UINT((mn + 1) * sizeof(wchar_t)));
      pm4Crumb(uint32_t(bi), false);
    }
    const auto drawCall = [&] {
      if (b.vsf) {
        // In-shader vertex fetch: staged guest memory + patched fetch constants;
        // the index list carries the remapped guest indices (= SV_VertexID).
        if (b.vsfShm)
          pm4ShmState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (b.vsfNoIb) {
          m_commandList->DrawInstanced(b.vertexCount, 1, b.vsfFirstVertex, 0);
        } else {
          m_commandList->DrawIndexedInstanced(b.vertexCount, 1, 0, 0, 0);
        }
      } else {
        m_commandList->DrawInstanced(b.vertexCount, 1, 0, 0);
      }
    };
    // p2.zpd: an open occlusion query measures this guest draw and nothing else - its own segment, begun here and
    // ended right after the draw call (the barrier below is no rasterization).
    const uint32_t zpdSeg = m_zpdOpenId ? zpdDrawBegin(b) : zpd::kNoSeg;
    drawCall();
#if SR_ORACLE
    for (const auto& check : b.resourceConsumerChecks)
      if (!memres::resourceConsumerRecorded(check, b.resourceConsumerId))
        throw std::runtime_error("native consumer check was not validated before draw");
    b.resourceConsumerFetch.reset();
    b.resourceConsumerFetchLease.reset();
    if (textureProbed) {
      m_resourceTextureProbe->recorded = true;
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[MEM_RES_TEX] draw_recorded=%u completion=unconfirmed", b.drawOrdinal);
    }
    if (b.resourceFetchCapture) {
      fetchObservation.recorded = true;
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[MEM_RES_FETCH] draw_recorded=%llu ordinal=%u completion=unconfirmed epoch_coverage=unknown",
          (unsigned long long)b.resourceFetchId, b.drawOrdinal);
    }
#endif
    if (zpdSeg != zpd::kNoSeg) {
      zpdDrawEnd(zpdSeg);  // before the probe draws, the B1b fill and any helper pass below
      // The probe (gfx/native_zpd.hpp segmentOpenProbe): the same draw again on segments that are not the answer,
      // writing nothing (not over the B1b scratch stencil: its pipelines bind the draw's own depth format).
      bool probed = false;
      if (!nbFill) {
        const std::pair<ID3D12PipelineState*, uint8_t> probes[2] = {{b.zpdRefPso, zpd::kRef},
                                                                    {b.zpdRasterPso, zpd::kRaster}};
        for (const auto& [probePso, kind] : probes) {
          if (!probePso) continue;
          const uint32_t probeSeg = zpdProbeBegin(kind);
          if (probeSeg == zpd::kNoSeg) continue;
          m_commandList->SetPipelineState(probePso);
          drawCall();
          zpdDrawEnd(probeSeg);
          probed = true;
        }
        if (probed) m_commandList->SetPipelineState(b.xlatPso);
      }
      zpdLogDraw(b, sc, ds != nullptr, probed, bi);
    }
    g_pm4Stats.draws++;
    if (nbProp && t.rectExact && t.fullWrite && t.clearValid) nbUniRecord(t, sc, b.drawOrdinal);  // B1c
    if (nbFill) {  // B1b: the old content, only where the draw wrote nothing
      gtMark(GtDraw);
#if SR_ORACLE
      uint32_t id = 0;
      if (m_nbFrameOn) {
        id = nbAdd("X batch=%zu n=%zu fill", bi, nbFillMoves.size());
        for (const Pm4Move& mv : nbFillMoves)
          nbAppend(id, " [%08X(%c%u%s)->%08X(%c%u%s) t%u+%u]", mv.src.v, mv.src.isDepth() ? 'D' : 'C',
                   mv.src.format(), mv.src.msaa() ? "m" : "", mv.dst.v, mv.dst.isDepth() ? 'D' : 'C',
                   mv.dst.format(), mv.dst.msaa() ? "m" : "", mv.start, mv.end - mv.start);
        nbBegin(id);
      }
#endif
      nbFillLoad(nbFillMoves, nbScratch->dsv);
      gtMark(GtLoad);
#if SR_ORACLE
      if (id) nbEnd();
#endif
      rootBound = false;
    }
    if (m_dredOn) pm4Crumb(uint32_t(bi), true);
    if (rts[0]) hdrFreezeProbe(b, *rts[0], bi, sc, constants);
    const Pm4DumpCfg& dc = pm4DumpCfg();
    if (dc.hit(long(bi), long(m_frameEpoch)) && rts[0]) {
      // Read RT0 back whole and write it as clamped 8-bit BGRA.
      Pm4HostRt& r0 = *rts[0];
      D3D12_RESOURCE_DESC desc = r0.res->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT rows = 0;
      UINT64 rowBytes = 0, total = 0;
      m_device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
      if (total <= kPm4StagingBytes && pm4EnsureReadback()) {
        pm4Transition(r0, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m_pm4Readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = r0.res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        pm4Flush();
        rootBound = false;
        uint8_t* mp = nullptr;
        const D3D12_RANGE rr{0, SIZE_T(total)};
        if (SUCCEEDED(m_pm4Readback->Map(0, &rr, reinterpret_cast<void**>(&mp))) && mp) {
          const uint32_t dw = r0.w, dh = std::min<uint32_t>(r0.h, 1024u * resscale::get().y);
          const edram::HostFmt hf = t.color[0].hostFormat();
          const uint32_t bpt = edram::hostBytesPerTexel(hf);
          std::vector<uint8_t> bgra(size_t(dw) * dh * 4u);
          float mn[4] = {1e30f, 1e30f, 1e30f, 1e30f}, mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
          double sum[4] = {};
          for (uint32_t y = 0; y < dh; y++)
            for (uint32_t x = 0; x < dw; x++) {
              float c[4];
              edram::hostToFloat4(hf, mp + uint64_t(y) * fp.Footprint.RowPitch + uint64_t(x) * bpt, c);
              for (int k = 0; k < 4; k++) {
                mn[k] = std::min(mn[k], c[k]);
                mx[k] = std::max(mx[k], c[k]);
                sum[k] += c[k];
              }
              uint8_t* o = &bgra[(size_t(y) * dw + x) * 4u];
              o[0] = uint8_t(edram::packUNorm(c[2], 255.f));
              o[1] = uint8_t(edram::packUNorm(c[1], 255.f));
              o[2] = uint8_t(edram::packUNorm(c[0], 255.f));
              o[3] = 255;
            }
          // step 4 lane C 2i (SR_PM4_DUMP_HASH=1, diagnostic, observe-only): the RT0 checkpoint as an FNV-1a of
          // its raw host texels (every row, dw * bpt bytes) per frame, and no BMP - for a replay looped hundreds of
          // times (SR_GPU_TRACE_REPLAY_LOOPS), so an odd frame names the first checkpoint whose bytes differ.
#if SR_ORACLE
          static const bool s_hashOnly = [] {
            const char* e = std::getenv("SR_PM4_DUMP_HASH");
            return e && e[0] == '1';
          }();
#else
          constexpr bool s_hashOnly = false;
#endif
          uint64_t fnv = 1469598103934665603ull;
          if (s_hashOnly)
            for (uint32_t y = 0; y < dh; y++) {
              const uint8_t* row = mp + uint64_t(y) * fp.Footprint.RowPitch;
              for (uint64_t k = 0; k < uint64_t(dw) * bpt; k++) fnv = (fnv ^ row[k]) * 1099511628211ull;
            }
          const D3D12_RANGE wr{0, 0};
          m_pm4Readback->Unmap(0, &wr);
          const double np = double(dw) * dh;
          if (s_hashOnly) {
            SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_DUMP_HASH] f%llu b%zu key=%08X %ux%u fnv=%016llX",
                        (unsigned long long)m_frameEpoch, bi, t.color[0].v, dw, dh, (unsigned long long)fnv);
            continue;
          }
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[PM4_DUMPSTAT] #%zu key=%08X %ux%u r[%.4g,%.4g,%.4g] g[%.4g,%.4g,%.4g] "
                      "b[%.4g,%.4g,%.4g] a[%.4g,%.4g,%.4g] (min,max,mean)",
                      bi, t.color[0].v, dw, dh, mn[0], mx[0], sum[0] / np, mn[1], mx[1],
                      sum[1] / np, mn[2], mx[2], sum[2] / np, mn[3], mx[3], sum[3] / np);
          char name[160];
          std::snprintf(name, sizeof(name), "/b%05zu_key%08X_vs%08X_ps%08X.bmp", bi,
                        t.color[0].v, uint32_t(b.vsHash >> 32), uint32_t(b.psHash >> 32));
          CreateDirectoryA(dc.dir.c_str(), nullptr);
          pm4WriteBmp(dc.dir + name, bgra.data(), dw, dh);
        }
      }
    }
  }
  livetrace::setDrawContext(0);
#if SR_ORACLE
  if (!batches.empty()) edramDumpAfterDraw(batches.back());  // SR_PM4_EDRAM_DUMP after the set's last draw
  const size_t ownRanges = edramOwnRanges();  // the tile map's ranges (the oracle unit)
#else
  const size_t ownRanges = nbSurfTable().size();  // C1 stage 3: the surface table's entries
#endif
  static std::atomic<uint32_t> s_log{0};
  const uint32_t ln = s_log.fetch_add(1) + 1;
  if (ln <= 8 || ln % 100 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_FRAME] #%u batches=%zu draws=%u skipped=%u noRt=%u transfers=%u tilesMoved=%u "
                "resolves=%u resolveTexels=%u clears=%u flushes=%u newRts=%u rts=%zu ownRanges=%zu "
                "drainMs=%.1f renderMs=%.1f (xfer %.1f bind %.1f resolve %.1f | gpuWait %.1f "
                "storeCpu %.1f resolveCpu %.1f)",
                ln, batches.size(), g_pm4Stats.draws, g_pm4Stats.skipped, g_pm4Stats.noRt,
                g_pm4Stats.transfers, g_pm4Stats.tilesMoved, g_pm4Stats.resolves,
                g_pm4Stats.resolveTexels, g_pm4Stats.clears, g_pm4Stats.flushes, g_pm4Stats.newRts,
                m_pm4Rts.size(), ownRanges,
                double(m_rtOn ? m_rtDrainNsShown : m_pm4DrainNs) * 1e-6,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameT0)
                    .count(), msXfer, msBind, msResolve, g_pm4Stats.msFlush, g_pm4Stats.msStoreCpu,
                g_pm4Stats.msResolveCpu);
  if (!m_rtOn) m_pm4DrainNs = 0;
  pm4MeterPublish();  // iter-467 native-b: NATIVE METER, this frame's EDRAM-model work
  // Reset AFTER reporting: drain-time skips (pm4PrepareDraw) land before render.
  g_pm4Stats = Pm4Stats{};
  pm4ShmRecordPending(pending, shmPendCursor, uint32_t(pending.size()));  // queued after the last batch
  pm4ShmEndFrame();
  // The next drain starts the next frame's set (the CP does it at the
  // handoff when the render thread runs - this is the render thread then).
  if (!m_rtOn) m_pm4DrainOpen = false;
}

// ═══════════════════════════════════════════════════════════════════════════
// iter-460: GPU EDRAM (SR_PM4_EDRAM_GPU, default ON under the RT cache).
//
// The CPU mirror above made every ownership transfer a GPU->CPU->GPU round
// trip with a full GPU sync (25-39 per intro frame, ~60 ms). Here the 10 MiB
// EDRAM is a GPU buffer in the same guest bit layout: a transfer's store is a
// compute pass (host RT -> packed guest bits, replicated to every sample) and
// its load a scissored full-RT triangle (guest bits -> host format). Same math
// as edram.hpp (packColor / unpackColor / packDepth / unpackDepth /
// sampleOffsetBytes), ported 1:1 with integer-exact float conversions, so the
// CPU path stays the reference (=0 restores it; the replay score is the gate).
// Depth stores read the raw plane bits from a copy (a UNORM24 SRV does not
// round-trip exactly); depth loads write SV_Depth (Xenia's XeUNorm24To32 is
// exact under the rasterizer's float->unorm24) and rebuild stencil one bit
// per pass (NVIDIA has no SV_StencilRef).
// ═══════════════════════════════════════════════════════════════════════════

bool pm4GpuEdramEnabled() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_EDRAM_GPU");
    return !(e && e[0] == '0');
  }();
  return v;
#else
  return true;  // C1 stage 3: the =0 arm is the CPU mirror of the oracle unit; the shipping transfers are the direct passes
#endif
}

// C1 stage 3: the HLSL in three parts - kPm4PackHlsl (pm4_render_detail.hpp: the constants, the render-target format
// encodings, the sample geometry of a transfer, VSQuad; shared with the oracle unit), kPm4XferHlsl (below: the direct
// transfer passes, shipping) and the oracle unit's store / load passes over the EDRAM buffer.
const char kPm4PackHlsl[] = R"HLSL(
cbuffer Xfer : register(b0) {
  uint gBase; uint gPitchTiles; uint gMsaa; uint gFlags;
  uint gRectX; uint gRectY; uint gRectW; uint gRectH;
  uint gSamples; uint gBit; uint gD0Off; uint gD0Pitch;
  uint gS1Off; uint gS1Pitch; uint gS1Bpt; uint gS1Byte;
};
#define kTileDwords 1280u
#define kEdramBytes 10485760u

uint Fmt() { return (gFlags >> 8) & 15u; }
bool IsDepth() { return (gFlags & 1u) != 0u; }
uint IntsLog2() { return (gFlags >> 1) & 1u; }
// iter-464 resscale: the draw resolution scale (flags bits 16-18 / 19-21; 0 = 1x).
uint ScaleX() { return max((gFlags >> 16) & 7u, 1u); }
uint ScaleY() { return max((gFlags >> 19) & 7u, 1u); }
uint EdramBytes() { return kTileDwords * ScaleX() * ScaleY() * 2048u * 4u; }

// edram.hpp sampleOffsetBytes; with a scale, Xenia edram.xesli XeEdramOffsetBytes
// (tile = 80*sx x 16*sy samples at 32bpp, px/py are host pixels).
uint SampleOffset(uint px, uint py, uint s) {
  uint il = IntsLog2();
  uint sx = (px << (gMsaa >= 2u ? 1u : 0u)) + ((s >> 1) & 1u);
  uint sy = (py << (gMsaa >= 1u ? 1u : 0u)) + (s & 1u);
  uint tw32 = 80u * ScaleX();
  uint th = 16u * ScaleY();
  uint tsx = tw32 >> il;
  uint tx = sx / tsx;
  uint ty = sy / th;
  uint base = gBase + ty * gPitchTiles + tx;
  sx -= tx * tsx;
  sy -= ty * th;
  if (IsDepth()) { uint hlf = tsx >> 1; sx = sx >= hlf ? sx - hlf : sx + hlf; }
  uint tileDw = tw32 * th;
  uint addr = base * tileDw + ((sy * tsx + sx) << il);
  addr %= tileDw * 2048u;
  return addr << 2;
}

bool IsNan(float f) { return (asuint(f) & 0x7FFFFFFFu) > 0x7F800000u; }

uint FloatToHalf(float f) {
  uint x = asuint(f);
  uint sgn = (x >> 16) & 0x8000u;
  uint mant = x & 0x7FFFFFu;
  int ex = int((x >> 23) & 0xFFu);
  if (ex == 0xFF) return sgn | 0x7C00u | (mant != 0u ? 0x200u : 0u);
  int e = ex - 127 + 15;
  // iter-467 freezefx: a DECIMAL bound. FXC types an unsuffixed hex literal as uint, so the
  // old `e >= 0x1F` compiled to an unsigned compare (uge): every e <= 0 (zero and |f| < 2^-14)
  // was stored as +-inf. SR_PM4_HALF_SIGNED=0 defines SR_HALF_HEXCMP = the old compare (A/B).
#ifdef SR_HALF_HEXCMP
  if (e >= 0x1F) return sgn | 0x7C00u;
#else
  if (e >= 31) return sgn | 0x7C00u;
#endif
  if (e <= 0) {
    if (e < -10) return sgn;
    mant |= 0x800000u;
    uint shift = uint(14 - e);
    uint h = mant >> shift;
    uint rem = mant & ((1u << shift) - 1u);
    uint halfway = 1u << (shift - 1u);
    if (rem > halfway || (rem == halfway && (h & 1u) != 0u)) h++;
    return sgn | h;
  }
  uint h2 = (uint(e) << 10) | (mant >> 13);
  uint rem2 = mant & 0x1FFFu;
  if (rem2 > 0x1000u || (rem2 == 0x1000u && (h2 & 1u) != 0u)) h2++;
  return sgn | h2;
}
float HalfToFloat(uint h) {
  uint sgn = (h & 0x8000u) << 16;
  uint ex = (h >> 10) & 0x1Fu;
  uint mant = h & 0x3FFu;
  if (ex == 0u) {
    if (mant == 0u) return asfloat(sgn);
    int e = 1;
    [loop] for (uint i = 0u; i < 11u && (mant & 0x400u) == 0u; i++) { mant <<= 1; e--; }
    mant &= 0x3FFu;
    return asfloat(sgn | (uint(e + 127 - 15) << 23) | (mant << 13));
  }
  if (ex == 0x1Fu) return asfloat(sgn | 0x7F800000u | (mant << 13));
  return asfloat(sgn | ((ex + 127u - 15u) << 23) | (mant << 13));
}
uint FloatTo7e3(float v) {
  uint i = asuint(v);
  if (IsNan(v) || (i & 0x80000000u) != 0u) return 0u;
  if (i > 0x41FF73FFu) return 0x3FFu;
  if (i < 0x3E800000u) {
    uint shift = 125u - (i >> 23);
    i = shift >= 32u ? 0u : ((0x800000u | (i & 0x7FFFFFu)) >> shift);
  } else {
    i += 0xC2000000u;
  }
  return ((i + 0x7FFFu + ((i >> 16) & 1u)) >> 16) & 0x3FFu;
}
float F7e3ToFloat(uint f10) {
  f10 &= 0x3FFu;
  if (f10 == 0u) return 0.0;
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7;
  if (exponent == 0u) {
    uint lz = 0u;
    [loop] for (uint i = 0u; i < 8u && (mantissa & 0x80u) == 0u; i++) { mantissa <<= 1; lz++; }
    mantissa &= 0x7Fu;
    return asfloat(((uint(1 - int(lz)) + 124u) << 23) | (mantissa << 16));
  }
  return asfloat(((exponent + 124u) << 23) | (mantissa << 16));
}
uint FloatTo20e4(float f32) {
  uint u = asuint(f32);
  u = min((u <= 0x7FFFFFFFu) ? u : 0u, 0x3FFFFFF8u);
  uint shift = min(113u - min(113u, u >> 23), 24u);
  uint denorm = ((u & 0x7FFFFFu) | 0x800000u) >> shift;
  uint f24 = (u < 0x38800000u) ? denorm : (u + 0xC8000000u);
  f24 += 3u + ((f24 >> 3) & 1u);
  return (f24 >> 3) & 0xFFFFFFu;
}
float F20e4ToFloat(uint f24) {
  f24 &= 0xFFFFFFu;
  if (f24 == 0u) return 0.0;
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20;
  if (exponent == 0u) {
    uint lz = 0u;
    [loop] for (uint i = 0u; i < 21u && (mantissa & 0x100000u) == 0u; i++) { mantissa <<= 1; lz++; }
    mantissa &= 0xFFFFFu;
    return asfloat(((uint(1 - int(lz)) + 112u) << 23) | (mantissa << 3));
  }
  return asfloat(((exponent + 112u) << 23) | (mantissa << 3));
}
uint PackUNorm(float f, float s) { return uint(saturate(f) * s + 0.5); }
uint PackFixed16(float f) {
  f = IsNan(f) ? 0.0 : f;
  f = min(max(f, -32.0), 32.0);
  return uint(int(f * (32767.0 / 32.0))) & 0xFFFFu;
}
float UnpackFixed16(uint v) { return max(-32.0, float(int(v << 16) >> 16) * (32.0 / 32767.0)); }

uint2 PackColor(uint fmt, float4 c) {
  if (fmt == 0u) return uint2(PackUNorm(c.r, 255.0) | (PackUNorm(c.g, 255.0) << 8) |
                              (PackUNorm(c.b, 255.0) << 16) | (PackUNorm(c.a, 255.0) << 24), 0u);
  if (fmt == 2u) return uint2(PackUNorm(c.r, 1023.0) | (PackUNorm(c.g, 1023.0) << 10) |
                              (PackUNorm(c.b, 1023.0) << 20) | (PackUNorm(c.a, 3.0) << 30), 0u);
  if (fmt == 3u) return uint2(FloatTo7e3(c.r) | (FloatTo7e3(c.g) << 10) | (FloatTo7e3(c.b) << 20) |
                              (PackUNorm(c.a, 3.0) << 30), 0u);
  if (fmt == 4u) return uint2(PackFixed16(c.r) | (PackFixed16(c.g) << 16), 0u);
  if (fmt == 5u) return uint2(PackFixed16(c.r) | (PackFixed16(c.g) << 16),
                              PackFixed16(c.b) | (PackFixed16(c.a) << 16));
  if (fmt == 6u) return uint2(FloatToHalf(c.r) | (FloatToHalf(c.g) << 16), 0u);
  if (fmt == 7u) return uint2(FloatToHalf(c.r) | (FloatToHalf(c.g) << 16),
                              FloatToHalf(c.b) | (FloatToHalf(c.a) << 16));
  if (fmt == 14u) return uint2(asuint(c.r), 0u);
  if (fmt == 15u) return uint2(asuint(c.r), asuint(c.g));
  return uint2(0u, 0u);
}
float4 UnpackColor(uint fmt, uint2 b) {
  uint lo = b.x, hi = b.y;
  if (fmt == 0u) return float4(float(lo & 0xFFu), float((lo >> 8) & 0xFFu), float((lo >> 16) & 0xFFu),
                               float(lo >> 24)) * (1.0 / 255.0);
  if (fmt == 2u) return float4(float(lo & 0x3FFu) * (1.0 / 1023.0), float((lo >> 10) & 0x3FFu) * (1.0 / 1023.0),
                               float((lo >> 20) & 0x3FFu) * (1.0 / 1023.0), float(lo >> 30) * (1.0 / 3.0));
  if (fmt == 3u) return float4(F7e3ToFloat(lo), F7e3ToFloat(lo >> 10), F7e3ToFloat(lo >> 20),
                               float(lo >> 30) * (1.0 / 3.0));
  if (fmt == 4u) return float4(UnpackFixed16(lo & 0xFFFFu), UnpackFixed16(lo >> 16), 0.0, 1.0);
  if (fmt == 5u) return float4(UnpackFixed16(lo & 0xFFFFu), UnpackFixed16(lo >> 16),
                               UnpackFixed16(hi & 0xFFFFu), UnpackFixed16(hi >> 16));
  if (fmt == 6u) return float4(HalfToFloat(lo & 0xFFFFu), HalfToFloat(lo >> 16), 0.0, 1.0);
  if (fmt == 7u) return float4(HalfToFloat(lo & 0xFFFFu), HalfToFloat(lo >> 16),
                               HalfToFloat(hi & 0xFFFFu), HalfToFloat(hi >> 16));
  if (fmt == 14u) return float4(asfloat(lo), 0.0, 0.0, 1.0);
  if (fmt == 15u) return float4(asfloat(lo), asfloat(hi), 0.0, 1.0);
  return float4(0.0, 0.0, 0.0, 1.0);
}

// ── store: host RT -> EDRAM (every sample) ──
Texture2D<float4> gSrc : register(t0);
float4 VSQuad(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2(float((id << 1) & 2u), float(id & 2u));
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)HLSL";


// The direct transfer passes (shipping): the alias geometry of a move - a destination pixel's tile sample is read from
// the source surface's pixel under it - with no EDRAM buffer between them.
const char kPm4XferHlsl[] = R"HLSL(
// ── direct transfer (iter-466 res4k, SR_PM4_XFER_DIRECT): the destination's EDRAM dwords are
// computed from the SOURCE host RT with the store's own pack, instead of a store into the EDRAM
// buffer and a load back (one pass, no s^2 x samples EDRAM traffic). Colour source and
// destination. The inverse of SampleOffset for the source key (b1): its tile pitch, 32/64bpp
// sample width, MSAA grid; MSAA keys render 1x, so any sample of a source pixel is its texel. ──
cbuffer XferSrc : register(b1) { uint gSBase; uint gSPitchTiles; uint gSMsaa; uint gSFlags; };
// iter-466 res4k round 2 (SR_PM4_XFER_DEPTH): a DEPTH source (gSFlags bit 0) reads its depth plane
// through t0 and its stencil plane through t3; the EDRAM dword is the store's (d24 << 8) | stencil.
// The unorm24 bits come back exactly: D3D's UNORM -> float -> UNORM round trip is exact, and
// Unorm24 below is that float -> UNORM conversion, round(f * (2^24 - 1)), in exact integer math
// (the 48-bit product mant * (2^24 - 1) as two uints; no doubles, so every PSO stays plain SM5).
uint Unorm24(float f) {
  uint b = asuint(f);
  if ((b & 0x80000000u) != 0u) return 0u;
  if (b >= 0x3F800000u) return 0xFFFFFFu;
  uint e = (b >> 23) & 0xFFu;
  if (e < 101u) return 0u;  // f < 2^-26
  uint mant = (b & 0x7FFFFFu) | 0x800000u;
  uint s = 150u - e;  // 24..49: p = (mant * 2^24 - mant) / 2^s
  uint lo1 = mant << 24;
  uint lo = lo1 - mant;
  uint hi = (mant >> 8) - (lo1 < mant ? 1u : 0u);
  if (s < 32u) {
    uint lo2 = lo + (1u << (s - 1u));
    uint hi2 = hi + (lo2 < lo ? 1u : 0u);
    return (lo2 >> s) | (hi2 << (32u - s));
  }
  if (s == 32u) { uint lo2 = lo + 0x80000000u; return hi + (lo2 < lo ? 1u : 0u); }
  return (hi + (1u << (s - 33u))) >> (s - 32u);
}
Texture2D<uint4> gSrcSt : register(t3);
uint SrcDepthDword(uint qx, uint qy) {
  float d = gSrc.Load(int3(int(qx), int(qy), 0)).x;
  uint st = gSrcSt.Load(int3(int(qx), int(qy), 0)).y & 0xFFu;
  uint d24 = ((gSFlags >> 8) & 15u) == 1u ? FloatTo20e4(d * 2.0) : Unorm24(d);
  return (d24 << 8) | st;
}
uint SrcDword(uint addrBytes) {
  uint dw = addrBytes >> 2;
  uint tw32 = 80u * ScaleX();
  uint th = 16u * ScaleY();
  uint tileDw = tw32 * th;
  uint t = dw / tileDw;
  uint w = dw - t * tileDw;
  uint il = (gSFlags >> 1) & 1u;
  uint tsx = tw32 >> il;
  uint hi = w & il;
  w >>= il;
  uint sy = w / tsx;
  uint sx = w - sy * tsx;
  // a depth tile's sample columns are stored with the halves swapped (SampleOffset): the same swap
  if ((gSFlags & 1u) != 0u) { uint hlf = tsx >> 1; sx = sx >= hlf ? sx - hlf : sx + hlf; }
  uint rel = (t + 2048u - (gSBase % 2048u)) % 2048u;
  uint ty = rel / gSPitchTiles;
  uint tx = rel - ty * gSPitchTiles;
  uint qx = (tx * tsx + sx) >> (gSMsaa >= 2u ? 1u : 0u);
  uint qy = (ty * th + sy) >> (gSMsaa >= 1u ? 1u : 0u);
  if ((gSFlags & 1u) != 0u) return SrcDepthDword(qx, qy);
  uint2 b = PackColor((gSFlags >> 8) & 15u, gSrc.Load(int3(int(qx), int(qy), 0)));
  return hi != 0u ? b.y : b.x;
}
float4 PSColorDirect(float4 pos : SV_Position) : SV_Target {
  uint off = SampleOffset(uint(pos.x), uint(pos.y), 0u);
  uint2 b = uint2(SrcDword(off), 0u);
  if (IntsLog2() != 0u) b.y = SrcDword((off + 4u) % EdramBytes());
  return UnpackColor(Fmt(), b);
}
// a DEPTH destination, in two steps: PSDwordDirect writes each destination pixel's EDRAM dword
// (from the source RT) into a scratch R32_UINT target once; then the load's PSDepth / PSStencil
// sequence (depth + one pass per stencil bit) reads it back with one Load each (t3 = the scratch).
// u1: 8 predicate qwords - qword k becomes nonzero when any pixel's stencil has bit k (a plain
// store of 1, skipped once seen: no atomics). SR_XFER_DEPTH_PRED predicates bit pass k on it.
RWByteAddressBuffer gXdPred : register(u1);
uint PSDwordDirect(float4 pos : SV_Position) : SV_Target {
  uint dw = SrcDword(SampleOffset(uint(pos.x), uint(pos.y), 0u));
  uint st = dw & 0xFFu;
  [branch] if (st != 0u) {
    [unroll] for (uint k = 0u; k < 8u; k++)
      if (((st >> k) & 1u) != 0u && gXdPred.Load(k * 8u) == 0u) gXdPred.Store(k * 8u, 1u);
  }
  return dw;
}
float PSDepthFromDw(float4 pos : SV_Position) : SV_Depth {
  uint d24 = gSrcSt.Load(int3(int(pos.x), int(pos.y), 0)).x >> 8;
  if (Fmt() == 1u) return F20e4ToFloat(d24) * 0.5;
  return float(d24 + (d24 >> 23)) * (1.0 / 16777216.0);
}
void PSStencilFromDw(float4 pos : SV_Position) {
  if (((gSrcSt.Load(int3(int(pos.x), int(pos.y), 0)).x >> gBit) & 1u) == 0u) discard;
}
)HLSL";

// r: HOST pixels (pm4HostRect). The scale rides in flags bits 16-21 (0 at 1x, so
// the 1x constants are the pre-resscale ones bit for bit). (Pm4XferConsts: pm4_render_detail.hpp)
Pm4XferConsts pm4XferConsts(edram::RtKey k, const edram::Rect& r) {
  Pm4XferConsts c{};
  c.base = k.base();
  c.pitchTiles = k.pitchTiles();
  c.msaa = k.msaa();
  c.flags = (k.isDepth() ? 1u : 0u) | (k.intsLog2() << 1) | ((k.format() & 15u) << 8);
  const resscale::Scale& rs = resscale::get();
  if (rs.scaled()) c.flags |= ((rs.x & 7u) << 16) | ((rs.y & 7u) << 19);
  c.rectX = r.x;
  c.rectY = r.y;
  c.rectW = r.w;
  c.rectH = r.h;
  c.samples = edram::msaaSampleCount(k.msaa());
  return c;
}

// iter-466 res4k: SR_PM4_XFER_DIRECT (default ON, =0 off) - a colour -> colour ownership transfer
// reads the source host RT directly (PSColorDirect) instead of storing it into the EDRAM buffer
// and loading it back. Xenia's host-RT path transfers the same way (one pass from the source RT,
// d3d12_render_target_cache.cc PerformTransfersAndResolveClears); the pack / unpack math is the
// EDRAM store's and load's own, so the destination receives the same bits.
bool pm4XferDirectEnabled() {
#if SR_ORACLE
  static const bool v = [] {
    // iter-466 res4k: baked default ON (replay byte-identical at 1x on 4 traces and at 3x on 7;
    // live 3x whole run, boss save and Fortress clean); =0 keeps the EDRAM store + load A/B.
    const char* e = std::getenv("SR_PM4_XFER_DIRECT");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;  // C1 stage 3 (3.1 rule 2): the =0 arm is the EDRAM store + load of the oracle unit - a fallback component
#endif
}
// iter-466 res4k round 2: transfers with a DEPTH source or destination also go direct - the source
// depth / stencil planes read through SRVs (no whole-plane copy, no EDRAM store), a depth
// destination written by PSDepthDirect + 8 PSStencilDirect bit passes (the EDRAM load's own
// passes). Baked default ON (replay md5-identical 1x 4/4, 3x 7/7, all 21 gp1 moves direct; live 3x
// A/B 2026-10-06); =0 keeps the EDRAM store + load A/B.
bool pm4XferDepthEnabled() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_XFER_DEPTH");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;  // C1 stage 3 (3.1 rule 2): the =0 arm is the EDRAM store + load of the oracle unit - a fallback component
#endif
}
namespace {
Microsoft::WRL::ComPtr<ID3D12RootSignature> g_xdRs;
Microsoft::WRL::ComPtr<ID3DBlob> g_xdPs, g_xdPsDepth, g_xdPsStencil, g_xdPsDword;
// the depth destination's scratch dword target (R32_UINT, grown to the largest destination RT)
Microsoft::WRL::ComPtr<ID3D12Resource> g_xdDw;
uint32_t g_xdDwW = 0, g_xdDwH = 0;
int32_t g_xdDwSrv = -1;
D3D12_CPU_DESCRIPTOR_HANDLE g_xdDwRtv{};
D3D12_RESOURCE_STATES g_xdDwState = D3D12_RESOURCE_STATE_RENDER_TARGET;
// SR_XFER_DEPTH_PRED: the 8 stencil-bit predicates (64 B, default heap) and a zero source (upload)
Microsoft::WRL::ComPtr<ID3D12Resource> g_xdPred, g_xdPredZero;
D3D12_RESOURCE_STATES g_xdPredState = D3D12_RESOURCE_STATE_COPY_DEST;
bool g_xdPredFailed = false;
bool pm4XferPredEnabled() {
  static const bool v = [] {
    // baked default ON (replay md5-identical 1x 4/4, 3x 7/7; live 3x fight 8.51 -> 8.38 ms, transfers
    // 1.33 -> 1.16, 2026-10-07); =0 runs every bit pass
    const char* e = std::getenv("SR_XFER_DEPTH_PRED");
    return !e || e[0] != '0';
  }();
  return v && !g_xdPredFailed && g_xdPred;
}
std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> g_xdPsos;
bool g_xdFailed = false, g_xdDepthFailed = false;
uint64_t g_xdMoves = 0;
}  // namespace

// iter-464 resscale: a guest-pixel rectangle (edram::rangeRects) in host pixels.
edram::Rect pm4HostRect(const edram::Rect& r) {
  const resscale::Scale& rs = resscale::get();
  return edram::Rect{r.x * rs.x, r.y * rs.y, r.w * rs.x, r.h * rs.y};
}

Microsoft::WRL::ComPtr<ID3DBlob> pm4CompileHlsl(const char* src, size_t srcLen, const char* name, const char* entry,
                                                 const char* target) {
  Microsoft::WRL::ComPtr<ID3DBlob> code, err;
  // iter-467 freezefx (SR_PM4_HALF_SIGNED, default ON; =0 compiles the old unsigned compare).
  const D3D_SHADER_MACRO defs[] = {{pm4HalfSignedOn() ? nullptr : "SR_HALF_HEXCMP", "1"}, {nullptr, nullptr}};
  const HRESULT hr = hostHlslCompile(src, srcLen, name, defs, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &code, &err);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PM4_RT] %s: %s (%s) compile failed: %s", name, entry, target,
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return nullptr;
  }
  return code;
}

// The frame renderer's passes: the shared pack / unpack text and the direct transfer passes.
static Microsoft::WRL::ComPtr<ID3DBlob> pm4CompileXfer(const char* entry, const char* target) {
  static const std::string src = std::string(kPm4PackHlsl) + kPm4XferHlsl;
  return pm4CompileHlsl(src.data(), src.size(), "pm4_xfer.hlsl", entry, target);
}

// C1 stage 3: the GPU side of the frame renderer - the full-RT triangle's vertex shader every transfer pass uses, the
// render-target SRV band (slot 0 a null SRV; the colour RTs' SRVs, a depth RT's plane SRVs). m_pm4GpuReady = the
// band and the shader exist (the scan-out, the present chain, the native targets and the direct texture loads
// allocate their slots in the band). The development build's oracle unit creates its EDRAM buffer behind this band, lazily,
// at its own consumers (edramGpuInit).
bool D3D12Backend::pm4GpuInit() {
  sr::kernel::MemSiteScope memSite("pm4GpuInit");   // step 4 7d: [MEM_SITE] names (SR_MEM_SITES, diagnostic)
  if (m_pm4GpuReady) return true;
  if (m_pm4GpuFailed || !xlatReady() || !pm4Init()) return false;
  auto fail = [&](const char* what) {
    m_pm4GpuFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PM4_RT] GPU init failed at %s", what);
    return false;
  };
  m_pm4LoadVs = pm4CompileXfer("VSQuad", "vs_5_1");
  if (!m_pm4LoadVs) return fail("shader compile");
  // Band slot 0: a null Texture2D SRV (bound when a pass reads no colour source).
  D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.Texture2D.MipLevels = 1;
  publishXlatSrv(nullptr, &nd, pm4RtSrvCpu(0));
  m_pm4RtSrvUsed = 1;
  // Existing colour RTs (created before the GPU path came up) get their SRVs.
  for (auto& kv : m_pm4Rts) {
    Pm4HostRt& rt = kv.second;
    if (edram::RtKey(kv.first).isDepth()) {
      pm4DepthSrvs(rt, edram::RtKey(kv.first).format());  // iter-466 res4k
      continue;
    }
    if (rt.srv >= 0 || m_pm4RtSrvUsed >= pm4RtSrvCapacity()) continue;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = nd;
    sd.Format = rt.fmt;
    rt.srv = int32_t(m_pm4RtSrvUsed++);
    publishXlatSrv(rt.res, &sd, pm4RtSrvCpu(uint32_t(rt.srv)));
  }
  m_pm4GpuReady = true;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_RT] GPU ready: the RT SRV band (%u slots) and the transfer passes' VS",
              pm4RtSrvCapacity());
  // release/emu-stage3 (Astra round 1 P2): the oracle unit's EDRAM buffer is NOT created here - the dependency is
  // one-way (edramGpuInit needs this band; nothing here needs the buffer) and the model's consumers create it at
  // their first use (edramStoreTiles, pm4MoveTiles' buffer path, pm4TraceInit, edramGpuUploadPending). Before, the
  // two initializers entered each other: edramGpuInit -> pm4GpuInit -> edramGpuInit allocated the buffer and compiled
  // its passes twice, and a shared-memory init created the model eagerly.
  return true;
}


// Step 4 item 7a (SR_PM4_EARLY_INIT=1, default OFF; =0 / unset = the lazy first use inside the first frame): the
// one-time GPU setup the guest's FIRST swap paid for on the CP - 0.9 s in the boot -> intro hand-off, the FMV state
// machine's lock held across BlockUntilIdle meanwhile (archive/step4_claude_20261018/timing NOTES 2026-10-20 02:32:
// 64-78 % of the CP's hand-off second compiled pm4_xfer / pm4_edram HLSL, 4-13 % in pm4ShmInit's driver calls). The
// CP calls this once after WP1, before its first command (kernel/gpu_submit.cpp cpMain), on the thread that renders
// the synchronous first frame, so the state is the same the lazy path would build, only earlier. Each part keeps its
// own gate (SR_PM4_XFER_DIRECT / _DEPTH, the shared memory, SR_PM4_EDRAM_GPU); a part that fails here is simply built
// (or failed) again at its first use, as before.
bool D3D12Backend::pm4EarlyInit() {
  const auto t0 = std::chrono::steady_clock::now();
  auto ms = [&] {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  bool ok = pm4Init() && pm4GpuInit();  // the staging buffers, the RT SRV band, VSQuad
  const double tGpu = ms();
  bool xfer = false, depth = false, shm = false, edramGpu = false;
  if (ok && pm4XferDirectEnabled()) {
    pm4XferDirectPso(DXGI_FORMAT_UNKNOWN);  // the root signature + PSColorDirect (a format's PSO stays lazy)
    xfer = g_xdRs && g_xdPs;
    if (xfer && pm4XferDepthEnabled())  // PSDepthFromDw / PSStencilFromDw / PSDwordDirect + the R32_UINT dword PSO
      depth = pm4XferDirectDepthPso(DXGI_FORMAT_UNKNOWN, 3u, 0u) != nullptr;
  }
  const double tXfer = ms();
  if (ok && pm4ShmEnabled()) shm = pm4ShmInit();
  const double tShm = ms();
#if SR_ORACLE
  if (ok && pm4GpuEdramEnabled()) edramGpu = edramGpuInit();  // the oracle unit's store CS + load PS
#endif
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_EARLY_INIT] SR_PM4_EARLY_INIT (default on): the frame renderer's one-time GPU setup before the first command "
              "in %.1f ms (pm4 + GPU band %s %.1f ms, transfer passes %s / depth %s %.1f ms, shared memory %s %.1f ms, "
              "EDRAM passes %s %.1f ms)",
              ms(), ok ? "ok" : "FAILED", tGpu, xfer ? "ok" : "-", depth ? "ok" : "-", tXfer - tGpu,
              shm ? "ok" : "-", tShm - tXfer, edramGpu ? "ok" : "-", ms() - tShm);
  return ok;
}

// iter-466 res4k: SR_PM4_XFER_DIRECT eligibility - the same answer for the store (skip) and the
// load (direct pass) of one move list: a colour source with an SRV, a colour destination; with
// SR_PM4_XFER_DEPTH (round 2) also a depth source (its plane SRVs) and / or a depth destination.
bool D3D12Backend::pm4XferDirectOk(const Pm4Move& mv) {
  if (!pm4XferDirectEnabled() || g_xdFailed || mv.src.empty() || mv.dst.empty() || mv.src.v == mv.dst.v)
    return false;
  const bool sd = mv.src.isDepth(), dd = mv.dst.isDepth();
  if ((sd || dd) && (!pm4XferDepthEnabled() || g_xdDepthFailed)) return false;
  Pm4HostRt* src = pm4Rt(mv.src);
  Pm4HostRt* dst = pm4Rt(mv.dst);
  if (!src || !dst || src == dst || (sd ? src->dsrv < 0 : src->srv < 0)) return false;
  if (dd) {
    pm4XferDirectPso(DXGI_FORMAT_UNKNOWN);  // builds the root signature (+ colour PS) only
    return g_xdRs && pm4XferDirectDepthPso(dst->fmt, 1, 0) != nullptr &&
           pm4XferDirectDepthPso(dst->fmt, 3, 0) != nullptr && pm4XferDwEnsure(dst->w, dst->h) &&
           pm4XferPredEnsure();
  }
  return pm4XferDirectPso(dst->fmt) != nullptr;
}

// iter-466 res4k round 2: the depth destination's scratch dword target, at least w x h host
// pixels (fresh RTV + SRV slots on growth: the old ones may still be in flight).
bool D3D12Backend::pm4XferDwEnsure(uint32_t w, uint32_t h) {
  if (g_xdDw && w <= g_xdDwW && h <= g_xdDwH) return true;
  if (m_pm4RtSrvUsed + 1u > pm4RtSrvCapacity() || m_pm4RtvUsed + 1u > kPm4RtvSlots) return false;
  if (g_xdDw) m_pm4Graveyard.push_back(g_xdDw);
  g_xdDw.Reset();
  const uint32_t nw = std::max(w, g_xdDwW), nh = std::max(h, g_xdDwH);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = nw;
  rd.Height = nh;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_R32_UINT;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  g_xdDwState = D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, g_xdDwState, nullptr,
                                               IID_PPV_ARGS(&g_xdDw))))
    return false;
  g_xdDw->SetName(L"PM4_XFER_DEPTH_DWORDS");
  g_xdDwW = nw;
  g_xdDwH = nh;
  g_xdDwRtv = m_pm4RtvHeap->GetCPUDescriptorHandleForHeapStart();
  g_xdDwRtv.ptr += SIZE_T(m_pm4RtvUsed++) * m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_device->CreateRenderTargetView(g_xdDw.Get(), nullptr, g_xdDwRtv);
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = DXGI_FORMAT_R32_UINT;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  g_xdDwSrv = int32_t(m_pm4RtSrvUsed++);
  publishXlatSrv(g_xdDw.Get(), &sd, pm4RtSrvCpu(uint32_t(g_xdDwSrv)));
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_XFER_DIRECT] depth dword scratch %ux%u", nw, nh);
  return true;
}

// iter-466 res4k round 2 (SR_XFER_DEPTH_PRED): the stencil-bit predicate buffer + its zero source.
bool D3D12Backend::pm4XferPredEnsure() {
  if (g_xdPred) return true;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = 64;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  g_xdPredState = D3D12_RESOURCE_STATE_COPY_DEST;
  D3D12_HEAP_PROPERTIES up{};
  up.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC zd = bd;
  zd.Flags = D3D12_RESOURCE_FLAG_NONE;
  void* mp = nullptr;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, g_xdPredState, nullptr,
                                               IID_PPV_ARGS(&g_xdPred))) ||
      FAILED(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &zd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&g_xdPredZero))) ||
      FAILED(g_xdPredZero->Map(0, nullptr, &mp)) || !mp) {
    g_xdPred.Reset();
    g_xdPredZero.Reset();
    g_xdPredFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PM4_XFER_DIRECT] stencil predicates failed - every bit pass runs");
    return false;
  }
  std::memset(mp, 0, 64);
  g_xdPredZero->Unmap(0, nullptr);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_XFER_DIRECT] stencil-bit passes predicated (zero bits skipped)");
  return true;
}

// iter-466 res4k round 2: a depth RT's two plane SRVs (depth, stencil) in the RT SRV band.
void D3D12Backend::pm4DepthSrvs(Pm4HostRt& rt, uint32_t depthFmt) {
  if (rt.dsrv >= 0 || !rt.res || m_pm4RtSrvUsed + 2u > pm4RtSrvCapacity()) return;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  const bool f32 = depthFmt == 1u;
  rt.dsrv = int32_t(m_pm4RtSrvUsed);
  m_pm4RtSrvUsed += 2u;
  sd.Format = f32 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  sd.Texture2D.PlaneSlice = 0;
  publishXlatSrv(rt.res, &sd, pm4RtSrvCpu(uint32_t(rt.dsrv)));
  sd.Format = f32 ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
  sd.Texture2D.PlaneSlice = 1;
  publishXlatSrv(rt.res, &sd, pm4RtSrvCpu(uint32_t(rt.dsrv) + 1u));
}

// kind 1: PSDepthFromDw (depth written, ALWAYS); kind 2: PSStencilFromDw for one stencil bit;
// kind 3: PSDwordDirect into the R32_UINT scratch (dsvFmt ignored).
ID3D12PipelineState* D3D12Backend::pm4XferDirectDepthPso(DXGI_FORMAT dsvFmt, uint32_t kind, uint32_t bit) {
  if (g_xdDepthFailed || !g_xdRs) return nullptr;
  const uint32_t key = 0x80000000u | uint32_t(dsvFmt) | (kind << 8) | (bit << 12);
  auto it = g_xdPsos.find(key);
  if (it != g_xdPsos.end()) return it->second.Get();
  auto fail = [&](const char* what) -> ID3D12PipelineState* {
    g_xdDepthFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PM4_XFER_DIRECT] depth %s failed - depth transfers stay on EDRAM", what);
    return nullptr;
  };
  if (!g_xdPsDepth) {
    g_xdPsDepth = pm4CompileXfer("PSDepthFromDw", "ps_5_1");
    g_xdPsStencil = pm4CompileXfer("PSStencilFromDw", "ps_5_1");
    g_xdPsDword = pm4CompileXfer("PSDwordDirect", "ps_5_1");
    if (!g_xdPsDepth || !g_xdPsStencil || !g_xdPsDword) return fail("PS compile");
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_XFER_DIRECT] depth on: depth sources / destinations go direct");
  }
  if (kind == 3u) dsvFmt = DXGI_FORMAT_UNKNOWN;
  const uint32_t key3 = 0x80000000u | uint32_t(dsvFmt) | (kind << 8) | (bit << 12);
  if (kind == 3u) {
    auto it3 = g_xdPsos.find(key3);
    if (it3 != g_xdPsos.end()) return it3->second.Get();
  }
  ID3DBlob* ps = kind == 1u ? g_xdPsDepth.Get() : kind == 2u ? g_xdPsStencil.Get() : g_xdPsDword.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = g_xdRs.Get();
  pd.VS = {m_pm4LoadVs->GetBufferPointer(), m_pm4LoadVs->GetBufferSize()};
  pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.SampleDesc.Count = 1;
  pd.DSVFormat = dsvFmt;
  D3D12_DEPTH_STENCIL_DESC& ds = pd.DepthStencilState;
  if (kind == 3u) {
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  } else if (kind == 1u) {  // the EDRAM load's kind-1 state
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  } else {  // kind 2
    ds.DepthEnable = FALSE;
    ds.StencilEnable = TRUE;
    ds.StencilReadMask = 0xFF;
    ds.StencilWriteMask = UINT8(1u << bit);
    D3D12_DEPTH_STENCILOP_DESC op{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE,
                                  D3D12_COMPARISON_FUNC_ALWAYS};
    ds.FrontFace = op;
    ds.BackFace = op;
  }
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  if (FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) return fail("pso");
  return g_xdPsos.emplace(kind == 3u ? key3 : key, pso).first->second.Get();
}

ID3D12PipelineState* D3D12Backend::pm4XferDirectPso(DXGI_FORMAT fmt, bool masked) {
  if (g_xdFailed) return nullptr;
  auto it = g_xdPsos.find(uint32_t(fmt) | (masked ? 0x10000u : 0u));
  if (it != g_xdPsos.end()) return it->second.Get();
  auto fail = [&](const char* what) -> ID3D12PipelineState* {
    g_xdFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PM4_XFER_DIRECT] %s failed - EDRAM transfers stay", what);
    return nullptr;
  };
  if (!g_xdRs) {
    D3D12_DESCRIPTOR_RANGE range{}, range3{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range3 = range;
    range3.BaseShaderRegister = 3;  // iter-466 round 2: a depth source's stencil plane
    D3D12_ROOT_PARAMETER p[5]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 16;
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u1: the stencil-bit predicates
    p[4].Descriptor.ShaderRegister = 1;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.ShaderRegister = 1;
    p[1].Constants.Num32BitValues = 4;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &range;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[3].DescriptorTable.NumDescriptorRanges = 1;
    p[3].DescriptorTable.pDescriptorRanges = &range3;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 5;
    rd.pParameters = p;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&g_xdRs))))
      return fail("root signature");
    g_xdPs = pm4CompileXfer("PSColorDirect", "ps_5_1");
    if (!g_xdPs || !m_pm4LoadVs) return fail("PSColorDirect compile");
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_XFER_DIRECT] on: colour transfers read the source RT");
  }
  if (fmt == DXGI_FORMAT_UNKNOWN) return nullptr;  // iter-466 round 2: the root-signature probe
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = g_xdRs.Get();
  pd.VS = {m_pm4LoadVs->GetBufferPointer(), m_pm4LoadVs->GetBufferSize()};
  pd.PS = {g_xdPs->GetBufferPointer(), g_xdPs->GetBufferSize()};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.SampleDesc.Count = 1;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = fmt;
  if (masked) {  // B1b (SR_NB_COVER_FILL): only where the draw's scratch stencil is still 0
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = TRUE;
    pd.DepthStencilState.StencilReadMask = 0xFF;
    pd.DepthStencilState.StencilWriteMask = 0;
    D3D12_DEPTH_STENCILOP_DESC o{};
    o.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    o.StencilFailOp = o.StencilPassOp = o.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = o;
  }
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  if (FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
    if (masked) return nullptr;  // B1b only: the moves keep their order
    return fail("pso");
  }
  return g_xdPsos.emplace(uint32_t(fmt) | (masked ? 0x10000u : 0u), pso).first->second.Get();
}

// B1b (SR_NB_COVER_FILL): one D24S8 scratch per host size (its stencil is the coverage mark), in DEPTH_WRITE for
// good; a DSV slot of the PM4 heap.
D3D12Backend::NbFillScratch* D3D12Backend::nbFillScratch(uint32_t w, uint32_t h) {
  const uint64_t key = (uint64_t(w) << 32) | h;
  auto it = m_nbFillScratch.find(key);
  if (it != m_nbFillScratch.end()) return it->second.res ? &it->second : nullptr;
  NbFillScratch& s = m_nbFillScratch[key];
  if (m_pm4DsvUsed >= kPm4DsvSlots) return nullptr;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
  D3D12_CLEAR_VALUE cv{};
  cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                               IID_PPV_ARGS(&s.res)))) {
    s.res.Reset();
    return nullptr;
  }
  s.res->SetName(L"NB_FILL_STENCIL");
  s.dsv = m_pm4DsvHeap->GetCPUDescriptorHandleForHeapStart();
  s.dsv.ptr += SIZE_T(m_pm4DsvUsed++) * m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
  dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  m_device->CreateDepthStencilView(s.res.Get(), &dd, s.dsv);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_FILL] coverage stencil %ux%u (DSV %u)", w, h, m_pm4DsvUsed - 1u);
  return &s;
}

// B1b: pm4GpuLoad's direct colour transfer, under the scratch stencil's EQUAL 0 test (every move here passed
// pm4XferDirectOk into a colour destination, and its masked PSO exists).
void D3D12Backend::nbFillLoad(const std::vector<Pm4Move>& moves, D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
  static uint64_t s_moves = 0, s_tiles = 0;
  pm4XlatHeaps();  // step 4 lane C 2i: this pass's tables are m_xlatSrvHeap's (ERROR 708 / GBV 935 without)
  for (const Pm4Move& mv : moves) {
    g_nbUni.erase(mv.dst.v);  // B1c: a move into a key rewrites it (as pm4LoadTiles)
    Pm4HostRt* rt = pm4Rt(mv.dst);
    Pm4HostRt* src = pm4Rt(mv.src);
    ID3D12PipelineState* pso = rt ? pm4XferDirectPso(rt->fmt, true) : nullptr;
    edram::Rect rects[edram::kRangeRectsMax];
    const int n = rt ? edram::rangeRects(mv.dst, mv.start, mv.end, rects) : 0;
    if (!src || !pso || !n) continue;
    const bool sd = mv.src.isDepth();
    pm4Transition(*src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pm4Transition(*rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->SetGraphicsRootSignature(g_xdRs.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->SetPipelineState(pso);
    m_commandList->OMSetRenderTargets(1, &rt->view, FALSE, &dsv);
    m_commandList->OMSetStencilRef(0);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(rt->w), float(rt->h), 0.0f, 1.0f};
    m_commandList->RSSetViewports(1, &vp);
    const uint32_t sc4[4] = {mv.src.base(), mv.src.pitchTiles(), mv.src.msaa(),
                             (sd ? 1u : 0u) | (mv.src.intsLog2() << 1) | ((mv.src.format() & 15u) << 8)};
    m_commandList->SetGraphicsRoot32BitConstants(1, 4, sc4, 0);
    const uint32_t s0 = uint32_t(sd ? src->dsrv : src->srv);
    m_commandList->SetGraphicsRootDescriptorTable(2, pm4RtSrvGpu(s0));
    m_commandList->SetGraphicsRootDescriptorTable(3, pm4RtSrvGpu(sd ? s0 + 1u : s0));
    for (int i = 0; i < n; i++) {
      const edram::Rect r = pm4HostRect(rects[i]);
      const D3D12_RECT sc{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
      m_commandList->RSSetScissorRects(1, &sc);
      const Pm4XferConsts c = pm4XferConsts(mv.dst, r);
      m_commandList->SetGraphicsRoot32BitConstants(0, 16, &c, 0);
      m_commandList->DrawInstanced(3, 1, 0, 0);
    }
    s_tiles += mv.end - mv.start;
    if (++s_moves <= 16 || (s_moves & (s_moves - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NB_FILL] %08X -> %08X tiles [%u,%u) after the draw, stencil-masked (moves %llu, tiles %llu)",
                  mv.src.v, mv.dst.v, mv.start, mv.end, (unsigned long long)s_moves, (unsigned long long)s_tiles);
  }
  for (const Pm4Move& mv : moves) g_pm4Stats.tilesMoved += mv.end - mv.start;
  g_pm4Stats.transfers += uint32_t(moves.size());
}

// C1 stage 3: the shipping executor of a claim's moves (docs/RELEASE_COMPOSITION.md 8.2). A move the direct passes
// take (pm4XferDirectOk: the source and the destination have host RTs and their PSOs exist) runs as one pass source
// RT -> destination RT; a load of nobody's tiles was turned into a host clear by nbZeroLoads (SR_NB_ZERO_INIT). In the
// dev build the oracle unit takes what is left - the EDRAM buffer's store + load (SR_PM4_EDRAM_GPU) or the CPU mirror
// (=0) - and the native meter counts it as edram_xfer; in the release configuration a move the direct passes cannot
// take stops the renderer (3.1 rule 4: [SURFACE_MOVE_BAD], exit 3), as [NATIVE_FLUSH_BAD] stops the consumer.
void D3D12Backend::pm4MoveTiles(const std::vector<Pm4Move>& moves) {
  if (moves.empty()) return;
  if (!g_nbUni.empty())  // B1c (SR_NB_CLEAR_PROP): a move into a key rewrites its tiles
    for (const Pm4Move& mv : moves) g_nbUni.erase(mv.dst.v);
#if SR_ORACLE
  if (!pm4GpuEdramEnabled() || !edramGpuInit()) {  // SR_PM4_EDRAM_GPU=0 (or the buffer failed): the CPU mirror path
    edramStoreTiles(moves);
    edramLoadTiles(moves);
    for (const Pm4Move& mv : moves) {  // NATIVE METER: every move went through the word model (edram_xfer)
      m_meterCur.bufferMoves++;
      m_meterCur.bufferMoveTiles += mv.end - mv.start;
    }
    return;
  }
  edramGpuStore(moves);  // the sources of the moves the direct passes do not take, into the buffer (and a pending
                         // snapshot upload)
#else
  if (!pm4GpuInit()) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[SURFACE_MOVE_BAD] the render-target SRV band is not ready: %zu moves", moves.size());
    sr::Log::shutdown();
    std::_Exit(3);
  }
#endif
  pm4GpuLoad(moves);
}

void D3D12Backend::pm4GpuLoad(const std::vector<Pm4Move>& moves) {
#if SR_ORACLE
  edramGpuUploadPending();  // a mirror snapshot restored on the CPU (replay start) reaches the buffer first
#endif
  pm4XlatHeaps();  // step 4 lane C 2i: this pass's tables are m_xlatSrvHeap's (ERROR 708 / GBV 935 without)
  bool bound = false;
  for (const Pm4Move& mv : moves) {
    Pm4HostRt* rt = pm4Rt(mv.dst);
    if (!rt) continue;
    edram::Rect rects[edram::kRangeRectsMax];
    const int n = edram::rangeRects(mv.dst, mv.start, mv.end, rects);
    if (!n) continue;
    if (pm4XferDirectOk(mv)) {  // iter-466 res4k: one pass from the source host RT
      Pm4HostRt* src = pm4Rt(mv.src);
      const bool sd = mv.src.isDepth(), dd = mv.dst.isDepth();  // round 2: SR_PM4_XFER_DEPTH
      ID3D12PipelineState* pso = dd ? pm4XferDirectDepthPso(rt->fmt, 1, 0) : pm4XferDirectPso(rt->fmt);
      if (src && pso) {
        pm4Transition(*src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        pm4Transition(*rt, dd ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->SetGraphicsRootSignature(g_xdRs.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->SetPipelineState(pso);
        if (dd)
          m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &rt->view);
        else
          m_commandList->OMSetRenderTargets(1, &rt->view, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0.0f, 0.0f, float(rt->w), float(rt->h), 0.0f, 1.0f};
        m_commandList->RSSetViewports(1, &vp);
        const uint32_t sc4[4] = {mv.src.base(), mv.src.pitchTiles(), mv.src.msaa(),
                                 (sd ? 1u : 0u) | (mv.src.intsLog2() << 1) | ((mv.src.format() & 15u) << 8)};
        m_commandList->SetGraphicsRoot32BitConstants(1, 4, sc4, 0);
        const uint32_t s0 = uint32_t(sd ? src->dsrv : src->srv);
        m_commandList->SetGraphicsRootDescriptorTable(2, pm4RtSrvGpu(s0));
        m_commandList->SetGraphicsRootDescriptorTable(3, pm4RtSrvGpu(sd ? s0 + 1u : s0));
        if (dd) {
          // step 1: the destination's dwords into the scratch target, every rect
          ID3D12PipelineState* dwPso = pm4XferDirectDepthPso(rt->fmt, 3, 0);
          if (!dwPso || !pm4XferDwEnsure(rt->w, rt->h)) {
            g_xdDepthFailed = true;  // the moves after this one take the EDRAM path
            continue;
          }
          if (g_xdDwState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            pm4Barrier(m_commandList.Get(), g_xdDw.Get(), g_xdDwState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g_xdDwState = D3D12_RESOURCE_STATE_RENDER_TARGET;
          }
          // the dword pass always writes the 8 stencil-bit flags (u1): zero them first; only
          // SR_XFER_DEPTH_PRED uses them (pm4XferDirectOk made sure the buffer exists)
          const bool pred = pm4XferPredEnabled();
          {
            if (g_xdPredState != D3D12_RESOURCE_STATE_COPY_DEST) {
              pm4Barrier(m_commandList.Get(), g_xdPred.Get(), g_xdPredState, D3D12_RESOURCE_STATE_COPY_DEST);
              g_xdPredState = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            m_commandList->CopyBufferRegion(g_xdPred.Get(), 0, g_xdPredZero.Get(), 0, 64);
            pm4Barrier(m_commandList.Get(), g_xdPred.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_xdPredState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          }
          if (g_xdPred) m_commandList->SetGraphicsRootUnorderedAccessView(4, g_xdPred->GetGPUVirtualAddress());
          m_commandList->OMSetRenderTargets(1, &g_xdDwRtv, FALSE, nullptr);
          m_commandList->SetPipelineState(dwPso);
          for (int i = 0; i < n; i++) {
            const edram::Rect r = pm4HostRect(rects[i]);
            const D3D12_RECT sc{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
            m_commandList->RSSetScissorRects(1, &sc);
            const Pm4XferConsts c = pm4XferConsts(mv.dst, r);
            m_commandList->SetGraphicsRoot32BitConstants(0, 16, &c, 0);
            m_commandList->DrawInstanced(3, 1, 0, 0);
          }
          pm4Barrier(m_commandList.Get(), g_xdDw.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          g_xdDwState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          if (pred) {
            pm4Barrier(m_commandList.Get(), g_xdPred.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_PREDICATION);
            g_xdPredState = D3D12_RESOURCE_STATE_PREDICATION;
          }
          m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &rt->view);
          m_commandList->SetGraphicsRootDescriptorTable(3, pm4RtSrvGpu(uint32_t(g_xdDwSrv)));
        }
        for (int i = 0; i < n; i++) {
          const edram::Rect r = pm4HostRect(rects[i]);
          const D3D12_RECT sc{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
          m_commandList->RSSetScissorRects(1, &sc);
          Pm4XferConsts c = pm4XferConsts(mv.dst, r);
          m_commandList->SetGraphicsRoot32BitConstants(0, 16, &c, 0);
          if (!dd) {
            m_commandList->DrawInstanced(3, 1, 0, 0);
            continue;
          }
          // the EDRAM load's depth sequence: stencil cleared in the rect, depth, then one pass per
          // stencil bit (no PS stencil-ref export on every GPU)
          m_commandList->ClearDepthStencilView(rt->view, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 1, &sc);
          m_commandList->SetPipelineState(pso);
          m_commandList->DrawInstanced(3, 1, 0, 0);
          m_commandList->OMSetStencilRef(0xFF);
          static const bool s_noSt = [] {  // DIAGNOSTIC ONLY (wrong stencil): the bit passes' cost
            const char* e = std::getenv("SR_XFER_DEPTH_NOSTENCIL");
            return e && e[0] == '1';
          }();
          for (uint32_t bit = 0; bit < 8 && !s_noSt; bit++) {
            ID3D12PipelineState* p = pm4XferDirectDepthPso(rt->fmt, 2, bit);
            if (!p) continue;
            c.bit = bit;
            m_commandList->SetGraphicsRoot32BitConstants(0, 16, &c, 0);
            m_commandList->SetPipelineState(p);
            const bool predOn = g_xdPredState == D3D12_RESOURCE_STATE_PREDICATION && pm4XferPredEnabled();
            if (predOn)  // skipped when no pixel of the move has stencil bit `bit` (the clear left 0)
              m_commandList->SetPredication(g_xdPred.Get(), uint64_t(bit) * 8u, D3D12_PREDICATION_OP_EQUAL_ZERO);
            m_commandList->DrawInstanced(3, 1, 0, 0);
            if (predOn) m_commandList->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
          }
        }
        bound = false;  // the next EDRAM move re-binds the load root signature
        if (++g_xdMoves <= 8 || (g_xdMoves & (g_xdMoves - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_XFER_DIRECT] #%llu %08X -> %08X tiles [%u,%u)",
                      (unsigned long long)g_xdMoves, mv.src.v, mv.dst.v, mv.start, mv.end);
        continue;
      }
    }
#if SR_ORACLE
    edramGpuLoadMove(mv, *rt, rects, n, bound);  // the oracle unit: the buffer's words into the destination RT
#else
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[SURFACE_MOVE_BAD] a surface move the direct passes cannot take: %08X -> %08X tiles [%u,%u) "
                "(src rt %d, dst depth %d, direct %d, depth passes %d)",
                mv.src.v, mv.dst.v, mv.start, mv.end, pm4Rt(mv.src) ? 1 : 0, mv.dst.isDepth() ? 1 : 0,
                pm4XferDirectEnabled() && !g_xdFailed ? 1 : 0, pm4XferDepthEnabled() && !g_xdDepthFailed ? 1 : 0);
    sr::Log::shutdown();
    std::_Exit(3);
#endif
  }
  for (const Pm4Move& mv : moves) g_pm4Stats.tilesMoved += mv.end - mv.start;
  g_pm4Stats.transfers += uint32_t(moves.size());
}



}  // namespace gfx
}  // namespace sr
