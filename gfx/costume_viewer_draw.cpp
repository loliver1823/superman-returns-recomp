// iter-465 viewer lane: the COSTUME VIEWER's 3D model pass (see costume_viewer_draw.hpp).
#ifdef _WIN32
#include "costume_viewer_draw.hpp"
#include "host_hlsl.hpp"  // step 4 7a: SR_HOST_HLSL_AOT (host HLSL compiled ahead of time)

#include "../app/log.hpp"
#include "../kernel/costume_viewer.hpp"
#include "../kernel/costume_viewer_model.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace sr::gfx::cviewdraw {
namespace {
using Microsoft::WRL::ComPtr;
using sr::kernel::cview::Model;

const char kHlsl[] = R"HLSL(
cbuffer C : register(b0) {
  row_major float4x4 VP;
  float4 Eye;
  float4 KeyDir;
  float4 FillDir;
  float4 Flags;   // x silhouette, y textured, z alpha test, w cape (two-sided lighting)
};
Texture2D T : register(t0);
SamplerState S : register(s0);
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct VO { float4 pos : SV_Position; float3 n : NORMAL; float3 w : TEXCOORD1; float2 uv : TEXCOORD0; };
VO VS(VI i) {
  VO o;
  o.pos = mul(VP, float4(i.p, 1));
  o.n = i.n; o.w = i.p; o.uv = i.uv;
  return o;
}
float4 PS(VO i, bool front : SV_IsFrontFace) : SV_Target {
  float3 n = normalize(i.n);
  float3 v = normalize(Eye.xyz - i.w);
  if (Flags.w > 0.5 && dot(n, v) < 0) n = -n;
  float4 albedo = Flags.y > 0.5 ? T.Sample(S, i.uv) : float4(0.72, 0.74, 0.8, 1);
  if (Flags.z > 0.5) clip(albedo.a - 0.3);
  float rim = pow(1 - saturate(dot(n, v)), 3.0);
  if (Flags.x > 0.5) return float4(float3(0.015, 0.02, 0.05) + rim * float3(0.42, 0.6, 1.0), 1);
  float key = saturate(dot(n, KeyDir.xyz));
  float fill = saturate(dot(n, FillDir.xyz));
  float3 h = normalize(KeyDir.xyz + v);
  float spec = pow(saturate(dot(n, h)), 28) * 0.16;
  float3 lit = float3(0.30, 0.32, 0.38) + key * float3(1.02, 0.98, 0.92) + fill * float3(0.18, 0.24, 0.36);
  float3 c = albedo.rgb * lit + spec + rim * float3(0.30, 0.40, 0.62);
  return float4(c, 1);
}
// The backdrop behind the suit (our own art, procedural): a faint S-shield outline and a glowing
// oval ring, drawn into the page's open centre before the model. VP[0].x = rect aspect, Eye.w = t.
struct BO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
BO VSBack(uint id : SV_VertexID) {
  BO o;
  float2 q = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
  o.uv = q;
  o.pos = float4(q.x * 2 - 1, 1 - q.y * 2, 0, 1);
  return o;
}
float sdSeg(float2 p, float2 a, float2 b) { float2 pa = p - a, ba = b - a; float h = saturate(dot(pa, ba) / dot(ba, ba)); return length(pa - ba * h); }
float4 PSBack(BO i) : SV_Target {
  float aspect = VP[0].x;
  float t = Eye.w;
  float2 p = float2((i.uv.x - 0.5) * aspect, 0.5 - i.uv.y) * 2.0;   // y up, height 2
  // the shield: wide top, short shoulders, a long point at the bottom
  float2 v0 = float2(-0.62, 0.70), v1 = float2(0.62, 0.70), v2 = float2(0.86, 0.34), v3 = float2(0.0, -0.80), v4 = float2(-0.86, 0.34);
  float d = min(min(sdSeg(p, v0, v1), sdSeg(p, v1, v2)), min(min(sdSeg(p, v2, v3), sdSeg(p, v3, v4)), sdSeg(p, v4, v0)));
  float edge = exp(-d * d / 0.0009) * 0.30 + exp(-d * d / 0.012) * 0.10;
  float3 col = float3(0.95, 0.72, 0.25) * edge;
  float a = edge;
  // the oval ring
  float2 e = p / float2(0.78, 0.98);
  float r = length(e);
  float pulse = 0.85 + 0.15 * sin(t * 1.3);
  float ring = exp(-pow((r - 1.0) / 0.035, 2)) * 0.85 * pulse + exp(-pow((r - 1.0) / 0.16, 2)) * 0.22;
  float inner = saturate(1.0 - r) * 0.10;
  col += float3(0.55, 0.78, 1.0) * ring + float3(0.25, 0.42, 0.85) * inner;
  a = saturate(a + ring + inner);
  return float4(col, a);
}
)HLSL";

