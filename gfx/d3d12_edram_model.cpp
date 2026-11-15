// The EDRAM WORD model: the oracle half of the PM4 frame renderer (gfx/d3d12_frame_render.cpp), the development build's
// oracle unit (EMULATION_SOURCES; docs/RELEASE_COMPOSITION.md section 8, stage 3). Xenia's model (contract
// docs/XENIA_EDRAM_CONTRACT.md):
//   - the tile ownership map over the 2048 EDRAM tiles (edram::Ownership; the shipping path decides with the surface
//     table of gfx/nb_surfaces.hpp - under SR_NB_SURF_VERIFY the two run side by side and must agree, with
//     SR_NB_SURF_TABLE=0 the map decides);
//   - the 10 MiB EDRAM buffer (SR_PM4_EDRAM_GPU) with its store (compute: host RT -> guest words, every sample)
//     and load (quad: words -> host RT) passes, and the CPU mirror (=0: the mirror alone; the trace snapshot,
//     SR_PM4_EDRAM_DUMP);
//   - the EDRAM-path resolve: the span's owners stored into the buffer, then Xenia's resolve CS
//     (gfx/d3d12_pm4_shmem.cpp pm4ShmResolve) or the CPU resolve from the mirror (edram::resolveToGuest), with
//     SR_PM4_ORACLE's compare against Xenia's dumps and SR_NATIVE_RESOLVE_DUAL / _VERIFY beside the native one.
// The frame renderer reaches it only under SR_ORACLE (gfx/d3d12_backend.hpp); the release configuration has no
// fallback into it (3.1 rule 4: [SURFACE_MOVE_BAD], [NATIVE_RESOLVE_BAD]). The helpers both units share are
// gfx/pm4_render_detail.hpp. Split out of gfx/d3d12_pm4_edram.cpp by archive/s3/s3_cut.ps1 (release/emu-stage3):
// the regions surf-verify, store-tiles, claim-trace-load-oracle, resolve, edram-hlsl, gpu-init, gpu-load, sync-mirror, bodies unchanged.

#include "gamma_ramp.hpp"
#include "par.hpp"
#include "cp_simd.hpp"
#include "../kernel/gpu_write_watch.hpp"
#include "../kernel/hitch_log.hpp"
#include "../kernel/native_meter.hpp"
#include "../kernel/ab_toggle.hpp"
#include "../kernel/sr_oracle.hpp"
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
#include "edram_model.hpp"        // the word model: the tile map, the sample addressing, the CPU resolve
#include "pm4_render_detail.hpp"  // the helpers shared with the frame renderer
#include "nb_surfaces.hpp"
#include "native_targets.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/phys_window.hpp"
#include "res_scale.hpp"
#include "shader_cache.hpp"
#include "xenos_state.hpp"
#include "pm4_live_trace.hpp"
#include "pm4_half_gate.hpp"
#include "native_draw.hpp"

#if !SR_ORACLE
#error "gfx/d3d12_edram_model.cpp is the oracle unit: it is compiled only with SR_ORACLE=1 (EMULATION_SOURCES)"
#endif

namespace sr {
namespace gfx {

// ---- surf-verify ----
// The tile ownership map (edram::Ownership, Xenia's render_target_cache ownership ranges): the oracle unit's own -
// the backend is a singleton, the map a static of this unit (gfx/d3d12_backend.hpp names no model type).
static edram::Ownership g_pm4Own;
size_t edramOwnRanges() { return g_pm4Own.rangeCount(); }
bool nbSurfVerifyOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_SURF_VERIFY");
    return e && e[0] == '1';
  }();
  return v;
}
uint64_t g_nbSurfClaims = 0, g_nbSurfClaimBad = 0, g_nbSurfQueries = 0, g_nbSurfQueryBad = 0;
void nbSurfReport(const char* what, bool bad, size_t mapRanges) {
  const uint64_t n = g_nbSurfClaims + g_nbSurfQueries;
  if (bad || n == 1 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(bad ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
                "[NB_SURF] verify%s %s: claims=%llu mismatches=%llu | owner queries=%llu mismatches=%llu | surfaces=%zu "
                "map ranges=%zu",
                bad ? " MISMATCH" : "", what, (unsigned long long)g_nbSurfClaims, (unsigned long long)g_nbSurfClaimBad,
                (unsigned long long)g_nbSurfQueries, (unsigned long long)g_nbSurfQueryBad, nbSurfTable().size(),
                mapRanges);
}

// ---- store-tiles ----
// Store: every source's pixels for its moved tiles -> CPU mirror (guest bits,
// all MSAA samples replicated). One GPU sync for the whole batch of moves.
// iter-464 resscale: the CPU mirror is the guest's 1:1 EDRAM; with a draw
// resolution scale only the GPU EDRAM buffer (scaled layout) exists.
static bool pm4CpuMirrorRefused(const char* what) {
  if (!resscale::get().scaled()) return false;
  static std::atomic<uint32_t> s_n{0};
  if (s_n.fetch_add(1) < 8)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[RES_SCALE] %s needs the 1:1 CPU EDRAM mirror - skipped at %ux%u", what,
                resscale::get().x, resscale::get().y);
  return true;
}

