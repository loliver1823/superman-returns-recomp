#include "resource_memory_check.hpp"
#include "resource_memory.hpp"
#include "resource_census.hpp"
#include "../app/log.hpp"
#include <cstdio>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>

namespace sr::gfx::memres {
namespace {
constexpr uint64_t kConsumerMaxBytes = 256ull << 20;
constexpr uint64_t kConsumerControlBytes = 64ull << 10;
// Reserve both the core latest-page table and the backend recorded-page table.
// Each occupied block additionally budgets 64 bytes of moved scope/copy vector
// handles. Copies of unbounded reference vectors are not part of this API.
constexpr uint64_t kConsumerIndexBytes = 2ull * kPages * sizeof(std::shared_ptr<const ConsumerBaseline>);
struct alignas(std::max_align_t) ConsumerBlock {
  std::byte data[4608];
  uint32_t next = UINT32_MAX;
};
constexpr uint64_t kConsumerBlockCharge = sizeof(ConsumerBlock) + 64u;
constexpr uint64_t kConsumerFetchCount = 256;
constexpr uint64_t kConsumerFetchCharge = 64ull << 10;
bool validWitness(const ConsumerWitness& w) {
  if (!w.known || w.page >= kPages || (w.window != 8 && w.window != 10) ||
      !w.grantCount || w.grantCount > 2) return false;
  for (uint32_t i = 0; i < w.grantCount; ++i) if (!w.grantSerial[i]) return false;
  return true;
}
bool sameLocalOwner(const ConsumerWitness& a, const ConsumerWitness& b) {
  return a.page == b.page && a.ownerSum == b.ownerSum && a.grantDigest == b.grantDigest &&
         a.grantCount == b.grantCount && a.grantSerial[0] == b.grantSerial[0] &&
         a.grantSerial[1] == b.grantSerial[1];
}
bool sameAdmission(const ConsumerWitness& a, const ConsumerWitness& b) {
  return validWitness(a) && validWitness(b) && sameLocalOwner(a, b) &&
         a.cpu == b.cpu && a.window == b.window && a.ownerSeq == b.ownerSeq &&
         a.grantGeneration == b.grantGeneration;
}
}

struct ConsumerState {
  std::mutex mutex;
  ConsumerStats counts;
  std::unique_ptr<ConsumerBlock[]> blocks;
  uint32_t free = UINT32_MAX;
  uint64_t nextId = 0;
  explicit ConsumerState(uint64_t budget) {
    if (budget > kConsumerMaxBytes || budget <= kConsumerIndexBytes + kConsumerControlBytes)
      throw std::invalid_argument("consumer budget cannot hold fixed index");
    counts.budget = budget;
    counts.fetchBudget = kConsumerFetchCount * kConsumerFetchCharge;
    counts.capacity = (budget - kConsumerIndexBytes - kConsumerControlBytes) / kConsumerBlockCharge;
    if (!counts.capacity) throw std::invalid_argument("consumer budget cannot hold a block");
    counts.reserved = kConsumerIndexBytes + kConsumerControlBytes + counts.capacity * kConsumerBlockCharge;
    blocks.reset(new ConsumerBlock[size_t(counts.capacity)]);
    for (uint32_t i = 0; i < counts.capacity; ++i)
      blocks[i].next = i + 1u < counts.capacity ? i + 1u : UINT32_MAX;
    free = 0;
  }
  void* allocate(size_t bytes, size_t alignment) {
    std::lock_guard<std::mutex> lock(mutex);
    if (bytes > sizeof(ConsumerBlock::data) || alignment > alignof(ConsumerBlock) || free == UINT32_MAX) {
      ++counts.capped;
      throw std::bad_alloc();
    }
    const uint32_t index = free;
    free = blocks[index].next;
    ++counts.live;
    counts.peak = std::max(counts.peak, counts.live);
    return blocks[index].data;
  }
  void release(void* pointer) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    const auto delta = static_cast<std::byte*>(pointer) - reinterpret_cast<std::byte*>(blocks.get());
    const uint32_t index = uint32_t(size_t(delta) / sizeof(ConsumerBlock));
    blocks[index].next = free;
    free = index;
    --counts.live;
  }
  void unknown() { std::lock_guard<std::mutex> lock(mutex); ++counts.unknown; }
};
namespace {
template<class T> struct ConsumerAllocator {
  using value_type = T;
  std::shared_ptr<ConsumerState> state;
  explicit ConsumerAllocator(std::shared_ptr<ConsumerState> s) : state(std::move(s)) {}
  template<class U> ConsumerAllocator(const ConsumerAllocator<U>& a) : state(a.state) {}
  T* allocate(size_t n) {
    if (n > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
    return static_cast<T*>(state->allocate(n * sizeof(T), alignof(T)));
  }
  void deallocate(T* p, size_t) noexcept { state->release(p); }
  template<class U> bool operator==(const ConsumerAllocator<U>& a) const noexcept { return state == a.state; }
  template<class U> bool operator!=(const ConsumerAllocator<U>& a) const noexcept { return !(*this == a); }
};
}
struct ConsumerStore::Impl {
  std::shared_ptr<ConsumerState> state;
  std::mutex mutex;
  std::vector<std::shared_ptr<const ConsumerBaseline>> latest;
  explicit Impl(uint64_t budget) : state(std::make_shared<ConsumerState>(budget)), latest(kPages) {}
};
ConsumerStore::ConsumerStore(uint64_t budget) : impl_(new Impl(budget)) {}
ConsumerStore::~ConsumerStore() = default;
ConsumerCheck::~ConsumerCheck() {
  if (state && phase.exchange(3) != 3) {
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->counts.abandoned;
    ++state->counts.unknown;
    ++state->counts.finalized;
    --state->counts.pending;
  }
}
struct ConsumerFetchLease {
  std::shared_ptr<ConsumerState> state;
  ~ConsumerFetchLease() {
    if (state) { std::lock_guard<std::mutex> lock(state->mutex); --state->counts.fetchLive; }
  }
};
std::shared_ptr<const ConsumerFetchLease> ConsumerStore::acquireFetch() {
  auto state = impl_->state;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->counts.fetchLive >= kConsumerFetchCount) {
      ++state->counts.fetchCapped; ++state->counts.unknown; return {};
    }
    ++state->counts.fetchLive;
    state->counts.fetchPeak = std::max(state->counts.fetchPeak, state->counts.fetchLive);
  }
  try {
    auto result = std::allocate_shared<ConsumerFetchLease>(ConsumerAllocator<ConsumerFetchLease>(state));
    result->state = state;
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->counts.fetchAcquired;
    return result;
  } catch (const std::bad_alloc&) {
    std::lock_guard<std::mutex> lock(state->mutex);
    --state->counts.fetchLive; ++state->counts.fetchCapped; ++state->counts.unknown;
    return {};
  }
}
std::shared_ptr<const ConsumerBaseline> ConsumerStore::capture(const uint8_t* frozen, const ConsumerWitness& witness) {
  auto state = impl_->state;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  // A failed replacement cannot leave the old latest entry usable by a new
  // Keep. Already queued tickets retain their own immutable old version.
  if (witness.page < kPages) impl_->latest[witness.page].reset();
  if (!frozen || !validWitness(witness)) { state->unknown(); return {}; }
  try {
    auto result = std::allocate_shared<ConsumerBaseline>(ConsumerAllocator<ConsumerBaseline>(state));
    result->witness = witness;
    std::memcpy(result->bytes.data(), frozen, result->bytes.size());
    {
      std::lock_guard<std::mutex> countLock(state->mutex);
      result->id = ++state->nextId;
      ++state->counts.captured;
    }
    impl_->latest[witness.page] = result;
    return result;
  } catch (const std::bad_alloc&) { state->unknown(); return {}; }
}
std::shared_ptr<const ConsumerCheck> ConsumerStore::keep(const uint8_t* frozen, const ConsumerWitness& before,
                                                        const ConsumerWitness& after, bool stable) {
  auto state = impl_->state;
  if (!frozen || !stable || !sameAdmission(before, after)) { state->unknown(); return {}; }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto baseline = impl_->latest[before.page];
  if (!baseline || !sameLocalOwner(baseline->witness, before) ||
      baseline->witness.cpu != before.cpu || baseline->witness.window != before.window) {
    state->unknown(); return {};
  }
  try {
    auto result = std::allocate_shared<ConsumerCheck>(ConsumerAllocator<ConsumerCheck>(state));
    result->baseline = std::move(baseline);
    result->observed = before;
    for (uint32_t word = 0; word < 1024; ++word) {
      if (std::memcmp(result->baseline->bytes.data() + word * 4u, frozen + word * 4u, 4u) != 0) {
        result->changed[word / 64u] |= uint64_t(1) << (word % 64u);
        ++result->changedWords;
      }
    }
    {
      std::lock_guard<std::mutex> countLock(state->mutex);
      result->id = ++state->nextId;
      ++state->counts.admitted;
      ++state->counts.pending;
      result->state = state;
    }
    return result;
  } catch (const std::bad_alloc&) { state->unknown(); return {}; }
}
bool ConsumerStore::bind(const std::shared_ptr<const ConsumerCheck>& check, uint64_t drawId, uint64_t vsHash) {
  auto state = impl_->state;
  if (!check || check->state != state) { state->unknown(); return false; }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!drawId || !vsHash || check->phase.load() != 0 || check->drawId || check->vsHash) {
    ++state->counts.unknown; return false;
  }
  check->drawId = drawId;
  check->vsHash = vsHash;
  return true;
}
ConsumerResult ConsumerStore::validate(const std::shared_ptr<const ConsumerCheck>& check, const FetchAnalysis& analysis,
                                      const std::shared_ptr<const ConsumerBaseline>& recorded,
                                      const ConsumerWitness& current) {
  auto state = impl_->state;
  ConsumerResult result;
  if (!check || check->state != state) { state->unknown(); result.reason = "missing_or_foreign_ticket"; return result; }
  uint8_t expected = 0;
  if (!check->phase.compare_exchange_strong(expected, 4)) {
    state->unknown(); result.reason = "duplicate_validation"; return result;
  }
  result.changedWords = check->changedWords;
  auto finish = [&]() {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (result.passed()) {
      check->phase.store(result.status == ConsumerStatus::Same ? 1 : 2);
    } else {
      check->phase.store(3);
      ++state->counts.finalized;
      --state->counts.pending;
      if (result.status == ConsumerStatus::Stale) ++state->counts.stale;
      else ++state->counts.unknown;
    }
    return result;
  };
  uint64_t expectedDraw = 0, expectedShader = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    expectedDraw = check->drawId;
    expectedShader = check->vsHash;
  }
  if (!expectedDraw || !expectedShader) { result.reason = "draw_not_bound"; return finish(); }
  if (!recorded || recorded != check->baseline) { result.reason = "recorded_copy_mismatch"; return finish(); }
  // The current witness is internally bracketed by the backend. Later CPU
  // writes/alias selection and global registry counters cannot invalidate an
  // older draw whose immutable capture is exactly the copy recorded at cursor.
  if (!validWitness(current) || !sameLocalOwner(current, check->observed)) {
    result.reason = "current_owner_mismatch"; return finish();
  }
  if (!check->changedWords) {
    result.status = ConsumerStatus::Same; result.reason = "whole_page_equal"; return finish();
  }
  if (!analysis.complete || !analysis.drawId || !analysis.vsHash || analysis.uses.empty() || analysis.uses.size() > 65536u) {
    result.reason = "incomplete_consumer"; return finish();
  }
  if (analysis.drawId != expectedDraw || analysis.vsHash != expectedShader) {
    result.reason = "consumer_draw_mismatch"; return finish();
  }
  // Validate the entire analysis before granting any exclusion. Rounded DWORD
  // overlap is conservative for sub-DWORD vfetch intervals.
  for (const auto& use : analysis.uses) {
    if (!use.len || uint64_t(use.phys) + use.len > kPhysBytes) {
      result.reason = "consumer_range_invalid"; return finish();
    }
  }
  std::array<uint64_t, 16> fetched{};
  const uint64_t pageStart = uint64_t(check->observed.page) << kPageLog2;
  for (const auto& use : analysis.uses) {
    const uint64_t begin = std::max(pageStart, uint64_t(use.phys));
    const uint64_t end = std::min(pageStart + 4096u, uint64_t(use.phys) + use.len);
    if (begin >= end) continue;
    const uint32_t first = uint32_t((begin - pageStart) / 4u);
    const uint32_t last = uint32_t((end - 1u - pageStart) / 4u);
    for (uint32_t group = first / 64u; group <= last / 64u; ++group) {
      const uint32_t low = group == first / 64u ? first % 64u : 0u;
      const uint32_t high = group == last / 64u ? last % 64u : 63u;
      const uint64_t lowMask = ~uint64_t(0) << low;
      const uint64_t highMask = ~uint64_t(0) >> (63u - high);
      fetched[group] |= lowMask & highMask;
    }
  }
  for (uint32_t i = 0; i < 16; ++i) {
    uint64_t overlap = fetched[i] & check->changed[i];
    while (overlap) { ++result.matchedWords; overlap &= overlap - 1u; }
  }
  result.status = result.matchedWords ? ConsumerStatus::Stale : ConsumerStatus::Disjoint;
  result.reason = result.matchedWords ? "changed_bytes_consumed" : "changed_bytes_disjoint";
  return finish();
}
bool ConsumerStore::recorded(const std::shared_ptr<const ConsumerCheck>& check, uint64_t drawId) {
  auto state = impl_->state;
  if (!check || check->state != state) { state->unknown(); return false; }
  std::lock_guard<std::mutex> lock(state->mutex);
  uint8_t phase = check->phase.load();
  if (!drawId || drawId != check->drawId || (phase != 1 && phase != 2) ||
      !check->phase.compare_exchange_strong(phase, 3)) {
    ++state->counts.unknown; return false;
  }
  ++state->counts.finalized;
  --state->counts.pending;
  if (phase == 1) ++state->counts.same; else ++state->counts.disjoint;
  return true;
}
ConsumerStats ConsumerStore::stats() const {
  std::lock_guard<std::mutex> lock(impl_->state->mutex);
  return impl_->state->counts;
}
namespace {
thread_local ConsumerScope* consumerScope = nullptr;
std::atomic<uint64_t> consumerUnavailable{0};
ConsumerStore* consumerStore() noexcept {
  static const std::unique_ptr<ConsumerStore> value = []() -> std::unique_ptr<ConsumerStore> {
    try {
      uint64_t megabytes = 128;
      if (const char* text = std::getenv("SR_MEM_RESOURCES_CONSUMER_CHECK_MB")) {
        megabytes = 0;
        if (!*text) throw std::invalid_argument("empty consumer budget");
        for (; *text; ++text) {
          if (*text < '0' || *text > '9' || megabytes > 256) throw std::invalid_argument("invalid consumer budget");
          megabytes = megabytes * 10u + uint64_t(*text - '0');
        }
        if (!megabytes || megabytes > 256) throw std::invalid_argument("consumer budget outside 1..256 MiB");
      }
      return std::make_unique<ConsumerStore>(megabytes << 20);
    } catch (const std::exception& e) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[MEM_RES_CHECK] consumer_config=UNKNOWN reason=%s", e.what());
      ++consumerUnavailable;
      return {};
    }
  }();
  return value.get();
}
void reportConsumer(bool terminal) {
  if (!resourceConsumerEnabled()) return;
  const auto* store = consumerStore();
  const ConsumerStats s = store ? store->stats() : ConsumerStats{};
  const FetchShaderStats shaders = consumerFetchShaderStats();
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] consumer_terminal=%u budget=%llu reserved=%llu capacity=%llu live=%llu peak=%llu "
      "captured=%llu admitted=%llu finalized=%llu same=%llu disjoint=%llu STALE=%llu unknown=%llu "
      "abandoned=%llu capped=%llu pending=%llu unavailable=%llu "
      "fetch_budget=%llu fetch_acquired=%llu fetch_live=%llu fetch_peak=%llu fetch_capped=%llu "
      "shader_bytes=%llu shader_capacity=%llu shader_retained=%llu shader_capped=%llu "
      "transient_f1_bytes=%u transient_uses=%u",
      unsigned(terminal), s.budget, s.reserved, s.capacity, s.live, s.peak, s.captured, s.admitted,
      s.finalized, s.same, s.disjoint, s.stale, s.unknown, s.abandoned, s.capped, s.pending,
      consumerUnavailable.load(), s.fetchBudget, s.fetchAcquired, s.fetchLive, s.fetchPeak, s.fetchCapped,
      shaders.bytes, shaders.capacity, shaders.retained, shaders.capped, 1u << 20, 65536u);
}
}
bool resourceConsumerEnabled() noexcept {
  static const bool on = [] {
    const char* value = std::getenv("SR_MEM_RESOURCES_CONSUMER_CHECK");
    const bool enabled = resourceCheckEnabled() && value && std::strcmp(value, "1") == 0;
    if (enabled) SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES_CHECK] consumer_enabled=1 current_draw_only_no_rebase");
    return enabled;
  }();
  return on;
}
bool resourceConsumerScopeActive() noexcept { return consumerScope != nullptr; }
ConsumerScope::ConsumerScope(bool enabled) noexcept {
  if (enabled && resourceConsumerEnabled()) {
    previous_ = consumerScope; consumerScope = this; active_ = true;
  }
}
ConsumerScope::~ConsumerScope() { if (active_) consumerScope = previous_; }
std::vector<std::shared_ptr<const ConsumerCheck>> ConsumerScope::take() noexcept {
  if (active_) { consumerScope = previous_; active_ = false; }
  return std::move(checks_);
}
bool resourceConsumerKept(const uint8_t* frozen, const ConsumerWitness& before, const ConsumerWitness& after, bool stable) {
  auto* store = resourceConsumerEnabled() ? consumerStore() : nullptr;
  if (!consumerScope || !store) { ++consumerUnavailable; return false; }
  auto check = store->keep(frozen, before, after, stable);
  if (!check) return false;
  try { consumerScope->checks_.push_back(std::move(check)); return true; }
  catch (const std::bad_alloc&) { ++consumerUnavailable; return false; }
}
ConsumerResult resourceConsumerValidate(const std::shared_ptr<const ConsumerCheck>& check, const FetchAnalysis& analysis,
    const std::shared_ptr<const ConsumerBaseline>& recorded, const ConsumerWitness& current) {
  auto* store = resourceConsumerEnabled() ? consumerStore() : nullptr;
  if (store) return store->validate(check, analysis, recorded, current);
  ++consumerUnavailable;
  ConsumerResult result; result.reason = "consumer_store_unavailable"; return result;
}
bool resourceConsumerBind(const std::shared_ptr<const ConsumerCheck>& check, uint64_t drawId, uint64_t vsHash) {
  auto* store = resourceConsumerEnabled() ? consumerStore() : nullptr;
  if (store) return store->bind(check, drawId, vsHash);
  ++consumerUnavailable; return false;
}
bool resourceConsumerRecorded(const std::shared_ptr<const ConsumerCheck>& check, uint64_t drawId) {
  auto* store = resourceConsumerEnabled() ? consumerStore() : nullptr;
  if (store) return store->recorded(check, drawId);
  ++consumerUnavailable; return false;
}
std::shared_ptr<const ConsumerFetchLease> resourceConsumerAcquireFetch() {
  auto* store = resourceConsumerEnabled() ? consumerStore() : nullptr;
  if (store) return store->acquireFetch();
  ++consumerUnavailable; return {};
}
namespace {
struct Page { uint64_t hash = 0; uint32_t cpu = 0; bool seen = false; };
struct Checker {
  std::mutex mutex;
  std::vector<Page> pages = std::vector<Page>(kPages);
  uint64_t captured = 0, checked = 0, rebased = 0, raced = 0, stale = 0, frames = 0, oracle = 0, patched = 0;
  void report(bool terminal) const {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES_CHECK] terminal=%u frames=%llu captured=%llu checked=%llu "
                        "rebased=%llu raced=%llu STALE=%llu generation=%u oracle=%llu patched=%llu", unsigned(terminal),
                 static_cast<unsigned long long>(frames), static_cast<unsigned long long>(captured),
                 static_cast<unsigned long long>(checked), static_cast<unsigned long long>(rebased),
                 static_cast<unsigned long long>(raced), static_cast<unsigned long long>(stale),
                 resourceMemory().generation(), static_cast<unsigned long long>(oracle),
                 static_cast<unsigned long long>(patched));
  }
};
Checker& checker() { static Checker value; return value; }
uint64_t fingerprint(const uint8_t* bytes) {
  // Deliberately independent of the development model's ShaderCache hash.
  uint64_t value = 14695981039346656037ull;
  for (uint32_t i = 0; i < 4096u; ++i) value = (value ^ bytes[i]) * 1099511628211ull;
  return value;
}
}

