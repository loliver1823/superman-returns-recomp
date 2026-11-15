#pragma once

// ── THE UNLOCK-OWNERSHIP STATE MACHINE (iter-456n, sixth adjudication) ──────
//
// ONE decision, used by production and driven directly by the tests. It exists
// because the previous ownership predicate was REVERSED against its own
// documented contract — `unlockOwnedForOther` returned true for a DIFFERENT
// signature, and the upload arm admitted capture when that was false, so it
// admitted exactly the same-epoch stale capture Class A exists to block and
// refused the recreation it must admit. The 54-test suite missed it because
// R2-5 only recomputed a local signature and compared it to itself; it never
// drove the production decision. Hence this header: the decision lives in one
// place, the production path has no private copy of it, and a test that drives
// this class is testing what actually runs.
//
// THE CONTRACT
//   An Unlock publication CLAIMS a (normalized key, descriptor epoch,
//   generation). A later bind-time capture of the same key is:
//     * REFUSED   when it matches the live claim exactly (same descriptor, same
//                 generation) — that is the stale capture clobbering fresh
//                 unlock pixels, measured live as key 0BC22000 going fresh at
//                 upload #6 and all-zero at #15.
//     * ADMITTED  when no claim exists;
//     * ADMITTED  when the descriptor differs — the object was recreated at
//                 this address with an incompatible descriptor, so the old
//                 claim does not carry over and is REPLACED on the next claim;
//     * ADMITTED  when the generation differs — the object was destroyed and
//                 something else now lives at this physical address. This is
//                 the case a descriptor comparison ALONE cannot see, because a
//                 recreation can reuse the same address AND the same
//                 descriptor; without a generation it silently inherits
//                 ownership from a dead object.
//
// KEYS ARE NORMALIZED HERE, not by the caller. Guest pointers arrive in both
// the 0x8… and 0xA… alias windows and both name the same pixels; folding at
// the boundary is the only way a caller cannot forget to.
//
// NOT COVERED, deliberately and visibly: this class does not decide WHEN a
// generation changes — it only records that one did. Production raises the
// event from the texture-create registration, so an object whose creation the
// harness never observes keeps its previous generation.

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "../kernel/sr_oracle.hpp"

