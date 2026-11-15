// iter-455j: SR_H7_ENTRYWATCH — bounded, default-OFF, OBSERVE-ONLY probe for the
// APT resolver-entry lifecycle (H7u / H7c).
//
// ⛔ WHAT THIS IS NOT. It writes no guest state, calls no guest function, suppresses
// no draw, changes no guest decision and adds no autoplay. Every hook delegates to
// its __imp__ unconditionally and unmodified; the only writes are to host memory.
// Registered `observe` in docs/OVERRIDES.json.
//
// ── The contract this probe observes (IDA + generated PPC, iter-455i/j) ────────
//
// AptResolvePendingClips_sub_826AFA58(a1):
//     a1  -> list head cell; *a1 = first node.
//     node: {+0 value, +4 next}                       (8 bytes, RAW/non-owning)
//     value = the 28-byte RESOLVER ENTRY:
//         +0  refcount   +4 interned name   +8 state   +12 prior state   +20 blob
//     states walked: 1 -> 2 (publish), 3 -> 4 (resolved), 2/4/5/6 skipped,
//     anything else -> AptListRemoveByValue_sub_8269B540(a1, entry) + release.
//     refcount 0 -> AptDisplayObjectDestroy_sub_826A3880(entry)
//                   then MemPoolFree_sub_8268DA40(dword_829A0AA8, entry, 0x1C).
//
//     ⚠ The list is passed IN as a1. This probe uses that argument and NEVER reads
//     the transient global 0x829A17BC: that global is published per-call and is 0
//     between calls, so polling it would sample a different — or absent — context
//     than the one actually being resolved.
//
// AptParseReloc_826AE6D0(r3, r4, r5, r6, r7)  [VERIFIED in IDA at 0x826AE70C-0x826AE74C]:
//     r4 = &holder     (*r4 is the holder object; the guest sets holder[2] = 3)
//     r5 = blob        (the base ADDED to the header's +20 offset)
//     r6 = header      (fields +20 and +28 are rebased across the registrar call)
//     r7 = node
//     recordArray = (*(header+20) ? *(header+20) + blob : 0) + 16
//     ⚠ The long-standing comment in apt_parse_trace.cpp said "r5 = dataLen, r6 =
//     blob". That is WRONG, and the first version of this probe inherited it and so
//     watched the wrong identity entirely. Decompile: `v11 = *(a4+20); if (v11) v11
//     += a3; v12 = (int *)(v11 + 16);` — a3 is r5 and a4 is r6, not the reverse.
//
// AptAS2SetVariableByPath_sub_826B7378(a1 = TASK list, ...) is the state-6 writer:
//     it erases any existing node for the clip (sub_826C3758 finder +
//     AptListNodeErase_sub_826C9D00) BEFORE pushing a new one — the
//     "replace-before-insert" property, which holds on every path reaching an owning
//     insertion within single-threaded intra-function control flow, and NOT beyond
//     (base-case uniqueness, task+8 immutability, re-entrancy and global uniqueness
//     are all OPEN — that is exactly what matchCount below is here to settle).
//
// AptInstantiateChildClip_sub_826B0960(a1 = TASK list, a2 = clip) — the 6->5 path:
//     task: {+0 refcount, +4 entry (owned), +8 clip, +12 processedFlag}
//     finder sub_826C3758 matches on task+8 == clip and returns the FIRST match, so
//     uniqueness is NOT enforced by it. AptListNodeErase sits INSIDE the `!= 5`
//     guard, so when the entry is already 5 the node stays linked while the clip is
//     replaced — the teardown asymmetry H7u is about.
//
// ── Shadow membership: STORED key is (resolver list, entry); the generation is
// validated INSIDE the record ────────────────────────────────────────────────
// Stated precisely, because "keyed by (list, entry, generation)" was an overstatement
// in an earlier packet. The map key is the 64-bit pair; each record carries the
// generation it was latched at, and `observeEntry` compares that against the current
// generation for the address and DISCARDS the record on mismatch. The observable
// behaviour is equivalent to a triple key — a stale-generation record is never read
// as if it described the live object — but the mechanism is a pair key plus a
// validate-and-discard step, and it is tested as such (test 8).
//
// Keying on the entry address ALONE was the earlier defect: one address can be a
// member of two latched lists, so a removal from one cleared membership belonging to
// the other — and since membership gates [H7_DTOR_LINKED], that silently SUPPRESSED
// the one signal this probe exists to emit.
//
// ── What it emits (all bounded) ───────────────────────────────────────────────
//   [H7_RESOLVE]      resolver census delta   (change-triggered, capped)
//   [H7_REMOVE]       AptListRemoveByValue, AFTER delegation, membership re-checked
//   [H7_REMOVE_OOS]   the same helper on a list this observer never latched. Observation
//                     ONLY: no membership is consulted or changed. It exists because
//                     [H7_REMOVE] 0 could not be told apart from "the helper never ran"
//   [H7_EXEC6]        one event per 6->5 executor entry (capped)
//   [H7_DESTROY]      every TRACKED-entry destruction, naming the reconfirmed result
//                     (linked / absent / unknown). Descriptive, carries no verdict; the
//                     decisive tags below are unchanged. Untracked destructions stay
//                     silent, so this means "a tracked entry was destroyed", nothing more
//   [H7_RMDESTROY]    the RAW clip -> +32 -> +40 candidate chain around
//                     AptCharacterRemoveAndDestroy, followed UNCONDITIONALLY, with the
//                     gating type word logged. It ATTRIBUTES NOTHING: it reports two
//                     independent facts, `typeEligible` (is the type tag 9?) and
//                     `observedTo5` (did w8 go to 5 across the call?). Neither field,
//                     alone or together, is a claim that the gated writer ran
//   [H7_PARSEFIRST]   first sighting of an H7c record-array identity. The bridge's own
//                     positive witness: without it, "never fired" and "every parse was a
//                     distinct first identity" produced identical logs
//   [H7_PARSEREP]     H7c REPEAT record-array sighting
//   [H7_PARSEREFUSE]  H7c parser call that produced NEITHER - an underivable record
//                     array, or an identity the observer had no capacity to track.
//                     FIRST / REPEAT / REFUSE are exhaustive and mutually exclusive:
//                     every parser-hook call is exactly one of them
// Anomaly classes, each ONCE-ONLY and each bypassing the ordinary cap:
//   [H7_DTOR_LINKED]  DESTRUCTOR ENTERED WHILE LINKED (historical alias:
//                     H7_LINKED_FREE, renamed at the director's direction because the
//                     old name overstated the hook point — the hook observes destructor
//                     ENTRY; the destructor then unlinks before its caller pool-frees, so
//                     entry, return, unlink and free are distinct events). Membership is
//                     RE-CONFIRMED linked by a complete walk at the moment of destructor
//                     entry — never from cached or unconfirmed state. This is a NECESSARY condition, not an
//                     "if and only if": the walk is not serialised against the guest,
//                     so a torn observation could in principle satisfy it (see the
//                     no-lock note on noteDestroy)
//   [H7_LINKED_UNKNOWN] entry destroyed while membership could NOT be re-confirmed.
//                     Deliberately a separate, non-decisive tag: it is not a linked
//                     free and must never be counted as one
//   [H7_REUSE_UNKNOWN] an address was re-censused after an OBSERVED destroy but its
//                     lifetime history is untracked. A fresh record is started; the
//                     tombstone is never retained as the same lifetime
//   [H7_MATCHDUP]     matchCount > 1 for one clip
//   [H7_CYCLE]        list cycle or per-walk node cap — a MALFORMED STRUCTURE only
//   [H7_PARTIAL]      a walk did not reach null, so absences were NOT reconciled
//   [H7_BADPTR]       unmapped/misaligned guest pointer refused
//   [H7_REUSE]        allocator handed the same address back after a destroy
//   [H7_CAP]          a persistent container or the census call budget filled up
// [H7_CYCLE] means malformed data. [H7_CAP] means the probe ran out of room. They
// were the same tag in the previous candidate, which made "the list is broken" and
// "I stopped looking" indistinguishable in the log.
//
// ⚠ COUNTABILITY OF THESE TAGS. Every [H7_*] tag is COUNT-UNSAFE by construction and
// `tools/audit_log_gating.py --tag H7_*` reports NOT RECOGNIZED (exit 3) — the tag is
// passed as a runtime `cls` string, so the auditor's literal-prefix scan cannot see
// it. That report is CORRECT and is not to be worked around by planting literals for
// the scanner to find. The consequence is a rule, not a caveat:
//   - ordinary classes are hard-capped at kOrdinaryCap, so a line count is a FLOOR,
//     never a population. When the cap engages the log says so on its own line.
//   - anomaly classes are ONCE-ONLY: their presence is a fact, their count is 1 by
//     construction and means nothing.
//   - the only number here that is a real total is matchCount inside a single
//     [H7_EXEC6] record, because it is derived from one complete bounded walk and
//     the record states whether that walk completed, hit a cycle, or was refused.
// Quote nothing else from these lines as a quantity.
//
// Host-side self-test: SR_H7_ENTRYWATCH_TEST=1 -> [H7_TEST] n/m passed. It drives
// the shadow core over a SYNTHETIC address space (no guest, no game process), which
// is the only way to exercise generations, removal scoping, cycles, caps and
// allocator reuse deterministically. It is decided in main() BEFORE the zombie
// killer runs: a "pure host-side test runner" that terminates somebody else's game
// is not host-side.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sr::kernel::h7 {

// ── Bounds. Every one of these is a REFUSAL point, not a truncation point: when a
// cap is hit the probe says so (once, per container) rather than silently reporting
// a short answer that reads as "nothing happened".
constexpr uint32_t kMaxNodes       = 4096;    // per-walk node budget
constexpr uint32_t kMaxShadow      = 4096;    // (list, entry) membership records
constexpr uint32_t kMaxGenTracked  = 8192;    // addresses with a live generation
constexpr uint32_t kMaxKnownLists  = 256;     // latched resolver lists
constexpr uint32_t kMaxParse       = 4096;    // H7c record-array identities
constexpr uint32_t kMaxRmd         = 4096;    // clip -> RemoveAndDestroy states
constexpr uint32_t kOrdinaryCap    = 64;      // per ordinary event class
constexpr uint32_t kNameChars      = 31;
// AptResolvePendingClips is pump-driven. Two full walks per call is trivial at the
// observed cadence, but the probe must not be able to throttle the run it measures
// if some path calls it far harder than expected.
constexpr uint32_t kMaxCensusCalls = 200000;

// ── Memory access is injected so the self-test can run without a guest ─────────
struct MemReader {
    bool (*read32)(void* ud, uint32_t va, uint32_t& out);
    bool (*readBytes)(void* ud, uint32_t va, uint8_t* dst, uint32_t n);
    void* ud;
};

struct Sink {
    void (*emit)(void* ud, const char* cls, const char* msg);
    void* ud;
};

inline bool validPtrVA(uint32_t va) { return va >= 0x10000u && va < 0xC0000000u && (va & 3u) == 0u; }

// ── H7c argument derivation, isolated so it can be tested adversarially ────────
// recordArray = (*(header+20) ? *(header+20) + blob : 0) + 16, exactly as the guest
// computes it at 0x826AE70C-0x826AE71C. Kept a free function precisely so a test can
// prove the mapping is r5=blob / r6=header and NOT the reverse.
inline uint32_t deriveRecordArray(uint32_t blob, uint32_t headerField20)
{
    return (headerField20 ? headerField20 + blob : 0u) + 16u;
}

struct ParseArgs {
    uint32_t blob = 0, header = 0, node = 0;
    uint32_t field20 = 0, recordArray = 0, record0 = 0;
    bool     valid = false;
    const char* refusal = "";
};

// Returns false (with `refusal` set) rather than guessing. The guest itself takes an
// early-out when blob == 0, so there is no record array in that case at all.
inline bool deriveParseArgs(uint32_t blob, uint32_t header, uint32_t node,
                            const MemReader& rd, ParseArgs& out)
{
    out.blob = blob; out.header = header; out.node = node;
    if (!blob)                { out.refusal = "blob==0 (guest takes the early-out branch)"; return false; }
    if (!validPtrVA(header))  { out.refusal = "header not a valid guest pointer";           return false; }
    if (!rd.read32(rd.ud, header + 20u, out.field20)) { out.refusal = "header+20 unreadable"; return false; }
    out.recordArray = deriveRecordArray(blob, out.field20);
    if (!out.field20)         { out.refusal = "header+20 == 0 (no record array)";           return false; }
    if (!validPtrVA(out.recordArray)) { out.refusal = "derived record array not a valid pointer"; return false; }
    if (!rd.read32(rd.ud, out.recordArray, out.record0)) { out.refusal = "record[0] unreadable"; return false; }
    out.valid = true;
    return true;
}

// ── Membership is TRI-STATE, and that is load-bearing. ────────────────────────
// A two-state `inList` flag forces every incomplete observation to be rounded to
// either "linked" or "absent", and BOTH roundings corrupt the one tag that matters:
//   - round to absent  -> a truncated walk silences a real [H7_DTOR_LINKED];
//   - round to linked  -> a correctly-removed entry whose removal could not be
//                         confirmed fires [H7_DTOR_LINKED] FALSELY when it is later
//                         destroyed.
// The first candidate did the former, the second did the latter. Unknown is a real
// answer and is reported as one.
enum class Membership : uint8_t { Unknown = 0, Linked = 1, Absent = 2 };

inline const char* membershipName(Membership m)
{
    return m == Membership::Linked ? "linked" : (m == Membership::Absent ? "absent" : "unknown");
}

// ── The shadow record. `name` is cached WHILE THE OBJECT IS LIVE so that a linked
// destruction can be reported by identity without ever dereferencing an object that
// is entering destruction.
struct EntryRec {
    uint32_t listVA    = 0;
    uint32_t entryVA   = 0;
    uint32_t gen       = 0;
    uint32_t nameVA    = 0;
    uint32_t state     = 0xFFFFFFFFu;
    uint32_t prior     = 0xFFFFFFFFu;
    uint32_t refcount  = 0;
    char     name[kNameChars + 1] = {};
    Membership member = Membership::Unknown;   // membership of THIS list
    bool     removed   = false;   // removal from THIS list observed after delegation
    bool     destroyed = false;   // tombstone: kept so allocator reuse is detectable
};

class Watch {
public:
    explicit Watch(Sink sink) : m_sink(sink) {}

    static bool validPtr(uint32_t va) { return validPtrVA(va); }
    static uint64_t compositeKey(uint32_t listVA, uint32_t entryVA)
    { return (uint64_t(listVA) << 32) | uint64_t(entryVA); }

