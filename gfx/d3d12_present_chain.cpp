// iter-466 gfx (PORT FEATURE, docs/GRAPHICS_SETTINGS.md): THE PRESENT CHAIN.
//
// The ordered, named host stages between the scan-out and the swap-chain buffer:
//
//   scan-out (pass 1, gfx/d3d12_pm4_shmem.cpp: the front buffer at the INTERNAL resolution
//             through the game's display gamma ramp, into m_outTex)
//     -> post     internal res   post-process anti-aliasing (FXAA)        gfxset  post_aa
//     -> colour   internal res   brightness / gamma, after the game's ramp gfxset  brightness, gamma
//     -> scale    output rect    upscaling filter (bilinear / CAS / FSR)   res4k   upscale_filter
//     -> sharpen  output rect    CAS / RCAS sharpening                     res4k   sharpness
//     -> fit      backbuffer     letterbox placement (+ dither)            res4k
//
// The order is Xenia's presenter order (reference/xenia-canary/src/xenia/ui/presenter.cc and
// gpu/d3d12/d3d12_command_processor.cc IssueSwap: FXAA on the gamma-applied guest output, then
// the CAS / FSR scaling and sharpening effects, then dither).
//
// A stage either records one or more passes and hands the next stage its result, or returns
// false and records nothing. With every stage at its default nothing is recorded and fit reads
// the scan-out exactly as before the chain existed: the presented pixels are byte-identical.
//
// Each stage reads a PresentSurface (an SRV in the xlat heap's RT SRV band, in
// PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE state) and writes a chain target
// (m_chainTgt[i], created on demand, left in the same read state). Helpers for a stage:
//   pm4ChainTargetEnsure  a target of a size / format (RTV, SRV, optional UAV)
//   pm4ChainPso           a full-screen pixel-shader PSO on the chain root signature
//   pm4ChainPass          one full-screen pass: SRV in -> target out, <= 16 root constants
// The chain root signature: b0 = 16 root constants, t0 = one SRV (table), s0 = linear clamp,
// s1 = point clamp (static samplers). A stage with its own root signature (e.g. a compute
// shader with a UAV) records its own commands and leaves the read state on its target.
//
// Charter: host rendering only - no guest memory, no guest code, no game decision.
#include "d3d12_backend.hpp"
#include "host_hlsl.hpp"  // step 4 7a: SR_HOST_HLSL_AOT (host HLSL compiled ahead of time)
#include "gfx_options.hpp"
#include "../kernel/pc_settings.hpp"
#include "../app/log.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace sr {
namespace gfx {

void pm4ShmBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                   D3D12_RESOURCE_STATES b);  // d3d12_pm4_edram.cpp
void presentFitRect(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, uint32_t& x,
                    uint32_t& y, uint32_t& w, uint32_t& h);  // d3d12_pm4_shmem.cpp

namespace {

constexpr D3D12_RESOURCE_STATES kChainRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Full-screen triangle VS shared by every chain pass (the scan-out's VSFull).
const char kChainVsHlsl[] = R"HLSL(
float4 VSFull(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2(float((id << 1) & 2u), float(id & 2u));
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)HLSL";

Microsoft::WRL::ComPtr<ID3DBlob> chainCompile(const char* src, size_t len, const char* name,
                                              const char* entry, const char* target) {
  Microsoft::WRL::ComPtr<ID3DBlob> code, err;
  const HRESULT hr = hostHlslCompile(src, len, name, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PRESENT_CHAIN] %s %s (%s) compile failed: %s", name, entry,
                target, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return nullptr;
  }
  return code;
}

}  // namespace

const char* D3D12Backend::presentStageName(PresentStage s) {
  switch (s) {
    case PresentStage::Post: return "post";
    case PresentStage::Colour: return "colour";
    case PresentStage::Scale: return "scale";
    case PresentStage::Sharpen: return "sharpen";
    case PresentStage::Fit: return "fit";
    default: return "?";
  }
}

