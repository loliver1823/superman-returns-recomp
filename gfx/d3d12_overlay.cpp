// iter-464 HOST OVERLAY: the in-game mouse cursor (docs/MOUSE_CURSOR.md section 5).
//
// Drawn on the present path AFTER dumpBackBuffer() and BEFORE Present, at every present
// site (presentFramebuffer x3, pm4PresentFrame), in its own command list on the same queue.
// So SR_PRESENT_DUMP captures, replay_ab scores and golden comparisons never contain it,
// and a frame where the cursor is hidden records and submits nothing at all.
//
// The cursor is procedural: one quad around the tip, and the pixel shader evaluates a
// signed distance field of the arrow polygon. That keeps it crisp at any resolution
// (scale = the 16:9 game image height / 720, so 4K = x3), needs no texture, no descriptor heap and no vertex
// buffer (root constants + SV_VertexID only).
//
// Style (user request 2026-10-01, memory project_mouse_cursor): inspired by the game's UI
// but deliberately distinct from its blue/silver crystal chrome:
//   * a crisp arrow with a faceted CRYSTAL TIP (white -> pale gold, two facets + a glint),
//   * a Superman-red body with a gold rim (cape / S-shield colours),
//   * a high-contrast dark outline, a soft gold glow and a drop shadow, so it reads on the
//     bright title art and on the dark pause scrim alike,
//   * hover: a gold ring around the tip; click: a quick expanding pulse ring.
#ifdef _WIN32

