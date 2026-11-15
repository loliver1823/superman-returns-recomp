// Bounded native D3D12/WARP proof, independent of guest/PM4/EDRAM code.
// Build sr_native_draw_test and run serially through the shared game slot.
#include "gfx/native_draw.hpp"
#include <d3dcompiler.h>
#include <array>
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
  std::printf("[NATIVE_DRAW_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) { std::fprintf(stderr, "SETUP FAILED %s hr=%08lX\n", name, (unsigned long)hr); std::exit(2); }
}
template<class T> std::span<const uint8_t> bytesOf(const T& value) {
  return {reinterpret_cast<const uint8_t*>(&value), sizeof(value)};
}
NativeBufferSlice ownUpload(ID3D12Device* device, std::span<const uint8_t> bytes,
                            uint32_t alignment = 16, bool constant = false) {
  NativeUploadArena producer; require(producer.initialize(device, 65536, 65536), "independent upload producer");
  NativeBufferSlice result;
  require(constant ? producer.stageConstantBuffer(bytes, result) : producer.stage(bytes, alignment, result), "own upload bytes");
  return result;  // The producer is destroyed; only the returned slice owns its page.
}
void ownershipProbe(ID3D12Device* device, ID3D12CommandQueue* queue, NativeImageStore& images) {
  ComPtr<ID3D12Fence> delay; require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay)), "ownership probe fence");
  require(queue->Wait(delay.Get(), 1), "ownership probe delay");
  std::weak_ptr<const void> packet; uint64_t ticket = 0;
  {
    NativeDrawRecording recording; require(recording.begin(device), "empty ownership recording");
    packet = recording.ownership(); require(recording.close(), "empty ownership close");
    require(recording.submit(images, ticket), "empty ownership submit");
  }
  check(!packet.expired(), "destroyed_recording_packet_retained_by_queue");
  require(delay->Signal(1), "release ownership probe"); require(images.wait(ticket, 10000), "ownership probe completion");
  require(images.collect(), "ownership probe collection");
  check(packet.expired(), "recording_packet_retired_after_checked_completion");
}
void uploadChecks(ID3D12Device* device) {
  NativeUploadArena arena; require(arena.initialize(device, 65536, 65536), "bounded upload budget");
  std::array<uint8_t, 17> source{}; source[0] = 7;
  NativeBufferSlice first; require(arena.stageConstantBuffer(source, first), "padded constant staging");
  source[0] = 0;
  check(first.address() % 256 == 0 && first.size() == 256 && first.bytes()[0] == 7,
        "constant_staging_owns_aligned_source_copy");
  bool zero = true; for (size_t i = 17; i < first.size(); ++i) zero &= first.bytes()[i] == 0;
  check(zero && first.hasZeroPadding(17, 256) && !first.hasZeroPadding(16, 256), "zero_padding_metadata_matches_owned_bytes");
  source[0] = 11; NativeBufferSlice adjacent; require(arena.stageConstantBuffer(source, adjacent), "adjacent immutable upload");
  check(first.address() != adjacent.address() && first.bytes()[0] == 7 && adjacent.bytes()[0] == 11,
        "later_upload_does_not_overwrite_published_bytes");
  adjacent = {};
  auto previous = first.address();
  check(arena.stageConstantBuffer({}, first) == E_INVALIDARG && first.address() == previous,
        "invalid_constant_staging_preserves_output");
  arena.discardTail();
  const std::array<uint8_t, 65536> fullPage{};
  NativeBufferSlice second;
  check(arena.stage(fullPage, 16, second) == E_OUTOFMEMORY && !second && arena.liveBytes() == 65536,
        "retained_page_blocks_budget_reuse");
  auto* retiredResource = first.resource();
  first = {};
  check(arena.liveBytes() == 0, "final_slice_returns_page_budget");
  require(arena.stage(fullPage, 16, second), "reuse retired upload budget");
  check(second.size() == fullPage.size() && arena.liveBytes() == 65536, "completed_page_budget_can_be_reused");
  check(second.resource() == retiredResource && second.bytes()[0] == 0 && second.offset() == 0,
        "retired_page_is_recycled_not_recreated");
}
// ---- step 6 R1a (J:/sr_lanec_drawthread/FREEZE_R1A_20261025.txt; SR_RENDER_KEEP_LEAN, default on) ----
// F1: a recording retains each image allocation once; interleaved recordings A / B / A retain it in both (a stale
// stamp only duplicates); the lease is held until collect after the fence, released by collect.
void imageRetentionChecks(ID3D12Device* device, ID3D12CommandQueue* queue, NativeImageStore& images) {
  const bool lean = renderKeepLean();
  NativeImageDesc desc; desc.width = desc.height = 8; desc.resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET; desc.clear.Format = desc.resourceFormat; desc.clear.Color[3] = 1;
  ComPtr<ID3D12Fence> delay; require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay)), "image retention fence");
  require(queue->Wait(delay.Get(), 1), "image retention delay");
  std::weak_ptr<const void> owner; uint64_t ticketA = 0, ticketB = 0;
  {
    NativeImageLease target; require(images.create(desc, target), "retained target");
    owner = target.ownership();
    NativeDrawRecording a, b; require(a.begin(device), "recording A"); require(b.begin(device), "recording B");
    check(a.serial() != 0 && b.serial() > a.serial(), "recording_serials_monotonic_nonzero");
    for (int i = 0; i < 5; ++i) require(a.keep(target), "A keeps the target");
    check(a.retainedImageCount() == (lean ? 1u : 5u),
          lean ? "image_retained_once_per_recording" : "image_retained_per_bind_with_lean_off");
    require(b.keep(target), "B keeps the target");          // interleaved: B stamps the allocation
    require(a.keep(target), "A keeps the target again");    // A's stamp was overwritten: a duplicate, never a miss
    check(b.retainedImageCount() == 1 && a.retainedImageCount() == (lean ? 2u : 6u), "interleaved_A_B_A_retains_in_both");
    require(images.recordClear(a.commands(), target), "A clears"); require(images.recordClear(b.commands(), target), "B clears");
    require(a.close(), "A close"); require(b.close(), "B close");
    require(a.submit(images, ticketA), "A submit"); require(b.submit(images, ticketB), "B submit");
    require(images.release(target.handle()), "retire the target handle");
  }
  const bool alive = !owner.expired();
  check(alive, "target_retained_by_its_recordings_before_execution");
  if (!alive) { std::printf("[NATIVE_DRAW_TEST] ABORT dangling target before GPU execution\n"); std::exit(1); }
  require(delay->Signal(1), "release the image retention delay");
  require(images.wait(ticketA, 10000), "A completes"); require(images.wait(ticketB, 10000), "B completes");
  check(!owner.expired(), "target_held_until_collect");
  require(images.collect(), "collect A and B");
  check(owner.expired(), "target_released_at_collect");
}
// F2b: one retained slice per upload page covers every slice staged on that page in the recording (a slice staged
// AFTER the first on the same page included); a slice on a new page is retained on its own; the pages live until
// collect after the fence, the GPU reads the later slices' bytes after every producer / slice owner is gone.
// The controls (--control=skip-page-keep / release-at-slice-death, the recording's mutants) must FAIL
// page_retained_for_a_later_slice_until_completion and abort before GPU execution.
void pageRetentionChecks(ID3D12Device* device, ID3D12CommandQueue* queue, NativeImageStore& images) {
  const bool lean = renderKeepLean();
  ComPtr<ID3D12Fence> delay; require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay)), "page retention fence");
  require(queue->Wait(delay.Get(), 1), "page retention delay");
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rb{}; rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rb.Width = 512; rb.Height = 1;
  rb.DepthOrArraySize = rb.MipLevels = 1; rb.SampleDesc.Count = 1; rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&readback)), "page retention readback");
  std::weak_ptr<const void> firstOwner, laterOwner, otherOwner; uint64_t ticket = 0;
  {
    NativeUploadArena arena; require(arena.initialize(device, 65536, 4 * 65536), "page retention arena");
    NativeDrawRecording r; require(r.begin(device), "page retention recording");
    require(r.keep(readback.Get()), "retain the readback");
    std::array<uint8_t, 256> a{}, b{}; a.fill(0x11); b.fill(0x22);
    std::vector<uint8_t> big(65536, 0x33);   // larger than the first page's remainder: a new page
    NativeBufferSlice first, later, other;
    require(arena.stage(a, 16, first), "first slice"); require(r.keep(first), "keep the first slice");
    require(arena.stage(b, 16, later), "a later slice on the same page");
    check(later.resource() == first.resource() && later.offset() > first.offset(), "later_slice_shares_the_first_page");
    require(r.keep(later), "keep the later slice");
    require(arena.stage(big, 16, other), "a slice on a new page");
    check(other.resource() != first.resource(), "a_large_slice_opens_a_new_page");
    require(r.keep(other), "keep the slice on the new page");
    check(r.retainedUploadCount() == (lean ? 2u : 3u),
          lean ? "one_retained_slice_per_page" : "one_retained_slice_per_keep_with_lean_off");
    r.commands()->CopyBufferRegion(readback.Get(), 0, later.resource(), later.offset(), 256);
    r.commands()->CopyBufferRegion(readback.Get(), 256, other.resource(), other.offset(), 256);
    firstOwner = first.ownership(); laterOwner = later.ownership(); otherOwner = other.ownership();
    require(r.close(), "page retention close"); require(r.submit(images, ticket), "page retention submit");
    first = {}; later = {}; other = {}; arena.discardTail();
  }  // the producer arena is gone too: only the recording's submission can hold the pages
  const bool alive = !firstOwner.expired() && !laterOwner.expired() && !otherOwner.expired();
  check(alive, "page_retained_for_a_later_slice_until_completion");
  if (!alive) { std::printf("[NATIVE_DRAW_TEST] ABORT dangling upload page before GPU execution\n"); std::exit(1); }
  require(delay->Signal(1), "release the page retention delay"); require(images.wait(ticket, 10000), "page retention completion");
  uint8_t* data = nullptr; const D3D12_RANGE range{0, 512};
  require(readback->Map(0, &range, reinterpret_cast<void**>(&data)), "map page retention readback");
  bool bytes = true; for (uint32_t i = 0; i < 512; ++i) bytes &= data[i] == (i < 256 ? 0x22 : 0x33);
  const D3D12_RANGE noWrites{0, 0}; readback->Unmap(0, &noWrites);
  check(bytes, "later_slice_bytes_read_after_owner_destruction");
  require(images.collect(), "page retention collect");
  check(firstOwner.expired() && laterOwner.expired() && otherOwner.expired(), "pages_released_at_collect");
}
// R1a condition 3: for one staging sequence the pages alive while recording, while only the submission holds them,
// and after collect are equal with SR_RENDER_KEEP_LEAN off and on (page lifetime is unchanged).
std::array<uint64_t, 3> pagesAliveSequence(ID3D12Device* device, ID3D12CommandQueue* queue, NativeImageStore& images,
                                           int mode) {
  renderKeepLeanForceForTest(mode);
  ComPtr<ID3D12Fence> delay; require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay)), "pages-alive fence");
  require(queue->Wait(delay.Get(), 1), "pages-alive delay");
  NativeUploadArena arena; require(arena.initialize(device, 65536, 16 * 65536), "pages-alive arena");
  std::array<uint64_t, 3> live{}; uint64_t ticket = 0;
  {
    NativeDrawRecording r; require(r.begin(device), "pages-alive recording");
    const std::vector<uint8_t> chunk(4096, 0x5a);
    for (int i = 0; i < 40; ++i) {   // 40 x 4 KiB at 256-byte alignment: three 64 KiB pages
      NativeBufferSlice slice; require(arena.stage(chunk, 256, slice), "pages-alive slice"); require(r.keep(slice), "pages-alive keep");
    }
    live[0] = arena.liveBytes();
    require(r.close(), "pages-alive close"); require(r.submit(images, ticket), "pages-alive submit");
  }
  arena.discardTail();
  live[1] = arena.liveBytes();
  require(delay->Signal(1), "release the pages-alive delay"); require(images.wait(ticket, 10000), "pages-alive completion");
  require(images.collect(), "pages-alive collect");
  live[2] = arena.liveBytes();
  renderKeepLeanForceForTest(-1);
  return live;
}
void checkRejected(ID3D12Device* device, const PreparedNativeDraw& draw, const char* name) {
  NativeDrawRecording isolated; require(isolated.begin(device), "isolated negative recording");
  check(isolated.draw(draw) == E_INVALIDARG && isolated.drawCount() == 0, name);
  require(isolated.close(), "abandon isolated negative recording");
}
NativeShaderCode compile(const char* source, const char* entry, const char* profile) {
  ComPtr<ID3DBlob> blob, errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), "native_draw_fixture", nullptr,
      nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
  if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", (const char*)errors->GetBufferPointer());
  require(hr, entry);
  NativeShaderCode code;
  require(NativeShaderCode::copy({(const uint8_t*)blob->GetBufferPointer(), blob->GetBufferSize()}, code), "shader ownership");
  return code;
}
NativePipelineDesc pipelineDesc(const NativeRootSignature& root, NativeShaderCode vs, NativeShaderCode ps) {
  NativePipelineDesc p; p.rootSignature = root.root(); p.vertex = vs; p.pixel = ps;
  p.colorCount = 1; p.colorFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  p.rasterizer.FillMode = D3D12_FILL_MODE_SOLID; p.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
  p.rasterizer.DepthClipEnable = TRUE;
  for (auto& rt : p.blend.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD; rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  p.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; p.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  p.depthStencil.StencilReadMask = p.depthStencil.StencilWriteMask = 255;
  p.depthStencil.FrontFace = p.depthStencil.BackFace = {
      D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  return p;
}
struct Readback {
  ComPtr<ID3D12Resource> resource;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
};
Readback readbackFor(ID3D12Device* device, ID3D12Resource* image) {
  Readback r; const auto desc = image->GetDesc();
  device->GetCopyableFootprints(&desc, 0, 1, 0, &r.footprint, nullptr, nullptr, &r.bytes);
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC b{}; b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = r.bytes;
  b.Height = 1; b.DepthOrArraySize = b.MipLevels = 1; b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &b,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.resource)), "readback");
  return r;
}
void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; commands->ResourceBarrier(1, &b);
}
void copyColor(NativeDrawRecording& recording, ID3D12Resource* color, const Readback& readback) {
  require(recording.keep(readback.resource.Get()), "retain readback");
  auto* list = recording.commands();
  transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{}, to{}; from.pResource = color; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource = readback.resource.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = readback.footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}
