// Actual WARP draws using the same translated-ABI snapshot/binder as gameplay.
// Run serially through tools/game_slot.py. No guest, PM4 or EDRAM inputs.
#include "gfx/translated_constants.hpp"
#include <d3dcompiler.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;
namespace {
unsigned passed = 0, failed = 0;
void check(bool value, const char* name) {
  std::printf("[TRANSLATED_CONSTANT_TEST] %s %s\n", value ? "PASS" : "FAIL", name);
  value ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) { std::fprintf(stderr, "SETUP FAILED %s hr=%08lX\n", name, (unsigned long)hr); std::exit(2); }
}
template<size_t N> std::span<const uint8_t> bytes(const std::array<float, N>& values) {
  return {reinterpret_cast<const uint8_t*>(values.data()), sizeof(values)};
}
struct Inputs {
  std::array<float, 1024> vs{}, ps{};
  std::array<float, 128> shared{};
  std::array<float, 192> fetch{};
  // compact: the values sit in the first 16-register block (read by the *Compact shaders),
  // so a 256-byte float extent holds them.
  void set(unsigned version, bool compact = false) {
    *this = {};
    vs[compact ? 0 : 1020] = float(version ? 96 : 32) / 255;
    vs[compact ? 4 : 1016] = 32.0f / 255;
    ps[compact ? 1 : 1021] = float(version ? 32 : 96) / 255;
    ps[compact ? 5 : 1017] = 32.0f / 255;
    shared[126] = float(version ? 64 : 192) / 255;
    fetch[191] = 1;
  }
};
HRESULT stage(NativeUploadArena& arena, const Inputs& input, bool fetch, TranslatedConstantSnapshot& output,
              size_t vsBytes = 4096, size_t psBytes = 4096) {
  return TranslatedConstantSnapshot::stage(arena, bytes(input.vs).first(vsBytes), bytes(input.ps).first(psBytes),
      bytes(input.shared), fetch ? bytes(input.fetch) : std::span<const uint8_t>{}, output);
}
NativeShaderCode compile(const char* source, const char* entry, const char* profile) {
  ComPtr<ID3DBlob> blob, errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), "translated_constant_fixture", nullptr,
      nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
  if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", (const char*)errors->GetBufferPointer());
  require(hr, entry);
  NativeShaderCode result;
  require(NativeShaderCode::copy({static_cast<const uint8_t*>(blob->GetBufferPointer()), blob->GetBufferSize()}, result), "shader copy");
  return result;
}
constexpr char shader[] = R"(
  cbuffer V0 : register(b0, space4) { float4 v0[256]; };
  cbuffer P1 : register(b1, space4) { float4 p1[256]; };
  cbuffer S2 : register(b2, space4) { float4 s2[32]; };
  cbuffer V3 : register(b3, space4) { float4 v3[256]; };
  cbuffer P4 : register(b4, space4) { float4 p4[256]; };
  cbuffer F9 : register(b5, space5) { float4 f9[48]; };
  float4 VS(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
  }
  float4 PS() : SV_Target0 {
    return float4(v0[255].x + v3[254].x, p1[255].y + p4[254].y, s2[31].z, 1);
  }
  float4 PSFetch() : SV_Target0 { float4 color = PS(); color.a = f9[47].w; return color; }
  float4 PSCompact() : SV_Target0 {
    return float4(v0[0].x + v3[1].x, p1[0].y + p4[1].y, s2[31].z, 1);
  }
  float4 PSCompactFetch() : SV_Target0 { float4 color = PSCompact(); color.a = f9[47].w; return color; }
)";
NativeRootSignature rootSignature(ID3D12Device* device) {
  std::array<D3D12_ROOT_PARAMETER, 10> params{};
  std::array<uint32_t, 10> extents{};
  for (uint32_t i = 0; i < params.size(); ++i) {
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    if (i < 5 || i == 9) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      params[i].Descriptor = {i == 9 ? 5u : i, i == 9 ? 5u : 4u};
      extents[i] = i == 2 ? 512 : i == 9 ? 768 : 4096;
    } else {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[i].Constants = {i + 32, 0, 1};
    }
  }
  D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters = UINT(params.size()); desc.pParameters = params.data();
  NativeRootSignature root;
  require(NativeRootSignature::create(device, desc, root, extents), "translated root contract");
  return root;
}
void validation(ID3D12Device* device) {
  NativeUploadArena arena; require(arena.initialize(device, 65536, 65536), "validation arena");
  Inputs input; input.set(0); TranslatedConstantSnapshot output;
  require(stage(arena, input, true, output), "valid fetched snapshot");
  check(output.storage().size() == 9472 && output.address(0) % 256 == 0 && output.address(9) % 256 == 0,
        "aligned_exact_fetched_extent");
  check(output.address(0) == output.address(3) && output.address(1) == output.address(4) &&
        output.address(2) == output.storage().address() + 8192 && output.address(9) == output.storage().address() + 8704,
        "aliases_and_shared_fetch_offsets");
  const auto before = output.storage().address();
  const auto vs = bytes(input.vs), ps = bytes(input.ps), shared = bytes(input.shared), fetch = bytes(input.fetch);
  check(TranslatedConstantSnapshot::stage(arena, vs.first(4095), ps, shared, fetch, output) == E_INVALIDARG &&
        output.storage().address() == before, "short_vs_refuses_without_replacing_output");
  check(TranslatedConstantSnapshot::stage(arena, vs, ps.first(4095), shared, fetch, output) == E_INVALIDARG &&
        output.storage().address() == before, "short_ps_refuses_without_replacing_output");
  check(TranslatedConstantSnapshot::stage(arena, vs, ps, shared.first(511), fetch, output) == E_INVALIDARG &&
        output.storage().address() == before, "short_shared_refuses_without_replacing_output");
  check(TranslatedConstantSnapshot::stage(arena, vs, ps, shared, fetch.first(767), output) == E_INVALIDARG &&
        output.storage().address() == before, "short_fetch_refuses_without_replacing_output");
  NativeUploadArena missing;
  check(stage(missing, input, true, output) == E_ABORT && output.storage().address() == before,
        "uninitialized_arena_preserves_output");
  NativeDrawRecording empty;
  check(output.bind(empty) == E_ABORT, "unopened_recording_refuses_binding");
  require(empty.begin(device), "validation recording");
  TranslatedConstantSnapshot invalid;
  check(invalid.bind(empty) == E_INVALIDARG, "empty_snapshot_refuses_binding");
  require(empty.close(), "validation close");
  check(output.bind(empty) == E_ABORT, "closed_recording_refuses_binding");
  arena.discardTail();
  const std::array<uint8_t, 65536> full{}; NativeBufferSlice other;
  check(arena.stage(full, 256, other) == E_OUTOFMEMORY && output.storage().address() == before,
        "retained_snapshot_prevents_page_budget_reuse");
  output = {}; check(arena.liveBytes() == 0, "abandoned_snapshot_releases_page");
  require(stage(arena, input, false, output), "decoded snapshot");
  check(output.storage().size() == 8704 && output.address(9) == 0 && output.address(8) == 0,
        "decoded_snapshot_has_no_fetch_binding");
  // Float extents: blocks pack at 256-byte offsets; b3/b4 still alias b0/b1.
  require(stage(arena, input, true, output, 256, 512), "compact snapshot");
  const auto base = output.storage().address();
  check(output.storage().size() == 256 + 512 + 512 + 768 && output.address(0) == base &&
        output.address(1) == base + 256 && output.address(2) == base + 768 && output.address(9) == base + 1280 &&
        output.address(3) == output.address(0) && output.address(4) == output.address(1) &&
        output.size(0) == 256 && output.size(1) == 512 && output.size(2) == 512 && output.size(9) == 768,
        "float_extents_pack_blocks_and_keep_aliases");
  const auto compact = output.storage().address();
  std::array<float, 1088> wide{};
  const auto wideBytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(wide.data()), sizeof(wide));
  check(TranslatedConstantSnapshot::stage(arena, vs.first(0), ps, shared, fetch, output) == E_INVALIDARG &&
        TranslatedConstantSnapshot::stage(arena, vs, ps.first(300), shared, fetch, output) == E_INVALIDARG &&
        TranslatedConstantSnapshot::stage(arena, wideBytes.first(4352), ps, shared, fetch, output) == E_INVALIDARG &&
        output.storage().address() == compact, "empty_unaligned_or_oversized_extent_refuses");
  // The page spans kFloatBytes from the PS block's start even when less is staged: a slice
  // whose reserve does not fit the tail goes to a new page; one whose reserve fits stays.
  NativeUploadArena paged; require(paged.initialize(device, 65536, 4 * 65536), "reserve arena");
  std::vector<uint8_t> filler(65536 - 2048, 0); NativeBufferSlice head;
  require(paged.stage(filler, 256, head), "filler leaving 2048 bytes");
  require(stage(paged, input, false, output, 256, 256), "compact snapshot past the reserve");
  check(output.storage().resource() != head.resource() && output.storage().offset() == 0,
        "unfitting_reserve_moves_snapshot_to_a_new_page");
  NativeBufferSlice second; filler.resize(65536 - 1024 - 8192);
  require(paged.stage(filler, 256, second), "filler leaving room for the reserve");
  TranslatedConstantSnapshot fitted;
  require(stage(paged, input, false, fitted, 256, 256), "compact snapshot within the reserve");
  check(fitted.storage().resource() == second.resource() &&
        fitted.storage().offset() + 256 + TranslatedConstantSnapshot::kFloatBytes <= 65536,
        "fitting_reserve_stays_on_the_page_inside_the_resource");
}
void drawProbe(ID3D12Device* device, ID3D12CommandQueue* queue, NativeImageStore& images, bool fetch,
               bool compact = false) {
  auto root = rootSignature(device);
  NativePipelineCache pipelines; require(pipelines.initialize(device), "probe pipeline cache");
  NativePipelineDesc pd; pd.rootSignature = root.root(); pd.vertex = compile(shader, "VS", "vs_5_1");
  pd.pixel = compile(shader, compact ? (fetch ? "PSCompactFetch" : "PSCompact") : (fetch ? "PSFetch" : "PS"),
                     "ps_5_1");
  pd.colorCount = 1; pd.colorFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pd.rasterizer.FillMode = D3D12_FILL_MODE_SOLID; pd.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
  pd.rasterizer.DepthClipEnable = TRUE;
  auto& blend = pd.blend.RenderTarget[0]; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO; blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pd.depthStencil.FrontFace = pd.depthStencil.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
      D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  NativePipelineLease pipeline; require(pipelines.acquire(pd, pipeline), "probe pipeline");
  NativeImageDesc image; image.width = image.height = 8; image.resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  image.clear.Format = image.resourceFormat; image.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
  NativeImageLease color; require(images.create(image, color), "probe target");
  ComPtr<ID3D12Resource> readback; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 readbackBytes;
  const auto desc = color.resource()->GetDesc();
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = readbackBytes;
  buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&readback)), "probe readback");
  ComPtr<ID3D12Fence> delay; require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay)), "GPU delay");
  require(queue->Wait(delay.Get(), 1), "queue delayed");
  std::array<std::weak_ptr<const void>, 2> owners; uint64_t ticket = 0;
  {
    NativeUploadArena arena; require(arena.initialize(device, 65536, 65536), "producer arena");
    NativeDrawRecording recording; require(recording.begin(device), "draw recording");
    require(recording.keep(color), "retain target"); require(recording.keep(readback.Get()), "retain readback");
    auto* list = recording.commands(); const auto view = color.view();
    list->OMSetRenderTargets(1, &view, FALSE, nullptr); list->SetGraphicsRootSignature(root.root());
    list->SetPipelineState(pipeline.pipeline()); const D3D12_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
    list->RSSetViewports(1, &viewport); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (unsigned version = 0; version < 2; ++version) {
      Inputs source; source.set(version, compact); TranslatedConstantSnapshot snapshot;
      require(stage(arena, source, fetch, snapshot, compact ? 256 : 4096, compact ? 256 : 4096),
              "published constants");
      owners[version] = snapshot.storage().ownership(); source = {};
      const D3D12_RECT scissor{LONG(version * 4), 0, LONG(version * 4 + 4), 8}; list->RSSetScissorRects(1, &scissor);
      require(snapshot.bind(recording), "actual snapshot binding"); list->DrawInstanced(3, 1, 0, 0);
    }
    arena.discardTail();
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {color.resource(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{}; from.pResource = color.resource();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr); require(recording.close(), "draw close");
    require(recording.submit(images, ticket), "owned draw submission");
  }
  check(images.wait(ticket, 0) == HRESULT_FROM_WIN32(WAIT_TIMEOUT), "actual_queue_still_delayed");
  const bool alive = !owners[0].expired() && !owners[1].expired();
  check(alive, "both_constant_versions_survive_producer_and_recording_destruction");
  if (!alive) { std::fprintf(stderr, "ABORT before executing dangling constants\n"); std::exit(1); }
  require(delay->Signal(1), "release GPU"); require(images.wait(ticket, 10000), "actual GPU completion");
  uint8_t* mapped; const D3D12_RANGE read{0, SIZE_T(readbackBytes)};
  require(readback->Map(0, &read, reinterpret_cast<void**>(&mapped)), "read actual pixels");
  bool exact = true;
  for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
    const std::array<uint8_t, 4> expected = x < 4 ? std::array<uint8_t, 4>{64, 128, 192, 255} :
        std::array<uint8_t, 4>{128, 64, 64, 255};
    exact &= std::memcmp(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4, expected.data(), 4) == 0;
  }
  const D3D12_RANGE noWrite{0, 0}; readback->Unmap(0, &noWrite);
  check(exact, compact ? (fetch ? "compact_extents_with_fetch_render_exact_after_source_mutation" :
                                  "compact_extents_render_exact_after_source_mutation") :
               fetch ? "fetch_aliases_and_full_extents_render_exact_after_source_mutation" :
                       "float_shared_aliases_render_exact_after_source_mutation");
  require(images.collect(), "collect completed constants");
  check(owners[0].expired() && owners[1].expired(), "both_versions_retire_after_checked_completion");
  require(images.release(color.handle()), "release target registry entry");
}
}  // namespace
int main() {
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  D3D12_COMMAND_QUEUE_DESC desc{}; ComPtr<ID3D12CommandQueue> queue;
  require(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
  NativeImageStore images; require(images.initialize(device.Get(), queue.Get(), 16, 4), "native image queue");
  validation(device.Get()); drawProbe(device.Get(), queue.Get(), images, false); drawProbe(device.Get(), queue.Get(), images, true);
  drawProbe(device.Get(), queue.Get(), images, false, true); drawProbe(device.Get(), queue.Get(), images, true, true);
  require(images.shutdown(), "checked shutdown");
  std::printf("[TRANSLATED_CONSTANT_TEST] RESULT checks=%u passed=%u failed=%u\n", passed + failed, passed, failed);
  return failed ? 1 : 0;
}