bool resourceCheckEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("SR_MEM_RESOURCES_CHECK");
    const bool on = resourceMode() && value && std::strcmp(value, "1") == 0;
    if (on) SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES_CHECK] enabled=1 independent_kept_page_checker");
    return on;
  }();
  return enabled;
}

std::shared_ptr<const ConsumerBaseline> resourceCheckCaptured(uint32_t page, uint32_t cpu,
    const uint8_t* bytes, const ConsumerWitness* witness) {
  if (!resourceCheckEnabled()) return {};
  auto& state = checker();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.pages.at(page) = Page{fingerprint(bytes), cpu, true};
  ++state.captured;
  if (!resourceConsumerEnabled()) return {};
  auto* store = consumerStore();
  if (!store) { ++consumerUnavailable; return {}; }
  ConsumerWitness unknown;
  unknown.page = page;
  if (!witness || witness->page != page || witness->cpu != cpu) witness = &unknown;
  return store->capture(bytes, *witness);
}

bool resourceCheckKept(uint32_t page, uint32_t cpu, const uint8_t* bytes) {
  if (!resourceCheckEnabled()) return true;
  const uint64_t hash = fingerprint(bytes);
  auto& state = checker();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (resourceMemory().cpuGeneration(page) != cpu) {
    ++state.raced;  // A concurrent completed writer is not an unchanged-page observation.
    return true;
  }
  Page& old = state.pages.at(page);
  if (!old.seen || old.cpu != cpu) {
    old = Page{hash, cpu, true};
    ++state.rebased;
    return true;
  }
  ++state.checked;
  if (hash == old.hash) return true;
  ++state.stale;
  SR_LOG_BOOT(sr::LogSeverity::Error, "[MEM_RES_CHECK] STALE page=%08X cpu=%u captured=%016llX current=%016llX",
               page << kPageLog2, cpu, static_cast<unsigned long long>(old.hash),
               static_cast<unsigned long long>(hash));
  state.report(false);
  return false;
}