    // ── Census the resolver list PASSED BY THE CALLER. ────────────────────────
    // TRANSACTIONAL: absences are reconciled ONLY when the walk reached a null next.
    // Any truncation (bad pointer, failed read, cycle, node cap) leaves membership
    // exactly as it was and says so, because clearing `inList` on partial evidence
    // suppresses [H7_DTOR_LINKED] — the probe would go quiet precisely when the
    // structure it is watching is damaged.
    void censusResolverList(uint32_t listHeadCellVA, const MemReader& rd)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (++m_censusCalls > m_censusCap) {
            capOnce(m_capCensusSeen, "census call budget", m_censusCap,
                    "the resolver is no longer walked; later membership changes are NOT observed");
            return;
        }
        if (!validPtr(listHeadCellVA)) { anomalyBadPtr("resolver list head", listHeadCellVA); return; }
        if (!m_knownLists.count(listHeadCellVA)) {
            if (m_knownLists.size() >= kMaxKnownLists) {
                capOnce(m_capListsSeen, "latched resolver lists", kMaxKnownLists,
                        "this list is NOT latched; its removals will read as out-of-scope");
                return;
            }
            m_knownLists.insert(listHeadCellVA);
        }

        uint32_t node = 0;
        if (!rd.read32(rd.ud, listHeadCellVA, node)) { anomalyBadPtr("list head deref", listHeadCellVA); return; }

        std::unordered_set<uint32_t> seenNodes;
        std::unordered_set<uint32_t> present;
        bool complete = false;
        const char* why = "";
        uint32_t n = 0;
        for (;;) {
            if (!node) { complete = true; break; }
            if (!validPtr(node))                    { anomalyBadPtr("resolver node", node); why = "invalid node"; break; }
            if (++n > kMaxNodes)                    { anomalyCycle("resolver list node budget", listHeadCellVA, n); why = "node budget"; break; }
            if (!seenNodes.insert(node).second)     { anomalyCycle("resolver node revisit", node, n); why = "cycle"; break; }

            uint32_t entry = 0;
            if (!rd.read32(rd.ud, node, entry))     { anomalyBadPtr("node value", node); why = "node value unreadable"; break; }
            if (entry) {
                if (!validPtr(entry)) { anomalyBadPtr("resolver entry", entry); why = "invalid entry"; break; }
                present.insert(entry);
                observeEntry(listHeadCellVA, entry, rd);
            }
            uint32_t next = 0;
            if (!rd.read32(rd.ud, node + 4u, next)) { anomalyBadPtr("node next", node + 4u); why = "next unreadable"; break; }
            node = next;
        }

        if (!complete) {
            if (!m_partialSeen) {
                m_partialSeen = true;
                emitRaw("[H7_PARTIAL]",
                        "list=%08X walk truncated after %u node(s) (%s) — entries seen in "
                        "the prefix stay CONFIRMED LINKED; every unseen entry becomes "
                        "UNKNOWN, which is neither absent nor still-linked",
                        listHeadCellVA, n, why);
            }
            ++m_partialCount;
            // Anything not reached is genuinely unknown. Leaving it Linked would let a
            // later destruction fire a decisive [H7_DTOR_LINKED] on stale evidence;
            // marking it Absent would silence a real one.
            for (auto& kv : m_shadow) {
                EntryRec& r = kv.second;
                if (r.listVA != listHeadCellVA || r.destroyed) continue;
                if (present.count(r.entryVA)) continue;
                if (r.member == Membership::Unknown) continue;
                r.member = Membership::Unknown;
                emitOrdinary(m_nResolve, "[H7_RESOLVE]",
                             "unk  list=%08X entry=%08X gen=%u name='%s' — not reached by a "
                             "truncated walk; membership now UNKNOWN",
                             r.listVA, r.entryVA, r.gen, r.name);
            }
            return;
        }

        // Complete walk: an entry we had latched on THIS list and did not see is absent.
        for (auto& kv : m_shadow) {
            EntryRec& r = kv.second;
            if (r.listVA != listHeadCellVA || r.destroyed) continue;
            if (present.count(r.entryVA)) continue;
            if (r.member == Membership::Absent) continue;
            r.member = Membership::Absent;
            emitOrdinary(m_nResolve, "[H7_RESOLVE]",
                         "gone list=%08X entry=%08X gen=%u name='%s' lastState=%u (confirmed absent)",
                         r.listVA, r.entryVA, r.gen, r.name, r.state);
        }
    }

    // ── AptListRemoveByValue, observed AFTER the helper has run. ──────────────
    // A pre-call update would report a removal the helper might not perform. This
    // re-checks membership of the exact (list, entry) pair and only records the
    // removal if the value is genuinely absent afterwards.
    void noteRemoveByValueAfter(uint32_t listVA, uint32_t entryVA, const MemReader& rd)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_knownLists.count(listVA)) {
            // Out-of-scope removals used to increment a HOST-ONLY counter with no log
            // surface, so `[H7_REMOVE] 0` could not be distinguished from "the helper was
            // never called". This is observation only: no shadow state is touched, and
            // the record deliberately says what it does NOT establish.
            ++m_removeOutOfScope;
            emitOrdinary(m_nRemoveOos, "[H7_REMOVE_OOS]",
                         "list=%08X entry=%08X — removal on a list this observer never "
                         "latched; NO membership was consulted or changed. Presence here "
                         "proves the helper ran; it says nothing about that entry",
                         listVA, entryVA);
            return;
        }

        bool complete = false;
        const bool stillPresent = listContains(listVA, entryVA, rd, complete);
        auto it = m_shadow.find(compositeKey(listVA, entryVA));

        if (it == m_shadow.end()) {
            emitOrdinary(m_nRemove, "[H7_REMOVE]",
                         "list=%08X entry=%08X — no shadow record for THIS list "
                         "(never censused as a member of it)", listVA, entryVA);
            return;
        }
        EntryRec& r = it->second;

        if (!complete) {
            // NOT left as-is. Keeping a stale `linked` here is exactly how a correctly
            // removed entry later produced a FALSE decisive [H7_DTOR_LINKED].
            const Membership was = r.member;
            r.member = Membership::Unknown;
            emitOrdinary(m_nRemove, "[H7_REMOVE]",
                         "list=%08X entry=%08X gen=%u name='%s' — post-call walk did not "
                         "complete; membership %s -> UNKNOWN (removal neither confirmed "
                         "nor denied)",
                         listVA, entryVA, r.gen, r.name, membershipName(was));
            return;
        }
        if (stillPresent) {
            r.member = Membership::Linked;
            emitOrdinary(m_nRemove, "[H7_REMOVE]",
                         "list=%08X entry=%08X gen=%u name='%s' — helper returned but the "
                         "value is STILL a member; not recording a removal",
                         listVA, entryVA, r.gen, r.name);
            return;
        }
        r.member = Membership::Absent;
        r.removed = true;
        emitOrdinary(m_nRemove, "[H7_REMOVE]",
                     "list=%08X entry=%08X gen=%u name='%s' state=%u prior=%u (confirmed absent)",
                     listVA, entryVA, r.gen, r.name, r.state, r.prior);
    }

    // ── The central destroy. Cached identity ONLY — the object is entering
    // destruction and must not be dereferenced here. Fires if the entry is still
    // linked in ANY latched list.
    void noteDestroy(uint32_t entryVA, const MemReader& rd)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto idx = m_byEntry.find(entryVA);
        if (idx != m_byEntry.end()) {
            // RE-CONFIRM AT THE DECISIVE MOMENT. Stored membership is a cache, and a
            // cache is not evidence: it may be Unknown, or Linked from a sweep that
            // predates a removal we could not confirm. [H7_DTOR_LINKED] is the tag
            // this whole probe exists to emit, so it fires ONLY on a walk that
            // completed AND found the entry, right now.
            //
            // Walking the list touches NODES only and compares their value words to
            // an address. It never dereferences the object entering destruction.
            //
            // ⚠ NO GUEST-SIDE LOCK. m_mu serialises this probe against itself; it does
            // NOT serialise against the guest. A concurrent mutation can produce a torn
            // observation, so "complete walk" means "walked to a null next without
            // hitting a refusal", not "observed an atomic snapshot". On the resolver
            // path the destroy happens inside stru_829A192C so the list is stable
            // there, but other callers of sub_826A3880 are NOT proven to hold it. A
            // stale-but-reachable node still holding this address would be read as
            // membership and would produce a FALSE linked free.
            bool anyConfirmedLinked = false, anyUnresolved = false;
            uint32_t linkedList = 0, unresolvedList = 0;
            for (uint32_t listVA : idx->second) {
                auto it = m_shadow.find(compositeKey(listVA, entryVA));
                if (it == m_shadow.end() || it->second.destroyed) continue;
                bool complete = false;
                const bool found = listContains(listVA, entryVA, rd, complete);
                if (!complete) {
                    it->second.member = Membership::Unknown;
                    if (!anyUnresolved) { anyUnresolved = true; unresolvedList = listVA; }
                    continue;
                }
                it->second.member = found ? Membership::Linked : Membership::Absent;
                if (found && !anyConfirmedLinked) { anyConfirmedLinked = true; linkedList = listVA; }
            }

            if (anyConfirmedLinked) {
                auto it = m_shadow.find(compositeKey(linkedList, entryVA));
                const EntryRec& r = it->second;
                if (!m_linkedFreeSeen) {
                    m_linkedFreeSeen = true;
                    emitRaw("[H7_DTOR_LINKED]",
                            "entry=%08X gen=%u name='%s' state=%u prior=%u list=%08X "
                            "removed=%d — DESTROYED WHILE STILL LINKED, membership "
                            "re-confirmed by a COMPLETE walk at destruction (cached identity)",
                            r.entryVA, r.gen, r.name, r.state, r.prior, r.listVA,
                            r.removed ? 1 : 0);
                }
                ++m_linkedFreeCount;
            } else if (anyUnresolved) {
                // Distinct, and deliberately NOT the decisive tag.
                ++m_linkedUnknownCount;
                if (!m_linkedUnknownSeen) {
                    m_linkedUnknownSeen = true;
                    auto it = m_shadow.find(compositeKey(unresolvedList, entryVA));
                    const char* nm = it != m_shadow.end() ? it->second.name : "";
                    emitRaw("[H7_LINKED_UNKNOWN]",
                            "entry=%08X list=%08X name='%s' — destroyed while membership "
                            "could NOT be re-confirmed (walk incomplete). This is NOT a "
                            "linked free and must not be counted as one",
                            entryVA, unresolvedList, nm);
                }
            }

            // Every tracked destruction gets a record, naming the reconfirmed result.
            // Without this the observer was silent for confirmed-ABSENT destruction and
            // for untracked objects, so "no destruction was observed with membership
            // linked or unresolved" read as "no resolver entry was destroyed at all" —
            // a claim I made and had to withdraw. The decisive tags above are unchanged;
            // this one is descriptive and carries no verdict.
            // m_byEntry is PERMANENT, so an address whose lifetime was destroyed and then
            // reused WITHOUT being re-censused still resolves here — and every one of its
            // records is a tombstone. Emitting then would describe an untracked lifetime
            // using the dead one's name, contradicting "untracked destructions stay
            // silent". Require at least one CURRENT membership, and take the name from a
            // current one: `front()` could pick a stale tombstone while the live record
            // sits in a later list.
            uint32_t currentLists = 0;
            const EntryRec* ident = nullptr;
            for (uint32_t listVA : idx->second) {
                auto itc = m_shadow.find(compositeKey(listVA, entryVA));
                if (itc == m_shadow.end() || itc->second.destroyed) continue;
                ++currentLists;
                if (!ident) ident = &itc->second;
            }
            if (ident) {
                const char* result = anyConfirmedLinked ? "linked"
                                   : (anyUnresolved ? "unknown" : "absent");
                emitOrdinary(m_nDestroy, "[H7_DESTROY]",
                             "entry=%08X name='%s' gen=%u currentLists=%u knownLists=%u "
                             "reconfirmed=%s (absent = walked and not found; unknown = "
                             "walk incomplete)",
                             entryVA, ident->name, ident->gen, currentLists,
                             unsigned(idx->second.size()), result);
            }

            // TOMBSTONE every membership record, do not erase. Dropping them would
            // make allocator reuse invisible: the next census would latch the same
            // address as a plain "new" entry and splice two lifetimes into one.
            for (uint32_t listVA : idx->second) {
                auto it = m_shadow.find(compositeKey(listVA, entryVA));
                if (it == m_shadow.end()) continue;
                it->second.destroyed = true;
            }
        }
        bumpGeneration(entryVA);
    }

    // ── One bounded event on the 6->5 executor path. ──────────────────────────
    // `firstTask` / `matchCount` come from an independent read-only walk of the SAME
    // task list the guest finder walks, so a second task holding the same clip is
    // observable rather than assumed away.
    struct ExecFacts {
        uint32_t list = 0, clip = 0, gate = 0;
        uint32_t firstTask = 0, firstTaskEntry = 0, firstTaskClip = 0;
        uint32_t matchCount = 0;
        uint32_t clip32 = 0, clip32_40 = 0;
        uint32_t entryStateBefore = 0xFFFFFFFFu, entryStateAfter = 0xFFFFFFFFu;
        uint32_t matchCountAfter = 0;
        bool     erased = false;
        bool     cycle = false;
        bool     valid = false;
    };

    void scanTaskList(uint32_t listHeadCellVA, uint32_t clip, const MemReader& rd, ExecFacts& f)
    {
        f.list = listHeadCellVA;
        f.clip = clip;
        f.matchCount = 0;
        f.firstTask = 0;
        f.firstTaskEntry = 0;
        f.firstTaskClip = 0;
        if (!validPtr(listHeadCellVA)) { anomalyBadPtrLocked("task list head", listHeadCellVA); return; }
        uint32_t node = 0;
        if (!rd.read32(rd.ud, listHeadCellVA, node)) { anomalyBadPtrLocked("task head deref", listHeadCellVA); return; }

        std::unordered_set<uint32_t> seen;
        uint32_t n = 0;
        while (node) {
            if (!validPtr(node)) { anomalyBadPtrLocked("task node", node); return; }
            if (++n > kMaxNodes) { f.cycle = true; anomalyCycleLocked("task list node budget", listHeadCellVA, n); return; }
            if (!seen.insert(node).second) { f.cycle = true; anomalyCycleLocked("task node revisit", node, n); return; }

            uint32_t task = 0;
            if (!rd.read32(rd.ud, node, task)) { anomalyBadPtrLocked("task node value", node); return; }
            if (task && validPtr(task)) {
                uint32_t taskClip = 0;
                if (rd.read32(rd.ud, task + 8u, taskClip) && taskClip == clip) {
                    if (f.matchCount == 0) {
                        f.firstTask = task;
                        rd.read32(rd.ud, task + 4u, f.firstTaskEntry);
                        f.firstTaskClip = taskClip;
                    }
                    ++f.matchCount;
                }
            }
            uint32_t next = 0;
            if (!rd.read32(rd.ud, node + 4u, next)) { anomalyBadPtrLocked("task node next", node + 4u); return; }
            node = next;
        }
        f.valid = true;
    }

    void emitExec(const ExecFacts& f)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (f.matchCount > 1 && !m_matchDupSeen) {
            m_matchDupSeen = true;   // once-only, cap-bypassing
            emitRaw("[H7_MATCHDUP]",
                    "clip=%08X list=%08X matchCount=%u — the finder returns the FIRST "
                    "match, so replace-before-insert does NOT imply global uniqueness",
                    f.clip, f.list, f.matchCount);
        }
        const RmDestroy* rmd = findRmd(f.clip);
        emitOrdinary(m_nExec, "[H7_EXEC6]",
                     "list=%08X clip=%08X gate=%08X task=%08X task+4=%08X task+8=%08X "
                     "clip+32=%08X *(clip+32)+40=%08X entryState %s->%s match=%u->%u "
                     "erased=%d eqTaskEntry_vs_clip3240=%d rmdBefore=%s rmdAfter=%s "
                     "walk=%s",
                     f.list, f.clip, f.gate, f.firstTask, f.firstTaskEntry, f.firstTaskClip,
                     f.clip32, f.clip32_40,
                     u32s(f.entryStateBefore).c_str(), u32s(f.entryStateAfter).c_str(),
                     f.matchCount, f.matchCountAfter, f.erased ? 1 : 0,
                     (f.firstTaskEntry && f.firstTaskEntry == f.clip32_40) ? 1 : 0,
                     rmd ? u32s(rmd->before).c_str() : "n/a",
                     rmd ? u32s(rmd->after).c_str() : "n/a",
                     f.valid ? "complete" : (f.cycle ? "CYCLE" : "REFUSED"));
    }

    void noteRemoveAndDestroy(uint32_t clip, uint32_t clip32, uint32_t typeWord,
                              uint32_t slot, uint32_t before, uint32_t after)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_rmd.find(clip) == m_rmd.end() && m_rmd.size() >= kMaxRmd) {
            capOnce(m_capRmdSeen, "RemoveAndDestroy identities", kMaxRmd,
                    "further clips get rmdBefore/rmdAfter = n/a, which is NOT 'no state change'");
        } else {
            m_rmd[clip] = RmDestroy{before, after};
        }
        // RAW CANDIDATE CHAIN, not a typed entry, and NOT AN ATTRIBUTION. The observer
        // follows clip -> +32 -> +40 on EVERY call, whereas the guest's state-5 store at
        // 0x826AC6C8 sits behind several gates. This record therefore reports two
        // INDEPENDENT facts and draws no conclusion from either: `typeEligible` (is the
        // type tag 9, as the writer's gate requires?) and `observedTo5` (did w8 change to
        // 5 across the call?). A non-enum w8 on a non-eligible candidate is expected, not
        // anomalous, and is NOT evidence against the statically proven writer path.
        // (I previously reported exactly that as a counterexample; it was not one.)
        // TWO INDEPENDENT FACTS, never merged into one verdict (director blocker 4).
        // The generated PPC gates the writer on type tag 9, so a 6->5 transition on a
        // NON-type-9 candidate is not that writer — the previous single wroteState5 field
        // would have printed YES for it and attributed it anyway.
        const bool typeKnown = (typeWord != 0xFFFFFFFFu);
        const char* typeEligible = !typeKnown ? "n/a" : ((typeWord >> 26) == 9u ? "yes" : "no");
        const bool stateKnown = (before != 0xFFFFFFFFu && after != 0xFFFFFFFFu);
        const char* observedTo5 = !stateKnown ? "n/a"
                                : ((before != 5u && after == 5u) ? "yes" : "no");
        emitOrdinary(m_nRmd, "[H7_RMDESTROY]",
                     "clip=%08X clip+32=%08X typeWord=%08X typeTag=%s typeEligible=%s "
                     "cand=*(clip+32)+40=%08X w8 %s -> %s observedTo5=%s — RAW CHAIN, "
                     "followed unconditionally. NEITHER field alone proves the gated "
                     "writer ran; the writer is gated on type tag 9",
                     clip, clip32, typeWord,
                     u32s(typeKnown ? (typeWord >> 26) : 0xFFFFFFFFu).c_str(), typeEligible,
                     slot, u32s(before).c_str(), u32s(after).c_str(), observedTo5);
    }

    // ── H7c record observer, keyed on the DERIVED RECORD ARRAY. A FIRST sighting emits
    // [H7_PARSEFIRST] (it used to be silent, which made "the bridge never fired" and
    // "every parse was a distinct identity" the same log); a REPEAT prints
    // first-vs-current so a cache hit re-driving delivery is visible as a fact rather
    // than an inference. No name filtering.
    // Every parser-hook call classifies as EXACTLY ONE of these, and each has its own
    // tag and its own cap. Previously a refusal printed [H7_PARSEREP] through the repeat
    // counter, so refusals were indistinguishable from repeats AND consumed the repeat
    // cap; observer-capacity refusal emitted only [H7_CAP] and so was not a parser
    // outcome at all.
    enum class ParseOutcome { First, Repeat, Refuse };

    ParseOutcome noteParseOutcome(const ParseArgs& p, bool derivedOk)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        ++m_parseCalls;
        if (!derivedOk) {
            ++m_parseRefuse;
            emitOrdinary(m_nParseRefuse, "[H7_PARSEREFUSE]",
                         "blob=%08X header=%08X node=%08X — %s",
                         p.blob, p.header, p.node, p.refusal);
            return ParseOutcome::Refuse;
        }
        auto it = m_parse.find(p.recordArray);
        if (it == m_parse.end()) {
            if (m_parse.size() >= kMaxParse) {
                capOnce(m_capParseSeen, "H7c record-array identities", kMaxParse,
                        "further first-sightings are not tracked, so a later repeat "
                        "would be missed entirely");
                // Observer capacity is a REFUSAL, not an absence. Reporting only the
                // capacity notice made this call vanish from the parser outcome census.
                ++m_parseRefuse;
                emitOrdinary(m_nParseRefuse, "[H7_PARSEREFUSE]",
                             "recordArray=%08X — observer capacity: identity NOT tracked, "
                             "so a later repeat of it cannot be recognised",
                             p.recordArray);
                return ParseOutcome::Refuse;
            }
            m_parse[p.recordArray] = ParseRec{p.blob, p.header, p.node, p.field20, p.record0, 1};
            // A silent first sighting made "the bridge never fired" and "every parse was a
            // distinct first identity" produce IDENTICAL logs. The iter-455j roll could
            // only tell them apart by borrowing a count from [PARSE_TRACE], which is
            // capped and sampled — i.e. by reading another instrument past what it
            // measures. This capped record is the bridge's own positive witness.
            emitOrdinary(m_nParseFirst, "[H7_PARSEFIRST]",
                         "recordArray=%08X blob=%08X header=%08X node=%08X hdr+20=%08X "
                         "record[0]=%08X (first sighting of this identity)",
                         p.recordArray, p.blob, p.header, p.node, p.field20, p.record0);
            ++m_parseFirst;
            return ParseOutcome::First;
        }
        ParseRec& q = it->second;
        ++q.sightings;
        ++m_parseRepeat;
        emitOrdinary(m_nParseRep, "[H7_PARSEREP]",
                     "recordArray=%08X sighting=%u blob %08X->%08X header %08X->%08X "
                     "node %08X->%08X hdr+20 %08X->%08X record[0] %08X->%08X %s",
                     p.recordArray, q.sightings, q.firstBlob, p.blob, q.firstHeader, p.header,
                     q.firstNode, p.node, q.firstField20, p.field20, q.firstRecord0, p.record0,
                     (q.firstBlob == p.blob && q.firstHeader == p.header && q.firstNode == p.node)
                         ? "(same source)" : "(SOURCE CHANGED)");
        return ParseOutcome::Repeat;
    }

    // ── test/diagnostic accessors ─────────────────────────────────────────────
    size_t   shadowSize()        { std::lock_guard<std::mutex> lk(m_mu); return m_shadow.size(); }
    size_t   knownListCount()    { std::lock_guard<std::mutex> lk(m_mu); return m_knownLists.size(); }
    uint32_t generationOf(uint32_t va) { std::lock_guard<std::mutex> lk(m_mu); return genOfLocked(va); }
    uint32_t linkedFreeCount()   { std::lock_guard<std::mutex> lk(m_mu); return m_linkedFreeCount; }
    uint32_t linkedUnknownCount(){ std::lock_guard<std::mutex> lk(m_mu); return m_linkedUnknownCount; }
    size_t   genTrackedCount()   { std::lock_guard<std::mutex> lk(m_mu); return m_gen.size(); }
    // The containment that makes the generation cap unreachable. Exposed so a test can
    // PIN it instead of relying on the structural single-writer argument, which stops
    // being true the moment someone adds a second writer.
    bool generationOfIsSubsetOfIndexed()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        for (const auto& kv : m_gen) if (!m_byEntry.count(kv.first)) return false;
        return true;
    }
    uint32_t removeOutOfScope()  { std::lock_guard<std::mutex> lk(m_mu); return m_removeOutOfScope; }
    uint32_t partialCount()      { std::lock_guard<std::mutex> lk(m_mu); return m_partialCount; }
    void     setCensusCapForTest(uint32_t c) { std::lock_guard<std::mutex> lk(m_mu); m_censusCap = c; }
    void     setGenCapForTest(uint32_t c)    { std::lock_guard<std::mutex> lk(m_mu); m_genCap = c; }
    uint32_t reuseCount()        { std::lock_guard<std::mutex> lk(m_mu); return m_reuseCount; }
    uint32_t parseCalls()        { std::lock_guard<std::mutex> lk(m_mu); return m_parseCalls; }
    uint32_t parseFirstCount()   { std::lock_guard<std::mutex> lk(m_mu); return m_parseFirst; }
    uint32_t parseRepeatCount()  { std::lock_guard<std::mutex> lk(m_mu); return m_parseRepeat; }
    uint32_t parseRefuseCount()  { std::lock_guard<std::mutex> lk(m_mu); return m_parseRefuse; }
    uint32_t reuseUnknownCount() { std::lock_guard<std::mutex> lk(m_mu); return m_reuseUnknownCount; }

    bool hasShadow(uint32_t listVA, uint32_t entryVA)
    { std::lock_guard<std::mutex> lk(m_mu); return m_shadow.count(compositeKey(listVA, entryVA)) != 0; }
    Membership membershipOf(uint32_t listVA, uint32_t entryVA)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_shadow.find(compositeKey(listVA, entryVA));
        return it == m_shadow.end() ? Membership::Unknown : it->second.member;
    }
    bool isLinked(uint32_t listVA, uint32_t entryVA)
    { return membershipOf(listVA, entryVA) == Membership::Linked; }
    bool isLive(uint32_t listVA, uint32_t entryVA)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_shadow.find(compositeKey(listVA, entryVA));
        return it != m_shadow.end() && !it->second.destroyed;
    }
    uint32_t shadowGen(uint32_t listVA, uint32_t entryVA)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_shadow.find(compositeKey(listVA, entryVA));
        return it == m_shadow.end() ? 0u : it->second.gen;
    }

