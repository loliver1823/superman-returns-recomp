#include "native_pipeline.hpp"
#include "../kernel/host_work.hpp"   // step 4 2k(4): a pipeline compile on a miss is host work (SR_GPU_HANG_HOST_WORK)
#include "../kernel/sr_oracle.hpp"   // step 4 (Astra 10:2x #7): SR_PSO_CREATE_STALL_MS is Oracle-only
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

namespace sr::gfx {

struct NativeShaderData {
  std::vector<uint8_t> bytes;
  uint64_t hash = 0;
};

struct NativePipelineAllocation {
  NativePipelineDesc desc;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  uint64_t sequence = 0;                       // 2k: creation order
  bool warm = false;                           // 2k: created by a Warm acquire
  mutable std::atomic<bool> drawHit{false};    // 2k: a draw asked for it after the warm-up made it
};

namespace {
// Step 4 2k (fix12): a draw waiting on the warm-up's compile lends the below-normal worker its own priority, so the
// compile it waits for is never starved behind other threads (no priority inversion); pso_warm.cpp's worker resets
// its priority after each pipeline.
void boostWorker(uint32_t tid) {
  if (!tid || tid == GetCurrentThreadId()) return;
  if (HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid)) {
    const int mine = GetThreadPriority(GetCurrentThread());
    const int theirs = GetThreadPriority(h);
    if (mine != THREAD_PRIORITY_ERROR_RETURN && theirs != THREAD_PRIORITY_ERROR_RETURN && theirs < mine)
      SetThreadPriority(h, mine);
    CloseHandle(h);
  }
}
std::atomic<int> g_singleFlightForce{-1};
std::atomic<NativePipelineObserver> g_observer{nullptr};
#if SR_ORACLE   // the test seams (step 6, Astra on 6af7487e): dev / difftest builds only, never the release exe
std::atomic<NativePipelineCache::CreateGateForTest> g_createGate{nullptr};
std::atomic<uint32_t> g_insertFailsForTest{0};
#endif
}  // namespace

bool NativePipelineCache::singleFlight() {
  const int f = g_singleFlightForce.load(std::memory_order_relaxed);
  if (f >= 0) return f != 0;
  static const bool on = [] {
    const char* e = std::getenv("SR_PSO_SINGLE_FLIGHT");
    return e && e[0] == '1';
  }();
  return on;
}
void NativePipelineCache::setSingleFlight(int on) { g_singleFlightForce.store(on, std::memory_order_relaxed); }
void NativePipelineCache::setObserver(NativePipelineObserver observer) {
  g_observer.store(observer, std::memory_order_release);
}
#if SR_ORACLE
void NativePipelineCache::setCreateGateForTest(CreateGateForTest gate) {
  g_createGate.store(gate, std::memory_order_release);
}
void NativePipelineCache::failInsertsForTest(uint32_t count) {
  g_insertFailsForTest.store(count, std::memory_order_release);
}
#endif

namespace {
uint64_t hashBytes(std::span<const uint8_t> bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
  return hash;
}

bool validDescription(const NativePipelineDesc& desc) {
  if (!desc.rootSignature || !desc.vertex || desc.colorCount > desc.colorFormats.size() ||
      desc.inputLayout.size() > 32 || !desc.samples.Count || desc.samples.Count > 32 ||
      (desc.samples.Count & (desc.samples.Count - 1)) ||
      desc.topology < D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT ||
      desc.topology > D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH ||
      !std::isfinite(desc.rasterizer.DepthBiasClamp) ||
      !std::isfinite(desc.rasterizer.SlopeScaledDepthBias)) return false;
  for (const auto& element : desc.inputLayout) {
    if (element.semantic.empty() || element.semantic.size() > 255 ||
        element.semantic.find('\0') != std::string::npos || element.inputSlot >= 32 ||
        element.format == DXGI_FORMAT_UNKNOWN ||
        (element.classification != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
         element.classification != D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA)) return false;
  }
  return true;
}
}