void D3D12Backend::edramStoreTiles(const std::vector<Pm4Move>& moves) {
  if (pm4GpuEdramEnabled() && edramGpuInit()) {
    edramGpuStore(moves);
    return;
  }
  if (pm4CpuMirrorRefused("CPU EDRAM store")) return;
  struct Job {
    edram::RtKey key;
    edram::Rect rect;
    uint64_t off;
    uint32_t rowPitch;
    uint32_t originX, originY;  // rect origin inside the copied region
    Pm4DepthPlanes planes{};    // depth: both planes (depth + stencil)
  };
  std::vector<Job> jobs;
  std::vector<uint32_t> depthCopied;   // depth keys already read back whole
  std::vector<Pm4DepthPlanes> depthPlanes;
  uint64_t cursor = 0;
  for (const Pm4Move& mv : moves) {
    if (mv.src.empty()) continue;
    Pm4HostRt* rt = pm4Rt(mv.src);
    if (!rt) continue;
    edram::Rect rects[edram::kRangeRectsMax];
    const int n = edram::rangeRects(mv.src, mv.start, mv.end, rects);
    if (mv.src.isDepth()) {
      // Depth resources copy whole-subresource only; read both planes once.
      size_t di = 0;
      for (; di < depthCopied.size(); di++)
        if (depthCopied[di] == mv.src.v) break;
      if (di == depthCopied.size()) {
        cursor = pm4AlignUp(cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        Pm4DepthPlanes pl;
        if (!pm4DepthFootprints(m_device.Get(), rt->res, cursor, pl) ||
            cursor + pl.total > kPm4StagingBytes)
          continue;
        pm4Transition(*rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT p = 0; p < 2; p++) {
          D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
          dst.pResource = m_pm4Readback.Get();
          dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          dst.PlacedFootprint = pl.fp[p];
          src.pResource = rt->res;
          src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          src.SubresourceIndex = p;
          m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        depthCopied.push_back(mv.src.v);
        depthPlanes.push_back(pl);
        cursor += pl.total;
      }
      for (int i = 0; i < n; i++) {
        Job j{mv.src, rects[i], 0, 0, 0, 0};
        j.planes = depthPlanes[di];
        jobs.push_back(j);
      }
      continue;
    }
    const uint32_t bpt = edram::hostBytesPerTexel(mv.src.hostFormat());
    for (int i = 0; i < n; i++) {
      const edram::Rect& r = rects[i];
      const uint32_t rowPitch = uint32_t(pm4AlignUp(uint64_t(r.w) * bpt, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
      cursor = pm4AlignUp(cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      const uint64_t bytes = uint64_t(rowPitch) * r.h;
      if (cursor + bytes > kPm4StagingBytes) continue;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      fp.Offset = cursor;
      fp.Footprint.Format = rt->fmt;
      fp.Footprint.Width = r.w;
      fp.Footprint.Height = r.h;
      fp.Footprint.Depth = 1;
      fp.Footprint.RowPitch = rowPitch;
      pm4Transition(*rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
      dst.pResource = m_pm4Readback.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp;
      src.pResource = rt->res;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;
      const D3D12_BOX box{r.x, r.y, 0, r.x + r.w, r.y + r.h, 1};
      m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
      jobs.push_back(Job{mv.src, r, cursor, rowPitch, r.x, r.y});
      cursor += bytes;
    }
  }
  if (jobs.empty()) return;
  const auto tf = std::chrono::steady_clock::now();
  pm4Flush();
  const auto tc = std::chrono::steady_clock::now();
  g_pm4Stats.msFlush += std::chrono::duration<double, std::milli>(tc - tf).count();
  struct CpuTimer {
    std::chrono::steady_clock::time_point t0;
    ~CpuTimer() {
      g_pm4Stats.msStoreCpu +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
  } cpuTimer{tc};
  uint8_t* mapped = nullptr;
  const D3D12_RANGE rr{0, SIZE_T(cursor)};
  if (FAILED(m_pm4Readback->Map(0, &rr, reinterpret_cast<void**>(&mapped))) || !mapped) return;
  uint8_t* mirror = m_pm4Edram.data();
  for (const Job& j : jobs) {
    const edram::RtKey k = j.key;
    const edram::HostFmt hf = k.hostFormat();
    const uint32_t bpt = edram::hostBytesPerTexel(hf);
    const uint32_t samples = edram::msaaSampleCount(k.msaa());
    const uint32_t intsLog2 = k.intsLog2();
    // Rows in parallel: each (x, y, sample) owns its own mirror bytes.
    par::parallelFor(0u, j.rect.h, [&](uint32_t y) {
      const uint8_t* row = mapped + j.off + uint64_t(y + j.rect.y - j.originY) * j.rowPitch;
      for (uint32_t x = 0; x < j.rect.w; x++) {
        uint64_t bits;
        if (k.isDepth()) {
          bits = pm4DepthGuestBits(k.format(), mapped, j.planes, j.rect.x + x, j.rect.y + y);
        } else {
          const uint8_t* texel = row + uint64_t(x + j.rect.x - j.originX) * bpt;
          float c[4];
          edram::hostToFloat4(hf, texel, c);
          bits = edram::packColor(k.format(), c);
        }
        const uint32_t lo = uint32_t(bits), hi = uint32_t(bits >> 32);
        for (uint32_t s = 0; s < samples; s++) {
          const uint32_t off = edram::sampleOffsetBytes(j.rect.x + x, j.rect.y + y, k.base(),
                                                         k.pitchTiles(), k.msaa(), k.isDepth(),
                                                         intsLog2, s);
          std::memcpy(mirror + off, &lo, 4);
          if (intsLog2) std::memcpy(mirror + (off + 4) % edram::kEdramBytes, &hi, 4);
        }
      }
    });
  }
  const D3D12_RANGE wr{0, 0};
  m_pm4Readback->Unmap(0, &wr);
}

// ---- claim-trace-load-oracle ----
// The development build's claim: the surface table's answer (pm4ClaimTable) or the tile map's (SR_NB_SURF_TABLE=0), and under
// SR_NB_SURF_VERIFY the other structure beside it - the two answers must be identical (nbSurfReport).
void D3D12Backend::edramClaim(edram::RtKey k, uint32_t start, uint32_t end, std::vector<Pm4Move>& moves) {
  const uint32_t len = end - start;
  const bool fast = nbClaimFastOn();
  // The tile map's decision (edram::Ownership): loads of the unowned runs first, then the previous owners' runs.
  auto mapClaim = [&](std::vector<Pm4Move>& out) {
    if (fast && (end <= edram::kTileCount ? g_pm4Own.ownedBy(k, start, end)
                                          : g_pm4Own.ownedBy(k, start, edram::kTileCount) &&
                                                g_pm4Own.ownedBy(k, 0, end - edram::kTileCount)))
      return;
    // Tiles nobody owns hold whatever the mirror holds (zero at boot, the
    // trace's EDRAM snapshot on replay): load them like a transfer from nowhere.
    thread_local std::vector<edram::Transfer> t_prev, t_tr;  // reused: no allocation per claim
    std::vector<edram::Transfer>& prev = t_prev;
    prev.clear();
    if (end <= edram::kTileCount) {
      g_pm4Own.owners(start, end, prev);
    } else {
      g_pm4Own.owners(start, edram::kTileCount, prev);
      g_pm4Own.owners(0, end - edram::kTileCount, prev);
    }
    for (const auto& p : prev)
      if (p.source.empty()) out.push_back(Pm4Move{edram::RtKey(), k, p.start, p.end});
    std::vector<edram::Transfer>& tr = t_tr;
    tr.clear();
    g_pm4Own.claim(k, start, len, tr);
    for (const auto& x : tr) out.push_back(Pm4Move{x.source, k, x.start, x.end});
  };
  // SR_NB_SURF_TABLE: the table decides (the map, kept only under SR_NB_SURF_VERIFY, is the oracle); otherwise the
  // map decides and SR_NB_SURF_VERIFY runs the table beside it. Either way the two answers must be identical.
  const bool table = nbSurfTableOn(), verify = nbSurfVerifyOn();
  const size_t m0 = moves.size();
  if (table) pm4ClaimTable(k, start, end, moves);
  else mapClaim(moves);
  if (!verify) return;
  thread_local std::vector<Pm4Move> t_other;
  t_other.clear();
  if (table) mapClaim(t_other);
  else pm4ClaimTable(k, start, end, t_other);
  g_nbSurfClaims++;
  bool same = moves.size() - m0 == t_other.size();
  for (size_t i = 0; same && i < t_other.size(); i++) {
    const Pm4Move& a = moves[m0 + i];
    const Pm4Move& b = t_other[i];
    same = a.src == b.src && a.dst == b.dst && a.start == b.start && a.end == b.end;
  }
  if (!same && ++g_nbSurfClaimBad <= 16) {
    std::string ma, ta;
    char buf[64];
    for (size_t i = m0; i < moves.size(); i++) {
      std::snprintf(buf, sizeof buf, " %08X->%08X[%u,%u)", moves[i].src.v, moves[i].dst.v, moves[i].start, moves[i].end);
      ma += buf;
    }
    for (const Pm4Move& m : t_other) {
      std::snprintf(buf, sizeof buf, " %08X->%08X[%u,%u)", m.src.v, m.dst.v, m.start, m.end);
      ta += buf;
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NB_SURF] claim MISMATCH #%llu %s:%s | %s:%s",
                (unsigned long long)g_nbSurfClaimBad, table ? "table" : "map", ma.c_str(), table ? "map" : "table",
                ta.c_str());
  }
  nbSurfReport("claim", !same, g_pm4Own.rangeCount());
}

// The owners of a resolve's source span: the surface table's answer (pm4SpanOwners) or the tile map's
// (SR_NB_SURF_TABLE=0), and under SR_NB_SURF_VERIFY the other structure's beside it (EDRAM zero step 2).
void D3D12Backend::edramSpanOwners(edram::RtKey k, const edram::ResolveInfo& ri,
                                   std::vector<edram::Transfer>& out) {
  const uint32_t yShift = k.msaa() >= edram::kMsaa2X ? 1u : 0u;
  const uint32_t pt = k.pitchTiles();
  const uint32_t row0 = (ri.y0 << yShift) / edram::kTileHeightSamples;
  const uint32_t row1 = ((ri.y1 << yShift) + edram::kTileHeightSamples - 1) / edram::kTileHeightSamples;
  const uint32_t len = std::min((row1 - row0) * pt, edram::kTileCount);
  const uint32_t start = (k.base() + row0 * pt) % edram::kTileCount;
  const uint32_t end = start + len;
  const size_t o0 = out.size();
  const bool table = nbSurfTableOn();  // EDRAM zero step 3: the surface table answers (the map under VERIFY)
  auto mapOwners = [&](std::vector<edram::Transfer>& o) {
    if (end <= edram::kTileCount) {
      g_pm4Own.owners(start, end, o);
    } else {
      g_pm4Own.owners(start, edram::kTileCount, o);
      g_pm4Own.owners(0, end - edram::kTileCount, o);
    }
  };
  if (table) pm4SpanOwners(k, ri, out);
  else mapOwners(out);
  if (!nbSurfVerifyOn()) return;
  std::vector<edram::Transfer> t;  // the other structure's owners of the same span
  if (table) mapOwners(t);
  else pm4SpanOwners(k, ri, t);
  g_nbSurfQueries++;
  bool same = t.size() == out.size() - o0;
  for (size_t i = 0; same && i < t.size(); i++)
    same = t[i].start == out[o0 + i].start && t[i].end == out[o0 + i].end && t[i].source == out[o0 + i].source;
  if (!same && ++g_nbSurfQueryBad <= 16)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NB_SURF] owners MISMATCH #%llu key=%08X span [%u,%u): map %zu runs, table %zu",
                (unsigned long long)g_nbSurfQueryBad, k.v, start, end, out.size() - o0, t.size());
  nbSurfReport("owners", !same, g_pm4Own.rangeCount());
}

// The mirror and the map, at pm4Init (the dev build).
void D3D12Backend::edramModelInit() {
  m_pm4Edram.assign(edram::kEdramBytes, 0);
  g_pm4Own.reset();
}

// SR_PM4_EDRAM_DUMP=<1-based draw ordinals> (+ SR_PM4_EDRAM_DUMP_DIR): after those draws, write the whole EDRAM
// mirror - every owner stored into it - as e<NNNNN>.bin, the counterpart of Xenia's trace_dump_edram_draws. true =
// the store's flush reset the command list (the caller re-binds its root signature).
bool D3D12Backend::edramDumpAfterDraw(const DrawBatch& db) {
  static const std::vector<uint32_t> s_edramDraws = [] {
    std::vector<uint32_t> v;
    if (const char* e = std::getenv("SR_PM4_EDRAM_DUMP")) {
      // Any non-digit separates (replay_ab.ps1 splits its gate list on commas,
      // so use '+': SR_PM4_EDRAM_DUMP=1+2+7).
      const char* p = e;
      while (*p) {
        if (*p < '0' || *p > '9') { p++; continue; }
        char* end = nullptr;
        v.push_back(uint32_t(std::strtoul(p, &end, 10)));
        p = end;
      }
    }
    return v;
  }();
  if (s_edramDraws.empty() || !db.drawOrdinal) return false;
  if (std::find(s_edramDraws.begin(), s_edramDraws.end(), db.drawOrdinal) == s_edramDraws.end()) return false;
  if (pm4CpuMirrorRefused("SR_PM4_EDRAM_DUMP")) return false;
  std::vector<edram::Transfer> owned;
  if (nbSurfTableOn()) nbSurfTable().owners(0, edram::kTileCount, owned);  // EDRAM zero step 3
  else g_pm4Own.owners(0, edram::kTileCount, owned);
  std::vector<Pm4Move> all;
  for (const auto& o : owned)
    if (!o.source.empty()) all.push_back(Pm4Move{o.source, edram::RtKey(), o.start, o.end});
  edramStoreTiles(all);
  if (pm4GpuEdramEnabled() && m_edramGpuReady) edramSyncMirror(0, edram::kTileCount);
  const char* dir = std::getenv("SR_PM4_EDRAM_DUMP_DIR");
  const std::string d = (dir && *dir) ? dir : "archive/pm4_edram";
  CreateDirectoryA(d.c_str(), nullptr);
  char name[32];
  std::snprintf(name, sizeof(name), "/e%05u.bin", db.drawOrdinal);
  if (FILE* f = std::fopen((d + name).c_str(), "wb")) {
    std::fwrite(m_pm4Edram.data(), 1, m_pm4Edram.size(), f);
    std::fclose(f);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_EDRAM] dumped mirror after draw %u (%zu owner ranges)", db.drawOrdinal,
              owned.size());
  return true;  // the store's flush reset the command list
}

void D3D12Backend::pm4TraceInit(livetrace::InitState& out) {
  if (!pm4Init()) return;
  // iter-464 resscale: Xenia InitializeTraceSubmitDownloads returns false when
  // scaled (no 1:1 EDRAM / memory mapping); the trace gets no EDRAM snapshot.
  if (pm4CpuMirrorRefused("the live trace's EDRAM snapshot")) return;
  // Outside a frame the list is closed: open it on this frame's allocator and
  // leave it closed again (pm4ShmSyncToGuest does the same).
  const bool wasClosed = m_clState != ClState::Recording;
  if (wasClosed) {
    clReset(Pm4Reset);
    // The GPU EDRAM store binds descriptor tables: a fresh list has no heaps
    // (the NVIDIA driver dereferences null without them).
    ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
    m_commandList->SetDescriptorHeaps(2, heaps);
  }
  // Every owned tile into the mirror, then the GPU EDRAM into the CPU mirror
  // (the SR_PM4_EDRAM_DUMP sequence).
  std::vector<edram::Transfer> owned;
  if (nbSurfTableOn()) nbSurfTable().owners(0, edram::kTileCount, owned);  // EDRAM zero step 3
  else g_pm4Own.owners(0, edram::kTileCount, owned);
  std::vector<Pm4Move> all;
  for (const auto& o : owned)
    if (!o.source.empty()) all.push_back(Pm4Move{o.source, edram::RtKey(), o.start, o.end});
  edramStoreTiles(all);
  if (pm4GpuEdramEnabled() && edramGpuInit()) edramSyncMirror(0, edram::kTileCount);
  out.edram.assign(m_pm4Edram.begin(), m_pm4Edram.end());
  pm4ShmTraceGpuPages(out.gpuPages);
  {  // iter-463 D8: this trace's in-frame resolves start over
    std::lock_guard<std::recursive_mutex> lk(m_shmMu);
    m_traceResolvedPages.clear();
  }
  // iter-462: every texture the traced frame binds re-validates its pages, so
  // the shared-memory path records them (pm4ShmValidate -> memRead). The
  // one-lookup bind fast path skipped textures validated in earlier frames and
  // Xenia played our traces with those pages zero (dark title/street renders).
  invalidateAllTextures();
  if (wasClosed && m_clState == ClState::Recording) clClose(Pm4Close);
}

void D3D12Backend::pm4RestoreEdram(const uint8_t* snapshot, size_t bytes) {
  if (!pm4Init() || bytes < edram::kEdramBytes) return;
  // iter-464 resscale: Xenia's RestoreEdramSnapshot returns early when the draw
  // resolution is scaled ("No 1:1 mapping", d3d12_render_target_cache.cc:1588) -
  // a scaled replay starts from a zero EDRAM, like Xenia's scaled trace dump.
  if (resscale::get().scaled()) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_EDRAM] snapshot NOT restored: draw resolution scaled (Xenia parity)");
    return;
  }
  // Everything the host RTs hold is superseded: the mirror is the truth and
  // every tile is unowned until a draw or resolve claims (and loads) it.
  std::memcpy(m_pm4Edram.data(), snapshot, edram::kEdramBytes);
  m_pm4GpuUploadPending = true;  // GPU EDRAM: uploaded before the next transfer
  g_pm4Own.reset();
  nbSurfTable().reset();  // EDRAM zero step 2: the surface table mirrors the map
  m_pm4EdramPristine = false;  // SR_NB_ZERO_INIT: unclaimed tiles hold the snapshot now, not zero
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_EDRAM] snapshot restored into the EDRAM mirror");
}

