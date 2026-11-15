// Host-only WARP fixture for the production resource upload/resolve ordering.
// No window or game execution is required. Two tiny SDK overrides exercise the
// actual End/UP completion dispatch; the cold-start case creates fixed pipelines.
//   difftest.exe --resource-memory-d3d12-test
// Negative wiring controls (each MUST exit 1 with a named byte mismatch):
//   ... --control=omit-preserve
//   ... --control=hoist-copy
//   ... --control=skip-copy
//   ... --control=missing-note (named CHECKER_REJECTION instead of byte mismatch)
//   difftest.exe --resource-memory-d3d12-test --ab   (mem.resources frozen-pass A/B: SR_AB_TOGGLE=SR_MEM_RESOURCES,
//   the cross-arm rules INV-M / INV-R / the dual rebaseline and the witness; compiled controls
//   -DSR_MEM_RES_AB_MUTANT_NOOP / _MODEL make it fail by name)
// These controls alter the fixture's call sequence, not the production bodies.
// The positive run also detects deletion of preservation, ticket invalidation,
// recorded-copy stamping, or per-batch cursor consumption in production.
#include "gfx/d3d12_backend.hpp"
#include "gfx/resource_memory.hpp"
#include "gfx/resource_ab.hpp"
#include "gfx/resource_memory_check.hpp"
#include "gfx/resource_census.hpp"
#include "gfx/shader_cache.hpp"
#include "gfx/render_draw_command.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/native_resource_writes.hpp"
#include "kernel/phys_window.hpp"
#include "kernel/quit.hpp"
#include "app/log.hpp"
#include "guest/ppc/ppc_recomp_shared.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" void __imp__sub_8279EC60(PPCContext& __restrict ctx, uint8_t* base);

namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kPage = 4096;
constexpr uint32_t kPhys = 0x00100000;
constexpr uint32_t kGuestBytes = 48 * kPage;  // the --ab cases use pages up to kPhys + 34 pages (32 left them uncommitted)
constexpr uint32_t kUploadBytes = 16 * kPage;
using Bytes = std::array<uint8_t, kPage>;
unsigned passed = 0, failed = 0;

void require(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    char message[192];
    std::snprintf(message, sizeof(message), "%s hr=%08lX", operation, (unsigned long)hr);
    throw std::runtime_error(message);
  }
}
void check(bool ok, const char* name) {
  std::printf("[RESOURCE_D3D12_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  std::fflush(stdout);  // a crashing case still shows the last check it passed (stdout is a pipe on the worker)
  ok ? ++passed : ++failed;
}
void checkBytes(const std::vector<uint8_t>& actual, const Bytes& expected, const char* name) {
  const bool same = actual.size() == expected.size() &&
                    std::equal(actual.begin(), actual.end(), expected.begin());
  if (!same) {
    const size_t count = std::min(actual.size(), expected.size());
    size_t at = 0;
    while (at < count && actual[at] == expected[at]) ++at;
    std::printf("[RESOURCE_D3D12_TEST] BYTE_MISMATCH %s offset=%zu actual=%d expected=%d size=%zu\n",
                name, at, at < actual.size() ? int(actual[at]) : -1,
                at < expected.size() ? int(expected[at]) : -1, actual.size());
  }
  check(same, name);
}
Bytes pattern(uint8_t seed) {
  Bytes bytes{};
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(seed + 37u * i + i / 251u);
  return bytes;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* device, uint64_t bytes,
                              D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = heap;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> resource;
  require(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state,
                                         nullptr, IID_PPV_ARGS(&resource)), "buffer");
  return resource;
}

// Reserve the normal address-space shape but commit only the fixture's pages.
// This mode is dispatched before difftest's general 4 GiB committed allocation.
class GuestPages {
 public:
  GuestPages() {
    constexpr size_t reservation = (size_t{1} << 32) + 8u;  // The native memcpy contract includes the alias guard.
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, reservation, MEM_RESERVE, PAGE_NOACCESS));
    if (!base_) throw std::runtime_error("guest reservation failed");
    for (uint32_t window : {0x80000000u, 0xA0000000u}) {
      if (!VirtualAlloc(base_ + window + kPhys, kGuestBytes, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(base_, 0, MEM_RELEASE);
        base_ = nullptr;
        throw std::runtime_error("guest page commit failed");
      }
    }
    sr::kernel::setGuestMemory(base_, reservation);
    // The real IB staging accessor validates against the process-wide arena,
    // just as it does after the game harness registers its thread runner.
    previousRunner_ = sr::kernel::getExCreateThreadRunner(&previousBase_, &previousSize_);
    sr::kernel::registerExCreateThreadRunner(nullptr, base_, reservation);
  }
  ~GuestPages() {
    sr::kernel::registerExCreateThreadRunner(previousRunner_, previousBase_, previousSize_);
    sr::kernel::setGuestMemory(nullptr, 0);
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
  }
  GuestPages(const GuestPages&) = delete;
  GuestPages& operator=(const GuestPages&) = delete;
 private:
  uint8_t* base_ = nullptr;
  sr::kernel::ExCreateThreadRunnerFn previousRunner_ = nullptr;
  uint8_t* previousBase_ = nullptr;
  size_t previousSize_ = 0;
};
}  // namespace

