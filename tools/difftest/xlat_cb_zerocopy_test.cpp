// step 6 I10p (Astra I10p r2: "a production-binding fixture ... REQUIRED before the live rolls"). The view path's
// render-side binder (D3D12Backend::zcBindConstants), the CP's rotation (zcRotateCbThird: both rotation sites' helper)
// and the check mode (zcConsume / zcSubmitted / zcBeginThird / zcNoteThirdHigh) on a WARP device, with real native
// recordings (clReset / clClose / clExecute) submitted to the backend's own queue.
//
// The GPU consumer: each consume records the production binder into the open recording, then one draw whose pixel
// shader reads the root CBVs the binder set (VS b0 / PS b1 / shared b2 space4, fetch b5 space5) and stores every word
// into a UAV slot. Every byte check is therefore the GPU's view at the consumer's own execution time, never a CPU
// read of the upload twin. A fence the fixture owns holds the queue, so "the first consumer is still outstanding" is
// deterministic, not a race with WARP. The CP side writes its blocks with the production writer
// (xlatUploadUsedBlocks into the twin, M1) and fills the batch fields as tryXlatDraw does; the drain's own batch
// construction is the release admission fixture's zc-views case.
//
// Cases (each line "[XLAT_CB_ZC_TEST] PASS|FAIL <name>"):
//   A  a retained generation rendered TWICE with changed shared bytes while the first consumer is outstanding: the
//      re-render stages fallbacks and never rewrites a slot; the first consumer reads the ORIGINAL bytes; the
//      cross-frame consumes count as checked_xframe; the unchanged fallback is reused across a recording reset and
//      held by the recording that reuses it (released after its last consumer).
//   B  the retire after completion (no BAD, every recorded range rechecked, the used range poisoned) and Astra r2 P3:
//      an empty replay-tail generation records no poison range of its own.
//   C  deterministic negatives, each counted exactly: a stale view (compare-on-consume + stale generation; the GPU reads
//      poison), stale bytes (compare-on-retire), an outstanding consumer and an open recording (the retire assert; the
//      outstanding consumer reads the poison - the hazard is real), a rotation without its own fence wait.
//   D  Astra r2 P2 through the production paths: the binder's slot writes (render thread) racing the retire's poison
//      (CP thread) leave the twin's bytes (a GPU copy) equal to the shadow's.
// Controls (each must FAIL its named check): --control=rewrite-slot (the binder's write-once guard cleared before the
// re-render) -> first-consumer-reads-the-original-shared-bytes; --control=skip-keep (the reused fallback not kept by
// the recording that binds it) -> fallback-slice-held-by-the-recording-that-reuses-it.
//     difftest --xlat-cb-zerocopy-test [--control=rewrite-slot|skip-keep|skip-page-keep|release-at-slice-death]
// Step 6 R1a: skip-page-keep / release-at-slice-death are the recording's own upload-keep mutants (SR_ORACLE seam);
// like skip-keep they must fail exactly fallback-slice-held-by-the-recording-that-reuses-it, in both
// SR_RENDER_KEEP_LEAN arms.
// Development build only (SR_ORACLE=1): the check mode is compiled out of release.
#include "gfx/d3d12_backend.hpp"
#include "app/log.hpp"
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !SR_ORACLE
#error The zero-copy fixture needs the check mode (SR_ORACLE=1: the development build's difftest).
#endif

using Microsoft::WRL::ComPtr;

// The fixture's only route to the twin's resource: a GPU copy into a readback buffer (Case D).
struct sr::gfx::XlatCBGpuTestAccess {
  static ID3D12Resource* resource(XlatCBGpu& g) { return g.m_resource.Get(); }
};

namespace {
unsigned g_passed = 0, g_failed = 0;
void check(bool ok, const std::string& name, const std::string& detail = {}) {
  std::printf("[XLAT_CB_ZC_TEST] %s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : " ",
              detail.c_str());
  std::fflush(stdout);
  ok ? ++g_passed : ++g_failed;
}
void requireHr(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    char m[192];
    std::snprintf(m, sizeof m, "%s hr=%08lX", what, (unsigned long)hr);
    throw std::runtime_error(m);
  }
}
void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}
uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// The consumer: a full-viewport triangle on a 1 x 1 viewport (one pixel) whose pixel shader stores every word of the
// four constant buffers the binder bound into the UAV slot: [0, 4096) VS, [4096, 8192) PS, [8192, 8704) shared,
// [8704, 9472) fetch.
constexpr char kConsumerHlsl[] = R"(
cbuffer V : register(b0, space4) { uint4 v[256]; };
cbuffer P : register(b1, space4) { uint4 p[256]; };
cbuffer S : register(b2, space4) { uint4 s[32]; };
cbuffer F : register(b5, space5) { uint4 f[48]; };
RWByteAddressBuffer o : register(u0, space9);
float4 vsMain(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.5, 1.0);
}
void psMain(float4 pos : SV_Position) {
  [loop] for (uint i = 0; i < 256; ++i) { o.Store4(i * 16, v[i]); o.Store4(4096 + i * 16, p[i]); }
  [loop] for (uint j = 0; j < 32; ++j) o.Store4(8192 + j * 16, s[j]);
  [loop] for (uint k = 0; k < 48; ++k) o.Store4(8704 + k * 16, f[k]);
}
)";
constexpr uint64_t kSlotBytes = 16384, kSlots = 16, kReadbackBytes = 16u << 20;
constexpr uint32_t kVsUse = 0x0003u, kPsUse = 0x0005u;   // VS blocks 0, 1; PS blocks 0, 2 (M1: only these written)
}  // namespace