private:
    struct RmDestroy { uint32_t before, after; };
    struct ParseRec  { uint32_t firstBlob, firstHeader, firstNode, firstField20, firstRecord0, sightings; };

    static std::string u32s(uint32_t v)
    {
        if (v == 0xFFFFFFFFu) return "n/a";
        char b[16]; std::snprintf(b, sizeof(b), "%u", v); return b;
    }

    const RmDestroy* findRmd(uint32_t clip) const
    {
        auto it = m_rmd.find(clip);
        return it == m_rmd.end() ? nullptr : &it->second;
    }

    // Bounded membership test. `complete` is false if the walk could not be finished,
    // in which case the answer is unknown rather than negative.
    bool listContains(uint32_t listHeadCellVA, uint32_t value, const MemReader& rd, bool& complete)
    {
        complete = false;
        if (!validPtr(listHeadCellVA)) return false;
        uint32_t node = 0;
        if (!rd.read32(rd.ud, listHeadCellVA, node)) return false;
        std::unordered_set<uint32_t> seen;
        uint32_t n = 0;
        while (node) {
            if (!validPtr(node) || ++n > kMaxNodes || !seen.insert(node).second) return false;
            uint32_t v = 0;
            if (!rd.read32(rd.ud, node, v)) return false;
            if (v == value) { complete = true; return true; }
            if (!rd.read32(rd.ud, node + 4u, node)) return false;
        }
        complete = true;
        return false;
    }

    // 0 is never a valid generation. It means "this address's lifetime history is NOT
    // supported" — as opposed to 1, which means "first lifetime". Conflating the two
    // is what let a tombstone be retained as the same object past the cap.
    static constexpr uint32_t kGenUntracked = 0;

    uint32_t genOfLocked(uint32_t va) const
    {
        auto it = m_gen.find(va);
        if (it != m_gen.end()) return it->second;
        // Before the cap, an unseen address is genuinely on its first lifetime. After
        // it, we simply do not know — and saying 1 would be a claim, not a default.
        return m_capGenSeen ? kGenUntracked : 1u;
    }

    void bumpGeneration(uint32_t va)
    {
        // Generation capacity is spent ONLY on addresses we actually track. A destroy
        // of an address that was never censused carries no lifetime history worth
        // keeping, and letting those consume the budget is what made the cap
        // reachable at all. With this, m_gen is bounded by the number of distinct
        // tracked entries, which is itself bounded by kMaxShadow — so the cap below
        // is a backstop, not an operating condition.
        if (!m_byEntry.count(va)) return;
        if (m_gen.size() >= m_genCap && !m_gen.count(va)) {
            // NOTE: deliberately does NOT name the uncertainty tag in brackets. A
            // message that embeds another tag's literal makes every count of that tag
            // wrong — which is exactly how this test first failed.
            capOnce(m_capGenSeen, "allocation generations", m_genCap,
                    "further addresses have UNTRACKED lifetime history; their reuse is "
                    "reported as an explicit uncertainty event, never silently merged");
            return;
        }
        m_gen[va] = genOfLocked(va) + 1u;
    }

    void indexEntry(uint32_t entryVA, uint32_t listVA)
    {
        auto& v = m_byEntry[entryVA];
        for (uint32_t l : v) if (l == listVA) return;
        v.push_back(listVA);
    }

    // Caller holds m_mu.
    void observeEntry(uint32_t listVA, uint32_t entryVA, const MemReader& rd)
    {
        uint32_t refc = 0, state = 0, prior = 0, nameVA = 0;
        if (!rd.read32(rd.ud, entryVA + 0u,  refc)  ||
            !rd.read32(rd.ud, entryVA + 4u,  nameVA) ||
            !rd.read32(rd.ud, entryVA + 8u,  state) ||
            !rd.read32(rd.ud, entryVA + 12u, prior)) {
            anomalyBadPtr("resolver entry fields", entryVA);
            return;
        }

        const uint64_t k = compositeKey(listVA, entryVA);
        const uint32_t curGen = genOfLocked(entryVA);
        auto it = m_shadow.find(k);

        if (it != m_shadow.end()) {
            // Two independent signals that this address now holds a DIFFERENT object:
            //   TOMBSTONE   - we OBSERVED the destroy. This is the reliable one, and it
            //                 does not depend on generation capacity at all. It is what
            //                 makes the splice impossible past the cap.
            //   GEN MISMATCH- a destroy we did not observe, inferred from the counter.
            //                 Only meaningful when BOTH generations are tracked, since
            //                 kGenUntracked means "no lifetime history", not "gen 0".
            const bool tombstone = it->second.destroyed;
            const bool bothTracked = (curGen != kGenUntracked) && (it->second.gen != kGenUntracked);
            const bool genMismatch = bothTracked && (it->second.gen != curGen);

            if (tombstone || genMismatch) {
                // Never retain a tombstone as the same lifetime. If the generation
                // counter can corroborate it, say so decisively; if it cannot, say
                // THAT — and start a fresh record either way.
                const bool supported = bothTracked && genMismatch;
                if (supported) {
                    if (!m_reuseSeen) {
                        m_reuseSeen = true;
                        emitRaw("[H7_REUSE]",
                                "entry=%08X list=%08X gen %u->%u — allocator reuse; prior "
                                "object ('%s', state %u, destroyed=%d) was a DIFFERENT "
                                "lifetime",
                                entryVA, listVA, it->second.gen, curGen, it->second.name,
                                it->second.state, it->second.destroyed ? 1 : 0);
                    }
                    ++m_reuseCount;
                } else {
                    ++m_reuseUnknownCount;
                    if (!m_reuseUnknownSeen) {
                        m_reuseUnknownSeen = true;
                        emitRaw("[H7_REUSE_UNKNOWN]",
                                "entry=%08X list=%08X — address re-censused after an "
                                "OBSERVED destroy, but its lifetime history is untracked "
                                "(gen %u vs %u; 0 = unsupported). Starting a FRESH record: "
                                "the prior object ('%s', state %u) is NOT assumed to be "
                                "this one",
                                entryVA, listVA, it->second.gen, curGen, it->second.name,
                                it->second.state);
                    }
                }
                m_shadow.erase(it);
                it = m_shadow.end();
            }
        }

        if (it == m_shadow.end()) {
            if (m_shadow.size() >= kMaxShadow) {
                capOnce(m_capShadowSeen, "membership records", kMaxShadow,
                        "further entries are NOT tracked; their destruction cannot be "
                        "reported as linked");
                return;
            }
            EntryRec r;
            r.listVA = listVA; r.entryVA = entryVA; r.gen = curGen;
            r.nameVA = nameVA; r.state = state; r.prior = prior; r.refcount = refc;
            r.member = Membership::Linked;   // positively observed in the walk
            cacheName(nameVA, r.name, rd);
            m_shadow[k] = r;
            indexEntry(entryVA, listVA);
            emitOrdinary(m_nResolve, "[H7_RESOLVE]",
                         "new  list=%08X entry=%08X gen=%u name='%s' state=%u prior=%u rc=%u",
                         listVA, entryVA, curGen, r.name, state, prior, refc);
            return;
        }

        EntryRec& r = it->second;
        const bool changed = (r.state != state) || (r.prior != prior) ||
                             (r.member != Membership::Linked);
        r.refcount = refc;
        if (r.nameVA != nameVA) { r.nameVA = nameVA; cacheName(nameVA, r.name, rd); }
        if (changed) {
            emitOrdinary(m_nResolve, "[H7_RESOLVE]",
                         "chg  list=%08X entry=%08X gen=%u name='%s' state %u->%u "
                         "prior %u->%u rc=%u member %s->linked",
                         listVA, entryVA, r.gen, r.name, r.state, state, r.prior, prior, refc,
                         membershipName(r.member));
            r.state = state; r.prior = prior;
        }
        r.member = Membership::Linked;   // positively observed in this walk
    }

    void cacheName(uint32_t nameVA, char* out, const MemReader& rd)
    {
        out[0] = 0;
        if (!validPtr(nameVA)) return;
        uint8_t lenb[4] = {};
        if (!rd.readBytes(rd.ud, nameVA, lenb, 4)) return;
        uint32_t len = (uint32_t(lenb[2]) << 8) | uint32_t(lenb[3]);   // big-endian u16 at +2
        if (len > kNameChars) len = kNameChars;
        if (!len) return;
        uint8_t buf[kNameChars + 1] = {};
        if (!rd.readBytes(rd.ud, nameVA + 8u, buf, len)) return;
        uint32_t w = 0;
        for (uint32_t i = 0; i < len; ++i) {
            const uint8_t c = buf[i];
            out[w++] = (c >= 32 && c < 127) ? char(c) : '.';
        }
        out[w] = 0;
    }

    void emitOrdinary(std::atomic<uint32_t>& counter, const char* cls, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 4, 5)))
