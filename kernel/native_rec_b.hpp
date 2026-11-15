#pragma once

// iter-467 native-b, P1 row 9 (docs/NATIVE_ROADMAP.md "Record interface for strand B"): the XDK's
// resolve / clear entries as host code writing NATIVE RECORDS instead of PM4.
//
//   D3DDeviceResolve           8210C5F8  -> kind kRkResolve   (one record span per piece between XDK kick sites)
//   D3DDeviceClearF            82101A58  -> kind kRkClear     (Clear 82101998 writes nothing itself: it re-enters ClearF)
//   SetShaderGPRAllocation     820F5570  -> kind kRkClearFull (+ its tail call D3DDeviceClearFullSurface 82100488)
//
// Each entry is a byte-exact port of the XDK body (IDA, the asm): it computes the same packets from the device
// and the call's arguments and makes the same device writes. What would have gone into the ring is kept as an OP
// LIST (the opaque record payload): type-0 runs and type-3 packets in XDK order, each predicated exactly as the XDK
// wrote it. The CP applies an op list through the engine's own native calls (nativeRun, nativeDraw,
// applyImLoadImmediate, applyEventWrite, applyBinReg; INVALIDATE_STATE / WAIT_REG_MEM consumed as execute() does);
// a resolve's copy-mode draw reaches pm4ExecResolveRegs exactly as on the PM4 path. render() writes the op list back
// as the XDK's bytes (spans shorter than the record marker, the CPPM4 oracle, the CHECK).
//
// The port is written once, against Sink: in record mode the sink is strand A's Entry (kernel/native_flush.hpp:
// kick() / alloc() at the XDK's own sites, opaque() per piece); in the dry CHECK it is a copy of the device, the
// pieces are rendered into a byte buffer, kick sites that would kick stop the dry run (the guest body's kick then
// makes the call a skip, counted), and the allocator runs as a native port of 820FCDF0's carve path.
//
// Gates:
//   SR_NB_REC            record mode, default ON (=0: the guest bodies write PM4 again; needs SR_NATIVE_FLUSH record mode).
//   SR_NB_REC_CHECK=1    dry CHECK (record mode off): per call the port runs on a device copy, then the guest body
//                        runs for real; ring bytes, every device dword (minus the async fields) and the guest writes
//                        outside the device are compared. [NB_REC] totals, [NB_REC_BAD] examples.
//   SR_NB_REC_KINDS=mask which entries (1 Resolve, 2 ClearF, 4 GPR / ClearFullSurface); default 7.
//
// C1: the dry CHECK and the NaN watch (SR_NB_NANWATCH) are development oracles, compiled out under SR_ORACLE=0
// (kernel/sr_oracle.hpp): their API below is then inline no-ops / constant false (what the entry hooks see with the
// gates unset). Record mode's own dry pre-pass (Prod with dry = true) is production: it decides the declines.

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "sr_oracle.hpp"

struct PPCContext;
namespace sr::gfx {
// C1 stage 1: the CP's engine (kernel/native_flush.hpp): the development PM4 walker, or the record engine alone in the
// release configuration (SR_ORACLE=0). The op list applies through record-engine methods only.
#if SR_ORACLE
class Pm4Engine;
using CpEngine = Pm4Engine;
#else
struct RecordEngine;
using CpEngine = RecordEngine;
#endif
}

namespace sr::native::recb {

constexpr uint32_t kDevSpan = 0x3600u;

enum Entry : int { kResolve = 0, kClearF = 1, kGpr = 2, kEntries };

// ---- the op list (payload words, host order) ----
//   T0:  (n << 16) | reg            (bit 31 clear), then n values (BIG-endian bytes, as the guest writes them)
//   T3:  0x80000000 | (pred << 30) | (opcode << 16) | n, then n body dwords (big-endian bytes)
//   PAD: 0x80FF0000 | n             n type-2 NOP dwords (0x80000000) the XDK writes for alignment (a T3 with the
//                                   opcode 0xFF, which the XDK never writes; pred clear - a predicated T3 is 0xC0..)
constexpr uint32_t kOpPad = 0x80FF0000u;
inline uint32_t opT0(uint32_t reg, uint32_t n) { return (n << 16) | (reg & 0x7FFFu); }
inline uint32_t opT3(uint32_t opcode, uint32_t n, bool pred) {
  return 0x80000000u | (pred ? 0x40000000u : 0u) | ((opcode & 0xFFu) << 16) | (n & 0xFFFFu);
}
// The XDK bytes of an op list (count) and the bytes themselves (out: big-endian ring words).
uint32_t renderBytes(const uint32_t* w, uint32_t nWords);
#if SR_ORACLE
void render(const uint32_t* w, uint32_t nWords, uint8_t* out);
#endif
// CP: apply an op list to the engine (no PM4 decode). false = malformed / an op it does not know.
bool apply(sr::gfx::CpEngine& eng, const uint32_t* w, uint32_t nWords);

// ---- the production context (one entry call) ----
struct Prod {
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
  uint32_t dev = 0;      // device VA
  uint8_t* d = nullptr;  // device bytes [0, kDevSpan): the real device, or the CHECK's copy
  bool dry = false;
  int mode = 0;  // 0 dry (the CHECK, and record mode's pre-pass that decides a decline), 2 live (strand A's Entry)
  void* entry = nullptr;  // live: the sr::native::flush::Entry
  bool stopped = false;  // the port reached something it does not model (dry: the CHECK compares nothing)
  const char* why = nullptr;

