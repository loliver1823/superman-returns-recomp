// iter-455l [WFFSM] r2 — the WorldFlow-FSM load-completion observer, reworked
// to the eight-defect verdict.
//   - Destination enter-script chain CORRECTED: state+0x10 -> object+0
//     (VTABLE) -> vtable+0x0C -> descriptor+4 (the generated state runner's
//     actual indirection; r1 omitted the vtable hop and the fixture
//     reproduced the mistake — wffsm_novtbl mutants that omission).
//   - EDGE records carry the FULL identity: correlation id, fsm, source and
//     destination state (idx + VA), predicate ref (txn+12 -> obj+12 ->
//     holder+4), destination enter-script ref (corrected chain), module
//     identity — each chain value with a tri-state validity (P present /
//     M missing / U unknown-unreadable); dedup keys on (fsm, txn).
//   - STATE dedup is PER-FSM (bounded table, explicit overflow reporting).
//   - EDGE tracker overflow is honest: untracked transactions past the table
//     announce once and count, never flood.
//   - READY-AGG is filtered to the AreWorldFlowAssetsLoaded callsite
//     (lr 0x8221898C) with an active condition context; READY-ONE to the
//     direct call at lr 0x821F7C70; both share the EDGE's correlation id and
//     each entry carries its class index and validity tri-state.
//   - ARM is once-only, serialized under the core mutex; armed=true publishes
//     only AFTER the ARM line is emitted.
//   - All four wrappers run injectable BODIES (templates below) that the
//     self-test drives with fake contexts and delegates: pre -> delegate ->
//     post exactly once, full-width registers, delegate outputs preserved,
//     TLS correlation and cleanup, LR filtering, no mutex across delegation.
// Gate: SR_WFFSM_OBS=1 (default OFF). Self-test: SR_WFFSM_OBS_TEST=1.

#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sr_oracle.hpp"

namespace sr::wffsm {

struct Sink {
    void (*emit)(void* ud, const char* line);
    void* ud;
};

// Independent per-class caps.
inline constexpr uint32_t kStateCap = 256;
inline constexpr uint32_t kEdgeCap  = 512;
inline constexpr uint32_t kAggCap   = 128;
inline constexpr uint32_t kOneCap   = 256;
inline constexpr uint32_t kEdgeTrack = 128;  // distinct (fsm,txn) tracked
inline constexpr uint32_t kFsmTrack  = 16;   // distinct fsms tracked (STATE)

// Tri-state validity for observed guest values (verdict defect 6): a failed
// READ must never masquerade as guest absence.
enum class Va1 : uint8_t { Unknown = 0, Missing = 1, Present = 2 };
inline char vaChar(Va1 v) {
    return v == Va1::Present ? 'P' : (v == Va1::Missing ? 'M' : 'U');
}

// One condition-evaluation record (built by condPre, completed by condPost).
struct EdgeEval {
    uint32_t corrId = 0;
    uint32_t fsm = 0;
    bool haveCtx = false;
    uint32_t srcIdx = 0xFFFFFFFFu;
    uint32_t srcStateVA = 0; Va1 srcStateV = Va1::Unknown;
    uint32_t txn = 0;
    uint32_t destIdx = 0; Va1 destIdxV = Va1::Unknown;
    uint32_t destStateVA = 0; Va1 destStateV = Va1::Unknown;
    uint32_t predRef = 0; Va1 predRefV = Va1::Unknown;
    uint32_t destRef = 0; Va1 destRefV = Va1::Unknown;
    uint32_t mod = 0; Va1 modV = Va1::Unknown;
};

class Core {
public:
    explicit Core(Sink sink) : m_sink(sink) {}

    // ARM: once-only, serialized; armed publishes AFTER the line is emitted
    // (verdict defect 7). The caller does NOT hold m_armMx.
    void onAptRequest(const char* path);
    void onAptRequestUnreadable(uint32_t va);
    bool armed() const { return m_armed.load(std::memory_order_acquire); }

    // STATE: per-FSM change dedup with explicit tracker overflow.
    void onWalkerState(uint32_t fsm, uint32_t idx, uint32_t stateVA,
                       uint32_t txnCount, uint64_t nowMs);