#endif
    ;

    void emitRaw(const char* cls, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 3, 4)))
#endif
    ;

    // The single sanitizing sink. Everything that reaches the log goes through it.
    void emitLine(const char* cls, const char* raw);

public:
    // Test-only probes that drive a CRAFTED message through the real production emit
    // paths. Test 27 previously called sanitizeEmbeddedTags() directly, so deleting the
    // production call sites would have left it green - it tested the function, not the
    // path. These exist so the test can prove the PATH sanitizes.
    void emitOrdinaryProbeForTest(const char* cls, const char* raw)
    { emitOrdinary(m_nTestProbe, cls, "%s", raw); }
    void emitRawProbeForTest(const char* cls, const char* raw)
    { emitRaw(cls, "%s", raw); }

private:

    // A capacity refusal is NOT a malformed structure. Keeping these under their own
    // tag is the difference between "the list is broken" and "I stopped looking".
    void capOnce(bool& seen, const char* what, uint32_t cap, const char* consequence)
    {
        if (seen) return;
        seen = true;
        emitRaw("[H7_CAP]", "%s cap %u reached — %s", what, cap, consequence);
    }

    void anomalyBadPtr(const char* what, uint32_t va)
    {
        ++m_badPtrCount;
        if (m_badPtrSeen) return;
        m_badPtrSeen = true;
        emitRaw("[H7_BADPTR]", "refused %s va=%08X (unmapped or misaligned)", what, va);
    }
    void anomalyCycle(const char* what, uint32_t va, uint32_t n)
    {
        if (m_cycleSeen) return;
        m_cycleSeen = true;
        emitRaw("[H7_CYCLE]", "%s at %08X after %u nodes — traversal ABORTED", what, va, n);
    }
    // scanTaskList runs without m_mu (it only touches locals); these take it.
    void anomalyBadPtrLocked(const char* what, uint32_t va)
    { std::lock_guard<std::mutex> lk(m_mu); anomalyBadPtr(what, va); }
    void anomalyCycleLocked(const char* what, uint32_t va, uint32_t n)
    { std::lock_guard<std::mutex> lk(m_mu); anomalyCycle(what, va, n); }

    Sink m_sink;
    std::mutex m_mu;
    std::unordered_map<uint64_t, EntryRec>              m_shadow;    // (list,entry) -> record
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_byEntry;   // entry -> lists
    std::unordered_map<uint32_t, uint32_t>              m_gen;
    std::unordered_map<uint32_t, RmDestroy>             m_rmd;
    std::unordered_map<uint32_t, ParseRec>              m_parse;
    std::unordered_set<uint32_t>                        m_knownLists;

    std::atomic<uint32_t> m_nResolve{0}, m_nRemove{0}, m_nExec{0}, m_nRmd{0}, m_nParse{0};
    // One counter per parser outcome, so the caps are independent: a flood of refusals
    // can no longer consume the budget a repeat would have used, and vice versa.
    std::atomic<uint32_t> m_nParseFirst{0}, m_nParseRep{0}, m_nParseRefuse{0};
    std::atomic<uint32_t> m_nRemoveOos{0}, m_nDestroy{0}, m_nTestProbe{0};
    uint32_t m_parseCalls = 0, m_parseFirst = 0, m_parseRepeat = 0, m_parseRefuse = 0;
    uint32_t m_linkedFreeCount = 0;
    uint32_t m_linkedUnknownCount = 0;
    uint32_t m_reuseCount = 0;
    uint32_t m_reuseUnknownCount = 0;
    uint32_t m_removeOutOfScope = 0;
    uint32_t m_badPtrCount = 0;
    uint32_t m_partialCount = 0;
    uint32_t m_censusCalls = 0;
    // Held as a member rather than used as a constant so the self-test can actually
    // exercise the cap instead of asserting it exists.
    uint32_t m_censusCap = kMaxCensusCalls;
    // Same reason as the census cap: held as a member so the self-test can drive the
    // unsupported-lifetime path. With bumpGeneration spending capacity only on tracked
    // identities, m_gen is bounded by distinct tracked entries (<= kMaxShadow), so the
    // real cap is structurally unreachable — which is exactly why it needs an injected
    // one to be tested at all rather than declared unreachable and left alone.
    uint32_t m_genCap = kMaxGenTracked;
    bool m_linkedFreeSeen = false;
    bool m_linkedUnknownSeen = false;
    bool m_matchDupSeen   = false;
    bool m_cycleSeen      = false;
    bool m_partialSeen    = false;
    bool m_badPtrSeen     = false;
    bool m_reuseSeen      = false;
    bool m_reuseUnknownSeen = false;
    bool m_capShadowSeen  = false;
    bool m_capListsSeen   = false;
    bool m_capParseSeen   = false;
    bool m_capRmdSeen     = false;
    bool m_capGenSeen     = false;
    bool m_capCensusSeen  = false;
};

// ── THE PRODUCTION-LOG GUARANTEE (director blocker 3) ─────────────────────────
// The log line is `cls + " " + msg`, so a `[H7_*]` token inside `msg` is
// indistinguishable from a second event to `rg`, `grep`, or any regex census of the
// RAW log. `Capture::countTag()` fixed only the synthetic suite - and by converting
// every assertion to it, it actively HID this regression from the tests. The class is
// only fixed if the rendered message cannot carry a second class token at all.
//
// This neutralises any such token in place: `[H7_FOO]` becomes `(H7_FOO)`. The
// information survives for a human reader; the tokenisation does not. It runs on every
// rendered message, so a future edit that names a tag in prose cannot corrupt that
// tag's count in the shipped log.
void sanitizeEmbeddedTags(char* msg)
{
    for (char* p = msg; *p; ++p) {
        if (p[0] != '[' || p[1] != 'H' || p[2] != '7' || p[3] != '_') continue;
        char* close = nullptr;
        for (char* q = p + 4; *q; ++q) {
            if (*q == ']') { close = q; break; }
            const bool ok = (*q >= 'A' && *q <= 'Z') || *q == '_' ||
                            (*q >= '0' && *q <= '9');
            if (!ok) break;
        }
        // Neutralise the OPENING bracket UNCONDITIONALLY. Bailing out when no
        // well-formed close was found left the literal "[H7_" in the line, which a raw
        // `rg -c "\[H7_"` counts as an event. That is reachable two ways: vsnprintf
        // truncating a long message mid-token at the buffer boundary, and any non-charset
        // content inside the brackets. A dangling "(H7_LINKED" at a truncation point is
        // cosmetically odd but cannot corrupt a census, which is the property that matters.
        *p = '(';
        if (close) { *close = ')'; p = close; }
    }
}

// THE SINGLE SINK for Watch events. Every [H7_*] EVENT leaves through here and is
// sanitized on the way, so
// the guarantee is structural rather than "true of the paths I remembered". The
// ordinary-cap announcement used to call the sink directly with a literal: safe by
// accident, and one careless edit away from breaking a census silently.
// The sink call below must remain the ONLY one in this file - and this comment
// deliberately does not repeat the token, so grepping for the call site stays
// unambiguous. Prose that names the thing being counted is the same confound as a
// log message that names another tag.
void Watch::emitLine(const char* cls, const char* raw)
{
    char msg[640];
    std::snprintf(msg, sizeof(msg), "%s", raw);
    sanitizeEmbeddedTags(msg);
    m_sink.emit(m_sink.ud, cls, msg);
}

void Watch::emitOrdinary(std::atomic<uint32_t>& counter, const char* cls, const char* fmt, ...)
{
    const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > kOrdinaryCap) {
        if (n == kOrdinaryCap + 1)
            emitLine(cls, "output cap reached; further ordinary events of this "
                          "class suppressed (anomaly classes are NOT suppressed)");
        return;
    }
    char msg[640];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emitLine(cls, msg);
}

void Watch::emitRaw(const char* cls, const char* fmt, ...)
{
    char msg[640];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emitLine(cls, msg);
}

// ── Gate + singletons ─────────────────────────────────────────────────────────
bool enabled()
{
    static const bool on = [] {
        const char* e = std::getenv("SR_H7_ENTRYWATCH");
        return e && e[0] == '1';   // DEFAULT OFF, opt-in only
    }();
    return on;
}

void logSink(void*, const char* cls, const char* msg)
{
    SR_LOG_BOOT(sr::LogSeverity::Info, "%s %s", cls, msg);
}

Watch& watch()
{
    static Watch w{Sink{&logSink, nullptr}};
    return w;
}

// ── The live guest reader. Every access is preceded by a range and alignment check,
// and isGuestVAInRange is a range check rather than a fault-and-catch — the harness VEH
// makes any host AV fatal before SEH filters run, so a diagnostic that faults kills the
// run it is measuring.
// ⚠ That is the presence of checks, NOT a demonstration of fault-freedom. No fault
// injection has been run against these paths, so "fault-free by construction" — which
// this comment used to claim — is not supported and is not claimed.
bool guestRead32(void*, uint32_t va, uint32_t& out)
{
    if (!validPtrVA(va)) return false;
    if (!sr::kernel::isGuestVAInRange(va) || !sr::kernel::isGuestVAInRange(va + 3u)) return false;
    out = sr::kernel::readGuestU32BE(va);
    return true;
}

bool guestReadBytes(void*, uint32_t va, uint8_t* dst, uint32_t n)
{
    if (!n || va < 0x10000u || va >= 0xC0000000u) return false;
    if (!sr::kernel::isGuestVAInRange(va) || !sr::kernel::isGuestVAInRange(va + n - 1u)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t w = sr::kernel::readGuestU32BE((va + i) & ~3u);
        dst[i] = uint8_t(w >> (24 - 8 * ((va + i) & 3u)));
    }
    return true;
}

MemReader guestReader() { return MemReader{&guestRead32, &guestReadBytes, nullptr}; }

// ── The resolver wrapper body, factored out so it is TESTABLE. ────────────────
// `sub_826AFA58` ends in `return sub_825F6B90(&stru_829A192C);`, so the guest's own
// return value lands in r3 and the incoming list argument is GONE by the time the
// delegate returns. The previous candidate read ctx.r3 again for the post-call
// census and therefore censused whatever the critical-section release happened to
// return. The list is latched once, before delegation, and both censuses use it.
template <class Delegate>
inline void resolverWrapperBody(Watch& w, const MemReader& rd, uint32_t& r3, Delegate&& delegate)
{
    const uint32_t listVA = r3;   // latch BEFORE the delegate can clobber r3
    w.censusResolverList(listVA, rd);
    delegate();
    w.censusResolverList(listVA, rd);
}

