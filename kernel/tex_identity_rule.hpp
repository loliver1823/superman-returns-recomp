// iter-455n Phase 2 (r4-frozen): the KEY-ONLY texture-identity registry body —
// one implementation, driven by production AND by the self-test.
//
// This is an OBSERVATION-ONLY registry. It records which guest texture object
// VAs are seen at Create and/or Lock time; it NEVER reads a texture object /
// header (that is the deferred, unauthorized v2), NEVER produces a renderer
// command, upload, or guest write, and is INVISIBLE to every s_texMap consumer
// (it lives in its own map, s_texIdentity, distinct from the behavior-capable
// s_texMap). See docs/CLAIMS_LEDGER.md CLAIM iter-455n-phase2-coverage-boundary-r4
// (Dwight PASS 2026-08-16) for the frozen contract this body implements:
//   D1 independent cap/counters (never reads s_texMap.size());
//   D2 CREATE records two independent results in BOTH directions;
//   D3 disjoint conservation equations (identity vs behavior-create);
//   D4 independent mutexes, no nested cross-domain acquisition;
//   D5 registerTexIdentity is the SOLE structural/evidence mutator of s_texIdentity;
//   D6 gate-off is RUNTIME behavioral neutrality (not binary identity).
// Killers K1-K4 + the Lock r3 A/B/C invariants are exercised by
// kernel/tex_identity_test.cpp against this exact body.
#pragma once

#include <cstdint>
#include <cstddef>
#include "player_instance_rule.hpp"

