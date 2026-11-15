#include "translated_textures.hpp"
#include "translated_samplers.hpp"
#include <algorithm>
#include <cstring>
#include <new>

namespace sr::gfx {
namespace {
// 16384 * 48 = 786,432 descriptors in one shader-visible heap (D3D12 allows 1,000,000).
constexpr uint32_t kPageDescriptors = 16384, kMaxPages = 48, kCapacity = 2048;
// Generational paging. A cache hit on a table at least kMigrateAge pages behind
// the tail re-creates it at the tail, so hot tables move forward and old pages
// drain. At kPressurePages live pages, tables older than kKeepAge pages are
// evicted (not the whole cache: a full drop makes every draw miss at once).
constexpr uint64_t kMigrateAge = TranslatedTextureCache::kMigrateAge, kKeepAge = TranslatedTextureCache::kKeepAge;
constexpr size_t kPressurePages = TranslatedTextureCache::kPressurePages;
NativeViewRecipe nullView(D3D12_SRV_DIMENSION dimension) {
  NativeViewRecipe r;
  r.srv.ViewDimension = dimension;
  r.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  r.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (dimension == D3D12_SRV_DIMENSION_TEXTURE2D) r.srv.Texture2D.MipLevels = 1;
  else r.srv.Texture3D.MipLevels = 1;
  return r;
}
bool equal(const NativeViewRecipe& a, const NativeViewRecipe& b) {
  return a.resource.Get() == b.resource.Get() && a.buffer.resource() == b.buffer.resource() &&
      a.buffer.device() == b.buffer.device() &&
      a.buffer.offset() == b.buffer.offset() && a.buffer.size() == b.buffer.size() &&
      std::memcmp(&a.srv, &b.srv, sizeof(a.srv)) == 0;
}
uint64_t recipeHash(std::span<const NativeViewRecipe* const> textures) {
  uint64_t h = 14695981039346656037ull;
  auto mix = [&](const void* p, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(p);
    // Hash words without imposing alignment or aliasing requirements. This is
    // only a lookup filter; every hit still compares the complete recipe key.
    while (size >= sizeof(uint64_t)) {
      uint64_t word; std::memcpy(&word, bytes, sizeof(word));
      h = (h ^ word) * 1099511628211ull;
      bytes += sizeof(word); size -= sizeof(word);
    }
    if (size) {
      uint64_t tail = 0; std::memcpy(&tail, bytes, size);
      h = (h ^ tail) * 1099511628211ull;
    }
  };
  for (const auto* p : textures) {
    const auto& r = *p;
    const uint64_t identity[] = {uint64_t(reinterpret_cast<uintptr_t>(r.resource.Get())),
        uint64_t(reinterpret_cast<uintptr_t>(r.buffer.resource())),
        uint64_t(reinterpret_cast<uintptr_t>(r.buffer.device())), r.buffer.offset(), r.buffer.size()};
    mix(identity, sizeof(identity)); mix(&r.srv, sizeof(r.srv));
  }
  return h;
}
}

HRESULT TranslatedTextureCache::select(ID3D12Device* device,
    std::span<const NativeViewRecipe, 48> textures, const NativeViewRecipe& raw,
    TranslatedTextureSnapshot& output) {
  std::array<const NativeViewRecipe*, 48> pointers;
  for (uint32_t i = 0; i < 48; ++i) pointers[i] = &textures[i];
  return select(device, pointers, raw, output);
}

HRESULT TranslatedTextureCache::select(ID3D12Device* device,
    std::span<const NativeViewRecipe* const, 48> textures, const NativeViewRecipe& raw,
    TranslatedTextureSnapshot& output) {
  if (!device || (m_device && m_device.Get() != device) ||
      raw.srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return E_INVALIDARG;
  TranslatedTextureSnapshot next;
  for (uint32_t i = 0; i < 16; ++i) {
    if (!textures[i] || !textures[16 + i] || !textures[32 + i] ||
        textures[i]->srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D ||
        textures[32 + i]->srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURECUBE) return E_INVALIDARG;
    next.indices[i] = i;
    switch (textures[16 + i]->srv.ViewDimension) {
      case D3D12_SRV_DIMENSION_TEXTURE2D: next.indices[16 + i] = 0x80000000u | (16u + i); break;
      case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: next.indices[16 + i] = 0xC0000000u | i; break;
      case D3D12_SRV_DIMENSION_TEXTURE3D: next.indices[16 + i] = 0xA0000000u | i; break;
      default: return E_INVALIDARG;
    }
    next.indices[32 + i] = 0x80000000u | i;
  }
  try {
    const HRESULT init = m_arena.initialize(device, kPageDescriptors, kMaxPages);
    if (FAILED(init)) return init;
    const uint64_t hash = recipeHash(textures);
    auto hit = m_cache.end();
    for (auto [at, end] = m_index.equal_range(hash); at != end; ++at)
      if (std::equal(textures.begin(), textures.end(), at->second->recipes.begin(),
                     [](const NativeViewRecipe* a, const NativeViewRecipe& b) { return equal(*a, b); })) {
        hit = at->second; break;
      }
    const uint64_t tail = m_arena.tailSerial();
    const bool migrate = hit != m_cache.end() && tail >= hit->descriptors.pageSerial() + kMigrateAge;
    const bool rawCreated = !(m_lastRaw && equal(raw, m_lastRawRecipe));
    if ((hit == m_cache.end() || migrate || rawCreated) && m_arena.livePages() >= kPressurePages) {
      // Cached tables pin their pages. Drop the owners in old pages; those
      // pages return once the recordings that still use them complete.
      for (auto at = m_cache.begin(); at != m_cache.end();) {
        if (at == hit || at->descriptors.pageSerial() + kKeepAge > tail) { ++at; continue; }
        for (auto [i, end] = m_index.equal_range(at->hash); i != end; ++i)
          if (i->second == at) { m_index.erase(i); break; }
        at = m_cache.erase(at); ++m_tableEvictions;
      }
    }
    NativeDescriptorVersion table;
    if (hit != m_cache.end() && !migrate) table = hit->descriptors;
    else {
      std::array<NativeViewRecipe, TranslatedTextureSnapshot::kDescriptors> recipes;
      for (uint32_t i = 0; i < 16; ++i) {
        recipes[i] = *textures[i];
        const auto& layered = *textures[16 + i];
        const bool twoD = layered.srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D;
        recipes[16 + i] = twoD ? layered : nullView(D3D12_SRV_DIMENSION_TEXTURE2D);
        recipes[32 + i] = twoD ? nullView(D3D12_SRV_DIMENSION_TEXTURE3D) : layered;
        recipes[48 + i] = *textures[32 + i];
      }
      const HRESULT hr = m_arena.create(recipes, table);
      if (FAILED(hr)) return hr;
    }
    NativeDescriptorVersion rawVersion;
    bool rawMade = false;
    if (const HRESULT hr = selectRaw(raw, rawVersion, rawMade); FAILED(hr)) return hr;
    // Commit. Cache hits borrowed the caller's recipes for lookup; only a newly
    // published immutable version acquires the strong resource/slice owners.
    if (hit != m_cache.end()) {
      if (migrate) { hit->descriptors = table; ++m_tableMigrations; }
      else ++m_tableHits;
      m_cache.splice(m_cache.begin(), m_cache, hit);
    }
    else {
      // Build the entry and its index slot before evicting, so an allocation
      // failure leaves the cache and the caller's snapshot unchanged.
      std::list<Entry> fresh(1);
      for (uint32_t i = 0; i < 48; ++i) fresh.front().recipes[i] = *textures[i];
      fresh.front().descriptors = table; fresh.front().hash = hash;
      m_index.reserve(m_index.size() + 1);
      m_cache.splice(m_cache.begin(), fresh);
      m_index.emplace(hash, m_cache.begin());
      if (m_cache.size() > kCapacity) {
        const auto last = std::prev(m_cache.end());
        for (auto [at, end] = m_index.equal_range(last->hash); at != end; ++at)
          if (at->second == last) { m_index.erase(at); break; }
        m_cache.pop_back();
      }
      ++m_tableMisses;
    }
    if (rawMade) { m_lastRawRecipe = raw; m_lastRaw = rawVersion; ++m_rawCreates; }
    next.descriptors = std::move(table); next.raw = std::move(rawVersion);
    m_device = device; output = std::move(next); return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT TranslatedTextureCache::selectRaw(const NativeViewRecipe& raw, NativeDescriptorVersion& output,
                                         bool& created) {
  created = !(m_lastRaw && equal(raw, m_lastRawRecipe));
  if (!created) { output = m_lastRaw; return S_OK; }
  return m_arena.create({&raw, 1}, output);
}

HRESULT TranslatedTextureCache::reuse(ID3D12Device* device, const TranslatedTextureSnapshot& previous,
                                     const NativeViewRecipe& raw, TranslatedTextureSnapshot& output) {
  if (!device || !m_device || m_device.Get() != device || !previous.descriptors ||
      raw.srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return E_INVALIDARG;
  try {
    NativeDescriptorVersion rawVersion;
    bool rawMade = false;
    if (const HRESULT hr = selectRaw(raw, rawVersion, rawMade); FAILED(hr)) return hr;
    if (rawMade) { m_lastRawRecipe = raw; m_lastRaw = rawVersion; ++m_rawCreates; }
    TranslatedTextureSnapshot next;
    next.indices = previous.indices; next.descriptors = previous.descriptors; next.raw = std::move(rawVersion);
    ++m_tableReuses; output = std::move(next); return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT TranslatedTextureSnapshot::bind(NativeDrawRecording& recording,
                                       const NativeDescriptorVersion& samplers) const {
  if (!descriptors || descriptors.count() != kDescriptors || !raw || raw.count() != 1 ||
      raw.heap() != descriptors.heap() || raw.type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
      !samplers || samplers.count() != kXlatSamplerTable || samplers.type() != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ||
      samplers.device() != descriptors.device()) return E_INVALIDARG;
  HRESULT hr = recording.keep(descriptors);
  if (FAILED(hr)) return hr;
  hr = recording.keep(raw);
  if (FAILED(hr)) return hr;
  hr = recording.keep(samplers);
  if (FAILED(hr)) return hr;
  auto* commands = recording.commands();
  ID3D12DescriptorHeap* heaps[] = {descriptors.heap(), samplers.heap()};
  commands->SetDescriptorHeaps(2, heaps);
  commands->SetGraphicsRootDescriptorTable(5, descriptors.gpu());
  commands->SetGraphicsRootDescriptorTable(6, descriptors.gpu(kLayeredBase));
  commands->SetGraphicsRootDescriptorTable(7, descriptors.gpu(kCubeBase));
  commands->SetGraphicsRootDescriptorTable(8, samplers.gpu());
  commands->SetGraphicsRootDescriptorTable(10, raw.gpu());
  commands->SetGraphicsRootDescriptorTable(11, descriptors.gpu(kLayeredBase));
  commands->SetGraphicsRootDescriptorTable(12, descriptors.gpu(kLayeredBase));
  commands->SetGraphicsRootDescriptorTable(13, descriptors.gpu(kCubeBase));
  return S_OK;
}

void TranslatedTextureCache::clear() {
  m_cache.clear(); m_index.clear(); m_lastRaw = {}; m_lastRawRecipe = {}; m_arena.discardTail();
}
}  // namespace sr::gfx
