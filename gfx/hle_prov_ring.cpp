// iter-455l r4: provenance-ring production adapters + the director's acceptance-bar
// self-test (SR_PROV_RING_TEST=1), reworked to the nine-defect ruling (c46fcd9).
// See hle_prov_ring.hpp for the contract.

#include "hle_prov_ring.hpp"
#include "../app/log.hpp"

#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace sr::gfx::prov {

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("SR_PROV_RING");
        return e && e[0] == '1';                    // default OFF (director spec)
    }();
    return on;
}

static void logSink(void*, const char* line) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "%s", line);
}

Ring& ring() {
    // The trigger lives inside dumpBackBuffer's readback, whose own gate is
    // SR_PRESENT_DUMP with e[0]=='1' — mirrored here so an arm without it
    // announces ARMED-BUT-BLIND instead of waiting forever (defect 9).
    static Ring* r = new Ring(Sink{&logSink, nullptr}, [] {
        const char* e = std::getenv("SR_PRESENT_DUMP");
        return e && e[0] == '1';
    }());                                           // ~1 MB — heap, not BSS-stack
    return *r;
}

// r3: the provenance payload is built at SUBMIT, when the draw's CPU constant
// blocks are alive — recordHleBatchToPool runs later (held-list re-records run
// PRESENTS later) when those pointers may dangle.
PoolDrawFeed makePoolDrawFeed(uint64_t ordinal, uint64_t vsHash, uint64_t psHash,
                              const uint8_t* vsConst, const uint8_t* psConst,
                              const uint32_t* texKey16, uint32_t depthControl,
                              uint32_t blendControl, uint32_t colorMask,
                              bool stateValid) {
    PoolDrawFeed f;
    f.ordinal = ordinal; f.vsHash = vsHash; f.psHash = psHash;
    f.constNull = (vsConst == nullptr);
    f.vsConstHash = vsConst ? fnvBytes(vsConst, 4096) : 0;
    f.psConstHash = psConst ? fnvBytes(psConst, 4096) : 0;
    if (vsConst) {
        std::memcpy(f.c0, vsConst, 16);
        std::memcpy(f.c1, vsConst + 16, 16);
    }
    if (texKey16) {
        uint32_t kept = 0, nonzero = 0;
        for (uint8_t i = 0; i < 16; i++) {
            if (!texKey16[i]) continue;
            nonzero++;
            if (kept < 4) { f.tex[kept].slot = i; f.tex[kept].key = texKey16[i]; kept++; }
        }
        f.texNonzeroTotal = nonzero;
    }
    f.depthControl = depthControl; f.blendControl = blendControl;
    f.colorMask = colorMask; f.stateValid = stateValid;
    return f;
}

// Defect 2's choke point: production calls this from exactly ONE site — inside
// recordHleBatchToPool after success — so the in-order caller and the
// renderHleDraws fallback feed identically. The self-test drives the same
// function; the gate and the no-dedupe re-feed behavior are mutation-testable.
void feedPoolDraw(Ring& r, bool pooled, const void* target, uint32_t keyHint,
                  const PoolDrawFeed& f, const float viewport[4],
                  const uint32_t scissor[4], uint32_t topology, uint32_t vertexCount,
                  const void* poolPso) {
    if (!pooled) return;   // a draw the pool refused is NOT a producer (defect 2)
    r.onPoolDraw(target, keyHint, f, viewport, scissor, topology, vertexCount,
                 poolPso);
}

// ── self-test ────────────────────────────────────────────────────────────────
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

bool tailHasOrd(const SurfaceTail& t, uint64_t ord) {
    const uint64_t kept = t.total < kTailLen ? t.total : kTailLen;
    for (uint64_t i = 0; i < kept; i++) {
        uint32_t idx = (t.next + kTailLen - 1 - (uint32_t)i) % kTailLen;
        if (t.ev[idx].ordinal == ord) return true;
    }
    return false;
}

