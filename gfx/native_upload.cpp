#include "native_upload.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace sr::gfx {
struct NativeUploadBudget {
  std::atomic<uint64_t> live{0};
  uint64_t limit = 0;
};
// Retired standard-size pages, still mapped. A page reaches the pool only from its
// destructor, i.e. after its last slice/recording/submission owner is gone - the
// same point at which it used to be destroyed - so reuse never overwrites bytes a
// GPU submission can still read. Creating and destroying upload resources every
// frame (CreateAllocation / DestroyAllocation / GPU VA map-free, with driver waits
// inside) cost most of the native render thread before this pool.
struct NativeUploadPool {
  struct Free { Microsoft::WRL::ComPtr<ID3D12Resource> resource; uint8_t* mapped = nullptr; };
  std::mutex mutex;
  std::vector<Free> free;
  uint64_t pageBytes = 0;
  size_t maxPages = 0;
  ~NativeUploadPool() { for (auto& f : free) if (f.mapped) f.resource->Unmap(0, nullptr); }
};
struct NativeUploadPage {
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  std::shared_ptr<NativeUploadBudget> budget;
  std::weak_ptr<NativeUploadPool> pool;
  uint8_t* mapped = nullptr;
  uint64_t capacity = 0, cursor = 0;
  bool accounted = false;
  // R1a F2b: the serial of the last recording that retained this page (0 = none). Relaxed: a stale value can only
  // make a recording push a slice it already covers (a duplicate), never skip one (FREEZE_R1A section 2).
  mutable std::atomic<uint64_t> retainedBy{0};
  ~NativeUploadPage() {
    if (accounted) budget->live.fetch_sub(capacity);
    if (mapped) {
      if (auto p = pool.lock(); p && capacity == p->pageBytes) {
        std::lock_guard lock(p->mutex);
        if (p->free.size() < p->maxPages) {
          try { p->free.push_back({std::move(resource), mapped}); return; } catch (const std::bad_alloc&) {}
        }
      }
      if (resource) resource->Unmap(0, nullptr);
    }
  }
};
// The per-slice owner of SR_RENDER_KEEP_LEAN=0 (the retention of before R1a): it holds the page; the slice's owner is
// an aliasing pointer to the page sharing this object's control block, so ownership() and retention are per slice.
struct NativeUploadAllocation {
  std::shared_ptr<NativeUploadPage> page;
};

namespace {
std::atomic<int> g_renderKeepLeanForce{-1};
}  // namespace
bool renderKeepLean() {
  const int forced = g_renderKeepLeanForce.load(std::memory_order_relaxed);
  if (forced >= 0) return forced != 0;
  static const bool on = [] {
    const char* e = std::getenv("SR_RENDER_KEEP_LEAN");
    return !(e && e[0] == '0');
  }();
  return on;
}
void renderKeepLeanForceForTest(int mode) { g_renderKeepLeanForce.store(mode, std::memory_order_relaxed); }

ID3D12Resource* NativeBufferSlice::resource() const { return m_page ? m_page->resource.Get() : nullptr; }
ID3D12Device* NativeBufferSlice::device() const { return m_page ? m_page->device.Get() : nullptr; }
uint64_t NativeBufferSlice::offset() const { return m_page ? m_offset : 0; }
uint64_t NativeBufferSlice::size() const { return m_page ? m_size : 0; }
D3D12_GPU_VIRTUAL_ADDRESS NativeBufferSlice::address() const {
  return m_page ? resource()->GetGPUVirtualAddress() + offset() : 0;
}
std::span<const uint8_t> NativeBufferSlice::bytes() const {
  return m_page ? std::span<const uint8_t>(m_page->mapped + offset(), size()) : std::span<const uint8_t>();
}
bool NativeBufferSlice::hasZeroPadding(uint64_t begin, uint64_t end) const {
  return m_page && begin <= end && end <= size() && (begin == end || begin >= m_meaningful);
}
std::weak_ptr<const void> NativeBufferSlice::ownership() const { return m_page; }
bool NativeBufferSlice::pageRetainedIn(uint64_t recordingSerial) const {
  return m_page && recordingSerial && m_page->retainedBy.load(std::memory_order_relaxed) == recordingSerial;
}
void NativeBufferSlice::markPageRetainedIn(uint64_t recordingSerial) const {
  if (m_page) m_page->retainedBy.store(recordingSerial, std::memory_order_relaxed);
}