bool D3D12Backend::pm4ChainInit() {
  if (m_chainRs) return true;
  if (m_chainFailed) return false;
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER p[2]{};
  p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[0].Constants.Num32BitValues = kChainConsts;
  p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[1].DescriptorTable.NumDescriptorRanges = 1;
  p[1].DescriptorTable.pDescriptorRanges = &range;
  p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC ss[2]{};
  for (int i = 0; i < 2; i++) {
    ss[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss[i].MaxLOD = D3D12_FLOAT32_MAX;
    ss[i].ShaderRegister = UINT(i);
    ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  D3D12_ROOT_SIGNATURE_DESC rd{};
  rd.NumParameters = 2;
  rd.pParameters = p;
  rd.NumStaticSamplers = 2;
  rd.pStaticSamplers = ss;
  Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
  if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
      FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&m_chainRs)))) {
    m_chainFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PRESENT_CHAIN] root signature failed - stages off");
    return false;
  }
  m_chainVs = chainCompile(kChainVsHlsl, sizeof(kChainVsHlsl) - 1, "present_chain_vs.hlsl", "VSFull",
                           "vs_5_1");
  if (!m_chainVs) {
    m_chainFailed = true;
    m_chainRs.Reset();
    return false;
  }
  return true;
}

ID3D12PipelineState* D3D12Backend::pm4ChainPso(const char* key, const char* hlsl, size_t len,
                                               const char* entry, DXGI_FORMAT rtvFormat) {
  if (!pm4ChainInit()) return nullptr;
  const std::string k = std::string(key) + "/" + std::to_string(unsigned(rtvFormat));
  auto it = m_chainPsos.find(k);
  if (it != m_chainPsos.end()) return it->second.Get();
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  auto ps = chainCompile(hlsl, len, key, entry, "ps_5_1");
  if (ps) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
    gd.pRootSignature = m_chainRs.Get();
    gd.VS = {m_chainVs->GetBufferPointer(), m_chainVs->GetBufferSize()};
    gd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = UINT_MAX;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = rtvFormat;
    gd.SampleDesc.Count = 1;
    if (FAILED(m_device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&pso)))) pso.Reset();
  }
  if (!pso)
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PRESENT_CHAIN] pso %s failed - that stage stays off", key);
  m_chainPsos[k] = pso;  // a failure is remembered (null): no retry every frame
  return pso.Get();
}

bool D3D12Backend::pm4ChainTargetEnsure(int idx, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav) {
  if (idx < 0 || idx >= kChainTargets || !w || !h) return false;
  ChainTarget& t = m_chainTgt[idx];
  if (t.tex && t.w == w && t.h == h && t.fmt == fmt && (t.uav >= 0 || !uav)) return true;
  if (!m_pm4GpuReady || m_pm4RtSrvUsed + 2u > pm4RtSrvCapacity()) return false;
  if (!t.rtvHeap) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.rtvHeap)))) return false;
  }
  if (t.tex) retireResource(t.tex);  // a frame in flight may still read it
  t.tex.Reset();
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = fmt;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
             (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
  D3D12_CLEAR_VALUE cv{};
  cv.Format = fmt;
  t.state = kChainRead;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t.state, &cv,
                                               IID_PPV_ARGS(&t.tex))))
    return false;
  wchar_t name[32];
  swprintf(name, 32, L"PRESENT_CHAIN_%d", idx);
  t.tex->SetName(name);
  m_device->CreateRenderTargetView(t.tex.Get(), nullptr, t.rtvHeap->GetCPUDescriptorHandleForHeapStart());
  // New descriptor slots per (rare) size change: a frame in flight may still read the old ones.
  t.srv = int32_t(m_pm4RtSrvUsed++);
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = fmt;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  publishXlatSrv(t.tex.Get(), &sd, pm4RtSrvCpu(uint32_t(t.srv)));
  t.uav = -1;
  if (uav) {
    t.uav = int32_t(m_pm4RtSrvUsed++);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = fmt;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(t.tex.Get(), nullptr, &ud, pm4RtSrvCpu(uint32_t(t.uav)));
  }
  t.w = w;
  t.h = h;
  t.fmt = fmt;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PRESENT_CHAIN] target %d %ux%u fmt %u%s (srv %d)", idx, w, h,
              unsigned(fmt), uav ? " +uav" : "", t.srv);
  return true;
}

