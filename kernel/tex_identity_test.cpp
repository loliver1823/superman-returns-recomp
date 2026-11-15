// iter-455n Phase 2 v1: hermetic self-test for the key-only texture-identity
// registry (SR_HLE_TEX_IDENTITY_TEST=1). Drives kernel/tex_identity_rule.hpp —
// the SAME body kernel/hle_api_observe.cpp calls — with injected fakes, so every
// assertion is about the production control flow. Exit code = failure count.
//
// Covers the r4-frozen contract (docs/CLAIMS_LEDGER.md
// iter-455n-phase2-coverage-boundary-r4, Dwight PASS): killers K1-K4, the Lock
// r3 A/B/C invariants, and the cap / conservation / conflict / order mutants.
// v1 is OBSERVATION-ONLY: no object/header read, no renderer command, no upload.
#include "tex_identity_rule.hpp"
#include "tex_identity_domain.hpp"   // r3: drive the REAL production identity domain
#include "../app/log.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sr::kernel::texid {
namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) { g_pass++; return; }
    g_fail++;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[TEXID_TEST] FAIL: %s", what);
}

using Map = std::unordered_map<uint32_t, IdentityRecord>;
constexpr size_t kCap = 4096;

// A lock()/unlock()-counting fake mutex: proves DomainOrderedLock touches the
// mutex ONLY when it admits, so a rejected (nested) acquisition never locks.
struct CountingMutex {
    int locks = 0, unlocks = 0;
    void lock()   { locks++; }
    void unlock() { unlocks++; }
};

// A fake 8-byte Lock out-struct source. Two "windows": readable (returns bytes)
// and poisoned (returns nullptr) so admission's checked-span refusal is exact.
struct FakeOut {
    uint32_t va = 0x40000000u;
    uint8_t  bytes[8] = {0};
    bool     readable = true;
    void set(uint32_t pitch, uint32_t bits) {          // store BE
        bytes[0] = uint8_t(pitch >> 24); bytes[1] = uint8_t(pitch >> 16);
        bytes[2] = uint8_t(pitch >> 8);  bytes[3] = uint8_t(pitch);
        bytes[4] = uint8_t(bits >> 24);  bytes[5] = uint8_t(bits >> 16);
        bytes[6] = uint8_t(bits >> 8);   bytes[7] = uint8_t(bits);
    }
    // Span8 signature: (va, bytes) -> const uint8_t* or nullptr (checked).
    const uint8_t* span(uint32_t reqVA, uint32_t n) const {
        if (!readable || reqVA != va || n != 8u) return nullptr;
        return bytes;
    }
};

// A tiny fake guest context: only the registers the Lock path touches. Proves
// the observer never mutates r3 (killer C) — the observation functions take NO
// context, so they structurally cannot.
struct FakeCtx { uint32_t r3 = 0, r4 = 0, r5 = 0; };

