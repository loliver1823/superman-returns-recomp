// step 6 I3-B (bindless, Fable ruling 44): the production SRV heap, native recording, submission and the SR_ORACLE
// closing check on a WARP device. The GPU waits on a gate fence the test holds before every recording, so "in flight"
// is deterministic. The modes named *-gpu* draw: a pixel shader reads the bound heap slot (or the vertex shader the
// raw view) at EXECUTION, and the rendered pixel is read back - the lifetime rule is proven by what the GPU saw.
// One mode per run: it prints one PASS line, or an assertion throws (exit 1). An API / device failure (requireHr) is
// INFRA (exit 2, no verdict). The selection modes end in the production nativeRecordingFail (exit 3, its stage
// named). No window, no game.
//   difftest --native-bindless-test <mode>   (tools/difftest/native_bindless_run.py runs every mode)
#include "gfx/d3d12_backend.hpp"
#include "gfx/gfx_options.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if SR_ORACLE

namespace {
using Microsoft::WRL::ComPtr;
// An API / device failure is infrastructure, never an assertion: a mutant is killed only by its mode's assertion.
struct InfraError : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void requireHr(HRESULT hr, const char* what) {
  if (FAILED(hr)) { std::fprintf(stderr, "%s hr=%08lX\n", what, (unsigned long)hr); throw InfraError(what); }
}
constexpr uint32_t kRed = 0xFF0000FFu, kGreen = 0xFF00FF00u;  // RGBA8 bytes as a little-endian word
// The pixel shader reads the heap slot named by its root constant (as a translated shader reads its selection word),
// the raw pixel shader the word the vertex shader fetched from the raw view (t0 space5).
const char* kVs =
    "ByteAddressBuffer g_raw : register(t0, space5);\n"
    "struct O { float4 p : SV_Position; nointerpolation uint w : W; };\n"
    "O main(uint i : SV_VertexID) { O o; float2 t = float2((i << 1) & 2, i & 2);\n"
    "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.w = g_raw.Load(0); return o; }\n";
const char* kPsTexture =
    "Texture2D<float4> g_tex[8192] : register(t0, space0);\n"
    "cbuffer C : register(b0, space9) { uint g_slot; };\n"
    "float4 main(float4 p : SV_Position, nointerpolation uint w : W) : SV_Target {\n"
    "  return g_tex[g_slot].Load(int3(0, 0, 0)); }\n";
const char* kPsRaw =
    "float4 main(float4 p : SV_Position, nointerpolation uint w : W) : SV_Target {\n"
    "  return float4(w & 255, (w >> 8) & 255, (w >> 16) & 255, (w >> 24) & 255) / 255.0; }\n";
}  // namespace