// Load: mirror -> each destination's moved tiles (sample 0), the CPU path (SR_PM4_EDRAM_GPU=0, or the buffer init
// failed). Color goes through a boxed upload copy; depth (whole-subresource copies only) is read back, patched and
// uploaded whole. (The GPU path: pm4MoveTiles - the direct passes, the buffer loads of edramGpuLoadMove.)
void D3D12Backend::edramLoadTiles(const std::vector<Pm4Move>& moves) {
  if (pm4CpuMirrorRefused("CPU EDRAM load")) return;
  const uint8_t* mirror = m_pm4Edram.data();
  auto sampleBits = [&](edram::RtKey k, uint32_t x, uint32_t y) -> uint64_t {
    const uint32_t off = edram::sampleOffsetBytes(x, y, k.base(), k.pitchTiles(), k.msaa(),
                                                  k.isDepth(), k.intsLog2(), 0);
    uint32_t lo, hi = 0;
    std::memcpy(&lo, mirror + off, 4);
    if (k.intsLog2()) std::memcpy(&hi, mirror + (off + 4) % edram::kEdramBytes, 4);
    return uint64_t(lo) | (uint64_t(hi) << 32);
  };
  // Depth destinations: read back whole first (one sync for all of them).
  struct DepthJob { edram::RtKey key; Pm4HostRt* rt; Pm4DepthPlanes pl; };
  std::vector<DepthJob> depthJobs;
  uint64_t rbCursor = 0;
  for (const Pm4Move& mv : moves) {
    if (!mv.dst.isDepth()) continue;
    bool seen = false;
    for (auto& dj : depthJobs) seen = seen || dj.key == mv.dst;
    if (seen) continue;
    Pm4HostRt* rt = pm4Rt(mv.dst);
    if (!rt) continue;
    DepthJob dj{mv.dst, rt, {}};
    rbCursor = pm4AlignUp(rbCursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!pm4DepthFootprints(m_device.Get(), rt->res, rbCursor, dj.pl) ||
        rbCursor + dj.pl.total > kPm4StagingBytes)
      continue;
    rbCursor += dj.pl.total;
    pm4Transition(*rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT p = 0; p < 2; p++) {
      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
      dst.pResource = m_pm4Readback.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = dj.pl.fp[p];
      src.pResource = rt->res;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = p;
      m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    depthJobs.push_back(dj);
  }
  if (!depthJobs.empty()) pm4Flush();

  auto uploadAlloc = [&](uint64_t bytes) -> uint64_t {
    uint64_t off = pm4AlignUp(m_pm4UploadUsed, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (off + bytes > kPm4StagingBytes) {
      pm4Flush();  // GPU done with every earlier upload; start over
      off = 0;
    }
    m_pm4UploadUsed = off + bytes;
    return off;
  };

  // Color destinations: boxed uploads.
  for (const Pm4Move& mv : moves) {
    if (mv.dst.isDepth()) continue;
    Pm4HostRt* rt = pm4Rt(mv.dst);
    if (!rt) continue;
    const edram::RtKey k = mv.dst;
    const edram::HostFmt hf = k.hostFormat();
    const uint32_t bpt = edram::hostBytesPerTexel(hf);
    edram::Rect rects[edram::kRangeRectsMax];
    const int n = edram::rangeRects(k, mv.start, mv.end, rects);
    for (int i = 0; i < n; i++) {
      const edram::Rect& r = rects[i];
      const uint32_t rowPitch = uint32_t(pm4AlignUp(uint64_t(r.w) * bpt, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
      const uint64_t bytes = uint64_t(rowPitch) * r.h;
      if (bytes > kPm4StagingBytes) continue;
      const uint64_t off = uploadAlloc(bytes);
      par::parallelFor(0u, r.h, [&](uint32_t y) {
        uint8_t* row = m_pm4UploadPtr + off + uint64_t(y) * rowPitch;
        for (uint32_t x = 0; x < r.w; x++) {
          float c[4];
          edram::unpackColor(k.format(), sampleBits(k, r.x + x, r.y + y), c);
          edram::float4ToHost(hf, c, row + uint64_t(x) * bpt);
        }
      });
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      fp.Offset = off;
      fp.Footprint.Format = rt->fmt;
      fp.Footprint.Width = r.w;
      fp.Footprint.Height = r.h;
      fp.Footprint.Depth = 1;
      fp.Footprint.RowPitch = rowPitch;
      pm4Transition(*rt, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
      dst.pResource = rt->res;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.pResource = m_pm4Upload.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = fp;
      m_commandList->CopyTextureRegion(&dst, r.x, r.y, 0, &src, nullptr);
    }
  }

  // Depth destinations: patch the read-back image with the moved rects, upload whole.
  if (!depthJobs.empty()) {
    uint8_t* rb = nullptr;
    const D3D12_RANGE rr{0, SIZE_T(rbCursor)};
    if (SUCCEEDED(m_pm4Readback->Map(0, &rr, reinterpret_cast<void**>(&rb))) && rb) {
      for (DepthJob& dj : depthJobs) {
        const uint64_t base = dj.pl.fp[0].Offset;
        const uint64_t off = uploadAlloc(dj.pl.total);
        std::memcpy(m_pm4UploadPtr + off, rb + base, size_t(dj.pl.total));
        Pm4DepthPlanes up = dj.pl;  // the same planes, rebased into the upload
        for (int p = 0; p < 2; p++) up.fp[p].Offset = off + (dj.pl.fp[p].Offset - base);
        for (const Pm4Move& mv : moves) {
          if (mv.dst != dj.key) continue;
          edram::Rect rects[edram::kRangeRectsMax];
          const int n = edram::rangeRects(dj.key, mv.start, mv.end, rects);
          for (int i = 0; i < n; i++)
            par::parallelFor(0u, rects[i].h, [&](uint32_t y) {
              for (uint32_t x = 0; x < rects[i].w; x++) {
                const uint32_t px = rects[i].x + x, py = rects[i].y + y;
                pm4DepthPutGuestBits(dj.key.format(), m_pm4UploadPtr, up, px, py,
                                     uint32_t(sampleBits(dj.key, px, py)));
              }
            });
        }
        pm4Transition(*dj.rt, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT p = 0; p < 2; p++) {
          D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
          dst.pResource = dj.rt->res;
          dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dst.SubresourceIndex = p;
          src.pResource = m_pm4Upload.Get();
          src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          src.PlacedFootprint = up.fp[p];
          m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
      }
      const D3D12_RANGE wr{0, 0};
      m_pm4Readback->Unmap(0, &wr);
    }
  }
  for (const Pm4Move& mv : moves) g_pm4Stats.tilesMoved += mv.end - mv.start;
  g_pm4Stats.transfers += uint32_t(moves.size());
}

// SR_PM4_ORACLE=<dir of Xenia resolve dumps> (trace replay): compare every
// resolve's guest bytes against Xenia's output for the same resolve, see
// pm4OracleCompare. SR_PM4_ORACLE_DIR=<dir> also writes ours/ref BMPs.
bool pm4OracleEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_ORACLE");
    return e && e[0];
  }();
  return v;
}

namespace {
// Xenia's per-resolve dumps (trace-dump --readback_resolve=full
// --trace_dump_resolve_dir=<dir>): r<NNN>_<address>_<length>.bin, in order.
struct XeResolveDump { uint32_t ord = 0, phys = 0; std::vector<uint8_t> bytes; };
const std::vector<XeResolveDump>& xeResolveDumps() {
  static const std::vector<XeResolveDump> v = [] {
    std::vector<XeResolveDump> out;
    const char* dir = std::getenv("SR_PM4_ORACLE");
    if (!dir || !dir[0] || (dir[0] == '1' && !dir[1])) return out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((std::string(dir) + "/r*.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
      XeResolveDump d;
      uint32_t len = 0;
      if (std::sscanf(fd.cFileName, "r%u_%x_%x.bin", &d.ord, &d.phys, &len) != 3) continue;
      if (FILE* f = std::fopen((std::string(dir) + "/" + fd.cFileName).c_str(), "rb")) {
        d.bytes.resize(len);
        d.bytes.resize(std::fread(d.bytes.data(), 1, len, f));
        std::fclose(f);
        d.phys &= 0x1FFFFFFFu;
        out.push_back(std::move(d));
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(),
              [](const XeResolveDump& a, const XeResolveDump& b) { return a.ord < b.ord; });
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_ORACLE] loaded %zu Xenia resolve dumps from %s",
                out.size(), dir);
    return out;
  }();
  return v;
}
}  // namespace

void D3D12Backend::pm4OracleCompare(const edram::ResolveInfo& ri, const uint8_t* dest, size_t bi) {
  static uint32_t s_ord = 0;
  static size_t s_cursor = 0;
  const uint32_t ord = s_ord++;
  const uint32_t bpt = edram::destBytesPerTexel(ri.destFormat);
  if (!bpt || ri.x1 <= ri.x0 || ri.y1 <= ri.y0) return;
  uint32_t bpbLog2 = 0;
  while ((1u << bpbLog2) < bpt) bpbLog2++;
  const uint32_t lo = ri.destBase & 0x1FFFFFFFu;
  const uint64_t span = uint64_t(ri.destPitch) * ((ri.y1 + 31u) & ~31u) * bpt;
  // Xenia's bytes for this resolve: its next dump (in resolve order) that
  // overlaps this destination.
  const auto& dumps = xeResolveDumps();
  const XeResolveDump* ref = nullptr;
  for (size_t i = s_cursor; i < dumps.size(); i++) {
    const auto& d = dumps[i];
    if (d.phys < lo + span && uint64_t(d.phys) + d.bytes.size() > lo) {
      ref = &d;
      s_cursor = i + 1;
      break;
    }
  }
  if (!ref) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_ORACLE] #%u batch=%zu dest=0x%08X fmt=%u %ux%u: no Xenia dump",
                ord, bi, lo, ri.destFormat, ri.x1 - ri.x0, ri.y1 - ri.y0);
    return;
  }
  const uint32_t unit = edram::endianUnitBytes(ri.destEndian);
  const uint32_t w = ri.x1 - ri.x0, h = ri.y1 - ri.y0;
  static const char* s_dir = std::getenv("SR_PM4_ORACLE_DIR");
  const bool dump = s_dir && *s_dir;
  std::vector<float> fo, fr;
  if (dump) {
    fo.assign(size_t(w) * h * 4u, 0.0f);
    fr.assign(size_t(w) * h * 4u, 0.0f);
  }
  double sumAbs[4] = {}, sumO[4] = {}, sumR[4] = {};
  uint32_t texels = 0, differ = 0;
  float maxR = 0.0f;
  for (uint32_t y = ri.y0; y < ri.y1; y++)
    for (uint32_t x = ri.x0; x < ri.x1; x++) {
      const int32_t off = xenos_tiling::tiled2D(int32_t(x), int32_t(y), ri.destPitch, bpbLog2);
      if (off < 0) continue;
      const uint64_t a = uint64_t(lo) + uint32_t(off);
      if (a < ref->phys || a + bpt > uint64_t(ref->phys) + ref->bytes.size()) continue;
      uint8_t o[16] = {}, r[16] = {};
      std::memcpy(o, dest + off, bpt);
      std::memcpy(r, &ref->bytes[size_t(a - ref->phys)], bpt);
      if (std::memcmp(o, r, bpt) != 0) differ++;
      if (unit > 1 && unit <= bpt)
        for (uint32_t u = 0; u + unit <= bpt; u += unit) {
          edram::endianSwapUnit(ri.destEndian, o + u);
          edram::endianSwapUnit(ri.destEndian, r + u);
        }
      float co[4], cr[4];
      pm4DecodeDest(ri.destFormat, o, co);
      pm4DecodeDest(ri.destFormat, r, cr);
      for (int k = 0; k < 4; k++) {
        sumAbs[k] += std::fabs(double(co[k]) - double(cr[k]));
        sumO[k] += co[k];
        sumR[k] += cr[k];
      }
      maxR = std::max({maxR, cr[0], cr[1], cr[2]});
      if (dump) {
        const size_t i = (size_t(y - ri.y0) * w + (x - ri.x0)) * 4u;
        std::memcpy(&fo[i], co, 16);
        std::memcpy(&fr[i], cr, 16);
      }
      texels++;
    }
  const double n = texels ? double(texels) : 1.0;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_ORACLE] #%u xe=r%03u batch=%zu src=0x%08X dest=0x%08X fmt=%u %ux%u texels=%u "
              "differ=%.1f%% mad=(%.4f,%.4f,%.4f,%.4f) ours=(%.4f,%.4f,%.4f,%.4f) "
              "ref=(%.4f,%.4f,%.4f,%.4f)",
              ord, ref->ord, bi, ri.srcKey.v, lo, ri.destFormat, w, h, texels, 100.0 * differ / n,
              sumAbs[0] / n, sumAbs[1] / n, sumAbs[2] / n, sumAbs[3] / n, sumO[0] / n, sumO[1] / n,
              sumO[2] / n, sumO[3] / n, sumR[0] / n, sumR[1] / n, sumR[2] / n, sumR[3] / n);
  if (dump && texels) {
    // Both images share the reference's scale so brightness compares directly.
    const float s = maxR > 1.0f ? 1.0f / maxR : 1.0f;
    std::vector<uint8_t> bgra(size_t(w) * h * 4u);
    auto write = [&](const std::vector<float>& f, const char* tag) {
      for (size_t i = 0; i < size_t(w) * h; i++) {
        bgra[i * 4 + 0] = uint8_t(edram::packUNorm(f[i * 4 + 2] * s, 255.f));
        bgra[i * 4 + 1] = uint8_t(edram::packUNorm(f[i * 4 + 1] * s, 255.f));
        bgra[i * 4 + 2] = uint8_t(edram::packUNorm(f[i * 4 + 0] * s, 255.f));
        bgra[i * 4 + 3] = 255;
      }
      char name[160];
      std::snprintf(name, sizeof(name), "/o%03u_b%05zu_%08X_fmt%u_%s.bmp", ord, bi, lo,
                    ri.destFormat, tag);
      CreateDirectoryA(s_dir, nullptr);
      pm4WriteBmp(std::string(s_dir) + name, bgra.data(), w, h);
    };
    write(fo, "ours");
    write(fr, "ref");
  }
}