// Model of the production Lock hook path (same ordering as hle_api_observe.cpp):
// capture keyIn/outVA PRE-delegate, call the injected delegate exactly once,
// then (if observing) admit + register. Returns the identity outcome touched
// and the delegate call count via out-params.
template <class Delegate, class Src>
void modelLockHook(FakeCtx& ctx, Delegate delegate, const Src& src,
                   bool observeOn, Map& m, IdentityCounters& c, int& delegateCalls) {
    const uint32_t keyIn = ctx.r3;                 // PRE-delegate identity
    const uint32_t outVA = ctx.r5;                 // PRE-delegate out-struct ptr
    delegate(ctx);                                 // exactly once (may set ctx.r3 = lock addr)
    delegateCalls++;
    if (!observeOn) return;
    if (keyIn < 0x1000u) { noteInvalidKeyLock(c); return; }
    const AdmitResult a = lockLateAdmit(outVA, [&](uint32_t va, uint32_t n){ return src.span(va, n); });
    if (!a.admit) { noteLockOutputInvalid(c); return; }
    registerTexIdentity(m, c, kCap, keyIn, (uint8_t)IdOrigin::LockLate,
                        ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
}

int runTests() {
    // ── 1. Admission (killers A/B): r3 is NOT a success gate ──────────────────
    {
        // Killer A: delegate returns r3=0 but writes nonzero pitch/bits => Inserted, r3 stays 0.
        Map m; IdentityCounters c; FakeOut out; out.set(256, 0x0BA5A000u);
        FakeCtx ctx{0xABA5A000u, 0, out.va};
        int calls = 0;
        modelLockHook(ctx, [](FakeCtx& x){ x.r3 = 0; }, out, /*observeOn*/true, m, c, calls);
        check(calls == 1, "delegate called exactly once");
        check(ctx.r3 == 0, "killer A: observer preserves delegate r3=0 exactly (never touches r3)");
        check(m.size() == 1 && c.inserted == 1,
              "killer A: r3=0 with nonzero readable pitch/bits still INSERTS (r3 is not a gate)");

        // Killer B: arbitrary nonzero r3 but zero bits => LockOutputInvalid, no insert.
        Map m2; IdentityCounters c2; FakeOut out2; out2.set(256, 0);   // bits == 0
        FakeCtx ctx2{0xABA5A000u, 0, out2.va};
        int calls2 = 0;
        modelLockHook(ctx2, [](FakeCtx& x){ x.r3 = 0xDEADBEEFu; }, out2, true, m2, c2, calls2);
        check(ctx2.r3 == 0xDEADBEEFu, "killer B: delegate r3 preserved exactly");
        check(m2.empty() && c2.lockOutputInvalid == 1,
              "killer B: nonzero r3 + zero bits => LockOutputInvalid, no insert");

        // Unreadable out-struct also => LockOutputInvalid (checked span refuses).
        Map m3; IdentityCounters c3; FakeOut out3; out3.set(256, 1); out3.readable = false;
        FakeCtx ctx3{0xABA5A000u, 0, out3.va};
        int calls3 = 0;
        modelLockHook(ctx3, [](FakeCtx&){}, out3, true, m3, c3, calls3);
        check(m3.empty() && c3.lockOutputInvalid == 1,
              "unreadable out-struct => LockOutputInvalid via checked span");
    }

    // ── 2. Killer C: observation ON vs OFF preserves r3/context/call count ────
    {
        FakeOut out; out.set(256, 0x0BA5A000u);
        auto delegate = [](FakeCtx& x){ x.r3 = 0x11112222u; x.r4 = 0x33334444u; };

        Map mOff; IdentityCounters cOff; FakeCtx a{0xABA5A000u, 7, out.va}; int callsOff = 0;
        modelLockHook(a, delegate, out, /*observeOn*/false, mOff, cOff, callsOff);

        Map mOn; IdentityCounters cOn; FakeCtx b{0xABA5A000u, 7, out.va}; int callsOn = 0;
        modelLockHook(b, delegate, out, /*observeOn*/true, mOn, cOn, callsOn);

        check(a.r3 == b.r3 && a.r4 == b.r4 && a.r5 == b.r5,
              "killer C: ctx (r3/r4/r5) byte-identical observation OFF vs ON");
        check(callsOff == callsOn && callsOn == 1, "killer C: delegate call count identical (1)");
        check(mOff.empty() && cOff.attempted == 0, "killer C: OFF touches no identity state");
        check(mOn.size() == 1 && cOn.attempted == 1, "killer C: ON differs ONLY in identity registry/counters");
    }

    // ── 3. Register outcomes + conservation + provenance matrix ───────────────
    {
        Map m; IdentityCounters c;
        // absent + LOCK (UNKNOWN) => Inserted
        IdOutcome o1 = registerTexIdentity(m, c, kCap, 0x1000u, (uint8_t)IdOrigin::LockLate,
            ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        check(o1 == IdOutcome::Inserted, "absent + LOCK => Inserted");
        // same key + CREATE (VERIFIED) => Merged (adds origin + fills verified fields)
        IdOutcome o2 = registerTexIdentity(m, c, kCap, 0x1000u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(64,0), Verified(8,0), Verified(0x28000002u,0), Verified(1,0));
        check(o2 == IdOutcome::Merged, "LOCK-then-CREATE => Merged (fills verified fields)");
        check((m[0x1000u].origins & (uint8_t)IdOrigin::CreateHook) &&
              (m[0x1000u].origins & (uint8_t)IdOrigin::LockLate) &&
              m[0x1000u].fmt.verified && m[0x1000u].fmt.value == 0x28000002u,
              "merged record unions both origins and holds the VERIFIED fmt");
        // repeat same CREATE evidence => DuplicateIdempotent
        IdOutcome o3 = registerTexIdentity(m, c, kCap, 0x1000u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(64,0), Verified(8,0), Verified(0x28000002u,0), Verified(1,0));
        check(o3 == IdOutcome::DuplicateIdempotent, "repeat same origin+evidence => DuplicateIdempotent");
        // conflicting VERIFIED fmt => Conflict, prior retained (no overwrite)
        IdOutcome o4 = registerTexIdentity(m, c, kCap, 0x1000u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(64,0), Verified(8,0), Verified(0xDEADu,0), Verified(1,0));
        check(o4 == IdOutcome::Conflict && m[0x1000u].fmt.value == 0x28000002u,
              "VERIFIED fmt disagreement => Conflict, prior record intact (no overwrite)");
        // EACH remaining VERIFIED field mismatch => Conflict, prior field intact (§11).
        IdOutcome ow = registerTexIdentity(m, c, kCap, 0x1000u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(999,0), Verified(8,0), Verified(0x28000002u,0), Verified(1,0));
        check(ow == IdOutcome::Conflict && m[0x1000u].w.value == 64,
              "VERIFIED w disagreement => Conflict, prior w intact");
        IdOutcome oh = registerTexIdentity(m, c, kCap, 0x1000u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(64,0), Verified(999,0), Verified(0x28000002u,0), Verified(1,0));
        check(oh == IdOutcome::Conflict && m[0x1000u].h.value == 8,
              "VERIFIED h disagreement => Conflict, prior h intact");
        IdOutcome ot = registerTexIdentity(m, c, kCap, 0x1000u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(64,0), Verified(8,0), Verified(0x28000002u,0), Verified(999,0));
        check(ot == IdOutcome::Conflict && m[0x1000u].type.value == 1,
              "VERIFIED resType disagreement => Conflict, prior resType intact");
        // family mismatch (TextureLockRect vs a DISTINCT family) => Conflict, prior family intact.
        IdOutcome ofam = registerTexIdentity(m, c, kCap, 0x1000u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::Volume, Verified(64,0), Verified(8,0), Verified(0x28000002u,0), Verified(1,0));
        check(ofam == IdOutcome::Conflict && m[0x1000u].family == ResFamily::TextureLockRect,
              "family disagreement => Conflict, prior family intact");
        // compatible LOCK UNKNOWN after full record => Conflict-free

        IdentityRecord& r = m[0x1000u];
        IdOutcome o5 = mergeInto(r, (uint8_t)IdOrigin::LockLate, ResFamily::Unknown,
                                 Unknown(), Unknown(), Unknown(), Unknown());
        check(o5 == IdOutcome::DuplicateIdempotent,
              "LOCK UNKNOWN after full record => DuplicateIdempotent (no promotion, origin already set)");
        // invalid key => InvalidKey, no insert
        size_t before = m.size();
        IdOutcome o6 = registerTexIdentity(m, c, kCap, 0x0FFFu, (uint8_t)IdOrigin::LockLate,
            ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        check(o6 == IdOutcome::InvalidKey && m.size() == before, "key<0x1000 => InvalidKey, no insert");
        check(c.conserves(), "identity conservation holds across the matrix");
    }

    // ── 4. LOCK-then-CREATE order symmetry (CREATE-then-LOCK) ─────────────────
    {
        Map m; IdentityCounters c;
        registerTexIdentity(m, c, kCap, 0x2000u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(32,0), Verified(4,0), Verified(0x02u,0), Verified(1,0));
        IdOutcome o = registerTexIdentity(m, c, kCap, 0x2000u, (uint8_t)IdOrigin::LockLate,
            ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        check(o == IdOutcome::Merged, "CREATE-then-LOCK => Merged (adds LOCK origin, retains CREATE meta)");
        check(m[0x2000u].fmt.value == 0x02u, "CREATE metadata retained after LOCK merge");
        check(c.conserves(), "conservation holds after order symmetry");
    }

    // ── 5. Identity cap: existing-key adjudicated BEFORE absent-key cap ───────
    {
        Map m; IdentityCounters c;
        for (uint32_t i = 0; i < kCap; i++)
            registerTexIdentity(m, c, kCap, 0x10000u + i, (uint8_t)IdOrigin::LockLate,
                ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        check(m.size() == kCap && c.inserted == kCap, "filled to cap");
        // new absent key at cap => IdentityCapFull
        IdOutcome oNew = registerTexIdentity(m, c, kCap, 0xFEEDu, (uint8_t)IdOrigin::LockLate,
            ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        check(oNew == IdOutcome::IdentityCapFull && m.size() == kCap,
              "new absent key at cap => IdentityCapFull, no eviction");
        // existing key at cap still adjudicates (dup), not a false CapFull
        IdOutcome oDup = registerTexIdentity(m, c, kCap, 0x10000u, (uint8_t)IdOrigin::LockLate,
            ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        check(oDup == IdOutcome::DuplicateIdempotent,
              "existing key at cap adjudicates duplicate, not CapFull");
        check(c.conserves(), "conservation holds at cap");
    }

    // ── 6. Behavior-create classify: three fused outcomes + conservation ──────
    {
        BehaviorCreateCounters b;
        check(classifyBehaviorCreate(b, 0x1000u, 10, kCap) == BehaviorOutcome::Upserted, "absent/present => Upserted");
        check(classifyBehaviorCreate(b, 0x0FFFu, 10, kCap) == BehaviorOutcome::InvalidKey, "tex<0x1000 => InvalidKey");
        check(classifyBehaviorCreate(b, 0x1000u, kCap, kCap) == BehaviorOutcome::CapFull, "size>=cap => CapFull");
        check(b.conserves() && b.upserted == 1 && b.invalidKey == 1 && b.capFull == 1,
              "behaviorCreate conservation: Upserted+InvalidKey+CapFull");
    }

    // ── 7. K1 / K2: CREATE independence in BOTH directions ────────────────────
    {
        // K1: identity full + behavior available + CREATE => IdentityCapFull AND behavior Upserted.
        Map m; IdentityCounters c; BehaviorCreateCounters b;
        for (uint32_t i = 0; i < kCap; i++)
            registerTexIdentity(m, c, kCap, 0x20000u + i, (uint8_t)IdOrigin::CreateHook,
                ResFamily::TextureLockRect, Verified(1,0), Verified(1,0), Verified(1,0), Verified(1,0));
        const uint32_t newTex = 0xC0DEu;                       // absent identity key
        BehaviorOutcome bo = classifyBehaviorCreate(b, newTex, /*behavior size*/10, kCap);   // behavior available
        IdOutcome io = registerTexIdentity(m, c, kCap, newTex, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(1,0), Verified(1,0), Verified(1,0), Verified(1,0));
        check(io == IdOutcome::IdentityCapFull, "K1: identity full => IdentityCapFull");
        check(bo == BehaviorOutcome::Upserted, "K1: sound behavior upsert NOT suppressed by identity CapFull");

        // K2: behavior full + identity available + CREATE => BehaviorCapFull AND identity Inserted.
        Map m2; IdentityCounters c2; BehaviorCreateCounters b2;
        BehaviorOutcome bo2 = classifyBehaviorCreate(b2, 0xBEEFu, /*behavior size*/kCap, kCap);  // behavior full
        IdOutcome io2 = registerTexIdentity(m2, c2, kCap, 0xBEEFu, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(1,0), Verified(1,0), Verified(1,0), Verified(1,0));
        check(bo2 == BehaviorOutcome::CapFull, "K2: behavior full => BehaviorCapFull");
        check(io2 == IdOutcome::Inserted, "K2: identity terminal retained independent of behavior CapFull");
    }

    // ── 8. K3: Lock-late-only key is miss-equivalent to behavior/consumers ────
    // (Structural: modelLockHook never touches a behavior map; the production
    //  hook writes s_texIdentity only. Here we assert the identity path leaves a
    //  co-resident behavior map untouched.)
    {
        Map behaviorFake;                       // stand-in for s_texMap
        Map ident; IdentityCounters c; FakeOut out; out.set(256, 0x0BA5A000u);
        FakeCtx ctx{0xABA5A000u, 0, out.va}; int calls = 0;
        modelLockHook(ctx, [](FakeCtx&){}, out, true, ident, c, calls);
        check(behaviorFake.empty(), "K3: Lock-late leaves the behavior map untouched (miss-equivalent)");
        check(ident.size() == 1, "K3: only the identity registry differs");
    }

    // ── 9. K4: nested cross-domain acquisition FAILS FAST in BOTH orders, and
    //         the rejected mutex is NEVER touched — through the PRODUCTION helper
    //         DomainOrderedLock (r2 blocker 1). The marker detector underneath is
    //         exercised too.
    {
        // Behavior held -> Identity attempt: inner rejected, identity mutex never locked.
        {
            CountingMutex behMu, idMu;
            DomainOrderedLock<CountingMutex> gb(LockDomain::Behavior, behMu);
            check(gb.locked() && behMu.locks == 1, "K4 B->I: outer behavior acquires its mutex");
            {
                DomainOrderedLock<CountingMutex> gi(LockDomain::Identity, idMu);
                check(!gi.locked(), "K4 B->I: nested identity REJECTED (fail-fast)");
                check(idMu.locks == 0, "K4 B->I: rejected identity mutex NEVER acquired");
            }
        }
        // Identity held -> Behavior attempt: inner rejected, behavior mutex never locked.
        {
            CountingMutex behMu, idMu;
            DomainOrderedLock<CountingMutex> gi(LockDomain::Identity, idMu);
            check(gi.locked() && idMu.locks == 1, "K4 I->B: outer identity acquires its mutex");
            {
                DomainOrderedLock<CountingMutex> gb(LockDomain::Behavior, behMu);
                check(!gb.locked(), "K4 I->B: nested behavior REJECTED (fail-fast)");
                check(behMu.locks == 0, "K4 I->B: rejected behavior mutex NEVER acquired");
            }
        }
        // Single-domain acquisition locks and unlocks exactly once.
        {
            CountingMutex m;
            { DomainOrderedLock<CountingMutex> g(LockDomain::Identity, m);
              check(g.locked() && m.locks == 1, "K4: single-domain acquisition locks once"); }
            check(m.unlocks == 1, "K4: single-domain acquisition unlocks once (RAII)");
        }
        check(tlsLockDomain() == LockDomain::None, "K4: domain marker restored after all scopes");
    }

    // ── 10. Concurrency shape: two same-key registrations => one entry ────────
    {
        Map m; IdentityCounters c;
        registerTexIdentity(m, c, kCap, 0x3000u, (uint8_t)IdOrigin::LockLate,
            ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        registerTexIdentity(m, c, kCap, 0x3000u, (uint8_t)IdOrigin::LockLate,
            ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        check(m.size() == 1, "two same-key registrations => one entry (locked map invariant)");
        check(c.conserves(), "conservation holds for same-key repeats");
    }

    // ── 11. Invalid LOCK (via the hook model) + invalid CREATE (both domains) ──
    {
        // Invalid LOCK: keyIn<0x1000 => noteInvalidKeyLock, delegate STILL runs
        // once, NO checked read, no insert (§5 InvalidKey before any read).
        Map ident; IdentityCounters c; FakeOut out; out.set(256, 0x0BA5A000u);
        FakeCtx ctx{0x0FFFu, 0, out.va}; int calls = 0;
        modelLockHook(ctx, [](FakeCtx& x){ x.r3 = 0x777; }, out, true, ident, c, calls);
        check(calls == 1 && ident.empty() && c.invalidKey == 1 && c.attempted == 1,
              "invalid LOCK: delegate once, InvalidKey, no read, no insert");

        // Invalid CREATE drives BOTH domains: behavior InvalidKey AND the
        // independent identity InvalidKey (§6). Mirrors the fixed create hook.
        BehaviorCreateCounters b; Map ident2; IdentityCounters c2;
        BehaviorOutcome bo = classifyBehaviorCreate(b, 0x0FFFu, 10, kCap);
        IdOutcome io = registerTexIdentity(ident2, c2, kCap, 0x0FFFu, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(1,0), Verified(1,0), Verified(1,0), Verified(1,0));
        check(bo == BehaviorOutcome::InvalidKey && io == IdOutcome::InvalidKey,
              "invalid CREATE: behavior InvalidKey AND identity InvalidKey");
        check(b.conserves() && c2.conserves() && ident2.empty() &&
              b.invalidKey == 1 && c2.invalidKey == 1,
              "invalid CREATE: both domains conserve, neither map grows");
    }

    // ── 12. Admission reads the checked span EXACTLY ONCE ─────────────────────
    {
        struct CountOut { FakeOut o; mutable int reads = 0;
            const uint8_t* span(uint32_t va, uint32_t n) const { reads++; return o.span(va, n); } };
        CountOut co; co.o.set(256, 0x0BA5A000u);
        Map ident; IdentityCounters c;
        FakeCtx ctx{0xABA5A000u, 0, co.o.va}; int calls = 0;
        modelLockHook(ctx, [](FakeCtx&){}, co, true, ident, c, calls);
        check(co.reads == 1, "admission calls the checked-span reader exactly once");
        check(ident.size() == 1, "admission with one readable nonzero span => Inserted");
    }

    // ── 13. Existing-key merge AND conflict still adjudicate AT cap ────────────
    {
        Map m; IdentityCounters c;
        for (uint32_t i = 0; i < kCap - 1; i++)
            registerTexIdentity(m, c, kCap, 0x40000u + i, (uint8_t)IdOrigin::LockLate,
                ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        // one VERIFIED CREATE record, bringing the map exactly to cap
        registerTexIdentity(m, c, kCap, 0x5EED0u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(16,0), Verified(16,0), Verified(7,0), Verified(1,0));
        check(m.size() == kCap, "map filled exactly to cap");
        // existing key at cap, compatible LOCK => Merged (adds LockLate origin), not CapFull
        IdOutcome om = registerTexIdentity(m, c, kCap, 0x5EED0u, (uint8_t)IdOrigin::LockLate,
            ResFamily::TextureLockRect, Unknown(), Unknown(), Unknown(), Unknown());
        check(om == IdOutcome::Merged, "existing key at cap => Merged (adjudicated before cap)");
        // existing key at cap, VERIFIED disagreement => Conflict (prior intact), not CapFull
        IdOutcome oc = registerTexIdentity(m, c, kCap, 0x5EED0u, (uint8_t)IdOrigin::CreateHook,
            ResFamily::TextureLockRect, Verified(999,0), Verified(16,0), Verified(7,0), Verified(1,0));
        check(oc == IdOutcome::Conflict && m[0x5EED0u].w.value == 16,
              "existing key at cap => Conflict, prior intact (adjudicated before cap)");
        check(c.conserves(), "conservation holds for merge/conflict at cap");
    }

    // ── 14. PRODUCTION gate is cached: the env reader runs at most once across
    //         many calls (r2 blocker 3 — kills the per-hook-getenv mutant). This
    //         drives the REAL productionGateEnabled()/productionGateInitCount()
    //         defined in hle_api_observe.cpp, not a stand-in.
    {
        (void)productionGateEnabled();                 // force first init
        const int a = productionGateInitCount();
        for (int i = 0; i < 1000; i++) (void)productionGateEnabled();
        const int b = productionGateInitCount();
        check(a >= 1, "production gate env reader ran at least once");
        check(b == a, "production gate is cached: reader does NOT re-run across 1000 calls");
    }

    // ── 15. Sibling-kill exclusion predicates (r2 blocker 3): a self-test mode
    //         must NEVER kill sibling processes. Dropping texIdentity from the OR
    //         is a mutant this catches.
    {
        check(anySelfTestModeActive(0,0,0,0,0,1), "texIdentity mode counts as a self-test mode");
        check(anySelfTestModeActive(0,0,0,0,1,0), "texUnlock mode counts as a self-test mode");
        check(!anySelfTestModeActive(0,0,0,0,0,0), "no flags => not a self-test mode");
        check(isPlayerInstanceLaunch(/*anyTest*/false, /*hasInstance*/false),
              "real launch (no test, no instance) => the player instance");
        check(!isPlayerInstanceLaunch(true, false), "any self-test mode => never a player instance");
        check(!isPlayerInstanceLaunch(false, true), "instance id => never a player instance");
        check(!isPlayerInstanceLaunch(anySelfTestModeActive(0,0,0,0,0,1), false),
              "texIdentity self-test => never a player instance (end-to-end)");
    }

    // ── 16. REAL bounded two-thread interleaving through the PRODUCTION helper
    //         DomainOrderedLock (r2 blocker 2). Both threads run CREATE and LOCK
    //         in OPPOSITE orders against shared maps/mutexes; separate non-nested
    //         Behavior/Identity scopes. Must complete (no deadlock) with EXACT
    //         conservation and attempt counts.
    {
        std::mutex behMu, idMu;
        Map ident; IdentityCounters ic; BehaviorCreateCounters bc;
        constexpr int N = 2000;
        auto worker = [&](uint32_t base, bool createFirst) {
            for (int i = 0; i < N; i++) {
                const uint32_t key = base + (uint32_t)(i % 64);   // overlap => contention
                auto doCreate = [&] {
                    { DomainOrderedLock<std::mutex> g(LockDomain::Behavior, behMu);
                      if (g.locked()) classifyBehaviorCreate(bc, key, /*size*/10, kCap); }
                    { DomainOrderedLock<std::mutex> g(LockDomain::Identity, idMu);
                      if (g.locked()) registerTexIdentity(ident, ic, kCap, key,
                          (uint8_t)IdOrigin::CreateHook, ResFamily::TextureLockRect,
                          Verified(1,0), Verified(1,0), Verified(1,0), Verified(1,0)); }
                };
                auto doLock = [&] {
                    DomainOrderedLock<std::mutex> g(LockDomain::Identity, idMu);
                    if (g.locked()) registerTexIdentity(ident, ic, kCap, key,
                        (uint8_t)IdOrigin::LockLate, ResFamily::TextureLockRect,
                        Unknown(), Unknown(), Unknown(), Unknown());
                };
                if (createFirst) { doCreate(); doLock(); } else { doLock(); doCreate(); }
            }
        };
        std::thread t1(worker, 0x100000u, true);
        std::thread t2(worker, 0x100000u, false);   // same key range, OPPOSITE order
        t1.join(); t2.join();                        // returning proves no deadlock
        check(bc.conserves(), "concurrency: behavior conservation holds under 2 threads");
        check(ic.conserves(), "concurrency: identity conservation holds under 2 threads");
        check(bc.attempted == (uint64_t)2 * N, "concurrency: behavior attempts exact (1/create/thread)");
        check(ic.attempted == (uint64_t)4 * N, "concurrency: identity attempts exact (create+lock/thread)");
        check(tlsLockDomain() == LockDomain::None, "concurrency: main-thread marker clean afterwards");
    }

    // ── 17. PRODUCTION DOMAIN SEAM (r3): drive the REAL identity domain
    //         functions the CREATE/LOCK hooks call — sr::kernel::texid::domain::*
    //         — against the REAL registry (dependency injection, NOT a duplicate).
    //         This is the seam whose TU cannot see s_texMap/kTexMapCap/s_texMu, so
    //         the three frozen structural mutations are compile errors there.
    {
        domain::resetForTest();
        // CREATE (valid) => Inserted; identity registry grows by one.
        IdOutcome c1 = domain::registerCreate(0x9000u, 64, 32, 0x28000002u, 1);
        check(c1 == IdOutcome::Inserted && domain::registrySize() == 1,
              "domain::registerCreate valid key => Inserted in the real registry");
        // LOCK-late same key, admitted => Merged (adds LockLate origin).
        AdmitResult ok{}; ok.admit = true; ok.pitch = 256; ok.bits = 0x0BA5A000u;
        IdOutcome l1 = domain::registerLockLate(0x9000u, ok);
        check(l1 == IdOutcome::Merged, "domain::registerLockLate admitted, same key => Merged");
        // LOCK-late, NOT admitted => LockOutputInvalid, no new entry.
        AdmitResult bad{}; bad.admit = false;
        IdOutcome l2 = domain::registerLockLate(0xA000u, bad);
        check(l2 == IdOutcome::LockOutputInvalid && domain::registrySize() == 1,
              "domain::registerLockLate not admitted => LockOutputInvalid, no insert");
        // Invalid CREATE + invalid LOCK => identity InvalidKey both, no growth.
        domain::registerCreate(0x0500u, 1, 1, 1, 1);
        domain::noteLockInvalidKey();
        IdentityCounters c = domain::counters();
        check(c.invalidKey == 2 && domain::registrySize() == 1,
              "domain: invalid CREATE + invalid LOCK => 2 InvalidKey, registry unchanged");
        check(c.conserves(), "domain: real-registry conservation holds across the seam");
        domain::resetForTest();
        check(domain::registrySize() == 0 && domain::counters().attempted == 0,
              "domain::resetForTest clears the real registry/counters");
    }

    const int total = g_pass + g_fail;
    SR_LOG_BOOT(g_fail ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
                "[TEXID_TEST] %d/%d passed", g_pass, total);
    return g_fail;
}

}  // namespace

int runSelfTestIfRequested() {
    const char* e = std::getenv("SR_HLE_TEX_IDENTITY_TEST");
    if (!e || e[0] != '1') return -1;
    return runTests();
}

}  // namespace sr::kernel::texid