namespace {
// I1 (step 6, Fable ruling 21): the interning table - per content hash, the live codes made with that hash. A copy
// whose bytes equal a live code's (compared in full, once, here) shares that code's data. Entries are weak: a code
// nobody holds expires, and its slot is pruned the next time its hash is copied (and every 4096 copies, all of them).
struct InternTable {
  std::mutex mutex;
  std::unordered_multimap<uint64_t, std::weak_ptr<const NativeShaderData>> codes;   // guarded by mutex
  uint64_t copies = 0, hits = 0;                                                    // guarded by mutex
};
// never destroyed: a copy made while the process exits (a worker still running) never meets a destroyed table
InternTable& internTable() {
  static InternTable* const table = new InternTable;
  return *table;
}
}  // namespace

HRESULT NativeShaderCode::copy(std::span<const uint8_t> bytes, NativeShaderCode& output) {
  if (bytes.empty() || bytes.size() > 64u * 1024u * 1024u) return E_INVALIDARG;
  try {
    const uint64_t hash = hashBytes(bytes);
    InternTable& t = internTable();
    std::lock_guard lock(t.mutex);
    if ((++t.copies & 4095u) == 0) {
      for (auto it = t.codes.begin(); it != t.codes.end();) it = it->second.expired() ? t.codes.erase(it) : std::next(it);
    }
    const auto [first, last] = t.codes.equal_range(hash);
    for (auto it = first; it != last;) {
      std::shared_ptr<const NativeShaderData> live = it->second.lock();
      if (!live) {
        it = t.codes.erase(it);
        continue;
      }
      if (live->bytes.size() == bytes.size() && std::equal(bytes.begin(), bytes.end(), live->bytes.begin())) {
        ++t.hits;
        output.m_data = std::move(live);
        return S_OK;
      }
      ++it;
    }
    auto data = std::make_shared<NativeShaderData>();
    data->bytes.assign(bytes.begin(), bytes.end());
    data->hash = hash;
    try {
      t.codes.emplace(hash, data);
    } catch (const std::bad_alloc&) {
      // not interned: the copy is still a complete, correct code (equality falls back to comparing bytes)
    }
    output.m_data = std::move(data);
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

uint64_t NativeShaderCode::internedCopies() {
  InternTable& t = internTable();
  std::lock_guard lock(t.mutex);
  return t.hits;
}

std::span<const uint8_t> NativeShaderCode::bytes() const {
  return m_data ? std::span<const uint8_t>(m_data->bytes) : std::span<const uint8_t>();
}
uint64_t NativeShaderCode::hash() const { return m_data ? m_data->hash : 0; }
bool NativeShaderCode::operator==(const NativeShaderCode& other) const {
  if (m_data == other.m_data) return true;
  return hash() == other.hash() && bytes().size() == other.bytes().size() &&
      std::equal(bytes().begin(), bytes().end(), other.bytes().begin());
}

ID3D12PipelineState* NativePipelineLease::pipeline() const {
  return m_pipeline ? m_pipeline->pipeline.Get() : nullptr;
}
const NativePipelineDesc* NativePipelineLease::description() const {
  return m_pipeline ? &m_pipeline->desc : nullptr;
}
std::weak_ptr<const void> NativePipelineLease::ownership() const { return m_pipeline; }

namespace {
// I1 (step 6, Fable ruling 21): the key's state words - every descriptor field CreateGraphicsPipelineState reads
// (create() below) - in a fixed order, each named (name, index) for the mutant test; the production key keeps the
// values only (the names are compile-time literals, unused outside the SR_ORACLE test seams).
// The input layout is encoded per element as [semantic size, its bytes, six fields]: self-delimiting, and the key's
// word count is compared, so the element COUNT the key used to carry first was redundant - an equivalent mutant no
// test can kill (Fable (2)) - and is no longer a word. No two descriptors' equality changes.
template <class Add>
void forEachKeyWord(const NativePipelineDesc& d, Add&& add) {
  const auto addFloat = [&](const char* name, uint32_t index, float value) {
    add(name, index, uint64_t(std::bit_cast<uint32_t>(value)));
  };
  add("colorCount", 0, d.colorCount);
  for (uint32_t i = 0; i < d.colorFormats.size(); ++i) add("colorFormat", i, d.colorFormats[i]);
  add("depthFormat", 0, d.depthFormat); add("sampleCount", 0, d.samples.Count);
  add("sampleQuality", 0, d.samples.Quality);
  add("sampleMask", 0, d.sampleMask); add("nodeMask", 0, d.nodeMask); add("topology", 0, d.topology);
  add("stripCut", 0, d.stripCut); add("flags", 0, d.flags);
  const auto& r = d.rasterizer;
  add("fillMode", 0, r.FillMode); add("cullMode", 0, r.CullMode); add("frontCounterClockwise", 0, r.FrontCounterClockwise);
  add("depthBias", 0, uint32_t(r.DepthBias));
  addFloat("depthBiasClamp", 0, r.DepthBiasClamp); addFloat("slopeScaledDepthBias", 0, r.SlopeScaledDepthBias);
  add("depthClipEnable", 0, r.DepthClipEnable); add("multisampleEnable", 0, r.MultisampleEnable);
  add("antialiasedLineEnable", 0, r.AntialiasedLineEnable);
  add("forcedSampleCount", 0, r.ForcedSampleCount); add("conservativeRaster", 0, r.ConservativeRaster);
  add("alphaToCoverage", 0, d.blend.AlphaToCoverageEnable); add("independentBlend", 0, d.blend.IndependentBlendEnable);
  for (uint32_t i = 0; i < 8; ++i) {
    const auto& b = d.blend.RenderTarget[i];
    add("blendEnable", i, b.BlendEnable); add("logicOpEnable", i, b.LogicOpEnable); add("srcBlend", i, b.SrcBlend);
    add("destBlend", i, b.DestBlend); add("blendOp", i, b.BlendOp); add("srcBlendAlpha", i, b.SrcBlendAlpha);
    add("destBlendAlpha", i, b.DestBlendAlpha); add("blendOpAlpha", i, b.BlendOpAlpha); add("logicOp", i, b.LogicOp);
    add("renderTargetWriteMask", i, b.RenderTargetWriteMask);
  }
  const auto& z = d.depthStencil;
  add("depthEnable", 0, z.DepthEnable); add("depthWriteMask", 0, z.DepthWriteMask); add("depthFunc", 0, z.DepthFunc);
  add("stencilEnable", 0, z.StencilEnable); add("stencilReadMask", 0, z.StencilReadMask);
  add("stencilWriteMask", 0, z.StencilWriteMask);
  for (uint32_t f = 0; f < 2; ++f) {
    const auto& face = f ? z.BackFace : z.FrontFace;
    add("stencilFailOp", f, face.StencilFailOp); add("stencilDepthFailOp", f, face.StencilDepthFailOp);
    add("stencilPassOp", f, face.StencilPassOp); add("stencilFunc", f, face.StencilFunc);
  }
  for (uint32_t i = 0; i < d.inputLayout.size(); ++i) {
    const auto& e = d.inputLayout[i];
    add("semanticSize", i, e.semantic.size());
    for (uint32_t c = 0; c < e.semantic.size(); ++c) add("semanticByte", i * 256u + c, (unsigned char)e.semantic[c]);
    add("semanticIndex", i, e.semanticIndex); add("inputFormat", i, e.format); add("inputSlot", i, e.inputSlot);
    add("byteOffset", i, e.byteOffset); add("classification", i, e.classification);
    add("instanceStepRate", i, e.instanceStepRate);
  }
}

#if SR_ORACLE   // I1 (Fable ruling 21 (2)): the mutant seam - one key component left out of the key
std::atomic<const char*> g_dropName{nullptr};
std::atomic<uint32_t> g_dropIndex{0};
bool dropped(const char* name, uint32_t index) {
  const char* d = g_dropName.load(std::memory_order_acquire);
  if (!d) return false;
  const uint32_t i = g_dropIndex.load(std::memory_order_acquire);
  return (i == UINT32_MAX || i == index) && std::strcmp(d, name) == 0;   // UINT32_MAX: every index of that name
}
bool anyDropped() { return g_dropName.load(std::memory_order_acquire) != nullptr; }
#else
constexpr bool dropped(const char*, uint32_t) { return false; }
constexpr bool anyDropped() { return false; }
#endif

// the state words, honouring the mutant seam (a no-op outside SR_ORACLE)
void fillKeyWords(const NativePipelineDesc& d, std::vector<uint64_t>& words) {
  words.clear();
  if (!anyDropped()) {
    forEachKeyWord(d, [&](const char*, uint32_t, uint64_t value) { words.push_back(value); });
  } else {
    forEachKeyWord(d, [&](const char* name, uint32_t index, uint64_t value) {
      if (!dropped(name, index)) words.push_back(value);
    });
  }
}

// the hash the cache has always used (the root identity, the five shader hashes, the state words)
uint64_t mixKey(IUnknown* root, const std::array<const NativeShaderCode*, 5>& shaders, std::span<const uint64_t> state) {
  const bool mutant = anyDropped();
  uint64_t hash = mutant && dropped("root", 0) ? 0 : uint64_t(reinterpret_cast<uintptr_t>(root));
  const auto mix = [&](uint64_t value) { hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2); };
  for (uint32_t i = 0; i < 5; ++i) mix(mutant && dropped("shader", i) ? 0 : shaders[i]->hash());
  for (uint64_t value : state) mix(value);
  return hash;
}

bool sameKey(IUnknown* ra, const std::array<const NativeShaderCode*, 5>& sa, std::span<const uint64_t> wa,
             IUnknown* rb, const std::array<const NativeShaderCode*, 5>& sb, std::span<const uint64_t> wb) {
  const bool mutant = anyDropped();
  if (ra != rb && !(mutant && dropped("root", 0))) return false;
  // interned codes: equal bytes share their data, so this is a pointer compare (operator== still compares bytes
  // for a code that could not be interned)
  for (uint32_t i = 0; i < 5; ++i)
    if (!(mutant && dropped("shader", i)) && !(*sa[i] == *sb[i])) return false;
  return wa.size() == wb.size() && std::equal(wa.begin(), wa.end(), wb.begin());
}

std::array<const NativeShaderCode*, 5> shaderPointers(const std::array<NativeShaderCode, 5>& codes) {
  return {&codes[0], &codes[1], &codes[2], &codes[3], &codes[4]};
}
}  // namespace

bool NativePipelineCache::Key::operator==(const Key& other) const {
  return sameKey(rootIdentity.Get(), shaderPointers(shaders), state, other.rootIdentity.Get(),
                 shaderPointers(other.shaders), other.state);
}

bool NativePipelineCache::Equal::operator()(const Key& a, const KeyView& b) const {
  return sameKey(a.rootIdentity.Get(), shaderPointers(a.shaders), a.state, b.rootIdentity, b.shaders, b.state);
}

size_t NativePipelineCache::Hash::operator()(const Key& key) const {
  return size_t(mixKey(key.rootIdentity.Get(), shaderPointers(key.shaders), key.state));
}
size_t NativePipelineCache::Hash::operator()(const KeyView& key) const {
  return size_t(mixKey(key.rootIdentity, key.shaders, key.state));
}

NativePipelineCache::Key NativePipelineCache::makeKey(const NativePipelineDesc& d,
                                                     Microsoft::WRL::ComPtr<IUnknown> identity) {
  Key key{std::move(identity), {d.vertex, d.pixel, d.geometry, d.hull, d.domain}, {}};
  key.state.reserve(192 + d.inputLayout.size() * 24);
  fillKeyWords(d, key.state);
  return key;
}

NativePipelineCache::KeyView NativePipelineCache::viewKey(const NativePipelineDesc& d, IUnknown* identity) {
  // the calling thread's buffer: reserved once, refilled per lookup, never shrunk - a hit allocates nothing
  thread_local std::vector<uint64_t> t_words;
  if (t_words.capacity() < 192 + d.inputLayout.size() * 24) t_words.reserve(192 + d.inputLayout.size() * 24);
  fillKeyWords(d, t_words);
  return KeyView{identity, {&d.vertex, &d.pixel, &d.geometry, &d.hull, &d.domain}, t_words};
}

#if SR_ORACLE
std::vector<NativePipelineCache::KeyComponentForTest> NativePipelineCache::keyComponentsForTest(
    const NativePipelineDesc& d) {
  std::vector<KeyComponentForTest> out;
  out.push_back({"root", 0, uint64_t(reinterpret_cast<uintptr_t>(d.rootSignature.Get()))});
  const NativeShaderCode* codes[5] = {&d.vertex, &d.pixel, &d.geometry, &d.hull, &d.domain};
  for (uint32_t i = 0; i < 5; ++i) out.push_back({"shader", i, uint64_t(reinterpret_cast<uintptr_t>(codes[i]->identity()))});
  forEachKeyWord(d, [&](const char* name, uint32_t index, uint64_t value) { out.push_back({name, index, value}); });
  return out;
}
void NativePipelineCache::dropKeyComponentForTest(const char* name, uint32_t index) {
  g_dropIndex.store(index, std::memory_order_release);
  g_dropName.store(name, std::memory_order_release);
}
#endif

HRESULT NativePipelineCache::initialize(ID3D12Device* device, uint32_t capacity) {
  if (!device || !capacity) return E_INVALIDARG;
  std::lock_guard lock(m_mutex);
  if (m_device) {
    Microsoft::WRL::ComPtr<IUnknown> current, incoming;
    if (FAILED(m_device.As(&current)) || FAILED(device->QueryInterface(IID_PPV_ARGS(&incoming)))) return E_INVALIDARG;
    return current.Get() == incoming.Get() && m_capacity == capacity ? S_FALSE : E_UNEXPECTED;
  }
  m_device = device;
  m_capacity = capacity;
  return S_OK;
}

HRESULT NativePipelineCache::create(const NativePipelineDesc& desc,
                                    std::shared_ptr<NativePipelineAllocation>& allocation, double& ms) const {
  allocation = std::make_shared<NativePipelineAllocation>();
  allocation->desc = desc;
  const auto& d = allocation->desc;
  std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
  elements.reserve(d.inputLayout.size());
  for (const auto& element : d.inputLayout) {
    elements.push_back({element.semantic.c_str(), element.semanticIndex, element.format,
        element.inputSlot, element.byteOffset, element.classification, element.instanceStepRate});
  }
  const auto shader = [](const NativeShaderCode& code) {
    const auto bytes = code.bytes();
    return D3D12_SHADER_BYTECODE{bytes.empty() ? nullptr : bytes.data(), bytes.size()};
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
  native.pRootSignature = d.rootSignature.Get();
  native.VS = shader(d.vertex); native.PS = shader(d.pixel); native.GS = shader(d.geometry);
  native.HS = shader(d.hull); native.DS = shader(d.domain);
  native.InputLayout = {elements.empty() ? nullptr : elements.data(), UINT(elements.size())};
  native.RasterizerState = d.rasterizer;
  native.BlendState = d.blend;
  native.DepthStencilState = d.depthStencil;
  native.NumRenderTargets = d.colorCount;
  for (size_t i = 0; i < d.colorFormats.size(); ++i) native.RTVFormats[i] = d.colorFormats[i];
  native.DSVFormat = d.depthFormat; native.SampleDesc = d.samples; native.SampleMask = d.sampleMask;
  native.PrimitiveTopologyType = d.topology; native.IBStripCutValue = d.stripCut;
  native.NodeMask = d.nodeMask; native.Flags = d.flags;
  const auto t0 = std::chrono::steady_clock::now();
  HRESULT hr;
  {
    sr::kernel::HostWorkScope work(sr::kernel::kHostWorkPsoCompile);   // observe-only (the watchdog's classification)
    // Step 4 2k(4) test stimulus (diagnostic, default off): SR_PSO_CREATE_STALL_MS=<n> holds the FIRST create on a
    // presenting thread (a draw's, on the CP or the PM4 render thread) n ms inside the scope - a deterministic
    // pipeline-compile stall for the watchdog's classification A/B (Fable ruling 10 condition 2). Compiled out of
    // release (Astra 10:2x #7: a stall stimulus is a test device; docs/release_composition.json marker).
#if SR_ORACLE
    static const int s_stallMs = [] {
      const char* e = std::getenv("SR_PSO_CREATE_STALL_MS");
      const int v = e ? std::atoi(e) : 0;
      return v > 0 ? (v > 60000 ? 60000 : v) : 0;
    }();
    static std::atomic<bool> s_stalled{false};
    if (s_stallMs && sr::kernel::hostwork_detail::t_presenting && !s_stalled.exchange(true))
      std::this_thread::sleep_for(std::chrono::milliseconds(s_stallMs));
#endif  // SR_ORACLE (SR_PSO_CREATE_STALL_MS)
#if SR_ORACLE   // step 6 (Astra on 6af7487e): the test seams are not in the release configuration
    const auto gate = g_createGate.load(std::memory_order_acquire);
    hr = gate ? gate(d) : S_OK;   // tests: a gate may hold a create or fail it
#else
    hr = S_OK;
#endif
    if (SUCCEEDED(hr)) hr = m_device->CreateGraphicsPipelineState(&native, IID_PPV_ARGS(&allocation->pipeline));
  }
  ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return hr;
}

bool NativePipelineCache::evictUnusedWarm() {
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    if (it->second->warm && !it->second->drawHit.load(std::memory_order_relaxed)) {
      m_entries.erase(it);
      ++m_counters.warmEvicted;
      return true;
    }
  }
  return false;
}

// Step 6 (2g P2, Fable ruling 14): a pipeline the cache cannot store is a create failure. Callers keep only the raw
// pointer, which the cache's entry retains: an uncached pipeline would be released with the caller's lease while the
// draw still uses it.
bool NativePipelineCache::insertEntry(Key& key, const std::shared_ptr<NativePipelineAllocation>& allocation) {
  try {
#if SR_ORACLE   // step 6 (Astra on 6af7487e): the fault injector is not in the release configuration
    if (g_insertFailsForTest.load(std::memory_order_relaxed)) {   // tests only: an out-of-memory insert
      uint32_t n = g_insertFailsForTest.load(std::memory_order_acquire);
      while (n && !g_insertFailsForTest.compare_exchange_weak(n, n - 1, std::memory_order_acq_rel)) {
      }
      if (n) throw std::bad_alloc();
    }
#endif
    m_entries.emplace(std::move(key), allocation);
    return true;
  } catch (const std::bad_alloc&) {
    ++m_counters.insertFailures;
    ++m_counters.failures;
    return false;
  }
}

void NativePipelineCache::noteHit(const NativePipelineAllocation& allocation, NativePipelineAcquire mode) {
  ++m_counters.hits;
  if (mode == NativePipelineAcquire::Draw && allocation.warm && !allocation.drawHit.exchange(true))
    ++m_counters.warmUsed;
}

HRESULT NativePipelineCache::acquire(const NativePipelineDesc& desc, NativePipelineLease& output,
                                     NativePipelineAcquire mode) {
  std::unique_lock lock(m_mutex);
  if (!m_device) return E_ABORT;
  if (!validDescription(desc)) return E_INVALIDARG;
  // I1 (step 6, Fable ruling 21): the root signature's device check - four COM round trips - runs once per root
  // signature. m_memoRoot PINS the last one that passed (its address cannot be reused by another object while it is
  // held), and a root signature's device and identity never change; m_device never changes once set.
  IUnknown* identity = nullptr;
  if (desc.rootSignature.Get() == m_memoRoot.Get()) {   // validDescription: never null, so never the empty memo
    identity = m_memoIdentity.Get();
    ++m_counters.rootMemoHits;
  } else {
    Microsoft::WRL::ComPtr<ID3D12Device> rootDevice;
    Microsoft::WRL::ComPtr<IUnknown> rootIdentity, expected, actual;
    if (FAILED(desc.rootSignature->GetDevice(IID_PPV_ARGS(&rootDevice))) ||
        FAILED(desc.rootSignature.As(&rootIdentity)) || FAILED(m_device.As(&expected)) ||
        FAILED(rootDevice.As(&actual)) || actual.Get() != expected.Get()) return E_INVALIDARG;
    m_memoRoot = desc.rootSignature;
    m_memoIdentity = std::move(rootIdentity);
    identity = m_memoIdentity.Get();
  }
  const bool warm = mode == NativePipelineAcquire::Warm;
  const NativePipelineObserver observer = g_observer.load(std::memory_order_acquire);
  try {
    // I1: the lookup borrows its key (viewKey: the descriptor's codes, the thread's reused word buffer) - a hit builds
    // and frees nothing. The view is used for this one find only; a miss builds the owned Key and uses it throughout.
    ++m_counters.borrowedKeyLookups;
    if (const auto found = m_entries.find(viewKey(desc, identity)); found != m_entries.end()) {
      noteHit(*found->second, mode);
      output.m_pipeline = found->second;
      return S_FALSE;
    }
    auto key = makeKey(desc, Microsoft::WRL::ComPtr<IUnknown>(identity));
  retry:   // a draw that joined a warm flight looks again once it ends or it stops waiting (Astra 10:5x #6, fix12)
    if (const auto found = m_entries.find(key); found != m_entries.end()) {
      noteHit(*found->second, mode);
      output.m_pipeline = found->second;
      return S_FALSE;
    }
    // Astra P2 2 + (10:2x) #3: a draw must never fail because of the warm-up. A Warm acquire may fill at most half
    // the cache; a draw's admission counts the entries and the DRAWS' creates in flight only (a warm create in
    // flight holds no slot a draw needs: when it finishes into a full cache it is not cached, warmDropped), and a
    // draw that finds the cache full first evicts a warm-made entry no draw has asked for (it has no lease beyond
    // the warm-up's own, already dropped).
    const size_t drawInflight = m_inflight.size() - m_warmInflight;
    if (warm && m_entries.size() + m_inflight.size() >= m_capacity / 2) return E_OUTOFMEMORY;
    if (!warm && m_entries.size() + drawInflight >= m_capacity) evictUnusedWarm();
    if (!singleFlight()) {
      // the original path: the create under the cache lock
      if (m_entries.size() >= m_capacity) return E_OUTOFMEMORY;
      std::shared_ptr<NativePipelineAllocation> allocation;
      double ms = 0;
      const HRESULT hr = create(desc, allocation, ms);
      if (FAILED(hr)) {
        ++m_counters.failures;
        if (observer) observer({&desc, hr, ms, warm, 0});
        return hr;
      }
      allocation->sequence = ++m_sequence;
      allocation->warm = warm;
      ++m_counters.creates;
      if (warm) ++m_counters.warmCreates;
      if (!insertEntry(key, allocation)) {   // step 6 (2g P2): not stored, so not handed out
        if (observer) observer({&desc, E_OUTOFMEMORY, ms, warm, 0});
        return E_OUTOFMEMORY;
      }
      if (observer) observer({&allocation->desc, hr, ms, warm, allocation->sequence});
      output.m_pipeline = std::move(allocation);
      return S_OK;
    }
    // 2k(3a) single flight: one create per key at a time, outside the lock
    if (const auto flying = m_inflight.find(key); flying != m_inflight.end()) {
      const std::shared_ptr<InFlight> flight = flying->second;
      ++m_counters.flightWaits;
      if (!warm && flight->warm) {
        // fix12 (orchestrator, Astra on fix11): the warm-up compiles on a below-normal thread - a draw waiting on it
        // lends the worker its own priority and waits at most kWarmJoinWaitMs. Then the draw looks again: the cached
        // pipeline, or (that create failed, finished uncached, or is still running) its own create on the draw path
        // below - a still-running warm flight is unlisted first, so later draws join the draw's create, and the
        // worker finishes and keeps its own result (the first finisher fills the cache). The warm-up never fails a
        // draw (Astra 10:5x #6).
        boostWorker(flight->workerTid);
        if (!m_flightDone.wait_for(lock, std::chrono::milliseconds(kWarmJoinWaitMs), [&] { return flight->done; })) {
          ++m_counters.warmFlightTimeouts;
          if (flight->listed) {   // listed = still m_inflight[key]
            m_inflight.erase(key);
            --m_warmInflight;
            flight->listed = false;
          }
        } else if (FAILED(flight->hr)) {
          ++m_counters.warmFlightRetries;
        }
        goto retry;
      }
      m_flightDone.wait(lock, [&] { return flight->done; });
      if (FAILED(flight->hr)) return flight->hr;   // that create failed and left no entry: a later call retries
      noteHit(*flight->result, mode);
      output.m_pipeline = flight->result;
      return S_FALSE;
    }
    if (!warm && m_entries.size() + drawInflight >= m_capacity) return E_OUTOFMEMORY;
    const auto flight = std::make_shared<InFlight>();
    flight->warm = warm;
    flight->workerTid = GetCurrentThreadId();
    m_inflight.emplace(key, flight);
    if (warm) ++m_warmInflight;
    lock.unlock();
    std::shared_ptr<NativePipelineAllocation> allocation;
    double ms = 0;
    HRESULT hr;
    try {
      hr = create(desc, allocation, ms);
    } catch (const std::bad_alloc&) {
      hr = E_OUTOFMEMORY;
    }
    lock.lock();
    if (flight->listed) {   // a warm flight a draw stopped waiting on was unlisted already (fix12)
      m_inflight.erase(key);
      if (warm) --m_warmInflight;
    }
    flight->hr = hr;
    flight->done = true;
    std::shared_ptr<const NativePipelineAllocation> winner;
    if (SUCCEEDED(hr)) {
      allocation->sequence = ++m_sequence;
      allocation->warm = warm;
      ++m_counters.creates;
      if (warm) ++m_counters.warmCreates;
      if (const auto there = m_entries.find(key); there != m_entries.end()) {
        // fix12: the key was filled while this create ran (a draw that stopped waiting on this warm flight, or the
        // warm flight such a draw left): the first finisher stays cached, this copy is released - never two inserts,
        // and a draw always gets the cached pipeline (the backend keeps only the pointer; the cache retains it)
        ++m_counters.lostRaces;
        noteHit(*there->second, mode);
        winner = there->second;
        flight->result = winner;
      } else if (warm && m_entries.size() + (m_inflight.size() - m_warmInflight) >= m_capacity) {
        // a warm create never takes a slot the entries and the draws in flight already fill (#3)
        ++m_counters.warmDropped;
      } else if (!insertEntry(key, allocation)) {
        // step 6 (2g P2, Fable ruling 14): created but not stored - a create failure for this caller and every
        // waiter (flight->hr, still under the lock they read it under), so the draw is skipped and never uses a
        // pipeline nothing retains; this copy is released below, outside the lock, and the next asker creates again
        hr = E_OUTOFMEMORY;
        flight->hr = hr;
      }
      if (!winner && SUCCEEDED(hr)) flight->result = allocation;
    } else {
      ++m_counters.failures;
    }
    lock.unlock();
    m_flightDone.notify_all();
    if (observer) observer({SUCCEEDED(hr) ? &allocation->desc : &desc, hr, ms, warm,
                            SUCCEEDED(hr) ? allocation->sequence : 0});
    if (FAILED(hr)) return hr;
    if (winner) {
      output.m_pipeline = std::move(winner);
      return S_FALSE;
    }
    output.m_pipeline = std::move(allocation);
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

std::vector<NativePipelineCache::Snapshot> NativePipelineCache::snapshot() const {
  std::vector<Snapshot> out;
  {
    std::lock_guard lock(m_mutex);
    out.reserve(m_entries.size());
    for (const auto& [key, allocation] : m_entries)
      out.push_back({allocation->desc, allocation->sequence, allocation->warm,
                     allocation->drawHit.load(std::memory_order_relaxed)});
  }
  std::sort(out.begin(), out.end(), [](const Snapshot& a, const Snapshot& b) { return a.sequence < b.sequence; });
  return out;
}

NativePipelineCache::Counters NativePipelineCache::counters() const {
  std::lock_guard lock(m_mutex);
  return m_counters;
}

size_t NativePipelineCache::size() const {
  std::lock_guard lock(m_mutex);
  return m_entries.size();
}
void NativePipelineCache::clear() {
  std::lock_guard lock(m_mutex);
  m_entries.clear();
  m_memoRoot.Reset();   // I1: the memo's pin goes with the entries' (the next acquire validates its root again)
  m_memoIdentity.Reset();
}

}  // namespace sr::gfx
