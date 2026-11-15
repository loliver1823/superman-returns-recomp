#include "gfx/native_pipeline.hpp"
#include "gfx/native_image_store.hpp"
#include <d3dcompiler.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <functional>
#include <new>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;

// I1 (step 6, Fable ruling 21): every operator new this test makes - the pipeline cache's hit path must make none
static std::atomic<uint64_t> g_news{0};
void* operator new(std::size_t n) {
  g_news.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[NATIVE_PIPELINE_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
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
  const HRESULT hr = D3DCompile(source, std::strlen(source), "native_pipeline_fixture", nullptr,
      nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
  if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", (const char*)errors->GetBufferPointer());
  require(hr, entry);
  NativeShaderCode code;
  require(NativeShaderCode::copy({(const uint8_t*)blob->GetBufferPointer(), blob->GetBufferSize()}, code),
          "own compiled shader");
  return code;
}
ComPtr<ID3D12RootSignature> rootSignature(ID3D12Device* device, UINT constantCount = 5) {
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants = {0, 0, constantCount};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC desc{1, &parameter, 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ComPtr<ID3DBlob> blob, errors;
  require(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "root serialize");
  ComPtr<ID3D12RootSignature> root;
  require(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                    IID_PPV_ARGS(&root)), "root create");
  return root;
}
NativePipelineDesc defaults(ComPtr<ID3D12RootSignature> root, NativeShaderCode vs, NativeShaderCode ps) {
  NativePipelineDesc d;
  d.rootSignature = std::move(root); d.vertex = std::move(vs); d.pixel = std::move(ps);
  d.colorCount = 1; d.colorFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
  d.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
  d.rasterizer.DepthClipEnable = TRUE;
  for (auto& rt : d.blend.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  d.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  d.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  d.depthStencil.StencilReadMask = d.depthStencil.StencilWriteMask = 255;
  d.depthStencil.FrontFace = d.depthStencil.BackFace = {
      D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  return d;
}
struct Readback {
  ComPtr<ID3D12Resource> buffer;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
};
Readback makeReadback(ID3D12Device* device, ID3D12Resource* image) {
  Readback readback;
  const auto desc = image->GetDesc();
  device->GetCopyableFootprints(&desc, 0, 1, 0, &readback.footprint, nullptr, nullptr, &readback.bytes);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = readback.bytes;
  buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback.buffer)), "readback allocation");
  return readback;
}
void copyColor(ID3D12GraphicsCommandList* list, ID3D12Resource* color, const Readback& readback) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
  list->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION from{}, to{};
  from.pResource = color; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource = readback.buffer.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = readback.footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  list->ResourceBarrier(1, &barrier);
}
bool pixelsEqual(const Readback& readback, std::array<uint8_t, 4> inside, std::array<uint8_t, 4> outside) {
  uint8_t* pixels = nullptr;
  D3D12_RANGE range{0, SIZE_T(readback.bytes)};
  require(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&pixels)), "readback map");
  bool matches = true;
  for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
    const auto& expected = x >= 2 && x < 6 && y >= 2 && y < 6 ? inside : outside;
    for (unsigned c = 0; c < 4; ++c)
      matches &= pixels[readback.footprint.Offset + y * readback.footprint.Footprint.RowPitch + x * 4 + c] == expected[c];
  }
  D3D12_RANGE noWrites{0, 0}; readback.buffer->Unmap(0, &noWrites);
  return matches;
}
}

