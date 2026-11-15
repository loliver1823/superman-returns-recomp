// iter-466 res4k round 2 (SR_TEX_LOAD_DIRECT): a resolution-scaled texture load that writes the
// texture itself. Xenia's scaled loaders (texture_load_{16,32,64}bpb / depth_{unorm,float}
// _scaled, BSD-3, Ben Vanik / Triang3l) write a linear scratch buffer that a CopyTextureRegion
// then puts into the texture; here the SAME source addressing (texture_load.xesli
// XeTextureLoadSourceAddress / XeTextureLoadLocalXAddressXor, endian swap, depth transforms)
// stores each texel through a UINT view of the texture's typeless family - the same bits the copy
// would have written, without the scratch buffer, its barrier and the copy.
// Included by gfx/d3d12_pm4_shmem.cpp after native_resolve.inc (NatXeslInclude).

namespace {

const char kTexLoadDirectHlsl[] = R"HLSL(
#define SHADING_LANGUAGE_HLSL_XE 1
#define XE_TEXTURE_LOAD_RESOLUTION_SCALED
#include "endian.xesli"
#include "pixel_formats.xesli"
#include "texture_load.xesli"

ByteAddressBuffer gTlSrc : register(t0, space0);
RWTexture2DArray<uint4> gTlDst : register(u0, space0);  // slice 0 of the bound view
// gTlLog2Bpb: 1 / 2 / 3 (16 / 32 / 64 bits per block); gTlUnpack: how one block's bits split into
// the UINT view's channels; gTlDepth: 0 none, 1 k_24_8 (unorm24 -> float32), 2 k_24_8_FLOAT.
cbuffer TlDirect : register(b1, space0) { uint gTlLog2Bpb; uint gTlUnpack; uint gTlDepth; uint gTlPad; };

uint4 TlUnpack32(uint w) {
  if (gTlUnpack == 1u) return uint4(w & 0xFFu, (w >> 8) & 0xFFu, (w >> 16) & 0xFFu, w >> 24);  // RGBA8
  if (gTlUnpack == 2u) return uint4(w & 0xFFFFu, w >> 16, 0u, 0u);                             // RG16
  return uint4(w, 0u, 0u, 0u);                                                                 // R32
}

[numthreads(4, 32, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  XeTextureLoadInfo li = XeTextureLoadGetInfo(pass_push_consts_xe);
  uint b = gTlLog2Bpb;
  uint bpc = 16u >> b;  // blocks per 16-byte chunk; a thread moves two chunks (Xenia's loaders)
  uint3 bi = tid << uint3(5u - b, 0u, 0u);
  if (any(bi.xy >= li.size_blocks.xy)) return;
  uint go = XeTextureLoadSourceAddress(li, bi, b);
  [unroll] for (uint c = 0u; c < 2u; c++) {
    uint4 w = gTlSrc.Load4(go);
    w = b == 1u ? XeEndianSwap16(w, li.endian_32) : XeEndianSwap32(w, li.endian_32);
    if (gTlDepth == 1u) w = float_bits_to_uint_xe(XeUNorm24To32(w >> 8u));
    else if (gTlDepth == 2u) w = XeFloat20e4To32(w >> 8u);
    uint x0 = bi.x + c * bpc;
    if (b == 1u) {
      [unroll] for (uint j = 0u; j < 8u; j++)
        gTlDst[uint3(x0 + j, bi.y, 0u)] = uint4((w[j >> 1] >> ((j & 1u) * 16u)) & 0xFFFFu, 0u, 0u, 0u);
    } else if (b == 2u) {
      [unroll] for (uint j = 0u; j < 4u; j++) gTlDst[uint3(x0 + j, bi.y, 0u)] = TlUnpack32(w[j]);
    } else {
      [unroll] for (uint j = 0u; j < 2u; j++) {
        uint lo = w[j * 2u], hi = w[j * 2u + 1u];
        gTlDst[uint3(x0 + j, bi.y, 0u)] = uint4(lo & 0xFFFFu, lo >> 16, hi & 0xFFFFu, hi >> 16);  // RGBA16
      }
    }
    if (c == 0u) go += XeTextureLoadLocalXAddressXor(bpc, b, li.is_tiled);
  }
}
)HLSL";

struct TexDirectState {
  Microsoft::WRL::ComPtr<ID3D12RootSignature> rs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  bool tried = false, ok = false;
  int32_t uavBase = -1;  // a ring of UAV descriptors in the RT SRV band
  uint32_t uavNext = 0;
  uint64_t loads = 0;
};
TexDirectState g_tld;
constexpr uint32_t kTldUavRing = 256;  // > loads per frame (~40) x frames in flight

bool texLoadDirectEnabled() {
#if SR_ORACLE
  static const bool v = [] {
    // baked default ON (replay md5-identical 1x 4/4, 3x 7/7; live 3x A/B 2026-10-06); =0: scratch
    const char* e = std::getenv("SR_TEX_LOAD_DIRECT");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;  // Shipping policy; unsupported formats still use the native scratch loader.
#endif
}

}  // namespace

// The typeless family, the UINT view, and how the loader's bits map onto it, for the host formats
// of the scaled-loadable guest formats (pm4LoaderHostFormat). false: the scratch path.
bool pm4TexDirectFormat(DXGI_FORMAT dxgi, DXGI_FORMAT* typeless, DXGI_FORMAT* uav, uint32_t* log2Bpb,
                        uint32_t* unpack, bool any16) {
  DXGI_FORMAT t = DXGI_FORMAT_UNKNOWN, u = DXGI_FORMAT_UNKNOWN;
  uint32_t b = 0, m = 0;
  switch (dxgi) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      t = DXGI_FORMAT_R8G8B8A8_TYPELESS; u = DXGI_FORMAT_R8G8B8A8_UINT; b = 2; m = 1; break;
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT:
      t = DXGI_FORMAT_R16G16_TYPELESS; u = DXGI_FORMAT_R16G16_UINT; b = 2; m = 2; break;
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
      t = DXGI_FORMAT_R32_TYPELESS; u = DXGI_FORMAT_R32_UINT; b = 2; m = 0; break;
    case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: {
      // 16bpp (the shadow map) stays on the scratch path unless SR_TEX_LOAD_DIRECT_16=1: per-texel
      // 16-bit typed stores measured slower live than the 16-byte buffer stores + copy (2026-10-07).
      static const bool s16 = [] { const char* e = std::getenv("SR_TEX_LOAD_DIRECT_16"); return e && e[0] == '1'; }();
      if (!s16 && !any16) return false;  // iter-467 native-b: any16 = a native target's UAV view
      t = DXGI_FORMAT_R16_TYPELESS; u = DXGI_FORMAT_R16_UINT; b = 1; break;
    }
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
      t = DXGI_FORMAT_R16G16B16A16_TYPELESS; u = DXGI_FORMAT_R16G16B16A16_UINT; b = 3; break;
    default: return false;
  }
  if (typeless) *typeless = t;
  if (uav) *uav = u;
  if (log2Bpb) *log2Bpb = b;
  if (unpack) *unpack = m;
  return true;
}
bool pm4TexLoadDirectOn() { return texLoadDirectEnabled() && !(g_tld.tried && !g_tld.ok); }
uint32_t pm4TldUavSlot() { return uint32_t(g_tld.uavBase) + uint32_t(g_tld.uavNext++ % kTldUavRing); }
ID3D12RootSignature* pm4TldRs() { return g_tld.rs.Get(); }
ID3D12PipelineState* pm4TldPso() { return g_tld.pso.Get(); }
uint64_t pm4TldNoteLoad() { return ++g_tld.loads; }