namespace sr {
namespace gfx {
namespace hle {

// The bind-capture decision, as a REASON rather than a bool, so the counters
// and the tests can tell the three ADMIT paths apart. A bare bool is how the
// inversion hid: both sides of it looked plausible.
enum class CaptureVerdict : uint32_t {
  AdmitNoOwner = 0,       // nothing has ever claimed this key
  AdmitRecreated,         // claimed, but under a DIFFERENT descriptor
  AdmitStaleGeneration,   // claimed, but that object is gone (destroy/reuse)
  RefuseSameEpoch,        // claimed, same descriptor, same generation
};

inline bool captureAdmitted(CaptureVerdict v) {
  return v != CaptureVerdict::RefuseSameEpoch;
}

inline const char* captureVerdictName(CaptureVerdict v) {
  switch (v) {
    case CaptureVerdict::AdmitNoOwner:      return "AdmitNoOwner";
    case CaptureVerdict::AdmitRecreated:    return "AdmitRecreated";
    case CaptureVerdict::AdmitStaleGeneration: return "AdmitStaleGeneration";
    case CaptureVerdict::RefuseSameEpoch:   return "RefuseSameEpoch";
  }
  return "?";
}

// ── WHO owns a key's pixels (slice 6, Class B v2) ──────────────────────────
// An Unlock publication and a backend-confirmed Resolve are both authoritative
// producers of a texture's content, and both must be able to refuse a stale
// bind capture. They differ only in provenance, so they share one claim record
// and one epoch rule; the kind is carried so the census and the tests can tell
// a resolve transition from an unlock one.
enum class Authority : uint32_t { None = 0, Unlock, Resolve };

inline const char* authorityName(Authority a) {
  switch (a) {
    case Authority::None:    return "none";
    case Authority::Unlock:  return "unlock";
    case Authority::Resolve: return "resolve";
  }
  return "?";
}

// ── slice 6: EXACTLY ONE TRACE ARM ────────────────────────────────────────
// Silently preferring one arm over another banks a fixture armed by a rule the
// operator did not choose, and the artifact cannot show which one fired. A
// conflict must REFUSE. Pure so it is testable without a roll — my first
// attempt to check this empirically ran in replay mode, where the recorder is
// never entered, and proved nothing.
// An arm variable set to "0" or "" is DISABLED, not enabled. Counting presence
// rather than effective value made `SR_HLE_TRACE_ARM_UNLOCKN=0` register as an
// active arm, so an operator disabling one arm the obvious way would have
// tripped the conflict refusal.
inline bool envArmEnabled(const char* v) {
  return v && v[0] && !(v[0] == '0' && v[1] == '\0');
}

// The arm names, and the counting rule, in ONE place that production calls.
// Taking the lookup as a parameter is what makes the PRODUCTION decision
// testable: production passes std::getenv, a test passes a stub. Testing a
// predicate over an already-computed count bound nothing — restoring
// presence-counting still passed the whole suite.
inline const char* const* traceArmNames(int& count) {
  static const char* kArms[] = {
      "SR_HLE_TRACE_HOTKEY",         "SR_HLE_TRACE_ARM_TEXKEY",
      "SR_HLE_TRACE_ARM_UNLOCKN",    "SR_HLE_TRACE_ARM_RESOLVEN",
      "SR_HLE_TRACE_ARM_PRERESOLVE", "SR_HLE_TRACE_ARM_GEOM"};
  count = 6;
  return kArms;
}

using TraceEnvLookup = const char* (*)(const char*);

inline int countEnabledArms(TraceEnvLookup get) {
  int n = 0, total = 0;
  const char* const* names = traceArmNames(total);
  for (int i = 0; i < total; i++)
    if (envArmEnabled(get(names[i]))) n++;
  return n;
}

inline bool traceArmsConflict(int enabledArms) { return enabledArms > 1; }
inline bool traceArmsUsable(int enabledArms) { return enabledArms == 1; }

// ── slice 6: IS THE OPENING COMMAND ITSELF PART OF THE TRACE? ─────────────
// A Present-boundary arm opens ON a Present and must NOT serialize it: the
// frame starts after it, and recording it would duplicate the boundary.
// The pre-treatment arm opens on the triggering RESOLVE, and that Resolve is
// the first TREATMENT EVENT — the whole reason the checkpoint sits where it
// does. Returning before serializing it silently produced a control fixture
// missing the very command it was built around.
//
// `isPresent` is the opening command's type; `preTreatArm` says which arm fired.
inline bool openingCommandIsRecorded(bool isPresent, bool preTreatArm) {
  return preTreatArm && !isPresent;
}

// ── slice 6: WHICH RULE decided a capture, as a pure function ─────────────
// The executor used to classify this inline, which meant the attribution the
// whole OFF/ON control rests on was shipped untested. It is a total function of
// three inputs, so it is written once here and driven directly by the tests.
enum class CaptureDisposition : uint32_t {
  Applied = 0,
  RefusedLegacyResolveSet,
  RefusedResolveEpoch,
  RefusedUnlockEpoch,
  RefusedNoOwnerOther,
  UploadFailed,
};

// `captureOk` is the ownership opinion; `legacyBlocked` is v1's permanent
// physical-key set (consulted only when Class-B policy is OFF); `prior` is the
// authority in force when the decision was taken. The legacy set is checked
// FIRST because when it blocks, it blocks regardless of what ownership said —
// that precedence IS the difference the control measures.
inline CaptureDisposition classifyDisposition(bool captureOk, bool legacyBlocked,
                                              Authority prior) {
  if (legacyBlocked) return CaptureDisposition::RefusedLegacyResolveSet;
  if (captureOk)     return CaptureDisposition::Applied;
  switch (prior) {
    case Authority::Resolve: return CaptureDisposition::RefusedResolveEpoch;
    case Authority::Unlock:  return CaptureDisposition::RefusedUnlockEpoch;
    default:                 return CaptureDisposition::RefusedNoOwnerOther;
  }
}

class UnlockOwnership {
 public:
  // Guest alias fold. Both windows name the same pixels.
  static uint32_t normalize(uint32_t key) { return key & 0x1FFFFFFFu; }