namespace sr::gfx {
// The only test privilege is device/storage setup and access to the SAME methods
// called by drainDrawQueue and renderPm4Batches. There is no alternate registry,
// copy recorder, generation protocol or snapshot implementation in this file.
struct ResourceRoutingTestAccess {
  [[noreturn]] static void failureContext(bool removeDevice) {
    // Isolated child only: use the real WARP device-removal API, then the real
    // fatal backend path. No guest arena, window, queue submission or game.
    sr::Log::init("native_failure.log");
    auto backend = std::make_unique<D3D12Backend>();
    ComPtr<IDXGIAdapter> warp;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&backend->m_factory)), "failure factory");
    require(backend->m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "failure WARP adapter");
    require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
                              IID_PPV_ARGS(&backend->m_device)), "failure WARP device");
    if (removeDevice) {
      ComPtr<ID3D12Device5> removable;
      require(backend->m_device.As(&removable), "real device removal interface");
      removable->RemoveDevice();
    }
    const HRESULT observed = backend->m_device->GetDeviceRemovedReason();
    if (FAILED(observed) != removeDevice) throw std::runtime_error("real removal state disagrees with fixture mode");
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE_TEST] removed=%u expected_reason=0x%08X expected_original=0x%08X",
                unsigned(removeDevice), unsigned(observed), unsigned(E_INVALIDARG));
    backend->nativeRecordingFail("failure-context-fixture", E_INVALIDARG);
  }

  explicit ResourceRoutingTestAccess(const std::string& control) : control_(control) {
    b_ = std::make_unique<D3D12Backend>();
    auto& b = *b_;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&b.m_device)), "WARP device");
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    require(b.m_device->CreateCommandQueue(&queue, IID_PPV_ARGS(&b.m_commandQueue)), "queue");
    require(b.m_nativeImages.initialize(b.m_device.Get(), b.m_commandQueue.Get(), 4, 0), "queue ownership");
    b.m_shm = buffer(b.m_device.Get(), kPhys + kGuestBytes, D3D12_HEAP_TYPE_DEFAULT,
                     D3D12_RESOURCE_STATE_COMMON);
    b.m_shmUpload = buffer(b.m_device.Get(), kUploadBytes, D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
    b.m_shmReadback = buffer(b.m_device.Get(), kUploadBytes, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    require(b.m_shmUpload->Map(0, &noRead, &mapped), "upload mapping");
    b.m_shmUploadPtr = static_cast<uint8_t*>(mapped);
    b.m_shmEpochOf.assign(memres::kPages, 0);
    b.m_shmStamp.assign(memres::kPages, 0);
    b.m_shmGpuWritten.assign(memres::kPages, 0);
    b.m_shmResolveMask.assign(memres::kPages, 0);
    b.m_shmReady = true;
    b.clReset(D3D12Backend::Pm4Reset);
  }
  ~ResourceRoutingTestAccess() {
    if (!b_) return;
    b_->pm4ShmSetDrainMode(false, false);
    // Successful paths have waited at each readback. Even on an exception, do
    // not release submitted resources before the owned queue's bounded drain.
    const HRESULT hr = b_->m_nativeImages.shutdown(10000);
    if (FAILED(hr)) {
      std::fprintf(stderr, "[RESOURCE_D3D12_TEST] CLEANUP_FAILED hr=%08lX\n", (unsigned long)hr);
      std::fflush(nullptr);
      std::_Exit(2);
    }
    if (b_->m_shmUploadPtr) {
      b_->m_shmUpload->Unmap(0, nullptr);
      b_->m_shmUploadPtr = nullptr;
    }
  }

  // mem-native: an EXACT CPU-write patch (the D3D header fence stamp, kernel/native_resource_stamps.hpp) on a
  // partially resolved page. The GPU owns [offset, offset + length); a header dword elsewhere on the page is
  // stamped with no coherency record. The next request copies exactly that dword: the resolve's bytes survive,
  // the stamp reaches the arena and the page stays the GPU's (no page upload over the resolve).
  // --control=skip-patch omits the note: the stamp never reaches the arena (the byte check fails).
  void exactPatch() {
    constexpr uint32_t phys = kPhys + 13 * kPage, offset = 1024, length = 256, fence = 0x218;
    auto& b = *b_;
    beginCase(phys);
    auto& memory = memres::resourceMemory();
    const auto original = pattern(0x51), resolved = pattern(0xC3);
    cpu(phys, original);
    capture(phys);
    uint32_t cursor = 0;
    record(batch(), cursor);
    const auto generation = memory.reserveGpu();
    prepareResolve(phys, offset, length, generation);
    record(batch(), cursor);
    gpuCopy(phys, resolved, offset, length);
    publishGpu(phys, offset, length, generation);
    auto* guest = kernel::guestVAToHost(0x80000000u + phys);
    if (!guest) throw std::runtime_error("fixture guest pointer missing");
    const uint8_t stamp[4] = {0x00, 0x00, 0x4D, 0x17};  // a fence value, big-endian
    std::memcpy(guest + fence, stamp, 4);
    const uint32_t cpuBefore = memory.cpuGeneration(phys >> 12), clockBefore = memory.generation();
    if (control_ != "skip-patch") memory.notePatch(0x80000000u + phys + fence, 4u);
    check(memory.cpuGeneration(phys >> 12) == cpuBefore && memory.generation() == clockBefore,
          "exact_patch_reserves_no_generation");
    const size_t pendingBefore = b.m_shmPending.size();
    capture(phys);
    check(control_ == "skip-patch" ||
              (b.m_shmPending.size() == pendingBefore + 1 && b.m_shmPending.back().patch &&
               b.m_shmPending.back().shmOff == phys + fence && b.m_shmPending.back().bytes == 4u),
          "exact_patch_queues_exactly_its_dword");
    record(batch(), cursor);
    Bytes expected = original;
    std::copy_n(resolved.begin() + offset, length, expected.begin() + offset);
    std::copy_n(stamp, 4, expected.begin() + fence);
    checkBytes(snapshot(phys), expected, "exact_patch_keeps_gpu_bytes_and_reaches_the_arena");
    check(memory.prepare(phys >> 12).decision == memres::Decision::kKeepGpu && b.m_shmGpuWritten[phys >> 12] == 1,
          "exact_patch_never_makes_the_gpu_written_page_cpu_owned");
    const size_t pendingAfter = b.m_shmPending.size();
    capture(phys);
    check(b.m_shmPending.size() == pendingAfter, "exact_patch_is_copied_once");
  }

  // mem-native: the request's same-drain memo (--fast-path: no checker, so the lock-free keep and the memo run).
  void memoCase() {
    constexpr uint32_t phys = kPhys + 14 * kPage;
    auto& b = *b_;
    auto& memory = memres::resourceMemory();
    beginCase(phys);
    cpu(phys, pattern(0x2D));
    capture(phys);
    const size_t afterUpload = b.m_shmPending.size();
    capture(phys);
    capture(phys);
    check(afterUpload == 1 && b.m_shmPending.size() == afterUpload && b.m_shmEpochOf[phys >> 12] == b.m_shmEpoch,
          "memo_current_range_queues_nothing_and_keeps_the_drain_stamp");
    // perf parity: a note in another 64-page block leaves the range's memo standing (the per-block change sequence);
    // a note in the range's own block (another page of it) does not
    const uint64_t hits0 = b.m_resourceRangeMemoHits;
    memory.noteCpu(0x00800000u, kPage, memres::Src::kOther);  // page 0x800: block 0x20, far from the range's
    capture(phys);
    check(b.m_resourceRangeMemoHits == hits0 + 1 && b.m_shmPending.size() == afterUpload,
          "memo_survives_a_note_in_another_block");
    memory.noteCpu(kPhys + 20 * kPage, kPage, memres::Src::kOther);  // the range's own block, another page
    capture(phys);
    check(b.m_resourceRangeMemoHits == hits0 + 1 && b.m_shmPending.size() == afterUpload,
          "memo_rewalks_after_a_note_in_its_own_block_and_finds_the_page_current");
    memory.notePatch(0x80000000u + phys + 0x40u, 4u);
    capture(phys);
    check(b.m_shmPending.size() == afterUpload + 1 && b.m_shmPending.back().patch &&
              b.m_shmPending.back().shmOff == phys + 0x40u,
          "memo_never_hides_a_pending_patch");
    cpu(phys, pattern(0x2E));
    capture(phys);
    check(b.m_shmPending.size() == afterUpload + 2 && !b.m_shmPending.back().patch &&
              b.m_shmPending.back().bytes == kPage,
          "memo_never_hides_a_page_note");
    const auto older = memory.reserveGpu();
    cpu(phys, pattern(0x2F));  // newer than the reserved resolve
    capture(phys);
    capture(phys);
    const size_t beforeGpu = b.m_shmPending.size();
    memory.noteGpuAt(phys, 64u, older);  // the deferred publication resets the newer CPU page's upload
    capture(phys);
    check(b.m_shmPending.size() == beforeGpu + 1 && b.m_shmPending.back().bytes == kPage,
          "memo_never_hides_a_deferred_gpu_publication");
    ++b.m_shmEpoch;
    capture(phys);
    check(b.m_shmEpochOf[phys >> 12] == b.m_shmEpoch && b.m_shmPending.size() == beforeGpu + 1,
          "memo_restamps_a_new_drain_epoch_without_a_copy");
    uint32_t cursor = 0;
    record(batch(), cursor);
    checkBytes(snapshot(phys), pattern(0x2F), "memo_case_final_bytes_are_the_last_cpu_write");
  }

  // step 6 I4 (SR_CP_RES_REQ_LEAN, default ON) --cp-res-req: the memo answers before m_shmMu, and a draw's asked
  // texture spans share one hold for their misses. memoCase's rules run on the lean path; a hit must complete while
  // another thread holds m_shmMu.
  void cpResReq() {
    memoCase();
    auto& b = *b_;
    constexpr uint32_t phys = kPhys + 15 * kPage;
    beginCase(phys);
    cpu(phys, pattern(0x61));
    capture(phys);  // the walk: the range is current and memoised
    {
      std::mutex m;
      std::condition_variable cv;
      bool held = false, release = false;
      std::thread holder([&] {
        std::unique_lock<std::recursive_mutex> shm(b.m_shmMu);
        std::unique_lock<std::mutex> g(m);
        held = true;
        cv.notify_all();
        cv.wait_for(g, std::chrono::seconds(10), [&] { return release; });
      });
      {
        std::unique_lock<std::mutex> g(m);
        cv.wait(g, [&] { return held; });
      }
      const uint64_t hits = b.m_cpResReq.hitsNoLock;
      auto asked = std::async(std::launch::async, [&] { capture(phys); });
      const bool done = asked.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
      {
        std::lock_guard<std::mutex> g(m);
        release = true;
      }
      cv.notify_all();
      holder.join();
      asked.get();
      check(done && b.m_cpResReq.hitsNoLock == hits + 1, "i4_memo_hit_takes_no_lock");
    }
    // (b) a draw's asked texture spans (pm4ResourceRequestSite): the misses share one hold, a current site takes none
    constexpr uint32_t a = kPhys + 16 * kPage, c = kPhys + 17 * kPage, d = kPhys + 18 * kPage;
    for (uint32_t p : {a, c, d}) cpu(p, pattern(uint8_t(p >> 12)));
    const memres::ResourceSpan spans[3] = {{a, kPage}, {c, kPage}, {d, kPage}};
    const auto site = [&] {
      b.pm4ShmSetDrainMode(false, true);
      try {
        b.pm4ResourceRequestSite(spans, 3);
      } catch (...) {
        b.pm4ShmSetDrainMode(false, false);
        throw;
      }
      b.pm4ShmSetDrainMode(false, false);
    };
    const auto before = b.m_cpResReq;
    const size_t pending = b.m_shmPending.size();
    site();
    check(b.m_cpResReq.holds == before.holds + 1 && b.m_cpResReq.lockedRanges == before.lockedRanges + 3 &&
              b.m_shmPending.size() == pending + 3,
          "i4_site_misses_share_one_hold");
    const auto mid = b.m_cpResReq;
    site();
    check(b.m_cpResReq.holds == mid.holds && b.m_cpResReq.hitsNoLock == mid.hitsNoLock + 3 &&
              b.m_shmPending.size() == pending + 3,
          "i4_current_site_takes_no_hold");
    uint32_t cursor = 0;
    record(batch(), cursor);
    checkBytes(snapshot(d), pattern(uint8_t(d >> 12)), "i4_site_bytes_reach_the_arena");
  }

  // step 6 I4 --cp-res-req-check (SR_CP_RES_REQ_CHECK=1): every lock-free hit is re-decided under the lock.
  void cpResReqCheck() {
    auto& b = *b_;
    auto& memory = memres::resourceMemory();
    constexpr uint32_t phys = kPhys + 19 * kPage, forged = kPhys + 21 * kPage;
    beginCase(phys);
    cpu(phys, pattern(0x71));
    capture(phys);
    const auto before = b.m_cpResReq;
    capture(phys);
    check(b.m_cpResReq.hitsNoLock == before.hitsNoLock + 1 && b.m_cpResReq.checked == before.checked + 1 &&
              b.m_cpResReq.bad == 0 && b.m_cpResReq.raced == 0,
          "i4_check_admits_a_current_hit");
    // A memo forged for a noted page (never uploaded) with the blocks unchanged since: the hit is false - BAD, not RACED,
    // decided at the second drain end after it (a suspect until then).
    cpu(forged, pattern(0x72));
    b.m_resourceRangeMemo[D3D12Backend::resourceRangeMemoSlot(forged, kPage)] =
        D3D12Backend::ResourceRangeMemo{forged, kPage, b.m_shmEpoch, memory.sequence()};
    capture(forged);
    const bool deferred = b.m_cpResReq.bad == 0 && b.m_cpResReqSuspects.size() == 1;
    b.cpResReqCensus();
    b.cpResReqCensus();
    check(deferred && b.m_cpResReq.bad == 1 && b.m_cpResReq.raced == 0 && b.m_cpResReqSuspects.empty(),
          "i4_check_flags_a_false_memo_hit");
    // Astra I4 r1 P2: a note between its two stores (the page generation raised, its block sequence not yet) reads as a
    // false hit at the CHECK; the sequence landing before the verdict makes it RACED, not BAD.
    constexpr uint32_t late = kPhys + 23 * kPage;
    cpu(late, pattern(0x73));  // the generation (and its sequence: the memo below is forged after it)
    b.m_resourceRangeMemo[D3D12Backend::resourceRangeMemoSlot(late, kPage)] =
        D3D12Backend::ResourceRangeMemo{late, kPage, b.m_shmEpoch, memory.sequence()};
    const uint64_t raced = b.m_cpResReq.raced;
    capture(late);
    memory.noteCpu(late, kPage, memres::Src::kOther);  // the note's block sequence lands after the CHECK
    b.cpResReqCensus();
    b.cpResReqCensus();
    check(b.m_cpResReq.bad == 1 && b.m_cpResReq.raced == raced + 1 && b.m_cpResReqSuspects.empty(),
          "i4_check_calls_a_late_note_raced");
  }

  void fastPath() {
    exactPatch();
    memoCase();
    partialPage();
    olderResolveNewerCpu();
    queuedDraws();
  }

  void run() {
    // exactPatch runs in --fast-path: with the kept-page checker on, a patched page is an unchecked-coverage gap
    // by design (gfx/resource_memory_check.hpp resourceCheckPatched).
    partialPage();
    olderResolveNewerCpu();
    queuedDraws();
    inlineDraws();
    inlineReuse();
    resolveBatchWrapper();
    replayPoison();
    replayPoisonCoverage();
    refusedProducer();
    keptPageChecker();
    consumedFetch();
  }

  void consumerChecks() {
    // Real staging, immutable admission, pending-copy recording and GPU reads.
    // The readback is a synthetic consumer; this fixture does not claim shader
    // execution. The renderer finalizes these same tickets after real Draw*.
    constexpr uint32_t phys = kPhys + 26 * kPage, indexPhys = kPhys + 27 * kPage;
    constexpr uint64_t shaderHash = 0x7D00000000000003ull;
    constexpr uint32_t header = 0x218, payload = 0x600, neighbor = 0xC00;
    auto& b = *b_;
    kernel::physwin::noteHeapRoutedGrant(0x80000000u + phys, 2 * kPage);
    beginCase(phys);
    b.m_xlatVertices.init(4096, false);
    b.m_xlatConstants.init(32768, false);
    b.m_xlatVBPtr = b.m_xlatVertices.data();
    b.m_xlatCBPtr = b.m_xlatConstants.data();
    b.m_xlatCBUsed = 4096;
    b.m_vsfShmSrvReady = true;
    const uint32_t words[] = {1u | (1u << 12) | (1u << 16), 2u << 12, 0u,
                              (1u << 12) | (1u << 19), 0x688u | (36u << 16), 1u};
    std::vector<uint8_t> ucode(sizeof(words));
    for (size_t i = 0; i < std::size(words); ++i)
      for (uint32_t j = 0; j < 4; ++j) ucode[i * 4 + j] = uint8_t(words[i] >> (24 - 8 * j));
    // No census test override: the real consumer gate must retain shader input.
    memres::noteVsUcode(shaderHash, ucode.data(), ucode.size());
    std::memset(kernel::guestVAToHost(0x80000000u + indexPhys), 0, 4);
    GuestDrawCommand command{};
    command.vsUcodeHash = shaderHash; command.vertexCount = 1; command.primType = 4;
    command.idxBasePhys = indexPhys; command.idxFmt32 = true; command.idxEndian = 2;
    CachedShader shader; shader.fetchSlots = {0};
    auto stage = [&](uint32_t offset) {
      command.fetchRaw[0] = (phys + offset) | 3u; command.fetchRaw[1] = 4u;
      D3D12Backend::Pm4VsfOut output{};
      memres::ConsumerScope scope;
      b.pm4ShmSetDrainMode(false, true);
      bool ok = false;
      try { ok = b.pm4StageVsf(command, shader, false, b.m_xlatVertices.size(), b.m_xlatConstants.size(), output); }
      catch (...) { b.pm4ShmSetDrainMode(false, false); throw; }
      b.pm4ShmSetDrainMode(false, false);
      if (!ok || !output.shm || output.noIb) throw std::runtime_error("consumer actual staging refused");
      auto draw = batch();
      draw.pm4 = true; draw.vsf = true; draw.vsfShm = true;
      draw.vsHash = shaderHash; draw.vertexCount = output.indexCount; draw.topology = output.topo;
      draw.vsfIbOffset = output.ibOffset; draw.vsfFetchCbOff = output.fetchCbOff;
      draw.resourceConsumerChecks = scope.take();
      b.resourceCaptureConsumer(draw, command);
      return draw;
    };
    auto finish = [&](const D3D12Backend::DrawBatch& draw) {
      bool ok = true;
      for (const auto& ticket : draw.resourceConsumerChecks)
        ok = memres::resourceConsumerRecorded(ticket, draw.resourceConsumerId) && ok;
      check(ok, "consumer_actual_gpu_read_finalizes_its_validated_tickets");
    };
    auto refuses = [&](const D3D12Backend::DrawBatch& draw, const char* name) {
      bool refused = false;
      try { b.resourceValidateConsumer(draw); }
      catch (const std::runtime_error& e) {
        refused = std::string(e.what()).find("consumer check refused") != std::string::npos;
        if (!refused) throw;
      }
      check(refused, name);
    };
    const auto original = pattern(0x29);
    cpu(phys, original); capture(phys);
    uint32_t cursor = 0; record(batch(), cursor);
    auto gpu = original;
    std::fill_n(gpu.begin() + neighbor, 4, 0xD3);
    const auto gpuGeneration = memres::resourceMemory().reserveGpu();
    gpuCopy(phys, gpu, neighbor, 4); publishGpu(phys, neighbor, 4, gpuGeneration);
    auto* guest = kernel::guestVAToHost(0x80000000u + phys);
    guest[header] ^= 0x40;
    const auto cpuGeneration = memres::resourceMemory().cpuGeneration(phys >> 12);
    auto a = stage(payload);
    check(a.resourceConsumerChecks.size() == 1 && a.resourceConsumerChecks[0]->changedWords == 1,
          "consumer_real_vsf_admission_retains_unnoted_header_word");
    record(a, cursor); b.resourceValidateConsumer(a);
    checkBytes(snapshot(phys), gpu, "consumer_disjoint_draw_keeps_original_payload_and_GPU_neighbor");
    finish(a);
    check(memres::resourceMemory().cpuGeneration(phys >> 12) == cpuGeneration,
          "consumer_disjoint_does_not_publish_a_fake_CPU_write");
    auto bDraw = stage(header);
    check(bDraw.resourceConsumerChecks.size() == 1 && bDraw.resourceConsumerChecks[0]->changedWords == 1,
          "consumer_later_draw_still_observes_the_original_unnoted_change");
    refuses(bDraw, "consumer_later_draw_that_reads_header_is_rejected");
    guest[payload] ^= 0x80;
    auto c = stage(payload);
    check(c.resourceConsumerChecks.size() == 1 && c.resourceConsumerChecks[0]->changedWords == 2,
          "consumer_captures_later_payload_change_as_well_as_first_metadata_word");
    refuses(c, "consumer_all_changed_words_reject_payload_not_just_first_word");
    checkBytes(snapshot(phys), gpu, "consumer_failed_validation_never_overwrites_GPU_neighbor");
    auto wrongDraw = stage(neighbor);
    ++wrongDraw.resourceConsumerId;
    refuses(wrongDraw, "consumer_readset_cannot_validate_another_draw_identity");
    // Queue a later CPU copy after the old draw's Keep. Latest captured bytes
    // must not masquerade as the baseline already recorded at the old cursor.
    auto oldDraw = stage(neighbor);
    const auto future = pattern(0x79);
    cpu(phys, future); capture(phys);
    auto futureDraw = batch();
    record(oldDraw, cursor);
    bool oldAccepted = true;
    try { b.resourceValidateConsumer(oldDraw); }
    catch (const std::runtime_error& e) {
      if (std::string(e.what()).find("consumer check refused") == std::string::npos) throw;
      oldAccepted = false;
    }
    check(oldAccepted, "consumer_old_draw_baseline_not_hoisted_to_future_capture");
    checkBytes(snapshot(phys), gpu, "consumer_old_draw_uses_recorded_baseline_despite_future_CPU_capture");
    if (oldAccepted) finish(oldDraw);
    record(futureDraw, cursor);
    checkBytes(snapshot(phys), future, "consumer_future_CPU_copy_becomes_recorded_only_at_its_cursor");
    auto equal = stage(payload);
    check(!equal.resourceConsumerFetch, "consumer_equal_page_needs_no_fetch_snapshot");
    b.resourceValidateConsumer(equal);
    checkBytes(snapshot(phys), future, "consumer_equal_page_keeps_exact_recorded_bytes");
    finish(equal);
  }

  void consumerF1() {
    constexpr uint32_t sourcePhys = kPhys + 28 * kPage, targetPhys = kPhys + 29 * kPage,
                       indexPhys = kPhys + 30 * kPage;
    constexpr uint64_t hash = 0x7D00000000000004ull;
    auto& b = *b_;
    beginCase(sourcePhys);
    kernel::physwin::noteHeapRoutedGrant(0x80000000u + sourcePhys, 3 * kPage);
    b.m_xlatVertices.init(4096, false); b.m_xlatConstants.init(16384, false);
    b.m_xlatVBPtr = b.m_xlatVertices.data(); b.m_xlatCBPtr = b.m_xlatConstants.data();
    b.m_xlatVBUsed = 0; b.m_xlatCBUsed = 4096; b.m_vsfShmSrvReady = true;
    const uint32_t words[] = {
        1u | (3u << 12) | (20u << 16), 2u << 12, 0u,
        1u | (1u << 16) | (50u << 26), 0x006C6C6Cu, 0xE0000000u,
        (1u << 5) | (2u << 12) | (1u << 19), 0x688u | (1u << 13) | (33u << 16), 1u,
        (2u << 5) | (3u << 12) | (1u << 19) | (1u << 25), 0x688u | (36u << 16), 4u};
    auto put = [](uint8_t* p, uint32_t value) {
      p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16);
      p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
    };
    std::vector<uint8_t> ucode(sizeof(words));
    for (size_t i = 0; i < std::size(words); ++i) put(ucode.data() + 4 * i, words[i]);
    memres::noteVsUcode(hash, ucode.data(), ucode.size());
    auto* indices = kernel::guestVAToHost(0x80000000u + indexPhys);
    put(indices, 0x01000001u); put(indices + 4, 3u);
    GuestDrawCommand command{};
    command.vsUcodeHash = hash; command.vertexCount = 2; command.primType = 4;
    command.idxBasePhys = indexPhys; command.idxFmt32 = true; command.idxEndian = 2;
    command.indxOffset = 2; command.minVtxIndx = 3; command.maxVtxIndx = 5;
    command.fetchRaw[0] = sourcePhys | 3u; command.fetchRaw[1] = (1024u << 2) | 2u;
    command.fetchRaw[2] = targetPhys | 3u; command.fetchRaw[3] = 1024u << 2;
    CachedShader shader; shader.fetchSlots = {0, 1};
    auto stage = [&] {
      D3D12Backend::Pm4VsfOut output{};
      memres::ConsumerScope scope;
      b.pm4ShmSetDrainMode(false, true);
      bool ok = false;
      try { ok = b.pm4StageVsf(command, shader, false, b.m_xlatVertices.size(), b.m_xlatConstants.size(), output); }
      catch (...) { b.pm4ShmSetDrainMode(false, false); throw; }
      b.pm4ShmSetDrainMode(false, false);
      if (!ok || !output.shm || output.noIb) throw std::runtime_error("consumer F1 actual staging refused");
      auto draw = batch(); draw.pm4 = true; draw.vsf = true; draw.vsfShm = true;
      draw.vsHash = hash; draw.vertexCount = output.indexCount; draw.topology = output.topo;
      draw.vsfIbOffset = output.ibOffset; draw.vsfFetchCbOff = output.fetchCbOff;
      draw.resourceConsumerChecks = scope.take();
      if (!draw.resourceConsumerChecks.empty()) b.resourceCaptureConsumer(draw, command);
      return draw;
    };
    Bytes first{}; put(first.data() + 24, 7); put(first.data() + 40, 11);
    cpu(sourcePhys, first); cpu(targetPhys, pattern(0x59));
    auto firstDraw = stage(); uint32_t cursor = 0; record(firstDraw, cursor);
    put(kernel::guestVAToHost(0x80000000u + sourcePhys) + 24, 17); // deliberately no completion note
    auto secondDraw = stage(); record(secondDraw, cursor);
    const auto changed = std::find_if(secondDraw.resourceConsumerChecks.begin(), secondDraw.resourceConsumerChecks.end(),
        [&](const auto& ticket) { return ticket->observed.page == (sourcePhys >> 12); });
    check(changed != secondDraw.resourceConsumerChecks.end() && (*changed)->changedWords == 1,
          "consumer_F1_actual_admission_keeps_unnoted_index_stream_change");
    bool refused = false;
    try { b.resourceValidateConsumer(secondDraw); }
    catch (const std::runtime_error& e) {
      refused = std::string(e.what()).find("consumer check refused") != std::string::npos;
      if (!refused) throw;
    }
    check(refused, "consumer_complete_readset_includes_F1_itself_not_only_second_level_vertices");
    checkBytes(snapshot(sourcePhys), first, "consumer_F1_validation_reads_recorded_GPU_indices_without_guest_rebase");
  }

  void coldStart() {
    auto& b = *b_;
    constexpr uint32_t phys = kPhys + 11 * kPage;
    // Keep the real device/queue but remove the fixture's injected storage.
    // The first resolve must initialize the actual production storage path.
    b.m_shmUpload->Unmap(0, nullptr);
    b.m_shmUploadPtr = nullptr;
    b.m_shm.Reset();
    b.m_shmUpload.Reset();
    b.m_shmReadback.Reset();
    b.m_shmReady = false;
    b.m_pm4GpuReady = true;  // RT descriptor setup is unrelated to raw byte routing.
    const auto original = pattern(0x37);
    cpu(phys, original);
    const auto generation = memres::resourceMemory().reserveGpu();
    prepareResolve(phys, 32, 64, generation);
    check(b.m_shmReady && b.m_shmPending.size() == 1,
          "first_resolve_initializes_storage_before_preservation");
    uint32_t cursor = 0;
    record(batch(), cursor);
    checkBytes(snapshot(phys), original, "first_resolve_preservation_reaches_new_storage");
  }

  void orderedReplayRestores() {
    auto& b = *b_;
    constexpr uint32_t phys = kPhys + 12 * kPage, page = phys >> 12;
    b.m_shmHash8.assign(memres::kPages, 0);
    b.m_shmHashA.assign(memres::kPages, 0);
    b.m_shmFresh8.assign(memres::kPages, 0);
    b.m_shmFreshA.assign(memres::kPages, 0);
    b.m_shmSrcA.assign(memres::kPages, 0);
    auto* guest = kernel::guestVAToHost(0x80000000u | phys);
    auto read = [&](uint32_t at) {
      std::vector<uint8_t> bytes;
      b.resourcePrivateSnapshot(at, kPage, bytes);
      return bytes;
    };
    const auto first = pattern(0x43), second = pattern(0x91), laterGpu = pattern(0xD7);
    beginCase(phys);
    std::memcpy(guest, first.data(), kPage);
    b.pm4ShmRangeWrittenByGpu(phys, kPage);
    const auto initialStamp = b.m_shmStamp[page];
    b.replayRestoredRange(0xA0000000u | (phys + 0x118), 4);
    check(b.replayOrderedRestores() && b.m_replayRestores.size() == 1 && b.m_shmPending.empty() &&
              b.m_shmGpuWritten[page] && b.m_shmStamp[page] == initialStamp,
          "replay_capture_masks_physical_alias_without_publishing_gpu_bytes");
    std::memset(guest, 0xFC, kPage);  // Capture must not be deferred until staging.
    b.replayStageRestores();
    auto drawA = batch();
    std::memcpy(guest, second.data(), kPage);
    b.replayRestoredRange(phys, kPage);
    b.replayStageRestores();
    auto drawB = batch();
    b.m_drawBatches = {drawA, drawB};
    // The host-only fixture has no geometry pipeline, so pm4DrainNow's outer
    // readiness guard would skip this operation. Exercise its actual body.
    b.drainDrawQueue();  // An empty packet drain must not hoist B before A.
    check(b.m_drawBatches[0].shmPendEnd == drawA.shmPendEnd &&
              b.m_drawBatches[1].shmPendEnd == drawB.shmPendEnd,
          "replay_empty_packet_drain_preserves_older_copy_cursors");
    std::memset(guest, 0xEC, kPage);  // Neither admitted draw may read this future image.
    // The original failure: an OLDER queued GPU write executes after the
    // restore was captured. Its legacy rebaseline sees future CPU bytes. The
    // restore's later copy must replace both those bytes and that authority.
    gpuCopy(phys, laterGpu, 0, kPage);
    b.pm4ShmRangeWrittenByGpu(phys, kPage);
    check(b.m_shmGpuWritten[page] &&
              b.m_shmHash8[page] == ShaderCache::hashUcode(guest, kPage),
          "replay_older_queued_gpu_write_rebaselines_future_host_bytes_before_restore_records");
    constexpr uint64_t textureKey = 0x7253746F7265ull;
    b.m_texClean[textureKey] = 41;
    b.m_texPages[phys >> 16].push_back(textureKey);
    const auto priorTextureVersion = b.pm4ShmVersion(phys, kPage);
    const auto priorBindGeneration = b.m_texBindGen.load(std::memory_order_relaxed);
    b.m_srPages.assign(memres::kPages, 0);
    b.m_srBlock.assign(memres::kPages / 64, 0);
    b.m_srPages[page] = 1;
    b.m_srBlock[page >> 6] = 1;
    b.m_srReady = true;
    uint32_t cursor = 0;
    record(b.m_drawBatches[0], cursor);
    check(!b.m_srPages[page] && !b.m_srBlock[page >> 6] && !b.m_texClean.contains(textureKey) &&
              b.m_texBindGen > priorBindGeneration && b.pm4ShmVersion(phys, kPage) != priorTextureVersion,
          "replay_recording_invalidates_real_texture_cache_version_and_scaled_ownership");
    checkBytes(read(phys), first, "replay_first_draw_uses_immutable_cpu_page_baseline");
    check(!b.m_shmGpuWritten[page] && b.m_shmHash8[page] == ShaderCache::hashUcode(first.data(), kPage) &&
              b.m_shmFresh8[page] == 0 && b.m_shmStamp[page] != initialStamp,
          "replay_recording_clears_gpu_authority_and_rebases_captured_version");
    record(b.m_drawBatches[1], cursor);
    checkBytes(read(phys), second, "replay_later_restore_follows_earlier_consumer");
    gpuCopy(phys, laterGpu, 0, kPage);
    b.pm4ShmRangeWrittenByGpu(phys, kPage);
    checkBytes(read(phys), laterGpu, "replay_newer_gpu_write_wins_after_restore");
    check(b.m_shmGpuWritten[page] != 0, "replay_newer_gpu_ownership_is_retained");

    beginCase(phys);
    std::memcpy(guest, first.data(), kPage);
    guest[17] = 0xAB;
    b.replayRestoredRange(phys + 17, 1);
    guest[1009] = 0xCD;
    b.replayRestoredRange(phys + 1009, 1);
    Bytes merged = first;
    merged[17] = 0xAB;
    merged[1009] = 0xCD;
    b.m_pm4DrainOpen = false;
    b.replayPrepareTail();
    check(b.m_replayRestores.empty() && b.m_shmPending.size() == 1 && b.m_drawBatches.empty(),
          "replay_no_later_draw_stages_tail_without_inventing_batch");
    b.pm4ShmRecordPending(b.m_shmPending, 0, uint32_t(b.m_shmPending.size()));
    checkBytes(read(phys), merged, "replay_partial_restores_merge_actual_cpu_baseline_before_tail");

    beginCase(phys);
    std::memcpy(guest, first.data(), kPage);
    std::memcpy(guest + kPage, second.data(), kPage);
    b.replayRestoredRange(phys + kPage - 2, 4);
    check(b.m_replayRestores.size() == 2 && b.m_replayRestores.contains(page) &&
              b.m_replayRestores.contains(page + 1), "replay_partial_write_captures_both_touched_pages");
    bool rejected = false;
    try { b.replayRestoredRange(0xFFFFFFFFu, 2); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected && b.m_replayRestores.size() == 2,
          "replay_physical_wrap_rejected_before_capture_mutation");
    b.replayStageRestores();
    cursor = 0;
    record(batch(), cursor);
    checkBytes(read(phys), first, "replay_cross_page_first_cpu_baseline");
    checkBytes(read(phys + kPage), second, "replay_cross_page_second_cpu_baseline");

    for (bool staged : {false, true}) {
      beginCase(phys);
      std::memcpy(guest, first.data(), kPage);
      b.replayRestoredRange(phys, kPage);
      if (staged) b.replayStageRestores();
      b.m_drawBatches.push_back(batch());
      b.replayPoisonResource(phys, kPage);
      b.replayStageRestores();
      b.pm4ShmRecordPending(b.m_shmPending, 0, uint32_t(b.m_shmPending.size()));
      Bytes magenta{};
      for (size_t i = 0; i < magenta.size(); i += 4) {
        magenta[i] = magenta[i + 2] = magenta[i + 3] = 0xFF;
      }
      checkBytes(read(phys), magenta, staged ? "replay_missing_resolve_poison_survives_staged_restore" :
                                           "replay_missing_resolve_poison_survives_uncopied_capture");
    }
  }

  void deferredFetchAdmission(bool payload = false) {
    // Real request/collect/validate path, in a separate census-only process.
    // Neither validation runs inside the original draw admission scope.
    auto& b = *b_;
    constexpr uint32_t phys = kPhys, moved = 0x218;
    const Bytes initial = pattern(0x31);
    auto* bytes = kernel::guestVAToHost(0x80000000u + phys);
    std::memcpy(bytes, initial.data(), kPage);
    kernel::physwin::noteHeapRoutedGrant(0x80000000u + phys, kPage);
    memres::noteReadFile(0x80000000u + phys, kPage);
    b.m_shmHash8.assign(memres::kPages, 0);
    b.m_shmHashA.assign(memres::kPages, 0);
    b.m_shmFresh8.assign(memres::kPages, 0);
    b.m_shmFreshA.assign(memres::kPages, 0);
    b.m_shmSrcA.assign(memres::kPages, 0);
    auto collect = [&](uint32_t ordinal) {
      memres::FetchAdmissionScope originalDraw(ordinal);
      memres::KindScope kind(memres::Kind::kVb);
      b.pm4ShmSetDrainMode(true, true);
      if (!b.pm4ShmRequest(phys, kPage)) throw std::runtime_error("census collection refused");
      b.pm4ShmSetDrainMode(false, false);
      memres::FetchTargetOrigin absent;
      check(!memres::fetchTargetForCurrentDraw(absent), "deferred_target_unavailable_during_original_draw");
    };
    collect(51);
    check(b.m_shmCollect.size() == 1, "baseline_request_reaches_real_deferred_collection");
    b.pm4ShmCollectFlush();
    memres::Totals before{};
    memres::snapshot(before);
    check(before.outcome[size_t(memres::Outcome::kAgreeUp)] == 1 &&
              before.outcome[size_t(memres::Outcome::kMissed)] == 0,
          "real_baseline_validation_records_cpu_upload_without_miss");
    constexpr char hud[] = "screens/hud/main_hud";
    const uint32_t hudVA = 0x80000000u + phys + kPage;
    std::memcpy(kernel::guestVAToHost(hudVA), hud, sizeof(hud));
    memres::noteFetchHudRequest(hudVA, kernel::getGuestBase());
    bytes[moved] ^= 0x80u;  // Deliberate missing boundary in a requested dword.
    if (payload) bytes[0x600] ^= 0x40u;  // A later changed word IS consumed.
    ++b.m_shmEpoch;
    collect(52);
    memres::Totals queued{};
    memres::snapshot(queued);
    check(b.m_shmCollect.size() == 1 && queued.outcome[size_t(memres::Outcome::kMissed)] == 0,
          "miss_decision_is_deferred_until_after_original_draw_scope");
    b.pm4ShmCollectFlush();
    memres::Totals after{};
    memres::snapshot(after);
    check(after.outcome[size_t(memres::Outcome::kMissed)] == 1 && b.m_shmCollect.empty(),
          "real_deferred_flush_detects_exact_moved_vb_miss");
    memres::FetchTargetOrigin origin;
    check(!memres::fetchTargetForCurrentDraw(origin), "unscoped_validation_cannot_claim_a_consuming_draw");
    {
      memres::FetchAdmissionScope laterDraw(53);
      check(memres::fetchTargetForCurrentDraw(origin) && origin.target == phys + moved &&
                origin.ordinal == 0 && origin.sequence != 0 && origin.missed == 1 &&
                origin.epoch == b.m_shmEpoch && origin.known && origin.grantCount == 1,
            "deferred_validation_batch_origin_finalized_for_strictly_later_draw");
    }
    // Exercise the actual production admission, not only the origin latch.
    // The selected moved metadata precedes a later draw's declared data, but
    // both occupy the validated page (the live Warworld counterexample).
    constexpr uint64_t directHash = 0x7D00000000000002ull;
    const uint32_t words[] = {1u | (1u << 12) | (1u << 16), 2u << 12, 0u,
                              (1u << 12) | (1u << 19), 0x688u | (36u << 16), 1u};
    std::vector<uint8_t> ucode(sizeof(words));
    for (size_t i = 0; i < std::size(words); ++i)
      for (uint32_t j = 0; j < 4; ++j) ucode[i * 4 + j] = uint8_t(words[i] >> (24 - 8 * j));
    memres::noteVsUcode(directHash, ucode.data(), ucode.size());
    b.m_xlatConstants.init(8192, false);
    b.m_xlatCBPtr = b.m_xlatConstants.data();
    GuestDrawCommand draw{};
    CachedShader shader;
    shader.fetchSlots = {0};
    auto admit = [&](uint32_t address, uint32_t dwords) {
      draw.fetchRaw[0] = address | 3u;
      draw.fetchRaw[1] = dwords << 2;
      std::memcpy(b.m_xlatCBPtr + 4096, draw.fetchRaw, sizeof(draw.fetchRaw));
      D3D12Backend::DrawBatch candidate{};
      candidate.vsHash = directHash; candidate.vertexCount = 1; candidate.vsfNoIb = true;
      candidate.vsfShm = payload;
      candidate.vsfFetchCbOff = 4096;
      b.resourceCaptureFetch(candidate, draw, shader);
      return candidate;
    };
    check(!admit(phys + 0x600, 64).resourceFetchCapture,
          "actual_capture_refuses_without_a_later_command_scope");
    {
      memres::FetchAdmissionScope laterDraw(54);
      const auto candidate = admit(phys + 0x600, 64);
      check(candidate.resourceFetchCapture && !candidate.resourceFetchDeclaredDword &&
                candidate.resourceFetchTarget == phys + moved && candidate.resourceFetchOriginMatched,
            "actual_capture_admits_target_page_without_changing_outside_declared_dword");
      if (candidate.resourceFetchCapture) {
        const auto analysis = memres::analyzeFetchDraw(*candidate.resourceFetchCapture, {});
        check(analysis.complete && analysis.uses.size() == 1 && analysis.uses[0].phys == phys + 0x600 &&
                  analysis.uses[0].len == 4 && memres::confirmFetchDraw(*candidate.resourceFetchCapture,
                                                                                b.resourceCurrentFetchInput(candidate)),
              "same_page_candidate_proves_this_draw_fetches_data_beyond_selected_metadata");
        if (payload) {
          check(candidate.resourceFetchPayload && candidate.resourceFetchPayload == origin.payload &&
                    candidate.resourceFetchPayload->known,
                "payload_actual_admission_retains_complete_frozen_origin");
          memres::FetchAnalysis observed;
          memres::FetchPayloadIntersection joined;
          b.resourceInspectFetch(candidate, &observed, &joined);
          check(observed.complete && joined.complete && joined.changedWords.size() == 2 &&
                    joined.changedWords[0].phys == phys + moved && !joined.changedWords[0].candidate &&
                    joined.changedWords[1].phys == phys + 0x600 && joined.changedWords[1].candidate &&
                    joined.candidateWords == 1,
                "payload_actual_consumer_detects_later_changed_word_when_first_metadata_word_is_excluded");
          if (candidate.resourceFetchPayload) {
            auto changed = candidate;
            auto altered = std::make_shared<memres::FetchPagePayload>(*candidate.resourceFetchPayload);
            altered->grantDigest ^= 1u;
            changed.resourceFetchPayload = altered;
            b.resourceInspectFetch(changed, &observed, &joined);
            check(observed.complete && !joined.complete && joined.reason == "payload_page_identity_changed" &&
                      joined.candidateWords == 0,
                  "payload_full_page_identity_check_rejects_changed_digest_with_valid_selected_dword");
          } else {
            check(false, "payload_full_page_identity_check_missing_frozen_payload");
          }
          // A changed full-page owner invalidates membership even if the
          // selected dword and staged shader inputs remain unchanged.
          kernel::physwin::noteHeapRoutedGrant(0x80000000u + phys, kPage);
          b.resourceInspectFetch(candidate, &observed, &joined);
          check(!joined.complete && joined.reason == "payload_page_identity_changed" &&
                    joined.candidateWords == 0,
                "payload_actual_consumer_refuses_changed_page_provenance");
        }
      } else {
        check(false, "same_page_candidate_missing_before_analysis");
      }
      check(!admit(phys + kPage, 64).resourceFetchCapture,
            "adjacent_page_does_not_admit_the_selected_page");
      check(!admit(phys + 0x600, 0).resourceFetchCapture,
            "empty_declared_range_does_not_admit_the_selected_page");
      const auto exact = admit(phys + moved, 64);
      check(exact.resourceFetchCapture && exact.resourceFetchDeclaredDword,
            "actual_capture_reports_declared_dword_overlap_separately");
      check(!admit(phys + 0x600, 64).resourceFetchCapture,
            "target_page_candidates_obey_the_same_explicit_capture_cap");
    }
    auto& counts = b.resourceFetchCounters();
    check(counts.considered == 6 && counts.awaitingOrigin == 1 && counts.noTargetPage == 2 &&
              counts.pageCandidates == 3 && counts.declaredDword == 1 && counts.captured == 2 && counts.capped == 1,
          "actual_admission_counters_account_for_every_candidate_and_refusal");
    b.resourceFetchReport("fixture");
    const auto hooks = kernel::quit::productionHooks();
    auto terminalLine = [&] {
      std::string all;
      hooks.printFamilies(hooks.user, "terminal", &all, true);
      const auto at = all.find("[MEM_RES_FETCH] admission_summary=terminal ");
      return at == std::string::npos ? std::string{} : all.substr(at, all.find('\n', at) - at);
    };
    const auto terminal = terminalLine();
    check(terminal.find("considered=6 awaiting_origin=1 no_target_page=2 page_candidates=3 "
                        "declared_dword=1 captured=2 capped=1") != std::string::npos,
          "actual_quit_family_prints_complete_admission_counts_without_backend_shutdown");
    char changed[256]{};
    int fields = 0;
    check(!terminal.empty() && !kernel::quit::snapshotDiff(terminal, terminalLine(), changed, sizeof(changed), &fields),
          "admission_terminal_verify_is_stable_without_producers");
    counts.considered.fetch_add(1);
    const auto movedTerminal = terminalLine();
    counts.considered.fetch_sub(1);
    check(kernel::quit::snapshotDiff(terminal, movedTerminal, changed, sizeof(changed), &fields) && fields > 0,
          "admission_terminal_verify_detects_a_later_counter_change");
    if (payload) {
      std::string all;
      hooks.printFamilies(hooks.user, "terminal", &all, true);
      const auto at = all.find("[MEM_RES_FETCH] payload_summary=terminal ");
      const auto line = at == std::string::npos ? std::string{} : all.substr(at, all.find('\n', at) - at);
      check(line.find("known=1") != std::string::npos && line.find("artifact=1") != std::string::npos &&
                line.find("seen=1 retained=1 capped=0 invalid=0") != std::string::npos,
            "payload_actual_quit_family_retains_known_artifact_and_population");
      // Emit the actual family snapshot retained above; the fixture deliberately
      // probes one draw three times, so its joins are not a live report input.
      std::printf("%s\n", line.c_str());
    }
  }

  void textureProbe() {
    // Synthetic bytes, actual production queued copies/submission/retirement.
    // The recorded bit models admission here; the replay proves a real draw.
    auto& b = *b_;
    constexpr uint32_t span = 4u * 640u * 640u * 2u;
    b.m_shm = buffer(b.m_device.Get(), 0x0B5E1000u + span, D3D12_HEAP_TYPE_DEFAULT,
                     D3D12_RESOURCE_STATE_COMMON);
    auto upload = buffer(b.m_device.Get(), 2u * span, D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* data = nullptr;
    const D3D12_RANGE empty{0, 0};
    require(upload->Map(0, &empty, reinterpret_cast<void**>(&data)), "probe upload map");
    for (uint32_t i = 0; i < 2u * span; ++i) data[i] = uint8_t(17u + i * 13u + i / 819200u);
    std::vector<uint8_t> expected(data, data + 2u * span);
    upload->Unmap(0, nullptr);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = desc.Height = 640;
    desc.DepthOrArraySize = 4; desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R16_UNORM;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    require(b.m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "probe texture");
    b.pm4ShmState(D3D12_RESOURCE_STATE_COPY_DEST);
    b.m_commandList->CopyBufferRegion(b.m_shm.Get(), 0x0B5E1000u, upload.Get(), 0, span);
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints{};
    b.m_device->GetCopyableFootprints(&desc, 0, 4, span, footprints.data(), nullptr, nullptr, nullptr);
    for (uint32_t layer = 0; layer < 4; ++layer) {
      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
      dst.pResource = texture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = layer;
      src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = footprints[layer];
      b.m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    b.m_commandList->ResourceBarrier(1, &barrier);
    b.m_xlatViews.resize(2);
    auto& view = b.m_xlatViews[1]; view.resource = texture;
    view.srv.Format = DXGI_FORMAT_R16_UNORM;
    view.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.srv.Texture2DArray.MipLevels = 1; view.srv.Texture2DArray.ArraySize = 4;
    b.m_xlatConstants.init(512, false); b.m_xlatCBPtr = b.m_xlatConstants.data();
    const uint32_t planning = 0xC0000001u, compact = 0xC0000000u;
    std::memcpy(b.m_xlatCBPtr + 64, &planning, 4);
    std::array<uint8_t, 256> floats{};
    std::array<uint8_t, 512> shared{}; std::memcpy(shared.data() + 64, &compact, 4);
    require(b.m_nativeConstants.initialize(b.m_device.Get(), 65536, 1u << 20), "probe constants arena");
    TranslatedConstantSnapshot constants;
    require(TranslatedConstantSnapshot::stage(b.m_nativeConstants, floats, floats, shared, {}, constants),
            "probe immutable constants");
    D3D12Backend::DrawBatch draw{};
    draw.vsHash = 0xCE2D505A0E7B9767ull; draw.psHash = 0x38392DB754F93075ull; draw.drawOrdinal = 41;
    constexpr uint32_t fetch[6] = {0x85024802, 0x0B5E1458, 0x0C4FE27F, 0x01001400, 0, 0x00000218};
    std::copy_n(fetch, 6, draw.texFetch);
    check(b.resourceTextureProbe(draw, constants), "texture_probe_queues_exact_bound_array");
    const auto path = b.m_resourceTextureProbe->path;
    check(!std::filesystem::exists(path) && b.m_resourceTextureProbe->ticket == 0,
          "texture_probe_no_output_before_real_submission");
    b.m_resourceTextureProbe->recorded = true;
    b.pm4Flush();
    check(!b.m_resourceTextureProbe && std::filesystem::exists(path),
          "texture_probe_collects_after_existing_fence_completion");
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> actual((std::istreambuf_iterator<char>(file)), {});
    const auto staged = constants.storage().bytes();
    expected.insert(expected.end(), staged.begin(), staged.end());
    check(actual == expected, "texture_probe_actual_arena_four_layers_and_constants_byte_exact");
    check(!b.resourceTextureProbe(draw, constants), "texture_probe_one_capture_bound");
  }

 private:
  std::unique_ptr<D3D12Backend> b_;
  std::string control_;
  std::vector<ComPtr<ID3D12Resource>> writes_;

  void cpu(uint32_t phys, const Bytes& bytes) {
    auto* dst = kernel::guestVAToHost(0x80000000u + phys);
    if (!dst) throw std::runtime_error("fixture guest pointer missing");
    std::memcpy(dst, bytes.data(), bytes.size());
    memres::resourceMemory().noteCpu(phys, kPage, memres::Src::kReadFile);
  }
  void beginCase(uint32_t phys) {
    auto& b = *b_;
    b.m_shmPending.clear();
    b.m_drawBatches.clear();
    b.m_shmArenaHighT[0] = b.m_shmUploadUsedT[0] = 0;
    ++b.m_shmEpoch;
    // Deterministic hostile initial GPU bytes: a missed upload cannot be
    // accidentally rescued by zero-initialized heaps or an earlier case.
    Bytes poison{};
    poison.fill(0xE7);
    gpuCopy(phys, poison, 0, kPage);
  }
  void capture(uint32_t phys) {
    auto& b = *b_;
    b.pm4ShmSetDrainMode(false, true);
    try {
      if (!b.pm4ResourceRequest(phys, kPage)) throw std::runtime_error("capture refused");
    } catch (...) {
      b.pm4ShmSetDrainMode(false, false);
      throw;
    }
    b.pm4ShmSetDrainMode(false, false);
    if (b.m_shmUploadUsedT[0] > kUploadBytes)
      throw std::runtime_error("fixture upload capacity exceeded");
  }
  void prepareResolve(uint32_t phys, uint32_t offset, uint32_t bytes, uint32_t generation) {
    auto& b = *b_;
    b.pm4ShmSetDrainMode(false, true);
    try {
      b.resourcePrepareResolveRange(phys + offset, bytes, generation);
    } catch (...) {
      b.pm4ShmSetDrainMode(false, false);
      throw;
    }
    b.pm4ShmSetDrainMode(false, false);
  }
  D3D12Backend::DrawBatch batch() {
    D3D12Backend::DrawBatch batch{};
    batch.shmPendEnd = uint32_t(b_->m_shmPending.size());
    return batch;
  }
  void record(const D3D12Backend::DrawBatch& batch, uint32_t& cursor) {
    b_->pm4RecordBatchPending(batch, b_->m_shmPending, cursor);
  }
  void gpuCopy(uint32_t phys, const Bytes& bytes, uint32_t offset, uint32_t length) {
    auto& b = *b_;
    auto source = buffer(b.m_device.Get(), kPage, D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    require(source->Map(0, &noRead, &mapped), "GPU test payload mapping");
    std::memcpy(mapped, bytes.data(), kPage);
    source->Unmap(0, nullptr);
    b.pm4ShmState(D3D12_RESOURCE_STATE_COPY_DEST);
    b.m_commandList->CopyBufferRegion(b.m_shm.Get(), uint64_t(phys) + offset,
                                      source.Get(), offset, length);
    writes_.push_back(std::move(source));
  }
  void publishGpu(uint32_t phys, uint32_t offset, uint32_t length, uint32_t generation) {
    b_->m_resourceGpuGeneration = generation;
    b_->pm4ShmRangeWrittenByGpu(phys + offset, length);
    b_->m_resourceGpuGeneration = 0;
  }
  std::vector<uint8_t> snapshot(uint32_t phys) {
    std::vector<uint8_t> output{0xDD};
    b_->resourceCpuSnapshot(phys, kPage, output);
    return output;
  }

 public:  // run by main (--ab); the 089aff28 merge left it below the private helpers
  // mem.resources frozen-pass A/B (gfx/resource_ab.hpp, --ab): the production request / validation / GPU-write paths
  // driven in alternate arms on one arena. Each case is a sequence a per-present toggle produces; each check names the
  // rule (INV-M / INV-R / the dual rebaseline / the witness) whose omission would fail it.
  void abCases() {
    auto& b = *b_;
    static const uint8_t zeros[kPage] = {};
    const uint64_t zh = ShaderCache::hashUcode(zeros, kPage);
    b.m_shmHash8.assign(memres::kPages, zh);
    b.m_shmHashA.assign(memres::kPages, zh);
    b.m_shmSrcA.assign(memres::kPages, 0u);
    b.m_shmFresh8.assign(memres::kPages, 0u);
    b.m_shmFreshA.assign(memres::kPages, 0u);
    check(memres::resourceAbDual(), "ab_dual_mode_latched_from_the_toggle");
    auto& memory = memres::resourceMemory();
    // One frame set of an arm, as drainDrawQueue runs it: its requests in the drain's collect + defer mode, the model's
    // collected validation at the drain's end (DeferGuard: the native arm captured per request), then the copies
    // recorded in order.
    auto present = [&](bool native, const std::function<void()>& requests) {
      memres::resourceAbEnter(native ? 1 : 0);
      b.pm4ShmSetDrainMode(true, true);
      try {
        requests();
        if (!native) b.pm4ShmCollectFlush();
      } catch (...) {
        b.pm4ShmSetDrainMode(false, false);
        throw;
      }
      b.pm4ShmSetDrainMode(false, false);
      auto set = batch();
      uint32_t cursor = 0;
      record(set, cursor);
      b.m_shmPending.clear();
      ++b.m_shmEpoch;  // the next set validates anew
      b.m_shmArenaHighT[0] = b.m_shmUploadUsedT[0] = 0;
    };
    auto request = [&](uint32_t phys) {
      if (!b.pm4ShmRequest(phys, kPage)) throw std::runtime_error("ab request refused");
    };
    auto arena = [&](uint32_t phys) {
      std::vector<uint8_t> out;
      b.resourcePrivateSnapshot(phys, kPage, out);
      return out;
    };
    auto unannounced = [&](uint32_t phys, const Bytes& bytes) {  // a CPU write with no boundary
      std::memcpy(kernel::guestVAToHost(0x80000000u + phys), bytes.data(), kPage);
    };
    // The witness counters by event (Astra review1 cases).
    auto ev = [](const memres::AbTotals& t, memres::AbEvent e) { return t.event[uint32_t(e)]; };
    constexpr auto kReest = memres::AbEvent::kModelReestablished, kUnann = memres::AbEvent::kCrossUnannounced,
                   kClob = memres::AbEvent::kCrossClobber, kPres = memres::AbEvent::kModelPreserve,
                   kNote = memres::AbEvent::kGpuNoteModelArm, kModelUp = memres::AbEvent::kModelUploadPages;
    const auto A = pattern(0x51), B = pattern(0x62), G = pattern(0x73), X = pattern(0x84);

    // INV-M, the ABA case: model uploads A; a native set uploads B; the CPU writes A back with a boundary. The model's
    // stored hash (A) would keep the arena's B unless the native upload made the model forget the page.
    const uint32_t p1 = kPhys + 28 * kPage;
    beginCase(p1);
    cpu(p1, A);
    present(false, [&] { request(p1); });
    checkBytes(arena(p1), A, "ab_model_set_uploads_the_first_bytes");
    cpu(p1, B);
    const auto before1 = memres::resourceAbTotals();
    present(true, [&] { request(p1); });
    checkBytes(arena(p1), B, "ab_native_set_uploads_the_noted_bytes");
    check(memres::resourceAbTotals().event[uint32_t(memres::AbEvent::kModelForgot)] >
              before1.event[uint32_t(memres::AbEvent::kModelForgot)],
          "ab_inv_m_native_upload_of_other_bytes_makes_the_model_forget");
    cpu(p1, A);
    present(false, [&] { request(p1); });
    checkBytes(arena(p1), A, "ab_inv_m_model_set_after_native_upload_never_keeps_stale_bytes_ABA");
    present(true, [&] { request(p1); });
    checkBytes(arena(p1), A, "ab_native_set_after_model_upload_agrees");

    // An A-arm KEEP then a B-arm present: an unannounced write the registry keeps over; the model repairs the arena in
    // its present and the INV-R counter names the repair; the registry's next keep shows the repaired bytes.
    const uint32_t p2 = kPhys + 29 * kPage;
    beginCase(p2);
    cpu(p2, A);
    present(true, [&] { request(p2); });
    const auto before2a = memres::resourceAbTotals();
    present(false, [&] { request(p2); });
    checkBytes(arena(p2), A, "ab_model_reestablishes_a_page_the_native_set_uploaded");
    check(ev(memres::resourceAbTotals(), kReest) == ev(before2a, kReest) + 1 &&
              ev(memres::resourceAbTotals(), kUnann) == ev(before2a, kUnann),
          "ab_p2_5_reestablishing_the_native_capture_bytes_is_no_repair");
    unannounced(p2, B);
    present(true, [&] { request(p2); });
    checkBytes(arena(p2), A, "ab_native_set_keeps_over_an_unannounced_write_by_design");
    const auto before2 = memres::resourceAbTotals();
    present(false, [&] { request(p2); });
    checkBytes(arena(p2), B, "ab_model_set_after_a_native_keep_uploads_the_unannounced_write");
    check(memres::resourceAbTotals().event[uint32_t(memres::AbEvent::kCrossUnannounced)] ==
              before2.event[uint32_t(memres::AbEvent::kCrossUnannounced)] + 1,
          "ab_inv_r_counts_the_model_repair_of_an_unannounced_write");
    present(true, [&] { request(p2); });
    checkBytes(arena(p2), B, "ab_native_keep_after_the_repair_is_consistent_with_ram");

    // The dual rebaseline: a native-set resolve writes the page after a noted CPU write; the model's next validation
    // must keep the GPU's bytes (its rebaseline ran in the native set), and so must the registry.
    const uint32_t p3 = kPhys + 30 * kPage;
    beginCase(p3);
    cpu(p3, A);
    present(false, [&] { request(p3); });
    cpu(p3, B);
    present(true, [&] {
      const uint32_t generation = memory.reserveGpu();
      gpuCopy(p3, G, 0, kPage);
      publishGpu(p3, 0, kPage, generation);
    });
    checkBytes(arena(p3), G, "ab_native_set_resolve_wrote_the_arena");
    present(false, [&] { request(p3); });
    checkBytes(arena(p3), G, "ab_dual_rebaseline_model_keeps_gpu_data_written_in_a_native_set");
    present(true, [&] { request(p3); });
    checkBytes(arena(p3), G, "ab_registry_keeps_gpu_data_after_a_model_set");

    // INV-R's window rule: the 0x8 copy zero, the 0xA copy the title's bytes, no allocator record. The model serves
    // 0xA, the registry 0x8: each arm must show its own decision (an A/B difference, visible per present).
    const uint32_t p4 = kPhys + 31 * kPage;
    beginCase(p4);
    std::memset(kernel::guestVAToHost(0x80000000u + p4), 0, kPage);
    std::memcpy(kernel::guestVAToHost(0xA0000000u + p4), X.data(), kPage);
    // The registry uploads first (its 0x8 copy, a record of what the arena holds), so the revoke is what makes its
    // next native set upload again rather than keep the model's 0xA bytes as its own.
    Bytes zero{};
    present(true, [&] { request(p4); });
    checkBytes(arena(p4), zero, "ab_native_set_serves_the_0x8_copy_first");
    const auto before4 = memres::resourceAbTotals();
    present(false, [&] { request(p4); });
    checkBytes(arena(p4), X, "ab_model_serves_the_0xA_copy_over_a_zero_0x8_copy");
    check(memres::resourceAbTotals().event[uint32_t(memres::AbEvent::kCrossWindow)] ==
              before4.event[uint32_t(memres::AbEvent::kCrossWindow)] + 1,
          "ab_inv_r_model_upload_from_another_window_revokes_the_registry");
    present(true, [&] { request(p4); });
    checkBytes(arena(p4), zero, "ab_native_set_shows_its_own_window_after_the_revocation");
    present(false, [&] { request(p4); });
    checkBytes(arena(p4), X, "ab_model_set_shows_its_own_window_again");

    // Astra review1 P1-1 (gfx/resource_ab.hpp STAMPS): the render thread renders model set F while the CP drains native
    // set F+1. F's render-side request accepts any stamp of F's frame or later; the native set's stamp - its request
    // over the page F's partial resolve was announced on - must not satisfy it, or F skips the upload of the CPU's noted
    // bytes its resolve preserves around (each isolated route uploads them).
    const uint32_t p5 = kPhys + 32 * kPage;
    beginCase(p5);
    cpu(p5, A);
    present(false, [&] { request(p5); });
    checkBytes(arena(p5), A, "ab_p1_1_model_set_uploads_the_first_bytes");
    cpu(p5, B);                                          // noted: F must show B around its partial resolve
    const uint32_t frameF = ++b.m_shmEpoch;              // F's first epoch: its render accepts stamps >= this
    memory.noteGpuAt(p5 + 0x800, 0x100, memory.reserveGpu());  // F's drain announces its partial resolve (model arm)
    ++b.m_shmEpoch;                                      // the CP drains F+1 (native) while the render thread is on F
    memres::resourceAbEnter(1);
    b.pm4ShmSetDrainMode(true, true);
    try { request(p5); } catch (...) { b.pm4ShmSetDrainMode(false, false); throw; }
    b.pm4ShmSetDrainMode(false, false);
    std::vector<D3D12Backend::ShmPend> nextSet;
    nextSet.swap(b.m_shmPending);                        // F+1's queued copies: recorded after F, not before it
    memres::resourceAbEnter(0);                          // the render thread takes F (model)
    b.m_rtOn = true;
    b.m_shmEpochFrameStart = frameF;
    const auto before5 = memres::resourceAbTotals();
    try { request(p5); } catch (...) { b.m_rtOn = false; b.m_shmEpochFrameStart = 0; throw; }
    b.m_rtOn = false;
    b.m_shmEpochFrameStart = 0;
    checkBytes(arena(p5), B, "ab_p1_1_a_later_native_set_stamp_never_satisfies_an_earlier_model_set_request");
    check(ev(memres::resourceAbTotals(), kModelUp) == ev(before5, kModelUp) + 1,
          "ab_p1_1_the_model_set_uploads_the_page_itself");
    // P1-2: that upload preserves CPU bytes around a resolve the registry holds as PENDING GPU data - not a clobber.
    check(ev(memres::resourceAbTotals(), kClob) == ev(before5, kClob) &&
              ev(memres::resourceAbTotals(), kPres) == ev(before5, kPres) + 1,
          "ab_p1_2_preservation_before_a_pending_resolve_is_no_clobber");
    b.m_shmPending.clear();
    ++b.m_shmEpoch;
    b.m_shmArenaHighT[0] = b.m_shmUploadUsedT[0] = 0;

    // Astra review1 P1-2 through the production drain note: a model set's resolve batch (the real extent wrapper's
    // registers) notes its generation to the registry at the drain (resourceAbNoteResolve), the model's validation then
    // preserves the noted CPU bytes (pending GPU data: preserve); after the resolve EXECUTES, an unannounced CPU write
    // the model's hash sees and the registry does not is the clobber.
    const uint32_t p6 = kPhys + 33 * kPage;
    beginCase(p6);
    cpu(p6, A);
    present(false, [&] { request(p6); });
    cpu(p6, B);
    D3D12Backend::DrawBatch resolve{};
    resolve.resolve = resolve.pm4 = true;
    resolve.resourceGpuGeneration = memory.reserveGpu();
    {
      auto& r = resolve.pm4Regs;  // resolveBatchWrapper's 8x8 color resolve, destination p6
      r.surfaceInfo = 80;
      r.colorInfo[0] = 0;
      r.copyControl = 0x00100000;
      r.copyDestBase = p6;
      r.copyDestPitch = 32u | (8u << 16);
      r.copyDestInfo = 6u << 7;
      r.windowScissorTL = 0x80000000u;
      r.windowScissorBR = 80u | (8u << 16);
      r.screenScissorBR = 0x20002000u;
      const float vertices[6] = {-0.5f, -0.5f, 7.5f, -0.5f, 7.5f, 7.5f};
      std::memcpy(r.resolveVerts, vertices, sizeof(vertices));
      r.resolveVertsValid = true;
    }
    const auto before6 = memres::resourceAbTotals();
    present(false, [&] {
      b.resourceAbNoteResolve(resolve);  // the model arm's drain: the registry hears the resolve's generation
      request(p6);                       // the model's validation: the CPU bytes the resolve will be drawn over
    });
    check(ev(memres::resourceAbTotals(), kNote) == ev(before6, kNote) + 1 &&
              memory.gpuGeneration(p6 >> 12) == resolve.resourceGpuGeneration,
          "ab_model_arm_drain_notes_the_resolve_generation_to_the_registry");
    check(ev(memres::resourceAbTotals(), kClob) == ev(before6, kClob) &&
              ev(memres::resourceAbTotals(), kPres) == ev(before6, kPres) + 1,
          "ab_p1_2_model_preservation_under_an_announced_resolve_is_no_clobber");
    checkBytes(arena(p6), B, "ab_model_preserves_the_noted_cpu_bytes_before_its_resolve");
    present(false, [&] {  // the resolve executes (its GPU write; the dual rebaseline)
      gpuCopy(p6, G, 0, 0x100);
      publishGpu(p6, 0, 0x100, resolve.resourceGpuGeneration);
    });
    unannounced(p6, X);
    const auto before6b = memres::resourceAbTotals();
    present(false, [&] { request(p6); });
    checkBytes(arena(p6), X, "ab_model_uploads_an_unannounced_write_over_executed_gpu_data");
    check(ev(memres::resourceAbTotals(), kClob) == ev(before6b, kClob) + 1,
          "ab_p1_2_model_upload_over_executed_gpu_data_the_registry_holds_is_a_clobber");

    // Astra review1 P2-5: INV-M forgot the page after a native upload of A; the CPU then writes B with no note. The
    // model re-establishing the page copies OTHER bytes than the native capture: a repair (the native keeps showed A).
    const uint32_t p7 = kPhys + 34 * kPage;
    beginCase(p7);
    cpu(p7, X);
    present(false, [&] { request(p7); });
    cpu(p7, A);
    present(true, [&] { request(p7); });
    unannounced(p7, B);
    const auto before7 = memres::resourceAbTotals();
    present(false, [&] { request(p7); });
    checkBytes(arena(p7), B, "ab_model_reestablishes_with_the_unannounced_bytes");
    check(ev(memres::resourceAbTotals(), kUnann) == ev(before7, kUnann) + 1 &&
              ev(memres::resourceAbTotals(), kReest) == ev(before7, kReest),
          "ab_p2_5_reestablishing_with_other_bytes_than_the_native_capture_is_a_repair");

    // Astra review1 P2-6 (gfx/resource_ab.hpp EMPTY): the production drain on an empty queue before any set is inert;
    // presents no drain began a set for alternate; a Z-pass partial set's present keeps its arm.
    memres::resourceAbEnter(-1);  // this thread as the CP before its first set (the boot)
    const auto before8 = memres::resourceAbTotals();
    b.drainDrawQueue();           // no command queued
    check(memres::resourceAbTotals().unlatched == before8.unlatched,
          "ab_p2_6_an_empty_drain_before_any_set_consults_no_arm");
    const int8_t e0 = memres::resourceAbPresentingSet();
    memres::resourceAbPresented();
    b.drainDrawQueue();           // an empty drain between presents
    const int8_t e1 = memres::resourceAbPresentingSet();
    memres::resourceAbPresented();
    check(e0 >= 0 && e1 >= 0 && e0 != e1, "ab_p2_6_consecutive_empty_presents_alternate");
    memres::resourceAbBeginSet();  // a partial (Z-pass) set: submitted without a present
    const int8_t z = memres::resourceAbArm();
    const int8_t e2 = memres::resourceAbPresentingSet();
    memres::resourceAbPresented();
    check(z == e2 && e2 != e1, "ab_p2_6_a_partial_set_then_its_present_keep_one_arm_and_alternate");
    memres::resourceAbBeginSet();
    check(memres::resourceAbArm() != e2, "ab_p2_6_the_next_set_takes_the_other_arm");
    check(memres::resourceAbTotals().emptySets == before8.emptySets + 2 &&
              memres::resourceAbTotals().unlatched == before8.unlatched,
          "ab_p2_6_two_empty_presenting_sets_counted_no_unlatched_decision");

    // textures_from_resolves (gfx/resource_texid.hpp cpu_after_gpu, the orchestrator's "no CPU write between" rule):
    // the census's page rule on the registry - a GPU-written page is resolve output; once a CPU note reaches it after
    // the GPU write it is a resolve destination read by bytes, named with the note's source.
    memres::resourceAbEnter(1);  // the registry decides (the census reads its generations)
    const uint32_t p9 = kPhys + 35 * kPage;
    beginCase(p9);
    memory.noteGpuAt(p9, kPage, memory.reserveGpu());
    uint32_t after9 = 0, src9 = 0;
    check(b.texIdGpuPages(p9, kPage, &after9, &src9) == 1 && after9 == 0,
          "texid_a_gpu_written_page_is_resolve_output");
    cpu(p9, A);  // a noted CPU write (Src::kReadFile) after the GPU write
    after9 = 0;
    check(b.texIdGpuPages(p9, kPage, &after9, &src9) == 0 && after9 == 1 &&
              src9 == uint32_t(memres::Src::kReadFile),
          "texid_a_cpu_note_after_the_gpu_write_counts_cpu_after_gpu_with_its_source");

    // The witness: decisions by who decided x the latched arm.
    const auto t = memres::resourceAbTotals();
    check(t.decNative > 0 && t.decModel > 0, "ab_witness_both_arms_decided");
    check(t.nativeInModel == 0, "ab_witness_no_native_decision_in_a_model_set");
    check(t.modelInNative == 0, "ab_witness_no_model_decision_in_a_native_set");
    check(t.unlatched == 0 && t.decUnlatched == 0, "ab_witness_every_decision_on_a_latched_thread");
    std::printf("[RESOURCE_D3D12_TEST] AB_TOTALS decisions native=%llu model=%llu native_in_model=%llu "
                "model_in_native=%llu forgot=%llu adopted=%llu cross_window=%llu cross_clobber=%llu "
                "cross_unannounced=%llu preserve=%llu reestablished=%llu empty_sets=%llu\n",
                (unsigned long long)t.decNative, (unsigned long long)t.decModel,
                (unsigned long long)t.nativeInModel, (unsigned long long)t.modelInNative,
                (unsigned long long)t.event[uint32_t(memres::AbEvent::kModelForgot)],
                (unsigned long long)t.event[uint32_t(memres::AbEvent::kModelAdopted)],
                (unsigned long long)t.event[uint32_t(memres::AbEvent::kCrossWindow)],
                (unsigned long long)t.event[uint32_t(memres::AbEvent::kCrossClobber)],
                (unsigned long long)t.event[uint32_t(memres::AbEvent::kCrossUnannounced)],
                (unsigned long long)t.event[uint32_t(memres::AbEvent::kModelPreserve)],
                (unsigned long long)t.event[uint32_t(memres::AbEvent::kModelReestablished)],
                (unsigned long long)t.emptySets);
  }

 private:
  void consumedFetch() {
    constexpr uint32_t sourcePhys = kPhys + 22 * kPage, targetPhys = kPhys + 23 * kPage,
                       indexPhys = kPhys + 24 * kPage;
    constexpr uint64_t shaderHash = 0x7D00000000000001ull;
    // The production shader observer is census-only; keep its test override
    // scoped while the native byte-routing path remains selected independently.
    memres::censusResetForTest(true);
    struct RestoreCensus { ~RestoreCensus() { memres::censusResetForTest(false); } } restoreCensus;
    auto& b = *b_;
    beginCase(sourcePhys);
    kernel::physwin::noteHeapRoutedGrant(0x80000000u + sourcePhys, 3 * kPage);
    b.m_xlatVertices.init(4096, false);
    b.m_xlatConstants.init(16384, false);
    b.m_xlatVBPtr = b.m_xlatVertices.data();
    b.m_xlatCBPtr = b.m_xlatConstants.data();
    b.m_xlatVBUsed = 0;
    b.m_xlatCBUsed = 4096;  // The first 4096 bytes are the actual staged VS constants.
    b.m_vsfShmSrvReady = true;
    // r1.x = r0.x + r0.x; integer F1 at r1.x -> r2; slot1 at r2.x.
    const uint32_t words[] = {
        1u | (3u << 12) | (20u << 16), 2u << 12, 0u,
        1u | (1u << 16) | (50u << 26), 0x006C6C6Cu, 0xE0000000u,
        (1u << 5) | (2u << 12) | (1u << 19), 0x688u | (1u << 13) | (33u << 16), 1u,
        (2u << 5) | (3u << 12) | (1u << 19) | (1u << 25), 0x688u | (36u << 16), 4u};
    std::vector<uint8_t> ucode(sizeof(words));
    auto put = [](uint8_t* p, uint32_t value) {
      p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16);
      p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
    };
    for (size_t i = 0; i < std::size(words); ++i) put(ucode.data() + 4 * i, words[i]);
    memres::noteVsUcode(shaderHash, ucode.data(), ucode.size());
    auto* indexBytes = kernel::guestVAToHost(0x80000000u + indexPhys);
    put(indexBytes, 0x01000001u); put(indexBytes + 4, 3u);
    GuestDrawCommand draw{};
    draw.vsUcodeHash = shaderHash; draw.vertexCount = 2; draw.primType = 4;
    draw.idxBasePhys = indexPhys; draw.idxFmt32 = true; draw.idxEndian = 2;
    draw.indxOffset = 2; draw.minVtxIndx = 3; draw.maxVtxIndx = 5;
    draw.fetchRaw[0] = sourcePhys | 3u; draw.fetchRaw[1] = (1024u << 2) | 2u;
    draw.fetchRaw[2] = targetPhys | 3u; draw.fetchRaw[3] = 1024u << 2;
    CachedShader shader;
    shader.fetchSlots = {0, 1};
    Bytes first{}, second{}, future{};
    put(first.data() + 24, 7); put(first.data() + 40, 11);
    put(second.data() + 24, 5); put(second.data() + 40, 13);
    put(future.data() + 24, 41); put(future.data() + 40, 43);
    cpu(targetPhys, pattern(0x39));
    auto stage = [&](uint64_t id) {
      D3D12Backend::Pm4VsfOut out{};
      b.pm4ShmSetDrainMode(false, true);
      bool ok = false;
      try { ok = b.pm4StageVsf(draw, shader, false, b.m_xlatVertices.size(), b.m_xlatConstants.size(), out); }
      catch (...) { b.pm4ShmSetDrainMode(false, false); throw; }
      b.pm4ShmSetDrainMode(false, false);
      if (!ok || !out.shm || out.noIb) throw std::runtime_error("actual fetch staging refused");
      auto result = batch();
      result.vsHash = shaderHash; result.vertexCount = out.indexCount; result.topology = out.topo;
      result.vsf = true; result.vsfShm = out.shm; result.vsfNoIb = out.noIb;
      result.vsfFirstVertex = out.firstVertex; result.vsfIbOffset = out.ibOffset;
      result.vsfFetchCbOff = out.fetchCbOff;
      b.resourceCaptureFetchInputs(result, id, targetPhys + 7 * 16, 0xFFFFu);
      return result;
    };
    cpu(sourcePhys, first);
    auto a = stage(1001);
    const auto* converted = reinterpret_cast<const uint32_t*>(b.m_xlatVBPtr + a.vsfIbOffset);
    check(converted[0] == 3 && converted[1] == 5,
          "fetch_capture_uses_real_endian_offset_mask_clamp_conversion");
    cpu(sourcePhys, second);
    auto next = stage(1002);
    cpu(sourcePhys, future);
    std::memcpy(kernel::guestVAToHost(0xA0000000u + sourcePhys), future.data(), kPage);
    uint32_t cursor = 0;
    record(a, cursor);
    auto contains = [&](const memres::FetchAnalysis& analysis, uint32_t index) {
      return std::any_of(analysis.uses.begin(), analysis.uses.end(), [&](const memres::FetchUse& use) {
        return use.slot == 1 && use.phys == targetPhys + index * 16;
      });
    };
    memres::FetchAnalysis observed;
    check(b.resourceInspectFetch(a, &observed) && observed.complete && contains(observed, 7) &&
              contains(observed, 11) && !contains(observed, 41),
          "fetch_inspection_reads_draw_A_recorded_F1_not_newer_guest_or_queued_B");
    check(std::equal(future.begin(), future.end(), kernel::guestVAToHost(0x80000000u + sourcePhys)) &&
              std::equal(future.begin(), future.end(), kernel::guestVAToHost(0xA0000000u + sourcePhys)),
          "fetch_inspection_does_not_mutate_either_guest_alias");
    Bytes gpu = first;
    put(gpu.data() + 24, 17);
    gpuCopy(sourcePhys, gpu, 24, 4);
    b.resourceInspectFetch(a, &observed);
    check(observed.complete && contains(observed, 17) && !contains(observed, 7),
          "fetch_inspection_includes_prior_GPU_writes_at_actual_cursor");
    record(next, cursor);
    b.resourceInspectFetch(next, &observed);
    check(observed.complete && contains(observed, 5) && contains(observed, 13) && !contains(observed, 17),
          "fetch_inspection_reads_draw_B_only_after_its_pending_cursor");
    auto* changed = reinterpret_cast<uint32_t*>(b.m_xlatVBPtr + next.vsfIbOffset);
    changed[0] = 4;
    b.resourceInspectFetch(next, &observed);
    check(!observed.complete && observed.uses.empty() && observed.reason == "submitted_inputs_changed",
          "fetch_inspection_rejects_staging_changed_before_actual_bind");
  }

  void partialPage() {
    constexpr uint32_t phys = kPhys, offset = 112, length = 64;
    beginCase(phys);
    const auto original = pattern(0x13);
    const auto replacement = pattern(0xA3);
    cpu(phys, original);
    auto& memory = memres::resourceMemory();
    const auto generation = memory.reserveGpu();
    if (control_ == "omit-preserve") {
      // Reproduce the old order: ownership became GPU before destination
      // preservation, so even a later request decided KeepGpu.
      memory.noteGpuAt(phys + offset, length, generation);
      capture(phys);
    } else {
      prepareResolve(phys, offset, length, generation);
    }
    check(b_->m_shmStamp[phys >> 12] == 0, "preservation_capture_does_not_publish_recorded_stamp");
    check(b_->m_shmPending.size() == 1, "partial_resolve_queues_destination_preservation");
    uint32_t cursor = 0;
    record(batch(), cursor);
    gpuCopy(phys, replacement, offset, length);
    publishGpu(phys, offset, length, generation);
    Bytes expected = original;
    std::copy_n(replacement.begin() + offset, length, expected.begin() + offset);
    // Guest writes after recording must survive the real GPU readback path.
    const auto later = pattern(0x69);
    cpu(phys, later);
    auto output = snapshot(phys);
    checkBytes(output, expected, "partial_page_keeps_untouched_cpu_bytes");
    check(std::equal(later.begin(), later.end(), kernel::guestVAToHost(0x80000000u + phys)),
          "native_snapshot_does_not_overwrite_newer_guest_bytes");
    if (!output.empty()) output[0] ^= 0xFF;
    checkBytes(snapshot(phys), expected, "snapshot_result_is_caller_owned");
    bool refused = false;
    try { b_->pm4ShmSyncToGuest(phys, kPage); }
    catch (const std::logic_error&) { refused = true; }
    check(refused, "native_legacy_guest_readback_refused");
  }

  void olderResolveNewerCpu() {
    constexpr uint32_t phys = kPhys + kPage, offset = 512, length = 128;
    beginCase(phys);
    auto& memory = memres::resourceMemory();
    const auto generation = memory.reserveGpu();
    const auto newer = pattern(0x35);
    cpu(phys, newer);  // Announced after this resolve was produced.
    prepareResolve(phys, offset, length, generation);
    const auto resolve = batch();
    capture(phys);  // Must upload again AFTER the older queued resolve.
    const auto draw = batch();
    check(resolve.shmPendEnd == 1 && draw.shmPendEnd == 2,
          "older_gpu_publication_invalidates_preserved_newer_cpu_upload");
    uint32_t cursor = 0;
    record(resolve, cursor);
    gpuCopy(phys, pattern(0xBA), offset, length);
    publishGpu(phys, offset, length, generation);
    // Republishing this exact token while recording must not invalidate the
    // already captured later draw's upload ticket for a second time.
    check(!memres::isUpload(memory.prepare(phys >> 12).decision),
          "duplicate_gpu_token_does_not_invalidate_later_captured_draw");
    if (control_ == "skip-copy" && b_->m_shmPending.size() > resolve.shmPendEnd)
      b_->m_shmPending[resolve.shmPendEnd].done = true;
    record(draw, cursor);
    checkBytes(snapshot(phys), newer, "newer_cpu_draw_restores_bytes_after_older_resolve");
  }

  void queuedDraws() {
    constexpr uint32_t phys = kPhys + 2 * kPage;
    beginCase(phys);
    const auto first = pattern(0x21), second = pattern(0x67), future = pattern(0xC4);
    cpu(phys, first);
    capture(phys);
    const auto drawA = batch();
    cpu(phys, second);
    capture(phys);
    const auto drawB = batch();
    cpu(phys, future);  // Neither queued draw may fetch these later bytes.
    check(drawA.shmPendEnd == 1 && drawB.shmPendEnd == 2, "each_draw_captures_its_own_upload_cursor");
    check(b_->m_shmStamp[phys >> 12] == 0, "future_queued_upload_does_not_publish_stamp");
    uint32_t cursor = 0;
    record(control_ == "hoist-copy" ? drawB : drawA, cursor);
    const auto stampA = b_->m_shmStamp[phys >> 12];
    check(stampA != 0 && cursor == drawA.shmPendEnd, "first_draw_records_only_its_pending_prefix");
    checkBytes(snapshot(phys), first, "first_draw_reads_first_captured_bytes");
    record(drawB, cursor);
    check(b_->m_shmStamp[phys >> 12] > stampA, "second_recorded_copy_advances_texture_stamp");
    checkBytes(snapshot(phys), second, "second_draw_reads_second_captured_bytes");
    check(std::equal(future.begin(), future.end(), kernel::guestVAToHost(0x80000000u + phys)),
          "both_snapshots_leave_future_guest_write_untouched");
  }

  void inlineDraws() {
    constexpr uint32_t phys = kPhys + 8 * kPage;
    constexpr uint32_t device = 0x80000000u + kPhys + 16 * kPage;
    beginCase(phys);
    auto& memory = memres::resourceMemory();
    const auto first = pattern(0x35), second = pattern(0x72), future = pattern(0xC1);
    auto* guest = kernel::guestVAToHost(0xA0000000u + phys);
    const uint32_t before = memory.cpuGeneration(phys >> 12);
    kernel::resourceInlineBegin(device, 0xA0000000u + phys, kPage);
    check(memory.cpuGeneration(phys >> 12) == before, "inline_allocation_does_not_announce_unwritten_bytes");
    std::memcpy(guest, first.data(), kPage);
    const uint32_t cursorBE = __builtin_bswap32(0xA0012340u);
    std::memcpy(kernel::guestVAToHost(device + 0x3314u), &cursorBE, 4);
    PPCContext end{};
    end.r3.u64 = device;
    sub_820FBBA0(end, kernel::getGuestBase());
    check(memory.cpuGeneration(phys >> 12) > before &&
          std::memcmp(kernel::guestVAToHost(device + 40u), &cursorBE, 4) == 0,
          "actual_End_override_notes_bytes_and_preserves_SDK_cursor_publication");
    capture(phys);
    const auto drawA = batch();
    // Exercise the actual memcpy override, inside the SDK UP scope. A note
    // deleted from either memcpy path leaves A current and fails this draw.
    constexpr uint32_t source = 0x80000000u + kPhys + 30 * kPage;
    std::memcpy(kernel::guestVAToHost(source), second.data(), kPage);
    {
      kernel::ResourceInlineUpScope scope(device);
      kernel::resourceInlineBegin(device, 0xA0000000u + phys, kPage);
      PPCContext ctx{};
      ctx.r1.u64 = 0x80000000u + kPhys + kGuestBytes - 16u;
      ctx.r3.u64 = 0xA0000000u + phys;
      ctx.r4.u64 = source;
      ctx.r5.u64 = kPage;
      if (control_ == "skip-inline-completion") __imp__sub_8279EC60(ctx, kernel::getGuestBase());
      else sub_8279EC60(ctx, kernel::getGuestBase());
      try { scope.finish(); }
      catch (const std::logic_error&) {
        if (control_ != "skip-inline-completion") throw;
        std::printf("[RESOURCE_D3D12_TEST] INLINE_COMPLETION_REJECTION omitted_memcpy_boundary\n");
        check(false, "omitted_UP_completion_rejected_before_cursor_publication");
        return;
      }
    }
    capture(phys);
    const auto drawB = batch();
    std::memcpy(guest, future.data(), kPage);
    check(drawA.shmPendEnd == 1 && drawB.shmPendEnd == 2, "inline_wrap_has_two_distinct_captured_cursors");
    uint32_t cursor = 0;
    record(drawA, cursor);
    checkBytes(snapshot(phys), first, "inline_first_draw_keeps_its_bytes_across_wrap");
    record(drawB, cursor);
    checkBytes(snapshot(phys), second, "inline_wrapped_draw_captures_actual_completed_UP_copy");
    check(std::equal(future.begin(), future.end(), guest), "inline_gpu_readback_does_not_touch_future_guest_bytes");
  }

  void inlineReuse() {
    constexpr uint32_t phys = kPhys + 9 * kPage, offset = 256, length = 96;
    beginCase(phys);
    auto& memory = memres::resourceMemory();
    auto* guest = kernel::guestVAToHost(0xA0000000u + phys);
    const auto original = pattern(0x4B), replacement = pattern(0xA9);
    std::memcpy(guest, original.data(), kPage);
    memory.noteInlineWrite(0xA0000000u + phys, kPage);
    // Reserve the later resolve BEFORE the earlier draw is drained: preparing
    // that draw may not manufacture ownership newer than this producer.
    const auto generation = memory.reserveGpu();
    capture(phys);
    uint32_t cursor = 0;
    record(batch(), cursor);
    const auto cpuBefore = memory.cpuGeneration(phys >> 12);
    prepareResolve(phys, offset, length, generation);
    check(memory.cpuGeneration(phys >> 12) == cpuBefore,
          "inline_destination_preservation_does_not_invent_newer_cpu_write");
    record(batch(), cursor);
    gpuCopy(phys, replacement, offset, length);
    publishGpu(phys, offset, length, generation);
    const auto pendingBefore = b_->m_shmPending.size();
    capture(phys);
    record(batch(), cursor);
    check(b_->m_shmPending.size() == pendingBefore,
          "later_gpu_reuse_retires_inline_refresh_without_uploading_stale_guest");
    Bytes expected = original;
    std::copy_n(replacement.begin() + offset, length, expected.begin() + offset);
    checkBytes(snapshot(phys), expected, "inline_to_partial_gpu_reuse_keeps_resolve_and_preserved_bytes");

    // The converse order: the resolve was produced BEFORE a new inline
    // allocation and its CPU capture. That producer cannot retire it.
    const auto older = memory.reserveGpu();
    const auto newer = pattern(0x73);
    std::memcpy(guest, newer.data(), kPage);
    memory.noteInlineWrite(0xA0000000u + phys, kPage);
    capture(phys);
    prepareResolve(phys, offset, length, older);
    const auto resolve = batch();
    capture(phys);
    const auto draw = batch();
    record(resolve, cursor);
    gpuCopy(phys, replacement, offset, length);
    publishGpu(phys, offset, length, older);
    record(draw, cursor);
    check(memory.sourceWindow(phys >> 12, 8) == 0xAu,
          "older_queued_gpu_token_preserves_new_inline_allocation");
    checkBytes(snapshot(phys), newer, "newer_inline_cpu_bytes_restore_after_older_queued_resolve");

    // Same pool reused by an authoritative non-inline CPU writer, with no
    // physical allocation event. Its live owner must replace old A metadata.
    const auto file = pattern(0xD3);
    cpu(phys, file);
    capture(phys);
    record(batch(), cursor);
    check(memory.sourceWindow(phys >> 12, 8) == 8u &&
          memory.prepare(phys >> 12).decision == memres::Decision::kKeep,
          "completed_cpu_reuse_expires_inline_window_and_refresh");
    checkBytes(snapshot(phys), file, "completed_cpu_reuse_uploads_new_source_bytes");
  }

  void refusedProducer() {
    constexpr uint32_t phys = kPhys + 6 * kPage;
    beginCase(phys);
    cpu(phys, pattern(0x19));
    const auto before = memres::resourceMemory().gpuGeneration(phys >> 12);
    bool refused = false;
    try { prepareResolve(phys, 0, 64, 0); }
    catch (const std::logic_error&) { refused = true; }
    check(refused && b_->m_shmPending.empty() &&
          memres::resourceMemory().gpuGeneration(phys >> 12) == before,
          "missing_producer_refused_before_capture_or_publication");
  }

  void resolveBatchWrapper() {
    constexpr uint32_t phys = kPhys + 3 * kPage;
    beginCase(phys);
    const auto bytes = pattern(0x46);
    cpu(phys, bytes);
    D3D12Backend::DrawBatch resolve{};
    resolve.resolve = resolve.pm4 = true;
    resolve.resourceGpuGeneration = memres::resourceMemory().reserveGpu();
    auto& r = resolve.pm4Regs;
    // The same valid 8x8 color resolve register fixture as edram_test.cpp;
    // only its destination address changes. Exercise the actual extent wrapper
    // and its pending-prefix publication, not a fixture-computed cursor.
    r.surfaceInfo = 80;
    r.colorInfo[0] = 0;
    r.copyControl = 0x00100000;
    r.copyDestBase = phys;
    r.copyDestPitch = 32u | (8u << 16);
    r.copyDestInfo = 6u << 7;
    r.windowScissorTL = 0x80000000u;
    r.windowScissorBR = 80u | (8u << 16);
    r.screenScissorBR = 0x20002000u;
    const float vertices[6] = {-0.5f, -0.5f, 7.5f, -0.5f, 7.5f, 7.5f};
    std::memcpy(r.resolveVerts, vertices, sizeof(vertices));
    r.resolveVertsValid = true;
    b_->pm4ShmSetDrainMode(false, true);
    try { b_->resourcePrepareResolve(resolve); }
    catch (...) { b_->pm4ShmSetDrainMode(false, false); throw; }
    b_->pm4ShmSetDrainMode(false, false);
    check(resolve.shmPendEnd == 1 && b_->m_shmPending.size() == 1,
          "real_resolve_extent_wrapper_publishes_preservation_cursor");
    uint32_t cursor = 0;
    record(resolve, cursor);
    checkBytes(snapshot(phys), bytes, "real_resolve_batch_records_preservation_bytes");
  }

  void replayPoison() {
    constexpr uint32_t phys = kPhys + 4 * kPage;
    beginCase(phys);
    cpu(phys, pattern(0x31));
    capture(phys);
    b_->m_drawBatches.push_back(batch());
    cpu(phys, pattern(0x76));
    capture(phys);
    b_->m_drawBatches.push_back(batch());
    b_->replayPoisonResource(phys, kPage);
    const auto poisonedPending = b_->m_shmPending.size();
    capture(phys);
    check(b_->m_shmPending.size() == poisonedPending, "known_oracle_poison_keeps_without_rebase_or_cpu_note");
    check(b_->m_shmPending.size() == 3 && b_->m_drawBatches[0].shmPendEnd == 2 &&
          b_->m_drawBatches[1].shmPendEnd == 3,
          "replay_poison_prepends_copy_and_shifts_each_batch_cursor");
    Bytes magenta{};
    for (size_t i = 0; i < magenta.size(); i += 4) {
      magenta[i] = 0xFF; magenta[i + 2] = 0xFF; magenta[i + 3] = 0xFF;
    }
    uint32_t cursor = 0;
    record(b_->m_drawBatches[0], cursor);
    checkBytes(snapshot(phys), magenta, "missing_resolve_cannot_reuse_first_captured_trace_picture");
    record(b_->m_drawBatches[1], cursor);
    checkBytes(snapshot(phys), magenta, "later_captured_trace_picture_cannot_undo_replay_poison");

    constexpr uint32_t emptyPhys = kPhys + 5 * kPage;
    beginCase(emptyPhys);
    b_->m_drawBatches.push_back(batch());
    b_->replayPoisonResource(emptyPhys, kPage);
    cursor = 0;
    record(b_->m_drawBatches[0], cursor);
    checkBytes(snapshot(emptyPhys), magenta, "replay_poison_initializes_destination_without_captured_upload");
  }

  void replayPoisonCoverage() {
    constexpr uint32_t phys = kPhys + 12 * kPage, offset = 256, length = 128;
    beginCase(phys);
    auto expected = pattern(0x23);
    cpu(phys, expected);
    capture(phys);
    const auto generation = memres::resourceMemory().cpuGeneration(phys >> 12);
    auto* guest = kernel::guestVAToHost(0x80000000u + phys);
    if (control_ == "poison-outside-write" || control_ == "poison-unbaselined-partial") {
      const bool outside = control_ == "poison-outside-write";
      if (outside) guest[13] ^= 1;  // Outside the oracle's modified subrange.
      const uint32_t target = outside ? phys : phys + kPage;
      const auto* targetBytes = kernel::guestVAToHost(0x80000000u + target);
      Bytes before;
      std::memcpy(before.data(), targetBytes, before.size());
      const auto pending = b_->m_shmPending.size();
      bool rejected = false;
      try { b_->replayPoisonResource(target + offset, length); }
      catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("valid checker coverage") != std::string::npos;
        if (!rejected) throw;
      }
      check(b_->m_shmPending.size() == pending && std::equal(before.begin(), before.end(), targetBytes),
            "unknown_or_stale_partial_poison_refused_before_guest_and_gpu_write");
      if (rejected) std::printf("[RESOURCE_D3D12_TEST] ORACLE_COVERAGE_REJECTION %s\n", control_.c_str());
      check(false, rejected ? "oracle_coverage_control_rejected" : "oracle_coverage_control_escaped");
      return;
    }
    for (uint32_t i = offset; i < offset + length; ++i) expected[i] = (i & 3u) == 1u ? 0 : 0xFF;
    if (control_ == "skip-poison-accounting") {
      std::memcpy(guest + offset, expected.data() + offset, length);
      bool rejected = false;
      try { capture(phys); }
      catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("checker found stale") != std::string::npos;
        if (!rejected) throw;
      }
      if (rejected) std::printf("[RESOURCE_D3D12_TEST] ORACLE_COVERAGE_REJECTION skip-poison-accounting\n");
      check(false, rejected ? "unaccounted_oracle_write_rejected" : "unaccounted_oracle_write_escaped");
      return;
    }
    b_->m_drawBatches.push_back(batch());
    b_->replayPoisonResource(phys + offset, length);
    capture(phys);  // Production kept-page checker must accept the predicted bytes.
    check(memres::resourceMemory().cpuGeneration(phys >> 12) == generation,
          "partial_oracle_poison_does_not_publish_cpu_ownership");
    check(std::equal(expected.begin(), expected.end(), guest),
          "partial_oracle_poison_preserves_all_unmodified_guest_bytes");
    uint32_t cursor = 0;
    record(b_->m_drawBatches.front(), cursor);
    checkBytes(snapshot(phys), expected, "partial_oracle_poison_updates_only_explicit_gpu_bytes");
  }

  void keptPageChecker() {
    constexpr uint32_t phys = kPhys + 7 * kPage;
    beginCase(phys);
    cpu(phys, pattern(0x17));
    capture(phys);
    uint32_t cursor = 0;
    record(batch(), cursor);
    const auto priorSize = b_->m_shmPending.size();
    capture(phys);
    check(b_->m_shmPending.size() == priorSize, "unchanged_kept_page_passes_independent_checker");
    const auto changed = pattern(0xB2);
    if (control_ == "missing-note") {
      // This is the missing completed-writer boundary the independent observer
      // must catch. Do not update the production registry or the checker.
      std::memcpy(kernel::guestVAToHost(0x80000000u + phys), changed.data(), changed.size());
      bool rejected = false;
      try { capture(phys); }
      catch (const std::runtime_error& e) {
        rejected = std::string(e.what()).find("checker found stale") != std::string::npos;
        if (rejected) std::printf("[RESOURCE_D3D12_TEST] CHECKER_REJECTION missing_cpu_write_note\n");
        else throw;
      }
      // Controls retain failure status even when the expected defect is caught.
      // The runner must require CHECKER_REJECTION and STALE=1 as well as exit1.
      check(false, rejected ? "missing_cpu_write_note_rejected" : "missing_cpu_write_note_escaped_checker");
      return;
    }
    cpu(phys, changed);
    capture(phys);
    check(b_->m_shmPending.size() == priorSize + 1, "completed_cpu_note_admits_changed_page");
    record(batch(), cursor);
    checkBytes(snapshot(phys), changed, "noted_changed_page_reaches_gpu_and_checker");
  }
};
}  // namespace sr::gfx

