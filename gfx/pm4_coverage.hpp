#pragma once

// NATIVE ROADMAP N0.2 / N0.5 (docs/NATIVE_ROADMAP.md §1 item 1, §4.3): the TRUE
// per-packet PM4 counter - THE acceptance metric of the native plan ("0 PM4
// packets interpreted per frame"). Pm4Engine::packets counts execute() calls;
// this counts every packet the command processor interprets, by kind:
//   type-0 register writes, by 256-register page (packets and dwords, exact
//          across page boundaries);
//   type-3 opcodes (packets and body dwords), predicated-skip separately,
//          SET_CONSTANT / LOAD_ALU_CONSTANT by constant type;
//   type-1, type-2, zero dwords, segment-chain tail blocks, truncated packets;
//   ring-level INDIRECT_BUFFER descriptors the CP thread dispatches itself.
// Frames end at PM4_XE_SWAP (0x64), in the live CP and the trace replay alike,
// and live at every present the CP applies with no 0x64 packet before it (a
// native swap record: presented() below). One F line per frame, packets or not:
// a run whose CP presented frames and printed no F line counted nothing, which
// is never "0 packets" (native/composed-census, tcmp2's dark census1).
//
// With SR_NATIVE_BRACKETS=1 (kernel/native_brackets) each packet is also looked
// up by the host address it was read from (ring packets by ring index): the
// owning XDK entry (ACCOUNTED) or none (UNSTAMPED = an inlined/missed writer).
//
// Gates (default OFF, observe-only): SR_PM4_COVERAGE=1 counts and reports;
// SR_NATIVE_BRACKETS=1 implies it. Log tags: [PM4_COV] (per frame, CUM),
// [PM4_COVMAP] (per owner), [PM4_COV_UNSTAMPED] (examples); "[PM4_COV] armed"
// once, at the counter's first hook (the positive witness that it is on), and
// "[PM4_COV] ENDS" beside every CUM (how its frames ended). Counters are
// confined to the thread that runs the engine (the CP thread live), so a count
// is a plain increment. tools/pm4_coverage_report.py turns a log into
// archive/native_cov/<tag>.{json,tsv}.

#include <cstddef>
#include <cstdint>

#include "../kernel/sr_oracle.hpp"

namespace sr::native {
struct Interval;  // kernel/native_brackets.hpp
}

