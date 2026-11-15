// Native sampler ABI and lifetime proof. Run serially through tools/game_slot.py.
#include "gfx/translated_samplers.hpp"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;
namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[TRANSLATED_SAMPLERS_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) { std::fprintf(stderr, "SETUP FAILED %s hr=%08lX\n", name, (unsigned long)hr); std::exit(2); }
}
NativeShaderCode compile(const char* source, const char* entry, const char* profile) {
  ComPtr<ID3DBlob> blob, errors;
  const auto hr = D3DCompile(source, std::strlen(source), "sampler_fixture", nullptr, nullptr,
      entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
  if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", (const char*)errors->GetBufferPointer());
  require(hr, entry); NativeShaderCode code;
  require(NativeShaderCode::copy({(const uint8_t*)blob->GetBufferPointer(), blob->GetBufferSize()}, code), "shader copy");
  return code;
}
void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  commands->ResourceBarrier(1, &barrier);
}
void recipeChecks() {
  TranslatedSamplerRecipes recipes;
  bool legal = true, tail = true;
  for (uint32_t aniso : {0u, 2u, 4u, 8u, 16u}) for (float bias : {-2.f, 0.f, 2.f}) {
    legal &= SUCCEEDED(translatedSamplerRecipes(aniso, bias, recipes));
    for (uint32_t i = 0; i < 16; ++i) {
      const auto& s = recipes[i].sampler;
      const auto filter = !(i & 1) ? D3D12_FILTER_MIN_MAG_MIP_POINT :
          aniso ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      legal &= s.Filter == filter && s.MaxAnisotropy == ((i & 1) && aniso ? aniso : 1) &&
          s.MipLODBias == bias && s.MinLOD == 0 && s.MaxLOD == D3D12_FLOAT32_MAX &&
          s.AddressU == ((i & 4) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP) &&
          s.AddressV == ((i & 2) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP) &&
          s.AddressW == ((i & 8) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP) &&
          s.ComparisonFunc == D3D12_COMPARISON_FUNC_NEVER;
    }
    for (uint32_t i = kXlatLegacySamplers; i < kXlatSamplerTable; ++i)
      tail &= std::memcmp(&recipes[i].sampler, &recipes[0].sampler, sizeof(D3D12_SAMPLER_DESC)) == 0;
  }
  check(legal, "all_sixteen_indices_preserve_filter_address_and_graphics_options");
  check(tail, "unassigned_volume_entries_repeat_index_0");
  const auto original = recipes[0].sampler.MipLODBias;
  check(translatedSamplerRecipes(1, 0, recipes) == E_INVALIDARG && recipes[0].sampler.MipLODBias == original,
        "invalid_anisotropy_preserves_recipes");
  check(translatedSamplerRecipes(16, std::numeric_limits<float>::quiet_NaN(), recipes) == E_INVALIDARG &&
      translatedSamplerRecipes(16, std::numeric_limits<float>::infinity(), recipes) == E_INVALIDARG,
      "nonfinite_lod_bias_rejected");
  check(translatedSamplerRecipes(0, -2.01f, recipes) == E_INVALIDARG &&
      translatedSamplerRecipes(0, 2.01f, recipes) == E_INVALIDARG, "out_of_range_lod_bias_rejected");
}
// Step-4 lane C round 2 (SR_XLAT_SAMPLER_V2): Xenia's GetSamplerParameters + WriteSampler for k3D fetches, with
// Astra's P2-3 / P2-4 counterexamples (dev 7af4a541: clamp_z 1 sampled WRAP, mirror-clamp / border became CLAMP,
// linear mag + point mip became trilinear, mip_min_level ignored).
uint32_t volumeFetch(uint32_t clampX, uint32_t clampY, uint32_t clampZ, uint32_t mag, uint32_t min, uint32_t mip,
                     uint32_t aniso, uint32_t mipMin, uint32_t mipMax, uint32_t border, uint32_t* f) {
  f[0] = 2u | (clampX << 10) | (clampY << 13) | (clampZ << 16) | (2u << 22) | (1u << 31);
  f[1] = 6u | (0x103A6u << 12);
  f[2] = 63u | (63u << 11) | (63u << 22);  // 64^3
  f[3] = (mag << 19) | (min << 21) | (mip << 23) | (aniso << 25);
  f[4] = (mipMin << 2) | (mipMax << 6);
  f[5] = border | (2u << 9) | (0x103B7u << 12);  // k3D, a mip page
  return 0;
}
// Round 3 (SR_XLAT_SAMPLER_V3): which entry a full volume table reclaims (Astra round-2 P2: the 49th distinct
// sampler fell back to a legacy recipe).
void reclaimChecks() {
  const std::vector<uint64_t> lastUse{7, 3, 9, 3, 5};
  check(reclaimableVolumeSampler(lastUse, 9) == 1, "reclaims_the_least_recently_bound_completed_entry_first_of_ties");
  check(reclaimableVolumeSampler(lastUse, 4) == 1, "only_entries_whose_frame_completed_are_reclaimable");
  check(reclaimableVolumeSampler(lastUse, 2) == SIZE_MAX, "none_reclaimable_while_every_frame_is_in_flight");
  const std::vector<uint64_t> current{10, 10, 10};
  check(reclaimableVolumeSampler(current, 9) == SIZE_MAX, "entries_bound_in_the_frame_being_recorded_are_kept");
  check(reclaimableVolumeSampler({}, 100) == SIZE_MAX, "empty_table_none");
}

