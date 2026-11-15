// iter-455l r4: THE PROVENANCE RING — director-authorized observe-only instrument
// (authorization: ledger 805e760; nine-defect rework ruling: c46fcd9). Answers ONE
// question: which producer sequence populates the pool surface whose resolve fills
// the runtime front key that hlePresentFront copies onto the backbuffer, at the
// moment the presented frame first becomes EXACT full-frame RGB black.
//
//   producer events → source pool surface → resolve into runtime frontKey
//                   → hlePresentFront copy → backbuffer
//
// ⚠ OBSERVE-ONLY. No guest writes, no behavior change, no SR_ALLOW_FORCING (this is
// not forcing), no ENTRYWATCH extension. Gate: SR_PROV_RING=1 (default OFF).
//
// ── The nine-defect rework, encoded structurally ─────────────────────────────
// 1. THREADS: onAptRequest() runs on the GUEST thread; every other feed runs on the
//    render thread. The arm is an atomic REQUEST consumed by the render thread at
//    its next feed; all mutable capture state is render-thread-owned. The guest
//    thread touches nothing but the atomic.
// 2. PRODUCER BOUNDARY (r3, ruling ad4621a): a draw is fed ONLY through
//    feedPoolDraw(), called from the SINGLE post-success chokepoint inside
//    recordHleBatchToPool — so BOTH live callers (the in-order submit path and the
//    renderHleDraws fallback/held-list loop) feed identically, with the POOL PSO.
//    A held batch re-recorded across presents feeds once per re-record: each
//    re-record IS another pool draw on that surface (truthful duplication, tested).
//    The provenance payload (constant hashes, c0/c1, tex binds, render state) is
//    precomputed at SUBMIT via makePoolDrawFeed() — the only moment the draw's CPU
//    constant blocks are alive — and travels on the HleBatch.
// 3. SNAPSHOT TIME: the source tail is FROZEN AT THE RECORDED RESOLVE (a copy taken
//    inside onResolve, stored with the per-destination record), never at Present —
//    a draw after the resolve cannot be named producer of pixels already copied
//    away. Carried frames reference the same frozen snapshot by ID.
// 4. CARRY SCOPE: a frame with no same-frame resolve match reports an OLDER
//    tracker entry only for ITS OWN front key (age = frames since that resolve) —
//    a new front texture never inherits another texture's provenance.
// 5. POST-CAP SELECTION: the per-destination tracker records EVERY recorded
//    resolve, independent of the 16-row detail cap, so a matching resolve beyond
//    the cap still wins (marked detailRowDropped). If any recorded resolve ever
//    failed to enter the tracker (overflow), a subsequent miss is reported
//    UNKNOWN-OVERFLOW — never silently wrong. (r3, ruling ad4621a:) the
//    detailRowDropped proof compares the resolve's UNIQUE snapId, never the draw
//    ordinal — consecutive resolves routinely share drawOrdinalNow(), and an
//    ordinal compare let an earlier retained row fake the dropped row's retention.
// 6. DUMP COMPLETENESS: every retained resolve row is emitted; every UNIQUE
//    selected snapshot is emitted once by ID; carried frames reference that ID.
// 7. IDENTITIES: resolve source format + host resource travel with the record;
//    texture keys keep their SAMPLER INDICES; the guest's effective render state
//    (RB_DEPTHCONTROL / RB_BLENDCONTROL0 / RB_COLOR_MASK + stateValid) travels
//    with each draw; scissor is printed; the POOL PSO is the recorded identity.
// 8. HONEST NAMES: these calls witness COMMAND RECORDING, not GPU completion —
//    fields are `recorded`/`copyRecorded`, and every refusal code renders with the
//    label its producer defined (copy: 0 ok, 1 noKey, 2 registryMiss,
//    3 blitRefused, 4 gateOff; resolve: ResolveSkip values, 10 rect, 11 sliced).
// 9. TRIGGER DEPENDENCY: the trigger lives inside the SR_PRESENT_DUMP readback.
//    Arming without that readback enabled announces ARMED-BUT-BLIND at arm time
//    (tested) — never a silent forever-wait. (r3, ruling ad4621a:) scan
//    eligibility is SEPARATE from file-output caps — scanEligible() keeps the
//    readback scanning every present while the ring is enabled, so the EVERY=1
//    240-file cap (exhausted ~8s in, long before main_hud) can no longer blind
//    the trigger after AVAILABLE was announced.
//
// Bounds (every one announces itself; no silent drops): 128-frame ring; 16 resolve
// detail rows per frame; 24-entry per-destination tracker (production dests=16);
// 12-event producer tails; 64 tracked surfaces.