// ---- resolve ----
// SR_NATIVE_RESOLVE_VERIFY's counters: match, MISMATCH, match via an owner, MISMATCH via an owner (natResolveDifftest
// reads them around each of its cases).
namespace {
uint64_t g_natVerify[4] = {};
// Explicit diagnostic only; the shipping build excludes this entire unit.
bool natDepthProbeOn() {
  static const bool on = [] { const char* e = std::getenv("SR_NAT_DEPTH_PROBE"); return e && e[0] == '1'; }();
  return on;
}
}  // namespace

// The EDRAM path of a resolve (the oracle unit; docs/RELEASE_COMPOSITION.md 8.2): the span's owners stored into the
// EDRAM buffer (or the CPU mirror), then Xenia's resolve CS, buffer -> shared memory (pm4ShmResolve), or the CPU
// resolve from the mirror (edram::resolveToGuest) with SR_PM4_ORACLE's compare against Xenia's dump. Taken for a copy
// the native kernel did not take (!natDone), and beside a native one under SR_NATIVE_RESOLVE_DUAL (both write the
// destination) or SR_NATIVE_RESOLVE_VERIFY (the two destinations' hashes compared, [NATIVE_VERIFY]). nbPath / nbExtS /
// nbExtL: the census record's path letter and extent when this path is the one that resolved.
void D3D12Backend::edramResolve(const edram::ResolveInfo& ri, const edram::Regs& regs, size_t bi, bool natDone,
                                bool nbTex, edram::RtKey natOwner, bool viaOwner, uint32_t natStart,
                                uint32_t natLen, char& nbPath, uint32_t& nbExtS, uint32_t& nbExtL) {
  static const bool s_natDual = [] {
    const char* e = std::getenv("SR_NATIVE_RESOLVE_DUAL");
    return e && e[0] == '1';
  }();
  // SR_NATIVE_RESOLVE_VERIFY=1 (diagnostic, GPU sync per checked resolve): the EDRAM path
  // re-resolves after the native one and the two destination hashes are compared in process
  // (the EDRAM model is the oracle; its bytes are what stays). Checks every owner-remapped
  // resolve and every 16th other one; [NATIVE_VERIFY].
  static const bool s_natVerify = [] {
    const char* e = std::getenv("SR_NATIVE_RESOLVE_VERIFY");
    return e && e[0] == '1';
  }();
  static uint64_t s_vSeq = 0;
  uint64_t& s_vMatch = g_natVerify[0];  // the counters natResolveDifftest reads
  uint64_t& s_vBad = g_natVerify[1];
  uint64_t& s_vMatchOwner = g_natVerify[2];
  uint64_t& s_vBadOwner = g_natVerify[3];
  // natResolveDifftest (m_natVerifyForce): every native resolve of its cases is verified, sampled or not.
  const bool verify = (s_natVerify || m_natVerifyForce) && natDone && !nbTex &&
                      (viaOwner || m_natVerifyForce || (++s_vSeq & 15u) == 0);
  if (natDone && !(s_natDual && !nbTex) && !verify) return;  // DUAL / VERIFY: the EDRAM path re-resolves
  uint64_t natHash = 0;
  uint32_t natNz = 0;
  bool natRead = false;  // Astra re-review 1: a failed readback is a MISMATCH, never a 0 == 0 match
  if (verify) natHash = pm4ShmHash(natStart, natLen, &natNz, &natRead);  // the native bytes, before this path overwrites them
  // Keep the real native destination before the model overwrites it. Only the
  // synthetic pure-D24 origin case is sampled; no extra GPU work or replay edit.
  std::vector<uint8_t> depthProbeNative;
  const bool depthProbe = m_natVerifyForce && natDepthProbeOn() && ri.isDepth && ri.srcKey.format() == 0u &&
                          ri.x0 == 0u && ri.y0 == 0u && ri.x1 == 80u && ri.y1 == 16u;
  if (depthProbe && natRead) {
    const size_t bytes = size_t(natLen) * resscale::get().area();
    uint8_t* ptr = nullptr;
    const D3D12_RANGE read{0, bytes}, written{0, 0};
    if (SUCCEEDED(m_shmReadback->Map(0, &read, reinterpret_cast<void**>(&ptr))) && ptr) {
      depthProbeNative.assign(ptr, ptr + bytes);
      m_shmReadback->Unmap(0, &written);
    }
  }
  {
    std::vector<edram::Transfer> owners;
    edramSpanOwners(ri.srcKey, ri, owners);
    std::vector<Pm4Move> moves;
    for (const auto& o : owners)
      if (!o.source.empty()) moves.push_back(Pm4Move{o.source, edram::RtKey(), o.start, o.end});
    const auto ts = Pm4Clk::now();
    edramStoreTiles(moves);  // mirror now holds the current bits of the span
    gtMark(GtResStore);      // iter-466 res4k: the resolve's own EDRAM store (SR_PM4_GPU_TIME)
    g_resMsStore += pm4Ms(ts);
    // iter-460 S3: Xenia's resolve CS, EDRAM buffer -> shared memory, no sync.
    uint32_t extStart = 0, extLen = 0;
    const auto tsh = Pm4Clk::now();
    const bool shmDone = pm4ShmEnabled() && pm4GpuEdramEnabled() && m_edramGpuReady &&
                         pm4ShmResolve(ri, regs, extStart, extLen);
    g_resMsShm += pm4Ms(tsh);
    if (shmDone) {
      if (!natDone) {
        nbPath = 'E';
        nbExtS = extStart;
        nbExtL = extLen;
      }
      if (verify) {
        uint32_t nz = 0;
        bool edramRead = false;
        const uint64_t h = pm4ShmHash(extStart, extLen, &nz, &edramRead);
        const bool same = natRead && edramRead && h == natHash && extStart == natStart && extLen == natLen;
        if (depthProbe) {
          uint8_t* ptr = nullptr;
          const D3D12_RANGE read{0, depthProbeNative.size()}, written{0, 0};
          if (!depthProbeNative.empty() && edramRead && extStart == natStart && extLen == natLen &&
              SUCCEEDED(m_shmReadback->Map(0, &read, reinterpret_cast<void**>(&ptr))) && ptr) {
            size_t first = 0;
            while (first < depthProbeNative.size() && depthProbeNative[first] == ptr[first]) ++first;
            const size_t word = first & ~size_t(3);
            uint32_t nativeWord = 0, modelWord = 0;
            if (word + 4u <= depthProbeNative.size()) {
              std::memcpy(&nativeWord, depthProbeNative.data() + word, 4);
              std::memcpy(&modelWord, ptr + word, 4);
            }
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[NAT_DEPTH_WORD] key=%08X start=%08X bytes=%zu firstByte=%zu wordOffset=%zu "
                        "native=%08X model=%08X equal=%u",
                        ri.srcKey.v, natStart, depthProbeNative.size(), first, word,
                        nativeWord, modelWord, first == depthProbeNative.size() ? 1u : 0u);
            m_shmReadback->Unmap(0, &written);
          } else {
            SR_LOG_BOOT(sr::LogSeverity::Error, "[NAT_DEPTH_WORD] unavailable (readback/extent)");
          }
        }
        if (!natRead || !edramRead)
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_VERIFY] readback FAILED (native %d, edram %d): counted as a MISMATCH",
                      natRead ? 1 : 0, edramRead ? 1 : 0);
        uint64_t& cnt = same ? (viaOwner ? s_vMatchOwner : s_vMatch) : (viaOwner ? s_vBadOwner : s_vBad);
        cnt++;
        const uint64_t all = s_vMatch + s_vBad + s_vMatchOwner + s_vBadOwner;
        if ((!same && (s_vBad + s_vBadOwner) <= 16) || (all & (all - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[NATIVE_VERIFY] %s key=%08X owner=%08X rect=(%u,%u)-(%u,%u) dest=%08X fmt=%u nz native=%u edram=%u | "
                      "match %llu bad %llu, via owner match %llu bad %llu",
                      same ? "match" : "MISMATCH", ri.srcKey.v, natOwner.v, ri.x0, ri.y0, ri.x1, ri.y1, ri.destBase,
                      ri.destFormat, natNz, nz, (unsigned long long)s_vMatch, (unsigned long long)s_vBad,
                      (unsigned long long)s_vMatchOwner, (unsigned long long)s_vBadOwner);
      }
      pm4NativeFrontInvalidate(extStart, extLen);
      pm4ResolveHashDebug(ri, extStart, extLen, false);
      g_pm4Stats.resolveTexels += (ri.x1 - ri.x0) * (ri.y1 - ri.y0);
      const auto ti = Pm4Clk::now();
      invalidateGuestRange(extStart & 0x1FFFFFFFu, extLen);
      g_resMsInval += pm4Ms(ti);
      static std::atomic<uint32_t> s_slog{0};
      if (s_slog.fetch_add(1) < 48)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PM4_RESOLVE] GPU src=%s key=0x%08X rect=(%u,%u)-(%u,%u) -> dest=0x%08X fmt=%u "
                    "extent=%08X+%u",
                    ri.isDepth ? "depth" : "color", ri.srcKey.v, ri.x0, ri.y0, ri.x1, ri.y1,
                    ri.destBase, ri.destFormat, extStart, extLen);
    } else if (pm4CpuMirrorRefused("a CPU resolve (one the GPU resolve path does not take)")) {
      // iter-464 resscale: nothing the CPU could write would be at the scale.
    } else {
    if (pm4GpuEdramEnabled() && m_edramGpuReady) {
      // GPU EDRAM: bring exactly the span's tiles back into the CPU mirror.
      const edram::RtKey k = ri.srcKey;
      const uint32_t yShift = k.msaa() >= edram::kMsaa2X ? 1u : 0u;
      const uint32_t row0 = (ri.y0 << yShift) / edram::kTileHeightSamples;
      const uint32_t row1 =
          ((ri.y1 << yShift) + edram::kTileHeightSamples - 1) / edram::kTileHeightSamples;
      edramSyncMirror((k.base() + row0 * k.pitchTiles()) % edram::kTileCount,
                      std::min((row1 - row0) * k.pitchTiles(), edram::kTileCount));
    }
    size_t avail = 0;
    int destWin = 8;
    uint8_t* dest = pm4GuestWritableWin(ri.destBase, avail, destWin);  // iter-460: the owning window
    uint32_t written = 0;
    const auto trc = std::chrono::steady_clock::now();
    if (dest) written = edram::resolveToGuest(m_pm4Edram.data(), ri, dest, avail);
    g_pm4Stats.msResolveCpu +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trc).count();
    g_resMsCpu += pm4Ms(trc);
    g_pm4Stats.resolveTexels += written;
    if (pm4OracleEnabled() && written) pm4OracleCompare(ri, dest, bi);
    // Later draws must decode the new bytes: evict overlapping cache entries.
    const uint32_t bpt = std::max(edram::destBytesPerTexel(ri.destFormat), 1u);
    const uint64_t lo = ri.destBase & 0x1FFFFFFFu;
    const uint64_t hi = lo + uint64_t(ri.destPitch) * ((ri.y1 + 31u) & ~31u) * bpt;
    // P1: re-validate (re-upload in place) instead of dropping the entry - a
    // dropped entry was recreated as a new resource and slot every frame.
    invalidateGuestRange(uint32_t(lo), hi - lo);
    pm4NativeFrontInvalidate(uint32_t(lo), uint32_t(hi - lo));
    if (!natDone) {
      nbPath = 'C';
      nbExtS = uint32_t(lo);
      nbExtL = uint32_t(hi - lo);
    }
    static std::atomic<uint32_t> s_log{0};
    if (s_log.fetch_add(1) < 48)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_RESOLVE] src=%s key=0x%08X rect=(%u,%u)-(%u,%u) sample=%u -> dest=0x%08X "
                  "fmt=%u endian=%u swap=%d pitch=%u bias=%.4f %s texels=%u owners=%zu",
                  ri.isDepth ? "depth" : "color", ri.srcKey.v, ri.x0, ri.y0, ri.x1, ri.y1,
                  ri.sampleSelect, ri.destBase, ri.destFormat, ri.destEndian, ri.destSwap ? 1 : 0,
                  ri.destPitch, ri.expBiasFactor, ri.raw ? "raw" : "convert", written, moves.size());
    }  // CPU resolve
  }
}