int resource_memory_d3d12_test_main(int argc, char** argv) {
  const auto started = std::chrono::steady_clock::now();
  std::string control;
  bool coldStart = false, textureProbe = false, fetchAdmission = false, fetchPayload = false, consumer = false,
       fastPath = false, replayRestore = false;
  bool ab = false, cpResReq = false, cpResReqCheck = false;  // step 6 I4
  std::string failureContext;
  for (int i = 2; i < argc; ++i) {
    const std::string option = argv[i];
    if (option.starts_with("--control=") && option.size() > 10 && control.empty()) control = option.substr(10);
    else if (option == "--consumer" && !consumer) consumer = true;
    else if (option == "--ab" && !ab) ab = true;
    else if (option == "--cold-start" && !coldStart) coldStart = true;
    else if (option == "--fast-path" && !fastPath) fastPath = true;
    else if (option == "--texture-probe" && !textureProbe) textureProbe = true;
    else if (option == "--fetch-admission" && !fetchAdmission) fetchAdmission = true;
    else if (option == "--fetch-payload" && !fetchPayload) fetchPayload = true;
    else if (option.starts_with("--failure-context=") && failureContext.empty())
      failureContext = option.substr(18);
    else if (option == "--replay-restore" && !replayRestore) replayRestore = true;
    else if (option == "--cp-res-req" && !cpResReq) cpResReq = true;
    else if (option == "--cp-res-req-check" && !cpResReqCheck) cpResReqCheck = true;
    else {
      std::fprintf(stderr, "unknown or duplicate fixture option: %s\n", argv[i]);
      return 2;
    }
  }
  if (!control.empty() && control != "omit-preserve" && control != "hoist-copy" &&
      control != "skip-copy" && control != "missing-note" && control != "skip-inline-completion" &&
      control != "poison-outside-write" && control != "poison-unbaselined-partial" &&
      control != "skip-poison-accounting" && control != "skip-patch") {
    std::fprintf(stderr, "unknown resource routing control: %s\n", control.c_str());
    return 2;
  }
  if (!failureContext.empty() && failureContext != "healthy" && failureContext != "removed") {
    std::fprintf(stderr, "unknown failure context mode\n");
    return 2;
  }
  if (unsigned(textureProbe) + unsigned(fetchAdmission) + unsigned(fetchPayload) + unsigned(coldStart) +
          unsigned(!failureContext.empty()) + unsigned(consumer) + unsigned(fastPath) + unsigned(replayRestore) +
          unsigned(ab) + unsigned(cpResReq) + unsigned(cpResReqCheck) > 1 ||
      ((textureProbe || fetchAdmission || fetchPayload || consumer || replayRestore || ab || cpResReq ||
        cpResReqCheck || !failureContext.empty()) && !control.empty())) {
    std::fprintf(stderr, "diagnostic probe requires its isolated fixture mode\n");
    return 2;
  }
  try {
    if (!failureContext.empty())
      sr::gfx::ResourceRoutingTestAccess::failureContext(failureContext == "removed");
    if (_putenv_s("SR_MEM_RESOURCES", (fetchAdmission || fetchPayload) ? "census" : replayRestore ? "0" : "1"))
      throw std::runtime_error("cannot select resource gate");
    if (consumer && _putenv_s("SR_MEM_RESOURCES_CONSUMER_CHECK", "1"))
      throw std::runtime_error("cannot select consumer checker");
    // The --ab cases keep over an unannounced write on purpose: the independent checker (which stops at such a keep)
    // stays off there; the toggle needs SR_MEM_RESOURCES=1 + SR_AB_TOGGLE, the write watch stays uninstalled.
    if (ab && (_putenv_s("SR_AB_TOGGLE", "SR_MEM_RESOURCES") || _putenv_s("SR_AB_PRESENT", "1") ||
               _putenv_s("SR_SHM_WRITE_TRACK", "0")))
      throw std::runtime_error("cannot select the frozen-pass A/B");
    // --fast-path: no observer, so the request's lock-free keep and same-drain memo run (mem-native); --ab too, and
    // the I4 modes (the lean path on, its CHECK oracle only in --cp-res-req-check).
    const bool cpRes = cpResReq || cpResReqCheck;
    if (!ab && !fastPath && !cpRes && _putenv_s("SR_MEM_RESOURCES_CHECK", "1"))
      throw std::runtime_error("cannot select independent checker");
    if (cpRes && (_putenv_s("SR_CP_RES_REQ_LEAN", "1") ||
                  _putenv_s("SR_CP_RES_REQ_CHECK", cpResReqCheck ? "1" : "0")))
      throw std::runtime_error("cannot select the I4 lean path");
    if (_putenv_s("SR_NATIVE_RESOLVE_SYNC", "1")) throw std::runtime_error("cannot bound pipeline initialization");
    if (textureProbe && _putenv_s("SR_MEM_RES_TEXTURE_PROBE", "1"))
      throw std::runtime_error("cannot select texture probe");
    if ((fetchAdmission || fetchPayload) && (_putenv_s("SR_MEM_RES_FETCH_CAPTURE", "1") ||
                          _putenv_s("SR_MEM_RES_FETCH_TARGET", "first_missed_after_hud") ||
                          _putenv_s("SR_MEM_RES_FETCH_CAP", "2") ||
                          _putenv_s("SR_SHM_WRITE_TRACK", "0") || _putenv_s("SR_MEM_RES_TRAPS", "0")))
      throw std::runtime_error("cannot select deferred census diagnostic");
    if (fetchPayload && (_putenv_s("SR_MEM_RES_FETCH_PAYLOAD", "1") ||
                         !std::getenv("SR_MEM_RES_FETCH_PAYLOAD_DIR")))
      throw std::runtime_error("payload fixture requires a fresh explicit SR_MEM_RES_FETCH_PAYLOAD_DIR");
    if (replayRestore && (_putenv_s("SR_REPLAY_ORDERED_RESTORES", "1") ||
                          _putenv_s("SR_PM4_RENDER_THREAD", "0") ||
                          _putenv_s("SR_CP_DRAIN_SKIP_EMPTY", "0") ||
                          _putenv_s("SR_SHM_WRITE_TRACK", "0")))
      throw std::runtime_error("cannot select ordered replay fixture");
    if (sr::gfx::memres::resourceMode() != !(fetchAdmission || fetchPayload || replayRestore))
      throw std::runtime_error("resource gate was latched before test");
    GuestPages guest;
    {
      sr::gfx::ResourceRoutingTestAccess fixture(control);
      if (replayRestore) fixture.orderedReplayRestores();
      else if (consumer) { fixture.consumerChecks(); fixture.consumerF1(); }
      else if (ab) fixture.abCases();
      else if (fetchAdmission || fetchPayload) fixture.deferredFetchAdmission(fetchPayload);
      else if (textureProbe) fixture.textureProbe();
      else if (coldStart) fixture.coldStart();
      else if (fastPath) fixture.fastPath();
      else if (cpResReq) fixture.cpResReq();
      else if (cpResReqCheck) fixture.cpResReqCheck();
      else fixture.run();
    }
    sr::gfx::memres::resourceCheckTerminal();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("[RESOURCE_D3D12_TEST] COMPLETE passed=%u failed=%u control=%s seconds=%.6f\n",
                passed, failed, control.empty() ? "none" : control.c_str(), seconds);
    return failed ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[RESOURCE_D3D12_TEST] SETUP_OR_EXECUTION_FAILED %s\n", e.what());
    return 2;
  }
}