#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sr::gfx::prov {

constexpr uint32_t kFrames = 128;
constexpr uint32_t kResolvesPerFrame = 16;   // detail rows
constexpr uint32_t kDestTrack = 24;          // per-destination tracker
constexpr uint32_t kTailLen = 12;
constexpr uint32_t kSurfaces = 64;

struct Sink {
    void (*emit)(void* ud, const char* line);
    void* ud;
};

struct TexBind { uint8_t slot = 0; uint32_t key = 0; };

// r3 (ruling ad4621a, defect 3): the readback SCAN must not die with the file
// caps. Pure and testable: the readback runs iff the dump gate is on AND either
// the file path would sample this present (legacy behavior) or the ring WANTS the
// scan. r4 (ruling 008481a): "wants" is the BOUNDED window Ring::scanWanted() —
// true only from the consumed arm through the one-shot dump — never the
// enabled()-for-lifetime gate, which forced the synchronous readback + full-frame
// CPU scan + [PRESENT_SIG] on every present from boot and forever after the dump.
constexpr bool scanEligible(bool presentDumpOn, bool ringScanWanted,
                            bool fileWouldSample) {
    return presentDumpOn && (ringScanWanted || fileWouldSample);
}

inline uint64_t fnvBytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// r3 (ruling ad4621a, defect 1): the provenance payload of one submitted draw,
// precomputed at SUBMIT (the only moment d.vsConst/d.psConst are alive) and
// carried on the HleBatch so the post-success chokepoint inside
// recordHleBatchToPool can feed the ring from EITHER caller.
struct PoolDrawFeed {
    uint64_t ordinal = 0;
    uint64_t vsHash = 0, psHash = 0;
    uint64_t vsConstHash = 0, psConstHash = 0;
    float c0[4] = {}, c1[4] = {};
    bool constNull = true;
    TexBind tex[4];                          // first 4 NONZERO binds, slots kept
    uint32_t texNonzeroTotal = 0;
    uint32_t depthControl = 0, blendControl = 0, colorMask = 0;
    bool stateValid = false;
};

PoolDrawFeed makePoolDrawFeed(uint64_t ordinal, uint64_t vsHash, uint64_t psHash,
                              const uint8_t* vsConst, const uint8_t* psConst,
                              const uint32_t* texKey16, uint32_t depthControl,
                              uint32_t blendControl, uint32_t colorMask,
                              bool stateValid);

struct ProducerEvent {
    enum Kind : uint8_t { None, Clear, Draw };
    Kind kind = None;
    uint64_t ordinal = 0;
    float clearRgba[4] = {};
    uint64_t vsHash = 0, psHash = 0;
    uint64_t vsConstHash = 0, psConstHash = 0;
    float c0[4] = {}, c1[4] = {};
    bool constNull = false;
    TexBind tex[4];                          // first 4 NONZERO binds WITH sampler slots
    uint32_t texNonzeroTotal = 0;            // full count (truncation announced)
    float viewport[4] = {};
    uint32_t scissor[4] = {};
    uint32_t topology = 0, vertexCount = 0;
    // Defect 7: the guest's effective render state from the draw's register shadow.
    uint32_t depthControl = 0, blendControl = 0, colorMask = 0;
    bool stateValid = false;
    const void* poolPso = nullptr;           // the POOL pipeline (defect 2)
};

struct SurfaceTail {
    const void* surf = nullptr;
    uint32_t keyHint = 0;
    ProducerEvent ev[kTailLen];
    uint32_t next = 0;
    uint64_t total = 0;
};