#include "d3d12_backend.hpp"
#include "host_hlsl.hpp"  // step 4 7a: SR_HOST_HLSL_AOT (host HLSL compiled ahead of time)
#include "mod_library_ui.hpp"
#include "settings_ui.hpp"   // iter-466 gfx: the PC SETTINGS screen
#include "costume_viewer_draw.hpp"
#include "present_luma.hpp"
#include "../app/log.hpp"
#include "../kernel/mod_library.hpp"
#include "../kernel/mouse_ui.hpp"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace sr {
namespace gfx {

namespace {
const char kOverlayHlsl[] = R"HLSL(
cbuffer C : register(b0) {
  float2 gTip;      // tip, backbuffer px
  float2 gBb;       // backbuffer size, px
  float  gScale;    // px per design unit (720p = 1)
  float  gHover;    // 0..1
  float  gPulse;    // seconds since the click, <0 none
  float  gPressed;  // 0/1
  float  gTime;     // seconds
};
static const float2 kBoxMin = float2(-16.0, -16.0);   // design units around the tip
static const float2 kBoxMax = float2(64.0, 64.0);
struct VO { float4 pos : SV_Position; float2 p : TEXCOORD0; };
VO VSQuad(uint id : SV_VertexID) {
  float2 c = float2(float(id & 1u), float((id >> 1) & 1u));
  float2 d = lerp(kBoxMin, kBoxMax, c);
  float2 px = gTip + d * gScale;
  VO o;
  o.pos = float4(px.x / gBb.x * 2.0 - 1.0, 1.0 - px.y / gBb.y * 2.0, 0.0, 1.0);
  o.p = d;
  return o;
}
// The arrow, tip at the origin, y down, in 720p pixels.
static const int N = 7;
static const float2 V[N] = {
  float2(0.0, 0.0), float2(0.0, 22.0), float2(5.2, 17.4), float2(9.2, 26.2),
  float2(12.9, 24.5), float2(9.0, 15.9), float2(15.6, 15.9) };
float sdArrow(float2 p) {
  float d = dot(p - V[0], p - V[0]);
  float s = 1.0;
  [unroll] for (int i = 0, j = N - 1; i < N; j = i, i++) {
    float2 e = V[j] - V[i];
    float2 w = p - V[i];
    float2 b = w - e * saturate(dot(w, e) / dot(e, e));
    d = min(d, dot(b, b));
    bool c0 = p.y >= V[i].y, c1 = p.y < V[j].y, c2 = e.x * w.y > e.y * w.x;
    if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) s = -s;
  }
  return s * sqrt(d);
}
// Premultiplied "over".
float4 over(float4 top, float4 bot) { return top + bot * (1.0 - top.a); }
float4 PSCursor(VO i) : SV_Target {
  float2 p = i.p;
  float aa = 0.75 / max(gScale, 0.25);          // ~1 backbuffer px in design units
  float4 col = float4(0, 0, 0, 0);

  // Drop shadow (offset, soft).
  float ds = sdArrow(p - float2(1.8, 2.6));
  float sh = 0.50 * (1.0 - smoothstep(-1.0, 3.2, ds));
  col = over(float4(0.0, 0.0, 0.0, 1.0) * sh, col);

  float d = sdArrow(p);
  // Soft gold glow outside the outline (stronger on hover).
  float glowA = (0.30 + 0.30 * gHover) * exp(-max(d, 0.0) / 2.6) * step(0.0, d);
  col = over(float4(1.0, 0.72, 0.18, 1.0) * glowA, col);

  // Hover ring around the tip.
  if (gHover > 0.0) {
    float r = length(p - float2(2.0, 3.0));
    float ring = (1.0 - smoothstep(0.9, 0.9 + 2.0 * aa, abs(r - 10.5))) * gHover;
    col = over(float4(1.0, 0.83, 0.30, 1.0) * (0.85 * ring), col);
    float halo = (1.0 - smoothstep(0.0, 6.0, abs(r - 10.5))) * 0.18 * gHover;
    col = over(float4(1.0, 0.75, 0.20, 1.0) * halo, col);
  }
  // Click pulse: an expanding gold-white ring.
  if (gPulse >= 0.0 && gPulse < 0.40) {
    float k = gPulse / 0.40;
    float rad = 6.0 + 34.0 * k;
    float r = length(p - float2(2.0, 3.0));
    float w = 2.4 * (1.0 - k) + 0.6;
    float ring = (1.0 - smoothstep(w * 0.5, w * 0.5 + 2.0 * aa, abs(r - rad))) * (1.0 - k);
    col = over(float4(1.0, 0.92, 0.65, 1.0) * (0.9 * ring), col);
  }

  // The arrow: dark outline, gold rim, red body, crystal tip.
  float inside = 1.0 - smoothstep(-aa, aa, d);
  if (inside > 0.0) {
    float3 c;
    float outlineW = 1.55;
    float rimW = 1.45;
    // Crystal tip: the first ~11 units along the arrow axis.
    float2 axis = normalize(float2(0.42, 0.91));
    float along = dot(p, axis);
    float side = axis.x * p.y - axis.y * p.x;          // which facet
    float tipK = 1.0 - smoothstep(9.0, 12.5, along);   // 1 in the crystal, 0 in the body
    // Body: Superman red, lighter at the top, deeper toward the tail.
    float3 body = lerp(float3(0.86, 0.10, 0.12), float3(0.52, 0.03, 0.06), saturate(along / 26.0));
    body *= (gPressed > 0.5) ? 0.85 : 1.0;
    // Crystal: two facets (white / pale gold) and a moving glint.
    float facet = side > 0.0 ? 1.0 : 0.80;
    float3 crystal = lerp(float3(1.0, 1.0, 0.98), float3(1.0, 0.86, 0.52), saturate(along / 11.0)) * facet;
    float glint = exp(-pow((along - fmod(gTime * 9.0, 40.0) + 6.0) / 1.6, 2.0)) * tipK;
    crystal += glint * 0.35;
    // A thin facet line down the crystal's axis.
    float edge = (1.0 - smoothstep(0.0, 0.55 + aa, abs(side))) * tipK * 0.35;
    crystal = lerp(crystal, float3(0.75, 0.55, 0.20), edge);
    c = lerp(body, crystal, tipK);
    // Gold rim just inside the outline (brighter while pressed or hovered).
    float rim = 1.0 - smoothstep(-outlineW - rimW - aa, -outlineW - rimW + aa, d);
    float3 gold = lerp(float3(0.96, 0.70, 0.16), float3(1.0, 0.88, 0.40), max(gHover, gPressed));
    c = lerp(gold, c, rim);
    // Dark outline.
    float ol = smoothstep(-outlineW - aa, -outlineW + aa, d);
    c = lerp(c, float3(0.07, 0.02, 0.04), ol);
    col = over(float4(c, 1.0) * inside, col);
  }
  return col;
}
)HLSL";

void overlayBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  cl->ResourceBarrier(1, &b);
}

Microsoft::WRL::ComPtr<ID3DBlob> overlayCompile(const char* entry, const char* target) {
  Microsoft::WRL::ComPtr<ID3DBlob> code, err;
  const HRESULT hr = hostHlslCompile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "host_overlay.hlsl",
                                nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[MOUSE_UI] overlay %s (%s) compile failed: %s",
                entry, target, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return nullptr;
  }
  return code;
}
}  // namespace

void D3D12Backend::dumpThenOverlay(ID3D12Resource* rt) {
  static const bool s_cursorInDump = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_WITH_CURSOR");
    return e && e[0] == '1';
  }();
  if (s_cursorInDump) {        // review captures: the frames show the cursor
    drawHostOverlay(rt);
    dumpBackBuffer(rt);
    presentluma::onPresent(m_device.Get(), m_commandQueue.Get(), rt);
    return;
  }
  dumpBackBuffer(rt);          // default: captures never contain the cursor
  // X2 (2026-10-18): SR_PRESENT_LUMA's non-blocking row copy sees exactly what the dump sees
  presentluma::onPresent(m_device.Get(), m_commandQueue.Get(), rt);
  drawHostOverlay(rt);
}

void D3D12Backend::drawHostOverlay(ID3D12Resource* rt) {
  if (!rt || !m_device || !m_commandQueue) return;
  // iter-465 modlib: the mod library (gfx/mod_library_ui.cpp) draws first, the cursor on top.
  const bool lib = modlibui::wantsFrame();
  const bool pcs = settingsui::wantsFrame(lib);   // iter-466 gfx: PC SETTINGS + the perf overlay
  const bool viewer = cviewdraw::wantsFrame();   // iter-465 viewer: the COSTUME VIEWER model
  const bool cursorOn = sr::kernel::mouseUiEnabled() && !m_overlayFailed;
  if (!lib && !pcs && !viewer && !cursorOn) return;
  // Only the swapchain buffer of this frame (its RTV slot is m_frameIndex).
  if (m_frameIndex >= UINT(kFrameCount) || rt != m_renderTargets[m_frameIndex].Get()) return;
  // The backbuffer's real size: a resizable / fullscreen swapchain (render/resscale) is not
  // the initial m_width x m_height.
  const D3D12_RESOURCE_DESC bbDesc = rt->GetDesc();
  const int bbW = int(bbDesc.Width), bbH = int(bbDesc.Height);
  if (bbW <= 0 || bbH <= 0) return;
  const sr::kernel::CursorOverlayState cs =
      cursorOn ? sr::kernel::mouseUiOverlayState(bbW, bbH) : sr::kernel::CursorOverlayState{};
  if (!lib && !pcs && !viewer && !cs.visible) return;
  // The command list + allocators are shared by both layers; the cursor's PSO is its own.
  if (!m_overlayList) {
    bool ok = true;
    for (int i = 0; i < kFrameCount && ok; ++i)
      ok = SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&m_overlayAlloc[i])));
    ok = ok && SUCCEEDED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     m_overlayAlloc[0].Get(), nullptr,
                                                     IID_PPV_ARGS(&m_overlayList)));
    if (!ok) {
      m_overlayFailed = true;
      m_overlayList.Reset();
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[MOUSE_UI] overlay init failed at command list - no host overlay is drawn");
      return;
    }
    m_overlayList->Close();
  }
  if (cs.visible && !m_overlayPso && !m_overlayFailed) {
    auto fail = [&](const char* what) {
      m_overlayFailed = true;
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[MOUSE_UI] overlay init failed at %s - the cursor is not drawn", what);
    };
    [&] {
      D3D12_ROOT_PARAMETER p{};
      p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      p.Constants.Num32BitValues = 9;
      p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC rd{};
      rd.NumParameters = 1;
      rd.pParameters = &p;
      Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
      if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
          FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&m_overlayRs))))
        return fail("root signature");
      auto vs = overlayCompile("VSQuad", "vs_5_1");
      auto ps = overlayCompile("PSCursor", "ps_5_1");
      if (!vs || !ps) return fail("shader compile");
      D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
      gd.pRootSignature = m_overlayRs.Get();
      gd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
      gd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
      auto& bt = gd.BlendState.RenderTarget[0];
      bt.BlendEnable = TRUE;
      bt.SrcBlend = D3D12_BLEND_ONE;              // premultiplied alpha
      bt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      bt.BlendOp = D3D12_BLEND_OP_ADD;
      bt.SrcBlendAlpha = D3D12_BLEND_ONE;
      bt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
      bt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      bt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      gd.SampleMask = UINT_MAX;
      gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      gd.RasterizerState.DepthClipEnable = TRUE;
      gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      gd.NumRenderTargets = 1;
      gd.RTVFormats[0] = bbDesc.Format;
      gd.SampleDesc.Count = 1;
      if (FAILED(m_device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&m_overlayPso))))
        return fail("pso");
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MOUSE_UI] cursor overlay ready (rtv fmt %u)",
                  unsigned(gd.RTVFormats[0]));
    }();
  }
  // moveToNextFrame() waited for this frame index's fence before we got here, so the
  // allocator's previous use is complete.
  ID3D12CommandAllocator* alloc = m_overlayAlloc[m_frameIndex].Get();
  if (FAILED(alloc->Reset()) || FAILED(m_overlayList->Reset(alloc, nullptr))) {
    static int s_warn = 0;
    if (s_warn++ < 3)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MOUSE_UI] overlay list reset failed - frame skipped");
    return;
  }
  overlayBarrier(m_overlayList.Get(), rt, D3D12_RESOURCE_STATE_PRESENT,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(m_frameIndex) * m_rtvDescriptorSize;
  m_overlayList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0.0f, 0.0f, float(bbW), float(bbH), 0.0f, 1.0f};
  const D3D12_RECT sc{0, 0, LONG(bbW), LONG(bbH)};
  m_overlayList->RSSetViewports(1, &vp);
  m_overlayList->RSSetScissorRects(1, &sc);
  if (viewer) {
    cviewdraw::FrameCtx vc;
    vc.device = m_device.Get();
    vc.cl = m_overlayList.Get();
    vc.rtv = rtv;
    vc.rtvFormat = bbDesc.Format;
    vc.bbW = bbW;
    vc.bbH = bbH;
    vc.frameIndex = m_frameIndex;
    vc.frameCount = kFrameCount;
    cviewdraw::render(vc);
    m_overlayList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_overlayList->RSSetViewports(1, &vp);
    m_overlayList->RSSetScissorRects(1, &sc);
  }
  if (lib) {
    modlibui::FrameCtx fc;
    fc.device = m_device.Get();
    fc.queue = m_commandQueue.Get();
    fc.cl = m_overlayList.Get();
    fc.rtvFormat = bbDesc.Format;
    fc.bbW = bbW;
    fc.bbH = bbH;
    modlibui::render(fc);
    // the library's ImGui pass set its own state: restore ours for the cursor
    m_overlayList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_overlayList->RSSetViewports(1, &vp);
    m_overlayList->RSSetScissorRects(1, &sc);
  }
  if (pcs) {
    settingsui::FrameCtx fc;
    fc.device = m_device.Get();
    fc.queue = m_commandQueue.Get();
    fc.cl = m_overlayList.Get();
    fc.rtvFormat = bbDesc.Format;
    fc.bbW = bbW;
    fc.bbH = bbH;
    settingsui::render(fc, lib);
    m_overlayList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_overlayList->RSSetViewports(1, &vp);
    m_overlayList->RSSetScissorRects(1, &sc);
  }
  if (cs.visible && m_overlayPso) {
    m_overlayList->SetPipelineState(m_overlayPso.Get());
    m_overlayList->SetGraphicsRootSignature(m_overlayRs.Get());
    const float c[9] = {cs.x, cs.y, float(bbW), float(bbH), cs.scale,
                        cs.hover, cs.pulse, cs.pressed, cs.timeSec};
    m_overlayList->SetGraphicsRoot32BitConstants(0, 9, c, 0);
    m_overlayList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_overlayList->DrawInstanced(4, 1, 0, 0);
  }
  // iter-465 modlib: SR_MOD_LIBRARY_SHOTS + F12 - the composited frame, overlay included.
  const bool shot = (lib && modlibui::shotRequested()) | (pcs && settingsui::shotRequested());
  Microsoft::WRL::ComPtr<ID3D12Resource> rb;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  if (shot) {
    overlayBarrier(m_overlayList.Get(), rt, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    UINT nr = 0;
    UINT64 rs = 0, tb = 0;
    m_device->GetCopyableFootprints(&bbDesc, 0, 1, 0, &fp, &nr, &rs, &tb);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = tb;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&rb)))) {
      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
      dst.pResource = rb.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp;
      src.pResource = rt;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;
      m_overlayList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    overlayBarrier(m_overlayList.Get(), rt, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_PRESENT);
  } else {
    overlayBarrier(m_overlayList.Get(), rt, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PRESENT);
  }
  if (FAILED(m_overlayList->Close())) return;
  ID3D12CommandList* l[] = {m_overlayList.Get()};
  m_commandQueue->ExecuteCommandLists(1, l);
  if (rb) writeOverlayShot(rb.Get(), fp, bbDesc.Format);
}

