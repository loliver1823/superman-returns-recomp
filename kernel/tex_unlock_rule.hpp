// iter-455n r2: THE texture Lock/Unlock upload body — one implementation,
// driven by production AND by the self-test.
//
// Director's r1 verdict (2026-08-14) found four blockers in the r1 shape:
//   1. IDENTITY SPLIT. The atlas locks at ABA5A000 but shaders bind the
//      NORMALIZED key 0BA5A000. r1 published the RAW pointer as the command
//      key, so the backend stored the fresh resource under 0xA... while the
//      swizzled-view lookup and s_texSlot both normalize — binding the fresh
//      unswizzled resource while building the alpha view over the OLD stale
//      bind-captured one. That defeats the glyph fix outright.
//      => SOURCE POINTER and TEXTURE IDENTITY are now separate fields, and
//         the identity is always normalized here, once.
//   2. The r1 "shared body" shared only the format predicate; the test called
//      arenaWrite itself and never drove latching, range validation, command
//      construction, normalization or ownership. => `runUnlock` below IS the
//      production path, with the three side-effecting steps injected.
//   3. A FAILED Lock kept the previous successful Lock's pointer, so
//      success -> failed-Lock -> Unlock re-uploaded stale bytes.
//      => `LockLatch::begin()` clears after every NORMALLY-RETURNING TRACKED
//         Lock wrapper call (director correction 2026-08-14: NOT 'every Lock
//         attempt' in any wider sense); `runUnlock` CONSUMES the latch, so a
//         second Unlock without a new Lock refuses. The fixture models EMPTY
//         LOCK OUTPUTS - it does not prove a live guest failure mode.
//   4. The r1 read proved neither overflow safety, translation contiguity,
//      committed/readable pages, nor the whole range, and sized nonzero mips
//      with the base-level height. => sizing refuses nonzero levels here, and
//      the reader contract is a CHECKED whole-range translation.
#pragma once

#include <cstdint>

#include "tex_layout.hpp"