// ---- edram-hlsl ----
// The EDRAM buffer's store (host RT -> words, every sample) and load (words -> host RT, sample 0) passes: the oracle
// unit's, appended to kPm4PackHlsl.
const char kPm4EdramStoreLoadHlsl[] = R"HLSL(
ByteAddressBuffer gScratch : register(t1);
RWByteAddressBuffer gEdramRW : register(u0);
[numthreads(8, 8, 1)]
void CSStore(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gRectW || id.y >= gRectH) return;
  uint px = gRectX + id.x;
  uint py = gRectY + id.y;
  uint2 bits;
  if (IsDepth()) {
    uint v0 = gScratch.Load(gD0Off + py * gD0Pitch + px * 4u);
    uint sa = gS1Off + py * gS1Pitch + px * gS1Bpt + gS1Byte;
    uint st = (gScratch.Load(sa & ~3u) >> ((sa & 3u) * 8u)) & 0xFFu;
    uint d24 = Fmt() == 1u ? FloatTo20e4(asfloat(v0) * 2.0) : (v0 & 0xFFFFFFu);
    bits = uint2((d24 << 8) | st, 0u);
  } else {
    bits = PackColor(Fmt(), gSrc.Load(int3(px, py, 0)));
  }
  [loop] for (uint s = 0u; s < gSamples; s++) {
    uint off = SampleOffset(px, py, s);
    gEdramRW.Store(off, bits.x);
    if (IntsLog2() != 0u) gEdramRW.Store((off + 4u) % EdramBytes(), bits.y);
  }
}

// ── load: EDRAM (sample 0) -> host RT, one scissored full-RT triangle (VSQuad: kPm4PackHlsl) ──
ByteAddressBuffer gEdram : register(t2);
uint2 LoadBits(float4 pos) {
  uint off = SampleOffset(uint(pos.x), uint(pos.y), 0u);
  uint2 b = uint2(gEdram.Load(off), 0u);
  if (IntsLog2() != 0u) b.y = gEdram.Load((off + 4u) % EdramBytes());
  return b;
}
float4 PSColor(float4 pos : SV_Position) : SV_Target { return UnpackColor(Fmt(), LoadBits(pos)); }
float PSDepth(float4 pos : SV_Position) : SV_Depth {
  uint d24 = LoadBits(pos).x >> 8;
  if (Fmt() == 1u) return F20e4ToFloat(d24) * 0.5;
  return float(d24 + (d24 >> 23)) * (1.0 / 16777216.0);
}
void PSStencil(float4 pos : SV_Position) {
  if (((LoadBits(pos).x >> gBit) & 1u) == 0u) discard;
}
)HLSL";

// ---- gpu-init ----
// The model's passes: the shared pack / unpack text and the store / load passes over the EDRAM buffer.
static Microsoft::WRL::ComPtr<ID3DBlob> edramCompile(const char* entry, const char* target) {
  static const std::string src = std::string(kPm4PackHlsl) + kPm4EdramStoreLoadHlsl;
  return pm4CompileHlsl(src.data(), src.size(), "pm4_edram.hlsl", entry, target);
}

// The 10 MiB EDRAM buffer and the store (compute) / load (graphics) passes over it: the oracle unit's GPU side.
bool D3D12Backend::edramGpuInit() {
  if (m_edramGpuReady) return true;
  if (m_edramGpuFailed || !pm4GpuInit()) return false;
  // release/emu-stage3 (Astra round 1 P2): the dependency is one-way now - pm4GpuInit no longer enters this
  // initializer, so the band's creation above cannot have created the buffer behind our back; the re-check keeps
  // a second allocation (and a half-failed one left "ready") impossible should that ever change.
  if (m_edramGpuReady) return true;
  auto fail = [&](const char* what) {
    m_edramGpuFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PM4_EDRAM_GPU] init failed at %s - CPU mirror path stays", what);
    return false;
  };
  // EDRAM buffer (committed DEFAULT-heap memory is zeroed, like the mirror).
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  // iter-464 resscale: kEdramSizeBytes * (scale_x * scale_y)
  // (d3d12_render_target_cache.cc:255-257).
  bd.Width = uint64_t(edram::kEdramBytes) * resscale::get().area();
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  m_pm4EdramGpuState = D3D12_RESOURCE_STATE_COMMON;  // buffers are created COMMON
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, m_pm4EdramGpuState,
                                               nullptr, IID_PPV_ARGS(&m_pm4EdramGpu))))
    return fail("edram buffer");
  m_pm4EdramGpu->SetName(L"PM4_EDRAM");
  // Root signatures. Store (compute): b0 constants, u0 EDRAM, t0 table (RT
  // SRV), t1 raw scratch. Load (graphics): b0 constants, t0 EDRAM.
  auto makeRs = [&](const D3D12_ROOT_PARAMETER* params, UINT n,
                    Microsoft::WRL::ComPtr<ID3D12RootSignature>& out) -> bool {
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = n;
    rd.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
    return SUCCEEDED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&out)));
  };
  {
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER p[4]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 16;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &srvRange;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[3].Descriptor.ShaderRegister = 1;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    if (!makeRs(p, 4, m_pm4StoreRs)) return fail("store root signature");
  }
  {
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 16;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[1].Descriptor.ShaderRegister = 2;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    if (!makeRs(p, 2, m_pm4LoadRs)) return fail("load root signature");
  }
  auto cs = edramCompile("CSStore", "cs_5_1");
  m_pm4LoadPsColor = edramCompile("PSColor", "ps_5_1");
  m_pm4LoadPsDepth = edramCompile("PSDepth", "ps_5_1");
  m_pm4LoadPsStencil = edramCompile("PSStencil", "ps_5_1");
  if (!cs || !m_pm4LoadPsColor || !m_pm4LoadPsDepth || !m_pm4LoadPsStencil) return fail("shader compile");
  D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
  cd.pRootSignature = m_pm4StoreRs.Get();
  cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  if (FAILED(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_pm4StoreColorPso))))
    return fail("store pso");
  m_pm4StoreDepthPso = m_pm4StoreColorPso;  // one kernel; IsDepth() selects the path
  m_edramGpuReady = true;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_EDRAM_GPU] ready: %u MiB EDRAM buffer (scale %ux%u), store CS + load PS "
              "(SR_PM4_EDRAM_GPU=0 = CPU mirror)",
              10u * resscale::get().area(), resscale::get().x, resscale::get().y);
  return true;
}

void D3D12Backend::pm4GpuEdramState(D3D12_RESOURCE_STATES s) {
  if (m_pm4EdramGpuState == s) {
    if (s == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      b.UAV.pResource = m_pm4EdramGpu.Get();
      m_commandList->ResourceBarrier(1, &b);
    }
    return;
  }
  pm4Barrier(m_commandList.Get(), m_pm4EdramGpu.Get(), m_pm4EdramGpuState, s);
  m_pm4EdramGpuState = s;
}

// A mirror snapshot restored on the CPU (replay start) reaches the GPU buffer
// before any transfer reads it.
static void pm4GpuUploadIfPending(bool& pending, ID3D12GraphicsCommandList* cl,
                                  ID3D12Resource* upload, uint8_t* uploadPtr, uint64_t& uploadUsed,
                                  const std::vector<uint8_t>& mirror, ID3D12Resource* edramBuf) {
  if (!pending) return;
  pending = false;
  const uint64_t off = (uploadUsed + 255u) & ~uint64_t(255u);
  if (off + edram::kEdramBytes > kPm4StagingBytes) return;
  std::memcpy(uploadPtr + off, mirror.data(), edram::kEdramBytes);
  uploadUsed = off + edram::kEdramBytes;
  cl->CopyBufferRegion(edramBuf, 0, upload, off, edram::kEdramBytes);
}

