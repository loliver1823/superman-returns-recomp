#pragma once

// iter-467 NATIVE strand A, stage S1 (docs/NATIVE_ROADMAP.md "S0 RESULTS" (d)): the XDK draw flush
// - D3DDeviceEmitDrawIndexedVertices_sub_820FC000 / D3DDeviceEmitDrawVertices_sub_820FBBF8 - as
// host code that writes a NATIVE DRAW RECORD instead of PM4.
//
// The flush walks the device's five dirty qwords (dev+0x00..0x20) exactly as the XDK does
// (recompiled PPC, IDA): VS / PS constant runs (GpuEmitType0RegPacketsVec 821078C0), the shader
// bind (82108668, a GUEST call), the window scissor (821072F8: the simple branch native, the
// tiled branch a guest call), register runs (GpuCmdFlushDirtyRegisterRuns 82107520) at 0x2200 /
// 0x2180 / 0x4900 / 0x2100 / 0x2000 / 0x2280 / 0x2300 / 0x2380, the fetch constants
// (GpuEmitType0RegPackets 82107760 + its 0x5000 trailer), the shader-constant runs (821075E0, a
// guest call), then [0x2102 base] + DRAW_INDX. Every byte the XDK would have written is
// accounted for: a contiguous native span becomes ONE type-3 NOP of the same length,
// {NOP, 'NFLU', posLo, posHi}, whose record (register runs + the draw) lives in a host arena;
// the cursor, segment kicks and fences advance exactly as the XDK's would. A span shorter than
// the 4-dword marker is written as the XDK's own bytes. Guest calls (shader bind, tiled scissor,
// 821075E0, a run that would cross the segment limit, the kick 820FD8C0) run in stream order
// between spans and write their own PM4. The CP applies a record with Pm4Engine::nativeRun /
// nativeDraw (no PM4 decode); a replayed segment re-applies the same record (the arena keeps
// half its size of history).
//
// Gates:
//   SR_NATIVE_FLUSH          record mode, DEFAULT ON (iter-467; =0 = the XDK body and PM4, the A/B).
//   SR_NATIVE_FLUSH_CHECK=1  check mode (record mode off, the stream stays PM4): per draw, the native flush runs dry
//                            on a copy of the device, then the guest body runs for real; the
//                            native PM4 rendering is compared byte for byte with what the guest
//                            wrote, plus the cursor and every device dword (dev..+0x3600) when
//                            the dry run needed no guest call. rdtsc of both = the per-draw budget.
//   SR_NATIVE_FLUSH_CHECK_EVERY=N  check every Nth draw per thread (default 1).
//   SR_NATIVE_FLUSH_TIMING=1 record mode: rdtsc around the native flush (guest side) and the
//                            record apply (CP side) -> [NATIVE_FLUSH] us/draw.
//   SR_NATIVE_BIND           P1 rows 1+2: the shader bind (82108668 family) and its LOAD_ALU_CONSTANTs as
//                            records, DEFAULT ON (iter-467; =0 = the guest call, the A/B).
//   SR_NATIVE_SEG_STRESS=N   correctness rolls: the XDK's own kick once N bytes went into the segment.
//   SR_NATIVE_BEGINV         P1 rows 4+6+7: BeginVertices / DrawVerticesUP's flush 820FB6E8 (the third draw
//                            flush: the same sections, then the UP fetch slot and the predicated auto-index
//                            draw) as records; default ON since batch 3b (2026-10-09), =0 keeps the A/B.
//   SR_NATIVE_FLUSH_SPLIT    p1.flush_residue (docs/NATIVE_ROADMAP.md "p1.flush_residue BUILD NOTE"), DEFAULT ON
//                            since 2026-10-14 (=0 keeps today's path for the A/B):
//                            a constant / register / fetch section that does not fit before the segment limit is
//                            chunked across the SDK's own kick as records (GpuCmdEmitRegisterRunChunked 82107058
//                            ported) instead of going to the guest leaf; a draw over 0xFFFF indices is split as
//                            820FC000 / 820FBBF8 split it; row 9's record pre-pass looks through the kicks and the GPU
//                            waits it cannot model (kernel/native_rec_b.cpp). SR_AB_TOGGLE=SR_NATIVE_FLUSH_SPLIT for
//                            the per-present A/B.
//                            Log: [NATIVE_FLUSH] <why> split (periodic / exit) and [NATIVE_FLUSH] split snapshot (the
//                            residue counters, cumulative from boot, on the first draw >= 0.5 s after the last line -
//                            not terminal: the proof grades boot through the last snapshot; freshness relative to the
//                            last logged timestamp allows 1.0 s; the remaining execution tail is unmeasured;
//                            tools/perf/flush_split_snapshot.py).
// Declined (guest body, counted): tile auto-predication (dev byte 10808 & 2, never set here - not the
// Z-pass, which is bit 0x40), more than 0xFFFF indices (under SR_NATIVE_FLUSH_SPLIT only a prim the XDK's
// indices-per-prim table gives 0, where the XDK traps, and BeginVertices), a live
// trace recording, the constant dedup on, no arena space.
// Log: [NATIVE_FLUSH] (periodic + exit), [NATIVE_FLUSH_BAD] (check examples).

#include <cstdint>
#include "native_record_format.hpp"
#include "sr_oracle.hpp"

struct PPCContext;
namespace sr::gfx {
// C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): the engine the CP thread applies records on. The development
// configuration's live engine is the PM4 walker derived from the record engine (gfx/pm4_engine.hpp: ring packets, IBs,
// the oracles); the release configuration (SR_ORACLE=0) links no walker, so its CP-side API below is the record
// engine's (gfx/record_engine.hpp) and a literal-PM4 gap in a segment is fatal there.
#if SR_ORACLE
class Pm4Engine;
using CpEngine = Pm4Engine;
#else
struct RecordEngine;
using CpEngine = RecordEngine;
#endif
}