constexpr int kMaxFrames = 3;
constexpr UINT kHeapSlots = 64;

struct Gpu {
  bool failed = false;
  ComPtr<ID3D12RootSignature> rs;
  ComPtr<ID3D12PipelineState> pso, psoBack;
  DXGI_FORMAT psoFormat = DXGI_FORMAT_UNKNOWN;
  ComPtr<ID3D12DescriptorHeap> srvHeap, dsvHeap;
  UINT srvInc = 0;
  ComPtr<ID3D12Resource> depth;
  int depthW = 0, depthH = 0;
  uint64_t frame = 0;
  struct Retired { ComPtr<ID3D12Resource> r; uint64_t at; };
  std::vector<Retired> retired;
  // the current model
  std::shared_ptr<const Model> model;
  std::vector<ComPtr<ID3D12Resource>> tex;   // per model texture
  ComPtr<ID3D12Resource> ib;
  std::vector<uint32_t> ibOffset, vbOffset;  // per mesh (indices, vertices)
  ComPtr<ID3D12Resource> vb[kMaxFrames];
  uint8_t* vbMap[kMaxFrames] = {};
  size_t vbBytes = 0;
};
Gpu& G() { static Gpu g; return g; }

void retire(ComPtr<ID3D12Resource>& r) {
  if (r) G().retired.push_back({r, G().frame});
  r.Reset();
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* dev, size_t bytes) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = std::max<size_t>(bytes, 256);
  d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&r)))) return nullptr;
  return r;
}

void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER x{};
  x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  x.Transition.pResource = r;
  x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  x.Transition.StateBefore = a;
  x.Transition.StateAfter = b;
  cl->ResourceBarrier(1, &x);
}

bool initPipeline(const FrameCtx& f) {
  Gpu& g = G();
  auto fail = [&](const char* what) {
    g.failed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[COSTUME_VIEWER] model pass init failed at %s", what);
    return false;
  };
  if (!g.rs) {
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 32;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &range;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2; rd.pParameters = p;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &s;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(f.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rs))))
      return fail("root signature");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srvHeap)))) return fail("srv heap");
    g.srvInc = f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC dd{};
    dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dd.NumDescriptors = 1;
    if (FAILED(f.device->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&g.dsvHeap)))) return fail("dsv heap");
    // slot 0: a null texture (untextured meshes)
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    f.device->CreateShaderResourceView(nullptr, &sd, g.srvHeap->GetCPUDescriptorHandleForHeapStart());
  }
  if (!g.pso || g.psoFormat != f.rtvFormat) {
    auto compile = [&](const char* entry, const char* target) {
      ComPtr<ID3DBlob> code, err;
      if (FAILED(sr::gfx::hostHlslCompile(kHlsl, sizeof(kHlsl) - 1, "costume_viewer.hlsl", nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[COSTUME_VIEWER] %s: %s", entry,
                    err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return ComPtr<ID3DBlob>();
      }
      return code;
    };
    auto vs = compile("VS", "vs_5_1"), ps = compile("PS", "ps_5_1");
    if (!vs || !ps) return fail("shader compile");
    D3D12_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = g.rs.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.InputLayout = {il, 3};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = f.rtvFormat;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso, back;
    if (FAILED(f.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return fail("pso");
    auto bvs = compile("VSBack", "vs_5_1"), bps = compile("PSBack", "ps_5_1");
    if (!bvs || !bps) return fail("backdrop shader compile");
    d.VS = {bvs->GetBufferPointer(), bvs->GetBufferSize()};
    d.PS = {bps->GetBufferPointer(), bps->GetBufferSize()};
    d.InputLayout = {nullptr, 0};
    d.DepthStencilState.DepthEnable = FALSE;
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    auto& bt = d.BlendState.RenderTarget[0];
    bt.BlendEnable = TRUE;
    bt.SrcBlend = D3D12_BLEND_ONE;              // premultiplied
    bt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    bt.BlendOp = D3D12_BLEND_OP_ADD;
    bt.SrcBlendAlpha = D3D12_BLEND_ONE;
    bt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    bt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (FAILED(f.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&back)))) return fail("backdrop pso");
    g.pso = pso;
    g.psoBack = back;
    g.psoFormat = f.rtvFormat;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME_VIEWER] model pass ready (rtv fmt %u)", unsigned(f.rtvFormat));
  }
  if (!g.depth || g.depthW != f.bbW || g.depthH != f.bbH) {
    retire(g.depth);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = UINT64(f.bbW); d.Height = UINT(f.bbH); d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    if (FAILED(f.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                 IID_PPV_ARGS(&g.depth)))) return fail("depth");
    f.device->CreateDepthStencilView(g.depth.Get(), nullptr, g.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    g.depthW = f.bbW; g.depthH = f.bbH;
  }
  return true;
}

