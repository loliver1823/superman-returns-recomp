#pragma once
// step 4 G7 (lane D): the 2D texture cache's bound under SR_TEX_CACHE_EVICT (gfx/d3d12_backend.cpp texEvict*).
//
// Without it the cache's only bound was the 8192-slot pool: each reload adds keys, and a key leaves only when the
// pool is full (the LRU victim). This header is the policy, and nothing else, so tools/difftest/tex_evict_policy_test
// drives the same code on the CPU:
//  - select(): which entries leave at a sweep, (a) by age, (b) by a byte budget, least recently used RESOURCE first
//    (a shared resource's aliases leave together: its bytes are freed only with its last alias);
//  - exhaustVictim(): slot exhaustion's victim, never one bound in the current frame nor pinned by the draw being
//    assembled (Astra 5d0c2a63 P1); none = a transient exhaustion (the caller caches nothing and retries);
//  - SlotRetire: an evicted entry's slot and resource wait for the GPU (the frame fence of the frame that evicted it),
//    then the slot is reusable. The debug check (the backend passes the slot's live last-use stamp in oracle
//    builds) holds back any entry whose slot was bound after the fence it waits for.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <unordered_map>
#include <vector>

namespace sr::gfx::texevict {

// One 2D texture-cache entry as the policy sees it.
struct Candidate {
  uint64_t key = 0;
  uint64_t lastUse = 0;  // the frame fence of its last bind (0 = never bound since the gate stamped it)
  uint64_t bytes = 0;    // its resource's size (every alias of a shared resource carries the same value)
  uint64_t group = 0;    // the shared resource's identity (0 = the entry owns its resource alone)
  uint32_t slot = 0;     // its descriptor slot (exhaustVictim's pin check)
};

struct Selection {
  std::vector<uint64_t> aged, budget;  // victims by age, then by the byte budget
  uint64_t agedBytes = 0, budgetBytes = 0;  // what they free (a shared resource: only when all its aliases leave)
  bool capped = false;  // maxN stopped the selection: the caller sweeps again at the next frame
};

// The victims at frame fence `now`, at most maxN:
//  (a) every entry unused for at least `ageFrames` frames (0 = no age rule); a shared resource's bytes count as
//      freed only when every alias of it is aged;
//  (b) then, while the live bytes stay over `budget` (0 = no budget), whole resources least recently used first (a
//      shared one is as recent as its newest remaining alias), down to 9/10 of the budget, never a resource bound
//      within the last `keepFrames` frames.
inline Selection select(const std::vector<Candidate>& all, uint64_t now, uint64_t ageFrames, uint64_t keepFrames,
                        uint64_t live, uint64_t budget, size_t maxN = SIZE_MAX) {
  Selection s;
  auto age = [now](uint64_t lastUse) { return now > lastUse ? now - lastUse : uint64_t(0); };
  struct Group {
    uint32_t aliases = 0, aged = 0;
    uint64_t bytes = 0;
  };
  std::unordered_map<uint64_t, Group> groups;
  std::vector<char> aged(all.size(), 0);
  for (size_t i = 0; i < all.size(); ++i) {
    const Candidate& e = all[i];
    // the P1 rule, not a tunable: an entry bound in the current frame (age 0) is never a victim, whatever the knobs
    const bool a = ageFrames && age(e.lastUse) >= std::max<uint64_t>(ageFrames, 1);
    if (a && s.aged.size() >= maxN) s.capped = true;
    aged[i] = a && !s.capped;
    if (aged[i]) s.aged.push_back(e.key);
    if (e.group) {
      Group& g = groups[e.group];
      g.aliases++;
      g.aged += aged[i];
      g.bytes = e.bytes;
    } else if (aged[i]) {
      s.agedBytes += e.bytes;
    }
  }
  for (const auto& [id, g] : groups)
    if (g.aged == g.aliases) s.agedBytes += g.bytes;
  uint64_t left = live > s.agedBytes ? live - s.agedBytes : 0u;
  if (!budget || left <= budget || s.capped) return s;
  // (b) one unit per resource still held: an own entry, or a shared resource's remaining aliases
  struct Unit {
    uint64_t lastUse = 0, bytes = 0, firstKey = 0;
    std::vector<uint64_t> keys;
  };
  std::vector<Unit> units;
  std::unordered_map<uint64_t, size_t> unitOf;
  for (size_t i = 0; i < all.size(); ++i) {
    if (aged[i]) continue;
    const Candidate& e = all[i];
    size_t u;
    if (e.group) {
      auto it = unitOf.find(e.group);
      if (it == unitOf.end()) {
        u = units.size();
        unitOf.emplace(e.group, u);
        units.push_back(Unit{e.lastUse, e.bytes, e.key, {}});
      } else {
        u = it->second;
      }
    } else {
      u = units.size();
      units.push_back(Unit{e.lastUse, e.bytes, e.key, {}});
    }
    Unit& un = units[u];
    un.lastUse = std::max(un.lastUse, e.lastUse);
    un.firstKey = std::min(un.firstKey, e.key);
    un.keys.push_back(e.key);
  }
  std::sort(units.begin(), units.end(), [](const Unit& a, const Unit& b) {
    return a.lastUse != b.lastUse ? a.lastUse < b.lastUse : a.firstKey < b.firstKey;
  });
  const uint64_t target = budget / 10u * 9u;
  const uint64_t keep = std::max<uint64_t>(keepFrames, 1);  // the P1 rule: never a resource bound in this frame
  for (const Unit& u : units) {
    if (left <= target || age(u.lastUse) < keep) break;  // sorted oldest first: the rest are newer still
    const size_t room = maxN - std::min(maxN, s.aged.size() + s.budget.size());
    if (u.keys.size() > room) {
      // fix3 (Astra 0a384c24 P2): a resource with more aliases than the cap allows goes alias by alias - this sweep
      // takes what fits (no bytes counted: they are freed with the last alias), the next sweep the rest. Progress
      // is guaranteed: every capped sweep evicts at least one key (maxN >= 1).
      s.budget.insert(s.budget.end(), u.keys.begin(), u.keys.begin() + room);
      s.capped = true;
      break;
    }
    s.budget.insert(s.budget.end(), u.keys.begin(), u.keys.end());
    s.budgetBytes += u.bytes;
    left = left > u.bytes ? left - u.bytes : 0u;
  }
  return s;
}

// Slot exhaustion's victim: the least recently bound entry, never one bound in the current frame (lastUse >= now)
// nor one whose slot is in pinned[0..nPinned) (the draw being assembled selected it; its tables are built after all
// its lookups). false = no victim: a transient exhaustion (every slot retiring or in use) - cache nothing, retry.
inline bool exhaustVictim(const std::vector<Candidate>& all, uint64_t now, const uint32_t* pinned, size_t nPinned,
                          uint64_t* victim) {
  bool found = false;
  uint64_t oldest = 0, best = 0;
  for (const Candidate& e : all) {
    if (e.lastUse >= now || std::find(pinned, pinned + nPinned, e.slot) != pinned + nPinned) continue;
    if (!found || e.lastUse < oldest || (e.lastUse == oldest && e.key < best)) {
      found = true;
      oldest = e.lastUse;
      best = e.key;
    }
  }
  if (found) *victim = best;
  return found;
}

// fix3 (Astra 0a384c24 P2): a transient exhaustion is retried only once the pool's state can have changed - a new
// frame, a fence completion, or a change in the free / retiring / next-slot / entry counts (the caller folds those
// into `state`); until then a failed bind returns at once instead of draining and scanning every entry again.
struct RetryGate {
  bool armed = false;
  uint64_t state = 0;
  bool shouldTry(uint64_t s) const { return !armed || s != state; }
  void failed(uint64_t s) { armed = true; state = s; }
  void succeeded() { armed = false; }
};
inline uint64_t poolState(std::initializer_list<uint64_t> parts) {
  uint64_t h = 14695981039346656037ull;
  for (uint64_t p : parts) h = (h ^ p) * 1099511628211ull;
  return h;
}

// fix2 / fix3: the one allocator of the slot pool - a retired slot the GPU is done with (when `useFree`), then a
// fresh one below `end`; 0 = none, and then nothing changed (the caller stays retryable: fix3, the movie planes).
inline uint32_t allocSlot(std::vector<uint32_t>& freeSlots, uint32_t& next, uint32_t end, bool useFree) {
  if (useFree && !freeSlots.empty()) {
    const uint32_t s = freeSlots.back();
    freeSlots.pop_back();
    return s;
  }
  return next < end ? next++ : 0u;
}

// Evicted slots waiting for the GPU. Entries are pushed in fence order; `Payload` holds what the slot kept alive.
template <class Payload>
class SlotRetire {
 public:
  struct Entry {
    uint32_t slot = 0;
    uint64_t fence = 0;  // reusable once the GPU completed this frame fence
    Payload payload{};
  };
  void retire(uint32_t slot, uint64_t fence, Payload p) { m_q.push_back(Entry{slot, fence, std::move(p)}); }
  size_t pending() const { return m_q.size(); }
  uint64_t violations() const { return m_violations; }
  // Releases at most `maxN` entries the GPU is done with (oldest first): `release(entry)` frees the payload and
  // makes the slot reusable. `lastUse(slot)` is the debug check: a slot bound after its retire fence (a stamp the
  // GPU has not passed) is held back and counted, never released (return 0 to skip the check).
  template <class LastUse, class Release>
  size_t drain(uint64_t completed, size_t maxN, LastUse lastUse, Release release) {
    size_t n = 0, held = 0;
    while (!m_q.empty() && n < maxN && m_q.front().fence <= completed && held < m_q.size()) {
      Entry e = std::move(m_q.front());
      m_q.pop_front();
      const uint64_t used = lastUse(e.slot);
      if (used > completed) {
        ++m_violations;
        ++held;
        e.fence = used;
        m_q.push_back(std::move(e));
        continue;
      }
      release(e);
      ++n;
    }
    return n;
  }
  template <class Release>
  void drainAll(Release release) {
    while (!m_q.empty()) {
      release(m_q.front());
      m_q.pop_front();
    }
  }

 private:
  std::deque<Entry> m_q;
  uint64_t m_violations = 0;
};

}  // namespace sr::gfx::texevict