void D3D12Backend::pm4ChainToRead(ChainTarget& t) {
  if (t.state != kChainRead) {
    pm4ShmBarrier(m_commandList.Get(), t.tex.Get(), t.state, kChainRead);
    t.state = kChainRead;
  }
}

bool D3D12Backend::pm4ChainPass(ID3D12PipelineState* pso, const PresentSurface& in, int idx,
                                uint32_t w, uint32_t h, DXGI_FORMAT fmt, const void* consts,
                                uint32_t nConsts, PresentSurface& out) {
  if (!pso || !in.res || in.srv < 0 || nConsts > kChainConsts) return false;
  if (!pm4ChainTargetEnsure(idx, w, h, fmt, false)) return false;
  ChainTarget& t = m_chainTgt[idx];
  if (t.state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    pm4ShmBarrier(m_commandList.Get(), t.tex.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtvHeap->GetCPUDescriptorHandleForHeapStart();
  m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
  const D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
  m_commandList->RSSetViewports(1, &vp);
  m_commandList->RSSetScissorRects(1, &sc);
  ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
  m_commandList->SetDescriptorHeaps(2, heaps);
  m_commandList->SetGraphicsRootSignature(m_chainRs.Get());
  m_commandList->SetPipelineState(pso);
  if (nConsts) m_commandList->SetGraphicsRoot32BitConstants(0, nConsts, consts, 0);
  m_commandList->SetGraphicsRootDescriptorTable(1, pm4RtSrvGpu(uint32_t(in.srv)));
  m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_commandList->DrawInstanced(3, 1, 0, 0);
  pm4ChainToRead(t);
  out = PresentSurface{t.tex.Get(), t.srv, w, h, idx};
  return true;
}

// ── the stages ─────────────────────────────────────────────────────────────────────────────
// Each returns false (and records nothing) at its default.

// post (gfxset lane): post-process anti-aliasing at the internal resolution - Xenia's FXAA, ported
// as Xenia runs it (gpu/d3d12/d3d12_command_processor.cc IssueSwap, postprocess_antialiasing):
//   1. the image with its perceptual luma in alpha (Xenia's apply_gamma_*_fxaa_luma: luma =
//      dot(rgb, (0.299, 0.587, 0.114)), into an R16G16B16A16_UNORM texture, kFxaaSourceTextureFormat);
//   2. Xenia's precompiled FXAA 3.11 compute shader (fxaa_cs: quality preset 12, subpix 0.75, edge
//      0.166 / 0.0833; fxaa_extreme_cs: preset 39, subpix 1.0, edge 0.063 / 0.0312) into an RGBA8
//      UAV, with Xenia's root signature (b0 = size + 1/size, u0 dest, t0 source, s0 linear clamp,
//      MaxLOD 0). NVIDIA FXAA 3.11 by Timothy Lottes (BSD-3, third_party/xenia-shaders/third_party/
//      fxaa/LICENSE); the shaders and bytecode are Xenia's (BSD-3). Credited in CREDITS.md.
// Default (post_aa = off): not run.
namespace {
namespace xefxaa {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/fxaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/fxaa_extreme_cs.h"
}  // namespace xefxaa
const char kFxaaLumaHlsl[] = R"HLSL(
Texture2D<float4> gSrc : register(t0);
float4 PSLuma(float4 pos : SV_Position) : SV_Target {
  float3 c = gSrc.Load(int3(int2(pos.xy), 0)).rgb;
  return float4(c, dot(c, float3(0.299, 0.587, 0.114)));
}
)HLSL";
}  // namespace

bool D3D12Backend::pm4StagePost(const gfxopt::Settings& s, const PresentSurface& in,
                                PresentSurface& out) {
  if (s.postAa == gfxopt::PostAa::Off || m_chainFailed) return false;
  if (!m_fxaaRs) {
    if (m_fxaaFailed) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 4;   // Xenia FxaaConstants {uint2 size; float2 size_inv;}
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxAnisotropy = 1;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    ss.MaxLOD = 0.0f;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = p;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &ss;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) &&
              SUCCEEDED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                      IID_PPV_ARGS(&m_fxaaRs)));
    if (ok) {
      cd.pRootSignature = m_fxaaRs.Get();
      cd.CS = {xefxaa::fxaa_cs, sizeof(xefxaa::fxaa_cs)};
      ok = SUCCEEDED(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_fxaaPso)));
      cd.CS = {xefxaa::fxaa_extreme_cs, sizeof(xefxaa::fxaa_extreme_cs)};
      ok = ok && SUCCEEDED(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_fxaaExtremePso)));
    }
    if (!ok) {
      m_fxaaFailed = true;
      m_fxaaRs.Reset();
      SR_LOG_BOOT(sr::LogSeverity::Error, "[PRESENT_CHAIN] FXAA pipelines failed - post AA off");
      return false;
    }
  }
  // 1. luma into alpha (R16G16B16A16_UNORM, Xenia's FXAA source format)
  ID3D12PipelineState* luma = pm4ChainPso("fxaa_luma", kFxaaLumaHlsl, sizeof(kFxaaLumaHlsl) - 1, "PSLuma",
                                          DXGI_FORMAT_R16G16B16A16_UNORM);
  PresentSurface src;
  if (!pm4ChainPass(luma, in, 0, in.w, in.h, DXGI_FORMAT_R16G16B16A16_UNORM, nullptr, 0, src)) return false;
  // 2. FXAA into an RGBA8 UAV target
  if (!pm4ChainTargetEnsure(1, in.w, in.h, DXGI_FORMAT_R8G8B8A8_UNORM, true)) return false;
  ChainTarget& t = m_chainTgt[1];
  if (t.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    pm4ShmBarrier(m_commandList.Get(), t.tex.Get(), t.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    t.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
  m_commandList->SetDescriptorHeaps(2, heaps);
  m_commandList->SetComputeRootSignature(m_fxaaRs.Get());
  m_commandList->SetPipelineState(s.postAa == gfxopt::PostAa::FxaaExtreme ? m_fxaaExtremePso.Get()
                                                                          : m_fxaaPso.Get());
  struct { uint32_t w, h; float iw, ih; } c{in.w, in.h, 1.0f / float(in.w), 1.0f / float(in.h)};
  m_commandList->SetComputeRoot32BitConstants(0, 4, &c, 0);
  m_commandList->SetComputeRootDescriptorTable(1, pm4RtSrvGpu(uint32_t(t.uav)));
  m_commandList->SetComputeRootDescriptorTable(2, pm4RtSrvGpu(uint32_t(src.srv)));
  m_commandList->Dispatch((in.w + 15u) / 16u, (in.h + 7u) / 8u, 1);
  pm4ChainToRead(t);
  out = PresentSurface{t.tex.Get(), t.srv, in.w, in.h, 1};
  return true;
}

// colour (gfxset lane): brightness / gamma on the presented image, after the game's own display
// gamma ramp (the console's DC_LUT, applied by the scan-out) - so the game's ramp is untouched and
// the adjustment is the player's display preference on top of it, as a PC game's brightness slider.
//   gamma g (0.50..1.50): out = in ^ (1 / g)   (g > 1 lifts the mid-tones; black and white stay)
//   brightness b (-50..50, k = b / 100): k > 0 blends toward white (out = c + (1 - c) k),
//   k < 0 scales toward black (out = c (1 + k)) - the classic image-editor brightness.
// Per channel, in the 8-bit display space the scan-out produced. Default (0, 1.00): not run.
namespace {
const char kColourHlsl[] = R"HLSL(
cbuffer C : register(b0) { float gBright; float gInvGamma; float gPad0; float gPad1; };
Texture2D<float4> gSrc : register(t0);
float4 PSColour(float4 pos : SV_Position) : SV_Target {
  float3 c = gSrc.Load(int3(int2(pos.xy), 0)).rgb;
  c = pow(saturate(c), gInvGamma);
  c = gBright >= 0.0 ? c + (1.0 - c) * gBright : c * (1.0 + gBright);
  return float4(saturate(c), 1.0);
}
)HLSL";
}  // namespace