// Box-filtered mip chain of an RGBA8 image.
std::vector<std::vector<uint8_t>> mips(const sr::kernel::cview::Texture& t) {
  std::vector<std::vector<uint8_t>> out{t.rgba};
  int w = t.w, h = t.h;
  while (w > 1 || h > 1) {
    const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    const auto& s = out.back();
    std::vector<uint8_t> d(size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y) for (int x = 0; x < nw; ++x) for (int c = 0; c < 4; ++c) {
      int sum = 0;
      for (int k = 0; k < 4; ++k) {
        const int sx = std::min(w - 1, x * 2 + (k & 1)), sy = std::min(h - 1, y * 2 + (k >> 1));
        sum += s[(size_t(sy) * w + sx) * 4 + c];
      }
      d[(size_t(y) * nw + x) * 4 + c] = uint8_t(sum / 4);
    }
    out.push_back(std::move(d));
    w = nw; h = nh;
  }
  return out;
}

bool bindModel(const FrameCtx& f, const std::shared_ptr<const Model>& m) {
  Gpu& g = G();
  for (auto& t : g.tex) retire(t);
  g.tex.clear();
  retire(g.ib);
  for (int i = 0; i < kMaxFrames; ++i) { retire(g.vb[i]); g.vbMap[i] = nullptr; }
  g.model = m;
  if (!m) return true;
  // indices + per-mesh offsets
  size_t ni = 0, nv = 0;
  g.ibOffset.clear(); g.vbOffset.clear();
  for (const auto& me : m->meshes) { g.ibOffset.push_back(uint32_t(ni)); g.vbOffset.push_back(uint32_t(nv)); ni += me.idx.size(); nv += me.pos.size() / 3; }
  g.ib = buffer(f.device, ni * 4);
  if (!g.ib) return false;
  uint8_t* p = nullptr;
  D3D12_RANGE none{0, 0};
  if (FAILED(g.ib->Map(0, &none, reinterpret_cast<void**>(&p)))) return false;
  for (size_t i = 0; i < m->meshes.size(); ++i)
    std::memcpy(p + size_t(g.ibOffset[i]) * 4, m->meshes[i].idx.data(), m->meshes[i].idx.size() * 4);
  g.ib->Unmap(0, nullptr);
  g.vbBytes = nv * 32;
  for (unsigned i = 0; i < f.frameCount && i < unsigned(kMaxFrames); ++i) {
    g.vb[i] = buffer(f.device, g.vbBytes);
    if (!g.vb[i] || FAILED(g.vb[i]->Map(0, &none, reinterpret_cast<void**>(&g.vbMap[i])))) return false;
  }
  // textures: default heap + mips, uploaded on this frame's list
  for (size_t ti = 0; ti < m->textures.size() && ti + 1 < kHeapSlots; ++ti) {
    const auto& t = m->textures[ti];
    const auto chain = mips(t);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = UINT64(t.w); d.Height = UINT(t.h); d.DepthOrArraySize = 1; d.MipLevels = UINT16(chain.size());
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> tex;
    if (FAILED(f.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&tex)))) return false;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(chain.size());
    std::vector<UINT> rows(chain.size());
    std::vector<UINT64> rowBytes(chain.size());
    UINT64 total = 0;
    f.device->GetCopyableFootprints(&d, 0, UINT(chain.size()), 0, fp.data(), rows.data(), rowBytes.data(), &total);
    ComPtr<ID3D12Resource> up = buffer(f.device, size_t(total));
    uint8_t* u = nullptr;
    if (!up || FAILED(up->Map(0, &none, reinterpret_cast<void**>(&u)))) return false;
    for (size_t mi = 0; mi < chain.size(); ++mi)
      for (UINT y = 0; y < rows[mi]; ++y)
        std::memcpy(u + fp[mi].Offset + UINT64(y) * fp[mi].Footprint.RowPitch, chain[mi].data() + y * rowBytes[mi], size_t(rowBytes[mi]));
    up->Unmap(0, nullptr);
    for (size_t mi = 0; mi < chain.size(); ++mi) {
      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
      dst.pResource = tex.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = UINT(mi);
      src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp[mi];
      f.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier(f.cl, tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    retire(up);   // released once this frame's list has run
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = UINT(chain.size());
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(1 + ti) * g.srvInc;
    f.device->CreateShaderResourceView(tex.Get(), &sd, h);
    g.tex.push_back(tex);
  }
  return true;
}

