// Actual WARP queue/query collector; no window, game or extra replay frame.
// difftest.exe --replay-census-test
#include "gfx/d3d12_backend.hpp"
#include "app/log.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
unsigned passed = 0, failed = 0;
void check(bool value, const char* name) {
  std::printf("[NATIVE_CENSUS_TEST] %s %s\n", value ? "PASS" : "FAIL", name);
  value ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) throw std::runtime_error(std::string(name) + " hr=" + std::to_string(unsigned(hr)));
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* device, D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = 65536;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> result;
  require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
      type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON,
      nullptr, IID_PPV_ARGS(&result)), "buffer");
  return result;
}
struct Capture {
  std::string text;
  Capture() {
    sr::Log::setThreadCapture([](void* user, const char* line) {
      auto& text = static_cast<Capture*>(user)->text;
      text += line;
      text += '\n';
    }, this, true);
  }
  ~Capture() { sr::Log::setThreadCapture(nullptr, nullptr, false); }
};
size_t occurrences(const std::string& text, const char* word) {
  size_t count = 0, pos = 0;
  while ((pos = text.find(word, pos)) != std::string::npos) { ++count; pos += std::strlen(word); }
  return count;
}
}  // namespace

namespace sr::gfx {
struct NativeCensusTestAccess {
  std::unique_ptr<D3D12Backend> backend = std::make_unique<D3D12Backend>();
  ComPtr<ID3D12Resource> upload, readback;
  NativeCensusTestAccess() {
    auto& b = *backend;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&b.m_device)), "device");
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    require(b.m_device->CreateCommandQueue(&queue, IID_PPV_ARGS(&b.m_commandQueue)), "queue");
    require(b.m_nativeImages.initialize(b.m_device.Get(), b.m_commandQueue.Get(), 4, 0), "queue owner");
    upload = buffer(b.m_device.Get(), D3D12_HEAP_TYPE_UPLOAD);
    readback = buffer(b.m_device.Get(), D3D12_HEAP_TYPE_READBACK);
    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    require(upload->Map(0, &noRead, &mapped), "upload map");
    std::memset(mapped, 0x6B, 65536);
    upload->Unmap(0, nullptr);
    b.nbReplayBegin();
  }
  ~NativeCensusTestAccess() {
    backend->m_rtOn = false;  // a refusal control, never an actual worker
    const HRESULT hr = backend->m_nativeImages.shutdown(10000);
    if (FAILED(hr)) {
      std::fprintf(stderr, "[NATIVE_CENSUS_TEST] CLEANUP_FAILED hr=%08lX\n", (unsigned long)hr);
      std::fflush(nullptr);
      std::_Exit(2);
    }
  }
  void record(bool submit = true, uint64_t frame = 17, uint32_t slot = 0, bool row = true) {
    auto& b = *backend;
    b.clReset(D3D12Backend::Pm4Reset);
    b.m_frameIndex = slot;
    b.m_frameEpoch = frame;
    b.gtFrameStart();
    if (!b.m_nbFrameOn) throw std::runtime_error("actual collector did not arm");
    if (row) b.nbBegin(b.nbAdd("R #1 key=00000001 C f6 m0 b0 p80 rect=(0,0)-(8,8) ss=0 -> dest=00100000 p8 h8 df6 e0 sw0 arr0 sl0 bias=1 cvt path=O ext=00100000+256"));
    for (unsigned n = 0; n < 16; ++n)
      b.m_commandList->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, 65536);
    b.gtMark(D3D12Backend::GtResolve);
    b.nbEnd();
    b.gtFrameEnd();  // actual ResolveQueryData + actual census seal
    b.clClose(D3D12Backend::Pm4Close);
    if (submit) b.clExecute(D3D12Backend::Pm4Exec);
  }
  void completed() { backend->waitForGpu(); }
  std::array<uint64_t, 3> times() {
    auto& b = *backend;
    std::array<uint64_t, 3> result{};
    D3D12_RANGE range{0, sizeof(result)};
    void* mapped = nullptr;
    require(b.m_gtReadback->Map(0, &range, &mapped), "query map");
    std::memcpy(result.data(), mapped, sizeof(result));
    const D3D12_RANGE noWrite{0, 0};
    b.m_gtReadback->Unmap(0, &noWrite);
    return result;
  }
  void rejects(const char* name) {
    Capture capture;
    check(!backend->nbReplayFinish() && !backend->m_nbRecs[0].empty() &&
          capture.text.find("complete=1") == std::string::npos &&
          capture.text.find("[NB_CENSUS]") == std::string::npos, name);
  }
  static void positive() {
    NativeCensusTestAccess f;
    auto& b = *f.backend;
    f.record();
    f.completed();
    check(!b.nbAdd("M post-frame-readback") && b.m_nbRecs[0].size() == 1,
          "frame_seal_closes_recording_before_diagnostic_readback");
    const auto actual = f.times();
    check(actual[0] && actual[1] > actual[0] && actual[2] >= actual[1], "real_copy_has_resolved_timestamp_interval");
    char expected[128];
    std::snprintf(expected, sizeof(expected), "[NB_CENSUS] f=17 #1 gpu=%.4f R #1",
                  double(actual[1] - actual[0]) * 1000.0 / double(b.m_gtFreq));
    // Post-frame scanout reuses these scratch arrays; the seal must not.
    std::memset(b.m_gtTag[0], 0xFF, sizeof(b.m_gtTag[0]));
    std::memset(b.m_gtItem[0], 0xFF, sizeof(b.m_gtItem[0]));
    b.m_gtN = 1;
    b.m_gtOpen = true;
    b.m_frameIndex = 1;
    b.m_frameEpoch = 18;
    const auto submits = b.m_clExecutes;
    Capture capture;
    check(b.nbReplayFinish(), "completed_final_frame_drains_without_another_present");
    check(capture.text.find(expected) != std::string::npos, "sealed_metadata_attributes_actual_resolved_gpu_interval");
    check(occurrences(capture.text, "#1 gpu=") == 1 && occurrences(capture.text, "summary resolves 1 ") == 1 &&
          occurrences(capture.text, "native declined:") == 1, "record_summary_and_declines_each_dump_once");
    check(b.m_nbRecs[0].empty() && !b.m_gtSlotN[0] && !b.m_nbFrameOn && b.m_frameEpoch == 18 &&
          b.m_clExecutes == submits && b.m_nbReplayFrames == 1, "finish_does_not_arm_render_or_advance_frame");
    const std::string first = capture.text;
    check(b.nbReplayFinish() && capture.text == first, "second_finish_cannot_duplicate_inventory");
  }
  static void negatives() {
    {
      NativeCensusTestAccess f; f.record(false);
      f.rejects("unsubmitted_resolve_list_cannot_be_completed_by_checkpoint");
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      f.record(false, 18);  // same query slot still contains frame 17's valid bytes
      auto& b = *f.backend;
      const auto staleCommands = b.m_commandList;
      b.clReset(D3D12Backend::Pm4Reset); // new immutable recording identity
      b.clClose(D3D12Backend::Pm4Close);
      b.m_commandList = staleCommands; // equal exposed COM identity cannot substitute for ownership
      b.clExecute(D3D12Backend::Pm4Exec);
      check(!b.m_nbReplaySlots[0].submission, "replacement_recording_does_not_join_stale_list_identity");
      f.rejects("reused_slot_stale_timestamps_do_not_prove_abandoned_frame");
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      f.backend->m_gtSlotN[0] = 0;
      f.rejects("missing_resolved_count_retains_pending_inventory");
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      f.backend->m_gtSlotN[0] = f.backend->m_nbReplaySlots[0].count = D3D12Backend::kGtPerSlot - 1;
      f.rejects("timestamp_capacity_cannot_be_reported_as_complete");
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      const auto original = f.backend->m_gtReadback;
      f.backend->m_gtReadback = buffer(f.backend->m_device.Get(), D3D12_HEAP_TYPE_DEFAULT);
      f.rejects("actual_query_map_failure_retains_pending_inventory");
      f.backend->m_gtReadback = original;
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      void* mapped = nullptr;
      const D3D12_RANGE noRead{0, 0};
      require(f.backend->m_gtReadback->Map(0, &noRead, &mapped), "missing timestamp control map");
      static_cast<uint64_t*>(mapped)[0] = 0;
      const D3D12_RANGE written{0, sizeof(uint64_t)};
      f.backend->m_gtReadback->Unmap(0, &written);
      f.rejects("missing_actual_timestamp_cannot_be_replaced_by_zero_time");
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      require(f.backend->m_nativeImages.shutdown(10000), "close queue admission");
      f.rejects("closed_real_queue_cannot_authorize_completion");
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      f.backend->clReset(D3D12Backend::Pm4Reset);
      f.rejects("open_recording_refuses_finalization");
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      f.backend->m_rtOn = true;
      f.rejects("render_worker_ownership_refuses_finalization");
    }
    {
      NativeCensusTestAccess f; f.record(); f.completed();
      f.backend->m_nbRecs[0].push_back({"M unsealed", 0});
      f.rejects("late_unsealed_record_cannot_inherit_old_timestamps");
    }
  }
  static void frameCoverage() {
    {
      NativeCensusTestAccess f;
      f.record(true, 2, 1); f.completed();
      f.record(true, 3, 0); f.completed();
      Capture capture;
      check(f.backend->nbReplayFinish() &&
            capture.text.find("[NB_CENSUS] f=2 #1") < capture.text.find("[NB_CENSUS] f=3 #1") &&
            occurrences(capture.text, "#1 gpu=") == 2,
            "pending_wrapped_slots_dump_in_recorded_frame_order");
    }
    {
      NativeCensusTestAccess f;
      f.record(true, 9, 0, false); f.completed();
      Capture capture;
      check(f.backend->nbReplayFinish() &&
            capture.text.find("f=9 summary resolves 0 ") != std::string::npos &&
            capture.text.find("complete=1 frames=1 dumped_frames=1 records=0") != std::string::npos &&
            occurrences(capture.text, "native declined:") == 1,
            "empty_captured_frame_emits_explicit_zero_inventory_after_completion");
    }
  }
  static void completionFence() {
    NativeCensusTestAccess f;
    auto& b = *f.backend;
    struct ReleaseFence {
      ComPtr<ID3D12Fence> value;
      ~ReleaseFence() { if (value) value->Signal(1); }
    } release;
    require(b.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&release.value)), "blocking fence");
    require(b.m_commandQueue->Wait(release.value.Get(), 1), "block actual queue");
    f.record();
    std::promise<void> started;
    auto entered = started.get_future();
    auto work = std::async(std::launch::async, [&] {
      Capture capture;
      started.set_value();
      const bool ok = b.nbReplayFinish();
      return std::make_pair(ok, capture.text);
    });
    entered.get();
    const bool blocked = work.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
    require(release.value->Signal(1), "release actual queue");
    const auto result = work.get();
    check(blocked, "blocked_gpu_work_prevents_early_census_publication");
    check(result.first && occurrences(result.second, "complete=1") == 1 &&
          occurrences(result.second, "#1 gpu=") == 1,
          "real_queue_completion_releases_exact_pending_inventory");
  }
};
}  // namespace sr::gfx

int native_census_test_main(int, char**) {
  _putenv_s("SR_NB_CENSUS", "64");
  _putenv_s("SR_NB_CENSUS_EVERY", "1");
  _putenv_s("SR_NB_CENSUS_SKIP", "0");
  try {
    sr::gfx::NativeCensusTestAccess::positive();
    sr::gfx::NativeCensusTestAccess::negatives();
    sr::gfx::NativeCensusTestAccess::frameCoverage();
    sr::gfx::NativeCensusTestAccess::completionFence();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "[NATIVE_CENSUS_TEST] SETUP_FAILED %s\n", error.what());
    return 2;
  }
  std::printf("[NATIVE_CENSUS_TEST] COMPLETE passed=%u failed=%u\n", passed, failed);
  return failed ? 1 : 0;
}