void D3D12Backend::edramGpuStore(const std::vector<Pm4Move>& moves) {
  if (m_pm4GpuUploadPending) {
    pm4GpuEdramState(D3D12_RESOURCE_STATE_COPY_DEST);
    pm4GpuUploadIfPending(m_pm4GpuUploadPending, m_commandList.Get(), m_pm4Upload.Get(),
                          m_pm4UploadPtr, m_pm4UploadUsed, m_pm4Edram, m_pm4EdramGpu.Get());
  }
  bool bound = false;
  auto bindStore = [&]() {
    if (bound) return;
    bound = true;
    pm4XlatHeaps();  // step 4 lane C 2i: this pass's tables are m_xlatSrvHeap's (ERROR 708 / GBV 935 without)
    m_commandList->SetComputeRootSignature(m_pm4StoreRs.Get());
    m_commandList->SetPipelineState(m_pm4StoreColorPso.Get());
    m_commandList->SetComputeRootUnorderedAccessView(1, m_pm4EdramGpu->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(2, pm4RtSrvGpu(0));
    // t1 is only read on the depth path; any GENERIC_READ buffer satisfies it.
    m_commandList->SetComputeRootShaderResourceView(3, m_pm4Upload->GetGPUVirtualAddress());
  };
  {  // iter-466 res4k (SR_RES_XFERLOG=<n>, diagnostic): the ownership transfers, by kind
    static const uint32_t s_n = [] {
      const char* e = std::getenv("SR_RES_XFERLOG");
      return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    static uint32_t s_i = 0;
    for (const Pm4Move& mv : moves)
      if (s_n && s_i++ < s_n)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[RES_XFERLOG] %08X(%s) -> %08X(%s) tiles [%u,%u) %s",
                    mv.src.v, mv.src.empty() ? "-" : mv.src.isDepth() ? "D" : "C", mv.dst.v,
                    mv.dst.empty() ? "-" : mv.dst.isDepth() ? "D" : "C", mv.start, mv.end,
                    pm4XferDirectOk(mv) ? "direct" : "edram");
  }
  for (const Pm4Move& mv : moves) {
    if (mv.src.empty()) continue;
    if (pm4XferDirectOk(mv)) continue;  // iter-466 res4k: the load reads the source RT itself
    Pm4HostRt* rt = pm4Rt(mv.src);
    if (!rt) continue;
    edram::Rect rects[edram::kRangeRectsMax];
    const int n = edram::rangeRects(mv.src, mv.start, mv.end, rects);
    if (!n) continue;
    Pm4DepthPlanes pl{};
    if (mv.src.isDepth()) {
      // Raw plane bits: depth + stencil copied whole into the scratch buffer.
      if (!pm4DepthFootprints(m_device.Get(), rt->res, 0, pl)) continue;
      if (pl.total > m_pm4DepthScratchBytes) {
        // The open command list may still reference the old scratch: keep it
        // alive until the next flush has waited for the GPU.
        if (m_pm4DepthScratch) m_pm4Graveyard.push_back(m_pm4DepthScratch);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = std::max<uint64_t>(pl.total, 64ull << 20);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        m_pm4DepthScratch.Reset();
        m_pm4DepthScratchState = D3D12_RESOURCE_STATE_COMMON;  // buffers are created COMMON
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                     m_pm4DepthScratchState, nullptr,
                                                     IID_PPV_ARGS(&m_pm4DepthScratch)))) {
          m_pm4DepthScratchBytes = 0;
          continue;
        }
        m_pm4DepthScratchBytes = bd.Width;
      }
      if (m_pm4DepthScratchState != D3D12_RESOURCE_STATE_COPY_DEST) {
        pm4Barrier(m_commandList.Get(), m_pm4DepthScratch.Get(), m_pm4DepthScratchState,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        m_pm4DepthScratchState = D3D12_RESOURCE_STATE_COPY_DEST;
      }
      pm4Transition(*rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT p = 0; p < 2; p++) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m_pm4DepthScratch.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = pl.fp[p];
        src.pResource = rt->res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = p;
        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      pm4Barrier(m_commandList.Get(), m_pm4DepthScratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      m_pm4DepthScratchState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      bound = false;
    } else {
      if (rt->srv < 0) continue;
      pm4Transition(*rt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    pm4GpuEdramState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bindStore();
    if (mv.src.isDepth()) {
      m_commandList->SetComputeRootShaderResourceView(3, m_pm4DepthScratch->GetGPUVirtualAddress());
    } else {
      m_commandList->SetComputeRootDescriptorTable(2, pm4RtSrvGpu(uint32_t(rt->srv)));
    }
    for (int i = 0; i < n; i++) {
      const edram::Rect hr = pm4HostRect(rects[i]);
      Pm4XferConsts c = pm4XferConsts(mv.src, hr);
      if (mv.src.isDepth()) {
        c.d0Off = uint32_t(pl.fp[0].Offset);
        c.d0Pitch = pl.fp[0].Footprint.RowPitch;
        c.s1Off = uint32_t(pl.fp[1].Offset);
        c.s1Pitch = pl.fp[1].Footprint.RowPitch;
        c.s1Bpt = pl.bpt[1];
        c.s1Byte = pm4StencilByte(pl.bpt[1]);
      }
      m_commandList->SetComputeRoot32BitConstants(0, 16, &c, 0);
      m_commandList->Dispatch((hr.w + 7u) / 8u, (hr.h + 7u) / 8u, 1);
    }
  }
}

ID3D12PipelineState* D3D12Backend::pm4GpuLoadPso(DXGI_FORMAT fmt, uint32_t kind, uint32_t bit) {
  const uint32_t key = uint32_t(fmt) | (kind << 8) | (bit << 12);
  auto it = m_pm4LoadPsos.find(key);
  if (it != m_pm4LoadPsos.end()) return it->second.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = m_pm4LoadRs.Get();
  pd.VS = {m_pm4LoadVs->GetBufferPointer(), m_pm4LoadVs->GetBufferSize()};
  ID3DBlob* ps = kind == 0 ? m_pm4LoadPsColor.Get() : (kind == 1 ? m_pm4LoadPsDepth.Get() : m_pm4LoadPsStencil.Get());
  pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  for (int i = 0; i < 8; i++) pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.SampleDesc.Count = 1;
  if (kind == 0) {
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
  } else {
    pd.DSVFormat = fmt;
    D3D12_DEPTH_STENCIL_DESC& ds = pd.DepthStencilState;
    if (kind == 1) {
      ds.DepthEnable = TRUE;
      ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    } else {
      ds.DepthEnable = FALSE;
      ds.StencilEnable = TRUE;
      ds.StencilReadMask = 0xFF;
      ds.StencilWriteMask = UINT8(1u << bit);
      D3D12_DEPTH_STENCILOP_DESC op{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                    D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
      ds.FrontFace = op;
      ds.BackFace = op;
    }
  }
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  if (FAILED(sr::hitch::timed(sr::hitch::kPso,
                              [&] { return m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)); }))) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PM4_EDRAM_GPU] load PSO failed fmt=%u kind=%u bit=%u", fmt, kind, bit);
    return nullptr;
  }
  return m_pm4LoadPsos.emplace(key, pso).first->second.Get();
}

// ---- gpu-load ----
// A mirror snapshot restored on the CPU (replay start) reaches the GPU buffer before any transfer reads it.
void D3D12Backend::edramGpuUploadPending() {
  if (!m_pm4GpuUploadPending || !edramGpuInit()) return;
  pm4GpuEdramState(D3D12_RESOURCE_STATE_COPY_DEST);
  pm4GpuUploadIfPending(m_pm4GpuUploadPending, m_commandList.Get(), m_pm4Upload.Get(), m_pm4UploadPtr,
                        m_pm4UploadUsed, m_pm4Edram, m_pm4EdramGpu.Get());
}