struct ResolveRec {
    uint64_t ordinal = 0;
    uint32_t rawDst = 0, normDst = 0;
    const void* srcSurf = nullptr;
    const void* srcHostRes = nullptr;        // defect 7
    uint32_t srcKeyHint = 0;
    uint32_t w = 0, h = 0;
    uint32_t srcFmt = 0;                     // defect 7 (DXGI value)
    uint32_t rectFlags = 0;
    bool recorded = false;                   // defect 8: command recorded, not GPU done
    uint32_t refuseReason = 0;
    uint64_t snapId = 0;                     // frozen-at-resolve tail snapshot (0=none)
};

struct FrameRec {
    uint64_t presentNo = 0;
    uint32_t frontKey = 0, w = 0, h = 0, slot = 0, fmt = 0;
    const void* hostRes = nullptr;
    bool copyRecorded = false;               // defect 8
    uint32_t copyRefuseReason = 0;
    ResolveRec resolves[kResolvesPerFrame];
    uint32_t resolveCount = 0;
    uint64_t resolveTotal = 0;
    ResolveRec match;
    bool matchValid = false;
    bool matchUnknownOverflow = false;       // defect 5
    bool matchDetailDropped = false;         // matched via tracker; detail row dropped
    uint32_t matchAge = 0;                   // 0 fresh; N = resolved N presents ago
    // Defect 3: the SELECTED snapshot, copied from the tracker's frozen-at-resolve
    // copy — never from the live surface. Carried frames keep the same snapId.
    SurfaceTail selSnap;
    uint64_t selSnapId = 0;
};

class Ring {
public:
    // triggerAvailable: whether the SR_PRESENT_DUMP readback (the trigger's host)
    // is enabled this run — defect 9, announced at arm time.
    explicit Ring(Sink sink, bool triggerAvailable)
        : m_sink(sink), m_trigAvailable(triggerAvailable) {}

    // ── GUEST THREAD: the only cross-thread entry; touches ONLY the atomic ───
    void onAptRequest(const char* path) {
        if (!path) return;
        if (std::strcmp(path, "screens/hud/main_hud") != 0) return;
        m_armReq.store(true, std::memory_order_release);
    }

    bool armed() const { return m_armed; }   // render-thread-owned; tests only
    bool dumped() const { return m_dumped; }

    // r4 (ruling 008481a): the SCAN WINDOW — render-thread-owned, true only from
    // the consumed arm through the one-shot dump. Called at the dumpBackBuffer
    // boundary; consumes a pending arm itself (requirement 2), so an arm no other
    // render feed latched still opens the window at the very next present.
    bool scanWanted() {
        consumeArm();
        return m_armed && !m_dumped;
    }

    // ── RENDER THREAD feeds ──────────────────────────────────────────────────
    void onClear(const void* target, uint32_t keyHint, const float rgba[4],
                 uint64_t ordinal) {
        consumeArm();
        if (!m_armed || m_dumped || !target) return;
        SurfaceTail* t = tailFor(target, keyHint);
        if (!t) return;
        ProducerEvent& e = t->ev[t->next];
        e = ProducerEvent{};
        e.kind = ProducerEvent::Clear;
        e.ordinal = ordinal;
        for (int i = 0; i < 4; i++) e.clearRgba[i] = rgba ? rgba[i] : 0.f;
        t->next = (t->next + 1) % kTailLen;
        t->total++;
    }