  // device fields (big-endian in guest memory)
  uint32_t r32(uint32_t off) const;
  uint64_t r64(uint32_t off) const;
  uint16_t r16(uint32_t off) const;
  uint8_t r8(uint32_t off) const { return d[off]; }
  void w32(uint32_t off, uint32_t v);
  void w64(uint32_t off, uint64_t v);
  void w8(uint32_t off, uint8_t v);
  // guest memory outside the device (reads: false = unmapped; writes: dry = recorded for the CHECK)
  bool g32(uint32_t va, uint32_t& v) const;
  bool g16(uint32_t va, uint16_t& v) const;
  bool g8(uint32_t va, uint8_t& v) const;
  bool gf32(uint32_t va, float& v) const;
  void gw32(uint32_t va, uint32_t v);
  void gwBytes(uint32_t va, const uint8_t* src, uint32_t n);  // raw bytes (already big-endian)
  struct GW {
    uint32_t va, off, n;  // the bytes are gwData[off, off + n)
  };
  std::vector<GW> gwrites;  // dry + recordGw only
  std::vector<uint8_t> gwData;
  bool recordGw = true;   // dry: keep the guest writes (the CHECK compares them); record mode's pre-pass does not
  bool keepBytes = true;  // dry: render the ring bytes (the CHECK compares them); record mode's pre-pass only counts them
  // dry: the device dwords in the async fence / pump block the port itself wrote (w32 / w64 / w8 / a guest write into
  // the device span) - the CHECK compares those, and only those, of the block. Tracked at the write, never by diffing
  // two reads of the live device (LLVM forwards memcpy(copy, before) to memcpy(copy, live): a second racing read).
  std::vector<uint32_t> asyncWrites;