// ── Item 9 bridge. sub_826AE6D0 already carries a strong definition in
// apt_parse_trace.cpp; a second one would not link, so the parser hook calls this.
// Arguments are r5/r6/r7 — see the VERIFIED mapping in the header comment. The first
// candidate passed r4 and r6 here and watched the wrong object entirely.
void noteParseRecordFromParser(uint32_t blobVA, uint32_t headerVA, uint32_t nodeVA)
{
    if (!enabled()) return;
    const MemReader rd = guestReader();
    ParseArgs p;
    const bool ok = deriveParseArgs(blobVA, headerVA, nodeVA, rd, p);
    watch().noteParseOutcome(p, ok);   // classifies as exactly one of FIRST/REPEAT/REFUSE
}

}  // namespace sr::kernel::h7

// ── Hooks ─────────────────────────────────────────────────────────────────────
// Every one delegates unconditionally. With the gate OFF each is a direct tail call
// into __imp__ and observes nothing.

extern "C" void __imp__sub_826AFA58(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_826AFA58)
{
    using namespace sr::kernel::h7;
    if (!enabled()) { __imp__sub_826AFA58(ctx, base); return; }
    // The list comes from the ARGUMENT, latched once. 0x829A17BC is never read here.
    const MemReader rd = guestReader();
    uint32_t r3 = ctx.r3.u32;
    resolverWrapperBody(watch(), rd, r3, [&] { __imp__sub_826AFA58(ctx, base); });
}

extern "C" void __imp__sub_8269B540(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8269B540)
{
    using namespace sr::kernel::h7;
    const uint32_t listVA = ctx.r3.u32, entryVA = ctx.r4.u32;   // latch before the call
    __imp__sub_8269B540(ctx, base);
    // AFTER delegation: a pre-call update would report a removal the helper might not
    // perform. Scoped to the exact (list, entry) record and re-checked for absence.
    if (enabled()) {
        const MemReader rd = guestReader();
        watch().noteRemoveByValueAfter(listVA, entryVA, rd);
    }
}

extern "C" void __imp__sub_826A3880(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_826A3880)
{
    using namespace sr::kernel::h7;
    // Cached identity only. The object is entering destruction; this hook
    // deliberately does not read a single field of IT — the re-confirmation walk
    // touches list nodes and compares their value words to this address.
    if (enabled()) {
        const MemReader rd = guestReader();
        watch().noteDestroy(ctx.r3.u32, rd);
    }
    __imp__sub_826A3880(ctx, base);
}

extern "C" void __imp__sub_826AC080(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_826AC080)
{
    using namespace sr::kernel::h7;
    uint32_t clip = 0, c32 = 0, typeWord = 0xFFFFFFFFu, slot = 0, before = 0xFFFFFFFFu;
    const bool watching = enabled();
    if (watching) {
        const MemReader rd = guestReader();
        clip = ctx.r3.u32;
        if (Watch::validPtr(clip) && rd.read32(rd.ud, clip + 32u, c32) && Watch::validPtr(c32)) {
            // *(clip+32)+8 is the TYPE WORD whose top bits gate whether +40 is a
            // state-carrying entry ((word >> 26) == 9 at sub_826B7378). Not logging it
            // is what made the iter-455j RMDESTROY records undecidable.
            rd.read32(rd.ud, c32 + 8u, typeWord);
            if (rd.read32(rd.ud, c32 + 40u, slot) && Watch::validPtr(slot))
                rd.read32(rd.ud, slot + 8u, before);
        }
    }
    __imp__sub_826AC080(ctx, base);
    if (watching) {
        const MemReader rd = guestReader();
        uint32_t after = 0xFFFFFFFFu;
        if (Watch::validPtr(slot)) rd.read32(rd.ud, slot + 8u, after);
        watch().noteRemoveAndDestroy(clip, c32, typeWord, slot, before, after);
    }
}

extern "C" void __imp__sub_826B0960(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_826B0960)
{
    using namespace sr::kernel::h7;
    Watch::ExecFacts f;
    const bool watching = enabled();
    if (watching) {
        const MemReader rd = guestReader();
        const uint32_t list = ctx.r3.u32, clip = ctx.r4.u32;
        watch().scanTaskList(list, clip, rd, f);
        // The guest's own gate: the finder+erase only runs when
        // (*(clip+12) & 0x60000000) == 0x20000000.
        if (Watch::validPtr(clip)) {
            rd.read32(rd.ud, clip + 12u, f.gate);
            if (rd.read32(rd.ud, clip + 32u, f.clip32) && Watch::validPtr(f.clip32))
                rd.read32(rd.ud, f.clip32 + 40u, f.clip32_40);
        }
        if (Watch::validPtr(f.firstTaskEntry))
            rd.read32(rd.ud, f.firstTaskEntry + 8u, f.entryStateBefore);
    }
    __imp__sub_826B0960(ctx, base);
    if (watching) {
        const MemReader rd = guestReader();
        Watch::ExecFacts after;
        watch().scanTaskList(f.list, f.clip, rd, after);
        f.matchCountAfter = after.matchCount;
        // AptListNodeErase sits INSIDE the `!= 5` guard, so an entry already at 5
        // leaves the node linked. That difference is the observation. Only a pair of
        // COMPLETE walks can support an erasure claim.
        f.erased = (f.valid && after.valid && f.matchCount > 0 && after.matchCount < f.matchCount);
        if (Watch::validPtr(f.firstTaskEntry))
            rd.read32(rd.ud, f.firstTaskEntry + 8u, f.entryStateAfter);
        watch().emitExec(f);
    }
}

// ── Host-side self-test (SR_H7_ENTRYWATCH_TEST=1) ─────────────────────────────
namespace sr::kernel::h7 {
namespace test {

struct FakeMem {
    std::unordered_map<uint32_t, uint32_t> w;   // VA -> big-endian-decoded word
    void put(uint32_t va, uint32_t v) { w[va] = v; }
};

bool fakeRead32(void* ud, uint32_t va, uint32_t& out)
{
    if (!validPtrVA(va)) return false;
    auto* m = static_cast<FakeMem*>(ud);
    auto it = m->w.find(va);
    if (it == m->w.end()) return false;
    out = it->second;
    return true;
}
bool fakeReadBytes(void* ud, uint32_t va, uint8_t* dst, uint32_t n)
{
    auto* m = static_cast<FakeMem*>(ud);
    for (uint32_t i = 0; i < n; ++i) {
        auto it = m->w.find((va + i) & ~3u);
        if (it == m->w.end()) return false;
        dst[i] = uint8_t(it->second >> (24 - 8 * ((va + i) & 3u)));
    }
    return true;
}

struct Capture {
    std::vector<std::string> lines;
    static void emit(void* ud, const char* cls, const char* msg)
    { static_cast<Capture*>(ud)->lines.push_back(std::string(cls) + " " + msg); }
    bool has(const char* needle) const
    {
        for (const auto& l : lines) if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    size_t count(const char* needle) const
    {
        size_t n = 0;
        for (const auto& l : lines) if (l.find(needle) != std::string::npos) ++n;
        return n;
    }
    // Counts a TAG. Capture::emit builds "<cls> <msg>", so a tag is only ever a tag at
    // offset 0; anywhere else it is prose. Substring counting conflated the two and
    // silently inflated tag counts whenever one message named another tag.
    size_t countTag(const char* tag) const
    {
        size_t n = 0;
        for (const auto& l : lines) if (l.rfind(tag, 0) == 0) ++n;
        return n;
    }
    // Lines of ONE class containing a needle. Counting a bare needle across every class
    // conflated [H7_DESTROY] with [H7_RESOLVE] - both print name='...'.
    size_t countTagWith(const char* tag, const char* needle) const
    {
        size_t n = 0;
        for (const auto& l : lines)
            if (l.rfind(tag, 0) == 0 && l.find(needle) != std::string::npos) ++n;
        return n;
    }
};

// A resolver list of `count` entries: 8-byte nodes from nodeBase, entries at
// entryBase + i*0x20, all at `state`, no name.
void buildResolverList(FakeMem& m, uint32_t headCell, uint32_t nodeBase,
                       uint32_t entryBase, uint32_t count, uint32_t state)
{
    m.put(headCell, count ? nodeBase : 0);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t node = nodeBase + i * 8u;
        const uint32_t entry = entryBase + i * 0x20u;
        m.put(node, entry);
        m.put(node + 4u, (i + 1 < count) ? (node + 8u) : 0u);
        m.put(entry + 0u, 1u);
        m.put(entry + 4u, 0u);
        m.put(entry + 8u, state);
        m.put(entry + 12u, 0u);
    }
}

int run()
{
    int pass = 0, fail = 0;
    auto CHECK = [&](bool ok, const char* what) {
        if (ok) { ++pass; }
        else {
            ++fail;
            // The one emission that does not pass through the Watch sink. An assertion
            // label naming a tag - and these labels routinely name tags in prose - would
            // otherwise put an unsanitized [H7_*] token into the log and corrupt a raw
            // census of it. Covered by the narrowed contract, but narrow wording is not
            // the same as being safe.
            char lbl[256];
            std::snprintf(lbl, sizeof(lbl), "%s", what);
            sanitizeEmbeddedTags(lbl);
            SR_LOG_BOOT(sr::LogSeverity::Error, "[H7_TEST] FAIL: %s", lbl);
        }
    };

    const uint32_t L1 = 0x30000000u, N1 = 0x30001000u, E1 = 0x30002000u;

    // 1. Census latches entries and reports them as new, once.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 3, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        CHECK(w.shadowSize() == 3, "census latches 3 entries");
        CHECK(cap.countTag("[H7_RESOLVE] new") == 3, "3 new events");
        w.censusResolverList(L1, rd);
        CHECK(cap.countTag("[H7_RESOLVE] new") == 3, "unchanged re-census emits nothing new");
        m.put(E1 + 8u, 4u);
        w.censusResolverList(L1, rd);
        CHECK(cap.countTag("[H7_RESOLVE] chg") == 1, "state change reported once");
    }

    // 2. TRANSACTIONAL RECONCILIATION (director item 2). A complete census, then a
    //    truncated one, must NOT mark the unseen entries gone — and must not silence
    //    a later linked-free.
    for (int variant = 0; variant < 3; ++variant) {
        const char* label = variant == 0 ? "bad pointer" : (variant == 1 ? "cycle" : "unreadable next");
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 3, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        CHECK(w.isLinked(L1, E1 + 0x20u) && w.isLinked(L1, E1 + 0x40u), "all three linked after a complete walk");

        if (variant == 0)      m.put(N1 + 4u, 0x00000003u);   // node 1 -> misaligned/sub-floor
        else if (variant == 1) m.put(N1 + 12u, N1);           // node 1 -> back to head
        else                   m.w.erase(N1 + 12u);           // node 1's next unreadable
        w.censusResolverList(L1, rd);

        CHECK(cap.countTag("[H7_PARTIAL]") == 1, "truncated walk announces itself");
        CHECK(cap.countTag("[H7_RESOLVE] gone") == 0, "truncated walk confirms NO absence");
        CHECK(w.membershipOf(L1, E1 + 0x40u) == Membership::Unknown,
              "an entry not reached by a truncated walk becomes UNKNOWN, not stale-linked");
        // Director-required: destruction of an unseen entry after a partial census must
        // produce UNCERTAINTY, not a decisive linked-free. The list is still broken here,
        // so the re-confirmation walk cannot complete either.
        w.noteDestroy(E1 + 0x40u, rd);
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 0, "no decisive linked-free on unconfirmed membership");
        CHECK(cap.countTag("[H7_LINKED_UNKNOWN]") == 1, "uncertainty is reported under its own tag");
        CHECK(w.linkedUnknownCount() == 1, "uncertainty is counted separately from linked frees");
        (void)label;
    }