  // The DESCRIPTOR epoch: every field the decode depends on.
  // fmtBits bits 0-25 carry fmt / tiled / endian / dimension / volume depth.
  // Bit 26 is UNLOCK-ORIGIN — provenance, not descriptor — and is masked out,
  // so a widened publish and a bind capture of the same surface compare equal
  // (which is exactly the comparison the refusal depends on).
  static uint64_t signature(uint32_t fmtBits, uint32_t dims, uint32_t pitch) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    const uint64_t parts[3] = {(uint64_t)(fmtBits & 0x03FFFFFFu), (uint64_t)dims,
                               (uint64_t)pitch};
    for (uint64_t p : parts) {          // splitmix-style mix: no shift-XOR
      h ^= p + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
      h ^= h >> 33;
    }
    return h ? h : 1ull;                // 0 is reserved for "no claim"
  }

  // A texture object was CREATED (or destroyed) at this address. Anything
  // claimed under the old generation is now stale, whatever its descriptor.
  // ⚠ RENDER THREAD ONLY, and it must arrive IN STREAM ORDER. Calling this
  // directly from the guest/API hook was a data race against claim() and
  // captureVerdict() on the render thread AND a semantic bug: a create raised
  // immediately can overtake TextureUnlock/TextureUpload commands already
  // queued for the OLD object, so those old commands then execute under the new
  // generation. It is delivered as RenderCommandType::TextureObjectEvent.
  // `reason`: 0 = create, 1 = destroy/release. Counted separately because one
  // aggregate number cannot say whether a route is firing at the right
  // boundary - which is what the coverage question actually asks.
  void noteObjectEvent(uint32_t key, uint32_t reason = 0) {
    m_generation[normalize(key)]++;
    m_events++;
    if (reason == 1) m_evDestroy++; else m_evCreate++;
  }

