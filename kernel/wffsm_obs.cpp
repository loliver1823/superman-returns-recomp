// iter-455l [WFFSM] r2 production halves + self-test. Contract: wffsm_obs.hpp.
//
// The four wrapper BODIES live in the hpp (templates) — production wrappers
// (apt_as_watch.cpp, apt_parse_trace.cpp, and the two overrides below) and the
// self-test both run them. The halves here do the checked reads and Core
// calls; no lock is ever held across a delegation.
//
// READS: rd32 translates BOTH endpoints through aliasGuestVA with contiguity
// required; rd8 is a single independently-checked byte. Every observed value
// carries a tri-state validity (P/M/U) — a failed READ is 'U', never 'M'
// (verdict defect 6: observer unreadability must not impersonate guest
// absence).

#include "wffsm_obs.hpp"
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sr::wffsm {

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("SR_WFFSM_OBS");
        return e && e[0] == '1';                    // default OFF
    }();
    return on;
}

static void logSink(void*, const char* line) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "%s", line);
}

Core& core() {
    static Core* c = new Core(Sink{&logSink, nullptr});
    return *c;
}

static std::mutex& coreMutex() {
    static std::mutex m;
    return m;
}

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

bool active() { return enabled() && core().armed(); }

// ── Core methods ────────────────────────────────────────────────────────────

bool Core::capped(uint32_t& counter, uint32_t cap, const char* cls) {
    if (counter < cap) { counter++; return false; }
    if (counter == cap) {
        counter++;
        emit("[WFFSM] %s SUPPRESSED past cap=%u (announced once; totals continue)",
             cls, cap);
    }
    return true;
}

void Core::emit(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (m_sink.emit) m_sink.emit(m_sink.ud, line);
}

// ARM: the adapter holds the core mutex around this call (serialization);
// the line emits BEFORE armed=true publishes (verdict defect 7).
void Core::onAptRequest(const char* path) {
    if (!path || std::strcmp(path, "screens/hud/main_hud") != 0) return;
    if (m_armEmitted) return;                    // once-only
    m_armEmitted = true;
    emit("[WFFSM] ARM on screens/hud/main_hud");
    m_armed.store(true, std::memory_order_release);
}

void Core::onAptRequestUnreadable(uint32_t va) {
    if (m_unreadableAnnounced) return;
    m_unreadableAnnounced = true;
    emit("[WFFSM] ARM-REQ UNREADABLE va=%08X (announced once; not armed)", va);
}

// STATE: per-FSM change dedup (verdict defect 3) with explicit overflow.
void Core::onWalkerState(uint32_t fsm, uint32_t idx, uint32_t stateVA,
                         uint32_t txnCount, uint64_t nowMs) {
    m_stateTotal++;
    int slot = -1;
    for (uint32_t i = 0; i < m_fsmN; i++)
        if (m_fsmKey[i] == fsm) { slot = (int)i; break; }
    if (slot >= 0) {
        if (m_fsmLast[slot] == idx) return;      // stable: no emit
        m_fsmLast[slot] = idx;
    } else if (m_fsmN < kFsmTrack) {
        slot = (int)m_fsmN++;
        m_fsmKey[slot] = fsm;
        m_fsmLast[slot] = idx;
    } else {
        m_fsmUntracked++;
        if (!m_fsmOverflowAnnounced) {
            m_fsmOverflowAnnounced = true;
            emit("[WFFSM] STATE-TRACKER OVERFLOW past %u fsms (announced once; "
                 "untracked fsms counted, not emitted)", kFsmTrack);
        }
        return;
    }
    if (capped(m_stateEmits, kStateCap, "STATE")) return;
    emit("[WFFSM] STATE fsm=%08X idx=%u state=%08X txns=%u t=%llu total=%u",
         fsm, idx, stateVA, txnCount, (unsigned long long)nowMs, m_stateTotal);
}

// EDGE: (fsm,txn)-keyed dedup (verdict defect 2) with honest tracker overflow
// (verdict defect 4).
void Core::onCondEval(const EdgeEval& e, bool result, uint64_t nowMs) {
    m_edgeTotal++;
    if (result) m_edgeTrue++;
    int slot = -1;
    for (uint32_t i = 0; i < m_trackN; i++)
        if (m_trackFsm[i] == e.fsm && m_trackTxn[i] == e.txn) { slot = (int)i; break; }
    bool fresh = false;
    if (slot < 0) {
        if (m_trackN < kEdgeTrack) {
            slot = (int)m_trackN++;
            m_trackFsm[slot] = e.fsm;
            m_trackTxn[slot] = e.txn;
            m_trackRes[slot] = 2;
            fresh = true;
        } else {
            m_edgeUntracked++;
            if (!m_edgeOverflowAnnounced) {
                m_edgeOverflowAnnounced = true;
                emit("[WFFSM] EDGE-TRACKER OVERFLOW past %u identities (announced "
                     "once; untracked evals counted, not emitted)", kEdgeTrack);
            }
            return;                              // never 'fresh forever'
        }
    }
    const uint8_t r = result ? 1 : 0;
    const bool changed = !fresh && m_trackRes[slot] != 2 && m_trackRes[slot] != r;
    m_trackRes[slot] = r;
    if (!fresh && !changed) return;
    if (capped(m_edgeEmits, kEdgeCap, "EDGE")) return;
    emit("[WFFSM] EDGE%s%s corr=%u fsm=%08X ctx=%u src=%u srcState=%08X(%c) "
         "txn=%08X dest=%u(%c) destState=%08X(%c) pred=%08X(%c) "
         "destScript=%08X(%c) mod=%08X(%c) result=%u t=%llu totals(evals=%u true=%u)",
         fresh ? " FIRST" : "", changed ? " CHANGE" : "",
         e.corrId, e.fsm, e.haveCtx ? 1u : 0u, e.srcIdx,
         e.srcStateVA, vaChar(e.srcStateV),
         e.txn, e.destIdx, vaChar(e.destIdxV),
         e.destStateVA, vaChar(e.destStateV),
         e.predRef, vaChar(e.predRefV),
         e.destRef, vaChar(e.destRefV),
         e.mod, vaChar(e.modV),
         (unsigned)r, (unsigned long long)nowMs, m_edgeTotal, m_edgeTrue);
}

