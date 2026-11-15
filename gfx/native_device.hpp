#pragma once

// NATIVE ROADMAP N1 (docs/NATIVE_ROADMAP.md §7 N1, mode A "record-only"): the
// native device core's draw-time state tracker, in record-only form.
//
// What it does (gate SR_NATIVE_RECORD=1, default OFF; implies SR_NATIVE_BRACKETS
// and SR_PM4_COVERAGE):
//   * At every XDK draw entry the game calls (DrawIndexedVertices 820FC000,
//     DrawVertices 820FBBF8, DrawVerticesUP 820FBBB0, BeginVertices 820FB6E8),
//     BEFORE the XDK runs, it projects the guest device shadow onto Xenos
//     registers the way a native renderer would read it (UnleashedRecomp
//     video.cpp FlushRenderStateForMainThread model): the packed register shadow
//     from dev+0x2800 (ENGINE_MAP "NATIVE N0"), the fetch constants dev+0x400,
//     the VS/PS float constants dev+0x700/+0x1700 and the bool/loop constants
//     dev+0x2700. It never clears a dirty mask and never writes guest memory.
//     After the XDK returns it snapshots the register shadow again (to classify
//     a mismatch as "derived by the XDK flush" vs "never in the shadow").
//   * The record is keyed by the bracket callId (kernel/native_brackets). On the
//     CP thread, gfx/pm4_coverage looks up the owner bracket of every DRAW_INDX /
//     DRAW_INDX_2 it is about to execute and calls checkDraw(): the native
//     projection is compared with the CP's register file at that moment, field by
//     field (roadmap §6.2 L1, in process). The PM4 path stays the oracle.
//
// Log tags: [NATIVE_REC] (periodic + final summary), [NATIVE_REC_MISMATCH]
// (capped examples). Counters on the CP side are confined to the CP thread.

#include <cstdint>

#include "../kernel/sr_oracle.hpp"

namespace sr::native::dev {

#if SR_ORACLE
extern bool g_recordOn;
inline bool on() { return g_recordOn; }
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): native_device.cpp is compiled out (CMakeLists.txt ORACLE_SOURCES). Every entry the
// shipping code calls is an inline no-op returning what it sees with SR_NATIVE_RECORD / SR_NATIVE_DRAW unset: the
// record is off, no bracket becomes an 'NDRW' native draw, so the CP never meets one.
inline bool on() { return false; }
#endif

enum DrawKind : uint8_t {
  kDrawIndexed = 0,    // 820FC000 DrawIndexedVertices
  kDrawAuto = 1,       // 820FBBF8 DrawVertices
  kDrawUP = 2,         // 820FBBB0 DrawVerticesUP
  kBeginVertices = 3,  // 820FB6E8 BeginVertices (its DRAW packet is stamped by EndVertices)
  kEndVertices = 4,    // 820FBBA0 EndVertices
  kDrawKinds
};

#if SR_ORACLE
// Render thread, inside the entry's SR_NATIVE_SCOPE (bracket open), before
// __imp__. `args` = r4..r7 of the call (logged with mismatch examples).
void onDrawBefore(uint32_t devVA, DrawKind kind, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7);
// The same thread, after __imp__ returned.
void onDrawAfter(uint32_t devVA);

