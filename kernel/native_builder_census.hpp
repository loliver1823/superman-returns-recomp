#pragma once

// iter-467 NATIVE DRAW BUILDER, stage S0: the kill-experiment census (docs/NATIVE_ROADMAP.md,
// "Strand A main track: the native DRAW BUILDER", section 6). Diagnostic, observe-only, default OFF.
//
//   SR_NATIVE_BUILDER_CENSUS=1  observe the three RW compiled-render-object draw calls (the
//   builder's cut): RwRenderObjectDraw 8261D0A0, RwRenderObjectDrawExtXform 8261CD90,
//   RwRenderObjectDrawInstanced 8261CED0 (strong-def overrides that delegate to __imp__), and the
//   XDK draw entries (hooks in kernel/hle_api_observe.cpp). Reports, per 5 s window:
//     (a) XDK draw entries inside / outside the cut, by kind (indexed, auto, UP, begin, end) and
//         Z-pass (device byte +10808 & 2);
//     (c) render-object stability: the sampled objects (see (e)) hash the object's bytes
//         [obj, BE32(obj+8)) (the packet-stream end; 512 bytes if implausible) in 64-byte lines
//         and compares with the object's previous sample: new / same / changed (+ lines changed);
//     (d) rdtsc inside the outermost cut call, and inside the XDK draw entries while in the cut
//         (plumbing = cut - XDK draws).
//     (e) redundancy, ms-weighted (orchestrator 2026-10-07: S1 must gain from ELIMINATED work):
//         objects sampled 1/4 by address (the same objects every frame); per outermost call the
//         PM4 bytes it emitted (device cursor +0x28 before/after, same segment +13508) vs the same
//         object's same occurrence in its previous frame -> same / diff / new / kicked, each with
//         the call's rdtsc; and the object bytes (c) with the call's rdtsc. eliminable_est =
//         cut ms x the output-same share: a LOWER bound (camera-dependent constants and the XDK's
//         dirty-state emission make outputs differ even when the object's own work repeats);
//         the object-bytes-same share is the upper side. Also the same output comparison with
//         the ALU float constants (type-0 0x4000..0x47FF payloads) left out: cacheable_est = the
//         work a per-object cache removes while still re-uploading changed constants.
//         [NATIVE_BUILDER_REDUNDANCY] lines.
//   (b), the pages the cut writes, is the write ledger's "cut" kind: SR_NATIVE_DRAW_LEDGER=1 +
//   SR_WRITE_WATCH=1 sample the cut extent like a draw entry (kernel/native_draw_ledger.*).
//   Log: [NATIVE_BUILDER_CENSUS] window lines (frames = VdSwap delta) + a cumulative line every 30 s.

#include <cstdint>

#include "sr_oracle.hpp"

namespace sr::native::census {

#if SR_ORACLE
extern const bool g_on;

// XDK draw entry hooks (kind = sr::native::dev::DrawKind), around the guest body.
void drawBefore(uint32_t devVA, int kind);
void drawAfter(int kind);
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): the census is compiled out (an unset SR_NATIVE_BUILDER_CENSUS).
inline constexpr bool g_on = false;
inline void drawBefore(uint32_t, int) {}
inline void drawAfter(int) {}
#endif

}  // namespace sr::native::census