void resourceCheckPatched(uint32_t page, uint32_t cpu, const uint8_t* bytes) {
  if (!resourceCheckEnabled() || !bytes) return;
  auto& state = checker();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.pages.at(page) = Page{fingerprint(bytes), cpu, true};
  ++state.patched;
}

void resourceCheckFrame() {
  if (!resourceCheckEnabled()) return;
  auto& state = checker();
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.frames;
  if (state.frames <= 3u || state.frames % 120u == 0u) { state.report(false); reportConsumer(false); }
}

bool resourceCheckReplayPoison(uint32_t phys, uint32_t len, const uint8_t* guestPhysBase) {
  if (!resourceCheckEnabled()) return true;
  // The replay intervention also changes pending captured copies. Until that
  // path replaces every immutable copy handle, it cannot claim correspondence.
  if (resourceConsumerEnabled()) {
    ++consumerUnavailable;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[MEM_RES_CHECK] consumer=UNKNOWN reason=replay_poison_unbound");
    return false;
  }
  uint32_t first, last;
  if (!guestPhysBase || !Registry::pageRange(phys, len, first, last)) return false;
  auto& state = checker();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (uint32_t page = first; page <= last; ++page) {
    const uint32_t start = page << kPageLog2;
    const uint32_t begin = std::max(phys, start) - start;
    const uint32_t end = uint32_t(std::min(uint64_t(phys) + len, uint64_t(start) + 4096u) - start);
    const uint32_t cpu = resourceMemory().cpuGeneration(page);
    Page& old = state.pages[page];
    std::array<uint8_t, 4096> predicted;
    std::memcpy(predicted.data(), guestPhysBase + start, predicted.size());
    const uint64_t before = fingerprint(predicted.data());
    if (resourceMemory().cpuGeneration(page) != cpu) {
      ++state.raced;
      return false;
    }
    if (old.seen && old.cpu == cpu) {
      if (old.hash != before) {
        ++state.stale;
        SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[MEM_RES_CHECK] STALE before_oracle page=%08X cpu=%u captured=%016llX current=%016llX",
                    start, cpu, static_cast<unsigned long long>(old.hash), static_cast<unsigned long long>(before));
        return false;
      }
    } else if (begin != 0 || end != 4096u) {
      // A known write cannot explain unknown untouched bytes. Keep the strict
      // incomplete-coverage failure instead of silently adopting those bytes.
      ++state.rebased;
      return false;
    }
    for (uint32_t i = begin; i < end; ++i) predicted[i] = (i & 3u) == 1u ? 0 : 0xFF;
    old = Page{fingerprint(predicted.data()), cpu, true};
    ++state.oracle;
  }
  return true;
}

void resourceCheckTerminal() {
  if (!resourceCheckEnabled()) return;
  auto& state = checker();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.report(true);
  reportConsumer(true);
}
}  // namespace sr::gfx::memres