bool D3D12Backend::pm4StageColour(const gfxopt::Settings& s, const PresentSurface& in,
                                  PresentSurface& out) {
  if (s.brightness == 0 && s.gamma == 1.0f) return false;
  ID3D12PipelineState* pso = pm4ChainPso("colour", kColourHlsl, sizeof(kColourHlsl) - 1, "PSColour",
                                         DXGI_FORMAT_R8G8B8A8_UNORM);
  const float c[4] = {float(s.brightness) / 100.0f, 1.0f / s.gamma, 0.0f, 0.0f};
  return pm4ChainPass(pso, in, 2, in.w, in.h, DXGI_FORMAT_R8G8B8A8_UNORM, c, 4, out);
}

// scale + sharpen (res4k lane, iter-466): Xenia's guest-output effects, ported as Xenia's presenter
// runs them (reference/xenia-canary/src/xenia/ui/presenter.cc BuildGuestOutputPaintFlow, presenter.h
// Cas*/Fsr* constants): AMD FidelityFX CAS (resample / sharpen) and FSR 1 (EASU upsampling, RCAS
// sharpening), with Xenia's D3D12 pixel shaders (src/xenia/ui/shaders, built by cmake/xenia_shaders.cmake,
// BSD-3; AMD FidelityFX CAS and FSR, MIT - third_party/xenia-shaders/third_party/FidelityFX-{CAS,FSR};
// credited in CREDITS.md). Each PS reads its own b0 from offset 0 (Xenia's D3D12 root signature gives
// the pixel stage its own constants) - here the chain root signature's b0 - plus t0 and s0.
//
//   upscale_filter  enlarge (fit > internal)            1:1                     shrink (fit < internal)
//   bilinear        fit's bilinear (sharpness 0), or     CAS sharpen if          fit's area filter
//                   Xenia bilinear + CAS sharpen (> 0)   sharpness > 0           (no sharpening)
//   cas             CAS resample (<= 2x per axis, then   CAS sharpen             CAS resample (downsample)
//                   bilinear for the rest)
//   fsr             EASU x2 passes (<= 4) + RCAS         as cas                  as cas
//
// sharpness 0..100: CAS additional sharpness 0..1 (Xenia kCasAdditionalSharpness), FSR RCAS
// sharpness reduction 2..0 stops (Xenia kFsrSharpnessReductionMax .. 0; Xenia's default 0.2 stops =
// sharpness 90). Defaults (bilinear, 0) record nothing: today's fit, byte-identical.
namespace {
namespace xeout {
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_easu_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_ps.h"
}  // namespace xeout
constexpr DXGI_FORMAT kScaleFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
// What the scale stage did this frame, for the sharpen stage (one present thread).
enum class ScaleRan { None, Bilinear, Cas, Easu };
ScaleRan s_scaleRan = ScaleRan::None;
bool s_sharpenFits = false;  // the image the sharpen stage gets is the output size (not a shrink)
float casPostSetup(int sharpness) {  // presenter.h CalculateCasPostSetupSharpness
  const float a = float(std::min(100, std::max(0, sharpness))) / 100.0f;
  return -1.0f / (8.0f - 3.0f * a);
}
float rcasPostSetup(int sharpness) {  // presenter.h FsrRcasConstants (exp2(-stops))
  const float stops = 2.0f * (1.0f - float(std::min(100, std::max(0, sharpness))) / 100.0f);
  return std::exp2(-stops);
}
}  // namespace

