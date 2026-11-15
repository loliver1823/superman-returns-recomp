// iter-455n r2: the texture Lock/Unlock upload contract (SR_HLE_TEX_UNLOCK_TEST=1).
//
// r1 was rejected because the test proved only an arena-copy helper: it called
// arenaWrite itself and never drove latching, range validation, command
// construction, key normalization or refusal paths. THIS suite drives
// `runUnlock` — the same body kernel/hle_api_observe.cpp calls — with injected
// fakes for the three side-effecting steps, so every assertion below is about
// production control flow.
//
// The bug that motivates the identity fixtures: the atlas locks at ABA5A000
// while shaders bind the normalized key 0BA5A000. Publishing the raw pointer
// made the backend store fresh pixels under one name while s_texSlot and the
// swizzled-view lookup used the other — fresh resource, stale alpha view.
#include "tex_unlock_rule.hpp"
#include "../app/log.hpp"
#include "../gfx/hle_video.hpp"   // iter-456n: the Class-A pool under test
#include "../gfx/tex_ownership.hpp" // iter-456n: the ownership machine
#include "../gfx/tex_version_bank.hpp" // iter-456p: the observer-only banker
#include "../gfx/vb_publish.hpp"  // iter-456p Step 3A: the VB publication rules
#include "../gfx/draw_manifest.hpp" // iter-457 gate 3: the suppression manifest
#include "../gfx/shader_cache.hpp"
#include "../gfx/cube_layout.hpp"  // iter-457 H1a: the ONE cube layout body
#include "../gfx/guest_image.hpp"  // iter-457 H1(b)-r3: the extracted 2D decoder
#include "../gfx/cube_bind_rule.hpp"  // iter-457 H1b: resource/SRV/bind rules
#include "../gfx/cube_transaction.hpp"  // iter-457 H1(b)-r5: THE upload transaction
#include "cube_capture_rule.hpp"  // iter-457 H1a-r2: THE cube capture body
#include "guest_context.hpp"      // iter-457 H1a-r3: install guest memory
#include "phys_window.hpp"        // iter-458 C14: an owner for the census's texture
#include "tex_census.hpp"         // iter-458 C14: the census's producer tap
#include "../guest/ppc/ppc_recomp_shared.h"  // iter-457 H1a-r4: PPCContext
#include <windows.h>                          // sparse guest reservation
#include <d3d12.h>                            // H1b-15: WARP device fixture
#include <dxgi1_4.h>                          // H1b-15: EnumWarpAdapter
#include <wrl/client.h>
#include <d3dcompiler.h>                      // H1b-15 r6: the consumption probe's shaders
#pragma comment(lib, "d3dcompiler.lib")
// d3d12.h is MIDL-generated and drags in rpcndr.h, whose `small` macro
// (= char) would rewrite the R17-5 local of that name below.
#ifdef small
#undef small
#endif
#include <string>
#include <chrono>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <set>
#include <array>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>

// iter-457 H1(a)-r4: the guest SetTexture override lives at GLOBAL scope
// (PPC_FUNC expands to a plain free function), not in sr::kernel - which is
// why an sr::kernel declaration was both ambiguous and unresolvable.
void sub_82100310(PPCContext& __restrict ctx, uint8_t* base);

namespace sr { namespace kernel {
bool cubeCaptureEnabledForTest();
// iter-457 H1(a)-r3: the PRODUCTION dispatch, driven by C13 below.
uint32_t observeSetTextureFetchForTest(uint32_t dev, uint32_t slot);
uint64_t cubeDispatchesForTest();
uint64_t legacy2DEntriesForTest();
bool cubeGateWantsOnForTest(const char* e);
uint64_t cubeCapPublishedForTest();
const char* cubeCapLastRefuseForTest();
void resetCubeCapCountersForTest();
bool setSetTextureDelegateBypassForTest(bool on);
bool setTextureDelegateBypassStateForTest();
} }

namespace sr::kernel::texunlock {
namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) { g_pass++; return; }
    g_fail++;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[TEXUNLOCK_TEST] FAIL: %s", what);
}

// iter-457 H1a: the same accounting, but the failure message may carry the
// VALUES that disagreed. A refusal test that cannot print what it actually got
// makes the next failure a guessing game.
template <typename... A>
void checkf(bool ok, const char* fmt, A... args) {
    if (ok) { g_pass++; return; }
    g_fail++;
    char buf[512];
    std::snprintf(buf, sizeof(buf), fmt, args...);
    SR_LOG_BOOT(sr::LogSeverity::Error, "[TEXUNLOCK_TEST] FAIL: %s", buf);
}

constexpr uint32_t kArenaFull = 0xFFFFFFFFu;
constexpr uint32_t kSlotBytes = 1u << 20;

// A fake guest with TWO windows over one physical page set, so the fixtures can
// prove the upload sources the EXACT locked window and not the other alias.
struct FakeGuest {
    std::vector<uint8_t> win8, winA;
    uint32_t phys = 0x0BA5A000u, span = 0;
    int reads = 0, derefRefusals = 0;
    bool poison = false;              // "uncommitted": reads must refuse
    explicit FakeGuest(uint32_t bytes) : win8(bytes, 0x55), winA(bytes, 0x11), span(bytes) {}
    const uint8_t* read(uint32_t va, uint32_t bytes) {
        reads++;
        if (poison) { derefRefusals++; return nullptr; }
        if (!bytes || (uint64_t)va + bytes > 0x100000000ull) { derefRefusals++; return nullptr; }
        if (bytes > span) { derefRefusals++; return nullptr; }   // overflow the object
        const uint32_t w = va & 0xF0000000u, p = va & kKeyMask;
        if (p != phys) { derefRefusals++; return nullptr; }
        if (w == 0x80000000u) return win8.data();
        if (w == 0xA0000000u) return winA.data();
        derefRefusals++;
        return nullptr;
    }
};

// A fake arena: every write is a distinct slot holding its own copy, so
// "two uploads with the second contents visible" is directly observable.
struct FakeArena {
    std::vector<std::vector<uint8_t>> slots;
    bool full = false;
    uint32_t write(const uint8_t* p, uint32_t bytes) {
        if (full) return kArenaFull;
        slots.emplace_back(p, p + bytes);
        return (uint32_t)slots.size() - 1u;
    }
};

struct Published { uint32_t key, srcVA, bytes, pitch, w, h, level, ofs; };

// One Unlock through the PRODUCTION body.
UnlockResult drive(const TexInfo& t, LockLatch& latch, FakeGuest& g, FakeArena& a,
                   std::vector<Published>& out, uint32_t maxBytes = kSlotBytes) {
    return runUnlock(t, latch, maxBytes, kArenaFull,
        [&](uint32_t va, uint32_t bytes) { return g.read(va, bytes); },
        [&](const uint8_t* p, uint32_t bytes) { return a.write(p, bytes); },
        [&](const UnlockResult& r, uint32_t ofs) {
            out.push_back({r.key, r.srcVA, r.bytes, r.pitch, r.width, r.height, r.level, ofs});
        });
}

int runTests() {
    // iter-457 H1(a)-r3: armed HERE, before any code path can latch the gate's
    // function-local static. C13 drives the production cube dispatch, which is
    // gated on it; C9 asserts the DEFAULT via the pure parse instead.
    _putenv_s("SR_HLE_CUBE_CAPTURE", "1");
    // iter-457 H1(b)-P1: the EXECUTOR upload gate too, for the same reason -
    // P1 drives the production cube success arm, which latches this static
    // on its first evaluation (C6's refusal fixtures reach that point first).
    // With no backend and no stand-in the arm still refuses, so every earlier
    // fixture keeps its counters.
    _putenv_s("SR_HLE_CUBE_UPLOAD", "1");
    const TexInfo k8{0x28000002u, 64, 8};        // the atlas class, 64x8
    const uint32_t kPitch = 64, kBytes = 64 * 8;
    const uint32_t kLockVA = 0xABA5A000u;        // EXACT Lock-returned pointer
    const uint32_t kIdent  = 0x0BA5A000u;        // what every consumer binds

    // ── 1. Format eligibility (the predicate, unchanged from r1) ──────────
    check(unlockSnapshotWanted(kMoviePlaneFmtDword), "movie-plane dword is eligible");
    check(unlockSnapshotWanted(0x18000002u), "k_8 with other high create bits is eligible");
    check(!unlockSnapshotWanted(0x28000052u), "a non-k_8 format is not eligible");

    // ── 2. IDENTITY: 0xA source bytes, 0x8 holds something else, and the
    //      published key is the NORMALIZED 0x0... value (blocker 1) ────────
    {
        FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
        std::memset(g.winA.data(), 0x22, kBytes);    // the REAL locked window
        std::memset(g.win8.data(), 0x99, kBytes);    // the decoy alias
        LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
        const UnlockResult r = drive(k8, l, g, a, pub);
        check(r.published && pub.size() == 1, "an eligible Unlock publishes once");
        check(r.key == kIdent && pub[0].key == kIdent,
              "the published KEY is the normalized identity 0BA5A000, not the raw "
              "lock pointer (the bug that split backend storage from the swizzle view)");
        check(r.srcVA == kLockVA,
              "the SOURCE stays the exact Lock-returned pointer ABA5A000");
        check(a.slots.size() == 1 && a.slots[0][0] == 0x22 && a.slots[0][kBytes - 1] == 0x22,
              "the snapshot carries the 0xA window's bytes, NOT the 0x8 alias");
        check(r.bytes == kBytes && r.pitch == kPitch && r.width == 64 && r.height == 8,
              "the command fields are the locked surface's own dimensions");
    }

    // ── 3. SAME IDENTITY, TWO Lock/Unlock cycles, CHANGED bytes ───────────
    {
        FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
        std::memset(g.winA.data(), 0x00, kBytes);
        for (uint32_t i = 0; i < 12; i++) g.winA[i] = 0x11;      // sparse atlas
        LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
        drive(k8, l, g, a, pub);
        std::memset(g.winA.data(), 0x22, kBytes);                // complete atlas
        l.begin(); l.succeed(kPitch, kLockVA, 0);                // the SECOND Lock
        drive(k8, l, g, a, pub);
        check(pub.size() == 2, "two Lock/Unlock cycles publish TWO commands (no dedupe)");
        check(pub[0].key == pub[1].key, "both name the SAME texture identity");
        check(a.slots.size() == 2 && a.slots[0][kBytes - 1] == 0x00 &&
                  a.slots[1][kBytes - 1] == 0x22,
              "a texel EMPTY in upload #1 is PAINTED in upload #2 (the freshness "
              "the permanent tuple dedupe destroyed)");
    }

    // ── 4. A FAILED second Lock cannot reuse the first pointer (blocker 3) ─
    {
        FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
        LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
        drive(k8, l, g, a, pub);
        l.begin();                       // the failed Lock: clears, never succeeds
        const UnlockResult r = drive(k8, l, g, a, pub);
        check(!r.published && r.refuse == Refuse::NoLatch,
              "Unlock after a FAILED Lock refuses instead of re-uploading the "
              "previous Lock's pointer");
        check(pub.size() == 1 && a.slots.size() == 1, "and publishes no second snapshot");
    }

    // ── 5. Duplicate Unlock with no new Lock refuses ──────────────────────
    {
        FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
        LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
        drive(k8, l, g, a, pub);
        const UnlockResult r = drive(k8, l, g, a, pub);
        check(!r.published && r.refuse == Refuse::NoLatch,
              "a second Unlock without a new Lock refuses (the latch is consumed)");
    }

    // ── 6. Unreadable / overflowing ranges refuse WITHOUT dereferencing ───
    {
        FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
        g.poison = true;                                  // uncommitted pages
        LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
        const UnlockResult r = drive(k8, l, g, a, pub);
        check(!r.published && r.refuse == Refuse::Unreadable,
              "an unreadable range refuses (checked translation, no dereference)");
        check(a.slots.empty(), "and nothing is copied");

        FakeGuest g2(kBytes); FakeArena a2; std::vector<Published> pub2;
        LockLatch l2; l2.begin(); l2.succeed(kPitch, 0xFFFFFF00u, 0);   // wraps 32 bits
        const UnlockResult r2 = drive(k8, l2, g2, a2, pub2);
        check(!r2.published && r2.refuse == Refuse::SpanOverflow,
              "a span that overflows 32 bits refuses BEFORE any read");
        check(g2.reads == 0, "and the reader was never even called");
    }

    // ── 7. Nonzero mip is refused, not mis-sized with base height (blocker 4)
    {
        FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
        LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 2);   // level 2
        const UnlockResult r = drive(k8, l, g, a, pub);
        check(!r.published && r.refuse == Refuse::NonZeroMip,
              "a nonzero mip level is explicitly REFUSED rather than sized with "
              "the base level's height");
    }

    // ── 8. Arena exhaustion and oversize refuse, and are named ────────────
    {
        FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
        a.full = true;
        LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
        const UnlockResult r = drive(k8, l, g, a, pub);
        check(!r.published && r.refuse == Refuse::ArenaFull, "a full arena refuses, named");

        FakeGuest g2(kBytes); FakeArena a2; std::vector<Published> pub2;
        LockLatch l2; l2.begin(); l2.succeed(kPitch, kLockVA, 0);
        const UnlockResult r2 = drive(k8, l2, g2, a2, pub2, 16u);   // tiny slot
        check(!r2.published && r2.refuse == Refuse::TooBig, "an oversize span refuses, named");
    }

    // ── 9. An ineligible format still CLEARS the latch (it must not be
    //      reusable by a later eligible Unlock) ─────────────────────────────
    {
        FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
        const TexInfo bc{0x28000052u, 64, 8};
        LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
        const UnlockResult r = drive(bc, l, g, a, pub);
        check(!r.published && r.refuse == Refuse::NotEligibleFormat,
              "a non-k_8 format stays on the bind-time path");
        const UnlockResult r2 = drive(k8, l, g, a, pub);
        check(!r2.published && r2.refuse == Refuse::NoLatch,
              "and its latch was consumed, so it cannot be reused later");
    }

    // ══ iter-456n CLASS A (SR_HLE_UNLOCK_UPLOAD_ALL) — the R2 bar ═══════════
    // Every case below drives the SAME production body with a WidePlan, so the
    // widened path is proven by control flow, not by a parallel model.
    {
        namespace TL = sr::kernel::texlayout;

        // Build a plan the way production does: from a fetch constant.
        auto planFor = [](uint32_t fmt, uint32_t w, uint32_t h,
                          uint32_t pitchTexels, bool tiled, uint32_t endian,
                          uint32_t level = 0) {
            WidePlan p{};
            uint32_t f[6] = {};
            f[0] = 2u | ((pitchTexels >> 5) << 22) | (tiled ? (1u << 31) : 0u);
            f[1] = (fmt & 0x3Fu) | ((endian & 3u) << 6) | (0x0BA5Au << 12);
            f[2] = (w - 1u) | ((h - 1u) << 13);
            // iter-456p: these fixtures synthesize 2D textures, so they must SAY
            // 2D. dword_5 bits 9-10 carry the dimension, and leaving them zero
            // declares 1D - a shape that does NOT occur in real data: across all
            // 38,380 texture fetches in the golden title frame, ZERO have
            // dimension 0 (29,127 are 2D, 7,431 3D, 1,822 cube). The fixtures
            // were under-specified, not describing a real 1D texture.
            f[5] = ((uint32_t)TL::TexDim::k2DOrStacked) << 9;
            p.fd = TL::decodeFetchConstant(f);
            p.haveFetch = p.fd.valid;
            p.layout = TL::computeLayout(p.fd.fmt, p.fd.width, p.fd.height,
                                         p.fd.pitchTexels, p.fd.tiled, level);
            return p;
        };

        // ── A1. THE FETCH CONSTANT ROUND-TRIPS (pitch is TEXELS, not bytes) ──
        {
            const WidePlan p = planFor(18, 256, 128, 256, true, 1);
            check(p.fd.valid && p.fd.fmt == 18 && p.fd.width == 256 &&
                      p.fd.height == 128 && p.fd.pitchTexels == 256 &&
                      p.fd.tiled && p.fd.endian == 1,
                  "A1: fetch constant decodes fmt/dims/pitchTexels/tiled/endian");
        }

        // ── A2. LAYOUT AGREES WITH THE CAPTURE SITE'S HISTORICAL SPAN ───────
        // spanPadded must equal the exact expression bind capture has always
        // used, or adopting the helper there would change default behaviour.
        {
            struct C { uint32_t fmt, w, h, pitch; bool tiled; };
            const C cs[] = {
                {2, 128, 128, 256, true}, {6, 64, 64, 64, true},
                {18, 256, 256, 256, true}, {20, 128, 128, 128, true},
                {49, 1024, 512, 1024, true}, {24, 64, 64, 64, false},
                {29, 1280, 720, 1280, false}, {31, 640, 360, 640, false},
                {36, 16, 16, 16, false}, {18, 1024, 256, 1024, true},
            };
            bool all = true;
            for (const C& c : cs) {
                const TL::Layout L = TL::computeLayout(c.fmt, c.w, c.h, c.pitch,
                                                       c.tiled, 0);
                const TL::BlockGeom g = TL::blockGeomFor(c.fmt);
                const uint32_t rowBlocks = (c.w + g.blockW - 1u) / g.blockW;
                const uint32_t rows = (c.h + g.blockW - 1u) / g.blockW;
                uint32_t bp = c.pitch / g.blockW;
                if (c.tiled) bp = (bp + 31u) & ~31u;
                if (!bp) bp = rowBlocks;
                const uint32_t spanRows = c.tiled ? ((rows + 31u) & ~31u) : rows;
                const uint64_t want = (uint64_t)spanRows * bp * g.blockBytes;
                if (!L.valid || L.spanPadded != want) { all = false; break; }
                // maxTouched must never exceed the padded span, and must cover
                // at least one full row of blocks.
                if (L.maxTouched > L.spanPadded ||
                    L.maxTouched < (uint64_t)rowBlocks * g.blockBytes) {
                    all = false; break;
                }
            }
            check(all, "A2: layout spanPadded reproduces the capture site's math "
                       "for every admitted format, and maxTouched is bounded by it");
        }

        // ── A3. maxTouched IS EXACT — brute force over the decoder's grid ────
        {
            const TL::Layout L = TL::computeLayout(18, 256, 128, 256, true, 0);
            uint32_t bpb = 0; while ((1u << bpb) < L.blockBytes) bpb++;
            int64_t hi = -1;
            for (uint32_t by = 0; by < L.rows; by++)
                for (uint32_t bx = 0; bx < L.rowBlocks; bx++) {
                    const int32_t o = sr::gfx::xenos_tiling::tiled2D(
                        int32_t(bx), int32_t(by), L.blockPitch, bpb);
                    if (o > hi) hi = o;
                }
            check(L.valid && L.maxTouched == (uint64_t)hi + L.blockBytes,
                  "A3: maxTouched equals the brute-forced maximum of the "
                  "decoder's own tiled2D addressing (+blockBytes)");
            check(L.maxTouched <= L.spanPadded,
                  "A3: and it never exceeds the padded span (no neighbour tail "
                  "claimed beyond what the decode addresses)");
        }

        // ── A4. NEIGHBOUR-TAIL SAFETY: the snapshot is sized at maxTouched ───
        {
            const WidePlan p = planFor(18, 256, 128, 256, true, 1);
            FakeGuest g((uint32_t)p.layout.maxTouched); FakeArena a;
            std::vector<Published> pub;
            std::memset(g.winA.data(), 0x5A, g.span);
            const TexInfo t{0x18000012u, 256, 128};
            LockLatch l; l.begin(); l.succeed(256, kLockVA, 0);
            const UnlockResult r = runUnlock(t, l, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a.write(q, b); },
                [&](const UnlockResult& rr, uint32_t ofs) {
                    pub.push_back({rr.key, rr.srcVA, rr.bytes, rr.pitch,
                                   rr.width, rr.height, rr.level, ofs});
                }, &p);
            check(r.published && r.widened && r.bytes == p.layout.maxTouched,
                  "A4: a DXT1 Unlock publishes, sized at maxTouched — the "
                  "format the legacy predicate refused outright");
            check(r.pitch == 256 && r.width == 256 && r.height == 128 &&
                      r.fmtCode == 18 && r.tiled && r.endian == 1,
                  "A4: and carries the header's texel pitch, dims, fmt, "
                  "tiled and endian for the backend decode");
        }

        // ── A5. GATE-OFF BYTE IDENTITY for the k_8 path (the hard bar) ───────
        {
            FakeGuest g1(kBytes), g2(kBytes); FakeArena a1, a2;
            std::vector<Published> p1, p2;
            std::memset(g1.winA.data(), 0x33, kBytes);
            std::memset(g2.winA.data(), 0x33, kBytes);
            LockLatch l1; l1.begin(); l1.succeed(kPitch, kLockVA, 0);
            const UnlockResult r1 = drive(k8, l1, g1, a1, p1);   // wide = nullptr
            const WidePlan p = planFor(2, 64, 8, 64, false, 0);
            LockLatch l2; l2.begin(); l2.succeed(kPitch, kLockVA, 0);
            const UnlockResult r2 = runUnlock(k8, l2, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g2.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a2.write(q, b); },
                [&](const UnlockResult& rr, uint32_t ofs) {
                    p2.push_back({rr.key, rr.srcVA, rr.bytes, rr.pitch,
                                  rr.width, rr.height, rr.level, ofs});
                }, &p);
            check(r1.published && r2.published &&
                      r1.key == r2.key && r1.srcVA == r2.srcVA &&
                      r1.bytes == r2.bytes && r1.pitch == r2.pitch &&
                      r1.width == r2.width && r1.height == r2.height,
                  "A5: for k_8 the widened path publishes the IDENTICAL command "
                  "fields as the legacy path (widening cannot move the atlas)");
            check(a1.slots.size() == 1 && a2.slots.size() == 1 &&
                      a1.slots[0] == a2.slots[0],
                  "A5: and the identical snapshot bytes");
        }

        // ── A6. UNSUPPORTED FORMAT AND NONZERO MIP ARE REFUSED, NAMED ───────
        {
            FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
            const WidePlan bad = planFor(22, 64, 8, 64, false, 0);  // depth-as-tex
            const TexInfo t{0x28000016u, 64, 8};
            LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
            const UnlockResult r = runUnlock(t, l, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a.write(q, b); },
                [&](const UnlockResult&, uint32_t) {}, &bad);
            check(!r.published && r.refuse == Refuse::UnsupportedFormat,
                  "A6: a format the BACKEND cannot decode is refused by its own "
                  "named reason, not silently dropped");
            const WidePlan mip = planFor(18, 256, 128, 256, true, 1, /*level=*/1);
            check(!mip.layout.valid &&
                      mip.layout.refuse == TL::LayoutRefuse::NonZeroMip,
                  "A6: the layout helper refuses a nonzero mip rather than "
                  "guessing its dimensions");
        }

        // ── A7. ARENA EXHAUSTION AND UNREADABLE SPANS STILL REFUSE ──────────
        {
            const WidePlan p = planFor(18, 256, 128, 256, true, 1);
            FakeGuest g((uint32_t)p.layout.maxTouched); FakeArena a;
            std::vector<Published> pub;
            a.full = true;
            const TexInfo t{0x18000012u, 256, 128};
            LockLatch l; l.begin(); l.succeed(256, kLockVA, 0);
            const UnlockResult r = runUnlock(t, l, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a.write(q, b); },
                [&](const UnlockResult&, uint32_t) {}, &p);
            check(!r.published && r.refuse == Refuse::ArenaFull,
                  "A7: arena exhaustion refuses by name on the widened path");
            FakeGuest g2((uint32_t)p.layout.maxTouched); g2.poison = true;
            FakeArena a2;
            LockLatch l2; l2.begin(); l2.succeed(256, kLockVA, 0);
            const UnlockResult r2 = runUnlock(t, l2, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g2.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a2.write(q, b); },
                [&](const UnlockResult&, uint32_t) {}, &p);
            check(!r2.published && r2.refuse == Refuse::Unreadable,
                  "A7: an unreadable/uncommitted span refuses before any access");
        }

        // ── A8. NO PERMANENT DEDUPE ON THE WIDENED PATH ─────────────────────
        {
            const WidePlan p = planFor(18, 256, 128, 256, true, 1);
            FakeGuest g((uint32_t)p.layout.maxTouched); FakeArena a;
            std::vector<Published> pub;
            const TexInfo t{0x18000012u, 256, 128};
            auto once = [&](uint8_t fill) {
                std::memset(g.winA.data(), fill, g.span);
                LockLatch l; l.begin(); l.succeed(256, kLockVA, 0);
                runUnlock(t, l, kSlotBytes, kArenaFull,
                    [&](uint32_t va, uint32_t b) { return g.read(va, b); },
                    [&](const uint8_t* q, uint32_t b) { return a.write(q, b); },
                    [&](const UnlockResult& rr, uint32_t ofs) {
                        pub.push_back({rr.key, rr.srcVA, rr.bytes, rr.pitch,
                                       rr.width, rr.height, rr.level, ofs});
                    }, &p);
            };
            once(0x01); once(0x02); once(0x03);
            check(pub.size() == 3 && a.slots.size() == 3 &&
                      a.slots[0][0] == 0x01 && a.slots[1][0] == 0x02 &&
                      a.slots[2][0] == 0x03,
                  "A8: three Unlocks of the same key publish THREE times with "
                  "the current bytes each time (the reference's rule)");
        }

        // ── A9. A DUPLICATE UNLOCK WITHOUT A NEW LOCK STILL REFUSES ─────────
        {
            const WidePlan p = planFor(18, 256, 128, 256, true, 1);
            FakeGuest g((uint32_t)p.layout.maxTouched); FakeArena a;
            const TexInfo t{0x18000012u, 256, 128};
            LockLatch l; l.begin(); l.succeed(256, kLockVA, 0);
            runUnlock(t, l, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a.write(q, b); },
                [&](const UnlockResult&, uint32_t) {}, &p);
            const UnlockResult r2 = runUnlock(t, l, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a.write(q, b); },
                [&](const UnlockResult&, uint32_t) {}, &p);
            check(!r2.published && r2.refuse == Refuse::NoLatch,
                  "A9: one Unlock per Lock holds on the widened path too");
        }

        // ── A10. WIDENING NEVER LOSES A LEGACY PUBLISH ──────────────────────
        // A k_8 texture whose header carries no usable fetch constant must
        // still publish through the legacy sizing rather than refuse.
        {
            FakeGuest g(kBytes); FakeArena a; std::vector<Published> pub;
            WidePlan none{};                        // haveFetch = false
            LockLatch l; l.begin(); l.succeed(kPitch, kLockVA, 0);
            const UnlockResult r = runUnlock(k8, l, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a.write(q, b); },
                [&](const UnlockResult& rr, uint32_t ofs) {
                    pub.push_back({rr.key, rr.srcVA, rr.bytes, rr.pitch,
                                   rr.width, rr.height, rr.level, ofs});
                }, &none);
            check(r.published && !r.widened && r.bytes == kBytes,
                  "A10: a fetch-less k_8 header still publishes on the legacy "
                  "sizing — widening can only ADD publishes, never remove one");
        }
    }

    // == iter-456n REWORK - the ORDERED FULL R2 SUITE (fifth adjudication) ==
    // Failed bar 2: the previous matrix exercised ONE widened DXT1 publish and
    // a geometry subset; codes 19 and 50 were absent, endian modes and the
    // widened 0x8/0xA identity were uncovered. This block closes that.
    {
        namespace TL = sr::kernel::texlayout;

        auto planFor2 = [](uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch,
                           bool tiled, uint32_t endian, uint32_t level = 0,
                           uint64_t budget = 0) {
            WidePlan p{};
            uint32_t f[6] = {};
            f[0] = 2u | ((pitch >> 5) << 22) | (tiled ? (1u << 31) : 0u);
            f[1] = (fmt & 0x3Fu) | ((endian & 3u) << 6) | (0x0BA5Au << 12);
            f[2] = (w - 1u) | ((h - 1u) << 13);
            // iter-456p: these fixtures synthesize 2D textures, so they must SAY
            // 2D. dword_5 bits 9-10 carry the dimension, and leaving them zero
            // declares 1D - a shape that does NOT occur in real data: across all
            // 38,380 texture fetches in the golden title frame, ZERO have
            // dimension 0 (29,127 are 2D, 7,431 3D, 1,822 cube). The fixtures
            // were under-specified, not describing a real 1D texture.
            f[5] = ((uint32_t)TL::TexDim::k2DOrStacked) << 9;
            p.fd = TL::decodeFetchConstant(f);
            p.haveFetch = p.fd.valid;
            p.layout = TL::computeLayout(p.fd.fmt, p.fd.width, p.fd.height,
                                         p.fd.pitchTexels, p.fd.tiled, level, budget);
            return p;
        };

        // -- R2-1. EVERY ADMITTED FORMAT PUBLISHES, tiled AND linear --------
        // The admitted set is exactly what the backend decode switch handles
        // (gfx/d3d12_backend.cpp hleUploadHostTextureFmt): 2, 6, 50, 18, 19,
        // 20, 49, 24, 29, 31, 36. Codes 19 and 50 were MISSING before.
        {
            struct F { uint32_t code, blockW, blockBytes; };
            const F fmts[] = {{2,1,1}, {6,1,4}, {50,1,4}, {18,4,8}, {19,4,16},
                              {20,4,16}, {49,4,16}, {24,1,2}, {29,1,8},
                              {31,1,4}, {36,1,4}};
            bool allGeom = true, allPub = true;
            for (const F& f : fmts) {
                const TL::BlockGeom g = TL::blockGeomFor(f.code);
                if (!g.supported || g.blockW != f.blockW ||
                    g.blockBytes != f.blockBytes) { allGeom = false; break; }
                for (int t = 0; t < 2; t++) {
                    const bool tiled = (t == 1);
                    const WidePlan p = planFor2(f.code, 64, 64, 64, tiled, 1);
                    if (!p.layout.valid) { allPub = false; break; }
                    FakeGuest g2((uint32_t)p.layout.maxTouched); FakeArena a2;
                    std::vector<Published> pub2;
                    std::memset(g2.winA.data(), 0x7E, g2.span);
                    const TexInfo t2{(f.code == 2u) ? 0x28000002u : (0x18000000u | f.code),
                                     64, 64};
                    LockLatch l2; l2.begin(); l2.succeed(64, kLockVA, 0);
                    const UnlockResult r2 = runUnlock(t2, l2, kSlotBytes, kArenaFull,
                        [&](uint32_t va, uint32_t b) { return g2.read(va, b); },
                        [&](const uint8_t* q, uint32_t b) { return a2.write(q, b); },
                        [&](const UnlockResult& rr, uint32_t ofs) {
                            pub2.push_back({rr.key, rr.srcVA, rr.bytes, rr.pitch,
                                            rr.width, rr.height, rr.level, ofs});
                        }, &p);
                    // k_8 is legacy-eligible so it publishes on the legacy
                    // sizing; every other admitted code publishes widened.
                    if (!r2.published) { allPub = false; break; }
                    if (f.code != 2u && (!r2.widened || r2.fmtCode != f.code)) {
                        allPub = false; break;
                    }
                }
                if (!allPub) break;
            }
            check(allGeom, "R2-1: block geometry matches the backend decode "
                           "switch for ALL 11 admitted formats (incl. 19 and 50)");
            check(allPub, "R2-1: every admitted format publishes, tiled AND linear");
        }

        // -- R2-2. ALL FOUR ENDIAN MODES ROUND-TRIP THROUGH THE PLAN --------
        {
            bool ok = true;
            for (uint32_t e = 0; e < 4; e++) {
                const WidePlan p = planFor2(18, 128, 128, 128, true, e);
                if (!p.fd.valid || p.fd.endian != e) { ok = false; break; }
                FakeGuest g3((uint32_t)p.layout.maxTouched); FakeArena a3;
                const TexInfo t3{0x18000012u, 128, 128};
                LockLatch l3; l3.begin(); l3.succeed(128, kLockVA, 0);
                const UnlockResult r3 = runUnlock(t3, l3, kSlotBytes, kArenaFull,
                    [&](uint32_t va, uint32_t b) { return g3.read(va, b); },
                    [&](const uint8_t* q, uint32_t b) { return a3.write(q, b); },
                    [&](const UnlockResult&, uint32_t) {}, &p);
                if (!r3.published || r3.endian != e) { ok = false; break; }
            }
            check(ok, "R2-2: endian modes 0-3 decode and are carried to the "
                      "published command unchanged");
        }

        // -- R2-3. WIDENED 0x8 / 0xA ALIAS IDENTITY -------------------------
        // The widened path must normalize identity exactly like the legacy one
        // and must source from the window the LOCK returned, not the alias.
        {
            const WidePlan p = planFor2(6, 64, 64, 64, false, 0);
            FakeGuest g4((uint32_t)p.layout.maxTouched); FakeArena a4;
            std::vector<Published> pub4;
            std::memset(g4.winA.data(), 0x5C, g4.span);   // the LOCKED window
            std::memset(g4.win8.data(), 0x1D, g4.span);   // the decoy alias
            const TexInfo t4{0x18000006u, 64, 64};
            LockLatch l4; l4.begin(); l4.succeed(256, kLockVA, 0);   // 0xA pointer
            const UnlockResult r4 = runUnlock(t4, l4, kSlotBytes, kArenaFull,
                [&](uint32_t va, uint32_t b) { return g4.read(va, b); },
                [&](const uint8_t* q, uint32_t b) { return a4.write(q, b); },
                [&](const UnlockResult& rr, uint32_t ofs) {
                    pub4.push_back({rr.key, rr.srcVA, rr.bytes, rr.pitch,
                                    rr.width, rr.height, rr.level, ofs});
                }, &p);
            check(r4.published && r4.key == kIdent && r4.srcVA == kLockVA,
                  "R2-3: widened publish normalizes the key to 0BA5A000 while "
                  "keeping the exact 0xA Lock pointer as the source");
            check(a4.slots.size() == 1 && a4.slots[0][0] == 0x5C,
                  "R2-3: and snapshots the LOCKED window, not the 0x8 alias");
        }

        // -- R2-4. CAPACITY / PRE-WALK BUDGET GUARD -------------------------
        {
            const TL::Layout big = TL::computeLayout(6, 4096, 4096, 4096, true,
                                                     0, /*budget=*/1u << 20);
            check(!big.valid && big.refuse == TL::LayoutRefuse::OverBudget,
                  "R2-4: an over-budget layout refuses BEFORE the exact walk");
            const TL::Layout okL = TL::computeLayout(18, 256, 256, 256, true, 0,
                                                     /*budget=*/16u << 20);
            check(okL.valid && okL.maxTouched <= okL.spanPadded,
                  "R2-4: an in-budget layout still computes the exact footprint");
        }

        // -- R2-5. EPOCH SIGNATURE DISCRIMINATION ---------------------------
        // Same address, DIFFERENT descriptor => a different signature, which is
        // what makes ownership self-clear on recreation / physical-key reuse.
        {
            const WidePlan a1 = planFor2(18, 128, 128, 128, true, 1);
            const WidePlan a2 = planFor2(18, 128, 128, 128, true, 1);
            const WidePlan b1 = planFor2(20, 128, 128, 128, true, 1);   // fmt
            const WidePlan c1 = planFor2(18, 64, 128, 128, true, 1);    // dims
            const WidePlan d1 = planFor2(18, 128, 128, 256, true, 1);   // pitch
            auto sig = [](const WidePlan& p) {
                return ((uint64_t)p.fd.fmt << 48) ^ ((uint64_t)p.fd.width << 32) ^
                       ((uint64_t)p.fd.height << 16) ^ (uint64_t)p.fd.pitchTexels;
            };
            check(sig(a1) == sig(a2) && sig(a1) != sig(b1) &&
                      sig(a1) != sig(c1) && sig(a1) != sig(d1),
                  "R2-5: identical descriptors share an epoch; fmt/dims/pitch "
                  "changes each start a NEW one (recreation and key reuse)");
        }

        // -- R2-6. NEIGHBOUR-TAIL: maxTouched < spanPadded where it matters --
        {
            const TL::Layout L = TL::computeLayout(6, 16, 16, 32, true, 0);
            check(L.valid && L.spanPadded == 4096 && L.maxTouched == 2048,
                  "R2-6: the exact footprint is HALF the padded span for a "
                  "16x16 tiled surface (no neighbour tail claimed)");
        }
    }

    // -- R2-7. CLASS-A POOL: STRESS, ZERO DROPS, ONCE-ONLY RELEASE ---------
    // Fifth adjudication, bar 4: "prove exact once-only release, and require
    // zero Class-A capacity drops on a stress burst larger than the observed
    // one". v1 dropped 14 publications from the shared 8-slot pool; this burst
    // is 512 concurrent live snapshots of varying size.
    {
        const sr::gfx::hle::ClassAStats before = sr::gfx::hle::classAStats();
        std::vector<uint8_t> src(64 * 1024, 0xA7);
        std::vector<std::pair<uint32_t, uint32_t>> held;   // (handle, bytes)
        bool anyReject = false, contentOk = true;
        for (int i = 0; i < 512; i++) {
            const uint32_t n = 1024u + (uint32_t)(i % 48) * 1024u;   // 1K..48K
            const uint32_t h = sr::gfx::hle::classAWrite(src.data(), n);
            if (h == sr::gfx::hle::kArenaFull) { anyReject = true; break; }
            const uint8_t* rd = sr::gfx::hle::classARead(h);
            if (!rd || rd[0] != 0xA7 || rd[n - 1] != 0xA7) contentOk = false;
            held.emplace_back(h, n);
        }
        check(!anyReject,
              "R2-7: a 512-snapshot burst (36x the v1 drop count) takes ZERO "
              "capacity rejects in the isolated Class-A pool");
        check(contentOk,
              "R2-7: every live handle reads back its own bytes while all 512 "
              "are concurrently held");
        const sr::gfx::hle::ClassAStats mid = sr::gfx::hle::classAStats();
        check(mid.inFlight >= 512u && mid.liveBytes > 0,
              "R2-7: the pool accounts all 512 as in-flight with live bytes");
        for (const auto& hb : held) sr::gfx::hle::classARelease(hb.first, hb.second);
        const sr::gfx::hle::ClassAStats after = sr::gfx::hle::classAStats();
        check(after.inFlight == before.inFlight && after.liveBytes == before.liveBytes,
              "R2-7: after releasing every handle, in-flight and live bytes "
              "return to their starting values (no leak)");
        // ONCE-ONLY: a second release of the same handles must be COUNTED as a
        // double free, never silently tolerated and never corrupting the pool.
        for (const auto& hb : held) sr::gfx::hle::classARelease(hb.first, hb.second);
        const sr::gfx::hle::ClassAStats dbl = sr::gfx::hle::classAStats();
        check(dbl.doubleFree == after.doubleFree + held.size() &&
                  dbl.inFlight == after.inFlight,
              "R2-7: a repeated release is counted as doubleFree and does NOT "
              "decrement in-flight again (exact once-only release)");
        check(sr::gfx::hle::classARead(sr::gfx::hle::kArenaFull) == nullptr &&
                  sr::gfx::hle::classARead(0u) == nullptr,
              "R2-7: a non-Class-A handle never reads Class-A memory (the two "
              "pools cannot be crossed)");
        // Over-size and budget refusals are named, not silent.
        const uint32_t huge = sr::gfx::hle::classAWrite(
            src.data(), sr::gfx::hle::kClassAMaxBytes + 1u);
        check(huge == sr::gfx::hle::kArenaFull,
              "R2-7: an over-max snapshot is refused by the pool, not truncated");
    }

    // == iter-456n R3 - THE OWNERSHIP STATE MACHINE, DRIVEN AS PRODUCTION ==
    // Sixth adjudication, blocking defect: the old predicate was REVERSED
    // against its own contract and R2-5 could not see it, because R2-5 only
    // recomputed a local signature and compared it with itself. Every check
    // below calls sr::gfx::hle::unlockOwnership() - the SAME instance the
    // upload arm calls - so a test cannot pass against a machine production
    // does not use. MUTATION-VALIDATED: swapping captureVerdict's
    // RefuseSameEpoch and AdmitRecreated arms (i.e. restoring the shipped
    // inversion) fails R3-1 and R3-2.
    {
        using sr::gfx::hle::UnlockOwnership;
        using sr::gfx::hle::CaptureVerdict;
        auto& own = sr::gfx::hle::unlockOwnership();
        own.reset();

        const uint32_t kKey  = 0x0BC22000u;             // the measured clobber
        const uint32_t kA8   = 0x8BC22000u;             // 0x8 alias, same pixels
        const uint32_t kAA   = 0xABC22000u;             // 0xA alias, same pixels
        const uint32_t dims  = (64u << 16) | 64u;
        const uint32_t fmtW  = 6u | (1u << 8) | (2u << 12) | (1u << 26); // widened
        const uint32_t fmtC  = 6u | (1u << 8) | (2u << 12);              // capture
        const uint64_t sigW  = UnlockOwnership::signature(fmtW, dims, 64u);
        const uint64_t sigC  = UnlockOwnership::signature(fmtC, dims, 64u);

        // -- R3-1. THE MEASURED DEFECT: same descriptor => capture REFUSED ----
        check(sigW == sigC,
              "R3-1: bit 26 is PROVENANCE, not descriptor - a widened publish "
              "and a bind capture of the same surface share one signature");
        check(own.captureVerdict(kKey, sigC) == CaptureVerdict::AdmitNoOwner,
              "R3-1: before any publish, a bind capture is admitted");
        own.claim(kKey, sigW);
        check(own.captureVerdict(kKey, sigC) == CaptureVerdict::RefuseSameEpoch,
              "R3-1: after an Unlock publish, the SAME-descriptor bind capture "
              "is REFUSED - the 0BC22000 fresh-at-#6 / all-zero-at-#15 clobber");

        // -- R3-2. INCOMPATIBLE RECREATION IS ADMITTED, AND REPLACES ---------
        const uint64_t sigFmt   = UnlockOwnership::signature(20u, dims, 64u);
        const uint64_t sigDims  = UnlockOwnership::signature(fmtC, (128u << 16) | 64u, 64u);
        const uint64_t sigPitch = UnlockOwnership::signature(fmtC, dims, 128u);
        check(own.captureVerdict(kKey, sigFmt)  == CaptureVerdict::AdmitRecreated &&
              own.captureVerdict(kKey, sigDims) == CaptureVerdict::AdmitRecreated &&
              own.captureVerdict(kKey, sigPitch)== CaptureVerdict::AdmitRecreated,
              "R3-2: a DIFFERENT format, dimensions or pitch is a recreation at "
              "this address and its capture is ADMITTED, never inherited");
        const uint64_t before = own.claimsReplaced();
        own.claim(kKey, sigFmt);
        check(own.claimsReplaced() == before + 1,
              "R3-2: claiming under a new descriptor REPLACES the old claim");
        check(own.captureVerdict(kKey, sigC) == CaptureVerdict::AdmitRecreated,
              "R3-2: and the SUPERSEDED descriptor no longer owns the key");

        // -- R3-3. DESTRUCTION / SAME-SIGNATURE REUSE START A NEW GENERATION -
        // The case a descriptor comparison ALONE cannot see: recreated at the
        // same address with the SAME descriptor. Without a generation this
        // silently inherits a dead object's ownership.
        own.reset();
        own.claim(kKey, sigW);
        check(own.captureVerdict(kKey, sigW) == CaptureVerdict::RefuseSameEpoch,
              "R3-3: baseline - the live claim refuses its own descriptor");
        own.noteObjectEvent(kKey);                      // create/destroy here
        check(own.captureVerdict(kKey, sigW) == CaptureVerdict::AdmitStaleGeneration,
              "R3-3: after an object event, the SAME-signature claim is STALE "
              "and capture is admitted - reuse never inherits ownership");
        own.claim(kKey, sigW);                          // the new object claims
        check(own.captureVerdict(kKey, sigW) == CaptureVerdict::RefuseSameEpoch,
              "R3-3: the NEW generation's own claim refuses stale capture again");
        check(own.generationOf(kKey) == 1u,
              "R3-3: exactly one generation was started");

        // -- R3-4. NORMALIZED ALIASES ARE ONE OBJECT ------------------------
        own.reset();
        own.claim(kA8, sigW);
        check(own.captureVerdict(kAA, sigW) == CaptureVerdict::RefuseSameEpoch &&
              own.captureVerdict(kKey, sigW) == CaptureVerdict::RefuseSameEpoch,
              "R3-4: a claim through the 0x8 alias owns the 0xA alias and the "
              "physical key - normalization is the boundary's job");
        own.noteObjectEvent(kAA);
        check(own.captureVerdict(kA8, sigW) == CaptureVerdict::AdmitStaleGeneration,
              "R3-4: and an object event through EITHER alias ends that epoch");

        // -- R3-5. THE LEGACY k_8 ROUTE SHARES THE SAME MACHINE --------------
        own.reset();
        const uint32_t kAtlas = 0x0BA5A000u;            // the glyph atlas
        const uint64_t sigK8 = UnlockOwnership::signature(
            2u, (128u << 16) | 128u, 128u);
        own.claim(kAtlas, sigK8);
        check(own.captureVerdict(kAtlas, sigK8) == CaptureVerdict::RefuseSameEpoch,
              "R3-5: a legacy k_8 Unlock claims through the SAME state machine "
              "as a widened publish - one ownership model, not two");
        check(own.captureVerdict(kAtlas,
                  UnlockOwnership::signature(2u, (64u << 16) | 128u, 128u)) ==
                  CaptureVerdict::AdmitRecreated,
              "R3-5: and a k_8 recreation at other dimensions is admitted");

        // -- R3-6. COUNTER CONSERVATION -------------------------------------
        own.reset();
        check(own.claimsNew() == 0 && own.claimsSame() == 0 &&
                  own.claimsReplaced() == 0 && own.refusals() == 0 &&
                  own.objectEvents() == 0 && own.trackedKeys() == 0,
              "R3-6: reset clears every counter and every tracked key");
        own.claim(0x1000u, sigW);                    // new
        own.claim(0x1000u, sigW);                    // same epoch
        own.claim(0x1000u, sigFmt);                  // replaced
        own.noteObjectEvent(0x1000u);
        own.claim(0x1000u, sigFmt);                  // replaced (new generation)
        check(own.claimsNew() == 1 && own.claimsSame() == 1 &&
                  own.claimsReplaced() == 2 && own.objectEvents() == 1,
              "R3-6: new / same-epoch / replaced claims and object events are "
              "each counted exactly once");
        check(own.publishesFor(0x1000u) == 4 && own.trackedKeys() == 1,
              "R3-6: all four publishes are attributed to the one key");
        own.reset();     // leave production state clean for the live run
    }

    // == iter-456n R4 - THE BIT-26 RECORD/REPLAY ROUND TRIP ================
    // Sixth adjudication: "add a bit-26 record/replay test proving recorder
    // payload retention, Class-A replay staging, zero handle/flag
    // disagreements, zero bad handles and once-only release."
    //
    // This is the defect that made the whole Q4 experiment vacuous: the
    // recorder answered "which pool holds these bytes?" privately, with an
    // unconditional texRead, so every unlock-origin upload was written
    // PAYLOADLESS while still looking like a well-formed record. The predicates
    // under test here are the ones the recorder, the replay staging and the
    // executor all now share.
    {
        namespace H = sr::gfx::hle;
        const H::ClassAStats base = H::classAStats();

        std::vector<uint8_t> pixels(16384);
        for (size_t i = 0; i < pixels.size(); i++)
            pixels[i] = (uint8_t)(0x40u + (i * 7u) % 0xB0u);   // never all-zero

        // -- RECORD: the producer stages into the Class-A pool ---------------
        const uint32_t handle = H::classAWrite(pixels.data(), (uint32_t)pixels.size());
        check(handle != H::kArenaFull && H::uploadIsClassA(handle),
              "R4-1: an unlock-origin publish is staged into the Class-A pool "
              "and its handle is identifiable as Class A");
        const uint32_t fmtW = 6u | (1u << 8) | (2u << 12) | H::kUploadUnlockOriginBit;
        const uint32_t fmtC = 6u | (1u << 8) | (2u << 12);
        check(H::uploadFlagAgrees(handle, fmtW),
              "R4-1: the handle and the bit-26 flag AGREE for a widened publish");
        check(!H::uploadFlagAgrees(handle, fmtC),
              "R4-1: a Class-A handle carrying no bit 26 is DETECTED as a "
              "disagreement, not silently resolved");

        // -- RECORD: payload retention. This is the exact read the recorder
        // does; before the fix it was texRead(handle) == nullptr, i.e. a
        // payloadless record.
        const uint8_t* recorded = H::classARead(handle);
        check(recorded != nullptr,
              "R4-2: the recorder can READ an unlock-origin payload (before the "
              "fix this was nullptr and the record was written payloadless)");
        bool retained = recorded != nullptr;
        if (retained)
            for (size_t i = 0; i < pixels.size(); i++)
                if (recorded[i] != pixels[i]) { retained = false; break; }
        check(retained,
              "R4-2: and the bytes it reads are byte-identical to what the "
              "producer staged");
        check(H::texRead(handle) == nullptr,
              "R4-2: the SHARED pool refuses the same handle - the two "
              "allocators cannot be crossed in either direction");

        // -- REPLAY: staging must go back to the pool the executor reads -----
        std::vector<uint8_t> fromTrace(recorded ? recorded : pixels.data(),
                                       (recorded ? recorded : pixels.data()) +
                                           pixels.size());
        const uint32_t staged =
            H::classAWrite(fromTrace.data(), (uint32_t)fromTrace.size());
        check(staged != H::kArenaFull && H::uploadIsClassA(staged),
              "R4-3: replay re-stages a bit-26 payload into the CLASS-A pool, "
              "so the executor's classARead finds it (staging it in the shared "
              "pool would drop the upload and count a badHandle)");
        const uint8_t* replayed = H::classARead(staged);
        bool same = replayed != nullptr;
        if (same)
            for (size_t i = 0; i < pixels.size(); i++)
                if (replayed[i] != pixels[i]) { same = false; break; }
        check(same,
              "R4-3: and the replayed pixels are byte-identical to the "
              "originals - the full producer -> record -> replay round trip");

        // -- ONCE-ONLY RELEASE, ZERO BAD HANDLES ----------------------------
        H::classARelease(handle, (uint32_t)pixels.size());
        H::classARelease(staged, (uint32_t)fromTrace.size());
        const H::ClassAStats after = H::classAStats();
        check(after.badHandle == base.badHandle,
              "R4-4: the whole round trip raises ZERO bad handles");
        check(after.doubleFree == base.doubleFree,
              "R4-4: and ZERO double frees");
        check(after.inFlight == base.inFlight && after.liveBytes == base.liveBytes,
              "R4-4: in-flight and live bytes return to their starting values");
        H::classARelease(handle, (uint32_t)pixels.size());
        const H::ClassAStats dbl = H::classAStats();
        check(dbl.doubleFree == after.doubleFree + 1 &&
                  dbl.inFlight == after.inFlight,
              "R4-4: a repeated release is COUNTED as a double free and does "
              "not decrement in-flight again (exact once-only release)");
    }

    // == iter-456n R5 - LIFECYCLE EVENTS TRAVEL THE STREAM, IN ORDER =======
    // Seventh adjudication, blocking defect: noteTextureObjectEvent() mutated
    // the ownership maps from the guest/API thread while claim() and
    // captureVerdict() ran on the detached render thread - an unsynchronized
    // unordered_map race. And a mutex would have fixed only the race, not the
    // semantics: the render thread lags the producer, so a create applied
    // immediately can overtake TextureUnlock/TextureUpload commands still
    // queued for the OLD object, which then execute under the NEW generation.
    //
    // R5-1 is the MUTATION GUARD for that exact regression: it fails if anyone
    // makes the producer apply the event on the calling thread again.
    {
        using sr::gfx::hle::UnlockOwnership;
        using sr::gfx::hle::CaptureVerdict;
        auto& own = sr::gfx::hle::unlockOwnership();
        own.reset();

        const uint32_t K = 0x0BC22000u;
        const uint64_t sig = UnlockOwnership::signature(
            6u | (1u << 8), (64u << 16) | 64u, 64u);

        // -- R5-1. THE PRODUCER MUST NOT APPLY, ONLY ENQUEUE ----------------
        own.claim(K, sig);
        const uint64_t genBefore = own.generationOf(K);
        const uint64_t evBefore = own.objectEvents();
        const uint64_t drainedBefore = sr::gfx::hle::drainedTotal();
        sr::gfx::hle::noteTextureObjectEvent(K, /*reason=*/0u);
        // The producer must have PUBLISHED, not applied. Sampled immediately,
        // before any drain wait, so the render thread cannot mask a direct call.
        const bool notApplied =
            own.generationOf(K) == genBefore && own.objectEvents() == evBefore;
        check(notApplied || sr::gfx::hle::drainedTotal() > drainedBefore,
              "R5-1: noteTextureObjectEvent PUBLISHES rather than applying "
              "ownership on the calling thread (fails if the direct "
              "cross-thread call is ever restored and the drain has not run)");

        // ⚠ WAIT FOR THE DRAIN, and do it for a reason beyond tidiness: the
        // render thread is started LAZILY by the first publish
        // (ensureStartedLocked) and is DETACHED, so a self-test that publishes
        // and returns leaves a brand-new thread racing static destruction on
        // the way out. Measured: 1 segfault in 6 runs with the publish, 0 in 8
        // without it, always AFTER the suite reported all tests passed. Draining
        // here removes the in-flight command AND upgrades the assertion: it
        // proves the event is applied BY THE RENDER THREAD, which is the whole
        // point of moving it into the stream.
        bool drained = false;
        for (int spin = 0; spin < 2000 && !drained; spin++) {
            if (sr::gfx::hle::drainedTotal() > drainedBefore &&
                sr::gfx::hle::queueDepth() == 0) { drained = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(drained,
              "R5-1: the published lifecycle event is drained by the render "
              "thread");
        check(!drained || own.objectEvents() == evBefore + 1,
              "R5-2: and the RENDER THREAD is what applies it - the generation "
              "advances only once the command executes, at its stream position");
        check(!drained || own.captureVerdict(K, sig) ==
                              CaptureVerdict::AdmitStaleGeneration,
              "R5-2: after the event executes, the old claim is stale and a "
              "capture of that key is admitted again");

        // -- R5-2. STREAM ORDER: old uploads, then create, then new capture --
        // The executor applies the event at its stream position. Replaying the
        // three steps in order must give: old claim owns -> event -> stale ->
        // new claim owns again.
        own.reset();
        own.claim(K, sig);                                  // old object's Unlock
        const bool s1 = own.captureVerdict(K, sig) == CaptureVerdict::RefuseSameEpoch;
        own.noteObjectEvent(K);                             // the create, in order
        const bool s2 = own.captureVerdict(K, sig) == CaptureVerdict::AdmitStaleGeneration;
        own.claim(K, sig);                                  // new object's Unlock
        const bool s3 = own.captureVerdict(K, sig) == CaptureVerdict::RefuseSameEpoch;
        check(s1 && s2 && s3,
              "R5-2b: in stream order the old claim owns, the create makes it "
              "stale, and the new object's claim owns again");

        // -- R5-3. A REORDERED EVENT PRODUCES A DIFFERENT, WRONG OUTCOME ----
        // This is the bug an immediate cross-thread event causes: the create
        // lands BEFORE the old object's queued Unlock, so the old upload then
        // claims under the NEW generation and its stale captures are refused
        // for the wrong object. If reordering were harmless this check would
        // fail, and the ordering requirement would be unfounded.
        own.reset();
        own.noteObjectEvent(K);                             // create overtakes
        own.claim(K, sig);                                  // the OLD upload
        const uint64_t genReordered = own.generationOf(K);
        own.reset();
        own.claim(K, sig);
        own.noteObjectEvent(K);
        const uint64_t genOrdered = own.generationOf(K);
        check(genReordered == 1u && genOrdered == 1u,
              "R5-3: both orders reach generation 1 - the generation COUNT "
              "alone cannot distinguish them");
        own.reset();
        own.noteObjectEvent(K); own.claim(K, sig);
        const bool reorderedVerdict =
            own.captureVerdict(K, sig) == CaptureVerdict::RefuseSameEpoch;
        own.reset();
        own.claim(K, sig); own.noteObjectEvent(K);
        const bool orderedVerdict =
            own.captureVerdict(K, sig) == CaptureVerdict::AdmitStaleGeneration;
        check(reorderedVerdict && orderedVerdict,
              "R5-3: but the VERDICTS differ - reordered says the key is owned, "
              "ordered says the claim is stale. Order is load-bearing, so it "
              "must be carried by the stream and not by wall-clock arrival");

        // -- R5-4. THE EVENT IS A REAL COMMAND WITH A NAME ------------------
        check(std::string(sr::gfx::hle::renderCommandName(
                  sr::gfx::hle::RenderCommandType::TextureObjectEvent)) ==
                  "TextureObjectEvent",
              "R5-4: the lifecycle event is a first-class command type, so the "
              "recorder and replay carry it in stream order for free");
        own.reset();
    }

    // == iter-456n R6 - THE PREAMBLE IS APPLIED-STATE *EXACT* ==============
    // Eighth adjudication, blocking defect and its predeclared killer:
    //   "H1: separate upload/unlock shadows are exact | KILLS IT: same-key
    //    two-order production preamble test and final digest comparison."
    //
    // Retention used TWO maps (s_shadowUp, s_shadowUnlock) and trace open
    // emitted every upload followed by every unlock. For a key applied through
    // BOTH classes - after physical reuse, or a descriptor/class transition -
    // the preamble was grouped by COMMAND CLASS instead of by when the write
    // happened, so an OLDER Unlock could overwrite a NEWER Upload on replay.
    // These checks drive the REAL production writer, not a copy of it.
    {
        namespace H = sr::gfx::hle;
        std::vector<uint8_t> upBytes(4096, 0xA1);
        std::vector<uint8_t> unBytes(4096, 0xB2);

        H::RenderCommand up{};
        up.type = H::RenderCommandType::TextureUpload;
        up.textureUpload.key = 0x0BC22000u;
        H::RenderCommand un{};
        un.type = H::RenderCommandType::TextureUnlock;
        un.textureUnlock.texture = 0x0BC22000u;

        const uint64_t upDigest =
            sr::gfx::ShaderCache::hashUcode(upBytes.data(), upBytes.size());
        const uint64_t unDigest =
            sr::gfx::ShaderCache::hashUcode(unBytes.data(), unBytes.size());
        check(upDigest != unDigest,
              "R6-0: the two payloads are distinguishable, so 'which one "
              "survived' is a real question");

        const uint32_t K = 0x0BC22000u;

        // -- R6-1. UPLOAD then UNLOCK: the UNLOCK must survive ---------------
        H::shadowResetForTest();
        H::shadowNoteAppliedPublic(up, upBytes.data(), (uint32_t)upBytes.size(), K);
        H::shadowNoteAppliedPublic(un, unBytes.data(), (uint32_t)unBytes.size(), K);
        uint32_t t1 = 0; uint64_t d1 = 0, q1 = 0;
        const bool got1 = H::shadowPeek(K, t1, d1, q1);
        check(got1 && d1 == unDigest &&
                  t1 == (uint32_t)H::RenderCommandType::TextureUnlock,
              "R6-1: Upload then Unlock on ONE key leaves the UNLOCK payload - "
              "the genuinely last applied write");
        check(H::shadowKeyCount() == 1,
              "R6-1: and both writes collapse into ONE record for that key, so "
              "the preamble cannot emit a stale sibling alongside it");

        // -- R6-2. UNLOCK then UPLOAD: the UPLOAD must survive ---------------
        // This is the interleaving the two-map design got WRONG: uploads were
        // emitted first, so the older unlock landed last and won on replay.
        H::shadowResetForTest();
        H::shadowNoteAppliedPublic(un, unBytes.data(), (uint32_t)unBytes.size(), K);
        H::shadowNoteAppliedPublic(up, upBytes.data(), (uint32_t)upBytes.size(), K);
        uint32_t t2 = 0; uint64_t d2 = 0, q2 = 0;
        const bool got2 = H::shadowPeek(K, t2, d2, q2);
        check(got2 && d2 == upDigest &&
                  t2 == (uint32_t)H::RenderCommandType::TextureUpload,
              "R6-2: Unlock then Upload on ONE key leaves the UPLOAD payload "
              "(the two-map design emitted uploads first and lost this)");
        check(q2 > 0,
              "R6-2: and the surviving record carries an apply sequence, which "
              "is what orders the preamble by when writes happened rather than "
              "by command class");

        // -- R6-3. APPLY ORDER IS TOTAL ACROSS KEYS AND CLASSES -------------
        H::shadowResetForTest();
        const uint32_t KA = 0x0BC22000u, KB = 0x0BC39000u;
        H::shadowNoteAppliedPublic(up, upBytes.data(), 64u, KB);   // first
        H::shadowNoteAppliedPublic(un, unBytes.data(), 64u, KA);   // second
        H::shadowNoteAppliedPublic(up, upBytes.data(), 64u, KA);   // third
        uint32_t ta = 0, tb = 0; uint64_t da = 0, db = 0, qa = 0, qb = 0;
        H::shadowPeek(KA, ta, da, qa);
        H::shadowPeek(KB, tb, db, qb);
        check(qb < qa,
              "R6-3: sequence numbers order writes across DIFFERENT keys and "
              "classes, so the preamble replays host state in the order it was "
              "actually built");

        // -- R6-4. A REFUSED COMMAND NEVER ENTERS THE PREAMBLE --------------
        // shadowNoteApplied is called ONLY from the executor's success path, so
        // "refused" is represented by never calling it. The guard rails around
        // that are what this checks: no pixels, no bytes, no record.
        H::shadowResetForTest();
        H::shadowNoteAppliedPublic(up, nullptr, 4096u, K);
        H::shadowNoteAppliedPublic(up, upBytes.data(), 0u, K);
        uint32_t t4 = 0; uint64_t d4 = 0, q4 = 0;
        check(!H::shadowPeek(K, t4, d4, q4) && H::shadowKeyCount() == 0,
              "R6-4: a write with no pixels or no bytes never enters the "
              "shadow, so a refused command cannot reach the preamble");

        // -- R6-5. GUEST ALIASES ARE ONE KEY --------------------------------
        H::shadowResetForTest();
        H::shadowNoteAppliedPublic(up, upBytes.data(), 64u, 0x8BC22000u);
        H::shadowNoteAppliedPublic(un, unBytes.data(), 64u, 0xABC22000u);
        uint32_t t5 = 0; uint64_t d5 = 0, q5 = 0;
        check(H::shadowKeyCount() == 1 && H::shadowPeek(K, t5, d5, q5) &&
                  t5 == (uint32_t)H::RenderCommandType::TextureUnlock,
              "R6-5: the 0x8 and 0xA alias windows collapse to ONE shadow "
              "record, so an alias cannot smuggle a second stale payload into "
              "the preamble");
        H::shadowResetForTest();
    }

    // == iter-456n R7 - RESTORE EXACTNESS + THE ORDERED VERDICT WITNESS ====
    // Ninth adjudication, two findings:
    //   (a) "Restored publish counts also drift when preamble records call
    //        claim() again" - the preamble's uploads replay writes the restored
    //        count ALREADY includes.
    //   (b) counts alone cannot show live and replay decided the same things in
    //        the same order, so the production decision is now witnessed as a
    //        rolling digest over (key, signature, generation, verdict).
    {
        using sr::gfx::hle::UnlockOwnership;
        using sr::gfx::hle::CaptureVerdict;
        auto& own = sr::gfx::hle::unlockOwnership();

        const uint32_t K = 0x0BC22000u, K2 = 0x0BC39000u;
        const uint64_t sigA = UnlockOwnership::signature(6u, (64u << 16) | 64u, 64u);
        const uint64_t sigB = UnlockOwnership::signature(20u, (64u << 16) | 64u, 64u);

        // -- R7-1. A RESTORED CLAIM IS NOT RE-COUNTED -----------------------
        own.reset();
        own.restoreClaim(K, sigA, /*generation=*/3u, /*publishes=*/7u);
        check(own.publishesFor(K) == 7u && own.generationOf(K) == 3u,
              "R7-1: a restore puts back the recorded generation and publish "
              "count verbatim");
        own.claim(K, sigA);   // the preamble upload replaying that same write
        check(own.publishesFor(K) == 7u,
              "R7-1: the FIRST matching claim after a restore consumes the "
              "restore marker instead of incrementing - replay's publish count "
              "no longer drifts above live's");
        // -- R7-1b. A NON-MATCHING claim must NOT consume the marker -------
        // Tenth adjudication defect: the marker was consumed by ANY first
        // claim, so a recreation or a mismatched publication was silently
        // suppressed - exactly the write that must be counted.
        own.reset();
        own.restoreClaim(K, sigA, /*generation=*/3u, /*publishes=*/7u);
        own.claim(K, sigB);          // DIFFERENT descriptor: a recreation
        check(own.publishesFor(K) == 8u,
              "R7-1b: a claim with a DIFFERENT signature does not consume the "
              "restore marker - the recreation is counted, not suppressed");
        own.reset();
        own.restoreClaim(K, sigA, /*generation=*/3u, /*publishes=*/7u);
        own.noteObjectEvent(K);      // generation moves to 4
        own.claim(K, sigA);          // same descriptor, DIFFERENT generation
        check(own.publishesFor(K) == 8u,
              "R7-1c: a claim at a DIFFERENT generation does not consume the "
              "marker either - a reused address is a new publication");
        own.reset();
        own.restoreClaim(K, sigA, /*generation=*/0u, /*publishes=*/5u);
        own.beginVerdictWindow();    // window starts; the claim never arrived
        own.claim(K, sigA);
        check(own.publishesFor(K) == 6u,
              "R7-1d: an UNMATCHED marker is cleared at window start, so it "
              "cannot suppress an unrelated later publication");

        own.reset();
        own.restoreClaim(K, sigA, 3u, 7u);
        own.claim(K, sigA);   // the matching re-assertion
        own.claim(K, sigA);   // a genuinely new publish
        check(own.publishesFor(K) == 8u,
              "R7-1: and a SUBSEQUENT claim counts normally, so the marker "
              "suppresses exactly one re-assertion and no more");
        check(own.captureVerdict(K, sigA) == CaptureVerdict::RefuseSameEpoch,
              "R7-1: the restored claim owns the key at its restored "
              "generation");

        // -- R7-2. THE WITNESS IS ORDER-SENSITIVE ---------------------------
        // The same SET of decisions in a different ORDER must produce a
        // different digest, or the digest cannot detect a reordering - which is
        // the whole reason it exists.
        own.reset();
        own.noteVerdict(K,  sigA, 0, CaptureVerdict::AdmitNoOwner);
        own.noteVerdict(K2, sigB, 1, CaptureVerdict::RefuseSameEpoch);
        const uint64_t digestAB = own.verdictDigest();
        own.reset();
        own.noteVerdict(K2, sigB, 1, CaptureVerdict::RefuseSameEpoch);
        own.noteVerdict(K,  sigA, 0, CaptureVerdict::AdmitNoOwner);
        const uint64_t digestBA = own.verdictDigest();
        check(digestAB != digestBA,
              "R7-2: the same decisions in a different ORDER give a different "
              "digest - the witness detects reordering, not just totals");

        // -- R7-3. AND FIELD-SENSITIVE --------------------------------------
        own.reset(); own.noteVerdict(K, sigA, 0, CaptureVerdict::AdmitNoOwner);
        const uint64_t base0 = own.verdictDigest();
        own.reset(); own.noteVerdict(K, sigA, 1, CaptureVerdict::AdmitNoOwner);
        const uint64_t genDiff = own.verdictDigest();
        own.reset(); own.noteVerdict(K, sigB, 0, CaptureVerdict::AdmitNoOwner);
        const uint64_t sigDiff = own.verdictDigest();
        own.reset(); own.noteVerdict(K, sigA, 0, CaptureVerdict::RefuseSameEpoch);
        const uint64_t vDiff = own.verdictDigest();
        own.reset(); own.noteVerdict(K2, sigA, 0, CaptureVerdict::AdmitNoOwner);
        const uint64_t kDiff = own.verdictDigest();
        check(base0 != genDiff && base0 != sigDiff && base0 != vDiff &&
                  base0 != kDiff,
              "R7-3: changing the key, signature, generation OR verdict changes "
              "the digest - all four fields are actually folded in");

        // -- R7-4. ALIASES DO NOT SPLIT THE WITNESS -------------------------
        own.reset(); own.noteVerdict(0x8BC22000u, sigA, 0, CaptureVerdict::AdmitNoOwner);
        const uint64_t a8 = own.verdictDigest();
        own.reset(); own.noteVerdict(0xABC22000u, sigA, 0, CaptureVerdict::AdmitNoOwner);
        const uint64_t aA = own.verdictDigest();
        check(a8 == aA,
              "R7-4: the 0x8 and 0xA aliases fold to one key in the witness, so "
              "live and replay cannot differ merely by which window they saw");

        // -- R7-5. COUNTS ACCOMPANY THE DIGEST ------------------------------
        own.reset();
        own.noteVerdict(K,  sigA, 0, CaptureVerdict::AdmitNoOwner);
        own.noteVerdict(K,  sigA, 0, CaptureVerdict::AdmitRecreated);
        own.noteVerdict(K,  sigA, 0, CaptureVerdict::AdmitStaleGeneration);
        own.noteVerdict(K,  sigA, 0, CaptureVerdict::RefuseSameEpoch);
        own.noteVerdict(K,  sigA, 0, CaptureVerdict::RefuseSameEpoch);
        check(own.verdictCount() == 5 && own.verdictNoOwner() == 1 &&
                  own.verdictRecreated() == 1 && own.verdictStaleGen() == 1 &&
                  own.verdictRefuse() == 2,
              "R7-5: every verdict class is counted, so a digest mismatch can "
              "be localized instead of only detected");
        own.reset();
    }

    // == slice 6 / CLASS B v2 - RESOLVE AUTHORITY, ADVERSARIALLY ===========
    // The director's adversarial list plus the three predeclared killers. Every
    // check drives sr::gfx::hle::unlockOwnership() - the SAME instance the
    // executor's capture decision calls - so none of this can pass against a
    // machine production does not use.
    //
    // The rule under test: a BACKEND-CONFIRMED resolve claims the same
    // (normalized key, descriptor epoch, generation) an Unlock would, and is
    // superseded by a later Unlock, a recreation, a destroy/reuse, or a
    // publication under an incompatible descriptor - by the shared epoch rule,
    // with no permanent physical-key exemption.
    {
        using sr::gfx::hle::UnlockOwnership;
        using sr::gfx::hle::CaptureVerdict;
        using sr::gfx::hle::Authority;
        auto& own = sr::gfx::hle::unlockOwnership();

        const uint32_t K   = 0x0B5BE000u;          // a real resolve target
        const uint32_t KA8 = 0x8B5BE000u, KAA = 0xAB5BE000u;
        const uint32_t dims = (640u << 16) | 360u;
        const uint64_t sigA = UnlockOwnership::signature(6u | (1u << 8), dims, 640u);
        const uint64_t sigFmt   = UnlockOwnership::signature(20u | (1u << 8), dims, 640u);
        const uint64_t sigPitch = UnlockOwnership::signature(6u | (1u << 8), dims, 1280u);
        const uint64_t sigDims  = UnlockOwnership::signature(6u | (1u << 8),
                                      (320u << 16) | 180u, 640u);

        // -- R8-1. UPLOAD -> SUCCESSFUL RESOLVE -> BIND ---------------------
        // The initial upload stays authoritative until the resolve lands; after
        // it, the resolve product refuses the stale guest capture.
        own.reset();
        check(own.captureVerdict(K, sigA) == CaptureVerdict::AdmitNoOwner,
              "R8-1: the INITIAL upload is admitted - nothing owns the key yet");
        own.claim(K, sigA);                       // the initial upload publishes
        check(own.authorityFor(K) == Authority::Unlock,
              "R8-1: and it holds the key as an Unlock authority");
        own.claimResolve(K, sigA);                // backend-confirmed resolve
        check(own.authorityFor(K) == Authority::Resolve,
              "R8-1: a confirmed resolve takes authority for that epoch");
        check(own.captureVerdict(K, sigA) == CaptureVerdict::RefuseSameEpoch,
              "R8-1: the later bind capture is REFUSED, so stale guest memory "
              "cannot overwrite the render product");

        // -- R8-2. UPLOAD -> REFUSED RESOLVE -> BIND  (KILLER H1) -----------
        // H1: "every structurally eligible resolve may acquire authority."
        // A refused resolve simply never reaches claimResolve, so the prior
        // upload must remain authoritative and the next decision unchanged.
        own.reset();
        own.claim(K, sigA);
        const CaptureVerdict before = own.captureVerdict(K, sigA);
        /* backend refused: claimResolve is NOT called */
        const CaptureVerdict after = own.captureVerdict(K, sigA);
        check(before == after && after == CaptureVerdict::RefuseSameEpoch,
              "R8-2 (H1): a REFUSED resolve changes nothing - the prior upload "
              "is still authoritative and the next capture decision is "
              "identical");
        check(own.authorityFor(K) == Authority::Unlock && own.resolveClaims() == 0,
              "R8-2 (H1): and no resolve authority was recorded at all");

        // -- R8-3. RESOLVE -> LATER UNLOCK supersedes -----------------------
        own.reset();
        own.claimResolve(K, sigA);
        own.claim(K, sigA);                       // an explicit guest Unlock
        check(own.authorityFor(K) == Authority::Unlock,
              "R8-3: a later explicit Unlock supersedes resolve authority");

        // -- R8-4. RESOLVE -> DIRECT PUBLICATION under a NEW descriptor -----
        own.reset();
        own.claimResolve(K, sigA);
        check(own.captureVerdict(K, sigFmt) == CaptureVerdict::AdmitRecreated,
              "R8-4: a publication under an INCOMPATIBLE descriptor is admitted "
              "against resolve authority, not refused forever");
        own.claim(K, sigFmt);
        check(own.authorityFor(K) == Authority::Unlock &&
                  own.captureVerdict(K, sigA) == CaptureVerdict::AdmitRecreated,
              "R8-4: and its first successful publication REPLACES the old "
              "claim, so the superseded epoch no longer owns the key");

        // -- R8-5. RECREATION / PHYSICAL-KEY REUSE  (KILLER H2) -------------
        // H2: "physical-key resolve ownership is safe across object lifetimes."
        // It is not, unless a generation advance clears it - which is exactly
        // what v1's permanent physical-key set could never do.
        own.reset();
        own.claimResolve(K, sigA);
        check(own.captureVerdict(K, sigA) == CaptureVerdict::RefuseSameEpoch,
              "R8-5 (H2): baseline - the resolve product owns the key");
        own.noteObjectEvent(K);                   // destroy / create / reuse
        check(own.captureVerdict(K, sigA) == CaptureVerdict::AdmitStaleGeneration,
              "R8-5 (H2): after the address is reused, the FIRST new-epoch "
              "publication is ADMITTED - a dead render product cannot own the "
              "next object at that address");
        own.claim(K, sigA);
        check(own.authorityFor(K) == Authority::Unlock,
              "R8-5 (H2): and the new object's own publication takes the key");

        // -- R8-6. INCOMPATIBLE SIGNATURE  (KILLER H3: dims are NOT enough) --
        // The design ruling rejected a dims-only epoch. Same key, same
        // generation, same DIMS, changed format or pitch must still admit.
        own.reset();
        own.claimResolve(K, sigA);
        check(own.captureVerdict(K, sigFmt) == CaptureVerdict::AdmitRecreated,
              "R8-6 (H3): same dims, CHANGED FORMAT is admitted - dims alone do "
              "not define the epoch");
        check(own.captureVerdict(K, sigPitch) == CaptureVerdict::AdmitRecreated,
              "R8-6 (H3): same dims, CHANGED PITCH is admitted too");
        check(own.captureVerdict(K, sigDims) == CaptureVerdict::AdmitRecreated,
              "R8-6: and changed dims are admitted, as before");
        check(own.captureVerdict(K, sigA) == CaptureVerdict::RefuseSameEpoch,
              "R8-6: while the MATCHING descriptor is still refused");

        // -- R8-7. NORMALIZED 0x8 / 0xA ALIASES -----------------------------
        own.reset();
        own.claimResolve(KA8, sigA);
        check(own.captureVerdict(KAA, sigA) == CaptureVerdict::RefuseSameEpoch &&
                  own.captureVerdict(K, sigA) == CaptureVerdict::RefuseSameEpoch,
              "R8-7: resolve authority claimed through one guest alias covers "
              "the other and the physical key");
        own.noteObjectEvent(KAA);
        check(own.captureVerdict(KA8, sigA) == CaptureVerdict::AdmitStaleGeneration,
              "R8-7: and an object event through EITHER alias ends it");

        // -- R8-8. A FAILED PUBLICATION MUST NOT POISON ITS RETRY -----------
        // The director's note: "A failed publication must not cause its retry to
        // be refused." A publication that never reached claim() leaves the
        // previous owner in place, so the retry sees the SAME verdict it saw
        // the first time rather than a refusal manufactured by the attempt.
        own.reset();
        own.claimResolve(K, sigA);
        const CaptureVerdict try1 = own.captureVerdict(K, sigFmt);
        /* publication failed: no claim recorded */
        const CaptureVerdict try2 = own.captureVerdict(K, sigFmt);
        check(try1 == try2 && try2 == CaptureVerdict::AdmitRecreated,
              "R8-8: a failed publication leaves ownership untouched, so its "
              "retry gets the same admission and is never refused because it "
              "already tried");

        // -- R8-9. CONSERVATION --------------------------------------------
        own.reset();
        check(own.resolveClaims() == 0 && own.trackedKeys() == 0,
              "R8-9: reset clears the resolve accounting with everything else");
        own.claimResolve(K, sigA);            // new
        own.claimResolve(K, sigA);            // same epoch
        own.claimResolve(K, sigFmt);          // replaced
        check(own.resolveClaims() == 3 && own.claimsNew() == 1 &&
                  own.claimsSame() == 1 && own.claimsReplaced() == 1 &&
                  own.trackedKeys() == 1,
              "R8-9: resolve claims are counted exactly once each and classed "
              "new / same-epoch / replaced like any other claim");
        check(own.publishesFor(K) == 3,
              "R8-9: and every resolve publication is attributed to the key");
        own.reset();
    }

    // == slice 6 R9 - THE CAPTURE-DISPOSITION CLASSIFIER =================
    // The OFF/ON control's whole claim is that the two arms can refuse the SAME
    // capture through DIFFERENT rules. That attribution was inline in the
    // executor and therefore untested; it is now a pure function and these
    // checks drive it directly.
    {
        using sr::gfx::hle::Authority;
        using sr::gfx::hle::CaptureDisposition;
        using sr::gfx::hle::classifyDisposition;

        // -- R9-1. THE LEGACY SET TAKES PRECEDENCE --------------------------
        // With Class-B policy OFF, ownership can say "admit" and v1's permanent
        // physical-key set still blocks. If this precedence were the other way
        // round the two arms would be indistinguishable, which is exactly the
        // confusion the disposition exists to remove.
        check(classifyDisposition(/*captureOk=*/true, /*legacyBlocked=*/true,
                                  Authority::None) ==
                  CaptureDisposition::RefusedLegacyResolveSet,
              "R9-1: ownership ADMITS but the legacy resolve set blocks -> "
              "RefusedLegacyResolveSet, not Applied");
        check(classifyDisposition(true, false, Authority::None) ==
                  CaptureDisposition::Applied,
              "R9-1: admitted with no legacy block -> Applied");

        // -- R9-2. REFUSALS NAME THEIR SOURCE ------------------------------
        check(classifyDisposition(false, false, Authority::Resolve) ==
                  CaptureDisposition::RefusedResolveEpoch,
              "R9-2: refused while a RESOLVE claim holds the epoch -> "
              "RefusedResolveEpoch");
        check(classifyDisposition(false, false, Authority::Unlock) ==
                  CaptureDisposition::RefusedUnlockEpoch,
              "R9-2: refused while an UNLOCK claim holds it -> "
              "RefusedUnlockEpoch");
        check(classifyDisposition(false, false, Authority::None) ==
                  CaptureDisposition::RefusedNoOwnerOther,
              "R9-2: refused with no authority at all is its own class, never "
              "silently attributed to one of the two");

        // -- R9-3. THE ATTRIBUTABLE DIFFERENCE ------------------------------
        // The same capture, same pixels, judged under the two arms: OFF refuses
        // through the legacy set, ON refuses through resolve authority. That
        // difference is the control's signal.
        const CaptureDisposition offArm =
            classifyDisposition(/*captureOk=*/true, /*legacyBlocked=*/true,
                                Authority::Resolve);
        const CaptureDisposition onArm =
            classifyDisposition(/*captureOk=*/false, /*legacyBlocked=*/false,
                                Authority::Resolve);
        check(offArm != onArm &&
                  offArm == CaptureDisposition::RefusedLegacyResolveSet &&
                  onArm == CaptureDisposition::RefusedResolveEpoch,
              "R9-3: the SAME refused capture is attributed to a DIFFERENT rule "
              "in each arm - equal pixels, distinguishable mechanism");

        // -- R9-4. AND THE LIFETIME DISCRIMINATOR ---------------------------
        // After a generation advance the OFF arm still refuses permanently
        // (the legacy set has no erase path) while the ON arm admits.
        const CaptureDisposition offAfterReuse =
            classifyDisposition(true, /*legacyBlocked=*/true, Authority::None);
        const CaptureDisposition onAfterReuse =
            classifyDisposition(/*captureOk=*/true, false, Authority::None);
        check(offAfterReuse == CaptureDisposition::RefusedLegacyResolveSet &&
                  onAfterReuse == CaptureDisposition::Applied,
              "R9-4: after the address is reused the OFF arm still refuses and "
              "the ON arm APPLIES - the behavioural discriminator Class B "
              "exists to fix");
    }

    // == slice 6 R10 - THE TWO DEFECTS THE DIRECTOR CAUGHT ================
    // Both were silent: a control fixture missing the very command it was built
    // around, and a rolling digest that never restarted at the window. Neither
    // would have failed any bar I had.
    {
        using sr::gfx::hle::openingCommandIsRecorded;

        // -- R10-1. THE TRIGGERING COMMAND IS SERIALIZED --------------------
        // The Present-boundary arms open ON a Present and must not record it
        // (the frame starts after it). The pre-treatment arm opens on the
        // triggering Resolve, and that Resolve IS the first treatment event.
        check(openingCommandIsRecorded(/*isPresent=*/false,
                                       /*preTreatArm=*/true),
              "R10-1: the pre-treatment arm's non-Present trigger IS recorded - "
              "without it the control fixture omits its first treatment event");
        check(!openingCommandIsRecorded(/*isPresent=*/true, true),
              "R10-1: but a Present is still not recorded even for that arm - "
              "the boundary must not be duplicated");
        check(!openingCommandIsRecorded(false, /*preTreatArm=*/false) &&
                  !openingCommandIsRecorded(true, false),
              "R10-1: and every other arm keeps the historical behaviour "
              "exactly, so banked fixtures are unaffected");

        // -- R10-1b. CONFLICTING ARMS REFUSE ------------------------------
        using sr::gfx::hle::traceArmsConflict;
        using sr::gfx::hle::traceArmsUsable;
        check(!traceArmsConflict(0) && !traceArmsConflict(1) &&
                  traceArmsConflict(2) && traceArmsConflict(6),
              "R10-1b: two or more enabled trace arms is a CONFLICT; one or "
              "none is not");
        check(!traceArmsUsable(0) && traceArmsUsable(1) && !traceArmsUsable(2),
              "R10-1b: and exactly one arm is the only usable configuration, so "
              "a conflict refuses to record rather than silently picking");

        // -- R10-2. THE DISPOSITION WINDOW RESTARTS -------------------------
        // A rolling digest that carries pre-window history makes live/replay
        // equality meaningless: both sides would have to have travelled the
        // same history, which is precisely what a replay does not do.
        sr::gfx::hle::resetDispositionWindow();
        const uint64_t d0 = sr::gfx::hle::dispositionDigest();
        const uint64_t n0 = sr::gfx::hle::dispositionCount();
        check(d0 == 0 && n0 == 0,
              "R10-2: resetting the disposition window zeroes the digest and "
              "the count");
        bool kindsZero = true;
        for (uint32_t k = 0; k < 6; k++)
          if (sr::gfx::hle::dispositionOfKind(k) != 0) kindsZero = false;
        check(kindsZero,
              "R10-2: and every per-kind counter, so a pre-window refusal "
              "cannot be counted as an in-window one");
    }

    // == slice 6 R11 - BINDING THE PRODUCTION PATH, NOT THE PREDICATE =====
    // The director's correction: R10 tested the predicate and the reset
    // FUNCTION, so restoring the unconditional return in traceRecord(), or
    // deleting a production reset call, would still have passed. These drive
    // the real operations instead.
    {
        namespace H = sr::gfx::hle;

        // -- R11-1. THE REAL BOUNDARY WRITER EMITS marker -> TRIGGER --------
        // recorderBoundarySelfTest drives writeWindowStartAndTrigger, the same
        // function live recording calls, into a temp file and reads it back.
        {
            const char* path = "archive/rec_boundary_selftest.bin";
            uint32_t kinds[2] = {0, 0}; uint32_t key = 0; uint64_t n = 0;
            const bool ok = H::recorderBoundarySelfTest(
                path, /*preTreatArm=*/true, H::RenderCommandType::Resolve,
                0x0B5BE000u, kinds, key, n);
            check(ok, "R11-1: the bounded recorder driver ran");
            check(ok && n == 2,
                  "R11-1: the pre-treatment boundary writes TWO records - the "
                  "window marker and the trigger");
            check(ok && kinds[0] == (uint32_t)H::RenderCommandType::OwnershipRestore,
                  "R11-1: the first is the WINDOW-START marker");
            check(ok && kinds[1] == (uint32_t)H::RenderCommandType::Resolve &&
                      key == 0x0B5BE000u,
                  "R11-1: and the second is the EXACT triggering Resolve, "
                  "carrying its own destKey - the first treatment event is in "
                  "the trace, not lost to an early return");
        }

        // -- R11-2. AND EVERY OTHER ARM IS UNCHANGED ------------------------
        {
            const char* path = "archive/rec_boundary_selftest2.bin";
            uint32_t kinds[2] = {0, 0}; uint32_t key = 0; uint64_t n = 0;
            const bool ok = H::recorderBoundarySelfTest(
                path, /*preTreatArm=*/false, H::RenderCommandType::Present, 0u,
                kinds, key, n);
            check(ok && n == 1 &&
                      kinds[0] == (uint32_t)H::RenderCommandType::OwnershipRestore,
                  "R11-2: a non-pre-treatment arm writes the marker ALONE, so "
                  "banked fixtures keep their historical boundary exactly");
        }

        // -- R11-3. ONE WINDOW-START OPERATION, AFTER A CONTAMINATED HISTORY -
        // Both witnesses must restart TOGETHER. Driving the shared operation
        // means deleting either reset from it fails here.
        {
            using sr::gfx::hle::UnlockOwnership;
            using sr::gfx::hle::CaptureVerdict;
            auto& own = sr::gfx::hle::unlockOwnership();
            own.reset();
            H::resetDispositionWindow();
            // contaminate BOTH witnesses with pre-window history
            const uint64_t sg = UnlockOwnership::signature(6u, (8u << 16) | 8u, 8u);
            own.noteVerdict(0x1000u, sg, 0, CaptureVerdict::AdmitNoOwner);
            own.noteVerdict(0x1000u, sg, 0, CaptureVerdict::RefuseSameEpoch);
            H::noteDispositionForTest(0x2000u, sg,
                                      H::CaptureDisposition::RefusedUnlockEpoch);
            H::noteDispositionForTest(0x2000u, sg,
                                      H::CaptureDisposition::Applied);
            check(own.verdictCount() == 2 && own.verdictDigest() != 0,
                  "R11-3: the verdict witness carries pre-window history");
            check(H::dispositionCount() == 2 && H::dispositionDigest() != 0 &&
                      H::dispositionOfKind(0) == 1,
                  "R11-3: and the DISPOSITION witness carries its own - both "
                  "must be contaminated or the reset is not being bound");
            H::beginWitnessWindow();          // THE production operation
            check(own.verdictCount() == 0 && own.verdictDigest() == 0,
                  "R11-3: the shared window start clears the VERDICT witness");
            check(H::dispositionCount() == 0 && H::dispositionDigest() == 0,
                  "R11-3: and the DISPOSITION witness, in the same operation - "
                  "so neither can be left contaminated by dropping one call");
            own.reset();
        }

        // -- R11-4. ARM VARIABLES ARE READ BY EFFECTIVE VALUE --------------
        // "0" and "" mean DISABLED. Counting presence made the obvious way of
        // turning an arm off register as a conflict.
        check(!H::envArmEnabled(nullptr) && !H::envArmEnabled("") &&
                  !H::envArmEnabled("0"),
              "R11-4: unset, empty and \"0\" are all DISABLED");
        check(H::envArmEnabled("1") && H::envArmEnabled("6") &&
                  H::envArmEnabled("0BA5A000"),
              "R11-4: any other value enables, including a hex key and a count");
    }

    // == slice 6 R12 - BINDING WHAT R11 STILL DID NOT ====================
    // The director showed R11 was still short: deleting BOTH production witness
    // resets passed 157/157, and restoring presence-counting passed too. R11
    // drove the helpers; these drive the production decisions.
    {
        namespace H = sr::gfx::hle;
        using sr::gfx::hle::UnlockOwnership;
        using sr::gfx::hle::CaptureVerdict;
        auto& own = sr::gfx::hle::unlockOwnership();

        // -- R12-1. THE BOUNDARY OPERATION STARTS THE WINDOW ---------------
        // beginWitnessWindow() now lives INSIDE writeWindowStartAndTrigger(),
        // so driving the real boundary writer must clear both witnesses. If
        // that call is deleted from production, this fails.
        own.reset();
        H::resetDispositionWindow();
        const uint64_t sg = UnlockOwnership::signature(6u, (8u << 16) | 8u, 8u);
        own.noteVerdict(0x3000u, sg, 0, CaptureVerdict::RefuseSameEpoch);
        H::noteDispositionForTest(0x3000u, sg,
                                  H::CaptureDisposition::RefusedResolveEpoch);
        check(own.verdictCount() == 1 && H::dispositionCount() == 1,
              "R12-1: both witnesses carry pre-window history");
        {
            const char* path = "archive/rec_boundary_r12.bin";
            uint32_t kinds[2] = {0, 0}; uint32_t key = 0; uint64_t n = 0;
            H::recorderBoundarySelfTest(path, true,
                                        H::RenderCommandType::Resolve,
                                        0x0BF36000u, kinds, key, n);
        }
        check(own.verdictCount() == 0 && own.verdictDigest() == 0,
              "R12-1: driving the REAL boundary writer cleared the verdict "
              "witness - the window start is part of that operation, not a "
              "separate call that can be dropped unnoticed");
        check(H::dispositionCount() == 0 && H::dispositionDigest() == 0,
              "R12-1: and the disposition witness, in the same operation");

        // -- R12-2. THE TRIGGER IS WRITTEN EXACTLY ONCE --------------------
        {
            const char* path = "archive/rec_boundary_r12b.bin";
            uint32_t kinds[2] = {0, 0}; uint32_t key = 0; uint64_t n = 0;
            H::recorderBoundarySelfTest(path, true,
                                        H::RenderCommandType::Resolve,
                                        0x0B5BE000u, kinds, key, n);
            check(n == 2,
                  "R12-2: the boundary writes EXACTLY two records - it wrote "
                  "the trigger a second time when the open path also fell "
                  "through to the generic recorder");
        }

        // -- R12-3. THE PRODUCTION ARM DECISION, DRIVEN --------------------
        // countEnabledArms() is what production calls; the lookup is the only
        // parameter, so a stub binds the real parsing AND counting rule.
        check(H::countEnabledArms([](const char*) -> const char* {
                  return nullptr; }) == 0,
              "R12-3: no arm variables set -> zero arms");
        check(H::countEnabledArms([](const char* n) -> const char* {
                  return std::strcmp(n, "SR_HLE_TRACE_ARM_PRERESOLVE") == 0
                             ? "1" : nullptr; }) == 1,
              "R12-3: one arm set -> exactly one");
        check(H::countEnabledArms([](const char*) -> const char* {
                  return "0"; }) == 0,
              "R12-3: EVERY arm set to \"0\" is zero arms - presence-counting "
              "made the obvious way of disabling an arm register as active");
        check(H::countEnabledArms([](const char* n) -> const char* {
                  return std::strcmp(n, "SR_HLE_TRACE_ARM_UNLOCKN") == 0
                             ? "6"
                         : std::strcmp(n, "SR_HLE_TRACE_ARM_PRERESOLVE") == 0
                             ? "1" : "0"; }) == 2,
              "R12-3: two genuinely-enabled arms are a conflict, while the "
              "others sitting at \"0\" are not counted");
        // -- R12-4. THE PRODUCTION VALUE, THROUGH THE REAL ENVIRONMENT -----
        // R12-3 drives the shared function; this drives what production
        // actually computes, so a hand-rolled counting rule at the call site
        // cannot slip past.
        _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "0");
        _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "");
        check(H::productionArmCount() == 0,
              "R12-4: an arm variable set to \"0\" and one set to empty are "
              "BOTH disabled in the production count");
        _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "6");
        check(H::productionArmCount() == 1,
              "R12-4: setting it to a real value enables exactly one arm");
        _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "1");
        check(H::productionArmCount() == 2 &&
                  sr::gfx::hle::traceArmsConflict(H::productionArmCount()),
              "R12-4: two genuinely-enabled arms are a conflict in the "
              "PRODUCTION count, not just in the helper");
        _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "");
        _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "");
        own.reset();
    }

    // == slice 6 R13 - DRIVE THE REAL DISPATCHERS ========================
    // Four production mutations survived R12 because every test reached a
    // helper while the decisions sat in the live recorder's opening path, the
    // replay marker dispatcher and the conflict refusal. These call
    // traceRecord() and execute() themselves.
    {
        namespace H = sr::gfx::hle;
        using sr::gfx::hle::UnlockOwnership;
        using sr::gfx::hle::CaptureVerdict;
        auto& own = sr::gfx::hle::unlockOwnership();

        auto resolveCmd = [](uint32_t key) {
            H::RenderCommand c{};
            c.type = H::RenderCommandType::Resolve;
            c.resolve.destKey = key;
            c.resolve.levelSliceFlags = sr::gfx::hle::kResolveDescValidBit;
            return c;
        };

        // -- R13-1. THE LIVE OPENING PATH WRITES marker -> TRIGGER ---------
        _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "1");
        _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "");
        _putenv_s("SR_HLE_TRACE_ARM_RESOLVEN", "");
        _putenv_s("SR_HLE_TRACE_ARM_TEXKEY", "");
        _putenv_s("SR_HLE_TRACE_ARM_GEOM", "");
        _putenv_s("SR_HLE_TRACE_HOTKEY", "");
        own.reset();
        {
            const char* path = "archive/rec_live_r13.bin";
            H::resetRecorderForTest(path);
            H::traceRecordForTest(resolveCmd(0x0B5BE000u));   // THE real path
            const bool opened = H::recorderOpenedForTest();
            const uint64_t recs = H::recorderRecordsForTest();
            H::closeRecorderForTest();
            check(opened,
                  "R13-1: traceRecord() opened the trace on the first "
                  "descriptor-valid Resolve from a clean pre-treatment state");
            check(opened && recs == 2,
                  "R13-1: and the LIVE opening path wrote EXACTLY two records - "
                  "the window marker and the trigger. Removing the boundary "
                  "writer, or restoring the duplicate fall-through, changes "
                  "this number.");
        }

        // -- R13-2. PRODUCTION CONFLICT REFUSAL ---------------------------
        _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "6");   // a SECOND enabled arm
        {
            const char* path = "archive/rec_live_r13b.bin";
            H::resetRecorderForTest(path);
            H::traceRecordForTest(resolveCmd(0x0B5BE000u));
            const bool opened = H::recorderOpenedForTest();
            H::closeRecorderForTest();
            check(!opened,
                  "R13-2: with TWO arms enabled the live recorder REFUSES to "
                  "open - bypassing that refusal in production fails here");
        }
        _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "");
        _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "");

        // -- R13-3. THE REPLAY MARKER DISPATCHER --------------------------
        // execute() on an OwnershipRestore with key==0 must start the window.
        {
            const uint64_t sg =
                UnlockOwnership::signature(6u, (8u << 16) | 8u, 8u);
            own.reset();
            H::resetDispositionWindow();
            own.noteVerdict(0x4000u, sg, 0, CaptureVerdict::RefuseSameEpoch);
            H::noteDispositionForTest(0x4000u, sg,
                                      H::CaptureDisposition::Applied);
            check(own.verdictCount() == 1 && H::dispositionCount() == 1,
                  "R13-3: both witnesses carry history before the marker");
            H::RenderCommand marker{};
            marker.type = H::RenderCommandType::OwnershipRestore;
            marker.ownershipRestore.key = 0u;
            H::executeForTest(marker);            // THE real dispatcher
            check(own.verdictCount() == 0 && H::dispositionCount() == 0,
                  "R13-3: executing the key-zero marker through execute() "
                  "starts the window on BOTH witnesses - removing that reset "
                  "from the replay dispatcher fails here");
        }

        // -- R13-4. A NON-ZERO OwnershipRestore IS STILL A CLAIM -----------
        {
            const uint64_t sg =
                UnlockOwnership::signature(6u, (64u << 16) | 64u, 64u);
            own.reset();
            H::RenderCommand rc{};
            rc.type = H::RenderCommandType::OwnershipRestore;
            rc.ownershipRestore.key = 0x0BC22000u;
            rc.ownershipRestore.generation = 2u;
            rc.ownershipRestore.sigLo = (uint32_t)(sg & 0xFFFFFFFFu);
            rc.ownershipRestore.sigHi = (uint32_t)(sg >> 32);
            rc.ownershipRestore.publishes = 5u;
            rc.ownershipRestore.reserved = (uint32_t)H::Authority::Resolve;
            H::executeForTest(rc);
            check(own.publishesFor(0x0BC22000u) == 5u &&
                      own.generationOf(0x0BC22000u) == 2u &&
                      own.authorityFor(0x0BC22000u) == H::Authority::Resolve,
                  "R13-4: a non-zero row restores key, generation, publishes "
                  "AND authority through the same dispatcher - the marker "
                  "encoding cannot swallow a real claim");
            own.reset();
        }
    }

    // == slice 6 R14 - BIND THE ACQUISITION CONTRACT TO THE FILE'S BYTES ==
    // R13 asserted the live opening path wrote TWO records, which is why
    // substituting a different Resolve still passed: a count cannot see
    // content. These read the production file back and compare bytes, and
    // cover the two guard conditions R13 never exercised.
    {
        namespace H = sr::gfx::hle;
        using sr::gfx::hle::UnlockOwnership;
        using sr::gfx::hle::CaptureVerdict;
        auto& own = sr::gfx::hle::unlockOwnership();

        // The on-disk record: a 32-byte RenderCommand, then payloadBytes,
        // reserved and an 8-byte hash. Parsed here rather than trusting a
        // count.
        struct RawRec { H::RenderCommand cmd; uint32_t payloadBytes;
                        uint32_t reserved; uint64_t hash; };
        auto readRecords = [](const char* path, RawRec* out, int maxN,
                              int& n, bool& cleanEof) {
            n = 0; cleanEof = false;
            FILE* f = std::fopen(path, "rb");
            if (!f) return false;
            // The file opens with a 32-byte TraceHeader (magic, version,
            // records, payloadBytes, presents, reserved); records follow it.
            std::fseek(f, 32, SEEK_SET);
            while (n < maxN && std::fread(&out[n], sizeof(RawRec), 1, f) == 1) n++;
            // EOF must be exact: no trailing bytes after the last record.
            const long here = std::ftell(f);
            std::fseek(f, 0, SEEK_END);
            cleanEof = (here == std::ftell(f));
            std::fclose(f);
            return true;
        };

        auto onlyPreTreatArm = [] {
            _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "1");
            _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "");
            _putenv_s("SR_HLE_TRACE_ARM_RESOLVEN", "");
            _putenv_s("SR_HLE_TRACE_ARM_TEXKEY", "");
            _putenv_s("SR_HLE_TRACE_ARM_GEOM", "");
            _putenv_s("SR_HLE_TRACE_HOTKEY", "");
        };
        auto resolveCmd = [](uint32_t key, bool descValid) {
            H::RenderCommand c{};
            c.type = H::RenderCommandType::Resolve;
            c.resolve.destKey = key;
            c.resolve.destSize = 0x0059E4FFu;
            c.resolve.levelSliceFlags =
                descValid ? sr::gfx::hle::kResolveDescValidBit : 0u;
            return c;
        };

        // -- R14-1. THE FILE IS marker -> BYTE-EXACT TRIGGER -> EOF --------
        onlyPreTreatArm();
        own.reset();
        H::clearResolveOwnedForTest();
        {
            const char* path = "archive/rec_r14_a.bin";
            const H::RenderCommand trig = resolveCmd(0x0B5BE000u, true);
            H::resetRecorderForTest(path);
            H::traceRecordForTest(trig);
            H::closeRecorderForTest();

            RawRec recs[8]; int n = 0; bool cleanEof = false;
            const bool read = readRecords(path, recs, 8, n, cleanEof);
            check(read && n == 2,
                  "R14-1: after the 32-byte header the live file holds EXACTLY "
                  "two records");
            // recs[0] is the 32-byte blank header the recorder writes first.
            const bool markerOk =
                read && n == 2 &&
                recs[0].cmd.type == H::RenderCommandType::OwnershipRestore &&
                recs[0].cmd.ownershipRestore.key == 0u;
            check(markerOk,
                  "R14-1: the first trace record is the WINDOW-START marker "
                  "(OwnershipRestore, key 0)");
            const bool trigOk =
                read && n == 2 &&
                std::memcmp(&recs[1].cmd, &trig, sizeof(H::RenderCommand)) == 0;
            check(trigOk,
                  "R14-1: the second is the triggering Resolve BYTE-EXACT - "
                  "substituting a different Resolve fails here, which a record "
                  "count could never see");
            check(read && cleanEof,
                  "R14-1: and the file ends exactly there - no trailing bytes");
        }

        // -- R14-2. AN INVALID RESOLVE DOES NOT OPEN; A VALID ONE DOES -----
        // Removing the descriptor-valid-bit requirement fails here.
        onlyPreTreatArm();
        own.reset();
        H::clearResolveOwnedForTest();
        {
            const char* path = "archive/rec_r14_b.bin";
            H::resetRecorderForTest(path);
            H::traceRecordForTest(resolveCmd(0x0B5BE000u, /*descValid=*/false));
            const bool openedOnInvalid = H::recorderOpenedForTest();
            check(!openedOnInvalid,
                  "R14-2: a Resolve WITHOUT the descriptor-valid bit does not "
                  "open the trace - a fixture armed on one would carry no "
                  "descriptor to compare");
            H::traceRecordForTest(resolveCmd(0x0BF36000u, /*descValid=*/true));
            const bool openedOnValid = H::recorderOpenedForTest();
            const uint64_t recs = H::recorderRecordsForTest();
            H::closeRecorderForTest();
            check(openedOnValid && recs == 2,
                  "R14-2: and the NEXT descriptor-valid Resolve opens it, so "
                  "the requirement gates the arm rather than disabling it");
        }

        // -- R14-3. DIRTY STATE REFUSES - via an Authority::Resolve claim ---
        onlyPreTreatArm();
        own.reset();
        H::clearResolveOwnedForTest();
        {
            const uint64_t sg =
                UnlockOwnership::signature(6u, (64u << 16) | 64u, 64u);
            own.claimResolve(0x0BC22000u, sg);      // state is NOT pre-treatment
            const char* path = "archive/rec_r14_c.bin";
            H::resetRecorderForTest(path);
            H::traceRecordForTest(resolveCmd(0x0B5BE000u, true));
            const bool opened = H::recorderOpenedForTest();
            H::closeRecorderForTest();
            check(!opened,
                  "R14-3: with a live Authority::Resolve claim the arm REFUSES "
                  "to open - the fixture would bake the treatment into its own "
                  "preamble");
            own.reset();
        }

        // -- R14-4. DIRTY STATE REFUSES - via the LEGACY set, independently --
        onlyPreTreatArm();
        own.reset();
        H::clearResolveOwnedForTest();
        {
            H::seedResolveOwnedForTest(0x0BB6D000u);   // v1's permanent set
            const char* path = "archive/rec_r14_d.bin";
            H::resetRecorderForTest(path);
            H::traceRecordForTest(resolveCmd(0x0B5BE000u, true));
            const bool opened = H::recorderOpenedForTest();
            H::closeRecorderForTest();
            check(!opened,
                  "R14-4: a non-empty LEGACY resolve set refuses on its own, "
                  "with no ownership claim present - the two dirty-state "
                  "conditions are bound independently");
            H::clearResolveOwnedForTest();
        }

        // -- R14-5. THE LIVE BOUNDARY RESETS BOTH WITNESSES ----------------
        // R13-3 covered the REPLAY dispatcher; this covers the LIVE one.
        onlyPreTreatArm();
        own.reset();
        H::clearResolveOwnedForTest();
        {
            const uint64_t sg =
                UnlockOwnership::signature(6u, (8u << 16) | 8u, 8u);
            H::resetDispositionWindow();
            own.noteVerdict(0x5000u, sg, 0, CaptureVerdict::AdmitNoOwner);
            H::noteDispositionForTest(0x5000u, sg,
                                      H::CaptureDisposition::Applied);
            check(own.verdictCount() == 1 && H::dispositionCount() == 1,
                  "R14-5: both witnesses carry history before the live open");
            const char* path = "archive/rec_r14_e.bin";
            H::resetRecorderForTest(path);
            H::traceRecordForTest(resolveCmd(0x0B5BE000u, true));
            H::closeRecorderForTest();
            check(own.verdictCount() == 0 && own.verdictDigest() == 0 &&
                      H::dispositionCount() == 0 && H::dispositionDigest() == 0,
                  "R14-5: opening the LIVE trace restarted both witnesses");
        }

        _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "");
        own.reset();
        H::clearResolveOwnedForTest();
    }

    // == slice 6 R15 - THE HEADER IS PART OF THE ARTIFACT ===============
    // R14 read the file, then SKIPPED its first 32 bytes. Two mutations lived
    // in exactly that blind spot: corrupting the magic, and no-op-ing the
    // header patch at auto-close so the blank header written at open survives.
    // Both produce a file replay would refuse, and both passed 184/184.
    //
    // This drives the REAL traceRecord() to auto-close on a Present (the test
    // never closes the file) and then parses the whole artifact FAIL-CLOSED:
    // every field, every record, and the header's totals reconciled against an
    // independent walk.
    {
        namespace H = sr::gfx::hle;
        auto& own = sr::gfx::hle::unlockOwnership();

        // Mirrors gfx/hle_video.cpp's TraceHeader. Deliberately re-declared
        // rather than shared: if the on-disk layout changes, this test must be
        // edited on purpose.
        struct FileHeader {
            char     magic[4];
            uint32_t version;
            uint64_t records;
            uint64_t payloadBytes;
            uint32_t presents;
            uint32_t reserved;
        };
        struct RawRec { H::RenderCommand cmd; uint32_t payloadBytes;
                        uint32_t reserved; uint64_t hash; };

        struct Walk {
            bool       ok = false;        // the parse itself succeeded
            FileHeader hdr{};
            RawRec     rec[8];
            int        n = 0;
            uint64_t   walkedPayload = 0;
            uint32_t   walkedPresents = 0;
            bool       cleanEof = false;
        };
        // FAIL-CLOSED: any short read, any record whose inline payload runs off
        // the end, or any trailing byte leaves ok=false. Nothing is assumed
        // from the header - the walk is independent, so the two can disagree.
        auto walkTrace = [](const char* path, Walk& w) {
            FILE* f = std::fopen(path, "rb");
            if (!f) return;
            if (std::fread(&w.hdr, sizeof(FileHeader), 1, f) != 1) {
                std::fclose(f); return;
            }
            std::fseek(f, 0, SEEK_END);
            const long fileEnd = std::ftell(f);
            std::fseek(f, (long)sizeof(FileHeader), SEEK_SET);
            while (w.n < 8) {
                // ⚠ DECIDE FROM BYTES REMAINING, BEFORE READING. A failed
                // fread CONSUMES a short final fragment and leaves the
                // position at fileEnd, so testing ftell afterwards reported a
                // clean EOF on an artifact carrying a trailing byte - which is
                // exactly how a one-byte production tail passed 193/193 while
                // srht_derive.py refused the same file.
                const long remaining = fileEnd - std::ftell(f);
                if (remaining == 0) break;                       // exact EOF
                if (remaining < (long)sizeof(RawRec)) {          // short tail
                    std::fclose(f); return;                      // ok stays false
                }
                if (std::fread(&w.rec[w.n], sizeof(RawRec), 1, f) != 1) {
                    std::fclose(f); return;
                }
                const uint32_t pb = w.rec[w.n].payloadBytes;
                if (pb) {
                    if (std::ftell(f) + (long)pb > fileEnd) { std::fclose(f); return; }
                    std::fseek(f, (long)pb, SEEK_CUR);
                    w.walkedPayload += pb;
                }
                if (w.rec[w.n].cmd.type == H::RenderCommandType::Present)
                    w.walkedPresents++;
                w.n++;
            }
            w.cleanEof = (std::ftell(f) == fileEnd);
            w.ok = true;
            std::fclose(f);
        };

        _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "1");
        _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "");
        _putenv_s("SR_HLE_TRACE_ARM_RESOLVEN", "");
        _putenv_s("SR_HLE_TRACE_ARM_TEXKEY", "");
        _putenv_s("SR_HLE_TRACE_ARM_GEOM", "");
        _putenv_s("SR_HLE_TRACE_HOTKEY", "");
        _putenv_s("SR_HLE_TRACE_FRAMES", "1");   // one Present closes the trace
        own.reset();
        H::clearResolveOwnedForTest();

        H::RenderCommand trig{};
        trig.type = H::RenderCommandType::Resolve;
        trig.resolve.destKey = 0x0B5BE000u;
        trig.resolve.destSize = 0x0059E4FFu;
        trig.resolve.levelSliceFlags = sr::gfx::hle::kResolveDescValidBit;
        H::RenderCommand pres{};
        pres.type = H::RenderCommandType::Present;

        const char* path = "archive/rec_r15_a.bin";
        H::resetRecorderForTest(path);
        H::traceRecordForTest(trig);
        check(H::recorderOpenedForTest(),
              "R15-1: the valid Resolve opened the trace");
        H::traceRecordForTest(pres);
        // NOT closed by the test. If production's header patch is removed the
        // file keeps the blank header written at open, and every check below
        // that reads a header field fails.
        check(!H::recorderOpenedForTest(),
              "R15-1: the Present drove PRODUCTION auto-close at "
              "SR_HLE_TRACE_FRAMES=1 - the test never closed the file");
        H::releaseTracePathForTest();

        Walk w;
        walkTrace(path, w);
        check(w.ok, "R15-2: the artifact parses fail-closed end to end");
        check(w.ok && w.hdr.magic[0] == 'S' && w.hdr.magic[1] == 'R' &&
                  w.hdr.magic[2] == 'H' && w.hdr.magic[3] == 'T',
              "R15-2: magic is SRHT - corrupting one magic byte produces a file "
              "replay refuses, which nothing tested before");
        // iter-456p Step 3A: DELIBERATELY BUMPED 5 -> 6. This literal did its
        // job — it refused to pass on its own after VertexBufferPublish landed
        // and SetStreamSource became a consumed binding, which forced this edit
        // to be made and reviewed rather than inherited. It stays a literal.
        //
        // iter-457 H1a: DELIBERATELY BUMPED 6 -> 7, and it did its job a second
        // time. v7 is the CUBE semantic gate: a v5/v6 record claiming a cube
        // upload is refused by name rather than interpreted, because mipBytes
        // lives in a union dword those versions never wrote and which held
        // indeterminate stack bytes. The envelope is unchanged, so this is the
        // only place the bump is visible - which is exactly why it is a literal.
        // Astra 086: DELIBERATELY moved to 8 - the ClearF colour payload.
        check(w.ok && w.hdr.version == 8u,
              "R15-2: version is 8 (literal on purpose: a format bump must be a "
              "deliberate edit here, not a silently-passing test)");
        check(w.ok && w.hdr.records == 3u && w.hdr.payloadBytes == 0u &&
                  w.hdr.presents == 1u && w.hdr.reserved == 0u,
              "R15-3: the header states records=3 payloadBytes=0 presents=1 "
              "reserved=0 - a no-op-ed header patch leaves the blank header and "
              "fails right here");
        check(w.ok && w.n == 3 && w.cleanEof,
              "R15-3: and the walk finds exactly three records, then EOF");
        const bool totalsAgree =
            w.ok && w.hdr.records == (uint64_t)w.n &&
            w.hdr.payloadBytes == w.walkedPayload &&
            w.hdr.presents == w.walkedPresents;
        check(totalsAgree,
              "R15-3: the header's totals equal an INDEPENDENT walk of the "
              "records - the header is reconciled, not trusted");
        const bool seqOk =
            w.ok && w.n == 3 &&
            w.rec[0].cmd.type == H::RenderCommandType::OwnershipRestore &&
            w.rec[0].cmd.ownershipRestore.key == 0u &&
            std::memcmp(&w.rec[1].cmd, &trig, sizeof(H::RenderCommand)) == 0 &&
            std::memcmp(&w.rec[2].cmd, &pres, sizeof(H::RenderCommand)) == 0;
        check(seqOk,
              "R15-4: the sequence is marker -> the exact Resolve -> the exact "
              "Present, all byte-compared");

        // -- R15-5. THE TRAILING-FRAGMENT BOUNDARY -------------------------
        // The parser must refuse ANY non-record byte after the third record,
        // including a fragment too short to be read as a record. Copies of the
        // real artifact with a deliberate tail stand in for the production
        // one-byte-append mutation, so the boundary is bound at every size
        // rather than at the single size that happened to be mutated.
        {
            auto copyWithTail = [](const char* src, const char* dst, int extra) {
                FILE* a = std::fopen(src, "rb");
                FILE* b = std::fopen(dst, "wb");
                if (!a || !b) { if (a) std::fclose(a); if (b) std::fclose(b);
                                return false; }
                unsigned char buf[4096];
                size_t got;
                while ((got = std::fread(buf, 1, sizeof(buf), a)) > 0)
                    std::fwrite(buf, 1, got, b);
                for (int i = 0; i < extra; i++) {
                    const unsigned char pad = 0xA5;
                    std::fwrite(&pad, 1, 1, b);
                }
                std::fclose(a); std::fclose(b);
                return true;
            };

            // exact EOF still passes, on a byte-for-byte copy
            Walk w0;
            const bool c0 = copyWithTail(path, "archive/rec_r15_t0.bin", 0);
            if (c0) walkTrace("archive/rec_r15_t0.bin", w0);
            check(c0 && w0.ok && w0.n == 3 && w0.cleanEof,
                  "R15-5: an untouched copy still parses - the tail check did "
                  "not simply refuse everything");

            // every short-tail size in 1..sizeof(RawRec)-1 must FAIL
            const int sizes[] = {1, 2, 17, 47};
            bool allRefused = true;
            int firstAccepted = 0;
            for (int e : sizes) {
                char dst[64];
                std::snprintf(dst, sizeof(dst), "archive/rec_r15_t%d.bin", e);
                Walk wt;
                if (!copyWithTail(path, dst, e)) { allRefused = false; break; }
                walkTrace(dst, wt);
                if (wt.ok) { allRefused = false; if (!firstAccepted) firstAccepted = e; }
            }
            check(allRefused && (int)sizeof(RawRec) == 48,
                  "R15-5: every trailing fragment of 1, 2, 17 and 47 bytes is "
                  "REFUSED (sizeof(RawRec)==48, so 47 is the largest tail a "
                  "record read cannot consume) - the one-byte production tail "
                  "that passed 193/193 fails here");

            // and a FULL extra record's worth of tail is refused too, by the
            // record cap and the cleanEof reconciliation rather than by size.
            Walk wf;
            copyWithTail(path, "archive/rec_r15_t48.bin", 48);
            walkTrace("archive/rec_r15_t48.bin", wf);
            check(!(wf.ok && wf.n == 3 && wf.cleanEof),
                  "R15-5: a full 48-byte tail is refused as well - the walk "
                  "either reads a fourth record or fails, never reports three "
                  "records and a clean EOF");
        }

        _putenv_s("SR_HLE_TRACE_FRAMES", "");
        _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "");
        own.reset();
        H::clearResolveOwnedForTest();
    }

    // == iter-456p R16 - THE OBSERVER-ONLY VERSION BANKER ==============
    // The director authorized ONE K=1 observer acquisition and required the
    // instrumentation to be frozen and proven BEFORE the roll is spent. These
    // bind the pure qualification rules; the sidecar and replay-neutrality
    // halves are separate.
    {
        namespace V = sr::gfx::hle;
        using V::BankVerdict;

        auto mkEpoch = [](uint32_t key) {
            V::EpochId e;
            e.key = key; e.headerIdent = 0xA1B2C3D4u;
            e.fmtBits = 2u;                       // fmt 2, linear, endian 0
            e.dims = (64u << 16) | 64u;
            e.pitch = 256u;                       // 64 px * 4 B = 256, no padding
            e.dimLevel = 0; e.aliasLo = key; e.aliasHi = key + 0x10000u;
            e.allocGeneration = 7; e.descEpoch = 1;
            return e;
        };

        // -- R16-1. THE GATE MATRIX: conflicts REFUSE, never silently choose --
        {
            V::AcquireEnv base;
            base.acquire = true; base.haveTraceRec = true; base.hotkey = true;
            base.frames = 4;
            check(V::acquireGate(base) == V::AcquireGate::Valid &&
                      V::acquireActive(base),
                  "R16-1: the exact authorized vector is the ONLY valid one");

            V::AcquireEnv off = base; off.acquire = false;
            check(V::acquireGate(off) == V::AcquireGate::Off &&
                      !V::acquireActive(off),
                  "R16-1: gate OFF is Off - not merely inactive-by-accident");

            struct { V::AcquireEnv e; V::AcquireGate want; const char* why; } bad[] = {
                {[&]{ auto x = base; x.haveTraceRec = false; return x; }(),
                 V::AcquireGate::NoTracePath,
                 "no fresh trace path"},
                {[&]{ auto x = base; x.hotkey = false; return x; }(),
                 V::AcquireGate::NoHotkey,
                 "no hotkey arm"},
                {[&]{ auto x = base; x.frames = 240; return x; }(),
                 V::AcquireGate::WrongFrames,
                 "frames != 4"},
                {[&]{ auto x = base; x.refresh = true; return x; }(),
                 V::AcquireGate::RefreshEnabled,
                 "SR_HLE_TEX_REFRESH enabled"},
                {[&]{ auto x = base; x.refreshFilters = true; return x; }(),
                 V::AcquireGate::RefreshFiltersSet,
                 "a refresh key/filter/from-present gate set"},
            };
            bool allRefused = true;
            for (const auto& b : bad)
                if (V::acquireGate(b.e) != b.want || V::acquireActive(b.e))
                    allRefused = false;
            check(allRefused,
                  "R16-1: every conflicting companion setting REFUSES with its "
                  "own named reason (no trace path / no hotkey / wrong frames / "
                  "refresh enabled / refresh filters) - never a silent choice");
        }

        // -- R16-7. FORMAT WIDTHS ARE THE XENOS ONES ------------------------
        // The director found format 24 misclassified as ONE byte. It is k_16 -
        // one 16-bit channel - so a k_16 footprint would have been half-sized,
        // comparing half of every row and calling the rest padding.
        {
            check(V::linearBytesPerPixel(24) == 2,
                  "R16-7: format 24 is k_16 and therefore TWO bytes per pixel, "
                  "not one");
            check(V::linearBytesPerPixel(6) == 4 &&
                      V::linearBytesPerPixel(2) == 4,
                  "R16-7: the 32-bit formats are four bytes");
            check(V::linearBytesPerPixel(18) == 0 &&
                      V::linearBytesPerPixel(49) == 0 &&
                      V::linearBytesPerPixel(20) == 0,
                  "R16-7: the BLOCK-compressed formats (DXT1, DXN, DXT4/5) "
                  "report 0 - bytes-per-pixel is the wrong question for them, "
                  "so they are refused rather than approximated");
            // and the footprint follows the corrected width
            V::Footprint f = V::realFootprint(24u, (64u << 16) | 8u, 256u);
            check(f.ok && f.rowBytes == 128u,
                  "R16-7: a 64-wide k_16 row is 128 real bytes, so a 256-byte "
                  "pitch leaves 128 of padding - under the old value it would "
                  "have compared only 64 and called 192 bytes padding");
        }

        // -- R16-2. THE REAL FOOTPRINT, and PADDING-ONLY DOES NOT QUALIFY -----
        {
            const uint32_t dims = (64u << 16) | 64u;
            V::Footprint f = V::realFootprint(2u, dims, 320u);   // 64 B padding
            check(f.ok && f.rowBytes == 256u && f.rows == 64u &&
                      f.realBytes == 256ull * 64 && f.spanBytes == 320ull * 64,
                  "R16-2: the real footprint is width*bpp per row, and the span "
                  "is pitch*rows - the difference is padding");

            std::vector<uint8_t> a(f.spanBytes, 0x11), b = a;
            // change ONLY padding on every row
            for (uint32_t r = 0; r < f.rows; r++)
                for (uint32_t i = f.rowBytes; i < f.pitch; i++)
                    b[(size_t)r * f.pitch + i] = 0x22;
            V::Diff d = V::diffFootprint(a.data(), b.data(), f);
            check(d.changedBytes == 0 && d.anyPaddingChange,
                  "R16-2: a padding-only change counts ZERO real changed bytes "
                  "and is flagged as padding");
            check(V::qualifyPair(mkEpoch(1), mkEpoch(1), {}, {}, f, d) ==
                      BankVerdict::PaddingOnly,
                  "R16-2: and it is refused as PaddingOnly, distinguishable "
                  "from SameContent rather than both reading as 'nothing'");

            // one real byte DOES qualify, and its location is reported
            std::vector<uint8_t> c = a;
            c[(size_t)3 * f.pitch + 5] = 0x99;
            V::Diff d2 = V::diffFootprint(a.data(), c.data(), f);
            check(d2.changedBytes == 1 && d2.firstRow == 3 &&
                      d2.firstOffset == 3ull * f.pitch + 5 &&
                      !d2.anyPaddingChange,
                  "R16-2: a single real-footprint byte is counted AND located");
            check(V::qualifyPair(mkEpoch(1), mkEpoch(1), {}, {}, f, d2) ==
                      BankVerdict::Qualified,
                  "R16-2: and it qualifies");

            // nonzero -> nonzero is explicitly valid
            std::vector<uint8_t> nz(f.spanBytes, 0x41), nz2(f.spanBytes, 0x42);
            V::Diff d3 = V::diffFootprint(nz.data(), nz2.data(), f);
            check(V::qualifyPair(mkEpoch(1), mkEpoch(1), {}, {}, f, d3) ==
                      BankVerdict::Qualified,
                  "R16-2: nonzero->nonzero qualifies (the transition need not "
                  "start all-zero)");

            // identical content is SameContent, not Qualified
            V::Diff d4 = V::diffFootprint(a.data(), a.data(), f);
            check(V::qualifyPair(mkEpoch(1), mkEpoch(1), {}, {}, f, d4) ==
                      BankVerdict::SameContent,
                  "R16-2: identical content is SameContent");
        }

        // -- R16-3. TILED AND UNKNOWN FORMATS ARE REFUSED, NOT APPROXIMATED ---
        {
            const uint32_t dims = (64u << 16) | 64u;
            check(!V::realFootprint(2u | (1u << 8), dims, 256u).ok,
                  "R16-3: a TILED surface is refused - its real bytes are not a "
                  "row prefix, so a padding change could pass as content");
            check(!V::realFootprint(99u, dims, 256u).ok,
                  "R16-3: an unknown format is refused rather than assumed");
            check(!V::realFootprint(2u, dims, 4u).ok,
                  "R16-3: a pitch narrower than one row is refused");
            V::Footprint bad;   // ok == false
            check(V::qualifyPair(mkEpoch(1), mkEpoch(1), {}, {}, bad, {}) ==
                      BankVerdict::UnsupportedLayout,
                  "R16-3: an uncomputable layout disqualifies as "
                  "UnsupportedLayout");
        }

        // -- R16-4. ANY EPOCH FIELD CHANGE SPLITS THE PAIR --------------------
        {
            V::Footprint f = V::realFootprint(2u, (64u << 16) | 64u, 256u);
            std::vector<uint8_t> a(f.spanBytes, 1), b(f.spanBytes, 2);
            V::Diff d = V::diffFootprint(a.data(), b.data(), f);
            check(d.changedBytes > 0, "R16-4: the pair really does differ");

            V::EpochId e0 = mkEpoch(0x0B5BE000u);
            struct { const char* what; V::EpochId e1; } muts[] = {
                {"headerIdent", [&]{ auto x = e0; x.headerIdent ^= 1u; return x; }()},
                {"fmtBits",     [&]{ auto x = e0; x.fmtBits = 6u; return x; }()},
                {"dims",        [&]{ auto x = e0; x.dims = (32u<<16)|32u; return x; }()},
                {"pitch",       [&]{ auto x = e0; x.pitch = 512u; return x; }()},
                {"dimLevel",    [&]{ auto x = e0; x.dimLevel = 1u; return x; }()},
                {"aliasLo",     [&]{ auto x = e0; x.aliasLo ^= 0x1000u; return x; }()},
                {"aliasHi",     [&]{ auto x = e0; x.aliasHi ^= 0x1000u; return x; }()},
                {"allocGen",    [&]{ auto x = e0; x.allocGeneration++; return x; }()},
                {"descEpoch",   [&]{ auto x = e0; x.descEpoch++; return x; }()},
                {"key",         [&]{ auto x = e0; x.key ^= 0x1000u; return x; }()},
            };
            bool allSplit = true;
            for (const auto& m : muts)
                if (V::qualifyPair(e0, m.e1, {}, {}, f, d) !=
                    BankVerdict::EpochChanged)
                    allSplit = false;
            check(allSplit,
                  "R16-4: changing ANY ONE of key, headerIdent, fmtBits, dims, "
                  "pitch, dimLevel, aliasLo, aliasHi, allocGeneration or "
                  "descEpoch splits the epoch - all ten, not a chosen subset");
            check(V::qualifyPair(e0, e0, {}, {}, f, d) == BankVerdict::Qualified,
                  "R16-4: and the unmutated pair still qualifies");
        }

        // -- R16-5. AN INTERVENING PUBLICATION DISQUALIFIES ------------------
        // The director's correction: a snapshot of CURRENT authority cannot see
        // an Unlock that happened and was later superseded. Monotonic serials
        // can, and these prove both are enforced independently.
        {
            V::Footprint f = V::realFootprint(2u, (64u << 16) | 64u, 256u);
            std::vector<uint8_t> a(f.spanBytes, 1), b(f.spanBytes, 2);
            V::Diff d = V::diffFootprint(a.data(), b.data(), f);
            V::EpochId e = mkEpoch(0x0B5BE000u);
            V::Serials s0{5, 9};

            check(V::qualifyPair(e, e, s0, {5, 9}, f, d) ==
                      BankVerdict::Qualified,
                  "R16-5: unchanged serials qualify");
            check(V::qualifyPair(e, e, s0, {6, 9}, f, d) ==
                      BankVerdict::UnlockIntervened,
                  "R16-5: an Unlock publication between v0 and v1 disqualifies "
                  "- even though the CURRENT authority may have been superseded "
                  "back, which a snapshot would have missed");
            check(V::qualifyPair(e, e, s0, {5, 10}, f, d) ==
                      BankVerdict::ResolveIntervened,
                  "R16-5: a successful Resolve ownership between v0 and v1 "
                  "disqualifies, independently of the Unlock serial");
        }

        // -- R16-6. CAPACITY: OVERFLOW INVALIDATES A NEGATIVE, NOT A POSITIVE -
        {
            V::BankLimits lim;
            lim.maxRecords = 2;
            lim.maxBanked = 64;
            V::VersionBank bank(lim);
            check(bank.negativeResultIsValid(),
                  "R16-6: a fresh bank can support a negative result");

            auto mkRec = [&](size_t bytes) {
                V::VersionBank::Record r;
                r.epoch = mkEpoch(0x0B5BE000u);
                r.v1.assign(bytes, 0xAB);
                return r;
            };
            check(bank.bank(mkRec(32)) && bank.records().size() == 1,
                  "R16-6: the first record banks");
            check(bank.bank(mkRec(32)) && bank.records().size() == 2,
                  "R16-6: the second fills the byte budget exactly");
            check(!bank.bank(mkRec(1)) &&
                      bank.count(BankVerdict::CapacityExceeded) == 1,
                  "R16-6: the third is REFUSED and counted as CapacityExceeded "
                  "- never silently dropped");
            check(!bank.negativeResultIsValid(),
                  "R16-6: and the run can no longer support 'none found'");
            check(bank.records().size() == 2 &&
                      bank.count(BankVerdict::Qualified) == 2,
                  "R16-6: while the already-banked positives survive intact");

            V::VersionBank b2;
            b2.noteExamined(b2.limits().maxExamined);
            check(!b2.examinedExhausted() && b2.negativeResultIsValid(),
                  "R16-6: examining exactly the budget is not an overflow");
            b2.noteExamined(1);
            check(b2.examinedExhausted() && !b2.negativeResultIsValid(),
                  "R16-6: one byte past it is, and it invalidates a negative");
        }
    }

    // == iter-456p R17 - THE SIDECAR ROUND TRIP ========================
    // The offline reader has 22 refusal tests, but they all parse files the
    // TEST built. This writes with PRODUCTION writeSidecar() and reads the bytes
    // back, so the two halves are bound to each other rather than to a shared
    // assumption. tools/texv_read.py then parses the same file.
    {
        namespace V = sr::gfx::hle;

        auto mkRec = [](uint32_t key, const std::vector<uint8_t>& v1,
                        uint64_t v0h) {
            V::VersionBank::Record r;
            r.epoch.key = key;
            r.epoch.headerIdent = 0xA1B2C3D4u;
            r.epoch.fmtBits = 2u;
            r.epoch.dims = (64u << 16) | 64u;
            r.epoch.pitch = 256u;
            r.epoch.dimLevel = 0;
            r.epoch.aliasLo = key;
            r.epoch.aliasHi = key + 0x10000u;
            r.epoch.allocGeneration = 7;
            r.epoch.descEpoch = 1;
            for (int i = 0; i < 6; i++) r.rawFetch[i] = 0xF0000000u | i;
            r.s0 = {5, 9};
            r.s1 = {5, 9};
            r.v0Hash = v0h;
            r.v1Hash = sr::gfx::ShaderCache::hashUcode(v1.data(), v1.size());
            r.v0NonZero = 11; r.v1NonZero = 22;
            r.spanBytes = 256ull * 64; r.realBytes = 256ull * 64;
            r.changedBytes = 5; r.firstChangedOffset = 17; r.firstChangedRow = 0;
            r.seedPresent = 1; r.observePresent = 2;
            r.sampler = 3; r.bindOrdinal = 4;
            r.v1 = v1;
            return r;
        };

        const char* path = "archive/rec_r17_sidecar.texv";
        std::vector<uint8_t> p1(64, 0xAB), p2(96, 0xCD);
        V::VersionBank bank;
        bank.bank(mkRec(0x0B5BE000u, p1, 0x1111111111111111ull));
        bank.bank(mkRec(0x0BF36000u, p2, 0x2222222222222222ull));
        V::TraceTotals tt; tt.records = 80619; tt.payloadBytes = 173604464;
        tt.presents = 4;
        check(V::writeSidecar(path, bank, tt, 0xABCD1234ABCD1234ull),
              "R17-1: production writeSidecar() reports success");

        // read the bytes back with the SAME on-disk structs
        FILE* f = std::fopen(path, "rb");
        V::SidecarHeader h{};
        bool readOk = f && std::fread(&h, sizeof(h), 1, f) == 1;
        check(readOk && h.magic[0] == 'T' && h.magic[1] == 'E' &&
                  h.magic[2] == 'X' && h.magic[3] == 'V' &&
                  h.version == V::kSidecarVersion,
              "R17-1: the file opens TEXV v1");
        check(readOk && h.records == 2 && h.payloadBytes == p1.size() + p2.size(),
              "R17-2: the header totals equal what was banked");
        check(readOk && h.srhtRecords == 80619 &&
                  h.srhtPayloadBytes == 173604464 && h.srhtPresents == 4 &&
                  h.nonce == 0xABCD1234ABCD1234ull,
              "R17-2: the sidecar records the TRACE totals it belongs to, plus "
              "the run nonce - this is what makes a trace mismatch detectable "
              "without SHA-256 in the engine");
        check(readOk && h.flags == 0,
              "R17-2: a run that hit no capacity limit is not flagged overflowed");

        V::SidecarRecord r1{}, r2{};
        std::vector<uint8_t> back1(p1.size()), back2(p2.size());
        bool bodyOk = readOk &&
            std::fread(&r1, sizeof(r1), 1, f) == 1 &&
            std::fread(back1.data(), 1, back1.size(), f) == back1.size() &&
            std::fread(&r2, sizeof(r2), 1, f) == 1 &&
            std::fread(back2.data(), 1, back2.size(), f) == back2.size();
        long endPos = f ? std::ftell(f) : 0;
        if (f) { std::fseek(f, 0, SEEK_END); }
        bool cleanEof = f && endPos == std::ftell(f);
        if (f) std::fclose(f);

        check(bodyOk && cleanEof,
              "R17-3: both records and both payloads read back, then EOF "
              "exactly - no trailing bytes");
        check(bodyOk && back1 == p1 && back2 == p2,
              "R17-3: the banked v1 BYTES survive the round trip verbatim");
        check(bodyOk && r1.key == 0x0B5BE000u && r2.key == 0x0BF36000u &&
                  r1.payloadBytes == p1.size() && r2.payloadBytes == p2.size(),
              "R17-3: keys and payload lengths survive, in bank order");
        check(bodyOk && r1.v0Hash == 0x1111111111111111ull &&
                  r1.v1Hash == sr::gfx::ShaderCache::hashUcode(p1.data(), p1.size()) &&
                  r1.payloadHash == r1.v1Hash,
              "R17-4: v0Hash, v1Hash and payloadHash are written as the reader "
              "requires (v1Hash == payloadHash, v0 != v1)");
        check(bodyOk && r1.s0Unlock == 5 && r1.s0Resolve == 9 &&
                  r1.s1Unlock == 5 && r1.s1Resolve == 9,
              "R17-4: the publication serials at BOTH v0 and v1 are recorded - "
              "the reader refuses the record if they differ");
        check(bodyOk && r1.rawFetch[0] == 0xF0000000u && r1.rawFetch[5] == 0xF0000005u,
              "R17-4: the RAW fetch constant is carried, so a reader can "
              "re-derive the decode instead of trusting ours");
        check(bodyOk && r1.aliasLo == 0x0B5BE000u &&
                  r1.aliasHi == 0x0B5BE000u + 0x10000u &&
                  r1.allocGeneration == 7 && r1.descEpoch == 1 &&
                  r1.dimLevel == 0 && r1.firstChangedOffset == 17 &&
                  r1.firstChangedRow == 0 && r1.changedBytes == 5,
              "R17-4: the alias window, generation, descriptor epoch and the "
              "changed-byte count AND location all survive");

        // -- R17-5. an overflowed bank flags the FILE ------------------------
        {
            V::BankLimits lim; lim.maxRecords = 1;
            V::VersionBank small(lim);
            small.bank(mkRec(0x0B5BE000u, p1, 0x33ull));
            small.bank(mkRec(0x0BF36000u, p2, 0x44ull));   // refused
            check(small.overflowed() && !small.negativeResultIsValid(),
                  "R17-5: the bank knows it overflowed");
            const char* op = "archive/rec_r17_overflow.texv";
            check(V::writeSidecar(op, small, tt, 1),
                  "R17-5: the overflowed bank still writes its positives");
            FILE* g = std::fopen(op, "rb");
            V::SidecarHeader oh{};
            bool ok2 = g && std::fread(&oh, sizeof(oh), 1, g) == 1;
            if (g) std::fclose(g);
            check(ok2 && (oh.flags & V::kSidecarOverflowed) && oh.records == 1,
                  "R17-5: and the FILE carries the overflow flag, so an offline "
                  "reader cannot mistake a capacity-truncated run for a "
                  "complete 'none found'");
        }
    }

    // == iter-456p R18 - THE DIMENSION-AWARE FETCH DECODER ==============
    // The old decoder read every texture fetch through the 2D size layout. On
    // the golden title frame that is wrong for 9,253 of 38,380 fetches (7,431
    // 3D + 1,822 cube), which decoded to extents like 6208x7696 pitch=64.
    // Layouts below are Xenia's xe_gpu_texture_fetch_t (xenos.h), read from the
    // reference rather than re-derived.
    {
        namespace TL = sr::kernel::texlayout;

        // Build a fetch constant with an explicit dimension.
        auto mk = [](TL::TexDim dim, uint32_t sizeWord, bool stacked = false,
                     uint32_t fmt = 24u, uint32_t pitch = 640u,
                     bool tiled = true, uint32_t endian = 1u) {
            std::array<uint32_t, 6> f{};
            f[0] = 2u | ((pitch >> 5) << 22) | (tiled ? (1u << 31) : 0u);
            f[1] = (fmt & 0x3Fu) | ((endian & 3u) << 6) |
                   (stacked ? (1u << 10) : 0u) | (0x0B5BEu << 12);
            f[2] = sizeWord;
            f[5] = ((uint32_t)dim) << 9;
            return f;
        };

        // -- R18-1. 2D is UNCHANGED, byte for byte ---------------------------
        // The director's Slice A.2 bar: correcting non-2D must not move any
        // existing 2D result.
        {
            auto f = mk(TL::TexDim::k2DOrStacked, (640u - 1u) | ((640u - 1u) << 13));
            TL::FetchDesc d = TL::decodeFetchConstant(f.data());
            check(d.valid && d.width == 640 && d.height == 640 && d.depth == 1 &&
                      d.fmt == 24 && d.pitchTexels == 640 && d.tiled &&
                      d.endian == 1 && d.basePhys == 0x0B5BE000u &&
                      d.dim == TL::TexDim::k2DOrStacked && !d.stacked,
                  "R18-1: a plain 2D fetch decodes exactly as before (640x640, "
                  "depth 1) and now also reports its dimension");
            check(d.isPlain2D(),
                  "R18-1: and it is the one shape the widened publish accepts");
        }

        // -- R18-2. 1D uses TWENTY-FOUR width bits ---------------------------
        {
            auto f = mk(TL::TexDim::k1D, 4096u - 1u);
            TL::FetchDesc d = TL::decodeFetchConstant(f.data());
            check(d.valid && d.width == 4096 && d.height == 1 && d.depth == 1,
                  "R18-2: a 1D fetch reads width from 24 bits, height and depth "
                  "are 1");
            // a width that overflows the 13-bit 2D field proves the 24-bit read
            auto f2 = mk(TL::TexDim::k1D, 100000u - 1u);
            TL::FetchDesc d2 = TL::decodeFetchConstant(f2.data());
            check(d2.valid && d2.width == 100000,
                  "R18-2: and a width of 100000 survives - impossible through "
                  "the 13-bit 2D field, so this proves the 1D layout is used");
            check(!d.isPlain2D() && !d2.isPlain2D(),
                  "R18-2: 1D is NOT plain 2D and the widened publish must "
                  "refuse it");
        }

        // -- R18-3. STACKED 2D carries a real depth --------------------------
        {
            auto flat = mk(TL::TexDim::k2DOrStacked,
                           (256u - 1u) | ((128u - 1u) << 13) | (5u << 26));
            TL::FetchDesc dn = TL::decodeFetchConstant(flat.data());
            check(dn.valid && dn.width == 256 && dn.height == 128 &&
                      dn.depth == 1 && !dn.stacked,
                  "R18-3: a NON-stacked 2D ignores stack_depth entirely - "
                  "Xenia's own comment says it is not meaningful there");

            auto st = mk(TL::TexDim::k2DOrStacked,
                         (256u - 1u) | ((128u - 1u) << 13) | (5u << 26),
                         /*stacked=*/true);
            TL::FetchDesc ds = TL::decodeFetchConstant(st.data());
            check(ds.valid && ds.width == 256 && ds.height == 128 &&
                      ds.depth == 6 && ds.stacked,
                  "R18-3: a STACKED 2D reads stack_depth (5 stored -> 6 slices)");
            check(!ds.isPlain2D(),
                  "R18-3: stacked is NOT plain 2D - the widened publish refuses "
                  "it even though its width and height are perfectly good");
        }

        // -- R18-4. 3D is 11 + 11 + 10 ---------------------------------------
        {
            auto f = mk(TL::TexDim::k3D,
                        (64u - 1u) | ((32u - 1u) << 11) | ((16u - 1u) << 22));
            TL::FetchDesc d = TL::decodeFetchConstant(f.data());
            check(d.valid && d.width == 64 && d.height == 32 && d.depth == 16,
                  "R18-4: a 3D fetch reads 11+11+10 (64x32x16)");
            // through the OLD 2D layout this same word decodes to nonsense
            const uint32_t sizeWord =
                (64u - 1u) | ((32u - 1u) << 11) | ((16u - 1u) << 22);
            const uint32_t old2Dw = (sizeWord & 0x1FFFu) + 1u;
            const uint32_t old2Dh = ((sizeWord >> 13) & 0x1FFFu) + 1u;
            check(old2Dw != 64 || old2Dh != 32,
                  "R18-4: and the old 2D decode of that same word gives "
                  "different extents - this is the defect, demonstrated");
            check(!d.isPlain2D(),
                  "R18-4: 3D is refused by the widened publish");
        }

        // -- R18-5. CUBE is six faces ----------------------------------------
        {
            auto f = mk(TL::TexDim::kCube, (128u - 1u) | ((128u - 1u) << 13));
            TL::FetchDesc d = TL::decodeFetchConstant(f.data());
            check(d.valid && d.width == 128 && d.height == 128 && d.depth == 6,
                  "R18-5: a cube reads 13+13 extents and depth is ALWAYS the 6 "
                  "faces, never the stored stack_depth");
            check(!d.isPlain2D(), "R18-5: cube is refused by the widened publish");
        }

        // -- R18-6. mip levels are carried -----------------------------------
        {
            auto f = mk(TL::TexDim::k2DOrStacked, (64u - 1u) | ((64u - 1u) << 13));
            f[4] = (3u << 2) | (9u << 6);          // mip_min 3, mip_max 9
            TL::FetchDesc d = TL::decodeFetchConstant(f.data());
            check(d.valid && d.mipMin == 3 && d.mipMax == 9,
                  "R18-6: mip_min_level and mip_max_level are decoded from "
                  "dword_4 bits 2-5 and 6-9");
        }

        // -- R18-7. a non-texture fetch is still refused ----------------------
        {
            auto f = mk(TL::TexDim::k2DOrStacked, (64u - 1u) | ((64u - 1u) << 13));
            f[0] = (f[0] & ~3u) | 3u;              // type 3 = vertex fetch
            check(!TL::decodeFetchConstant(f.data()).valid,
                  "R18-7: a VERTEX fetch constant is still refused outright");
        }
    }


    // ================= R19. THE VERTEX-BUFFER PUBLICATION (Step 3A) ==========
    // Every case the director enumerated before authorizing a roll: two
    // simultaneous streams, null/rebind, offset == size and offset > size,
    // unchanged-resource dedup, changed-content republish, both alias
    // outcomes, payload corruption refusal, and v5 compatibility.
    //
    // These are pure-function tests by construction (gfx/vb_publish.hpp holds
    // no state and reads no guest memory), which is the point: the capture
    // policy is provable without a running game, so the ONE authorized K=1
    // refresh is spent on evidence rather than on debugging.
    {
        namespace V = sr::gfx;

        // -- R19-1. the object header decode, against the PPC we execute -----
        // sub_820F3ED8 writes (sizeBytes & 0x03FFFFFC) | flags | 2 at +0x1C.
        {
            // 4096 bytes, endian mode 2, with a high flag bit set that must NOT
            // leak into the size.
            const uint32_t rawSize = (4096u & 0x03FFFFFCu) | 0x80000000u | 2u;
            auto d = V::decodeVbHeader(0x8EA08C90u, rawSize);
            check(d.valid && d.dataSize == 4096u && d.endian == 2u,
                  "R19-1: dataSize masks with 0x03FFFFFC and endian is the low "
                  "2 bits, so creation flags cannot inflate the size");
        }

        // -- R19-2. the alias window is stripped from the key ----------------
        // The 0x8 and 0xA windows are the SAME memory. A key that kept the
        // window would file one buffer under two identities and republish it
        // forever.
        {
            auto a = V::decodeVbHeader(0x8EA08C90u, 256u | 1u);
            auto b = V::decodeVbHeader(0xAEA08C90u, 256u | 1u);
            check(a.physBase == b.physBase && a.physBase == 0x0EA08C90u,
                  "R19-2: both alias windows normalize to ONE physBase");
        }

        // -- R19-3. a zero base or zero size is not a valid header -----------
        {
            check(!V::decodeVbHeader(0u, 256u).valid &&
                  !V::decodeVbHeader(0x8EA08C90u, 0u).valid,
                  "R19-3: a zero base or zero size decodes INVALID rather than "
                  "publishing an empty resource at address zero");
        }

        // -- R19-4. TWO SIMULTANEOUS STREAMS, independently bound ------------
        {
            V::VbBinding b[V::kVbMaxStreams];
            b[0] = V::VbBinding{0xA857FFA0u, 0u, 36u, true};
            b[1] = V::VbBinding{0x921DD1C0u, 0u, 4u, true};
            auto d0 = V::decodeVbHeader(0x0857FFA0u, 1728u | 1u);
            auto d1 = V::decodeVbHeader(0x121DD1C0u, 192u | 1u);
            check(V::vbCheckBinding(0, b[0], d0, 0) == V::VbRefuse::None &&
                  V::vbCheckBinding(1, b[1], d1, 0) == V::VbRefuse::None &&
                  V::vbViewSize(b[0], d0) == 1728u &&
                  V::vbViewSize(b[1], d1) == 192u,
                  "R19-4: two simultaneously-bound streams each resolve to "
                  "their own view size");
        }

        // -- R19-5. a NULL bind unbinds, and does not keep the old buffer ----
        // A stale binding here would publish the PREVIOUS buffer's bytes for a
        // stream the guest has explicitly released.
        {
            V::VbBinding live{0xA857FFA0u, 0u, 36u, true};
            V::VbBinding null{0u, 0u, 0u, false};
            auto d = V::decodeVbHeader(0x0857FFA0u, 1728u | 1u);
            check(V::vbCheckBinding(0, live, d, 0) == V::VbRefuse::None &&
                  V::vbCheckBinding(0, null, d, 0) == V::VbRefuse::NotLive,
                  "R19-5: a null bind refuses as notLive rather than serving "
                  "the previously-bound buffer");
        }

        // -- R19-6. REBIND replaces, and the new size governs ----------------
        {
            V::VbBinding b{0xA857FFA0u, 0u, 36u, true};
            auto dOld = V::decodeVbHeader(0x0857FFA0u, 1728u | 1u);
            b = V::VbBinding{0x921DD1C0u, 64u, 4u, true};
            auto dNew = V::decodeVbHeader(0x121DD1C0u, 192u | 1u);
            check(V::vbViewSize(b, dNew) == 128u && dOld.dataSize == 1728u,
                  "R19-6: a rebind uses the NEW object's size and offset");
        }

        // -- R19-7. offset == size is LEGAL and yields an empty view ---------
        // The reference's `dataSize - offset` is zero here. A zero-size view is
        // a real state the guest can bind; refusing it would invent a rule.
        {
            V::VbBinding b{0xA857FFA0u, 256u, 4u, true};
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            check(V::vbCheckBinding(0, b, d, 0) == V::VbRefuse::None &&
                  V::vbViewSize(b, d) == 0u,
                  "R19-7: offset == dataSize is ACCEPTED and gives a zero-size "
                  "view, not a refusal");
        }

        // -- R19-8. offset > size is refused BY NAME ------------------------
        {
            V::VbBinding b{0xA857FFA0u, 260u, 4u, true};
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            check(V::vbCheckBinding(0, b, d, 0) == V::VbRefuse::OffsetBeyondSize,
                  "R19-8: offset > dataSize refuses as offsetBeyondSize (a "
                  "wrapped subtraction would produce a ~4 GB view)");
        }

        // -- R19-9. a stream index outside 0..15 is refused ------------------
        {
            V::VbBinding b{0xA857FFA0u, 0u, 36u, true};
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            check(V::vbCheckBinding(16, b, d, 0) == V::VbRefuse::StreamOutOfRange,
                  "R19-9: a stream index outside 0..15 refuses by name");
        }

        // -- R19-10. the size cap refuses by name, not silently -------------
        {
            V::VbBinding b{0xA857FFA0u, 0u, 36u, true};
            auto d = V::decodeVbHeader(0x0857FFA0u, (1u << 24) | 1u);
            check(V::vbCheckBinding(0, b, d, 1u << 20) == V::VbRefuse::SizeTooLarge,
                  "R19-10: a buffer beyond the residency cap refuses as "
                  "sizeTooLarge so coverage degrades visibly");
        }

        // -- R19-11. BOTH ALIAS OUTCOMES -----------------------------------
        // The vbc3 lesson: a head-only probe chose the zero window for every
        // 0xA-written buffer and blacked out an era. Both directions tested.
        {
            uint8_t zeros[64] = {};
            uint8_t data[64];
            for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i + 1);

            auto c1 = V::vbChooseAlias(data, zeros, 64);
            check(c1.alias == V::VbAlias::Primary && c1.bytes == data &&
                  c1.primaryHadContent && !c1.altHadContent,
                  "R19-11a: content in the PRIMARY window selects primary");

            auto c2 = V::vbChooseAlias(zeros, data, 64);
            check(c2.alias == V::VbAlias::Alt && c2.bytes == data &&
                  !c2.primaryHadContent && c2.altHadContent,
                  "R19-11b: content ONLY in the alt window selects alt — the "
                  "exact case a head-only probe got wrong");
        }

        // -- R19-12. a genuinely all-zero buffer publishes, and says so ------
        // Not an error to paper over: all-zero may be what the guest wrote.
        {
            uint8_t z1[32] = {}, z2[32] = {};
            auto c = V::vbChooseAlias(z1, z2, 32);
            check(c.refuse == V::VbRefuse::None && c.bytes == z1 &&
                  !c.primaryHadContent && !c.altHadContent,
                  "R19-12: an all-zero buffer is published from the primary "
                  "with both content flags FALSE, so the reader can tell a "
                  "real zero buffer from a bad alias choice");
        }

        // -- R19-13. neither window readable refuses ------------------------
        {
            auto c = V::vbChooseAlias(nullptr, nullptr, 32);
            check(c.refuse == V::VbRefuse::Unreadable && c.bytes == nullptr,
                  "R19-13: with neither window mapped the choice REFUSES "
                  "rather than publishing from a null pointer");
        }

        // -- R19-14. only ONE window mapped still works --------------------
        {
            uint8_t data[16];
            for (int i = 0; i < 16; i++) data[i] = (uint8_t)(0xA0 + i);
            auto c = V::vbChooseAlias(nullptr, data, 16);
            check(c.refuse == V::VbRefuse::None && c.alias == V::VbAlias::Alt &&
                  c.bytes == data,
                  "R19-14: an unmapped primary falls to the alt window");
        }

        // -- R19-15. UNCHANGED-RESOURCE DEDUP ------------------------------
        {
            V::VbResourceState st;
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            uint8_t bytes[64];
            for (int i = 0; i < 64; i++) bytes[i] = (uint8_t)i;
            const uint64_t dig = V::vbDigest(bytes, 64);
            const auto a1 = V::vbDecidePublish(st, d, dig);
            const auto a2 = V::vbDecidePublish(st, d, dig);
            const auto a3 = V::vbDecidePublish(st, d, dig);
            check(a1 == V::VbPublishAction::FirstSight &&
                  a2 == V::VbPublishAction::Unchanged &&
                  a3 == V::VbPublishAction::Unchanged && st.generation == 1u,
                  "R19-15: identical content publishes ONCE and thereafter "
                  "binds only — the generation does not drift");
        }

        // -- R19-16. CHANGED-CONTENT REPUBLISH bumps the generation ---------
        {
            V::VbResourceState st;
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            uint8_t b1[32] = {}, b2[32] = {};
            b2[17] = 0xFF;                       // one byte differs
            const auto a1 = V::vbDecidePublish(st, d, V::vbDigest(b1, 32));
            const auto a2 = V::vbDecidePublish(st, d, V::vbDigest(b2, 32));
            check(a1 == V::VbPublishAction::FirstSight &&
                  a2 == V::VbPublishAction::ContentChanged &&
                  st.generation == 2u,
                  "R19-16: a ONE-BYTE content change republishes and bumps the "
                  "generation, so a stale consumer cannot be handed different "
                  "bytes under the same name");
        }

        // -- R19-17. a reused address with a DIFFERENT SIZE is a new resource
        // Treating it as the same one would publish the new size's bytes under
        // the old size's identity.
        {
            V::VbResourceState st;
            auto dA = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            auto dB = V::decodeVbHeader(0x0857FFA0u, 512u | 1u);
            uint8_t bytes[32] = {};
            const uint64_t dig = V::vbDigest(bytes, 32);
            V::vbDecidePublish(st, dA, dig);
            const auto a2 = V::vbDecidePublish(st, dB, dig);
            check(a2 == V::VbPublishAction::FirstSight && st.dataSize == 512u &&
                  st.generation == 1u,
                  "R19-17: the same base at a DIFFERENT SIZE is a new resource, "
                  "not an unchanged one, even when the digest matches");
        }

        // -- R19-18. a different ENDIAN mode is also a new resource ---------
        {
            V::VbResourceState st;
            auto dA = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            auto dB = V::decodeVbHeader(0x0857FFA0u, 256u | 2u);
            uint8_t bytes[32] = {};
            const uint64_t dig = V::vbDigest(bytes, 32);
            V::vbDecidePublish(st, dA, dig);
            check(V::vbDecidePublish(st, dB, dig) == V::VbPublishAction::FirstSight,
                  "R19-18: a changed endian mode is a new resource — the same "
                  "bytes mean something different under a different swap");
        }

        // -- R19-19. PAYLOAD CORRUPTION is detected by the digest ------------
        // The change detector must actually detect. A digest that ignored the
        // tail (or the head) would pass corruption through as `Unchanged` and
        // the republish path would never fire.
        {
            uint8_t good[256];
            for (int i = 0; i < 256; i++) good[i] = (uint8_t)i;
            const uint64_t dg = V::vbDigest(good, 256);
            bool allDetected = true;
            for (int pos = 0; pos < 256; pos++) {
                uint8_t bad[256];
                std::memcpy(bad, good, 256);
                bad[pos] ^= 0xFF;
                if (V::vbDigest(bad, 256) == dg) { allDetected = false; break; }
            }
            check(allDetected,
                  "R19-19: flipping ANY ONE of the 256 bytes changes the "
                  "digest — corruption cannot replay as 'unchanged'");
        }

        // -- R19-20. the digest covers LENGTH, not just content -------------
        {
            uint8_t z[64] = {};
            check(V::vbDigest(z, 32) != V::vbDigest(z, 64),
                  "R19-20: a truncated payload digests differently, so a short "
                  "read cannot pass as the whole buffer");
        }

        // -- R19-21. the packed endian/alias/generation dword round-trips ----
        {
            bool ok = true;
            for (uint32_t e = 0; e < 4 && ok; e++)
                for (uint32_t a = 0; a < 2 && ok; a++)
                    for (uint32_t g : {0u, 1u, 2u, 255u, 4096u, 65535u}) {
                        const uint32_t p =
                            V::vbPackEndAliasGen(e, (V::VbAlias)a, g);
                        if (V::vbUnpackEndian(p) != e ||
                            (uint32_t)V::vbUnpackAlias(p) != a ||
                            V::vbUnpackGeneration(p) != g) { ok = false; break; }
                    }
            check(ok,
                  "R19-21: every (endian, alias, generation) triple round-trips "
                  "through the packed dword — the alias is RECORDED, never "
                  "re-derived");
        }

        // -- R19-22. the three fields occupy disjoint bits -------------------
        {
            const uint32_t p = V::vbPackEndAliasGen(3u, V::VbAlias::Alt, 0xABCDu);
            check(V::vbUnpackEndian(p) == 3u &&
                  V::vbUnpackAlias(p) == V::VbAlias::Alt &&
                  V::vbUnpackGeneration(p) == 0xABCDu && p == 0xABCD0103u,
                  "R19-22: endian, alias and generation occupy disjoint bits");
        }

        // -- R19-22b. THE GENERATION SATURATES, it does not wrap -------------
        // A wrap at 65536 would make a CHANGED buffer read as unchanged under
        // an old generation — the exact failure the field exists to prevent.
        {
            const uint32_t at = V::vbPackEndAliasGen(1u, V::VbAlias::Primary,
                                                     V::kVbMaxGeneration);
            const uint32_t over = V::vbPackEndAliasGen(1u, V::VbAlias::Primary,
                                                       V::kVbMaxGeneration + 1u);
            const uint32_t way = V::vbPackEndAliasGen(1u, V::VbAlias::Primary,
                                                      0x7FFFFFFFu);
            check(V::vbUnpackGeneration(at) == V::kVbMaxGeneration &&
                  V::vbUnpackGeneration(over) == V::kVbMaxGeneration &&
                  V::vbUnpackGeneration(way) == V::kVbMaxGeneration &&
                  V::vbUnpackEndian(over) == 1u,
                  "R19-22b: a generation past 16 bits SATURATES rather than "
                  "wrapping to 0, and does not corrupt the endian field");
        }

        // -- R19-23. every refusal has a distinct NAME ----------------------
        // A shared or empty name would collapse two different failures into one
        // line and make a census of them meaningless.
        {
            bool distinct = true;
            for (uint32_t i = 0; i < (uint32_t)V::VbRefuse::Count && distinct; i++)
                for (uint32_t j = i + 1; j < (uint32_t)V::VbRefuse::Count; j++)
                    if (std::strcmp(V::vbRefuseName((V::VbRefuse)i),
                                    V::vbRefuseName((V::VbRefuse)j)) == 0) {
                        distinct = false;
                        break;
                    }
            check(distinct, "R19-23: all refusal names are distinct");
        }

        // -- R19-25. the sparse probe matches an unchanged buffer -----------
        // The hot path is every draw; a whole-buffer digest there would make
        // the recording roll unusable. The probe is the same rule the VB
        // residency cache already measured.
        {
            uint8_t buf[4096];
            for (int i = 0; i < 4096; i++) buf[i] = (uint8_t)(i * 7);
            auto sg = V::vbCaptureSparseSig(buf, 4096);
            check(sg.len == V::kVbSigBytes &&
                  V::vbSparseSigMatches(sg, buf, 4096),
                  "R19-25: a captured sparse signature matches its own buffer");
        }

        // -- R19-26. the probe SEES edits in all three windows ---------------
        {
            uint8_t buf[4096];
            for (int i = 0; i < 4096; i++) buf[i] = (uint8_t)(i * 7);
            auto sg = V::vbCaptureSparseSig(buf, 4096);
            bool head = false, mid = false, tail = false;
            uint8_t t[4096];

            std::memcpy(t, buf, 4096); t[0] ^= 0xFF;
            head = !V::vbSparseSigMatches(sg, t, 4096);
            std::memcpy(t, buf, 4096); t[4096 / 2] ^= 0xFF;
            mid = !V::vbSparseSigMatches(sg, t, 4096);
            std::memcpy(t, buf, 4096); t[4095] ^= 0xFF;
            tail = !V::vbSparseSigMatches(sg, t, 4096);
            check(head && mid && tail,
                  "R19-26: an edit in the head, middle or tail window is SEEN");
        }

        // -- R19-27. THE RESIDUE, PROVEN RATHER THAN ASSUMED AWAY -----------
        // ⚠ This test asserts a LIMITATION, deliberately. An edit that touches
        // none of the three probed windows is MISSED and the buffer replays at
        // its previous generation. Calling the dedup exact would be the
        // overclaim; the exposure is counted at runtime instead.
        {
            uint8_t buf[4096];
            for (int i = 0; i < 4096; i++) buf[i] = (uint8_t)(i * 7);
            auto sg = V::vbCaptureSparseSig(buf, 4096);
            uint8_t t[4096];
            std::memcpy(t, buf, 4096);
            t[1000] ^= 0xFF;            // between head and mid windows
            check(V::vbSparseSigMatches(sg, t, 4096) &&
                  V::vbProbeMissRisk(4096),
                  "R19-27: an edit outside all three probe windows is MISSED - "
                  "a counted risk class, and vbProbeMissRisk says so");
        }

        // -- R19-28. small buffers get a FULL compare, so no residue ---------
        {
            uint8_t buf[64];
            for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
            auto sg = V::vbCaptureSparseSig(buf, 64);
            bool allSeen = true;
            for (int pos = 0; pos < 64; pos++) {
                uint8_t t[64];
                std::memcpy(t, buf, 64);
                t[pos] ^= 0xFF;
                if (V::vbSparseSigMatches(sg, t, 64)) { allSeen = false; break; }
            }
            check(allSeen && !V::vbProbeMissRisk(64),
                  "R19-28: at or under 96 bytes the compare is FULL - every "
                  "single-byte edit is seen and the risk flag is clear");
        }

        // -- R19-29. a size change never matches an old signature -----------
        {
            uint8_t buf[4096] = {};
            auto sg = V::vbCaptureSparseSig(buf, 4096);
            check(!V::vbSparseSigMatches(sg, buf, 64),
                  "R19-29: a signature captured at one size does not match a "
                  "different size, so a resized buffer cannot read unchanged");
        }

        // -- R19-30. the locked alias is part of the resource state ---------
        // Re-choosing the window per draw would cost a whole-buffer scan on the
        // hot path AND let the source flip mid-run, republishing forever.
        {
            V::VbResourceState st;
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            V::vbDecidePublish(st, d, 1234u);
            st.alias = V::VbAlias::Alt;
            uint8_t z[32] = {};
            st.sig = V::vbCaptureSparseSig(z, 32);
            check(st.alias == V::VbAlias::Alt && st.sig.len == 32u &&
                  st.generation == 1u,
                  "R19-30: the resource state carries the LOCKED alias and the "
                  "signature the next probe reads against");
        }

        // -- R19-31. A FAILED FIRST-SIGHT PUBLICATION RETRIES CLEANLY --------
        // ⛔ THE DEFECT THIS SUITE MISSED. The first cut mutated the resource
        // state at the DECISION and rolled back on a pool refusal - restoring
        // digest and generation but NOT the sparse signature. The next draw's
        // cheap probe then matched the uncommitted signature and the buffer
        // took the unchanged fast path forever. 238 buffers were poisoned that
        // way on the one measured roll. peek/commit removes the class.
        {
            V::VbResourceState st;
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            uint8_t bytes[64];
            for (int i = 0; i < 64; i++) bytes[i] = (uint8_t)i;
            const uint64_t dig = V::vbDigest(bytes, 64);

            // The publication is DECIDED but the pool refuses, so nothing is
            // committed.
            const auto a1 = V::vbPeekPublish(st, d, dig);
            // ... no commit ...
            const auto a2 = V::vbPeekPublish(st, d, dig);
            check(a1 == V::VbPublishAction::FirstSight &&
                  a2 == V::VbPublishAction::FirstSight && !st.known &&
                  st.sig.len == 0u,
                  "R19-31: a first-sight publication that FAILED to park its "
                  "payload peeks FirstSight again - no signature, digest or "
                  "generation was installed");
        }

        // -- R19-32. A FAILED CHANGED-CONTENT PUBLICATION RETRIES CLEANLY ----
        // The path that actually fired 238 times.
        {
            V::VbResourceState st;
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            uint8_t v1[64] = {}, v2[64] = {};
            v2[9] = 0x5A;
            const uint64_t d1 = V::vbDigest(v1, 64), d2 = V::vbDigest(v2, 64);

            // v1 publishes successfully.
            V::vbCommitPublish(st, d, d1, V::VbAlias::Primary,
                               V::vbCaptureSparseSig(v1, 64));
            check(st.generation == 1u, "R19-32a: the first publication commits");

            // v2 is decided, then the pool refuses - nothing committed.
            const auto a1 = V::vbPeekPublish(st, d, d2);
            const auto a2 = V::vbPeekPublish(st, d, d2);
            check(a1 == V::VbPublishAction::ContentChanged &&
                  a2 == V::VbPublishAction::ContentChanged &&
                  st.generation == 1u && st.digest == d1 &&
                  V::vbSparseSigMatches(st.sig, v1, 64),
                  "R19-32b: a FAILED changed-content publication leaves the "
                  "PREVIOUS generation intact and peeks ContentChanged again - "
                  "the retry is not poisoned");

            // ⚠ THE KILLER. Under the old partial rollback the signature would
            // now match v2, so the cheap probe would say unchanged and the
            // buffer would never publish again. Assert it does NOT.
            check(!V::vbSparseSigMatches(st.sig, v2, 64),
                  "R19-32c: the uncommitted signature is NOT installed - the "
                  "cheap probe still SEES v2 as a change");

            // and the retry succeeds
            const uint32_t g = V::vbCommitPublish(
                st, d, d2, V::VbAlias::Primary, V::vbCaptureSparseSig(v2, 64));
            check(g == 2u && st.digest == d2 &&
                  V::vbPeekPublish(st, d, d2) == V::VbPublishAction::Unchanged,
                  "R19-32d: the retry commits at generation 2 and then dedups");
        }

        // -- R19-33. commit moves EVERY field together ----------------------
        // A commit that updated the digest but not the alias would leave later
        // probes reading the wrong window.
        {
            V::VbResourceState st;
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 2u);
            uint8_t b[64] = {};
            b[3] = 1;
            const uint64_t dg = V::vbDigest(b, 64);
            const uint32_t g = V::vbCommitPublish(st, d, dg, V::VbAlias::Alt,
                                                  V::vbCaptureSparseSig(b, 64));
            check(g == 1u && st.known && st.physBase == d.physBase &&
                  st.dataSize == 256u && st.endian == 2u && st.digest == dg &&
                  st.alias == V::VbAlias::Alt &&
                  V::vbSparseSigMatches(st.sig, b, 64),
                  "R19-33: a commit installs base, size, endian, digest, alias "
                  "and signature together");
        }

        // -- R19-34. peek NEVER mutates -------------------------------------
        // Mutation on the read path is what created the whole defect class.
        {
            V::VbResourceState st;
            auto d = V::decodeVbHeader(0x0857FFA0u, 256u | 1u);
            uint8_t b[32] = {};
            V::vbCommitPublish(st, d, 111u, V::VbAlias::Primary,
                               V::vbCaptureSparseSig(b, 32));
            const V::VbResourceState before = st;
            for (int i = 0; i < 5; i++) {
                V::vbPeekPublish(st, d, 111u);
                V::vbPeekPublish(st, d, 222u);
                auto other = V::decodeVbHeader(0x09999000u, 512u | 3u);
                V::vbPeekPublish(st, other, 333u);
            }
            check(st.known == before.known && st.digest == before.digest &&
                  st.generation == before.generation &&
                  st.alias == before.alias && st.sig.len == before.sig.len &&
                  st.physBase == before.physBase &&
                  st.dataSize == before.dataSize && st.endian == before.endian,
                  "R19-34: fifteen peeks across matching, changed and foreign "
                  "descriptors mutate NOTHING");
        }

        // -- R19-24. V5 COMPATIBILITY: streams unavailable, never faked ------
        // The director's hard line. A v5 trace carries SetStreamSource records
        // but no VertexBufferPublish, so its binds name buffers that were never
        // captured. The reader must report them UNAVAILABLE. This asserts the
        // version constants that encode that contract; the replay-side gate is
        // exercised by the v5 fixture replay itself.
        {
            // iter-457 H1a: the ceiling moved to 7 and THE FLOOR DID NOT.
            // A cube bump that raised the minimum would silently invalidate
            // every banked v5/v6 fixture, which is the opposite of the narrow
            // semantic gate the director ruled for.
            // Astra 086: the ceiling moved to 8 (ClearF colour payload) and
            // THE FLOOR STILL DID NOT.
            check(sr::gfx::hle::kTraceVersionForTest == 8u &&
                  sr::gfx::hle::kTraceVersionMinForTest == 5u,
                  "R19-24: the reader speaks v5..v8, so every banked v5 fixture "
                  "still replays — with streams UNAVAILABLE, never fabricated");
        }
    }


        // -- R20 (iter-457 gate 3): THE STRICT SUPPRESSION MANIFEST ----------
        // The director's H-S1 killer: "every empty, malformed, overlapping,
        // out-of-range and wrong-trace fixture suppresses zero and returns
        // nonzero." Each rejection case below therefore asserts BOTH the named
        // reason AND that the resulting manifest selects nothing -- a parser
        // that refused but left a populated selection would still fail OPEN.
        {
            using namespace sr::gfx::manifest;
            const char* kSha =
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
            auto head = [&](const char* sel) {
                return std::string("trace-sha256 ") + kSha +
                       "\nadmitted-draws 1000\n" + sel;
            };

            // R20-1. a singleton parses and selects exactly one ordinal
            {
                Manifest m; const Reject r = parse(head("select 42\n"), &m);
                check(r == Reject::None && m.requested == 1 && !m.selectAll &&
                      m.selects(42) && !m.selects(41) && !m.selects(43) &&
                      m.traceSha == kSha && m.admittedDraws == 1000,
                      "R20-1: a singleton manifest parses, pins the trace and "
                      "admitted count, and selects exactly its one ordinal");
            }

            // R20-2. disjoint ascending ranges; requested is their SUM
            {
                Manifest m;
                const Reject r =
                    parse(head("select 0-9\nselect 20-29\nselect 100\n"), &m);
                check(r == Reject::None && m.requested == 21 &&
                      m.selects(0) && m.selects(9) && m.selects(20) &&
                      m.selects(29) && m.selects(100) &&
                      !m.selects(10) && !m.selects(19) && !m.selects(30) &&
                      !m.selects(99) && !m.selects(101),
                      "R20-2: disjoint ascending ranges parse, requested is the "
                      "sum, and membership is exact at every boundary");
            }

            // R20-3. select all
            {
                Manifest m; const Reject r = parse(head("select all\n"), &m);
                check(r == Reject::None && m.selectAll && m.requested == 1000 &&
                      m.selects(0) && m.selects(999) && !m.selects(1000),
                      "R20-3: select-all covers every admitted ordinal and stops "
                      "at the pinned admitted count");
            }

            // R20-4..19. THE REJECTIONS. Each must also select NOTHING.
            {
                const std::string shaLine =
                    std::string("trace-sha256 ") + kSha + "\n";
                const std::string upperSha =
                    std::string("trace-sha256 ") +
                    "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef" +
                    "\nadmitted-draws 10\nselect 1\n";
                std::vector<std::pair<std::string, std::pair<Reject, const char*>>> bad;
                bad.push_back({head(""), {Reject::EmptySelection,
                    "R20-4: no selector at all is EMPTY, not a silent no-op"}});
                bad.push_back({head("selct 5\n"), {Reject::MalformedLine,
                    "R20-5: an unknown directive is malformed"}});
                bad.push_back({head("select 5 6\n"), {Reject::MalformedLine,
                    "R20-6: a selector with stray tokens is malformed"}});
                bad.push_back({std::string("admitted-draws 10\nselect 1\n"),
                    {Reject::MissingSha,
                    "R20-7: a manifest with no pinned trace sha is refused"}});
                bad.push_back({shaLine + "select 1\n", {Reject::MissingAdmitted,
                    "R20-8: a manifest with no admitted-draws is refused"}});
                bad.push_back({shaLine + shaLine + "admitted-draws 10\nselect 1\n",
                    {Reject::DuplicateDirective,
                    "R20-9: a duplicated directive is refused"}});
                bad.push_back({std::string("trace-sha256 abc\nadmitted-draws 10\nselect 1\n"),
                    {Reject::BadSha, "R20-10: a short sha is refused"}});
                bad.push_back({upperSha, {Reject::BadSha,
                    "R20-11: an uppercase sha is refused - one canonical form"}});
                bad.push_back({shaLine + "admitted-draws 0\nselect 1\n",
                    {Reject::BadAdmitted, "R20-12: admitted-draws 0 is refused"}});
                bad.push_back({head("select all\nselect 5\n"), {Reject::MixedAll,
                    "R20-13: all mixed with explicit selectors is refused"}});
                bad.push_back({head("select 9-3\n"), {Reject::BadRange,
                    "R20-14: an inverted range is refused"}});
                bad.push_back({head("select 20-29\nselect 0-9\n"), {Reject::Overlap,
                    "R20-15: descending ranges are refused"}});
                bad.push_back({head("select 0-9\nselect 5-14\n"), {Reject::Overlap,
                    "R20-16: overlapping ranges are refused"}});
                bad.push_back({head("select 0-9\nselect 9-14\n"), {Reject::Overlap,
                    "R20-17: ranges sharing an endpoint are refused"}});
                bad.push_back({head("select 995-1005\n"), {Reject::OutOfRange,
                    "R20-18: a range past admitted-draws is refused"}});
                bad.push_back({head("select 1000\n"), {Reject::OutOfRange,
                    "R20-19: an ordinal equal to admitted-draws is refused "
                    "because ordinals are zero-based"}});
                for (size_t bi = 0; bi < bad.size(); ++bi) {
                    Manifest m; const Reject r = parse(bad[bi].first, &m);
                    const bool selectsNothing =
                        !m.selectAll && m.ranges.empty() && m.requested == 0 &&
                        !m.selects(0) && !m.selects(1) && !m.selects(999);
                    check(r == bad[bi].second.first && selectsNothing,
                          bad[bi].second.second);
                }
            }

            // R20-20. comments, indentation and blank lines are legal
            {
                Manifest m;
                const std::string t = std::string("# leading comment\n\n") +
                    "trace-sha256 " + kSha + "   # pinned\n" +
                    "\nadmitted-draws 1000\n" +
                    "select 7   # the one we want\n";
                const Reject r = parse(t, &m);
                check(r == Reject::None && m.requested == 1 && m.selects(7),
                      "R20-20: comments and blank lines parse cleanly rather "
                      "than refusing a valid manifest");
            }

            // R20-21. the binary search holds over many ranges
            {
                std::string t = std::string("trace-sha256 ") + kSha +
                                "\nadmitted-draws 100000\n";
                for (int i = 0; i < 500; ++i)
                    t += "select " + std::to_string(i * 100) + "-" +
                         std::to_string(i * 100 + 9) + "\n";
                Manifest m; const Reject r = parse(t, &m);
                bool ok = (r == Reject::None) && m.requested == 5000;
                for (int i = 0; ok && i < 500; ++i) {
                    ok = m.selects((uint64_t)i * 100) &&
                         m.selects((uint64_t)i * 100 + 9) &&
                         !m.selects((uint64_t)i * 100 + 10);
                }
                check(ok, "R20-21: 500 disjoint ranges parse and every boundary "
                          "resolves correctly through the binary search");
            }

            // R20-22. the completion tally
            {
                const bool okEqual  = tallyConsistent(Tally{10, 10, 10, 6904});
                const bool badMatch = tallyConsistent(Tally{10,  9, 10, 6904});
                const bool badSupp  = tallyConsistent(Tally{10, 10,  9, 6904});
                const bool badTotal = tallyConsistent(Tally{10, 10, 10,    5});
                check(okEqual && !badMatch && !badSupp && !badTotal,
                      "R20-22: the tally accepts only requested==matched=="
                      "suppressed<=totalSubmitted, so any divergence between "
                      "requested and actual omission invalidates the replay");
            }

            // R20-23. an unreadable path refuses, and selects nothing
            {
                Manifest m;
                const Reject r =
                    parseFile("nonexistent/there/is/no/such.manifest", &m);
                check(r == Reject::Unreadable && m.requested == 0 &&
                      !m.selectAll && !m.selects(0),
                      "R20-23: an unreadable manifest path refuses by name and "
                      "leaves a manifest that selects nothing");
            }


            // R20-25. SHA-256 known-answer vectors (FIPS 180-4). Without this
            // the wrong-trace rule would rest on an unverified hash.
            {
                const bool v1 = sha256Bytes("") ==
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
                const bool v2 = sha256Bytes("abc") ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
                const bool v3 = sha256Bytes(std::string(1000000, 'a')) ==
                    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
                check(v1 && v2 && v3,
                      "R20-25: SHA-256 reproduces the FIPS 180-4 vectors for the "
                      "empty string, abc, and a million a - so the wrong-trace "
                      "rule rests on a verified hash");
            }
            // R20-24. every rejection reason has a name
            {
                const Reject all[] = {Reject::None, Reject::Unreadable,
                    Reject::MalformedLine, Reject::MissingSha,
                    Reject::MissingAdmitted, Reject::DuplicateDirective,
                    Reject::BadSha, Reject::BadAdmitted, Reject::EmptySelection,
                    Reject::MixedAll, Reject::BadRange, Reject::Unsorted,
                    Reject::Overlap, Reject::OutOfRange, Reject::TraceMismatch,
                    Reject::AdmittedMismatch};
                bool named = true;
                for (Reject r : all)
                    named = named && rejectName(r) &&
                            std::strcmp(rejectName(r), "unknown") != 0;
                check(named, "R20-24: every rejection reason has a name, so a "
                             "refusal always says WHY");
            }
        }

    // ══ iter-457 H1a: THE CUBE UPLOAD CONTRACT ═══════════════════════════
    // Drives the PRODUCTION validator `sr::gfx::hle::checkCubeUpload` and the
    // PRODUCTION layout body `sr::gfx::cube::computeCubeLayout` - not a copy.
    // Every refusal case mutates ONE field of an otherwise valid record, so a
    // pass proves the validator discriminates that field rather than rejecting
    // everything.
    {
        using namespace sr::gfx::hle;
        namespace cube = sr::gfx::cube;

        // ── C1. The layout body agrees with the independent Python port ─────
        // tools/check_cube_layout.py derives these from Xenia and checks them
        // against the four cube keys in archive/iter456_title_fresh.srht. Two
        // ports of the same authority that disagree means one is wrong.
        {
            const auto L = cube::computeCubeLayout(6, 128, 128, 128, true, 8,
                                                   true, 1, 1, 4);
            check(L.ok, "C1: a 128x128 k_8_8_8_8 tiled cube layout resolves");
            check(L.faceStrideBytes == 65536,
                  "C1: face stride 65,536 B (512 B row pitch x 128 tile-aligned "
                  "block rows), matching the Python port");
            check(L.baseSpanBytes == 393216,
                  "C1: SIX-FACE base span 393,216 B - the number the guest's own "
                  "mip_address independently confirms");
            check(L.packedLevel == 3,
                  "C1: the packed mip tail starts at level 3 for 128x128");
            check(L.mipsTotalExtentBytes == 147456,
                  "C1: the six-face mip chain totals 147,456 B");
            check(L.totalBytes == 540672,
                  "C1: total guest footprint 540,672 B = 393,216 + 147,456");

            // ── H1b-9. WHERE EACH STORED MIP LIVES ─────────────────────────
            // \u26d4 These are the numbers `tools/check_cube_layout.py` has been
            // PRINTING for this fixture since H1(a) - "mip offsets {1: 0,
            // 2: 98304, 3: 122880} strides {1: 16384, 2: 4096, 3: 4096}" -
            // computed by an INDEPENDENT Python port of the same Xenia
            // authority. Until H1(b) the C++ body discarded them and reported
            // only the total, so the two implementations agreed on a sum while
            // nothing checked the placement inside it. An uploader cannot
            // place a face from a sum, and a mip written to the wrong offset
            // is a texture that looks right at full size and wrong at
            // distance.
            check(L.lastStoredLevel == 3,
                  "H1b-9: levels 1..3 are stored separately; 4..7 live inside "
                  "the packed tail and have no storage of their own");
            check(L.mipOffsetBytes[1] == 0 && L.mipSliceStride[1] == 16384,
                  "H1b-9 KILLER: level 1 starts at 0 with a 16,384 B face "
                  "stride - the Python port's own value");
            check(L.mipOffsetBytes[2] == 98304 && L.mipSliceStride[2] == 4096,
                  "H1b-9 KILLER: level 2 starts at 98,304 (= 6 x 16,384) with "
                  "a 4,096 B stride");
            check(L.mipOffsetBytes[3] == 122880 && L.mipSliceStride[3] == 4096,
                  "H1b-9 KILLER: level 3 - the PACKED level - starts at "
                  "122,880 (= 98,304 + 6 x 4,096). Getting the packed tail's "
                  "start wrong is the failure the H1(a) GetPackedMipOffset "
                  "port exists to prevent");
            check(L.mipOffsetBytes[4] == 0 && L.mipSliceStride[4] == 0,
                  "H1b-9 KILLER: level 4 has NO separate storage and reports "
                  "zero, so an uploader that walks past lastStoredLevel reads "
                  "an obviously-invalid stride instead of a plausible offset");
            check(L.mipOffsetBytes[3] + L.mipSliceStride[3] * 6u
                      <= L.mipsTotalExtentBytes,
                  "H1b-9: the last stored level's six faces fit inside the "
                  "reported mip region - placement and total agree");
        }
        // ⛔ KILLER: the shape where the forbidden `6*w*h*bpp` intuition and the
        // authority DISAGREE. At 128x128 they coincide, which is exactly why
        // deriving the span from intuition would look right and generalise
        // wrong.
        {
            const auto L = cube::computeCubeLayout(6, 33, 33, 128, true, 1,
                                                   false, 1, 1, 4);
            check(L.ok && L.faceStrideBytes == 32768,
                  "C1 KILLER: a 33x33 cube face is the TILE-ALIGNED 32,768 B, "
                  "not 33*33*4 = 4,356");
            check(L.baseSpanBytes != 6u * 33u * 33u * 4u,
                  "C1 KILLER: the authority (196,608) and the 6*w*h*bpp "
                  "intuition (26,136) DISAGREE, so the intuition can never ship");
        }
        // ⛔ KILLER: the SKINNY shape. The packed tail's extent is the max over
        // EVERY sublevel in it, not just the packed level. Seven near-square
        // shapes cannot tell those two rules apart - the director produced this
        // 1x17 counterexample after I claimed, from seven samples, that they
        // were always equal. Here sublevel 0 gives y = 0 + 17 = 17 and sublevel
        // 3 gives y = 16 + 2 = 18, so a packed-level-only rule under-reports.
        {
            uint32_t px = 0, py = 0;
            const bool p0 = cube::detail::packedMipOffset(1, 17, 1, 1, 0, px, py);
            check(p0 && px == 16 && py == 0,
                  "C1: a 1x17 packs mip 0 at x=16, y=0");
            uint32_t px3 = 0, py3 = 0;
            const bool p3 = cube::detail::packedMipOffset(1, 17, 1, 1, 3, px3, py3);
            checkf(p3 && px3 == 0 && py3 == 16,
                   "C1 KILLER: a 1x17 packs mip 3 at x=0, y=16, so its extent "
                   "(16+2 = 18) EXCEEDS the packed level's own (17) - the max "
                   "must be taken over every sublevel (got %u,%u)", px3, py3);
        }
        // ⛔ KILLER: a format the backend cannot decode REFUSES. Capturing
        // pixels nothing can read is worse than not capturing them.
        {
            uint32_t bw = 0, bh = 0, bpb = 0;
            check(!cube::cubeBlockGeom(22, bw, bh, bpb),
                  "C1 KILLER: depth-as-texture format 22 is NOT decodable and "
                  "the shared block-geometry table says so");
            check(cube::cubeBlockGeom(18, bw, bh, bpb) && bw == 4 && bh == 4 &&
                      bpb == 8,
                  "C1: DXT1 is a 4x4 block of 8 bytes, from the ONE shared "
                  "table (kernel/tex_layout.hpp), not a second copy");
        }

        // ── C2. A VALID cube record round-trips through the real validator ──
        // Built exactly as the producer builds it, so the fields under test are
        // the production encoding rather than a restatement of it.
        auto makeCube = [](uint32_t w, uint32_t h, uint32_t pitch,
                           uint32_t mipLevels, bool packed) {
            RenderCommand c;
            c.type = RenderCommandType::TextureUpload;
            uint32_t bw = 1, bh = 1, bpb = 4;
            cube::cubeBlockGeom(6, bw, bh, bpb);
            const auto L = cube::computeCubeLayout(6, w, h, pitch, true,
                                                   mipLevels, packed, bw, bh, bpb);
            c.textureUpload.key = 0x03050000u;
            c.textureUpload.arenaOffset = 0;
            c.textureUpload.bytes = L.baseSpanBytes;
            c.textureUpload.mipBytes = L.mipsTotalExtentBytes;
            c.textureUpload.dims = (w << 16) | (h & 0xFFFFu);
            c.textureUpload.pitch = pitch;
            c.textureUpload.fmtBits =
                6u | (1u << 8) | (2u << 12) |
                (kUploadTexDimCube << kUploadTexDimShift) | (5u << 16) |
                ((mipLevels - 1u) << kUploadMipLevelsShift) |
                (packed ? kUploadPackedMipsBit : 0u);
            return c;
        };
        const RenderCommand good = makeCube(128, 128, 128, 8, true);
        const uint64_t goodTotal =
            (uint64_t)good.textureUpload.bytes + good.textureUpload.mipBytes;
        {
            const CubeCheck r = checkCubeUpload(good, goodTotal);
            check(r.ok, "C2: a well-formed cube record VALIDATES");
            checkf(r.baseBytes == 393216 && r.mipBytes == 147456,
                  "C2: the validator recomputes both segment lengths from the "
                  "record's own geometry (got %u/%u)", r.baseBytes, r.mipBytes);
            check(goodTotal == 540672,
                  "C2: TraceRecord.payloadBytes == baseBytes + mipBytes");
        }
        // The producer's own encoding must be readable by the predicates.
        check(uploadIsCube(good.textureUpload.fmtBits),
              "C2: texDim bits 14-15 read CUBE");
        check(uploadMipLevels(good.textureUpload.fmtBits) == 8,
              "C2: mipLevels-1 rides bits 27-29 and reads back as a COUNT of 8");
        check(uploadPackedMips(good.textureUpload.fmtBits),
              "C2: packedMips rides bit 30");
        // ⛔ The high group must not collide with the fields already there.
        check((good.textureUpload.fmtBits & 0xFFu) == 6u &&
                  ((good.textureUpload.fmtBits >> 12) & 3u) == 2u &&
                  ((good.textureUpload.fmtBits >> 16) & 0x3FFu) == 5u,
              "C2 KILLER: format, ENDIAN (bits 12-13) and depth-1 survive the "
              "cube fields intact - bits 27-30 were chosen because 9-11 would "
              "have run into endian at bit 12");

        // ── C3. EACH field mutated INDEPENDENTLY must refuse BY NAME ────────
        // A validator that rejects everything proves nothing; each case below
        // changes exactly one thing and asserts the SPECIFIC reason.
        struct Case { const char* what; const char* expect; RenderCommand c; uint64_t total; };
        auto mut = [&](const char* what, const char* expect,
                       void (*f)(RenderCommand&, uint64_t&)) {
            RenderCommand c = good; uint64_t t = goodTotal;
            f(c, t);
            const CubeCheck r = checkCubeUpload(c, t);
            checkf(!r.ok && std::strcmp(r.why, expect) == 0,
                   "C3 REFUSAL %s -> expected '%s', got '%s'", what, expect, r.why);
        };
        mut("baseBytes one byte short", "baseBytes",
            [](RenderCommand& c, uint64_t& t) { c.textureUpload.bytes--; t--; });
        mut("mipBytes one byte long", "mipBytes",
            [](RenderCommand& c, uint64_t& t) { c.textureUpload.mipBytes++; t++; });
        mut("the mip region is dropped entirely", "mipBytes",
            [](RenderCommand& c, uint64_t& t) {
              t -= c.textureUpload.mipBytes; c.textureUpload.mipBytes = 0; });
        mut("payload total disagrees with the two segments", "payloadTotal",
            [](RenderCommand&, uint64_t& t) { t += 4096; });
        mut("the face count says 5 instead of 6", "faceCount",
            [](RenderCommand& c, uint64_t&) {
              c.textureUpload.fmtBits =
                  (c.textureUpload.fmtBits & ~(0x3FFu << 16)) | (4u << 16); });
        mut("the format is one the backend cannot decode", "format",
            [](RenderCommand& c, uint64_t&) {
              c.textureUpload.fmtBits = (c.textureUpload.fmtBits & ~0xFFu) | 22u; });
        mut("packedMips is flipped off", "mipBytes",
            [](RenderCommand& c, uint64_t&) {
              c.textureUpload.fmtBits &= ~kUploadPackedMipsBit; });
        mut("the mip level count is wrong", "mipBytes",
            [](RenderCommand& c, uint64_t&) {
              c.textureUpload.fmtBits =
                  (c.textureUpload.fmtBits & ~(7u << kUploadMipLevelsShift)) |
                  (2u << kUploadMipLevelsShift); });
        mut("the pitch cannot address the width", "pitch",
            [](RenderCommand& c, uint64_t&) { c.textureUpload.pitch = 64; });
        mut("the dimension is not CUBE", "notCube",
            [](RenderCommand& c, uint64_t&) {
              c.textureUpload.fmtBits &= ~(3u << kUploadTexDimShift); });

        // ⛔ KILLER: a NON-cube upload must never acquire a mip segment. The
        // director's rule - v7 non-cube uploads keep existing semantics.
        {
            RenderCommand c = good;
            c.textureUpload.fmtBits &= ~(3u << kUploadTexDimShift);
            const CubeCheck r = checkCubeUpload(c, goodTotal);
            check(!r.ok && std::strcmp(r.why, "notCube") == 0,
                  "C3 KILLER: the validator declines a non-cube record rather "
                  "than validating it as one");
        }

        // ── C5. THE RECORDER TOTAL, through the PRODUCTION recorder ────────
        // ⛔ THE DIRECTOR'S FIND, as an executable fixture. The recorder used to
        // serialize `TextureUpload.bytes` as the payload length, so a cube's
        // SECOND region (the mip chain) would be silently DROPPED between the
        // producer and the trace. This drives H::traceRecordForTest — the same
        // body the live producer calls — and reads the length back off the real
        // artifact, so a regression here fails on the file, not on a comment.
        {
            namespace H = sr::gfx::hle;
            const uint32_t total = good.textureUpload.bytes +
                                   good.textureUpload.mipBytes;
            std::vector<uint8_t> pix(total);
            for (uint32_t i = 0; i < total; i++) pix[i] = (uint8_t)(i * 7u);
            const uint32_t off = H::texWrite(pix.data(), total);
            check(off != 0xFFFFFFFFu,
                  "C5: the 540,672 B two-region snapshot fits one texture slot");
            if (off != 0xFFFFFFFFu) {
                H::RenderCommand up = good;
                up.textureUpload.arenaOffset = off;
                H::RenderCommand pres2{};
                pres2.type = H::RenderCommandType::Present;
                _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "1");
                _putenv_s("SR_HLE_TRACE_FRAMES", "1");
                const char* cpath = "archive/rec_h1a_cube.bin";
                H::resetRecorderForTest(cpath);
                H::RenderCommand trig2{};
                trig2.type = H::RenderCommandType::Resolve;
                trig2.resolve.destKey = 0x0B5BE000u;
                trig2.resolve.destSize = 0x0059E4FFu;
                trig2.resolve.levelSliceFlags = sr::gfx::hle::kResolveDescValidBit;
                H::traceRecordForTest(trig2);
                H::traceRecordForTest(up);
                H::traceRecordForTest(pres2);
                H::releaseTracePathForTest();

                // A TARGETED read of the one field this fixture claims. The
                // R15 block's fail-closed walker is scoped to it; duplicating
                // that walker here would be the drift risk it exists to avoid,
                // so this reads the header version and each record's own
                // payloadBytes and nothing else.
                struct RawHdr { char magic[4]; uint32_t version; uint64_t records;
                                uint64_t payloadBytes; uint32_t presents;
                                uint32_t reserved; };
                struct RawRec2 { H::RenderCommand cmd; uint32_t payloadBytes;
                                 uint32_t reserved; uint64_t payloadHash; };
                bool found = false, hdrOk = false; uint32_t gotLen = 0, ver = 0;
                if (FILE* cf = std::fopen(cpath, "rb")) {
                    RawHdr hh{};
                    if (std::fread(&hh, sizeof(hh), 1, cf) == 1) {
                        hdrOk = hh.magic[0] == 'S' && hh.magic[1] == 'R' &&
                                hh.magic[2] == 'H' && hh.magic[3] == 'T';
                        ver = hh.version;
                    }
                    RawRec2 rr{};
                    while (hdrOk && std::fread(&rr, sizeof(rr), 1, cf) == 1) {
                        if (rr.cmd.type == H::RenderCommandType::TextureUpload) {
                            found = true; gotLen = rr.payloadBytes;
                        }
                        if (rr.payloadBytes)
                            std::fseek(cf, (long)rr.payloadBytes, SEEK_CUR);
                    }
                    std::fclose(cf);
                }
                check(hdrOk, "C5: the cube artifact carries an SRHT header");
                check(hdrOk && ver == 8u,
                      "C5: a trace carrying a cube upload is written as the "
                      "current version (v8 since Astra 086; cubes need >= 7)");
                check(found, "C5: the cube upload reached the trace");
                checkf(found && gotLen == total,
                       "C5 KILLER: TraceRecord.payloadBytes is the TOTAL "
                       "(base + mips = %u), not `bytes` alone (%u) - serializing "
                       "`bytes` here is what silently dropped the mip region "
                       "(got %u)", total, good.textureUpload.bytes, gotLen);
                check(found && gotLen != good.textureUpload.bytes,
                      "C5 KILLER: and it is DIFFERENT from `bytes`, so the "
                      "fixture can actually tell the two apart");
            }
        }

        // ── C7. THE BIT POSITIONS, AS LITERALS ─────────────────────────
        // ⛔ C2 asserted the round trip through the SAME named constants the
        // producer packs with, so moving a constant moved both sides together
        // and the mutation harness proved it: "packedMips is read from the
        // wrong bit" and "mipLevels reads bits 9-11" both SURVIVED. A constant
        // checked against itself is not a check. These are LITERALS on purpose,
        // exactly like the trace-version literal above.
        check(kUploadMipLevelsShift == 27u && kUploadMipLevelsMask == 7u,
              "C7 LITERAL: mipLevels-1 occupies bits 27-29");
        check(kUploadPackedMipsBit == (1u << 30),
              "C7 LITERAL: packedMips is bit 30");
        check(kUploadTexDimShift == 14u && kUploadTexDimCube == 3u,
              "C7 LITERAL: texDim is bits 14-15 and CUBE is 3");
        // ⛔ And the whole encoding, as one literal word. If any field moves,
        // this is the assertion that names it.
        // ⚠ I first wrote 0x545C0106 here from my head and this assertion
        // caught it. The real encoding decomposes as
        //   fmt 6 | tiled 1<<8 | endian 2<<12 | texDim 3<<14 | depth 5<<16
        //   | mipLevels-1 7<<27 | packedMips 1<<30  ==  0x7805E106
        checkf(good.textureUpload.fmtBits == 0x7805E106u,
               "C7 LITERAL: the full cube fmtBits encoding for 128x128 fmt6 "
               "tiled endian2 mips8 packed is 0x7805E106 (got 0x%08X)",
               good.textureUpload.fmtBits);
        // The bit groups must not overlap: OR-ing them must equal their sum.
        {
            const uint32_t dim = kUploadTexDimCube << kUploadTexDimShift;
            const uint32_t mips = 7u << kUploadMipLevelsShift;
            const uint32_t pk = kUploadPackedMipsBit;
            const uint32_t depth = 0x3FFu << 16;
            const uint32_t endian = 3u << 12;
            check((dim | mips | pk | depth | endian) ==
                      (dim + mips + pk + depth + endian) - (depth & mips),
                  "C7: depth-1 (16-25) and mipLevels (27-29) and packedMips "
                  "(30) and endian (12-13) and texDim (14-15) are laid out so "
                  "only depth/mips could ever have collided, and they do not");
            check((dim & endian) == 0 && (mips & depth) == 0 &&
                      (pk & mips) == 0 && (pk & depth) == 0,
                  "C7 KILLER: no cube field overlaps endian, depth or texDim - "
                  "bits 9-11 were rejected precisely because mipLevels-1 needs "
                  "three bits and would have run into endian at 12");
        }

        // ── C8. THE PACKED-TAIL EXTENT, at the layout level ─────────────
        // ⛔ C1 tested packedMipOffset directly; the mutation harness showed
        // that leaves the LAYOUT's tail loop unguarded ("uses only the packed
        // level" SURVIVED). These drive computeCubeLayout on shapes where the
        // two rules differ.
        {
            // A SQUARE takes the `log2W > log2H` ELSE arm. Inverting the
            // comparison to `>=` flips a square to the other arm, and only a
            // square can see that.
            uint32_t sx = 0, sy = 0;
            cube::detail::packedMipOffset(128, 128, 1, 1, 3, sx, sy);
            checkf(sx == 16 && sy == 0,
                   "C8 KILLER: a SQUARE 128x128 packs mip 3 along X (16,0), not "
                   "along Y - only a square distinguishes `>` from `>=` here "
                   "(got %u,%u)", sx, sy);
            // ⛔ r2 ERROR. I asserted a 1x17 has no separate mip region because
            // packedMipLevel is 0 and the C++ loop started at level 1. Xenia
            // treats packed_level == 0 as TWO tails (texture_util.cc:284-289).
            // The independent Python port already said mips_total=24576. The
            // fixture was false; this is the correction.
            const auto sk = cube::computeCubeLayout(6, 1, 17, 32, true, 5, true,
                                                    1, 1, 4);
            check(sk.ok && sk.packedLevel == 0,
                  "C8: a 1x17 packs from level 0 - min dimension <= 16");
            checkf(sk.ok && sk.mipsTotalExtentBytes == 24576,
                   "C8 KILLER: packed_level==0 has a SEPARATE mip tail of "
                   "24,576 B (texture_util.cc:284-289 two-tail loop). The r2 "
                   "fixture that asserted this region was ZERO was FALSE "
                   "(got %u)", sk.mipsTotalExtentBytes);
            check(sk.mipSliceStride[0] == 4096,
                  "C8: the mip tail is stored as mips[0] with a 4,096 B stride");
            check(sk.basePackedTailExtentYBlocks == 17 &&
                      sk.packedTailExtentYBlocks == 18,
                  "C8 KILLER: the BASE tail of a 1x17 is y=17 (sublevel 0 only) "
                  "and the MIPS tail is y=18 - sublevel 3 sits at y=16 with 2 "
                  "rows of its own. Seven near-square samples could not see this");
        }

        // ── C9. THE GATE DEFAULT ───────────────────────────────
        // SR_HLE_CUBE_CAPTURE must be OFF unless explicitly set to 1. A gate
        // that defaults on changes what every capture RECORDS.
        {
            // ⚠ The gate CACHES its env read in a function-local static, so
            // whichever fixture touched it first would own that latch forever -
            // C9 wants it OFF and C13 needs it ON. The rule is the PARSE, and
            // that is what is asserted here; the caching is plumbing.
            check(!sr::kernel::cubeGateWantsOnForTest(nullptr),
                  "C9 KILLER: SR_HLE_CUBE_CAPTURE is DEFAULT OFF - unset must "
                  "not enable it");
            check(!sr::kernel::cubeGateWantsOnForTest("0"),
                  "C9: an explicit 0 is OFF");
            check(!sr::kernel::cubeGateWantsOnForTest(""),
                  "C9: an EMPTY value is OFF, not 'set therefore on'");
            check(sr::kernel::cubeGateWantsOnForTest("1"),
                  "C9: only an explicit 1 turns it on");
        }

        // ── C6. THE FAIL-CLOSED EXECUTOR ARM ACTUALLY RUNS ───────────────
        // ⛔ Not "the branch exists" — "the branch EXECUTED". Before this arm,
        // a texDim == 3 upload fell into the 2D path and six faces' worth of
        // bytes were uploaded as one image into the 2D descriptor band. The
        // counter is read before and after so a mutant that removes the arm
        // shows up as a count that did not move.
        {
            namespace H = sr::gfx::hle;
            // ⛔ r1's version of this fixture staged NOTHING and passed
            // `arenaOffset = 0`. The director caught what that hid: with no
            // real allocation, a wrong-pool release is invisible, and a
            // counter moving proves neither OWNERSHIP nor TERMINAL control
            // flow. This version allocates a genuine texture slot first and
            // asserts all three properties.
            const uint32_t total = good.textureUpload.bytes +
                                   good.textureUpload.mipBytes;
            std::vector<uint8_t> pix(total, 0xC3);
            const uint32_t texBefore = H::texPoolInFlightForTest();
            const uint32_t arenaBefore = H::arenaInFlightForTest();
            const uint32_t off = H::texWrite(pix.data(), total);
            check(off != 0xFFFFFFFFu, "C6: a real texture slot was allocated");
            checkf(H::texPoolInFlightForTest() == texBefore + 1,
                   "C6: texWrite took exactly one texture slot (%u -> %u)",
                   texBefore, H::texPoolInFlightForTest());

            const uint64_t refBefore = H::cubeExecRefusedForTest();
            const uint64_t nonCubeBefore = H::nonCubeUploadEntriesForTest();
            H::RenderCommand up2 = good;
            up2.textureUpload.arenaOffset = off;
            H::executeForTest(up2);              // THE real dispatcher

            checkf(H::cubeExecRefusedForTest() == refBefore + 1,
                   "C6: the executor refused the cube by name (%llu -> %llu)",
                   (unsigned long long)refBefore,
                   (unsigned long long)H::cubeExecRefusedForTest());
            // ⛔ KILLER H1a-4: THE REFUSAL IS TERMINAL. This witness is
            // incremented at the FIRST statement of the non-cube path, before
            // any backend availability test, so deleting the `break` moves it
            // even with no backend at all. A "no 2D slot registered" assertion
            // could not do this - with a null backend nothing registers a slot
            // either way, which is exactly why that mutant survived r1.
            checkf(H::nonCubeUploadEntriesForTest() == nonCubeBefore,
                   "C6 KILLER: the cube refusal is TERMINAL - it did NOT fall "
                   "through into the non-cube path (entries %llu -> %llu)",
                   (unsigned long long)nonCubeBefore,
                   (unsigned long long)H::nonCubeUploadEntriesForTest());
            // ⛔ KILLER H1a-1: THE RIGHT POOL WAS RELEASED. `texWrite`
            // allocates from TexPool and its offsets carry no kPool*Flag, so
            // `poolRelease` would fall through to `arenaRelease`: the texture
            // slot stays busy forever AND an unrelated arena slot is freed.
            // Both halves are asserted, because either alone is survivable.
            checkf(H::texPoolInFlightForTest() == texBefore,
                   "C6 KILLER: the refusal returned the TEXTURE slot it "
                   "actually owned (%u -> %u; a poolRelease here leaks it)",
                   texBefore, H::texPoolInFlightForTest());
            checkf(H::arenaInFlightForTest() == arenaBefore,
                   "C6 KILLER: and it did NOT free an unrelated ARENA slot "
                   "(%u -> %u)", arenaBefore, H::arenaInFlightForTest());
        }

        // ── C10. THE VALID v7 ROUND TRIP, THROUGH THE REAL replayTrace ────
        // ⛔ The director's blocker: C2 called the validator directly and C5
        // read the recorder artifact directly, so the valid payload had never
        // traversed recorder -> file -> production replay staging -> executor.
        // This drives H::replayTrace on the artifact C5 actually emitted.
        {
            namespace H = sr::gfx::hle;
            H::resetCubeCountersForTest();
            H::TraceStats st{};
            const int rc = H::replayTrace("archive/rec_h1a_cube.bin", 1, &st);
            checkf(rc == 0, "C10: replayTrace accepted the v7 cube artifact "
                            "(rc=%d)", rc);
            checkf(st.hashMismatches == 0,
                   "C10 KILLER: ZERO hash mismatches over the FULL two-region "
                   "payload - the recorded length and the recorded bytes agree "
                   "end to end (got %llu)",
                   (unsigned long long)st.hashMismatches);
            checkf(st.payloadBytes == 540672ull,
                   "C10 KILLER: replay walked 540,672 payload bytes = 393,216 "
                   "base + 147,456 mips. A recorder that wrote `bytes` alone "
                   "would have walked 393,216 (got %llu)",
                   (unsigned long long)st.payloadBytes);
            checkf(H::cubeReplayAcceptedForTest() == 1 &&
                       H::cubeReplayRefusedForTest() == 0,
                   "C10: the replay cube gate ACCEPTED exactly one record and "
                   "refused none (accepted %llu, refused %llu)",
                   (unsigned long long)H::cubeReplayAcceptedForTest(),
                   (unsigned long long)H::cubeReplayRefusedForTest());
            checkf(H::cubeExecRefusedForTest() == 1,
                   "C10 KILLER: and the EXECUTOR then refused it fail-closed - "
                   "accepted by replay, refused by the executor, which is the "
                   "whole H1(a) contract (got %llu)",
                   (unsigned long long)H::cubeExecRefusedForTest());
            checkf(H::nonCubeUploadEntriesForTest() == 0,
                   "C10 KILLER: nothing entered the non-cube path during the "
                   "replay (got %llu)",
                   (unsigned long long)H::nonCubeUploadEntriesForTest());
        }

        // ── C11. A PRE-v7 RECORD CLAIMING A CUBE IS REFUSED BY NAME ──────
        // The version gate's WIRING, not just its constants. The same artifact
        // is copied with its header version rewritten to 5; everything else is
        // byte-identical, so the ONLY difference under test is the version.
        {
            namespace H = sr::gfx::hle;
            const char* v5path = "archive/rec_h1a_cube_v5.bin";
            bool made = false;
            if (FILE* in = std::fopen("archive/rec_h1a_cube.bin", "rb")) {
                std::vector<uint8_t> all;
                uint8_t buf[8192];
                size_t got;
                while ((got = std::fread(buf, 1, sizeof(buf), in)) > 0)
                    all.insert(all.end(), buf, buf + got);
                std::fclose(in);
                if (all.size() > 8) {
                    const uint32_t five = 5u;      // header dword 1 = version
                    std::memcpy(all.data() + 4, &five, 4);
                    if (FILE* out = std::fopen(v5path, "wb")) {
                        made = std::fwrite(all.data(), 1, all.size(), out) ==
                               all.size();
                        std::fclose(out);
                    }
                }
            }
            check(made, "C11: a v5 copy of the cube artifact was written");
            if (made) {
                H::resetCubeCountersForTest();
                H::TraceStats st5{};
                H::replayTrace(v5path, 1, &st5);
                checkf(H::cubeReplayRefusedForTest() == 1,
                       "C11 KILLER: the replayer REFUSED the pre-v7 cube record "
                       "(refusals %llu)",
                       (unsigned long long)H::cubeReplayRefusedForTest());
                checkf(std::strcmp(H::cubeReplayLastReasonForTest(),
                                   "preV7CubeRecord") == 0,
                       "C11 KILLER: and it refused for the VERSION reason "
                       "specifically, not by accidentally failing conservation "
                       "(got '%s')", H::cubeReplayLastReasonForTest());
                checkf(H::cubeReplayAcceptedForTest() == 0,
                       "C11: nothing was accepted (got %llu)",
                       (unsigned long long)H::cubeReplayAcceptedForTest());
                checkf(H::cubeExecRefusedForTest() == 0 &&
                           H::nonCubeUploadEntriesForTest() == 0,
                       "C11 KILLER: a refused record reaches NEITHER the "
                       "executor's cube arm NOR the non-cube path - it is "
                       "dropped, never reinterpreted as 2D (exec %llu, "
                       "nonCube %llu)",
                       (unsigned long long)H::cubeExecRefusedForTest(),
                       (unsigned long long)H::nonCubeUploadEntriesForTest());
            }
            H::resetCubeCountersForTest();
        }

        // ── C12. THE PRODUCER, driven through its REAL body ─────────────
        // ⛔ The mutant "the producer does not verify stack_depth == 5"
        // survived r1 because the capture was inline in the guest SetTexture
        // override and nothing could reach it. The rule now lives in
        // kernel/cube_capture_rule.hpp, PRODUCTION's only call site calls it,
        // and these fixtures call the SAME body with the three side effects
        // injected - the shape tex_unlock_rule.hpp already established.
        {
            namespace CC = sr::kernel::cubecap;
            // A synthetic cube fetch constant, assembled field by field from
            // xenos.h rather than copied off a trace.
            auto mkFetch = [](uint32_t stackDepth, uint32_t basePage,
                              uint32_t mipPage, uint32_t mipMax) {
                std::array<uint32_t, 6> f{};
                f[0] = 2u | (4u << 22) | (1u << 31);        // type, pitch 128, tiled
                f[1] = 6u | (2u << 6) | (basePage << 12);   // fmt, endian, base
                f[2] = 127u | (127u << 13) | (stackDepth << 26);
                f[3] = 0;
                f[4] = (mipMax & 0xFu) << 6;
                f[5] = (3u << 9) | (1u << 11) | (mipPage << 12);  // CUBE, packed
                return f;
            };
            std::vector<uint8_t> guest(4u << 20, 0xA7);
            std::vector<uint8_t> scratch(sr::gfx::hle::kTexSlotBytes);
            std::set<uint64_t> seen;
            int reads = 0, snaps = 0;
            std::vector<CC::CubeCapResult> published;
            auto drive = [&](const std::array<uint32_t, 6>& f) {
                reads = 0; snaps = 0;
                return CC::runCubeCapture(
                    f.data(), sr::gfx::hle::kTexSlotBytes,
                    sr::gfx::hle::kArenaFull,
                    [&](uint64_t k) { return seen.insert(k).second; },
                    [&](uint32_t, uint32_t bytes) -> const uint8_t* {
                        reads++;
                        return bytes <= guest.size() ? guest.data() : nullptr;
                    },
                    [&](const uint8_t*, uint32_t) { snaps++; return 12345u; },
                    [&](const CC::CubeCapResult& r) { published.push_back(r); },
                    scratch.data(), (uint32_t)scratch.size());
            };

            // ── the VALID case: stack_depth == 5 ─────────────────────
            const auto okRes = drive(mkFetch(5, 0x03050u, 0x030B0u, 7));
            check(okRes.published, "C12: a valid cube fetch PUBLISHES");
            checkf(okRes.baseBytes == 393216u && okRes.mipBytes == 147456u,
                   "C12 KILLER: TWO-REGION publication - 393,216 B base and "
                   "147,456 B mips (got %u / %u)", okRes.baseBytes,
                   okRes.mipBytes);
            checkf(okRes.totalBytes == 540672u,
                   "C12: the arena holds both regions concatenated (got %u)",
                   okRes.totalBytes);
            checkf(reads == 2,
                   "C12 KILLER: BOTH guest regions were read - base_address and "
                   "mip_address are independent in Xenia, so adjacency is never "
                   "assumed (got %d reads)", reads);
            check(snaps == 1, "C12: exactly one arena snapshot for both regions");
            checkf(published.size() == 1 &&
                       published[0].fmtBits == 0x7805E106u,
                   "C12 KILLER: the PUBLISHED command carries the cube "
                   "encoding 0x7805E106 (got 0x%08X)",
                   published.empty() ? 0u : published[0].fmtBits);
            check(okRes.faceStride == 65536u && okRes.mipLevels == 8 &&
                      okRes.packedMips,
                  "C12: face stride, mip count and packedMips all reach the "
                  "command");

            // ── the INVALID case: stack_depth != 5 ──────────────────
            // ⛔ THE MUTANT THIS KILLS. Six faces must be VERIFIED from the
            // fetch, not assumed. Every other field is identical.
            const size_t pubBefore = published.size();
            const auto badRes = drive(mkFetch(0, 0x04050u, 0x040B0u, 7));
            checkf(!badRes.published &&
                       badRes.refuse == CC::CubeCapRefuse::StackDepth,
                   "C12 KILLER: stack_depth != 5 REFUSES by name (published=%d, "
                   "reason '%s')", (int)badRes.published,
                   CC::cubeCapRefuseName(badRes.refuse));
            checkf(published.size() == pubBefore,
                   "C12 KILLER: and it published NOTHING (%zu -> %zu)",
                   pubBefore, published.size());
            checkf(reads == 0 && snaps == 0,
                   "C12 KILLER: it refused BEFORE reading guest memory or "
                   "touching the arena - the invariant is checked first, so a "
                   "malformed fetch never costs a slot (reads %d, snaps %d)",
                   reads, snaps);

            // A second valid drive of the SAME identity must dedupe, so the
            // refusal above cannot be mistaken for the dedupe path.
            const auto dupRes = drive(mkFetch(5, 0x03050u, 0x030B0u, 7));
            checkf(!dupRes.published &&
                       dupRes.refuse == CC::CubeCapRefuse::Duplicate,
                   "C12: a repeated identity refuses as 'duplicate', a "
                   "DIFFERENT reason from stackDepth (got '%s')",
                   CC::cubeCapRefuseName(dupRes.refuse));

            // mip_address == 0 means NO mips whatever the level field says.
            const auto noMip = drive(mkFetch(5, 0x05050u, 0u, 7));
            checkf(noMip.published && noMip.mipBytes == 0 &&
                       noMip.baseBytes == 393216u,
                   "C12 KILLER: mip_address 0 publishes the base region ONLY, "
                   "with mipBytes 0, however high mip_max_level reads "
                   "(base %u, mips %u)", noMip.baseBytes, noMip.mipBytes);
        }

        // ── C13. THE GUEST SetTexture OVERRIDE, END TO END ────────────
        // ⛔ TWO DEFECTS THE DIRECTOR FOUND IN r3, both fixed here.
        //
        // GAP A: r3's "valid" cube never published. It refused as
        // baseUnreadable, and r3 asserted only the dispatch witness - a green
        // assertion over a refused path. `pickAliasWindow` forms
        // `0x80000000 | (phys & 0x1FFFFFFF)`, so a 2 MiB low-address image can
        // never contain the address it probes. There is NO base address that
        // lets a sub-2 GiB image succeed.
        //
        // GAP B: r3 called the observer directly, so replacing the wrapper's
        // `observeSetTextureFetch(dev, slot)` with 0 still left 410/410 green.
        // C13 now enters `sub_82100310` itself.
        {
            namespace K = sr::kernel;
            namespace H = sr::gfx::hle;
            // A SPARSE guest image: reserve past the 0x8 alias window, commit
            // only what is actually touched. Reserving 2.1 GiB costs address
            // space, not memory.
            const size_t kResv = 0x8400000ull + 0x80000000ull;   // > 0x83FFFFFF
            uint8_t* g = (uint8_t*)VirtualAlloc(nullptr, kResv, MEM_RESERVE,
                                                PAGE_NOACCESS);
            check(g != nullptr, "C13: the sparse guest reservation succeeded");
            if (g) {
                auto commit = [&](uint32_t va, size_t n) {
                    return VirtualAlloc(g + va, n, MEM_COMMIT,
                                        PAGE_READWRITE) != nullptr;
                };
                const uint32_t dev = 0x00100000u, slot = 3u;
                // The device pages the wrapper and the delegate touch: the
                // fetch-constant array at +1024, the previous-binding array at
                // +12272, and the two mip shadows near +11678/+11704.
                bool ok = commit(dev, 0x8000u);
                // THE CUBE. base 0x03050000 -> probed at 0x83050000.
                const uint32_t basePhys = 0x03050000u;
                const uint32_t baseSpan = 393216u;      // six faces
                const uint32_t mipSpan  = 147456u;
                ok = ok && commit(0x80000000u | basePhys, baseSpan);
                // ⛔ THE MIP IS DELIBERATELY NON-ADJACENT, and the range that
                // WOULD be adjacent is left UNCOMMITTED. A producer that
                // assumed one contiguous region - or read the mips from
                // base+baseSpan - cannot pass this fixture, it faults or
                // refuses. Xenia treats the two addresses as independent; our
                // four observed keys merely happen to be adjacent.
                const uint32_t mipPhys = 0x04200000u;
                ok = ok && commit(0x80000000u | mipPhys, mipSpan);
                check(ok, "C13: device, base and NON-ADJACENT mip pages "
                          "committed (the adjacent range deliberately is NOT)");

                // Distinguishable payloads, so `base || mips` can be checked
                // byte-exactly rather than by length alone.
                uint8_t* hb = g + (0x80000000u | basePhys);
                uint8_t* hm = g + (0x80000000u | mipPhys);
                for (uint32_t i = 0; i < baseSpan; i++) hb[i] = (uint8_t)(i * 31u + 7u);
                for (uint32_t i = 0; i < mipSpan; i++)  hm[i] = (uint8_t)(i * 17u + 200u);

                uint8_t* prevBase = nullptr; size_t prevSize = 0;
                K::getGlobalGuestMemory(&prevBase, &prevSize);
                K::setGuestMemory(g, kResv);

                // The fetch constant the delegate would have written. The
                // bypass is on, so the fixture writes it; everything after it
                // in sub_82100310 runs for real.
                const uint32_t fcva = dev + 1024u + 24u * slot;
                auto writeFetch = [&](uint32_t stackDepth, uint32_t bp,
                                      uint32_t mp, uint32_t mipMax) {
                    K::writeGuestBE32(fcva + 0,  2u | (4u << 22) | (1u << 31));
                    K::writeGuestBE32(fcva + 4,  6u | (2u << 6) | (bp << 12));
                    K::writeGuestBE32(fcva + 8,  127u | (127u << 13) |
                                                 (stackDepth << 26));
                    K::writeGuestBE32(fcva + 12, 0);
                    K::writeGuestBE32(fcva + 16, (mipMax & 0xFu) << 6);
                    K::writeGuestBE32(fcva + 20, (3u << 9) | (1u << 11) |
                                                 (mp << 12));
                };

                // Capture the enqueued command SYNCHRONOUSLY - the executor
                // frees the payload slot, so reading it after the fact races a
                // live consumer.
                static H::RenderCommand s_tapped;
                static int s_tapCount;
                static std::vector<uint8_t> s_tapPayload;
                s_tapCount = 0;
                H::setEnqueueTapForTest([](const H::RenderCommand& c) {
                    if (c.type != H::RenderCommandType::TextureUpload) return;
                    if (!H::uploadIsCube(c.textureUpload.fmtBits)) return;
                    s_tapped = c;
                    s_tapCount++;
                    const uint32_t n = c.textureUpload.bytes +
                                       c.textureUpload.mipBytes;
                    if (const uint8_t* p = H::texRead(c.textureUpload.arenaOffset))
                        s_tapPayload.assign(p, p + n);
                });

                // ⚠ WHAT THIS ASSERTION DOES AND DOES NOT PROVE. It proves
                // the bypass ARMS inside this process, which is what the rest
                // of C13 needs. It says NOTHING about a process where the gate
                // is closed - the director deleted the only outside-suite
                // refusal and this assertion stayed green, so the earlier
                // comment claiming it proved "the gate is real rather than
                // decorative" was an overclaim. The gate-off half is proved by
                // a SEPARATE PROCESS (SR_HLE_TEX_GATEOFF_TEST, G1-G5 in
                // runGateOffSelfTestIfRequested below), which observes both the
                // false return and the state that never moved.
                check(K::setSetTextureDelegateBypassForTest(true),
                      "C13: the delegate bypass ARMS inside the suite. The refusal "
                      "on the other side of the gate is a different process's "
                      "assertion, not this one");
                PPCContext cx{};
                cx.r3.u32 = dev; cx.r4.u32 = slot; cx.r5.u32 = 0; cx.r6.u32 = 0;

                // ── the VALID cube, through sub_82100310 ───────────────
                K::resetCubeCapCountersForTest();
                s_tapPayload.clear();
                writeFetch(5, basePhys >> 12, mipPhys >> 12, 7);
                ::sub_82100310(cx, g);            // THE GUEST OVERRIDE ITSELF

                checkf(K::cubeDispatchesForTest() == 1,
                       "C13 KILLER: sub_82100310 reached the cube dispatch. "
                       "Replacing its observeSetTextureFetch call with 0 - which "
                       "passed r3 silently at 410/410 - stops this dead (%llu)",
                       (unsigned long long)K::cubeDispatchesForTest());
                checkf(K::cubeCapPublishedForTest() == 1,
                       "C13 KILLER: and it PUBLISHED. r3 asserted only the "
                       "dispatch and its 'valid' cube was refused as "
                       "baseUnreadable the whole time (published %llu, last "
                       "refusal '%s')",
                       (unsigned long long)K::cubeCapPublishedForTest(),
                       K::cubeCapLastRefuseForTest());
                checkf(K::legacy2DEntriesForTest() == 0,
                       "C13: and took no legacy 2D path (%llu)",
                       (unsigned long long)K::legacy2DEntriesForTest());

                // THE EXACT COMMAND, field by field.
                checkf(s_tapCount == 1,
                       "C13: exactly one cube TextureUpload was enqueued (%d)",
                       s_tapCount);
                if (s_tapCount == 1) {
                    const auto& u = s_tapped.textureUpload;
                    checkf(u.key == basePhys,
                           "C13: key is the base address %08X (got %08X)",
                           basePhys, u.key);
                    checkf(u.bytes == baseSpan && u.mipBytes == mipSpan,
                           "C13 KILLER: the command carries BOTH region lengths "
                           "- %u base + %u mips (got %u / %u)",
                           baseSpan, mipSpan, u.bytes, u.mipBytes);
                    checkf(u.dims == ((128u << 16) | 128u),
                           "C13: dims are 128x128 (got %08X)", u.dims);
                    checkf(u.pitch == 128u, "C13: pitch is 128 (got %u)", u.pitch);
                    checkf(u.fmtBits == 0x7805E106u,
                           "C13 KILLER: the exact cube encoding - format, tiled, "
                           "endian, texDim CUBE, six faces, 8 mip levels and "
                           "packedMips (got 0x%08X)", u.fmtBits);
                    checkf(u.arenaOffset != 0xFFFFFFFFu,
                           "C13: a real arena slot was taken (got %08X)",
                           u.arenaOffset);

                    // ⛔ BYTE-EXACT base || mips. A one-region capture, or one
                    // that guessed the mips were adjacent, cannot produce this.
                    checkf(s_tapPayload.size() == (size_t)baseSpan + mipSpan,
                           "C13: the arena holds both regions (%zu of %u)",
                           s_tapPayload.size(), baseSpan + mipSpan);
                    if (s_tapPayload.size() == (size_t)baseSpan + mipSpan) {
                        const bool baseOk =
                            std::memcmp(s_tapPayload.data(), hb, baseSpan) == 0;
                        const bool mipOk =
                            std::memcmp(s_tapPayload.data() + baseSpan, hm,
                                        mipSpan) == 0;
                        check(baseOk,
                              "C13 KILLER: the first 393,216 arena bytes are the "
                              "SIX BASE FACES, byte for byte");
                        check(mipOk,
                              "C13 KILLER: and the next 147,456 are the mip "
                              "chain read from a NON-ADJACENT address - which a "
                              "single contiguous read could not have produced");
                    }
                }

                // ── stack_depth != 5 ───────────────────────────
                K::resetCubeCapCountersForTest();
                s_tapCount = 0;
                writeFetch(0, basePhys >> 12, mipPhys >> 12, 7);
                ::sub_82100310(cx, g);
                checkf(K::cubeDispatchesForTest() == 1 &&
                           K::cubeCapPublishedForTest() == 0,
                       "C13: an invalid cube reaches the dispatch and publishes "
                       "nothing (dispatch %llu, published %llu)",
                       (unsigned long long)K::cubeDispatchesForTest(),
                       (unsigned long long)K::cubeCapPublishedForTest());
                checkf(std::strcmp(K::cubeCapLastRefuseForTest(),
                                   "stackDepth") == 0,
                       "C13 KILLER: refused for the Xenos invariant "
                       "specifically, IN SITU (got '%s')",
                       K::cubeCapLastRefuseForTest());
                check(s_tapCount == 0,
                      "C13 KILLER: and enqueued NOTHING");

                // ── a 2D fetch ────────────────────────────────
                K::resetCubeCapCountersForTest();
                K::writeGuestBE32(fcva + 20, (1u << 9));      // k2DOrStacked
                ::sub_82100310(cx, g);
                checkf(K::cubeDispatchesForTest() == 0,
                       "C13 KILLER: a 2D fetch does NOT enter the cube dispatch "
                       "(%llu)", (unsigned long long)K::cubeDispatchesForTest());
                checkf(K::legacy2DEntriesForTest() == 1,
                       "C13 KILLER: and DOES take the legacy 2D path - so both "
                       "witnesses are live, and the zeros asserted above are "
                       "real discriminations rather than dead counters (%llu)",
                       (unsigned long long)K::legacy2DEntriesForTest());

                H::setEnqueueTapForTest(nullptr);
                K::setSetTextureDelegateBypassForTest(false);
                K::setGuestMemory(prevBase, prevSize);
                VirtualFree(g, 0, MEM_RELEASE);
            }
        }

        // ── C14. THE TEXTURE CENSUS'S PRODUCER TAP, END TO END (Astra 069) ──
        // The bind capture's census hunk (hle_api_observe.cpp) through the
        // guest override sub_82100310 itself, with the census on: the ISSUED
        // decode names ITS OWN upload (FIFO index, the payload hash publish()
        // took from the snapshot the command carries, the 0x8 alias the
        // snapshot came from); an unchanged re-bind records nothing; a guest
        // write in that alias is SUPPRESSED_CHANGED, hashed in that alias.
        {
            namespace K = sr::kernel;
            namespace H = sr::gfx::hle;
            namespace T = sr::kernel::txc;
            const size_t kResv = 0x8400000ull + 0x80000000ull;
            uint8_t* g = (uint8_t*)VirtualAlloc(nullptr, kResv, MEM_RESERVE,
                                                PAGE_NOACCESS);
            check(g != nullptr, "C14: the sparse guest reservation succeeded");
            if (g) {
                auto commit = [&](uint32_t va, size_t n) {
                    return VirtualAlloc(g + va, n, MEM_COMMIT,
                                        PAGE_READWRITE) != nullptr;
                };
                const uint32_t dev = 0x00100000u, slot = 2u;
                const uint32_t basePhys = 0x03400000u;
                const uint32_t kBytes = 128u * 128u * 4u;   // 128x128 k_8_8_8_8, linear
                const bool ok = commit(dev, 0x8000u) &&
                                commit(0x80000000u | basePhys, kBytes);
                check(ok, "C14: device and texture pages committed");
                uint8_t* hb = g + (0x80000000u | basePhys);
                for (uint32_t i = 0; i < kBytes; i++) hb[i] = (uint8_t)(i * 13u + 5u);
                uint8_t* prevBase = nullptr; size_t prevSize = 0;
                K::getGlobalGuestMemory(&prevBase, &prevSize);
                K::setGuestMemory(g, kResv);
                const char* tmp = std::getenv("TEMP");
                const std::string path = std::string(tmp ? tmp : ".") + "/sr_txc_c14.bin";
                T::setForTest(path.c_str(), 0);
                K::physwin::notePhysGrant(0xA0000000u | basePhys, kBytes);
                const uint32_t fcva = dev + 1024u + 24u * slot;
                K::writeGuestBE32(fcva + 0,  2u | (4u << 22));            // linear, pitch 128
                K::writeGuestBE32(fcva + 4,  6u | (2u << 6) | ((basePhys >> 12) << 12));
                K::writeGuestBE32(fcva + 8,  127u | (127u << 13));
                K::writeGuestBE32(fcva + 12, 0);
                K::writeGuestBE32(fcva + 16, 0);
                K::writeGuestBE32(fcva + 20, (1u << 9));                  // 2D
                check(K::setSetTextureDelegateBypassForTest(true),
                      "C14: the delegate bypass arms inside the suite");
                PPCContext cx{};
                cx.r3.u32 = dev; cx.r4.u32 = slot; cx.r5.u32 = 0; cx.r6.u32 = 0;
                auto evs = [] {
                    std::vector<T::Ev> v(1u << 14);
                    v.resize((size_t)T::eventsForTest(v.data(), v.size()));
                    return v;
                };
                auto decodesOf = [&](uint32_t key) {
                    std::vector<T::Ev> r;
                    for (const T::Ev& e : evs())
                        if (e.type == T::kTexDecode && e.a == key) r.push_back(e);
                    return r;
                };

                ::sub_82100310(cx, g);                 // THE GUEST OVERRIDE ITSELF
                T::Ev pub{};
                int pubs = 0;
                for (const T::Ev& e : evs())
                    if (e.type == T::kTexPublish && e.a == basePhys) { pub = e; pubs++; }
                const auto d1 = decodesOf(basePhys);
                checkf(pubs == 1 && d1.size() == 1,
                       "C14: one bind published one upload and one census decode "
                       "(publishes %d, decodes %zu)", pubs, d1.size());
                if (pubs == 1 && d1.size() == 1) {
                    const T::Ev& d = d1[0];
                    checkf((d.e & 0xFFu) == T::kIssued && ((d.e >> 24) & 0xFu) == 8u &&
                               d.y == pub.z && d.x == pub.x && d.z < d.y && d.y <= d.w,
                           "C14 KILLER: the ISSUED decode names ITS upload (FIFO %llu vs "
                           "publish %llu), the same payload hash, alias 0x8, and brackets "
                           "it ([%llu,%llu])",
                           (unsigned long long)d.y, (unsigned long long)pub.z,
                           (unsigned long long)d.z, (unsigned long long)d.w);
                    checkf(pub.c <= kBytes && pub.x == T::probeHash(hb, pub.c) &&
                               pub.d == ((128u << 16) | 128u) && (pub.e & 0xFFu) == 6u,
                           "C14 KILLER: publish() hashed the snapshot's bytes - the "
                           "guest texture as bound (bytes %u)", pub.c);
                    checkf(((d.e >> 8) & 0xFFu) == 1u && (d.v != 0),
                           "C14: the decode sees the texture's one owner");
                }
                ::sub_82100310(cx, g);                 // the same bind, unchanged
                checkf(decodesOf(basePhys).size() == 1,
                       "C14: an unchanged re-bind is deduped and records nothing (%zu)",
                       decodesOf(basePhys).size());
                for (uint32_t i = 0; i < 64; i++) hb[i] ^= 0x5Au;     // a guest write
                ::sub_82100310(cx, g);
                const auto d3 = decodesOf(basePhys);
                checkf(d3.size() == 2 && (d3.back().e & 0xFFu) == T::kSuppressedChanged &&
                           ((d3.back().e >> 24) & 0xFu) == 8u &&
                           d3.back().x == T::probeHash(hb, pub.c) && d3.back().y == 0,
                       "C14 KILLER: a guest write in the upload's alias is SUPPRESSED_CHANGED, "
                       "hashed in that alias (decodes %zu)", d3.size());

                T::setForTest(nullptr, 0);
                K::setSetTextureDelegateBypassForTest(false);
                K::setGuestMemory(prevBase, prevSize);
                VirtualFree(g, 0, MEM_RELEASE);
            }
        }

        // ── H1b. THE CUBE RESOURCE / SRV / UPLOAD / BIND CONTRACT ──────────
        // Authorized by the r7 verdict. Every clause of the director's
        // contract is a separate assertion, because "the cube slice works" is
        // exactly the kind of sentence that hides which half of it does.
        {
            namespace CB = sr::gfx::cubebind;

            // ── H1b-1. THE RESOURCE IS A 2D ARRAY OF SIX, NOT A VOLUME ─────
            // \u26d4 D3D12 has no cube resource dimension. A cube IS a
            // TEXTURE2D with six array slices, and only the SRV says "cube".
            // A TEXTURE3D of depth 6 compiles, binds, and samples the wrong
            // thing - which is why this is asserted rather than assumed.
            const auto r128 = CB::planResource(CB::Dim::Cube, 128, 128, 8, true);
            check(r128.ok, "H1b-1: a 128x128 8-mip cube plans successfully");
            check(r128.depthOrArraySize == 6,
                  "H1b-1 KILLER: DepthOrArraySize is SIX - the six faces are "
                  "ARRAY SLICES of a TEXTURE2D, not the depth of a volume");
            check(r128.mipLevels == 8,
                  "H1b-1: all eight represented mips are planned");
            check(r128.subresourceCount == 48,
                  "H1b-1 KILLER: 6 faces x 8 mips = 48 subresources. Uploading "
                  "6 (faces only) or 8 (mips only) is the arithmetic slip this "
                  "pins");

            // ── H1b-2. SUBRESOURCE ORDER IS MIP-MAJOR WITHIN EACH SLICE ────
            check(CB::subresourceIndex(0, 0, 8) == 0,
                  "H1b-2: face 0 mip 0 is subresource 0");
            check(CB::subresourceIndex(0, 3, 8) == 3,
                  "H1b-2: mips advance by one inside a face");
            check(CB::subresourceIndex(1, 0, 8) == 8,
                  "H1b-2 KILLER: face 1 starts at mipLevels, not at 1. Getting "
                  "this backwards writes face 1's base into face 0's mip 1 - "
                  "six faces present, every one of them wrong");
            check(CB::subresourceIndex(5, 7, 8) == 47,
                  "H1b-2: the last face's last mip is the last subresource");

            // ── H1b-3. NAMED FAIL-CLOSED REFUSALS ──────────────────────────
            check(CB::planResource(CB::Dim::TwoD, 128, 128, 1, true).refuse
                      == CB::CubeRefuse::NotCube,
                  "H1b-3: a non-cube dimension is refused as NotCube");
            check(CB::planResource(CB::Dim::Cube, 0, 0, 1, true).refuse
                      == CB::CubeRefuse::ZeroExtent,
                  "H1b-3: a zero extent is refused");
            check(CB::planResource(CB::Dim::Cube, 128, 64, 1, true).refuse
                      == CB::CubeRefuse::NonSquare,
                  "H1b-3 KILLER: a NON-SQUARE request is refused, not squared "
                  "up. A cube face is square by definition, so a non-square "
                  "extent means we decoded something that is not a cube");
            check(CB::planResource(CB::Dim::Cube, 8192, 8192, 1, true).refuse
                      == CB::CubeRefuse::ExtentTooLarge,
                  "H1b-3: an oversized extent is refused");
            check(CB::planResource(CB::Dim::Cube, 128, 128, 1, false).refuse
                      == CB::CubeRefuse::UnsupportedFormat,
                  "H1b-3: an unsupported format is refused");
            check(CB::planResource(CB::Dim::Cube, 128, 128, 0, true).refuse
                      == CB::CubeRefuse::NoMipLevels,
                  "H1b-3: zero mip levels is refused");
            check(CB::planResource(CB::Dim::Cube, 128, 128, 9, true).refuse
                      == CB::CubeRefuse::TooManyMips,
                  "H1b-3 KILLER: 128x128 holds EIGHT mips; a ninth is refused "
                  "rather than allocated and left undefined");
            check(CB::planResource(CB::Dim::Cube, 1, 1, 1, true).ok,
                  "H1b-3 CONTROL: a 1x1 cube is legal and holds exactly one mip");

            // ── H1b-4. THE UPLOAD IS ALL SIX FACES OR NOTHING ──────────────
            check(!CB::planUpload(r128, 5, 8, 100, 100).ok,
                  "H1b-4 KILLER: FIVE faces is refused. A five-face cube "
                  "renders - wrongly, in one direction only - and reads as a "
                  "bug in the scene rather than in the upload");
            check(CB::planUpload(r128, 5, 8, 100, 100).refuse
                      == CB::CubeRefuse::FaceDataMissing,
                  "H1b-4: ...and the refusal is NAMED FaceDataMissing");
            check(CB::planUpload(r128, 6, 7, 100, 100).refuse
                      == CB::CubeRefuse::MipDataMissing,
                  "H1b-4 KILLER: a missing represented mip is refused too - a "
                  "cube whose tail mip is undefined shimmers at distance");
            check(CB::planUpload(r128, 6, 8, 99, 100).refuse
                      == CB::CubeRefuse::ByteCountMismatch,
                  "H1b-4 KILLER: the byte count must match EXACTLY. A short "
                  "buffer copied as if it were whole is the reading of "
                  "uninitialised memory the layout work exists to prevent");
            check(CB::planUpload(r128, 6, 8, 100, 0).refuse
                      == CB::CubeRefuse::ByteCountMismatch,
                  "H1b-4: a zero EXPECTED byte count is refused - it means the "
                  "layout was never computed, so nothing can be checked");
            const auto u128 = CB::planUpload(r128, 6, 8, 393216, 393216);
            check(u128.ok && u128.subresources == 48,
                  "H1b-4 CONTROL: six faces, eight mips and an exact byte "
                  "count upload all 48 subresources");
            // \u26d4 The arguments here are chosen so EVERY OTHER CHECK IN
            // planUpload WOULD PASS: a refused plan carries mipLevels 0, so
            // mipsPresent 0 matches it, six faces match, and the byte counts
            // agree. Only the `!res.ok` guard can refuse this. The first
            // version of this fixture passed mipsPresent 1 and was therefore
            // killed by MipDataMissing instead - it survived deleting the
            // guard, which is exactly the blindness this harness looks for.
            const auto rBad = CB::planResource(CB::Dim::Cube, 128, 64, 1, true);
            const auto uBad = CB::planUpload(rBad, 6, 0, 8, 8);
            check(!uBad.ok,
                  "H1b-4 KILLER: an upload against a REFUSED resource plan is "
                  "refused even when every other clause would have passed");
            check(uBad.refuse == CB::CubeRefuse::NonSquare,
                  "H1b-4 KILLER: ...and it carries the RESOURCE's reason "
                  "(NonSquare) forward rather than inventing a downstream one. "
                  "A refusal that renames itself sends the reader to the wrong "
                  "code");

            // ── H1b-5. INDICES 1..7, DUMMY STAYS 0 ─────────────────────────
            // r5: the indices are a FREE LIST, not a cursor. slotAcquire hands
            // out the lowest free real index; slotRelease returns one. Index 0
            // (the white dummy) is never in the mask and never handed out.
            {
                namespace TX = sr::gfx::cubetx;
                CB::CubeRefuse why = CB::CubeRefuse::None;
                uint32_t mask = TX::kFreeMaskAll;
                check(TX::slotFreeCount(mask) == 7,
                      "H1b-5: seven real cube indices are available");
                check((TX::kFreeMaskAll & 1u) == 0,
                      "H1b-5 KILLER: index 0 is NOT in the free list - it is the "
                      "white dummy every unbound cube slot already reads");
                check(TX::slotAcquire(mask, why) == 1 && why == CB::CubeRefuse::None,
                      "H1b-5: the first real cube index is 1");
                for (uint32_t i = 2; i <= 7; i++) {
                    const uint32_t got = TX::slotAcquire(mask, why);
                    checkf(got == i && why == CB::CubeRefuse::None,
                           "H1b-5: acquisition %u yields index %u (got %u)", i, i, got);
                }
                check(TX::slotAcquire(mask, why) == 0
                          && why == CB::CubeRefuse::NoSlot && mask == 0,
                      "H1b-5 KILLER: exhaustion degrades to the DUMMY (0) and says "
                      "NoSlot. Wrapping to 1 would alias another scene's cube - a "
                      "wrong picture instead of the old one");
                TX::slotRelease(mask, 0);
                check((mask & 1u) == 0 && TX::slotAcquire(mask, why) == 0
                          && why == CB::CubeRefuse::NoSlot,
                      "H1b-5 KILLER: releasing index 0 does not put the dummy into "
                      "the free list");
                TX::slotRelease(mask, 4);
                check(TX::slotAcquire(mask, why) == 4 && why == CB::CubeRefuse::None,
                      "H1b-5: a released index is the next one handed out");
            }

            // ── H1b-6. EVERY BIND WRITES ALL THREE BANDS ───────────────────
            // \u26d4 The real index goes in the matching band and the other two
            // are ZEROED. Before this, the publisher wrote the 2D and 3D bands
            // for the same slot unconditionally and never touched the cube
            // band. That is safe only while a key can never appear in two
            // tables; the moment it can, a shader compiled for one dimension
            // reads a live index belonging to another and samples a completely
            // different texture.
            const auto bc = CB::planBands(CB::Dim::Cube, 42, 5, 3);
            check(bc.bandCube == 3,
                  "H1b-6: a CUBE slot publishes its real index in the cube band");
            check(bc.band2D == 0 && bc.band3D == 0,
                  "H1b-6 KILLER: ...and ZEROES the 2D and 3D bands, even though "
                  "real indices were available for both. Leaving 42 in the 2D "
                  "band is how a cube-bound slot samples an unrelated texture");
            const auto b3 = CB::planBands(CB::Dim::ThreeD, 42, 5, 3);
            check(b3.band3D == 5 && b3.band2D == 0 && b3.bandCube == 0,
                  "H1b-6 KILLER: a VOLUME slot publishes only the 3D band");
            const auto b2 = CB::planBands(CB::Dim::TwoD, 42, 5, 3);
            check(b2.band2D == 42 && b2.band3D == 0 && b2.bandCube == 0,
                  "H1b-6 KILLER: a 2D slot publishes only the 2D band - this is "
                  "the case that used to publish 2D AND 3D together");
            const auto b1 = CB::planBands(CB::Dim::OneD, 42, 5, 3);
            check(b1.band2D == 42 && b1.band3D == 0 && b1.bandCube == 0,
                  "H1b-6: a 1D fetch rides the 2D band, as the backend has no "
                  "1D range");
            const auto bz = CB::planBands(CB::Dim::Cube, 42, 5, 0);
            check(bz.bandCube == 0 && bz.band2D == 0 && bz.band3D == 0,
                  "H1b-6 KILLER: a cube slot with NO real cube index publishes "
                  "the dummy in the cube band and still zeroes the others. It "
                  "degrades to the pre-slice picture, never to a wrong one");

            // ── H1b-7. DIMENSION COMES FROM THE TABLES, NOT THE SHADOW ─────
            check(CB::dimFromTables(true, false) == CB::Dim::Cube,
                  "H1b-7: a key owned by the cube table binds as a cube");
            check(CB::dimFromTables(false, true) == CB::Dim::ThreeD,
                  "H1b-7: a key owned by the volume table binds as 3D");
            check(CB::dimFromTables(false, false) == CB::Dim::TwoD,
                  "H1b-7: everything else binds as 2D");
            check(CB::dimFromTables(true, true) == CB::Dim::Cube,
                  "H1b-7 KILLER: when BOTH tables claim a key, cube wins. A "
                  "key is only ever created into one of them, so the other is "
                  "the stale entry - and this is precisely the staleness the "
                  "band zeroing exists to contain");

            // ── H1b-10. THE REAL CAPTURED CUBES ARE ADMITTED ───────────────
            // \u26d4 THE PRE-CAPTURE CHECK. The director authorized exactly ONE
            // fresh solo K=1 capture for this slice. If the fail-closed
            // gauntlet refused the cubes the guest actually creates, that roll
            // would be spent proving nothing - so the four cube keys already
            // decoded from the FROZEN fixture
            // (archive/iter456_title_fresh.srht) are run through the REAL
            // rules here, before the roll rather than after it.
            //
            // All four are the same shape, reported by
            // `tools/check_cube_layout.py <trace>`:
            //   0x03050000 (slots 0-3, 1680 binds), 0x0314a000 (slots 4/6,
            //   1263), 0x1319c000 (slot 0, 19), 0x1f1f0000 (slot 3, 398)
            //   128x128, fmt 6 k_8_8_8_8, tiled, endian 2, pitch 128,
            //   mips 0..7, packed, face stride 65,536, base span 393,216,
            //   mip region 147,456.
            //
            // This asserts ADMISSION, not correctness of pixels. A cube that
            // is admitted can still be laid out wrongly; what it cannot be is
            // silently absent.
            {
                const uint32_t kW = 128, kH = 128, kFmt = 6, kPitch = 128;
                const uint32_t kMips = 8;
                const uint64_t kBaseBytes = 393216, kMipBytes = 147456;

                const auto rp = CB::planResource(CB::Dim::Cube, kW, kH, kMips,
                                                 /*formatSupported=*/true);
                check(rp.ok,
                      "H1b-10 KILLER: the REAL captured cube shape (128x128 "
                      "k_8_8_8_8, 8 mips) is ADMITTED by planResource. If this "
                      "ever refuses, the one authorized K=1 capture would be "
                      "spent proving nothing");
                check(rp.depthOrArraySize == 6 && rp.mipLevels == 8 &&
                          rp.subresourceCount == 48,
                      "H1b-10: ...as six array slices with eight mips = 48 "
                      "subresources");

                const auto L = cube::computeCubeLayout(kFmt, kW, kH, kPitch,
                                                       /*tiled=*/true, kMips,
                                                       /*packedMips=*/true,
                                                       1, 1, 4);
                check(L.ok && L.faceStrideBytes == 65536 &&
                          L.baseSpanBytes == 393216 &&
                          L.mipsTotalExtentBytes == 147456,
                      "H1b-10 KILLER: the C++ layout reproduces the frozen "
                      "fixture's own numbers - 65,536 / 393,216 / 147,456");

                // The executor hands hleUploadCube `a.bytes` as the base span
                // and `a.mipBytes` as the mip region, with the mip bytes
                // staged immediately after the base in ONE buffer. Assert the
                // preconditions that call site relies on, so a recorder change
                // that splits or reorders them fails HERE rather than as a
                // wrong picture.
                check(kBaseBytes == uint64_t(L.faceStrideBytes) * 6u,
                      "H1b-10 KILLER: the captured base span is EXACTLY six "
                      "face strides - the conservation identity the whole H1(a) "
                      "layout arc was built to establish");
                check(kMipBytes >= L.mipsTotalExtentBytes,
                      "H1b-10: the captured mip region covers the layout's "
                      "reported extent");
                check(L.mipOffsetBytes[L.lastStoredLevel] +
                          uint64_t(L.mipSliceStride[L.lastStoredLevel]) * 6u
                          <= kMipBytes,
                      "H1b-10 KILLER: the LAST stored level's six faces fit "
                      "inside the captured mip bytes. This is the read that "
                      "would run off the end of the staged buffer if the "
                      "packed-tail offset were wrong");

                const auto uu = CB::planUpload(rp, 6, 8, kBaseBytes, kBaseBytes);
                check(uu.ok && uu.subresources == 48,
                      "H1b-10 KILLER: the real cube passes the fail-closed "
                      "upload gauntlet with all 48 subresources. Every one of "
                      "the four captured keys has this identical shape, so "
                      "this covers all of them");
            }

            // ── H1b-11. ALL 48 PLACEMENTS OF THE REAL CUBE ─────────────────
            // \u26d4 THE FIXTURE THAT WOULD HAVE CAUGHT r1. r1's backend looped
            // mip 0..7 and refused every level above lastStoredLevel (3) as
            // MipDataMissing - so the ONLY cube shape the guest creates was
            // refused on mip 4, EVERY TIME. Every synthetic test passed,
            // because they stopped at layout totals and planUpload and never
            // drove the per-subresource loop. The director reproduced the
            // refusal on a device.
            //
            // Packed levels do not lack storage. Levels 3..7 SHARE the tail
            // slice stored at level 3, each at its own Xenos (x, y). This
            // walks all 6 x 8 placements and requires every one to resolve.
            {
                const uint32_t kW = 128, kH = 128, kMips = 8;
                const auto L = cube::computeCubeLayout(6, kW, kH, 128, true,
                                                       kMips, true, 1, 1, 4);
                check(L.ok && L.packedLevel == 3 && L.lastStoredLevel == 3,
                      "H1b-11: the real shape has its packed tail at level 3, "
                      "and only levels 1..3 are separately stored");

                uint32_t resolved = 0, packedSubs = 0;
                bool allOk = true;
                for (uint32_t f = 0; f < 6; f++) {
                    for (uint32_t m = 0; m < kMips; m++) {
                        const auto pl = cube::placeSubresource(L, kW, kH, 1, 1,
                                                               f, m);
                        if (!pl.ok) { allOk = false; continue; }
                        resolved++;
                        if (pl.region == cube::SubResRegion::PackedTail)
                            packedSubs++;
                    }
                }
                check(allOk && resolved == 48,
                      "H1b-11 KILLER: ALL 48 placements resolve. r1 resolved 24 "
                      "and refused the rest, which is the whole feature being "
                      "dead on the only data that exists");
                check(packedSubs == 30,
                      "H1b-11 KILLER: levels 3..7 across six faces = 30 "
                      "subresources come from the PACKED TAIL, not from "
                      "separate storage. If this is 0 the packed arm is unused "
                      "and mip 4 is about to be refused again");

                // Face 0's placements, by region and offset.
                const auto m0 = cube::placeSubresource(L, kW, kH, 1, 1, 0, 0);
                check(m0.region == cube::SubResRegion::Base &&
                          m0.byteOffset == 0 && m0.width == 128,
                      "H1b-11: face 0 mip 0 is the base span at offset 0");
                const auto m1 = cube::placeSubresource(L, kW, kH, 1, 1, 0, 1);
                check(m1.region == cube::SubResRegion::Mip &&
                          m1.byteOffset == 0 && m1.width == 64 &&
                          m1.availBytes == 16384,
                      "H1b-11: face 0 mip 1 is separately stored at mip-region "
                      "offset 0, 64x64, 16,384 B");
                const auto m4 = cube::placeSubresource(L, kW, kH, 1, 1, 0, 4);
                check(m4.ok && m4.region == cube::SubResRegion::PackedTail,
                      "H1b-11 KILLER: MIP 4 RESOLVES. This is the exact "
                      "subresource r1 refused as MipDataMissing on every real "
                      "cube");
                checkf(m4.width == 8 && m4.height == 8 &&
                           m4.tailWidth == 32 && m4.tailHeight == 16,
                       "H1b-11 KILLER: mip 4 is an 8x8 sub-rect of a 32x16 TAIL "
                       "SLICE (got %ux%u in %ux%u tail, layout extent %ux%u). "
                       "The tail is WIDER than the 16x16 level stored in it, "
                       "because its sublevels sit beside it - reading the tail "
                       "at the level's own size would miss every sublevel and "
                       "leave the distant mips as garbage",
                       m4.width, m4.height, m4.tailWidth, m4.tailHeight,
                       L.packedTailExtentXBlocks, L.packedTailExtentYBlocks);
                check(m4.byteOffset == L.mipOffsetBytes[3],
                      "H1b-11 KILLER: ...and it reads the LEVEL-3 byte offset, "
                      "because that is where the shared tail is stored. Using "
                      "mip 4's own offset would read past the region");
                const auto m7 = cube::placeSubresource(L, kW, kH, 1, 1, 5, 7);
                check(m7.ok && m7.region == cube::SubResRegion::PackedTail &&
                          m7.width == 1 && m7.height == 1,
                      "H1b-11: the last face's 1x1 tail sublevel resolves too");

                // Faces are strided inside every region.
                const auto f3m1 = cube::placeSubresource(L, kW, kH, 1, 1, 3, 1);
                check(f3m1.byteOffset == uint64_t(L.mipSliceStride[1]) * 3u,
                      "H1b-11 KILLER: face 3's mip 1 is three slice strides in "
                      "- a placement that ignored `face` would put all six "
                      "faces on top of each other");
                const auto f2m4 = cube::placeSubresource(L, kW, kH, 1, 1, 2, 4);
                check(f2m4.byteOffset ==
                          uint64_t(L.mipOffsetBytes[3]) +
                          uint64_t(L.mipSliceStride[3]) * 2u,
                      "H1b-11 KILLER: a PACKED sublevel is strided by face too, "
                      "using the TAIL's stride");

                // The square arm of GetPackedMipOffset: 128x128 is square, so
                // sublevels advance in X until pm >= 3, then in Y.
                uint32_t x3 = 0, y3 = 0, x4 = 0, y4 = 0;
                const auto p3 = cube::placeSubresource(L, kW, kH, 1, 1, 0, 3);
                const auto p4 = cube::placeSubresource(L, kW, kH, 1, 1, 0, 4);
                x3 = p3.xBlocks; y3 = p3.yBlocks; x4 = p4.xBlocks; y4 = p4.yBlocks;
                // ⛔ THESE ARE THE AUTHORITY'S NUMBERS, cross-checked against
                // the independent Python port
                // (tools/check_cube_layout.py packed_mip_offset):
                //   mip 3 -> (16,0)  4 -> (8,0)  5 -> (4,0)
                //   mip 6 -> (0,8)   7 -> (0,4)
                // I first asserted mip 3 sat at (0,0) because that is where a
                // tail's own base level "obviously" belongs. It does not, and
                // the fixture caught the guess.
                check(x3 == 16 && y3 == 0,
                      "H1b-11 KILLER: the tail's base sublevel sits at (16,0), "
                      "not the origin");
                check(x4 == 8 && y4 == 0,
                      "H1b-11 KILLER: mip 4 sits at (8,0) - a DIFFERENT place "
                      "from mip 3. Placing both at the origin would overwrite "
                      "one with the other and render as subtly wrong rather "
                      "than obviously broken");
                const auto p6 = cube::placeSubresource(L, kW, kH, 1, 1, 0, 6);
                check(p6.xBlocks == 0 && p6.yBlocks == 8,
                      "H1b-11 KILLER: at pm >= 3 the packing AXIS FLIPS to Y - "
                      "mip 6 is at (0,8). This is the branch a square texture "
                      "hides until the tail runs deep enough, and it is why "
                      "the H1(a) port kept the else-arm comment");

                // An out-of-range request is refused, not clamped.
                check(!cube::placeSubresource(L, kW, kH, 1, 1, 6, 0).ok,
                      "H1b-11: face 6 does not exist and is refused");
                check(!cube::placeSubresource(L, kW, kH, 1, 1, 0, 8).ok,
                      "H1b-11: mip 8 does not exist and is refused");
            }

            // ── H1b-12. packed_level == 0 TWO-TAIL + DISTINCT SENTINELS ────
            // An 8x8 square cube. packedMipLevel is 0, so Xenia stores TWO
            // tails: the BASE holding mip 0 at (16,0), and mips[0] holding
            // the chain. Distinct sentinels prove mip 0 is not read from the
            // mip region and is not at the origin of the base.
            {
                const uint32_t kW = 8, kH = 8, kMips = 4;
                const auto L = cube::computeCubeLayout(6, kW, kH, 32, true,
                                                       kMips, true, 1, 1, 4);
                check(L.ok && L.packedLevel == 0,
                      "H1b-12: an 8x8 packs from level 0");
                checkf(L.mipsTotalExtentBytes == 24576,
                       "H1b-12 KILLER: the mip region is 24,576 B, the number "
                       "the Python port has always reported and r2's C++ "
                       "fixture denied (got %u)", L.mipsTotalExtentBytes);
                check(L.mipSliceStride[0] == 4096 && L.mipOffsetBytes[0] == 0,
                      "H1b-12: the mip tail is stored as mips[0]");
                check(L.basePackedTailExtentXBlocks == 24 &&
                          L.basePackedTailExtentYBlocks == 8,
                      "H1b-12: the BASE tail is 24x8 (mip 0 at x=16 plus 8)");
                check(L.packedTailExtentXBlocks == 24 &&
                          L.packedTailExtentYBlocks == 8,
                      "H1b-12: the MIPS tail of an 8x8 with max_level 3 is "
                      "also 24x8 (mip 0's offset dominates)");

                const auto m0 = cube::placeSubresource(L, kW, kH, 1, 1, 0, 0);
                check(m0.ok && m0.region == cube::SubResRegion::BasePackedTail,
                      "H1b-12 KILLER: mip 0 is the BASE packed tail, not an "
                      "ordinary image at the origin");
                check(m0.xBlocks == 16 && m0.yBlocks == 0 &&
                          m0.width == 8 && m0.height == 8,
                      "H1b-12 KILLER: mip 0 sits at (16,0) of the base tail");
                check(m0.byteOffset == 0 && m0.availBytes == L.faceStrideBytes,
                      "H1b-12: ...and it is read from the BASE span");

                const auto m1 = cube::placeSubresource(L, kW, kH, 1, 1, 0, 1);
                check(m1.ok && m1.region == cube::SubResRegion::PackedTail,
                      "H1b-12 KILLER: mip 1 resolves from the MIP packed tail "
                      "- r2 refused it because mipSliceStride[0] was 0");
                check(m1.xBlocks == 8 && m1.yBlocks == 0 &&
                          m1.width == 4 && m1.height == 4,
                      "H1b-12: mip 1 sits at (8,0) of the mip tail, 4x4");
                check(m1.byteOffset == L.mipOffsetBytes[0],
                      "H1b-12: ...and it is read from the MIP region, not the "
                      "base. Mixing the two tails is a cube whose mip 0 and "
                      "mip 1 show each other's pixels");

                // Distinct sentinels: paint the decoded base tail with 0xA1
                // at (16,0) 8x8, and the mip tail with 0xB2 at (8,0) 4x4.
                // Extracting as the backend does must recover those values
                // and MUST NOT recover the other sentinel.
                //
                // ⛔ Guard the paint: check() does not return, so a mutant that
                // zeroes tailWidth (packedHere=false, or mip 0 taken as a
                // Base image) used to AV on putPx of a 0-byte vector and
                // score as "crashed, proves nothing". A mutation that cannot
                // FAIL is not a test.
                if (!(m0.ok && m0.tailWidth >= 24 && m0.tailHeight >= 8 &&
                      m1.ok && m1.tailWidth >= 24 && m1.tailHeight >= 8)) {
                    check(false,
                          "H1b-12 KILLER: both tails must have a 24x8 window "
                          "before sentinels can be painted. A mutant that "
                          "drops packed placement used to crash here instead "
                          "of failing the region checks above");
                } else {
                std::vector<uint8_t> baseLin(size_t(m0.tailWidth) *
                                             m0.tailHeight * 4u, 0x11);
                std::vector<uint8_t> mipLin(size_t(m1.tailWidth) *
                                            m1.tailHeight * 4u, 0x22);
                auto putPx = [](std::vector<uint8_t>& img, uint32_t w,
                                uint32_t x, uint32_t y, uint8_t v) {
                    const size_t o = (size_t(y) * w + x) * 4u;
                    if (o + 3 >= img.size()) return;
                    img[o] = img[o + 1] = img[o + 2] = img[o + 3] = v;
                };
                for (uint32_t y = 0; y < 8; y++)
                    for (uint32_t x = 0; x < 8; x++)
                        putPx(baseLin, m0.tailWidth, m0.xBlocks + x,
                              m0.yBlocks + y, 0xA1);
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++)
                        putPx(mipLin, m1.tailWidth, m1.xBlocks + x,
                              m1.yBlocks + y, 0xB2);
                auto extract = [](const std::vector<uint8_t>& img, uint32_t w,
                                  uint32_t x0, uint32_t y0, uint32_t sw,
                                  uint32_t sh) {
                    std::vector<uint8_t> out(size_t(sw) * sh * 4u, 0);
                    for (uint32_t y = 0; y < sh; y++) {
                        const size_t src = (size_t(y0 + y) * w + x0) * 4u;
                        const size_t n = size_t(sw) * 4u;
                        if (src + n > img.size()) break;
                        std::memcpy(out.data() + size_t(y) * n, img.data() + src, n);
                    }
                    return out;
                };
                const auto e0 = extract(baseLin, m0.tailWidth, m0.xBlocks,
                                        m0.yBlocks, 8, 8);
                const auto e1 = extract(mipLin, m1.tailWidth, m1.xBlocks,
                                        m1.yBlocks, 4, 4);
                bool allA = true, allB = true, aSawB = false, bSawA = false;
                for (uint8_t v : e0) {
                    if (v != 0xA1) allA = false;
                    if (v == 0xB2) aSawB = true;
                }
                for (uint8_t v : e1) {
                    if (v != 0xB2) allB = false;
                    if (v == 0xA1) bSawA = true;
                }
                check(allA && !aSawB,
                      "H1b-12 KILLER: extracting mip 0 from the BASE tail at "
                      "(16,0) yields only the base sentinel 0xA1");
                check(allB && !bSawA,
                      "H1b-12 KILLER: extracting mip 1 from the MIP tail at "
                      "(8,0) yields only the mip sentinel 0xB2");
                }
            }

            // ── H1b-13. FAILURES NEVER CONSUME A SLOT; POST-SUBMIT CLASSIFICATION
            {
                namespace TX = sr::gfx::cubetx;
                uint32_t mask = TX::kFreeMaskAll;
                for (int i = 0; i < 8; i++) {
                    CB::CubeRefuse why = CB::CubeRefuse::None;
                    const uint32_t idx = TX::slotAcquire(mask, why);
                    check(why == CB::CubeRefuse::None && idx == 1,
                          "H1b-13 KILLER: eight FAILURES each take and RETURN the "
                          "same candidate - a failure never consumes a slot");
                    TX::slotRelease(mask, idx);   // what every failure path does
                }
                check(mask == TX::kFreeMaskAll,
                      "H1b-13 KILLER: after eight failed attempts all seven indices "
                      "are still free, so the ninth cannot return NoSlot");
                {
                    CB::CubeRefuse why = CB::CubeRefuse::None;
                    check(TX::slotAcquire(mask, why) == 1
                              && TX::slotFreeCount(mask) == 6,
                          "H1b-13: a successful publication keeps its slot");
                }

                check(CB::classifySubmit(0, 0, 0, true) == CB::SubmitStatus::Ok,
                      "H1b-13: all-success is Ok");
                check(CB::classifySubmit((int32_t)0x80004005, 0, 0, true) ==
                          CB::SubmitStatus::CloseFailed,
                      "H1b-13 KILLER: a failed Close is named CloseFailed - "
                      "r2 ignored the HRESULT and published anyway");
                check(CB::classifySubmit(0, (int32_t)0x80004005, 0, true) ==
                          CB::SubmitStatus::SignalFailed,
                      "H1b-13 KILLER: a failed Signal is named, not swallowed");
                check(CB::classifySubmit(0, 0, (int32_t)0x80004005, true) ==
                          CB::SubmitStatus::EventFailed,
                      "H1b-13: a failed SetEventOnCompletion is named");
                check(CB::classifySubmit(0, 0, 0, false) ==
                          CB::SubmitStatus::WaitFailed,
                      "H1b-13 KILLER: a wait timeout is WaitFailed - r2 "
                      "returned true after WaitForSingleObject timed out");
                check(std::string(CB::submitStatusName(
                          CB::SubmitStatus::WaitFailed)) == "WaitFailed",
                      "H1b-13: every submit failure has a loggable name");
                // r3: submit failures propagate their EXACT category through
                // the caller boundary, not collapsed to ByteCountMismatch.
                check(CB::submitToRefuse(CB::SubmitStatus::CloseFailed) ==
                          CB::CubeRefuse::CloseFailed,
                      "H1b-13 KILLER: CloseFailed maps to CubeRefuse::CloseFailed, "
                      "not ByteCountMismatch");
                check(CB::submitToRefuse(CB::SubmitStatus::SignalFailed) ==
                          CB::CubeRefuse::SignalFailed,
                      "H1b-13 KILLER: SignalFailed preserved through boundary");
                check(CB::submitToRefuse(CB::SubmitStatus::EventFailed) ==
                          CB::CubeRefuse::EventFailed,
                      "H1b-13: EventFailed preserved");
                check(CB::submitToRefuse(CB::SubmitStatus::WaitFailed) ==
                          CB::CubeRefuse::WaitFailed,
                      "H1b-13: WaitFailed preserved");
                check(std::string(CB::refuseName(CB::CubeRefuse::CloseFailed))
                          == "CloseFailed",
                      "H1b-13: the new refuse reasons have loggable names");
                check(std::string(CB::refuseName(CB::CubeRefuse::WaitFailed))
                          == "WaitFailed",
                      "H1b-13: ...for all four submit failure categories");
            }

            // ── H1b-14. decodeGuestImage BYTE-EQUIVALENT TO PRE-EXTRACT ────
            // Frozen copy of the inlined untiler as it lived in
            // hleUploadHostTextureFmt at 897ece7^ (the commit that extracted
            // it). A drift here is a 2D texture that looks like an art bug.
            {
                auto preExtract = [](const uint8_t* src, uint64_t srcBytes,
                                     uint32_t blockW, uint32_t blockBytes,
                                     uint32_t width, uint32_t height,
                                     uint32_t pitchTexels, bool tiled,
                                     uint32_t endian, std::vector<uint8_t>& lin,
                                     bool& truncated) -> bool {
                    const uint32_t rowBlocks = (width + blockW - 1u) / blockW;
                    const uint32_t rows = (height + blockW - 1u) / blockW;
                    const uint32_t rowBytes = rowBlocks * blockBytes;
                    if (!rowBytes || !rows) return false;
                    uint32_t blockPitch = pitchTexels / blockW;
                    if (tiled) blockPitch = (blockPitch + 31u) & ~31u;
                    if (!blockPitch) blockPitch = rowBlocks;
                    lin.assign(size_t(rows) * rowBytes, 0);
                    truncated = false;
                    if (tiled) {
                        uint32_t bpbLog2 = 0;
                        while ((1u << bpbLog2) < blockBytes) bpbLog2++;
                        for (uint32_t by = 0; by < rows; by++) {
                            uint8_t* drow = lin.data() + size_t(by) * rowBytes;
                            for (uint32_t bx = 0; bx < rowBlocks; bx++) {
                                const int32_t off = sr::gfx::xenos_tiling::tiled2D(
                                    int32_t(bx), int32_t(by), blockPitch, bpbLog2);
                                if (off < 0 ||
                                    uint64_t(off) + blockBytes > srcBytes) {
                                    truncated = true; continue; }
                                std::memcpy(drow + size_t(bx) * blockBytes,
                                            src + off, blockBytes);
                            }
                        }
                    } else {
                        const uint32_t srcPitch = blockPitch * blockBytes;
                        for (uint32_t y = 0; y < rows; y++) {
                            const uint64_t off = uint64_t(y) * srcPitch;
                            if (off + rowBytes > srcBytes) {
                                truncated = true; break; }
                            std::memcpy(lin.data() + size_t(y) * rowBytes,
                                        src + off, rowBytes);
                        }
                    }
                    if (endian) {
                        uint32_t* p = reinterpret_cast<uint32_t*>(lin.data());
                        for (size_t i = 0; i < lin.size() / 4; i++) {
                            uint32_t v = p[i];
                            switch (endian & 3u) {
                            case 1:
                                v = ((v & 0xFF00FF00u) >> 8) |
                                    ((v & 0x00FF00FFu) << 8);
                                break;
                            case 2:
                                v = _byteswap_ulong(v);
                                break;
                            case 3:
                                v = (v >> 16) | (v << 16);
                                break;
                            default:
                                break;
                            }
                            p[i] = v;
                        }
                    }
                    return true;
                };
                auto roundTrip = [&](uint32_t w, uint32_t h, bool tiled,
                                     uint32_t endian) {
                    const uint32_t pitch = tiled ? ((w + 31u) & ~31u) : w;
                    std::vector<uint8_t> src(size_t(pitch) * h * 4u, 0);
                    for (size_t i = 0; i < src.size(); i++)
                        src[i] = uint8_t(i * 17u + 3u);
                    sr::gfx::DecodedImage live;
                    const bool liveOk = sr::gfx::decodeGuestImage(
                        src.data(), src.size(), 1, 4, w, h, pitch, tiled,
                        endian, live);
                    std::vector<uint8_t> frozen;
                    bool trunc = false;
                    const bool frozenOk = preExtract(
                        src.data(), src.size(), 1, 4, w, h, pitch, tiled,
                        endian, frozen, trunc);
                    check(liveOk && frozenOk,
                          "H1b-14: both decoders accept the fixture");
                    check(live.lin == frozen && live.truncated == trunc,
                          "H1b-14 KILLER: decodeGuestImage is BYTE-IDENTICAL "
                          "to the pre-extract inlined untiler");
                };
                roundTrip(32, 32, true, 2);
                roundTrip(8, 8, true, 2);
                roundTrip(16, 4, false, 0);
                roundTrip(64, 16, true, 1);
            }

            // ── H1b-15. THE TRANSACTION ON A REAL DEVICE (WARP) ────────────
            // ⛔ r4 was rejected because nothing observed D3D12 OBJECT behaviour:
            // which allocator was Reset, which slot received a descriptor,
            // which resource stayed published. Every assertion below drives
            // cubetx::CubeTransactor - the SAME body D3D12Backend::hleUploadCube
            // calls - against a WARP device and reads identities back. A
            // missing device FAILS this block; it never skips.
            {
                namespace TX = sr::gfx::cubetx;
                using Microsoft::WRL::ComPtr;
                ComPtr<IDXGIFactory4> factory;
                ComPtr<IDXGIAdapter> warp;
                ComPtr<ID3D12Device> dev;
                ComPtr<ID3D12CommandQueue> queue;
                ComPtr<ID3D12DescriptorHeap> heap;
                bool devOk = SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
                if (devOk) devOk = SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
                if (devOk) devOk = SUCCEEDED(D3D12CreateDevice(
                    warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
                if (devOk) {
                    D3D12_COMMAND_QUEUE_DESC qd{};
                    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                    devOk = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
                }
                if (devOk) {
                    D3D12_DESCRIPTOR_HEAP_DESC hd{};
                    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                    hd.NumDescriptors = TX::kSlotCount;
                    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                    devOk = SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
                }
                check(devOk,
                      "H1b-15 PRECONDITION: a WARP D3D12 device, direct queue and "
                      "shader-visible heap exist. A skipped fixture is NOT a pass, so "
                      "a missing device FAILS here");
                if (devOk) {
                    const UINT inc = dev->GetDescriptorHandleIncrementSize(
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    const D3D12_CPU_DESCRIPTOR_HANDLE slot0 =
                        heap->GetCPUDescriptorHandleForHeapStart();
                    // A 4x4 two-mip RGBA8 cube: 12 subresources, one per (face, mip).
                    auto makeSubs = [](uint8_t seed) {
                        std::vector<TX::StagedSub> v;
                        for (uint32_t face = 0; face < CB::kCubeFaces; face++)
                            for (uint32_t mip = 0; mip < 2; mip++) {
                                TX::StagedSub s;
                                s.w = s.h = 4u >> mip;
                                s.rows = s.h; s.rowBytes = s.w * 4u;
                                s.index = CB::subresourceIndex(face, mip, 2);
                                s.lin.assign(size_t(s.rows) * s.rowBytes,
                                             uint8_t(seed + face * 16u + mip));
                                v.push_back(std::move(s));
                            }
                        return v;
                    };
                    auto request = [&](uint32_t key, const std::vector<TX::StagedSub>& subs,
                                       TX::Fault f, int failAt, uint64_t gfxFence) {
                        TX::UploadRequest rq;
                        rq.key = key; rq.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
                        rq.width = rq.height = 4;
                        rq.depthOrArraySize = (uint16_t)CB::kCubeFaces; rq.mipLevels = 2;
                        rq.subs = subs.data(); rq.count = (uint32_t)subs.size();
                        rq.graphicsFenceNow = gfxFence;
                        rq.renderThreadOwnsPresent = true;   // the fixture IS the presenter
                        rq.fault = f; rq.failAtSubresource = failAt; rq.waitMs = 10000;
                        return rq;
                    };

                    // ── THE CONSUMPTION PROBE (r6) ──────────────────────────
                    // A real pixel shader samples WHATEVER descriptor sits in a
                    // heap slot - the production path: TEXTURECUBE SRV through
                    // the shader-visible CBV_SRV_UAV heap, resource at rest in
                    // PIXEL_SHADER_RESOURCE - and writes one texel per face
                    // into a 6x1 target the fixture reads back. srvWrites and
                    // the wiring test's text checks are supporting evidence;
                    // THIS is what proves a slot resolves to a resource.
                    static const char* const kProbeHlsl =
                        "TextureCube<float4> cube : register(t0);\n"
                        "SamplerState samp : register(s0);\n"
                        "float4 vsmain(uint vid : SV_VertexID) : SV_Position {\n"
                        "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
                        "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
                        "}\n"
                        "float4 psmain(float4 pos : SV_Position) : SV_Target {\n"
                        "  uint x = (uint)pos.x;\n"
                        "  float3 d = x == 0 ? float3(1,0,0) : x == 1 ? float3(-1,0,0)\n"
                        "           : x == 2 ? float3(0,1,0) : x == 3 ? float3(0,-1,0)\n"
                        "           : x == 4 ? float3(0,0,1) : float3(0,0,-1);\n"
                        "  return cube.SampleLevel(samp, d, 0);\n"
                        "}\n";
                    ComPtr<ID3D12RootSignature> probeRs;
                    ComPtr<ID3D12PipelineState> probePso;
                    ComPtr<ID3D12Resource> probeRt, probeRead;
                    ComPtr<ID3D12DescriptorHeap> probeRtvHeap;
                    ComPtr<ID3D12CommandAllocator> probeAlloc;
                    ComPtr<ID3D12GraphicsCommandList> probeList;
                    ComPtr<ID3D12Fence> probeFence;
                    HANDLE probeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    UINT64 probeFv = 0;
                    bool probeOk = probeEvent != nullptr;
                    const char* probeStage = "event";
                    {
                        D3D12_DESCRIPTOR_RANGE range{};
                        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                        range.NumDescriptors = 1;
                        D3D12_ROOT_PARAMETER param{};
                        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                        param.DescriptorTable.NumDescriptorRanges = 1;
                        param.DescriptorTable.pDescriptorRanges = &range;
                        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                        D3D12_STATIC_SAMPLER_DESC ss{};
                        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                        ss.AddressU = ss.AddressV = ss.AddressW =
                            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        ss.MaxLOD = D3D12_FLOAT32_MAX;
                        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                        D3D12_ROOT_SIGNATURE_DESC rs{};
                        rs.NumParameters = 1; rs.pParameters = &param;
                        rs.NumStaticSamplers = 1; rs.pStaticSamplers = &ss;
                        ComPtr<ID3DBlob> sig, err, vs, ps;
                        if (probeOk) { probeStage = "root signature";
                            probeOk = SUCCEEDED(D3D12SerializeRootSignature(
                                &rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))
                                && SUCCEEDED(dev->CreateRootSignature(
                                    0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                    IID_PPV_ARGS(&probeRs))); }
                        if (probeOk) { probeStage = "shaders";
                            probeOk = SUCCEEDED(D3DCompile(kProbeHlsl, strlen(kProbeHlsl),
                                "cubeprobe.hlsl", nullptr, nullptr, "vsmain", "vs_5_0",
                                0, 0, &vs, &err))
                                && SUCCEEDED(D3DCompile(kProbeHlsl, strlen(kProbeHlsl),
                                "cubeprobe.hlsl", nullptr, nullptr, "psmain", "ps_5_0",
                                0, 0, &ps, &err)); }
                        if (probeOk) { probeStage = "pipeline";
                            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                            pd.pRootSignature = probeRs.Get();
                            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
                            pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
                            pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
                                D3D12_COLOR_WRITE_ENABLE_ALL;
                            pd.SampleMask = UINT_MAX;
                            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                            pd.RasterizerState.DepthClipEnable = TRUE;
                            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                            pd.NumRenderTargets = 1;
                            pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                            pd.SampleDesc.Count = 1;
                            probeOk = SUCCEEDED(dev->CreateGraphicsPipelineState(
                                &pd, IID_PPV_ARGS(&probePso))); }
                        if (probeOk) { probeStage = "target + readback";
                            D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
                            D3D12_RESOURCE_DESC rd{};
                            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                            rd.Width = 6; rd.Height = 1; rd.DepthOrArraySize = 1;
                            rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            rd.SampleDesc.Count = 1;
                            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                            D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            cv.Color[0] = cv.Color[1] = cv.Color[2] = cv.Color[3] = 237.0f / 255.0f;
                            probeOk = SUCCEEDED(dev->CreateCommittedResource(
                                &def, D3D12_HEAP_FLAG_NONE, &rd,
                                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                IID_PPV_ARGS(&probeRt)));
                            D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
                            D3D12_RESOURCE_DESC bd{};
                            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                            bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1;
                            bd.MipLevels = 1; bd.SampleDesc.Count = 1;
                            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                            probeOk = probeOk && SUCCEEDED(dev->CreateCommittedResource(
                                &rb, D3D12_HEAP_FLAG_NONE, &bd,
                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                IID_PPV_ARGS(&probeRead)));
                            D3D12_DESCRIPTOR_HEAP_DESC hd{};
                            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
                            probeOk = probeOk && SUCCEEDED(dev->CreateDescriptorHeap(
                                &hd, IID_PPV_ARGS(&probeRtvHeap)));
                            if (probeOk)
                                dev->CreateRenderTargetView(probeRt.Get(), nullptr,
                                    probeRtvHeap->GetCPUDescriptorHandleForHeapStart()); }
                        if (probeOk) { probeStage = "command objects";
                            probeOk = SUCCEEDED(dev->CreateCommandAllocator(
                                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probeAlloc)))
                                && SUCCEEDED(dev->CreateCommandList(
                                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, probeAlloc.Get(),
                                    nullptr, IID_PPV_ARGS(&probeList)))
                                && SUCCEEDED(probeList->Close())
                                && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&probeFence))); }
                    }
                    checkf(probeOk,
                           "H1b-15 PRECONDITION: the WARP consumption probe exists (failed "
                           "at: %s). Descriptor consumption is measured by a real shader "
                           "or this line FAILS - it never skips", probeStage);
                    // Samples slot `slot` of the transaction's heap through the probe;
                    // out[f] = the R byte the shader read from face f. 0xEE = not run.
                    auto sampleSlot = [&](uint32_t slot, uint8_t out[6]) -> bool {
                        for (int i = 0; i < 6; i++) out[i] = 0xEE;
                        if (!probeOk) return false;
                        if (FAILED(probeAlloc->Reset()) ||
                            FAILED(probeList->Reset(probeAlloc.Get(), probePso.Get())))
                            return false;
                        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
                        probeList->SetDescriptorHeaps(1, heaps);
                        probeList->SetGraphicsRootSignature(probeRs.Get());
                        D3D12_GPU_DESCRIPTOR_HANDLE g = heap->GetGPUDescriptorHandleForHeapStart();
                        g.ptr += UINT64(slot) * inc;
                        probeList->SetGraphicsRootDescriptorTable(0, g);
                        D3D12_VIEWPORT vp{0.0f, 0.0f, 6.0f, 1.0f, 0.0f, 1.0f};
                        D3D12_RECT sc{0, 0, 6, 1};
                        probeList->RSSetViewports(1, &vp);
                        probeList->RSSetScissorRects(1, &sc);
                        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                            probeRtvHeap->GetCPUDescriptorHandleForHeapStart();
                        const float clear[4] = {237.0f / 255.0f, 237.0f / 255.0f,
                                                237.0f / 255.0f, 237.0f / 255.0f};
                        probeList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                        probeList->ClearRenderTargetView(rtv, clear, 0, nullptr);
                        probeList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        probeList->DrawInstanced(3, 1, 0, 0);
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = probeRt.Get();
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        probeList->ResourceBarrier(1, &b);
                        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                        dst.pResource = probeRead.Get();
                        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        dst.PlacedFootprint.Footprint.Width = 6;
                        dst.PlacedFootprint.Footprint.Height = 1;
                        dst.PlacedFootprint.Footprint.Depth = 1;
                        dst.PlacedFootprint.Footprint.RowPitch = 256;
                        src.pResource = probeRt.Get();
                        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        src.SubresourceIndex = 0;
                        probeList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                        probeList->ResourceBarrier(1, &b);
                        if (FAILED(probeList->Close())) return false;
                        ID3D12CommandList* lists[] = {probeList.Get()};
                        queue->ExecuteCommandLists(1, lists);
                        const UINT64 fv = ++probeFv;
                        if (FAILED(queue->Signal(probeFence.Get(), fv))) return false;
                        if (probeFence->GetCompletedValue() < fv) {
                            if (FAILED(probeFence->SetEventOnCompletion(fv, probeEvent)) ||
                                WaitForSingleObject(probeEvent, 10000) != WAIT_OBJECT_0)
                                return false;
                        }
                        uint8_t* mp = nullptr;
                        D3D12_RANGE rr{0, 24};
                        if (FAILED(probeRead->Map(0, &rr, reinterpret_cast<void**>(&mp))))
                            return false;
                        for (int i = 0; i < 6; i++) out[i] = mp[i * 4];   // R; the sentinel fills RGBA
                        D3D12_RANGE nw{0, 0};
                        probeRead->Unmap(0, &nw);
                        return true;
                    };
                    // makeSubs(seed) fills face f (mip 0) with seed + 16 f, so the six
                    // R bytes the shader reads through a slot name the resource.
                    auto checkSample = [&](uint32_t slot, uint8_t seed, const char* what) {
                        uint8_t got[6];
                        const bool ran = sampleSlot(slot, got);
                        bool match = ran;
                        for (uint32_t f = 0; f < 6 && match; f++)
                            match = got[f] == uint8_t(seed + f * 16u);
                        // POSITIVE WITNESS: the suite prints only failures, so the
                        // values a real shader read back are logged here.
                        SR_LOG_BOOT(sr::LogSeverity::Info,
                            "[H1B15_READBACK] slot=%u R=%u,%u,%u,%u,%u,%u wantSeed=%u match=%d",
                            slot, (unsigned)got[0], (unsigned)got[1], (unsigned)got[2],
                            (unsigned)got[3], (unsigned)got[4], (unsigned)got[5],
                            (unsigned)seed, match ? 1 : 0);
                        checkf(match,
                               "%s [READBACK slot %u faces R=%u,%u,%u,%u,%u,%u; want seed %u "
                               "-> %u,%u,%u,%u,%u,%u]", what, slot,
                               (unsigned)got[0], (unsigned)got[1], (unsigned)got[2],
                               (unsigned)got[3], (unsigned)got[4], (unsigned)got[5],
                               (unsigned)seed, (unsigned)uint8_t(seed),
                               (unsigned)uint8_t(seed + 16u), (unsigned)uint8_t(seed + 32u),
                               (unsigned)uint8_t(seed + 48u), (unsigned)uint8_t(seed + 64u),
                               (unsigned)uint8_t(seed + 80u));
                    };
                    // Null-safe on purpose: when an earlier assertion has already
                    // failed (no publication), the fixture must keep FAILING, not
                    // crash - a crash scores as "proves nothing" in the harness.
                    auto refCount = [](IUnknown* u) -> ULONG {
                        if (!u) return 0;
                        u->AddRef(); return u->Release();
                    };
                    auto waitReal = [](const std::shared_ptr<TX::UploadContext>& c) -> bool {
                        if (!c || !c->fence) return false;
                        if (c->fence->GetCompletedValue() >= c->fenceValue) return true;
                        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                        bool ok = ev && SUCCEEDED(c->fence->SetEventOnCompletion(c->fenceValue, ev))
                                  && WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
                        if (ev) CloseHandle(ev);
                        return ok;
                    };
                    const auto subsA = makeSubs(1), subsB = makeSubs(101);
                    TX::CubeTransactor tx;
                    CB::CubeRefuse why = CB::CubeRefuse::None;
                    auto go = [&](uint32_t key, const std::vector<TX::StagedSub>& subs,
                                  TX::Fault f, int failAt, uint64_t gfxFence) {
                        tx.beginAttempt();
                        return tx.upload(dev.Get(), queue.Get(), slot0, inc,
                                         request(key, subs, f, failAt, gfxFence), why);
                    };

                    // (a) FIRST PUBLICATION
                    const uint32_t idxA = go(0x100, subsA, TX::Fault::None, -1, 1);
                    check(idxA == 1 && why == CB::CubeRefuse::None,
                          "H1b-15a: the first real cube publishes at index 1");
                    check(tx.slotForKey(0x100) == 1, "H1b-15a: the key resolves to slot 1");
                    check(tx.srvWrites(1) == 1 && tx.srvWrites(0) == 0,
                          "H1b-15a KILLER: exactly one descriptor write landed, in slot 1, "
                          "none in the dummy slot");
                    TX::CubeEntry pubA;
                    check(tx.entry(0x100, pubA) && pubA.res && pubA.idx == 1,
                          "H1b-15a: a published entry holds a live resource");
                    check(tx.pool().activeAllocator() != nullptr,
                          "H1b-15a KILLER: after a PROVEN-complete submission the context "
                          "is active (idle) again - the only state in which acquire may "
                          "Reset it");
                    check(tx.pool().contextsCreated() == 1 && tx.pool().retiredCount() == 0
                              && tx.pool().lostCount() == 0,
                          "H1b-15a: one context, nothing retired, nothing lost");
                    check(TX::slotFreeCount(tx.freeMask()) == 6,
                          "H1b-15a: six indices remain free");
                    checkSample(1, 1,
                        "H1b-15a KILLER: a real shader sampling heap slot 1 reads resource "
                        "A's sentinels - the first publication is CONSUMED through its "
                        "shader-visible descriptor, not merely bookkept");

                    // (b) THE FOUR SAME-KEY FAULTS, each BEFORE the operation it models
                    struct FaultCase { TX::Fault f; CB::CubeRefuse want; const char* name; };
                    const FaultCase cases[] = {
                        {TX::Fault::Close,  CB::CubeRefuse::CloseFailed,  "close"},
                        {TX::Fault::Signal, CB::CubeRefuse::SignalFailed, "signal"},
                        {TX::Fault::Event,  CB::CubeRefuse::EventFailed,  "event"},
                        {TX::Fault::Wait,   CB::CubeRefuse::WaitFailed,   "wait"},
                    };
                    std::shared_ptr<TX::UploadContext> lostCtx;
                    std::shared_ptr<TX::UploadContext> retiredCtxs[2];
                    for (const auto& fc : cases) {
                        const uint32_t freeBefore = TX::slotFreeCount(tx.freeMask());
                        const uint32_t writes1Before = tx.srvWrites(1);
                        const size_t retiredBefore = tx.pool().retiredCount();
                        const size_t lostBefore = tx.pool().lostCount();
                        const uint64_t createdBefore = tx.pool().contextsCreated();
                        const uint32_t r = go(0x100, subsB, fc.f, -1, 2);
                        checkf(r == 0 && why == fc.want,
                               "H1b-15b[%s] KILLER: refused by its EXACT name (got idx=%u "
                               "why=%s)", fc.name, r, CB::refuseName(why));
                        TX::CubeEntry e;
                        checkf(tx.slotForKey(0x100) == 1 && tx.entry(0x100, e)
                                   && e.res.Get() == pubA.res.Get() && e.idx == 1,
                               "H1b-15b[%s] KILLER: the published (resource, slot) pair is "
                               "identity-for-identity unchanged", fc.name);
                        checkf(tx.srvWrites(1) == writes1Before,
                               "H1b-15b[%s] KILLER: the published shader-visible slot was "
                               "NOT rewritten", fc.name);
                        bool noStray = true;
                        for (uint32_t s = 2; s < TX::kSlotCount; s++)
                            noStray = noStray && tx.srvWrites(s) == 0;
                        checkf(noStray,
                               "H1b-15b[%s] KILLER: no descriptor was written into the "
                               "candidate slot of a failed transaction", fc.name);
                        checkf(TX::slotFreeCount(tx.freeMask()) == freeBefore,
                               "H1b-15b[%s] KILLER: the candidate slot went back to the "
                               "free list - a failure never consumes a slot", fc.name);
                        {
                            char what[160];
                            std::snprintf(what, sizeof(what),
                                "H1b-15b[%s] KILLER: after the failed refresh the shader "
                                "still reads resource A through slot 1 (B's sentinels never "
                                "became visible)", fc.name);
                            checkSample(1, 1, what);
                        }
                        switch (fc.f) {
                        case TX::Fault::Close:
                            check(tx.pool().retiredCount() == retiredBefore
                                      && tx.pool().lostCount() == lostBefore,
                                  "H1b-15b[close] KILLER: nothing executed, so nothing is "
                                  "retired or quarantined");
                            check(tx.pool().activeAllocator() == nullptr,
                                  "H1b-15b[close]: the context that failed to Close is "
                                  "DISCARDED, not kept for reuse");
                            break;
                        case TX::Fault::Signal:
                            check(tx.pool().lostCount() == lostBefore + 1
                                      && tx.pool().retiredCount() == retiredBefore,
                                  "H1b-15b[signal] KILLER: executing work with NO token is "
                                  "QUARANTINED in the lost list, never retired behind a "
                                  "fence nobody signalled");
                            lostCtx = tx.pool().lostCtx(lostBefore);
                            check(lostCtx && lostCtx->serial == createdBefore + 1
                                      && lostCtx->submissions == 1 && lostCtx->resets == 1,
                                  "H1b-15b[signal] KILLER: the quarantined context IS the "
                                  "one that executed (fresh serial, one submission, one "
                                  "Reset at creation)");
                            check(tx.pool().activeAllocator() == nullptr,
                                  "H1b-15b[signal] KILLER: the quarantined allocator is not "
                                  "left active for the next acquire to Reset");
                            break;
                        case TX::Fault::Event:
                        case TX::Fault::Wait: {
                            check(tx.pool().retiredCount() == retiredBefore + 1
                                      && tx.pool().lostCount() == lostBefore,
                                  "H1b-15b[event/wait] KILLER: Signal succeeded, so the work "
                                  "is RETIRED behind its fence value rather than quarantined");
                            auto rc = tx.pool().retiredCtx(retiredBefore);
                            check(rc && rc->serial == createdBefore + 1 && rc->submissions == 1
                                      && rc->resets == 1 && rc->fenceValue == 1
                                      && tx.pool().retiredFence(retiredBefore) == rc->fenceValue,
                                  "H1b-15b[event/wait] KILLER: the retired context is the one "
                                  "that executed and waits on the value it signalled");
                            check(tx.pool().activeAllocator() == nullptr,
                                  "H1b-15b[event/wait] KILLER: an unproven context is never "
                                  "returned to active");
                            retiredCtxs[fc.f == TX::Fault::Event ? 0 : 1] = rc;
                            break; }
                        default: break;
                        }
                    }

                    // (c) PENDING WORK IS NEVER RECYCLED. A fresh context serves the
                    //     next upload while the retired ones wait; drain frees them
                    //     only when the completion value says so.
                    tx.pool().setCompletedOverride(0);   // "nothing has completed"
                    check(tx.drainUploads() == 0 && tx.pool().retiredCount() == 2,
                          "H1b-15c KILLER: with completion held below both retired values, "
                          "drain releases NOTHING");
                    {
                        const uint64_t createdBefore = tx.pool().contextsCreated();
                        const uint32_t idxC = go(0x300, subsA, TX::Fault::None, -1, 3);
                        check(idxC == 2 && why == CB::CubeRefuse::None,
                              "H1b-15c: a clean upload succeeds while two contexts are pending");
                        check(tx.pool().contextsCreated() == createdBefore + 1,
                              "H1b-15c KILLER: it ran on a FRESH context, not a pending one");
                        ID3D12CommandAllocator* const active = tx.pool().activeAllocator();
                        check(active && retiredCtxs[0] && retiredCtxs[1] && lostCtx
                                  && active != retiredCtxs[0]->alloc.Get()
                                  && active != retiredCtxs[1]->alloc.Get()
                                  && active != lostCtx->alloc.Get(),
                              "H1b-15c KILLER: the active allocator is none of the pending "
                              "or quarantined ones");
                        check(retiredCtxs[0] && retiredCtxs[1] && lostCtx
                                  && retiredCtxs[0]->resets == 1 && retiredCtxs[1]->resets == 1
                                  && lostCtx->resets == 1,
                              "H1b-15c KILLER: no pending or quarantined allocator was Reset");
                        check(tx.pool().retiredCount() == 2 && tx.pool().lostCount() == 1,
                              "H1b-15c: the pending and quarantined sets are untouched by a "
                              "successful upload");
                    }
                    tx.pool().setCompletedOverride(1);   // EXACTLY the retired value
                    check(tx.drainUploads() == 2 && tx.pool().retiredCount() == 0,
                          "H1b-15c KILLER: at EXACTLY the signalled value both retired "
                          "contexts are released (>=, not >)");
                    check(tx.pool().lostCount() == 1,
                          "H1b-15c KILLER: drain never touches the quarantine - no token, "
                          "no release before teardown");
                    tx.pool().setCompletedOverride(-1);
                    {
                        // The REAL fence path: one more unproven submission, wait for
                        // its fence for real, then drain with no override.
                        const uint32_t r = go(0x300, subsB, TX::Fault::Wait, -1, 3);
                        check(r == 0 && why == CB::CubeRefuse::WaitFailed
                                  && tx.pool().retiredCount() == 1
                                  && tx.slotForKey(0x300) == 2,
                              "H1b-15c: a further Wait failure retires one more context and "
                              "leaves the published pair alone");
                        auto rc = tx.pool().retiredCtx(0);
                        check(waitReal(rc),
                              "H1b-15c: the retired submission's real fence completes");
                        check(tx.drainUploads() == 1 && tx.pool().retiredCount() == 0,
                              "H1b-15c KILLER: with no override, drain reads the REAL fence "
                              "and releases the completed context");
                    }

                    // (d) EIGHT CLOSE FAILURES ON A NEW KEY, THEN A CLEAN UPLOAD
                    {
                        const uint32_t freeBefore = TX::slotFreeCount(tx.freeMask());
                        for (int i = 0; i < 8; i++) {
                            const uint32_t r = go(0x400, subsA, TX::Fault::Close, -1, 4);
                            checkf(r == 0 && why == CB::CubeRefuse::CloseFailed
                                       && tx.slotForKey(0x400) == 0,
                                   "H1b-15d[%d] KILLER: a Close failure publishes nothing", i);
                        }
                        check(TX::slotFreeCount(tx.freeMask()) == freeBefore,
                              "H1b-15d KILLER: eight failures consumed NO index (the r2 leak "
                              "returned NoSlot on the eighth)");
                        check(tx.pool().lostCount() == 1 && tx.pool().retiredCount() == 0,
                              "H1b-15d: Close failures leave the quarantine and retired sets "
                              "alone");
                        const uint32_t idxD = go(0x400, subsA, TX::Fault::None, -1, 4);
                        check(idxD == 3 && why == CB::CubeRefuse::None
                                  && tx.slotForKey(0x400) == 3 && tx.srvWrites(3) == 1,
                              "H1b-15d KILLER: a clean upload after a Close failure succeeds "
                              "on a fresh context and takes the lowest free index (3)");
                    }

                    // (e) SAME-KEY SUCCESS: versioned publication, old pair retired TOGETHER
                    {
                        const ULONG rcBefore = refCount(pubA.res.Get());
                        const uint32_t idxE = go(0x100, subsB, TX::Fault::None, -1, 7);
                        check(idxE == 4 && why == CB::CubeRefuse::None
                                  && tx.slotForKey(0x100) == 4,
                              "H1b-15e KILLER: a same-key refresh publishes into a NEW slot "
                              "(4), never into the slot an in-flight frame may still sample");
                        check(tx.srvWrites(4) == 1 && tx.srvWrites(1) == 1,
                              "H1b-15e KILLER: the descriptor write went to slot 4; slot 1 "
                              "was not rewritten");
                        TX::CubeEntry e;
                        check(tx.entry(0x100, e) && e.res && e.res.Get() != pubA.res.Get(),
                              "H1b-15e: the published resource is the new one");
                        checkSample(4, 101,
                            "H1b-15e KILLER: the shader reads resource B through the NEW slot "
                            "4 - the successful refresh SWITCHED slot and resource");
                        checkSample(1, 1,
                            "H1b-15e KILLER: the shader still reads resource A through the "
                            "OLD slot 1 - a frame in flight on slot 1 keeps its picture");
                        check(tx.retiredCubes() == 1 && (tx.freeMask() & (1u << 1)) == 0,
                              "H1b-15e KILLER: the old (resource, slot 1) pair is RETIRED and "
                              "slot 1 is NOT free yet");
                        // (d) THE RETIRED SLOT IS NOT REUSED BEFORE ITS GRAPHICS FENCE:
                        // a new key arriving now must skip slot 1 (free are {5,6,7}).
                        {
                            const auto subsC = makeSubs(31);
                            const uint32_t idxC = go(0x800, subsC, TX::Fault::None, -1, 7);
                            check(idxC == 5 && why == CB::CubeRefuse::None,
                                  "H1b-15d KILLER: a new key uploaded while slot 1 is retired "
                                  "takes slot 5, not the retired slot");
                            checkSample(5, 31,
                                "H1b-15d KILLER: the shader reads the new key's sentinels "
                                "through slot 5");
                            checkSample(1, 1,
                                "H1b-15d KILLER: slot 1 STILL reads resource A - nothing "
                                "rewrote the retired descriptor before its graphics fence");
                        }
                        check(tx.drainGraphics(6) == 0 && (tx.freeMask() & (1u << 1)) == 0
                                  && tx.retiredCubes() == 1,
                              "H1b-15e KILLER: a graphics fence BELOW the retire value frees "
                              "nothing");
                        check(refCount(pubA.res.Get()) == rcBefore,
                              "H1b-15e: the old resource is still held while retired");
                        check(tx.drainGraphics(7) == 1 && (tx.freeMask() & (1u << 1)) != 0
                                  && tx.retiredCubes() == 0,
                              "H1b-15e KILLER: at the retire value the old slot returns to "
                              "the free list");
                        check(refCount(pubA.res.Get()) == rcBefore - 1,
                              "H1b-15e KILLER: ...and the old resource's holder is dropped in "
                              "the same drain, so the pair was released TOGETHER");
                        // Only NOW may slot 1 be handed out again - and when it is, the
                        // shader reads the new occupant through it.
                        {
                            const auto subsD = makeSubs(61);
                            const uint32_t idxD = go(0x900, subsD, TX::Fault::None, -1, 8);
                            check(idxD == 1 && why == CB::CubeRefuse::None,
                                  "H1b-15d: after the fence the freed slot 1 is the lowest "
                                  "free index again");
                            checkSample(1, 61,
                                "H1b-15d KILLER: slot 1 now reads its NEW occupant - the "
                                "descriptor was rewritten only after retirement completed");
                        }
                    }

                    // (g) A FAILURE PART WAY THROUGH THE RECORDING (SR_HLE_CUBE_FAIL_AT)
                    {
                        ID3D12CommandAllocator* const activeBefore = tx.pool().activeAllocator();
                        const uint64_t createdBefore = tx.pool().contextsCreated();
                        const uint32_t freeBefore = TX::slotFreeCount(tx.freeMask());
                        const uint32_t r = go(0x100, subsA, TX::Fault::None, 5, 8);
                        check(r == 0 && why == CB::CubeRefuse::CloseFailed,
                              "H1b-15g KILLER: aborting at subresource 5 of 12 refuses as "
                              "CloseFailed");
                        check(tx.slotForKey(0x100) == 4 && tx.srvWrites(4) == 1
                                  && TX::slotFreeCount(tx.freeMask()) == freeBefore,
                              "H1b-15g KILLER: nothing published, the live pair untouched, "
                              "the candidate slot returned");
                        check(activeBefore && tx.pool().activeAllocator() == activeBefore
                                  && tx.pool().contextsCreated() == createdBefore,
                              "H1b-15g KILLER: the aborted recording (never executed) leaves "
                              "the SAME context active, so the next upload needs no fresh one");
                        check(tx.pool().retiredCount() == 0 && tx.pool().lostCount() == 1,
                              "H1b-15g: an aborted recording retires and quarantines nothing");
                    }

                    // (i) THE EXACT REQUEST CONTRACT, ON THE DEVICE (r6). Every
                    //     counterexample is refused by ITS OWN name, consumes no
                    //     slot, creates no upload context, submits nothing. Each
                    //     is built so that a REMOVED clause makes the device
                    //     perform a legal-but-wrong copy (observable as a
                    //     publication), never an out-of-bounds one (a crash).
                    {
                        struct Counter {
                            const char* name; CB::CubeRefuse want;
                            std::vector<TX::StagedSub> subs; bool ownsPresent;
                        };
                        std::vector<Counter> counters;
                        {   // extent: 2x2 declared for face 0 mip 0 (a 4x4 subresource)
                            auto s = makeSubs(7);
                            s[0].w = s[0].h = 2; s[0].rows = 2; s[0].rowBytes = 8;
                            s[0].lin.assign(16, 7);
                            counters.push_back({"extent", CB::CubeRefuse::ExtentMismatch, s, true});
                        }
                        {   // duplicate index: entry 3 repeats index 2 (12 entries)
                            auto s = makeSubs(7);
                            s[3] = s[2];
                            counters.push_back({"duplicate", CB::CubeRefuse::IndexDuplicate, s, true});
                        }
                        {   // missing index: the last entry dropped (11 entries)
                            auto s = makeSubs(7);
                            s.pop_back();
                            counters.push_back({"missing", CB::CubeRefuse::IndexMissing, s, true});
                        }
                        {   // too few rows: 3 rows declared for a 4-row face
                            auto s = makeSubs(7);
                            s[0].rows = 3; s[0].lin.assign(3u * 16u, 7);
                            counters.push_back({"rows", CB::CubeRefuse::RowCountMismatch, s, true});
                        }
                        {   // short rowBytes: 12 declared where RGBA8 x 4 texels needs 16
                            auto s = makeSubs(7);
                            s[0].rowBytes = 12; s[0].lin.assign(4u * 12u, 7);
                            counters.push_back({"rowBytes", CB::CubeRefuse::RowBytesMismatch, s, true});
                        }
                        {   // undersized backing: 60 bytes behind a 4x16 footprint (the
                            // vector keeps its 64-byte capacity, so a removed clause reads
                            // stale bytes, not unmapped memory)
                            auto s = makeSubs(7);
                            s[0].lin.assign(64, 7); s[0].lin.resize(60);
                            counters.push_back({"backing", CB::CubeRefuse::BackingMismatch, s, true});
                        }
                        {   // the render thread does not own Present
                            counters.push_back({"present", CB::CubeRefuse::PresentNotOwned,
                                                makeSubs(7), false});
                        }
                        uint32_t key = 0x700;
                        for (const auto& cx : counters) {
                            const uint64_t createdBefore = tx.pool().contextsCreated();
                            const uint32_t freeBefore = TX::slotFreeCount(tx.freeMask());
                            const size_t retiredBefore = tx.pool().retiredCount();
                            TX::UploadRequest rq = request(key, cx.subs, TX::Fault::None, -1, 9);
                            rq.renderThreadOwnsPresent = cx.ownsPresent;
                            tx.beginAttempt();
                            const uint32_t r = tx.upload(dev.Get(), queue.Get(), slot0, inc, rq, why);
                            checkf(r == 0 && why == cx.want && tx.slotForKey(key) == 0,
                                   "H1b-15i[%s] KILLER: refused by its EXACT name %s (got idx=%u "
                                   "why=%s), nothing published", cx.name,
                                   CB::refuseName(cx.want), r, CB::refuseName(why));
                            checkf(tx.pool().contextsCreated() == createdBefore
                                       && TX::slotFreeCount(tx.freeMask()) == freeBefore
                                       && tx.pool().retiredCount() == retiredBefore,
                                   "H1b-15i[%s] KILLER: no upload context created, no slot "
                                   "consumed, nothing retired - the refusal precedes all slot "
                                   "and device work", cx.name);
                            key++;
                        }
                    }

                    // (f) EXHAUSTION: NoSlot is refused before any device work
                    {
                        // live: 0x100@4, 0x300@2, 0x400@3, 0x800@5, 0x900@1 -> free {6,7}
                        const uint32_t want[2] = {6, 7};
                        for (int i = 0; i < 2; i++) {
                            const uint32_t r = go(0x500 + i, subsA, TX::Fault::None, -1, 9);
                            checkf(r == want[i] && why == CB::CubeRefuse::None,
                                   "H1b-15f[%d]: the next cube takes the lowest free index "
                                   "(%u, got %u)", i, want[i], r);
                        }
                        check(TX::slotFreeCount(tx.freeMask()) == 0,
                              "H1b-15f: all seven indices are live");
                        const uint64_t createdBefore = tx.pool().contextsCreated();
                        const uint32_t r = go(0x600, subsA, TX::Fault::None, -1, 9);
                        check(r == 0 && why == CB::CubeRefuse::NoSlot
                                  && tx.slotForKey(0x600) == 0,
                              "H1b-15f KILLER: the eighth cube is refused NoSlot and degrades "
                              "to the dummy - never aliases a live cube. THIS IS THE BOUNDED "
                              "LIMIT: seven slots, no eviction, not a cache");
                        check(tx.pool().contextsCreated() == createdBefore
                                  && tx.pool().retiredCount() == 0,
                              "H1b-15f: NoSlot is decided before any staging or submission");
                    }

                    // (h) CONSERVATION: every attempt is exactly one of uploaded/refused
                    {
                        const TX::Summary s = tx.summary();
                        check(TX::summaryConserves(s), "H1b-15h KILLER: the summary conserves");
                        checkf(s.attempted == 30 && s.uploaded == 8 && s.refused == 22
                                   && s.noSlot == 1,
                               "H1b-15h KILLER: attempted=%llu uploaded=%llu refused=%llu "
                               "noSlot=%llu (expected 30/8/22/1 - every counter increment is "
                               "load-bearing)",
                               (unsigned long long)s.attempted, (unsigned long long)s.uploaded,
                               (unsigned long long)s.refused, (unsigned long long)s.noSlot);
                        check(s.freeSlots == 0 && s.liveCubes == 7 && s.retiredCubes == 0
                                  && s.retiredCtx == 0 && s.lostCtx == 1,
                              "H1b-15h: free=0 live=7 retiredCubes=0 retiredCtx=0 lostCtx=1");
                        char line[320];
                        TX::formatSummary(s, "fixture", line, sizeof(line));
                        check(std::string(line) ==
                                  "[S6_CUBE_SUM] site=fixture attempted=30 uploaded=8 "
                                  "refused=22 noSlot=1 free=0 live=7 retiredCubes=0 "
                                  "retiredCtx=0 lostCtx=1 conserves=YES",
                              "H1b-15h: the summary line renders exactly these numbers");
                    }

                    // Teardown only after the queue is provably idle (the quarantined
                    // context's work has no token, so the fixture flushes the queue).
                    {
                        ComPtr<ID3D12Fence> flush;
                        bool idle = SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                               IID_PPV_ARGS(&flush)));
                        if (idle) {
                            idle = SUCCEEDED(queue->Signal(flush.Get(), 1));
                            if (idle && flush->GetCompletedValue() < 1) {
                                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                                idle = ev && SUCCEEDED(flush->SetEventOnCompletion(1, ev))
                                       && WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
                                if (ev) CloseHandle(ev);
                            }
                        }
                        check(idle, "H1b-15: the queue drains before teardown");
                        // POSITIVE ACTIVATION WITNESS: the suite prints only
                        // failures, so this is the one line that proves the
                        // device fixture RAN rather than being skipped.
                        {
                            DXGI_ADAPTER_DESC ad{};
                            warp->GetDesc(&ad);
                            char name[128] = {};
                            for (int i = 0; i < 127 && ad.Description[i]; i++)
                                name[i] = (char)(ad.Description[i] < 128 ? ad.Description[i] : '?');
                            const TX::Summary s = tx.summary();
                            SR_LOG_BOOT(sr::LogSeverity::Info,
                                "[H1B15_DEVICE] ran on '%s' (vendor 0x%04X): contexts=%llu "
                                "attempted=%llu uploaded=%llu refused=%llu noSlot=%llu "
                                "lostCtx=%u retiredCtx=%u live=%u free=%u",
                                name, (unsigned)ad.VendorId,
                                (unsigned long long)tx.pool().contextsCreated(),
                                (unsigned long long)s.attempted,
                                (unsigned long long)s.uploaded,
                                (unsigned long long)s.refused,
                                (unsigned long long)s.noSlot,
                                s.lostCtx, s.retiredCtx, s.liveCubes, s.freeSlots);
                        }
                        tx.teardown();
                        const TX::Summary s = tx.summary();
                        check(s.freeSlots == 7 && s.liveCubes == 0 && s.retiredCubes == 0
                                  && s.retiredCtx == 0 && s.lostCtx == 0,
                              "H1b-15: teardown releases everything, quarantine included");
                        if (probeEvent) CloseHandle(probeEvent);
                    }
                }
            }

            // ── H1b-16. THE FAULT SEAM PARSE AND THE SUMMARY LINE (pure) ────
            {
                namespace TX = sr::gfx::cubetx;
                check(TX::faultFromEnv(nullptr) == TX::Fault::None
                          && TX::faultFromEnv("") == TX::Fault::None,
                      "H1b-16: no env, no fault");
                check(TX::faultFromEnv("close") == TX::Fault::Close
                          && TX::faultFromEnv("signal") == TX::Fault::Signal
                          && TX::faultFromEnv("event") == TX::Fault::Event
                          && TX::faultFromEnv("wait") == TX::Fault::Wait,
                      "H1b-16: SR_HLE_CUBE_FAIL_SUBMIT names all four faults");
                check(TX::faultFromEnv("1") == TX::Fault::None
                          && TX::faultFromEnv("yes") == TX::Fault::None,
                      "H1b-16 KILLER: an unrecognised value injects NOTHING - a typo must "
                      "not fault a run");
                TX::Summary s;
                s.attempted = 5; s.uploaded = 2; s.refused = 3; s.noSlot = 1;
                s.freeSlots = 5; s.liveCubes = 2; s.retiredCubes = 0;
                char buf[320];
                TX::formatSummary(s, "replay-final", buf, sizeof(buf));
                check(std::string(buf) ==
                          "[S6_CUBE_SUM] site=replay-final attempted=5 uploaded=2 refused=3 "
                          "noSlot=1 free=5 live=2 retiredCubes=0 retiredCtx=0 lostCtx=0 "
                          "conserves=YES",
                      "H1b-16: the summary line is byte-exact");
                s.refused = 2;
                TX::formatSummary(s, "periodic", buf, sizeof(buf));
                check(std::strstr(buf, "site=periodic") != nullptr
                          && std::strstr(buf, "conserves=VIOLATION") != nullptr,
                      "H1b-16 KILLER: attempted != uploaded + refused reports VIOLATION");
                s.refused = 3; s.noSlot = 4;
                check(!TX::summaryConserves(s),
                      "H1b-16 KILLER: noSlot > refused is a violation");
                s.noSlot = 1; s.freeSlots = 4;
                check(!TX::summaryConserves(s),
                      "H1b-16 KILLER: a lost index (free + live + retired != 7) is a "
                      "violation");
            }

            // ── H1b-17. THE EXACT REQUEST CONTRACT, PURE, EVERY FORMAT (r6) ──
            // validateRequest is the clause list the device fixture relies on;
            // here it is driven without a device for every DXGI format the cube
            // path can be handed, including the block-compressed ones, with one
            // counterexample per clause.
            {
                namespace TX = sr::gfx::cubetx;
                struct Fmt { DXGI_FORMAT f; uint32_t blockDim, bpb; const char* name; };
                const Fmt fmts[] = {
                    {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4, "R8G8B8A8"},
                    {DXGI_FORMAT_BC1_UNORM, 4, 8, "BC1"},
                    {DXGI_FORMAT_BC2_UNORM, 4, 16, "BC2"},
                    {DXGI_FORMAT_BC3_UNORM, 4, 16, "BC3"},
                    {DXGI_FORMAT_BC5_UNORM, 4, 16, "BC5"},
                    {DXGI_FORMAT_R16_UNORM, 1, 2, "R16"},
                    {DXGI_FORMAT_R32_FLOAT, 1, 4, "R32F"},
                    {DXGI_FORMAT_R8_UNORM, 1, 1, "R8"},
                    {DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8, "RGBA16F"},
                    {DXGI_FORMAT_R16G16_FLOAT, 1, 4, "RG16F"},
                };
                // An exact request: `ext` x `ext`, two mips, six faces.
                auto build = [](const Fmt& fm, uint32_t ext) {
                    std::vector<TX::StagedSub> v;
                    for (uint32_t face = 0; face < CB::kCubeFaces; face++)
                        for (uint32_t mip = 0; mip < 2; mip++) {
                            TX::StagedSub s;
                            s.w = s.h = ext >> mip ? ext >> mip : 1u;
                            s.rows = (s.h + fm.blockDim - 1u) / fm.blockDim;
                            s.rowBytes = ((s.w + fm.blockDim - 1u) / fm.blockDim) * fm.bpb;
                            s.index = CB::subresourceIndex(face, mip, 2);
                            s.lin.assign(size_t(s.rows) * s.rowBytes, uint8_t(face));
                            v.push_back(std::move(s));
                        }
                    return v;
                };
                auto req = [](const Fmt& fm, uint32_t ext, const std::vector<TX::StagedSub>& v) {
                    TX::UploadRequest rq;
                    rq.key = 1; rq.dxgi = fm.f; rq.width = rq.height = ext;
                    rq.depthOrArraySize = (uint16_t)CB::kCubeFaces; rq.mipLevels = 2;
                    rq.subs = v.data(); rq.count = (uint32_t)v.size();
                    rq.renderThreadOwnsPresent = true;
                    return rq;
                };
                for (const Fmt& fm : fmts) {
                    uint32_t bd = 0, bpb = 0;
                    checkf(TX::formatBlock(fm.f, bd, bpb) && bd == fm.blockDim && bpb == fm.bpb,
                           "H1b-17[%s] KILLER: the transaction's block geometry is %ux%u "
                           "block, %u bytes (got %ux%u, %u) - matching guestFormatToDxgi",
                           fm.name, fm.blockDim, fm.blockDim, fm.bpb, bd, bd, bpb);
                    const uint32_t ext = fm.blockDim == 4 ? 8u : 4u;   // BC: 8x8 -> 2x2 blocks, then 1x1
                    const auto good = build(fm, ext);
                    checkf(TX::validateRequest(req(fm, ext, good)) == CB::CubeRefuse::None,
                           "H1b-17[%s]: an exact 12-subresource request validates", fm.name);
                    { auto v = good; v[3] = v[2];
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::IndexDuplicate,
                             "H1b-17[%s] KILLER: a duplicated index is IndexDuplicate", fm.name); }
                    { auto v = good; v.pop_back();
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::IndexMissing,
                             "H1b-17[%s] KILLER: eleven subresources is IndexMissing - the count "
                             "is exactly depthOrArraySize * mipLevels or nothing", fm.name); }
                    { auto v = good; v.push_back(v[5]);
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::IndexDuplicate,
                             "H1b-17[%s] KILLER: thirteen subresources cannot all be unique - "
                             "refused", fm.name); }
                    { auto v = good; v[0].index = 12;
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::IndexOutOfRange,
                             "H1b-17[%s] KILLER: index 12 of 12 is IndexOutOfRange", fm.name); }
                    { auto v = good; v[0].w += 1;
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::ExtentMismatch,
                             "H1b-17[%s] KILLER: a wrong extent is ExtentMismatch", fm.name); }
                    { auto v = good; v[0].rows -= 1; v[0].lin.resize(size_t(v[0].rows) * v[0].rowBytes);
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::RowCountMismatch,
                             "H1b-17[%s] KILLER: one block row short is RowCountMismatch", fm.name); }
                    { auto v = good; v[0].rowBytes -= 1; v[0].lin.resize(size_t(v[0].rows) * v[0].rowBytes);
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::RowBytesMismatch,
                             "H1b-17[%s] KILLER: one byte short per row is RowBytesMismatch", fm.name); }
                    { auto v = good; v[0].rowBytes += fm.bpb; v[0].lin.resize(size_t(v[0].rows) * v[0].rowBytes);
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::RowBytesMismatch,
                             "H1b-17[%s] KILLER: the row footprint is EXACT, not a minimum", fm.name); }
                    { auto v = good; v[11].lin.pop_back();
                      checkf(TX::validateRequest(req(fm, ext, v)) == CB::CubeRefuse::BackingMismatch,
                             "H1b-17[%s] KILLER: backing one byte short is BackingMismatch", fm.name); }
                    { TX::UploadRequest rq = req(fm, ext, good); rq.renderThreadOwnsPresent = false;
                      checkf(TX::validateRequest(rq) == CB::CubeRefuse::PresentNotOwned,
                             "H1b-17[%s] KILLER: without Present ownership the request is "
                             "PresentNotOwned before any other clause", fm.name); }
                }
                {   // an unknown format never reaches the device
                    const Fmt odd{DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, "RGBA32F"};
                    const auto v = build(odd, 4);
                    check(TX::validateRequest(req(odd, 4, v)) == CB::CubeRefuse::UnsupportedFormat,
                          "H1b-17 KILLER: a DXGI format the cube path was never given a "
                          "geometry for is UnsupportedFormat");
                }
                // The default for the field is FAIL CLOSED, and the production
                // parse for SR_HLE_OWN_PRESENT is ON when unset.
                check(TX::UploadRequest{}.renderThreadOwnsPresent == false,
                      "H1b-17 KILLER: a request that never claimed Present ownership refuses");
                _putenv_s("SR_HLE_OWN_PRESENT", "");
                check(sr::gfx::hle::ownPresentEnabled(),
                      "H1b-17 KILLER: with SR_HLE_OWN_PRESENT unset the render thread owns "
                      "Present - the default cube path is unchanged by the r6 guard");
                // Every new refusal has a loggable name.
                const CB::CubeRefuse named[] = {
                    CB::CubeRefuse::IndexOutOfRange, CB::CubeRefuse::IndexDuplicate,
                    CB::CubeRefuse::IndexMissing, CB::CubeRefuse::ExtentMismatch,
                    CB::CubeRefuse::RowCountMismatch, CB::CubeRefuse::RowBytesMismatch,
                    CB::CubeRefuse::BackingMismatch, CB::CubeRefuse::PresentNotOwned};
                for (CB::CubeRefuse r : named)
                    checkf(std::string(CB::refuseName(r)) != "Unknown",
                           "H1b-17: refusal %u has a name", (unsigned)r);
                check(std::string(CB::refuseName(CB::CubeRefuse::PresentNotOwned)) == "PresentNotOwned",
                      "H1b-17: the ownership refusal is spelled PresentNotOwned");
            }

            // ── H1b-8. EVERY REFUSAL HAS A NAME ────────────────────────────
            check(std::string(CB::refuseName(CB::CubeRefuse::FaceDataMissing))
                      == "FaceDataMissing",
                  "H1b-8: refusals report their own name, so a log line says "
                  "WHICH clause refused rather than that something did");
            check(std::string(CB::refuseName(CB::CubeRefuse::NoSlot)) == "NoSlot",
                  "H1b-8: ...for every enumerator the paths above can return");
        }

        // ── P1. THE APPLIED CUBE TRAVELS IN THE PREAMBLE (iter-457 H1(b)-P1) ──
        // K=1 c57b7ed banked a valid v7 title trace with ZERO cube payload: the
        // guest uploaded its five cubes two minutes before the window opened and
        // the preamble was fed only by the 2D success arms. These drive the REAL
        // executor arm (the backend CALL stood in for - no D3D12Backend exists in
        // this process) and the REAL recorder, then read the artifact back.
        // Predeclared: P1 round trip / P2 refused work cannot contaminate / P3
        // dimensional transitions on one key keep the last-applied rule.
        {
            namespace H = sr::gfx::hle;
            namespace CBp = sr::gfx::cubebind;
            static uint32_t s_p1Next = 1;        // what the stand-in returns (0 = refuse)
            static uint32_t s_p1Calls = 0;
            static H::CubeUploadRequest s_p1Last{};
            H::CubeUploadStandInFn standIn =
                [](const H::CubeUploadRequest& rq, uint32_t& code) -> uint32_t {
                    s_p1Calls++;
                    s_p1Last = rq;
                    if (!s_p1Next) code = (uint32_t)CBp::CubeRefuse::FaceDataMissing;
                    return s_p1Next;
                };
            auto mkCube = [](uint32_t key, uint32_t w, uint32_t h, uint32_t mipLevels,
                             bool packed) {
                RenderCommand c{};
                c.type = RenderCommandType::TextureUpload;
                uint32_t bw = 1, bh = 1, bpb = 4;
                cube::cubeBlockGeom(6, bw, bh, bpb);
                const auto L = cube::computeCubeLayout(6, w, h, w, true, mipLevels,
                                                       packed, bw, bh, bpb);
                c.textureUpload.key = key;
                c.textureUpload.arenaOffset = 0;
                c.textureUpload.bytes = L.baseSpanBytes;
                c.textureUpload.mipBytes = L.mipsTotalExtentBytes;
                c.textureUpload.dims = (w << 16) | (h & 0xFFFFu);
                c.textureUpload.pitch = w;
                c.textureUpload.fmtBits =
                    6u | (1u << 8) | (2u << 12) |
                    (kUploadTexDimCube << kUploadTexDimShift) | (5u << 16) |
                    ((mipLevels - 1u) << kUploadMipLevelsShift) |
                    (packed ? kUploadPackedMipsBit : 0u);
                return c;
            };
            // Two DISTINCT deterministic patterns so base-only, shifted or swapped
            // regions cannot compare equal by accident.
            auto fillCube = [](std::vector<uint8_t>& v, uint32_t base, uint32_t mips,
                               uint8_t salt) {
                v.assign((size_t)base + mips, 0);
                for (uint32_t i = 0; i < base; i++) v[i] = (uint8_t)((i * 7u + 3u) ^ salt);
                for (uint32_t j = 0; j < mips; j++)
                    v[(size_t)base + j] = (uint8_t)((j * 13u + 5u) ^ salt ^ 0xA5u);
            };
            auto sameCmd = [](const RenderCommand& x, const RenderCommand& y) {
                return x.type == y.type && x.textureUpload.key == y.textureUpload.key &&
                       x.textureUpload.bytes == y.textureUpload.bytes &&
                       x.textureUpload.mipBytes == y.textureUpload.mipBytes &&
                       x.textureUpload.dims == y.textureUpload.dims &&
                       x.textureUpload.fmtBits == y.textureUpload.fmtBits &&
                       x.textureUpload.pitch == y.textureUpload.pitch;
            };
            auto same = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
                return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
            };
            // ONE cube through the REAL executor arm (texWrite -> execute()).
            auto applyCube = [&](RenderCommand c, const std::vector<uint8_t>& px,
                                 uint32_t idxToReturn) {
                const uint32_t off = H::texWrite(px.data(), (uint32_t)px.size());
                check(off != 0xFFFFFFFFu, "P1: the two-region payload fits one texture slot");
                c.textureUpload.arenaOffset = off;
                s_p1Next = idxToReturn;
                H::executeForTest(c);
                return c;
            };
            struct P1Rec { RenderCommand cmd; uint32_t payloadBytes; uint64_t payloadHash;
                           std::vector<uint8_t> payload; };
            struct P1Trace { bool hdrOk = false; uint32_t ver = 0, presents = 0;
                             uint64_t records = 0, payloadBytes = 0;
                             uint64_t walked = 0, walkedPayload = 0, walkedPresents = 0;
                             long fileSize = 0; bool exactEof = false;
                             std::vector<P1Rec> recs; };
            // A targeted walker over the artifact (header, envelope, payload).
            auto parse = [](const char* path) {
                P1Trace t{};
                struct RawHdr { char magic[4]; uint32_t version; uint64_t records;
                                uint64_t payloadBytes; uint32_t presents; uint32_t reserved; };
                struct RawRec { RenderCommand cmd; uint32_t payloadBytes; uint32_t reserved;
                                uint64_t payloadHash; };
                FILE* f = std::fopen(path, "rb");
                if (!f) return t;
                RawHdr hh{};
                if (std::fread(&hh, sizeof(hh), 1, f) == 1) {
                    t.hdrOk = hh.magic[0] == 'S' && hh.magic[1] == 'R' &&
                              hh.magic[2] == 'H' && hh.magic[3] == 'T';
                    t.ver = hh.version; t.records = hh.records;
                    t.payloadBytes = hh.payloadBytes; t.presents = hh.presents;
                }
                RawRec rr{};
                while (t.hdrOk && std::fread(&rr, sizeof(rr), 1, f) == 1) {
                    P1Rec r{};
                    r.cmd = rr.cmd; r.payloadBytes = rr.payloadBytes;
                    r.payloadHash = rr.payloadHash;
                    if (rr.payloadBytes) {
                        r.payload.resize(rr.payloadBytes);
                        if (std::fread(r.payload.data(), 1, rr.payloadBytes, f) !=
                            rr.payloadBytes) { t.hdrOk = false; break; }
                    }
                    t.walked++; t.walkedPayload += rr.payloadBytes;
                    if (rr.cmd.type == RenderCommandType::Present) t.walkedPresents++;
                    t.recs.push_back(std::move(r));
                }
                const long pos = std::ftell(f);
                std::fseek(f, 0, SEEK_END);
                t.fileSize = std::ftell(f);
                t.exactEof = (pos == t.fileSize);
                std::fclose(f);
                return t;
            };
            auto isCubeUp = [](const P1Rec& r) {
                return r.cmd.type == RenderCommandType::TextureUpload &&
                       uploadIsCube(r.cmd.textureUpload.fmtBits);
            };
            auto isMarker = [](const P1Rec& r) {
                return r.cmd.type == RenderCommandType::OwnershipRestore &&
                       r.cmd.ownershipRestore.key == 0u;
            };
            auto armPreResolve = []() {
                _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "1");
                _putenv_s("SR_HLE_TRACE_ARM_UNLOCKN", "");
                _putenv_s("SR_HLE_TRACE_ARM_RESOLVEN", "");
                _putenv_s("SR_HLE_TRACE_ARM_TEXKEY", "");
                _putenv_s("SR_HLE_TRACE_ARM_GEOM", "");
                _putenv_s("SR_HLE_TRACE_HOTKEY", "");
                _putenv_s("SR_HLE_TRACE_FRAMES", "1");   // one Present auto-closes
            };
            auto trigger = []() {
                RenderCommand trig{};
                trig.type = RenderCommandType::Resolve;
                trig.resolve.destKey = 0x0B5BE000u;
                trig.resolve.destSize = 0x0059E4FFu;
                trig.resolve.levelSliceFlags = sr::gfx::hle::kResolveDescValidBit;
                return trig;
            };

            const uint32_t KA = 0x03050000u, KB = 0x0BA5A000u, KC = 0x1F0B0000u;
            const RenderCommand cubeA = mkCube(KA, 128, 128, 8, true);
            const uint32_t baseA = cubeA.textureUpload.bytes;
            const uint32_t mipA = cubeA.textureUpload.mipBytes;
            std::vector<uint8_t> pixA, pixA2, pixC, pix2d;
            fillCube(pixA, baseA, mipA, 0x00);
            fillCube(pixA2, baseA, mipA, 0x5C);   // a DIFFERENT refresh payload
            check(baseA == 393216u && mipA == 147456u,
                  "P1-0: the fixture cube is the accepted 128x128 k_8_8_8_8 8-level packed "
                  "cube (393,216 + 147,456 B), the same shape rec_h1a_cube.bin replays");
            auto& ownP1 = H::unlockOwnership();

            H::setCubeUploadStandInForTest(standIn);
            H::shadowResetForTest();              // seq restarts at 1, shadow open
            const uint32_t inFlight0 = H::texPoolInFlightForTest();
            const uint64_t refused0 = H::cubeExecRefusedForTest();

            // ── P1: SUCCESS retains the FULL base+mip region under the normalized key
            s_p1Calls = 0;
            const RenderCommand upA = applyCube(cubeA, pixA, 1u);
            check(s_p1Calls == 1,
                  "P1-1: the executor reached the cube upload (gate on; the stand-in "
                  "answered for the backend)");
            check(s_p1Last.baseBytes == baseA && s_p1Last.mipBytes == mipA &&
                      s_p1Last.base && s_p1Last.mip == s_p1Last.base + baseA &&
                      s_p1Last.key == KA && s_p1Last.mipLevels == 8 && s_p1Last.packed,
                  "P1-1: the executor handed the backend two CONTIGUOUS regions of the "
                  "recorded lengths and the decoded cube metadata");
            check(H::texPoolInFlightForTest() == inFlight0,
                  "P1-1: the texture slot was released after success");
            uint64_t digestA = 0;
            {
                RenderCommand sc{}; std::vector<uint8_t> sb; uint64_t sq = 0;
                const bool got = H::shadowPeekFull(KA, sc, sb, sq);
                check(got && H::shadowKeyCount() == 1,
                      "P1-1 KILLER: a SUCCESSFUL cube upload is retained in the applied-"
                      "state shadow under ONE normalized key (deleting the success "
                      "callback, or retaining under another key, fails here)");
                check(got && sameCmd(sc, upA) && uploadIsCube(sc.textureUpload.fmtBits) &&
                          sc.textureUpload.mipBytes == mipA && sc.textureUpload.bytes == baseA,
                      "P1-1 KILLER: the retained command is the ORIGINAL v7 cube command - "
                      "texDim 3, faces, format, dims, pitch, endian, mip levels, packed, "
                      "bytes AND mipBytes (a zeroed mipBytes fails here)");
                check(got && same(sb, pixA),
                      "P1-1 KILLER: the retained payload is byte-identical to base PLUS mip "
                      "(retaining base only fails here)");
                check(got && sq == 1, "P1-1: it is apply-order 1 in a fresh shadow");
                uint32_t ty = 0; uint64_t sq2 = 0;
                H::shadowPeek(KA, ty, digestA, sq2);
            }
            checkf(digestA == sr::gfx::ShaderCache::hashUcode(pixA.data(), pixA.size()),
                   "P1-1: the shadow digest is XXH3 of the exact concatenated regions "
                   "(%016llX)", (unsigned long long)digestA);

            // ── P2: a REFUSED refresh of the same key leaves the prior success untouched
            s_p1Calls = 0;
            applyCube(cubeA, pixA2, 0u);
            check(s_p1Calls == 1 && H::cubeExecRefusedForTest() == refused0 + 1,
                  "P2-1: the refused refresh took the NAMED refusal arm");
            check(H::texPoolInFlightForTest() == inFlight0,
                  "P2-1: the refused payload's slot was released");
            {
                RenderCommand sc{}; std::vector<uint8_t> sb; uint64_t sq = 0;
                const bool got = H::shadowPeekFull(KA, sc, sb, sq);
                check(got && H::shadowKeyCount() == 1 && sq == 1 && same(sb, pixA),
                      "P2-1 KILLER: the refused bytes did NOT enter the shadow and the prior "
                      "cube is still the retained record (a callback outside the "
                      "nonzero-index success arm fails here)");
            }
            // P2-2: no stand-in and no backend -> the arm cannot succeed -> nothing enters
            H::setCubeUploadStandInForTest(nullptr);
            applyCube(cubeA, pixA2, 1u);
            {
                RenderCommand sc{}; std::vector<uint8_t> sb; uint64_t sq = 0;
                const bool got = H::shadowPeekFull(KA, sc, sb, sq);
                check(got && sq == 1 && H::shadowKeyCount() == 1 && same(sb, pixA) &&
                          H::cubeExecRefusedForTest() == refused0 + 2,
                      "P2-2: with no backend the cube refuses by name and the shadow keeps "
                      "the earlier applied cube");
            }
            H::setCubeUploadStandInForTest(standIn);

            // ── P3: dimensional transitions on ONE normalized key
            RenderCommand up2d{};
            up2d.type = RenderCommandType::TextureUpload;
            up2d.textureUpload.key = KA;
            up2d.textureUpload.bytes = 128u * 128u * 4u;
            up2d.textureUpload.mipBytes = 0;
            up2d.textureUpload.dims = (128u << 16) | 128u;
            up2d.textureUpload.pitch = 128u;
            up2d.textureUpload.fmtBits = 6u | (1u << 8) | (2u << 12);
            pix2d.assign(up2d.textureUpload.bytes, 0x3Cu);
            // The 2D arms' own production entry (no backend here for that arm).
            H::shadowNoteAppliedPublic(up2d, pix2d.data(), (uint32_t)pix2d.size(), KA);
            {
                RenderCommand sc{}; std::vector<uint8_t> sb; uint64_t sq = 0;
                const bool got = H::shadowPeekFull(KA, sc, sb, sq);
                check(got && H::shadowKeyCount() == 1 &&
                          !uploadIsCube(sc.textureUpload.fmtBits) && same(sb, pix2d) && sq == 2,
                      "P3-1: cube -> 2D on one key leaves ONE record, the 2D one, at "
                      "apply-order 2");
            }
            const RenderCommand upA3 = applyCube(cubeA, pixA, 1u);
            {
                RenderCommand sc{}; std::vector<uint8_t> sb; uint64_t sq = 0;
                const bool got = H::shadowPeekFull(KA, sc, sb, sq);
                check(got && H::shadowKeyCount() == 1 && sameCmd(sc, upA3) &&
                          same(sb, pixA) && sq == 3,
                      "P3-2: 2D -> cube on the same key leaves ONE record, the cube with "
                      "base+mip, at apply-order 3");
            }
            // alias normalization: the 0xA window collapses onto the same physical key
            applyCube(mkCube(0xA0000000u | KA, 128, 128, 8, true), pixA2, 1u);
            {
                RenderCommand sc{}; std::vector<uint8_t> sb; uint64_t sq = 0;
                const bool got = H::shadowPeekFull(KA, sc, sb, sq);
                check(got && H::shadowKeyCount() == 1 && sq == 4 && same(sb, pixA2) &&
                          sc.textureUpload.key == (0xA0000000u | KA),
                      "P3-3: an aliased cube key normalizes onto the SAME shadow record and "
                      "the last successful writer wins (a changed key fails here)");
            }

            // ── THE ROUND TRIP through the PRODUCTION recorder ──────────────────
            std::error_code ecP1;
            std::filesystem::create_directories("archive/h1b_p1", ecP1);
            // (a) the MINIMAL fixture: one cube applied BEFORE the window opens,
            //     emitted by the production preamble writer before a Present.
            const char* fixPath = "archive/h1b_p1/cube_preamble_v7.srht";
            armPreResolve();
            H::clearResolveOwnedForTest();
            ownP1.reset();
            H::resetRecorderForTest(fixPath);     // clears the shadow, opens it
            const RenderCommand fixA = applyCube(cubeA, pixA, 1u);
            uint64_t fixDigest = 0;
            { uint32_t ty = 0; uint64_t sq = 0; H::shadowPeek(KA, ty, fixDigest, sq); }
            H::traceRecordForTest(trigger());
            const bool fixOpened = H::recorderOpenedForTest();
            RenderCommand pres{};
            pres.type = RenderCommandType::Present;
            H::traceRecordForTest(pres);
            const bool fixClosedByProduction = !H::recorderOpenedForTest();
            H::releaseTracePathForTest();
            check(fixOpened, "P1-RT: the pre-resolve arm opened the live recorder");
            check(fixClosedByProduction,
                  "P1-RT: the production auto-close (FRAMES=1) closed it and patched "
                  "the header - the test never touched the file");
            {
                const P1Trace t = parse(fixPath);
                check(t.hdrOk && t.ver == 8u, "P1-RT: the fixture is an SRHT v8 trace");
                checkf(t.hdrOk && t.records == t.walked && t.payloadBytes == t.walkedPayload &&
                           t.presents == t.walkedPresents && t.presents == 1u && t.exactEof,
                       "P1-RT: header totals equal the walk and the file ends exactly "
                       "(records %llu/%llu payload %llu/%llu presents %u/%llu eof=%d)",
                       (unsigned long long)t.records, (unsigned long long)t.walked,
                       (unsigned long long)t.payloadBytes, (unsigned long long)t.walkedPayload,
                       t.presents, (unsigned long long)t.walkedPresents, (int)t.exactEof);
                size_t cubeIdx = 0, markerIdx = 0, ncube = 0, nmarker = 0, nup = 0;
                for (size_t i = 0; i < t.recs.size(); i++) {
                    if (isCubeUp(t.recs[i])) { ncube++; cubeIdx = i; }
                    if (t.recs[i].cmd.type == RenderCommandType::TextureUpload) nup++;
                    if (isMarker(t.recs[i])) { nmarker++; markerIdx = i; }
                }
                checkf(ncube == 1 && nup == 1 && nmarker == 1,
                       "P1-RT KILLER: the production preamble emitted EXACTLY ONE cube "
                       "TextureUpload record (uploads %llu cubes %llu markers %llu)",
                       (unsigned long long)nup, (unsigned long long)ncube,
                       (unsigned long long)nmarker);
                check(ncube == 1 && nmarker == 1 && cubeIdx < markerIdx,
                      "P1-RT: the cube record precedes the window-start marker, i.e. it is "
                      "PREAMBLE state, not an in-window command");
                check(!t.recs.empty() && t.recs.back().cmd.type == RenderCommandType::Present,
                      "P1-RT: the Present is the last record");
                checkf(t.records == 4,
                       "P1-RT: exactly four records - preamble cube, marker, trigger, "
                       "Present (got %llu)", (unsigned long long)t.records);
                if (ncube == 1) {
                    const P1Rec& r = t.recs[cubeIdx];
                    check(sameCmd(r.cmd, fixA) && uploadIsCube(r.cmd.textureUpload.fmtBits) &&
                              r.cmd.textureUpload.mipBytes == mipA,
                          "P1-RT KILLER: the emitted record keeps texDim 3, faces/depth, "
                          "format, dims, pitch, endian, mip level, packed-mip, bytes and "
                          "mipBytes of the applied command");
                    checkf(r.payloadBytes == baseA + mipA,
                           "P1-RT KILLER: the envelope payload is the TOTAL base + mip "
                           "(%u), not bytes alone (%u) - got %u",
                           baseA + mipA, baseA, r.payloadBytes);
                    check(same(r.payload, pixA),
                          "P1-RT KILLER: the emitted payload is byte-identical to the exact "
                          "concatenated regions (serialising base only fails here)");
                    checkf(r.payloadHash == fixDigest &&
                               r.payloadHash == sr::gfx::ShaderCache::hashUcode(
                                                    r.payload.data(), r.payload.size()),
                           "P1-RT: the stored hash is XXH3 of exactly those bytes and equals "
                           "the shadow digest (%016llX)", (unsigned long long)r.payloadHash);
                    SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[H1B_P1] fixture %s: records=%llu payloadBytes=%llu presents=%u "
                        "fileBytes=%ld cube key=%08X base=%u mip=%u "
                        "xxh3(base)=%016llX xxh3(mip)=%016llX xxh3(total)=%016llX "
                        "storedHash=%016llX",
                        fixPath, (unsigned long long)t.records,
                        (unsigned long long)t.payloadBytes, t.presents, t.fileSize,
                        KA, baseA, mipA,
                        (unsigned long long)sr::gfx::ShaderCache::hashUcode(pixA.data(), baseA),
                        (unsigned long long)sr::gfx::ShaderCache::hashUcode(pixA.data() + baseA, mipA),
                        (unsigned long long)sr::gfx::ShaderCache::hashUcode(pixA.data(), pixA.size()),
                        (unsigned long long)r.payloadHash);
                }
            }
            // (b) APPLY ORDER across keys and classes: cube KC, then 2D KB (the 2D
            //     arms' entry), then cube KA - map order would be KA, KB, KC.
            const char* orderPath = "archive/h1b_p1/order_p1.srht";
            fillCube(pixC, baseA, mipA, 0x77);
            armPreResolve();
            H::clearResolveOwnedForTest();
            ownP1.reset();
            H::resetRecorderForTest(orderPath);
            const RenderCommand upC = applyCube(mkCube(KC, 128, 128, 8, true), pixC, 1u);
            RenderCommand up2dB = up2d;
            up2dB.textureUpload.key = KB;
            H::shadowNoteAppliedPublic(up2dB, pix2d.data(), (uint32_t)pix2d.size(), KB);
            const RenderCommand upA4 = applyCube(cubeA, pixA2, 1u);
            H::traceRecordForTest(trigger());
            H::traceRecordForTest(pres);
            H::releaseTracePathForTest();
            {
                const P1Trace t = parse(orderPath);
                std::vector<uint32_t> ups;
                size_t markerIdx = t.recs.size(), lastUp = 0;
                for (size_t i = 0; i < t.recs.size(); i++) {
                    if (t.recs[i].cmd.type == RenderCommandType::TextureUpload) {
                        ups.push_back(t.recs[i].cmd.textureUpload.key & 0x1FFFFFFFu);
                        lastUp = i;
                    }
                    if (isMarker(t.recs[i])) markerIdx = i;
                }
                check(t.hdrOk && t.ver == 8u && t.records == t.walked &&
                          t.payloadBytes == t.walkedPayload && t.exactEof && t.presents == 1u,
                      "P1-ORD: the order fixture is a whole, exact v8 trace");
                check(ups.size() == 3 && ups[0] == KC && ups[1] == KB && ups[2] == KA &&
                          lastUp < markerIdx,
                      "P1-ORD KILLER: the preamble emits cube KC, 2D KB, cube KA in APPLY "
                      "order across keys and classes (map order would be KA, KB, KC), all "
                      "before the window marker");
                if (ups.size() == 3) {
                    const P1Rec* rc = nullptr; const P1Rec* rb = nullptr; const P1Rec* ra = nullptr;
                    for (const P1Rec& r : t.recs) {
                        if (r.cmd.type != RenderCommandType::TextureUpload) continue;
                        const uint32_t k = r.cmd.textureUpload.key & 0x1FFFFFFFu;
                        if (k == KC) rc = &r; else if (k == KB) rb = &r; else if (k == KA) ra = &r;
                    }
                    check(rc && sameCmd(rc->cmd, upC) && rc->payloadBytes == baseA + mipA &&
                              same(rc->payload, pixC),
                          "P1-ORD: cube KC carries its own base+mip bytes");
                    check(rb && !uploadIsCube(rb->cmd.textureUpload.fmtBits) &&
                              rb->cmd.textureUpload.mipBytes == 0 &&
                              rb->payloadBytes == up2dB.textureUpload.bytes && same(rb->payload, pix2d),
                          "P1-ORD: the ordinary 2D record is UNCHANGED - payload == bytes, "
                          "mipBytes 0 (zero behaviour change for 2D/unlock preambles)");
                    check(ra && sameCmd(ra->cmd, upA4) && ra->payloadBytes == baseA + mipA &&
                              same(ra->payload, pixA2),
                          "P1-ORD: cube KA carries the LAST applied payload (pixA2), not an "
                          "earlier one");
                }
            }
            // restore: no stand-in, no arm, no frame cap
            H::setCubeUploadStandInForTest(nullptr);
            s_p1Next = 1;
            _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "");
            _putenv_s("SR_HLE_TRACE_FRAMES", "");
            H::shadowResetForTest();
        }

        // ── C4. The VERSION GATE constants ─────────────────────────────────
        check(kTraceVersionForTest == 8,
              "C4: the trace version is 8 (Astra 086: the ClearF colour payload; "
              "v7 was the cube semantic gate)");
        check(kTraceVersionMinForTest == 5,
              "C4 KILLER: the MINIMUM stays 5 - v5 and v6 must keep replaying "
              "their representable program, so the bump must NOT raise the floor");
    }

    // ══ Astra 085: THE ClearF COLOUR - payload, restaging and ownership ══════
    // hr1b carried 12 ClearF records with colorBytes=16 and NO payload: the
    // recorder had no ClearF branch, the replayer no restaging branch, and the
    // executor read and released the recorded (live-session) colorBlock with
    // the gate on or off. These fixtures drive the REAL recorder, the REAL
    // replayTrace and the REAL executor, and assert on counters and pool
    // occupancy - never on capped log lines.
    {
        namespace H = sr::gfx::hle;
        // Non-black HDR on purpose: > 1, negative, large, fractional. A lossy
        // path (8-bit pack, clamp, default) cannot reproduce these bits.
        const float hdr[4] = {2.5f, -0.25f, 1024.0f, 0.5f};
        const uint32_t kFlags = 0x3Fu;   // TARGET0..3 + ZBUFFER + STENCIL
        const auto mkClearF = [&](uint32_t block, uint32_t bytes) {
            H::RenderCommand c{};
            c.type = H::RenderCommandType::ClearF;
            c.clearF.device = 0x839F2880u;
            c.clearF.flags = kFlags;
            c.clearF.rect = 0;
            c.clearF.colorBlock = block;
            c.clearF.colorBytes = bytes;
            const float z = 0.0f;
            std::memcpy(&c.clearF.z, &z, 4);
            c.clearF.stencil = 0;
            return c;
        };
        // An UNRELATED busy block with a sentinel: the ownership witness. A
        // command that reads or releases a handle it does not own either
        // changes what this block reads as, or frees it.
        uint8_t sentinel[H::kClearFColorBytes];
        for (uint32_t i = 0; i < H::kClearFColorBytes; i++)
            sentinel[i] = (uint8_t)(0xA0u + i);
        const uint32_t unrelated = H::blockWrite(sentinel, sizeof(sentinel));
        check(unrelated != H::kBlockFull, "CF0: an unrelated block was allocated");
        const auto unrelatedIntact = [&]() {
            const uint8_t* p = H::blockRead(unrelated);
            return p && std::memcmp(p, sentinel, sizeof(sentinel)) == 0;
        };

        // ── CF1. EXECUTOR: a well-formed HDR colour is read bit-exactly and
        //         its block (and only its block) is released ─────────────────
        {
            H::resetClearFStatsForTest();
            const uint32_t blk = H::blockWrite(hdr, sizeof(hdr));
            check(blk != H::kBlockFull, "CF1: the colour block was allocated");
            const uint32_t before = H::blockInFlightForTest();
            H::executeForTest(mkClearF(blk, H::kClearFColorBytes));
            const auto s = H::clearFStatsForTest();
            check(s.lastColourOk, "CF1: the executor read the colour");
            check(std::memcmp(s.lastRgba, hdr, sizeof(hdr)) == 0,
                  "CF1 KILLER: the decoded colour is BIT-EXACT non-black HDR "
                  "(2.5, -0.25, 1024, 0.5) - no clamp, pack or default");
            checkf(s.lastFlags == kFlags,
                   "CF1: with a colour the flags pass through unmasked (%08X)",
                   s.lastFlags);
            checkf(H::blockInFlightForTest() == before - 1,
                   "CF1 KILLER: the executor released EXACTLY its own block "
                   "(%u -> %u)", before, H::blockInFlightForTest());
            check(unrelatedIntact(),
                  "CF1: the unrelated block is still busy and unchanged");
        }

        // ── CF2. EXECUTOR: no colour -> target bits refused, nothing read or
        //         released, no colour invented ──────────────────────────────
        {
            H::resetClearFStatsForTest();
            const uint32_t before = H::blockInFlightForTest();
            // The handle names a BUSY unrelated block: the old executor would
            // have released it whenever colorBytes was non-zero, and read it
            // whenever colorBytes >= 16. Here colorBytes == 0.
            H::executeForTest(mkClearF(unrelated, 0));
            const auto s = H::clearFStatsForTest();
            check(!s.lastColourOk, "CF2: a missing colour is reported missing");
            checkf(s.colourMissing == 1 && s.targetsMasked == 1,
                   "CF2 KILLER: counted missing (%llu) and target bits masked "
                   "(%llu), BY NAME", (unsigned long long)s.colourMissing,
                   (unsigned long long)s.targetsMasked);
            checkf(s.lastFlags == (kFlags & ~0xFu),
                   "CF2 KILLER: effective flags keep depth+stencil only (0x30), "
                   "never a TARGET clear with an invented colour (got %08X)",
                   s.lastFlags);
            checkf(H::blockInFlightForTest() == before && unrelatedIntact(),
                   "CF2 KILLER: nothing was released and the unrelated block is "
                   "intact (%u -> %u)", before, H::blockInFlightForTest());
        }

        // ── CF3. EXECUTOR: a MALFORMED size is neither read nor released ─────
        {
            H::resetClearFStatsForTest();
            const uint32_t before = H::blockInFlightForTest();
            H::executeForTest(mkClearF(unrelated, 12u));
            const auto s = H::clearFStatsForTest();
            check(!s.lastColourOk, "CF3: a 12-byte colour is not read");
            checkf(s.colourMalformed == 1,
                   "CF3: counted malformed by name (%llu)",
                   (unsigned long long)s.colourMalformed);
            checkf(H::blockInFlightForTest() == before && unrelatedIntact(),
                   "CF3 KILLER: a malformed command did NOT release the block its "
                   "handle names (%u -> %u) and did not alter it", before,
                   H::blockInFlightForTest());
        }

        // ── CF4. RECORDER: the colour travels as the record's payload ────────
        struct RawHdrCF { char magic[4]; uint32_t version; uint64_t records;
                          uint64_t payloadBytes; uint32_t presents; uint32_t reserved; };
        struct RawRecCF { H::RenderCommand cmd; uint32_t payloadBytes;
                          uint32_t reserved; uint64_t payloadHash; };
        static_assert(sizeof(RawHdrCF) == 32 && sizeof(RawRecCF) == 48,
                      "SRHT envelope sizes");
        const char* cfPath = "archive/rec_clearf.bin";
        {
            // A block that is FREE when recorded: the recorder must not claim
            // a colour it cannot read.
            const uint32_t gone = H::blockWrite(sentinel, sizeof(sentinel));
            H::blockRelease(gone);
            const uint32_t good = H::blockWrite(hdr, sizeof(hdr));
            check(good != H::kBlockFull, "CF4: the recorded colour block exists");
            _putenv_s("SR_HLE_TRACE_ARM_PRERESOLVE", "1");
            _putenv_s("SR_HLE_TRACE_FRAMES", "1");
            H::resetRecorderForTest(cfPath);
            H::RenderCommand trig{};
            trig.type = H::RenderCommandType::Resolve;
            trig.resolve.destKey = 0x0B5BE000u;
            trig.resolve.destSize = 0x0059E4FFu;
            trig.resolve.levelSliceFlags = sr::gfx::hle::kResolveDescValidBit;
            H::traceRecordForTest(trig);
            H::traceRecordForTest(mkClearF(gone, H::kClearFColorBytes));  // unreadable
            H::traceRecordForTest(mkClearF(good, H::kClearFColorBytes));  // the real one
            H::RenderCommand pres{};
            pres.type = H::RenderCommandType::Present;
            H::traceRecordForTest(pres);
            H::releaseTracePathForTest();
            H::blockRelease(good);   // the recorder does not execute; we own it

            int nClear = 0;
            uint32_t cfVer = 0;
            bool firstEmpty = false, secondExact = false;
            if (FILE* f = std::fopen(cfPath, "rb")) {
                RawHdrCF hh{};
                bool ok = std::fread(&hh, sizeof(hh), 1, f) == 1 &&
                          std::memcmp(hh.magic, "SRHT", 4) == 0;
                if (ok) cfVer = hh.version;
                RawRecCF rr{};
                while (ok && std::fread(&rr, sizeof(rr), 1, f) == 1) {
                    std::vector<uint8_t> pl(rr.payloadBytes);
                    if (rr.payloadBytes &&
                        std::fread(pl.data(), 1, rr.payloadBytes, f) != rr.payloadBytes)
                        break;
                    if (rr.cmd.type != H::RenderCommandType::ClearF) continue;
                    nClear++;
                    if (nClear == 1)
                        firstEmpty = rr.payloadBytes == 0 &&
                                     rr.cmd.clearF.colorBytes == 0 &&
                                     rr.cmd.clearF.colorBlock == H::kBlockFull;
                    if (nClear == 2)
                        secondExact = rr.payloadBytes == H::kClearFColorBytes &&
                                      rr.cmd.clearF.colorBytes == H::kClearFColorBytes &&
                                      std::memcmp(pl.data(), hdr, sizeof(hdr)) == 0;
                }
                std::fclose(f);
            }
            checkf(nClear == 2, "CF4: both ClearF records reached the trace (%d)",
                   nClear);
            checkf(cfVer == 8u,
                   "CF4 KILLER: a trace that can carry ClearF colour payloads is "
                   "written as v8 - the version a v7 reader refuses (got v%u)",
                   cfVer);
            check(secondExact,
                  "CF4 KILLER: the ClearF record carries its 16-byte colour as "
                  "the payload, bit-exact - the branch whose absence stripped "
                  "every hr1b clear colour");
            check(firstEmpty,
                  "CF4 KILLER: an UNREADABLE colour is recorded as no colour "
                  "(colorBytes=0, kBlockFull, no payload), never as a claim the "
                  "payload cannot back");
        }

        // Astra 086: replay accounting is PER CALL (TraceStats) and CONSERVES:
        // every ClearF record lands in exactly one outcome.
        const auto conserves = [](const H::TraceStats& t) {
            return t.clearFRestaged + t.clearFMissing + t.clearFNoColour +
                       t.clearFMalformed + t.clearFAllocFail ==
                   t.clearFRecords;
        };

        // ── CF5. REPLAY: restaged into an OWNED block, read exactly, released ─
        {
            H::resetClearFStatsForTest();
            const uint32_t before = H::blockInFlightForTest();
            H::TraceStats st{};
            const int rc = H::replayTrace(cfPath, 1, &st);
            const auto s = H::clearFStatsForTest();
            checkf(rc == 0 && st.hashMismatches == 0,
                   "CF5: replay accepted the trace (rc=%d, mismatches=%llu)", rc,
                   (unsigned long long)st.hashMismatches);
            checkf(st.clearFRecords == 2 && st.clearFRestaged == 1 &&
                       st.clearFNoColour == 1 && st.clearFMissing == 0 &&
                       st.clearFMalformed == 0 && st.clearFAllocFail == 0,
                   "CF5 KILLER: 2 records = 1 restaged + 1 noColour (got records "
                   "%llu restaged %llu noColour %llu missing %llu malformed %llu "
                   "allocFail %llu)",
                   (unsigned long long)st.clearFRecords,
                   (unsigned long long)st.clearFRestaged,
                   (unsigned long long)st.clearFNoColour,
                   (unsigned long long)st.clearFMissing,
                   (unsigned long long)st.clearFMalformed,
                   (unsigned long long)st.clearFAllocFail);
            check(conserves(st), "CF5: the ClearF outcomes conserve the records");
            check(s.lastColourOk && std::memcmp(s.lastRgba, hdr, sizeof(hdr)) == 0,
                  "CF5 KILLER: the executor read the RECORDED HDR colour "
                  "bit-exactly after the file round trip");
            checkf(H::blockInFlightForTest() == before && unrelatedIntact(),
                   "CF5 KILLER: the restaged block was released and nothing "
                   "else was (%u -> %u)", before, H::blockInFlightForTest());
        }

        // Rewrite helper: load cfPath, let `edit` change the SECOND ClearF
        // record (its command and payload), fix the header total, write out.
        const auto rewrite = [&](const char* out, auto&& edit) {
            std::vector<uint8_t> all;
            if (FILE* in = std::fopen(cfPath, "rb")) {
                uint8_t b[8192]; size_t got;
                while ((got = std::fread(b, 1, sizeof(b), in)) > 0)
                    all.insert(all.end(), b, b + got);
                std::fclose(in);
            }
            if (all.size() < sizeof(RawHdrCF)) return false;
            RawHdrCF hh{};
            std::memcpy(&hh, all.data(), sizeof(hh));
            std::vector<uint8_t> o(all.begin(), all.begin() + sizeof(hh));
            size_t off = sizeof(hh);
            uint64_t total = 0;
            int nClear = 0;
            while (off + sizeof(RawRecCF) <= all.size()) {
                RawRecCF rr{};
                std::memcpy(&rr, all.data() + off, sizeof(rr));
                off += sizeof(rr);
                std::vector<uint8_t> pl(all.begin() + off,
                                        all.begin() + off + rr.payloadBytes);
                off += rr.payloadBytes;
                if (rr.cmd.type == H::RenderCommandType::ClearF && ++nClear == 2)
                    edit(rr, pl);
                rr.payloadBytes = (uint32_t)pl.size();
                total += pl.size();
                const uint8_t* rp = reinterpret_cast<const uint8_t*>(&rr);
                o.insert(o.end(), rp, rp + sizeof(rr));
                o.insert(o.end(), pl.begin(), pl.end());
            }
            hh.payloadBytes = total;
            std::memcpy(o.data(), &hh, sizeof(hh));
            FILE* f = std::fopen(out, "wb");
            if (!f) return false;
            const bool w = std::fwrite(o.data(), 1, o.size(), f) == o.size();
            std::fclose(f);
            return w && nClear >= 2;
        };

        // ── CF6. THE hr1b SHAPE: colorBytes=16, NO payload, a live handle
        //         that aliases a BUSY unrelated block in this process ─────────
        const char* p6 = "archive/rec_clearf_legacy.bin";
        {
            const bool made = rewrite(p6, [&](RawRecCF& r, std::vector<uint8_t>& pl) {
                pl.clear();
                r.payloadHash = 0;
                r.cmd.clearF.colorBytes = H::kClearFColorBytes;
                r.cmd.clearF.colorBlock = unrelated;
            });
            check(made, "CF6: the payload-less legacy trace was written");
            H::resetClearFStatsForTest();
            const uint32_t before = H::blockInFlightForTest();
            H::TraceStats st{};
            H::replayTrace(p6, 1, &st);
            const auto s = H::clearFStatsForTest();
            checkf(st.clearFMissing == 1 && st.clearFRestaged == 0 &&
                       st.clearFNoColour == 1 && conserves(st),
                   "CF6 KILLER: the replayer reported the colour MISSING by name "
                   "(missing %llu, restaged %llu, noColour %llu, records %llu)",
                   (unsigned long long)st.clearFMissing,
                   (unsigned long long)st.clearFRestaged,
                   (unsigned long long)st.clearFNoColour,
                   (unsigned long long)st.clearFRecords);
            checkf(!s.lastColourOk && s.lastFlags == (kFlags & ~0xFu),
                   "CF6 KILLER: no colour was invented - target bits refused "
                   "(effective %08X)", s.lastFlags);
            checkf(H::blockInFlightForTest() == before && unrelatedIntact(),
                   "CF6 KILLER: the recorded live handle aliasing a BUSY block "
                   "was neither read nor released (%u -> %u) - the exact hr1b "
                   "hazard", before, H::blockInFlightForTest());
        }

        // ── CF7. MALFORMED: command 16, payload 12 ───────────────────────────
        {
            const char* p7 = "archive/rec_clearf_malformed.bin";
            const bool made = rewrite(p7, [&](RawRecCF& r, std::vector<uint8_t>& pl) {
                pl.resize(12);   // truncated colour
                r.payloadHash = 0;   // a mismatch is counted, not decisive here
                r.cmd.clearF.colorBytes = H::kClearFColorBytes;
                r.cmd.clearF.colorBlock = unrelated;
            });
            check(made, "CF7: the malformed trace was written");
            H::resetClearFStatsForTest();
            const uint32_t before = H::blockInFlightForTest();
            H::TraceStats st{};
            H::replayTrace(p7, 1, &st);
            const auto s = H::clearFStatsForTest();
            checkf(st.clearFMalformed == 1 && st.clearFRestaged == 0 && conserves(st),
                   "CF7 KILLER: refused as MALFORMED by name (malformed %llu, "
                   "restaged %llu)", (unsigned long long)st.clearFMalformed,
                   (unsigned long long)st.clearFRestaged);
            checkf(!s.lastColourOk && H::blockInFlightForTest() == before &&
                       unrelatedIntact(),
                   "CF7 KILLER: nothing read, nothing released, unrelated block "
                   "intact (%u -> %u)", before, H::blockInFlightForTest());
        }

        // ── CF8. ALLOCATION FAILURE: the pool is genuinely exhausted ─────────
        {
            H::arenaWaitSetEnabledForTest(0);   // fail now, never wait
            std::vector<uint32_t> held;
            for (;;) {
                const uint32_t b = H::blockWrite(sentinel, sizeof(sentinel));
                if (b == H::kBlockFull) break;
                held.push_back(b);
            }
            checkf(H::blockInFlightForTest() == H::kBlockCount,
                   "CF8: the block pool is exhausted (%u/%u)",
                   H::blockInFlightForTest(), H::kBlockCount);
            H::resetClearFStatsForTest();
            H::TraceStats st{};
            H::replayTrace(cfPath, 1, &st);
            const auto s = H::clearFStatsForTest();
            checkf(st.clearFAllocFail == 1 && st.clearFRestaged == 0 && conserves(st),
                   "CF8 KILLER: the restage failure is counted BY NAME "
                   "(allocFail %llu, restaged %llu)",
                   (unsigned long long)st.clearFAllocFail,
                   (unsigned long long)st.clearFRestaged);
            checkf(!s.lastColourOk && s.lastFlags == (kFlags & ~0xFu),
                   "CF8 KILLER: the failed restage became 'no colour', never a "
                   "read of kBlockFull or an invented value (effective %08X)",
                   s.lastFlags);
            checkf(H::blockInFlightForTest() == H::kBlockCount,
                   "CF8 KILLER: no held block was released by the replay (%u)",
                   H::blockInFlightForTest());
            for (uint32_t b : held) H::blockRelease(b);
            H::arenaWaitSetEnabledForTest(-1);
        }

        // ── CF9. THE SUBMISSION BOUNDARY (Astra 086) ─────────────────────────
        // The executor's clear reaches hleClearBound through ONE function; the
        // tap receives exactly its arguments. Telemetry recorded BESIDE the
        // call (CF2's lastFlags) cannot see a mutant that computes effFlags and
        // then submits a.flags - this can. z and stencil are NON-ZERO so their
        // preservation is visible.
        {
            static int tapCalls = 0;
            static uint32_t tapFlags = 0;
            static float tapRgba[4] = {0, 0, 0, 0};
            static float tapZ = 0.f;
            static uint8_t tapStencil = 0;
            const auto resetTap = [] {
                tapCalls = 0; tapFlags = 0; tapZ = 0.f; tapStencil = 0;
                for (float& v : tapRgba) v = 0.f;
            };
            H::setClearSubmitTapForTest(
                [](uint32_t f, const float r[4], float z, uint8_t st) {
                    tapCalls++;
                    tapFlags = f;
                    std::memcpy(tapRgba, r, sizeof(tapRgba));
                    tapZ = z;
                    tapStencil = st;
                });
            const float zDeep = 0.75f;
            const auto mkZs = [&](uint32_t block, uint32_t bytes, uint32_t flags) {
                H::RenderCommand c = mkClearF(block, bytes);
                c.clearF.flags = flags;
                std::memcpy(&c.clearF.z, &zDeep, 4);
                c.clearF.stencil = 0x15Au;   // only the low byte is a stencil
                return c;
            };
            H::guestClearSetEnabledForTest(1);

            // A: colour present -> every bit submitted as the guest asked.
            resetTap();
            const uint32_t blk = H::blockWrite(hdr, sizeof(hdr));
            H::executeForTest(mkZs(blk, H::kClearFColorBytes, kFlags));
            checkf(tapCalls == 1 && tapFlags == kFlags,
                   "CF9-A: one submission with the guest's flags (calls %d, "
                   "flags %08X)", tapCalls, tapFlags);
            check(std::memcmp(tapRgba, hdr, sizeof(hdr)) == 0,
                  "CF9-A KILLER: the SUBMITTED colour is the HDR colour, bit-exact");
            checkf(tapZ == zDeep && tapStencil == 0x5Au,
                   "CF9-A: depth 0.75 and stencil 0x5A reach the submission "
                   "(z %g, stencil %02X)", (double)tapZ, tapStencil);

            // B: colour missing -> the SUBMITTED flags are the masked ones,
            //    and depth/stencil still arrive intact.
            resetTap();
            H::executeForTest(mkZs(H::kBlockFull, 0, kFlags));
            checkf(tapCalls == 1 && tapFlags == (kFlags & ~0xFu),
                   "CF9-B KILLER: without a colour the SUBMITTED flags are 0x30 - "
                   "hleClearBound never receives a TARGET bit (calls %d, flags "
                   "%08X)", tapCalls, tapFlags);
            checkf(tapZ == zDeep && tapStencil == 0x5Au,
                   "CF9-B KILLER: depth/stencil survive the colour refusal (z %g, "
                   "stencil %02X)", (double)tapZ, tapStencil);

            // C: a TARGETS-ONLY clear with no colour has nothing left to do.
            resetTap();
            H::executeForTest(mkZs(H::kBlockFull, 0, 0x0Fu));
            checkf(tapCalls == 0,
                   "CF9-C: a colour-less targets-only clear submits NOTHING "
                   "(calls %d)", tapCalls);

            // D: gate off -> nothing submitted, and the owned block is still
            //    released (the release does not depend on the gate).
            resetTap();
            H::guestClearSetEnabledForTest(0);
            const uint32_t before = H::blockInFlightForTest();
            const uint32_t blk2 = H::blockWrite(hdr, sizeof(hdr));
            H::executeForTest(mkZs(blk2, H::kClearFColorBytes, kFlags));
            checkf(tapCalls == 0 && H::blockInFlightForTest() == before,
                   "CF9-D: gate OFF submits nothing and still releases its own "
                   "block (calls %d, blocks %u -> %u)", tapCalls, before,
                   H::blockInFlightForTest());

            H::setClearSubmitTapForTest(nullptr);
            H::guestClearSetEnabledForTest(-1);
            check(unrelatedIntact(), "CF9: the unrelated block is intact");
        }

        // ── CF10. EXECUTOR: a size GREATER than 16 is malformed too ──────────
        {
            H::resetClearFStatsForTest();
            const uint32_t before = H::blockInFlightForTest();
            H::executeForTest(mkClearF(unrelated, 32u));
            const auto s = H::clearFStatsForTest();
            checkf(!s.lastColourOk && s.colourMalformed == 1,
                   "CF10 KILLER: a 32-byte colour is malformed, not read "
                   "(malformed %llu)", (unsigned long long)s.colourMalformed);
            checkf(H::blockInFlightForTest() == before && unrelatedIntact(),
                   "CF10 KILLER: and the block its handle names is neither read "
                   "nor released (%u -> %u)", before, H::blockInFlightForTest());
        }

        // ── CF11. REPLAY: command 12 with a VALID 16-byte payload ────────────
        {
            const char* p11 = "archive/rec_clearf_cmd12.bin";
            const bool made = rewrite(p11, [&](RawRecCF& r, std::vector<uint8_t>&) {
                // The payload stays the genuine 16-byte colour; only the
                // command's claim disagrees.
                r.cmd.clearF.colorBytes = 12u;
                r.cmd.clearF.colorBlock = unrelated;
            });
            check(made, "CF11: the command-mismatch trace was written");
            H::resetClearFStatsForTest();
            const uint32_t before = H::blockInFlightForTest();
            H::TraceStats st{};
            H::replayTrace(p11, 1, &st);
            const auto s = H::clearFStatsForTest();
            checkf(st.clearFMalformed == 1 && st.clearFRestaged == 0 && conserves(st),
                   "CF11 KILLER: a valid payload does not rescue a command that "
                   "disagrees - malformed %llu, restaged %llu",
                   (unsigned long long)st.clearFMalformed,
                   (unsigned long long)st.clearFRestaged);
            checkf(!s.lastColourOk && H::blockInFlightForTest() == before &&
                       unrelatedIntact(),
                   "CF11 KILLER: nothing read, nothing released, unrelated block "
                   "intact (%u -> %u)", before, H::blockInFlightForTest());
        }

        // ── CF12. TWO SUCCESSIVE REPLAYS, NO RESET BETWEEN ───────────────────
        // A process-cumulative counter printed beside a per-call record count
        // reads 2 restaged on the second call of an identical trace. Per-call
        // accounting reads 1 both times.
        {
            const uint32_t before = H::blockInFlightForTest();
            H::TraceStats a{}, b{};
            H::replayTrace(cfPath, 1, &a);
            H::replayTrace(cfPath, 1, &b);
            checkf(a.clearFRecords == 2 && a.clearFRestaged == 1 &&
                       b.clearFRecords == 2 && b.clearFRestaged == 1 &&
                       conserves(a) && conserves(b),
                   "CF12 KILLER: each call reports ITS OWN ClearF accounting "
                   "(call1 records %llu restaged %llu; call2 records %llu "
                   "restaged %llu)",
                   (unsigned long long)a.clearFRecords,
                   (unsigned long long)a.clearFRestaged,
                   (unsigned long long)b.clearFRecords,
                   (unsigned long long)b.clearFRestaged);
            checkf(H::blockInFlightForTest() == before && unrelatedIntact(),
                   "CF12: both replays returned every block they restaged "
                   "(%u -> %u)", before, H::blockInFlightForTest());
        }

        // ── CF13. A LEGACY v7 TRACE STILL REPLAYS SAFELY ─────────────────────
        // The same hr1b-shaped record, labelled v7: the v8 reader keeps
        // reading legacy traces and still refuses the missing colour.
        {
            const char* p13 = "archive/rec_clearf_legacy_v7.bin";
            bool made = false;
            std::vector<uint8_t> all;
            if (FILE* in = std::fopen(p6, "rb")) {
                uint8_t buf[8192]; size_t got;
                while ((got = std::fread(buf, 1, sizeof(buf), in)) > 0)
                    all.insert(all.end(), buf, buf + got);
                std::fclose(in);
            }
            if (all.size() > 8) {
                const uint32_t seven = 7u;   // header dword 1 = version
                std::memcpy(all.data() + 4, &seven, 4);
                if (FILE* out = std::fopen(p13, "wb")) {
                    made = std::fwrite(all.data(), 1, all.size(), out) == all.size();
                    std::fclose(out);
                }
            }
            check(made, "CF13: a v7-labelled legacy trace was written");
            H::resetClearFStatsForTest();
            const uint32_t before = H::blockInFlightForTest();
            H::TraceStats st{};
            const int rc = H::replayTrace(p13, 1, &st);
            const auto s = H::clearFStatsForTest();
            checkf(rc == 0 && st.clearFMissing == 1 && conserves(st),
                   "CF13 KILLER: the v8 reader still ACCEPTS v7 and reports its "
                   "payload-less colour MISSING (rc %d, missing %llu)", rc,
                   (unsigned long long)st.clearFMissing);
            checkf(!s.lastColourOk && H::blockInFlightForTest() == before &&
                       unrelatedIntact(),
                   "CF13 KILLER: nothing read or released from the legacy handle "
                   "(%u -> %u)", before, H::blockInFlightForTest());
        }

        H::blockRelease(unrelated);
        H::resetClearFStatsForTest();
    }

    const int total = g_pass + g_fail;
    SR_LOG_BOOT(g_fail ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
                "[TEXUNLOCK_TEST] %d/%d passed", g_pass, total);
    return g_fail;
}

}  // namespace

int runSelfTestIfRequested() {
    const char* e = std::getenv("SR_HLE_TEX_UNLOCK_TEST");
    if (!e || e[0] != '1') return -1;
    return runTests();
}

// \u26d4 THE GATE-OFF PROCESS (director defect 3, iter-457 r6).
//
// C13 asserts the setter returns TRUE inside the suite. That proves the seam
// works; it proves NOTHING about a process where the gate is off - the director
// deleted the only outside-suite refusal and the suite still reported 423/423.
// A structural gate that is only ever exercised from the side where it opens is
// not a tested gate.
//
// So: a SEPARATE process, entered on SR_HLE_TEX_GATEOFF_TEST=1, where
// SR_HLE_TEX_UNLOCK_TEST is NOT set. It calls the REAL setter and observes both
// halves of the refusal - the false return AND the state that never moved.
int runGateOffSelfTestIfRequested() {
    const char* e = std::getenv("SR_HLE_TEX_GATEOFF_TEST");
    if (!e || e[0] != '1') return -1;

    g_pass = g_fail = 0;
    namespace K = sr::kernel;

    // The precondition IS part of the test. If this process were somehow the
    // suite process, every assertion below would be vacuous, so refuse loudly
    // rather than report a green run that measured nothing.
    const char* suite = std::getenv("SR_HLE_TEX_UNLOCK_TEST");
    check(!(suite && suite[0] == '1'),
          "G1 PRECONDITION: this process does NOT have SR_HLE_TEX_UNLOCK_TEST=1, "
          "so the structural gate is genuinely closed here");

    check(K::setTextureDelegateBypassStateForTest() == false,
          "G2: the bypass starts false");

    const bool armed = K::setSetTextureDelegateBypassForTest(true);
    check(armed == false,
          "G3 KILLER: the setter REFUSES outside the suite - it returns false");
    check(K::setTextureDelegateBypassStateForTest() == false,
          "G4 KILLER: ...and the stored flag never moved. This is the half a "
          "return-value assertion cannot reach, and the half that decides "
          "whether the guest body can be skipped in a shipping process");

    // Refusing once is not enough: a setter that latched on the first call
    // would pass G3/G4 and still arm on a retry.
    const bool armedAgain = K::setSetTextureDelegateBypassForTest(true);
    check(armedAgain == false && K::setTextureDelegateBypassStateForTest() == false,
          "G5 KILLER: a SECOND arming attempt is refused too, and the flag is "
          "still false - the refusal is not a one-shot latch");

    const int total = g_pass + g_fail;
    SR_LOG_BOOT(g_fail ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
                "[TEXGATEOFF_TEST] %d/%d passed", g_pass, total);
    return g_fail;
}

}  // namespace sr::kernel::texunlock