void Core::onSlotsAgg(uint32_t corrId, uint32_t mgr, uint32_t groupId,
                      const uint32_t entries[9], const Va1 entryV[9],
                      bool result, uint64_t nowMs) {
    m_aggTotal++;
    if (capped(m_aggEmits, kAggCap, "READY-AGG")) return;
    char ebuf[300];
    int off = 0;
    for (int i = 0; i < 9; i++) {
        int w = std::snprintf(ebuf + off, sizeof(ebuf) - off, "%d:%08X(%c) ",
                              i, entries[i], vaChar(entryV[i]));
        if (w <= 0 || off + w >= (int)sizeof(ebuf) - 1) break;
        off += w;
    }
    emit("[WFFSM] READY-AGG corr=%u mgr=%08X group=%u result=%u class0=%c "
         "entries=[%s] t=%llu total=%u",
         corrId, mgr, groupId, result ? 1u : 0u, vaChar(entryV[0]), ebuf,
         (unsigned long long)nowMs, m_aggTotal);
}

void Core::onSlotOne(uint32_t corrId, int classIdx, uint32_t entry, bool ready,
                     uint64_t nowMs) {
    m_oneTotal++;
    if (!ready) m_oneFalse++;
    if (capped(m_oneEmits, kOneCap, "READY-ONE")) return;
    emit("[WFFSM] READY-ONE corr=%u class=%d entry=%08X ready=%u t=%llu "
         "totals(calls=%u false=%u)",
         corrId, classIdx, entry, ready ? 1u : 0u, (unsigned long long)nowMs,
         m_oneTotal, m_oneFalse);
}

// ── checked guest reads + tri-state helper ──────────────────────────────────
static bool rd32(uint8_t* base, uint64_t va64, uint32_t* out) {
    if (!va64 || (va64 & 3u) || va64 + 3ull > 0xFFFFFFFFull) return false;
    const uint32_t va = (uint32_t)va64;
    if (!sr::kernel::isGuestVAInRange(va) ||
        !sr::kernel::isGuestVAInRange(va + 3u))
        return false;
    const uint32_t a0 = sr::kernel::aliasGuestVA(va);
    const uint32_t a1 = sr::kernel::aliasGuestVA(va + 3u);
    if (a1 != a0 + 3u) return false;
    const uint8_t* p = base + a0;
    if (!sr::kernel::hostRangeReadable(p, 4)) return false;
    *out = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           (uint32_t)p[3];
    return true;
}
static bool rd8(uint8_t* base, uint64_t va64, uint8_t* out) {
    if (!va64 || va64 > 0xFFFFFFFFull) return false;
    const uint32_t va = (uint32_t)va64;
    if (!sr::kernel::isGuestVAInRange(va)) return false;
    const uint8_t* p = base + sr::kernel::aliasGuestVA(va);
    if (!sr::kernel::hostRangeReadable(p, 1)) return false;
    *out = *p;
    return true;
}
static bool readGuestPath(uint8_t* base, uint32_t va, char* out, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        uint8_t b = 0;
        if (!rd8(base, (uint64_t)va + i, &b)) return false;
        out[i] = (char)b;
        if (b == 0) return true;
    }
    return false;
}
struct TriVal { uint32_t v = 0; Va1 s = Va1::Unknown; };
static TriVal rdTri(uint8_t* base, uint64_t va64) {
    TriVal t;
    uint32_t v = 0;
    if (!rd32(base, va64, &v)) { t.s = Va1::Unknown; return t; }
    t.v = v;
    t.s = v ? Va1::Present : Va1::Missing;
    return t;
}
// One chain step: only a Present link is followed; U/M propagate (defect 6).
static TriVal chain(uint8_t* base, const TriVal& prev, uint32_t off) {
    if (prev.s != Va1::Present) { TriVal t; t.s = prev.s; return t; }
    return rdTri(base, (uint64_t)prev.v + off);
}

// ARM path: the WHOLE transition is serialized under the mutex (defect 7).
void armReqBody(uint8_t* base, uint32_t pathVA, Core& c, std::mutex& mx) {
    char path[96];
    if (readGuestPath(base, pathVA, path, sizeof(path))) {
        std::lock_guard<std::mutex> lk(mx);
        c.onAptRequest(path);
    } else {
        std::lock_guard<std::mutex> lk(mx);
        c.onAptRequestUnreadable(pathVA);
    }
}

void onAptRequestVA(uint32_t pathVA, uint8_t* base) {
    if (!enabled()) return;
    armReqBody(base, pathVA, core(), coreMutex());
}

// ── nesting-safe TLS ────────────────────────────────────────────────────────
namespace {
constexpr uint32_t kDepth = 8;
thread_local WalkCtx t_walk[kDepth];
thread_local uint32_t t_walkN = 0;
thread_local uint32_t t_readyN = 0;
struct CondPending { EdgeEval e; };
thread_local CondPending t_cond[kDepth];
thread_local uint32_t t_condN = 0;
struct AggPending {
    uint32_t corrId = 0, mgr = 0, gid = 0;
    uint32_t entries[9] = {};
    Va1 entryV[9] = {};
    uint32_t cursor = 0;   // next class the ORDERED generated loop can call
};
thread_local AggPending t_agg[kDepth];
thread_local uint32_t t_aggN = 0;
std::atomic<uint32_t> s_corr{0};
}  // namespace