namespace sr::native::flush {

constexpr uint32_t kMarkerMagic = 0x4E464C55u;  // 'NFLU'

// Record words (host order). RUN = (count << 16) | reg (bit 30 = a dedup CHECK run), then `count`
// big-endian register values. PACKET = 0x80000000 | (kind << 24) | (n << 8) | flags; flags bit 0 =
// indexed (draws), bit 1 = predicated (the CP tests binSelect & binMask at EVERY execution of the
// record, Z-pass / tile replays included, exactly as execute() does for a predicated type-3 packet).
// P1 rows 1+2 (the shader bind family 82108668 and its LOAD_ALU_CONSTANTs): the kinds below the
// draw. The CP applies each through the Pm4Engine::apply* method the PM4 packet also uses.
// ---- P2-B segmentless transport (SR_NATIVE_SEGMENTLESS, default OFF; implies SR_NATIVE_RINGLESS) ----
// The close (GpuCommandBufferFlush_820FD8C0, overridden whole here) registers the closed segment as an
// ordered list of PARTS - the arena records of its spans and the literal PM4 bytes of the gaps between
// them (what a guest emitter still writes) - under the segment's phys; GpuRingUpdateWritePointer's
// override (kernel/gpu_ringless.cpp) then publishes a SEGMENT item instead of an INDIRECT_BUFFER, and
// the CP applies the parts in order (cpSegment). The fence tail and the coherency block become
// kRkFence / kRkInvalidate records; the ring, the markers and the padding are not written.
bool segmentlessOn();
// tools/graceful-quit (kernel/quit.hpp): the [NATIVE_FLUSH] report with any why token (the terminal snapshot), and
// the sum of the producer / consumer counters (the quit's settle digest: it moves whenever flush work happens).
void reportLine(const char* why);
uint64_t activityTotal();
// The gate AND the in-roll A/B window at this moment (the producer side's test; see native_flush.cpp).
bool segmentlessActiveNow();
// p1.flush_residue: SR_NATIVE_FLUSH_SPLIT (latched) / the gate AND its per-present A/B window at this moment (callers
// decide once per draw / entry: the S1 flush in draw(), row 9 in its runRecord).
bool flushSplitOn();
bool flushSplitActive();
// SR_AB_PRESENT_DRAINED (diagnostic): after a close, commit the pending per-present A/B flip once the stream is
// quiesced (native_flush.cpp; called by the 820FD8C0 wrapper only when sr::abt::drainedMode()).
void abDrainAfterClose(uint32_t dev);
// boot.device_create (native-a, 2026-10-10): SR_NATIVE_DEVCREATE (default ON since native/devcreate-fix, 2026-10-15;
// =0 keeps the guest bodies and the per-present A/B arm) - the device-create producers of
// the boot PM4 block as host code: D3DDeviceInitRingBuffer_820FDE90 and GpuCreateBlockInterruptThreads_82110EB8
// (kernel/native_devcreate.cpp), D3DDeviceResetState_8210E520 and the segment re-emit 820F0048 (here, with
// their dry CHECK kinds 8 / 9). Takes effect only under the native transport (segmentlessActiveNow()).
bool devCreateOn();
bool devResetLeaf(PPCContext& ctx, uint8_t* base);
bool segmentBeginLeaf(PPCContext& ctx, uint8_t* base);
uint64_t devResetCalls();   // 8210E520 handled natively
uint64_t segBeginCalls();   // 820F0048 handled natively
uint64_t devCreateDecl();   // both: declined to the guest body
// boot gaps (native/boot-gaps, 2026-10-16; a follow-up on boot.device_create, docs/NATIVE_ROADMAP.md "BOOT GAPS BUILD
// NOTE"): SR_NATIVE_MODE_STATE (default ON; =0 keeps the guest body and the per-present A/B arm) - the device create's
// last literal PM4, GpuRingEmitModeStateReg_82103238(dev, mode) from D3DDeviceInitDefaultRenderStates_821058B0 (its one
// code caller; once per title-device creation): dev+0x2A24 bits 12..13 = mode, the kick check, WAIT_UNTIL 0x5C8 =
// 0x20000 and RB_BC_CONTROL 0xF01 = dev+0x2A24 as two run records in one Entry (dry CHECK kind 10). Frame 1's UNSTAMPED
// r05 / r0F pair (dcf2 census1: the boot block's gap 1, 16 bytes) - unstamped because D3DDeviceCreate 820F34C0's bracket
// opens before the device exists. Takes effect only under the native transport (segmentlessActiveNow()).
bool modeStateOn();
bool modeStateLeaf(PPCContext& ctx, uint8_t* base);
uint64_t modeStateCalls();  // 82103238 handled natively
uint64_t modeStateDecl();   // declined to the guest body (the gate on)
// The display controller's emitters (boot gaps; kernel/native_display_emit.cpp): SR_NATIVE_DISPLAY (default ON; =0
// keeps the guest bodies, SWAP's persisted-display decline and the per-present A/B arm). The gate AND record mode AND
// the native transport (its A/B window) AND outside the scoped SDK oracles at this moment.
bool displayGate();    // SR_NATIVE_DISPLAY (latched)
bool displayActive();
// The leaves (true = taken natively; false = declined at the top, the guest body runs): GpuRingEmitGammaRamp_821056E0
// (wide false) / GpuRingEmitGammaRampWide_821057C0 (wide true), GpuSetGammaRamp_82111CB8, GpuScalerEmitCommandBuffer
// 8210A788. Their overrides call them under displayGate().
bool gammaRampLeaf(PPCContext& ctx, uint8_t* base, bool wide);
bool setGammaRampLeaf(PPCContext& ctx, uint8_t* base);
bool scalerLeaf(PPCContext& ctx, uint8_t* base);
// Inside PresentGateOracle / ShaderConstRunsOracle (the SDK oracles every transport leaf declines in).
bool sdkOracleScope();
// The SWAP admission's fault-free guest read (a snapshot of mapped readable memory) and its writability test (plainly
// writable, or tracker-owned as the guest finds it) for the display leaves' own admissions.
bool guestRead(uint32_t va, void* destination, uint32_t bytes);
bool guestWritable(uint32_t va, uint32_t bytes);
// The display emitters' counters ([NATIVE_FLUSH] <why> display ...): calls taken natively per emitter, declines (the
// gate on), the scaler's literal fallback (the platform's VdInitializeScalerCommandBuffer returned a program: kept as
// the SDK's bytes, a literal gap), the reserve failures (820FC910 returned 0: nothing emitted), SWAP's persisted-display
// conversions, and the CP's applications of the two record kinds.
struct DisplayCounters {
  uint64_t ramp = 0, rampWide = 0, setGamma = 0, scaler = 0, persist = 0;
  uint64_t rampDecl = 0, setGammaDecl = 0, scalerDecl = 0;
  uint64_t scalerProgram = 0, reserveFail = 0;
  uint64_t cpRamp = 0, cpScaler = 0;
};
DisplayCounters displayCounters();
void displayNotePersist();  // swapLeaf: one persisted-display Swap taken natively
void displayReport(const char* why);
uint64_t displayLutHash();  // the presenter's display ramp (gfx/gamma_ramp.hpp displayRamp), FNV-1a 64: the report's lut=
// The SDK bytes a kRkGammaRamp / kRkScaler payload stands for (9236 / 5652 / 80), 0 = not a valid payload of that kind.
uint32_t displayRecordBytes(uint32_t kind, const uint32_t* w, uint32_t n);
// Observe-only: the literal-PM4 gap parts the CP has applied (cpSegment; the [NATIVE_FLUSH] seg line's gapApplied) - the
// boot-gaps difftest's "no literal PM4" check.
uint64_t segGapPartsApplied();

// p2.zpd's bake (native/zpd-bake, 2026-10-16): SR_NATIVE_ZPD (BAKED default ON, the latency model; =0 keeps the guest
// body and the guest path - the A/B arm) - GpuEmitTileSurfaceConfig_82101E08, the occlusion query's packets, as an
// Entry with EVENT_WRITE_ZPD as the kRkZpd record (dry CHECK kind 11). zpdPortOn = the gate (`!e || e[0] != '0'`) and
// gfx/native_zpd's mode not off; the leaf also needs the record transport (SR_NATIVE_FLUSH) and the Entry's open;
// SR_AB_TOGGLE=SR_NATIVE_ZPD toggles the port per present (the answering stays native). Counters: also in the
// [NATIVE_ZPD] census line's port= / portDecl= (zpd::notePort).
bool zpdPortOn();
uint64_t zpdPortCalls();  // 82101E08 handled natively
uint64_t zpdPortDecl();   // declined to the guest body by the Entry (the A/B's guest arm not counted here)
#if defined(SR_DIFFTEST)
void setZpdPortForTest(int v);  // -1 = the env gate (default), 0 = off, 1 = on (the mode's own override stays apart)
#endif
// P2-B (2026-10-09, after the p2c wedge; docs/NATIVE_P2B_SEGMENTLESS.md): a segment's identity is an
// ID, never its address. The close hands the parts it is publishing to GpuRingUpdateWritePointer's
// override on the calling thread - segmentPublishId() returns that id for exactly that call (phys and
// dwords re-checked) - so a block GUEST code wrote at a reused phys (the A/B's guest-path closes, a
// flush block, the Z-pass kick block) can never pick up another block's parts: p2c_abp wedged at
// present 55 when the guest's 11-dword flush block at 1E97A580 was answered with the native 22-dword
// coherency block registered there at present 4. Only the pump's replay of a Z-pass bracket
// (GpuReplayMarkerInterpreter 82110460 -> 820FD218 per list record, and chained segments while a
// chain is in flight) resolves by (phys, dwords), among the entries of the current and the previous
// bracket; those expire with the bracket. 0 = not a native segment: a plain INDIRECT_BUFFER.
uint64_t segmentPublishId(uint32_t phys, uint32_t dwords);
// native/quit-drain round 3 (Astra round 2 P1): what the graceful quit's ring cut drops through 820FD218 (kernel/
// gpu_ringless.cpp reads it under the cut only). 0 a plain IB: its guest bytes are its packets, so the cut may read the
// fence its tail carries; 1 the segmentless close's own hand-off on this thread: publishSegment records the S it
// carries; 2 a deferred entry the pump replays, 3 a hand-off pending for another block: no authoritative fence, so
// nothing is recorded (fail closed). A native publication's guest bytes are never read for a fence: record mode leaves
// the bytes under a native footprint untouched. Non-consuming (segmentPublishId's lookups, the hand-off kept).
int cutPublicationKind(uint32_t phys, uint32_t dwords);
#if defined(SR_DIFFTEST)
// tools/difftest/quit_test.cpp (round 3 P1): the segmentless close's publish of one native segment carrying `fence`
// (and, native/quit-ringspace, its fence tail's `progress` word), through the production publishSegment - its 820FD800
// call reaches 820FD218's override, the ring cut's drop branch.
uint32_t publishSegmentForTest(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t cursorEnd, uint32_t phys,
                               uint32_t dwords, uint32_t fence, uint32_t progress = 0u);
#endif
// SR_NATIVE_SEG_TILE (observe-only; a no-op otherwise): the drained reconciliation, called on the device thread after
// the guest's BlockUntilIdle 820FDCB8 returns - outside a Z-pass bracket every registered segment and record part must
// have had its first application by then (kernel/seg_tile.hpp; counted on the [NATIVE_FLUSH] seg tile line).
void segTileDrained();
// SR_NATIVE_SEG_TILE's forced drained point after a segmentless close (every SR_NATIVE_SEG_TILE_DRAIN_MS; native_flush.cpp).
void segTileForcedDrain(uint32_t dev);
// CP thread: apply the parts of segment `id` (phys / dwords re-checked; false = unknown or a
// mismatch, or a rejected record: a fatal native-consumer failure, never permission to parse the block).
// A segment the close published directly is applied once and forgotten; a bracket's (replayed
// twice by the pump) stays until its bracket expires.
bool cpSegment(sr::gfx::CpEngine& eng, uint64_t id, uint32_t phys, uint32_t dwords);
// CP thread, observe-only (the segh UMSK clobber, 2026-10-10): the segment part cpSegment is applying right now
// - "seg id=.. phys=.. dw=.. part i/n record pos=.." or "... part i/n gap va=.. len=.. at +off" - into `out`;
// false when no segment part is being applied. `gapVA` / `gapCopy` / `gapLen` = the gap's guest address, the
// bytes copied at the close and their length (0 / nullptr for a record part).
bool cpSegPartDescribe(char* out, size_t n, uint32_t* gapVA, const uint8_t** gapCopy, uint32_t* gapLen);

// ---- strand B's side of the contract ----
// CP thread: the execution of an opaque record kind. `w` = the n host-order dwords of the owner's
// snapshot. false = rejected (the record counts as bad). Registered once at init; nullptr = none.
using OpaqueHandler = bool (*)(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng);
#if SR_ORACLE
// Oracle-only reconstruction of the XDK packet bytes for CHECK and CPPM4.
using OpaqueRenderer = void (*)(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes);
void setOpaqueHandler(uint32_t kind, OpaqueHandler fn, OpaqueRenderer render = nullptr);
OpaqueRenderer opaqueRenderer(uint32_t kind);
#else
void setOpaqueHandler(uint32_t kind, OpaqueHandler fn);
#endif
OpaqueHandler opaqueHandler(uint32_t kind);

// Producer (the calling guest thread, inside the owner's override of the XDK entry): one XDK entry
// - a resolve, a clear - as record spans at the current cursor, exactly as the draw flush writes
// its own: a marker covering each span's XDK bytes, the records in the arena, the dedup stream
// position kept, the XDK's kick at the XDK's own sites. The original one-span usage still holds:
//   Entry e(ctx, base, dev);
//   if (!e.open()) { guest entry; return; }          // record mode off or declined: nothing written
//   fill the snapshot as the XDK's packets would set the registers
//   if (!e.packet(kRkResolve, &regs, sizeof(regs) / 4, xdkBytes)) { e.abandon(); guest entry; return; }
//   e.close();
// `xdkBytes` = the ring bytes the XDK entry would have written for this call (exact; >= 16). The
// payload is copied at packet(). Rule 1: packet() declines before anything is written, and
// abandon() leaves no trace; a span is written only by close().
// iter-467 (2026-10-09, the row 9 decision): an entry is a SEQUENCE of spans, cut where the XDK cuts
// them - at its own kick sites (kick()), around its allocator / helper calls (alloc(), guest()) -
// with any number of items (opaque records, plain runs) per span. These are the Driver's own
// primitives (the draw flushes use the same); P2-B replaces their implementation (kicks become
// no-ops, the cursor virtual, alloc() a host staging block) under the same interface.
//   Entry e(ctx, base, dev);
//   if (!e.open()) { guest entry; return; }
//   ... e.opaque(kRkResolve, ops, n, xdkBytes, pred) / e.run(...) at each XDK write site;
//   ... e.kick() exactly where the asm has `cmplw cursor, dev+0x30; bl 820FD8C0`;
//   ... va = e.alloc(dwords, 16) where the asm calls 820FCDF0 (0 = the XDK's failure);
//   e.close();   // or e.abandon() before anything was written
class Entry {
 public:
  Entry(PPCContext& ctx, uint8_t* base, uint32_t dev);
  ~Entry();
  bool open();
  // p2.transport step 1: open at a LEAF's cursor - `lastDw`, the dword before the block, as the leaf's caller passes
  // it (GpuEmitCpuCallbackPackets_820FDA80 writes into space its caller reserved and the caller then sets dev+40
  // itself): close() puts dev+40 back as it was. No constant dedup on such an Entry.
  bool openAt(uint32_t lastDw);
  // The CALLBACK item (kRkCallback): cb, its argument, the flags word (bits 24..29 the CPU mask, 0 = CPU 2; the
  // 3D-idle prefix when flags & 0xC0FFFFFF == 0) - 92 or 100 XDK bytes. lastDw = the block's last dword (the r3
  // 820FDA80 returns).
  bool callback(uint32_t cb, uint32_t arg, uint32_t flags, uint32_t& lastDw);
  // Complete 82111EC8 emission, including both perf timestamps and kick sites.
  // result starts as incoming r3; only a final timestamp kick changes it.
  bool presentGate(uint64_t& result);
  // GpuCmdEmitShaderConstantRuns_821075E0: four registers per dirty-mask bit,
  // including its preamble and native chunking at the SDK's segment boundary.
  bool shaderConstantRuns(uint64_t mask, uint64_t& result);
  // An opaque record item (kinds 7..9): the owner's payload (host order, n dwords, copied), standing
  // for `xdkBytes` of the XDK's PM4 (multiple of 4; any size - the span total decides whether the
  // span becomes a record or literal PM4). Several per span.
  bool opaque(uint32_t kind, const void* payload, uint32_t dwords, uint32_t xdkBytes, bool predicated = false);
  bool packet(uint32_t kind, const void* payload, uint32_t dwords, uint32_t xdkBytes, bool predicated = false) {
    return opaque(kind, payload, dwords, xdkBytes, predicated);
  }
  // A plain type-0 run record: `count` big-endian values at `beValues` (copied), `pad` type-2 NOPs
  // before the header as the XDK's emitter aligned it.
  bool run(uint32_t reg, uint32_t count, const uint8_t* beValues, uint32_t pad = 0);
  // A type-3 packet item of the Driver's own record kinds, literal body (host order, up to 4 dwords):
  // kRkBinReg (hdr 0xC0006000 | which << 8, body {value}, reg = which 0..3) or kRkSetConst (the
  // SET_CONSTANT header with its count and predicate bit, body = the offset word then the values).
  bool pkt(uint32_t rk, uint32_t hdr, const uint32_t* body, uint32_t bodyN, uint32_t reg = 0);
  // P1 row 5: D3DDeviceEmitScissorTileRects_820F0208's items (the XDK kick checks inside included);
  // r3 as the asm leaves it. false = the XDK's own paths the record mode cannot take (never in record
  // mode today); the caller abandons.
  bool tileRects(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t& r3);
  // P1 row 3b: D3DDeviceSetPredication_8210D400(dev, mask): its device writes, the XDK kick check and
  // the SET_BIN_MASK_LO {dev+12432} packet; r3 = the cursor as the asm leaves it.
  bool setPredication(uint32_t mask, uint32_t& r3);
  // p2.zpd's bake: GpuEmitTileSurfaceConfig_82101E08(dev, query, which)'s items (Driver::zpdSurface) - the query's
  // register runs and its EVENT_WRITE_ZPD as a kRkZpd record; r3 = the cursor as the asm leaves it.
  bool zpdSurface(uint32_t query, uint32_t which, uint32_t& r3);
  // P2-B item 5: D3DDeviceEmitPm4Packet_82113010(dev, type, index, count): the reserve (4*count + 5 dwords,
  // the XDK's own flush when they do not fit), then the item - pad NOPs, the SET_CONSTANT header with the
  // predication word, the offset word and the 16*count-byte body the caller fills afterwards - as a
  // kRkSetConstMem record. bodyVA = the body (r3 as the asm returns it), 0 = the XDK's reserve failure
  // (nothing emitted). false = declined before anything was written (the caller abandons).
  bool setConstMem(uint32_t type, uint32_t index, uint32_t count, uint32_t& bodyVA);
  // boot.device_create: a REG_RMW {rmwInfo, andMask, orMask} / an EVENT_WRITE_SHD {initiator, addr|endian,
  // value} item (kRkRegRmw / kRkEventShd), and the packet tails of D3DDeviceResetState_8210E520 (the kick
  // check, SQ_FLOW_CNTL, the two SCLK_PWRMGT REG_RMWs, VC_FIFO_DEPTHS, PA_CL_ENHANCE, SCRATCH_REG4/3) and of
  // the segment re-emit 820F0048 with r4 == 0 (the kick check, the four bin packets, the kick check, WAIT_UNTIL,
  // RB_BC_CONTROL). The device fields they read are the device's: the caller wrote them first, as the asm does.
  bool regRmw(uint32_t rmwInfo, uint32_t andMask, uint32_t orMask);
  bool eventShd(uint32_t initiator, uint32_t addr, uint32_t value);
  // p2.transport RESIDUE: the 11-dword COHER block {COHER_STATUS_HOST = status, COHER_SIZE / BASE = size, base,
  // WAIT_REG_MEM 0xA31 bit 31 clear} as one item - kRkInvalidate {base, size, whole} for status 0x03000000 (whole 0) /
  // 0x01000000 (whole 1), the opaque kRkCoherHost {status, size, base} for any other status.
  bool coherency(uint32_t status, uint32_t size, uint32_t base);
  bool devResetPackets();
  bool segmentBeginPackets();
  // boot gaps: GpuRingEmitModeStateReg_82103238's packets - the kick check, WAIT_UNTIL 0x5C8 = 0x20000, RB_BC_CONTROL
  // 0xF01 = dev+0x2A24 (the caller stored the mode bits first, as the asm does).
  bool modeStatePackets();
  // D3DDeviceReserveCommandSpace_820FC910(dev, dwords) at the SDK's own call site: the cursor stays when the dwords fit
  // before the segment limit dev+44; otherwise the open span is written and the XDK's flush and new segment run. 1 =
  // reserved; -1 = the XDK's failure (820FCF90 returned 0 - the SDK would write through that null cursor; nothing may be
  // emitted); 0 = not open.
  int reserve(uint32_t dwords);
  // The XDK's kick check at its own site: cursor > dev+0x30 -> the open span is written, 820FD8C0
  // runs (the segment switch), a new span opens at the new cursor. STRESS lowers the threshold.
  bool kick();
  // p1.flush_residue: GpuCommandBufferFlush 820FD8C0 UNCONDITIONALLY (GpuCmdEmitRegisterRunChunked 82107058's chunk
  // path: the open span is written, the cursor stored in dev+0x28, the segment switched), a new span at the new cursor.
  bool flush();
  // GpuCmdBufAllocate_820FCDF0(dev, dwords, align) between spans: the open span is written and
  // dev+0x28 published first (the allocator reads it and may lower dev+0x30 to it), the cursor is
  // re-read after. Returns the allocation VA, 0 = the XDK's failure.
  uint32_t alloc(uint32_t dwords, uint32_t align);
  // Other SDK helpers between spans: same publish / re-sync, result in ctx.r3.
  // Nested shader-constant work stays on the SDK path while this Entry owns the arena lock.
  bool guest(void (*fn)(PPCContext&, uint8_t*), uint64_t r3, uint64_t r4 = 0, uint64_t r5 = 0, uint64_t r6 = 0,
             uint64_t r7 = 0);
  void abandon();
  void close();
  // Read-only: the Driver's cursor (dev+0x28's value including the open span's committed bytes - dev+0x28
  // itself is published only when a span is written) and the segment limit dev+0x2C, for the XDK's own
  // run-fits tests (82107520: `cursor + 4n < limit`) reproduced between kick sites.
  uint32_t cursor() const;
  uint32_t limit() const;
 private:
  friend class ReplayKickCallback;
  friend struct BracketAccess;  // p2.transport BRACKET: the kick's detached spans (native_flush.cpp)
  friend struct EntryPoolTest;  // step 6 I6: the free list's SR_ORACLE test seams (native_flush.cpp)
  struct ClosedSpan;
  ClosedSpan closeDetached();
  struct Impl;
  // From Impl::take(): the calling thread's bounded free list (step 6 I6, SR_ENTRY_POOL) or the heap. Entry holds no
  // other ownership state: Impl::give() decides where it goes (the old `pooled_` flag, declared after impl_, was reset
  // by its own default initialiser after take() set it - the pool never worked; render/gate3/FREEZE_I6_I7_20261022).
  Impl* impl_ = nullptr;
};

// step 6 I6 (SR_ENTRY_POOL, default ON; =0 = a value-initialised Impl allocated and freed per Entry, the A/B): the
// Entry::Impl free list (bounded, kCap per thread) and its counters - also the [ENTRY_POOL] milestone lines.
bool entryPoolOn();
struct EntryPoolCounters {
  uint64_t takes = 0, hits = 0, misses = 0, pushes = 0, overflowDeletes = 0, crossThreadGives = 0, payloadReleases = 0,
           exitFrees = 0, threads = 0, resetBad = 0;
};
EntryPoolCounters entryPoolCounters();
#if SR_ORACLE
// tools/difftest/entry_pool_test.cpp's seams (SR_ORACLE builds only).
void entryPoolForceForTest(int v);         // -1 = the env gate, 0 = off, 1 = on
void entryPoolSkipResetForTest(int field);  // -2 = the env seam, -1 none, 0 dr, 1 opened, 2 locked, 3 atMode, 4 atCursor0,
                                            // 5 payload, 6 span (the mutant: reset() skips that statement)
int entryPoolCachedForTest();               // this thread's list length (-1 = destroyed)
// one Impl: use -> give (poisoned) -> take with `skip` skipped; returns whether the Impl was reused, `complete` = the
// SR_ORACLE state check (the taken Impl against a value-initialised one)
bool entryPoolResetCheckForTest(int skip, bool& complete);
// taken on this thread, given on another: true = this thread's list unchanged; the other's length and the counter delta
bool entryPoolCrossThreadForTest(int& otherCached, uint64_t& crossDelta);
uint64_t entryPoolThreadExitForTest(int nested);  // a thread nesting `nested` Entries exits: Impls its list freed
// step 6 N4's negative controls (one per process: an ASan report ends it): an Impl taken, used and given - parked on a
// free list - then touched; given on this thread / on another (alive, its list holding the Impl). Under ASan
// (SR_ASAN_TESTS) the touch must stop the process with a use-after-poison report; without ASan the call returns the
// touched byte (-1 = the Impl was not parked: nothing touched).
int entryPoolTouchAfterGiveForTest();
int entryPoolTouchAfterCrossGiveForTest();
// step 6 I7 (SR_SEG_ARENA): the segment-record arena's counters and tools/difftest/seg_arena_test.cpp's seams.
struct SegArenaCounters {
  uint64_t records = 0, solo = 0, arenas = 0, chunks = 0, chunkReuse = 0, chunkFreed = 0, bytes = 0, retained = 0,
           oversized = 0, nodeReuse = 0, capFaults = 0, capLogged = 0, bad = 0, freeChunks = 0, freeBytes = 0;
  uint64_t checks = 0, verifies = 0;  // the record checksums computed / verified: 0 unless SR_SEG_ARENA_HASH=1
  uint64_t fills = 0;  // released chunks filled with 0xDB (SR_ORACLE): 0 unless SR_SEG_ARENA_HASH=1
  // step 6 I12 (SR_AUDIT_RING): the reclaim audit - the ring's entries and the map's (the =0 arm), the reclaims that
  // found an entry / found none (either arm), and the ring's counted fallbacks (an out-of-order insert, a duplicate pos,
  // an out-of-order reclaim's tombstone, a compaction)
  uint64_t ringN = 0, mapN = 0, auditHits = 0, auditMisses = 0, ringSlow = 0, ringDupes = 0, ringTombs = 0,
           ringCompacts = 0, ringSlots = 0;  // ringN: the live entries (references held); ringSlots: incl. tombstones
};
SegArenaCounters segArenaCounters();
void segArenaForceForTest(int on, int keep);  // -1 = the env gate, 0 = off, 1 = on (SR_SEG_ARENA / _KEEP)
void segArenaCapForTest(uint64_t bytes);      // a smaller retention cap for the cap checks (0 = the 32 MiB)
bool segArenaHashOnForTest();                  // SR_SEG_ARENA_HASH=1: the checksums and condition 5's folds run
void auditRingForceForTest(int v);             // I12: -1 = SR_AUDIT_RING, 0 = the map, 1 = the ring
// capture `bytes` bytes of `fill` at `pos` as closeSpan does (under g_segMu); the test holds the record by the token
uint64_t segArenaCaptureForTest(uint32_t bytes, uint64_t pos, uint8_t fill);
void segArenaCloseForTest();                     // takeFeetLocked's swap: the open arena is replaced (retired)
bool segArenaReleaseForTest(uint64_t token);     // the test's reference goes
bool segArenaReclaimForTest(uint64_t pos);       // the audit's reclaim of `pos`
// step 6 I15 P2's guard (SR_GUESTMEM_LEAN): the guarded copy - false when the source faults (an access violation caught)
bool guardedCopyForTest(void* destination, const void* source, uint32_t bytes);
// ... and the known-mapped read itself (gateReadMapped): its result, with the fault / plain-read counters' deltas
bool gateReadMappedForTest(uint32_t va, void* destination, uint32_t bytes, uint64_t* faults, uint64_t* mapped);
bool guestMemLeanOnForTest();  // SR_GUESTMEM_LEAN's value (on unless =0)
bool segArenaRecordIsForTest(uint64_t token, uint32_t bytes, uint8_t fill);  // the held record's bytes are intact
int segArenaEarlyReleaseForTest();               // I7 (4)'s negative control (an ASan report must stop it)
#endif

bool on();
bool checkOn();
bool shaderConstantRunsLeaf(PPCContext& ctx, uint8_t* base);
// SR_NATIVE_SHADER_CONST_RUNS (default ON since the construns bake; =0 keeps the SDK body).
bool shaderConstRunsGate();
// The construns bake's observe-only counters (the [NATIVE_FLUSH] shaderConstRuns line; read-only here for
// difftest --shader-const-runs-test bake): Driver-section and SDK-leaf native calls, the leaf's declines by reason,
// entry segment switches, the dev+44 - dev+48 margin check, tessellated draws, the CP's render-back count.
struct ShaderConstRunsCounters {
  uint64_t calls = 0, chunked = 0, drv = 0, sdk = 0, abGuest = 0;
  uint64_t declOff = 0, declOracle = 0, declMask = 0, declSpan = 0, declOpen = 0, declEmit = 0;
  uint64_t kick = 0, marginBad = 0, tessDraws = 0, tessSdk = 0, cpPm4 = 0;
};
ShaderConstRunsCounters shaderConstRunsCounters();
// D3DDeviceEmitDrawTessellated 820FC478's observe override: one tessellated draw on this thread's stack.
void tessEnter();
void tessLeave();
// Keep nested shader-constant calls on the SDK path during independent comparisons.
class ShaderConstRunsOracle {
 public:
  ShaderConstRunsOracle();
  ~ShaderConstRunsOracle();
  ShaderConstRunsOracle(const ShaderConstRunsOracle&) = delete;
  ShaderConstRunsOracle& operator=(const ShaderConstRunsOracle&) = delete;
};
bool presentGateOn();  // SR_NATIVE_GATE, default ON since the transport bake (=0 off); requires segmentless
bool presentGateLeaf(PPCContext& ctx, uint8_t* base);
// Dry SDK comparison only: capture before the scoped guest oracle, compare its
// emitted bytes/cursor/return afterwards. This does not prove execution pacing.
struct PresentGateCheck {
  uint32_t dev = 0, cursor = 0, payload[3] = {};
  uint64_t returnValue = 0;
  bool ready = false, skipped = false;
};
#if SR_ORACLE
bool presentGateCheckBefore(const PPCContext& ctx, PresentGateCheck& state);
bool presentGateCheckAfter(const PresentGateCheck& state, const PPCContext& ctx);
#else
inline bool presentGateCheckBefore(const PPCContext&, PresentGateCheck&) { return false; }
inline bool presentGateCheckAfter(const PresentGateCheck&, const PPCContext&) { return false; }
#endif
// Scoped guest oracle: prevent the nested 820FDA80 call emitting records.
class PresentGateOracle {
 public:
  PresentGateOracle();
  ~PresentGateOracle();
  PresentGateOracle(const PresentGateOracle&) = delete;
  PresentGateOracle& operator=(const PresentGateOracle&) = delete;
};
bool presentGatePayload(uint32_t pcVA, uint32_t interval, uint32_t pacingBits, uint32_t* out);
uint32_t presentGateDwords(const uint32_t* payload, uint32_t* out);  // exactly 39
bool presentGateApply(uint32_t kind, const uint32_t* payload, uint32_t n, sr::gfx::CpEngine& eng);
// First SWAP stage: the VdSwap export writes one native item in its caller's
// reserved 256-byte block. False declines before any mutation. Default ON since
// the transport bake (=0 off); requires segmentless. The caller remains
// responsible for advancing dev+40.
bool swapBlockOn();  // SR_NATIVE_SWAP_BLOCK (default ON; =0 off)
// SR_NATIVE_SWAP (default ON since the transport bake; =0 off; requires the
// block): normal SDK swap body, including the original N-1 throttle.
// Capture/timestamp/PIX/persist paths decline before mutation. Development
// helpers and oracle paths remain; this does not close native release removal.
bool swapOn();
bool swapLeaf(PPCContext& ctx, uint8_t* base);
uint64_t swapSdkCalls();
// The tcmp1 diagnosis (transport-fix lane, 2026-10-14): the SDK swap leaf's declines by reason, in its admission order
// (each counted once, the first that holds), then the two post-swap literal fallbacks (those calls still count as
// taken). The periodic [NATIVE_FLUSH] swapSdkWhy line prints them; every decline also writes a capped SWAP_DECL sample
// (the cursor's reserved block page by page: protection, tracker ownership, plain / tracker writability; the mode bytes).
// A refusal for a failed READ is its own reason (the *Read ones), apart from the refusal of the state read (Astra
// round 1, P2: a memory failure must never pass as a mode decline).
// boot gaps: kSwFallbackPersist = the persisted display's REG_RMW 0x1E4E written as the SDK's literal bytes when its
// Entry could not open after VdSwap (counted, sampled; the call still taken), after the two older fallbacks.
enum SwapDeclWhy : int {
  kSwDevRead, kSwDevWrite, kSwFront, kSwStack, kSwTiling, kSwFlags2A39, kSwMovieCapture, kSwPix, kSwTimestamps, kSw3B28,
  kSwHsioRead, kSwHsio, kSwMovieGlobalRead, kSwMovieGlobal, kSwRect, kSwSysbuf, kSwRootRead, kSwRoot, kSwExport,
  kSwCursorAlign, kSwCursorWrite, kSwEntry, kSwFallbackFront, kSwFallbackScaler, kSwFallbackPersist, kSwWhyN
};
uint64_t swapSdkDeclWhy(int why);
const char* swapSdkDeclName(int why);
uint64_t swapSdkDeclSampled(int why);  // the SWAP_DECL sample lines written for that reason (the capped samples)
// tools/graceful-quit's ring cut (kernel/quit.hpp): the native publishes it dropped, each an orderly stop - an ordinary
// close's segment, a replay kick callback, a BRACKET_END's coherency block, a BRACKET_END. Printed as [NATIVE_FLUSH]
// <why> quitCut once any is non-zero.
struct QuitCutCounters {
  uint64_t segs = 0, kicks = 0, coherency = 0, ends = 0;
};
QuitCutCounters quitCutCounters();
// BRACKET_END's memory declines (bracket line `memory=`) by site: the frame (dev / stack alignment, a low stack), the
// device and stack writability, the list descriptors' read, then per recording list (0 = dev+0x32C8, 1 = 0x32A0,
// 2 = 0x32B4) its prefix / cursor alignment and writability, and the arena Entry. Printed by the periodic
// [NATIVE_FLUSH] bracketMemory line; every decline also writes a capped BRACKET_MEMORY sample of the refused region.
enum BracketMemorySite : int {
  kBmFrame, kBmDev, kBmStack, kBmLists, kBmAlign0, kBmAlign1, kBmAlign2, kBmPrefix0, kBmPrefix1, kBmPrefix2,
  kBmCursor0, kBmCursor1, kBmCursor2, kBmEntry, kBmN
};
uint64_t bracketMemoryDecl(int site);
const char* bracketMemoryName(int site);
// The present-side transport kinds' counters (read-only; the composed present difftest checks which leaf took each
// call: exactly one conversion per producer call, never two).
struct TransportCounters {
  uint64_t gateCalls = 0, gateSkip = 0, gateDecl = 0, gateApplied = 0;
  uint64_t cbCalls = 0, cbDecl = 0, cbApplied = 0;
  uint64_t swapBlockCalls = 0, swapBlockDecl = 0, swapBlockApplied = 0;
  uint64_t swapSdkCalls = 0, swapSdkDecl = 0;
  uint64_t segBeginCalls = 0, devCreateDecl = 0;
  // the baked rows 5 / 3b leaves (820F0208, 8210D400; not transport kinds): calls taken, for the oracle-scope contract
  uint64_t tileRectCalls = 0, tilePredCalls = 0;
};
TransportCounters transportCounters();
bool swapBlockLeaf(uint32_t dev, uint32_t blockVA, uint32_t fetchVA, uint32_t width, uint32_t height);
bool swapBlockApply(uint32_t kind, const uint32_t* payload, uint32_t n, sr::gfx::CpEngine& eng);
void swapBlockRender(uint32_t kind, const uint32_t* payload, uint32_t n, uint8_t* out, uint32_t bytes);
// p2.transport step 1: the XDK dwords of a kRkCallback payload {cb, arg, mask, flags, pcAddr} (23, or 25 with the
// 3D-idle prefix) - the renderer's, and difftest --callback-test's oracle against the guest 820FDA80.
uint32_t callbackDwords(const uint32_t* w, uint32_t* v);

// 82110C78 owns this separately allocated callback tail. Its complete emitter
// preserves the SDK byte projection; only a successful kick append publishes
// the immutable native record. Allocation failure and omitted GPU work leave
// no registry entry. Default OFF: SR_NATIVE_REPLAY_CALLBACK=1, segmentless.
class ReplayKickCallback {
 public:
  ReplayKickCallback() = default;
  ~ReplayKickCallback();
  ReplayKickCallback(const ReplayKickCallback&) = delete;
  ReplayKickCallback& operator=(const ReplayKickCallback&) = delete;
  bool emit(PPCContext& ctx);
  void append(PPCContext& ctx, uint8_t* base);
 private:
  uint32_t dev_ = 0, start_ = 0;
  uint32_t payload_[5]{};
  bool emitted_ = false;
};
bool replayKickCallbackOn();
#if defined(SR_DIFFTEST)
// Manual, worker-free handoff harness. Runtime requires the real active CP.
using ReplayKickPublicationObserver = void (*)(uint64_t id, uint32_t phys, uint32_t dwords, bool chained);
void setReplayKickPublicationObserver(ReplayKickPublicationObserver observer);
// Runs after the owned record is detached and the producer lock is released.
// A fixture can hold this producer while another Entry closes/drains a segment.
using ReplayKickCloseObserver = void (*)();
void setReplayKickCloseObserver(ReplayKickCloseObserver observer);
#endif

// ---- p2.transport BRACKET begin / end + BS (NATIVE_ROADMAP transport plan item 4, its build note) ----
// SR_NATIVE_BRACKET (default ON since the transport bake, 2026-10-15; =0 off; requires the segmentless record transport
// and the async CP; SR_AB_TOGGLE=SR_NATIVE_BRACKET per present, decided once per bracket at its begin). Non-tiling
// Z-pass only.
//   BRACKET_BEGIN  GpuReplayListBeginRecording_82110B30: the decision at the top, then the SDK body whole (its CPU
//                  list bookkeeping, as today), then a host generation opens; the pinned closes bind to it in order.
//   BRACKET_END    GpuReplayListEndRecordingKick_82110C78: decided before any write (generation, gates, memory, the
//                  recorded list walked and bound by sequence); the SDK body ported with the coherency block as
//                  kRkInvalidate records, the callback block replaced by a kRkBracket record whose SEGMENT carries
//                  the replay steps (BS values and the bound segments: depth, neutral once, colour), and the kick
//                  appended with increment 0 - dev+0x2A74 stays drained, no interrupt, no pump.
// A declined end runs the SDK body; the closes stay registered deferred, so the guest pump replays them as today.
bool bracketGate();                                   // SR_NATIVE_BRACKET (latched)
bool bracketBeginDecide(const PPCContext& ctx);       // 82110B30's decision (counted); call before the SDK body
void bracketOpen(uint32_t dev);                       // after the SDK body when bracketBeginDecide said yes
void bracketGuestClose(uint32_t dev);                 // a guest-body 820FD8C0 inside an open generation poisons it
bool bracketEnd(PPCContext& ctx, uint8_t* base);      // 82110C78; false = declined (run the SDK body)
// ---- p2.transport RESIDUE (NATIVE_ROADMAP transport plan item 5, its build note) ----
// Each latched and default ON since the transport bake (2026-10-15; =0 off), each requires the segmentless record
// transport, each with its own per-present A/B
// (SR_AB_TOGGLE=<gate>, SR_AB_PRESENT=1). The ports of the generated bodies (guest/ppc); the one difference from the SDK
// is the producer's PM4 at the cursor, written as an Entry span (records) instead of guest stores.
//   SR_NATIVE_IDLE_WAIT=1    D3DDeviceBlockUntilIdle_820FDCB8: its WAIT_UNTIL {0x5C8 = 0x20000} as a register run; the
//                            kick check, GpuBlockOnFence(S, 4) and the dev+0x2A74 spin stay the SDK's code (called by
//                            the observe override in cs_timeout_underflow_fix.cpp).
//   SR_NATIVE_INVAL_RANGE=1  GpuInvalidateMemoryRange_820F4280's owner-thread block: the run {0x5C8} + kRkInvalidate.
//   SR_NATIVE_LOCK_COHER=1   the lock core 820F3988's owner-thread COHER block: kRkInvalidate or kRkCoherHost.
// A leaf returns false when it declined at the top (counted by reason): the caller then runs the guest body whole.
bool idleWaitOn();
bool invalRangeOn();
bool lockCoherOn();
bool idleWaitLeaf(PPCContext& ctx, uint8_t* base);
bool invalRangeLeaf(PPCContext& ctx, uint8_t* base);
bool lockCoherLeaf(PPCContext& ctx, uint8_t* base);

#if defined(SR_DIFFTEST)
// Worker-free consumer harness: called for every SEGMENT the native close / BRACKET publishes directly while no CP is
// active (the hand-off the ringless override would take), and for a chained one (chained = true).
using SegmentPublicationObserver = void (*)(uint64_t id, uint32_t phys, uint32_t dwords, bool chained);
void setSegmentPublicationObserver(SegmentPublicationObserver observer);
#endif

// kind 0 = DrawIndexedVertices (r3 dev, r4 prim, r5 baseVertex, r6 startIndex, r7 count),
// kind 1 = DrawVertices (r3 dev, r4 prim, r5 startVertex, r6 count), kind 2 = BeginVertices
// 820FB6E8 (r3 dev, r4 prim, r5 count, r6 stride bytes; returns the ring allocation in r3, 0 on
// failure). Record mode: true = the draw was flushed natively and the guest body must NOT run.
bool draw(PPCContext& ctx, uint8_t* base, int kind);

// Check mode, around the guest body.
#if SR_ORACLE
void checkBefore(PPCContext& ctx, uint8_t* base, int kind);
void checkAfter(PPCContext& ctx, uint8_t* base, int kind);
#else
inline void checkBefore(PPCContext&, uint8_t*, int) {}
inline void checkAfter(PPCContext&, uint8_t*, int) {}
#endif

#if SR_ORACLE
// CP thread: a {NOP, 'NFLU', posLo, posHi} marker (body = the NOP body, cnt = its dword count) - met only by the
// development PM4 walker (a plain IB / ring packet); the segmentless transport writes no marker. C1 stage 1: dev-only.
void cpExecute(sr::gfx::Pm4Engine& eng, const uint8_t* body, uint32_t cnt);
#endif
// CP thread: the arena position of the record being applied (0 = not inside a record); the
// draw ledger (gfx/draw_ledger) tags each draw with it.
uint64_t cpCurrentRecord();

}  // namespace sr::native::flush