// Render thread, after D3DDeviceEmitPm4Packet_sub_82113010(dev, a2, a3, count) returned
// `data`: a SET_CONSTANT (ALU) packet the CALLER fills with 4*count dwords at `data` - the
// B2 emitter class (Lion's per-tile c96-c111). The constants never touch the device shadow,
// so the native ALU timeline applies them (from the ring, filled by then) at the next draw.
void onConstantPacket(uint32_t devVA, uint32_t a2, uint32_t a3, uint32_t count, uint32_t data);
#else
inline void onDrawBefore(uint32_t, DrawKind, uint32_t, uint32_t, uint32_t, uint32_t) {}
inline void onDrawAfter(uint32_t) {}
inline void onConstantPacket(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
#endif

// CP thread (gfx/pm4_coverage), before a DRAW_INDX (0x22) / DRAW_INDX_2 (0x36)
// executes. callId/entry = the owning bracket's stamp (0 / -1 = none).
// The engine's active shaders at the draw: ucode hashes (XXH3 of the IM_LOADed
// bytes), constant-use masks (gfx/xenos_ucode constantBlocksRead; bit 16 =
// relative) and the VS load source.
struct CpShaders {
  uint64_t vs = 0, ps = 0;
  uint32_t vsUse = 0, psUse = 0;
  uint32_t vsAddr = 0, vsDwords = 0;
};
#if SR_ORACLE
void checkDraw(uint32_t callId, int entry, uint32_t op, const CpShaders& sh);
// CP thread: a draw of bracket callId was bin-predicated away (it still counts as one
// execution for the Z-pass model).
void noteDrawSkipped(uint32_t callId);

// CP thread: a constant write into the ALU file (type-0 0x4000-0x47FF, SET_CONSTANT /
// LOAD_ALU_CONSTANT type 0) by the bracket owner `entry` - who writes the blocks that
// mismatch (the XDK's own Clear/Resolve constants vs the game's).
void noteAluWrite(int entry, uint32_t reg, uint32_t count);

// NATIVE mode A' DRIVEN (SR_NATIVE_DRIVE=1, needs SR_NATIVE_RECORD=1, default OFF): checkDraw
// writes the projection (124 packed registers, 32 fetch slots - not for the UP / BeginVertices
// family, whose vertex slot the XDK fills at draw time - and the bool/loop constants) into the
// CP register file, so the backend builds the draw from NATIVE state; afterDraw() puts the CP's
// values back once the draw executed, so the PM4 stream stays the oracle for everything else.
// ALU constants stay the CP's (the projection keeps hashes only).
void afterDraw();

// NATIVE step 3 (SR_NATIVE_SKIP=1, needs SR_NATIVE_DRIVE=1): PM4 state packets inside the bracket
// of a DRAW entry whose record is complete (not the UP / Begin-End family) and whose carried ALU
// file matches the record are NOT executed - the draw is built from the projection, which then
// stays in the register file (the native state is the state). Every 16th bracket is parsed and
// compared as before (the PM4 oracle). CP thread. true = skip this register write run.
bool skipState(uint32_t callId, bool drawEntry, uint32_t reg, uint32_t count);

// [NATIVE_REC] summary (CP thread, from the coverage report; and at exit).
void report(const char* why);

// NATIVE mode B stage 1a (SR_NATIVE_DRAW=1, default OFF; implies SR_NATIVE_RECORD): plain
// DrawIndexedVertices / DrawVertices brackets become one type-3 NOP {'NDRW', slot} of the same
// length; the CP calls ndExecute at it (gfx/pm4_engine). SR_NATIVE_DRAW_VERIFY_EVERY=N runs every
// Nth bracket as its original packets (checkDraw then compares). See native_device.cpp.
bool ndOn();
void ndExecute(uint8_t* host, uint32_t seq, void (*exec)(void*, const uint8_t*, size_t), void* eng);
// The .nrt sidecar of a live trace (one record per native bracket recorded while tracing) and
// the native replay's substitution (gfx/gpu_trace_replay, SR_REPLAY_NRT=1).
size_t nrtRecordBytes();
// The native replay, per packet of the trace while `rec` is the next record: 0 = not in its
// bracket (execute as usual), 1 = a state packet the native replay does not execute, 2 = a packet
// it runs as PM4 (the record's earlier ranges were just written), 3 = the DRAW (every remaining
// range and literal written; execute it, the record is done).
int nrtPacket(const void* rec, uint32_t phys);
#else
// The CP-side seams (checkDraw, noteDrawSkipped, noteAluWrite, afterDraw, skipState, report: gfx/pm4_coverage) and
// the .nrt replay (gfx/gpu_trace_replay) left with their oracles; only the CP's NOP dispatch remains.
inline bool ndOn() { return false; }
inline void ndExecute(uint8_t*, uint32_t, void (*)(void*, const uint8_t*, size_t), void*) {}
#endif


}  // namespace sr::native::dev