ID3D12PipelineState* D3D12Backend::pm4ChainPsoBytecode(const char* key, const void* ps, size_t n) {
  if (!pm4ChainInit()) return nullptr;
  const std::string k = std::string("xe/") + key;
  auto it = m_chainPsos.find(k);
  if (it != m_chainPsos.end()) return it->second.Get();
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
  gd.pRootSignature = m_chainRs.Get();
  gd.VS = {m_chainVs->GetBufferPointer(), m_chainVs->GetBufferSize()};
  gd.PS = {ps, n};
  gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  gd.SampleMask = UINT_MAX;
  gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  gd.RasterizerState.DepthClipEnable = TRUE;
  gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  gd.NumRenderTargets = 1;
  gd.RTVFormats[0] = kScaleFmt;
  gd.SampleDesc.Count = 1;
  if (FAILED(m_device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&pso)))) {
    pso.Reset();
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PRESENT_CHAIN] pso %s failed - that effect stays off", key);
  }
  m_chainPsos[k] = pso;
  return pso.Get();
}

bool D3D12Backend::pm4StageScale(const gfxopt::Settings& s, const PresentSurface& in, uint32_t fitW,
                                 uint32_t fitH, PresentSurface& out) {
  s_scaleRan = ScaleRan::None;
  s_sharpenFits = false;
  if (m_chainFailed || !fitW || !fitH || !in.w || !in.h) return false;
  const bool enlarge = fitW > in.w || fitH > in.h;
  const bool same = fitW == in.w && fitH == in.h;
  const bool shrink = !enlarge && !same;
  s_sharpenFits = same;
  int tgt = in.tgt == 3 ? 4 : 3;  // ping-pong 3 / 4 (never the input's own target)
  PresentSurface cur = in;
  auto pass = [&](const char* key, const void* ps, size_t n, uint32_t w, uint32_t h, const void* c,
                  uint32_t nc) {
    PresentSurface next;
    if (!pm4ChainPass(pm4ChainPsoBytecode(key, ps, n), cur, tgt, w, h, kScaleFmt, c, nc, next))
      return false;
    cur = next;
    tgt = tgt == 3 ? 4 : 3;
    return true;
  };
  // Bilinear stretch of the rest (presenter.cc: after FFX reached its limit), Xenia's bilinear PS.
  auto bilinearTo = [&](uint32_t w, uint32_t h) {
    struct { int32_t ox, oy; float iw, ih; } c{0, 0, 1.0f / float(w), 1.0f / float(h)};
    return pass("bilinear", xeout::guest_output_bilinear_ps, sizeof(xeout::guest_output_bilinear_ps), w,
                h, &c, 4);
  };
  auto casTo = [&](uint32_t w, uint32_t h) {  // CAS resample (or sharpen at 1:1)
    if (w == cur.w && h == cur.h) {
      struct { int32_t ox, oy; float sharp; } c{0, 0, casPostSetup(s.sharpness)};
      return pass("cas_sharpen", xeout::guest_output_ffx_cas_sharpen_ps,
                  sizeof(xeout::guest_output_ffx_cas_sharpen_ps), w, h, &c, 3);
    }
    struct { int32_t ox, oy; float rx, ry, sharp; } c{0, 0, float(cur.w) / float(w),
                                                      float(cur.h) / float(h), casPostSetup(s.sharpness)};
    return pass("cas_resample", xeout::guest_output_ffx_cas_resample_ps,
                sizeof(xeout::guest_output_ffx_cas_resample_ps), w, h, &c, 5);
  };
  switch (s.upscale) {
    case gfxopt::Upscale::Bilinear:
      // sharpness 0: fit stretches / shrinks as before. > 0: stretch here so sharpen sees the output size.
      if (s.sharpness <= 0 || !enlarge) return false;
      if (!bilinearTo(fitW, fitH)) return false;
      s_scaleRan = ScaleRan::Bilinear;
      s_sharpenFits = true;
      break;
    case gfxopt::Upscale::Fsr:
      if (enlarge) {
        // EASU doubles per pass up to the fit size (presenter.cc; Xenia's max passes go up to
        // kFsrMaxUpsamplingPassesMax 4 - 720p internal -> 4K output takes 2).
        for (int i = 0; i < 4 && (cur.w < fitW || cur.h < fitH); i++) {
          const uint32_t w = std::min(cur.w * 2u, fitW), h = std::min(cur.h * 2u, fitH);
          struct { float rx, ry, iwInv, ihInv; } c{float(cur.w) / float(w), float(cur.h) / float(h),
                                                     1.0f / float(cur.w), 1.0f / float(cur.h)};
          if (!pass("fsr_easu", xeout::guest_output_ffx_fsr_easu_ps,
                    sizeof(xeout::guest_output_ffx_fsr_easu_ps), w, h, &c, 4))
            return false;
        }
        if ((cur.w != fitW || cur.h != fitH) && !bilinearTo(fitW, fitH)) return false;
        s_scaleRan = ScaleRan::Easu;
        s_sharpenFits = true;
        break;
      }
      [[fallthrough]];  // 1:1 or shrink: CAS (presenter.cc)
    case gfxopt::Upscale::Cas: {
      if (shrink || same) {
        if (!casTo(fitW, fitH)) return false;
      } else {
        // CAS upsamples up to 2x per axis, then bilinear (presenter.cc).
        const uint32_t w = std::min(cur.w * 2u, fitW), h = std::min(cur.h * 2u, fitH);
        if (!casTo(w, h)) return false;
        if ((w != fitW || h != fitH) && !bilinearTo(fitW, fitH)) return false;
      }
      s_scaleRan = ScaleRan::Cas;
      break;
    }
    default:
      return false;
  }
  out = cur;
  return true;
}