static void pushWalk(uint32_t fsm, uint32_t idx) {
    if (t_walkN < kDepth) t_walk[t_walkN] = WalkCtx{fsm, idx};
    t_walkN++;
}
static void popWalk() {
    if (t_walkN) t_walkN--;
}
bool topWalk(WalkCtx* out) {
    if (!t_walkN || t_walkN > kDepth) return false;
    *out = t_walk[t_walkN - 1];
    return true;
}
bool condActive() { return t_condN != 0; }
bool inReady() { return t_readyN != 0; }
uint32_t walkDepth() { return t_walkN; }
uint32_t condDepth() { return t_condN; }

// exposed for the self-test's correlation assertion
uint32_t topCondCorr() {
    return (t_condN && t_condN <= kDepth) ? t_cond[t_condN - 1].e.corrId : 0;
}

// ── production halves ───────────────────────────────────────────────────────

void walkerPreProd(uint32_t fsm, uint8_t* base, Core& c) {
    uint32_t idx = 0xFFFFFFFFu;
    (void)rd32(base, (uint64_t)fsm + 8u, &idx);
    if (c.armed()) {
        uint32_t cnt = 0, arr = 0, stateVA = 0, txnCount = 0;
        if (rd32(base, (uint64_t)fsm + 0x18u, &cnt) &&
            rd32(base, (uint64_t)fsm + 0x1Cu, &arr) && idx < cnt)
            (void)rd32(base, (uint64_t)arr + 4ull * idx, &stateVA);
        if (stateVA) (void)rd32(base, (uint64_t)stateVA + 8u, &txnCount);
        std::lock_guard<std::mutex> lk(coreMutex());
        c.onWalkerState(fsm, idx, stateVA, txnCount, nowMs());
    }
    pushWalk(fsm, idx);
}
void walkerPostProd() { popWalk(); }

void condPreProd(uint32_t txn, uint8_t* base) {
    CondPending p{};
    EdgeEval& e = p.e;
    e.txn = txn;
    e.corrId = s_corr.fetch_add(1, std::memory_order_relaxed) + 1;
    WalkCtx w{};
    e.haveCtx = topWalk(&w);
    uint32_t cnt = 0, arr = 0;
    bool fsmOk = false;
    if (e.haveCtx) {
        e.fsm = w.fsm;                            // kept for identity + dedup
        e.srcIdx = w.idx;
        fsmOk = rd32(base, (uint64_t)w.fsm + 0x18u, &cnt) &&
                rd32(base, (uint64_t)w.fsm + 0x1Cu, &arr);
        if (fsmOk && e.srcIdx < cnt) {
            TriVal s = rdTri(base, (uint64_t)arr + 4ull * e.srcIdx);
            e.srcStateVA = s.v; e.srcStateV = s.s;
        }
    }
    // txn+4 is a NUMERIC state index - 0 is a VALID destination (r2 verdict
    // defect 1): a scalar gets readable/unknown semantics, never presence.
    uint32_t destIdxRaw = 0;
    const bool destIdxOk = rd32(base, (uint64_t)txn + 4u, &destIdxRaw);
    e.destIdx = destIdxRaw;
    e.destIdxV = destIdxOk ? Va1::Present : Va1::Unknown;
    if (fsmOk && destIdxOk && destIdxRaw < cnt) {
        TriVal ds = rdTri(base, (uint64_t)arr + 4ull * destIdxRaw);
        e.destStateVA = ds.v; e.destStateV = ds.s;
        // destination enter-script chain (verdict defect 1 — the VTABLE hop):
        // state+0x10 -> object; object+0 = vtable; vtable+0x0C = descriptor;
        // descriptor+4 = ref.
        TriVal obj = chain(base, ds, 0x10u);
        TriVal vtbl = chain(base, obj, 0x0u);
        TriVal desc = chain(base, vtbl, 0x0Cu);
        TriVal ref = chain(base, desc, 0x4u);
        e.destRef = ref.v; e.destRefV = ref.s;
    }
    // predicate chain: txn+12 -> object+12 -> holder+4.
    {
        TriVal t0; t0.v = txn; t0.s = txn ? Va1::Present : Va1::Missing;
        TriVal o1 = chain(base, t0, 12u);
        TriVal o2 = chain(base, o1, 12u);
        TriVal pr = chain(base, o2, 4u);
        e.predRef = pr.v; e.predRefV = pr.s;
    }
    {
        TriVal m = rdTri(base, 0x8296D8CCu);
        e.mod = m.v; e.modV = m.s;
    }
    if (t_condN < kDepth) t_cond[t_condN] = p;
    t_condN++;
}

void condPostProd(Core& c, uint32_t retR3, uint64_t t) {
    if (!t_condN) return;
    t_condN--;
    if (t_condN >= kDepth) return;
    const CondPending& p = t_cond[t_condN];
    const bool result = retR3 != 0;              // the REAL post-delegate boolean
    std::lock_guard<std::mutex> lk(coreMutex());
    c.onCondEval(p.e, result, t);
}

void readyPreProd(uint32_t mgr, uint32_t gid, uint8_t* base) {
    AggPending a{};
    a.mgr = mgr;
    a.gid = gid;
    a.corrId = topCondCorr();                    // correlation (defect 5)
    for (uint32_t i = 0; i < 9; i++) {
        TriVal tbl = rdTri(base, (uint64_t)mgr + 4ull * (i + 55));
        TriVal entry = chain(base, tbl, 4u * gid);
        a.entries[i] = entry.v;
        a.entryV[i] = entry.s;                   // U propagates, never fakes M
    }
    if (t_aggN < kDepth) t_agg[t_aggN] = a;
    t_aggN++;
    t_readyN++;
}

void readyPostProd(Core& c, uint32_t retR3, uint64_t t) {
    if (t_readyN) t_readyN--;
    if (!t_aggN) return;
    t_aggN--;
    if (t_aggN >= kDepth) return;
    const AggPending& a = t_agg[t_aggN];
    std::lock_guard<std::mutex> lk(coreMutex());
    c.onSlotsAgg(a.corrId, a.mgr, a.gid, a.entries, a.entryV, retR3 != 0, t);
}

