// iter-358: NATURAL GAUGES — the honest instrument.
//
// Why this exists (read before adding a gauge):
//
//   The K-run aggregate's movInst/feFrames/defBuilt/aptTick columns were wired
//   to [BLOB_INJECT] / [FE_TICK] / [APT_TICK] — tags emitted ONLY by the
//   forcing drivers in splash_hooks.cpp. Those drivers sit behind
//   forcingAllowed() (SR_ALLOW_FORCING, default OFF), so on a natural
//   zero-env run they never execute and those columns read 0 BY
//   CONSTRUCTION — no matter what the game actually does. We were reading a
//   dead instrument and calling it a frontier.
//
//   Second failure this replaces: the SAMPLING TRAP (six occurrences to date).
//   Probes log under `if (n <= 24)` caps, then a later reader counts log lines
//   and reports the CAP as the measurement. [APT_REQ] capped at 24; [CLIP_CTOR]
//   conflated NULL-movieDef ctors (bookkeeping) with real movie instances, so
//   "clipCtor=453" could be 453 empties and zero real movies.
//
// The contract here:
//   * Counters are ATOMIC and UNCAPPED. Verbose per-event logging may be
//     capped for volume; the COUNT never is.
//   * A wall-clock thread emits one authoritative [GAUGE] census line every
//     10s (and the aggregate reads the LAST one). Never count log lines again.
//   * Every gauge measures a NATURAL signal — a pass-through observer on a
//     guest function the game itself calls. If a gauge can only move when
//     SR_ALLOW_FORCING is on, it does not belong here.
#pragma once
#include <cstdint>

namespace sr { namespace kernel {

enum class Gauge : uint32_t {
  FeTick = 0,      // sub_821494C0  — FE master tick, natural calls only
  // ⛔ NOT sub_826D27C8. That is this function's entry+0xB0 — the LR after the
  // call to AptExecuteFrameInContext at 0x826D27C4 — and it has no __imp__
  // symbol, so hooking it fails to link. The footnote in natural_gauges.cpp
  // (~line 101) recorded that after it cost an hour; this comment kept the
  // wrong value anyway and tools/audit_log_gating.py copied it from here.
  AptPump,         // sub_826D2718  — Apt executor pump tick (hooked in
                   //                 kernel/apt_parse_trace.cpp)
  ClipCtorAll,     // sub_826B0458  — every type-12 clip ctor
  ClipCtorReal,    // sub_826B0458  with movieDef != NULL = a REAL movie instance
  DefExec,         // sub_826B2298  — begin movie-def processing
  AptReq,          // sub_826D2690  — load-by-name request (uncapped, unlike [APT_REQ])
  Count
};

void     gaugeBump(Gauge g);
uint32_t gaugeGet(Gauge g);

// Emits one [GAUGE] census line immediately (also called by the 10s thread).
void gaugeCensus(const char* why);

} }  // namespace sr::kernel
