#include "gfx/native_image_store.hpp"
#include "gfx/backend_publication.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;

namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[NATIVE_IMAGE_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "SETUP FAILED %s hr=%08lX\n", name, (unsigned long)hr);
    std::exit(2);
  }
}
NativeImageDesc colorDesc() {
  NativeImageDesc d;
  d.width = d.height = 8;
  d.resourceFormat = d.clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
  d.clear.Color[0] = 1.0f;
  d.clear.Color[1] = 0.25f;
  d.clear.Color[2] = 0.5f;
  d.clear.Color[3] = 1.0f;
  return d;
}
}

int main() {
  {
    BackendPublication<int> publication;
    int backend = 7, otherBackend = 9;
    publication.publish(&backend);
    auto lease = publication.acquire();
    check(lease.get() == &backend, "publication_acquires_backend_lease");
    check(!publication.retire(&otherBackend), "publication_cannot_retire_another_backend");
    auto moved = std::move(lease);
    check(!lease && moved.get() == &backend, "publication_lease_move_transfers_one_user");
    std::promise<bool> retired;
    auto finished = retired.get_future();
    std::thread worker([&] { retired.set_value(publication.retire(&backend)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool admissionStopped = false;
    do {
      auto probe = publication.acquire();
      if (!probe) { admissionStopped = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    check(admissionStopped && finished.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
          "publication_retirement_stops_new_users_and_waits_for_prior_user");
    moved = {};
    check(finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready && finished.get(),
          "publication_retirement_finishes_after_last_lease");
    worker.join();
    check(!publication.acquire(), "retired_publication_cannot_return_old_backend");
  }
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "native device");
  D3D12_COMMAND_QUEUE_DESC q{};
  q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  require(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "native queue");

  NativeImageStore store;
  NativeImageLease a, alias, b, output;
  check(store.initialize(nullptr, queue.Get(), 2, 1) == E_INVALIDARG, "null_device_rejected");
  require(store.initialize(device.Get(), queue.Get(), 2, 1), "store");
  check(store.initialize(device.Get(), queue.Get(), 2, 1) == S_FALSE, "same_initialization_idempotent");
  check(store.initialize(device.Get(), queue.Get(), 3, 1) == E_UNEXPECTED, "changed_capacity_rejected");
  const auto color = colorDesc();
  require(store.create(color, a), "first image");
  const auto old = a.handle();
  require(store.acquire(old, alias), "lease");
  check(a.resource() == alias.resource() && a.view().ptr == alias.view().ptr, "lease_identity");
  check(a.resource()->GetDesc().SampleDesc.Count == 1 && a.description()->width == 8, "explicit_native_properties");
  require(store.release(old), "release first handle");
  check(store.stats().live == 0 && store.stats().colorDescriptors == 1, "released_recording_lease_retains_descriptor");
  require(store.create(color, b), "replacement image");
  check(b.handle().slot == old.slot && b.handle().generation > old.generation &&
        b.resource() != a.resource(), "replacement_has_new_generation");
  output = b;
  check(store.acquire(old, output) == E_INVALIDARG && output.handle() == b.handle(), "stale_acquisition_preserves_output");
  check(store.release(old) == E_INVALIDARG && store.stats().live == 1, "stale_release_rejected");
  check(store.create(color, output) == E_OUTOFMEMORY && output.handle() == b.handle(), "held_descriptor_exhaustion");
  a = {}; alias = {};
  check(store.stats().colorDescriptors == 1, "abandoned_lease_frees_descriptor");
  auto invalid = color;
  invalid.width = 0;
  check(store.create(invalid, output) == E_INVALIDARG && output.handle() == b.handle(), "invalid_dimensions_preserve_output");
  invalid = color;
  invalid.resourceFormat = invalid.clear.Format = DXGI_FORMAT_D32_FLOAT;
  check(FAILED(store.create(invalid, output)) && output.handle() == b.handle() &&
        store.stats().colorDescriptors == 1 && store.stats().live == 1,
        "failed_native_allocation_returns_descriptor");

  NativeImageDesc depthDesc;
  depthDesc.width = depthDesc.height = 8;
  depthDesc.resourceFormat = DXGI_FORMAT_R24G8_TYPELESS;
  depthDesc.kind = NativeImageKind::DepthStencil;
  depthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  depthDesc.clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  depthDesc.clear.DepthStencil = {0.5f, 37};
  depthDesc.depthClearFlags = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
  NativeImageLease depth;
  require(store.create(depthDesc, depth), "depth image");
  check(store.stats().depthDescriptors == 1 && depth.view().ptr != 0, "typed_depth_view");
  check(store.create(depthDesc, output) == E_OUTOFMEMORY && output.handle() == b.handle(), "depth_descriptor_exhaustion");

  NativeImageStore other;
  NativeImageLease foreign;
  require(other.initialize(device.Get(), queue.Get(), 1, 0), "other store");
  require(other.create(color, foreign), "foreign image");
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&commands)), "list");
  check(store.recordClear(commands.Get(), foreign) == E_INVALIDARG, "foreign_clear_rejected");
  for (const auto type : {D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY,
                          D3D12_COMMAND_LIST_TYPE_BUNDLE}) {
    ComPtr<ID3D12CommandAllocator> nonDirectAllocator;
    ComPtr<ID3D12GraphicsCommandList> nonDirect;
    require(device->CreateCommandAllocator(type, IID_PPV_ARGS(&nonDirectAllocator)), "non-direct allocator");
    require(device->CreateCommandList(0, type, nonDirectAllocator.Get(), nullptr,
                                    IID_PPV_ARGS(&nonDirect)), "non-direct list");
    check(store.recordClear(nonDirect.Get(), b) == E_INVALIDARG,
          type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute_clear_rejected" :
          type == D3D12_COMMAND_LIST_TYPE_COPY ? "copy_clear_rejected" : "bundle_clear_rejected");
    require(nonDirect->Close(), "close rejected list");
  }
  require(store.recordClear(commands.Get(), b), "color clear");
  require(store.recordClear(commands.Get(), depth), "depth clear");

  const auto rd = b.resource()->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowSize = 0, bytes = 0;
  device->GetCopyableFootprints(&rd, 0, 1, 0, &footprint, &rows, &rowSize, &bytes);
  ComPtr<ID3D12Resource> readback;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rb{};
  rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb.Width = bytes;
  rb.Height = rb.DepthOrArraySize = rb.MipLevels = 1;
  rb.SampleDesc.Count = 1;
  rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  require(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rb,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = b.resource();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  commands->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = b.resource();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  require(commands->Close(), "close recording");
  uint64_t value = 777;
  std::array<NativeImageLease, 1> foreignImages{foreign};
  check(store.submit(commands.Get(), allocator.Get(), foreignImages, value) == E_INVALIDARG &&
        value == 777 && store.stats().pending == 0, "foreign_submission_rejected_without_execution");

  // Actually record then discard a separate list. Its image cannot lose the
  // attachment descriptor while the recording lease is retained.
  ComPtr<ID3D12CommandAllocator> abandonedAllocator;
  ComPtr<ID3D12GraphicsCommandList> abandoned;
  require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&abandonedAllocator)), "abandoned allocator");
  require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, abandonedAllocator.Get(), nullptr,
                                  IID_PPV_ARGS(&abandoned)), "abandoned list");
  require(other.recordClear(abandoned.Get(), foreign), "unsubmitted clear");
  const auto foreignHandle = foreign.handle();
  const auto foreignView = foreign.view();
  require(other.release(foreign.handle()), "unsubmitted handle release");
  check(other.stats().colorDescriptors == 1, "unsubmitted_recording_retains_descriptor");
  require(abandoned->Close(), "close abandoned");
  require(abandonedAllocator->Reset(), "discard allocator");
  require(abandoned->Reset(abandonedAllocator.Get(), nullptr), "discard recording");
  require(abandoned->Close(), "close discarded list");
  foreign = {};
  // foreignImages still owns a lease, deliberately testing overlapping owners.
  check(other.stats().colorDescriptors == 1, "overlapping_recording_lease_retains_descriptor");
  foreignImages = {};
  check(other.stats().colorDescriptors == 0, "discarded_recording_releases_descriptor");
  require(other.create(color, foreign), "replacement after discard");
  check(foreign.view().ptr == foreignView.ptr && foreign.handle().generation > foreignHandle.generation,
        "descriptor_reuse_after_all_recordings_abandoned");

  ComPtr<ID3D12Fence> blocker;
  require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)), "blocker");
  require(queue->Wait(blocker.Get(), 1), "delay native queue");
  std::array<NativeImageLease, 2> images{b, depth};
  const auto colorHandle = b.handle(), depthHandle = depth.handle();
  require(store.submit(commands.Get(), allocator.Get(), images, value), "native submission");
  check(value == 1 && store.stats().pending == 1, "actual_submission_receipt");
  require(store.release(colorHandle), "submitted color release");
  require(store.release(depthHandle), "submitted depth release");
  images = {}; b = {}; depth = {}; output = {};
  allocator.Reset(); commands.Reset();
  check(store.stats().live == 0 && store.stats().pending == 1 && store.stats().colorDescriptors == 1 &&
        store.stats().depthDescriptors == 1, "submitted_uses_retain_resources_and_descriptors");
  require(store.collect(), "pending collect");
  check(store.stats().pending == 1 && store.wait(value, 0) == HRESULT_FROM_WIN32(WAIT_TIMEOUT),
        "delayed_fence_cannot_retire");
  uint64_t marker = 777;
  require(store.checkpoint(marker), "checked queue checkpoint");
  check(marker > value && store.wait(marker, 0) == HRESULT_FROM_WIN32(WAIT_TIMEOUT) &&
        store.stats().pending == 1, "checkpoint_waits_for_prior_queued_work");
  check(store.shutdown(0) == HRESULT_FROM_WIN32(WAIT_TIMEOUT) && store.stats().pending == 1 &&
        store.stats().colorDescriptors == 1, "shutdown_timeout_retains_pending_work");
  check(store.create(color, output) == E_ABORT && !output, "shutdown_stops_admission");
  uint64_t refusedMarker = 777;
  check(store.checkpoint(refusedMarker) == E_ABORT && refusedMarker == 777,
        "closed_checkpoint_preserves_output");
  require(blocker->Signal(1), "release queue delay");
  require(store.wait(value, 10000), "successful GPU completion");
  check(store.wait(marker, 10000) == S_OK, "checkpoint_completes_after_prior_work");
  require(store.collect(), "completed collect");
  check(store.stats().pending == 0 && store.stats().colorDescriptors == 0 && store.stats().depthDescriptors == 0,
        "completed_submission_releases_descriptors");
  uint8_t* pixels = nullptr;
  D3D12_RANGE readRange{0, SIZE_T(bytes)};
  require(readback->Map(0, &readRange, reinterpret_cast<void**>(&pixels)), "map pixels");
  bool clearMatches = true;
  const uint8_t expected[4] = {255, 64, 128, 255};
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 8; ++x)
      for (uint32_t c = 0; c < 4; ++c)
        clearMatches &= pixels[footprint.Offset + y * footprint.Footprint.RowPitch + x * 4 + c] == expected[c];
  D3D12_RANGE noWrites{0, 0};
  readback->Unmap(0, &noWrites);
  check(clearMatches, "actual_native_clear_and_copy_pixels");
  require(store.shutdown(10000), "shutdown completed queue");
  check(store.shutdown(0) == S_FALSE, "shutdown_idempotent");
  check(store.acquire(colorHandle, output) == E_ABORT, "closed_store_rejects_acquisition");
  require(other.shutdown(), "other shutdown");

  // No later submission or shutdown signal can accidentally mask a missing
  // checkpoint signal here: this fresh queue timeline is waited first.
  NativeImageStore checkpointStore;
  require(checkpointStore.initialize(device.Get(), queue.Get(), 1, 0), "checkpoint store");
  ComPtr<ID3D12Fence> checkpointBlocker;
  require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&checkpointBlocker)), "checkpoint blocker");
  require(queue->Wait(checkpointBlocker.Get(), 1), "delay standalone checkpoint");
  uint64_t isolatedMarker = 0;
  require(checkpointStore.checkpoint(isolatedMarker), "standalone checkpoint");
  check(checkpointStore.wait(isolatedMarker, 0) == HRESULT_FROM_WIN32(WAIT_TIMEOUT),
        "standalone_checkpoint_respects_queue_delay");
  require(checkpointBlocker->Signal(1), "release standalone checkpoint delay");
  check(checkpointStore.wait(isolatedMarker, 10000) == S_OK,
        "standalone_checkpoint_completes_without_later_signal");
  require(checkpointStore.shutdown(), "checkpoint store shutdown");

  NativeImageLease survivor;
  {
    NativeImageStore temporary;
    require(temporary.initialize(device.Get(), queue.Get(), 1, 0), "temporary store");
    require(temporary.create(color, survivor), "surviving lease");
    require(temporary.shutdown(), "temporary shutdown");
  }
  check(survivor.resource()->GetDesc().Width == 8 && survivor.view().ptr != 0,
        "lease_owns_resource_and_heap_after_store_destruction");
  std::printf("[NATIVE_IMAGE_TEST] passed=%u failed=%u\n", passed, failed);
  return failed ? 1 : 0;
}
