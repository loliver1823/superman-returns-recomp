// Step 4 item 2k(3b): the pipeline manifest (gfx/pso_manifest.cpp) - a round trip through the file image gives
// descriptions that HIT the pipelines the originals made (the same cache key: the warm-up's whole point), equal
// shaders are stored once, malformed images are rejected whole, merge keeps order and drops duplicates. WARP device.
#include "gfx/native_pipeline.hpp"
#include "gfx/pso_manifest.hpp"

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;

namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[PSO_MANIFEST_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "SETUP FAILED %s hr=%08lX\n", name, (unsigned long)hr);
    std::exit(2);
  }
}
NativeShaderCode compile(const char* source, const char* entry, const char* profile) {
  ComPtr<ID3DBlob> blob, errors;
  require(D3DCompile(source, std::strlen(source), "pso_manifest_fixture", nullptr, nullptr, entry, profile,
                     D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors), entry);
  NativeShaderCode code;
  require(NativeShaderCode::copy({(const uint8_t*)blob->GetBufferPointer(), blob->GetBufferSize()}, code), "copy");
  return code;
}
}  // namespace

int main() {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> device;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants = {0, 0, 4};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ComPtr<ID3DBlob> rblob, rerr;
  require(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rblob, &rerr), "root serialize");
  ComPtr<ID3D12RootSignature> root;
  require(device->CreateRootSignature(0, rblob->GetBufferPointer(), rblob->GetBufferSize(), IID_PPV_ARGS(&root)),
          "root");
  static constexpr char src[] = R"(
    cbuffer C : register(b0) { float4 rgba; };
    struct VI { float3 p : POSITION; float2 uv : TEXCOORD0; };
    float4 VS(VI v) : SV_Position { return float4(v.p, 1); }
    float4 PS() : SV_Target0 { return rgba; }
    float4 PS2() : SV_Target0 { return rgba.bgra; }
  )";
  const auto vs = compile(src, "VS", "vs_5_1"), ps = compile(src, "PS", "ps_5_1"), ps2 = compile(src, "PS2", "ps_5_1");
  NativePipelineDesc a;
  a.rootSignature = root; a.vertex = vs; a.pixel = ps;
  a.inputLayout = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0},
                   {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12}};
  a.colorCount = 2; a.colorFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; a.colorFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
  a.depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
  a.rasterizer.FillMode = D3D12_FILL_MODE_SOLID; a.rasterizer.CullMode = D3D12_CULL_MODE_BACK;
  a.rasterizer.DepthBias = -3; a.rasterizer.SlopeScaledDepthBias = 0.5f; a.rasterizer.DepthClipEnable = TRUE;
  a.blend.IndependentBlendEnable = TRUE;
  for (auto& rt : a.blend.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD; rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  a.blend.RenderTarget[1].BlendEnable = TRUE; a.blend.RenderTarget[1].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  a.blend.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  a.depthStencil.DepthEnable = TRUE; a.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  a.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; a.depthStencil.StencilEnable = TRUE;
  a.depthStencil.StencilReadMask = 0x0F; a.depthStencil.StencilWriteMask = 0xF0;
  a.depthStencil.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_REPLACE,
                              D3D12_COMPARISON_FUNC_EQUAL};
  a.depthStencil.BackFace = {D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_DECR,
                             D3D12_COMPARISON_FUNC_ALWAYS};
  a.stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
  NativePipelineDesc b = a;
  b.pixel = ps2;            // an "alpha-test variant": the same pipeline but its pixel bytecode
  NativePipelineDesc c = a;
  c.rasterizer.CullMode = D3D12_CULL_MODE_NONE;   // shares both shaders with a

  NativePipelineCache cache;
  require(cache.initialize(device.Get(), 64), "cache");
  NativePipelineLease la, lb, lc;
  require(cache.acquire(a, la), "a"); require(cache.acquire(b, lb), "b"); require(cache.acquire(c, lc), "c");

  const auto image = psom::serialize({&a, &b, &c});
  std::vector<NativePipelineDesc> back;
  std::string why;
  check(psom::parse(image, back, &why) && back.size() == 3, "round_trip_parses");
  bool hits = back.size() == 3;
  for (size_t i = 0; i < back.size(); ++i) {
    back[i].rootSignature = root;
    NativePipelineLease l;
    const HRESULT hr = cache.acquire(back[i], l, NativePipelineAcquire::Warm);
    hits = hits && hr == S_FALSE && l.pipeline() == (i == 0 ? la : i == 1 ? lb : lc).pipeline();
  }
  check(hits, "parsed_descriptions_hit_the_original_pipelines (same cache key)");
  check(back.size() == 3 && back[0].vertex.bytes().data() == back[2].vertex.bytes().data() &&
            back[0].pixel.bytes().data() == back[2].pixel.bytes().data() && !(back[1].pixel == back[0].pixel),
        "equal_shaders_share_one_code_after_parse; the variant pixel stays distinct");
  check(psom::identity(a) == psom::identity(back[0]) && psom::identity(a) != psom::identity(b) &&
            psom::identity(a) != psom::identity(c),
        "identity_ignores_the_root_and_separates_state_and_shaders");
  // vs + ps + ps2 stored once each: the header's shader count (u32 after the magic and the version) is 3
  const uint32_t nShaders = image[12] | (image[13] << 8) | (image[14] << 16) | (uint32_t(image[15]) << 24);
  check(nShaders == 3, "each_shader_stored_once (3 table entries for 3 pipelines' 6 stage uses)");
  // malformed images
  auto bad = image;
  bad[0] ^= 1;
  check(!psom::parse(bad, back) && back.empty(), "wrong_magic_rejected");
  bad = image;
  bad[8 + 4 + 4 + 4 + 8 + 4 + 1] ^= 0x40;   // inside the first shader's bytes (header 20 + its hash 8 + size 4)
  check(!psom::parse(bad, back), "corrupt_shader_bytes_rejected (content hash)");
  bad.assign(image.begin(), image.end() - 5);
  check(!psom::parse(bad, back), "truncated_image_rejected");
  bad = image;
  bad.push_back(0);
  check(!psom::parse(bad, back), "trailing_bytes_rejected");
  bad = image;
  bad[8] = 9;
  check(!psom::parse(bad, back, &why) && why.find("version") != std::string::npos, "other_version_rejected");
  check(psom::parse(psom::serialize({}), back) && back.empty(), "empty_manifest_round_trips");
  // merge: existing order first, duplicates dropped
  std::vector<NativePipelineDesc> existing;
  psom::parse(psom::serialize({&c, &a}), existing);
  const auto merged = psom::merge(existing, {&a, &b});
  check(merged.size() == 3 && psom::identity(*merged[0]) == psom::identity(c) &&
            psom::identity(*merged[1]) == psom::identity(a) && psom::identity(*merged[2]) == psom::identity(b),
        "merge_keeps_order_and_drops_duplicates");
  // the file helpers
  char tmp[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  const std::string path = std::string(tmp) + "sr_pso_manifest_test\\m.bin";
  std::vector<uint8_t> read;
  check(psom::writeFileAtomic(path, image) && psom::readFile(path, read) && read == image, "file_round_trip");
  // fix12 (Astra on fix11): the writer enforces the reader's limits. An entry the reader would reject (33 inputs) is
  // left out, the rest parses; an image over the 64 MB cap is not made, and the merge keeps the previous file whole.
  {
    NativePipelineDesc wide = b;
    wide.inputLayout.assign(33, {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 0});
    size_t skipped = 0;
    std::string w;
    const auto img = psom::serialize({&a, &wide, &c}, &w, &skipped);
    std::vector<NativePipelineDesc> got;
    check(skipped == 1 && psom::parse(img, got) && got.size() == 2 && psom::identity(got[0]) == psom::identity(a) &&
              psom::identity(got[1]) == psom::identity(c),
          "fix12_an_entry_over_the_reader_s_limits_is_left_out_and_the_rest_parses");
    NativePipelineDesc big = a;
    {
      std::vector<uint8_t> huge(size_t(psom::kMaxFileBytes), 0x11);   // one 64 MB "shader": the file would be over
      require(NativeShaderCode::copy(huge, big.vertex), "big shader");
    }
    const auto over = psom::serialize({&a, &big}, &w);
    check(over.empty() && w.find("64 MB") != std::string::npos, "fix12_an_image_over_the_64_MB_cap_is_not_made");
    std::vector<uint8_t> before, after;
    psom::readFile(path, before);
    psom::MergeResult mr;
    std::string mergeWhy;
    const bool merged = psom::mergeIntoFile(path, {}, {&big}, &mr, &mergeWhy);
    psom::readFile(path, after);
    check(!merged && after == before && after == image && mergeWhy.find("previous file is kept") != std::string::npos,
          "fix12_a_merge_over_the_cap_keeps_the_previous_file");
    DeleteFileA((path + ".lock").c_str());
  }
  std::printf("[PSO_MANIFEST_TEST] passed=%u failed=%u\n", passed, failed);
  return failed ? 1 : 0;
}