bool pixels(const Readback& readback, std::array<uint8_t, 4> insideColor = {80, 120, 160, 255}) {
  uint8_t* data = nullptr; const D3D12_RANGE range{0, SIZE_T(readback.bytes)};
  require(readback.resource->Map(0, &range, reinterpret_cast<void**>(&data)), "map result");
  bool ok = true;
  for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
    const bool inside = x >= 2 && x < 6 && y >= 2 && y < 6;
    const std::array<uint8_t, 4> expected = inside ? insideColor : std::array<uint8_t, 4>{0, 0, 0, 255};
    const auto* pixel = data + readback.footprint.Offset + y * readback.footprint.Footprint.RowPitch + x * 4;
    ok &= std::memcmp(pixel, expected.data(), 4) == 0;
  }
  const D3D12_RANGE noWrites{0, 0}; readback.resource->Unmap(0, &noWrites); return ok;
}
constexpr char shader[] = R"(
  cbuffer C0 : register(b0, space4) { float4 c0; };
  cbuffer C1 : register(b1, space4) { float4 c1; };
  cbuffer C2 : register(b2, space4) { float4 c2; };
  cbuffer C3 : register(b3, space4) { float4 c3; };
  cbuffer C4 : register(b4, space4) { float4 c4; };
  cbuffer CF : register(b5, space5) { float4 cf; };
  cbuffer Scale : register(b6, space4) { float2 pixelScale; };
  Texture2D<float4> t0 : register(t0, space0);
  Texture2D<float4> t1 : register(t0, space1);
  Texture2D<float4> t2 : register(t0, space2);
  Texture2D<float4> t6 : register(t0, space6);
  Texture2D<float4> t7 : register(t0, space7);
  Texture2D<float4> t8 : register(t0, space8);
  SamplerState sam : register(s0, space3);
  ByteAddressBuffer verts : register(t0, space5);
  float4 VSDecoded(float4 position : POSITION) : SV_Position { return position; }
  float4 VSFetch(uint id : SV_VertexID) : SV_Position { return asfloat(verts.Load4(id * 16)); }
  float4 Shade() {
    float4 textures = (t0.SampleLevel(sam, .5, 0) + 2 * t1.SampleLevel(sam, .5, 0) +
      4 * t2.SampleLevel(sam, .5, 0) + 8 * t6.SampleLevel(sam, .5, 0) + 16 * t7.SampleLevel(sam, .5, 0) +
      32 * t8.SampleLevel(sam, .5, 0)) / 6;
    float4 constants = c0 + 2 * c1 + c2 + 2 * c3 + c4 + 2 * cf;
    return float4(textures.rgb * constants.rgb * float3(pixelScale, 1), 1);
  }
  float4 PS() : SV_Target0 { return Shade(); }
  struct Mrt { float4 first : SV_Target0; float4 third : SV_Target2; };
  Mrt PSMrt() { Mrt value; value.first = Shade(); value.third = value.first.brga; return value; }
)";
}

