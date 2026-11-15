#include "kernel/resource_reader_observe.hpp"
// iter-467 NATIVE strand A S1: the native draw flush and its records. Contract: native_flush.hpp.
#include "native_flush.hpp"
#include "native_memory.hpp"
#include "sr_oracle.hpp"
#if SR_ORACLE
#include "gpu_write_watch.hpp"  // gateWritable: pages the GPU write tracker owns read-only (gww::cpuWritableRange)
#endif

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// step 6 N4 (NEWPAIR_CHECKS section 4): an AddressSanitizer build (CMake SR_ASAN_TESTS, or ASAN) poisons an Entry::Impl
// while it sits on a free list (Entry::Impl::park / unpark). Without ASan none of it is compiled.
#if defined(__SANITIZE_ADDRESS__)
#define SR_EP_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SR_EP_ASAN 1
#endif
#endif
#ifndef SR_EP_ASAN
#define SR_EP_ASAN 0
#endif
#if SR_EP_ASAN
#include <sanitizer/asan_interface.h>
#endif

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "stubs.hpp"
#include "gpu_submit.hpp"
#include "gpu_ringless.hpp"  // p2.transport BRACKET: whether 820FD218's override takes a publish
#include "native_const_dedup.hpp"
#include "native_meter.hpp"
#include "ab_toggle.hpp"
#include "p2_trace.hpp"
#include "native_brackets.hpp"
#include "../gfx/pm4_coverage.hpp"  // the census's copy windows (SR_NATIVE_BRACKETS, observe-only)
#include "seg_tile.hpp"
#include "quit.hpp"  // tools/graceful-quit: a publish the ring cut dropped is an orderly stop (ringCut())
#include "native_rec_b.hpp"  // iter-467 native-b row 9: recb::checkArmed() (the 820F0208 leaf below)
#include "../app/log.hpp"
#include "../gfx/resource_census.hpp"  // completed resource producers and Oracle census
#if SR_ORACLE  // C1 stage 1: the development packet walker only
#include "../gfx/pm4_engine.hpp"
#else          // the release configuration links no walker: records only, a literal-PM4 gap is fatal
#include "../gfx/record_engine.hpp"
#endif
#include "../gfx/pm4_live_trace.hpp"
#include "../gfx/native_device.hpp"
#include "../gfx/native_zpd.hpp"  // p2.zpd's bake: the port's gate (zpd::on) and counters (zpd::notePort)

namespace sr::native::flush {

void ensureShaderConstSyncHandlers();
void ensureResidueHandlers();  // p2.transport RESIDUE: kRkCoherHost's handler and renderer
namespace {

bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
// Default ON (iter-467; ft9/ft10 in-roll A/B pooled: tick -0.19 +-0.19 ms, fps +2.9 +-1.6). It was
// default OFF for the flicker (2026-10-08: objects vanished / drew with stale constants for one present)
// until the root cause - the S2a dedup mirror stored while planning, see constRuns - was fixed and the
// flicker gate (TOOLKIT 1.8f) passed with it on. SR_NATIVE_FLUSH=0 keeps the A/B;
// SR_NATIVE_FLUSH_CHECK=1 (the PM4 stream + the dry comparison) turns record mode off.
// C1: the CHECK is a development oracle. Its comparisons live at the draw entries (kernel/hle_api_observe.cpp) and the
// present gate (kernel/alloc_overlap_watch.cpp), compiled out under SR_ORACLE=0 (kernel/sr_oracle.hpp); so is the gate
// itself - no env read, false - or setting it would still turn record mode, the native draws and segmentless off.
#if SR_ORACLE
const bool g_check = envOn("SR_NATIVE_FLUSH_CHECK");
#else
// (brace-initialized: one `g_check =` declaration, the gate's, stays the text scripts/audit_forcing.py reads)
constexpr bool g_check{false};
#endif
const bool g_on = [] {  // default ON again once the root cause (constRuns storing while planning) was fixed
  const char* e = std::getenv("SR_NATIVE_FLUSH");
  return (!e || e[0] != '0') && !g_check;
}();
const bool g_timing = envOn("SR_NATIVE_FLUSH_TIMING");
// Flicker hunt split (diagnostic): SR_NATIVE_FLUSH_PM4=1 keeps the native generation but writes every
// span into the ring as PM4 (what the XDK body would have written) instead of a record, so the CP's
// record apply is out of the loop. Needs SR_NATIVE_CONST_DEDUP=0 (a span that left groups out
// cannot be rendered as PM4; such spans stay records).
// Flicker hunt split 2 (diagnostic): SR_NATIVE_FLUSH_CPPM4=1 keeps the records but the CP renders each
// record back to PM4 packets and runs them through Pm4Engine::execute (the PM4 path's own type-0 /
// DRAW_INDX branches) instead of nativeRecord. Clean = the apply is at fault; dirty = the storage.
// C1 stage 1 (docs/RELEASE_COMPOSITION.md 3.2 stage 1, 9.9): both splits are development oracles that need the PM4
// walker - compiled out under SR_ORACLE=0 (no env read, false), with the walker (gfx/pm4_engine.cpp, EMULATION_SOURCES).
#if SR_ORACLE
const bool g_pm4Out = envOn("SR_NATIVE_FLUSH_PM4");
const bool g_cpPm4 = envOn("SR_NATIVE_FLUSH_CPPM4");
#else
constexpr bool g_pm4Out{false};
constexpr bool g_cpPm4{false};
#endif
// P1 rows 1+2 (NATIVE_ROADMAP "RE-PLAN 2026-10-08"): the shader bind GpuEmitDrawState_82108668 and
// its LOAD_ALU_CONSTANTs as host code writing records (Driver::bindState). SR_NATIVE_BIND, default
// ON (2026-10-08, exe eb60db4baff2: replay 9/9, dry CHECK 22.8M draws 0 BAD, CPPM4 dense freeze
// exact, the same under SR_NATIVE_SEG_STRESS=1024, the frozen-pass A/B exact, meter 6,869 -> 3,283
// PM4 packets per ingame frame, in-roll A/B tick +0.005 +-0.44 ms; the flicker gate in PROGRESS).
// =0 keeps the A/B: the bind stays the guest call it was.
const bool g_bind = [] {
  const char* e = std::getenv("SR_NATIVE_BIND");
  return !e || e[0] != '0';
}();
// STRESS (the coordinator's addition for rows 1+2): SR_NATIVE_SEG_STRESS=N bytes makes the XDK's own
// kick (GpuCommandBufferFlush 820FD8C0, the segment switch where the flicker lived) fire whenever the
// native path has written N bytes into the segment - at the XDK's check sites only, never elsewhere
// - so the switch inside a bind / reserve happens on a large share of draws. A correctness roll's
// setting; 0 = the real threshold dev+48.
// P1 rows 4+6+7: BeginVertices / DrawVerticesUP's flush (820FB6E8) as records (Driver::beginVertices).
// SR_NATIVE_BEGINV, default ON since batch 3b (2026-10-09: dry CHECK 24.64M draws 0 BAD, execution CHECK
// 12/12 exact, stress + frozen-pass A/B PASS); =0 keeps the A/B; SR_AB_TOGGLE=SR_NATIVE_BEGINV for the
// in-roll / frozen-pass A/B.
const bool g_beginVOn = [] {
  const char* e = std::getenv("SR_NATIVE_BEGINV");
  return !e || e[0] != '0';
}();
// P1 row 3: the tiled window-scissor section GpuCmdEmitWindowScissorState_821072F8 inside the row-4
// flush as host items (Driver::windowScissorState). SR_NATIVE_WSCISSOR, default ON since 2026-10-10
// (the orchestrator's go; proof r3b on exe d855d0e75a9b, a drawn world: dry CHECK 0 BAD, CPPM4 11/11,
// stress 11/11 (r3b_rerun), frozen-pass A/B vs the PM4 path PASS under rule R with 406 static pairs,
// flicker gate PASS, meter -23.1 pm4 per ingame frame; ledger p1.row3); =0 keeps the A/B.
const bool g_wscissor = [] {
  const char* e = std::getenv("SR_NATIVE_WSCISSOR");
  return !e || e[0] != '0';
}();
// C1 stage 1: a correctness roll's setting (the stress proofs run the development build) - compiled out under
// SR_ORACLE=0 (no env read, 0: the real threshold).
#if SR_ORACLE
const uint32_t g_segStress = [] {
  const char* e = std::getenv("SR_NATIVE_SEG_STRESS");
  return e && *e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u;
}();
#else
constexpr uint32_t g_segStress{0u};
#endif
// P2-B (NATIVE_ROADMAP "P2-B BUILD PLAN"): SR_NATIVE_SEGMENTLESS, default ON - re-baked 2026-10-13. Baked
// 2026-10-11 (f7f09ba4) and reverted the same day over the save-started gate's frozen-pass drift, which was the
// sea's wall-clock shader time 0x29 under the sim hold, not segmentless (ledger p2.segmentless / save_gate_drift,
// sgd6b); the freeze oracle now holds 0x29 and the eye adaptation. =0 keeps the guest body for the A/B.
// Implies SR_NATIVE_RINGLESS (kernel/gpu_ringless.cpp reads both). The close 820FD8C0 is host code, the segment is published as an
// ordered part list (arena records + the literal PM4 of the gaps), the fence tail and the coherency
// block are records; no ring, no marker, no padding.
bool segmentlessGate() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_SEGMENTLESS");
    return !e || e[0] != '0';
  }();
  return on;
}
const bool g_segmentless = segmentlessGate();
// iter-467 native-a: SR_NATIVE_SEG_TILE=1 (default OFF, observe-only; p2.segmentless v2's strict transport witness,
// kernel/seg_tile.hpp): the producer tile check and the CP first-application check. Unset: none of the tile calls
// below runs (each is behind this latched flag) and the default path is unchanged.
// C1 stage 1: an observe-only development witness - compiled out under SR_ORACLE=0 (no env read, false).
#if SR_ORACLE
bool segTileGate() {  // the gate as a zero-arg helper (scripts/audit_forcing.py, the override manifest)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_SEG_TILE");
    return e && e[0] == '1';
  }();
  return on;
}
const bool g_segTile = segTileGate();
#else
constexpr bool g_segTile{false};
#endif
// p2.transport step 1 (NATIVE_ROADMAP "p2.transport BUILD PLAN" 1): SR_NATIVE_CALLBACK (requires SR_NATIVE_SEGMENTLESS;
// SR_AB_TOGGLE=SR_NATIVE_CALLBACK for the per-present A/B) - GpuEmitCpuCallbackPackets_820FDA80's block as a kRkCallback
// record (the vblank gate's; the Z-pass kick's tail block is BRACKET's, declined). Default ON since the transport bake
// (native/transport-bake, 2026-10-15; NATIVE_ROADMAP "TRANSPORT BAKE PLAN": p2.transport closed on the composed proof
// tcmp3, exe 2bfd499ebe58); =0 keeps the guest body and the A/B.
bool callbackOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_CALLBACK");
    return !e || e[0] != '0';
  }();
  return on;
}
// p2.transport BRACKET (NATIVE_ROADMAP transport plan item 4, its build note): SR_NATIVE_BRACKET, default ON since the
// transport bake (2026-10-15); =0 puts every bracket hook below behind a latched false - the guest path and the A/B.
bool bracketOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_BRACKET");
    return !e || e[0] != '0';
  }();
  return on;
}
const bool g_bracket = bracketOn();
thread_local unsigned g_presentGateOracleDepth = 0;
thread_local unsigned g_shaderConstRunsOracleDepth = 0;
bool shaderConstRunsOn() {
  static const bool on = [] {
    const char* value = std::getenv("SR_NATIVE_SHADER_CONST_RUNS");
    return !value || value[0] != '0';
  }();
  return on;  // default ON since the construns bake (native/construns-bake, 2026-10-14); =0 keeps the SDK body (A/B)
}
// p1.flush_residue (docs/NATIVE_ROADMAP.md "p1.flush_residue BUILD NOTE"): SR_NATIVE_FLUSH_SPLIT, default ON since
// 2026-10-14 (the bake, native/flush-residue-2; the evidence: p1.flush_residue's frs1 / frs1b sets on exe 2087ca4acdb4 -
// the census's flush_residue group 243.947 -> 0.017 a present, all of it 821075E0's alignment pads, which the census
// witnesses since (gfx/pm4_coverage.hpp kConstRunPadKey), the frozen-pass A/B 15/15,
// stress 15/15, the gate 15/15 / world 15, the meter t0 142.3 -> 35.0 and t2nop 182.2 -> 52.0 per ingame frame - and
// the row-9 GPU wait they met, modelled since; the live bake proof: native_proof --plan flush_split_bake). =0 keeps
// today's path (the guest leaf at the segment limit, draws over 0xFFFF declined whole, row 9's pre-pass stopping at a
// kick or a GPU wait) for the A/B, in the release configuration too: SR_ORACLE=0 compiles out the in-roll A/B toggle
// and the dry CHECK, not this gate (as for the earlier bakes). The S1 flush's sections that do not fit before the
// segment limit are chunked across the SDK's own kick as records (GpuCmdEmitRegisterRunChunked 82107058 ported:
// Driver::chunkRun) instead of going to the guest leaf, draws over 0xFFFF indices are split as 820FC000 / 820FBBF8
// split them (Driver::drawPacket), and row 9's record pre-pass looks through the kicks and the GPU waits it cannot
// model (kernel/native_rec_b.cpp). With =0, every path below is the code it was.
bool flushSplitGate() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_FLUSH_SPLIT");
    return !e || e[0] != '0';
  }();
  return on;
}
const bool g_split = flushSplitGate();
// The gate AND the per-present A/B window (SR_AB_TOGGLE=SR_NATIVE_FLUSH_SPLIT, SR_AB_PRESENT=1) at this moment; the S1
// flush decides once per draw (Driver::split), row 9 once per entry.
bool flushSplitWindow() {
  if (!g_split) return false;
  static const int s_ab = sr::abt::reg("SR_NATIVE_FLUSH_SPLIT");
  return s_ab < 0 || sr::abt::on(s_ab);
}
// C1 stage 1, L3b's targeted lever (docs/RELEASE_COMPOSITION.md, the stage-1 record: in s1pre_r / s1pre_r2 every shipping
// =0 arm put literal PM4 outside a Z-pass recording before any inside one): SR_DIAG_ZPASS_LEAF=1, default OFF. A test
// instrument of the shipped binary (class diag: it stays in the release configuration, where it is the one route to the
// release CP's bracket-replayed gap - gapRefused in bracketStep). While a Z-pass recording pins the command buffer
// (dev+0x3290 != 0: GpuReplayListBeginRecording 82110B30 .. GpuReplayListEndRecordingKick 82110C78), an S1 section that
// does not fit before the segment limit goes to its guest leaf (821078C0 / 82107520 / 82107760 - for that section what
// SR_NATIVE_FLUSH_SPLIT=0 does) instead of 82107058's chunks. Nothing else changes: a section outside a recording, a
// draw over 0xFFFF indices and row 9 stay as the split converts them. Exactly "1" turns it on; unset or anything else is
// one latched false, and Driver::zpassLeaf() returns false before it reads anything (inert). Its first section logs one
// line; the report prints its counts while it is on.
bool zpassLeafGate() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_DIAG_ZPASS_LEAF");
    return e && e[0] == '1' && e[1] == '\0';
  }();
  return on;
}
const bool g_zpassLeaf = zpassLeafGate();
// XDK .rdata 820B3668 (IDA): {indices per primitive, overlap} per primitive type, 8 bytes each - the split of a draw over
// 0xFFFF indices in 820FC000 / 820FBBF8 (`slwi r11,prim,3 ; lwzx` from 820B3668 and 820B366C). Read from guest memory
// as the XDK reads it; false = unmapped.
constexpr uint32_t kSplitTableVA = 0x820B3668u;
bool splitTable(uint32_t prim, uint32_t& ipp, uint32_t& overlap) {
  const uint32_t va = kSplitTableVA + (prim << 3);
  const uint8_t* h = sr::kernel::guestVAToHost(va);
  const uint8_t* e = sr::kernel::guestVAToHost(va + 7u);
  if (!h || e - h != 7) return false;
  uint32_t v[2];
  std::memcpy(v, h, 8);
  ipp = _byteswap_ulong(v[0]);
  overlap = _byteswap_ulong(v[1]);
  return true;
}
// The gate AND the in-roll A/B window (SR_AB_TOGGLE=SR_NATIVE_SEGMENTLESS, per present with SR_AB_PRESENT=1):
// spans written in an ON window carry no marker, so a segment holding one must close natively whatever the
// window says at the close (flushSegmentless checks the footprints).
bool segmentlessActive() {
  if (!g_segmentless) return false;
  static const int s_ab = sr::abt::reg("SR_NATIVE_SEGMENTLESS");
  return s_ab < 0 || sr::abt::on(s_ab);
}

constexpr uint32_t kDevSpan = 0x3600u;  // every device field the flush reads or writes
constexpr uint32_t kNopBE = 0x00000080u;  // 0x80000000 big-endian (type-2 NOP)

inline uint32_t be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}
inline uint64_t be64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return _byteswap_uint64(v);
}
inline void put32(uint8_t* p, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(p, &v, 4);
}
inline void put64(uint8_t* p, uint64_t v) {
  v = _byteswap_uint64(v);
  std::memcpy(p, &v, 8);
}

// ---- counters ----
// Statistics only, one writer per side (the render thread / the CP thread): a load + store, no
// locked instruction, and every counter on its own cache line - the per-draw atomics on lines
// both threads wrote cost the first record-mode rolls ~0.1 us/draw.
struct Sw {  // host statistics (never guest memory); a torn read only skews a report line
  uint64_t v = 0;
  uint64_t fetch_add(uint64_t n, std::memory_order = std::memory_order_relaxed) { return (v += n) - n; }
  uint64_t get() const { return v; }
};
#define SR_SW alignas(64) Sw
enum Why { kTaken, kTileRec, kCount, kTrace, kDedup, kSpan, kNoSpace, kIb, kWhyN };
const char* const kWhyName[kWhyN] = {"taken", "tileRec", "count>0xFFFF", "trace", "dedup", "devSpan", "arena", "ib"};
SR_SW g_why[kWhyN];
SR_SW g_spansRec, g_spansPm4, g_guestCalls, g_kicks, g_fallbackSec;
SR_SW g_recBytes, g_ringBytes;
SR_SW g_ddSeen, g_ddPred, g_ddSkipped;  // S2a: constant groups planned / resident / left out of records
SR_SW g_cpRecs, g_cpBad, g_cpRuns, g_cpDraws, g_recDropped, g_recAudited, g_recDesync;
SR_SW g_recBoundBad, g_recBoundOver;  // closeSpan: the record's first-pass size differed from the bytes written (never) / was exceeded
SR_SW g_cpTorn, g_cpNearReclaim;  // execution-time integrity (flicker hunt): header changed during the apply / record within 2 draws of the reclaim line
SR_SW g_sectionAborts, g_sectionAbortsPlanned;  // constRuns sections handed to the guest leaf at the segment limit / of them, after runs were already planned (the root-cause site)
SR_SW g_shortSkipSpans;                           // spans under 16 bytes that claimed a skipped group (impossible; must stay 0)
// p1.flush_residue (SR_NATIVE_FLUSH_SPLIT): per leaf (0 = 821078C0 constants, 1 = 82107520 register runs, 2 = 82107760
// fetch constants) the sections chunked natively at the segment limit / their chunk items / the kicks between chunks;
// the fetch sections whose last run went chunked (the slow trailer {0x00025000, 0, 0, 0} after a kick check); draws over
// 0xFFFF indices split natively / their parts / the per-part kick checks that kicked / DrawIndexed / DrawVertices calls
// over 0xFFFF declined under the gate (the XDK's indices-per-prim entry is 0 or unmapped: the XDK traps there).
SR_SW g_splitSec[3], g_splitChunks[3], g_splitKicks[3], g_splitSlowTrailer;
SR_SW g_bigDraws, g_bigParts, g_bigKicks, g_bigDecl;
SR_SW g_bigDeclOther;  // count > 0xFFFF declined under the gate by a flush with no XDK split (BeginVertices 820FB6E8, ...)
SR_SW g_zpassLeafSec[3];  // SR_DIAG_ZPASS_LEAF: per leaf, the sections at the segment limit it sent to the guest leaf (pinned)
// P1 rows 1+2: binds taken natively / of them with an immediate (ring-copy) VS load / pointer IM_LOADs
// written / LOAD_ALU_CONSTANTs written / guest helper calls (82108520, 82107A30, 821081C0) / reserve
// flushes (820FC910 switched the segment inside a bind) / segment switches after which the dedup
// mirror still held groups although dev+13508 moved (must stay 0: a new segment drops every group).
SR_SW g_binds, g_bindImm, g_bindPtr, g_bindLoads, g_bindHelpers, g_reserveFlushes, g_epochStale;
SR_SW g_cpPackets;  // CP: non-draw packet records applied
SR_SW g_entries, g_entryPkts, g_entryDecl;  // strand B's Entry: spans written / opaque records / declines (open or packet refused)
SR_SW g_tileRectCalls, g_tileRectTiled, g_tileRectDecl;  // P1 row 5 (820F0208 as an Entry): calls / tiled calls / declined to the guest
SR_SW g_cbCalls, g_cbDecl, g_cbApplied, g_cbPredSkip, g_cbChk, g_cbChkBad;  // p2.transport step 1 (SR_NATIVE_CALLBACK)
SR_SW g_kickCbEmitted, g_kickCbPublished, g_kickCbApplied, g_kickCbChained, g_kickCbLiteral, g_kickCbOmitted;
SR_SW g_gateCalls, g_gateSkip, g_gateDecl, g_gateApplied;
// p2.transport BRACKET: begins taken / declined; ends taken (success, failure branch) / declined by reason; generations
// replaced unclosed (stale) / poisoned by a guest close; consumer: BRACKET_END records, BS steps and nested segment
// passes applied; BRACKET_END segments chained behind a running guest pump (A/B transitions only).
enum BrDecl { kBrNoGen, kBrPoisoned, kBrGate, kBrMode, kBrMemory, kBrWalk, kBrBind, kBrPublished, kBrPending, kBrDeclN };
SR_SW g_brBegin, g_brBeginDecl, g_brBeginAbOff, g_brEnd, g_brEndFailed, g_brLiteral, g_brStale, g_brPoison, g_brConsumed,
    g_brBs, g_brSegs, g_brChained, g_brRetired;
SR_SW g_brDecl[kBrDeclN];
// p2.transport RESIDUE (SR_NATIVE_IDLE_WAIT / _INVAL_RANGE / _LOCK_COHER; NATIVE_ROADMAP transport plan item 5 build
// note): per producer (idle, inval, lock) the calls taken natively, declined at the top by reason (the guest body
// whole; noPk = a decline whose guest body writes no PM4: another thread's merge, the IB lock's flags) and late (the
// Entry refused at the record point: the SDK's own dwords written there instead); pk = the PM4 packets the taken calls
// replaced by records (IDLE 1, INVAL 4, LOCK 3), litPk = the packets these producers still wrote as PM4 (declines with
// a packet + late); host = lock blocks with a COHER status kRkInvalidate cannot carry (kind 22); hostApplied = the
// consumer's kind-22 applications. Several threads may decline: atomics.
enum RsProducer { kRsIdle, kRsInval, kRsLock, kRsN };
enum RsDecl { kRsAb, kRsSeg, kRsMem, kRsMode, kRsEntry, kRsOwner, kRsNoPk, kRsDeclN };
std::atomic<uint64_t> g_rsTaken[kRsN], g_rsLate[kRsN], g_rsDecl[kRsN][kRsDeclN];
std::atomic<uint64_t> g_rsPk{0}, g_rsLitPk{0}, g_rsHost{0}, g_rsHostApplied{0};
std::atomic<uint64_t> g_rsMerge[kRsN];  // INVAL / LOCK ports run on another thread: the SDK's dev+0x2D18 merge, no PM4
SR_SW g_swapCalls, g_swapDecl, g_swapApplied;
SR_SW g_swapSdkCalls, g_swapSdkDecl;
// tools/graceful-quit's ring cut (kernel/quit.hpp; tfx2, 2026-10-15): native publishes the cut dropped - an ordinary
// close's segment, a replay kick callback, a BRACKET_END's coherency block and its END. Each is an orderly stop (the
// override drops the guest's own publishes the same way), never a mismatch, a literal route or a fatal.
SR_SW g_cutSegs, g_cutKicks, g_cutCoher, g_cutEnds;
// The graceful quit's ring cut is set (kernel/quit.hpp): from then on the 820FD218 override publishes nothing - it
// drops the doorbell (a lock holder or exempt thread runs on) or parks the thread - so a hand-off not taken is the cut's.
bool ringCut() { return sr::kernel::quit::g_ringCut.load(std::memory_order_acquire); }
// The tcmp1 diagnosis (transport-fix lane, 2026-10-14): the SDK swap leaf's declines by reason and BRACKET_END's memory
// declines by site (native_flush.hpp SwapDeclWhy / BracketMemorySite); g_swapSdkDecl and g_brDecl[kBrMemory] stay the
// totals. Each decline also writes a capped [NATIVE_FLUSH] SWAP_DECL / BRACKET_MEMORY sample line.
SR_SW g_swapWhy[kSwWhyN];
SR_SW g_swapSampled[kSwWhyN];  // the SWAP_DECL sample lines written per reason (swapSdkDeclSampled)
SR_SW g_brMemory[kBmN];
SR_SW g_gateChk, g_gateChkBad, g_gateChkSkip, g_gateChkDecl;
SR_SW g_setConstMemCalls, g_setConstMemDecl, g_setConstMemFail;  // P2-B item 5 (82113010 as an Entry): taken / declined / the XDK's reserve failure
SR_SW g_cpSetConstPm4;  // the CP thread: kRkSetConstMem records rendered back to PM4 and executed (the CPPM4 split)
SR_SW g_wscCalls, g_wscTiled;  // P1 row 3 (821072F8 inside the flush): calls / tiled calls
// SR_NATIVE_LEAF_TRACE=1: the first 400 calls of each row-3 / 5 / 3b site and every 2000th after, with the
// device state the call saw and the cursor before / after (the black-world hunt, 2026-10-09).
const bool g_leafTrace = [] {
  const char* e = std::getenv("SR_NATIVE_LEAF_TRACE");
  return e && e[0] == '1';
}();
bool leafTraceDue(uint64_t n) { return n <= 400 || n % 2000 == 0; }
SR_SW g_entryRuns, g_entryAllocs, g_entryGuest, g_opaqueNoRender;
// P2-B: closes taken / parts / gap PM4 KB / parts applied by the CP (records, gaps) / SEGMENT items whose
// id the CP did not know or whose phys-dwords disagreed (idMiss, mismatch: both must stay 0) / segments
// handed to their own publish / bracket replays resolved, not resolved (a guest-closed segment in the
// A/B: a plain IB) / bracket entries expired / whole-body guest modes / coherency blocks / pinned closes.
SR_SW g_segClosed, g_segParts, g_segGapBytes, g_segRecApplied, g_segGapApplied, g_segIdMiss, g_segMismatch,
    g_segHandoff, g_segReplayHit, g_segReplayMiss, g_segExpired, g_segGuestMode, g_segCoher, g_segPinned;
// P2-B footprints by sequence (the segd wedge, docs/NATIVE_P2B_SEGMENTLESS.md 2b / 2c): pending footprints outside
// their segment or out of order (footStray, not applied) / dropped by a close that went the guest way (feetLost) -
// both must stay 0; coherDropOld (observe-only) = the open segment's footprints the coherency block's old range
// rule discarded, the defect's witness on the same roll.
SR_SW g_segFootStray, g_segFeetLost, g_segCoherDropOld;
// The native close's coherency range blocks (820FD3B8's range half) and, of them, ranges that reach 4 GB (base +
// size wraps: the all-ones sentinel defect's signature, fixed on native/coher-sentinel - must stay 0).
SR_SW g_coherRange, g_coherTop;
SR_SW g_segChained;  // closes the guest's 820FD800 chained to the pump (dev+0x2A74 != 0 under its spinlock)
SR_SW g_segCapDrop;  // deferred entries the cap dropped before they completed (nothing in flight; must stay 0)
SR_SW g_segCapPressure;  // collection threshold exceeded while live passes remain; not a hard-cap proof
SR_SW g_litLenBad;   // literal spans whose items' PM4 bytes differed from the span's (the CPPM4 split; must stay 0)
// The ring path's literal spans (shorter than the 16-byte marker, so always the XDK's own PM4 there; segmentless
// records them) by the meter's packet kind (native_meter.hpp Kind) - observe-only: the census behind the segmentless
// meter's prediction (the orchestrator 2026-10-11: the drop derived from a default-path census, not fitted).
SR_SW g_litKind[sr::native::meter::kKinds];
SR_SW g_beginV, g_beginAllocFail;  // P1 rows 4+6+7: BeginVertices flushes taken natively / of them with the XDK allocator failing
SR_SW g_shaderConstRuns, g_shaderConstChunks;
// p1.row3_construns bake (native/construns-bake, 2026-10-14): who reaches 821075E0 and how, observe-only, printed on the
// shaderConstRuns line. drv = the S1 Driver section's native calls (inside 820FBBF8 / 820FC000), sdk = the shared SDK
// leaf's native calls (the guest bodies' bl 0x821075E0 and the production table), abGuest = the Driver section's
// A/B-off guest calls, decl = SDK leaf calls with the gate on that ran the guest body (by reason), kick = calls whose
// entry check (cursor > dev+48) switched the segment, marginBad = calls whose dev+44 - dev+48 was not the XDK's 160
// (the static bound that keeps every caller's <= 6-group mask off the chunked path holds only at 160), tessDraws =
// D3DDeviceEmitDrawTessellated 820FC478 calls, tessSdk = SDK leaf calls made inside one, cpPm4 = kRkShaderConstSync
// items the CP rendered back to PM4 and executed (the CPPM4 split; the consumer side, as setconst's cpPm4).
enum ScrDecl { kScrOff, kScrOracle, kScrMask, kScrSpan, kScrOpen, kScrEmit, kScrDeclN };
std::atomic<uint64_t> g_scrDrv{0}, g_scrSdk{0}, g_scrAbGuest{0}, g_scrKick{0}, g_scrMarginBad{0};
std::atomic<uint64_t> g_scrDecl[kScrDeclN];
std::atomic<uint64_t> g_tessDraws{0}, g_tessSdk{0};
thread_local unsigned t_tessDepth = 0;
SR_SW g_cpShaderSyncPm4;  // the CP thread only
thread_local uint32_t t_shaderSyncPm4 = 0;  // cpRecordPm4's renders of the record being applied (counted if executed)
SR_SW g_ckImm;      // check mode: dry runs stopped at the immediate VS load (its guest patch helpers mutate the copy)
SR_SW g_tGuestTsc, g_tGuestN, g_tCpTsc, g_tCpN, g_tGuestCallsTsc;
// check mode
SR_SW g_ckN, g_ckFull, g_ckPrefix, g_ckKick, g_ckBadBytes, g_ckBadLen, g_ckBadDev, g_ckSkip, g_ckNativeTsc,
    g_ckGuestTsc, g_ckTimed;
// ...and per CHECK kind (the bracket kinds: 5 the S1 leaf, 6 the callback, 7 EmitPm4Packet, 8 ResetState 8210E520, 9 the
// segment re-emit 820F0048): COMPLETED (full) comparisons and bad brackets, printed on the CHECK report line as
// "kinds full/bad: k8=1/0 k9=5400/0", so a plan can require that EACH emitter it advertises was compared - an aggregate
// "BAD 0" says nothing about a kind the CHECK never reached (native/devcreate-fix, Astra round 1 P2 on the dry CHECK).
constexpr int kCkKinds = 16;
SR_SW g_ckKindFull[kCkKinds], g_ckKindBad[kCkKinds];

// ---- the record arena (host) ----
constexpr uint64_t kRecArena = 16ull << 20;  // L3-warm; half = ~3 frames of history
constexpr uint32_t kRecMagic = 0x4E524543u;  // 'NREC'
constexpr uint32_t kMaxDrawRec = 64u << 10;  // upper bound of one draw's records
uint8_t* g_arena = nullptr;
uint64_t g_head = 0;                       // producer (under g_mu)
uint64_t g_reclaim = 0;                    // producer: the oldest record not yet audited (under g_mu)
uint64_t g_tailSeen = 0;                   // producer's last read of g_cpTail (under g_mu)
alignas(64) std::atomic<uint64_t> g_cpTail{0};  // end of the newest record the CP executed (CP writes)
// P2-B segment registry: the open segment's span footprints (the owner thread appends at every closeSpan;
// the close consumes them) and the closed segments' part lists BY ID (docs/NATIVE_P2B_SEGMENTLESS.md).
// A segment the close publishes itself travels to the SEGMENT item on the publishing thread
// (t_segPublishing -> segmentPublishId -> gpuCpPushSegment) and is applied once. A segment the pump
// publishes later - the Z-pass bracket's mode-B list records (replayed twice) and chained segments while
// a chain is in flight - is DEFERRED: it is also kept in g_segDeferred under its bracket generation, where
// the pump's 820FD218 calls resolve it by (phys, dwords); entries two brackets old expire.
// Never by phys alone: the 2 MB command buffer is reused, and in the per-present A/B the guest's own
// closes write blocks at the same tail positions (p2c_abp, 2026-10-09: the guest's 11-dword flush block
// at 1E97A580 in present 55 was answered with the native 22-dword coherency block registered there at
// present 4; its INVALIDATEs replaced the block, the kick's SCRATCH write-backs never landed and the CP
// sat in the next WAIT).
struct SegRecord;
struct SegParts;
struct SegFoot { uint32_t lo, hi; uint64_t pos; std::shared_ptr<SegRecord> record; };
// Part kinds: 0 an arena record (owned at emission), 1 the guest's bytes between footprints (copied at the close).
// p2.transport BRACKET adds the replay steps of a BRACKET_END segment: 2 a bin select (SET_BIN_SELECT_LO, value in
// `off`), 3 a bound bracket segment applied whole (`nested`: kinds 0 / 1 only). Only bracketEnd builds kinds 2 / 3.
struct SegPart {
  uint8_t kind;
  uint32_t len;
  uint64_t pos;
  uint32_t off;
  uint32_t va = 0;
  std::shared_ptr<SegRecord> record;  // kind 0: owned at emission; kind 1: copied guest bytes
  std::shared_ptr<SegParts> nested;   // kind 3: the bound segment (immutable, held until this pass ran)
};
struct SegParts {
  std::vector<SegPart> parts;
  std::vector<uint8_t> bytes;
#if SR_ORACLE
  // SR_NATIVE_BRACKETS (the PM4 census, observe-only): the owner stamps of the closed range, taken with the copy
  // (native_brackets' stamp lifetime) - the CP attributes the gap parts by them, not by what the guest wrote since.
  std::vector<sr::native::Interval> stamps;
#endif
  uint32_t phys = 0, dwords = 0;
  uint64_t id = 0;        // the registry key (1-based, never reused)
  uint64_t bracket = 0;   // deferred entries: the Z-pass bracket generation they belong to
  bool deferred = false;  // published by the pump later (bracket list record / chained segment)
  bool kickCallback = false;  // exact 82110C78 tail, for its produced/consumed witness
  uint64_t bracketGen = 0;     // BRACKET_END: the native generation whose replay steps follow the record
  uint32_t bracketSteps = 0;   // BRACKET_END: the number of step parts (kinds 2 / 3)
  // Lifetime by the CONSUMER's progress (the segh / segi1 idMiss, 2026-10-10: the CP lagged a few seconds behind the
  // producer after a 3 s present stall, two Z-pass brackets began meanwhile, and the generation expiry erased entries
  // the pump had already published - the CP met their ids unknown and parsed the segment memory as an IB: the
  // native records' positions are stale bytes there, one of them zeroed SCRATCH_UMSK). Under g_segMu.
  uint32_t passes = 1;     // the publishes a deferred entry gets: a bracket list record 2 (depth, colour), a chained segment 1
  uint32_t published = 0;  // SEGMENT items handed out with this id (the close's hand-off or a replay hit)
  uint32_t started = 0;    // CP passes that have begun (under g_segMu)
  uint32_t finished = 0;   // CP passes that returned from all parts (under g_segMu)
  bool complete() const { return finished >= passes && finished == published && started == finished; }
};
// Collection threshold, not permission to drop unfinished or unpublished passes.
// A hard capacity policy needs pump-aware admission; never wait here for a pass
// which this same producer has yet to publish.
constexpr size_t kSegDeferredCap = 8192;
std::mutex g_segMu;
std::vector<SegFoot> g_segFeet;
std::unordered_map<uint64_t, std::shared_ptr<SegParts>> g_segById;  // every live entry (under g_segMu)
std::vector<std::shared_ptr<SegParts>> g_segDeferred;                // the deferred ones, oldest first
uint64_t g_segNextId = 1;      // under g_segMu
uint64_t g_segBracket = 0;     // the current Z-pass bracket generation (owner thread)
bool g_segWasPinned = false;   // owner thread: the previous close's pinned state (a 0 -> 1 edge = a new bracket)
thread_local std::shared_ptr<SegParts> t_segPublishing;  // the close's hand-off to the 820FD218 override
struct SegCur { const SegParts* sp = nullptr; uint32_t idx = 0; };
thread_local SegCur t_segCur;  // CP thread: the part cpSegment is applying (observe-only, cpSegPartDescribe)
// p2.transport BRACKET: the open native generation (one: the device's owner thread records one bracket at a time).
// Opened by bracketOpen after the SDK BeginRecording body; every pinned close of that device on that thread appends
// its parts IN ORDER; the kick binds the recorded list's 0x82 records to them by sequence. Under g_segMu.
struct BracketClose {
  uint32_t listVA, dwords, phys;
  std::shared_ptr<SegParts> sp;
};
struct BracketGen {
  uint64_t gen = 0;
  uint32_t dev = 0, head = 0, header = 0;
  DWORD thread = 0;
  bool poisoned = false;
  std::vector<BracketClose> closes;
};
std::unique_ptr<BracketGen> g_brOpen;
uint64_t g_brNextGen = 1;      // under g_segMu
uint64_t g_brLastConsumed = 0; // CP thread: the newest BRACKET_END applied (monotone)
#if defined(SR_DIFFTEST)
SegmentPublicationObserver g_segPublishObserver = nullptr;
std::vector<std::shared_ptr<SegParts>> g_brTestBound;  // the last kick's bound closes (the bracket suite's retirement check)
std::vector<std::array<uint32_t, 3>> g_brTestBinding;  // the last binding's trace records {first, count, generation}
std::vector<std::array<uint32_t, 4>> g_brTestLifecycle;  // the last BRACKET_END's CP lifecycle records {aux, id, steps, gen}
std::vector<std::array<uint32_t, 4>> g_brTestPlan;  // the last binding's plan records {aux, step index, value, gen}
uint64_t g_brTestCutGen = 0;  // the last BRACKET_END the ring cut dropped (traced, aux 10)
// tools/difftest/flush_split_test.inc's SDK arm through a generated GAME caller (RwDrawInstancedConstBatched 8261B050):
// on this thread the draw flushes (draw()) and SetPredication (setPredicationLeaf) decline to their guest bodies, as the
// arm's direct __imp__ calls of the XDK entries do. Difftest only: no such switch exists in the game.
thread_local bool t_fsGuestOnly = false;
#endif
struct SpinLock {
  std::atomic<bool> f{false};
  void lock() {
    for (int i = 0; f.exchange(true, std::memory_order_acquire); i++)
      if (i > 64) std::this_thread::yield();
      else _mm_pause();
  }
  void unlock() { f.store(false, std::memory_order_release); }
};
SpinLock g_mu;

// Record words (host order): RUN = (count << 16) | reg, then count values (big-endian, as the
// guest wrote them), bit 30 set = a CHECK run (S2a: predicted resident, the CP proves it first);
// DRAW = 0x80000000 | (indexed ? 1 : 0) | (predicated ? 2 : 0), then initiator, dmaBase, dmaSize.
struct RecHdr {
  uint32_t magic, bytes;
  uint64_t pos;
  uint32_t exec;  // set after the CP's first execution returns (the reclaim audit below)
  uint32_t pad;
};

// ---- step 6 I7: the segment-record arena (render/gate3/FREEZE_I7_20261022.txt = I7_DESIGN_v1 + Fable 30's seven
// conditions; Fable 45: the draw-thread / frame claims and the scope lock).
// SR_SEG_ARENA (default ON): a closed span's record is bump-allocated in the OPEN SegArena (a refcounted chain of chunks)
// and handed out as an ALIASING std::shared_ptr - its pointer is the record, its ownership the arena's control block -
// so an arena and its chunks live until the LAST owning reference to ANY of its records goes (SegFoot, a detached foot,
// SegPart in g_segById / g_segDeferred / BracketGen.closes / t_segPublishing / a CP pass, the audit map). There is no
// reset and no reuse of a live chunk: a chunk is released only in its arena's destructor (lifetime argument L1-L5,
// I7_DESIGN_v1 section 4). The open arena is replaced at each segment's close (takeFeetLocked); the audit map recycles
// its nodes; the open segment's footprint vector keeps its capacity; buildParts reserves.
// =0: one allocation per record, freed at its own last reference (today's lifetime and allocation pattern), the map's
// own nodes, the empty-vector swap, no reserve - and the SAME record bytes / positions / part lists (condition 5: the
// [SEG_ARENA] capture checksums of a run under =1 and =0 are equal).
// SR_SEG_ARENA_KEEP (default ON): a released arena's chunks go to a bounded free list (64 chunks / 4 MiB); =0 frees them
// (the ASan run: a use after release is then a heap-use-after-free report).
struct SegRecord {
  // Written once, in captureSegmentRecord under g_segMu (with g_mu held by the writer), before any pointer to it
  // escapes; immutable thereafter except `completed` (the CP's progress). Trivially destructible: no destructor runs.
  std::atomic<bool> completed{false};
  uint32_t nWords = 0;
  uint32_t check = 0;  // SR_ORACLE: the words' checksum at capture, re-verified at the CP's apply and the audit's reclaim
  // User-declared: NOT an aggregate, so the old SegRecord(source, bytes) form cannot compile into a header without its
  // words (Astra I7 P1-1); a record is made only by fillRecord over storage sized for its words.
  SegRecord() noexcept = default;
  size_t size() const { return nWords; }  // in words
  const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(this + 1); }
  uint32_t* words() { return reinterpret_cast<uint32_t*>(this + 1); }
};
static_assert(std::is_trivially_destructible_v<SegRecord>, "I7 (1): the arena never runs a record destructor");
static_assert(sizeof(SegRecord) % 4u == 0 && alignof(SegRecord) <= 8u, "the words follow the header, 4-byte aligned");

namespace segarena {
constexpr uint32_t kChunkMin = 4u << 10, kChunkMax = 64u << 10;  // the first chunk, then doubling to 64 KiB
constexpr size_t kChunkKeep = 64, kChunkKeepBytes = 4u << 20;    // the free list's bound
constexpr uint64_t kRetainCap = 32ull << 20;                     // I7 (3): retired-but-pinned arenas' chunk bytes
constexpr size_t kAuditSpareCap = 4096;                          // recycled audit nodes kept
struct Chunk {
  Chunk* next = nullptr;
  uint32_t cap = 0, used = 0;
  uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }
};
static_assert(sizeof(Chunk) == 16 && std::is_trivially_destructible_v<Chunk>, "I7 (1): the chunk header");
std::atomic<uint64_t> g_arenas{0}, g_arenasMax{0}, g_chunks{0}, g_chunksMax{0}, g_chunkReuse{0}, g_chunkFreed{0},
    g_bytes{0}, g_bytesMax{0}, g_records{0}, g_solo{0}, g_nodeReuse{0}, g_oversized{0}, g_capFaults{0}, g_retained{0},
    g_retainedMax{0}, g_bad{0}, g_capLogged{0}, g_checks{0}, g_verifies{0}, g_fills{0};
#if SR_ORACLE
bool hashOn();  // SR_SEG_ARENA_HASH=1 (below): the opt-in for ALL of I7's dev-only checking work
#endif
std::atomic<int> g_forceOn{-1}, g_forceKeep{-1};  // tests: -1 = the env gate, 0 = off, 1 = on
std::atomic<uint64_t> g_capForTest{0};            // tests: a smaller retention cap (0 = kRetainCap)
uint64_t retainCap() {
  const uint64_t t = g_capForTest.load(std::memory_order_relaxed);
  return t ? t : kRetainCap;
}
void raiseMax(std::atomic<uint64_t>& m, uint64_t v) {
  uint64_t cur = m.load(std::memory_order_relaxed);
  while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}
bool envOn(const char* name) {
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}
bool on() {
  const int f = g_forceOn.load(std::memory_order_relaxed);
  if (f >= 0) return f != 0;
  static const bool v = [] {
    const bool o = envOn("SR_SEG_ARENA");
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SEG_ARENA] %s (SR_SEG_ARENA; step 6 I7: segment records in refcounted arenas)",
                o ? "on" : "OFF - one allocation per record");
    return o;
  }();
  return v;
}
bool keep() {
  const int f = g_forceKeep.load(std::memory_order_relaxed);
  if (f >= 0) return f != 0;
  static const bool v = envOn("SR_SEG_ARENA_KEEP");
  return v;
}
void poison(void* p, size_t n) {
  (void)p;
  (void)n;
#if SR_EP_ASAN
  ASAN_POISON_MEMORY_REGION(p, n);
#endif
}
void unpoison(void* p, size_t n) {
  (void)p;
  (void)n;
#if SR_EP_ASAN
  ASAN_UNPOISON_MEMORY_REGION(p, n);
#endif
}
// The chunk free list: never destroyed (an arena may die during static destruction, e.g. difftest's exit), its own
// mutex, taken last (nothing is locked while it is held).
struct FreeList {
  std::mutex mu;
  std::vector<Chunk*> byClass[5];  // 4, 8, 16, 32, 64 KiB
  size_t n = 0, bytes = 0;
};
FreeList& freeList() {
  static FreeList* f = [] {
    auto* p = new FreeList();
    for (auto& v : p->byClass) v.reserve(kChunkKeep);
    return p;
  }();
  return *f;
}
int chunkClass(uint32_t cap) {
  switch (cap) {
    case 4u << 10: return 0;
    case 8u << 10: return 1;
    case 16u << 10: return 2;
    case 32u << 10: return 3;
    case 64u << 10: return 4;
    default: return -1;  // an oversized record's exact chunk: never kept
  }
}
Chunk* chunkTake(uint32_t cap) {
  Chunk* c = nullptr;
  const int k = chunkClass(cap);
  if (k >= 0) {
    FreeList& f = freeList();
    std::lock_guard<std::mutex> lk(f.mu);
    if (!f.byClass[k].empty()) {
      c = f.byClass[k].back();
      f.byClass[k].pop_back();
      f.n--;
      f.bytes -= cap;
    }
  }
  if (c) {
    unpoison(c->data(), cap);  // I7 (4): unpoisoned the moment it leaves the free list
    g_chunkReuse.fetch_add(1, std::memory_order_relaxed);
  } else {
    c = new (::operator new(sizeof(Chunk) + size_t(cap))) Chunk();
    c->cap = cap;
    if (k < 0) g_oversized.fetch_add(1, std::memory_order_relaxed);
  }
  c->next = nullptr;
  c->used = 0;
  raiseMax(g_chunksMax, g_chunks.fetch_add(1, std::memory_order_relaxed) + 1);
  return c;
}
void chunkGive(Chunk* c) {
  g_chunks.fetch_sub(1, std::memory_order_relaxed);
  const uint32_t cap = c->cap;
#if SR_ORACLE
  // I7 (4): a read of a released record sees 0xDB, never a live record's bytes. OPT-IN with the checksums
  // (SR_SEG_ARENA_HASH=1; H2, the coordinator 10-24 19:3x): ~0.29 ms per present on the dev draw thread (hod-dev's
  // complete histogram, as VCRUNTIME memmove under chunkGive). ASan's poison below is unchanged.
  if (hashOn()) {
    std::memset(c->data(), 0xDB, cap);
    g_fills.fetch_add(1, std::memory_order_relaxed);
  }
#endif
  const int k = chunkClass(cap);
  if (k >= 0 && keep()) {
    poison(c->data(), cap);  // I7 (4): poisoned BEFORE it reaches the free list
    FreeList& f = freeList();
    std::lock_guard<std::mutex> lk(f.mu);
    if (f.n < kChunkKeep && f.bytes + cap <= kChunkKeepBytes) {
      f.byClass[k].push_back(c);
      f.n++;
      f.bytes += cap;
      return;
    }
  }
  unpoison(c->data(), cap);  // freed: the allocator's own state takes over (KEEP=0: a later use is a use-after-free)
  g_chunkFreed.fetch_add(1, std::memory_order_relaxed);
  c->~Chunk();
  ::operator delete(static_cast<void*>(c));
}
}  // namespace segarena

struct SegArena {
  segarena::Chunk* head = nullptr;  // the newest chunk: the bump allocation's
  uint64_t bytes = 0;               // its chunks' capacity
  uint32_t nextCap = segarena::kChunkMin;
  bool retired = false;             // counted in g_retained (set at the swap under g_segMu; read here after the last ref)
  SegArena() {
    segarena::raiseMax(segarena::g_arenasMax, segarena::g_arenas.fetch_add(1, std::memory_order_relaxed) + 1);
  }
  SegArena(const SegArena&) = delete;
  SegArena& operator=(const SegArena&) = delete;
  // The chunk bytes alloc(nWords) would add to this arena (0: it fits the newest chunk) - the cap check counts them
  // BEFORE allocating (Astra I7 P1-2), so retained + open never exceeds the cap and the open arena always retires.
  static uint32_t growFor(const segarena::Chunk* head, uint32_t nextCap, uint32_t nWords) {
    const uint32_t need = (uint32_t(sizeof(SegRecord)) + 4u * nWords + 7u) & ~7u;
    if (head && head->cap - head->used >= need) return 0;
    if (need > segarena::kChunkMax) return need;
    uint32_t cap = nextCap;
    while (cap < need) cap <<= 1;
    return cap;
  }
  // The last owning reference to any of its records went (any thread): the chunks go back (I7 (4): poisoned / 0xDB).
  ~SegArena() {
    for (segarena::Chunk* c = head; c;) {
      segarena::Chunk* n = c->next;
      segarena::chunkGive(c);
      c = n;
    }
    if (retired) segarena::g_retained.fetch_sub(bytes, std::memory_order_relaxed);
    segarena::g_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    segarena::g_arenas.fetch_sub(1, std::memory_order_relaxed);
  }
  // Under g_segMu (I7 (2)). 8-byte aligned slots; a record that does not fit the newest chunk gets a new one (the
  // next size up to 64 KiB; a record larger than that an exact-size chunk).
  SegRecord* alloc(uint32_t nWords) {
    const uint32_t need = (uint32_t(sizeof(SegRecord)) + 4u * nWords + 7u) & ~7u;
    if (!head || head->cap - head->used < need) {
      uint32_t cap = nextCap;
      if (need > segarena::kChunkMax) {
        cap = need;
      } else {
        while (cap < need) cap <<= 1;
        nextCap = std::min<uint32_t>(segarena::kChunkMax, cap << 1);
      }
      segarena::Chunk* c = segarena::chunkTake(cap);
      c->next = head;
      head = c;
      bytes += cap;
      segarena::raiseMax(segarena::g_bytesMax, segarena::g_bytes.fetch_add(cap, std::memory_order_relaxed) + cap);
    }
    SegRecord* r = reinterpret_cast<SegRecord*>(head->data() + head->used);
    head->used += need;
    return r;
  }
};

// The arena audit keeps its own reference until reclamation. Consumer progress
// belongs to the immutable record, not to an arena header which a later pass
// may find reused. Under g_segMu; CP completion itself needs no producer lock.
std::unordered_map<uint64_t, std::shared_ptr<SegRecord>> g_segRecordAudit;
// I7, all under g_segMu ONLY (condition 2: never read without it, never copied out of it): the open arena, the audit's
// recycled nodes (design (d)) and the open segment's spare footprint vector (design (e)).
std::shared_ptr<SegArena> g_segOpenArena;
std::vector<std::unordered_map<uint64_t, std::shared_ptr<SegRecord>>::node_type> g_segAuditSpare;
std::vector<SegFoot> g_segFeetSpare;
bool g_segArenaCapped = false;

// Step 6 I12 (the coordinator 10-24; lane C's J:/sr_lanec_drawthread/AUDIT_RING_DESIGN_v0): the reclaim audit as a
// pos-ordered FIFO RING. pos = g_head at the write is strictly increasing, the writer inserts in pos order
// (captureSegmentRecord) and the reclaim walks g_reclaim forward in pos order (recordExecutedForAudit): the map above was
// a FIFO keyed by an increasing integer - std::_Hash at 0.41 ms per present on the draw-submission thread (the I7
// window's seg1w profile). Under g_segMu ONLY, as the map (I7 condition 2). The LIFETIME is the map's: the ring holds
// exactly the references the map held and releases each at the same reclaim, under the same lock; an entry the reclaim
// walks past without a header match stays, as it stayed in the map. SR_AUDIT_RING=0 restores the map (the ABBA).
// Fallbacks, counted (expected 0 live): an insert at or below the newest pos (a test seam's order) - an equal live pos is
// the map's emplace no-op (the first record stays), a smaller one a sorted insert; a reclaim of a pos that is not the
// oldest (a binary search; its slot becomes a tombstone, popped when it reaches the front). An entry the reclaim walks
// past (a header the walk does not match: g_recDesync) stays live at the front, as the map kept it - every later reclaim
// is then a tombstone, so once the tombstones are over half the ring (and 64) it is COMPACTED (counted): the ring stays
// bounded by its live entries, as the map's node count was.
namespace auditring {
std::atomic<int> g_force{-1};   // tests: -1 = SR_AUDIT_RING, 0 = the map, 1 = the ring (a test's positions are its own)
bool on() {
  const int f = g_force.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool env = [] {
    const char* e = std::getenv("SR_AUDIT_RING");
    return !(e && e[0] == '0');
  }();
  return env;
}
struct Slot {
  uint64_t pos = 0;
  std::shared_ptr<SegRecord> rec;
  bool dead = false;
};
std::vector<Slot> g_ring;   // empty, or a power of two
size_t g_first = 0, g_n = 0, g_deadN = 0;   // g_deadN: the tombstones among the g_n
// Counters: written under g_segMu only (a plain load + store, no locked RMW), read anywhere.
std::atomic<uint64_t> g_slowInserts{0}, g_dupes{0}, g_tombs{0}, g_grows{0}, g_hits{0}, g_misses{0}, g_compacts{0};
inline void bump(std::atomic<uint64_t>& c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
inline Slot& at(size_t i) { return g_ring[(g_first + i) & (g_ring.size() - 1)]; }
void grow() {
  std::vector<Slot> n(g_ring.empty() ? 1024 : 2 * g_ring.size());
  for (size_t i = 0; i < g_n; i++) n[i] = std::move(at(i));
  g_ring.swap(n);
  g_first = 0;
  bump(g_grows);
}
void compact() {   // drop every tombstone, the live entries in order
  size_t k = 0;
  for (size_t i = 0; i < g_n; i++) {
    if (at(i).dead) continue;
    if (k != i) at(k) = std::move(at(i));
    k++;
  }
  for (size_t i = k; i < g_n; i++) {
    at(i).rec.reset();
    at(i).dead = false;
  }
  g_n = k;
  g_deadN = 0;
  bump(g_compacts);
}
size_t lowerBound(uint64_t pos) {   // the first index whose pos >= pos
  size_t lo = 0, hi = g_n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (at(mid).pos < pos) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
// -> false when a LIVE entry holds pos already (the map's emplace no-op). `reused`: stored without an allocation.
bool insert(uint64_t pos, const std::shared_ptr<SegRecord>& rec, bool& reused) {
  reused = false;
  if (g_n == 0 || pos > at(g_n - 1).pos) {   // the writer's order: an append
    if (g_n == g_ring.size()) grow();
    else reused = true;
    Slot& s = at(g_n);
    s.pos = pos;
    s.rec = rec;
    s.dead = false;
    g_n++;
    return true;
  }
  bump(g_slowInserts);
  const size_t i = lowerBound(pos);
  if (i < g_n && at(i).pos == pos) {
    if (!at(i).dead) {
      bump(g_dupes);
      return false;
    }
    at(i).rec = rec;   // a tombstone at pos: the map had no entry there - this is an insert
    at(i).dead = false;
    g_deadN--;
    return true;
  }
  if (g_n == g_ring.size()) grow();
  for (size_t j = g_n; j > i; j--) at(j) = std::move(at(j - 1));
  Slot& s = at(i);
  s.pos = pos;
  s.rec = rec;
  s.dead = false;
  g_n++;
  return true;
}
void popDead() {
  while (g_n && at(0).dead) {
    at(0).dead = false;
    g_first = (g_first + 1) & (g_ring.size() - 1);
    g_n--;
    g_deadN--;
  }
}
// Step 6 I14b (J:/sr_lanec_drawthread/FREEZE_I14B_20261024.txt): the reclaim's read of a record's `completed` byte was
// a cache MISS on the draw thread (99 % of recordExecutedForAudit's self on RAWPC, ~0.33 ms per present: the CP wrote
// that header about one arena cycle earlier). A HINT only: the next reclaims' record headers are prefetched one
// record write ahead - no read, order, verdict or lifetime changes. SR_AUDIT_PREFETCH=0 turns it off (the A/B arm).
bool prefetchOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_AUDIT_PREFETCH");
    return !(e && e[0] == '0');
  }();
  return on;
}
inline void prefetchHeader(const void* p) {
  if (p) _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
}
// The audit's entry at pos, moved out (a hit) - or nullptr (a miss: not a segment record, or already reclaimed).
std::shared_ptr<SegRecord> take(uint64_t pos) {
  if (!g_n) return nullptr;
  if (at(0).pos == pos && !at(0).dead) {   // the reclaim's order: the oldest
    std::shared_ptr<SegRecord> r = std::move(at(0).rec);
    g_first = (g_first + 1) & (g_ring.size() - 1);
    g_n--;
    popDead();
    if (prefetchOn())  // I14b: the next two live entries are the next reclaims' (their `completed` bytes)
      for (size_t k = 0; k < 2 && k < g_n; k++)
        if (!at(k).dead) prefetchHeader(at(k).rec.get());
    return r;
  }
  const size_t i = lowerBound(pos);
  if (i >= g_n || at(i).pos != pos || at(i).dead) return nullptr;
  std::shared_ptr<SegRecord> r = std::move(at(i).rec);
  at(i).dead = true;
  g_deadN++;
  bump(g_tombs);
  if (g_deadN >= 64 && 2 * g_deadN > g_n) compact();   // a stuck front (an entry the walk passed): stay bounded
  return r;
}
}  // namespace auditring

namespace segarena {
// The record checksum (SR_ORACLE): FNV-1a's step over 8-byte words (the tail bytes one at a time), folded to 32 bits. It
// was byte-at-a-time FNV-1a 32: 15.8 % of the draw-submission thread's CPU and 11.5 % of the CP's in the I7 window's
// profiled seg1w roll (lane C 10-24 15:5x, J:/sr_lanec_drawthread) - a dev-build cost on the critical path. Any changed
// byte still changes the word it is in, and so the hash. OPT-IN with condition 5's folds (SR_SEG_ARENA_HASH=1, the
// coordinator 10-24 17:2x: dev timing builds must not carry costs release never has): unset, no record is checksummed
// at capture and none verified (check = 0; checks / verifies count the work).
#if SR_ORACLE
bool hashOn();
#endif
uint32_t recordCheck(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 1099511628211ull;
  }
  for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
  return uint32_t(h ^ (h >> 32));
}
void fillRecord(SegRecord* r, const uint8_t* source, uint32_t bytes) {
  const uint32_t n = (bytes + 3u) / 4u;
  new (r) SegRecord();
  r->nWords = n;
  if (n) r->words()[n - 1] = 0;  // the pad bytes past `bytes` read as zero, as the old zero-filled vector's did
  std::memcpy(r->words(), source, bytes);
#if SR_ORACLE
  if (hashOn()) {
    r->check = recordCheck(r->data(), 4u * size_t(n));
    g_checks.fetch_add(1, std::memory_order_relaxed);
  }
#endif
}
struct SoloDelete {
  void operator()(SegRecord* r) const noexcept { ::operator delete(static_cast<void*>(r)); }
};
#if SR_ORACLE
// Condition (5)'s capture checksums: every record (pos, bytes) and every buildParts result (kinds, lengths, positions,
// gap bytes), folded in order; the [SEG_ARENA] final line prints them (a run under =1 and =0 must match). OPT-IN,
// SR_SEG_ARENA_HASH=1 (the condition (5) suite runs set it): folded byte-wise under a mutex on the draw-submission thread
// it was 8.3 % of that thread's CPU in the profiled seg1w roll; off, the final line prints capture_hash=off.
bool hashOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_SEG_ARENA_HASH");
    return e && e[0] == '1';
  }();
  return on;
}
std::mutex g_hashMu;
uint64_t g_capHash = 1469598103934665603ull, g_partsHash = 1469598103934665603ull;
uint64_t g_auditHash = 1469598103934665603ull;   // I12: every reclaim's (pos, hit) in order - SR_AUDIT_RING=1 vs =0
void fold(uint64_t& h, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
}
void auditFold(uint64_t pos, bool hit) {   // under g_segMu (the reclaim's); then g_hashMu, the capture's order
  if (!hashOn()) return;
  std::lock_guard<std::mutex> hl(g_hashMu);
  const uint64_t v[2] = {pos, hit ? 1ull : 0ull};
  fold(g_auditHash, v, sizeof v);
}
// A record's bytes against its capture checksum (the CP's apply, the audit's reclaim): a mismatch is BAD.
void verify(const SegRecord& r, const char* where) {
  if (!hashOn()) return;  // opt-in (SR_SEG_ARENA_HASH=1): no capture checksum to verify against
  g_verifies.fetch_add(1, std::memory_order_relaxed);
  if (recordCheck(r.data(), 4u * r.size()) == r.check) return;
  const uint64_t n = g_bad.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8)
    SR_LOG_BOOT(sr::LogSeverity::Error, "[SEG_ARENA] BAD #%llu: a record's bytes changed after its capture (at %s)",
                (unsigned long long)n, where);
}
#endif
void milestone(uint64_t n) {
  if (n != 1 && n != 1000 && n % 1000000u != 0) return;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[SEG_ARENA] %s records=%llu solo=%llu arenas=%llu/%llu chunks=%llu/%llu reuse=%llu freed=%llu "
              "bytes=%llu/%llu retained=%llu/%llu oversized=%llu node_reuse=%llu cap_faults=%llu bad=%llu",
              on() ? "on" : "OFF", (unsigned long long)n, (unsigned long long)g_solo.load(),
              (unsigned long long)g_arenas.load(), (unsigned long long)g_arenasMax.load(),
              (unsigned long long)g_chunks.load(), (unsigned long long)g_chunksMax.load(),
              (unsigned long long)g_chunkReuse.load(), (unsigned long long)g_chunkFreed.load(),
              (unsigned long long)g_bytes.load(), (unsigned long long)g_bytesMax.load(),
              (unsigned long long)g_retained.load(), (unsigned long long)g_retainedMax.load(),
              (unsigned long long)g_oversized.load(), (unsigned long long)g_nodeReuse.load(),
              (unsigned long long)g_capFaults.load(), (unsigned long long)g_bad.load());
}
// I7 (3): under g_segMu. A record whose allocation would put the retired arenas' bytes plus the open one's (with the
// chunk it would add) over the cap takes the =0 path, until it is back under; every entry into that state is a
// REPORTED FAULT - under SR_ORACLE an ERROR line for EVERY entry (Astra I7 P1-3), in release a rate-limited WARN.
bool cappedLocked(uint64_t grow) {
  const uint64_t open = g_segOpenArena ? g_segOpenArena->bytes : 0;
  const bool over = g_retained.load(std::memory_order_relaxed) + open + grow > retainCap();
  if (over && !g_segArenaCapped) {
    const uint64_t n = g_capFaults.fetch_add(1, std::memory_order_relaxed) + 1;
#if SR_ORACLE
    const bool say = true;
    const sr::LogSeverity sev = sr::LogSeverity::Error;
#else
    const bool say = n <= 4 || (n & (n - 1)) == 0;
    const sr::LogSeverity sev = sr::LogSeverity::Warn;
#endif
    if (say) {
      g_capLogged.fetch_add(1, std::memory_order_relaxed);
      SR_LOG_BOOT(sev, "[SEG_ARENA] RETAIN CAP #%llu: %llu arenas hold %llu retired + %llu open bytes (+%llu) > %llu - "
                  "new records take the per-record path until it is under the cap",
                  (unsigned long long)n, (unsigned long long)g_arenas.load(),
                  (unsigned long long)g_retained.load(), (unsigned long long)open, (unsigned long long)grow,
                  (unsigned long long)retainCap());
    }
  }
  g_segArenaCapped = over;
  return over;
}
// I7 (3) + design (b): at a segment's close, under g_segMu. The open arena is retired (counted) unless that would put
// the retained total over the cap - then it stays open (it takes no records while capped), retired at a later close.
void retireOpenLocked() {
  if (!g_segOpenArena) return;
  SegArena& a = *g_segOpenArena;
  if (g_retained.load(std::memory_order_relaxed) + a.bytes > retainCap()) {  // unreachable while captures check growth
    cappedLocked(0);
    return;
  }
  a.retired = true;
  raiseMax(g_retainedMax, g_retained.fetch_add(a.bytes, std::memory_order_relaxed) + a.bytes);
  g_segOpenArena.reset();  // only the global's reference: the arena lives on through its records
}
}  // namespace segarena

// Under g_segMu (the writer holds g_mu too). Returns the record's ONLY kind of owning reference.
std::shared_ptr<SegRecord> captureSegmentRecord(const uint8_t* source, uint32_t bytes, uint64_t pos) {
  const uint32_t nWords = (bytes + 3u) / 4u;
  std::shared_ptr<SegRecord> record;
  const bool arena = segarena::on();
  const uint32_t grow = !arena ? 0u
                        : g_segOpenArena ? SegArena::growFor(g_segOpenArena->head, g_segOpenArena->nextCap, nWords)
                                         : SegArena::growFor(nullptr, segarena::kChunkMin, nWords);
  if (arena && !segarena::cappedLocked(grow)) {
    if (!g_segOpenArena) g_segOpenArena = std::make_shared<SegArena>();
    SegRecord* r = g_segOpenArena->alloc(nWords);
    segarena::fillRecord(r, source, bytes);
    record = std::shared_ptr<SegRecord>(g_segOpenArena, r);  // the ALIASING constructor: ownership = the arena
  } else {
    SegRecord* r = static_cast<SegRecord*>(::operator new(sizeof(SegRecord) + 4u * size_t(nWords)));
    segarena::fillRecord(r, source, bytes);
    record = std::shared_ptr<SegRecord>(r, segarena::SoloDelete{});
    if (arena) segarena::g_solo.fetch_add(1, std::memory_order_relaxed);
  }
#if SR_ORACLE
  if (segarena::hashOn()) {
    std::lock_guard<std::mutex> hl(segarena::g_hashMu);
    segarena::fold(segarena::g_capHash, &pos, sizeof pos);
    segarena::fold(segarena::g_capHash, record->data(), 4u * size_t(nWords));
  }
#endif
  if (auditring::on()) {  // I12: the ring (an append in the writer's pos order; no allocation once sized)
    bool reused = false;
    auditring::insert(pos, record, reused);
    if (arena && reused) segarena::g_nodeReuse.fetch_add(1, std::memory_order_relaxed);
  } else if (arena && !g_segAuditSpare.empty()) {  // design (d): a recycled node, no allocation
    auto node = std::move(g_segAuditSpare.back());
    g_segAuditSpare.pop_back();
    node.key() = pos;
    node.mapped() = record;
    auto ins = g_segRecordAudit.insert(std::move(node));
    if (ins.inserted) {
      segarena::g_nodeReuse.fetch_add(1, std::memory_order_relaxed);
    } else {  // a key already present: emplace's no-op, the node goes back
      ins.node.mapped().reset();
      g_segAuditSpare.push_back(std::move(ins.node));
    }
  } else {
    g_segRecordAudit.emplace(pos, record);
  }
  segarena::milestone(segarena::g_records.fetch_add(1, std::memory_order_relaxed) + 1);
  return record;
}
// A record built from bytes outside the capture path (difftest's record suites: an owned record applied directly) -
// the per-record form, sized for its words (Astra I7 P1-1).
std::shared_ptr<SegRecord> segRecordFromBytes(const uint8_t* source, uint32_t bytes) {
  const uint32_t nWords = (bytes + 3u) / 4u;
  SegRecord* r = static_cast<SegRecord*>(::operator new(sizeof(SegRecord) + 4u * size_t(nWords)));
  segarena::fillRecord(r, source, bytes);
  return std::shared_ptr<SegRecord>(r, segarena::SoloDelete{});
}
bool recordExecutedForAudit(uint64_t pos, const uint8_t* slot) {
  std::lock_guard<std::mutex> lk(g_segMu);
  if (auditring::on()) {  // I12: the oldest entry in the reclaim's order (a binary search otherwise)
    std::shared_ptr<SegRecord> r = auditring::take(pos);
#if SR_ORACLE
    segarena::auditFold(pos, r != nullptr);
#endif
    if (!r) {
      auditring::bump(auditring::g_misses);
      return __atomic_load_n(&reinterpret_cast<const RecHdr*>(slot)->exec, __ATOMIC_ACQUIRE) != 0;
    }
    auditring::bump(auditring::g_hits);
    const bool completed = r->completed.load(std::memory_order_acquire);
#if SR_ORACLE
    segarena::verify(*r, "the audit's reclaim");
#endif
    return completed;  // `r` (the audit's reference) drops here, under g_segMu - as the map's reset() did
  }
  auto it = g_segRecordAudit.find(pos);
#if SR_ORACLE
  segarena::auditFold(pos, it != g_segRecordAudit.end());
#endif
  if (it == g_segRecordAudit.end()) {
    auditring::bump(auditring::g_misses);
    return __atomic_load_n(&reinterpret_cast<const RecHdr*>(slot)->exec, __ATOMIC_ACQUIRE) != 0;
  }
  auditring::bump(auditring::g_hits);
  const bool completed = it->second->completed.load(std::memory_order_acquire);
#if SR_ORACLE
  segarena::verify(*it->second, "the audit's reclaim");
#endif
  if (segarena::on() && g_segAuditSpare.size() < segarena::kAuditSpareCap) {  // design (d): keep the node
    auto node = g_segRecordAudit.extract(it);
    node.mapped().reset();  // the audit's reference (may release the arena: its chunks, never the map's lock)
    if (g_segAuditSpare.capacity() == 0) g_segAuditSpare.reserve(segarena::kAuditSpareCap);
    g_segAuditSpare.push_back(std::move(node));
  } else {
    g_segRecordAudit.erase(it);
  }
  return completed;
}
// The difftest fixtures compiled into this file (tools/difftest/*_test.inc): an arm's reset empties the audit - BOTH
// structures, every reference released (as the map's clear()); a fixture's check reads the audit's record at pos from
// whichever arm is on. Under g_segMu, as the map's.
void auditClearLocked() {
  g_segRecordAudit.clear();
  for (size_t i = 0; i < auditring::g_n; i++) {
    auditring::at(i).rec.reset();
    auditring::at(i).dead = false;
  }
  auditring::g_first = auditring::g_n = auditring::g_deadN = 0;
}
std::shared_ptr<SegRecord> auditFindLocked(uint64_t pos) {
  if (auditring::on()) {
    const size_t i = auditring::lowerBound(pos);
    if (i < auditring::g_n && auditring::at(i).pos == pos && !auditring::at(i).dead) return auditring::at(i).rec;
    return nullptr;
  }
  auto it = g_segRecordAudit.find(pos);
  return it == g_segRecordAudit.end() ? nullptr : it->second;
}

// SR_NATIVE_SEG_TILE (observe-only): the tile + first-application checks (kernel/seg_tile.hpp). Every call is
// behind g_segTile. g_tileMu guards both (the producer and the CP thread; segReport reads them).
std::mutex g_tileMu;
sr::native::segtile::Tile g_tile;
sr::native::segtile::First g_first;
// The drained reconciliation (segTileDrained): the guest's BlockUntilIdle returns checked, those inside a Z-pass
// bracket skipped (its deferred segments wait for the guest's replay), and those where registered != first-applied.
std::atomic<uint64_t> g_tileDrains{0}, g_tileDrainBracket{0}, g_tileDrainMiss{0};
// The FORCED drained reconciliation (segTileForcedDrain): the game drains itself (BlockUntilIdle) only while it
// loads, so gameplay has no drained point. Every SR_NATIVE_SEG_TILE_DRAIN_MS (default 10000; 0 = off) a segmentless
// close waits (<= 200 ms) until the transport is drained and compares registered with first-applied. Postponed (the
// next close retries): inside a Z-pass bracket, or not drained in time.
std::atomic<uint64_t> g_tileForced{0}, g_tileForcedMiss{0}, g_tileForcedPostponed{0};
// A registered segment's record parts -> the tile (their sizes from the arena headers the writer wrote).
void tileSegment(const std::vector<SegPart>& parts);

#if !SR_ORACLE
[[noreturn]] void nativeAdmissionFatal(const char* reason) {
  SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_BAD] native admission: %s", reason);
  sr::Log::shutdown();
  std::_Exit(3);
}
#endif

// ---- the flush model: PM4-level items ----
// A type-0 register run (pkt 0: reg, count, src or lit), or a type-3 packet (pkt 1: hdr, bodyN
// literal body dwords, then `count` dwords of big-endian bytes at src - the immediate ucode). A
// packet's record kind rk (native_flush.hpp RecKind) says what the CP applies; kRkNoRecord = the
// packet has no CP effect (INVALIDATE_STATE) and is only bytes the XDK would have written.
constexpr uint8_t kRkNoRecord = 0xFF;
struct Item {
  uint16_t pad;      // type-2 NOPs before the packet
  uint8_t pkt;       // 1 = type-3 packet
  uint8_t rk;        // pkt: RecKind (kRkDraw = DRAW_INDX) or kRkNoRecord
  uint32_t reg, count;
  const uint8_t* src;  // big-endian values (device bytes) ; nullptr = lit
  uint32_t lit[6];     // literal values (host order) when src == nullptr (6: the UP fetch slot)
  uint32_t drawHdr, body[4], bodyN;  // pkt: the type-3 header (predicate bit 0 included) and literal body
  bool chk;           // S2a CHECK: a run the dedup predicts resident (record tag bit 30, the CP checks it)
  uint32_t fixedBytes;  // an OPAQUE record (strand B's kinds): the XDK bytes it stands for; its PM4 form is the owner's renderer
  uint8_t deferSrc;   // Entry items: src is resolved from the payload copy right before the span is written (lit[0] = offset)
  uint32_t bytes() const { return 4u * pad + (fixedBytes ? fixedBytes : 4u + 4u * (pkt ? bodyN + count : count)); }
};

#if SR_ORACLE
void renderPm4(const Item& it, uint8_t* out) {
  uint8_t* p = out;
  for (uint32_t k = 0; k < it.pad; k++, p += 4) std::memcpy(p, &kNopBE, 4);
  if (it.fixedBytes) {
    if (it.rk == kRkFence) {  // GpuBuildFenceWritePacket's 10 dwords: INVALIDATE_STATE 0x7FFF, EVENT_WRITE_SHD x2
      const uint32_t v[10] = {0xC0003B00u, 0x7FFFu, 0xC0025800u, 3u, it.body[0], it.body[1], 0xC0025800u, 3u, it.body[2], it.body[3]};
      for (uint32_t k = 0; k < 10; k++) put32(p + 4u * k, v[k]);
      return;
    }
    if (it.rk == kRkInvalidate) {  // GpuBuildFlushMemoryPackets' 11 dwords: COHER_STATUS_HOST, COHER_SIZE/BASE, WAIT_REG_MEM
      const uint32_t v[11] = {0x00000A31u, it.body[2] ? 0x01000000u : 0x03000000u, 0x00010A2Fu, it.body[1], it.body[0],
                              0xC0043C00u, 3u, 0x0A31u, 0u, 0x80000000u, 8u};
      for (uint32_t k = 0; k < 11; k++) put32(p + 4u * k, v[k]);
      return;
    }
    // An opaque item written as PM4 (a span shorter than the marker, or the CPPM4 split): the
    // owner's renderer, else type-2 NOPs (counted by the caller).
    if (OpaqueRenderer r = opaqueRenderer(it.rk)) {
      r(it.rk, reinterpret_cast<const uint32_t*>(it.src), it.count, p, it.fixedBytes);
    } else {
      for (uint32_t k = 0; k < it.fixedBytes / 4u; k++) std::memcpy(p + 4u * k, &kNopBE, 4);
    }
    return;
  }
  if (it.pkt) {
    put32(p, it.drawHdr);
    for (uint32_t k = 0; k < it.bodyN; k++) put32(p + 4 + 4 * k, it.body[k]);
    if (it.count) std::memcpy(p + 4 + 4 * it.bodyN, it.src, 4u * it.count);
    return;
  }
  put32(p, ((it.count - 1u) << 16) | it.reg);
  if (it.src) {
    std::memcpy(p + 4, it.src, 4u * it.count);
  } else {
    for (uint32_t k = 0; k < it.count; k++) put32(p + 4 + 4 * k, it.lit[k]);
  }
}
#endif

// Guest memory writes that may cross a host mapping boundary.
#if SR_ORACLE
void writeGuest(uint32_t va, const uint8_t* src, uint32_t n) {
  uint8_t* h0 = sr::kernel::guestVAToHost(va);
  uint8_t* h1 = n ? sr::kernel::guestVAToHost(va + n - 1u) : h0;
  if (h0 && h1 && h1 - h0 == ptrdiff_t(n) - 1) {
    std::memcpy(h0, src, n);
    return;
  }
  for (uint32_t i = 0; i < n; i += 4)
    if (uint8_t* h = sr::kernel::guestVAToHost(va + i)) std::memcpy(h, src + i, std::min<uint32_t>(4u, n - i));
}
#endif

// ---- the driver ----
struct Driver {
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
  uint8_t* d = nullptr;  // device bytes: the real device (record mode) or a copy (dry)
  uint32_t dev = 0;
  bool dry = false;
  bool stopped = false;          // dry: a guest call / kick was needed - the prefix is final
  uint32_t c = 0;                // cursor = VA of the last dword written
  uint32_t spanStart = 0;
  // The open span (record mode) / every item (dry): per-thread buffers, no allocation per draw.
  static constexpr uint32_t kSpanCap = 192, kAllCap = 1024;
  Item* items = nullptr;
  uint32_t nItems = 0;
  Item* all = nullptr;
  uint32_t nAll = 0;
  uint64_t guestTsc = 0;  // time inside guest calls (shader bind, leaves, kicks)
  // p1.flush_residue (SR_NATIVE_FLUSH_SPLIT and its A/B window, decided once per draw): sections chunk at the segment
  // limit (chunkRun) and draws over 0xFFFF indices split (drawPacket) instead of the guest leaf / body.
  bool split = false;
  // record mode output
  uint64_t recPos = 0;
  // An independently allocated tail returns its footprint directly to its owner.
  // The record is captured before g_mu is released and never joins g_segFeet.
  SegFoot* detachedFoot = nullptr;
  // S2a: records carry only the constant groups the stream does not already hold (the dedup core,
  // native_const_dedup: SR_NATIVE_CONST_DEDUP skips, _CHECK flags them for the CP). The XDK
  // length is unchanged (the span's marker covers the skipped bytes).
  bool ddWant = false;  // the core is active for this flush
  bool dd = false;      // the stream position is known: decisions valid, emitted groups stored
  bool ddSkip = false;  // leave resident groups out (not CHECK, not an A/B off window)
  bool ddChk = false;   // CHECK: predicted groups travel flagged
  uint32_t ddSeen = 0, ddPred = 0, ddSkipped = 0;
  void ddSync() {
    if (ddWant) dd = sr::native::cdedup::syncStream(base, dev, d, c);
  }

  uint32_t lim() const { return be32(d + 44); }
  // SR_DIAG_ZPASS_LEAF (above, at flushSplitWindow): true = this section, which does not fit before the segment limit,
  // goes to the guest leaf - the lever is on, the split is on for this draw (otherwise the leaf takes it anyway) and a
  // Z-pass recording pins the command buffer. Counted per leaf in record mode, the first one logged.
  bool zpassLeaf(int leaf) {
    if (!g_zpassLeaf || !split || be32(d + 0x3290u) == 0u) return false;
    if (!dry && g_zpassLeafSec[0].get() + g_zpassLeafSec[1].get() + g_zpassLeafSec[2].get() == 0u)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_FLUSH] SR_DIAG_ZPASS_LEAF: the first S1 section at the segment limit inside a Z-pass recording "
                  "goes to the guest leaf %d (dev=%08X cursor=%08X limit=%08X pin=%08X) - its PM4 is a gap of a segment "
                  "the bracket replays", leaf, dev, c, lim(), be32(d + 0x3290u));
    if (!dry) g_zpassLeafSec[leaf].fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  uint32_t thr() const { return be32(d + 48); }

  void add(const Item& it) {
    if (dry) {
      if (stopped) return;
      if (nAll >= kAllCap) {  // too long to keep: the prefix so far is compared
        stopped = true;
        return;
      }
      all[nAll++] = it;
    } else {
      if (nItems == kSpanCap) closeSpan();  // a span may end between any two packets
      items[nItems++] = it;
    }
    c += it.bytes();
  }

  // Record mode: write the open span (marker + record, or the XDK bytes when shorter than 16).
  void closeSpan() {
    if (!g_on && !g_check) return;  // the gates (SR_NATIVE_FLUSH / _CHECK) at every write site
    if (dry || (!nItems && c == spanStart)) {
      if (!dry) put32(d + 40, c);
      return;
    }
    const uint32_t len = c - spanStart;
    g_ringBytes.fetch_add(len, std::memory_order_relaxed);
    // A span shorter than the 4-dword marker can hold no skipped group (a group is 64 bytes), so it is
    // always the XDK's own bytes. Before the root-cause fix an ABORTED section left spanSkip set on an
    // 8-byte span: its marker (count 0, unrecognizable to the CP) overran the next packet, which the
    // guest leaf then overwrote - the lone 0x2203 window-scissor write was lost (the 66-102
    // never-executed records per run; 0 since). Counted in case it ever recurs.
    if (len < 16u && spanSkip) g_shortSkipSpans.fetch_add(1, std::memory_order_relaxed);
    const bool segNow = detachedFoot || segmentlessActive();
#if SR_ORACLE
    // An opaque span can be literal PM4 only through its owners' renderers (every opaque item needs one).
    bool opaqueRenderable = true;
    if (opaque)
      for (uint32_t i = 0; i < nItems; i++)
        if (items[i].fixedBytes && !opaqueRenderer(items[i].rk)) opaqueRenderable = false;
    // Segmentless: a span of any length is a record (there is no marker to fit); the CPPM4 split still
    // writes literal PM4, which the close then carries as a gap part - the oracle run of P2-B.
    // forceLiteral (P2-B item 5, the ring path only): the span holds a body the CALLER fills after the call;
    // a marker over it would be overwritten. Every such item has a renderer.
    const bool literal = (!segNow && forceLiteral) ||
                         (segNow ? (g_pm4Out && !spanSkip && (!opaque || opaqueRenderable))
                                 : (len < 16u || (g_pm4Out && !spanSkip && (!opaque || opaqueRenderable))));
    if (literal) {
      if (opaque && !opaqueRenderable) g_opaqueNoRender.fetch_add(1, std::memory_order_relaxed);
      // The items' PM4 must be exactly the span (the cursor's bytes): a longer render cut its tail off and the
      // CP read past the span; a shorter one left stale buffer bytes in it (litLenBad=, must stay 0).
      uint32_t total = 0;
      for (uint32_t i = 0; i < nItems; i++) total += items[i].bytes();
      if (total != len) litLenBad(total, len);
      if (!segNow) litCensus();
      thread_local std::vector<uint8_t> t_buf;
      if (t_buf.size() < std::max(len, total)) t_buf.resize(std::max(len, total));
      uint8_t* buf = t_buf.data();
      uint32_t o = 0;
      for (uint32_t i = 0; i < nItems; i++) {
        renderPm4(items[i], buf + o);
        o += items[i].bytes();
      }
      writeGuest(spanStart + 4u, buf, len);
      g_spansPm4.fetch_add(1, std::memory_order_relaxed);
    } else {
#else
    if (!segNow) nativeAdmissionFatal("span has no native segment");
    {
#endif
      // The record's exact size, from a first pass over the items (the writer below mirrors it). It is
      // NOT bounded by the span's PM4 bytes: a CONST_LOAD record is 4 words against a 3-word packet and
      // an opaque record is a tag plus a payload of up to its XDK bytes, so a span of several such items
      // outgrew the old `len + 16` bound (strand B's job49: a GPR entry of ~6 opaque items landed on the
      // arena's last page and the copy ran past the VirtualAlloc'd arena - THREAD_FATAL in closeSpan).
      uint32_t recWordsN = 0;
      for (uint32_t i = 0; i < nItems; i++) {
        const Item& it = items[i];
        if (it.pkt) {
          switch (it.rk) {
            case kRkNoRecord: break;
            case kRkDraw: recWordsN += 4; break;
            case kRkShaderPtr: recWordsN += 3; break;
            case kRkShaderImm: recWordsN += 3 + it.count; break;
            case kRkConstLoad: recWordsN += 4; break;
            case kRkSetConst: recWordsN += 1 + it.bodyN; break;  // tag, offset word, bodyN - 1 values
            case kRkBinReg: recWordsN += 2; break;
            case kRkEvent: recWordsN += 2; break;
            case kRkZpd: recWordsN += 2; break;  // p2.zpd's bake: tag + initiator
            case kRkFence: recWordsN += 5; break;
            case kRkInvalidate: recWordsN += 4; break;
            case kRkRegRmw: case kRkEventShd: recWordsN += 4; break;  // boot.device_create: tag + 3 words
            default: if (recOpaque(it.rk)) recWordsN += 1 + it.count; break;
          }
        } else if (it.count) {
          recWordsN += 1 + it.count;
        }
      }
      const uint32_t bound = uint32_t(sizeof(RecHdr)) + 4u * recWordsN;
      uint64_t pos = g_head;
      if ((pos % kRecArena) + bound > kRecArena) pos += kRecArena - (pos % kRecArena);  // no straddling
      if (g_segTile && pos != g_head) {  // SR_NATIVE_SEG_TILE: the padding is claimed, never a gap
        std::lock_guard<std::mutex> lk(g_tileMu);
        g_tile.pad(g_head, pos);
      }
      // The reclaim audit (iter-467 flicker hunt): every record the arena is about to overwrite must
      // have been executed by the CP at least once - one that never was is a draw span the frame lost.
      while (g_reclaim + kRecArena < pos + bound) {
        RecHdr oh;
        std::memcpy(&oh, g_arena + (g_reclaim % kRecArena), sizeof(oh));
        if (oh.magic == kRecMagic && oh.pos == g_reclaim && oh.bytes >= sizeof(RecHdr) && oh.bytes <= kMaxDrawRec) {
          g_recAudited.fetch_add(1, std::memory_order_relaxed);
          if (!recordExecutedForAudit(g_reclaim, g_arena + (g_reclaim % kRecArena))) {
            const uint64_t n = g_recDropped.fetch_add(1, std::memory_order_relaxed) + 1;
            uint32_t tag0 = 0;
            if (oh.bytes > sizeof(RecHdr)) std::memcpy(&tag0, g_arena + (g_reclaim % kRecArena) + sizeof(RecHdr), 4);
            if (n <= 24 || (n & (n - 1)) == 0)
              SR_LOG_BOOT(sr::LogSeverity::Error,
                          "[NATIVE_FLUSH_DROP] #%llu record pos=%llu bytes=%u was never executed by the CP "
                          "(head=%llu cpTail=%llu) first word %08X (%s kind=%u reg=%04X count=%u)",
                          (unsigned long long)n, (unsigned long long)g_reclaim, oh.bytes, (unsigned long long)g_head,
                          (unsigned long long)g_cpTail.load(std::memory_order_relaxed), tag0,
                          (tag0 & 0x80000000u) ? "PACKET" : "RUN", (tag0 & 0x80000000u) ? (tag0 >> 24) & 0x7Fu : 0u,
                          tag0 & 0x7FFFu, (tag0 >> 16) & 0x3FFFu);
          }
          g_reclaim += oh.bytes;
        } else {
          // the padding before an arena boundary (no straddling)
          const uint64_t toEnd = kRecArena - (g_reclaim % kRecArena);
          if (toEnd == kRecArena) g_recDesync.fetch_add(1, std::memory_order_relaxed);
          g_reclaim += toEnd;
        }
        // I14b (a hint only): the next slot's header, which this walk or the next record write's reads - its kRecMagic
        // compare was a load miss on RAWPC (~0.067 ms per present). Always inside g_arena.
        if (auditring::prefetchOn()) auditring::prefetchHeader(g_arena + (g_reclaim % kRecArena));
      }
      uint8_t* r = g_arena + (pos % kRecArena);
      uint32_t* w = reinterpret_cast<uint32_t*>(r + sizeof(RecHdr));
      for (uint32_t i = 0; i < nItems; i++) {
        const Item& it = items[i];
        if (it.pkt) {
          if (it.rk == kRkNoRecord) continue;
          const uint32_t pred = (it.drawHdr & 1u) ? 2u : 0u;
          switch (it.rk) {
            case kRkDraw: {
              const bool indexed = it.bodyN >= 4u;
              *w++ = recTag(kRkDraw, 0u, (indexed ? 1u : 0u) | pred);
              *w++ = it.body[1];
              *w++ = indexed ? it.body[2] : 0u;
              *w++ = indexed ? it.body[3] : 0u;
              break;
            }
            case kRkShaderPtr:
              *w++ = recTag(kRkShaderPtr, 0u, pred);
              *w++ = it.body[0];
              *w++ = it.body[1];
              break;
            case kRkShaderImm:
              *w++ = recTag(kRkShaderImm, it.count, pred);
              *w++ = it.body[0];
              *w++ = it.body[1];
              std::memcpy(w, it.src, 4u * it.count);  // the patched ucode, big-endian as written
              w += it.count;
              break;
            case kRkConstLoad:
              *w++ = recTag(kRkConstLoad, 0u, pred);
              *w++ = it.body[0];
              *w++ = it.body[1];
              *w++ = it.body[2];
              break;
            case kRkSetConst:  // body[0] = offsetType, body[1..bodyN) = values (big-endian in the record)
              *w++ = recTag(kRkSetConst, it.bodyN - 1u, pred);
              *w++ = it.body[0];
              for (uint32_t k = 1; k < it.bodyN; k++) *w++ = _byteswap_ulong(it.body[k]);
              break;
            case kRkBinReg:  // reg = which (0 mask lo, 1 mask hi, 2 select lo, 3 select hi), body[0] = value
              *w++ = recTag(kRkBinReg, it.reg, pred);
              *w++ = it.body[0];
              break;
            case kRkEvent:
              *w++ = recTag(kRkEvent, 0u, pred);
              *w++ = it.body[0];
              break;
            case kRkZpd:  // p2.zpd's bake: EVENT_WRITE_ZPD {initiator}
              *w++ = recTag(kRkZpd, 0u, pred);
              *w++ = it.body[0];
              break;
            case kRkFence:  // P2-B: {fb4|2, progress, fb0|2, S}
              *w++ = recTag(kRkFence, 0u, 0u);
              *w++ = it.body[0];
              *w++ = it.body[1];
              *w++ = it.body[2];
              *w++ = it.body[3];
              break;
            case kRkInvalidate:  // P2-B: {base, size, whole}
              *w++ = recTag(kRkInvalidate, 0u, 0u);
              *w++ = it.body[0];
              *w++ = it.body[1];
              *w++ = it.body[2];
              break;
            case kRkRegRmw:    // boot.device_create: {rmwInfo, andMask, orMask}
            case kRkEventShd:  // boot.device_create: {initiator, addr|endian, value}
              *w++ = recTag(it.rk, 0u, pred);
              *w++ = it.body[0];
              *w++ = it.body[1];
              *w++ = it.body[2];
              break;
            default:
              if (recOpaque(it.rk)) {  // strand B's snapshot, host order, n = its dwords
                *w++ = recTag(it.rk, it.count, pred);
                std::memcpy(w, it.src, 4u * it.count);
                w += it.count;
              }
              break;
          }
          continue;
        }
        if (!it.count) continue;
        *w++ = (it.chk ? 0x40000000u : 0u) | (it.count << 16) | it.reg;
        if (it.src) {
          std::memcpy(w, it.src, 4u * it.count);
        } else {
          for (uint32_t k = 0; k < it.count; k++) w[k] = _byteswap_ulong(it.lit[k]);
        }
        w += it.count;
      }
      const uint32_t bytes = uint32_t(reinterpret_cast<uint8_t*>(w) - r);
      if (bytes != bound) {  // the two passes disagree: never (boundOver = the dangerous direction)
        g_recBoundBad.fetch_add(1, std::memory_order_relaxed);
        if (bytes > bound) g_recBoundOver.fetch_add(1, std::memory_order_relaxed);
      }
      RecHdr h{kRecMagic, bytes, pos, 0u, 0u};
      std::memcpy(r, &h, sizeof(h));
      g_head = pos + bytes;
      g_recBytes.fetch_add(bytes, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      if (segNow) {
        // P2-B: no marker; the close finds the record through the segment's footprints.
        std::lock_guard<std::mutex> lk(g_segMu);
        SegFoot foot{spanStart + 4u, c + 4u, pos, captureSegmentRecord(r, bytes, pos)};
        if (detachedFoot) *detachedFoot = std::move(foot);
        else g_segFeet.push_back(std::move(foot));
      } else {
#if SR_ORACLE
        // SR_NATIVE_SEG_TILE: a marker-path record is LEGITIMATELY outside every segment - segmentless was inactive
        // for this span (gate / A/B window off, the S1 record mode); the CP reaches it through the NOP below.
        if (g_segTile) {
          std::lock_guard<std::mutex> lk(g_tileMu);
          g_tile.unsegmented(pos, bytes);
        }
        // The marker: a type-3 NOP covering the whole span (count field = dwords - 2).
        uint8_t m[16];
        put32(m, 0xC0000000u | (((len / 4u) - 2u) << 16) | (0x10u << 8));
        put32(m + 4, kMarkerMagic);
        put32(m + 8, uint32_t(pos));
        put32(m + 12, uint32_t(pos >> 32));
        writeGuest(spanStart + 4u, m, 16);
#else
        nativeAdmissionFatal("record lost its native segment");
#endif
      }
      g_spansRec.fetch_add(1, std::memory_order_relaxed);
    }
    nItems = 0;
    spanSkip = false;
    opaque = false;
    forceLiteral = false;
    spanStart = c;
    put32(d + 40, c);
  }
  bool spanSkip = false;  // the open span left constant groups out: it is always a record
  bool opaque = false;    // the open span holds an opaque record (strand B): it is always a record
  bool forceLiteral = false;  // the open span holds a caller-filled body (82113010): literal on the ring path
#if SR_ORACLE
  // The ring path's literal span by the meter's packet kinds (g_litKind; the CP's meter counts the same headers).
  void litCensus() {
    namespace m = sr::native::meter;
    for (uint32_t i = 0; i < nItems; i++) {
      const Item& it = items[i];
      if (it.pad) g_litKind[m::kT2Nop].fetch_add(it.pad, std::memory_order_relaxed);
      if (it.fixedBytes) {
        if (it.rk == kRkFence) {  // INVALIDATE_STATE + 2 EVENT_WRITE_SHD
          g_litKind[m::kOther].fetch_add(1, std::memory_order_relaxed);
          g_litKind[m::kSync].fetch_add(2, std::memory_order_relaxed);
        } else if (it.rk == kRkInvalidate) {  // 2 type-0 runs + WAIT_REG_MEM
          g_litKind[m::kT0].fetch_add(2, std::memory_order_relaxed);
          g_litKind[m::kSync].fetch_add(1, std::memory_order_relaxed);
        } else {
          g_litKind[m::kOther].fetch_add(1, std::memory_order_relaxed);  // an opaque record's PM4 (one packet counted)
        }
      } else if (it.pkt) {
        g_litKind[m::kindOf3((it.drawHdr >> 8) & 0xFFu, it.bodyN > 0 ? it.body[0] : 0u, it.bodyN > 1 ? it.body[1] : 0u)]
            .fetch_add(1, std::memory_order_relaxed);
      } else if (it.count) {
        g_litKind[m::kT0].fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  // closeSpan's literal path: the items' PM4 bytes disagree with the span's (never; the first 8, then powers of 2).
  void litLenBad(uint32_t total, uint32_t len) {
    const uint64_t n = g_litLenBad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 8 && (n & (n - 1)) != 0) return;
    char w[640];
    int at = 0;
    for (uint32_t i = 0; i < nItems && at < int(sizeof(w)) - 64; i++) {
      const Item& it = items[i];
      at += std::snprintf(w + at, sizeof(w) - size_t(at), " %u:%s rk=%u reg=%04X n=%u pad=%u%s fixed=%u bytes=%u", i,
                          it.pkt ? "pkt" : "run", unsigned(it.rk), it.reg, it.count, unsigned(it.pad), it.chk ? " chk" : "",
                          it.fixedBytes, it.bytes());
    }
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_LIT] #%llu span va=%08X len=%u items' bytes=%u (%u items):%s",
                (unsigned long long)n, spanStart + 4u, len, total, nItems, w);
  }

#endif

  // Before a guest call: the open span is written and dev+40 holds the cursor. After: the
  // guest's cursor. Dry: stop here (everything so far is the comparable prefix).
  bool beforeGuest() {
    if (!g_on && !g_check) return false;  // the gates (SR_NATIVE_FLUSH / _CHECK) at every write site
    if (dry) {
      stopped = true;
      return false;
    }
    closeSpan();
    if (dd) sr::native::cdedup::noteEmitted(c);  // the guest's packets follow: the adapter scans them
    g_guestCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_timing) guestT0 = __rdtsc();
    return true;
  }
  uint64_t guestT0 = 0;
  void afterGuest() {
    if (g_timing) guestTsc += __rdtsc() - guestT0;
    c = be32(d + 40);
    spanStart = c;
    ddSync();
  }

  // The kick threshold the native path tests: dev+48, or under SR_NATIVE_SEG_STRESS the segment start
  // (dev+13508) + N bytes when that comes first.
  uint32_t thrEff() const {
    const uint32_t t = thr();
    if (!g_segStress) return t;
    const uint32_t s = be32(d + 13508) + g_segStress;
    return s < t ? s : t;
  }
  // GpuCommandBufferFlush_sub_820FD8C0: the segment switch (the XDK's kick), unconditionally.
  bool kick() {
    if (!g_on && !g_check) return false;  // the gates (SR_NATIVE_FLUSH / _CHECK) at every write site
    if (dry) {
      stopped = true;
      return false;
    }
    closeSpan();
    const uint32_t seg0 = be32(d + 13508);
    const uint64_t k0 = g_timing ? __rdtsc() : 0;
    ctx->r3.u64 = dev;
    sub_820FD8C0(*ctx, base);
    if (g_timing) guestTsc += __rdtsc() - k0;
    g_kicks.fetch_add(1, std::memory_order_relaxed);
    c = ctx->r3.u32;  // the new segment's cursor (also in dev+40)
    spanStart = c;
    ddSync();  // a new segment: the epoch drops every group
    epochCheck(seg0);
    return true;
  }
  bool kickPoint() {  // the XDK's "cursor > dev+48 -> GpuCommandBufferFlush"
    if (!g_on && !g_check) return false;  // the gates (SR_NATIVE_FLUSH / _CHECK) at every write site
    if (c <= thrEff()) return true;
    return kick();
  }
  // After a segment switch: the dedup mirror must hold nothing once dev+13508 moved (the epoch rule
  // the dedup CHECK proves at execution; counted here at the producer as well).
  void epochCheck(uint32_t seg0) {
    if (dd && be32(d + 13508) != seg0 && sr::native::cdedup::validGroups()) g_epochStale.fetch_add(1, std::memory_order_relaxed);
  }
  // D3DDeviceReserveCommandSpace_sub_820FC910(dev, n): n dwords must fit before the segment LIMIT
  // (dev+44); otherwise the XDK flushes the segment (820FD8C0) and, when they still do not fit, begins
  // a new segment sized for them (820FCF90). 1 = reserved; 0 = dry stopped; -1 = the XDK's failure
  // (nothing is emitted then). STRESS: the switch comes early, as the kick does.
  int reserveDw(uint32_t n) {
    if (g_segStress && c + 4u * n > thrEff() && !kick()) return 0;
    if (c + 4u * n <= lim()) return 1;
    if (!beforeGuest()) return 0;
    const uint32_t seg0 = be32(d + 13508);
    ctx->r3.u64 = dev;
    ctx->r4.u64 = n;
    sub_820FC910(*ctx, base);
    const uint32_t r = ctx->r3.u32;
    afterGuest();
    epochCheck(seg0);
    g_reserveFlushes.fetch_add(1, std::memory_order_relaxed);
    return r ? 1 : -1;
  }

  // Room for a section's items at the end of the buffer (record: closes the span when full; dry:
  // nullptr = too long to keep, the comparable prefix ends here).
  Item* reserve(uint32_t n) {
    if (dry) {
      if (stopped || nAll + n > kAllCap) {
        stopped = true;
        return nullptr;
      }
      return all + nAll;
    }
    if (nItems + n > kSpanCap) closeSpan();
    return items + nItems;
  }
  void commit(uint32_t n, uint32_t bytes) {
    if (dry) nAll += n;
    else nItems += n;
    c += bytes;
  }
  static void run(Item& it, uint32_t pad, uint32_t reg, uint32_t count, const uint8_t* src) {
    it.pad = uint16_t(pad);
    it.pkt = 0;
    it.rk = 0;
    it.reg = reg;
    it.count = count;
    it.src = src;
    it.chk = false;
    it.fixedBytes = 0;
    it.deferSrc = 0;
  }
  // A type-3 packet item: hdr (predicate bit included), up to 4 literal body dwords, then `ucodeDw`
  // dwords of big-endian bytes at `ucode` (the immediate load). rk = its record kind.
  static void packet(Item& it, uint8_t rk, uint32_t hdr, std::initializer_list<uint32_t> body,
                     const uint8_t* ucode = nullptr, uint32_t ucodeDw = 0) {
    it.pad = 0;
    it.pkt = 1;
    it.rk = rk;
    it.reg = 0;
    it.count = ucodeDw;
    it.src = ucode;
    it.chk = false;
    it.fixedBytes = 0;
    it.deferSrc = 0;
    it.drawHdr = hdr;
    it.bodyN = 0;
    for (uint32_t v : body) it.body[it.bodyN++] = v;
  }

  // GpuEmitType0RegPacketsVec_sub_821078C0(dev, mask, baseReg, dev+srcOff): 16 registers per bit.
  // Planned first: every run must take the fast path, else the whole call is the guest leaf's.
  //
  // iter-467 FLICKER ROOT CAUSE (2026-10-08): the dedup mirror (native_const_dedup core) was updated
  // with cd::store() WHILE PLANNING each run. When a later run of the same section failed the
  // segment-limit check, the whole section went to the guest leaf and the planned items were dropped
  // - but the mirror already said their groups were resident, so the dedup leaf (and the next draws,
  // until the value changed or the next kick reset the epoch) skipped groups nobody had emitted: the
  // CP drew with the previous values of those constants (an object off-screen or unlit, a black sky,
  // the whole scene's lighting off for one present). Whether a section aborts depends on the cursor's
  // distance to the segment limit, which differs between frozen passes - hence the one-present
  // flicker. The leaf stores only in its write pass after the fit check, and invalidates on abort,
  // which is why the PM4 path never showed it. Now: the mirror is updated at COMMIT, once the
  // section is final (SR_NATIVE_CONST_DEDUP_STORE_EARLY=1 restores the old order for the A/B).
  static bool storeEarly() {
    static const bool v = envOn("SR_NATIVE_CONST_DEDUP_STORE_EARLY");
    return v;
  }
  bool constRuns(uint64_t mask, uint32_t reg, uint32_t srcOff) {
    Item* out = reserve(65);
    if (!out) return false;
    uint32_t L = lim();
    uint32_t cc = c, r = reg, s2 = srcOff, n = 0;
    uint64_t m = mask;
    uint32_t skippedHere = 0, seenHere = 0, predHere = 0;
    bool spanSkipHere = false;
    const bool early = storeEarly();
    // Commit: the items planned so far are final - now the emitted groups are what the stream holds (rule 2).
    auto commitPlanned = [&]() {
      if (dd && !early) {
        namespace cd = sr::native::cdedup;
        for (uint32_t i = 0; i < n; i++) {
          const Item& it = out[i];
          const uint32_t g0 = (it.reg - cd::kAluReg) / cd::kGroupRegs;
          for (uint32_t q = 0; q < it.count / cd::kGroupRegs; q++) cd::store(g0 + q, it.src + 64u * q);
        }
      }
      ddSeen += seenHere;
      ddPred += predHere;
      ddSkipped += skippedHere;
      if (spanSkipHere) spanSkip = true;
      commit(n, cc - c);
      n = seenHere = predHere = skippedHere = 0;
      spanSkipHere = false;
    };
    bool chunked = false;
    while (m) {
      const uint32_t lz = uint32_t(__builtin_clzll(m));
      s2 += lz * 64u;
      r += lz * 16u;
      m <<= lz;
      const uint32_t rn = (~m) ? uint32_t(__builtin_clzll(~m)) : 64u;
      const uint32_t regs = rn * 16u;
      if (!(cc + 4u * (regs + 3u) < L)) {
        if (!split || zpassLeaf(0)) {
          // The whole section is the guest leaf's: nothing planned above is committed, and the mirror
          // was not touched (see the root cause above), so the leaf decides from the stream's truth.
          g_sectionAborts.fetch_add(1, std::memory_order_relaxed);
          if (n) g_sectionAbortsPlanned.fetch_add(1, std::memory_order_relaxed);
          return guestLeaf(0, mask, reg, srcOff);
        }
        // p1.flush_residue: what 821078C0 does with this run - 82107058's chunks across the kick (chunkRun). The runs
        // before it took the fast path, as in the leaf: committed first (the mirror stores at commit), then the
        // chunks read the exact source address (lwzu), not the lvx-aligned group.
        commitPlanned();
        if (!chunked) g_splitSec[0].fetch_add(1, std::memory_order_relaxed);
        chunked = true;
        if (!chunkRun(0, r, d + s2, regs, 16u)) return false;
        out = reserve(65);
        if (!out) return false;
        L = lim();
        cc = c;
        s2 += 64u * rn;
        r += regs;
        m = rn >= 64u ? 0ull : (m << rn);
        continue;
      }
      const uint32_t a = cc & 0xFu;
      const uint32_t pad = a == 4u ? 1u : a == 0u ? 2u : a == 12u ? 3u : 0u;
      const uint8_t* src = d + (((dev + s2) & ~15u) - dev);  // lvx: the 16-byte aligned group
      if (!dd) {
        run(out[n++], pad, r, regs, src);
      } else {
        namespace cd = sr::native::cdedup;
        const uint32_t g0 = (r - cd::kAluReg) / cd::kGroupRegs;
        bool pr[64];
        for (uint32_t k = 0; k < rn; k++) {
          pr[k] = cd::resident(g0 + k, src + 64u * k);
          predHere += pr[k] ? 1u : 0u;
        }
        seenHere += rn;
        // CHECK splits a run where the prediction changes (each part its own record run, the flag per run) -
        // never under the PM4 output (SR_NATIVE_FLUSH_PM4): a literal span is the XDK's bytes, ONE header per
        // run, and the flag cannot travel in PM4. A split there rendered a header per part while the cursor
        // (cc below) counted one: the span's last 4 bytes per split were cut off, its last run overran into
        // the next packet and the CP parsed values as headers (segj's CPPM4 kick-block stall, 2026-10-10).
        const bool chkSplit = ddChk && !g_pm4Out;
        for (uint32_t k = 0; k < rn;) {
          if (ddSkip && pr[k]) {
            skippedHere++;
            spanSkipHere = true;
            k++;
            continue;
          }
          uint32_t j = k + 1;
          while (j < rn && !(ddSkip && pr[j]) && (!chkSplit || pr[j] == pr[k])) j++;
          Item& it = out[n++];
          run(it, k ? 0u : pad, r + 16u * k, 16u * (j - k), src + 64u * k);
          if (chkSplit && pr[k]) {
            it.chk = true;
            cd::notePredictedRun(j - k);
          }
          if (early)
            for (uint32_t q = k; q < j; q++) cd::store(g0 + q, src + 64u * q);
          k = j;
        }
      }
      cc += 4u * (pad + 1u + regs);
      s2 += 64u * rn;
      r += regs;
      m = rn >= 64u ? 0ull : (m << rn);
    }
    commitPlanned();  // the section is final
    return true;
  }

  // p1.flush_residue: GpuCmdEmitRegisterRunChunked_sub_82107058(dev, cursor, reg, src - 4, count, elem) at a run that
  // does not fit before the segment limit (asm, IDA): the registers that fit - n = ((((limit - cursor) >> 2) - 1) /
  // elem) * elem, signed (srawi / divw), capped at the run (cmplw) - as a header and n values read word by word from the
  // exact source (lwzu), with NO alignment pad; then, while registers remain, the cursor into dev+0x28 and
  // GpuCommandBufferFlush 820FD8C0 UNCONDITIONALLY (kick(): the open span is written first), the rest from the new
  // segment's cursor. It never rewinds. The caller has committed every planned item. leaf 0 (821078C0's constants,
  // elem 16: whole 64-byte groups) stores each chunk's groups in the dedup mirror at its commit - no skip, no CHECK
  // flag; the kick's new epoch drops the groups of the chunks before it. false = the dry run stopped at the kick.
  bool chunkRun(int leaf, uint32_t reg, const uint8_t* src, uint32_t count, uint32_t elem) {
    for (;;) {
      const int32_t avail = (int32_t(lim() - c) >> 2) - 1;
      const int32_t q = avail / int32_t(elem);
      if (q > 0) {
        uint32_t k = uint32_t(q) * elem;
        if (!(k < count)) k = count;
        Item* it = reserve(1);
        if (!it) return false;
        run(*it, 0, reg, k, src);
        if (leaf == 0 && dd) {
          namespace cd = sr::native::cdedup;
          const uint32_t g0 = (reg - cd::kAluReg) / cd::kGroupRegs;
          for (uint32_t g = 0; g < k / cd::kGroupRegs; g++) cd::store(g0 + g, src + 64u * g);
          ddSeen += k / cd::kGroupRegs;
        }
        commit(1, 4u * (1u + k));
        g_splitChunks[leaf].fetch_add(1, std::memory_order_relaxed);
        count -= k;
        reg += k;
        src += 4u * k;
      }
      if (!count) return true;
      if (!kick()) return false;
      g_splitKicks[leaf].fetch_add(1, std::memory_order_relaxed);
    }
  }

  // GpuCmdFlushDirtyRegisterRuns_sub_82107520(dev, mask, baseReg, dev+srcOff): 1 register per bit.
  bool regRuns(uint64_t mask, uint32_t reg, uint32_t srcOff) {
    Item* out = reserve(65);
    if (!out) return false;
    uint32_t L = lim();
    uint32_t cc = c, r = reg, s2 = srcOff, n = 0;
    uint64_t m = mask;
    bool chunked = false;
    while (m) {
      const uint32_t lz = uint32_t(__builtin_clzll(m));
      r += lz;
      s2 += 4u * lz;
      m <<= lz;
      const uint32_t rn = (~m) ? uint32_t(__builtin_clzll(~m)) : 64u;
      if (!(cc + 4u * rn < L)) {
        if (!split || zpassLeaf(1)) return guestLeaf(1, mask, reg, srcOff);
        commit(n, cc - c);  // p1.flush_residue: the runs before took the fast path; this one 82107058's chunks
        n = 0;
        if (!chunked) g_splitSec[1].fetch_add(1, std::memory_order_relaxed);
        chunked = true;
        if (!chunkRun(1, r, d + s2, rn, 1u)) return false;
        out = reserve(65);
        if (!out) return false;
        L = lim();
        cc = c;
        r += rn;
        s2 += 4u * rn;
        m = rn >= 64u ? 0ull : (m << rn);
        continue;
      }
      run(out[n++], 0, r, rn, d + s2);
      cc += 4u * (1u + rn);
      r += rn;
      s2 += 4u * rn;
      m = rn >= 64u ? 0ull : (m << rn);
    }
    commit(n, cc - c);
    return true;
  }

  // GpuEmitType0RegPackets_sub_82107760(dev, mask): 6 dwords (one fetch slot) per bit from
  // dev+0x400 at register 0x4800, a type-2 pad so each header lands at 4 mod 8, and the
  // SHADER_CONSTANT_FLUSH_FETCH trailer {0x00025000, 0, 0x00025000, 0} after a fast-path end.
  // p1.flush_residue: a run that does not fit goes through 82107058 (6-dword elements); when the LAST run went that way
  // the asm (821077FC) stores the cursor, makes the kick check (cursor > dev+0x30 -> 820FD8C0) and writes the trailer
  // {0x00025000, 0, 0, 0} instead.
  bool fetchRuns(uint64_t mask) {
    Item* out = reserve(34);
    if (!out) return false;
    uint32_t L = lim();
    uint32_t cc = c, r = 0x4800u, s2 = 0x400u, n = 0;
    uint64_t m = mask;
    bool chunked = false, lastChunked = false;
    while (m) {
      const uint32_t lz = uint32_t(__builtin_clzll(m));
      m <<= lz;
      r += 6u * lz;
      s2 += 24u * lz;
      const uint32_t rn = (~m) ? uint32_t(__builtin_clzll(~m)) : 64u;
      if (!(cc + 4u * (6u * rn + 5u) < L) || n >= 32) {
        if (!split || n >= 32 || zpassLeaf(2)) return guestLeaf(2, mask, 0, 0);
        commit(n, cc - c);
        n = 0;
        if (!chunked) g_splitSec[2].fetch_add(1, std::memory_order_relaxed);
        chunked = lastChunked = true;
        if (!chunkRun(2, r, d + s2, 6u * rn, 6u)) return false;
        out = reserve(34);
        if (!out) return false;
        L = lim();
        cc = c;
        r += 6u * rn;
        s2 += 24u * rn;
        m = rn >= 64u ? 0ull : (m << rn);
        continue;
      }
      lastChunked = false;
      const uint32_t pad = (cc & 7u) ? 1u : 0u;  // header at cc+4 when cc%8==0, else cc+8
      run(out[n++], pad, r, 6u * rn, d + s2);
      cc += 4u * (pad + 1u + 6u * rn);
      r += 6u * rn;
      s2 += 24u * rn;
      m = rn >= 64u ? 0ull : (m << rn);
    }
    if (lastChunked) {  // 821077FC: nothing was planned after the chunks (n == 0); the kick check, the slow trailer
      if (!kickPoint()) return false;
      out = reserve(1);
      if (!out) return false;
      run(out[0], 0, 0x5000u, 3u, nullptr);
      out[0].lit[0] = out[0].lit[1] = out[0].lit[2] = 0u;
      commit(1, 16u);
      g_splitSlowTrailer.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    Item& tr = out[n++];
    run(tr, 0, 0x5000u, 3u, nullptr);
    tr.lit[0] = 0u;
    tr.lit[1] = 0x00025000u;
    tr.lit[2] = 0u;
    cc += 16u;
    commit(n, cc - c);
    return true;
  }

  // A section the native fast path does not take: the guest leaf writes it (PM4, its own
  // chunking / segment switch). leaf 0 = 821078C0, 1 = 82107520, 2 = 82107760.
  bool guestLeaf(int leaf, uint64_t mask, uint32_t reg, uint32_t srcOff) {
    if (!g_on && !g_check) return false;  // the gates (SR_NATIVE_FLUSH / _CHECK) at every write site
    if (!beforeGuest()) return false;
    g_fallbackSec.fetch_add(1, std::memory_order_relaxed);
    ctx->r3.u64 = dev;
    ctx->r4.u64 = mask;
    ctx->r5.u64 = reg;
    ctx->r6.u64 = dev + srcOff;
    if (leaf == 0) sub_821078C0(*ctx, base);
    else if (leaf == 1) sub_82107520(*ctx, base);
    else sub_82107760(*ctx, base);
    afterGuest();
    return true;
  }

  // ---- P1 rows 1+2: the shader bind GpuEmitDrawState_sub_82108668(dev, d2) as host code ----
  // One entry (NATIVE_ROADMAP rule 1): nothing below falls back to the guest part-way. The guest
  // functions it still calls are computations on guest objects or the XDK's own ring management:
  // 82108520 (the in-place VS fetch patch + its per-variant cache), 82107A30 / 821081C0 (fetch
  // patching of the ring copy), 820FC910 (the reserve: a segment flush) - none of them an emitter.
  // Every item carries the XDK's exact byte length; the cursor and the kicks advance as the XDK's.
  // Rule 2: a LOAD_ALU_CONSTANT invalidates its ALU groups in the dedup mirror at commit (the
  // adapter's scan did it for the guest's packets; records are invisible to the scan).
  // Dry (the CHECK): the pointer bind runs to the end with a side-effect-free port of 82108520's
  // decision; the immediate VS load stops the prefix (its guest helpers patch the ring copy).
  // Masks from the asm (the decompiler shows them as 32-bit RORs): bit 49 surface state, 52 PS
  // fetch constants + PS IM_LOAD (then |= 1<<46), 51 VS, |= (1<<51)|8 on a dev+0x28D4 mode change,
  // 3<<51 the VS reload test, 3<<47 the 0x2180 run (tested at bit 48), bit 3 the 0x2208 write.
  const uint8_t* G(uint32_t va) const { return base + sr::kernel::aliasGuestVA(va); }
  uint32_t rd(uint32_t va) const { return be32(G(va)); }
  static uint32_t phys(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }
  static constexpr uint32_t kNone = 0xFFFFFFFFu;  // the XDK's -1 sentinels (compared as the XDK compares them)
  uint32_t scratchVA = 0, savedR1 = 0;
  // A guest stack frame for the bind's by-reference local (the XDK keeps it at +0x50 of its own
  // 0x100-byte frame; the helpers' frames go below r1).
  uint32_t scratch() {
    if (!scratchVA) {
      savedR1 = ctx->r1.u32;
      ctx->r1.u32 = (savedR1 - 0x100u) & ~0xFu;
      put32(base + sr::kernel::aliasGuestVA(ctx->r1.u32), savedR1);  // the back chain, as stwu writes it
      scratchVA = ctx->r1.u32 + 0x50u;
    }
    return scratchVA;
  }
  void releaseScratch() {
    if (scratchVA) {
      ctx->r1.u32 = savedR1;
      scratchVA = 0;
    }
  }
  void guestTimed(void (*fn)(PPCContext&, uint8_t*)) {
    const uint64_t k0 = g_timing ? __rdtsc() : 0;
    fn(*ctx, base);
    if (g_timing) guestTsc += __rdtsc() - k0;
    g_bindHelpers.fetch_add(1, std::memory_order_relaxed);
  }
  // SET_BIN_MASK_LO / _HI (which 0 / 1) and SET_BIN_SELECT_LO / _HI (2 / 3): one body dword.
  static void binReg(Item& it, uint32_t which, uint32_t value) {
    packet(it, kRkBinReg, 0xC0006000u | (which << 8), {value});
    it.reg = which;
  }
  // P1 row 3: GpuCmdEmitWindowScissorState_sub_821072F8(dev, d2, a3 = dev+10432, a4 = 0) as host items,
  // from its asm. The XDK kick check at entry; then, under predicated tiling (byte 10811 & 0x20 and:
  // byte 10808 & 0x10, or & 0x20 with the five surfaces dev+12168.. equal to their dev+12452.. shadows
  // or zero), SET_BIN_MASK_HI {0}, per tile i < dev+12472: SET_BIN_MASK_LO {3 << 2i} + the predicated
  // SET_CONSTANT {0x40203, ((dev+12836+4i) << 17 | (a3 & 0x1FFFF)) & ~1} with the kick check after
  // each tile, the Z-pass tile (byte 10808 & 0x40 and a3 bit 0: LO {1} + SET_CONSTANT of tile 0,
  // unmasked), then LO {dev+12432} / HI {dev+12436}. Otherwise one 8-byte run {0x2203, a3}. The asm
  // returns r4 & ~0x100 with its upper bits (li r12,-0x101; and r3,r20,r12), as d2 here.
  bool windowScissorState(uint64_t& d2) {
    if (!kickPoint()) return false;
    const uint32_t a3 = be32(d + 10432);
    const bool tiled = (d[10811] & 0x20u) && tiledSurfaces();
    {
      const uint64_t n = g_wscCalls.fetch_add(1, std::memory_order_relaxed) + 1;
      if (tiled) g_wscTiled.fetch_add(1, std::memory_order_relaxed);
      if (g_leafTrace && (n <= 400 || n % 2000 == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[LEAF] 821072F8 #%llu tiled=%d b08=%02X b0B=%02X tiles=%u a3=%08X m12432=%08X m12436=%08X c=%08X dry=%d",
                    (unsigned long long)n, int(tiled), d[10808], d[10811], be32(d + 12472), a3, be32(d + 12432),
                    be32(d + 12436), c, int(dry));
    }
    if (!tiled) {
      Item* o = reserve(1);
      if (!o) return false;
      run(o[0], 0, 0x2203u, 1u, nullptr);
      o[0].lit[0] = a3;
      commit(1, 8u);
      d2 &= ~0x100ull;
      return true;
    }
    {
      Item* o = reserve(1);
      if (!o) return false;
      binReg(o[0], 1u, 0u);
      commit(1, 8u);
    }
    const uint32_t tiles = be32(d + 12472);
    for (uint32_t i = 0; i < tiles; i++) {
      Item* o = reserve(2);
      if (!o) return false;
      const uint32_t v = ((be32(d + 12836 + 4u * i) << 17) | (a3 & 0x1FFFFu)) & 0xFFFFFFFEu;
      binReg(o[0], 0u, 3u << (2u * i));
      packet(o[1], kRkSetConst, 0xC0012D01u, {0x00040203u, v});
      commit(2, 20u);
      if (!kickPoint()) return false;
    }
    Item* o = reserve(4);
    if (!o) return false;
    uint32_t n = 0, bytes = 0;
    auto add = [&] { bytes += o[n].bytes(); n++; };
    if ((d[10808] & 0x40u) && (a3 & 1u)) {
      binReg(o[n], 0u, 1u);
      add();
      packet(o[n], kRkSetConst, 0xC0012D01u, {0x00040203u, (be32(d + 12836) << 17) | (a3 & 0x1FFFFu)});
      add();
    }
    binReg(o[n], 0u, be32(d + 12432));
    add();
    binReg(o[n], 1u, be32(d + 12436));
    add();
    commit(n, bytes);
    d2 &= ~0x100ull;
    return true;
  }
  // P1 row 5: D3DDeviceEmitScissorTileRects_820F0208(dev, x0, y0, x1, y1) as items, from its asm (the
  // Entry leaf and the dry CHECK kind 5 share it). The entry kick check (r3 = the flush's cursor when it
  // fires, dev otherwise); tiled (byte 10808 & 0x10, or & 0x20 with the five surfaces dev+12168.. equal
  // to their shadows dev+12452.. or zero): SET_BIN_MASK_HI {0}; per tile i < dev+12472 (rect
  // dev+12476+16i {l, t, r, b}, offset dev+12716+8i {x, y}; signed compares): x0' = max(x0, l), y0' =
  // max(y0, t), x1' = min(x1, r), y1' = min(y1, b), all zero when x0' >= x1' or y0' >= y1';
  // SET_BIN_MASK_LO {3 << 2i} + the predicated SET_CONSTANT {0x40080, (-y & 0x7FFF) << 16 | (-x &
  // 0x7FFF), TL, BR}, r3 = 3, the kick check; then (byte 10811 & 0x20 and byte 10808 & 0x40)
  // SET_BIN_MASK_LO {1} + SET_CONSTANT {0x40081, TL, BR} of the unclamped rect; LO {dev+12432} / HI
  // {dev+12436}. Untiled: the run {0x2080: 0, TL, BR}. TL / BR = (y & 0x3FFF) << 16 | (x & 0x3FFF).
  static uint32_t tlbr(int32_t x, int32_t y) { return ((uint32_t(y) << 16) & 0x3FFF0000u) | (uint32_t(x) & 0x3FFFu); }
  // The XDK's "tiled" test shared by 820F0208 / 821072F8 / 8210D400: byte 10808 & 0x10, or & 0x20 with
  // the five surfaces dev+12168.. each equal to its shadow dev+12452.. or zero.
  bool tiledSurfaces() const {
    if (d[10808] & 0x10u) return true;
    if (!(d[10808] & 0x20u)) return false;
    for (uint32_t k = 0; k < 5u; k++) {
      const uint32_t s = be32(d + 12168 + 4u * k);
      if (s && be32(d + 12452 + 4u * k) != s) return false;
    }
    return true;
  }
  // P1 row 3b: D3DDeviceSetPredication_8210D400(dev, mask) from its asm. mask != 0: dev+12432 = mask,
  // dev+12440 = 1, byte 10808 &= ~2. mask == 0: dev+12440 = 0, byte 10808 bit 1 = tiled && !(byte &
  // 8) && (byte 10811 & 0x20 || (byte 11849 == 0 && byte 11915 == 0)), dev+12432 = -1. Then dev+12436 =
  // 0, dev+10800 = mask, dev+10804 = 0, the kick check, SET_BIN_MASK_LO {dev+12432}; r3 = the cursor.
  bool setPredication(uint32_t mask, uint32_t& r3) {
    if (mask) {
      put32(d + 12432, mask);
      put32(d + 12440, 1u);
      d[10808] &= 0xFDu;
    } else {
      const uint8_t b = d[10808];
      put32(d + 12440, 0u);
      const bool v = tiledSurfaces() && !(b & 0x08u) && ((d[10811] & 0x20u) || (!d[11849] && !d[11915]));
      put32(d + 12432, 0xFFFFFFFFu);
      d[10808] = uint8_t((b & 0xFDu) | (v ? 0x02u : 0u));
    }
    put32(d + 12436, 0u);
    put32(d + 10800, mask);
    put32(d + 10804, 0u);
    if (!kickPoint()) return false;
    Item* o = reserve(1);
    if (!o) return false;
    binReg(o[0], 0u, be32(d + 12432));
    commit(1, 8u);
    r3 = c;
    return true;
  }
  // p2.zpd's bake (native/zpd-bake, 2026-10-16): GpuEmitTileSurfaceConfig_82101E08(dev, query, which), the occlusion
  // query's packets that D3DQueryIssue 82102BC8 emits for an END (which 0) or a BEGIN (1), from its asm (IDA,
  // 2026-10-16). The kick check (cursor > dev+48); RB_MODECONTROL {6} (0x2208) and RB_SURFACE_INFO / RB_COLOR_INFO
  // {0x20000, 0} (0x2000, two registers). One tile (query+144 <= 1): RB_SAMPLE_COUNT_ADDR (0x2325) {the slot's physical
  // address}. Tiles > 1: SET_BIN_MASK_HI {0}; per tile i: SET_BIN_MASK_LO {3 << 2i} + the predicated SET_CONSTANT
  // {0x40325, tile i's slot} with the kick check after each tile; then LO {dev+12432} / HI {dev+12436}. Then
  // EVENT_WRITE_ZPD {21} as the kRkZpd record. The slot = query+24+4i + 32 * which through the XDK's VA -> physical
  // conversion. The dirty bits (the 64-bit masks): dev+16 |= 8, dev+32 |= 0x2000, dev+24 |= 1 << 38 | 1 << 37 (rldicr
  // 1,38 / 1,37 at 82101F84 / 82101FB8). r3 = the asm's `result`: the cursor after the entry kick check + 16, or the
  // cursor an in-loop kick returned. The Entry leaf and the dry CHECK kind 11 share it.
  bool zpdSurface(uint32_t q, uint32_t which, uint32_t& r3) {
    if (!kickPoint()) return false;
    const uint32_t tiles = sr::kernel::readGuestU32BE(q + 144u);
    auto slot = [&](uint32_t i) {
      const uint32_t v = sr::kernel::readGuestU32BE(q + 24u + 4u * i) + 32u * which;
      return (((v >> 20) + 0x200u) & 0x1000u) + (v & 0x1FFFFFFFu);
    };
    r3 = c + 16u;
    {
      Item* o = reserve(tiles > 1u ? 3 : 4);
      if (!o) return false;
      uint32_t n = 0, bytes = 0;
      auto add = [&] { bytes += o[n].bytes(); n++; };
      run(o[n], 0, 0x2208u, 1u, nullptr);
      o[n].lit[0] = 6u;
      add();
      run(o[n], 0, 0x2000u, 2u, nullptr);
      o[n].lit[0] = 0x20000u;
      o[n].lit[1] = 0u;
      add();
      if (tiles > 1u) {
        binReg(o[n], 1u, 0u);
        add();
      } else {
        run(o[n], 0, 0x2325u, 1u, nullptr);
        o[n].lit[0] = slot(0);
        add();
        packet(o[n], kRkZpd, 0xC0005B00u, {21u});
        add();
      }
      commit(n, bytes);
    }
    if (tiles > 1u) {
      for (uint32_t i = 0; i < tiles; i++) {
        Item* o = reserve(2);
        if (!o) return false;
        const uint32_t sh = (2u * i) & 0x3Fu;  // slw: the shift's low 6 bits, 32..63 -> 0
        binReg(o[0], 0u, sh < 32u ? 3u << sh : 0u);
        packet(o[1], kRkSetConst, 0xC0012D01u, {0x00040325u, slot(i)});
        commit(2, 20u);
        if (c > thrEff()) {  // the asm: dev+40 = cursor; cursor > dev+48 -> r3 = GpuCommandBufferFlush(dev)
          if (!kick()) return false;
          r3 = c;
        }
      }
      Item* o = reserve(3);
      if (!o) return false;
      binReg(o[0], 0u, be32(d + 12432));
      binReg(o[1], 1u, be32(d + 12436));
      packet(o[2], kRkZpd, 0xC0005B00u, {21u});
      commit(3, 24u);
    }
    put64(d + 16, be64(d + 16) | 0x8ull);
    put64(d + 32, be64(d + 32) | 0x2000ull);
    put64(d + 24, be64(d + 24) | (1ull << 38) | (1ull << 37));
    return true;
  }
  // P2-B item 5: D3DDeviceEmitPm4Packet_82113010(dev, type, index, count), from the generated body
  // (guest/ppc, 2026-10-10): ReserveCommandSpace_820FC910(dev, 4*count + 5) - 0 = failure, r3 = 0 and
  // nothing emitted; three type-2 NOPs stored at c+4, c+8, c+12 unconditionally; pad = (-(c + 12)) & 15
  // bytes so the body is 16-byte aligned; at c+4+pad the header 0xC0002D00 | (4*count) << 16 | dev+12440
  // (the predication word SetPredication keeps at 0 / 1) and the offset word ((index - (type << 8)) << 2)
  // & 0x7FC; r3 = the body VA = c + 12 + pad, which the CALLER fills afterwards (Lion: 16 bytes per
  // call); dev+40 = body + 16*count - 4. The item: pad NOPs + {header, offset} + the body, as the opaque
  // record {header, offset, bodyVA} (kRkSetConstMem): the CP reads the values at apply, the renderer
  // writes the XDK bytes with the body copied from memory - unfilled on the ring path, where the span
  // is literal (a marker over the body would be overwritten by the fill). Record form only under
  // segmentless. false = the dry run stopped (a guest call was needed); bodyVA = 0 = the reserve failed.
  // p2.transport step 1 (kRkCallback): GpuEmitCpuCallbackPackets_820FDA80's block at the current c (its caller
  // reserved the space): payload {cb, arg, mask, flags, pcAddr} - the CPU mask from flags bits 24..29 (0 = 4, CPU 2),
  // pcAddr = the scratch write-back block dev+0x2A14 as the WAITs address it (phys | 2); 100 XDK bytes with the
  // WAIT_UNTIL 3D-idle prefix (flags & 0xC0FFFFFF == 0), else 92. Record form only under segmentless.
  bool presentGate() {
    uint32_t payload[3];
    if (!presentGatePayload(be32(d + 0x2A14u), be32(d + 0x33A4u), be32(d + 0x2D3Cu), payload)) return false;
    Item* o = reserve(1);
    if (!o) return false;
    Item& it = o[0];
    it.pad = 0;
    it.pkt = 1;
    it.rk = uint8_t(kRkPresentGate);
    it.reg = 0;
    it.count = 3;
    std::memcpy(it.lit, payload, sizeof(payload));
    it.src = reinterpret_cast<const uint8_t*>(it.lit);
    it.chk = false;
    it.fixedBytes = 156;
    it.deferSrc = 0;
    it.drawHdr = 0;  // the two outer waits are unconditional
    it.bodyN = 0;
    opaque = true;
    commit(1, it.bytes());
    return true;
  }

  // 821075E0 / 82107058, verified against the PPC instructions. A bit selects
  // four consecutive registers, MSB first. Only the fast path aligns its header
  // to 4 mod 8; the chunked path uses every available group of four registers.
  bool shaderConstantRuns(uint64_t mask, uint64_t& result) {
    if (!mask) return false;
    const bool switches = c > thrEff();  // the entry check below switches the segment (counted once it succeeds)
    if (!kickPoint()) return false;
    if (switches) g_scrKick.fetch_add(1, std::memory_order_relaxed);
    if (lim() - thr() != 160u) g_scrMarginBad.fetch_add(1, std::memory_order_relaxed);
    ensureShaderConstSyncHandlers();
    Item* preamble = reserve(1);
    if (!preamble) return false;
    *preamble = {};
    preamble->pkt = 1;
    preamble->rk = uint8_t(kRkShaderConstSync);
    preamble->count = 1;
    preamble->lit[0] = be32(d + 0x281Cu);
    preamble->src = reinterpret_cast<const uint8_t*>(preamble->lit);
    preamble->fixedBytes = 52u;
    opaque = true;
    commit(1, 52u);
    result = 3u;
    uint32_t reg = 0x2388u, source = 0x27A0u;
    while (mask) {
      const uint32_t leading = uint32_t(__builtin_clzll(mask));
      reg += 4u * leading;
      source += 16u * leading;
      mask <<= leading;
      const uint32_t groups = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64u;
      uint32_t remaining = 4u * groups;
      if (uint64_t(c) + 4u * remaining + 4u < lim()) {
        Item* item = reserve(1);
        if (!item) return false;
        run(*item, (c & 7u) ? 1u : 0u, reg, remaining, d + source);
        commit(1, item->bytes());
        source += 4u * remaining;
        reg += remaining;
      } else {
        g_shaderConstChunks.fetch_add(1, std::memory_order_relaxed);
        do {
          const int64_t available = (int64_t(lim()) - c) / 4 - 1;
          const uint32_t fits = available > 0 ? uint32_t(available / 4) * 4u : 0u;
          const uint32_t count = std::min(remaining, fits);
          if (count) {
            Item* item = reserve(1);
            if (!item) return false;
            run(*item, 0, reg, count, d + source);
            commit(1, item->bytes());
            remaining -= count;
            source += 4u * count;
            reg += count;
          }
          if (remaining && !kick()) return false;
        } while (remaining);
        result = c;  // the SDK leaves the last chunk helper's return in r3
      }
      mask = groups == 64u ? 0ull : mask << groups;
    }
    g_shaderConstRuns.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  bool callback(uint32_t cb, uint32_t arg, uint32_t flags) {
    const uint32_t m = (flags >> 24) & 0x3Fu;
    const uint32_t pc = be32(d + 0x2A14u);
    Item* o = reserve(1);
    if (!o) return false;
    Item& it = o[0];
    it.pad = 0;
    it.pkt = 1;
    it.rk = uint8_t(kRkCallback);
    it.reg = 0;
    it.count = 5u;  // the payload dwords
    it.lit[0] = cb;
    it.lit[1] = arg;
    it.lit[2] = m ? m : 4u;
    it.lit[3] = flags;
    it.lit[4] = ((((pc >> 20) + 0x200u) & 0x1000u) + (pc & 0x1FFFFFFFu)) | 2u;
    it.src = reinterpret_cast<const uint8_t*>(it.lit);  // in place: the item buffers are per-thread and never move
    it.chk = false;
    it.fixedBytes = (flags & 0xC0FFFFFFu) == 0 ? 100u : 92u;
    it.deferSrc = 0;
    it.drawHdr = 0;  // the block's predicated packets test the bins themselves (callbackApply)
    it.bodyN = 0;
    opaque = true;
    if (!segmentlessActive()) forceLiteral = true;
    commit(1, it.bytes());
    return true;
  }
  bool setConstMem(uint32_t type, uint32_t index, uint32_t count, uint32_t& bodyVA) {
    bodyVA = 0;
    const uint32_t nv = 4u * count;
    const int r = reserveDw(nv + 5u);
    if (r == 0) return false;
    if (r < 0) return true;  // the XDK returns 0 to the caller: nothing emitted
    const uint32_t c0 = c;
    const uint32_t padB = (0u - (c0 + 12u)) & 15u;
    const uint32_t hdr = 0xC0002D00u | ((nv << 16) & 0xFFFF0000u) | be32(d + 12440);
    const uint32_t off = ((index - (type << 8)) << 2) & 0x7FCu;
    const uint32_t body = c0 + 12u + padB;
    Item* o = reserve(1);
    if (!o) return false;
    Item& it = o[0];
    it.pad = uint16_t(padB / 4u);
    it.pkt = 1;
    it.rk = uint8_t(kRkSetConstMem);
    it.reg = 0;
    it.count = 3u;  // the payload dwords
    it.lit[0] = hdr;
    it.lit[1] = off;
    it.lit[2] = body;
    it.src = reinterpret_cast<const uint8_t*>(it.lit);  // in place: the item buffers are per-thread and never move
    it.chk = false;
    it.fixedBytes = 8u + 4u * nv;  // header, offset, the body
    it.deferSrc = 0;
    it.drawHdr = hdr & 1u;  // predicated: the record tag's bit (the CP's bin test at every apply)
    it.bodyN = 0;
    opaque = true;
    if (!segmentlessActive()) forceLiteral = true;
    commit(1, it.bytes());
    bodyVA = body;
    return true;
  }
  // boot.device_create: D3DDeviceResetState_8210E520's packet tail (values from the asm, guest/ppc 2026-10-10):
  // the kick check, SQ_FLOW_CNTL = 0x04000000, REG_RMW SCLK_PWRMGT_CNTL2 |= (dword 829AC0F8 | 0x80010000), REG_RMW
  // SCLK_PWRMGT_CNTL3 |= dword 829AC0FC, VC_FIFO_DEPTHS = 0x1F60, PA_CL_ENHANCE = 15 with behaviour-flag bit 1
  // (dev+20416) else 3, SCRATCH_REG4 = 0x0BADF00D (pc+0x10's first write-back), SCRATCH_REG3 = 0: 72 bytes.
  bool devResetPackets() {
    if (!kickPoint()) return false;
    const uint32_t g0 = sr::kernel::readGuestU32BE(0x829AC0F8u), g1 = sr::kernel::readGuestU32BE(0x829AC0FCu);
    const uint32_t enh = (be32(d + 20416) & 2u) ? 15u : 3u;
    Item* o = reserve(7);
    if (!o) return false;
    uint32_t n = 0, bytes = 0;
    auto reg1 = [&](uint32_t reg, uint32_t v) {
      run(o[n], 0, reg, 1u, nullptr);
      o[n].lit[0] = v;
      bytes += o[n].bytes();
      n++;
    };
    auto rmw = [&](uint32_t reg, uint32_t orMask) {
      packet(o[n], kRkRegRmw, 0xC0022100u, {reg, 0xFFFFFFFFu, orMask});
      bytes += o[n].bytes();
      n++;
    };
    reg1(0x0D01u, 0x04000000u);
    rmw(0x81u, g0 | 0x80010000u);
    rmw(0x82u, g1);
    reg1(0x0E42u, 0x1F60u);
    reg1(0x0C85u, enh);
    reg1(0x057Cu, 0x0BADF00Du);
    reg1(0x057Bu, 0u);
    commit(n, bytes);
    return true;
  }
  // boot.device_create: the segment re-emit 820F0048's packets with r4 == 0 (the boot call from 82106010 and
  // Swap's per-frame call): the kick check, SET_BIN_MASK_LO {dev+12432}, SET_BIN_SELECT_LO {~0}, SET_BIN_MASK_HI
  // {dev+12436}, SET_BIN_SELECT_HI {0}, the kick check, WAIT_UNTIL = 0x20000, RB_BC_CONTROL = dev+10788 (which the
  // asm re-stores with its own value): 32 + 16 bytes. The tile rects that follow are row 5's own entry.
  bool segmentBeginPackets() {
    if (!kickPoint()) return false;
    {
      Item* o = reserve(4);
      if (!o) return false;
      binReg(o[0], 0u, be32(d + 12432));
      binReg(o[1], 2u, 0xFFFFFFFFu);
      binReg(o[2], 1u, be32(d + 12436));
      binReg(o[3], 3u, 0u);
      commit(4, 32u);
    }
    if (!kickPoint()) return false;
    Item* o = reserve(2);
    if (!o) return false;
    run(o[0], 0, 0x05C8u, 1u, nullptr);
    o[0].lit[0] = 0x00020000u;
    run(o[1], 0, 0x0F01u, 1u, nullptr);
    o[1].lit[0] = be32(d + 10788);
    commit(2, 16u);
    return true;
  }
  // boot gaps (SR_NATIVE_MODE_STATE): GpuRingEmitModeStateReg_82103238(dev, mode)'s packets, after its caller-visible
  // device write (dev+10788 = 0x2A24 bits 12..13 = mode, stored by the leaf first, as the asm stores it before the kick
  // check): the kick check (`cmplw cursor, dev+0x30; bl 820FD8C0`), WAIT_UNTIL 0x5C8 = 0x20000, RB_BC_CONTROL 0xF01 =
  // dev+10788 re-read after the kick (`lwz r11, 0x2A24(r31)`): 16 bytes - the same two runs as 820F0048's tail.
  bool modeStatePackets() {
    if (!kickPoint()) return false;
    Item* o = reserve(2);
    if (!o) return false;
    run(o[0], 0, 0x05C8u, 1u, nullptr);
    o[0].lit[0] = 0x00020000u;
    run(o[1], 0, 0x0F01u, 1u, nullptr);
    o[1].lit[0] = be32(d + 10788);
    commit(2, 16u);
    return true;
  }
  bool tileRects(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t& r3) {
    r3 = dev;
    uint32_t c0 = c;
    if (!kickPoint()) return false;
    if (c != c0) r3 = c;
    const bool tiled = tiledSurfaces();
    if (!tiled) {
      Item* o = reserve(1);
      if (!o) return false;
      run(o[0], 0, 0x2080u, 3u, nullptr);
      o[0].lit[0] = 0u;
      o[0].lit[1] = tlbr(x0, y0);
      o[0].lit[2] = tlbr(x1, y1);
      commit(1, 16u);
      return true;
    }
    {
      Item* o = reserve(1);
      if (!o) return false;
      binReg(o[0], 1u, 0u);
      commit(1, 8u);
    }
    const uint32_t tiles = be32(d + 12472);
    for (uint32_t i = 0; i < tiles; i++) {
      const uint8_t* rc = d + 12476 + 16u * i;
      const uint8_t* of = d + 12716 + 8u * i;
      int32_t tx0 = std::max(x0, int32_t(be32(rc)));
      int32_t ty0 = std::max(y0, int32_t(be32(rc + 4)));
      int32_t tx1 = std::min(x1, int32_t(be32(rc + 8)));
      int32_t ty1 = std::min(y1, int32_t(be32(rc + 12)));
      if (tx0 >= tx1 || ty0 >= ty1) tx0 = ty0 = tx1 = ty1 = 0;
      const uint32_t nx = uint32_t(-int32_t(be32(of))), ny = uint32_t(-int32_t(be32(of + 4)));
      Item* o = reserve(2);
      if (!o) return false;
      binReg(o[0], 0u, 3u << (2u * i));
      packet(o[1], kRkSetConst, 0xC0032D01u,
             {0x00040080u, ((ny << 16) & 0x7FFF0000u) | (nx & 0x7FFFu), tlbr(tx0, ty0), tlbr(tx1, ty1)});
      commit(2, 28u);
      r3 = 3u;
      c0 = c;
      if (!kickPoint()) return false;
      if (c != c0) r3 = c;
    }
    Item* o = reserve(4);
    if (!o) return false;
    uint32_t n = 0, bytes = 0;
    auto add = [&] { bytes += o[n].bytes(); n++; };
    if ((d[10811] & 0x20u) && (d[10808] & 0x40u)) {
      binReg(o[n], 0u, 1u);
      add();
      packet(o[n], kRkSetConst, 0xC0022D01u, {0x00040081u, tlbr(x0, y0), tlbr(x1, y1)});
      add();
    }
    binReg(o[n], 0u, be32(d + 12432));
    add();
    binReg(o[n], 1u, be32(d + 12436));
    add();
    commit(n, bytes);
    return true;
  }
  // A pointer IM_LOAD of VS variant v: {phys(ucode VA), size dwords}.
  void imLoad(Item& it, uint32_t hdr, uint32_t vs, uint32_t v) {
    const uint32_t r = vs + rd(vs + 0x380u + 8u * v) + 0x368u;
    const uint32_t ua = rd(r) + rd(vs + 0x20u);
    packet(it, kRkShaderPtr, hdr, {phys(ua), rd(r + 4u) >> 2});
    g_bindPtr.fetch_add(1, std::memory_order_relaxed);
  }

  // GpuCmdUpdateSurfaceState_sub_82107110(dev, d2): device fields, then EVENT_WRITE 15 when the
  // surface changed under a pending flush flag (dev byte 10809 bit 2). The caller ORs 0x100 into d2.
  bool surfaceState() {
    uint32_t v4 = 0;
    const uint32_t ps = be32(d + 12416);
    if (ps) v4 = (rd(ps + rd(ps + 0x40u) + 0x30u) >> 27) & 1u;
    const uint32_t v6 = be32(d + 10420);
    const uint32_t v7 = be32(d + 11580) >> 20;
    const uint32_t v8 = (be32(d + 10432) & ~1u) | ((uint32_t(d[10810]) >> 5) & (v6 >> 1) & 1u);
    put32(d + 10432, v8);
    uint32_t v9;
    if (!(v7 & 2u)) {
      v9 = v7 & 1u;
    } else {
      const uint32_t v10 = (v6 >> 4) & 7u;
      const bool v12 = (v8 & 0x10u) ? (v10 == 4u || v10 == 6u || v10 == 2u) : (v10 == 1u || v10 == 3u || v10 == 2u);
      v9 = (v4 == 0u ? 1u : 0u) & (v8 & 1u) & ((v6 >> 1) & 1u) & (v12 ? 1u : 0u);
      if (v6 & 1u) {
        v9 &= ((v6 & 0x3800u) == 0u && (v6 & 0xE0000u) == 0u) ? 1u : 0u;
        if (v6 & 0x80u) v9 &= ((v6 & 0xE0000000u) == 0u && (v6 & 0x3800000u) == 0u) ? 1u : 0u;
      }
    }
    const uint32_t v14 = ((2u * v9) & 2u) | (be32(d + 10432) & ~2u);
    put32(d + 10432, v14);
    if (!(v14 & 2u)) {
      if (!(v6 & 2u) || !(v6 & 4u)) return true;
      const uint32_t v19 = (v6 >> 4) & 7u;
      if (v19 != 7u && v19 != 5u) {
        const bool ok = (v14 & 0x10u) ? (v19 == 1u || v19 == 3u) : (v19 == 4u || v19 == 6u);
        if (!ok) return true;
      }
      d[10809] |= 4u;
      return true;
    }
    if (d[10809] & 4u) {
      d[10809] &= uint8_t(~4u);
      if (!kickPoint()) return false;
      Item* o = reserve(1);
      if (!o) return false;
      packet(o[0], kRkEvent, 0xC0004600u, {15u});
      commit(1, 8u);
    }
    return true;
  }

  // GpuRingEmitFetchConstants_sub_82108470(dev, obj, dataBase): one LOAD_ALU_CONSTANT per entry of
  // the shader's constant table {u16 index, u16 count, u32 offset} (row 2).
  bool fetchConsts(uint32_t a2, uint32_t a3) {
    const uint32_t t = rd(a2 + 0x14u);
    if (!t) return true;
    const uint32_t tbl = a2 + t;
    uint32_t e = tbl + 0x14u;
    const uint32_t end = rd(tbl + 0x10u) + tbl + 0x14u;
    while (e < end) {
      const uint8_t* E = G(e);
      const uint32_t count = (uint32_t(E[2]) << 8) | E[3];
      if (!count) break;
      const uint32_t index = (uint32_t(E[0]) << 8) | E[1];
      const uint32_t src = be32(E + 4) + a3;
      e += 8u;
      if (!kickPoint()) return false;
      Item* o = reserve(1);
      if (!o) return false;
      const uint32_t ot = 4u * index;
      packet(o[0], kRkConstLoad, 0xC0022F00u, {phys(src), ot, count});
      commit(1, 16u);
      // Rule 2, as the adapter's scan treats the packet (native_const_dedup scanPackets 0x2F).
      if (ddWant && !dry && ((ot >> 16) & 0xFFu) == 0u)
        sr::native::cdedup::invalidateRegs(sr::native::cdedup::kAluReg + (ot & 0x7FFu), count & 0xFFFu);
      g_bindLoads.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }

  // D3DVsPatchFetchesInPlaceIfIdle_sub_82108520(dev, vs, decl, variant): 1 = the in-place ucode is
  // (now) right for the declaration, 0 = the GPU may still read it (the caller loads a patched copy).
  // Record mode: the guest helper (its cache, the global id counter, dev+0x2D20). Dry: the decision
  // only - the cache compare, then the fence - and the device copy's 0x2D20 fields.
  int patchInPlace(uint32_t vs, uint32_t decl, uint32_t v) {
    if (!dry) {
      ctx->r3.u64 = dev;
      ctx->r4.u64 = vs;
      ctx->r5.u64 = decl;
      ctx->r6.u64 = v;
      const uint32_t traceF = p2trace::g_on ? rd(vs + 0x1A0u * v + 0x40u) : 0u;
      guestTimed(sub_82108520);
      const int r = ctx->r3.u32 ? 1 : 0;
      if (p2trace::g_on) p2trace::onPatch(traceF, uint32_t(r));  // iter-467 P2 trace: the answer
      return r;
    }
    const uint32_t cache = vs + 0x1A0u * v;
    bool differ = rd(cache + 0x28u) != rd(decl + 0x30u);
    if (!differ) {
      uint64_t c30, c38, d0, d8, m20, m28;
      std::memcpy(&c30, G(cache + 0x30u), 8);
      std::memcpy(&c38, G(cache + 0x38u), 8);
      std::memcpy(&d0, d + 12256, 8);
      std::memcpy(&d8, d + 12264, 8);
      std::memcpy(&m20, G(decl + 0x20u), 8);
      std::memcpy(&m28, G(decl + 0x28u), 8);
      differ = (((c30 ^ d0) & m20) | ((c38 ^ d8) & m28)) != 0;
    }
    if (differ) {
      const uint32_t f = rd(cache + 0x40u);
      if (f) {
        const uint32_t cur = be32(d + 10780);
        const uint32_t gpu = rd(be32(d + 10768));
        if (cur - f < cur - gpu) return 0;
      }
    }
    std::memcpy(d + 11552, d + 12256, 16);
    return 1;
  }

  // GpuLoadShaderImmediate_sub_82108300(dev, patch, vs, &local, decl, rec, psVar, variant): INVALIDATE_STATE
  // 0x100, then IM_LOAD_IMMEDIATE with the VS ucode copied into the ring (through the 0xC0000000
  // alias, as the XDK reads it) and patched there by the guest helpers; the record takes the
  // patched copy. GpuCacheFlushRange_82106F98 (dcbf over the source) has no host equivalent and is
  // not called - the recompiler drops dcbf as well.
  bool loadImmediate(uint32_t a2, uint32_t vs, uint32_t rec, uint32_t psVar, uint32_t var, uint32_t decl,
                     uint32_t& varB0) {
    if (dry) {
      g_ckImm.fetch_add(1, std::memory_order_relaxed);
      stopped = true;
      return false;
    }
    const uint32_t sizeB = rd(rec + 4u);
    const uint32_t n = sizeB >> 2;
    const int r = reserveDw(n + 5u);
    if (r < 0) return true;
    if (!r) return false;
    Item* o = reserve(2);
    if (!o) return false;
    packet(o[0], kRkNoRecord, 0xC0003B00u, {0x100u});
    const uint32_t copyVA = c + 24u;  // after the INVALIDATE (8) and the load's header, type, size (12)
    const uint32_t ua = rd(rec) + rd(vs + 0x20u);
    const uint32_t srcVA = phys(ua) - 0x40000000u;
    // the copy into the ring, dword-wise through the alias when either side straddles a mapping
    {
      const uint32_t s0 = sr::kernel::aliasGuestVA(srcVA), s1 = sr::kernel::aliasGuestVA(srcVA + sizeB - 1u);
      const uint32_t t0 = sr::kernel::aliasGuestVA(copyVA), t1 = sr::kernel::aliasGuestVA(copyVA + sizeB - 1u);
      if (s1 >= s0 && s1 - s0 == sizeB - 1u && t1 >= t0 && t1 - t0 == sizeB - 1u) {
        std::memcpy(base + t0, base + s0, sizeB);
      } else {
        for (uint32_t i = 0; i < sizeB; i += 4u)
          std::memcpy(base + sr::kernel::aliasGuestVA(copyVA + i), base + sr::kernel::aliasGuestVA(srcVA + i),
                      std::min<uint32_t>(4u, sizeB - i));
      }
    }
    if (a2 && (G(rec + 8u)[0] & 7u) != 7u) {
      // ShaderPatchVertexFetches_sub_821081C0(rec, copy, &local, psVar): guest, on the copy
      const uint32_t sc = scratch();
      put32(base + sr::kernel::aliasGuestVA(sc), varB0);
      ctx->r3.u64 = rec;
      ctx->r4.u64 = copyVA;
      ctx->r5.u64 = sc;
      ctx->r6.u64 = psVar;
      guestTimed(sub_821081C0);
      varB0 = rd(sc);
    }
    d[10810] = uint8_t((d[10810] & 0x7Fu) | (a2 ? 0x80u : 0u));
    // ShaderAssembleVertexFetches_sub_82107A30(vs, copy, decl, dev + 12256, variant): guest, on the
    // copy. The FIFTH argument (r7 = the variant, `mr r7, r23` at 0x82108424) is one the decompiler
    // does not show; without it the helper indexed vs[0x380 + 8 * garbage] and wrote through a wild
    // vfetch table (roll p1a_cppm4: a GameKernelLoop runner's r31 overwritten, ICALL_FAIL at boot).
    ctx->r3.u64 = vs;
    ctx->r4.u64 = copyVA;
    ctx->r5.u64 = decl;
    ctx->r6.u64 = dev + 12256u;
    ctx->r7.u64 = var;
    guestTimed(sub_82107A30);
    std::memcpy(d + 11552, d + 12256, 16);
    // The record's copy of the patched ucode (host memory: the span is closed later, by then the ring
    // bytes are final, but the record writer needs one contiguous source).
    thread_local std::vector<uint8_t> t_ucode;
    if (t_ucode.size() < 4u * n) t_ucode.resize(4u * n);
    for (uint32_t i = 0; i < 4u * n; i += 4u) std::memcpy(t_ucode.data() + i, base + sr::kernel::aliasGuestVA(copyVA + i), 4);
    packet(o[1], kRkShaderImm, 0xC0002B00u | (((n + 1u) << 16) & 0x3FFF0000u), {0u, (sizeB & 0xFFFFu) >> 2}, t_ucode.data(), n);
    commit(2, 20u + 4u * n);
    g_bindImm.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // GpuEmitDrawState_sub_82108668(dev, d2). false = dry stopped.
  bool bindState(uint64_t& d2) {
    constexpr uint64_t B = 1ull;
    if (d2 & (B << 49)) {
      if (!surfaceState()) return false;
      d2 |= 0x100u;
    }
    const uint32_t vs = be32(d + 12420), decl = be32(d + 11536), ps = be32(d + 12416);
    if (!vs) return true;
    g_binds.fetch_add(1, std::memory_order_relaxed);
    uint32_t var = 0;                                 // the VS variant
    uint32_t rec = vs + rd(vs + 0x380u) + 0x368u;     // its record: +0 ucode offset, +4 size, +8 / +C, +14 signature
    uint32_t psVar = 0, r18 = 0, r17 = 0, sig;
    if (ps) {
      psVar = ps + rd(ps + 0x40u) + 0x28u;
      r18 = rd(psVar + 8u);
      r17 = rd(psVar + 0xCu);
      if ((r18 & 0x70000000u) > 0x10000000u) d[10810] |= 8u;
      if (d2 & (B << 52)) {
        if (!fetchConsts(ps + 0x28u, rd(ps + 0x18u))) return false;
        if (!kickPoint()) return false;
        d2 |= B << 46;
        const uint32_t ua = rd(psVar) + rd(ps + 0x18u);
        Item* o = reserve(1);
        if (!o) return false;
        packet(o[0], kRkShaderPtr, 0xC0012700u,
               {((((ua >> 20) + 0x200u) & 0x1000u) + (ua & 0x1FFFFFFEu)) | 1u, rd(psVar + 4u) >> 2});
        commit(1, 12u);
        g_bindPtr.fetch_add(1, std::memory_order_relaxed);
        put32(d + 10408, rd(psVar + 0x18u));
        const uint32_t m = be32(d + 10452);
        if ((m & 7u) != 4u) {
          d2 |= (B << 51) | 8u;
          put32(d + 10452, (m & ~7u) | 4u);
        }
      }
      sig = (rd(psVar + 0x14u) ^ rd(rec + 0x14u)) & 0x7FFFFu;
    } else {
      if (rd(vs + 0x368u) & 0x20u) {
        var = 1;
        rec = vs + rd(vs + 0x388u) + 0x368u;
      }
      const uint32_t m = be32(d + 10452);
      if ((m & 7u) != 5u) {
        d2 |= (B << 51) | 8u;
        put32(d + 10452, (m & ~7u) | 5u);
      }
      sig = rd(rec + 0x14u) & 0x1Fu;
    }
    const bool mismatch = sig != 0;
    const uint32_t r26 = be32(d + 10452);
    uint32_t r29 = r26;
    const uint32_t r21 = rd(rec + 0xCu);
    uint32_t varB0 = rd(rec + 8u);  // the XDK's by-reference local (821081C0 may change it)
    uint32_t zc = kNone, predVar = kNone, vsVar = kNone, c180a = kNone, c180b = kNone;
    if (d[10808] & 0x40u) {  // Z-pass: the pass's own 0x2208 mode (dev+0x3088)
      bool five = true;
      if (psVar) {
        if (be32(d + 10428) & 8u) five = false;
        else if (rd(psVar + 0x1Cu) & 0x30u) five = false;
      }
      if (five) r29 = (r29 & ~7u) | 5u;
      if (r29 != be32(d + 12424) || (d2 & 8u)) {
        d2 |= 8u;
        put32(d + 12424, r29);
        zc = r29;
      }
    }
    if ((d[10810] & 0x80u) || mismatch) {
      if (d2 & (3ull << 51)) {
        d[10810] &= 0xBFu;
        if (!fetchConsts(vs + 0x368u, rd(vs + 0x20u))) return false;
        if (!loadImmediate(mismatch ? 1u : 0u, vs, rec, psVar, var, decl, varB0)) return false;
      }
    } else if (d2 & (B << 51)) {
      if (!fetchConsts(vs + 0x368u, rd(vs + 0x20u))) return false;
      d[10810] &= 0xBFu;
      const uint32_t vflags = rd(vs + 0x368u);
      if (!decl || (vflags & 0x40u)) {
        vsVar = var;
        if (((r29 ^ r26) & 7u) && (vflags & 0x20u)) {
          d[10810] |= 0x40u;
          predVar = 1;
        }
      } else if (d[10808] & 0x80u) {
        if (!loadImmediate(0u, vs, rec, psVar, var, decl, varB0)) return false;
      } else {
        int r = patchInPlace(vs, decl, var);
        if (r) vsVar = var;
        else if (!loadImmediate(0u, vs, rec, psVar, var, decl, varB0)) return false;
        if (((r29 ^ r26) & 7u) && (vflags & 0x20u)) {
          r = patchInPlace(vs, decl, 1u);
          if (r) {
            d[10810] |= 0x40u;
            predVar = 1;
          }
        }
      }
    }
    if (d2 & (3ull << 51)) {
      put32(d + 10404, r21 | r17);
      put32(d + 10400, varB0 | r18);
      d2 |= 3ull << 47;
      if (d[10810] & 0x40u) {
        const uint32_t v1 = vs + rd(vs + 0x388u);
        c180a = rd(v1 + 0x370u) | r18;
        c180b = rd(v1 + 0x374u) | r17;
      }
    }
    if (d[10808] & 0x40u) {  // the Z-pass emission: one kick check, then up to 11 packets
      if (!kickPoint()) return false;
      Item* o = reserve(12);
      if (!o) return false;
      uint32_t n = 0, bytes = 0;
      auto add = [&] { bytes += o[n].bytes(); n++; };
      binReg(o[n], 0u, 0x15555555u);
      add();
      binReg(o[n], 1u, 0u);
      add();
      if (d2 & 8u) {
        d2 &= ~8ull;
        run(o[n], 0, 0x2208u, 1u, d + 10452);
        add();
      }
      if (zc != kNone) {
        packet(o[n], kRkSetConst, 0xC0012D01u, {0x00040208u, zc});
        add();
      }
      if (d2 & (B << 48)) {
        d2 &= ~(3ull << 47);
        run(o[n], 0, 0x2180u, 2u, d + 10400);
        add();
      }
      if (c180a != kNone) {
        packet(o[n], kRkSetConst, 0xC0022D01u, {0x00040180u, c180a, c180b});
        add();
      }
      if (predVar != kNone) {
        imLoad(o[n], 0xC0012701u, vs, predVar);
        add();
        if (vsVar != kNone) {
          binReg(o[n], 0u, 0x2AAAAAAAu);
          add();
          imLoad(o[n], 0xC0012701u, vs, vsVar);
          add();
        }
      } else if (vsVar != kNone) {
        imLoad(o[n], 0xC0012700u, vs, vsVar);
        add();
      }
      binReg(o[n], 0u, be32(d + 12432));
      add();
      binReg(o[n], 1u, be32(d + 12436));
      add();
      commit(n, bytes);
    } else if (vsVar != kNone) {
      if (!kickPoint()) return false;
      Item* o = reserve(1);
      if (!o) return false;
      imLoad(o[0], 0xC0012700u, vs, vsVar);
      commit(1, 12u);
    }
    return true;
  }

  // P1 rows 4+6+7: D3DDeviceEmitDrawIndexedGen_sub_820FB6E8 (BeginVertices; DrawVerticesUP 820FBBB0
  // calls it) after flushState: a kick check, GpuCmdBufAllocate_820FCDF0 (the ring's own allocator:
  // the vertex space is carved from the segment's TAIL, dev+44/48 move - a guest call between spans,
  // as the reserve is), then type-0 0x2102 {0}, the UP fetch slot 31 (0x48BA x6), 0x5000 x3 and the
  // predicated auto-index DRAW_INDX. The XDK leaves dev+40 BEFORE these packets and publishes it from
  // dev+0x3314 at EndVertices / after the UP memcpy; the caller writes the vertices into the
  // allocation meanwhile. false = dry stopped. Record mode: allocVA = the allocation (0 = the XDK's
  // failure: nothing emitted after the flush), preDraw = the cursor the XDK leaves in dev+40.
  bool beginVertices(uint32_t prim, uint32_t count, uint32_t strideB, uint32_t& allocVA, uint32_t& preDraw) {
    const uint32_t dwords = (count * strideB) >> 2;
    allocVA = 0;
    if (!kickPoint()) return false;
    if (!beforeGuest()) return false;
    ctx->r3.u64 = dev;
    ctx->r4.u64 = dwords;
    ctx->r5.u64 = 16u;
    sub_820FCDF0(*ctx, base);
    allocVA = ctx->r3.u32;
    afterGuest();
    preDraw = c;
    g_beginV.fetch_add(1, std::memory_order_relaxed);
    if (!allocVA) {
      g_beginAllocFail.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    Item* o = reserve(4);
    if (!o) return false;
    run(o[0], 0, 0x2102u, 1u, nullptr);
    o[0].lit[0] = 0u;
    run(o[1], 0, 0x48BAu, 6u, nullptr);
    o[1].lit[0] = 1u;
    o[1].lit[1] = 0u;
    o[1].lit[2] = 1u;
    o[1].lit[3] = 0u;
    o[1].lit[4] = phys(allocVA) | 3u;
    o[1].lit[5] = ((4u * dwords) & 0x3FFFFFCu) | 0x10000002u;
    run(o[2], 0, 0x5000u, 3u, nullptr);
    o[2].lit[0] = o[2].lit[1] = o[2].lit[2] = 0u;
    Item& dr = o[3];
    dr.pad = 0;
    dr.pkt = 1;
    dr.rk = kRkDraw;
    dr.reg = dr.count = 0;
    dr.src = nullptr;
    dr.chk = false;
    dr.fixedBytes = 0;
    dr.deferSrc = 0;
    dr.drawHdr = 0xC0012201u;
    dr.body[0] = viz();
    dr.body[1] = (count << 16) | (prim & 0x3Fu) | 0x80u;
    dr.bodyN = 2;
    commit(4, 8u + 28u + 16u + 12u);
    return true;
  }

  // The flush (820FC000 / 820FBBF8 up to the draw packet). false = dry stopped.
  bool flushState() {
    if (!g_on && !g_check) return false;  // the gates (SR_NATIVE_FLUSH / _CHECK) at every write site
    const uint64_t e0 = be64(d + 0), e1 = be64(d + 8);
    uint64_t d2 = be64(d + 16);
    const uint64_t d3 = be64(d + 24), d4 = be64(d + 32);
    if (e0) {
      if (!constRuns(e0, 0x4000u, 1792u)) return false;
      put64(d + 0, 0);
    }
    if (e1) {
      if (!constRuns(be64(d + 8), 0x4400u, 5888u)) return false;
      put64(d + 8, 0);
    }
    if (d2) {
      if (d2 & (0xFull << 49)) {
        // P1 rows 1+2: the bind as host code (bindState). In-roll A/B (SR_AB_TOGGLE=SR_NATIVE_BIND, per
        // window or per present with SR_AB_PRESENT=1): the guest call in the OFF windows.
        static const int s_abBind = sr::abt::reg("SR_NATIVE_BIND");
        if (g_bind && (s_abBind < 0 || sr::abt::on(s_abBind))) {
          if (!bindState(d2)) return false;
        } else {
          if (!beforeGuest()) return false;
          ctx->r3.u64 = dev;
          ctx->r4.u64 = d2;
          sub_82108668(*ctx, base);
          d2 = ctx->r3.u64;
          afterGuest();
        }
      }
      if (d2 & 0xFFFull) {
        if (d2 & 0x100ull) {
          if (d[10811] & 0x20u) {  // the tiled branch: P1 row 3 (host) or the guest leaf
            static const int s_abWs = sr::abt::reg("SR_NATIVE_WSCISSOR");
            if (g_wscissor && (s_abWs < 0 || sr::abt::on(s_abWs))) {
              if (!windowScissorState(d2)) return false;
            } else {
              if (!beforeGuest()) return false;
              ctx->r3.u64 = dev;
              ctx->r4.u64 = d2;
              ctx->r5.u64 = be32(d + 10432);
              ctx->r6.u64 = 0;
              sub_821072F8(*ctx, base);
              d2 = ctx->r3.u64;
              afterGuest();
            }
          } else {
            if (!kickPoint()) return false;
            Item* out = reserve(1);
            if (!out) return false;
            run(out[0], 0, 0x2203u, 1u, nullptr);
            out[0].lit[0] = be32(d + 10432);
            commit(1, 8u);
            d2 &= ~0x100ull;
          }
        }
        if (d2 & 0xFFFull)
          if (!regRuns((d2 & 0xFFFull) << 52, 0x2200u, 10420u)) return false;
      }
      if (d2 & 0x00000FFFFFFFF000ull)
        if (!fetchRuns((d2 << 20) & 0xFFFFFFFF00000000ull)) return false;
      if (d2 & (31ull << 44))
        if (!regRuns((d2 << 15) & (31ull << 59), 0x2180u, 10400u)) return false;
      put64(d + 16, 0);
    }
    if (d3) {
      if (d3 & 2ull) {
        put64(d + 11568, 0xFFFFFFFFFF000000ull);
        if (!regRuns(0xFFFFFFFFFF000000ull, 0x4900u, 9984u)) return false;
      }
      if (d3 & 0x7FFFFCull)
        if (!regRuns((d3 << 41) & (0x1FFFFFull << 43), 0x2100u, 10316u)) return false;
      if (d3 & 0x7FFF800000ull)
        if (!regRuns((d3 << 25) & (0xFFFFull << 48), 0x2000u, 10240u)) return false;
      if (d3 & (0x1FFFFFull << 39))
        if (!regRuns((d3 << 4) & (0x1FFFFFull << 43), 0x2280u, 10468u)) return false;
      put64(d + 24, 0);
    }
    if (d4) {
      if (d4 & 0x3FFFFFFFFFull)
        if (!regRuns((d4 << 26) & (0x3FFFFFFFFFull << 26), 0x2300u, 10552u)) return false;
      if (d4 & (0xFFull << 38))
        if (!regRuns((d4 << 18) & (0xFFull << 56), 0x2380u, 10704u)) return false;
      if (d4 & (0x3Full << 49)) {
        const uint64_t mask = (d4 << 9) & (0x3Full << 58);
        static const int ab = sr::abt::reg("SR_NATIVE_SHADER_CONST_RUNS");
        if (shaderConstRunsOn() && (ab < 0 || sr::abt::on(ab))) {
          uint64_t result;
          if (!shaderConstantRuns(mask, result)) return false;
          g_scrDrv.fetch_add(1, std::memory_order_relaxed);
        } else {
          if (shaderConstRunsOn()) g_scrAbGuest.fetch_add(1, std::memory_order_relaxed);
          if (!beforeGuest()) return false;
          ctx->r3.u64 = dev;
          ctx->r4.u64 = mask;
          // This branch already selected the SDK while we own g_mu. An
          // asynchronous A/B flip must not re-enter the shared native leaf.
          ShaderConstRunsOracle oracle;
          sub_821075E0(*ctx, base);
          afterGuest();
        }
      }
      put64(d + 32, 0);
    }
    return true;
  }

  uint32_t viz() const {
    const uint8_t b = d[11915];
    return (b ? 0x100u : 0u) | (d[11850u + b] & 0x3Fu);
  }

  // [0x2102, base] + DRAW_INDX. kind 0: prim, baseVertex, startIndex, count; 1: prim, start, count.
  // p1.flush_residue: count > 0xFFFF reaches here only under SR_NATIVE_FLUSH_SPLIT with a non-zero indices-per-prim
  // entry (declined() decided): 820FC000's loop 820FC27C..820FC468 / 820FBBF8's 820FBE68..820FBFF0 (asm, IDA) - per
  // part the kick check, {0x2102 = r5} (baseVertex; for DrawVertices the CURRENT startVertex) and the DRAW_INDX of
  // chunk = ((0xFFFF / ipp) & ~1) * ipp indices (divwu; the whole rest once it is <= 0xFFFF), then count = overlap +
  // count - chunk and the start (startIndex / startVertex) += chunk - overlap, 32-bit wrap as the asm.
  bool drawPacket(int kind, uint32_t prim, uint32_t a, uint32_t b, uint32_t count, uint32_t ib0, uint32_t ib6) {
    if (count <= 0xFFFFu) return drawPart(kind, prim, a, b, count, ib0, ib6);
    uint32_t ipp = 0, overlap = 0;
    if (!splitTable(prim, ipp, overlap) || !ipp) return false;  // declined() refused these before anything was written
    g_bigDraws.fetch_add(1, std::memory_order_relaxed);
    for (;;) {
      const uint32_t chunk = count > 0xFFFFu ? ((0xFFFFu / ipp) & ~1u) * ipp : count;
      const bool kicks = c > thrEff();
      if (!drawPart(kind, prim, a, b, chunk, ib0, ib6)) return false;
      if (kicks) g_bigKicks.fetch_add(1, std::memory_order_relaxed);
      g_bigParts.fetch_add(1, std::memory_order_relaxed);
      if (count == chunk) return true;
      count = overlap + (count - chunk);
      if (kind == 0) b += chunk - overlap;
      else a += chunk - overlap;
    }
  }
  bool drawPart(int kind, uint32_t prim, uint32_t a, uint32_t b, uint32_t count, uint32_t ib0, uint32_t ib6) {
    if (!g_on && !g_check) return false;  // the gates (SR_NATIVE_FLUSH / _CHECK) at every write site
    if (!kickPoint()) return false;
    Item* out = reserve(2);
    if (!out) return false;
    run(out[0], 0, 0x2102u, 1u, nullptr);
    out[0].lit[0] = a;
    Item& dr = out[1];
    dr.pad = 0;
    dr.pkt = 1;
    dr.rk = kRkDraw;
    dr.reg = dr.count = 0;
    dr.src = nullptr;
    dr.chk = false;
    dr.fixedBytes = 0;
    dr.deferSrc = 0;
    if (kind == 0) {
      uint32_t initiator = (count << 16) | (prim & 0x3Fu);
      const uint32_t endian = (ib0 << 1) & 0xC0000000u;
      uint32_t size = (count & 0xFFFFFFu) | endian;
      uint32_t addr = b * 2u + ib6;
      if (ib0 & 0x80000000u) {
        addr = b * 4u + ib6;
        size = ((count << 1) & 0x00FFFFFEu) | endian;
        initiator |= 0x800u;
      }
      const uint32_t dmaBase = ((((addr >> 20) & 0xFFFu) + 512u) & 0x1000u) + (addr & 0x1FFFFFFFu);
      dr.drawHdr = 0xC0032201u;
      dr.body[0] = viz();
      dr.body[1] = initiator;
      dr.body[2] = dmaBase;
      dr.body[3] = size;
      dr.bodyN = 4;
    } else {
      dr.drawHdr = 0xC0012201u;
      dr.body[0] = viz();
      dr.body[1] = (count << 16) | (prim & 0x3Fu) | 0x80u;
      dr.bodyN = 2;
    }
    commit(2, out[0].bytes() + dr.bytes());
    return true;
  }
};

void report(const char* why) {
  std::string w;
  char b[64];
  for (int i = 0; i < kWhyN; i++) {
    std::snprintf(b, sizeof(b), " %s=%llu", kWhyName[i], (unsigned long long)g_why[i].get());
    w += b;
  }
  const double tsc = [] {
    static double f = 0;
    if (f == 0) {
      LARGE_INTEGER q0, q1;
      QueryPerformanceCounter(&q0);
      const uint64_t t0 = __rdtsc();
      Sleep(20);
      QueryPerformanceCounter(&q1);
      const uint64_t t1 = __rdtsc();
      LARGE_INTEGER fq;
      QueryPerformanceFrequency(&fq);
      f = double(t1 - t0) / (double(q1.QuadPart - q0.QuadPart) / double(fq.QuadPart)) / 1e6;  // ticks/us
    }
    return f;
  }();
  auto us = [&](uint64_t t, uint64_t n) { return n ? double(t) / tsc / double(n) : 0.0; };
  if (g_on)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s draws:%s | spans rec=%llu pm4=%llu litLenBad=%llu guestCalls=%llu kicks=%llu sectionFallbacks=%llu "
                "ringKB=%llu recKB=%llu | CP recs=%llu runs=%llu draws=%llu bad=%llu torn=%llu nearReclaim=%llu | reclaim audited=%llu NEVER-EXECUTED=%llu "
                "desync=%llu boundBad=%llu boundOver=%llu | us/draw guest=%.3f (n=%llu) "
                "cp(record walk + runs, no emitDraw)=%.3f (n=%llu) | guest calls inside us/draw=%.3f | "
                "constGroups seen=%llu resident=%llu skipped=%llu | constRuns sectionAborts=%llu afterPlanning=%llu shortSkipSpans=%llu"
                " | bind%s binds=%llu imm=%llu ptr=%llu loads=%llu helpers=%llu reserveFlush=%llu epochStale=%llu segStress=%u | CP packets=%llu"
                " | entries=%llu entryPkts=%llu entryDecl=%llu entryRuns=%llu allocs=%llu guest=%llu noRender=%llu"
                " | beginV=%llu allocFail=%llu | tileRects=%llu tiled=%llu decl=%llu"
                " | setConstMem=%llu decl=%llu fail=%llu cpPm4=%llu"
                " | seg closed=%llu parts=%llu gapKB=%llu recApplied=%llu gapApplied=%llu"
                " idMiss=%llu mismatch=%llu handoff=%llu replayHit=%llu replayMiss=%llu expired=%llu"
                " guestMode=%llu coher=%llu pinned=%llu chained=%llu footStray=%llu feetLost=%llu coherDropOld=%llu",
                why, w.c_str(), (unsigned long long)g_spansRec.get(), (unsigned long long)g_spansPm4.get(),
                (unsigned long long)g_litLenBad.get(),
                (unsigned long long)g_guestCalls.get(), (unsigned long long)g_kicks.get(),
                (unsigned long long)g_fallbackSec.get(), (unsigned long long)(g_ringBytes.get() >> 10),
                (unsigned long long)(g_recBytes.get() >> 10), (unsigned long long)g_cpRecs.get(),
                (unsigned long long)g_cpRuns.get(), (unsigned long long)g_cpDraws.get(),
                (unsigned long long)g_cpBad.get(), (unsigned long long)g_cpTorn.get(),
                (unsigned long long)g_cpNearReclaim.get(), (unsigned long long)g_recAudited.get(),
                (unsigned long long)g_recDropped.get(), (unsigned long long)g_recDesync.get(),
                (unsigned long long)g_recBoundBad.get(), (unsigned long long)g_recBoundOver.get(),
                us(g_tGuestTsc.get(), g_tGuestN.get()),
                (unsigned long long)g_tGuestN.get(), us(g_tCpTsc.get(), g_tCpN.get()),
                (unsigned long long)g_tCpN.get(), us(g_tGuestCallsTsc.get(), g_tGuestN.get()),
                (unsigned long long)g_ddSeen.get(), (unsigned long long)g_ddPred.get(),
                (unsigned long long)g_ddSkipped.get(), (unsigned long long)g_sectionAborts.get(),
                (unsigned long long)g_sectionAbortsPlanned.get(), (unsigned long long)g_shortSkipSpans.get(),
                g_bind ? "=native" : "=guest", (unsigned long long)g_binds.get(), (unsigned long long)g_bindImm.get(),
                (unsigned long long)g_bindPtr.get(), (unsigned long long)g_bindLoads.get(),
                (unsigned long long)g_bindHelpers.get(), (unsigned long long)g_reserveFlushes.get(),
                (unsigned long long)g_epochStale.get(), g_segStress, (unsigned long long)g_cpPackets.get(),
                (unsigned long long)g_entries.get(), (unsigned long long)g_entryPkts.get(),
                (unsigned long long)g_entryDecl.get(), (unsigned long long)g_entryRuns.get(),
                (unsigned long long)g_entryAllocs.get(), (unsigned long long)g_entryGuest.get(),
                (unsigned long long)g_opaqueNoRender.get(), (unsigned long long)g_beginV.get(),
                (unsigned long long)g_beginAllocFail.get(), (unsigned long long)g_tileRectCalls.get(),
                (unsigned long long)g_tileRectTiled.get(), (unsigned long long)g_tileRectDecl.get(),
                (unsigned long long)g_setConstMemCalls.get(), (unsigned long long)g_setConstMemDecl.get(),
                (unsigned long long)g_setConstMemFail.get(), (unsigned long long)g_cpSetConstPm4.get(),
                (unsigned long long)g_segClosed.get(),
                (unsigned long long)g_segParts.get(), (unsigned long long)(g_segGapBytes.get() >> 10),
                (unsigned long long)g_segRecApplied.get(), (unsigned long long)g_segGapApplied.get(),
                (unsigned long long)g_segIdMiss.get(), (unsigned long long)g_segMismatch.get(),
                (unsigned long long)g_segHandoff.get(), (unsigned long long)g_segReplayHit.get(),
                (unsigned long long)g_segReplayMiss.get(), (unsigned long long)g_segExpired.get(),
                (unsigned long long)g_segGuestMode.get(),
                (unsigned long long)g_segCoher.get(), (unsigned long long)g_segPinned.get(),
                (unsigned long long)g_segChained.get(),
                (unsigned long long)g_segFootStray.get(), (unsigned long long)g_segFeetLost.get(),
                (unsigned long long)g_segCoherDropOld.get());
  if (g_on && g_spansPm4.get()) {
    namespace m = sr::native::meter;
    static const char* const kN[m::kKinds] = {"t0",  "t1",   "t2nop", "drawDma", "drawAuto", "drawImm", "shader",
                                             "const", "ib", "sync",  "event",   "bin",      "nop3",    "other"};
    char w[400];
    int at = 0;
    uint64_t tot = 0;
    for (uint32_t k = 0; k < m::kKinds && at < int(sizeof(w)) - 32; k++) {
      tot += g_litKind[k].get();
      at += std::snprintf(w + at, sizeof(w) - size_t(at), " %s=%llu", kN[k], (unsigned long long)g_litKind[k].get());
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_FLUSH] %s literal spans=%llu packets=%llu by kind:%s", why,
                (unsigned long long)g_spansPm4.get(), (unsigned long long)tot, w);
  }
  if (g_check) {
    // the per-kind tail: every bracket kind the CHECK completed or found bad, ascending ("k8=1/0 k9=5400/0")
    char ck[256];
    int at = 0;
    for (int k = 0; k < kCkKinds && at < int(sizeof(ck)) - 48; k++) {
      const unsigned long long f = (unsigned long long)g_ckKindFull[k].get(), b = (unsigned long long)g_ckKindBad[k].get();
      if (f || b) at += std::snprintf(ck + at, sizeof(ck) - size_t(at), " k%d=%llu/%llu", k, f, b);
    }
    ck[at] = 0;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s CHECK draws=%llu full=%llu prefix=%llu skipped=%llu kick=%llu | BAD bytes=%llu "
                "len=%llu dev=%llu | us/draw native(dry)=%.3f guest=%.3f (n=%llu) | bind%s binds=%llu ptr=%llu loads=%llu "
                "immStops=%llu segStress=%u | kinds full/bad:%s",
                why, (unsigned long long)g_ckN.get(), (unsigned long long)g_ckFull.get(),
                (unsigned long long)g_ckPrefix.get(), (unsigned long long)g_ckSkip.get(),
                (unsigned long long)g_ckKick.get(), (unsigned long long)g_ckBadBytes.get(),
                (unsigned long long)g_ckBadLen.get(), (unsigned long long)g_ckBadDev.get(),
                us(g_ckNativeTsc.get(), g_ckTimed.get()), us(g_ckGuestTsc.get(), g_ckTimed.get()),
                (unsigned long long)g_ckTimed.get(), g_bind ? "=native" : "=guest", (unsigned long long)g_binds.get(),
                (unsigned long long)g_bindPtr.get(), (unsigned long long)g_bindLoads.get(),
                (unsigned long long)g_ckImm.get(), g_segStress, ck);
  }
  if (callbackOn() || g_check)  // p2.transport step 1: the CALLBACK records and the leaf's CHECK
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s callback calls=%llu decl=%llu applied=%llu predSkip=%llu chk=%llu chkBad=%llu", why,
                (unsigned long long)g_cbCalls.get(), (unsigned long long)g_cbDecl.get(),
                (unsigned long long)g_cbApplied.get(), (unsigned long long)g_cbPredSkip.get(),
                (unsigned long long)g_cbChk.get(), (unsigned long long)g_cbChkBad.get());
  if (replayKickCallbackOn())
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s replayKickCallback emitted=%llu published=%llu applied=%llu chained=%llu literal=%llu omitted=%llu", why,
                (unsigned long long)g_kickCbEmitted.get(), (unsigned long long)g_kickCbPublished.get(),
                (unsigned long long)g_kickCbApplied.get(), (unsigned long long)g_kickCbChained.get(),
                (unsigned long long)g_kickCbLiteral.get(), (unsigned long long)g_kickCbOmitted.get());
  if (g_bracket) {  // p2.transport BRACKET (SR_NATIVE_BRACKET): producer, declines by reason, consumer
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s bracket begin=%llu beginDecl=%llu abOff=%llu end=%llu failed=%llu literal=%llu "
                "decl noGen=%llu poisoned=%llu gate=%llu mode=%llu memory=%llu walk=%llu bind=%llu published=%llu "
                "pending=%llu "
                "| stale=%llu poison=%llu retired=%llu chained=%llu | consumed=%llu bs=%llu segs=%llu", why,
                (unsigned long long)g_brBegin.get(), (unsigned long long)g_brBeginDecl.get(),
                (unsigned long long)g_brBeginAbOff.get(), (unsigned long long)g_brEnd.get(),
                (unsigned long long)g_brEndFailed.get(), (unsigned long long)g_brLiteral.get(),
                (unsigned long long)g_brDecl[kBrNoGen].get(), (unsigned long long)g_brDecl[kBrPoisoned].get(),
                (unsigned long long)g_brDecl[kBrGate].get(), (unsigned long long)g_brDecl[kBrMode].get(),
                (unsigned long long)g_brDecl[kBrMemory].get(), (unsigned long long)g_brDecl[kBrWalk].get(),
                (unsigned long long)g_brDecl[kBrBind].get(), (unsigned long long)g_brDecl[kBrPublished].get(),
                (unsigned long long)g_brDecl[kBrPending].get(),
                (unsigned long long)g_brStale.get(), (unsigned long long)g_brPoison.get(),
                (unsigned long long)g_brRetired.get(), (unsigned long long)g_brChained.get(),
                (unsigned long long)g_brConsumed.get(), (unsigned long long)g_brBs.get(),
                (unsigned long long)g_brSegs.get());
    // the memory declines by site (a separate line: the bracket line's own pattern is graded as written); list 0, 1, 2
    // = dev+0x32C8 / 0x32A0 / 0x32B4
    auto m = [](int s) { return (unsigned long long)g_brMemory[s].get(); };
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s bracketMemory frame=%llu dev=%llu stack=%llu lists=%llu align=%llu/%llu/%llu "
                "prefix=%llu/%llu/%llu cursor=%llu/%llu/%llu entry=%llu", why,
                m(kBmFrame), m(kBmDev), m(kBmStack), m(kBmLists), m(kBmAlign0), m(kBmAlign1), m(kBmAlign2),
                m(kBmPrefix0), m(kBmPrefix1), m(kBmPrefix2), m(kBmCursor0), m(kBmCursor1), m(kBmCursor2), m(kBmEntry));
  }
  if (g_cutSegs.get() || g_cutKicks.get() || g_cutCoher.get() || g_cutEnds.get())  // the graceful quit's ring cut
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_FLUSH] %s quitCut segs=%llu kicks=%llu coherency=%llu ends=%llu", why,
                (unsigned long long)g_cutSegs.get(), (unsigned long long)g_cutKicks.get(),
                (unsigned long long)g_cutCoher.get(), (unsigned long long)g_cutEnds.get());
  if (idleWaitOn() || invalRangeOn() || lockCoherOn()) {  // p2.transport RESIDUE: taken / late / declines by reason
    auto n = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    char decl[kRsN][96];
    for (int p = 0; p < kRsN; p++)
      std::snprintf(decl[p], sizeof(decl[p]), "ab=%llu seg=%llu mem=%llu mode=%llu entry=%llu owner=%llu noPk=%llu",
                    n(g_rsDecl[p][kRsAb]), n(g_rsDecl[p][kRsSeg]), n(g_rsDecl[p][kRsMem]), n(g_rsDecl[p][kRsMode]),
                    n(g_rsDecl[p][kRsEntry]), n(g_rsDecl[p][kRsOwner]), n(g_rsDecl[p][kRsNoPk]));
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s residue idle=%llu late=%llu decl %s | inval=%llu late=%llu merge=%llu decl %s | "
                "lock=%llu late=%llu merge=%llu host=%llu decl %s | pk=%llu litPk=%llu | hostApplied=%llu", why,
                n(g_rsTaken[kRsIdle]), n(g_rsLate[kRsIdle]), decl[kRsIdle], n(g_rsTaken[kRsInval]),
                n(g_rsLate[kRsInval]), n(g_rsMerge[kRsInval]), decl[kRsInval], n(g_rsTaken[kRsLock]),
                n(g_rsLate[kRsLock]), n(g_rsMerge[kRsLock]), n(g_rsHost), decl[kRsLock], n(g_rsPk), n(g_rsLitPk),
                n(g_rsHostApplied));
  }
  if (presentGateOn())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_FLUSH] %s presentGate calls=%llu skipped=%llu decl=%llu applied=%llu", why,
                (unsigned long long)g_gateCalls.get(), (unsigned long long)g_gateSkip.get(),
                (unsigned long long)g_gateDecl.get(), (unsigned long long)g_gateApplied.get());
  if (swapBlockOn())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_FLUSH] %s swapBlock calls=%llu decl=%llu applied=%llu", why,
                (unsigned long long)g_swapCalls.get(), (unsigned long long)g_swapDecl.get(),
                (unsigned long long)g_swapApplied.get());
  if (swapOn()) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_FLUSH] %s swapSdk calls=%llu decl=%llu", why,
                (unsigned long long)g_swapSdkCalls.get(), (unsigned long long)g_swapSdkDecl.get());
    // the declines by reason, in admission order (a separate line: the swapSdk line is graded as written); the two
    // post-swap literal fallbacks last (those calls still count as taken)
    char reasons[1024];  // 25 reasons (kSwWhyN), each name=value (<= 36 chars)
    int at = 0;
    for (int w = 0; w < kSwWhyN && at >= 0 && at < int(sizeof(reasons)); w++)
      at += std::snprintf(reasons + at, sizeof(reasons) - size_t(at), "%s%s=%llu", w ? " " : "", swapSdkDeclName(w),
                          (unsigned long long)g_swapWhy[w].get());
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_FLUSH] %s swapSdkWhy %s", why, reasons);
  }
  // boot gaps: the display emitters and the mode state ([NATIVE_FLUSH] <why> display ...; printed with the gates off too:
  // the =0 arm's zeros are its witness)
  displayReport(why);
  if (g_check)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s presentGate CHECK tested=%llu skipped=%llu bad=%llu declined=%llu", why,
                (unsigned long long)g_gateChk.get(), (unsigned long long)g_gateChkSkip.get(),
                (unsigned long long)g_gateChkBad.get(), (unsigned long long)g_gateChkDecl.get());
  {  // printed with the gate off too (the =0 arm: the tessellated draws, the SDK body everywhere)
    auto ld = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    unsigned long long decl = 0;
    for (const auto& one : g_scrDecl) decl += ld(one);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s shaderConstRuns calls=%llu chunked=%llu | gate=%d drv=%llu sdk=%llu abGuest=%llu "
                "decl=%llu (off %llu oracle %llu mask %llu span %llu open %llu emit %llu) | kick=%llu marginBad=%llu "
                "| tessDraws=%llu tessSdk=%llu | cpPm4=%llu",
                why, (unsigned long long)g_shaderConstRuns.get(), (unsigned long long)g_shaderConstChunks.get(),
                shaderConstRunsOn() ? 1 : 0, ld(g_scrDrv), ld(g_scrSdk), ld(g_scrAbGuest), decl,
                ld(g_scrDecl[kScrOff]), ld(g_scrDecl[kScrOracle]), ld(g_scrDecl[kScrMask]), ld(g_scrDecl[kScrSpan]),
                ld(g_scrDecl[kScrOpen]), ld(g_scrDecl[kScrEmit]), ld(g_scrKick), ld(g_scrMarginBad), ld(g_tessDraws),
                ld(g_tessSdk), (unsigned long long)g_cpShaderSyncPm4.get());
  }
  if (g_split)  // p1.flush_residue (SR_NATIVE_FLUSH_SPLIT): the census of each native path (build note)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s split sections const=%llu reg=%llu fetch=%llu chunks const=%llu reg=%llu fetch=%llu "
                "kicks const=%llu reg=%llu fetch=%llu slowTrailer=%llu | big draws=%llu parts=%llu kicks=%llu decl=%llu",
                why, (unsigned long long)g_splitSec[0].get(), (unsigned long long)g_splitSec[1].get(),
                (unsigned long long)g_splitSec[2].get(), (unsigned long long)g_splitChunks[0].get(),
                (unsigned long long)g_splitChunks[1].get(), (unsigned long long)g_splitChunks[2].get(),
                (unsigned long long)g_splitKicks[0].get(), (unsigned long long)g_splitKicks[1].get(),
                (unsigned long long)g_splitKicks[2].get(), (unsigned long long)g_splitSlowTrailer.get(),
                (unsigned long long)g_bigDraws.get(), (unsigned long long)g_bigParts.get(),
                (unsigned long long)g_bigKicks.get(), (unsigned long long)g_bigDecl.get());
  if (g_zpassLeaf)  // C1 stage 1, L3b's lever: the sections it sent to the guest leaf (each inside a Z-pass recording)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s zpassLeaf (SR_DIAG_ZPASS_LEAF) sections const=%llu reg=%llu fetch=%llu", why,
                (unsigned long long)g_zpassLeafSec[0].get(), (unsigned long long)g_zpassLeafSec[1].get(),
                (unsigned long long)g_zpassLeafSec[2].get());
}

// p1.flush_residue (Astra rounds 1-2): the gate's residue counters, cumulative from boot, as ONE line (only under
// SR_NATIVE_FLUSH_SPLIT): the S1 declines the gate must leave at 0 (count>0xFFFF beyond the two classes it cannot
// convert, section fallbacks / aborts) and row 9's per-entry record counters. maybeReport reads the clock on EVERY call
// (a taken or dry-checked draw; GetTickCount64 reads the shared user page) and prints the line on the first such call
// at least kSplitSnapMs after the previous line: while draws run, consecutive lines are kSplitSnapMs apart plus at most
// one inter-draw gap and the tick granularity (~16 ms); with no draw, no line. The line is NOT terminal: the rolls end
// by Stop-Process (no quit hook), so the proof (tools/perf/flush_split_snapshot.py) grades the counters from boot
// through the last line; freshness relative to the last logged timestamp allows 1.0 s (period + 0.5 s); the remaining
// execution tail is unmeasured. The line is formatted and logged under the producer
// lock g_mu (draw()'s lock_guard; once a period): its cost to the other producers is to be measured in a roll. The
// periodic draws line (report) comes every 2^18 draws (~7.5 s in gameplay).
constexpr uint64_t kSplitSnapMs = 500;
std::atomic<uint64_t> g_splitSnapAt{0}, g_splitSnapN{0};
// The line's text after "[NATIVE_FLUSH] " (the difftest prints it for the parser's format contract).
std::string splitSnapshotText() {
  const uint64_t n = g_splitSnapN.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t r[3][sr::native::recb::kRecCounters];
  for (int e = 0; e < 3; e++) sr::native::recb::recCounters(e, r[e]);
  char rec[3][224];
  static const char* const kName[3] = {"resolve", "clearF", "gpr"};
  for (int e = 0; e < 3; e++)
    std::snprintf(rec[e], sizeof rec[e],
                  "%s calls=%llu taken=%llu open=%llu pre=%llu live=%llu kickT=%llu kickTStop=%llu waitT=%llu",
                  kName[e], (unsigned long long)r[e][0], (unsigned long long)r[e][1], (unsigned long long)r[e][6],
                  (unsigned long long)r[e][7], (unsigned long long)r[e][5], (unsigned long long)r[e][2],
                  (unsigned long long)r[e][4], (unsigned long long)r[e][8]);
  char line[1024];
  std::snprintf(line, sizeof line,
                "split snapshot #%llu period=%llums draws=%llu count>0xFFFF=%llu ipp0=%llu noSplit=%llu "
                "sectionFallbacks=%llu sectionAborts=%llu afterPlanning=%llu | big draws=%llu parts=%llu | sections "
                "const=%llu reg=%llu fetch=%llu | %s | %s | %s",
                (unsigned long long)n, (unsigned long long)kSplitSnapMs, (unsigned long long)g_why[kTaken].get(),
                (unsigned long long)g_why[kCount].get(), (unsigned long long)g_bigDecl.get(),
                (unsigned long long)g_bigDeclOther.get(), (unsigned long long)g_fallbackSec.get(),
                (unsigned long long)g_sectionAborts.get(), (unsigned long long)g_sectionAbortsPlanned.get(),
                (unsigned long long)g_bigDraws.get(), (unsigned long long)g_bigParts.get(),
                (unsigned long long)g_splitSec[0].get(), (unsigned long long)g_splitSec[1].get(),
                (unsigned long long)g_splitSec[2].get(), rec[0], rec[1], rec[2]);
  return line;
}
void splitSnapshot() { SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_FLUSH] %s", splitSnapshotText().c_str()); }

void maybeReport() {
  thread_local uint64_t t_n = 0;
  ++t_n;
  if ((t_n & 0x3FFFFu) == 0) report("periodic");
  if (g_split) {  // every call: the period bounds the interval, not a draw count (Astra round 2)
    const uint64_t now = GetTickCount64();
    uint64_t at = g_splitSnapAt.load(std::memory_order_relaxed);
    if (now - at >= kSplitSnapMs && g_splitSnapAt.compare_exchange_strong(at, now)) splitSnapshot();
  }
}

// P2-B: the segment transport's counters every 2048 closes (segmentless only, observe-only). The periodic line
// above comes every 2^18 draws and the exit line needs a clean exit, so a boot-time hang carried neither (segf).
// tools/graceful-quit: why = nullptr prints the periodic lines exactly as before ("[NATIVE_FLUSH] seg closed=",
// "[NATIVE_FLUSH] seg tile segs="); a why token goes right after the tag ("[NATIVE_FLUSH] terminal seg closed=").
void segReport(const char* why = nullptr) {
  const char* w = why ? why : "";
  const char* sp = why ? " " : "";
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_FLUSH] %s%sseg closed=%llu parts=%llu gapKB=%llu recApplied=%llu gapApplied=%llu idMiss=%llu "
              "mismatch=%llu handoff=%llu replayHit=%llu replayMiss=%llu expired=%llu guestMode=%llu coher=%llu "
              "pinned=%llu chained=%llu footStray=%llu feetLost=%llu coherDropOld=%llu capDrop=%llu litLenBad=%llu "
              "coherRange=%llu coherTop=%llu",
              w, sp, (unsigned long long)g_segClosed.get(), (unsigned long long)g_segParts.get(),
              (unsigned long long)(g_segGapBytes.get() >> 10), (unsigned long long)g_segRecApplied.get(),
              (unsigned long long)g_segGapApplied.get(), (unsigned long long)g_segIdMiss.get(),
              (unsigned long long)g_segMismatch.get(), (unsigned long long)g_segHandoff.get(),
              (unsigned long long)g_segReplayHit.get(), (unsigned long long)g_segReplayMiss.get(),
              (unsigned long long)g_segExpired.get(), (unsigned long long)g_segGuestMode.get(),
              (unsigned long long)g_segCoher.get(), (unsigned long long)g_segPinned.get(),
              (unsigned long long)g_segChained.get(), (unsigned long long)g_segFootStray.get(),
              (unsigned long long)g_segFeetLost.get(), (unsigned long long)g_segCoherDropOld.get(),
              (unsigned long long)g_segCapDrop.get(), (unsigned long long)g_litLenBad.get(),
              (unsigned long long)g_coherRange.get(), (unsigned long long)g_coherTop.get());
  if (g_segTile) {  // SR_NATIVE_SEG_TILE: the strict transport witness (p2.segmentless v2)
    sr::native::segtile::TileCounters t;
    sr::native::segtile::FirstCounters f;
    uint64_t water;
    {
      std::lock_guard<std::mutex> lk(g_tileMu);
      t = g_tile.counters();
      f = g_first.counters();
      water = g_tile.water();
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FLUSH] %s%sseg tile segs=%llu recs=%llu unsegmented=%llu pads=%llu gap=%llu overlap=%llu order=%llu "
                "| first segs=%llu recs=%llu dup=%llu reorder=%llu | water=%llu head=%llu | idreorder=%llu deferred=%llu "
                "| drains=%llu drainBracket=%llu drainMiss=%llu | forced=%llu forcedMiss=%llu forcedPostponed=%llu",
                w, sp, (unsigned long long)t.segs, (unsigned long long)t.recs, (unsigned long long)t.unsegmented,
                (unsigned long long)t.pads, (unsigned long long)t.gap, (unsigned long long)t.overlap,
                (unsigned long long)t.order, (unsigned long long)f.segs, (unsigned long long)f.recs,
                (unsigned long long)f.dup, (unsigned long long)f.reorder, (unsigned long long)water,
                (unsigned long long)g_head, (unsigned long long)f.idreorder, (unsigned long long)f.deferred,
                (unsigned long long)g_tileDrains.load(std::memory_order_relaxed),
                (unsigned long long)g_tileDrainBracket.load(std::memory_order_relaxed),
                (unsigned long long)g_tileDrainMiss.load(std::memory_order_relaxed),
                (unsigned long long)g_tileForced.load(std::memory_order_relaxed),
                (unsigned long long)g_tileForcedMiss.load(std::memory_order_relaxed),
                (unsigned long long)g_tileForcedPostponed.load(std::memory_order_relaxed));
  }
}

// boot.device_create: D3DDeviceResetState_8210E520's device writes (the asm, guest/ppc 2026-10-10) on a device
// image - the dry CHECK applies them to its copy, the leaf to the device. Plain stores and bit masks: idempotent.
void devResetFields(uint8_t* d) {
  put64(d + 11544, 0x00000000FFFFFFFFull);
  for (uint32_t i = 0; i < 26; i++) put32(d + 1024 + 24 * i, be32(d + 1024 + 24 * i) & ~3u);
  for (uint32_t i = 0; i < 18; i++) put32(d + 1648 + 8 * i, (be32(d + 1648 + 8 * i) & ~3u) | 1u);
  put32(d + 10476, 8u);
  put32(d + 10300, 0x20002000u);
  put32(d + 10500, 14u);
  put32(d + 10580, 0x000FF000u);
  put32(d + 10640, 14u);
  put32(d + 10584, 0x000FF100u);
  put32(d + 10316, 0x00FFFFFFu);
  put32(d + 10452, 4u);
  put32(d + 10560, 4u);
  put32(d + 10644, 16u);
  put32(d + 10696, 2u);
  put32(d + 10436, be32(d + 10436) | 0x00080000u);
  put32(d + 10440, be32(d + 10440) | 0x00010000u);
  for (uint32_t k = 0; k < 40; k += 8) put64(d + k, ~0ull);
  put32(d + 10788, 14u);
}
// The segment re-emit 820F0048's scissor words for D3DDeviceEmitScissorTileRects_820F0208(dev, x0, y0, x1, y1):
// the asm's sign-extended 15-bit halves of dev+10308 (x0 lo, y0 hi) and dev+10312 (x1 lo, y1 hi).
inline int32_t lo15(uint32_t v) { return int32_t(v << 17) >> 17; }
inline int32_t hi15(uint32_t v) { return int32_t(v << 1) >> 17; }

struct Args {
  uint32_t dev, prim, a, b, count;
};
Args args(const PPCContext& ctx, int kind) {
  if (kind == 0) return Args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
  if (kind == 2) return Args{ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, 0u, ctx.r5.u32};  // BeginVertices: a = stride bytes
  if (kind == 5) return Args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};  // tile rects: x0 y0 x1 y1
  if (kind == 6) return Args{ctx.r3.u32, ctx.r4.u32, 0u, 0u, 0u};  // SetPredication: prim = mask
  if (kind == 7) return Args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0u, ctx.r6.u32};  // EmitPm4Packet: prim = type, a = index, count
  if (kind == 8) return Args{ctx.r3.u32, 0u, 0u, 0u, 0u};                            // ResetState 8210E520: dev
  if (kind == 9) return Args{ctx.r3.u32, ctx.r4.u32, 0u, 0u, 0u};                    // segment re-emit 820F0048: prim = r4
  if (kind == 11) return Args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0u, 0u};           // 82101E08 (zpd): prim = query, a = which
  if (kind == 10) return Args{ctx.r3.u32, ctx.r4.u32, 0u, 0u, 0u};                   // mode state 82103238: prim = mode
  return Args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0u, ctx.r6.u32};
}

// Declines that need no guest state change. devH = the device's host bytes. split = SR_NATIVE_FLUSH_SPLIT's window for
// this draw (p1.flush_residue: kinds 0 / 1 over 0xFFFF indices split natively when the XDK's table entry is non-zero).
bool declined(const Args& a, uint8_t*& devH, uint32_t& ib0, uint32_t& ib6, int kind, bool split = false) {
  // The device does not move: its host span is checked once per thread and device.
  thread_local uint32_t t_dev = 0;
  thread_local uint8_t* t_devH = nullptr;
  if (a.dev != t_dev || !t_devH) {
    uint8_t* h = sr::kernel::guestVAToHost(a.dev);
    uint8_t* e = sr::kernel::guestVAToHost(a.dev + kDevSpan - 1u);
    t_devH = (h && e - h == ptrdiff_t(kDevSpan - 1u)) ? h : nullptr;
    t_dev = a.dev;
  }
  devH = t_devH;
  if (!devH) {
    g_why[kSpan].fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  // byte dev+10808 bit 0x02 (lbz in 820FC000): tile auto-predication - GpuSetTilePredicationMask
  // 8210D400 sets it when predication is switched off under predicated tiling, and the XDK flush then
  // writes the draw into a scratch segment between SET_BIN_MASK packets for the per-tile replay. Not
  // the Z-pass (BeginZPass sets bit 0x40; its replays re-run whole IBs). Never set in this game's
  // gameplay (0 declines): no screen tiling (tile census e3, 2026-10-08).
  if (devH[10808] & 0x02u) {
    g_why[kTileRec].fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (kind != 5 && a.count > 0xFFFFu) {  // kind 5: count = y1 (0x7FFFFFFF for the unbounded scissor)
    // p1.flush_residue: under SR_NATIVE_FLUSH_SPLIT the indexed / auto-indexed flushes split natively (drawPacket) -
    // unless the XDK's indices-per-prim entry is 0 (its divwu traps: the guest body does the same) or unmapped.
    uint32_t ipp = 0, overlap = 0;
    const bool splittable = (kind == 0 || kind == 1) && splitTable(a.prim, ipp, overlap) && ipp != 0;
    if (!(split && splittable)) {
      g_why[kCount].fetch_add(1, std::memory_order_relaxed);
      if (split) ((kind == 0 || kind == 1) ? g_bigDecl : g_bigDeclOther).fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  if (kind == 0) {
    const uint32_t ibVA = be32(devH + 12164);
    const uint8_t* ib = ibVA ? sr::kernel::guestVAToHost(ibVA) : nullptr;
    if (!ib) {
      g_why[kIb].fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    ib0 = be32(ib);
    ib6 = be32(ib + 24);
  }
  return false;
}

#if SR_ORACLE
// ---- check mode state (per thread) ----
struct CheckTls {
  bool armed = false;
  uint32_t nested = 0;  // brackets inside an armed bracket (820F0048 -> 820F0208): the outer one compares
  uint32_t dev = 0, c0 = 0, lim0 = 0, cEnd = 0;
  bool stopped = false;
  uint8_t copy[kDevSpan];
  Item items[Driver::kAllCap];
  uint32_t nItems = 0;
  uint64_t nativeTsc = 0, t0 = 0;
};
CheckTls& ckTls() {
  thread_local CheckTls* t = new CheckTls();
  return *t;
}
std::atomic<uint32_t> g_ckExamples{0};

// Device dwords other threads write while a draw runs (fc1, 5.7M full checks: 0.12% of them
// differed, only here). The XDK flush never stores to them (820FC000 / 820FBBF8 and the leaves it
// calls without a guest call store only dev+0..0x30, +11568 and the ring - recompiled PPC); the
// CP / graphics ISR advance them (fence and interrupt bookkeeping, +0x2A1C..+0x2BB0 class).
// fc2: also +10848 / +10852 / +10860..10868 / +11176 - the whole fence / interrupt block the kick
// machinery owns (+0x2A18..+0x2BB0, the draw ledger's kick fields; fc3 added +0x2A18) is excluded, plus +0x2C50.
// P1 rows 1+2: the dword at +0x2A38 (bytes 10808..10811: the Z-pass / record mode flags, the pending
// EVENT_WRITE flag and the bind's flag byte 0x2A3A bits 3 / 6 / 7) is written by the flush itself, not
// by the kick machinery, so the bind's device writes are compared (it was never one of the measured
// asynchronous offsets: those were +0x2A18, +0x2A60.., +0x2A94.., +0x2BA8).
// The dry CHECK's device diff leaves out the fields another thread moves while the guest body runs: the
// fence / pump block dev+0x2A18..0x2BB4 (less the mode byte 0x2A38), dev+11344, and (2026-10-09, p5a_chk:
// 768 DEVICE BADs, every one at +11544 / +11548 within one second of the loading window, bytes 0 len 0)
// the deferred CPU-write range dev+0x2D18 / 0x2D1C that the lock core swaps from its own thread (strand B
// found the same on its row-9 CHECK).
bool asyncField(uint32_t off) {
  return (off >= 0x2A18u && off < 0x2BB4u && off != 0x2A38u) || off == 11344u || off == 0x2D18u || off == 0x2D1Cu;
}

#endif

thread_local uint64_t t_curRec = 0;  // CP thread: the record being applied (draw ledger)

}  // namespace

bool on() { return g_on; }
bool checkOn() { return g_check; }
bool shaderConstRunsGate() { return shaderConstRunsOn(); }
uint64_t cpCurrentRecord() { return t_curRec; }

bool draw(PPCContext& ctx, uint8_t* base, int kind) {
  if (!g_on) return false;
#if defined(SR_DIFFTEST)
  if (t_fsGuestOnly) return false;  // the split suite's SDK arm (8261B050's own calls): the guest body
#endif
  // In-roll A/B (kernel/ab_toggle, SR_AB_TOGGLE=SR_NATIVE_FLUSH): the PM4 path in OFF windows.
  static const int s_ab = sr::abt::reg("SR_NATIVE_FLUSH");
  if (s_ab >= 0 && !sr::abt::on(s_ab)) return false;
  if (kind == 2) {  // rows 4+6+7: its own gate and toggle
    static const int s_abB = sr::abt::reg("SR_NATIVE_BEGINV");
    if (!g_beginVOn || (s_abB >= 0 && !sr::abt::on(s_abB))) return false;
  }
  const uint64_t t0 = g_timing ? __rdtsc() : 0;
  const Args a = args(ctx, kind);
  uint8_t* devH = nullptr;
  uint32_t ib0 = 0, ib6 = 0;
  const bool split = flushSplitWindow();  // p1.flush_residue: decided once for the whole draw
  if (declined(a, devH, ib0, ib6, kind, split)) return false;
  if (sr::gfx::livetrace::armed()) {
    g_why[kTrace].fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  static const bool s_other = sr::native::dev::on();  // fixed at startup
  if (s_other) {  // 1a owns the stream
    g_why[kDedup].fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  std::lock_guard<SpinLock> lk(g_mu);
  if (!g_arena) {
    g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_arena) {
      g_why[kNoSpace].fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  // Arena space: half the arena stays as history behind the CP (replayed segments re-read it).
  for (int spin = 0; g_head + kMaxDrawRec - g_tailSeen > kRecArena / 2 &&
                     g_head + kMaxDrawRec - (g_tailSeen = g_cpTail.load(std::memory_order_acquire)) > kRecArena / 2;
       spin++) {
    if (spin > 2000) {
      g_why[kNoSpace].fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (spin < 64) _mm_pause();
    else std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  thread_local Item t_span[Driver::kSpanCap];
  Driver dr;
  dr.ctx = &ctx;
  dr.base = base;
  dr.d = devH;
  dr.dev = a.dev;
  dr.items = t_span;
  dr.split = split;
  dr.c = be32(devH + 40);
  dr.spanStart = dr.c;
  if (sr::native::cdedup::active()) {
    // S2a. In-roll A/B (SR_AB_TOGGLE=SR_NATIVE_CONST_DEDUP): OFF windows still keep the mirror (every
    // group emitted and stored), so no state is left behind by the toggle.
    static const int s_abd = sr::abt::reg("SR_NATIVE_CONST_DEDUP");
    dr.ddWant = true;
    dr.ddChk = sr::native::cdedup::checkOn();
    dr.ddSkip = sr::native::cdedup::on() && (s_abd < 0 || sr::abt::on(s_abd));
    dr.ddSync();
  }
  if (kind == 2) {
    // BeginVertices: the device's stride byte dev+0x2FE0 = stride/4 for the bind, restored right after the
    // flush sections, as 820FB6E8 does. Bit 51 (re-patch the fetches) is set when the new stride differs
    // from the stride the bound VS was PATCHED with (dev+0x2D20, the copy the bind makes of dev+0x2FE0 -
    // `lbz r10, 0x2D20(r31); cmplw r9, r10` at 820FB704/820FB71C), not from the previous stream stride:
    // batch 3's dry CHECK caught the latter (897,670 BAD draws: a pointer IM_LOAD + INVALIDATE the XDK
    // did not emit; the execution CHECK failed 3 freezes of 12).
    const uint8_t oldStride = devH[12256], newStride = uint8_t((a.a >> 2) & 0xFFu);
    devH[12256] = newStride;
    if (newStride != devH[11552] && be32(devH + 12420)) put64(devH + 16, be64(devH + 16) | (1ull << 51));
    dr.flushState();
    devH[12256] = oldStride;
    uint32_t allocVA = 0, preDraw = 0;
    dr.beginVertices(a.prim, a.count, a.a, allocVA, preDraw);
    dr.closeSpan();
    dr.releaseScratch();
    if (allocVA) {
      const uint32_t dwords = (a.count * a.a) >> 2;
      put32(devH + 40, preDraw);  // the XDK leaves the cursor before the draw; EndVertices / UP publish +0x3314
      put64(devH + 16, be64(devH + 16) | 0x1000ull);
      put32(devH + 13076, dr.c);
      put32(devH + 13080, allocVA);
      put32(devH + 13088, dwords);
#if SR_ORACLE
      // mem.resources 2a (memres2, SR_MEM_RESOURCES=census): the inline vertices' allocation in the command buffer
      // [dev+0x34AC, dev+0x34B0) - made while a Z-pass list records (dev+0x3290: a pinned segment) or not
      if (sr::gfx::memres::census())
        sr::gfx::memres::noteUpAlloc(Driver::phys(allocVA), 4u * dwords, be32(devH + 0x3290u) != 0u,
                                     Driver::phys(be32(devH + 0x34ACu)), Driver::phys(be32(devH + 0x34B0u)));
#endif
    }
    ctx.r3.u64 = allocVA;  // the entry's return value: the allocation VA (0 = failure)
  } else {
    dr.flushState();
    dr.drawPacket(kind, a.prim, a.a, a.b, a.count, ib0, ib6);
    dr.closeSpan();
    dr.releaseScratch();
  }
  if (dr.ddWant) {
    if (dr.dd) sr::native::cdedup::noteEmitted(dr.c);
    sr::native::cdedup::countGroups(dr.ddSeen, dr.ddPred);
    g_ddSeen.fetch_add(dr.ddSeen);
    g_ddPred.fetch_add(dr.ddPred);
    g_ddSkipped.fetch_add(dr.ddSkipped);
  }
  g_why[kTaken].fetch_add(1, std::memory_order_relaxed);
  if (g_timing) {
    // Native work only: the guest calls it makes (shader bind, leaves, kicks) are the game's
    // own code either way.
    g_tGuestTsc.fetch_add(__rdtsc() - t0 - dr.guestTsc, std::memory_order_relaxed);
    g_tGuestN.fetch_add(1, std::memory_order_relaxed);
    g_tGuestCallsTsc.fetch_add(dr.guestTsc, std::memory_order_relaxed);
  }
  maybeReport();
  return true;
}

// P2-B item 5 (kRkSetConstMem): the CP applies the record by reading its values from the body the caller
// filled (as the PM4 path reads the ring); the renderer writes the XDK bytes - the header, the offset word
// and the body copied from memory (what is there at render time: the caller's values at the CP, the
// unfilled bytes during the dry CHECK and on the ring path, where the span is literal and the fill follows).
bool setConstMemApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkSetConstMem || n != 3u) return false;
  const uint32_t nv = (w[0] >> 16) & 0x3FFFu;
  if (!nv) return true;
  const uint8_t* b = sr::kernel::guestVAToHost(w[2]);
  const uint8_t* e = sr::kernel::guestVAToHost(w[2] + 4u * nv - 1u);
  if (!b || !e || e - b != ptrdiff_t(4u * nv) - 1) return false;
  eng.applySetConstant(w[1], b, nv);
  return true;
}
#if SR_ORACLE
void setConstMemRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  (void)kind;
  (void)n;
  if (bytes < 8u) return;
  put32(out, w[0]);
  put32(out + 4, w[1]);
  const uint32_t nb = bytes - 8u;
  if (!nb) return;
  const uint8_t* b = sr::kernel::guestVAToHost(w[2]);
  const uint8_t* e = sr::kernel::guestVAToHost(w[2] + nb - 1u);
  if (b && e && e - b == ptrdiff_t(nb) - 1) {
    std::memmove(out + 8, b, nb);
  } else {
    for (uint32_t k = 0; k < nb / 4u; k++) std::memcpy(out + 8 + 4u * k, &kNopBE, 4);
  }
}
#endif
// p2.transport step 1 (kRkCallback): the CP applies GpuEmitCpuCallbackPackets_820FDA80's 23 / 25 dwords in their order
// through the methods those packets reach - [type-0 0x5C8 WAIT_UNTIL = 0x20000], SCRATCH_REG4/5 = cb / arg and
// SCRATCH_REG0 = mask (writeReg's write-backs to pc+0x10 / +0x14 / +0), the predicated WAIT pc+0 == mask, the
// predicated INTERRUPT(mask) (applyInterrupt: the guest ISR 820FDBA0, source 1, per CPU bit - it calls cb(arg) and
// clears its bit), reg 0x45E = mask, the predicated WAIT pc+0 == 0, SCRATCH_REG4 = 0x0BADF00D. A predicated packet is
// skipped when (binSelect & binMask) == 0, as execute() skips it.
bool callbackApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkCallback || n != 5u) return false;
  const uint32_t cb = w[0], arg = w[1], mask = w[2], flags = w[3], pcAddr = w[4];
  const bool pred = (eng.binSelect & eng.binMask) != 0;
  uint8_t wb[20];
  auto wait = [&](uint32_t ref) {
    put32(wb, 0x13u);  // memory, equal
    put32(wb + 4, pcAddr);
    put32(wb + 8, ref);
    put32(wb + 12, 0xFFFFFFFFu);
    put32(wb + 16, 0x100u);
    eng.applyWaitRegMem(wb, 5u);
  };
  if ((flags & 0xC0FFFFFFu) == 0) eng.writeReg(0x5C8u, 0x20000u);
  eng.writeReg(0x57Cu, cb);
  eng.writeReg(0x57Du, arg);
  eng.writeReg(0x578u, mask);
  if (pred) {
    wait(mask);
    eng.applyInterrupt(mask);
  }
  eng.writeReg(0x45Eu, mask);
  if (pred) {
    wait(0u);
  } else {
    eng.predSkipped += 3;
    g_cbPredSkip.fetch_add(1, std::memory_order_relaxed);
  }
  eng.writeReg(0x57Cu, 0x0BADF00Du);
  g_cbApplied.fetch_add(1, std::memory_order_relaxed);
  return true;
}
// The XDK's bytes, dword for dword as 820FDA80 writes them (the CPPM4 split, the ring path, the leaf's CHECK).
#if SR_ORACLE
uint32_t callbackDwords(const uint32_t* w, uint32_t* v) {
  const uint32_t cb = w[0], arg = w[1], mask = w[2], flags = w[3], pcAddr = w[4];
  uint32_t k = 0;
  if ((flags & 0xC0FFFFFFu) == 0) {
    v[k++] = 0x000005C8u;
    v[k++] = 0x00020000u;
  }
  const uint32_t tail[23] = {0x0001057Cu, cb, arg, 0x00000578u, mask,
                             0xC0043C01u, 0x13u, pcAddr, mask, 0xFFFFFFFFu, 0x100u,
                             0xC0005401u, mask, 0x0000045Eu, mask,
                             0xC0043C01u, 0x13u, pcAddr, 0u, 0xFFFFFFFFu, 0x100u,
                             0x0000057Cu, 0x0BADF00Du};
  for (uint32_t i = 0; i < 23; i++) v[k++] = tail[i];
  return k;
}
#endif
bool presentGatePayload(uint32_t pcVA, uint32_t interval, uint32_t pacingBits, uint32_t* out) {
  const uint32_t blanks = interval <= 1u ? 1u : interval == 2u ? 2u : interval == 4u ? 3u : 0u;
  if (!blanks || !out || pcVA > UINT32_MAX - 4u) return false;
  auto physical = [](uint32_t va) {
    return ((((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu)) | 2u;
  };
  out[0] = (blanks << 16) | ((pacingBits >> 23) & 0x7Fu);
  out[1] = physical(pcVA);
  out[2] = physical(pcVA + 4u);
  return true;
}
#if SR_ORACLE
uint32_t presentGateDwords(const uint32_t* w, uint32_t* v) {
  const uint32_t prefix[8] = {0x00000579u, 1u, 0xC0043C00u, 0x13u, w[2], 1u, UINT32_MAX, 0x100u};
  std::memcpy(v, prefix, sizeof(prefix));
  const uint32_t callback[5] = {0x82111E38u, w[0], 4u, 0u, w[1]};
  const uint32_t n = 8u + callbackDwords(callback, v + 8u);
  const uint32_t tail[6] = {0xC0043C00u, 0x13u, w[2], 0u, UINT32_MAX, 0x100u};
  std::memcpy(v + n, tail, sizeof(tail));
  return n + 6u;
}
#endif
bool presentGateApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkPresentGate || !w || n != 3u) return false;
  uint8_t wait[20];
  put32(wait, 0x13u);
  put32(wait + 4u, w[2]);
  put32(wait + 8u, 1u);
  put32(wait + 12u, UINT32_MAX);
  put32(wait + 16u, 0x100u);
  eng.writeReg(0x579u, 1u);
  eng.applyWaitRegMem(wait, 5u);
  const uint32_t callback[5] = {0x82111E38u, w[0], 4u, 0u, w[1]};
  callbackApply(kRkCallback, callback, 5u, eng);
  put32(wait + 8u, 0u);
  eng.applyWaitRegMem(wait, 5u);
  g_gateApplied.fetch_add(1, std::memory_order_relaxed);
  return true;
}
#if SR_ORACLE
void presentGateRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkPresentGate || n != 3u || bytes != 156u) return;
  uint32_t values[39];
  presentGateDwords(w, values);
  for (uint32_t i = 0; i < 39u; i++) put32(out + 4u * i, values[i]);
}
#endif
void ensurePresentGateHandlers() {
  static const bool once = [] {
#if SR_ORACLE
    setOpaqueHandler(kRkPresentGate, presentGateApply, presentGateRender);
#else
    setOpaqueHandler(kRkPresentGate, presentGateApply);
#endif
    return true;
  }();
  (void)once;
}
bool swapBlockApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkSwap || !w || n != 10u || w[0] > 1u) return false;
  if (w[0])
    for (uint32_t i = 0; i < 6u; ++i) eng.writeReg(0x4800u + i, w[1u + i]);
  eng.applySwap(w[7], w[8], w[9]);
  g_swapApplied.fetch_add(1, std::memory_order_relaxed);
  return true;
}
#if SR_ORACLE
void swapBlockRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkSwap || !w || n != 10u || w[0] > 1u || !out || bytes != 256u) return;
  uint32_t i = 0;
  if (w[0]) {
    put32(out + 4u * i++, 0x00054800u);
    for (uint32_t j = 0; j < 6u; ++j) put32(out + 4u * i++, w[1u + j]);
  }
  put32(out + 4u * i++, 0xC0036400u);
  put32(out + 4u * i++, 0x53574150u);
  put32(out + 4u * i++, w[7]);
  put32(out + 4u * i++, w[8]);
  put32(out + 4u * i++, w[9]);
  for (; i < 64u; ++i) put32(out + 4u * i, 0x80000000u);
}
#endif
void ensureSwapBlockHandlers() {
  static const bool once = [] {
#if SR_ORACLE
    setOpaqueHandler(kRkSwap, swapBlockApply, swapBlockRender);
#else
    setOpaqueHandler(kRkSwap, swapBlockApply);
#endif
    return true;
  }();
  (void)once;
}
bool swapScalerResetApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkSwapScalerReset || !w || n != 1u || w[0]) return false;
  eng.writeReg(0x1838u, 0u);
  uint8_t wait[20];
  const uint32_t values[5] = {3u, 0x1973u, 0u, 1u, 256u};
  for (uint32_t i = 0; i < 5u; ++i) put32(wait + 4u * i, values[i]);
  eng.applyWaitRegMem(wait, 5u);
  eng.writeReg(0x1852u, 1u);
  return true;
}
#if SR_ORACLE
void swapScalerResetRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkSwapScalerReset || !w || n != 1u || w[0] || !out || bytes != 40u) return;
  const uint32_t values[10] = {0x1838u, 0u, 0xC0043C00u, 3u, 0x1973u, 0u, 1u, 256u, 0x1852u, 1u};
  for (uint32_t i = 0; i < 10u; ++i) put32(out + 4u * i, values[i]);
}
#endif
void ensureSwapScalerResetHandlers() {
  static std::once_flag registered;
  std::call_once(registered, [] {
#if SR_ORACLE
    setOpaqueHandler(kRkSwapScalerReset, swapScalerResetApply, swapScalerResetRender);
#else
    setOpaqueHandler(kRkSwapScalerReset, swapScalerResetApply);
#endif
  });
}
bool shaderConstSyncApply(uint32_t kind, const uint32_t* payload, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkShaderConstSync || !payload || n != 1u) return false;
  eng.writeReg(0x2007u, payload[0]);
  eng.writeReg(0x0A31u, 0x10000u);
  eng.writeReg(0x0A2Fu, 0u);
  eng.writeReg(0x0A30u, 0x1000u);
  // The SDK then waits for (COHER_STATUS_HOST & 0x80000000) == 0.
  // The fixed write above satisfies it; there is no hardware poll to emulate.
  return true;
}
#if SR_ORACLE
void shaderConstSyncRender(uint32_t kind, const uint32_t* payload, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkShaderConstSync || !payload || n != 1u || bytes != 52u) return;
  const uint32_t words[13] = {0x2007u, payload[0], 0x0A31u, 0x10000u,
      0x00010A2Fu, 0u, 0x1000u, 0xC0043C00u, 3u, 0x0A31u, 0u, 0x80000000u, 8u};
  for (uint32_t i = 0; i < 13u; ++i) put32(out + 4u * i, words[i]);
}
#endif
void ensureShaderConstSyncHandlers() {
  static const bool once = [] {
#if SR_ORACLE
    setOpaqueHandler(kRkShaderConstSync, shaderConstSyncApply, shaderConstSyncRender);
#else
    setOpaqueHandler(kRkShaderConstSync, shaderConstSyncApply);
#endif
    return true;
  }();
  (void)once;
}
#if SR_ORACLE
void callbackRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkCallback || n != 5u) return;
  uint32_t v[25];
  const uint32_t k = callbackDwords(w, v);
  for (uint32_t i = 0; i < k && 4u * i + 4u <= bytes; i++) put32(out + 4u * i, v[i]);
}
#endif
void ensureCallbackHandlers() {
  static const bool s_once = [] {
#if SR_ORACLE
    setOpaqueHandler(kRkCallback, callbackApply, callbackRender);
#else
    setOpaqueHandler(kRkCallback, callbackApply);
#endif
    return true;
  }();
  (void)s_once;
}

// p2.transport BRACKET_END (kRkBracket, the CP thread): the record that stands where the Z-pass kick's callback block
// stood. It runs first in its segment; the replay steps follow as the segment's own parts (cpSegment). Its handler only
// binds identity: the payload's generation is the segment's, applied once, in generation order.
bool bracketApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine&) {
  if (kind != kRkBracket || !w || n != 8u) return false;
  const SegParts* sp = t_segCur.sp;
  const uint64_t gen = uint64_t(w[0]) | (uint64_t(w[1]) << 32);
  if (!sp || !gen || sp->bracketGen != gen || sp->bracketSteps != w[2] || gen <= g_brLastConsumed) return false;
  g_brLastConsumed = gen;
  g_brConsumed.fetch_add(1, std::memory_order_relaxed);
  return true;
}
// Its PM4 form: the SDK's 23-dword callback block (the CPPM4 / literal renderer; the kick's CPU projection).
#if SR_ORACLE
void bracketRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkBracket || !w || n != 8u || !out || bytes != 92u) return;
  uint32_t v[25];
  if (callbackDwords(w + 3, v) != 23u) return;
  for (uint32_t i = 0; i < 23u; i++) put32(out + 4u * i, v[i]);
}
#endif
void ensureBracketHandlers() {
  static const bool s_once = [] {
#if SR_ORACLE
    setOpaqueHandler(kRkBracket, bracketApply, bracketRender);
#else
    setOpaqueHandler(kRkBracket, bracketApply);
#endif
    return true;
  }();
  (void)s_once;
}

void ensureSetConstMemHandlers() {
  static const bool s_once = [] {
#if SR_ORACLE
    setOpaqueHandler(kRkSetConstMem, setConstMemApply, setConstMemRender);
#else
    setOpaqueHandler(kRkSetConstMem, setConstMemApply);
#endif
    return true;
  }();
  (void)s_once;
}

// p2.transport RESIDUE (kRkCoherHost, the CP thread): the lock core's COHER block with the caller's own status - exactly
// what kRkInvalidate's applyCoherency does with the status as given: COHER_STATUS_HOST, COHER_SIZE_HOST, COHER_BASE_HOST
// (writeReg, as the block's two type-0 packets reach them), then the block's register-mode WAIT_REG_MEM {3, 0xA31, 0,
// 0x80000000, 8} through the CP's own handshake. Payload {status, size, base}.
bool coherHostApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkCoherHost || !w || n != 3u) return false;
  eng.writeReg(0x0A31u, w[0]);
  eng.writeReg(0x0A2Fu, w[1]);
  eng.writeReg(0x0A30u, w[2]);
  static const uint8_t kWait[20] = {0, 0, 0, 3, 0, 0, 0x0A, 0x31, 0, 0, 0, 0, 0x80, 0, 0, 0, 0, 0, 0, 8};
  eng.applyWaitRegMem(kWait, 5u);
  g_rsHostApplied.fetch_add(1, std::memory_order_relaxed);
  return true;
}
// Its PM4 form: the 11 dwords 820F3988 writes (the CPPM4 split and the literal span of SR_NATIVE_FLUSH_PM4).
#if SR_ORACLE
void coherHostRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkCoherHost || !w || n != 3u || !out || bytes != 44u) return;
  const uint32_t v[11] = {0x00000A31u, w[0], 0x00010A2Fu, w[1], w[2], 0xC0043C00u, 3u, 0x0A31u, 0u, 0x80000000u, 8u};
  for (uint32_t i = 0; i < 11u; i++) put32(out + 4u * i, v[i]);
}
#endif
void ensureResidueHandlers() {
  static const bool s_once = [] {
#if SR_ORACLE
    setOpaqueHandler(kRkCoherHost, coherHostApply, coherHostRender);
#else
    setOpaqueHandler(kRkCoherHost, coherHostApply);
#endif
    return true;
  }();
  (void)s_once;
}

#if SR_ORACLE
void checkBefore(PPCContext& ctx, uint8_t* base, int kind) {
  CheckTls& t = ckTls();
  if (t.armed) {  // a bracket inside an armed bracket (the segment re-emit's tile rects): the outer one compares
    t.nested++;
    return;
  }
  t.armed = false;
  if (!g_check) return;
  static const uint32_t s_every = [] {
    const char* e = std::getenv("SR_NATIVE_FLUSH_CHECK_EVERY");
    return e && *e ? std::max<uint32_t>(1u, uint32_t(std::strtoul(e, nullptr, 0))) : 1u;
  }();
  thread_local uint32_t t_n = 0;
  if (++t_n % s_every) return;
  const Args a = args(ctx, kind);
  uint8_t* devH = nullptr;
  uint32_t ib0 = 0, ib6 = 0;
  // The dedup leaf (skips or CHECK marks) would change the guest body's PM4: it is suspended on the
  // sampled draw (the plain XDK body), so the flush CHECK runs with the dedup on and with its CHECK.
  const bool split = flushSplitWindow();  // p1.flush_residue: the dry run chunks / splits as the record mode would
  if (declined(a, devH, ib0, ib6, kind, split)) {
    g_ckSkip.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::memcpy(t.copy, devH, kDevSpan);
  const uint64_t n0 = __rdtsc();
  Driver dr;
  dr.ctx = &ctx;
  dr.base = base;
  dr.all = t.items;
  dr.d = t.copy;
  dr.dev = a.dev;
  dr.split = split;
  dr.dry = true;
  dr.c = be32(t.copy + 40);
  dr.spanStart = dr.c;
  t.dev = a.dev;
  t.c0 = dr.c;
  t.lim0 = be32(t.copy + 44);
  if (kind == 2) {  // BeginVertices: the stride byte for the bind, as in draw(); the dry run stops at the allocator
    const uint8_t oldStride = t.copy[12256], newStride = uint8_t((a.a >> 2) & 0xFFu);
    t.copy[12256] = newStride;
    if (newStride != t.copy[11552] && be32(t.copy + 12420)) put64(t.copy + 16, be64(t.copy + 16) | (1ull << 51));
    const bool ok = dr.flushState();
    t.copy[12256] = oldStride;
    if (ok) {
      uint32_t allocVA = 0, preDraw = 0;
      dr.beginVertices(a.prim, a.count, a.a, allocVA, preDraw);
    }
  } else if (kind == 5) {  // the tile-rect leaf: its items alone (no flush state)
    uint32_t r3 = 0;
    dr.tileRects(int32_t(a.prim), int32_t(a.a), int32_t(a.b), int32_t(a.count), r3);
  } else if (kind == 6) {  // SetPredication: its device writes on the copy, then its packet
    uint32_t r3 = 0;
    dr.setPredication(a.prim, r3);
  } else if (kind == 7) {  // EmitPm4Packet: the reserve, then its item (the body compares as the memory holds it)
    ensureSetConstMemHandlers();
    uint32_t body = 0;
    dr.setConstMem(a.prim, a.a, a.count, body);
  } else if (kind == 8) {  // ResetState 8210E520: its device writes on the copy, then its packet tail
    devResetFields(t.copy);
    dr.devResetPackets();
  } else if (kind == 9) {  // the segment re-emit 820F0048: its packets (r4 == 0), row 5's tile rects, the dirty masks
    if (a.prim == 0) dr.segmentBeginPackets();
    const uint32_t x = be32(t.copy + 10308), y = be32(t.copy + 10312);
    uint32_t r3 = 0;
    dr.tileRects(lo15(x), hi15(x), lo15(y), hi15(y), r3);
    for (uint32_t k = 0; k < 40; k += 8) put64(t.copy + k, ~0ull);
  } else if (kind == 11) {  // p2.zpd's bake, 82101E08: its packets (the query object read from guest memory), the dirty bits
    uint32_t r3 = 0;
    dr.zpdSurface(a.prim, a.a, r3);
  } else if (kind == 10) {  // boot gaps: the mode state 82103238 - its device write on the copy, then its two runs
    put32(t.copy + 10788, (be32(t.copy + 10788) & ~0x3000u) | ((a.prim & 3u) << 12));
    dr.modeStatePackets();
  } else if (dr.flushState()) {
    dr.drawPacket(kind, a.prim, a.a, a.b, a.count, ib0, ib6);
  }
  if (!dr.stopped) put32(t.copy + 40, dr.c);
  t.nativeTsc = __rdtsc() - n0;
  t.stopped = dr.stopped;
  t.cEnd = dr.c;
  t.nItems = dr.nAll;
  t.armed = true;
  if (sr::native::cdedup::active()) sr::native::cdedup::suspend(true);
  t.t0 = __rdtsc();
}
#endif

#if SR_ORACLE
void checkAfter(PPCContext& ctx, uint8_t* base, int kind) {
  (void)ctx;
  (void)base;
  (void)kind;
  CheckTls& t = ckTls();
  if (t.nested) {  // the inner bracket's close (see checkBefore)
    t.nested--;
    return;
  }
  if (!t.armed) return;
  t.armed = false;
  if (sr::native::cdedup::active()) sr::native::cdedup::suspend(false);
  const uint64_t guestTsc = __rdtsc() - t.t0;
  g_ckN.fetch_add(1, std::memory_order_relaxed);
  maybeReport();
  uint8_t* devH = sr::kernel::guestVAToHost(t.dev);
  if (!devH) return;
  const uint32_t c1 = be32(devH + 40), lim1 = be32(devH + 44);
  if (lim1 != t.lim0 || c1 < t.c0) {  // the guest body switched segments (a kick inside it)
    g_ckKick.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The native rendering of what the dry run planned (all of it, or the prefix before the first
  // guest call / kick it could not make).
  const uint32_t nb = t.cEnd - t.c0;
  std::vector<uint8_t> nat(nb);
  uint32_t o = 0;
  for (uint32_t i = 0; i < t.nItems; i++) {
    renderPm4(t.items[i], nat.data() + o);
    o += t.items[i].bytes();
  }
  auto example = [&](const char* what, uint32_t at, uint32_t nv, uint32_t gv) {
    if (g_ckExamples.fetch_add(1, std::memory_order_relaxed) >= 24) return;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NATIVE_FLUSH_BAD] %s kind=%d dev=%08X c0=%08X native_len=%u guest_len=%u at +%u native=%08X guest=%08X "
                "items=%zu full=%d",
                what, kind, t.dev, t.c0, nb, c1 - t.c0, at, nv, gv, size_t(t.nItems), int(!t.stopped));
  };
  bool bad = false;
  if (!t.stopped && c1 != t.cEnd) {
    g_ckBadLen.fetch_add(1, std::memory_order_relaxed);
    example("LEN", 0, nb, c1 - t.c0);
    bad = true;
  }
  const uint32_t cmp = std::min<uint32_t>(nb, c1 - t.c0);
  if (cmp) {
    const uint8_t* g = sr::kernel::guestVAToHost(t.c0 + 4u);
    const uint8_t* ge = sr::kernel::guestVAToHost(t.c0 + 4u + cmp - 1u);
    if (g && ge - g == ptrdiff_t(cmp) - 1 && std::memcmp(g, nat.data(), cmp) != 0) {
      uint32_t at = 0;
      while (at + 4 <= cmp && std::memcmp(g + at, nat.data() + at, 4) == 0) at += 4;
      g_ckBadBytes.fetch_add(1, std::memory_order_relaxed);
      example("BYTES", at, be32(nat.data() + at), be32(g + at));
      bad = true;
    }
  }
  if (!t.stopped) {
    uint32_t nd = 0, first = 0;
    for (uint32_t k = 0; k < kDevSpan; k += 4)
      if (!asyncField(k) && std::memcmp(t.copy + k, devH + k, 4) != 0) {
        if (!nd) first = k;
        nd++;
      }
    if (nd) {
      g_ckBadDev.fetch_add(1, std::memory_order_relaxed);
      example("DEVICE", first, be32(t.copy + first), be32(devH + first));
      bad = true;
    }
    g_ckFull.fetch_add(1, std::memory_order_relaxed);
    if (kind >= 0 && kind < kCkKinds) g_ckKindFull[kind].fetch_add(1, std::memory_order_relaxed);
    if (!bad) {
      g_ckNativeTsc.fetch_add(t.nativeTsc, std::memory_order_relaxed);
      g_ckGuestTsc.fetch_add(guestTsc, std::memory_order_relaxed);
      g_ckTimed.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    g_ckPrefix.fetch_add(1, std::memory_order_relaxed);
  }
  if (bad && kind >= 0 && kind < kCkKinds) g_ckKindBad[kind].fetch_add(1, std::memory_order_relaxed);
}
#endif

bool cpApplyRecord(sr::gfx::CpEngine& eng, uint64_t pos, SegRecord* owned = nullptr);

#if SR_ORACLE  // C1 stage 1: the CPPM4 render-back and the NFLU marker executor need the PM4 walker (dev-only)
// Split 2 (SR_NATIVE_FLUSH_CPPM4=1, the CP thread): a record's items as the PM4 packets the XDK writes for them,
// appended to `out` for Pm4Engine::execute (type-0 runs, DRAW_INDX with 4 body dwords - the viz dword is 0, the PM4
// path never reads it). opaqueSeen = a kind with no PM4 form here (strand B's): the caller runs the record itself
// through nativeRecord instead. setConstPm4 = the kRkSetConstMem items rendered (P2-B item 5: the SET_CONSTANT header,
// the offset word and the body the caller filled, read now as the PM4 path reads the ring - setConstMemRender, the
// renderer the producer's literal span uses; without the pad NOPs, which are no-ops the record does not carry).
// The setconst_render fixture (tools/difftest/setconst_render_test.inc) calls it directly.
void cpRecordPm4(const uint32_t* w, const uint32_t* e, std::vector<uint8_t>& out,
                 sr::gfx::Pm4Engine::NativeRecordStats& st, bool& opaqueSeen, uint32_t& setConstPm4) {
  auto put = [&](uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    out.insert(out.end(), b, b + 4);
  };
  for (const uint32_t* p = w; p < e;) {
    const uint32_t tag = *p++;
    if (tag & 0x80000000u) {
      const uint32_t kind = (tag >> 24) & 0x7Fu, n = (tag >> 8) & 0xFFFFu, pred = (tag & 2u) ? 1u : 0u;
      const uint32_t words = recWords(kind, n);
      if (!words || words > size_t(e - p)) {
        st.bad++;
        break;
      }
      switch (kind) {
        case kRkDraw:
          put(0xC0032201u | pred);
          put(0u);
          put(p[0]);
          put(p[1]);
          put(p[2]);
          st.draws++;
          break;
        case kRkShaderPtr:
          put(0xC0012700u | pred);
          put(p[0]);
          put(p[1]);
          break;
        case kRkShaderImm:  // the XDK header: count = ucode dwords + 1 (type, size, ucode)
          put(0xC0002B00u | (((n + 1u) << 16) & 0x3FFF0000u) | pred);
          put(p[0]);
          put(p[1]);
          out.insert(out.end(), reinterpret_cast<const uint8_t*>(p + 2), reinterpret_cast<const uint8_t*>(p + 2) + 4u * n);
          break;
        case kRkConstLoad:
          put(0xC0022F00u | pred);
          put(p[0]);
          put(p[1]);
          put(p[2]);
          break;
        case kRkSetConst:  // count = values (offsetType + n values - 1)
          put(0xC0002D00u | ((n << 16) & 0x3FFF0000u) | pred);
          put(p[0]);
          out.insert(out.end(), reinterpret_cast<const uint8_t*>(p + 1), reinterpret_cast<const uint8_t*>(p + 1) + 4u * n);
          break;
        case kRkBinReg:
          put(0xC0006000u | ((n & 3u) << 8) | pred);
          put(p[0]);
          break;
        case kRkEvent:
          put(0xC0004600u | pred);
          put(p[0]);
          break;
        case kRkZpd:  // p2.zpd's bake: EVENT_WRITE_ZPD {initiator}
          put(0xC0005B00u | pred);
          put(p[0]);
          break;
        case kRkFence: {  // P2-B: the 10-dword fence tail
          const uint32_t v[10] = {0xC0003B00u, 0x7FFFu, 0xC0025800u, 3u, p[0], p[1], 0xC0025800u, 3u, p[2], p[3]};
          for (uint32_t k = 0; k < 10; k++) put(v[k]);
          break;
        }
        case kRkInvalidate: {  // P2-B: the 11-dword COHER block
          const uint32_t v[11] = {0x00000A31u, p[2] ? 0x01000000u : 0x03000000u, 0x00010A2Fu, p[1], p[0],
                                  0xC0043C00u, 3u, 0x0A31u, 0u, 0x80000000u, 8u};
          for (uint32_t k = 0; k < 11; k++) put(v[k]);
          break;
        }
        case kRkRegRmw:  // boot.device_create: REG_RMW {rmwInfo, andMask, orMask}
          put(0xC0022100u | pred);
          put(p[0]);
          put(p[1]);
          put(p[2]);
          break;
        case kRkEventShd:  // boot.device_create: EVENT_WRITE_SHD {initiator, addr|endian, value}
          put(0xC0025800u | pred);
          put(p[0]);
          put(p[1]);
          put(p[2]);
          break;
        case kRkPm4:  // P2-B: literal PM4
          out.insert(out.end(), reinterpret_cast<const uint8_t*>(p), reinterpret_cast<const uint8_t*>(p) + 4u * n);
          break;
        case kRkShaderConstSync: {
          if (n != 1u) {
            opaqueSeen = true;  // the normal handler rejects the invalid payload
            break;
          }
          const size_t begin = out.size();
          out.resize(begin + 52u);
          shaderConstSyncRender(kind, p, n, out.data() + begin, 52u);
          t_shaderSyncPm4++;  // the construns bake's cpPm4= (counted by the caller once the PM4 executed)
          break;
        }
        case kRkCoherHost: {  // p2.transport RESIDUE: the lock core's 11-dword COHER block, its own status
          if (n != 3u) {
            opaqueSeen = true;  // the normal handler rejects the invalid payload
            break;
          }
          const size_t begin = out.size();
          out.resize(begin + 44u);
          coherHostRender(kind, p, n, out.data() + begin, 44u);
          break;
        }
        case kRkSetConstMem: {  // P2-B item 5: {header, offset, bodyVA}; the header's count = the body's dwords
          if (n != 3u) {
            st.bad++;  // setConstMemApply's own refusal: a malformed payload
            break;
          }
          const uint32_t nb = 4u * ((p[0] >> 16) & 0x3FFFu);
          const uint8_t* b0 = nb ? sr::kernel::guestVAToHost(p[2]) : nullptr;
          const uint8_t* b1 = nb ? sr::kernel::guestVAToHost(p[2] + nb - 1u) : nullptr;
          if (nb && !(b0 && b1 && b1 - b0 == ptrdiff_t(nb) - 1)) {
            st.bad++;  // ...and its other one: a body not mapped in one piece (the renderer would write NOPs there)
            break;
          }
          const size_t begin = out.size();
          out.resize(begin + 8u + nb);
          setConstMemRender(kind, p, n, out.data() + begin, 8u + nb);
          setConstPm4++;
          break;
        }
        case kRkGammaRamp:
        case kRkScaler: {  // boot gaps: the display emitters' SDK dwords through their own renderers (the CPPM4 split)
          const uint32_t bytes = displayRecordBytes(kind, p, n);
          const OpaqueRenderer render = opaqueRenderer(kind);
          if (!bytes || !render) {
            opaqueSeen = true;  // the normal handler rejects the invalid payload
            break;
          }
          const size_t begin = out.size();
          out.resize(begin + bytes);
          render(kind, p, n, out.data() + begin, bytes);
          break;
        }
        default:
          if (recOpaque(kind)) opaqueSeen = true;  // no PM4 form: the record runs through nativeRecord
          break;
      }
      if (kind != kRkDraw) st.packets++;
      p += words;
      continue;
    }
    const uint32_t count = (tag >> 16) & 0x3FFFu, reg = tag & 0x7FFFu;
    if (count > size_t(e - p)) {
      ++st.bad;
      break;
    }
    if (count) put(((count - 1u) << 16) | reg);
    const uint8_t* be = reinterpret_cast<const uint8_t*>(p);
    out.insert(out.end(), be, be + 4u * count);
    p += count;
    st.runs++;
  }
}

void cpExecute(sr::gfx::Pm4Engine& eng, const uint8_t* body, uint32_t cnt) {
  if (!g_on) return;  // markers exist only in record mode
  if (!body || cnt < 3u) {
    eng.nativeRecordFailed = true;
    g_cpBad.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  cpApplyRecord(eng, uint64_t(be32(body + 4)) | (uint64_t(be32(body + 8)) << 32));
}
#endif  // SR_ORACLE (CPPM4's render-back, the NFLU marker executor)

// Markers still target the first-consumer arena. Segment parts use bytes owned
// at emission, including their deferred replays after the arena has wrapped.
bool cpApplyRecord(sr::gfx::CpEngine& eng, uint64_t pos, SegRecord* owned) {
  if (eng.nativeRecordFailed) return false;
  const uint64_t t0 = g_timing ? __rdtsc() : 0;
  if ((!owned && (!g_arena || pos % kRecArena > kRecArena - sizeof(RecHdr))) ||
      (owned && owned->size() * 4u < sizeof(RecHdr))) {
    g_cpBad.fetch_add(1, std::memory_order_relaxed);
    eng.nativeRecordFailed = true;
    return false;
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint8_t* r = owned ? owned->data() : g_arena + (pos % kRecArena);
  RecHdr h;
  std::memcpy(&h, r, sizeof(h));
  if (h.magic != kRecMagic || h.pos != pos || h.bytes < sizeof(RecHdr) || h.bytes > kMaxDrawRec ||
      (h.bytes & 3u) || pos > UINT64_MAX - h.bytes ||
      (owned ? h.bytes > owned->size() * 4u : pos % kRecArena + h.bytes > kRecArena)) {
    const uint64_t n = g_cpBad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_BAD] CP record at pos %llu overwritten or torn (magic %08X pos %llu)",
                  (unsigned long long)pos, h.magic, (unsigned long long)h.pos);
    eng.nativeRecordFailed = true;
    return false;
  }
  const uint32_t* w = reinterpret_cast<const uint32_t*>(r + sizeof(RecHdr));
  const uint32_t* e = reinterpret_cast<const uint32_t*>(r + h.bytes);
#if SR_ORACLE
  if (owned) segarena::verify(*owned, "the CP's apply");  // I7: the record is the bytes captured
#endif
  sr::gfx::CpEngine::NativeRecordStats st;
  bool complete = true;
  t_curRec = pos;
#if SR_ORACLE  // C1 stage 1: CPPM4 is a development oracle over the PM4 walker
  if (g_cpPm4) {
    // Split 2: the record as PM4 packets through execute() (cpRecordPm4).
    thread_local std::vector<uint8_t> t_pm4;
    t_pm4.clear();
    bool opaqueSeen = false;
    uint32_t setConstPm4 = 0;
    t_shaderSyncPm4 = 0;
    cpRecordPm4(w, e, t_pm4, st, opaqueSeen, setConstPm4);
    if (st.bad) {
      complete = false;
    } else if (opaqueSeen) {  // strand B's record: no PM4 form, the record itself runs (its own entry's span)
      st = sr::gfx::CpEngine::NativeRecordStats{};
      complete = eng.nativeRecord(w, e, st, g_timing);
    } else {
      eng.execute(t_pm4.data(), t_pm4.size(), 1);
      complete = !eng.nativeRecordFailed;
      if (complete && setConstPm4) g_cpSetConstPm4.fetch_add(setConstPm4, std::memory_order_relaxed);
      if (complete && t_shaderSyncPm4) g_cpShaderSyncPm4.fetch_add(t_shaderSyncPm4, std::memory_order_relaxed);
    }
  } else
#endif
  {
    complete = eng.nativeRecord(w, e, st, g_timing);
  }
  t_curRec = 0;
  if (!complete || st.bad || eng.nativeRecordFailed) {
    eng.nativeRecordFailed = true;
    const uint64_t n = g_cpBad.fetch_add(std::max<uint64_t>(st.bad, 1u), std::memory_order_relaxed) + 1;
    if (n <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_BAD] rejected record at pos %llu; no completion or packet fallback",
                  (unsigned long long)pos);
    return false;
  }
  // Publish completion before advancing cpTail. Do not acquire g_mu here: a
  // producer holding it may be waiting for this consumer to free queue space.
  // Owned completion never modifies a reusable arena header. The marker path
  // retains its first-consumer backpressure and execution-integrity check.
  if (owned) {
    owned->completed.store(true, std::memory_order_release);
  } else {
    RecHdr h2;
    uint8_t* slot = g_arena + (pos % kRecArena);
    std::memcpy(&h2, slot, sizeof(h2));
    const bool same = h2.magic == h.magic && h2.pos == h.pos && h2.bytes == h.bytes;
    if (same) __atomic_store_n(&reinterpret_cast<RecHdr*>(slot)->exec, 1u, __ATOMIC_RELEASE);
    if (!same) {
      const uint64_t n = g_cpTorn.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_BAD] CP record at pos %llu was overwritten DURING its apply (now magic %08X pos %llu)",
                    (unsigned long long)pos, h2.magic, (unsigned long long)h2.pos);
      eng.nativeRecordFailed = true;
      return false;
    }
    if (g_head > pos && g_head - pos > kRecArena - 2ull * kMaxDrawRec)
      g_cpNearReclaim.fetch_add(1, std::memory_order_relaxed);
  }
  const uint64_t runs = st.runs, draws = st.draws, drawTsc = st.drawTsc;
  g_cpRecs.fetch_add(1, std::memory_order_relaxed);
  g_cpRuns.fetch_add(runs, std::memory_order_relaxed);
  g_cpDraws.fetch_add(draws, std::memory_order_relaxed);
  g_cpPackets.fetch_add(st.packets, std::memory_order_relaxed);
  // Newest record executed (replays re-execute older ones: max).
  const uint64_t end = pos + h.bytes;
  if (end > g_cpTail.load(std::memory_order_relaxed)) g_cpTail.store(end, std::memory_order_release);
  sr::native::meter::add(sr::native::meter::kNativeRec);
  if (g_timing) {
    // The record walk + register runs only (the draw itself, emitDraw, is the same work on the
    // PM4 path): the number to set against the PM4 parse + register writes it replaces.
    g_tCpTsc.fetch_add(__rdtsc() - t0 - drawTsc, std::memory_order_relaxed);
    g_tCpN.fetch_add(draws, std::memory_order_relaxed);
  }
  return true;
}

// ---- strand B's side of the contract (native_flush.hpp): opaque record kinds and the Entry ----
namespace {
OpaqueHandler g_opaque[kRkKinds] = {};
#if SR_ORACLE
OpaqueRenderer g_opaqueRender[kRkKinds] = {};
#endif
}

#if SR_ORACLE
void setOpaqueHandler(uint32_t kind, OpaqueHandler fn, OpaqueRenderer render) {
  if (!recOpaque(kind)) return;
  g_opaque[kind] = fn;
  g_opaqueRender[kind] = render;
}
OpaqueRenderer opaqueRenderer(uint32_t kind) { return recOpaque(kind) ? g_opaqueRender[kind] : nullptr; }
#else
void setOpaqueHandler(uint32_t kind, OpaqueHandler fn) {
  if (recOpaque(kind)) g_opaque[kind] = fn;
}
#endif
OpaqueHandler opaqueHandler(uint32_t kind) { return recOpaque(kind) ? g_opaque[kind] : nullptr; }

struct Entry::ClosedSpan { SegFoot foot; };

struct Entry::Impl {
  Driver dr;
  bool opened = false, locked = false;
  bool atMode = false;     // openAt(): a leaf's cursor - close() restores dev+40 to atCursor0
  uint32_t atCursor0 = 0;
  std::vector<uint8_t> payload;  // the items' bytes, copied at opaque() / run() (the span writer reads them)
  Item span[Driver::kSpanCap];
  DWORD takerTid = 0;  // diagnostic: the thread that took it (a give on another thread is counted, never refused)
  // The payload vector may grow until the span is written: resolve the deferred src pointers right
  // before every closeSpan (close(), kick(), alloc(), guest()).
  void fixup() {
    for (uint32_t i = 0; i < dr.nItems; i++)
      if (dr.items[i].deferSrc) dr.items[i].src = payload.data() + dr.items[i].lit[0];
  }
  // step 6 I6 (SR_ENTRY_POOL, default ON; =0 = a value-initialised `new Impl()` + delete per Entry, the A/B - what every
  // Entry got before I6): a BOUNDED per-thread free list (render/gate3/FREEZE_I6_I7_20261022.txt + addendum 1). take()
  // pops an Impl and resets it to EXACTLY a freshly value-initialised Impl's state (reset(): each Item writer sets only its
  // kind's fields, so the span is cleared too - the ~17 KB memset `new Impl()` did stays, the allocation, the backend-heap
  // malloc / free and the heap lock go); give() pushes onto the CALLING thread's list while it holds < kCap, else deletes.
  // No nesting depth and no LIFO assumption: a give on another thread or out of order lands on that thread's list (or is
  // deleted). The list's thread_local destructor frees it at thread exit. (The pool this replaces - strand B, job52 - never
  // worked: Entry declared its `pooled_` flag after impl_, so the flag's default initialiser reset what take() had set and
  // every ~Entry deleted the Impl without returning its slot; after 4 Entries per thread every Entry took the heap.)
  static constexpr int kCap = 4;
  static constexpr size_t kPayloadKeep = 16u * 1024u;  // a larger payload's storage is released on give
  void reset();
#if SR_ORACLE
  void poison();               // every field off its default, the span and the payload's storage 0xA5
  bool resetComplete() const;  // the state a value-initialised Impl has (every field; the payload's size)
#endif
  struct FreeList {
    Impl* slots[kCap] = {};
    int n = 0;
    bool counted = false;  // this thread's list has held an Impl (the threads counter)
    ~FreeList();
  };
  static thread_local FreeList t_list;  // the calling thread's list
  static Impl* take();
  static void give(Impl* im);
  // step 6 N4 (NEWPAIR_CHECKS section 4; ASan builds only - without ASan both are empty): a parked Impl is POISONED, its
  // bytes and its payload's storage (the capacity the list keeps), so a use of it after its give - on any thread - stops
  // with an ASan use-after-poison report. give() parks it last (after poison()); take() and the thread-exit delete
  // unpark it first (the Impl's bytes before the payload's: the vector's pointers live inside the Impl).
  void park() {
#if SR_EP_ASAN
    if (payload.capacity()) ASAN_POISON_MEMORY_REGION(payload.data(), payload.capacity());
    ASAN_POISON_MEMORY_REGION(this, sizeof(Impl));
#endif
  }
  void unpark() {
#if SR_EP_ASAN
    ASAN_UNPOISON_MEMORY_REGION(this, sizeof(Impl));
    if (payload.capacity()) ASAN_UNPOISON_MEMORY_REGION(payload.data(), payload.capacity());
#endif
  }
};
static_assert(std::is_trivially_copyable_v<Item>, "Entry::Impl::reset() clears the span with memset");

namespace {
// step 6 I6's counters (relaxed: diagnostics; [ENTRY_POOL] milestone lines, so a player-profile tracer roll prints them)
std::atomic<uint64_t> g_epTakes{0}, g_epHits{0}, g_epMisses{0}, g_epPushes{0}, g_epOverflow{0}, g_epCross{0},
    g_epPayloadRel{0}, g_epExitFrees{0}, g_epThreads{0}, g_epResetBad{0};
thread_local bool t_entryListGone = false;  // trivially destructible: still readable after the list's destructor ran
#if SR_ORACLE
std::atomic<int> g_epForce{-1};    // tests: -1 = the env gate, 0 = off, 1 = on
std::atomic<int> g_epSkipTest{-2};  // tests: -2 = the env seam, -1 = none, 0..6 = the reset statement a mutant skips
int epSkip() {
  const int t = g_epSkipTest.load(std::memory_order_relaxed);
  if (t != -2) return t;
  // SR_ENTRY_POOL_TEST_SKIP=<0..6> (SR_ORACLE builds only): the mutant - reset() skips that statement (0 dr, 1 opened,
  // 2 locked, 3 atMode, 4 atCursor0, 5 payload, 6 span); the oracles must catch every one
  static const int env = [] {
    const char* e = std::getenv("SR_ENTRY_POOL_TEST_SKIP");
    const int v = (e && *e) ? std::atoi(e) : -1;
    if (v >= 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[ENTRY_POOL] MUTANT: reset() skips statement %d (SR_ENTRY_POOL_TEST_SKIP) - "
                  "a test roll, never a measurement", v);
    return v;
  }();
  return env;
}
#endif
}  // namespace

thread_local Entry::Impl::FreeList Entry::Impl::t_list;

bool entryPoolOn() {
#if SR_ORACLE
  const int f = g_epForce.load(std::memory_order_relaxed);
  if (f >= 0) return f != 0;
#endif
  static const bool on = [] {
    const char* e = std::getenv("SR_ENTRY_POOL");
    return !e || e[0] != '0';
  }();
  return on;
}

EntryPoolCounters entryPoolCounters() {
  EntryPoolCounters c;
  c.takes = g_epTakes.load(std::memory_order_relaxed);
  c.hits = g_epHits.load(std::memory_order_relaxed);
  c.misses = g_epMisses.load(std::memory_order_relaxed);
  c.pushes = g_epPushes.load(std::memory_order_relaxed);
  c.overflowDeletes = g_epOverflow.load(std::memory_order_relaxed);
  c.crossThreadGives = g_epCross.load(std::memory_order_relaxed);
  c.payloadReleases = g_epPayloadRel.load(std::memory_order_relaxed);
  c.exitFrees = g_epExitFrees.load(std::memory_order_relaxed);
  c.threads = g_epThreads.load(std::memory_order_relaxed);
  c.resetBad = g_epResetBad.load(std::memory_order_relaxed);
  return c;
}

#if SR_ORACLE
namespace {
// The counters at process exit (the difftest suites, a graceful game exit): whether a run took, reused and reset Impls at
// all - the periodic [ENTRY_POOL] line prints only at takes 1 / 1000 / every 1 M. stdout, not the log: the log may be torn
// down by now; the counters are constant-initialised atomics, readable during static destruction.
struct EntryPoolExitLine {
  ~EntryPoolExitLine() {
    const EntryPoolCounters c = entryPoolCounters();
    if (!c.takes) return;
    std::printf("[ENTRY_POOL] final takes=%llu hits=%llu misses=%llu pushes=%llu overflow=%llu cross=%llu payload_rel=%llu "
                "exit_frees=%llu threads=%llu reset_bad=%llu\n",
                (unsigned long long)c.takes, (unsigned long long)c.hits, (unsigned long long)c.misses,
                (unsigned long long)c.pushes, (unsigned long long)c.overflowDeletes, (unsigned long long)c.crossThreadGives,
                (unsigned long long)c.payloadReleases, (unsigned long long)c.exitFrees, (unsigned long long)c.threads,
                (unsigned long long)c.resetBad);
    std::fflush(stdout);
  }
} g_epExitLine;
}  // namespace
#endif

void Entry::Impl::reset() {
#if SR_ORACLE
  const int skip = epSkip();
#else
  constexpr int skip = -1;
#endif
  if (skip != 0) dr = Driver{};
  if (skip != 1) opened = false;
  if (skip != 2) locked = false;
  if (skip != 3) atMode = false;
  if (skip != 4) atCursor0 = 0;
  if (skip != 5) payload.clear();  // capacity kept (give() released it above kPayloadKeep)
  if (skip != 6) std::memset(span, 0, sizeof span);
}

#if SR_ORACLE
void Entry::Impl::poison() {
  dr.c = dr.spanStart = 0xA5A5A5A4u;
  dr.nItems = 7u;
  dr.nAll = 7u;
  dr.recPos = ~0ull;
  dr.guestTsc = ~0ull;
  dr.dry = dr.stopped = dr.split = true;
  dr.spanSkip = dr.opaque = dr.forceLiteral = true;
  dr.ddWant = dr.dd = dr.ddSkip = dr.ddChk = true;
  dr.ddSeen = dr.ddPred = dr.ddSkipped = 0xA5u;
  dr.guestT0 = ~0ull;
  dr.scratchVA = dr.savedR1 = 0xA5A5A5A4u;
  opened = locked = atMode = true;
  atCursor0 = 0xA5A5A5A4u;
  if (payload.capacity()) {  // fill the storage it keeps (no allocation: an empty-capacity payload stays empty)
    payload.resize(payload.capacity());
    std::memset(payload.data(), 0xA5, payload.size());
  }
  std::memset(span, 0xA5, sizeof span);
}

bool Entry::Impl::resetComplete() const {
  static const Item kZero{};  // static storage: zero-initialised, padding included
  if (opened || locked || atMode || atCursor0 != 0 || !payload.empty()) return false;
  if (dr.ctx || dr.base || dr.d || dr.dev || dr.dry || dr.stopped || dr.c || dr.spanStart || dr.items || dr.nItems ||
      dr.all || dr.nAll || dr.guestTsc || dr.split || dr.recPos || dr.detachedFoot || dr.ddWant || dr.dd || dr.ddSkip ||
      dr.ddChk || dr.ddSeen || dr.ddPred || dr.ddSkipped || dr.spanSkip || dr.opaque || dr.forceLiteral || dr.guestT0 ||
      dr.scratchVA || dr.savedR1)
    return false;
  for (const Item& it : span)
    if (std::memcmp(&it, &kZero, sizeof(Item)) != 0) return false;
  return true;
}
#endif

Entry::Impl::FreeList::~FreeList() {
  t_entryListGone = true;  // an Entry destroyed later on this thread deletes its Impl
  for (int i = 0; i < n; i++) {
    slots[i]->unpark();  // N4 (ASan builds): ~Impl runs over the parked bytes
    delete slots[i];
    slots[i] = nullptr;
    g_epExitFrees.fetch_add(1, std::memory_order_relaxed);
  }
  n = 0;
}

Entry::Impl* Entry::Impl::take() {
  const uint64_t n = g_epTakes.fetch_add(1, std::memory_order_relaxed) + 1;
  Impl* im = nullptr;
  if (entryPoolOn() && !t_entryListGone) {
    FreeList& f = t_list;
    if (f.n > 0) {
      im = f.slots[--f.n];
      f.slots[f.n] = nullptr;
      im->unpark();  // N4 (ASan builds): addressable again before reset() writes it
      im->reset();
      g_epHits.fetch_add(1, std::memory_order_relaxed);
#if SR_ORACLE
      // the reset's completeness against a value-initialised Impl: every reuse early on, then every 64th
      if ((n <= 4096 || (n & 63u) == 0) && !im->resetComplete()) {
        const uint64_t bad = g_epResetBad.fetch_add(1, std::memory_order_relaxed) + 1;
        if (bad <= 8)
          SR_LOG_BOOT(sr::LogSeverity::Error, "[ENTRY_POOL] RESET INCOMPLETE #%llu: a reused Entry::Impl differs from a "
                      "value-initialised one (take #%llu, tid=%lu)", (unsigned long long)bad, (unsigned long long)n,
                      (unsigned long)GetCurrentThreadId());
      }
#endif
    }
  }
  if (!im) {
    g_epMisses.fetch_add(1, std::memory_order_relaxed);
    im = new Impl();  // value-initialised (the span zero-filled), as every Entry got before I6
  }
  im->takerTid = GetCurrentThreadId();
  if (n == 1 || n == 1000 || n % 1000000u == 0) {
    const EntryPoolCounters c = entryPoolCounters();
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[ENTRY_POOL] %s takes=%llu hits=%llu misses=%llu pushes=%llu overflow=%llu cross=%llu payload_rel=%llu "
                "exit_frees=%llu threads=%llu reset_bad=%llu (SR_ENTRY_POOL; cap %d / thread)",
                entryPoolOn() ? "on" : "OFF", (unsigned long long)c.takes, (unsigned long long)c.hits,
                (unsigned long long)c.misses, (unsigned long long)c.pushes, (unsigned long long)c.overflowDeletes,
                (unsigned long long)c.crossThreadGives, (unsigned long long)c.payloadReleases,
                (unsigned long long)c.exitFrees, (unsigned long long)c.threads, (unsigned long long)c.resetBad, kCap);
  }
  return im;
}

void Entry::Impl::give(Impl* im) {
  if (entryPoolOn() && !t_entryListGone) {
    FreeList& f = t_list;
    if (f.n < kCap) {
      if (im->takerTid != GetCurrentThreadId()) g_epCross.fetch_add(1, std::memory_order_relaxed);
      if (im->payload.capacity() > kPayloadKeep) {
        std::vector<uint8_t>().swap(im->payload);
        g_epPayloadRel.fetch_add(1, std::memory_order_relaxed);
      }
#if SR_ORACLE
      im->poison();  // the next take()'s reset must erase all of it
#endif
      if (!f.counted) {
        f.counted = true;
        g_epThreads.fetch_add(1, std::memory_order_relaxed);
      }
      im->park();  // N4 (ASan builds): poisoned while it sits on the list
      f.slots[f.n++] = im;
      g_epPushes.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    g_epOverflow.fetch_add(1, std::memory_order_relaxed);
  }
  delete im;
}

Entry::Entry(PPCContext& ctx, uint8_t* base, uint32_t dev) : impl_(Impl::take()) {
  impl_->dr.ctx = &ctx;
  impl_->dr.base = base;
  impl_->dr.dev = dev;
  impl_->dr.items = impl_->span;
}

Entry::~Entry() {
  if (impl_) {
    if (impl_->opened) abandon();
    Impl::give(impl_);
    impl_ = nullptr;
  }
}

#if SR_ORACLE
// tools/difftest/entry_pool_test.cpp's seams (SR_ORACLE only): Entry's friend, so they can reach the private Impl.
struct EntryPoolTest {
  static int cached() { return t_entryListGone ? -1 : Entry::Impl::t_list.n; }
  // one Impl through use -> give (poisoned) -> take with reset statement `skip` skipped; `complete` = the state check
  static bool resetCheck(int skip, bool& complete) {
    using Impl = Entry::Impl;
    const int prevSkip = g_epSkipTest.load(std::memory_order_relaxed);
    g_epSkipTest.store(-1, std::memory_order_relaxed);
    Impl* a = Impl::take();
    a->payload.assign(64, 0x11);  // a used payload: give() keeps its storage, poison() fills it
    Impl::give(a);
    g_epSkipTest.store(skip, std::memory_order_relaxed);
    Impl* b = Impl::take();
    g_epSkipTest.store(prevSkip, std::memory_order_relaxed);
    const bool reused = (a == b);
    complete = b->resetComplete();
    b->reset();  // a clean Impl back on the list, whatever the mutant left
    Impl::give(b);
    return reused;
  }
  // an Impl taken here and given on another thread lands on the OTHER thread's list (counted); this list is unchanged
  static bool crossThread(int& otherCached, uint64_t& crossDelta) {
    using Impl = Entry::Impl;
    Impl* im = Impl::take();
    const int mine = cached();
    const uint64_t c0 = g_epCross.load(std::memory_order_relaxed);
    std::thread th([&] {
      Impl::give(im);
      otherCached = cached();
    });
    th.join();  // the other thread's list is destroyed at its exit: its Impl freed (exitFrees)
    crossDelta = g_epCross.load(std::memory_order_relaxed) - c0;
    return cached() == mine;
  }
  // a thread nests `nested` device-less Entries (the constructor only stores) and exits: the list's destructor frees
  // what it cached - returns exitFrees' delta
  static uint64_t threadExit(int nested) {
    const uint64_t f0 = g_epExitFrees.load(std::memory_order_relaxed);
    std::thread th([nested] {
      PPCContext c{};
      std::vector<std::unique_ptr<Entry>> es;
      for (int i = 0; i < nested; i++) es.push_back(std::make_unique<Entry>(c, nullptr, 0u));
      while (!es.empty()) es.pop_back();  // innermost first
    });
    th.join();
    return g_epExitFrees.load(std::memory_order_relaxed) - f0;
  }
  // step 6 N4's negative controls: an Impl taken, used and given - parked on a free list, poisoned in an ASan build -
  // then TOUCHED: the use after release N4 must catch. `cross`: given on ANOTHER thread, which stays alive (its list
  // holding the Impl) while this thread touches it (21(8)'s cross-thread frees). Under ASan the touch stops the process
  // with a use-after-poison report; without ASan it reads the parked, still-allocated Impl and returns its low byte.
  // -1 = the Impl was not parked (the pool off, the list full or gone): nothing is touched.
  static int touchAfterGive(bool cross) {
    using Impl = Entry::Impl;
    Impl* im = Impl::take();
    im->payload.assign(64, 0x11);  // a used payload: its storage is parked (and poisoned) with the Impl
    std::atomic<int> stage{0};
    bool parked = false;
    std::thread th;
    if (cross) {
      th = std::thread([&] {
        const int before = cached();
        Impl::give(im);
        parked = before >= 0 && cached() == before + 1;
        stage.store(1, std::memory_order_release);
        while (stage.load(std::memory_order_acquire) != 2) std::this_thread::yield();  // alive: its list holds `im`
      });
      while (stage.load(std::memory_order_acquire) != 1) std::this_thread::yield();
    } else {
      const int before = cached();
      Impl::give(im);
      parked = before >= 0 && cached() == before + 1;
    }
    int v = -1;
    if (parked) {
      const volatile uint32_t* p = &im->atCursor0;
      v = int(*p & 0xFFu);  // THE TOUCH: a read of a released (parked) Impl
    }
    if (cross) {
      stage.store(2, std::memory_order_release);
      th.join();
    }
    return v;
  }
};
void entryPoolForceForTest(int v) { g_epForce.store(v, std::memory_order_relaxed); }
void entryPoolSkipResetForTest(int field) { g_epSkipTest.store(field, std::memory_order_relaxed); }
int entryPoolCachedForTest() { return EntryPoolTest::cached(); }
bool entryPoolResetCheckForTest(int skip, bool& complete) { return EntryPoolTest::resetCheck(skip, complete); }
bool entryPoolCrossThreadForTest(int& otherCached, uint64_t& crossDelta) {
  return EntryPoolTest::crossThread(otherCached, crossDelta);
}
uint64_t entryPoolThreadExitForTest(int nested) { return EntryPoolTest::threadExit(nested); }
int entryPoolTouchAfterGiveForTest() { return EntryPoolTest::touchAfterGive(false); }
int entryPoolTouchAfterCrossGiveForTest() { return EntryPoolTest::touchAfterGive(true); }

// ---- step 6 I7's seams (tools/difftest/seg_arena_test.cpp; SR_ORACLE only) and its final counters line
namespace {
struct SegArenaExitLine {
  ~SegArenaExitLine() {
    if (!segarena::g_records.load()) return;
    // The audit's state under ITS lock, held through the hashes (auditFold's order: g_segMu -> g_hashMu; Astra I12 P3 and
    // its fix-check P2) - TRIED, never waited for: a thread ended inside g_segMu at process exit would hang it here. Not
    // acquired: the I12 fields are replaced by "audit=unavailable", which no grader reads as evidence (it FAILs).
    std::unique_lock<std::mutex> sl(g_segMu, std::try_to_lock);
    std::lock_guard<std::mutex> hl(segarena::g_hashMu);
    char hashes[512];  // condition (5)'s checksums, or "off" without SR_SEG_ARENA_HASH=1 (no identity can be read from it)
    char hashes2[256];
    if (segarena::hashOn())
      std::snprintf(hashes2, sizeof hashes2, "capture_hash=%016llx parts_hash=%016llx audit_hash=%016llx",
                    (unsigned long long)segarena::g_capHash, (unsigned long long)segarena::g_partsHash,
                    (unsigned long long)segarena::g_auditHash);
    else
      std::snprintf(hashes2, sizeof hashes2, "capture_hash=off parts_hash=off audit_hash=off");
    if (sl.owns_lock())
      std::snprintf(hashes, sizeof hashes, "%s ring=%s ring_n=%llu audit_hits=%llu audit_misses=%llu ring_slow=%llu "
                    "ring_dupes=%llu ring_tombs=%llu ring_compacts=%llu ring_grows=%llu map_n=%llu ring_slots=%llu",
                    hashes2, auditring::on() ? "on" : "OFF",
                    (unsigned long long)(auditring::g_n - auditring::g_deadN),  // the LIVE entries (references held)
                    (unsigned long long)auditring::g_hits.load(), (unsigned long long)auditring::g_misses.load(),
                    (unsigned long long)auditring::g_slowInserts.load(), (unsigned long long)auditring::g_dupes.load(),
                    (unsigned long long)auditring::g_tombs.load(), (unsigned long long)auditring::g_compacts.load(),
                    (unsigned long long)auditring::g_grows.load(), (unsigned long long)g_segRecordAudit.size(),
                    (unsigned long long)auditring::g_n);
    else
      std::snprintf(hashes, sizeof hashes, "%s audit=unavailable (g_segMu held at exit)", hashes2);
    std::printf("[SEG_ARENA] final %s records=%llu solo=%llu arenas_max=%llu chunks_max=%llu reuse=%llu freed=%llu "
                "bytes_max=%llu retained_max=%llu oversized=%llu node_reuse=%llu cap_faults=%llu bad=%llu %s "
                "checks=%llu verifies=%llu fills=%llu prefetch=%s\n",
                segarena::on() ? "on" : "OFF", (unsigned long long)segarena::g_records.load(),
                (unsigned long long)segarena::g_solo.load(), (unsigned long long)segarena::g_arenasMax.load(),
                (unsigned long long)segarena::g_chunksMax.load(), (unsigned long long)segarena::g_chunkReuse.load(),
                (unsigned long long)segarena::g_chunkFreed.load(), (unsigned long long)segarena::g_bytesMax.load(),
                (unsigned long long)segarena::g_retainedMax.load(), (unsigned long long)segarena::g_oversized.load(),
                (unsigned long long)segarena::g_nodeReuse.load(), (unsigned long long)segarena::g_capFaults.load(),
                (unsigned long long)segarena::g_bad.load(), hashes, (unsigned long long)segarena::g_checks.load(),
                (unsigned long long)segarena::g_verifies.load(), (unsigned long long)segarena::g_fills.load(),
                auditring::prefetchOn() ? "on" : "off");
    std::fflush(stdout);
  }
} g_segArenaExitLine;
std::vector<SegFoot> takeFeetLocked();  // below (segmentClose's)
std::unordered_map<uint64_t, std::shared_ptr<SegRecord>> g_saTestHeld;  // tokens -> records the test holds (g_segMu)
uint64_t g_saTestNext = 1;
}  // namespace
SegArenaCounters segArenaCounters() {
  SegArenaCounters c;
  c.records = segarena::g_records.load();
  c.solo = segarena::g_solo.load();
  c.arenas = segarena::g_arenas.load();
  c.chunks = segarena::g_chunks.load();
  c.chunkReuse = segarena::g_chunkReuse.load();
  c.chunkFreed = segarena::g_chunkFreed.load();
  c.bytes = segarena::g_bytes.load();
  c.retained = segarena::g_retained.load();
  c.oversized = segarena::g_oversized.load();
  c.nodeReuse = segarena::g_nodeReuse.load();
  c.capFaults = segarena::g_capFaults.load();
  c.capLogged = segarena::g_capLogged.load();
  c.bad = segarena::g_bad.load();
  c.checks = segarena::g_checks.load();
  c.verifies = segarena::g_verifies.load();
  c.fills = segarena::g_fills.load();
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    c.ringN = auditring::g_n - auditring::g_deadN;  // the references held (tombstones hold none)
    c.ringSlots = auditring::g_n;
    c.mapN = g_segRecordAudit.size();
  }
  c.auditHits = auditring::g_hits.load();
  c.auditMisses = auditring::g_misses.load();
  c.ringSlow = auditring::g_slowInserts.load();
  c.ringDupes = auditring::g_dupes.load();
  c.ringTombs = auditring::g_tombs.load();
  c.ringCompacts = auditring::g_compacts.load();
  {
    segarena::FreeList& f = segarena::freeList();
    std::lock_guard<std::mutex> lk(f.mu);
    c.freeChunks = f.n;
    c.freeBytes = f.bytes;
  }
  return c;
}
void segArenaForceForTest(int on, int keep) {
  segarena::g_forceOn.store(on, std::memory_order_relaxed);
  segarena::g_forceKeep.store(keep, std::memory_order_relaxed);
}
void segArenaCapForTest(uint64_t bytes) { segarena::g_capForTest.store(bytes, std::memory_order_relaxed); }
bool segArenaHashOnForTest() { return segarena::hashOn(); }
void auditRingForceForTest(int v) { auditring::g_force.store(v, std::memory_order_relaxed); }
uint64_t segArenaCaptureForTest(uint32_t bytes, uint64_t pos, uint8_t fill) {
  std::vector<uint8_t> src(bytes, fill);
  std::lock_guard<std::mutex> lk(g_segMu);
  std::shared_ptr<SegRecord> r = captureSegmentRecord(src.data(), bytes, pos);
  const uint64_t t = g_saTestNext++;
  g_saTestHeld[t] = std::move(r);
  return t;
}
void segArenaCloseForTest() {
  std::lock_guard<std::mutex> lk(g_segMu);
  std::vector<SegFoot> feet = takeFeetLocked();
  if (!feet.empty()) {  // a test close takes no live footprints: give them back in order
    feet.insert(feet.end(), g_segFeet.begin(), g_segFeet.end());
    g_segFeet.swap(feet);
  }
}
bool segArenaReleaseForTest(uint64_t token) {
  std::shared_ptr<SegRecord> r;
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    auto it = g_saTestHeld.find(token);
    if (it == g_saTestHeld.end()) return false;
    r = std::move(it->second);
    g_saTestHeld.erase(it);
  }
  return true;  // `r` drops here, outside g_segMu (as a CP pass would)
}
bool segArenaReclaimForTest(uint64_t pos) {
  uint8_t slot[sizeof(RecHdr)] = {};
  return recordExecutedForAudit(pos, slot);
}
bool segArenaRecordIsForTest(uint64_t token, uint32_t bytes, uint8_t fill) {
  std::lock_guard<std::mutex> lk(g_segMu);
  auto it = g_saTestHeld.find(token);
  if (it == g_saTestHeld.end() || it->second->size() != (bytes + 3u) / 4u) return false;
  const uint8_t* p = it->second->data();
  for (uint32_t i = 0; i < bytes; i++)
    if (p[i] != fill) return false;
  for (uint32_t i = bytes; i < 4u * ((bytes + 3u) / 4u); i++)
    if (p[i] != 0) return false;
  return true;
}
// I7 (4)'s negative control: one record in a fresh arena, held by a SegPart (as a closed segment holds it), then the
// arena's chunks released EARLY - the lifetime bug the design excludes - while the part still holds the record, and
// the record read. Under ASan with SR_SEG_ARENA_KEEP=0 the read must stop the process with a heap-use-after-free
// report (KEEP=1: use-after-poison); without ASan it reads the released chunk (0xDB under SR_SEG_ARENA_HASH=1) and
// returns the byte.
int segArenaEarlyReleaseForTest() {
  auto arena = std::make_shared<SegArena>();
  uint8_t src[64];
  std::memset(src, 0x5A, sizeof src);
  SegRecord* r;
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    r = arena->alloc(16);
    segarena::fillRecord(r, src, sizeof src);
  }
  SegPart part{0u, 64u, 1ull, 0u, 0u, std::shared_ptr<SegRecord>(arena, r)};
  for (segarena::Chunk* c = arena->head; c;) {  // THE BUG: the chunks go while `part` still holds the record
    segarena::Chunk* n = c->next;
    segarena::chunkGive(c);
    c = n;
  }
  arena->head = nullptr;
  const volatile uint8_t* p = part.record->data();
  return int(p[0]);  // THE TOUCH: a read of a released record
}
#endif

// The same admission as draw(): record mode on, the device mapped in one host span, no live trace,
// the 1a projection not owning the stream, arena space (half the arena stays as history).
bool Entry::open() {
  Impl& im = *impl_;
  if (!g_on || im.opened) return false;
  uint8_t* devH = nullptr;
  uint32_t ib0 = 0, ib6 = 0;
  const Args a{im.dr.dev, 0u, 0u, 0u, 0u};
  if (declined(a, devH, ib0, ib6, 1)) {
    g_entryDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (sr::gfx::livetrace::armed() || sr::native::dev::on()) {
    g_entryDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_mu.lock();
  im.locked = true;
  if (!g_arena) {
    g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_arena) {
      g_mu.unlock();
      im.locked = false;
      g_entryDecl.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  for (int spin = 0; g_head + kMaxDrawRec - g_tailSeen > kRecArena / 2 &&
                     g_head + kMaxDrawRec - (g_tailSeen = g_cpTail.load(std::memory_order_acquire)) > kRecArena / 2;
       spin++) {
    if (spin > 2000) {
      g_mu.unlock();
      im.locked = false;
      g_entryDecl.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (spin < 64) _mm_pause();
    else std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  im.dr.d = devH;
  im.dr.c = be32(devH + 40);
  im.dr.spanStart = im.dr.c;
  if (sr::native::cdedup::active()) {
    static const int s_abd = sr::abt::reg("SR_NATIVE_CONST_DEDUP");
    im.dr.ddWant = true;
    im.dr.ddChk = sr::native::cdedup::checkOn();
    im.dr.ddSkip = sr::native::cdedup::on() && (s_abd < 0 || sr::abt::on(s_abd));
    im.dr.ddSync();
  }
  im.payload.clear();
  im.opened = true;
  return true;
}

bool Entry::opaque(uint32_t kind, const void* payload, uint32_t dwords, uint32_t xdkBytes, bool predicated) {
  Impl& im = *impl_;
  if (!im.opened || !recOpaque(kind) || !payload || !dwords || dwords > 0xFFFFu || !xdkBytes || (xdkBytes & 3u)) {
    g_entryDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Driver& dr = im.dr;
  if (dr.nItems + 1 > Driver::kSpanCap) im.fixup();  // reserve() closes the span when it is full
  Item* o = dr.reserve(1);
  if (!o) return false;
  const size_t at = im.payload.size();
  im.payload.resize(at + 4u * dwords);
  std::memcpy(im.payload.data() + at, payload, 4u * dwords);
  Item& it = o[0];
  it.pad = 0;
  it.pkt = 1;
  it.rk = uint8_t(kind);
  it.reg = 0;
  it.count = dwords;
  it.src = nullptr;  // resolved by fixup(): the payload vector may still grow
  it.chk = false;
  it.fixedBytes = xdkBytes;
  it.deferSrc = 1;
  it.drawHdr = predicated ? 1u : 0u;
  it.bodyN = 0;
  it.lit[0] = uint32_t(at);  // offset into the payload copy
  dr.opaque = true;
  dr.commit(1, xdkBytes);
  g_entryPkts.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool Entry::openAt(uint32_t lastDw) {
  if (!open()) return false;
  Impl& im = *impl_;
  im.atCursor0 = be32(im.dr.d + 40);
  im.atMode = true;
  im.dr.ddWant = false;  // no constant dedup on a leaf's block
  im.dr.c = lastDw;
  im.dr.spanStart = lastDw;
  return true;
}

bool Entry::callback(uint32_t cb, uint32_t arg, uint32_t flags, uint32_t& lastDw) {
  Impl& im = *impl_;
  if (!im.opened) return false;
  if (!im.dr.callback(cb, arg, flags)) return false;
  lastDw = im.dr.c;
  return true;
}

bool Entry::presentGate(uint64_t& result) {
  if (!impl_->opened) return false;
  Driver& dr = impl_->dr;
  impl_->fixup();
  // Segment allocation can call SDK emitters while this Entry owns g_mu.
  // Keep those nested calls literal rather than recursively opening an Entry.
  ShaderConstRunsOracle shaderOracle;
  auto timestamp = [&](bool begin) {
    if (be32(dr.d + 0x4E70u) != 2u) return true;
    const uint32_t writer = be32(dr.d + 0x4E78u);
    const uint32_t slot = writer == 3u ? 0u : writer + 1u;
    if (slot == be32(dr.d + 0x4E74u)) return true;
    // Slot offset precedes the kick; the SDK loads the timestamp base after it.
    const uint32_t offset = 4u * (2u * slot + (begin ? 0u : 1u));
    const bool kicked = dr.c > dr.thrEff();
    if (!dr.kickPoint()) return false;
    if (!begin && kicked) result = dr.c;
    const uint32_t va = be32(dr.d + 0x4E6Cu) + offset;
    const uint32_t phys = (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu);
    return eventShd(0x80000003u, phys | 2u, 0xDEADBEEFu);
  };
  // Record mode makes the admitted sequence total. The gate itself has a kick
  // check even if timestamps are inactive or their ring is full.
  return timestamp(true) && dr.kickPoint() && dr.presentGate() && timestamp(false);
}
bool Entry::shaderConstantRuns(uint64_t mask, uint64_t& result) {
  if (!impl_->opened) return false;
  impl_->fixup();
  return impl_->dr.shaderConstantRuns(mask, result);
}
bool Entry::run(uint32_t reg, uint32_t count, const uint8_t* beValues, uint32_t pad) {
  Impl& im = *impl_;
  if (!im.opened || !count || count > 0x3FFFu || !beValues || reg > 0x7FFFu || pad > 3u) {
    g_entryDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Driver& dr = im.dr;
  if (dr.nItems + 1 > Driver::kSpanCap) im.fixup();
  Item* o = dr.reserve(1);
  if (!o) return false;
  const size_t at = im.payload.size();
  im.payload.resize(at + 4u * count);
  std::memcpy(im.payload.data() + at, beValues, 4u * count);
  Driver::run(o[0], pad, reg, count, nullptr);
  o[0].deferSrc = 1;
  o[0].lit[0] = uint32_t(at);
  dr.commit(1, o[0].bytes());
  g_entryRuns.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool Entry::pkt(uint32_t rk, uint32_t hdr, const uint32_t* body, uint32_t bodyN, uint32_t reg) {
  Impl& im = *impl_;
  const bool three = rk == kRkRegRmw || rk == kRkEventShd;  // boot.device_create's 3-word packets
  if (!im.opened || !body || bodyN == 0 || bodyN > 4u || (rk != kRkBinReg && rk != kRkSetConst && !three) ||
      (rk == kRkBinReg && (bodyN != 1u || reg > 3u)) || (three && bodyN != 3u)) {
    g_entryDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Driver& dr = im.dr;
  if (dr.nItems + 1 > Driver::kSpanCap) im.fixup();
  Item* o = dr.reserve(1);
  if (!o) return false;
  Driver::packet(o[0], uint8_t(rk), hdr, {});
  for (uint32_t i = 0; i < bodyN; i++) o[0].body[o[0].bodyN++] = body[i];
  o[0].reg = reg;
  dr.commit(1, o[0].bytes());
  g_entryPkts.fetch_add(1, std::memory_order_relaxed);
  return true;
}
bool Entry::tileRects(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t& r3) {
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();  // the items are literal; the kick checks inside write spans as kick() does
  return im.dr.tileRects(x0, y0, x1, y1, r3);
}
bool Entry::setPredication(uint32_t mask, uint32_t& r3) {
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();
  return im.dr.setPredication(mask, r3);
}
bool Entry::zpdSurface(uint32_t query, uint32_t which, uint32_t& r3) {  // p2.zpd's bake: 82101E08's items
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();
  return im.dr.zpdSurface(query, which, r3);
}
bool Entry::setConstMem(uint32_t type, uint32_t index, uint32_t count, uint32_t& bodyVA) {
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();
  return im.dr.setConstMem(type, index, count, bodyVA);
}
bool Entry::kick() {
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();
  return im.dr.kickPoint();  // record mode: never false
}
bool Entry::flush() {  // p1.flush_residue: 82107058's unconditional 820FD8C0 (the open span written first)
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();
  return im.dr.kick();  // record mode: never false
}
bool Entry::regRmw(uint32_t rmwInfo, uint32_t andMask, uint32_t orMask) {
  const uint32_t b[3] = {rmwInfo, andMask, orMask};
  return pkt(kRkRegRmw, 0xC0022100u, b, 3u);
}
bool Entry::eventShd(uint32_t initiator, uint32_t addr, uint32_t value) {
  const uint32_t b[3] = {initiator, addr, value};
  return pkt(kRkEventShd, 0xC0025800u, b, 3u);
}
// p2.transport RESIDUE: the 11-dword COHER block as one item (44 XDK bytes). The two statuses kRkInvalidate encodes
// travel as kRkInvalidate {base, size, whole} (its renderer and applyCoherency write exactly them); any other status as
// the opaque kRkCoherHost {status, size, base}.
bool Entry::coherency(uint32_t status, uint32_t size, uint32_t base) {
  Impl& im = *impl_;
  if (!im.opened) {
    g_entryDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (status != 0x03000000u && status != 0x01000000u) {
    ensureResidueHandlers();
    const uint32_t payload[3] = {status, size, base};
    return opaque(kRkCoherHost, payload, 3u, 44u);
  }
  Driver& dr = im.dr;
  if (dr.nItems + 1 > Driver::kSpanCap) im.fixup();  // reserve() closes the span when it is full
  Item* o = dr.reserve(1);
  if (!o) return false;
  Driver::packet(o[0], uint8_t(kRkInvalidate), 0u, {base, size, status == 0x01000000u ? 1u : 0u});
  o[0].fixedBytes = 44u;
  dr.commit(1, 44u);
  g_entryPkts.fetch_add(1, std::memory_order_relaxed);
  return true;
}
bool Entry::devResetPackets() {
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();
  return im.dr.devResetPackets();
}
bool Entry::segmentBeginPackets() {
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();
  return im.dr.segmentBeginPackets();
}
bool Entry::modeStatePackets() {
  Impl& im = *impl_;
  if (!im.opened) return false;
  im.fixup();
  return im.dr.modeStatePackets();
}
// boot gaps: 820FC910 at its call site (the Driver's reserveDw: the fast test against dev+44, else the guest reserve -
// the XDK's flush, then its new segment - with the open span written first and the cursor re-read after).
int Entry::reserve(uint32_t dwords) {
  Impl& im = *impl_;
  if (!im.opened) return 0;
  im.fixup();
  return im.dr.reserveDw(dwords);
}

uint32_t Entry::cursor() const { return impl_->opened ? impl_->dr.c : 0u; }
uint32_t Entry::limit() const { return impl_->opened ? impl_->dr.lim() : 0u; }

uint32_t Entry::alloc(uint32_t dwords, uint32_t align) {
  Impl& im = *impl_;
  if (!im.opened) return 0;
  Driver& dr = im.dr;
  im.fixup();
  if (!dr.beforeGuest()) return 0;
  dr.ctx->r3.u64 = dr.dev;
  dr.ctx->r4.u64 = dwords;
  dr.ctx->r5.u64 = align;
  sub_820FCDF0(*dr.ctx, dr.base);
  const uint32_t va = dr.ctx->r3.u32;
  dr.afterGuest();
  g_entryAllocs.fetch_add(1, std::memory_order_relaxed);
  return va;
}

bool Entry::guest(void (*fn)(PPCContext&, uint8_t*), uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7) {
  Impl& im = *impl_;
  if (!im.opened || !fn) return false;
  Driver& dr = im.dr;
  im.fixup();
  if (!dr.beforeGuest()) return false;
  dr.ctx->r3.u64 = r3;
  dr.ctx->r4.u64 = r4;
  dr.ctx->r5.u64 = r5;
  dr.ctx->r6.u64 = r6;
  dr.ctx->r7.u64 = r7;
  ShaderConstRunsOracle oracle;  // nested SDK work cannot acquire this Entry's arena lock again
  fn(*dr.ctx, dr.base);
  dr.afterGuest();
  g_entryGuest.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void Entry::abandon() {
  Impl& im = *impl_;
  if (!im.opened) return;
  im.dr.nItems = 0;  // nothing written: the cursor in dev+40 is untouched (closeSpan never ran)
  im.opened = false;
  im.atMode = false;
  if (im.locked) {
    g_mu.unlock();
    im.locked = false;
  }
}

void Entry::close() {
  Impl& im = *impl_;
  if (!im.opened) return;
  Driver& dr = im.dr;
  im.fixup();
  dr.closeSpan();
  if (im.atMode) {  // openAt(): the leaf's caller sets the cursor itself
    put32(dr.d + 40, im.atCursor0);
    im.atMode = false;
  }
  if (dr.ddWant) {
    if (dr.dd) sr::native::cdedup::noteEmitted(dr.c);
    sr::native::cdedup::countGroups(dr.ddSeen, dr.ddPred);
  }
  g_entries.fetch_add(1, std::memory_order_relaxed);
  im.opened = false;
  if (im.locked) {
    g_mu.unlock();
    im.locked = false;
  }
}

Entry::ClosedSpan Entry::closeDetached() {
  ClosedSpan result{};
  impl_->dr.detachedFoot = &result.foot;
  close();  // closeSpan captures into result while this Entry still owns g_mu.
  impl_->dr.detachedFoot = nullptr;
  return result;
}


// ============================================================================================
// P2-B segmentless transport (SR_NATIVE_SEGMENTLESS): GpuCommandBufferFlush_820FD8C0 as host code.
// Written from the asm of 820FD8C0 / 820FD660 / 820FD580 / 820FD3B8 / 820FD800 (IDA, 2026-10-09).
// ============================================================================================

bool segmentlessOn() { return g_segmentless && g_on; }

// tools/graceful-quit (kernel/quit.hpp): the periodic report with any why token (the terminal snapshot, called once
// the producers are parked and the CP drained), and one number that moves whenever a producer or the CP does flush
// work (the quit's settle digest).
// The segment lines too, with the same token, once the segment transport has run (P1-2 of Astra round 1 on
// tools/graceful-quit: idMiss / mismatch / capDrop and the tile's gap / overlap / order get terminal values).
void reportLine(const char* why) {
  report(why);
  if (segmentlessOn() || g_segClosed.get()) segReport(why);
}
uint64_t segGapPartsApplied() { return g_segGapApplied.get(); }

uint64_t activityTotal() {
  uint64_t n = 0;
  for (int i = 0; i < kWhyN; i++) n += g_why[i].get();
  return n + g_spansRec.get() + g_spansPm4.get() + g_kicks.get() + g_guestCalls.get() + g_cpRecs.get() +
         g_cpRuns.get() + g_cpDraws.get() + g_segClosed.get() + g_segRecApplied.get() + g_segGapApplied.get() +
         g_binds.get() + g_entries.get() + g_beginV.get();
}

namespace {
// Capped diagnostic for the two counters that must stay 0 (the memory note of the wedge: "dump the
// CP's item kinds around the kick before theorising").
void segDiag(const char* what, uint64_t id, uint32_t phys, uint32_t dwords, const SegParts* sp) {
  static std::atomic<uint32_t> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) < 8)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_FLUSH] SEGMENT %s: item id=%llu phys=%08X dw=%u%s%08X dw=%u%s",
                what, (unsigned long long)id, phys, dwords, sp ? " registered phys=" : "", sp ? sp->phys : 0u,
                sp ? sp->dwords : 0u, sp && sp->deferred ? " (deferred)" : "");
}

// The PM4 census (SR_NATIVE_BRACKETS, observe-only): the gap part about to run is a host copy of the guest bytes at
// part.va - its packets are attributed by the stamps the segment took with the copy.
#if SR_ORACLE
inline void censusCopy(const SegParts& sp, const SegPart& part) {
  if (sr::gfx::pm4cov::on())
    sr::gfx::pm4cov::setCopyWindow(sp.bytes.data() + part.off, sr::native::stampKey(part.va), sp.stamps.data(),
                                   sp.stamps.size());
}
#else
inline void censusCopy(const SegParts&, const SegPart&) {}
#endif

// p2.transport BRACKET (the CP thread): one replay step of a BRACKET_END segment - a bin select (kind 2) or a bound
// bracket segment applied whole (kind 3) - exactly what the pump's 82110140 / 820FD218 items brought this consumer:
// the BS through applyBinReg (the P2-A BS item's method; the CPPM4 split runs the packet through execute()), the
// segment's records and gap bytes in order, its FIRST application counted for SR_NATIVE_SEG_TILE as a bracket's.
void bracketLifecycle(uint16_t aux, const SegParts& sp, uint32_t steps) {
  namespace tr = sr::native::p2trace;
  if (!tr::g_on) return;
  tr::event(tr::kCpItem, aux, uint32_t(sp.id), steps, uint32_t(sp.bracketGen));
#if defined(SR_DIFFTEST)
  if (aux == 6u) g_brTestLifecycle.clear();
  g_brTestLifecycle.push_back({uint32_t(aux), uint32_t(sp.id), steps, uint32_t(sp.bracketGen)});  // exactly as traced
#endif
}
#if !SR_ORACLE
// C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9, 3.1 rule 4): no PM4 walker in the release link, so a gap part - literal
// PM4 a guest emitter wrote between a segment's spans, a producer not converted - is fatal, never skipped (the caller
// latches it; the CP stops: [NATIVE_FLUSH_BAD], exit 3; dev executes and counts it, g_segGapApplied). The line names the
// location (Astra round 1): a native segment (cpSegment) or one a BRACKET_END replays (bracketStep: the Z-pass recording).
void gapRefused(const SegParts& sp, const SegPart& part, bool br) {
  static std::atomic<uint32_t> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) < 4)
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NATIVE_FLUSH_BAD] literal-PM4 gap in %s id=%llu phys=%08X: va=%08X len=%u - a producer was not "
                "converted; the release configuration has no PM4 walker (C1 stage 1)", br ? "a bracket-replayed segment"
                : "native segment", (unsigned long long)sp.id, sp.phys, part.va, part.len);
}
#endif
bool bracketStep(sr::gfx::CpEngine& eng, const SegParts& owner, const SegPart& part) {
  namespace tr = sr::native::p2trace;
  if (part.kind == 2u) {
    const uint32_t value = part.off;
#if SR_ORACLE  // C1 stage 1: CPPM4 is a development oracle over the PM4 walker
    if (g_cpPm4) {
      uint8_t pkt[8];
      put32(pkt, 0xC0006200u);  // SET_BIN_SELECT_LO, as 82110140 writes it
      put32(pkt + 4, value);
      eng.execute(pkt, sizeof(pkt), 1);
    } else
#endif
    {
      eng.applyBinReg(2u, value);
    }
    if (tr::g_on) tr::event(tr::kBs, 1u, 0u, value, uint32_t(owner.bracketGen));
    g_brBs.fetch_add(1, std::memory_order_relaxed);
    return !eng.nativeRecordFailed;
  }
  const std::shared_ptr<SegParts>& sp = part.nested;
  if (part.kind != 3u || !sp || sp->bracketGen) return false;  // a bound segment holds records and gaps only
  bool first;
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    first = sp->started == 0;
    sp->started++;
  }
  const bool tileFirst = g_segTile && first;
  if (tileFirst) {
    std::lock_guard<std::mutex> lk(g_tileMu);
    g_first.begin(sp->id, true);  // a Z-pass bracket's segment: counted apart, as the guest replay's were
  }
  if (tr::g_on) tr::event(tr::kCpItem, 4u, uint32_t(sp->id), sp->phys, sp->dwords);
  t_segCur.sp = sp.get();
  for (size_t i = 0; i < sp->parts.size(); i++) {
    const SegPart& p = sp->parts[i];
    t_segCur.idx = uint32_t(i);
    if (p.kind == 0) {
      if (!p.record) return false;
      if (tileFirst) {
        const bool ran = p.record->completed.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lk(g_tileMu);
        g_first.rec(p.pos, ran);
      }
      if (!cpApplyRecord(eng, p.pos, p.record.get())) return false;
      g_segRecApplied.fetch_add(1, std::memory_order_relaxed);
    } else if (p.kind == 1u && p.off <= sp->bytes.size() && p.len <= sp->bytes.size() - p.off) {
#if SR_ORACLE
      censusCopy(*sp, p);
      eng.execute(sp->bytes.data() + p.off, p.len, 0);
      if (eng.nativeRecordFailed) return false;
      g_segGapApplied.fetch_add(1, std::memory_order_relaxed);
#else
      gapRefused(*sp, p, true);  // C1 stage 1: fatal in the release configuration (the caller latches the failure)
      return false;
#endif
    } else {
      return false;
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    sp->finished++;
  }
  g_brSegs.fetch_add(1, std::memory_order_relaxed);
  return true;
}
}  // namespace

uint64_t segmentPublishId(uint32_t phys, uint32_t dwords) {
  if (!g_segmentless) return 0;
  // 1. The close publishing its own segment: handed over on this thread, checked, consumed.
  if (std::shared_ptr<SegParts> sp = std::move(t_segPublishing)) {
    t_segPublishing.reset();
    if (sp->phys == phys && sp->dwords == dwords) {
      std::lock_guard<std::mutex> lk(g_segMu);
      sp->published++;
      if (sp->deferred) {  // registered deferred in case 820FD800 chained it: published now, it is a plain entry
        sp->deferred = false;  // (applied once; before its item exists, so the CP never saw it)
        for (auto it = g_segDeferred.rbegin(); it != g_segDeferred.rend(); ++it)
          if (it->get() == sp.get()) {
            g_segDeferred.erase(std::next(it).base());
            break;
          }
      }
      g_segHandoff.fetch_add(1, std::memory_order_relaxed);
      if (sr::native::p2trace::g_on) sr::native::p2trace::event(sr::native::p2trace::kSegLookup, 1u, uint32_t(sp->id), phys, dwords);
      return sp->id;
    }
    g_segMismatch.fetch_add(1, std::memory_order_relaxed);
    segDiag("hand-off mismatch", sp->id, phys, dwords, sp.get());
    if (sr::native::p2trace::g_on) sr::native::p2trace::event(sr::native::p2trace::kSegLookup, 3u, uint32_t(sp->id), phys, dwords);
    std::lock_guard<std::mutex> lk(g_segMu);
    g_segById.erase(sp->id);  // the parts never reach the CP; the block goes as the IB it is
    for (auto it = g_segDeferred.begin(); it != g_segDeferred.end(); ++it)  // and its tentative deferred entry
      if (it->get() == sp.get()) {
        g_segDeferred.erase(it);
        break;
      }
    return 0;
  }
  // 2. The pump replaying a bracket (or a chained segment): the deferred entries of the current and the
  //    previous bracket, by (phys, dwords). A guest-closed segment in the A/B is not here: a plain IB.
  std::lock_guard<std::mutex> lk(g_segMu);
  for (auto it = g_segDeferred.rbegin(); it != g_segDeferred.rend(); ++it) {
    const SegParts& sp = **it;
    if (sp.phys == phys && sp.dwords == dwords) {
      (*it)->published++;
      g_segReplayHit.fetch_add(1, std::memory_order_relaxed);
      if (sr::native::p2trace::g_on) sr::native::p2trace::event(sr::native::p2trace::kSegLookup, 2u, uint32_t(sp.id), phys, dwords);
      return sp.id;
    }
  }
  if (!g_segDeferred.empty()) g_segReplayMiss.fetch_add(1, std::memory_order_relaxed);
  if (sr::native::p2trace::g_on) sr::native::p2trace::event(sr::native::p2trace::kSegLookup, 0u, 0u, phys, dwords);
  return 0;
}

// native/quit-drain round 3 (Astra round 2 P1): what the graceful quit's ring cut is dropping through 820FD218 -
// segmentPublishId's two lookups, non-consuming (the hand-off stays for publishSegment, no replay counted). Read only
// under the cut (kernel/gpu_ringless.cpp): a native publication's guest bytes are not its packets (record mode leaves
// the bytes under a native footprint untouched, so a reused segment can still end in an older close's EVENT_WRITE_SHD).
int cutPublicationKind(uint32_t phys, uint32_t dwords) {
  if (!g_segmentless) return 0;
  if (t_segPublishing) return t_segPublishing->phys == phys && t_segPublishing->dwords == dwords ? 1 : 3;
  std::lock_guard<std::mutex> lk(g_segMu);
  for (const std::shared_ptr<SegParts>& sp : g_segDeferred)
    if (sp->phys == phys && sp->dwords == dwords) return 2;
  return 0;
}

bool cpSegment(sr::gfx::CpEngine& eng, uint64_t id, uint32_t phys, uint32_t dwords) {
  if (eng.nativeRecordFailed) return false;
  std::shared_ptr<SegParts> sp;
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    auto it = g_segById.find(id);
    if (it == g_segById.end()) {
      g_segIdMiss.fetch_add(1, std::memory_order_relaxed);
      segDiag("unknown id", id, phys, dwords, nullptr);
      eng.nativeRecordFailed = true;
      return false;
    }
    sp = it->second;
    if (sp->phys != phys || sp->dwords != dwords) {
      g_segMismatch.fetch_add(1, std::memory_order_relaxed);
      segDiag("item mismatch", id, phys, dwords, sp.get());
      eng.nativeRecordFailed = true;
      return false;
    }
    if (sp->started >= sp->published) {
      g_segMismatch.fetch_add(1, std::memory_order_relaxed);
      segDiag("unpublished pass", id, phys, dwords, sp.get());
      eng.nativeRecordFailed = true;
      return false;
    }
    sp->started++;
  }
  // SR_NATIVE_SEG_TILE: the FIRST application only (a deferred entry's later applies are the guest's own replays).
  const bool tileFirst = g_segTile && sp->started == 1;
  if (tileFirst) {
    std::lock_guard<std::mutex> lk(g_tileMu);
    g_first.begin(sp->id, sp->deferred);
  }
  t_segCur.sp = sp.get();
  uint32_t bracketSteps = 0;
  // A BRACKET_END's CP lifecycle (P2 trace kind 24, Astra round 1 on the transport-fix lane): aux 6 when its replay
  // begins, aux 7 once every step ran {f = segment id low, a = steps (planned / applied), b = generation low}. A
  // bracket with a begin and no end was in flight when the trace ended (p2_trace.py tail_bound_unapplied requires it).
  if (sp->bracketGen) bracketLifecycle(6u, *sp, sp->bracketSteps);
  for (size_t i = 0; i < sp->parts.size(); i++) {
    const SegPart& part = sp->parts[i];
    t_segCur.idx = uint32_t(i);
    if (part.kind == 2u || part.kind == 3u) {  // p2.transport BRACKET: a replay step of a BRACKET_END segment
      if (!sp->bracketGen || !bracketStep(eng, *sp, part)) {
        eng.nativeRecordFailed = true;
        t_segCur.sp = nullptr;
        return false;
      }
      t_segCur.sp = sp.get();
      t_segCur.idx = uint32_t(i);
      bracketSteps++;
      continue;
    }
    if (part.kind == 0) {
      if (!part.record) {
        eng.nativeRecordFailed = true;
        t_segCur.sp = nullptr;
        return false;
      }
      if (tileFirst) {  // executed before its segment's first application = a duplicate
        const bool ran = part.record->completed.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lk(g_tileMu);
        g_first.rec(part.pos, ran);
      }
      if (!cpApplyRecord(eng, part.pos, part.record.get())) {
        eng.nativeRecordFailed = true;
        t_segCur.sp = nullptr;
        return false;
      }
      g_segRecApplied.fetch_add(1, std::memory_order_relaxed);
    } else {
      if (part.kind != 1u || part.off > sp->bytes.size() || part.len > sp->bytes.size() - part.off) {
        eng.nativeRecordFailed = true;
        t_segCur.sp = nullptr;
        return false;
      }
#if SR_ORACLE
      censusCopy(*sp, part);
      eng.execute(sp->bytes.data() + part.off, part.len, 0);  // the gap: what a guest emitter still wrote
      if (eng.nativeRecordFailed) { t_segCur.sp = nullptr; return false; }
      g_segGapApplied.fetch_add(1, std::memory_order_relaxed);
#else
      // C1 stage 1: no PM4 walker in the release configuration - a literal-PM4 gap is fatal (gapRefused).
      gapRefused(*sp, part, false);
      eng.nativeRecordFailed = true;
      t_segCur.sp = nullptr;
      return false;
#endif
    }
  }
  t_segCur.sp = nullptr;
  if (bracketSteps != sp->bracketSteps) {  // a BRACKET_END whose steps did not all run, or steps on another segment
    eng.nativeRecordFailed = true;
    return false;
  }
  if (sp->bracketGen) bracketLifecycle(7u, *sp, bracketSteps);
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    sp->finished++;
    if (sp->kickCallback) g_kickCbApplied.fetch_add(1, std::memory_order_relaxed);
    if (!sp->deferred && sp->complete()) g_segById.erase(sp->id);
  }
  return true;
}

bool cpSegPartDescribe(char* out, size_t n, uint32_t* gapVA, const uint8_t** gapCopy, uint32_t* gapLen) {
  const SegParts* sp = t_segCur.sp;
  if (gapVA) *gapVA = 0;
  if (gapCopy) *gapCopy = nullptr;
  if (gapLen) *gapLen = 0;
  if (!sp || t_segCur.idx >= sp->parts.size()) return false;
  const SegPart& part = sp->parts[t_segCur.idx];
  if (part.kind == 0) {
    std::snprintf(out, n, "seg id=%llu phys=%08X dw=%u%s part %u/%zu record pos=%llu len=%u",
                  (unsigned long long)sp->id, sp->phys, sp->dwords, sp->deferred ? " (deferred)" : "", t_segCur.idx,
                  sp->parts.size(), (unsigned long long)part.pos, part.len);
  } else {
    std::snprintf(out, n, "seg id=%llu phys=%08X dw=%u%s part %u/%zu gap va=%08X len=%u", (unsigned long long)sp->id,
                  sp->phys, sp->dwords, sp->deferred ? " (deferred)" : "", t_segCur.idx, sp->parts.size(), part.va,
                  part.len);
    if (gapVA) *gapVA = part.va;
    if (gapCopy) *gapCopy = sp->bytes.data() + part.off;
    if (gapLen) *gapLen = part.len;
  }
  return true;
}

namespace {

// Guest bytes [lo, hi) appended to `out` (4-byte-safe across host mappings).
void readGuestBytes(uint32_t lo, uint32_t hi, std::vector<uint8_t>& out) {
  if (hi <= lo) return;
  const uint32_t n = hi - lo;
  const uint8_t* h0 = sr::kernel::guestVAToHost(lo);
  const uint8_t* h1 = sr::kernel::guestVAToHost(hi - 1u);
  if (h0 && h1 && h1 - h0 == ptrdiff_t(n) - 1) {  // one host mapping: a single copy
    out.insert(out.end(), h0, h0 + n);
    return;
  }
  for (uint32_t va = lo; va < hi; va += 4) {
    uint8_t b[4] = {0, 0, 0, 0};
    if (const uint8_t* h = sr::kernel::guestVAToHost(va)) std::memcpy(b, h, std::min<uint32_t>(4u, hi - va));
    out.insert(out.end(), b, b + std::min<uint32_t>(4u, hi - va));
  }
}

// A footprint belongs to the segment it was WRITTEN in, by sequence: closeSpan appends it to the open segment's
// list and that segment's close takes the whole list, in order. Nothing assigns a footprint to a segment by
// comparing its address with another block's (the segd wedge, docs/NATIVE_P2B_SEGMENTLESS.md 2b: the coherency
// block, carved by the XDK allocator from the segment tail ABOVE the open segment, ran the old range rule
// "f.hi <= lo: behind the range, stale" over the open segment's footprints and dropped all of them, so every
// non-pinned segment went out as gap bytes - unwritten memory on the first lap, the previous lap's PM4 after the
// wrap). Under g_segMu.
std::vector<SegFoot> takeFeetLocked() {
  std::vector<SegFoot> feet;
  feet.swap(g_segFeet);
  if (segarena::on()) {
    g_segFeet.swap(g_segFeetSpare);  // I7 design (e): the next segment starts with a recycled capacity
    segarena::retireOpenLocked();    // I7 design (b): the segment's close replaces the open arena
  }
  return feet;
}
// I7 design (e): a close hands its footprint vector back once buildParts has taken its records: its capacity becomes
// the next close's spare. The footprints' references are dropped first, outside g_segMu.
void recycleFeet(std::vector<SegFoot>& feet) {
  if (!segarena::on()) return;
  feet.clear();
  std::lock_guard<std::mutex> lk(g_segMu);
  if (feet.capacity() > g_segFeetSpare.capacity()) g_segFeetSpare.swap(feet);
}

void footDiag(const char* what, const SegFoot& f, uint32_t lo, uint32_t hi, uint32_t at) {
  static std::atomic<uint32_t> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) < 8)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NATIVE_FLUSH] SEGMENT footprint %s: foot [%08X, %08X) pos=%llu, segment [%08X, %08X) at %08X", what,
                f.lo, f.hi, (unsigned long long)f.pos, lo, hi, at);
}

// The closed range [lo, hi) with its own footprints `feet` (in the order they were written): each footprint's
// record, and the guest's bytes between them (and before the first / after the last) as gap parts - the PM4 a
// guest emitter wrote around the native spans of THIS segment. A footprint outside [lo, hi) or behind the
// previous one cannot be placed: counted (footStray, must stay 0), logged, not applied.
std::shared_ptr<SegParts> buildParts(uint32_t lo, uint32_t hi, const std::vector<SegFoot>& feet) {
  auto sp = std::make_shared<SegParts>();
  if (segarena::on()) {  // I7 design (e): one allocation per vector - the parts, and the gap bytes (a first pass)
    sp->parts.reserve(2 * feet.size() + 1);
    uint32_t gap = 0, at0 = lo;
    for (const SegFoot& f : feet) {
      if (f.lo < at0 || f.hi > hi || f.hi < f.lo) continue;
      gap += f.lo - at0;
      at0 = f.hi;
    }
    if (at0 < hi) gap += hi - at0;
    sp->bytes.reserve(gap);
  }
#if SR_ORACLE
  if (sr::native::g_bracketsOn && hi > lo) {  // the census: the range's stamps move into the segment with its bytes
    const uint32_t key = sr::native::stampKey(lo);
    if (key) sp->stamps = sr::native::takeIntervals(key, key + (hi - lo));
  }
#endif
  uint32_t at = lo;
  for (const SegFoot& f : feet) {
    if (f.lo < at || f.hi > hi || f.hi < f.lo) {
      g_segFootStray.fetch_add(1, std::memory_order_relaxed);
      footDiag(f.lo < lo || f.hi > hi ? "outside its segment" : "out of order", f, lo, hi, at);
      continue;
    }
    if (f.lo > at) {
      const uint32_t off = uint32_t(sp->bytes.size());
      readGuestBytes(at, f.lo, sp->bytes);
      sp->parts.push_back(SegPart{1u, f.lo - at, 0ull, off, at});
      g_segGapBytes.fetch_add(f.lo - at, std::memory_order_relaxed);
    }
    sp->parts.push_back(SegPart{0u, f.hi - f.lo, f.pos, 0u, 0u, f.record});
    at = f.hi;
  }
  if (at < hi) {
    const uint32_t off = uint32_t(sp->bytes.size());
    readGuestBytes(at, hi, sp->bytes);
    sp->parts.push_back(SegPart{1u, hi - at, 0ull, off, at});
    g_segGapBytes.fetch_add(hi - at, std::memory_order_relaxed);
  }
  g_segParts.fetch_add(sp->parts.size(), std::memory_order_relaxed);
#if SR_ORACLE
  if (segarena::hashOn()) {  // condition (5): the part list as the CP will apply it (SR_SEG_ARENA_HASH=1)
    std::lock_guard<std::mutex> hl(segarena::g_hashMu);
    segarena::fold(segarena::g_partsHash, &lo, sizeof lo);
    segarena::fold(segarena::g_partsHash, &hi, sizeof hi);
    for (const SegPart& p : sp->parts) {
      const uint64_t f[5] = {p.kind, p.len, p.pos, p.off, p.va};
      segarena::fold(segarena::g_partsHash, f, sizeof f);
    }
    if (!sp->bytes.empty()) segarena::fold(segarena::g_partsHash, sp->bytes.data(), sp->bytes.size());
  }
#endif
  return sp;
}

// SR_NATIVE_SEG_TILE: a registered segment's record parts, sized from the headers the writer wrote (a header that is
// not this record's leaves its bytes unclaimed: the next close counts the hole as a gap).
void tileSegment(const std::vector<SegPart>& parts) {
  std::vector<uint64_t> pos;
  std::vector<uint32_t> bytes;
  for (const SegPart& part : parts) {
    if (part.kind != 0) continue;
    RecHdr h;
    std::memcpy(&h, part.record->data(), sizeof(h));
    if (h.magic != kRecMagic || h.pos != part.pos) continue;
    pos.push_back(part.pos);
    bytes.push_back(h.bytes);
  }
  std::lock_guard<std::mutex> lk(g_tileMu);
  g_tile.segment(pos.data(), bytes.data(), pos.size());
}

// The closed segment's parts get their id (and, when the pump will publish them later, their bracket
// generation). Under g_segMu; the caller then publishes directly (publishSegment) or leaves the entry
// to the pump's replay.
void registerParts(uint32_t phys, uint32_t dwords, const std::shared_ptr<SegParts>& sp, bool deferred,
                   uint32_t passes = 1) {
  sp->phys = phys;
  sp->dwords = dwords;
  sp->deferred = deferred;
  sp->passes = passes;
  if (g_segTile) tileSegment(sp->parts);  // SR_NATIVE_SEG_TILE: a registered segment's records are claimed
  std::lock_guard<std::mutex> lk(g_segMu);
  sp->id = g_segNextId++;
  sp->bracket = g_segBracket;
  g_segById[sp->id] = sp;
  if (deferred) {
    g_segDeferred.push_back(sp);
    if (g_segDeferred.size() > kSegDeferredCap) {
      // Collect only completed entries. published == finished alone does not
      // make an entry disposable: it may still owe its unpublished second pass.
      size_t dropped = 0;
      {
        auto keep = g_segDeferred.begin();
        for (auto it = g_segDeferred.begin(); it != g_segDeferred.end(); ++it) {
          SegParts& e = **it;
          const bool drop = &e != sp.get() && g_segDeferred.size() - dropped > kSegDeferredCap / 2 &&
                            e.complete();
          if (drop) {
            g_segById.erase(e.id);
            dropped++;
          } else {
            if (keep != it) *keep = std::move(*it);
            ++keep;
          }
        }
        g_segDeferred.erase(keep, g_segDeferred.end());
        dropped = 0;
      }
      if (g_segDeferred.size() > kSegDeferredCap) {
        const uint64_t n = g_segCapPressure.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[NATIVE_FLUSH] deferred capacity pressure #%llu: %zu live entries retained",
                      (unsigned long long)n, g_segDeferred.size());
      }
    }
  }
}

// p2.transport BRACKET: a pinned (mode-B) close of the open native generation's device, on its thread, binds in order:
// its list record's VA, n, phys and parts. Another thread's pinned close of that device poisons the generation.
void bracketNoteClose(uint32_t dev, uint32_t listVA, uint32_t dwords, uint32_t phys, const std::shared_ptr<SegParts>& sp) {
  std::lock_guard<std::mutex> lk(g_segMu);
  if (!g_brOpen || g_brOpen->dev != dev) return;
  if (g_brOpen->thread != GetCurrentThreadId()) {
    if (!g_brOpen->poisoned) g_brPoison.fetch_add(1, std::memory_order_relaxed);
    g_brOpen->poisoned = true;
    return;
  }
  g_brOpen->closes.push_back(BracketClose{listVA, dwords, phys, sp});
}

// A new Z-pass bracket begins (the close sees dev+0x3290 go 0 -> pinned): the entries of brackets older
// than the previous one have been replayed (twice) and applied; they expire. Owner thread.
void segmentBracketBegin() {
  std::lock_guard<std::mutex> lk(g_segMu);
  ++g_segBracket;
  // Entries two brackets old expire only when COMPLETE (every pass published and applied, nothing in flight); an
  // older entry the CP or the pump still needs stays - the producer's bracket count is not the consumer's progress.
  size_t n = 0;
  auto keep = g_segDeferred.begin();
  for (auto it = g_segDeferred.begin(); it != g_segDeferred.end(); ++it) {
    SegParts& e = **it;
    if (e.bracket + 1 < g_segBracket && e.complete()) {
      g_segById.erase(e.id);
      n++;
    } else {
      if (keep != it) *keep = std::move(*it);
      ++keep;
    }
  }
  g_segDeferred.erase(keep, g_segDeferred.end());
  if (n) g_segExpired.fetch_add(n, std::memory_order_relaxed);
}

// GpuRingAppendJumpSegment_820FD800(dev, cursorEnd, phys, dwords, 0, list = dev+0x32C8) - the guest's own body,
// called as 820FD660 calls it: under the spinlock dev+0x2A78 it re-checks chain mode (dev+0x2A74 != 0) and either
// writes the 4-dword list block (the pump publishes the segment later, after its bracket replay) or, out of chain
// mode, publishes through 820FD218 (our override). The pump's end-sync 82110768 takes the same lock before it
// parks, so a segment is chained only while the pump will still walk it. (segf, 2026-10-10: this close used to read
// dev+0x2A74 unlocked and write the list block itself; at present 2731 the pump had already found the list empty
// and parked - the chained segment was never published, the guest's BlockOnFence(S) waited forever, H0.)
// Which branch it takes is the guest's decision, so the parts are registered DEFERRED (found by the pump's
// replay by (phys, dwords)) and handed over on this thread at once (segmentPublishId turns a hand-off into a
// plain, applied-once entry). Returns the cursor after (cursorEnd, or cursorEnd + 16 when chained).
// `fence` = the fence this segment carries (segmentClose's S; 0 = none, the coherency block): a publish the graceful
// quit's ring cut drops records it (kernel/quit.hpp recordDroppedFence, native/quit-drain round 2). `progress` = the
// progress word its fence tail carries ((gen & 3) | cursor, the kRkFence record's first EVENT_WRITE_SHD; 0 = none):
// recorded the same way (recordDroppedProgress, native/quit-ringspace) - only these free a stranded ring-space wait.
uint32_t publishSegment(Driver& dr, uint32_t cursorEnd, uint32_t phys, uint32_t dwords,
                        const std::shared_ptr<SegParts>& sp, uint32_t fence, uint32_t progress) {
  PPCContext& ctx = *dr.ctx;
  registerParts(phys, dwords, sp, /*deferred=*/true);
  t_segPublishing = sp;
  ctx.r3.u64 = dr.dev;
  ctx.r4.u64 = cursorEnd;
  ctx.r5.u64 = phys;
  ctx.r6.u64 = dwords;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = dr.dev + 0x32C8u;
  sub_820FD800(ctx, dr.base);
  const uint32_t after = ctx.r3.u32;
#if defined(SR_DIFFTEST)
  // Worker-free fixtures: no CP, so the guest 820FD218 wrote the descriptor; take the hand-off the ringless override
  // takes on the publishing thread and give the id to the harness's in-order consumer.
  if (g_segPublishObserver && t_segPublishing == sp && after == cursorEnd && !ringCut())  // (the cut: none taken)
    if (const uint64_t id = segmentPublishId(phys, dwords)) g_segPublishObserver(id, phys, dwords, false);
#endif
  if (t_segPublishing) {  // no hand-off taken: chained (the pump publishes it), or the ring path took the block
    t_segPublishing.reset();
    if (after == cursorEnd + 16u) {
      g_segChained.fetch_add(1, std::memory_order_relaxed);
      // The PM4 census (observe-only): 820FCA80's software-list node [cursorEnd, +16), the pump's to read, left
      // unowned on purpose and marked, so a CP read there would count apart (chain=), never as anyone's packets.
      sr::native::markChain(cursorEnd, 16u);
    } else if (ringCut()) {  // the graceful quit's ring cut dropped this publish: the parts are dead, an orderly stop
      sr::kernel::quit::recordDroppedFence(dr.dev, fence);  // the fence it carried (0: none) - never written now
      sr::kernel::quit::recordDroppedProgress(dr.dev, progress);  // its progress word (0: none) - never written now
      std::lock_guard<std::mutex> lk(g_segMu);
      g_segById.erase(sp->id);
      g_segDeferred.erase(std::remove(g_segDeferred.begin(), g_segDeferred.end(), sp), g_segDeferred.end());
      g_cutSegs.fetch_add(1, std::memory_order_relaxed);
    } else {  // the override did not run (a whole-body guest mode): the block went as PM4; the parts are dead
      std::lock_guard<std::mutex> lk(g_segMu);
      g_segById.erase(sp->id);
      for (auto it = g_segDeferred.begin(); it != g_segDeferred.end(); ++it)
        if (it->get() == sp.get()) {
          g_segDeferred.erase(it);
          break;
        }
      g_segMismatch.fetch_add(1, std::memory_order_relaxed);
      segDiag("publish not taken", sp->id, phys, dwords, sp.get());
    }
  }
  return after;
}

// GpuBuildFlushMemoryPackets_820FD3B8 + its publish (the first half of 820FD8C0): the deferred CPU-write
// range dev+0x2D18 (taken atomically, the empty sentinel left) and, when byte dev+0x2A3A & 4, the whole command
// buffer, as INVALIDATE records in a span over the block the XDK allocates from the segment tail; published as its
// own segment through a 4-dword tail hole, exactly as the XDK.
// The range is the big-endian doubleword {end, base} (dev+0x2D18 = end, dev+0x2D1C = base). Its EMPTY value is the
// SDK's 0x00000000FFFFFFFF (820FD3B8's r26 = clrldi(-1, 32); D3DDeviceResetState 8210E520 writes it): end 0, base
// 0xFFFFFFFF, so the lock core's non-owner merge (820F4280 / 820F3988: base = min, end = max) yields exactly the
// merged range. (Until 2026-10-14 this port compared and restored all ones: every close after a reset or a guest
// kick then emitted a bogus {base 0xFFFFF000, size 0x1000} invalidate, every guest kick after a native close did
// too, and a merge into all ones kept end 0xFFFFFFFF - an invalidate up to 4 GB. native/coher-sentinel.)
constexpr uint64_t kCoherEmpty = 0x00000000FFFFFFFFull;
void coherencyBlock(Driver& dr) {
  PPCContext& ctx = *dr.ctx;
  uint8_t* d = dr.d;
  const bool hasRange = be64(d + 0x2D18u) != kCoherEmpty;
  const bool whole = ((d[0x2A3Au] >> 2) & 1u) != 0;
  const uint32_t n = 11u * (uint32_t(hasRange) + uint32_t(whole));
  if (!n) return;
  const uint32_t cur0 = be32(d + 40);
  ctx.r3.u64 = dr.dev;
  ctx.r4.u64 = n;
  ctx.r5.u64 = 32u;
  sub_820FCDF0(ctx, dr.base);  // the tail allocation (moves dev+0x2C / 0x30 as the XDK's)
  const uint32_t va = ctx.r3.u32;
  if (!va) return;
  dr.spanStart = va - 4u;
  dr.c = va - 4u;
  dr.nItems = 0;
  if (hasRange) {
    // The XDK's ldarx / stdcx loop: take the range (re-read at the swap, as the asm) and leave the empty sentinel
    // (the lock core on other threads merges into it with the same protocol).
    volatile long long* q = reinterpret_cast<volatile long long*>(d + 0x2D18u);
    const long long empty = static_cast<long long>(_byteswap_uint64(kCoherEmpty));  // guest bytes 00000000 FFFFFFFF
    long long raw;
    for (;;) {
      raw = *q;
      if (_InterlockedCompareExchange64(q, empty, raw) == raw) break;
    }
    uint64_t v;
    std::memcpy(&v, &raw, 8);
    v = _byteswap_uint64(v);
    const uint32_t end = uint32_t(v >> 32), base = uint32_t(v) & ~0xFFFu;
    const uint32_t size = ((end + 0xFFFu) & ~0xFFFu) - base;
    g_coherRange.fetch_add(1, std::memory_order_relaxed);
    if (size && uint64_t(base) + size >= 0x100000000ull) g_coherTop.fetch_add(1, std::memory_order_relaxed);
    Item* o = dr.reserve(1);
    Driver::packet(o[0], uint8_t(kRkInvalidate), 0u, {base, size, 0u});
    o[0].fixedBytes = 44u;
    dr.commit(1, 44u);
  }
  if (whole) {
    const uint32_t bufLo = be32(d + 0x34ACu), bufHi = be32(d + 0x34B0u);
    auto physOf = [](uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); };
    const uint32_t basePage = physOf(bufLo) & ~0xFFFu;
    const uint32_t endPage = (physOf(bufHi) + 0xFFFu) & ~0xFFFu;
    Item* o = dr.reserve(1);
    Driver::packet(o[0], uint8_t(kRkInvalidate), 0u, {basePage, endPage - basePage, 1u});
    o[0].fixedBytes = 44u;
    dr.commit(1, 44u);
  }
  dr.opaque = false;
  dr.closeSpan();          // the record + its foot [va, va + 4n) (none when the span went literal: CPPM4)
  put32(d + 40, cur0);     // the block lives in the tail: the cursor is untouched
  // Its own footprint only - the one closeSpan just appended. The open segment's footprints stay pending for
  // segmentClose; coherDropOld counts the ones the old range rule dropped here (observe-only).
  std::vector<SegFoot> own;
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    if (!g_segFeet.empty() && g_segFeet.back().lo == va && g_segFeet.back().hi == va + 4u * n) {
      own.push_back(g_segFeet.back());
      g_segFeet.pop_back();
    }
    for (const SegFoot& f : g_segFeet)
      if (f.hi <= va) g_segCoherDropOld.fetch_add(1, std::memory_order_relaxed);
  }
  auto sp = buildParts(va, va + 4u * n, own);
  const uint32_t phys = (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu);
  g_segCoher.fetch_add(1, std::memory_order_relaxed);
  // GpuCmdBufAllocate(dev, 4, 4): the 4-dword tail hole the chain block would use; then the publish.
  ctx.r3.u64 = dr.dev;
  ctx.r4.u64 = 4u;
  ctx.r5.u64 = 4u;
  sub_820FCDF0(ctx, dr.base);
  const uint32_t va4 = ctx.r3.u32;
  if (!va4) return;
  publishSegment(dr, va4, phys, n, sp, 0u, 0u);  // the coherency block carries no fence (and no progress word)
}

// GpuCommandSegmentFlush_820FD660 mode B (pinned: the Z-pass list record) / mode C (the fence tail +
// the publish), then the tail: the same buffer continues at the next 32-byte boundary, or a new segment.
void segmentClose(Driver& dr, bool pinned) {
  PPCContext& ctx = *dr.ctx;
  uint8_t* d = dr.d;
  const uint32_t seg = be32(d + 0x34C4u);
  const uint32_t end = be32(d + 40) + 4u;
  const uint32_t nDw = (end - seg) >> 2;
  const uint32_t phys = (((seg >> 20) + 0x200u) & 0x1000u) + (seg & 0x1FFFFFFFu);
  uint32_t after = end;
  if (!pinned) {
    // GpuBuildFenceWritePacket(dev, end): S, the progress word, the device bookkeeping; the tail as a record.
    const uint32_t gen = be32(d + 0x34BCu), cursor = be32(d + 40), S = be32(d + 0x2A1Cu), fb = be32(d + 0x2A10u);
    put32(d + 0x329Cu, gen);
    put32(d + 0x3298u, cursor);
    const uint32_t progress = (gen & 3u) | cursor;
    auto physOf = [](uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); };
    dr.spanStart = end - 4u;
    dr.c = end - 4u;
    dr.nItems = 0;
    Item* o = dr.reserve(1);
    Driver::packet(o[0], uint8_t(kRkFence), 0u, {physOf(fb + 4u) | 2u, progress, physOf(fb) | 2u, S});
    o[0].fixedBytes = 40u;
    dr.commit(1, 40u);
    dr.opaque = false;
    dr.closeSpan();  // the record + its foot [end, end + 40)
    put32(d + 0x2A1Cu, S + 2u);
    if (sr::native::p2trace::g_on) sr::native::p2trace::event(sr::native::p2trace::kClose, 0, S, cursor, be32(d + 0x3298u));
    const uint32_t cursorEnd = end + 40u;
    std::vector<SegFoot> feet;
    {
      std::lock_guard<std::mutex> lk(g_segMu);
      feet = takeFeetLocked();  // every footprint written since the previous close: this segment's
    }
    auto sp = buildParts(seg, cursorEnd, feet);
    recycleFeet(feet);  // I7 design (e)
    // The PM4 census (observe-only): the closed range's stamps moved with the copy; the fence record's bytes the span
    // just wrote are the close's own - the open bracket piece restarts past them (nothing later stamps the range).
    sr::native::restartPiece(be32(d + 40));
    after = publishSegment(dr, cursorEnd, phys, (cursorEnd - seg) >> 2, sp, S, progress);  // carries S and its progress
  } else {
    g_segPinned.fetch_add(1, std::memory_order_relaxed);
    std::vector<SegFoot> feet;
    {
      std::lock_guard<std::mutex> lk(g_segMu);
      feet = takeFeetLocked();
    }
    if (!nDw && !feet.empty()) buildParts(seg, end, feet);  // an empty segment holding footprints: footStray
    if (nDw) {
      // The mode-B record {0x82000000 | n, phys} in the scratch list dev+0x32C8 (the pump replays it
      // through 820FD218, which finds the parts registered here).
      const uint32_t list = dr.dev + 0x32C8u;
      uint32_t cur = be32(d + 0x32C8u + 8u);
      const uint32_t lim = be32(d + 0x32C8u + 0xCu);
      if (cur + 8u > lim) {
        ctx.r3.u64 = list;
        sub_820FC978(ctx, dr.base);
        cur = ctx.r3.u32;
      }
      sr::kernel::writeGuestBE32(cur + 4u, phys);
      sr::kernel::writeGuestBE32(cur, 0x82000000u | nDw);
      put32(d + 0x32D0u, cur + 8u);
      auto sp = buildParts(seg, end, feet);
      sr::native::restartPiece(be32(d + 40));  // the census: as above (the closed range's stamps moved with the copy)
      registerParts(phys, nDw, sp, /*deferred=*/true, /*passes=*/2);  // the pump replays the list twice through 820FD218
      if (g_bracket) bracketNoteClose(dr.dev, cur, nDw, phys, sp);    // p2.transport BRACKET: bound in order
    }
    recycleFeet(feet);  // I7 design (e)
  }
  if (g_segTile) {  // SR_NATIVE_SEG_TILE: this close took every footprint, so every record below the head is claimed
    std::lock_guard<std::mutex> lk(g_tileMu);
    g_tile.closeAt(g_head);
  }
  // The tail. (The PM4 census, observe-only: neither a chained close's list node nor the never-written alignment pad
  // is stamped by the switch's cut or the flush's leave - the open bracket piece restarts past them.)
  const uint32_t v = (after + 31u) & ~31u;
  if (v > be32(d + 48)) {
    put32(d + 40, after - 4u);
    sr::native::restartPiece(after - 4u);
    ctx.r3.u64 = dr.dev;
    ctx.r4.u64 = 0;
    sub_820FCF90(ctx, dr.base);  // GpuCmdBufBeginNewSegment: the reserve and its waits, a guest computation
  } else {
    put32(d + 0x34C4u, v);
    put32(d + 40, v - 4u);
    sr::native::restartPiece(v - 4u);
  }
}

}  // namespace

#if defined(SR_DIFFTEST)
uint32_t publishSegmentForTest(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t cursorEnd, uint32_t phys,
                               uint32_t dwords, uint32_t fence, uint32_t progress) {
  Driver dr;
  dr.ctx = &ctx;
  dr.base = base;
  dr.dev = dev;
  return publishSegment(dr, cursorEnd, phys, dwords, std::make_shared<SegParts>(), fence, progress);
}
#endif

// GpuCommandBufferFlush_820FD8C0(dev): the close. false = a whole-body guest mode (no-GPU, the
// command-buffer device, the no-space scratch mode, PIX attached, HSIO failed) or the gate off.
bool flushSegmentless(PPCContext& ctx, uint8_t* base) {
  if (!g_segmentless || !g_on || g_presentGateOracleDepth) return false;
  if (!segmentlessActive()) {  // an OFF window: the guest close, unless a marker-less span is open in this segment
    std::lock_guard<std::mutex> lk(g_segMu);
    if (g_segFeet.empty()) return false;
  }
  const uint32_t dev = ctx.r3.u32;
  uint8_t* d = dev ? sr::kernel::guestVAToHost(dev) : nullptr;
  if (!d) return false;
  const uint8_t b38 = d[0x2A38u], b39 = d[0x2A39u];
  if ((b38 & 0x80u) || (b39 & 0x42u) || be32(d + 0x4DF4u) || be32(d + 0x4E84u) ||
      sr::kernel::readGuestU32BE(0x829AC0ECu) != 0) {
    g_segGuestMode.fetch_add(1, std::memory_order_relaxed);
    // The guest close publishes the segment's memory as PM4, where the pending footprints' records were never
    // written: they are lost (feetLost, must stay 0) and must not leak into the next segment's close.
    std::lock_guard<std::mutex> lk(g_segMu);
    if (!g_segFeet.empty()) {
      g_segFeetLost.fetch_add(g_segFeet.size(), std::memory_order_relaxed);
      g_segFeet.clear();
    }
    return false;
  }
  const bool pinned = be32(d + 0x3290u) != 0;
  if (pinned && !g_segWasPinned) segmentBracketBegin();  // a new Z-pass bracket: older brackets' entries expire
  g_segWasPinned = pinned;
  // The arena: draw() / Entry::open() allocate it under g_mu, but the first close of a run comes from
  // D3DDeviceCreate's init block before any draw (p2c_abp crashed there: the record writer at a null arena,
  // "write at data address 0x18"). The producers are the owner thread, so no lock is needed here (and the
  // kick inside draw() arrives with g_mu held: taking it would deadlock).
  if (!g_arena) {
    g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_arena) return false;
  }
  thread_local Item t_items[4];
  Driver dr;
  dr.ctx = &ctx;
  dr.base = base;
  dr.d = d;
  dr.dev = dev;
  dr.items = t_items;
  dr.c = be32(d + 40);
  dr.spanStart = dr.c;
  if (!pinned) coherencyBlock(dr);
  segmentClose(dr, pinned);
  ctx.r3.u64 = be32(d + 40);  // returns dev+0x28
  if ((g_segClosed.fetch_add(1, std::memory_order_relaxed) & 2047u) == 2047u) segReport();
  return true;
}

void segTileDrained() {
  if (!g_segTile) return;
  if (g_segWasPinned) {  // inside a Z-pass bracket: its deferred segments wait for the guest's own replay
    g_tileDrainBracket.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  sr::native::segtile::TileCounters t;
  sr::native::segtile::FirstCounters f;
  {
    std::lock_guard<std::mutex> lk(g_tileMu);
    t = g_tile.counters();
    f = g_first.counters();
  }
  g_tileDrains.fetch_add(1, std::memory_order_relaxed);
  if (t.segs == f.segs && t.recs == f.recs) return;
  const uint64_t n = g_tileDrainMiss.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NATIVE_FLUSH] seg tile drain MISS #%llu: BlockUntilIdle returned with registered segs=%llu recs=%llu, "
                "first-applied segs=%llu recs=%llu", (unsigned long long)n, (unsigned long long)t.segs,
                (unsigned long long)t.recs, (unsigned long long)f.segs, (unsigned long long)f.recs);
}

}  // namespace sr::native::flush

// The close, overridden whole under SR_NATIVE_SEGMENTLESS; the guest body otherwise (the N0 scope
// instruments both). SR_AB_PRESENT_DRAINED (diagnostic, iter-467 native-a): the per-present A/B flip that
// onGuestSwap left pending commits after a close once the stream is quiesced (abDrainAfterClose). Without it
// sr::abt::drainedMode() is a latched false and the close is exactly the two lines it always was.
extern "C" void __imp__sub_820FD8C0(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_820FD8C0) {
  SR_NATIVE_SCOPE(0x820FD8C0);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const uint32_t dev = ctx.r3.u32;
  if (!(sr::native::flush::segmentlessGate() && sr::native::flush::flushSegmentless(ctx, base))) {
    __imp__sub_820FD8C0(ctx, base);
    if (sr::native::flush::bracketOn()) sr::native::flush::bracketGuestClose(dev);  // p2.transport BRACKET
  } else
    sr::native::flush::segTileForcedDrain(dev);  // SR_NATIVE_SEG_TILE's forced drained point (a no-op otherwise)
  if (sr::abt::drainedMode()) sr::native::flush::abDrainAfterClose(dev);
}

namespace sr::native::flush {
namespace {

// P1 row 5: D3DDeviceEmitScissorTileRects_820F0208(dev, x0, y0, x1, y1) as an Entry. SR_NATIVE_TILERECTS,
// default OFF until proven; =0 keeps the A/B (SR_AB_TOGGLE feature of the same name). It is the one
// emitter behind SetScissorRect 820F2720 (and SetViewport / the render-target setters through it), also
// called by the segment re-emit 820F0048 (DeviceCreate, EndTiling, InvokeRenderPass, Swap), BeginTiled
// 820FAEE0, BeginTiling 8210D588, BeginZPass 8210DF38 and strand B's producers (ClearFull, ClearImpl,
// Resolve), whose own entries never nest it (B: the guest body runs only when their entry declined).
// From the asm: the entry kick check (r3 = the flush's cursor when it fires, dev otherwise); tiled
// (byte 10808 & 0x10, or & 0x20 with the five surfaces dev+12168.. equal to their shadows dev+12452..
// or zero): SET_BIN_MASK_HI {0}; per tile i < dev+12472 (rect dev+12476+16i {l, t, r, b}, offset
// dev+12716+8i {x, y}; signed compares): x0' = max(x0, l), y0' = max(y0, t), x1' = min(x1, r),
// y1' = min(y1, b), all zero when x0' >= x1' or y0' >= y1'; SET_BIN_MASK_LO {3 << 2i}, the predicated
// SET_CONSTANT {0x40080, (-y & 0x7FFF) << 16 | (-x & 0x7FFF), TL, BR}, r3 = 3, the kick check; then
// (byte 10811 & 0x20 and byte 10808 & 0x40) SET_BIN_MASK_LO {1} + SET_CONSTANT {0x40081, TL, BR} of the
// unclamped rect; LO {dev+12432} / HI {dev+12436}. Untiled: the run {0x2080: 0, TL, BR}. TL / BR =
// (y & 0x3FFF) << 16 | (x & 0x3FFF).
// The gate as a zero-arg helper (scripts/gen_override_manifest.py follows one such call from the
// override body to find the gate; a file-scope flag read inside the leaf is invisible to it).
bool tileRectsOn() {
  // Default ON since 2026-10-10 (the orchestrator's go; proof p1.row5 on exe d855d0e75a9b, a drawn world: dry
  // CHECK 0 BAD, CPPM4 11/11, stress 12/12, frozen-pass A/B vs the PM4 path PASS under rule R with 440 static
  // pairs, flicker gate PASS, meter -107.9 pm4 per ingame frame with row 3b); =0 keeps the A/B.
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_TILERECTS");
    return !e || e[0] != '0';
  }();
  return on;
}

bool emitScissorTileRects(PPCContext& ctx, uint8_t* base) {
  // Both scoped SDK oracles (the orchestrator's decision, 2026-10-14, transport-composed lane): inside
  // PresentGateOracle every body is the SDK's (a composed oracle's Swap re-emit 820F0048 -> here, BeginZPass's tile
  // rects), so the oracle stays pure; inside ShaderConstRunsOracle an Entry on this thread already holds g_mu (not
  // re-entrant), where opening another would spin forever. Outside both scopes nothing changes.
  if (!tileRectsOn() || !g_on || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;
  static const int s_ab = sr::abt::reg("SR_NATIVE_TILERECTS");
  if (s_ab >= 0 && !sr::abt::on(s_ab)) return false;
  const uint32_t dev = ctx.r3.u32;
  const uint8_t* d = dev ? sr::kernel::guestVAToHost(dev) : nullptr;
  if (!d) return false;
  const int32_t x0 = ctx.r4.s32, y0 = ctx.r5.s32, x1 = ctx.r6.s32, y1 = ctx.r7.s32;
  const uint32_t c0 = be32(d + 40), lim0 = be32(d + 44), thr0 = be32(d + 48);
  Entry e(ctx, base, dev);
  if (!e.open()) {
    g_tileRectDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint64_t n = g_tileRectCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (d[10808] & 0x30u) g_tileRectTiled.fetch_add(1, std::memory_order_relaxed);
  uint32_t r3 = dev;
  if (!e.tileRects(x0, y0, x1, y1, r3)) {  // unreachable in record mode (reserve never fails there)
    e.abandon();
    g_tileRectDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  e.close();
  ctx.r3.u64 = r3;
  if (g_leafTrace && leafTraceDue(n))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[LEAF] 820F0208 #%llu tid=%lu rect=(%d,%d)-(%d,%d) b08=%02X b0B=%02X tiles=%u c0=%08X lim=%08X thr=%08X -> c=%08X r3=%08X lr=%08X",
                (unsigned long long)n, (unsigned long)GetCurrentThreadId(), x0, y0, x1, y1, d[10808], d[10811],
                be32(d + 12472), c0, lim0, thr0, be32(d + 40), r3, uint32_t(ctx.lr));
  return true;
}

}  // namespace

uint64_t tileRectCalls() { return g_tileRectCalls.get(); }
uint64_t tileRectTiled() { return g_tileRectTiled.get(); }
uint64_t tileRectDecl() { return g_tileRectDecl.get(); }

}  // namespace sr::native::flush

// The tile-rect emitter (P1 row 5), under SR_NATIVE_TILERECTS; the guest body otherwise, bracketed by the
// dry CHECK (kind 5) under SR_NATIVE_FLUSH_CHECK: the items the leaf would write, rendered as PM4, against
// the bytes the guest body wrote.
// iter-467 native-b (row 9 on dev, where row 5 is baked ON): while the row-9 dry CHECK (SR_NB_REC_CHECK) compares a
// guest Resolve / ClearF / ClearFullSurface body, which calls this leaf for its window-scissor restore, the leaf
// stays on the guest path (sr::native::recb::checkArmed(), kernel/native_rec_b.hpp) - else the CHECK sees records
// where the XDK wrote PM4.
extern "C" void __imp__sub_820F0208(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_820F0208) {
  SR_NATIVE_SCOPE(0x820F0208);
  if (sr::native::flush::tileRectsOn() && !sr::native::recb::checkArmed() &&
      sr::native::flush::emitScissorTileRects(ctx, base))
    return;
  const bool chk = sr::native::flush::checkOn();
  if (chk) sr::native::flush::checkBefore(ctx, base, 5);
  __imp__sub_820F0208(ctx, base);
  if (chk) sr::native::flush::checkAfter(ctx, base, 5);
}

namespace sr::native::flush {
namespace {

// P1 row 3b: D3DDeviceSetPredication_8210D400(dev, mask) as an Entry (Driver::setPredication: the device
// writes, the kick check, SET_BIN_MASK_LO {dev+12432}). SR_NATIVE_TILEPRED, default OFF until proven; =0
// keeps the A/B. Callers: game code (RwDrawInstancedConstBatched 8261B050, Lion SetPerTileConstC100
// 827333E0, RenderScriptRunTechnique 8277B998) and the XDK (ClearF, EndTiling, BeginZPass, EndZPass).
bool tilePredOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  // Default ON since 2026-10-10 with row 5 (the orchestrator's go; proof p1.row3b = the rows 5+3b plan r5b on
  // exe d855d0e75a9b: the same rolls as tileRectsOn's comment); =0 keeps the A/B.
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_TILEPRED");
    return !e || e[0] != '0';
  }();
  return on;
}
SR_SW g_tilePredCalls, g_tilePredDecl;

bool setPredicationLeaf(PPCContext& ctx, uint8_t* base) {
  // Both scoped SDK oracles, as emitScissorTileRects (row 5) above: the decline happens only inside a scope.
  if (!tilePredOn() || !g_on || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;
#if defined(SR_DIFFTEST)
  if (t_fsGuestOnly) return false;  // the split suite's SDK arm (8261B050's own calls): the XDK body
#endif
  static const int s_ab = sr::abt::reg("SR_NATIVE_TILEPRED");
  if (s_ab >= 0 && !sr::abt::on(s_ab)) return false;
  const uint32_t dev = ctx.r3.u32;
  if (!dev || !sr::kernel::guestVAToHost(dev)) return false;
  const uint8_t* d = sr::kernel::guestVAToHost(dev);
  const uint32_t c0 = be32(d + 40), b08 = d[10808];
  Entry e(ctx, base, dev);
  if (!e.open()) {
    g_tilePredDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint64_t n = g_tilePredCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t r3 = 0;
  if (!e.setPredication(ctx.r4.u32, r3)) {  // unreachable in record mode: the device writes are done
    e.abandon();                             // (idempotent; the guest body repeats them)
    g_tilePredDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  e.close();
  ctx.r3.u64 = r3;
  if (g_leafTrace && leafTraceDue(n))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[LEAF] 8210D400 #%llu tid=%lu mask=%08X b08=%02X->%02X b0B=%02X m12432=%08X p12440=%u c0=%08X -> c=%08X r3=%08X lr=%08X",
                (unsigned long long)n, (unsigned long)GetCurrentThreadId(), ctx.r4.u32, b08, d[10808], d[10811],
                be32(d + 12432), be32(d + 12440), c0, be32(d + 40), r3, uint32_t(ctx.lr));
  return true;
}

}  // namespace

uint64_t tilePredCalls() { return g_tilePredCalls.get(); }
uint64_t tilePredDecl() { return g_tilePredDecl.get(); }

}  // namespace sr::native::flush

// SetPredication (P1 row 3b), under SR_NATIVE_TILEPRED; the guest body otherwise, bracketed by the dry
// CHECK (kind 6) under SR_NATIVE_FLUSH_CHECK. (The N0 bracket shim of 8210D400 is this override now.)
extern "C" void __imp__sub_8210D400(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8210D400) {
  SR_NATIVE_SCOPE(0x8210D400);
  if (sr::native::flush::tilePredOn() && sr::native::flush::setPredicationLeaf(ctx, base)) return;
  const bool chk = sr::native::flush::checkOn();
  if (chk) sr::native::flush::checkBefore(ctx, base, 6);
  __imp__sub_8210D400(ctx, base);
  if (chk) sr::native::flush::checkAfter(ctx, base, 6);
}

namespace sr::native::flush {

// ---- p2.zpd's bake (native/zpd-bake, 2026-10-16; docs/NATIVE_ROADMAP.md "p2.zpd BAKE PLAN") ----
// GpuEmitTileSurfaceConfig_82101E08(dev, query, which) as an Entry (Driver::zpdSurface): the occlusion query's packets
// D3DQueryIssueBeginEnd_82102BC8 emits for a BEGIN (which 1) and an END (which 0) - its only caller; the queries are
// HudGlowOcclusionQuery_82304058's, through the 82779A28 / 82779A38 wrappers - with EVENT_WRITE_ZPD as the kRkZpd record
// the CP applies through RecordEngine::applyZpdEvent (the method the 0x5B packet reaches: the host occlusion query of
// gfx/native_zpd). The gate is SR_NATIVE_ZPD itself, BAKED default ON in the latency model: unset = ON, =0 keeps the
// guest body and the guest path (the 0x5B packet as guest PM4, no host query) - the A/B's OFF arm; the per-present A/B
// (SR_AB_TOGGLE=SR_NATIVE_ZPD) toggles THIS PORT only, the answering stays native in both arms (the mode is latched:
// guest-visible answers). Before the bake the queries' packets were the default build's last literal PM4 in Metropolis
// (chapters 3-18, the stage-1 lane's census).
#if defined(SR_DIFFTEST)
namespace {
std::atomic<int> g_zpdPortTest{-1};  // the difftest's override: -1 = the env gate, 0 off, 1 on
}
void setZpdPortForTest(int v) { g_zpdPortTest.store(v, std::memory_order_relaxed); }
#endif
bool zpdPortOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_ZPD");  // `e`: scripts/audit_forcing.py reads `!e ||` as default ON
    return !e || e[0] != '0';
  }();
#if defined(SR_DIFFTEST)
  if (const int t = g_zpdPortTest.load(std::memory_order_relaxed); t >= 0) return t != 0;
#endif
  return on && sr::gfx::zpd::on();
}

namespace {
SR_SW g_zpdPortCalls, g_zpdPortDecl;

bool zpdSurfaceLeaf(PPCContext& ctx, uint8_t* base) {
  // The scoped SDK oracles, as setPredicationLeaf: inside PresentGateOracle every transport body is the SDK's, inside
  // ShaderConstRunsOracle an Entry already owns the arena lock.
  if (!g_on || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;
  static const int s_ab = sr::abt::reg("SR_NATIVE_ZPD");
  if (s_ab >= 0 && !sr::abt::on(s_ab)) {
    sr::gfx::zpd::notePort(false);  // the A/B's guest arm: counted, the guest body runs
    return false;
  }
  const uint32_t dev = ctx.r3.u32, query = ctx.r4.u32, which = ctx.r5.u32;
  if (!dev || !sr::kernel::guestVAToHost(dev) || !query || !sr::kernel::guestVAToHost(query) ||
      !sr::kernel::guestVAToHost(query + 151u)) {
    g_zpdPortDecl.fetch_add(1, std::memory_order_relaxed);
    sr::gfx::zpd::notePort(false);
    return false;
  }
  Entry e(ctx, base, dev);
  if (!e.open()) {
    g_zpdPortDecl.fetch_add(1, std::memory_order_relaxed);
    sr::gfx::zpd::notePort(false);
    return false;
  }
  const uint64_t n = g_zpdPortCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t r3 = 0;
  if (!e.zpdSurface(query, which, r3)) {  // unreachable in record mode (no write before the first kick check fails)
    e.abandon();
    g_zpdPortDecl.fetch_add(1, std::memory_order_relaxed);
    sr::gfx::zpd::notePort(false);
    return false;
  }
  e.close();
  ctx.r3.u64 = r3;
  sr::gfx::zpd::notePort(true);
  if (g_leafTrace && leafTraceDue(n)) {
    const uint8_t* d = sr::kernel::guestVAToHost(dev);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[LEAF] 82101E08 #%llu tid=%lu q=%08X which=%u tiles=%u -> c=%08X r3=%08X lr=%08X",
                (unsigned long long)n, (unsigned long)GetCurrentThreadId(), query, which,
                sr::kernel::readGuestU32BE(query + 144u), be32(d + 40), r3, uint32_t(ctx.lr));
  }
  return true;
}

}  // namespace

uint64_t zpdPortCalls() { return g_zpdPortCalls.get(); }
uint64_t zpdPortDecl() { return g_zpdPortDecl.get(); }

}  // namespace sr::native::flush

// GpuEmitTileSurfaceConfig 82101E08 (p2.zpd's bake), under SR_NATIVE_ZPD (default ON); the guest body otherwise,
// bracketed by the dry CHECK (kind 11) under SR_NATIVE_FLUSH_CHECK. No N0 scope: 82101E08 is no bracket-table entry.
extern "C" void __imp__sub_82101E08(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82101E08) {
  if (sr::native::flush::zpdPortOn() && sr::native::flush::zpdSurfaceLeaf(ctx, base)) return;
#if SR_ORACLE
  const bool chk = sr::native::flush::checkOn();
  if (chk) sr::native::flush::checkBefore(ctx, base, 11);
  __imp__sub_82101E08(ctx, base);
  if (chk) sr::native::flush::checkAfter(ctx, base, 11);
#else
  sr::native::flush::nativeAdmissionFatal("ZPD query producer declined native records");
#endif
}

namespace sr::native::flush {
namespace {

// P2-B item 5: D3DDeviceEmitPm4Packet_82113010(dev, type, index, count) as an Entry (Driver::setConstMem:
// the reserve, the pad / header / offset and the caller-filled body as a kRkSetConstMem record; the CP reads
// the values at apply). SR_NATIVE_SETCONSTMEM, default ON since 2026-10-14 (the bake; proof p2.setconst_mem =
// plan setconstmem, scm4 on exe 42f048a64456: replay 9/9, dry CHECK kind 7 0 BAD, the frozen-pass A/Bs on the ring
// path and under segmentless, the records-as-PM4 freeze, the flicker gate, the meter's SET_CONSTANT kind 6.9 -> 0.0
// per ingame frame under segmentless (pm4 -15.4); the live bake proof: native_proof --plan setconstmem_bake).
// =0 keeps the guest body for the A/B, in the release configuration too: SR_ORACLE=0 compiles out the in-roll
// A/B toggle and the dry CHECK, not this gate (as for the earlier bakes). Its only caller
// in this title is Lion's per-tile c100 (827333E0: VS then PS per tile, 16 bytes each, between its
// SetPredication calls); the census counted 38.8 packets per frame (25.4 NOP pads + 13.4 SET_CONSTANT).
// On the ring path the span is literal (the XDK's bytes written by the host, the body by the caller) and
// the meter keeps counting it; the record form - the meter's drop - exists only under segmentless.
bool setConstMemOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_SETCONSTMEM");
    return !e || e[0] != '0';
  }();
  return on;
}

bool emitPm4PacketLeaf(PPCContext& ctx, uint8_t* base) {
  if (!setConstMemOn() || !g_on) return false;
  static const int s_ab = sr::abt::reg("SR_NATIVE_SETCONSTMEM");
  if (s_ab >= 0 && !sr::abt::on(s_ab)) return false;
  const uint32_t dev = ctx.r3.u32;
  if (!dev || !sr::kernel::guestVAToHost(dev)) return false;
  ensureSetConstMemHandlers();
  Entry e(ctx, base, dev);
  if (!e.open()) {
    g_setConstMemDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  uint32_t body = 0;
  if (!e.setConstMem(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, body)) {  // unreachable in record mode (reserve never fails there)
    e.abandon();
    g_setConstMemDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  e.close();
  g_setConstMemCalls.fetch_add(1, std::memory_order_relaxed);
  if (!body) g_setConstMemFail.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = body;
  return true;
}

}  // namespace

uint64_t setConstMemCalls() { return g_setConstMemCalls.get(); }
uint64_t setConstMemDecl() { return g_setConstMemDecl.get(); }

bool segmentlessActiveNow() { return segmentlessActive(); }
bool flushSplitOn() { return g_split; }
bool flushSplitActive() { return flushSplitWindow(); }

// SR_AB_PRESENT_DRAINED (see ab_toggle.hpp): after a close on the producer thread, commit the pending
// per-present flip once nothing of the old mode is open or in flight - outside a Z-pass bracket (dev+0x3290),
// no open footprint and no incomplete deferred segment (g_segMu), the CP's ring read index (fb+0x3C, fb =
// dev+0x2A10) at the write index (dev+0x2A44) and every arena record executed (g_cpTail >= g_head). The CP is
// given up to 200 ms; otherwise the flip stays pending for the next close ([AB_DRAIN] postponed).
void abDrainAfterClose(uint32_t dev) {
  if (!sr::abt::pendingFlip()) return;
  static uint64_t s_close = 0;  // the producer thread only
  const uint64_t closeNo = ++s_close;
  const uint8_t* d = dev ? sr::kernel::guestVAToHost(dev) : nullptr;
  if (!d) return;
  if (be32(d + 0x3290u) != 0) {
    sr::abt::postponePending("pinned");
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (int spin = 0;; spin++) {
    bool quiet;
    {
      std::lock_guard<std::mutex> lk(g_segMu);
      quiet = g_segFeet.empty();
      for (const auto& e : g_segDeferred)
        if (!e->complete()) {
          quiet = false;
          break;
        }
    }
    const uint32_t fb = be32(d + 0x2A10u);
    const bool ring = fb && sr::kernel::readGuestU32BE(fb + 0x3Cu) == be32(d + 0x2A44u);
    const bool cp = g_cpTail.load(std::memory_order_acquire) >= g_head;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (quiet && ring && cp) {
      sr::abt::commitPending(closeNo, us);
      return;
    }
    if (us > 200000.0) {
      sr::abt::postponePending(!quiet ? "segments open" : !ring ? "ring not drained" : "records not executed");
      return;
    }
    if (spin < 64) _mm_pause();
    else std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

// SR_NATIVE_SEG_TILE's FORCED drained point (p2.segmentless, Astra 2026-10-13: gameplay had no drained
// reconciliation - the game calls BlockUntilIdle only while it loads). After a segmentless close on the producer
// thread, every SR_NATIVE_SEG_TILE_DRAIN_MS (default 10000; 0 = off): outside a Z-pass bracket, wait (<= 200 ms) for
// the drained state abDrainAfterClose uses - no open footprint, no incomplete deferred segment, the CP's ring read
// index at the write index, every arena record executed - then compare registered with first-applied. Not drained in
// time, or inside a bracket: postponed, the next close retries. Each point is logged and, under SR_P2_TRACE, traced
// (kDrained) for tools/perf/p2_trace.py's terminal reconciliation.
void segTileForcedDrain(uint32_t dev) {
  if (!g_segTile) return;
  static const uint64_t s_periodMs = [] {
    const char* e = std::getenv("SR_NATIVE_SEG_TILE_DRAIN_MS");
    return e ? std::strtoull(e, nullptr, 10) : 10000ull;
  }();
  if (!s_periodMs) return;
  using clk = std::chrono::steady_clock;
  static clk::time_point s_due = clk::now() + std::chrono::milliseconds(s_periodMs);  // the producer thread only
  if (clk::now() < s_due) return;
  const uint8_t* d = dev ? sr::kernel::guestVAToHost(dev) : nullptr;
  if (!d) return;
  if (g_segWasPinned || be32(d + 0x3290u) != 0) {  // a Z-pass bracket's deferred segments wait for the guest's replay
    g_tileForcedPostponed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto t0 = clk::now();
  double us = 0.0;
  for (int spin = 0;; spin++) {
    bool quiet;
    {
      std::lock_guard<std::mutex> lk(g_segMu);
      quiet = g_segFeet.empty();
      for (const auto& e : g_segDeferred)
        if (!e->complete()) {
          quiet = false;
          break;
        }
    }
    const uint32_t fb = be32(d + 0x2A10u);
    const bool ring = fb && sr::kernel::readGuestU32BE(fb + 0x3Cu) == be32(d + 0x2A44u);
    const bool cp = g_cpTail.load(std::memory_order_acquire) >= g_head;
    us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    if (quiet && ring && cp) break;
    if (us > 200000.0) {
      g_tileForcedPostponed.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (spin < 64) _mm_pause();
    else std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  s_due = clk::now() + std::chrono::milliseconds(s_periodMs);
  sr::native::segtile::TileCounters t;
  sr::native::segtile::FirstCounters f;
  {
    std::lock_guard<std::mutex> lk(g_tileMu);
    t = g_tile.counters();
    f = g_first.counters();
  }
  const bool match = t.segs == f.segs && t.recs == f.recs;
  const uint64_t n = g_tileForced.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!match) g_tileForcedMiss.fetch_add(1, std::memory_order_relaxed);
  if (sr::native::p2trace::g_on)
    sr::native::p2trace::event(sr::native::p2trace::kDrained, match ? 1u : 0u, (uint32_t)t.segs, (uint32_t)f.segs,
                               (uint32_t)(t.recs - f.recs));
  SR_LOG_BOOT(match ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
              "[NATIVE_FLUSH] seg tile forced drain #%llu: registered segs=%llu recs=%llu, first-applied segs=%llu "
              "recs=%llu, drained in %.0f us: %s", (unsigned long long)n, (unsigned long long)t.segs,
              (unsigned long long)t.recs, (unsigned long long)f.segs, (unsigned long long)f.recs, us,
              match ? "MATCH" : "MISS");
}

// ---- boot.device_create (native-a, 2026-10-10): SR_NATIVE_DEVCREATE, default ON since native/devcreate-fix ----
// Not in the transport bake (native/transport-bake, 2026-10-15): boot.device_create's bake is its own record's. Its
// boot_order_gap unknown (dcc4: the backend attached 20-36 s late in 2 of 3 ON boots) was root-caused on 2026-10-15
// (NATIVE_ROADMAP "boot.device_create BAKE PLAN"): the installed shader cache's SYNCHRONOUS boot load from D: sat on
// the attach's critical path on those exes, in BOTH arms (dc5c census0, devcreate OFF: a 48.4 s load, 37.6 s wait),
// and left it with render/boot-attach (f9c08c2d). Baked default ON here: unset = ON, =0 keeps the guest bodies and the
// A/B arm. With the transport baked too, the default path's gameplay PM4 is 0: the guest 820F0048 re-emit under Swap -
// 6 PM4 a present (60 / 61 / 62 / 63, then the type-0 WAIT_UNTIL 0x5C8 and RB_BC_CONTROL 0xF01: IDA
// 0x820F0098-0x820F0110) - was the last producer on it.
bool devCreateOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_DEVCREATE");
    return !e || e[0] != '0';
  }();
  static const int s_ab = sr::abt::reg("SR_NATIVE_DEVCREATE");  // the per-present A/B of the Swap-side call
  // The scoped SDK oracles (transport-composed lane, 2026-10-14): inside PresentGateOracle every transport body is the
  // SDK's (the segmentless close itself declines there, so a native re-emit's records would land in a guest-closed
  // segment, never consumed); inside ShaderConstRunsOracle an Entry already owns the arena lock g_mu, and the leaf's own
  // Entry would wait on it. SWAP, SWAP_BLOCK, BRACKET and the residue decline in both, GATE and CALLBACK in the first;
  // devcreate in neither, so a composed oracle (SR_NATIVE_DEVCREATE=1) ran a native 820F0048 inside the SDK swap.
  return on && g_on && segmentlessActive() && !g_presentGateOracleDepth && !g_shaderConstRunsOracleDepth &&
         (s_ab < 0 || sr::abt::on(s_ab));
}

namespace {
std::atomic<uint64_t> g_devResetCalls{0}, g_devResetDecl{0}, g_segBeginCalls{0}, g_segBeginDecl{0};
}

// D3DDeviceResetState_8210E520(dev): the device writes (devResetFields), then the packet tail as records. Declines
// (nothing written) when the Entry declines; the field writes are idempotent, so a tail that declines after them
// (unreachable in record mode) leaves the guest body re-applying the same values.
bool devResetLeaf(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  Entry e(ctx, base, dev);
  if (!e.open()) {
    g_devResetDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  uint8_t* d = sr::kernel::guestVAToHost(dev);
  if (!d) {
    e.abandon();
    g_devResetDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  devResetFields(d);
  if (!e.devResetPackets()) {
    e.abandon();
    g_devResetDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  e.close();
  g_devResetCalls.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// D3DDeviceCmdBufSegmentBeginReemitState_820F0048(dev, r4): with r4 == 0 its packets as records (one Entry),
// then D3DDeviceEmitScissorTileRects_820F0208 - row 5's own entry, at the XDK's own call boundary, after this
// Entry closed (two entries never nest) - then the dirty masks dev+0..+39 = ~0, as the asm. r3 = the tile
// rects' r3, as the asm leaves it.
bool segmentBeginLeaf(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, mode = ctx.r4.u32;
  if (!sr::kernel::readGuestU32BE(dev + 40)) return false;  // no command buffer: the guest body (its R1 hook skips the chain)
  Entry e(ctx, base, dev);
  if (!e.open()) {
    g_segBeginDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (mode == 0) {
    if (!e.segmentBeginPackets()) {
      e.abandon();
      g_segBeginDecl.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    e.close();
  } else {
    e.abandon();  // nothing of this entry's: the tile rects and the masks only
  }
  const uint32_t x = sr::kernel::readGuestU32BE(dev + 10308), y = sr::kernel::readGuestU32BE(dev + 10312);
  ctx.r3.u64 = dev;
  ctx.r4.u64 = uint64_t(int64_t(lo15(x)));
  ctx.r5.u64 = uint64_t(int64_t(hi15(x)));
  ctx.r6.u64 = uint64_t(int64_t(lo15(y)));
  ctx.r7.u64 = uint64_t(int64_t(hi15(y)));
  sub_820F0208(ctx, base);
  for (uint32_t k = 0; k < 40; k += 4) sr::kernel::writeGuestBE32(dev + k, 0xFFFFFFFFu);
  g_segBeginCalls.fetch_add(1, std::memory_order_relaxed);
  return true;
}
uint64_t devResetCalls() { return g_devResetCalls.load(std::memory_order_relaxed); }
uint64_t segBeginCalls() { return g_segBeginCalls.load(std::memory_order_relaxed); }
uint64_t devCreateDecl() {
  return g_devResetDecl.load(std::memory_order_relaxed) + g_segBeginDecl.load(std::memory_order_relaxed);
}

// ---- boot gaps (native/boot-gaps, 2026-10-16; docs/NATIVE_ROADMAP.md "BOOT GAPS BUILD NOTE") ----
// SR_NATIVE_MODE_STATE: default ON (=0 keeps the guest body and the per-present A/B arm). The device create's last
// literal PM4: GpuRingEmitModeStateReg_82103238, called once per title-device creation from
// D3DDeviceInitDefaultRenderStates_821058B0 (0x821059D4, its one code caller; IDA read-only, 2026-10-16), between
// D3DDeviceResetState 8210E520 and the create's flush at 0x821061D4 - the gap the stage-1 census found at frame 1
// (dcf2 census1: UNSTAMPED regs 05:1 0F:1, one literal-PM4 gap part of 16 bytes). The census cannot stamp it: the
// D3DDeviceCreate 820F34C0 bracket opens before the device exists. Record form: one Entry, the asm's order.
bool modeStateOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_MODE_STATE");
    return !e || e[0] != '0';
  }();
  static const int s_ab = sr::abt::reg("SR_NATIVE_MODE_STATE");
  return on && g_on && segmentlessActive() && !g_presentGateOracleDepth && !g_shaderConstRunsOracleDepth &&
         (s_ab < 0 || sr::abt::on(s_ab));
}

namespace {
std::atomic<uint64_t> g_modeStateCalls{0}, g_modeStateDecl{0};
}

// GpuRingEmitModeStateReg_82103238(dev r3, mode r4): dev+0x2A24 = (dev+0x2A24 & ~0x3000) | (mode & 3) << 12 (`insrwi r10,
// r4, 2, 18`, stored before the kick check), the kick check, WAIT_UNTIL / RB_BC_CONTROL. Rule 1: the decision (the gate,
// a command buffer, the Entry) precedes the device write; a packet failure after it (unreachable in record mode) abandons
// the Entry and the guest body re-stores the same value. r3 = the cursor the asm leaves in r3 and dev+0x28; r9 / r10 as
// the asm leaves them (0xF01, 0x20000); r11 (the new dev+0x2A24 re-read) is local to the generated body
// (non_argument_as_local, tools/regen/superman_regen.toml [localize]) - its ctx copy is not written, here as there.
bool modeStateLeaf(PPCContext& ctx, uint8_t* base) {
  if (!modeStateOn()) return false;  // the leaf's own gate (the override tests it first as well)
  const uint32_t dev = ctx.r3.u32, mode = ctx.r4.u32;
  if (!sr::kernel::readGuestU32BE(dev + 40)) {  // no command buffer: the guest body (it would write through it anyway)
    g_modeStateDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Entry e(ctx, base, dev);
  if (!e.open()) {
    g_modeStateDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t field = (sr::kernel::readGuestU32BE(dev + 10788) & ~0x3000u) | ((mode & 3u) << 12);
  sr::kernel::writeGuestBE32(dev + 10788, field);
  ShaderConstRunsOracle nested;  // the kick's segment switch under the arena lock: nested SDK emitters stay guest bodies
  if (!e.modeStatePackets()) {
    e.abandon();
    g_modeStateDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  e.close();
  ctx.r3.u64 = sr::kernel::readGuestU32BE(dev + 40);
  ctx.r9.u64 = 0x0F01u;
  ctx.r10.u64 = 0x00020000u;
  g_modeStateCalls.fetch_add(1, std::memory_order_relaxed);
  return true;
}
uint64_t modeStateCalls() { return g_modeStateCalls.load(std::memory_order_relaxed); }
uint64_t modeStateDecl() { return g_modeStateDecl.load(std::memory_order_relaxed); }

// SR_NATIVE_DISPLAY's moment test (the latched gate is displayGate(), kernel/native_display_emit.cpp): record mode, the
// native transport in its A/B window, outside the scoped SDK oracles (an Entry inside ShaderConstRunsOracle would spin on
// the arena lock its owner holds; inside PresentGateOracle the SDK's own bytes are the oracle's).
bool displayActive() {
  static const int s_ab = sr::abt::reg("SR_NATIVE_DISPLAY");
  return displayGate() && g_on && segmentlessActive() && !g_presentGateOracleDepth && !g_shaderConstRunsOracleDepth &&
         (s_ab < 0 || sr::abt::on(s_ab));
}
bool sdkOracleScope() { return g_presentGateOracleDepth || g_shaderConstRunsOracleDepth; }

}  // namespace sr::native::flush

namespace sr::native::flush {
namespace {
// p2.transport step 1: 820FDA80(dev r3, lastDw r4, flags r5, cb r6, arg r7) as an Entry at the leaf's cursor. Only a
// block in the OPEN SEGMENT just past the cursor (the vblank gate 82111EC8 calls it at dev+40 + 32, after its own
// SCRATCH_REG1 write and WAIT): the Z-pass kick 82110C78 passes a tail allocation (dev+0x34B8) published as its own
// jump segment - BRACKET's step, declined (the guest body writes it, the CP runs it as today). The decision (the gate,
// segmentless, the A/B mask) is read ONCE per call: one call is entirely a record or entirely the guest's bytes.
bool callbackLeaf(PPCContext& ctx, uint8_t* base) {
  // Both scoped SDK oracles (transport-composed lane, 2026-10-14): under ShaderConstRunsOracle an Entry owns the arena
  // spinlock g_mu, which this leaf's own Entry would spin on for ever. Unreachable today (no 820FDA80 caller runs nested
  // in an Entry), declined so that the oracle's contract is uniform across the transport leaves.
  if (g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;
  if (!g_on || !segmentlessActive()) return false;
  static const int s_ab = sr::abt::reg("SR_NATIVE_CALLBACK");
  if (s_ab >= 0 && !sr::abt::on(s_ab)) return false;
  const uint32_t dev = ctx.r3.u32, at = ctx.r4.u32;
  const uint8_t* d = dev ? sr::kernel::guestVAToHost(dev) : nullptr;
  if (!d) return false;
  const uint32_t cur = be32(d + 40);
  if (at < cur || at - cur > 64u || at + 4u == be32(d + 0x34B8u)) {
    g_cbDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  ensureCallbackHandlers();
  Entry e(ctx, base, dev);
  if (!e.openAt(at)) {
    g_cbDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  uint32_t last = 0;
  if (!e.callback(ctx.r6.u32, ctx.r7.u32, ctx.r5.u32, last)) {
    e.abandon();
    g_cbDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  e.close();
  g_cbCalls.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = last;
  return true;
}
}  // namespace

PresentGateOracle::PresentGateOracle() { ++g_presentGateOracleDepth; }
PresentGateOracle::~PresentGateOracle() { --g_presentGateOracleDepth; }
ShaderConstRunsOracle::ShaderConstRunsOracle() { ++g_shaderConstRunsOracleDepth; }
ShaderConstRunsOracle::~ShaderConstRunsOracle() { --g_shaderConstRunsOracleDepth; }

namespace {
uint8_t* gateSpan(uint32_t va, uint32_t bytes) {
  if (!va || !bytes || va > UINT32_MAX - (bytes - 1u)) return nullptr;
  uint8_t* first = sr::kernel::guestVAToHost(va);
  const uint8_t* last = sr::kernel::guestVAToHost(va + bytes - 1u);
  return first && last && last - first == ptrdiff_t(bytes - 1u) &&
                 sr::kernel::hostRangeReadableCached(first, bytes) ? first : nullptr;
}
bool gateNoKick(const uint8_t* d) {
  uint32_t threshold = be32(d + 48u);
  if (g_segStress) {
    const uint64_t stress = uint64_t(be32(d + 13508u)) + g_segStress;
    if (stress > UINT32_MAX) return false;
    threshold = std::min(threshold, uint32_t(stress));
  }
  const uint32_t cursor = be32(d + 40u);
  // The segment footprint also publishes finalCursor+4 as its exclusive end.
  return !(cursor & 3u) && cursor <= threshold && uint64_t(cursor) + 160u <= UINT32_MAX &&
         cursor + 156u <= be32(d + 44u) && gateSpan(cursor + 4u, 156u);
}
bool gateRead(uint32_t va, void* destination, uint32_t bytes) {
  const uint8_t* source = gateSpan(va, bytes);
  SIZE_T copied = 0;
  return source && ReadProcessMemory(GetCurrentProcess(), source, destination, bytes, &copied) && copied == bytes;
}
// Step 6 I15 (J:/sr_lanec_drawthread/FREEZE_I15_20261024.txt): the guest-memory micro bundle's switch SR_GUESTMEM_LEAN
// (default on; =0 every piece off - today's code), read once; and its counters ([GUESTMEM_LEAN] at exit).
bool guestMemLeanOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_GUESTMEM_LEAN");
    return !(e && e[0] == '0');
  }();
  return on;
}
std::atomic<uint64_t> g_leanMappedReads{0}, g_leanMappedFaults{0}, g_leanWalkChunks{0}, g_leanWalkServed{0};
// P2: the guarded plain copy (G3's pattern, the coordinator's condition): an access violation is caught, never raised.
// No C++ object in this frame (SEH). This TU has no -fasync-exceptions, so a __try covers faults raised inside CALLS
// only: the copy is an indirect call through a volatile pointer (never lowered to inline loads) from a noinline
// function. The harness's VEHs pass read faults on (gwwVeh and imageWriteVeh claim writes only; the crash handler is
// the unhandled filter, after this frame). L1 in --seg-arena-test faults this exact code on a NOACCESS page.
void* (*volatile g_guardedMemcpy)(void*, const void*, size_t) = &std::memcpy;
__declspec(noinline) bool guardedCopy(void* destination, const void* source, uint32_t bytes) noexcept {
  __try {
    g_guardedMemcpy(destination, source, bytes);
    return true;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}
// P2: a read of a KNOWN-MAPPED guest address - the device struct (mapped while the device lives) or an XEX image global
// - as gateSpan's cached readability check then the guarded copy, instead of a ReadProcessMemory syscall per read
// (presentGateLeaf / bracketGuestMode / swapLeaf: ~0.06 ms per present on the draw thread). A fault (an address that
// is not what the caller says) is counted and falls back to gateRead, which decides as before. Guest POINTERS (list
// links, the front buffer's header, a rect, a root cell) never come here: they keep gateRead.
bool gateReadMapped(uint32_t va, void* destination, uint32_t bytes) {
  if (!guestMemLeanOn()) return gateRead(va, destination, bytes);
  const uint8_t* source = gateSpan(va, bytes);
  if (!source) return false;
  if (guardedCopy(destination, source, bytes)) {
    g_leanMappedReads.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  g_leanMappedFaults.fetch_add(1, std::memory_order_relaxed);
  return gateRead(va, destination, bytes);
}
#if SR_ORACLE
// I15's per-piece counters at exit (dev): the guarded reads and their faults (P2), the walk's runs and the tokens they
// served (P4). P1's clock reads are on the [HRR] line, P5's cached-id tests on the [CONST_DEDUP] line.
struct GuestMemLeanExitLine {
  ~GuestMemLeanExitLine() {
    const uint64_t m = g_leanMappedReads.load(), f = g_leanMappedFaults.load(), c = g_leanWalkChunks.load(),
                   s = g_leanWalkServed.load();
    if (!m && !f && !c && !s) return;
    std::printf("[GUESTMEM_LEAN] final lean=%s mapped=%llu faults=%llu walk_chunks=%llu walk_served=%llu\n",
                guestMemLeanOn() ? "on" : "off", (unsigned long long)m, (unsigned long long)f, (unsigned long long)c,
                (unsigned long long)s);
    std::fflush(stdout);
  }
} g_guestMemLeanExitLine;
#endif
}  // namespace (I15's seam is external)
#if SR_ORACLE
bool guardedCopyForTest(void* destination, const void* source, uint32_t bytes) {
  return guardedCopy(destination, source, bytes);
}
// L2's seam (the coordinator 10-25 00:2x): the production gateReadMapped, with its counters' deltas over the call.
bool gateReadMappedForTest(uint32_t va, void* destination, uint32_t bytes, uint64_t* faults, uint64_t* mapped) {
  const uint64_t f0 = g_leanMappedFaults.load(), m0 = g_leanMappedReads.load();
  const bool ok = gateReadMapped(va, destination, bytes);
  if (faults) *faults = g_leanMappedFaults.load() - f0;
  if (mapped) *mapped = g_leanMappedReads.load() - m0;
  return ok;
}
bool guestMemLeanOnForTest() { return guestMemLeanOn(); }
#endif
namespace {
bool gateWritable(uint32_t va, uint32_t bytes) {
  uint8_t* begin = gateSpan(va, bytes);
#if SR_ORACLE
  // A page the GPU write tracker (kernel/gpu_write_watch.hpp) made read-only after validating it is as writable as the
  // guest finds it: the first store, the SDK body's or this leaf's, faults into the tracker's handler, which releases
  // the block (both physical copies) and publishes it dirty. The admission rule of native_pump / native_recording /
  // native_transport (12343e71). Without it the SWAP / BRACKET admissions declined whenever the cursor or a recording
  // list sat on such a page with no guest store before them in that block (the tcmp1 diagnosis, 2026-10-14): segmentless
  // record spans write no guest bytes, so under the composed kinds nothing released it first. Every caller is behind its
  // gate: SR_NATIVE_SWAP and SR_NATIVE_BRACKET (default ON since the transport bake, 2026-10-15; =0 off) and
  // SR_NATIVE_REPLAY_CALLBACK (default OFF).
  return sr::native::memory::writableRange(begin, bytes) || sr::kernel::gww::cpuWritableRange(begin, bytes);
#else
  return sr::native::memory::writableRange(begin, bytes);
#endif
}
// The decline samples' views (the tcmp1 diagnosis): [va, va + bytes) page by page - each page's protection (VirtualQuery
// on the host copy; "/own" where the tracker holds the physical page) - and the two writability answers; the device's
// mode fields the SWAP / BRACKET admissions read.
void pageView(uint32_t va, uint32_t bytes, char* out, size_t n) {
  if (!va || !bytes || va > UINT32_MAX - (bytes - 1u) || !sr::kernel::guestVAToHost(va)) {
    std::snprintf(out, n, "va=%08X n=%u (no span)", va, bytes);
    return;
  }
  uint8_t* host = sr::kernel::guestVAToHost(va);
  const bool plain = sr::native::memory::writableRange(host, bytes);
#if SR_ORACLE
  const bool tracked = sr::kernel::gww::cpuWritableRange(host, bytes);
  int at = std::snprintf(out, n, "va=%08X n=%u writable=%d tracker=%d pages", va, bytes, int(plain), int(tracked));
#else
  int at = std::snprintf(out, n, "va=%08X n=%u writable=%d pages", va, bytes, int(plain));
#endif
  for (uint64_t p = va & ~0xFFFu; p <= uint64_t(va) + bytes - 1u && at > 0 && at < int(n) - 40; p += 0x1000u) {
    MEMORY_BASIC_INFORMATION region{};
    uint8_t* hp = sr::kernel::guestVAToHost(uint32_t(p));
    const bool queried = hp && VirtualQuery(hp, &region, sizeof(region)) != 0;
#if SR_ORACLE
    const bool physical = p >= 0x80000000u;
    const bool owned = physical && sr::kernel::gww::protGen(uint32_t((p & 0x1FFFFFFFu) >> 12u)) != 0u;
#else
    constexpr bool owned = false;
#endif
    at += std::snprintf(out + at, n - size_t(at), " %08X:%s%lX%s", uint32_t(p), queried ? "" : "?",
                        queried ? (unsigned long)region.Protect : 0ul, owned ? "/own" : "");
  }
}
void modeView(uint32_t dev, char* out, size_t n) {
  uint8_t mode[4] = {}, word[4], movie[1] = {};
  auto field = [&](uint32_t va) { return gateRead(va, word, 4u) ? be32(word) : 0xFFFFFFFFu; };
  const bool ok = gateRead(dev + 0x2A38u, mode, 4u);
  const uint32_t hsio = field(0x829AC0ECu);
  if (!gateRead(0x829ABD8Cu, movie, 1u)) movie[0] = 0xFFu;
  std::snprintf(out, n,
                "modes%s 2A38..3B=%02X %02X %02X %02X 4FB0=%08X 4DF4=%08X 4E84=%08X 4E58=%08X 4E70=%08X 3B28=%08X "
                "hsio=%08X movie=%02X", ok ? "" : "(unread)", mode[0], mode[1], mode[2], mode[3], field(dev + 0x4FB0u),
                field(dev + 0x4DF4u), field(dev + 0x4E84u), field(dev + 0x4E58u), field(dev + 0x4E70u),
                field(dev + 0x3B28u), hsio, movie[0]);
}
bool sampleDue(std::atomic<uint64_t>& count, uint64_t& k) {  // per reason: the first 8, then every power of two
  k = count.fetch_add(1, std::memory_order_relaxed) + 1u;
  return k <= 8u || (k & (k - 1u)) == 0u;
}
// One SDK swap decline: the total, its reason, and the sample (the cursor's reserved block: the region whose plain
// writability the admission requires).
bool swapDecline(int why, uint32_t dev, uint32_t stack, uint32_t cursor) {
  g_swapSdkDecl.fetch_add(1, std::memory_order_relaxed);
  g_swapWhy[why].fetch_add(1, std::memory_order_relaxed);
  static std::atomic<uint64_t> s_n[kSwWhyN];
  uint64_t k = 0;
  if (sampleDue(s_n[why], k)) {
    char block[512], modes[256];
    pageView(cursor ? cursor + 4u : 0u, 296u, block, sizeof(block));
    modeView(dev, modes, sizeof(modes));
    g_swapSampled[why].fetch_add(1, std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_FLUSH] SWAP_DECL #%llu why=%s vdswap=%llu dev=%08X stack=%08X cursor=%08X "
                "block: %s | %s", (unsigned long long)k, swapSdkDeclName(why),
                (unsigned long long)sr::kernel::getVdSwapCount(), dev, stack, cursor, block, modes);
  }
  return false;
}
// One BRACKET_END memory decline: its site and the sample of the region it refused.
void bracketMemorySample(int site, uint32_t dev, uint32_t list, uint32_t va, uint32_t bytes) {
  g_brMemory[site].fetch_add(1, std::memory_order_relaxed);
  static std::atomic<uint64_t> s_n[kBmN];
  uint64_t k = 0;
  if (sampleDue(s_n[site], k)) {
    char region[512], modes[256];
    pageView(va, bytes, region, sizeof(region));
    modeView(dev, modes, sizeof(modes));
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_FLUSH] BRACKET_MEMORY #%llu site=%s list=%04X vdswap=%llu dev=%08X: %s | %s",
                (unsigned long long)k, bracketMemoryName(site), list, (unsigned long long)sr::kernel::getVdSwapCount(),
                dev, region, modes);
  }
}
}
// boot gaps: the SWAP admission's helpers for the display leaves (kernel/native_display_emit.cpp).
bool guestRead(uint32_t va, void* destination, uint32_t bytes) { return gateRead(va, destination, bytes); }
bool guestWritable(uint32_t va, uint32_t bytes) { return gateWritable(va, bytes); }

namespace {
#if defined(SR_DIFFTEST)
ReplayKickPublicationObserver g_kickPublicationObserver = nullptr;
ReplayKickCloseObserver g_kickCloseObserver = nullptr;
#endif
}
bool replayKickCallbackOn() {
  static const bool enabled = envOn("SR_NATIVE_REPLAY_CALLBACK");
  return enabled;
}
#if defined(SR_DIFFTEST)
void setReplayKickPublicationObserver(ReplayKickPublicationObserver observer) {
  g_kickPublicationObserver = observer;
}
void setReplayKickCloseObserver(ReplayKickCloseObserver observer) {
  g_kickCloseObserver = observer;
}
#endif

bool ReplayKickCallback::emit(PPCContext& ctx) {
  if (!replayKickCallbackOn() || !g_on || !segmentlessActive() || emitted_ ||
      g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;
  static const int ab = sr::abt::reg("SR_NATIVE_REPLAY_CALLBACK");
  if (ab >= 0 && !sr::abt::on(ab)) return false;
  const uint32_t dev = ctx.r3.u32, last = ctx.r4.u32, flags = ctx.r5.u32;
  uint32_t pcBE = 0;
  // This helper admits the whole 23-dword kick form before touching the output
  // or context. The allocator owns its mapping through the controller call.
  if (!dev || ((dev | last) & 3u) || dev > UINT32_MAX - 0x4E87u ||
      last > UINT32_MAX - 96u || !(flags & 1u) || ctx.r6.u32 != 0x82110B00u ||
      !gateRead(dev + 0x2A14u, &pcBE, 4u) || !gateWritable(last + 4u, 92u)) return false;
  uint8_t* output = sr::kernel::guestVAToHost(last + 4u);
  const uint8_t* scratch = sr::kernel::guestVAToHost(dev + 0x2A14u);
  const uintptr_t outputHost = reinterpret_cast<uintptr_t>(output);
  const uintptr_t scratchHost = reinterpret_cast<uintptr_t>(scratch);
  // 820FDA80 reads this word after its first seven writes and again later.
  // Physical aliases must also decline before the snapshot/output diverge.
  if (outputHost <= scratchHost ? scratchHost - outputHost < 92u : outputHost - scratchHost < 4u)
    return false;
  if (sr::kernel::g_physUnifyActive && last + 4u >= 0x80000000u && dev + 0x2A14u >= 0x80000000u) {
    // Unified windows have distinct host addresses over the same backing.
    // Conservatively reject a physical-window boundary as well as overlap.
    const uint32_t outPhysical = (last + 4u) & 0x1FFFFFFFu;
    const uint32_t scratchPhysical = (dev + 0x2A14u) & 0x1FFFFFFFu;
    if (outPhysical > 0x1FFFFFFFu - 91u ||
        (outPhysical <= scratchPhysical ? scratchPhysical - outPhysical < 92u : outPhysical - scratchPhysical < 4u))
      return false;
  }
  const uint32_t pc = _byteswap_ulong(pcBE), mask = (flags >> 24) & 0x3Fu;
  const uint32_t payload[5] = {ctx.r6.u32, ctx.r7.u32, mask ? mask : 4u, flags,
      ((((pc >> 20) + 0x200u) & 0x1000u) + (pc & 0x1FFFFFFFu)) | 2u};
#if SR_ORACLE
  uint32_t values[25];
  if (callbackDwords(payload, values) != 23u) return false;
  // The development projection is also visible to subsequent CPU allocator /
  // PIX observers. The admitted queue consumes the immutable record instead.
  for (uint32_t i = 0; i < 23u; ++i) put32(output + 4u * i, values[i]);
#endif
  dev_ = dev; start_ = last + 4u;
  std::memcpy(payload_, payload, sizeof(payload_));
  emitted_ = true;
  ctx.r3.u64 = ctx.r4.u64 + 92u;  // SDK stwu/add chain preserves the full-width cursor.
  g_kickCbEmitted.fetch_add(1, std::memory_order_relaxed);
  return true;
}

ReplayKickCallback::~ReplayKickCallback() {
  if (emitted_) g_kickCbOmitted.fetch_add(1, std::memory_order_relaxed);
}

void ReplayKickCallback::append(PPCContext& ctx, uint8_t* base) {
  const bool emitted = emitted_;
  emitted_ = false;  // An owned helper emission may be appended at most once.
  auto literal = [&] {
    if (emitted) g_kickCbLiteral.fetch_add(1, std::memory_order_relaxed);
    sub_820FD800(ctx, base);
  };
  const uint32_t phys = (((start_ >> 20) + 0x200u) & 0x1000u) + (start_ & 0x1FFFFFFFu);
  const uint64_t before = ctx.r4.u64;
  uint8_t mode = 0;
  uint32_t pix = 0, pixCallback = 0;
  bool consumer = sr::kernel::gpuCpActive();
#if defined(SR_DIFFTEST)
  consumer = consumer || g_kickPublicationObserver;
#endif
  // Helpers may alter mode or fail allocation. Re-admit this independent
  // append before record/registry mutation; the complete CPU controller never
  // falls back after its own writes. Its already-emitted byte projection makes
  // every declined append the exact original whole helper call.
  if (!emitted || !consumer || g_pm4Out || !segmentlessActive() ||
      dev_ > UINT32_MAX - 0x4E87u ||
      ctx.r3.u32 != dev_ || ctx.r5.u32 != phys || ctx.r6.u64 != 23u || ctx.r7.u64 != 1u ||
      ctx.r8.u32 != dev_ + 0x32DCu ||
      !gateRead(dev_ + 0x2A39u, &mode, 1u) || (mode & 2u) ||
      !gateRead(dev_ + 0x4DF4u, &pix, 4u) || pix ||
      !gateRead(dev_ + 0x4E84u, &pixCallback, 4u) || pixCallback) { literal(); return; }
  ensureCallbackHandlers();
  Entry entry(ctx, base, dev_);
  if (!entry.openAt(start_ - 4u)) { literal(); return; }
  if (!entry.opaque(kRkCallback, payload_, 5u, 92u)) { entry.abandon(); literal(); return; }
  const auto closed = entry.closeDetached();
#if defined(SR_DIFFTEST)
  if (g_kickCloseObserver) g_kickCloseObserver();
#endif
  const SegFoot& foot = closed.foot;
  // After the record writer commits, a missing footprint is an integrity
  // failure. Do not silently execute a second interpretation of this tail.
  if (!foot.record || foot.lo != start_ || foot.hi != start_ + 92u) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_BAD] replay kick callback footprint missing");
    std::abort();
  }
  auto parts = buildParts(start_, start_ + 92u, {foot});
  parts->kickCallback = true;
  registerParts(phys, 23u, parts, true, 1u);
  auto outer = std::move(t_segPublishing);
  t_segPublishing = parts;
  sub_820FD800(ctx, base);  // Preserve all original CPU list/lock/PIX decisions.
#if defined(SR_DIFFTEST)
  // With no worker the real SDK helper still writes the ring. The test takes
  // the exact direct token here, then compares its application separately.
  if (g_kickPublicationObserver && t_segPublishing == parts && ctx.r3.u64 == before && !ringCut())
    segmentPublishId(phys, 23u);
#endif
  const bool chained = t_segPublishing == parts && ctx.r3.u64 == before + 16u;
  const bool handed = t_segPublishing != parts;
  if (chained) {
    t_segPublishing.reset();
    g_kickCbChained.fetch_add(1, std::memory_order_relaxed);
  } else if (!handed) {
    // A development whole-body helper route kept the exact projected bytes.
    // It cannot retain a native alias for a later reuse of this physical slot.
    t_segPublishing.reset();
    std::lock_guard<std::mutex> lock(g_segMu);
    g_segById.erase(parts->id);
    g_segDeferred.erase(std::remove(g_segDeferred.begin(), g_segDeferred.end(), parts), g_segDeferred.end());
    (ringCut() ? g_cutKicks : g_kickCbLiteral).fetch_add(1, std::memory_order_relaxed);  // the cut: an orderly stop
  }
  t_segPublishing = std::move(outer);
  if (chained || handed) {
    g_kickCbPublished.fetch_add(1, std::memory_order_relaxed);
#if defined(SR_DIFFTEST)
    if (g_kickPublicationObserver) g_kickPublicationObserver(parts->id, phys, 23u, chained);
#endif
  }
}

// ============================================================================================
// p2.transport BRACKET begin / end + BS (NATIVE_ROADMAP transport plan item 4 and its build note; native_flush.hpp).
// Written from the generated bodies of 82110B30 / 82110C78 / 820FD3B8 / 820FDA80 / 82110460 (guest/ppc, 2026-10-14;
// IDA's MCP was down - the generated code is what runs) and ENGINE_MAP's 2026-10-12 replay boundaries.
// ============================================================================================

struct BracketAccess {
  static Driver& driver(Entry& e) { return e.impl_->dr; }
};

bool bracketGate() { return g_bracket; }
#if defined(SR_DIFFTEST)
void setSegmentPublicationObserver(SegmentPublicationObserver observer) { g_segPublishObserver = observer; }
#endif

namespace {
uint32_t brRd(uint32_t va) { return sr::kernel::readGuestU32BE(va); }
uint32_t brPhys(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }

// The record consumer exists: the async CP (the ringless override takes the hand-off on the publishing thread), or the
// difftest harness's in-order consumer.
bool bracketConsumer() {
  if (sr::kernel::gpuCpActive()) return true;
#if defined(SR_DIFFTEST)
  return g_segPublishObserver != nullptr;
#else
  return false;
#endif
}
// The modes in which the close or the ring writers leave the work to the guest body: the command-buffer device and
// tiling (0x2A38 & 0x80 / 0x20), no-GPU (0x2A39 & 2), PIX (0x4DF4 / 0x4E84), the HSIO failure global; at the begin also
// a pending allocation failure (0x2A39 & 0x40), whose bracket the SDK drops at its kick anyway.
bool bracketGuestMode(uint32_t dev, bool begin) {
  uint8_t mode[4], pix[4], pixCb[4], hsio[4];
  // I15 P2: the device's fields and the XEX global are known-mapped (the guarded plain read)
  if (!gateReadMapped(dev + 0x2A38u, mode, 4u) || !gateReadMapped(dev + 0x4DF4u, pix, 4u) ||
      !gateReadMapped(dev + 0x4E84u, pixCb, 4u) || !gateReadMapped(0x829AC0ECu, hsio, 4u))
    return true;
  return (mode[0] & 0xA0u) || (mode[1] & (begin ? 0x42u : 0x02u)) || be32(pix) || be32(pixCb) || be32(hsio);
}
// At an append: would GpuRingUpdateWritePointer's ringless override take this publish (and its hand-off)? The live
// state, as the override reads it. When it would not, the SDK's own route is taken from that point (the projected
// bytes are the SDK's).
bool bracketLiteralRoute(uint32_t dev) {
  return !sr::kernel::ringless::gateOn() || !bracketConsumer() || ((brRd(dev + 0x2A38u) >> 16) & 2u) ||
         brRd(dev + 0x4DF4u) || brRd(dev + 0x4E84u);
}

// The recorded list as 82110460 reads it, from head+4 to the list cursor (exclusive), software links followed.
struct BrToken {
  uint8_t kind;  // 0 header 0x80, 1 depth 0x84, 2 record 0x82, 3 end 0x85, 4 colour 0x86
  uint32_t at, n, phys;
};
bool bracketWalk(uint32_t head, uint32_t end, std::vector<BrToken>& out) {
  uint32_t at = head + 4u;
  const uint32_t endPhys = brPhys(end);
  // I15 P4 (SR_GUESTMEM_LEAN): one fault-safe read per 512-byte run of the list instead of one per token - a token
  // whose bytes lie in the run is served from it; past the run or across a link a new run is read. The list is guest-
  // written (its links are guest pointers), so every read stays gateRead's; a run that cannot be read whole (an edge)
  // falls back to the exact per-token read - the same outcome as before. The walk runs on the producer, which writes
  // the list, so a run's bytes are the bytes a per-token read would see.
  uint8_t blk[512];
  uint32_t blkVa = 0, blkN = 0;
  auto get = [&](uint32_t va, uint32_t n, uint8_t* w) -> bool {
    if (!guestMemLeanOn()) return gateRead(va, w, n);
    if (blkN && va >= blkVa && uint64_t(va) + n <= uint64_t(blkVa) + blkN) {
      std::memcpy(w, blk + (va - blkVa), n);
      g_leanWalkServed.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    if (gateRead(va, blk, sizeof blk)) {
      blkVa = va;
      blkN = uint32_t(sizeof blk);
      g_leanWalkChunks.fetch_add(1, std::memory_order_relaxed);
      std::memcpy(w, blk, n);
      return true;
    }
    blkN = 0;
    return gateRead(va, w, n);
  };
  for (uint32_t guard = 0; guard < 4096u; guard++) {
    if (brPhys(at) == endPhys) return true;
    uint8_t w[252];
    if ((at & 3u) || !get(at, 4u, w)) return false;
    const uint32_t word = be32(w);
    if (word == 0x80000000u) {
      if (!get(at, 252u, w)) return false;  // the 63-dword header (S+0x74's copy)
      out.push_back({0u, at, 0u, 0u});
      at += 252u;
    } else if (word == 0x84000000u || word == 0x85000000u || word == 0x86000000u) {
      out.push_back({uint8_t(word == 0x84000000u ? 1u : word == 0x85000000u ? 3u : 4u), at, 0u, 0u});
      at += 4u;
    } else if ((word >> 24) == 0x82u) {
      if (!get(at, 8u, w)) return false;
      out.push_back({2u, at, word & 0xFFFFFFu, be32(w + 4)});
      at += 8u;
    } else if ((word & 0xC0000000u) == 0xC0000000u && word != 0xC0000000u) {
      at = word + 4u;  // a software link: the next block's data (its soft view)
    } else {
      return false;  // 0x81, 0x83, 0x87..0x8C, a data record or a terminator: not the canonical bracket
    }
  }
  return false;
}
// The canonical non-tiling bracket [hdr][84](82)*[85](82)*[86] and its replay steps, by 82110460's own control flow
// over it: flags S+0x28 (bit 30 depth, bit 31 colour), the return positions S+0x170 (the 0x84) / S+0x174 (the 0x86),
// the bin-select cache S+0x16C that the header resets to 0x7FFFFFFF (82110140 writes only a changed value), the tile
// index S+0x2C the header resets to 0 (so the tiled colour entry at 0x84 never applies). A step is a BS value (rec <
// 0) or the index of a bound record.
struct BrStep {
  uint32_t bs;
  int rec;
};
bool bracketPlan(const std::vector<BrToken>& t, std::vector<BrStep>& steps, uint32_t& records) {
  size_t i = 0;
  auto at = [&](uint8_t k) { return i < t.size() && t[i].kind == k; };
  if (!at(0)) return false;
  i++;
  if (!at(1)) return false;
  i++;
  while (at(2)) i++;
  if (!at(3)) return false;
  i++;
  while (at(2)) i++;
  if (!at(4)) return false;
  i++;
  if (i != t.size()) return false;
  std::vector<int> index(t.size(), -1);
  records = 0;
  for (size_t k = 0; k < t.size(); k++)
    if (t[k].kind == 2u) index[k] = int(records++);
  uint32_t flags = 0, cache = 0x7FFFFFFFu;
  size_t p170 = 0, p174 = 0;
  auto bs = [&] {
    const uint32_t v = (flags & 0x80000000u) ? 0x2AAAAAAAu : (flags & 0x40000000u) ? 0x15555555u : 0xFFFFFFFFu;
    if (v != cache) {
      cache = v;
      steps.push_back({v, -1});
    }
  };
  size_t k = 0;
  for (size_t guard = 0; k < t.size(); guard++) {
    if (guard > 4u * t.size() + 16u) return false;
    switch (t[k].kind) {
      case 0: cache = 0x7FFFFFFFu; k++; break;
      case 1: p170 = k; k++; flags |= 0x40000000u; bs(); break;
      case 2: steps.push_back({0u, index[k]}); k++; break;
      case 3:
        k++;
        flags &= ~0x40000000u;
        if (flags & 0x80000000u) k = p174;
        else bs();
        break;
      default:  // 4
        p174 = k;
        k++;
        flags ^= 0x80000000u;
        if (flags & 0x80000000u) k = p170;
        else bs();
        break;
    }
  }
  return (flags & 0xC0000000u) == 0u;  // the replay leaves S+0x28 as it found it
}

// Removes a generation's bound entries from the deferred registry: they are the BRACKET_END segment's now (or never
// replayed: the SDK's failure / no-kick branches). Under g_segMu.
void bracketRetireLocked(const BracketGen& g) {
  for (const BracketClose& c : g.closes) {
    if (!c.sp) continue;
    g_segById.erase(c.sp->id);
    g_segDeferred.erase(std::remove(g_segDeferred.begin(), g_segDeferred.end(), c.sp), g_segDeferred.end());
    g_brRetired.fetch_add(1, std::memory_order_relaxed);
  }
}

// One of the kick's blocks as a record span at its own allocation (`lastDw` = the dword before it), captured into a
// detached footprint (it never joins the open segment's), dev+40 left as it was.
SegFoot bracketSpan(Driver& dr, uint32_t lastDw, const Item* items, uint32_t n, bool opaque) {
  const uint32_t cursor = be32(dr.d + 40);
  SegFoot foot{};
  dr.c = dr.spanStart = lastDw;
  dr.nItems = 0;
  dr.opaque = opaque;
  for (uint32_t k = 0; k < n; k++) {
    Item* o = dr.reserve(1);
    *o = items[k];
    dr.commit(1, items[k].bytes());
  }
  dr.detachedFoot = &foot;
  dr.closeSpan();
  dr.detachedFoot = nullptr;
  put32(dr.d + 40, cursor);
  dr.c = dr.spanStart = cursor;
  return foot;
}

// Publishes a BRACKET part through the guest's own 820FD800 (its spinlock, chain decision and PIX checks), handing
// the parts over on this thread as publishSegment does. -> 0 handed over (published directly), 1 chained behind a
// running guest pump (found later by its replay), 2 neither (the ring path took the block: never after an admitted
// record route).
int bracketPublish(PPCContext& work, uint8_t* base, uint32_t dev, uint64_t at, uint32_t phys, uint32_t dwords,
                   const std::shared_ptr<SegParts>& sp) {
  registerParts(phys, dwords, sp, /*deferred=*/true, 1u);
  std::shared_ptr<SegParts> outer = std::move(t_segPublishing);
  t_segPublishing = sp;
  work.r3.u64 = dev;
  work.r4.u64 = at;
  work.r5.u64 = phys;
  work.r6.u64 = dwords;
  work.r7.u64 = 0u;  // the increment: no pump work is queued by a native bracket (the build note's dev+0x2A74 rule)
  work.r8.u64 = dev + 0x32DCu;
  sub_820FD800(work, base);
#if defined(SR_DIFFTEST)
  if (g_segPublishObserver && t_segPublishing == sp && work.r3.u64 == at && !ringCut())
    if (const uint64_t id = segmentPublishId(phys, dwords)) g_segPublishObserver(id, phys, dwords, false);
#endif
  int route = 2;
  if (t_segPublishing != sp) route = 0;
  else if (work.r3.u64 == at + 16u) route = 1;
  if (route) t_segPublishing.reset();
  if (route == 2) {
    std::lock_guard<std::mutex> lk(g_segMu);
    g_segById.erase(sp->id);
    g_segDeferred.erase(std::remove(g_segDeferred.begin(), g_segDeferred.end(), sp), g_segDeferred.end());
  }
#if defined(SR_DIFFTEST)
  if (route == 1 && g_segPublishObserver) g_segPublishObserver(sp->id, phys, dwords, true);
#endif
  t_segPublishing = std::move(outer);
  return route;
}
// The P2 trace's binding record (the tcmp1 diagnosis, 2026-10-14): a BRACKET_END about to be published binds these
// closes; their segments are applied only inside it (kind 24 aux 4, once per pass). Traced before the publish as kind 24
// aux 5 {f = first id (low 32), a = count, b = generation (low 32)}, one record per contiguous id run, so the grader
// (tools/perf/p2_trace.py segment_order) can tell the final bracket's applies cut by the trace's end
// (tail_bound_unapplied) from skipped ids. Then its replay plan step by step (Astra round 1): kind 24 aux 8 a bin
// select {f = step index, a = value, b = generation (low 32)}, aux 9 a bound segment's application {f = step index,
// a = its id (low 32), b = generation} - the order the CP's steps (kind 3 aux 1 bin selects, kind 24 aux 4 applies,
// between its lifecycle records aux 6 / 7) follow. Observe-only.
void traceBracketBinding(const BracketGen& gen, const std::vector<BrStep>& plan) {
  std::vector<uint64_t> ids;
  ids.reserve(gen.closes.size());
  for (const BracketClose& c : gen.closes)
    if (c.sp) ids.push_back(c.sp->id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
#if defined(SR_DIFFTEST)
  g_brTestBinding.clear();
#endif
  for (size_t i = 0; i < ids.size();) {
    size_t j = i + 1u;
    while (j < ids.size() && ids[j] == ids[j - 1u] + 1u) j++;
    const uint32_t first = uint32_t(ids[i]), count = uint32_t(j - i), generation = uint32_t(gen.gen);
    sr::native::p2trace::event(sr::native::p2trace::kCpItem, 5u, first, count, generation);
#if defined(SR_DIFFTEST)
    g_brTestBinding.push_back({first, count, generation});  // exactly what was traced
#endif
    i = j;
  }
#if defined(SR_DIFFTEST)
  g_brTestPlan.clear();
#endif
  for (size_t s = 0; s < plan.size(); s++) {
    const int rec = plan[s].rec;
    const bool apply = rec >= 0;
    const uint32_t value = !apply ? plan[s].bs
                           : size_t(rec) < gen.closes.size() && gen.closes[size_t(rec)].sp
                               ? uint32_t(gen.closes[size_t(rec)].sp->id) : 0u;
    const uint16_t aux = apply ? 9u : 8u;
    sr::native::p2trace::event(sr::native::p2trace::kCpItem, aux, uint32_t(s), value, uint32_t(gen.gen));
#if defined(SR_DIFFTEST)
    g_brTestPlan.push_back({uint32_t(aux), uint32_t(s), value, uint32_t(gen.gen)});  // exactly what was traced
#endif
  }
}
// P2 trace kind 24 aux 10 {f = the END segment id (low 32), a = 0, b = generation (low 32)}: a BRACKET_END the ring
// cut dropped - its binding / plan records stand, the CP never begins it (p2_trace.py bracket_terminal: brackets_cut).
void traceBracketCut(const SegParts& sp, uint64_t gen) {
  if (!sr::native::p2trace::g_on) return;
  sr::native::p2trace::event(sr::native::p2trace::kCpItem, 10u, uint32_t(sp.id), 0u, uint32_t(gen));
#if defined(SR_DIFFTEST)
  g_brTestCutGen = gen;
#endif
}
[[noreturn]] void bracketFatal(const char* what) {
  SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_BAD] bracket: %s", what);
  sr::Log::shutdown();
  std::abort();
}
}  // namespace

bool bracketBeginDecide(const PPCContext& ctx) {
  if (!g_bracket) return false;
  static const int ab = sr::abt::reg("SR_NATIVE_BRACKET");
  if (ab >= 0 && !sr::abt::on(ab)) {
    g_brBeginAbOff.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t dev = ctx.r3.u32;
  if (!g_on || !segmentlessActive() || g_pm4Out || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth ||
      !bracketConsumer() || !dev || (dev & 3u) || !gateWritable(dev, 0x5000u) || bracketGuestMode(dev, true)) {
    g_brBeginDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void bracketOpen(uint32_t dev) {
  uint8_t w[12];
  const bool ok = gateRead(dev + 0x32C8u, w, 12u);
  const uint32_t head = ok ? be32(w) : 0u, cursor = ok ? be32(w + 8) : 0u;
  uint8_t pin[4];
  const uint32_t pinned = gateRead(dev + 0x3290u, pin, 4u) ? be32(pin) : 0u;
  std::lock_guard<std::mutex> lk(g_segMu);
  if (g_brOpen) {  // a begin without its kick: that bracket's closes stay deferred, as the guest path leaves them
    g_brStale.fetch_add(1, std::memory_order_relaxed);
    g_brOpen.reset();
  }
  if (!head || !pinned || cursor < 252u) {  // the SDK body recorded no list here: nothing to bind
    g_brBeginDecl.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto g = std::make_unique<BracketGen>();
  g->gen = g_brNextGen++;
  g->dev = dev;
  g->head = head;
  g->header = cursor - 252u;  // 82110B30 leaves dev+0x32D0 just past its 63-dword header
  g->thread = GetCurrentThreadId();
  g_brOpen = std::move(g);
  g_brBegin.fetch_add(1, std::memory_order_relaxed);
}

void bracketGuestClose(uint32_t dev) {
  std::lock_guard<std::mutex> lk(g_segMu);
  if (g_brOpen && g_brOpen->dev == dev && !g_brOpen->poisoned) {
    g_brOpen->poisoned = true;  // its list record has no parts: the kick must take the SDK's own route
    g_brPoison.fetch_add(1, std::memory_order_relaxed);
  }
}

// GpuReplayListEndRecordingKick_82110C78(dev r3, tiles r4, cpumask r5). Everything is decided before the first write;
// after it the port of the generated body runs to its end (rule 1).
bool bracketEnd(PPCContext& ctx, uint8_t* base) {
  if (!g_bracket) return false;
  const uint32_t dev = ctx.r3.u32, stack = ctx.r1.u32;
  std::unique_ptr<BracketGen> gen;
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    if (g_brOpen && g_brOpen->dev == dev && g_brOpen->thread == GetCurrentThreadId()) gen = std::move(g_brOpen);
  }
  auto decline = [](BrDecl why) {
    g_brDecl[why].fetch_add(1, std::memory_order_relaxed);
    return false;
  };
  if (!gen) return decline(kBrNoGen);  // the begin was the SDK's (gate, A/B window or a begin decline)
#if defined(SR_DIFFTEST)
  g_brTestBound.clear();
  for (const BracketClose& c : gen->closes) g_brTestBound.push_back(c.sp);
#endif
  if (gen->poisoned) return decline(kBrPoisoned);
  if (!g_on || !segmentlessActive() || g_pm4Out || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth ||
      !bracketConsumer())
    return decline(kBrGate);
  // A memory decline is counted by its site (native_flush.hpp BracketMemorySite) with a sample of the refused region.
  auto memory = [&](int site, uint32_t list, uint32_t va, uint32_t bytes) {
    bracketMemorySample(site, dev, list, va, bytes);
    return decline(kBrMemory);
  };
  if ((dev & 3u) || (stack & 3u) || stack < 0x2000u) return memory(kBmFrame, 0u, stack, 4u);
  if (!gateWritable(dev, 0x5000u)) return memory(kBmDev, 0u, dev, 0x5000u);
  if (!gateWritable(stack - 0x1000u, 0x1000u)) return memory(kBmStack, 0u, stack - 0x1000u, 0x1000u);
  if (bracketGuestMode(dev, false)) return decline(kBrMode);
  // Mixed traffic (the per-present A/B; live tbr_abp, 2026-10-14): an OFF bracket's pump work is outstanding (dev+0x2A74
  // != 0: a guest kick its EndSync has not drained). 820FD800 chains every close made while it is non-zero into the
  // list it is given - after this kick, the current list dev+0x32C8, THIS bracket's, which no pump walks once its kick
  // is native: those segments (their fences, their kicks) would never be published (fence waits time out at
  // 820F33E8's 5000-tick limit, ~805 ms a present; a lost guest kick leaves the count stuck). The SDK kick instead
  // (its +1) makes the pump walk this list after the outstanding one. Only this thread kicks: a 0 read here stays 0
  // until its next guest kick, so a native end never meets a chain.
  uint8_t pumpWork[4];
  if (!gateRead(dev + 0x2A74u, pumpWork, 4u) || be32(pumpWork)) return decline(kBrPending);
  uint8_t lists[0x40];  // 0x32A0 .. 0x32DF: the three recording lists' descriptors and the old chain's
  if (!gateRead(dev + 0x32A0u, lists, sizeof(lists))) return memory(kBmLists, 0x32A0u, dev + 0x32A0u, 0x40u);
  for (int i = 0; i < 3; i++) {  // list 0, 1, 2 = 0x32C8, 0x32A0, 0x32B4: terminated below
    const uint32_t off = i == 0 ? 0x28u : i == 1 ? 0x00u : 0x14u;
    const uint32_t prefix = be32(lists + off + 4u), cursor = be32(lists + off + 8u);
    if ((prefix | cursor) & 3u) return memory(kBmAlign0 + i, 0x32A0u + off, prefix, 4u);
    if (!gateWritable(prefix, 4u)) return memory(kBmPrefix0 + i, 0x32A0u + off, prefix, 4u);
    if (!gateWritable(cursor, 4u)) return memory(kBmCursor0 + i, 0x32A0u + off, cursor, 4u);
  }
  const uint32_t head = be32(lists + 0x28u), listEnd = be32(lists + 0x30u);
  std::vector<BrToken> tokens;
  std::vector<BrStep> plan;
  uint32_t records = 0;
  if (head != gen->head || !bracketWalk(head, listEnd, tokens) || !bracketPlan(tokens, plan, records) ||
      brPhys(tokens[0].at) != brPhys(gen->header))
    return decline(kBrWalk);
  // Identity by sequence: the i-th 0x82 record of the list is the i-th pinned close of this generation.
  if (records != gen->closes.size()) return decline(kBrBind);
  for (size_t k = 0, r = 0; k < tokens.size(); k++) {
    if (tokens[k].kind != 2u) continue;
    const BracketClose& c = gen->closes[r++];
    if (!c.sp || brPhys(tokens[k].at) != brPhys(c.listVA) || tokens[k].n != c.dwords || tokens[k].phys != c.phys ||
        c.sp->phys != c.phys || c.sp->dwords != c.dwords)
      return decline(kBrBind);
  }
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    for (const BracketClose& c : gen->closes)
      if (!c.sp->deferred || c.sp->published || c.sp->started || !g_segById.count(c.sp->id)) return decline(kBrPublished);
  }
  // The arena for both records, held for the whole kick (its backpressure wait happens here, before any write).
  Entry entry(ctx, base, dev);
  if (!entry.open()) return memory(kBmEntry, 0u, 0u, 0u);
  ensureBracketHandlers();
  Driver& dr = BracketAccess::driver(entry);
  dr.ddWant = dr.dd = false;  // no constant dedup on the kick's blocks
  uint8_t* const d = dr.d;

  // ---- the port of the generated 82110C78 (frame 0xB0), from here to the end ----
  PPCContext work = ctx;
  const uint32_t frame = stack - 0xB0u;
  work.r1.u32 = frame;
  sr::kernel::writeGuestBE32(frame, stack);
  const uint64_t cpuArg = ctx.r5.u64;
  // (1) GpuBuildFlushMemoryPackets_820FD3B8(dev, frame+0x54 phys, frame+0x50 count) as kRkInvalidate records: the SDK's
  // empty sentinel 0x00000000FFFFFFFF on the packed range (end << 32 | base), the same allocation from its own frame,
  // the same out-params, its 11 / 22 dwords projected into the block (the records are written at the append).
  Item coh[2] = {};
  uint32_t cohItems = 0, cohVA = 0;
  if (!(d[0x2A39u] & 0x40u)) {
    const uint32_t inner = frame - 0xB0u;  // 820FD3B8's own stwu r1,-176(r1)
    sr::kernel::writeGuestBE32(inner, frame);
    const bool range = be64(d + 0x2D18u) != 0x00000000FFFFFFFFull;
    const bool whole = ((d[0x2A3Au] >> 2) & 1u) != 0;
    const uint32_t n = 11u * (uint32_t(range) + uint32_t(whole));
    uint32_t va = 0;
    if (n) {
      work.r1.u32 = inner;
      work.r3.u64 = dev;
      work.r4.u64 = n;
      work.r5.u64 = 32u;
      sub_820FCDF0(work, base);
      va = work.r3.u32;
      work.r1.u32 = frame;
    }
    if (!va) {
      sr::kernel::writeGuestBE32(frame + 0x50u, 0u);  // *count = 0; the phys out-param keeps its stack value
    } else {
      auto block = [&](uint32_t baseAddr, uint32_t size, uint32_t wholeFlag) {
        Item& it = coh[cohItems++];
        Driver::packet(it, uint8_t(kRkInvalidate), 0u, {baseAddr, size, wholeFlag});
        it.fixedBytes = 44u;
#if SR_ORACLE
        uint8_t bytes[44];
        renderPm4(it, bytes);
        writeGuest(va + 44u * (cohItems - 1u), bytes, 44u);
#endif
      };
      if (range) {  // the ldarx / stdcx loop: take the current range, leave the sentinel
        volatile long long* q = reinterpret_cast<volatile long long*>(d + 0x2D18u);
        const long long empty = static_cast<long long>(_byteswap_uint64(0x00000000FFFFFFFFull));
        long long raw;
        for (;;) {
          raw = *q;
          if (_InterlockedCompareExchange64(q, empty, raw) == raw) break;
        }
        uint64_t v;
        std::memcpy(&v, &raw, 8);
        v = _byteswap_uint64(v);
        const uint32_t end = uint32_t(v >> 32), lo = uint32_t(v) & ~0xFFFu;
        block(lo, ((end + 0xFFFu) & ~0xFFFu) - lo, 0u);
      }
      if (whole) {
        const uint32_t lo = brPhys(be32(d + 0x34ACu)) & ~0xFFFu;
        block(lo, ((brPhys(be32(d + 0x34B0u)) + 0xFFFu) & ~0xFFFu) - lo, 1u);
      }
      sr::kernel::writeGuestBE32(frame + 0x50u, n);
      sr::kernel::writeGuestBE32(frame + 0x54u, brPhys(va));
      cohVA = va;
    }
  }
  // (2) The 100-dword kick block. The SDK's 820FDA80 writes the 23-dword callback block there (cb 82110B00, ctx = the
  // list head's view, flags r5|1: no 3D-idle prefix), reading the scratch pointer dev+0x2A14 after its 7th and 17th
  // dword; the same bytes are projected, and the BRACKET_END record stands for them.
  work.r3.u64 = dev;
  work.r4.u64 = 100u;
  work.r5.u64 = 32u;
  sub_820FCDF0(work, base);
  const uint64_t kick = work.r3.u64;
  uint64_t kickEnd;
  uint32_t payload[8] = {};
  if (uint32_t(kick)) {
    const uint32_t root = brRd(dev + 0x32C8u);
    const uint32_t view = root ? brPhys(root) - 0x40000000u : 0u;
    const uint32_t flags = uint32_t(cpuArg | 1u), mask = (flags >> 24) & 0x3Fu;
    const uint32_t id = mask ? mask : 4u, out = uint32_t(kick);
#if SR_ORACLE
    const uint32_t head7[7] = {0x0001057Cu, 0x82110B00u, view, 0x00000578u, id, 0xC0043C01u, 0x13u};
    for (uint32_t k = 0; k < 7u; k++) sr::kernel::writeGuestBE32(out + 4u * k, head7[k]);
    const uint32_t pc = brPhys(brRd(dev + 0x2A14u)) | 2u;
    const uint32_t mid[9] = {pc, id, 0xFFFFFFFFu, 0x100u, 0xC0005401u, id, 0x0000045Eu, id, 0xC0043C01u};
    for (uint32_t k = 0; k < 9u; k++) sr::kernel::writeGuestBE32(out + 28u + 4u * k, mid[k]);
    sr::kernel::writeGuestBE32(out + 64u, 0x13u);
    const uint32_t pc2 = brPhys(brRd(dev + 0x2A14u)) | 2u;
    const uint32_t tail[6] = {pc2, 0u, 0xFFFFFFFFu, 0x100u, 0x0000057Cu, 0x0BADF00Du};
    for (uint32_t k = 0; k < 6u; k++) sr::kernel::writeGuestBE32(out + 68u + 4u * k, tail[k]);
#else
    const uint32_t pc = brPhys(brRd(dev + 0x2A14u)) | 2u;
#endif
    payload[0] = uint32_t(gen->gen);
    payload[1] = uint32_t(gen->gen >> 32);
    payload[2] = uint32_t(plan.size());
    payload[3] = 0x82110B00u;
    payload[4] = view;
    payload[5] = id;
    payload[6] = flags;
    payload[7] = pc;
    kickEnd = kick + 92u;  // 820FDA80's returned last dword (kick - 4 + 92) + 4
  } else {
    kickEnd = brRd(frame + 0x54u);
  }
  // (3) The 32-dword append hole.
  work.r3.u64 = dev;
  work.r4.u64 = 32u;
  work.r5.u64 = 4u;
  sub_820FCDF0(work, base);
  uint64_t appendAt = uint32_t(work.r3.u64) ? work.r3.u64 : uint64_t(brRd(frame + 0x54u));
  // (4) Recording off; the allocation-failure flag decides the branch.
  const uint8_t flags2 = d[0x2A39u];
  sr::kernel::writeGuestBE32(dev + 0x3290u, 0u);
  sr::kernel::writeGuestBE32(dev + 0x3294u, 0u);
  const uint32_t failed = (flags2 >> 6) & 1u;
  if (failed) {  // the CB-state restore and return 1, as the asm; the SDK never replays this list
    const uint32_t fields[6] = {0x28u, 0x2Cu, 0x34C4u, 0x34C0u, 0x34B8u, 0x34BCu};
    for (uint32_t k = 0; k < 6u; k++) sr::kernel::writeGuestBE32(dev + fields[k], brRd(dev + 0x32F0u + 4u * k));
    d[0x2A39u] = uint8_t(flags2 & 0xBFu);
    sr::kernel::writeGuestBE32(dev + 0x30u, brRd(dev + 0x32F4u) - 160u);
    {
      std::lock_guard<std::mutex> lk(g_segMu);
      bracketRetireLocked(*gen);
    }
    entry.abandon();
    g_brEndFailed.fetch_add(1, std::memory_order_relaxed);
    ctx.r3.u64 = 1u;
    return true;
  }
  // (5) Terminate the three lists: the block's first word -> the terminator's view, the terminator, limit 0.
  for (uint32_t off : {0x32C8u, 0x32A0u, 0x32B4u}) {
    const uint32_t prefix = brRd(dev + off + 4u);
    sr::kernel::writeGuestBE32(prefix, brPhys(brRd(dev + off + 8u)) - 0x40000000u);
    sr::kernel::writeGuestBE32(brRd(dev + off + 8u), 0xC0000000u);
    sr::kernel::writeGuestBE32(dev + off + 12u, 0u);
  }
  // (6) The coherency block, appended through the old chain descriptor dev+0x32DC with increment 0 (as the SDK).
  const uint32_t count = brRd(frame + 0x50u);
  if (count) {
    const uint32_t phys = brRd(frame + 0x54u);
    if (bracketLiteralRoute(dev) || !cohVA || 4u * count != 44u * cohItems) {
#if SR_ORACLE
      // The SDK's own route: the projected 11 / 22 dwords go as the PM4 they are (no record was written).
      work.r3.u64 = dev; work.r4.u64 = appendAt; work.r5.u64 = phys; work.r6.u64 = count; work.r7.u64 = 0u;
      work.r8.u64 = dev + 0x32DCu;
      sub_820FD800(work, base);
      appendAt = work.r3.u64;
#else
      nativeAdmissionFatal("bracket coherency has no native footprint");
#endif
    } else {
      const SegFoot foot = bracketSpan(dr, cohVA - 4u, coh, cohItems, false);
      if (!foot.record || foot.lo != cohVA || foot.hi != cohVA + 4u * count) bracketFatal("coherency footprint missing");
      auto parts = buildParts(cohVA, cohVA + 4u * count, {foot});
      const int route = bracketPublish(work, base, dev, appendAt, phys, count, parts);
      // Not taken: under the graceful quit's ring cut an orderly stop (dropped, as the guest's own publish would be;
      // the END below is dropped the same way); anything else is the integrity failure it always was.
      if (route == 2 && !ringCut()) bracketFatal("coherency block published without its hand-off");
      if (route == 2) g_cutCoher.fetch_add(1, std::memory_order_relaxed);
      appendAt = work.r3.u64;
    }
  }
  // (7) The kick: the SDK appends its callback block with increment 1 (dev+0x2A74, the pump's work); BRACKET appends
  // the BRACKET_END segment with increment 0 and the consumer replays the steps in its place.
  const uint8_t flags3 = d[0x2A39u];
  if (brRd(dev + 0x4DF4u) || !(flags3 & 2u)) {
    const uint32_t kickPhys = brPhys(uint32_t(kick));
    const uint32_t kickDwords = uint32_t(int32_t(uint32_t(kickEnd - kick)) >> 2);
    if (bracketLiteralRoute(dev) || !uint32_t(kick) || kickDwords != 23u) {
#if SR_ORACLE
      // The SDK's own kick (late PIX / no-GPU from a helper): the projected bytes as PM4, increment 1, the pump replays
      // the still-deferred entries - exactly the guest body from here.
      work.r3.u64 = dev; work.r4.u64 = appendAt; work.r5.u64 = kickPhys; work.r6.u64 = kickDwords; work.r7.u64 = 1u;
      work.r8.u64 = dev + 0x32DCu;
      sub_820FD800(work, base);
      g_brLiteral.fetch_add(1, std::memory_order_relaxed);
#else
      nativeAdmissionFatal("bracket callback has no native footprint");
#endif
    } else {
      Item end{};
      end.pkt = 1;
      end.rk = uint8_t(kRkBracket);
      end.count = 8u;
      end.src = reinterpret_cast<const uint8_t*>(payload);
      end.fixedBytes = 92u;
      const SegFoot foot = bracketSpan(dr, uint32_t(kick) - 4u, &end, 1u, true);
      if (!foot.record || foot.lo != uint32_t(kick) || foot.hi != uint32_t(kick) + 92u)
        bracketFatal("BRACKET_END footprint missing");
      auto parts = buildParts(uint32_t(kick), uint32_t(kick) + 92u, {foot});
      for (const BrStep& s : plan) {
        SegPart p{};
        if (s.rec < 0) {
          p.kind = 2u;
          p.off = s.bs;
        } else {
          p.kind = 3u;
          p.nested = gen->closes[size_t(s.rec)].sp;
        }
        parts->parts.push_back(std::move(p));
      }
      parts->bracketGen = gen->gen;
      parts->bracketSteps = uint32_t(plan.size());
      if (sr::native::p2trace::g_on) traceBracketBinding(*gen, plan);
      const int route = bracketPublish(work, base, dev, appendAt, kickPhys, kickDwords, parts);
      if (route == 2 && !ringCut()) bracketFatal("BRACKET_END published without its hand-off");
      if (route == 1) g_brChained.fetch_add(1, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lk(g_segMu);
        bracketRetireLocked(*gen);
      }
      if (route == 2) {  // the graceful quit's ring cut dropped the END: its bound segments never run, the bracket ends
        g_cutEnds.fetch_add(1, std::memory_order_relaxed);
        traceBracketCut(*parts, gen->gen);
      } else {
        g_brEnd.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } else {
    // No-GPU mode with no PIX object: the SDK appends no kick and never replays the list.
    std::lock_guard<std::mutex> lk(g_segMu);
    bracketRetireLocked(*gen);
  }
  entry.abandon();
  ctx.r3.u64 = 0u;
  return true;
}

// SR_NATIVE_SWAP_BLOCK (VdSwap's 64-dword block as kind 19) and SR_NATIVE_SWAP (82112050's SDK body as host code; it
// requires the block): both default ON since the transport bake (native/transport-bake, 2026-10-15); =0 keeps the guest
// body / the export's literal block and the A/B.
bool swapBlockOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_SWAP_BLOCK");  // `e`: scripts/audit_forcing.py reads `!e ||` as default ON
    return !e || e[0] != '0';
  }();
  return on;
}
bool swapOn() {
  static const bool enabled = [] {
    const char* e = std::getenv("SR_NATIVE_SWAP");
    return !e || e[0] != '0';
  }();
  static const int ab = sr::abt::reg("SR_NATIVE_SWAP");
  return enabled && swapBlockOn() && g_on && segmentlessActive() &&
         !g_presentGateOracleDepth && !g_shaderConstRunsOracleDepth && (ab < 0 || sr::abt::on(ab));
}
uint64_t swapSdkCalls() { return g_swapSdkCalls.get(); }
TransportCounters transportCounters() {
  TransportCounters t;
  t.gateCalls = g_gateCalls.get(); t.gateSkip = g_gateSkip.get(); t.gateDecl = g_gateDecl.get();
  t.gateApplied = g_gateApplied.get();
  t.cbCalls = g_cbCalls.get(); t.cbDecl = g_cbDecl.get(); t.cbApplied = g_cbApplied.get();
  t.swapBlockCalls = g_swapCalls.get(); t.swapBlockDecl = g_swapDecl.get(); t.swapBlockApplied = g_swapApplied.get();
  t.swapSdkCalls = g_swapSdkCalls.get(); t.swapSdkDecl = g_swapSdkDecl.get();
  t.segBeginCalls = segBeginCalls(); t.devCreateDecl = devCreateDecl();
  t.tileRectCalls = tileRectCalls(); t.tilePredCalls = tilePredCalls();
  return t;
}
uint64_t swapSdkDeclWhy(int why) { return why >= 0 && why < kSwWhyN ? g_swapWhy[why].get() : 0u; }
uint64_t swapSdkDeclSampled(int why) { return why >= 0 && why < kSwWhyN ? g_swapSampled[why].get() : 0u; }
QuitCutCounters quitCutCounters() {
  return QuitCutCounters{g_cutSegs.get(), g_cutKicks.get(), g_cutCoher.get(), g_cutEnds.get()};
}
const char* swapSdkDeclName(int why) {
  static const char* const kNames[kSwWhyN] = {
      "devRead", "devWrite", "front", "stack", "tiling", "flags2A39", "movieCapture", "pix", "timestamps", "3B28",
      "hsioRead", "hsio", "movieGlobalRead", "movieGlobal", "rect", "sysbuf", "rootRead", "root", "export",
      "cursorAlign", "cursorWrite", "entry", "fallbackFront", "fallbackScaler", "fallbackPersist"};
  return why >= 0 && why < kSwWhyN ? kNames[why] : "?";
}
uint64_t bracketMemoryDecl(int site) { return site >= 0 && site < kBmN ? g_brMemory[site].get() : 0u; }
const char* bracketMemoryName(int site) {
  static const char* const kNames[kBmN] = {"frame",   "dev",     "stack",   "lists",   "align0",  "align1", "align2",
                                           "prefix0", "prefix1", "prefix2", "cursor0", "cursor1", "cursor2", "entry"};
  return site >= 0 && site < kBmN ? kNames[site] : "?";
}

bool swapLeaf(PPCContext& ctx, uint8_t* base) {
  if (!swapOn()) return false;
  const uint32_t dev = ctx.r3.u32, front = ctx.r4.u32, rect = ctx.r5.u32;
  uint8_t snapshot[0x5000u], header[52], rectangle[28], globals[4];
  const uint32_t stack = ctx.r1.u32;
  uint32_t cursor = 0u;  // known once the device snapshot is read; the samples show its reserved block
  auto decline = [&](int why) { return swapDecline(why, dev, stack, cursor); };
  // Complete admission precedes all writes. The caller must keep these mapped
  // writable device/stack/buffer objects alive throughout the SDK operation.
  // Each refusal is counted by its reason (native_flush.hpp SwapDeclWhy), in this order.
  if (!gateReadMapped(dev, snapshot, sizeof(snapshot))) return decline(kSwDevRead);  // I15 P2: the device struct
  cursor = be32(snapshot + 40u);
  if (!gateWritable(dev, sizeof(snapshot))) return decline(kSwDevWrite);
  if (!gateRead(front, header, sizeof(header))) return decline(kSwFront);
  if (stack < 0x11B0u || !gateWritable(stack - 0x11B0u, 0x11B0u)) return decline(kSwStack);
  if (snapshot[0x2A38u] & 0x80u) return decline(kSwTiling);
  // no-GPU 0x02, persist 0x10, allocation failure 0x40. boot gaps (SR_NATIVE_DISPLAY, default ON): the persisted display
  // (0x10: set once by the device create's D3DDeviceCreatePresentBuffers 82105B18, consumed by the first Swap - dcf2
  // census1's SWAP_DECL #1 why=flags2A39 and frame 2's 527 literal packets) is native; =0 declines it as before.
  const bool persist = (snapshot[0x2A39u] & 0x10u) != 0u && displayActive();
  const bool flags2A39 = (snapshot[0x2A39u] & (persist ? 0x42u : 0x52u)) != 0u;
  if (flags2A39) return decline(kSwFlags2A39);
  if (snapshot[0x4FB0u] & 8u) return decline(kSwMovieCapture);
  if (be32(snapshot + 0x4DF4u) || be32(snapshot + 0x4E84u)) return decline(kSwPix);
  if (be32(snapshot + 0x4E58u)) return decline(kSwTimestamps);
  if (be32(snapshot + 0x3B28u)) return decline(kSw3B28);
  if (be32(snapshot + 0x4E70u)) return decline(kSwTimestamps);
  if (!gateReadMapped(0x829AC0ECu, globals, 4u)) return decline(kSwHsioRead);  // I15 P2: an XEX global
  if (be32(globals)) return decline(kSwHsio);
  if (!gateReadMapped(0x829ABD8Cu, globals, 1u)) return decline(kSwMovieGlobalRead);  // I15 P2: an XEX global
  if (globals[0]) return decline(kSwMovieGlobal);
  if (rect && !gateRead(rect, rectangle, sizeof(rectangle))) return decline(kSwRect);
#if SR_ORACLE
  // Preserve the legacy disabled system-buffer diagnostic for oracle runs.
  const char* sysbuf = std::getenv("SR_VD_SYSCMDBUF");
  if (sysbuf && sysbuf[0] == '0') return decline(kSwSysbuf);
#endif
  uint8_t root[4], cell[4];
  // I15 P2: 0x82000948 is an XEX global (guarded plain read); the cell it points to is a guest POINTER (gateRead)
  if (!gateReadMapped(0x82000948u, root, 4u) || !gateRead(be32(root), cell, 4u)) return decline(kSwRootRead);
  if (be32(cell) != dev) return decline(kSwRoot);  // the root's device is another one
  if (!gateReadMapped(0x829AC100u, globals, 4u)) return decline(kSwRootRead);  // I15 P2: an XEX global
  const bool preKick = be32(globals) != 0u;
  const auto swap = sr::kernel::ExportRegistry::instance().getExport("VdSwap");
  if (!swap || !sr::kernel::gpuCpActive()) return decline(kSwExport);
  // boot gaps: the persisted display's two kernel calls (the SDK's VdPersistDisplay and MmFreePhysicalMemory) and the
  // stack its D3DDeviceSetDisplayModeAndGamma 82105DD0 call uses below the SDK frame (0xCD0, its ramp builder 0x8F0 more)
  const auto persistDisplay = persist ? sr::kernel::ExportRegistry::instance().getExport("VdPersistDisplay") : nullptr;
  const auto freePhysical = persist ? sr::kernel::ExportRegistry::instance().getExport("MmFreePhysicalMemory") : nullptr;
  if (persist && (!persistDisplay || !freePhysical)) return decline(kSwExport);
  if (persist && (stack < 0x3000u || !gateWritable(stack - 0x3000u, 0x3000u))) return decline(kSwStack);
  if ((cursor & 3u) || uint64_t(cursor) + 300u > UINT32_MAX) return decline(kSwCursorAlign);
  if (!gateWritable(cursor + 4u, 296u)) return decline(kSwCursorWrite);
  { Entry admission(ctx, base, dev); if (!admission.open()) return decline(kSwEntry); admission.abandon(); }

  const uint32_t frame = stack - 0x1B0u;
  PPCContext work = ctx;
  work.r1.u64 = frame;
  sr::kernel::writeGuestBE32(frame, stack);
  uint8_t fetch[24];
  std::memcpy(fetch, header + 28u, sizeof(fetch));
  uint32_t formatWord = be32(fetch + 4u);
  const uint32_t format = formatWord & 63u;
  if (format == 50u) formatWord = (formatWord & ~63u) | 6u;
  else if (format == 7u) formatWord = (formatWord & ~63u) | 54u;
  const uint32_t displayFormat = be32(snapshot + 0x33B0u);
  put32(fetch, (be32(fetch) & 0xFFFFFC03u) | ((displayFormat >> 7u) & 0x3FCu));
  put32(fetch + 4u, formatWord);
  const uint32_t fourth = (be32(fetch + 12u) & 0xFFFFE000u) | ((displayFormat >> 17u) & 0x1FFFu);
  put32(fetch + 12u, fourth);
  std::memcpy(sr::kernel::guestVAToHost(dev + 0x3470u), header, sizeof(header));
  std::memcpy(sr::kernel::guestVAToHost(frame + 0x80u), fetch, sizeof(fetch));
  const uint32_t extra = ((fourth >> 30u) & 2u) + 1u;
  const uint32_t dims = be32(fetch + 8u);
  const uint32_t width = (dims & 0x1FFFu) + extra, height = ((dims >> 13u) & 0x1FFFu) + extra;
  sr::kernel::writeGuestBE32(frame + 0x60u, formatWord & 0xFFFFF000u);
  sr::kernel::writeGuestBE32(frame + 0x64u, height);
  sr::kernel::writeGuestBE32(frame + 0x68u, width);
  sr::kernel::writeGuestBE32(frame + 0x6Cu, formatWord & 63u);
  sr::kernel::writeGuestBE32(frame + 0x70u, be32(snapshot + 0x33B4u));
  std::memset(sr::kernel::guestVAToHost(frame + 0xC0u), 0, 0x94u);
  sr::kernel::writeGuestBE32(frame + 0xC0u, 0xBEEF0000u);
  sr::kernel::writeGuestBE32(frame + 0x74u, 0xBEEF0001u);
  auto call = [&](void (*fn)(PPCContext&, uint8_t*), uint32_t r4 = 0u, uint32_t r5 = 0u, uint32_t r6 = 0u) {
    work.r3.u64 = dev; work.r4.u64 = r4; work.r5.u64 = r5; work.r6.u64 = r6;
    fn(work, base);
  };
  if (preKick && be32(snapshot + 40u) > be32(snapshot + 48u)) call(sub_820FD8C0);
  call(sub_820FC910, 64u);
  const uint32_t reserved = work.r3.u32;
  sr::kernel::ExportRegistry::setCurrentExtraArgs(frame + 0x6Cu, frame + 0x70u);
  swap(reserved + 4u, frame + 0x80u, sr::kernel::readGuestU32BE(dev + 0x2A10u) + 8u,
       frame + 0xC0u, 0xBEEF0001u, frame + 0x60u);
  sr::kernel::writeGuestBE32(dev + 40u, reserved + 256u);
  sr::kernel::writeGuestBE32(dev + 0x4E80u, 0u);
  uint8_t* d = sr::kernel::guestVAToHost(dev);
  // boot gaps: the persisted display's head (0x8211236C): the SDK sets the display mode and gamma here -
  // D3DDeviceSetDisplayModeAndGamma_82105DD0(dev, dev+0x3370, 1), the guest body: its linear ramp, when it differs from
  // dev+0x3510, through GpuRingEmitGammaRamp 821056E0 / ...Wide 821057C0 (the kRkGammaRamp leaves under
  // SR_NATIVE_DISPLAY), VdSetDisplayMode, VdGetCurrentDisplayInformation into dev+0x4DFC..0x4E08.
  if (persist && (d[0x2A39u] & 0x10u)) call(sub_82105DD0, dev + 0x3370u, 1u);
  if (rect) {
    // Reserve/export callbacks may change the rectangle and its cached state.
    // Admission snapshots establish readability, not later SDK input values.
    std::memcpy(rectangle, sr::kernel::guestVAToHost(rect), sizeof(rectangle));
    if (std::memcmp(rectangle, d + 0x33D0u, sizeof(rectangle))) {
      std::memcpy(d + 0x33D0u, rectangle, sizeof(rectangle));
      call(sub_8210A9F8, rect);
    }
  }
  // Descriptor+8 is zero: restore the caller's scaler/gamma state, as the SDK.
  const bool scaler = (d[0x2A3Au] & 0x10u) != 0u;
  if (scaler) std::memset(d + 0x33ECu, 0, 56u);
  if (d[0x2A3Bu] & 0x40u) call(sub_82111CB8, be32(d + 0x33B0u), dev + 0x3510u);
  d[0x2A3Bu] &= ~0x40u;
  if (scaler) {
    d[0x2A3Au] &= ~0x10u;
    call(sub_8210A788, width, height, dev + 0x3424u);
  }
  const uint32_t frontVA = sr::kernel::readGuestU32BE(frame + 0x60u);
  if (be32(d + 0x4E0Cu) != frontVA) {
    const uint32_t phys = (((frontVA >> 20u) + 512u) & 0x1000u) + (frontVA & 0x1FFFFFFFu);
    uint8_t value[4]; put32(value, phys);
    Entry e(work, base, dev);
    if (e.open()) {
      ShaderConstRunsOracle nested;
      e.kick(); e.run(0x1844u, 1u, value); e.close();
    }
    else {
#if SR_ORACLE
      // Development record admission can fail after SDK allocator callbacks.
      // Preserve the operation through the literal oracle, never drop it.
      swapDecline(kSwFallbackFront, dev, stack, be32(d + 40u));  // counted (total + reason) and sampled; still taken
      if (be32(d + 40u) > be32(d + 48u)) call(sub_820FD8C0);
      const uint32_t at = be32(d + 40u);
      sr::kernel::writeGuestBE32(at + 4u, 0x1844u);
      sr::kernel::writeGuestBE32(at + 8u, phys);
      put32(d + 40u, at + 8u);
#else
      nativeAdmissionFatal("swap front-buffer record refused");
#endif
    }
    put32(d + 0x4E0Cu, frontVA);
  }
  if (scaler) {
    ensureSwapScalerResetHandlers();
    const uint32_t zero = 0u;
    Entry e(work, base, dev);
    if (e.open()) {
      ShaderConstRunsOracle nested;
      e.kick(); e.opaque(kRkSwapScalerReset, &zero, 1u, 40u); e.close();
    }
    else {
#if SR_ORACLE
      swapDecline(kSwFallbackScaler, dev, stack, be32(d + 40u));  // counted (total + reason) and sampled; still taken
      if (be32(d + 40u) > be32(d + 48u)) call(sub_820FD8C0);
      const uint32_t at = be32(d + 40u);
      swapScalerResetRender(kRkSwapScalerReset, &zero, 1u, sr::kernel::guestVAToHost(at + 4u), 40u);
      put32(d + 40u, at + 40u);
#else
      nativeAdmissionFatal("swap scaler record refused");
#endif
    }
  }
  if (persist && (d[0x2A39u] & 0x10u)) {
    // boot gaps: the persisted display's tail (0x8211265C): after the kick check REG_RMW {0x1E4E, ~1, 0} as one kRkRegRmw
    // record (the SDK's literal 4 dwords when the Entry cannot take it: counted as fallbackPersist, the call still taken),
    // then VdPersistDisplay(sp+0x98 = 12 zero bytes, sp+0xA8) and, when it returns non-zero, MmFreePhysicalMemory(1,
    // *(sp+0xA8)) - the SDK's frame offsets in the SDK frame - then the bit cleared.
    bool recorded = false;
    {
      Entry e(work, base, dev);
      if (e.open()) {
        ShaderConstRunsOracle nested;
        recorded = e.kick() && e.regRmw(0x1E4Eu, 0xFFFFFFFEu, 0u);
        if (recorded) e.close();
        else e.abandon();
      }
    }
    if (!recorded) {
#if SR_ORACLE
      swapDecline(kSwFallbackPersist, dev, stack, be32(d + 40u));  // counted (total + reason) and sampled; still taken
      if (be32(d + 40u) > be32(d + 48u)) call(sub_820FD8C0);
      const uint32_t at = be32(d + 40u);
      sr::kernel::writeGuestBE32(at + 4u, 0xC0022100u);
      sr::kernel::writeGuestBE32(at + 8u, 0x1E4Eu);
      sr::kernel::writeGuestBE32(at + 12u, 0xFFFFFFFEu);
      sr::kernel::writeGuestBE32(at + 16u, 0u);
      put32(d + 40u, at + 16u);
#else
      nativeAdmissionFatal("swap persist record refused");
#endif
    }
    std::memset(sr::kernel::guestVAToHost(frame + 0x98u), 0, 12u);
    if (persistDisplay(frame + 0x98u, frame + 0xA8u, 0u, 0u, 0u, 0u))
      freePhysical(1u, sr::kernel::readGuestU32BE(frame + 0xA8u), 0u, 0u, 0u, 0u);
    d[0x2A39u] &= ~0x10u;
    displayNotePersist();
  }
  const uint32_t fence = be32(d + 0x2A1Cu);
  call(sub_820FD8C0);
  // PC render targets do not have EDRAM retraining. The development oracle's
  // VdRetrainEDRAM returns false, so the retired loop has no device effects.
  call(sub_820FCF90, be32(d + 0x34B4u) >> 2u);
  const uint32_t previous = be32(d + 0x34D0u);
  if (previous) call(sub_820FD9A8, previous, 3u);
  else work.r4.u64 = 0u;  // 82112708 loads r4 even when the throttle call is skipped.
  put32(d + 0x34D0u, fence);
  d[0x2A39u] &= ~0x20u;
  work.r3.u64 = dev;
  sub_8210BBE8(work, base);  // the SDK retains the preceding call's volatile arguments
  call(sub_820F0048);
  ctx.r3.u64 = work.r3.u64;
  g_swapSdkCalls.fetch_add(1, std::memory_order_relaxed);
  return true;
}
bool swapBlockLeaf(uint32_t dev, uint32_t blockVA, uint32_t fetchVA, uint32_t width, uint32_t height) {
  if (!swapBlockOn() || !g_on || !segmentlessActive() ||
      g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return false;
  static const int ab = sr::abt::reg("SR_NATIVE_SWAP_BLOCK");
  if (ab >= 0 && !sr::abt::on(ab)) return false;
  auto decline = [] { g_swapDecl.fetch_add(1, std::memory_order_relaxed); return false; };
  uint8_t device[0x5000u];
  uint8_t hsio[4];
  // Cached readability alone is insufficient if a mapped page was protected
  // after a prior call. Admission reads an owned, fault-free snapshot.
  if (!gateRead(dev, device, sizeof(device)) || blockVA < 0x80000004u || (blockVA & 3u) ||
      uint64_t(blockVA) + 256u > UINT32_MAX || !gateSpan(blockVA, 256u) ||
      blockVA - 4u != be32(device + 40u) || blockVA < be32(device + 13508u) ||
      blockVA + 252u > be32(device + 44u) || (device[0x2A38u] & 0x80u) ||
      (device[0x2A39u] & 0x42u) || be32(device + 0x4DF4u) || be32(device + 0x4E84u) ||
      !gateRead(0x829AC0ECu, hsio, sizeof(hsio)) || be32(hsio)) return decline();
  uint32_t payload[10] = {};
  if (fetchVA) {
    uint8_t fetch[24];
    if (!gateRead(fetchVA, fetch, sizeof(fetch))) return decline();
    payload[0] = 1u;
    for (uint32_t i = 0; i < 6u; ++i) payload[1u + i] = be32(fetch + 4u * i);
    // Preserve the current VdSwap virtual-to-physical translation, including
    // the low format bits in fetch word 1. The snapshot is owned by the item.
    payload[7] = payload[2] & 0x1FFFF000u;
    payload[2] = (payload[2] & 0xFFFu) | payload[7];
  }
  payload[8] = width;
  payload[9] = height;
  ensureSwapBlockHandlers();
  // This reserved leaf neither calls the SDK nor uses its caller's registers.
  PPCContext context{};
  Entry entry(context, sr::kernel::getGuestBase(), dev);
  if (!entry.openAt(blockVA - 4u)) return decline();
  if (!entry.opaque(kRkSwap, payload, 10u, 256u)) { entry.abandon(); return decline(); }
  entry.close();  // restores dev+40; 82112050 advances it after the export
  g_swapCalls.fetch_add(1, std::memory_order_relaxed);
  return true;
}
#if SR_ORACLE
bool presentGateCheckBefore(const PPCContext& ctx, PresentGateCheck& state) {
  state = {};
  auto decline = [] { g_gateChkDecl.fetch_add(1, std::memory_order_relaxed); return false; };
  uint8_t device[kDevSpan];
  const uint32_t dev = ctx.r3.u32;
  if (!gateRead(dev, device, sizeof(device))) return decline();
  state.dev = dev;
  state.returnValue = ctx.r3.u64;
  state.cursor = be32(device + 40u);
  state.skipped = be32(device + 0x33A4u) == 0x80000000u || (be32(device + 0x3490u) & 0xFFFFF000u) == 0u;
  if (!state.skipped) {
    uint8_t perf[4];
    if (dev > UINT32_MAX - 0x4E73u || !gateRead(dev + 0x4E70u, perf, sizeof(perf)) || be32(perf) == 2u ||
        !presentGatePayload(be32(device + 0x2A14u), be32(device + 0x33A4u), be32(device + 0x2D3Cu), state.payload) ||
        !gateNoKick(device)) return decline();
  }
  state.ready = true;
  return true;
}
#endif
bool shaderConstantRunsLeaf(PPCContext& ctx, uint8_t* base) {
  if (!shaderConstRunsOn()) return false;
  if (t_tessDepth) g_tessSdk.fetch_add(1, std::memory_order_relaxed);
  // Every false below runs the SDK body (its PM4 into the stream): counted by reason (the construns bake's witness).
  auto decline = [](ScrDecl why) {
    g_scrDecl[why].fetch_add(1, std::memory_order_relaxed);
    return false;
  };
  if (!g_on) return decline(kScrOff);
  if (g_shaderConstRunsOracleDepth) return decline(kScrOracle);
  if (!ctx.r4.u64) return decline(kScrMask);
  static const int flushAB = sr::abt::reg("SR_NATIVE_FLUSH");
  static const int shaderAB = sr::abt::reg("SR_NATIVE_SHADER_CONST_RUNS");
  if ((flushAB >= 0 && !sr::abt::on(flushAB)) || (shaderAB >= 0 && !sr::abt::on(shaderAB))) return decline(kScrOff);
  const uint32_t dev = ctx.r3.u32;
  // Kicks also access fields beyond the draw-state span. Validate the complete
  // device and the fixed preamble before opening or changing guest state.
  const uint8_t* d = gateSpan(dev, 0x5000u);
  if (!d) return decline(kScrSpan);
  const uint32_t cursor = be32(d + 40u);
  if ((cursor & 3u) || cursor > UINT32_MAX - 56u || !gateSpan(cursor + 4u, 52u)) return decline(kScrSpan);
  Entry entry(ctx, base, dev);
  if (!entry.open()) return decline(kScrOpen);
  uint64_t result = 0;
  // Nonzero mask, record mode and a successful open make this emission total;
  // internal SDK segment switches complete within this Entry.
  if (!entry.shaderConstantRuns(ctx.r4.u64, result)) {
    entry.abandon();
    return decline(kScrEmit);
  }
  entry.close();
  ctx.r3.u64 = result;
  g_scrSdk.fetch_add(1, std::memory_order_relaxed);
  return true;
}
// D3DDeviceEmitDrawTessellated 820FC478 (the override below): a tessellated draw is on this thread's stack.
void tessEnter() {
  g_tessDraws.fetch_add(1, std::memory_order_relaxed);
  ++t_tessDepth;
}
void tessLeave() { --t_tessDepth; }
ShaderConstRunsCounters shaderConstRunsCounters() {
  ShaderConstRunsCounters s;
  auto ld = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  s.calls = g_shaderConstRuns.get(); s.chunked = g_shaderConstChunks.get();
  s.drv = ld(g_scrDrv); s.sdk = ld(g_scrSdk); s.abGuest = ld(g_scrAbGuest);
  s.declOff = ld(g_scrDecl[kScrOff]); s.declOracle = ld(g_scrDecl[kScrOracle]); s.declMask = ld(g_scrDecl[kScrMask]);
  s.declSpan = ld(g_scrDecl[kScrSpan]); s.declOpen = ld(g_scrDecl[kScrOpen]); s.declEmit = ld(g_scrDecl[kScrEmit]);
  s.kick = ld(g_scrKick); s.marginBad = ld(g_scrMarginBad);
  s.tessDraws = ld(g_tessDraws); s.tessSdk = ld(g_tessSdk); s.cpPm4 = g_cpShaderSyncPm4.get();
  return s;
}
#if SR_ORACLE
bool presentGateCheckAfter(const PresentGateCheck& state, const PPCContext& ctx) {
  if (!state.ready) return false;
  g_gateChk.fetch_add(1, std::memory_order_relaxed);
  if (state.skipped) g_gateChkSkip.fetch_add(1, std::memory_order_relaxed);
  uint8_t cursor[4], flag = 0;
  const uint32_t expectedCursor = state.cursor + (state.skipped ? 0u : 156u);
  bool same = ctx.r3.u64 == state.returnValue && gateRead(state.dev + 40u, cursor, sizeof(cursor)) &&
              be32(cursor) == expectedCursor && gateRead(state.dev + 0x2A39u, &flag, sizeof(flag)) && (flag & 0x20u);
  if (same && !state.skipped) {
    uint8_t bytes[156];
    uint32_t expected[39];
    same = gateRead(state.cursor + 4u, bytes, sizeof(bytes));
    presentGateDwords(state.payload, expected);
    for (uint32_t i = 0; same && i < 39u; ++i) same = be32(bytes + 4u * i) == expected[i];
  }
  if (!same && g_gateChkBad.fetch_add(1, std::memory_order_relaxed) < 8u)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NATIVE_FLUSH_BAD] presentGate CHECK: dev=%08X cursor=%08X expected=%08X r3=%llX skipped=%u",
                state.dev, state.cursor, expectedCursor, (unsigned long long)ctx.r3.u64, unsigned(state.skipped));
  return same;
}
#endif
bool presentGateLeaf(PPCContext& ctx, uint8_t* base) {
  // ShaderConstRunsOracle as well (transport-composed lane, 2026-10-14): see callbackLeaf - the same spinlock re-entry,
  // unreachable today, declined for a uniform oracle contract.
  if (!presentGateOn() || !g_on || !segmentlessActive() || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth)
    return false;
  static const int ab = sr::abt::reg("SR_NATIVE_GATE");
  if (ab >= 0 && !sr::abt::on(ab)) return false;
  uint64_t returnValue = ctx.r3.u64;
  const uint32_t dev = ctx.r3.u32;
  auto decline = [] { g_gateDecl.fetch_add(1, std::memory_order_relaxed); return false; };
  uint8_t device[kDevSpan];
  if (!gateReadMapped(dev, device, sizeof(device))) return decline();  // I15 P2: the device struct (guarded plain read)
  uint8_t* d = sr::kernel::guestVAToHost(dev);
  if (be32(device + 0x33A4u) == 0x80000000u || (be32(device + 0x3490u) & 0xFFFFF000u) == 0u) {
    d[0x2A39u] |= 0x20u;
    g_gateSkip.fetch_add(1, std::memory_order_relaxed);
    ctx.r3.u64 = returnValue;
    return true;
  }
  uint8_t completeDevice[0x5000u], hsio[4];
  // I15 P2: the device struct and the XEX global (guarded plain reads)
  if (!gateReadMapped(dev, completeDevice, sizeof(completeDevice)) || !gateReadMapped(0x829AC0ECu, hsio, 4u))
    return decline();
  const uint8_t* snapshot = completeDevice;
  uint32_t payload[3];
  if (!presentGatePayload(be32(snapshot + 0x2A14u), be32(snapshot + 0x33A4u), be32(snapshot + 0x2D3Cu), payload)) return decline();
  // A guest-mode close would discard markerless footprints. Decline the whole
  // entry before recording anything when the segmentless close cannot own it.
  if ((snapshot[0x2A38u] & 0x80u) || (snapshot[0x2A39u] & 0x42u) ||
      be32(snapshot + 0x4DF4u) || be32(snapshot + 0x4E84u) || be32(hsio)) return decline();
  uint32_t threshold = be32(snapshot + 48u);
  if (g_segStress) {
    const uint64_t stress = uint64_t(be32(snapshot + 13508u)) + g_segStress;
    if (stress > UINT32_MAX) return decline();
    threshold = std::min(threshold, uint32_t(stress));
  }
  const uint32_t writer = be32(snapshot + 0x4E78u);
  const uint32_t slot = writer == 3u ? 0u : writer + 1u;
  const bool perf = be32(snapshot + 0x4E70u) == 2u && slot != be32(snapshot + 0x4E74u);
  const uint32_t cursor = be32(snapshot + 40u), bytes = perf ? 188u : 156u;
  if ((cursor & 3u) || uint64_t(cursor) + bytes + 4u > UINT32_MAX) return decline();
  const bool needsKick = cursor > threshold || (perf && uint64_t(cursor) + 172u > threshold);
  if (!needsKick) {
    if (uint64_t(cursor) + bytes > be32(snapshot + 44u) || !gateSpan(cursor + 4u, bytes)) return decline();
  } else {
    const uint32_t lo = be32(snapshot + 0x34ACu), hi = be32(snapshot + 0x34B0u);
    if (!lo || hi <= lo || cursor < lo || uint64_t(cursor) + 4u > hi ||
        !gateSpan(lo, hi - lo)) return decline();
  }
  ensurePresentGateHandlers();
  Entry entry(ctx, base, dev);
  if (!entry.open()) return decline();
  if (!entry.presentGate(returnValue)) {
    entry.abandon();
    return decline();
  }
  entry.close();
  d[0x2A39u] |= 0x20u;
  ctx.r3.u64 = returnValue;
  g_gateCalls.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// The leaf's CHECK (SR_NATIVE_FLUSH_CHECK, generation): the guest body ran; its bytes must equal the renderer's.
#if SR_ORACLE
void callbackCheck(const PPCContext& in, uint32_t r3) {
  const uint32_t dev = in.r3.u32, at = in.r4.u32;
  const uint8_t* d = dev ? sr::kernel::guestVAToHost(dev) : nullptr;
  if (!d) return;
  const uint32_t pc = be32(d + 0x2A14u), flags = in.r5.u32, m = (flags >> 24) & 0x3Fu;
  const uint32_t w[5] = {in.r6.u32, in.r7.u32, m ? m : 4u, flags,
                         ((((pc >> 20) + 0x200u) & 0x1000u) + (pc & 0x1FFFFFFFu)) | 2u};
  uint32_t v[25];
  const uint32_t k = callbackDwords(w, v);
  g_cbChk.fetch_add(1, std::memory_order_relaxed);
  bool bad = r3 != at + 4u * k;
  for (uint32_t i = 0; i < k && !bad; i++) bad = sr::kernel::readGuestU32BE(at + 4u + 4u * i) != v[i];
  if (bad && g_cbChkBad.fetch_add(1, std::memory_order_relaxed) < 8)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_FLUSH_BAD] callback CHECK: at=%08X flags=%08X cb=%08X r3=%08X (want %08X)",
                at, flags, in.r6.u32, r3, at + 4u * k);
}
#endif

// ============================================================================================
// p2.transport RESIDUE (NATIVE_ROADMAP transport plan item 5, its build note; transport-residue lane 2026-10-14): the
// last guest PM4 of the transport item, three producers each behind its own gate (latched; default ON since the transport
// bake, native/transport-bake 2026-10-15; =0 keeps the guest body and the A/B), each requiring the segmentless record
// transport, each with its own per-present A/B:
//   SR_NATIVE_IDLE_WAIT    D3DDeviceBlockUntilIdle_820FDCB8   WAIT_UNTIL {0x5C8 = 0x20000}       -> a register run
//   SR_NATIVE_INVAL_RANGE  GpuInvalidateMemoryRange_820F4280  owner: WAIT_UNTIL + the COHER block -> run + kRkInvalidate
//   SR_NATIVE_LOCK_COHER   the lock core 820F3988              owner: the COHER block            -> kRkInvalidate / 22
// Each leaf is a port of the generated body (guest/ppc - IDA's MCP was unreachable; the recompiled code is what runs):
// the SDK's own code throughout, the producer's PM4 stores at the cursor replaced by ONE Entry span. Rule 1: the decision
// is taken at the top, before any write; a decline returns false and the caller runs the guest body whole. At the
// record point a late refusal (the Entry's arena wait; a guest mode a helper flipped inside the kick) writes the SDK's
// own dwords instead: from that point the port is the guest body, so the call's output is the SDK's whole, never half
// of each (counted late). Nothing here writes dev+0x2A74, fb+0 or S: BlockUntilIdle's exits stay the SDK's own (the
// fence wait GpuBlockOnFence(S, 4) whose flush closes the record's segment natively, then the spin on dev+0x2A74).
// ============================================================================================

bool idleWaitOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_IDLE_WAIT");
    return !e || e[0] != '0';
  }();
  return on;
}
bool invalRangeOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_INVAL_RANGE");
    return !e || e[0] != '0';
  }();
  return on;
}
bool lockCoherOn() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_LOCK_COHER");
    return !e || e[0] != '0';
  }();
  return on;
}

namespace {
constexpr uint32_t kRsDevSpan = 0x5000u;  // the device fields the residue and the close's guest-mode test read

// The close's whole-body guest modes (flushSegmentless): a record whose segment then closed the guest way would be lost.
bool rsGuestMode(const uint8_t* d) {
  return (d[0x2A38u] & 0x80u) || (d[0x2A39u] & 0x42u) || be32(d + 0x4DF4u) || be32(d + 0x4E84u) ||
         sr::kernel::readGuestU32BE(0x829AC0ECu) != 0;
}

// The owner-thread test the lock core / InvalidateGpuCache make through 82468EF8: KTHREAD(KPCR+0x100)+332 against
// dev+0x2A08, read exactly as the guest reads it.
bool rsOwner(const PPCContext& ctx, uint32_t dev) {
  auto readable = [](uint32_t va) {  // fault-free: the harness VEH makes a host AV fatal
    const uint8_t* h = sr::kernel::guestVAToHost(va);
    return h && sr::kernel::hostRangeReadableCached(h, 4u);
  };
  if (!readable(ctx.r13.u32 + 256u)) return false;
  const uint32_t kt = PPC_LOAD_U32(ctx.r13.u32 + 256);
  if (!readable(kt + 332u)) return false;
  return PPC_LOAD_U32(kt + 332) == PPC_LOAD_U32(dev + 10760);
}

// The decision at the top (everything but the producer's own gate): -1 = admitted (d = the device's host bytes), else
// the decline reason. `owner` is set whenever the device is mapped (the caller needs it for the decline's packet count).
// Read-only.
int rsAdmit(const PPCContext& ctx, uint32_t dev, int ab, uint8_t*& d, bool& owner) {
  d = nullptr;
  owner = false;
  if (dev && !(dev & 3u)) {
    uint8_t* h = sr::kernel::guestVAToHost(dev);
    const uint8_t* e = sr::kernel::guestVAToHost(dev + kRsDevSpan - 1u);
    if (h && e - h == ptrdiff_t(kRsDevSpan - 1u)) d = h;
  }
  if (!d) return kRsMem;
  owner = rsOwner(ctx, dev);
  if (ab >= 0 && !sr::abt::on(ab)) return kRsAb;
  if (!g_on || !segmentlessActive() || g_presentGateOracleDepth || g_shaderConstRunsOracleDepth) return kRsSeg;
  if (rsGuestMode(d)) return kRsMode;
  if ((d[10808] & 0x02u) || sr::gfx::livetrace::armed() || sr::native::dev::on()) return kRsEntry;
  return -1;
}

void rsDecline(int p, int why, uint32_t pk) {
  g_rsDecl[p][why].fetch_add(1, std::memory_order_relaxed);
  if (pk) g_rsLitPk.fetch_add(pk, std::memory_order_relaxed);
}

// The residue's one record point: an Entry span at the cursor `at` (== dev+0x28 by construction: the SDK's own kick
// check just ran) holding what `fill` adds. Re-checked first: segmentless still active and the close's guest-mode
// predicate still false (a helper inside the kick may have flipped it). Refused late: `literal` writes the SDK's own
// dwords there instead. pk = the PM4 packets the span stands for.
#if defined(SR_DIFFTEST)
uint32_t g_rsTestSpanLo = 0, g_rsTestSpanHi = 0;  // the last record span's bytes [lo, hi) (the residue suite's witness)
std::vector<std::array<uint32_t, 3>> g_rsTestSpans;  // every record span since the suite cleared it: {producer, lo, hi}
#endif
template <class Fill, class Literal>
void rsSpan(PPCContext& ctx, uint8_t* base, uint32_t dev, const uint8_t* d, int p, uint32_t at, uint32_t pk, Fill fill,
            Literal literal) {
  bool ok = false;
  if (segmentlessActive() && !rsGuestMode(d) && be32(d + 40) == at) {
    Entry e(ctx, base, dev);
    if (e.open()) {
      if (fill(e)) {
        e.close();
        ok = true;
#if defined(SR_DIFFTEST)
        g_rsTestSpanLo = at + 4u;
        g_rsTestSpanHi = be32(d + 40) + 4u;
        g_rsTestSpans.push_back({uint32_t(p), g_rsTestSpanLo, g_rsTestSpanHi});
#endif
      } else {
        e.abandon();
      }
    }
  }
  if (ok) {
    g_rsPk.fetch_add(pk, std::memory_order_relaxed);
  } else {
    literal();
    g_rsLate[p].fetch_add(1, std::memory_order_relaxed);
    g_rsLitPk.fetch_add(pk, std::memory_order_relaxed);
  }
}
const uint8_t kWaitUntil3dIdle[4] = {0x00u, 0x02u, 0x00u, 0x00u};  // WAIT_UNTIL 0x5C8 = 0x00020000, big-endian
}  // namespace

// D3DDeviceBlockUntilIdle_820FDCB8(dev r3), ctx-style in the generated code (tools/regen keep_ctx_style). The SDK:
// [dev+0x28 > dev+0x30 -> 820FD8C0], WAIT_UNTIL {0x5C8 = 0x20000} at the cursor, dev+0x28 += 8, then if S = dev+0x2A1C:
// GpuBlockOnFence(dev, S, 4), then while (dev+0x2A74 != 0). Only the two stwu become the record.
bool idleWaitLeaf(PPCContext& ctx, uint8_t* base) {
  if (!idleWaitOn()) return false;
  static const int ab = sr::abt::reg("SR_NATIVE_IDLE_WAIT");
  const uint32_t dev = ctx.r3.u32;
  uint8_t* d = nullptr;
  bool owner = false;
  const int why = rsAdmit(ctx, dev, ab, d, owner);
  if (why >= 0 || !owner) {  // the guest body writes its WAIT_UNTIL on any thread
    rsDecline(kRsIdle, why >= 0 ? why : kRsOwner, 1u);
    return false;
  }
  // ---- the port of the generated 820FDCB8 ----
  uint32_t ea{};
  ctx.r12.u64 = ctx.lr;
  PPC_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
  PPC_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64);
  ea = -96 + ctx.r1.u32;
  PPC_STORE_U32(ea, ctx.r1.u32);
  ctx.r1.u32 = ea;
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r11.u64 = PPC_LOAD_U32(ctx.r31.u32 + 48);
  ctx.r3.u64 = PPC_LOAD_U32(ctx.r31.u32 + 40);
  ctx.cr6.compare<uint32_t>(ctx.r3.u32, ctx.r11.u32, ctx.xer);
  if (ctx.cr6.gt) {
    ctx.r3.u64 = ctx.r31.u64;
    ctx.lr = 0x820FDCE4;
    sub_820FD8C0(ctx, base);
  }
  // loc_820FDCE4: the WAIT_UNTIL at the cursor r3 - the record in place of `stwu r11,4(r3); stwu r10,4(r3)`.
  ctx.r11.s64 = 1480;
  ctx.r10.s64 = 131072;
  {
    const uint32_t at = ctx.r3.u32;
    rsSpan(ctx, base, dev, d, kRsIdle, at, 1u, [&](Entry& e) { return e.run(0x5C8u, 1u, kWaitUntil3dIdle); },
           [&] {
             PPC_STORE_U32(at + 4u, ctx.r11.u32);
             PPC_STORE_U32(at + 8u, ctx.r10.u32);
           });
    ctx.r3.u32 = at + 8u;  // the stwu chain leaves r3 at its last effective address
  }
  ctx.r4.u64 = PPC_LOAD_U32(ctx.r31.u32 + 10780);
  ctx.cr0.compare<uint32_t>(ctx.r4.u32, 0, ctx.xer);
  PPC_STORE_U32(ctx.r31.u32 + 40, ctx.r3.u32);  // the record span's close published the same cursor
  if (!ctx.cr0.eq) {
    ctx.r5.s64 = 4;
    ctx.r3.u64 = ctx.r31.u64;
    ctx.lr = 0x820FDD10;
    sub_820FD9A8(ctx, base);  // the SDK's fence wait: its F == S flush closes the record's segment (FENCE S)
  }
  // loc_820FDD10: the SDK's own drain spin - dev+0x2A74 is moved only by the guest's 820FD800 / EndSync 82110768.
  do {
    ctx.r11.u64 = PPC_LOAD_U32(ctx.r31.u32 + 10868);
    ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
  } while (!ctx.cr6.eq);
  ctx.r1.s64 = ctx.r1.s64 + 96;
  ctx.r12.u64 = PPC_LOAD_U32(ctx.r1.u32 + -8);
  ctx.lr = ctx.r12.u64;
  ctx.r31.u64 = PPC_LOAD_U64(ctx.r1.u32 + -16);
  g_rsTaken[kRsIdle].fetch_add(1, std::memory_order_relaxed);
  return true;
}

// GpuInvalidateMemoryRange_820F4280(r3, base r4, size r5) - the device from VdGlobalDevice; localized in the generated
// code (r27..r31 locals, no lr bookkeeping). The owner thread: [kick check], 13 dwords - WAIT_UNTIL {0x20000},
// COHER_STATUS_HOST 0x03000000, COHER_SIZE / BASE {page-rounded size, base}, WAIT_REG_MEM {3, 0xA31, 0, 0x80000000, 8}
// - which become one span: the run {0x5C8} and kRkInvalidate {base, size, 0}. Another thread only merges [phys, phys +
// size) into dev+0x2D18 (no PM4): the port runs the SDK's merge loop as written (counted merge), so the body is the
// SDK's whole whichever branch the SDK's own owner test takes.
bool invalRangeLeaf(PPCContext& ctx, uint8_t* base) {
  if (!invalRangeOn()) return false;
  static const int ab = sr::abt::reg("SR_NATIVE_INVAL_RANGE");
  const uint32_t dev = PPC_LOAD_U32(PPC_LOAD_U32(0x82000948u));  // *(*VdGlobalDevice cell), as the body loads it
  uint8_t* d = nullptr;
  bool owner = false;
  const int why = rsAdmit(ctx, dev, ab, d, owner);
  if (why >= 0) {
    rsDecline(kRsInval, why, owner ? 4u : 0u);  // the owner's guest body writes its 4 packets
    return false;
  }
  // ---- the port of the generated 820F4280 ----
  if (invalRangeOn()) {  // the gate again as the block condition (the forcing audit drops guards at labels)
    PPCXERRegister xer{};
    PPCRegister reserved{};
    PPCCRRegister cr0{};
    PPCCRRegister cr6{};
    PPCRegister r11{};
    PPCRegister r27{};
    PPCRegister r28{};
    PPCRegister r29{};
    PPCRegister r30{};
    PPCRegister r31{};
    uint32_t ea{};
    uint32_t at{};
    ea = -128 + ctx.r1.u32;
    PPC_STORE_U32(ea, ctx.r1.u32);
    ctx.r1.u32 = ea;
    r11.s64 = -2113929216;
    ctx.r9.u64 = __builtin_rotateleft64(ctx.r4.u32 | (ctx.r4.u64 << 32), 12) & 0xFFF;
    ctx.r10.u64 = ctx.r4.u32 & 0x1FFFFFFF;
    r11.u64 = PPC_LOAD_U32(r11.u32 + 2376);
    r31.u64 = PPC_LOAD_U32(r11.u32 + 0);
    r11.s64 = ctx.r9.s64 + 512;
    r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 0) & 0x1000;
    r30.u64 = r11.u64 + ctx.r10.u64;
    r29.u64 = r30.u64 + ctx.r5.u64;
    sub_82468EF8(ctx, base);
    r11.u64 = PPC_LOAD_U32(r31.u32 + 10760);
    cr6.compare<uint32_t>(r11.u32, ctx.r3.u32, xer);
    if (cr6.eq) goto loc_820F4334;
    ctx.r8.s64 = r31.s64 + 11544;
loc_820F42C4:
    r11.u64 = PPC_LOAD_U64(ctx.r8.u32 + 0);
    ctx.r10.u64 = r11.u64;
    cr6.compare<uint32_t>(r30.u32, ctx.r10.u32, xer);
    if (!cr6.lt) goto loc_820F42D8;
    ctx.r10.u64 = r30.u64;
loc_820F42D8:
    ctx.r9.u64 = __builtin_rotateleft64(r11.u64, 32) & 0xFFFFFFFF;
    ctx.r9.u64 = __builtin_rotateleft32(ctx.r9.u32, 0);
    cr6.compare<uint32_t>(r29.u32, ctx.r9.u32, xer);
    if (!cr6.gt) goto loc_820F42EC;
    ctx.r9.u64 = r29.u64;
loc_820F42EC:
    ctx.r9.u64 = __builtin_rotateleft64(ctx.r9.u64, 32) & 0xFFFFFFFF00000000;
    ctx.r10.u64 = ctx.r10.u64 & 0xFFFFFFFF;
    ctx.r10.u64 = ctx.r9.u64 | ctx.r10.u64;
loc_820F42F8:
    reserved.u64 = *(uint64_t*)(base + PPC_GUEST_OFFSET(ctx.r8.u32));
    ctx.r7.u64 = __builtin_bswap64(reserved.u64);
    cr6.compare<int64_t>(ctx.r7.s64, r11.s64, xer);
    if (!cr6.eq) goto loc_820F431C;
    cr0.lt = 0;
    cr0.gt = 0;
    cr0.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + PPC_GUEST_OFFSET(ctx.r8.u32)),
                                          reserved.s64, __builtin_bswap64(ctx.r10.s64));
    cr0.so = xer.so;
    if (!cr0.eq) goto loc_820F42F8;
    goto loc_820F4324;
loc_820F431C:
    cr0.lt = 0;
    cr0.gt = 0;
    cr0.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + PPC_GUEST_OFFSET(ctx.r8.u32)),
                                          reserved.s64, __builtin_bswap64(ctx.r7.s64));
    cr0.so = xer.so;
loc_820F4324:
    ctx.r10.u64 = ctx.r7.u64;
    cr6.compare<uint64_t>(ctx.r10.u64, r11.u64, xer);
    if (!cr6.eq) goto loc_820F42C4;
    g_rsMerge[kRsInval].fetch_add(1, std::memory_order_relaxed);
    goto loc_820F43CC;
loc_820F4334:
    ctx.r10.u64 = PPC_LOAD_U32(r31.u32 + 48);
    r11.u64 = PPC_LOAD_U32(r31.u32 + 40);
    cr6.compare<uint32_t>(r11.u32, ctx.r10.u32, xer);
    if (!cr6.gt) goto loc_820F4350;
    ctx.r3.u64 = r31.u64;
    sub_820FD8C0(ctx, base);
    r11.u64 = ctx.r3.u64;
loc_820F4350:
    ctx.r10.s64 = 1480;
    ctx.r7.s64 = 131072;
    ctx.r8.s64 = 2609;
    ctx.r6.s64 = 50331648;
    ctx.r9.s64 = 65536;
    ctx.r5.s64 = r29.s64 + 4095;
    ctx.r4.u64 = ctx.r9.u64 | 2607;
    ctx.r10.u64 = __builtin_rotateleft64(r30.u32 | (r30.u64 << 32), 0) & 0xFFFFF000;
    ctx.r9.u64 = __builtin_rotateleft64(ctx.r5.u32 | (ctx.r5.u64 << 32), 0) & 0xFFFFF000;
    ctx.r5.s64 = -1073479680;
    ctx.r9.s64 = ctx.r9.s64 - ctx.r10.s64;
    ctx.r5.u64 = ctx.r5.u64 | 15360;
    ctx.r3.s64 = 3;
    r30.s64 = 2609;
    r29.s64 = 0;
    r28.s64 = -2147483648;
    r27.s64 = 8;
    // The 13 stwu at r11+4.. (1480, 0x20000, 2609, 0x03000000, r4, r9, r10, r5, r3, r30, r29, r28, r27) - the span.
    at = r11.u32;
    rsSpan(ctx, base, r31.u32, d, kRsInval, at, 4u,
           [&](Entry& e) {
             return e.run(0x5C8u, 1u, kWaitUntil3dIdle) && e.coherency(0x03000000u, ctx.r9.u32, ctx.r10.u32);
           },
           [&] {
             const uint32_t v[13] = {1480u, 131072u, 2609u, 50331648u, ctx.r4.u32, ctx.r9.u32, ctx.r10.u32, ctx.r5.u32,
                                     ctx.r3.u32, r30.u32, r29.u32, r28.u32, r27.u32};
             for (uint32_t k = 0; k < 13u; k++) PPC_STORE_U32(at + 4u + 4u * k, v[k]);
           });
    r11.u32 = at + 52u;
    PPC_STORE_U32(r31.u32 + 40, r11.u32);
    g_rsTaken[kRsInval].fetch_add(1, std::memory_order_relaxed);
loc_820F43CC:
    ctx.r1.s64 = ctx.r1.s64 + 128;
    return true;
  }
  return false;
}

// The lock core 820F3988(res r3, reason r4, r5, base r6, r7, addr r8, size r9, flags r10, COHER status at the caller's
// sp+0x54) - localized in the generated code. The resource's fence wait GpuBlockOnFence(res+8 / +0xC, reason); then,
// if !(flags & 0x12): on the owner thread [kick check] + the 11-dword COHER block (one span: Entry::coherency -
// kRkInvalidate for 0x03000000 / 0x01000000, kRkCoherHost otherwise), on another thread the dev+0x2D18 merge (as written,
// counted merge); then the lock bookkeeping (dirty range, the physical view, res+0 += 0x100) and r3. Declined at the
// top: flags & 0x12 (IndexBuffer_Lock always: the guest body writes no PM4, nothing to convert).
bool lockCoherLeaf(PPCContext& ctx, uint8_t* base) {
  if (!lockCoherOn()) return false;
  static const int ab = sr::abt::reg("SR_NATIVE_LOCK_COHER");
  if (ctx.r10.u32 & 0x12u) {
    rsDecline(kRsLock, kRsNoPk, 0u);
    return false;
  }
  const uint32_t dev = PPC_LOAD_U32(PPC_LOAD_U32(0x82000948u));  // *(*VdGlobalDevice cell), as the body loads it
  uint8_t* d = nullptr;
  bool owner = false;
  const int why = rsAdmit(ctx, dev, ab, d, owner);
  if (why >= 0) {
    rsDecline(kRsLock, why, owner ? 3u : 0u);  // the owner's guest body writes its 3 packets
    return false;
  }
  // ---- the port of the generated 820F3988 ----
  if (lockCoherOn()) {  // the gate again as the block condition (the forcing audit drops guards at labels)
    PPCXERRegister xer{};
    PPCRegister reserved{};
    PPCCRRegister cr0{};
    PPCCRRegister cr6{};
    PPCRegister r11{};
    PPCRegister r22{};
    PPCRegister r23{};
    PPCRegister r24{};
    PPCRegister r25{};
    PPCRegister r26{};
    PPCRegister r27{};
    PPCRegister r28{};
    PPCRegister r29{};
    PPCRegister r30{};
    PPCRegister r31{};
    uint32_t ea{};
    uint32_t at{};
    ea = -176 + ctx.r1.u32;
    PPC_STORE_U32(ea, ctx.r1.u32);
    ctx.r1.u32 = ea;
    r25.u64 = ctx.r3.u64;
    r27.u64 = ctx.r9.u64;
    r26.u64 = ctx.r10.u64;
    r22.u64 = ctx.r5.u64;
    r23.u64 = ctx.r6.u64;
    r11.u64 = PPC_LOAD_U32(r25.u32 + 0);
    r24.u64 = ctx.r7.u64;
    r28.u64 = ctx.r8.u64;
    ctx.r9.u64 = r11.u32 & 0xF;
    ctx.r10.u64 = r25.u64;
    cr6.compare<uint32_t>(ctx.r9.u32, 4, xer);
    if (!cr6.eq) goto loc_820F39DC;
    r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 0) & 0x40000000;
    cr0.compare<int32_t>(r11.s32, 0, xer);
    if (cr0.eq) goto loc_820F39DC;
    r11.u64 = PPC_LOAD_U32(r25.u32 + 24);
    cr0.compare<uint32_t>(r11.u32, 0, xer);
    if (cr0.eq) goto loc_820F39DC;
    ctx.r10.u64 = r11.u64;
loc_820F39DC:
    r11.u64 = r26.u64 & 4112;
    cr0.compare<int32_t>(r11.s32, 0, xer);
    cr0.compare<uint32_t>(r11.u32, 0, xer);
    if (cr0.eq) goto loc_820F39F0;
    r11.u64 = PPC_LOAD_U32(ctx.r10.u32 + 12);
    goto loc_820F39F4;
loc_820F39F0:
    r11.u64 = PPC_LOAD_U32(ctx.r10.u32 + 8);
loc_820F39F4:
    cr6.compare<uint32_t>(r11.u32, 0, xer);
    r31.s64 = -2113929216;
    if (cr6.eq) goto loc_820F3A14;
    ctx.r5.u64 = ctx.r4.u64;
    ctx.r4.u64 = r11.u64;
    r11.u64 = PPC_LOAD_U32(r31.u32 + 2376);
    ctx.r3.u64 = PPC_LOAD_U32(r11.u32 + 0);
    sub_820FD9A8(ctx, base);  // the resource's fence wait (the SDK's; never the residue's)
loc_820F3A14:
    r11.u64 = r26.u64 & 18;
    cr0.compare<int32_t>(r11.s32, 0, xer);
    cr0.compare<uint32_t>(r11.u32, 0, xer);
    if (!cr0.eq) goto loc_820F3B4C;
    r11.u64 = PPC_LOAD_U32(r31.u32 + 2376);
    ctx.r9.u64 = __builtin_rotateleft64(r28.u32 | (r28.u64 << 32), 12) & 0xFFF;
    ctx.r10.u64 = r28.u32 & 0x1FFFFFFF;
    r31.u64 = PPC_LOAD_U32(r11.u32 + 0);
    r11.s64 = ctx.r9.s64 + 512;
    r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 0) & 0x1000;
    r30.u64 = r11.u64 + ctx.r10.u64;
    r29.u64 = r30.u64 + r27.u64;
    sub_82468EF8(ctx, base);
    r11.u64 = PPC_LOAD_U32(r31.u32 + 10760);
    cr6.compare<uint32_t>(r11.u32, ctx.r3.u32, xer);
    if (cr6.eq) goto loc_820F3AC4;
    ctx.r8.s64 = r31.s64 + 11544;
loc_820F3A54:
    r11.u64 = PPC_LOAD_U64(ctx.r8.u32 + 0);
    ctx.r10.u64 = r11.u64;
    cr6.compare<uint32_t>(r30.u32, ctx.r10.u32, xer);
    if (!cr6.lt) goto loc_820F3A68;
    ctx.r10.u64 = r30.u64;
loc_820F3A68:
    ctx.r9.u64 = __builtin_rotateleft64(r11.u64, 32) & 0xFFFFFFFF;
    ctx.r9.u64 = __builtin_rotateleft32(ctx.r9.u32, 0);
    cr6.compare<uint32_t>(r29.u32, ctx.r9.u32, xer);
    if (!cr6.gt) goto loc_820F3A7C;
    ctx.r9.u64 = r29.u64;
loc_820F3A7C:
    ctx.r9.u64 = __builtin_rotateleft64(ctx.r9.u64, 32) & 0xFFFFFFFF00000000;
    ctx.r10.u64 = ctx.r10.u64 & 0xFFFFFFFF;
    ctx.r10.u64 = ctx.r9.u64 | ctx.r10.u64;
loc_820F3A88:
    reserved.u64 = *(uint64_t*)(base + PPC_GUEST_OFFSET(ctx.r8.u32));
    ctx.r7.u64 = __builtin_bswap64(reserved.u64);
    cr6.compare<int64_t>(ctx.r7.s64, r11.s64, xer);
    if (!cr6.eq) goto loc_820F3AAC;
    cr0.lt = 0;
    cr0.gt = 0;
    cr0.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + PPC_GUEST_OFFSET(ctx.r8.u32)),
                                          reserved.s64, __builtin_bswap64(ctx.r10.s64));
    cr0.so = xer.so;
    if (!cr0.eq) goto loc_820F3A88;
    goto loc_820F3AB4;
loc_820F3AAC:
    cr0.lt = 0;
    cr0.gt = 0;
    cr0.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + PPC_GUEST_OFFSET(ctx.r8.u32)),
                                          reserved.s64, __builtin_bswap64(ctx.r7.s64));
    cr0.so = xer.so;
loc_820F3AB4:
    ctx.r10.u64 = ctx.r7.u64;
    cr6.compare<uint64_t>(ctx.r10.u64, r11.u64, xer);
    if (!cr6.eq) goto loc_820F3A54;
    g_rsMerge[kRsLock].fetch_add(1, std::memory_order_relaxed);
    goto loc_820F3B4C;
loc_820F3AC4:
    ctx.r10.u64 = PPC_LOAD_U32(r31.u32 + 48);
    r11.u64 = PPC_LOAD_U32(r31.u32 + 40);
    cr6.compare<uint32_t>(r11.u32, ctx.r10.u32, xer);
    if (!cr6.gt) goto loc_820F3AE0;
    ctx.r3.u64 = r31.u64;
    sub_820FD8C0(ctx, base);
    r11.u64 = ctx.r3.u64;
loc_820F3AE0:
    ctx.r9.s64 = 2609;
    ctx.r8.u64 = PPC_LOAD_U32(ctx.r1.u32 + 260);  // the caller's COHER_STATUS_HOST (its sp+0x54)
    ctx.r7.s64 = 65536;
    ctx.r6.s64 = r29.s64 + 4095;
    ctx.r7.u64 = ctx.r7.u64 | 2607;
    ctx.r10.u64 = __builtin_rotateleft64(r30.u32 | (r30.u64 << 32), 0) & 0xFFFFF000;
    ctx.r9.u64 = __builtin_rotateleft64(ctx.r6.u32 | (ctx.r6.u64 << 32), 0) & 0xFFFFF000;
    ctx.r6.s64 = -1073479680;
    ctx.r9.s64 = ctx.r9.s64 - ctx.r10.s64;
    ctx.r6.u64 = ctx.r6.u64 | 15360;
    ctx.r5.s64 = 3;
    ctx.r4.s64 = 2609;
    ctx.r3.s64 = 0;
    r30.s64 = -2147483648;
    r29.s64 = 8;
    // The 11 stwu at r11+4.. (2609, status, r7, r9, r10, r6, r5, r4, r3, r30, r29) - the span.
    at = r11.u32;
    if (ctx.r8.u32 != 0x03000000u && ctx.r8.u32 != 0x01000000u) g_rsHost.fetch_add(1, std::memory_order_relaxed);
    rsSpan(ctx, base, r31.u32, d, kRsLock, at, 3u,
           [&](Entry& e) { return e.coherency(ctx.r8.u32, ctx.r9.u32, ctx.r10.u32); },
           [&] {
             const uint32_t v[11] = {2609u, ctx.r8.u32, ctx.r7.u32, ctx.r9.u32, ctx.r10.u32, ctx.r6.u32,
                                     ctx.r5.u32, ctx.r4.u32, ctx.r3.u32, r30.u32, r29.u32};
             for (uint32_t k = 0; k < 11u; k++) PPC_STORE_U32(at + 4u + 4u * k, v[k]);
           });
    r11.u32 = at + 44u;
    PPC_STORE_U32(r31.u32 + 40, r11.u32);
    g_rsTaken[kRsLock].fetch_add(1, std::memory_order_relaxed);
loc_820F3B4C:
    ctx.r6.u64 = __builtin_rotateleft64(r26.u32 | (r26.u64 << 32), 0) & 0x10;
    cr0.compare<int32_t>(ctx.r6.s32, 0, xer);
    if (!cr0.eq) goto loc_820F3B60;
    r11.u64 = PPC_LOAD_U32(r25.u32 + 0);
    r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 0) & 0x200000;
    cr0.compare<int32_t>(r11.s32, 0, xer);
    if (cr0.eq) goto loc_820F3BEC;
loc_820F3B60:
    r11.u64 = r26.u32 & 0x1;
    cr0.compare<int32_t>(r11.s32, 0, xer);
    if (!cr0.eq) goto loc_820F3BCC;
    cr6.compare<uint32_t>(r22.u32, 0, xer);
    if (cr6.eq) goto loc_820F3B84;
    cr6.compare<uint32_t>(r24.u32, 0, xer);
    if (cr6.eq) goto loc_820F3B84;
    r11.s64 = r28.s64 - r24.s64;
    ctx.r8.s64 = r25.s64 + 24;
    goto loc_820F3B8C;
loc_820F3B84:
    r11.s64 = r28.s64 - r23.s64;
    ctx.r8.s64 = r25.s64 + 20;
loc_820F3B8C:
    ctx.r9.u64 = r11.u64 + r27.u64;
    ctx.r10.u64 = PPC_LOAD_U32(ctx.r8.u32 + 0);
    ctx.r7.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 25) & 0x1FFFFFF;
    r11.s64 = ctx.r9.s64 + 127;
    ctx.r9.u64 = ctx.r10.u32 & 0xFFFF;
    r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 25) & 0x1FFFFFF;
    ctx.r10.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 16) & 0xFFFF;
    cr6.compare<uint32_t>(r11.u32, ctx.r9.u32, xer);
    if (cr6.gt) goto loc_820F3BB4;
    r11.u64 = ctx.r9.u64;
loc_820F3BB4:
    cr6.compare<uint32_t>(ctx.r7.u32, ctx.r10.u32, xer);
    if (!cr6.lt) goto loc_820F3BC0;
    ctx.r10.u64 = ctx.r7.u64;
loc_820F3BC0:
    ctx.r10.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 16) & 0xFFFF0000;
    r11.u64 = ctx.r10.u64 | r11.u64;
    PPC_STORE_U32(ctx.r8.u32 + 0, r11.u32);
loc_820F3BCC:
    cr6.compare<uint32_t>(ctx.r6.u32, 0, xer);
    if (cr6.eq) goto loc_820F3BEC;
    r11.u64 = __builtin_rotateleft64(r28.u32 | (r28.u64 << 32), 12) & 0xFFF;
    ctx.r10.u64 = r28.u32 & 0x1FFFFFFF;
    r11.s64 = r11.s64 + 512;
    r11.u64 = __builtin_rotateleft64(r11.u32 | (r11.u64 << 32), 0) & 0x1000;
    r11.u64 = r11.u64 + ctx.r10.u64;
    r28.s64 = r11.s64 + -1073741824;
loc_820F3BEC:
    r11.s64 = 256;
loc_820F3BF0:
    reserved.u32 = *(uint32_t*)(base + PPC_GUEST_OFFSET(r25.u32));
    ctx.r10.u64 = __builtin_bswap32(reserved.u32);
    ctx.r9.u64 = r11.u64 + ctx.r10.u64;
    cr0.lt = 0;
    cr0.gt = 0;
    cr0.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + PPC_GUEST_OFFSET(r25.u32)), reserved.s32,
                                          __builtin_bswap32(ctx.r9.s32));
    cr0.so = xer.so;
    if (!cr0.eq) goto loc_820F3BF0;
    ctx.r3.u64 = r28.u64;
    ctx.r1.s64 = ctx.r1.s64 + 176;
    return true;
  }
  return false;
}
}  // namespace sr::native::flush

// Shared SDK entry: direct draw callers and the generated function table use
// the same replacement. CHECK disables record mode, leaving the SDK independent.
extern "C" void __imp__sub_821075E0(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_821075E0) {
  if (sr::native::flush::shaderConstRunsOn() && sr::native::flush::shaderConstantRunsLeaf(ctx, base)) return;
  __imp__sub_821075E0(ctx, base);
}

// D3DDeviceEmitDrawTessellated 820FC478 (observe; the construns bake, 2026-10-14): counts tessellated draws and marks
// the thread so the SDK leaf above counts the 821075E0 calls made inside one (tessDraws= / tessSdk= on the
// shaderConstRuns line) - the live witness for p1.row3_construns' "rendered tessellation". Writes nothing to the guest;
// the body runs exactly once. Its census scope (formerly the generated shim) is the first statement. A development
// oracle like that shim (which lived in an ORACLE_SOURCES file): the release configuration (SR_ORACLE=0) keeps the
// recompiled body in its place, as before.
#if SR_ORACLE
extern "C" void __imp__sub_820FC478(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_820FC478) {
  SR_NATIVE_SCOPE(0x820FC478);  // NATIVE N0 (scripts/gen_native_brackets.py)
  sr::native::flush::tessEnter();
  __imp__sub_820FC478(ctx, base);
  sr::native::flush::tessLeave();
}
#endif

// GpuEmitCpuCallbackPackets_820FDA80 (p2.transport step 1), under SR_NATIVE_CALLBACK on the segmentless transport; the
// guest body otherwise (checked against the renderer under SR_NATIVE_FLUSH_CHECK).
extern "C" void __imp__sub_820FDA80(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_820FDA80) {
  if (sr::native::flush::callbackOn() && sr::native::flush::callbackLeaf(ctx, base)) return;
#if SR_ORACLE
  if (sr::native::flush::checkOn()) {
    const PPCContext in = ctx;
    __imp__sub_820FDA80(ctx, base);
    sr::native::flush::callbackCheck(in, ctx.r3.u32);
    return;
  }
#endif
  __imp__sub_820FDA80(ctx, base);
}

// EmitPm4Packet (P2-B item 5), under SR_NATIVE_SETCONSTMEM (default ON since 2026-10-14); the guest body otherwise
// (=0, a declined call, CHECK mode), bracketed by the dry CHECK (kind 7) under SR_NATIVE_FLUSH_CHECK. Either way the
// native device's record-only observer
// (gfx/native_device onConstantPacket, SR_NATIVE_RECORD) sees the body VA, as the former observe
// override in hle_api_observe.cpp let it.
extern "C" void __imp__sub_82113010(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82113010) {
  SR_NATIVE_SCOPE(0x82113010);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const uint32_t dev = ctx.r3.u32, a2 = ctx.r4.u32, a3 = ctx.r5.u32, count = ctx.r6.u32;
  if (sr::native::flush::setConstMemOn() && sr::native::flush::emitPm4PacketLeaf(ctx, base)) {
    if (sr::native::dev::on()) sr::native::dev::onConstantPacket(dev, a2, a3, count, ctx.r3.u32);
    return;
  }
  const bool chk = sr::native::flush::checkOn();
  if (chk) sr::native::flush::checkBefore(ctx, base, 7);
  __imp__sub_82113010(ctx, base);
  if (chk) sr::native::flush::checkAfter(ctx, base, 7);
  if (sr::native::dev::on()) sr::native::dev::onConstantPacket(dev, a2, a3, count, ctx.r3.u32);
}

// boot.device_create: D3DDeviceResetState_8210E520 (the device-create state reset) and the segment re-emit
// 820F0048 (the boot call from 82106010 and Swap's per-frame call), under SR_NATIVE_DEVCREATE on the native
// transport; the guest body otherwise, bracketed by the dry CHECK (kinds 8 / 9) under SR_NATIVE_FLUSH_CHECK.
extern "C" void __imp__sub_8210E520(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8210E520) {
  if (sr::native::flush::devCreateOn() && sr::native::flush::devResetLeaf(ctx, base)) return;
  const bool chk = sr::native::flush::checkOn();
  if (chk) sr::native::flush::checkBefore(ctx, base, 8);
  __imp__sub_8210E520(ctx, base);
  if (chk) sr::native::flush::checkAfter(ctx, base, 8);
}
extern "C" void __imp__sub_820F0048(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_820F0048) {
  if (sr::native::flush::devCreateOn() && sr::native::flush::segmentBeginLeaf(ctx, base)) return;
  const bool chk = sr::native::flush::checkOn();
  if (chk) sr::native::flush::checkBefore(ctx, base, 9);
  __imp__sub_820F0048(ctx, base);
  if (chk) sr::native::flush::checkAfter(ctx, base, 9);
}
// boot gaps: GpuRingEmitModeStateReg_82103238 (D3DDeviceInitDefaultRenderStates 821058B0's mode-state write at device
// creation) under SR_NATIVE_MODE_STATE (default ON) on the native transport; the guest body otherwise, bracketed by the
// dry CHECK (kind 10) under SR_NATIVE_FLUSH_CHECK.
extern "C" void __imp__sub_82103238(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82103238) {
  if (sr::native::flush::modeStateOn() && sr::native::flush::modeStateLeaf(ctx, base)) return;
  const bool chk = sr::native::flush::checkOn();
  if (chk) sr::native::flush::checkBefore(ctx, base, 10);
  __imp__sub_82103238(ctx, base);
  if (chk) sr::native::flush::checkAfter(ctx, base, 10);
}

// GpuReplayListBeginRecording_82110B30 (p2.transport BRACKET_BEGIN, SR_NATIVE_BRACKET): the decision at the top, the
// SDK body whole (its close and its CPU list bookkeeping, as today), then the native generation opens. Gate off or
// declined: exactly the SDK body.
extern "C" void __imp__sub_82110B30(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82110B30) {
  const uint32_t dev = ctx.r3.u32;
  const bool native = sr::native::flush::bracketOn() && sr::native::flush::bracketBeginDecide(ctx);
  __imp__sub_82110B30(ctx, base);
  if (native) sr::native::flush::bracketOpen(dev);
}

// p2.transport RESIDUE (NATIVE_ROADMAP transport plan item 5 build note). GpuInvalidateMemoryRange_820F4280 (its N0 shim
// gave way to this override, which keeps the scope) under SR_NATIVE_INVAL_RANGE and the lock core 820F3988 under
// SR_NATIVE_LOCK_COHER, both default ON since the transport bake: the gate off (=0) or a decline at the top = exactly the
// guest body.
extern "C" void __imp__sub_820F4280(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_820F4280) {
  SR_NATIVE_SCOPE(0x820F4280);  // NATIVE N0 (scripts/gen_native_brackets.py)
  if (sr::native::flush::invalRangeOn() && sr::native::flush::invalRangeLeaf(ctx, base)) return;
  __imp__sub_820F4280(ctx, base);
}
extern "C" void __imp__sub_820F3988(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_820F3988) {
#if SR_ORACLE
  sr::kernel::reader_observe::lockIntent(ctx.r3.u32, ctx.r4.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
  // the guest-reader census (resproof): the same lock graded after the body (its fence wait done; read-only locks too)
  const bool readerOn = sr::kernel::reader_observe::enabled();
  const uint32_t lr3 = ctx.r3.u32, lr4 = ctx.r4.u32, lr8 = ctx.r8.u32, lr9 = ctx.r9.u32, lr10 = ctx.r10.u32;
#endif
  if (sr::native::flush::lockCoherOn() && sr::native::flush::lockCoherLeaf(ctx, base)) {
#if SR_ORACLE
    if (readerOn) sr::kernel::reader_observe::lockGraded(lr3, lr4, lr8, lr9, lr10);
#endif
    return;
  }
  __imp__sub_820F3988(ctx, base);
#if SR_ORACLE
  if (readerOn) sr::kernel::reader_observe::lockGraded(lr3, lr4, lr8, lr9, lr10);
#endif
}

#if defined(SR_DIFFTEST)
// Compiled only into difftest. Fixtures exercise this translation unit's actual
// record ownership, registry, publication and consumer, rather than a copy.
#include "../tools/difftest/native_segment_ownership_test.inc"
#include "../tools/difftest/native_record_failure_test.inc"
#include "../tools/difftest/suite_bare.inc"
#include "../tools/difftest/native_bracket_test.inc"
#include "../tools/difftest/native_residue_test.inc"
#include "../tools/difftest/coher_sentinel_test.inc"
#include "../tools/difftest/native_census_test.inc"  // after native_residue_test.inc: reuses its rt:: fixture
#include "../tools/difftest/setconst_render_test.inc"  // P2-B item 5: kRkSetConstMem vs the SDK, its CPPM4 render-back
#include "../tools/difftest/flush_split_test.inc"  // p1.flush_residue: SR_NATIVE_FLUSH_SPLIT vs the SDK's chunks / splits
#include "../tools/difftest/zpd_port_test.inc"  // p2.zpd's bake: 82101E08's port (kRkZpd) vs the SDK; after setconst_render_test.inc (its fixture)
#endif