// Waits for the copy, then writes <SR_MOD_LIBRARY_SHOTS>/modlib_<n>_<w>x<h>.bmp (a developer
// capture: the frame the player sees, host overlays included - present dumps never have them).
void D3D12Backend::writeOverlayShot(ID3D12Resource* readback,
                                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, DXGI_FORMAT fmt) {
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  m_commandQueue->Signal(fence.Get(), 1);
  if (fence->GetCompletedValue() < 1) {
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 10000);
  }
  CloseHandle(ev);
  void* mp = nullptr;
  const D3D12_RANGE rr{0, SIZE_T(fp.Footprint.RowPitch) * fp.Footprint.Height};
  if (FAILED(readback->Map(0, &rr, &mp))) return;
  const uint32_t w = fp.Footprint.Width, h = fp.Footprint.Height;
  const bool bgr = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  const uint32_t rowOut = (w * 3u + 3u) & ~3u;
  std::vector<uint8_t> out(size_t(rowOut) * h + 54u, 0);
  uint8_t* hdr = out.data();
  auto le32 = [&](size_t o, uint32_t v) { std::memcpy(hdr + o, &v, 4); };
  hdr[0] = 0x42;
  hdr[1] = 0x4D;
  le32(2, uint32_t(out.size()));
  le32(10, 54);
  le32(14, 40);
  le32(18, w);
  le32(22, h);
  hdr[26] = 1;
  hdr[28] = 24;
  le32(34, rowOut * h);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* src = static_cast<const uint8_t*>(mp) + size_t(y) * fp.Footprint.RowPitch;
    uint8_t* dst = out.data() + 54u + size_t(h - 1 - y) * rowOut;
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* px = src + size_t(x) * 4u;
      dst[x * 3 + 0] = bgr ? px[0] : px[2];
      dst[x * 3 + 1] = px[1];
      dst[x * 3 + 2] = bgr ? px[2] : px[0];
    }
  }
  readback->Unmap(0, nullptr);
  static int s_n = 0;
  const std::string& dir = sr::kernel::modlib::shotDir();
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  char name[64];
  std::snprintf(name, sizeof name, "/modlib_%03d_%ux%u.bmp", s_n++, w, h);
  const std::string path = dir + name;
  if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MODLIB] shot %s", path.c_str());
  }
}

}  // namespace gfx
}  // namespace sr

#endif  // _WIN32