void slotOneProd(Core& c, uint32_t entry, uint32_t retR3, uint64_t t) {
    uint32_t corr = 0;
    int classIdx = -1;
    if (t_aggN && t_aggN <= kDepth) {
        AggPending& a = t_agg[t_aggN - 1];
        corr = a.corrId;
        // The generated loop visits classes IN ORDER and calls only for
        // present entries (r2 verdict defect 2): consume the next present
        // class at the cursor. The address must match that class's snapshot
        // or the attribution is refused (-1) - duplicate entry addresses can
        // no longer collapse onto the first match.
        while (a.cursor < 9 && a.entryV[a.cursor] != Va1::Present) a.cursor++;
        if (a.cursor < 9 && a.entries[a.cursor] == entry) {
            classIdx = (int)a.cursor;
            a.cursor++;
        }
    }
    std::lock_guard<std::mutex> lk(coreMutex());
    c.onSlotOne(corr, classIdx, entry, retR3 != 0, t);
}

}  // namespace sr::wffsm

// ── the two GLOBAL overrides (audited: exactly one strong T per target) ─────

extern "C" void __imp__sub_821F7C38(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_821F7C38) {
    if (!sr::wffsm::active()) { __imp__sub_821F7C38(ctx, base); return; }
    sr::wffsm::readyBody(
        ctx, base, [](PPCContext& c, uint8_t* b) { __imp__sub_821F7C38(c, b); },
        sr::wffsm::core());
}

extern "C" void __imp__sub_82167808(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82167808) {
    if (!sr::wffsm::active()) { __imp__sub_82167808(ctx, base); return; }
    sr::wffsm::slotBody(
        ctx, base, [](PPCContext& c, uint8_t* b) { __imp__sub_82167808(c, b); },
        sr::wffsm::core());
}

