#pragma once

// NATIVE strand A: stream-resident constant dedup (docs/PERFORMANCE.md 2.13).
//
// The XDK draw flush copies every DIRTY ALU constant group (16 registers = 4 vec4 = 64 bytes)
// into the command buffer. In gameplay that is ~6.4 MB per frame and 83% of those groups are
// byte-identical to what the GPU constant file already holds at that point of the stream
// (trace live_5400). A group is skipped only when the value the STREAM last wrote to it (not the
// device shadow: direct packets and LOAD_ALU_CONSTANT write the GPU without the shadow) is the
// same 64 bytes.
//
// Two layers (so the native draw path reuses the core without the ring):
//   core     - the ALU constant file as the stream last wrote it: valid bit + 64 bytes per group,
//              invalidated by whoever knows another writer ran (no PM4 knowledge);
//   adapter  - the producer stream: segment epoch (segment start / limit / wrap generation, so
//              every IB range is self-contained and an IB replay - predicated tiling - re-runs
//              the writes a skip relies on), owner thread, the XDK record modes, and a
//              invalidation after unknown cursor advances. Oracle alone scans packet bytes.
//
//   SR_NATIVE_CONST_DEDUP          skip resident groups (default ON since iter-467 S2a; =0 off)
//   SR_NATIVE_CONST_DEDUP_CHECK=1  decide but emit everything (no skips); each run the dedup would skip
//                                  is announced by a 'DDCK' NOP and the CP proves, when it
//                                  executes that packet (replays included), that the register
//                                  file already holds those bytes ([CONST_DEDUP])
//   SR_NATIVE_CONST_DEDUP_MUTATE=1 diagnostic: no scan of other writers - the CHECK must fail
//   SR_NATIVE_CONST_DEDUP_MODES    also inside the XDK Z-pass / record modes (default ON; =0 emits all there)
//
// C1: the CHECK and the mutation test are development oracles, compiled out under SR_ORACLE=0 (kernel/sr_oracle.hpp):
// checkOn() then returns false and the check-mode API is inline no-ops, so its callers (the CP, the native flush and
// device) see what they see with SR_NATIVE_CONST_DEDUP_CHECK unset.

#include <cstdint>

#include "sr_oracle.hpp"

namespace sr::native::cdedup {

bool on();       // skips applied
bool checkOn();  // decide only + CP proof
inline bool active() { return on() || checkOn(); }

constexpr uint32_t kAluReg = 0x4000u, kAluRegs = 0x800u, kGroupRegs = 16u, kGroups = kAluRegs / kGroupRegs;

// ---- core -----------------------------------------------------------------------------------
enum Why : uint32_t { kWhyEpoch, kWhyThread, kWhyMode, kWhyMisframe, kWhyOp, kWhyGap, kWhyFallback, kWhyDevice, kWhyN };
void invalidateAll(Why w);
void invalidateRegs(uint32_t reg, uint32_t count);  // any register range; only its ALU part matters
bool resident(uint32_t group, const uint8_t* be64);  // valid and the same 64 bytes (guest byte order)
void store(uint32_t group, const uint8_t* be64);
uint32_t validGroups();  // groups the mirror holds (0 right after an epoch change: the producer-side check)

// ---- producer stream position (render thread) ---------------------------------------------
// Before an emission into the command buffer of device `dev` (host view devH, >= 13512 bytes)
// at cursor `cur` (the last dword written): epoch / thread / mode checks. Oracle scans
// packets written between emissions; native release invalidates on an unaccounted cursor
// advance, without reading those stale command bytes. false = emit all for this call.
bool syncStream(uint8_t* base, uint32_t dev, const uint8_t* devH, uint32_t cur);
// The emission ended with the cursor at `end` (the last dword it wrote).
void noteEmitted(uint32_t end);
// The emission went somewhere the adapter cannot follow (the guest body's chunked path): drop
// every group and the stream position.
void lostStream();
// Finalize stream position through `end`: Oracle scans packet bytes; release invalidates
// residency for any unaccounted cursor advance.
void advanceTo(uint32_t dev, uint32_t end);

// ---- check mode --------------------------------------------------------------------------------
// The render thread emits each run of would-be-skipped groups as its own type-0 packet, preceded by
// a type-3 NOP {0xC0001000, 'DDCK'}: the prediction travels in the stream, so every execution of it
// (IB replays included) is checked, and nothing goes stale.
constexpr uint32_t kCheckMagic = 0x4444434Bu;  // 'DDCK'
#if SR_ORACLE
void notePredictedRun(uint32_t groups);
// CP thread: a 'DDCK' NOP was executed / the next type-0 packet takes the prediction.
void cpNotePrediction();
bool cpTakePrediction();
// CP thread, before the type-0 write of `count` registers from `reg` whose data (big-endian) is at
// host `data`: every group must already be in `regFile` (host order).
void cpCheck(const uint8_t* data, uint32_t reg, uint32_t count, const uint32_t* regFile);
// CP thread (1a ndExecute): the same over a restored bracket's packets.
void cpCheckPackets(const uint8_t* host, uint32_t bytes, const uint32_t* regFile);
#else
// SR_ORACLE=0: no CHECK, so no prediction is ever marked or checked.
inline void notePredictedRun(uint32_t) {}
inline void cpNotePrediction() {}
inline bool cpTakePrediction() { return false; }
inline void cpCheck(const uint8_t*, uint32_t, uint32_t, const uint32_t*) {}
inline void cpCheckPackets(const uint8_t*, uint32_t, const uint32_t*) {}
#endif

// Counters (render thread).
void countGroups(uint32_t seen, uint32_t skipped);

// The S1 flush CHECK (kernel/native_flush, SR_NATIVE_FLUSH_CHECK) compares its native model with the
// plain XDK body on sampled draws: while suspended on this thread the dedup leaf emits nothing
// itself (the guest body runs, the adapter loses track), so both CHECKs can share one roll.
void suspend(bool s);
bool suspended();

}  // namespace sr::native::cdedup