  // ── THE ORDERED CAPTURE-DECISION WITNESS (ninth adjudication) ────────────
  // Counts alone cannot show that live and replay made the SAME decisions in
  // the SAME order, and a restored-claim census cannot either: a fixture with
  // no ordinary bind captures and no lifecycle events reports new=0/refusals=0
  // whatever the decision logic does. This folds every production decision -
  // (normalized key, signature, generation, verdict) - into a rolling digest,
  // so live and replay are comparable exactly rather than by aggregate.
  // Recorded at the DECISION site, not at the call site, so nothing can decide
  // without being witnessed.
  void noteVerdict(uint32_t key, uint64_t sig, uint64_t gen, CaptureVerdict v) {
    uint64_t h = m_verdictDigest.load(std::memory_order_relaxed);
    const uint64_t parts[4] = {(uint64_t)normalize(key), sig, gen,
                               (uint64_t)v};
    for (uint64_t x : parts) {
      h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
      h ^= h >> 33;
    }
    m_verdictDigest.store(h, std::memory_order_relaxed);
    m_verdictN++;
    switch (v) {
      case CaptureVerdict::AdmitNoOwner:         m_vNoOwner++;    break;
      case CaptureVerdict::AdmitRecreated:       m_vRecreated++;  break;
      case CaptureVerdict::AdmitStaleGeneration: m_vStaleGen++;   break;
      case CaptureVerdict::RefuseSameEpoch:      m_vRefuse++;     break;
    }
  }
  // ⚠ WINDOW-RELATIVE BY DESIGN. A rolling digest seeded at trace open cannot
  // be compared live-vs-replay, because a replay must first re-execute the
  // PREAMBLE, and every preamble upload is another capture decision - ones live
  // made BEFORE the window. Measured: live made 4 in-window decisions
  // (1 admit + 3 refuse) while its replay reported 10 (1 admit + 9 refuse),
  // the extra six being preamble re-decisions folded into the same digest.
  // So the window START zeroes both the digest and the counters on BOTH sides.
  // The live close value is then a pure function of the in-window decision
  // sequence, and the replay must reproduce it exactly.
  void beginVerdictWindow() {
    // Unmatched restore markers do not survive into the window: a claim that
    // never arrived must not suppress a later, unrelated publication.
    for (auto& kv : m_claim) kv.second.restoredPending = false;
    m_verdictDigest.store(0, std::memory_order_relaxed);
    m_verdictN = 0; m_vNoOwner = 0; m_vRecreated = 0; m_vStaleGen = 0;
    m_vRefuse = 0;
  }
  uint64_t verdictDigest() const {
    return m_verdictDigest.load(std::memory_order_relaxed);
  }
  uint64_t verdictCount() const { return m_verdictN.load(std::memory_order_relaxed); }
  uint64_t verdictNoOwner() const { return m_vNoOwner.load(std::memory_order_relaxed); }
  uint64_t verdictRecreated() const { return m_vRecreated.load(std::memory_order_relaxed); }
  uint64_t verdictStaleGen() const { return m_vStaleGen.load(std::memory_order_relaxed); }
  uint64_t verdictRefuse() const { return m_vRefuse.load(std::memory_order_relaxed); }

  CaptureVerdict captureVerdict(uint32_t key, uint64_t sig) const {
    const uint32_t k = normalize(key);
    const auto it = m_claim.find(k);
    if (it == m_claim.end() || it->second.sig == 0)
      return CaptureVerdict::AdmitNoOwner;
    if (it->second.sig != sig) return CaptureVerdict::AdmitRecreated;
    if (it->second.generation != generationOf(k))
      return CaptureVerdict::AdmitStaleGeneration;
    return CaptureVerdict::RefuseSameEpoch;
  }

  // An Unlock publication takes ownership. REPLACES any previous claim — a
  // claim is never merged, so a recreation cannot inherit the dead object's.
  void claim(uint32_t key, uint64_t sig) {
    const uint32_t k = normalize(key);
    Claim& c = m_claim[k];
    const uint64_t gen = generationOf(k);
    // ⚠ The restore marker may be consumed ONLY by a claim that actually
    // matches the restored descriptor AND generation (tenth adjudication).
    // Consuming it on ANY first claim silently suppressed a recreation or a
    // mismatched publication — the exact class of write that must be counted.
    const bool matchesRestore =
        c.restoredPending && c.sig == sig && c.generation == gen;
    if (c.sig == 0) {
      m_new++;
      // ── THE CREATE-COVERAGE GAUGE (eighth adjudication) ────────────────
      // A key whose FIRST claim arrives while its generation is still 0 was
      // never announced by any create route we hook. That is the exact
      // residual of incomplete lifecycle coverage, measured rather than
      // argued: if every construction path raised an event, this would be 0.
      // It is a gauge, not a guard - nothing branches on it.
      if (m_generation.find(k) == m_generation.end()) m_unattributed++;
    }
    else if (c.sig == sig && c.generation == gen) m_same++;
    else m_replaced++;
    c.sig = sig;
    c.generation = gen;
    // Any claim resolves the pending state; only a MATCHING one is treated as
    // the re-assertion the restored publish count already covers.
    c.restoredPending = false;
    c.authority = Authority::Unlock;   // a later Unlock supersedes a resolve
    if (!matchesRestore) c.publishes++;
    // Published for the cross-thread census, which reads atomics only.
    m_keys.store(m_claim.size(), std::memory_order_relaxed);
  }