namespace sr::gfx {
struct NativeBindlessTestAccess {
  D3D12Backend b;
  ComPtr<ID3D12Fence> gate, setupFence;
  ComPtr<ID3D12InfoQueue> info;  // SR_BINDLESS_TEST_DEBUG only
  // The debug layer's ERROR / CORRUPTION messages so far; `print` lists every warning and error.
  uint64_t debugErrors(bool print) {
    const UINT64 n = info->GetNumStoredMessages();
    uint64_t errors = 0;
    for (UINT64 i = 0; i < n; ++i) {
      SIZE_T length = 0;
      if (FAILED(info->GetMessage(i, nullptr, &length)) || !length) continue;
      std::vector<char> bytes(length);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
      if (FAILED(info->GetMessage(i, message, &length))) continue;
      errors += message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
      if (print && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        std::fprintf(stderr, "[NATIVE_BINDLESS_TEST] D3D12 sev=%d id=%d %s\n", int(message->Severity), int(message->ID),
                     message->pDescription);
    }
    return errors;
  }
  // The debug layer's messages, at exit (a PASS, a FAIL or an INFRA): an error message is the verdict's evidence.
  void report() {
    const uint64_t errors = debugErrors(true);
    std::fprintf(stderr, "[NATIVE_BINDLESS_TEST] debug layer: %llu messages, %llu errors\n",
                 (unsigned long long)info->GetNumStoredMessages(), (unsigned long long)errors);
  }
  ~NativeBindlessTestAccess() {
    if (info) report();
  }
  ComPtr<ID3D12RootSignature> root;  // production draws set m_nativeXlatRoot first (xlatBindRootTables)
  ComPtr<ID3D12PipelineState> psoTexture, psoRaw;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  uint64_t gateValue = 0, setupValue = 0;
  struct Target { ComPtr<ID3D12Resource> rt, readback; uint32_t rtv = 0; };
  std::vector<Target> targets;

  void setup() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    requireHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    requireHr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    // SR_BINDLESS_TEST_DEBUG=1 (or =gbv, + GPU-based validation): the D3D12 debug layer, its messages printed at exit;
    // any ERROR fails the mode. WARP accepts some invalid binds (a table outside the set heap) that the layer reports.
    if (const char* debug = std::getenv("SR_BINDLESS_TEST_DEBUG")) {
      ComPtr<ID3D12Debug1> layer;
      requireHr(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)), "debug layer (d3d12SDKLayers.dll)");
      layer->EnableDebugLayer();
      if (std::strcmp(debug, "gbv") == 0) layer->SetEnableGPUBasedValidation(TRUE);
    }
    requireHr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&b.m_device)), "WARP device");
    if (std::getenv("SR_BINDLESS_TEST_DEBUG")) {
      requireHr(b.m_device.As(&info), "debug info queue");
      info->SetMessageCountLimit(4096);
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    requireHr(b.m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&b.m_commandQueue)), "queue");
    requireHr(b.m_nativeImages.initialize(b.m_device.Get(), b.m_commandQueue.Get()), "native image store");
    requireHr(b.m_nativeRawGeometry.initialize(b.m_device.Get(), 4u << 20, 64u << 20), "native raw geometry");
    requireHr(b.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate fence");
    requireHr(b.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&setupFence)), "setup fence");
    require(b.createXlatSrvHeap(), "production SRV heap");
    b.texEvictInit();
    require(b.m_texEvictOn, "the fence-retired slot pool is on");
    b.m_xlatConstants.init(65536, false);
    b.m_xlatCBPtr = b.m_xlatConstants.data();
    // What a selection of 0 names: the 2D band's slot 0 and the legacy 3D / cube tables' entry 0.
    view(0, D3D12_SRV_DIMENSION_TEXTURE2D, nullptr);
    view(D3D12Backend::kXlat2DSlots, D3D12_SRV_DIMENSION_TEXTURE3D, nullptr);
    view(D3D12Backend::kXlat2DSlots + 8, D3D12_SRV_DIMENSION_TEXTURECUBE, nullptr);
    std::memset(b.m_xlatCBPtr, 0, 48 * sizeof(uint32_t));
    makeRoot();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 16;
    requireHr(b.m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "RTV heap");
  }
  // The translated root's TABLE layout (parameters 5-8, 10-13; d3d12_backend.cpp createXlatPipeline). Parameter 0
  // is the fixture shader's slot constant (b0 space9); the others are unused one-value constants.
  void makeRoot() {
    D3D12_DESCRIPTOR_RANGE ranges[8] = {};
    D3D12_ROOT_PARAMETER params[14] = {};
    const struct { uint32_t param, count, space; bool sampler; } tables[8] = {
        {5, D3D12Backend::kXlat2DSlots, 0, false}, {6, 8, 1, false}, {7, 8, 2, false},
        {8, kXlatSamplerTable, 3, true}, {10, 1, 5, false}, {11, D3D12Backend::kXlat2DSlots, 6, false},
        {12, D3D12Backend::kXlat2DSlots, 7, false}, {13, D3D12Backend::kXlat2DSlots, 8, false}};
    for (uint32_t i = 0; i < 14; ++i) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[i].Constants.ShaderRegister = i;
      params[i].Constants.RegisterSpace = 9;
      params[i].Constants.Num32BitValues = 1;
      params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (uint32_t i = 0; i < 8; ++i) {
      ranges[i].RangeType = tables[i].sampler ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[i].NumDescriptors = tables[i].count;
      ranges[i].RegisterSpace = tables[i].space;
      auto& p = params[tables[i].param];
      p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      p.DescriptorTable.NumDescriptorRanges = 1;
      p.DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 14;
    desc.pParameters = params;
    ComPtr<ID3DBlob> blob, error;
    requireHr(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "fixture root serialize");
    requireHr(b.m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "fixture root");
  }
  ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code, error;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "native-bindless-fixture", nullptr, nullptr, "main",
                                  target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(hr) && error) std::fprintf(stderr, "%s\n", static_cast<const char*>(error->GetBufferPointer()));
    requireHr(hr, "fixture shader compile");
    return code;
  }
  void makePipelines() {
    const auto vs = compile(kVs, "vs_5_1");
    for (bool raw : {false, true}) {
      const auto ps = compile(raw ? kPsRaw : kPsTexture, "ps_5_1");
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
      pd.pRootSignature = root.Get();
      pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
      pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
      pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pd.SampleMask = UINT_MAX;
      pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pd.RasterizerState.DepthClipEnable = TRUE;
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pd.NumRenderTargets = 1;
      pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pd.SampleDesc.Count = 1;
      requireHr(b.m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(raw ? &psoRaw : &psoTexture)), "PSO");
    }
  }
  ComPtr<ID3D12Resource> image(bool target, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = 1;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (target) td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> r;
    requireHr(b.m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&r)),
              "image");
    return r;
  }
  ComPtr<ID3D12Resource> texture() { return image(false, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); }
  D3D12_CPU_DESCRIPTOR_HANDLE rtv(uint32_t i) {
    return {rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
            SIZE_T(i) * b.m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
  }
  // A 1x1 texture of one colour (cleared as a render target in its own submission, then shader-readable).
  ComPtr<ID3D12Resource> coloured(uint32_t rgba) {
    auto r = image(true, D3D12_RESOURCE_STATE_RENDER_TARGET);
    b.m_device->CreateRenderTargetView(r.Get(), nullptr, rtv(15));
    const float c[4] = {float(rgba & 255) / 255, float((rgba >> 8) & 255) / 255, float((rgba >> 16) & 255) / 255,
                        float(rgba >> 24) / 255};
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    requireHr(b.m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "setup alloc");
    requireHr(b.m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                            IID_PPV_ARGS(&list)), "setup list");
    list->ClearRenderTargetView(rtv(15), c, 0, nullptr);
    barrier(list.Get(), r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    requireHr(list->Close(), "setup close");
    ID3D12CommandList* lists[] = {list.Get()};
    b.m_commandQueue->ExecuteCommandLists(1, lists);
    requireHr(b.m_commandQueue->Signal(setupFence.Get(), ++setupValue), "setup signal");
    while (setupFence->GetCompletedValue() < setupValue) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return r;
  }
  static void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                      D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER rb{};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = from;
    rb.Transition.StateAfter = to;
    list->ResourceBarrier(1, &rb);
  }
  void view(uint32_t slot, D3D12_SRV_DIMENSION dimension, ID3D12Resource* resource) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = dimension;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (dimension == D3D12_SRV_DIMENSION_TEXTURE2D) sv.Texture2D.MipLevels = 1;
    else if (dimension == D3D12_SRV_DIMENSION_TEXTURE3D) sv.Texture3D.MipLevels = 1;
    else sv.TextureCube.MipLevels = 1;
    b.publishXlatSrv(resource, &sv, {b.m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                     SIZE_T(slot) * b.m_xlatSrvInc});
  }
  // The GPU waits on the gate before anything submitted after this point: the recording stays in flight until
  // release().
  void open() {
    requireHr(b.m_commandQueue->Wait(gate.Get(), ++gateValue), "gate wait");
    b.clReset(D3D12Backend::Pm4Reset);
    require(b.m_nativeRecording && b.m_clState == D3D12Backend::ClState::Recording, "recording open");
    b.m_nativeRecording->commands()->SetGraphicsRootSignature(root.Get());
  }
  void submit() {
    b.clClose(D3D12Backend::Pm4Close);
    b.clExecute(D3D12Backend::Pm4Exec);
  }
  void release() { requireHr(gate->Signal(gateValue), "gate signal"); }
  void idle() {
    release();
    requireHr(b.m_nativeImages.wait(b.m_nativeSubmission, 10000), "submissions complete");
  }
  // A draw's texture binding (the production bindXlatTextures): selection word 0 names `slot`; the rest name the
  // defaults.
  void bind(uint32_t slot, const NativeBufferSlice* raw = nullptr) {
    reinterpret_cast<uint32_t*>(b.m_xlatCBPtr)[0] = slot;
    D3D12Backend::DrawBatch batch{};
    batch.sharedCbOff = 0;
    if (raw) { batch.vsf = true; batch.vsfRawVertices = *raw; }
    b.bindXlatTextures(batch);
  }
  // bind + one draw into a fresh 1x1 target + its readback copy, all in the open recording. Returns the target.
  uint32_t draw(uint32_t slot, const NativeBufferSlice* raw = nullptr) {
    bind(slot, raw);
    auto* cl = b.m_nativeRecording->commands();
    Target t;
    t.rt = image(true, D3D12_RESOURCE_STATE_RENDER_TARGET);
    t.rtv = uint32_t(targets.size());
    require(t.rtv < 15, "fixture targets");
    b.m_device->CreateRenderTargetView(t.rt.Get(), nullptr, rtv(t.rtv));
    D3D12_HEAP_PROPERTIES rh{};
    rh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    requireHr(b.m_device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&t.readback)), "readback");
    cl->SetGraphicsRoot32BitConstant(0, slot, 0);
    cl->SetPipelineState(raw ? psoRaw.Get() : psoTexture.Get());
    const auto handle = rtv(t.rtv);
    cl->OMSetRenderTargets(1, &handle, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0, 0, 1, 1, 0, 1};
    const D3D12_RECT sc{0, 0, 1, 1};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
    barrier(cl, t.rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = t.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
    src.pResource = t.rt.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    targets.push_back(std::move(t));
    return uint32_t(targets.size() - 1);
  }
  uint32_t pixel(uint32_t target) {
    void* p = nullptr;
    const D3D12_RANGE r{0, 4};
    requireHr(targets[target].readback->Map(0, &r, &p), "readback map");
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    const D3D12_RANGE none{0, 0};
    targets[target].readback->Unmap(0, &none);
    return v;
  }
  NativeBufferSlice staged(uint32_t word) {
    NativeBufferSlice slice;
    // Production's alignment (d3d12_frame_render.cpp stages raw vertex bytes at 16): a raw view's first element
    // must sit on 16 bytes, and a 4-aligned slice removes the device (the debug layer's error #31).
    requireHr(b.m_nativeRawGeometry.stage({reinterpret_cast<const uint8_t*>(&word), 4}, 16u, slice),
              "staged raw range");
    return slice;
  }
  void bad(uint64_t inFlight, uint64_t beforeSubmit, const char* what) {
    std::printf("[NATIVE_BINDLESS_TEST] bad_in_flight=%llu bad_before_submit=%llu checked=%llu\n",
                (unsigned long long)b.m_i3bBadInFlight, (unsigned long long)b.m_i3bBadOpen,
                (unsigned long long)b.m_i3bChecked);
    require(b.m_i3bBadInFlight == inFlight && b.m_i3bBadOpen == beforeSubmit, what);
  }
  void seen(uint32_t target, uint32_t want, const char* what) {
    const uint32_t got = pixel(target);
    std::printf("[NATIVE_BINDLESS_TEST] target %u read %08X (want %08X)\n", target, got, want);
    require(got == want, what);
  }

  void run(const std::string& mode) {
    setup();
    const uint32_t s = b.texSlotAlloc();
    require(s != 0, "a pool slot");
    auto plain = texture();  // slot s holds a 2D view from the start (a mode may republish it before any bind)
    view(s, D3D12_SRV_DIMENSION_TEXTURE2D, plain.Get());
    // Preflight: the production sampler selection runs before any recording, so a failure there is named.
    std::printf("[NATIVE_BINDLESS_TEST] preflight samplers\n");
    std::fflush(stdout);
    const auto options = gfxopt::current();
    requireHr(b.m_nativeSamplers.select(b.m_device.Get(), uint32_t(options.anisotropic), options.lodBias,
                                        b.m_volSamplers, b.m_volSamplerGen, b.m_drawSamplers), "sampler version");
    b.m_drawSamplersVersion = gfxopt::version();
    b.m_drawSamplersVolGen = b.m_volSamplerGen;
    std::printf("[NATIVE_BINDLESS_TEST] preflight ok\n");
    std::fflush(stdout);
    if (mode.find("-gpu") != std::string::npos) makePipelines();  // every drawing mode (inplace-gpu-control too)
    if (mode == "rewrite-before-submit") {
      // A slot rewritten in place after a draw bound it, before the recording is submitted: the submission check.
      auto r1 = texture();
      view(s, D3D12_SRV_DIMENSION_TEXTURE2D, r1.Get());
      open();
      bind(s);
      auto r2 = texture();
      view(s, D3D12_SRV_DIMENSION_TEXTURE2D, r2.Get());
      submit();
      bad(0, 1, "a rewrite between bind and submission is BAD");
      idle();
    } else if (mode == "rewrite-in-flight") {
      // A slot rewritten in place while the recording that read it is still executing: the write check.
      auto r1 = texture();
      view(s, D3D12_SRV_DIMENSION_TEXTURE2D, r1.Get());
      open();
      bind(s);
      submit();
      auto r2 = texture();
      view(s, D3D12_SRV_DIMENSION_TEXTURE2D, r2.Get());
      bad(1, 0, "a rewrite of a slot an in-flight submission reads is BAD");
      idle();
    } else if (mode == "cow-gpu" || mode == "inplace-gpu-control") {
      // Recording 1 draws slot s (red) and stays in flight; then the view changes to green. Copy-on-write (the
      // production path) gives green a NEW slot, so recording 1, executing only after the change, still reads RED,
      // and recording 2 reads GREEN from the new slot. The control rewrites s in place instead: recording 1 then
      // reads GREEN (the readback sees execution-time descriptors) and the write check reports BAD.
      ComPtr<ID3D12Resource> owner = coloured(kRed);  // the texture's only owner besides the slot's recipe
      view(s, D3D12_SRV_DIMENSION_TEXTURE2D, owner.Get());
      auto green = coloured(kGreen);
      open();
      const uint32_t first = draw(s);
      submit();
      uint32_t t = s;
      if (mode == "cow-gpu") {
        t = b.texSlotCow(s, owner, "fixture");
        require(t != 0 && t != s && !owner && b.m_texSlotRetire.pending() == 1 && b.m_i3bSlotCow == 1,
                "copy-on-write: a fresh slot, the old one (and its resource, now owned by the retire) retiring");
      } else {
        owner.Reset();
      }
      view(t, D3D12_SRV_DIMENSION_TEXTURE2D, green.Get());
      green.Reset();
      open();
      const uint32_t second = draw(t);
      submit();
      idle();
      if (mode == "cow-gpu") {
        seen(first, kRed, "the in-flight recording read the OLD view (copy-on-write kept it)");
        seen(second, kGreen, "the next recording reads the new view from the new slot");
        bad(0, 0, "copy-on-write writes are never BAD");
      } else {
        seen(first, kGreen, "the control: an in-place rewrite reaches the in-flight recording");
        bad(1, 0, "the control: the write check reports the in-place rewrite");
      }
    } else if (mode == "raw-ring-gpu") {
      // A full ring wrap with real writes. Recording 1 draws from raw slot 0 (red) and stays in flight; the next
      // recordings write the other 65535 ring slots; the last recording's write lands on slot 0 again and must wait
      // until recording 1 completed - so recording 1 read RED, the last reads GREEN. The fill is split into
      // frame-sized recordings (4096 binds): one list of 65535 binds (~655k root calls) removes the WARP device.
      open();
      const auto red = staged(kRed);
      const uint32_t first = draw(s, &red);
      submit();
      require(b.m_rawRingWrites == 1, "one ring write");
      for (uint32_t i = 1; i < D3D12Backend::kXlatRawRingSlots;) {
        open();
        for (const uint32_t end = std::min(i + 4096u, D3D12Backend::kXlatRawRingSlots); i < end; ++i) {
          const auto other = staged(0x01000000u | i);
          bind(s, &other);
        }
        submit();
      }
      require(b.m_rawRingWrites == D3D12Backend::kXlatRawRingSlots && b.m_i3bRawWaits == 0, "the ring is full");
      open();
      std::thread releaser([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        release();
      });
      const auto greenWord = staged(kGreen);
      const uint32_t third = draw(s, &greenWord);
      releaser.join();
      submit();
      idle();
      std::printf("[NATIVE_BINDLESS_TEST] raw_ring_waits=%llu\n", (unsigned long long)b.m_i3bRawWaits);
      require(b.m_i3bRawWaits == 1, "the wrapping write waited for the submission that read the slot");
      seen(first, kRed, "the in-flight recording read its own raw view (the ring waited)");
      seen(third, kGreen, "the wrapped slot holds the new raw view");
      bad(0, 0, "a waited ring write is never BAD");
    } else if (mode == "heap-change-gpu") {
      // Something else sets other descriptor heaps on the list between two draws (as the EDRAM passes do): the next
      // draw's bind restores the translated heap, so both draws read their own slots.
      ComPtr<ID3D12Resource> red = coloured(kRed), green = coloured(kGreen);
      view(s, D3D12_SRV_DIMENSION_TEXTURE2D, red.Get());
      const uint32_t s2 = b.texSlotAlloc();
      view(s2, D3D12_SRV_DIMENSION_TEXTURE2D, green.Get());
      ComPtr<ID3D12DescriptorHeap> foreign;
      D3D12_DESCRIPTOR_HEAP_DESC hd{};
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      hd.NumDescriptors = 16;
      hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      requireHr(b.m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&foreign)), "foreign heap");
      open();
      const uint32_t first = draw(s);
      ID3D12DescriptorHeap* heaps[] = {foreign.Get()};
      b.m_nativeRecording->commands()->SetDescriptorHeaps(1, heaps);
      const uint32_t second = draw(s2);
      submit();
      idle();
      seen(first, kRed, "the first draw reads its slot");
      seen(second, kGreen, "the draw after a foreign heap change reads its own slot");
      bad(0, 0, "no BAD");
    } else if (mode == "raw-ring-same-recording") {
      // The same staged range in one recording reuses its ring slot; a new recording writes a new one.
      open();
      const auto a = staged(3);
      bind(s, &a);
      bind(s, &a);
      require(b.m_rawRingWrites == 1, "one ring write for a repeated range in one recording");
      submit();
      open();
      bind(s, &a);
      require(b.m_rawRingWrites == 2, "a new recording writes its own ring slot");
      submit();
      bad(0, 0, "ring reuse is never BAD");
      idle();
    } else if (mode == "exhaust-victim") {
      // Slot exhaustion rewrites its victim's slot at once: only a slot bound two or more frames ago qualifies, and
      // the reuse waits until the GPU finished the frame that last bound it. The first check runs with every fence
      // complete, so a wrong victim (the previous frame's slot) fails the assertion rather than a wait that never ends.
      requireHr(b.m_device->CreateFence(10, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&b.m_fence)), "frame fence");
      b.m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
      b.m_fenceValues[b.m_frameIndex] = 10;              // this frame ends at 10; bound now = stamp 11
      b.m_texRefresh[0xA1].slot = 5; b.m_texSlotUse[5] = 11;  // bound in this frame
      b.m_texRefresh[0xA2].slot = 6; b.m_texSlotUse[6] = 10;  // bound in the previous frame (its fence: this frame's end)
      uint64_t victim = 0;
      require(!b.texExhaustVictim(victim), "no victim while every slot was bound in this or the previous frame");
      // Two frames on (the GPU still at 10): slot 7, bound two frames ago, qualifies; its fence 11 is not done.
      b.m_fenceValues[b.m_frameIndex] = 12;
      b.m_texSlotUse[5] = 13;                                  // this frame
      b.m_texSlotUse[6] = 12;                                  // the previous frame
      b.m_texRefresh[0xA3].slot = 7; b.m_texSlotUse[7] = 11;  // two frames ago
      std::thread signaler([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        b.m_fence->Signal(11);
      });
      const bool found = b.texExhaustVictim(victim);
      signaler.join();
      std::printf("[NATIVE_BINDLESS_TEST] exhaust victim=%llX waits=%llu\n", (unsigned long long)victim,
                  (unsigned long long)b.m_i3bExhaustWaits);
      require(found && victim == 0xA3 && b.m_i3bExhaustWaits == 1 && b.m_fence->GetCompletedValue() >= 11,
              "the two-frames-old victim, after a counted wait for its frame");
      CloseHandle(b.m_fenceEvent);
      b.m_fenceEvent = nullptr;
    } else if (mode == "single-submission") {
      // Fable 44 (1) (a): a native recording executes once. A second submit of the same recording is refused.
      open();
      bind(s);
      submit();
      uint64_t again = 0;
      require(b.m_nativeRecording->submit(b.m_nativeImages, again) == E_ABORT, "a second submission is refused");
      idle();
    } else if (mode == "selection-dimension") {
      // A selection word that names a slot of another view type: the production refusal (exit 3,
      // stage texture-binding-dimension).
      auto r1 = texture();
      view(s, D3D12_SRV_DIMENSION_TEXTURE2D, r1.Get());
      reinterpret_cast<uint32_t*>(b.m_xlatCBPtr)[16] = 0xA0000000u | s;  // the 3D band names a 2D slot
      open();
      bind(s);
      throw std::runtime_error("selection-dimension: the wrong-type selection was bound");
    } else if (mode == "selection-range") {
      // A legacy cube index one past its 8-entry table, landing on a slot that DOES hold a cube view: only the
      // bounds rule refuses it (exit 3, stage texture-binding-index), so a bounds-check mutant binds it and fails.
      auto r1 = texture();
      view(s, D3D12_SRV_DIMENSION_TEXTURE2D, r1.Get());
      view(D3D12Backend::kXlat2DSlots + 16, D3D12_SRV_DIMENSION_TEXTURECUBE, nullptr);
      reinterpret_cast<uint32_t*>(b.m_xlatCBPtr)[32] = 8;
      open();
      bind(s);
      throw std::runtime_error("selection-range: the out-of-range selection was bound");
    } else {
      throw std::runtime_error("unknown mode");
    }
    // Under the debug layer an invalid call is the mode's failure, even where WARP renders the right pixel (M10).
    if (info) require(debugErrors(false) == 0, "the D3D12 debug layer reported no ERROR");
    std::printf("[NATIVE_BINDLESS_TEST] PASS %s\n", mode.c_str());
  }
};
}  // namespace sr::gfx

int runNativeBindlessTest(const char* mode) {
  if (!mode) {
    std::printf("[NATIVE_BINDLESS_TEST] FAIL no mode (tools/difftest/native_bindless_run.py lists them)\n");
    return 2;
  }
  // A failed mode may leave a gated recording in flight: the verdict is printed first and the process ends without
  // tearing the device down under it (a teardown race would replace exit 1 / 2 with a crash).
  auto* access = new sr::gfx::NativeBindlessTestAccess;
  int code = 0;
  try {
    access->run(mode);
  } catch (const InfraError& error) {
    std::fprintf(stderr, "[NATIVE_BINDLESS_TEST] INFRA %s\n", error.what());
    code = 2;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "[NATIVE_BINDLESS_TEST] FAIL %s\n", error.what());
    code = 1;
  }
  if (code) {
    if (access->gate) access->gate->Signal(access->gateValue);
    if (access->info) access->report();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
  }
  delete access;
  return 0;
}
#else
#include <cstdio>
int runNativeBindlessTest(const char*) {
  std::printf("[NATIVE_BINDLESS_TEST] not compiled: the I3-B closing check is SR_ORACLE-only\n");
  return 2;
}
#endif