    // Reached ONLY via feedPoolDraw() (defect 2's choke point). The payload was
    // precomputed at submit; viewport/scissor/topology/pool PSO arrive from the
    // batch at the chokepoint. Re-feeds of a held batch land as separate events —
    // each re-record is another real pool draw on the surface (no dedupe).
    void onPoolDraw(const void* target, uint32_t keyHint, const PoolDrawFeed& f,
                    const float viewport[4], const uint32_t scissor[4],
                    uint32_t topology, uint32_t vertexCount, const void* poolPso) {
        consumeArm();
        if (!m_armed || m_dumped || !target) return;
        SurfaceTail* t = tailFor(target, keyHint);
        if (!t) return;
        ProducerEvent& e = t->ev[t->next];
        e = ProducerEvent{};
        e.kind = ProducerEvent::Draw;
        e.ordinal = f.ordinal;
        e.vsHash = f.vsHash; e.psHash = f.psHash;
        e.constNull = f.constNull;
        e.vsConstHash = f.vsConstHash; e.psConstHash = f.psConstHash;
        for (int i = 0; i < 4; i++) { e.c0[i] = f.c0[i]; e.c1[i] = f.c1[i]; }
        for (int i = 0; i < 4; i++) e.tex[i] = f.tex[i];
        e.texNonzeroTotal = f.texNonzeroTotal;
        if (viewport) for (int i = 0; i < 4; i++) e.viewport[i] = viewport[i];
        if (scissor)  for (int i = 0; i < 4; i++) e.scissor[i] = scissor[i];
        e.topology = topology; e.vertexCount = vertexCount;
        e.poolPso = poolPso;
        e.depthControl = f.depthControl; e.blendControl = f.blendControl;
        e.colorMask = f.colorMask; e.stateValid = f.stateValid;
        t->next = (t->next + 1) % kTailLen;
        t->total++;
    }

    void onResolve(uint32_t rawDst, uint32_t normDst, const void* srcSurf,
                   const void* srcHostRes, uint32_t srcKeyHint, uint32_t w, uint32_t h,
                   uint32_t srcFmt, uint32_t rectFlags, bool recorded,
                   uint32_t refuseReason, uint64_t ordinal) {
        consumeArm();
        if (!m_armed || m_dumped) return;
        m_frameResolveTotal++;
        ResolveRec r{};
        r.ordinal = ordinal; r.rawDst = rawDst; r.normDst = normDst;
        r.srcSurf = srcSurf; r.srcHostRes = srcHostRes; r.srcKeyHint = srcKeyHint;
        r.w = w; r.h = h; r.srcFmt = srcFmt; r.rectFlags = rectFlags;
        r.recorded = recorded; r.refuseReason = refuseReason;
        if (recorded) {
            r.snapId = ++m_snapIdCounter;
            trackDest(r);                    // defect 3+5: freeze the tail NOW,
        }                                    // independent of the detail cap
        if (m_frameResolveCount < kResolvesPerFrame)
            m_frameResolves[m_frameResolveCount++] = r;
    }

    void onPresent(uint64_t presentNo, uint32_t frontKey, uint32_t w, uint32_t h,
                   uint32_t slot, const void* hostRes, uint32_t fmt, bool copyRecorded,
                   uint32_t copyRefuseReason) {
        consumeArm();
        if (!m_armed || m_dumped) return;
        FrameRec& f = m_frames[m_frameNext];
        f = FrameRec{};
        f.presentNo = presentNo;
        f.frontKey = frontKey; f.w = w; f.h = h; f.slot = slot;
        f.hostRes = hostRes; f.fmt = fmt;
        f.copyRecorded = copyRecorded; f.copyRefuseReason = copyRefuseReason;
        f.resolveCount = m_frameResolveCount;
        f.resolveTotal = m_frameResolveTotal;
        for (uint32_t i = 0; i < m_frameResolveCount; i++) f.resolves[i] = m_frameResolves[i];
        // Selection: the tracker's entry for THIS front key — fresh if stamped this
        // frame, otherwise carried WITH AGE for the SAME key only (defect 4). The
        // snapshot is the tracker's frozen-at-resolve copy (defect 3).
        const DestLast* dl = findDest(frontKey);
        if (dl) {
            f.match = dl->rec;
            f.matchValid = true;
            f.matchAge = (uint32_t)(m_frameStamp - dl->frameStamp);
            f.matchDetailDropped = (f.matchAge == 0) && !detailHas(dl->rec);
            f.selSnap = dl->snap;
            f.selSnapId = dl->rec.snapId;
        } else if (m_destOverflows > 0) {
            // A recorded resolve was lost to tracker overflow at some point, so
            // "no entry" cannot be distinguished from "the lost one matched".
            f.matchUnknownOverflow = true;
        }
        m_frameNext = (m_frameNext + 1) % kFrames;
        if (m_frameCount < kFrames) m_frameCount++; else m_frameOverwrites++;
        m_frameResolveCount = 0;
        m_frameResolveTotal = 0;
        m_frameStamp++;
    }

