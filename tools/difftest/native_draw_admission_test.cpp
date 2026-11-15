// Actual SR_ORACLE=0 drainDrawQueue admission on a WARP device. No window/game,
// packet interpreter, substitute drain, or production fault-injection gate.
#include "gfx/d3d12_backend.hpp"
#include "gfx/shader_cache.hpp"
#include "gfx/xenos_state.hpp"
#include "gfx/cube_layout.hpp"
#include "gfx/xenos_tiling.hpp"
#include "app/log.hpp"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#if SR_ORACLE
#error This fixture must compile the native release layout.
#endif

namespace {
using Microsoft::WRL::ComPtr;
constexpr uint64_t kVs = 0xDADB000000000001ull, kPs = 0xDADB000000000002ull;
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void requireHr(HRESULT hr, const char* what) {
  if (FAILED(hr)) { std::fprintf(stderr, "%s hr=%08lX\n", what, (unsigned long)hr); throw std::runtime_error(what); }
}
void put(const std::filesystem::path& path, const void* bytes, size_t size) {
  std::ofstream out(path, std::ios::binary);
  out.write(static_cast<const char*>(bytes), std::streamsize(size));
  require(bool(out), "fixture cache write");
}
void makeCache(const std::filesystem::path& path) {
  require(std::filesystem::create_directory(path), "fresh fixture cache required");
  // Host shaders deliberately have no resource bindings. The production cache,
  // native pipeline service, target normalization and VSF staging still run.
  const char* vs = "float4 main(uint i:SV_VertexID):SV_Position { return float4((i==1)?0.5:-0.5,(i==2)?0.5:-0.5,0,1); }";
  const char* ps = "float4 main():SV_Target0 { return float4(0.25,0.5,0.75,1); }";
  for (bool vertex : {true, false}) {
    ComPtr<ID3DBlob> code, error;
    const char* source = vertex ? vs : ps;
    requireHr(D3DCompile(source, std::strlen(source), "native-draw-fixture", nullptr, nullptr,
                        "main", vertex ? "vs_5_0" : "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                        &code, &error), "fixture shader compile");
    const char* name = vertex ? "DADB000000000001.vs.dxil" : "DADB000000000002.ps.dxil";
    put(path / name, code->GetBufferPointer(), code->GetBufferSize());
    if (vertex) {
      put(path / "DADB000000000001.vs.vsf.dxil", code->GetBufferPointer(), code->GetBufferSize());
      constexpr char sidecar[] = "0 0 4 38 0 0 0 0\n";
      put(path / "DADB000000000001.vs.vfetch", sidecar, sizeof(sidecar)-1);
    }
  }
  _putenv_s("SR_SHADER_CACHE_DIR", path.string().c_str());
  auto& cache = sr::gfx::getShaderCache();
  cache.ensureLoaded();
  require(cache.installedPresent() && cache.findVs(kVs) && cache.findPs(kPs), "real installed shader cache");
}
}  // namespace

namespace sr::gfx {
struct NativeDrawAdmissionTestAccess {
  static void materialization(const std::string& mode) {
    // Exercise the real failure branch before any device or shader setup.
    // The held key deliberately has no cached target, so stale backing storage
    // cannot be mistaken for a successful materialization.
    D3D12Backend b;
    b.m_nbHeld.emplace_back();
    auto& held = b.m_nbHeld.back();
    held.on = mode != "materialize-inactive";
    held.key = 0x712300000001ull;
    held.x1 = held.y1 = 32;
    held.auxOn = mode == "materialize-missing-aux";
    held.depth = mode == "materialize-missing-stencil";
    b.m_nbHeldPage.resize(8);
    b.m_nbHeldPage[3] = 1;
    std::printf("[NATIVE_MATERIALIZE_TEST] ENTER mode=%s held=1 page=3\n", mode.c_str());
    std::fflush(stdout);
    b.nbMaterializeHeld(1);
    if (mode == "materialize-inactive") {
      require(!held.on && b.m_nbHeldPage[3] == 1 && b.m_meterCur.materializations == 0,
              "inactive-holder-has-no-materialization-side-effects");
      std::printf("[NATIVE_MATERIALIZE_TEST] PASS inactive-holder-noop\n");
      return;
    }
    require(held.on && b.m_nbHeldPage[3] == 1, "failed-materialization-retained-owner");
    throw std::runtime_error("failed-materialization-returned");
  }
  static void textureRanges(bool shortSpan) {
    // Actual native cache methods, without device/GPU setup. The short-span
    // control reproduces the former rows*pitch registration at the real tiled
    // sample that crosses into an otherwise unregistered 64 KiB block.
    D3D12Backend b;
    constexpr uint32_t base = 0x01001000u, mip = 0x02001000u, slot = 19;
    uint32_t f[6] = {2u | (8u << 22) | (1u << 31), base | 6u,
                     255u | (32u << 13), 0, 0, 0};
    const uint64_t key = ShaderCache::hashUcode(f, sizeof(f));
    uint32_t pitchBlocks = 0;
    const uint32_t extent = cube::detail::levelSliceStride(
        0, true, 256, 33, 256, 1, 1, 4, true, &pitchBlocks);
    const uint32_t sample = uint32_t(xenos_tiling::tiled2D(255, 32, pitchBlocks, 2));
    constexpr uint32_t oldSpan = 33u * 256u * 4u;
    require(extent == 0x10000u && oldSpan == 0x8400u && sample == 0xF0ECu,
            "actual tiled-tail geometry");
    b.texMarkClean(key, slot, base, shortSpan ? oldSpan : extent);
    require(b.getOrCreateTexture2D(f) == slot && b.m_texBindFast,
            "actual 2D fast-clean lookup");
    constexpr uint64_t unrelated = 0xA17223u;
    b.texMarkClean(unrelated, 20, 0x03000000u, 4096);
    const uint64_t generation = b.m_texBindGen.load();
    b.invalidateGuestRange(base + sample, 4);
    require(!b.m_texClean.count(key), "native-tiled-tail-invalidated");
    require(b.m_texBindGen.load() > generation && b.m_texClean.at(unrelated) == 20,
            "targeted invalidation advances bind generation and preserves unrelated key");

    // The second mark covers a separate mip allocation for the same key.
    b.texMarkClean(key, slot, base, extent);
    const auto layout = cube::computeGuestLayout(6, 256, 256, 256, true, 9,
                                                true, 1, 1, 4, 1);
    require(layout.ok && layout.mipsTotalExtentBytes, "real packed mip layout");
    b.texMarkClean(key, slot, mip, layout.mipsTotalExtentBytes);
    b.invalidateGuestRange(mip + layout.mipsTotalExtentBytes - 1, 1);
    require(!b.m_texClean.count(key), "native-mip-tail-invalidated");

    // Volume registration already uses the full aligned volume span.
    constexpr uint64_t volumeKey = 0x712311u;
    constexpr uint32_t volumeBase = 0x04001000u;
    constexpr uint32_t volumeSpan = 256u * 4u * 64u * 4u;
    b.texMarkClean(volumeKey, 21, volumeBase, volumeSpan);
    b.invalidateGuestRange(volumeBase + volumeSpan - 1, 1);
    require(!b.m_texClean.count(volumeKey), "native-volume-tail-invalidated");
    require(b.m_texClean.at(unrelated) == 20, "unrelated texture remains cached");
    std::printf("[NATIVE_TEXTURE_CLEAN_TEST] PASS tiled-tail mip-tail volume-tail unrelated fast-clean\n");
  }
  static void run(const std::string& mode) {
    D3D12Backend b;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    requireHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    requireHr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    requireHr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&b.m_device)), "WARP device");
    requireHr(b.m_nativePipelines.initialize(b.m_device.Get()), "native pipelines");
    requireHr(b.m_nativeRawGeometry.initialize(b.m_device.Get(), 65536, 65536), "native raw geometry");
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    requireHr(NativeRootSignature::create(b.m_device.Get(), desc, b.m_nativeXlatRoot), "fixture resource-free root");
    // Only the small CPU planning prefix is needed. Keep an already-open third
    // so the actual drain does not rotate to another 64 MiB planning partition.
    b.m_xlatVertices.init(65536, false);
    b.m_xlatConstants.init(65536, false);
    b.m_xlatVBPtr = b.m_xlatVertices.data();
    b.m_xlatCBPtr = b.m_xlatConstants.data();
    b.m_xlatReady = true;
    b.m_pm4DrainOpen = true;
    b.m_xlatThird = 0;
    b.m_width = b.m_height = 64;

    GuestDrawCommand c{};
    c.hasDraw = true;
    c.vertexCount = 3;
    c.drawOrdinal = 71;
    c.vsUcodeHash = mode == "missing-vs" ? kVs+10 : kVs;
    c.psUcodeHash = kPs;
    c.primType = 4;
    c.modeControl = 4;
    c.surfaceInfo = 80;
    c.colorMask = 15;
    c.blendControl = 0x00010001;
    c.vteCntl = 0x3F;
    c.vpScale[0] = 32; c.vpScale[1] = -32; c.vpScale[2] = 1;
    c.vpOffset[0] = c.vpOffset[1] = 32;
    c.windowScissorTL = 0x80000000u;
    c.windowScissorBR = c.screenScissorBR = (64u<<16)|64u;
    if (mode == "vsf-capacity") b.m_xlatCBUsed = b.kXlatCBBytes/3 - 256;
    // step 6 drop fix: draw 71 has an EMPTY viewport (x scale 0) - it writes its VS / PS blocks, names them for reuse
    // (SR_CP_CONST_DEDUP=1, set by the runner) and is dropped; draw 72 has the same constants and a real viewport, so
    // it binds draw 71's VS block. Its own new blocks (fetch, PS, shared) must not land on that block.
    GuestDrawCommand c2 = c;
    if (mode == "drop-then-reuse") {
      for (uint32_t i = 0; i < 1024; ++i) { c.aluVs[i] = 0x3F000000u + i; c.aluPs[i] = 0x40000000u + i; }
      c.vsConstUse = 0x0001u;
      c.psConstUse = 0x0001u;
      c2 = c;
      c2.drawOrdinal = 72;
      for (uint32_t i = 0; i < 1024; ++i) c2.aluPs[i] = 0x41000000u + i;   // a different PS: a new PS block
      c.vpScale[0] = 0.0f;                                                 // draw 71: an empty viewport
    }
    // step 6 I10p (SR_XLAT_CB_ZEROCOPY; the coordinator's memory split): the view path through the actual drain. The
    // twin is the real 192 MiB upload resource; the compact scratch is 3 x 4 KiB here so its boundary is reachable;
    // a cacheable shadow (0xAB) shows every byte the CP wrote into the twin (the twin itself is never read).
    const bool zc = mode.rfind("zc-", 0) == 0;
    std::vector<uint8_t> shadow;
    if (zc) {
      requireHr(b.m_xlatCBGpu.initialize(b.m_device.Get(), b.kXlatCBBytes), "upload twin");
      b.m_zcOn = true;
      b.m_xlatConstants.init(3u * 4096u, false);
      b.m_xlatCBPtr = b.m_xlatConstants.data();
      shadow.assign(1u << 20, 0xAB);
      b.m_xlatCBGpu.setShadow(shadow.data(), nullptr);
      // the shared block just fits (its end = the third's end) / just does not (one byte later: 256 bytes over)
      if (mode == "zc-shared-fits") b.m_xlatShUsed = 4096u - 512u;
      if (mode == "zc-shared-full") b.m_xlatShUsed = 4096u - 511u;
      for (uint32_t i = 0; i < 1024; ++i) { c.aluVs[i] = 0x3F000000u + i; c.aluPs[i] = 0x40000000u + i; }
      c.vsConstUse = 0x0005u;   // blocks 0 and 2 (M1: only those are written)
      c.psConstUse = 0x0002u;   // block 1
    }
    if (mode == "cb-shared-full") {
      // today's scratch path at the same boundary: fetch (768) + VS (4096) + PS (4096) + the shared block (512) = 9472
      // bytes from a 256-aligned cursor; started 9216 bytes before the third's end, the shared block ends 256 bytes past
      // it -> cbFull (code 8); the fetch block (written first) still fits
      b.m_xlatConstants.init(b.kXlatCBBytes, false);
      b.m_xlatCBPtr = b.m_xlatConstants.data();
      b.m_xlatCBUsed = b.kXlatCBBytes/3 - 9216;
    }
    if (mode == "zc-views") {   // a second, identical draw: M2 (its fetch block is the first draw's view)
      c2 = c;
      c2.drawOrdinal = 72;
    }
    auto& queue = getDrawCommandQueue();
    require(queue.size() == 0 && queue.push(c), "queue actual draw");
    if (mode == "drop-then-reuse" || mode == "zc-views") require(queue.push(c2), "queue the second draw");
    std::printf("[NATIVE_DRAW_TEST] ENTER mode=%s ordinal=71\n", mode.c_str());
    std::fflush(stdout);
    b.drainDrawQueue();
    if (mode == "drop-then-reuse") {
      require(queue.size() == 0 && b.m_drawBatches.size() == 1 && b.m_drawBatches[0].drawOrdinal == 72,
              "the empty-viewport draw is consumed without a batch; the second draw is the one batch");
      const auto& d = b.m_drawBatches[0];
      require(b.m_cdHits == 1, "the second draw binds the dropped draw's VS block (one reuse)");
      auto disjoint = [](uint64_t a, uint64_t an, uint64_t o, uint64_t on) { return a + an <= o || o + on <= a; };
      const uint64_t vs = d.xlatVsCbOff;
      require(disjoint(vs, 4096, d.xlatPsCbOff, 4096) && disjoint(vs, 4096, d.sharedCbOff, 512) &&
                  (!d.vsf || disjoint(vs, 4096, d.vsfFetchCbOff, 768)),
              "the reusing draw's own blocks do not land on the block it binds");
      require(std::memcmp(b.m_xlatCBPtr + vs, c.aluVs, 256) == 0, "the bound VS block still holds the draw's constants");
      std::printf("[NATIVE_DRAW_TEST] PASS drop-then-reuse batches=1 reused-block-intact ordinal=72\n");
      return;
    }
    if (zc) {
      require(mode == "zc-views" || mode == "zc-shared-fits", "rejected view-path draw returned instead of exit3");
      const size_t want = mode == "zc-views" ? 2u : 1u;
      require(queue.size() == 0 && b.m_drawBatches.size() == want, "view-path batch count");
      const auto& b0 = b.m_drawBatches[0];
      require(b0.xlat && b0.pm4 && b0.vsf && b0.zcView && b0.vertexCount == 3 && b0.drawOrdinal == 71,
              "view batch identity");
      auto aligned = [](uint64_t o) { return o % 256u == 0; };
      require(aligned(b0.xlatVsCbOff) && aligned(b0.xlatPsCbOff) && aligned(b0.vsfFetchCbOff) &&
                  aligned(b0.zcSharedOff) && b0.zcSharedOff + 512u <= b.m_xlatCBUsed &&
                  b0.sharedCbOff + 512u <= b.m_xlatConstants.size() / 3u,
              "views inside the CB third, the shared block inside the compact scratch's third");
      // M1 through the production drain: VS blocks 0 and 2, PS block 1 - every other byte of the 4 KiB extents untouched
      auto blockIs = [&](uint64_t off, uint32_t blk, const uint32_t* src) {
        return std::memcmp(shadow.data() + off + 256u * blk, src + 64u * blk, 256) == 0;
      };
      auto blockUntouched = [&](uint64_t off, uint32_t blk) {
        for (uint32_t i = 0; i < 256; ++i) if (shadow[off + 256u * blk + i] != 0xAB) return false;
        return true;
      };
      bool m1 = blockIs(b0.xlatVsCbOff, 0, c.aluVs) && blockIs(b0.xlatVsCbOff, 2, c.aluVs) &&
                blockIs(b0.xlatPsCbOff, 1, c.aluPs);
      for (uint32_t blk = 0; blk < 16; ++blk) {
        if (blk != 0 && blk != 2) m1 = m1 && blockUntouched(b0.xlatVsCbOff, blk);
        if (blk != 1) m1 = m1 && blockUntouched(b0.xlatPsCbOff, blk);
      }
      require(m1, "m1-only-the-used-blocks-written");
      bool slotUnwritten = true;   // the shared slot is the render thread's: the CP never writes it
      for (uint32_t i = 0; i < 512; ++i) slotUnwritten = slotUnwritten && shadow[b0.zcSharedOff + i] == 0xAB;
      require(slotUnwritten, "the CP leaves the shared slot to the render thread");
      if (mode == "zc-views") {
        const auto& b1 = b.m_drawBatches[1];
        require(b1.zcView && b1.vsfFetchCbOff == b0.vsfFetchCbOff && b.m_zcFetchWrites.load() == 1 &&
                    b.m_zcFetchDedups.load() == 1 && b1.zcSharedOff != b0.zcSharedOff,
                "m2-the-identical-fetch-block-is-the-first-view");
        std::printf("[NATIVE_DRAW_TEST] PASS view-path batches=2 m1 m2 shared-slot-unwritten ordinal=71\n");
      } else {
        require(b0.sharedCbOff == 4096u - 512u && b.m_xlatShUsed == 4096u, "the shared block ends at the third's end");
        std::printf("[NATIVE_DRAW_TEST] PASS view-path shared-block-fits-at-the-boundary ordinal=71\n");
      }
      return;
    }
    require(mode == "positive", "rejected draw returned instead of exit3");
    require(queue.size() == 0 && b.m_drawBatches.size() == 1, "exactly one consumed batch");
    const auto& batch = b.m_drawBatches[0];
    require(batch.xlat && batch.pm4 && batch.vsf && batch.xlatPso && batch.vertexCount == 3 &&
            batch.drawOrdinal == 71 && batch.vsHash == kVs && batch.psHash == kPs,
            "actual translated VSF batch identity");
    require(batch.vsfRawVertices && !batch.vsfShm && b.m_totalGeoVerts == 0,
            "owned native geometry without fixed fallback");
    std::printf("[NATIVE_DRAW_TEST] PASS translated-vsf-batch count=1 vertices=3 ordinal=71 fixed=0\n");
    // No command list or GPU submission was made. Normal destructors release
    // the real cache/arena and device; no queue lifetime is bypassed.
  }
};
}  // namespace sr::gfx

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string mode = argv[1];
  if (mode != "positive" && mode != "missing-vs" && mode != "vsf-capacity" && mode != "drop-then-reuse" &&
      mode != "cb-shared-full" && mode != "zc-views" && mode != "zc-shared-fits" && mode != "zc-shared-full" &&
      mode != "texture-ranges" && mode != "texture-short-span" &&
      mode != "materialize-inactive" && mode != "materialize-missing-target" &&
      mode != "materialize-missing-aux" && mode != "materialize-missing-stencil") return 2;
  _putenv_s("SR_SHADER_RUNTIME_XLAT", "0");
  _putenv_s("SR_SHADER_LOAD_ASYNC", "0");
  _putenv_s("SR_PM4_VSF_SHM", "0");
  _putenv_s("SR_MEM_RESOURCES", "0");
  _putenv_s("SR_LOG", "dev");
  sr::Log::init("native_draw.log");
  try {
    if (mode.rfind("materialize-", 0) == 0) {
      sr::gfx::NativeDrawAdmissionTestAccess::materialization(mode);
    } else if (mode == "texture-ranges" || mode == "texture-short-span") {
      sr::gfx::NativeDrawAdmissionTestAccess::textureRanges(mode == "texture-short-span");
    } else {
      makeCache(std::filesystem::absolute(argv[2]));
      sr::gfx::NativeDrawAdmissionTestAccess::run(mode);
    }
    sr::Log::shutdown();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "[NATIVE_DRAW_TEST] FAIL %s\n", error.what());
    sr::Log::shutdown();
    return 1;
  }
}