namespace sr::kernel::texid {

// ── Provenance evidence set ──────────────────────────────────────────────────
enum class IdOrigin : uint8_t { CreateHook = 1u, LockLate = 2u };   // OR-ed bitset
// v1 only ever emits TextureLockRect. `Volume` is a reserved DISTINCT family (a
// future v2 could classify a volume texture) that exists so the merge-conflict
// guard on `family` is reachable/testable; v1 production never emits it.
enum class ResFamily : uint8_t { Unknown = 0, TextureLockRect = 1, Volume = 2 };

// A per-field metadata cell: UNKNOWN (!verified) or VERIFIED(value, source).
// v1 Lock-late supplies ONLY UNKNOWN; CREATE supplies VERIFIED create args.
struct MetaField { bool verified = false; uint32_t value = 0; uint8_t source = 0; };
inline MetaField Unknown() { return MetaField{}; }
inline MetaField Verified(uint32_t v, uint8_t src) { return MetaField{true, v, src}; }

struct IdentityRecord {
    uint32_t  key = 0;
    uint8_t   origins = 0;                       // OR of IdOrigin
    ResFamily family = ResFamily::Unknown;
    MetaField w, h, fmt, type;
};

// ── Terminal outcomes (identity domain) ──────────────────────────────────────
enum class IdOutcome : uint8_t {
    Inserted, DuplicateIdempotent, Merged, Conflict, InvalidKey, LockOutputInvalid, IdentityCapFull
};
inline const char* idOutcomeName(IdOutcome o) {
    switch (o) {
        case IdOutcome::Inserted:            return "inserted";
        case IdOutcome::DuplicateIdempotent: return "duplicate-idempotent";
        case IdOutcome::Merged:              return "merged";
        case IdOutcome::Conflict:            return "conflict";
        case IdOutcome::InvalidKey:          return "invalid-key";
        case IdOutcome::LockOutputInvalid:   return "lock-output-invalid";
        case IdOutcome::IdentityCapFull:     return "identity-cap-full";
    }
    return "?";
}

// D3 identity conservation: attempted == sum of the seven terminals.
// LockOutputInvalid is reachable for the LOCK origin only.
struct IdentityCounters {
    uint64_t attempted = 0, inserted = 0, dupIdem = 0, merged = 0, conflict = 0,
             invalidKey = 0, lockOutputInvalid = 0, capFull = 0;
    bool conserves() const {
        return attempted ==
            inserted + dupIdem + merged + conflict + invalidKey + lockOutputInvalid + capFull;
    }
};

// ── Merge matrix (existing key) ──────────────────────────────────────────────
// Two-pass: detect a hard conflict WITHOUT mutating (so a Conflict retains the
// prior record intact), else union the origin bit + promote UNKNOWN->VERIFIED.
//   origin union alone OR any promotion  => Merged
//   same origin, nothing promoted        => DuplicateIdempotent
//   VERIFIED!=VERIFIED or family mismatch => Conflict (prior intact)
inline IdOutcome mergeInto(IdentityRecord& cur, uint8_t origin, ResFamily fam,
                           const MetaField& w, const MetaField& h,
                           const MetaField& fmt, const MetaField& type) {
    auto verifiedDisagree = [](const MetaField& c, const MetaField& in) {
        return in.verified && c.verified && c.value != in.value;
    };
    const bool famConflict =
        (fam != ResFamily::Unknown && cur.family != ResFamily::Unknown && cur.family != fam);
    if (famConflict || verifiedDisagree(cur.w, w) || verifiedDisagree(cur.h, h) ||
        verifiedDisagree(cur.fmt, fmt) || verifiedDisagree(cur.type, type)) {
        return IdOutcome::Conflict;              // retain prior record intact
    }
    const bool originChanged = ((cur.origins | origin) != cur.origins);
    cur.origins |= origin;                       // monotonic union
    bool promoted = false;
    if (fam != ResFamily::Unknown && cur.family == ResFamily::Unknown) {
        cur.family = fam; promoted = true;
    }
    auto promote = [&](MetaField& c, const MetaField& in) {
        if (in.verified && !c.verified) { c = in; promoted = true; }
    };
    promote(cur.w, w); promote(cur.h, h); promote(cur.fmt, fmt); promote(cur.type, type);
    return (promoted || originChanged) ? IdOutcome::Merged : IdOutcome::DuplicateIdempotent;
}

// THE sole structural/evidence mutator of the identity map (D5). Caller holds
// the identity mutex. Existing-key adjudication precedes absent-key cap (D1).
template <class Map>
IdOutcome registerTexIdentity(Map& m, IdentityCounters& c, size_t cap,
                              uint32_t key, uint8_t origin, ResFamily fam,
                              MetaField w, MetaField h, MetaField fmt, MetaField type) {
    c.attempted++;
    if (key < 0x1000u) { c.invalidKey++; return IdOutcome::InvalidKey; }
    auto it = m.find(key);
    if (it != m.end()) {
        const IdOutcome o = mergeInto(it->second, origin, fam, w, h, fmt, type);
        switch (o) {
            case IdOutcome::Merged:   c.merged++;   return IdOutcome::Merged;
            case IdOutcome::Conflict: c.conflict++; return IdOutcome::Conflict;
            default:                  c.dupIdem++;  return IdOutcome::DuplicateIdempotent;
        }
    }
    if (m.size() >= cap) { c.capFull++; return IdOutcome::IdentityCapFull; }
    m[key] = IdentityRecord{key, origin, fam, w, h, fmt, type};
    c.inserted++;
    return IdOutcome::Inserted;
}

// Lock-path non-inserting terminals (each is one attempt in the same stream).
inline void noteInvalidKeyLock(IdentityCounters& c)    { c.attempted++; c.invalidKey++; }
inline void noteLockOutputInvalid(IdentityCounters& c) { c.attempted++; c.lockOutputInvalid++; }

// ── Lock observer-admission (blocker-1 contract, r4-frozen) ──────────────────
// Post-delegate r3 is a computed address/offset and is NEVER consulted here —
// this function takes NO context, so it structurally cannot gate on r3 or alter
// it (Lock r3-neutrality). Admission reads ONE contiguous checked 8-byte BE span
// at the SAVED (pre-delegate) outVA; readable + pitch!=0 + bits!=0 admits a
// usable mapping. This is observer admission, NOT an API success/failure code.
struct AdmitResult { bool admit = false; uint32_t pitch = 0, bits = 0; };
template <class Span8>            // Span8(va, bytes) -> const uint8_t* (checked) or nullptr
AdmitResult lockLateAdmit(uint32_t outVA, Span8 span8) {
    AdmitResult r{};
    if (!outVA) return r;
    const uint8_t* p = span8(outVA, 8u);
    if (!p) return r;
    r.pitch = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    r.bits  = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
    r.admit = (r.pitch != 0 && r.bits != 0);
    return r;
}

// ── Behavior-create accounting (SEPARATE domain, disjoint conservation D3) ────
// Names the CURRENT s_texMap create semantics EXACTLY (audit 2026-08-16): the
// create path has no find/compare/idempotence/conflict — operator[] is an
// unconditional insert-if-absent / overwrite-if-present, wrapped by the cap and
// >=0x1000 guards. So THREE fused outcomes, never four. This function only
// COUNTS; it never changes the assignment.
enum class BehaviorOutcome : uint8_t { Upserted, InvalidKey, CapFull };
struct BehaviorCreateCounters {
    uint64_t attempted = 0, upserted = 0, invalidKey = 0, capFull = 0;
    bool conserves() const { return attempted == upserted + invalidKey + capFull; }
};
inline BehaviorOutcome classifyBehaviorCreate(BehaviorCreateCounters& c,
                                              uint32_t tex, size_t size, size_t cap) {
    c.attempted++;
    if (tex < 0x1000u) { c.invalidKey++; return BehaviorOutcome::InvalidKey; }
    if (size >= cap)   { c.capFull++;   return BehaviorOutcome::CapFull; }   // includes existing-key-at-cap
    c.upserted++;      return BehaviorOutcome::Upserted;                     // fused insert/overwrite
}

// ── K4 cross-domain lock-order detector ──────────────────────────────────────
// A thread-local domain marker: entering one domain's lock while the other is
// already held sets ok=false. Production wraps each domain's lock scope with it
// (they are sequential, never nested, so ok stays true); the self-test nests
// them deliberately to prove the detector fires.
enum class LockDomain : uint8_t { None, Identity, Behavior };
inline LockDomain& tlsLockDomain() { thread_local LockDomain d = LockDomain::None; return d; }
struct DomainLockScope {
    LockDomain prev;
    bool ok;
    explicit DomainLockScope(LockDomain want) {
        prev = tlsLockDomain();
        ok = (prev == LockDomain::None);         // no OTHER domain lock held
        tlsLockDomain() = want;
    }
    ~DomainLockScope() { tlsLockDomain() = prev; }
    DomainLockScope(const DomainLockScope&) = delete;
    DomainLockScope& operator=(const DomainLockScope&) = delete;
};

// THE ONE shared domain-ordered acquisition helper (production AND self-test use
// this exact class). It enters the domain marker BEFORE the mutex and acquires
// the mutex ONLY when no conflicting domain is already held on this thread. On a
// nested cross-domain attempt it REJECTS: the mutex is never touched, so an
// inversion deadlock is impossible in EITHER order (Behavior->Identity and
// Identity->Behavior alike). locked() reports whether the guarded section may run.
// Templated on Mutex (calls .lock()/.unlock()) so the header stays free of
// <mutex>; production passes std::mutex, tests pass std::mutex or a counting fake.
template <class Mutex>
class DomainOrderedLock {
    DomainLockScope scope_;
    Mutex* held_ = nullptr;
public:
    DomainOrderedLock(LockDomain domain, Mutex& m) : scope_(domain) {
        if (scope_.ok) { m.lock(); held_ = &m; }   // acquire ONLY if not nested
    }
    ~DomainOrderedLock() { if (held_) held_->unlock(); }   // release BEFORE marker clears
    DomainOrderedLock(const DomainOrderedLock&) = delete;
    DomainOrderedLock& operator=(const DomainOrderedLock&) = delete;
    bool locked() const { return scope_.ok; }
};


// ── Production feature gate (cached once) — declared here, defined in
// kernel/hle_api_observe.cpp — so the self-test can drive the REAL gate and prove
// the per-hook-getenv mutant fails: the environment reader runs exactly once
// regardless of call volume, observable via productionGateInitCount().
bool productionGateEnabled();
int  productionGateInitCount();

}  // namespace sr::kernel::texid
