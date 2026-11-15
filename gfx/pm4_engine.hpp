#pragma once

// The development PM4 engine: Xenos PM4 command-stream parsing over the record engine (gfx/record_engine.hpp, which
// holds the device-state shadow and every record's effect). THE one parser, proven against the
// golden Xenia GPU trace (menu_pressstart: 63,054 packets, 3,501 draws,
// parse=CLEAN-EOF), fed by the trace replayer, the CP thread's ring / indirect-buffer path and the
// development oracles (CPPM4, the A/B OFF arms). Framing per the Xenia oracle (pm4_command_processor_implement.h) and
// Microsoft's own PM4 walker in this XEX (sub_82111798): a CONTIGUOUS stream
// of 4-byte big-endian dwords — type in bits 30-31, type-3 opcode in bits
// 15-8, count = ((hdr>>16)&0x3FFF)+1, INDIRECT_BUFFER recursion.
//
// History (iter-358h): the live path carried its own parser that walked the
// deleted fake device's 8-byte {0,dword} slot format with a pre-iter-358g
// opcode table — on the real command stream it read every other dword (6
// type-3 packets found in ~91 MB of real commands). RING_DUMP proved the real
// stream is contiguous and packet-aligned at (prev VdSwap r3) + 0x100. That
// parser is deleted; this engine replaces it.
//
// C1 (docs/RELEASE_COMPOSITION.md 2.1, stage 2a, 2026-10-15): the record path's members and methods moved to
// RecordEngine (the shipping class); what stays here is reached only by PM4 bytes - execute(), cpPacket(), the
// window scan, the parse-health counters, the record->PM4 oracle. C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): it
// is out of the release link (CMakeLists.txt EMULATION_SOURCES) - the default path reaches no packet since the
// transport and device-create bakes; the release configuration's CP applies records through RecordEngine alone and
// stops on a literal-PM4 gap, a kind-1 IB item or a ring packet. The development build keeps it as the oracle.

#include <cstddef>
#include <cstdint>

#include "kernel/sr_oracle.hpp"  // C1: the window-scan helpers (scanExtent, drainSegmentChain) and the executor are dev-only
#include "record_engine.hpp"

namespace sr {
namespace gfx {

struct Pm4Engine : RecordEngine {
  Pm4Engine();  // installs the opaque record dispatch (record_engine.hpp opaqueKnown / opaqueApply)

  bool opcodeUnknown[256] = {};

  // EVENT_WRITE_EXT (resolve trigger) capture.
  uint64_t resolveEvents = 0;           // EVENT_WRITE_EXT (0x59/0x5E) count
  uint32_t resolveBaseAtLastEvent = 0;  // copy-dest latch sampled at that event

  // iter-371: INDIRECT_BUFFER (0x3F/0x37) descriptors rejected as unmapped or
  // implausibly large. THIRD member of the live-parse health family (with
  // badShaderLoads and bogusShaderLoads) — all three must stay 0 on the golden
  // trace replay, whose IBs are real. A non-zero value live means the parser was
  // handed a descriptor that would have sent it into unrelated guest memory.
  uint64_t badIndirectBuffers = 0;
  // iter-369h: windows abandoned after a run of unknown opcodes (the parser had
  // left the command buffer). Like badShaderLoads this is a LIVE-PARSE HEALTH
  // metric; the golden trace replay must keep it at 0.
  uint64_t desyncStops = 0;
  // iter-369i: how much of the LAST top-level window the parser actually
  // consumed. A CORRECT window must be consumed exactly — a remainder proves the
  // window itself is wrong (e.g. we were handed an allocation STRIDE rather than
  // the length the guest actually wrote), independently of any opcode judgement.
  size_t lastWindowBytes = 0, lastConsumed = 0;
  uint64_t windowsShort = 0;   // windows that did not consume to the end
  // iter-369j: which opcode was the LAST successfully consumed packet before a
  // window terminated early. Since window LENGTH is correct (99.99% consumed,
  // 369i), a short exit means alignment was lost mid-window — so the opcode that
  // repeatedly precedes it is the one whose payload size we compute wrong.
  uint32_t desyncPrevOp[256] = {};
  // iter-369k: byte offset just past the LAST cleanly-parsed EVENT_WRITE_SHD
  // (0x58, the fence) in the last top-level window. If the guest's written data
  // really ends at its trailing fence, this sits well short of lastWindowBytes
  // and everything after it is stale — which is the hypothesis under test.
  size_t lastFenceEnd = 0;
  // iter-369l: header-only pre-scan returning the offset past the trailing fence
  // — the guest's WRITTEN extent. See the definition for why this exists.
  // validEnd (out) receives the offset where the header walk STOPPED looking
  // like PM4 — the fallback bound for windows that carry no trailing fence.
  //
  // iter-370: why the walk stopped. The bound is only as trustworthy as its
  // stop condition, and a WRONG early stop is indistinguishable from a short
  // idle frame in the extent number alone — both read "guest wrote 0.0%". This
  // names the dword that ended the walk so the two can be told apart.
  // C1 (docs/RELEASE_COMPOSITION.md 2.2): scanExtent and drainSegmentChain serve only the legacy VdSwap window scan
  // (gfx/null_renderer.cpp onVdSwapFrame, SR_PM4_CP=0), a fallback component compiled out of the release
  // configuration with them.
#if SR_ORACLE
  struct ScanStop {
    enum Reason : uint32_t { kWindowEnd, kTypeOne, kOverrun, kUnknownOp };
    size_t offset = 0;      // byte offset of the dword that stopped the walk
    uint32_t header = 0;    // that dword (host-endian view of the BE header)
    Reason reason = kWindowEnd;
  };
  size_t scanExtent(const uint8_t* host, size_t bytes, size_t* validEnd = nullptr,
                    ScanStop* stop = nullptr) const;
#endif
  uint64_t windowsTruncated = 0;   // windows bounded to their written extent