    void onBackbufferScan(uint64_t nonBlackPx, uint64_t totalPx) {
        consumeArm();
        if (!m_armed || m_dumped) return;
        if (totalPx == 0 || nonBlackPx != 0) return;
        m_dumped = true;
        dump();
    }

    uint32_t frameCount() const { return m_frameCount; }
    const FrameRec* frameAt(uint32_t backIdx) const {
        if (backIdx >= m_frameCount) return nullptr;
        uint32_t idx = (m_frameNext + kFrames - 1 - backIdx) % kFrames;
        return &m_frames[idx];
    }

private:
    struct DestLast {
        uint32_t normDst = 0;
        uint64_t frameStamp = 0;
        ResolveRec rec;
        SurfaceTail snap;                    // frozen at rec's resolve (defect 3)
        bool used = false;
    };

    void consumeArm() {
        if (m_armed) return;
        if (!m_armReq.load(std::memory_order_acquire)) return;
        m_armed = true;
        if (m_trigAvailable)
            emit("[PROV_RING] ARMED on screens/hud/main_hud (ring=%u frames; trigger "
                 "hosted by the SR_PRESENT_DUMP readback, AVAILABLE)", kFrames);
        else
            emit("[PROV_RING] ARMED-BUT-BLIND on screens/hud/main_hud: the trigger "
                 "lives in the SR_PRESENT_DUMP readback, which is OFF this run — "
                 "capture runs but no dump can ever fire. Set SR_PRESENT_DUMP=1.");
    }

    SurfaceTail* tailFor(const void* surf, uint32_t keyHint) {
        for (uint32_t i = 0; i < m_tailCount; i++)
            if (m_tails[i].surf == surf) {
                if (keyHint && !m_tails[i].keyHint) m_tails[i].keyHint = keyHint;
                return &m_tails[i];
            }
        if (m_tailCount >= kSurfaces) { m_tailDropped++; return nullptr; }
        SurfaceTail& t = m_tails[m_tailCount++];
        t = SurfaceTail{};
        t.surf = surf; t.keyHint = keyHint;
        return &t;
    }
    const SurfaceTail* findTail(const void* surf) const {
        for (uint32_t i = 0; i < m_tailCount; i++)
            if (m_tails[i].surf == surf) return &m_tails[i];
        return nullptr;
    }

    void trackDest(const ResolveRec& r) {
        DestLast* d = nullptr;
        for (uint32_t i = 0; i < kDestTrack && !d; i++)
            if (m_dest[i].used && m_dest[i].normDst == r.normDst) d = &m_dest[i];
        for (uint32_t i = 0; i < kDestTrack && !d; i++)
            if (!m_dest[i].used) { d = &m_dest[i]; d->used = true; d->normDst = r.normDst; }
        if (!d) { m_destOverflows++; return; }
        d->rec = r;
        d->frameStamp = m_frameStamp;
        const SurfaceTail* t = r.srcSurf ? findTail(r.srcSurf) : nullptr;
        if (t) d->snap = *t;
        else { d->snap = SurfaceTail{}; d->snap.surf = r.srcSurf; }
    }
    const DestLast* findDest(uint32_t normDst) const {
        for (uint32_t i = 0; i < kDestTrack; i++)
            if (m_dest[i].used && m_dest[i].normDst == normDst) return &m_dest[i];
        return nullptr;
    }
    // Identity is the resolve's UNIQUE snapId — never the draw ordinal, which
    // consecutive resolves share (r3 defect 2: an earlier retained row with the
    // same ordinal faked the dropped 17th row's retention).
    bool detailHas(const ResolveRec& r) const {
        for (uint32_t i = 0; i < m_frameResolveCount; i++)
            if (m_frameResolves[i].snapId == r.snapId && r.snapId) return true;
        return false;
    }

    void emit(const char* fmt2, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 2, 3)))