  // ── TRACE PREAMBLE SUPPORT (iter-456n, seventh adjudication) ─────────────
  // A replay used to start with EMPTY ownership while the live run it replays
  // had already accumulated claims and generations, so the two made different
  // accept/refuse decisions on the same command stream. The preamble now
  // carries this state; these two members are how it is read out and put back.
  // Both run on the render thread, like every other ownership access.
  void restoreClaim(uint32_t key, uint64_t sig, uint64_t generation,
                    uint64_t publishes, Authority authority = Authority::Unlock) {
    const uint32_t k = normalize(key);
    Claim& c = m_claim[k];
    c.sig = sig;
    c.generation = generation;
    c.publishes = publishes;
    c.restoredPending = true;
    c.authority = authority;
    m_generation[k] = generation;
    m_keys.store(m_claim.size(), std::memory_order_relaxed);
  }

  template <typename F>
  void forEachClaim(F fn) const {
    for (const auto& kv : m_claim)
      if (kv.second.sig != 0)
        fn(kv.first, kv.second.sig, kv.second.generation, kv.second.publishes,
           kv.second.authority);
  }

  // A BACKEND-CONFIRMED resolve claims the same (key, descriptor epoch,
  // generation) an Unlock would. Only success reaches here: an attempted or
  // refused resolve must change nothing, which is enforced at the call site by
  // claiming only when the backend returned a slot.
  //
  // It REPLACES whatever held the key, because a resolve product is newer than
  // anything that came before it — and it is superseded in turn by a later
  // Unlock claim, by a generation advance (create/destroy/physical reuse), or
  // by a publication under an incompatible descriptor, all of which fall out of
  // the shared epoch rule rather than needing a special case.
  void claimResolve(uint32_t key, uint64_t sig) {
    const uint32_t k = normalize(key);
    Claim& c = m_claim[k];
    const uint64_t gen = generationOf(k);
    if (c.sig == 0) m_new++;
    else if (c.sig == sig && c.generation == gen) m_same++;
    else m_replaced++;
    c.sig = sig;
    c.generation = gen;
    c.publishes++;
    c.restoredPending = false;
    c.authority = Authority::Resolve;
    m_resolveClaims++;
    m_keys.store(m_claim.size(), std::memory_order_relaxed);
  }

  Authority authorityFor(uint32_t key) const {
    const auto it = m_claim.find(normalize(key));
    return (it == m_claim.end() || it->second.sig == 0) ? Authority::None
                                                        : it->second.authority;
  }
  // The claim's descriptor epoch, or 0 when the key is unclaimed. Used by the
  // legacy-record fallback, which may adopt an ALREADY ESTABLISHED descriptor
  // but must never invent one.
  uint64_t signatureFor(uint32_t key) const {
    const auto it = m_claim.find(normalize(key));
    return it == m_claim.end() ? 0ull : it->second.sig;
  }
  uint64_t resolveClaims() const {
    return m_resolveClaims.load(std::memory_order_relaxed);
  }

  uint64_t generationOf(uint32_t key) const {
    const auto it = m_generation.find(normalize(key));
    return it == m_generation.end() ? 0ull : it->second;
  }
  uint64_t publishesFor(uint32_t key) const {
    const auto it = m_claim.find(normalize(key));
    return it == m_claim.end() ? 0ull : it->second.publishes;
  }
  bool hasClaim(uint32_t key) const {
    const auto it = m_claim.find(normalize(key));
    return it != m_claim.end() && it->second.sig != 0;
  }