  uint64_t chainBlocksSkipped = 0;  // iter-397q: segment-chain tail blocks stepped over
  // iter-397q: the most recent tail block's payload — {next segment size in
  // dwords, next segment physical address}. The persistent chain consumer
  // (drainSegmentChain) seeds its park cursor from these.
  uint32_t lastChainTailSize = 0;
  uint32_t lastChainTailPhys = 0;
  // Persistent software-CP chain-consumer state (drainSegmentChain).
  uint32_t chainParkPhys = 0;       // where the next tail block will appear
  uint32_t chainStuckPolls = 0;     // consecutive polls with the hole unfilled
  uint64_t chainSeedTailCount = 0;  // chainBlocksSkipped at last seed adoption
  uint64_t chainSegmentsConsumed = 0;
  uint32_t chainSegDebt = 0;        // producer-counted segments not yet consumed

  // Consume the jump-chained segment stream from the persistent park cursor.
  // `newSegments` = segments the producer closed since the last call (from the
  // fence counter dev+10780, +2 per segment) — the drain never consumes more
  // than the accumulated debt (stale-hole cycle brake).
#if SR_ORACLE
  void drainSegmentChain(uint32_t newSegments);
#endif

  // The packet-only CP effects (cpMode): the EXT query write-backs and Xenia's fake sample counts. (The ZPD event
  // itself - cpZpdEvents / cpZpdNative, applyZpdEvent / applyZpd - is the record engine's since p2.zpd's bake: the
  // kRkZpd record and the 0x5B packet reach the one implementation.)
  uint64_t cpVizQueries = 0;
  uint64_t cpZpdFakeWrites = 0, cpExtEvents = 0;  // iter-467 p2 (3): SR_CP_ZPD_FAKE write-backs
  uint32_t cpZpdFakeCount = 0;                     // Xenia fake_zpd_sample_count_ (starts 0 -> first write 100)
  static bool cpZpdFakeOn();

  // Execute a contiguous PM4 region. `bytes` need not be packet-exact; a
  // packet extending past the end stops the walk (same as the replay).
  void execute(const uint8_t* host, size_t bytes, int depth = 0);

#if SR_ORACLE
  // NATIVE strand A executor (kernel/native_exec.hpp, SR_NATIVE_EXEC; dev-only since stage 0): perform a store item's
  // stores (executor thread).
  void execPerformStores(const GuestDrawCommand& c);
#endif

  // The development oracle's record loop: the same records through the PM4 opcode census (cpMode's CPPM4 split
  // and the census difftests), kRkPm4 executed as literal PM4.
  void nativeRecordOracle(const uint32_t* w, const uint32_t* e, NativeRecordStats& st, bool timing);

 private:
  // cpMode-only packet semantics (Xenia pm4_command_processor_implement.h).
  // Returns true when the opcode was handled here.
  bool cpPacket(uint32_t op, const uint8_t* body, uint32_t cnt);
  void cpStoreMem(uint32_t addrWithEndian, uint32_t value);
};

}  // namespace gfx
}  // namespace sr