#endif
    {
        char line[768];
        va_list ap; va_start(ap, fmt2);
        vsnprintf(line, sizeof(line), fmt2, ap);
        va_end(ap);
        m_sink.emit(m_sink.ud, line);
    }

    static const char* copyReasonName(uint32_t r) {
        switch (r) {
            case 0: return "ok";
            case 1: return "noKey";
            case 2: return "registryMiss";
            case 3: return "blitRefused";
            case 4: return "gateOff";
        }
        return "unnamed";
    }

    void emitTail(const SurfaceTail& t, uint64_t id) {
        const uint64_t kept = t.total < kTailLen ? t.total : kTailLen;
        emit("[PROV_RING] snapshot id=%llu surf=%p key=%08X events kept=%llu of %llu "
             "(older truncated=%llu)",
             (unsigned long long)id, t.surf, t.keyHint, (unsigned long long)kept,
             (unsigned long long)t.total, (unsigned long long)(t.total - kept));
        for (uint64_t i = 0; i < kept; i++) {
            uint32_t idx = (t.next + kTailLen - 1 - uint32_t(i)) % kTailLen;
            const ProducerEvent& e = t.ev[idx];
            if (e.kind == ProducerEvent::Clear) {
                emit("[PROV_RING]   tail[-%llu] CLEAR ord=%llu rgba=(%.3f,%.3f,%.3f,%.3f)",
                     (unsigned long long)i, (unsigned long long)e.ordinal,
                     e.clearRgba[0], e.clearRgba[1], e.clearRgba[2], e.clearRgba[3]);
            } else if (e.kind == ProducerEvent::Draw) {
                emit("[PROV_RING]   tail[-%llu] DRAW ord=%llu vs=%016llX ps=%016llX "
                     "vsCF=%016llX psCF=%016llX%s c0=(%.3f,%.3f,%.3f,%.3f) "
                     "c1=(%.3f,%.3f,%.3f,%.3f) tex=[s%u:%08X s%u:%08X s%u:%08X s%u:%08X "
                     "of %u] vp=(%.0f,%.0f,%.0f,%.0f) sc=(%u,%u,%u,%u) topo=%u n=%u "
                     "poolPso=%p depthCtl=%08X blendCtl=%08X colorMask=%08X stateValid=%d",
                     (unsigned long long)i, (unsigned long long)e.ordinal,
                     (unsigned long long)e.vsHash, (unsigned long long)e.psHash,
                     (unsigned long long)e.vsConstHash, (unsigned long long)e.psConstHash,
                     e.constNull ? " constFile=NULL" : "",
                     e.c0[0], e.c0[1], e.c0[2], e.c0[3],
                     e.c1[0], e.c1[1], e.c1[2], e.c1[3],
                     e.tex[0].slot, e.tex[0].key, e.tex[1].slot, e.tex[1].key,
                     e.tex[2].slot, e.tex[2].key, e.tex[3].slot, e.tex[3].key,
                     e.texNonzeroTotal,
                     e.viewport[0], e.viewport[1], e.viewport[2], e.viewport[3],
                     e.scissor[0], e.scissor[1], e.scissor[2], e.scissor[3],
                     e.topology, e.vertexCount, e.poolPso,
                     e.depthControl, e.blendControl, e.colorMask, (int)e.stateValid);
            }
        }
    }

    void dump() {
        emit("[PROV_RING] DUMP: exact full-frame RGB zero first observed; frames=%u "
             "(ring %u; overwritten=%llu) tailSurfaces=%u/%u droppedSurfaces=%llu "
             "destTracker=%u/%u trackerOverflows=%llu — every bound announces its "
             "own truncation",
             m_frameCount, kFrames, (unsigned long long)m_frameOverwrites,
             m_tailCount, kSurfaces, (unsigned long long)m_tailDropped,
             destUsed(), kDestTrack, (unsigned long long)m_destOverflows);
        // Frames oldest → newest, each with EVERY retained resolve row (defect 6).
        for (uint32_t b = m_frameCount; b-- > 0;) {
            const FrameRec* f = frameAt(b);
            if (!f) continue;
            emit("[PROV_RING] frame p=%llu front=%08X %ux%u slot=%u host=%p fmt=%u "
                 "copy=%s(%s) resolves=%u/%llu match=%s%s%s age=%u snapId=%llu "
                 "matchOrd=%llu src=%p srcKey=%08X srcHost=%p srcFmt=%u",
                 (unsigned long long)f->presentNo, f->frontKey, f->w, f->h, f->slot,
                 f->hostRes, f->fmt,
                 f->copyRecorded ? "recorded" : "REFUSED",
                 copyReasonName(f->copyRefuseReason),
                 f->resolveCount, (unsigned long long)f->resolveTotal,
                 f->matchUnknownOverflow ? "UNKNOWN-OVERFLOW"
                                         : (f->matchValid ? "yes" : "NONE"),
                 f->matchDetailDropped ? "(detailRowDropped)" : "",
                 (f->matchValid && f->matchAge) ? "(carried)" : "",
                 f->matchAge, (unsigned long long)f->selSnapId,
                 (unsigned long long)(f->matchValid ? f->match.ordinal : 0),
                 f->matchValid ? f->match.srcSurf : nullptr,
                 f->matchValid ? f->match.srcKeyHint : 0,
                 f->matchValid ? f->match.srcHostRes : nullptr,
                 f->matchValid ? f->match.srcFmt : 0);
            for (uint32_t i = 0; i < f->resolveCount; i++) {
                const ResolveRec& r = f->resolves[i];
                emit("[PROV_RING]   resolve ord=%llu raw=%08X norm=%08X src=%p host=%p "
                     "key=%08X %ux%u srcFmt=%u rectFlags=%08X %s reason=%u snapId=%llu",
                     (unsigned long long)r.ordinal, r.rawDst, r.normDst, r.srcSurf,
                     r.srcHostRes, r.srcKeyHint, r.w, r.h, r.srcFmt, r.rectFlags,
                     r.recorded ? "recorded" : "REFUSED", r.refuseReason,
                     (unsigned long long)r.snapId);
            }
        }
        // Every UNIQUE selected snapshot, once, by ID (defect 6).
        for (uint32_t b = m_frameCount; b-- > 0;) {
            const FrameRec* f = frameAt(b);
            if (!f || !f->selSnapId) continue;
            bool seen = false;
            for (uint32_t b2 = m_frameCount; b2-- > b + 1;) {
                const FrameRec* g = frameAt(b2);
                if (g && g->selSnapId == f->selSnapId) { seen = true; break; }
            }
            if (!seen) emitTail(f->selSnap, f->selSnapId);
        }
        emit("[PROV_RING] DUMP END (one-shot; instrument now quiescent)");
    }

    uint32_t destUsed() const {
        uint32_t n = 0;
        for (uint32_t i = 0; i < kDestTrack; i++) if (m_dest[i].used) n++;
        return n;
    }

    Sink m_sink;
    const bool m_trigAvailable;
    std::atomic<bool> m_armReq{false};       // the ONLY cross-thread state
    bool m_armed = false;                    // render-thread-owned from here down
    bool m_dumped = false;
    ResolveRec m_frameResolves[kResolvesPerFrame];
    uint32_t m_frameResolveCount = 0;
    uint64_t m_frameResolveTotal = 0;
    uint64_t m_snapIdCounter = 0;
    DestLast m_dest[kDestTrack];
    uint64_t m_frameStamp = 0;
    uint64_t m_destOverflows = 0;
    FrameRec m_frames[kFrames];
    uint32_t m_frameNext = 0, m_frameCount = 0;
    uint64_t m_frameOverwrites = 0;
    SurfaceTail m_tails[kSurfaces];
    uint32_t m_tailCount = 0;
    uint64_t m_tailDropped = 0;
};

// Defect 2's choke point: the ONLY route a draw takes into the ring. Production
// calls it from exactly ONE site — inside recordHleBatchToPool, after success —
// so both live callers (in-order submit and the renderHleDraws fallback) feed
// identically; the self-test drives the same function, so the pooled gate and the
// no-dedupe re-feed behavior are both mutation-testable.
void feedPoolDraw(Ring& r, bool pooled, const void* target, uint32_t keyHint,
                  const PoolDrawFeed& f, const float viewport[4],
                  const uint32_t scissor[4], uint32_t topology, uint32_t vertexCount,
                  const void* poolPso);

bool enabled();
Ring& ring();
int runSelfTestIfRequested();

}  // namespace sr::gfx::prov