namespace sr::gfx {
struct XlatCbZeroCopyTestAccess {
  using Batch = D3D12Backend::DrawBatch;
  struct CpDraw {
    Batch batch;
    std::array<uint32_t, 1024> vs{}, ps{};
    std::array<uint8_t, 768> fetch{};
    std::array<uint8_t, 512> shared{};
  };
  struct Counts {
    uint64_t checked, xframe, bad, stale, retired, retireChecked, retireBad, fenceBad;
    uint64_t binds, writes, reuses, fallbacks;
  };

  std::unique_ptr<D3D12Backend> b_;
  std::string control_;
  ComPtr<ID3D12Fence> gate_, fence_;
  uint64_t fenceValue_ = 0;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> pso_;
  ComPtr<ID3D12Resource> out_, readback_;
  ComPtr<ID3D12CommandAllocator> alloc_;
  ComPtr<ID3D12GraphicsCommandList> list_;

  explicit XlatCbZeroCopyTestAccess(std::string control) : control_(std::move(control)) {
    b_ = std::make_unique<D3D12Backend>();
    auto& b = *b_;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    requireHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    requireHr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    requireHr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&b.m_device)), "WARP device");
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    requireHr(b.m_device->CreateCommandQueue(&queue, IID_PPV_ARGS(&b.m_commandQueue)), "queue");
    requireHr(b.m_nativeImages.initialize(b.m_device.Get(), b.m_commandQueue.Get(), 4, 0), "queue ownership");
    requireHr(b.m_nativeConstants.initialize(b.m_device.Get(), 65536, 4u << 20), "constant arena (fallbacks)");
    // The view path as createXlatPipeline installs it: the 192 MiB write-only twin, the check mode's shadow under
    // its mutex (zcShadowInit), and a compact shared scratch (3 x 1 MiB here).
    requireHr(b.m_xlatCBGpu.initialize(b.m_device.Get(), b.kXlatCBBytes), "upload twin");
    b.m_zcOn = true;
    b.m_zcCheck = true;
    b.m_zcMutant = false;
    require(b.zcShadowInit(), "check-mode shadow");
    require(b.m_xlatConstants.init(3u << 20, false), "shared scratch");
    b.m_xlatCBPtr = b.m_xlatConstants.data();
    requireHr(b.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate_)), "gate fence");
    requireHr(b.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "fixture fence");
    requireHr(b.m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_)), "allocator");
    requireHr(b.m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr,
                                            IID_PPV_ARGS(&list_)), "list");
    requireHr(list_->Close(), "list close");
    // The consumer's root: the production parameter indices for every CBV the binder sets (0 / 3 = VS, 1 / 4 = PS,
    // 2 = shared, 9 = fetch; the production registers b0-b4 space4, b5 space5), placeholders at 5-8, the UAV at 10.
    // Parameter 9 is visible to all stages here (production: the vertex stage) so the pixel shader can store it.
    D3D12_ROOT_PARAMETER params[11] = {};
    for (uint32_t i = 0; i < 5; ++i) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      params[i].Descriptor.ShaderRegister = i;
      params[i].Descriptor.RegisterSpace = 4;
      params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (uint32_t i = 5; i < 9; ++i) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[i].Constants.ShaderRegister = 10 + i;
      params[i].Constants.RegisterSpace = 10;
      params[i].Constants.Num32BitValues = 1;
      params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[9].Descriptor.ShaderRegister = 5;
    params[9].Descriptor.RegisterSpace = 5;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[10].Descriptor.ShaderRegister = 0;
    params[10].Descriptor.RegisterSpace = 9;
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 11;
    rs.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    requireHr(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err), "root serialize");
    requireHr(b.m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)),
              "root");
    auto compile = [&](const char* entry, const char* target) {
      ComPtr<ID3DBlob> code, errors;
      const HRESULT hr = D3DCompile(kConsumerHlsl, sizeof(kConsumerHlsl) - 1, "xlat-cb-zerocopy-consumer", nullptr,
                                    nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
      if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
      requireHr(hr, "consumer shader compile");
      return code;
    };
    const auto vs = compile("vsMain", "vs_5_1"), ps = compile("psMain", "ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 0;
    pd.SampleDesc.Count = 1;
    requireHr(b.m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)), "consumer pipeline");
    auto buffer = [&](uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = type;
      D3D12_RESOURCE_DESC rd{};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = bytes;
      rd.Height = 1;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      rd.Flags = flags;
      ComPtr<ID3D12Resource> r;
      requireHr(b.m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                    IID_PPV_ARGS(&r)), "fixture buffer");
      return r;
    };
    out_ = buffer(kSlots * kSlotBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    readback_ = buffer(kReadbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
  }
  ~XlatCbZeroCopyTestAccess() {
    if (!b_) return;
    if (gate_) gate_->Signal(1u << 20);   // nothing stays held
    const HRESULT hr = b_->m_nativeImages.shutdown(10000);
    if (FAILED(hr)) {
      std::fprintf(stderr, "[XLAT_CB_ZC_TEST] CLEANUP_FAILED hr=%08lX\n", (unsigned long)hr);
      std::fflush(nullptr);
      std::_Exit(2);
    }
  }
  D3D12Backend& b() { return *b_; }

  // ---- the CP side (as tryXlatDraw / pm4StageVsf / the drain's rotation do it) ----
  // The drain's rotation (drainDrawQueue, !pm4Append): the next third, its fence wait, the CB third / generation /
  // check-mode retire (zcRotateCbThird), then - at the drain site only - the cursors reset into the new set's third.
  // drainSite = false is replayPrepareTail's rotation (no cursor reset). waitThird: the third the fence wait names
  // (default: the new one; ~0u = no wait at all).
  void rotate(bool drainSite, uint32_t waitThird = 3u) {
    auto& b = this->b();
    b.m_xlatThird = (b.m_xlatThird + 1u) % 3u;
    if (waitThird != ~0u) b.pm4WaitThirdFree(waitThird == 3u ? b.m_xlatThird : waitThird);
    b.zcRotateCbThird(drainSite);
    if (drainSite) {
      b.m_xlatCBUsed = uint64_t(b.m_xlatCbThird) * (b.kXlatCBBytes / 3);
      b.m_xlatShUsed = uint64_t(b.m_xlatThird) * (b.m_xlatConstants.size() / 3);
    }
  }
  // One translated draw's CP work on the view path: the VS / PS / shared-slot / fetch allocation in the CB third (the
  // drain's layout), M1 writes of the used blocks and the fetch block into the twin, the shared block into the compact
  // scratch, and the batch fields (offsets, generation, the check mode's use masks and hashes from the sources).
  CpDraw cpDraw(uint32_t ordinal, uint32_t seed) {
    auto& b = this->b();
    CpDraw d;
    for (uint32_t i = 0; i < 1024; ++i) {
      d.vs[i] = (seed * 0x01000193u) ^ (0x3F000000u + i);
      d.ps[i] = (seed * 0x0100019Du) ^ (0x40000000u + 3u * i);
    }
    for (uint32_t i = 0; i < 768; ++i) d.fetch[i] = uint8_t(seed * 7u + i);
    for (uint32_t i = 0; i < 512; ++i) d.shared[i] = uint8_t(seed * 11u + 5u * i);
    uint64_t cb = b.m_xlatCBUsed;
    const uint64_t vsOff = cb;
    cb += 4096;
    const uint64_t psOff = alignUp(cb, 256);
    cb = psOff + 4096;
    const uint64_t shOff = alignUp(cb, 256);
    cb = shOff + 512;
    const uint64_t fetchOff = alignUp(cb, 256);
    cb = fetchOff + 768;
    require(cb <= uint64_t(b.m_xlatCbThird + 1) * (b.kXlatCBBytes / 3), "fixture CB third capacity");
    b.m_xlatCBUsed = cb;
    auto up = [&](uint64_t o, const void* p, size_t n) { require(b.m_xlatCBGpu.write(o, p, n), "twin write"); };
    xlatUploadUsedBlocks(up, vsOff, d.vs.data(), kVsUse);
    xlatUploadUsedBlocks(up, psOff, d.ps.data(), kPsUse);
    up(fetchOff, d.fetch.data(), d.fetch.size());
    const uint64_t shScr = alignUp(b.m_xlatShUsed, 256);
    require(shScr + 512 <= uint64_t(b.m_xlatThird + 1) * (b.m_xlatConstants.size() / 3), "fixture scratch capacity");
    std::memcpy(b.m_xlatCBPtr + shScr, d.shared.data(), d.shared.size());
    b.m_xlatShUsed = shScr + 512;
    Batch& x = d.batch;
    x.drawOrdinal = ordinal;
    x.vsf = true;
    x.zcView = true;
    x.xlatVsCbOff = vsOff;
    x.xlatPsCbOff = psOff;
    x.vsfFetchCbOff = fetchOff;
    x.sharedCbOff = shScr;
    x.zcSharedOff = shOff;
    x.zcSet = b.m_zcCpSet;
    x.zcVsUse = kVsUse;
    x.zcPsUse = kPsUse;
    x.zcThird = b.m_xlatCbThird;
    x.zcGen = b.m_zcGen[b.m_xlatCbThird];
    x.zcVsHash = xlatCbHash(reinterpret_cast<const uint8_t*>(d.vs.data()), kVsUse, 4096);
    x.zcPsHash = xlatCbHash(reinterpret_cast<const uint8_t*>(d.ps.data()), kPsUse, 4096);
    x.zcFetchHash = xlatCbHash(d.fetch.data(), ~0u, 768);
    return d;
  }

  // ---- the render side ----
  void open() { b().clReset(D3D12Backend::Pm4Reset); }
  uint64_t submit() {
    b().clClose(D3D12Backend::Pm4Close);
    b().clExecute(D3D12Backend::Pm4Exec);
    return b().m_nativeSubmission;
  }
  void hold(uint64_t v) { requireHr(b().m_commandQueue->Wait(gate_.Get(), v), "queue hold"); }
  void release(uint64_t v) { requireHr(gate_->Signal(v), "gate release"); }
  bool done(uint64_t ticket) {
    uint64_t completed = 0;
    requireHr(b().m_nativeImages.completionSnapshot(completed), "completion snapshot");
    return completed >= ticket;
  }
  void wait(uint64_t ticket) { requireHr(b().m_nativeImages.wait(ticket, 10000), "submission wait"); }
  // step 6 I3-B (bindless): the binder no longer overlays texture indices - the shared block it binds is the CP's own
  // bytes. A "texture set" here is the 48 selection words the test writes into the CP scratch's shared block before
  // a bind, so a re-render with a different set still changes the bound shared bytes (the fallback-staging case).
  struct Textures { std::array<uint32_t, 48> indices{}; };
  static Textures textures(uint32_t seed) {
    Textures t;
    for (uint32_t i = 0; i < 48; ++i) t.indices[i] = seed * 1000u + i;
    return t;
  }
  // The production bind of one view batch into the open recording, then the consumer draw into UAV slot `slot`.
  // slot = ~0u: the bind only (Case D).
  void consume(const Batch& d, const Textures& t, uint32_t slot) {
    auto& b = this->b();
    require(b.m_nativeRecording != nullptr, "an open recording");
    auto* cmd = b.m_nativeRecording->commands();
    cmd->SetGraphicsRootSignature(root_.Get());
    std::memcpy(b.m_xlatCBPtr + d.sharedCbOff, t.indices.data(), sizeof(t.indices));   // the CP's selection words
    b.zcBindConstants(d);
    if (slot == ~0u) return;
    require(slot < kSlots, "consumer slot");
    cmd->SetGraphicsRootUnorderedAccessView(10, out_->GetGPUVirtualAddress() + uint64_t(slot) * kSlotBytes);
    cmd->SetPipelineState(pso_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, 1, 1};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = out_.Get();
    cmd->ResourceBarrier(1, &uav);
  }
  // A GPU copy of [off, off + n) of `src` into the readback buffer, executed on the backend's queue after everything
  // already submitted there (callers release every hold first), waited for, returned as bytes.
  std::vector<uint8_t> gpuCopy(ID3D12Resource* src, uint64_t off, uint64_t n, bool fromUav) {
    require(n <= kReadbackBytes, "readback size");
    requireHr(alloc_->Reset(), "allocator reset");
    requireHr(list_->Reset(alloc_.Get(), nullptr), "list reset");
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = src;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (fromUav) {
      t.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      list_->ResourceBarrier(1, &t);
    }
    list_->CopyBufferRegion(readback_.Get(), 0, src, off, n);
    if (fromUav) {
      std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
      list_->ResourceBarrier(1, &t);
    }
    requireHr(list_->Close(), "copy close");
    ID3D12CommandList* lists[] = {list_.Get()};
    b().m_commandQueue->ExecuteCommandLists(1, lists);
    requireHr(b().m_commandQueue->Signal(fence_.Get(), ++fenceValue_), "copy signal");
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "copy event");
    requireHr(fence_->SetEventOnCompletion(fenceValue_, event), "copy event arm");
    WaitForSingleObject(event, 10000);
    CloseHandle(event);
    require(fence_->GetCompletedValue() >= fenceValue_, "copy completed");
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    void* mapped = nullptr;
    const D3D12_RANGE all{0, size_t(n)};
    requireHr(readback_->Map(0, &all, &mapped), "readback map");
    std::memcpy(bytes.data(), mapped, size_t(n));
    const D3D12_RANGE none{0, 0};
    readback_->Unmap(0, &none);
    return bytes;
  }
  std::vector<uint8_t> shadowRange(uint64_t off, uint64_t n) {
    std::lock_guard<std::mutex> lk(b().m_zcShadowMu);
    const uint8_t* s = b().m_zcShadow.data() + off;
    return std::vector<uint8_t>(s, s + n);
  }
  Counts counts() {
    auto& b = this->b();
    return {b.m_zcChecked.load(), b.m_zcCheckedXframe.load(), b.m_zcBad.load(), b.m_zcStaleGen.load(),
            b.m_zcRetired.load(), b.m_zcRetireChecked.load(), b.m_zcRetireBad.load(), b.m_zcFenceBad.load(),
            b.m_zcBinds, b.m_zcSharedWrites, b.m_zcSharedReuses, b.m_zcSharedFallbacks};
  }
  static std::string describe(const Counts& c) {
    char m[320];
    std::snprintf(m, sizeof m,
                  "(checked=%llu xframe=%llu BAD=%llu stale=%llu retired=%llu retire_checked=%llu retire_BAD=%llu "
                  "assert_BAD=%llu binds=%llu writes=%llu reuses=%llu fallbacks=%llu)",
                  (unsigned long long)c.checked, (unsigned long long)c.xframe, (unsigned long long)c.bad,
                  (unsigned long long)c.stale, (unsigned long long)c.retired, (unsigned long long)c.retireChecked,
                  (unsigned long long)c.retireBad, (unsigned long long)c.fenceBad, (unsigned long long)c.binds,
                  (unsigned long long)c.writes, (unsigned long long)c.reuses, (unsigned long long)c.fallbacks);
    return m;
  }

  // ---- what a consumer must have read (the GPU's view, from its UAV slot) ----
  static std::array<uint8_t, 512> sharedOf(const CpDraw& d, const Textures& t) {
    auto s = d.shared;
    std::memcpy(s.data(), t.indices.data(), sizeof(t.indices));   // the selection words consume() wrote
    return s;
  }
  static const uint8_t* slotOf(const std::vector<uint8_t>& out, uint32_t slot) { return out.data() + slot * kSlotBytes; }
  // the CP's views: the used VS blocks (0, 1), the used PS blocks (0, 2), the whole fetch block
  static bool viewsMatch(const uint8_t* got, const CpDraw& d) {
    const auto* vs = reinterpret_cast<const uint8_t*>(d.vs.data());
    const auto* ps = reinterpret_cast<const uint8_t*>(d.ps.data());
    return std::memcmp(got, vs, 512) == 0 && std::memcmp(got + 4096, ps, 256) == 0 &&
           std::memcmp(got + 4096 + 512, ps + 512, 256) == 0 && std::memcmp(got + 8704, d.fetch.data(), 768) == 0;
  }
  static bool sharedMatch(const uint8_t* got, const std::array<uint8_t, 512>& s) {
    return std::memcmp(got + 8192, s.data(), s.size()) == 0;
  }
  static bool allFF(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      if (p[i] != 0xFFu) return false;
    return true;
  }
  // the used views read the check mode's poison (0xFF = NaN floats)
  static bool viewsPoisoned(const uint8_t* got) {
    return allFF(got, 512) && allFF(got + 4096, 256) && allFF(got + 4096 + 512, 256) && allFF(got + 8704, 768);
  }

  void run() {
    auto& b = this->b();
    const auto texA = textures(1), texB = textures(2), texC = textures(3);
    // The first set: the drain's rotation into third 0.
    b.m_xlatThird = 2;
    rotate(true);
    check(b.m_xlatCbThird == 0 && b.m_zcFenceBad.load() == 0, "first-rotation-no-assert");

    // ---- A: a retained generation rendered twice, the first consumer outstanding ----
    const CpDraw d1 = cpDraw(1, 0x11), d2 = cpDraw(2, 0x22);
    const uint64_t cursorA = b.m_xlatCBUsed;
    hold(1);   // the queue waits for gate 1 before R1 executes
    open();
    consume(d1.batch, texA, 0);
    consume(d2.batch, texA, 1);
    const uint64_t t1 = submit();
    Counts c = counts();
    check(c.writes == 2 && c.fallbacks == 0 && c.reuses == 0 && c.binds == 2,
          "first-render-writes-each-shared-slot-once", describe(c));
    check(c.checked == 6 && c.xframe == 0 && c.bad == 0 && c.stale == 0, "first-render-consume-checks", describe(c));
    check(!done(t1), "first-consumer-outstanding");
    if (control_ == "rewrite-slot") b.m_zcSlotAny[0] = false;   // CONTROL: the write-once guard removed
    open();   // the synchronous re-render of the retained set, with changed shared bytes (new texture indices)
    consume(d1.batch, texB, 2);
    consume(d2.batch, texB, 3);
    const uint64_t t2 = submit();
    c = counts();
    check(c.writes == 2 && c.fallbacks == 2, "re-render-stages-fallbacks-never-rewrites-a-slot", describe(c));
    check(c.checked == 12 && c.xframe == 6 && c.bad == 0, "re-render-counts-checked-xframe", describe(c));
    const auto s1a = sharedOf(d1, texA), s2a = sharedOf(d2, texA);
    check(shadowRange(d1.batch.zcSharedOff, 512) == std::vector<uint8_t>(s1a.begin(), s1a.end()) &&
              shadowRange(d2.batch.zcSharedOff, 512) == std::vector<uint8_t>(s2a.begin(), s2a.end()),
          "re-render-leaves-the-first-consumers-slots-intact");
    check(!done(t1) && !done(t2), "both-consumers-outstanding");
    // The unchanged fallback reused across a recording reset (the last bound bytes: d2 + texB, this generation).
    hold(2);
    open();
    // R3, the recording that reuses the staged slice: its payload, for the retention probe (step 6 R1a: with
    // SR_RENDER_KEEP_LEAN on a slice's ownership is its PAGE, which the arena's tail and the later fallback also hold,
    // so "the slice is alive" no longer proves R3 holds it - the probe asks R3's payload directly, in both arms).
    const std::weak_ptr<const void> r3 = b.m_nativeRecording->ownership();
    if (control_ == "skip-keep") b.m_zcSharedKeptIn = b.m_nativeRecordingSerial;   // CONTROL: the keep skipped
    // R1a CONTROLS in the recording itself, for R3's keeps only: skip-page-keep (the page stamped as retained, the
    // slice not pushed) and release-at-slice-death (an upload keep retains nothing).
    if (control_ == "skip-page-keep") NativeDrawRecording::uploadKeepMutantForTest(1);
    if (control_ == "release-at-slice-death") NativeDrawRecording::uploadKeepMutantForTest(2);
    consume(d2.batch, texB, 4);
    NativeDrawRecording::uploadKeepMutantForTest(0);
    const std::weak_ptr<const void> reused = b.m_zcSharedSlice.ownership();
    const uint64_t t3 = submit();
    c = counts();
    check(c.reuses == 1 && c.fallbacks == 2 && c.writes == 2 && !reused.expired(),
          "unchanged-reuse-across-recording-reset-binds-the-staged-slice", describe(c));
    open();   // a later fallback replaces the backend's cached slice
    consume(d1.batch, texC, 5);
    const uint64_t t4 = submit();
    c = counts();
    check(c.fallbacks == 3, "later-change-stages-a-new-fallback", describe(c));
    release(1);
    wait(t2);
    requireHr(b.m_nativeImages.collect(), "collect");   // R1 and R2 (which staged the reused slice) are released
    check(done(t1) && done(t2) && !done(t3) && !done(t4), "gate-1-completes-the-first-two-recordings-only");
    check(!reused.expired() && NativeDrawRecording::payloadRetainsUpload(r3, reused),
          "fallback-slice-held-by-the-recording-that-reuses-it");
    release(2);
    wait(t4);
    requireHr(b.m_nativeImages.collect(), "collect");
    // =0: the slice's own owner is gone; on: its page may stay (the tail, the later fallback) - its consumer is gone.
    check(renderKeepLean() ? r3.expired() : reused.expired(), "fallback-slice-released-after-its-last-consumer");
    c = counts();
    check(c.bad == 0 && c.stale == 0 && c.fenceBad == 0 && c.retireBad == 0, "phase-a-no-BAD", describe(c));

    // ---- B: the retire after completion; Astra r2 P3 (replay tails follow the A set: no cursor reset) ----
    rotate(false);   // a replay tail into third 1: third 0's poison range = the finished A set's own cursor
    check(b.m_zcThirdHigh[0] == cursorA, "finished-set-records-its-own-poison-range");
    const Counts beforeRetire = counts();
    rotate(false);   // a replay tail into third 2
    rotate(false);   // a replay tail into third 0: the retire of the A generation
    c = counts();
    // ranges recorded at the binds: 3 views x 6 consumes + the 2 shared slots written
    check(c.retireChecked - beforeRetire.retireChecked == 20 && c.retireBad == beforeRetire.retireBad &&
              c.fenceBad == beforeRetire.fenceBad && c.retired - beforeRetire.retired == 2,
          "retire-after-completion-rechecks-every-range-no-BAD", describe(c));
    check(shadowRange(d1.batch.xlatVsCbOff, 512) == std::vector<uint8_t>(512, 0xFF) &&
              shadowRange(d2.batch.vsfFetchCbOff, 768) == std::vector<uint8_t>(768, 0xFF),
          "retired-range-poisoned");
    // a replay tail into third 1: third 0's EMPTY tail generation ends while the cursor is still the A set's (inside
    // third 0) - before the fix it was recorded as this generation's poison range
    rotate(false);
    check(b.m_zcThirdHigh[0] == 0 && b.m_xlatCBUsed == cursorA, "p3-empty-tail-generation-records-no-poison-range");
    rotate(false);   // a tail into third 2
    rotate(true);    // the drain into third 0 (nothing to poison; the cursor resets)
    c = counts();
    check(c.fenceBad == beforeRetire.fenceBad && c.retireBad == beforeRetire.retireBad, "phase-b-no-BAD",
          describe(c));

    // ---- C: deterministic negatives ----
    // C1 a stale view: the retired A generation's batch bound again (its views were poisoned at the retire)
    Counts n0 = counts();
    open();
    consume(d1.batch, texC, 6);
    const uint64_t t5 = submit();
    wait(t5);
    c = counts();
    check(c.bad - n0.bad == 3 && c.stale - n0.stale == 1, "stale-view-negative-detected-by-consume", describe(c));
    // the stale bind's ranges were recorded in third 0's live generation: retire it before the next case (they still
    // hold the poison they were hashed with: no BAD)
    n0 = c;
    rotate(true);
    rotate(true);
    rotate(true);
    c = counts();
    check(c.retireBad == n0.retireBad && c.fenceBad == n0.fenceBad && c.retireChecked - n0.retireChecked == 3,
          "stale-bind-ranges-retire-unchanged", describe(c));
    // C2 stale bytes: the CP rewrites a published block while its consumer may read it
    const CpDraw d3 = cpDraw(3, 0x33);
    open();
    consume(d3.batch, texA, 7);
    const uint64_t t6 = submit();
    wait(t6);
    std::array<uint8_t, 256> junk;
    junk.fill(0x5C);
    require(b.m_xlatCBGpu.write(d3.batch.xlatVsCbOff, junk.data(), junk.size()), "the stale-bytes write");
    n0 = counts();
    rotate(true);
    rotate(true);
    rotate(true);   // back into third 0: the retire compares d3's ranges
    c = counts();
    check(c.retireBad - n0.retireBad == 1 && c.fenceBad == n0.fenceBad, "stale-bytes-negative-detected-on-retire",
          describe(c));
    // C3 an outstanding consumer: third 0 reused while the submission that read its views has not completed
    const CpDraw d4 = cpDraw(4, 0x44);
    hold(3);
    open();
    consume(d4.batch, texA, 8);
    const uint64_t t7 = submit();
    n0 = counts();
    rotate(true);
    rotate(true);
    rotate(true);
    c = counts();
    check(c.fenceBad - n0.fenceBad == 1 && !done(t7), "outstanding-consumer-negative-detected-by-the-retire-assert",
          describe(c));
    release(3);
    wait(t7);
    // C3b an open recording: third 0 reused while the recording that bound its views is not submitted
    const CpDraw d5 = cpDraw(5, 0x55);
    open();
    consume(d5.batch, texA, 9);
    n0 = counts();
    rotate(true);
    rotate(true);
    rotate(true);
    c = counts();
    check(c.fenceBad - n0.fenceBad == 1, "open-recording-negative-detected-by-the-retire-assert", describe(c));
    const uint64_t t8 = submit();
    wait(t8);
    // C4 rotations without their own fence wait (none; another third's)
    n0 = counts();
    rotate(true, ~0u);
    c = counts();
    check(c.fenceBad - n0.fenceBad == 1, "rotation-without-a-fence-wait-detected", describe(c));
    n0 = c;
    rotate(true, (b.m_xlatThird + 2u) % 3u);   // waits for the third AFTER the new one
    c = counts();
    check(c.fenceBad - n0.fenceBad == 1, "rotation-waiting-for-another-third-detected", describe(c));

    // The GPU's view of every consumer (all holds released, every submission complete).
    const auto out = gpuCopy(out_.Get(), 0, kSlots * kSlotBytes, true);
    check(viewsMatch(slotOf(out, 0), d1) && viewsMatch(slotOf(out, 1), d2), "first-consumer-reads-the-cp-views");
    check(sharedMatch(slotOf(out, 0), sharedOf(d1, texA)) && sharedMatch(slotOf(out, 1), sharedOf(d2, texA)),
          "first-consumer-reads-the-original-shared-bytes");
    check(viewsMatch(slotOf(out, 2), d1) && viewsMatch(slotOf(out, 3), d2) &&
              sharedMatch(slotOf(out, 2), sharedOf(d1, texB)) && sharedMatch(slotOf(out, 3), sharedOf(d2, texB)),
          "re-render-reads-its-changed-shared-bytes");
    check(viewsMatch(slotOf(out, 4), d2) && sharedMatch(slotOf(out, 4), sharedOf(d2, texB)),
          "unchanged-reuse-reads-the-staged-bytes");
    check(viewsMatch(slotOf(out, 5), d1) && sharedMatch(slotOf(out, 5), sharedOf(d1, texC)),
          "later-fallback-reads-its-own-bytes");
    check(viewsPoisoned(slotOf(out, 6)), "stale-view-reads-the-poison");
    check(viewsMatch(slotOf(out, 7), d3) && sharedMatch(slotOf(out, 7), sharedOf(d3, texA)),
          "stale-bytes-consumer-read-the-bytes-it-bound");
    check(viewsPoisoned(slotOf(out, 8)), "outstanding-consumer-reads-the-poison-the-assert-names");

    // ---- D: P2 through the production paths - the binder's slot writes vs the retire's poison ----
    while (b.m_xlatCbThird != 0) rotate(true);
    constexpr uint32_t kRace = 1024;
    std::vector<CpDraw> race;
    race.reserve(kRace);
    for (uint32_t k = 0; k < kRace; ++k) race.push_back(cpDraw(100 + k, 0x1000u + k));
    const uint64_t lo = 0, hi = b.m_xlatCBUsed;
    open();
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> poisons{0};
    std::thread cp([&] {   // the CP thread: the retire's poison of the third the render thread binds (the mutant's case)
      while (!stop.load(std::memory_order_acquire)) {
        b.pm4WaitThirdFree(0);
        b.zcNoteThirdHigh(0, hi);
        b.zcBeginThird(0);
        poisons.fetch_add(1, std::memory_order_relaxed);
      }
    });
    const uint64_t writes0 = b.m_zcSharedWrites;
    for (uint32_t k = 0; k < kRace; ++k) {
      consume(race[k].batch, texA, ~0u);
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(40);
      while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    cp.join();
    const uint64_t raceWrites = b.m_zcSharedWrites - writes0;
    const uint64_t t9 = submit();
    wait(t9);
    const auto twin = gpuCopy(XlatCBGpuTestAccess::resource(b.m_xlatCBGpu), lo, hi - lo, false);
    char detail[96];
    std::snprintf(detail, sizeof detail, "(slot writes %llu, poisons %u, %llu bytes)", (unsigned long long)raceWrites,
                  poisons.load(), (unsigned long long)(hi - lo));
    check(raceWrites == kRace && poisons.load() >= 2 && twin == shadowRange(lo, hi - lo),
          "p2-binder-slot-writes-racing-the-retire-poison-leave-twin-equal-to-shadow", detail);
    b.zcCounterLine(" fixture");
    const Counts end = counts();
    std::printf("[XLAT_CB_ZC_TEST] COUNTS checked=%llu checked_xframe=%llu %s\n", (unsigned long long)end.checked,
                (unsigned long long)end.xframe, describe(end).c_str());
  }
};
}  // namespace sr::gfx