    // EDGE: dedup on (fsm,txn); first-sighting + result-change + totals;
    // honest tracker overflow (no flood).
    void onCondEval(const EdgeEval& e, bool result, uint64_t nowMs);

    // READY-AGG: correlated, entries carry class index + tri-state.
    void onSlotsAgg(uint32_t corrId, uint32_t mgr, uint32_t groupId,
                    const uint32_t entries[9], const Va1 entryV[9],
                    bool result, uint64_t nowMs);

    // READY-ONE: correlated actual return with its class index.
    void onSlotOne(uint32_t corrId, int classIdx, uint32_t entry, bool ready,
                   uint64_t nowMs);

    uint32_t stateEmits() const { return m_stateEmits; }
    uint32_t edgeEmits() const { return m_edgeEmits; }

private:
    bool capped(uint32_t& counter, uint32_t cap, const char* cls);
    void emit(const char* fmt, ...);

    Sink m_sink;
    std::atomic<bool> m_armed{false};
    bool m_armEmitted = false;
    bool m_unreadableAnnounced = false;
    // STATE: bounded per-FSM last-idx table.
    uint32_t m_fsmKey[kFsmTrack] = {};
    uint32_t m_fsmLast[kFsmTrack] = {};
    uint32_t m_fsmN = 0;
    bool m_fsmOverflowAnnounced = false;
    uint32_t m_fsmUntracked = 0;
    uint32_t m_stateEmits = 0, m_stateTotal = 0;
    // EDGE: bounded (fsm,txn) tracker.
    uint32_t m_trackFsm[kEdgeTrack] = {};
    uint32_t m_trackTxn[kEdgeTrack] = {};
    uint8_t m_trackRes[kEdgeTrack] = {};
    uint32_t m_trackN = 0;
    bool m_edgeOverflowAnnounced = false;
    uint32_t m_edgeUntracked = 0;
    uint32_t m_edgeEmits = 0, m_edgeTotal = 0, m_edgeTrue = 0;
    // READY
    uint32_t m_aggEmits = 0, m_aggTotal = 0;
    uint32_t m_oneEmits = 0, m_oneTotal = 0, m_oneFalse = 0;
};

#if SR_ORACLE
bool enabled();                       // SR_WFFSM_OBS=1, default OFF
bool active();                        // enabled() && core().armed()
Core& core();
uint64_t nowMs();

void onAptRequestVA(uint32_t pathVA, uint8_t* base);

// TLS context queries (production internals, exposed for fixtures).
struct WalkCtx { uint32_t fsm; uint32_t idx; };
bool topWalk(WalkCtx* out);
bool condActive();                    // a condition pre is pending on this thread
bool inReady();                       // inside the outer readiness call
uint32_t walkDepth();
uint32_t condDepth();
uint32_t topCondCorr();

// Production pre/post halves (cpp). The templated BODIES below are the only
// production callers; the self-test drives the bodies, not the halves.
void walkerPreProd(uint32_t fsm, uint8_t* base, Core& c);
void walkerPostProd();
void condPreProd(uint32_t txn, uint8_t* base);
void condPostProd(Core& c, uint32_t retR3, uint64_t nowMs);
void readyPreProd(uint32_t mgr, uint32_t gid, uint8_t* base);
void readyPostProd(Core& c, uint32_t retR3, uint64_t nowMs);
void slotOneProd(Core& c, uint32_t entry, uint32_t retR3, uint64_t nowMs);
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): wffsm_obs.cpp is compiled out (CMakeLists.txt ORACLE_SOURCES), and with it the
// two observe-only overrides of sub_821F7C38 / sub_82167808 (docs/OVERRIDES.json: pure delegation while the observer
// is inactive), so the recompiled bodies run. The wrappers in apt_as_watch.cpp / apt_parse_trace.cpp / fe_flow_watch.cpp
// see what they see with SR_WFFSM_OBS unset: never enabled, never armed, one direct delegation. core() is reached
// only when enabled() / active() is true, i.e. never.
inline bool enabled() { return false; }
inline bool active() { return false; }
inline Core& core() {
    static Core c(Sink{nullptr, nullptr});
    return c;
}
inline void onAptRequestVA(uint32_t, uint8_t*) {}
#endif