bool D3D12Backend::pm4StageSharpen(const gfxopt::Settings& s, const PresentSurface& in,
                                   PresentSurface& out) {
  if (m_chainFailed) return false;
  if (s_scaleRan == ScaleRan::Easu) {  // FSR: RCAS (1:1) after EASU
    struct { int32_t ox, oy; float sharp; } c{0, 0, rcasPostSetup(s.sharpness)};
    return pm4ChainPass(pm4ChainPsoBytecode("fsr_rcas", xeout::guest_output_ffx_fsr_rcas_ps,
                                            sizeof(xeout::guest_output_ffx_fsr_rcas_ps)),
                        in, 5, in.w, in.h, kScaleFmt, &c, 3, out);
  }
  // cas sharpens inside its resample; bilinear + sharpness: CAS sharpen 1:1 on the output-size
  // image (enlarged by scale, or already 1:1). A shrink keeps fit's area filter, unsharpened.
  if (s.upscale != gfxopt::Upscale::Bilinear || s.sharpness <= 0 || !s_sharpenFits) return false;
  struct { int32_t ox, oy; float sharp; } c{0, 0, casPostSetup(s.sharpness)};
  return pm4ChainPass(pm4ChainPsoBytecode("cas_sharpen", xeout::guest_output_ffx_cas_sharpen_ps,
                                          sizeof(xeout::guest_output_ffx_cas_sharpen_ps)),
                      in, 5, in.w, in.h, kScaleFmt, &c, 3, out);
}

