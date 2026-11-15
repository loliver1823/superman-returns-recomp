#include "native_image_store.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <new>
#include <numeric>
#include <thread>

namespace sr::gfx {

namespace {
bool sameDevice(ID3D12DeviceChild* child, ID3D12Device* device) {
  Microsoft::WRL::ComPtr<ID3D12Device> owner;
  Microsoft::WRL::ComPtr<IUnknown> a, b;
  return child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) &&
      SUCCEEDED(owner.As(&a)) && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
}
}

struct NativeImageDescriptorHeap {
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  uint32_t stride = 0, capacity = 0;
  mutable std::mutex mutex;
  std::vector<uint32_t> free;

  HRESULT initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = count;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) return hr;
    stride = device->GetDescriptorHandleIncrementSize(type);
    capacity = count;
    free.resize(count);
    std::iota(free.rbegin(), free.rend(), 0u);
    return S_OK;
  }
  bool take(uint32_t& slot) {
    std::lock_guard lock(mutex);
    if (free.empty()) return false;
    slot = free.back();
    free.pop_back();
    return true;
  }
  void give(uint32_t slot) {
    std::lock_guard lock(mutex);
    free.push_back(slot);  // Capacity was reserved for every slot at initialize.
  }
  uint32_t used() const {
    std::lock_guard lock(mutex);
    return capacity - uint32_t(free.size());
  }
  D3D12_CPU_DESCRIPTOR_HANDLE view(uint32_t slot) const {
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(slot) * stride;
    return h;
  }
};

struct NativeImageAllocation {
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  std::shared_ptr<NativeImageDescriptorHeap> descriptors;
  std::shared_ptr<const uint8_t> owner;
  NativeImageDesc desc;
  NativeImageHandle handle;
  uint32_t descriptor = 0;
  bool hasDescriptor = false;
  // R1a F1: the serial of the last recording that retained this allocation (0 = none; relaxed, see the lease).
  mutable std::atomic<uint64_t> retainedBy{0};
  ~NativeImageAllocation() {
    if (hasDescriptor) descriptors->give(descriptor);
  }
};