void volumeChecks(ID3D12Device* device) {
  uint32_t f[6];
  volumeFetch(0, 0, 1, 1, 1, 1, 0, 0, 6, 0, f);
  auto d = volumeSamplerDesc(volumeSamplerParams(f), 0, 0);
  check(d.AddressW == D3D12_TEXTURE_ADDRESS_MODE_MIRROR && d.AddressU == D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        "clamp_z_mirrored_repeat_is_MIRROR_not_WRAP");
  bool modes = true;
  const D3D12_TEXTURE_ADDRESS_MODE want[8] = {
      D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE};
  for (uint32_t m = 0; m < 8; ++m) {
    volumeFetch(m, m, m, 1, 1, 1, 0, 0, 6, 0, f);
    d = volumeSamplerDesc(volumeSamplerParams(f), 0, 0);
    modes &= d.AddressU == want[m] && d.AddressV == want[m] && d.AddressW == want[m];
  }
  check(modes, "every_xenos_clamp_mode_maps_as_xenia_kAddressModeMap");
  volumeFetch(2, 2, 6, 1, 1, 1, 0, 0, 6, 1, f);  // border on W, k_ABGR_White
  d = volumeSamplerDesc(volumeSamplerParams(f), 0, 0);
  check(d.AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER && d.BorderColor[0] == 1.f && d.BorderColor[3] == 1.f,
        "border_mode_carries_the_fetch_border_colour");
  volumeFetch(2, 2, 2, 1, 1, 1, 0, 0, 6, 1, f);  // no border mode: the colour is dropped (Xenia)
  check(volumeSamplerParams(f).borderColor == 0, "no_border_mode_border_colour_black");
  volumeFetch(0, 0, 0, 1, 1, 0, 0, 0, 6, 0, f);  // linear mag / min, POINT mip
  d = volumeSamplerDesc(volumeSamplerParams(f), 0, 0);
  check(d.Filter == D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, "linear_mag_min_point_mip_is_not_trilinear");
  volumeFetch(0, 0, 0, 0, 1, 1, 0, 0, 6, 0, f);  // point mag, linear min, linear mip
  d = volumeSamplerDesc(volumeSamplerParams(f), 0, 0);
  check(d.Filter == D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR, "min_and_mag_filters_independent");
  volumeFetch(0, 0, 0, 1, 1, 1, 0, 2, 6, 0, f);
  d = volumeSamplerDesc(volumeSamplerParams(f), 0, 0);
  check(d.MinLOD == 2.f && d.MaxLOD == D3D12_FLOAT32_MAX, "mip_min_level_is_MinLOD");
  volumeFetch(0, 0, 0, 1, 1, 2, 0, 1, 6, 0, f);  // kBaseMap mip filter, mip_min 1
  d = volumeSamplerDesc(volumeSamplerParams(f), 0, 0);
  check(d.MinLOD == 1.f && d.MaxLOD == 1.25f && d.Filter == D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
        "basemap_is_MaxLOD_MinLOD_plus_quarter_over_the_kept_chain");
  volumeFetch(0, 0, 0, 1, 1, 1, 3, 0, 6, 0, f);  // aniso kMax_4_1
  d = volumeSamplerDesc(volumeSamplerParams(f), 0, 0);
  check(d.Filter == D3D12_FILTER_ANISOTROPIC && d.MaxAnisotropy == 4, "fetch_anisotropy_4_1");
  volumeFetch(0, 0, 0, 1, 1, 1, 0, 0, 6, 0, f);
  d = volumeSamplerDesc(volumeSamplerParams(f), 16, 0.5f);
  check(d.Filter == D3D12_FILTER_ANISOTROPIC && d.MaxAnisotropy == 16 && d.MipLODBias == 0.5f,
        "anisotropy_option_overrides_a_trilinear_mipmapped_volume");
  volumeFetch(0, 0, 0, 0, 1, 1, 0, 0, 6, 0, f);  // point mag: the option does not apply (Xenia's rule)
  d = volumeSamplerDesc(volumeSamplerParams(f), 16, 0);
  check(d.Filter == D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR && d.MaxAnisotropy == 1,
        "anisotropy_option_skips_point_magnification");
  // The table: volume samplers land at 16+, a 49th is refused, the generation keys the native version.
  std::vector<VolumeSamplerParams> vols;
  volumeFetch(0, 0, 1, 1, 1, 0, 0, 0, 6, 0, f);
  vols.push_back(volumeSamplerParams(f));
  TranslatedSamplerRecipes recipes;
  check(SUCCEEDED(translatedSamplerRecipes(0, 0, vols, recipes)) &&
            recipes[kXlatLegacySamplers].sampler.AddressW == D3D12_TEXTURE_ADDRESS_MODE_MIRROR &&
            recipes[kXlatLegacySamplers].sampler.Filter == D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
        "first_volume_sampler_at_index_16");
  std::vector<VolumeSamplerParams> many(kXlatVolumeSamplers + 1, vols[0]);
  check(translatedSamplerRecipes(0, 0, many, recipes) == E_INVALIDARG, "volume_samplers_beyond_48_rejected");
  TranslatedSamplerCache cache; NativeDescriptorVersion a, b2, c;
  require(cache.select(device, 0, 0, {}, 0, a), "gen 0");
  require(cache.select(device, 0, 0, vols, 1, b2), "gen 1");
  require(cache.select(device, 0, 0, vols, 1, c), "gen 1 again");
  check(a.heap() != b2.heap() && b2.heap() == c.heap(), "a_new_volume_sampler_is_a_new_version_reused_after");
}
void budgetChecks(ID3D12Device* device) {
  TranslatedSamplerCache cache; NativeDescriptorVersion first, again, current;
  check(cache.select(nullptr, 0, 0, first) == E_INVALIDARG && !first, "null_device_rejected");
  require(cache.select(device, 0, 0, first), "default version");
  require(cache.select(device, 0, 0, again), "default reuse");
  check(first.heap() == again.heap() && first.count() == kXlatSamplerTable,
        "default_table_reused_with_the_full_table");
  require(cache.select(device, 16, -2, current), "override version"); const auto overrideHeap = current.heap();
  require(cache.select(device, 16, -2, again), "override reuse");
  check(overrideHeap == again.heap() && overrideHeap != first.heap(), "unchanged_options_reuse_distinct_version");
  check(cache.select(device, 3, 0, current) == E_INVALIDARG && current.heap() == overrideHeap,
        "invalid_selection_preserves_published_output");
  require(cache.select(device, 0, 0, again), "return to defaults");
  check(again.heap() == first.heap(), "return_to_defaults_reuses_original_version");
  ComPtr<ID3D12Device> foreign;
  require(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&foreign)), "foreign device");
  check(cache.select(foreign.Get(), 0, 0, current) == E_INVALIDARG && current.heap() == overrideHeap,
        "foreign_device_rejected_without_output_change");
  TranslatedSamplerCache bounded;
  std::array<NativeDescriptorVersion, TranslatedSamplerCache::kMaxLiveVersions> held;
  for (size_t i = 0; i < held.size(); ++i)
    require(bounded.select(device, 0, -2.f + float(i) / 16.f, held[i]), "bounded version");
  check(bounded.liveVersions() == held.size(), "all_retained_versions_count_toward_budget");
  const auto handle = current.heap();
  check(bounded.select(device, 0, 1.99f, current) == E_OUTOFMEMORY && current.heap() == handle,
        "version_budget_failure_preserves_output");
  const auto retired = held[0].ownership(); held[0] = {};
  check(retired.expired(), "released_old_version_returns_budget");
  require(bounded.select(device, 0, 1.99f, current), "reuse returned budget");
  check(bounded.liveVersions() == held.size(), "returned_budget_admits_new_immutable_version");
}
constexpr char shader[] = R"(
  Texture2D<float4> tex : register(t0);
  SamplerState samplers[16] : register(s0);
  cbuffer Params : register(b0) { float2 uv; uint samplerIndex; };
  struct Vertex { float4 position : SV_Position; float2 gradient : TEXCOORD; };
  Vertex VS(uint id : SV_VertexID) {
    Vertex value; value.position = float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, .5, 1);
    value.gradient = value.position.xy * .25; return value;
  }
  float4 PS(Vertex value) : SV_Target { return tex.Sample(samplers[samplerIndex], uv + value.gradient); }
)";
void delayedPixels(ID3D12Device* device, ID3D12CommandQueue* queue, NativeImageStore& images) {
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3]{};
  for (uint32_t i = 0; i < 2; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i].DescriptorTable = {1, &ranges[i]}; params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants = {0, 0, 3}; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  NativeRootSignature root; require(NativeRootSignature::create(device, rootDesc, root), "root");
  NativePipelineDesc pipelineDesc; pipelineDesc.rootSignature = root.root();
  pipelineDesc.vertex = compile(shader, "VS", "vs_5_1"); pipelineDesc.pixel = compile(shader, "PS", "ps_5_1");
  pipelineDesc.colorCount = 1; pipelineDesc.colorFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pipelineDesc.rasterizer.FillMode = D3D12_FILL_MODE_SOLID; pipelineDesc.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
  pipelineDesc.rasterizer.DepthClipEnable = TRUE;
  auto& blend = pipelineDesc.blend.RenderTarget[0]; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO; blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipelineDesc.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pipelineDesc.depthStencil.StencilReadMask = pipelineDesc.depthStencil.StencilWriteMask = 255;
  pipelineDesc.depthStencil.FrontFace = pipelineDesc.depthStencil.BackFace = {
      D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  NativePipelineCache pipelines; require(pipelines.initialize(device), "pipelines");
  NativePipelineLease pipeline; require(pipelines.acquire(pipelineDesc, pipeline), "pipeline");
  NativeImageDesc colorDesc; colorDesc.width = 8; colorDesc.height = 12;
  colorDesc.resourceFormat = colorDesc.clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  colorDesc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
  NativeImageLease color; require(images.create(colorDesc, color), "output");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 readbackBytes = 0;
  auto desc = color.resource()->GetDesc(); device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
  D3D12_HEAP_PROPERTIES readHeap{}; readHeap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rb{}; rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rb.Width = readbackBytes;
  rb.Height = rb.DepthOrArraySize = rb.MipLevels = 1; rb.SampleDesc.Count = 1; rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  require(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&readback)), "readback");
  ComPtr<ID3D12Fence> delay; require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay)), "delay");
  require(queue->Wait(delay.Get(), 1), "delayed execution");
  std::weak_ptr<const void> owner, optionOwner; uint64_t ticket = 0;
  {
    NativeDrawRecording recording; require(recording.begin(device), "recording");
    check(recording.keep(NativeDescriptorVersion{}) == E_INVALIDARG, "empty_descriptor_version_rejected");
    require(recording.keep(color), "retain output"); require(recording.keep(readback.Get()), "retain readback");
    NativeUploadArena upload; require(upload.initialize(device, 65536, 65536), "upload");
    std::array<uint8_t, 1024> texels{};
    constexpr uint8_t source[2][8] = {{255, 0, 0, 255, 0, 255, 0, 255}, {0, 0, 255, 255, 255, 255, 255, 255}};
    std::memcpy(texels.data(), source[0], 8); std::memcpy(texels.data() + 256, source[1], 8);
    texels[512] = texels[513] = texels[515] = 255;  // mip 1 is yellow
    NativeBufferSlice bytes; require(upload.stage(texels, 512, bytes), "immutable texels"); require(recording.keep(bytes), "retain texels");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = td.Height = 2;
    td.DepthOrArraySize = 1; td.MipLevels = 2; td.SampleDesc.Count = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> texture;
    require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&texture)), "texture"); require(recording.keep(texture.Get()), "retain texture");
    D3D12_TEXTURE_COPY_LOCATION from{}, to{}; from.pResource = bytes.resource(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = bytes.offset(); from.PlacedFootprint.Footprint = {td.Format, 2, 2, 1, 256};
    to.pResource = texture.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    recording.commands()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    from.PlacedFootprint.Offset += 512; from.PlacedFootprint.Footprint = {td.Format, 1, 1, 1, 256};
    to.SubresourceIndex = 1; recording.commands()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(recording.commands(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    NativeViewRecipe view; view.resource = texture; view.srv.Format = td.Format; view.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.srv.Texture2D.MipLevels = 2;
    NativeDescriptorVersion views;
    require(NativeDescriptorVersion::create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {&view, 1}, views), "texture view");
    require(recording.keep(views), "retain texture descriptors");
    TranslatedSamplerCache cache; NativeDescriptorVersion samplers, options;
    require(cache.select(device, 0, 0, samplers), "selected sampler version"); owner = samplers.ownership();
    require(cache.select(device, 16, 1, options), "option sampler version"); optionOwner = options.ownership();
    require(recording.keep(samplers), "retain sampler descriptors"); require(recording.keep(options), "retain option descriptors");
    // Direct native recording matches the production adapter, rather than a
    // PreparedNativeDraw also retaining the version and hiding a keep() defect.
    auto* commands = recording.commands();
    commands->SetGraphicsRootSignature(root.root()); commands->SetPipelineState(pipeline.pipeline());
    auto rtv = color.view(); commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (uint32_t phase = 0; phase < 3; ++phase) {
      const auto& selected = phase == 1 ? options : samplers;
      require(recording.keep(selected), "repeat sampler retention");
      ID3D12DescriptorHeap* heaps[2] = {views.heap(), selected.heap()}; commands->SetDescriptorHeaps(2, heaps);
      commands->SetGraphicsRootDescriptorTable(0, views.gpu()); commands->SetGraphicsRootDescriptorTable(1, selected.gpu());
      for (uint32_t panel = 0; panel < 2; ++panel) for (uint32_t i = 0; i < 16; ++i) {
        const float uv = panel ? .5f : 1.25f;
        uint32_t constants[3]; std::memcpy(constants, &uv, 4); std::memcpy(constants + 1, &uv, 4); constants[2] = i;
        const auto y = i / 8 + panel * 2 + phase * 4;
        D3D12_VIEWPORT viewport{float(i % 8), float(y), 1, 1, 0, 1};
        D3D12_RECT scissor{LONG(i % 8), LONG(y), LONG(i % 8 + 1), LONG(y + 1)};
        commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
        commands->SetGraphicsRoot32BitConstants(2, 3, constants, 0); commands->DrawInstanced(3, 1, 0, 0);
      }
    }
    samplers = {}; options = {};
    texels.fill(0);
    for (uint32_t i = 0; i < 9; ++i) require(cache.select(device, i & 1 ? 16 : 2, -2.f + float(i) / 2, samplers), "replace option version");
    check(cache.liveVersions() == 3, "queued_version_survives_more_than_seven_option_changes");
    if (owner.expired() || optionOwner.expired()) {
      std::puts("[TRANSLATED_SAMPLERS_TEST] ABORT dangling recorded sampler before submission"); std::exit(1);
    }
    transition(commands, color.resource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    from = {}; to = {}; from.pResource = color.resource(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    require(recording.close(), "close");
    check(recording.keep(samplers) == E_ABORT, "closed_recording_rejects_descriptor_retention");
    require(recording.submit(images, ticket), "submit");
  }
  check(images.wait(ticket, 0) == HRESULT_FROM_WIN32(WAIT_TIMEOUT), "actual_queue_still_delayed");
  check(!owner.expired() && !optionOwner.expired(), "sampler_retained_after_cache_and_recording_destruction");
  if (owner.expired() || optionOwner.expired()) { std::puts("[TRANSLATED_SAMPLERS_TEST] ABORT dangling queued sampler before GPU execution"); std::exit(1); }
  require(delay->Signal(1), "release delay"); require(images.wait(ticket, 10000), "completion");
  uint8_t* data = nullptr; const D3D12_RANGE range{0, SIZE_T(readbackBytes)};
  require(readback->Map(0, &range, reinterpret_cast<void**>(&data)), "map pixels");
  bool addresses = true, filtering = true, bias = true;
  for (uint32_t phase = 0; phase < 3; ++phase) for (uint32_t panel = 0; panel < 2; ++panel) for (uint32_t i = 0; i < 16; ++i) {
    std::array<uint8_t, 4> expected{255, 255, 255, 255};
    if (phase == 1) expected = {255, 255, 0, 255};
    else if (!panel) {
      const bool right = (i & 4) != 0, bottom = (i & 2) != 0;
      expected = {uint8_t(right == bottom ? 255 : 0), uint8_t(right ? 255 : 0), uint8_t(bottom ? 255 : 0), 255};
    } else if (i & 1) expected = {128, 128, 128, 255};
    const auto* pixel = data + footprint.Offset + (i / 8 + panel * 2 + phase * 4) * footprint.Footprint.RowPitch + (i % 8) * 4;
    (phase == 1 ? bias : panel ? filtering : addresses) &= std::memcmp(pixel, expected.data(), 4) == 0;
  }
  const D3D12_RANGE noWrites{0, 0}; readback->Unmap(0, &noWrites);
  check(addresses, "all_wrap_clamp_indices_exact_pixels_after_owner_destruction");
  check(filtering, "all_point_linear_indices_exact_pixels_after_owner_destruction");
  check(bias, "lod_bias_selects_expected_mip_between_actual_a_b_a_heap_switches");
  require(images.collect(), "collect");
  check(owner.expired() && optionOwner.expired(), "sampler_version_retired_after_checked_completion");
  require(images.release(color.handle()), "release output");
}
}
int main() {
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  D3D12_COMMAND_QUEUE_DESC desc{}; require(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
  recipeChecks(); budgetChecks(device.Get()); volumeChecks(device.Get()); reclaimChecks();
  NativeImageStore images; require(images.initialize(device.Get(), queue.Get(), 4, 4), "images");
  delayedPixels(device.Get(), queue.Get(), images); require(images.shutdown(), "shutdown");
  std::printf("[TRANSLATED_SAMPLERS_TEST] RESULT checks=%u passed=%u failed=%u\n", passed + failed, passed, failed);
  return failed ? 1 : 0;
}