// ── the chain ─────────────────────────────────────────────────────────────────────────────

// The internal-resolution stages (post, colour) on `src`; the result is also what the replay
// frame dump reads (pm4ReadOutput).
D3D12Backend::PresentSurface D3D12Backend::pm4ChainInternal(const gfxopt::Settings& s,
                                                            PresentSurface cur, uint32_t& ran) {
  PresentSurface next;
  if (pm4StagePost(s, cur, next)) {
    cur = next;
    ran |= 1u << unsigned(PresentStage::Post);
  }
  if (pm4StageColour(s, cur, next)) {
    cur = next;
    ran |= 1u << unsigned(PresentStage::Colour);
  }
  return cur;
}

// SR_PRESENT_CHAIN_TIME=1 (diagnostic, default off): GPU timestamps around the chain's stages,
// averaged over 120 presents - the cost in ms of each option the chain carries.
namespace {
bool chainTimeEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PRESENT_CHAIN_TIME");
    return e && e[0] == '1';
  }();
  return v;
}
}  // namespace

void D3D12Backend::pm4ChainStamp(uint32_t i) {
  if (!m_ctHeap) return;
  m_commandList->EndQuery(m_ctHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, uint32_t(m_frameIndex) * kCtStamps + i);
}

void D3D12Backend::pm4ChainTimeFrame() {
  if (!chainTimeEnabled()) return;
  if (!m_ctHeap) {
    if (m_ctFailed) return;
    m_ctFailed = true;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kCtStamps * kFrameCount;
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
    if (FAILED(m_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_ctHeap))) ||
        FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&m_ctReadback))) ||
        FAILED(m_commandQueue->GetTimestampFrequency(&m_ctFreq)) || !m_ctFreq) {
      m_ctHeap.Reset();
      return;
    }
    m_ctFailed = false;
    return;   // stamps start with the next present
  }
  // this slot's previous stamps (its fence was waited before this frame was recorded)
  const uint32_t slot = uint32_t(m_frameIndex);
  if (m_ctValid[slot]) {
    const size_t off = size_t(slot) * kCtStamps * 8u;
    D3D12_RANGE rr{off, off + kCtStamps * 8u};
    void* p = nullptr;
    if (SUCCEEDED(m_ctReadback->Map(0, &rr, &p)) && p) {
      const uint64_t* t = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(p) + off);
      for (uint32_t i = 0; i + 1 < kCtStamps; ++i)
        m_ctAcc[i] += t[i + 1] > t[i] ? double(t[i + 1] - t[i]) * 1000.0 / double(m_ctFreq) : 0.0;
      const D3D12_RANGE nw{0, 0};
      m_ctReadback->Unmap(0, &nw);
      if (++m_ctN == 120) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PRESENT_CHAIN_TIME] avg over 120 presents (ms): post %.3f colour %.3f scale %.3f "
                    "sharpen %.3f fit %.3f (internal %ux%u)",
                    m_ctAcc[0] / 120, m_ctAcc[1] / 120, m_ctAcc[2] / 120, m_ctAcc[3] / 120, m_ctAcc[4] / 120,
                    m_outW, m_outH);
        for (double& a : m_ctAcc) a = 0;
        m_ctN = 0;
      }
    }
  }
}