bool D3D12Backend::pm4TexLoadDirectInit() {
  if (g_tld.tried) return g_tld.ok;
  g_tld.tried = true;
  auto fail = [&](const char* what, const char* detail) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[TEX_LOAD_DIRECT] %s failed%s%s - the scratch path stays", what,
                detail ? ": " : "", detail ? detail : "");
    return false;
  };
  if (m_pm4RtSrvUsed + kTldUavRing > pm4RtSrvCapacity()) return fail("UAV ring", nullptr);
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER p[4]{};
  p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // texture_load.xesli push consts
  p[0].Constants.Num32BitValues = 10;
  p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t0: the scaled resolve memory
  p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // u0: the texture
  p[2].DescriptorTable.NumDescriptorRanges = 1;
  p[2].DescriptorTable.pDescriptorRanges = &range;
  p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1
  p[3].Constants.ShaderRegister = 1;
  p[3].Constants.Num32BitValues = 4;
  D3D12_ROOT_SIGNATURE_DESC rd{};
  rd.NumParameters = 4;
  rd.pParameters = p;
  Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
  if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    return fail("root signature", err ? static_cast<const char*>(err->GetBufferPointer()) : nullptr);
  if (FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g_tld.rs))))
    return fail("root signature (create)", nullptr);
  std::vector<uint8_t> code;
  std::string cerr;
  if (!nbAotCode("tex_load_direct", kTexLoadDirectHlsl, sizeof(kTexLoadDirectHlsl) - 1,
                 "gpu/shaders/sr_tex_load_direct.cs.hlsl", nullptr, false, code, cerr))
    return fail("compile", cerr.c_str());
  D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
  cd.pRootSignature = g_tld.rs.Get();
  cd.CS = {code.data(), code.size()};
  if (FAILED(m_device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&g_tld.pso)))) return fail("pso", nullptr);
  g_tld.uavBase = int32_t(m_pm4RtSrvUsed);
  m_pm4RtSrvUsed += kTldUavRing;
  g_tld.ok = true;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[TEX_LOAD_DIRECT] on: scaled 16/32/64bpb + depth loads write the texture (UAV ring %u)",
              kTldUavRing);
  return true;
}