namespace sr::gfx::pm4cov {

#if SR_ORACLE
extern bool g_on;
#else
inline constexpr bool g_on = false;  // SR_ORACLE=0 (kernel/sr_oracle.hpp): the counter is compiled out
#endif
inline bool on() { return g_on; }

// kType1Skip: a type-1 header outside cpMode (the legacy/replay path steps over its
// header dword only, so its two data dwords are then parsed as headers).
enum OtherKind : uint8_t { kType1 = 0, kType2, kZeroDword, kChainTail, kTruncated, kType1Skip, kOtherKinds };
// The producer census's address witness for 821075E0's alignment pad (p1.flush_residue, native/flush-residue-2; Astra
// round 1): GpuCmdEmitShaderConstantRuns stores a type-2 NOP at cursor+4 and writes its run's header right after it when
// the cursor is 4 mod 8 (IDA 0x82107708 / 0x82107720). A type-2 NOP whose NEXT dword in the same window is such a header -
// type-0, not one-register, 4k registers inside [0x2388, 0x23A0) (the leaf's 6-bit group mask: 0x2388 + 4 * group) - is
// keyed 512 + kConstRunPadKey ("o6") in the producer lines; every other type-2 NOP keeps 512 + kType2 ("o1"). Counted
// as a type-2 NOP everywhere else (the t2 / F / CUM totals are unchanged). No S1 leaf writes a run starting in that range
// (their register runs end at 0x2387), so an o6 belongs to the construns run it precedes, one per run by construction.
constexpr int kConstRunPadKey = kOtherKinds;
constexpr int kOtherKeys = kOtherKinds + 1;  // the producer census's "oN" keys: the kinds, then the witnessed pad
inline bool constRunHeader(uint32_t hdr) {
  const uint32_t reg = hdr & 0x7FFFu, n = ((hdr >> 16) & 0x3FFFu) + 1u;
  return (hdr >> 30) == 0u && (hdr & 0x8000u) == 0u && reg >= 0x2388u && (n & 3u) == 0u && reg + n <= 0x23A0u;
}

#if SR_ORACLE
// Pm4Engine::execute hooks (only called when on()).
void windowBegin(const uint8_t* host, size_t bytes, int depth);
// true = NATIVE step 3: skip executing this state packet (SR_NATIVE_SKIP).
bool packet0(const uint8_t* at, int depth, uint32_t startReg, uint32_t count, bool oneReg);
bool packet3(const uint8_t* at, int depth, uint32_t op, uint32_t count, bool predSkipped);
// After a DRAW_INDX / DRAW_INDX_2 executed (NATIVE mode A' DRIVEN restores the CP registers).
void afterDraw();
// `next` = the following dword of the same window (0 = none): a type-2 NOP before constRunHeader(next) is keyed
// kConstRunPadKey (above).
void packetOther(const uint8_t* at, int depth, OtherKind k, uint32_t next = 0);
// The engine's active shaders for the next draw packet (NATIVE N1 check): ucode
// hashes, constant-use masks (bit b = block c16b..c16b+15 may be read, bit 16 =
// relative addressing) and the VS IM_LOAD source (address 0 = IM_LOAD_IMMEDIATE).
void setDrawShaders(uint64_t vs, uint64_t ps, uint32_t vsUse, uint32_t psUse, uint32_t vsAddr,
                    uint32_t vsDwords);

// The CP thread (kernel/gpu_command_processor.cpp): the next depth-0 execute()
// packet is ring dword `ringIndex`; and the ring-level IB descriptors it
// dispatches without execute().
// `dwords` = the packet's size; its ring stamps are cleared once consumed.
void setRingPacket(uint32_t ringIndex, uint32_t dwords);
void clearRingPacket();
void ringIndirect(uint32_t ringIndex, uint32_t dwords);

// Pm4Engine::applySwap (cpMode, before the CP's present callback): the CP applied a present - the XE_SWAP packet's
// 'SWAP' block, or one that never was a packet (SR_NATIVE_SWAP_BLOCK's kRkSwap record, swapBlockApply, through either
// record executor). The frame the XE_SWAP packet hook (packet3, op 0x64) has just ended is not ended again; any other
// present ends the frame here. Before this hook a present with no 0x64 packet ended no frame: under the composed
// transport (SWAP_BLOCK on) the CP applied every present as a record and the census printed no F line, no CUM, no
// [PM4_COVMAP] and no [NATIVE_BRACKETS] census - all of them reported from the frame end (tcmp2 census1: 5,640 presents,
// zero lines), indistinguishable in the log from a counter that was off.
void presented();
// Pm4Engine::cpPacket: the XE_SWAP packet packet3 just ended a frame at carries no 'SWAP' signature and applies no
// present - the next present (a record, with no packet between) ends its own frame.
void swapWithoutPresent();

// Periodic/final cumulative report (also emitted every 256 frames).
void report(const char* why);
// tools/graceful-quit (kernel/quit.hpp): the terminal snapshot's report, called once the CP has parked. A drain that
// ended inside a frame prints that partial frame first ("[PM4_COV] <why> tail frame=N+1 ..." + its F line), then
// "[PM4_COV] armed <why> ..." and report(why), all from the folded state: a second call (the quit's verify re-render)
// prints every one of those lines with the same integer fields. Returns -1 when the counter is off, 1 when a tail
// frame was printed, else 0.
int terminal(const char* why);

// The next depth-0 execute() window, `host`, is a host COPY of guest command bytes (a segmentless segment's gap part,
// kernel/native_flush cpSegment): `phys` = the stamp key of the guest VA the bytes were copied from
// (sr::native::stampKey), `iv[0..n)` = the owner stamps taken with the copy (sorted; kept alive by the caller
// until execute() returns). Without it a copy has no guest address and every packet in it is UNSTAMPED. Good for
// one window: the next depth-0 window drops it unless it is `host`.
void setCopyWindow(const uint8_t* host, uint32_t phys, const sr::native::Interval* iv, size_t n);
// Census accessors (difftest): packets by owner (kEntries index; kEntryCount = UNSTAMPED, +1 = RING_UNSTAMPED) and
// op key (type-3 op | 256 + type-0 page | 512 + other), by the outermost owner or the innermost producer.
uint64_t ownerPackets(int owner, int opKey, bool inner);
uint64_t unstampedPackets();
uint64_t chainPackets();  // of the unstamped: read from a chained close's list node (the CUM line's chain=)
uint64_t accountedPackets();
// Frame accounting (difftest; the [PM4_COV] ENDS line): frames ended (= F lines printed), the presents the CP applied
// while counting (presented() calls), and the frames a present ended (no XE_SWAP packet before it).
uint64_t framesEnded();
uint64_t presentsApplied();
uint64_t presentEnds();
#else
// SR_ORACLE=0: the hooks are no-ops (an unset SR_PM4_COVERAGE: on() is false and no packet is skipped).
inline void windowBegin(const uint8_t*, size_t, int) {}
inline bool packet0(const uint8_t*, int, uint32_t, uint32_t, bool) { return false; }
inline bool packet3(const uint8_t*, int, uint32_t, uint32_t, bool) { return false; }
inline void afterDraw() {}
inline void packetOther(const uint8_t*, int, OtherKind, uint32_t = 0) {}
inline void setDrawShaders(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
inline void setRingPacket(uint32_t, uint32_t) {}
inline void clearRingPacket() {}
inline void ringIndirect(uint32_t, uint32_t) {}
inline void presented() {}
inline void swapWithoutPresent() {}
inline void report(const char*) {}
inline int terminal(const char*) { return -1; }
inline void setCopyWindow(const uint8_t*, uint32_t, const sr::native::Interval*, size_t) {}
inline uint64_t ownerPackets(int, int, bool) { return 0; }
inline uint64_t unstampedPackets() { return 0; }
inline uint64_t chainPackets() { return 0; }
inline uint64_t accountedPackets() { return 0; }
inline uint64_t framesEnded() { return 0; }
inline uint64_t presentsApplied() { return 0; }
inline uint64_t presentEnds() { return 0; }
#endif

}  // namespace sr::gfx::pm4cov