// ── self-test (SR_WFFSM_OBS_TEST=1) ─────────────────────────────────────────
namespace sr::wffsm {
namespace {

struct Cap {
    std::vector<std::string> lines;
    static void emit(void* ud, const char* line) {
        static_cast<Cap*>(ud)->lines.push_back(line);
    }
    bool has(const char* s) const {
        for (auto& l : lines) if (l.find(s) != std::string::npos) return true;
        return false;
    }
    size_t count(const char* s) const {
        size_t n = 0;
        for (auto& l : lines) if (l.find(s) != std::string::npos) n++;
        return n;
    }
};

struct FakeReg { union { uint64_t u64; uint32_t u32; }; };
struct FakeCtx { FakeReg r3, r4, r5, r6; uint64_t lr; };

struct FakeGuest {
    uint8_t* res = nullptr;
    static constexpr size_t kSpan = 0xC0000000ull;
    static void require(bool ok, const char* what) {
        if (ok) return;
        SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[WFFSM_TEST] FIXTURE FAIL (fail-closed): %s", what);
        std::exit(3);
    }
    FakeGuest() {
        res = (uint8_t*)VirtualAlloc(nullptr, kSpan, MEM_RESERVE, PAGE_NOACCESS);
        require(res != nullptr, "VirtualAlloc MEM_RESERVE failed");
        sr::kernel::setGuestMemory(res, kSpan);
    }
    ~FakeGuest() {
        if (res) VirtualFree(res, 0, MEM_RELEASE);
    }
    uint8_t* base() { return res; }
    void commit(uint32_t atVA, size_t size = 0x1000) {
        void* p = VirtualAlloc(res + sr::kernel::aliasGuestVA(atVA), size,
                               MEM_COMMIT, PAGE_READWRITE);
        require(p != nullptr, "VirtualAlloc MEM_COMMIT failed");
    }
    void w32(uint32_t atVA, uint32_t v) {
        uint8_t* p = res + sr::kernel::aliasGuestVA(atVA);
        require(sr::kernel::hostRangeReadable(p, 4), "w32 target not committed");
        p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
    }
    void wstr(uint32_t atVA, const char* s) {
        for (size_t i = 0;; i++) {
            uint8_t* p = res + sr::kernel::aliasGuestVA(atVA + (uint32_t)i);
            require(sr::kernel::hostRangeReadable(p, 1), "wstr target not committed");
            *p = (uint8_t)s[i];
            if (!s[i]) break;
        }
    }
};

// Fake FSM graph with the CORRECTED destination chain:
//   fsm{+8 idx,+0x18 cnt,+0x1C arr}; arr[i]->state; state{+8 txnCount,
//   +0x10 scriptObj}; scriptObj+0 -> vtable; vtable+0x0C -> descriptor;
//   descriptor+4 -> enter-script ref. txn{+4 destIdx, +12 predObj};
//   predObj+12 -> predHolder; predHolder+4 -> predicate ref.
struct Graph {
    FakeGuest& g;
    uint32_t fsm, arr, st0, st1, obj1, vtbl1, desc1, txn01, pobj, phold;
    uint32_t obj0, vtbl0, desc0, txn10;
    Graph(FakeGuest& gg, uint32_t baseVA, uint32_t destScriptRef,
          uint32_t predRef) : g(gg) {
        fsm = baseVA; arr = baseVA + 0x100;
        st0 = baseVA + 0x200; st1 = baseVA + 0x300;
        obj1 = baseVA + 0x400; vtbl1 = baseVA + 0x480; desc1 = baseVA + 0x4C0;
        txn01 = baseVA + 0x600; pobj = baseVA + 0x700; phold = baseVA + 0x780;
        obj0 = baseVA + 0x800; vtbl0 = baseVA + 0x880; desc0 = baseVA + 0x8C0;
        txn10 = baseVA + 0x900;
        g.commit(baseVA);
        g.w32(fsm + 8, 0);
        g.w32(fsm + 0x18, 2);
        g.w32(fsm + 0x1C, arr);
        g.w32(arr + 0, st0); g.w32(arr + 4, st1);
        g.w32(st0 + 8, 1);
        g.w32(st1 + 8, 0);
        g.w32(st1 + 0x10, obj1);
        g.w32(obj1 + 0, vtbl1);                  // the VTABLE hop (defect 1)
        g.w32(vtbl1 + 0x0C, desc1);
        g.w32(desc1 + 4, destScriptRef);
        g.w32(txn01 + 4, 1);
        g.w32(txn01 + 12, pobj);
        g.w32(pobj + 12, phold);
        g.w32(phold + 4, predRef);
        // state 0 also carries a script chain, and txn10 targets INDEX 0 -
        // the valid zero destination (r2 verdict defect 1).
        g.w32(st0 + 0x10, obj0);
        g.w32(obj0 + 0, vtbl0);
        g.w32(vtbl0 + 0x0C, desc0);
        g.w32(desc0 + 4, 0x77);                  // state-0 enter-script ref
        g.w32(txn10 + 4, 0);                     // destination = index 0
        g.w32(txn10 + 12, pobj);
    }
};

int runTests() {
    int pass = 0, fail = 0;
    auto CHECK = [&](bool ok, const char* what) {
        if (ok) ++pass;
        else { ++fail; SR_LOG_BOOT(sr::LogSeverity::Error, "[WFFSM_TEST] FAIL: %s", what); }
    };

    // 1. ARM: once-only, serialized, concurrent; prefix/unreadable behavior.
    {
        FakeGuest g;
        std::mutex mx;
        g.commit(0xC0200000u);
        g.wstr(0xC0200000u, "screens/hud/main_hud");
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        armReqBody(g.base(), 0xC0200000u, c, mx);
        armReqBody(g.base(), 0xC0200000u, c, mx);
        CHECK(c.armed() && cap.count("[WFFSM] ARM on") == 1,
              "repeated exact requests arm and announce EXACTLY once");
        Cap cc; Core c2(Sink{&Cap::emit, &cc});
        std::thread th[4];
        for (auto& t : th)
            t = std::thread([&] {
                // every guest-executing thread registers guest memory first
                // (the production side-thread contract; [APT_REQ] runs on a
                // registered guest thread)
                sr::kernel::setGuestMemory(g.res, FakeGuest::kSpan);
                for (int i = 0; i < 50; i++)
                    armReqBody(g.base(), 0xC0200000u, c2, mx);
            });
        for (auto& t : th) t.join();
        CHECK(c2.armed() && cc.count("[WFFSM] ARM on") == 1,
              "200 CONCURRENT exact requests still emit exactly one ARM line");
        g.commit(0x00500000u);
        g.wstr(0x00500000u, "screens/hud/main_hu");
        Cap cap3; Core c3(Sink{&Cap::emit, &cap3});
        armReqBody(g.base(), 0x00500000u, c3, mx);
        armReqBody(g.base(), 0x00400000u, c3, mx);
        armReqBody(g.base(), 0x00400000u, c3, mx);
        CHECK(!c3.armed() && !cap3.has("ARM on") &&
                  cap3.count("ARM-REQ UNREADABLE va=00400000") == 1,
              "prefix never arms; unreadable refuses once");
    }

    // 2. condBody END-TO-END (defects 1+2; lualie/novtbl mutants): the real
    //    boolean; the corrected VTABLE chain; the full identity fields.
    {
        FakeGuest g;
        Graph gr(g, 0x00140000u, 0x2C, 0x2A);
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        pushWalk(gr.fsm, 0);
        FakeCtx cx{}; cx.r3.u64 = gr.txn01; cx.lr = 0;
        condBody(cx, g.base(), [](FakeCtx& cc, uint8_t*) { cc.r3.u64 = 0; }, c);
        CHECK(cap.has("EDGE FIRST") && cap.has("result=0") &&
                  cap.has("destScript=0000002C(P)") && cap.has("pred=0000002A(P)") &&
                  cap.has("dest=1(P)") && cap.has("src=0"),
              "the body records the REAL false result + the VTABLE-chain script ref");
        char fsmTag[24];
        std::snprintf(fsmTag, sizeof(fsmTag), "fsm=%08X", gr.fsm);
        CHECK(cap.has(fsmTag) && cap.has("corr="),
              "the EDGE record names the FSM and carries a correlation id");
        cx.r3.u64 = gr.txn01;
        condBody(cx, g.base(), [](FakeCtx& cc, uint8_t*) { cc.r3.u64 = 0; }, c);
        CHECK(cap.count("EDGE") == 1, "a repeated identical result does not emit");
        cx.r3.u64 = gr.txn01;
        condBody(cx, g.base(), [](FakeCtx& cc, uint8_t*) { cc.r3.u64 = 7; }, c);
        popWalk();
        CHECK(cap.has("EDGE CHANGE") && cap.has("result=1") &&
                  cap.has("totals(evals=3 true=1)"),
              "a result CHANGE emits the real boolean with totals");
        CHECK(condDepth() == 0 && walkDepth() == 0, "cond+walk TLS fully consumed");
    }

    // 3. (fsm,txn) dedup key (defect 2): same txn under another fsm emits.
    {
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        EdgeEval e{};
        e.txn = 0x7777;
        e.fsm = 0xAAAA0000u; e.haveCtx = true;
        c.onCondEval(e, false, 10);
        e.fsm = 0xBBBB0000u;
        c.onCondEval(e, false, 20);
        CHECK(cap.count("EDGE FIRST") == 2,
              "the dedup key is (fsm,txn) - same txn under another fsm emits");
    }

    // 4. walkerBody: pre->delegate->post exactly once; context inside the
    //    delegate; TLS cleanup; full-width register preservation; per-FSM
    //    STATE dedup (defect 3; globallast mutant) through the ARMED core.
    {
        FakeGuest g;
        Graph ga(g, 0x00170000u, 0x2C, 0x2A);
        Graph gb(g, 0x00180000u, 0x31, 0x2B);
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        {
            std::mutex mx;
            g.commit(0x00190000u);
            g.wstr(0x00190000u, "screens/hud/main_hud");
            armReqBody(g.base(), 0x00190000u, c, mx);   // local core ARMED
        }
        int delRuns = 0;
        bool ctxInDelegate = false;
        auto drive = [&](uint32_t fsmVA) {
            FakeCtx cx{};
            cx.r3.u64 = 0xFFFFFFFF00000000ull | fsmVA;  // dirty upper half
            cx.lr = 0;
            walkerBody(cx, g.base(),
                       [&](FakeCtx& cc, uint8_t*) {
                           delRuns++;
                           WalkCtx w{};
                           ctxInDelegate = topWalk(&w) && w.fsm == fsmVA;
                           cc.r4.u64 = 0xFEED0000DEADBEEFull;
                       },
                       c);
            return cx.r4.u64 == 0xFEED0000DEADBEEFull;
        };
        bool preserved = drive(ga.fsm);
        CHECK(delRuns == 1 && ctxInDelegate && preserved && walkDepth() == 0,
              "walkerBody: one delegation, context visible inside, TLS cleaned, "
              "delegate-written registers preserved");
        drive(gb.fsm);
        drive(ga.fsm);
        drive(gb.fsm);
        CHECK(cap.count("[WFFSM] STATE fsm=") == 2,
              "stable interleaved fsms A0,B0,A0,B0 emit once EACH (per-FSM dedup)");
        g.w32(ga.fsm + 8, 1);                    // A really changes state
        drive(ga.fsm);
        CHECK(cap.count("[WFFSM] STATE fsm=") == 3,
              "a real index change on a tracked fsm still emits");
    }

    // 4b. walkerDispatch (iter-455m pre-arm rework r2; prewrap + eagercore
    //     mutants): the ONE production dispatch decision. Gate-off delegates
    //     directly WITHOUT invoking the core accessor (blocker 1: the eager
    //     argument heap-constructed the singleton on the old gate-off path);
    //     enabled invokes it EXACTLY once; enabled-but-unarmed delegates
    //     directly - no TLS context, no record (walkerPreProd and the guest
    //     read are unreachable by construction); after the exact main_hud
    //     arm the NEXT call wraps with context exposed. FULL-WIDTH proof on
    //     every path (blocker 2): dirty 64-bit r3/r4/r5/r6 in, the delegate
    //     asserts all four arrive unchanged, overwrites all four, and every
    //     full-width write must survive the return.
    {
        FakeGuest g;
        Graph ga(g, 0x001A0000u, 0x2C, 0x2A);
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        int coreCalls = 0;
        auto coreFn = [&]() -> Core& { coreCalls++; return c; };
        int delRuns = 0;
        uint32_t depthIn = 99;
        bool ctxIn = true;
        bool recvOk = false;
        const uint64_t in4 = 0xBBBB000011110000ull;
        const uint64_t in5 = 0xCCCC000022220000ull;
        const uint64_t in6 = 0xDDDD000033330000ull;
        const uint64_t out3 = 0x1111FFFF0000AAAAull;
        const uint64_t out4 = 0x2222FFFF0000BBBBull;
        const uint64_t out5 = 0x3333FFFF0000CCCCull;
        const uint64_t out6 = 0x4444FFFF0000DDDDull;
        auto drive = [&](bool gateEnabled) {
            FakeCtx cx{};
            const uint64_t in3 = 0xAAAA000000000000ull | ga.fsm;  // dirty top
            cx.r3.u64 = in3;
            cx.r4.u64 = in4;
            cx.r5.u64 = in5;
            cx.r6.u64 = in6;
            cx.lr = 0;
            walkerDispatch(cx, g.base(),
                           [&](FakeCtx& cc, uint8_t*) {
                               delRuns++;
                               depthIn = walkDepth();
                               WalkCtx w{};
                               ctxIn = topWalk(&w);
                               recvOk = cc.r3.u64 == in3 &&
                                        cc.r4.u64 == in4 &&
                                        cc.r5.u64 == in5 &&
                                        cc.r6.u64 == in6;
                               cc.r3.u64 = out3;
                               cc.r4.u64 = out4;
                               cc.r5.u64 = out5;
                               cc.r6.u64 = out6;
                           },
                           gateEnabled, coreFn);
            return cx.r3.u64 == out3 && cx.r4.u64 == out4 &&
                   cx.r5.u64 == out5 && cx.r6.u64 == out6;
        };
        const bool p0 = drive(false);            // gate OFF, core UNARMED
        CHECK(delRuns == 1 && coreCalls == 0 && depthIn == 0 && !ctxIn &&
                  recvOk && p0,
              "gate-off dispatch: direct, the core accessor is NEVER invoked, "
              "full-width r3-r6 in AND out preserved");
        delRuns = 0; depthIn = 99; ctxIn = true; recvOk = false;
        const bool p1 = drive(true);             // enabled, core UNARMED
        CHECK(delRuns == 1 && coreCalls == 1 && depthIn == 0 && !ctxIn &&
                  recvOk && p1 && cap.count("[WFFSM]") == 0,
              "pre-arm dispatch: enabled-but-unarmed delegates directly (one "
              "delegation, accessor EXACTLY once, no TLS context, no record, "
              "full-width preserved)");
        {
            std::mutex mx;
            g.commit(0x001B0000u);
            g.wstr(0x001B0000u, "screens/hud/main_hud");
            armReqBody(g.base(), 0x001B0000u, c, mx);
        }
        delRuns = 0; depthIn = 99; ctxIn = false; recvOk = false;
        const bool p2 = drive(true);             // enabled, core ARMED
        CHECK(delRuns == 1 && coreCalls == 2 && depthIn == 1 && ctxIn &&
                  recvOk && p2 && cap.count("[WFFSM] STATE fsm=") == 1,
              "post-arm dispatch: the NEXT walker call wraps normally "
              "(accessor once more, context exposed, STATE recorded, "
              "full-width preserved through the wrap)");
        delRuns = 0; depthIn = 99; recvOk = false;
        const bool p3 = drive(false);            // gate OFF, core ARMED
        CHECK(delRuns == 1 && coreCalls == 2 && depthIn == 0 && recvOk && p3,
              "gate-off dispatch on an ARMED core: still direct, accessor "
              "still not invoked, full-width preserved");
        CHECK(walkDepth() == 0, "dispatch fixture leaves the walk TLS clean");
    }

    // 5. STATE tracker overflow (defect 3): explicit, once, no flood.
    {
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        for (uint32_t i = 0; i < kFsmTrack + 5; i++)
            c.onWalkerState(0x1000 + i * 0x100, 0, 0, 0, i);
        CHECK(cap.count("STATE-TRACKER OVERFLOW") == 1 &&
                  cap.count("[WFFSM] STATE fsm=") == kFsmTrack,
              "fsm 17+ announces overflow once and is counted, not emitted");
    }

    // 6. EDGE tracker overflow (defect 4): identity 129 repeated is NOT
    //    'fresh forever' - one announce, zero emits.
    {
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        EdgeEval e{};
        e.fsm = 0x1000;
        for (uint32_t i = 0; i < kEdgeTrack; i++) {
            e.txn = 0x9000 + i;
            c.onCondEval(e, false, i);
        }
        const size_t before = cap.count("EDGE FIRST");
        e.txn = 0x9000 + kEdgeTrack;             // identity 129
        for (int r = 0; r < 10; r++) c.onCondEval(e, false, 200 + r);
        CHECK(cap.count("EDGE-TRACKER OVERFLOW") == 1 &&
                  cap.count("EDGE FIRST") == before,
              "an untracked identity announces once and NEVER floods the cap");
    }

    // 7. Independent caps (capbleed mutant): EDGE flood leaves STATE alive.
    {
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        EdgeEval e{};
        e.fsm = 0x1000;
        for (uint32_t round = 0; round < 5; round++)
            for (uint32_t i = 0; i < kEdgeTrack; i++) {
                e.txn = 0xA000 + i;
                c.onCondEval(e, (round & 1) != 0, round * 1000 + i);
            }
        CHECK(cap.count("EDGE SUPPRESSED past cap=") == 1,
              "the EDGE class suppresses exactly once at its own cap");
        c.onWalkerState(0x4321, 7, 0x8000, 2, 999);
        CHECK(cap.has("[WFFSM] STATE fsm=00004321 idx=7"),
              "a STATE record still emits after the EDGE flood (independent caps)");
    }

    // 8. readyBody + slotBody (defect 5): LR filters, shared correlation id,
    //    class indexes, actual returns (oneomit mutant), output preservation.
    {
        FakeGuest g;
        Graph gr(g, 0x001A0000u, 0x2C, 0x2A);
        const uint32_t mgr = 0x001B0000u, tbl0 = 0x001B1000u, tbl1 = 0x001B2000u;
        g.commit(mgr); g.commit(tbl0); g.commit(tbl1);
        g.w32(mgr + 4 * 55, tbl0);
        g.w32(mgr + 4 * 56, tbl1);
        g.w32(tbl0 + 4 * 3, 0xAAAA0000u);
        g.w32(tbl1 + 4 * 3, 0xBBBB0000u);
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        pushWalk(gr.fsm, 0);
        condPreProd(gr.txn01, g.base());         // active condition context
        FakeCtx bad{}; bad.r3.u64 = mgr; bad.r4.u64 = 3; bad.lr = 0x12345678u;
        bool badRan = false;
        readyBody(bad, g.base(),
                  [&](FakeCtx&, uint8_t*) { badRan = true; }, c);
        CHECK(badRan && !inReady() && cap.lines.empty(),
              "a wrong-LR aggregate call delegates only (no records, no flag)");
        FakeCtx cx{}; cx.r3.u64 = mgr; cx.r4.u64 = 3; cx.lr = kAwfalCallLR;
        readyBody(cx, g.base(),
                  [&](FakeCtx& cc, uint8_t* b) {
                      FakeGuest::require(inReady(), "flag set across delegate");
                      FakeCtx s1{}; s1.r3.u64 = 0xAAAA0000u; s1.lr = kSlotCallLR;
                      slotBody(s1, b, [](FakeCtx& sc, uint8_t*) { sc.r3.u64 = 1; }, c);
                      FakeCtx s2{}; s2.r3.u64 = 0xBBBB0000u; s2.lr = kSlotCallLR;
                      slotBody(s2, b, [](FakeCtx& sc, uint8_t*) { sc.r3.u64 = 0; }, c);
                      FakeCtx s3{}; s3.r3.u64 = 0xAAAA0000u; s3.lr = 0x11111111u;
                      slotBody(s3, b, [](FakeCtx& sc, uint8_t*) { sc.r3.u64 = 1; }, c);
                      cc.r3.u64 = 0xFEEDF00D00000000ull;   // aggregate ret: false
                  },
                  c);
        CHECK(cx.r3.u64 == 0xFEEDF00D00000000ull,
              "the delegate's full-width return survives the body untouched");
        CHECK(cap.has("class=0 entry=AAAA0000 ready=1") &&
                  cap.has("class=1 entry=BBBB0000 ready=0"),
              "individual ACTUAL returns recorded with their class indexes");
        CHECK(cap.count("READY-ONE") == 2,
              "the wrong-LR slot call is filtered out");
        CHECK(cap.has("READY-AGG") && cap.has("group=3 result=0") &&
                  cap.has("0:AAAA0000(P) 1:BBBB0000(P) 2:00000000(M)"),
              "the aggregate carries entries with tri-state validity");
        char corrTag[24];
        std::snprintf(corrTag, sizeof(corrTag), "corr=%u", topCondCorr());
        CHECK(cap.count(corrTag) == 3,
              "READY-AGG and both READY-ONE records share the condition's corr id");
        condPostProd(c, 0, 99);
        popWalk();
        CHECK(!inReady() && condDepth() == 0 && walkDepth() == 0,
              "all TLS cleaned after the correlated sequence");
    }

    // 9. Tri-state honesty (defect 6): an UNREADABLE table is 'U', never 'M'.
    {
        FakeGuest g;
        Graph gr(g, 0x001C0000u, 0x2C, 0x2A);
        const uint32_t mgrU = 0x001D0000u;       // mgr committed, table NOT
        g.commit(mgrU);
        g.w32(mgrU + 4 * 55, 0x00700000u);       // class-0 table: uncommitted VA
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        pushWalk(gr.fsm, 0);
        condPreProd(gr.txn01, g.base());
        FakeCtx cx{}; cx.r3.u64 = mgrU; cx.r4.u64 = 3; cx.lr = kAwfalCallLR;
        readyBody(cx, g.base(), [](FakeCtx& cc, uint8_t*) { cc.r3.u64 = 0; }, c);
        condPostProd(c, 0, 10);
        popWalk();
        CHECK(cap.has("class0=U") && cap.has("0:00000000(U)"),
              "an unreadable class-0 table reads U (unknown), NOT M (missing)");
        CHECK(cap.has("1:00000000(M)"),
              "a readable-null table reads M (guest-absent)");
    }

    // 10. No-context eval + unreadable chain links are honest.
    {
        FakeGuest g;
        g.commit(0x001E0000u);
        g.w32(0x001E0000u + 4, 1);
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        FakeCtx cx{}; cx.r3.u64 = 0x001E0000u; cx.lr = 0;
        condBody(cx, g.base(), [](FakeCtx& cc, uint8_t*) { cc.r3.u64 = 0; }, c);
        CHECK(cap.has("ctx=0") && cap.has("pred=00000000(M)"),
              "a context-less eval records ctx=0; a null pred link reads M");
        FakeCtx cy{}; cy.r3.u64 = 0x00710000u; cy.lr = 0;   // uncommitted txn
        condBody(cy, g.base(), [](FakeCtx& cc, uint8_t*) { cc.r3.u64 = 0; }, c);
        CHECK(cap.has("dest=0(U)"),
              "an unreadable txn field reads U, never a fake value");
    }

    // 11. Destination INDEX 0 is valid (r2 verdict defect 1; zeromissing
    //     mutant): its state VA and enter-script ref must resolve.
    {
        FakeGuest g;
        Graph gr(g, 0x001F0000u, 0x2C, 0x2A);
        g.w32(gr.fsm + 8, 1);                    // current state = 1
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        pushWalk(gr.fsm, 1);
        FakeCtx cx{}; cx.r3.u64 = gr.txn10; cx.lr = 0;   // dest idx = 0
        condBody(cx, g.base(), [](FakeCtx& cc, uint8_t*) { cc.r3.u64 = 0; }, c);
        popWalk();
        char stTag[32];
        std::snprintf(stTag, sizeof(stTag), "destState=%08X(P)", gr.st0);
        CHECK(cap.has("dest=0(P)") && cap.has(stTag) &&
                  cap.has("destScript=00000077(P)"),
              "destination INDEX 0 resolves its state VA and script ref");
    }

    // 12. Duplicate-address class attribution (r2 verdict defect 2;
    //     firstmatch mutant): classes 0 and 1 sharing one entry report
    //     class=0 then class=1 via the ordered cursor.
    {
        FakeGuest g;
        Graph gr(g, 0x00200000u, 0x2C, 0x2A);
        const uint32_t mgr = 0x00210000u, tbl0 = 0x00211000u, tbl1 = 0x00212000u;
        g.commit(mgr); g.commit(tbl0); g.commit(tbl1);
        g.w32(mgr + 4 * 55, tbl0);
        g.w32(mgr + 4 * 56, tbl1);
        g.w32(tbl0 + 4 * 3, 0xCCCC0000u);        // classes 0 and 1 SHARE
        g.w32(tbl1 + 4 * 3, 0xCCCC0000u);        // one entry address
        Cap cap; Core c(Sink{&Cap::emit, &cap});
        pushWalk(gr.fsm, 0);
        condPreProd(gr.txn01, g.base());
        FakeCtx cx{}; cx.r3.u64 = mgr; cx.r4.u64 = 3; cx.lr = kAwfalCallLR;
        readyBody(cx, g.base(),
                  [&](FakeCtx&, uint8_t* b) {
                      FakeCtx s1{}; s1.r3.u64 = 0xCCCC0000u; s1.lr = kSlotCallLR;
                      slotBody(s1, b, [](FakeCtx& sc, uint8_t*) { sc.r3.u64 = 1; }, c);
                      FakeCtx s2{}; s2.r3.u64 = 0xCCCC0000u; s2.lr = kSlotCallLR;
                      slotBody(s2, b, [](FakeCtx& sc, uint8_t*) { sc.r3.u64 = 1; }, c);
                  },
                  c);
        condPostProd(c, 0, 10);
        popWalk();
        CHECK(cap.has("class=0 entry=CCCC0000 ready=1") &&
                  cap.has("class=1 entry=CCCC0000 ready=1"),
              "a shared entry address reports class=0 THEN class=1 (cursor)");
    }

    const int total = pass + fail;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[WFFSM_TEST] %d/%d passed", pass, total);
    return fail;
}

}  // namespace

int runSelfTestIfRequested() {
    const char* e = std::getenv("SR_WFFSM_OBS_TEST");
    if (!e || e[0] != '1') return -1;
    return runTests();
}

}  // namespace sr::wffsm