// The AreWorldFlowAssetsLoaded callsite of sub_821F7C38 and the direct
// sub_82167808 callsite inside sub_821F7C38 (verdict defect 5).
inline constexpr uint32_t kAwfalCallLR = 0x8221898Cu;
inline constexpr uint32_t kSlotCallLR  = 0x821F7C70u;

#if SR_ORACLE
// ── injectable wrapper bodies (verdict defect 8) — production wrappers AND
//    the self-test run THESE. One delegation on every path; no lock is held
//    across any delegate call (the halves lock only around Core calls). ─────
template <typename Ctx, typename D>
void walkerBody(Ctx& ctx, uint8_t* base, D delegate, Core& c) {
    walkerPreProd(ctx.r3.u32, base, c);
    delegate(ctx, base);
    walkerPostProd();
}
// ── iter-455m pre-arm dispatch (director rework after the gate-off control):
//    the walker wrap engages ONLY when the observer is ACTIVE (gate on AND
//    the core armed). Enabled-but-unarmed and gate-off both delegate
//    DIRECTLY: exactly once, no walkerPreProd, no guest read, no TLS
//    push/pop, no WFFSM record. The always-on enabled() wrap did hot-path
//    work on every sub_8278C448 call while producing no evidence — an
//    instrument-design defect elevated as a perturbation candidate by the
//    six-miss/control experiment (ledger iter-455m-control1). The decision
//    is sampled ONCE per call: an arm landing mid-call takes effect on the
//    NEXT walker call. This dispatch is the ONE production decision body;
//    the self-test drives it with both flag values and both arm states.
// The core accessor is LAZY (r2 blocker 1): gate-off delegates directly
// WITHOUT invoking it - the old gate-off path never constructed the
// singleton, and the first-call heap allocation is exactly the perturbation
// class this dispatch removes. Enabled invokes the accessor EXACTLY once;
// the wrap engages only when the returned core is armed.
template <typename Ctx, typename D, typename CoreFn>
void walkerDispatch(Ctx& ctx, uint8_t* base, D delegate,
                    bool gateEnabled, CoreFn coreFn) {
    if (!gateEnabled) {
        delegate(ctx, base);
        return;
    }
    Core& c = coreFn();
    if (c.armed()) {
        walkerBody(ctx, base, delegate, c);
    } else {
        delegate(ctx, base);
    }
}
template <typename Ctx, typename D>
void condBody(Ctx& ctx, uint8_t* base, D delegate, Core& c) {
    condPreProd(ctx.r3.u32, base);
    delegate(ctx, base);
    condPostProd(c, ctx.r3.u32, nowMs());
}
template <typename Ctx, typename D>
void readyBody(Ctx& ctx, uint8_t* base, D delegate, Core& c) {
    const uint32_t lr = (uint32_t)ctx.lr;
    if (lr != kAwfalCallLR || !condActive()) {   // LR + context filter
        delegate(ctx, base);
        return;
    }
    readyPreProd(ctx.r3.u32, ctx.r4.u32, base);
    delegate(ctx, base);
    readyPostProd(c, ctx.r3.u32, nowMs());
}
template <typename Ctx, typename D>
void slotBody(Ctx& ctx, uint8_t* base, D delegate, Core& c) {
    const uint32_t lr = (uint32_t)ctx.lr;
    if (lr != kSlotCallLR || !inReady()) {       // direct-call LR filter
        delegate(ctx, base);
        return;
    }
    const uint32_t entry = ctx.r3.u32;
    delegate(ctx, base);
    slotOneProd(c, entry, ctx.r3.u32, nowMs());
}

int runSelfTestIfRequested();
#else
// SR_ORACLE=0: the gate-off dispatch - exactly one direct delegation, no guest read, no TLS, no record.
template <typename Ctx, typename D, typename CoreFn>
void walkerDispatch(Ctx& ctx, uint8_t* base, D delegate, bool, CoreFn) {
    delegate(ctx, base);
}
template <typename Ctx, typename D>
void condBody(Ctx& ctx, uint8_t* base, D delegate, Core&) {
    delegate(ctx, base);
}
#endif

}  // namespace sr::wffsm