void D3D12Backend::pm4PresentChain(ID3D12Resource* backbuffer) {
  // iter-466 res4k: the scale / sharpen stages exist on this present path - list the PC SETTINGS
  // rows (kernel/pc_settings.hpp: hidden until the owning lane registers them). The values are
  // gfxopt keys, so a change applies live through gfxopt::set.
  static const bool s_registered = [] {
    sr::kernel::pcsettings::setAvailable("upscale_filter", true);
    sr::kernel::pcsettings::setAvailable("sharpness", true);
    return true;
  }();
  (void)s_registered;
  const gfxopt::Settings s = gfxopt::current();
  uint32_t ran = 0;
  pm4ChainTimeFrame();
  const bool stamp = m_ctHeap != nullptr;
  if (stamp) pm4ChainStamp(0);
  PresentSurface cur{m_outTex.Get(), m_outSrv, m_outW, m_outH};
  PresentSurface next;
  // the internal-resolution stages (pm4ChainInternal's order, stamped one by one)
  if (pm4StagePost(s, cur, next)) {
    cur = next;
    ran |= 1u << unsigned(PresentStage::Post);
  }
  if (stamp) pm4ChainStamp(1);
  if (pm4StageColour(s, cur, next)) {
    cur = next;
    ran |= 1u << unsigned(PresentStage::Colour);
  }
  if (stamp) pm4ChainStamp(2);
  m_chainInternalTgt = cur.tgt;
  const D3D12_RESOURCE_DESC bd = backbuffer->GetDesc();
  uint32_t fx = 0, fy = 0, fw = 0, fh = 0;
  presentFitRect(cur.w, cur.h, uint32_t(bd.Width), bd.Height, fx, fy, fw, fh);
  if (pm4StageScale(s, cur, fw, fh, next)) {
    cur = next;
    ran |= 1u << unsigned(PresentStage::Scale);
  }
  if (stamp) pm4ChainStamp(3);
  if (pm4StageSharpen(s, cur, next)) {
    cur = next;
    ran |= 1u << unsigned(PresentStage::Sharpen);
  }
  if (stamp) pm4ChainStamp(4);
  pm4FitToBackbuffer(backbuffer, cur);
  ran |= 1u << unsigned(PresentStage::Fit);
  if (stamp) {
    pm4ChainStamp(5);
    const uint32_t slot = uint32_t(m_frameIndex);
    m_commandList->ResolveQueryData(m_ctHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kCtStamps, kCtStamps,
                                    m_ctReadback.Get(), uint64_t(slot) * kCtStamps * 8u);
    m_ctValid[slot] = true;
  }
  if (ran != m_chainLastRan) {
    m_chainLastRan = ran;
    std::string line;
    for (unsigned i = 0; i < unsigned(PresentStage::Count); i++)
      line += std::string(" ") + presentStageName(PresentStage(i)) + "=" + ((ran >> i) & 1u ? "on" : "-");
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PRESENT_CHAIN]%s (internal %ux%u -> backbuffer %llux%u)",
                line.c_str(), m_outW, m_outH, (unsigned long long)bd.Width, bd.Height);
  }
}

}  // namespace gfx
}  // namespace sr