int main() {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "native device");
  D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  require(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "native queue");
  static constexpr char shader[] = R"(
    cbuffer DrawData : register(b0) { float4 rgba; float depthValue; };
    float4 VS(uint id : SV_VertexID) : SV_Position {
      float2 p = float2((id << 1) & 2, id & 2);
      return float4(p * float2(2, -2) + float2(-1, 1), depthValue, 1);
    }
    float4 PS() : SV_Target0 { return rgba; }
    float4 PS2() : SV_Target0 { return rgba.bgra; }
  )";
  auto vs = compile(shader, "VS", "vs_5_1"), ps = compile(shader, "PS", "ps_5_1");
  auto base = defaults(rootSignature(device.Get()), vs, ps);
  NativePipelineCache cache;
  NativePipelineLease lease, output;
  check(cache.acquire(base, output) == E_ABORT && !output, "uninitialized_cache_rejected");
  check(cache.initialize(nullptr) == E_INVALIDARG, "null_device_rejected");
  check(cache.initialize(device.Get(), 0) == E_INVALIDARG, "zero_capacity_rejected");
  require(cache.initialize(device.Get(), 128), "pipeline cache");
  check(cache.initialize(device.Get(), 128) == S_FALSE, "same_initialization_idempotent");
  check(cache.initialize(device.Get(), 129) == E_UNEXPECTED, "changed_capacity_rejected");
  check(cache.acquire(base, lease) == S_OK && lease && cache.size() == 1, "actual_native_pipeline_created");
  check(cache.acquire(base, output) == S_FALSE && output.pipeline() == lease.pipeline(), "complete_descriptor_cache_hit");

  auto clone = base;
  require(NativeShaderCode::copy(vs.bytes(), clone.vertex), "duplicate shader owner");
  check(clone.vertex == base.vertex && cache.acquire(clone, output) == S_FALSE &&
        output.pipeline() == lease.pipeline(), "equal_bytecode_distinct_owners_cache_hit");
  // I1: shader copies are interned - equal bytes share one code (the hit path compares pointers, never bytes)
  {
    const uint64_t i0 = NativeShaderCode::internedCopies();
    NativeShaderCode a, b;
    require(NativeShaderCode::copy(vs.bytes(), a), "interned copy a");
    require(NativeShaderCode::copy(vs.bytes(), b), "interned copy b");
    check(a.identity() == vs.identity() && b.identity() == vs.identity() && a == vs &&
              NativeShaderCode::internedCopies() == i0 + 2,
          "i1_equal_bytes_share_one_interned_code");
    const auto ps2 = compile(shader, "PS2", "ps_5_1");
    check(ps2.identity() != ps.identity() && !(ps2 == ps) && NativeShaderCode::internedCopies() == i0 + 2,
          "i1_distinct_bytes_are_distinct_codes");
    // a code nobody holds expires: its bytes are copied afresh (never resurrected), still equal and complete
    const std::array<uint8_t, 5> transient{7, 7, 7, 7, 7};
    {
      NativeShaderCode t;
      require(NativeShaderCode::copy(transient, t), "transient code");
    }
    NativeShaderCode again;
    require(NativeShaderCode::copy(transient, again), "transient code again");
    check(again && again.bytes().size() == 5 && again.bytes()[4] == 7 && NativeShaderCode::internedCopies() == i0 + 2,
          "i1_an_expired_code_is_copied_afresh");
  }
  // I1: a hit borrows its key and (same root as the last validated) skips the COM checks: it allocates nothing
  {
    NativePipelineLease l;
    require(cache.acquire(base, l) == S_FALSE ? S_OK : E_FAIL, "hit before the allocation count");
    const auto c0 = cache.counters();
    const uint64_t news0 = g_news.load();
    bool hits = true;
    for (int i = 0; i < 1000; ++i) hits &= cache.acquire(base, l) == S_FALSE && l.pipeline() == lease.pipeline();
    const uint64_t news = g_news.load() - news0;
    const auto c1 = cache.counters();
    std::printf("[NATIVE_PIPELINE_TEST] i1_hit_allocations=%llu per 1000 hits\n", (unsigned long long)news);
    check(hits && news == 0, "i1_cache_hit_allocates_nothing");
    check(c1.hits == c0.hits + 1000 && c1.borrowedKeyLookups == c0.borrowedKeyLookups + 1000 &&
              c1.rootMemoHits == c0.rootMemoHits + 1000,
          "i1_counters_count_borrowed_lookups_and_root_memo_hits");
  }
  NativeShaderCode preserved = vs;
  check(NativeShaderCode::copy({}, preserved) == E_INVALIDARG && preserved == vs, "empty_bytecode_preserves_output");
  std::array<uint8_t, 4> source{1, 2, 3, 4};
  NativeShaderCode owned;
  require(NativeShaderCode::copy(source, owned), "source bytecode copy");
  source.fill(9);
  check(owned.bytes()[0] == 1 && owned.bytes()[3] == 4, "shader_source_mutation_cannot_change_owned_bytes");
  check(!(owned == vs), "different_shader_bytes_are_distinct");

  const auto distinct = [&](NativePipelineDesc changed, const char* name) {
    NativePipelineLease separate;
    const auto before = cache.size();
    check(cache.acquire(changed, separate) == S_OK && separate.pipeline() != lease.pipeline() &&
          cache.size() == before + 1 && cache.acquire(changed, output) == S_FALSE &&
          output.pipeline() == separate.pipeline(), name);
  };
  clone = base; clone.rasterizer.CullMode = D3D12_CULL_MODE_FRONT; distinct(clone, "cull_state_changes_pipeline");
  clone = base; clone.rasterizer.DepthBias = 3; distinct(clone, "signed_depth_bias_changes_pipeline");
  clone = base; clone.rasterizer.SlopeScaledDepthBias = 0.25f; distinct(clone, "float_raster_state_changes_pipeline");
  clone = base; clone.blend.RenderTarget[0].RenderTargetWriteMask = 3; distinct(clone, "color_write_mask_changes_pipeline");
  clone = base; clone.blend.RenderTarget[0].BlendEnable = TRUE;
  clone.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  clone.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA; distinct(clone, "blend_factors_change_pipeline");
  clone = base; clone.sampleMask = 0; distinct(clone, "sample_mask_changes_pipeline");
  clone = base; clone.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; distinct(clone, "primitive_type_changes_pipeline");
  clone = base; clone.stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF; distinct(clone, "strip_cut_changes_pipeline");
  clone = base; clone.pixel = compile(shader, "PS2", "ps_5_1"); distinct(clone, "pixel_bytecode_changes_pipeline");
  // The runtime may intern equivalent root signatures. Use a different valid
  // signature, still covering the shader's five constants, to test identity.
  clone = base; clone.rootSignature = rootSignature(device.Get(), 6); distinct(clone, "different_root_object_changes_pipeline");
  clone = base; clone.inputLayout.push_back({"UNUSED", 0, DXGI_FORMAT_R32G32_FLOAT});
  distinct(clone, "owned_input_layout_changes_pipeline");
  clone = base; clone.colorCount = 2; clone.colorFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
  clone.blend.IndependentBlendEnable = TRUE; clone.blend.RenderTarget[0].RenderTargetWriteMask = 0;
  distinct(clone, "sparse_attachment_formats_and_mask_are_preserved");

  const auto rejects = [&](NativePipelineDesc invalid, const char* name) {
    output = lease; const auto before = cache.size();
    check(cache.acquire(invalid, output) == E_INVALIDARG && output.pipeline() == lease.pipeline() &&
          cache.size() == before, name);
  };
  clone = base; clone.rootSignature.Reset(); rejects(clone, "missing_root_preserves_output");
  clone = base; clone.vertex = {}; rejects(clone, "missing_vertex_code_preserves_output");
  clone = base; clone.colorCount = 9; rejects(clone, "attachment_overflow_rejected");
  clone = base; clone.samples.Count = 3; rejects(clone, "non_power_of_two_samples_rejected");
  clone = base; clone.rasterizer.DepthBiasClamp = std::numeric_limits<float>::quiet_NaN(); rejects(clone, "nonfinite_raster_state_rejected");
  clone = base; clone.inputLayout.push_back({std::string("A\0B", 3), 0, DXGI_FORMAT_R32_FLOAT}); rejects(clone, "embedded_nul_semantic_rejected");
  ComPtr<ID3D12Device> otherDevice;
  // D3D12CreateDevice returns one device per adapter in a process:
  // https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice
  // A second WARP call cannot exercise the foreign-device boundary. This one
  // fixture requires a hardware adapter; absence is an explicit setup failure.
  for (UINT index = 0; !otherDevice; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
    if (enumerated == DXGI_ERROR_NOT_FOUND) break;
    require(enumerated, "hardware adapter enumeration");
    DXGI_ADAPTER_DESC1 desc{};
    require(adapter->GetDesc1(&desc), "hardware adapter description");
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&otherDevice))))
      std::printf("[NATIVE_PIPELINE_TEST] foreign_adapter_vendor=%04X device=%04X\n", desc.VendorId, desc.DeviceId);
  }
  require(otherDevice ? S_OK : DXGI_ERROR_NOT_FOUND, "distinct hardware device required");
  ComPtr<IUnknown> localIdentity, foreignIdentity;
  require(device.As(&localIdentity), "local device identity");
  require(otherDevice.As(&foreignIdentity), "foreign device identity");
  require(localIdentity.Get() != foreignIdentity.Get() ? S_OK : E_UNEXPECTED, "devices must be distinct");
  clone = base; clone.rootSignature = rootSignature(otherDevice.Get()); rejects(clone, "foreign_device_root_rejected");
  // I1: the root memo admits only the root that passed the device check: a foreign root is checked (and rejected)
  // every time, and never replaces the memo
  {
    NativePipelineLease l;
    require(cache.acquire(base, l) == S_FALSE ? S_OK : E_FAIL, "memo set to the local root");
    const auto c0 = cache.counters();
    auto foreign = base;
    foreign.rootSignature = rootSignature(otherDevice.Get());
    const bool rejected = cache.acquire(foreign, l) == E_INVALIDARG && cache.acquire(foreign, l) == E_INVALIDARG;
    const auto c1 = cache.counters();
    const bool kept = cache.acquire(base, l) == S_FALSE && cache.counters().rootMemoHits == c1.rootMemoHits + 1;
    check(rejected && c1.rootMemoHits == c0.rootMemoHits && kept, "i1_root_memo_never_admits_a_foreign_root");
    // another valid root replaces the memo (its own check passed); the first is checked again on its return
    auto local = base;
    local.rootSignature = rootSignature(device.Get(), 7);
    const auto c2 = cache.counters();
    const bool other = SUCCEEDED(cache.acquire(local, l)) && cache.acquire(local, l) == S_FALSE &&
                       cache.acquire(base, l) == S_FALSE;
    check(other && cache.counters().rootMemoHits == c2.rootMemoHits + 1, "i1_root_memo_follows_the_last_valid_root");
  }
  clone = base; clone.vertex = owned; output = lease;
  const auto beforeFailure = cache.size();
  const HRESULT badShader = cache.acquire(clone, output);
  std::printf("[NATIVE_PIPELINE_TEST] invalid_shader_hresult=%08lX\n", (unsigned long)badShader);
  check(FAILED(badShader) && output.pipeline() == lease.pipeline(), "driver_rejects_invalid_owned_shader_without_fallback");
  check(cache.acquire(clone, output) == badShader && cache.size() == beforeFailure &&
        output.pipeline() == lease.pipeline(), "failed_creation_does_not_poison_cache_or_replace_output");
  NativePipelineCache limited;
  require(limited.initialize(device.Get(), 1), "limited cache");
  NativePipelineLease limitedLease;
  require(limited.acquire(base, limitedLease), "limited first pipeline");
  clone = base; clone.sampleMask = 0;
  check(limited.acquire(clone, limitedLease) == E_OUTOFMEMORY &&
        limited.acquire(base, output) == S_FALSE && output.pipeline() == limitedLease.pipeline(),
        "capacity_exhaustion_preserves_lease_and_existing_hits");

  // Probe depth and stencil through color readback after each pass. This checks
  // actual GPU operations, without depending on packed depth-plane byte layouts.
  NativeImageStore images;
  require(images.initialize(device.Get(), queue.Get(), 1, 1), "images");
  NativeImageDesc colorDesc;
  colorDesc.width = colorDesc.height = 8;
  colorDesc.resourceFormat = colorDesc.clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  colorDesc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
  NativeImageLease color, depth;
  require(images.create(colorDesc, color), "color");
  NativeImageDesc depthDesc;
  depthDesc.width = depthDesc.height = 8; depthDesc.kind = NativeImageKind::DepthStencil;
  depthDesc.resourceFormat = DXGI_FORMAT_R24G8_TYPELESS;
  depthDesc.clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; depthDesc.clear.DepthStencil = {1.0f, 0};
  depthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  depthDesc.depthClearFlags = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
  require(images.create(depthDesc, depth), "depth");
  auto depthPipeline = base;
  depthPipeline.depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
  depthPipeline.depthStencil.DepthEnable = TRUE;
  depthPipeline.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  depthPipeline.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  depthPipeline.depthStencil.StencilEnable = TRUE;
  depthPipeline.depthStencil.FrontFace.StencilPassOp = depthPipeline.depthStencil.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
  NativePipelineLease zPipeline, stencilPipeline;
  require(cache.acquire(depthPipeline, zPipeline), "depth pipeline");
  auto stencilDesc = depthPipeline;
  stencilDesc.depthStencil.DepthEnable = FALSE;
  stencilDesc.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  stencilDesc.depthStencil.FrontFace.StencilFunc = stencilDesc.depthStencil.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
  stencilDesc.depthStencil.FrontFace.StencilPassOp = stencilDesc.depthStencil.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
  require(cache.acquire(stencilDesc, stencilPipeline), "stencil pipeline");
  std::array<Readback, 3> readbacks{makeReadback(device.Get(), color.resource()),
      makeReadback(device.Get(), color.resource()), makeReadback(device.Get(), color.resource())};
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&commands)), "commands");
  require(images.recordClear(commands.Get(), color), "color clear");
  require(images.recordClear(commands.Get(), depth), "depth stencil clear");
  const auto rtv = color.view(), dsv = depth.view();
  commands->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  commands->SetGraphicsRootSignature(zPipeline.description()->rootSignature.Get());
  commands->SetPipelineState(zPipeline.pipeline());
  const D3D12_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
  const D3D12_RECT clipped{2, 2, 6, 6}, full{0, 0, 8, 8};
  commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &clipped);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands->OMSetStencilRef(37);
  const float pink[5] = {1, 0.25f, 0.5f, 1, 0.25f};
  commands->SetGraphicsRoot32BitConstants(0, 5, pink, 0);
  commands->DrawInstanced(3, 1, 0, 0);
  copyColor(commands.Get(), color.resource(), readbacks[0]);
  commands->RSSetScissorRects(1, &full); commands->OMSetStencilRef(2);
  const float blue[5] = {0, 0, 1, 1, 0.9f};
  commands->SetGraphicsRoot32BitConstants(0, 5, blue, 0);
  commands->DrawInstanced(3, 1, 0, 0);
  copyColor(commands.Get(), color.resource(), readbacks[1]);
  commands->SetPipelineState(stencilPipeline.pipeline()); commands->OMSetStencilRef(37);
  const float green[5] = {0, 1, 0, 1, 0.1f};
  commands->SetGraphicsRoot32BitConstants(0, 5, green, 0);
  commands->DrawInstanced(3, 1, 0, 0);
  copyColor(commands.Get(), color.resource(), readbacks[2]);
  require(commands->Close(), "close commands");
  // Clear the cache before submitting: explicit recording leases still own the
  // pipeline, root and bytecode throughout the real GPU execution below.
  cache.clear();
  check(cache.size() == 0 && zPipeline.pipeline() && stencilPipeline.description()->rootSignature &&
        zPipeline.description()->vertex == vs, "recording_leases_survive_cache_clear");
  uint64_t submission = 0;
  const std::array<NativeImageLease, 2> attachments{color, depth};
  require(images.submit(commands.Get(), allocator.Get(), attachments, submission), "draw submission");
  require(images.wait(submission, 10000), "draw completion");
  require(images.collect(), "draw retirement");
  check(pixelsEqual(readbacks[0], {255, 64, 128, 255}, {0, 0, 0, 0}), "actual_draw_obeys_clipped_scissor_and_color_pipeline");
  check(pixelsEqual(readbacks[1], {255, 64, 128, 255}, {0, 0, 255, 255}), "actual_depth_write_blocks_farther_draw_only_in_written_region");
  check(pixelsEqual(readbacks[2], {0, 255, 0, 255}, {0, 0, 255, 255}), "actual_stencil_replace_and_equal_restrict_later_draw");
  require(images.shutdown(), "image shutdown");

  // Step 4 2k: the observer, warm acquires, the snapshot and single flight (SR_PSO_SINGLE_FLIGHT).
  {
    static std::atomic<int> created{0}, warmSeen{0};
    static std::atomic<uint64_t> lastSequence{0};
    NativePipelineCache::setObserver(+[](const NativePipelineCreated& c) {
      if (SUCCEEDED(c.hr)) { created++; lastSequence = c.sequence; }
      if (c.warm) warmSeen++;
    });
    for (int flight = 0; flight < 2; ++flight) {
      NativePipelineCache::setSingleFlight(flight);
      NativePipelineCache fresh;
      require(fresh.initialize(device.Get(), 64), "2k cache");
      created = 0; warmSeen = 0;
      NativePipelineLease a, b, c;
      auto other = base; other.rasterizer.CullMode = D3D12_CULL_MODE_BACK;
      const bool made = fresh.acquire(base, a, NativePipelineAcquire::Warm) == S_OK &&
                        fresh.acquire(other, b) == S_OK && fresh.acquire(base, c) == S_FALSE &&
                        c.pipeline() == a.pipeline();
      const auto counts = fresh.counters();
      const auto snap = fresh.snapshot();
      check(made && created == 2 && warmSeen == 1 && lastSequence == 2,
            flight ? "2k_single_flight_observer_sees_each_create" : "2k_observer_sees_each_create");
      check(counts.creates == 2 && counts.warmCreates == 1 && counts.warmUsed == 1 && counts.hits == 1,
            flight ? "2k_single_flight_counters" : "2k_counters_warm_create_then_draw_hit");
      check(snap.size() == 2 && snap[0].sequence == 1 && snap[0].warm && snap[0].drawHit && !snap[1].warm &&
                snap[1].desc.rasterizer.CullMode == D3D12_CULL_MODE_BACK && snap[0].desc.vertex == vs,
            flight ? "2k_single_flight_snapshot" : "2k_snapshot_in_creation_order");
    }
    // eight threads ask for one new key at once: one create, the rest wait for it (or hit it)
    NativePipelineCache::setSingleFlight(1);
    NativePipelineCache shared;
    require(shared.initialize(device.Get(), 64), "2k shared cache");
    created = 0;
    auto key = base; key.rasterizer.DepthBias = 7;
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    std::atomic<ID3D12PipelineState*> first{nullptr};
    std::atomic<bool> same{true};
    for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
      NativePipelineLease l;
      if (SUCCEEDED(shared.acquire(key, l)) && l) {
        ok++;
        ID3D12PipelineState* expected = nullptr;
        if (!first.compare_exchange_strong(expected, l.pipeline()) && expected != l.pipeline()) same = false;
      }
    });
    for (auto& t : threads) t.join();
    const auto sc = shared.counters();
    check(ok == 8 && same && created == 1 && sc.creates == 1 && sc.hits + 1 == 8 && shared.size() == 1,
          "2k_single_flight_one_create_per_key_under_contention");
    // a different key while one is in flight is not serialized behind it (the lock is not held across the create)
    NativePipelineLease x, y;
    auto k2 = base; k2.rasterizer.DepthBias = 8;
    auto k3 = base; k3.rasterizer.DepthBias = 9;
    std::thread t1([&] { shared.acquire(k2, x); });
    std::thread t2([&] { shared.acquire(k3, y); });
    t1.join(); t2.join();
    check(x && y && x.pipeline() != y.pipeline() && shared.size() == 3, "2k_single_flight_distinct_keys_both_created");
    // Astra P2 2: the warm-up fills at most half the cache; a draw that finds it full evicts a warm-made entry no
    // draw asked for, so a draw never loses its pipeline to the warm-up
    for (int flight = 0; flight < 2; ++flight) {
      NativePipelineCache::setSingleFlight(flight);
      NativePipelineCache tiny;
      require(tiny.initialize(device.Get(), 4), "2k small cache");
      auto v = [&](INT bias) { auto d = base; d.rasterizer.DepthBias = 100 + bias; return d; };
      NativePipelineLease l;
      const bool warmCap = tiny.acquire(v(1), l, NativePipelineAcquire::Warm) == S_OK &&
                           tiny.acquire(v(2), l, NativePipelineAcquire::Warm) == S_OK &&
                           tiny.acquire(v(3), l, NativePipelineAcquire::Warm) == E_OUTOFMEMORY && tiny.size() == 2;
      check(warmCap, flight ? "2k_single_flight_warm_fills_at_most_half" : "2k_warm_fills_at_most_half");
      NativePipelineLease d1, d2, d3, d4;
      const bool drawsFit = tiny.acquire(v(4), d1) == S_OK && tiny.acquire(v(5), d2) == S_OK &&
                            tiny.size() == 4 && tiny.acquire(v(6), d3) == S_OK && tiny.size() == 4 &&
                            tiny.counters().warmEvicted == 1;
      check(drawsFit, flight ? "2k_single_flight_full_cache_evicts_an_unused_warm_entry_for_a_draw"
                             : "2k_full_cache_evicts_an_unused_warm_entry_for_a_draw");
      // the remaining warm entry, once a draw used it, is never evicted
      INT kept = 0;
      for (const auto& e : tiny.snapshot())
        if (e.warm) kept = e.desc.rasterizer.DepthBias - 100;
      const bool used = kept && tiny.acquire(v(kept), l) == S_FALSE;
      const bool full = tiny.acquire(v(7), d4) == E_OUTOFMEMORY;
      check(used && full, flight ? "2k_single_flight_a_drawn_warm_entry_stays" : "2k_a_drawn_warm_entry_stays");
    }
    // Astra (10:2x) #3: a warm create IN FLIGHT holds no slot a draw needs. Capacity 4: one draw entry, a warm
    // create blocked inside its compile, then three more distinct draws all succeed; the warm create finishes into
    // the full cache and is not cached (warmDropped) - and a draw that waited on that warm flight gets what the full
    // cache gives any draw (fix12: never the uncached pipeline - the backend keeps only the pointer, which the cache
    // retains; an uncached one would be released with the draw's lease).
    {
      NativePipelineCache::setSingleFlight(1);
      static std::atomic<bool> gateIn{false}, gateOpen{false};
      gateIn = false; gateOpen = false;
      NativePipelineCache::setCreateGateForTest(+[](const NativePipelineDesc& d) -> HRESULT {
        if (d.rasterizer.DepthBias != 212) return S_OK;
        gateIn = true;
        while (!gateOpen) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return S_OK;
      });
      NativePipelineCache tiny;
      require(tiny.initialize(device.Get(), 4), "2k in-flight cache");
      auto v = [&](INT bias) { auto d = base; d.rasterizer.DepthBias = 200 + bias; return d; };
      NativePipelineLease d1, d3, d4, d5, w, joined;
      const bool first = tiny.acquire(v(11), d1) == S_OK;
      HRESULT warmHr = E_PENDING, joinHr = E_PENDING;
      std::thread warmer([&] { warmHr = tiny.acquire(v(12), w, NativePipelineAcquire::Warm); });
      while (!gateIn) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      std::thread joiner([&] { joinHr = tiny.acquire(v(12), joined); });   // a draw that waits on the warm flight
      while (tiny.counters().flightWaits == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const bool drawsIn = tiny.acquire(v(13), d3) == S_OK && tiny.acquire(v(14), d4) == S_OK &&
                           tiny.acquire(v(15), d5) == S_OK && tiny.size() == 4;
      gateOpen = true;
      warmer.join();
      joiner.join();
      const auto c = tiny.counters();
      check(first && drawsIn, "2k_inflight_warm_create_never_fails_a_draw");
      check(warmHr == S_OK && w && c.warmDropped == 1 && tiny.size() == 4,
            "2k_inflight_warm_create_into_a_full_cache_is_dropped_not_cached");
      check(joinHr == E_OUTOFMEMORY && !joined,
            "2k_a_draw_that_waited_on_an_uncached_warm_flight_gets_what_the_full_cache_gives_a_draw");
      NativePipelineCache::setCreateGateForTest(nullptr);
    }
    // Astra (10:5x) #6: a draw that joins a warm flight which then FAILS creates its own pipeline (it never inherits
    // the warm-up's failure); a warm acquire joining a failed warm flight still reports the failure.
    {
      NativePipelineCache::setSingleFlight(1);
      static std::atomic<bool> in{false}, open{false};
      static std::atomic<int> calls{0};
      in = false; open = false; calls = 0;
      NativePipelineCache::setCreateGateForTest(+[](const NativePipelineDesc& d) -> HRESULT {
        if (d.rasterizer.DepthBias != 313) return S_OK;
        if (calls++ == 0) {   // the warm-up's create: held, then failed
          in = true;
          while (!open) std::this_thread::sleep_for(std::chrono::milliseconds(1));
          return E_FAIL;
        }
        return S_OK;          // the draw's own create
      });
      NativePipelineCache c;
      require(c.initialize(device.Get(), 16), "2k warm-fail cache");
      auto key = base; key.rasterizer.DepthBias = 313;
      NativePipelineLease w, d;
      HRESULT warmHr = S_OK, drawHr = E_PENDING;
      std::thread warmer([&] { warmHr = c.acquire(key, w, NativePipelineAcquire::Warm); });
      while (!in) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      std::thread drawer([&] { drawHr = c.acquire(key, d); });
      while (c.counters().flightWaits == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      open = true;
      warmer.join();
      drawer.join();
      check(warmHr == E_FAIL && !w, "2k_a_failed_warm_create_reports_its_failure_to_the_warm_up");
      check(drawHr == S_OK && d && c.counters().warmFlightRetries == 1 && c.size() == 1,
            "2k_a_draw_that_joined_a_failed_warm_flight_creates_its_own_pipeline");
      NativePipelineCache::setCreateGateForTest(nullptr);
    }
    // fix12: a draw on a warm flight lends the worker its priority, waits at most kWarmJoinWaitMs, then compiles its
    // own copy; the first finisher fills the cache, the worker keeps (and returns) its own pipeline
    {
      NativePipelineCache::setSingleFlight(1);
      static std::atomic<bool> in{false}, open{false};
      static std::atomic<int> calls{0};
      in = false; open = false; calls = 0;
      NativePipelineCache::setCreateGateForTest(+[](const NativePipelineDesc& d) -> HRESULT {
        if (d.rasterizer.DepthBias != 414) return S_OK;
        if (calls++ == 0) {   // the warm-up's compile: stuck until the test opens it
          in = true;
          while (!open) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return S_OK;
      });
      NativePipelineCache c;
      require(c.initialize(device.Get(), 16), "2k stuck-warm cache");
      auto key = base; key.rasterizer.DepthBias = 414;
      NativePipelineLease w, d;
      HRESULT warmHr = E_PENDING, drawHr = E_PENDING;
      std::thread warmer([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        warmHr = c.acquire(key, w, NativePipelineAcquire::Warm);
      });
      while (!in) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const auto t0 = std::chrono::steady_clock::now();
      std::thread drawer([&] { drawHr = c.acquire(key, d); });
      while (c.counters().flightWaits == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const int boosted = GetThreadPriority(warmer.native_handle());
      drawer.join();
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      open = true;
      warmer.join();
      check(boosted == THREAD_PRIORITY_NORMAL, "2k_a_draw_on_a_warm_flight_lends_the_worker_its_priority");
      check(drawHr == S_OK && d && ms >= 1900.0 && ms < 6000.0 && c.counters().warmFlightTimeouts == 1,
            "2k_a_draw_on_a_stuck_warm_flight_compiles_its_own_copy_after_the_bound");
      check(warmHr == S_FALSE && w && w.pipeline() == d.pipeline() && c.size() == 1 && c.counters().lostRaces == 1,
            "2k_the_draw_finished_first_fills_the_cache_and_the_worker_s_later_copy_is_released");
      NativePipelineCache::setCreateGateForTest(nullptr);
    }
    // ... and the other order: the warm create ends while the draw compiles its own - the warm one stays cached, the
    // draw's copy is released and the draw gets the cached (warm-made, now drawn) pipeline
    {
      NativePipelineCache::setSingleFlight(1);
      static std::atomic<bool> warmIn{false}, warmOpen{false}, drawIn{false}, drawOpen{false};
      static std::atomic<int> calls{0};
      warmIn = warmOpen = drawIn = drawOpen = false;
      calls = 0;
      NativePipelineCache::setCreateGateForTest(+[](const NativePipelineDesc& d) -> HRESULT {
        if (d.rasterizer.DepthBias != 415) return S_OK;
        if (calls++ == 0) {
          warmIn = true;
          while (!warmOpen) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
          drawIn = true;
          while (!drawOpen) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return S_OK;
      });
      NativePipelineCache c;
      require(c.initialize(device.Get(), 16), "2k warm-first cache");
      auto key = base; key.rasterizer.DepthBias = 415;
      NativePipelineLease w, d;
      HRESULT warmHr = E_PENDING, drawHr = E_PENDING;
      std::thread warmer([&] { warmHr = c.acquire(key, w, NativePipelineAcquire::Warm); });
      while (!warmIn) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      std::thread drawer([&] { drawHr = c.acquire(key, d); });
      while (!drawIn) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // after the bound
      warmOpen = true;
      warmer.join();
      drawOpen = true;
      drawer.join();
      check(warmHr == S_OK && drawHr == S_FALSE && w && d && w.pipeline() == d.pipeline() && c.size() == 1 &&
                c.counters().lostRaces == 1 && c.counters().warmUsed == 1,
            "2k_the_warm_create_finished_first_stays_cached_and_the_draw_s_copy_is_released");
      NativePipelineCache::setCreateGateForTest(nullptr);
    }
    // Step 6 (2g P2, Fable ruling 14): a pipeline created but not stored (the insert's bad_alloc, forced here) is a
    // failure - E_OUTOFMEMORY and no lease, for its creator and for every waiter on its flight - never a pipeline the
    // cache does not retain (the backend keeps only the raw pointer). The next asker creates it again and stores it.
    {
      static std::atomic<int> failedSeen{0};
      NativePipelineCache::setObserver(+[](const NativePipelineCreated& c) {
        if (c.hr == E_OUTOFMEMORY && c.sequence == 0) failedSeen++;
      });
      for (int flight = 0; flight < 2; ++flight) {   // the create under the lock, and single flight
        NativePipelineCache::setSingleFlight(flight);
        NativePipelineCache c;
        require(c.initialize(device.Get(), 16), "2g insert-fail cache");
        auto key = base; key.rasterizer.DepthBias = 516 + flight;
        failedSeen = 0;
        NativePipelineLease l;
        NativePipelineCache::failInsertsForTest(1);
        const HRESULT hr = c.acquire(key, l);
        const auto f = c.counters();
        check(hr == E_OUTOFMEMORY && !l && c.size() == 0 && f.creates == 1 && f.insertFailures == 1 &&
                  f.failures == 1 && failedSeen == 1,
              flight ? "2g_single_flight_a_pipeline_the_cache_cannot_store_fails_the_draw_and_is_not_handed_out"
                     : "2g_a_pipeline_the_cache_cannot_store_fails_the_draw_and_is_not_handed_out");
        NativePipelineLease again, hit;
        const bool stored = c.acquire(key, again) == S_OK && again && c.size() == 1 &&
                            c.acquire(key, hit) == S_FALSE && hit.pipeline() == again.pipeline();
        check(stored, flight ? "2g_single_flight_after_a_failed_insert_the_next_asker_creates_and_stores_it"
                             : "2g_after_a_failed_insert_the_next_asker_creates_and_stores_it");
      }
      // the waiters: a draw's create held on a latch, a second draw and a warm acquire waiting on that flight; the
      // insert fails - all three get E_OUTOFMEMORY and no lease (the waiters read the flight's result only)
      {
        NativePipelineCache::setSingleFlight(1);
        static std::atomic<bool> in{false}, open{false};
        in = false; open = false;
        NativePipelineCache::setCreateGateForTest(+[](const NativePipelineDesc& d) -> HRESULT {
          if (d.rasterizer.DepthBias != 518) return S_OK;
          in = true;
          while (!open) std::this_thread::sleep_for(std::chrono::milliseconds(1));
          return S_OK;
        });
        NativePipelineCache c;
        require(c.initialize(device.Get(), 16), "2g insert-fail waiters cache");
        auto key = base; key.rasterizer.DepthBias = 518;
        NativePipelineLease a, b, w;
        HRESULT ha = E_PENDING, hb = E_PENDING, hw = E_PENDING;
        NativePipelineCache::failInsertsForTest(1);
        std::thread creator([&] { ha = c.acquire(key, a); });
        while (!in) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::thread waiter([&] { hb = c.acquire(key, b); });
        std::thread warmWaiter([&] { hw = c.acquire(key, w, NativePipelineAcquire::Warm); });
        while (c.counters().flightWaits < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        open = true;
        creator.join();
        waiter.join();
        warmWaiter.join();
        const auto f = c.counters();
        check(ha == E_OUTOFMEMORY && hb == E_OUTOFMEMORY && hw == E_OUTOFMEMORY && !a && !b && !w &&
                  c.size() == 0 && f.creates == 1 && f.insertFailures == 1,
              "2g_single_flight_every_waiter_on_an_unstored_pipeline_s_flight_gets_the_failure_and_no_lease");
        NativePipelineLease again;
        check(c.acquire(key, again) == S_OK && again && c.size() == 1,
              "2g_single_flight_after_the_failed_flight_the_next_asker_creates_and_stores_it");
        NativePipelineCache::setCreateGateForTest(nullptr);
      }
      // a warm create the cache cannot store never fails a draw that joined its flight: the draw creates its own
      {
        NativePipelineCache::setSingleFlight(1);
        static std::atomic<bool> in{false}, open{false};
        static std::atomic<int> calls{0};
        in = false; open = false; calls = 0;
        NativePipelineCache::setCreateGateForTest(+[](const NativePipelineDesc& d) -> HRESULT {
          if (d.rasterizer.DepthBias != 519) return S_OK;
          if (calls++ == 0) {   // the warm-up's create: held until the draw waits on it
            in = true;
            while (!open) std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          return S_OK;
        });
        NativePipelineCache c;
        require(c.initialize(device.Get(), 16), "2g warm insert-fail cache");
        auto key = base; key.rasterizer.DepthBias = 519;
        NativePipelineLease w, d, hit;
        HRESULT warmHr = E_PENDING, drawHr = E_PENDING;
        NativePipelineCache::failInsertsForTest(1);   // the warm create's insert
        std::thread warmer([&] { warmHr = c.acquire(key, w, NativePipelineAcquire::Warm); });
        while (!in) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::thread drawer([&] { drawHr = c.acquire(key, d); });
        while (c.counters().flightWaits == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        open = true;
        warmer.join();
        drawer.join();
        const auto f = c.counters();
        check(warmHr == E_OUTOFMEMORY && !w && drawHr == S_OK && d && c.size() == 1 && f.insertFailures == 1 &&
                  f.warmFlightRetries == 1 && c.acquire(key, hit) == S_FALSE && hit.pipeline() == d.pipeline(),
              "2g_a_warm_create_the_cache_cannot_store_never_fails_a_draw_that_joined_it");
        NativePipelineCache::setCreateGateForTest(nullptr);
      }
      NativePipelineCache::failInsertsForTest(0);
    }
    NativePipelineCache::setSingleFlight(-1);
    NativePipelineCache::setObserver(nullptr);
  }

  // I1 (Fable ruling 21 (2)): the MUTANT test. Every key component - the root, each shader, every render-state, format
  // and sample word, every input-element field and semantic byte - is flipped ALONE (the components' diff proves it):
  // (a) the real key must miss (a create is attempted; the gate refuses it), and (b) with that component left out of
  // the key - the mutant - the same flip must HIT, so this test sees that component's absence. Any surviving mutant
  // fails I1. The semantic sizes are framing (no flip changes one alone): their mutants are killed by layout pairs
  // whose words agree once the size words are gone.
  {
    NativePipelineCache::setSingleFlight(0);
    using Components = std::vector<NativePipelineCache::KeyComponentForTest>;
    auto key0 = base;   // two input elements: the per-element components exist at indices 0 and 1
    key0.inputLayout.push_back({"UNUSED", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0});
    key0.inputLayout.push_back({"EXTRA", 0, DXGI_FORMAT_R32_FLOAT, 1, 0});
    const auto otherRoot = rootSignature(device.Get(), 6);
    using Mutate = std::function<void(NativePipelineDesc&, uint32_t)>;
    const auto rt = [](NativePipelineDesc& d, uint32_t i) -> D3D12_RENDER_TARGET_BLEND_DESC& {
      return d.blend.RenderTarget[i];
    };
    const auto face = [](NativePipelineDesc& d, uint32_t i) -> D3D12_DEPTH_STENCILOP_DESC& {
      return i ? d.depthStencil.BackFace : d.depthStencil.FrontFace;
    };
    const auto flip = [](BOOL& v) { v = !v; };
    const std::vector<std::pair<const char*, Mutate>> mutators = {
        {"root", [&](NativePipelineDesc& d, uint32_t) { d.rootSignature = otherRoot; }},
        {"shader", [&](NativePipelineDesc& d, uint32_t i) {
           NativeShaderCode* stages[5] = {&d.vertex, &d.pixel, &d.geometry, &d.hull, &d.domain};
           *stages[i] = owned;
         }},
        {"colorCount", [](NativePipelineDesc& d, uint32_t) { d.colorCount = d.colorCount == 1 ? 2 : 1; }},
        {"colorFormat", [](NativePipelineDesc& d, uint32_t i) {
           d.colorFormats[i] = d.colorFormats[i] == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                                                   : DXGI_FORMAT_R16G16B16A16_FLOAT;
         }},
        {"depthFormat", [](NativePipelineDesc& d, uint32_t) { d.depthFormat = DXGI_FORMAT_D32_FLOAT; }},
        {"sampleCount", [](NativePipelineDesc& d, uint32_t) { d.samples.Count = 4; }},
        {"sampleQuality", [](NativePipelineDesc& d, uint32_t) { d.samples.Quality = 1; }},
        {"sampleMask", [](NativePipelineDesc& d, uint32_t) { d.sampleMask ^= 1u; }},
        {"nodeMask", [](NativePipelineDesc& d, uint32_t) { d.nodeMask ^= 1u; }},
        {"topology", [](NativePipelineDesc& d, uint32_t) { d.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; }},
        {"stripCut", [](NativePipelineDesc& d, uint32_t) { d.stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF; }},
        {"flags", [](NativePipelineDesc& d, uint32_t) { d.flags = D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG; }},
        {"fillMode", [](NativePipelineDesc& d, uint32_t) { d.rasterizer.FillMode = D3D12_FILL_MODE_WIREFRAME; }},
        {"cullMode", [](NativePipelineDesc& d, uint32_t) { d.rasterizer.CullMode = D3D12_CULL_MODE_BACK; }},
        {"frontCounterClockwise", [&](NativePipelineDesc& d, uint32_t) { flip(d.rasterizer.FrontCounterClockwise); }},
        {"depthBias", [](NativePipelineDesc& d, uint32_t) { d.rasterizer.DepthBias += 3; }},
        {"depthBiasClamp", [](NativePipelineDesc& d, uint32_t) { d.rasterizer.DepthBiasClamp += 0.5f; }},
        {"slopeScaledDepthBias", [](NativePipelineDesc& d, uint32_t) { d.rasterizer.SlopeScaledDepthBias += 0.25f; }},
        {"depthClipEnable", [&](NativePipelineDesc& d, uint32_t) { flip(d.rasterizer.DepthClipEnable); }},
        {"multisampleEnable", [&](NativePipelineDesc& d, uint32_t) { flip(d.rasterizer.MultisampleEnable); }},
        {"antialiasedLineEnable", [&](NativePipelineDesc& d, uint32_t) { flip(d.rasterizer.AntialiasedLineEnable); }},
        {"forcedSampleCount", [](NativePipelineDesc& d, uint32_t) { d.rasterizer.ForcedSampleCount ^= 1u; }},
        {"conservativeRaster", [](NativePipelineDesc& d, uint32_t) {
           d.rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
         }},
        {"alphaToCoverage", [&](NativePipelineDesc& d, uint32_t) { flip(d.blend.AlphaToCoverageEnable); }},
        {"independentBlend", [&](NativePipelineDesc& d, uint32_t) { flip(d.blend.IndependentBlendEnable); }},
        {"blendEnable", [&](NativePipelineDesc& d, uint32_t i) { flip(rt(d, i).BlendEnable); }},
        {"logicOpEnable", [&](NativePipelineDesc& d, uint32_t i) { flip(rt(d, i).LogicOpEnable); }},
        {"srcBlend", [&](NativePipelineDesc& d, uint32_t i) { rt(d, i).SrcBlend = D3D12_BLEND_SRC_ALPHA; }},
        {"destBlend", [&](NativePipelineDesc& d, uint32_t i) { rt(d, i).DestBlend = D3D12_BLEND_INV_SRC_ALPHA; }},
        {"blendOp", [&](NativePipelineDesc& d, uint32_t i) { rt(d, i).BlendOp = D3D12_BLEND_OP_SUBTRACT; }},
        {"srcBlendAlpha", [&](NativePipelineDesc& d, uint32_t i) { rt(d, i).SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA; }},
        {"destBlendAlpha", [&](NativePipelineDesc& d, uint32_t i) {
           rt(d, i).DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
         }},
        {"blendOpAlpha", [&](NativePipelineDesc& d, uint32_t i) { rt(d, i).BlendOpAlpha = D3D12_BLEND_OP_SUBTRACT; }},
        {"logicOp", [&](NativePipelineDesc& d, uint32_t i) { rt(d, i).LogicOp = D3D12_LOGIC_OP_CLEAR; }},
        {"renderTargetWriteMask", [&](NativePipelineDesc& d, uint32_t i) { rt(d, i).RenderTargetWriteMask ^= 1u; }},
        {"depthEnable", [&](NativePipelineDesc& d, uint32_t) { flip(d.depthStencil.DepthEnable); }},
        {"depthWriteMask", [](NativePipelineDesc& d, uint32_t) {
           d.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
         }},
        {"depthFunc", [](NativePipelineDesc& d, uint32_t) { d.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS; }},
        {"stencilEnable", [&](NativePipelineDesc& d, uint32_t) { flip(d.depthStencil.StencilEnable); }},
        {"stencilReadMask", [](NativePipelineDesc& d, uint32_t) { d.depthStencil.StencilReadMask ^= 1u; }},
        {"stencilWriteMask", [](NativePipelineDesc& d, uint32_t) { d.depthStencil.StencilWriteMask ^= 1u; }},
        {"stencilFailOp", [&](NativePipelineDesc& d, uint32_t i) { face(d, i).StencilFailOp = D3D12_STENCIL_OP_ZERO; }},
        {"stencilDepthFailOp", [&](NativePipelineDesc& d, uint32_t i) {
           face(d, i).StencilDepthFailOp = D3D12_STENCIL_OP_ZERO;
         }},
        {"stencilPassOp", [&](NativePipelineDesc& d, uint32_t i) { face(d, i).StencilPassOp = D3D12_STENCIL_OP_ZERO; }},
        {"stencilFunc", [&](NativePipelineDesc& d, uint32_t i) {
           face(d, i).StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
         }},
        {"semanticSize", nullptr},   // framing: the layout pairs below
        {"semanticByte", [](NativePipelineDesc& d, uint32_t i) { d.inputLayout[i / 256].semantic[i % 256] ^= 0x20; }},
        {"semanticIndex", [](NativePipelineDesc& d, uint32_t i) { d.inputLayout[i].semanticIndex += 1; }},
        {"inputFormat", [](NativePipelineDesc& d, uint32_t i) {
           d.inputLayout[i].format = DXGI_FORMAT_R32G32B32A32_FLOAT;
         }},
        {"inputSlot", [](NativePipelineDesc& d, uint32_t i) { d.inputLayout[i].inputSlot ^= 2u; }},
        {"byteOffset", [](NativePipelineDesc& d, uint32_t i) { d.inputLayout[i].byteOffset += 4; }},
        {"classification", [](NativePipelineDesc& d, uint32_t i) {
           auto& c = d.inputLayout[i].classification;
           c = c == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                               : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
         }},
        {"instanceStepRate", [](NativePipelineDesc& d, uint32_t i) { d.inputLayout[i].instanceStepRate += 1; }},
    };
    const auto mutatorFor = [&](const char* name) -> const std::pair<const char*, Mutate>* {
      for (const auto& m : mutators)
        if (!std::strcmp(m.first, name)) return &m;
      return nullptr;
    };
    // exactly one component differs, and it is (name, index)
    const auto isolated = [](const Components& a, const Components& b, const char* name, uint32_t index) {
      if (a.size() != b.size()) return false;
      size_t changed = 0;
      bool target = false;
      for (size_t k = 0; k < a.size(); ++k) {
        if (std::strcmp(a[k].name, b[k].name) || a[k].index != b[k].index) return false;
        if (a[k].value != b[k].value) {
          ++changed;
          target = !std::strcmp(a[k].name, name) && a[k].index == index;
        }
      }
      return changed == 1 && target;
    };
    static constexpr NativePipelineCache::CreateGateForTest refuse =
        +[](const NativePipelineDesc&) -> HRESULT { return E_ABORT; };
    // (a) the real key: `from` created for real, then `to` must MISS (its create reaches the gate, which refuses it)
    const auto realMisses = [&](const NativePipelineDesc& from, const NativePipelineDesc& to) {
      NativePipelineCache probe;
      NativePipelineLease a, b;
      if (FAILED(probe.initialize(device.Get(), 4)) || probe.acquire(from, a) != S_OK) return false;
      NativePipelineCache::setCreateGateForTest(refuse);
      const HRESULT hr = probe.acquire(to, b);
      NativePipelineCache::setCreateGateForTest(nullptr);
      return hr == E_ABORT && !b && probe.size() == 1;
    };
    // (b) the mutant key (name, index dropped): `from` created for real, then `to` must HIT it
    const auto mutantHits = [&](const NativePipelineDesc& from, const NativePipelineDesc& to, const char* name,
                                uint32_t index) {
      NativePipelineCache::dropKeyComponentForTest(name, index);
      bool hit = false;
      {
        NativePipelineCache fresh;
        NativePipelineLease a, b;
        if (SUCCEEDED(fresh.initialize(device.Get(), 4)) && fresh.acquire(from, a) == S_OK) {
          NativePipelineCache::setCreateGateForTest(refuse);   // a surviving mutant misses: never a real create
          hit = fresh.acquire(to, b) == S_FALSE && b.pipeline() == a.pipeline();
          NativePipelineCache::setCreateGateForTest(nullptr);
        }
      }
      NativePipelineCache::dropKeyComponentForTest(nullptr, 0);
      return hit;
    };
    unsigned total = 0, killed = 0, unmapped = 0, unisolated = 0, unmissed = 0;
    const auto run = [&](const NativePipelineDesc& from, const NativePipelineDesc& to, const char* name,
                         uint32_t index) {
      ++total;
      if (!realMisses(from, to)) {
        ++unmissed;
        std::printf("[NATIVE_PIPELINE_TEST] i1_mutant %s[%u]: the real key did not miss\n", name, index);
      } else if (mutantHits(from, to, name, index)) {
        ++killed;
      } else {
        std::printf("[NATIVE_PIPELINE_TEST] i1_mutant %s[%u] SURVIVED\n", name, index);
      }
    };
    // the framing pairs: S and M agree word for word once the size word(s) are gone. Dropping element k's size alone:
    // S = [k plain elements] {"AB", 2, R32G32_FLOAT, 0, 0, instance, 2} {"C", 0, R32_FLOAT, 1, 0, vertex, 0}
    // M = [k plain elements] {"A", 66, format 2, 16, 0, vertex, 1} {"\x01C", 0, R32_FLOAT, 1, 0, vertex, 0}
    // Dropping every size: the step rate 2 -> 68 and the second semantic "\x01C" -> "DC".
    const auto framingPair = [&](uint32_t k, bool every, NativePipelineDesc& S, NativePipelineDesc& M) {
      S = base;
      for (uint32_t j = 0; j < k; ++j)
        S.inputLayout.push_back({"PLAIN", j, DXGI_FORMAT_R32_FLOAT, 2, 4 * j});
      M = S;
      S.inputLayout.push_back({"AB", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
                               every ? 68u : 2u});
      S.inputLayout.push_back({"C", 0, DXGI_FORMAT_R32_FLOAT, 1, 0});
      M.inputLayout.push_back({"A", 66, DXGI_FORMAT(2), 16, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 1});
      M.inputLayout.push_back({every ? std::string("DC") : std::string("\x01" "C"), 0, DXGI_FORMAT_R32_FLOAT, 1, 0});
    };
    const Components components = NativePipelineCache::keyComponentsForTest(key0);
    for (const auto& c : components) {
      const auto* m = mutatorFor(c.name);
      if (!m) {
        ++unmapped;
        std::printf("[NATIVE_PIPELINE_TEST] i1_mutant %s[%u]: no mutation\n", c.name, c.index);
        continue;
      }
      if (!m->second) {   // semanticSize[k]: its framing pair
        NativePipelineDesc S, M;
        framingPair(c.index, false, S, M);
        run(S, M, c.name, c.index);
        continue;
      }
      auto mutated = key0;
      m->second(mutated, c.index);
      if (!isolated(components, NativePipelineCache::keyComponentsForTest(mutated), c.name, c.index)) {
        ++unisolated;
        std::printf("[NATIVE_PIPELINE_TEST] i1_mutant %s[%u]: the flip changed another component\n", c.name, c.index);
        continue;
      }
      run(key0, mutated, c.name, c.index);
    }
    {   // the code-level framing mutant: every semantic size left out
      NativePipelineDesc S, M;
      framingPair(0, true, S, M);
      run(S, M, "semanticSize", UINT32_MAX);
    }
    std::printf("[NATIVE_PIPELINE_TEST] i1_mutants killed=%u/%u components=%zu unmapped=%u unisolated=%u unmissed=%u\n",
                killed, total, components.size(), unmapped, unisolated, unmissed);
    check(total == components.size() + 1 && killed == total && !unmapped && !unisolated && !unmissed,
          "i1_every_key_component_mutant_is_killed");
    NativePipelineCache::setSingleFlight(-1);
  }
  std::printf("[NATIVE_PIPELINE_TEST] passed=%u failed=%u\n", passed, failed);
  return failed ? 1 : 0;
}