  // the ring: the cursor model (the XDK's dev+0x28 as it would be now) and the current piece
  uint32_t cursor() const;   // where the next byte goes (the value the XDK would have in dev+0x28)
  uint32_t limit() const;    // dev+0x2C (82107520's run-fits test: cursor + 4n < limit)
  std::vector<uint32_t> ops;  // the current piece
  // ops (values host order; stored big-endian)
  void t0(uint32_t reg, std::initializer_list<uint32_t> v);
  void t0n(uint32_t reg, uint32_t n, const uint32_t* v);
  void t0dev(uint32_t reg, uint32_t n, uint32_t devOff);  // n values from the device shadow at devOff
  void t3(uint32_t opcode, bool pred, std::initializer_list<uint32_t> body);
  void t3n(uint32_t opcode, bool pred, std::initializer_list<uint32_t> head, uint32_t n, const uint8_t* beTail);
  void pad(uint32_t n);
  // the XDK's sites
  bool kick();                                   // `if (dev+0x28 > dev+0x30) 820FD8C0` at the XDK's own site
  // p1.flush_residue: 82107058's UNCONDITIONAL 820FD8C0 between the chunks of a run crossing the segment limit (live:
  // Entry::flush; the dry CHECK: a stop; the record pre-pass under split: a transparent kick).
  bool flush();
  uint32_t alloc(uint32_t dwords, uint32_t align);  // 820FCDF0 (0 = the XDK's failure)
  // p1.flush_residue (SR_NATIVE_FLUSH_SPLIT and its A/B window, decided once per entry; docs/NATIVE_ROADMAP.md
  // "p1.flush_residue BUILD NOTE" 3): split = 82107520's runs that cross the segment limit chunk across the kick
  // (82107058) instead of stopping; transparent = record mode's PRE-PASS does not stop at a kick site that would kick -
  // it marks `kicked` and goes on with the copy. Past such a kick the cursor, the limit and the allocator are unknown
  // (the kick family writes only them, the fence S +2 per close, the coherency range and byte 10809 - IDA), so the
  // pre-pass assumes every 820FCDF0 succeeds (a placeholder range [kPlaceholderVA, +phAlloc) whose writes are not
  // mapping-checked: the real allocation is command-buffer memory) and the resolve's S-dependent path declines on an
  // even S (fences are odd, +2 per close). The live pass and the dry CHECK never set transparent.
  // Under transparent the pre-pass also MODELS 820FCB30's wait for the GPU (frs1: stress_snap's clearF and rec_chk's
  // resolve met it; it was a stop): the wait only spins on the progress word the CP writes and returns the same
  // allocation (its one device write is the wait-time accumulator dev+20024 / 20032, which no stop reads; its timing
  // callback dev+13068 is never set in this title - the pre-pass still stops if it is), so the pre-pass marks `waited`
  // and goes on; the live pass's real 820FCDF0 then waits inside the Entry exactly as the guest body would (the CP
  // thread never takes the producer lock g_mu). The dry CHECK keeps the stop.
  bool split = false, transparent = false, kicked = false, waited = false;
  static constexpr uint32_t kPlaceholderVA = 0xFFF00000u;
  uint32_t phAlloc = 0;
  uint32_t chunks = 0;  // runs chunked across the limit in this call (82107058)
  void stop(const char* w) {
    stopped = true;
    if (!why) why = w;
  }
  bool finish();  // the last piece; dry: the cursor into the device copy
  // Back to a fresh call's state, the vectors keeping their capacity (the Prods are thread_local and reused: no heap
  // traffic per entry call).
  void reset();
  uint32_t kind = 0;  // the record kind of this entry's pieces
  uint32_t bytes = 0;  // ring bytes so far (all pieces)
  uint32_t items = 0;  // live: opaque record items written
  std::vector<uint8_t> dryBytes;  // dry: the rendered ring bytes of every piece
  uint32_t c0 = 0;  // dry: the cursor at entry
  uint32_t c = 0;   // dry: the cursor model
};

// ---- shared XDK building blocks (native_rec_b.cpp; each a port of the named guest function) ----
// Phase C (the dirty flush the three entry cores share): 0x2203 (821072F8 when q10 & 0x100, with the caller's
// re-OR quirk), then the 0x2200 / 0x2180 / 0x2100 / 0x2000 / 0x2280 / 0x2300 runs (82107520) and their mask
// clears. false = stopped (tiled 0x2203 branch, a run crossing the segment limit).
bool phaseC(Prod& p);
// 82107520(dev, mask, baseReg, dev+srcOff): one type-0 packet per contiguous run of set bits (MSB first).
bool regRuns(Prod& p, uint64_t mask, uint32_t reg, uint32_t srcOff);
// 821072F8 untiled: kick check, {0x2203, hiz}; returns q10 & ~0x100. Tiled (byte 0x2A3B & 0x20) -> stop.
bool hizWrite(Prod& p, uint32_t hiz);
// 820F0208 untiled: kick check, {0x00022080, 0, TL, BR} with TL = (y & 0x3FFF) << 16 | (x & 0x3FFF).
bool windowScissor(Prod& p, uint32_t l, uint32_t t, uint32_t r, uint32_t b);
// 8210C518: the 56-byte default state block (no kick check inside) and its dirty bits.
void defaultBlock(Prod& p);

// ---- the entries (record mode: true = done natively, the guest body must not run) ----
bool resolve(PPCContext& ctx, uint8_t* base);
bool clearF(PPCContext& ctx, uint8_t* base);
bool gprAlloc(PPCContext& ctx, uint8_t* base);
// The ports themselves (record mode and the CHECK's dry run share them).
void portResolve(Prod& p);
void portClearF(Prod& p);
void portGpr(Prod& p);

bool on();
#if SR_ORACLE
bool checkOn();
#else
inline bool checkOn() { return false; }  // SR_ORACLE=0: the dry CHECK is compiled out (reads as unset)
#endif
bool entryOn(int entry);
#if SR_ORACLE
// CHECK mode, around the guest body.
void checkBefore(int entry, PPCContext& ctx, uint8_t* base);
void checkAfter(int entry, PPCContext& ctx, uint8_t* base);
// True on this thread between a compared call's checkBefore and checkAfter, i.e. while the guest body whose ring bytes
// the CHECK compares is running. A native leaf the body calls (strand A's 820F0208 row-5 override) stays on the guest
// path while it is set, or the CHECK sees records where the XDK wrote PM4.
bool checkArmed();
#else
inline void checkBefore(int, PPCContext&, uint8_t*) {}
inline void checkAfter(int, PPCContext&, uint8_t*) {}
inline bool checkArmed() { return false; }
#endif
// Registers the CP-side handlers (apply + render) for kinds 7..9 (once, from the backend's PM4 init).
void registerHandlers();
// Record mode's per-entry counters (the difftest's wiring witness, tools/difftest/flush_split_test.inc, and the
// SR_NATIVE_FLUSH_SPLIT snapshot line): {calls, taken, kickT, chunks, kickTStop, liveStop, declined open, declined pre,
// waitT (taken through a modelled GPU wait)}.
constexpr int kRecCounters = 9;
void recCounters(int entry, uint64_t out[kRecCounters]);
// SR_NB_NANWATCH=<entry>[:<entry>...] (observe-only diagnostic; iter-467 black-world hunt), entry = PHYS (hex: one
// 32-bit BE word; every finite <-> non-finite transition is logged) or PHYS/LEN[h] (hex: a byte range of float32, or
// with h float16, values; every change of its non-finite count is logged, +inf counted apart). Polled once per guest
// VdSwap (sr::kernel::getVdSwapCount) from the guest's Resolve calls ([NB_NANWATCH]); a value line every 600 swaps.
// SR_NB_NANWATCH_TRACE=<k> (with SR_PM4_TRACE_HOTKEY=1): request a live PM4 trace on each of the first k presents
// (every 2nd) on which a watched word is non-finite.
#if SR_ORACLE
void nanWatchTick();
#else
inline void nanWatchTick() {}  // SR_ORACLE=0: the observe-only NaN watch is compiled out
#endif

}  // namespace sr::native::recb