// One move the direct passes do not take: its destination's tiles loaded from the EDRAM buffer (its source was stored
// by edramGpuStore) - colour through PSColor, depth through PSDepth + one pass per stencil bit.
void D3D12Backend::edramGpuLoadMove(const Pm4Move& mv, Pm4HostRt& rtRef, const edram::Rect* rects, int n, bool& bound) {
  Pm4HostRt* rt = &rtRef;
  const bool depth = mv.dst.isDepth();
  pm4GpuEdramState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  pm4Transition(*rt, depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (!bound) {
    bound = true;
    m_commandList->SetGraphicsRootSignature(m_pm4LoadRs.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
  m_commandList->SetGraphicsRootShaderResourceView(1, m_pm4EdramGpu->GetGPUVirtualAddress());
  if (depth)
    m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &rt->view);
  else
    m_commandList->OMSetRenderTargets(1, &rt->view, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0.0f, 0.0f, float(rt->w), float(rt->h), 0.0f, 1.0f};
  m_commandList->RSSetViewports(1, &vp);
  for (int i = 0; i < n; i++) {
    const edram::Rect r = pm4HostRect(rects[i]);  // iter-464 resscale: host pixels
    const D3D12_RECT sc{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
    m_commandList->RSSetScissorRects(1, &sc);
    Pm4XferConsts c = pm4XferConsts(mv.dst, r);
    m_commandList->SetGraphicsRoot32BitConstants(0, 16, &c, 0);
    if (!depth) {
      if (ID3D12PipelineState* p = pm4GpuLoadPso(rt->fmt, 0, 0)) {
        m_commandList->SetPipelineState(p);
        m_commandList->DrawInstanced(3, 1, 0, 0);
      }
      continue;
    }
    m_commandList->ClearDepthStencilView(rt->view, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 1, &sc);
    if (ID3D12PipelineState* p = pm4GpuLoadPso(rt->fmt, 1, 0)) {
      m_commandList->SetPipelineState(p);
      m_commandList->DrawInstanced(3, 1, 0, 0);
    }
    m_commandList->OMSetStencilRef(0xFF);
    for (uint32_t bit = 0; bit < 8; bit++) {
      ID3D12PipelineState* p = pm4GpuLoadPso(rt->fmt, 2, bit);
      if (!p) continue;
      c.bit = bit;
      m_commandList->SetGraphicsRoot32BitConstants(0, 16, &c, 0);
      m_commandList->SetPipelineState(p);
      m_commandList->DrawInstanced(3, 1, 0, 0);
    }
  }
  m_meterCur.bufferMoves++;  // NATIVE METER (C1 stage 3): a move executed through the EDRAM buffer, whatever decided it
  m_meterCur.bufferMoveTiles += mv.end - mv.start;
}

// ---- sync-mirror ----
void D3D12Backend::edramSyncMirror(uint32_t start, uint32_t len) {
  if (!len) return;
  len = std::min(len, edram::kTileCount);
  start %= edram::kTileCount;
  constexpr uint32_t kTileBytes = edram::kTileDwords * 4u;
  const uint32_t n0 = std::min(len, edram::kTileCount - start), n1 = len - n0;
  pm4GpuEdramState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_commandList->CopyBufferRegion(m_pm4Readback.Get(), 0, m_pm4EdramGpu.Get(), uint64_t(start) * kTileBytes,
                                  uint64_t(n0) * kTileBytes);
  if (n1)
    m_commandList->CopyBufferRegion(m_pm4Readback.Get(), uint64_t(n0) * kTileBytes, m_pm4EdramGpu.Get(), 0,
                                    uint64_t(n1) * kTileBytes);
  const auto tf = std::chrono::steady_clock::now();
  pm4Flush();
  m_pm4EdramGpuState = D3D12_RESOURCE_STATE_COPY_SOURCE;  // unchanged by the flush
  const auto tc = std::chrono::steady_clock::now();
  g_pm4Stats.msFlush += std::chrono::duration<double, std::milli>(tc - tf).count();
  uint8_t* mapped = nullptr;
  const D3D12_RANGE rr{0, SIZE_T(uint64_t(len) * kTileBytes)};
  if (FAILED(m_pm4Readback->Map(0, &rr, reinterpret_cast<void**>(&mapped))) || !mapped) return;
  std::memcpy(m_pm4Edram.data() + size_t(start) * kTileBytes, mapped, size_t(n0) * kTileBytes);
  if (n1) std::memcpy(m_pm4Edram.data(), mapped + size_t(n0) * kTileBytes, size_t(n1) * kTileBytes);
  const D3D12_RANGE wr{0, 0};
  m_pm4Readback->Unmap(0, &wr);
  g_pm4Stats.msStoreCpu +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc).count();
}

// ---- difftest ----
// release/emu-stage3 (Astra round 1 P1): SR_NAT_DIFFTEST=1 - synthetic resolves of the shapes the native memory path
// (pm4NativeResolve) must take now that the release configuration has no EDRAM path behind it, each verified against
// the word model's bytes: the source host RT of a fresh key is filled with a block pattern, the key claims its span,
// the resolve runs through pm4ExecResolveRegs exactly as a record's would, and SR_NATIVE_RESOLVE_VERIFY's compare
// (forced for the case) hashes the native destination against the EDRAM path's re-resolve of the same registers. The
// trace replay calls it after its frames; the pre-merge check (docs/RELEASE_COMPOSITION.md 8.4) runs it on the menu
// trace at 1x. PASS per case = the kernel took it (one native resolve, no model resolve) and the hashes match. At a
// draw resolution scale the verify compares the 1x memory, which a scaled resolve does not write: skipped (the
// replay compare at 3x is the scaled proof).
std::string nbNatStatus();  // gfx/d3d12_pm4_shmem.cpp

namespace {
bool natDifftestOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NAT_DIFFTEST");
    return e && e[0] == '1';
  }();
  return v;
}
struct NatDifftestCase {
  const char* name;
  bool depth;
  uint32_t msaa, fmt;       // the source surface: MSAA mode, RB_COLOR_INFO format / RB_DEPTH_INFO format
  uint32_t pitchPx;         // the surface pitch in pixels (1280 = 16 tiles; 1600 = 20: the ring wraps mid-row)
  uint32_t x0, y0, x1, y1;  // the rectangle, surface pixels (8-aligned; rows past the surface's last tile row wrap)
  uint32_t destFormat;      // the destination TextureFormat (depth: derived by computeResolve, 22 / 23)
  bool raw;                 // copy_command raw (bitwise) / convert
  bool mixed;               // a depth surface at the same base / pitch claims the rectangle's second tile row (M)
};
// The folds: a 1x tile is 80 x 16 pixels; 2x halves the rows (16 sample rows = 8 px), 4x also the columns. The ring:
// 2048 tiles; at pitch 16 the surface's 128 tile rows (2048 px) wrap to row 0 (Astra re-review 1's case); at pitch 20
// row 102 straddles the wrap (its columns 8.. are tiles 0.. again: row 0's).
constexpr NatDifftestCase kNatDifftestCases[] = {
    {"colour_8888_origin", false, 0, 0, 1280, 0, 0, 80, 16, 6, false, false},         // control: no fold
    {"colour_8888_fold_xy", false, 0, 0, 1280, 80, 16, 160, 32, 6, false, false},     // one tile right and one down
    {"colour_8888_fold_y2", false, 0, 0, 1280, 0, 16, 80, 48, 6, false, false},       // past one row, two rows tall
    {"colour_8888_raw_fold_xy", false, 0, 0, 1280, 80, 16, 160, 32, 6, true, false},
    {"colour_8888_unaligned", false, 0, 0, 1280, 8, 24, 88, 40, 6, false, false},     // in-tile offsets (8, 8) + a fold
    {"colour_8888_wrap_ring", false, 0, 0, 1280, 0, 2048, 80, 2064, 6, false, false}, // row 128 = tile 2048 -> row 0
    {"colour_8888_wrap_straddle", false, 0, 0, 1600, 640, 1632, 720, 1648, 6, false, false},  // row 102 col 8 = tile 2048
    {"colour_8888_wrap_both_sides", false, 0, 0, 1600, 560, 1632, 720, 1648, 6, false, false},  // columns 7 and 8: 7 stays, 8 wraps
    {"depth_24_8_wrap_both_sides", true, 0, 0, 1600, 560, 1632, 720, 1648, 22, true, false},
    // 64bpp (16_16_16_16: 40 tiles of 40 px per row; 2048 / 40 = 51.2 rows): row 51 column 8 = tile 2048 -> row 0
    {"colour_64bpp_wrap_both_sides", false, 0, 5, 1600, 280, 816, 360, 832, 26, false, false},  // columns 7 and 8
    {"colour_8888_mixed_depth_row", false, 0, 0, 1280, 0, 0, 80, 32, 6, false, true}, // row 1 owned by a depth surface
    {"colour_8888_mixed_wrap_ring", false, 0, 0, 1280, 0, 2048, 80, 2080, 6, false, true},  // M + the ring wrap: row 129 = row 1, the depth owner's
    {"colour_2x_fold_y", false, 1, 0, 1280, 0, 8, 80, 24, 6, false, false},           // 2x: y0 = 8 px = 16 sample rows
    {"colour_4x_fold_xy", false, 2, 0, 1280, 40, 8, 80, 16, 6, false, false},         // 4x: x0 = 40 px = 80 samples
    {"colour_64bpp_fold_y", false, 0, 5, 1280, 0, 16, 80, 32, 26, false, false},      // 16_16_16_16 -> k_16_16_16_16
    {"colour_2_10_10_10_fold_y", false, 0, 2, 1280, 0, 16, 80, 32, 6, false, false},  // the full 32bpp kernel
    {"colour_8888_to_8bpp_fold_y", false, 0, 0, 1280, 0, 16, 80, 32, 2, false, false},    // the full 8bpp kernel (k_8)
    {"colour_8888_to_565_fold_y", false, 0, 0, 1280, 0, 16, 80, 32, 4, false, false},     // the full 16bpp kernel (k_5_6_5)
    {"colour_8888_to_128bpp_fold_y", false, 0, 0, 1280, 0, 16, 80, 32, 38, false, false}, // the full 128bpp kernel
    {"depth_24_8_origin", true, 0, 0, 1280, 0, 0, 80, 16, 22, true, false},           // control
    {"depth_24_8_fold_y", true, 0, 0, 1280, 0, 16, 80, 32, 22, true, false},          // the review's case: 1x depth, y0 >= 16
    {"depth_24fs8_fold_y", true, 0, 1, 1280, 0, 16, 80, 32, 23, true, false},
    {"depth_24_8_fold_xy", true, 0, 0, 1280, 80, 16, 160, 48, 22, true, false},
    {"depth_24_8_unaligned", true, 0, 0, 1280, 8, 24, 88, 40, 22, true, false},
    {"depth_24_8_wrap_ring", true, 0, 0, 1280, 0, 2048, 80, 2064, 22, true, false},   // the re-review's case
    {"depth_2x_fold_y", true, 1, 0, 1280, 0, 8, 80, 24, 22, true, false},
};
}  // namespace

void D3D12Backend::natResolveDifftest() {
  if (!natDifftestOn()) return;
  // At a draw resolution scale (the replay with SR_RES_SCALE=3) the same cases run against the scaled resolve
  // memory: pm4ShmHash reads it there (Astra re-review 2), so the scaled kernels - the tile dimensions scaled,
  // the fold in scaled pixels - are verified as the 1x ones are.
  const resscale::Scale& rs = resscale::get();
  if (!pm4Init() || !pm4ShmInit() || (rs.scaled() && !m_srReady)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NAT_DIFFTEST] the PM4 renderer / the shared memory did not initialize");
    return;
  }
  const bool wasClosed = m_clState != ClState::Recording;
  auto open = [&] {  // the list may have been flushed (the verify's hashes) or closed by a transfer pass
    if (m_clState == ClState::Recording) return;
    clReset(Pm4Reset);
    ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
    m_commandList->SetDescriptorHeaps(2, heaps);
  };
  // The span of whole tile rows [y0, y1) of a key, claimed as a draw binding it would claim it (the moves executed).
  auto claimRows = [&](edram::RtKey k, uint32_t y0, uint32_t y1) {
    const uint32_t ly = k.msaa() >= edram::kMsaa2X ? 1u : 0u;
    const uint32_t row0 = (y0 << ly) / edram::kTileHeightSamples;
    const uint32_t row1 = ((y1 << ly) + edram::kTileHeightSamples - 1u) / edram::kTileHeightSamples;
    std::vector<Pm4Move> moves;
    pm4Claim(k, (k.base() + row0 * k.pitchTiles()) % edram::kTileCount, (row1 - row0) * k.pitchTiles(), moves);
    nbZeroLoads(moves, 0);
    pm4MeterMoves(moves);
    pm4MoveTiles(moves);
    open();
  };
  // The pattern: one value per 4 x 4 guest-pixel block of the rectangle, written where the key's host RT holds the
  // block's TILE - a row past the surface's last tile row, or a column past the ring's end, is tile (index mod 2048),
  // held at that tile's own row / column (what the model's store reads too: edram::rangeRects, split at the base).
  // A block whose canonical location differs from its own gets a DECOY at its own location (another value), so a
  // reader of the non-canonical pixels (the un-split model, an un-canonicalised kernel) mismatches instead of
  // reading whatever was there (Astra re-review 2: "write the case contents so canonical and non-canonical pixels
  // differ").
  auto fillPattern = [&](Pm4HostRt* rt, edram::RtKey k, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                         uint32_t salt) {
    const uint32_t tw = k.tileWidthPixels(), th = k.tileHeightPixels(), pt = k.pitchTiles();
    pm4Transition(*rt, k.isDepth() ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto clearBlock = [&](uint32_t cx, uint32_t cy, uint32_t h) {
      if (cx + 4u > rt->w / rs.x || cy + 4u > rt->h / rs.y) return;  // outside the host RT: nothing to write
      const D3D12_RECT r{LONG(cx * rs.x), LONG(cy * rs.y), LONG((cx + 4u) * rs.x), LONG((cy + 4u) * rs.y)};
      if (k.isDepth()) {
        m_commandList->ClearDepthStencilView(rt->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                             float((h >> 8) % 977u) / 976.0f, UINT8(h & 0xFFu), 1, &r);
      } else {
        const float c[4] = {float(h & 0xFFu) / 255.0f, float((h >> 8) & 0xFFu) / 255.0f,
                            float((h >> 16) & 0xFFu) / 255.0f, float((h >> 24) & 0xFFu) / 255.0f};
        m_commandList->ClearRenderTargetView(rt->view, c, 1, &r);
      }
    };
    uint32_t b = 0;
    for (uint32_t y = y0; y < y1; y += 4u)
      for (uint32_t x = x0; x < x1; x += 4u, b++) {
        uint32_t lin = (y / th) * pt + x / tw;
        lin %= edram::kTileCount;
        const uint32_t cx = (lin % pt) * tw + x % tw, cy = (lin / pt) * th + y % th;
        const uint32_t h = (b + 1u) * 2654435761u ^ (salt * 40503u);
        clearBlock(cx, cy, h);
        if (cx != x || cy != y) clearBlock(x, y, ~h ^ 0x5A5A5A5Au);  // the decoy at the non-canonical location
      }
  };
  // Compare the existing depth SRVs with the model's just-copied raw planes.
  // All 80 patterned 4x4 blocks of the origin case are sampled, one texel each.
  // This reads only; it does not change the fixture's expected result or path.
  auto probeDepth = [&](Pm4HostRt& rt, edram::RtKey key) {
    auto unavailable = [&](const char* phase, HRESULT hr) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NAT_DEPTH_SAMPLE] unavailable phase=%s hr=%08X", phase, unsigned(hr));
    };
    Pm4DepthPlanes planes{};
    if (rt.dsrv < 0 || !m_pm4DepthScratch || !pm4DepthFootprints(m_device.Get(), rt.res, 0, planes) ||
        planes.total > m_pm4DepthScratchBytes || planes.total > UINT32_MAX || planes.bpt[0] != 4u ||
        (planes.bpt[1] != 1u && planes.bpt[1] != 4u && planes.bpt[1] != 8u)) {
      unavailable("source", E_INVALIDARG); return;
    }
    constexpr uint32_t count = 80u, bytes = count * 16u;
    static constexpr char shader[] = R"HLSL(