  void noteRefusal() { m_refused++; }
  // ── CENSUS ACCESSORS ARE CROSS-THREAD ────────────────────────────────────
  // The maps are touched ONLY on the render thread (every mutation now arrives
  // as an ordered stream command), but the periodic census is printed from the
  // API thread. Reading m_claim.size() from there would be a data race on an
  // unordered_map, so the render thread PUBLISHES the key count into an atomic
  // and the census reads only atomics. Seventh adjudication.
  uint64_t claimsNew() const { return m_new.load(std::memory_order_relaxed); }
  uint64_t claimsSame() const { return m_same.load(std::memory_order_relaxed); }
  uint64_t claimsReplaced() const { return m_replaced.load(std::memory_order_relaxed); }
  uint64_t refusals() const { return m_refused.load(std::memory_order_relaxed); }
  uint64_t objectEvents() const { return m_events.load(std::memory_order_relaxed); }
  uint64_t createEvents() const { return m_evCreate.load(std::memory_order_relaxed); }
  uint64_t destroyEvents() const { return m_evDestroy.load(std::memory_order_relaxed); }
  uint64_t unattributedFirstClaims() const {
    return m_unattributed.load(std::memory_order_relaxed);
  }
  uint64_t trackedKeys() const { return m_keys.load(std::memory_order_relaxed); }

  void reset() {
    m_claim.clear(); m_generation.clear();
    m_new = 0; m_same = 0; m_replaced = 0; m_refused = 0; m_events = 0;
    m_keys = 0; m_evCreate = 0; m_evDestroy = 0; m_unattributed = 0;
    m_resolveClaims = 0;
    m_verdictDigest = 0; m_verdictN = 0; m_vNoOwner = 0; m_vRecreated = 0;
    m_vStaleGen = 0; m_vRefuse = 0;
  }

 private:
  struct Claim {
    uint64_t sig = 0;
    uint64_t generation = 0;
    uint64_t publishes = 0;
    Authority authority = Authority::None;
    // iter-456n (ninth adjudication): true between a preamble RESTORE and the
    // first matching claim that follows it. The preamble's own uploads replay
    // writes the restored count ALREADY includes, so counting them again makes
    // replay's publish counts drift above live's. The first matching claim
    // consumes this flag instead of incrementing.
    bool restoredPending = false;
  };
  std::unordered_map<uint32_t, Claim> m_claim;
  std::unordered_map<uint32_t, uint64_t> m_generation;
  std::atomic<uint64_t> m_new{0}, m_same{0}, m_replaced{0}, m_refused{0},
      m_events{0}, m_keys{0}, m_evCreate{0}, m_evDestroy{0},
      m_unattributed{0}, m_resolveClaims{0}, m_verdictDigest{0}, m_verdictN{0}, m_vNoOwner{0},
      m_vRecreated{0}, m_vStaleGen{0}, m_vRefuse{0};
};

#if SR_ORACLE
// THE production instance. The tests drive this one, not a copy, so a test can
// never pass against a state machine production does not use.
UnlockOwnership& unlockOwnership();

// Raised by the texture create / destroy-release hooks (kernel side). ENQUEUES
// a RenderCommandType::TextureObjectEvent; it never touches ownership state on
// the calling thread. reason: 0 = create, 1 = destroy/release.
void noteTextureObjectEvent(uint32_t pixelKey, uint32_t reason = 0);
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): both live in hle_video.cpp, compiled out with the HLE renderer A/B (CMakeLists.txt
// ORACLE_SOURCES). In the dev build the texture create / release hooks raise lifecycle events on the default PM4 path
// too, and the HLE consumer thread applies them to this machine: observer bookkeeping (generations, event counts).
// The release drops it - an event is discarded, and the instance the [S6_OWNERSHIP] log line reads stays empty.
// Nothing outside the HLE executor (compiled out) and that log line reads the machine (evidence: gfx/hle_video.hpp,
// at renderCommandName), so gameplay and rendering are unaffected; the observer's own counts are not the same.
inline UnlockOwnership& unlockOwnership() {
  static UnlockOwnership s_own;
  return s_own;
}
inline void noteTextureObjectEvent(uint32_t, uint32_t = 0) {}
#endif

}  // namespace hle
}  // namespace gfx
}  // namespace sr