using M4 = std::array<float, 16>;
M4 mul4(const M4& a, const M4& b) {
  M4 r{};
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { float s = 0; for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; }
  return r;
}
void norm3(float* v) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) for (int i = 0; i < 3; ++i) v[i] /= l; }

}  // namespace

bool wantsFrame() { return !G().failed && sr::kernel::costumeViewerEnabled() && sr::kernel::costumeViewerFrame().visible; }

bool render(const FrameCtx& f) {
  Gpu& g = G();
  ++g.frame;
  // release what no in-flight list can still read
  g.retired.erase(std::remove_if(g.retired.begin(), g.retired.end(),
                                 [&](const Gpu::Retired& r) { return g.frame > r.at + unsigned(f.frameCount) + 2; }),
                  g.retired.end());
  if (g.failed || !f.device || !f.cl || f.bbW <= 0 || f.bbH <= 0) return false;
  const auto vf = sr::kernel::costumeViewerFrame();
  if (!vf.visible) return false;
  if (!initPipeline(f)) return false;
  if (vf.model != g.model && !bindModel(f, vf.model)) {
    g.failed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[COSTUME_VIEWER] model upload failed");
    return false;
  }
  // the page's open centre (the game's 16:9 image inside the back buffer)
  const float W = float(f.bbW), H = float(f.bbH);
  float fw, fh;
  if (W * 720.0f <= H * 1280.0f) { fw = W; fh = W * 720.0f / 1280.0f; } else { fh = H; fw = H * 1280.0f / 720.0f; }
  const float ox = (W - fw) * 0.5f, oy = (H - fh) * 0.5f;
  const float rx0 = ox + vf.rect[0] * fw, ry0 = oy + vf.rect[1] * fh, rx1 = ox + vf.rect[2] * fw, ry1 = oy + vf.rect[3] * fh;
  const float rw = rx1 - rx0, rh = ry1 - ry0;
  const D3D12_RECT sc{LONG(rx0), LONG(ry0), LONG(rx1), LONG(ry1)};
  const D3D12_VIEWPORT vpo{rx0, ry0, rw, rh, 0.0f, 1.0f};
  {
    float bc[32] = {};
    bc[0] = rw / rh;
    bc[19] = float(vf.seconds);
    f.cl->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    f.cl->RSSetViewports(1, &vpo);
    f.cl->RSSetScissorRects(1, &sc);
    f.cl->SetPipelineState(g.psoBack.Get());
    f.cl->SetGraphicsRootSignature(g.rs.Get());
    f.cl->SetGraphicsRoot32BitConstants(0, 32, bc, 0);
    f.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    f.cl->DrawInstanced(4, 1, 0, 0);
  }
  if (!g.model) return true;
  const Model& m = *g.model;
  const unsigned fi = f.frameIndex % unsigned(kMaxFrames);
  if (!g.vbMap[fi]) return false;
  // pose + skin into this frame's vertex buffer
  std::vector<float> sk, v;
  sr::kernel::cview::evalSkin(m, vf.seconds, vf.silhouette ? 0.0f : 0.09f, &sk);
  for (size_t i = 0; i < m.meshes.size(); ++i) {
    sr::kernel::cview::skinMesh(m.meshes[i], sk, &v);
    std::memcpy(g.vbMap[fi] + size_t(g.vbOffset[i]) * 32, v.data(), v.size() * 4);
  }
  // camera: orbit the suit (model space, Y up, facing +Z)
  const float* mn = m.boundsMin; const float* mx = m.boundsMax;
  const float hgt = std::max(0.5f, mx[1] - mn[1]);
  const float cx = 0.5f * (mn[0] + mx[0]), cy = 0.5f * (mn[1] + mx[1]), cz = 0.5f * (mn[2] + mx[2]);
  const float fov = 0.52f, aspect = rw / rh;
  const float visH = hgt * 1.10f / vf.zoom;
  const float dist = (visH * 0.5f) / std::tan(fov * 0.5f);
  // zoomed in, the view slides up to the head and shoulders (a margin above the head)
  const float zt = std::clamp((vf.zoom - 1.0f) / 0.5f, 0.0f, 1.0f);
  const float ty = cy + (std::max(cy, mx[1] - visH * 0.42f) - cy) * zt;
  float eye[3] = {cx + std::sin(vf.yaw) * dist, ty + vf.pitch * dist, cz + std::cos(vf.yaw) * dist};
  const float tgt[3] = {cx, ty, cz};
  float zA[3] = {eye[0] - tgt[0], eye[1] - tgt[1], eye[2] - tgt[2]};
  norm3(zA);
  float xA[3] = {zA[2], 0, -zA[0]};   // up x z
  norm3(xA);
  const float yA[3] = {zA[1] * xA[2] - zA[2] * xA[1], zA[2] * xA[0] - zA[0] * xA[2], zA[0] * xA[1] - zA[1] * xA[0]};
  const M4 view = {xA[0], xA[1], xA[2], -(xA[0] * eye[0] + xA[1] * eye[1] + xA[2] * eye[2]),
                   yA[0], yA[1], yA[2], -(yA[0] * eye[0] + yA[1] * eye[1] + yA[2] * eye[2]),
                   zA[0], zA[1], zA[2], -(zA[0] * eye[0] + zA[1] * eye[1] + zA[2] * eye[2]),
                   0, 0, 0, 1};
  const float nearZ = 0.05f, farZ = 50.0f, fy = 1.0f / std::tan(fov * 0.5f);
  const M4 proj = {fy / aspect, 0, 0, 0, 0, fy, 0, 0, 0, 0, farZ / (nearZ - farZ), nearZ * farZ / (nearZ - farZ), 0, 0, -1, 0};
  const M4 vp = mul4(proj, view);
  // lights fixed to the camera: key from upper left front, fill from the right
  float key[3], fill[3];
  for (int i = 0; i < 3; ++i) { key[i] = -0.55f * xA[i] + 0.62f * yA[i] + 0.56f * zA[i]; fill[i] = 0.8f * xA[i] + 0.1f * yA[i] + 0.3f * zA[i]; }
  norm3(key); norm3(fill);
  float cb[32] = {};
  std::memcpy(cb, vp.data(), 64);
  cb[16] = eye[0]; cb[17] = eye[1]; cb[18] = eye[2];
  cb[20] = key[0]; cb[21] = key[1]; cb[22] = key[2];
  cb[24] = fill[0]; cb[25] = fill[1]; cb[26] = fill[2];
  cb[28] = vf.silhouette ? 1.0f : 0.0f;
  // record
  ID3D12GraphicsCommandList* cl = f.cl;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = g.dsvHeap->GetCPUDescriptorHandleForHeapStart();
  cl->OMSetRenderTargets(1, &f.rtv, FALSE, &dsv);
  cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &sc);
  cl->RSSetViewports(1, &vpo);
  cl->RSSetScissorRects(1, &sc);
  cl->SetPipelineState(g.pso.Get());
  cl->SetGraphicsRootSignature(g.rs.Get());
  ID3D12DescriptorHeap* heaps[] = {g.srvHeap.Get()};
  cl->SetDescriptorHeaps(1, heaps);
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VERTEX_BUFFER_VIEW vbv{g.vb[fi]->GetGPUVirtualAddress(), UINT(g.vbBytes), 32};
  D3D12_INDEX_BUFFER_VIEW ibv{g.ib->GetGPUVirtualAddress(), UINT(g.ib->GetDesc().Width), DXGI_FORMAT_R32_UINT};
  cl->IASetVertexBuffers(0, 1, &vbv);
  cl->IASetIndexBuffer(&ibv);
  for (size_t i = 0; i < m.meshes.size(); ++i) {
    const auto& me = m.meshes[i];
    const bool tex = me.texture >= 0 && size_t(me.texture) < g.tex.size();
    cb[29] = tex ? 1.0f : 0.0f;
    cb[30] = me.cape && tex ? 1.0f : 0.0f;
    cb[31] = me.cape ? 1.0f : 0.0f;
    cl->SetGraphicsRoot32BitConstants(0, 32, cb, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(tex ? 1 + me.texture : 0) * g.srvInc;
    cl->SetGraphicsRootDescriptorTable(1, h);
    cl->DrawIndexedInstanced(UINT(me.idx.size()), 1, g.ibOffset[i], INT(g.vbOffset[i]), 0);
  }
  cl->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
  return true;
}

}  // namespace sr::gfx::cviewdraw
#endif
