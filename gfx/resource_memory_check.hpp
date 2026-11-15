#pragma once
#include "../kernel/sr_oracle.hpp"
#include <cstdint>
#if SR_ORACLE
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#endif

namespace sr::gfx::memres {
#if SR_ORACLE
struct FetchAnalysis;
struct ConsumerWitness {
  uint32_t page = UINT32_MAX, cpu = 0, window = 0; // page index, not byte address
  uint64_t ownerSum = 0, grantGeneration = 0, grantDigest = 0;
  uint32_t ownerSeq = 0, grantCount = 0, grantSerial[2] = {};
  bool known = false;
};
struct ConsumerBaseline {
  ConsumerWitness witness;
  std::array<uint8_t, 4096> bytes{};
  uint64_t id = 0;
};
struct ConsumerState;
struct ConsumerCheck {
  ~ConsumerCheck();
  std::shared_ptr<const ConsumerBaseline> baseline;
  ConsumerWitness observed;
  std::array<uint64_t, 16> changed{};
  uint32_t changedWords = 0;
  uint64_t id = 0;
 private:
  friend class ConsumerStore;
  std::shared_ptr<ConsumerState> state;
  // 0 admitted, 1/2 validated Same/Disjoint, 3 finalized, 4 validating.
  mutable std::atomic<uint8_t> phase{0};
  mutable uint64_t drawId = 0, vsHash = 0; // written once by bind, protected by state mutex
};
enum class ConsumerStatus { Same, Disjoint, Stale, Unknown };
struct ConsumerResult {
  ConsumerStatus status = ConsumerStatus::Unknown;
  const char* reason = "unvalidated";
  uint32_t changedWords = 0, matchedWords = 0;
  bool passed() const noexcept { return status == ConsumerStatus::Same || status == ConsumerStatus::Disjoint; }
};
struct ConsumerStats {
  uint64_t budget = 0, reserved = 0, capacity = 0, live = 0, peak = 0;
  uint64_t captured = 0, admitted = 0, finalized = 0, same = 0, disjoint = 0;
  uint64_t stale = 0, unknown = 0, abandoned = 0, capped = 0, pending = 0;
  uint64_t fetchBudget = 0, fetchAcquired = 0, fetchLive = 0, fetchPeak = 0, fetchCapped = 0;
};
struct ConsumerFetchLease;
// Immutable baselines and tickets use a fixed bounded allocation pool. Pages
// are never evicted to admit another page. Replaced versions stay alive while
// any queued copy/check retains them. Destruction of an unfinalized check fails.
class ConsumerStore {
 public:
  explicit ConsumerStore(uint64_t budgetBytes);
  ~ConsumerStore();
  ConsumerStore(const ConsumerStore&) = delete;
  ConsumerStore& operator=(const ConsumerStore&) = delete;
  std::shared_ptr<const ConsumerBaseline> capture(const uint8_t* frozen, const ConsumerWitness& witness);
  std::shared_ptr<const ConsumerCheck> keep(const uint8_t* frozen, const ConsumerWitness& before,
                                          const ConsumerWitness& after, bool stable);
  bool bind(const std::shared_ptr<const ConsumerCheck>& check, uint64_t drawId, uint64_t vsHash);
  ConsumerResult validate(const std::shared_ptr<const ConsumerCheck>& check, const FetchAnalysis& analysis,
                          const std::shared_ptr<const ConsumerBaseline>& recorded,
                          const ConsumerWitness& current);
  bool recorded(const std::shared_ptr<const ConsumerCheck>& check, uint64_t drawId);
  std::shared_ptr<const ConsumerFetchLease> acquireFetch();
  ConsumerStats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
bool resourceConsumerEnabled() noexcept;
bool resourceConsumerScopeActive() noexcept;
class ConsumerScope {
 public:
  explicit ConsumerScope(bool enabled = true) noexcept;
  ~ConsumerScope();
  ConsumerScope(const ConsumerScope&) = delete;
  ConsumerScope& operator=(const ConsumerScope&) = delete;
  std::vector<std::shared_ptr<const ConsumerCheck>> take() noexcept;
 private:
  friend bool resourceConsumerKept(const uint8_t*, const ConsumerWitness&, const ConsumerWitness&, bool);
  ConsumerScope* previous_ = nullptr;
  bool active_ = false;
  std::vector<std::shared_ptr<const ConsumerCheck>> checks_;
};
// Caller freezes all 4096 bytes between bracketed identity observations and
// confirms the source bytes did not change. This is observational, not atomic.
bool resourceConsumerKept(const uint8_t* frozen, const ConsumerWitness& before,
                          const ConsumerWitness& after, bool stable);
bool resourceConsumerBind(const std::shared_ptr<const ConsumerCheck>& check, uint64_t drawId, uint64_t vsHash);
ConsumerResult resourceConsumerValidate(const std::shared_ptr<const ConsumerCheck>& check,
    const FetchAnalysis& analysis, const std::shared_ptr<const ConsumerBaseline>& recorded,
    const ConsumerWitness& current);
// A successful analysis is not consumption. Call only after the actual Draw*
// records successfully. Dropping a validated ticket beforehand is UNKNOWN.
bool resourceConsumerRecorded(const std::shared_ptr<const ConsumerCheck>& check, uint64_t drawId);
// Separate bounded draw-input store: at most256 concurrent captures, each with
// a proven64KiB upper bound including lease/control block and vector capacity.
// Acquire before capture allocation; move one lease with each DrawBatch.
std::shared_ptr<const ConsumerFetchLease> resourceConsumerAcquireFetch();
// Independent observer. It never supplies a native ownership/upload decision.
// Captures establish the last known CPU bytes; a kept page with the same CPU
// generation must still contain those bytes. A noted write may rebase them.
bool resourceCheckEnabled() noexcept;
std::shared_ptr<const ConsumerBaseline> resourceCheckCaptured(uint32_t page, uint32_t cpu,
    const uint8_t* bytes, const ConsumerWitness* witness = nullptr);
bool resourceCheckKept(uint32_t page, uint32_t cpu, const uint8_t* bytes);
// mem-native: a kept page whose exact CPU-write patch (ResourceMemory::notePatch) is being copied. A
// whole-page fingerprint cannot tell the announced dwords from an unannounced write elsewhere on the page,
// so this observation is NOT a check: the fingerprint is re-taken and the page counted `patched`, an
// unchecked-coverage gap the strict grade reports (tools/perf/mem_resources_check.py). The census
// (SR_MEM_RESOURCES=census) is the per-dword oracle of patched pages.
void resourceCheckPatched(uint32_t page, uint32_t cpu, const uint8_t* bytes);
// Called before the synchronous replay oracle writes magenta into guest memory.
// Verify the existing baseline first, then predict only that explicit write.
// Unknown bytes outside a partial-page write cannot establish a baseline.
bool resourceCheckReplayPoison(uint32_t phys, uint32_t len, const uint8_t* guestPhysBase);
void resourceCheckFrame();
void resourceCheckTerminal();
#else
inline bool resourceCheckEnabled() noexcept { return false; }
inline void resourceCheckCaptured(uint32_t, uint32_t, const uint8_t*) {}
inline bool resourceCheckKept(uint32_t, uint32_t, const uint8_t*) { return true; }
inline void resourceCheckPatched(uint32_t, uint32_t, const uint8_t*) {}
inline void resourceCheckFrame() {}
inline void resourceCheckTerminal() {}
#endif
}  // namespace sr::gfx::memres