HRESULT NativeUploadArena::initialize(ID3D12Device* device, uint64_t pageBytes, uint64_t budgetBytes) {
  constexpr uint64_t granularity = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  if (!device || pageBytes < granularity || pageBytes % granularity || budgetBytes < pageBytes ||
      budgetBytes % granularity) return E_INVALIDARG;
  std::lock_guard lock(m_mutex);
  if (m_device) return m_device.Get() == device && m_pageBytes == pageBytes &&
      m_budget->limit == budgetBytes ? S_FALSE : E_UNEXPECTED;
  try {
    auto budget = std::make_shared<NativeUploadBudget>();
    budget->limit = budgetBytes;
    auto pool = std::make_shared<NativeUploadPool>();
    pool->pageBytes = pageBytes;
    pool->maxPages = size_t(std::max<uint64_t>(budgetBytes / pageBytes / 2, 1));
    m_budget = std::move(budget); m_pool = std::move(pool); m_device = device; m_pageBytes = pageBytes;
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT NativeUploadArena::stage(std::span<const uint8_t> bytes, uint32_t alignment,
                                 NativeBufferSlice& output) {
  return stagePadded(bytes, alignment, bytes.size(), output);
}
HRESULT NativeUploadArena::stageConstantBuffer(std::span<const uint8_t> bytes, NativeBufferSlice& output) {
  if (bytes.empty() || bytes.size() > 65536) return E_INVALIDARG;
  return stagePadded(bytes, 256, (uint64_t(bytes.size()) + 255) & ~uint64_t(255), output);
}
HRESULT NativeUploadArena::stageConstantParts(std::span<const Part> parts, uint64_t bytes,
                                             NativeBufferSlice& output, uint64_t reserveBytes) {
  if (!bytes || bytes > 65536 || parts.empty() || reserveBytes > 65536) return E_INVALIDARG;
  uint64_t end = 0;
  for (const auto& part : parts) {
    if (part.bytes.empty() || part.offset < end || part.offset > bytes ||
        part.bytes.size() > bytes - part.offset) return E_INVALIDARG;
    end = part.offset + part.bytes.size();
  }
  return stageWrite(parts, bytes, 256, (bytes + 255) & ~uint64_t(255), output, reserveBytes);
}
HRESULT NativeUploadArena::stagePadded(std::span<const uint8_t> bytes, uint32_t alignment,
                                      uint64_t paddedBytes, NativeBufferSlice& output) {
  if (bytes.empty()) return E_INVALIDARG;
  const Part whole{0, bytes};
  return stageWrite({&whole, 1}, bytes.size(), alignment, paddedBytes, output);
}
HRESULT NativeUploadArena::stageWrite(std::span<const Part> parts, uint64_t meaningful, uint32_t alignment,
                                     uint64_t paddedBytes, NativeBufferSlice& output, uint64_t reserveBytes) {
  if (!meaningful || meaningful > paddedBytes || !alignment || (alignment & (alignment - 1)) || alignment > 65536)
    return E_INVALIDARG;
  std::lock_guard lock(m_mutex);
  if (!m_device) return E_ABORT;
  constexpr uint64_t granularity = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  // The page must span `fits` bytes from the slice's start; the cursor advances by paddedBytes.
  const uint64_t fits = std::max(paddedBytes, reserveBytes);
  if (fits > m_budget->limit || fits > UINT64_MAX - (granularity - 1)) return E_OUTOFMEMORY;
  try {
    // Allocate the slice before reserving/writing a range, so allocation failure
    // cannot publish a partial range or change the caller's previous output.
    // R1a: only SR_RENDER_KEEP_LEAN=0 allocates a per-slice owner; on, the slice owns its page directly.
    const bool lean = renderKeepLean();
    std::shared_ptr<NativeUploadAllocation> allocation = lean ? nullptr : std::make_shared<NativeUploadAllocation>();
    auto page = m_tail;
    const uint64_t aligned = page ? (page->cursor + alignment - 1) & ~(uint64_t(alignment) - 1) : 0;
    if (!page || aligned > page->capacity || fits > page->capacity - aligned) {
      // An arena's tail alone is not an in-flight use. Abandon it, but retained
      // slices keep old storage and budget alive without a frame-based shortcut.
      page.reset(); m_tail.reset();
      const uint64_t capacity = std::max(m_pageBytes,
          (fits + granularity - 1) & ~(granularity - 1));
      const uint64_t live = m_budget->live.load();
      if (capacity > m_budget->limit || live > m_budget->limit - capacity) return E_OUTOFMEMORY;
      page = std::make_shared<NativeUploadPage>();
      page->device = m_device; page->budget = m_budget; page->capacity = capacity; page->pool = m_pool;
      if (capacity == m_pageBytes) {
        std::lock_guard poolLock(m_pool->mutex);
        if (!m_pool->free.empty()) {
          page->resource = std::move(m_pool->free.back().resource);
          page->mapped = m_pool->free.back().mapped;
          m_pool->free.pop_back();
        }
      }
    }
    if (!page->accounted) {
      // A new page: a pooled resource (already mapped) or a new committed one.
      const uint64_t capacity = page->capacity;
      if (!page->mapped) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = capacity;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&page->resource));
        if (FAILED(hr)) return hr;
        const D3D12_RANGE noRead{0, 0};
        hr = page->resource->Map(0, &noRead, reinterpret_cast<void**>(&page->mapped));
        if (FAILED(hr)) return hr;
      }
      m_budget->live.fetch_add(capacity); page->accounted = true;
    }
    const uint64_t offset = (page->cursor + alignment - 1) & ~(uint64_t(alignment) - 1);
    std::shared_ptr<const NativeUploadPage> owner;
    if (allocation) {
      allocation->page = page;
      owner = std::shared_ptr<const NativeUploadPage>(allocation, allocation->page.get());  // aliasing: per slice
    } else {
      owner = page;
    }
    uint8_t* const at = page->mapped + offset;
    uint64_t cursor = 0;  // parts are ordered and disjoint (checked by the callers)
    for (const auto& part : parts) {
      if (part.offset > cursor) std::memset(at + cursor, 0, part.offset - cursor);
      std::memcpy(at + part.offset, part.bytes.data(), part.bytes.size());
      cursor = part.offset + part.bytes.size();
    }
    if (paddedBytes > cursor) std::memset(at + cursor, 0, paddedBytes - cursor);
    page->cursor = offset + paddedBytes;
    m_tail = std::move(page);
    output.m_page = std::move(owner);
    output.m_offset = offset; output.m_size = paddedBytes; output.m_meaningful = meaningful;
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
uint64_t NativeUploadArena::liveBytes() const {
  std::lock_guard lock(m_mutex);
  return m_budget ? m_budget->live.load() : 0;
}
void NativeUploadArena::discardTail() { std::lock_guard lock(m_mutex); m_tail.reset(); }
}  // namespace sr::gfx
