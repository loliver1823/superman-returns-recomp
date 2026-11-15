// Native texture-view admission and pixels; hold tools/game_slot.py while running.
#include "gfx/native_draw.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;
namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[NATIVE_TEXTURE_VIEWS_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) { std::fprintf(stderr, "SETUP FAILED %s hr=%08lX\n", name, (unsigned long)hr); std::exit(2); }
}
void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  commands->ResourceBarrier(1, &b);
}
NativeShaderCode compile(const char* source, const char* entry, const char* profile) {
  ComPtr<ID3DBlob> blob, errors;
  const auto hr = D3DCompile(source, std::strlen(source), "texture_view_fixture", nullptr, nullptr,
      entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
  if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", (const char*)errors->GetBufferPointer());
  require(hr, entry); NativeShaderCode code;
  require(NativeShaderCode::copy({(const uint8_t*)blob->GetBufferPointer(), blob->GetBufferSize()}, code), "shader copy");
  return code;
}
std::array<uint8_t, 4> texel(uint32_t layer, uint32_t z, uint32_t mip) {
  return {uint8_t(17 + layer * 19 + mip * 71), uint8_t(31 + z * 47 + mip * 23),
          uint8_t(83 + layer * 13 + z * 11 + mip * 29), 255};
}
NativeViewRecipe texture(ID3D12Device* device, NativeDrawRecording& recording, NativeUploadArena& uploads,
                         D3D12_SRV_DIMENSION dimension, uint16_t depthOrLayers, bool gray = false) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = dimension == D3D12_SRV_DIMENSION_TEXTURE3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = 4; desc.DepthOrArraySize = depthOrLayers; desc.MipLevels = 2;
  desc.Format = gray ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  NativeViewRecipe view;
  require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&view.resource)), "texture allocation");
  require(recording.keep(view.resource.Get()), "retain texture");
  const UINT count = desc.MipLevels * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : depthOrLayers);
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
  std::vector<UINT> rows(count); std::vector<UINT64> rowBytes(count); UINT64 total = 0;
  device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), rowBytes.data(), &total);
  std::vector<uint8_t> bytes(size_t(total), 0);
  for (UINT sub = 0; sub < count; ++sub) {
    const auto& f = footprints[sub]; const UINT mip = sub % desc.MipLevels, layer = sub / desc.MipLevels;
    for (UINT z = 0; z < f.Footprint.Depth; ++z) for (UINT y = 0; y < rows[sub]; ++y)
      for (UINT x = 0; x < f.Footprint.Width; ++x) {
        const auto color = gray ? (mip ? std::array<uint8_t, 4>{0, 128, 0, 255} : std::array<uint8_t, 4>{128, 128, 128, 255}) : texel(layer, z, mip);
        std::memcpy(bytes.data() + f.Offset + (uint64_t(z) * rows[sub] + y) * f.Footprint.RowPitch + x * 4, color.data(), 4);
      }
  }
  NativeBufferSlice data; require(uploads.stage(bytes, 512, data), "texture bytes");
  require(recording.keep(data), "retain texture upload"); bytes.assign(bytes.size(), 0);
  for (UINT sub = 0; sub < count; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = data.resource(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprints[sub]; from.PlacedFootprint.Offset += data.offset();
    to.pResource = view.resource.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = sub;
    recording.commands()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  transition(recording.commands(), view.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  view.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view.srv.ViewDimension = dimension;
  view.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  switch (dimension) {
    case D3D12_SRV_DIMENSION_TEXTURE2D: view.srv.Texture2D.MipLevels = 2; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: view.srv.Texture2DArray.MipLevels = 2; view.srv.Texture2DArray.ArraySize = depthOrLayers; break;
    case D3D12_SRV_DIMENSION_TEXTURE3D: view.srv.Texture3D.MipLevels = 2; break;
    default: view.srv.TextureCube.MipLevels = 2; break;
  }
  return view;
}
void admission(ID3D12Device* device, const std::array<NativeViewRecipe, 6>& views) {
  NativeDescriptorVersion output;
  require(NativeDescriptorVersion::create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, views, output), "all texture dimensions");
  check(output.count() == views.size(), "typed_arrays_volumes_cubes_and_depth_planes_admitted");
  const auto original = output.heap();
  NativeDescriptorVersion duplicateOutput = output;
  std::array<NativeViewRecipe, 3> repeated{views[0], views[0], views[0]};
  NativeDescriptorVersion duplicates;
  check(SUCCEEDED(NativeDescriptorVersion::create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, repeated, duplicates)) &&
      duplicates.count() == repeated.size(), "identical_views_admitted");
  repeated[2].srv.Texture2D.MostDetailedMip = 1;
  check(NativeDescriptorVersion::create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, repeated, duplicateOutput) == E_INVALIDARG &&
      duplicateOutput.heap() == original, "changed_duplicate_view_rejected_without_replacing_output");
  duplicateOutput = output;
  NativeUploadArena uploads; require(uploads.initialize(device, 64u << 10, 64u << 10), "duplicate buffer arena");
  std::array<uint8_t, 16> bytes{}; NativeBufferSlice first, second;
  require(uploads.stage(bytes, 4, first), "first buffer slice");
  require(uploads.stage(bytes, 4, second), "second buffer slice");
  std::array<NativeViewRecipe, 2> buffers{};
  buffers[0].buffer = first; buffers[0].srv.Format = DXGI_FORMAT_R32_TYPELESS;
  buffers[0].srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  buffers[0].srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  buffers[0].srv.Buffer = {first.offset() / 4, 4, 0, D3D12_BUFFER_SRV_FLAG_RAW};
  buffers[1] = buffers[0]; buffers[1].buffer = second;
  check(first.resource() == second.resource() && first.offset() != second.offset() &&
      NativeDescriptorVersion::create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, buffers, duplicateOutput) == E_INVALIDARG &&
      duplicateOutput.heap() == original, "changed_duplicate_buffer_slice_rejected_without_replacing_output");
  auto reject = [&](NativeViewRecipe bad, const char* name) {
    check(NativeDescriptorVersion::create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {&bad, 1}, output) == E_INVALIDARG &&
        output.heap() == original, name);
  };
  auto bad = views[1]; bad.srv.Texture2DArray.ArraySize = 0; reject(bad, "empty_array_rejected_without_replacing_output");
  bad = views[1]; bad.srv.Texture2DArray.FirstArraySlice = 3; bad.srv.Texture2DArray.ArraySize = 2; reject(bad, "array_extent_rejected");
  bad = views[1]; bad.srv.Texture2DArray.FirstArraySlice = UINT32_MAX; reject(bad, "array_overflow_rejected");
  bad = views[1]; bad.srv.Texture2DArray.MostDetailedMip = 1; bad.srv.Texture2DArray.MipLevels = 2; reject(bad, "array_mip_extent_rejected");
  bad = views[2]; bad.srv.Texture3D.MipLevels = 0; reject(bad, "empty_volume_mips_rejected");
  bad = views[2]; bad.srv.Texture3D.MostDetailedMip = UINT32_MAX; reject(bad, "volume_mip_overflow_rejected");
  bad = views[3]; bad.srv.TextureCube.MipLevels = 3; reject(bad, "cube_mip_extent_rejected");
  bad = views[3]; bad.resource = views[1].resource; reject(bad, "cube_with_fewer_than_six_faces_rejected");
  bad = views[2]; bad.resource = views[1].resource; reject(bad, "volume_view_of_array_rejected");
  bad = views[1]; bad.resource = views[2].resource; reject(bad, "array_view_of_volume_rejected");
  bad = views[0]; bad.resource = views[1].resource; reject(bad, "plain_2d_view_of_array_rejected");
  bad = views[0]; bad.srv.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; reject(bad, "typeless_shader_view_rejected");
  bad = views[0]; bad.srv.Format = DXGI_FORMAT_R16G16_UNORM; reject(bad, "incompatible_typeless_family_rejected");
  bad = views[1]; bad.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; reject(bad, "typed_resource_reinterpretation_rejected");
  bad = views[4]; bad.srv.Texture2D.PlaneSlice = 1; reject(bad, "depth_format_on_stencil_plane_rejected");
  bad = views[5]; bad.srv.Texture2D.PlaneSlice = 0; reject(bad, "stencil_format_on_depth_plane_rejected");
  bad = views[5]; bad.srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT; reject(bad, "wrong_depth_stencil_family_rejected");
  bad = views[1]; bad.srv.Texture2DArray.PlaneSlice = 1; reject(bad, "colour_plane_one_rejected");
  bad = views[0]; bad.srv.Texture2D.ResourceMinLODClamp = std::numeric_limits<float>::quiet_NaN(); reject(bad, "nonfinite_view_lod_rejected");
  bad = views[0]; bad.srv.Texture2D.ResourceMinLODClamp = 1; reject(bad, "unchecked_nonzero_view_lod_rejected");
  bad = views[5]; bad.resource.Reset(); reject(bad, "unchecked_null_stencil_plane_rejected");
  bad = views[0]; bad.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; reject(bad, "unimplemented_multisample_view_rejected");
}
constexpr char shader[] = R"(
  Texture2D<float4> colour : register(t0, space0);
  Texture2DArray<float4> layers : register(t0, space1);
  Texture3D<float4> volume : register(t0, space2);
  TextureCube<float4> cube : register(t0, space3);
  Texture2D<float> depth : register(t0, space4);
  Texture2D<uint4> stencil : register(t0, space5);
  SamplerState pointClamp : register(s0);
  cbuffer Params : register(b0) { uint kind; uint layer; uint mip; };
  float4 VS(uint id : SV_VertexID) : SV_Position {
    return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, .5, 1);
  }
  float4 PS() : SV_Target {
    float3 dirs[6] = {float3(1,0,0),float3(-1,0,0),float3(0,1,0),float3(0,-1,0),float3(0,0,1),float3(0,0,-1)};
    float3 value = 0;
    if (kind == 0) value = colour.Load(int3(0,0,mip)).rgb;
    else if (kind == 1) value = layers.Load(int4(0,0,layer,mip)).rgb;
    else if (kind == 2) value = volume.Load(int4(0,0,layer,mip)).rgb;
    else if (kind == 3) value = cube.SampleLevel(pointClamp, dirs[layer], mip).rgb;
    else if (kind == 4) value.r = depth.Load(int3(0,0,0));
    else value.r = stencil.Load(int3(0,0,0)).g / 255.0;
    return float4(value,1);
  }
)";
}
int main() {
  ComPtr<ID3D12Debug> debug;
  const bool debugEnabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
  if (debugEnabled) debug->EnableDebugLayer();
  std::printf("[NATIVE_TEXTURE_VIEWS_TEST] debug_layer=%u\n", unsigned(debugEnabled));
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  ComPtr<ID3D12InfoQueue> validation;
  if (debugEnabled) require(device.As(&validation), "validation messages");
  D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  require(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
  NativeImageStore images; require(images.initialize(device.Get(), queue.Get(), 4, 4), "images");
  auto setupRecording = std::make_unique<NativeDrawRecording>();
  auto& recording = *setupRecording; require(recording.begin(device.Get()), "recording");
  NativeUploadArena uploads; require(uploads.initialize(device.Get()), "uploads");
  std::array<NativeViewRecipe, 6> views;
  views[0] = texture(device.Get(), recording, uploads, D3D12_SRV_DIMENSION_TEXTURE2D, 1, true);
  views[1] = texture(device.Get(), recording, uploads, D3D12_SRV_DIMENSION_TEXTURE2DARRAY, 4);
  views[2] = texture(device.Get(), recording, uploads, D3D12_SRV_DIMENSION_TEXTURE3D, 4);
  views[3] = texture(device.Get(), recording, uploads, D3D12_SRV_DIMENSION_TEXTURECUBE, 6);
  NativeImageDesc dd; dd.width = dd.height = 4; dd.kind = NativeImageKind::DepthStencil;
  dd.resourceFormat = DXGI_FORMAT_R24G8_TYPELESS; dd.clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dd.clear.DepthStencil = {.25f, 7}; dd.depthClearFlags = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
  dd.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  NativeImageLease d24; require(images.create(dd, d24), "D24S8"); require(recording.keep(d24), "retain D24S8");
  require(images.recordClear(recording.commands(), d24), "clear D24S8");
  transition(recording.commands(), d24.resource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  views[4].resource = d24.resource(); views[4].srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  views[4].srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  views[4].srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; views[4].srv.Texture2D.MipLevels = 1;
  views[5] = views[4]; views[5].srv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT; views[5].srv.Texture2D.PlaneSlice = 1;
  admission(device.Get(), views);
  std::array<NativeImageLease, 3> otherDepth;
  std::array<NativeViewRecipe, 4> depthViews;
  for (uint32_t i = 0; i < otherDepth.size(); ++i) {
    NativeImageDesc desc = dd;
    desc.resourceFormat = i == 0 ? DXGI_FORMAT_R16_TYPELESS :
        i == 1 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS;
    desc.clear.Format = i == 0 ? DXGI_FORMAT_D16_UNORM :
        i == 1 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    desc.clear.DepthStencil = {i == 2 ? .75f : .25f, 11};
    desc.depthClearFlags = i == 2 ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH;
    require(images.create(desc, otherDepth[i]), "additional depth allocation");
    require(recording.keep(otherDepth[i]), "retain additional depth");
    require(images.recordClear(recording.commands(), otherDepth[i]), "clear additional depth");
    transition(recording.commands(), otherDepth[i].resource(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    depthViews[i] = views[4]; depthViews[i].resource = otherDepth[i].resource();
    depthViews[i].srv.Format = i == 0 ? DXGI_FORMAT_R16_UNORM :
        i == 1 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  }
  depthViews[3] = depthViews[2]; depthViews[3].srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
  depthViews[3].srv.Texture2D.PlaneSlice = 1;
  NativeDescriptorVersion depthTable;
  require(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
      depthViews, depthTable), "additional depth views");
  const auto oldDepthHeap = depthTable.heap();
  auto wrongDepth = depthViews[0]; wrongDepth.srv.Format = DXGI_FORMAT_R16_FLOAT;
  check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
      {&wrongDepth, 1}, depthTable) == E_INVALIDARG && depthTable.heap() == oldDepthHeap,
      "d16_depth_resource_rejects_float_reinterpretation");
  wrongDepth = depthViews[1]; wrongDepth.srv.Format = DXGI_FORMAT_R32_UINT;
  check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
      {&wrongDepth, 1}, depthTable) == E_INVALIDARG && depthTable.heap() == oldDepthHeap,
      "d32_depth_resource_rejects_integer_reinterpretation");
  depthTable = {}; wrongDepth = {};
  // Finish initialization separately: the render packet must own texture SRVs
  // without the initialization packet's direct resource references masking it.
  require(recording.close(), "close texture initialization"); uint64_t uploadTicket = 0;
  require(recording.submit(images, uploadTicket), "submit texture initialization");
  setupRecording.reset(); require(images.wait(uploadTicket, 10000), "complete texture initialization");
  require(images.collect(), "collect texture initialization"); uploads.discardTail();
  auto drawRecording = std::make_unique<NativeDrawRecording>();
  auto& drawing = *drawRecording; require(drawing.begin(device.Get()), "draw recording");
  std::array<D3D12_ROOT_PARAMETER, 7> params{}; std::array<D3D12_DESCRIPTOR_RANGE, 6> ranges{};
  for (uint32_t i = 0; i < 6; ++i) {
    ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, i, 0};
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i].DescriptorTable = {1, &ranges[i]}; params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[6].Constants = {0,0,3};
  D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER; sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rd{UINT(params.size()), params.data(), 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  NativeRootSignature root; require(NativeRootSignature::create(device.Get(), rd, root), "root");
  NativePipelineDesc pd; pd.rootSignature = root.root(); pd.vertex = compile(shader,"VS","vs_5_1");
  pd.pixel = compile(shader,"PS","ps_5_1"); pd.colorCount = 1; pd.colorFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto& blend = pd.blend.RenderTarget[0]; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO; blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.rasterizer.FillMode = D3D12_FILL_MODE_SOLID; pd.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
  pd.rasterizer.DepthClipEnable = TRUE;
  pd.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pd.depthStencil.FrontFace = pd.depthStencil.BackFace = {
      D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  NativePipelineCache pipelines; require(pipelines.initialize(device.Get()), "pipelines");
  NativePipelineLease pipeline; require(pipelines.acquire(pd, pipeline), "pipeline");
  constexpr uint32_t pixelCases = 28;
  NativeImageDesc cd; cd.width = pixelCases; cd.height = 1; cd.resourceFormat = cd.clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  cd.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
  NativeImageLease target; require(images.create(cd,target), "target");
  std::vector<std::array<uint8_t,4>> expected; std::vector<const char*> labels;
  std::vector<NativeDescriptorVersion> versions;
  auto draw = [&](std::array<NativeViewRecipe,6> selected, uint32_t kind, uint32_t layer, uint32_t mip,
                  std::array<uint8_t,4> color, const char* label) {
    NativeDescriptorVersion table; require(NativeDescriptorVersion::create(device.Get(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,selected,table),label);
    const LONG x = LONG(expected.size()); expected.push_back(color); labels.push_back(label);
    PreparedNativeDraw d; d.pipeline = pipeline; d.signature = root; d.colors[0] = target;
    d.viewport = {float(x),0,1,1,0,1}; d.scissor = {x,0,x+1,1}; d.kind = NativeDrawKind::FetchVertices; d.count = 3;
    for (uint32_t i = 0; i < 6; ++i) { NativeRootBinding b; b.parameter = i; b.table = table; b.firstDescriptor = i; b.descriptorCount = 1; d.bindings.push_back(b); }
    NativeRootBinding b; b.parameter = 6; b.constants = {kind,layer,mip}; d.bindings.push_back(b);
    require(drawing.draw(d), "native draw"); versions.push_back(table);
  };
  draw(views,0,0,0,{128,128,128,255},"typeless_unorm_pixels");
  auto selected = views; selected[0].srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  draw(selected,0,0,0,{55,55,55,255},"typeless_srgb_pixels");
  draw(views,0,0,1,{0,128,0,255},"selected_mip_pixels");
  selected = views; selected[0].srv.Texture2D.MostDetailedMip = 1; selected[0].srv.Texture2D.MipLevels = 1;
  draw(selected,0,0,0,{0,128,0,255},"2d_view_first_mip_pixels");
  selected = views; selected[1].srv.Texture2DArray.FirstArraySlice = 2; selected[1].srv.Texture2DArray.ArraySize = 2;
  draw(selected,1,0,0,texel(2,0,0),"array_first_slice_pixels");
  draw(selected,1,1,1,texel(3,0,1),"array_relative_slice_and_mip_pixels");
  selected[1].srv.Texture2DArray.MostDetailedMip = 1; selected[1].srv.Texture2DArray.MipLevels = 1;
  draw(selected,1,1,0,texel(3,0,1),"array_view_first_mip_pixels");
  draw(views,2,3,0,texel(0,3,0),"volume_depth_slice_pixels");
  draw(views,2,1,1,texel(0,1,1),"volume_mip_depth_pixels");
  selected = views; selected[2].srv.Texture3D.MostDetailedMip = 1; selected[2].srv.Texture3D.MipLevels = 1;
  draw(selected,2,1,0,texel(0,1,1),"volume_view_first_mip_pixels");
  const char* faces[] = {"cube_positive_x_pixels","cube_negative_x_pixels","cube_positive_y_pixels","cube_negative_y_pixels","cube_positive_z_pixels","cube_negative_z_pixels"};
  for (uint32_t face = 0; face < 6; ++face) draw(views,3,face,0,texel(face,0,0),faces[face]);
  draw(views,3,4,1,texel(4,0,1),"cube_mip_pixels");
  selected = views; selected[3].srv.TextureCube.MostDetailedMip = 1; selected[3].srv.TextureCube.MipLevels = 1;
  draw(selected,3,4,0,texel(4,0,1),"cube_view_first_mip_pixels");
  draw(views,4,0,0,{64,0,0,255},"d24_depth_plane_pixels"); draw(views,5,0,0,{7,0,0,255},"d24_stencil_plane_pixels");
  const char* depthNames[] = {"d16_depth_pixels", "d32_depth_pixels", "d32s8_depth_plane_pixels", "d32s8_stencil_plane_pixels"};
  for (uint32_t i = 0; i < depthViews.size(); ++i) {
    selected = views; selected[i == 3 ? 5 : 4] = depthViews[i];
    const uint8_t value = i == 3 ? 11 : i == 2 ? 191 : 64;
    draw(selected, i == 3 ? 5 : 4, 0, 0, {value,0,0,255}, depthNames[i]);
  }
  const char* nullNames[] = {"null_2d_pixels","null_array_pixels","null_volume_pixels","null_cube_pixels"};
  for (uint32_t i = 0; i < 4; ++i) { selected = views; selected[i].resource.Reset(); draw(selected,i,0,0,{0,0,0,255},nullNames[i]); }
  check(expected.size() == pixelCases, "every_pixel_case_has_a_distinct_target_pixel");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
  const auto targetDesc = target.resource()->GetDesc(); device->GetCopyableFootprints(&targetDesc,0,1,0,&footprint,&rows,&rowBytes,&total);
  D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = total;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK; ComPtr<ID3D12Resource> readback;
  require(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
  require(drawing.keep(readback.Get()), "retain readback");
  transition(drawing.commands(),target.resource(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{},to{}; from.pResource = target.resource(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
  drawing.commands()->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  std::vector<std::weak_ptr<const void>> owners; for (const auto& v : versions) owners.push_back(v.ownership());
  views = {}; selected = {}; depthViews = {}; versions.clear(); uploads.discardTail(); pipeline = {}; pipelines.clear();
  require(images.release(d24.handle()), "release depth producer"); d24 = {};
  for (auto& image : otherDepth) { require(images.release(image.handle()), "release depth producer"); image = {}; }
  check(images.stats().depthDescriptors == 0, "depth_producer_leases_destroyed_before_render_submission");
  ComPtr<ID3D12Fence> delay; require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay)), "delay fence");
  require(queue->Wait(delay.Get(), 1), "delay GPU execution");
  require(drawing.close(),"close"); uint64_t ticket = 0; require(drawing.submit(images,ticket),"submit");
  drawRecording.reset();
  check(images.wait(ticket, 0) == HRESULT_FROM_WIN32(WAIT_TIMEOUT), "render_queue_is_actually_delayed");
  bool alive = !owners.empty(); for (const auto& owner : owners) alive &= !owner.expired();
  check(alive && images.stats().pending == 1, "texture_versions_retained_after_producer_and_recording_destruction");
  if (!alive) { std::printf("[NATIVE_TEXTURE_VIEWS_TEST] ABORT dangling queued texture before GPU execution\n"); std::exit(1); }
  require(delay->Signal(1), "release delayed GPU");
  require(images.wait(ticket,10000),"complete"); require(images.collect(),"collect");
  uint8_t* data = nullptr; D3D12_RANGE readRange{0,size_t(total)}; require(readback->Map(0,&readRange,reinterpret_cast<void**>(&data)),"map");
  for (size_t i = 0; i < expected.size(); ++i) {
    const auto* actual = data + footprint.Offset + i * 4;
    const bool same = std::memcmp(actual,expected[i].data(),4) == 0; check(same,labels[i]);
    if (!same) std::printf("pixel=%zu actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",i,actual[0],actual[1],actual[2],actual[3],expected[i][0],expected[i][1],expected[i][2],expected[i][3]);
  }
  D3D12_RANGE noWrites{0,0}; readback->Unmap(0,&noWrites);
  bool retired = true; for (const auto& owner : owners) retired &= owner.expired(); check(retired,"texture_versions_retire_after_checked_completion");
  require(images.release(target.handle()),"release target");
  require(images.shutdown(),"shutdown");
  if (validation) {
    bool clean = true;
    for (UINT64 i = 0; i < validation->GetNumStoredMessages(); ++i) {
      SIZE_T size = 0; require(validation->GetMessage(i, nullptr, &size), "validation message size");
      std::vector<uint8_t> bytes(size); auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      require(validation->GetMessage(i, message, &size), "validation message");
      if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
        clean = false; std::fprintf(stderr, "D3D12 ERROR %s\n", message->pDescription);
      }
    }
    check(clean, "debug_layer_reports_no_error_or_corruption");
  }
  std::printf("[NATIVE_TEXTURE_VIEWS_TEST] RESULT checks=%u passed=%u failed=%u\n",passed+failed,passed,failed);
  return failed ? 1 : 0;
}
