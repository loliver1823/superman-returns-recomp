#include "kernel/resource_reader_observe.hpp"
#include "host_hlsl.hpp"  // step 4 7a: SR_HOST_HLSL_AOT (host HLSL compiled ahead of time)
#include "par.hpp"
#include "../kernel/sr_oracle.hpp"
#if SR_ORACLE
#include "draw_contract.hpp"   // iter-456p: the target draw-contract row
#endif
#include "d3d12_backend.hpp"
#include "present_dump_writer.hpp"  // step 4 item 4(c): SR_PRESENT_DUMP_QUEUE
#include "../kernel/host_work.hpp"    // step 4 2k(4): the dump write is host work (SR_GPU_HANG_HOST_WORK)
#include "../kernel/gpu_hang_wd.hpp"  // step 4 2k(4), Astra P2 4: the frame fence as the watchdog's causality probe
#include "pso_warm.hpp"                // step 4 2k / Astra P2 1: psoShutdown before the backend's teardown
#include "../kernel/mem_sites.hpp"    // step 4 7d: SR_MEM_SITES allocation names
#include "native_failure_diagnostics.hpp"
#include "pm4_quad_gate.hpp"  // iter-467 freezefx: SR_PM4_QUAD_STRIP
#if SR_ORACLE
#include "draw_manifest.hpp"  // iter-457 gate 3: strict suppression manifest
#include "hle_prov_ring.hpp"
#endif
#include "hle_video.hpp"   // iter-445 C4: ownPresentEnabled (single source of truth)
#include "null_renderer.hpp"
#include "xenos_state.hpp"
#include "xenos_tiling.hpp"
#include "cube_layout.hpp"
#include "volume_layout.hpp"  // step-4 lane C (iter-467 p2): 3D layout, every level (SR_TEX3D_MIPS)
#include "volume_gates.hpp"   // step-4 lane C round 2: SR_TEX3D_V2, SR_XLAT_SAMPLER_V2
#include "alpha_test.hpp"     // step-4 lane C: the Xenos alpha test (SR_XLAT_ALPHA_TEST, tools/xenosrecomp/0005)
#include "cp_simd.hpp"   // iter-466 cp lane: SR_CP_* gates
#include "resource_census.hpp"  // mem.resources step 2a: the census's fetch kind (no-op under SR_ORACLE=0)
#include "resource_memory.hpp"
#include "resource_ab.hpp"  // mem.resources frozen-pass A/B (resourceDecides / dual mode)
#include "resource_texid.hpp"  // mem.resources textures_from_resolves: the texture-identity census (oracle)
#if SR_ORACLE
#include "resource_memory_check.hpp"
#include "kernel/quit.hpp"
#endif
#include <stdexcept>
#include <intrin.h>
#if SR_ORACLE
#include <DbgHelp.h>  // 2i diag: SR_D3D12_DEBUG_STACKS
#include "../app/crash_handler.hpp"
#endif
#include "../kernel/native_drain_census.hpp"  // strand A: drain census (observe-only)
#include "../kernel/native_exec.hpp"  // strand A: the executor (pipelined fences)
#if SR_ORACLE
#include "../kernel/gpu_write_watch.hpp"  // iter-466 cp lane: write-tracked texture marks
#endif
#include "../kernel/phys_window.hpp"
#include "../kernel/devtools.hpp"   // iter-463 phase 2: RUN_CLASS.json present-dump sidecar
#include "../kernel/highfps.hpp"    // iter-464 PORT FEATURE: present sync / tearing policy
#include "../kernel/hitch_log.hpp"  // iter-466 stutter: SR_HITCH_LOG (observe-only)
#include "drain_reuse.hpp"  // step 6 R6 fix round 2: the drain reuse's adjacency, Regs refresh and census counters
#include <dxgi1_5.h>
#include "pm4_live_trace.hpp"
#include "guest_image.hpp"
#include "res_scale.hpp"   // iter-464: draw resolution scale + display settings
#include "native_zpd.hpp"  // p2.zpd: EVENT_WRITE_ZPD as host occlusion queries (SR_NATIVE_ZPD)
#include "gfx_options.hpp" // iter-466 gfx: host graphics options (sampler table)
#include "xenos_ucode.hpp"
#include "glyph_subst.hpp"            // iter-464 input lane: device-matched button prompts
#include "../kernel/host_input.hpp"   // iter-464 input lane: the active glyph set
#include "../kernel/host_automation.hpp"   // iter-467 hooks: the harness dump mark (present dump)
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <set>
#include <filesystem>
#include <system_error>

// iter-449y: resolve write-back writes GUEST memory (kernel/guest_context.hpp
// owns the definition; declared here at global scope so the gfx namespaces
// below cannot shadow `sr`).
namespace sr { namespace kernel { uint8_t* guestVAToHost(uint32_t guestVA); } }

namespace sr {
namespace gfx {

// iter-438e: 2D texture slot pool size. 1024 thrashed catastrophically the
// moment the attract/ident era started streaming real scene assets (31K LRU
// evictions/run, texBind = 80% of drain time, frame time inflated past the
// game's state-advance timegate -> boot wedged at FE tick ~7). The shaders
// index an UNBOUNDED array (g_Texture2DDescriptorHeap[]), so growing the
// root-signature range + heap is free. 3D table sits at [kTex2DSlots..+7],
// cube at [kTex2DSlots+8..+15].
static constexpr uint32_t kTex2DSlots = 8192;
// iter-464 input lane: the prompt-glyph band (icons x glyph sets, with headroom).
static constexpr uint32_t kGlyphSlots = 256;
// iter-460: SRVs of the PM4 host render targets live after the 3D/cube slots.
static constexpr uint32_t kPm4RtSrvSlots = 1024;
static constexpr uint32_t kPm4RtSrvBase = kTex2DSlots + 16;
// iter-460: the in-shader vertex fetch's staged memory (t0 space5) as a BOUNDED
// raw view - a vfetch whose computed address leaves the arena reads 0, as the
// console's memory controller keeps it inside physical memory, instead of
// faulting the GPU through an unbounded root SRV (title-city hang, frame ~1834).
static constexpr uint32_t kVsfVertexSrvSlot = kPm4RtSrvBase + kPm4RtSrvSlots;
// iter-460 perf (SR_PM4_VSF_SHM): the same view over the shared memory.
static constexpr uint32_t kVsfShmSrvSlot = kVsfVertexSrvSlot + 1;
// step 6 I3-B (bindless, Fable ruling 44): native translated draws bind this heap directly, as the legacy pass does.
// A null raw view for draws without in-shader vertex fetch, then the ring of raw vertex-fetch views: a slot is
// written once per changed staged range and reused only after the native submission that read it completed.
static constexpr uint32_t kRawNullSrvSlot = kVsfShmSrvSlot + 1;
static constexpr uint32_t kRawRingBase = kRawNullSrvSlot + 1;
static constexpr uint32_t kRawRingSlots = 65536;

// ── iter-450UE FIX B: PIN THE SWIZZLE ENCODING TO THE REAL D3D12 MACROS ─────
// xenos_state.hpp spells the Shader4ComponentMapping encoding numerically so it
// stays free of d3d12.h and its arithmetic stays unit-testable. That is only
// honest if it matches the SDK, so assert it HERE, where d3d12.h is in scope.
// Without this, the six arithmetic tests would be testing a private re-encoding
// rather than the thing D3D12 actually consumes.
static_assert(sr::gfx::texswz::kD3D12DefaultMapping ==
                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
              "texswz identity mapping drifted from the D3D12 SDK default");
static_assert(sr::gfx::texswz::toD3D12ComponentMapping(
                  sr::gfx::texswz::makeSwizzle(4, 4, 4, 0)) ==
                  D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(4, 4, 4, 0),
              "texswz encode drifted from D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING");
static_assert(sr::gfx::texswz::toD3D12ComponentMapping(
                  sr::gfx::texswz::makeSwizzle(2, 1, 0, 3)) ==
                  D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 1, 0, 3),
              "texswz encode drifted (BGRA case)");

DrawStats& getDrawStats() {
  static DrawStats s_stats;
  return s_stats;
}

namespace {
// Step 4 2k(4) / Astra P2 4: the frame fence the GPU-hang watchdog reads to decide whether open host work CAUSED a
// flat guest fence (the host queue idle or progressing) or a real GPU hang lies under it. Published once (an extra
// reference, never released: the watchdog may read it until the process ends); the last signaled value per frame.
std::atomic<ID3D12Fence*> g_wdFence{nullptr};
std::atomic<uint64_t> g_wdSignaled{0};
bool wdQueueProgress(uint64_t* completed, uint64_t* signaled) {
  ID3D12Fence* f = g_wdFence.load(std::memory_order_acquire);
  if (!f) return false;
  *signaled = g_wdSignaled.load(std::memory_order_acquire);
  *completed = f->GetCompletedValue();
  return *completed != UINT64_MAX;   // device removed: unknown
}
void wdPublishFrameFence(ID3D12Fence* f) {
  if (!f) return;
  ID3D12Fence* expected = nullptr;
  if (g_wdFence.compare_exchange_strong(expected, f, std::memory_order_acq_rel)) {
    f->AddRef();
    sr::kernel::gpuHangSetQueueProbe(&wdQueueProgress);
  }
}
void wdNoteFrameSignal(uint64_t value) { g_wdSignaled.store(value, std::memory_order_release); }

// 0x00000000 = no overlay. Otherwise packed RGBA8888 (alpha is unused).
std::atomic<uint32_t> g_splashOverlayColor{0u};
// Host present-image override (iter-273): RGBA8888 1280x720 image blitted by the
// present path. Guarded by g_hostImageMutex.
std::mutex            g_hostImageMutex;
std::vector<uint8_t>  g_hostImage;
int                   g_hostImageW = 0;
int                   g_hostImageH = 0;
std::atomic<bool>     g_hostImageSet{false};
}  // namespace

void setSplashOverlayColor(uint32_t rgbaPacked) {
  g_splashOverlayColor.store(rgbaPacked, std::memory_order_relaxed);
}
uint32_t getSplashOverlayColor() {
  return g_splashOverlayColor.load(std::memory_order_relaxed);
}
void clearSplashOverlayColor() {
  g_splashOverlayColor.store(0u, std::memory_order_relaxed);
}

void setHostPresentImage(const uint8_t* rgba, int width, int height) {
  if (!rgba || width <= 0 || height <= 0) return;
  std::lock_guard<std::mutex> lk(g_hostImageMutex);
  g_hostImage.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
  g_hostImageW = width;
  g_hostImageH = height;
  g_hostImageSet.store(true, std::memory_order_release);
}
void clearHostPresentImage() {
  g_hostImageSet.store(false, std::memory_order_release);
}
bool getHostPresentImage(std::vector<uint8_t>& out, int& width, int& height) {
  if (!g_hostImageSet.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lk(g_hostImageMutex);
  if (g_hostImage.empty()) return false;
  out = g_hostImage;
  width = g_hostImageW;
  height = g_hostImageH;
  return true;
}

}  // namespace gfx
}  // namespace sr

#ifdef _WIN32

#include "../app/log.hpp"
#include "kernel/guest_context.hpp"
#include "shader_cache.hpp"
#include "vfetch_pair_rule.hpp"  // iter-458 H-PAIR: the guest's own pairing rule
#include "kernel/perf_stats.hpp"     // iter-464 perf: SR_PERF_STATS present stamps
#include "kernel/ab_toggle.hpp"      // iter-467 native-b: in-roll A/B of SR_NB_BIND_MEMO (TOOLKIT 1.8d)
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <d3dcompiler.h>

namespace sr { namespace gfx {
void pm4NoteZeroIb(const GuestDrawCommand& c, const uint8_t* ib, int win);  // d3d12_pm4_edram.cpp
} }

// iter-407 PLATFORM FIX (SR_GPU_PHYS_A): GPU physical addresses resolve through
// a PHYSICAL window, not 0x80000000.
//
// Identical defect to iter-404g, which fixed it in the PM4 engine only. Per
// ENGINE_MAP 6e the physical heaps are 0xA0000000/0xC0000000/0xE0000000 while
// 0x80000000 is a VIRTUAL heap, and our harness keeps them as distinct host
// regions - so every one of these reads landed on memory the guest never wrote.
//
// MEASURED (iter-406d): the movie's Y plane (fmt2 1280x720 @ phys 0x0F7C0000)
// and chroma planes decoded to ALL-BLACK, while the WMV3 decoder demonstrably
// emits a full 1382400-byte frame every ~35ms. The decoder was writing through
// the 0xA window and the texture cache was reading the 0x8 alias.
//
// Scope stays GPU-side (texture fetch, index buffers, resolve dest) exactly as
// iter-404g did; guest load/store semantics are untouched, which is the
// distinction that made iter-404g safe where iter-373's aliasGuestVA version
// regressed the trunk.
static inline const uint8_t* gpuPhysToHost(uint32_t phys) {
  static const bool s_physA = [] {
    // DEFAULT-OFF: console-correct by the ENGINE_MAP 6e model and by analogy
    // with iter-404g, but it did NOT fix what it was written for - the video
    // planes stayed black with it on - so it has ZERO measured benefit while
    // touching five GPU read paths. Opt in with SR_GPU_PHYS_A=1 when there is
    // a consumer that demonstrably needs it. Correct-looking and beneficial are
    // different claims; iter-373 is the standing reminder.
    const char* v = std::getenv("SR_GPU_PHYS_A");
    return v && v[0] == '1';
  }();
  // iter-460: the allocators' own records first (kernel/phys_window.hpp
  // gpuWindow, SR_PM4_OWNER_WINDOW): when exactly one window's grant covers the
  // address, that window holds the GPU's bytes. Our 0x8 window at low physical
  // addresses is the XEX image and the main heap, while the game's 224 MB world
  // pool is a physical grant at 0x9D76xxxx-0xAB75xxxx: its textures at phys
  // 0x02xxxxxx read as EXECUTABLE bytes through 0x8 (the title city's 64^3
  // lighting volume at 0x02288000 = VA 0x82288000, inside the image). The shared
  // memory and the in-shader vertex fetch already choose this way; this makes
  // every other GPU read (3D / cube / array textures, the CPU texture decoder,
  // translated vertex and index buffers, CPU resolves) agree with them.
  static const bool s_ownerReads = [] {
    const char* e = std::getenv("SR_GPU_OWNER_READS");
    return !e || e[0] != '0';
  }();
  // iter-460 (SR_CB_WINDOW): data inside a command buffer (DrawPrimitiveUP
  // vertices, resolve rectangles) is read where the command processor read that
  // buffer, whatever else claims the page.
  if (sr::kernel::physwin::commandBufferWindowEnabled()) {
    const uint32_t p = phys & 0x1FFFFFFFu;
    const int cw = sr::kernel::physwin::commandBufferWindow(p, 1u);
    if (cw == 8) return sr::kernel::guestVAToHost(0x80000000u + p);
    if (cw == 0xA) return sr::kernel::guestVAToHost(0xA0000000u + p);
  }
  if (s_ownerReads && sr::kernel::physwin::gpuOwnerWindowEnabled()) {
    const uint32_t p = phys & 0x1FFFFFFFu;
    const int w = sr::kernel::physwin::gpuWindow(p, 1u);
    // Census of the decisions ([GPU_READ_WIN]): a legacy 0x8 read inside the
    // XEX image, or on a page both registries claim, is the suspect class.
    static std::atomic<uint64_t> s_n[5];  // A, 8, legacy-neither, legacy-both, legacy-image
    const int gf = w ? 0 : sr::kernel::physwin::gpuGrantFlags(p, 1u);
    const bool image = !s_physA && p >= 0x02000000u && p < 0x02AE0000u;
    const int cat = w == 0xA ? 0 : w == 8 ? 1 : image ? 4 : gf ? 3 : 2;
    const uint64_t n = s_n[cat].fetch_add(1, std::memory_order_relaxed) + 1;
    if (cat >= 3 && (n <= 8 || (n & (n - 1)) == 0))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[GPU_READ_WIN] %s #%llu phys=%08X grants(heap8=%d phys8=%d physA=%d)",
                  cat == 4 ? "legacy-0x8-IN-IMAGE" : "legacy-both", (unsigned long long)n, p,
                  gf & 1, (gf >> 1) & 1, (gf >> 2) & 1);
    static std::atomic<uint64_t> s_total{0};
    const uint64_t t = s_total.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((t & (t - 1)) == 0 && t >= 1024)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[GPU_READ_WIN] census after %llu reads: ownerA=%llu owner8=%llu legacy(neither)=%llu "
                  "legacy(both)=%llu legacy-in-image=%llu",
                  (unsigned long long)t, (unsigned long long)s_n[0].load(),
                  (unsigned long long)s_n[1].load(), (unsigned long long)s_n[2].load(),
                  (unsigned long long)s_n[3].load(), (unsigned long long)s_n[4].load());
    if (w == 0xA) {
      if (const uint8_t* h = sr::kernel::guestVAToHost(0xA0000000u + p)) return h;
    } else if (w == 8) {
      return sr::kernel::guestVAToHost(0x80000000u + p);
    }
  }
  return sr::kernel::guestVAToHost((s_physA ? 0xA0000000u : 0x80000000u) +
                                   (phys & 0x1FFFFFFFu));
}


#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace sr {
namespace gfx {

static void resourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res,
                            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = res;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cmdList->ResourceBarrier(1, &barrier);
}

// ── iter-445 C4: THE OPEN COMMAND LIST ──────────────────────────────────────
// SR_HLE_FRAME_LIST, DEFAULT OFF. With the gate off presentFramebuffer is
// byte-for-byte the function it has always been.
//
// THE POINT: today the frame list is opened at the top of presentFramebuffer and
// closed at its end, so it is CLOSED while hle::execute() runs — and ~4,249 of
// the frame's ~4,250 commands execute against a closed list. Nine command types
// have no case arm at all, six of them because a closed list physically cannot
// record them: Clear, ClearF, SetRenderTarget, SetDepthStencilSurface,
// SetViewport, SetScissorRect (and Resolve, once it is published). Measured on
// the fixture: 16,316 of 27,055 commands, 60% of the stream.
//
// SR_ORACLE=0 (kernel/sr_oracle.hpp, the D3D12 backend split): the HLE renderer - the SR_PM4_LIVE=0 A/B whose only
// driver, gfx/hle_video.cpp, is compiled out - leaves this file with it: the frame list, the guest surface pool and
// its clears / resolves / write-back / luminance probe, the HLE texture / cube / volume uploads and views, the draw
// staging and batch recording (with the SR_DRAWID / SR_DRAW_MANIFEST / SR_DRAW_CONTRACT / SR_S6_* instruments), the
// surface dump, and the host-driven present (presentFramebuffer / presentPlaceholder / the front-buffer blit, the
// SR_HLE_OWN_PRESENT=0 loop in app/main.cpp). Each block below is `#if SR_ORACLE`. The PM4 path (drainDrawQueue,
// renderGuestGeometry, the resolves, pm4RenderFrame's present) and the present dump stay.
#if SR_ORACLE
bool frameListEnabled() {
  // iter-445 C7 PRE-BAKE FIX (audit, FATAL). ownPresentEnabled() MUST be part of
  // the gate itself, not merely a precondition inside ensureFrameOpen. As first
  // written, SR_HLE_FRAME_LIST=1 with SR_HLE_OWN_PRESENT=0 disabled only the
  // frame OPEN: presentFramebuffer still took the gate-ON arm (whose whole body
  // is ensureFrameOpen), all three legacy skips still fired, and the tail
  // recorded a clear, the draws and the barrier into a NEVER-RESET, CLOSED list
  // - then clExecute submitted it. Baking the gate default-ON would have turned
  // the documented present escape hatch into device corruption one env var away.
  // iter-445 C7: BAKED DEFAULT-ON after the release gate. `=0` keeps the A/B for
  // at least one further iteration - the closed-list path is NOT deleted here.
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_FRAME_LIST");
    return (!e || e[0] != '0') && sr::gfx::hle::ownPresentEnabled();
  }();
  return v;
}
#endif

// ── iter-445 C1: THE FRAME-LIST LEDGER (contract in d3d12_backend.hpp) ──────
const char* D3D12Backend::clSiteName(uint32_t s) {
  static const char* k[SiteCount] = {
      "create", "initClose", "xlatReset", "xlatClose", "xlatExec", "geoReset",
      "copyRtClose", "copyRtExec", "resAsyncClose", "resAsyncExec",
      "resSyncClose", "resSyncExec", "resReset", "frameOpenAlloc", "frameOpen",
      "hostImgClose", "hostImgExec", "geoBranchClose", "geoBranchExec",
      "frameTailClose", "frameTailExec", "guardDiscard", "probe",
      "pm4Close", "pm4Exec", "pm4Reset"};
  return s < SiteCount ? k[s] : "?";
}

void D3D12Backend::clNoteTid(int site) {
  const unsigned long t = (unsigned long)GetCurrentThreadId();
  if (!m_clTid) { m_clTid = t; return; }
  if (m_clTid != t) {
    if (++m_clTidMismatches <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[FRAME_LIST] TID MISMATCH: command list touched from tid %lu, "
          "expected %lu at site %s — a D3D12 command list is thread-affine", t, m_clTid,
          site >= 0 ? clSiteName(uint32_t(site)) : "-");
  }
}

void drainReuseCadenceLine(const char* when);  // step 6 R6 bake (beside reuseReport)
#if SR_ORACLE
void xlatDropCadenceLine(const char* when);    // step 6 drop fix (beside it)
#endif

HRESULT D3D12Backend::openNativeRecording() {
  const HRESULT collected = m_nativeImages.collect();
#if SR_ORACLE
  readerObserveCompletion();
#endif
  if (FAILED(collected)) return collected;
  try {
    auto recording = std::make_unique<NativeDrawRecording>();
    const HRESULT hr = recording->begin(m_device.Get());
    if (FAILED(hr)) return hr;
    hdrProbeCollect();
    hdrProbeDiscardUnsubmitted();
#if SR_ORACLE
    resourceTextureProbeCollect();
    resourceTextureProbeDiscard();
#endif
    m_commandList2.Reset();
    m_commandList = recording->commands();
    m_nativeRecording = std::move(recording);
    ++m_nativeRecordingSerial;   // step 6 I10p: the recording's identity (its kept shared slice; check mode's retire)
    m_rawRingRecStart = m_rawRingWrites;  // step 6 I3-B: this recording's raw-ring writes start here
#if SR_ORACLE
    m_i3bRecBinds.clear();  // step 6 I3-B: a discarded recording's binds were never executed
    if (sr::kernel::reader_observe::enabled())
      m_readerRecording = sr::kernel::reader_observe::recording(m_readerStream, m_nativeRecording->ownership(),
          uint32_t(NativeDrawRecording::ownershipMetadataBytes()));
#endif
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

[[noreturn]] void D3D12Backend::nativeRecordingFail(const char* stage, HRESULT hr) {
  sr::Log::EmergencyScope emergency;
  SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_RECORDING] FAILED stage=%s hr=0x%08X ticket=%llu",
              stage, unsigned(hr), (unsigned long long)m_nativeSubmission);
  reportNativeGpuFailure(m_device.Get(), m_factory.Get(), stage, hr);
  // A failed queue signal may follow execution. Leave ownership quarantined;
  // never continue to present/convert/reclaim or unwind those GPU references.
  std::_Exit(3);
}

void D3D12Backend::keepNativeImage(const NativeImageLease& image) {
  const HRESULT hr = m_nativeRecording ? m_nativeRecording->keep(image) : E_UNEXPECTED;
  if (FAILED(hr)) nativeRecordingFail("retain-image", hr);
}

void D3D12Backend::waitNativeFence(ID3D12Fence* fence, uint64_t value, HANDLE event, const char* stage) {
  if (!fence || !event || !value) nativeRecordingFail(stage, E_INVALIDARG);
  uint64_t completed = fence->GetCompletedValue();
  if (completed == UINT64_MAX) nativeRecordingFail(stage, DXGI_ERROR_DEVICE_REMOVED);
  if (completed >= value) return;
  const HRESULT hr = fence->SetEventOnCompletion(value, event);
  if (FAILED(hr)) nativeRecordingFail(stage, hr);
  const DWORD waited = WaitForSingleObject(event, 10000);
  const DWORD waitError = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
  completed = fence->GetCompletedValue();
  if (completed == UINT64_MAX) nativeRecordingFail(stage, DXGI_ERROR_DEVICE_REMOVED);
  if (waited != WAIT_OBJECT_0 || completed < value)
    nativeRecordingFail(stage, waited == WAIT_FAILED ? HRESULT_FROM_WIN32(waitError) : HRESULT_FROM_WIN32(WAIT_TIMEOUT));
}

void D3D12Backend::clReset(ClSite s) {
  // iter-445 C7: RE-BASE THE TID EXPECTATION AT THE FIRST FRAME OPEN, whichever
  // path opens it. It was latched at the first TOUCH, which is the MAIN thread
  // inside init(), while every steady-state lifecycle call runs on the detached
  // render thread — so tidMismatch could never read 0 on a live roll and the
  // criterion would fail on a perfectly healthy run.
  // ⚠ MEASURED: the first cut put this in beginFrame(), which ONLY the gated
  // path calls. The gate-OFF control roll therefore still reported
  // tidMismatch=21780 = exactly 3 lifecycle calls x 7,260 presents — the
  // instrument lying on the A/B path while reading clean on the default one.
  if (s == FrameOpen && !m_clTidRebased) { m_clTidRebased = true; m_clTid = 0; }
  clNoteTid(int(s));
  m_clSiteHits[s]++;
  // Publish Recording only after creation of its owned list/allocator succeeds.
  const HRESULT hl = openNativeRecording();
  if (SUCCEEDED(hl)) {
    m_clState = ClState::Recording;
    m_clOpens++;
  } else {
    m_clState = ClState::Closed;
    if (++m_clResetFails <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[FRAME_LIST] OPEN FAILED site=%s list=0x%08X",
          clSiteName(s), (unsigned)hl);
    nativeRecordingFail("open", hl);
  }
}

void D3D12Backend::clClose(ClSite s) {
  clNoteTid(int(s));
  m_clSiteHits[s]++;
  // Close() returns E_FAIL when the list was put into an error state by an
  // illegal recording — which is precisely the failure the open-list refactor
  // can introduce, and checking it costs nothing.
  const HRESULT hr = m_nativeRecording ? m_nativeRecording->close() : E_UNEXPECTED;
  if (FAILED(hr) && ++m_clCloseFails <= 4)
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[FRAME_LIST] CLOSE FAILED site=%s hr=0x%08X — the list was put into "
        "an error state by an illegal recording", clSiteName(s), (unsigned)hr);
  m_clState = ClState::Closed;
  m_clCloses++;
  // iter-445 C7 PRE-BAKE FIX (audit): latch it. C3's doctrine - a dropped frame
  // is a visible glitch, a submitted half-frame is a corrupted device - was
  // enforced in the guard and violated in the normal tail, which called
  // clExecute unconditionally straight after this.
  m_clLastCloseOk = SUCCEEDED(hr);
}

#if SR_ORACLE
void D3D12Backend::readerObserveCompletion() {
  if (!sr::kernel::reader_observe::enabled()) return;
  uint64_t completed = 0;
  const HRESULT hr = m_nativeImages.completionSnapshot(completed);
  sr::kernel::reader_observe::completion(m_readerStream, completed, SUCCEEDED(hr));
}
#endif

void D3D12Backend::clExecute(ClSite s) {
  clNoteTid(int(s));
  if (!m_clLastCloseOk) {
    // The list is in an error state; submitting it escalates one illegal
    // recording into a removed device. Drop the frame and SAY SO.
    m_clSkippedSubmits++;
    if (m_clSkippedSubmits <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[FRAME_LIST] SUBMIT SKIPPED #%llu site=%s - the preceding Close "
          "FAILED, so this list is poisoned. Dropping the frame beats "
          "corrupting the device.",
          (unsigned long long)m_clSkippedSubmits, clSiteName(s));
    nativeRecordingFail("submit-after-failed-close", E_FAIL);
  }
  m_clSiteHits[s]++;
  const HRESULT hr = m_nativeRecording ? m_nativeRecording->submit(m_nativeImages, m_nativeSubmission) : E_UNEXPECTED;
  if (FAILED(hr)) nativeRecordingFail("submit", hr);
  // step 6 I3-B: the raw-ring slots this recording wrote are reused only after this submission completes
  if (m_rawRingWrites > m_rawRingRecStart)
    m_rawRingSegments.push_back({m_rawRingRecStart, m_rawRingWrites, m_nativeSubmission});
  m_rawRingRecStart = m_rawRingWrites;
#if SR_ORACLE
  zcSubmitted();   // step 6 I10p check mode: the thirds whose views this recording bound -> its ticket
  i3bSubmitted(m_nativeSubmission);  // step 6 I3-B closing check
  if (m_nbReplay) nbReplaySubmitted();
  if (sr::kernel::reader_observe::enabled()) {
    sr::kernel::reader_observe::submitted(m_readerRecording, m_nativeRecording->ownership(), m_nativeSubmission);
    readerObserveCompletion();
  }
#endif
  hdrProbeSubmitted(m_nativeSubmission);
  hdrProbeCollect();
#if SR_ORACLE
  resourceTextureProbeSubmitted(m_nativeSubmission);
  resourceTextureProbeCollect();
#endif
  m_clExecutes++;
  if (m_clExecutes <= 4 || m_clExecutes % 1000 == 0) drainReuseCadenceLine("");
#if SR_ORACLE
  if (m_clExecutes <= 4 || m_clExecutes % 1000 == 0) xlatDropCadenceLine("");   // step 6 drop fix
#endif
  if (m_clExecutes <= 4 || m_clExecutes % 1000 == 0) zcCounterLine("");   // step 6 I10p
  if (m_clExecutes <= 4 || m_clExecutes % 1000 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_RECORDING] submitted=%llu ticket=%llu pending=%zu site=%s constants=%llu constant_live_bytes=%llu vertices=%llu indices=%llu geometry_live_bytes=%llu samplers=%llu sampler_versions=%zu texture_draws=%llu texture_refs=%llu texture_pages=%zu texture_table_hits=%llu texture_table_misses=%llu texture_table_migrations=%llu texture_table_evictions=%llu texture_table_reuses=%llu raw_views=%llu raw_live_bytes=%llu constant_reuses=%llu",
                (unsigned long long)m_clExecutes, (unsigned long long)m_nativeSubmission,
                m_nativeImages.stats().pending, clSiteName(s),
                (unsigned long long)m_nativeConstantDraws, (unsigned long long)m_nativeConstants.liveBytes(),
                (unsigned long long)m_nativeVertexBindings, (unsigned long long)m_nativeIndexBindings,
                (unsigned long long)m_nativeGeometry.liveBytes(), (unsigned long long)m_nativeSamplerBindings,
                m_nativeSamplers.liveVersions(), (unsigned long long)m_nativeTextureDraws,
                (unsigned long long)m_nativeTextureBindings, m_nativeTextures.livePages(),
                (unsigned long long)m_nativeTextures.tableHits(), (unsigned long long)m_nativeTextures.tableMisses(),
                (unsigned long long)m_nativeTextures.tableMigrations(), (unsigned long long)m_nativeTextures.tableEvictions(),
                (unsigned long long)m_nativeTextureReuses,
                (unsigned long long)m_nativeTextures.rawCreates(),
                (unsigned long long)m_nativeRawGeometry.liveBytes(),
                (unsigned long long)m_nativeConstantReuses);
  if (m_clExecutes <= 4 || m_clExecutes % 1000 == 0) i3bCensusLine("");
}

// step 6 I3-B (bindless, Fable ruling 44): the hit counter (direct_draws > 0), table_creates must stay 0. At the
// submission cadence, and once at the end (`why`: shutdown, or a replay's end - a short trace's cadence lines can all
// precede its draws).
void D3D12Backend::i3bCensusLine(const char* why) {
  char check[192] = "";
#if SR_ORACLE
  std::snprintf(check, sizeof(check), " checked_binds=%llu bad_in_flight=%llu bad_before_submit=%llu legacy_refs=%llu",
                (unsigned long long)m_i3bChecked, (unsigned long long)m_i3bBadInFlight,
                (unsigned long long)m_i3bBadOpen, (unsigned long long)m_i3bLegacyRefs);
#endif
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[I3_BINDLESS] direct_draws=%llu table_creates=%llu raw_ring_writes=%llu raw_ring_waits=%llu "
              "slot_cow=%llu exhaust_waits=%llu%s%s",
              (unsigned long long)m_i3bDirectDraws,
              (unsigned long long)(m_nativeTextures.tableMisses() + m_nativeTextures.tableMigrations()),
              (unsigned long long)m_rawRingWrites, (unsigned long long)m_i3bRawWaits,
              (unsigned long long)m_i3bSlotCow, (unsigned long long)m_i3bExhaustWaits, check, why);
}

void D3D12Backend::recordGuard(ClSite s) {
  if (m_clState == ClState::Recording) return;
  if (++m_clViolations <= 4)
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[FRAME_LIST] VIOLATION site=%s: recording into a CLOSED list",
        clSiteName(s));
}

// iter-445 C2: drain the debug layer's message queue into OUR log. The
// verification standard for the whole refactor sequence is "the message SET is
// unchanged versus the recorded baseline" — not "zero errors", because this
// renderer may well have pre-existing layer complaints and a sequence that
// demanded zero would either stall or tempt a fudge. So: log each DISTINCT
// message once with its severity and a repeat count, and let the commit message
// carry the set verbatim.
void D3D12Backend::pm4Crumb(uint32_t batch, bool after) {
  if (!m_dredOn || m_crumbFailed) return;
  if (!m_commandList2 && FAILED(m_commandList.As(&m_commandList2))) {
    m_crumbFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[D3D12_CRUMB] current list interface unavailable");
    return;
  }
  if (!m_crumbBuf) {
    // CPU-visible (write-back, system memory) so it stays readable after the
    // device is removed.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mp = nullptr;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&m_crumbBuf))) ||
        FAILED(m_crumbBuf->Map(0, nullptr, &mp)) || !mp) {
      m_crumbFailed = true;
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[D3D12_CRUMB] breadcrumb buffer unavailable");
      return;
    }
    m_crumbs = static_cast<volatile uint32_t*>(mp);
    for (int i = 0; i < 4; i++) m_crumbs[i] = 0xFFFFFFFFu;
  }
  const D3D12_GPU_VIRTUAL_ADDRESS va = m_crumbBuf->GetGPUVirtualAddress() + (after ? 8u : 0u);
  const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p[2] = {{va, uint32_t(m_frameEpoch)}, {va + 4u, batch}};
  const D3D12_WRITEBUFFERIMMEDIATE_MODE m[2] = {
      after ? D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT : D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
      after ? D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT : D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN};
  m_commandList2->WriteBufferImmediate(2, p, m);
}

bool D3D12Backend::checkDeviceRemoved(const char* where) {
  if (!m_device) return false;
  const HRESULT reason = m_device->GetDeviceRemovedReason();
  if (reason == S_OK) return false;
  if (m_removalReported) return true;
  m_removalReported = true;
  SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_REMOVED] at %s: reason=0x%08X (DRED %s)", where,
              (unsigned)reason, m_dredOn ? "on" : "off - rerun with SR_D3D12_DRED=1");
  if (m_crumbs) {
    const uint32_t sf = m_crumbs[0], sb = m_crumbs[1], ff = m_crumbs[2], fb = m_crumbs[3];
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[D3D12_CRUMB] last draw STARTED frame=%u batch=%u, last FINISHED frame=%u batch=%u "
                "(this frame=%u, %zu batches)",
                sf, sb, ff, fb, uint32_t(m_frameEpoch), m_drawBatches.size());
    for (const CrumbFrame& cf : m_crumbHist) {
      if (cf.frame != sf) continue;
      for (uint32_t bi = sb > 3u ? sb - 3u : 0u; bi <= sb + 2u && bi < cf.b.size(); bi++) {
        const CrumbBatch& b = cf.b[bi];
        SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[D3D12_CRUMB]   frame %u batch %u%s: vs=%016llX ps=%016llX count=%u vsf=%d "
                    "point=%d topo=%u vbOff=%u ibOff=%u",
                    sf, bi, bi == sb ? " <== HUNG" : "", (unsigned long long)b.vs,
                    (unsigned long long)b.ps, b.count, b.vsf ? 1 : 0, b.point ? 1 : 0, b.topo,
                    b.vbOff, b.ibOff);
      }
    }
  }
  if (!m_dredOn) return true;
  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(m_device.As(&dred))) return true;
  // DRED 1.2: the marker (breadcrumb context) nearest the first incomplete op.
  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc1{};
  if (SUCCEEDED(dred.As(&dred1)) && SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc1))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc1.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
      const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0u;
      if (done >= n->BreadcrumbCount) continue;
      for (UINT i = 0; i < n->BreadcrumbContextsCount; i++) {
        const D3D12_DRED_BREADCRUMB_CONTEXT& c = n->pBreadcrumbContexts[i];
        if (c.BreadcrumbIndex + 12u < done || c.BreadcrumbIndex > done + 12u) continue;
        char narrow[160] = {};
        for (int k = 0; k < 159 && c.pContextString && c.pContextString[k]; k++)
          narrow[k] = char(c.pContextString[k]);
        SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_DRED]   marker@op[%u]%s: %s", c.BreadcrumbIndex,
                    c.BreadcrumbIndex <= done ? "" : " (after)", narrow);
      }
    }
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
    uint32_t lists = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && lists < 64;
         n = n->pNext, lists++) {
      const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0u;
      if (done >= n->BreadcrumbCount) continue;  // this list finished
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[D3D12_DRED] list '%s' queue '%s': %u/%u ops completed - in flight:",
                  n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?",
                  n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "?", done,
                  n->BreadcrumbCount);
      const uint32_t lo = done > 6u ? done - 6u : 0u;
      const uint32_t hi = std::min(n->BreadcrumbCount, done + 4u);
      for (uint32_t i = lo; i < hi; i++)
        SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_DRED]   op[%u] = %d%s", i,
                    int(n->pCommandHistory[i]), i == done ? "   <-- first incomplete" : "");
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_DRED] page fault VA=0x%016llX",
                (unsigned long long)pf.PageFaultVA);
    uint32_t k = 0;
    for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a && k < 16;
         a = a->pNext, k++)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_DRED]   live alloc near fault: '%s' type=%d",
                  a->ObjectNameA ? a->ObjectNameA : "?", int(a->AllocationType));
    k = 0;
    for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a && k < 16;
         a = a->pNext, k++)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_DRED]   FREED alloc near fault: '%s' type=%d",
                  a->ObjectNameA ? a->ObjectNameA : "?", int(a->AllocationType));
  }
  return true;
}

void D3D12Backend::drainDebugLayer() {
  if (!m_infoQueue) return;
  const UINT64 n = m_infoQueue->GetNumStoredMessages();
  for (UINT64 i = 0; i < n; i++) {
    SIZE_T len = 0;
    if (FAILED(m_infoQueue->GetMessage(i, nullptr, &len)) || !len) continue;
    if (len > 8192) len = 8192;
    std::vector<uint8_t> buf(len);
    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
    if (FAILED(m_infoQueue->GetMessage(i, msg, &len))) continue;
    m_dbgMessages++;
    if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
        msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR)
      m_dbgErrors++;
    else if (msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
      m_dbgWarnings++;
    // Dedupe on the message ID so a per-frame complaint reports once, not
    // 60 times — a flooded log is an unread log.
    const uint32_t id = (uint32_t)msg->ID;
    if (m_dbgSeen.insert(id).second) {
      static const char* kSev[] = {"CORRUPTION", "ERROR", "WARNING", "INFO",
                                   "MESSAGE"};
      const uint32_t sv = (uint32_t)msg->Severity;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[D3D12_DEBUG] %s id=%u: %.*s",
          sv < 5 ? kSev[sv] : "?", id,
          (int)(msg->DescriptionByteLength > 500 ? 500
                                                 : msg->DescriptionByteLength),
          msg->pDescription ? msg->pDescription : "");
    }
  }
  if (n) m_infoQueue->ClearStoredMessages();
}

#if SR_ORACLE
void D3D12Backend::frameListReport(uint64_t presents) {
  drainDebugLayer();
  // iter-445 C7 PRE-BAKE FIX (audit, FATAL): m_legacyExecutes was DECLARED and
  // PRINTED and never written - `legacyExec=0` was a constant that could not
  // fail, and it was quoted as MEASURED pass-criterion 2 of the C4 commit. The
  // data was already being collected one field over; it just was not summed.
  // (feedback_measure_dont_assert: never quote a number no instrument produced.)
  m_legacyExecutes =
      m_clSiteHits[HostImgClose] + m_clSiteHits[HostImgExec] +
      m_clSiteHits[GeoBranchClose] + m_clSiteHits[GeoBranchExec] +
      m_clSiteHits[GeoReset] + m_clSiteHits[CopyRtClose] +
      m_clSiteHits[CopyRtExec] + m_clSiteHits[ResolveAsyncClose] +
      m_clSiteHits[ResolveAsyncExec] + m_clSiteHits[ResolveSyncClose] +
      m_clSiteHits[ResolveSyncExec] + m_clSiteHits[ResolveReset];
  char sites[512];
  int o = 0;
  for (uint32_t i = 0; i < SiteCount && o < 440; i++)
    if (m_clSiteHits[i])
      o += snprintf(sites + o, sizeof(sites) - o, " %s=%llu", clSiteName(i),
                    (unsigned long long)m_clSiteHits[i]);
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FRAME_LIST] opens=%llu closes=%llu executes=%llu | init=(%u,%u,%u) "
      "delta=(%llu,%llu,%llu) presents=%llu | violations=%llu resetFails=%llu "
      "closeFails=%llu discards=%llu tidMismatch=%llu | dbgLayer=%s "
      "dbgMsgs=%llu err=%llu warn=%llu | windowCmds=%llu legacyExec=%llu "
      "deviceRemoved=%d skipSubmit=%llu geoDrop=%llu/%lluv "
      "| sites:%s",
      (unsigned long long)m_clOpens, (unsigned long long)m_clCloses,
      (unsigned long long)m_clExecutes, m_clInitOpens, m_clInitCloses,
      m_clInitExecutes, (unsigned long long)(m_clOpens - m_clInitOpens),
      (unsigned long long)(m_clCloses - m_clInitCloses),
      (unsigned long long)(m_clExecutes - m_clInitExecutes),
      (unsigned long long)presents, (unsigned long long)m_clViolations,
      (unsigned long long)m_clResetFails, (unsigned long long)m_clCloseFails,
      (unsigned long long)m_clDiscards, (unsigned long long)m_clTidMismatches,
      m_dbgActive ? "active" : (m_dbgLevel ? "REQUESTED-BUT-UNAVAILABLE" : "off"),
      (unsigned long long)m_dbgMessages, (unsigned long long)m_dbgErrors,
      (unsigned long long)m_dbgWarnings, (unsigned long long)m_windowCmds,
      (unsigned long long)m_legacyExecutes, m_deviceRemoved ? 1 : 0,
      (unsigned long long)m_clSkippedSubmits,
      (unsigned long long)m_geoBatchesDropped,
      (unsigned long long)m_geoVertsDropped, o ? sites : " -");
}

// ── iter-446 R1/R2: THE GUEST SURFACE POOL ──────────────────────────────────
// Contract, proof and the measured surface table: ENGINE_MAP §6g.9.

namespace {

// Xenos ColorRenderTargetFormat / DepthRenderTargetFormat → DXGI.
//
// ⚠ TWO OF THESE ARE DELIBERATE WIDENINGS, not equivalences, and they are the
// kind of thing that must be written down or it gets rediscovered as a bug:
//   * fmt 3 `k_2_10_10_10_FLOAT` is Xenos 7e3 — a 10-bit float with a 3-bit
//     exponent that has NO DXGI equivalent at all. R16G16B16A16_FLOAT is the
//     narrowest host format that holds its range without clipping the HDR
//     values this game's primary target actually carries (§6g.7 measured the
//     primary AT fmt 3).
//   * fmt 12 `k_2_10_10_10_FLOAT_AS_16_16_16_16` is the same data already
//     declared as fp16 by the guest, so it maps to the same place honestly.
// Returning UNKNOWN is a REFUSAL, never a fallback: a wrong render-target
// format renders plausible garbage, which is the worst failure mode available.
// ⛔ iter-446 depth resolve: a DEPTH surface's RESOURCE is created TYPELESS.
// A fully-typed D32_FLOAT_S8X24_UINT resource can be a depth target and nothing
// else — it cannot be SRV'd and cannot be copied into anything a shader reads,
// which is why the resolve refused every depth source (7,982/run) and why the
// most-bound MISSING texture in the frame was the resolved depth buffer.
// Typeless lets ONE resource carry three views: DSV (typed, for rendering),
// SRV (the depth plane, for sampling), and a copy source in its own typeless
// family. See surfaceDsvFormat / surfaceDepthSrvFormat below.
DXGI_FORMAT surfaceDxgiFormat(uint32_t fmt, bool isDepth) {
  if (isDepth) {
    switch (fmt) {
      case 0: return DXGI_FORMAT_R24G8_TYPELESS;      // k_24_8
      case 1: return DXGI_FORMAT_R32G8X24_TYPELESS;   // k_24_8_FLOAT (20e4)
      default: return DXGI_FORMAT_UNKNOWN;
    }
  }
  switch (fmt) {
    case 0:  return DXGI_FORMAT_R8G8B8A8_UNORM;       // k_8_8_8_8
    case 1:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // k_8_8_8_8_GAMMA
    case 2:  return DXGI_FORMAT_R10G10B10A2_UNORM;    // k_2_10_10_10
    case 3:  return DXGI_FORMAT_R16G16B16A16_FLOAT;   // k_2_10_10_10_FLOAT (7e3)
    // ⛔ iter-446: these are SIGNED fixed point, range -32..32 (Xenia xenos.h
    // annotates "Fixed point -32...32" over both, and its own
    // GetColorDrawDXGIFormat maps them to _SNORM). We had _UNORM, so every
    // negative value clamped to 0.
    // ⚠ SNORM RECOVERS THE SIGN ONLY — the host format is ±1 while the guest's
    // is ±32, so THE RANGE IS STILL UNIMPLEMENTED and anything |v|>1 clamps.
    // The missing piece is a per-RT output scale of 1/32 in the translated
    // pixel shader with a matching x32 on the sample side; that belongs to the
    // shader-translator slice. Do NOT read this row as "done".
    case 4:  return DXGI_FORMAT_R16G16_SNORM;         // k_16_16
    case 5:  return DXGI_FORMAT_R16G16B16A16_SNORM;   // k_16_16_16_16
    case 6:  return DXGI_FORMAT_R16G16_FLOAT;         // k_16_16_FLOAT
    case 7:  return DXGI_FORMAT_R16G16B16A16_FLOAT;   // k_16_16_16_16_FLOAT
    case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;    // ..._AS_10_10_10_10
    case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;   // ..._AS_16_16_16_16
    case 14: return DXGI_FORMAT_R32_FLOAT;            // k_32_FLOAT
    case 15: return DXGI_FORMAT_R32G32_FLOAT;         // k_32_32_FLOAT
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// The TYPED depth view for rendering, and the shader-readable view of the same
// resource's depth plane. ⚠ The ENCODING is not claimed equivalent: Xenos
// k_24_8_FLOAT is a 20e4 float and this is IEEE 32-bit. The round trip is
// self-consistent (we render the depth AND sample it in the same host format),
// which is what depth reconstruction needs; a shader that reads raw depth BITS
// would still be wrong. Named, not hidden.
DXGI_FORMAT surfaceDsvFormat(uint32_t fmt) {
  switch (fmt) {
    case 0: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 1: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT surfaceDepthSrvFormat(uint32_t fmt) {
  switch (fmt) {
    case 0: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case 1: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// iter-446 S5. Default OFF: this changes WHERE the guest draws land.
bool drawToPoolEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_DRAW_TO_POOL");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
    return !e || e[0] != '0';
  }();
  return s_on;
}

// iter-446 S2. Default OFF: this changes WHEN every draw is recorded.
bool drawInOrderEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_DRAW_INORDER");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
    return !e || e[0] != '0';
  }();
  return s_on;
}

bool resolveDepthGateOn() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_RESOLVE_DEPTH");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
    return !e || e[0] != '0';
  }();
  return s_on;
}

bool surfacePoolGateOn() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_RT_POOL");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
    return !e || e[0] != '0';
  }();
  return s_on;
}

}  // namespace

bool D3D12Backend::ensureSurfaceHeaps() {
  if (m_hleRtvHeap && m_hleDsvHeap) return true;
  if (!m_device) return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kHleRtvSlots;
  if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_hleRtvHeap))))
    return false;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = kHleDsvSlots;
  if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_hleDsvHeap)))) {
    m_hleRtvHeap.Reset();
    return false;
  }
  m_hleRtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_hleDsvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SURF_POOL] heaps created: RTV x%u (inc %u), DSV x%u (inc %u)",
      kHleRtvSlots, m_hleRtvSize, kHleDsvSlots, m_hleDsvSize);
  return true;
}

D3D12Backend::HleSurface* D3D12Backend::hleSurfaceGet(
    uint32_t surfVA, uint32_t surfInfo, uint32_t colorInfo, uint32_t byteSize,
    uint32_t sizeWord, bool isDepth) {
  m_surfPoolOn = surfacePoolGateOn();
  if (!m_surfPoolOn || !m_device || surfVA < 0x1000u) return nullptr;

  auto it = m_hleSurfaces.find(surfVA);
  if (it != m_hleSurfaces.end()) return &it->second;

  const sr::gfx::hle::SurfaceDesc d =
      sr::gfx::hle::decodeSurface(surfInfo, colorInfo, byteSize, sizeWord,
                                  isDepth);
  const DXGI_FORMAT dxgi = surfaceDxgiFormat(d.format, isDepth);
  if (!d.valid || dxgi == DXGI_FORMAT_UNKNOWN) {
    // REFUSE, and say which half failed. A guessed size or format would produce
    // a resource that renders convincingly wrong content.
    if (m_surfBadHeader < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[SURF_POOL] REFUSED surf=%08X %s: %ux%u pitch=%u msaa=%u fmt=%u "
          "bytes=0x%X — decode %s, dxgi %s", surfVA, isDepth ? "DS" : "RT",
          d.width, d.height, d.pitch, d.msaa, d.format, d.bytes,
          d.valid ? "ok" : "FAILED",
          dxgi == DXGI_FORMAT_UNKNOWN ? "UNMAPPED" : "ok");
    m_surfBadHeader++;
    return nullptr;
  }
  if (!ensureSurfaceHeaps()) { m_surfNoSlot++; return nullptr; }
  const uint32_t slot = isDepth ? m_hleDsvNext : m_hleRtvNext;
  if (slot >= (isDepth ? kHleDsvSlots : kHleRtvSlots)) {
    if (m_surfNoSlot < 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[SURF_POOL] out of %s descriptors at surf=%08X — passes from here on "
          "are MISSING, not merely unbound", isDepth ? "DSV" : "RTV", surfVA);
    m_surfNoSlot++;
    return nullptr;
  }

  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  // ⛔ §6g.12: the TRUE extent (surface+36), NOT the EDRAM pitch or the
  // tile-padded row count. `pitch` is align80(width); building from it made a
  // 4x4 luminance target into an 80x16 resource.
  rd.Width = d.width;
  rd.Height = d.height;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = dxgi;
  // ⚠ MSAA IS DOWNGRADED TO 1x ON PURPOSE, and it is counted so it can never be
  // a silent difference. One measured surface is 4X on console (§6g.9); a
  // multisampled host resource would have to be resolved before it could be
  // sampled, which is a second resolve path on top of the guest's own. Content
  // is identical; edge quality is not. Revisit when the resolve arm lands.
  rd.SampleDesc.Count = 1;
  rd.SampleDesc.Quality = 0;
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rd.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (d.samples > 1) m_surfMsaaDowngrade++;

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  const D3D12_RESOURCE_STATES initial =
      isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
              : D3D12_RESOURCE_STATE_RENDER_TARGET;

  HleSurface s;
  // ⛔ CORRECTED iter-446 R6, and the reasoning had been exactly BACKWARDS.
  // This passed nullptr, justified as "a mismatched optimized value warns on
  // every clear". The opposite is true: NULL warns on EVERY clear
  // (D3D12 ids 820/821, "did not pass any clear value to resource creation —
  // the clear operation is typically slower"), while a supplied value warns
  // only on the clears that actually DIFFER. Measured the moment R6 armed:
  // dbgMsgs 0 -> 63, exactly one per clear issued (45 RT + 18 DS).
  // That matters beyond speed: iter-445 C2 established an EMPTY baseline
  // message set, and an instrument that is never quiet cannot report anything.
  // The value chosen is the MEASURED common case (rgba 0,0,0,1 and z=0 — the
  // guest's own clears, §6g.8), so the residual warnings name the genuinely
  // unusual clears instead of drowning them.
  D3D12_CLEAR_VALUE cv{};
  // ⚠ A typeless resource still needs a TYPED optimized clear value.
  cv.Format = isDepth ? surfaceDsvFormat(d.format) : dxgi;
  if (isDepth) {
    cv.DepthStencil.Depth = 0.0f;
    cv.DepthStencil.Stencil = 0;
  } else {
    cv.Color[0] = 0.0f; cv.Color[1] = 0.0f; cv.Color[2] = 0.0f; cv.Color[3] = 1.0f;
  }
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               initial, &cv,
                                               IID_PPV_ARGS(&s.res)))) {
    if (m_surfCreateFail < 8)
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[SURF_POOL] CreateCommittedResource FAILED surf=%08X %ux%u dxgi=%d",
          surfVA, d.pitch, d.height, (int)dxgi);
    m_surfCreateFail++;
    return nullptr;
  }
  s.width = d.width;
  s.height = d.height;
  s.xenosFormat = d.format;
  s.samples = d.samples;
  s.dxgi = dxgi;
  s.isDepth = isDepth;
  s.state = initial;
  s.descIndex = slot;

  if (isDepth) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_hleDsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * m_hleDsvSize;
    // The resource is TYPELESS, so the view MUST name its format explicitly —
    // passing nullptr would be an invalid-parameter error, not a default.
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = surfaceDsvFormat(d.format);
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(s.res.Get(), &dv, h);
    s.dsvFormat = dv.Format;
    m_hleDsvNext++;
  } else {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_hleRtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * m_hleRtvSize;
    m_device->CreateRenderTargetView(s.res.Get(), nullptr, h);
    m_hleRtvNext++;
  }
  m_surfCreated++;
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SURF_POOL] created surf=%08X %s %ux%u (pitch %u, padded rows %u) "
      "xfmt=%u dxgi=%d slot=%u%s",
      surfVA, isDepth ? "DS" : "RT", s.width, s.height, d.pitch, d.paddedRows,
      s.xenosFormat, (int)dxgi, slot,
      d.samples > 1 ? " (console MSAA -> 1x)" : "");

  return &m_hleSurfaces.emplace(surfVA, std::move(s)).first->second;
}

D3D12Backend::HleSurface* D3D12Backend::hleSurfaceFind(uint32_t surfVA) {
  auto it = m_hleSurfaces.find(surfVA);
  return it == m_hleSurfaces.end() ? nullptr : &it->second;
}

void D3D12Backend::hleBindRenderTarget(uint32_t index, HleSurface* s) {
  if (index >= 4u) return;
  m_hleBoundRT[index] = s;
  if (s) m_surfRtBinds++; else m_surfNullBinds++;
}

void D3D12Backend::hleBindDepthStencil(HleSurface* s) {
  m_hleBoundDS = s;
  if (s) m_surfDsBinds++; else m_surfNullBinds++;
}

// iter-446 R5b: THE RESOLVE COPY. This is the step that makes an offscreen pass
// reachable — its product stops being stale guest memory and becomes a host
// texture the sampler can actually read.
//
// WHY THE DESTINATION IS CREATED HERE AND NOWHERE ELSE (ENGINE_MAP §6g.10): a
// resolve destination's pixels are produced BY THE GPU. There is no
// Lock/Unlock for a render product, ever, so it can never arrive through the
// upload path however complete that path becomes. MEASURED before this was
// written: 105,925 resolves, 16 destinations, destKnown = 0.
//
// FORMAT: the destination is created with the SOURCE's DXGI format, which makes
// CopyResource legal by construction. That is a deliberate v1 limit, not an
// equivalence claim — a real 360 resolve also converts EDRAM tiling and format,
// and doing that properly needs a full-screen resolve pass rather than a copy.
// Every case this cannot express is REFUSED AND NAMED below, never approximated.
// ── iter-457: RESOLVE INTO A SLICE OF A STACKED DESTINATION ───────────────
// Mirrors hleResolveToTexture, with three deliberate differences:
//   1. the resource is a TEXTURE3D of `depth` slices, not a Texture2D;
//   2. the SRV is TEXTURE3D and lands in the 3D descriptor range
//      [kTex2DSlots..+7], which is where every consumer of a stacked
//      product actually looks;
//   3. the copy targets DstZ = slice, so slices accumulate independently.
//
// Everything else is copied from the 2D path ON PURPOSE, because its
// comments record hazards that were paid for: a RE-CREATE must take a
// FRESH descriptor slot (an in-flight list may still sample the old one),
// the displaced resource must be retired against the frame fence rather
// than dropped, and this writes the FRAME list so it must refuse unless
// the list is recording.
uint32_t D3D12Backend::hleResolveToVolume(HleSurface* src, uint32_t destKey,
                                          uint32_t destW, uint32_t destH,
                                          uint32_t depth, uint32_t slice,
                                          ResolveSkip& why) {
  why = ResolveSkip::None;
  if (!m_initialized || !m_xlatSrvHeap || !destKey) {
    why = ResolveSkip::Disabled; return 0; }
  if (!src || !src->res) { why = ResolveSkip::NoSource; return 0; }
  // A depth we did not read from the guest is a guess, and a wrong depth
  // samples the wrong layer SILENTLY - the worst outcome available here.
  if (!depth || depth > 64u || slice >= depth) {
    why = ResolveSkip::BadDestSize; return 0; }
  if (src->isDepth && !resolveDepthGateOn()) {
    why = ResolveSkip::DepthSource; return 0; }
  if (!destW || !destH || destW > 8192u || destH > 8192u ||
      destW > src->width || destH > src->height) {
    why = ResolveSkip::BadDestSize; return 0; }
  if (!frameListEnabled()) { why = ResolveSkip::NoFrameList; return 0; }
  if (m_clState != ClState::Recording) { why = ResolveSkip::NoList; return 0; }

  destKey &= 0x1FFFFFFFu;
  HleVol& v = m_hleVolumes[destKey];
  // ⛔ NEVER take over a volume the UPLOAD path owns. Its pixels are the
  // guest's own and complete; ours would be one slice over the top.
  if (v.res && v.uploaded && !v.fromResolve) {
    why = ResolveSkip::Disabled; return 0; }

  const DXGI_FORMAT want = src->isDepth
      ? surfaceDepthSrvFormat(src->xenosFormat) : src->dxgi;
  if (want == DXGI_FORMAT_UNKNOWN) { why = ResolveSkip::CreateFailed; return 0; }

  if (!v.res || v.w != destW || v.h != destH || v.d != depth ||
      v.fmt != want) {
    D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    td.Width = destW; td.Height = destH;
    td.DepthOrArraySize = (UINT16)depth;
    td.MipLevels = 1; td.Format = want; td.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    if (FAILED(m_device->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res)))) {
      if (!v.res) m_hleVolumes.erase(destKey);
      why = ResolveSkip::CreateFailed; return 0;
    }
    const bool recreate = (v.res != nullptr);
    if (!v.idx || recreate) {
      // Only 7 usable 3D descriptors exist (index 0 is the white dummy),
      // so unlike the 8192-slot 2D range this CAN run out. Refuse by name.
      if (m_volNextIdx > 7u) {
        if (!v.res) m_hleVolumes.erase(destKey);
        why = ResolveSkip::NoSlot; return 0;
      }
      v.idx = m_volNextIdx++;
    }
    if (recreate) retireResource(v.res);
    v.res = res;
    v.w = destW; v.h = destH; v.d = depth; v.fmt = want;
    v.fromResolve = true; v.uploaded = false;
    v.sliceMask = 0;          // a new resource holds NO written slices
    v.state = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = want;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture3D.MipLevels = 1;
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(kTex2DSlots + v.idx) * inc};
    publishXlatSrv(v.res.Get(), &sv, h);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[S6_RESOLVE3D] created STACKED destination key=%08X %ux%ux%u "
        "dxgi=%d -> 3D idx=%u", destKey, destW, destH, depth, (int)want,
        v.idx);
  }

  D3D12_RESOURCE_BARRIER b[2] = {};
  UINT nb = 0;
  auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                        D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[nb].Transition.pResource = r;
    b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[nb].Transition.StateBefore = from;
    b[nb].Transition.StateAfter = to;
    nb++;
  };
  transition(src->res.Get(), src->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(v.res.Get(), v.state, D3D12_RESOURCE_STATE_COPY_DEST);
  if (nb) m_commandList->ResourceBarrier(nb, b);
  src->state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  v.state = D3D12_RESOURCE_STATE_COPY_DEST;

  // The box takes the DESTINATION's declared extent out of the padded
  // source (same reason as the 2D path), and DstZ selects the slice. A
  // Texture3D keeps every slice in subresource 0, so the subresource index
  // stays 0 and only the Z offset moves.
  D3D12_TEXTURE_COPY_LOCATION dstL{};
  dstL.pResource = v.res.Get();
  dstL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dstL.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION srcL{};
  srcL.pResource = src->res.Get();
  srcL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcL.SubresourceIndex = 0;
  D3D12_BOX box{};
  box.left = 0; box.top = 0; box.front = 0;
  box.right = destW; box.bottom = destH; box.back = 1;
  m_commandList->CopyTextureRegion(&dstL, 0, 0, slice, &srcL, &box);
  v.sliceMask |= (1u << slice);
  return v.idx;
}

uint32_t D3D12Backend::hleResolveToTexture(HleSurface* src, uint32_t destKey,
                                           uint32_t destW, uint32_t destH,
                                           ResolveSkip& why) {
  why = ResolveSkip::None;
  if (!m_initialized || !m_xlatSrvHeap || !destKey) { why = ResolveSkip::Disabled; return 0; }
  if (!src || !src->res) { why = ResolveSkip::NoSource; return 0; }
  // ⛔ iter-446: DEPTH SOURCES ARE NOW ALLOWED. They were refused (7,982/run)
  // because a fully-typed depth resource cannot be copied into anything a
  // shader reads — and the resolved depth buffer turned out to be the MOST
  // BOUND MISSING TEXTURE in the frame (key 0CC32000 on sampler 2, the depth
  // resolve of the 1280x720 depth surface). The surface is now created TYPELESS
  // (surfaceDxgiFormat), so the destination can be created in the SAME typeless
  // family — making the copy trivially legal — and SRV'd through the depth
  // plane. `SR_HLE_RESOLVE_DEPTH` gates it separately from colour resolves so
  // the two can be attributed apart.
  if (src->isDepth && !resolveDepthGateOn()) {
    why = ResolveSkip::DepthSource;
    return 0;
  }
  // ⛔ §6g.12: REFUSE rather than fall back to the source's padded size.
  // A destination sized from the EDRAM-padded source is 8 rows too tall
  // for every 360-line target and stretches everything that samples it by
  // 2.2% — a plausible-but-wrong result, which is the failure mode this
  // entire finding is about. Also reject an extent larger than the source
  // can supply: that would mean the two disagree about what is being
  // resolved, and guessing which is right is not available to us.
  if (!destW || !destH || destW > 8192u || destH > 8192u ||
      destW > src->width || destH > src->height) {
    if (m_resolveBadDest < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[SURF_POOL] resolve REFUSED key=%08X: dest extent %ux%u vs "
          "source %ux%u", destKey, destW, destH, src->width, src->height);
    m_resolveBadDest++;
    why = ResolveSkip::BadDestSize;
    return 0;
  }
  // ⛔⛔ PRE-BAKE AUDIT FIX (iter-446). This gated on the raw `m_clState` alone,
  // which is NOT sufficient authority to write the shared frame command list.
  // `frameListEnabled()` folds in ownPresentEnabled(); with SR_HLE_OWN_PRESENT=0
  // the MAIN thread presents while the render thread still runs execute(), so
  // `m_clState == Recording` is then observing the MAIN thread's recording
  // window — and this function would record barriers and a CopyResource into a
  // list another thread is writing. Close() returns E_FAIL and the frame is
  // dropped, or the device is removed.
  //
  // This is the invariant stated at the present path (~:6146): "safe today only
  // because nothing in execute() touches the command list". THIS CHANGE is what
  // made that false — it is the first execute()-side writer of the frame list.
  // The pre-existing texture upload path avoids the same trap by using a
  // SEPARATE m_texUploadList ("the FRAME list may be mid-recording here").
  // Same shape as the two FATALs iter-445's C7 pre-bake audit caught.
  // iter-450k [RESOLVE_TRACE]: full branch visibility for one queried key —
  // the NoFrameList/NoList returns below are SILENT, and a silently skipped
  // scene-resolve copy is indistinguishable from a successful one in every
  // other instrument (the e/r/s census counts ATTEMPTS at the call site).
  static const uint32_t s_rtKey = [] {
    const char* e = std::getenv("SR_AUTOPSY_KEY");
    return e && e[0] ? (uint32_t)strtoul(e, nullptr, 16) & 0x1FFFFFFFu : 0u;
  }();
  const bool rtHit = s_rtKey && ((destKey & 0x1FFFFFFFu) == s_rtKey);
  auto rtLog = [&](const char* what) {
    static std::atomic<uint32_t> s_rtN{0};
    const uint32_t n = s_rtN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 24)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[RESOLVE_TRACE] #%u key=%08X %s | src %ux%u dxgi=%u depth=%d "
          "| dest %ux%u",
          n, destKey, what, src->width, src->height,
          (unsigned)src->dxgi, (int)src->isDepth, destW, destH);
  };
  if (!frameListEnabled()) {
    if (rtHit) rtLog("SKIP NoFrameList");
    why = ResolveSkip::NoFrameList;
    return 0;
  }
  if (m_clState != ClState::Recording) {
    if (rtHit) rtLog("SKIP NoList (clState)");
    why = ResolveSkip::NoList;
    return 0;
  }

  // ⛔ AUDIT FIX: normalise the key EXACTLY as every other user of this map
  // does. The executor registers `destKey & 0x1FFFFFFF` into s_texSlot and the
  // upload path keys m_hleTextures the same way, while this function was
  // indexing with the RAW destKey — so a key with the high window bits set
  // produced TWO entries for one texture and leaked an SRV slot, and the
  // resolve's resource could never be found by the sampler's lookup. This is
  // the iter-445 texture-identity bug's exact shape, re-introduced.
  destKey &= 0x1FFFFFFFu;
  HleTex& t = m_hleTextures[destKey];
  if (rtHit) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "COPY path | entry %s %ux%u fmt=%u slot=%u fromResolve=%d "
             "uploaded=%d willCreate=%d",
             t.res ? "EXISTS" : "ABSENT", t.w, t.h, (unsigned)t.fmt, t.slot,
             (int)t.fromResolve, (int)t.uploaded,
             (int)(!t.res || t.w != destW || t.h != destH ||
                   t.fmt != src->dxgi));
    rtLog(buf);
  }
  if (!t.res || t.w != destW || t.h != destH || t.fmt != src->dxgi) {
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = destW;    // the DESTINATION's declared extent, not the padded source
    td.Height = destH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = src->dxgi;
    td.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    if (FAILED(m_device->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&res)))) {
      // operator[] already inserted an empty entry; leaving it would make a
      // failed create look like a live-but-unbacked texture to every later
      // lookup. The NoSlot path below erases for the same reason.
      if (!t.res) m_hleTextures.erase(destKey);
      why = ResolveSkip::CreateFailed;
      return 0;
    }
    // ⛔ AUDIT FIX (iter-446 pre-bake): a RE-CREATE must not reuse the slot.
    // Rewriting descriptor `t.slot` in place mutates a descriptor that frame
    // N-1's still-in-flight command list may be sampling through the bound 2D
    // SRV table — the GPU would read a descriptor changed underneath it. Take a
    // FRESH slot for the new resource and let the old one go stale; it is
    // unreachable once s_texSlot is repointed. (The old slot is not recycled:
    // reclaiming it needs the same fence proof, and slots are cheap at 8192.)
    const bool recreate = (t.res != nullptr);
    if (!t.slot || recreate) {
      if (!texSlotAvailable()) {
        if (!t.res) m_hleTextures.erase(destKey);
        why = ResolveSkip::NoSlot;
        return 0;
      }
      t.slot = texSlotAlloc();
    }
    // ⛔ AND THE DISPLACED RESOURCE MUST OUTLIVE THE FRAMES READING IT.
    // `t.res = res` used to drop the last reference immediately; D3D12 command
    // lists hold no reference to what they read, so an in-flight frame would be
    // sampling freed memory. Retire it against the frame fence instead.
    if (recreate) { retireResource(t.res); m_resolveRecreates++; }
    t.res = res;
    // iter-450UE FIX B: the resource is being REPLACED, so every swizzled
    // view over the old one is now a stale descriptor. Invalidate here,
    // in the same statement group that swaps `res` — a view outliving its
    // resource is the one real hazard this feature adds.
    t.swizViews.clear();
    t.w = destW;
    t.h = destH;
    t.fmt = src->dxgi;
    t.fromResolve = true;
    t.uploaded = false;
    t.state = D3D12_RESOURCE_STATE_COPY_DEST;
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    // A typeless depth destination must be VIEWED through its depth plane; a
    // typeless format is not a legal SRV format.
    sv.Format = src->isDepth ? surfaceDepthSrvFormat(src->xenosFormat)
                             : src->dxgi;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    if (sv.Format == DXGI_FORMAT_UNKNOWN) {
      m_hleTextures.erase(destKey);
      why = ResolveSkip::CreateFailed;
      return 0;
    }
    auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(t.slot) * inc};
    publishXlatSrv(t.res.Get(), &sv, h);
    m_resolveTexCreated++;
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[S6_RESOLVE] created destination key=%08X %ux%u (src %ux%u padded) dxgi=%d -> slot=%u",
        destKey, t.w, t.h, src->width, src->height, (int)src->dxgi, t.slot);
  }

  D3D12_RESOURCE_BARRIER b[2] = {};
  UINT nb = 0;
  auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                        D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[nb].Transition.pResource = r;
    b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[nb].Transition.StateBefore = from;
    b[nb].Transition.StateAfter = to;
    nb++;
  };
  transition(src->res.Get(), src->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_DEST);
  if (nb) m_commandList->ResourceBarrier(nb, b);
  src->state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  t.state = D3D12_RESOURCE_STATE_COPY_DEST;

  // ⛔ §6g.12: CopyTextureRegion with an EXPLICIT BOX, not CopyResource.
  // CopyResource requires identical dimensions, and the source is deliberately
  // the EDRAM-PADDED surface while the destination is the guest's declared
  // extent — 640x368 into 640x360. The box takes the live rows and leaves the
  // padding behind, which is exactly what the console's resolve does.
  D3D12_TEXTURE_COPY_LOCATION dstL{};
  dstL.pResource = t.res.Get();
  dstL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dstL.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION srcL{};
  srcL.pResource = src->res.Get();
  srcL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcL.SubresourceIndex = 0;
  D3D12_BOX box{};
  box.left = 0; box.top = 0; box.front = 0;
  box.right = destW; box.bottom = destH; box.back = 1;
  m_commandList->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, &box);

  // ── iter-450c LUMINANCE PROBE (SR_LUM_PROBE=1, diagnostic) ──────────────
  // The HDR primary resolves TWICE per engine frame: post-world (the pyramid's
  // scene capture) and post-composite-back. Present-boundary dumps only ever
  // see the second — snapshot the source's 64x64 center HERE, in-stream,
  // while it is already in COPY_SOURCE, tagged by in-frame resolve index.
  {
    static const bool s_probeOn = [] {
      const char* e = std::getenv("SR_LUM_PROBE");
      return e && e[0] == '1';
    }();
    if (s_probeOn && !src->isDepth && src->width >= 1280u &&
        src->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
      if (!m_lumProbeTex[0]) {
        for (int i = 0; i < 2; i++) {
          D3D12_HEAP_PROPERTIES def{};
          def.Type = D3D12_HEAP_TYPE_DEFAULT;
          D3D12_RESOURCE_DESC td{};
          td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
          td.Width = 64;
          td.Height = 64;
          td.DepthOrArraySize = 1;
          td.MipLevels = 1;
          td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
          td.SampleDesc.Count = 1;
          m_device->CreateCommittedResource(
              &def, D3D12_HEAP_FLAG_NONE, &td,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
              IID_PPV_ARGS(&m_lumProbeTex[i]));
        }
      }
      if (m_lumProbeTex[0] && m_lumProbeTex[1]) {
        const uint32_t idx = m_lumProbeCount < 2u ? m_lumProbeCount : 1u;
        m_lumProbeCount++;
        D3D12_TEXTURE_COPY_LOCATION pd{}, psrc{};
        pd.pResource = m_lumProbeTex[idx].Get();
        pd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        pd.SubresourceIndex = 0;
        psrc.pResource = src->res.Get();
        psrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        psrc.SubresourceIndex = 0;
        D3D12_BOX pb{608, 328, 0, 672, 392, 1};
        m_commandList->CopyTextureRegion(&pd, 0, 0, 0, &psrc, &pb);
      }
    }
  }

  // Both ends are put back into the state their next user expects: the source
  // returns to RENDER_TARGET (the guest keeps drawing into it) and the
  // destination becomes shader-readable, which is the entire point.
  nb = 0;
  // ⚠ A depth source returns to DEPTH_WRITE, not RENDER_TARGET — putting a
  // depth resource into RENDER_TARGET is an invalid transition and the debug
  // layer would say so.
  const D3D12_RESOURCE_STATES srcHome = src->isDepth
      ? D3D12_RESOURCE_STATE_DEPTH_WRITE
      : D3D12_RESOURCE_STATE_RENDER_TARGET;
  transition(src->res.Get(), src->state, srcHome);
  transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if (nb) m_commandList->ResourceBarrier(nb, b);
  src->state = srcHome;
  t.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  m_resolveCopies++;
  // ⛔ AUDIT FINDING 3: report the GAP, not just the activity. No draw path
  // targets the pool yet, so `everDrawn` is false for every surface and this
  // equals `copies`. It can only fall when real geometry starts landing —
  // which is exactly what makes it a metric that CAN fail.
  if (!src->everDrawn) m_resolveSrcNeverDrawn++;
  // ── iter-449y: RESOLVE WRITE-BACK enqueue ────────────────────────────────
  // Historical opt-in Oracle writeback experiment. The claimed CPU exposure
  // consumer was not established: opcode 1E leaves its Lock output unused;
  // the traced adaptation chain is GPU-side. No guest-reader acceptance is
  // implied by this present-time copy (see guest_resolve_readers_20261017).
  {
    static const bool wbOn = [] {
      const char* e = std::getenv("SR_HLE_RESOLVE_WB");
      return e && e[0] == '1';
    }();
    // Historical experiment's format filter; not an identified CPU-reader set.
    const bool wbFmt = src->dxgi == DXGI_FORMAT_R32_FLOAT ||
                       src->dxgi == DXGI_FORMAT_R16G16_FLOAT ||
                       src->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM;
    if (wbOn && !src->isDepth && wbFmt && destW <= 640u && destH <= 360u)
      m_wbPending.push_back({destKey, destW, destH, src->dxgi, 0});
    // iter-450k SR_WB_PROBE=1 (diagnostic): READBACK-ONLY coverage of every
    // small non-depth dest the WB filter excludes (16F tiny RTs etc.) — the
    // per-stage zero/non-zero map of the post chain must include the whole
    // exposure family, not just the 4 B/px members. Never writes guest mem.
    static const bool s_wbProbe = [] {
      const char* e = std::getenv("SR_WB_PROBE");
      return e && e[0] == '1';
    }();
    if (s_wbProbe && !src->isDepth && !(wbOn && wbFmt) && destW <= 640u &&
        destH <= 360u)
      m_wbPending.push_back({destKey, destW, destH, src->dxgi, 1});
  }
  return t.slot;
}

// iter-449y: drain the write-back queue — GPU-read each small resolved texture
// and write its texels into guest memory at the destination key's physical
// address (both CPU windows), big-endian, linearly. Texel (0,0) lands at +0,
// without an established guest CPU consumer. Runs on the render
// thread in the present path, after the frame's list is submitted (queue order
// makes the resolve→readback sequence correct without extra fences).
void D3D12Backend::serviceResolveWriteback() {
  // iter-450k [KEYTEX]: present-time content readback of the QUERIED key's
  // texture (SR_AUTOPSY_KEY). The resolve-copy chain for 0BF36000 verifies
  // clean at every layer while the sampler still reads zero — only the dest
  // resource's own pixels can arbitrate copy-broken vs sampling-broken.
  {
    static const uint32_t s_ktKey = [] {
      const char* e = std::getenv("SR_AUTOPSY_KEY");
      return e && e[0] ? (uint32_t)strtoul(e, nullptr, 16) & 0x1FFFFFFFu : 0u;
    }();
    static std::atomic<uint32_t> s_ktN{0};
    if (s_ktKey && m_readbackBuffer && m_texUploadList && m_texUploadAlloc &&
        s_ktN.load(std::memory_order_relaxed) < 8) {
      auto it = m_hleTextures.find(s_ktKey);
      if (it != m_hleTextures.end() && it->second.res) {
        HleTex& t = it->second;
        D3D12_RESOURCE_DESC rd = t.res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 tb = 0;
        m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &tb);
        if (tb <= size_t(m_readbackRowPitch) * 2u * m_height &&
            SUCCEEDED(m_texUploadAlloc->Reset()) &&
            SUCCEEDED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) {
          D3D12_RESOURCE_BARRIER b{};
          b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b.Transition.pResource = t.res.Get();
          b.Transition.StateBefore = t.state;
          b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
          b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          if (t.state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            m_texUploadList->ResourceBarrier(1, &b);
          D3D12_TEXTURE_COPY_LOCATION cd{}, cs{};
          cd.pResource = m_readbackBuffer.Get();
          cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          cd.PlacedFootprint = fp;
          cs.pResource = t.res.Get();
          cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          cs.SubresourceIndex = 0;
          m_texUploadList->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
          std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
          if (t.state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            m_texUploadList->ResourceBarrier(1, &b);
          if (SUCCEEDED(m_texUploadList->Close())) {
            ID3D12CommandList* ls[] = {m_texUploadList.Get()};
            m_commandQueue->ExecuteCommandLists(1, ls);
            const UINT64 fv = ++m_texFenceValue;
            m_commandQueue->Signal(m_texFence.Get(), fv);
            if (m_texFence->GetCompletedValue() < fv) {
              m_texFence->SetEventOnCompletion(fv, m_texFenceEvent);
              WaitForSingleObject(m_texFenceEvent, 10000);
            }
            void* mp = nullptr;
            D3D12_RANGE rr{0, size_t(tb)};
            if (SUCCEEDED(m_readbackBuffer->Map(0, &rr, &mp))) {
              uint64_t nz = 0, total = 0;
              for (uint32_t y = 0; y < t.h; y += 8) {
                const uint8_t* row = static_cast<const uint8_t*>(mp) +
                                     size_t(y) * fp.Footprint.RowPitch;
                const uint32_t rowDw = fp.Footprint.RowPitch / 4u;
                for (uint32_t x = 0; x < rowDw; x += 8) {
                  total++;
                  if (*reinterpret_cast<const uint32_t*>(row + size_t(x) * 4u))
                    nz++;
                }
              }
              const uint32_t n = s_ktN.fetch_add(1, std::memory_order_relaxed) + 1;
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[KEYTEX] #%u key=%08X %ux%u fmt=%u slot=%u state=%d | "
                  "sampled dwords nz=%llu/%llu",
                  n, s_ktKey, t.w, t.h, (unsigned)t.fmt, t.slot, (int)t.state,
                  (unsigned long long)nz, (unsigned long long)total);
              m_readbackBuffer->Unmap(0, nullptr);
            }
          }
        }
      }
    }
  }
  if (m_wbPending.empty()) return;
  if (!m_readbackBuffer || !m_texUploadList || !m_texUploadAlloc) {
    m_wbPending.clear();
    return;
  }
  std::vector<WbPending> work;
  work.swap(m_wbPending);
  static std::atomic<uint64_t> s_wbN{0};
  for (const auto& wb : work) {
    auto it = m_hleTextures.find(wb.key);
    if (it == m_hleTextures.end() || !it->second.res) continue;
    HleTex& t = it->second;
    D3D12_RESOURCE_DESC rd = t.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT nr = 0;
    UINT64 rs = 0, tb = 0;
    m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &nr, &rs, &tb);
    if (tb > size_t(m_readbackRowPitch) * 2u * m_height) continue;
    if (FAILED(m_texUploadAlloc->Reset())) return;
    if (FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) return;
    resourceBarrier(m_texUploadList.Get(), t.res.Get(), t.state,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION cd{}, cs{};
    cd.pResource = m_readbackBuffer.Get();
    cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    cd.PlacedFootprint = fp;
    cs.pResource = t.res.Get();
    cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cs.SubresourceIndex = 0;
    m_texUploadList->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
    resourceBarrier(m_texUploadList.Get(), t.res.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE, t.state);
    m_texUploadList->Close();
    ID3D12CommandList* l[] = {m_texUploadList.Get()};
    m_commandQueue->ExecuteCommandLists(1, l);
    m_texFenceValue++;
    m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
    if (m_texFence->GetCompletedValue() < m_texFenceValue) {
      m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
      WaitForSingleObject(m_texFenceEvent, 10000);
    }
    void* mp = nullptr;
    D3D12_RANGE rr{0, size_t(tb)};
    if (FAILED(m_readbackBuffer->Map(0, &rr, &mp))) continue;
    // ── iter-450j WRITE-BACK STALENESS GUARD ─────────────────────────────
    // 3/3 rolls with WB=1 wedged at boot (no movie, parser never ran): the
    // first present's resolves are ALL-ZERO, and zero-filling 12 dest keys
    // (incl. 0D1AB000/0D29C000, which the movie pipeline binds) STOMPS
    // guest data living at those keys before they ever become real resolve
    // dests — the mirror image of capture-at-bind staleness. An all-zero
    // write-back also carries no information for the exposure seed: the
    // guest reading its own untouched memory is strictly better. Skip it.
    {
      bool anyNz = false;
      const uint32_t bpp = (wb.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u : 4u;
      for (uint32_t y = 0; y < wb.h && !anyNz; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(mp) +
                             size_t(y) * fp.Footprint.RowPitch;
        for (uint32_t x = 0; x < wb.w * (bpp / 4u); x++)
          if (*reinterpret_cast<const uint32_t*>(row + size_t(x) * 4u)) {
            anyNz = true;
            break;
          }
      }
      if (wb.probeOnly) {
        // iter-450k: readback-only stage probe — log the verdict + head
        // bytes, touch nothing.
        char hx[3 * 16 + 4] = {};
        const uint8_t* r0 = static_cast<const uint8_t*>(mp);
        for (uint32_t i = 0; i < 16u; i++)
          snprintf(hx + i * 3, 4, "%02X ", r0[i]);
        static std::set<uint32_t> s_probeLogged;
        if (s_probeLogged.insert(wb.key ^ (anyNz ? 0x80000000u : 0u)).second)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[WB_PROBE] key=%08X %ux%u dxgi=%u %s | head: %s", wb.key,
              wb.w, wb.h, (unsigned)wb.fmt,
              anyNz ? "NON-ZERO" : "all-zero", hx);
        m_readbackBuffer->Unmap(0, nullptr);
        continue;
      }
      if (!anyNz) {
        m_readbackBuffer->Unmap(0, nullptr);
        static std::atomic<uint64_t> s_wbSkipZ{0};
        const uint64_t zn = s_wbSkipZ.fetch_add(1, std::memory_order_relaxed) + 1;
        // iter-450k: first skip PER KEY always logs — the per-stage
        // zero/non-zero map of the post chain must not hide behind a
        // global cap (one replay = the whole cascade's verdict).
        static std::set<uint32_t> s_skipLogged;
        const bool firstForKey = s_skipLogged.insert(wb.key).second;
        if (firstForKey || zn <= 6 || (zn & (zn - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[RESOLVE_WB] skip #%llu key=%08X %ux%u ALL-ZERO (staleness "
              "guard: guest memory left untouched)%s",
              (unsigned long long)zn, wb.key, wb.w, wb.h,
              firstForKey ? " [first for key]" : "");
        continue;
      }
    }
    uint8_t* d8 = ::sr::kernel::guestVAToHost(0x80000000u | wb.key);
    uint8_t* dA = ::sr::kernel::guestVAToHost(0xA0000000u | wb.key);
    float texel0 = 0.0f;
    for (uint32_t y = 0; y < wb.h; y++) {
      const uint8_t* row = static_cast<const uint8_t*>(mp) +
                           size_t(y) * fp.Footprint.RowPitch;
      for (uint32_t x = 0; x < wb.w; x++) {
        uint32_t v;
        std::memcpy(&v, row + size_t(x) * 4u, 4u);
        if (x == 0 && y == 0) std::memcpy(&texel0, &v, 4u);
        // Guest layout is big-endian per COMPONENT: 32-bit texels swap whole
        // dwords (8in32); 16-bit-per-channel texels swap each half (8in16).
        const uint32_t be =
            wb.fmt == DXGI_FORMAT_R16G16_FLOAT
                ? ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u)
                : _byteswap_ulong(v);
        const size_t off = (size_t(y) * wb.w + x) * 4u;
        if (d8) std::memcpy(d8 + off, &be, 4u);
        if (dA) std::memcpy(dA + off, &be, 4u);
      }
    }
    m_readbackBuffer->Unmap(0, nullptr);
    const uint64_t n = s_wbN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 12 || n % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[RESOLVE_WB] #%llu key=%08X %ux%u texel0=%g -> guest mem",
          (unsigned long long)n, wb.key, wb.w, wb.h, texel0);
  }
}

// iter-450c: drain the luminance probe — GPU-read the two 64x64 in-frame HDR
// snapshots and log per-resolve means/ranges. Same list/fence pattern as the
// write-back drain; queue order guarantees the in-stream probe copies landed.
void D3D12Backend::serviceLumProbe() {
  const uint32_t n = m_lumProbeCount > 2u ? 2u : m_lumProbeCount;
  m_lumProbeCount = 0;
  if (!n || !m_lumProbeTex[0]) return;
  if (!m_readbackBuffer || !m_texUploadList || !m_texUploadAlloc) return;
  float mean[2][3] = {};
  float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f};
  uint32_t distinct[2] = {};
  // xeHalfToFloat is declared later in this TU; local IEEE half decode.
  const auto h2fp = [](uint16_t h) -> float {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    uint32_t bits;
    if (e == 0) {
      if (!m) { bits = s << 31; }
      else {
        int ex = -1; uint32_t mm = m;
        while (!(mm & 0x400u)) { mm <<= 1; ex--; }
        bits = (s << 31) | (uint32_t)(127 - 15 + ex + 1) << 23 |
               ((mm & 0x3FFu) << 13);
      }
    } else if (e == 31) {
      bits = (s << 31) | 0x7F800000u | (m << 13);
    } else {
      bits = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  };
  for (uint32_t i = 0; i < n; i++) {
    D3D12_RESOURCE_DESC rd = m_lumProbeTex[i]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT nr = 0;
    UINT64 rs = 0, tb = 0;
    m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &nr, &rs, &tb);
    if (FAILED(m_texUploadAlloc->Reset())) return;
    if (FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) return;
    resourceBarrier(m_texUploadList.Get(), m_lumProbeTex[i].Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION cd{}, cs{};
    cd.pResource = m_readbackBuffer.Get();
    cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    cd.PlacedFootprint = fp;
    cs.pResource = m_lumProbeTex[i].Get();
    cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cs.SubresourceIndex = 0;
    m_texUploadList->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
    resourceBarrier(m_texUploadList.Get(), m_lumProbeTex[i].Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
    m_texUploadList->Close();
    ID3D12CommandList* l[] = {m_texUploadList.Get()};
    m_commandQueue->ExecuteCommandLists(1, l);
    m_texFenceValue++;
    m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
    if (m_texFence->GetCompletedValue() < m_texFenceValue) {
      m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
      WaitForSingleObject(m_texFenceEvent, 10000);
    }
    void* mp = nullptr;
    D3D12_RANGE rr{0, size_t(tb)};
    if (FAILED(m_readbackBuffer->Map(0, &rr, &mp))) continue;
    std::set<uint64_t> seen;
    for (uint32_t y = 0; y < 64u; y++) {
      const uint8_t* row = static_cast<const uint8_t*>(mp) +
                           size_t(y) * fp.Footprint.RowPitch;
      for (uint32_t x = 0; x < 64u; x++) {
        const uint16_t* px =
            reinterpret_cast<const uint16_t*>(row + size_t(x) * 8u);
        uint64_t pk;
        std::memcpy(&pk, px, 8u);
        seen.insert(pk);
        for (int c = 0; c < 3; c++) {
          const float v = h2fp(px[c]);
          mean[i][c] += v;
          if (v < mn[i]) mn[i] = v;
          if (v > mx[i]) mx[i] = v;
        }
      }
    }
    distinct[i] = (uint32_t)seen.size();
    for (int c = 0; c < 3; c++) mean[i][c] /= 4096.0f;
    m_readbackBuffer->Unmap(0, nullptr);
  }
  static uint64_t s_pn = 0;
  const uint64_t pn = ++s_pn;
  // iter-450f: SR_TEXSLOT_WHITE=t needs a per-phase probe cadence — every 16
  // (~2-3 s) instead of every 128 (~20 s) so [WHITETOGGLE] phases carry
  // multiple probe lines each.
  static const bool s_dense = [] {
    const char* e = std::getenv("SR_TEXSLOT_WHITE");
    return e && (e[0] == 't' || e[0] == 'T');
  }();
  if (pn <= 60 || pn % (s_dense ? 16 : 128) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[LUM_PROBE] #%llu resolves=%u | res1 mean=(%.4f,%.4f,%.4f) "
        "range=[%.4f,%.4f] distinct=%u | res2 mean=(%.4f,%.4f,%.4f) "
        "range=[%.4f,%.4f] distinct=%u",
        (unsigned long long)pn, n, mean[0][0], mean[0][1], mean[0][2], mn[0],
        mx[0], distinct[0], mean[1][0], mean[1][1], mean[1][2], mn[1], mx[1],
        distinct[1]);
}
#endif

// ⛔ AUDIT FIX (iter-446 pre-bake). Contract on the declaration. Stamped with
// the value the queue is ABOUT to signal (the current frame's), so the drain
// can only free it after that frame has completed.
// ⚠ THREADING: locked, and the lock is not belt-and-braces. The RESOLVE's
// producer is single-threaded by construction (it refuses unless
// frameListEnabled(), which implies own-present, which makes the render thread
// the presenting thread) — but the TEXTURE UPLOAD paths below also retire, and
// they run from execute() on the render thread with NO such interlock, while
// drainRetired runs from moveToNextFrame which is the MAIN thread when
// own-present is off. Extending retirement to those paths is what makes the
// lock necessary; it is taken only on a re-create, which measured 0/run.
void D3D12Backend::retireResource(Microsoft::WRL::ComPtr<ID3D12Resource> res) {
  if (!res) return;
  std::lock_guard<std::mutex> lk(m_retiredMu);
  m_retired.push_back({std::move(res), m_fenceValues[m_frameIndex]});
  if (m_retired.size() > m_retiredPeak) m_retiredPeak = m_retired.size();
}

void D3D12Backend::drainRetired(uint64_t completedFence) {
  if (completedFence == UINT64_MAX) nativeRecordingFail("retired-completion", DXGI_ERROR_DEVICE_REMOVED);
#if SR_ORACLE
  // r5: cube pairs displaced by a same-key publication wait on THIS fence
  // (the frame that may still sample the old slot); cube upload contexts
  // wait on their own fences. Both drains take their own locks.
  m_cubeTx.drainGraphics(completedFence);
  m_cubeTx.drainUploads();
#endif
  std::lock_guard<std::mutex> lk(m_retiredMu);
  if (m_retired.empty()) return;
  size_t w = 0;
  for (size_t i = 0; i < m_retired.size(); i++) {
    if (m_retired[i].fence <= completedFence) { m_retiredFreed++; continue; }
    if (w != i) m_retired[w] = std::move(m_retired[i]);
    w++;
  }
  m_retired.resize(w);
}

#if SR_ORACLE
// Move a pooled surface into `to`, tracking its state. Cheap no-op when it is
// already there, which is the common case.
void D3D12Backend::hleTransition(HleSurface* s, D3D12_RESOURCE_STATES to) {
  if (!s || !s->res || s->state == to) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = s->res.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = s->state;
  b.Transition.StateAfter = to;
  m_commandList->ResourceBarrier(1, &b);
  s->state = to;
}

// ── iter-446 R6: THE GUEST'S OWN CLEAR ──────────────────────────────────────
// The host has invented clears for this renderer's whole history. This is the
// guest issuing its own, against the surfaces IT bound — the charter's natural
// path rather than a host guess.
//
// ⚠ FLAGS ARE THE 360 LAYOUT and that is not a detail: bits 0-3 select
// TARGET0..TARGET3, 0x10 is ZBUFFER and 0x20 is STENCIL. Measured values are
// 0x3F and 0x30, and 0x30 is a DEPTH+STENCIL-ONLY clear — under PC D3D9 values
// (TARGET=0x1, ZBUFFER=0x2, STENCIL=0x4) it would AND to zero and every
// depth-only clear would vanish silently. D3DDeviceClearF proves the layout by
// splitting the mask at exactly bit 4 (ENGINE_MAP §6g.8).
void D3D12Backend::hleClearBound(uint32_t flags, const float rgba[4], float z,
                                 uint8_t stencil) {
  if (!m_initialized || !m_hleRtvHeap) return;
  // Same interlock as the resolve, and for the same reason: this records into
  // the shared frame list from the render thread, so `m_clState == Recording`
  // is only OUR authority when frameListEnabled() (which folds in
  // ownPresentEnabled()) is true. Otherwise the main thread owns the list.
  if (!frameListEnabled()) { m_clearNoList++; return; }
  if (m_clState != ClState::Recording) { m_clearNoList++; return; }

  for (uint32_t i = 0; i < 4u; i++) {
    if (!(flags & (1u << i))) continue;
    HleSurface* s = m_hleBoundRT[i];
    // A target the guest asked to clear but that we never pooled is DROPPED and
    // COUNTED. Silently clearing something else — the backbuffer, say — would
    // paint an offscreen pass's clear over the picture.
    if (!s || !s->res) { m_clearUnboundRt++; continue; }
    hleTransition(s, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        m_hleRtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)s->descIndex * m_hleRtvSize;
    m_commandList->ClearRenderTargetView(h, rgba, 0, nullptr);
    m_clearRt++;
    // Provenance ring (observe-only): clears are producer events too.
#if SR_ORACLE
    if (sr::gfx::prov::enabled())
      sr::gfx::prov::ring().onClear(s, 0u, rgba,
          m_drawIdOrdinal.load(std::memory_order_relaxed));
#endif
  }

  const D3D12_CLEAR_FLAGS dsFlags = (D3D12_CLEAR_FLAGS)(
      ((flags & 0x10u) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
      ((flags & 0x20u) ? D3D12_CLEAR_FLAG_STENCIL : 0));
  if (!dsFlags) return;
  HleSurface* d = m_hleBoundDS;
  if (!d || !d->res || !m_hleDsvHeap) { m_clearUnboundDs++; return; }
  hleTransition(d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  D3D12_CPU_DESCRIPTOR_HANDLE dh =
      m_hleDsvHeap->GetCPUDescriptorHandleForHeapStart();
  dh.ptr += (SIZE_T)d->descIndex * m_hleDsvSize;
  m_commandList->ClearDepthStencilView(dh, dsFlags, z, stencil, 0, nullptr);
  m_clearDs++;
}

void D3D12Backend::hleSurfaceReport(uint64_t presents) {
  // ⚠ Ask the GATE, not m_surfPoolOn. m_surfPoolOn is only written the first
  // time a surface is requested, so a gate-ON run that saw zero SetRenderTarget
  // commands would print nothing at all — indistinguishable from the gate being
  // off, which is precisely the instrument-that-cannot-fail shape iter-445 was
  // burned by twice.
  // ── iter-450UE FIX B: the arithmetic tests, echoed at RUNTIME ─────────────
  // The authoritative tests are the static_asserts in xenos_state.hpp (they
  // cannot be skipped and fail the BUILD). This prints the same cases so a
  // verifier has a command to run and a table to read, rather than having to
  // take "it compiled" on trust. SR_HLE_TEX_SWIZZLE_TEST=1, once per process.
  {
    static const bool s_swzTest = [] {
      const char* e = std::getenv("SR_HLE_TEX_SWIZZLE_TEST");
      return e && e[0] == '1';
    }();
    static bool s_done = false;
    if (s_swzTest && !s_done) {
      s_done = true;
      using namespace sr::gfx::texswz;
      struct Case { const char* name; uint32_t got, want; };
      const Case cases[] = {
        {"1 pressStart 000R over k_8 RRRR",
         guestToHostSwizzle(0x124u, kSwizzleRRRR), 0x124u},
        {"1b   .. as a D3D12 mapping (4,4,4,0)",
         toD3D12ComponentMapping(guestToHostSwizzle(0x124u, kSwizzleRRRR)),
         uint32_t(D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(4, 4, 4, 0))},
        {"2 guest RGBA over identity host",
         guestToHostSwizzle(kSwizzleRGBA, kSwizzleRGBA), kSwizzleRGBA},
        {"3 ZERO/ONE selectors stay constant",
         guestToHostSwizzle(makeSwizzle(kSelZero, kSelOne, kSelZero, kSelOne),
                            kSwizzleRGBA),
         makeSwizzle(kSelZero, kSelOne, kSelZero, kSelOne)},
        {"3b undefined 6/7 normalise to 4/5",
         guestToHostSwizzle(makeSwizzle(6u, 7u, 6u, 7u), kSwizzleRGBA),
         makeSwizzle(kSelZero, kSelOne, kSelZero, kSelOne)},
        {"4 guest BGRA over identity host",
         guestToHostSwizzle(makeSwizzle(kSelB, kSelG, kSelR, kSelA), kSwizzleRGBA),
         makeSwizzle(kSelB, kSelG, kSelR, kSelA)},
        {"5 guest RGBA over 1-channel host",
         guestToHostSwizzle(kSwizzleRGBA, kSwizzleRRRR), kSwizzleRRRR},
        {"6 guest RGBA over 2-channel host",
         guestToHostSwizzle(kSwizzleRGBA, kSwizzleRGGG), kSwizzleRGGG},
      };
      uint32_t fails = 0;
      for (const Case& c : cases) {
        const bool ok = c.got == c.want;
        if (!ok) fails++;
        SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
            "[SWZ_TEST] %s : got=0x%03X want=0x%03X %s",
            c.name, c.got, c.want, ok ? "PASS" : "FAIL");
      }
      SR_LOG_BOOT(fails ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
          "[SWZ_TEST] %u/%zu passed", unsigned(sizeof(cases) / sizeof(cases[0])) - fails,
          sizeof(cases) / sizeof(cases[0]));
    }
  }
  // iter-450UE FIX B census. Printed UNCONDITIONALLY when the gate is on, and
  // its mode is printed with it — a census that only prints on the interesting
  // path cannot distinguish "nothing happened" from "never ran".
  {
    static const int s_swzMode = [] {
      const char* e = std::getenv("SR_HLE_TEX_SWIZZLE");
      return e ? atoi(e) : 2;   // iter-455n: MUST match the bind-path default
    }();
    if (s_swzMode >= 1)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_SWZ] mode=%d composed=%llu identity=%llu censusOnly=%llu "
          "skipMode=%llu skipResolve=%llu unknownFmt=%llu baseMismatch=%llu "
          "exhausted=%llu views=%llu nextSlot=%u/%u",
          s_swzMode,
          (unsigned long long)m_swzBindings.load(std::memory_order_relaxed),
          (unsigned long long)m_swzIdentity.load(std::memory_order_relaxed),
          (unsigned long long)m_swzCensusOnly.load(std::memory_order_relaxed),
          (unsigned long long)m_swzSkipMode.load(std::memory_order_relaxed),
          (unsigned long long)m_swzSkipResolve.load(std::memory_order_relaxed),
          (unsigned long long)m_swzUnknownFmt.load(std::memory_order_relaxed),
          (unsigned long long)m_swzBaseMismatch.load(std::memory_order_relaxed),
          (unsigned long long)m_swzExhausted.load(std::memory_order_relaxed),
          (unsigned long long)m_swzViews.load(std::memory_order_relaxed),
          m_texNextSlot, kTex2DSlots);
  }
  if (!surfacePoolGateOn()) return;
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SURF_POOL] presents=%llu | live=%zu (rtv %u/%u, dsv %u/%u) created=%llu "
      "| REFUSED badHeader=%llu noSlot=%llu createFail=%llu | binds rt=%llu "
      "ds=%llu null=%llu | msaaDowngrade=%llu | resolve copies=%llu dests=%llu | clear rt=%llu ds=%llu DROPPED(rt=%llu ds=%llu noList=%llu) | inOrder(draws=%llu frames=%llu zfuncLate=%llu clears=open%llu/tail%llu) | poolDraws=%llu noDs=%llu refused(noRt=%llu fmt=%llu) fmtPairs=%zu | land pool(V=%llu IV=%llu UP=%llu) bb(V=%llu IV=%llu UP=%llu) | srcNeverDrawn=%llu recreates=%llu retired(peak=%llu freed=%llu)",
      (unsigned long long)presents, m_hleSurfaces.size(), m_hleRtvNext,
      kHleRtvSlots, m_hleDsvNext, kHleDsvSlots,
      (unsigned long long)m_surfCreated, (unsigned long long)m_surfBadHeader,
      (unsigned long long)m_surfNoSlot, (unsigned long long)m_surfCreateFail,
      (unsigned long long)m_surfRtBinds, (unsigned long long)m_surfDsBinds,
      (unsigned long long)m_surfNullBinds,
      (unsigned long long)m_surfMsaaDowngrade,
      // ⛔ CLAIM RETRACTED by the iter-446 pre-bake audit. `m_resolveCopies` vs
      // the executor's `copies` is a TAUTOLOGY, not a cross-check: this
      // increments immediately before `return t.slot`, `t.slot` can never be 0
      // there (m_texNextSlot starts at 1 and is assigned right before t.res),
      // and the executor increments iff the returned slot is non-zero — one
      // call site, no branch between, neither ever reset. They cannot differ in
      // ANY reachable state, and commit 3f7a5da quoted their agreement as a
      // self-audit pass. It carries zero bits; read it as a plain total.
      // ✅ `dests` vs the executor's `owned` IS genuinely independent — a
      // re-create bumps this and not that — so that half stands.
      (unsigned long long)m_resolveCopies,
      (unsigned long long)m_resolveTexCreated,
      (unsigned long long)m_clearRt, (unsigned long long)m_clearDs,
      (unsigned long long)m_clearUnboundRt,
      (unsigned long long)m_clearUnboundDs,
      (unsigned long long)m_clearNoList,
      (unsigned long long)m_hleInOrderRecorded,
      (unsigned long long)m_hleInOrderFrames,
      (unsigned long long)m_depthClearZfuncLate,
      (unsigned long long)m_frameOpenClears,
      (unsigned long long)m_frameTailClears,
      (unsigned long long)m_poolDraws,
      (unsigned long long)m_poolNoDsDraws,
      (unsigned long long)m_poolRefusedNoRt,
      (unsigned long long)m_poolRefusedFmt,
      m_poolFmtVariants.size(),
      (unsigned long long)m_landByType[0][0], (unsigned long long)m_landByType[0][1],
      (unsigned long long)m_landByType[0][2], (unsigned long long)m_landByType[1][0],
      (unsigned long long)m_landByType[1][1], (unsigned long long)m_landByType[1][2],
      (unsigned long long)m_resolveSrcNeverDrawn,
      (unsigned long long)m_resolveRecreates,
      (unsigned long long)m_retiredPeak,
      (unsigned long long)m_retiredFreed);
  // r5: the cube summary is NOT emitted here. The executor owns both named
  // sites (hle_video.cpp: "periodic" in execute(), "replay-final" once in
  // replayTrace()); emitting it from this shared report is what duplicated
  // the replay line in r4.
}
#endif

#if SR_ORACLE
// step 4 lane C 2i (diag, SR_D3D12_DEBUG_STACKS=1 with SR_D3D12_DEBUG): the call stack of D3D12 ERROR 708 (a table from
// a heap that is not set) and 935 (GBV: an uninitialized root argument read) - the first 6 of each with stacks, then
// the running count at powers of two. ID3D12InfoQueue1's callback runs synchronously at the offending API call.
static std::atomic<uint32_t> g_dbgStackN[2];
static void __stdcall dbgStackCallback(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY, D3D12_MESSAGE_ID id,
                                       LPCSTR desc, void*) {
  const int k = id == D3D12_MESSAGE_ID(708) ? 0 : id == D3D12_MESSAGE_ID(935) ? 1 : -1;
  if (k < 0) return;
  const uint32_t n = g_dbgStackN[k].fetch_add(1) + 1;
  if (n > 6 && (n & (n - 1)) != 0) return;
  void* frames[28] = {};
  const USHORT nf = n <= 6 ? CaptureStackBackTrace(1, 28, frames, nullptr) : 0;
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[D3D12_STACK] id=%u #%u: %.200s", unsigned(id), n, desc ? desc : "");
  if (!nf) return;
  sr::DbgHelpLock dbg(1000);
  if (!dbg.held()) return;
  static bool s_init = false;
  if (!s_init) {
    s_init = true;
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  }
  for (USHORT i = 0; i < nf; ++i) {
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    const DWORD64 a = DWORD64(frames[i]);
    const char* name = SymFromAddr(GetCurrentProcess(), a, &disp, sym) ? sym->Name : "?";
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD ld = 0;
    const bool hasLine = SymGetLineFromAddr64(GetCurrentProcess(), a, &ld, &line) != FALSE;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[D3D12_STACK]   #%u %s+0x%llX %s:%lu", unsigned(i), name,
                (unsigned long long)disp, hasLine && line.FileName ? line.FileName : "?",
                hasLine ? (unsigned long)line.LineNumber : 0ul);
  }
}
#endif

bool D3D12Backend::init(void* hwnd, int width, int height) {
  sr::kernel::MemSiteScope memSite("D3D12Backend::init");   // step 4 7d: [MEM_SITE] names (SR_MEM_SITES, diagnostic)
  if (m_initialized) return true;
  m_width = width;
  m_height = height;
  HWND hWnd = static_cast<HWND>(hwnd);
  // render/boot-attach (docs/NATIVE_ROADMAP.md "Boot attach", SR_SHADER_LOAD_ASYNC default ON): the
  // shader cache and the runtime translator load on their own thread from here on; what stays here is
  // the probe (one directory listing) and the thread's start, so main() attaches the backend once the
  // device, swap chain and PSOs exist. The CP waits for the installed stage itself (WP1,
  // kernel/gpu_command_processor.cpp). The [SHADER_CACHE] / [DISPLAY] lines give each bound in
  // process-relative ms (msSinceProcessStart).
  const auto initT0 = std::chrono::steady_clock::now();
  const double initAt = msSinceProcessStart();
  noteBackendInitStep("the shader cache probe");  // the first step: records this thread and the start
  getShaderCache().beginBootLoad();
  noteBackendInitStep("the debug layer");

  HRESULT hr;

  // ── iter-445 C2: THE DEBUG LAYER, COMPILED IN AND GATED AT RUNTIME ────────
  // This block was `#ifndef NDEBUG`, and CMakePresets.json pins the ONLY
  // supported config to Ninja + clang + x64 RELEASE — so the layer has never
  // been in the shipped binary, and every "no debug-layer errors" claim about
  // this renderer has been a statement about code that does not exist.
  //
  // The open-command-list refactor's entire failure class (recording into a
  // closed list, resetting an allocator whose list still records, an unbalanced
  // backbuffer barrier, heaps not re-bound after a Reset, a resource in the
  // wrong state at Present) is EXACTLY what this layer reports and what nothing
  // else in our toolchain can see. So it is compiled in unconditionally and
  // switched at runtime, default OFF: with SR_D3D12_DEBUG unset the default exe
  // behaves byte-for-byte as before.
  //
  // ⚠ LEVEL 2 BREAKS ON ERROR AND IS DELIBERATELY NOT THE DEFAULT: a debug
  // break with no debugger attached raises EXCEPTION_BREAKPOINT, and the
  // harness VEH makes that fatal. Level 1 DRAINS the message queue and logs it,
  // which is what a verdict actually needs — the message TEXT, not a corpse.
  m_dbgLevel = [] {
    const char* e = std::getenv("SR_D3D12_DEBUG");
    return e ? (uint32_t)std::atoi(e) : 0u;
  }();
  if (m_dbgLevel) {
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
      debugController->EnableDebugLayer();   // MUST precede D3D12CreateDevice
      m_dbgActive = true;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[D3D12_DEBUG] debug layer ENABLED (level %u)", m_dbgLevel);
      // step 4 lane C 2i (SR_D3D12_GBV=1, diagnostic, with SR_D3D12_DEBUG): GPU-based validation - resource states,
      // barriers and descriptors checked as the GPU executes (a missing barrier between a write and a read is what
      // the CPU-side layer cannot see). Very slow; for one replay, never a roll. Dev builds only.
#if SR_ORACLE
      if (const char* g = std::getenv("SR_D3D12_GBV"); g && g[0] == '1') {
        Microsoft::WRL::ComPtr<ID3D12Debug1> d1;
        if (SUCCEEDED(debugController.As(&d1))) {
          d1->SetEnableGPUBasedValidation(TRUE);
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[D3D12_DEBUG] GPU-based validation ENABLED (SR_D3D12_GBV=1)");
        } else {
          SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_DEBUG] SR_D3D12_GBV=1 but ID3D12Debug1 is unavailable");
        }
      }
#endif
    } else {
      // Loud, because a silent failure here turns every downstream
      // "zero debug-layer errors" verdict into a vacuous one.
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[D3D12_DEBUG] DEBUG LAYER NOT AVAILABLE — D3D12GetDebugInterface "
          "failed. Install the Windows 'Graphics Tools' optional feature. Any "
          "verdict that depends on layer messages is VOID until it is active.");
    }
  }

  // iter-460: DRED (auto-breadcrumbs + page-fault allocation data) - names the
  // command and the resource of a device removal at small cost. MUST precede
  // D3D12CreateDevice.
  {
    const char* e = std::getenv("SR_D3D12_DRED");
    if (e && e[0] == '1') {
      Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> ds;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&ds)))) {
        ds->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        ds->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        // DRED 1.2: SetMarker strings become breadcrumb contexts (the PM4
        // path marks every draw with its batch index and shader hashes).
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> ds1;
        if (SUCCEEDED(ds.As(&ds1)))
          ds1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        m_dredOn = true;
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[D3D12_DRED] auto-breadcrumbs + page-fault data ON");
      } else {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_DRED] settings interface unavailable");
      }
    }
  }

  noteBackendInitStep("the DXGI factory");  // render/boot-attach: the host-wait report
  hr = CreateDXGIFactory1(IID_PPV_ARGS(&m_factory));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateDXGIFactory1 failed: 0x%08X", hr);
    return false;
  }

  noteBackendInitStep("the D3D12 device");  // render/boot-attach: the host-wait report
  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] D3D12CreateDevice failed: 0x%08X", hr);
    return false;
  }

  // iter-445 C2: the info queue is how the layer's messages are READ. Without
  // it the layer only writes to the debugger's output window, which this
  // process has no way to see or record.
  if (m_dbgActive) {
    if (SUCCEEDED(m_device.As(&m_infoQueue))) {
      // iter-445 C7 PRE-BAKE FIX (audit): without this the runtime keeps only
      // 1024 messages and silently discards the rest between our drains - an
      // "empty message set" could then mean "we lost them".
      m_infoQueue->SetMessageCountLimit((UINT64)-1);
#if SR_ORACLE
      if (const char* sd = std::getenv("SR_D3D12_DEBUG_STACKS"); sd && sd[0] == '1') {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue1> iq1;
        DWORD cookie = 0;
        const HRESULT rh = SUCCEEDED(m_infoQueue.As(&iq1))
                               ? iq1->RegisterMessageCallback(&dbgStackCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                                              nullptr, &cookie)
                               : E_NOINTERFACE;
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[D3D12_DEBUG] stack callback for ERROR 708 / 935: %s (0x%08X)",
                    SUCCEEDED(rh) ? "registered" : "UNAVAILABLE", unsigned(rh));
      }
#endif
      if (m_dbgLevel >= 2) {
        // Opt-in only; see the warning above the enable block.
        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[D3D12_DEBUG] break-on-severity ARMED (CORRUPTION|ERROR) — a "
            "violation will raise EXCEPTION_BREAKPOINT and the harness VEH "
            "makes that fatal. That is the intent at level 2.");
      }
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[D3D12_DEBUG] ID3D12InfoQueue unavailable — the layer is on but its "
          "messages cannot be read, so they cannot be reported.");
    }
  }

  // iter-464 resscale: fix the draw resolution scale against this device (Xenia
  // d3d12_command_processor.cc:841-858: tiled resources + VA bits per resource).
  {
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va{};
    const bool haveO = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)));
    const bool haveVa = SUCCEEDED(
        m_device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va, sizeof(va)));
    resscale::clampToDevice(haveO && o.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_1,
                            haveVa ? va.MaxGPUVirtualAddressBitsPerResource : 0u,
                            D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
  }

  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  noteBackendInitStep("the command queue");  // render/boot-attach: the host-wait report
  hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateCommandQueue failed: 0x%08X", hr);
    return false;
  }

  hr = m_nativeImages.initialize(m_device.Get(), m_commandQueue.Get());
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_IMAGES] initialization failed: 0x%08X", hr);
    return false;
  }

#if SR_ORACLE
  m_readerStream = sr::kernel::reader_observe::stream();
  m_readerRecording = 0;
#endif
  hr = m_nativeConstants.initialize(m_device.Get(), 4u << 20, 256u << 20);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_CONSTANTS] initialization failed: 0x%08X", hr);
    return false;
  }

  hr = m_nativeGeometry.initialize(m_device.Get(), 4u << 20, 256u << 20);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GEOMETRY] initialization failed: 0x%08X", hr);
    return false;
  }

  hr = m_nativeRawGeometry.initialize(m_device.Get(), 4u << 20, 256u << 20);
  if (FAILED(hr)) return false;

  hr = m_nativePipelines.initialize(m_device.Get());
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_PIPELINE] initialization failed: 0x%08X", hr);
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 scDesc{};
  scDesc.Width = static_cast<UINT>(width);
  scDesc.Height = static_cast<UINT>(height);
  scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scDesc.SampleDesc.Count = 1;
  scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scDesc.BufferCount = kFrameCount;
  scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  // iter-464 PORT FEATURE (SR_HIGHFPS uncapped / above the display refresh): present
  // without vsync and let the display tear where it supports that. Off: no flag (as before).
  // iter-465 polish: also whenever port features are on - the DISPLAY OPTIONS page can turn
  // VSYNC off while the game runs, and a swap chain only tears if it was created able to.
  // Presents still pass DXGI_PRESENT_ALLOW_TEARING only at sync 0 with tearing wanted.
  m_allowTearing = false;
  if (sr::kernel::highfps::swapChainWantsTearingFlag()) {
    Microsoft::WRL::ComPtr<IDXGIFactory5> f5;
    BOOL tearOk = FALSE;
    if (SUCCEEDED(m_factory.As(&f5)) &&
        SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearOk, sizeof(tearOk))) &&
        tearOk)
      m_allowTearing = true;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[HIGHFPS] swap chain ALLOW_TEARING %s",
                m_allowTearing ? "on" : "unsupported (sync-0 presents without it)");
  }
  if (m_allowTearing) scDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  m_swapChainFlags = scDesc.Flags;

  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
  noteBackendInitStep("the swap chain");  // render/boot-attach: the host-wait report
  hr = m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain1);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateSwapChainForHwnd failed: 0x%08X", hr);
    return false;
  }
  m_factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
  swapChain1.As(&m_swapChain);
  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
  m_bbW = uint32_t(width);  // iter-464: the swapchain size (resizable, applyPendingResize)
  m_bbH = uint32_t(height);

  // RTV descriptor heap: kFrameCount backbuffers + 1 offscreen guest RT.
  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
  rtvHeapDesc.NumDescriptors = kFrameCount + 2;  // +1 LDR geo RT, +1 HDR scene RT
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateDescriptorHeap(RTV) failed: 0x%08X", hr);
    return false;
  }
  m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (int i = 0; i < kFrameCount; i++) {
    hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
    if (FAILED(hr)) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] GetBuffer(%d) failed: 0x%08X", i, hr);
      return false;
    }
    m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
    rtvHandle.ptr += m_rtvDescriptorSize;
  }

  // Each recording owns a fresh list and allocator. The checked queue retains
  // submitted owners; frame rotation never resets an allocator still in use.
  hr = openNativeRecording();
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateCommandList failed: 0x%08X", hr);
    return false;
  }
  // iter-445 C1: a command list is born RECORDING, so CreateCommandList is an
  // OPEN with no Reset — count it as one or the ledger's equality is off by
  // one for the life of the process.
  clNoteTid();
  m_clOpens++;
  m_clSiteHits[Create]++;
  m_clState = ClState::Recording;
  clClose(InitClose);

  // Fence for CPU/GPU sync.
  hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateFence failed: 0x%08X", hr);
    return false;
  }
  wdPublishFrameFence(m_fence.Get());   // step 4 2k(4) / Astra P2 4: the GPU-hang watchdog's queue-progress probe
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  for (int i = 0; i < kFrameCount; i++) m_fenceValues[i] = 1;

  // Upload buffer for framebuffer blit (enough for 1280x720 RGBA).
  m_uploadBufferSize = static_cast<size_t>(width) * height * 4;
  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bufDesc{};
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Width = m_uploadBufferSize;
  bufDesc.Height = 1;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.MipLevels = 1;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hr = m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&m_uploadBuffer));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateCommittedResource(upload) failed: 0x%08X", hr);
    return false;
  }

  // Readback buffer for RESOLVE (copy RT to guest framebuffer). Sized for the
  // widest pixel we read back (8 bytes = R16G16B16A16_FLOAT HDR snapshot dump);
  // the LDR resolve path uses m_readbackRowPitch (4 bytes) into the same buffer.
  m_readbackRowPitch = (static_cast<UINT>(width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                       & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  const UINT readbackRowPitch8 = (static_cast<UINT>(width) * 8 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                       & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  size_t readbackSize = static_cast<size_t>(readbackRowPitch8) * height;
  D3D12_HEAP_PROPERTIES readbackHeap{};
  readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
  readbackHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  readbackHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  bufDesc.Width = readbackSize;
  hr = m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&m_readbackBuffer));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateCommittedResource(readback) failed: 0x%08X", hr);
    return false;
  }

  m_initialized = true;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12] Backend initialized: %dx%d, %d back buffers", width, height, kFrameCount);

  noteBackendInitStep("the geometry PSO");  // render/boot-attach: the host-wait report
  m_geoPipelineReady = createGuestGeoPipeline();
  if (m_geoPipelineReady) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12] Guest geometry pipeline ready");
  }

  noteBackendInitStep("the xlat pipeline");  // render/boot-attach: the host-wait report
  m_xlatReady = createXlatPipeline();
  if (xlatReady()) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12] Translated-shader (XenosRecomp) pipeline ready");
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[DISPLAY] backend init %.1f ms (device, swap chain, PSOs, xlat objects; process +%.1f to +%.1f ms); "
              "the shader cache load is %s",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - initT0).count(), initAt,
              msSinceProcessStart(),
              getShaderCache().fullyLoaded()       ? "done"
              : getShaderCache().installedLoaded() ? "in its translator stage"
                                                   : "still in its installed stage");
  noteBackendInitStep(nullptr);  // returned: ready

  return true;
}

// render/boot-attach: the xlat path's readiness. createXlatPipeline decided it from the probe; if the
// load then admits nothing (every listed entry unreadable), it turns off as the old load-first test
// did. Never blocks (installedPresent answers from the probe until stage 1 ends).
bool D3D12Backend::xlatReady() const { return m_xlatReady && getShaderCache().installedPresent(); }

// ── step 4 item 7d(b): the translated-draw scratch, committed by high-water (d3d12_backend.hpp XlatScratch) ──
namespace {
// fix12c (orchestrator 12:5x): NOT baked in step 4 - default OFF, =1 opts in. Commit-on-growth makes any read of a
// not-yet-committed range an access violation in a player's game (fix12's first-draws diagnostic was one); its
// ~600 MB serves the step-6 commit target, which takes it with item 1 behind a first-touch fault safety net.
bool xlatScratchCommitOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_SCRATCH_COMMIT");
    return e && e[0] == '1';
  }();
  return on;
}
constexpr uint64_t kXlatCommitStep = 1ull << 20;
}  // namespace

// step 6 I10p (Fable ruling 40; the coordinator's variant 10-23; frozen bisect2e/I10p_FROZEN_20261023.txt):
// SR_XLAT_CB_ZEROCOPY: DEFAULT ON since its merge (the coordinator 10-23: the graded remeasure measured it gate ON -
// render -3.78 ms / frame draw-normalised - so it ships ON); =0 opts out (the scratch path). The CP writes each
// draw's CHANGED VS / PS float blocks
// (M1: only the used 256-byte blocks) and its vsf fetch block (M2: deduped against the previous draw's, a cacheable
// copy) ONCE, straight into m_xlatCBGpu (the write-only upload twin of the CB thirds, gfx/xlat_cb_gpu.hpp); the
// batch's offsets are the views; the CB third and its fence own them. The render thread binds them and writes only
// the draw's shared slot, when the shared bytes change. Refused (said once; the scratch path stays) where a
// diagnostic reads the scratch's VS / PS / fetch bytes or the staged snapshot (those bytes do not exist on the view
// path), and where the legacy (non-PM4) drain could run (its batches bind from the scratch).
namespace {
bool xlatCbZeroCopyOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_CB_ZEROCOPY");
    if (e && e[0] == '0') {   // default ON; =0 = the scratch path (said once, for the receipts)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[XLAT_CB_ZEROCOPY] off (SR_XLAT_CB_ZEROCOPY=0): the scratch path");
      return false;
    }
    const char* why = nullptr;
    const char* hdr = std::getenv("SR_HDR_ASYNC_PROBE");
    if (hdr && hdr[0] == '1') why = "SR_HDR_ASYNC_PROBE reads the staged constants' bytes";
    if (!why && !pm4RtCacheEnabled()) why = "the legacy (non-PM4) drain binds from the scratch (SR_PM4_RTCACHE off)";
#if SR_ORACLE
    const char* tex = std::getenv("SR_MEM_RES_TEXTURE_PROBE");
    if (!why && tex && std::strcmp(tex, "1") == 0) why = "SR_MEM_RES_TEXTURE_PROBE reads the staged constants' bytes";
    const char* fc = std::getenv("SR_MEM_RES_FETCH_CAPTURE");
    if (!why && fc && std::strcmp(fc, "1") == 0) why = "SR_MEM_RES_FETCH_CAPTURE reads the scratch VS / fetch bytes";
    if (!why && memres::resourceConsumerEnabled()) why = "the memres consumer capture reads the scratch constants";
#endif
    SR_LOG_BOOT(sr::LogSeverity::Info, "[XLAT_CB_ZEROCOPY] %s%s",
                why ? "REFUSED (the scratch path): " : "on: VS / PS / fetch blocks written once into the write-only "
                                                       "upload twin of the CB thirds, bound as views (the third's "
                                                       "fence owns them); the shared slot written on change",
                why ? why : "");
    return !why;
  }();
  return on;
}
#if SR_ORACLE
// SR_XLAT_CB_ZEROCOPY_CHECK=1 (SR_ORACLE only): every m_xlatCBGpu write also lands in a cacheable SHADOW (the twin is
// never read); the CP records in the batch the hash of the bytes each view must hold (from its cacheable sources); at
// bind the render thread re-hashes the shadow's used blocks of every view (compare-on-consume) and records the range
// for its CB third; when the CP reuses the third (after its fence wait) it asserts that every recording that bound
// the third's views was submitted and has completed (the retire assert), re-hashes every recorded range
// (compare-on-retire: a CPU write into a published range while the GPU could still read it), then poisons the
// third's used range (0xFF: NaN floats).
bool xlatCbZeroCopyCheckOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_CB_ZEROCOPY_CHECK");
    const bool v = e && e[0] == '1' && xlatCbZeroCopyOn();
    if (v)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[XLAT_CB_ZEROCOPY_CHECK] on: compare-on-consume / -retire against the "
                                         "cacheable shadow, the retire assert, poison of a retired third");
    return v;
  }();
  return on;
}
// SR_XLAT_CB_ZEROCOPY_MUTANT=third (SR_ORACLE, TEST-ONLY, check mode only): every 8th set writes its CB blocks into
// the PREVIOUS set's CB third (a third reused one set early: its views may still be unrendered or on the GPU). The
// check mode must catch it ([XLAT_CB_ZEROCOPY_CHECK] BAD: retire assert / stale generation / consume / retire).
bool xlatCbZeroCopyMutantThird() {
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_CB_ZEROCOPY_MUTANT");
    const bool v = e && std::strcmp(e, "third") == 0 && xlatCbZeroCopyCheckOn();
    if (e && !v)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[XLAT_CB_ZEROCOPY] MUTANT REFUSED: it needs SR_XLAT_CB_ZEROCOPY_CHECK=1");
    if (v)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[XLAT_CB_ZEROCOPY] MUTANT third: every 8th set reuses the previous set's "
                                         "CB third (one set early)");
    return v;
  }();
  return on;
}
#endif
}  // namespace

XlatScratch::~XlatScratch() {
  if (m_reserved && m_base) VirtualFree(m_base, 0, MEM_RELEASE);
}

bool XlatScratch::init(uint64_t bytes, bool reserveOnly) {
  if (m_reserved && m_base) VirtualFree(m_base, 0, MEM_RELEASE);   // a re-init replaces the previous storage
  m_base = nullptr;
  m_reserved = false;
  for (auto& e : m_end) e.store(0, std::memory_order_relaxed);
  m_vec.clear();
  m_bytes = bytes;
  if (reserveOnly) {
    m_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, SIZE_T(bytes), MEM_RESERVE, PAGE_READWRITE));
    m_reserved = m_base != nullptr;
    return m_reserved;
  }
  try {
    m_vec.assign(size_t(bytes), 0);
  } catch (const std::bad_alloc&) {
    return false;
  }
  m_base = m_vec.data();
  return true;
}

bool XlatScratch::commit(uint64_t end, bool* grew) {
  if (!m_reserved || end == 0) return true;
  if (end > m_bytes) return false;
  const uint64_t third = m_bytes / 3;
  const uint32_t t = uint32_t(std::min<uint64_t>((end - 1) / third, 2));
  const uint64_t lo = uint64_t(t) * third, hi = t == 2 ? m_bytes : lo + third;
  const uint64_t have = m_end[t].load(std::memory_order_acquire);
  if (end <= have) return true;   // the fast path: already committed
  const uint64_t from = std::max(have, lo);
  const uint64_t to = std::min(hi, (end + kXlatCommitStep - 1) & ~(kXlatCommitStep - 1));
  // MEM_COMMIT of pages already committed (a 1 MB step straddling a third's edge) is a no-op, never a reset
  if (!VirtualAlloc(m_base + from, SIZE_T(to - from), MEM_COMMIT, PAGE_READWRITE)) return false;
  uint64_t cur = have;
  while (cur < to && !m_end[t].compare_exchange_weak(cur, to, std::memory_order_acq_rel)) {
  }
  if (grew) *grew = true;
  return true;
}

uint64_t XlatScratch::committedBytes() const {
  if (!m_reserved) return m_bytes;
  const uint64_t third = m_bytes / 3;
  uint64_t n = 0;
  for (uint32_t t = 0; t < 3; ++t) {
    const uint64_t e = m_end[t].load(std::memory_order_relaxed);
    if (e > uint64_t(t) * third) n += e - uint64_t(t) * third;
  }
  return n;
}

bool D3D12Backend::xlatCommit(uint64_t vbEnd, uint64_t cbEnd) {
  bool grew = false;
  if (!m_xlatVertices.commit(vbEnd, &grew) || !m_xlatConstants.commit(cbEnd, &grew)) {
    static std::atomic<uint32_t> s_failN{0};
    if (s_failN.fetch_add(1, std::memory_order_relaxed) < 4)
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[XLAT_HWM] scratch commit FAILED (vb end %llu, cb end %llu; GetLastError %lu) - the draw is rejected "
                  "as with a full third (SR_XLAT_SCRATCH_COMMIT)",
                  (unsigned long long)vbEnd, (unsigned long long)cbEnd, (unsigned long)GetLastError());
    return false;
  }
  if (!grew) return true;   // the per-draw fast path
  // the high-water line: each new 16 MB of committed scratch (vb + cb), and the first commit
  static uint64_t s_logged = 0;
  const uint64_t vb = m_xlatVertices.committedBytes(), cb = m_xlatConstants.committedBytes();
  if (s_logged == 0 || vb + cb >= s_logged + (16ull << 20)) {
    s_logged = vb + cb;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XLAT_HWM] scratch committed: vb %.0f of %.0f MB, cb %.0f of %.0f MB (step 4 7d(b): committed by "
                "high-water, SR_XLAT_SCRATCH_COMMIT)",
                double(vb) / 1048576.0, double(kXlatVBBytes) / 1048576.0, double(cb) / 1048576.0,
                double(kXlatCBBytes) / 1048576.0);
  }
  return true;
}

#if SR_ORACLE
static const char* kGeoShaderSrc = R"hlsl(
struct VSInput  { float3 pos : POSITION; float4 color : COLOR; };
struct PSInput  { float4 pos : SV_POSITION; float4 color : COLOR; };

// Per-draw constants (root constants, 10 dwords) — snapshot of the Xenos
// viewport state at draw time (PA_CL_VPORT_* + PA_CL_VTE_CNTL).
cbuffer DrawConsts : register(b0) {
  float vsx; float vsy; float vsz; float vox;   // c0
  float voy; float voz; uint  vte; float rtW;   // c1
  float rtH; float pad0;                        // c2.xy
};

PSInput VSMain(VSInput v) {
  PSInput o;
  // Xenos VTE semantics (registers.h PA_CL_VTE_CNTL; draw_util.cc
  // GetHostViewportInfo): each viewport scale/offset applies only when its
  // ena bit is set. Menu/UI draws disable all six -> vertex XY are already
  // PIXELS; 3D draws enable them -> scale/offset produce pixels. Either way
  // the result here is screen-space, mapped to NDC against the RT size.
  // (vtx_xy_fmt / 1/W handling not modeled yet — menu vertices arrive
  // pre-divided; filed for the ucode-translation stage.)
  float sx = (vte & 1u) ? vsx : 1.0;
  float ox = (vte & 2u) ? vox : 0.0;
  float sy = (vte & 4u) ? vsy : 1.0;
  float oy = (vte & 8u) ? voy : 0.0;
  float px = v.pos.x * sx + ox;
  float py = v.pos.y * sy + oy;
  o.pos = float4(px / (rtW * 0.5) - 1.0, 1.0 - py / (rtH * 0.5), 0.5, 1.0);
  o.color = v.color;
  return o;
}

float4 PSMain(PSInput p) : SV_TARGET {
  return p.color;
}
)hlsl";

#endif  // SR_ORACLE
bool D3D12Backend::createGuestGeoPipeline() {
  HRESULT hr;

#if SR_ORACLE
  // Root signature: one 32-bit-constants parameter (b0, 10 dwords).
  D3D12_ROOT_PARAMETER rootParam{};
  rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  rootParam.Constants.ShaderRegister = 0;
  rootParam.Constants.RegisterSpace = 0;
  rootParam.Constants.Num32BitValues = 10;
  rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 1;
  rsDesc.pParameters = &rootParam;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errBlob;
  hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
  if (FAILED(hr)) return false;
  hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_geoRootSig));
  if (FAILED(hr)) return false;

  Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob;
  hr = hostHlslCompile(kGeoShaderSrc, strlen(kGeoShaderSrc), "geo.hlsl", nullptr,
                  "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] Geo VS compile: %s",
                errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown");
    return false;
  }
  errBlob.Reset();
  hr = hostHlslCompile(kGeoShaderSrc, strlen(kGeoShaderSrc), "geo.hlsl", nullptr,
                  "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob);
  if (FAILED(hr)) return false;

  D3D12_INPUT_ELEMENT_DESC inputElems[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.InputLayout = {inputElems, _countof(inputElems)};
  psoDesc.pRootSignature = m_geoRootSig.Get();
  psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
  psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.RasterizerState.DepthClipEnable = TRUE;
  // Straight-alpha blending — the UI draws with src=SrcAlpha dst=InvSrcAlpha
  // (RB_BLENDCONTROL 0x07060706-class); per-draw blend state is filed.
  auto& rtBlend = psoDesc.BlendState.RenderTarget[0];
  rtBlend.BlendEnable = TRUE;
  rtBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
  rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
  rtBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.SampleMask = UINT_MAX;
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  // iter-359e: a DSV is bound for the translated path; the fixed-function
  // pipeline ignores depth (DepthEnable=false) but declares the format.
  psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  psoDesc.SampleDesc.Count = 1;

  hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_geoPSO));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] CreateGeoPSO: 0x%08X", hr);
    return false;
  }

  // Dynamic upload vertex buffer: kMaxGuestVerts * (pos float3 + color float4).
  D3D12_HEAP_PROPERTIES heapProps{};
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bufDesc{};
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Width = static_cast<UINT64>(kMaxGuestVerts) * kGeoVertexStride;
  bufDesc.Height = 1;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.MipLevels = 1;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&m_geoVertexBuffer));
  if (FAILED(hr)) return false;

  m_geoVBView.BufferLocation = m_geoVertexBuffer->GetGPUVirtualAddress();
  m_geoVBView.StrideInBytes = kGeoVertexStride;
  m_geoVBView.SizeInBytes = static_cast<UINT>(kMaxGuestVerts * kGeoVertexStride);

#endif  // SR_ORACLE
  // Offscreen guest render target (RTV heap slot kFrameCount): the real draws
  // land here, decoupled from the swapchain; the resolve reads it back into
  // guest memory and a CopyResource shows it in the window.
  D3D12_HEAP_PROPERTIES rtHeap{};
  rtHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rtDesc{};
  rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rtDesc.Width = static_cast<UINT64>(m_width);
  rtDesc.Height = static_cast<UINT>(m_height);
  rtDesc.DepthOrArraySize = 1;
  rtDesc.MipLevels = 1;
  rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rtDesc.SampleDesc.Count = 1;
  rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = rtDesc.Format;
  clear.Color[3] = 1.0f;
  hr = m_device->CreateCommittedResource(&rtHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                         IID_PPV_ARGS(&m_geoRT));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] Create geo RT: 0x%08X", hr);
    return false;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtvHandle.ptr += static_cast<SIZE_T>(kFrameCount) * m_rtvDescriptorSize;
  m_device->CreateRenderTargetView(m_geoRT.Get(), nullptr, rtvHandle);

  // iter-359m: HDR scene surface (the EDRAM surface model). The scene color
  // passes use float RT formats (RB_COLOR_INFO fmt 3/12 = 2_10_10_10_FLOAT);
  // rendering them into an R16G16B16A16_FLOAT surface preserves values > 1 so
  // the tonemap/composite (which samples the scene resolve) gets correct-range
  // input instead of the clamped LDR that over-exposed to yellow.
  rtDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  clear.Format = rtDesc.Format;
  hr = m_device->CreateCommittedResource(&rtHeap, D3D12_HEAP_FLAG_NONE, &rtDesc,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                         IID_PPV_ARGS(&m_sceneHdrRT));
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12] Create HDR scene RT: 0x%08X", hr);
    return false;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  hdrRtv.ptr += static_cast<SIZE_T>(kFrameCount + 1) * m_rtvDescriptorSize;
  m_device->CreateRenderTargetView(m_sceneHdrRT.Get(), nullptr, hdrRtv);

  return true;
}

namespace {
inline uint32_t geoReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// ── Translated-shader path helpers (iter-359c) ───────────────────────────────

// Xenos fetch-constant endian op applied to a host-LE dword read of guest memory.
inline uint32_t xeSwap32(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);  // 8in16
    case 2: return _byteswap_ulong(v);                                   // 8in32
    case 3: return (v >> 16) | (v << 16);                                // 16in32
    default: return v;
  }
}

inline float xeHalfToFloat(uint16_t h) {
  uint32_t sign = (h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t man = h & 0x3FF;
  uint32_t bits;
  if (exp == 0) {
    if (!man) { bits = sign; }
    else {  // subnormal
      exp = 127 - 15 + 1;
      while (!(man & 0x400)) { man <<= 1; exp--; }
      man &= 0x3FF;
      bits = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (man << 13);
  } else {
    bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// iter-444 S6e: bytes ONE fetch of this format reads. Needed to prove a
// declared attribute actually lies inside the guest's own vertex record —
// without it a FLOAT4 declared at the last offset reads 16 bytes past the
// snapshot, which is silent garbage rather than a fault.
inline uint32_t xeFormatBytes(uint32_t fmt) {
  using VF = ucode::VertexFormat;
  switch (VF(fmt)) {
    case VF::k_32_FLOAT: case VF::k_32: case VF::k_16_16_FLOAT:
    case VF::k_16_16: case VF::k_8_8_8_8: case VF::k_2_10_10_10:
    case VF::k_10_11_11: case VF::k_11_11_10:
      return 4;
    case VF::k_32_32_FLOAT: case VF::k_32_32:
    case VF::k_16_16_16_16_FLOAT: case VF::k_16_16_16_16:
      return 8;
    case VF::k_32_32_32_FLOAT:
      return 12;
    case VF::k_32_32_32_32_FLOAT: case VF::k_32_32_32_32:
      return 16;
    default:
      return 0;   // unknown — the caller must refuse the draw, not guess
  }
}

inline float xeSnorm(int32_t v, int32_t maxAbs) {
  float f = float(v) / float(maxAbs);
  return f < -1.0f ? -1.0f : f;
}

inline int32_t xeSignExtend(uint32_t v, int bits) {
  uint32_t m = 1u << (bits - 1);
  return int32_t((v ^ m)) - int32_t(m);
}

// Decode ONE vertex attribute to the canonical float4 the recompiled VS reads
// (the fetch RESULT: x = low bits, per Xenos spec — the shader's own compiled
// dstSwizzle does any reordering). `src` is dword-aligned guest memory.
inline void xeDecodeAttr(const VfetchElement& e, const uint8_t* src, uint32_t endian,
                         float out[4]) {
  out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;
  auto rd = [&](uint32_t k) {
    uint32_t raw;
    std::memcpy(&raw, src + k * 4, 4);
    return xeSwap32(raw, endian);
  };
  auto rdF = [&](uint32_t k) {
    uint32_t v = rd(k);
    float f;
    std::memcpy(&f, &v, 4);
    return std::isfinite(f) ? f : 0.0f;
  };
  using VF = ucode::VertexFormat;
  switch (VF(e.format)) {
    case VF::k_32_FLOAT:          out[0] = rdF(0); break;
    case VF::k_32_32_FLOAT:       out[0] = rdF(0); out[1] = rdF(1); break;
    case VF::k_32_32_32_FLOAT:    out[0] = rdF(0); out[1] = rdF(1); out[2] = rdF(2); break;
    case VF::k_32_32_32_32_FLOAT: out[0] = rdF(0); out[1] = rdF(1); out[2] = rdF(2); out[3] = rdF(3); break;
    case VF::k_16_16_FLOAT: {
      uint32_t v = rd(0);
      out[0] = xeHalfToFloat(uint16_t(v & 0xFFFF));
      out[1] = xeHalfToFloat(uint16_t(v >> 16));
      break;
    }
    case VF::k_16_16_16_16_FLOAT: {
      uint32_t v0 = rd(0), v1 = rd(1);
      out[0] = xeHalfToFloat(uint16_t(v0 & 0xFFFF));
      out[1] = xeHalfToFloat(uint16_t(v0 >> 16));
      out[2] = xeHalfToFloat(uint16_t(v1 & 0xFFFF));
      out[3] = xeHalfToFloat(uint16_t(v1 >> 16));
      break;
    }
    case VF::k_8_8_8_8: {
      uint32_t v = rd(0);
      for (int c = 0; c < 4; c++) {
        uint32_t b = (v >> (c * 8)) & 0xFF;
        if (e.isInteger)      out[c] = e.isSigned ? float(xeSignExtend(b, 8)) : float(b);
        else if (e.isSigned)  out[c] = xeSnorm(xeSignExtend(b, 8), 127);
        else                  out[c] = float(b) / 255.0f;
      }
      // iter-449u: D3DCOLOR stores B,G,R,A — the shader expects R in .x.
      if (e.bgra) { const float t = out[0]; out[0] = out[2]; out[2] = t; }
      break;
    }
    case VF::k_2_10_10_10: {
      uint32_t v = rd(0);
      uint32_t c10[3] = {v & 0x3FF, (v >> 10) & 0x3FF, (v >> 20) & 0x3FF};
      uint32_t w2 = v >> 30;
      for (int c = 0; c < 3; c++) {
        if (e.isInteger)      out[c] = e.isSigned ? float(xeSignExtend(c10[c], 10)) : float(c10[c]);
        else if (e.isSigned)  out[c] = xeSnorm(xeSignExtend(c10[c], 10), 511);
        else                  out[c] = float(c10[c]) / 1023.0f;
      }
      if (e.isInteger)      out[3] = e.isSigned ? float(xeSignExtend(w2, 2)) : float(w2);
      else if (e.isSigned)  out[3] = xeSnorm(xeSignExtend(w2, 2), 1);
      else                  out[3] = float(w2) / 3.0f;
      break;
    }
    case VF::k_16_16: {
      uint32_t v = rd(0);
      uint32_t h[2] = {v & 0xFFFF, v >> 16};
      for (int c = 0; c < 2; c++) {
        if (e.isInteger)      out[c] = e.isSigned ? float(xeSignExtend(h[c], 16)) : float(h[c]);
        else if (e.isSigned)  out[c] = xeSnorm(xeSignExtend(h[c], 16), 32767);
        else                  out[c] = float(h[c]) / 65535.0f;
      }
      break;
    }
    case VF::k_16_16_16_16: {
      uint32_t v0 = rd(0), v1 = rd(1);
      uint32_t h[4] = {v0 & 0xFFFF, v0 >> 16, v1 & 0xFFFF, v1 >> 16};
      for (int c = 0; c < 4; c++) {
        if (e.isInteger)      out[c] = e.isSigned ? float(xeSignExtend(h[c], 16)) : float(h[c]);
        else if (e.isSigned)  out[c] = xeSnorm(xeSignExtend(h[c], 16), 32767);
        else                  out[c] = float(h[c]) / 65535.0f;
      }
      break;
    }
    case VF::k_32: {
      uint32_t v = rd(0);
      out[0] = e.isSigned ? float(int32_t(v)) : float(v);  // integer use only observed
      break;
    }
    case VF::k_32_32: {
      out[0] = e.isSigned ? float(int32_t(rd(0))) : float(rd(0));
      out[1] = e.isSigned ? float(int32_t(rd(1))) : float(rd(1));
      break;
    }
    default:
      // k_10_11_11 / k_11_11_10 / k_32_32_32_32 int — not in the menu corpus.
      break;
  }
}

const char* xeUsageSemantic(uint32_t usage) {
  static const char* kNames[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL",
                                 "PSIZE",    "TEXCOORD",    "TANGENT",      "BINORMAL",
                                 "TESSFACTOR", "POSITIONT", "COLOR",        "FOG",
                                 "DEPTH",    "SAMPLE"};
  return usage < 14 ? kNames[usage] : "TEXCOORD";
}
}  // namespace

// ── Translated-shader (XenosRecomp) pipeline — iter-359c ────────────────────
// Contract (tools/xenosrecomp/README.md, ENGINE_MAP §6d "Shaders"):
// root CBVs b0/b1 space4 = VS(256)/PS(256, was 224 until P1) float4 constant files, b2 = shared
// (desc indices c0-15, booleans c16, alpha c17, NDC remap c18), b3/b4 = SAME
// buffers as b0/b1 rebound as plain arrays for dynamic indexing. Bindless SRV
// heaps t0 spaces 0-2 + samplers s0 space3; v1 = 1×1 white at index 0.
// The translated draws' shader-visible SRV heap and its per-slot state (step 6 I3-B: factored out of
// createXlatPipeline, so tools/difftest/native_bindless_test.cpp builds the production heap).
bool D3D12Backend::createXlatSrvHeap() {
  static_assert(kXlat2DSlots == kTex2DSlots && kXlatRawRingSlots == kRawRingSlots);  // the fixture's view
  // Shader-visible heaps. SRV slots 0-7 per space table share one heap:
  // table space0 starts at slot 0, space1 at slot 8, space2 at slot 16.
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  // iter-359j: a single frame creates 300+ textures (render-time re-creation);
  // the 256-slot heap silently whited the overflow. [0..kTex2DSlots-1] 2D
  // (0 = white), then 8 3D, then 8 cube (see kTex2DSlots note).
  // iter-460: + the PM4 host render targets' SRV band (GPU EDRAM store reads
  // them), after the 3D/cube slots so no xlat table range covers it.
  // + the VSF vertex views; step 6 I3-B: + the null raw view and the raw ring (written directly, no recipes)
  hd.NumDescriptors = kRawRingBase + kRawRingSlots;
  static_assert(kRawNullSrvSlot == kTex2DSlots + 16 + kPm4RtSrvSlots + 2);
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_xlatSrvHeap)))) return false;
  m_xlatSrvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  try {
    m_xlatViews.assign(kRawRingBase, NativeViewRecipe{}); m_xlatViewId.assign(kRawRingBase, 0u);
    m_xlatSlotDim.assign(kRawRingBase, uint8_t(D3D12_SRV_DIMENSION_UNKNOWN));
#if SR_ORACLE
    m_i3bSlotGen.assign(hd.NumDescriptors, 0u); m_i3bSlotSubmitted.assign(hd.NumDescriptors, 0u);
    m_i3bSlotBoundRec.assign(hd.NumDescriptors, 0u);
#endif
  }
  catch (const std::bad_alloc&) { return false; }
  m_rawRingWrites = m_rawRingRecStart = m_rawRingCheckedTo = 0; m_rawRingSegments.clear();
  m_rawLastRec = 0;
  {  // step 6 I3-B: the null raw view a draw without in-shader vertex fetch binds as table 10 (written once)
    const UINT rawInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R32_TYPELESS;
    rv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    rv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    rv.Buffer.NumElements = 16;
    rv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    publishXlatSrv(nullptr, &rv, {m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                  SIZE_T(kRawNullSrvSlot) * rawInc});
  }
  return true;
}

bool D3D12Backend::createXlatPipeline() {
  sr::kernel::MemSiteScope memSite("createXlatPipeline (xlat roots, upload, CPU scratch)");   // step 4 7d: [MEM_SITE] names (SR_MEM_SITES, diagnostic)
  // render/boot-attach: "is there an installed cache" decides, as before (VS + PS count == 0 ->
  // fixed-function path only). SR_SHADER_LOAD_ASYNC=1: the probe answers without loading (the load
  // runs on its thread); =0: the old order - load the whole cache here, before the attach.
  auto& shaderCache = getShaderCache();
  if (!ShaderCache::bootLoadAsync()) shaderCache.ensureLoaded();
  if (!shaderCache.installedPresent())
    return false;  // no cache on disk — fixed-function path only

  HRESULT hr;

  D3D12_ROOT_PARAMETER params[15] = {};
  for (int i = 0; i < 5; i++) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = UINT(i);
    params[i].Descriptor.RegisterSpace = 4;
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE srvRanges[3] = {};
  for (int i = 0; i < 3; i++) {
    srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[i].NumDescriptors = (i == 0) ? kTex2DSlots : 8;
    srvRanges[i].BaseShaderRegister = 0;
    srvRanges[i].RegisterSpace = UINT(i);
    srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
    params[5 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[5 + i].DescriptorTable.NumDescriptorRanges = 1;
    params[5 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
    params[5 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE sampRange{};
  sampRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  sampRange.NumDescriptors = kXlatSamplerTable;  // 16 legacy + the volume samplers (translated_samplers.hpp)
  sampRange.BaseShaderRegister = 0;
  sampRange.RegisterSpace = 3;
  params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[8].DescriptorTable.NumDescriptorRanges = 1;
  params[8].DescriptorTable.pDescriptorRanges = &sampRange;
  params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // P1 in-shader vertex fetch (<hash>.vs.vsf.dxil): b5 space5 = the draw's
  // vertex fetch constants, t0 space5 = the staged guest vertex memory (raw).
  params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[9].Descriptor.ShaderRegister = 5;
  params[9].Descriptor.RegisterSpace = 5;
  params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_DESCRIPTOR_RANGE vsfRange{};
  vsfRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  vsfRange.NumDescriptors = 1;
  vsfRange.BaseShaderRegister = 0;
  vsfRange.RegisterSpace = 5;
  params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[10].DescriptorTable.NumDescriptorRanges = 1;
  params[10].DescriptorTable.pDescriptorRanges = &vsfRange;
  params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  // P1: t0 space6 = the same 2D slot band viewed as Texture2DArray (stacked
  // textures sampled by a 3D-or-stacked tfetch; getOrCreateTexture2DArray).
  D3D12_DESCRIPTOR_RANGE arrRange{};
  arrRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  arrRange.NumDescriptors = kTex2DSlots;
  arrRange.BaseShaderRegister = 0;
  arrRange.RegisterSpace = 6;
  params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[11].DescriptorTable.NumDescriptorRanges = 1;
  params[11].DescriptorTable.pDescriptorRanges = &arrRange;
  params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // P1: t0 space7 / space8 = the 2D slot band viewed as Texture3D / TextureCube
  // (getOrCreateTexture3D, getOrCreateTexture2DArray(cube)). Same heap start
  // as table 5/11; a slot is only ever read through the view of its type.
  D3D12_DESCRIPTOR_RANGE bandRanges[2] = {};
  for (int i = 0; i < 2; i++) {
    bandRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bandRanges[i].NumDescriptors = kTex2DSlots;
    bandRanges[i].BaseShaderRegister = 0;
    bandRanges[i].RegisterSpace = UINT(7 + i);
    params[12 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[12 + i].DescriptorTable.NumDescriptorRanges = 1;
    params[12 + i].DescriptorTable.pDescriptorRanges = &bandRanges[i];
    params[12 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // iter-464 resscale: b6 space4 = g_PixelPosScaleBias (2 root constants,
  // 1/scale - 1 per axis; 0 at 1x) - the Xenos param-gen pixel position in guest
  // pixels (tools/xenosrecomp/0003-iter464-resscale-pixel-position.patch). Its own
  // cbuffer, so no other shader's DXIL changed.
  params[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[14].Constants.ShaderRegister = 6;
  params[14].Constants.RegisterSpace = 4;
  params[14].Constants.Num32BitValues = 2;
  params[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.NumParameters = 15;
  rsDesc.pParameters = params;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  // Step 4 item 2k (diagnostic, default off): SR_PSO_SALT=<n> makes the root signature (and so every translated PSO's
  // driver hash) new without changing what any shader reads: a reproducible first-run, cold-driver-cache boot without
  // touching the driver's shared cache folder. n = 1..3: the hull / domain stages' root access denied (no translated
  // pipeline has them); n >= 4: one unused static sampler at s<n> space 99 (no shader declares space 99) - any number
  // of single-use salts. Each value is one more PSO set in the driver cache, and cold exactly once: record the salts.
  // Astra (10:2x) #7: it changes the root signature every translated draw uses - a test device only, compiled out of
  // release (SR_ORACLE=0; docs/release_composition.json marker SR_PSO_SALT).
#if SR_ORACLE
  static D3D12_STATIC_SAMPLER_DESC s_saltSampler{};
  if (const char* salt = std::getenv("SR_PSO_SALT"); salt && *salt) {
    const long v = std::strtol(salt, nullptr, 10);
    if (v >= 1 && v <= 3) {
      if (v & 1) rsDesc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
      if (v & 2) rsDesc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
    } else if (v >= 4 && v <= 1000000) {
      s_saltSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      s_saltSampler.AddressU = s_saltSampler.AddressV = s_saltSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      s_saltSampler.MaxLOD = D3D12_FLOAT32_MAX;
      s_saltSampler.ShaderRegister = UINT(v);
      s_saltSampler.RegisterSpace = 99;
      s_saltSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      rsDesc.NumStaticSamplers = 1;
      rsDesc.pStaticSamplers = &s_saltSampler;
    }
    if (v >= 1 && v <= 1000000)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PSO_CREATE] SR_PSO_SALT=%ld: the translated root signature salted (flags "
                  "0x%X, %u static sampler) - every translated pipeline compiles cold this run (a diagnostic)", v,
                  unsigned(rsDesc.Flags), unsigned(rsDesc.NumStaticSamplers));
  }
#endif  // SR_ORACLE (SR_PSO_SALT)
  Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errBlob;
  hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
  if (FAILED(hr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[D3D12_XLAT] root sig serialize: %s",
                errBlob ? (const char*)errBlob->GetBufferPointer() : "?");
    return false;
  }
  hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_xlatRootSig));
  if (FAILED(hr)) return false;

  // step 6 I3-B (bindless, Fable ruling 44): production translated draws index m_xlatSrvHeap directly, so the
  // native root keeps the legacy root's full table ranges (it used compact 64-entry tables before).
  std::array<uint32_t, 15> extents{};
  for (uint32_t i = 0; i < 5; ++i) extents[i] = i == 2 ? 512u : 4096u;
  extents[9] = 768;
  hr = NativeRootSignature::create(m_device.Get(), rsDesc, m_nativeXlatRoot, extents);
  if (FAILED(hr)) return false;

  // Native geometry/constant snapshots read cacheable CPU planning storage.
  // Reading a mapped UPLOAD heap here is prohibitively slow on discrete GPUs.
  D3D12_HEAP_PROPERTIES up{};
  up.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RANGE noRead{0, 0};
  // step 4 7d(b): SR_XLAT_SCRATCH_COMMIT=1 = one reservation each, committed by high-water (xlatCommit); the
  // default (fix12c: not baked, step 6) = the zero-filled vectors as before
  // step 6 I10p (SR_XLAT_CB_ZEROCOPY): the view path's write-only upload twin of the CB thirds (kXlatCBBytes, committed
  // whole by the driver); the cacheable scratch then holds only the CP's shared blocks (kXlatShBytes). A failed twin
  // allocation keeps the scratch path (said once).
  m_zcOn = false;
  if (xlatCbZeroCopyOn()) {
    const HRESULT zhr = m_xlatCBGpu ? S_OK : m_xlatCBGpu.initialize(m_device.Get(), kXlatCBBytes);
    m_zcOn = SUCCEEDED(zhr);
    if (!m_zcOn)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[XLAT_CB_ZEROCOPY] REFUSED (the scratch path): the %llu MiB upload twin "
                  "was not allocated (hr 0x%08X)", (unsigned long long)(kXlatCBBytes >> 20), unsigned(zhr));
  }
  if (!m_xlatVertices.init(kXlatVBBytes, xlatScratchCommitOn()) ||
      !m_xlatConstants.init(m_zcOn ? kXlatShBytes : kXlatCBBytes, xlatScratchCommitOn()))
    return false;
  m_xlatVBPtr = m_xlatVertices.data();
  m_xlatCBPtr = m_xlatConstants.data();
#if SR_ORACLE
  m_zcCheck = m_zcOn && xlatCbZeroCopyCheckOn();
  m_zcMutant = m_zcCheck && xlatCbZeroCopyMutantThird();
  if (m_zcCheck && !zcShadowInit()) return false;
#endif
  if (m_zcOn) {   // Gate 3 / memory (the coordinator's condition 4): what moved where
#if SR_ORACLE
    const char* shadowNote = m_zcCheck ? "; check mode: + the shadow (cacheable, the twin's size)" : "";
#else
    const char* shadowNote = "";
#endif
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XLAT_CB_ZEROCOPY] memory: upload twin %llu MiB (UPLOAD heap, committed); cacheable scratch %llu MiB "
                "(was %llu MiB: shared blocks only, %s)%s",
                (unsigned long long)(m_xlatCBGpu.bytes() >> 20), (unsigned long long)(m_xlatConstants.size() >> 20),
                (unsigned long long)(kXlatCBBytes >> 20),
                xlatScratchCommitOn() ? "committed by high-water" : "zero-filled", shadowNote);
  }

  // Depth buffer for the offscreen guest RT (iter-359e): real per-draw depth
  // state needs a DSV — without one, 3D draws composite in painter's order.
  {
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = static_cast<UINT64>(m_width);
    dd.Height = static_cast<UINT>(m_height);
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dc{};
    dc.Format = DXGI_FORMAT_D32_FLOAT;
    dc.DepthStencil.Depth = 1.0f;
    if (FAILED(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, IID_PPV_ARGS(&m_geoDepth))))
      return false;
    D3D12_DESCRIPTOR_HEAP_DESC dh{};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = 1;
    if (FAILED(m_device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_dsvHeap))))
      return false;
    m_device->CreateDepthStencilView(m_geoDepth.Get(), nullptr,
                                     m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  // Shader-visible heaps: the SRV heap (createXlatSrvHeap), then the sampler heap.
  if (!createXlatSrvHeap()) return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  m_texKeys.clear();  // fresh slots: no key may match a table from before
  m_viewIds.clear(); m_viewIdCount = 0;
  m_drawSamplers = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  // iter-466 gfx: kSampBlocks blocks of the sampler table (kXlatSamplerTable entries: the 16 legacy recipes, then
  // the volume samplers of SR_XLAT_SAMPLER_V2); block 0 is the game's own filtering (written once below; a new
  // volume sampler only adds its entry), the others carry the anisotropic / LOD bias options.
  hd.NumDescriptors = kXlatSamplerTable * kSampBlocks;
  hr = m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_xlatSamplerHeap));
  if (FAILED(hr)) return false;

  // 1×1 white fallback textures (2D / 3D / cube) — v1 stands in for the real
  // texture cache; every descriptor index the shared constants publish is 0.
  auto makeWhite = [&](D3D12_RESOURCE_DIMENSION dim, UINT16 depthOrArray,
                       Microsoft::WRL::ComPtr<ID3D12Resource>& tex) -> bool {
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = dim;
    td.Width = 1;
    td.Height = 1;
    td.DepthOrArraySize = depthOrArray;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    return SUCCEEDED(m_device->CreateCommittedResource(
        &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&tex)));
  };
  if (!makeWhite(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, m_xlatWhite2D)) return false;
  if (!makeWhite(D3D12_RESOURCE_DIMENSION_TEXTURE3D, 1, m_xlatWhite3D)) return false;
  if (!makeWhite(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 6, m_xlatWhiteCube)) return false;

  // One-off upload of the white texel(s) through a small upload buffer.
  {
    D3D12_RESOURCE_DESC sd{};
    sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    sd.Width = 4096;
    sd.Height = 1;
    sd.DepthOrArraySize = 1;
    sd.MipLevels = 1;
    sd.SampleDesc.Count = 1;
    sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    hr = m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &sd,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           IID_PPV_ARGS(&staging));
    if (FAILED(hr)) return false;
    uint8_t* p = nullptr;
    if (FAILED(staging->Map(0, &noRead, reinterpret_cast<void**>(&p)))) return false;
    std::memset(p, 0xFF, 4096);
    staging->Unmap(0, nullptr);

    clReset(XlatReset);
    if (m_clState != ClState::Recording) return false;
    auto copyOne = [&](ID3D12Resource* dst, UINT sub) {
      D3D12_TEXTURE_COPY_LOCATION d{}, s{};
      d.pResource = dst;
      d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      d.SubresourceIndex = sub;
      s.pResource = staging.Get();
      s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      s.PlacedFootprint.Footprint.Width = 1;
      s.PlacedFootprint.Footprint.Height = 1;
      s.PlacedFootprint.Footprint.Depth = 1;
      s.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
      m_commandList->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    };
    copyOne(m_xlatWhite2D.Get(), 0);
    copyOne(m_xlatWhite3D.Get(), 0);
    for (UINT f = 0; f < 6; f++) copyOne(m_xlatWhiteCube.Get(), f);
    resourceBarrier(m_commandList.Get(), m_xlatWhite2D.Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    resourceBarrier(m_commandList.Get(), m_xlatWhite3D.Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    resourceBarrier(m_commandList.Get(), m_xlatWhiteCube.Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    clClose(XlatClose);
    clExecute(XlatExec);
    waitForGpu();
  }

  // SRV descriptors: every 2D slot (space0 table, heap 0-7) = white 2D;
  // space1 table (heap 8-15) = white 3D; space2 table (heap 16-23) = cube.
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
  // iter-462 (SR_PM4_NULL_ZERO): a fetch with no texture behind it samples
  // (0,0,0,0), as Xenia's null SRV does (d3d12_texture_cache.cc:380-390, all
  // four channels FORCE_VALUE_0). White made soft particles' fade term 0, so
  // steam/smoke/fire trails drew with alpha 0 (Xenia draws them from the same
  // trace bytes). Default ON: replay gate archive/pm4_score/v1nz1 vs v1nz0 -
  // lg1 8.43 -> 5.50, lg2 7.36 -> 5.14, gp2 3.61 -> 3.36, gp1 4.89 -> 4.86,
  // menu 2.70 and the four FMV traces unchanged. =0 keeps the white A/B.
  static const bool s_nullZero = [] {
    const char* e = std::getenv("SR_PM4_NULL_ZERO");
    return !e || e[0] != '0';
  }();
  const UINT nullMap = s_nullZero ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                                        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                                        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                                        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                                        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0)
                                  : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  for (uint32_t i = 0; i < kTex2DSlots; i++) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = nullMap;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(i) * inc};
    publishXlatSrv(m_xlatWhite2D.Get(), &sv, h);
    if (i == 0) m_texNullSrv = sv;  // step 4 G7 (SR_TEX_CACHE_EVICT): a freed slot goes back to this view
  }
  texEvictInit();
  for (int i = 0; i < 8; i++) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = nullMap;
    sv.Texture3D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(kTex2DSlots + i) * inc};
    publishXlatSrv(m_xlatWhite3D.Get(), &sv, h);
  }
  // ── iter-457 DIAGNOSTIC: SR_HLE_CUBE_NULL_ZERO (default OFF) ──────────────
  // H0: does the WHITE cube fallback materially contribute to a title-scene
  // defect? Nothing publishes a real guest cube yet, so every cube-fetching
  // draw reads THIS dummy (shared[32..47] is zero-init -> index 0 -> the cube
  // table's first descriptor, which is this white 6-face resource). The
  // reference's nulls are 1x1 R8_UNORM with an ALL-ZERO component mapping
  // (UnleashedRecomp video.cpp:1898-1945), so a missing texture multiplies as
  // 0.0 there and as 1.0 here.
  //
  // This gate changes ONLY the component mapping of the EXISTING cube null.
  // It does not touch the 2D or 3D nulls, does not add a dummy, and does not
  // publish any real cube index. A visible delta does NOT prove real-cube
  // correctness; a null result does NOT kill the real-cube hypothesis. It is a
  // diagnostic, never a fix, and it stays default OFF.
  static const bool s_cubeNullZero = [] {
    const char* e = std::getenv("SR_HLE_CUBE_NULL_ZERO");
    return e && e[0] == '1';
  }();
  for (int i = 0; i < 8; i++) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    sv.Shader4ComponentMapping =
        s_nullZero ? nullMap
        : s_cubeNullZero
            ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0)
            : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.TextureCube.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(kTex2DSlots + 8 + i) * inc};
    publishXlatSrv(m_xlatWhiteCube.Get(), &sv, h);
  }
  // Dedicated texture-upload list + fence (never touches the frame list).
  if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&m_texUploadAlloc))))
    return false;
  if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         m_texUploadAlloc.Get(), nullptr,
                                         IID_PPV_ARGS(&m_texUploadList))))
    return false;
  m_texUploadList->Close();
  if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_texFence))))
    return false;
  m_texFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!m_texFenceEvent) return false;

  // iter-411: a SAMPLER VARIANT TABLE, replacing 16 identical linear-WRAP
  // samplers. Two separate defects met here:
  //  1. Every sampler was WRAP. The Xenos fetch constant carries clamp_x/y/z
  //     (xenos.h:1251, ClampMode) and mag/min/mip filters (xenos.h:1301), and
  //     the guest's UI/post surfaces are clamp-to-edge. Sampling a full-screen
  //     quad with WRAP pulls the opposite edge in wherever the texcoord grazes
  //     outside [0,1] — the vertical streaking visible in the replay.
  //  2. The shader ABI's SAMPLER descriptor indices (SharedConstants c12..c15,
  //     per XenosRecomp shader_recompiler.cpp:1293) were NEVER written, so every
  //     draw read index 0. That was invisible while all 16 samplers were
  //     identical; it becomes load-bearing the moment they differ, so the index
  //     write lands in the same change as the table.
  // Layout: index = clampV*2 + linear, then clampU selects the block of 4.
  //   idx = (clampU ? 4 : 0) | (clampV ? 2 : 0) | (linear ? 1 : 0)
  m_sampInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  xlatWriteSamplerBlock(0, 0, 0.0f);   // the game's own filtering (iter-466: moved into the helper)

  // iter-464 input lane: the prompt-glyph replacement band (gfx/glyph_subst.hpp). One
  // slot per (icon, glyph set) the run ever shows; reserved now, while the bump
  // allocator is fresh, so the LRU texture pool can never starve it.
  if (glyphs::init() && m_texNextSlot + kGlyphSlots < kTex2DSlots) {
    m_glyphIcon.assign(kTex2DSlots, 0);
    m_glyphBasePage.assign(kTex2DSlots, 0);
    m_glyphNextSlot = m_texNextSlot;
    m_texNextSlot += kGlyphSlots;
    m_glyphSlotEnd = m_texNextSlot;
    m_glyphOn = true;
  }

  return true;
}

// iter-466 gfx (PORT FEATURE, docs/GRAPHICS_SETTINGS.md): one 16-sampler block of the xlat table.
// Layout as iter-411: idx = (clampU ? 4 : 0) | (clampV ? 2 : 0) | (linear ? 1 : 0). `aniso` >= 2 turns
// the LINEAR variants anisotropic (Xenia's anisotropic_override; the point variants stay point);
// `lodBias` is added to every sampler (MipLODBias). Block 0 = (0, 0.0) = the iter-411 table, byte for
// byte for the active fields: same filter, addressing, MaxLOD and MipLODBias;
// ignored anisotropy/comparison fields now use legal native values.
void D3D12Backend::xlatWriteSamplerBlock(uint32_t block, uint32_t aniso, float lodBias) {
  TranslatedSamplerRecipes recipes;
  const HRESULT hr = translatedSamplerRecipes(aniso, lodBias, m_volSamplers, recipes);
  if (FAILED(hr)) nativeRecordingFail("legacy-sampler-recipe", hr);
  const auto sCpu = m_xlatSamplerHeap->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = 0; i < recipes.size(); ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h{sCpu.ptr + SIZE_T(block * kXlatSamplerTable + i) * m_sampInc};
    m_device->CreateSampler(&recipes[i].sampler, h);
  }
}

// The sampler table the next draws bind: block 0 while the options are at their defaults, else a
// block written for the current options. A change writes the NEXT block of a ring (1..kSampBlocks-1),
// never the block in use, so frames in flight keep valid descriptors (one change per options
// version; a ring lap takes kSampBlocks - 1 changes, far more than the frames in flight).
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::xlatSamplerTable() {
  const uint64_t v = gfxopt::version();
  if (v != m_sampVersion) {
    m_sampVersion = v;
    const gfxopt::Settings s = gfxopt::current();
    const uint32_t aniso = s.anisotropic >= 2 ? uint32_t(s.anisotropic) : 0u;
    if (aniso == 0 && s.lodBias == 0.0f) {
      m_sampBlock = 0;
      m_sampAniso = 0;
      m_sampLodBias = 0.0f;
    } else {
      m_sampRing = m_sampRing % (kSampBlocks - 1) + 1;
      xlatWriteSamplerBlock(m_sampRing, aniso, s.lodBias);
      m_sampBlock = m_sampRing;
      m_sampAniso = aniso;
      m_sampLodBias = s.lodBias;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[GFX_OPT] sampler table block %u (anisotropic %u, lod bias %.2f)",
                m_sampBlock, aniso, double(s.lodBias));
  }
  D3D12_GPU_DESCRIPTOR_HANDLE h = m_xlatSamplerHeap->GetGPUDescriptorHandleForHeapStart();
  h.ptr += uint64_t(m_sampBlock) * kXlatSamplerTable * m_sampInc;
  return h;
}

// Guest texture fetch constant (6 dwords, stubs.cpp:2806 layout) → SRV heap slot.
// v1: k_8_8_8_8 (untiled via xenos_tiling when tiled) + DXT1/2_3/4_5 as BC1-3
// (linear copy; tiled DXT logged as approximate). Failure → 0 (white).
namespace {
// SR_TEX_DUMP / SR_RESOLVE_DUMP diagnostics (iter-359h content hunt): write
// what the texture cache decoded / what each resolve read back, so "wrong
// texture vs wrong decode vs wrong pass content" is settled by looking.
bool texDumpEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_TEX_DUMP"); return e && e[0] == '1'; }();
  return v;
}
bool resolveDumpEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_RESOLVE_DUMP"); return e && e[0] == '1'; }();
  return v;
}
// iter-413: depth-as-texture (k_24_8 / k_24_8_FLOAT). SR_TEX_DEPTHFMT=0
// restores the pre-iter-413 behaviour (fail() -> WHITE) for a clean A/B.
bool texDepthFmtEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_TEX_DEPTHFMT"); return !e || e[0] != '0'; }();
  return v;
}
// iter-408: content revalidation of cached textures (SR_TEX_REVALIDATE=0 to
// restore the old upload-once behaviour for A/B).
bool texRevalidateEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_TEX_REVALIDATE"); return !e || e[0] != '0'; }();
  return v;
}
// step 4 G7 (lane D): SR_TEX_CACHE_EVICT - the 2D texture cache's bound (D3D12Backend::texEvict*, the policy in
// gfx/tex_evict_policy.hpp). BAKED ON (step 4 G7 bake: L-D1 hitch, L-D2 reload witness, lane C's replay identity +
// forced-eviction arms + flicker on the baked package); =0 = the cache as before, whose only bound is the 8192-slot
// LRU. Knobs: SR_TEX_CACHE_AGE_FRAMES (7200: the leak's bound), SR_TEX_CACHE_BUDGET_MB (768: a backstop normal play
// never reaches - L-D1 measured Metropolis's working set at 156 MB and the whole cache's live peak at 168 MB, and
// other levels are unmeasured, so it sits at ~4.6x that rather than at 2x one level; 0 = no budget).
static bool texCacheEvictEnabled() {
  // step 6 I3-B (bindless): always on. The gate-off legacy LRU rewrites a victim's slot in place, which a bindless
  // draw already recorded would read; SR_TEX_CACHE_EVICT=0 is refused (logged once).
  static const bool v = [] {
    const char* e = std::getenv("SR_TEX_CACHE_EVICT");
    if (e && e[0] == '0')
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[TEX_CACHE_EVICT] SR_TEX_CACHE_EVICT=0 refused: the bindless texture path "
                  "needs the fence-retired slot pool (step 6 I3-B)");
    return true;
  }();
  return v;
}
static constexpr uint64_t kTexSweepFrames = 300;   // a sweep every 300 frames (~5 s at 60)
static constexpr uint64_t kTexKeepFrames = 30;     // the budget never takes an entry bound within 30 frames
static constexpr uint32_t kTexBurstCreates = 256;  // this many creates since the last sweep: sweep at the next frame
static constexpr size_t kTexReleasePerFrame = 64;  // retired slots released per frame (resource frees spread out)
static constexpr size_t kTexSweepMaxEvict = 512;   // fix2: a sweep evicts at most 512, the rest at the next frame

// iter-466 res4k: GPU-loaded 2D textures shared by identity across fetch constants that differ
// only in sampler state / swizzle (D3D12Backend::TexShared). Baked default ON (replay md5-identical
// 1x 4/4, 3x 7/7; live 3x A/B 2026-10-06); =0 keeps the A/B.
bool texShareEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_TEX_SHARE"); return !e || e[0] != '0'; }();
  return v;
}
// rows are BGRA (BMP convention), top-down.
// iter-447: returns whether the file was actually written. Every caller used
// to ignore the silent `if (!fp) return;` and then LOG THE DUMP AS DONE — a
// roll pointed at a non-existent SR_PRESENT_DUMP_DIR logged 54 successful
// dumps and wrote ZERO files. The instrument could not fail; now it can.
bool writeBmp32(const char* name, const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t srcPitch) {
  FILE* fp = fopen(name, "wb");
  if (!fp) {
    static std::atomic<uint32_t> s_wfail{0};
    if (s_wfail.fetch_add(1, std::memory_order_relaxed) < 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[PRESENT_DUMP] WRITE FAILED %s (directory missing? fopen errno=%d)",
          name, errno);
    return false;
  }
  const uint32_t img = w * h * 4u;
  uint8_t fh[14] = {'B', 'M'};
  uint32_t fsz = 54 + img, off = 54;
  std::memcpy(fh + 2, &fsz, 4); std::memcpy(fh + 10, &off, 4);
  fwrite(fh, 1, 14, fp);
  uint8_t ih[40] = {};
  uint32_t ihs = 40; int32_t bw = int32_t(w), bh = -int32_t(h);
  uint16_t planes = 1, bpp = 32;
  std::memcpy(ih, &ihs, 4); std::memcpy(ih + 4, &bw, 4); std::memcpy(ih + 8, &bh, 4);
  std::memcpy(ih + 12, &planes, 2); std::memcpy(ih + 14, &bpp, 2); std::memcpy(ih + 20, &img, 4);
  fwrite(ih, 1, 40, fp);
  for (uint32_t y = 0; y < h; y++) fwrite(bgra + size_t(y) * srcPitch, 1, size_t(w) * 4, fp);
  fclose(fp);
  return true;
}
// Minimal DDS wrapper for BC payloads (PIL opens DXT1/DXT3/DXT5/ATI2).
void writeDds(const char* name, const uint8_t* data, size_t bytes, uint32_t w, uint32_t h,
              const char fourcc[4]) {
  FILE* fp = fopen(name, "wb");
  if (!fp) return;
  uint32_t hdr[32] = {};
  hdr[0] = 0x20534444;  // 'DDS '
  hdr[1] = 124;
  hdr[2] = 0x1007 | 0x80000;  // CAPS|HEIGHT|WIDTH|PIXELFORMAT | LINEARSIZE
  hdr[3] = h; hdr[4] = w;
  hdr[5] = uint32_t(bytes);
  hdr[19] = 32;          // ddspf size
  hdr[20] = 0x4;         // DDPF_FOURCC
  std::memcpy(&hdr[21], fourcc, 4);
  hdr[27] = 0x1000;      // DDSCAPS_TEXTURE
  fwrite(hdr, 1, 128, fp);
  fwrite(data, 1, bytes, fp);
  fclose(fp);
}
}  // namespace

// iter-437c [DRAIN_PROF] plumbing: cheap QPC timers + phase accumulators. The
// drain is the present-rate bottleneck under the game's real resolve traffic
// (96K resolves/run since the iter-437 Resolve hook) — these name the cost.
static int64_t profQpc() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return li.QuadPart;
}
static int64_t profQpcFreq() {
  static const int64_t f = [] {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
  }();
  return f;
}
static uint64_t profUs(int64_t dt) {
  return uint64_t(dt * 1000000 / profQpcFreq());
}
static std::atomic<uint64_t> profResolveN{0}, profResolveWaitUs{0},
    profResolveConvUs{0}, profEvictUs{0}, profSnapshotUs{0}, profDrawUs{0},
    profRenderUs{0},
    // iter-437d judder attribution: the ~35ms/present missing from renderMs.
    profDrainUs{0}, profBlitUs{0}, profPresentUs{0}, profPaceUs{0},
    // iter-438d: the composite-era collapse suspects — total texture-bind
    // (decode+upload) time and the count of bind-overlap full-GPU-sync flushes.
    profTexBindUs{0}, profBindFlushN{0};

#if SR_ORACLE
// iter-437c: deferred-resolve readback pool. Sized for a worst-case drain
// (~100 resolves, several full-frame): flush-on-full handles overflow.
static constexpr uint64_t kResolveRbBytes = 64ull << 20;

int D3D12Backend::findResolveTexSlot(uint32_t basePhys) const {
  const uint32_t bp = basePhys & 0x1FFFFFFFu;
  for (const auto& r : m_resolveTextures)
    if ((r.basePhys & 0x1FFFFFFFu) == bp) return int(r.slot);
  return -1;
}

// iter-438d: creation-only variant for the LIVE deferred path — the copy is
// recorded IN-STREAM by recordPendingResolve (stream-ordered after the pass's
// draws), so no upload-list execute / fence here. Resource at rest in
// PIXEL_SHADER_RESOURCE. On dimension mismatch the entry is recreated (step 6
// I3-B: in a NEW SRV slot, copy-on-write; the old slot retires with the fence).
D3D12Backend::ResolveTex* D3D12Backend::ensureResolveTex(uint32_t destPhys,
                                                         uint32_t w, uint32_t h,
                                                         DXGI_FORMAT fmt) {
  if (!w || !h) return nullptr;
  const uint32_t bp = destPhys & 0x1FFFFFFFu;
  ResolveTex* rt = nullptr;
  for (auto& r : m_resolveTextures)
    if ((r.basePhys & 0x1FFFFFFFu) == bp) { rt = &r; break; }
  if (rt && rt->w == w && rt->h == h && rt->res) return rt;
  uint32_t slot = 0;
  if (!rt) {
    if (!texSlotAvailable()) return nullptr;
    slot = texSlotAlloc();
  }
  D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
  td.Format = fmt; td.SampleDesc.Count = 1;
  Microsoft::WRL::ComPtr<ID3D12Resource> res;
  if (FAILED(m_device->CreateCommittedResource(
          &def, D3D12_HEAP_FLAG_NONE, &td,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&res))))
    return nullptr;
  if (rt) {
    // step 6 I3-B: copy-on-write - the old slot keeps the old view until the GPU is done with it
    slot = texSlotCow(rt->slot, rt->res, "ensureResolveTex");
    if (!slot) return nullptr;  // no slot: the entry stays as it was, this resolve is skipped
    rt->slot = slot;
  }
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = fmt;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE hcpu{
      m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
      SIZE_T(slot) * inc};
  publishXlatSrv(res.Get(), &sv, hcpu);
  if (rt) {
    rt->w = w; rt->h = h; rt->res = std::move(res);
    return rt;
  }
  ResolveTex nr{};
  nr.basePhys = destPhys;
  nr.w = w; nr.h = h;
  nr.slot = slot;
  nr.res = std::move(res);
  m_resolveTextures.push_back(std::move(nr));
  return &m_resolveTextures.back();
}

// Snapshot the current RT (post-pass) into a persistent GPU texture keyed by
// the resolve dest address. Runs on the dedicated texture-upload list (the
// frame list is between segments here; m_geoRT is in RENDER_TARGET state).
void D3D12Backend::snapshotResolveTexture(uint32_t destPhys, uint32_t w, uint32_t h,
                                          bool hdrSource) {
  ID3D12Resource* srcRT = hdrSource ? m_sceneHdrRT.Get() : m_geoRT.Get();
  const DXGI_FORMAT fmt = hdrSource ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                    : DXGI_FORMAT_R8G8B8A8_UNORM;
  if (!srcRT || !w || !h) return;
  if (w > uint32_t(m_width)) w = uint32_t(m_width);
  if (h > uint32_t(m_height)) h = uint32_t(m_height);
  const uint32_t bp = destPhys & 0x1FFFFFFFu;

  ResolveTex* rt = nullptr;
  for (auto& r : m_resolveTextures)
    if ((r.basePhys & 0x1FFFFFFFu) == bp) { rt = &r; break; }

  if (!rt) {
    if (!texSlotAvailable()) return;  // slot pool shared with sampled textures
    ResolveTex nr{};
    nr.basePhys = destPhys;
    nr.w = w; nr.h = h;
    nr.slot = texSlotAlloc();
    D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    if (FAILED(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&nr.res))))
      return;
    // SRV into the shared 2D table slot.
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE hcpu{
        m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(nr.slot) * inc};
    publishXlatSrv(nr.res.Get(), &sv, hcpu);
    m_resolveTextures.push_back(std::move(nr));
    rt = &m_resolveTextures.back();
  }

  // Copy the RT sub-rect into the persistent texture on the dedicated list.
  if (FAILED(m_texUploadAlloc->Reset())) return;
  if (FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) return;
  resourceBarrier(m_texUploadList.Get(), rt->res.Get(),
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  resourceBarrier(m_texUploadList.Get(), srcRT,
                  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION d{}, s{};
  d.pResource = rt->res.Get(); d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
  s.pResource = srcRT; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
  D3D12_BOX box{0, 0, 0, w, h, 1};
  m_texUploadList->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
  resourceBarrier(m_texUploadList.Get(), srcRT,
                  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  resourceBarrier(m_texUploadList.Get(), rt->res.Get(),
                  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_texUploadList->Close();
  ID3D12CommandList* lists[] = {m_texUploadList.Get()};
  m_commandQueue->ExecuteCommandLists(1, lists);
  m_texFenceValue++;
  m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
  if (m_texFence->GetCompletedValue() < m_texFenceValue) {
    m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
    WaitForSingleObject(m_texFenceEvent, INFINITE);
  }

  // iter-359n: dump what this snapshot captured (SR_RESOLVE_DUMP) — the
  // left/right split hunt needs to see whether the scene surface itself is
  // partial or the composite geometry only covers half.
  if (resolveDumpEnabled() && m_readbackBuffer) {
    D3D12_RESOURCE_DESC rd = rt->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT nr = 0; UINT64 rs = 0, tb = 0;
    m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &nr, &rs, &tb);
    if (tb <= size_t(m_readbackRowPitch) * 2u * m_height) {  // readback buffer capacity
      if (SUCCEEDED(m_texUploadAlloc->Reset()) &&
          SUCCEEDED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) {
        resourceBarrier(m_texUploadList.Get(), rt->res.Get(),
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION cd{}, cs{};
        cd.pResource = m_readbackBuffer.Get(); cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; cd.PlacedFootprint = fp;
        cs.pResource = rt->res.Get(); cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; cs.SubresourceIndex = 0;
        m_texUploadList->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
        resourceBarrier(m_texUploadList.Get(), rt->res.Get(),
                        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_texUploadList->Close();
        ID3D12CommandList* l2[] = {m_texUploadList.Get()};
        m_commandQueue->ExecuteCommandLists(1, l2);
        m_texFenceValue++;
        m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
        if (m_texFence->GetCompletedValue() < m_texFenceValue) {
          m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
          WaitForSingleObject(m_texFenceEvent, INFINITE);
        }
        void* mp = nullptr; D3D12_RANGE rr{0, size_t(tb)};
        if (SUCCEEDED(m_readbackBuffer->Map(0, &rr, &mp))) {
          const bool hdr = (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT);
          std::vector<uint8_t> bgra(size_t(w) * h * 4u);
          auto h2f = [](uint16_t v) -> float {
            uint32_t s = (v >> 15) & 1, e = (v >> 10) & 0x1F, m = v & 0x3FF, out;
            if (e == 0) out = m ? ((s << 31) | ((127 - 15 + 1) << 23) | (m << 13)) : (s << 31);
            else if (e == 31) out = (s << 31) | 0x7F800000 | (m << 13);
            else out = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
            float f; std::memcpy(&f, &out, 4); return f;
          };
          for (uint32_t y = 0; y < h; y++) {
            const uint8_t* row = static_cast<const uint8_t*>(mp) + size_t(y) * fp.Footprint.RowPitch;
            for (uint32_t x = 0; x < w; x++) {
              uint8_t r8, g8, b8;
              if (hdr) {
                const uint16_t* px = reinterpret_cast<const uint16_t*>(row + size_t(x) * 8);
                auto tm = [&](uint16_t hv) { float f = h2f(hv); f = f / (f + 1.0f); return uint8_t(f < 0 ? 0 : f > 1 ? 255 : f * 255.0f); };
                r8 = tm(px[0]); g8 = tm(px[1]); b8 = tm(px[2]);
              } else {
                const uint8_t* px = row + size_t(x) * 4; r8 = px[0]; g8 = px[1]; b8 = px[2];
              }
              uint8_t* dp = bgra.data() + (size_t(y) * w + x) * 4;
              dp[0] = b8; dp[1] = g8; dp[2] = r8; dp[3] = 255;
            }
          }
          char nm[128];
          snprintf(nm, sizeof(nm), "snapdump_0x%08X_%s_%ux%u.bmp", destPhys, hdr ? "hdr" : "ldr", w, h);
          writeBmp32(nm, bgra.data(), w, h, w * 4);
          m_readbackBuffer->Unmap(0, nullptr);
        }
      }
    }
  }
}

#endif  // SR_ORACLE
// iter-359l: resolve-produces-texture is gated OFF by default. It proves the
// render-to-texture path (the composite samples the scene we rendered — the
// white background goes away), but the minimal single-RT snapshot is R8G8B8A8:
// the composite fetches the scene DEPTH (k_24_8_FLOAT) and HDR color
// (k_16_16_16_16_EXPAND) in their real EDRAM formats, so color-bytes-as-depth
// corrupts the tonemap (yellow flood). The clean fix is per-format surfaces
// (HDR R16F color RT + real depth), the EDRAM-surface-model completion.
static bool rttEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_XLAT_RTT"); return e && e[0] == '1'; }();
  return v;
}

#if SR_ORACLE
// iter-438d: GPU-side resolve textures on the LIVE path (default ON, =0 A/B).
// Every recorded resolve also copies the RT into a persistent bindable texture
// so later binds skip the guest-memory decode round trip entirely.
static bool resolveTexSnapEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_RESOLVE_TEXSNAP"); return !e || e[0] != '0'; }();
  return v;
}

// iter-439 P1: honor the resolve's SOURCE RECT (tile sub-rect resolves).
// OBSERVE-FIRST: default OFF until a live roll confirms the rect values the
// emit passes (then flip default and run the full verification gate).
static bool resolveSrcRectEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_RESOLVE_SRCRECT"); return e && e[0] == '1'; }();
  return v;
}

// iter-439 P2: async drain-end resolve flush (fence-signal + defer the convert
// to a later drain) instead of a full waitForGpu every drain. SR_RESOLVE_ASYNC=0
// restores the iter-437c synchronous flush for A/B.
static bool resolveAsyncEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_RESOLVE_ASYNC"); return !e || e[0] != '0'; }();
  return v;
}

// iter-439d: mid-frame continuation drains keep the RT (no clear, draw once)
// instead of erasing the frame's first half. SR_FRAME_CONT=0 restores the
// clear-every-drain behavior for A/B. See the hpp note at m_thisDrainContinuesFrame.
static bool frameContEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_FRAME_CONT"); return !e || e[0] != '0'; }();
  return v;
}

// iter-439g POST-MORTEM (roll G contact sheet, user-caught): presenting only
// boundary-complete frames REPLACED THE MOVIES WITH SOLID MAROON for ~50s —
// the sets that end at a VdSwap marker are the maroon post-clear state, and
// the actual movie-quad sets are the "incomplete" ones this gate suppressed.
// The VdSwap-marker/frame alignment is NOT understood well enough to gate
// presentation on it. Default OFF until that alignment is proven.
static bool presentCompleteEnabled() {
  static const bool v = [] { const char* e = std::getenv("SR_PRESENT_COMPLETE"); return e && e[0] == '1'; }();
  return v;
}

#endif  // SR_ORACLE
// iter-419: the low 2 bits of a fetch constant's first dword are its TYPE, not a
// dimension - Xenia xenos.h:1147. Naming them keeps the logs honest: a slot that
// reads kVertex is a vertex fetch legitimately sharing those dwords (the
// xe_gpu_fetch_group_t union, xenos.h:1333), not a corrupt texture header.
static const char* xeFetchTypeName(uint32_t t) {
  switch (t) {
    case 0: return "kInvalidTexture";
    case 1: return "kInvalidVertex";
    case 2: return "kTexture";
    default: return "kVertex";
  }
}

// iter-413: Xenos 20e4 depth -> IEEE float32, transcribed from Xenia
// (src/xenia/gpu/xenos.cc:151 Float20e4To32). A 24-bit float with a 20-bit
// mantissa and 4-bit exponent, bias 112 relative to IEEE's 127.
static float xeFloat20e4To32(uint32_t f24) {
  f24 &= 0xFFFFFFu;
  if (!f24) return 0.0f;
  uint32_t mantissa = f24 & 0xFFFFFu;
  uint32_t exponent = f24 >> 20;
  if (!exponent) {
    // Denormal: normalise into the destination float (Xenia's lzcnt path).
    uint32_t lz = 0;
    while (lz < 21 && !(mantissa & (1u << (20 - lz)))) lz++;
    exponent = uint32_t(1 - int32_t(lz));
    mantissa = (mantissa << lz) & 0xFFFFFu;
  }
  const uint32_t bits = ((exponent + 112u) << 23) | (mantissa << 3);
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

// iter-408: extracted verbatim from getOrCreateTexture2D so the create path and
// the refresh path stage bytes identically. fromShaderResource distinguishes a
// freshly created resource (already in COPY_DEST) from a live cached one that
// must be transitioned out of PIXEL_SHADER_RESOURCE and back.
bool D3D12Backend::uploadTexture2D(ID3D12Resource* tex, const uint8_t* lin,
                                   uint32_t rows, uint32_t rowBytes, uint32_t width,
                                   uint32_t height, DXGI_FORMAT dxgi,
                                   bool fromShaderResource, uint32_t depth,
                                   uint32_t subresource) {
  static std::atomic<uint32_t> s_upFailLog{0};
  auto failUp = [&](const char* what, HRESULT chr) -> bool {
    if (s_upFailLog.fetch_add(1, std::memory_order_relaxed) < 12)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[TEX_FAIL] %s hr=0x%08X (not cached)",
                  what, (unsigned)chr);
    return false;
  };
  D3D12_HEAP_PROPERTIES up{};
  up.Type = D3D12_HEAP_TYPE_UPLOAD;
  const uint32_t upPitch = (rowBytes + 255u) & ~255u;
  D3D12_RESOURCE_DESC sd{};
  sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  sd.Width = uint64_t(upPitch) * rows; sd.Height = 1;
  sd.DepthOrArraySize = 1; sd.MipLevels = 1; sd.SampleDesc.Count = 1;
  sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> staging;
  HRESULT thr = m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &sd,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging));
  if (FAILED(thr)) return failUp("CreateCommittedResource(staging)", thr);
  uint8_t* mp = nullptr;
  D3D12_RANGE noRead{0, 0};
  thr = staging->Map(0, &noRead, reinterpret_cast<void**>(&mp));
  if (FAILED(thr)) return failUp("staging Map", thr);
  for (uint32_t y = 0; y < rows; y++)
    std::memcpy(mp + size_t(y) * upPitch, lin + size_t(y) * rowBytes, rowBytes);
  staging->Unmap(0, nullptr);

  // iter-460: a PM4 batch binding its textures records the copy on the frame
  // list, in order with the draws (earlier draws keep sampling the previous
  // bytes) - no dedicated submit, no GPU wait. The staging buffer retires with
  // the frame fence.
  ID3D12GraphicsCommandList* ul = m_texUploadList.Get();
  if (m_texInline) {
    ul = m_commandList.Get();
  } else {
    // Dedicated upload list — the FRAME list may be mid-recording here.
    thr = m_texUploadAlloc->Reset();
    if (FAILED(thr)) return failUp("texUploadAlloc Reset", thr);
    thr = m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr);
    if (FAILED(thr)) return failUp("texUploadList Reset", thr);
  }
  if (fromShaderResource)
    resourceBarrier(ul, tex,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION d{}, s{};
  d.pResource = tex;
  d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  // iter-457 H1(b): a cube writes 6 faces x N mips into ONE resource, so the
  // destination subresource has to be addressable. Default 0 is exactly what
  // the zero-init gave every existing caller, so their behaviour is unchanged.
  d.SubresourceIndex = subresource;
  s.pResource = staging.Get();
  s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  s.PlacedFootprint.Footprint.Format = dxgi;
  s.PlacedFootprint.Footprint.Width = width;
  s.PlacedFootprint.Footprint.Height = height;
  s.PlacedFootprint.Footprint.Depth = depth;
  s.PlacedFootprint.Footprint.RowPitch = upPitch;
  ul->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
  resourceBarrier(ul, tex,
                  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if (m_texInline) {
    retireResource(std::move(staging));
    return true;
  }
  m_texUploadList->Close();
  ID3D12CommandList* lists[] = {m_texUploadList.Get()};
  m_commandQueue->ExecuteCommandLists(1, lists);
  const UINT64 fv = ++m_texFenceValue;
  m_commandQueue->Signal(m_texFence.Get(), fv);
  if (m_texFence->GetCompletedValue() < fv) {
    m_texFence->SetEventOnCompletion(fv, m_texFenceEvent);
    WaitForSingleObject(m_texFenceEvent, 10000);
  }
  return true;
}

#if SR_ORACLE
// ── iter-442 S4.4: HOST-BYTES texture upload for the API-HLE movie path ─────
// Contract and rationale in the header. The bytes come from the producer-side
// arena (snapshotted at the guest's own Unlock), so unlike getOrCreateTexture2D
// this reads NO guest memory, hashes nothing, and shares no state with the
// resolve path that would evict it.
uint32_t D3D12Backend::hleUploadHostTexture(uint32_t key, const uint8_t* bytes,
                                            uint32_t pitch, uint32_t width,
                                            uint32_t height) {
  if (!m_initialized || !bytes || !width || !height || !pitch) return 0;
  // createXlatPipeline() failing leaves these null, and the harness VEH makes a
  // host AV fatal before any SEH filter runs — so check, never fault-and-catch.
  if (!m_xlatSrvHeap) {
    static bool s_said = false;
    if (!s_said) { s_said = true;
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[S4_TEXUP] no xlat SRV heap (createXlatPipeline failed) — planes cannot upload");
    }
    return 0;
  }
  HleTex& t = m_hleTextures[key];
  const bool dimsChanged = t.res && (t.w != width || t.h != height);
  if (!t.res || dimsChanged) {
    // fix3 (Astra 0a384c24 P2): gate on, the slot first. No slot = nothing changes (no resource swap, no SRV into slot
    // 0, the fallback every unrelated draw samples), and the next upload of the plane tries again.
    if (!t.slot && m_texEvictOn) {
      t.slot = texSlotAlloc();
      if (!t.slot) {
        texCreateFailLog("movie plane: no slot (retried at its next upload)", S_OK, nullptr, 0u, 0u, width, height);
        if (!t.res) m_hleTextures.erase(key);
        return 0;
      }
    }
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width; td.Height = height;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    if (FAILED(m_device->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&res)))) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[S4_TEXUP] CreateCommittedResource failed %ux%u key=%08X", width, height, key);
      return 0;
    }
    // ⛔ AUDIT FOLLOW-UP: this path had the SAME release-in-place lifetime bug
    // the resolve did — `t.res = res` dropped the last reference to a resource
    // an in-flight frame may still be sampling. Pre-existing, so the pre-bake
    // audit scoped it out; the retire helper was written shared so it could be
    // fixed here without inventing a second mechanism.
    if (t.res) {
      // step 6 I3-B: copy-on-write - a resized texture gets a NEW slot; the old slot retires with the old resource
      const uint32_t cow = texSlotCow(t.slot, t.res, "hle-upload-resize");
      if (!cow) return 0;  // no slot: the entry keeps its old texture (retried at its next upload)
      t.slot = cow;
    }
    t.res = res;
    // iter-450UE FIX B: the resource is being REPLACED, so every swizzled
    // view over the old one is now a stale descriptor. Invalidate here,
    // in the same statement group that swaps `res` — a view outliving its
    // resource is the one real hazard this feature adds.
    t.swizViews.clear();
    t.w = width; t.h = height;
    t.uploaded = false;
    // ⛔ AUDIT FIX (see the sibling upload path): `fmt` must be recorded here
    // too, or the resolve's re-create guard reads UNKNOWN and fires
    // destructively. This path is fixed-format — a single-channel movie plane.
    t.fmt = DXGI_FORMAT_R8_UNORM; t.fromResolve = false;
    t.state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (!t.slot) t.slot = m_texNextSlot++;   // ONCE per plane, never per frame (gate off, unchanged; gate on: above)
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    // Default mapping: the PS reads .x, and a plane is single-channel, so no
    // swizzle is wanted here (verified against the movie PS's DXIL).
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(t.slot) * inc};
    publishXlatSrv(t.res.Get(), &sv, h);
  }
  // rowBytes = the GUEST pitch: uploadTexture2D uses it as the SOURCE stride,
  // while the copy footprint's width comes from `width`. That is exactly the
  // measured contract (a 640-wide chroma plane has pitch 768) — passing width
  // here would shear every chroma plane.
  if (!uploadTexture2D(t.res.Get(), bytes, height, pitch, width, height,
                       DXGI_FORMAT_R8_UNORM, /*fromShaderResource=*/t.uploaded))
    return 0;
  t.uploaded = true;
  static uint64_t s_n = 0;
  const uint64_t n = ++s_n;
  if (n <= 12 || n % 600 == 0) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < 64 && i < pitch; i++) sum += bytes[i];
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[S4_TEXUP] #%llu key=%08X %ux%u pitch=%u -> slot=%u %s pool=%zu mean64=%llu",
        (unsigned long long)n, key, width, height, pitch, t.slot,
        dimsChanged ? "recreate" : (t.uploaded ? "reuse" : "create"),
        m_hleTextures.size(), (unsigned long long)(sum / 64));
  }
  return t.slot;
}
#endif

// ── iter-457 H1(b): THE SHARED GUEST-IMAGE DECODE ──────────────────────────
// Extracted from hleUploadHostTextureFmt below, unchanged, so the cube path
// can decode a FACE with the identical untiling, bounds checking and byte
// swap. Two copies of Xenos tiling would diverge - and the copy that diverged
// would be the one nobody looks at, because the 2D path is exercised on every
// frame and the cube path only in an env-mapped scene.
//
// Returns false for a format we will not guess at. NEVER invents a mapping: a
// wrong decode is indistinguishable from a bind or upload bug downstream.
bool guestFormatToDxgi(uint32_t fmt, DXGI_FORMAT& dxgi, uint32_t& blockW,
                       uint32_t& blockBytes) {
  blockW = 1; blockBytes = 4;
  switch (fmt) {
    case 6: case 50: dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
    case 18: dxgi = DXGI_FORMAT_BC1_UNORM; blockW = 4; blockBytes = 8; return true;
    case 19: dxgi = DXGI_FORMAT_BC2_UNORM; blockW = 4; blockBytes = 16; return true;
    case 20: dxgi = DXGI_FORMAT_BC3_UNORM; blockW = 4; blockBytes = 16; return true;
    case 49: dxgi = DXGI_FORMAT_BC5_UNORM; blockW = 4; blockBytes = 16; return true;
    case 24: dxgi = DXGI_FORMAT_R16_UNORM; blockBytes = 2; return true;
    case 36: dxgi = DXGI_FORMAT_R32_FLOAT; return true;
    case 2:  dxgi = DXGI_FORMAT_R8_UNORM;  blockBytes = 1; return true;   // k_8
    // iter-447: oracle-backed (Xenia texture_info.h:32 - EXPAND is FLOAT for
    // sampling). Half-float texels; endian rides the existing xeSwap32 path
    // (8in16 per dword is two independent halfword swaps, which is correct
    // for 16-bit channels).
    case 29: dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; blockBytes = 8; return true;
    case 31: dxgi = DXGI_FORMAT_R16G16_FLOAT;       blockBytes = 4; return true;
    default: dxgi = DXGI_FORMAT_UNKNOWN; return false;
  }
}

#if SR_ORACLE
// One decoded 2D image + the untiler live in gfx/guest_image.hpp so the cube
// uploader, the 2D uploader, and the TEXUNLOCK suite share ONE body.
// ── iter-457 H1(b): THE GUEST CUBE UPLOAD ──────────────────────────────────
//
// Authorized by the H1(a)-r7 verdict, whose contract this implements literally:
// TEXTURE2D with DepthOrArraySize = 6, a TEXTURECUBE SRV, real indices 1..7
// with index 0 remaining the white dummy, and ALL six faces plus all
// represented mips uploaded with named fail-closed refusals.
//
// EVERY decision here comes from gfx/cube_bind_rule.hpp, which is pure and
// mutation-tested (tools/mutate_cube_h1b.py). This function is the D3D12
// mechanics only. That split is deliberate: a cube bound or laid out wrongly
// samples a different texture and produces a plausible WRONG PICTURE rather
// than a crash, so the decisions belong somewhere a test can drive them
// without a device.
//
// ⛔ WHY FAIL-CLOSED IS THE WHOLE DESIGN. A five-face cube renders. It renders
// wrongly, in one direction only, and it reads as a bug in the scene rather
// than in the upload - which is how this class of defect survives for
// iterations. Refusing hands back 0, the caller keeps the white dummy, and the
// picture is exactly what it was before this slice existed.
uint32_t D3D12Backend::hleCubeSlotForKey(uint32_t key) const {
  return m_cubeTx.slotForKey(key & 0x1FFFFFFFu);
}

void D3D12Backend::hleCubeSumReport(const char* site) {
  // UNCAPPED, CONSERVATION-CHECKED. The numbers live in the line.
  // Do not count [S6_CUBE] lines - they are capped/sampled.
  // ⚠ audit_log_gating.py --tag S6_CUBE_SUM exits 2 (UNPROVED): that is a
  // fail-closed screen that says "no gate found", not proof of count safety.
  // The emitter is visibly uncapped by code inspection; the tool cannot
  // certify that structurally.
  // The tag is a LITERAL here on purpose: audit_log_gating.py finds emitters
  // by the "[TAG]" literal at the SR_LOG_BOOT call.
  char fields[300];
  sr::gfx::cubetx::formatSummaryFields(m_cubeTx.summary(), site, fields,
                                       sizeof(fields));
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[S6_CUBE_SUM] %s", fields);
}

uint32_t D3D12Backend::hleUploadCube(uint32_t key, const uint8_t* base,
                                     uint64_t baseBytes, const uint8_t* mipSrc,
                                     uint64_t mipBytes, uint32_t fmt,
                                     uint32_t width, uint32_t height,
                                     uint32_t pitchTexels, bool tiled,
                                     uint32_t endian, uint32_t mipLevels,
                                     bool packedMips,
                                     sr::gfx::cubebind::CubeRefuse& why) {
  namespace CB = sr::gfx::cubebind;
  namespace TX = sr::gfx::cubetx;
  why = CB::CubeRefuse::None;
  // Every entry is ONE attempt. Every exit below is exactly one of: a
  // refusal recorded HERE (pre-transaction), a refusal recorded INSIDE the
  // transaction, or a publication. That is what makes conserves=YES a
  // measurement rather than a hope.
  m_cubeTx.beginAttempt();
  auto refuse = [&](CB::CubeRefuse r) -> uint32_t {
    why = r;
    m_cubeTx.recordRefusal(r);
    return 0u;
  };
  if (!m_initialized || !m_xlatSrvHeap || !base || !baseBytes)
    return refuse(CB::CubeRefuse::ZeroExtent);

  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  uint32_t blockW = 1, blockBytes = 4;
  const bool fmtOk = guestFormatToDxgi(fmt, dxgi, blockW, blockBytes);

  // The RESOURCE decision, including every refusal, is the rule's to make.
  const auto plan = CB::planResource(CB::Dim::Cube, width, height, mipLevels,
                                     fmtOk);
  if (!plan.ok) return refuse(plan.refuse);

  // ── THE LAYOUT ──────────────────────────────────────────────────────────
  // Face stride is NOT 6*w*h*bpp. It is the tile-aligned row pitch times the
  // tile-aligned block-row count, aligned up to 4096 - the H1(a) work exists
  // because those two disagree at 33x33 (32768 vs an intuitive 4356), and the
  // 128x128 cubes in the frozen fixture agree only by coincidence.
  const auto lay = sr::gfx::cube::computeCubeLayout(
      fmt, width, height, pitchTexels, tiled, mipLevels, packedMips,
      blockW, blockW, blockBytes);
  if (!lay.ok) return refuse(CB::CubeRefuse::UnsupportedFormat);

  // Conservation: the six faces must exactly span what the guest handed us.
  const uint64_t needBase = uint64_t(lay.faceStrideBytes) * CB::kCubeFaces;
  if (baseBytes < needBase) return refuse(CB::CubeRefuse::ByteCountMismatch);
  // Mip data is only required when the layout says a separate mip region
  // exists. A packed tail with no separate region is not a missing mip.
  const bool wantMips = lay.mipsTotalExtentBytes > 0;
  if (wantMips && (!mipSrc || mipBytes < lay.mipsTotalExtentBytes))
    return refuse(CB::CubeRefuse::MipDataMissing);

  const auto up = CB::planUpload(plan, CB::kCubeFaces, plan.mipLevels,
                                 needBase, needBase);
  if (!up.ok) return refuse(up.refuse);

  const uint32_t k = key & 0x1FFFFFFFu;

  // ── THE DECODE: ALL 48 SUBRESOURCES, OR NOTHING ─────────────────────────
  // ⛔ TWO DEFECTS THE DIRECTOR FOUND IN r1, both fixed here.
  //
  // 1. r1 looped mip 0..7 and refused every level above `lastStoredLevel` (3)
  //    as MipDataMissing - so the ONLY cube shape the guest creates was
  //    refused on mip 4, every time. Packed levels 3..7 SHARE the tail slice
  //    stored at level 3, each at its own Xenos (x, y). Placement is now
  //    cube::placeSubresource's decision, which the suite drives for all 48.
  //
  // 2. r1 called uploadTexture2D per subresource. That helper transitions the
  //    WHOLE resource COPY_DEST -> PIXEL_SHADER_RESOURCE after each copy,
  //    while `c.uploaded` stayed false for all 48 - so copy 2 declared a
  //    before-state of COPY_DEST for a resource already in shader-read, and a
  //    failure partway left a PARTIALLY WRITTEN cube while the code claimed
  //    "nothing is written". Now: stage everything, copy under ONE COPY_DEST
  //    interval, transition ONCE at the end - inside the transaction.
  using Sub = TX::StagedSub;
  std::vector<Sub> subs;
  subs.reserve(up.subresources);

  // Decoded tail slices. packed_level == 0 has TWO tails per face (base and
  // mip); a single cache would decode mip 0 from the base tail then reuse
  // that image for mips 1+, sampling the wrong sentinel.
  using SR = sr::gfx::cube::SubResRegion;
  std::vector<DecodedImage> mipTail(CB::kCubeFaces), baseTail(CB::kCubeFaces);
  bool mipTailReady[6] = {};
  bool baseTailReady[6] = {};

  for (uint32_t face = 0; face < CB::kCubeFaces; face++) {
    for (uint32_t mip = 0; mip < plan.mipLevels; mip++) {
      const auto pl = sr::gfx::cube::placeSubresource(lay, width, height,
                                                     blockW, blockW, face, mip);
      if (!pl.ok) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_CUBE] key=%08X face=%u mip=%u has NO PLACEMENT - refusing the "
            "whole cube", k, face, mip);
        return refuse(CB::CubeRefuse::MipDataMissing);
      }
      const bool fromBase =
          pl.region == SR::Base || pl.region == SR::BasePackedTail;
      const bool isTail =
          pl.region == SR::PackedTail || pl.region == SR::BasePackedTail;
      const uint8_t* regionBase = fromBase ? base : mipSrc;
      const uint64_t regionBytes = fromBase ? baseBytes : mipBytes;
      if (!regionBase || pl.byteOffset + pl.availBytes > regionBytes)
        return refuse(CB::CubeRefuse::MipDataMissing);

      Sub sub;
      sub.index = CB::subresourceIndex(face, mip, plan.mipLevels);
      sub.w = pl.width; sub.h = pl.height;

      if (isTail) {
        DecodedImage& cache =
            (pl.region == SR::BasePackedTail) ? baseTail[face] : mipTail[face];
        bool& ready =
            (pl.region == SR::BasePackedTail) ? baseTailReady[face]
                                              : mipTailReady[face];
        if (!ready) {
          if (!decodeGuestImage(regionBase + pl.byteOffset, pl.availBytes,
                                blockW, blockBytes, pl.tailWidth, pl.tailHeight,
                                pl.tailWidth, tiled, endian, cache))
            return refuse(CB::CubeRefuse::ByteCountMismatch);
          if (cache.truncated) {
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[S6_CUBE] key=%08X face=%u packed TAIL short - refusing",
                k, face);
            return refuse(CB::CubeRefuse::ByteCountMismatch);
          }
          ready = true;
        }
        const uint32_t subRowBlocks = (pl.width + blockW - 1u) / blockW;
        const uint32_t subRows = (pl.height + blockW - 1u) / blockW;
        sub.rowBytes = subRowBlocks * blockBytes;
        sub.rows = subRows;
        sub.lin.assign(size_t(subRows) * sub.rowBytes, 0);
        for (uint32_t y = 0; y < subRows; y++) {
          const uint64_t srcRow = uint64_t(pl.yBlocks + y) * cache.rowBytes
                                  + uint64_t(pl.xBlocks) * blockBytes;
          if (srcRow + sub.rowBytes > cache.lin.size())
            return refuse(CB::CubeRefuse::ByteCountMismatch);
          std::memcpy(sub.lin.data() + size_t(y) * sub.rowBytes,
                      cache.lin.data() + srcRow, sub.rowBytes);
        }
      } else {
        DecodedImage img;
        if (!decodeGuestImage(regionBase + pl.byteOffset, pl.availBytes,
                              blockW, blockBytes, pl.width, pl.height,
                              mip == 0 ? pitchTexels : pl.width, tiled, endian,
                              img))
          return refuse(CB::CubeRefuse::ByteCountMismatch);
        if (img.truncated) {
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[S6_CUBE] key=%08X face=%u mip=%u SHORT (needed %llu) - the "
              "whole cube is REFUSED rather than uploaded with a black face",
              k, face, mip, (unsigned long long)img.needed);
          return refuse(CB::CubeRefuse::ByteCountMismatch);
        }
        sub.rows = img.rows; sub.rowBytes = img.rowBytes;
        sub.lin = std::move(img.lin);
      }
      subs.push_back(std::move(sub));
    }
  }
  if (subs.size() != up.subresources)
    return refuse(CB::CubeRefuse::FaceDataMissing);

  // ── THE TRANSACTION ─────────────────────────────────────────────────────
  // ⛔ NOTHING HAS TOUCHED THE DEVICE YET. From here, cubetx::CubeTransactor
  // owns the candidate slot, the candidate resource, the staging buffer, the
  // upload context and the publication - and kernel/tex_unlock_test.cpp
  // drives that identical body on a WARP device (H1b-15). The two fault seams
  // are diagnostic only, default OFF, and fire BEFORE the operation they
  // model, so the recovery code that runs afterwards is the real one.
  static const TX::Fault s_fault =
      TX::faultFromEnv(std::getenv("SR_HLE_CUBE_FAIL_SUBMIT"));
  static const int s_failAt = [] {
    const char* e = std::getenv("SR_HLE_CUBE_FAIL_AT");
    return e && e[0] ? atoi(e) : -1;
  }();
  TX::UploadRequest rq;
  rq.key = k;
  rq.dxgi = dxgi;
  rq.width = plan.width;
  rq.height = plan.height;
  rq.depthOrArraySize = plan.depthOrArraySize;   // SIX SLICES, TEXTURE2D
  rq.mipLevels = plan.mipLevels;
  rq.subs = subs.data();
  rq.count = (uint32_t)subs.size();
  // r7 (r6 verdict): THE OWNERSHIP DECISION COMES FIRST AND DOMINATES EVERY
  // ACCESS TO THE GRAPHICS-FRAME STATE. With SR_HLE_OWN_PRESENT=0 the main
  // thread reads and writes m_frameIndex / m_fenceValues in moveToNextFrame
  // without m_retiredMu, so even READING them from the render thread is the
  // data race the r5 and r6 verdicts named - r6 read them and only then
  // refused. Now nothing in this function touches either field unless this
  // thread owns Present. The false arm still hands the request to the
  // transaction: PresentNotOwned owns the refusal, the counters and the
  // replay-final report.
  const bool renderThreadOwnsPresent = sr::gfx::hle::ownPresentEnabled();
  rq.renderThreadOwnsPresent = renderThreadOwnsPresent;
  rq.graphicsFenceNow = 0;   // inert: the false arm never stamps it
  if (renderThreadOwnsPresent) {
    // The value the frame queue signals when the CURRENT frame's lists
    // complete - the same stamp retireResource uses. A displaced (resource,
    // slot) pair is released only once this value has passed. Only the
    // owning thread may read it, and only here.
    std::lock_guard<std::mutex> lk(m_retiredMu);
    rq.graphicsFenceNow = m_fenceValues[m_frameIndex];
  }
  rq.fault = s_fault;
  rq.failAtSubresource = s_failAt;
  rq.waitMs = 10000;
  if (s_fault != TX::Fault::None || s_failAt >= 0) {
    static std::atomic<uint32_t> s_injLog{0};
    if (s_injLog.fetch_add(1, std::memory_order_relaxed) < 12)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_CUBE] INJECTED fault=%s failAt=%d for key=%08X - the transaction "
          "must publish NOTHING", TX::faultName(s_fault), s_failAt, k);
  }
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE slot0{
      cpu.ptr + SIZE_T(kTex2DSlots + 8u) * inc};   // cube band, index 0 = dummy
  CB::CubeRefuse txWhy = CB::CubeRefuse::None;
  const uint32_t idx = m_cubeTx.upload(m_device.Get(), m_commandQueue.Get(),
                                       slot0, inc, rq, txWhy);
  if (!idx) {
    // The transaction recorded this refusal itself; counting it here too
    // would break conservation. Only the name crosses the boundary.
    why = txWhy;
    static std::atomic<uint32_t> s_failLog{0};
    if (s_failLog.fetch_add(1, std::memory_order_relaxed) < 12)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_CUBE] key=%08X %ux%u mips=%u REFUSED in the transaction: %s - "
          "cube NOT published, the published pair (if any) is unchanged",
          k, width, height, plan.mipLevels, CB::refuseName(txWhy));
    return 0;
  }

  static std::atomic<uint64_t> s_n{0};
  const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 12 || n % 200 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[S6_CUBE] #%llu uploaded key=%08X %ux%u fmt=%u mips=%u faces=6 "
        "subres=%u -> cube idx=%u",
        (unsigned long long)n, k, width, height, fmt, plan.mipLevels,
        up.subresources, idx);
  return idx;
}

// ── iter-444 S6g: the GENERAL texture upload (contract in the header) ───────
// The movie-plane sibling below is hardcoded R8_UNORM/linear/level-0 because
// that is all a YUV plane ever is. General assets are 100% TILED (census:
// tiled=1001 linear=0) and overwhelmingly block-compressed (DXT1 > DXN >
// DXT4_5), so this path must untile on the BLOCK grid and byte-swap, using the
// same algorithms as getOrCreateTexture2D — with one deliberate difference:
// EVERY source read is bounds-checked against the snapshot size. The LLE path
// has no such check and its tiled read span provably exceeds what it validates.
uint32_t D3D12Backend::hleUploadHostTextureFmt(uint32_t key, const uint8_t* src,
                                              uint32_t srcBytes, uint32_t fmt,
                                              uint32_t width, uint32_t height,
                                              uint32_t pitchTexels, bool tiled,
                                              uint32_t endian) {
  if (!m_initialized || !src || !srcBytes || !width || !height) return 0;
  if (!m_xlatSrvHeap) return 0;
  // iter-450c (SR_LUM_PROBE diagnostic): ALL-ZERO SNAPSHOT CENSUS. The world
  // pixel shaders output bit-zero while depth writes fine — a black SAMPLED
  // texture (capture-at-bind of guest memory the CPU fills LATER) would zero
  // every draw that multiplies by it. Name every all-zero upload.
  {
    // iter-450e: ALWAYS-ON — the zero-set feeds hleTexWasZeroUpload (the
    // SR_TEXSLOT_WHITE=zero diagnostic). One pass per one-time upload.
    bool anyNz = false;
    for (uint32_t i = 0; i < srcBytes; i++)
      if (src[i]) { anyNz = true; break; }
    static std::atomic<uint32_t> s_zn{0}, s_tn{0};
    const uint32_t tn = s_tn.fetch_add(1, std::memory_order_relaxed) + 1;
    m_uploadKeys.insert(key & 0x1FFFFFFFu);
    if (!anyNz) {
      m_zeroUploadKeys.insert(key & 0x1FFFFFFFu);
      const uint32_t zn = s_zn.fetch_add(1, std::memory_order_relaxed) + 1;
      if (zn <= 60 || (zn & (zn - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TEXUP_ZERO] #%u (of %u uploads) key=%08X fmt=%u %ux%u "
            "bytes=%u ALL-ZERO snapshot",
            zn, tn, key, fmt, width, height, srcBytes);
    } else {
      m_zeroUploadKeys.erase(key & 0x1FFFFFFFu);
      if (tn <= 12 || tn % 500 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[TEXUP_NZ] #%u key=%08X fmt=%u %ux%u nonzero", tn, key, fmt,
            width, height);
    }
    // iter-450f [KEYUP]: raw content stats for the queried key at upload
    // time. For BC2/BC3 the first 2 bytes of each 16-byte block are the
    // ALPHA endpoints — the channel no RGB dump ever verified. Also logs the
    // color-endpoint words (bytes 8..11) and the first 32 raw bytes.
    static const uint32_t s_keyQ = [] {
      const char* e = std::getenv("SR_AUTOPSY_KEY");
      return e && e[0] ? (uint32_t)strtoul(e, nullptr, 16) & 0x1FFFFFFFu : 0u;
    }();
    if (s_keyQ && (key & 0x1FFFFFFFu) == s_keyQ) {
      uint32_t a0min = 255, a0max = 0, a1min = 255, a1max = 0;
      uint64_t a0sum = 0, a1sum = 0, c0nz = 0, blocks = 0;
      for (uint32_t o = 0; o + 16 <= srcBytes; o += 16) {
        const uint8_t a0 = src[o], a1 = src[o + 1];
        a0min = a0 < a0min ? a0 : a0min; a0max = a0 > a0max ? a0 : a0max;
        a1min = a1 < a1min ? a1 : a1min; a1max = a1 > a1max ? a1 : a1max;
        a0sum += a0; a1sum += a1;
        if (src[o + 8] | src[o + 9] | src[o + 10] | src[o + 11]) c0nz++;
        blocks++;
      }
      char hexs[3 * 32 + 4] = {};
      for (uint32_t i = 0; i < 32 && i < srcBytes; i++)
        snprintf(hexs + i * 3, 4, "%02X ", src[i]);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[KEYUP] key=%08X fmt=%u %ux%u bytes=%u blocks=%llu | "
          "a0[min=%u max=%u mean=%llu] a1[min=%u max=%u mean=%llu] "
          "colorNZblocks=%llu | first32: %s",
          key, fmt, width, height, srcBytes, (unsigned long long)blocks,
          a0min, a0max, (unsigned long long)(blocks ? a0sum / blocks : 0),
          a1min, a1max, (unsigned long long)(blocks ? a1sum / blocks : 0),
          (unsigned long long)c0nz, hexs);
    }
  }

  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  uint32_t blockW = 1, blockBytes = 4;
  if (!guestFormatToDxgi(fmt, dxgi, blockW, blockBytes)) {
    // NEVER guess a format. An unknown one is COUNTED and refused, because a
    // wrong decode is indistinguishable from a bind or upload bug downstream.
    static std::atomic<uint32_t> s_unk{0};
    const uint32_t n = s_unk.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_TEXUP] unsupported fmt=%u %ux%u key=%08X -> WHITE", fmt,
          width, height, key);
    return 0;
  }

  DecodedImage img;
  if (!decodeGuestImage(src, srcBytes, blockW, blockBytes, width, height,
                        pitchTexels, tiled, endian, img))
    return 0;
  const uint32_t rows = img.rows;
  const uint32_t rowBytes = img.rowBytes;
  std::vector<uint8_t>& lin = img.lin;
  if (img.truncated) {
    // NO SILENT TRUNCATION: a short decode paints a partly-black texture that
    // reads as an art bug. Say it, and say how short the snapshot was.
    static std::atomic<uint32_t> s_tr{0};
    const uint32_t n = s_tr.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || n % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_TEXUP] #%u snapshot SHORT: fmt=%u %ux%u pitch=%u tiled=%d "
          "srcBytes=%u needed>=%llu - texture partially decoded",
          n, fmt, width, height, pitchTexels, tiled ? 1 : 0, srcBytes,
          (unsigned long long)img.needed);
  }

  HleTex& t = m_hleTextures[key];
  const bool dimsChanged = t.res && (t.w != width || t.h != height);
  if (!t.res || dimsChanged) {
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width; td.Height = height;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = dxgi; td.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    if (FAILED(m_device->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&res))))
      return 0;
    // ⛔ AUDIT FOLLOW-UP: retire rather than release in place — see the sibling
    // upload path. An in-flight frame can still be sampling this resource.
    if (t.res) {
      // step 6 I3-B: copy-on-write - a resized texture gets a NEW slot; the old slot retires with the old resource
      const uint32_t cow = texSlotCow(t.slot, t.res, "hle-upload-resize");
      if (!cow) return 0;  // no slot: the entry keeps its old texture (retried at its next upload)
      t.slot = cow;
    }
    t.res = res; t.w = width; t.h = height; t.uploaded = false;
    // iter-450UE FIX B: the resource is being REPLACED, so every swizzled
    // view over the old one is now a stale descriptor. Invalidate here,
    // in the same statement group that swaps `res` — a view outliving its
    // resource is the one real hazard this feature adds.
    t.swizViews.clear();
    // ⛔ AUDIT FIX: record fmt/state here too. `fmt` had exactly ONE write
    // site (the resolve), so every upload-created entry carried
    // DXGI_FORMAT_UNKNOWN and the resolve's `t.fmt != src->dxgi` re-create
    // guard was UNCONDITIONALLY TRUE for them — i.e. the destructive
    // release-and-rewrite branch below would fire on every single resolve
    // into a key the upload path had touched.
    t.fmt = dxgi; t.fromResolve = false;
    t.state = D3D12_RESOURCE_STATE_COPY_DEST;
    // The SRV heap is finite (kTex2DSlots). The census measured 883 distinct
    // textures against ~4 live per frame, so exhaustion is real over a long
    // run — COUNT it rather than silently handing back the white fallback.
    if (!t.slot) {
      if (!texSlotAvailable()) {
        static std::atomic<uint32_t> s_ex{0};
        const uint32_t n = s_ex.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4 || n % 200 == 0)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[S6_TEXUP] SRV slot exhaustion (%u used) - key=%08X gets WHITE; "
              "LRU eviction is the next slice", kTex2DSlots, key);
        m_hleTextures.erase(key);
        return 0;
      }
      t.slot = texSlotAlloc();
    }
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = dxgi;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(t.slot) * inc};
    publishXlatSrv(t.res.Get(), &sv, h);
  }
  if (!uploadTexture2D(t.res.Get(), lin.data(), rows, rowBytes, width, height,
                       dxgi, /*fromShaderResource=*/t.uploaded))
    return 0;
  t.uploaded = true;
  static std::atomic<uint64_t> s_n{0};
  const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 12 || n % 200 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[S6_TEXUP] #%llu key=%08X fmt=%u %ux%u pitch=%u tiled=%d endian=%u "
        "-> slot=%u (%zu B decoded, pool=%zu)",
        (unsigned long long)n, key, fmt, width, height, pitchTexels,
        tiled ? 1 : 0, endian, t.slot, lin.size(), m_hleTextures.size());
  return t.slot;
}
#endif

// ── iter-450UE FIX B: THE SWIZZLED VIEW (SR_HLE_TEX_SWIZZLE) ────────────────
//
// OUR host-format swizzle table, keyed on the DXGI format we ACTUALLY created.
// Values are lifted from the reference's host-format table (xenia-canary
// d3d12_texture_cache.h:196, the `swizzle` column); the KEY differs on purpose
// (see the note in xenos_state.hpp): our tree has two guest->DXGI switches that
// disagree, so keying on the guest format would describe a resource we did not
// bind. The question this answers is "which host component carries guest
// channel g", which is a property of the host format alone.
static uint32_t hostFormatSwizzleForDxgi(DXGI_FORMAT f) {
  switch (f) {
    // Single-channel: every guest channel resolves to host R (reference RRRR).
    // This is the row that makes an alpha-only k_8 font atlas work.
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
      return sr::gfx::texswz::kSwizzleRRRR;
    // Two-channel: guest B and A resolve to host G (reference RGGG).
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_BC5_UNORM:
      return sr::gfx::texswz::kSwizzleRGGG;
    // Four-channel and the BC1/2/3 family: identity (reference RGBA).
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
      return sr::gfx::texswz::kSwizzleRGBA;
    // ⛔ An unlisted format is NOT assumed identity — the caller refuses.
    default:
      return 0xFFFFFFFFu;
  }
}

// P1 (SR_PM4_RTCACHE): the PM4 path's SRV component mapping - the fetch
// constant's swizzle (dword_3 bits 1-12) composed over the host format column,
// as xenia-canary D3D12TextureCache builds every SRV. The PM4 path decodes
// only guest-format bytes (resolves land in guest memory), so there is no
// resolve-product exception here. An unlisted host format keeps the default
// mapping and is counted.
static std::atomic<uint32_t> s_pm4SwzUnknown{0};
static UINT pm4TexMapping(DXGI_FORMAT f, uint32_t dword3) {
  if (!pm4RtCacheEnabled()) return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  const uint32_t hostSwz = hostFormatSwizzleForDxgi(f);
  if (hostSwz == 0xFFFFFFFFu) {
    s_pm4SwzUnknown.fetch_add(1, std::memory_order_relaxed);
    return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  }
  return sr::gfx::texswz::toD3D12ComponentMapping(sr::gfx::texswz::guestToHostSwizzle(
      sr::gfx::texswz::guestSwizzleFromFetchDword3(dword3), hostSwz));
}

#if SR_ORACLE
// Returns the SRV heap slot for `key` viewed through the mapping composed from
// the guest fetch swizzle, or 0 to mean "use what you already had".
//
// ⛔ NEVER rewrites a live descriptor: a new mapping always takes a FRESH slot
// from the bump allocator. Writing a never-bound descriptor is safe with frames
// in flight; rewriting a bound one is not.
// ⛔ GUEST TEXTURES ONLY. A resolve product's host format is chosen by US, so
// the guest format's host-swizzle row does not describe it and composing one
// would be a guess. Dummies short-circuit at the call site; volumes and cubes
// are out of scope for this slice and are named as such.
uint32_t D3D12Backend::hleEnsureTexView(uint32_t key, uint16_t guestSwiz,
                                        int mode) {
  auto it = m_hleTextures.find(key & 0x1FFFFFFFu);
  if (it == m_hleTextures.end() || !it->second.res) return 0;
  HleTex& t = it->second;
  if (t.fromResolve) { m_swzSkipResolve.fetch_add(1, std::memory_order_relaxed); return 0; }
  const uint32_t hostSwz = hostFormatSwizzleForDxgi(t.fmt);
  if (hostSwz == 0xFFFFFFFFu) {
    m_swzUnknownFmt.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint32_t composed =
      sr::gfx::texswz::guestToHostSwizzle(guestSwiz, hostSwz);
  const uint32_t mapping = sr::gfx::texswz::toD3D12ComponentMapping(composed);
  m_swzBindings.fetch_add(1, std::memory_order_relaxed);
  if (mapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) {
    m_swzIdentity.fetch_add(1, std::memory_order_relaxed);
    return 0;   // nothing to do — the existing view already says this
  }
  // Mode 1 = CENSUS ONLY: count and log, change no pixel.
  // Mode 2 = single-channel host formats only (the proven k_8 000R class).
  if (mode < 3 && hostSwz != sr::gfx::texswz::kSwizzleRRRR) {
    m_swzSkipMode.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  if (mode < 2) { m_swzCensusOnly.fetch_add(1, std::memory_order_relaxed); return 0; }
  for (const auto& v : t.swizViews)
    if (v.swiz == guestSwiz) return v.slot;
  if (!texSlotAvailable()) {
    // Degrade to the IDENTITY view, never to white: the worst case is a
    // texture rendering as it does today, not a texture disappearing.
    m_swzExhausted.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint32_t slot = texSlotAlloc();
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = t.fmt;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = mapping;
  sv.Texture2D.MipLevels = 1;
  auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(slot) * inc};
  publishXlatSrv(t.res.Get(), &sv, h);
  t.swizViews.push_back({guestSwiz, slot});
  const uint64_t n = m_swzViews.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_SWZ] view #%llu key=%08X dxgi=%u guestSwz=0x%03X hostSwz=0x%03X "
        "composed=0x%03X mapping=0x%04X -> slot=%u",
        (unsigned long long)n, key & 0x1FFFFFFFu, unsigned(t.fmt),
        unsigned(guestSwiz), hostSwz, composed, mapping, slot);
  return slot;
}

// ── iter-449x: the VOLUME upload (Texture3D — the LUT class) ────────────────
// Census (iter-449w, 2 rolls): every live guest volume is 64x64x64 k_8_8_8_8
// TILED. Two dominant scene PS tfetch3D; before this they sampled the white 3D
// dummy — a prime suspect for the FE era's crushed output (color-grade LUT).
// Tiling math is Xenia's Tiled3D (texture_address.h) ported beside the 2D port.
uint32_t D3D12Backend::hleUploadHostVolume(uint32_t key, const uint8_t* src,
                                           uint32_t srcBytes, uint32_t fmt,
                                           uint32_t width, uint32_t height,
                                           uint32_t depth, uint32_t pitchTexels,
                                           bool tiled, uint32_t endian) {
  if (!m_initialized || !src || !srcBytes || !width || !height || !depth)
    return 0;
  if (!m_xlatSrvHeap) return 0;
  if (fmt != 6u && fmt != 50u) {
    // NEVER guess a format (same rule as the 2D path): count and refuse.
    static std::atomic<uint32_t> s_unk{0};
    if (s_unk.fetch_add(1, std::memory_order_relaxed) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_VOLUP] unsupported fmt=%u %ux%ux%u key=%08X -> WHITE", fmt,
          width, height, depth, key);
    return 0;
  }

  uint32_t pitch = pitchTexels ? pitchTexels : width;
  if (tiled) pitch = (pitch + 31u) & ~31u;
  const uint32_t hA = (height + 31u) & ~31u;

  std::vector<uint8_t> lin(size_t(width) * height * depth * 4u, 0);
  bool truncated = false;
  if (tiled) {
    for (uint32_t z = 0; z < depth; z++) {
      for (uint32_t y = 0; y < height; y++) {
        uint8_t* drow = lin.data() + (size_t(z) * height + y) * width * 4u;
        for (uint32_t x = 0; x < width && x < pitch; x++) {
          const int64_t off = xenos_tiling::tiled3D(
              int32_t(x), int32_t(y), int32_t(z), pitch, hA, 2);
          if (off < 0 || uint64_t(off) + 4u > srcBytes) { truncated = true; continue; }
          std::memcpy(drow + size_t(x) * 4u, src + off, 4u);
        }
      }
    }
  } else {
    const uint32_t srcPitch = pitch * 4u;
    for (uint32_t z = 0; z < depth; z++) {
      for (uint32_t y = 0; y < height; y++) {
        const uint64_t off = (uint64_t(z) * height + y) * srcPitch;
        if (off + size_t(width) * 4u > srcBytes) { truncated = true; break; }
        std::memcpy(lin.data() + (size_t(z) * height + y) * width * 4u,
                    src + off, size_t(width) * 4u);
      }
    }
  }
  if (truncated) {
    static std::atomic<uint32_t> s_tr{0};
    if (s_tr.fetch_add(1, std::memory_order_relaxed) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_VOLUP] snapshot SHORT: %ux%ux%u pitch=%u tiled=%d srcBytes=%u "
          "- volume partially decoded", width, height, depth, pitch,
          tiled ? 1 : 0, srcBytes);
  }
  if (endian) {
    uint32_t* p = reinterpret_cast<uint32_t*>(lin.data());
    for (size_t i = 0; i < lin.size() / 4; i++) p[i] = xeSwap32(p[i], endian);
  }

  HleVol& v = m_hleVolumes[key];
  const bool dimsChanged = v.res && (v.w != width || v.h != height || v.d != depth);
  if (!v.res || dimsChanged) {
    if (!v.idx) {
      if (m_volNextIdx > 7u) {
        // The space1 table has 8 slots (0 = white). Census says 8 distinct
        // volume KEYS but they alias few contents; COUNT exhaustion, no LRU yet.
        static std::atomic<uint32_t> s_ex{0};
        if (s_ex.fetch_add(1, std::memory_order_relaxed) < 8)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[S6_VOLUP] volume table full (7 live) - key=%08X gets WHITE",
              key);
        m_hleVolumes.erase(key);
        return 0;
      }
      v.idx = m_volNextIdx++;
    }
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    td.Width = width; td.Height = height;
    td.DepthOrArraySize = (UINT16)depth; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    if (FAILED(m_device->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&res))))
      return 0;
    if (v.res) retireResource(v.res);   // an in-flight frame may sample it
    v.res = res; v.w = width; v.h = height; v.d = depth; v.uploaded = false;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture3D.MipLevels = 1;
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(kTex2DSlots + v.idx) * inc};
    publishXlatSrv(v.res.Get(), &sv, h);
  }
  if (!uploadTexture2D(v.res.Get(), lin.data(), height * depth, width * 4u,
                       width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                       /*fromShaderResource=*/v.uploaded, depth))
    return 0;
  v.uploaded = true;
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 12 || n % 100 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_VOLUP] #%u key=%08X %ux%ux%u tiled=%d endian=%u -> 3D idx=%u "
        "(volumes live=%zu)",
        n, key, width, height, depth, tiled ? 1 : 0, endian, v.idx,
        m_hleVolumes.size());
  return v.idx;
}

// ── iter-442 S4.6 / iter-444 S6e+S6f: stage the guest's draws, record at Present ─
//
// THE PAIRING RULE (iter-435, already proven in the xlat drain at the
// hleVdecl arm): the guest's vdecl author order equals the translated VS's
// input-register order, so element i of the declaration feeds input i. The VS
// is authoritative for HOW MANY inputs exist — the movie quad's declaration
// has THREE elements {POSITION FLOAT3, TEXCOORD FLOAT2, COLOR FLOAT4} while
// its VS signature has TWO (measured: [S4_PSO] sigN=2, stride=32), so the
// COLOR element is simply not fetched. A declaration SHORTER than the VS's
// input count is unknowable rather than merely lossy, so it is refused.
//
// The format comes from the DECLARATION (low 6 bits of the packed 360
// D3DDECLTYPE are the Xenos VertexFormat — verified against three live
// elements: 0x39=k_32_32_32_FLOAT, 0x25=k_32_32_FLOAT, 0x26=k_32_32_32_32_FLOAT),
// while signedness/normalisation come from the translated VS's own sidecar,
// because those describe how the SHADER interprets the fetch, not where the
// bytes are. For the all-float formats the movie quad uses, neither applies.
const char* D3D12Backend::hleRejectName(HleReject r) {
  switch (r) {
    case HleReject::None:         return "none";
    case HleReject::Shape:        return "shape";
    case HleReject::NoVs:         return "noVs";
    case HleReject::NoDecl:       return "noDecl";
    case HleReject::BadFormat:    return "badFormat";
    case HleReject::NoPso:        return "noPso";
    case HleReject::VbFull:       return "vbFull";
    case HleReject::CbFull:       return "cbFull";
    case HleReject::TooManyDraws: return "tooManyDraws";
    case HleReject::NoConst:      return "noConst";
    case HleReject::MultiStream:  return "multiStream";
  }
  return "?";
}

// The per-frame upload rings. Created lazily: a roll that never reaches a draw
// should not commit 160 MB, and the device does not exist when this TU loads.
bool D3D12Backend::hleEnsureRings() {
  sr::kernel::MemSiteScope memSite("hleEnsureRings (HLE VB / CB)");   // step 4 7d: [MEM_SITE] names (SR_MEM_SITES, diagnostic)
  if (m_hleRingsReady) return true;
  if (!m_device) return false;
  D3D12_HEAP_PROPERTIES up{};
  up.Type = D3D12_HEAP_TYPE_UPLOAD;
  auto mk = [&](uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource>& res,
                uint8_t*& ptr) -> bool {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res))))
      return false;
    D3D12_RANGE nr{0, 0};
    return SUCCEEDED(res->Map(0, &nr, reinterpret_cast<void**>(&ptr)));
  };
  if (!mk(kHleVBBytes, m_hleVB, m_hleVBPtr) ||
      !mk(kHleCBBytes, m_hleCB, m_hleCBPtr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[S6_RING] HLE upload ring create/map FAILED — no draw can be staged");
    m_hleVB.Reset(); m_hleCB.Reset();
    return false;
  }
  m_hleVsConst.assign(4096, 0);
  m_hlePsConst.assign(4096, 0);  // iter-449v: 256 float4
  m_hleBatches.reserve(1024);
  m_hleHeldList.reserve(1024);
  m_hleHalf = 0;
  m_hleVBUsed = 0;
  m_hleCBUsed = 0;
  m_hleRingsReady = true;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[S6_RING] HLE upload rings ready: VB %lluMB CB %lluMB, 2 halves each, "
      "max %u draws/frame",
      (unsigned long long)(kHleVBBytes >> 20),
      (unsigned long long)(kHleCBBytes >> 20), kHleMaxDraws);
  return true;
}

// -- iter-457 gate 3 [DRAWMAN]: the completion tally ------------------------
// "At completion print and check requested, matched, suppressed, and total
// submitted; any inequality invalidates the replay and must return nonzero."
// File-scope so the replay driver reads them without threading a backend
// pointer through app/main.cpp.
static std::atomic<bool>     g_drawManRejected{false};
static std::atomic<uint64_t> g_drawManRequested{0};
static std::atomic<uint64_t> g_drawManMatched{0};
static std::atomic<uint64_t> g_drawManSuppressed{0};
static std::atomic<uint64_t> g_drawManTotalSubmitted{0};
// The manifest's PINNED admitted-draw count, compared against the observed
// total at completion so AdmittedMismatch is reachable.
static std::atomic<uint64_t> g_drawManRequestedAdmitted{0};

// 0 when no manifest was supplied or the treatment was EXACT; 2 when the
// manifest was refused; 3 when requested != matched != suppressed. The replay
// uses it as its exit code, so a treatment that diverged from what was asked
// for can never be reported as a clean run.
int hleDrawManifestStatus() {
  const char* e = std::getenv("SR_DRAW_MANIFEST");
  if (!e || !e[0]) return 0;                    // no treatment requested
  const manifest::Tally t{
      g_drawManRequested.load(std::memory_order_relaxed),
      g_drawManMatched.load(std::memory_order_relaxed),
      g_drawManSuppressed.load(std::memory_order_relaxed),
      g_drawManTotalSubmitted.load(std::memory_order_relaxed)};
  if (g_drawManRejected.load(std::memory_order_relaxed)) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[DRAWMAN] INVALID: manifest refused; suppressed=%llu (must be 0) "
        "totalSubmitted=%llu",
        (unsigned long long)t.suppressed,
        (unsigned long long)t.totalSubmitted);
    return 2;
  }
  // THE PINNED ADMITTED COUNT, enforced. Without this the manifest could
  // pin any value and the run would still report EXACT - the director
  // proved AdmittedMismatch was unreachable by pinning 100000000 and
  // getting exit 0. It is checked HERE because the observed count is only
  // knowable once the pass has finished; preventing the suppression itself
  // requires a probe in a SEPARATE process, which the driver performs.
  const uint64_t pinned = g_drawManRequestedAdmitted.load(std::memory_order_relaxed);
  if (pinned && pinned != t.totalSubmitted) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[DRAWMAN] INVALID: manifest pins admitted-draws=%llu but this pass "
        "submitted %llu - the ordinals were derived against a different "
        "fixture or build and this replay is NOT valid",
        (unsigned long long)pinned, (unsigned long long)t.totalSubmitted);
    return 4;
  }
  const bool ok = manifest::tallyConsistent(t);
  SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Error,
      "[DRAWMAN] TALLY requested=%llu matched=%llu suppressed=%llu "
      "totalSubmitted=%llu -> %s",
      (unsigned long long)t.requested, (unsigned long long)t.matched,
      (unsigned long long)t.suppressed, (unsigned long long)t.totalSubmitted,
      ok ? "EXACT" : "INVALID");
  return ok ? 0 : 3;
}

bool D3D12Backend::hleSubmitDraw(const HleDraw& d) {
  m_hleLastReject = HleReject::None;
  // ── iter-450UA [DRAWID]: DRAW IDENTITY + ISOLATION (diagnostic, default OFF)
  // Purpose (director ruling C-450Ua): associate a specific draw with a
  // specific thing on screen by SUPPRESSING it, rather than inferring identity
  // from glyph count or apparent shape. Two halves:
  //   SR_DRAWID=1            — record identity for every draw (see the record
  //                            site near the batch push).
  //   SR_DRAWID_SUPPRESS=<n> — drop exactly draw ordinal n.
  // The ordinal is assigned HERE, at entry, before any early return, so it is
  // stable no matter what refuses downstream, and it is deterministic under
  // replay (one executor thread walking a fixed command stream).
  // ⚠ Observation + omission only: no guest memory is written, no guest
  // decision is overridden, nothing is fabricated. Suppression REMOVES one of
  // our own draw submissions to test what it contributed; it is the diagnostic
  // inverse of forcing, and it is never on by default.
  const uint64_t drawOrd =
      m_drawIdOrdinal.fetch_add(1, std::memory_order_relaxed);
  {
    static const long long s_suppress = [] {
      const char* e = std::getenv("SR_DRAWID_SUPPRESS");
      return (e && e[0]) ? std::atoll(e) : -1LL;
    }();
    if (s_suppress >= 0 && (uint64_t)s_suppress == drawOrd) {
      static std::atomic<uint32_t> s_supLog{0};
      if (s_supLog.fetch_add(1, std::memory_order_relaxed) < 4)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[DRAWID] SUPPRESSED ordinal=%llu vs=%016llX ps=%016llX prim=%u "
            "count=%u srcType=%u",
            (unsigned long long)drawOrd, (unsigned long long)d.vsHash,
            (unsigned long long)d.psHash, d.primType, d.srcCount, d.srcType);
      // Counted as staged-but-not-drawn, matching the SR_S6_STRIPAB precedent
      // above: the coverage triple then shows staged > recorded by exactly the
      // number of suppressed draws, which is the honest reading.
      return true;
    }
  }
  // -- iter-458 H-VCAP OMISSION (SR_S6_EMULATE_VCAP=<bytes>, default OFF) ----
  // Astra 051's killer: replay a trace captured with SR_S6_VERTCAP raised
  // against ITSELF with exactly the draws the legacy producer cap would have
  // dropped (non-indexed DrawVertices whose stride*count exceeds <bytes>)
  // omitted here. Same trace, same roll -- attributable, unlike hwin4 vs hwin5.
  // OMISSION, NOT FORCING: removes our own submissions only.
  {
    static const long long s_emuCap = [] {
      const char* e = std::getenv("SR_S6_EMULATE_VCAP");
      return (e && e[0]) ? std::atoll(e) : -1LL;
    }();
    if (s_emuCap >= 0 && d.srcType == 0u &&
        (long long)(uint64_t(d.srcStride) * d.srcCount) > s_emuCap) {
      static std::atomic<uint64_t> s_emuN{0};
      const uint64_t n = s_emuN.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 4096)  // every event: the LAST line is the exact omitted count
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_VCAP_EMU] omitted DrawVertices #%llu (stride*count=%llu > %lld) "
            "vs=%016llX",
            (unsigned long long)n,
            (unsigned long long)(uint64_t(d.srcStride) * d.srcCount), s_emuCap,
            (unsigned long long)d.vsHash);
      return true;
    }
  }
  // -- iter-457 gate 3 [DRAWMAN]: THE STRICT SUPPRESSION MANIFEST ----------
  // SR_DRAW_MANIFEST=<path>. Supersedes the single-ordinal gate above for sweep
  // use: the director ruled that "a single-ordinal SR_DRAWID_SUPPRESS loop is
  // not by itself an exhaustive family/range sweep and cannot prove absence
  // when cancelling contributors are possible."
  //
  // FAIL-CLOSED BY CONSTRUCTION. Parsed once; ANY rejection leaves a Manifest
  // that selects nothing, so a malformed, empty, overlapping, out-of-range or
  // wrong-trace file suppresses ZERO draws rather than an arbitrary set. The
  // refusal is logged BY NAME and latched, so hleDrawManifestStatus() fails the
  // replay exit code instead of letting "no treatment applied" look like a
  // clean run.
  //
  // WARNING: OMISSION, NOT FORCING. It removes draws WE would have submitted,
  // to measure what they contributed. No guest memory is written and no guest
  // decision is overridden.
  {
    static manifest::Manifest s_man;
    static const manifest::Reject s_rej = [] {
      const char* e = std::getenv("SR_DRAW_MANIFEST");
      if (!e || !e[0]) return manifest::Reject::None;    // absent = no treatment
      manifest::Reject r = manifest::parseFile(e, &s_man);
      // THE WRONG-TRACE RULE. The contract requires refusal when the manifest
      // names a different fixture than the one being replayed: ordinals are
      // only meaningful against the trace they were derived from, so applying
      // them to another trace would suppress an ARBITRARY set while reporting
      // an exact tally. An unreadable trace yields an empty digest and is
      // treated as a FAILURE to verify, never as a match.
      if (r == manifest::Reject::None) {
        if (const char* tp = std::getenv("SR_HLE_TRACE_REPLAY")) {
          const std::string actual = manifest::sha256File(tp);
          if (actual.empty() || actual != s_man.traceSha) {
            SR_LOG_BOOT(sr::LogSeverity::Error,
                "[DRAWMAN] trace mismatch: manifest pins %s, %s hashes to %s",
                s_man.traceSha.c_str(), tp,
                actual.empty() ? "<unreadable>" : actual.c_str());
            r = manifest::Reject::TraceMismatch;
            s_man = manifest::Manifest{};   // select NOTHING
          }
        }
      }
      if (r != manifest::Reject::None) {
        SR_LOG_BOOT(sr::LogSeverity::Error,
            "[DRAWMAN] REFUSED manifest '%s': %s - suppressing ZERO draws",
            e, manifest::rejectName(r));
        g_drawManRejected.store(true, std::memory_order_relaxed);
      } else {
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[DRAWMAN] manifest '%s' accepted: trace=%s admitted=%llu "
            "requested=%llu selectAll=%d ranges=%llu",
            e, s_man.traceSha.c_str(),
            (unsigned long long)s_man.admittedDraws,
            (unsigned long long)s_man.requested, (int)s_man.selectAll,
            (unsigned long long)s_man.ranges.size());
        g_drawManRequested.store(s_man.requested, std::memory_order_relaxed);
        g_drawManRequestedAdmitted.store(s_man.admittedDraws,
                                         std::memory_order_relaxed);
      }
      return r;
    }();
    (void)s_rej;
    g_drawManTotalSubmitted.fetch_add(1, std::memory_order_relaxed);
    if ((s_man.selectAll || !s_man.ranges.empty()) && s_man.selects(drawOrd)) {
      g_drawManMatched.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<uint32_t> s_manLog{0};
      if (s_manLog.fetch_add(1, std::memory_order_relaxed) < 4)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[DRAWMAN] SUPPRESSED ordinal=%llu vs=%016llX ps=%016llX prim=%u "
            "count=%u", (unsigned long long)drawOrd,
            (unsigned long long)d.vsHash, (unsigned long long)d.psHash,
            d.primType, d.srcCount);
      g_drawManSuppressed.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  // ── iter-456p: THE TARGET DRAW-CONTRACT ROW (SR_DRAW_CONTRACT=<ordinal>) ──
  // The HLE half of the director's two-row differential. Same field set as the
  // PM4 half so the rows diff line-for-line. Observer-only, default OFF: it
  // reads the assembled draw and formats a line.
  {
    static const long long s_want = [] {
      const char* e = std::getenv("SR_DRAW_CONTRACT");
      return (e && e[0]) ? std::atoll(e) : -1LL;
    }();
    if (s_want >= 0 && (uint64_t)s_want == drawOrd) {
      sr::gfx::DrawContract dc;
      dc.ordinal = drawOrd;
      dc.source = sr::gfx::DrawContractSource::HLE;
      dc.haveShaders = true;
      dc.vsHash = d.vsHash;
      dc.psHash = d.psHash;
      dc.havePrim = true;
      dc.primType = d.primType;
      dc.srcSelect = d.srcType;      // 0=DrawVertices 1=Indexed 2=UP/inline
      // ⚠ NOT an index count and NOT an index type. This is an API
      // DrawVertices source count; the driver's lowering may turn it into an
      // indexed-instanced draw with entirely different numbers.
      dc.drawCount = d.srcCount;
      dc.countDomain = "srcCount(API DrawVertices)";
      dc.haveSrcSelect = true;
      dc.srcSelectDomain = "srcType(our producer tag)";
      dc.texDomain = sr::gfx::DrawContract::TexDomain::Bound;
      // ⚠ srcVerts are HOST bytes at this point. The director requires guest
      // bytes hashed in their ORIGINAL big-endian order BEFORE any host decode,
      // so this digest is NOT comparable to the PM4 side's and is deliberately
      // reported under its own name. vtxDigest stays '-' rather than carrying a
      // number that would invite a false field match.
      if (d.srcVerts && d.srcBytes)
        dc.vtxHostDigest = sr::gfx::drawContractDigest(d.srcVerts, d.srcBytes),
        dc.haveVtxHostDigest = true;
      dc.vtxBytes = d.srcBytes;
      dc.srcStride = d.srcStride;
      dc.haveSrcStride = true;
      dc.hostSynthLayout = d.hostVerts;
      dc.haveHostSynth = true;
      if (d.stateValid) {
        dc.haveFixed = true;
        dc.depthControl = d.depthControl;
        dc.blendControl0 = d.blendControl;
        dc.colorMask = d.colorMask;
        dc.haveVp = true;
        dc.vte = d.vteCntl;
        std::memcpy(&dc.vp[0], &d.vpScale[0], 4);
        std::memcpy(&dc.vp[1], &d.vpOffset[0], 4);
        std::memcpy(&dc.vp[2], &d.vpScale[1], 4);
        std::memcpy(&dc.vp[3], &d.vpOffset[1], 4);
        std::memcpy(&dc.vp[4], &d.vpScale[2], 4);
        std::memcpy(&dc.vp[5], &d.vpOffset[2], 4);
      }
      for (uint32_t t = 0; t < 16; t++) {
        if (!d.texKey[t]) continue;
        dc.tex[t].present = true;
        dc.tex[t].slot = t;
        dc.tex[t].key = d.texKey[t];
        dc.texCount++;
      }
      static char s_row[16384];
      sr::gfx::formatDrawContract(dc, s_row, sizeof(s_row));
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[DRAW_CONTRACT] BEGIN");
      SR_LOG_BOOT(sr::LogSeverity::Warn, "%s", s_row);
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[DRAW_CONTRACT] END");

      // ── iter-456p step 2: THE STREAM CONTRACT ──────────────────────────
      // The director's H9: is this draw pixel-inert because its consumed
      // POSITION1/index stream is not represented by our one-stream executor?
      // Reported, never inferred: the declaration's semantic-to-stream map, the
      // reference-derived indexVertexStream, and whether the executor actually
      // holds a binding for each consumed stream.
      //
      // THE REFERENCE RULE (UnleashedRecomp video.cpp:4914 and :4585-4589):
      //   a POSITION element with usageIndex == 1 sets
      //     vertexDeclaration->indexVertexStream = element->stream
      //   instancing  = (indexVertexStream != 0)
      //   indexCount  = vertexBufferViews[indexVertexStream].size / 4
      //   draw        = drawIndexedInstanced(indexCount,
      //                                      primitiveCount / indexCount, ...)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[STREAM_CONTRACT] BEGIN");
      {
        uint32_t idxVtxStream = 0;
        bool haveP1 = false;
        uint32_t maxStream = 0;
        const uint32_t nD = d.declCount > 16u ? 16u : d.declCount;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "declCount=%u declRaw=%d", d.declCount,
                    d.declRaw ? 1 : 0);
        for (uint32_t i = 0; i < nD && d.declRaw; i++) {
          const uint8_t* e = d.declRaw + i * 12u;
          auto be32l = [](const uint8_t* p) {
            return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
          };
          const uint32_t d0 = be32l(e), d1 = be32l(e + 4), d2 = be32l(e + 8);
          const uint32_t off = d0 & 0xFFFFu, stream = d0 >> 16;
          const uint32_t usage = d2 >> 16, usageIndex = (d2 >> 8) & 0xFFu;
          if (stream > maxStream) maxStream = stream;
          // D3DDECLUSAGE_POSITION == 0
          if (usage == 0u && usageIndex == 1u) {
            idxVtxStream = stream;
            haveP1 = true;
          }
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "decl[%u] stream=%u off=%u type=%08X usage=%u "
                      "usageIndex=%u%s",
                      i, stream, off, d1, usage, usageIndex,
                      (usage == 0u && usageIndex == 1u) ? "   <== POSITION1"
                                                        : "");
        }
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "indexVertexStream=%s%u instancing=%s maxDeclStream=%u",
                    haveP1 ? "" : "(none)", idxVtxStream,
                    (haveP1 && idxVtxStream != 0) ? "YES" : "no", maxStream);
        // What the executor actually holds. SetStreamSource is in the executor's
        // DROPPED list, so there is no per-stream state at all: the single
        // srcVerts/srcStride pair below is the whole vertex-input model.
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "executorStreams=1 (one-stride model) srcStride=%u "
                    "srcBytes=%u srcVerts=%d",
                    d.srcStride, d.srcBytes, d.srcVerts ? 1 : 0);
        // ⚠ DIRECTOR CORRECTION (2026-08-31). This field was named
        // `consumedNonZeroStream`, but it is printed from `maxDeclStream != 0`
        // and there is NO shader-input consumption test anywhere near it. The
        // name asserted a measurement the code never made. Renamed to what it
        // measures, with the reference's own criterion reported SEPARATELY —
        // that one does not depend on consumption at all: UnleashedRecomp
        // assigns `indexVertexStream` from a POSITION/usageIndex==1 element at
        // declaration-parse time, regardless of any shader-input census
        // (video.cpp:4912-4915).
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "declaredNonZeroStream=%s referenceIndexStreamPresent=%s "
                    "shaderConsumptionTested=NO executorBindingForIt=%s",
                    (maxStream != 0) ? "YES" : "no",
                    (haveP1 && idxVtxStream != 0) ? "YES" : "no",
                    (maxStream != 0) ? "ABSENT (SetStreamSource is dropped)"
                                     : "n/a");
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[STREAM_CONTRACT] END");
    }
  }

  // iter-450O STRIP A/B (replay diagnostic, default off): SR_S6_STRIPAB=skip
  // drops every strip-converted draw, =only drops everything else. Two
  // contact PNGs of the same fixture present then attribute the angular-
  // shard fracture to (or away from) the strip-conversion path. Named and
  // counted; never baked.
  {
    static const char* s_stripAB = std::getenv("SR_S6_STRIPAB");
    if (s_stripAB && s_stripAB[0] && s_stripAB[0] != '0') {
      const bool drop = (s_stripAB[0] == 's') ? d.fromStrip
                        : (s_stripAB[0] == 'o') ? !d.fromStrip
                                                : false;
      if (drop) {
        static std::atomic<uint64_t> s_dropped{0};
        const uint64_t k = s_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k == 1 || (k % 2000u) == 0u)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[S6_STRIPAB] mode=%s dropped=%llu (latest: vs=%016llX prim=%u "
              "count=%u fromStrip=%d)",
              s_stripAB, (unsigned long long)k, (unsigned long long)d.vsHash,
              d.primType, d.srcCount, (int)d.fromStrip);
        return true;   // counted as staged, deliberately not drawn
      }
    }
  }
  // iter-450k [PSDRAW]: full submit-path visibility for one queried PS —
  // logs entry; the paired "SUBMITTED" line comes from the DrawInstanced
  // site. Entry with no SUBMITTED = rejected; the follow-up entry's
  // lastReject names the reason.
  static const uint64_t s_psQ = [] {
    const char* e = std::getenv("SR_AUTOPSY_PS");
    return e && e[0] ? (uint64_t)strtoull(e, nullptr, 16) : 0ull;
  }();
  const bool psHit = s_psQ && d.psHash == s_psQ;
  if (psHit) {
    static std::atomic<uint32_t> s_pdN{0};
    const uint32_t n = s_pdN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 24) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[PSDRAW] #%u ENTRY ps=%016llX vs=%016llX prim=%u count=%u "
          "stride=%u declCount=%u vte=%08X stateValid=%d",
          n, (unsigned long long)d.psHash, (unsigned long long)d.vsHash,
          d.primType, d.srcCount, d.srcStride, d.declCount, d.vteCntl,
          (int)d.stateValid);
      // The quad's own vertices, first 5 floats each (stride 20 = xyz+uv):
      // where the guest THINKS this rect lands.
      const float* vf = reinterpret_cast<const float*>(d.srcVerts);
      if (vf && d.srcStride >= 20 && d.srcCount >= 4)
        for (uint32_t v = 0; v < 4; v++) {
          const float* p = reinterpret_cast<const float*>(
              reinterpret_cast<const uint8_t*>(vf) + size_t(v) * d.srcStride);
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[PSDRAW]    v%u = (%g, %g, %g) uv=(%g, %g)", v, p[0], p[1],
              p[2], p[3], p[4]);
        }
      // iter-450L: position-sanity census over the WHOLE payload — a mesh
      // whose VB was misread (foreign bytes) carries denormal/huge floats at
      // the position element; a healthy mesh doesn't. Positions assumed at
      // offset 0, float3 (t39 — true for every world decl seen).
      if (vf && d.srcStride >= 12) {
        uint32_t bad = 0, total = 0;
        for (uint32_t v = 0; v < d.srcCount; v++) {
          const float* p = reinterpret_cast<const float*>(
              reinterpret_cast<const uint8_t*>(vf) + size_t(v) * d.srcStride);
          for (int c2 = 0; c2 < 3; c2++) {
            total++;
            const float av = p[c2] < 0 ? -p[c2] : p[c2];
            if (av != 0.0f && (av < 1e-30f || av > 1e7f)) bad++;
            if (p[c2] != p[c2]) bad++;
          }
        }
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[PSDRAW]    posSanity: bad=%u/%u", bad, total);
      }
      // iter-450L: the DECL's element map (offset/stream/type) — the blanket
      // 8in32 producer swap is only correct for 32-bit lanes; any 8/16-bit
      // element here is corrupted by construction (u8x4 palette indices
      // byte-reversed = the shard suspect).
      if (d.declRaw && d.declCount) {
        char dm[200]; size_t o = 0;
        const uint8_t* dr = d.declRaw;
        for (uint32_t e = 0; e < d.declCount && e < 12 && o + 18 < sizeof(dm);
             e++) {
          const uint32_t dd0 = (uint32_t(dr[e * 12 + 0]) << 24) |
                               (uint32_t(dr[e * 12 + 1]) << 16) |
                               (uint32_t(dr[e * 12 + 2]) << 8) |
                               uint32_t(dr[e * 12 + 3]);
          const uint32_t dd1 = (uint32_t(dr[e * 12 + 4]) << 24) |
                               (uint32_t(dr[e * 12 + 5]) << 16) |
                               (uint32_t(dr[e * 12 + 6]) << 8) |
                               uint32_t(dr[e * 12 + 7]);
          o += (size_t)snprintf(dm + o, sizeof(dm) - o, " s%u@%u:t%02X",
                                (dd0 >> 16) & 3u, dd0 & 0xFFFFu, dd1 & 0x3Fu);
        }
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[PSDRAW]    decl:%s", dm);
      }
    }
  }
  const unsigned long tid = (unsigned long)GetCurrentThreadId();
  if (!m_hleThread) m_hleThread = tid;
  else if (m_hleThread != tid) {
    static bool s_said = false;
    if (!s_said) { s_said = true;
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[S4_THREAD] MISMATCH: draw submitted from tid %lu, expected %lu — "
          "this state is deliberately lock-free because execute() and present "
          "run on the SAME render thread", tid, m_hleThread);
    }
  }
  if (!d.srcVerts || !d.srcCount || !d.srcStride) {
    m_hleLastReject = HleReject::Shape; return false;
  }

  // ── Topology, and the expansions D3D12 has no native form for ─────────────
  D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  bool quadList = false, rectList = false;
  switch (d.primType) {
    case 4:  topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  break;  // kTriangleList
    case 6:  topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;  // kTriangleStrip
    case 8:  rectList = true; break;                              // kRectangleList
    case 13: quadList = true; break;                              // kQuadList
    default:
      // kPointList (1) needs the sprite-expansion GS and its own PSO variant —
      // out of this slice, and counted rather than silently dropped.
      m_hleLastReject = HleReject::Shape; return false;
  }
  const uint32_t outCount = rectList ? (d.srcCount / 3u) * 6u
                          : quadList ? (d.srcCount / 4u) * 6u
                                     : d.srcCount;
  if (!outCount) { m_hleLastReject = HleReject::Shape; return false; }

  auto& cache = getShaderCache();
  cache.ensureLoaded();   // MANDATORY before getXlatPso — a premature miss
                          // NEGATIVE-CACHES that key for the whole process.
  const CachedShader* vs = cache.findVs(d.vsHash);
  if (!vs || vs->layout.empty()) { m_hleLastReject = HleReject::NoVs; return false; }
  const uint32_t nAttrs = uint32_t(vs->layout.size());
  if (nAttrs == 0 || nAttrs > 16) { m_hleLastReject = HleReject::NoVs; return false; }
  if (!d.declRaw || d.declCount < nAttrs) {
    // iter-450k reject forensics: counts alone cannot say WHICH pass dies
    // here — the measure-quad hunt needs (ps, rt, prim) per reason.
    static std::atomic<uint32_t> s_ndN{0};
    const uint32_t ndn = s_ndN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ndn <= 12 || (ndn % 2000u) == 0u)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[REJ_NODECL] #%u vs=%016llX ps=%016llX prim=%u count=%u "
          "declRaw=%d declCount=%u nAttrs=%u",
          ndn, (unsigned long long)d.vsHash, (unsigned long long)d.psHash,
          d.primType, d.srcCount, (int)(d.declRaw != nullptr), d.declCount,
          nAttrs);
    m_hleLastReject = HleReject::NoDecl; return false;
  }

  // Decode the paired declaration elements into the fetch descriptors
  // xeDecodeAttr already understands. The guest bytes were byteswapped to host
  // order in the arena by the producer (fetch-constant endian 2 = 8IN32), so
  // the decode runs with endian 0 — swapping twice would be silent garbage.
  // iter-449u SEMANTIC MATCHING (reference video.cpp:4880-4904 pairs by
  // (usage, usageIndex), never by position): the old positional pairing fed
  // decl element i into shader input i, and for the dominant scene VS the two
  // orders provably differ (decl u=0,3,5,6,10 vs sidecar u=0,3,6,5,10) —
  // TEXCOORD data reached iTangent and tangents reached iTexCoord, collapsing
  // texture lookups to quasi-constant UVs. lay[] is built in SIDECAR order:
  // for shader input j, the DECL element with the same (usage, usageIndex) is
  // selected; positional is the logged fallback. Flags (signed/integer/bgra)
  // come from the DECL's own packed type — 79/183 sidecars are zeroed
  // placeholders, so sourcing flags there decoded signed 2_10_10_10 normals
  // as [0,1] and dropped D3DCOLOR's BGRA order.
  auto be32 = [](const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  struct DeclEl { uint32_t off, stream, type, usage, usageIndex; };
  DeclEl de[16];
  const uint32_t nDecl = d.declCount > 16u ? 16u : d.declCount;
  for (uint32_t i = 0; i < nDecl; i++) {
    const uint8_t* e = d.declRaw + i * 12u;
    const uint32_t d0 = be32(e), d1 = be32(e + 4), d2 = be32(e + 8);
    // iter-449u [S6_PAIR] decode: d2 low half is (usageIndex << 8) | a
    // type-low-byte residue (measured: TEXCOORD1 = 0x100, NORMAL0 = 0x0B9 =
    // FLOAT3's low byte). Taking the full 16 bits as usageIndex made most
    // semantic matches fail into chaotic per-element fallbacks.
    de[i] = DeclEl{d0 & 0xFFFFu, d0 >> 16, d1, d2 >> 16, (d2 >> 8) & 0xFFu};
  }
  // Semantic matching is only safe when the sidecar's (usage,usageIndex)
  // pairs are distinct — a fully-degenerate metadata set would pair every
  // input to decl element 0. Distinctness check, else positional.
  // iter-449u VERDICT: semantic re-pairing is DEFAULT-OFF. The workflow's
  // "TEXCOORD/TANGENT provably swapped" claim is REFUTED by the fixture A/B:
  // D3D12 matches input-layout elements to shader registers BY SEMANTIC
  // NAME, so packing in decl order with decl-order semantics is internally
  // consistent — re-packing in sidecar order breaks the agreement (measured:
  // the scene collapsed to band quads with the matcher on, tables correct).
  // The gate stays for study; the flag decode below is the real fix and is
  // unconditional.
  static const bool s_semMatch = [] {
    const char* e = std::getenv("SR_S6_SEMMATCH");
    return e && e[0] == '1';
  }();
  bool semanticOk = s_semMatch;
  for (uint32_t a = 0; a < nAttrs && semanticOk; a++)
    for (uint32_t b = a + 1; b < nAttrs; b++)
      if (vs->layout[a].usage == vs->layout[b].usage &&
          vs->layout[a].usageIndex == vs->layout[b].usageIndex) {
        semanticOk = false;
        break;
      }
  // iter-449u DIAGNOSTIC: dump both pairing tables for the first distinct
  // shaders — the ground truth for any mispair claim.
  {
    static std::atomic<unsigned> s_pairN{0};
    static uint64_t s_seen[24] = {};
    bool logged = false;
    for (uint64_t h : s_seen) if (h == d.vsHash) logged = true;
    if (!logged && s_pairN.load(std::memory_order_relaxed) < 24) {
      const unsigned slot = s_pairN.fetch_add(1, std::memory_order_relaxed);
      if (slot < 24) {
        s_seen[slot] = d.vsHash;
        char sc[128] = {0}, dc[160] = {0};
        size_t so = 0, dco = 0;
        for (uint32_t j = 0; j < nAttrs && so + 8 < sizeof(sc); j++)
          so += (size_t)snprintf(sc + so, sizeof(sc) - so, " %u.%u",
                                 vs->layout[j].usage, vs->layout[j].usageIndex);
        for (uint32_t j = 0; j < nDecl && dco + 14 < sizeof(dc); j++)
          dco += (size_t)snprintf(dc + dco, sizeof(dc) - dco, " s%u:%u.%u@%u",
                                  de[j].stream, de[j].usage, de[j].usageIndex,
                                  de[j].off);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_PAIR] vs=%016llX sidecar[%u]:%s | decl[%u]:%s | semOk=%d",
            (unsigned long long)d.vsHash, nAttrs, sc, nDecl, dc,
            semanticOk ? 1 : 0);
      }
    }
  }
  // ── iter-450M OFFSET PAIRING (SR_S6_OFFPAIR) ──────────────────────────────
  // The sidecar is not a semantic guess — each VfetchElement records the SLOT
  // and BYTE OFFSET the translated ucode actually fetches, and the cache key
  // hashes the vfetch-PATCHED bytes, so those offsets are exact per entry.
  // Pairing by usage labels (positional OR semantic) is a proxy that fails
  // whenever sidecar and decl orders/labels disagree (world VS: POSITION0 fed
  // from the packed NORMAL positionally; SEMMATCH fixed the world but killed
  // the title screen 2/2 live — some decl variant pairs wrong under it).
  // Here input i reads the bytes the ucode read: offset = sidecar offset,
  // format/flags from the DECL element AT that offset when one exists (bgra/
  // signed truth), else from the sidecar's own format. An input whose fetch
  // SLOT resolves to a stream other than the gathered one gets a bounded
  // dummy (offset 0) — never a cross-stream misread, never a refusal.
  // iter-450N BAKED default-ON (=0 keeps the A/B): 2/2 sequential live rolls
  // eyeballed — title screen crisp both rolls, backdrop textured/lit vs
  // positional's flat shards; fixture three-way strictly favors it.
  static const bool s_offPair = [] {
    const char* e = std::getenv("SR_S6_OFFPAIR");
    return !e || e[0] != '0';
  }();
  VfetchElement lay[16];
  bool streamInit = false;
  // ── iter-458 H-PAIR (SR_S6_PAIR_GUEST, default OFF) ───────────────────────
  // OFFPAIR's premise above ("the cache key hashes the vfetch-PATCHED bytes")
  // is false on the HLE path: publishDrawState hashes the VS before the guest
  // draw patches it, so the dominant world VS carry UNPATCHED sidecars (every
  // fetch offset 0) and OFFPAIR decodes every input from the POSITION bytes.
  // For exactly those sidecars, pair by the guest runtime's own rule
  // (ShaderAssembleVertexFetches_sub_82107A30; gfx/vfetch_pair_rule.hpp).
  // Patched sidecars keep OFFPAIR. NOT title-neutral: on the iter458_title /
  // title_city / 450uk_dialog replays the UI layer looks unchanged by eye but
  // the 3D backdrop DOES change, and neither side resembles Xenia's city
  // flyby; whole-era preservation is unverified (Astra 049). Offline result
  // only: the character becomes recognizable (hwin4 replay), the world stays
  // far from the 25023 oracle, so this is NOT the dominant world defect.
  static const bool s_pairGuest = [] {
    const char* e = std::getenv("SR_S6_PAIR_GUEST");
    return e && e[0] == '1';
  }();
  if (s_pairGuest && !d.hostVerts) {
    vfpair::DeclEl pde[16];
    for (uint32_t j = 0; j < nDecl; j++)
      pde[j] = vfpair::DeclEl{de[j].off, de[j].stream, de[j].type, de[j].usage,
                              de[j].usageIndex};
    if (vfpair::sidecarUnpatched(vs->layout.data(), nAttrs) &&
        vfpair::anySemanticMatch(vs->layout.data(), nAttrs, pde, nDecl)) {
      static std::atomic<uint64_t> s_pgHow[4];
      static std::atomic<uint64_t> s_pgDraws{0};
      uint32_t hows = 0;  // 4 bits per input, for the first-draws log
      for (uint32_t i = 0; i < nAttrs; i++) {
        vfpair::How how = vfpair::How::Matched;
        lay[i] = vfpair::pairGuestRule(vs->layout[i], pde, nDecl, de[0].stream,
                                       d.srcStride, &xeFormatBytes, &how);
        // H-MS DIAGNOSTIC (SR_S6_PAIR_FOREIGN_POS0=1, default OFF): decode a
        // foreign-stream dummy in the format of the gathered stream's offset-0
        // element (POSITION1 := POSITION0 bytes, OFF's benign value) instead of
        // the foreign element's own format (k_32 of float bits = 3.2e9). Kills
        // or confirms "the stretched shape is the stream-1 dummy".
        // RESULT (hwin4 replay): H-MS REJECTED -- activation witnessed by
        // [S6_PAIRDUMP] (POSITION1 = POSITION0 bytes), frames pixel-identical
        // on 8/8 presents. Kept only as the repro for that rejection.
        static const bool s_foreignPos0 = [] {
          const char* e = std::getenv("SR_S6_PAIR_FOREIGN_POS0");
          return e && e[0] == '1';
        }();
        if (s_foreignPos0 && how == vfpair::How::DummyForeign) {
          for (uint32_t j = 0; j < nDecl; j++)
            if (de[j].stream == de[0].stream && de[j].off == 0u &&
                xeFormatBytes(de[j].type & 0x3Fu) &&
                xeFormatBytes(de[j].type & 0x3Fu) <= d.srcStride) {
              lay[i].format = de[j].type & 0x3Fu;
              lay[i].isSigned = ((de[j].type >> 8) & 1u) != 0;
              lay[i].isInteger = ((de[j].type >> 9) & 1u) != 0;
              lay[i].bgra = ((de[j].type >> 11) & 1u) != 0;
              break;
            }
        }
        s_pgHow[uint32_t(how)].fetch_add(1, std::memory_order_relaxed);
        if (i < 8) hows |= uint32_t(how) << (i * 4);
      }
      const uint64_t n = s_pgDraws.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_PAIRGUEST] draw#%llu vs=%016llX nAttrs=%u hows=%08X "
            "(0=match 1=default 2=foreign 3=bad) | totals match=%llu "
            "default=%llu foreign=%llu bad=%llu",
            (unsigned long long)n, (unsigned long long)d.vsHash, nAttrs, hows,
            (unsigned long long)s_pgHow[0].load(), (unsigned long long)s_pgHow[1].load(),
            (unsigned long long)s_pgHow[2].load(), (unsigned long long)s_pgHow[3].load());
      goto layDone;
    }
  }
  // iter-450Q: host-synthesized vertices (movie quad) are laid out
  // positionally BY CONSTRUCTION — the sidecar's fetch offsets describe the
  // guest's own vertex layout for this VS, not our synthesized buffer.
  // Pairing them by sidecar offset swapped position<->uv and every movie
  // frame rendered as a rotated top-right-quadrant image (OFFPAIR bake
  // regression; window proven iter450k_upswap vs iter450M_offpair).
  if (s_offPair && !d.hostVerts) {
    const uint32_t gatherStream = de[0].stream;
    for (uint32_t i = 0; i < nAttrs; i++) {
      const VfetchElement& sv = vs->layout[i];
      const uint32_t svStream = sv.slot <= 95u ? (95u - sv.slot) : 0u;
      const uint32_t svOff =
          sv.offsetDwords >= 0 ? uint32_t(sv.offsetDwords) * 4u : 0xFFFFu;
      lay[i] = VfetchElement{};
      // Decl element at the same (stream, offset) — the flag authority.
      int dk = -1;
      for (uint32_t j = 0; j < nDecl; j++)
        if (de[j].stream == svStream && de[j].off == svOff) { dk = int(j); break; }
      const bool foreign = svStream != gatherStream;
      const uint32_t fmt = dk >= 0 ? (de[dk].type & 0x3Fu) : sv.format;
      const uint32_t fb = xeFormatBytes(fmt);
      if (foreign || !fb || (svOff % 4u) || svOff + fb > d.srcStride) {
        // Bounded dummy: position bytes decoded as this input's format —
        // deterministic, in-range, and correct-by-vacuity when the shader
        // never reads the input (the world VS's POSITION1 case).
        lay[i].offsetDwords = 0;
        lay[i].format = fb ? fmt : 38u;  // 38 = 32_32_FLOAT fallback
        lay[i].usage = sv.usage;
        lay[i].usageIndex = sv.usageIndex;
        static uint64_t s_dum = 0;
        if (++s_dum <= 8)
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[S6_OFFPAIR] input%u -> DUMMY (%s: slotStream=%u gather=%u "
              "off=%u fb=%u stride=%u) vs=%016llX",
              i, foreign ? "foreign stream" : "bad offset/format", svStream,
              gatherStream, svOff, fb, d.srcStride,
              (unsigned long long)d.vsHash);
        continue;
      }
      lay[i].offsetDwords = int32_t(svOff / 4u);
      lay[i].format = fmt;
      lay[i].usage = sv.usage;
      lay[i].usageIndex = sv.usageIndex;
      if (dk >= 0) {
        lay[i].isSigned = ((de[dk].type >> 8) & 1u) != 0;
        lay[i].isInteger = ((de[dk].type >> 9) & 1u) != 0;
        lay[i].bgra = ((de[dk].type >> 11) & 1u) != 0;
      } else {
        lay[i].isSigned = sv.isSigned;
        lay[i].isInteger = sv.isInteger;
        lay[i].bgra = sv.bgra;
      }
    }
    // iter-458 E1 CENSUS (SR_S6_PAIR_CENSUS=1, default OFF, log-only): for the
    // draws OFFPAIR decided, would the guest rule have paired the same bytes?
    // A PATCHED sidecar records the patch for whatever decl was bound when the
    // VS was first hashed; a later draw with a different decl reuses the pinned
    // hash, so a disagreement names a stale-patch (or mislabelled) sidecar.
    // Unpatched sidecars are counted separately (every input sits at +0).
    {
      static const bool s_pairCensus = [] {
        const char* e = std::getenv("SR_S6_PAIR_CENSUS");
        return e && e[0] == '1';
      }();
      if (s_pairCensus) {
        vfpair::DeclEl pde[16];
        for (uint32_t j = 0; j < nDecl; j++)
          pde[j] = vfpair::DeclEl{de[j].off, de[j].stream, de[j].type, de[j].usage,
                                  de[j].usageIndex};
        const bool unpatched = vfpair::sidecarUnpatched(vs->layout.data(), nAttrs);
        static std::atomic<uint64_t> s_cAgree[2], s_cDiff[2], s_cDraws[2], s_cDrawsDiff[2];
        const int cls = unpatched ? 1 : 0;
        bool any = false;
        for (uint32_t i = 0; i < nAttrs; i++) {
          vfpair::How how{};
          const VfetchElement g = vfpair::pairGuestRule(vs->layout[i], pde, nDecl, de[0].stream,
                                                        d.srcStride, &xeFormatBytes, &how);
          const bool same = g.offsetDwords == lay[i].offsetDwords && g.format == lay[i].format;
          (same ? s_cAgree : s_cDiff)[cls].fetch_add(1, std::memory_order_relaxed);
          if (!same) {
            any = true;
            // One draw's worth of diffs per distinct VS (first 48 VS), so a
            // single chatty shader cannot hide the rest.
            static uint64_t s_cVs[48] = {};
            static uint64_t s_cVsDraw[48] = {};
            const uint64_t drawKey = s_cDraws[0].load() + s_cDraws[1].load() + 1;
            bool logIt = false;
            if (!unpatched)
              for (int q = 0; q < 48; q++) {
                if (s_cVs[q] == d.vsHash) { logIt = s_cVsDraw[q] == drawKey; break; }
                if (s_cVs[q] == 0) { s_cVs[q] = d.vsHash; s_cVsDraw[q] = drawKey; logIt = true; break; }
              }
            if (logIt)
              SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[S6_PAIRCENSUS] PATCHED-DIFF vs=%016llX in%u u%u.%u offpair(+%d fmt%u) "
                  "guest(+%d fmt%u how%u) sidecar(slot%u +%d stride%u fmt%u) srcStride=%u",
                  (unsigned long long)d.vsHash, i, vs->layout[i].usage,
                  vs->layout[i].usageIndex, lay[i].offsetDwords * 4, lay[i].format,
                  g.offsetDwords * 4, g.format, unsigned(how), vs->layout[i].slot,
                  vs->layout[i].offsetDwords * 4, vs->layout[i].strideDwords,
                  vs->layout[i].format, d.srcStride);
          }
        }
        s_cDraws[cls].fetch_add(1, std::memory_order_relaxed);
        if (any) s_cDrawsDiff[cls].fetch_add(1, std::memory_order_relaxed);
        const uint64_t n = s_cDraws[0].load() + s_cDraws[1].load();
        if ((n & (n - 1)) == 0 || n % 2000 == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[S6_PAIRCENSUS] draws=%llu | patched: draws=%llu diffDraws=%llu inputs "
              "agree=%llu diff=%llu | unpatched: draws=%llu diffDraws=%llu inputs "
              "agree=%llu diff=%llu",
              (unsigned long long)n, (unsigned long long)s_cDraws[0].load(),
              (unsigned long long)s_cDrawsDiff[0].load(), (unsigned long long)s_cAgree[0].load(),
              (unsigned long long)s_cDiff[0].load(), (unsigned long long)s_cDraws[1].load(),
              (unsigned long long)s_cDrawsDiff[1].load(), (unsigned long long)s_cAgree[1].load(),
              (unsigned long long)s_cDiff[1].load());
      }
    }
    goto layDone;
  }
  for (uint32_t i = 0; i < nAttrs; i++) {
    uint32_t k = i;  // positional fallback
    if (semanticOk) {
      // Match ONLY among elements of the snapshotted stream (element 0's —
      // the producer's `slot = 95 - stream` resolution). Decls duplicate
      // (usage,usageIndex) across streams (morph targets); a cross-stream
      // match would decode this buffer's bytes at the OTHER stream's offsets
      // and then trip the multi-stream refusal — measured: the whole scene
      // vanished to a handful of band quads on the first matcher build.
      for (uint32_t j = 0; j < nDecl; j++)
        if (de[j].stream == de[0].stream &&
            de[j].usage == vs->layout[i].usage &&
            de[j].usageIndex == vs->layout[i].usageIndex) {
          k = j;
          break;
        }
      if (k != i) {
        static uint64_t s_remap = 0;
        if (++s_remap <= 8)
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[S6_EXPAND] semantic remap: input%u (u%u.%u) <- decl elem%u "
              "(was elem%u positionally) vs=%016llX",
              i, vs->layout[i].usage, vs->layout[i].usageIndex, k, i,
              (unsigned long long)d.vsHash);
      }
    }
    // iter-445 S6k: the producer snapshots ONE stream (element 0's, resolved
    // through `slot = 95 - stream`), so a CONSUMED element from a different
    // stream would be decoded at this buffer's offsets out of the wrong
    // bytes. Only consumed elements matter — unread extra decl elements
    // (commonly a 7th in stream 1) no longer refuse anything, because
    // matching never selects them.
    if (!streamInit) { m_hleDrawStream = de[k].stream; streamInit = true; }
    else if (de[k].stream != m_hleDrawStream) {
      static uint64_t s_ms = 0;
      if (++s_ms <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_EXPAND] REFUSED multi-stream: input%u <- elem%u is stream "
            "%u, first is stream %u (vs=%016llX consumes %u attrs)",
            i, k, de[k].stream, m_hleDrawStream, (unsigned long long)d.vsHash,
            nAttrs);
      m_hleLastReject = HleReject::MultiStream;
      return false;
    }
    lay[i] = VfetchElement{};
    lay[i].offsetDwords = int32_t(de[k].off / 4u);
    lay[i].format = de[k].type & 0x3Fu;
    lay[i].usage = de[k].usage;
    lay[i].usageIndex = de[k].usageIndex;
    // Packed declType byte 1 (verified against the reference's GuestDeclType
    // constants, video.h:213-238): bit8 = signed, bit9 = integer, bit11 =
    // BGRA swizzle (D3DCOLOR 0x182886 is the only carrier).
    lay[i].isSigned = ((de[k].type >> 8) & 1u) != 0;
    lay[i].isInteger = ((de[k].type >> 9) & 1u) != 0;
    lay[i].bgra = ((de[k].type >> 11) & 1u) != 0;
    // A declared attribute must lie WHOLLY inside the guest's own vertex
    // record, and its format must be one we actually decode. An unknown format
    // refuses the draw (counted) rather than fetching zeros — a draw silently
    // expanded from the wrong bytes is worse than a skipped one.
    const uint32_t fb = xeFormatBytes(lay[i].format);
    const uint32_t off = de[k].off;
    if (!fb || (off % 4u) || off + fb > d.srcStride) {
      static uint64_t s_bad = 0;
      if (++s_bad <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_EXPAND] REFUSED elem%u off=%u fmt=%u (=%u B) srcStride=%u "
            "vs=%016llX", k, off, lay[i].format, fb, d.srcStride,
            (unsigned long long)d.vsHash);
      m_hleLastReject = HleReject::BadFormat;
      return false;
    }
  }
layDone:
  // Every source vertex the expansion will touch must be inside the snapshot.
  if (uint64_t(d.srcCount) * d.srcStride > d.srcBytes) {
    m_hleLastReject = HleReject::Shape; return false;
  }

  if (!hleEnsureRings()) { m_hleLastReject = HleReject::NoPso; return false; }
  // A draw whose constant file never arrived would upload ZEROS and render a
  // collapsed transform that still counted as recorded. The producer publishes
  // SetConstants before every draw that carries vertices, so this can only
  // happen if that block write was rejected — measured at 0, but "measured 0"
  // is not "impossible", and silent zeros are indistinguishable from a real
  // transform bug. Refuse and name it instead.
  if (!m_hleHaveConst && !d.vsConst && !d.psConst) {
    m_hleLastReject = HleReject::NoConst; return false;
  }
  if (m_hleBatches.size() >= kHleMaxDraws) {
    m_hleListFull++; m_hleLastReject = HleReject::TooManyDraws; return false;
  }

  // ── Capacity FIRST, writes after ──────────────────────────────────────────
  // Checking after writing would leave partially-staged bytes behind a refused
  // draw, and the cursors would disagree with what is actually in the ring.
  const uint32_t stride = nAttrs * 16u;
  const uint64_t vbHalfBase = uint64_t(m_hleHalf) * (kHleVBBytes / 2);
  const uint64_t vbHalfEnd = vbHalfBase + (kHleVBBytes / 2);
  if (m_hleVBUsed < vbHalfBase || m_hleVBUsed > vbHalfEnd) m_hleVBUsed = vbHalfBase;
  if (m_hleVBUsed + uint64_t(outCount) * stride > vbHalfEnd) {
    m_hleVbFull++; m_hleLastReject = HleReject::VbFull; return false;
  }
  auto alignUp = [](uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); };
  const uint64_t cbHalfBase = uint64_t(m_hleHalf) * (kHleCBBytes / 2);
  const uint64_t cbHalfEnd = cbHalfBase + (kHleCBBytes / 2);
  if (m_hleCBUsed < cbHalfBase || m_hleCBUsed > cbHalfEnd) m_hleCBUsed = cbHalfBase;
  uint64_t cb = alignUp(m_hleCBUsed, 256);
  const uint64_t vsOff = cb;                cb += 4096;
  const uint64_t psOff = alignUp(cb, 256);  cb = psOff + 4096;  // iter-449v
  const uint64_t shOff = alignUp(cb, 256);  cb = shOff + kHleSharedBytes;
  if (cb > cbHalfEnd) {
    m_hleCbFull++; m_hleLastReject = HleReject::CbFull; return false;
  }

  // PSO before any write, so a miss costs no ring space.
  // iter-444 S6h: the guest's OWN depth/blend/mask when it reached us. The
  // hardcoded fallback (depth OFF, opaque blend, all channels) stays for the
  // pre-state era and for the movie quad, which genuinely wants exactly that.
  // Depth was the visible one: with no depth test general geometry rendered as
  // a shard pile because every triangle painted over every other.
  const uint32_t dCtl  = d.stateValid ? d.depthControl : 0u;
  if (d.stateValid) {
    // zfunc is RB_DEPTHCONTROL bits 4-6 (Xenia CompareFunction: 4=GREATER,
    // 6=GREATER_EQUAL). The clear must be the FAR value for that comparison or
    // the test rejects everything.
    const uint32_t zf = (dCtl >> 4) & 7u;
    m_hleDepthClear = (zf == 4u || zf == 6u) ? 0.0f : 1.0f;
  }
  const uint32_t bCtl  = d.stateValid ? d.blendControl : 0x00010001u;
  const uint32_t cMask = d.stateValid ? (d.colorMask & 0xFu) : 0xFu;
  ID3D12PipelineState* pso = getXlatPso(d.vsHash, d.psHash, dCtl, bCtl, cMask,
      /*depthOnly=*/false, /*hdrTarget=*/false, /*pointSprite=*/false,
      /*poolDepth=*/false);
  if (!pso) { m_hleLastReject = HleReject::NoPso; return false; }

  // ── Expand into the vertex ring ───────────────────────────────────────────
  const uint64_t vbOffset = m_hleVBUsed;
  float* out = reinterpret_cast<float*>(m_hleVBPtr + vbOffset);
  const uint32_t nf = nAttrs * 4;
  auto fetchOne = [&](uint32_t idx, float* dst) {
    const uint8_t* src = d.srcVerts + size_t(idx) * d.srcStride;
    for (uint32_t a = 0; a < nAttrs; a++)
      xeDecodeAttr(lay[a], src + lay[a].offsetDwords * 4, /*endian=*/0u, dst + a * 4);
  };
  uint32_t written = 0;
  if (rectList) {
    // iter-359k, reused verbatim: the 4th vertex mirrors the RIGHT-ANGLE corner
    // across the opposite edge, and WHICH vertex that is depends on the rect's
    // winding. A fixed v3 = -v0+v1+v2 puts wrong-winding rects across the
    // screen (the black depth-prime wedges and the diagonal UI slices).
    const uint32_t rects = d.srcCount / 3u;
    float v[3][64], v3[64];
    for (uint32_t r = 0; r < rects; r++) {
      for (int k = 0; k < 3; k++) fetchOne(r * 3u + k, v[k]);
      int corner = 0;
      for (int i = 0; i < 3; i++) {
        const int j = (i + 1) % 3, k2 = (i + 2) % 3;
        const bool sxj = v[i][0] == v[j][0], syk = v[i][1] == v[k2][1];
        const bool sxk = v[i][0] == v[k2][0], syj = v[i][1] == v[j][1];
        if ((sxj && syk) || (sxk && syj)) { corner = i; break; }
      }
      const int a = (corner + 1) % 3, b = (corner + 2) % 3;
      for (uint32_t f = 0; f < nf; f++) v3[f] = v[a][f] + v[b][f] - v[corner][f];
      std::memcpy(out + size_t(written + 0) * nf, v[0], nf * 4);
      std::memcpy(out + size_t(written + 1) * nf, v[1], nf * 4);
      std::memcpy(out + size_t(written + 2) * nf, v[2], nf * 4);
      std::memcpy(out + size_t(written + 3) * nf, v[a], nf * 4);
      std::memcpy(out + size_t(written + 4) * nf, v[b], nf * 4);
      std::memcpy(out + size_t(written + 5) * nf, v3, nf * 4);
      written += 6;
    }
  } else if (quadList) {
    // kQuadList: 4 verts per quad -> 2 triangles, split as Xenia's D3D12 GS does (gfx/pm4_quad_gate.hpp).
    const uint32_t quads = d.srcCount / 4u;
    float v[4][64];
    for (uint32_t q = 0; q < quads; q++) {
      for (int k = 0; k < 4; k++) fetchOne(q * 4u + k, v[k]);
      // iter-467 freezefx (gfx/pm4_quad_gate.hpp): Xenia's GS split (0,1,3)(3,1,2); SR_PM4_QUAD_STRIP=0 old.
      const int* sp = pm4QuadSplit();
      for (int k = 0; k < 6; k++) std::memcpy(out + size_t(written + k) * nf, v[sp[k]], nf * 4);
      written += 6;
    }
  } else {
    for (uint32_t i = 0; i < d.srcCount; i++, written++)
      fetchOne(i, out + size_t(written) * nf);
  }
  if (!written) { m_hleLastReject = HleReject::Shape; return false; }

  // iter-458 H-PAIR evidence (SR_S6_PAIR_DUMP=1, default OFF): the pairing and
  // the DECODED vertex 0 for the first 2 draws of each of the first 32 VS
  // hashes. Astra 048: a pairing claim needs decoded attributes, not tables.
  {
    static const bool s_pairDump = [] {
      const char* e = std::getenv("SR_S6_PAIR_DUMP");
      return e && e[0] == '1';
    }();
    if (s_pairDump) {
      static uint64_t s_dvs[32] = {};
      static uint32_t s_dn[32] = {};
      int slot = -1;
      for (int q = 0; q < 32; q++) {
        if (s_dvs[q] == d.vsHash) { slot = q; break; }
        if (s_dvs[q] == 0) { s_dvs[q] = d.vsHash; slot = q; break; }
      }
      if (slot >= 0 && s_dn[slot] < 2) {
        s_dn[slot]++;
        for (uint32_t a = 0; a < nAttrs; a++) {
          const float* v = out + a * 4;
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[S6_PAIRDUMP] vs=%016llX draw%u in%u u%u.%u -> off=%d fmt=%u "
              "s%u i%u b%u stride=%u | v0=(%.5g, %.5g, %.5g, %.5g)",
              (unsigned long long)d.vsHash, s_dn[slot], a, lay[a].usage,
              lay[a].usageIndex, lay[a].offsetDwords * 4, lay[a].format,
              lay[a].isSigned ? 1u : 0u, lay[a].isInteger ? 1u : 0u,
              lay[a].bgra ? 1u : 0u, d.srcStride, v[0], v[1], v[2], v[3]);
        }
      }
    }
  }

  // ── Stage the constants ───────────────────────────────────────────────────
  // These are DEVICE state: a draw with no SetConstants before it legitimately
  // inherits the previous draw's, so the held copy is the source of truth and
  // an update merely refreshes it.
  if (d.vsConst) { std::memcpy(m_hleVsConst.data(), d.vsConst, 4096); m_hleHaveConst = true; }
  if (d.psConst) { std::memcpy(m_hlePsConst.data(), d.psConst, 4096); m_hleHaveConst = true; }
  std::memcpy(m_hleCBPtr + vsOff, m_hleVsConst.data(), 4096);
  std::memcpy(m_hleCBPtr + psOff, m_hlePsConst.data(), 4096);

  // SharedConstants (XenosRecomp shader_recompiler.cpp:1290-1294): dwords 0..15
  // = Texture2D descriptor indices, 16..31 Texture3D, 32..47 TextureCube,
  // 48..63 SAMPLER indices; c16 (dword 64) bools; c18 (72..75) the NDC
  // scale/offset. c18 stays IDENTITY here — the movie quad is already in clip
  // space, and real viewport/VTE handling arrives with the render state.
  uint32_t shared[128] = {};
  // iter-449u: all 16 2D descriptor indices + all 16 sampler indices (was
  // slots 0-2 only — s3+ sampled the 1x1 white fallback on every draw).
  // iter-449x: the 3D table (dwords 16-31) is LIVE (volume LUT slice). The
  // CUBE table (dwords 32-47) remains the named open gap — env-map slots
  // still resolve the white cube dummy until that slice lands.
  for (uint32_t s = 0; s < 16; s++) {
    shared[s] = d.texSlot[s];
    // ── iter-450UE FIX B: THE GUEST'S OWN TEXTURE COMPONENT SWIZZLE ─────────
    // SR_HLE_TEX_SWIZZLE: 0 = off (the A/B control), 1 = census only (no pixel
    // change), 2 = apply to SINGLE-CHANNEL host formats only (the k_8 class
    // that PRESS START is), 3 = apply to every binding.
    // ⚠ WHY MODES: 55.6% of live bindings carry a non-identity guest swizzle,
    // but only the k_8 000R case is PROVEN uncompensated (iter-450UC). Mode 3
    // is not a bigger fix, it is a bigger CLAIM — it must wait on the mode-1
    // census, which also answers whether the movie planes carry a swizzle.
    //
    // iter-455n: DEFAULT IS NOW 2 (director order, glyph-regression pivot).
    // Mode 2 is the PROVEN case — the single-channel atlas maps into alpha,
    // which is what makes glyph text render instead of solid placeholder bars
    // (iter-450ui rendered complete storage-dialog text with this on; the
    // iter-455m control, with it off, shows bars). It was solved
    // experimentally and then left behind a default-off gate for five
    // iterations. `=0` is retained as the A/B control; mode 3 stays opt-in
    // because the census that would justify it was never run.
    {
      static const int s_swzMode = [] {
        const char* e = std::getenv("SR_HLE_TEX_SWIZZLE");
        return e ? atoi(e) : 2;   // iter-455n: baked default (was 0)
      }();
      // slot 0 is the 1x1 white dummy: a dummy has no guest format, so there
      // is nothing to compose and nothing to fix.
      if (s_swzMode >= 1 && shared[s] && d.texSwiz[s] != 0xFFFFu) {
        // ⛔ IDENTITY GUARD. The fetch entry we decoded must describe the
        // texture actually bound to this sampler. They come from one shadow
        // captured at two moments; a disagreement means staleness, and
        // applying one texture's swizzle to another is a silent wrong picture.
        if ((d.texSwizBase[s] & 0x1FFFFFFFu) ==
            (d.texKey[s] & 0x1FFFFFFFu)) {
          const uint32_t v =
              hleEnsureTexView(d.texKey[s], d.texSwiz[s], s_swzMode);
          if (v) shared[s] = v;
        } else {
          m_swzBaseMismatch.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    // iter-449x: Texture3D range index (space1; 0 = white 3D dummy). The
    // shader's compile-time tfetch dimension picks which range it reads, so
    // publishing both is exactly the reference's SetTextureInRenderThread.
    shared[16 + s] = d.texSlot3D[s];
    // ── iter-457 H1(b): THE THREE-BAND RULE (SR_HLE_CUBE_BIND, default OFF) ─
    // \u26d4 The director's contract: every bind writes ALL THREE bands, with
    // the real index only in the dimensionality-matching band and ZERO in the
    // other two. The decision is gfx/cube_bind_rule.hpp's, which is pure and
    // mutation-tested; this is only where it is applied.
    //
    // ⚠ THIS IS NOT PIXEL-NEUTRAL, which is why it is gated. The lines above
    // publish the 2D index AND the 3D index for the same slot unconditionally.
    // For a key that lives in the volume table, the rule STOPS publishing its
    // 2D index - so any shader reading the 2D band for a volume-bound slot
    // changes what it samples. Whether one does is a measurement, not an
    // assumption, and `=0` keeps the A/B.
    {
      static const bool s_cubeBind = [] {
        const char* e = std::getenv("SR_HLE_CUBE_BIND");
        return e && e[0] == '1';
      }();
      if (s_cubeBind) {
        namespace CB = sr::gfx::cubebind;
        const auto plan = CB::planBands(
            CB::dimFromTables(d.texSlotCube[s] != 0, d.texSlot3D[s] != 0),
            shared[s], d.texSlot3D[s], d.texSlotCube[s]);
        shared[s] = plan.band2D;
        shared[16 + s] = plan.band3D;
        shared[32 + s] = plan.bandCube;
      }
    }
    // iter-450T: the guest's OWN sampler state, decoded from the fetch-shadow
    // snapshot (SetSamplerState RMWs it in place — IDA-verified; see
    // hle_video.cpp's SetRenderState consumer). SR_HLE_SAMPSTATE: 0 = the
    // pre-450T fixed clamp+linear, 1 = clamp only (filter forced linear),
    // 2 = clamp+filter. 0xFF = no data -> fixed 7 regardless (old traces,
    // capture miss).
    //
    // DEFAULT 1 — DIRECTOR RULING 2026-08-10 (docs/CLAIMS_LEDGER.md), applied
    // in iter-450W. The iter-450T bake to mode 2 was reverted by that ruling
    // and the change was never made; this line carried default 2 for a further
    // arc while the ledger said otherwise.
    // ⛔ The old comment here claimed "2 gated live rolls eyeballed clean across
    // movie/title/backdrop eras". That wording is INVALIDATED and removed: the
    // title era is NOT clean (PRESS START is placeholder boxes, Superman is
    // absent, the backdrop geometry is fractured versus the Xenia oracle), so
    // it could never have been eyeballed clean.
    // WHY 1 AND NOT 2: mode 2 is not "the guest's sampler state". It decodes
    // only U/V clamp and mag_filter, then uses that single bit to pick a
    // COMBINED D3D12 min/mag/mip filter. min_filter, mip_filter, anisotropy, W
    // addressing, border colour, LOD bias and mip bounds are all unconsumed.
    // Mode 1 keeps the independently reproduced guest U/V address modes — the
    // part that is actually decoded — and forces the existing linear filter.
    // Mode 2 stays reachable through the gate for A/B work; it is not a default
    // until the renderer carries the complete Xenos sampler contract and mips.
    {
      static const int s_sampMode = [] {
        const char* e = std::getenv("SR_HLE_SAMPSTATE");
        return e ? atoi(e) : 1;
      }();
      shared[48 + s] =
          (s_sampMode == 0 || d.sampSel[s] == 0xFF)
              ? 7u
              : (s_sampMode == 1 ? uint32_t((d.sampSel[s] & 6u) | 1u)
                                 : uint32_t(d.sampSel[s]));
    }
  }
  // iter-449v: g_Booleans — the guest's VS/PS bool constants (13 VS branch on
  // them; permanently 0 before this). Reference formula video.cpp:4299-4307.
  shared[64] = d.boolBits;
  {
    // DIAGNOSTIC ONLY (SR_S4_YYY=1), never baked: bind LUMA to all three
    // texture registers, which splits a colour cast into "indices reach the PS"
    // vs "the chroma data is wrong".
    static const bool s_yyy = [] {
      const char* e = std::getenv("SR_S4_YYY");
      return e && e[0] == '1';
    }();
    if (s_yyy) shared[1] = shared[2] = shared[0];
  }
  // ── iter-444 S6h: c18 (g_NdcScaleOffset) + the viewport, from the guest ────
  // PORTED VERBATIM from the xlat drain's computation rather than re-derived:
  // that code carries iter-359i (pixel space is the PASS surface, not the
  // window), iter-359k and iter-362 (a DEGENERATE vpScale ~0.5 must fall back
  // to the full RT or the whole draw collapses into a 1x1 viewport at the
  // top-left — the "text on row 0" bug). Re-deriving it would re-earn those.
  float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
  D3D12_VIEWPORT vpOut{0.0f, 0.0f, float(m_width), float(m_height), 0.0f, 1.0f};
  if (d.stateValid) {
    const float kMinPass = 1.0f;
    if (d.vteCntl & 0x1u) {
      // VTE on: the VS emits CLIP SPACE; the viewport regs map NDC to pixels
      // exactly like D3D12's own viewport transform (Y flip via vsy < 0).
      const float vsx = d.vpScale[0], vsy = d.vpScale[1];
      const float vox = d.vpOffset[0], voy = d.vpOffset[1];
      const float W = 2.0f * vsx, H = -2.0f * vsy;
      if (W >= kMinPass && H >= kMinPass) {
        vpOut.TopLeftX = vox - vsx;
        vpOut.TopLeftY = voy + vsy;
        vpOut.Width = W;
        vpOut.Height = H;
        float z0 = d.vpOffset[2], z1 = d.vpOffset[2] + d.vpScale[2];
        if (z1 < z0) { const float t2 = z0; z0 = z1; z1 = t2; }
        vpOut.MinDepth = z0 < 0 ? 0.0f : (z0 > 1 ? 1.0f : z0);
        vpOut.MaxDepth = z1 < vpOut.MinDepth ? vpOut.MinDepth : (z1 > 1 ? 1.0f : z1);
        if (vpOut.MaxDepth <= vpOut.MinDepth) vpOut.MaxDepth = vpOut.MinDepth + 1e-3f;
      }
    } else {
      // VTE off (UI / clear rects): the VS emits PIXELS, remapped to NDC in the
      // shader epilogue through c18. The pixel space is the PASS surface.
      float passW = 2.0f * (d.vpScale[0] < 0 ? -d.vpScale[0] : d.vpScale[0]);
      float passH = 2.0f * (d.vpScale[1] < 0 ? -d.vpScale[1] : d.vpScale[1]);
      if (passW < kMinPass) passW = float(m_width);
      if (passH < kMinPass) passH = float(m_height);
      ndc[0] = 2.0f / passW;
      ndc[1] = -2.0f / passH;
      ndc[2] = -1.0f;
      ndc[3] = 1.0f;
      vpOut.Width = passW < float(m_width) ? passW : float(m_width);
      vpOut.Height = passH < float(m_height) ? passH : float(m_height);
    }
  }
  float* shf = reinterpret_cast<float*>(shared);
  std::memcpy(&shf[72], ndc, 16);   // c18
  std::memcpy(m_hleCBPtr + shOff, shared, sizeof(shared));

  const D3D12_GPU_VIRTUAL_ADDRESS cbBase = m_hleCB->GetGPUVirtualAddress();
  HleBatch b{};
  b.pso = pso;
  b.vsCbv = cbBase + vsOff;
  b.psCbv = cbBase + psOff;
  b.sharedCbv = cbBase + shOff;
  b.vbOffset = vbOffset;
  b.vertexCount = written;
  b.stride = stride;
  b.topology = topo;
  b.viewport = vpOut;
  // iter-446 S1: latch THIS draw's targets, at submit, from the live mirror.
  for (uint32_t i = 0; i < 4u; i++) b.rt[i] = m_hleBoundRT[i];
  b.ds = m_hleBoundDS;
  b.vsHash = d.vsHash;
  b.psHash = d.psHash;
  // ── iter-446 S5: the PSO for THIS draw's own bound target ─────────────────
  // Refused BY NAME rather than approximated: a draw sent to a surface whose
  // format the PSO was not built for is a D3D12 error, and one sent to the
  // wrong surface renders plausibly and is wrong.
  b.poolPso = nullptr;
  if (drawToPoolEnabled()) {
    // ── iter-449b: A MISSING DEPTH SURFACE IS NOT A MISSING TARGET ──────────
    // The old condition demanded BOTH rt[0] AND ds, and counted the union as
    // `noRt` — 642,213 draws/run, HALF of everything recorded. The
    // [S6_NULLBIND] probe then proved RT0 is NEVER null-bound (86,016 null
    // binds, every one idx 1-3, shadow agreeing) — so that entire bucket was
    // depth-LESS passes (guest binds RT0, sets DS to NULL — the standard UI/2D
    // choreography), refused for lacking a depth buffer they never asked for.
    // A depth-less draw gets a PSO with DSVFormat=UNKNOWN + depth disabled and
    // a null DSV at bind; `noDsDraws` counts them as a CENSUS, not a refusal.
    const bool hasDs = b.ds && b.ds->res;
    if (!b.rt[0] || !b.rt[0]->res) {
      m_poolRefusedNoRt++;
    } else if (!formatRenderable(b.rt[0]->dxgi) ||
               (hasDs && b.ds->dsvFormat == DXGI_FORMAT_UNKNOWN)) {
      // ── iter-447: THE REFUSAL IS NOW ABOUT CAPABILITY, NOT ABOUT ONE PAIR ──
      // It used to read `dxgi != R8G8B8A8_UNORM`, with the comment "no draw
      // demands it today". 723,398 draws/run demand it — that premise expired
      // the moment SR_S6_WIDE opened. What must STILL be refused is a format
      // this adapter cannot render to (or a depth surface with no typed view):
      // handing D3D12 such a desc yields a failed PSO create, which surfaces as
      // a silent nullptr and a dropped draw — a wrong picture with no counter.
      // Refuse BY NAME, exactly as before, but on a condition that is about the
      // HARDWARE rather than about our own missing variant.
      m_poolRefusedFmt++;
      static uint64_t s_fmtLog = 0;
      if (++s_fmtLog <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[SURF_POOL] REFUSED draw: rtv fmt %u not renderable (xenos %u) or "
            "dsv UNKNOWN (dsv %u) — no guessed variant",
            (unsigned)b.rt[0]->dxgi, (unsigned)b.rt[0]->xenosFormat,
            hasDs ? (unsigned)b.ds->dsvFormat : 0u);
    } else {
      // The BOUND surfaces' own formats, straight through — the reference's
      // `args.renderTarget->format` / `args.depthStencil->format` shape.
      if (!hasDs) m_poolNoDsDraws++;
      b.poolPso = getXlatPso(d.vsHash, d.psHash, d.depthControl, d.blendControl,
                             d.colorMask, /*depthOnly=*/false,
                             /*hdrTarget=*/false, /*pointSprite=*/false,
                             /*poolDepth=*/true,
                             /*rtvFmt=*/b.rt[0]->dxgi,
                             /*dsvFmt=*/hasDs ? b.ds->dsvFormat
                                              : DXGI_FORMAT_UNKNOWN,
                             /*noDsv=*/!hasDs);
      // A null PSO here means the shader pair was uncached, NOT a format
      // problem — that is a different, already-counted refusal downstream.
      if (b.poolPso) m_poolFmtVariants.insert(
          ((uint64_t)b.rt[0]->dxgi << 32) |
          (uint32_t)(hasDs ? b.ds->dsvFormat : DXGI_FORMAT_UNKNOWN));
    }
  }
  // iter-449c: the landing census, split by producer. A batch with a poolPso
  // will be recorded onto the guest's bound surface; one without goes to the
  // legacy backbuffer path (and the front-blit then OVERWRITES the backbuffer
  // at present — i.e. that content is INVISIBLE whenever the blit is armed,
  // which is exactly why this split answers the missing-UI question).
  {
    const uint32_t t = d.srcType < 3u ? d.srcType : 0u;
    m_landByType[b.poolPso ? 0 : 1][t]++;
    // First 6 UP submits: the state a UI draw actually carries, so "lands but
    // offscreen" and "never lands" stop being indistinguishable.
    if (d.srcType == 2u) {
      static std::atomic<uint64_t> s_upLog{0};
      const uint64_t k = s_upLog.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 6)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_UPSTATE] #%llu prim=%u count=%u pool=%d rt0=%s(%ux%u) ds=%d "
            "stateValid=%d vte=%08X vp=(%.0f,%.0f %.0fx%.0f)",
            (unsigned long long)k, d.primType, d.srcCount,
            b.poolPso ? 1 : 0,
            b.rt[0] ? "yes" : "NULL",
            b.rt[0] ? b.rt[0]->width : 0, b.rt[0] ? b.rt[0]->height : 0,
            (b.ds && b.ds->res) ? 1 : 0, d.stateValid ? 1 : 0, d.vteCntl,
            b.viewport.TopLeftX, b.viewport.TopLeftY,
            b.viewport.Width, b.viewport.Height);
    }
  }
  // ── iter-450UA [DRAWID] RECORD SITE ───────────────────────────────────────
  // Everything the director asked for, in one greppable line per draw, emitted
  // where the batch, the viewport and the expanded vertices are all in scope.
  // SR_DRAWID=1 enables; SR_DRAWID_MAXV bounds the vertex count logged
  // (default 256 — "small UP draws" without hard-coding the UI's shape);
  // SR_DRAWID_ALL=1 removes that bound.
  {
    static const bool s_drawIdOn = [] {
      const char* e = std::getenv("SR_DRAWID");
      return e && e[0] == '1';
    }();
    static const uint32_t s_maxV = [] {
      const char* e = std::getenv("SR_DRAWID_MAXV");
      const long v = e ? std::atol(e) : 0;
      return (uint32_t)(v > 0 ? v : 256);
    }();
    static const bool s_all = [] {
      const char* e = std::getenv("SR_DRAWID_ALL");
      return e && e[0] == '1';
    }();
    // SR_DRAWID_RT=<hex guest surface key>: log only draws bound to that
    // render target. This is a LOG-VOLUME FILTER, not an identification — on a
    // live roll the 4096-line cap is otherwise consumed by boot-era draws long
    // before the title appears. Association is still established by
    // suppression, never by which surface a draw targets.
    static const uint32_t s_rtFilter = [] {
      const char* e = std::getenv("SR_DRAWID_RT");
      return (e && e[0]) ? (uint32_t)strtoul(e, nullptr, 16) : 0u;
    }();
    uint32_t rtKeyF = 0;
    if (s_drawIdOn && s_rtFilter && b.rt[0])
      for (const auto& kv : m_hleSurfaces)
        if (&kv.second == b.rt[0]) { rtKeyF = kv.first; break; }
    static std::atomic<uint32_t> s_logged{0};
    if (s_drawIdOn && (s_all || d.srcCount <= s_maxV) &&
        (!s_rtFilter || rtKeyF == s_rtFilter) &&
        s_logged.fetch_add(1, std::memory_order_relaxed) < 4096u) {
      // WHICH expanded attribute is POSITION0. The expansion writes VS input i
      // at float offset i*4, so the signature's index IS the attribute index.
      // Never assumed to be 0: at least one shader in this title declares
      // POSITION1 as signature element 0 (iter-450O), and guessing here would
      // silently report the wrong rectangle.
      uint32_t posAttr = 0; bool posNamed = false;
      if (vs) {
        for (uint32_t i = 0; i < nAttrs && i < vs->signature.size(); i++) {
          const auto& se = vs->signature[i];
          if (se.index == 0 && se.name.size() >= 8 &&
              se.name.compare(0, 8, "POSITION") == 0) {
            posAttr = i; posNamed = true; break;
          }
        }
      }
      // Bounding box of that attribute over the vertices we actually expanded.
      float mnx = 1e30f, mny = 1e30f, mnz = 1e30f;
      float mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
      uint32_t finite = 0;
      for (uint32_t v = 0; v < written; v++) {
        const float* p = out + (size_t)v * nf + (size_t)posAttr * 4u;
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
          continue;
        finite++;
        if (p[0] < mnx) mnx = p[0];  if (p[0] > mxx) mxx = p[0];
        if (p[1] < mny) mny = p[1];  if (p[1] > mxy) mxy = p[1];
        if (p[2] < mnz) mnz = p[2];  if (p[2] > mxz) mxz = p[2];
      }
      if (!finite) { mnx = mny = mnz = mxx = mxy = mxz = 0.0f; }
      // ⚠ SPACE HONESTY. `out` holds VS INPUTS, not post-VS positions. With
      // VTE off (the UI/2D path) the guest supplies PIXELS in the pass surface
      // and the shader epilogue only remaps them through c18 — so the box IS
      // the screen rectangle. With VTE on the input is model space and the box
      // says nothing about where the draw lands; it is labelled so and must
      // not be read as a screen rect.
      const bool pixelSpace = d.stateValid && !(d.vteCntl & 0x1u);
      // The bound RT's guest key, by reverse lookup (the pool is ~15 entries).
      uint32_t rtKey = 0;
      if (b.rt[0])
        for (const auto& kv : m_hleSurfaces)
          if (&kv.second == b.rt[0]) { rtKey = kv.first; break; }
      // Every sampler that has either a bound key or a resolved descriptor.
      char sl[420]; size_t so = 0; uint32_t nSlots = 0;
      for (uint32_t s = 0; s < 16u && so + 40 < sizeof(sl); s++) {
        if (!d.texKey[s] && !d.texSlot[s] && !d.texSlot3D[s]) continue;
        nSlots++;
        so += (size_t)snprintf(sl + so, sizeof(sl) - so, " s%u:%08X->d%u",
                               s, d.texKey[s], d.texSlot[s]);
        if (d.texSlot3D[s])
          so += (size_t)snprintf(sl + so, sizeof(sl) - so, "/3d%u",
                                 (unsigned)d.texSlot3D[s]);
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[DRAWID] #%llu src=%s prim=%u vin=%u vout=%u vs=%016llX ps=%016llX "
          "vte=%08X sv=%d rt=%08X(%ux%u) pool=%d vp=(%.0f,%.0f %.0fx%.0f) "
          "a%u%s=[%.1f,%.1f,%.1f]..[%.1f,%.1f,%.1f] space=%s slots=%u:%s",
          (unsigned long long)drawOrd,
          d.srcType == 2u ? "UP" : (d.srcType == 1u ? "IDX" : "DV"),
          d.primType, d.srcCount, written,
          (unsigned long long)d.vsHash, (unsigned long long)d.psHash,
          d.vteCntl, d.stateValid ? 1 : 0, rtKey,
          b.rt[0] ? b.rt[0]->width : 0u, b.rt[0] ? b.rt[0]->height : 0u,
          b.poolPso ? 1 : 0,
          b.viewport.TopLeftX, b.viewport.TopLeftY,
          b.viewport.Width, b.viewport.Height,
          posAttr, posNamed ? "(POS0)" : "(?)",
          mnx, mny, mnz, mxx, mxy, mxz,
          pixelSpace ? "px" : (d.stateValid ? "model" : "nostate"),
          nSlots, nSlots ? sl : " -");
    }
  }
  m_drawRtBindsAtLastSubmit = m_surfRtBinds;   // iter-446 S1 tiebreaker
  // iter-455l r3 (ruling ad4621a defect 1): precompute the provenance payload at
  // SUBMIT — the single post-success chokepoint inside recordHleBatchToPool
  // feeds from it later (held-list re-records run PRESENTS later, when d's CPU
  // constant blocks are gone). The effective render state rides d's register
  // shadow (defect 7).
#if SR_ORACLE
  if (sr::gfx::prov::enabled())
    b.provFeed = sr::gfx::prov::makePoolDrawFeed(
        drawOrd, d.vsHash, d.psHash, d.vsConst, d.psConst, d.texKey,
        d.depthControl, d.blendControl, d.colorMask, d.stateValid);
#endif
  // iter-458 H-VCAP submission witness (Astra 051: recovered draws must reach
  // actual submission): non-indexed DrawVertices whose snapshot exceeds the
  // legacy 4 KB producer cap, counted HERE, past every refusal.
  if (d.srcType == 0u && uint64_t(d.srcStride) * d.srcCount > 4096u) {
    static std::atomic<uint64_t> s_bigDvSubmit{0}, s_bigDvVerts{0};
    const uint64_t n = s_bigDvSubmit.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t vtx = s_bigDvVerts.fetch_add(d.srcCount, std::memory_order_relaxed) + d.srcCount;
    if (n <= 4096)  // every event: the running total's LAST line is the exact count
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_VCAP_SUBMIT] DrawVertices >4KB SUBMITTED=%llu verts=%llu (last vs=%016llX "
          "count=%u stride=%u)",
          (unsigned long long)n, (unsigned long long)vtx, (unsigned long long)d.vsHash,
          d.srcCount, d.srcStride);
  }
  m_hleBatches.push_back(b);
  // ── iter-446 S2: RECORD AT STREAM POSITION ────────────────────────────────
  // The batch is still staged (it is the source for the held list, and the
  // retirement path owns it), but it is ALSO recorded here, in the guest own
  // command order, so a Resolve that follows this draw resolves what this draw
  // wrote. The gate condition is copied verbatim from hleClearBound: writing
  // the shared frame list needs frameListEnabled(), which folds in
  // ownPresentEnabled(), or the MAIN thread owns the list.
  if (drawInOrderEnabled() && frameListEnabled() &&
      m_clState == ClState::Recording && hleFrameStateReady()) {
    flushHleFrameState();
    ID3D12PipelineState* pso = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    const bool pooled = recordHleBatchToPool(b, pso, topo);
    if (pooled) rebindFrameTarget();
    recordHleBatch(b, pso, topo);
    m_hleInOrderRecorded++;
    m_hleDrawn++;          // counted HERE, at the DrawInstanced site
    m_hleFrameRecordedInOrder = true;
    if (psHit) {
      static std::atomic<uint32_t> s_pdOk{0};
      const uint32_t n = s_pdOk.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 24)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[PSDRAW] #%u SUBMITTED verts=%u pooled=%d poolPso=%d rt0=%s "
            "rt0res=%d rt0dim=%ux%u",
            n, outCount, (int)pooled, b.poolPso ? 1 : 0,
            b.rt[0] ? "set" : "NULL", (b.rt[0] && b.rt[0]->res) ? 1 : 0,
            b.rt[0] ? b.rt[0]->width : 0, b.rt[0] ? b.rt[0]->height : 0);
    }
  }
  m_hleVBUsed = vbOffset + uint64_t(written) * stride;
  m_hleCBUsed = cb;

  // ── iter-444 S6j: THE ATTRIBUTE PROBE ─────────────────────────────────────
  // Geometry renders depth-sorted and textured but grossly MAGNIFIED, so the
  // suspect is attribute decode SCALE, not the matrix. The decisive tell is the
  // decoded POSITION range: a normalised 16-bit attribute read as an INTEGER
  // gives +-32767 instead of +-1, and iter-427g was bitten by exactly this
  // class via placeholder sidecar values on container-hashed shaders.
  // Logs the declared format AND what it actually produced, per distinct VS.
  if (d.primType != 13u) {
    static std::mutex s_pm;
    static std::set<uint64_t> s_seen;
    bool first = false;
    { std::lock_guard<std::mutex> lk(s_pm);
      if (s_seen.size() < 24) first = s_seen.insert(d.vsHash).second; }
    if (first) {
      float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
      for (uint32_t v = 0; v < written; v++) {
        const float* p = out + size_t(v) * nf;
        for (int k = 0; k < 4; k++) {
          if (p[k] < lo[k]) lo[k] = p[k];
          if (p[k] > hi[k]) hi[k] = p[k];
        }
      }
      char eb[240]; int eo = 0;
      for (uint32_t i = 0; i < nAttrs && eo < 190; i++)
        eo += snprintf(eb + eo, sizeof(eb) - eo, " [%u]off=%d f=%u u=%u s=%d n=%d",
                       i, lay[i].offsetDwords * 4, lay[i].format, lay[i].usage,
                       lay[i].isSigned ? 1 : 0, lay[i].isInteger ? 1 : 0);
      // The RANGE alone cannot tell "all garbage" from "one garbage vertex" —
      // print the first few decoded positions too.
      char vb2[200]; int vo = 0;
      for (uint32_t v = 0; v < written && v < 5 && vo < 160; v++) {
        const float* p = out + size_t(v) * nf;
        vo += snprintf(vb2 + vo, sizeof(vb2) - vo, " (%.1f,%.1f,%.1f)",
                       p[0], p[1], p[2]);
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_ATTR] vs=%016llX prim=%u attrs=%u srcStride=%u cnt=%u |%s | "
          "range x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f] | v0..4:%s",
          (unsigned long long)d.vsHash, d.primType, nAttrs, d.srcStride,
          d.srcCount, eb, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], vb2);
    }
  }
  const uint64_t serial = ++m_hleSerial;
  if (serial <= 4) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_EXPAND] #%llu prim=%u attrs=%u stride=%u srcStride=%u declN=%u | "
        "e0={off%d fmt%u use%u} | src=%u -> out=%u | v0=(%.3f,%.3f,%.3f,%.3f)"
        " | const=%d",
        (unsigned long long)serial, d.primType, nAttrs, stride, d.srcStride,
        d.declCount, lay[0].offsetDwords * 4, lay[0].format, lay[0].usage,
        d.srcCount, written, out[0], out[1], out[2], out[3],
        m_hleHaveConst ? 1 : 0);
  }
  return true;
}

// ── iter-444 S6f: record the whole frame's draw list ────────────────────────
// Called from inside presentFramebuffer, where the frame command list is OPEN
// and the back buffer is already in RENDER_TARGET state.
// iter-446 S1: the census that proves the per-draw capture is per-DRAW.
// ⚠ Built from m_hleBatches, NEVER from m_hleBoundRT — reading the mirror here
// is the exact bug this must be able to detect, and a census that shares the
// bug it is testing for cannot fail. If every batch in a frame reports the same
// target, the capture is frame-global and S1 did nothing.
void D3D12Backend::drawRtCensus() {
  if (m_hleBatches.empty()) return;
  // ⚠ FIRST ATTEMPT AT THIS COULD NOT FAIL. It claimed ">1 distinct RT across
  // the frame's batches" as the pass criterion — but the fixture stages exactly
  // ONE batch per frame (movie quad only; general geometry is still refused),
  // so distinctRt is 1 BY CONSTRUCTION and proves nothing either way.
  //
  // THE DISCRIMINATOR THAT WORKS AT ONE BATCH: compare each captured target
  // against what the mirror holds RIGHT NOW. This runs at Present, so the
  // mirror holds the guest's LAST bind of the frame. If the capture were
  // frame-global — the bug — captured and current would be identical for every
  // batch, always. Any difference PROVES the value was latched at submit.
  const HleSurface* first = m_hleBatches[0].rt[0];
  uint32_t distinctRt = 1, nullRt = 0, mrt = 0, vsMirror = 0;
  for (const auto& b : m_hleBatches) {
    if (b.rt[0] != first) distinctRt = 2;   // ">1" is the only claim needed
    if (!b.rt[0]) nullRt++;
    if (b.rt[1]) mrt++;
    if (b.rt[0] != m_hleBoundRT[0] || b.ds != m_hleBoundDS) vsMirror++;
  }
  m_drawRtVsMirror += vsMirror;
  // ⛔ S5 SIZING: what formats would a pool-targeted draw actually need? The PSO
  // BAKES RTVFormats[0] and DSVFormat (:1306/:1309, :3905/:3909), so a draw can
  // only be re-pointed at a pooled surface whose formats match the PSO it was
  // built with. Our pool depth is D32_FLOAT_S8X24_UINT while every PSO bakes
  // D32_FLOAT — so even a colour match fails on depth. This reports the exact
  // (rtv,dsv) pairs the staged draws would demand, which is the number that
  // sizes the PSO-variant work rather than guessing at it.
  for (const auto& b : m_hleBatches) {
    const uint32_t rf = b.rt[0] ? (uint32_t)b.rt[0]->dxgi : 0u;
    const uint32_t df = b.ds ? (uint32_t)b.ds->dsvFormat : 0u;
    const uint64_t pair = ((uint64_t)rf << 32) | df;
    bool seen = false;
    for (uint32_t i = 0; i < m_drawFmtN; i++)
      if (m_drawFmtPairs[i] == pair) { seen = true; break; }
    if (!seen && m_drawFmtN < 16u) m_drawFmtPairs[m_drawFmtN++] = pair;
  }
  // ⚠ AND CAN THIS TRACE DISCRIMINATE AT ALL? `differsFromMirror` reading 0 is
  // consistent with BOTH "the capture is per-draw and the value happens to
  // match" and "the capture is frame-global". The tiebreaker is whether any RT
  // BIND happened after the last draw submitted: if none did, captured ==
  // mirror NECESSARILY and the test is inconclusive BY CONSTRUCTION rather than
  // passing. Reported so the distinction is never silently collapsed.
  const uint64_t bindsAfter = m_surfRtBinds - m_drawRtBindsAtLastSubmit;
  if (bindsAfter) m_drawRtDiscriminable++;
  m_drawRtFrames++;
  if (distinctRt > 1) m_drawRtMultiFrames++;
  m_drawRtNullTarget += nullRt;
  m_drawRtMrt += mrt;
  if (m_drawRtFrames <= 4 || (m_drawRtFrames % 600) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[S6_DRAWRT] frame=%llu batches=%zu distinctRt=%s nullRt=%u mrt=%u "
        "differsFromMirror=%u | totals: frames=%llu multiTargetFrames=%llu "
        "nullTarget=%llu mrt=%llu perDrawProof=%llu bindsAfterLastDraw=%llu "
        "discriminableFrames=%llu distinctFmtPairs=%u first=%08X/%08X",
        (unsigned long long)m_drawRtFrames, m_hleBatches.size(),
        distinctRt > 1 ? ">1" : "1", nullRt, mrt, vsMirror,
        (unsigned long long)m_drawRtFrames,
        (unsigned long long)m_drawRtMultiFrames,
        (unsigned long long)m_drawRtNullTarget,
        (unsigned long long)m_drawRtMrt,
        (unsigned long long)m_drawRtVsMirror,
        (unsigned long long)bindsAfter,
        (unsigned long long)m_drawRtDiscriminable, m_drawFmtN,
        m_drawFmtN ? (uint32_t)(m_drawFmtPairs[0] >> 32) : 0u,
        m_drawFmtN ? (uint32_t)(m_drawFmtPairs[0] & 0xFFFFFFFFu) : 0u);
}

// ── iter-446 S2: the frame-global head, extracted ──────────────────────────
// It used to live inline at the top of renderHleDraws, which is fine while every
// draw records at Present and WRONG the moment a draw records at stream
// position: the head must be in the list BEFORE the first such draw.
bool D3D12Backend::hleFrameStateReady() const {
  // iter-445 C1 FAULT-FREE: m_dsvHeap is created only inside the geo-pipeline
  // block, so it is null whenever createGuestGeoPipeline() failed. The harness
  // VEH makes a host AV FATAL before any SEH filter runs - check, never fault.
  return m_xlatRootSig && m_xlatSrvHeap && m_xlatSamplerHeap && m_rtvHeap &&
         m_dsvHeap && m_hleVB;
}

// Re-issue ONLY the target binding and scissor. Used to put the frame back on
// the back buffer after a pool draw stole both.
void D3D12Backend::rebindFrameTarget() {
  if (!hleFrameStateReady()) return;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += m_frameIndex * m_rtvDescriptorSize;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
  m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  D3D12_RECT sc{0, 0, LONG(m_width), LONG(m_height)};
  m_commandList->RSSetScissorRects(1, &sc);
}

void D3D12Backend::flushHleFrameState() {
  if (m_hleFrameStateSet) return;      // idempotent within a frame
  if (!hleFrameStateReady()) return;
  m_hleFrameStateSet = true;
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto srvGpu = m_xlatSrvHeap->GetGPUDescriptorHandleForHeapStart();
  ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
  m_commandList->SetDescriptorHeaps(2, heaps);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += m_frameIndex * m_rtvDescriptorSize;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
  m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  // iter-444 S6h: CLEAR THE DEPTH BUFFER - an uncleared buffer keeps the
  // PREVIOUS frame depth, so frame N+1 fails the test almost everywhere and
  // geometry progressively disappears, a symptom that reads as "draws stopped".
  //
  // THIS IS WHY THE DEPTH CLEAR DOES NOT MOVE TO beginFrame WITH THE COLOUR
  // CLEAR. m_hleDepthClear is written at SUBMIT time from the guest own
  // RB_DEPTHCONTROL zfunc; at beginFrame no draw has submitted yet, so it would
  // clear using frame N-1 compare function - reintroducing the exact
  // disappearing-geometry symptom this clear exists to prevent. Issuing it here,
  // from the first flush of the frame, uses the value the first submitting draw
  // just wrote. depthClearZfuncLate counts frames where a LATER draw implied a
  // different value, i.e. where even this is not enough.
  m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH,
                                       m_hleDepthClear, 0, 0, nullptr);
  if (m_hleDepthClearSeen && m_hleDepthClearLast != m_hleDepthClear)
    m_depthClearZfuncLate++;
  m_hleDepthClearSeen = true;
  m_hleDepthClearLast = m_hleDepthClear;
  D3D12_RECT sc{0, 0, LONG(m_width), LONG(m_height)};
  m_commandList->RSSetScissorRects(1, &sc);
  m_commandList->SetGraphicsRootSignature(m_xlatRootSig.Get());
  xlatSetPixelPosScale();  // iter-464 resscale (root param 14)
  m_commandList->SetGraphicsRootDescriptorTable(5, srvGpu);
  m_commandList->SetGraphicsRootDescriptorTable(
      6, {srvGpu.ptr + UINT64(kTex2DSlots) * inc});
  m_commandList->SetGraphicsRootDescriptorTable(
      7, {srvGpu.ptr + UINT64(kTex2DSlots + 8) * inc});
  m_commandList->SetGraphicsRootDescriptorTable(
      8, xlatSamplerTable());
  m_commandList->SetGraphicsRootDescriptorTable(11, srvGpu);
  m_commandList->SetGraphicsRootDescriptorTable(12, srvGpu);
  m_commandList->SetGraphicsRootDescriptorTable(13, srvGpu);
}

// ── iter-446 S5: draw the batch into the surface the GUEST bound ────────────
// Returns true if it drew. The frame binding is restored by the caller before
// the legacy draw, so this never changes what reaches the screen.
//
// WHY BOTH DRAWS, rather than moving the target outright: D3DDeviceSwap names
// its front buffer (measured 82998720 / 839F5CF0, alternating) and NEITHER IS
// A POOLED SURFACE - the front buffer is never SetRenderTarget'd. So there is
// no route yet from a pooled composite to the screen, and re-pointing alone
// would render the movie into a surface nothing presents. The pool draw is the
// CORRECT one; the legacy draw is the path that still reaches the display until
// a composite route exists. Both are the guest's own draw, issued twice - a
// transitional duplication, gated and counted, not a second invented draw.
bool D3D12Backend::recordHleBatchToPool(const HleBatch& b,
                                        ID3D12PipelineState*& curPso,
                                        D3D12_PRIMITIVE_TOPOLOGY& curTopo) {
  // iter-449b: a depth-less batch (b.ds null, PSO built with DSVFormat=UNKNOWN)
  // binds a null DSV — that pairing is what makes it D3D12-legal.
  if (!b.poolPso || !b.rt[0] || !b.rt[0]->res) return false;
  const bool hasDs = b.ds && b.ds->res;
  if (!m_hleRtvHeap || (hasDs && !m_hleDsvHeap)) return false;
  hleTransition(b.rt[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv =
      m_hleRtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += (SIZE_T)b.rt[0]->descIndex * m_hleRtvSize;
  if (hasDs) {
    hleTransition(b.ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_hleDsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += (SIZE_T)b.ds->descIndex * m_hleDsvSize;
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  } else {
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  }
  // The scissor must be clamped to THIS surface, not the back buffer: the pool
  // targets run down to 4x4 and the frame-global scissor is 1280x720.
  D3D12_RECT sc{0, 0, LONG(b.rt[0]->width), LONG(b.rt[0]->height)};
  m_commandList->RSSetScissorRects(1, &sc);
  // Force the PSO/topology to be re-issued for the pool pipeline, and again for
  // the legacy one afterwards.
  curPso = nullptr;
  curTopo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  HleBatch pb = b;
  pb.pso = b.poolPso;
  recordHleBatch(pb, curPso, curTopo);
  hleNotePoolDraw(b.rt[0]);   // the setter that had no call site until now
  m_poolDraws++;
  curPso = nullptr;
  curTopo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  // iter-455l r3 (ruling ad4621a defect 1): THE single post-success provenance
  // chokepoint. Both live callers — the in-order submit path and the
  // renderHleDraws fallback/held-list loop — pass through this function, so
  // every draw that actually reached the pool feeds the ring here and nothing
  // else does. A held batch re-recorded on a later present feeds again: that
  // re-record IS another pool draw on the surface (tested, no dedupe).
#if SR_ORACLE
  if (sr::gfx::prov::enabled()) {
    const float vp[4] = {b.viewport.TopLeftX, b.viewport.TopLeftY,
                         b.viewport.Width, b.viewport.Height};
    const uint32_t scv[4] = {0u, 0u, b.rt[0]->width, b.rt[0]->height};
    sr::gfx::prov::feedPoolDraw(sr::gfx::prov::ring(), true, b.rt[0], 0u,
                                b.provFeed, vp, scv, (uint32_t)b.topology,
                                b.vertexCount, b.poolPso);
  }
#endif
  return true;
}

void D3D12Backend::recordHleBatch(const HleBatch& b,
                                  ID3D12PipelineState*& curPso,
                                  D3D12_PRIMITIVE_TOPOLOGY& curTopo) {
  const D3D12_GPU_VIRTUAL_ADDRESS vbBase = m_hleVB->GetGPUVirtualAddress();
  if (b.pso != curPso) { curPso = b.pso; m_commandList->SetPipelineState(b.pso); }
  // S6h: PER-DRAW - the guest viewport regs differ between passes, and a
  // frame-global viewport put every sub-pass draw in the wrong place.
  m_commandList->RSSetViewports(1, &b.viewport);
  m_commandList->SetGraphicsRootConstantBufferView(0, b.vsCbv);
  m_commandList->SetGraphicsRootConstantBufferView(1, b.psCbv);
  m_commandList->SetGraphicsRootConstantBufferView(2, b.sharedCbv);
  m_commandList->SetGraphicsRootConstantBufferView(3, b.vsCbv);
  m_commandList->SetGraphicsRootConstantBufferView(4, b.psCbv);
  D3D12_VERTEX_BUFFER_VIEW vbv{};
  vbv.BufferLocation = vbBase + b.vbOffset;
  vbv.SizeInBytes = b.vertexCount * b.stride;
  vbv.StrideInBytes = b.stride;
  m_commandList->IASetVertexBuffers(0, 1, &vbv);
  if (b.topology != curTopo) { curTopo = b.topology; m_commandList->IASetPrimitiveTopology(b.topology); }
  m_commandList->DrawInstanced(b.vertexCount, 1, 0, 0);
}

bool D3D12Backend::renderHleDraws() {
  drawRtCensus();
  // S2 RETIREMENT CONTRACT. When the frame already recorded its draws at stream
  // position there is nothing to re-issue - but this MUST still return true.
  // Returning false makes hleRetireFrame count every staged draw as DROPPED and
  // skip the m_hleHalf flip; with kFrameCount=2 and one frame in flight, frame
  // N+1 would then stage into the very bytes frame N submitted command list is
  // still reading.
  if (m_hleFrameRecordedInOrder) {
    m_hleInOrderFrames++;
    // MEASURED REGRESSION, caught by [PRESENT_SIG] on this path first use:
    // returning here also skipped `m_hleQuadAge = 0`, which only happens on the
    // `fresh` branch below. The age therefore never reset on a frame that DID
    // draw, reached kHold after four presents, cleared the held list and
    // returned false forever - so every present past the fourth showed an EMPTY
    // frame (distinct=1). A frame that recorded in-order is the freshest frame
    // there is; say so.
    m_hleQuadAge = 0;
    return true;
  }
  // THE MEASURED 2:1 HAZARD: the movie advances at ~half the present rate (8
  // presents carried 4 movie frames), so most presents have no fresh draw. The
  // swapchain is FLIP_DISCARD, which does NOT preserve the previous backbuffer,
  // whereas the console's front buffer persists until the guest overwrites it.
  // Re-issuing the guest's OWN last list against the same resources emulates
  // that persistence: no guest function is called, no guest state written, and
  // the present count still equals the guest's swap count. The cap stops a
  // finished movie freezing on screen forever.
  // iter-449p: SR_HLE_QUAD_HOLD=0 now means HOLD DISABLED (it used to fall
  // back to 4, so the mechanism could never be A/B'd off). Rationale for the
  // A/B: the hold emulated console front-buffer persistence before the front
  // chain existed; with SR_HLE_PRESENT_FRONT baked, the resolved front
  // texture persists natively — and the hold's stale-frame re-issues (held
  // draws ran 350-930K/roll ≈ every 5th present replaying a 1-4-frame-old
  // list) are a TEMPORAL-ALIASING engine for the FE/scene eras: user-visible
  // as "everything flickers and morphs".
  // iter-449p BAKE: default 0 (hold RETIRED). The A/B roll proved the front
  // chain carries the persistence natively (holdoff1: whole intro intact,
  // held=0, movie gate exact) — set SR_HLE_QUAD_HOLD=4 to resurrect the old
  // behavior for comparison.
  static const uint32_t kHold = [] {
    const char* e = std::getenv("SR_HLE_QUAD_HOLD");
    const int v = e ? std::atoi(e) : 0;
    return (uint32_t)(v >= 0 ? v : 0);
  }();
  const bool fresh = !m_hleBatches.empty();
  if (!fresh && kHold == 0) { m_hleHeldList.clear(); return false; }
  std::vector<HleBatch>* list;
  if (fresh) {
    list = &m_hleBatches;
    m_hleQuadAge = 0;
  } else {
    if (m_hleHeldList.empty()) return false;
    if (m_hleQuadAge > kHold) { m_hleHeldList.clear(); return false; }
    list = &m_hleHeldList;
  }
  m_hleQuadAge++;
  // iter-445 C1 FAULT-FREE FIX: this guard covered the xlat heaps but NOT
  // m_rtvHeap/m_dsvHeap, which are dereferenced ~10 lines below — and
  // m_dsvHeap is created only inside the geo-pipeline block, so it is null
  // whenever createGuestGeoPipeline() failed. Other users of it null-check
  // (see the m_dsvHeap tests further down this file); this path did not. The
  // harness VEH makes a host AV FATAL before any SEH filter runs, so this must
  // be a check, never a fault-and-catch.
  if (!hleFrameStateReady()) return false;
  recordGuard(Probe);
  flushHleFrameState();

  ID3D12PipelineState* curPso = nullptr;
  D3D12_PRIMITIVE_TOPOLOGY curTopo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  for (const HleBatch& b : *list) {
    if (recordHleBatchToPool(b, curPso, curTopo)) rebindFrameTarget();
    recordHleBatch(b, curPso, curTopo);
  }

  const size_t n = list->size();
  if (fresh) m_hleDrawn += n; else m_hleHeld += n;
  if (m_hleDrawn + m_hleHeld <= 64 || (m_hleDrawn + m_hleHeld) % 4000 < n) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[S4_DRAW] list=%zu %s age=%u vp=%ux%u | drawn=%llu held=%llu | "
        "ringMB vb=%llu cb=%llu half=%u | full: vb=%llu cb=%llu list=%llu",
        n, fresh ? "FRESH" : "held", m_hleQuadAge - 1, m_width, m_height,
        (unsigned long long)m_hleDrawn, (unsigned long long)m_hleHeld,
        (unsigned long long)((m_hleVBUsed % (kHleVBBytes / 2)) >> 20),
        (unsigned long long)((m_hleCBUsed % (kHleCBBytes / 2)) >> 20),
        m_hleHalf, (unsigned long long)m_hleVbFull,
        (unsigned long long)m_hleCbFull, (unsigned long long)m_hleListFull);
  }

  return n != 0;
}

// Retire the staged frame. Called from an RAII guard so it runs EXACTLY ONCE
// per present on every exit path, not only the one that records.
void D3D12Backend::hleRetireFrame(bool recorded) {
  if (m_hleBatches.empty()) return;
  if (recorded) {
    // The recorded list becomes the HELD list, and the next frame stages into
    // the OTHER half so the GPU keeps reading these bytes safely while it
    // executes this command list.
    m_hleHeldList.swap(m_hleBatches);
    // ⚠ FLIP ONLY ON A RECORDED FRAME. The held list is re-recorded for up to
    // kHold presents and its batches point into the half they were staged in.
    // An unconditional flip (tempting, for symmetry) would put the next fresh
    // staging into exactly those bytes two presents later. This is safe for a
    // non-obvious reason: a frame that recorded nothing also STAGED nothing
    // past this point, so the held bytes cannot have moved.
    m_hleHalf ^= 1u;
  } else {
    // NO SILENT DROPS: a present that took an early return or the upload
    // branch discards work the instrument already counted as staged.
    m_hleDropped += m_hleBatches.size();
    static uint64_t s_warned = 0;
    if (++s_warned <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S4_DRAW] DROPPED %zu staged draws — this present never reached "
          "renderHleDraws (early return or upload branch); total dropped=%llu",
          m_hleBatches.size(), (unsigned long long)m_hleDropped);
  }
  m_hleBatches.clear();
  m_hleVBUsed = uint64_t(m_hleHalf) * (kHleVBBytes / 2);
  m_hleCBUsed = uint64_t(m_hleHalf) * (kHleCBBytes / 2);
}
#endif

// ── iter-464 input lane: DEVICE-MATCHED BUTTON PROMPTS ─────────────────────────
// glyphNote runs where a 2D texture's bytes were just (re)uploaded: a candidate size
// and format is hashed and matched against data/glyphs/glyph_map.txt. glyphRemap runs
// at every 2D bind: one vector read unless the slot holds a prompt icon, then the
// active glyph set's replacement slot (built on first use) is bound instead. The
// game's draw is otherwise untouched: same geometry, blend and sampler.
void D3D12Backend::glyphNote(uint32_t slot, uint32_t basePage, uint32_t fmt, uint32_t w,
                             uint32_t h, const uint8_t* src, size_t avail, uint32_t pitchTexels,
                             bool tiled, uint32_t blockW, uint32_t blockBytes) {
  if (!m_glyphOn || slot >= m_glyphIcon.size()) return;
  uint16_t icon = 0;
  if (glyphs::candidate(fmt, w, h)) {
    // iter-465 polish: the identities (data/glyphs/glyph_map.txt) hash the icon's LINEAR
    // payload as ui.AST stores it (DXT5 blocks, 8in16-swapped, row-major). The live texture
    // the Apt renderer creates is TILED (r5 roll: [TEX_CACHE] fmt=20 32x32 pitch=128 tiled=1,
    // base 0x1E430000): its first 1024 bytes are scattered blocks of a 32x32-block macro tile,
    // so hashing them never matched and no prompt icon was ever identified live (0 in every
    // run). De-tile the base level (Xenia's tiled layout, gfx/guest_image.hpp, no endian swap:
    // the guest bytes ARE the payload bytes) and hash that.
    uint64_t hash = 0;
    bool readable = true;
    if (tiled) {
      const uint32_t span = glyphs::tiledSliceBytes(w, h, pitchTexels, blockW, blockBytes);
      readable = span && gpuPhysToHost(basePage * 4096u + span - 1u);
      if (readable) icon = glyphs::identifyTiled(fmt, w, h, src, span, pitchTexels, blockW, blockBytes, &hash);
    } else {
      icon = glyphs::identify(fmt, w, h, src, avail);
    }
    static std::atomic<uint32_t> s_candLog{0};
    const uint32_t cn = s_candLog.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cn <= 48 || (!icon && cn % 100 == 0))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[GLYPH] candidate #%u tex@0x%08X fmt=%u %ux%u pitch=%u tiled=%d%s detiled-fnv=%016llX -> %s",
                  cn, basePage * 4096u, fmt, w, h, pitchTexels, tiled ? 1 : 0,
                  readable ? "" : " (slice not readable)", (unsigned long long)hash,
                  icon ? glyphs::iconName(icon) : "not a prompt icon");
  }
  m_glyphIcon[slot] = icon;
  m_glyphBasePage[slot] = basePage;
  if (icon) {
    static std::atomic<uint32_t> s_idLog{0};
    const uint32_t n = s_idLog.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 64 || n % 200 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[GLYPH] prompt icon %s identified: tex@0x%08X fmt=%u %ux%u slot %u #%u",
                  glyphs::iconName(icon), basePage * 4096u, fmt, w, h, slot, n);
  }
}

uint32_t D3D12Backend::glyphRemap(uint32_t slot, const uint32_t* f) {
  if (!m_glyphOn || slot == 0u || slot >= m_glyphIcon.size()) return slot;
  const uint16_t icon = m_glyphIcon[slot];
  if (!icon || m_glyphBasePage[slot] != ((f[1] >> 12) & 0xFFFFFu)) return slot;
  namespace HI = sr::kernel::hostinput;
  if (!HI::glyphsEnabled()) return slot;
  const uint8_t set = uint8_t(HI::activeGlyphSet());
  // Keyboard prompts have variants (the bound input differs by context / rebind): the key
  // carries the variant signature, so each variant is uploaded once and then reused.
  const uint64_t var = glyphs::variant(icon, set);
  const uint64_t rkey = ((uint64_t(icon) << 8) | set) ^ (var * 0x9E3779B97F4A7C15ull);
  auto it = m_glyphRepl.find(rkey);
  if (it != m_glyphRepl.end()) return it->second ? it->second : slot;
  uint32_t rs = 0;
  const glyphs::Image* img = glyphs::replacement(icon, set);
  if (img && !img->levels.empty() && m_glyphNextSlot < m_glyphSlotEnd) {
    const uint32_t mips = uint32_t(img->levels.size());
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = img->size; td.Height = img->size;
    td.DepthOrArraySize = 1; td.MipLevels = UINT16(mips);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    bool ok = SUCCEEDED(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    for (uint32_t m = 0; ok && m < mips; ++m) {
      const uint32_t n = std::max<uint32_t>(1u, img->size >> m);
      ok = uploadTexture2D(tex.Get(), img->levels[m].data(), n, n * 4u, n, n,
                           DXGI_FORMAT_R8G8B8A8_UNORM, m > 0u, 1u, m);
    }
    if (ok) {
      rs = m_glyphNextSlot++;
      wchar_t nm[64];
      swprintf(nm, 64, L"GLYPH %S/%S", glyphs::iconName(icon),
               HI::glyphSetName(HI::GlyphSet(set)));
      tex->SetName(nm);
      D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
      sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      // Identity: the replacement is true RGBA, which is what the guest fetch swizzle
      // produces from the original (straight-alpha DXT5, docs/INPUT_GLYPHS.md).
      sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sv.Texture2D.MipLevels = mips;
      const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      D3D12_CPU_DESCRIPTOR_HANDLE hd{m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                     SIZE_T(rs) * inc};
      publishXlatSrv(tex.Get(), &sv, hd);
      m_glyphRes.push_back(tex);
    }
  }
  m_glyphRepl[rkey] = rs;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[GLYPH] %s under %s (variant %016llX) -> %s",
              glyphs::iconName(icon), HI::glyphSetName(HI::GlyphSet(set)),
              (unsigned long long)var,
              rs ? "replacement bound" : img ? "KEPT (no slot or upload failed)" : "game's own icon");
  return rs ? rs : slot;
}

uint32_t D3D12Backend::getOrCreateTexture2DLookup(const uint32_t* f) {
  // iter-416/419: also silent. NOTE the field is the fetch-constant TYPE, not a
  // dimension - Xenia xenos.h:1147 FetchConstantType { kInvalidTexture=0,
  // kInvalidVertex=1, kTexture=2, kVertex=3 }. So `!= 2` means "this slot does
  // not currently hold a TEXTURE constant", and the overwhelmingly common reason
  // is that it holds a VERTEX one: xe_gpu_fetch_group_t (xenos.h:1333) is a
  // UNION in which texture fetch slot N occupies the very same six dwords as
  // vertex fetch slots 3N/3N+1/3N+2. A slot reading kVertex is therefore normal
  // hardware behaviour, not corruption.
  if ((f[0] & 3u) != 2u) {
    if (f[0] || f[1]) {  // an entirely empty fetch slot is normal, not a finding
      static std::atomic<uint32_t> s_dimLog{0};
      if (s_dimLog.fetch_add(1, std::memory_order_relaxed) < 12)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[TEX_REJECT] slot holds %s (type=%u), not a texture -> WHITE: "
                    "f0=%08X f1=%08X f2=%08X",
                    xeFetchTypeName(f[0] & 3u), f[0] & 3u, f[0], f[1], f[2]);
    }
    return 0;
  }
#if SR_ORACLE
  m_texTrack.armed = false;  // iter-466 cp lane: armed below for this texture only
  m_texTrackLast = 0;
#endif
  m_texBindFast = false;  // iter-467 native-b SR_NB_BIND_MEMO
  uint64_t key = ShaderCache::hashUcode(f, 24);
  const bool rtc = pm4RtCacheEnabled();  // iter-467 native-b: read once (per-slot hot path)
  // iter-464 resscale: a texture a scaled resolve wrote is a DIFFERENT cache entry
  // (Xenia TextureKey bit scaled_resolve), scale x the guest size, loaded from the
  // scaled resolve memory. Only on the PM4 GPU-loader path; 1x: no cost.
  uint32_t tsx = 1, tsy = 1;
  if (resscale::get().scaled() && m_texInline && rtc && pm4ShmEnabled() &&
      texResolutionScaled(f)) {
    tsx = resscale::get().x;
    tsy = resscale::get().y;
    key ^= 0x5265735363616C65ull;  // "ResScale"
  }
  // iter-460 perf: the clean set carries the slot - a title-city frame binds
  // ~25,000 2D textures, and this one lookup is the whole cost of a texture
  // nothing wrote since it was validated (the P1 check below, hoisted).
  if (rtc) {
    auto cit = m_texClean.find(key);
    if (cit != m_texClean.end() && cit->second) {
      m_texBindFast = true;
#if SR_ORACLE
      if (memres::texIdOn()) texIdCleanHit(key, f);
#endif
      return cit->second;
    }
  }
#if SR_ORACLE
  // A resolve already produced a bindable texture at this base? Bind it
  // directly — the scene color/depth resolves read back in EDRAM formats the
  // memory decoder returns white for. iter-438d: also the LIVE path (the
  // in-stream snapshot recorded by recordPendingResolve) — zero CPU cost.
  if (rttEnabled() || resolveTexSnapEnabled()) {
    const uint32_t basePage = (f[1] >> 12) & 0xFFFFFu;
    int rslot = findResolveTexSlot(basePage * 4096u);
    if (rslot >= 0) return uint32_t(rslot);
  }
#endif  // SR_ORACLE
  auto it = m_texCache.find(key);
#if SR_ORACLE
  // step 4 G7 (lane D, NOTES 2026-10-20 18:2x): SR_TEX_KEY_LOG=1 - every NEW texture-cache key, below
  const bool texKeyRecreate = it == m_texCache.end() && m_texRefresh.count(key) != 0;
#endif
  // iter-408: a hit is no longer automatically an answer. Slot 0 (a cached
  // DECODE-class rejection) and revalidation-off still short-circuit here;
  // otherwise we fall through, re-derive the surface parameters from the same
  // fetch constant (so they are identical by construction) and compare the
  // SOURCE bytes against what we uploaded. See TexRefresh in the header.
  const uint32_t cachedSlot = (it != m_texCache.end()) ? it->second : 0u;
  if (it != m_texCache.end() && (cachedSlot == 0u || !texRevalidateEnabled()))
    return cachedSlot;
  // P1: nothing it reads changed since it was validated - no hash needed.
  if (cachedSlot != 0u && rtc && m_texClean.count(key)) {
    auto rit = m_texRefresh.find(key);
    if (rit != m_texRefresh.end()) rit->second.lastUse = ++m_texUseClock;
#if SR_ORACLE
    if (memres::texIdOn()) texIdCleanHit(key, f);
#endif
    return cachedSlot;
  }
  // fail() caches DECODE-class rejections (unsupported format, bad header) —
  // deterministic per key. D3D/transient failures must NOT be cached: the old
  // path white-cached every texture forever after one mid-frame Reset E_FAIL.
  auto fail = [&]() -> uint32_t { m_texCache.emplace(key, 0u); return 0u; };
  static std::atomic<uint32_t> s_texFailLog{0};
  auto failTransient = [&](const char* what, HRESULT chr) -> uint32_t {
    if (s_texFailLog.fetch_add(1, std::memory_order_relaxed) < 12)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[TEX_FAIL] %s hr=0x%08X (not cached)",
                  what, (unsigned)chr);
    return 0u;
  };

  const uint32_t basePage = (f[1] >> 12) & 0xFFFFFu;
  const uint32_t fmt = f[1] & 0x3Fu;
  const uint32_t endian = (f[1] >> 6) & 3u;
  const uint32_t width = (f[2] & 0x1FFFu) + 1u;
  const uint32_t height = ((f[2] >> 13) & 0x1FFFu) + 1u;
  uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  const bool tiled = ((f[0] >> 31) & 1u) != 0;
  // iter-416: this rejection was SILENT. Every other failure path in this
  // function logs, so a texture that vanished here left no trace at all and
  // simply appeared as a WHITE draw — which is exactly the state the FMV's
  // 1280x720 luma plane is in (only the two 640x360 chroma planes ever get
  // created, and a YUV movie needs three).
  if (!basePage || !width || !height || width > 4096 || height > 4096) {
    static std::atomic<uint32_t> s_rejLog{0};
    if (s_rejLog.fetch_add(1, std::memory_order_relaxed) < 24)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[TEX_REJECT] fetch dims/base out of range -> WHITE: base=0x%08X "
                  "fmt=%u %ux%u pitchTexels=%u tiled=%u f0=%08X f1=%08X f2=%08X",
                  basePage * 4096u, fmt, width, height, pitchTexels,
                  unsigned(tiled), f[0], f[1], f[2]);
    return fail();
  }
  if (pitchTexels < width) pitchTexels = width;
  const uint8_t* src = gpuPhysToHost(basePage * 4096u);
  if (!src) return fail();
#if SR_ORACLE
  // iter-438c H1 probe: this bind lands on a surface a resolve recently wrote
  // (or dead-skipped). Log the fetch format the decoder will use vs the format
  // the resolve wrote, and whether the guest bytes are actually non-zero — the
  // three ways this chain silently produces a black/white composite.
  {
    const uint32_t basePhys = basePage * 4096u;
    for (const auto& r : m_recentResolveDests) {
      if (!r.phys || basePhys < r.phys || basePhys >= r.phys + r.bytes) continue;
      static std::atomic<uint32_t> s_rbindLog{0};
      const uint32_t bn = s_rbindLog.fetch_add(1, std::memory_order_relaxed);
      if (bn < 40 || bn % 500 == 0) {
        size_t nz = 0;
        for (size_t i = 0; i < 4096; i += 16) if (src[i]) nz++;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RESOLVE_BIND] #%u tex@0x%08X fetchFmt=%u %ux%u pitch=%u tiled=%u "
            "<- resolve dest=0x%08X destInfo=%08X (wroteFmt=%u)%s nzProbe=%zu/256",
            bn, basePhys, fmt, width, height, pitchTexels, unsigned(tiled),
            r.phys, r.destInfo, (r.destInfo >> 7) & 0x3Fu,
            r.dead ? " DEAD-STALE" : "", nz);
      }
      break;
    }
  }
#endif  // SR_ORACLE
  // iter-427d: SAME read-side alias probe as the vertex path (SR_VB_ALIAS_PROBE,
  // iter-425). The movie planes are LockRect'd through 0xAE-class VAs; under the
  // flat memory model those bytes are INVISIBLE at this 0x8-window read, so the
  // YUV converter samples zeros and the quad paints one flat colour no matter
  // how correct the UVs are. Console-consistent: every window holds the same
  // bytes on hardware, so preferring the window that HAS data is faithful
  // whenever exactly one was written.
  {
    static const bool s_probe = [] {
      const char* e2 = std::getenv("SR_VB_ALIAS_PROBE");
      return !e2 || e2[0] != '0';
    }();
    if (s_probe) {
      // Sample ACROSS the surface, not its first bytes: a letterboxed movie
      // frame's first rows are legitimately black through EVERY window, which
      // made a head-only probe blind exactly when it mattered.
      const uint64_t spanBytes = uint64_t(pitchTexels) * height;  // >=1B/texel
      auto sampleNz = [&](const uint8_t* p) -> uint32_t {
        uint32_t nz = 0;
        for (int s = 0; s < 64; s++) {
          const uint64_t off = (spanBytes * s) / 64;
          for (int b = 0; b < 16; b++) if (p[off + b]) { nz++; break; }
        }
        return nz;
      };
      const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u + basePage * 4096u);
      // iter-466 hfr: src already IS the 0xA window (the owner-window read picked it - every
      // gameplay texture upload, [PM4_SHM_FRAME] 0xA=558987 0x8=0): both samples would read
      // the same bytes, so the probe cannot fire. It cost 2.2% of the render thread.
      if (alt && alt != src && sr::kernel::isGuestVAInRange(0xA0000000u + basePage * 4096u +
                                              uint32_t(spanBytes))) {
        const uint32_t nz8 = sampleNz(src);
        const uint32_t nzA = sampleNz(alt);
        if (nz8 == 0 && nzA > 0) {
          static std::atomic<uint32_t> s_texAlias{0};
          const uint32_t tn = s_texAlias.fetch_add(1, std::memory_order_relaxed) + 1;
          if (tn <= 8 || tn % 2000 == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[TEX_ALIAS] #%u texture @phys %08X: 0x8-window empty, "
                        "0xA has data (nzA=%u/64, fmt=%u %ux%u)",
                        tn, basePage * 4096u, nzA, fmt, width, height);
          src = alt;
        }
      }
    }
  }
  // iter-408c: the pool is now RECYCLED rather than exhausted. Previously this
  // returned fail() once 1024 slots were handed out, which cached slot 0 (white)
  // for the key FOREVER — and `creates` hit 1023 on 3/3 rolls, so in every run a
  // point came where all newly-seen textures, including later movie frame
  // buffers, rendered white. Evicting the least-recently-used entry and reusing
  // its slot keeps the working set resident instead.
  // step 4 G7 (SR_TEX_CACHE_EVICT): a key whose cache entry went while its refresh record stayed re-creates through
  // the one erase path - the old resource and slot are retired (the assignment below would release the resource at
  // once, while a recorded frame may still sample it, and leak its slot).
  if (m_texEvictOn && cachedSlot == 0u && m_texRefresh.count(key)) {
    texEvictKey(key, kTexEvRecreate);
    texEvictFinish();
  }
  uint32_t reuseSlot = 0;
  // gate on: a slot taken from a slot-exhaustion victim goes to the free list when this create fails after all
  struct ReuseBack {
    std::vector<uint32_t>& freeSlots;
    uint32_t& slot;
    bool armed = false;
    ~ReuseBack() {
      if (armed && slot) freeSlots.push_back(slot);
    }
  } reuseBack{m_texFreeSlots, reuseSlot};
  // gate on, the pool at its cap: first the retired slots the GPU finished with since this frame's tick - unless a
  // transient exhaustion already failed in this very pool state (fix3, Astra 0a384c24 P2: the drain + the candidate
  // scan ran again on every bind; now only after a new frame, a fence completion or a pool change)
  auto poolStateNow = [&] {
    return sr::gfx::texevict::poolState({uint64_t(m_fenceValues[m_frameIndex]),
                                         m_fence ? uint64_t(m_fence->GetCompletedValue()) : 0u,
                                         uint64_t(m_texFreeSlots.size()), uint64_t(m_texSlotRetire.pending()),
                                         uint64_t(m_texNextSlot), uint64_t(m_texRefresh.size())});
  };
  if (m_texEvictOn && cachedSlot == 0u && !texSlotAvailable()) {
    if (!m_texRetry.shouldTry(poolStateNow())) {
      m_texTransientFails++;
      m_texTransientSuppressed++;
      return 0u;  // nothing can have changed since the last failure: no drain, no scan, nothing cached
    }
    texEvictDrain(256u);
  }
  if (m_texEvictOn && cachedSlot == 0u && !texSlotAvailable()) {
    // fix2 (Astra 5d0c2a63 P1 / P2): the least recently bound entry, never one bound in this frame nor a slot the draw
    // being assembled selected (it keeps slot numbers until its tables are built); none (every slot retiring or in
    // use) = a transient exhaustion: nothing is cached, the next bind retries
    uint64_t victim = 0;
    if (!texExhaustVictim(victim)) {
      m_texRetry.failed(poolStateNow());
      m_texTransientFails++;
      texCreateFailLog("2D slots exhausted, transient (every slot retiring or bound now; not cached, retried)", S_OK,
                       nullptr, basePage * 4096u, fmt, width, height);
      return 0u;
    }
    // the one erase path; the victim's slot is reused now (texExhaustVictim waited until the GPU finished the frame
    // that last bound it); its resource waits for the frame fence
    m_texRetry.succeeded();
    texEvictKey(victim, kTexEvExhaust, &reuseSlot);
    texEvictFinish();
    reuseBack.armed = true;
    static std::atomic<uint32_t> s_evictLog2{0};
    const uint32_t en = s_evictLog2.fetch_add(1, std::memory_order_relaxed) + 1;
    if (en <= 4 || en % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_EVICT] #%u reusing slot %u (LRU, SR_TEX_CACHE_EVICT) for base=0x%08X fmt=%u %ux%u",
                  en, reuseSlot, basePage * 4096u, fmt, width, height);
  } else if (!m_texEvictOn && m_texNextSlot >= kTex2DSlots && cachedSlot == 0u) {
    uint64_t oldest = ~0ull;
    uint64_t victim = 0;
    bool found = false;
    for (const auto& kv : m_texRefresh)
      if (kv.second.lastUse < oldest) { oldest = kv.second.lastUse; victim = kv.first; found = true; }
    if (!found) {
      static std::atomic<uint32_t> s_exhaustLog{0};
      if (s_exhaustLog.fetch_add(1, std::memory_order_relaxed) < 8)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[TEX_CACHE] SLOT EXHAUSTION (%u), nothing evictable (refreshMap=%zu) — "
                    "base=0x%08X fmt=%u %ux%u gets WHITE",
                    kTex2DSlots, m_texRefresh.size(), basePage * 4096u, fmt, width, height);
      texCreateFailLog("2D slots exhausted", S_OK, nullptr, basePage * 4096u, fmt, width, height);
      return fail();
    }
    reuseSlot = m_texRefresh[victim].slot;
    // Inline uploads never signal m_texFence: the frame list being recorded
    // may already sample the victim, so it lives until the frame fence.
    if (m_texInline)
      retireResource(m_texRefresh[victim].tex);
    else
      m_texRetired.emplace_back(m_texFenceValue + 1, m_texRefresh[victim].tex);
    if (const uint64_t sk = m_texRefresh[victim].shareKey) {  // iter-466 res4k (SR_TEX_SHARE)
      auto sit = m_texShared.find(sk);
      if (sit != m_texShared.end() && (sit->second.users <= 1u || --sit->second.users == 0u))
        texSharedErase(sk);  // the last binder: the retire list keeps it alive for the GPU
    }
    m_texRefresh.erase(victim);
    m_texCache.erase(victim);
    m_texClean.erase(victim);
    m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO
    // Release anything whose fence has passed (the upload below signals and
    // waits on m_texFence, so this drains steadily).
    const UINT64 done = m_texFence ? m_texFence->GetCompletedValue() : 0;
    m_texRetired.erase(std::remove_if(m_texRetired.begin(), m_texRetired.end(),
                                      [done](const auto& e) { return e.first <= done; }),
                       m_texRetired.end());
    static std::atomic<uint32_t> s_evictLog{0};
    const uint32_t en = s_evictLog.fetch_add(1, std::memory_order_relaxed) + 1;
    if (en <= 4 || en % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_EVICT] #%u reusing slot %u (LRU) for base=0x%08X fmt=%u %ux%u",
                  en, reuseSlot, basePage * 4096u, fmt, width, height);
  }

  DXGI_FORMAT dxgi;
  uint32_t blockW = 1, blockBytes = 4;  // block granularity (texel for linear formats)
  switch (fmt) {
    case 6:  dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; break;                          // k_8_8_8_8
    case 50: dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; break;                          // k_8_8_8_8_AS_16_16_16_16
    case 18: dxgi = DXGI_FORMAT_BC1_UNORM; blockW = 4; blockBytes = 8; break;   // k_DXT1
    case 19: dxgi = DXGI_FORMAT_BC2_UNORM; blockW = 4; blockBytes = 16; break;  // k_DXT2_3
    case 20: dxgi = DXGI_FORMAT_BC3_UNORM; blockW = 4; blockBytes = 16; break;  // k_DXT4_5
    case 49: dxgi = DXGI_FORMAT_BC5_UNORM; blockW = 4; blockBytes = 16; break;  // k_DXN
    case 24: dxgi = DXGI_FORMAT_R16_UNORM; blockBytes = 2; break;               // k_16
    case 36: dxgi = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; break;               // k_32_FLOAT
    // iter-413 DEPTH-AS-TEXTURE. The FMV oracle showed "[TEX_CACHE] unsupported
    // fmt=23 1280x720" twice per frame -> fail() -> a WHITE 1x1 sampled by the
    // draw. Both are a packed dword (24-bit depth in the high bits, 8-bit
    // stencil in the low) with no directly-sampleable DXGI equivalent, so they
    // are unpacked to R32_FLOAT below, after the endian swap.
    case 22:
    case 23:
      if (!texDepthFmtEnabled()) {
        static std::atomic<uint32_t> s_dLog{0};
        if (s_dLog.fetch_add(1, std::memory_order_relaxed) < 4)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[TEX_CACHE] depth fmt=%u %ux%u disabled (SR_TEX_DEPTHFMT=0) -> WHITE",
                      fmt, width, height);
        return fail();
      }
      dxgi = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; break;
    // 16-bit-per-channel family. The scene composites through these: the HDR
    // cityscape is fetched as fmt=29 at 1280x720 (+ its 320x180 bloom chain),
    // and every one of them used to land in fail() -> a WHITE 1x1, which is why
    // the whole background of the title screen came out white.
    // _EXPAND is the biased/signed-normalised encoding, so it maps to SNORM;
    // the plain integer forms are UNORM.
    case 2:  dxgi = DXGI_FORMAT_R8_UNORM; blockBytes = 1; break;                // k_8
    case 25: dxgi = DXGI_FORMAT_R16G16_UNORM; blockBytes = 4; break;            // k_16_16
    case 26: dxgi = DXGI_FORMAT_R16G16B16A16_UNORM; blockBytes = 8; break;      // k_16_16_16_16
    // P1: on the PM4 path _EXPAND samples as FLOAT - xenia-canary GetBaseFormat
    // (texture_info.h:30-32) maps k_16*_EXPAND to k_16*_FLOAT. The legacy xlat
    // path keeps SNORM so the gate-off control stays comparable.
    case 27: dxgi = pm4RtCacheEnabled() ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R16_SNORM;
             blockBytes = 2; break;                                              // k_16_EXPAND
    case 28: dxgi = pm4RtCacheEnabled() ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16_SNORM;
             blockBytes = 4; break;                                              // k_16_16_EXPAND
    case 29: dxgi = pm4RtCacheEnabled() ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                        : DXGI_FORMAT_R16G16B16A16_SNORM;
             blockBytes = 8; break;                                              // k_16_16_16_16_EXPAND
    case 30: dxgi = DXGI_FORMAT_R16_FLOAT; blockBytes = 2; break;               // k_16_FLOAT
    case 31: dxgi = DXGI_FORMAT_R16G16_FLOAT; blockBytes = 4; break;            // k_16_16_FLOAT
    case 32: dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; blockBytes = 8; break;      // k_16_16_16_16_FLOAT
    default: {
      static std::atomic<uint32_t> s_fmtLog{0};
      if (s_fmtLog.fetch_add(1) < 8)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[TEX_CACHE] unsupported fmt=%u %ux%u", fmt, width, height);
      return fail();
    }
  }
  const uint32_t rows = (height + blockW - 1) / blockW;      // block rows
  const uint32_t rowBlocks = (width + blockW - 1) / blockW;  // blocks per row
  const uint32_t rowBytes = rowBlocks * blockBytes;
  const uint32_t srcPitch = (pitchTexels / blockW) * blockBytes;

  // iter-408: hash the SOURCE span, not the decoded result — it is the same
  // span the resolve-eviction bookkeeping already records, and hashing it
  // avoids paying for a de-tile on every bind just to discover nothing moved.
  const size_t srcBytes = size_t(rows) * srcPitch;
  // iter-460 S4 (SR_PM4_SHMEM): a PM4 bind loads the texture on the GPU from
  // the shared memory (Xenia texture_load_*), versioned by the shared memory's
  // page change stamps instead of a CPU hash of the source bytes.
  const bool shmTex = pm4ShmEnabled() && m_texInline && pm4RtCacheEnabled() &&
                      pm4ShmLoadable(fmt) && pm4ShmInit();
  std::vector<uint8_t> nativeBase, nativeMips;
  if (!shmTex && (pm4ShmEnabled() || memres::resourceMode())) {
    // The CPU decoder reads guest RAM: resolve output it covers comes back first.
    uint32_t pb = 0;
    const uint32_t span = std::max<uint32_t>(uint32_t(srcBytes),
        sr::gfx::cube::detail::levelSliceStride(0, true, width, height, pitchTexels, blockW, blockW,
                                               blockBytes, tiled, &pb));
    if (memres::resourceDecides()) {
      resourceCpuSnapshot(basePage * 4096u, span, nativeBase);
      src = nativeBase.data();
#if SR_ORACLE
    } else {
      pm4ShmSyncToGuest(basePage * 4096u, span);
#endif
    }
    const uint32_t mipPage = (f[5] >> 12) & 0xFFFFFu;
#if SR_ORACLE
    if (mipPage && !memres::resourceDecides()) pm4ShmSyncToGuest(mipPage * 4096u, uint32_t(srcBytes));
#endif
  }
#if SR_ORACLE
  uint64_t contentHash = shmTex || memres::resourceDecides() ? 0u : ShaderCache::hashUcode(src, srcBytes);
#else
  uint64_t contentHash = 0;  // assigned the recorded resource version below
#endif

  // ── P1 (PM4 path): the mip chain, placed by Xenia's GetGuestTextureLayout ──
  // mip_address (dword_5 bits 12-31) != 0 gives levels 1..min(mip_max_level,
  // log2(longest axis)) (texture_util.cc:68-90). packed_mips (bit 11) puts every
  // level from GetPackedMipLevel on into ONE shared tail slice, and a texture
  // whose packed level is 0 keeps even its BASE in a tail at a packed offset
  // (texture_util.cc:284-289). Layout and placement are the tested
  // cube_layout.hpp bodies with one face. Legacy xlat path: base only, as before.
  namespace CL = sr::gfx::cube;
  CL::CubeLayout mipLayout{};
  const uint8_t* mipSrc = nullptr;
  uint32_t mipLevels = 1;
  if (pm4RtCacheEnabled() && fmt != 22u && fmt != 23u) {
    const uint32_t mipPage = (f[5] >> 12) & 0xFFFFFu;
    const bool packed = ((f[5] >> 11) & 1u) != 0;
    const uint32_t sizeMax = CL::detail::log2Floor(width > height ? width : height);
    const uint32_t mipMax = mipPage ? std::min((f[4] >> 6) & 0xFu, sizeMax) : 0u;
    const bool baseTail = packed && CL::detail::packedMipLevel(width, height) == 0u;
    if (mipMax > 0u || baseTail) {
      mipLayout = CL::computeGuestLayout(fmt, width, height, pitchTexels, tiled, mipMax + 1u,
                                         packed, blockW, blockW, blockBytes, 1u);
      const uint32_t ext = mipLayout.mipsTotalExtentBytes;
      if (mipLayout.ok && mipMax > 0u) {
        mipSrc = gpuPhysToHost(mipPage * 4096u);
        if (!mipSrc || !ext || !gpuPhysToHost(mipPage * 4096u + ext - 1u)) mipLayout.ok = false;
      }
      if (mipLayout.ok) {
        mipLevels = mipMax + 1u;
        if (mipSrc && !shmTex && memres::resourceDecides()) {
          resourceCpuSnapshot(mipPage * 4096u, ext, nativeMips);
          mipSrc = nativeMips.data();
        }
#if SR_ORACLE
        if (mipSrc && !shmTex && !memres::resourceDecides())
          contentHash ^= ShaderCache::hashUcode(mipSrc, ext) * 0x9E3779B97F4A7C15ull;
#endif
      } else {
        static std::atomic<uint32_t> s_mipRefuse{0};
        if (s_mipRefuse.fetch_add(1, std::memory_order_relaxed) < 16)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[TEX_MIPS] refused (%s) fmt=%u %ux%u levels=%u packed=%d base=0x%08X "
                      "mips=0x%08X -> base level only",
                      CL::cubeRefuseName(mipLayout.refuse), fmt, width, height, mipMax + 1u,
                      packed ? 1 : 0, basePage * 4096u, mipPage * 4096u);
      }
    }
  }
  // Native clean marks must cover the full range actually consumed, including
  // tiled padding and packed tails beyond rows * pitch. A same-frame write to
  // any such page must invalidate the fast-clean answer before the next bind.
  uint64_t cleanBaseBytes = srcBytes;  // retain the legacy Oracle registration
  if (!shmTex && memres::resourceDecides()) {
    if (mipLayout.ok && mipLayout.baseSpanBytes > nativeBase.size()) {
      resourceCpuSnapshot(basePage * 4096u, mipLayout.baseSpanBytes, nativeBase);
      src = nativeBase.data();
    }
    uint32_t version = pm4ShmVersion(basePage * 4096u, uint32_t(nativeBase.size()));
    if (!nativeMips.empty())
      version = std::max(version, pm4ShmVersion(((f[5] >> 12) & 0xFFFFFu) * 4096u, uint32_t(nativeMips.size())));
    contentHash = (1ull << 63) | version;
    cleanBaseBytes = nativeBase.size();
  }
  // iter-460 live trace: the CPU decoder reads these bytes (the GPU loader's
  // pages are recorded by pm4ShmRequest).
  if (!shmTex && livetrace::recording()) {
    livetrace::memRead(basePage * 4096u, src, uint32_t(srcBytes));
    if (mipSrc && mipLayout.ok && mipLayout.mipsTotalExtentBytes)
      livetrace::memRead(((f[5] >> 12) & 0xFFFFFu) * 4096u, mipSrc, mipLayout.mipsTotalExtentBytes);
  }
#if SR_ORACLE
  // iter-460 (SR_PM4_TEXPROBE=<phys hex>): where does this texture's data live?
  {
    static const uint32_t s_probe = [] {
      const char* e = std::getenv("SR_PM4_TEXPROBE");
      return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    static uint32_t s_probeN = 0;
    if (s_probe && basePage * 4096u == (s_probe & 0x1FFFF000u) && (s_probeN++ % 200u) == 0u) {
      const uint32_t phys = basePage * 4096u;
      const uint32_t len = uint32_t(srcBytes);
      auto stat = [&](uint32_t win, char* out, size_t outN) {
        const uint32_t va = win + phys;
        const uint8_t* h = sr::kernel::guestVAToHost(va);
        if (!h || !sr::kernel::isGuestVAInRange(va + len - 1u)) {
          std::snprintf(out, outN, "unmapped");
          return;
        }
        uint32_t nz = 0;
        for (uint32_t i = 0; i < len; i++) nz += h[i] != 0;
        std::snprintf(out, outN, "nz=%u/%u head=%02X%02X%02X%02X %02X%02X%02X%02X", nz, len, h[0],
                      h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
      };
      char s8[96], sA[96];
      stat(0x80000000u, s8, sizeof(s8));
      stat(0xA0000000u, sA, sizeof(sA));
      const uint32_t pg = phys >> 12;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_PROBE] %08X fmt=%u %ux%u tiled=%d endian=%u owner=%d shmTex=%d | 0x8: %s | "
                  "0xA: %s | shm page0 gpuWritten=%d srcA=%d stamp=%u",
                  phys, fmt, width, height, tiled ? 1 : 0, endian,
                  sr::kernel::physwin::gpuWindow(phys, len), shmTex ? 1 : 0, s8, sA,
                  m_shmReady ? int(m_shmGpuWritten[pg]) : -1, m_shmReady ? int(m_shmSrcA[pg]) : -1,
                  m_shmReady ? m_shmStamp[pg] : 0u);
    }
  }
#endif
  if (shmTex) {
    // Request the base and mip ranges (uploads what the CPU changed), then the
    // texture's version = the newest page stamp under them.
    uint32_t pb = 0;
    uint32_t baseLen = CL::detail::levelSliceStride(0, true, width, height, pitchTexels, blockW,
                                                    blockW, blockBytes, tiled, &pb);
    if (mipLayout.ok) baseLen = std::max(baseLen, mipLayout.faceStrideBytes);
    if (memres::resourceDecides()) cleanBaseBytes = baseLen;
    const memres::KindScope mrKind(memres::Kind::kTex);  // mem.resources step 2a: the census's fetch kind
    pm4ShmRequest(basePage * 4096u, baseLen);
    uint32_t v = pm4ShmVersion(basePage * 4096u, baseLen);
    uint32_t trackMip = 0, trackMipLen = 0;
    if (mipLayout.ok && mipLevels > 1u && mipLayout.mipsTotalExtentBytes) {
      const uint32_t mipPhys = ((f[5] >> 12) & 0xFFFFFu) * 4096u;
      pm4ShmRequest(mipPhys, mipLayout.mipsTotalExtentBytes);
      v = std::max(v, pm4ShmVersion(mipPhys, mipLayout.mipsTotalExtentBytes));
      trackMip = mipPhys;
      trackMipLen = mipLayout.mipsTotalExtentBytes;
    }
    contentHash = (1ull << 63) | v;
    // iter-466 cp lane (SR_SHM_WT_TEXCLEAN): the ranges the loader reads and their version;
    // texMarkClean decides whether the mark can outlive the present.
#if SR_ORACLE
    if (texCleanTrackedOn())
      m_texTrack = TexTrack{true, basePage * 4096u, baseLen, trackMip, trackMipLen, v};
#endif
  }
  // Per-run tallies. The sampled [TEX_REFRESH] lines are far too sparse to
  // settle "does the fix actually do anything on this roll" — the first eight
  // events plus every five-hundredth can easily miss a surface entirely, which
  // is exactly what happened on the first roll measured.
  static std::atomic<uint64_t> s_creates{0}, s_refreshes{0}, s_skips{0};
  auto texStatsTick = [&]() {
    const uint64_t tot = s_creates.load(std::memory_order_relaxed) +
                         s_refreshes.load(std::memory_order_relaxed) +
                         s_skips.load(std::memory_order_relaxed);
    if (tot % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_STATS] creates=%llu refreshes=%llu unchanged=%llu",
                  (unsigned long long)s_creates.load(std::memory_order_relaxed),
                  (unsigned long long)s_refreshes.load(std::memory_order_relaxed),
                  (unsigned long long)s_skips.load(std::memory_order_relaxed));
  };
#if SR_ORACLE
  // mem.resources textures_from_resolves (gfx/resource_texid.hpp, SR_MEM_RES_TEXID=1): this bind's class, recorded at
  // whichever return below; the load origin a successful load gives the bound texture.
  struct TexIdRec {
    bool on = false;
    uint32_t pages = 0, gpu = 0, base = 0, fmt = 0, cpuAfter = 0, src = 0;
    bool decided = false;  // a path below named the class (else: a failed bind with resolve pages counts unknown)
    memres::TexIdClass cls = memres::TexIdClass::kUnknown;
    uint8_t* store = nullptr;  // the cache entry's recorded class (its clean-mark hits repeat it)
    void set(memres::TexOrigin o) { cls = memres::texIdClassOf(o, gpu, cpuAfter); decided = true; }
    void stale() { cls = gpu || cpuAfter ? memres::TexIdClass::kStale : memres::TexIdClass::kPlain; decided = true; }
    ~TexIdRec() {
      if (!on) return;
      if (!decided && !gpu && !cpuAfter) cls = memres::TexIdClass::kPlain;
      if (store) *store = uint8_t(cls);
      memres::texIdBind(memres::TexIdKind::k2D, cls, pages, gpu, base, fmt, cpuAfter, src);
    }
  } tid;
  const memres::TexOrigin tidLoad = !memres::texIdOn() ? memres::TexOrigin::kNone
                                    : shmTex ? memres::TexOrigin::kArena
                                    : memres::resourceDecides() ? memres::TexOrigin::kCpuSnapshot
                                                                : memres::TexOrigin::kGuestBytes;
  if (memres::texIdOn()) {
    tid.on = true;
    tid.base = basePage * 4096u;
    tid.fmt = fmt;
    const uint32_t baseSpan = uint32_t(std::max<uint64_t>(srcBytes, cleanBaseBytes));
    const uint32_t mipSpan = mipLayout.ok && mipLevels > 1u ? mipLayout.mipsTotalExtentBytes : 0u;
    tid.pages = (baseSpan + 4095u) / 4096u + (mipSpan + 4095u) / 4096u;
    tid.gpu = texIdGpuPages(basePage * 4096u, baseSpan, &tid.cpuAfter, &tid.src) +
              (mipSpan ? texIdGpuPages(((f[5] >> 12) & 0xFFFFFu) * 4096u, mipSpan, &tid.cpuAfter, &tid.src) : 0u);
  }
#endif
  if (cachedSlot != 0u) {
    auto rit = m_texRefresh.find(key);
    if (rit != m_texRefresh.end()) rit->second.lastUse = ++m_texUseClock;  // LRU touch
    if (rit != m_texRefresh.end() && rit->second.contentHash == contentHash) {
#if SR_ORACLE
      if (tid.on) {
        tid.set(memres::TexOrigin(rit->second.texIdOrigin));
        tid.store = &rit->second.texIdClass;
      }
#endif
      s_skips.fetch_add(1, std::memory_order_relaxed);
      texStatsTick();
      if (pm4RtCacheEnabled()) {
        texMarkClean(key, cachedSlot, basePage * 4096u, cleanBaseBytes);
        if (mipSrc) texMarkClean(key, cachedSlot, ((f[5] >> 12) & 0xFFFFFu) * 4096u, mipLayout.mipsTotalExtentBytes);
      }
      return cachedSlot;  // unchanged since upload — the common case
    }
    if (rit == m_texRefresh.end()) {
#if SR_ORACLE
      tid.stale();
#endif
      return cachedSlot;  // pre-iter-408 entry
    }
    s_refreshes.fetch_add(1, std::memory_order_relaxed);
  } else {
    s_creates.fetch_add(1, std::memory_order_relaxed);
  }
  texStatsTick();

  // Stage endian-correct, untiled block rows. Xenos tiling operates on the
  // BLOCK grid for compressed formats — the same tiled2D transform with the
  // block pitch and bytes-per-block (Xenia texture_util model).
  std::vector<uint8_t> lin;
  if (!shmTex) lin.resize(size_t(rows) * rowBytes);
  if (shmTex) {
    // GPU load below (uploadLevels): no CPU untile.
  } else if (tiled) {
    uint32_t bpbLog2 = 0;
    while ((1u << bpbLog2) < blockBytes) bpbLog2++;
    // Xenia texture_util.cc:323: the tiled block pitch is aligned UP to 32
    // blocks (not clamped to a minimum).
    uint32_t blockPitch = ((pitchTexels / blockW) + 31u) & ~31u;
    // iter-460: rows in parallel (each row writes only its own bytes); the
    // single-threaded per-block walk was most of a live movie frame's bind cost.
    par::parallelFor(0u, rows, [&](uint32_t by) {
      uint8_t* drow = lin.data() + size_t(by) * rowBytes;
      for (uint32_t bx = 0; bx < rowBlocks; bx++) {
        const int32_t off = xenos_tiling::tiled2D(int32_t(bx), int32_t(by),
                                                  blockPitch, bpbLog2);
        std::memcpy(drow + size_t(bx) * blockBytes, src + off, blockBytes);
      }
    });
  } else {
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(lin.data() + size_t(y) * rowBytes, src + size_t(y) * srcPitch, rowBytes);
  }
  // Endian: texture data is BE guest memory; endian op per the fetch constant.
  if (endian && !shmTex) {
    uint32_t* p = reinterpret_cast<uint32_t*>(lin.data());
    const size_t words = lin.size() / 4;
    const uint32_t chunks = uint32_t((words + 16383u) / 16384u);
    par::parallelFor(0u, chunks, [&](uint32_t c) {
      const size_t e = std::min(words, size_t(c + 1u) * 16384u);
      for (size_t i = size_t(c) * 16384u; i < e; i++) p[i] = xeSwap32(p[i], endian);
    });
  }

  // iter-413: unpack depth-as-texture into the R32_FLOAT the SRV declares.
  // Runs AFTER the endian swap, because the packed dword has to be in host
  // order before the depth and stencil halves can be separated.
  if ((fmt == 22u || fmt == 23u) && !shmTex) {
    uint32_t* p = reinterpret_cast<uint32_t*>(lin.data());
    const size_t n = lin.size() / 4;
    for (size_t i = 0; i < n; i++) {
      const uint32_t d24 = p[i] >> 8;  // depth high 24, stencil low 8
      const float f = (fmt == 22u) ? (float(d24) / 16777215.0f)
                                   : xeFloat20e4To32(d24);
      std::memcpy(&p[i], &f, 4);
    }
  }

  // Distinguish "we decoded it wrong" from "there was nothing there": a
  // resolve-destination surface that the guest never wrote (or that our
  // resolve never wrote BACK to guest memory) decodes to a clean zero image,
  // which is a different bug from a bad format mapping.
  if (fmt >= 24 && fmt <= 36) {
    size_t nz = 0;
    for (size_t i = 0; i < lin.size(); i++)
      if (lin[i]) { nz++; if (nz > 64) break; }
    static std::atomic<uint32_t> s_wideLog{0};
    if (s_wideLog.fetch_add(1, std::memory_order_relaxed) < 16)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_CACHE] wide fmt=%u %ux%u tiled=%u endian=%u base=0x%08X "
                  "bytes=%zu src=%s",
                  fmt, width, height, unsigned(tiled), endian, basePage * 4096u,
                  lin.size(), nz ? "HAS-DATA" : "ALL-ZERO");
  }

  // iter-408: a refresh reaches here every time the source changes, i.e. once
  // per video frame per plane. Dumping all of them is ~110 MB/s of BMP writes,
  // enough to distort the very run being measured — so refresh dumps are capped
  // and sequence-numbered (a numbered series is also what you want for a movie:
  // frame 1 vs frame 40 answers "is it moving", which one overwritten file
  // cannot).
  static std::atomic<uint32_t> s_refreshDumps{0};
  const bool isRefresh = (cachedSlot != 0u);
  uint32_t dumpSeq = 0;
  bool doDump = texDumpEnabled() && !shmTex;
  if (doDump && isRefresh) {
    dumpSeq = s_refreshDumps.fetch_add(1, std::memory_order_relaxed) + 1;
    doDump = dumpSeq <= 40;
  }
  if (doDump) {
    char nm[128];
    if (blockW == 4) {
      const char* cc = (fmt == 18) ? "DXT1" : (fmt == 19) ? "DXT3" : (fmt == 20) ? "DXT5" : "ATI2";
      snprintf(nm, sizeof(nm), "texdump_slot%u_fmt%u_%ux%u_0x%08X_r%u.dds",
               m_texNextSlot, fmt, width, height, basePage * 4096u, dumpSeq);
      writeDds(nm, lin.data(), lin.size(), width, height, cc);
    } else {
      // Linear: expand to BGRA for a viewable BMP (k_16/k_32F -> gray).
      std::vector<uint8_t> bgra(size_t(width) * height * 4u);
      for (uint32_t y = 0; y < height && y < rows; y++) {
        for (uint32_t x = 0; x < width; x++) {
          const uint8_t* sp = lin.data() + size_t(y) * rowBytes + size_t(x) * blockBytes;
          uint8_t* dp = bgra.data() + (size_t(y) * width + x) * 4;
          // iter-414 TOOLING: decode PER FORMAT, not per byte-width. The old
          // chain had exactly three arms and an `else` that read four bytes as
          // a float — so every 8-byte-per-texel surface (fmt 26/29/32, the
          // whole 16-bit-per-channel family, including the 1280x720 scene
          // surface) was rendered from half its first channel and came out
          // uniform. That is the SAME defect as the k_8 bug fixed in iter-408,
          // which produced two retracted findings; leaving another instance of
          // it in the inspection path is leaving a trap for the next session.
          auto u8 = [](float v) {
            return uint8_t(v <= 0.0f ? 0 : v >= 1.0f ? 255 : v * 255.0f + 0.5f);
          };
          auto half2f = [](uint16_t h) {
            const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
            uint32_t o;
            if (e == 0) o = m ? ((s << 31) | ((127 - 15 + 1) << 23) | (m << 13)) : (s << 31);
            else if (e == 31) o = (s << 31) | 0x7F800000u | (m << 13);
            else o = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
            float f; std::memcpy(&f, &o, 4); return f;
          };
          auto rd16 = [&](int i) { uint16_t v; std::memcpy(&v, sp + i * 2, 2); return v; };
          // P1: the PM4 path uploads _EXPAND as half floats - dump what we bound.
          const uint32_t dumpFmt = (fmt >= 27u && fmt <= 29u && pm4RtCacheEnabled()) ? fmt + 3u : fmt;
          switch (dumpFmt) {
            case 2:  // k_8
              dp[0] = dp[1] = dp[2] = sp[0]; dp[3] = 255; break;
            case 6: case 50:  // k_8_8_8_8 — RGBA -> BGRA
              dp[0] = sp[2]; dp[1] = sp[1]; dp[2] = sp[0]; dp[3] = sp[3]; break;
            case 24:  // k_16 UNORM
              dp[0] = dp[1] = dp[2] = uint8_t(rd16(0) >> 8); dp[3] = 255; break;
            case 27:  // k_16_EXPAND (SNORM)
              dp[0] = dp[1] = dp[2] = u8(int16_t(rd16(0)) / 32767.0f * 0.5f + 0.5f);
              dp[3] = 255; break;
            case 30:  // k_16_FLOAT
              dp[0] = dp[1] = dp[2] = u8(half2f(rd16(0))); dp[3] = 255; break;
            case 25:  // k_16_16 UNORM
              dp[2] = uint8_t(rd16(0) >> 8); dp[1] = uint8_t(rd16(1) >> 8);
              dp[0] = 0; dp[3] = 255; break;
            case 28:  // k_16_16_EXPAND
              dp[2] = u8(int16_t(rd16(0)) / 32767.0f * 0.5f + 0.5f);
              dp[1] = u8(int16_t(rd16(1)) / 32767.0f * 0.5f + 0.5f);
              dp[0] = 0; dp[3] = 255; break;
            case 31:  // k_16_16_FLOAT
              dp[2] = u8(half2f(rd16(0))); dp[1] = u8(half2f(rd16(1)));
              dp[0] = 0; dp[3] = 255; break;
            case 26:  // k_16_16_16_16 UNORM
              dp[2] = uint8_t(rd16(0) >> 8); dp[1] = uint8_t(rd16(1) >> 8);
              dp[0] = uint8_t(rd16(2) >> 8); dp[3] = 255; break;
            case 29:  // k_16_16_16_16_EXPAND (SNORM) — the HDR scene surface
              dp[2] = u8(int16_t(rd16(0)) / 32767.0f * 0.5f + 0.5f);
              dp[1] = u8(int16_t(rd16(1)) / 32767.0f * 0.5f + 0.5f);
              dp[0] = u8(int16_t(rd16(2)) / 32767.0f * 0.5f + 0.5f);
              dp[3] = 255; break;
            case 32: {  // k_16_16_16_16_FLOAT — Reinhard so HDR stays legible
              auto tm = [&](uint16_t h) { float f = half2f(h); return u8(f / (f + 1.0f)); };
              dp[2] = tm(rd16(0)); dp[1] = tm(rd16(1)); dp[0] = tm(rd16(2));
              dp[3] = 255; break;
            }
            default: {  // fmt 36 k_32_FLOAT and the depth formats (already f32)
              float fv; std::memcpy(&fv, sp, 4);
              dp[0] = dp[1] = dp[2] = u8(fv); dp[3] = 255; break;
            }
          }
        }
      }
      snprintf(nm, sizeof(nm), "texdump_slot%u_fmt%u_%ux%u_0x%08X_r%u.bmp",
               m_texNextSlot, fmt, width, height, basePage * 4096u, dumpSeq);
      writeBmp32(nm, bgra.data(), width, height, width * 4);
    }
  }

  // P1: decode every level the layout placed (mipLevels > 1, or a base that
  // lives in a packed tail). Level 0 at the origin reuses `lin` above.
  struct MipImage { std::vector<uint8_t> lin; uint32_t rows = 0, rowBytes = 0, w = 0, h = 0; };
  std::vector<MipImage> chain;
  if (mipLayout.ok && !shmTex) {
    uint32_t bpbLog2 = 0;
    while ((1u << bpbLog2) < blockBytes) bpbLog2++;
    for (uint32_t m = 0; m < mipLevels; m++) {
      const CL::SubResPlacement p = CL::placeSubresource(mipLayout, width, height, blockW, blockW, 0u, m);
      if (!p.ok) { chain.clear(); break; }
      MipImage L;
      L.w = p.width;
      L.h = p.height;
      const uint32_t wB = (p.width + blockW - 1u) / blockW, hB = (p.height + blockW - 1u) / blockW;
      L.rows = hB;
      L.rowBytes = wB * blockBytes;
      if (p.region == CL::SubResRegion::Base) {
        L.lin = lin;
      } else {
        const bool inBase = p.region == CL::SubResRegion::BasePackedTail;
        const uint8_t* rsrc = (inBase ? src : mipSrc) + p.byteOffset;
        const uint32_t storeLevel = p.region == CL::SubResRegion::PackedTail ? mipLayout.packedLevel
                                    : inBase                                 ? 0u
                                                                             : m;
        uint32_t pitchBlocks = 0;
        CL::detail::levelSliceStride(storeLevel, inBase, width, height, pitchTexels, blockW, blockW,
                                     blockBytes, tiled, &pitchBlocks);
        uint32_t rowPitchBytes = pitchBlocks * blockBytes;
        if (!tiled && !inBase) rowPitchBytes = (rowPitchBytes + 255u) & ~255u;
        const bool inTail = p.region != CL::SubResRegion::Mip;
        const uint32_t x0 = inTail ? p.xBlocks : 0u, y0 = inTail ? p.yBlocks : 0u;
        L.lin.resize(size_t(L.rows) * L.rowBytes);
        for (uint32_t by = 0; by < hB; by++)
          for (uint32_t bx = 0; bx < wB; bx++) {
            const size_t off = tiled ? size_t(xenos_tiling::tiled2D(int32_t(x0 + bx), int32_t(y0 + by),
                                                                    pitchBlocks, bpbLog2))
                                     : size_t(y0 + by) * rowPitchBytes + size_t(x0 + bx) * blockBytes;
            std::memcpy(L.lin.data() + (size_t(by) * wB + bx) * blockBytes, rsrc + off, blockBytes);
          }
        if (endian) {
          uint32_t* q = reinterpret_cast<uint32_t*>(L.lin.data());
          for (size_t i = 0; i < L.lin.size() / 4; i++) q[i] = xeSwap32(q[i], endian);
        }
      }
      chain.push_back(std::move(L));
    }
  }
  // Upload every level into subresource m (footprints block-aligned: a 2x2
  // BC mip is still one 4x4 block). No chain = the base-only upload.
  auto uploadLevels = [&](ID3D12Resource* res, bool fromSR) -> bool {
    if (shmTex) {
      ShmTexDesc d;
      d.fmt = fmt;
      d.width = width;
      d.height = height;
      d.pitchTexels = pitchTexels;
      d.blockW = blockW;
      d.blockBytes = blockBytes;
      d.endian = endian;
      d.basePhys = basePage * 4096u;
      d.mipPhys = ((f[5] >> 12) & 0xFFFFFu) * 4096u;
      d.levels = mipLayout.ok ? mipLevels : 1u;
      d.tiled = tiled;
      d.dxgi = dxgi;
      d.layout = mipLayout.ok ? &mipLayout : nullptr;
      d.scaleX = tsx;  // iter-464 resscale
      d.scaleY = tsy;
      {  // iter-466 res4k (SR_RES_TEXLOG=<n>, diagnostic): why each GPU texture load happens
        static const uint32_t s_n = [] {
          const char* e = std::getenv("SR_RES_TEXLOG");
          return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
        }();
        static std::atomic<uint32_t> s_i{0};
        if (s_n && (tsx > 1u || tsy > 1u) && s_i.fetch_add(1, std::memory_order_relaxed) < s_n)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[RES_TEXLOG] %s key=%016llX base=%08X fmt=%u %ux%u s%u ver=%u f=%08X %08X %08X "
                      "%08X %08X %08X",
                      cachedSlot ? "refresh" : "create", (unsigned long long)key, d.basePhys, fmt,
                      width, height, tsx, uint32_t(contentHash), f[0], f[1], f[2], f[3], f[4], f[5]);
      }
      return pm4ShmLoadTexture(res, fromSR, d);
    }
    if (chain.empty())
      return uploadTexture2D(res, lin.data(), rows, rowBytes, width, height, dxgi, fromSR);
    for (uint32_t m = 0; m < chain.size(); m++) {
      const MipImage& L = chain[m];
      const uint32_t fw = (L.w + blockW - 1u) / blockW * blockW, fh = (L.h + blockW - 1u) / blockW * blockW;
      if (!uploadTexture2D(res, L.lin.data(), L.rows, L.rowBytes, fw, fh, dxgi, fromSR || m > 0u, 1u, m))
        return false;
    }
    return true;
  };
  const uint32_t resMips = shmTex ? (mipLayout.ok ? mipLevels : 1u)
                                 : (chain.empty() ? 1u : uint32_t(chain.size()));

  // iter-408 REFRESH: this surface is already cached and its source bytes
  // changed — a video frame, or any surface the guest CPU rewrites in place.
  // Re-upload into the SAME resource so the existing SRV and slot stay valid
  // (allocating a new slot per frame would exhaust the 1024-slot pool in
  // seconds and churn descriptors for no reason).
  if (cachedSlot != 0u) {
    auto& rf = m_texRefresh[key];
#if SR_ORACLE
    if (tid.on) tid.store = &rf.texIdClass;
#endif
    if (!rf.tex) {
#if SR_ORACLE
      tid.stale();
#endif
      return cachedSlot;
    }
    // iter-466 res4k (SR_TEX_SHARE): a sibling key already loaded this version into the shared
    // resource this slot binds - nothing to load.
    TexShared* sh = nullptr;
    if (rf.shareKey) {
      auto sit = m_texShared.find(rf.shareKey);
      if (sit != m_texShared.end() && sit->second.tex.Get() == rf.tex.Get()) sh = &sit->second;
    }
    if (sh && sh->contentHash == contentHash) {
      m_texShareSkips++;
#if SR_ORACLE
      if (tid.on) {
        rf.texIdOrigin = sh->texIdOrigin;  // the shared resource's content: loaded, or written by a native target
        tid.set(memres::TexOrigin(sh->texIdOrigin));
      }
#endif
    } else {
      if (!uploadLevels(rf.tex.Get(), /*fromSR=*/true)) {
#if SR_ORACLE
        tid.stale();
#endif
        return cachedSlot;  // transient upload failure: keep showing the last frame
      }
      if (sh) {
        sh->contentHash = contentHash;
        m_texShareLoads++;
      }
#if SR_ORACLE
      if (tid.on) {
        rf.texIdOrigin = uint8_t(tidLoad);
        if (sh) sh->texIdOrigin = uint8_t(tidLoad);
        tid.set(tidLoad);
      }
#endif
    }
    rf.contentHash = contentHash;
    glyphNote(cachedSlot, basePage, fmt, width, height, src, srcBytes, pitchTexels, tiled, blockW,
              blockBytes);  // iter-464 prompt icons (iter-465: tiled ones de-tiled)
    if (pm4RtCacheEnabled()) {
      texMarkClean(key, cachedSlot, basePage * 4096u, cleanBaseBytes);
      if (mipSrc) texMarkClean(key, cachedSlot, ((f[5] >> 12) & 0xFFFFFu) * 4096u, mipLayout.mipsTotalExtentBytes);
    }
    static std::atomic<uint32_t> s_refreshLog{0};
    const uint32_t rn = s_refreshLog.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rn <= 8 || (rn % 500) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_REFRESH] #%u slot=%u fmt=%u %ux%u base=0x%08X source changed",
                  rn, cachedSlot, fmt, width, height, basePage * 4096u);
    return cachedSlot;
  }

  // ── iter-442 S4.4 lives just below; this marker keeps the two apart ───────
  // Create + upload (dedicated one-off submit on the frame list, then wait).
  // iter-466 res4k (SR_TEX_SHARE): the GPU-loaded texture's identity - everything the loader and
  // the resource depend on, none of the sampler state or swizzle (those stay per key / SRV).
  uint64_t shareKey = 0;
  TexShared* shared = nullptr;
  if (shmTex && texShareEnabled()) {
    const uint32_t id[12] = {basePage, (f[5] >> 12) & 0xFFFFFu, fmt, uint32_t(dxgi), width, height,
                             pitchTexels, (tiled ? 1u : 0u) | (endian << 1) | (blockW << 4),
                             resMips, f[4] & 0x3FCu, f[5] & 0xE00u, (tsx << 8) | tsy};
    shareKey = ShaderCache::hashUcode(id, sizeof(id)) | 1ull;
    auto sit = m_texShared.find(shareKey);
    if (sit != m_texShared.end() && sit->second.tex) shared = &sit->second;
  }
  Microsoft::WRL::ComPtr<ID3D12Resource> tex;
  uint64_t newBytes = 0;  // step 4 G7 (SR_TEX_CACHE_EVICT): the size of a resource created here
#if SR_ORACLE
  memres::TexOrigin tidCreated = tidLoad;  // the new cache entry's content origin
#endif
  if (shared) {
    tex = shared->tex;
    if (shared->contentHash == contentHash) {
      m_texShareSkips++;
#if SR_ORACLE
      tidCreated = memres::TexOrigin(shared->texIdOrigin);
#endif
    } else {
      if (!uploadLevels(tex.Get(), /*fromSR=*/true)) return 0u;
      shared->contentHash = contentHash;
      m_texShareLoads++;
#if SR_ORACLE
      if (tid.on) shared->texIdOrigin = uint8_t(tidLoad);
#endif
    }
  } else {
  D3D12_HEAP_PROPERTIES def{};
  def.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = width * tsx; td.Height = height * tsy;  // iter-464 resscale
  td.DepthOrArraySize = 1; td.MipLevels = UINT16(resMips);
  td.Format = dxgi; td.SampleDesc.Count = 1;
  // iter-466 res4k round 2 (SR_TEX_LOAD_DIRECT): a scaled single-level texture the loader writes
  // through a UINT view - typeless + UAV (the SRV below keeps the typed format).
  // iter-467 native-b (SR_NB_TARGETS_1X): at 1x too, for a texture over pages a GPU resolve wrote - its next
  // resolve writes it directly (the CPU-loaded copy path takes a typeless destination as it is).
  const bool nbGpuPage = basePage < m_shmGpuWritten.size() && m_shmGpuWritten[basePage];
  if (shmTex && (tsx > 1u || tsy > 1u || (nbGpuPage && pm4NbTargets1xOn())) && resMips == 1u &&
      pm4TexLoadDirectOn()) {
    DXGI_FORMAT tl = DXGI_FORMAT_UNKNOWN;
    if (pm4TexDirectFormat(dxgi, &tl, nullptr, nullptr, nullptr, pm4NbTargetsOn())) {
      td.Format = tl;
      td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
  }
  HRESULT thr = sr::hitch::timed(sr::hitch::kResCreate, [&] {
    return m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
  });
  if (FAILED(thr)) {
    texCreateFailLog("2D CreateCommittedResource", thr, &td, basePage * 4096u, fmt, width, height);
    return failTransient("CreateCommittedResource(tex)", thr);
  }
  if (m_texEvictOn) newBytes = m_device->GetResourceAllocationInfo(0, 1, &td).SizeInBytes;
  {
    wchar_t nm[64];
    swprintf(nm, 64, L"TEX %08X fmt%u %ux%u%s", basePage * 4096u, fmt, width, height,
             tsx > 1 || tsy > 1 ? L" SCALED" : L"");
    tex->SetName(nm);
  }
  if (!uploadLevels(tex.Get(), /*fromSR=*/false))
    return 0u;  // uploadTexture2D already logged; not cached, so it retries
  if (shareKey) {
    TexShared& ns = m_texShared[shareKey];
    ns.tex = tex;
    ns.contentHash = contentHash;
    ns.users = 0;
    ns.allocBytes = newBytes;
    m_texShareLoads++;
    // iter-467 native-b B2: the identity a native-target resolve matches (gfx/native_targets.inc)
    uint32_t pbW = 0;
    uint32_t bl = CL::detail::levelSliceStride(0, true, width, height, pitchTexels, blockW, blockW,
                                               blockBytes, tiled, &pbW);
    if (mipLayout.ok) bl = std::max(bl, mipLayout.faceStrideBytes);
    ns.base = basePage * 4096u;
    ns.baseLen = bl;
    ns.pitch = pitchTexels;
    ns.fmt = fmt;
    ns.endian = endian;
    ns.w = width;
    ns.h = height;
    ns.tsx = tsx;
    ns.tsy = tsy;
    ns.dxgi = dxgi;
    ns.single = resMips == 1u;
    ns.tiled = tiled;
    ns.uav = (td.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
#if SR_ORACLE
    if (tid.on) ns.texIdOrigin = uint8_t(tidLoad);
#endif
    m_texSharedByBase.emplace(ns.base, shareKey);
  }
  }

  uint32_t slot = reuseSlot;
  reuseBack.armed = false;  // the slot is this entry's now
  if (!slot) slot = texSlotAlloc();  // fix2: the one allocator (the exhaustion check above left one available)
  if (!slot) {
    texCreateFailLog("2D no slot after the exhaustion check", S_OK, nullptr, basePage * 4096u, fmt, width, height);
    return 0u;
  }
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = dxgi;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  // The key hashes all six dwords, swizzle included, so one mapping per slot.
  sv.Shader4ComponentMapping = pm4TexMapping(dxgi, f[3]);
  sv.Texture2D.MipLevels = resMips;
  D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr + SIZE_T(slot) * inc};
  publishXlatSrv(tex.Get(), &sv, h);
  // iter-408: the refresh record keeps its own reference, so a later re-upload
  // targets this exact resource without depending on m_texResources indexing
  // (slots are also consumed by the resolve path, so slot != vector index).
  // The refresh record is the SOLE owner of the texture now: the old
  // m_texResources vector only kept resources alive and grew forever, which
  // would defeat eviction (a recycled slot's resource must actually be freed).
  m_texRefresh[key] = TexRefresh{tex, contentHash, slot, ++m_texUseClock, shareKey,
                                  sv.Shader4ComponentMapping, sv.Format};
  if (m_texEvictOn) {  // step 4 G7: the cache's live bytes; a burst of creates or the budget asks for a sweep
    if (!shareKey) m_texRefresh[key].allocBytes = newBytes;
    m_texLiveBytes += newBytes;
    m_texLivePeak = std::max(m_texLivePeak, m_texLiveBytes);
    if (++m_texCreatesSinceSweep >= kTexBurstCreates || (m_texEvictBudget && m_texLiveBytes > m_texEvictBudget))
      m_texSweepWanted = true;
  }
#if SR_ORACLE
  if (tid.on) {
    m_texRefresh[key].texIdOrigin = uint8_t(tidCreated);
    tid.set(tidCreated);
    tid.store = &m_texRefresh[key].texIdClass;
  }
#endif
  if (shareKey) {
    m_texShared[shareKey].users++;
    static uint64_t s_shareLog = 0;
    if (++s_shareLog <= 4 || (s_shareLog % 4096u) == 0u)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_SHARE] %llu keys bound: loads %llu, sibling skips %llu (%zu shared resources)",
                  (unsigned long long)s_shareLog, (unsigned long long)m_texShareLoads,
                  (unsigned long long)m_texShareSkips, m_texShared.size());
  }
#if SR_ORACLE
  // step 4 G7 (lane D): SR_TEX_KEY_LOG=1 (diagnostic, default OFF) - why the cache gains keys at every reload: each new
  // key's fields, whether it is a RE-CREATE (its m_texCache key was erased while its m_texRefresh record stayed), and
  // how many OTHER live entries already cover the same base + byte span (a newer generation at the same address)
  {
    static const bool s_keyLog = [] {
      const char* e = std::getenv("SR_TEX_KEY_LOG");
      return e && e[0] == '1';
    }();
    if (s_keyLog) {
      const uint32_t base = basePage * 4096u, bytes = uint32_t(size_t(rows) * srcPitch);
      uint32_t sameSpan = 0;
      for (const auto& e : m_texEntries)
        if (e.key != key && e.basePhys == base && e.bytes == bytes && m_texCache.count(e.key)) ++sameSpan;
      static std::atomic<uint64_t> s_n{0};
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[TEX_KEY] #%llu new key=%016llX base=%08X bytes=%u fmt=%u %ux%u pitch=%u tiled=%d mipbase=%08X "
                  "f=%08X %08X %08X %08X %08X %08X recreate=%d sameSpanLive=%u slot=%u next=%u entries=%zu "
                  "refresh=%zu (SR_TEX_KEY_LOG)",
                  (unsigned long long)s_n.fetch_add(1) + 1, (unsigned long long)key, base, bytes, fmt, width, height,
                  pitchTexels, tiled ? 1 : 0, ((f[5] >> 12) & 0xFFFFFu) * 4096u, f[0], f[1], f[2], f[3], f[4], f[5],
                  texKeyRecreate ? 1 : 0, sameSpan, slot, m_texNextSlot, m_texEntries.size(), m_texRefresh.size());
    }
  }
#endif
  m_texCache.emplace(key, slot);
  glyphNote(slot, basePage, fmt, width, height, src, srcBytes, pitchTexels, tiled, blockW,
            blockBytes);  // iter-464 prompt icons (iter-465: tiled ones de-tiled)
  m_texEntries.push_back({key, basePage * 4096u, uint32_t(size_t(rows) * srcPitch)});
  if (pm4RtCacheEnabled()) {
    texMarkClean(key, slot, basePage * 4096u, cleanBaseBytes);
    if (mipSrc) texMarkClean(key, slot, ((f[5] >> 12) & 0xFFFFFu) * 4096u, mipLayout.mipsTotalExtentBytes);
  }
  static std::atomic<uint32_t> s_texLog{0};
  if (s_texLog.fetch_add(1) < 200)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[TEX_CACHE] slot %u: fmt=%u %ux%u pitch=%u tiled=%d endian=%u base=0x%08X",
                slot, fmt, width, height, pitchTexels, tiled ? 1 : 0, endian, basePage * 4096u);
  return slot;
}

uint32_t D3D12Backend::getOrCreateTexture2DArray(const uint32_t* f, bool cube) {
  // iter-460 perf: PM4 binds load the layers on the GPU from the shared
  // memory (pm4ShmLoadArray); the CPU layer path below is the fallback.
  if (m_texInline && pm4ShmEnabled() && pm4RtCacheEnabled()) {
    const uint32_t s = pm4ShmLoadArray(f, cube);
    if (s) return s;
  }
  // The layers are copied on the dedicated upload list, which executes BEFORE
  // the frame list: their own uploads must be complete, not inline.
  struct InlineOff {
    bool& f; bool saved;
    explicit InlineOff(bool& v) : f(v), saved(v) { f = false; }
    ~InlineOff() { f = saved; }
  } inlineOff(m_texInline);
  // A cube is always six faces (texture_util.cc:228, GetGuestTextureLayout);
  // a stacked texture has size_2d.stack_depth + 1 layers.
  const uint32_t layers = cube ? 6u : ((f[2] >> 26) & 0x3Fu) + 1u;
  if (layers <= 1u || (f[0] & 3u) != 2u) return 0u;
  const uint32_t fmt = f[1] & 0x3Fu;
  DXGI_FORMAT gdx;
  uint32_t blockW = 1, blockBytes = 4;
  if (!guestFormatToDxgi(fmt, gdx, blockW, blockBytes)) return 0u;
  const uint32_t width = (f[2] & 0x1FFFu) + 1u, height = ((f[2] >> 13) & 0x1FFFu) + 1u;
  uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  if (pitchTexels < width) pitchTexels = width;
  // Layer stride (texture_util.cc:312-352): the base row pitch in blocks and
  // the block rows are both aligned to 32 (tiled AND linear - "mip dimensions
  // are always aligned to 32x32x4 blocks, including for linear textures"),
  // and the slice rounds up to 4 KB.
  const uint32_t pitchBlocks = (pitchTexels + blockW - 1u) / blockW;
  const uint32_t rowPitch = ((pitchBlocks + 31u) & ~31u) * blockBytes;
  const uint32_t blockRows = ((((height + blockW - 1u) / blockW) + 31u) & ~31u);
  const uint32_t layerStride = (rowPitch * blockRows + 4095u) & ~4095u;
  const uint32_t basePage = (f[1] >> 12) & 0xFFFFFu;

  // Decode every layer as a plain 2D texture (stack depth cleared).
  std::vector<ID3D12Resource*> src(layers, nullptr);
  std::vector<uint64_t> hashes(layers, 0);
  for (uint32_t i = 0; i < layers; i++) {
    uint32_t fl[6];
    std::memcpy(fl, f, sizeof(fl));
    fl[1] = (fl[1] & 0xFFFu) | ((basePage + i * (layerStride >> 12)) << 12);
    fl[2] &= ~(0x3Fu << 26);
    if (!getOrCreateTexture2D(fl)) return 0u;
    auto it = m_texRefresh.find(ShaderCache::hashUcode(fl, 24));
    if (it == m_texRefresh.end() || !it->second.tex) return 0u;
    src[i] = it->second.tex.Get();
    hashes[i] = it->second.contentHash;
  }
  const uint64_t key = ShaderCache::hashUcode(f, 24) ^ (cube ? 0x437562654D6170ull   // "CubeMap"
                                                              : 0x5374616B6564ull);  // "Staked"
  TexArray& a = m_texArrays[key];
  if (a.res && a.layerSrc == src && a.layerHash == hashes) return a.slot;  // all layers unchanged
  const DXGI_FORMAT dxgi = src[0]->GetDesc().Format;
  if (!a.res && !pm4TexArrayCreate(a, dxgi, width, height, layers, cube, f[3])) return 0u;
  a.gpu = false;
  a.layerVer.clear();
  // Copy the layers in on the dedicated upload list (the layers were uploaded
  // the same way, so they are complete when this executes).
  if (FAILED(m_texUploadAlloc->Reset()) ||
      FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr)))
    return 0u;
  resourceBarrier(m_texUploadList.Get(), a.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COPY_DEST);
  for (uint32_t i = 0; i < layers; i++) {
    resourceBarrier(m_texUploadList.Get(), src[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = a.res.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.SubresourceIndex = i;
    s.pResource = src[i];
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    m_texUploadList->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    resourceBarrier(m_texUploadList.Get(), src[i], D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  resourceBarrier(m_texUploadList.Get(), a.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_texUploadList->Close();
  ID3D12CommandList* lists[] = {m_texUploadList.Get()};
  m_commandQueue->ExecuteCommandLists(1, lists);
  const UINT64 fv = ++m_texFenceValue;
  m_commandQueue->Signal(m_texFence.Get(), fv);
  if (m_texFence->GetCompletedValue() < fv) {
    m_texFence->SetEventOnCompletion(fv, m_texFenceEvent);
    WaitForSingleObject(m_texFenceEvent, 10000);
  }
  a.layerSrc = src;
  a.layerHash = hashes;
  static std::atomic<uint32_t> s_log{0};
  if (s_log.fetch_add(1) < 32)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[TEX_ARRAY] slot %u: %s fmt=%u %ux%u x%u layers stride=0x%X base=0x%08X swz=%03X",
                a.slot, cube ? "cube" : "stacked", fmt, width, height, layers, layerStride,
                basePage * 4096u, (f[3] >> 1) & 0xFFFu);
  return a.slot;
}

bool D3D12Backend::pm4TexArrayCreate(TexArray& a, DXGI_FORMAT dxgi, uint32_t width,
                                     uint32_t height, uint32_t layers, bool cube,
                                     uint32_t f3, uint32_t levels, bool scaled) {
  if (!texSlotAvailable()) {
    texCreateFailLog(cube ? "cube slots exhausted" : "array slots exhausted", S_OK, nullptr, 0u, 0u, width, height);
    return false;
  }
  D3D12_HEAP_PROPERTIES def{};
  def.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = width;
  td.Height = height;
  td.DepthOrArraySize = UINT16(layers);
  // iter-463 D4b: the fetch constant's level count (pm4ShmLoadArray; xenia
  // d3d12_texture_cache.cc:1361 MipLevels = mip_max_level + 1). 1 = base only.
  td.MipLevels = UINT16(levels ? levels : 1u);
  td.Format = dxgi;
  td.SampleDesc.Count = 1;
  // iter-466 res4k round 2 (SR_TEX_LOAD_DIRECT): a scaled single-level array (the shadow map) the
  // loader writes per slice through a UINT view - typeless + UAV (the SRV keeps the typed format).
  if (scaled && td.MipLevels == 1u && pm4TexLoadDirectOn()) {
    DXGI_FORMAT tl = DXGI_FORMAT_UNKNOWN;
    if (pm4TexDirectFormat(dxgi, &tl, nullptr, nullptr, nullptr, pm4NbTargetsOn())) {
      td.Format = tl;
      td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
  }
  const HRESULT chr = sr::hitch::timed(sr::hitch::kResCreate, [&] {
    return m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&a.res));
  });
  if (FAILED(chr)) {
    texCreateFailLog(cube ? "cube CreateCommittedResource" : "array CreateCommittedResource", chr, &td, 0u, 0u,
                     width, height);
    return false;
  }
  {
    wchar_t nm[64];
    swprintf(nm, 64, L"TEXARR %s %ux%u x%u m%u", cube ? L"cube" : L"stack", width, height, layers,
             unsigned(td.MipLevels));
    a.res->SetName(nm);
  }
  a.slot = texSlotAlloc();
  a.layers = layers;
  a.levels = td.MipLevels;
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = dxgi;
  sv.Shader4ComponentMapping = pm4TexMapping(dxgi, f3);
  if (cube) {
    // D3D12 views any 2D array of >= 6 slices as a cube; face order is the
    // D3D one on both sides (+X -X +Y -Y +Z -Z).
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    sv.TextureCube.MipLevels = a.levels;
  } else {
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = a.levels;
    sv.Texture2DArray.ArraySize = layers;
  }
  publishXlatSrv(
      a.res.Get(), &sv, {m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(a.slot) * inc});
  return true;
}

// ── step 4 G7 (lane D): SR_TEX_CACHE_EVICT ─────────────────────────────────────────────────────────────────────────
void D3D12Backend::texEvictInit() {
  m_texEvictOn = texCacheEvictEnabled();
  if (!m_texEvictOn) return;
  auto num = [](const char* name, uint64_t def) {
    const char* e = std::getenv(name);
    return e && e[0] ? uint64_t(std::strtoull(e, nullptr, 10)) : def;
  };
  m_texEvictAge = num("SR_TEX_CACHE_AGE_FRAMES", 7200);
  m_texEvictBudget = num("SR_TEX_CACHE_BUDGET_MB", 768) << 20;  // the bake: a backstop (see texCacheEvictEnabled)
  m_texSweepFrames = kTexSweepFrames;
  m_texKeepFrames = kTexKeepFrames;
#if SR_ORACLE
  // fix3 (the coordinator, for lane C's forced-eviction replay arm): a short replay sweeps and evicts with e.g.
  // SR_TEX_CACHE_SWEEP_FRAMES=1 + SR_TEX_CACHE_AGE_FRAMES=2, or SR_TEX_CACHE_BUDGET_MB=64 +
  // SR_TEX_CACHE_KEEP_FRAMES=1 (both at least 1: a sweep runs at a frame's first bind, so nothing it takes was bound
  // in that frame)
  m_texSweepFrames = std::max<uint64_t>(1, num("SR_TEX_CACHE_SWEEP_FRAMES", kTexSweepFrames));
  m_texKeepFrames = std::max<uint64_t>(1, num("SR_TEX_CACHE_KEEP_FRAMES", kTexKeepFrames));
#endif
  if (m_texSlotUse.size() != kTex2DSlots) m_texSlotUse.assign(kTex2DSlots, 0);
#if SR_ORACLE
  if (m_texSlotEvicted.size() != kTex2DSlots) m_texSlotEvicted.assign(kTex2DSlots, 0);
#endif
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[TEX_CACHE_EVICT] on: age %llu frames (SR_TEX_CACHE_AGE_FRAMES), budget %llu MB (SR_TEX_CACHE_BUDGET_MB, "
              "0 = none), a sweep every %llu frames or after %u creates, the budget keeps %llu frames, %zu releases "
              "per frame",
              (unsigned long long)m_texEvictAge, (unsigned long long)(m_texEvictBudget >> 20),
              (unsigned long long)m_texSweepFrames, kTexBurstCreates, (unsigned long long)m_texKeepFrames,
              kTexReleasePerFrame);
}

void D3D12Backend::texRegNote(uint64_t key, uint32_t phys, uint64_t len, bool tracked) {
  if (!len || !m_texRefresh.count(key)) return;  // a 2D cache entry's (volumes and arrays keep theirs)
  m_texRegs[key].push_back(TexReg{phys, uint32_t(std::min<uint64_t>(len, UINT32_MAX)), tracked});
}

// THE erase path: every map that knows the key lets go of it, and the bind memo forgets every answer. The resource
// and the slot wait for the GPU (the frame fence of the frame recording now), unless reuseSlotNow takes the slot at
// once for slot exhaustion (the legacy LRU's contract), whose resource still waits for the frame fence.
void D3D12Backend::texEvictKey(uint64_t key, TexEvictWhy why, uint32_t* reuseSlotNow) {
  m_texCache.erase(key);
  m_texClean.erase(key);
#if SR_ORACLE
  m_texCleanFrame.erase(key);
  if (m_texTrackLast == key) m_texTrackLast = 0;
#endif
  m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO: no memo may answer with its slot
  m_texEvictedKeys.push_back(key);  // m_texEntries + m_texPages let go of it in texEvictFinish (one pass per batch)
  if (auto rg = m_texRegs.find(key); rg != m_texRegs.end()) {  // invalidateGuestRange's pages + their marks
    for (const TexReg& r : rg->second) {
      m_texTracked.erase(key ^ (uint64_t(r.phys) << 32) ^ (r.tracked ? 0x7472ull : 0ull));
      const uint32_t lo = (r.phys & 0x1FFFFFFFu) >> 16;
      const uint32_t hi = uint32_t(((uint64_t(r.phys & 0x1FFFFFFFu) + r.len - 1u) >> 16));
      for (uint32_t pg = lo; pg <= hi; pg++) m_texEvictPages.push_back(pg);
    }
    m_texRegs.erase(rg);
  }
  auto rit = m_texRefresh.find(key);
  if (rit == m_texRefresh.end()) return;  // a cached rejection (slot 0) owns nothing
  const uint32_t slot = rit->second.slot;
  uint64_t freed = rit->second.allocBytes;
  Microsoft::WRL::ComPtr<ID3D12Resource> tex = std::move(rit->second.tex);
  if (const uint64_t sk = rit->second.shareKey) {  // iter-466 res4k (SR_TEX_SHARE): the last binder takes it along
    auto sit = m_texShared.find(sk);
    if (sit != m_texShared.end() && (sit->second.users <= 1u || --sit->second.users == 0u)) {
      freed += sit->second.allocBytes;
      texSharedErase(sk);  // (its native-target hold gives the memory back first)
    }
  }
  m_texRefresh.erase(rit);
  m_texLiveBytes -= std::min(m_texLiveBytes, freed);
  m_texEvictCount++;
  m_texEvictBytes += freed;
  m_texEvictWhyN[why]++;
  if (reuseSlotNow) {
    *reuseSlotNow = slot;
    retireResource(std::move(tex));
    return;
  }
  if (slot && slot < m_texSlotUse.size()) {
#if SR_ORACLE
    m_texSlotEvicted[slot] = 1;
#endif
    m_texSlotRetire.retire(slot, texFenceNow(), std::move(tex));
  } else {
    retireResource(std::move(tex));
  }
}

// fix2: one pass over m_texEntries and one remove per touched page for the whole batch (the per-key page scans of
// 5d0c2a63 were quadratic in a page's keys). Every evicting caller finishes before the next registration of a key.
void D3D12Backend::texEvictFinish() {
  if (m_texEvictedKeys.empty()) return;
  const std::unordered_set<uint64_t> gone(m_texEvictedKeys.begin(), m_texEvictedKeys.end());
  m_texEvictedKeys.clear();
  m_texEntries.erase(std::remove_if(m_texEntries.begin(), m_texEntries.end(),
                                    [&](const TexEntry& e) { return gone.count(e.key) != 0; }),
                     m_texEntries.end());
  std::sort(m_texEvictPages.begin(), m_texEvictPages.end());
  m_texEvictPages.erase(std::unique(m_texEvictPages.begin(), m_texEvictPages.end()), m_texEvictPages.end());
  for (uint32_t pg : m_texEvictPages) {
    auto pit = m_texPages.find(pg);
    if (pit == m_texPages.end()) continue;
    auto& v = pit->second;
    const size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](uint64_t k) { return gone.count(k) != 0; }), v.end());
    m_texPageRefs -= std::min<uint64_t>(m_texPageRefs, before - v.size());
    if (v.empty()) m_texPages.erase(pit);
  }
  m_texEvictPages.clear();
}

uint32_t D3D12Backend::texSlotAlloc() {
  const bool fromFree = m_texEvictOn && !m_texFreeSlots.empty();
  const uint32_t s = sr::gfx::texevict::allocSlot(m_texFreeSlots, m_texNextSlot, kTex2DSlots, m_texEvictOn);
  if (fromFree && s) {
    m_texSlotReuses++;
#if SR_ORACLE
    if (s < m_texSlotEvicted.size()) m_texSlotEvicted[s] = 0;
#endif
  }
  return s;
}

bool D3D12Backend::texSlotAvailable() const {
  return (m_texEvictOn && !m_texFreeSlots.empty()) || m_texNextSlot < kTex2DSlots;
}

// The slots the GPU is done with go back to the init's null view (the recipe lets go of the evicted resource) and
// to the free list, at most maxN per call. Debug check (oracle): a slot bound after the fence it waited for is held.
void D3D12Backend::texEvictDrain(size_t maxN) {
  if (!m_texSlotRetire.pending() || !m_fence || !m_xlatSrvHeap) return;
  struct Timed {  // fix3 (Astra 0a384c24 P3): every caller's drain in the worst
    uint64_t& worst;
    int64_t t0 = profQpc();
    ~Timed() { worst = std::max(worst, profUs(profQpc() - t0)); }
  } timed{m_texDrainUsMax};
  const uint64_t completed = m_fence->GetCompletedValue();
  if (completed == UINT64_MAX) return;  // device removed: the frame path reports it
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const SIZE_T heap = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
  const uint64_t violations = m_texSlotRetire.violations();
  m_texSlotRetire.drain(
      completed, maxN,
      [&](uint32_t s) -> uint64_t {
#if SR_ORACLE
        return s < m_texSlotUse.size() ? m_texSlotUse[s] : 0u;
#else
        (void)s;
        return 0u;
#endif
      },
      [&](auto& e) {
        publishXlatSrv(m_xlatWhite2D.Get(), &m_texNullSrv, D3D12_CPU_DESCRIPTOR_HANDLE{heap + SIZE_T(e.slot) * inc});
        if (e.slot < m_glyphIcon.size()) m_glyphIcon[e.slot] = 0;
        e.payload.Reset();
        m_texFreeSlots.push_back(e.slot);
      });
  if (m_texSlotRetire.violations() != violations)
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[TEX_CACHE_EVICT] FENCE CHECK: %llu retired slot(s) were bound after their retire fence (completed "
                "%llu) - held until that bind's frame passes (total %llu)",
                (unsigned long long)(m_texSlotRetire.violations() - violations), (unsigned long long)completed,
                (unsigned long long)m_texSlotRetire.violations());
}

#if SR_ORACLE
void D3D12Backend::texEvictStaleBind(uint32_t slot) {
  if (++m_texStaleBinds <= 16 || m_texStaleBinds % 1000 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[TEX_CACHE_EVICT] STALE BIND #%llu: slot %u was answered after its entry was evicted",
                (unsigned long long)m_texStaleBinds, slot);
}
#endif

// Once per recorded frame, from the bind path (the thread that owns the cache's maps): release what the GPU is done
// with, and sweep every kTexSweepFrames frames or when a create asked for one.
void D3D12Backend::texEvictTick() {
  const uint64_t frame = m_fenceValues[m_frameIndex];
  if (frame == m_texTickFence) return;
  m_texTickFence = frame;
  // fix2: with the pool at its cap and nothing free, 256 a frame (an 8191-slot backlog drains in 32 frames)
  texEvictDrain(m_texFreeSlots.empty() && m_texNextSlot >= kTex2DSlots ? 256u : kTexReleasePerFrame);
  if (m_texSweepWanted || frame - m_texSweepFence >= m_texSweepFrames) texEvictSweep();
}

void D3D12Backend::texEvictSweep() {
  const uint64_t now = texFenceNow();
  m_texSweepFence = m_fenceValues[m_frameIndex];
  const bool wanted = m_texSweepWanted;
  m_texSweepWanted = false;
  const uint32_t creates = m_texCreatesSinceSweep;
  m_texCreatesSinceSweep = 0;
  m_texSweeps++;
  const int64_t t0 = profQpc();
  std::vector<sr::gfx::texevict::Candidate> c;
  c.reserve(m_texRefresh.size());
  uint64_t recent = 0;  // the working set: the resources bound within the last 1800 frames
  std::unordered_set<uint64_t> recentShared;
  for (const auto& [key, rf] : m_texRefresh) {
    if (!rf.slot || rf.slot >= m_texSlotUse.size()) continue;
    // fix2 (Astra P2): by resource - every alias of a shared resource carries its size and identity, and the policy
    // frees the bytes only with the last alias
    uint64_t bytes = rf.allocBytes, group = 0;
    if (rf.shareKey) {
      auto sit = m_texShared.find(rf.shareKey);
      if (sit != m_texShared.end()) {
        bytes = sit->second.allocBytes;
        group = rf.shareKey;
      }
    }
    const uint64_t use = m_texSlotUse[rf.slot];
    if (now - std::min(now, use) < 1800u && (!group || recentShared.insert(group).second)) recent += bytes;
    // the P1 rule: a slot pinned by a draw being assembled is never a sweep victim (none is pinned at a frame's first
    // bind, where sweeps run; the check keeps it so)
    if (std::find(m_texPinned, m_texPinned + m_texPinnedN, rf.slot) != m_texPinned + m_texPinnedN) continue;
    c.push_back({key, use, bytes, group, rf.slot});
  }
  const auto s = sr::gfx::texevict::select(c, now, m_texEvictAge, m_texKeepFrames, m_texLiveBytes, m_texEvictBudget,
                                           kTexSweepMaxEvict);
  for (uint64_t k : s.aged) texEvictKey(k, kTexEvAge);
  for (uint64_t k : s.budget) texEvictKey(k, kTexEvBudget);
  texEvictFinish();
  if (s.capped) m_texSweepWanted = true;  // the rest at the next frame
  const uint64_t us = profUs(profQpc() - t0);
  m_texSweepUsMax = std::max(m_texSweepUsMax, us);
  const size_t evicted = s.aged.size() + s.budget.size();
  static uint32_t s_log = 0;
  if (evicted || wanted || (m_texSweeps % 12u) == 0u) {  // every eviction, and a heartbeat each minute or so
    if (++s_log <= 400 || s_log % 20u == 0u)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_CACHE_EVICT] sweep #%llu%s %.2f ms (worst %.2f, drain worst %.2f): aged %zu (%.1f MB) + budget "
                  "%zu (%.1f MB)%s | live %zu keys %.1f MB "
                  "(peak %.1f), bound in 1800 frames %.1f MB, budget %llu MB | slots next %u free %zu retiring %zu "
                  "reused %llu | creates since %u, evicted total %llu (%.1f MB: age %llu budget %llu exhaust %llu "
                  "recreate %llu resolve %llu)",
                  (unsigned long long)m_texSweeps, wanted ? " (asked)" : "", us / 1000.0, m_texSweepUsMax / 1000.0,
                  m_texDrainUsMax / 1000.0, s.aged.size(), s.agedBytes / 1048576.0, s.budget.size(),
                  s.budgetBytes / 1048576.0, s.capped ? " CAPPED" : "", m_texRefresh.size(), m_texLiveBytes / 1048576.0,
                  m_texLivePeak / 1048576.0, recent / 1048576.0, (unsigned long long)(m_texEvictBudget >> 20),
                  m_texNextSlot, m_texFreeSlots.size(), m_texSlotRetire.pending(),
                  (unsigned long long)m_texSlotReuses, creates, (unsigned long long)m_texEvictCount,
                  m_texEvictBytes / 1048576.0, (unsigned long long)m_texEvictWhyN[kTexEvAge],
                  (unsigned long long)m_texEvictWhyN[kTexEvBudget], (unsigned long long)m_texEvictWhyN[kTexEvExhaust],
                  (unsigned long long)m_texEvictWhyN[kTexEvRecreate], (unsigned long long)m_texEvictWhyN[kTexEvResolve]);
  }
}

// [TEX_CREATE_FAIL]: a texture the cache could not create, whatever the cause (an HRESULT, or no slot), with its size.
void D3D12Backend::texCreateFailLog(const char* what, HRESULT hr, const D3D12_RESOURCE_DESC* td, uint32_t base,
                                    uint32_t fmt, uint32_t w, uint32_t h) {
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 64 && n % 100u != 0u) return;
  uint64_t bytes = 0;
  if (td && m_device) bytes = m_device->GetResourceAllocationInfo(0, 1, td).SizeInBytes;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[TEX_CREATE_FAIL] #%u %s hr=0x%08X %llux%u x%u mips=%u dxgi=%u bytes=%llu (%.1f MB) guest %ux%u fmt=%u "
              "base=%08X | cache keys %zu live %.1f MB next slot %u free %zu evict=%d",
              n, what, unsigned(hr), td ? (unsigned long long)td->Width : 0ull, td ? td->Height : 0u,
              td ? unsigned(td->DepthOrArraySize) : 0u, td ? unsigned(td->MipLevels) : 0u,
              td ? unsigned(td->Format) : 0u, (unsigned long long)bytes, bytes / 1048576.0, w, h, fmt, base,
              m_texRefresh.size(), m_texLiveBytes / 1048576.0, m_texNextSlot, m_texFreeSlots.size(),
              m_texEvictOn ? 1 : 0);
}

void D3D12Backend::texMarkClean(uint64_t key, uint32_t slot, uint32_t phys, uint64_t bytes) {
  m_texClean[key] = slot;
  m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO
  // iter-466 cp lane (SR_SHM_WT_TEXCLEAN): the shared-memory 2D path armed m_texTrack with the
  // ranges its loader read and their version. Every page still protected and no page changed
  // since (version) -> the mark survives the present; anything else -> dropped at the present.
#if SR_ORACLE
  if (texCleanTrackedOn()) {
    bool tracked = false;
    if (m_texTrack.armed) {
      const TexTrack t = m_texTrack;
      m_texTrack.armed = false;  // (the mip call of the same texture inherits the decision)
      uint32_t v = pm4ShmVersion(t.base, t.baseLen);
      if (t.mipLen) v = std::max(v, pm4ShmVersion(t.mip, t.mipLen));
      tracked = v == t.version && texRangeProtected(t.base, t.baseLen) &&
                (!t.mipLen || texRangeProtected(t.mip, t.mipLen));
      if (tracked) {  // the loader's full ranges invalidate it, not only the decode span
        for (const auto& r : {std::make_pair(t.base, t.baseLen), std::make_pair(t.mip, t.mipLen)}) {
          if (!r.second || !m_texTracked.insert(key ^ (uint64_t(r.first) << 32) ^ 0x7472ull).second) continue;
          if (m_texEvictOn) texRegNote(key, r.first, r.second, true);
          const uint32_t lo2 = (r.first & 0x1FFFFFFFu) >> 16;
          const uint32_t hi2 = uint32_t(((uint64_t(r.first & 0x1FFFFFFFu) + r.second - 1u) >> 16));
          for (uint32_t pg = lo2; pg <= hi2; pg++) m_texPages[pg].push_back(key);
          m_texPageRefs += hi2 - lo2 + 1u;
        }
        m_texTrackedMarks++;
        m_texCleanFrame.erase(key);
      }
      m_texTrackLast = tracked ? key : 0u;
    } else {
      tracked = m_texTrackLast == key;  // the second (mip) mark of a tracked texture
    }
    if (!tracked) {
      m_texCleanFrame.insert(key);
      m_texFrameMarks++;
    }
  }
#endif
  if (!bytes || !m_texTracked.insert(key ^ (uint64_t(phys) << 32)).second) return;
  if (m_texEvictOn) texRegNote(key, phys, bytes, false);
  const uint32_t lo = (phys & 0x1FFFFFFFu) >> 16;
  const uint32_t hi = uint32_t(((uint64_t(phys & 0x1FFFFFFFu) + bytes - 1u) >> 16));
  for (uint32_t pg = lo; pg <= hi; pg++) m_texPages[pg].push_back(key);
  m_texPageRefs += hi - lo + 1u;
}

#if SR_ORACLE
bool D3D12Backend::texCleanTrackedOn() const {
  static const bool on = sr::kernel::gww::enabled() && cpsimd::gateLogged("SR_SHM_WT_TEXCLEAN", true);
  return on;
}

bool D3D12Backend::texRangeProtected(uint32_t phys, uint32_t len) const {
  if (!len) return true;
  const uint32_t p0 = (phys & 0x1FFFFFFFu) >> 12;
  const uint32_t p1 = uint32_t(((uint64_t(phys & 0x1FFFFFFFu) + len - 1u) >> 12));
  for (uint32_t p = p0; p <= p1; p++)
    if (!sr::kernel::gww::protGen(p)) return false;
  return true;
}

#endif
// The present's texture invalidation under SR_SHM_WT_TEXCLEAN: the frame-clean marks, then
// every texture registered on a block that released protected pages since the last present.
void D3D12Backend::invalidateTexturesAtPresent() {
  m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO
#if SR_ORACLE
  for (uint64_t k : m_texCleanFrame) m_texClean.erase(k);
  m_texCleanFrame.clear();
#endif
#if SR_ORACLE
  if (memres::resourceMode()) memres::resourceMemory().takeDirtyBlocks(m_texReleasedTmp);
  else sr::kernel::gww::takeReleasedBlocks(m_texReleasedTmp);
  static_assert(memres::ResourceMemory::kDirtyBlockLog2 == sr::kernel::gww::kBlockLog2);
  if (memres::resourceAbDual()) {  // the frozen-pass A/B: the union of both arms' invalidations (it never decides)
    static thread_local std::vector<uint32_t> s_gwwBlocks;
    sr::kernel::gww::takeReleasedBlocks(s_gwwBlocks);
    m_texReleasedTmp.insert(m_texReleasedTmp.end(), s_gwwBlocks.begin(), s_gwwBlocks.end());
  }
#else
  memres::resourceMemory().takeDirtyBlocks(m_texReleasedTmp);
#endif
  for (uint32_t b : m_texReleasedTmp) {
#if SR_ORACLE
    invalidateGuestRange(b << (12 + sr::kernel::gww::kBlockLog2), 4096ull << sr::kernel::gww::kBlockLog2);
#else
    invalidateGuestRange(b << (12 + memres::ResourceMemory::kDirtyBlockLog2),
                         4096ull << memres::ResourceMemory::kDirtyBlockLog2);
#endif
    m_texBlockInvals++;
  }
#if SR_ORACLE
  m_texTrackLast = 0;
#endif
}

void D3D12Backend::invalidateGuestRange(uint32_t phys, uint64_t bytes) {
  if (!bytes || m_texClean.empty()) return;
  const uint32_t lo = (phys & 0x1FFFFFFFu) >> 16;
  const uint32_t hi = uint32_t(((uint64_t(phys & 0x1FFFFFFFu) + bytes - 1u) >> 16));
  for (uint32_t pg = lo; pg <= hi; pg++) {
    auto it = m_texPages.find(pg);
    if (it == m_texPages.end()) continue;
    for (uint64_t k : it->second) m_texClean.erase(k);
    m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO
  }
}

uint32_t D3D12Backend::getOrCreateTexture3D(const uint32_t* f) {
  if ((f[0] & 3u) != 2u || ((f[5] >> 9) & 3u) != 2u) return 0u;
  const uint64_t key = ShaderCache::hashUcode(f, 24) ^ 0x566F6C756D65ull;  // "Volume"
  m_texBindFast = false;  // iter-467 native-b SR_NB_BIND_MEMO
  {  // iter-460 perf: nothing it reads changed - one lookup (see the 2D path)
    auto cit = m_texClean.find(key);
    if (cit != m_texClean.end() && cit->second) {
      m_texBindFast = true;
      return cit->second;
    }
  }
  const uint32_t fmt = f[1] & 0x3Fu, endian = (f[1] >> 6) & 3u;
  DXGI_FORMAT dxgi;
  uint32_t blockW = 1, blockBytes = 4;
  if (!guestFormatToDxgi(fmt, dxgi, blockW, blockBytes)) return 0u;
  const uint32_t width = (f[2] & 0x7FFu) + 1u, height = ((f[2] >> 11) & 0x7FFu) + 1u,
                 depth = ((f[2] >> 22) & 0x3FFu) + 1u;  // size_3d
  const bool tiled = ((f[0] >> 31) & 1u) != 0;
  const uint32_t basePage = (f[1] >> 12) & 0xFFFFFu;
  if (!basePage) return 0u;
  uint32_t pitchTexels = ((f[0] >> 22) & 0x1FFu) << 5;
  if (pitchTexels < width) pitchTexels = width;
  // Step-4 lane C, from iter-467 p2 (research/p2 5007e489): SR_TEX3D_MIPS - the whole mip chain, placed by Xenia's
  // GetGuestTextureLayout for k3D (gfx/volume_layout.hpp), with the base read to its data EXTENT rather than the
  // slice stride. The base-only path below loads level 0 (MipLevels 1) and reads the stride, which ends before a
  // tiled 64^3 k_8_8_8_8 volume's extent (1,048,576 < 1,050,752 bytes): its tail texels stay zero, and a minified
  // noise volume (car paint, building windows) aliases. Baked ON with SR_XLAT_SAMPLER_W after the replay matrix (on
  // top of W: gp1 0.664 -> 0.485, gp2 0.479 -> 0.392 vs Xenia); =0 = base level only (dev A/B; the release has no
  // =0 arm - the base-only path stays its loader for a volume the mips path refuses).
#if SR_ORACLE
  static const bool s_tex3dMips = [] {
    const char* e = std::getenv("SR_TEX3D_MIPS");
    return !e || e[0] != '0';
  }();
#else
  constexpr bool s_tex3dMips = true;
#endif
  if (s_tex3dMips) {
    namespace VL = sr::gfx::volume;
    const uint32_t blockLog2 = blockW == 4u ? 2u : 0u;
    uint32_t bpbLog2 = 0;
    while ((1u << bpbLog2) < blockBytes) bpbLog2++;
    // texture_util.cc GetSubresourcesFromFetchConstant: no mip page = level 0 only; else
    // min(mip_max_level, log2(the longest axis, depth included)).
    const uint32_t mipPage = (f[5] >> 12) & 0xFFFFFu;
    const bool packed = ((f[5] >> 11) & 1u) != 0;
    const uint32_t sizeMax = VL::log2Floor(std::max(std::max(width, height), depth));
    // A kBaseMap fetch (dword_3 bits 23-24 mip_filter == 2) samples level 0 only: Xenia's sampler sets MaxLOD to
    // MinLOD (+0.25) for it, and our sampler table has no such variant, so the resource carries level 0 alone. Its
    // fetch constant is its own cache key, so the base-only volume never aliases the mipped one.
    const bool baseMap = ((f[3] >> 23) & 3u) == 2u;
    uint32_t maxLevel = (mipPage && !baseMap) ? std::min((f[4] >> 6) & 0xFu, sizeMax) : 0u;
    if (sr::gfx::volume::samplerV2On()) {
      // SR_XLAT_SAMPLER_V2 (P2-5): the chain stays - kBaseMap is the sampler's (MaxLOD = MinLOD + 0.25,
      // xlatVolumeSamplerIndex), as in Xenia, which keeps the levels because a fetch instruction may override the
      // mip filter. GetSubresourcesFromFetchConstant: mip_max_level = max(min(mip_max_level, size max),
      // min(mip_min_level, size max)) when there is a mip page, else 0.
      const uint32_t mipMinX = std::min((f[4] >> 2) & 0xFu, sizeMax);
      maxLevel = mipPage ? std::max(std::min((f[4] >> 6) & 0xFu, sizeMax), mipMinX) : 0u;
    }
    VL::Layout L = VL::computeLayout(width, height, depth, pitchTexels, tiled, blockLog2, blockBytes, packed,
                                     maxLevel);
    const uint32_t basePhys = basePage * 4096u, mipPhys = mipPage * 4096u;
    // A replayed trace that never supplied the mip pages (our own live traces, recorded before this path): those
    // pages are the replay's never-written zeros, so the base level alone is closest to what the live game drew
    // (the TEX_ARRAY_MIPS rule in pm4ShmLoadArray). SR_REPLAY_MIPS_SUPPLIED=0 loads them anyway.
    // SR_TEX3D_V3 (Astra round-2 P2 on the late mip arrival): the chain this fetch asks for, kept when the replay
    // fallback below drops it - its pages stay registered for invalidation, so the mip pages arriving later (a
    // trace write or a resolve) drop the clean mark and the next bind reloads the volume with its chain (the
    // level-count change recreates it, P2-6). V2 alone registered the base only, so the base-only volume stayed
    // clean for good.
    uint32_t unsuppliedMipSpan = 0;
    if (L.ok && L.maxLevel && !replaySupplied(mipPhys, L.mipsTotalExtentBytes)) {
      if (sr::gfx::volume::tex3dV3On()) unsuppliedMipSpan = L.mipsTotalExtentBytes;
      static std::atomic<uint32_t> s_unsup3{0};
      if (s_unsup3.fetch_add(1, std::memory_order_relaxed) < 16)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[TEX_VOLUME] replay: mips 0x%08X+0x%X not in the trace -> base level only (fmt=%u %ux%ux%u)",
                    mipPhys, L.mipsTotalExtentBytes, fmt, width, height, depth);
      L = VL::computeLayout(width, height, depth, pitchTexels, tiled, blockLog2, blockBytes, packed, 0u);
    }
    const uint32_t baseSpan = L.ok ? L.base.extentBytes : 0u;
    const uint32_t mipSpan = (L.ok && L.maxLevel) ? L.mipsTotalExtentBytes : 0u;
    const bool mapped = L.ok && baseSpan && baseSpan <= 0x4000000u && gpuPhysToHost(basePhys) &&
                        gpuPhysToHost(basePhys + baseSpan - 1u) &&
                        (!mipSpan || (mipSpan <= 0x4000000u && gpuPhysToHost(mipPhys) &&
                                      gpuPhysToHost(mipPhys + mipSpan - 1u)));
    if (mapped) {
      TexVolume& v = m_texVolumes[key];
      const uint8_t* bsrc = gpuPhysToHost(basePhys);
      const uint8_t* msrc = mipSpan ? gpuPhysToHost(mipPhys) : nullptr;
      std::vector<uint8_t> nativeBase, nativeMips;
      if (pm4ShmEnabled() || memres::resourceMode()) {  // as the base-only path below reads them
        if (memres::resourceDecides()) {
          resourceCpuSnapshot(basePhys, baseSpan, nativeBase);
          bsrc = nativeBase.data();
          if (mipSpan) {
            resourceCpuSnapshot(mipPhys, mipSpan, nativeMips);
            msrc = nativeMips.data();
          }
#if SR_ORACLE
        } else {
          pm4ShmSyncToGuest(basePhys, baseSpan);
          if (mipSpan) pm4ShmSyncToGuest(mipPhys, mipSpan);
#endif
        }
      }
      livetrace::memRead(basePhys, bsrc, baseSpan);  // iter-460 live trace: what this loader reads
      if (mipSpan) livetrace::memRead(mipPhys, msrc, mipSpan);
      // The content key of both ranges, as the 2D path keys base + mips.
      uint64_t hash = 0;
#if SR_ORACLE
      if (!memres::resourceDecides()) {
        hash = ShaderCache::hashUcode(bsrc, baseSpan);
        if (mipSpan) hash ^= ShaderCache::hashUcode(msrc, mipSpan) * 0x9E3779B97F4A7C15ull;
      } else
#endif
      {
        uint32_t version = pm4ShmVersion(basePhys, baseSpan);
        if (mipSpan) version = std::max(version, pm4ShmVersion(mipPhys, mipSpan));
        hash = (1ull << 63) | version;
      }
      const bool v2 = sr::gfx::volume::tex3dV2On();
      if (v.res && v.hash == hash && (!v2 || v.levels == L.maxLevel + 1u)) {
        texMarkClean(key, v.slot, basePhys, baseSpan);
        if (mipSpan) texMarkClean(key, v.slot, mipPhys, mipSpan);
        else if (unsuppliedMipSpan) texMarkClean(key, v.slot, mipPhys, unsuppliedMipSpan);
        return v.slot;
      }
      // SR_TEX3D_V2 (P2-2): the endian swap works on GUEST words - Xenia's load shaders swap the 32-bit words of
      // guest memory and then pick the texels - so a block narrower than a word (k_16 / k_8_8 / k_8 levels) and a
      // tail shorter than a word convert too. The old order (swap each 4 bytes of the decoded output) skipped a
      // sub-dword tail and, for 8in32 on 16-bit texels, paired texels in output order.
      std::vector<uint8_t> laneBase, laneMips;
      const bool laneSwap = v2 && endian != 0u;
      if (laneSwap) {
        // SR_TEX3D_V3: by the format's block size, as Xenia's loaders (volume_layout.hpp swapGuestUnits); V2 alone
        // swaps 32-bit guest words for every format.
        const bool v3 = sr::gfx::volume::tex3dV3On();
        if (v3) VL::swapGuestUnits(bsrc, baseSpan, endian, blockBytes, laneBase);
        else VL::swapGuestWords(bsrc, baseSpan, endian, laneBase);
        bsrc = laneBase.data();
        if (msrc) {
          if (v3) VL::swapGuestUnits(msrc, mipSpan, endian, blockBytes, laneMips);
          else VL::swapGuestWords(msrc, mipSpan, endian, laneMips);
          msrc = laneMips.data();
        }
      }
      // Decode every level: Tiled3D (or linear rows) over the storage level that holds it, at its packed-tail
      // origin when it lives in one (texture_util.cc GetPackedMipOffset).
      struct VolLevel {
        std::vector<uint8_t> lin;
        uint32_t w = 1, h = 1, d = 1, rows = 0, rowBytes = 0;
      };
      std::vector<VolLevel> levels(L.maxLevel + 1u);
      uint32_t clipped = 0;
      bool placedAll = true;
      for (uint32_t m = 0; m <= L.maxLevel; m++) {
        const VL::Placement P = VL::placeLevel(L, width, height, depth, blockLog2, m);
        if (!P.ok) {
          placedAll = false;
          break;
        }
        const uint8_t* region = P.inBasePage ? bsrc : msrc;
        const uint32_t regionSpan = P.inBasePage ? baseSpan : mipSpan;
        if (!region) {
          placedAll = false;
          break;
        }
        VolLevel& lv = levels[m];
        lv.w = P.width;
        lv.h = P.height;
        lv.d = P.depth;
        const uint32_t wB = (P.width + blockW - 1u) / blockW, hB = (P.height + blockW - 1u) / blockW;
        lv.rows = hB;
        lv.rowBytes = wB * blockBytes;
        lv.lin.assign(size_t(lv.rowBytes) * hB * P.depth, 0u);
        for (uint32_t z = 0; z < P.depth; z++)
          for (uint32_t by = 0; by < hB; by++)
            for (uint32_t bx = 0; bx < wB; bx++) {
              const int64_t off = int64_t(P.byteOffset) +
                                  VL::blockOffset(*P.storage, tiled, bpbLog2, P.x0 + bx, P.y0 + by, P.z0 + z);
              if (off < 0 || uint64_t(off) + blockBytes > regionSpan) {
                clipped++;
                continue;
              }
              std::memcpy(lv.lin.data() + ((size_t(z) * hB + by) * wB + bx) * blockBytes, region + off,
                          blockBytes);
            }
        if (endian && !laneSwap) {
          uint32_t* q = reinterpret_cast<uint32_t*>(lv.lin.data());
          for (size_t i = 0; i < lv.lin.size() / 4; i++) q[i] = xeSwap32(q[i], endian);
        }
      }
      if (placedAll) {
        const uint32_t nLevels = L.maxLevel + 1u;
        // SR_TEX3D_V2 (P2-6): a resource made with another level count (the base-only path's one level, or a
        // replay that supplied the mip pages only later) is recreated with this chain - a FRESH slot, the old
        // resource retired against the frame fence (in-flight lists may still sample the old descriptor) - instead
        // of uploading min(levels) and caching this content as complete.
        if (v2 && v.res && v.levels != nLevels && texSlotAvailable()) {
          SR_LOG_BOOT(sr::LogSeverity::Info, "[TEX_VOLUME] slot %u: %u -> %u levels, recreated", v.slot, v.levels,
                      nLevels);
          retireResource(v.res);
          v.res.Reset();
          v.hash = 0;
        }
        const bool created = !v.res;
        if (created) {
          if (!texSlotAvailable()) {
            texCreateFailLog("volume slots exhausted", S_OK, nullptr, basePage * 4096u, fmt, width, height);
            m_texVolumes.erase(key);
            return 0u;
          }
          D3D12_HEAP_PROPERTIES def{};
          def.Type = D3D12_HEAP_TYPE_DEFAULT;
          D3D12_RESOURCE_DESC td{};
          td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
          td.Width = width;
          td.Height = height;
          td.DepthOrArraySize = UINT16(depth);
          td.MipLevels = UINT16(nLevels);
          td.Format = dxgi;
          td.SampleDesc.Count = 1;
          const HRESULT vhr = sr::hitch::timed(sr::hitch::kResCreate, [&] {
            return m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&v.res));
          });
          if (FAILED(vhr)) {
            texCreateFailLog("volume CreateCommittedResource", vhr, &td, basePage * 4096u, fmt, width, height);
            m_texVolumes.erase(key);
            return 0u;
          }
          v.slot = texSlotAlloc();
          v.levels = nLevels;
          D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
          sv.Format = dxgi;
          sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
          sv.Shader4ComponentMapping = pm4TexMapping(dxgi, f[3]);
          sv.Texture3D.MipLevels = nLevels;
          const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
          publishXlatSrv(v.res.Get(), &sv,
                         {m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(v.slot) * inc});
        }
        // A resource the base-only path created earlier (a refused call of this key) keeps its own level count:
        // only the levels it has are written.
        const uint32_t upLevels = std::min(nLevels, v.levels);
        for (uint32_t m = 0; m < upLevels; m++) {
          const VolLevel& lv = levels[m];
          const uint32_t fw = (lv.w + blockW - 1u) / blockW * blockW, fh = (lv.h + blockW - 1u) / blockW * blockW;
          if (!uploadTexture2D(v.res.Get(), lv.lin.data(), lv.rows * lv.d, lv.rowBytes, fw, fh, dxgi,
                               /*fromShaderResource=*/!created || m > 0u, lv.d, m))
            return (created && m == 0u) ? 0u : v.slot;
        }
        v.hash = hash;
        texMarkClean(key, v.slot, basePhys, baseSpan);
        if (mipSpan) texMarkClean(key, v.slot, mipPhys, mipSpan);
        else if (unsuppliedMipSpan) texMarkClean(key, v.slot, mipPhys, unsuppliedMipSpan);
#if SR_ORACLE
        holdNoteTex('V', basePhys, fmt, width, height);  // 2i diagnostic (SR_DIAG_HOLD_LOG)
#endif
        static std::atomic<uint32_t> s_log3m{0};
        if (s_log3m.fetch_add(1, std::memory_order_relaxed) < 32)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[TEX_VOLUME] mips slot %u: fmt=%u %ux%ux%u tiled=%d levels=%u/%u packed=%u base=0x%08X+%u "
                      "mips=0x%08X+%u clipped=%u",
                      v.slot, fmt, width, height, depth, tiled ? 1 : 0, upLevels, nLevels, L.packedLevel,
                      basePhys, baseSpan, mipPhys, mipSpan, clipped);
        return v.slot;
      }
      if (!v.res) m_texVolumes.erase(key);
    }
    static std::atomic<uint32_t> s_refuse3m{0};
    if (s_refuse3m.fetch_add(1, std::memory_order_relaxed) < 16)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[TEX_VOLUME] mips refused (layout ok=%d mapped=%d) fmt=%u %ux%ux%u base=0x%08X mips=0x%08X -> "
                  "base level only",
                  L.ok ? 1 : 0, mapped ? 1 : 0, fmt, width, height, depth, basePhys, mipPhys);
  }
  // GetGuestTextureLayout (texture_util.cc:312-352) for the base level: row
  // pitch = pitch aligned to 32 blocks, z slice = row pitch x block rows
  // aligned to 32, the volume = slices x depth aligned to 4 (kTextureTileDepth).
  const uint32_t rowBlocks = (width + blockW - 1u) / blockW, rows = (height + blockW - 1u) / blockW;
  const uint32_t pitchBlocks = (((pitchTexels + blockW - 1u) / blockW) + 31u) & ~31u;
  const uint32_t rowsAligned = (rows + 31u) & ~31u;
  const uint64_t rowPitch = uint64_t(pitchBlocks) * blockBytes;
  const uint64_t sliceBytes = rowPitch * rowsAligned;
  const uint64_t span = sliceBytes * ((depth + 3u) & ~3u);
  const uint8_t* src = gpuPhysToHost(basePage * 4096u);
  if (!src || span > 0x4000000ull || !gpuPhysToHost(basePage * 4096u + uint32_t(span) - 1u)) return 0u;
  TexVolume& v = m_texVolumes[key];
  std::vector<uint8_t> nativeVolume;
  if (pm4ShmEnabled() || memres::resourceMode()) {
    if (memres::resourceDecides()) {
      resourceCpuSnapshot(basePage * 4096u, uint32_t(span), nativeVolume);
      src = nativeVolume.data();
#if SR_ORACLE
    } else {
      pm4ShmSyncToGuest(basePage * 4096u, uint32_t(span));
#endif
    }
  }
  livetrace::memRead(basePage * 4096u, src, uint32_t(span));  // iter-460 live trace
#if SR_ORACLE
  const uint64_t hash = memres::resourceDecides()
      ? (1ull << 63) | pm4ShmVersion(basePage * 4096u, uint32_t(span))
      : ShaderCache::hashUcode(src, size_t(span));
#else
  const uint64_t hash = (1ull << 63) | pm4ShmVersion(basePage * 4096u, uint32_t(span));
#endif
  if (v.res && v.hash == hash) {
    texMarkClean(key, v.slot, basePage * 4096u, span);
    return v.slot;
  }

  // Untile (Tiled3D over the block grid, texture_address.h) or copy linear rows.
  const uint32_t rowBytes = rowBlocks * blockBytes;
  std::vector<uint8_t> lin(size_t(rowBytes) * rows * depth);
  uint32_t bpbLog2 = 0;
  while ((1u << bpbLog2) < blockBytes) bpbLog2++;
  for (uint32_t z = 0; z < depth; z++)
    for (uint32_t by = 0; by < rows; by++) {
      uint8_t* drow = lin.data() + (size_t(z) * rows + by) * rowBytes;
      if (!tiled) {
        std::memcpy(drow, src + z * sliceBytes + by * rowPitch, rowBytes);
        continue;
      }
      for (uint32_t bx = 0; bx < rowBlocks; bx++) {
        const int64_t off = xenos_tiling::tiled3D(int32_t(bx), int32_t(by), int32_t(z), pitchBlocks,
                                                  rowsAligned, bpbLog2);
        if (off >= 0 && uint64_t(off) + blockBytes <= span)
          std::memcpy(drow + size_t(bx) * blockBytes, src + off, blockBytes);
      }
    }
  if (endian) {
    uint32_t* p = reinterpret_cast<uint32_t*>(lin.data());
    for (size_t i = 0; i < lin.size() / 4; i++) p[i] = xeSwap32(p[i], endian);
  }
  if (texDumpEnabled() && blockW == 1 && blockBytes == 4) {
    // SR_TEX_DUMP: the first four z slices side by side, raw RGBA -> BGRA.
    const uint32_t n = depth < 4u ? depth : 4u;
    std::vector<uint8_t> bgra(size_t(width) * n * height * 4u);
    for (uint32_t z = 0; z < n; z++)
      for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
          const uint8_t* sp = lin.data() + ((size_t(z) * rows + y) * width + x) * 4u;
          uint8_t* dp = bgra.data() + (size_t(y) * width * n + size_t(z) * width + x) * 4u;
          dp[0] = sp[2]; dp[1] = sp[1]; dp[2] = sp[0]; dp[3] = sp[3];
        }
    char nm[128];
    snprintf(nm, sizeof(nm), "texdump_vol_fmt%u_%ux%ux%u_0x%08X.bmp", fmt, width, height, depth,
             basePage * 4096u);
    writeBmp32(nm, bgra.data(), width * n, height, width * n * 4u);
  }
  const bool created = !v.res;
  if (created) {
    if (!texSlotAvailable()) {
      texCreateFailLog("volume slots exhausted", S_OK, nullptr, basePage * 4096u, fmt, width, height);
      m_texVolumes.erase(key);
      return 0u;
    }
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = UINT16(depth);
    td.MipLevels = 1;
    td.Format = dxgi;
    td.SampleDesc.Count = 1;
    const HRESULT vhr = sr::hitch::timed(sr::hitch::kResCreate, [&] {
      return m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&v.res));
    });
    if (FAILED(vhr)) {
      texCreateFailLog("volume CreateCommittedResource", vhr, &td, basePage * 4096u, fmt, width, height);
      m_texVolumes.erase(key);
      return 0u;
    }
    v.slot = texSlotAlloc();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = dxgi;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = pm4TexMapping(dxgi, f[3]);
    sv.Texture3D.MipLevels = 1;
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    publishXlatSrv(
        v.res.Get(), &sv, {m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(v.slot) * inc});
  }
  if (!uploadTexture2D(v.res.Get(), lin.data(), rows * depth, rowBytes, width, height, dxgi,
                       /*fromShaderResource=*/!created, depth))
    return created ? 0u : v.slot;
  v.hash = hash;
  texMarkClean(key, v.slot, basePage * 4096u, span);
  static std::atomic<uint32_t> s_log{0};
  if (s_log.fetch_add(1) < 32)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[TEX_VOLUME] slot %u: fmt=%u %ux%ux%u tiled=%d endian=%u base=0x%08X swz=%03X", v.slot,
                fmt, width, height, depth, tiled ? 1 : 0, endian, basePage * 4096u, (f[3] >> 1) & 0xFFFu);
  return v.slot;
}

namespace {
// Xenos BlendFactor (xenos.h:747) → D3D12_BLEND.
D3D12_BLEND xeBlendFactor(uint32_t f) {
  switch (f) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}
D3D12_BLEND_OP xeBlendOp(uint32_t op) {
  switch (op) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}
// iter-370: the kPointList sprite-expansion geometry shader, compiled offline
// by tools/build_point_gs.ps1 (see tools/xenosrecomp/point_sprite.gs.hlsl).
// Loaded once; an empty blob means point draws stay on the fallback path rather
// than silently rendering un-expanded 1-pixel points.
const std::vector<uint8_t>& pointSpriteGs() {
  static const std::vector<uint8_t> blob = [] {
    std::vector<uint8_t> b;
    // render/shader-coverage: the GS sits beside the installed cache's dxil folder, found the way the cache is
    // (ShaderCache::shippedDataDir: SR_SHADER_CACHE_DIR, else the exe folder's data/shader_cache/dxil, else the
    // working folder's). The old rule read only the working folder, so a player package started from elsewhere -
    // or a roll pointed at a package's cache - had no GS and the release stopped at its first point-list draw
    // (missing-pso, lF_R4b).
    const std::string path = (ShaderCache::resolveInstalledDir() / ".." / "point_sprite.gs.dxil").string();
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      const long n = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      if (n > 0) {
        b.resize(size_t(n));
        if (std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
      }
      std::fclose(f);
    }
    SR_LOG_BOOT(b.empty() ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
                "[D3D12_XLAT] point-sprite GS %s (%s, %zu bytes)",
                b.empty() ? "MISSING - point draws will not translate" : "loaded",
                path.c_str(), b.size());
    return b;
  }();
  return blob;
}
}  // namespace

// iter-447: refuse-by-name needs a real answer, not a guess at which Xenos
// formats happen to be renderable on this adapter. One CheckFeatureSupport per
// distinct format, cached.
bool D3D12Backend::formatRenderable(DXGI_FORMAT f) {
  if (f == DXGI_FORMAT_UNKNOWN) return false;
  auto it = m_fmtRenderable.find((uint32_t)f);
  if (it != m_fmtRenderable.end()) return it->second;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
  fs.Format = f;
  const bool ok =
      SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs,
                                              sizeof(fs))) &&
      (fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0;
  m_fmtRenderable.emplace((uint32_t)f, ok);
  return ok;
}

// Input layout for a translated VS (shared by getXlatPso and getPm4Pso).
static UINT xlatInputLayout(const CachedShader* vs, D3D12_INPUT_ELEMENT_DESC* elems) {
  UINT n = 0;
  if (!vs->signature.empty()) {
    // iter-359h: the layout must cover EVERY element of the VS DXIL input
    // signature or CreateGraphicsPipelineState fails E_INVALIDARG (4 menu
    // shaders declare inputs no vfetch reads — e.g. POSITION0 declared but
    // unused). Data offsets come from the matching sidecar element (the CPU
    // decoder packs attribute i at i*16); declared-but-unused inputs read
    // dummy bytes at offset 0.
    for (const auto& s : vs->signature) {
      if (n >= 32) break;
      elems[n].SemanticName = s.name.c_str();
      elems[n].SemanticIndex = s.index;
      elems[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      elems[n].InputSlot = 0;
      UINT off = 0;
      for (size_t i = 0; i < vs->layout.size(); i++) {
        if (vs->layout[i].usageIndex == s.index &&
            s.name == xeUsageSemantic(vs->layout[i].usage)) {
          off = UINT(i) * 16;
          break;
        }
      }
      elems[n].AlignedByteOffset = off;
      elems[n].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      n++;
    }
  } else {
    // No parsable signature (unexpected) — legacy sidecar-order layout.
    for (const auto& e : vs->layout) {
      if (n >= 16) break;
      elems[n].SemanticName = xeUsageSemantic(e.usage);
      elems[n].SemanticIndex = e.usageIndex;
      elems[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      elems[n].InputSlot = 0;
      elems[n].AlignedByteOffset = n * 16;
      elems[n].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      n++;
    }
  }
  return n;
}

ID3D12PipelineState* D3D12Backend::getXlatPso(uint64_t vsHash, uint64_t psHash,
                                              uint32_t depthControl, uint32_t blendControl,
                                              uint32_t colorMask, bool depthOnly, bool hdrTarget,
                                              bool pointSprite, bool poolDepth,
                                              DXGI_FORMAT rtvFmt, DXGI_FORMAT dsvFmt,
                                              bool noDsv, bool nativeTextures) {
  auto& psoCache = nativeTextures ? m_nativeXlatPsoCache : m_xlatPsoCache;
  // Only the bits the PSO consumes join the key. depthOnly = RB_MODECONTROL
  // edram_mode==5: the console writes no color regardless of the mask.
  // iter-449b noDsv: a depth-LESS pass (guest bound RT0 with DS NULL) — the
  // PSO carries DSVFormat=UNKNOWN and depth is FORCED off (D3D12 requires it
  // with no DSV bound), so the depth bits are zeroed out of the key too: they
  // cannot affect this pipeline, and keying them would mint spurious variants.
  const uint32_t depthBits = noDsv ? 0u : (depthControl & 0x76u);  // z_enable/z_write/zfunc
  const uint32_t maskBits = depthOnly ? 0u : (colorMask & 0xFu);  // RT0 write mask
  uint64_t key = vsHash ^ (psHash * 0x9E3779B97F4A7C15ull);
  key ^= (uint64_t(blendControl) << 8) ^ (uint64_t(depthBits) << 40) ^ (uint64_t(maskBits) << 47);
  // iter-359m: the RTV format (LDR RGBA8 vs HDR RGBA16F) is part of the PSO —
  // a mismatch vs the bound RT is a D3D12 error, so it must join the key.
  key ^= hdrTarget ? 0x5BD1E995ull << 32 : 0ull;
  // iter-370: the point-sprite variant binds a geometry shader and declares POINT
  // topology, so it is a DIFFERENT pipeline for the same VS/PS pair — it must not
  // collide with the triangle variant in this cache.
  key ^= pointSprite ? 0xC2B2AE3D27D4EB4Full : 0ull;
  // iter-446 S5: the DSV format joins the key for exactly the reason the RTV
  // format does one line above. Pool depth surfaces are TYPELESS R32G8X24 with
  // a D32_FLOAT_S8X24_UINT view (so the depth resolve can sample them), while
  // the legacy geo depth is D32_FLOAT - and a PSO whose DSVFormat differs from
  // the bound DSV is a D3D12 error. MEASURED: the staged draws demand exactly
  // ONE (rtv,dsv) pair, 0000001C/00000014, so this is one extra variant and not
  // a combinatorial system.
  key ^= poolDepth ? 0x7FEB352D9E3779B9ull : 0ull;
  // ── iter-447: THE TARGET FORMATS AS REAL KEY FIELDS ────────────────────────
  // The two lines above are boolean APPROXIMATIONS of a format, and that is
  // precisely why the pool could serve one pair and refused 723,398 draws/run
  // on `fmt`. The reference makes them first-class PipelineState members set
  // from the bound surface (video.cpp:144-145, 3339, 3361) and hashes the
  // struct (:4139) — no variant system, no enumeration, the cache materialises
  // whatever the guest demands. This is that, minus a struct rewrite.
  //
  // ⚠ UNKNOWN keeps the LEGACY key bit-identical: the movie path passes nothing
  // and therefore hashes exactly as it did before this change. That property is
  // the whole reason the booleans survive — the verified render must not move
  // while the pool path widens.
  const DXGI_FORMAT effRtv =
      (rtvFmt != DXGI_FORMAT_UNKNOWN)
          ? rtvFmt
          : (hdrTarget ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
  // iter-449b: noDsv means REALLY no depth plane — not the legacy default.
  const DXGI_FORMAT effDsv =
      noDsv ? DXGI_FORMAT_UNKNOWN
            : (dsvFmt != DXGI_FORMAT_UNKNOWN)
                  ? dsvFmt
                  : (poolDepth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                               : DXGI_FORMAT_D32_FLOAT);
  if (rtvFmt != DXGI_FORMAT_UNKNOWN)
    key ^= (uint64_t)rtvFmt * 0x9E3779B97F4A7C15ull;
  if (dsvFmt != DXGI_FORMAT_UNKNOWN)
    key ^= (uint64_t)dsvFmt * 0xC2B2AE3D27D4EB4Full;
  key ^= noDsv ? 0xA0761D6478BD642Full : 0ull;
  auto it = psoCache.find(key);
  if (it != psoCache.end()) return it->second.Get();

  auto& cache = getShaderCache();
  const CachedShader* vs = cache.findVs(vsHash);
  const CachedShader* ps = cache.findPs(psHash);
  // iter-463: a missing shader is NOT negative-cached - runtime translation
  // (SR_SHADER_RUNTIME_XLAT) may admit it later and the same key must then build.
  if (!vs || !ps) return nullptr;
  if (vs->layout.empty()) {
    psoCache.emplace(key, nullptr);
    return nullptr;
  }

  D3D12_INPUT_ELEMENT_DESC elems[32] = {};
  const UINT n = xlatInputLayout(vs, elems);

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.InputLayout = {elems, n};
  pd.pRootSignature = nativeTextures ? m_nativeXlatRoot.root() : m_xlatRootSig.Get();
  pd.VS = {vs->dxil.data(), vs->dxil.size()};
  pd.PS = {ps->dxil.data(), ps->dxil.size()};
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  // Blend from the draw's RB_BLENDCONTROL0 snapshot (Xenia registers.h:780).
  {
    const uint32_t csrc = blendControl & 0x1Fu;
    const uint32_t cop = (blendControl >> 5) & 7u;
    const uint32_t cdst = (blendControl >> 8) & 0x1Fu;
    const uint32_t asrc = (blendControl >> 16) & 0x1Fu;
    const uint32_t aop = (blendControl >> 21) & 7u;
    const uint32_t adst = (blendControl >> 24) & 0x1Fu;
    auto& rtb = pd.BlendState.RenderTarget[0];
    // Xenia model: disabled when the whole equation is src*ONE + dst*ZERO, ADD.
    rtb.BlendEnable = !(csrc == 1 && cop == 0 && cdst == 0 &&
                        asrc == 1 && aop == 0 && adst == 0);
    rtb.SrcBlend = xeBlendFactor(csrc);
    rtb.DestBlend = xeBlendFactor(cdst);
    rtb.BlendOp = xeBlendOp(cop);
    rtb.SrcBlendAlpha = xeBlendFactor(asrc);
    rtb.DestBlendAlpha = xeBlendFactor(adst);
    rtb.BlendOpAlpha = xeBlendOp(aop);
    rtb.RenderTargetWriteMask = UINT8(maskBits);
  }
  // Depth from RB_DEPTHCONTROL: bit1 z_enable, bit2 z_write, [6:4] zfunc
  // (Xenos CompareFunction 0-7 maps to D3D12_COMPARISON_FUNC 1-8 directly).
  // SR_XLAT_NODEPTH=1: diagnostic — depth test off everywhere (iter-359i:
  // discriminates "helper/post draws z-fail" from every other empty-pass cause).
  static const bool s_noDepth = [] {
    const char* e = std::getenv("SR_XLAT_NODEPTH");
    return e && e[0] == '1';
  }();
  // iter-449b noDsv: no DSV will be bound, so depth MUST be off (D3D12 rule),
  // whatever RB_DEPTHCONTROL says — on console EDRAM depth always exists, so a
  // guest CAN leave z_enable set on a pass it gave no depth surface; honouring
  // the bit here would be an E_INVALIDARG PSO, i.e. a silently dropped draw.
  pd.DepthStencilState.DepthEnable =
      (!s_noDepth && !noDsv && (depthControl & 0x2u)) ? TRUE : FALSE;
  pd.DepthStencilState.DepthWriteMask =
      (!noDsv && (depthControl & 0x4u)) ? D3D12_DEPTH_WRITE_MASK_ALL
                                        : D3D12_DEPTH_WRITE_MASK_ZERO;
  pd.DepthStencilState.DepthFunc =
      D3D12_COMPARISON_FUNC(((depthControl >> 4) & 7u) + 1);
  pd.DSVFormat = effDsv;
  pd.SampleMask = UINT_MAX;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = effRtv;
  pd.SampleDesc.Count = 1;

  // iter-370 kPointList: attach the sprite-expansion GS. The IA feeds POINTS;
  // the GS emits the screen-space quad (see point_sprite.gs.hlsl for why the
  // expansion cannot happen in the CPU vertex decode).
  if (pointSprite) {
    const auto& gs = pointSpriteGs();
    if (gs.empty()) {
      psoCache.emplace(key, nullptr);
      return nullptr;
    }
    pd.GS = {gs.data(), gs.size()};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  }

  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  HRESULT hr = sr::hitch::timed(sr::hitch::kPso,
                                [&] { return m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)); });
  static std::atomic<uint32_t> s_psoFailLog{0};
  if (FAILED(hr)) {
    if (s_psoFailLog.fetch_add(1) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[D3D12_XLAT] CreatePSO FAILED 0x%08X vs=%016llX ps=%016llX (%u attrs)",
                  hr, (unsigned long long)vsHash, (unsigned long long)psHash, n);
    psoCache.emplace(key, nullptr);
    return nullptr;
  }
  auto* raw = pso.Get();
  psoCache.emplace(key, std::move(pso));
  static std::atomic<uint32_t> s_psoOkLog{0};
  if (s_psoOkLog.fetch_add(1) < 12)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[D3D12_XLAT] PSO ready vs=%016llX ps=%016llX attrs=%u",
                (unsigned long long)vsHash, (unsigned long long)psHash, n);
  return raw;
}

// P1 (docs/RENDERER_PM4_HYBRID.md): the EDRAM path's PSO. Real per-slot RT
// formats (MRT), per-RT blend from RB_BLENDCONTROL0-3 and per-RT write masks,
// and the depth state edram::computeDrawTargets normalized. Every consumed
// field joins the key.
ID3D12PipelineState* D3D12Backend::getPm4Pso(const Pm4PsoDesc& d) {
  auto& cache = getShaderCache();
  const CachedShader* vs = cache.findVs(d.vsHash);
  const CachedShader* ps = cache.findPs(d.psHash);
  if (!vs || !ps) return nullptr;  // iter-463: not negative-cached (see getXlatPso)
  // Both translated stages are required by this adapter. The general native
  // service also supports depth-only pipelines without a PS; an ownership
  // failure here must never silently select that different native pipeline.
  if (!(d.vsf ? vs->nativeFetchCode : vs->nativeCode) || !ps->nativeCode) return nullptr;
  if (d.vsf ? vs->dxilFetch.empty() : vs->layout.empty()) {
    return nullptr;
  }
  // step 4 lane C (gfx/alpha_test.hpp): the alpha-test variant - the PS's own or (round 4) one the runtime translator
  // made at its IM_LOAD (ShaderCache::alphaTestCode); pm4PrepareDraw asks for it only when there is one.
  const NativeShaderCode atCode = d.alphaTest ? cache.alphaTestCode(d.psHash, *ps) : NativeShaderCode{};
  if (d.alphaTest && !atCode) return nullptr;
  D3D12_INPUT_ELEMENT_DESC elems[32] = {};
  const UINT n = d.vsf ? 0u : xlatInputLayout(vs, elems);
  NativePipelineDesc pd;
  pd.inputLayout.reserve(n);
  for (UINT i = 0; i < n; ++i) {
    const auto& e = elems[i];
    pd.inputLayout.push_back({e.SemanticName, e.SemanticIndex, e.Format, e.InputSlot,
        e.AlignedByteOffset, e.InputSlotClass, e.InstanceDataStepRate});
  }
  pd.rootSignature = m_nativeXlatRoot.root();
  pd.vertex = d.vsf ? vs->nativeFetchCode : vs->nativeCode;
  pd.stripCut = d.stripCut ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                  : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  pd.pixel = d.noPixel      ? NativeShaderCode{}  // p2.zpd raster probe: no pixel shader
             : d.alphaTest ? atCode
                           : ps->nativeCode;
  pd.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
  // Xenia pipeline_cache.cc:1383-1398 / 3013-3030.
  pd.rasterizer.CullMode = d.cull == 1u   ? D3D12_CULL_MODE_FRONT
                                : d.cull == 2u ? D3D12_CULL_MODE_BACK
                                               : D3D12_CULL_MODE_NONE;
  pd.rasterizer.FrontCounterClockwise = d.frontCcw ? TRUE : FALSE;
  pd.rasterizer.DepthBias = d.depthBias;
  pd.rasterizer.SlopeScaledDepthBias = d.depthBiasSlope;
  pd.rasterizer.DepthClipEnable = d.depthClip ? TRUE : FALSE;
  pd.blend.IndependentBlendEnable = TRUE;
  pd.colorCount = d.numRt;
  for (uint32_t i = 0; i < 4; i++) {
    auto& rtb = pd.blend.RenderTarget[i];
    if (i >= d.numRt) continue;
    pd.colorFormats[i] = d.rtv[i] != DXGI_FORMAT_UNKNOWN ? d.rtv[i] : DXGI_FORMAT_R8G8B8A8_UNORM;
    const uint32_t bc = d.blend[i];
    const uint32_t csrc = bc & 0x1Fu, cop = (bc >> 5) & 7u, cdst = (bc >> 8) & 0x1Fu;
    const uint32_t asrc = (bc >> 16) & 0x1Fu, aop = (bc >> 21) & 7u, adst = (bc >> 24) & 0x1Fu;
    rtb.BlendEnable = d.writeMask[i] && !(csrc == 1 && cop == 0 && cdst == 0 &&
                                          asrc == 1 && aop == 0 && adst == 0);
    rtb.SrcBlend = xeBlendFactor(csrc);
    rtb.DestBlend = xeBlendFactor(cdst);
    rtb.BlendOp = xeBlendOp(cop);
    rtb.SrcBlendAlpha = xeBlendFactor(asrc);
    rtb.DestBlendAlpha = xeBlendFactor(adst);
    rtb.BlendOpAlpha = xeBlendOp(aop);
    rtb.RenderTargetWriteMask = d.rtv[i] != DXGI_FORMAT_UNKNOWN ? UINT8(d.writeMask[i] & 0xFu) : 0;
  }
  const bool hasDsv = d.dsv != DXGI_FORMAT_UNKNOWN;
  pd.depthStencil.DepthEnable = (hasDsv && d.zEnable) ? TRUE : FALSE;
  pd.depthStencil.DepthWriteMask =
      (hasDsv && d.zWrite) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  pd.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC((d.zFunc & 7u) + 1);
  if (hasDsv && d.stencil) {
    // RB_DEPTHCONTROL: func +8, fail +11, zpass +14, zfail +17; the _bf set
    // at +20.. applies only with backface_enable (+7), else back = front.
    // Xenos CompareFunction / StencilOp are the D3D12 enums minus one.
    const uint32_t s = d.stencilCtl;
    auto face = [](uint32_t f) {
      D3D12_DEPTH_STENCILOP_DESC o{};
      o.StencilFunc = D3D12_COMPARISON_FUNC((f & 7u) + 1);
      o.StencilFailOp = D3D12_STENCIL_OP(((f >> 3) & 7u) + 1);
      o.StencilPassOp = D3D12_STENCIL_OP(((f >> 6) & 7u) + 1);
      o.StencilDepthFailOp = D3D12_STENCIL_OP(((f >> 9) & 7u) + 1);
      return o;
    };
    pd.depthStencil.StencilEnable = TRUE;
    pd.depthStencil.StencilReadMask = UINT8(d.stencilMasks & 0xFFu);
    pd.depthStencil.StencilWriteMask = UINT8((d.stencilMasks >> 8) & 0xFFu);
    pd.depthStencil.FrontFace = face(s >> 8);
    pd.depthStencil.BackFace = ((s >> 7) & 1u) ? face(s >> 20) : face(s >> 8);
  }
  pd.depthFormat = d.dsv;
  if (d.pointSprite) {
    static const NativeShaderCode geometry = [] {
      NativeShaderCode code;
      NativeShaderCode::copy(pointSpriteGs(), code);
      return code;
    }();
    if (!geometry) return nullptr;
    pd.geometry = geometry;
    pd.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  }
  NativePipelineLease pso;
  const HRESULT hr = sr::hitch::timed(
      sr::hitch::kPso, [&] { return m_nativePipelines.acquire(pd, pso); });
  if (FAILED(hr)) {
    static std::atomic<uint32_t> s_fail{0};
    if (s_fail.fetch_add(1) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[PM4_PSO] CreatePSO FAILED 0x%08X vs=%016llX ps=%016llX numRt=%u "
                  "rtv=%u,%u,%u,%u dsv=%u",
                  hr, (unsigned long long)d.vsHash, (unsigned long long)d.psHash, d.numRt,
                  d.rtv[0], d.rtv[1], d.rtv[2], d.rtv[3], d.dsv);
    // step 6 (2g P2, Fable ruling 14): once, when the cache created a pipeline but could not store it (bad_alloc):
    // the draw is skipped, never drawn with a pipeline nothing retains
    static std::atomic<bool> s_insertLogged{false};
    if (hr == E_OUTOFMEMORY && !s_insertLogged.load(std::memory_order_relaxed)) {
      const uint64_t n = m_nativePipelines.counters().insertFailures;
      if (n && !s_insertLogged.exchange(true))
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[PM4_PSO] the pipeline cache could not store a pipeline it created (bad_alloc; %llu so far): "
                    "it was released and the draw skipped (logged once; step 6 2g P2)",
                    (unsigned long long)n);
    }
    return nullptr;
  }
  // I1 (step 6, Fable ruling 21 (1)): the lookup fast path's counters, so a replay or a run reports that the changed
  // path ran - per thread (no shared counter on the draw path), at that thread's 1st, 1,000th, 100,000th and every
  // 1,000,000th pipeline
  static thread_local uint64_t t_psoAcquires = 0;
  if (const uint64_t n = ++t_psoAcquires; n == 1 || n == 1000 || n == 100000 || n % 1000000 == 0) {
    const auto c = m_nativePipelines.counters();
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PSO_CACHE] thread=%lu acquires=%llu hits=%llu creates=%llu borrowed_key_lookups=%llu "
                "root_memo_hits=%llu interned_shader_copies=%llu entries=%zu",
                (unsigned long)GetCurrentThreadId(), (unsigned long long)n, (unsigned long long)c.hits,
                (unsigned long long)c.creates, (unsigned long long)c.borrowedKeyLookups,
                (unsigned long long)c.rootMemoHits, (unsigned long long)NativeShaderCode::internedCopies(),
                m_nativePipelines.size());
  }
  // The native cache retains this lease until checked graphics-queue teardown.
  return pso.pipeline();
}

// P1: root signature + bindless tables exactly as the xlat render loop sets
// them (the heap layout constants are file-local here).
void D3D12Backend::publishXlatSrv(ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* view, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
  if (!m_device || !m_xlatSrvHeap || !view) nativeRecordingFail("texture-publication", E_INVALIDARG);
  const auto begin = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
  const UINT stride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (!stride || destination.ptr < begin || (destination.ptr - begin) % stride)
    nativeRecordingFail("texture-descriptor-address", E_INVALIDARG);
  const SIZE_T slot = (destination.ptr - begin) / stride;
  if (slot >= m_xlatViews.size() || slot >= m_xlatViewId.size())
    nativeRecordingFail("texture-descriptor-slot", E_INVALIDARG);
  auto& current = m_xlatViews[slot];
  // The same view republished into its slot: the descriptor already holds it and
  // the recipe already owns the resource. This heap is written only from here.
  if (current.resource.Get() == resource && !current.buffer &&
      std::memcmp(&current.srv, view, sizeof(*view)) == 0) return;
  NativeViewRecipe recipe; recipe.resource = resource; recipe.srv = *view;
#if SR_ORACLE
  i3bSlotWrite(uint32_t(slot), "publish");  // step 6 I3-B closing check: a live slot is never rewritten
#endif
  current = std::move(recipe);  // ownership precedes descriptor publication
  m_xlatViewId[slot] = viewIdOf(current);
  if (slot < m_xlatSlotDim.size()) m_xlatSlotDim[slot] = uint8_t(view->ViewDimension);
  m_device->CreateShaderResourceView(resource, view, destination);
}

uint32_t D3D12Backend::viewIdOf(const NativeViewRecipe& recipe) {
  ViewIdentity identity;
  identity.storage = recipe.buffer ? static_cast<const void*>(recipe.buffer.resource()) : recipe.resource.Get();
  if (recipe.buffer) { identity.offset = recipe.buffer.offset(); identity.size = recipe.buffer.size(); }
  std::memcpy(&identity.srv, &recipe.srv, sizeof(identity.srv));
  static_assert(sizeof(ViewIdentity) % 8 == 0);
  uint64_t words[sizeof(ViewIdentity) / 8], hash = 14695981039346656037ull;
  std::memcpy(words, &identity, sizeof(identity));
  for (const uint64_t word : words) hash = (hash ^ word) * 1099511628211ull;
  // An entry may outlive its resource. A new resource at the same address then takes
  // the old id, but nothing can still hold that id: slots and key tables keep their
  // resources alive.
  auto& bucket = m_viewIds[hash];
  for (const auto& [known, id] : bucket)
    if (std::memcmp(&known, &identity, sizeof(identity)) == 0) return id;
  if (m_nextViewId == UINT32_MAX || m_viewIdCount >= (1u << 16)) {
    // Start over: new ids for every slot, no key or entry from before. (Ids are never
    // reused within a run of the counter, so a wrap must drop everything that holds one.)
    m_viewIds.clear(); m_viewIdCount = 0;
    if (m_nextViewId == UINT32_MAX) {
      m_nextViewId = 1;
      dropTexKeys();
      for (size_t s = 0; s < m_xlatViews.size(); ++s)
        if (&m_xlatViews[s] != &recipe) m_xlatViewId[s] = viewIdOf(m_xlatViews[s]);
    }
    return viewIdOf(recipe);
  }
  m_viewIds[hash].emplace_back(identity, m_nextViewId); ++m_viewIdCount;
  return m_nextViewId++;
}

void D3D12Backend::dropTexKeys(uint64_t tail, uint64_t age) {
  for (auto& key : m_texKeys)
    if (key.valid && (tail == UINT64_MAX || tail - key.snapshot.descriptors.pageSerial() >= age))
      key = TexKey{};
}

// step 6 I3-B (bindless, Fable ruling 44): a native translated draw indexes m_xlatSrvHeap with the CP's own selection
// words, as the legacy pass does (xlatBindRootTables) - no per-draw compact table, no texture cache lookup. The words
// are checked (bounds, and the slot's view dimension); the shader reads the slot at execution, so no writer ever
// rewrites a slot an open or submitted recording may read: every slot change is copy-on-write with fence retirement
// (texSlotCow, texEvictDrain), checked under SR_ORACLE (i3bSlotWrite / i3bSubmitted). Recordings are
// single-submission (openNativeRecording makes a new one; NativeDrawRecording::submit moves Closed -> Spent), so a
// copy-on-write never meets a recording executed a second time.
void D3D12Backend::bindXlatTextures(const DrawBatch& batch) {
  if (!m_nativeRecording || !m_xlatSrvHeap || batch.sharedCbOff > m_xlatConstants.size() ||
      48u * sizeof(uint32_t) > m_xlatConstants.size() - batch.sharedCbOff)
    nativeRecordingFail("texture-binding-range", E_INVALIDARG);
  uint32_t selected[48];
  std::memcpy(selected, m_xlatCBPtr + batch.sharedCbOff, sizeof(selected));
  auto check = [&](uint32_t index, uint32_t base, uint32_t count, D3D12_SRV_DIMENSION dimension) {
    if (index >= count || uint64_t(base) + index >= m_xlatSlotDim.size())
      nativeRecordingFail("texture-binding-index", E_INVALIDARG);
    if (m_xlatSlotDim[base + index] != uint8_t(dimension))
      nativeRecordingFail("texture-binding-dimension", E_INVALIDARG);
#if SR_ORACLE
    i3bSlotBound(base + index);
#endif
  };
  for (uint32_t i = 0; i < 16; ++i) {
    check(selected[i], 0, kTex2DSlots, D3D12_SRV_DIMENSION_TEXTURE2D);
    const uint32_t volume = selected[16 + i], cube = selected[32 + i];
    const bool band = (volume & 0x80000000u) != 0;
    const auto dimension = !band || (volume & 0x20000000u) ? D3D12_SRV_DIMENSION_TEXTURE3D :
        (volume & 0x40000000u) ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
    check(band ? volume & 0x1fffffffu : volume, band ? 0 : kTex2DSlots, band ? kTex2DSlots : 8u, dimension);
    const bool cubeBand = (cube & 0x80000000u) != 0;
    check(cubeBand ? cube & 0x1fffffffu : cube, cubeBand ? 0 : kTex2DSlots + 8u, cubeBand ? kTex2DSlots : 8u,
          D3D12_SRV_DIMENSION_TEXTURECUBE);
#if SR_ORACLE
    // The legacy 8-entry 3D / cube tables are written in place by the HLE uploads; native draws name only entry 0.
    m_i3bLegacyRefs += uint64_t(!band && volume != 0) + uint64_t(!cubeBand && cube != 0);
#endif
  }
  const D3D12_GPU_DESCRIPTOR_HANDLE heap = m_xlatSrvHeap->GetGPUDescriptorHandleForHeapStart();
  auto at = [&](uint32_t slot) { return D3D12_GPU_DESCRIPTOR_HANDLE{heap.ptr + uint64_t(slot) * m_xlatSrvInc}; };
  D3D12_GPU_DESCRIPTOR_HANDLE raw = at(kRawNullSrvSlot);
  if (batch.vsf) {
    if (batch.vsfShm) {
      check(0, kVsfShmSrvSlot, 1, D3D12_SRV_DIMENSION_BUFFER);
      raw = at(kVsfShmSrvSlot);
    } else raw = rawRingView(batch.vsfRawVertices);
  }
#if SR_ORACLE
  else i3bSlotBound(kRawNullSrvSlot);
#endif
  // The sampler options change only through gfxopt::set (version bump): re-select then.
  HRESULT hr = S_OK;
  if (const uint64_t version = gfxopt::version();
      !m_drawSamplers || version != m_drawSamplersVersion || m_volSamplerGen != m_drawSamplersVolGen) {
    const auto options = gfxopt::current();
    hr = m_nativeSamplers.select(m_device.Get(), uint32_t(options.anisotropic), options.lodBias, m_volSamplers,
                                 m_volSamplerGen, m_drawSamplers);
    if (FAILED(hr)) nativeRecordingFail("sampler-version", hr);
    if (m_drawSamplers.count() != kXlatSamplerTable || m_drawSamplers.type() != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ||
        m_drawSamplers.device() != m_device.Get())
      nativeRecordingFail("sampler-version", E_INVALIDARG);
    m_drawSamplersVersion = version;
    m_drawSamplersVolGen = m_volSamplerGen;
  }
  // The recording retains the sampler version once (each open is a new recording, m_clOpens).
  if (m_samplersKeptRec != m_clOpens || m_samplersKeptVersion != m_drawSamplersVersion ||
      m_samplersKeptVolGen != m_drawSamplersVolGen) {
    hr = m_nativeRecording->keep(m_drawSamplers);
    if (FAILED(hr)) nativeRecordingFail("sampler-retain", hr);
    m_samplersKeptRec = m_clOpens;
    m_samplersKeptVersion = m_drawSamplersVersion;
    m_samplersKeptVolGen = m_drawSamplersVolGen;
  }
  auto* commands = m_nativeRecording->commands();
  if (!commands) nativeRecordingFail("texture-bind", E_ABORT);
  ID3D12DescriptorHeap* heaps[] = {m_xlatSrvHeap.Get(), m_drawSamplers.heap()};
  commands->SetDescriptorHeaps(2, heaps);
  commands->SetGraphicsRootDescriptorTable(5, heap);                   // t0 space0: the 2D band
  commands->SetGraphicsRootDescriptorTable(6, at(kTex2DSlots));        // space1: the legacy 3D table
  commands->SetGraphicsRootDescriptorTable(7, at(kTex2DSlots + 8));    // space2: the legacy cube table
  commands->SetGraphicsRootDescriptorTable(8, m_drawSamplers.gpu());
  commands->SetGraphicsRootDescriptorTable(10, raw);                   // t0 space5: the raw vertex fetch
  commands->SetGraphicsRootDescriptorTable(11, heap);                  // spaces 6-8: the 2D band as array / 3D / cube
  commands->SetGraphicsRootDescriptorTable(12, heap);
  commands->SetGraphicsRootDescriptorTable(13, heap);
  ++m_nativeTextureDraws; ++m_nativeSamplerBindings; ++m_i3bDirectDraws;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::rawRingView(const NativeBufferSlice& slice) {
  // A RAW view's first element must sit on 16 bytes (D3D12 error #31: an invalid view removes the device); the
  // staged path aligns its slices to 16 (pm4StageVsf), so any other offset is refused, not bound.
  if (!slice || slice.offset() % 16 || slice.size() % 4 || slice.size() / 4 > UINT32_MAX)
    nativeRecordingFail("raw-fetch-snapshot", E_INVALIDARG);
  HRESULT hr = m_nativeRecording->keep(slice);  // the recording owns the staged range until it completes
  if (FAILED(hr)) nativeRecordingFail("raw-fetch-retain", hr);
  uint32_t slot = m_rawLastSlot;
  if (m_rawLastRec != m_clOpens || m_rawLastResource != slice.resource() || m_rawLastOffset != slice.offset() ||
      m_rawLastSize != slice.size()) {
    const uint64_t n = m_rawRingWrites;
    if (n >= kRawRingSlots && n - kRawRingSlots >= m_rawRingCheckedTo) {
      const uint64_t old = n - kRawRingSlots;  // the write this slot holds
      if (old >= m_rawRingRecStart) nativeRecordingFail("raw-ring-overrun", E_OUTOFMEMORY);  // one recording's own
      while (!m_rawRingSegments.empty() && m_rawRingSegments.front().end <= old) m_rawRingSegments.pop_front();
      if (!m_rawRingSegments.empty() && m_rawRingSegments.front().start <= old) {
        const RawRingSegment segment = m_rawRingSegments.front();
        m_rawRingSegments.pop_front();
        uint64_t completed = 0;
        hr = m_nativeImages.completionSnapshot(completed);
        if (FAILED(hr)) nativeRecordingFail("raw-ring-completion", hr);
        if (completed < segment.submission) {
          ++m_i3bRawWaits;
          i3bWaitLine("raw_ring", segment.submission);
          sr::hitch::Scope hs(sr::hitch::kGpuWait);
          hr = m_nativeImages.wait(segment.submission, 10000);
          if (FAILED(hr)) nativeRecordingFail("raw-ring-wait", hr);
        }
        m_rawRingCheckedTo = segment.end;
      } else m_rawRingCheckedTo = old + 1;  // written by a recording that was discarded, never executed
    }
    slot = kRawRingBase + uint32_t(n % kRawRingSlots);
#if SR_ORACLE
    i3bSlotWrite(slot, "raw-ring");
#endif
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = slice.offset() / 4;
    srv.Buffer.NumElements = UINT(slice.size() / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    m_device->CreateShaderResourceView(slice.resource(), &srv,
        {m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(slot) * m_xlatSrvInc});
    m_rawRingWrites = n + 1;
    m_rawLastRec = m_clOpens; m_rawLastResource = slice.resource();
    m_rawLastOffset = slice.offset(); m_rawLastSize = slice.size(); m_rawLastSlot = slot;
  }
#if SR_ORACLE
  i3bSlotBound(slot);
#endif
  return {m_xlatSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(slot) * m_xlatSrvInc};
}

void D3D12Backend::i3bWaitLine(const char* kind, uint64_t detail) {
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 1000 || n % 1000 == 0) {
    // QueryPerformanceCounter ticks and their frequency, so a grader maps the wait onto the present stamps
    LARGE_INTEGER qpc{}, qpf{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&qpf);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[I3_BINDLESS] WAIT #%u kind=%s detail=%llu frame_fence=%llu qpc=%lld qpf=%lld (a HITCH frame for Q)",
                n, kind, (unsigned long long)detail, (unsigned long long)m_fenceValues[m_frameIndex],
                (long long)qpc.QuadPart, (long long)qpf.QuadPart);
  }
}

// The slot-exhaustion victim: the least recently bound entry, never one bound in this frame nor a slot the draw being
// assembled selected (fix2, Astra 5d0c2a63 P1 / P2); false = none (a transient exhaustion: nothing is cached, the next
// bind retries). step 6 I3-B: bindless draws read a slot at execution, so its slot is rewritten at once only after
// the GPU finished the frames that bound it. now = this frame's fence: only slots bound two or more frames ago
// qualify, whose last-use fence was already signaled (the wait is never on this frame's own signal); the wait is
// counted, a HITCH frame for Q.
bool D3D12Backend::texExhaustVictim(uint64_t& victim) {
  std::vector<sr::gfx::texevict::Candidate> c;
  c.reserve(m_texRefresh.size());
  for (const auto& kv : m_texRefresh)
    if (kv.second.slot < m_texSlotUse.size())
      c.push_back({kv.first, m_texSlotUse[kv.second.slot], 0u, 0u, kv.second.slot});
  if (!sr::gfx::texevict::exhaustVictim(c, m_fenceValues[m_frameIndex], m_texPinned, m_texPinnedN, &victim))
    return false;
  if (const uint32_t vs = m_texRefresh[victim].slot; vs < m_texSlotUse.size()) {
    const uint64_t need = m_texSlotUse[vs];
    const uint64_t completed = m_fence->GetCompletedValue();
    if (completed == UINT64_MAX) nativeRecordingFail("tex-exhaust-completion", DXGI_ERROR_DEVICE_REMOVED);
    if (need > completed) {
      ++m_i3bExhaustWaits;
      i3bWaitLine("exhaust", need);
      sr::hitch::Scope hs(sr::hitch::kGpuWait);
      waitNativeFence(m_fence.Get(), need, m_fenceEvent, "tex-exhaust-wait");
    }
  }
  return true;
}

uint32_t D3D12Backend::texSlotCow(uint32_t oldSlot, Microsoft::WRL::ComPtr<ID3D12Resource>& oldResource,
                                  const char* site) {
  if (!texSlotAvailable()) return 0;
  const uint32_t slot = texSlotAlloc();
  if (!slot) return 0;
  if (oldSlot && m_texEvictOn && oldSlot < m_texSlotUse.size()) {
#if SR_ORACLE
    m_texSlotEvicted[oldSlot] = 1;
#endif
    m_texSlotRetire.retire(oldSlot, texFenceNow(), std::move(oldResource));
  } else if (oldResource) retireResource(std::move(oldResource));  // outside the pool: never rewritten again
  ++m_i3bSlotCow;
  m_texBindGen++;  // iter-467 native-b SR_NB_BIND_MEMO: no remembered answer names the old slot
  if (m_i3bSlotCow <= 16 || m_i3bSlotCow % 1000 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[I3_BINDLESS] COW #%llu site=%s slot %u -> %u (the old slot retires)",
                (unsigned long long)m_i3bSlotCow, site, oldSlot, slot);
  return slot;
}

#if SR_ORACLE
void D3D12Backend::i3bSlotBound(uint32_t slot) {
  if (slot >= m_i3bSlotBoundRec.size()) return;
  ++m_i3bChecked;
  if (m_i3bSlotBoundRec[slot] == m_clOpens) return;
  m_i3bSlotBoundRec[slot] = m_clOpens;
  m_i3bRecBinds.emplace_back(slot, m_i3bSlotGen[slot]);
}

void D3D12Backend::i3bSlotWrite(uint32_t slot, const char* what) {
  if (slot >= m_i3bSlotGen.size()) return;
  ++m_i3bSlotGen[slot];
  if (!m_i3bSlotSubmitted[slot]) return;
  uint64_t completed = 0;
  if (FAILED(m_nativeImages.completionSnapshot(completed)) || completed >= m_i3bSlotSubmitted[slot]) return;
  if (++m_i3bBadInFlight <= 16)
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[I3_BINDLESS] BAD in-flight rewrite #%llu: slot %u (%s) last read by native submission %llu, "
                "completed %llu",
                (unsigned long long)m_i3bBadInFlight, slot, what, (unsigned long long)m_i3bSlotSubmitted[slot],
                (unsigned long long)completed);
}

void D3D12Backend::i3bSubmitted(uint64_t submission) {
  for (const auto& [slot, generation] : m_i3bRecBinds) {
    if (m_i3bSlotGen[slot] != generation && ++m_i3bBadOpen <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[I3_BINDLESS] BAD rewrite before submission #%llu: slot %u bound at generation %u, now %u "
                  "(native submission %llu)",
                  (unsigned long long)m_i3bBadOpen, slot, generation, m_i3bSlotGen[slot],
                  (unsigned long long)submission);
    m_i3bSlotSubmitted[slot] = submission;
  }
  m_i3bRecBinds.clear();
}
#endif

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::pm4RtSrvCpu(uint32_t slot) const {
  D3D12_CPU_DESCRIPTOR_HANDLE h = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += SIZE_T(kPm4RtSrvBase + slot) *
           m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return h;
}
// step 4 lane C 2i (lane C NOTES 2026-10-20, 2i (2)): the D3D12 debug layer + GPU-based validation on the gp2 replay
// named ERROR 708 (SetGraphicsRootDescriptorTable: the handle's heap is not the heap that is set) and GBV ERROR 935
// (a PIXEL draw read an uninitialized root argument, parameter 2). A translated draw binds its textures through
// TranslatedTextureSnapshot::bind, which sets the NATIVE descriptor arena's heaps on m_commandList; the EDRAM passes
// after it (the direct transfers, B1b's fill, the texture load from a host RT, the native resolve, the GPU store,
// and native_targets.inc's target resolve + held-memory materialization - Astra P1 on the fix) bind tables from
// m_xlatSrvHeap (pm4RtSrvGpu) assuming the heaps set at the list's start - undefined behaviour: the descriptors read
// are whatever the driver resolves the stale handle to. pm4XlatHeaps() re-sets the xlat heaps at the start of every
// such pass (one SetDescriptorHeaps; each translated draw sets its own again).
// BAKED ON as a platform fix (docs/RENDERER_PM4_HYBRID.md section 13.1): the GBV A/B (708 / 935 -> 0), the replay
// matrix r9set (22/22, the PC picture unchanged), the PC flicker gate. It is not the cause of the 890M one-present
// specks (the Ally A/B s4r_heapab). Dev: SR_PM4_XLAT_HEAPS=0 = the old stale-heap binds, an A/B arm only. Release:
// always on.
static bool pm4XlatHeapsOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_XLAT_HEAPS");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;
#endif
}
void D3D12Backend::pm4XlatHeaps() {
  if (!pm4XlatHeapsOn() || !m_commandList || !m_xlatSrvHeap || !m_xlatSamplerHeap) return;
  ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
  m_commandList->SetDescriptorHeaps(2, heaps);
}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::pm4RtSrvGpu(uint32_t slot) const {
  D3D12_GPU_DESCRIPTOR_HANDLE h = m_xlatSrvHeap->GetGPUDescriptorHandleForHeapStart();
  h.ptr += uint64_t(kPm4RtSrvBase + slot) *
           m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return h;
}
uint32_t D3D12Backend::pm4RtSrvCapacity() const { return kPm4RtSrvSlots; }

void D3D12Backend::xlatSetPixelPosScale() {
  const resscale::Scale& rs = resscale::get();
  const float bias[2] = {rs.scaled() ? 1.0f / float(rs.x) - 1.0f : 0.0f,
                         rs.scaled() ? 1.0f / float(rs.y) - 1.0f : 0.0f};
  m_commandList->SetGraphicsRoot32BitConstants(14, 2, bias, 0);
}

TranslatedConstantSnapshot D3D12Backend::bindXlatConstants(const DrawBatch& b) {
  if (!m_nativeRecording || !m_xlatCBPtr) nativeRecordingFail("constant-recording", E_UNEXPECTED);
  bindXlatTextures(b);
  if (b.zcView) {   // step 6 I10p: the CP's views - no snapshot is staged (callers that read one are refused)
    zcBindConstants(b);
    ++m_nativeConstantDraws;
    return {};
  }
  auto range = [&](uint64_t offset, uint32_t size) -> std::span<const uint8_t> {
    if (offset > m_xlatConstants.size() || size > m_xlatConstants.size() - offset)
      nativeRecordingFail("constant-staging-range", E_INVALIDARG);
    return {m_xlatCBPtr + offset, size};
  };
  // step 6 I3-B: the shared block is the CP's own bytes - its 48 selection words index m_xlatSrvHeap directly.
  const auto shared = range(b.sharedCbOff, TranslatedConstantSnapshot::kSharedBytes);
  using Snapshot = TranslatedConstantSnapshot;
  // VS / PS: only up to the last block their shaders read (b.xlatVsBytes / xlatPsBytes).
  const std::span<const uint8_t> blocks[] = {
      range(b.xlatVsCbOff, b.xlatVsBytes), range(b.xlatPsCbOff, b.xlatPsBytes), shared,
      b.vsf ? range(b.vsfFetchCbOff, Snapshot::kFetchBytes) : std::span<const uint8_t>{}};
  // Where m_lastConstantBytes keeps each block (the largest extents, not the snapshot's layout).
  constexpr uint32_t offsets[] = {0, Snapshot::kFloatBytes, 2 * Snapshot::kFloatBytes,
                                  2 * Snapshot::kFloatBytes + Snapshot::kSharedBytes};
  // Equal extents and bytes are equal constants: the previous slice is immutable and owned.
  // Blocks most likely to differ are compared first (fetch, vs, ps, shared).
  const bool fetch = !blocks[3].empty();
  bool same = bool(m_lastConstants) && m_lastConstantFetch == fetch &&
      m_lastConstants.size(0) == blocks[0].size() && m_lastConstants.size(1) == blocks[1].size();
  for (const uint32_t i : {3u, 0u, 1u, 2u}) {
    if (!same) break;
    if (!blocks[i].empty())
      same = std::memcmp(blocks[i].data(), m_lastConstantBytes.data() + offsets[i], blocks[i].size()) == 0;
  }
  TranslatedConstantSnapshot constants;
  HRESULT hr = S_OK;
  if (same) {
    constants = m_lastConstants;
    ++m_nativeConstantReuses;
  } else {
    hr = Snapshot::stage(m_nativeConstants, blocks[0], blocks[1], blocks[2], blocks[3], constants);
    if (FAILED(hr)) nativeRecordingFail("constant-snapshot", hr);
    for (uint32_t i = 0; i < 4; ++i)
      if (!blocks[i].empty()) std::memcpy(m_lastConstantBytes.data() + offsets[i], blocks[i].data(), blocks[i].size());
    m_lastConstants = constants; m_lastConstantFetch = fetch;
  }
  hr = constants.bind(*m_nativeRecording);
  if (FAILED(hr)) nativeRecordingFail("constant-bind", hr);
  ++m_nativeConstantDraws;
  return constants;
}

// step 6 I10p (SR_XLAT_CB_ZEROCOPY): a view batch's constants. VS / PS / fetch: root CBVs straight at the CP's
// blocks in the write-only twin (the CB third's fence owns them; nothing is kept per draw). Shared: the CP's block
// from the scratch with this bind's texture indices; equal to the previous draw's in this render set -> that address
// again; else written ONCE into this draw's own slot in the twin (reserved by the CP in the same third). A slot at or
// below one already written in this set (a batch bound again) is never rewritten - it is staged instead (counted).
void D3D12Backend::zcBindConstants(const DrawBatch& b) {
  using Snapshot = TranslatedConstantSnapshot;
  const uint64_t cap = m_xlatCBGpu.bytes();
  auto viewOk = [&](uint64_t off, uint64_t n) { return off % 256u == 0 && off <= cap && n <= cap - off; };
  if (!m_xlatCBGpu || b.sharedCbOff > m_xlatConstants.size() ||
      Snapshot::kSharedBytes > m_xlatConstants.size() - b.sharedCbOff || !viewOk(b.xlatVsCbOff, Snapshot::kFloatBytes) ||
      !viewOk(b.xlatPsCbOff, Snapshot::kFloatBytes) || !viewOk(b.zcSharedOff, Snapshot::kSharedBytes) ||
      (b.vsf && !viewOk(b.vsfFetchCbOff, Snapshot::kFetchBytes)))
    nativeRecordingFail("constant-view-range", E_INVALIDARG);
  // step 6 I3-B (bindless): the shared block is the CP's own bytes (its selection words index m_xlatSrvHeap)
  std::array<uint8_t, Snapshot::kSharedBytes> shared;
  std::memcpy(shared.data(), m_xlatCBPtr + b.sharedCbOff, shared.size());
  bool written = false;
  if (m_zcSharedValid && m_zcSharedSet == b.zcSet &&
      std::memcmp(shared.data(), m_zcSharedBytes.data(), shared.size()) == 0) {
    ++m_zcSharedReuses;   // the same generation's slot (never rewritten in it) or its staged slice (kept per recording)
  } else {
    const uint32_t third = uint32_t(std::min<uint64_t>(b.zcSharedOff / (kXlatCBBytes / 3), 2));
    if (m_zcSlotSet[third] != b.zcSet) {   // a new generation of this CB third: its earlier slots are retired
      m_zcSlotSet[third] = b.zcSet;
      m_zcSlotAny[third] = false;
    }
    if (!m_zcSlotAny[third] || b.zcSharedOff > m_zcSlotHigh[third]) {
      m_xlatCBGpu.write(b.zcSharedOff, shared.data(), shared.size());
      m_zcSlotHigh[third] = b.zcSharedOff;
      m_zcSlotAny[third] = true;
      m_zcSharedAddr = m_xlatCBGpu.address(b.zcSharedOff);
      m_zcSharedSlice = {};
      written = true;
      ++m_zcSharedWrites;
    } else {   // a slot at or below one already written in this generation (a batch bound again, a set rendered again)
      const HRESULT hr = m_nativeConstants.stageConstantBuffer({shared.data(), shared.size()}, m_zcSharedSlice);
      if (FAILED(hr)) nativeRecordingFail("constant-shared-stage", hr);
      m_zcSharedAddr = m_zcSharedSlice.address();
      m_zcSharedKeptIn = 0;
      ++m_zcSharedFallbacks;
    }
    m_zcSharedBytes = shared;
    m_zcSharedSet = b.zcSet;
    m_zcSharedValid = true;
  }
  if (m_zcSharedSlice && m_zcSharedKeptIn != m_nativeRecordingSerial) {   // a staged fallback: kept per recording
    const HRESULT hr = m_nativeRecording->keep(m_zcSharedSlice);
    if (FAILED(hr)) nativeRecordingFail("constant-shared-retain", hr);
    m_zcSharedKeptIn = m_nativeRecordingSerial;
  }
  auto* commands = m_nativeRecording->commands();
  if (!commands) nativeRecordingFail("constant-view-recording", E_UNEXPECTED);
  const D3D12_GPU_VIRTUAL_ADDRESS vs = m_xlatCBGpu.address(b.xlatVsCbOff), ps = m_xlatCBGpu.address(b.xlatPsCbOff);
  // The parameters TranslatedConstantSnapshot::bind sets: VS b0 / b3, PS b1 / b4, shared b2, fetch (9).
  commands->SetGraphicsRootConstantBufferView(0, vs);
  commands->SetGraphicsRootConstantBufferView(1, ps);
  commands->SetGraphicsRootConstantBufferView(2, m_zcSharedAddr);
  commands->SetGraphicsRootConstantBufferView(3, vs);
  commands->SetGraphicsRootConstantBufferView(4, ps);
  if (b.vsf) commands->SetGraphicsRootConstantBufferView(9, m_xlatCBGpu.address(b.vsfFetchCbOff));
  ++m_zcBinds;
#if SR_ORACLE
  zcConsume(b, shared.data(), written);
#else
  (void)written;
#endif
}

void D3D12Backend::zcCounterLine(const char* when) {
  if (!m_zcOn) return;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[XLAT_CB_ZEROCOPY]%s binds=%llu shared_writes=%llu shared_reuses=%llu shared_fallbacks=%llu cp: "
              "vs_writes=%llu ps_writes=%llu fetch_writes=%llu fetch_dedups=%llu bytes=%llu shared_full=%llu "
              "third_peak_kib: twin=%llu of %llu scratch=%llu of %llu",
              when, (unsigned long long)m_zcBinds, (unsigned long long)m_zcSharedWrites,
              (unsigned long long)m_zcSharedReuses, (unsigned long long)m_zcSharedFallbacks,
              (unsigned long long)m_zcVsWrites.load(std::memory_order_relaxed),
              (unsigned long long)m_zcPsWrites.load(std::memory_order_relaxed),
              (unsigned long long)m_zcFetchWrites.load(std::memory_order_relaxed),
              (unsigned long long)m_zcFetchDedups.load(std::memory_order_relaxed),
              (unsigned long long)m_zcBytes.load(std::memory_order_relaxed),
              (unsigned long long)m_zcShFull.load(std::memory_order_relaxed),
              (unsigned long long)(m_zcCbPeak.load(std::memory_order_relaxed) >> 10),
              (unsigned long long)((kXlatCBBytes / 3) >> 10),
              (unsigned long long)(m_zcShPeak.load(std::memory_order_relaxed) >> 10),
              (unsigned long long)((m_xlatConstants.size() / 3) >> 10));
#if SR_ORACLE
  if (m_zcCheck)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XLAT_CB_ZEROCOPY_CHECK]%s checked=%llu checked_xframe=%llu BAD=%llu stale_gen=%llu retired=%llu "
                "retire_checked=%llu retire_BAD=%llu retire_assert_BAD=%llu mutant=%d",
                when, (unsigned long long)m_zcChecked.load(), (unsigned long long)m_zcCheckedXframe.load(),
                (unsigned long long)m_zcBad.load(),
                (unsigned long long)m_zcStaleGen.load(), (unsigned long long)m_zcRetired.load(),
                (unsigned long long)m_zcRetireChecked.load(), (unsigned long long)m_zcRetireBad.load(),
                (unsigned long long)m_zcFenceBad.load(), m_zcMutant ? 1 : 0);
#endif
}

// step 6 I10p: the CP's CB-third rotation, shared by both rotation sites (drainDrawQueue's !pm4Append rotation =
// drainSite, which resets the CB cursor into the new set's third right after; replayPrepareTail, which does not). The
// set generation advances (the shared slot's write-once key). Check mode: the finished set's used CB end becomes its
// third's poison range - only when the finished generation owns the cursor (Astra r2 P3: an empty replay-tail set
// keeps an older set's cursor, which a later tail in the same third would otherwise revive as its own); the TEST-ONLY
// mutant takes the previous CB third every 8th drain set (one set early); then the new third's retire assert /
// compare-on-retire / poison (zcBeginThird).
void D3D12Backend::zcRotateCbThird(bool drainSite) {
  const uint32_t prevCbThird = m_xlatCbThird;
  m_xlatCbThird = m_xlatThird;
#if SR_ORACLE
  if (m_zcCheck) {
    zcNoteThirdHigh(prevCbThird, m_zcCursorSet == m_zcCpSet ? m_xlatCBUsed : 0);
    if (drainSite && m_zcMutant && (++m_zcSetSerial % 8u) == 0u) m_xlatCbThird = prevCbThird;
  }
#else
  (void)prevCbThird;
#endif
  ++m_zcCpSet;
#if SR_ORACLE
  if (m_zcCheck) {
    zcBeginThird(m_xlatCbThird);
    if (drainSite) m_zcCursorSet = m_zcCpSet;   // the caller resets m_xlatCBUsed into m_xlatCbThird next
  }
#else
  (void)drainSite;
#endif
}

#if SR_ORACLE
// Check mode's install (createXlatPipeline; the zero-copy fixture): the cacheable shadow of the twin - every twin
// write lands there too, under m_zcShadowMu; every check reads it, never the twin.
bool D3D12Backend::zcShadowInit() {
  if (!m_xlatCBGpu || !m_zcShadow.init(kXlatCBBytes, false)) return false;
  m_xlatCBGpu.setShadow(m_zcShadow.data(), &m_zcShadowMu);
  return true;
}

// Render thread, at a view batch's bind: compare-on-consume (the shadow's bytes of each view against what the CP says
// it must hold), the generation (a view of a third the CP has since reused = stale), and the ranges recorded for the
// third's retire, with the open recording that bound them.
void D3D12Backend::zcConsume(const DrawBatch& b, const uint8_t* shared, bool sharedWritten) {
  if (!m_zcCheck) return;
  const uint8_t* shadow = m_zcShadow.data();
  struct View { const char* what; uint64_t off; uint32_t use, bytes; uint64_t want; };
  const View views[3] = {{"vs", b.xlatVsCbOff, b.zcVsUse, 4096u, b.zcVsHash},
                         {"ps", b.xlatPsCbOff, b.zcPsUse, 4096u, b.zcPsHash},
                         {"fetch", b.vsfFetchCbOff, ~0u, 768u, b.zcFetchHash}};
  const uint32_t n = b.vsf ? 3u : 2u;
  uint64_t got[3] = {};
  {
    std::lock_guard<std::mutex> sl(m_zcShadowMu);
    for (uint32_t i = 0; i < n; ++i) got[i] = xlatCbHash(shadow + views[i].off, views[i].use, views[i].bytes);
  }
  for (uint32_t i = 0; i < n; ++i) {
    m_zcChecked.fetch_add(1, std::memory_order_relaxed);
    if (got[i] != views[i].want) {
      const uint64_t bad = m_zcBad.fetch_add(1, std::memory_order_relaxed) + 1;
      if (bad <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[XLAT_CB_ZEROCOPY_CHECK] BAD consume draw#%u %s view off=%llu third=%u gen=%u: the shadow's bytes "
                    "differ from the CP's", b.drawOrdinal, views[i].what, (unsigned long long)views[i].off, b.zcThird,
                    b.zcGen);
    }
  }
  std::lock_guard<std::mutex> lk(m_zcMu);
  auto& live = m_zcLive[b.zcThird % 3u];
  if (live.gen != b.zcGen) {
    const uint64_t stale = m_zcStaleGen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (stale <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[XLAT_CB_ZEROCOPY_CHECK] BAD stale generation draw#%u third=%u: the view's gen %u, the third's %u "
                  "(the CP reused the third before this bind)", b.drawOrdinal, b.zcThird, b.zcGen, live.gen);
  } else if (live.ticket) {   // an earlier recording that bound this generation has been submitted: cross-frame
    m_zcCheckedXframe.fetch_add(n, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < n; ++i) live.ranges.push_back({views[i].off, views[i].use, views[i].bytes, got[i]});
  if (sharedWritten) live.ranges.push_back({b.zcSharedOff, ~0u, 512u, xlatCbHash(shared, ~0u, 512u)});
  live.openRec = m_nativeRecordingSerial;
}

// Render thread, after a recording's submit: its views' thirds are carried by that submission ticket.
void D3D12Backend::zcSubmitted() {
  if (!m_zcCheck) return;
  std::lock_guard<std::mutex> lk(m_zcMu);
  for (auto& live : m_zcLive)
    if (live.openRec && live.openRec == m_nativeRecordingSerial) {
      live.ticket = std::max(live.ticket, m_nativeSubmission);
      live.openRec = 0;
    }
}

// CP, at the rotation into a CB third (after pm4WaitThirdFree, before any write of the new set): the retire assert
// (every recording that bound the third's views has been submitted and has completed), compare-on-retire (every
// recorded range unchanged since its bind), the poison of the third's used range, the new generation.
void D3D12Backend::zcBeginThird(uint32_t third) {
  if (!m_zcCheck || third >= 3u) return;
  uint64_t openRec = 0, ticket = 0;
  uint32_t oldGen = 0;
  m_zcRetireScratch.clear();
  {
    std::lock_guard<std::mutex> lk(m_zcMu);
    auto& live = m_zcLive[third];
    openRec = live.openRec;
    ticket = live.ticket;
    oldGen = live.gen;
    m_zcRetireScratch.swap(live.ranges);
    live.openRec = 0;
    live.ticket = 0;
    live.gen = ++m_zcGen[third];   // the generation this set publishes
  }
  uint64_t completed = 0;   // read after the ticket: a completion seen here covers every submission before it
  const bool known = SUCCEEDED(m_nativeImages.completionSnapshot(completed));
  // the deterministic fence assert: this rotation waited on THIS third's fence (pm4WaitThirdFree), once
  const bool waited = m_zcWaitedThird == third && m_zcWaits != m_zcWaitsSeen;
  m_zcWaitsSeen = m_zcWaits;
  if (!waited) {
    const uint64_t bad = m_zcFenceBad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[XLAT_CB_ZEROCOPY_CHECK] BAD retire assert: CB third %u (gen %u) reused without its own fence wait "
                  "(the last wait: third %u)", third, oldGen, m_zcWaitedThird);
  }
  if (openRec || (ticket && (!known || completed < ticket))) {
    const uint64_t bad = m_zcFenceBad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[XLAT_CB_ZEROCOPY_CHECK] BAD retire assert: third %u (gen %u) reused while %s (ticket %llu, "
                  "completed %llu)", third, oldGen,
                  openRec ? "a recording that bound its views is still open"
                          : "a submission that read its views is not complete",
                  (unsigned long long)ticket, (unsigned long long)completed);
  }
  const uint8_t* shadow = m_zcShadow.data();
  std::unique_lock<std::mutex> sl(m_zcShadowMu);
  for (const ZcRange& r : m_zcRetireScratch) {
    m_zcRetireChecked.fetch_add(1, std::memory_order_relaxed);
    if (xlatCbHash(shadow + r.off, r.use, r.bytes) != r.hash) {
      const uint64_t bad = m_zcRetireBad.fetch_add(1, std::memory_order_relaxed) + 1;
      if (bad <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[XLAT_CB_ZEROCOPY_CHECK] BAD retire third %u (gen %u) off=%llu bytes=%u: changed after its bind",
                    third, oldGen, (unsigned long long)r.off, r.bytes);
    }
  }
  sl.unlock();   // fill() takes the shadow mutex itself
  // the poison (0xFF bytes = NaN floats) of the retired generation's used CB range, twin + shadow
  const uint64_t lo = uint64_t(third) * (kXlatCBBytes / 3);
  const uint64_t hi = std::min(m_zcThirdHigh[third], lo + kXlatCBBytes / 3);
  if (hi > lo) m_xlatCBGpu.fill(lo, 0xFF, size_t(hi - lo));
  m_zcThirdHigh[third] = 0;
  m_zcRetired.fetch_add(1, std::memory_order_relaxed);
}

// CP, at a rotation: the finished set's used CB end becomes its third's poison range - only when the cursor lies inside
// that third (Astra r1 P3: replayPrepareTail rotates without resetting the cursor).
void D3D12Backend::zcNoteThirdHigh(uint32_t third, uint64_t used) {
  if (third >= 3u) return;
  const uint64_t lo = uint64_t(third) * (kXlatCBBytes / 3);
  m_zcThirdHigh[third] = (used > lo && used <= lo + kXlatCBBytes / 3) ? used : 0;
}
#endif

// Decoded vertices and fetch indices are native draw inputs. Publish a copy
// after any texture-induced flush, then retain it before recording its view.
// Raw shader vertex fetch is retained by the immutable texture-table version.
void D3D12Backend::bindXlatGeometry(const DrawBatch& b) {
  if (b.vsf && b.vsfNoIb) return;  // SV_VertexID supplies the auto-indexed draw
  if (!m_nativeRecording || !m_xlatVBPtr) nativeRecordingFail("geometry-recording", E_UNEXPECTED);
  if (!b.vertexCount || (!b.vsf && (!b.xlatStride ||
      b.xlatStride > D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES)))
    nativeRecordingFail("geometry-layout", E_INVALIDARG);
  const uint64_t size = uint64_t(b.vertexCount) * (b.vsf ? 4u : b.xlatStride);
  const uint64_t offset = b.vsf ? b.vsfIbOffset : b.vertexOffset;
  if (size > UINT32_MAX || offset > kXlatVBBytes || size > kXlatVBBytes - offset)
    nativeRecordingFail("geometry-staging-range", E_INVALIDARG);
  NativeBufferSlice geometry;
  HRESULT hr = m_nativeGeometry.stage({m_xlatVBPtr + offset, size_t(size)}, b.vsf ? 4u : 16u, geometry);
  if (FAILED(hr)) nativeRecordingFail("geometry-snapshot", hr);
  hr = m_nativeRecording->keep(geometry);
  if (FAILED(hr)) nativeRecordingFail("geometry-retain", hr);
  if (b.vsf) {
    const D3D12_INDEX_BUFFER_VIEW view{geometry.address(), uint32_t(size), DXGI_FORMAT_R32_UINT};
    m_commandList->IASetIndexBuffer(&view);
    ++m_nativeIndexBindings;
  } else {
    const D3D12_VERTEX_BUFFER_VIEW view{geometry.address(), uint32_t(size), b.xlatStride};
    m_commandList->IASetVertexBuffers(0, 1, &view);
    ++m_nativeVertexBindings;
  }
}

#if SR_ORACLE
void D3D12Backend::resourceCaptureFetch(DrawBatch& batch, const GuestDrawCommand& draw, const CachedShader& shader) {
  struct Config { bool on = false, automatic = false; uint32_t target = 0, cap = 64; };
  static const Config config = [] {
    Config cfg;
    const char* enable = std::getenv("SR_MEM_RES_FETCH_CAPTURE");
    if (!enable || std::strcmp(enable, "1") != 0) return cfg;
    if (!memres::census()) throw std::runtime_error("consumed-fetch diagnostic requires resource census mode");
    auto number = [](const char* name, int base, uint64_t max) {
      const char* raw = std::getenv(name);
      char* end = nullptr;
      const uint64_t value = raw ? std::strtoull(raw, &end, base) : max + 1;
      if (!raw || !*raw || !end || *end || value > max)
        throw std::runtime_error(std::string("invalid consumed-fetch diagnostic setting: ") + name);
      return uint32_t(value);
    };
    cfg.automatic = memres::fetchAutoTargetMode();
    if (!cfg.automatic) cfg.target = number("SR_MEM_RES_FETCH_TARGET", 16, 0x1FFFFFFCull);
    if (cfg.target & 3u) throw std::runtime_error("consumed-fetch target must be a physical dword address");
    if (std::getenv("SR_MEM_RES_FETCH_CAP")) cfg.cap = number("SR_MEM_RES_FETCH_CAP", 10, 128);
    if (!cfg.cap) throw std::runtime_error("consumed-fetch diagnostic cap must be positive");
    cfg.on = true;
    // The automated graceful exit uses quick_exit, not backend shutdown.
    // A static counter family survives backend lifetime and is re-rendered by
    // the existing terminal verifier after producers have been drained.
    sr::kernel::quit::addFamily("resource_fetch", resourceFetchReport);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_FETCH] enabled=1 target=%08X cap=%u automatic=%u scope=sampled_draw_candidates epoch_coverage=unknown",
        cfg.target, cfg.cap, cfg.automatic ? 1u : 0u);
    return cfg;
  }();
  if (!config.on) return;
  auto& counts = resourceFetchCounters();
  counts.considered.fetch_add(1, std::memory_order_relaxed);
  auto rejected = [&](std::atomic<uint64_t>& counter, const char* reason) {
    const uint64_t count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & (count - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[MEM_RES_FETCH] admission_rejected=%s count=%llu sampled_counter=1 epoch_coverage=unknown",
          reason, (unsigned long long)count);
  };
  memres::FetchTargetOrigin origin;
  if (config.automatic && !memres::fetchTargetForCurrentDraw(origin)) {
    rejected(counts.awaitingOrigin, "no_finalized_earlier_origin");
    return;
  }
  const uint32_t target = config.automatic ? origin.target : config.target;
  const uint32_t pageBegin = target & ~0xFFFu;
  const uint64_t pageEnd = uint64_t(pageBegin) + 4096u;
  bool overlaps = false, declaredDword = false;
  for (const uint32_t slot : shader.fetchSlots) {
    if (slot >= 96u) continue;
    const uint32_t first = draw.fetchRaw[slot * 2u], size = ((draw.fetchRaw[slot * 2u + 1u] >> 2u) & 0xFFFFFFu) * 4u;
    const uint32_t phys = first & 0x1FFFFFFCu;
    if ((first & 3u) != 3u || !size) continue;
    // A whole-page MISS can name metadata before this draw's declared data.
    // Admit that page as a diagnostic candidate, then retain the exact target
    // and let actual fetched intervals answer membership for this draw only.
    if (uint64_t(phys) < pageEnd && uint64_t(phys) + size > pageBegin) overlaps = true;
    if (target >= phys && uint64_t(target) < uint64_t(phys) + size) declaredDword = true;
  }
  if (!overlaps) { rejected(counts.noTargetPage, "no_declared_target_page"); return; }
  const uint64_t id = counts.pageCandidates.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (declaredDword) counts.declaredDword.fetch_add(1, std::memory_order_relaxed);
  if (id > config.cap) {
    counts.capped.fetch_add(1, std::memory_order_relaxed);
    if (id == uint64_t(config.cap) + 1u)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES_FETCH] cap_reached=%u later_draws=unknown", config.cap);
    return;
  }
  const uint32_t blocks = ((draw.vsConstUse & 0x10000u) || !(draw.vsConstUse & 0xFFFFu))
                              ? 0xFFFFu : draw.vsConstUse & 0xFFFFu;
  batch.resourceFetchDeclaredDword = declaredDword;
  resourceCaptureFetchInputs(batch, id, target, blocks);
  counts.captured.fetch_add(1, std::memory_order_relaxed);
  if (config.automatic) {
    batch.resourceFetchPayload = origin.payload;
    batch.resourceFetchOriginSequence = origin.sequence;
    batch.resourceFetchOriginMatched = origin.known && batch.resourceFetchOwnerKnown &&
        origin.ownerSum == batch.resourceFetchOwnerSum && origin.ownerSeq == batch.resourceFetchOwnerSeq &&
        origin.grantCount == batch.resourceFetchGrantCount && origin.grantDigest == batch.resourceFetchGrantDigest &&
        origin.grantGeneration == batch.resourceFetchGrantGeneration &&
        std::equal(origin.serial, origin.serial + 2, batch.resourceFetchGrantSerial);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_FETCH] origin_link=%llu captured=%llu matched=%u scope=later_command_only epoch_coverage=unknown",
        (unsigned long long)origin.sequence, (unsigned long long)id, batch.resourceFetchOriginMatched ? 1u : 0u);
  }
}

D3D12Backend::ResourceFetchCounters& D3D12Backend::resourceFetchCounters() {
  static ResourceFetchCounters counters;
  return counters;
}

void D3D12Backend::resourceFetchReport(const char* phase) {
  const auto& c = resourceFetchCounters();
  if (!c.considered.load(std::memory_order_relaxed)) return;
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_FETCH] admission_summary=%s considered=%llu awaiting_origin=%llu no_target_page=%llu "
      "page_candidates=%llu declared_dword=%llu captured=%llu capped=%llu epoch_coverage=unknown",
      phase, (unsigned long long)c.considered.load(), (unsigned long long)c.awaitingOrigin.load(),
      (unsigned long long)c.noTargetPage.load(), (unsigned long long)c.pageCandidates.load(),
      (unsigned long long)c.declaredDword.load(), (unsigned long long)c.captured.load(),
      (unsigned long long)c.capped.load());
}

memres::FetchCaptureInput D3D12Backend::resourceCurrentFetchInput(const DrawBatch& batch) const {
  memres::FetchCaptureInput input;
  input.drawId = batch.resourceFetchId;
  input.vsHash = batch.vsHash;
  input.indices = batch.vsfNoIb ? nullptr : reinterpret_cast<const uint32_t*>(m_xlatVBPtr + batch.vsfIbOffset);
  input.indexCount = batch.vertexCount;
  input.firstVertex = batch.vsfFirstVertex;
  input.indexed = !batch.vsfNoIb;
  input.stripCut = batch.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
  input.constants = reinterpret_cast<const uint32_t*>(m_xlatCBPtr + batch.xlatVsCbOff);
  input.fetch = reinterpret_cast<const uint32_t*>(m_xlatCBPtr + batch.vsfFetchCbOff);
  input.constantBlocks = batch.resourceFetchConstantBlocks;
  return input;
}

void D3D12Backend::resourceCaptureConsumer(DrawBatch& batch, const GuestDrawCommand& command) {
  static std::atomic<uint64_t> nextId{0};
  batch.resourceConsumerId = nextId.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (!batch.pm4 || !batch.vsf || !batch.vsfShm)
    throw std::runtime_error("native consumer check has no supported shared-memory draw");
  bool changed = false;
  for (const auto& check : batch.resourceConsumerChecks) {
    if (!memres::resourceConsumerBind(check, batch.resourceConsumerId, batch.vsHash))
      throw std::runtime_error("native consumer ticket could not bind its draw identity");
    changed |= check->changedWords != 0;
  }
  if (!changed) return;  // Equal pages need the recorded baseline/owner check only.
  batch.resourceConsumerFetchLease = memres::resourceConsumerAcquireFetch();
  if (!batch.resourceConsumerFetchLease)
    throw std::runtime_error("native consumer fetch capture budget exhausted");
  batch.resourceConsumerConstantBlocks = (command.vsConstUse & 0x10000u) || !(command.vsConstUse & 0xFFFFu)
      ? 0xFFFFu : command.vsConstUse & 0xFFFFu;
  auto input = resourceCurrentFetchInput(batch);
  input.drawId = batch.resourceConsumerId;
  input.constantBlocks = batch.resourceConsumerConstantBlocks;
  batch.resourceConsumerFetch = memres::captureFetchDraw(input);
}

void D3D12Backend::resourceCaptureFetchInputs(DrawBatch& batch, uint64_t id, uint32_t target,
                                            uint32_t constantBlocks) {
  batch.resourceFetchId = id;
  batch.resourceFetchTarget = target;
  batch.resourceFetchConstantBlocks = constantBlocks;
  batch.resourceFetchCapture = memres::captureFetchDraw(resourceCurrentFetchInput(batch));
  const auto owner = sr::kernel::physwin::ownerVersion(target, 4);
  const auto grants = sr::kernel::physwin::ownersOfSpan(target, 4);
  const auto confirmed = sr::kernel::physwin::ownerVersion(target, 4);
  batch.resourceFetchOwnerKnown = owner.ok && confirmed.ok && owner.sum == confirmed.sum &&
                                  owner.cbSeq == confirmed.cbSeq && grants.nLive != 0;
  batch.resourceFetchOwnerSum = owner.sum;
  batch.resourceFetchOwnerSeq = owner.cbSeq;
  batch.resourceFetchGrantDigest = grants.setDigest;
  batch.resourceFetchGrantGeneration = grants.gen;
  batch.resourceFetchGrantCount = grants.nLive;
  std::copy_n(grants.serial, 2, batch.resourceFetchGrantSerial);
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_FETCH] captured=%llu ordinal=%u vs=%016llX indices=%u no_ib=%u shm=%u target=%08X "
      "owner=%llu/%u/%u grants=%u/%u/%u digest=%llu generation=%llu declared_dword=%u "
      "admission=target_page consumption=unconfirmed",
      (unsigned long long)id, batch.drawOrdinal, (unsigned long long)batch.vsHash, batch.vertexCount,
      batch.vsfNoIb ? 1u : 0u, batch.vsfShm ? 1u : 0u, target,
      (unsigned long long)owner.sum, owner.cbSeq, batch.resourceFetchOwnerKnown ? 1u : 0u,
      grants.nLive, grants.serial[0], grants.serial[1], (unsigned long long)grants.setDigest,
      (unsigned long long)grants.gen, batch.resourceFetchDeclaredDword ? 1u : 0u);
}
#endif

void D3D12Backend::xlatBindRootTables() {
  m_commandList->SetGraphicsRootSignature(m_nativeXlatRoot.root());
  xlatSetPixelPosScale();
  // Every draw binds and retains its complete texture/sampler table version.
}

// P1: the render-time texture bind of the xlat loop, without its pending-
// resolve flush machinery (the EDRAM path resolves synchronously into guest
// memory and evicts, so every bind here already sees post-resolve bytes).
// SharedConstants: c0..c3 Texture2D indices = dwords 0..15, c12..c15 sampler
// indices = dwords 48..63 (iter-411).
// iter-460 perf census: per-frame time inside the bind by texture path.
double g_bindMs2D = 0, g_bindMs3D = 0, g_bindMsArr = 0;
uint32_t g_bindN2D = 0, g_bindN3D = 0, g_bindNArr = 0;
extern const bool g_pm4DiagTiming;  // d3d12_pm4_edram.cpp (SR_PM4_DIAG_TIMING)

// Step-4 lane C (from iter-467 p2, research/p2 5007e489): SR_XLAT_SAMPLER_W - the W bit. Baked ON after the replay
// matrix (lane C NOTES 2026-10-18: gp2 vs Xenia 2.831 -> 0.479, window region 7.315 -> 0.981; 9/9 default identical
// before the bake); =0 keeps the old W CLAMP for an A/B in the dev build, the release has no such arm.
static bool xlatSamplerWOn() {
#if SR_ORACLE
  static const bool s_on = [] {
    const char* e = std::getenv("SR_XLAT_SAMPLER_W");
    return !e || e[0] != '0';
  }();
  return s_on;
#else
  return true;
#endif
}

// The PM4 bind path's sampler-table index (layout in translated_samplers.hpp): clampU 4, clampV 2, linear 1, and
// bit 3 = W repeats under SR_XLAT_SAMPLER_W. Every PM4 sampler had AddressW CLAMP, so a volume addressed with a
// world-derived w (the 64^3 noise volumes behind the building windows and car paint, clamp_z = kRepeat in gp2)
// sampled its edge slices wherever w left [0, 1]. Xenia (d3d12_texture_cache.cc WriteSampler: AddressW from
// clamp_z; texture_util.cc GetClampModesForDimension: clamp_z is read only for k3D, CLAMP otherwise) - so the bit
// is set for a k3D fetch only (dword_5 bits 9-10 == 2). clamp_z 0 / 1 (repeat, mirrored repeat) repeat, the same
// rule clampU / clampV already follow (the table has no MIRROR variants).
static inline uint32_t pm4SamplerIndex(const uint32_t* tf) {
  const uint32_t clampX = (tf[0] >> 10) & 7u, clampY = (tf[0] >> 13) & 7u;
  const uint32_t magF = (tf[3] >> 19) & 3u;
  uint32_t idx = (clampX >= 2u ? 4u : 0u) | (clampY >= 2u ? 2u : 0u) | (magF != 0u ? 1u : 0u);
  if (xlatSamplerWOn() && ((tf[5] >> 9) & 3u) == 2u && ((tf[0] >> 16) & 7u) < 2u) idx |= 8u;
  return idx;
}

// Step-4 lane C round 2 (SR_XLAT_SAMPLER_V2, gfx/volume_gates.hpp): a volume (k3D) fetch samples through its own
// full sampler - Xenia's GetSamplerParameters + WriteSampler (translated_samplers.cpp volumeSamplerParams /
// volumeSamplerDesc): U / V / W clamp incl. mirror, mirror-once and border with the border colour, min / mag / mip
// filters, anisotropy, MinLOD = mip_min_level, kBaseMap as MaxLOD = MinLOD (+0.25) over the whole chain. The
// legacy 16 (pm4SamplerIndex) keep every other fetch, and a volume while the gate is off or the 48 volume entries
// are used up (logged).
uint32_t D3D12Backend::xlatSamplerIndex(const uint32_t* tf) {
  if (sr::gfx::volume::samplerV2On() && (tf[0] & 3u) == 2u && ((tf[5] >> 9) & 3u) == 2u) {
    const uint32_t v = xlatVolumeSamplerIndex(tf);
    if (v) return v;
  }
  return pm4SamplerIndex(tf);
}

uint32_t D3D12Backend::xlatVolumeSamplerIndex(const uint32_t* tf) {
  const VolumeSamplerParams p = volumeSamplerParams(tf);
  const uint32_t key = p.key();
  const bool v3 = sr::gfx::volume::samplerV3On();
  const uint64_t frameFence = m_fenceValues[m_frameIndex];
  if (auto it = m_volSamplerIndex.find(key); it != m_volSamplerIndex.end()) {
    if (v3) m_volSamplerLastUse[it->second - kXlatLegacySamplers] = frameFence;
    return it->second;
  }
  if (!m_xlatSamplerHeap) return 0u;
  uint32_t index = 0;
  if (m_volSamplers.size() < kXlatVolumeSamplers) {
    index = kXlatLegacySamplers + uint32_t(m_volSamplers.size());
    m_volSamplers.push_back(p);
    m_volSamplerLastUse.push_back(frameFence);
  } else if (v3) {
    // SR_XLAT_SAMPLER_V3: reclaim the least recently used entry whose last frame has completed on the GPU - no list
    // in flight reads that index in any block, so block 0 and the block in use are rewritten in place (the other
    // ring blocks are rewritten whole when they come round, xlatWriteSamplerBlock) and the native path builds a new
    // immutable version (m_volSamplerGen). Only when every entry was bound in a frame still in flight (more than 48
    // distinct volume samplers inside the frames in flight) does the fetch fall back to its legacy recipe.
    const uint64_t completed = m_fence ? m_fence->GetCompletedValue() : 0u;
    const size_t lru = reclaimableVolumeSampler(m_volSamplerLastUse, completed);
    if (lru != SIZE_MAX) {
      const uint64_t lastUse = m_volSamplerLastUse[lru];
      const uint32_t oldKey = m_volSamplers[lru].key();
      m_volSamplerIndex.erase(oldKey);
      m_volSamplers[lru] = p;
      m_volSamplerLastUse[lru] = frameFence;
      index = kXlatLegacySamplers + uint32_t(lru);
      if (m_volSamplerReclaims++ < 8)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[XLAT_SAMPLER] volume sampler table full: entry %u reclaimed from key %08X (last bound in the "
                    "frame of fence %llu, completed %llu) for key %08X",
                    index, oldKey, (unsigned long long)lastUse, (unsigned long long)completed, key);
    }
  }
  if (!index) {
    if (m_volSamplerFallbacks++ < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[XLAT_SAMPLER] volume sampler table full (%u%s): key %08X uses the legacy recipe", kXlatVolumeSamplers,
                  v3 ? ", every entry bound in a frame still in flight" : "", key);
    return 0u;
  }
  m_volSamplerIndex.emplace(key, index);
  m_volSamplerGen++;  // the native path builds a new immutable sampler version on its next draw
  // The legacy heap: the new entry in block 0 (no options) and in the block in use (its options). No recorded draw
  // has used this index yet; a ring block written later carries every entry (xlatWriteSamplerBlock).
  const auto sCpu = m_xlatSamplerHeap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_SAMPLER_DESC d0 = volumeSamplerDesc(p, 0u, 0.0f);
  m_device->CreateSampler(&d0, {sCpu.ptr + SIZE_T(index) * m_sampInc});
  if (m_sampBlock) {
    const D3D12_SAMPLER_DESC dc = volumeSamplerDesc(p, m_sampAniso, m_sampLodBias);
    m_device->CreateSampler(&dc, {sCpu.ptr + SIZE_T(m_sampBlock * kXlatSamplerTable + index) * m_sampInc});
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[XLAT_SAMPLER] volume sampler %u: clamp %u/%u/%u border %u filter mag %u min %u mip %u basemap %u "
              "aniso %u minLOD %u (fetch %08X %08X %08X %08X %08X %08X)",
              index, p.clampX, p.clampY, p.clampZ, p.borderColor, p.magLinear ? 1 : 0, p.minLinear ? 1 : 0,
              p.mipLinear ? 1 : 0, p.mipBaseMap ? 1 : 0, p.aniso, p.mipMinLevel, tf[0], tf[1], tf[2], tf[3], tf[4],
              tf[5]);
  return index;
}

void D3D12Backend::xlatBindBatchTextures(const DrawBatch& b) {
  // iter-467 native-b: the RT-cache gate read once per call, not per slot (render-thread profile: pm4RtCacheEnabled
  // 0.12 ms self per 3x frame - a cross-TU call with a static-init guard, ~100 slots x 4 sites per batch).
  const bool rtc = pm4RtCacheEnabled();
  uint32_t* sharedTex = reinterpret_cast<uint32_t*>(m_xlatCBPtr + b.sharedCbOff);
  uint32_t* sharedSmp = sharedTex + 48;
  using PClk = std::chrono::steady_clock;
  auto msSince = [](PClk::time_point t0) {
    return g_pm4DiagTiming ? std::chrono::duration<double, std::milli>(PClk::now() - t0).count() : 0.0;
  };
  auto stamp = [] { return g_pm4DiagTiming ? PClk::now() : PClk::time_point{}; };
  // iter-467 native-b B-CPU (SR_NB_BIND_MEMO, baked ON after cr2; =0 off): see BindMemo (d3d12_backend.hpp).
  static const bool s_memoEnv = [] {
    const char* e = std::getenv("SR_NB_BIND_MEMO");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON (=0 A/B)
  }();
  static const int s_memoAb = sr::abt::reg("SR_NB_BIND_MEMO");  // SR_AB_TOGGLE: read per call
  const bool s_memo = s_memoEnv && (s_memoAb < 0 || sr::abt::on(s_memoAb));
  if (m_texEvictOn) {
    texEvictTick();       // step 4 G7 (SR_TEX_CACHE_EVICT): once per frame
    m_texPinnedN = 0;     // fix2 (Astra P1): this draw's selections pin their slots from here
  }
  struct PinScope {  // fix3 (Astra 0a384c24 P3): ... until this draw's selection ends, on every exit
    size_t& n;
    ~PinScope() { n = 0; }
  } pinScope{m_texPinnedN};
  for (uint32_t t = 0; t < 16; t++) {
    const uint32_t* tf = &b.texFetch[t * 6];
    if (s_memo && rtc) {
      BindMemo& m = m_bindMemo[t];
      // A plain 2D answer is the clean set's (gen covers it); a stacked / cube / volume answer also depends on the
      // array lookup's own epoch / clock short-cut.
      if (m.gen == m_texBindGen.load(std::memory_order_relaxed) && std::memcmp(m.f, tf, 24) == 0 &&
          ((m.kind == 1u && !m.arr) || (m.clock == m_shmStampClock && m.epoch == m_shmEpoch))) {
        m_bindMemoHits++;
#if SR_ORACLE
        m_texTrack.armed = false;  // what the lookups' fast paths leave behind
        m_texTrackLast = 0;
#endif
        sharedSmp[t] = xlatSamplerIndex(tf);
        if (m.kind == 2u) {
          const uint32_t dim = (tf[5] >> 9) & 3u;
          sharedTex[t] = 0u;
          sharedTex[16 + t] = (dim == 2u && m.s3) ? (0xA0000000u | m.s3) : 0u;
          sharedTex[32 + t] = (dim == 3u && m.s3) ? (0x80000000u | m.s3) : 0u;
        } else {
          if (m_texEvictOn) texEvictUse(m.raw2d);  // step 4 G7: a memo hit is a bind of its slot (+ its pin)
          const uint32_t tex2D = glyphRemap(m.raw2d, tf);  // the prompt glyph swap follows the input device
          sharedTex[32 + t] = 0u;
          sharedTex[t] = tex2D;
          sharedTex[16 + t] = m.arr ? (0xC0000000u | m.arr) : (0x80000000u | tex2D);
        }
        continue;
      }
      m_bindMemoMiss++;
      m.gen = ~0ull;  // re-armed below only when the answer came from the lookups' fast paths
    }
    // P1: the fetch constant's DIMENSION (dword_5 bits 9-10: 0 1D, 1 2D or
    // stacked, 2 3D, 3 cube) picks the resource, as in Xenia. Volumes and
    // cubes live in the 2D slot band and are reached through the space7
    // Texture3D / space8 TextureCube tables (bit 31 | bit 29 in the 3D dword,
    // bit 31 in the cube dword; tfetch3D/tfetchCube in shader_common.h).
    const uint32_t dim = (tf[5] >> 9) & 3u;
    if (rtc && (tf[0] & 3u) == 2u && (dim == 2u || dim == 3u)) {
      const auto t0 = stamp();
      const uint32_t s = dim == 2u ? getOrCreateTexture3D(tf) : getOrCreateTexture2DArray(tf, true);
      if (dim == 2u) { g_bindMs3D += msSince(t0); g_bindN3D++; } else { g_bindMsArr += msSince(t0); g_bindNArr++; }
      // SR_NB_BIND_MEMO: a volume from the clean set; a cube array answered by its own epoch / clock short-cut
      // (checked again under the same epoch and clock it is that short-cut's answer).
      if (s_memo && s && (dim == 2u ? m_texBindFast : true)) {
        BindMemo& m = m_bindMemo[t];
        std::memcpy(m.f, tf, 24);
        m.s3 = s;
        m.kind = 2u;
        m.gen = m_texBindGen;
        m.clock = m_shmStampClock;
        m.epoch = m_shmEpoch;
      }
      sharedTex[t] = 0u;
      sharedTex[16 + t] = (dim == 2u && s) ? (0xA0000000u | s) : 0u;
      sharedTex[32 + t] = (dim == 3u && s) ? (0x80000000u | s) : 0u;
      sharedSmp[t] = xlatSamplerIndex(tf);
      continue;
    }
    if (rtc) sharedTex[32 + t] = 0u;
    const bool is2D = (tf[0] & 3u) == 2u;
    const auto t2d = stamp();
    // iter-465 perf2: sharedTex / sharedSmp point into the constant-buffer UPLOAD heap
    // (write-combined): the slot value is kept in a register and only ever STORED there.
    // Reading it back (the 16+t entry below did) is an uncached WC read that also flushes
    // the write-combining buffers: 27.6% of the render thread on that one line (raw-PC
    // profile, archive/v1_perf2/r_lp_1).
    const uint32_t raw2D = is2D ? getOrCreateTexture2D(tf) : 0u;
    const bool fast2D = m_texBindFast;
    const uint32_t tex2D = is2D ? glyphRemap(raw2D, tf) : 0u;  // iter-464 prompts
    sharedTex[t] = tex2D;
    if (is2D) { g_bindMs2D += msSince(t2d); g_bindN2D++; }
    // Dwords 16..31 = the 3D-table indices. A "3D or stacked" tfetch of a
    // texture we hold as 2D samples it through the 2D table (bit 31, see
    // tfetch3D in tools/XenosRecomp shader_common.h); real volumes have no
    // xlat-path upload yet, so this is also their best available read.
    // A STACKED 2D texture (size_2d.stack_depth > 0: the shadow cascades are
    // one 640x640x4 k_16 array) goes through the array table (bit 30), so the
    // fetch's denormalized Z selects the layer as on Xenos.
    uint32_t arr = 0;
    if (is2D && ((tf[2] >> 26) & 0x3Fu) != 0u && ((tf[5] >> 9) & 3u) == 1u) {
      const auto ta = stamp();
      arr = getOrCreateTexture2DArray(tf);
      g_bindMsArr += msSince(ta); g_bindNArr++;
    }
    sharedTex[16 + t] = arr ? (0xC0000000u | arr) : is2D ? (0x80000000u | tex2D) : 0u;
    sharedSmp[t] = xlatSamplerIndex(tf);
    // SR_NB_BIND_MEMO: a 2D answer from the clean set (with its stacked twin, which the array lookup's own epoch /
    // clock short-cut answers again under the same epoch and clock).
    if (s_memo && rtc && is2D && fast2D && raw2D) {
      BindMemo& m = m_bindMemo[t];
      std::memcpy(m.f, tf, 24);
      m.raw2d = raw2D;
      m.arr = arr;
      m.kind = 1u;
      m.gen = m_texBindGen;
      m.clock = m_shmStampClock;
      m.epoch = m_shmEpoch;
    }
  }
}

// iter-358k: the drain decodes REAL draws — real vertex layout (posOffset/
// posFormat/colorFormat from the VS vfetch bindings), real index buffers
// (DRAW_INDX kDMA: u16/u32 + endian from the packet), real primitive types
// (strip / list / rectlist expansion), and a per-draw viewport snapshot fed
// to the shader as root constants. The former heuristic branches (fetch-slot
// fallback, FETCH_BLOCK stride guessing, 128MB pool scans, pending-draw
// probing) are DELETED — decode from the draw's own data or skip it, never
// guess.
// step 6 R6 fix round 2 (Astra r2 P1): the drain census counters are the draining thread's single-writer atomics
// (drainreuse::add: a relaxed load + store, never reset; [PM4_BIND_CENSUS] reports deltas).
extern std::atomic<double> g_drainMsPrep, g_drainMsVsfFetch, g_drainMsVsfIb, g_drainMsDecode;
extern std::atomic<uint32_t> g_drainN, g_drainCalls;
extern std::atomic<uint32_t> g_drainDecRect, g_drainDecClip, g_drainDecOther;
bool nbCoverFillOn();            // gfx/d3d12_frame_render.cpp: SR_NB_COVER_FILL (A/B-togglable) - the reuse key's bit
bool pm4NbClearElideOnForKey();  // gfx/d3d12_frame_render.cpp: SR_NB_CLEAR_ELIDE (A/B-togglable) - the reuse key's bit

bool D3D12Backend::vsfShmSrvCreate() {
  if (m_vsfShmSrvReady) return true;
  if (!m_shm || !m_xlatSrvHeap) return false;
  D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
  vd.Format = DXGI_FORMAT_R32_TYPELESS;
  vd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  vd.Buffer.FirstElement = 0;
  vd.Buffer.NumElements = UINT(m_shm->GetDesc().Width / 4u);
  vd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE h = m_xlatSrvHeap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += SIZE_T(kVsfShmSrvSlot) * inc;
  publishXlatSrv(m_shm.Get(), &vd, h);
  m_vsfShmSrv = m_xlatSrvHeap->GetGPUDescriptorHandleForHeapStart();
  m_vsfShmSrv.ptr += uint64_t(kVsfShmSrvSlot) * inc;
  m_vsfShmSrvReady = true;
  return true;
}

// iter-466 cp round 2 (SR_CP_DRAIN_LEAN): the drain's per-draw diagnostic counters (log-N
// samplers) are only bumped on the CP thread. A fetch_add there is a locked instruction per draw,
// which also waits for the draw's write-combined upload stores to drain (~2.6% of the CP on
// one counter); with the gate the bump is a plain load + store (the same values single-threaded).
static bool cpDiagLean() {
  static const bool v = cpsimd::gateLogged("SR_CP_DRAIN_LEAN", true);
  return v;
}
static uint32_t cpDiagBump(std::atomic<uint32_t>& a) {
  if (!cpDiagLean()) return a.fetch_add(1, std::memory_order_relaxed);
  const uint32_t v = a.load(std::memory_order_relaxed);
  a.store(v + 1u, std::memory_order_relaxed);
  return v;
}

// iter-466 cp round 2 (SR_CP_CONST_TRACK, gfx/xenos_state.hpp): a tracked command's changed
// constant blocks into the drain's copy of every block, in sequence. A command out of sequence
// (none expected: one FIFO, one consumer) is counted and asks the producer to re-send every
// used block. SR_CP_CONST_TRACK_CHECK=1: the command carries every used block; compare.
void D3D12Backend::cvMerge(const GuestDrawCommand& cmd) {
  if (cmd.constSeq != m_cvSeq + 1u) {
    m_cvGaps++;
    if (m_cvGaps <= 8 || (m_cvGaps & (m_cvGaps - 1u)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[CONST_TRACK] GAP #%llu: command seq %u after %u",
                  (unsigned long long)m_cvGaps, cmd.constSeq, m_cvSeq);
    getDrawCommandQueue().requestConstResync();
  }
  m_cvSeq = cmd.constSeq;
  for (uint32_t m = cmd.constPresent; m;) {
    const uint32_t b = uint32_t(__builtin_ctz(m));
    m &= m - 1u;
    const uint32_t* src = b < 16u ? cmd.aluVs + 64u * b : cmd.aluPs + 64u * (b - 16u);
    std::memcpy(m_cvCons + 64u * b, src, 256);
  }
  m_cvUpDirty |= cmd.constPresent;
  static const bool s_check = cpsimd::gate("SR_CP_CONST_TRACK_CHECK", false);
  if (s_check) {
    auto full = [](uint32_t u) { return ((u & 0x10000u) || (u & 0xFFFFu) == 0u) ? 0xFFFFu : (u & 0xFFFFu); };
    const uint32_t used = full(cmd.vsConstUse) | (full(cmd.psConstUse) << 16);
    for (uint32_t m = used; m;) {
      const uint32_t b = uint32_t(__builtin_ctz(m));
      m &= m - 1u;
      const uint32_t* src = b < 16u ? cmd.aluVs + 64u * b : cmd.aluPs + 64u * (b - 16u);
      if (std::memcmp(m_cvCons + 64u * b, src, 256) != 0) {
        m_cvMismatch++;
        if (m_cvMismatch <= 16)
          SR_LOG_BOOT(sr::LogSeverity::Error, "[CONST_TRACK] MISMATCH #%llu seq=%u block=%u present=%08X",
                      (unsigned long long)m_cvMismatch, cmd.constSeq, b, cmd.constPresent);
      }
    }
  }
}

// NATIVE strand A (SR_NATIVE_DRAIN_REUSE; step 6 R6 bake, Fable ruling 23 (2): default ON, =0 off - the
// 10-08 rejection (622199d2: no gain while prepare cost ~0.6 ms / frame) no longer holds, see NOTES 10-22): a drained
// draw whose pipeline / target /
// viewport inputs are the previous draw's in the same drain (82% of gameplay draws, census dc1)
// reuses that draw's pm4PrepareDraw and pm4ViewportScissor results instead of recomputing them.
// Consecutive means the IMMEDIATELY previous command the drain consumed (step 6 R6, Astra round 1 P3): every command
// and every drain ENTRY bump t_drainSeq (fix round 2, Astra r2 P1: gfx/drain_reuse.hpp), so a non-candidate draw,
// an early exit, an early-returning drain or a drain boundary in between invalidates the entry; no drain counter
// takes part. The resolution scale, g_rtHeightScaleY and the two coverage gates are keyed (fix round 2, P2a), and the
// unkeyed copy / clear / resolve Regs fields are refreshed from the current command on a hit.
// The key is every command field the two read (pm4RegsFromDraw's set, the shader pair, the prim and
// path flags, poly offset, the four blend controls); rect lists (prim 8: B1's clear detection reads
// their vertices) are never reused; the cache lives for one drain on the drain's thread (PSO
// pointers and the resolution scale are stable inside it). SR_NATIVE_DRAIN_REUSE_CHECK=1 recomputes
// every reused draw and compares ([NATIVE_DRAIN_REUSE] BAD). In-roll A/B: SR_AB_TOGGLE=SR_NATIVE_DRAIN_REUSE.
namespace {
bool drainReuseOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NATIVE_DRAIN_REUSE");
    return !e || e[0] != '0';
  }();
  static const int ab = sr::abt::reg("SR_NATIVE_DRAIN_REUSE");
  return v && (ab < 0 || sr::abt::on(ab));
}
bool drainReuseCheck() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NATIVE_DRAIN_REUSE_CHECK");
    return e && e[0] == '1';
  }();
  return v;
#else
  return false;  // SR_ORACLE=0 (kernel/sr_oracle.hpp): the CHECK is compiled out; SR_NATIVE_DRAIN_REUSE_CHECK reads as unset
#endif
}
struct DrainReuse {
  uint64_t seq = 0;  // step 6 R6 (Astra r1 P3, r2 P1): t_drainSeq.v of the command that stored or last reused the entry
  uint32_t n = 0;
  uint32_t k[48] = {};
  bool prepared = false;
  edram::Regs regs;
  edram::DrawTargets t;
  ID3D12PipelineState* pso = nullptr;
  ID3D12PipelineState* markPso = nullptr;
  bool vpValid = false, vpOk = false;
  D3D12_VIEWPORT vp{};
  float ndc[4] = {};
  D3D12_RECT sc{};
};
thread_local DrainReuse t_reuse;
thread_local drainreuse::Seq t_drainSeq;  // step 6 R6: bumped at every drain entry and every command consumed (monotonic)
#if SR_ORACLE
// step 6 R6 CHECK (Astra r1 P2): the fresh prepare's outputs on a reuse hit, which the viewport CHECK recomputes from
// (thread_local storage: no per-draw construction cost; only the CHECK writes them).
thread_local edram::Regs t_chkRegs;
thread_local edram::DrawTargets t_chkTargets;
#endif
std::atomic<uint64_t> g_reuseHits{0}, g_reuseMiss{0}, g_reuseChecked{0}, g_reuseBad{0};
#if SR_ORACLE
// step 6 drop fix (the coordinator 10-23): a translated draw dropped at its empty viewport AFTER its VS / PS block
// writes had left m_xlatCBUsed below those blocks while the CP's reuse state (SR_CP_CONST_TRACK's m_cvUpVsOff /
// m_cvUpPsOff, SR_CP_CONST_DEDUP's m_cdVsOff / m_cdPsOff) named them: the next draw's own allocation started on them
// and overwrote a block a later draw then bound. Fixed: the drop advances the cursor past its blocks. The census:
// drops after a write, reuses of such a dropped draw's block (the old build's corrupted binds), and the assert that
// closes the class - every reused CB block (VS / PS / the vsf fetch block) and every reused decoded VB range lies
// wholly below its cursor. SR_XLAT_DROP_MUTANT=1 (TEST-ONLY) removes the fix: the assert must catch it.
std::atomic<uint64_t> g_dropAfterWrite{0}, g_dropThenReuse{0}, g_reuseCursorChecked{0}, g_reuseCursorBad{0},
    g_dropOffsetsLost{0};
bool xlatDropMutant() {
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_DROP_MUTANT");
    const bool v = e && e[0] == '1';
    if (v) SR_LOG_BOOT(sr::LogSeverity::Warn, "[XLAT_DROP] MUTANT: the empty-viewport drop leaves the CB cursor below its blocks");
    return v;
  }();
  return on;
}
// The assert: a reused block [off, off + bytes) must lie wholly below the cursor `used` (what = vs / ps / fetch / vb).
void xlatReuseBelowCursor(uint64_t off, uint64_t bytes, uint64_t used, const char* what, uint32_t ordinal) {
  g_reuseCursorChecked.fetch_add(1, std::memory_order_relaxed);
  if (off + bytes <= used) return;
  const uint64_t bad = g_reuseCursorBad.fetch_add(1, std::memory_order_relaxed) + 1;
  if (bad <= 16)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[XLAT_DROP] BAD reuse above the cursor: draw#%u %s block [%llu, %llu) with the cursor at %llu (this "
                "draw's own allocation starts on it)", ordinal, what, (unsigned long long)off,
                (unsigned long long)(off + bytes), (unsigned long long)used);
}
void xlatDropLine(const char* when) {
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[XLAT_DROP]%s drop_after_write=%llu drop_then_reuse=%llu reuse_checked=%llu reuse_above_cursor_BAD=%llu "
              "drop_offsets_lost=%llu mutant=%d", when, (unsigned long long)g_dropAfterWrite.load(),
              (unsigned long long)g_dropThenReuse.load(), (unsigned long long)g_reuseCursorChecked.load(),
              (unsigned long long)g_reuseCursorBad.load(), (unsigned long long)g_dropOffsetsLost.load(),
              xlatDropMutant() ? 1 : 0);
}
#endif
#if SR_ORACLE
// step 6 R6 fix rounds 3 + 4 (the coordinator; Astra r3's "external invariant", r4 P3): no two drains may OVERLAP.
// The reuse state (t_reuse / t_drainSeq) is per thread and every drain's entry bump invalidates that thread's entry,
// so drains SERIALIZED on different threads are correct (SR_CP_DRAIN_ASYNC's worker and the SR_NATIVE_EXEC executor
// hand drains between threads under s_liveMu, gfx/null_renderer.cpp), and the census counters' load + store is safe
// under that serialization. What breaks R6 and the single-writer counters is two drains IN FLIGHT at once (or a drain
// re-entered from inside one). The oracle asserts that: an in-flight count around the whole drain; entering while
// another drain is in flight counts as a CHECK BAD and logs "[NATIVE_DRAIN_REUSE] BAD overlapping drain" (the CHECK
// rolls' BAD = 0 / no-BAD-line rule covers it). Two atomic RMWs per DRAIN (not per draw); compiled out of the release.
std::atomic<int> g_drainsInFlight{0};
struct DrainOverlapGuard {
  DrainOverlapGuard() {
    if (g_drainsInFlight.fetch_add(1, std::memory_order_acq_rel) != 0) {
      const uint64_t bad = g_reuseBad.fetch_add(1, std::memory_order_relaxed) + 1;
      if (bad <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_DRAIN_REUSE] BAD overlapping drain (thread %u: another drain is in flight)",
                    unsigned(GetCurrentThreadId()));
    }
  }
  ~DrainOverlapGuard() { g_drainsInFlight.fetch_sub(1, std::memory_order_acq_rel); }
  DrainOverlapGuard(const DrainOverlapGuard&) = delete;
  DrainOverlapGuard& operator=(const DrainOverlapGuard&) = delete;
};
#endif
void reuseLine(const char* when) {
  const uint64_t n = g_reuseHits.load(std::memory_order_relaxed) + g_reuseMiss.load(std::memory_order_relaxed);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_DRAIN_REUSE]%s draws=%llu reused=%llu (%.1f%%) | CHECK checked=%llu BAD=%llu",
              when, (unsigned long long)n, (unsigned long long)g_reuseHits.load(),
              n ? 100.0 * double(g_reuseHits.load()) / double(n) : 0.0, (unsigned long long)g_reuseChecked.load(),
              (unsigned long long)g_reuseBad.load());
}
void reuseReport() {
  const uint64_t n = g_reuseHits.load(std::memory_order_relaxed) + g_reuseMiss.load(std::memory_order_relaxed);
  if ((n & 0x3FFFFu) != 0) return;
  reuseLine("");
}
// step 4 item 7d (lane D, diagnostic, SR_XLAT_HWM=1; default off = one static bool per set): when a third of the
// translated-draw scratch rotates, its use is final; log each new per-third high-water (1 MB steps) so the
// zero-filled kXlatVBBytes / kXlatCBBytes (512 + 192 MB, 3 thirds) can be sized by measurement.
void xlatHighWater(uint32_t third, uint64_t vbUsed, uint64_t cbUsed, uint64_t vbBytes, uint64_t cbBytes) {
  static const bool on = [] {
    const char* e = std::getenv("SR_XLAT_HWM");
    return e && e[0] == '1';
  }();
  if (!on) return;
  static uint64_t vbPeak = 0, cbPeak = 0, vbLogged = 0, cbLogged = 0;
  static uint32_t sets = 0;
  ++sets;
  const uint64_t vbBase = uint64_t(third) * (vbBytes / 3), cbBase = uint64_t(third) * (cbBytes / 3);
  if (vbUsed < vbBase || cbUsed < cbBase) return;
  vbPeak = std::max(vbPeak, vbUsed - vbBase);
  cbPeak = std::max(cbPeak, cbUsed - cbBase);
  if (sets == 1 || vbPeak >= vbLogged + (1ull << 20) || cbPeak >= cbLogged + (1ull << 20)) {
    vbLogged = vbPeak;
    cbLogged = cbPeak;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[XLAT_HWM] set %u: per-third peak vb %.1f of %.1f MB, cb %.1f of %.1f MB",
                sets, double(vbPeak) / 1048576.0, double(vbBytes / 3) / 1048576.0, double(cbPeak) / 1048576.0,
                double(cbBytes / 3) / 1048576.0);
  }
}
}  // namespace

// step 6 R6 bake: the [NATIVE_DRAIN_REUSE] line at the [NATIVE_RECORDING] cadence and at shutdown, so a short roll or a
// single-frame replay trace shows reused= / CHECK checked= (the 2^18-draw cadence never fires there).
void drainReuseCadenceLine(const char* when) {
  if (drainReuseOn() || drainReuseCheck()) reuseLine(when);
}
#if SR_ORACLE
void xlatDropCadenceLine(const char* when) { xlatDropLine(when); }   // step 6 drop fix: every roll / trace shows it
#endif

void D3D12Backend::drainDrawQueue() {
  t_drainSeq.drainEntry();  // step 6 R6 fix round 2 (Astra r2 P1): first, before every early return of this drain
#if SR_ORACLE
  const DrainOverlapGuard drainOverlapGuard;  // step 6 R6 fix round 4: no overlapping drain (covers every return below)
#endif
  auto& queue = getDrawCommandQueue();
  drainreuse::add(g_drainCalls, 1u);
  // NATIVE strand A (SR_CP_DRAIN_SKIP_EMPTY, default OFF): a drain with no command to consume does
  // nothing but open a new shared-memory validation epoch (and, through DeferGuard, re-stamp the open
  // set's first batch); the next drain that has commands opens its own epoch anyway.
  static const bool s_skipEmpty = cpsimd::gateLogged("SR_CP_DRAIN_SKIP_EMPTY", false);
  if (s_skipEmpty && !queue.front()) return;
  // iter-460 perf (SR_PM4_VSF_SHM): this drain's shared-memory requests queue
  // their copies, and every drain is a new validation epoch (a page the guest
  // rewrote between two indirect buffers is re-hashed, as the staged path
  // re-hashed every fetch).
  struct DeferGuard {
    D3D12Backend* be;
    size_t firstBatch = 0;
    bool consumed = false;  // the drain took a command (the early returns below leave it false)
    explicit DeferGuard(D3D12Backend* b) : be(b) {
#if SR_ORACLE
      // Packet-only replay drains must not move later restores in front of
      // a previously admitted draw. A real new set resets this below.
      if (be->replayOrderedRestores()) firstBatch = be->m_drawBatches.size();
      static const bool s_collect = [] {  // SR_PM4_SHM_COLLECT=0: validate per request
        const char* e = std::getenv("SR_PM4_SHM_COLLECT");
        return !e || e[0] != '0';
      }();
      be->pm4ShmSetDrainMode(/*collect=*/s_collect && be->m_shmReady, /*defer=*/true);
#else
      be->pm4ShmSetDrainMode(false, true);
#endif
    }
    ~DeferGuard() {
#if SR_ORACLE
      // mem.resources frozen-pass A/B (gfx/resource_ab.hpp EMPTY; Astra review1 P2-6): a drain that consumed nothing
      // outside an open set - the boot's drains before the first set, a drain between a present and the next set - is
      // inert: it collected nothing and owns no batch, so no arm is consulted (inside an open set the set's arm is).
      if (!consumed && memres::resourceAbDual() && (memres::resourceAbArm() < 0 || !be->m_pm4DrainOpen)) {
        be->pm4ShmCollectFlush();  // (empty: no request ran since the guard began)
        be->pm4ShmSetDrainMode(false, false);
        return;
      }
#endif
      if (memres::resourceDecides()) {
        be->cpResReqCensus();  // step 6 I4: [CP_RES_REQ], after this drain's draws
        // Native captures are already attached to their individual draw. Moving
        // later copies to the first draw would expose future ring-buffer bytes.
        be->pm4ShmSetDrainMode(false, false);
        return;
      }
#if SR_ORACLE
      // Validate everything this drain collected (parallel hashes), then the
      // drain's batches carry the queued copies: the render records them
      // before the drain's first batch (the running cursor).
      be->pm4ShmCollectFlush();
      const uint32_t pendEnd = uint32_t(be->m_shmPending.size());
      // iter-466 cp round 2 (SR_CP_PEND_FIRST): the render records pending copies with a running
      // cursor (pm4RenderFrame: [cursor, b.shmPendEnd) before batch b, never backwards), checked
      // at the top of every batch; every batch of this drain holds <= pendEnd. Raising the
      // drain's FIRST batch moves the cursor to pendEnd there - what raising all of them did -
      // without touching ~1 KB-strided cold batches (1.6% of the CP).
      static const bool s_pendFirst = cpsimd::gateLogged("SR_CP_PEND_FIRST", true);
      const size_t endB = s_pendFirst ? std::min(be->m_drawBatches.size(), firstBatch + 1u) : be->m_drawBatches.size();
      for (size_t i = firstBatch; i < endB; i++)
        be->m_drawBatches[i].shmPendEnd = std::max(be->m_drawBatches[i].shmPendEnd, pendEnd);
      be->pm4ShmSetDrainMode(false, false);
#endif
    }
  } deferGuard(this);
  if (m_shmReady) {
    std::lock_guard<std::recursive_mutex> lk(m_shmMu);
    m_shmEpoch++;
  }
  // iter-437e FRAME-COHERENT DRAIN: consume only COMPLETE frames — up to the
  // last VdSwap boundary marker (see onVdSwapFrame). The 39-presents/s loop
  // (437d) frequently caught the queue mid-frame; a drain holding a frame's
  // early draws without its movie quad rendered a near-empty RT = the
  // 1-present black flash mid-movie (boot_437d.mp4 t=18.5s). drainAll stays
  // for the pre-first-swap boot era and the trace replay (no markers there),
  // and as the half-full safety valve so a marker drought can never stall.
  const uint64_t bAvail = queue.boundariesPushed();
  // P2: the live PM4 path drains at draw time (after every IB), never waiting
  // for a complete frame - the guest's swap closes the set.
  const bool drainAll =
      pm4LiveEnabled() || (bAvail == 0) || (bAvail == m_framesDrained &&
                        queue.size() >= DrawCommandQueue::kCapacity / 2);
  if (!drainAll && bAvail == m_framesDrained)
    return;  // no complete frame queued — keep the previous frame's batches
  // iter-461: the commands are consumed in place (front / popFront), not
  // copied out of the ring - see DrawCommandQueue.
  const GuestDrawCommand* cmdp = queue.front();
  if (!cmdp) return;  // nothing new — keep the previous frame's batches
  deferGuard.consumed = true;
  sr::native::dcensus::drainBegin();  // NATIVE strand A drain census (observe-only)

  // P1 (SR_PM4_RTCACHE): incremental drains append to the open frame set
  // (pm4DrainNow after every packet); renderPm4Batches closes it.
  const bool pm4Append = pm4RtCacheEnabled() && m_pm4DrainOpen;
  if (!pm4Append) {
    // iter-439d: this drain's batch set continues the frame the LAST drain left
    // unfinished (it ended mid-frame at the half-full valve, not at a VdSwap
    // boundary). renderGuestGeometry keeps the RT for these. See hpp note.
    m_thisDrainContinuesFrame = m_nextDrainContinuesFrame;
    m_contBatchesRendered = false;

    m_drawBatches.clear();
    m_totalGeoVerts = 0;
  }
  m_pm4DrainOpen = pm4RtCacheEnabled();
  deferGuard.firstBatch = m_drawBatches.size();

#if SR_ORACLE
  float* mapped = nullptr;
  D3D12_RANGE rr{0, 0};
  if (FAILED(m_geoVertexBuffer->Map(0, &rr, reinterpret_cast<void**>(&mapped))) || !mapped)
    return;

  uint32_t vertexOffset = m_totalGeoVerts;
  static std::atomic<uint32_t> s_drawLogN{0};
  static std::atomic<uint32_t> s_skipLogN{0};

#endif  // SR_ORACLE
  // ── translated-shader drain state (iter-359c) ──────────────────────────────
  auto& shaderCache = getShaderCache();
  // render/boot-attach: no ensureLoaded here (it would wait for the translator stage on the drain).
  // The lookups below wait for the stage their answer needs (WP2); their hashes come from IM_LOADs
  // that already did.
#if SR_ORACLE
  static const bool s_xlatOn = [] {
    const char* e = std::getenv("SR_XLAT_SHADERS");
    return !e || e[0] != '0';
  }();
#else
  static constexpr bool s_xlatOn = true;
#endif
  // Alternate upload halves per drain: previous batches (still referenced by
  // in-flight frames) keep their half intact until the drain after next.
  // iter-461: thirds (see kXlatVBBytes), rotated at the first drain of a set.
  if (!pm4Append) {
    xlatHighWater(m_xlatThird, m_xlatVBUsed, m_xlatCBUsed, kXlatVBBytes, kXlatCBBytes);   // 7d diag (SR_XLAT_HWM)
    if (m_zcOn) {   // step 6 I10p: the finished set's use of its CB third (twin) and of the compact scratch
      const uint64_t cbBase = uint64_t(m_xlatCbThird) * (kXlatCBBytes / 3);
      const uint64_t shBase = uint64_t(m_xlatThird) * (m_xlatConstants.size() / 3);
      if (m_xlatCBUsed > cbBase && m_xlatCBUsed - cbBase > m_zcCbPeak.load(std::memory_order_relaxed))
        m_zcCbPeak.store(m_xlatCBUsed - cbBase, std::memory_order_relaxed);
      if (m_xlatShUsed > shBase && m_xlatShUsed - shBase > m_zcShPeak.load(std::memory_order_relaxed))
        m_zcShPeak.store(m_xlatShUsed - shBase, std::memory_order_relaxed);
    }
    m_xlatThird = (m_xlatThird + 1u) % 3u;
    // iter-461: the third's last frame has completed on the GPU (the render
    // thread's fence; always already true on the synchronous path), the shm
    // arena rotates with it, and the set's first epoch is recorded.
    pm4WaitThirdFree(m_xlatThird);
#if SR_ORACLE
    if (replayOrderedRestores() && !pm4ShmInit())
      throw std::runtime_error("replay restore cannot initialize storage");
#endif
    pm4ShmBeginSet(m_xlatThird);
    // step 6 I10p: the CB third the set writes (its views: owned by that third's fence) - m_xlatThird; on the view
    // path in check mode its reuse is asserted / rechecked / poisoned first (zcBeginThird), and the TEST-ONLY mutant
    // takes the previous set's CB third every 8th set (one set early).
    zcRotateCbThird(true);
    m_xlatVBUsed = uint64_t(m_xlatThird) * (kXlatVBBytes / 3);
    m_xlatCBUsed = uint64_t(m_xlatCbThird) * (kXlatCBBytes / 3);
    m_xlatShUsed = uint64_t(m_xlatThird) * (m_xlatConstants.size() / 3);
  }
  const uint64_t vbHalfEnd = uint64_t(m_xlatThird + 1) * (kXlatVBBytes / 3);
#if SR_ORACLE
  // The third now belongs to this set. Captures precede this drain's draws,
  // while earlier batches retain their original pending-copy cursors.
  if (replayOrderedRestores()) replayStageRestores();
#endif
  const uint64_t cbHalfEnd = uint64_t(m_xlatCbThird + 1) * (kXlatCBBytes / 3);
  const uint64_t shHalfEnd = uint64_t(m_xlatThird + 1) * (m_xlatConstants.size() / 3);   // I10p: the compact scratch
  static std::atomic<uint32_t> s_xlatLogN{0};

  // Per-drain accounting: which pipeline does each draw take, and why does a
  // draw fall off the translated path? (xlatWhy indices: 0=off 1=noVs 2=noPs
  // 3=noLayout 4=noPso 5=prim 6=vbFull 7=fetchInvalid 8=cbFull 9=zeroVerts)
  uint32_t nXlat = 0, nFixed = 0, xlatWhy[10] = {};
  uint32_t modeHist[8] = {};
  uint32_t primDrop[32] = {};  // which prim types fall off the xlat path
  uint32_t nDedup = 0;
  uint32_t nHdrDraws = 0, nHdrResolves = 0;  // iter-359n: HDR-routing census
  // Per-frame-set decoded-VB reuse: dedupe key -> {VB byte offset, vertex
  // count}. Entries always point into the current upload half (the cursor
  // resets with the set; P1 incremental drains keep both across packets).
  auto& vbDedup = m_xlatVbDedup;
  if (!pm4Append) {
    vbDedup.clear();
    m_cdVsOff = m_cdPsOff = ~0ull;  // iter-466 cp round 2: SR_CP_CONST_DEDUP lives in the third too
    m_cvUpVsOff = m_cvUpPsOff = ~0ull;  // and SR_CP_CONST_TRACK's upload reuse
    m_vsfCbLastOff = ~0ull;  // iter-466 cp lane: the CB / IB reuse lives in the same third
#if SR_ORACLE
    m_ibConv.clear();
    m_dropN = 0;   // step 6 drop fix: the census's dropped offsets live in the third too
    ++m_cpSetSerial;
#endif
  }

#if !SR_ORACLE
  // This is the current draw's rejection, not a prior draw's capped census.
  int nativeXlatReject = -1;
  int nativeXlatState = -1;  // render/shader-coverage: the translator's DropState for a missing VS / PS
#endif
  auto tryXlatDraw = [&](const GuestDrawCommand& c) -> bool {
#if !SR_ORACLE
    nativeXlatReject = -1;
    nativeXlatState = -1;
#else
    const memres::FetchAdmissionScope fetchAdmission(c.drawOrdinal);
#endif
    livetrace::setDrawContext(c.drawOrdinal);  // iter-460 live trace
    struct LtCtxReset { ~LtCtxReset() { livetrace::setDrawContext(0); } } ltCtxReset;
    // iter-424b: a rejected YUV draw was invisible — every gate below returns
    // before the disposition log. Name the gate that kills it.
    const bool yuvRej = (c.psUcodeHash == 0x20A5E5204FD23BE2ull);
    auto yrej = [&](int gate) {
#if !SR_ORACLE
      nativeXlatReject = gate;
#endif
      if (yuvRej)
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[YUV_REJ] gate=%d (1=noVs 2=noPs 3=noLayout 4=noPso 5=prim 6=vbFull 7=fetchInv 8=cbFull 9=zeroV)", gate);
      // iter-440f: name every UNIQUE shader pair that falls off the xlat path
      // (the red-era hunt: which composite shader is missing/rejected). First
      // 64 unique pairs only; dedupe by vs^ps hash.
      static std::mutex mu;
      static std::unordered_set<uint64_t> seen;
      const uint64_t key = c.vsUcodeHash ^ (c.psUcodeHash * 0x9E3779B97F4A7C15ull) ^ uint64_t(gate);
      std::lock_guard<std::mutex> lk(mu);
      if (seen.size() < 64 && seen.insert(key).second)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[XLAT_REJECT] gate=%d vs=%016llX ps=%016llX prim=%u mode=%u colInfo=0x%08X",
            gate, (unsigned long long)c.vsUcodeHash, (unsigned long long)c.psUcodeHash,
            c.primType, c.modeControl & 7u, c.colorInfo);
    };
    if (!s_xlatOn || !xlatReady() || !m_xlatVBPtr || !m_xlatCBPtr) { yrej(0); xlatWhy[0]++; return false; }

    // iter-359g pass wiring (recon wf_3fc645cd census): RB_MODECONTROL
    // edram_mode routes the draw. 6 = the resolve rectlist — the console
    // copies EDRAM to guest memory here; we schedule an RT readback instead
    // of drawing it. 5 = depth-only (no color writes). 0 = inert.
    const uint32_t edramMode = c.modeControl & 7u;
    modeHist[edramMode]++;
    if (edramMode == 6u) sr::native::dcensus::noteResolveDraw();
    if (edramMode == 6u && pm4RtCacheEnabled()) {
      // P1: the EDRAM path executes the resolve from its register snapshot
      // (edram::computeResolve) at render time - see pm4ExecResolve.
      DrawBatch rb{};
      rb.resolve = true;
      rb.pm4 = true;
      rb.pm4Regs = pm4RegsFromDraw(c);
      rb.resolveDestPhys = c.copyDestBase & 0x1FFFFFFFu;
      rb.resolveDestInfo = c.copyDestInfo;
      rb.drawOrdinal = c.drawOrdinal;
      if (memres::resourceMode()) {
        rb.resourceGpuGeneration = c.resourceGpuGeneration;
        if (memres::resourceDecides()) resourcePrepareResolve(rb);
#if SR_ORACLE
        else resourceAbNoteResolve(rb);  // the frozen-pass A/B's model arm: the registry's GPU note, no preservation
#endif
      }
#if SR_ORACLE
      if (livetrace::recording()) pm4TraceNoteResolve(rb.pm4Regs);  // iter-463 D8
#endif
      m_drawBatches.push_back(rb);
      return true;
    }
    if (edramMode == 6u) {
      // RB_COPY_DEST_INFO carries the dest endian/format/swap bits — the
      // byte-order contract for what the console writes to guest memory.
      // iter-408b: UNCAPPED tally alongside the capped detail line. The capped
      // line alone read as "all resolves happen in one 0.18s burst at boot and
      // then stop for three minutes" — which was the cap being reached, not the
      // resolves stopping. Any capped log used to argue something STOPPED is
      // making a claim its own sampling cannot support.
      static std::atomic<uint64_t> s_resQueued{0}, s_resToFront{0};
      const uint64_t rq = s_resQueued.fetch_add(1, std::memory_order_relaxed) + 1;
      if ((c.copyDestBase & 0x1FFFFFFFu) == (m_guestFrontPhys & 0x1FFFFFFFu) && m_guestFrontPhys)
        s_resToFront.fetch_add(1, std::memory_order_relaxed);
      if (rq % 200 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[RESOLVE_STATS] queued=%llu toFrontBuffer=%llu (front=0x%08X)",
                    (unsigned long long)rq,
                    (unsigned long long)s_resToFront.load(std::memory_order_relaxed),
                    m_guestFrontPhys);
      static std::atomic<uint32_t> s_cdiLog{0};
      if (s_cdiLog.fetch_add(1, std::memory_order_relaxed) < 32)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[XLAT_RESOLVE] queue dest=0x%08X destInfo=0x%08X (fmt=%u endian=%u swap=%u) copyControl=0x%08X",
                    c.copyDestBase, c.copyDestInfo, (c.copyDestInfo >> 7) & 0x3Fu,
                    c.copyDestInfo & 7u, (c.copyDestInfo >> 24) & 1u, c.copyControl);
      DrawBatch rb{};
      rb.resolve = true;
      rb.resolveDestPhys = c.copyDestBase & 0x1FFFFFFFu;
      rb.resolveDestInfo = c.copyDestInfo;
      // Source surface: RB_COLOR_INFO float format => the resolve reads the HDR
      // scene RT (snapshot as an HDR texture); else the LDR RT (iter-359m).
      {
        const uint32_t cf = (c.colorInfo >> 16) & 0xFu;
        rb.colorInfo = c.colorInfo;
        rb.hdrTarget = rttEnabled() &&
            (cf == 3u || cf == 6u || cf == 7u || cf == 12u || cf == 14u || cf == 15u);
      }
      if (rb.hdrTarget) nHdrResolves++;
      rb.resolvePitchTexels = c.copyDestPitch & 0x3FFFu;
      // Surface height: the trace's helper passes are square (pitch 640 ->
      // 640 rows); full-pitch surfaces are the 1280x720 frame.
      // iter-437b: the HLE Resolve hook carries the TRUE height from the dest
      // surface fetch — the heuristic scribbled past non-square dests
      // (640x360 got 640 rows).
      rb.resolveRows = c.copyDestHeight
          ? c.copyDestHeight
          : ((rb.resolvePitchTexels >= 1280u) ? 720u : rb.resolvePitchTexels);
      rb.resolveSrcX = c.copySrcX; rb.resolveSrcY = c.copySrcY;
      rb.resolveSrcW = c.copySrcW; rb.resolveSrcH = c.copySrcH;
      m_drawBatches.push_back(rb);
      return true;
    }
    if (edramMode == 0u) return true;  // ignore-mode draw: consume, render nothing
    // iter-359i: mode 5 (kDepth) draws are NOT color-mask-0 — the game's
    // EDRAM-clear idiom is a mode-5 rect with mask=0xFFFF priming BOTH depth
    // (z, func ALWAYS) and color (the MIN-blend accumulation floor). Honor
    // RB_COLOR_MASK as written; draws that want no color set mask=0 themselves.
    const bool depthOnly = false;

    // iter-359m: does this draw target an HDR (float) EDRAM color surface?
    // RB_COLOR_INFO format bits [19:16] (Xenia registers.h): 3=2_10_10_10_FLOAT,
    // 6=k_16_16_FLOAT, 7=k_16_16_16_16_FLOAT, 12=..._FLOAT_AS_16_16_16_16,
    // 14=k_32_FLOAT — all render into the R16F scene surface. 8888/gamma/
    // 2_10_10_10-unorm stay on the LDR RT. Gated with the surface model.
    const uint32_t colorFmt = (c.colorInfo >> 16) & 0xFu;
    const bool hdrTarget = rttEnabled() &&
        (colorFmt == 3u || colorFmt == 6u || colorFmt == 7u ||
         colorFmt == 12u || colorFmt == 14u || colorFmt == 15u);

    // iter-463 D8 (SR_PM4_TRACE_FULL): a traced frame records this draw's IB,
    // vertex fetch ranges and full texture backing BEFORE the gates below - a
    // draw they reject reads none of it, and Xenia played our traces without
    // those bytes (the arena's missing-VS draws). No-op when not recording.
#if SR_ORACLE
    if (livetrace::recording()) pm4TraceDrawBacking(c);
#endif
    const CachedShader* cvs = shaderCache.findVs(c.vsUcodeHash);
    const CachedShader* cps = shaderCache.findPs(c.psUcodeHash);
    // iter-463: a VS miss DROPS the draw (no placeholder); [SHADER_MISS] names it
    // and why it cannot reuse a cached twin (SR_SHADER_MISS_REPORT, default ON).
    // render/shader-coverage (the miss policy): the IM_LOAD that missed waited for its translation, so a
    // shader still missing here has a final state (no template, refused, failed, the budget passed) - dev
    // drops the draw ([SHADER_DROP]), the release stops with that state named.
    if (!cvs) {
      yrej(1); xlatWhy[1]++;
      const int st = shaderCache.noteVsMissDraw(c.vsUcodeHash, c.psUcodeHash);
#if !SR_ORACLE
      nativeXlatState = st;
#else
      (void)st;
#endif
      return false;
    }
    if (!cps) {
      yrej(2); xlatWhy[2]++;
      const int st = shaderCache.notePsMissDraw(c.psUcodeHash);
#if !SR_ORACLE
      nativeXlatState = st;
#else
      (void)st;
#endif
      return false;
    }
    if (cvs->layout.empty()) { yrej(3); xlatWhy[3]++; return false; }
    // iter-370: the point-sprite variant is a distinct pipeline (GS + POINT
    // topology), so the prim type has to be classified BEFORE the PSO lookup.
    const bool pointList = (c.primType == 1u);
    // P1: the EDRAM path picks targets, formats and depth state from the
    // register snapshot; a draw that binds nothing is consumed (not drawn).
    const bool pm4 = pm4RtCacheEnabled();
    // P1 in-shader vertex fetch (Xenia's model): the VS reads guest memory at
    // the index IT computes - self-indexed (slot-94 index streams) and
    // instanced world draws cannot be pre-decoded per vertex on the CPU. Rect
    // lists (4th vertex derived from the other three) and clip-disabled pixel
    // draws (their EDRAM claim is bounded by the CPU-decoded Y) stay on the
    // pre-decode path.
    const bool clipOffXy = ((c.clipCntl >> 16) & 1u) && ((c.vteCntl >> 8) & 1u);
    const bool vsf = pm4 && pm4VsfEnabled() && !cvs->dxilFetch.empty() && c.primType != 8u &&
                     !clipOffXy;
    const bool vsfStripCut =
        vsf && c.primType == 6u && c.idxBasePhys && ((c.suScModeCntl >> 21) & 1u);
    edram::Regs pmRegs;
    edram::DrawTargets pmTargets;
    ID3D12PipelineState* pso = nullptr;
    // NATIVE strand A drain census (SR_NATIVE_DRAIN_CENSUS, observe-only): are this draw's pipeline /
    // target / viewport inputs the previous drained draw's, and what do prepare + viewport cost?
    bool dcSame = false;
    // p2.zpd: a draw inside an open query is prepared afresh (its probe pipelines are pm4PrepareDraw's, not cached).
    const bool reuseCand = pm4 && c.primType != 8u && drainReuseOn() && !m_zpdDrainQuery;
    uint32_t rk[48];
    uint32_t rn = 0;
    if ((sr::native::dcensus::on() || reuseCand) && pm4) {
      uint32_t* k = rk;
      uint32_t n = 0;
      auto put = [&](uint32_t v) { k[n++] = v; };
      auto putf = [&](float f) { uint32_t v; std::memcpy(&v, &f, 4); put(v); };
      put(uint32_t(c.vsUcodeHash)); put(uint32_t(c.vsUcodeHash >> 32));
      put(uint32_t(c.psUcodeHash)); put(uint32_t(c.psUcodeHash >> 32));
      put(c.primType | (pointList ? 0x100u : 0u) | (vsf ? 0x200u : 0u) | (vsfStripCut ? 0x400u : 0u));
      put(c.surfaceInfo); put(c.colorInfo); put(c.colorInfo123[0]); put(c.colorInfo123[1]); put(c.colorInfo123[2]);
      put(c.depthInfo); put(c.colorMask); put(c.depthControl); put(c.stencilRefMask); put(c.stencilRefMaskBF);
      put(c.modeControl); put(c.windowOffset); put(c.windowScissorTL); put(c.windowScissorBR);
      put(c.screenScissorTL); put(c.screenScissorBR); put(c.suScModeCntl); put(c.clipCntl); put(c.vteCntl);
      put(c.vtxCntl); for (int i = 0; i < 3; i++) { putf(c.vpScale[i]); putf(c.vpOffset[i]); }
      put(c.blendControl); put(c.blendControl123[0]); put(c.blendControl123[1]); put(c.blendControl123[2]);
      for (int i = 0; i < 4; i++) put(c.polyOffset[i]);
      // step 4 lane C: RB_COLORCONTROL picks the alpha-test pipeline (gfx/alpha_test.hpp); off, the key is unchanged.
      if (alphatest::enabled()) put(alphatest::word(c.colorControl));
      // step 6 R6 fix round 2 (Astra r2 P2a): the non-command inputs pm4PrepareDraw / pm4ViewportScissor read - the
      // resolution scale (slope bias, viewport / scissor scaling), the RT-height scale (RtKey::height in
      // computeDrawTargets) and the two coverage gates (A/B-togglable at runtime).
      {
        const resscale::Scale& rs = resscale::get();
        put(rs.x | (rs.y << 8));
      }
      put(edram::g_rtHeightScaleY);
      put((pm4NbClearElideOnForKey() ? 1u : 0u) | (nbCoverFillOn() ? 2u : 0u));
      rn = n;
      if (sr::native::dcensus::on()) dcSame = sr::native::dcensus::keySame(k, n);
    }
    bool reused = false;
#if SR_ORACLE
    bool chkFresh = false;  // step 6 R6 CHECK: t_chkRegs / t_chkTargets hold this draw's fresh prepare
#endif
    if (pm4) {
      const auto tp0 = g_pm4DiagTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      const uint64_t dc0 = sr::native::dcensus::on() ? __rdtsc() : 0;
      bool prepared = false;
      if (reuseCand && drainreuse::adjacent(t_reuse.seq, t_drainSeq.v) && t_reuse.n == rn &&
          std::memcmp(t_reuse.k, rk, 4u * rn) == 0) {
        reused = true;
        t_reuse.seq = t_drainSeq.v;  // the entry stays this command's: the next identical draw may reuse it too
        prepared = t_reuse.prepared;
        pmRegs = t_reuse.regs;
        drainreuse::refreshNonKey(pmRegs, c);  // fix round 2 (Astra r2 P2a): the unkeyed copy / clear / resolve fields
        pmTargets = t_reuse.t;
        pso = t_reuse.pso;
        m_pm4MarkPso = t_reuse.markPso;
        g_reuseHits.fetch_add(1, std::memory_order_relaxed);
#if SR_ORACLE
        if (drainReuseCheck()) {  // the full computation, compared - every output (Astra r1 P2: the Regs too)
          edram::Regs& r2 = t_chkRegs;
          edram::DrawTargets& t2 = t_chkTargets;
          r2 = edram::Regs{};          // as fresh as the miss path's per-draw pmRegs / pmTargets
          t2 = edram::DrawTargets{};
          ID3D12PipelineState* p2 = nullptr;
          const bool ok2 = pm4PrepareDraw(c, pointList, vsf, vsfStripCut, r2, t2, p2);
          chkFresh = true;
          g_reuseChecked.fetch_add(1, std::memory_order_relaxed);
          const bool regsBad = ok2 && !edram::sameRegs(r2, pmRegs);
          if (ok2 != prepared ||
              (ok2 && (p2 != pso || m_pm4MarkPso != t_reuse.markPso || !edram::sameTargets(t2, pmTargets) || regsBad))) {
            const uint64_t bad = g_reuseBad.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bad <= 16)
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                          "[NATIVE_DRAIN_REUSE] BAD prepare draw#%u prim=%u prepared %d/%d pso %p/%p regs %s",
                          c.drawOrdinal, c.primType, int(prepared), int(ok2), (void*)pso, (void*)p2,
                          regsBad ? "DIFFER" : "same");
          }
          m_pm4MarkPso = t_reuse.markPso;
        }
#endif
      } else {
        prepared = pm4PrepareDraw(c, pointList, vsf, vsfStripCut, pmRegs, pmTargets, pso);
        if (reuseCand) {
          g_reuseMiss.fetch_add(1, std::memory_order_relaxed);
          if (drainreuse::cacheable(prepared, pso != nullptr, m_pm4MarkWanted, m_pm4MarkPso != nullptr)) {
            t_reuse.seq = t_drainSeq.v;
            t_reuse.n = rn;
            std::memcpy(t_reuse.k, rk, 4u * rn);
            t_reuse.prepared = prepared;
            t_reuse.regs = pmRegs;
            t_reuse.t = pmTargets;
            t_reuse.pso = pso;
            t_reuse.markPso = m_pm4MarkPso;
            t_reuse.vpValid = false;
          } else {
            t_reuse.n = 0;  // fix rounds 3 + 4: a failed or incomplete prepare empties the entry - never reused (rn > 0)
            t_reuse.vpValid = false;
          }
        }
      }
      if (reuseCand) reuseReport();
      if (sr::native::dcensus::on()) sr::native::dcensus::notePrep(dcSame, __rdtsc() - dc0);
      if (g_pm4DiagTiming)
        drainreuse::add(g_drainMsPrep,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp0).count());
      drainreuse::add(g_drainN, 1u);
      if (!prepared) return true;
    } else {
      pso = getXlatPso(c.vsUcodeHash, c.psUcodeHash,
                       c.depthControl, c.blendControl, c.colorMask,
                       depthOnly, hdrTarget, pointList,
                       /*poolDepth=*/false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                       /*noDsv=*/false, /*nativeTextures=*/true);
    }
    if (!pso) { yrej(4); xlatWhy[4]++; return false; }

    D3D_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool rectList = false;
    bool quadList = false;
    // Strips and fans are expanded to triangle lists when they need it: fans
    // always (no host fan topology), indexed strips when primitive reset is on
    // (PA_SU_SC_MODE_CNTL.multi_prim_ib_ena; primitive_processor.cc:616) - an
    // index equal to VGT_MULTI_PRIM_IB_RESET_INDX restarts the strip. Decoded
    // as a vertex instead, the reset index fetched an out-of-range (0,0,0,1)
    // and pulled every city mesh's strips into spikes at the origin.
    const bool stripReset = c.primType == 6 && c.idxBasePhys && ((c.suScModeCntl >> 21) & 1u);
    const bool fanList = c.primType == 5;
    const bool listConvert = stripReset || fanList;
    switch (c.primType) {
      case 1: topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;  // kPointList
      case 4: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
      case 5: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;  // kTriangleFan, expanded
      case 6:
        topo = stripReset ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case 8: {
        rectList = true;
        // iter-362 texture-frontier: the menu SHAPE quads are rectlists (prim=8),
        // not the prim=13 post quads QUAD_TEX covers — so their texture state was
        // never logged. Dump each bound fetch's type/base/fmt/DIMS to tell a
        // genuinely solid-fill shape (no fetch) from a texture we mis-decode or
        // don't support. Gated (resolveDumpEnabled) — never tax the default path.
        if (resolveDumpEnabled()) {
          static std::atomic<uint32_t> s_rectTexLog{0};
          if (s_rectTexLog.fetch_add(1, std::memory_order_relaxed) < 40) {
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RECT_TEX] verts=%u vte=0x%X vpS=(%.1f,%.1f) colorInfo=0x%08X",
                c.vertexCount, c.vteCntl, c.vpScale[0], c.vpScale[1], c.colorInfo);
            for (uint32_t t = 0; t < 16; t++) {
              const uint32_t* tf = &c.fetchRaw[t * 6];
              if ((tf[0] & 3u) != 0u) {
                const uint32_t base = ((tf[1] >> 12) & 0xFFFFFu) * 4096u;
                const uint32_t w = (tf[2] & 0x1FFFu) + 1u;
                const uint32_t h = ((tf[2] >> 13) & 0x1FFFu) + 1u;
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[RECT_TEX]   t%u: type=%u base=0x%08X fmt=%u %ux%u tiled=%u",
                    t, tf[0] & 3u, base, tf[1] & 0x3Fu, w, h, (tf[0] >> 31) & 1u);
              }
            }
          }
        }
        break;
      }
      case 13: {  // kQuadList: 4 verts -> 2 tris
        quadList = true;
        // iter-359j/n diagnostic: the tonemap/post quads live here. Name their
        // texture sources + this draw's viewport (coverage — the left/right
        // split hunt) + whether each source has a resolve texture bound.
        static std::atomic<uint32_t> s_quadLog{0};
        // iter-397q: ALSO log whenever the bound PS hash changes — the capped
        // count exhausted in the first movie seconds and went stale as
        // evidence exactly when the question was "did the PS ever change".
        // iter-460: a NEVER-SEEN PS, not merely a different one - the live PM4
        // path alternates quad shaders every draw and logged ~1000 lines/s.
        static std::mutex s_quadPsMu;
        static std::unordered_set<uint64_t> s_quadPsSeen;
        bool psChanged;
        {
          std::lock_guard<std::mutex> lk(s_quadPsMu);
          psChanged = s_quadPsSeen.insert(c.psUcodeHash).second;
        }
        if (s_quadLog.fetch_add(1, std::memory_order_relaxed) < 40 || psChanged) {
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[QUAD_TEX] quad verts=%u vte=0x%X vpS=(%.0f,%.0f) vpO=(%.0f,%.0f) color=0x%08X vs=%016llX ps=%016llX",
              c.vertexCount, c.vteCntl, c.vpScale[0], c.vpScale[1],
              c.vpOffset[0], c.vpOffset[1], c.colorInfo,
              (unsigned long long)c.vsUcodeHash, (unsigned long long)c.psUcodeHash);
          for (uint32_t t = 0; t < 16; t++) {
            const uint32_t* tf = &c.fetchRaw[t * 6];
            if ((tf[0] & 3u) != 0u) {
              const uint32_t base = ((tf[1] >> 12) & 0xFFFFFu) * 4096u;
              SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[QUAD_TEX]   t%u: base=0x%08X fmt=%u", t, base, tf[1] & 0x3Fu);
            }
          }
        }
        break;
      }
      default:
#if !SR_ORACLE
        nativeXlatReject = 5;
#endif
        xlatWhy[5]++;
        if (c.primType < 32) primDrop[c.primType]++;
        return false;
    }

    // iter-427b: HLE layout override. Container-hashed VSes carry PLACEHOLDER
    // vfetch instructions (the console runtime patches slot/offset/stride from
    // the vertex declaration at draw time), so when the D3D9-HLE hook supplied
    // an explicit element list, it — not the sidecar — describes the stream.
    // Element ORDER matches the sidecar/input-signature order by construction.
    sr::gfx::VfetchElement hleLay[8];
    const sr::gfx::VfetchElement* lay = cvs->layout.data();
    uint32_t nAttrs = uint32_t(cvs->layout.size());
    if (c.hleLayoutCount > 0 && c.hleLayoutCount <= 8) {
      for (uint32_t i = 0; i < c.hleLayoutCount; i++) {
        hleLay[i] = sr::gfx::VfetchElement{};
        hleLay[i].slot = c.hleLayout[i].slot;
        hleLay[i].offsetDwords = int32_t(c.hleLayout[i].offsetBytes / 4);
        hleLay[i].strideDwords = c.hleLayout[i].strideBytes / 4;
        hleLay[i].format = c.hleLayout[i].format;
        hleLay[i].usage = c.hleLayout[i].usage;
        hleLay[i].usageIndex = c.hleLayout[i].usageIndex;
      }
      lay = hleLay;
      nAttrs = c.hleLayoutCount;
    } else if (c.hleVdeclCount > 0 && c.hleVdeclCount <= 12) {
      // iter-435: RAW vdecl elements from the D3D9-HLE hook, paired
      // POSITIONALLY with the translated VS's input signature (vdecl author
      // order == input register order — the rule the movie hack encoded by
      // hand; its 2-element pos/uv mapping is exactly "first 2 vdecl elements
      // in order"). Stream s binds fetch slot 95-s (movie: stream0 -> slot95,
      // verified). If the vdecl has FEWER elements than the VS consumes, the
      // pairing is unknowable — skip rather than feed a corrupt layout.
      const uint32_t want = uint32_t(cvs->layout.size());
      if (want > 8 || c.hleVdeclCount < want) {
        static std::atomic<uint32_t> s_vdMism{0};
        const uint32_t vm = s_vdMism.fetch_add(1, std::memory_order_relaxed) + 1;
        if (vm <= 8 || vm % 2000 == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HLE_VDMAP] #%u SKIP vs=%016llX sigAttrs=%u vdeclN=%u",
              vm, (unsigned long long)c.vsUcodeHash, want, c.hleVdeclCount);
        yrej(3); xlatWhy[3]++; return false;
      }
      for (uint32_t i = 0; i < want; i++) {
        const auto& e = c.hleVdecl[i];
        hleLay[i] = sr::gfx::VfetchElement{};
        hleLay[i].slot = 95u - (e.stream & 3u);
        hleLay[i].offsetDwords = int32_t(e.offsetBytes / 4);
        hleLay[i].strideDwords = c.hleStreamStride[e.stream & 3u] / 4;
        hleLay[i].format = e.declType & 0x3Fu;
        hleLay[i].usage = e.usage;
        hleLay[i].usageIndex = e.usageIndex;
      }
      lay = hleLay;
      nAttrs = want;
      static std::atomic<uint32_t> s_vdOk{0};
      const uint32_t vo = s_vdOk.fetch_add(1, std::memory_order_relaxed) + 1;
      if (vo <= 12 || vo % 5000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[HLE_VDMAP] #%u OK vs=%016llX attrs=%u e0={s%u@%u f%u st%u} "
            "e1={s%u@%u f%u} idx=%08X bv=%u",
            vo, (unsigned long long)c.vsUcodeHash, want,
            hleLay[0].slot, hleLay[0].offsetDwords * 4, hleLay[0].format,
            hleLay[0].strideDwords * 4,
            want > 1 ? hleLay[1].slot : 0,
            want > 1 ? uint32_t(hleLay[1].offsetDwords * 4) : 0,
            want > 1 ? hleLay[1].format : 0, c.idxBasePhys, c.hleBaseVertex);
    }
    const bool isYuvHle =
        (c.psUcodeHash == 0x20A5E5204FD23BE2ull) && c.hleLayoutCount > 0;
    if (nAttrs == 0 || nAttrs > 16) { yrej(3); xlatWhy[3]++; return false; }
    const uint32_t strideOut = nAttrs * 16;
    const uint32_t outCount = rectList ? (c.vertexCount / 3u) * 6u
                            : quadList ? (c.vertexCount / 4u) * 6u
                            : listConvert ? (c.vertexCount >= 3u ? (c.vertexCount - 2u) * 3u : 0u)
                                       : c.vertexCount;
    if (!outCount) { yrej(9); xlatWhy[9]++; return false; }
    Pm4VsfOut vsfOut{};
#if SR_ORACLE
    std::vector<std::shared_ptr<const memres::ConsumerCheck>> consumerChecks;
#endif
    const uint32_t shmPendBegin = uint32_t(m_shmPending.size());
    if (vsf) {
#if SR_ORACLE
      // Only shared-memory vertex requests can defer a changed-page check.
      // Textures and resolve preservation retain the strict whole-page check.
      memres::ConsumerScope consumerScope(memres::resourceConsumerEnabled());
#endif
#if SR_ORACLE
      const uint64_t fetchDedups0 = m_vsfCbDedup;   // step 6 drop fix: a deduped fetch block is a reuse
#endif
      if (!pm4StageVsf(c, *cvs, vsfStripCut, vbHalfEnd, cbHalfEnd, vsfOut)) {
        yrej(6); xlatWhy[6]++;
#if SR_ORACLE
        return true;  // consumed: never paint a fetch-variant draw via the fallback
#else
        nativeXlatReject = 12;  // failed staging is not a completed native draw
        return false;
#endif
      }
#if SR_ORACLE
      if (m_vsfCbDedup != fetchDedups0)
        xlatReuseBelowCursor(vsfOut.fetchCbOff, 768, m_xlatCBUsed, "fetch", c.drawOrdinal);
#endif
      topo = vsfOut.topo;
#if SR_ORACLE
      consumerChecks = consumerScope.take();
#endif
    }

    // iter-359h VB dedupe: the frame decodes the same meshes twice (815-draw
    // depth prepass + the color pass over the same geometry) — reuse this
    // drain's already-decoded vertices when layout + fetch constants + index
    // stream are identical. Root fix for the vbFull=312 census class.
    struct DedupKey {
      uint64_t vs;
      uint32_t cnt, rect, ib, fmt32, endian, pad;
      uint32_t basev;  // iter-435: baseVertex shifts every fetch — must key
      uint32_t fetch[16 * 3];
    } dk;
    // iter-466 cp lane (SR_CP_VSF_NOWIN): an in-shader-fetch draw never looks the key up (the
    // dedupe below is for the CPU pre-decode), so its per-attribute window lookups and 128-byte
    // content samples are skipped (gpuPhysToHost here was ~3.3% of the CP, b4_g).
    const bool needKey = !(vsf && cpsimd::vsfNoWin());
    if (needKey) {
    std::memset(&dk, 0, sizeof(dk));
    dk.vs = c.vsUcodeHash;
    dk.cnt = c.vertexCount;
    dk.rect = rectList ? 1u : quadList ? 2u : stripReset ? 3u | (c.primResetIndex << 8) : fanList ? 4u : 0u;
    dk.ib = c.idxBasePhys;
    dk.fmt32 = c.idxFmt32 ? 1u : 0u;
    dk.endian = c.idxEndian;
    dk.basev = c.hleBaseVertex;
    for (uint32_t i = 0; i < nAttrs; i++) {
      dk.fetch[i * 3 + 0] = c.fetchRaw[lay[i].slot * 2];
      dk.fetch[i * 3 + 1] = c.fetchRaw[lay[i].slot * 2 + 1];
      // iter-435: stride keyed too — vdecl-derived strides vary per layout.
      dk.fetch[i * 3 + 2] =
          uint32_t(lay[i].offsetDwords) | (lay[i].strideDwords << 16);
      // iter-427f: make the key CONTENT-sensitive. The movie quad's VB sits at
      // the same address with the same size every frame, so an address-only
      // key made every later frame reuse the FIRST decode — captured before
      // the VB was ever written, i.e. four (0,0,0,1) vertices, forever. Mixing
      // in a bounded byte sample keeps the same-frame mesh-reuse benefit while
      // letting mutated buffers re-decode.
      {
        const uint32_t d0s = c.fetchRaw[lay[i].slot * 2];
        if ((d0s & 3u) == 3u) {
          const uint32_t phys = ((d0s >> 2) & 0x3FFFFFFF) * 4;
          const uint32_t szB = ((c.fetchRaw[lay[i].slot * 2 + 1] >> 2) & 0x00FFFFFF) * 4;
          const uint32_t n = szB < 128u ? szB : 128u;
          // Sample BOTH physical windows — the movie VB's content is only
          // visible through 0xA under the flat model (the decode's alias probe
          // reads it there), so an 0x8-only sample would never see mutation.
          const uint8_t* sp = gpuPhysToHost(phys);
          if (sp) dk.pad ^= uint32_t(ShaderCache::hashUcode(sp, n));
          const uint8_t* ap = sr::kernel::guestVAToHost(0xA0000000u + phys);
          if (ap && sr::kernel::isGuestVAInRange(0xA0000000u + phys + n))
            dk.pad ^= uint32_t(ShaderCache::hashUcode(ap, n)) * 0x9E3779B9u;
        }
      }
    }
    }  // needKey
    const uint64_t dkey = needKey ? ShaderCache::hashUcode(&dk, sizeof(dk)) : 0ull;
    uint64_t vbOffsetBytes = 0;
    uint32_t written = 0;
    const auto dit = vsf ? vbDedup.end() : vbDedup.find(dkey);
    if (vsf) {
      written = vsfOut.indexCount;  // the shader fetches; nothing to pre-decode
    } else if (dit != vbDedup.end()) {
      vbOffsetBytes = dit->second.first;
      written = dit->second.second;
      nDedup++;
#if SR_ORACLE
      xlatReuseBelowCursor(vbOffsetBytes, uint64_t(written) * strideOut, m_xlatVBUsed, "vb", c.drawOrdinal);
#endif
    } else {
    const auto tDec0 = g_pm4DiagTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct DecTimer {
      std::chrono::steady_clock::time_point t0;
      ~DecTimer() {
        if (g_pm4DiagTiming)
          drainreuse::add(g_drainMsDecode,
                          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      }
    } decTimer{tDec0};
    if (m_xlatVBUsed + uint64_t(outCount) * strideOut > vbHalfEnd) { yrej(6); xlatWhy[6]++; return false; }
    if (!xlatCommit(m_xlatVBUsed + uint64_t(outCount) * strideOut, 0)) { yrej(6); xlatWhy[6]++; return false; }

    // Per-attribute guest sources from the DRAW-TIME fetch-constant snapshot.
    struct Src { const uint8_t* base; uint32_t strideBytes; uint32_t endian; uint32_t sizeBytes; };
    Src srcs[16];
    for (uint32_t i = 0; i < nAttrs; i++) {
      const auto& e = lay[i];
#if SR_ORACLE
      if (e.slot >= 96) return false;
#else
      if (e.slot >= 96) { nativeXlatReject = 10; return false; }
#endif
      const uint32_t d0 = c.fetchRaw[e.slot * 2];
      const uint32_t d1 = c.fetchRaw[e.slot * 2 + 1];
      if ((d0 & 3) != 3) { yrej(7); xlatWhy[7]++; return false; }  // unpopulated fetch constant
      const uint32_t addrDwords = (d0 >> 2) & 0x3FFFFFFF;
      const uint8_t* base = gpuPhysToHost(addrDwords * 4);
      if (!base) { yrej(7); xlatWhy[7]++; return false; }
      // iter-425 PLATFORM ACCOMMODATION (SR_VB_ALIAS_PROBE, default ON, =0 A/B):
      // our harness does NOT fold the physical windows (folding writes is the
      // known trunk-regressor, iter-373 — see aliasGuestVA and
      // MEMORY_MODEL_PLAN.md §2.2/§3), so a vertex buffer the guest wrote
      // through an 0xA0000000-class VA is INVISIBLE at the 0x80000000-window
      // read this decoder does. Measured: the movie quad's VB at phys
      // 0x0E7068B0 reads all-zero here while the movie system's objects live
      // at 0xAE-class VAs — a degenerate quad, 192 correct-shader draws
      // painting nothing. On CONSOLE every window holds identical bytes, so
      // preferring whichever window actually has data is console-consistent
      // whenever exactly one was written — which is precisely our situation.
      // Read-side only: no write folding, no allocator collision risk.
      {
        static const bool s_probe = [] {
          const char* e2 = std::getenv("SR_VB_ALIAS_PROBE");
          return !e2 || e2[0] != '0';
        }();
        if (s_probe) {
          bool zero8 = true;
          for (int b = 0; b < 64; b++) if (base[b]) { zero8 = false; break; }
          if (yuvRej) {
            const uint8_t* altD =
                sr::kernel::guestVAToHost(0xA0000000u + (addrDwords * 4));
            uint32_t anz = 0;
            if (altD) { for (int b = 0; b < 64; b++) if (altD[b]) anz++; }
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[VB_PROBE] YUV attr slot=%u phys=%08X zero8=%d altNz64=%u",
                        e.slot, addrDwords * 4, zero8 ? 1 : 0, anz);
          }
          if (zero8) {
            const uint8_t* alt =
                sr::kernel::guestVAToHost(0xA0000000u + (addrDwords * 4));
            if (alt) {
              bool zeroA = true;
              for (int b = 0; b < 64; b++) if (alt[b]) { zeroA = false; break; }
              if (!zeroA) {
                static std::atomic<uint32_t> s_aliasLog{0};
                const uint32_t an = s_aliasLog.fetch_add(1, std::memory_order_relaxed) + 1;
                if (an <= 8 || an % 2000 == 0)
                  SR_LOG_BOOT(sr::LogSeverity::Info,
                              "[VB_ALIAS] #%u vfetch @phys %08X: 0x8-window zero, "
                              "using 0xA-window data",
                              an, addrDwords * 4);
                base = alt;
              }
            }
          }
        }
      }
      srcs[i].base = base + e.offsetDwords * 4;
      srcs[i].strideBytes = e.strideDwords * 4;
      srcs[i].endian = d1 & 3;
      srcs[i].sizeBytes = ((d1 >> 2) & 0x00FFFFFF) * 4;
      livetrace::memRead(addrDwords * 4, base, srcs[i].sizeBytes);
      // iter-427: container-hashed VSes carry PLACEHOLDER vfetch instructions
      // (stride 0) that the console runtime patches from the vertex decl at
      // draw time. The stream-source fetch constant is tightly sized, so
      // size/vertexCount reproduces the patched stride (movie VB: 144B / 6
      // verts = 24B). Without this every vertex reads offset 0 -> degenerate.
      if (!srcs[i].strideBytes && c.vertexCount)
        srcs[i].strideBytes = (srcs[i].sizeBytes / c.vertexCount) & ~3u;
    }
    // iter-427c: the movie quad renders as a WEDGE — name the true VB layout
    // from the bytes the decoder actually reads (post-alias pointer, so this
    // log cannot lie the way the 0x8-window raw log did).
    if (isYuvHle) {
      static std::atomic<uint32_t> s_vbDump{0};
      if (s_vbDump.fetch_add(1, std::memory_order_relaxed) < 4) {
        const uint8_t* vb0 = srcs[0].base;
        char vb[520]; int vn = 0;
        vn += std::snprintf(vb, sizeof(vb), "[YUV_VB36]");
        for (int d = 0; d < 36 && vn < 470; d++) {
          uint32_t w;
          std::memcpy(&w, vb0 + d * 4, 4);
          vn += std::snprintf(vb + vn, sizeof(vb) - vn, " %08X", w);
        }
        SR_LOG_BOOT(sr::LogSeverity::Info, "%s", vb);
      }
    }
    const uint8_t* ib = nullptr;
    if (c.idxBasePhys)
      ib = gpuPhysToHost(c.idxBasePhys);
    // iter-435: index buffers have the same 0xA-window blindness as vertex
    // buffers (SR_VB_ALIAS_PROBE above) — the Apt UI's IBs live in 0xA-class
    // allocations. If the physical window reads all-zero, try the 0xA alias.
    if (ib && c.vertexCount) {
      const uint32_t span = c.vertexCount * (c.idxFmt32 ? 4u : 2u);
      const uint32_t chk = span < 64u ? span : 64u;
      bool zero8 = true;
      for (uint32_t b = 0; b < chk; b++) if (ib[b]) { zero8 = false; break; }
      if (zero8) {
        const uint8_t* alt = sr::kernel::guestVAToHost(0xA0000000u + c.idxBasePhys);
        if (alt && sr::kernel::isGuestVAInRange(0xA0000000u + c.idxBasePhys + span)) {
          bool zeroA = true;
          for (uint32_t b = 0; b < chk; b++) if (alt[b]) { zeroA = false; break; }
          if (!zeroA) {
            static std::atomic<uint32_t> s_ibAlias{0};
            const uint32_t ian = s_ibAlias.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ian <= 8 || ian % 2000 == 0)
              SR_LOG_BOOT(sr::LogSeverity::Info,
                          "[IB_ALIAS] #%u idx @phys %08X: 0x8-window zero, using 0xA-window",
                          ian, c.idxBasePhys);
            ib = alt;
          }
        }
      }
    }

    if (ib && c.vertexCount) {
      pm4NoteZeroIb(c, ib, -1);  // iter-460 [IB_ZERO]
      livetrace::memRead(c.idxBasePhys, ib, c.vertexCount * (c.idxFmt32 ? 4u : 2u));
    }
    auto readIndex = [&](uint32_t i) -> uint32_t {
      // iter-435: hleBaseVertex = DrawIndexedVertices' base-vertex argument
      // (VGT_INDX_OFFSET) — added to every fetched index. Zero everywhere else.
      if (!ib) return i + c.hleBaseVertex;
      if (c.idxFmt32) {
        uint32_t raw;
        std::memcpy(&raw, ib + size_t(i) * 4u, 4);
        if (c.idxEndian == 2) raw = _byteswap_ulong(raw);
        else if (c.idxEndian == 3) raw = (raw >> 16) | (raw << 16);
        return (raw & 0xFFFFFFu) + c.hleBaseVertex;
      }
      uint16_t raw;
      std::memcpy(&raw, ib + size_t(i) * 2u, 2);
      if (c.idxEndian == 1) raw = _byteswap_ushort(raw);
      return uint32_t(raw) + c.hleBaseVertex;
    };

    auto fetchOne = [&](uint32_t srcIdx, float* dst) {
      for (uint32_t i = 0; i < nAttrs; i++) {
        const auto& e = lay[i];
        const uint64_t off = uint64_t(srcIdx) * srcs[i].strideBytes;
        // Bounds vs the fetch constant's own size — never read past the guest
        // buffer (an unmapped guest page is a FATAL host AV under the VEH).
        // The format's OWN width: a flat 16 zeroed every sub-16-byte attribute
        // of a tightly sized buffer's last vertex (the UI quads' 6th vertex
        // lost its colour and UV).
        const uint32_t fmtBytes = xeFormatBytes(e.format);
        if (off + uint64_t(e.offsetDwords) * 4 + (fmtBytes ? fmtBytes : 16u) > srcs[i].sizeBytes) {
          dst[i * 4 + 0] = 0; dst[i * 4 + 1] = 0; dst[i * 4 + 2] = 0; dst[i * 4 + 3] = 1;
          continue;
        }
        xeDecodeAttr(e, srcs[i].base + off, srcs[i].endian, dst + i * 4);
      }
    };

    float* out = reinterpret_cast<float*>(m_xlatVBPtr + m_xlatVBUsed);
    if (rectList) {
      const uint32_t rects = c.vertexCount / 3u;
      const uint32_t nf = nAttrs * 4;
      float v[3][64];
      float v3[64];
      for (uint32_t r = 0; r < rects; r++) {
        for (int k = 0; k < 3; k++) fetchOne(readIndex(r * 3u + k), v[k]);
        // iter-359k: the 4th vertex mirrors the RIGHT-ANGLE corner across the
        // opposite edge — but which of the 3 verts is that corner depends on
        // the rect's winding (Xenia detects it by shared coordinates). The old
        // fixed v3=-v0+v1+v2 assumed corner=v0; wrong-winding rects put their
        // derived triangle across the screen — the black depth-prime wedges
        // AND the diagonal slices through the UI tiles.
        int corner = 0;
        for (int i = 0; i < 3; i++) {
          const int j = (i + 1) % 3, k2 = (i + 2) % 3;
          const bool sxj = v[i][0] == v[j][0], syk = v[i][1] == v[k2][1];
          const bool sxk = v[i][0] == v[k2][0], syj = v[i][1] == v[j][1];
          if ((sxj && syk) || (sxk && syj)) { corner = i; break; }
        }
        const int a = (corner + 1) % 3, b = (corner + 2) % 3;
        for (uint32_t f = 0; f < nf; f++)
          v3[f] = v[a][f] + v[b][f] - v[corner][f];
        std::memcpy(out + size_t(written + 0) * nf, v[0], nf * 4);
        std::memcpy(out + size_t(written + 1) * nf, v[1], nf * 4);
        std::memcpy(out + size_t(written + 2) * nf, v[2], nf * 4);
        std::memcpy(out + size_t(written + 3) * nf, v[a], nf * 4);
        std::memcpy(out + size_t(written + 4) * nf, v[b], nf * 4);
        std::memcpy(out + size_t(written + 5) * nf, v3, nf * 4);
        written += 6;
      }
    } else if (quadList) {
      // kQuadList: 4 verts per quad -> 2 triangles (gfx/pm4_quad_gate.hpp).
      const uint32_t quads = c.vertexCount / 4u;
      const uint32_t nf = nAttrs * 4;
      float v[4][64];
      for (uint32_t q = 0; q < quads; q++) {
        for (int k = 0; k < 4; k++) fetchOne(readIndex(q * 4u + k), v[k]);
        // iter-467 freezefx (gfx/pm4_quad_gate.hpp): Xenia's GS split (0,1,3)(3,1,2); SR_PM4_QUAD_STRIP=0 old.
        const int* sp = pm4QuadSplit();
        for (int k = 0; k < 6; k++) std::memcpy(out + size_t(written + k) * nf, v[sp[k]], nf * 4);
        written += 6;
      }
    } else if (listConvert) {
      // Triangle list from a strip (with primitive reset) or a fan. The reset
      // test reads the index BEFORE the base-vertex offset (registers.h:340:
      // reset check -> offsetting -> clamping). Odd strip triangles keep the
      // D3D winding (k+1, k, k+2).
      const uint32_t nf = nAttrs * 4;
      float p0[64], p1[64], cur[64];
      uint32_t have = 0, parity = 0;
      for (uint32_t i = 0; i < c.vertexCount; i++) {
        if (stripReset && ib) {
          uint32_t raw;
          if (c.idxFmt32) {
            std::memcpy(&raw, ib + size_t(i) * 4u, 4);
            if (c.idxEndian == 2) raw = _byteswap_ulong(raw);
            else if (c.idxEndian == 3) raw = (raw >> 16) | (raw << 16);
            raw &= 0xFFFFFFu;
          } else {
            uint16_t r16;
            std::memcpy(&r16, ib + size_t(i) * 2u, 2);
            if (c.idxEndian == 1) r16 = _byteswap_ushort(r16);
            raw = r16;
          }
          if (raw == c.primResetIndex) {
            have = 0;
            parity = 0;
            continue;
          }
        }
        fetchOne(readIndex(i), cur);
        if (have >= 2) {
          float* o = out + size_t(written) * nf;
          const float* a = fanList ? p0 : (parity ? p1 : p0);
          const float* b = fanList ? p1 : (parity ? p0 : p1);
          std::memcpy(o, a, nf * 4);
          std::memcpy(o + nf, b, nf * 4);
          std::memcpy(o + 2 * nf, cur, nf * 4);
          written += 3;
          parity ^= 1u;
        }
        if (fanList) {
          std::memcpy(have == 0 ? p0 : p1, cur, nf * 4);  // p0 = hub, p1 = previous rim
        } else {
          std::memcpy(p0, p1, nf * 4);
          std::memcpy(p1, cur, nf * 4);
        }
        have++;
      }
    } else {
      for (uint32_t i = 0; i < c.vertexCount; i++, written++)
        fetchOne(readIndex(i), out + size_t(written) * nAttrs * 4);
    }
#if SR_ORACLE
    if (!written) return false;
#else
    if (!written) { nativeXlatReject = 11; return false; }
#endif
    vbOffsetBytes = m_xlatVBUsed;
    m_xlatVBUsed += uint64_t(written) * strideOut;
    vbDedup.emplace(dkey, std::make_pair(vbOffsetBytes, written));
    }  // dedupe miss

    // P1 instrument: SR_PM4_VLOG=<vs hash high 8 hex digits> logs the decoded
    // canonical vertices (float4 per attribute) of the first 4 matching draws;
    // SR_PM4_VLOG_PHYS=<hex> narrows it to draws whose attribute 0 fetch
    // starts at that physical address. SR_PM4_VSCENSUS=1 logs one line per
    // distinct VS: primitive, index/reset state and the vertex layout.
    {
      static const bool s_vsCensus = [] {
        const char* e = std::getenv("SR_PM4_VSCENSUS");
        return e && e[0] == '1';
      }();
      if (s_vsCensus) {
        static std::unordered_map<uint64_t, uint32_t> s_seen;
        const uint32_t n = ++s_seen[c.vsUcodeHash];
        if (n == 1) {
          char line[640];
          int ln = std::snprintf(line, sizeof(line),
                                 "[PM4_VS] vs=%016llX ps=%016llX prim=%u verts=%u idx=%s reset=%d/%X "
                                 "hleLay=%u hleVd=%u attrs:",
                                 (unsigned long long)c.vsUcodeHash, (unsigned long long)c.psUcodeHash,
                                 c.primType, c.vertexCount,
                                 c.idxBasePhys ? (c.idxFmt32 ? "32" : "16") : "auto",
                                 int((c.suScModeCntl >> 21) & 1u), c.primResetIndex,
                                 c.hleLayoutCount, c.hleVdeclCount);
          for (uint32_t a = 0; a < nAttrs && ln < 600; a++)
            ln += std::snprintf(line + ln, sizeof(line) - ln, " s%u+%d/%u f%u u%u.%u%s%s", lay[a].slot,
                                lay[a].offsetDwords, lay[a].strideDwords, lay[a].format, lay[a].usage,
                                lay[a].usageIndex, lay[a].isSigned ? "s" : "",
                                lay[a].isInteger ? "i" : "");
          SR_LOG_BOOT(sr::LogSeverity::Info, "%s", line);
        }
      }
    }
    {
      static const uint32_t s_vlog = [] {
        const char* e = std::getenv("SR_PM4_VLOG");
        return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
      }();
      static const uint32_t s_vlogPhys = [] {
        const char* e = std::getenv("SR_PM4_VLOG_PHYS");
        return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
      }();
      const uint32_t a0Phys = nAttrs ? ((c.fetchRaw[lay[0].slot * 2] >> 2) & 0x3FFFFFFFu) * 4u : 0u;
      static std::atomic<uint32_t> s_vlogN{0};
      if (s_vlog && !vsf && uint32_t(c.vsUcodeHash >> 32) == s_vlog && written &&
          (!s_vlogPhys || a0Phys == s_vlogPhys) && s_vlogN.fetch_add(1) < 4) {
        const float* vbf = reinterpret_cast<const float*>(m_xlatVBPtr + vbOffsetBytes);
        for (uint32_t a = 0; a < nAttrs; a++) {
          const uint32_t d0 = c.fetchRaw[lay[a].slot * 2], d1 = c.fetchRaw[lay[a].slot * 2 + 1];
          const uint32_t phys = ((d0 >> 2) & 0x3FFFFFFFu) * 4u;
          const uint8_t* p8 = gpuPhysToHost(phys);
          uint32_t w0 = 0;
          if (p8) std::memcpy(&w0, p8, 4);
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[PM4_VLOG] attr%u slot=%u off=%d stride=%u fmt=%u fetch=%08X %08X phys=0x%08X "
                      "first dword@0x8=%08X dedupeHit=%d",
                      a, lay[a].slot, lay[a].offsetDwords, lay[a].strideDwords, lay[a].format, d0, d1,
                      phys, w0, dit != vbDedup.end() ? 1 : 0);
        }
        for (uint32_t v = 0; v < written && v < 8; v++) {
          char line[512];
          int n = std::snprintf(line, sizeof(line), "[PM4_VLOG] vs=%08X v%u:", s_vlog, v);
          for (uint32_t a = 0; a < nAttrs && n < 460; a++) {
            const float* f = vbf + size_t(v) * nAttrs * 4 + size_t(a) * 4;
            n += std::snprintf(line + n, sizeof(line) - n, " a%u(u%u.%u)=(%.1f,%.1f,%.2f,%.2f)", a,
                               lay[a].usage, lay[a].usageIndex, f[0], f[1], f[2], f[3]);
          }
          SR_LOG_BOOT(sr::LogSeverity::Info, "%s", line);
        }
      }
    }
    // P1: a screen-space draw (clip disabled + vtx_xy_fmt: pre-transformed
    // pixel positions, the clear-rect idiom) claims EDRAM only down to its
    // lowest vertex - Xenia bounds these by running the VS on the CPU
    // (draw_extent_estimator.cc). The decoded POSITION input stands in for the
    // pass-through VS output here.
    if (pm4 && !vsf && ((pmRegs.clipCntl >> 16) & 1u) && ((pmRegs.vteCntl >> 8) & 1u)) {
      int32_t posAttr = -1;
      for (uint32_t i = 0; i < nAttrs; i++)
        if (lay[i].usage == 0u && lay[i].usageIndex == 0u) { posAttr = int32_t(i); break; }
      if (posAttr >= 0 && written) {
        const float* vbf = reinterpret_cast<const float*>(m_xlatVBPtr + vbOffsetBytes);
        float maxY = 0.0f;
        for (uint32_t v = 0; v < written; v++) {
          const float y = vbf[size_t(v) * nAttrs * 4 + size_t(posAttr) * 4 + 1];
          if (y == y) maxY = std::max(maxY, y);
        }
        if ((pmRegs.suScModeCntl >> 16) & 1u) maxY += float(edram::windowOffsetY(pmRegs));
        const int32_t bound = int32_t(std::min(std::ceil(std::max(maxY, 0.0f) + 0.5f), 16384.0f));
        const CachedShader* cps2 = shaderCache.findPs(c.psUcodeHash);
        const bool polygonal = !(c.primType == 1u || c.primType == 2u || c.primType == 3u);
        const edram::DrawTargets prepared = pmTargets;
        pmTargets = edram::computeDrawTargets(pmRegs, cps2 ? cps2->colorOutMask() : 0u,
                                              polygonal, bound);
        // iter-467 native-b B1a: keep pm4PrepareDraw's XDK-clear coverage (SR_NB_CLEAR_ELIDE).
        pmTargets.fullWrite = prepared.fullWrite;
        std::memcpy(pmTargets.fullRect, prepared.fullRect, sizeof(pmTargets.fullRect));
        pmTargets.rectExact = prepared.rectExact;  // B1c (SR_NB_CLEAR_PROP)
        pmTargets.clearValid = prepared.clearValid;
        pmTargets.clearStencil = prepared.clearStencil;
        std::memcpy(pmTargets.clearColor, prepared.clearColor, sizeof(pmTargets.clearColor));
      }
    }

    // Per-draw constant blocks: VS floats (b0/b3), PS floats (b1/b4), shared (b2).
    auto alignUp = [](uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); };
    uint64_t cb = alignUp(m_xlatCBUsed, 256);
    // iter-466 cp round 2 (SR_CP_CONST_DEDUP): Xenia uploads a float-constant buffer only when its
    // registers were written since (cbuffer_binding_float_*.up_to_date). Here: a draw whose used
    // VS / PS blocks hold the same bytes as the previous upload in this set binds that upload again
    // instead of writing 4 KB-class blocks into the write-combined heap.
    static const bool s_constDedup = cpsimd::gateLogged("SR_CP_CONST_DEDUP", false);
    auto sameUsed = [](const uint32_t* a, const uint32_t* b, uint32_t use) {
      if ((use & 0x10000u) || (use & 0xFFFFu) == 0xFFFFu || (use & 0xFFFFu) == 0u)
        return std::memcmp(a, b, 4096) == 0;
      for (uint32_t m = use & 0xFFFFu; m;) {
        const uint32_t bk = uint32_t(__builtin_ctz(m));
        if (std::memcmp(a + 64u * bk, b + 64u * bk, 256) != 0) return false;
        m &= m - 1u;
      }
      return true;
    };
    // iter-466 cp round 2 (SR_CP_CONST_TRACK): a tracked command's constants are the drain's
    // merged copy (cvMerge); its upload is reused while the set's latest upload covers its blocks
    // and none of them changed since (Xenia's up_to_date). Untracked: as before. A PS upload the
    // host options may patch (gfxopt::kPsToneMapBloom) is never shared, in either mode.
    const uint32_t* aluVs = c.constTracked ? m_cvCons : c.aluVs;
    const uint32_t* aluPs = c.constTracked ? m_cvCons + 1024 : c.aluPs;
    auto full16 = [](uint32_t u) { return ((u & 0x10000u) || (u & 0xFFFFu) == 0u) ? 0xFFFFu : (u & 0xFFFFu); };
    const uint32_t uVs = full16(c.vsConstUse), uPs = full16(c.psConstUse);
    const bool psPatched = c.psUcodeHash == gfxopt::kPsToneMapBloom;
    const bool vsReuse = c.constTracked
                             ? (m_cvUpVsOff != ~0ull && (uVs & ~m_cvUpVsMask) == 0 && (uVs & m_cvUpDirty) == 0)
                             : (s_constDedup && m_cdVsOff != ~0ull && c.vsConstUse == m_cdVsUse &&
                                sameUsed(c.aluVs, m_cdVs, c.vsConstUse));
    const bool psReuse = !psPatched &&
                         (c.constTracked
                              ? (m_cvUpPsOff != ~0ull && (uPs & ~m_cvUpPsMask) == 0 &&
                                 ((uPs << 16) & m_cvUpDirty) == 0)
                              : (s_constDedup && m_cdPsOff != ~0ull && c.psConstUse == m_cdPsUse &&
                                 sameUsed(c.aluPs, m_cdPs, c.psConstUse)));
    uint64_t vsOff = vsReuse ? (c.constTracked ? m_cvUpVsOff : m_cdVsOff) : cb;
    if (!vsReuse) cb += 4096;
    uint64_t psOff = psReuse ? (c.constTracked ? m_cvUpPsOff : m_cdPsOff) : alignUp(cb, 256);
    if (!psReuse) cb = psOff + 4096;
    const uint64_t shOff = alignUp(cb, 256); cb = shOff + 512;
#if SR_ORACLE
    // step 6 drop fix: the assert - a reused block lies below the cursor (the census of reuses of a dropped draw's
    // block, where the old build bound overwritten bytes, follows the admission checks below)
    if (vsReuse) xlatReuseBelowCursor(vsOff, 4096, m_xlatCBUsed, "vs", c.drawOrdinal);
    if (psReuse) xlatReuseBelowCursor(psOff, 4096, m_xlatCBUsed, "ps", c.drawOrdinal);
#endif
    if (cb > cbHalfEnd) { yrej(8); xlatWhy[8]++; return false; }
    // step 6 I10p: on the view path shOff is the draw's shared SLOT in the twin (the render thread writes it); the CP's
    // shared block goes to the compact scratch (shScr), the render's read-modify-write source.
    uint64_t shScr = shOff;
    if (m_zcOn) {
      shScr = alignUp(m_xlatShUsed, 256);
      if (shScr + 512 > shHalfEnd) { zcBump(m_zcShFull); yrej(8); xlatWhy[8]++; return false; }
    }
    if (!xlatCommit(0, m_zcOn ? shScr + 512 : cb)) { yrej(8); xlatWhy[8]++; return false; }   // step 4 7d(b): before the block writes
#if SR_ORACLE
    const bool vsDropped = vsReuse && droppedBlock(vsOff), psDropped = psReuse && droppedBlock(psOff);
    if (vsDropped || psDropped) {
      const uint64_t n = g_dropThenReuse.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 32)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[XLAT_DROP] reuse of a dropped draw's block: draw#%u set=%llu %s%s (the old build bound the bytes "
                    "the next allocation wrote over them)", c.drawOrdinal, (unsigned long long)m_cpSetSerial,
                    vsDropped ? "vs " : "", psDropped ? "ps" : "");
    }
#endif
    if (c.constTracked) {
      m_cvUpReuse += (vsReuse ? 1u : 0u) + (psReuse ? 1u : 0u);
    } else {
      if (vsReuse) m_cdHits++;
      if (psReuse) m_cdHits++;
    }
    {
      // SR_PM4_CONST_USED: only the blocks the shaders read (c.vsConstUse / psConstUse).
      auto upUsed = [](uint8_t* dst, const uint32_t* src, uint32_t use) {
        if ((use & 0x10000u) || (use & 0xFFFFu) == 0xFFFFu || (use & 0xFFFFu) == 0u) {
          std::memcpy(dst, src, 4096);
          return;
        }
        for (uint32_t m = use & 0xFFFFu; m;) {
          const uint32_t b = uint32_t(__builtin_ctz(m));
          std::memcpy(dst + 256u * b, src + 64u * b, 256);
          m &= m - 1u;
        }
      };
      // step 6 I10p: the view path writes the same used blocks (M1: xlatUploadUsedBlocks, the upUsed pattern) ONCE,
      // straight into the write-only twin at the same offsets; a PS the host options patch is composed in cacheable
      // memory (m_zcPsTmp) first - nothing ever reads the twin.
      auto zcUp = [&](uint64_t off, const uint32_t* src, uint32_t use) {
        zcBump(m_zcBytes, xlatUploadUsedBlocks([&](uint64_t d, const void* p, size_t n) { m_xlatCBGpu.write(d, p, n); },
                                          off, src, use));
      };
      if (!vsReuse) {
        if (m_zcOn) {
          zcUp(vsOff, aluVs, c.vsConstUse);
          zcBump(m_zcVsWrites);
        } else {
          upUsed(m_xlatCBPtr + vsOff, aluVs, c.vsConstUse);
        }
      }
      if (!psReuse) {
        if (m_zcOn) {
          const uint32_t* psSrc = aluPs;
          if (psPatched) {
            std::memcpy(m_zcPsTmp, aluPs, sizeof(m_zcPsTmp));
            gfxopt::patchPsConstants(c.psUcodeHash, reinterpret_cast<float*>(m_zcPsTmp), c.psConstUse);
            psSrc = m_zcPsTmp;
          }
          zcUp(psOff, psSrc, c.psConstUse);
          zcBump(m_zcPsWrites);
        } else {
          upUsed(m_xlatCBPtr + psOff, aluPs, c.psConstUse);
          // iter-466 gfx: bloom / edge blur scale one constant of the game's post shaders in this
          // host copy (gfx_options.hpp); nothing is written at the defaults. (An upload of that
          // shader is never shared with another draw: psPatched above.)
          gfxopt::patchPsConstants(c.psUcodeHash, reinterpret_cast<float*>(m_xlatCBPtr + psOff),
                                   c.psConstUse);
        }
      }
      if (c.constTracked) {
        if (!vsReuse) {
          m_cvUpVsOff = vsOff;
          m_cvUpVsMask = uVs;
          m_cvUpDirty &= ~0xFFFFu;
        }
        if (!psReuse) {
          m_cvUpPsOff = psPatched ? ~0ull : psOff;  // a possibly patched upload is never rebound
          m_cvUpPsMask = uPs;
          m_cvUpDirty &= 0xFFFFu;
        }
      } else if (s_constDedup) {  // the host copy of this set's latest upload (cache-resident compare source)
        if (!vsReuse) {
          upUsed(reinterpret_cast<uint8_t*>(m_cdVs), c.aluVs, c.vsConstUse);
          m_cdVsUse = c.vsConstUse;
          m_cdVsOff = vsOff;
        }
        if (!psReuse && c.psUcodeHash == gfxopt::kPsToneMapBloom) {
          m_cdPsOff = ~0ull;  // possibly patched: never bound for another draw
        } else if (!psReuse) {
          upUsed(reinterpret_cast<uint8_t*>(m_cdPs), c.aluPs, c.psConstUse);
          m_cdPsUse = c.psConstUse;
          m_cdPsOff = psOff;
        }
      }
    }
    uint32_t shared[128] = {};  // c0..c15 desc indices, c16 bools, c18 NDC
    // iter-359j: descriptor indices [0..15] are written at RENDER time (batch
    // execution) from the fetch groups stored in the batch — see renderGuest
    // Geometry. Drain-time creation here read PRE-frame guest memory, so any
    // draw sampling a same-frame resolve got stale bytes.
    shared[64] = (c.boolConsts[0] & 0xFFFFu) | ((c.boolConsts[4] & 0xFFFFu) << 16);
    // c17.x g_AlphaThreshold = 0 (spec-const alpha test compiled out) - except for the alpha-test variant
    // (step 4 lane C, gfx/alpha_test.hpp, tools/xenosrecomp/0005): c17.x = RB_ALPHA_REF, c17.y = 8 | alpha_func.
    // Only the variant declares c17.y; the plain shaders never read either. Off, the block is unchanged.
    if (alphatest::enabled()) {
      shared[68] = c.alphaRef;
      shared[69] = alphatest::word(c.colorControl);
    }
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    D3D12_VIEWPORT vp{0.0f, 0.0f, float(m_width), float(m_height), 0.0f, 1.0f};
    // iter-362 text-collapse fix (SR_XLAT_FULLVP, A/B): the menu-text draws
    // carry a DEGENERATE vpScale (~0.5) — in the VTE-on path W=2*0.5=1px, in the
    // VTE-off path passW=1px — collapsing the whole draw into a 1x1 viewport at
    // the top-left (the "text on row 0" strip). A genuine sub-pass carries a real
    // vpScale (640-pass → vpScale 320 → 640px), so only the <~8px case is the
    // don't-care default; fall back to the full render target then. VTE-on: the
    // VS emits clip space, so a full-screen viewport maps [-1,1]→full RT.
    static const bool s_fullVp = [] {
      const char* e = std::getenv("SR_XLAT_FULLVP"); return e && e[0] == '1'; }();
    const float kMinPass = s_fullVp ? 8.0f : 1.0f;
    D3D12_RECT pmScissor{};
    if (pm4) {
      // P1: Xenia's GetHostViewportInfo / GetScissor (edram::computeViewport):
      // window offset, half pixel, clip-disabled pixel draws, Y flip.
      const uint64_t dv0 = sr::native::dcensus::on() ? __rdtsc() : 0;
      bool vpOk;
      if (reused && t_reuse.vpValid) {
#if SR_ORACLE
        // step 6 R6 CHECK (Astra r1 P2): the miss path's own seeds (this draw's vp / ndc before the reuse overwrites them)
        D3D12_VIEWPORT vp2 = vp;
        float ndc2[4] = {ndc[0], ndc[1], ndc[2], ndc[3]};
#endif
        vpOk = t_reuse.vpOk;
        vp = t_reuse.vp;
        std::memcpy(ndc, t_reuse.ndc, sizeof(ndc));
        pmScissor = t_reuse.sc;
#if SR_ORACLE
        if (drainReuseCheck()) {
          D3D12_RECT sc2{};
          // recomputed from the FRESH prepare (t_chkRegs / t_chkTargets), not from the reused pmRegs / pmTargets
          const bool ok2 = chkFresh ? pm4ViewportScissor(t_chkRegs, t_chkTargets, c.psUcodeHash, vp2, ndc2, sc2)
                                    : pm4ViewportScissor(pmRegs, pmTargets, c.psUcodeHash, vp2, ndc2, sc2);
          g_reuseChecked.fetch_add(1, std::memory_order_relaxed);
          if (ok2 != vpOk || (ok2 && (std::memcmp(&vp2, &vp, sizeof(vp)) != 0 || std::memcmp(ndc2, ndc, sizeof(ndc)) != 0 ||
                                      std::memcmp(&sc2, &pmScissor, sizeof(sc2)) != 0))) {
            const uint64_t bad = g_reuseBad.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bad <= 16)
              SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_DRAIN_REUSE] BAD viewport draw#%u", c.drawOrdinal);
          }
        }
#endif
      } else {
        vpOk = pm4ViewportScissor(pmRegs, pmTargets, c.psUcodeHash, vp, ndc, pmScissor);
        if (reuseCand && t_reuse.seq == t_drainSeq.v) {
          t_reuse.vpValid = true;
          t_reuse.vpOk = vpOk;
          t_reuse.vp = vp;
          std::memcpy(t_reuse.ndc, ndc, sizeof(ndc));
          t_reuse.sc = pmScissor;
        }
      }
      if (sr::native::dcensus::on()) sr::native::dcensus::noteViewport(dcSame, __rdtsc() - dv0);
      if (!vpOk) {  // empty viewport: rasterizes nothing
        // step 6 drop fix: this draw's VS / PS blocks are written and the reuse state may name them - the cursor
        // passes them (the next draw's allocation would start on them).
#if SR_ORACLE
        if (!vsReuse || !psReuse) {
          g_dropAfterWrite.fetch_add(1, std::memory_order_relaxed);
          for (const uint64_t off : {!vsReuse ? vsOff : ~0ull, !psReuse ? psOff : ~0ull}) {
            if (off == ~0ull) continue;
            if (m_dropN >= kDropOffsets) g_dropOffsetsLost.fetch_add(1, std::memory_order_relaxed);
            m_dropOff[m_dropN % kDropOffsets] = off;
            ++m_dropN;
          }
        }
        if (!xlatDropMutant())
#endif
          m_xlatCBUsed = cb;
        return true;
      }
    } else if (c.vteCntl & 0x1) {
      // VTE on: the VS outputs clip space; the game's viewport regs map NDC to
      // pixels exactly like the D3D12 viewport transform (Y flip via vsy<0).
      const float vsx = c.vpScale[0], vsy = c.vpScale[1];
      const float vox = c.vpOffset[0], voy = c.vpOffset[1];
      const float W = 2.0f * vsx, H = -2.0f * vsy;
      // Degenerate (default) vpScale → leave vp at the full-RT default above.
      if (W >= kMinPass && H >= kMinPass) {
        vp.TopLeftX = vox - vsx;
        vp.TopLeftY = voy + vsy;
        vp.Width = W;
        vp.Height = H;
        float z0 = c.vpOffset[2], z1 = c.vpOffset[2] + c.vpScale[2];
        if (z1 < z0) std::swap(z0, z1);
        vp.MinDepth = z0 < 0 ? 0.0f : (z0 > 1 ? 1.0f : z0);
        vp.MaxDepth = z1 < vp.MinDepth ? vp.MinDepth : (z1 > 1 ? 1.0f : z1);
        if (vp.MaxDepth <= vp.MinDepth) vp.MaxDepth = vp.MinDepth + 1e-3f;
      }
    } else {
      // VTE off (UI/clear rects): the VS outputs PIXELS — remap to NDC in the
      // shader epilogue via g_NdcScaleOffset (c18). iter-359i: the pixel space
      // is the PASS surface, whose dimensions the viewport regs still carry
      // (W=2|vpScale.x|, H=2|vpScale.y|) — the helper pass is 640×640, and
      // remapping by the 1280×720 window landed its clear rect (and every
      // probe quad) at half size in the corner.
      float passW = 2.0f * (c.vpScale[0] < 0 ? -c.vpScale[0] : c.vpScale[0]);
      float passH = 2.0f * (c.vpScale[1] < 0 ? -c.vpScale[1] : c.vpScale[1]);
      // iter-362 text-collapse fix: for VTX_XY_FMT draws (vte bit8, viewport
      // scale/offset DISABLED) the game leaves vpScale at a don't-care default
      // (~0.5 → passW≈1), so deriving the pass size from it collapses the whole
      // draw into a 1x1 viewport at the top-left (the "text on row 0" bug). A
      // real sub-pass carries a genuine vpScale (640-pass → vpScale 320 →
      // passW 640), so only the degenerate <~8px case is the default — fall
      // back to the full render target then (kMinPass computed above).
      if (passW < kMinPass) passW = float(m_width);
      if (passH < kMinPass) passH = float(m_height);
      ndc[0] = 2.0f / passW;
      ndc[1] = -2.0f / passH;
      // Land the pass 1:1 in the RT's top-left (where the resolve reads).
      vp.Width = passW < float(m_width) ? passW : float(m_width);
      vp.Height = passH < float(m_height) ? passH : float(m_height);
      ndc[2] = -1.0f;
      ndc[3] = 1.0f;
    }
    std::memcpy(&shared[72], ndc, 16);  // c18
    // iter-370 kPointList: c19.xy = the sprite's NDC radius, consumed by
    // point_sprite.gs.hlsl. PA_SU_POINT_SIZE holds the HALF width/height in
    // 12.4 fixed point (Xenia registers.h:444), so the screen DIAMETER is
    // field * 2/16; dividing by the viewport extent turns that into an NDC
    // radius because the 0.5 diameter->radius and 2.0 NDC-span factors cancel
    // (d3d12_command_processor.cc:3950 records exactly this cancellation).
    if (pointList) {
      const float diamX = float((c.pointSize >> 16) & 0xFFFFu) * (2.0f / 16.0f);
      const float diamY = float(c.pointSize & 0xFFFFu) * (2.0f / 16.0f);
      const float extX = vp.Width > 1.0f ? vp.Width : float(m_width);
      const float extY = vp.Height > 1.0f ? vp.Height : float(m_height);
      // iter-464 resscale: the PM4 viewport is in host pixels and the diameter in
      // guest pixels - Xenia's point_screen_diameter_to_ndc_radius = scale / extent
      // (d3d12_command_processor.cc:3983-3988). p2.zpd: edram::pointSpriteNdcRadius
      // (the WARP fixture draws the HUD query's point through the same function).
      float pr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      edram::pointSpriteNdcRadius(c.pointSize, extX, extY, pm4 ? resscale::get().x : 1u,
                                  pm4 ? resscale::get().y : 1u, pr);
      std::memcpy(&shared[76], pr, 16);  // c19
      static std::atomic<uint32_t> s_ptLog{0};
      const uint32_t pn = s_ptLog.fetch_add(1, std::memory_order_relaxed);
      if (pn < 8)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[XLAT_POINT] verts=%u pointSize=0x%08X minmax=0x%08X "
                    "diam=(%.2f,%.2f)px vp=(%.0fx%.0f) ndcR=(%.5f,%.5f)",
                    c.vertexCount, c.pointSize, c.pointMinMax, diamX, diamY,
                    vp.Width, vp.Height, pr[0], pr[1]);
    }
    std::memcpy(m_xlatCBPtr + shScr, shared, sizeof(shared));   // the scratch (I10p: its compact cursor)

    // iter-466 cp round 2 (SR_CP_BATCH_INPLACE): the batch (~1 KB) is built in its vector slot
    // instead of zeroed on the stack and copied in (the zeroing line alone was ~3% of the CP).
    // Nothing between here and the push reads m_drawBatches.
    static const bool s_inplace = cpsimd::gateLogged("SR_CP_BATCH_INPLACE", true);
    std::optional<DrawBatch> stackBatch;
    if (!s_inplace) stackBatch.emplace();
    DrawBatch& batch = s_inplace ? m_drawBatches.emplace_back() : *stackBatch;
    batch.xlat = true;
    batch.xlatPso = pso;
    batch.xlatPsoMark = pm4 ? m_pm4MarkPso : nullptr;  // B1b (SR_NB_COVER_FILL)
    if (pm4 && m_zpdDrainQuery) {  // p2.zpd: the probe pipelines and the draw-line state of a query draw
      batch.zpdRefPso = m_pm4ZpdRefPso;
      batch.zpdRasterPso = m_pm4ZpdRasterPso;
      batch.zpdPointSize = c.pointSize;
      batch.zpdColorCtl = c.colorControl;
      batch.zpdVtxAddr = m_pm4ZpdVtxAddr;
      std::memcpy(batch.zpdVtx, m_pm4ZpdVtx, sizeof(batch.zpdVtx));
    }
    batch.vertexCount = written;
    batch.vertexOffset = uint32_t(vbOffsetBytes);  // BYTE offset (dedupe-aware)
    batch.xlatStride = strideOut;
    batch.topology = topo;
    batch.xlatVsCbOff = vsOff;
    batch.xlatPsCbOff = psOff;
    // The blocks past the last used one are never read: the snapshot stages only up to it.
    auto usedBytes = [](uint32_t use) -> uint16_t {
      const uint32_t blocks = use & 0xFFFFu;
      if ((use & 0x10000u) || blocks == 0u) return 4096;  // relative addressing / unknown: all
      return uint16_t((32u - uint32_t(__builtin_clz(blocks))) * 256u);
    };
    batch.xlatVsBytes = usedBytes(c.vsConstUse);
    batch.xlatPsBytes = usedBytes(c.psConstUse);
    batch.xlatViewport = vp;
    batch.colorInfo = c.colorInfo;
    batch.hdrTarget = hdrTarget;
    if (hdrTarget) nHdrDraws++;
    std::memcpy(batch.texFetch, c.fetchRaw, sizeof(batch.texFetch));
    batch.sharedCbOff = shScr;
    batch.zcView = m_zcOn;   // step 6 I10p: the VS / PS / fetch offsets are views into m_xlatCBGpu
    batch.zcSharedOff = shOff;
    batch.zcSet = m_zcCpSet;
#if SR_ORACLE
    if (m_zcCheck) {   // what each view must hold, from the cacheable sources (compare-on-consume)
      batch.zcVsUse = c.vsConstUse;
      batch.zcPsUse = c.psConstUse;
      batch.zcThird = m_xlatCbThird;
      batch.zcGen = m_zcGen[m_xlatCbThird];
      batch.zcVsHash = xlatCbHash(reinterpret_cast<const uint8_t*>(aluVs), c.vsConstUse, 4096);
      batch.zcPsHash = xlatCbHash(reinterpret_cast<const uint8_t*>(psPatched && !psReuse ? m_zcPsTmp : aluPs),
                                  c.psConstUse, 4096);
      batch.zcFetchHash = vsf ? vsfOut.zcFetchHash : 0;
    }
#endif
    batch.vsHash = c.vsUcodeHash;
    batch.psHash = c.psUcodeHash;
    if (pm4) {
      batch.pm4 = true;
      batch.pm4Regs = pmRegs;
      batch.pm4Targets = pmTargets;
      batch.pm4Scissor = pmScissor;
      batch.pm4Blend0 = c.blendControl;
      batch.drawOrdinal = c.drawOrdinal;
      if (vsf) {
        batch.vsf = true;
        batch.vsfFetchCbOff = vsfOut.fetchCbOff;
        batch.vsfIbOffset = vsfOut.ibOffset;
        batch.vsfRawVertices = std::move(vsfOut.rawVertices);
        batch.vsfShm = vsfOut.shm;
        batch.vsfNoIb = vsfOut.noIb;
        batch.vsfFirstVertex = vsfOut.firstVertex;
      } else if (pm4) {
        if (c.primType == 8u) drainreuse::add(g_drainDecRect, 1u);
        else if (clipOffXy) drainreuse::add(g_drainDecClip, 1u);
        else drainreuse::add(g_drainDecOther, 1u);
      }
    }
    if (memres::resourceDecides() && batch.pm4) {
      // Capture CPU-owned texture bytes at this draw, before later resolves or
      // writes. The renderer binds these copies at the matching batch cursor.
      // A slot equal to the previous pm4 draw's is current again while no note,
      // no newly pending patch and no new stamp happened since it was asked
      // (perf parity round 2: 16 slots on every draw were ~25% of the CP).
      auto& memory = memres::resourceMemory();
#if SR_ORACLE
      const bool skipOk = !memres::resourceAbDual() && !memres::resourceCheckEnabled() &&
                          !memres::resourceConsumerEnabled();
#else
      constexpr bool skipOk = true;
#endif
      const uint64_t seq = memory.sequence(), armed = memory.patchArmed();
      if (!skipOk || seq != m_resourceTexSlotSeq || armed != m_resourceTexSlotArmed ||
          m_shmEpoch != m_resourceTexSlotEpoch || m_rtOn != m_resourceTexSlotRt ||
          m_shmEpochFrameStart != m_resourceTexSlotFrameStart) {
        m_resourceTexSlotValid = 0;
        m_resourceTexSlotSeq = seq;
        m_resourceTexSlotArmed = armed;
        m_resourceTexSlotEpoch = m_shmEpoch;
        m_resourceTexSlotRt = m_rtOn;
        m_resourceTexSlotFrameStart = m_shmEpochFrameStart;
      }
      // step 6 I4: the asked slots' spans are requested together (one m_shmMu hold for their misses).
      memres::ResourceSpan asked[32];
      uint32_t askedCount = 0;
      for (uint32_t slot = 0; slot < 16u; ++slot) {
        const uint32_t* fetch = &batch.texFetch[slot * 6u];
        const uint16_t bit = uint16_t(1u << slot);
        if (skipOk && (m_resourceTexSlotValid & bit) && std::memcmp(m_resourceTexSlot[slot], fetch, 24) == 0) {
          ++m_resourceTexSlotSkips;
          continue;
        }
        ++m_resourceTexSlotAsks;
        askedCount += memres::resourceTextureSpans(fetch, asked + askedCount);
        std::memcpy(m_resourceTexSlot[slot], fetch, 24);
        m_resourceTexSlotValid |= bit;
      }
      pm4ResourceRequestSite(asked, askedCount);
    }
    batch.shmPendBegin = shmPendBegin;
    batch.shmPendEnd = uint32_t(m_shmPending.size());
#if SR_ORACLE
    batch.resourceConsumerChecks = std::move(consumerChecks);
    if (!batch.resourceConsumerChecks.empty()) resourceCaptureConsumer(batch, c);
    if (batch.pm4 && batch.vsf) resourceCaptureFetch(batch, c, *cvs);
#endif
    {
      // iter-466 cp round 2 (SR_CP_TEX_SKIPSAME): the prepass (pm4ShmTexturePrepass) walks every
      // batch's texFetch at the swap, cold; a batch whose texFetch equals the previous pm4 draw
      // batch's collects nothing there (every slot equals the previous one), so it is flagged now,
      // while both are hot. The previous batch is looked up in m_drawBatches itself.
      static const bool s_skipSame = cpsimd::gateLogged("SR_CP_TEX_SKIPSAME", true);
      if (s_skipSame && batch.pm4) {
        const size_t n = m_drawBatches.size() - (s_inplace ? 1u : 0u);  // batches before this one
        for (size_t k = 0; k < 8u && k < n; k++) {
          const DrawBatch& pb = m_drawBatches[n - 1u - k];
          if (pb.resolve || !pb.pm4) continue;
          batch.texSame = std::memcmp(pb.texFetch, batch.texFetch, sizeof(batch.texFetch)) == 0;
          break;
        }
      }
    }
    if (!s_inplace) {
#if SR_ORACLE
      if (memres::resourceConsumerEnabled()) m_drawBatches.push_back(std::move(*stackBatch));
      else
#endif
        m_drawBatches.push_back(*stackBatch);
    }
    m_xlatCBUsed = cb;
    if (m_zcOn) m_xlatShUsed = shScr + 512;
    nXlat++;

    const uint32_t xln = cpDiagBump(s_xlatLogN);
    // iter-362: late menu-render sampling ONLY under the dump diagnostic (see
    // the [D3D12_DRAIN] note — ungated it throttled default runs to 2fps).
    // iter-424: the substituted movie draw (true YUV shaders) is the whole
    // frontier — log EVERY one of its dispositions, uncapped, plus a small
    // general sample. It appears a handful of times per run, so this is cheap.
    const bool isYuvDraw = (c.psUcodeHash == 0x20A5E5204FD23BE2ull);
    // iter-426: one line per DISTINCT PS identity — names every pixel shader
    // the stream draws with, and whether PARAM_GEN is on for it (progCntl b18,
    // pos in ctxMisc b8-15). The YUV-tag miss in the fmv trace replay showed we
    // had no census at all: 151 draws, zero identity lines.
    {
      static std::set<uint64_t> s_seenPs;
      static std::mutex s_seenMx;
      // iter-466 cp round 2 (SR_CP_DRAIN_LEAN): the census is full after 64 identities; the
      // per-draw mutex after that only cost a locked instruction per draw.
      static std::atomic<bool> s_seenFull{false};
      std::unique_lock<std::mutex> lk(s_seenMx, std::defer_lock);
      if (!(cpDiagLean() && s_seenFull.load(std::memory_order_relaxed))) lk.lock();
      if (lk.owns_lock() && s_seenPs.size() >= 64) s_seenFull.store(true, std::memory_order_relaxed);
      if (lk.owns_lock() && s_seenPs.size() < 64 && s_seenPs.insert(c.psUcodeHash).second)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PS_CENSUS] ps=%016llX vs=%016llX prim=%u verts=%u "
                    "progCntl=%08X ctxMisc=%08X paramGen=%u pos=%u "
                    "t0=%u@%08X t1=%u@%08X t2=%u@%08X",
                    (unsigned long long)c.psUcodeHash,
                    (unsigned long long)c.vsUcodeHash, c.primType, c.vertexCount,
                    c.programCntl, c.contextMisc, (c.programCntl >> 18) & 1u,
                    (c.contextMisc >> 8) & 0xFFu,
                    c.fetchRaw[0] & 3u, c.fetchRaw[1] & 0xFFFFF000u,
                    c.fetchRaw[6] & 3u, c.fetchRaw[7] & 0xFFFFF000u,
                    c.fetchRaw[12] & 3u, c.fetchRaw[13] & 0xFFFFF000u);
    }
    // iter-426b: the movie-composite draw (Xenia truth: ps=3839 samples the
    // memexport-converted RGB image) — print its t0 fetch constant verbatim so
    // the scrambled-stripes decode defect names its own format/pitch/tiling.
    if (c.psUcodeHash == 0x38392DB754F93075ull) {
      static std::atomic<uint32_t> s_cmpN{0};
      const uint32_t cn = s_cmpN.fetch_add(1, std::memory_order_relaxed);
      if (cn < 12 || cn % 200 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[CMP_DRAW] #%u t0raw=%08X %08X %08X %08X %08X %08X vs=%016llX",
                    cn, c.fetchRaw[0], c.fetchRaw[1], c.fetchRaw[2],
                    c.fetchRaw[3], c.fetchRaw[4], c.fetchRaw[5],
                    (unsigned long long)c.vsUcodeHash);
    }
    // iter-426: EVERY param_gen draw gets a line (the census dedups by hash, so
    // a stub-hashed movie quad could hide its registers behind an earlier draw).
    if ((c.programCntl >> 18) & 1u) {
      static std::atomic<uint32_t> s_pgN{0};
      const uint32_t pn = cpDiagBump(s_pgN);
      if (pn < 16 || pn % 500 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PGEN_DRAW] #%u ps=%016llX vs=%016llX prim=%u verts=%u "
                    "pos=%u t0=%u@%08X",
                    pn, (unsigned long long)c.psUcodeHash,
                    (unsigned long long)c.vsUcodeHash, c.primType, c.vertexCount,
                    (c.contextMisc >> 8) & 0xFFu,
                    c.fetchRaw[0] & 3u, c.fetchRaw[1] & 0xFFFFF000u);
    }
    // SR_PM4_DRAWLOG=<ordinal>[+<ordinal>...]: the draw's decoded vertices,
    // viewport and scissor (the batch/draw ordinals of [PM4_BATCH]).
    static const std::string s_drawLog = [] {
      const char* e = std::getenv("SR_PM4_DRAWLOG");
      return e ? "+" + std::string(e) + "+" : std::string();
    }();
    if (!s_drawLog.empty() &&
        s_drawLog.find("+" + std::to_string(c.drawOrdinal) + "+") != std::string::npos) {
      const float* v = reinterpret_cast<const float*>(m_xlatVBPtr + vbOffsetBytes);
      const uint32_t sf = strideOut / 4u;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_DRAWLOG] draw=%u prim=%u verts=%u->%u vsf=%d vp=(%.1f,%.1f %.1fx%.1f z%.3f-%.3f) "
                  "sc=(%ld,%ld)-(%ld,%ld) ndc=(%.4f,%.4f,%.4f,%.4f) vte=%08X clip=%08X sc_mode=%08X "
                  "vpS=(%.2f,%.2f,%.4f) vpO=(%.2f,%.2f,%.4f)",
                  c.drawOrdinal, c.primType, c.vertexCount, written, vsf ? 1 : 0, vp.TopLeftX,
                  vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth, long(pmScissor.left),
                  long(pmScissor.top), long(pmScissor.right), long(pmScissor.bottom), ndc[0], ndc[1],
                  ndc[2], ndc[3], c.vteCntl, c.clipCntl, c.suScModeCntl, c.vpScale[0], c.vpScale[1],
                  c.vpScale[2], c.vpOffset[0], c.vpOffset[1], c.vpOffset[2]);
      for (uint32_t i = 0; !vsf && i < written && i < 6u; i++)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_DRAWLOG]   v%u = (%.4f, %.4f, %.4f, %.4f)", i,
                    v[i * sf], v[i * sf + 1], v[i * sf + 2], v[i * sf + 3]);
      const float* vc = reinterpret_cast<const float*>(aluVs);
      for (uint32_t i = 0; i < 4u; i++)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_DRAWLOG]   vs c%u = (%.4f, %.4f, %.4f, %.4f)", i,
                    vc[i * 4], vc[i * 4 + 1], vc[i * 4 + 2], vc[i * 4 + 3]);
    }
    if (isYuvDraw || xln < 24 || (resolveDumpEnabled() && (xln % 50) == 0)) {
      const float* v0 = reinterpret_cast<const float*>(m_xlatVBPtr + vbOffsetBytes);
      // fix12b: a VSF draw decodes no vertices (vbOffsetBytes stays 0, `written` counts its indices) - the scratch
      // holds none for it, and under 7d(b)'s reservation that range may be uncommitted: nothing is read for it
      const uint32_t nv = vsf ? 0u : written;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[D3D12_XLAT]%s draw prim=%u verts=%u->%u attrs=%u vte=0x%X "
                  "vpS=(%.1f,%.1f,%.3f) vpO=(%.1f,%.1f,%.3f)",
                  isYuvDraw ? " YUV" : "", c.primType, c.vertexCount, written, nAttrs, c.vteCntl,
                  c.vpScale[0], c.vpScale[1], c.vpScale[2],
                  c.vpOffset[0], c.vpOffset[1], c.vpOffset[2]);
      if (isYuvDraw)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[D3D12_XLAT]   YUV id vs=%016llX layout=%zu vf95=%08X %08X "
                    "t0=%u@%08X t1=%u@%08X t2=%u@%08X colorInfo=%08X hdr=%d "
                    "progCntl=%08X ctxMisc=%08X",
                    (unsigned long long)c.vsUcodeHash, cvs->layout.size(),
                    c.fetchRaw[190], c.fetchRaw[191],
                    c.fetchRaw[0] & 3u, c.fetchRaw[1] & 0xFFFFF000u,
                    c.fetchRaw[6] & 3u, c.fetchRaw[7] & 0xFFFFF000u,
                    c.fetchRaw[12] & 3u, c.fetchRaw[13] & 0xFFFFF000u,
                    c.colorInfo, hdrTarget ? 1 : 0,
                    c.programCntl, c.contextMisc);
      if (isYuvDraw) {
        // iter-424e: RAW vertex-buffer bytes through the decoder's own window —
        // the v0 log prints zeros on draws that demonstrably rendered, so it is
        // not trustworthy; the memory is.
        const uint32_t vbPhys = c.fetchRaw[190] & ~3u;
        const uint8_t* vb = gpuPhysToHost(vbPhys);
        if (vb) {
          const uint32_t* w = reinterpret_cast<const uint32_t*>(vb);
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[D3D12_XLAT]   YUV vb@%08X raw: %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                      vbPhys, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
        } else {
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[D3D12_XLAT]   YUV vb@%08X UNMAPPED", vbPhys);
        }
        // iter-425b: the same phys through EVERY window — where do the quad's
        // bytes actually live, or were they never written at all?
        for (uint32_t win = 0x80000000u; win >= 0x80000000u && win <= 0xE0000000u; win += 0x20000000u) {
          const uint8_t* wp = sr::kernel::guestVAToHost(win + vbPhys);
          uint32_t nz = 0;
          if (wp) { for (int b = 0; b < 144; b++) if (wp[b]) nz++; }
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[D3D12_XLAT]   YUV vb window %02X: %s nz=%u/144",
                      win >> 24, wp ? "mapped" : "UNMAPPED", nz);
        }
        // iter-424d: the converter's colour matrix lives in the PS float
        // constants — all-zero constants paint BLACK on the black clear, which
        // would explain a healthy draw with an invisible result.
        const float* pf = reinterpret_cast<const float*>(aluPs);
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[D3D12_XLAT]   YUV psC c0=(%.3f,%.3f,%.3f,%.3f) c1=(%.3f,%.3f,%.3f,%.3f) "
                    "c2=(%.3f,%.3f,%.3f,%.3f)",
                    pf[0], pf[1], pf[2], pf[3], pf[4], pf[5], pf[6], pf[7],
                    pf[8], pf[9], pf[10], pf[11]);
      }
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[D3D12_XLAT]   state depth=0x%X blend=0x%X mask=0x%X mode=%u "
                  "v0=(%.2f,%.2f,%.3f,%.2f) v1=(%.2f,%.2f) v2=(%.2f,%.2f)",
                  c.depthControl, c.blendControl, c.colorMask, edramMode,
                  nv > 0 ? v0[0] : 0.0f, nv > 0 ? v0[1] : 0.0f, nv > 0 ? v0[2] : 0.0f, nv > 0 ? v0[3] : 0.0f,
                  nv > 1 ? v0[strideOut / 4] : 0.0f,
                  nv > 1 ? v0[strideOut / 4 + 1] : 0.0f,
                  nv > 2 ? v0[strideOut / 2] : 0.0f,
                  nv > 2 ? v0[strideOut / 2 + 1] : 0.0f);
    }
    return true;
  };

#if SR_ORACLE
  // Bisect gate: SR_XLAT_MAX=N renders only the first N translated draws and
  // DROPS the rest (no fixed-fn fallback) — the dump shows the composition up
  // to draw N, naming whichever draw floods the frame.
  static const long s_xlatMax = [] {
    const char* e = std::getenv("SR_XLAT_MAX");
    return e && e[0] ? atol(e) : -1;
  }();
  // SR_XLAT_ONLY=1: draws that can't translate are DROPPED instead of painted
  // by the fixed-function fallback (which whites out color-less vertices) —
  // shows the pure translated composition.
  static const bool s_xlatOnly = [] {
    const char* e = std::getenv("SR_XLAT_ONLY");
    return e && e[0] == '1';
  }();

#endif  // SR_ORACLE
  // iter-359i pass-geometry segment map (SR_SEG_LOG=1): one line whenever the
  // surface/window/viewport identity changes between consecutive draws — the
  // tile-2/helper-pass hunt reads pass boundaries from this.
  static const bool s_segLog = [] {
    const char* e = std::getenv("SR_SEG_LOG");
    return e && e[0] == '1';
  }();
  uint32_t drawSeq = 0;
  uint32_t lastSeg[6] = {0xFFFFFFFFu, 0, 0, 0, 0, 0};
  bool lastWasBoundary = false;  // iter-439d: did this drain END on a swap?
  do {
    const GuestDrawCommand& cmd = *cmdp;
    t_drainSeq.command();  // step 6 R6 (Astra r1 P3): a reuse needs the immediately previous command to have stored it
#if SR_ORACLE
    // NATIVE strand A executor (kernel/native_exec.hpp): an effect item ends the drain - the executor
    // performs it once every command ahead of it is drained (only executor mode pushes them).
    if (cmd.effectKind) break;
    if (sr::native::exec::onExecutorThread()) {
      if (cmd.execChk) sr::native::exec::chkAtExecute(cmd, queue.committed() - cmd.effectSeq);
      sr::native::exec::lagSpin();  // CHECK only: SR_NATIVE_EXEC_LAG_US per item (busy wait)
    }
#endif
    if (cmd.constTracked) cvMerge(cmd);  // iter-466 cp round 2: before any command is skipped
    if (cmd.frameBoundary) {
      m_framesDrained++;
      // iter-440c: publish consumption so the API Swap hook's guest block
      // (SR_API_PRESENT_BLOCK) can release — reference model backpressure.
      getDrawCommandQueue().notifyBoundaryConsumed();
      lastWasBoundary = true;
      // Stop at the last boundary that was available when this drain began —
      // later markers (and any partial frame behind them) wait for the next
      // drain, so the batch list always ends on a completed swap.
      if (!drainAll && m_framesDrained >= bAvail) break;
      continue;
    }
    lastWasBoundary = false;
    // iter-440c API clear: the guest's own Clear/ClearF at stream position.
    if (cmd.clearOp) {
      DrawBatch cb{};
      cb.clearOp = true;
      cb.clearFlags = cmd.clearFlags;
      std::memcpy(cb.clearColor, cmd.clearColor, sizeof(cb.clearColor));
      cb.clearZ = cmd.clearZ;
      cb.clearStencil = static_cast<uint8_t>(cmd.clearStencil & 0xFFu);
      m_drawBatches.push_back(cb);
      continue;
    }
    // p2.zpd (SR_NATIVE_ZPD): an occlusion-query marker at stream position; the render loop records the host
    // query there. A drained END makes the next EVENT_WRITE_SHD its covering-fence check.
    if (cmd.zpdOp) {
      zpdDrainMarker(cmd.zpdOp, cmd.zpdId);
      continue;
    }
    if (!cmd.hasDraw || cmd.vertexCount == 0) continue;
    if (s_segLog) {
      drawSeq++;
      const uint32_t seg[6] = {cmd.surfaceInfo, cmd.colorInfo, cmd.depthInfo,
                               cmd.windowScissorBR, cmd.modeControl & 7u,
                               uint32_t(int32_t(cmd.vpOffset[1]))};
      if (std::memcmp(seg, lastSeg, sizeof(seg)) != 0) {
        std::memcpy(lastSeg, seg, sizeof(seg));
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[SEG] draw#%u mode=%u surf=0x%08X color=0x%08X depth=0x%08X "
            "winScis=0x%08X/0x%08X vpS=(%.0f,%.0f) vpO=(%.0f,%.0f) vte=0x%X",
            drawSeq, cmd.modeControl & 7u, cmd.surfaceInfo, cmd.colorInfo,
            cmd.depthInfo, cmd.windowScissorTL, cmd.windowScissorBR,
            cmd.vpScale[0], cmd.vpScale[1],
            cmd.vpOffset[0], cmd.vpOffset[1], cmd.vteCntl);
      }
    }
#if SR_ORACLE
    // Bisect cap drops draws past N but NEVER mode-6 resolves — post pass
    // separation the front buffer only holds what the final resolve copies,
    // so dropping resolves would make every capped dump show stale content.
    if (s_xlatMax >= 0 && nXlat >= uint32_t(s_xlatMax) &&
        (cmd.modeControl & 7u) != 6u) continue;
#endif  // SR_ORACLE
    // iter-466 gfx: EDGE BLUR off leaves the game's edge blur draw out (gfx_options.hpp).
    // (Only the draw: the resolve after it still has that pixel shader bound.)
    if ((cmd.modeControl & 7u) != 6u && gfxopt::skipDraw(cmd.psUcodeHash)) continue;
    if (tryXlatDraw(cmd)) continue;  // real shaders; falls through when unavailable
#if SR_ORACLE
    if (s_xlatOnly) continue;
    nFixed++;
    const uint32_t stride = (cmd.hasState && cmd.stateData[1] >= 12 && cmd.stateData[1] <= 512)
                          ? cmd.stateData[1] : 0;
    if (!stride || !cmd.drawParams[4]) continue;

    // Position must be float3/float4 (everything the menu uses); refuse others.
    const bool posOk = cmd.posFormat == 0 ||
        cmd.posFormat == uint32_t(ucode::VertexFormat::k_32_32_32_FLOAT) ||
        cmd.posFormat == uint32_t(ucode::VertexFormat::k_32_32_32_32_FLOAT);
    if (!posOk) {
      if (s_skipLogN.fetch_add(1, std::memory_order_relaxed) < 8)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12_DRAIN] skip draw: posFmt=%u", cmd.posFormat);
      continue;
    }

    const uint8_t* vb = sr::kernel::guestVAToHost(0x80000000u + (cmd.drawParams[4] & 0x1FFFFFFFu));
    if (!vb) continue;
    const uint8_t* ib = nullptr;
    if (cmd.idxBasePhys)
      ib = gpuPhysToHost(cmd.idxBasePhys);

    // Topology: Xenos PrimitiveType (xenos.h:39-91).
    D3D_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool rectList = false;
    switch (cmd.primType) {
      case 4: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;   // kTriangleList
      case 6: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;  // kTriangleStrip
      case 8: rectList = true; break;                              // kRectangleList
      default: {
        // Points/lines/fans etc. — not menu content; count and skip.
        if (s_skipLogN.fetch_add(1, std::memory_order_relaxed) < 8)
          SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12_DRAIN] skip draw: prim=%u count=%u",
                      cmd.primType, cmd.vertexCount);
        continue;
      }
    }

    const uint32_t outCount = rectList ? (cmd.vertexCount / 3u) * 6u : cmd.vertexCount;
    if (outCount == 0 || vertexOffset + outCount > kMaxGuestVerts) continue;

    // Index lookup (Xenia: VGT_DMA_SIZE.swap_mode; k8in16 for u16, k8in32 for
    // u32 are the normal guest settings; only low 24 bits of an index count).
    auto readIndex = [&](uint32_t i) -> uint32_t {
      if (!ib) return i;
      if (cmd.idxFmt32) {
        uint32_t raw;
        std::memcpy(&raw, ib + size_t(i) * 4u, 4);
        if (cmd.idxEndian == 2) raw = _byteswap_ulong(raw);
        else if (cmd.idxEndian == 3) raw = (raw >> 16) | (raw << 16);
        return raw & 0xFFFFFFu;
      }
      uint16_t raw;
      std::memcpy(&raw, ib + size_t(i) * 2u, 2);
      if (cmd.idxEndian == 1) raw = _byteswap_ushort(raw);
      return raw;
    };

    // Fetch one guest vertex -> 7 host floats (pos3 + color4).
    const bool colF4 = cmd.colorFormat == uint32_t(ucode::VertexFormat::k_32_32_32_32_FLOAT);
    const bool colU8 = cmd.colorFormat == uint32_t(ucode::VertexFormat::k_8_8_8_8);
    auto fetchVertex = [&](uint32_t srcIdx, float* dst) {
      const uint8_t* src = vb + size_t(srcIdx) * stride;
      const uint8_t* pp = src + cmd.posOffsetBytes;
      for (int c = 0; c < 3; c++) {
        uint32_t raw = geoReadBE32(pp + c * 4);
        float f;
        std::memcpy(&f, &raw, 4);
        if (!std::isfinite(f)) f = 0.0f;
        dst[c] = f;
      }
      float col[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      if (colF4) {
        const uint8_t* cp = src + cmd.colorOffsetBytes;
        for (int c = 0; c < 4; c++) {
          uint32_t raw = geoReadBE32(cp + c * 4);
          float f;
          std::memcpy(&f, &raw, 4);
          col[c] = std::isfinite(f) ? f : 1.0f;
        }
      } else if (colU8) {
        // Guest-memory byte order for a BE 8_8_8_8 dword: [A][R][G][B].
        const uint8_t* cp = src + cmd.colorOffsetBytes;
        col[3] = cp[0] / 255.0f;
        col[0] = cp[1] / 255.0f;
        col[1] = cp[2] / 255.0f;
        col[2] = cp[3] / 255.0f;
      }
      dst[3] = col[0]; dst[4] = col[1]; dst[5] = col[2]; dst[6] = col[3];
    };

    float* out = mapped + size_t(vertexOffset) * kGeoVertexFloats;
    uint32_t written = 0;
    if (rectList) {
      // kRectangleList: 3 verts per rect; v3 = -v0 + v1 + v2 (Xenia's GS
      // mirrors position AND interpolators across the diagonal the same way).
      const uint32_t rects = cmd.vertexCount / 3u;
      for (uint32_t r = 0; r < rects; r++) {
        float v[3][kGeoVertexFloats];
        for (int k = 0; k < 3; k++) fetchVertex(readIndex(r * 3u + k), v[k]);
        float v3[kGeoVertexFloats];
        for (uint32_t c = 0; c < kGeoVertexFloats; c++) v3[c] = -v[0][c] + v[1][c] + v[2][c];
        std::memcpy(out + (written + 0) * kGeoVertexFloats, v[0], sizeof(v[0]));
        std::memcpy(out + (written + 1) * kGeoVertexFloats, v[1], sizeof(v[1]));
        std::memcpy(out + (written + 2) * kGeoVertexFloats, v[2], sizeof(v[2]));
        std::memcpy(out + (written + 3) * kGeoVertexFloats, v[2], sizeof(v[2]));
        std::memcpy(out + (written + 4) * kGeoVertexFloats, v[1], sizeof(v[1]));
        std::memcpy(out + (written + 5) * kGeoVertexFloats, v3, sizeof(v3));
        written += 6;
      }
    } else {
      for (uint32_t i = 0; i < cmd.vertexCount; i++, written++)
        fetchVertex(readIndex(i), out + size_t(written) * kGeoVertexFloats);
    }
    if (!written) continue;

    DrawBatch batch{};
    batch.vertexCount = written;
    batch.vertexOffset = vertexOffset;
    batch.flags = cmd.drawFlags;
    batch.topology = topo;
    // Root constants: vsx vsy vsz vox | voy voz vte rtW | rtH pad
    std::memcpy(&batch.consts[0], &cmd.vpScale[0], 12);
    std::memcpy(&batch.consts[3], &cmd.vpOffset[0], 12);
    batch.consts[6] = cmd.vteCntl;
    const float rtW = static_cast<float>(m_width), rtH = static_cast<float>(m_height);
    std::memcpy(&batch.consts[7], &rtW, 4);
    std::memcpy(&batch.consts[8], &rtH, 4);
    batch.consts[9] = 0;
    m_drawBatches.push_back(batch);
    vertexOffset += written;

    // iter-362: cap<24 only ever logged BOOT draws; the menu-render draws start
    // ~90s in. Sample late draws too (first 24 + every 50th) and add the window/
    // surface/scissor regs — the text draws' vpScale=0.5 (1x1 viewport collapse)
    // needs the pass surface dims (surfaceInfo pitch, screenScissorBR) + window
    // offset to tell "normalized viewport for a small surface" from a real bug.
    {
      const uint32_t dln = cpDiagBump(s_drawLogN);
      // iter-362: late sampling ONLY under the dump diagnostic — an ungated
      // every-50th log in the hot drain path throttled default runs (2fps) and
      // starved the async Apt load's timing window (0/24 deep builds).
      if (dln < 24 || (resolveDumpEnabled() && (dln % 50) == 0)) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[D3D12_DRAIN] draw prim=%u verts=%u->%u stride=%u %s%s vte=0x%X vp=(%.1f,%.1f|%.1f,%.1f) col=%u "
          "surf=0x%08X winOff=0x%08X winScisBR=0x%08X scrScisBR=0x%08X",
          cmd.primType, cmd.vertexCount, written, stride, ib ? "IDX" : "auto",
          cmd.idxFmt32 ? "32" : "16", cmd.vteCntl,
          cmd.vpScale[0], cmd.vpOffset[0], cmd.vpScale[1], cmd.vpOffset[1], cmd.colorFormat,
          cmd.surfaceInfo, cmd.windowOffset, cmd.windowScissorBR, cmd.screenScissorBR);
      }
    }
#else
    // The release never substitutes pos/color geometry or silently drops a
    // rejected translated draw. Keep the exact offending identity before exit.
    static const char* const reasons[] = {
      "pipeline-not-ready", "missing-vs", "missing-ps", "invalid-layout",
      "missing-pso", "unsupported-primitive", "vertex-staging-full",
      "invalid-fetch", "constant-staging-full", "zero-output-vertices",
      "layout-slot-out-of-range", "decoded-zero-vertices", "vsf-staging-failed"
    };
    const char* reason = nativeXlatReject >= 0 && nativeXlatReject < int(sizeof(reasons) / sizeof(reasons[0]))
        ? reasons[nativeXlatReject] : "unspecified-translation-rejection";
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[NATIVE_RECORDING] FAILED stage=untranslated-draw ordinal=%u vs=%016llX ps=%016llX "
        "reason=%s code=%d xlat=%s prim=%u mode=%u count=%u "
        "why=[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]",
        cmd.drawOrdinal, (unsigned long long)cmd.vsUcodeHash, (unsigned long long)cmd.psUcodeHash,
        reason, nativeXlatReject, nativeXlatState >= 0 ? dropStateName(nativeXlatState) : "n/a",
        cmd.primType, cmd.modeControl & 7u, cmd.vertexCount,
        xlatWhy[0], xlatWhy[1], xlatWhy[2], xlatWhy[3], xlatWhy[4],
        xlatWhy[5], xlatWhy[6], xlatWhy[7], xlatWhy[8], xlatWhy[9]);
    sr::Log::flush(2000);  // bounded: preserve the detailed rejection before _Exit
    nativeRecordingFail("untranslated-draw", E_FAIL);
#endif  // SR_ORACLE
  } while (queue.popFront(), (cmdp = queue.front()) != nullptr);
  {
    static const bool s_rewind = cpsimd::gateLogged("SR_CP_QUEUE_REWIND", true);
    if (s_rewind) queue.rewindIfEmptyOnProducer();
  }

  // iter-439d: markers exist but this drain didn't end on one — the frame is
  // split; the NEXT drain continues it (no clear, RT persists). Boot/replay
  // (no markers, bAvail==0) keep today's clear-every-drain behavior.
  m_nextDrainContinuesFrame = (bAvail > 0) && !lastWasBoundary;
  {
    static std::atomic<uint32_t> s_contLogN{0};
    if (m_nextDrainContinuesFrame) {
      const uint32_t cn = s_contLogN.fetch_add(1, std::memory_order_relaxed) + 1;
      if (cn <= 12 || cn % 500 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FRAME_CONT] drain ended MID-FRAME #%u (framesDrained=%llu avail=%llu) — next drain continues",
            cn, (unsigned long long)m_framesDrained, (unsigned long long)bAvail);
    }
  }

#if SR_ORACLE
  D3D12_RANGE wr{0, size_t(vertexOffset) * kGeoVertexStride};
  m_geoVertexBuffer->Unmap(0, &wr);
  m_totalGeoVerts = vertexOffset;
#endif  // SR_ORACLE
  shaderCache.flushVsMissReport();  // iter-463: this drain's dropped-draw census

  static std::atomic<uint32_t> s_drainLogN{0};
  // iter-440f: the first-8 gating hid the census from every post-boot era —
  // the ident-script red frames never printed their xlat/fixed split. Fire
  // time-based (every 5s) as well, like DRAIN_PROF.
  static std::atomic<int64_t> s_lastCensus{0};
  bool censusDue = s_drainLogN.fetch_add(1, std::memory_order_relaxed) < 8;
  {
    const int64_t nowQ = profQpc();
    int64_t last = s_lastCensus.load(std::memory_order_relaxed);
    if (nowQ - last > 5 * profQpcFreq() &&
        s_lastCensus.compare_exchange_strong(last, nowQ)) censusDue = true;
  }
  if (censusDue) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[D3D12_DRAIN] pipeline census: xlat=%u fixed=%u vbDedup=%u hdrDraws=%u hdrResolves=%u",
        nXlat, nFixed, nDedup, nHdrDraws, nHdrResolves);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[D3D12_DRAIN] fallback why: noVs=%u noPs=%u noLayout=%u noPso=%u prim=%u",
        xlatWhy[1], xlatWhy[2], xlatWhy[3], xlatWhy[4], xlatWhy[5]);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[D3D12_DRAIN] fallback why2: vbFull=%u fetchInv=%u cbFull=%u zeroV=%u",
        xlatWhy[6], xlatWhy[7], xlatWhy[8], xlatWhy[9]);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[D3D12_DRAIN] edram modes: 0=%u 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u",
        modeHist[0], modeHist[1], modeHist[2], modeHist[3],
        modeHist[4], modeHist[5], modeHist[6], modeHist[7]);
    char pbuf[160];
    int pn = 0;
    for (uint32_t p = 0; p < 32; p++)
      if (primDrop[p] && pn < int(sizeof(pbuf)) - 24)
        pn += snprintf(pbuf + pn, sizeof(pbuf) - pn, " %u:%u", p, primDrop[p]);
    if (pn)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12_DRAIN] prim drops (type:count):%s", pbuf);
  }
}

#if SR_ORACLE
void D3D12Backend::renderGuestGeometry() {
  // P1 (SR_PM4_RTCACHE=1): the EDRAM path owns the whole batch set - per-key
  // host RTs, ownership transfers and resolves through the EDRAM mirror. Its
  // resolves write guest memory like the console, so the frame reaches the
  // screen through the guest front buffer (m_frameDrewToLdr stays false).
  if (pm4RtCacheEnabled()) {
    m_frameDrewToLdr = false;
    gtFrameStart();  // iter-466 res4k: SR_PM4_GPU_TIME on the replay too (LOOPS = a GPU bench)
    renderPm4Batches(m_drawBatches, m_shmPending);
    gtFrameEnd();
    return;
  }
  // iter-439 P2: settle last drain's async resolve flushes whose fence has
  // passed (usually all of them by now — a whole present elapsed). Pure CPU;
  // the frame list stays untouched.
  completeInFlightResolves(false);
  // Draw into the offscreen guest RT (persistent RENDER_TARGET state).
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtvHandle.ptr += static_cast<SIZE_T>(kFrameCount) * m_rtvDescriptorSize;
  // iter-359m: HDR scene RTV (slot kFrameCount+1). curTarget tracks which is
  // bound (0=LDR, 1=HDR); a batch's hdrTarget switches it. All resolve helpers
  // that touch the DSV/RTV re-bind through bindTarget() so the state is one
  // place. The composite (LDR) samples the HDR scene snapshot via SRV.
  D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  hdrRtvHandle.ptr += static_cast<SIZE_T>(kFrameCount + 1) * m_rtvDescriptorSize;

  // iter-439d: a continuation drain resumes a frame the last drain left half
  // drawn — clearing here is what painted the half-screen seams and the
  // mid-movie solid flashes. Keep the RT/depth; the guest's own frame clears
  // arrived with the frame's FIRST drain. A continuation set also renders
  // exactly once (re-draw without clear would double-blend); re-presents of
  // an already-rendered set skip straight to the downstream copy.
  const bool cont = frameContEnabled() && m_thisDrainContinuesFrame;
  const bool skipDraws = cont && m_contBatchesRendered;

  // iter-440c (migration Phase 2, SR_API_CLEAR=1): the guest's own Clear/ClearF
  // commands arrive in the batch stream and execute at stream position — the
  // host stops inventing a frame clear (reference model: video.cpp hooks Clear;
  // the host NEVER clears on its own). Present-source bookkeeping still resets.
#if SR_ORACLE
  static const bool s_apiClear = [] {
    const char* e = std::getenv("SR_API_CLEAR");
    return e && e[0] == '1';
  }();
#else
  // SR_ORACLE=0: SR_API_CLEAR's producer (kernel/hle_api_observe.cpp) is compiled out, so it reads as unset here too
  // (this function's only caller, presentFramebuffer, is compiled out as well).
  constexpr bool s_apiClear = false;
#endif
  const float clearColor[4] = {0.0f, 0.0f, 0.02f, 1.0f};
  if (!cont) {
    if (!s_apiClear) {
      m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
      if (m_sceneHdrRT) {
        const float hdrClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_commandList->ClearRenderTargetView(hdrRtvHandle, hdrClear, 0, nullptr);
      }
    }
    m_frameDrewToLdr = false;  // iter-435d: present-source selector, see hpp
  }

  D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
  D3D12_RECT scissor{0, 0, m_width, m_height};
  m_commandList->RSSetScissorRects(1, &scissor);
  int curTarget = 0;  // 0 = LDR geo RT (default binding below)
  if (m_dsvHeap) {
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    if (!cont && !s_apiClear)
      m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH,
                                       m_hleDepthClear, 0, 0, nullptr);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsv);
  } else {
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
  }

  if (xlatReady()) {
    ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
    m_commandList->SetDescriptorHeaps(2, heaps);
  }

  // iter-359m: bind the LDR or HDR scene RT (shared DSV). Called on target
  // changes and after every resolve reopens the list.
  auto bindTarget = [&](int t) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = (t == 1) ? hdrRtvHandle : rtvHandle;
    if (m_dsvHeap) {
      D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
      m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    } else {
      m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    curTarget = t;
  };

  // Batches interleave the two pipelines in original draw order.
  int curKind = -1;  // 0 = fixed-function, 1 = translated
  const size_t lastBatch = m_drawBatches.empty() ? 0 : m_drawBatches.size() - 1;
  {
    static std::atomic<uint32_t> s_rgLogN{0};
    if (s_rgLogN.fetch_add(1, std::memory_order_relaxed) < 6) {
      uint32_t nRes = 0;
      for (auto& b : m_drawBatches) if (b.resolve) nRes++;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[XLAT_RENDER] batches=%zu resolves=%u",
                  m_drawBatches.size(), nRes);
    }
  }
  for (size_t bi = 0; !skipDraws && bi < m_drawBatches.size(); bi++) {
    auto& batch = m_drawBatches[bi];
    // iter-440c: the guest's own Clear/ClearF at stream position (SR_API_CLEAR).
    // Flags: 0xF = color targets 0-3 (clear the currently-bound scene RT; the
    // multi-RT distinction collapses onto LDR/HDR here), 0x10 = depth,
    // 0x20 = stencil (DSV is depth-only today — stencil bit is honored as a
    // depth-buffer clear participant and logged).
    if (batch.clearOp) {
      if (batch.clearFlags & 0xFu) {
        // iter-440d: the console has ONE EDRAM color surface; both our LDR and
        // HDR RTs model it, so a guest color clear applies to BOTH. Routing by
        // curTarget starved the HDR RT of clears entirely — it accumulated
        // blends forever and resolved as the pulsing solid RED that replaced
        // every post-movie era in fullrun440c (clears were black; the red was
        // saturation on the never-cleared float RT).
        m_commandList->ClearRenderTargetView(rtvHandle, batch.clearColor, 0, nullptr);
        if (m_sceneHdrRT)
          m_commandList->ClearRenderTargetView(hdrRtvHandle, batch.clearColor, 0, nullptr);
      }
      if ((batch.clearFlags & 0x30u) && m_dsvHeap) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH,
                                             batch.clearZ, 0, 0, nullptr);
      }
      static std::atomic<uint32_t> s_clrN{0};
      const uint32_t cn = s_clrN.fetch_add(1, std::memory_order_relaxed) + 1;
      if (cn <= 12 || cn % 2000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[API_CLEAR] exec #%u flags=0x%02X rgba=(%.2f,%.2f,%.2f,%.2f) z=%.2f target=%s",
                    cn, batch.clearFlags, batch.clearColor[0], batch.clearColor[1],
                    batch.clearColor[2], batch.clearColor[3], batch.clearZ,
                    curTarget == 1 ? "HDR" : "LDR");
      continue;
    }
    if (batch.resolve) {
#if SR_ORACLE
      // iter-359g: execute the console's EDRAM->guest resolve — read the RT
      // back into guest memory at the copy dest, evict cached textures built
      // from that range, then clear the RT/DSV for the next pass (except after
      // the frame's final resolve, whose content the present path shows).
      // iter-426c DIAGNOSTIC (SR_NO_RESOLVE_WB=1, replay-only use): skip the
      // EDRAM->guest writeback so trace-seeded memory stays pristine. The fmv
      // trace showed our own resolve overwriting Xenia's correct luma image at
      // 0x1E830000 with our broken point-pass output (white + dot grid) — this
      // gate separates "our sampling decode is wrong" from "our pass corrupted
      // the input". Eviction still runs, so textures re-read guest truth.
      static const bool s_noResolveWb = [] {
        const char* e = std::getenv("SR_NO_RESOLVE_WB");
        return e && e[0] == '1';
      }();
      uint8_t* dst = const_cast<uint8_t*>(gpuPhysToHost(batch.resolveDestPhys));
      if (dst && batch.resolvePitchTexels && !s_noResolveWb) {
        if (!rttEnabled()) {
          // iter-437c LIVE PATH: record the GPU copy in-stream at the
          // resolve's position; wait+convert deferred (flushPendingResolves).
          // The replay fixture (SR_XLAT_RTT=1) keeps the immediate path below
          // so RTT snapshots capture the RT at the resolve's stream position.
          if (recordPendingResolve(batch)) {
            // A buffer-full flush reset the frame list mid-drain; rebind the
            // caller's dead draw state (the 437c all-rolls crash: draws
            // recorded into a bare list -> host AV in the runtime).
            bindTarget(curTarget);
            curKind = -1;
          }
          continue;
        }
        // Dest texel size from RB_COPY_DEST_INFO format (k_16 = 2 bytes;
        // 8888 / k_32_FLOAT = 4) — row stride in guest RAM is pitch*texelSize.
        const uint32_t dfmt = (batch.resolveDestInfo >> 7) & 0x3Fu;
        const int texelBytes = (dfmt == 24u || dfmt == 25u || dfmt == 26u) ? 2 : 4;
        copyRenderTargetToGuestFramebuffer(dst, int(batch.resolvePitchTexels) * texelBytes,
                                           int(batch.resolveRows), batch.resolveDestInfo);
        // iter-408b: for resolves that target the surface we actually display,
        // measure what just got written. This separates "the render target was
        // empty" (problem is upstream, in the draws) from "we wrote content and
        // it is lost later" (problem is downstream, in present).
        if (m_guestFrontPhys &&
            (batch.resolveDestPhys & 0x1FFFFFFFu) == (m_guestFrontPhys & 0x1FFFFFFFu)) {
          const size_t span = size_t(batch.resolvePitchTexels) * texelBytes *
                              size_t(batch.resolveRows);
          // Count NON-BLACK TEXELS, not non-zero bytes. The first version of
          // this probe stepped one byte at a time and reported ~50% non-zero
          // for a render target holding nothing but the clear colour
          // (0,0,0.02,1 -> bytes 00 00 05 FF): the blue and alpha bytes alone
          // make half the buffer "non-zero". A metric that a blank surface
          // passes cannot tell you the surface has content.
          size_t nonBlack = 0, sampled = 0;
          for (size_t i = 0; i + 4 <= span; i += 4 * 251) {
            ++sampled;
            if (dst[i] > 8 || dst[i + 1] > 8 || dst[i + 2] > 8) nonBlack++;
          }
          static std::atomic<uint64_t> s_frontN{0};
          const uint64_t fn = s_frontN.fetch_add(1, std::memory_order_relaxed) + 1;
          if (fn <= 6 || fn % 100 == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[FRONT_RESOLVE] #%llu dest=0x%08X %ux%u nonBlackTexels=%zu/%zu",
                        (unsigned long long)fn, batch.resolveDestPhys,
                        batch.resolvePitchTexels, batch.resolveRows, nonBlack,
                        sampled);
          // iter-408c: when the resolve DID carry content, write it out. This is
          // the game's own composited output at the instant it left the render
          // target, and the bytes are already in host-readable guest memory, so
          // it costs nothing but the file. The back-buffer capture answers "what
          // did the monitor get"; this answers "what did the draws produce",
          // which is the rung the black screen actually sits on.
          static std::atomic<uint32_t> s_frontShots{0};
          if (nonBlack > 0 && s_frontShots.load(std::memory_order_relaxed) < 12) {
            const uint32_t shot = s_frontShots.fetch_add(1, std::memory_order_relaxed);
            if (shot < 12) {
              const uint32_t w = batch.resolvePitchTexels, h = batch.resolveRows;
              std::vector<uint8_t> bgra(size_t(w) * h * 4u);
              for (size_t t = 0; t < size_t(w) * h; ++t) {
                const uint8_t* sp = dst + t * 4;
                uint8_t* dp = bgra.data() + t * 4;
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
              }
              char nm[128];
              snprintf(nm, sizeof(nm), "composite_%02u_n%zu_%ux%u.bmp", shot, nonBlack, w, h);
              writeBmp32(nm, bgra.data(), w, h, w * 4);
              SR_LOG_BOOT(sr::LogSeverity::Info, "[COMPOSITE_DUMP] %s", nm);
            }
          }
        }
        const int64_t tEv0 = profQpc();
        uint32_t evicted = 0;
        const uint64_t lo = batch.resolveDestPhys;
        const uint64_t hi = lo + uint64_t(batch.resolvePitchTexels) * batch.resolveRows * texelBytes;
        std::vector<uint64_t> overlapKeys;  // step 4 G7 (SR_TEX_CACHE_EVICT): evicted after the scan
        for (auto& e : m_texEntries) {
          const uint32_t basePhys = e.basePhys & 0x1FFFFFFFu;
          if (basePhys < hi && uint64_t(basePhys) + e.bytes > lo &&
              (m_texEvictOn ? m_texCache.count(e.key) != 0 : m_texCache.erase(e.key) != 0)) {
            evicted++;
            if (m_texEvictOn) overlapKeys.push_back(e.key);
          }
        }
        for (uint64_t k : overlapKeys) texEvictKey(k, kTexEvResolve);
        texEvictFinish();
        profEvictUs.fetch_add(profUs(profQpc() - tEv0), std::memory_order_relaxed);
        static std::atomic<uint32_t> s_resLogN{0};
        if (s_resLogN.fetch_add(1, std::memory_order_relaxed) < 32) {
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[XLAT_RESOLVE] dest=0x%08X pitch=%u rows=%u evicted=%u %s",
                      batch.resolveDestPhys, batch.resolvePitchTexels, batch.resolveRows,
                      evicted, bi == lastBatch ? "(final, no clear)" : "");
        }
        // iter-359l: also snapshot the RT into a bindable texture at this dest,
        // so the tonemap/composite can sample the scene it just rendered (the
        // scene HDR color / depth read back in EDRAM formats the memory decoder
        // can't handle). Skips the frame's own final resolve (nothing samples
        // the front buffer within the frame). Gated (see rttEnabled).
        if (bi != lastBatch && rttEnabled()) {
          const int64_t tSn0 = profQpc();
          snapshotResolveTexture(batch.resolveDestPhys, batch.resolvePitchTexels,
                                 batch.resolveRows, batch.hdrTarget);
          profSnapshotUs.fetch_add(profUs(profQpc() - tSn0), std::memory_order_relaxed);
        }
        // copyRenderTargetToGuestFramebuffer executed + waited; reopen the list.
        // iter-359i: NO automatic RT/DSV clear here — EDRAM persists across
        // resolves on console (the helper pass interleaves ~440 draw+resolve
        // micro-cycles that ACCUMULATE via MIN blend); the game clears its
        // surfaces itself with explicit rect draws (z ALWAYS + mask 0xFFFF).
        // Our per-resolve clear was wiping that accumulation every cycle.
        clReset(GeoReset);  // iter-445 C1
        m_commandList->RSSetScissorRects(1, &scissor);
        if (xlatReady()) {
          ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
          m_commandList->SetDescriptorHeaps(2, heaps);
        }
        bindTarget(curTarget);  // rebind whichever surface was active
        curKind = -1;  // force full state rebind for the next pass's draws
      } else {
        static std::atomic<uint32_t> s_resSkipN{0};
        if (s_resSkipN.fetch_add(1, std::memory_order_relaxed) < 8)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[XLAT_RESOLVE] SKIP dest=0x%08X pitch=%u dst=%s",
                      batch.resolveDestPhys, batch.resolvePitchTexels,
                      dst ? "ok" : "null");
      }
      continue;
#else
      // Native resolves execute in the frame renderer with their captured
      // host targets. This legacy readback batch has no release consumer.
      nativeRecordingFail("legacy-resolve-batch", E_INVALIDARG);
#endif
    }
    if (batch.xlat) {
      // iter-359m: route to the HDR scene surface or the LDR RT (the PSO's
      // RTV format was keyed to match). A mismatch is a D3D12 error.
      const int wantTarget = batch.hdrTarget ? 1 : 0;
      if (wantTarget != curTarget) { bindTarget(wantTarget); curKind = -1; }
      // iter-435e: "any LDR draw" was too coarse a present-source signal —
      // UI frames carry stray LDR utility quads (alpha-0 scrims) that kept
      // the navy RT on screen (the user's black-after-intros report, twice).
      // The one content class PROVEN to present live from the RT is the YUV
      // movie composite; everything else reaches the eye via resolves into
      // the guest front buffer.
      if (batch.psHash == 0x20A5E5204FD23BE2ull) m_frameDrewToLdr = true;
      // iter-359j: bind textures NOW — this frame's earlier resolves already
      // rewrote guest memory + evicted overlapping cache entries, so creation
      // here sees post-resolve content (same-frame resolve->texture sampling,
      // the console contract). The CB region is consumed at the next flush,
      // which is always after this write.
      {
        uint32_t* sharedTex = reinterpret_cast<uint32_t*>(m_xlatCBPtr + batch.sharedCbOff);
        // iter-411: SharedConstants layout (XenosRecomp shader_recompiler.cpp
        // :1290-1294) is c0..c3 Texture2D indices, c4..c7 Texture3D, c8..c11
        // TextureCube, c12..c15 SAMPLER indices — i.e. dwords 0..15 and 48..63.
        // Only the first block was ever written; the shader loads its sampler
        // index from c12 (confirmed in the DXIL: cbufferLoadLegacy regIndex 12).
        uint32_t* sharedSmp = sharedTex + 48;
        bool anyTex = false, anyWhite = false;
        char sb[192]; int sn = 0;
        m_texPinnedN = 0;  // fix3 (Astra 0a384c24 P3): this path's draw pins its own selections, then lets go
        struct PinScope {
          size_t& n;
          ~PinScope() { n = 0; }
        } pinScope{m_texPinnedN};
        for (uint32_t t = 0; t < 16; t++) {
          const uint32_t* tf = &batch.texFetch[t * 6];
          // isTexture, NOT "is2D": see the FetchConstantType note above.
          const bool is2D = (tf[0] & 3u) == 2u;
          // iter-437c: sampling a pending-resolve dest? Flush first so the
          // texture create below reads post-resolve guest bytes (console
          // same-frame resolve->sample contract). Conservative extent: 8B a
          // texel covers every dest format incl. mips.
          if (is2D && (!m_pendingResolves.empty() || !m_inFlightResolves.empty())) {
            const uint64_t tb =
                (uint64_t((tf[1] >> 12) & 0xFFFFFu) * 4096u) & 0x1FFFFFFFu;
            // iter-438d: a bind served by the GPU-side resolve texture needs
            // NO flush — the in-stream snapshot copy is already ordered after
            // the resolve inside the open list. Only binds that will fall to
            // the guest-memory decode path still need post-resolve bytes.
            const bool servedBySnap =
                resolveTexSnapEnabled() && findResolveTexSlot(uint32_t(tb)) >= 0;
            const uint32_t tw = (tf[2] & 0x1FFFu) + 1u;
            const uint32_t th = ((tf[2] >> 13) & 0x1FFFu) + 1u;
            // iter-439e: a bind whose conservative extent is absurd (roll E:
            // one phantom 6208x7696 bind = 380MB span) overlap-matched EVERY
            // pending resolve and forced 45K sync flushes/run — attract fell
            // to 0.2 presents/s. Same-frame resolve consumers are <=720p
            // surfaces; a claimed span >32MB cannot be one — skip the flush.
            const uint64_t bindSpan = uint64_t(tw) * th * 8u;
            if (bindSpan > (32ull << 20)) {
              static std::atomic<uint32_t> s_phantomN{0};
              const uint32_t pn2 = s_phantomN.fetch_add(1, std::memory_order_relaxed) + 1;
              if (pn2 <= 4 || pn2 % 5000 == 0)
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[BIND_OVERLAP] #%u PHANTOM-EXTENT skip tb=0x%08X %ux%u "
                    "tf2=0x%08X (span %lluMB > 32MB cap)",
                    pn2, uint32_t(tb), tw, th, tf[2],
                    (unsigned long long)(bindSpan >> 20));
            } else
            if (!servedBySnap &&
                pendingResolveOverlaps(tb, tb + bindSpan)) {
              profBindFlushN.fetch_add(1, std::memory_order_relaxed);
              // iter-439 P2 diag: WHY did this bind miss the texsnap fast
              // path? Distinguish "fetch base != resolve dest base" (snap
              // exists at the dest, keyed differently) from "dest never
              // snapped". This names the fix that would delete the remaining
              // sync flushes (8K/run in the ident era, each a full GPU wait).
              {
                static std::atomic<uint32_t> s_bofN{0};
                const uint32_t bn = s_bofN.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bn <= 20 || bn % 500 == 0) {
                  uint32_t pd = 0;
                  for (const auto& p : m_pendingResolves)
                    if (!p.dead) { pd = p.destPhys; break; }
                  SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[BIND_OVERLAP] #%u tb=0x%08X %ux%u pendDest=0x%08X "
                      "snapAtDest=%d snapCount=%zu",
                      bn, uint32_t(tb), tw, th, pd,
                      pd ? (findResolveTexSlot(pd) >= 0 ? 1 : 0) : -1,
                      m_resolveTextures.size());
                }
              }
              // iter-439 P2: the overlap may live in an async-flushed batch —
              // settle those first (fence wait + convert; no list reset), then
              // sync-flush anything still recorded in the open list.
              completeInFlightResolves(true);
              if (flushPendingResolves("bind-overlap")) {
                bindTarget(curTarget);
                curKind = -1;
              }
            }
          }
          if (is2D) {
            const int64_t tB0 = profQpc();
            sharedTex[t] = glyphRemap(getOrCreateTexture2D(tf), tf);  // iter-464 prompts
            profTexBindUs.fetch_add(profUs(profQpc() - tB0),
                                    std::memory_order_relaxed);
          } else {
            sharedTex[t] = 0u;
          }
          // Fetch-constant sampler state (Xenia xenos.h): dword0 bits 10-12 =
          // clamp_x, 13-15 = clamp_y (ClampMode: 0 repeat, 1 mirrored-repeat,
          // everything else is a clamp family); dword3 bits 19-20 = mag_filter,
          // 21-22 = min_filter (0 = point, 1 = linear, 3 = "use fetch default").
          {
            // SR_XLAT_SAMPLERS: 0 = pre-iter-411 (always index 0 = linear+wrap),
            // 1 = honour CLAMP only (filtering forced linear), 2 = clamp+filter.
            // Split because the two halves rest on different evidence: the clamp
            // bits are confirmed (xenos.h:1251-1252) and the filter bits are
            // confirmed (xenos.h:1301, dword_3 +19), but only measurement says
            // whether either actually helps, and they must not be judged as one.
            static const int s_mode = [] {
              const char* e = std::getenv("SR_XLAT_SAMPLERS");
              return e ? atoi(e) : 2;
            }();
            const uint32_t clampX = (tf[0] >> 10) & 7u;
            const uint32_t clampY = (tf[0] >> 13) & 7u;
            const uint32_t magF = (tf[3] >> 19) & 3u;
            const bool clampU = s_mode >= 1 && clampX >= 2u;
            const bool clampV = s_mode >= 1 && clampY >= 2u;
            const bool linear = (s_mode < 2) || (magF != 0u);  // 3 = default -> linear
            // Mode 0 must reproduce the PRE-iter-411 binary exactly, and that
            // means index 1 (linear + wrap), not index 0 — index 0 in the new
            // table is POINT + wrap. An "off" switch that does not restore the
            // old behaviour makes every A/B against it meaningless; this one is
            // verified by hash against the pre-change replay output.
            sharedSmp[t] = s_mode == 0 ? 1u
                                       : ((clampU ? 4u : 0u) | (clampV ? 2u : 0u) |
                                          (linear ? 1u : 0u));
          }
          if (is2D) {
            anyTex = true;
            if (!sharedTex[t]) anyWhite = true;
            if (sn < int(sizeof(sb)) - 32)
              sn += snprintf(sb + sn, sizeof(sb) - sn, " t%u:base=0x%08X->slot%u",
                             t, ((tf[1] >> 12) & 0xFFFFFu) * 4096u, sharedTex[t]);
          }
        }
        // iter-408c: a draw whose textures all resolve to slot 0 samples the
        // white 1x1 fallback, which renders as a FLAT fill - exactly what the
        // composite dumps show. Log the resolved slots so "the shader is wrong"
        // and "the shader sampled white" stop being indistinguishable.
        if (anyTex) {
          static std::atomic<uint32_t> s_bindLog{0};
          const uint32_t bn = s_bindLog.fetch_add(1, std::memory_order_relaxed) + 1;
          if (anyWhite ? (bn <= 60 || bn % 500 == 0) : (bn <= 20))
            SR_LOG_BOOT(sr::LogSeverity::Info, "[TEX_BIND] ps=%016llX vs=%016llX%s%s",
                        (unsigned long long)batch.psHash, (unsigned long long)batch.vsHash, sb,
                        anyWhite ? "  <-- HAS WHITE FALLBACK" : "");
          // iter-417: [TEX_BIND] only shows slots that RESOLVED to a 2D texture,
          // so a slot holding a non-2D or empty fetch is invisible — which is
          // how "the movie binds only t1/t2" was left ambiguous between "the
          // luma is missing" and "the luma is in a slot we never print". Dump
          // the raw fetch words for the low slots on the first few draws, with
          // the decoded dim/fmt/size, so the question is answered by data.
          // iter-420: the YUV->RGB pixel shader is 20A5E5204FD23BE2 (the only
          // one in the cache that does the 3-sample + colour-matrix sequence).
          // ALWAYS dump its fetch slots, and keep a small general sample for
          // everything else. iter-417 sampled only the first 6 bind events and
          // therefore dumped the frame's opening draws, not the movie draw.
          static std::atomic<uint32_t> s_rawLog{0};
          const bool isYuvPs = (batch.psHash == 0x20A5E5204FD23BE2ull);
          static std::atomic<uint32_t> s_yuvLog{0};
          const bool dumpThis =
              isYuvPs ? (s_yuvLog.fetch_add(1, std::memory_order_relaxed) < 4)
                      : (s_rawLog.fetch_add(1, std::memory_order_relaxed) < 6);
          if (dumpThis) {
            SR_LOG_BOOT(sr::LogSeverity::Info, "[FETCH_RAW] draw ps=%016llX%s",
                        (unsigned long long)batch.psHash, isYuvPs ? "  <== YUV SHADER" : "");
            for (uint32_t t = 0; t < 6; t++) {
              const uint32_t* tf = &batch.texFetch[t * 6];
              if (!tf[0] && !tf[1] && !tf[2]) {
                SR_LOG_BOOT(sr::LogSeverity::Info, "[FETCH_RAW]   t%u: <empty>", t);
                continue;
              }
              SR_LOG_BOOT(sr::LogSeverity::Info,
                          "[FETCH_RAW]   t%u: type=%u(%s) fmt=%u %ux%u base=0x%08X "
                          "tiled=%u f=%08X %08X %08X",
                          t, tf[0] & 3u, xeFetchTypeName(tf[0] & 3u), tf[1] & 0x3Fu,
                          (tf[2] & 0x1FFFu) + 1u, ((tf[2] >> 13) & 0x1FFFu) + 1u,
                          ((tf[1] >> 12) & 0xFFFFFu) * 4096u, (tf[0] >> 31) & 1u,
                          tf[0], tf[1], tf[2]);
            }
          }
        }
      }
      if (curKind != 1) {
        curKind = 1;
        xlatBindRootTables();  // native immutable sampler heap, complete table restore
      }
      m_commandList->SetPipelineState(batch.xlatPso);
      m_commandList->RSSetViewports(1, &batch.xlatViewport);
      bindXlatConstants(batch);  // texture indices are final; submission owns these bytes
      bindXlatGeometry(batch);  // checked owned vertex bytes; no mutable ring binding
      m_commandList->IASetPrimitiveTopology(batch.topology);
      m_commandList->DrawInstanced(batch.vertexCount, 1, 0, 0);
    } else {
      // Fixed-function PSO is RGBA8 — always the LDR target. Early-boot-only
      // path (pre-xlat): keep RT presenting there (no guest fb exists yet).
      if (curTarget != 0) { bindTarget(0); curKind = -1; }
      m_frameDrewToLdr = true;
      if (curKind != 0) {
        curKind = 0;
        m_commandList->SetPipelineState(m_geoPSO.Get());
        m_commandList->SetGraphicsRootSignature(m_geoRootSig.Get());
        m_commandList->RSSetViewports(1, &vp);
        m_commandList->IASetVertexBuffers(0, 1, &m_geoVBView);
      }
      m_commandList->SetGraphicsRoot32BitConstants(0, 10, batch.consts, 0);
      m_commandList->IASetPrimitiveTopology(batch.topology);
      m_commandList->DrawInstanced(batch.vertexCount, 1, batch.vertexOffset, 0);
    }
  }

  // iter-439d: the continuation set is now on the RT — later presents of the
  // same set must not draw it again (no clear + re-draw = double blend).
  if (cont) m_contBatchesRendered = true;

  // iter-437c: end of drain — everything downstream (backbuffer copy, legacy
  // fb copy, present blit) reads the RT or guest memory; submit all pending
  // resolves now. Reopens the frame list; the copy below records into it.
  // iter-439 P2: allowAsync — the guest write-back defers to the next drain's
  // fence poll instead of a full waitForGpu here (the queue-full wedge).
  flushPendingResolves("drain-end", /*allowAsync=*/true);

  // Show the guest RT in the window: copy into the current backbuffer.
  // iter-439f: only COMPLETE frames reach the screen. m_nextDrainContinuesFrame
  // (set by drainDrawQueue just before this render) says whether the CURRENT
  // batch set ended at a VdSwap boundary. Complete → present the RT and
  // snapshot it; mid-frame → present the snapshot (the last complete frame),
  // never the half-drawn RT. Console parity: scanout never shows mid-frame
  // EDRAM state. SR_FRAME_CONT=0 disables (with the continuation model).
  auto* bb = m_renderTargets[m_frameIndex].Get();
  const bool frameComplete =
      !(presentCompleteEnabled() && m_nextDrainContinuesFrame);
  if (presentCompleteEnabled() && !m_lastGoodRT && m_geoRT) {
    D3D12_RESOURCE_DESC lgd = m_geoRT->GetDesc();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &lgd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&m_lastGoodRT))))
      m_lastGoodRT.Reset();
    m_lastGoodRTValid = false;
  }
  if (frameComplete || !m_lastGoodRT || !m_lastGoodRTValid) {
    resourceBarrier(m_commandList.Get(), m_geoRT.Get(),
                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    resourceBarrier(m_commandList.Get(), bb,
                    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    m_commandList->CopyResource(bb, m_geoRT.Get());
    if (frameComplete && m_lastGoodRT) {
      // Snapshot the completed frame (lastGood rests in COPY_DEST).
      m_commandList->CopyResource(m_lastGoodRT.Get(), m_geoRT.Get());
      m_lastGoodRTValid = true;
    }
    resourceBarrier(m_commandList.Get(), m_geoRT.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    resourceBarrier(m_commandList.Get(), bb,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  } else {
    // Mid-frame: re-present the last complete frame.
    resourceBarrier(m_commandList.Get(), m_lastGoodRT.Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    resourceBarrier(m_commandList.Get(), bb,
                    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    m_commandList->CopyResource(bb, m_lastGoodRT.Get());
    resourceBarrier(m_commandList.Get(), m_lastGoodRT.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    resourceBarrier(m_commandList.Get(), bb,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  }
}

// Bytes per texel of a resolve DESTINATION, keyed by RB_COPY_DEST_INFO's
// copy_dest_format (a xenos::ColorFormat, Xenia registers.h:876 — same
// numbering as TextureFormat). The old expression was
// `(f==24||f==25||f==26) ? 2 : 4`, which is right only for k_16: k_16_16 is 4
// bytes and k_16_16_16_16 is 8, and the whole 16-bit FLOAT family was missing.
// Getting this wrong writes the surface at the wrong stride, which shears it.
static int resolveDestTexelBytes(uint32_t dFmt) {
  switch (dFmt) {
    case 24: case 30:            return 2;  // k_16, k_16_FLOAT
    case 26: case 29: case 32:   return 8;  // k_16_16_16_16 / _EXPAND / _FLOAT
    default:                     return 4;  // 8888, k_16_16, k_16_16_FLOAT, k_32_FLOAT
  }
}

// f32 -> IEEE half. Round-to-nearest-even is overkill here (the source is 8-bit
// quantised already), so this truncates the mantissa.
static uint16_t floatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exp = int32_t((x >> 23) & 0xFFu) - 127 + 15;
  const uint32_t mant = x & 0x7FFFFFu;
  if (exp <= 0) return uint16_t(sign);                       // underflow -> +-0
  if (exp >= 31) return uint16_t(sign | 0x7C00u);            // overflow -> +-inf
  return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
}

// The console's resolve writes its destination in the TILED layout that the
// texture fetch constant then describes (Xenia draw_util.cc:1183 offsets
// copy_dest_base through texture_address::Tiled2D), while ours writes linear
// rows — so a surface that is later SAMPLED gets untiled by the texture path
// and comes out scrambled.
//
// DEFAULT OFF — measured, not assumed. Turning it on globally is WORSE: the
// title screen's logo/text/letterbox (which render correctly today) collapse
// into horizontal-striped mush, while the background stays scrambled. So the
// surfaces feeding the present/UI path are not consumed as tiled the way the
// scene-composite source is, and one global switch cannot be right for both.
// Kept gated so the per-surface version can be built on it; do not flip this
// default without an A/B on the replay fixture (tools/replay_ab.ps1).
static bool resolveTiledEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_RESOLVE_TILED");
    return e && e[0] == '1';
  }();
  return v;
}

// DIAGNOSTIC (default off): skip the EDRAM->guest write-back entirely.
//
// Under the trace replay the .xtr prefills guest memory with Xenia's OWN resolve
// output, in correct console-tiled layout. Our linear write-back lands on the
// same addresses and clobbers it before the texture cache reads them. Turning
// the write-back off leaves the golden content intact, which isolates "is our
// texture DECODE correct?" from "is our resolve WRITE correct?" — two bugs that
// otherwise alias into one scrambled image.
static bool resolveNoWriteEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_RESOLVE_NOWRITE");
    return e && e[0] == '1';
  }();
  return v;
}

// iter-437c: the RGBA8-readback -> guest-memory convert, extracted verbatim
// from copyRenderTargetToGuestFramebuffer so the deferred-resolve flush path
// (flushPendingResolves) shares one implementation. `src` points at the first
// row of this resolve's region in a mapped readback buffer.
static void convertResolveToGuest(uint8_t* hostDest, int destPitchBytes,
                                  uint32_t destInfo, const uint8_t* src0,
                                  UINT srcPitch, int copyW, int copyH) {
  const uint32_t dFmt = (destInfo >> 7) & 0x3Fu;
  const uint32_t dEndian = destInfo & 7u;
  const bool dSwap = ((destInfo >> 24) & 1u) != 0;
  const int texelBytes = resolveDestTexelBytes(dFmt);

  // iter-358h PLATFORM FIX: hostDest is the GUEST framebuffer — never write
  // past the guest allocation (a host AV is fatal under the harness VEH).
  size_t availBytes = 0;
  {
    uint8_t* gbase = nullptr; size_t gsz = 0;
    sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
    const uint8_t* gb = gbase;
    if (!gb || !gsz || destPitchBytes <= 0 || copyW <= 0 ||
        hostDest < gb || hostDest >= gb + gsz) {
      static std::atomic<uint32_t> s_n{0};
      if (s_n.fetch_add(1, std::memory_order_relaxed) < 4)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[D3D12] resolve dest OUT OF RANGE — skipping copy (dst=%p guest=[%p,%p) pitch=%d)",
          (const void*)hostDest, (const void*)gb, (const void*)(gb + gsz), destPitchBytes);
      return;
    }
    const size_t avail = static_cast<size_t>((gb + gsz) - hostDest);
    availBytes = avail;
    const size_t rowBytes = static_cast<size_t>(copyW) * static_cast<size_t>(texelBytes);
    int maxRows = 0;
    if (avail >= rowBytes)
      maxRows = static_cast<int>((avail - rowBytes) / static_cast<size_t>(destPitchBytes)) + 1;
    if (copyH > maxRows) {
      static std::atomic<uint32_t> s_n2{0};
      if (s_n2.fetch_add(1, std::memory_order_relaxed) < 4)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[D3D12] resolve would overrun guest memory — clamping %d -> %d rows",
          copyH, maxRows);
      copyH = maxRows;
    }
    if (copyH <= 0) return;
  }

  // iter-359h: honor the console's resolve dest contract (Xenia resolve
  // shaders — recon wf_161cf206): pack R|G<<8|B<<16|A<<24 little-endian,
  // red/blue-swap first when copy_dest_swap is set, then apply the endian128
  // byte permutation, and convert to the dest FORMAT. The front buffer uses
  // fmt 6 + swap=1 + endian none = bytes B,G,R,A ascending (LE_X8R8G8B8).
  static std::atomic<uint32_t> s_fmtWarnN{0};
  if (dFmt != 6u && dFmt != 24u && dFmt != 25u && dFmt != 26u && dFmt != 29u &&
      dFmt != 30u && dFmt != 31u && dFmt != 32u && dFmt != 36u && dFmt != 50u &&
      destInfo != 0 &&
      s_fmtWarnN.fetch_add(1, std::memory_order_relaxed) < 8)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[XLAT_RESOLVE] unhandled dest format %u (destInfo=0x%08X) — writing as 8888",
                dFmt, destInfo);

  if (resolveDumpEnabled()) {
    // Dump WHAT THE PASS RENDERED (the RT readback, RGBA) as viewable BGRA.
    static std::atomic<uint32_t> s_resDumpN{0};
    const uint32_t rn = s_resDumpN.fetch_add(1, std::memory_order_relaxed);
    if (rn < 32 || (rn % 50) == 0) {
      std::vector<uint8_t> bgra(size_t(copyW) * copyH * 4u);
      for (int y = 0; y < copyH; y++) {
        const uint8_t* sp = src0 + size_t(y) * srcPitch;
        uint8_t* dp = bgra.data() + size_t(y) * copyW * 4;
        for (int x = 0; x < copyW; x++) {
          dp[x * 4 + 0] = sp[x * 4 + 2]; dp[x * 4 + 1] = sp[x * 4 + 1];
          dp[x * 4 + 2] = sp[x * 4 + 0]; dp[x * 4 + 3] = sp[x * 4 + 3];
        }
      }
      char nm[160];
      const char* inst = std::getenv("SR_INSTANCE_ID");
      snprintf(nm, sizeof(nm), "resolvedump_%s_%05u_info0x%08X_%dx%d.bmp",
               inst ? inst : "solo", rn, destInfo, copyW, copyH);
      writeBmp32(nm, bgra.data(), uint32_t(copyW), uint32_t(copyH), uint32_t(copyW) * 4);
    }
  }
  const uint32_t pitchTexels = uint32_t(destPitchBytes / texelBytes);
  const uint32_t pitchAligned = (pitchTexels + 31u) & ~31u;
  unsigned bpbLog2 = 0;
  while ((1u << bpbLog2) < uint32_t(texelBytes)) bpbLog2++;
  const bool tiledWrite = resolveTiledEnabled();
  uint32_t oob = 0;

  // Byte offset of one destination texel. Every write is bounds-checked: the
  // harness VEH turns a host AV into a fatal before any SEH filter runs, so a
  // tiled address that lands past the guest allocation must be dropped, never
  // faulted on.
  auto destTexel = [&](int x, int y) -> uint8_t* {
    size_t off;
    if (tiledWrite) {
      const int32_t t = xenos_tiling::tiled2D(x, y, pitchAligned, bpbLog2);
      if (t < 0) { oob++; return nullptr; }
      off = size_t(t);
    } else {
      off = size_t(y) * size_t(destPitchBytes) + size_t(x) * size_t(texelBytes);
    }
    if (off + size_t(texelBytes) > availBytes) { oob++; return nullptr; }
    return hostDest + off;
  };

  for (int y = 0; y < copyH; y++) {
    const uint8_t* src = src0 + size_t(y) * srcPitch;
    for (int x = 0; x < copyW; x++) {
      uint8_t* dst = destTexel(x, y);
      if (!dst) continue;
      const uint8_t r = src[x * 4 + 0], g = src[x * 4 + 1],
                    b = src[x * 4 + 2], a = src[x * 4 + 3];
      if (texelBytes == 2) {
        // k_16 / k_16_FLOAT: scalar from the RT red channel (the helper passes
        // render scalar data). endian 8in16 = byteswap each u16.
        uint16_t v = (dFmt == 30u) ? floatToHalf(float(r) * (1.0f / 255.0f))
                                   : uint16_t(uint32_t(r) * 257u);
        if (dEndian == 1u || dEndian == 2u) v = uint16_t((v >> 8) | (v << 8));
        std::memcpy(dst, &v, 2);
      } else if (texelBytes == 8) {
        // 4x16 per channel — this is the HDR scene surface. The _FLOAT form
        // stores halves; the integer and _EXPAND forms store 16-bit normalised.
        uint16_t c[4];
        if (dFmt == 32u) {
          c[0] = floatToHalf(float(r) * (1.0f / 255.0f));
          c[1] = floatToHalf(float(g) * (1.0f / 255.0f));
          c[2] = floatToHalf(float(b) * (1.0f / 255.0f));
          c[3] = floatToHalf(float(a) * (1.0f / 255.0f));
        } else {
          c[0] = uint16_t(uint32_t(r) * 257u); c[1] = uint16_t(uint32_t(g) * 257u);
          c[2] = uint16_t(uint32_t(b) * 257u); c[3] = uint16_t(uint32_t(a) * 257u);
        }
        if (dEndian == 1u || dEndian == 2u)
          for (int i = 0; i < 4; i++) c[i] = uint16_t((c[i] >> 8) | (c[i] << 8));
        std::memcpy(dst, c, 8);
      } else if (dFmt == 36u) {
        // k_32_FLOAT: scalar float from the RT red channel (luminance chain).
        float f = float(r) * (1.0f / 255.0f);
        uint32_t w;
        std::memcpy(&w, &f, 4);
        if (dEndian == 2u) w = _byteswap_ulong(w);
        else if (dEndian == 1u) w = ((w & 0xFF00FF00u) >> 8) | ((w & 0x00FF00FFu) << 8);
        else if (dEndian == 3u) w = (w >> 16) | (w << 16);
        std::memcpy(dst, &w, 4);
      } else if (dFmt == 25u || dFmt == 31u) {
        uint16_t c[2];
        if (dFmt == 31u) {
          c[0] = floatToHalf(float(r) * (1.0f / 255.0f));
          c[1] = floatToHalf(float(g) * (1.0f / 255.0f));
        } else {
          c[0] = uint16_t(uint32_t(r) * 257u); c[1] = uint16_t(uint32_t(g) * 257u);
        }
        if (dEndian == 1u || dEndian == 2u)
          for (int i = 0; i < 2; i++) c[i] = uint16_t((c[i] >> 8) | (c[i] << 8));
        std::memcpy(dst, c, 4);
      } else {
        // 8888 (and legacy destInfo==0): RGBA bytes, optional R/B swap, then
        // the endian128 permutation.
        uint8_t rr = r, gg = g, bb = b, aa = a;
        if (dSwap) { uint8_t t = rr; rr = bb; bb = t; }
        uint8_t o0 = rr, o1 = gg, o2 = bb, o3 = aa;                    // endian none
        if (dEndian == 1u) { o0 = gg; o1 = rr; o2 = aa; o3 = bb; }     // 8in16
        else if (dEndian == 2u) { o0 = aa; o1 = bb; o2 = gg; o3 = rr; }  // 8in32
        else if (dEndian == 3u) { o0 = bb; o1 = aa; o2 = rr; o3 = gg; }  // 16in32
        dst[0] = o0; dst[1] = o1; dst[2] = o2; dst[3] = o3;
      }
    }
  }
  if (oob) {
    static std::atomic<uint32_t> s_oobN{0};
    if (s_oobN.fetch_add(1, std::memory_order_relaxed) < 6)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[XLAT_RESOLVE] %u texels fell outside guest memory (tiled=%u "
                  "pitch=%u texelBytes=%d) — dropped",
                  oob, unsigned(tiledWrite), pitchAligned, texelBytes);
  }
}

void D3D12Backend::copyRenderTargetToGuestFramebuffer(uint8_t* hostDest, int destPitchBytes,
                                                      int maxRows, uint32_t destInfo) {
  if (!hostDest || destPitchBytes <= 0 || !m_readbackBuffer || !m_geoRT) return;
  if (resolveNoWriteEnabled()) return;
  auto* rt = m_geoRT.Get();

  D3D12_RESOURCE_DESC rtDesc = rt->GetDesc();

  // Dest texel layout from RB_COPY_DEST_INFO (Xenia registers.h:870 +
  // resolve.xesli): [12:7] format, [2:0] endian128, [24] red/blue swap.
  // destInfo==0 keeps the legacy 8888 no-swap behavior (bytes R,G,B,A).
  const uint32_t dFmt = (destInfo >> 7) & 0x3Fu;
  const uint32_t dEndian = destInfo & 7u;
  const bool dSwap = ((destInfo >> 24) & 1u) != 0;
  const int texelBytes = resolveDestTexelBytes(dFmt);

  const int copyW = (m_width < (destPitchBytes / texelBytes)) ? m_width : (destPitchBytes / texelBytes);
  int copyH = (m_height < maxRows) ? m_height : maxRows;
  if (copyW <= 0 || copyH <= 0) return;

  // iter-437b: read back ONLY the dest-sized region. The HLE Resolve hook
  // feeds ~200 resolves/s (display + HDR scene + the 1x1..64x64 luminance
  // chain + downsamples); a full 1280x720 RT readback per resolve saturated
  // the drain (queue-full drops of draws AND display resolves, presents
  // -40%). The box slashes the PCIe/CPU bytes for every sub-full dest.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  footprint.Offset = 0;
  footprint.Footprint.Format = rtDesc.Format;
  footprint.Footprint.Width = UINT(copyW);
  footprint.Footprint.Height = UINT(copyH);
  footprint.Footprint.Depth = 1;
  footprint.Footprint.RowPitch =
      UINT((UINT(copyW) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
  const UINT64 totalBytes = UINT64(footprint.Footprint.RowPitch) * UINT64(copyH);

  resourceBarrier(m_commandList.Get(), rt,
                  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource = rt;
  srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcLoc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource = m_readbackBuffer.Get();
  dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstLoc.PlacedFootprint = footprint;

  D3D12_BOX srcBox = {0, 0, 0, UINT(copyW), UINT(copyH), 1};
  m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);

  resourceBarrier(m_commandList.Get(), rt,
                  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

  // iter-437c [DRAIN_PROF]: phase timers. The drain is the present-rate
  // bottleneck (17/s vs 29/s pre-hook) and the queue drops under the game's
  // real resolve traffic (96K/run) — name the dominant cost before cutting.
  const int64_t t0 = profQpc();

  clClose(CopyRtClose);      // iter-445 C1
  clExecute(CopyRtExec);
  waitForGpu();

  const int64_t t1 = profQpc();

  D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};
  void* mapped = nullptr;
  if (FAILED(m_readbackBuffer->Map(0, &readRange, &mapped))) return;
  profResolveWaitUs.fetch_add(profUs(t1 - t0), std::memory_order_relaxed);
  profResolveN.fetch_add(1, std::memory_order_relaxed);
  struct ConvertTimer {
    int64_t t;
    ConvertTimer() : t(profQpc()) {}
    ~ConvertTimer() {
      profResolveConvUs.fetch_add(profUs(profQpc() - t), std::memory_order_relaxed);
    }
  } convTimer;

  const UINT srcPitch = footprint.Footprint.RowPitch;
  convertResolveToGuest(hostDest, destPitchBytes, destInfo,
                        static_cast<const uint8_t*>(mapped), srcPitch, copyW, copyH);

  D3D12_RANGE writeRange = {0, 0};
  m_readbackBuffer->Unmap(0, &writeRange);
}

// iter-437c: record one resolve's GPU copy in-stream and defer wait+convert.
// See the PendingResolve note in the header for the full contract.
bool D3D12Backend::recordPendingResolve(const DrawBatch& batch) {
  bool flushed = false;
  if (!m_resolveReadback) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kResolveRbBytes;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&m_resolveReadback)))) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[XLAT_RESOLVE] deferred readback buffer creation FAILED");
      return false;
    }
  }
  const uint32_t dfmt = (batch.resolveDestInfo >> 7) & 0x3Fu;
  const int texelBytes = resolveDestTexelBytes(dfmt);
  const int destPitchBytes = int(batch.resolvePitchTexels) * texelBytes;
  // iter-439 P1: sub-rect resolves (predicated tiling). With the gate on and
  // a rect present, copy exactly the tile's region of the RT and place it at
  // the same offset in the dest (X360 Resolve: dest point = src rect origin).
  int srcX = 0, srcY = 0;
  int copyW, copyH;
  const bool useRect = resolveSrcRectEnabled() && batch.resolveSrcW && batch.resolveSrcH;
  if (useRect) {
    srcX = (int(batch.resolveSrcX) < m_width) ? int(batch.resolveSrcX) : m_width;
    srcY = (int(batch.resolveSrcY) < m_height) ? int(batch.resolveSrcY) : m_height;
    copyW = int(batch.resolveSrcW);
    if (srcX + copyW > m_width) copyW = m_width - srcX;
    copyH = int(batch.resolveSrcH);
    if (srcY + copyH > m_height) copyH = m_height - srcY;
  } else {
    copyW = (m_width < (destPitchBytes / texelBytes)) ? m_width
                                                      : (destPitchBytes / texelBytes);
    copyH = (m_height < int(batch.resolveRows)) ? m_height : int(batch.resolveRows);
  }
  if (copyW <= 0 || copyH <= 0) return false;
  const UINT rowPitch =
      UINT((UINT(copyW) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
  const uint64_t bytes = uint64_t(rowPitch) * uint64_t(copyH);
  uint64_t offset =
      (m_resolveRbCursor + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
      ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  if (offset + bytes > kResolveRbBytes) {
    // Buffer exhausted: flush reopens the frame list. The 437c crash class:
    // this KILLS the caller's recorded draw state (RTV/PSO) — the return
    // value tells the drain loop to rebind before its next draw.
    flushed = flushPendingResolves("rb-full");
    offset = 0;
  }

  // Evict overlapping texture-cache entries NOW so a later bind re-creates
  // from guest memory — the bind-time overlap check then flushes first, so
  // the re-create reads post-resolve bytes (console same-frame contract).
  {
    const int64_t tEv0 = profQpc();
    uint32_t evicted = 0;
    const uint64_t lo = batch.resolveDestPhys & 0x1FFFFFFFu;
    const uint64_t hi = lo + uint64_t(batch.resolvePitchTexels) * batch.resolveRows * texelBytes;
    std::vector<uint64_t> overlapKeys;  // step 4 G7 (SR_TEX_CACHE_EVICT): evicted after the scan
    for (auto& e : m_texEntries) {
      const uint32_t basePhys = e.basePhys & 0x1FFFFFFFu;
      if (basePhys < hi && uint64_t(basePhys) + e.bytes > lo &&
          (m_texEvictOn ? m_texCache.count(e.key) != 0 : m_texCache.erase(e.key) != 0)) {
        evicted++;
        if (m_texEvictOn) overlapKeys.push_back(e.key);
      }
    }
    for (uint64_t k : overlapKeys) texEvictKey(k, kTexEvResolve);
    texEvictFinish();
    profEvictUs.fetch_add(profUs(profQpc() - tEv0), std::memory_order_relaxed);
    (void)evicted;
  }
  // Coalesce: a still-pending same-dest resolve survived with no overlapping
  // bind (one would have flushed) — its output is overwritten unread. Dead.
  // iter-439 P1: with sub-rect resolves the two TILE resolves share a dest —
  // only coalesce when the new rect actually covers the pending one.
  for (auto& p : m_pendingResolves)
    if (!p.dead && p.destPhys == batch.resolveDestPhys &&
        srcX <= p.dstX && srcY <= p.dstY &&
        srcX + copyW >= p.dstX + p.copyW && srcY + copyH >= p.dstY + p.copyH)
      p.dead = true;

  auto* rt = m_geoRT.Get();
  resourceBarrier(m_commandList.Get(), rt,
                  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource = rt;
  srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcLoc.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource = m_resolveReadback.Get();
  dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstLoc.PlacedFootprint.Offset = offset;
  dstLoc.PlacedFootprint.Footprint.Format = rt->GetDesc().Format;
  dstLoc.PlacedFootprint.Footprint.Width = UINT(copyW);
  dstLoc.PlacedFootprint.Footprint.Height = UINT(copyH);
  dstLoc.PlacedFootprint.Footprint.Depth = 1;
  dstLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
  D3D12_BOX srcBox = {UINT(srcX), UINT(srcY), 0,
                      UINT(srcX + copyW), UINT(srcY + copyH), 1};
  m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
  // iter-438d GPU-side resolve texture: ALSO record a copy into a persistent
  // bindable texture at this dest (stream-ordered: lands after this pass's
  // draws, before any later bind that samples it). Later binds are served by
  // findResolveTexSlot with ZERO CPU decode/upload and no bind-time GPU sync —
  // the composite-era collapse was texBindMs=77% of render + 714 full syncs
  // per profile window, all re-decoding resolve dests from guest memory.
  // Guest write-back (the readback above) is unchanged — memory truth keeps
  // flowing for probes/mods at drain-end cost only.
  // iter-439 P1: the ResolveTex is sized to the FULL dest surface; sub-rect
  // resolves land at their (dstX,dstY) placement inside it.
  if (resolveTexSnapEnabled()) {
    const uint32_t fullW =
        uint32_t((m_width < (destPitchBytes / texelBytes)) ? m_width
                                                           : (destPitchBytes / texelBytes));
    const uint32_t fullH =
        uint32_t((m_height < int(batch.resolveRows)) ? m_height : int(batch.resolveRows));
    if (ResolveTex* rtex = ensureResolveTex(batch.resolveDestPhys,
                                            fullW ? fullW : uint32_t(copyW),
                                            fullH ? fullH : uint32_t(copyH),
                                            rt->GetDesc().Format)) {
      resourceBarrier(m_commandList.Get(), rtex->res.Get(),
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION texDst = {};
      texDst.pResource = rtex->res.Get();
      texDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      texDst.SubresourceIndex = 0;
      const UINT tdX = (UINT(srcX) < rtex->w) ? UINT(srcX) : 0u;
      const UINT tdY = (UINT(srcY) < rtex->h) ? UINT(srcY) : 0u;
      D3D12_BOX texBox = srcBox;
      if (tdX + UINT(copyW) > rtex->w) texBox.right = texBox.left + (rtex->w - tdX);
      if (tdY + UINT(copyH) > rtex->h) texBox.bottom = texBox.top + (rtex->h - tdY);
      m_commandList->CopyTextureRegion(&texDst, tdX, tdY, 0, &srcLoc, &texBox);
      resourceBarrier(m_commandList.Get(), rtex->res.Get(),
                      D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
  }
  resourceBarrier(m_commandList.Get(), rt,
                  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  m_resolveRbCursor = offset + bytes;

  PendingResolve p{};
  p.destPhys = batch.resolveDestPhys;
  p.pitchTexels = batch.resolvePitchTexels;
  p.rows = uint32_t(copyH);
  p.destInfo = batch.resolveDestInfo;
  p.rbOffset = offset;
  p.rbRowPitch = rowPitch;
  p.copyW = copyW;
  p.copyH = copyH;
  p.dstX = srcX;
  p.dstY = srcY;
  m_pendingResolves.push_back(p);
  return flushed;
}

bool D3D12Backend::pendingResolveOverlaps(uint64_t lo, uint64_t hi) const {
  auto hit = [&](const PendingResolve& p) {
    const uint32_t dfmt = (p.destInfo >> 7) & 0x3Fu;
    const uint64_t plo = p.destPhys & 0x1FFFFFFFu;
    const uint64_t phi =
        plo + uint64_t(p.pitchTexels) * resolveDestTexelBytes(dfmt) * p.rows;
    return plo < hi && phi > lo;
  };
  for (const auto& p : m_pendingResolves)
    if (hit(p)) return true;
  // iter-439 P2: an async-flushed-but-unconverted resolve is still pending
  // from the guest-memory point of view — its write-back hasn't landed.
  for (const auto& f : m_inFlightResolves)
    for (const auto& p : f.items)
      if (hit(p)) return true;
  return false;
}

// iter-439 P2: convert one flush's items from the mapped readback buffer into
// guest memory. Shared by the sync flush and the deferred async completion —
// rbOffsets are absolute within m_resolveReadback, so mappedBase is always the
// buffer's start.
void D3D12Backend::convertResolveItems(const std::vector<PendingResolve>& items,
                                       const uint8_t* mappedBase) {
    for (const auto& p : items) {
      profResolveN.fetch_add(1, std::memory_order_relaxed);
      {
        // iter-438c H1 probe bookkeeping: remember every resolve dest (dead
        // ones too — a bind reading a dead dest reads STALE guest bytes, which
        // is itself a finding). Ring is tiny; bind-time lookup is capped-log.
        const uint32_t dfmt0 = (p.destInfo >> 7) & 0x3Fu;
        auto& r = m_recentResolveDests[m_recentResolveCursor++ & 15u];
        r.phys = p.destPhys & 0x1FFFFFFFu;
        r.bytes = p.pitchTexels * uint32_t(resolveDestTexelBytes(dfmt0)) *
                  uint32_t(p.copyH > 0 ? p.copyH : 0);
        r.destInfo = p.destInfo;
        r.dead = p.dead;
      }
      if (p.dead) continue;
      uint8_t* dst = const_cast<uint8_t*>(gpuPhysToHost(p.destPhys));
      if (!dst) continue;
      const uint32_t dfmt = (p.destInfo >> 7) & 0x3Fu;
      const int texelBytes = resolveDestTexelBytes(dfmt);
      // iter-439 P1: sub-rect placement — write the tile's region at its
      // offset inside the dest surface, not at the origin.
      dst += size_t(p.dstY) * size_t(p.pitchTexels) * texelBytes +
             size_t(p.dstX) * texelBytes;
      const int64_t tc = profQpc();
      convertResolveToGuest(dst, int(p.pitchTexels) * texelBytes, p.destInfo,
                            mappedBase + p.rbOffset,
                            p.rbRowPitch, p.copyW, p.copyH);
      profResolveConvUs.fetch_add(profUs(profQpc() - tc), std::memory_order_relaxed);
      // iter-408b FRONT_RESOLVE tally + composite dumps (moved from the
      // immediate path): resolves that target the displayed surface, measured.
      if (m_guestFrontPhys &&
          (p.destPhys & 0x1FFFFFFFu) == (m_guestFrontPhys & 0x1FFFFFFFu)) {
        const size_t span = size_t(p.pitchTexels) * texelBytes * size_t(p.copyH);
        size_t nonBlack = 0, sampled = 0;
        for (size_t i = 0; i + 4 <= span; i += 4 * 251) {
          ++sampled;
          if (dst[i] > 8 || dst[i + 1] > 8 || dst[i + 2] > 8) nonBlack++;
        }
        static std::atomic<uint64_t> s_frontN{0};
        const uint64_t fn = s_frontN.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fn <= 6 || fn % 100 == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[FRONT_RESOLVE] #%llu dest=0x%08X %ux%u nonBlackTexels=%zu/%zu",
                      (unsigned long long)fn, p.destPhys, p.pitchTexels,
                      p.copyH, nonBlack, sampled);
        static std::atomic<uint32_t> s_frontShots{0};
        if (nonBlack > 0 && s_frontShots.load(std::memory_order_relaxed) < 12) {
          const uint32_t shot = s_frontShots.fetch_add(1, std::memory_order_relaxed);
          if (shot < 12) {
            const uint32_t w = p.pitchTexels, h = uint32_t(p.copyH);
            std::vector<uint8_t> bgra(size_t(w) * h * 4u);
            for (size_t t = 0; t < size_t(w) * h; ++t) {
              const uint8_t* sp = dst + t * 4;
              uint8_t* dp = bgra.data() + t * 4;
              dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
            }
            char nm[128];
            snprintf(nm, sizeof(nm), "composite_%02u_n%zu_%ux%u.bmp", shot, nonBlack, w, h);
            writeBmp32(nm, bgra.data(), w, h, w * 4);
            SR_LOG_BOOT(sr::LogSeverity::Info, "[COMPOSITE_DUMP] %s", nm);
          }
        }
      }
    }
}

// iter-439 P2: settle async-flushed resolves whose fence has passed (poll), or
// ALL of them (waitAll — the bind-overlap/rb-full paths need guest bytes now).
// Pure CPU + fence wait; never touches the frame command list, so callers keep
// their recorded draw state. Rewinds the readback ring once fully reclaimed.
bool D3D12Backend::completeInFlightResolves(bool waitAll) {
  if (m_inFlightResolves.empty()) {
    if (m_pendingResolves.empty()) { m_resolveRbBase = 0; m_resolveRbCursor = 0; }
    return false;
  }
  if (!m_resolveFence) nativeRecordingFail("resolve-missing-fence", E_UNEXPECTED);
  if (waitAll) {
    const UINT64 want = m_inFlightResolves.back().fenceValue;
    const int64_t t0 = profQpc();
    waitNativeFence(m_resolveFence.Get(), want, m_resolveFenceEvent, "resolve-wait");
    profResolveWaitUs.fetch_add(profUs(profQpc() - t0), std::memory_order_relaxed);
  }
  const UINT64 done = m_resolveFence->GetCompletedValue();
  if (done == UINT64_MAX) nativeRecordingFail("resolve-completion", DXGI_ERROR_DEVICE_REMOVED);
  bool any = false;
  while (!m_inFlightResolves.empty() &&
         m_inFlightResolves.front().fenceValue <= done) {
    auto& f = m_inFlightResolves.front();
    void* mapped = nullptr;
    D3D12_RANGE rr = {static_cast<SIZE_T>(f.rbBegin), static_cast<SIZE_T>(f.rbEnd)};
    if (SUCCEEDED(m_resolveReadback->Map(0, &rr, &mapped))) {
      convertResolveItems(f.items, static_cast<const uint8_t*>(mapped));
      D3D12_RANGE wr = {0, 0};
      m_resolveReadback->Unmap(0, &wr);
    }
    m_inFlightResolves.pop_front();
    any = true;
  }
  if (m_inFlightResolves.empty() && m_pendingResolves.empty()) {
    m_resolveRbBase = 0;  // whole ring reclaimed — rewind (healthy steady state)
    m_resolveRbCursor = 0;
  }
  return any;
}

bool D3D12Backend::flushPendingResolves(const char* why, bool allowAsync) {
  if (m_pendingResolves.empty()) return false;
  {
    static std::atomic<uint32_t> s_flushLogN{0};
    const uint32_t fl = s_flushLogN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fl <= 12 || fl % 1000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[RESOLVE_FLUSH] #%u why=%s pending=%zu bytes=%llu inFlight=%zu",
                  fl, why, m_pendingResolves.size(),
                  (unsigned long long)(m_resolveRbCursor - m_resolveRbBase),
                  m_inFlightResolves.size());
  }

  bool asyncOk = false;
  if (allowAsync && resolveAsyncEnabled()) {
    if (!m_resolveFence && m_device) {
      if (SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                          IID_PPV_ARGS(&m_resolveFence)))) {
        m_resolveFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_resolveFenceEvent) nativeRecordingFail("resolve-event", HRESULT_FROM_WIN32(GetLastError()));
      } else
        m_resolveFence.Reset();
    }
    asyncOk = (m_resolveFence != nullptr && m_resolveFenceEvent != nullptr);
  }

  if (asyncOk) {
    // ASYNC (iter-439 P2): submit + signal, park the items; the convert runs
    // at a later drain's completeInFlightResolves once the fence passes. The
    // guest sees its resolve bytes one drain late — the console's own
    // resolve-to-scanout distance — and the drain no longer serializes on the
    // GPU every present (the ident-era queue-full wedge).
    clClose(ResolveAsyncClose);   // iter-445 C1
    clExecute(ResolveAsyncExec);
    if (m_resolveFenceNext >= UINT64_MAX - 1) nativeRecordingFail("resolve-ticket-exhausted", E_UNEXPECTED);
    const UINT64 fv = ++m_resolveFenceNext;
    const HRESULT signaled = m_commandQueue->Signal(m_resolveFence.Get(), fv);
    if (FAILED(signaled)) nativeRecordingFail("resolve-signal", signaled);
    InFlightResolveFlush f;
    f.items = std::move(m_pendingResolves);
    f.fenceValue = fv;
    f.rbBegin = m_resolveRbBase;
    f.rbEnd = m_resolveRbCursor;
    m_inFlightResolves.push_back(std::move(f));
    m_pendingResolves.clear();
    m_resolveRbBase = m_resolveRbCursor;  // ring advances; reclaimed on completion
  } else {
    // SYNC (rb-full / bind-overlap / SR_RESOLVE_ASYNC=0): settle older async
    // flushes first (their regions and guest bytes), then wait and convert.
    completeInFlightResolves(true);
    const int64_t t0 = profQpc();
    clClose(ResolveSyncClose);    // iter-445 C1
    clExecute(ResolveSyncExec);
    waitForGpu();
    profResolveWaitUs.fetch_add(profUs(profQpc() - t0), std::memory_order_relaxed);

    void* mapped = nullptr;
    D3D12_RANGE rr = {0, static_cast<SIZE_T>(m_resolveRbCursor)};
    if (SUCCEEDED(m_resolveReadback->Map(0, &rr, &mapped))) {
      convertResolveItems(m_pendingResolves, static_cast<const uint8_t*>(mapped));
      D3D12_RANGE wr = {0, 0};
      m_resolveReadback->Unmap(0, &wr);
    }
    m_pendingResolves.clear();
    m_resolveRbCursor = 0;
    m_resolveRbBase = 0;
  }

  // Reopen the frame list for the caller; draw-state rebind (target/PSO) is
  // the caller's job via bindTarget + curKind=-1.
  clReset(ResolveReset);   // iter-445 C1
  D3D12_RECT scissor = {0, 0, LONG(m_width), LONG(m_height)};
  m_commandList->RSSetScissorRects(1, &scissor);
  if (xlatReady()) {
    ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
    m_commandList->SetDescriptorHeaps(2, heaps);
  }
  return true;
}


#endif  // SR_ORACLE
// step 4 2k: the census / manifest / warm-up on this backend's native pipeline cache (gfx/pso_warm.cpp)
void D3D12Backend::psoAttach() {
  psoAttachTo(&m_nativePipelines, m_nativeXlatRoot.root());
#if SR_ORACLE
  // step 4 G7 option (c): SR_ALLOC_CENSUS's backend source (registered once; the census reads it from its thread)
  static std::atomic<bool> s_reg{false};
  if (!s_reg.exchange(true))
    sr::perf::allocCensusRegister("backend", +[](void* ctx, sr::perf::AllocCensusEmit emit) {
      static_cast<const D3D12Backend*>(ctx)->allocCensusReport(emit);
    }, this);
#endif
}

#if SR_ORACLE
// Entry counts only (no bytes: none of these containers tracks them). Read from the census thread without the
// owners' locks: a size() read is a racy word read, acceptable for a diagnostic sampled every 30 s.
void D3D12Backend::allocCensusReport(void (*emit)(const char* group, const char* name, double value)) const {
  emit("backend", "native_pipelines", double(m_nativePipelines.size()));
  emit("backend", "xlat_pso_cache", double(m_xlatPsoCache.size()));
  emit("backend", "native_xlat_pso_cache", double(m_nativeXlatPsoCache.size()));
  emit("backend", "view_ids", double(m_viewIds.size()));
  emit("backend", "pm4_rts", double(m_pm4Rts.size()));
  emit("backend", "chain_psos", double(m_chainPsos.size()));
  emit("backend", "xlat_vb_dedup", double(m_xlatVbDedup.size()));
  emit("backend", "pm4_load_psos", double(m_pm4LoadPsos.size()));
  emit("backend", "nb_fill_scratch", double(m_nbFillScratch.size()));
  emit("backend", "ib_conv", double(m_ibConv.size()));
  emit("backend", "hle_surfaces", double(m_hleSurfaces.size()));
  emit("backend", "tex_cache", double(m_texCache.size()));
  emit("backend", "tex_refresh", double(m_texRefresh.size()));
  emit("backend", "tex_entries", double(m_texEntries.size()));
  emit("backend", "tex_tracked", double(m_texTracked.size()));
  emit("backend", "tex_shared", double(m_texShared.size()));
  emit("backend", "tex_retired", double(m_texRetired.size()));
  emit("backend", "tex_next_slot", double(m_texNextSlot));
  emit("backend", "tex_page_refs", double(m_texPageRefs));
  emit("backend", "tex_arrays", double(m_texArrays.size()));
  emit("backend", "tex_volumes", double(m_texVolumes.size()));
  if (m_texEvictOn) {  // step 4 G7: SR_TEX_CACHE_EVICT
    emit("backend", "tex_live_mb", double(m_texLiveBytes) / 1048576.0);
    emit("backend", "tex_live_peak_mb", double(m_texLivePeak) / 1048576.0);
    emit("backend", "tex_evicted", double(m_texEvictCount));
    emit("backend", "tex_evicted_mb", double(m_texEvictBytes) / 1048576.0);
    emit("backend", "tex_free_slots", double(m_texFreeSlots.size()));
    emit("backend", "tex_retiring", double(m_texSlotRetire.pending()));
    emit("backend", "tex_regs", double(m_texRegs.size()));
    emit("backend", "tex_fence_check_holds", double(m_texSlotRetire.violations()));
    emit("backend", "tex_stale_binds", double(m_texStaleBinds));
    emit("backend", "tex_transient_fails", double(m_texTransientFails));
    emit("backend", "tex_transient_suppressed", double(m_texTransientSuppressed));
    emit("backend", "tex_sweep_max_ms", double(m_texSweepUsMax) / 1000.0);
    emit("backend", "tex_drain_max_ms", double(m_texDrainUsMax) / 1000.0);
  }
  emit("backend", "hle_textures", double(m_hleTextures.size()));
  emit("backend", "tex_pages", double(m_texPages.size()));
  emit("backend", "tex_clean", double(m_texClean.size()));
  emit("backend", "glyph_repl", double(m_glyphRepl.size()));
  emit("backend", "shm_scratch_mb", double(m_shmScratchBytes) / 1048576.0);
  emit("backend", "shader_cache_vs", double(getShaderCache().vsCount()));
  emit("backend", "shader_cache_ps", double(getShaderCache().psCount()));
  // (4) video memory: this process's use, local and non-local (on an APU both are system RAM)
  if (m_device && m_factory) {
    Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
    if (SUCCEEDED(m_factory->EnumAdapterByLuid(m_device->GetAdapterLuid(), IID_PPV_ARGS(&a3)))) {
      DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
      if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
        emit("video", "local_usage_mb", double(local.CurrentUsage) / 1048576.0);
      if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal)))
        emit("video", "nonlocal_usage_mb", double(nonLocal.CurrentUsage) / 1048576.0);
    }
  }
}
#endif

void D3D12Backend::shutdown() {
  if (!m_initialized) return;
  // step 4 2k / Astra P2 1: the warm-up joined before anything it uses goes; a worker still busy after the deadline
  // (Astra 10:2x #5) keeps everything: the process ends here, nothing torn down under it
  if (!psoShutdown()) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PSO_WARM] the backend's teardown found a pipeline worker still busy - std::quick_exit(0), nothing it "
                "uses is torn down");
    sr::Log::shutdown();
    std::quick_exit(0);
  }
  retirePm4LiveBackend(this);
  hle::retireBackend(this);
  pm4RenderThreadStop();  // iter-461: before the fence event goes
  i3bCensusLine(" at=shutdown");  // the recordings are done: the final totals
#if SR_ORACLE
  memres::resourceCheckTerminal();
  resourceFetchReport("shutdown");
#endif
  const HRESULT imageDrain = m_nativeImages.shutdown();
  if (FAILED(imageDrain)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_IMAGES] shutdown drain failed: 0x%08X", imageDrain);
    std::_Exit(3);
  }
  hdrProbeDiscardUnsubmitted();
  hdrProbeCollect();  // the successful existing shutdown drain completed every submitted copy
  if (!m_hdrProbePending.empty()) nativeRecordingFail("hdr-async-shutdown-pending", E_UNEXPECTED);
#if SR_ORACLE
  resourceTextureProbeDiscard();
  resourceTextureProbeCollect();
  if (m_resourceTextureProbe) nativeRecordingFail("resource-texture-probe-shutdown-pending", E_UNEXPECTED);
#endif
  m_pm4Rts.clear();
  {
    const auto c = m_nativePipelines.counters();   // I1 (Fable ruling 21 (1)): the run's totals
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PSO_CACHE] shutdown hits=%llu creates=%llu borrowed_key_lookups=%llu root_memo_hits=%llu "
                "interned_shader_copies=%llu entries=%zu",
                (unsigned long long)c.hits, (unsigned long long)c.creates, (unsigned long long)c.borrowedKeyLookups,
                (unsigned long long)c.rootMemoHits, (unsigned long long)NativeShaderCode::internedCopies(),
                m_nativePipelines.size());
  }
  m_nativePipelines.clear();
  m_commandList2.Reset();
  m_commandList.Reset();
  m_nativeRecording.reset();  // unsubmitted recordings are discarded, never executed
  drainReuseCadenceLine(" shutdown");
#if SR_ORACLE
  xlatDropCadenceLine(" shutdown");   // step 6 drop fix
#endif
  zcCounterLine(" shutdown");   // step 6 I10p
  m_zcSharedSlice = {};
  m_nativeSamplers = {};
  m_nativeTextures.clear();
  m_nativeRawGeometry.discardTail();
  m_texKeys.clear();
  m_drawSamplers = {};
  m_lastConstants = {};
  m_xlatViews.clear();  // checked shutdown has completed submitted uses
  m_xlatViewId.clear();
  m_viewIds.clear(); m_viewIdCount = 0;
  m_nativeConstants.discardTail();
  m_nativeGeometry.discardTail();
  if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
#if SR_ORACLE
  if (m_resolveFenceEvent) { CloseHandle(m_resolveFenceEvent); m_resolveFenceEvent = nullptr; }
#endif
  m_initialized = false;
}

#if SR_ORACLE
// iter-410 XENIA PARITY: present the GUEST FRONT BUFFER, not the render target.
//
// Xenia's D3D12CommandProcessor::IssueSwap does NOT present its render target.
// It calls texture_cache_->RequestSwapTexture(), which reads TEXTURE FETCH
// CONSTANT 0 through the ordinary texture path (BindingInfoFromFetchConstant ->
// FindOrCreateTexture -> LoadTextureData) and applies the guest swizzle. The
// front buffer in guest RAM is the console's actual output; EDRAM/the render
// target is an intermediate that the guest's resolves drain into it.
//
// Ours copied m_geoRT to the back buffer, so the screen only ever showed what
// the MOST RECENT drain drew - and the RT is cleared at the top of every
// renderGuestGeometry. Measured consequence: the back buffer was flat on 92/92
// captures while the resolve DESTINATION (the front buffer) demonstrably held
// content. The golden-trace replay looked fine only because a single-frame
// trace delivers the whole frame in one call, so RT == frame by accident.
//
// Uses the dedicated upload list (its own allocator + fence), because the frame
// list is already closed and executed by the time the geometry path gets here.
static bool presentFrontBufferEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PRESENT_FRONTBUF");
    return e && e[0] == '1';
  }();
  return v;
}

bool D3D12Backend::blitGuestFramebufferToBackBuffer(const uint8_t* data, int w, int h,
                                                    int pitchBytes, bool srcIsHostScratch,
                                                    ID3D12Resource* rt) {
  if (!data || w <= 0 || h <= 0 || pitchBytes <= 0 || !rt || !m_uploadBuffer) return false;
  const int copyW = (w < m_width) ? w : m_width;
  const int copyH = (h < m_height) ? h : m_height;
  if (copyW <= 0 || copyH <= 0) return false;
  // Fault-free bounds check (HARD RULE: a host AV is fatal under the harness
  // VEH). Host scratch is sized by construction; guest memory must be proven.
  if (!srcIsHostScratch) {
    uint8_t* gb = nullptr; size_t gsz = 0;
    sr::kernel::getGlobalGuestMemory(&gb, &gsz);
    const size_t span = size_t(copyH - 1) * size_t(pitchBytes) + size_t(copyW) * 4u;
    if (!gb || !gsz || data < gb || data + span > gb + gsz) return false;
  }
  const UINT rowPitch = UINT((m_width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                             ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
  if (m_uploadBufferSize < size_t(rowPitch) * size_t(m_height)) return false;
  void* mapped = nullptr;
  D3D12_RANGE noRead{0, 0};
  if (FAILED(m_uploadBuffer->Map(0, &noRead, &mapped))) return false;
  uint8_t* dst = static_cast<uint8_t*>(mapped);
  std::memset(dst, 0, size_t(rowPitch) * size_t(m_height));
  for (int y = 0; y < copyH; y++) {
    const uint8_t* src = data + size_t(y) * size_t(pitchBytes);
    uint8_t* dstRow = dst + size_t(y) * rowPitch;
    // iter-440f THE RED ERAS: this blit assumed guest B,G,R,A (stale iter-359h
    // note) while the other present branch (upload path) reads R,G,B,A — and
    // the resolve write-back writes R,G,B,A (Frida fb dump during a "red"
    // frame: (0x19,0x21,0x2F)=dark blue ident content; the swap here turned
    // every fb-blit era solid red/maroon — INCLUDING roll-G's maroon).
    // One contract now: R,G,B,A straight copy. SR_FB_BLIT_BGRA=1 restores the
    // old swap for A/B.
    static const bool s_bgra = [] {
      const char* e = std::getenv("SR_FB_BLIT_BGRA");
      return e && e[0] == '1';
    }();
    if (s_bgra) {
      for (int x = 0; x < copyW; x++) {
        dstRow[x * 4 + 0] = src[x * 4 + 2];
        dstRow[x * 4 + 1] = src[x * 4 + 1];
        dstRow[x * 4 + 2] = src[x * 4 + 0];
        dstRow[x * 4 + 3] = src[x * 4 + 3];
      }
    } else {
      std::memcpy(dstRow, src, size_t(copyW) * 4u);
    }
  }
  m_uploadBuffer->Unmap(0, nullptr);

  if (FAILED(m_texUploadAlloc->Reset())) return false;
  if (FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) return false;
  resourceBarrier(m_texUploadList.Get(), rt,
                  D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION s{}, d{};
  s.pResource = m_uploadBuffer.Get();
  s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  s.PlacedFootprint.Offset = 0;
  s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  s.PlacedFootprint.Footprint.Width = UINT(m_width);
  s.PlacedFootprint.Footprint.Height = UINT(m_height);
  s.PlacedFootprint.Footprint.Depth = 1;
  s.PlacedFootprint.Footprint.RowPitch = rowPitch;
  d.pResource = rt;
  d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  d.SubresourceIndex = 0;
  m_texUploadList->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
  resourceBarrier(m_texUploadList.Get(), rt,
                  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  m_texUploadList->Close();
  ID3D12CommandList* l[] = {m_texUploadList.Get()};
  m_commandQueue->ExecuteCommandLists(1, l);
  m_texFenceValue++;
  m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
  if (m_texFence->GetCompletedValue() < m_texFenceValue) {
    m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
    WaitForSingleObject(m_texFenceEvent, 10000);
  }
  return true;
}
#endif

// iter-408b: capture the actual swapchain back buffer immediately before
// Present (SR_PRESENT_DUMP, default OFF). Every other dump in this file shows
// an INPUT to the pipeline — a decoded texture, a resolve source. None of them
// answer the only question that matters for "is there a picture": what did the
// user's monitor receive. File sampling (iter-455l r3: wording corrected per the
// director — there is no "every 20th"): upload-path presents every 3rd up to 90
// files; clear-path every 120th up to 60 (both sparse knobs tunable); EVERY=1
// window mode capped at 240. The SCAN is decided separately below — file caps
// never end it while the provenance ring needs it.
#if SR_ORACLE
namespace {
bool holdLogOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_DIAG_HOLD_LOG");
    return e && e[0] == '1';
  }();
  return on;
}
}  // namespace

void D3D12Backend::holdNoteTex(char kind, uint32_t base, uint32_t fmt, uint32_t w, uint32_t h) {
  if (!holdLogOn() || sr::kernel::highfps::freezeHeldMs() <= 0.0) return;
  std::lock_guard<std::mutex> lk(m_holdMu);
  m_holdTexCount++;
  if (m_holdTex.size() < 8) m_holdTex.push_back({kind, base, fmt, w, h});
}

void D3D12Backend::holdLogPresent(uint64_t pn, uint64_t fnv, uint32_t w, uint32_t h) {
  if (!holdLogOn()) return;
  static uint64_t s_draws = 0, s_tex = 0, s_arr = 0, s_res = 0, s_up = 0, s_inv = 0, s_faults = 0;
  const uint64_t faults = sr::kernel::gww::enabled() ? sr::kernel::gww::stats().faults : 0u;
  const double heldMs = sr::kernel::highfps::freezeHeldMs();
  std::vector<HoldTex> tex;
  uint32_t texCount = 0;
  {
    std::lock_guard<std::mutex> lk(m_holdMu);
    tex.swap(m_holdTex);
    texCount = m_holdTexCount;
    m_holdTexCount = 0;
  }
  if (heldMs > 0.0) {
    char list[400];
    size_t o = 0;
    list[0] = 0;
    for (const HoldTex& t : tex) {
      if (o + 40 >= sizeof(list)) break;
      o += size_t(std::snprintf(list + o, sizeof(list) - o, " %c:%08X/f%u/%ux%u", t.kind, t.base, t.fmt, t.w, t.h));
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[HOLD_FRAME] hold #%u held %.0f ms p%06llu fnv=%016llX %ux%u | draws %llu tex %llu arr %llu res %llu "
                "up %.1f KB inval %llu wwFaults %llu | loads %u:%s",
                sr::kernel::highfps::freezeClock().holdId, heldMs, (unsigned long long)pn, (unsigned long long)fnv, w,
                h, (unsigned long long)(m_zpdDrawSerial - s_draws), (unsigned long long)(m_shmTexLoads - s_tex),
                (unsigned long long)(m_shmArrayLoads - s_arr), (unsigned long long)(m_shmResolves - s_res),
                double(m_shmUploadBytes - s_up) / 1024.0, (unsigned long long)(m_texBlockInvals - s_inv),
                (unsigned long long)(faults - s_faults), texCount, list);
  }
  s_draws = m_zpdDrawSerial;
  s_tex = m_shmTexLoads;
  s_arr = m_shmArrayLoads;
  s_res = m_shmResolves;
  s_up = m_shmUploadBytes;
  s_inv = m_texBlockInvals;
  s_faults = faults;
}
#endif

void D3D12Backend::dumpBackBuffer(ID3D12Resource* rt) {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP");
    return e && e[0] == '1';
  }();
  // iter-445 C1: the project's ONLY trustworthy pixel oracle must never go
  // quiet with no message — PrintWindow and gdigrab both return black for our
  // flip-model swapchain, so if this returns silently there is nothing left to
  // judge a frame by, and "no dumps appeared" reads as "the run produced no
  // frames" rather than "the dumper was disabled".
  if (!s_on) return;
  if (!rt || !m_readbackBuffer || !m_texUploadList || !m_texUploadAlloc) {
    static uint64_t s_quiet = 0;
    if (++s_quiet <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[PRESENT_DUMP] SKIPPED #%llu: rt=%d readback=%d uploadList=%d "
          "uploadAlloc=%d — the pixel oracle is unavailable this frame",
          (unsigned long long)s_quiet, rt ? 1 : 0, m_readbackBuffer ? 1 : 0,
          m_texUploadList ? 1 : 0, m_texUploadAlloc ? 1 : 0);
    return;
  }
  static std::atomic<uint64_t> s_pn{0}, s_files{0};
  const uint64_t pn = s_pn.fetch_add(1, std::memory_order_relaxed);
  // Presents that actually uploaded the guest framebuffer are the only ones
  // that can show a picture, and they are RARE (the clear path dominates until
  // the guest starts swapping). Sampling uniformly across all presents spent
  // the whole budget on clear-path frames and never captured a single real one.
  // So: capture upload-path presents densely, clear-path ones sparsely.
  // iter-442 S4.7: SR_PRESENT_DUMP_EVERY=1 bypasses the sampling entirely. The
  // replay fixture is 40-90 presents long (s6i.srht = 60), so the every-120th
  // rule below would capture at most ONE frame of it — and a first-light verdict needs the whole
  // sequence, not a lucky sample.
  static const bool s_every = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_EVERY");
    return e && e[0] == '1';
  }();
  const bool uploaded = m_lastPresentUploaded;
  // iter-455l r3 (ruling ad4621a defect 3): file output and the SCAN are decided
  // SEPARATELY. These branches used to `return`, which ended the readback itself —
  // with SR_PROV_RING=1 the ring then announced AVAILABLE yet went permanently
  // blind once the caps filled (EVERY=1's 240 files ≈ 8 s at 30 fps, long before
  // main_hud). Files keep every cadence/cap unchanged; scanEligible() keeps the
  // readback alive for the trigger while the ring is enabled.
  bool fileWouldSample = true;
  // iter-458 (Astra 014): SR_PRESENT_DUMP_DURING_TRACE=1 (default OFF,
  // observe-only) writes EVERY present while the HLE trace recorder is open,
  // plus an 8-present tail, so the live frames of the exact trace window can be
  // graded against the replay of the same trace. Keyed to the recorder's
  // published state, not a guessed present number - the window's position
  // varies by roll.
  static const bool s_duringTrace = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_DURING_TRACE");
    return e && e[0] == '1';
  }();
  static std::atomic<int> s_traceTail{0};
  bool traceWin = false;
  if (s_duringTrace) {
    if (sr::gfx::hle::traceRecorderOpen()) {
      traceWin = true;
      s_traceTail.store(8, std::memory_order_relaxed);
    } else if (s_traceTail.load(std::memory_order_relaxed) > 0) {
      traceWin = true;
      s_traceTail.fetch_sub(1, std::memory_order_relaxed);
    }
  }
  // iter-466 hfr: SR_PRESENT_DUMP_FREEZE=1 (with the SR_DIAG_SIM_FREEZE diagnostic) writes only
  // a few presents of each sim freeze - the clock-inventory frames (docs/HIGH_FRAMERATE.md 6).
  static const bool s_freezeOnly = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_FREEZE");
    return e && (e[0] == '1' || e[0] == '2');  // =2: every frozen present (highfps::freezeDumpWanted)
  }();
  if (s_freezeOnly) {
    fileWouldSample = sr::kernel::highfps::freezeDumpWanted(pn);
    if (fileWouldSample) memres::resourceAbDumped(pn);  // the frozen-pass A/B: this dumped present's decider
  } else if (traceWin) {
    // dense: no stride, no cap
  } else if (s_every) {
    // iter-449p: SR_PRESENT_DUMP_FROM=<present#> positions the every-present
    // window anywhere in the run (default 0 = boot), and SR_PRESENT_DUMP_CAP
    // bounds it (default 240). Purpose: a CONSECUTIVE frame window over the
    // era under test — ffmpeg'd into an mp4 for the eyeball and diffed
    // frame-over-frame for a flicker METRIC. Contact sheets sample 1-in-120
    // and structurally cannot show temporal artifacts.
    static const uint64_t s_from = [] {
      const char* e = std::getenv("SR_PRESENT_DUMP_FROM");
      const long long v = e ? std::atoll(e) : 0;
      return (uint64_t)(v > 0 ? v : 0);
    }();
    static const uint32_t s_cap = [] {
      const char* e = std::getenv("SR_PRESENT_DUMP_CAP");
      const long v = e ? std::atol(e) : 0;
      return (uint32_t)(v > 0 ? v : 240);
    }();
    // iter-467 hooks: SR_PRESENT_DUMP_FROM_MARK=1 opens the window at the first present after the
    // harness channel's DUMP MARK (kHarnessFlagDumpMark; tools/controls/harness_play.py `mark dump`)
    // and ignores SR_PRESENT_DUMP_FROM. A present number is not a place in the game: boots differ by
    // thousands of presents before play (dev gate postmerge_sj: the HUD at p14,156 in one roll and
    // p21,251 in the next), so the flicker gate's span re-placed from one roll's count opened on the
    // next roll's opening movie. No mark, no window (the log says when it opens).
    static const bool s_fromMark = [] {
      const char* e = std::getenv("SR_PRESENT_DUMP_FROM_MARK");
      return e && e[0] == '1';
    }();
    static std::atomic<uint64_t> s_markPn{0};
    uint64_t from = s_from;
    if (s_fromMark) {
      from = s_markPn.load(std::memory_order_relaxed);
      if (!from && (sr::kernel::automation::harnessFlags() &
                    sr::kernel::automation::kHarnessFlagDumpMark)) {
        from = pn ? pn : 1;
        s_markPn.store(from, std::memory_order_relaxed);
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PRESENT_DUMP] window opened by the harness dump mark at present %llu (cap %u)",
                    (unsigned long long)from, s_cap);
      }
      if (!from) from = UINT64_MAX;
    }
    if (pn < from) fileWouldSample = false;
    else if (s_files.load(std::memory_order_relaxed) >= s_cap) fileWouldSample = false;
  } else if (uploaded) {
    static std::atomic<uint64_t> s_upFiles{0};
    if (pn % 3 != 0) fileWouldSample = false;
    else if (s_upFiles.fetch_add(1, std::memory_order_relaxed) >= 90)
      fileWouldSample = false;
  } else {
    // iter-424c: the movie window sits ~55-70s in (present ~3300-4200); the old
    // every-60th-cap-20 went blind at ~p1200, BEFORE any movie frame. Sample
    // sparser but cap higher so coverage spans the whole run.
    // iter-450V: both numbers are now tunable, because the hardcoded cap of 60
    // silently ended whole-run coverage 118 s into a 430 s drive run — the era
    // under test (everything after the storage dialog) produced no frames at
    // all, and the contact sheet looked complete. SR_PRESENT_DUMP_SPARSE_EVERY
    // widens the stride, SR_PRESENT_DUMP_SPARSE_CAP raises the ceiling.
    static const uint64_t s_sparseEvery = [] {
      const char* e = std::getenv("SR_PRESENT_DUMP_SPARSE_EVERY");
      const long long v = e ? std::atoll(e) : 0;
      return (uint64_t)(v > 0 ? v : 120);
    }();
    static const uint32_t s_sparseCap = [] {
      const char* e = std::getenv("SR_PRESENT_DUMP_SPARSE_CAP");
      const long v = e ? std::atol(e) : 0;
      return (uint32_t)(v > 0 ? v : 60);
    }();
    if (pn % s_sparseEvery != 0) fileWouldSample = false;
    else if (s_files.load(std::memory_order_relaxed) >= s_sparseCap)
      fileWouldSample = false;
  }
  // r4 (ruling 008481a): the ring's scan window is BOUNDED — scanWanted() is true
  // only from the consumed arm through the one-shot dump, so the readback + CPU
  // scan + [PRESENT_SIG] run per-present ONLY inside that window (and on legacy
  // file-sampled presents, as always). Pre-arm and post-dump presents with
  // exhausted caps do no readback at all.
#if SR_ORACLE
  if (!sr::gfx::prov::scanEligible(
          s_on,
          sr::gfx::prov::enabled() && sr::gfx::prov::ring().scanWanted(),
          fileWouldSample))
    return;
#else
  if (!(s_on && fileWouldSample)) return;  // SR_ORACLE=0: no provenance ring, so only the file path samples
#endif
  const uint64_t fileNo =
      fileWouldSample ? s_files.fetch_add(1, std::memory_order_relaxed) : 0;
  static const bool hdrProbe = [] {
    const char* e = std::getenv("SR_HDR_ASYNC_PROBE");
    return e && e[0] == '1';
  }();
  if (hdrProbe && fileWouldSample) {
    const auto freeze = sr::kernel::highfps::freezeClock();
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[HDR_ASYNC_PRESENT] frame=%llu present=%llu freeze=%u held=%u",
                (unsigned long long)m_frameEpoch, (unsigned long long)pn,
                freeze.holdId, freeze.holdStartTicks != 0);
  }

  D3D12_RESOURCE_DESC rd = rt->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT nr = 0; UINT64 rs = 0, tb = 0;
  m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &nr, &rs, &tb);
  // iter-464: a backbuffer of any size (a resized / fullscreen window) - the dump
  // has its own readback buffer, grown to fit (the shared one is 1280x720-sized).
  if (tb > m_dumpReadbackBytes) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = tb;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    m_dumpReadback.Reset();
    m_dumpReadbackBytes = 0;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&m_dumpReadback))))
      return;
    m_dumpReadbackBytes = tb;
  }
  if (FAILED(m_texUploadAlloc->Reset())) return;
  if (FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) return;
  resourceBarrier(m_texUploadList.Get(), rt,
                  D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION cd{}, cs{};
  cd.pResource = m_dumpReadback.Get();
  cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; cd.PlacedFootprint = fp;
  cs.pResource = rt; cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; cs.SubresourceIndex = 0;
  m_texUploadList->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
  resourceBarrier(m_texUploadList.Get(), rt,
                  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  m_texUploadList->Close();
  ID3D12CommandList* l[] = {m_texUploadList.Get()};
  m_commandQueue->ExecuteCommandLists(1, l);
  m_texFenceValue++;
  m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
  if (m_texFence->GetCompletedValue() < m_texFenceValue) {
    m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
    WaitForSingleObject(m_texFenceEvent, 10000);
  }
  void* mp = nullptr; D3D12_RANGE rr{0, size_t(tb)};
  if (FAILED(m_dumpReadback->Map(0, &rr, &mp))) return;
  const uint32_t w = uint32_t(fp.Footprint.Width), h = uint32_t(fp.Footprint.Height);
  std::vector<uint8_t> bgra(size_t(w) * h * 4u);
  // ⛔⛔ `nonBlackPx` IS A CONSTANT AND WAS QUOTED AS EVIDENCE ALL THROUGH
  // iter-446. The back buffer is cleared to {0.04,0.04,0.1} = bytes (10,10,26),
  // and this test is `px[0] | px[1] | px[2]` — so an EMPTY navy frame reports
  // 921600/921600, exactly what a perfect movie frame reports. "the frames are
  // not blank, nonBlackPx=921600/921600" proved nothing whatsoever. It is kept
  // only so old logs stay comparable; the three counters beside it are the ones
  // that can actually fail.
  //   pxNotClear — pixels that differ from THIS frame's clear colour. 0 means
  //                the frame is empty, whatever `nonBlack` says.
  //   distinct   — populated buckets of a 4:4:4 histogram. A cleared frame is 1.
  //   fnv        — content hash, so two frames can be compared from the log
  //                alone without diffing BMPs.
  uint64_t nonBlack = 0, pxNotClear = 0;
  std::vector<uint8_t> hist(4096, 0);
  uint64_t fnv = 1469598103934665603ull;
  const uint8_t cr = m_lastClearRgba8[0], cg = m_lastClearRgba8[1],
                cb = m_lastClearRgba8[2];
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* row = static_cast<const uint8_t*>(mp) + size_t(y) * fp.Footprint.RowPitch;
    for (uint32_t x = 0; x < w; x++) {
      const uint8_t* px = row + size_t(x) * 4;
      uint8_t* dp = bgra.data() + (size_t(y) * w + x) * 4;
      dp[0] = px[2]; dp[1] = px[1]; dp[2] = px[0]; dp[3] = 255;
      if (px[0] | px[1] | px[2]) nonBlack++;
      // px is BGRA from the readback; the latched clear is RGB.
      if (px[2] != cr || px[1] != cg || px[0] != cb) pxNotClear++;
      hist[((px[2] >> 4) << 8) | ((px[1] >> 4) << 4) | (px[0] >> 4)] = 1;
      fnv = (fnv ^ px[0]) * 1099511628211ull;
      fnv = (fnv ^ px[1]) * 1099511628211ull;
      fnv = (fnv ^ px[2]) * 1099511628211ull;
    }
  }
  uint32_t distinct = 0;
  for (uint8_t v : hist) distinct += v;
  m_dumpReadback->Unmap(0, nullptr);
#if SR_ORACLE
  holdLogPresent(pn, fnv, w, h);  // 2i diagnostic (SR_DIAG_HOLD_LOG)
#endif
  // Provenance ring trigger (director spec): EXACT full-frame RGB zero from the
  // EXISTING readback — nonBlack counts pixels with any nonzero RGB, so zero means
  // every pixel is exactly (0,0,0). NOT distinct==1 (a 4:4:4 histogram bucket
  // cannot prove exact uniformity). One-shot inside the ring.
#if SR_ORACLE
  if (sr::gfx::prov::enabled())
    sr::gfx::prov::ring().onBackbufferScan(nonBlack, (uint64_t)w * h);
#endif
  // iter-442: dumps default into archive/ (the gitignored evidence ground).
  // They used to land in the repo ROOT, which the hygiene rule forbids.
  // iter-462: the directory is created here - a user-played roll lost every
  // frame to a dump dir that did not exist yet (archive/v1_tape2).
  static const char* s_dir = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_DIR");
    const char* d = (e && *e) ? e : "archive";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
  }();
  // iter-455l r3: the BMP is written only on file-sampled presents; scan-only
  // presents (ring-driven, past the caps) still produced the counters above.
  if (fileWouldSample) {
    // iter-463 phase 2 (DEVTOOLS_DESIGN 3.1 A6): a dev-tool-assisted (or devtool-derived-save)
    // run tags its dump directory with a RUN_CLASS.json sidecar - the pixels cannot carry the
    // tag. Untagged (the default): nothing is written.
    static std::atomic<bool> s_runClassWritten{false};
    if (!s_runClassWritten.load(std::memory_order_relaxed)) {
      const std::string rc = sr::kernel::devToolsRunClassJson();
      if (!rc.empty()) {
        std::string p = std::string(s_dir) + "/RUN_CLASS.json";
        if (FILE* f = fopen(p.c_str(), "wb")) {
          fwrite(rc.data(), 1, rc.size(), f);
          fclose(f);
          s_runClassWritten.store(true, std::memory_order_relaxed);
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[RUN_CLASS] present-dump sidecar %s", p.c_str());
        }
      }
    }
    char nm[220];
    snprintf(nm, sizeof(nm), "%s/present_%s_%03llu_p%06llu_%ux%u.bmp", s_dir,
             uploaded ? "UP" : "cl",
             (unsigned long long)fileNo, (unsigned long long)pn, w, h);
    // iter-447: the success line prints ONLY when the file exists — this logged
    // 54 phantom dumps into a directory that was never created.
    // app/log-async (docs/TOOLKIT.md 3.2.3): this BMP write stays synchronous on the render thread;
    // one that takes > 100 ms is named by an [IO_STALL] line (a disk stall, not the game).
    // Step 4 item 4(c) (SR_PRESENT_DUMP_QUEUE=<n>, default 0 = this synchronous write): the BMP goes to one writer
    // thread instead (gfx/present_dump_writer.hpp; a full queue blocks here, nothing is dropped). The writer logs the
    // [PRESENT_DUMP] line once the file exists and keeps the [IO_STALL] line.
    if (dumpq::capacity()) {
      dumpq::Job job;
      job.path = nm;
      job.bgra = std::move(bgra);
      job.w = w;
      job.h = h;
      job.nonBlack = nonBlack;
      sr::kernel::HostWorkScope work(sr::kernel::kHostWorkDumpWrite);   // a full queue blocks in submit
      dumpq::submit(std::move(job), +[](const dumpq::Job& j) {
        return writeBmp32(j.path.c_str(), j.bgra.data(), j.w, j.h, j.w * 4u);
      });
    } else {
    const auto bmpT0 = std::chrono::steady_clock::now();
    bool bmpOk = false;
    {
      sr::kernel::HostWorkScope work(sr::kernel::kHostWorkDumpWrite);
      // Step 4 2k(4) test stimulus (diagnostic, default off): SR_PRESENT_DUMP_STALL_MS=<n> holds the FIRST
      // synchronous dump write n ms - a deterministic dump stall (the D: HDD's are intermittent) for the
      // watchdog's host-work classification A/B (SR_GPU_HANG_HOST_WORK). Compiled out of release (Astra 10:2x #7:
      // a stall stimulus is a test device; marker SR_PRESENT_DUMP_STALL_MS).
#if SR_ORACLE
      static const int s_stallMs = [] {
        const char* e = std::getenv("SR_PRESENT_DUMP_STALL_MS");
        const int v = e ? std::atoi(e) : 0;
        return v > 0 ? (v > 60000 ? 60000 : v) : 0;
      }();
      static std::atomic<bool> s_stalled{false};
      if (s_stallMs && !s_stalled.exchange(true)) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[PRESENT_DUMP] SR_PRESENT_DUMP_STALL_MS=%d: holding this dump write "
                    "%d ms (a test stimulus: the watchdog's host-work classification)", s_stallMs, s_stallMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(s_stallMs));
      }
#endif  // SR_ORACLE (SR_PRESENT_DUMP_STALL_MS)
      bmpOk = writeBmp32(nm, bgra.data(), w, h, w * 4);
    }
    sr::Log::noteIoStall("present_dump_bmp", 54ull + uint64_t(w) * h * 4u,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bmpT0).count());
    if (bmpOk)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PRESENT_DUMP] %s nonBlackPx=%llu/%u (⚠ constant — see the note)",
                  nm, (unsigned long long)nonBlack, w * h);
    }  // synchronous write
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PRESENT_SIG] p=%llu pxNotClear=%llu/%u distinct=%u fnv=%016llX "
              "clear=(%u,%u,%u)",
              (unsigned long long)pn, (unsigned long long)pxNotClear, w * h,
              distinct, (unsigned long long)fnv, cr, cg, cb);
}

#if SR_ORACLE
// iter-449s AUTOPSY: dump every live pool COLOR surface as a BMP. Same
// readback plumbing as dumpBackBuffer, but per pool surface — the presented
// backbuffer only ever shows the front-blit result, so "did the demo land on
// the scene RT" needs its own oracle. Diagnostic; called from replay teardown.
void D3D12Backend::hleDumpSurfaces(const char* dir) {
  if (!dir || !*dir) return;
  if (!m_readbackBuffer || !m_texUploadList || !m_texUploadAlloc) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SURF_DUMP] SKIPPED: readback plumbing unavailable");
    return;
  }
  for (auto& [key, s] : m_hleSurfaces) {
    if (s.isDepth || !s.res) continue;
    D3D12_RESOURCE_DESC rd = s.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT nr = 0;
    UINT64 rs = 0, tb = 0;
    m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &nr, &rs, &tb);
    if (tb > size_t(m_readbackRowPitch) * 2u * m_height) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SURF_DUMP] %08X %ux%u SKIPPED: exceeds readback capacity",
                  key, s.width, s.height);
      continue;
    }
    if (FAILED(m_texUploadAlloc->Reset())) return;
    if (FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) return;
    resourceBarrier(m_texUploadList.Get(), s.res.Get(), s.state,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION cd{}, cs{};
    cd.pResource = m_readbackBuffer.Get();
    cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    cd.PlacedFootprint = fp;
    cs.pResource = s.res.Get();
    cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cs.SubresourceIndex = 0;
    m_texUploadList->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
    resourceBarrier(m_texUploadList.Get(), s.res.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE, s.state);
    m_texUploadList->Close();
    ID3D12CommandList* l[] = {m_texUploadList.Get()};
    m_commandQueue->ExecuteCommandLists(1, l);
    m_texFenceValue++;
    m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
    if (m_texFence->GetCompletedValue() < m_texFenceValue) {
      m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
      WaitForSingleObject(m_texFenceEvent, 10000);
    }
    void* mp = nullptr;
    D3D12_RANGE rr{0, size_t(tb)};
    if (FAILED(m_readbackBuffer->Map(0, &rr, &mp))) continue;
    const uint32_t w = uint32_t(fp.Footprint.Width),
                   h = uint32_t(fp.Footprint.Height);
    std::vector<uint8_t> bgra(size_t(w) * h * 4u);
    uint64_t notBlack = 0;
    std::vector<uint8_t> hist(4096, 0);
    // iter-449u INSTRUMENT FIX: this loop read EVERY surface at 4 B/px. The
    // fmt-3 scene RT is R16G16B16A16_FLOAT = 8 B/px, so the dump reinterpreted
    // half-float mantissa/exponent bytes as unorm8 channels — MANUFACTURING
    // the entire "psychedelic palette" evidence set (banded skies = exponent
    // plateaus; the iter-408 "dumper read k_8 as float" class again).
    // Half-floats are decoded and tonemapped (clamp) to 8-bit here.
    const bool isHalf = rd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto halfToU8 = [](uint16_t hf) -> uint8_t {
      const uint32_t sgn = hf >> 15, ex = (hf >> 10) & 0x1F, mn = hf & 0x3FF;
      if (sgn || ex == 0) return 0;              // negative/denorm -> 0
      const float v = std::ldexp(1.0f + mn / 1024.0f, (int)ex - 15);
      return v >= 1.0f ? 255u : (uint8_t)(v * 255.0f + 0.5f);
    };
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t* row = static_cast<const uint8_t*>(mp) +
                           size_t(y) * fp.Footprint.RowPitch;
      for (uint32_t x = 0; x < w; x++) {
        uint8_t r8, g8, b8;
        if (isHalf) {
          const uint16_t* px = reinterpret_cast<const uint16_t*>(
              row + size_t(x) * 8);
          r8 = halfToU8(px[0]); g8 = halfToU8(px[1]); b8 = halfToU8(px[2]);
        } else {
          const uint8_t* px = row + size_t(x) * 4;
          r8 = px[0]; g8 = px[1]; b8 = px[2];
        }
        uint8_t* dp = bgra.data() + (size_t(y) * w + x) * 4;
        dp[0] = b8; dp[1] = g8; dp[2] = r8; dp[3] = 255;
        if (r8 | g8 | b8) notBlack++;
        hist[((r8 >> 4) << 8) | ((g8 >> 4) << 4) | (b8 >> 4)] = 1;
      }
    }
    uint32_t distinct = 0;
    for (uint8_t v : hist) distinct += v;
    m_readbackBuffer->Unmap(0, nullptr);
    char nm[220];
    snprintf(nm, sizeof(nm), "%s/surf_%08X_%ux%u%s.bmp", dir, key, s.width,
             s.height, s.everDrawn ? "" : "_neverdrawn");
    if (writeBmp32(nm, bgra.data(), w, h, w * 4))
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SURF_DUMP] %s notBlack=%llu/%u distinct=%u drawn=%d",
                  nm, (unsigned long long)notBlack, w * h, distinct,
                  s.everDrawn ? 1 : 0);
  }
  // iter-449y: ALSO dump resolve-product TEXTURES (m_hleTextures fromResolve).
  // The black-screen forensics need the content of the resolved HDR scene
  // (key 0C168000 class): the surface loop above cannot see it — a resolve
  // dest is a texture, and every post pass samples it. Same readback path;
  // fp16 decoded by the same corrected helper.
  // iter-450e: SR_HLE_TEX_DUMP=<hex,hex,...> ALSO dumps listed UPLOADED
  // textures (the every-world-draw shared-texture suspects). BC formats
  // decode wrong as pixels here, so a format-independent rawNonzero count
  // is logged for the black-or-not verdict.
  static const std::set<uint32_t> s_texDumpKeys = [] {
    std::set<uint32_t> ks;
    if (const char* e = std::getenv("SR_HLE_TEX_DUMP")) {
      // Separator-agnostic: the roll harness's -Gates string splits on
      // commas, so the key list arrives '+'-separated. Skip any non-hex
      // run between keys.
      const char* p = e;
      while (*p) {
        char* end = nullptr;
        const unsigned long v = strtoul(p, &end, 16);
        if (end == p) { p++; continue; }
        ks.insert((uint32_t)v & 0x1FFFFFFFu);
        p = end;
      }
    }
    return ks;
  }();
  for (uint32_t k : s_texDumpKeys) {
    auto it = m_hleTextures.find(k);
    if (it == m_hleTextures.end() || !it->second.res)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[SURF_DUMP] listed key %08X NOT in m_hleTextures (%zu entries)%s",
          k, m_hleTextures.size(),
          it != m_hleTextures.end() ? " (entry exists, res null)" : "");
  }
  for (auto& [key, t] : m_hleTextures) {
    const bool listed = s_texDumpKeys.count(key & 0x1FFFFFFFu) != 0;
    if ((!t.fromResolve && !listed) || !t.res) continue;
    D3D12_RESOURCE_DESC rd = t.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT nr = 0;
    UINT64 rs = 0, tb = 0;
    m_device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &nr, &rs, &tb);
    if (tb > size_t(m_readbackRowPitch) * 2u * m_height) continue;
    if (FAILED(m_texUploadAlloc->Reset())) return;
    if (FAILED(m_texUploadList->Reset(m_texUploadAlloc.Get(), nullptr))) return;
    resourceBarrier(m_texUploadList.Get(), t.res.Get(), t.state,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION cd{}, cs{};
    cd.pResource = m_readbackBuffer.Get();
    cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    cd.PlacedFootprint = fp;
    cs.pResource = t.res.Get();
    cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cs.SubresourceIndex = 0;
    m_texUploadList->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
    resourceBarrier(m_texUploadList.Get(), t.res.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE, t.state);
    m_texUploadList->Close();
    ID3D12CommandList* l[] = {m_texUploadList.Get()};
    m_commandQueue->ExecuteCommandLists(1, l);
    m_texFenceValue++;
    m_commandQueue->Signal(m_texFence.Get(), m_texFenceValue);
    if (m_texFence->GetCompletedValue() < m_texFenceValue) {
      m_texFence->SetEventOnCompletion(m_texFenceValue, m_texFenceEvent);
      WaitForSingleObject(m_texFenceEvent, 10000);
    }
    void* mp = nullptr;
    D3D12_RANGE rr{0, size_t(tb)};
    if (FAILED(m_readbackBuffer->Map(0, &rr, &mp))) continue;
    const uint32_t w = uint32_t(fp.Footprint.Width),
                   h = uint32_t(fp.Footprint.Height);
    std::vector<uint8_t> bgra(size_t(w) * h * 4u);
    uint64_t notBlack = 0;
    const bool isHalf = rd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto halfToU8 = [](uint16_t hf) -> uint8_t {
      const uint32_t sgn = hf >> 15, ex = (hf >> 10) & 0x1F, mn = hf & 0x3FF;
      if (sgn || ex == 0) return 0;
      const float v = std::ldexp(1.0f + mn / 1024.0f, (int)ex - 15);
      return v >= 1.0f ? 255u : (uint8_t)(v * 255.0f + 0.5f);
    };
    // iter-450e: format-independent truth for the black-or-not verdict —
    // BC formats decode wrong through the pixel path below, but a raw
    // nonzero-byte count over the copyable footprint cannot lie.
    uint64_t rawNonzero = 0;
    {
      const uint8_t* base = static_cast<const uint8_t*>(mp);
      for (uint64_t i = 0; i < tb; i++)
        if (base[i]) rawNonzero++;
    }
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t* row = static_cast<const uint8_t*>(mp) +
                           size_t(y) * fp.Footprint.RowPitch;
      for (uint32_t x = 0; x < w; x++) {
        uint8_t r8, g8, b8;
        if (isHalf) {
          const uint16_t* px = reinterpret_cast<const uint16_t*>(
              row + size_t(x) * 8);
          r8 = halfToU8(px[0]); g8 = halfToU8(px[1]); b8 = halfToU8(px[2]);
        } else {
          const uint8_t* px = row + size_t(x) * 4;
          r8 = px[0]; g8 = px[1]; b8 = px[2];
        }
        uint8_t* dp = bgra.data() + (size_t(y) * w + x) * 4;
        dp[0] = b8; dp[1] = g8; dp[2] = r8; dp[3] = 255;
        if (r8 | g8 | b8) notBlack++;
      }
    }
    m_readbackBuffer->Unmap(0, nullptr);
    char nm[220];
    snprintf(nm, sizeof(nm), "%s/tex_%08X_%ux%u_%s.bmp", dir, key, w, h,
             t.fromResolve ? "resolve" : "up");
    if (writeBmp32(nm, bgra.data(), w, h, w * 4))
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SURF_DUMP] %s notBlack=%llu/%u fmt=%d rawNonzero=%llu/%llu",
                  nm, (unsigned long long)notBlack, w * h, (int)rd.Format,
                  (unsigned long long)rawNonzero, (unsigned long long)tb);
  }
}

// iter-445 C4: open the frame's command list and put it in the state a
// recorder can use. Mirrors the reference's BeginCommandList
// (video.cpp:1557-1633) in SHAPE only — see the honesty note on the two binds.
void D3D12Backend::beginFrame() {
  // iter-446 S2: per-frame state. m_hleFrameStateSet makes flushHleFrameState
  // idempotent within the frame; m_hleFrameRecordedInOrder tells
  // renderHleDraws that the draws are already in the list.
  m_hleFrameStateSet = false;
  m_hleFrameRecordedInOrder = false;
  if (!m_clInitLatched) {
    m_clInitLatched = true;
    m_clInitOpens = (uint32_t)m_clOpens;
    m_clInitCloses = (uint32_t)m_clCloses;
    m_clInitExecutes = (uint32_t)m_clExecutes;
  }
  m_clSiteHits[FrameOpenAlloc]++;
  // iter-445 C7 PRE-BAKE FIX (audit): STAMP the epoch here rather than have the
  // guard guess `m_frameEpoch + 1` at its construction. Under the gate the
  // window is already open by then - hle::execute() opens it at the head of the
  // very call that dispatches Present - so the guard's expected epoch was off
  // by one and it could never fire: C3's protection was structurally dead in
  // exactly the configuration C7 bakes.
  m_openEpoch = ++m_frameEpoch;
  clReset(FrameOpen);
  if (m_clState != ClState::Recording) return;   // clReset already logged
  m_frameRT = m_renderTargets[m_frameIndex].Get();
  // The backbuffer transition that the no-framebuffer branch used to record
  // itself. It moves here because under the open list the branch is no longer
  // the first thing that touches the list in a frame.
  resourceBarrier(m_commandList.Get(), m_frameRT,
                  D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  // ⚠ GUARDED: createXlatPipeline() can fail and leave all three null, and the
  // harness VEH makes a host AV fatal before any SEH filter runs — check,
  // never fault-and-catch.
  // ⚠ HONESTY NOTE: these two mirror video.cpp:1623-1630, but they are INERT
  // today, because renderHleDraws re-sets both itself before it draws. Do not
  // cite them as verified reference fidelity — they are here so that a command
  // issued from execute() has a usable binding state once anything consumes it.
  if (m_xlatRootSig && m_xlatSrvHeap && m_xlatSamplerHeap) {
    ID3D12DescriptorHeap* heaps[2] = {m_xlatSrvHeap.Get(), m_xlatSamplerHeap.Get()};
    m_commandList->SetDescriptorHeaps(2, heaps);
    m_commandList->SetGraphicsRootSignature(m_xlatRootSig.Get());
    xlatSetPixelPosScale();  // iter-464 resscale (root param 14)
  }
  // ── iter-446 S2: THE COLOUR CLEAR MOVES HERE, and it is MANDATORY ─────────
  // A draw recorded at stream position lands EARLY in the frame list. The tail
  // clear in presentFramebuffer would then be recorded AFTER it and erase it -
  // the frame would present as an empty clear and the draw would be invisible
  // while every counter still said it was recorded. So under the gate the clear
  // happens at frame open, which is also the reference shape (video.cpp
  // BeginCommandList sets g_renderTarget = g_backBuffer up front).
  // The tail clear is suppressed to match; see presentFramebuffer.
  if (drawInOrderEnabled() && m_rtvHeap) {
    float clearColor[4] = {0.04f, 0.04f, 0.1f, 1.0f};
    if (uint32_t splashRgba = getSplashOverlayColor()) {
      clearColor[0] = ((splashRgba >> 24) & 0xFF) / 255.0f;
      clearColor[1] = ((splashRgba >> 16) & 0xFF) / 255.0f;
      clearColor[2] = ((splashRgba >> 8) & 0xFF) / 255.0f;
      clearColor[3] = 1.0f;
    }
    m_lastClearRgba8[0] = (uint8_t)(clearColor[0] * 255.0f + 0.5f);
    m_lastClearRgba8[1] = (uint8_t)(clearColor[1] * 255.0f + 0.5f);
    m_lastClearRgba8[2] = (uint8_t)(clearColor[2] * 255.0f + 0.5f);
    m_lastClearRgba8[3] = 255;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += m_frameIndex * m_rtvDescriptorSize;
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_frameOpenClears++;
  }
}

// Lazy, idempotent, and called from BOTH the head of hle::execute() and the head
// of presentFramebuffer — whoever gets there first opens the window.
//
// ⚠ NO SELF-ENQUEUE: endFrame deliberately does NOT open the next frame. The
// reference's ProcPresent ends by ENQUEUEING BeginCommandList (video.cpp:2853)
// but that runs on the GUEST thread and only queues work; doing it inline on
// our render thread would make Recording the steady state at presentFramebuffer's
// exit, and C3's FrameGuard would then fire on every normal present.
//
// ⚠ THE ownPresent PRECONDITION IS NOT OPTIONAL: with SR_HLE_OWN_PRESENT=0 the
// MAIN thread presents while the render thread still runs execute(). That is
// safe today only because nothing in execute() touches the command list — a
// lazy open at execute()'s head would have the render thread Reset and hold open
// the very list the main thread is recording into.
// ── iter-448: OFFSCREEN→SCREEN ROUTING, the final hop ────────────────────────
// The guest's Swap names its front buffer; the front-buffer object's embedded
// fetch constant names the pixel memory; that memory is a RESOLVE DESTINATION
// we populated; this copies it onto the backbuffer. Every step of that chain is
// the guest's own decision — we only follow the pointer the hardware scanout
// would have followed. Gated SR_HLE_PRESENT_FRONT — BAKED DEFAULT-ON since
// iter-449o (=0 keeps the A/B); it REPLACES the backbuffer content and was
// eyeballed in before baking.
bool D3D12Backend::hlePresentFront(uint32_t key, uint32_t dims) {
  (void)dims;
  // Refuse BY NAME — shown=0 with every reason invisible cost a roll.
  static uint64_t s_rNoList = 0, s_rNoTex = 0;
  if (m_clState != ClState::Recording || !m_frameRT || !m_commandList) {
    if (++s_rNoList <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_FRONT] REFUSED blit: clState=%d frameRT=%d list=%d",
          (int)m_clState, m_frameRT ? 1 : 0, m_commandList ? 1 : 0);
    return false;
  }
  auto it = m_hleTextures.find(key);
  if (it == m_hleTextures.end() || !it->second.res) {
    if (++s_rNoTex <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_FRONT] REFUSED blit: key=%08X not in m_hleTextures (%zu entries)",
          key, m_hleTextures.size());
    return false;
  }
  HleTex& t = it->second;
  // Exact-match CopyResource ONLY. A mismatch is refused BY NAME (the census
  // shows HIT without shown), never stretched or reinterpreted: a scaled or
  // format-punned present renders plausibly and is wrong.
  if (t.w != uint32_t(m_width) || t.h != uint32_t(m_height) ||
      t.fmt != DXGI_FORMAT_R8G8B8A8_UNORM) {
    static uint64_t s_mm = 0;
    if (++s_mm <= 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[S6_FRONT] REFUSED blit: tex %ux%u fmt=%u vs backbuffer %dx%d",
          t.w, t.h, (unsigned)t.fmt, m_width, m_height);
    return false;
  }
  if (t.state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    resourceBarrier(m_commandList.Get(), t.res.Get(), t.state,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    t.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  resourceBarrier(m_commandList.Get(), m_frameRT,
                  D3D12_RESOURCE_STATE_RENDER_TARGET,
                  D3D12_RESOURCE_STATE_COPY_DEST);
  m_commandList->CopyResource(m_frameRT, t.res.Get());
  resourceBarrier(m_commandList.Get(), m_frameRT,
                  D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_RENDER_TARGET);
  // Back to sampleable — later draws in the next frame may bind it.
  resourceBarrier(m_commandList.Get(), t.res.Get(),
                  D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  t.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  return true;
}

void D3D12Backend::ensureFrameOpen() {
  if (!m_initialized || !frameListEnabled()) return;
  if (!sr::gfx::hle::ownPresentEnabled()) {
    static bool s_said = false;
    if (!s_said) {
      s_said = true;
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[FRAME_LIST] INCOMPATIBLE GATES: SR_HLE_FRAME_LIST=1 requires the "
          "render thread to own present (SR_HLE_OWN_PRESENT). Frame list "
          "DISABLED for this run.");
    }
    return;
  }
  if (m_clState == ClState::Recording) return;   // already open — idempotent
  beginFrame();
}

void D3D12Backend::presentFramebuffer(const uint8_t* data, int fbWidth, int fbHeight, int fbPitchBytes,
                                      bool tiled) {
  if (!m_initialized) return;

  // iter-444 S6f: retire the staged draw list on EVERY exit from this function.
  // Four early returns below and the whole upload branch never reach
  // renderHleDraws(); without this the batches accumulate for ever and the
  // ring cursors never reset. RAII so a future fifth exit cannot forget.
  // ── iter-445 C3: FrameGuard — the same RAII, given a second job ───────────
  // Today the open window is only the ~450 lines between the frame open and the
  // frame tail, and it contains no live early exit — which is the ONLY reason
  // the unchecked Resets here have never bitten. Commit 4 widens that window to
  // span hle::execute(), and from that point ANY exit from this function leaves
  // the frame's command list OPEN: the next frame's Reset then throws away
  // whatever was recorded, silently, and the allocator is reset while the GPU
  // may still be reading it.
  //
  // ⚠⚠ IT CLOSES AND **DISCARDS** — IT MUST NEVER CLOSE AND EXECUTE. Under
  // commit 4 the frame open records the PRESENT->RENDER_TARGET barrier and the
  // frame tail records the matching RENDER_TARGET->PRESENT. Submitting a
  // half-recorded frame would put the forward transition on the GPU timeline
  // with no return transition: the backbuffer is genuinely left in
  // RENDER_TARGET and the next Present is a use-after-state-violation. A
  // dropped frame is a visible glitch; a submitted half-frame is a corrupted
  // device. Discarding is the conservative failure.
  //
  // EPOCH-KEYED so it cannot double-close: the frame open bumps m_frameEpoch,
  // and the guard acts only if the epoch it captured is still current AND the
  // list is still Recording. A normal exit closes through the frame tail, which
  // sets state Closed, so the guard sees nothing to do.
  struct FrameGuard {
    D3D12Backend* be;
    bool recorded;
    uint64_t epoch;
    ~FrameGuard() {
      be->hleRetireFrame(recorded);
      if (be->m_clState == ClState::Recording && be->m_openEpoch == epoch) {
        be->m_clDiscards++;
        if (be->m_clDiscards <= 4)
          SR_LOG_BOOT(sr::LogSeverity::Error,
              "[FRAME_LIST] ABNORMAL EXIT #%llu: frame list still recording at "
              "epoch %llu — closing and DISCARDING (never submitting: a "
              "half-recorded frame leaves the backbuffer in RENDER_TARGET)",
              (unsigned long long)be->m_clDiscards,
              (unsigned long long)epoch);
        be->clClose(GuardDiscard);   // Close only. No ExecuteCommandLists.
      }
    }
  } hleRetire{this, false, m_openEpoch};   // stamped by beginFrame, not guessed

  // iter-358j: the 360 system front buffer is Xenos-TILED (fetch dword0 bit 31;
  // live: 0x8AB7F000 1280x720 k_8_8_8_8 tiled=1). A linear read of it is
  // scrambled — untile into a scratch buffer first (same address math Xenia's
  // texture-load shaders use). The tiled source span is validated against the
  // guest allocation HERE (fault-free); the scratch then bypasses the
  // guest-range containment check below (it is host memory by construction).
  bool srcIsUntileScratch = false;
  // The resolve destination is ALWAYS the guest framebuffer, even after the
  // untile below redirects `data` to host scratch for the upload path.
  uint8_t* guestFbDest = const_cast<uint8_t*>(data);
  const int guestFbPitch = fbPitchBytes;
  // iter-437 (re-applying the 436g finding, now with its root cause known):
  // the tiled flag is the CONSOLE's internal resolve->scanout contract, but in
  // our pipeline BOTH sides are platform-owned — the writer is our linear
  // resolve write-back (copyRenderTargetToGuestFramebuffer, now fed per-frame
  // by the HLE Resolve hook) and the reader is this present. Linear-in,
  // linear-out; untiling linear bytes is the 436f/g checkerboard scramble.
  // SR_FB_UNTILE=1 restores the untile for A/B.
  static const bool s_fbUntile = [] {
    const char* e = std::getenv("SR_FB_UNTILE");
    return e && e[0] == '1';
  }();
  if (s_fbUntile && tiled && data && fbWidth > 0 && fbHeight > 0 && fbPitchBytes > 0) {
    static std::vector<uint8_t> s_untileScratch;  // main-thread only, like s_hostImg
    const uint32_t pitchTexels = static_cast<uint32_t>(fbPitchBytes) / 4u;
    uint8_t* gbase = nullptr; size_t gsz = 0;
    sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
    const size_t span = xenos_tiling::tiledSpanBytes32(pitchTexels, static_cast<uint32_t>(fbHeight));
    const bool pitchOk = pitchTexels >= static_cast<uint32_t>(fbWidth) && (pitchTexels % 32u) == 0u;
    if (gbase && gsz && pitchOk && data >= gbase && data + span <= gbase + gsz) {
      s_untileScratch.resize(static_cast<size_t>(fbWidth) * fbHeight * 4u);
      xenos_tiling::untile32(s_untileScratch.data(), data, pitchTexels,
                             static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight));
      data = s_untileScratch.data();
      fbPitchBytes = fbWidth * 4;
      srcIsUntileScratch = true;
    } else {
      static std::atomic<uint32_t> s_n{0};
      if (s_n.fetch_add(1, std::memory_order_relaxed) < 4)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[D3D12] tiled framebuffer span invalid — skipping present "
          "(data=%p span=0x%zX pitchTexels=%u guest=[%p,+0x%zX))",
          (const void*)data, span, pitchTexels, (void*)gbase, gsz);
      data = nullptr;  // fall through to placeholder/clear path
    }
  }

  {  // iter-408b: latch the displayed surface for the resolve tally.
    uint32_t va = 0; int gw = 0, gh = 0, gp = 0; bool gt = false;
    if (sr::kernel::getGuestFramebufferInfo(&va, &gw, &gh, &gp, &gt) && va)
      m_guestFrontPhys = va & 0x1FFFFFFFu;
  }

  static std::atomic<uint32_t> s_diagPresentCount{0};
  uint32_t presentN = s_diagPresentCount.fetch_add(1, std::memory_order_relaxed);
  bool diagLog = (presentN < 90u || (presentN % 300u == 0));

  // iter-439 P2: settle async resolve flushes whose fence has passed EVERY
  // present, not just drains — otherwise the last flush before a batch pause
  // never converts and the guest fb present path reads stale bytes forever.
  completeInFlightResolves(false);

  // ── iter-445 C4: THE FRAME OPEN, GATE-AWARE ───────────────────────────────
  // GATE ON: the window may ALREADY be open — hle::execute() opens it lazily at
  // its own head, which is the entire point (commands issued from execute() must
  // land in this frame's list). ensureFrameOpen is idempotent.
  // GATE OFF: byte-for-byte the pre-C4 behaviour.
  if (frameListEnabled()) {
    ensureFrameOpen();
  } else {
    // iter-445 C1: THE FRAME OPEN. Both HRESULTs were discarded here; clReset
    // checks them and refuses to report Recording unless both succeeded. The
    // init epoch (CreateCommandList + createXlatPipeline) is latched on the
    // first frame open so the ledger can report per-frame deltas rather than
    // totals that fold a one-off startup epoch into every frame.
    if (!m_clInitLatched) {
      m_clInitLatched = true;
      m_clInitOpens = (uint32_t)m_clOpens;
      m_clInitCloses = (uint32_t)m_clCloses;
      m_clInitExecutes = (uint32_t)m_clExecutes;
    }
    m_clSiteHits[FrameOpenAlloc]++;
    m_frameEpoch++;
    clReset(FrameOpen);
  }
  m_frameRT = m_renderTargets[m_frameIndex].Get();

  auto* rt = m_renderTargets[m_frameIndex].Get();

  // Host present-image override (iter-273): if set, blit the host-rendered REAL
  // press-start screen (decoded from ScrPressStart.AST) and present immediately,
  // ahead of all guest-geometry / test-pattern / splash paths. Highest precedence.
  {
    static std::vector<uint8_t> s_hostImg;
    int hiW = 0, hiH = 0;
    // iter-445 C4: SKIPPED under the frame-list gate — this branch Closes and
    // Executes the list on its own schedule and returns, which the open window
    // cannot survive. SR_HOST_PRESENT_IMAGE is therefore INERT while the gate
    // is on (stated in the commit message, not just here).
    if (!frameListEnabled() &&
        getHostPresentImage(s_hostImg, hiW, hiH) && !s_hostImg.empty()) {
      void* mapped = nullptr;
      D3D12_RANGE readRange{0, 0};
      if (SUCCEEDED(m_uploadBuffer->Map(0, &readRange, &mapped))) {
        int copyW = (hiW < m_width) ? hiW : m_width;
        int copyH = (hiH < m_height) ? hiH : m_height;
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        for (int y = 0; y < copyH; y++) {
          std::memcpy(dst + static_cast<size_t>(y) * m_width * 4,
                      s_hostImg.data() + static_cast<size_t>(y) * hiW * 4,
                      static_cast<size_t>(copyW) * 4);
        }
        m_uploadBuffer->Unmap(0, nullptr);
      }
      resourceBarrier(m_commandList.Get(), rt,
                      D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION srcLoc{};
      srcLoc.pResource = m_uploadBuffer.Get();
      srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      srcLoc.PlacedFootprint.Offset = 0;
      srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      srcLoc.PlacedFootprint.Footprint.Width = static_cast<UINT>(m_width);
      srcLoc.PlacedFootprint.Footprint.Height = static_cast<UINT>(m_height);
      srcLoc.PlacedFootprint.Footprint.Depth = 1;
      srcLoc.PlacedFootprint.Footprint.RowPitch =
          static_cast<UINT>((m_width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
      D3D12_TEXTURE_COPY_LOCATION dstLoc{};
      dstLoc.pResource = rt;
      dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dstLoc.SubresourceIndex = 0;
      m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
      resourceBarrier(m_commandList.Get(), rt,
                      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
      clClose(HostImgClose);     // iter-445 C1
      clExecute(HostImgExec);
      dumpThenOverlay(rt);   // Capture before the host cursor overlay.
      presentSwap();
      moveToNextFrame();
      return;
    }
  }

  // If guest is submitting draw commands, try to render guest geometry.
  auto& stats = getDrawStats();
  uint32_t pendingSubmits = stats.drawSubmits.load(std::memory_order_relaxed);
  uint32_t queueSizeBeforeDrain = getDrawCommandQueue().size();
  bool resolveRequested = getResolveRequested();  // Peek for diagnostics; clear later in resolve path.

  if (diagLog) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[D3D_DIAG] present #%u queueSize=%u resolveReq=%d pendingSubmits=%u data=%p fb=%dx%d pitch=%d",
      presentN, queueSizeBeforeDrain, resolveRequested ? 1 : 0, pendingSubmits,
      (void*)data, fbWidth, fbHeight, fbPitchBytes);
  }
  // iter-369g FRAME BOUNDARY. The console's frame boundary is VdSwap; this
  // present runs on main.cpp's free ~60Hz loop, which is unrelated to the guest's
  // frame rate. Draining every tick meant one render could carry many swaps'
  // worth of commands — measured at 1888 batches / 1707 resolve batches in a
  // SINGLE render, against 68 resolve events for the whole run. Every one of
  // those resolve batches flushes the GPU and reads the render target back, so
  // the frame being composited was continually torn down mid-assembly.
  //
  // Drain only when the guest has actually completed a swap (getSwapFrameSeq
  // advanced). This is console semantics, not forcing: no guest function is
  // called, no guest state written — we just stop rendering at moments the guest
  // never asked us to.
  // SR_PRESENT_ON_SWAP=0 restores the old drain-every-tick behaviour for A/B.
  static const bool s_frameBoundary = [] {
    const char* e = std::getenv("SR_PRESENT_ON_SWAP");
    return !e || e[0] != '0';
  }();
  if (m_geoPipelineReady) {
    static uint64_t s_lastSwapSeq = ~0ull;
    const uint64_t seq = getSwapFrameSeq();
    // iter-438e PRESSURE RELIEF: the swap-paced drain (16/s) capped consumption
    // at ~130K cmds/s; the ident-era draw storm (8.3K/s, bursty) pinned the
    // queue at 8191 and every producer push then burst-waited 100ms INSIDE the
    // guest device CS — starving the asset-loader thread ([CRITSEC_STALL]) so
    // the load never finished and the ident looped forever (the feFrames=7
    // boot wedge). Drain also whenever the queue passes 25% — drainDrawQueue's
    // own boundary logic keeps renders frame-coherent either way.
    const bool pressure =
        getDrawCommandQueue().size() >= DrawCommandQueue::kCapacity / 4;
    if (!s_frameBoundary || seq != s_lastSwapSeq || pressure) {
      s_lastSwapSeq = seq;
      const uint32_t qBefore = getDrawCommandQueue().size();
      {
        const int64_t tD0 = profQpc();
        drainDrawQueue();
        profDrainUs.fetch_add(profUs(profQpc() - tD0), std::memory_order_relaxed);
      }
      // iter-409: the OTHER half of the draw-flow census. Pushes are counted in
      // pm4_engine; this counts what a drain actually consumed and how many
      // batches survived translation. A gap between pushed and consumed is a
      // drop; a gap between consumed and batches is a translation reject.
      static std::atomic<uint64_t> s_drains{0}, s_consumed{0}, s_batches{0}, s_maxQ{0};
      const uint64_t dn = s_drains.fetch_add(1, std::memory_order_relaxed) + 1;
      s_consumed.fetch_add(qBefore, std::memory_order_relaxed);
      s_batches.fetch_add(m_drawBatches.size(), std::memory_order_relaxed);
      uint64_t prevMax = s_maxQ.load(std::memory_order_relaxed);
      while (qBefore > prevMax &&
             !s_maxQ.compare_exchange_weak(prevMax, qBefore, std::memory_order_relaxed)) {}
      if (dn % 200 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[DRAW_FLOW] drains=%llu consumed=%llu batches=%llu maxQueueSeen=%llu/%u",
                    (unsigned long long)dn,
                    (unsigned long long)s_consumed.load(std::memory_order_relaxed),
                    (unsigned long long)s_batches.load(std::memory_order_relaxed),
                    (unsigned long long)s_maxQ.load(std::memory_order_relaxed),
                    DrawCommandQueue::kCapacity);
    }
  }

  if (pendingSubmits > 0) {
    static bool s_loggedOnce = false;
    if (!s_loggedOnce) {
      s_loggedOnce = true;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12] First draw submits detected (%u)", pendingSubmits);
    }
  }

  // iter-358k: probePendingDraws (deferred pool re-scans) deleted — draws
  // decode from their own packet + register snapshot or are skipped.

  // iter-359g ATTRIBUTION FIX: the old gate keyed on the FIXED-path vertex
  // counter only — with every draw translated (SR_XLAT_ONLY) it was 0 and the
  // whole render was silently skipped; the dumped "render" was the trace's own
  // restored framebuffer content. Batches of any kind must trigger the render.
  // iter-445 C4: SKIPPED under the gate — this branch reaches
  // flushPendingResolves' mid-frame Close/Execute/Reset via renderGuestGeometry.
  // ⚠ COMPENSATING CLEANUP REQUIRED: it is the ONLY caller of
  // renderGuestGeometry (which is what clears m_drawBatches) while
  // drainDrawQueue() keeps running unconditionally — skipping it without the
  // clear would grow m_drawBatches without bound.
  if (frameListEnabled() && (m_totalGeoVerts > 0 || !m_drawBatches.empty())) {
    static bool s_said = false;
    if (!s_said) {
      s_said = true;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[FRAME_LIST] LEGACY BRANCH SKIPPED site=geometry (batches=%zu "
          "verts=%u) — clearing them so the queue cannot grow unbounded",
          m_drawBatches.size(), m_totalGeoVerts);
    }
    // iter-445 C7 PRE-BAKE FIX (audit): COUNT the drop. One static Warn per
    // process is a silent drop by this project's own standard (hleRetireFrame
    // counts every retired batch), and [DRAW_FLOW] upstream keeps crediting
    // these same batches as translated.
    m_geoBatchesDropped += m_drawBatches.size();
    m_geoVertsDropped += m_totalGeoVerts;
    m_drawBatches.clear();
    m_totalGeoVerts = 0;
    (void)getAndClearResolveRequested();
  }
  if (!frameListEnabled() &&
      (m_totalGeoVerts > 0 || !m_drawBatches.empty()) && m_geoPipelineReady) {
    static bool s_loggedGeo = false;
    if (!s_loggedGeo) {
      s_loggedGeo = true;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D12] Rendering guest geometry: %u verts, %zu batches",
                  m_totalGeoVerts, m_drawBatches.size());
    }
    // iter-359h: when the frame's batches carried mode-6 resolves, the pass
    // wiring already wrote the front buffer with the console's byte contract
    // (fmt6/swap1/endian0 = B,G,R,A) — the legacy end-of-frame copy here would
    // OVERWRITE it with legacy R,G,B,A bytes. Compute before render (which
    // clears the batch list); still consume the request flag either way.
    bool hadResolves = false;
    for (const auto& b : m_drawBatches)
      if (b.resolve) { hadResolves = true; break; }
    {
      const int64_t tR0 = profQpc();
      renderGuestGeometry();
      profRenderUs.fetch_add(profUs(profQpc() - tR0), std::memory_order_relaxed);
    }
    // iter-437c: the drain-cost profile. iter-438d: TIME-based (every 5s) —
    // the geo-present counter gating never fired in eras that present through
    // the non-geometry path (ident/menu: 0 PRESENT_GEO in a whole run), which
    // hid the profile exactly when the drain collapsed to 3 resolves/s.
    {
      static std::atomic<uint32_t> s_profN{0};
      static std::atomic<int64_t> s_lastProf{0};
      const uint32_t pn = s_profN.fetch_add(1, std::memory_order_relaxed) + 1;
      const int64_t nowQ = profQpc();
      int64_t last = s_lastProf.load(std::memory_order_relaxed);
      const bool timeDue = (nowQ - last) > 5 * profQpcFreq();
      if (timeDue) s_lastProf.store(nowQ, std::memory_order_relaxed);
      if (pn % 300 == 0 || timeDue)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[DRAIN_PROF] presents=%u resolves=%llu renderMs=%llu "
            "resolveWaitMs=%llu convertMs=%llu evictMs=%llu snapshotMs=%llu "
            "drainMs=%llu blitMs=%llu presentMs=%llu paceMs=%llu "
            "texBindMs=%llu bindFlushN=%llu",
            pn,
            (unsigned long long)profResolveN.load(std::memory_order_relaxed),
            (unsigned long long)(profRenderUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profResolveWaitUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profResolveConvUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profEvictUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profSnapshotUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profDrainUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profBlitUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profPresentUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profPaceUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)(profTexBindUs.load(std::memory_order_relaxed) / 1000),
            (unsigned long long)profBindFlushN.load(std::memory_order_relaxed));
    }
    const bool resolveReq = getAndClearResolveRequested();
    if (resolveReq && !hadResolves && guestFbDest && fbWidth > 0 && fbHeight > 0 && guestFbPitch > 0) {
      // Resolve into the ORIGINAL guest framebuffer (not the untile scratch
      // `data` may have been redirected to) — front-buffer byte contract.
      copyRenderTargetToGuestFramebuffer(guestFbDest, guestFbPitch, 720, 0x01000300u);
      // iter-358: capture the frames where the GAME'S OWN geometry actually
      // landed. The 120/240/360s timer misses them entirely — the geometry burst
      // is ~4s into boot — so every timed shot showed an empty frame and read as
      // "we draw nothing". Dump the first 3 real-geometry resolves instead.
      // First 3 catch the ~4s boot burst; every 100th thereafter catches the LATE
      // frames — the menu only exists after the FMV ladder + catalog populate
      // (~60-120s), and a shot budget spent entirely on boot would miss it. The
      // vert count is in the filename so a glance shows which stage it came from.
      static std::atomic<uint32_t> s_geoShot{0};
      uint32_t gs = s_geoShot.fetch_add(1, std::memory_order_relaxed);
      // Tag with BOTH vert count and BATCH count: a dump labelled "56verts" while
      // m_drawBatches was empty is what let a fabricated test-triangle overlay
      // pass as guest geometry. batches=0 means nothing of the game's was drawn,
      // whatever the vert counter says.
      if (gs < 3 || (gs % 100) == 0) {
        char why[64];
        snprintf(why, sizeof(why), "geo%u_%uverts_%zubatches", gs + 1,
                 m_totalGeoVerts, m_drawBatches.size());
        sr::kernel::dumpGuestFramebufferTagged(why);
      }
      if (diagLog)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[D3D_DIAG] present #%u resolve copy_done -> guest fb %dx%d pitch=%d", presentN, fbWidth, fbHeight, fbPitchBytes);
    } else {
      clClose(GeoBranchClose);   // iter-445 C1
      clExecute(GeoBranchExec);
    }
    // iter-410: overwrite the m_geoRT copy with the guest FRONT BUFFER, which
    // is what the console actually scans out (see the Xenia note above). The
    // geometry render still ran — its resolves are what filled that front
    // buffer — we just stop showing the intermediate instead of the result.
    // iter-435d HYBRID (the "black press-start window" fix): frames whose
    // batches all routed to the HDR surface + resolves leave the LDR RT at
    // clear-navy — present the guest front buffer for those (the UI composite
    // the resolves produced; proven by present dumps: p3000 shows the title
    // artwork). Frames that drew the LDR RT directly (movies) keep the live
    // RT copy — full-fb-always presenting turned movies into a stale-frame
    // slideshow with black flashes (the "very fucked up" run).
    if (presentFrontBufferEnabled() || !m_frameDrewToLdr) {
      static std::atomic<uint64_t> s_fb{0}, s_fbFail{0};
      const int64_t tB0 = profQpc();
      const bool ok = blitGuestFramebufferToBackBuffer(guestFbDest, fbWidth, fbHeight,
                                                       guestFbPitch, srcIsUntileScratch, rt);
      profBlitUs.fetch_add(profUs(profQpc() - tB0), std::memory_order_relaxed);
      const uint64_t n = (ok ? s_fb.fetch_add(1, std::memory_order_relaxed)
                             : s_fbFail.fetch_add(1, std::memory_order_relaxed)) + 1;
      if (n <= 3 || n % 300 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PRESENT_FRONTBUF] %s ok=%llu failed=%llu (fb=%p %dx%d pitch=%d)",
                    ok ? "blit" : "BLIT-FAILED",
                    (unsigned long long)s_fb.load(std::memory_order_relaxed),
                    (unsigned long long)s_fbFail.load(std::memory_order_relaxed),
                    (const void*)guestFbDest, fbWidth, fbHeight, guestFbPitch);
    }
    dumpThenOverlay(rt);   // iter-464: the dump, then the host overlay, then Present
    {
      const int64_t tP0 = profQpc();
      presentSwap();
      const int64_t tP1 = profQpc();
      moveToNextFrame();
      profPresentUs.fetch_add(profUs(tP1 - tP0), std::memory_order_relaxed);
      profPaceUs.fetch_add(profUs(profQpc() - tP1), std::memory_order_relaxed);
    }
    return;
  }

  // Splash overlay takes precedence over guest framebuffer when active —
  // routes the present through the no-fb clear path so the per-logo tint is
  // visible. Set by kernel/splash_hooks.cpp during the EA→DC→WBIE sequence.
  const bool splashOverlayActive = (getSplashOverlayColor() != 0u);

  // iter-408b: uncapped tally of WHICH tail branch runs. The screen was flat
  // for 92/92 sampled presents while the front buffer was measurably receiving
  // content 2900+ times, so the two ends disagree and only the branch actually
  // taken can reconcile them.
  {
    static std::atomic<uint64_t> s_upload{0}, s_clear{0};
    const bool willUpload = data && fbWidth > 0 && fbHeight > 0 && !splashOverlayActive;
    m_lastPresentUploaded = willUpload;
    const uint64_t n = (willUpload ? s_upload.fetch_add(1, std::memory_order_relaxed)
                                   : s_clear.fetch_add(1, std::memory_order_relaxed)) + 1;
    // First few of EACH kind plus a periodic tick: a pure every-300th sample
    // never showed the transition into the upload path, because presents slow
    // down sharply once real geometry starts rendering.
    // Log EVERY present that arrives with real framebuffer data. Those are rare
    // by construction, and a sampled counter cannot show one that lands between
    // its sample points — which is exactly how a call that main.cpp definitely
    // made ("First present: fbVA=0x8A940000") left no trace here at all.
    if (n <= 5 || n % 300 == 0 || data)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PRESENT_PATH] upload=%llu clear=%llu (data=%s %dx%d splashOverlay=%d)",
                  (unsigned long long)s_upload.load(std::memory_order_relaxed),
                  (unsigned long long)s_clear.load(std::memory_order_relaxed),
                  data ? "yes" : "NULL", fbWidth, fbHeight, splashOverlayActive ? 1 : 0);
  }
  // iter-445 C4: SKIPPED under the gate (MEASURED DEAD: upload=0 of 3900
  // presents, and structurally unreachable from presentPlaceholder's nullptr).
  // Falling through to the else-arm is the whole point — that arm is where
  // renderHleDraws records, and it is the branch the fixture and the live boot
  // both take.
  if (!frameListEnabled() && data && fbWidth > 0 && fbHeight > 0 && !splashOverlayActive) {
    // Copy guest framebuffer data to the render target via upload buffer.
    // Map upload buffer, copy pixel data, then CopyTextureRegion from buffer to RT.
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    if (SUCCEEDED(m_uploadBuffer->Map(0, &readRange, &mapped))) {
      int copyWidth = (fbWidth < m_width) ? fbWidth : m_width;
      int copyHeight = (fbHeight < m_height) ? fbHeight : m_height;

      // iter-358h PLATFORM FIX — unbounded read of the guest framebuffer.
      //
      // `data`, fbHeight and fbPitchBytes all come from the GUEST (VdSwap tells us
      // where its front buffer is). We then read fbHeight * fbPitchBytes bytes with
      // no check that the span is inside our guest allocation. Crash:
      //   [CRASH] Exception 0xC0000005  ... mov dl, byte ptr [rax+rcx]
      //   = this loop's `src[x*4+1]`, src walked past the end of guest memory.
      //
      // This bug is OLD. It never fired while setupFakeGpuDevice() fabricated a
      // front buffer at a known-good address — the fake was MASKING it. Deleting
      // the fake (iter-358f) let the game's own framebuffer pointer through, and
      // the missing bounds check surfaced immediately. A real console's GPU reads
      // real memory; we must never read outside the guest allocation.
      //
      // Fault-FREE clamp (HARD RULE: the harness VEH makes any host AV fatal
      // before SEH filters run — validate, never fault-and-catch).
      {
        // getGuestBase()/getGuestSize() are THREAD-LOCAL (set via setGuestMemory
        // per guest-running thread). The D3D12 present thread is a host thread and
        // never calls setGuestMemory, so they read [0,0) here — a first cut of this
        // check used them and silently rejected EVERY upload, "fixing" the crash by
        // disabling the framebuffer path. Use the PROCESS-GLOBAL accessor.
        uint8_t* gbase = nullptr; size_t gsz = 0;
        sr::kernel::getGlobalGuestMemory(&gbase, &gsz);
        const uint8_t* gb = gbase;
        // The untile scratch is host memory, validated at untile time — the
        // guest-range containment check does not apply to it.
        if (!srcIsUntileScratch &&
            (!gb || !gsz || fbPitchBytes <= 0 || data < gb || data >= gb + gsz)) {
          m_uploadBuffer->Unmap(0, nullptr);
          static std::atomic<uint32_t> s_n{0};
          if (s_n.fetch_add(1, std::memory_order_relaxed) < 4)
            SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[D3D12] guest framebuffer OUT OF RANGE — skipping upload "
              "(data=%p guest=[%p,%p) %dx%d pitch=%d)",
              (const void*)data, (const void*)gb, (const void*)(gb + gsz),
              fbWidth, fbHeight, fbPitchBytes);
          return;
        }
        // Clamp rows to what is actually mapped: the last row we touch reads
        // bytes [ (h-1)*pitch, (h-1)*pitch + copyWidth*4 ).
        const size_t avail = srcIsUntileScratch
            ? static_cast<size_t>(fbHeight) * static_cast<size_t>(fbPitchBytes)
            : static_cast<size_t>((gb + gsz) - data);
        const size_t rowBytes = static_cast<size_t>(copyWidth) * 4u;
        int maxRows = 0;
        if (avail >= rowBytes)
          maxRows = static_cast<int>((avail - rowBytes) / static_cast<size_t>(fbPitchBytes)) + 1;
        if (copyHeight > maxRows) {
          static std::atomic<uint32_t> s_n{0};
          if (s_n.fetch_add(1, std::memory_order_relaxed) < 4)
            SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[D3D12] guest framebuffer overruns guest memory — clamping %d -> %d rows "
              "(data=%p avail=0x%zX pitch=%d)",
              copyHeight, maxRows, (const void*)data, avail, fbPitchBytes);
          copyHeight = maxRows;
        }
        if (copyHeight <= 0 || copyWidth <= 0) { m_uploadBuffer->Unmap(0, nullptr); return; }
      }
      // iter-359h (recon wf_161cf206, Xenia source contract): the 360 front
      // buffer is LE_X8R8G8B8 — bytes [B][G][R][A] ascending in guest RAM
      // (LE word 0xAARRGGBB; display fetch swiz 0xA0A reads R<-byte2 G<-byte1
      // B<-byte0). Convert to RGBA for DXGI_FORMAT_R8G8B8A8_UNORM.
      uint8_t* dst = static_cast<uint8_t*>(mapped);
      for (int y = 0; y < copyHeight; y++) {
        const uint8_t* src = data + y * fbPitchBytes;
        uint8_t* dstRow = dst + y * copyWidth * 4;
        for (int x = 0; x < copyWidth; x++) {
          dstRow[x * 4 + 0] = src[x * 4 + 2]; // R
          dstRow[x * 4 + 1] = src[x * 4 + 1]; // G
          dstRow[x * 4 + 2] = src[x * 4 + 0]; // B
          dstRow[x * 4 + 3] = src[x * 4 + 3]; // A
        }
      }
      m_uploadBuffer->Unmap(0, nullptr);
    }

    // Transition RT to COPY_DEST, copy from upload buffer, then transition to PRESENT.
    resourceBarrier(m_commandList.Get(), rt,
                    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = m_uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width = static_cast<UINT>(m_width);
    srcLoc.PlacedFootprint.Footprint.Height = static_cast<UINT>(m_height);
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch =
        static_cast<UINT>((m_width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                          ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = rt;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    resourceBarrier(m_commandList.Get(), rt,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  } else {
    // No framebuffer: clear to splash overlay color (per-logo) when active,
    // otherwise dark blue. The splash hook in kernel/splash_hooks.cpp sets
    // the overlay via setSplashOverlayColor() when each splash basename
    // arrives — gives a visible per-logo screen tint during boot.
    // iter-445 C4: under the gate beginFrame() already recorded this
    // transition — recording it twice is an unbalanced-barrier error the debug
    // layer reports and the GPU may or may not forgive.
    if (!frameListEnabled())
      resourceBarrier(m_commandList.Get(), rt,
                      D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_frameIndex * m_rtvDescriptorSize;
    float clearColor[4] = {0.04f, 0.04f, 0.1f, 1.0f};
    // Latched so dumpBackBuffer can tell "cleared" from "drawn" — the whole
    // reason nonBlackPx was useless is that it could not.
    if (uint32_t splashRgba = getSplashOverlayColor()) {
      clearColor[0] = ((splashRgba >> 24) & 0xFF) / 255.0f;
      clearColor[1] = ((splashRgba >> 16) & 0xFF) / 255.0f;
      clearColor[2] = ((splashRgba >>  8) & 0xFF) / 255.0f;
      clearColor[3] = 1.0f;
    }
    // iter-445 C5: under probe mode 2 this clear is SUPPRESSED, so the probe's
    // magenta is the last clear before renderHleDraws' draws. Deleted with the
    // probe in C7.
    // iter-445 C7 PRE-BAKE FIX (audit blocker 8): recordGuard at a recording
    // point NOT trivially adjacent to the open. After C7 deletes the C5 probe,
    // recordGuard would have exactly one call site sitting right beside
    // ensureFrameOpen, and `violations == 0` would degenerate into a restatement
    // of `resetFails == 0` - a criterion with no demonstrated falsifier.
    recordGuard(FrameTailClose);
    // iter-446 S2: with the in-order gate on this clear has ALREADY happened,
    // at frame open, BEFORE any stream-position draw. Repeating it here would
    // erase every such draw. Exactly one clear per frame either way - counted
    // on both sides so "which clear ran" is never a guess.
    if (!drawInOrderEnabled()) {
      // Latch the ACTUAL clear tuple (after any splash override) so
      // dumpBackBuffer's pxNotClear compares against what was really cleared.
      m_lastClearRgba8[0] = (uint8_t)(clearColor[0] * 255.0f + 0.5f);
      m_lastClearRgba8[1] = (uint8_t)(clearColor[1] * 255.0f + 0.5f);
      m_lastClearRgba8[2] = (uint8_t)(clearColor[2] * 255.0f + 0.5f);
      m_lastClearRgba8[3] = 255;
      m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
      m_frameTailClears++;
    }

    // ── iter-442 S4.6: the movie quad, recorded HERE ────────────────────────
    // This is the branch both the replay fixture and the live default boot
    // take, the RT is already RENDER_TARGET, and the frame list is open (reset
    // at the top of this function). Drawing into m_geoRT instead would be the
    // documented silent-black trap: it only reaches the screen through
    // renderGuestGeometry, which is gated on draw batches this era never has.
    hleRetire.recorded = renderHleDraws();

    recordGuard(FrameTailClose);   // iter-445 C7 (audit blocker 8)
    resourceBarrier(m_commandList.Get(), rt,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  }

  // iter-358k: the default-path "always resolve" round-trip (upload guest fb →
  // RT → write RT back over the guest fb) is DELETED. It existed because
  // resolveRequested was never raised naturally; EVENT_WRITE_EXT now raises it
  // and the resolve belongs solely to the geometry path — with the resolve
  // source retargeted to the offscreen RT, the old block would have clobbered
  // CPU-written framebuffer content (FMV frames) with stale geometry output.
  {
    clClose(FrameTailClose);   // iter-445 C1
    clExecute(FrameTailExec);
  }

  dumpThenOverlay(rt);   // iter-464: the dump, then the host overlay, then Present
  // iter-449s AUTOPSY: SR_HLE_SURF_DUMP_AT=<present#> dumps every live pool
  // surface ONCE at that present, on the render thread, into SR_HLE_SURF_DUMP
  // (dir). Live pixel truth per stage — the presented backbuffer only shows
  // the front-blit result.
  {
    static const long long s_surfAt = [] {
      const char* e = std::getenv("SR_HLE_SURF_DUMP_AT");
      return e ? std::atoll(e) : -1;
    }();
    static std::atomic<bool> s_surfDone{false};
    static std::atomic<uint64_t> s_surfPn{0};
    const uint64_t pn = s_surfPn.fetch_add(1, std::memory_order_relaxed);
    // iter-449y: drain the resolve write-back queue every present (the
    // exposure chain's guest-memory feed; see serviceResolveWriteback).
    serviceResolveWriteback();
    // iter-450c: drain the luminance probe (SR_LUM_PROBE diagnostic).
    serviceLumProbe();
    if (s_surfAt >= 0 && (long long)pn >= s_surfAt &&
        !s_surfDone.exchange(true)) {
      const char* sd = std::getenv("SR_HLE_SURF_DUMP");
      hleDumpSurfaces(sd && *sd ? sd : "archive");
    }
  }
  // iter-445 C4: CAPTURE THE HRESULT. A removed/reset device leaves the whole
  // ledger perfectly balanced while nothing reaches the screen — the exact
  // "every metric green, screen black" trap this sequence exists to avoid.
  {
    const HRESULT hp = presentSwap();
    if (FAILED(hp) && !m_deviceRemoved) {
      m_deviceRemoved = true;
      const HRESULT reason =
          m_device ? m_device->GetDeviceRemovedReason() : S_OK;
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[FRAME_LIST] PRESENT FAILED hr=0x%08X deviceRemovedReason=0x%08X — "
          "the ledger will keep balancing while NOTHING reaches the screen",
          (unsigned)hp, (unsigned)reason);
    }
  }
  moveToNextFrame();
}

void D3D12Backend::presentPlaceholder(uint64_t frameCount) {
  presentFramebuffer(nullptr, 0, 0, 0);
}
#endif

// iter-464 PORT FEATURE (docs/RESOLUTION.md): window resize. The main thread
// records the size; the presenting thread (pm4RenderFrame - the CP or the render
// thread) waits for the GPU, releases the backbuffers and resizes before it
// records its next frame. The PM4 present fits the frame to whatever size this is.
void D3D12Backend::requestResize(int w, int h) {
  if (w <= 0 || h <= 0 || !pm4LiveEnabled()) return;
  m_pendingResize.store((uint64_t(uint32_t(w)) << 32) | uint32_t(h), std::memory_order_release);
}

void D3D12Backend::applyPendingResize() {
  const uint64_t v = m_pendingResize.exchange(0, std::memory_order_acq_rel);
  if (!v || !m_swapChain) return;
  const uint32_t w = uint32_t(v >> 32), h = uint32_t(v);
  if (w == m_bbW && h == m_bbH) return;
  waitForGpu();
  for (int i = 0; i < kFrameCount; i++) {
    m_renderTargets[i].Reset();
    m_fenceValues[i] = m_fenceValues[m_frameIndex];
  }
  const HRESULT hr = m_swapChain->ResizeBuffers(kFrameCount, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, m_swapChainFlags);
  if (FAILED(hr))
    SR_LOG_BOOT(sr::LogSeverity::Error, "[DISPLAY] ResizeBuffers %ux%u failed 0x%08X", w, h, unsigned(hr));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (int i = 0; i < kFrameCount; i++) {
    if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])))) continue;
    m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtv);
    rtv.ptr += m_rtvDescriptorSize;
  }
  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
  if (SUCCEEDED(hr)) {
    m_bbW = w;
    m_bbH = h;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[DISPLAY] swapchain resized to %ux%u", m_bbW, m_bbH);
}

void D3D12Backend::waitForGpu() {
  if (!m_commandQueue) return;
  uint64_t ticket = 0;
  HRESULT hr = m_nativeImages.checkpoint(ticket);
  if (FAILED(hr)) nativeRecordingFail("queue-checkpoint", hr);
  hr = m_nativeImages.wait(ticket, 10000);
  if (FAILED(hr)) nativeRecordingFail("queue-wait", hr);
  hr = m_nativeImages.collect();
#if SR_ORACLE
  readerObserveCompletion();
#endif
  if (FAILED(hr)) nativeRecordingFail("queue-collect", hr);
  hdrProbeCollect();
}

HRESULT D3D12Backend::presentSwap() {
  const UINT sync = sr::kernel::highfps::presentSyncInterval();
  const UINT flags =
      (sync == 0 && m_allowTearing && sr::kernel::highfps::wantTearing()) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
  const uint64_t hitchT0 = sr::hitch::begin();
  HRESULT hr = m_swapChain->Present(sync, flags);
  sr::hitch::end(sr::hitch::kPresentCall, hitchT0);
  if (hr == DXGI_ERROR_INVALID_CALL && flags) {
    // DXGI refuses ALLOW_TEARING in exclusive full screen (sync 0 tears there anyway): stop
    // asking and present this frame without it.
    m_allowTearing = false;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[HIGHFPS] Present refused ALLOW_TEARING (exclusive full screen?) - sync-0 presents without it");
    hr = m_swapChain->Present(sync, 0u);
  }
  sr::kernel::highfps::onHostPresent();   // [HIGHFPS_PACE] host cadence (no-op unless pace logging)
  sr::perf::onHostPresent();              // iter-464 perf: SR_PERF_STATS present stamps (no-op when off)
  sr::hitch::onHostPresent();             // iter-466 stutter: SR_HITCH_LOG (no-op when off)
  return hr;
}

void D3D12Backend::moveToNextFrame() {
  UINT64 currentFenceValue = m_fenceValues[m_frameIndex];
  const HRESULT signaled = m_commandQueue->Signal(m_fence.Get(), currentFenceValue);
  if (FAILED(signaled)) nativeRecordingFail("frame-signal", signaled);
  else wdNoteFrameSignal(currentFenceValue);   // step 4 2k(4): the watchdog's causality probe
  m_lastSignaledFence = currentFenceValue;  // iter-461: this frame's completion marker

  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

  const uint64_t completed = m_fence->GetCompletedValue();
  if (completed == UINT64_MAX) nativeRecordingFail("frame-completion", DXGI_ERROR_DEVICE_REMOVED);
  if (completed < m_fenceValues[m_frameIndex]) {
    sr::hitch::Scope hs(sr::hitch::kGpuWait);
    waitNativeFence(m_fence.Get(), m_fenceValues[m_frameIndex], m_fenceEvent, "frame-wait");
  }
  m_fenceValues[m_frameIndex] = currentFenceValue + 1;

  // ⛔ AUDIT FIX (iter-446): free resources displaced by a re-create only once
  // the GPU has provably finished with them. Called HERE because this is the
  // one place a fence completion is already known.
  const HRESULT collected = m_nativeImages.collect();
#if SR_ORACLE
  readerObserveCompletion();
#endif
  if (FAILED(collected)) nativeRecordingFail("frame-collect", collected);
  const uint64_t retired = m_fence->GetCompletedValue();
  if (retired == UINT64_MAX) nativeRecordingFail("frame-retirement", DXGI_ERROR_DEVICE_REMOVED);
  drainRetired(retired);
  hdrProbeCollect();

  // GPU frame is now complete — notify the GPU IRQ thread so it signals the
  // guest GPU sync events exactly once (simulates GPU-done DPC after Present).
  notifyGpuFramePresented();
}

}  // namespace gfx
}  // namespace sr

#endif  // _WIN32