int main(int argc, char** argv) {
  // R1a: --control=skip-page-keep | release-at-slice-death runs ONLY the page-retention checks under the recording's
  // upload-keep mutant; it must FAIL page_retained_for_a_later_slice_until_completion and abort (exit 1).
  int mutant = 0;
  if (argc == 2 && !std::strcmp(argv[1], "--control=skip-page-keep")) mutant = 1;
  else if (argc == 2 && !std::strcmp(argv[1], "--control=release-at-slice-death")) mutant = 2;
  if (argc > 2 || (argc == 2 && !mutant && std::strcmp(argv[1], "--ownership-only"))) return 2;
  std::printf("[NATIVE_DRAW_TEST] keep_lean=%s control=%s\n", renderKeepLean() ? "on" : "off",
              mutant == 1 ? "skip-page-keep" : mutant == 2 ? "release-at-slice-death" : "none");
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  require(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
  NativeImageStore images; require(images.initialize(device.Get(), queue.Get(), 16, 4), "image queue");
  if (mutant) {
    NativeDrawRecording::uploadKeepMutantForTest(mutant);
    pageRetentionChecks(device.Get(), queue.Get(), images);   // a working control aborts inside
    NativeDrawRecording::uploadKeepMutantForTest(0);
    require(images.shutdown(), "control shutdown");
    std::printf("[NATIVE_DRAW_TEST] RESULT checks=%u passed=%u failed=%u\n", passed + failed, passed, failed);
    return failed ? 1 : 0;
  }
  ownershipProbe(device.Get(), queue.Get(), images);
  if (argc == 2) {
    require(images.shutdown(), "ownership-only shutdown");
    std::printf("[NATIVE_DRAW_TEST] RESULT checks=%u passed=%u failed=%u\n", passed + failed, passed, failed);
    return failed ? 1 : 0;
  }
  uploadChecks(device.Get());
  imageRetentionChecks(device.Get(), queue.Get(), images);
  pageRetentionChecks(device.Get(), queue.Get(), images);
  {
    const auto off = pagesAliveSequence(device.Get(), queue.Get(), images, 0);
    const auto on = pagesAliveSequence(device.Get(), queue.Get(), images, 1);
    std::printf("[NATIVE_DRAW_TEST] pages_alive bytes off=%llu/%llu/%llu on=%llu/%llu/%llu\n",
                (unsigned long long)off[0], (unsigned long long)off[1], (unsigned long long)off[2],
                (unsigned long long)on[0], (unsigned long long)on[1], (unsigned long long)on[2]);
    check(off == on && on[0] == 3u * 65536u && on[1] == 3u * 65536u && on[2] == 0, "pages_alive_at_collect_equal_in_both_arms");
  }
  const auto decoded = compile(shader, "VSDecoded", "vs_5_1"), fetch = compile(shader, "VSFetch", "vs_5_1");
  const auto pixel = compile(shader, "PS", "ps_5_1");
  const auto pixelMrt = compile(shader, "PSMrt", "ps_5_1");

  // Exact 15-parameter native ABI structure, including PixelPosScale at 14.
  std::array<D3D12_ROOT_PARAMETER, 15> parameters{};
  std::array<D3D12_DESCRIPTOR_RANGE, 8> ranges{};
  std::array<uint32_t, 15> extents{};
  for (uint32_t i = 0; i < 5; ++i) {
    parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[i].Descriptor = {i, 4}; extents[i] = 16;
  }
  extents[0] = 512;  // Conservative native ABI extent; isolates short aligned-buffer admission.
  const std::array<uint32_t, 8> tableParameters{5, 6, 7, 8, 10, 11, 12, 13};
  const std::array<uint32_t, 8> spaces{0, 1, 2, 3, 5, 6, 7, 8};
  for (uint32_t i = 0; i < ranges.size(); ++i) {
    ranges[i] = {i == 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER : D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, spaces[i], 0};
    parameters[tableParameters[i]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[tableParameters[i]].DescriptorTable = {1, &ranges[i]};
  }
  parameters[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[9].Descriptor = {5, 5}; extents[9] = 16;
  parameters[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[14].Constants = {6, 4, 2};
  D3D12_ROOT_SIGNATURE_DESC rootDesc{UINT(parameters.size()), parameters.data(), 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  NativeRootSignature root;
  check(NativeRootSignature::create(device.Get(), rootDesc, root) == E_INVALIDARG, "root_buffers_need_declared_extents");
  require(NativeRootSignature::create(device.Get(), rootDesc, root, extents), "root ABI");
  const auto rootObject = root.root();
  rootDesc.NumStaticSamplers = 1;
  check(NativeRootSignature::create(device.Get(), rootDesc, root, extents) == E_INVALIDARG && root.root() == rootObject,
        "null_static_sampler_array_rejected");
  rootDesc.NumStaticSamplers = 0;
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  check(NativeRootSignature::create(device.Get(), rootDesc, root, extents) == E_INVALIDARG && root.root() == rootObject,
        "unsupported_table_preserves_root");
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = UINT32_MAX;
  check(NativeRootSignature::create(device.Get(), rootDesc, root, extents) == E_INVALIDARG, "unbounded_table_rejected");
  ranges[0].NumDescriptors = 1;
  root = {};

  NativeViewRecipe samplerRecipe;
  samplerRecipe.sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  samplerRecipe.sampler.AddressU = samplerRecipe.sampler.AddressV = samplerRecipe.sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplerRecipe.sampler.MaxAnisotropy = 1; samplerRecipe.sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  samplerRecipe.sampler.MaxLOD = std::numeric_limits<float>::max();
  NativeDescriptorVersion samplerVersion;
  require(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, {&samplerRecipe, 1}, samplerVersion), "sampler");
  const auto samplerHeap = samplerVersion.heap();
  auto invalidSampler = samplerRecipe; invalidSampler.sampler.Filter = D3D12_FILTER(0x7fffffff);
  check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, {&invalidSampler, 1}, samplerVersion) == E_INVALIDARG &&
        samplerVersion.heap() == samplerHeap, "invalid_sampler_filter_preserves_version");
  invalidSampler = samplerRecipe; invalidSampler.sampler.MaxAnisotropy = 0;
  check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, {&invalidSampler, 1}, samplerVersion) == E_INVALIDARG,
        "zero_anisotropy_rejected");
  invalidSampler = samplerRecipe; invalidSampler.sampler.ComparisonFunc = D3D12_COMPARISON_FUNC(0);
  check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, {&invalidSampler, 1}, samplerVersion) == E_INVALIDARG,
        "invalid_comparison_rejected");
  samplerVersion = {};

  for (uint32_t branch = 0; branch < 3; ++branch) {
    Readback result, thirdResult; uint64_t ticket = 0;
    std::vector<std::weak_ptr<const void>> owners;
    ComPtr<ID3D12Fence> delay; require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delay)), "delay fence");
    require(queue->Wait(delay.Get(), 1), "delay queue");
    {
      NativeRootSignature drawRoot; require(NativeRootSignature::create(device.Get(), rootDesc, drawRoot, extents), "independent draw root");
      owners.push_back(drawRoot.ownership());
      NativeDescriptorVersion branchSamplers;
      require(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
          {&samplerRecipe, 1}, branchSamplers), "branch sampler version");
      owners.push_back(branchSamplers.ownership());
      NativePipelineCache pipelines; require(pipelines.initialize(device.Get()), "pipeline cache");
      NativePipelineLease pipeline;
      auto p = pipelineDesc(drawRoot, branch == 0 ? decoded : fetch, branch == 0 ? pixelMrt : pixel);
      if (branch == 0) {
        p.inputLayout.push_back({"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0});
        p.colorCount = 3; p.colorFormats[1] = p.colorFormats[2] = p.colorFormats[0];
        p.blend.IndependentBlendEnable = TRUE; p.blend.RenderTarget[1].RenderTargetWriteMask = 0;
      }
      require(pipelines.acquire(p, pipeline), "draw pipeline");
      owners.push_back(pipeline.ownership());
      NativeImageDesc colorDesc; colorDesc.width = colorDesc.height = 8; colorDesc.resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
      colorDesc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET; colorDesc.clear.Format = colorDesc.resourceFormat; colorDesc.clear.Color[3] = 1;
      NativeImageLease color; require(images.create(colorDesc, color), "draw color"); result = readbackFor(device.Get(), color.resource());
      owners.push_back(color.ownership());
      NativeDrawRecording recording; require(recording.begin(device.Get()), "recording");
      require(recording.keep(color), "retain clear image"); require(images.recordClear(recording.commands(), color), "clear color");
      NativeImageLease hole, third;
      if (branch == 0) {
        require(images.createNullColor(colorDesc.resourceFormat, {1, 0}, hole), "owned null RTV");
        require(images.create(colorDesc, third), "third MRT color"); thirdResult = readbackFor(device.Get(), third.resource());
        require(recording.keep(third), "retain third clear image"); require(images.recordClear(recording.commands(), third), "clear third color");
        owners.push_back(hole.ownership()); owners.push_back(third.ownership());
        check(hole && hole.isNullColor() && !hole.resource() && hole.view().ptr, "null_rtv_owns_descriptor_without_texture");
        check(images.recordClear(recording.commands(), hole) == E_INVALIDARG, "null_rtv_clear_rejected");
        const auto handle = hole.handle();
        check(images.createNullColor(DXGI_FORMAT_UNKNOWN, {1, 0}, hole) == E_INVALIDARG && hole.handle() == handle,
              "invalid_null_rtv_preserves_output");
      }

      std::array<std::array<float, 4>, 3> sourceVertices{{{-1, 1, .5f, 1}, {3, 1, .5f, 1}, {-1, -3, .5f, 1}}};
      NativeBufferSlice vertices = ownUpload(device.Get(), bytesOf(sourceVertices)); owners.push_back(vertices.ownership());
      std::array<std::array<float, 4>, 6> constants{{{.125f, 0, 0, 0}, {.25f, 0, 0, 0},
          {0, .125f, 0, 0}, {0, .25f, 0, 0}, {0, 0, .125f, 0}, {0, 0, .25f, 0}}};
      std::array<NativeBufferSlice, 6> cb;
      for (uint32_t i = 0; i < cb.size(); ++i) {
        std::array<uint8_t, 512> data{}; std::memcpy(data.data(), constants[i].data(), 16);
        cb[i] = ownUpload(device.Get(), {data.data(), i == 0 ? data.size() : 16}, 256, true);
        owners.push_back(cb[i].ownership());
      }
      bool independent = vertices.resource() != cb[0].resource();
      for (uint32_t i = 0; i < cb.size(); ++i) for (uint32_t j = 0; j < i; ++j) independent &= cb[i].resource() != cb[j].resource();
      check(independent, "constant_and_vertex_allocations_are_independent");
      D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = td.Height = td.DepthOrArraySize = td.MipLevels = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
      std::array<NativeViewRecipe, 7> recipes{};
      constexpr std::array<uint8_t, 6> gray{255, 128, 64, 32, 16, 8};
      for (uint32_t i = 0; i < 6; ++i) {
        std::array<uint8_t, 256> texel{}; texel[0] = texel[1] = texel[2] = gray[i]; texel[3] = 255;
        NativeBufferSlice texelSlice = ownUpload(device.Get(), texel, 512); owners.push_back(texelSlice.ownership());
        ComPtr<ID3D12Resource> texture;
        require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&texture)), "texture");
        require(recording.keep(texture.Get()), "retain texture copy"); require(recording.keep(texelSlice), "retain owned source copy");
        if (branch == 0 && i == 0) check(recording.keep(texelSlice.resource()) == E_INVALIDARG, "borrowed_upload_keep_rejected");
        D3D12_TEXTURE_COPY_LOCATION from{}, to{}; from.pResource = texelSlice.resource(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = texelSlice.offset(); from.PlacedFootprint.Footprint = {td.Format, 1, 1, 1, 256};
        to.pResource = texture.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        recording.commands()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(recording.commands(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        recipes[i].resource = texture; recipes[i].srv.Format = td.Format; recipes[i].srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        recipes[i].srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; recipes[i].srv.Texture2D.MipLevels = 1;
        texel.fill(0);
      }
      recipes[6].buffer = vertices; recipes[6].srv.Format = DXGI_FORMAT_R32_TYPELESS; recipes[6].srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      recipes[6].srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      recipes[6].srv.Buffer = {vertices.offset() / 4, UINT(vertices.size() / 4), 0, D3D12_BUFFER_SRV_FLAG_RAW};
      NativeDescriptorVersion views; require(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, recipes, views), "views");
      owners.push_back(views.ownership());
      // Repeated keeps of one version / one staged range retain it once (hashed and
      // last-range checks); the owner checks below prove it survives until completion.
      check(recording.keep(views) == S_OK && recording.keep(views) == S_OK &&
            recording.keep(vertices) == S_OK && recording.keep(vertices) == S_OK,
            "repeated_version_and_range_keeps_accepted");
      if (branch == 0) {
        auto invalidView = recipes[0]; invalidView.srv.Texture2D.MipLevels = 2;
        check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {&invalidView, 1}, views) == E_INVALIDARG,
              "texture_mip_extent_rejected");
        invalidView = recipes[6]; invalidView.srv.Buffer.NumElements++;
        check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {&invalidView, 1}, views) == E_INVALIDARG,
              "buffer_descriptor_slice_extent_rejected");
        invalidView = recipes[6]; invalidView.srv.Format = DXGI_FORMAT_R32_UINT;
        check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {&invalidView, 1}, views) == E_INVALIDARG,
              "raw_buffer_format_rejected");
        invalidView = recipes[0]; invalidView.srv.Shader4ComponentMapping = 0;
        check(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {&invalidView, 1}, views) == E_INVALIDARG,
              "invalid_component_mapping_rejected");
      }
      PreparedNativeDraw d; d.pipeline = pipeline; d.signature = drawRoot; d.colors[0] = color; d.viewport = {0, 0, 8, 8, 0, 1};
      if (branch == 0) { d.colors[1] = hole; d.colors[2] = third; }
      d.scissor = {2, 2, 6, 6}; d.count = 3; d.kind = NativeDrawKind(branch);
      if (branch == 0) { d.vertices = vertices; d.vertexStride = 16; }
      if (branch == 2) {
        std::array<uint32_t, 3> indices{0, 1, 2}; d.indices = ownUpload(device.Get(), bytesOf(indices), 4);
        owners.push_back(d.indices.ownership());
        indices.fill(UINT32_MAX);
      }
      for (uint32_t i = 0; i < 15; ++i) {
        NativeRootBinding b; b.parameter = i;
        if (i < 5 || i == 9) b.buffer = cb[i == 9 ? 5 : i];
        else if (i == 14) { const std::array<float, 2> scale{.5f, .75f}; b.constants.resize(2); std::memcpy(b.constants.data(), scale.data(), 8); }
        else {
          b.table = i == 8 ? branchSamplers : views;
          b.firstDescriptor = i == 5 ? 0 : i == 6 ? 1 : i == 7 ? 2 : i == 11 ? 3 : i == 12 ? 4 : i == 13 ? 5 : i == 10 ? 6 : 0;
          b.descriptorCount = 1;
        }
        d.bindings.push_back(std::move(b));
      }
      if (branch == 0) {
        auto writeHoleDesc = p; writeHoleDesc.blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        NativePipelineLease writeHole; require(pipelines.acquire(writeHoleDesc, writeHole), "write-enabled null-slot fixture");
        auto invalidHole = d; invalidHole.pipeline = writeHole;
        checkRejected(device.Get(), invalidHole, "write_enabled_null_rtv_rejected_before_recording");
        auto invalid = d; invalid.bindings[0].buffer = cb[1];
        checkRejected(device.Get(), invalid, "short_aligned_cbv_extent_rejected_before_recording");
        std::array<uint8_t, 256> unsealed{}; std::memcpy(unsealed.data(), constants[1].data(), 16);
        invalid = d; invalid.bindings[1].buffer = ownUpload(device.Get(), unsealed, 256);
        checkRejected(device.Get(), invalid, "unsealed_cbv_padding_rejected_before_recording");
        invalid = d; invalid.bindings[14].constants.pop_back();
        checkRejected(device.Get(), invalid, "missing_pixel_scale_rejected_before_recording");
        invalid = d; invalid.count = 4;
        checkRejected(device.Get(), invalid, "vertex_extent_rejected_before_recording");
        constexpr uint32_t oversizedStride = D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES + 1;
        const std::array<uint8_t, oversizedStride * 3> largeVertices{};
        invalid = d; invalid.vertexStride = oversizedStride;
        invalid.vertices = ownUpload(device.Get(), largeVertices);
        checkRejected(device.Get(), invalid, "native_maximum_stride_rejected_before_recording");
      }
      require(recording.draw(d), "encode native draw"); check(recording.drawCount() == 1, "native_draw_retained");
      copyColor(recording, color.resource(), result);
      if (branch == 0) copyColor(recording, third.resource(), thirdResult);
      require(recording.close(), "close native draw");
      sourceVertices = {}; constants = {};
      recipes = {}; d = {}; pipeline = {}; pipelines.clear(); views = {}; branchSamplers = {};
      for (unsigned i = 0; i < 20; ++i) {
        NativeDescriptorVersion replacement; require(NativeDescriptorVersion::create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            {&samplerRecipe, 1}, replacement), "new sampler version");
      }
      require(recording.submit(images, ticket), "submit owned packet");
      check(images.wait(ticket, 0) == HRESULT_FROM_WIN32(WAIT_TIMEOUT), "gpu_is_delayed");
      require(images.release(color.handle()), "retire color registry handle");
      if (branch == 0) { require(images.release(hole.handle()), "retire null descriptor handle"); require(images.release(third.handle()), "retire third image handle"); }
      check(images.stats().pending == 1, "queue_owns_packet_before_completion");
    }
    // Producer, recorder, pipeline cache, descriptor handles and image handle
    // have all gone away. Only the queue's checked submission owns the packet.
    bool alive = true; for (const auto& owner : owners) alive &= !owner.expired();
    check(alive, "each_independent_owner_retained_before_gpu_execution");
    // A retention defect must never execute commands referencing freed memory.
    // The isolated fault-control process exits while its queue is still delayed.
    if (!alive) { std::printf("[NATIVE_DRAW_TEST] ABORT dangling queued resource before GPU execution\n"); std::exit(1); }
    require(delay->Signal(1), "release delayed GPU"); require(images.wait(ticket, 10000), "complete native packet");
    check(pixels(result), branch == 0 ? "decoded_vertices_exact_pixels_after_owner_destruction" :
        branch == 1 ? "fetch_vertices_exact_pixels_after_owner_destruction" : "indexed_fetch_exact_pixels_after_owner_destruction");
    if (branch == 0) check(pixels(thirdResult, {160, 80, 120, 255}), "sparse_mrt_preserves_target_two_index");
    require(images.collect(), "collect native packet"); check(images.stats().pending == 0, "completed_packet_retired");
    bool retired = true; for (const auto& owner : owners) retired &= owner.expired();
    check(retired, "each_independent_owner_retired_after_completion");
  }
  require(images.shutdown(), "drain native queue");
  std::printf("[NATIVE_DRAW_TEST] RESULT checks=%u passed=%u failed=%u\n", passed + failed, passed, failed);
  return failed ? 1 : 0;
}