namespace sr::kernel::texunlock {

// The create-time format dword of the movie planes: GPUTEXTUREFORMAT_8 (L8).
inline constexpr uint32_t kMoviePlaneFmtDword = 0x28000002u;
// The format CODE in that dword's low byte. k_8 is the SINGLE-CHANNEL class —
// the movie planes and the font atlas both live here.
inline constexpr uint32_t kFmtCode8 = 0x02u;
// The guest-VA identity mask. Every consumer of a texture key normalizes with
// this (s_texSlot, s_unlockOwned, the swizzled-view lookup), so the producer
// must publish the same normalized value or the two name different textures.
inline constexpr uint32_t kKeyMask = 0x1FFFFFFFu;

// True = this Unlock uploads the whole locked surface, every time, with no
// permanent tuple dedupe, sourced from the exact pointer Lock returned.
// False = the texture keeps the bind-time capture path (fallback for textures
// never seen through Lock). Scope is the k_8 single-channel class, where the
// latched lock pitch is exact in BOTH bytes and texels.
inline bool unlockSnapshotWanted(uint32_t createFmt) {
    return createFmt == kMoviePlaneFmtDword || (createFmt & 0xFFu) == kFmtCode8;
}

// What Lock latched for a texture. `begin()` runs on EVERY Lock attempt — a
// failed Lock must not leave the previous success's pointer behind (blocker 3).
struct LockLatch {
    uint32_t pitch = 0, bits = 0, level = 0;
    bool valid = false;
    void begin() { pitch = 0; bits = 0; level = 0; valid = false; }
    void succeed(uint32_t p, uint32_t b, uint32_t lv) {
        pitch = p; bits = b; level = lv; valid = (p != 0 && b != 0);
    }
    bool consume(uint32_t& p, uint32_t& b, uint32_t& lv) {
        if (!valid) return false;
        p = pitch; b = bits; lv = level;
        begin();                       // one Unlock per Lock, by construction
        return true;
    }
};

// The create-time facts the plan needs.
struct TexInfo { uint32_t fmt = 0, w = 0, h = 0; };

// Why an Unlock did not upload. Every refusal is NAMED so the caller can
// distinguish it; production logging of these is SAMPLED, and there is no
// exhaustive per-reason census (director correction 2026-08-14 - the earlier
// 'every refusal is counted/logged' claim was false).
enum class Refuse : uint8_t {
    None = 0, NotEligibleFormat, NoLatch, BadDims, NonZeroMip, SpanOverflow,
    TooBig, Unreadable, ArenaFull,
    // iter-456n Class A: the widened path's own named refusals. Kept DISTINCT
    // from NotEligibleFormat so "the backend cannot decode this" can never be
    // confused with "the legacy k_8 predicate declined it".
    NoFetchConstant, UnsupportedFormat, LayoutInvalid,
};
inline const char* refuseName(Refuse r) {
    switch (r) {
        case Refuse::None:              return "none";
        case Refuse::NotEligibleFormat: return "format-not-k8";
        case Refuse::NoLatch:           return "no-live-lock";
        case Refuse::BadDims:           return "bad-dims";
        case Refuse::NonZeroMip:        return "nonzero-mip";
        case Refuse::SpanOverflow:      return "span-overflow";
        case Refuse::TooBig:            return "too-big";
        case Refuse::Unreadable:        return "unreadable";
        case Refuse::ArenaFull:         return "arena-full";
        case Refuse::NoFetchConstant:   return "no-fetch-constant";
        case Refuse::UnsupportedFormat: return "backend-unsupported-format";
        case Refuse::LayoutInvalid:     return "layout-invalid";
    }
    return "?";
}

struct UnlockResult {
    bool published = false;
    Refuse refuse = Refuse::None;
    uint32_t key = 0;        // NORMALIZED identity (what every consumer uses)
    uint32_t srcVA = 0;      // the EXACT Lock-returned pointer (the source)
    uint32_t bytes = 0, pitch = 0, width = 0, height = 0, level = 0;
    // iter-456n Class A: set only on the widened path, so a consumer can tell
    // a widened publish from a legacy one WITHOUT re-deriving anything.
    bool widened = false;
    uint32_t fmtCode = 0, endian = 0;
    bool tiled = false;
};

// The widened (Class A) plan: what the AUTHORITATIVE header says, not what the
// Lock out-struct happened to report. Built by the caller from the texture
// header's own fetch constant (texlayout::decodeFetchConstant) so the byte
// pitch/texel pitch confusion cannot occur.
struct WidePlan {
    bool haveFetch = false;               // the header carried a 2D fetch
    sr::kernel::texlayout::FetchDesc fd{};
    sr::kernel::texlayout::Layout layout{};
};

// THE body. Production supplies the real steps; the self-test supplies fakes,
// so both drive this exact control flow, this exact normalization, and these
// exact refusals.
//
//   maxBytes : arena slot capacity (0 = unbounded, for tests that don't model it)
//   rd(va, bytes) -> const uint8_t*   CHECKED whole-range translation:
//        must prove contiguity + committed/readable pages over the ENTIRE
//        span and return nullptr otherwise. The checks PRECEDE access; that
//        is a necessary discipline, NOT a demonstration of fault-freedom
//        (director correction 2026-08-14). The harness VEH makes a host AV
//        fatal before SEH filters run, so fault-and-catch is not an option.
//   snap(ptr, bytes) -> uint32_t      copy into the arena; arenaFull on refusal
//   pub(res)                          publish the TextureUnlock command
// `wide` == nullptr  -> LEGACY path, byte-for-byte the iter-455n behaviour
//                       (k_8 predicate, span = lockPitch * createHeight).
// `wide` != nullptr  -> CLASS A: every backend-supported level-0 2D format,
//                       sized by the SHARED layout helper's maxTouched (the
//                       exact bytes the decode addresses), pitch in TEXELS
//                       from the header's own fetch constant, and NO permanent
//                       dedupe. The legacy arm is untouched by this branch.
template <typename Reader, typename Snapshotter, typename Publisher>
UnlockResult runUnlock(const TexInfo& t, LockLatch& latch, uint32_t maxBytes,
                       uint32_t arenaFull, Reader rd, Snapshotter snap,
                       Publisher pub, const WidePlan* wide = nullptr) {
    UnlockResult r{};
    uint32_t pitch = 0, bits = 0, level = 0;
    // Consume FIRST: a duplicate Unlock with no new Lock must refuse even for
    // an eligible format, and an ineligible format must still clear the latch
    // so it cannot be reused later.
    const bool haveLatch = latch.consume(pitch, bits, level);

    // ── format admission ─────────────────────────────────────────────────────
    if (!wide) {
        if (!unlockSnapshotWanted(t.fmt)) { r.refuse = Refuse::NotEligibleFormat; return r; }
    } else {
        // Legacy-eligible formats keep publishing even if the header carried no
        // usable fetch constant, so widening can never LOSE a k_8 publish.
        const bool legacyOk = unlockSnapshotWanted(t.fmt);
        if (!wide->haveFetch || !wide->fd.valid) {
            if (!legacyOk) { r.refuse = Refuse::NoFetchConstant; return r; }
        } else if (!sr::kernel::texlayout::blockGeomFor(wide->fd.fmt).supported) {
            if (!legacyOk) { r.refuse = Refuse::UnsupportedFormat; return r; }
        } else if (!wide->layout.valid) {
            if (!legacyOk) { r.refuse = Refuse::LayoutInvalid; return r; }
        }
    }
    if (!haveLatch)                   { r.refuse = Refuse::NoLatch;           return r; }
    if (!t.h || !t.w || !pitch)       { r.refuse = Refuse::BadDims;           return r; }
    // Blocker 4: base-level height is wrong for a mip. Refused, never guessed —
    // and the widened path inherits the refusal (its layout helper refuses
    // nonzero levels independently, so the two agree by construction).
    if (level != 0)                   { r.refuse = Refuse::NonZeroMip;        return r; }

    // ── span ────────────────────────────────────────────────────────────────
    // Legacy: lock BYTE pitch * create height (exact for k_8, where bytes ==
    // texels). Widened: the decode's own maximum touched byte.
    const bool useWide = wide && wide->haveFetch && wide->fd.valid &&
                         wide->layout.valid &&
                         sr::kernel::texlayout::blockGeomFor(wide->fd.fmt).supported;
    const uint64_t span = useWide ? wide->layout.maxTouched
                                  : (uint64_t)pitch * (uint64_t)t.h;
    if (span == 0 || span > 0xFFFFFFFFull ||
        (uint64_t)bits + span > 0x100000000ull) { r.refuse = Refuse::SpanOverflow; return r; }
    if (maxBytes && span > (uint64_t)maxBytes)  { r.refuse = Refuse::TooBig;       return r; }

    const uint8_t* src = rd(bits, (uint32_t)span);   // checked; never faults
    if (!src)                         { r.refuse = Refuse::Unreadable;        return r; }

    const uint32_t ofs = snap(src, (uint32_t)span);
    if (ofs == arenaFull)             { r.refuse = Refuse::ArenaFull;         return r; }

    r.published = true;
    r.key    = bits & kKeyMask;   // ⛔ IDENTITY: normalized exactly once, here
    r.srcVA  = bits;              //    SOURCE: the exact Lock-returned pointer
    r.bytes  = (uint32_t)span;
    // ⚠ PITCH UNITS: the published pitch must be what the CONSUMER expects.
    // The widened path publishes the fetch constant's TEXEL pitch; the legacy
    // path keeps publishing the lock byte pitch (identical for k_8).
    r.pitch  = useWide ? wide->fd.pitchTexels : pitch;
    r.width  = useWide ? wide->fd.width  : t.w;
    r.height = useWide ? wide->fd.height : t.h;
    r.level  = level;
    if (useWide) {
        r.widened = true;
        r.fmtCode = wide->fd.fmt;
        r.endian  = wide->fd.endian;
        r.tiled   = wide->fd.tiled;
    }
    pub(r, ofs);
    return r;
}

}  // namespace sr::kernel::texunlock