cbuffer Probe : register(b0) {
  uint d0Off, d0Pitch, s1Off, s1Pitch, s1Bpt, s1Byte, scaleX, scaleY;
};
Texture2D<float> depth : register(t0);
Texture2D<uint4> stencil : register(t1);
ByteAddressBuffer raw : register(t2);
RWByteAddressBuffer result : register(u0);
[numthreads(32, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 80u) return;
  uint2 p = uint2((id.x % 20u) * 4u * scaleX, (id.x / 20u) * 4u * scaleY);
  uint sa = s1Off + p.y * s1Pitch + p.x * s1Bpt + s1Byte;
  result.Store4(id.x * 16u, uint4(asuint(depth.Load(int3(p, 0))), stencil.Load(int3(p, 0)).y,
      raw.Load(d0Off + p.y * d0Pitch + p.x * 4u), (raw.Load(sa & ~3u) >> ((sa & 3u) * 8u)) & 255u));
}
)HLSL";
    auto code = pm4CompileHlsl(shader, sizeof(shader) - 1u, "nat-depth-probe", "main", "cs_5_1");
    if (!code) { unavailable("compile", E_FAIL); return; }
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> result, readback;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor = {2, 0};
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor = {0, 0};
    D3D12_ROOT_SIGNATURE_DESC rsd{4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr)) { unavailable("serialize", hr); return; }
    hr = m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&signature));
    if (FAILED(hr)) { unavailable("signature", hr); return; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = signature.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    hr = m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) { unavailable("pipeline", hr); return; }
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         nullptr, IID_PPV_ARGS(&result));
    if (FAILED(hr)) { unavailable("output", hr); return; }
    heap.Type = D3D12_HEAP_TYPE_READBACK; desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) { unavailable("readback", hr); return; }
    const uint32_t constants[] = {uint32_t(planes.fp[0].Offset), planes.fp[0].Footprint.RowPitch,
                                 uint32_t(planes.fp[1].Offset), planes.fp[1].Footprint.RowPitch,
                                 planes.bpt[1], pm4StencilByte(planes.bpt[1]), rs.x, rs.y};
    open();
    pm4Transition(rt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (m_pm4DepthScratchState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
      pm4Barrier(m_commandList.Get(), m_pm4DepthScratch.Get(), m_pm4DepthScratchState,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      m_pm4DepthScratchState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    m_commandList->SetPipelineState(pipeline.Get());
    m_commandList->SetComputeRootSignature(signature.Get());
    m_commandList->SetComputeRoot32BitConstants(0, 8, constants, 0);
    m_commandList->SetComputeRootDescriptorTable(1, pm4RtSrvGpu(uint32_t(rt.dsrv)));
    m_commandList->SetComputeRootShaderResourceView(2, m_pm4DepthScratch->GetGPUVirtualAddress());
    m_commandList->SetComputeRootUnorderedAccessView(3, result->GetGPUVirtualAddress());
    m_commandList->Dispatch((count + 31u) / 32u, 1, 1);
    pm4Barrier(m_commandList.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, bytes);
    pm4Flush();  // submit and wait before any local pipeline/buffer is released
    uint32_t* mapped = nullptr;
    const D3D12_RANGE read{0, bytes}, written{0, 0};
    hr = readback->Map(0, &read, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr) || !mapped) { unavailable("map", FAILED(hr) ? hr : E_POINTER); return; }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NAT_DEPTH_SAMPLE] begin key=%08X samples=%u resourceFmt=%u dsvFmt=%u "
                "expectedDepthSrv=%u expectedStencilSrv=%u srvSlot=%d copyDepthBpt=%u copyStencilBpt=%u scale=%ux%u",
                key.v, count, unsigned(rt.res->GetDesc().Format), unsigned(rt.fmt),
                unsigned(DXGI_FORMAT_R24_UNORM_X8_TYPELESS), unsigned(DXGI_FORMAT_X24_TYPELESS_G8_UINT),
                rt.dsrv, planes.bpt[0], planes.bpt[1], rs.x, rs.y);
    for (uint32_t i = 0; i < count; ++i)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NAT_DEPTH_SAMPLE] i=%u x=%u y=%u srvFloat=%08X srvStencil=%08X rawDepth=%08X rawStencil=%08X",
                  i, (i % 20u) * 4u * rs.x, (i / 20u) * 4u * rs.y,
                  mapped[4u * i], mapped[4u * i + 1u], mapped[4u * i + 2u], mapped[4u * i + 3u]);
    readback->Unmap(0, &written);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NAT_DEPTH_SAMPLE] end samples=%u", count);
  };
  open();
  uint32_t pass = 0, fail = 0, idx = 0;
  for (const NatDifftestCase& t : kNatDifftestCases) {
    const uint32_t i = idx++;
    const uint32_t pt = edram::pitchTiles32(t.pitchPx, t.msaa);
    // Tiles high in the EDRAM, re-claimed by every case (the replayed frame's surfaces sit lower; a previous
    // case's key hands its tiles over through the same moves a draw's binding would make).
    const uint32_t base = t.depth ? 1792u : 1536u;
    const edram::RtKey key = edram::RtKey::make(base, pt, t.msaa, t.depth, t.fmt);
    Pm4HostRt* rt = pm4Rt(key);
    if (!rt) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NAT_DIFFTEST] case %s: no host RT for key %08X -> FAIL", t.name, key.v);
      fail++;
      continue;
    }
    claimRows(key, t.y0, t.y1);
    fillPattern(rt, key, t.x0, t.y0, t.x1, t.y1, i + 1u);
    edram::RtKey keyB;
    if (t.mixed) {  // the rectangle's second tile row goes to a depth surface at the same base / pitch (the M class)
      keyB = edram::RtKey::make(base, pt, t.msaa, true, 0u);
      Pm4HostRt* rtB = pm4Rt(keyB);
      if (!rtB) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[NAT_DIFFTEST] case %s: no host RT for the depth owner %08X -> FAIL",
                    t.name, keyB.v);
        fail++;
        continue;
      }
      const uint32_t th = key.tileHeightPixels();
      claimRows(keyB, t.y0 + th, t.y0 + 2u * th);
      fillPattern(rtB, keyB, t.x0, t.y0 + th, t.x1, t.y0 + 2u * th, i + 101u);
    }
    // The resolve, as a record's: the register snapshot alone drives it.
    edram::Regs regs;
    regs.surfaceInfo = t.pitchPx | (t.msaa << 16);
    regs.modeControl = 4u;  // kColorDepth
    if (t.depth) regs.depthInfo = base | (t.fmt << 16);
    else regs.colorInfo[0] = base | (t.fmt << 16);
    regs.copyControl = (t.depth ? 4u : 0u) | ((t.raw ? 0u : 1u) << 20);  // sample 0, no clear
    regs.copyDestBase = 0x1C000000u + i * 0x200000u;  // physical, page-aligned; the replay's guest memory is flat
    regs.copyDestPitch = 1024u | (2112u << 16);       // the destination holds the rectangle at its own coordinates
    regs.copyDestInfo = 2u | (t.destFormat << 7);     // endian 8in32, no exp bias, no swap
    regs.windowScissorTL = 1u << 31;                  // window offset disabled
    regs.windowScissorBR = (4096u << 16) | 4096u;
    regs.screenScissorBR = (4096u << 16) | 4096u;
    regs.resolveVerts[0] = float(t.x0);
    regs.resolveVerts[1] = float(t.y0);
    regs.resolveVerts[2] = float(t.x1);
    regs.resolveVerts[3] = float(t.y0);
    regs.resolveVerts[4] = float(t.x0);
    regs.resolveVerts[5] = float(t.y1);
    regs.resolveVertsValid = true;
    const edram::ResolveInfo ri = edram::computeResolve(regs);
    const bool shapeOk = ri.valid && ri.copy && ri.x0 == t.x0 && ri.y0 == t.y0 && ri.x1 == t.x1 && ri.y1 == t.y1 &&
                         ri.srcKey == key && ri.destFormat == t.destFormat;
    const uint32_t nat0 = m_meterCur.resolvesGuestMem, edr0 = m_meterCur.resolvesEdram;
    const uint64_t ok0 = g_natVerify[0] + g_natVerify[2], bad0 = g_natVerify[1] + g_natVerify[3];
    const uint64_t okOwner0 = g_natVerify[2];
    m_natVerifyForce = true;
    if (shapeOk) pm4ExecResolveRegs(regs, 0);
    m_natVerifyForce = false;
    open();
    const uint32_t nat = m_meterCur.resolvesGuestMem - nat0, edr = m_meterCur.resolvesEdram - edr0;
    const uint64_t ok = (g_natVerify[0] + g_natVerify[2]) - ok0, bad = (g_natVerify[1] + g_natVerify[3]) - bad0;
    const uint64_t okOwner = g_natVerify[2] - okOwner0;
    const bool passed = shapeOk && nat == 1u && edr == 0u && ok == 1u && bad == 0u && (!t.mixed || okOwner == 1u);
    if (passed) pass++;
    else fail++;
    SR_LOG_BOOT(passed ? sr::LogSeverity::Info : sr::LogSeverity::Error,
                "[NAT_DIFFTEST] case %s: key=%08X%s rect=(%u,%u)-(%u,%u) dest=%08X dfmt=%u %s | native %u model %u | "
                "verify match %llu (via owner %llu) bad %llu | shape %s -> %s",
                t.name, key.v, t.mixed ? " + depth owner" : "", t.x0, t.y0, t.x1, t.y1, regs.copyDestBase,
                ri.destFormat, t.raw ? "raw" : "convert", nat, edr, (unsigned long long)ok,
                (unsigned long long)okOwner, (unsigned long long)bad, shapeOk ? "ok" : "REJECTED",
                passed ? "PASS" : "FAIL");
    if (natDepthProbeOn() && std::strcmp(t.name, "depth_24_8_origin") == 0 && nat == 1u && ok + bad == 1u)
      probeDepth(*rt, key);
  }
  SR_LOG_BOOT(fail ? sr::LogSeverity::Error : sr::LogSeverity::Info,
              "[NAT_DIFFTEST] summary cases=%u pass=%u fail=%u scale=%ux%u | not covered here: the NAT_TEX variant "
              "and its second-rectangle dispatch (they need the texture cache's entry at the destination, and the "
              "second rectangle runs at a scale only: the live rolls' T path and the replay compare at 3x) | range "
              "splits so far %llu | %s",
              pass + fail, pass, fail, rs.x, rs.y,
              (unsigned long long)edram::g_rangeRectSplits.load(std::memory_order_relaxed), nbNatStatus().c_str());
  if (wasClosed && m_clState == ClState::Recording) clClose(Pm4Close);
}

}  // namespace gfx
}  // namespace sr