int runTests() {
    int pass = 0, fail = 0;
    auto CHECK = [&](bool ok, const char* what) {
        if (ok) ++pass;
        else { ++fail; SR_LOG_BOOT(sr::LogSeverity::Error, "[PROV_TEST] FAIL: %s", what); }
    };
    const float NAVY[4] = {0.04f, 0.04f, 0.10f, 1.f};
    const float VP[4] = {0, 0, 1280, 720};
    const uint32_t SC[4] = {0, 0, 1280, 720};
    uint8_t constA[4096] = {}; constA[0] = 1;      // c0.x raw byte pattern
    uint32_t tex[16] = {};
    // arm + consume: the atomic request needs a render-thread feed to latch
    auto armNow = [&](Ring* r) {
        r->onAptRequest("screens/hud/main_hud");
        const float z[4] = {};
        r->onClear(nullptr, 0, z, 0);              // null target: consumes arm only
    };
    auto drawOn = [&](Ring* r, bool pooled, const void* S, uint64_t ord,
                      uint64_t vs, const void* pso) {
        PoolDrawFeed f = makePoolDrawFeed(ord, vs, 0xBB, constA, nullptr, tex,
                                          0, 0, 0xF, true);
        feedPoolDraw(*r, pooled, S, 0u, f, VP, SC, 4, 6, pso);
    };

    // 1. NO ARM / NO CAPTURE; exact request arms ONCE via the atomic handoff;
    //    non-exact never arms (superstring + prefix).
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        const void* S = (const void*)0x1000;
        r->onClear(S, 0xAA000000u, NAVY, 1);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r->onBackbufferScan(0, 921600);            // exact black BUT unarmed
        CHECK(r->frameCount() == 0 && !r->dumped() && cap.lines.empty(),
              "unarmed: nothing captured, nothing dumped");
        r->onAptRequest("screens/hud/main_hud2");  // superstring — not exact
        r->onAptRequest("screens/hud/main_hu");    // prefix — not exact
        r->onClear(S, 0, NAVY, 2);
        CHECK(!r->armed(), "non-exact requests never arm");
        armNow(r);
        CHECK(r->armed() && cap.count("ARMED") == 1, "the exact request arms");
        armNow(r);
        CHECK(cap.count("ARMED") == 1, "arming is once-only");
        delete r;
    }

    // 2. ATOMIC TWO-THREAD ARM HANDOFF (defect 1): the guest thread sets only the
    //    request; the render thread latches it at its next feed.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        std::thread guest([&] { r->onAptRequest("screens/hud/main_hud"); });
        guest.join();
        CHECK(!r->armed(), "the guest thread's request is pending, not yet latched");
        r->onClear((const void*)0x1000, 0, NAVY, 1);
        CHECK(r->armed() && cap.count("ARMED") == 1,
              "the render thread consumes the atomic arm at its next feed");
        delete r;
    }

    // 3. TRIGGER DEPENDENCY (defect 9): arming with the SR_PRESENT_DUMP readback
    //    unavailable announces ARMED-BUT-BLIND; with it available announces AVAILABLE.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, false);
        armNow(r);
        CHECK(cap.count("ARMED-BUT-BLIND") == 1 && cap.has("SR_PRESENT_DUMP"),
              "arming without the readback announces ARMED-BUT-BLIND");
        delete r;
        Cap cap2; Ring* r2 = new Ring(Sink{&Cap::emit, &cap2}, true);
        armNow(r2);
        CHECK(cap2.has("AVAILABLE") && !cap2.has("ARMED-BUT-BLIND"),
              "arming with the readback announces it available");
        delete r2;
    }

    // 4. SAME-FRAME FRONT-KEY CHANGE selects THIS frame's key, never the prior one.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0, * SB = (const void*)0xB0;
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, (const void*)0xA0A, 0x11u,
                     1280, 720, 28, 0, true, 0, 10);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, (const void*)0xA0A, 0x11u,
                     1280, 720, 28, 0, true, 0, 20);
        r->onResolve(0x8B000000u, 0x0B000000u, SB, (const void*)0xB0B, 0x22u,
                     1280, 720, 28, 0, true, 0, 21);
        r->onPresent(2, 0x0B000000u, 1280, 720, 3, (const void*)0x78, 28, true, 0);
        const FrameRec* f = r->frameAt(0);
        CHECK(f && f->matchValid && f->match.srcSurf == SB && f->match.ordinal == 21,
              "same-frame front-key change selects THIS frame's key");
        CHECK(f->matchAge == 0, "and the match is fresh, not carried");
        delete r;
    }

    // 5. UNRELATED RESOLVES EXCLUDED; the LAST of multiple RECORDED matches wins;
    //    a REFUSED matching resolve after the last recorded one does NOT win.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* S1 = (const void*)0xA1, * S2 = (const void*)0xA2,
                  * SX = (const void*)0xEE;
        r->onResolve(0x8A94E000u, 0x0A94E000u, S1, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 30);
        r->onResolve(0x8C000000u, 0x0C000000u, SX, nullptr, 0x33u, 640, 360, 28,
                     0, true, 0, 31);
        r->onResolve(0x8A94E000u, 0x0A94E000u, S2, nullptr, 0x22u, 1280, 720, 28,
                     0, true, 0, 32);
        r->onResolve(0x8A94E000u, 0x0A94E000u, S1, nullptr, 0x11u, 1280, 720, 28,
                     0, false, 5, 33);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f = r->frameAt(0);
        CHECK(f && f->matchValid && f->match.srcSurf == S2 && f->match.ordinal == 32,
              "last RECORDED matching resolve wins; unrelated and refused excluded");
        delete r;
    }

    // 6. CARRY-FORWARD across presents with no new resolve, with age, SAME KEY ONLY
    //    (defect 4): a front-key change without a matching resolve does NOT carry.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0;
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 40);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r->onPresent(2, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r->onPresent(3, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f0 = r->frameAt(0);   // newest (present 3)
        const FrameRec* f2 = r->frameAt(2);   // oldest (present 1)
        CHECK(f2 && f2->matchValid && f2->matchAge == 0, "resolve frame matches fresh");
        CHECK(f0 && f0->matchValid && f0->match.ordinal == 40 && f0->matchAge == 2,
              "provenance persists across presents with its AGE");
        // the guest switches to a NEVER-RESOLVED front key: no carry, honest NONE
        r->onPresent(4, 0x0D000000u, 1280, 720, 3, (const void*)0x79, 28, true, 0);
        const FrameRec* fn = r->frameAt(0);
        CHECK(fn && !fn->matchValid && !fn->matchUnknownOverflow,
              "a front-key change without a matching resolve does NOT carry");
        r->onBackbufferScan(0, 921600);
        CHECK(cap.has("match=NONE"), "the dump reports the no-carry frame as NONE");
        delete r;
    }

    // 7. PRODUCER BOUNDARY (defect 2): a pool-rejected draw is ABSENT; an accepted
    //    draw carries the POOL PSO.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0, * PSO = (const void*)0x9F00;
        drawOn(r, /*pooled=*/false, SA, 501, 0x1111, (const void*)0xBAD);
        drawOn(r, /*pooled=*/true, SA, 502, 0x2222, PSO);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 503);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f = r->frameAt(0);
        CHECK(f && f->matchValid && f->selSnap.total == 1 && !tailHasOrd(f->selSnap, 501),
              "a pool-rejected draw is absent from the producer tail");
        uint32_t newest = (f->selSnap.next + kTailLen - 1) % kTailLen;
        CHECK(tailHasOrd(f->selSnap, 502) && f->selSnap.ev[newest].poolPso == PSO,
              "the accepted draw is present and carries the POOL PSO");
        delete r;
    }

    // 8. SNAPSHOT TIME (defect 3): draw A -> resolve -> draw B -> Present
    //    attributes A, not B; carried frames keep the SAME frozen tail (draw C
    //    after the resolve never enters it).
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0;
        drawOn(r, true, SA, 601, 0xAAAA, (const void*)0x9F01);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 610);
        drawOn(r, true, SA, 602, 0xBBBB, (const void*)0x9F01);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f = r->frameAt(0);
        CHECK(f && f->matchValid && tailHasOrd(f->selSnap, 601) &&
              !tailHasOrd(f->selSnap, 602),
              "the tail is frozen AT the resolve: post-resolve draw B is absent");
        const uint64_t frozenId = f->selSnapId;
        CHECK(frozenId != 0, "the frozen snapshot has an ID");
        drawOn(r, true, SA, 603, 0xCCCC, (const void*)0x9F01);
        r->onPresent(2, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r->onPresent(3, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* fc = r->frameAt(0);
        CHECK(fc && fc->matchValid && fc->matchAge == 2 && fc->selSnapId == frozenId &&
              fc->selSnap.total == 1 && !tailHasOrd(fc->selSnap, 603),
              "carried frames keep the original frozen tail by ID");
        delete r;
    }

    // 9. POST-CAP SELECTION (defect 5): a matching resolve beyond the 16-row
    //    detail cap still wins, marked detailRowDropped; tracker overflow makes a
    //    miss explicitly UNKNOWN.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0;
        for (uint32_t i = 0; i < kResolvesPerFrame; i++)   // 16 rows, OTHER dests
            r->onResolve(0x81000000u + i * 0x10000u, 0x01000000u + i * 0x10000u,
                         SA, nullptr, 0x11u, 640, 360, 28, 0, true, 0, 700 + i);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 750);                     // the 17th: detail-dropped
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f = r->frameAt(0);
        CHECK(f && f->matchValid && f->match.ordinal == 750 && f->matchDetailDropped,
              "a matching resolve beyond the detail cap still wins, marked dropped");
        CHECK(f->resolveCount == kResolvesPerFrame && f->resolveTotal == 17,
              "the detail rows keep the truthful kept/total");
        r->onBackbufferScan(0, 921600);
        CHECK(cap.has("(detailRowDropped)"), "the dump marks the dropped detail row");
        delete r;
        // tracker overflow: 25 distinct destinations, then present the 25th
        Cap cap2; Ring* r2 = new Ring(Sink{&Cap::emit, &cap2}, true);
        armNow(r2);
        for (uint32_t i = 0; i < kDestTrack + 1; i++)
            r2->onResolve(0x82000000u + i * 0x10000u, 0x02000000u + i * 0x10000u,
                          SA, nullptr, 0x11u, 640, 360, 28, 0, true, 0, 800 + i);
        r2->onPresent(1, 0x02000000u + kDestTrack * 0x10000u, 1280, 720, 3,
                      (const void*)0x77, 28, true, 0);
        const FrameRec* f2 = r2->frameAt(0);
        CHECK(f2 && !f2->matchValid && f2->matchUnknownOverflow,
              "a miss after tracker overflow is explicitly UNKNOWN");
        r2->onBackbufferScan(0, 921600);
        CHECK(cap2.has("UNKNOWN-OVERFLOW") && cap2.has("trackerOverflows=1"),
              "the dump reports the unknown and counts the overflow");
        delete r2;
    }

    // 10. PRODUCER HISTORIES: clear-only, clear-then-draw ordered, multi-target
    //     isolation, constant fingerprint travel.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0, * SB = (const void*)0xB0;
        r->onClear(SA, 0x11u, NAVY, 100);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 101);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f = r->frameAt(0);
        CHECK(f && f->selSnap.surf == SA && f->selSnap.total == 1 &&
              f->selSnap.ev[0].kind == ProducerEvent::Clear,
              "clear-only producer history snapshots");
        r->onClear(SB, 0x22u, NAVY, 110);
        drawOn(r, true, SA, 111, 0x1111, (const void*)0x5000);
        PoolDrawFeed fb = makePoolDrawFeed(112, 0x3333, 0x4444, constA, nullptr, tex,
                                           0, 0, 0xF, true);
        feedPoolDraw(*r, true, SB, 0x22u, fb, VP, SC, 4, 6, (const void*)0x5001);
        r->onResolve(0x8B000000u, 0x0B000000u, SB, nullptr, 0x22u, 1280, 720, 28,
                     0, true, 0, 113);
        r->onPresent(2, 0x0B000000u, 1280, 720, 3, (const void*)0x78, 28, true, 0);
        f = r->frameAt(0);
        CHECK(f && f->selSnap.surf == SB && f->selSnap.total == 2,
              "multi-target: only the SOURCE surface's history is snapshotted");
        uint32_t newest = (f->selSnap.next + kTailLen - 1) % kTailLen;
        CHECK(f->selSnap.ev[newest].kind == ProducerEvent::Draw &&
              f->selSnap.ev[newest].vsHash == 0x3333 &&
              f->selSnap.ev[newest].vertexCount == 6,
              "clear-then-draw ordered tail with the draw newest");
        CHECK(f->selSnap.ev[newest].vsConstHash != 0 &&
              f->selSnap.ev[newest].c0[0] != 0.f,
              "the constant fingerprint travels with the draw");
        delete r;
    }

    // 11. TRIGGER: exact black once; uniform NONblack never; nonuniform sharing one
    //     4:4:4 histogram bucket never; empty readback never; one-shot.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r->onBackbufferScan(921600, 921600);
        CHECK(!r->dumped(), "uniform nonblack does not trigger");
        r->onBackbufferScan(17, 921600);
        CHECK(!r->dumped(), "nonuniform pixels sharing one histogram bucket do not trigger");
        r->onBackbufferScan(0, 0);
        CHECK(!r->dumped(), "an empty readback never triggers");
        r->onBackbufferScan(0, 921600);
        CHECK(r->dumped() && cap.count("DUMP END") == 1, "exact full-frame zero dumps once");
        r->onBackbufferScan(0, 921600);
        CHECK(cap.count("DUMP END") == 1, "the dump is one-shot");
        delete r;
    }

    // 12. DUMP COMPLETENESS (defect 6): every retained resolve row is emitted; TWO
    //     distinct pre-black snapshots both appear, each once, referenced by ID.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0, * SB = (const void*)0xB0;
        drawOn(r, true, SA, 901, 0xAAAA, (const void*)0x9F02);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 902);
        r->onResolve(0x8C000000u, 0x0C000000u, SA, nullptr, 0x33u, 640, 360, 28,
                     0, false, 5, 903);               // refused row must still render
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        drawOn(r, true, SB, 911, 0xBBBB, (const void*)0x9F03);
        r->onResolve(0x8B000000u, 0x0B000000u, SB, nullptr, 0x22u, 1280, 720, 28,
                     0, true, 0, 912);
        r->onPresent(2, 0x0B000000u, 1280, 720, 3, (const void*)0x78, 28, true, 0);
        r->onPresent(3, 0x0B000000u, 1280, 720, 3, (const void*)0x78, 28, true, 0);
        r->onBackbufferScan(0, 921600);
        CHECK(cap.count("resolve ord=") == 3,
              "every retained resolve row is emitted, including the refused one");
        CHECK(cap.has("REFUSED reason=5"), "the refused row renders its reason");
        CHECK(cap.count("snapshot id=") == 2,
              "both distinct pre-black snapshots are emitted, each once");
        CHECK(cap.has("tail[-0] DRAW ord=901") && cap.has("tail[-0] DRAW ord=911"),
              "each snapshot carries its own frame's producer draw");
        delete r;
    }

    // 13. REFUSAL LABELS (defect 8): every copy-refusal code renders with the label
    //     its producer defined.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r->onPresent(2, 0, 1280, 720, 0, nullptr, 0, false, 1);
        r->onPresent(3, 0x0A94E000u, 1280, 720, 0, nullptr, 0, false, 2);
        r->onPresent(4, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, false, 3);
        r->onPresent(5, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, false, 4);
        r->onBackbufferScan(0, 921600);
        CHECK(cap.has("copy=recorded(ok)"), "reason 0 renders recorded(ok)");
        CHECK(cap.has("copy=REFUSED(noKey)"), "reason 1 renders noKey");
        CHECK(cap.has("copy=REFUSED(registryMiss)"), "reason 2 renders registryMiss");
        CHECK(cap.has("copy=REFUSED(blitRefused)"), "reason 3 renders blitRefused");
        CHECK(cap.has("copy=REFUSED(gateOff)"), "reason 4 renders gateOff");
        delete r;
    }

    // 14. IDENTITY SENTINELS (defect 7): source host resource + format, sampler
    //     index, effective render state, scissor, and pool PSO all reach the log.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0;
        uint32_t texS[16] = {}; texS[7] = 0xFEED0007u;
        const float vpS[4] = {0, 0, 1280, 720};
        const uint32_t scS[4] = {1, 2, 3, 4};
        PoolDrawFeed fs = makePoolDrawFeed(951, 0x5555, 0x6666, constA, nullptr,
                                           texS, 0xDEADD001u, 0xB1E2DC01u,
                                           0x0000000Fu, true);
        feedPoolDraw(*r, true, SA, 0x11u, fs, vpS, scS, 4, 3, (const void*)0x9F00);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, (const void*)0x8A00, 0x11u,
                     1280, 720, 91, 0, true, 0, 952);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r->onBackbufferScan(0, 921600);
        CHECK(cap.has("s7:FEED0007"), "the sampler INDEX travels with the texture key");
        CHECK(cap.has("depthCtl=DEADD001") && cap.has("blendCtl=B1E2DC01") &&
              cap.has("colorMask=0000000F") && cap.has("stateValid=1"),
              "the effective render state reaches the log");
        CHECK(cap.has("sc=(1,2,3,4)"), "the scissor reaches the log");
        CHECK(cap.has("9F00"), "the POOL PSO identity reaches the log");
        CHECK(cap.has("srcFmt=91") && cap.has("8A00"),
              "the resolve source format and host resource reach the log");
        delete r;
    }

    // 15. CAPS ANNOUNCE THEMSELVES with truthful truncation status.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0;
        for (uint32_t i = 0; i < kTailLen + 3; i++)     // 15 producers, 12 kept
            r->onClear(SA, 0x11u, NAVY, 300 + i);
        for (uint32_t i = 0; i < kResolvesPerFrame + 5; i++)
            r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                         0, true, 0, 200 + i);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f = r->frameAt(0);
        CHECK(f && f->resolveCount == kResolvesPerFrame &&
              f->resolveTotal == kResolvesPerFrame + 5,
              "resolve cap keeps the truthful total");
        CHECK(f->matchValid && f->match.ordinal == 200 + kResolvesPerFrame + 4 &&
              f->matchDetailDropped,
              "the LAST recorded resolve wins even past the detail cap");
        CHECK(f->selSnap.total == kTailLen + 3, "tail cap keeps the truthful total");
        r->onBackbufferScan(0, 921600);
        CHECK(cap.has("older truncated=3"), "the dump announces tail truncation");
        CHECK(cap.has("resolves=16/21"), "the dump announces resolve truncation");
        delete r;
        Cap cap2; Ring* r2 = new Ring(Sink{&Cap::emit, &cap2}, true);
        armNow(r2);
        for (uint32_t i = 0; i < kSurfaces + 1; i++)
            r2->onClear((const void*)(uintptr_t)(0x9000 + i * 16), 0, NAVY, i);
        r2->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        r2->onBackbufferScan(0, 921600);
        CHECK(cap2.has("droppedSurfaces=1"), "the surface-table cap announces its drop");
        delete r2;
    }

    // 16. RE-FEED THROUGH THE CHOKEPOINT (r3 defect 1): the same batch fed twice
    //     (in-order at submit + the renderHleDraws fallback / held-list re-record)
    //     lands TWICE — each re-record is a real pool draw; nothing is deduped.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0;
        PoolDrawFeed f = makePoolDrawFeed(971, 0x7777, 0x88, constA, nullptr, tex,
                                          0, 0, 0xF, true);
        feedPoolDraw(*r, true, SA, 0u, f, VP, SC, 4, 6, (const void*)0x9F04);
        feedPoolDraw(*r, true, SA, 0u, f, VP, SC, 4, 6, (const void*)0x9F04);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 972);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* fr = r->frameAt(0);
        CHECK(fr && fr->matchValid && fr->selSnap.total == 2 &&
              tailHasOrd(fr->selSnap, 971),
              "both chokepoint callers' feeds land - a held re-record is not deduped");
        delete r;
    }

    // 17. DETAIL-ROW IDENTITY IS THE SNAPID (r3 defect 2): consecutive resolves
    //     share one draw ordinal; the dropped 17th row must still read DROPPED,
    //     and a retained matching row sharing an ordinal must not.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        const void* SA = (const void*)0xA0;
        for (uint32_t i = 0; i < kResolvesPerFrame; i++)
            r->onResolve(0x81000000u + i * 0x10000u, 0x01000000u + i * 0x10000u,
                         SA, nullptr, 0x11u, 640, 360, 28, 0, true, 0, 777);
        r->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                     0, true, 0, 777);   // detail-dropped; SAME ordinal as every row
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f = r->frameAt(0);
        CHECK(f && f->matchValid && f->match.ordinal == 777 && f->matchDetailDropped,
              "a dropped row sharing its ordinal with retained rows reads DROPPED");
        Cap cap2; Ring* r2 = new Ring(Sink{&Cap::emit, &cap2}, true);
        armNow(r2);
        r2->onResolve(0x8C000000u, 0x0C000000u, SA, nullptr, 0x33u, 640, 360, 28,
                      0, true, 0, 888);
        r2->onResolve(0x8A94E000u, 0x0A94E000u, SA, nullptr, 0x11u, 1280, 720, 28,
                      0, true, 0, 888);
        r2->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        const FrameRec* f2 = r2->frameAt(0);
        CHECK(f2 && f2->matchValid && !f2->matchDetailDropped,
              "a RETAINED matching row sharing an ordinal is not marked dropped");
        delete r2; delete r;
    }

    // 18. SCAN ELIGIBILITY OUTLIVES FILE CAPS (r3 defect 3): the trigger's scan
    //     is decided separately from file output, so "cap exhausted before arm,
    //     black after arm" still dumps.
    {
        CHECK(scanEligible(true, true, false),
              "file caps exhausted: the enabled ring keeps the scan alive");
        CHECK(!scanEligible(true, false, false),
              "ring off: exhausted caps end the scan exactly as before");
        CHECK(scanEligible(true, false, true),
              "legacy file sampling still scans with the ring off");
        CHECK(!scanEligible(false, true, true),
              "SR_PRESENT_DUMP off: no scan - the ARMED-BUT-BLIND case");
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        armNow(r);
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        if (scanEligible(true, true, /*fileWouldSample=*/false))
            r->onBackbufferScan(0, 921600);
        CHECK(r->dumped(), "black after arm still dumps with file caps exhausted");
        delete r;
    }

    // 19. SCAN-WINDOW LIFECYCLE (r4, ruling 008481a): the window opens at the
    //     CONSUMED arm and closes at the one-shot dump — never enabled()-for-
    //     lifetime. A pending arm is consumed at this boundary itself.
    {
        Cap cap; Ring* r = new Ring(Sink{&Cap::emit, &cap}, true);
        CHECK(!r->scanWanted(),
              "unarmed: no scan wanted - exhausted file caps stay readback-free");
        std::thread guest([&] { r->onAptRequest("screens/hud/main_hud"); });
        guest.join();
        CHECK(r->scanWanted() && r->armed(),
              "a pending arm is consumed AT the scan boundary and opens the window");
        r->onPresent(1, 0x0A94E000u, 1280, 720, 3, (const void*)0x77, 28, true, 0);
        if (scanEligible(true, r->scanWanted(), /*fileWouldSample=*/false))
            r->onBackbufferScan(0, 921600);
        CHECK(r->dumped(), "exact black inside the window dumps");
        CHECK(!r->scanWanted(),
              "after the one-shot dump the window is CLOSED - no further readbacks");
        delete r;
    }

    const int total = pass + fail;
    SR_LOG_BOOT(fail ? sr::LogSeverity::Error : sr::LogSeverity::Info,
                "[PROV_TEST] %d/%d passed", pass, total);
    return fail;
}

}  // namespace

int runSelfTestIfRequested() {
    const char* e = std::getenv("SR_PROV_RING_TEST");
    if (!e || e[0] != '1') return -1;
    return runTests();
}

}  // namespace sr::gfx::prov