    // 3. A complete walk DOES reconcile: the shortened list marks the tail gone.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 3, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        m.put(N1 + 4u, 0u);                                   // truncate the list legitimately
        w.censusResolverList(L1, rd);
        CHECK(cap.countTag("[H7_PARTIAL]") == 0, "a legitimately shorter list is not 'partial'");
        CHECK(cap.countTag("[H7_RESOLVE] gone") == 2, "both dropped entries reconciled");
        CHECK(w.membershipOf(L1, E1 + 0x20u) == Membership::Absent,
              "a complete walk confirms ABSENCE, which is not the same as unknown");
    }

    // 4. CROSS-LIST SCOPING (director item 3). Same entry address in two latched
    //    lists: removing from one must leave the other linked, and destruction must
    //    still fire.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m;
        const uint32_t L2 = 0x31000000u, N2 = 0x31001000u;
        buildResolverList(m, L1, N1, E1, 1, 3);
        m.put(L2, N2); m.put(N2, E1); m.put(N2 + 4u, 0u);     // second list, SAME entry
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        w.censusResolverList(L2, rd);
        CHECK(w.shadowSize() == 2, "one address in two lists = two membership records");
        CHECK(w.isLinked(L1, E1) && w.isLinked(L2, E1), "linked in both");

        m.put(L2, 0u);                                        // genuinely removed from L2 only
        w.noteRemoveByValueAfter(L2, E1, rd);
        CHECK(!w.isLinked(L2, E1), "removal clears membership of the list it happened on");
        CHECK(w.isLinked(L1, E1), "removal does NOT clear membership of the other list");
        w.noteDestroy(E1, rd);
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 1, "destruction still fires via the still-linked list");
    }

    // 5. REMOVAL ONLY AFTER DELEGATION (director item 4). If the helper did not
    //    actually remove the value, no removal is recorded.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 1, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        w.noteRemoveByValueAfter(L1, E1, rd);                 // value still present
        CHECK(cap.has("STILL a member"), "a no-op helper is reported, not believed");
        CHECK(w.membershipOf(L1, E1) == Membership::Linked,
              "a no-op helper leaves membership CONFIRMED linked by the post-call walk");
        m.put(L1, 0u);
        w.noteRemoveByValueAfter(L1, E1, rd);
        CHECK(w.membershipOf(L1, E1) == Membership::Absent,
              "a real removal is recorded once confirmed absent");
        CHECK(cap.has("confirmed absent"), "the confirmed removal says so");
    }

    // 6. Removal on an unlatched list is ignored entirely.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 1, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.noteRemoveByValueAfter(0x40000000u, E1, rd);
        CHECK(cap.countTag("[H7_REMOVE]") == 0, "removal on an unlatched list ignored");
        CHECK(w.removeOutOfScope() == 1, "out-of-scope removal counted");
    }

    // 7. Linked destruction: fires while linked, silent when properly unlinked.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 2, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        m.put(L1, N1 + 8u);                                   // entry 0 genuinely gone
        w.noteRemoveByValueAfter(L1, E1, rd);
        // Director-required: a COMPLETE current walk proving absence stays silent.
        w.noteDestroy(E1, rd);
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 0, "complete walk proving absence is silent");
        CHECK(cap.countTag("[H7_LINKED_UNKNOWN]") == 0, "a confirmed absence is not uncertainty");
        // Director-required: a COMPLETE current walk that FINDS the entry is decisive.
        w.noteDestroy(E1 + 0x20u, rd);
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 1, "complete walk finding the entry emits linked-free");
        w.noteDestroy(E1 + 0x20u, rd);
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 1, "linked-free anomaly is once-only");
    }

    // 8. Generations + allocator reuse via tombstones.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 1, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        CHECK(w.shadowGen(L1, E1) == 1, "first generation is 1");
        w.noteDestroy(E1, rd);
        CHECK(w.hasShadow(L1, E1), "destroy keeps a tombstone");
        CHECK(!w.isLive(L1, E1), "the tombstone is not live");
        CHECK(w.generationOf(E1) == 2, "destroy bumps the generation");
        w.censusResolverList(L1, rd);
        CHECK(cap.countTag("[H7_REUSE]") == 1, "allocator reuse is REPORTED, not spliced");
        CHECK(w.shadowGen(L1, E1) == 2, "re-latched at generation 2");
        CHECK(w.isLive(L1, E1), "the reused address is live again");
    }

    // 9. Bad pointers are refused, not dereferenced.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(0x00000004u, rd);
        CHECK(cap.countTag("[H7_BADPTR]") == 1, "sub-floor list head refused");
        CHECK(w.shadowSize() == 0, "nothing latched from a refused walk");
        CHECK(!Watch::validPtr(0x30000001u), "misaligned VA rejected");
        CHECK(!Watch::validPtr(0xD0000000u), "above-ceiling VA rejected");
        CHECK(!Watch::validPtr(0u), "null rejected");
        CHECK(Watch::validPtr(0x82000000u), "a normal guest VA is accepted");
    }

    // 10. Ordinary output cap engages and announces; anomalies emit through it.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m;
        const uint32_t n = kOrdinaryCap + 10u;
        buildResolverList(m, L1, N1, E1, n, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        CHECK(w.shadowSize() == n, "all entries latched despite the output cap");
        CHECK(cap.countTag("[H7_RESOLVE]") == kOrdinaryCap + 1, "output capped, with a notice");
        CHECK(cap.has("output cap reached"), "the output cap is announced, not silent");
        w.noteDestroy(E1 + 5u * 0x20u, rd);
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 1, "anomaly emits through the ordinary cap");
    }

    // 11. PERSISTENT-CONTAINER CAPS (director item 5), each with its own [H7_CAP]
    //     announcement — and never labelled [H7_CYCLE].
    {   // census call budget
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 1, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.setCensusCapForTest(2);
        w.censusResolverList(L1, rd); w.censusResolverList(L1, rd);
        CHECK(cap.countTag("[H7_CAP]") == 0, "cap not announced before it is reached");
        w.censusResolverList(L1, rd);
        CHECK(cap.count("census call budget") == 1, "census budget announced under [H7_CAP]");
        w.censusResolverList(L1, rd);
        CHECK(cap.countTag("[H7_CAP]") == 1, "census cap announcement is once-only");
        CHECK(cap.countTag("[H7_CYCLE]") == 0, "a capacity refusal is NOT reported as a cycle");
    }
    {   // latched-list cap
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        for (uint32_t i = 0; i <= kMaxKnownLists; ++i) {
            const uint32_t head = 0x50000000u + i * 0x1000u;
            m.put(head, 0u);
            w.censusResolverList(head, rd);
        }
        CHECK(w.knownListCount() == kMaxKnownLists, "latched lists stop at the cap");
        CHECK(cap.count("latched resolver lists") == 1, "list cap announced under [H7_CAP]");
    }
    {   // H7c record-array identity cap
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        for (uint32_t i = 0; i <= kMaxParse; ++i) {
            ParseArgs p; p.blob = 0x40000000u; p.header = 0x41000000u; p.node = 0x42000000u;
            p.field20 = 0x100u; p.recordArray = 0x60000000u + i * 0x20u; p.record0 = i; p.valid = true;
            w.noteParseOutcome(p, true);
        }
        CHECK(cap.count("H7c record-array identities") == 1, "parse cap announced under [H7_CAP]");
        CHECK(cap.countTag("[H7_CYCLE]") == 0, "parse cap is not a cycle");
    }
    {   // RemoveAndDestroy identity cap
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        for (uint32_t i = 0; i <= kMaxRmd; ++i)
            w.noteRemoveAndDestroy(0x70000000u + i * 0x20u, 0x72000000u, 0x24000000u,
                                   0x71000000u, 6, 5);
        CHECK(cap.count("RemoveAndDestroy identities") == 1, "rmd cap announced under [H7_CAP]");
    }
    {   // membership-record cap. It has to be reached across TWO lists: the per-walk
        // node budget equals kMaxShadow, so one list can never overflow the shadow -
        // it would hit the node budget first and report a cycle instead. Bases are
        // spread far apart because at this size a densely packed node array would
        // overlap the entry array and quietly corrupt the fixture.
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m;
        const uint32_t LA = 0x38000000u, NA = 0x38100000u, EA = 0x38800000u;
        const uint32_t LB = 0x39000000u, NB = 0x39100000u, EB = 0x39800000u;
        buildResolverList(m, LA, NA, EA, kMaxShadow, 3);      // exactly fills the shadow
        buildResolverList(m, LB, NB, EB, 4u, 3);              // these have nowhere to go
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(LA, rd);
        CHECK(w.shadowSize() == kMaxShadow, "the first list fills the shadow exactly");
        CHECK(cap.countTag("[H7_CYCLE]") == 0, "a full-but-legal walk is not a cycle");
        CHECK(cap.countTag("[H7_PARTIAL]") == 0, "a full-but-legal walk completes");
        w.censusResolverList(LB, rd);
        CHECK(w.shadowSize() == kMaxShadow, "membership records stop at the cap");
        CHECK(cap.count("membership records") == 1, "shadow cap announced under [H7_CAP]");
        CHECK(cap.countTag("[H7_CYCLE]") == 0, "shadow cap is not a cycle");
    }

    // 12. matchCount: two tasks holding one clip is visible.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m;
        const uint32_t clip = 0x33000000u;
        m.put(0x32000000u, 0x32001000u);
        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t node = 0x32001000u + i * 8u, task = 0x32002000u + i * 0x10u;
            m.put(node, task);
            m.put(node + 4u, (i < 2) ? node + 8u : 0u);
            m.put(task + 4u, 0x34000000u + i * 0x20u);
            m.put(task + 8u, (i == 1) ? 0x33000004u : clip);
        }
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        Watch::ExecFacts f;
        w.scanTaskList(0x32000000u, clip, rd, f);
        CHECK(f.valid, "task walk completed");
        CHECK(f.matchCount == 2, "matchCount counts BOTH tasks holding the clip");
        CHECK(f.firstTask == 0x32002000u, "firstTask is the first match, as the guest finder is");
        CHECK(f.firstTaskEntry == 0x34000000u, "firstTask+4 read");
        w.emitExec(f);
        CHECK(cap.countTag("[H7_MATCHDUP]") == 1, "matchCount>1 raises the uniqueness anomaly");
    }

    // 13. H7c ARGUMENT MAPPING (director item 1), tested ADVERSARIALLY. The guest
    //     computes recordArray = (*(header+20) ? *(header+20)+blob : 0)+16 with
    //     r5=blob and r6=header. The previous candidate used r4 and treated r6 as
    //     the blob; these checks fail if that mapping ever comes back.
    {
        const uint32_t blob = 0x44000000u, header = 0x45000000u, node = 0x46000000u;
        const uint32_t off = 0x2000u;
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; m.put(header + 20u, off); m.put(blob + off + 16u, 0xABCDu);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};

        CHECK(deriveRecordArray(blob, off) == blob + off + 16u, "recordArray = blob + hdr[20] + 16");
        CHECK(deriveRecordArray(blob, 0) == 16u, "a zero header offset yields the degenerate 16");
        // ADVERSARIAL: the swapped mapping must produce a DIFFERENT address, so this
        // test cannot pass with blob and header transposed.
        CHECK(deriveRecordArray(header, off) != deriveRecordArray(blob, off),
              "blob/header transposition yields a different record array");

        ParseArgs p;
        CHECK(deriveParseArgs(blob, header, node, rd, p), "derivation succeeds on valid input");
        CHECK(p.recordArray == blob + off + 16u, "derived from r5=blob and r6=header");
        CHECK(p.record0 == 0xABCDu, "pre-call record[0] read from the derived address");
        CHECK(p.field20 == off, "header+20 captured");

        ParseArgs q;
        CHECK(!deriveParseArgs(0, header, node, rd, q), "blob==0 refused (guest early-outs)");
        ParseArgs r;
        FakeMem m2; m2.put(header + 20u, 0u);
        MemReader rd2{&fakeRead32, &fakeReadBytes, &m2};
        CHECK(!deriveParseArgs(blob, header, node, rd2, r), "header+20==0 refused, not guessed");
        ParseArgs s;
        CHECK(!deriveParseArgs(blob, 0x00000002u, node, rd, s), "invalid header pointer refused");

        // A first sighting emits [H7_PARSEFIRST] and NOTHING ELSE. The previous version
        // of this check asserted only zero PARSEREP, which is silent about whether the
        // call produced any output at all - it kept passing after first sightings stopped
        // being silent, and its own wording still said they were.
        w.noteParseOutcome(p, true);
        CHECK(cap.countTag("[H7_PARSEFIRST]")  == 1, "a first sighting emits exactly one PARSEFIRST");
        CHECK(cap.countTag("[H7_PARSEREP]")    == 0, "and no repeat");
        CHECK(cap.countTag("[H7_PARSEREFUSE]") == 0, "and no refusal");
        w.noteParseOutcome(p, true);
        CHECK(cap.countTag("[H7_PARSEREP]")    == 1, "repeat sighting reported");
        CHECK(cap.countTag("[H7_PARSEFIRST]")  == 1, "and no second first-witness");
        CHECK(cap.has("(same source)"), "unchanged blob/header/node labelled same source");
        ParseArgs p2 = p; p2.blob = 0x47000000u;
        w.noteParseOutcome(p2, true);
        CHECK(cap.has("(SOURCE CHANGED)"), "changed blob labelled a source change");
    }

    // 14. WRAPPER-LEVEL: the delegate CLOBBERS r3, as the real guest function does
    //     (`sub_826AFA58` ends in `return sub_825F6B90(...)`). Both censuses must
    //     still use the latched a1. This test fails if the wrapper ever re-reads r3.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m;
        const uint32_t decoy = 0x3A000000u, decoyNode = 0x3A001000u, decoyEntry = 0x3A002000u;
        buildResolverList(m, L1, N1, E1, 2, 3);
        buildResolverList(m, decoy, decoyNode, decoyEntry, 2, 3);   // fully valid, must NOT be seen
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};

        uint32_t r3 = L1;
        bool ran = false;
        resolverWrapperBody(w, rd, r3, [&] { ran = true; r3 = decoy; });   // guest clobbers r3

        CHECK(ran, "the delegate ran");
        CHECK(r3 == decoy, "the delegate really did clobber r3");
        CHECK(w.knownListCount() == 1, "exactly one list was censused");
        CHECK(w.hasShadow(L1, E1), "the latched list was censused");
        CHECK(!w.hasShadow(decoy, decoyEntry), "the clobbered-in list was NEVER censused");
        CHECK(w.shadowSize() == 2, "only the latched list's entries are tracked");
    }

    // 15. Incomplete post-removal confirmation followed by destruction must NOT emit
    //     a decisive linked-free — the exact false-positive sequence the director
    //     constructed: censused linked, helper runs, confirmation walk fails, entry
    //     is destroyed. It was correctly removed; claiming a linked free would be a
    //     fabricated H7u sighting.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 2, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        CHECK(w.membershipOf(L1, E1) == Membership::Linked, "entry starts confirmed linked");

        m.put(L1, N1 + 8u);          // the helper genuinely removed it ...
        m.w.erase(N1 + 12u);         // ... but the list is now unwalkable past node 1
        w.noteRemoveByValueAfter(L1, E1, rd);
        CHECK(w.membershipOf(L1, E1) == Membership::Unknown,
              "an unconfirmable removal downgrades membership to UNKNOWN, not stale-linked");

        w.noteDestroy(E1, rd);
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 0,
              "a correctly-removed entry does NOT produce a false linked-free");
        CHECK(cap.countTag("[H7_LINKED_UNKNOWN]") == 1, "the uncertainty is reported instead");
    }

    // 16. GENERATION CAPACITY IS NOT SPENT ON UNTRACKED ADDRESSES. This is what makes
    //     the cap structurally unreachable: destroys of addresses that were never
    //     censused carry no lifetime history worth keeping. The previous candidate let
    //     them consume the budget, which is how the cap became reachable at all.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        for (uint32_t i = 0; i <= kMaxGenTracked; ++i) w.noteDestroy(0x80000000u + i * 0x20u, rd);
        CHECK(w.genTrackedCount() == 0, "destroying never-censused addresses spends NO capacity");
        CHECK(cap.countTag("[H7_CAP]") == 0, "and therefore never reaches the cap");
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 0, "destroying untracked addresses says nothing");
        CHECK(kMaxGenTracked >= kMaxShadow,
              "the generation budget covers every trackable identity, so the cap is a backstop");
    }

    // 17. THE DIRECTOR'S EXACT SEQUENCE: fill -> census -> destroy -> reuse, past the
    //     cap. Previously genOfLocked kept returning 1, the mismatch branch never fired,
    //     [H7_REUSE] was absent, and the tombstone was retained as the SAME lifetime -
    //     the precise splice the counter exists to prevent. The cap is injected because
    //     the real one is now unreachable by construction (test 16).
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        const uint32_t FILL = 8u;
        w.setGenCapForTest(FILL);

        // Fill generation capacity with FILL *tracked* identities (each censused, then
        // destroyed, so each legitimately owns a generation slot).
        for (uint32_t i = 0; i < FILL; ++i) {
            const uint32_t head = 0x60000000u + i * 0x10000u;
            const uint32_t node = head + 0x100u, ent = head + 0x200u;
            buildResolverList(m, head, node, ent, 1, 3);
            w.censusResolverList(head, rd);
            w.noteDestroy(ent, rd);
        }
        CHECK(w.genTrackedCount() == FILL, "generation capacity is full");
        CHECK(cap.countTag("[H7_CAP]") == 0, "filling exactly to the cap does not announce it");

        // Now a NEW tracked address, beyond capacity.
        const uint32_t H = 0x6F000000u, N = H + 0x100u, E = H + 0x200u;
        buildResolverList(m, H, N, E, 1, 3);
        w.censusResolverList(H, rd);
        CHECK(w.isLive(H, E), "the new entry is latched");
        w.noteDestroy(E, rd);
        CHECK(cap.count("allocation generations") == 1, "the cap is announced when exceeded");
        CHECK(!w.isLive(H, E), "the destroy left a tombstone");

        // Reuse the same address. The tombstone must NOT be retained as one lifetime.
        w.censusResolverList(H, rd);
        CHECK(cap.countTag("[H7_REUSE_UNKNOWN]") == 1,
              "unsupported reuse emits a DISTINCT uncertainty event, not silence");
        CHECK(w.reuseUnknownCount() == 1, "and is counted as unsupported, not as confirmed reuse");
        CHECK(w.reuseCount() == 0, "it is NOT reported as a confirmed reuse");
        CHECK(w.isLive(H, E), "a FRESH record replaced the tombstone");
        CHECK(cap.countTag("[H7_REUSE]") == 0, "no false claim of a corroborated lifetime change");

        // Once-only, and supported identities still distinguish reuse afterwards.
        w.noteDestroy(E, rd);
        w.censusResolverList(H, rd);
        CHECK(cap.countTag("[H7_CAP]") == 1, "the cap notice stays once-only");
        CHECK(cap.countTag("[H7_REUSE_UNKNOWN]") == 1, "the uncertainty tag stays once-only");

        const uint32_t supported = 0x60000000u;      // tracked before the cap
        w.censusResolverList(supported, rd);         // re-latch at its bumped generation
        w.noteDestroy(supported + 0x200u, rd);
        w.censusResolverList(supported, rd);
        CHECK(w.reuseCount() >= 1, "supported identities still distinguish reuse past the cap");
    }

    // 18. MIXED COMPLETE/INCOMPLETE MULTI-LIST DESTROY, BOTH REGISTRATION ORDERS.
    //     The case that decides whether a positive confirmation on one list survives an
    //     unresolvable walk of another. Run twice with the census order swapped, because
    //     m_byEntry preserves insertion order and a verdict that depended on it would be
    //     an artefact of which list happened to be censused first.
    for (int order = 0; order < 2; ++order) {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m;
        const uint32_t LA = 0x90000000u, NA = 0x90001000u;              // walkable, holds EE
        const uint32_t LB = 0x90010000u, NB0 = 0x90011000u, NB1 = 0x90011008u;
        const uint32_t EE = 0x90005000u, XX = 0x90006000u;              // EE is in BOTH lists
        auto entryFields = [&](uint32_t e, uint32_t state) {
            m.put(e + 0u, 1u); m.put(e + 4u, 0u); m.put(e + 8u, state); m.put(e + 12u, 0u);
        };
        // A: head -> node -> EE
        m.put(LA, NA); m.put(NA, EE); m.put(NA + 4u, 0u);
        // B: head -> node0(XX) -> node1(EE). EE sits AFTER node0, so breaking node0's
        // `next` makes the walk fail BEFORE it can reach EE — an incomplete walk, not a
        // walk that proves absence.
        m.put(LB, NB0); m.put(NB0, XX); m.put(NB0 + 4u, NB1);
        m.put(NB1, EE); m.put(NB1 + 4u, 0u);
        entryFields(EE, 3u); entryFields(XX, 3u);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};

        if (order == 0) { w.censusResolverList(LA, rd); w.censusResolverList(LB, rd); }
        else            { w.censusResolverList(LB, rd); w.censusResolverList(LA, rd); }
        CHECK(w.membershipOf(LA, EE) == Membership::Linked, "EE confirmed linked in A");
        CHECK(w.membershipOf(LB, EE) == Membership::Linked, "EE confirmed linked in B");

        m.w.erase(NB0 + 4u);      // B becomes unwalkable before it reaches EE
        w.noteDestroy(EE, rd);

        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 1,
              "the positive confirmation still produces exactly one decisive linked-free");
        CHECK(cap.countTag("[H7_LINKED_UNKNOWN]") == 0,
              "uncertainty on the other list does NOT replace or suppress the decisive result");
        CHECK(w.linkedFreeCount() == 1, "counted once, from the confirmed list only");
        CHECK(w.linkedUnknownCount() == 0, "and not also counted as uncertain");
        CHECK(w.membershipOf(LA, EE) == Membership::Linked,
              "the completed list stays Linked");
        CHECK(w.membershipOf(LB, EE) == Membership::Unknown,
              "only the incomplete list becomes Unknown — no contamination either way");
        CHECK(!w.isLive(LA, EE) && !w.isLive(LB, EE), "both memberships are tombstoned");
    }

    // 19. The m_gen ⊆ m_byEntry containment, PINNED rather than argued, in a workload
    //     that INCLUDES cross-list membership. Previously this rested on there being
    //     exactly one writer of each container - true today, silently false the moment a
    //     second writer appears - and the earlier version asserted it over a workload
    //     with no address in two lists, which is exactly where a per-membership
    //     generation slot would have shown up. Test 18 has cross-list membership but a
    //     separate Watch, so it cannot close this; the two have to be one workload.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        const uint32_t L2 = 0x91000000u, N2 = 0x91001000u;
        buildResolverList(m, L1, N1, E1, 3, 3);
        m.put(L2, N2); m.put(N2, E1); m.put(N2 + 4u, 0u);   // SAME address, second list

        w.censusResolverList(L1, rd);
        w.censusResolverList(L2, rd);
        CHECK(w.hasShadow(L1, E1) && w.hasShadow(L2, E1),
              "one address in two lists yields TWO membership records");
        CHECK(w.shadowSize() == 4, "3 from the first list plus the cross-list record");
        CHECK(w.generationOf(E1) == 1, "one address, one generation, whatever its list count");

        w.noteDestroy(E1, rd);                       // tracked, member of TWO lists
        CHECK(w.generationOf(E1) == 2,
              "a destroy bumps the ADDRESS once — not once per list membership");
        CHECK(w.genTrackedCount() == 1,
              "generation ownership is per entry address, NOT multiplied by membership");

        w.noteDestroy(0xA0000000u, rd);              // untracked -> spends nothing
        CHECK(w.genTrackedCount() == 1, "an untracked destroy spends no capacity");

        w.noteDestroy(E1 + 0x20u, rd);               // second tracked destroy
        CHECK(w.genTrackedCount() == 2, "two tracked addresses, two slots");

        w.censusResolverList(L1, rd);                // reuse path, first list
        w.censusResolverList(L2, rd);                // reuse path, cross-list record too
        CHECK(w.generationOfIsSubsetOfIndexed(),
              "every generation key is an indexed entry, after cross-list reuse");
        CHECK(w.genTrackedCount() == 2,
              "reuse across two lists still does not multiply generation ownership");
        CHECK(w.isLive(L1, E1) && w.isLive(L2, E1),
              "both membership records were freshly re-latched, neither left tombstoned");
    }

    // 20. [H7_PARSEFIRST] — the bridge's own positive witness. Previously a first
    //     sighting was silent, so "the bridge never fired" and "every parse was a
    //     distinct first identity" produced IDENTICAL logs and the roll had to borrow a
    //     capped count from another tag to tell them apart.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        ParseArgs p; p.blob = 0x44000000u; p.header = 0x45000000u; p.node = 0x46000000u;
        p.field20 = 0x2000u; p.recordArray = 0x44002010u; p.record0 = 0xABCDu; p.valid = true;

        w.noteParseOutcome(p, true);
        CHECK(cap.countTag("[H7_PARSEFIRST]") == 1, "a first sighting now WITNESSES itself");
        CHECK(cap.countTag("[H7_PARSEREP]") == 0, "and is still not reported as a repeat");
        w.noteParseOutcome(p, true);
        CHECK(cap.countTag("[H7_PARSEFIRST]") == 1, "a repeat does not emit a second first-witness");
        CHECK(cap.countTag("[H7_PARSEREP]") == 1, "the repeat is reported as a repeat");
        CHECK(cap.countTag("[H7_PARSEREFUSE]") == 0, "and neither is reported as a refusal");
        // DISCRIMINATING: a run with no parses must be distinguishable from this one.
        Capture cap2; Watch w2{Sink{&Capture::emit, &cap2}};
        CHECK(cap2.countTag("[H7_PARSEFIRST]") == 0 && cap2.countTag("[H7_PARSEREP]") == 0,
              "a bridge that never fires now looks DIFFERENT from one that fired once");
    }

    // 21. [H7_REMOVE_OOS] — out-of-scope removals had a host counter and no log surface,
    //     so [H7_REMOVE] 0 could not be told apart from "the helper never ran".
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 1, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        const size_t shadowBefore = w.shadowSize();

        w.noteRemoveByValueAfter(0x40000000u, E1, rd);     // unlatched list
        CHECK(cap.countTag("[H7_REMOVE_OOS]") == 1, "an out-of-scope removal is now VISIBLE");
        CHECK(cap.countTag("[H7_REMOVE]") == 0, "and is not reported as an in-scope removal");
        CHECK(w.removeOutOfScope() == 1, "still counted");
        CHECK(w.shadowSize() == shadowBefore, "no shadow record was created");
        CHECK(w.membershipOf(L1, E1) == Membership::Linked,
              "and NO membership was mutated by an out-of-scope removal");
    }

    // 22. [H7_DESTROY] — every tracked destruction is recorded with its reconfirmed
    //     result. The silence for confirmed-absent destruction is what let me claim
    //     "no resolver entry was destroyed at all", which was false.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 3, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);

        w.noteDestroy(E1, rd);                              // still linked
        CHECK(cap.count("reconfirmed=linked") == 1, "a linked destruction is described");
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 1, "and the decisive tag is UNCHANGED");

        m.put(L1, N1 + 16u);                                // entry 1 genuinely removed
        w.noteRemoveByValueAfter(L1, E1 + 0x20u, rd);
        w.noteDestroy(E1 + 0x20u, rd);
        CHECK(cap.count("reconfirmed=absent") == 1,
              "a confirmed-ABSENT destruction is now visible instead of silent");
        CHECK(cap.countTag("[H7_DTOR_LINKED]") == 1, "and raises no decisive tag");

        // The walk must fail BEFORE reaching the target: listContains tests a node's
        // value before reading its next, so breaking the TAIL node's next would still
        // find the entry and complete. Head at node1 (value E1+0x20, not the target)
        // with node1's next erased.
        m.put(L1, N1 + 8u);
        m.w.erase(N1 + 12u);
        w.noteDestroy(E1 + 0x40u, rd);
        CHECK(cap.count("reconfirmed=unknown") == 1, "an unresolvable destruction says so");
        CHECK(cap.countTag("[H7_LINKED_UNKNOWN]") == 1, "and still raises the uncertainty tag");
        CHECK(cap.countTag("[H7_DESTROY]") == 3, "all three tracked destructions recorded");

        // DISCRIMINATING: an UNTRACKED destruction stays silent, so [H7_DESTROY] means
        // "tracked entry destroyed" and nothing broader.
        w.noteDestroy(0xB0000000u, rd);
        CHECK(cap.countTag("[H7_DESTROY]") == 3, "an untracked destruction emits nothing");
    }

    // 23. [H7_RMDESTROY] as a RAW CANDIDATE CHAIN. The type word is logged and the record
    //     reports typeEligible and observedTo5 as INDEPENDENT facts. It types nothing and
    //     attributes nothing; test 26 covers the adversarial non-type-9 case.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        // gated writer DID fire: w8 6 -> 5
        w.noteRemoveAndDestroy(0x50000000u, 0x50001000u, 0x24000000u, 0x50002000u, 6, 5);
        CHECK(cap.has("observedTo5=yes") && cap.has("typeEligible=yes"),
              "a type-9 candidate observed transitioning to 5 reports BOTH facts");
        CHECK(cap.has("typeWord=24000000"), "the gating type word is logged");
        CHECK(cap.has("typeTag=9"), "and its >>26 tag is derived (0x24000000>>26 == 9)");

        // gated writer did NOT fire, and w8 is a large non-enum value — the iter-455j
        // shape. This must NOT read as anomalous.
        w.noteRemoveAndDestroy(0x51000000u, 0x51001000u, 0x00000000u, 0x51002000u,
                               360448u, 360448u);
        CHECK(cap.count("observedTo5=no") == 1,
              "a candidate that did not transition reports observedTo5=no");
        CHECK(cap.has("RAW CHAIN"), "the record states the chain is followed unconditionally");
        CHECK(cap.countTag("[H7_RMDESTROY]") == 2, "both records emitted");

        // unreadable chain still refuses rather than inventing values
        w.noteRemoveAndDestroy(0x52000000u, 0x52001000u, 0xFFFFFFFFu, 0x00000008u,
                               0xFFFFFFFFu, 0xFFFFFFFFu);
        CHECK(cap.has("typeTag=n/a"), "an unreadable type word reports n/a, not 0");
        CHECK(cap.count("w8 n/a -> n/a") == 1, "an invalid candidate reports n/a both sides");
    }

    // 24. PARSER OUTCOMES ARE THREE EXCLUSIVE, EXHAUSTIVE CLASSES (blocker 1).
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        ParseArgs ok1; ok1.recordArray = 0x44002010u; ok1.blob = 0x44000000u;
        ok1.header = 0x45000000u; ok1.node = 0x46000000u; ok1.field20 = 0x2000u; ok1.valid = true;
        ParseArgs bad; bad.blob = 0; bad.header = 0x45000000u; bad.refusal = "blob==0";

        w.noteParseOutcome(ok1, true);    // FIRST
        w.noteParseOutcome(ok1, true);    // REPEAT
        w.noteParseOutcome(bad, false);   // REFUSE
        CHECK(cap.countTag("[H7_PARSEFIRST]")  == 1, "FIRST has its own tag");
        CHECK(cap.countTag("[H7_PARSEREP]")    == 1, "REPEAT has its own tag");
        CHECK(cap.countTag("[H7_PARSEREFUSE]") == 1, "REFUSE has its own tag, not PARSEREP");
        // EXHAUSTIVE and MUTUALLY EXCLUSIVE: every call lands in exactly one class.
        CHECK(w.parseCalls() == 3, "three calls counted");
        CHECK(w.parseFirstCount() + w.parseRepeatCount() + w.parseRefuseCount() == w.parseCalls(),
              "the three classes are exhaustive");
        CHECK(w.parseFirstCount() == 1 && w.parseRepeatCount() == 1 && w.parseRefuseCount() == 1,
              "and mutually exclusive - one increment per call");
    }
    // 24b. OBSERVER-CAPACITY refusal is a PARSER OUTCOME, not merely a capacity notice.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        for (uint32_t i = 0; i <= kMaxParse; ++i) {
            ParseArgs p; p.recordArray = 0x60000000u + i * 0x20u; p.blob = 0x44000000u;
            p.header = 0x45000000u; p.node = 0x46000000u; p.field20 = 0x2000u; p.valid = true;
            w.noteParseOutcome(p, true);
        }
        CHECK(w.parseRefuseCount() == 1, "the over-capacity call is classified REFUSE");
        CHECK(cap.countTag("[H7_PARSEREFUSE]") == 1, "and emits a parser outcome");
        CHECK(cap.countTag("[H7_CAP]") == 1, "alongside - not instead of - the capacity notice");
        CHECK(w.parseFirstCount() + w.parseRepeatCount() + w.parseRefuseCount() == w.parseCalls(),
              "exhaustive even at capacity");
    }
    // 24c. CROSS-CAP INDEPENDENCE, all three directions. Exhausting one class's output
    //      budget must not suppress either of the others.
    {
        struct Dir { const char* exhaust; const char* a; const char* b; };
        const Dir dirs[3] = {
            {"[H7_PARSEFIRST]",  "[H7_PARSEREP]",    "[H7_PARSEREFUSE]"},
            {"[H7_PARSEREP]",    "[H7_PARSEFIRST]",  "[H7_PARSEREFUSE]"},
            {"[H7_PARSEREFUSE]", "[H7_PARSEFIRST]",  "[H7_PARSEREP]"},
        };
        for (int d = 0; d < 3; ++d) {
            Capture cap; Watch w{Sink{&Capture::emit, &cap}};
            auto mk = [&](uint32_t i) {
                ParseArgs p; p.recordArray = 0x70000000u + i * 0x20u; p.blob = 0x44000000u;
                p.header = 0x45000000u; p.node = 0x46000000u; p.field20 = 0x2000u; p.valid = true;
                return p;
            };
            // Blow past the cap of the class under test.
            for (uint32_t i = 0; i < kOrdinaryCap + 5u; ++i) {
                if (d == 0) w.noteParseOutcome(mk(i), true);                    // FIRSTs
                else if (d == 1) { ParseArgs p = mk(0); w.noteParseOutcome(p, true); w.noteParseOutcome(p, true); }
                else { ParseArgs b; b.blob = 0; b.refusal = "x"; w.noteParseOutcome(b, false); }
            }
            const size_t exhausted = cap.countTag(dirs[d].exhaust);
            CHECK(exhausted >= kOrdinaryCap, "the class under test reached its own cap");

            // SNAPSHOT IMMEDIATELY BEFORE the final calls. The previous version asserted
            // only `> 0` afterwards, so for d==1 it was satisfied by a FIRST emitted
            // during the exhaust phase — it would have passed even if an exhausted REPEAT
            // cap suppressed every subsequent FIRST. An exact +1 delta is the only form
            // that actually tests suppression.
            const size_t beforeA = cap.countTag(dirs[d].a);
            const size_t beforeB = cap.countTag(dirs[d].b);

            // Exactly one of each class, in one shot: a fresh identity (FIRST), the same
            // identity again (REPEAT), and an underivable one (REFUSE).
            ParseArgs fresh = mk(0xFFFFu);
            w.noteParseOutcome(fresh, true);
            w.noteParseOutcome(fresh, true);
            ParseArgs b2; b2.blob = 0; b2.refusal = "y";
            w.noteParseOutcome(b2, false);

            CHECK(cap.countTag(dirs[d].a) == beforeA + 1,
                  "the first unaffected class emitted EXACTLY one more line");
            CHECK(cap.countTag(dirs[d].b) == beforeB + 1,
                  "the second unaffected class emitted EXACTLY one more line");
        }
        // 3 exhausted classes x 2 unaffected classes = all 6 relationships, each as a
        // delta rather than a presence check.
    }

    // 25. [H7_DESTROY] MUST NOT DESCRIBE AN UNTRACKED LIFETIME (blocker 2).
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m; buildResolverList(m, L1, N1, E1, 1, 3);
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        w.censusResolverList(L1, rd);
        w.noteDestroy(E1, rd);                       // tracked -> described
        CHECK(cap.countTag("[H7_DESTROY]") == 1, "the tracked destruction is described");
        // The allocator reuses E1 but the new lifetime is NEVER censused. m_byEntry is
        // permanent, so the historical membership is still findable - and every record
        // for it is a tombstone.
        w.noteDestroy(E1, rd);
        CHECK(cap.countTag("[H7_DESTROY]") == 1,
              "a destroy with only tombstoned memberships stays SILENT, as promised");
    }
    // 25b. Identity must come from a CURRENT membership, not from whichever list was
    //      indexed first. front() could be a stale tombstone while the live record is later.
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        FakeMem m;
        const uint32_t LA = 0x94000000u, NA = 0x94001000u, EE = 0x94005000u;
        const uint32_t LB = 0x95000000u, NB = 0x95001000u;
        auto ent = [&](uint32_t e, uint32_t nameVA) {
            m.put(e + 0u, 1u); m.put(e + 4u, nameVA); m.put(e + 8u, 3u); m.put(e + 12u, 0u);
        };
        // name "OLD" then later "NEW" at the same entry address
        const uint32_t NM_OLD = 0x96000000u, NM_NEW = 0x96001000u;
        m.put(NM_OLD, 0x00000003u); m.put(NM_OLD + 8u, 0x4F4C4400u);   // "OLD"
        m.put(NM_NEW, 0x00000003u); m.put(NM_NEW + 8u, 0x4E455700u);   // "NEW"
        MemReader rd{&fakeRead32, &fakeReadBytes, &m};
        m.put(LA, NA); m.put(NA, EE); m.put(NA + 4u, 0u);
        ent(EE, NM_OLD);
        w.censusResolverList(LA, rd);                   // list A, name OLD
        w.noteDestroy(EE, rd);                          // A's record tombstoned
        // A NEW lifetime at the same address, censused only into list B.
        m.put(LB, NB); m.put(NB, EE); m.put(NB + 4u, 0u);
        ent(EE, NM_NEW);
        w.censusResolverList(LB, rd);
        w.noteDestroy(EE, rd);
        CHECK(cap.has("name='NEW'"),
              "identity comes from the CURRENT lifetime, not the stale tombstone in list A");
        CHECK(cap.countTag("[H7_DESTROY]") == 2, "both destructions were described");
        CHECK(cap.countTagWith("[H7_DESTROY]", "name='OLD' gen=1") == 1 &&
              cap.countTagWith("[H7_DESTROY]", "name='NEW' gen=2") == 1,
              "each destruction used ITS OWN lifetime's name and generation");
        CHECK(cap.countTagWith("[H7_DESTROY]", "name='OLD' gen=2") == 0,
              "the stale tombstone's identity was never attributed to the new lifetime");
    }

    // 26. typeEligible and observedTo5 are SEPARATE facts (blocker 4).
    {
        Capture cap; Watch w{Sink{&Capture::emit, &cap}};
        // type 9 AND a 6->5 transition: both facts positive.
        w.noteRemoveAndDestroy(0x50000000u, 0x50001000u, 0x24000000u, 0x50002000u, 6, 5);
        CHECK(cap.has("typeEligible=yes"), "tag 9 is type-eligible");
        CHECK(cap.has("observedTo5=yes"), "and the transition is reported");
        // ADVERSARIAL: a NON-type-9 candidate making the same 6->5 transition must NOT be
        // attributable to the gated writer. The old single wroteState5 field printed YES.
        w.noteRemoveAndDestroy(0x51000000u, 0x51001000u, 0x08000000u, 0x51002000u, 6, 5);
        CHECK(cap.count("typeEligible=no") == 1,
              "a non-type-9 candidate is NOT type-eligible even when it transitions to 5");
        CHECK(cap.count("observedTo5=yes") == 2, "the transition is still reported as a fact");
        CHECK(!cap.has("wroteState5"), "the merged verdict field is gone entirely");
        CHECK(cap.has("NEITHER field alone proves"), "and the record says so in the log");
        // unknown chain reports n/a on both, not a false negative
        w.noteRemoveAndDestroy(0x52000000u, 0x52001000u, 0xFFFFFFFFu, 0x00000008u,
                               0xFFFFFFFFu, 0xFFFFFFFFu);
        CHECK(cap.has("typeEligible=n/a") && cap.has("observedTo5=n/a"),
              "an unreadable chain reports n/a on both, never 'no'");
    }

    // 27. THE PRODUCTION EMIT PATHS SANITIZE - proven by driving crafted messages
    //     THROUGH them, not by calling the helper. The previous version called
    //     sanitizeEmbeddedTags() directly and its real Watch event contained no embedded
    //     tag, so deleting either production call site would have left it green: it
    //     tested the function and claimed the path.
    {
        const char* crafted = "see [H7_DTOR_LINKED] and [H7_CAP] for detail";

        // (a) the ORDINARY path
        {
            Capture cap; Watch w{Sink{&Capture::emit, &cap}};
            w.emitOrdinaryProbeForTest("[H7_RESOLVE]", crafted);
            CHECK(cap.lines.size() == 1, "the ordinary probe emitted one line");
            CHECK(cap.lines[0].find("[H7_", 1) == std::string::npos,
                  "ORDINARY path: no bracketed tag survives past position 0");
            CHECK(cap.lines[0].find("(H7_DTOR_LINKED)") != std::string::npos,
                  "ORDINARY path: the token is neutralised but still readable");
            CHECK(cap.count("[H7_") == cap.countTag("[H7_"),
                  "ORDINARY path: a naive substring census equals a positional one");
        }
        // (b) the RAW path
        {
            Capture cap; Watch w{Sink{&Capture::emit, &cap}};
            w.emitRawProbeForTest("[H7_DTOR_LINKED]", crafted);
            CHECK(cap.lines.size() == 1, "the raw probe emitted one line");
            CHECK(cap.lines[0].find("[H7_", 1) == std::string::npos,
                  "RAW path: no bracketed tag survives past position 0");
            CHECK(cap.lines[0].find("(H7_CAP)") != std::string::npos,
                  "RAW path: the token is neutralised but still readable");
        }
        // (c) the ORDINARY-CAP ANNOUNCEMENT path, which used to bypass sanitization
        //     entirely by calling the sink directly with a literal.
        {
            Capture cap; Watch w{Sink{&Capture::emit, &cap}};
            for (uint32_t i = 0; i < kOrdinaryCap + 3u; ++i)
                w.emitOrdinaryProbeForTest("[H7_RESOLVE]", crafted);
            CHECK(cap.has("output cap reached"), "the cap announcement was produced");
            for (const auto& l : cap.lines)
                if (l.find("[H7_", 1) != std::string::npos) {
                    CHECK(false, "CAP path: a bracketed tag survived past position 0");
                    break;
                }
            CHECK(cap.count("[H7_") == cap.countTag("[H7_"),
                  "CAP path included: substring census still equals positional census");
        }
        // (d) the invariant across a REAL mixed workload, not just probes.
        {
            Capture cap; Watch w{Sink{&Capture::emit, &cap}};
            FakeMem m; buildResolverList(m, L1, N1, E1, 3, 3);
            MemReader rd{&fakeRead32, &fakeReadBytes, &m};
            w.censusResolverList(L1, rd);
            w.noteDestroy(E1, rd);
            w.noteRemoveByValueAfter(0x40000000u, E1, rd);
            w.noteRemoveAndDestroy(0x50000000u, 0x50001000u, 0x24000000u, 0x50002000u, 6, 5);
            CHECK(cap.lines.size() > 4, "the mixed workload emitted several classes");
            CHECK(cap.count("[H7_") == cap.countTag("[H7_"),
                  "real events also carry exactly one bracketed tag each");
        }
    }

    // 28. SANITIZER BOUNDARY BEHAVIOUR. The previous implementation bailed out when it
    //     found no well-formed close, leaving the literal "[H7_" in the line - which a
    //     raw `rg -c "\[H7_"` counts as an event. Every case below FAILS against that
    //     version. All of them are reachable: the first from vsnprintf truncating a long
    //     message at the 640-byte boundary, the rest from message content.
    {
        struct Case { const char* in; const char* why; };
        const Case cases[] = {
            {"tail truncated mid-token [H7_LINKED",       "truncated at the buffer boundary"},
            {"[H7_",                                      "a bare prefix with no close"},
            {"lowercase [H7_abc] body",                   "non-charset content (lowercase)"},
            {"spaced [H7_FOO BAR] body",                  "non-charset content (space)"},
            {"[H7_A][H7_B] adjacent",                     "two adjacent well-formed tokens"},
            {"[H7_] empty body",                          "an empty token body"},
        };
        for (const auto& c : cases) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", c.in);
            sanitizeEmbeddedTags(buf);
            CHECK(std::string(buf).find("[H7_") == std::string::npos, c.why);
        }
        // And through the real emit paths, so the guarantee is about the LINE, not the
        // helper: a truncated token must not survive into the log either.
        {
            Capture cap; Watch w{Sink{&Capture::emit, &cap}};
            w.emitOrdinaryProbeForTest("[H7_RESOLVE]", "cut off here [H7_LINKED");
            w.emitRawProbeForTest("[H7_CYCLE]", "and here [H7_abc] too");
            for (const auto& l : cap.lines)
                CHECK(l.find("[H7_", 1) == std::string::npos,
                      "no malformed token survives the production emit paths");
            CHECK(cap.count("[H7_") == cap.countTag("[H7_"),
                  "a naive raw-log census still equals a positional one");
        }
        // A long message that genuinely hits the render buffer limit.
        {
            Capture cap; Watch w{Sink{&Capture::emit, &cap}};
            // 630 + 16 = 646 > 640, so vsnprintf CUTS the token mid-way. At 600 this
            // fixture did not truncate at all and the case proved nothing.
            std::string filler(630, 'x');
            filler += "[H7_DTOR_LINKED]";
            w.emitOrdinaryProbeForTest("[H7_RESOLVE]", filler.c_str());
            CHECK(cap.lines.size() == 1, "the oversized message still emitted one line");
            CHECK(cap.lines[0].find("[H7_", 1) == std::string::npos,
                  "even a message truncated by the renderer carries no second token");
        }
    }

    const int total = pass + fail;
    SR_LOG_BOOT(fail ? sr::LogSeverity::Error : sr::LogSeverity::Info,
                "[H7_TEST] %d/%d passed", pass, total);
    return fail;
}

}  // namespace test

// Returns -1 when the test was NOT requested, otherwise the FAILURE COUNT (0 = all
// passed). main() decides this BEFORE the zombie killer runs and returns immediately,
// so the probe can be proved before a live roll is authorized.
//
// NARROWED (director): the guarantee is over effects CONTROLLED INSIDE `SDL_main` — the
// zombie killer, log init and guest boot all sit after the gate. It is NOT a guarantee
// about the process: static initializers and SDL's own `WinMain` run before `main`
// entirely and no gate placed inside `SDL_main` can precede them. Our static
// initializers have not been audited for side effects.
int runSelfTestIfRequested()
{
    const char* e = std::getenv("SR_H7_ENTRYWATCH_TEST");
    if (!e || e[0] != '1') return -1;
    return test::run();
}

}  // namespace sr::kernel::h7