NativeImageHandle NativeImageLease::handle() const { return m_image ? m_image->handle : NativeImageHandle{}; }
ID3D12Resource* NativeImageLease::resource() const { return m_image ? m_image->resource.Get() : nullptr; }
ID3D12Device* NativeImageLease::device() const { return m_image ? m_image->device.Get() : nullptr; }
bool NativeImageLease::isNullColor() const { return m_image && m_image->desc.nullColorView; }
std::weak_ptr<const void> NativeImageLease::ownership() const { return m_image; }
bool NativeImageLease::retainedIn(uint64_t recordingSerial) const {
  return m_image && recordingSerial && m_image->retainedBy.load(std::memory_order_relaxed) == recordingSerial;
}
void NativeImageLease::markRetainedIn(uint64_t recordingSerial) const {
  if (m_image) m_image->retainedBy.store(recordingSerial, std::memory_order_relaxed);
}
D3D12_CPU_DESCRIPTOR_HANDLE NativeImageLease::view() const {
  return m_image ? m_image->descriptors->view(m_image->descriptor) : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
const NativeImageDesc* NativeImageLease::description() const { return m_image ? &m_image->desc : nullptr; }

NativeImageStore::~NativeImageStore() {
  // An unresolved GPU lifetime cannot be made safe by dropping COM references.
  if (FAILED(shutdown())) std::terminate();
}

HRESULT NativeImageStore::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                                    uint32_t colorCapacity, uint32_t depthCapacity) {
  std::lock_guard lock(m_mutex);
  if (!device || !queue || (!colorCapacity && !depthCapacity) ||
      queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
  if (m_device) return m_accepting && m_device.Get() == device && m_queue.Get() == queue &&
      (m_colorHeap ? m_colorHeap->capacity : 0) == colorCapacity &&
      (m_depthHeap ? m_depthHeap->capacity : 0) == depthCapacity ? S_FALSE : E_UNEXPECTED;
  Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
  HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&queueDevice));
  if (FAILED(hr)) return hr;
  Microsoft::WRL::ComPtr<IUnknown> a, b;
  if (FAILED(hr = device->QueryInterface(IID_PPV_ARGS(&a)))) return hr;
  if (FAILED(hr = queueDevice.As(&b))) return hr;
  if (a.Get() != b.Get()) return E_INVALIDARG;
  try {
    auto color = colorCapacity ? std::make_shared<NativeImageDescriptorHeap>() : nullptr;
    auto depth = depthCapacity ? std::make_shared<NativeImageDescriptorHeap>() : nullptr;
    if (color && FAILED(hr = color->initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, colorCapacity))) return hr;
    if (depth && FAILED(hr = depth->initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, depthCapacity))) return hr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return hr;
    auto identity = std::make_shared<const uint8_t>(0);
    m_device = device;
    m_queue = queue;
    m_fence = std::move(fence);
    m_colorHeap = std::move(color);
    m_depthHeap = std::move(depth);
    m_identity = std::move(identity);
    m_accepting = true;
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT NativeImageStore::create(const NativeImageDesc& desc, NativeImageLease& output) {
  std::lock_guard lock(m_mutex);
  if (!m_accepting) return E_ABORT;
  if (desc.nullColorView || !desc.width || !desc.height || desc.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      desc.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !desc.samples ||
      desc.resourceFormat == DXGI_FORMAT_UNKNOWN || desc.clear.Format == DXGI_FORMAT_UNKNOWN ||
      (desc.kind != NativeImageKind::Color && desc.kind != NativeImageKind::DepthStencil)) return E_INVALIDARG;
  const bool depth = desc.kind == NativeImageKind::DepthStencil;
  if (depth && (!desc.depthClearFlags || (desc.depthClearFlags & ~(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL)))) return E_INVALIDARG;
  if (depth && (desc.depthClearFlags & D3D12_CLEAR_FLAG_STENCIL) &&
      desc.clear.Format != DXGI_FORMAT_D24_UNORM_S8_UINT &&
      desc.clear.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT) return E_INVALIDARG;
  const auto& heap = depth ? m_depthHeap : m_colorHeap;
  if (!heap) return E_OUTOFMEMORY;
  try {
    auto image = std::make_shared<NativeImageAllocation>();
    image->device = m_device;
    image->descriptors = heap;
    image->owner = m_identity;
    image->desc = desc;
    if (!heap->take(image->descriptor)) return E_OUTOFMEMORY;
    image->hasDescriptor = true;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = desc.resourceFormat;
    rd.SampleDesc = {desc.samples, desc.sampleQuality};
    rd.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        desc.initialState, &desc.clear, IID_PPV_ARGS(&image->resource));
    if (FAILED(hr)) return hr;
    if (depth) {
      D3D12_DEPTH_STENCIL_VIEW_DESC view{};
      view.Format = desc.clear.Format;
      view.ViewDimension = desc.samples == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DMS;
      m_device->CreateDepthStencilView(image->resource.Get(), &view, heap->view(image->descriptor));
    } else {
      D3D12_RENDER_TARGET_VIEW_DESC view{};
      view.Format = desc.clear.Format;
      view.ViewDimension = desc.samples == 1 ? D3D12_RTV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2DMS;
      m_device->CreateRenderTargetView(image->resource.Get(), &view, heap->view(image->descriptor));
    }
    return publish(image, output);
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT NativeImageStore::publish(const std::shared_ptr<NativeImageAllocation>& image, NativeImageLease& output) {
  // Called with m_mutex held. Publish only after native allocation/view creation.
  size_t slot = 0;
  while (slot < m_slots.size() && (m_slots[slot].image || m_slots[slot].generation == UINT64_MAX)) ++slot;
  if (slot >= UINT32_MAX) return E_OUTOFMEMORY;
  if (slot == m_slots.size()) m_slots.emplace_back();
  auto& entry = m_slots[slot];
  image->handle = {uint32_t(slot + 1), entry.generation + 1};
  entry.generation = image->handle.generation;
  entry.image = image;
  output.m_image = image;
  return S_OK;
}
HRESULT NativeImageStore::createNullColor(DXGI_FORMAT format, DXGI_SAMPLE_DESC samples, NativeImageLease& output) {
  std::lock_guard lock(m_mutex);
  if (!m_accepting) return E_ABORT;
  if (!samples.Count || samples.Count > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT ||
      (samples.Count & (samples.Count - 1)) || format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
  HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
  if (FAILED(hr)) return hr;
  if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) return E_INVALIDARG;
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format, samples.Count, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
  hr = m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality));
  if (FAILED(hr)) return hr;
  if (samples.Quality >= quality.NumQualityLevels) return E_INVALIDARG;
  if (!m_colorHeap) return E_OUTOFMEMORY;
  try {
    auto image = std::make_shared<NativeImageAllocation>(); image->device = m_device;
    image->descriptors = m_colorHeap; image->owner = m_identity;
    image->desc.resourceFormat = image->desc.clear.Format = format;
    image->desc.samples = samples.Count; image->desc.sampleQuality = samples.Quality; image->desc.nullColorView = true;
    if (!m_colorHeap->take(image->descriptor)) return E_OUTOFMEMORY;
    image->hasDescriptor = true;
    D3D12_RENDER_TARGET_VIEW_DESC view{}; view.Format = format;
    view.ViewDimension = samples.Count == 1 ? D3D12_RTV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2DMS;
    m_device->CreateRenderTargetView(nullptr, &view, m_colorHeap->view(image->descriptor));
    return publish(image, output);
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT NativeImageStore::acquire(NativeImageHandle handle, NativeImageLease& output) const {
  std::lock_guard lock(m_mutex);
  if (!m_accepting) return E_ABORT;
  if (!handle.slot || handle.slot > m_slots.size()) return E_INVALIDARG;
  const auto& entry = m_slots[handle.slot - 1];
  if (!entry.image || entry.generation != handle.generation) return E_INVALIDARG;
  output.m_image = entry.image;
  return S_OK;
}

HRESULT NativeImageStore::release(NativeImageHandle handle) {
  std::lock_guard lock(m_mutex);
  if (!m_accepting) return E_ABORT;
  if (!handle.slot || handle.slot > m_slots.size()) return E_INVALIDARG;
  auto& entry = m_slots[handle.slot - 1];
  if (!entry.image || entry.generation != handle.generation) return E_INVALIDARG;
  entry.image.reset();
  return S_OK;
}

HRESULT NativeImageStore::recordClear(ID3D12GraphicsCommandList* commands, const NativeImageLease& image) const {
  std::lock_guard lock(m_mutex);
  if (!m_accepting) return E_ABORT;
  if (!commands || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
      !image.m_image || image.m_image->desc.nullColorView || image.m_image->owner != m_identity ||
      !sameDevice(commands, m_device.Get())) return E_INVALIDARG;
  const auto& d = image.m_image->desc;
  if (d.kind == NativeImageKind::DepthStencil)
    commands->ClearDepthStencilView(image.view(), d.depthClearFlags, d.clear.DepthStencil.Depth,
                                   d.clear.DepthStencil.Stencil, 0, nullptr);
  else commands->ClearRenderTargetView(image.view(), d.clear.Color, 0, nullptr);
  return S_OK;
}

HRESULT NativeImageStore::submit(ID3D12CommandList* commands, ID3D12CommandAllocator* allocator,
                                std::span<const NativeImageLease> images, uint64_t& submission,
                                std::span<const std::shared_ptr<const void>> owners) {
  std::lock_guard lock(m_mutex);
  if (!m_accepting) return E_ABORT;
  if (!commands || !allocator || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
      m_nextFence == UINT64_MAX || !sameDevice(commands, m_device.Get()) ||
      !sameDevice(allocator, m_device.Get())) return E_INVALIDARG;
  for (const auto& image : images)
    if (!image.m_image || image.m_image->owner != m_identity) return E_INVALIDARG;
  for (const auto& owner : owners)
    if (!owner) return E_INVALIDARG;
  try {
    Submission pending;
    pending.fence = UINT64_MAX;  // Unsignaled work must never look completed.
    pending.commands = commands;
    pending.allocator = allocator;
    pending.images.assign(images.begin(), images.end());
    pending.owners.assign(owners.begin(), owners.end());
    m_pending.push_back(std::move(pending));
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
  m_queue->ExecuteCommandLists(1, &commands);
  const uint64_t value = m_nextFence++;
  const HRESULT hr = m_queue->Signal(m_fence.Get(), value);
  if (FAILED(hr)) { m_accepting = false; return hr; }
  m_pending.back().fence = value;
  m_lastSubmitted = value;
  submission = value;
  return S_OK;
}

HRESULT NativeImageStore::checkpoint(uint64_t& submission) {
  std::lock_guard lock(m_mutex);
  if (!m_accepting) return E_ABORT;
  if (m_nextFence == UINT64_MAX) return E_UNEXPECTED;
  const uint64_t value = m_nextFence++;
  const HRESULT hr = m_queue->Signal(m_fence.Get(), value);
  if (FAILED(hr)) { m_accepting = false; return hr; }
  m_lastSubmitted = submission = value;
  return S_OK;
}

HRESULT NativeImageStore::collect() {
  std::lock_guard lock(m_mutex);
  if (!m_fence) return E_UNEXPECTED;
  const uint64_t completed = m_fence->GetCompletedValue();
  if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
  std::erase_if(m_pending, [completed](const Submission& s) { return s.fence <= completed; });
  return S_OK;
}

HRESULT NativeImageStore::completionSnapshot(uint64_t& completed) const {
  std::lock_guard lock(m_mutex);
  completed = 0;
  if (!m_fence) return E_UNEXPECTED;
  const uint64_t value = m_fence->GetCompletedValue();
  if (value == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
  completed = value;
  return S_OK;
}

HRESULT NativeImageStore::wait(uint64_t submission, uint32_t timeoutMs) const {
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  {
    std::lock_guard lock(m_mutex);
    if (!m_fence || !submission || submission > m_lastSubmitted || timeoutMs > 60000) return E_INVALIDARG;
    fence = m_fence;
  }
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    const uint64_t completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (completed >= submission) return S_OK;
    if (std::chrono::steady_clock::now() >= end) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

HRESULT NativeImageStore::shutdown(uint32_t timeoutMs) {
  uint64_t last = 0;
  {
    std::lock_guard lock(m_mutex);
    if (!m_fence) return S_FALSE;
    if (m_closed) return S_FALSE;
    if (timeoutMs > 60000 || m_nextFence == UINT64_MAX) return E_INVALIDARG;
    m_accepting = false;
    last = m_nextFence++;
    const HRESULT hr = m_queue->Signal(m_fence.Get(), last);
    if (FAILED(hr)) return hr;
    m_lastSubmitted = last;
  }
  const HRESULT hr = wait(last, timeoutMs);
  if (FAILED(hr)) return hr;  // Retain allocations, descriptors and pending uses.
  std::lock_guard lock(m_mutex);
  m_pending.clear();
  for (auto& slot : m_slots) slot.image.reset();
  m_closed = true;
  return S_OK;
}

NativeImageStats NativeImageStore::stats() const {
  std::lock_guard lock(m_mutex);
  NativeImageStats result;
  for (const auto& slot : m_slots) result.live += bool(slot.image);
  result.pending = m_pending.size();
  result.colorDescriptors = m_colorHeap ? m_colorHeap->used() : 0;
  result.depthDescriptors = m_depthHeap ? m_depthHeap->used() : 0;
  return result;
}

}  // namespace sr::gfx