int xlat_cb_zerocopy_test_main(int argc, char** argv) {
  const auto started = std::chrono::steady_clock::now();
  std::string control;
  for (int i = 2; i < argc; ++i) {
    const std::string option = argv[i];
    if (option.starts_with("--control=") && control.empty()) control = option.substr(10);
    else {
      std::fprintf(stderr, "unknown or duplicate fixture option: %s\n", argv[i]);
      return 2;
    }
  }
  if (!control.empty() && control != "rewrite-slot" && control != "skip-keep" && control != "skip-page-keep" &&
      control != "release-at-slice-death") {
    std::fprintf(stderr, "unknown zero-copy control: %s\n", control.c_str());
    return 2;
  }
  _putenv_s("SR_MEM_RESOURCES", "0");
  _putenv_s("SR_LOG", "dev");
  sr::Log::init("xlat_cb_zerocopy.log");
  int rc = 0;
  try {
    {
      sr::gfx::XlatCbZeroCopyTestAccess fixture(control);
      fixture.run();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("[XLAT_CB_ZC_TEST] COMPLETE passed=%u failed=%u control=%s keep_lean=%s seconds=%.3f\n", g_passed,
                g_failed, control.empty() ? "none" : control.c_str(), sr::gfx::renderKeepLean() ? "on" : "off",
                seconds);
    rc = g_failed ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[XLAT_CB_ZC_TEST] SETUP_OR_EXECUTION_FAILED %s\n", e.what());
    rc = 2;
  }
  sr::Log::shutdown();
  return rc;
}
