#pragma once

#include "../kernel/sr_oracle.hpp"   // SR_ORACLE: the test seams below (0 in the release configuration)
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sr::gfx {

struct NativeShaderData;

// Immutable host shader bytecode. The source buffer may disappear after copy.
// I1 (step 6, Fable ruling 21): copies are INTERNED - a copy of bytes equal to a live code's shares that code's data,
// so equal shaders compare by pointer (the pipeline cache's hit path never compares bytes; it used to on every hit of
// a warm-up-made pipeline, whose manifest copy and the shader cache's copy of the same bytes were distinct owners).
class NativeShaderCode {
 public:
  static HRESULT copy(std::span<const uint8_t> bytes, NativeShaderCode& output);
  std::span<const uint8_t> bytes() const;
  uint64_t hash() const;
  bool operator==(const NativeShaderCode& other) const;
  explicit operator bool() const { return bool(m_data); }
  // I1: copies that found a live equal code and shared its data (the interning's hit counter, process-wide)
  static uint64_t internedCopies();
  const void* identity() const { return m_data.get(); }   // equal identities => equal bytes (interning: and back)
 private:
  std::shared_ptr<const NativeShaderData> m_data;
};

struct NativeInputElement {
  std::string semantic;
  uint32_t semanticIndex = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t inputSlot = 0, byteOffset = 0;
  D3D12_INPUT_CLASSIFICATION classification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  uint32_t instanceStepRate = 0;
};

// Native pipeline properties only. Register decoding and shader translation
// belong to callers; this service never discovers attachments or guest state.
struct NativePipelineDesc {
  Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
  NativeShaderCode vertex, pixel, geometry, hull, domain;
  std::vector<NativeInputElement> inputLayout;
  D3D12_RASTERIZER_DESC rasterizer{};
  D3D12_BLEND_DESC blend{};
  D3D12_DEPTH_STENCIL_DESC depthStencil{};
  std::array<DXGI_FORMAT, 8> colorFormats{};
  uint32_t colorCount = 0;
  DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
  DXGI_SAMPLE_DESC samples{1, 0};
  uint32_t sampleMask = UINT32_MAX, nodeMask = 0;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
};

struct NativePipelineAllocation;

// Retain while recording and through submission completion. Clearing the cache
// does not invalidate a lease, which owns the PSO, root signature and description.
class NativePipelineLease {
 public:
  ID3D12PipelineState* pipeline() const;
  const NativePipelineDesc* description() const;
  std::weak_ptr<const void> ownership() const;
  explicit operator bool() const { return pipeline() != nullptr; }
 private:
  std::shared_ptr<const NativePipelineAllocation> m_pipeline;
  friend class NativePipelineCache;
};

// Step 4 item 2k (lane D, 2026-10-20; every gate default OFF): the first-run pipeline-compile stall. A Warm
// acquire is the warm-up thread's (gfx/pso_warm.cpp), not a draw's.
enum class NativePipelineAcquire : uint8_t { Draw = 0, Warm = 1 };
// One CreateGraphicsPipelineState call the cache made (a miss), for the observer.
struct NativePipelineCreated {
  const NativePipelineDesc* desc = nullptr;   // valid during the call only
  HRESULT hr = S_OK;
  double ms = 0;                              // the create call's own wall time
  bool warm = false;                          // a Warm acquire's
  uint64_t sequence = 0;                      // creation order in this cache (1, 2, ...)
};
using NativePipelineObserver = void (*)(const NativePipelineCreated&);

class NativePipelineCache {
 public:
  HRESULT initialize(ID3D12Device* device, uint32_t capacity = 65536);
  // S_FALSE is a successful cache hit. Any failure preserves output and leaves
  // no cache entry, so later acquisition can retry. There is no fallback.
  // A draw's success always hands out a pipeline the cache retains (callers keep only the raw pointer): a pipeline
  // created but not stored (the insert's bad_alloc) is E_OUTOFMEMORY for its creator and every waiter.
  HRESULT acquire(const NativePipelineDesc& desc, NativePipelineLease& output,
                  NativePipelineAcquire mode = NativePipelineAcquire::Draw);
  size_t size() const;
  // Callers must retain leases for all recorded/submitted uses before clearing.
  void clear();

  // 2k(3a) SR_PSO_SINGLE_FLIGHT=1: the create runs OUTSIDE the cache lock - a compile on one thread no longer
  // stalls every other thread's lookups (hits included); a second asker of the same key waits for that one
  // compile only. Off (default): the create runs under the lock, as before. setSingleFlight: tests / the warm-up
  // (-1 = back to the environment).
  static bool singleFlight();
  static void setSingleFlight(int on);
  // 2k(1): called after every create (draw or warm), outside the cache lock in single-flight mode. nullptr = none.
  static void setObserver(NativePipelineObserver observer);
#if SR_ORACLE   // step 6 (Astra on 6af7487e): the test seams exist in dev / difftest builds only, never the release exe
  // Tests only: called inside every create, before the device call (a test blocks one key's create on a latch).
  using CreateGateForTest = HRESULT (*)(const NativePipelineDesc&);   // S_OK: create; a failure: return it uncreated
  static void setCreateGateForTest(CreateGateForTest gate);
  // Tests only (step 6, 2g P2): the next `count` cache inserts throw std::bad_alloc, as an out-of-memory insert would.
  static void failInsertsForTest(uint32_t count);
#endif
  // Step 4 2k (fix12): the longest a draw waits for the warm-up's compile of the key it needs, before it compiles its
  // own copy on the draw path (the first finisher fills the cache; the other create's copy is released).
  static constexpr uint32_t kWarmJoinWaitMs = 2000;
  struct Snapshot {
    NativePipelineDesc desc;
    uint64_t sequence = 0;
    bool warm = false;      // created by a Warm acquire
    bool drawHit = false;   // ... and later asked for by a draw (a warm-up that paid off)
  };
  // The entries now (descriptions copied), in creation order.
  std::vector<Snapshot> snapshot() const;
  struct Counters {
    uint64_t hits = 0, creates = 0, warmCreates = 0, warmUsed = 0, flightWaits = 0, failures = 0, warmEvicted = 0;
    uint64_t warmDropped = 0;   // a warm create that finished with the cache full of draws' entries: not cached
    uint64_t warmFlightRetries = 0;   // draws that joined a warm flight which failed, then created their own
    uint64_t warmFlightTimeouts = 0;  // draws that waited kWarmJoinWaitMs on a warm flight, then created their own
    uint64_t lostRaces = 0;   // creates whose key another create filled meanwhile: released for the cached one
    // step 6 (2g P2, Fable ruling 14): created, but the cache could not store it (bad_alloc) - released, and a
    // failure (also in `failures`) for its creator and every waiter, never an uncached pipeline
    uint64_t insertFailures = 0;
    // I1 (step 6, Fable ruling 21 (1)): the lookup fast path's own counters
    uint64_t borrowedKeyLookups = 0;   // lookups keyed by the borrowed view (no key built): every valid acquire
    uint64_t rootMemoHits = 0;         // lookups whose root signature was the validated one (no COM round trips)
  };
  Counters counters() const;
#if SR_ORACLE   // I1 (Fable ruling 21 (2)): the key's components and the mutant seam - dev / difftest builds only
  // One key component: its name, its index within that name (a render target, a face, an input element, a byte) and
  // its value. Components "shader" 0..4 and "root" 0 are the identities, the rest the state words, in key order.
  struct KeyComponentForTest {
    const char* name;
    uint32_t index;
    uint64_t value;
  };
  static std::vector<KeyComponentForTest> keyComponentsForTest(const NativePipelineDesc& desc);
  // A MUTANT key: the component (name, index) is left out of the key's hash and equality (nullptr = none; index
  // UINT32_MAX = every index of that name). The mutant test flips each component and requires a miss; with that
  // component's mutant active the flip must hit. Set it only while no cache holds entries (it changes the hash).
  static void dropKeyComponentForTest(const char* name, uint32_t index);
#endif
 private:
  struct Key {
    Microsoft::WRL::ComPtr<IUnknown> rootIdentity;
    std::array<NativeShaderCode, 5> shaders;
    std::vector<uint64_t> state;
    bool operator==(const Key& other) const;
  };
  // I1: a lookup's key, BORROWED - the root identity, the descriptor's five shader codes, and the state words in the
  // calling thread's reused buffer - so a hit builds and frees nothing. Hash and equality treat it exactly as the Key
  // it would make (the transparent hash / equality below).
  struct KeyView {
    IUnknown* rootIdentity = nullptr;
    std::array<const NativeShaderCode*, 5> shaders{};
    std::span<const uint64_t> state;
  };
  struct Hash {
    using is_transparent = void;
    size_t operator()(const Key& key) const;
    size_t operator()(const KeyView& key) const;
  };
  struct Equal {
    using is_transparent = void;
    bool operator()(const Key& a, const Key& b) const { return a == b; }
    bool operator()(const Key& a, const KeyView& b) const;
    bool operator()(const KeyView& a, const Key& b) const { return (*this)(b, a); }
  };
  static Key makeKey(const NativePipelineDesc& desc, Microsoft::WRL::ComPtr<IUnknown> identity);
  static KeyView viewKey(const NativePipelineDesc& desc, IUnknown* identity);
  struct InFlight {
    bool done = false;
    bool warm = false;   // a Warm acquire's create: never counts against a draw's admission (Astra 10:2x #3)
    uint32_t workerTid = 0;   // the creating thread (a draw joining a warm flight lends it its priority)
    bool listed = true;       // still the key's m_inflight entry (a draw that stopped waiting unlists a warm flight)
    HRESULT hr = E_PENDING;
    std::shared_ptr<const NativePipelineAllocation> result;
  };
  HRESULT create(const NativePipelineDesc& desc, std::shared_ptr<NativePipelineAllocation>& allocation,
                 double& ms) const;
  void noteHit(const NativePipelineAllocation& allocation, NativePipelineAcquire mode);
  bool evictUnusedWarm();   // under the lock: drops one warm-made entry no draw asked for
  // under the lock: stores the entry; false (counted) when the insert throws bad_alloc
  bool insertEntry(Key& key, const std::shared_ptr<NativePipelineAllocation>& allocation);
  mutable std::mutex m_mutex;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  uint32_t m_capacity = 0;
  std::unordered_map<Key, std::shared_ptr<const NativePipelineAllocation>, Hash, Equal> m_entries;
  std::unordered_map<Key, std::shared_ptr<InFlight>, Hash, Equal> m_inflight;   // single-flight creates in progress
  // I1: the last root signature that passed the device check, PINNED (its address cannot be reused while held), and
  // its identity: a lookup with that root skips the four COM round trips (a root signature's device never changes)
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_memoRoot;
  Microsoft::WRL::ComPtr<IUnknown> m_memoIdentity;
  size_t m_warmInflight = 0;   // of m_inflight, the Warm acquires' (a draw's admission ignores them)
  std::condition_variable m_flightDone;
  uint64_t m_sequence = 0;
  Counters m_counters;
};

}  // namespace sr::gfx
