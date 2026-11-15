#pragma once

// NATIVE mode B stage 0: the guest-visible write ledger of the XDK draw entries
// (docs/NATIVE_ROADMAP.md, "native draws from the N1 projection"). Diagnostic, observe-only.
//
//   SR_NATIVE_DRAW_LEDGER=1          sample every Nth draw entry (default N = 256,
//   SR_NATIVE_DRAW_LEDGER_EVERY=N)   per thread) around the guest XDK body:
//     - the device struct (dev .. dev+kDevSpan): every dword the call changed, by draw kind;
//     - segment kicks inside the call (GpuCommandBufferFlush_820FD8C0 /
//       GpuCmdBufBeginNewSegment_820FCF90, via the bracket scopes: needs SR_NATIVE_FRAMESPLIT=1
//       or SR_NATIVE_BRACKETS=1);
//     - with SR_WRITE_WATCH=1, on every SR_NATIVE_DRAW_LEDGER_WW_EVERY-th sample (16): guest pages first written during the call (Windows write watch,
//       read WITHOUT reset so the texture invalidation's own resets are untouched), outside
//       the device struct and the command-buffer segment the call wrote into.
//   SR_NATIVE_DRAW_LEDGER_PAGES=va,va,... (up to 8) diffs those pages byte-exact around each sample;
//   SR_NATIVE_DRAW_LEDGER_PAGES_KIND=k restricts that to one kind (5 = the builder's cut extent).
//   [NATIVE_LEDGER] lines every 4096 samples.

#include <cstdint>

#include "sr_oracle.hpp"

namespace sr::native::ledger {
#if SR_ORACLE
extern const bool g_on;
// kind = sr::native::dev::DrawKind (0 indexed, 1 auto, 2 UP, 3 begin, 4 end), or 5 = the draw
// builder's cut extent (kernel/native_builder_census, SR_NATIVE_BUILDER_CENSUS=1).
void before(uint32_t devVA, int kind);
void after(uint32_t devVA, int kind);
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): the write ledger is compiled out (an unset SR_NATIVE_DRAW_LEDGER).
inline constexpr bool g_on = false;
inline void before(uint32_t, int) {}
inline void after(uint32_t, int) {}
#endif
}  // namespace sr::native::ledger
