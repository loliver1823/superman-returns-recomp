// Shared Xenos PM4 engine — see pm4_engine.hpp. The execute() body is the
// golden-trace-proven executePM4 moved verbatim from gpu_trace_replay.cpp
// (iter-358g opcode table), with physToHost null guards added because the
// live path can hand it unmapped physical addresses.

#include "../kernel/tex_layout.hpp"   // iter-456p: the ONE canonical fetch decoder
#include "../kernel/gpu_hang_wd.hpp"   // iter-463: the fence-write latch
#include "../kernel/native_const_dedup.hpp"  // strand A: CONST_DEDUP_CHECK proof
#include "../kernel/native_drain_census.hpp"  // strand A: drain census (observe-only)
#include "pm4_live_trace.hpp"
#include "../kernel/sr_oracle.hpp"
#if SR_ORACLE
#include "draw_contract.hpp"          // iter-456p: the target draw-contract row
#endif
#include "pm4_engine.hpp"
#include "native_draw_builder.hpp"
#include <intrin.h>
#include "native_device.hpp"
#include "pm4_coverage.hpp"   // NATIVE N0.2: true per-packet counter (SR_PM4_COVERAGE)
#include "../kernel/native_meter.hpp"   // iter-467 native meter: PM4 packets interpreted
#include "../kernel/sr_oracle.hpp"      // C1: the SR_NATIVE_IDLE_DIAG timing compiles out under SR_ORACLE=0
#include "../kernel/native_flush.hpp"   // iter-467 native A S1: native draw records
#include "draw_ledger.hpp"              // iter-467 flicker: the per-draw ledger over frozen presents
#include "../kernel/native_exec.hpp"    // iter-467 native A: the executor (pipelined fences)
#include "../kernel/native_vblank.hpp"  // display.registers: the SCRATCH_REG1 (pc+4 rearm) generation
#include "null_renderer.hpp"            // nativeExec* (the executor thread beside the drain)
#include "native_zpd.hpp"               // p2.zpd: EVENT_WRITE_ZPD as host occlusion queries (SR_NATIVE_ZPD)
#include "gamma_ramp.hpp"
#include "shader_cache.hpp"
#include "xenos_state.hpp"
#include "xenos_ucode.hpp"
#include "cp_simd.hpp"
#include "../app/log.hpp"
namespace sr { namespace kernel { uint8_t* guestVAToHost(uint32_t guestVA); bool isGuestVAInRange(uint32_t guestVA); bool logVerbose(); } }

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "record_engine_detail.hpp"   // C1 stage 2a: the byte / register-store helpers shared with gfx/record_engine.cpp
namespace sr {
namespace gfx {
using namespace redetail;

// C1 stage 2a (docs/RELEASE_COMPOSITION.md 2.1): the opaque record kinds' handlers (kernel/native_flush.hpp
// setOpaqueHandler) take this class; the record engine dispatches through these hooks until the registry names
// RecordEngine (after the transport bake merges). Installed by every instance: idempotent, process-wide like the registry.
Pm4Engine::Pm4Engine() {
  RecordEngine::opaqueKnown = [](uint32_t kind) { return sr::native::flush::opaqueHandler(kind) != nullptr; };
  RecordEngine::opaqueApply = [](RecordEngine& eng, uint32_t kind, const uint32_t* w, uint32_t n) {
    const sr::native::flush::OpaqueHandler h = sr::native::flush::opaqueHandler(kind);
    return h != nullptr && h(kind, w, n, static_cast<Pm4Engine&>(eng));
  };
}

void Pm4Engine::nativeRecordOracle(const uint32_t* w, const uint32_t* e, NativeRecordStats& st, bool timing) {
  auto& xr = getXenosRegisters();
  const bool bulk = bulkType0Enabled();
  static const bool s_bulkSse = cpsimd::gateLogged("SR_CP_BULK_SSE", true) && cpsimd::available();
  while (w < e) {
    const uint32_t tag = *w++;
    if (tag & 0x80000000u) {  // PACKET (native_flush.hpp RecKind): kind 0 = DRAW: initiator, dmaBase, dmaSize
      namespace nf = sr::native::flush;
      const uint32_t kind = (tag >> 24) & 0x7Fu, n = (tag >> 8) & 0xFFFFu;
      const bool pred = (tag & 2u) != 0;
      if (kind == nf::kRkDraw) {
        const uint64_t d0 = timing ? __rdtsc() : 0;
        nativeDraw(pred, w[0], w[1], w[2]);
        if (timing) st.drawTsc += __rdtsc() - d0;
        w += 3;
        st.draws++;
        continue;
      }
      const uint32_t words = nf::recWords(kind, n);
      if (!words || w + words > e) {  // not a record this CP knows: the rest of the record is unreadable
        st.bad++;
        return;
      }
      st.packets++;
      // Bin predication, as execute() for a predicated type-3 packet: consumed, not run.
      if (pred && (binSelect & binMask) == 0) {
        predSkipped++;
        w += words;
        continue;
      }
      switch (kind) {
        case nf::kRkShaderPtr:
          opcodeHist[0x27]++;
          applyImLoad(w[0], w[1]);
          break;
        case nf::kRkShaderImm:
          opcodeHist[0x2B]++;
          applyImLoadImmediate(w[0] & 0x3u, w[1], reinterpret_cast<const uint8_t*>(w + 2), n + 2u);
          break;
        case nf::kRkConstLoad:
          opcodeHist[0x2F]++;
          applyLoadAluConstant(w[0], w[1], w[2]);
          break;
        case nf::kRkSetConst:
          opcodeHist[0x2D]++;
          applySetConstant(w[0], reinterpret_cast<const uint8_t*>(w + 1), n);
          break;
        case nf::kRkBinReg:
          opcodeHist[0x60u + (n & 3u)]++;
          applyBinReg(n & 3u, w[0]);
          break;
        case nf::kRkEvent:
          opcodeHist[0x46]++;
          applyEventWrite(w[0]);
          break;
        case nf::kRkZpd:  // p2.zpd's bake: EVENT_WRITE_ZPD {initiator} (the record exists only with the native path on)
          opcodeHist[0x5B]++;
          applyZpdEvent(w[0], true);
          break;
        case nf::kRkFence:  // P2-B: the segment's fence tail {fb4|2, progress, fb0|2, S}
          opcodeHist[0x58] += 2;
          applyEventWriteShd(3u, w[0], w[1]);
          applyEventWriteShd(3u, w[2], w[3]);
          break;
        case nf::kRkInvalidate:  // P2-B: the coherency block {base, size, whole}
          opcodeHist[0x3C]++;
          applyCoherency(w[0], w[1], w[2] != 0u);
          break;
        case nf::kRkRegRmw:  // boot.device_create: {rmwInfo, andMask, orMask}
          opcodeHist[0x21]++;
          applyRegRmw(w[0], w[1], w[2]);
          break;
        case nf::kRkEventShd:  // boot.device_create: {initiator, addr|endian, value}
          opcodeHist[0x58]++;
          applyEventWriteShd(w[0], w[1], w[2]);
          break;
        case nf::kRkPm4:  // P2-B: literal PM4 dwords (the gap bridge's record form)
          execute(reinterpret_cast<const uint8_t*>(w), 4u * n, 1);
          break;
        default:  // strand B's opaque kinds: the handler the owner registered
          if (nf::OpaqueHandler h = nf::opaqueHandler(kind)) {
            if (!h(kind, w, n, *this)) st.bad++;
          } else {
            st.bad++;
          }
          break;
      }
      w += words;
      continue;
    }
    const uint32_t count = (tag >> 16) & 0x3FFFu, reg = tag & 0x7FFFu;
    const uint8_t* be = reinterpret_cast<const uint8_t*>(w);
    // S2a CHECK run (bit 30): constant groups the dedup predicted the register file already holds.
    if (tag & 0x40000000u) sr::native::cdedup::cpCheck(be, reg, count, xr.mutableData());
    if (!bulk) {
      for (uint32_t i = 0; i < count; i++) writeReg(reg + i, readBE32(be + i * 4));
    } else if (!regRangeHasEffects(reg, count)) {
      // The type-0 branch of execute(), and a lone register the same way: with no side-effect
      // register in it, writeReg is exactly the register-file store (plus the counter).
      if (count == 1) {
        xr.write(reg, readBE32(be));
      } else if (s_bulkSse) {
        bulkBeStore(xr, reg, be, count);
      } else {
        for (uint32_t i = 0; i < count; i++) xr.write(reg + i, readBE32(be + i * 4));
      }
      regWrites += count;
    } else {
      // Split at the side-effect registers: writeReg for those (every effect as the PM4 path
      // gives it), bulk stores for the plain stretches between them (a fetch run that starts at
      // slot 0 no longer pays writeReg for the other 186 registers).
      uint32_t i = 0;
      while (i < count) {
        if (regRangeHasEffects(reg + i, 1)) {
          writeReg(reg + i, readBE32(be + i * 4));
          i++;
          continue;
        }
        uint32_t j = i + 1;
        while (j < count && !regRangeHasEffects(reg + j, 1)) j++;
        if (j - i == 1) xr.write(reg + i, readBE32(be + i * 4));
        else if (s_bulkSse) bulkBeStore(xr, reg + i, be + i * 4, j - i);
        else
          for (uint32_t k = i; k < j; k++) xr.write(reg + k, readBE32(be + k * 4));
        regWrites += j - i;
        i = j;
      }
    }
    w += count;
    st.runs++;
  }
}

// iter-369l: header-only pre-scan for the guest's WRITTEN extent.
//
// Measured (369k): in IDLE windows the guest writes ~424 bytes of real content
// and the r3-delta window is ~229,248 — so executing the whole window decodes a
// quarter-megabyte of STALE memory as PM4. That is the source of the string
// tables and PE headers decoded as shader ucode, the bogus resolve destinations,
// and 254-of-256 distinct opcodes. In BUSY windows the trailing fence already
// sits at the end (fenceEnd == consumed), so they must not be touched.
//
// Walks headers only — executes nothing — and returns the offset just past the
// LAST EVENT_WRITE_SHD (0x58) reached before the stream stops looking like PM4.
// A frame legitimately contains MANY fences (golden census: 12/frame), so it is
// the TRAILING one that bounds the window.
// iter-397q PLATFORM CONTRACT (SR_PM4_CHAINSKIP): the segment-chain TAIL BLOCK.
// GpuCmdBufWriteJumpPacket_sub_820FCA80 closes every command segment (once the
// driver enters jump-chain mode, dev+10868 != 0 — all of FMV playback) with a
// 4-dword block that is NOT PM4:
//   [0] continue-at pointer in the write-combined -0x40000000 space (0xCxxxxxxx)
//   [1] next-segment size in dwords | 0x82000000
//   [2] next-segment physical address
//   [3] 0xC0000000 (the replay driver's park/sync marker)
// It is consumed by the software command processor (VdInterruptPumpWait ->
// GpuCommandBufferReplayDriver), never by PM4 parsing. Read linearly it decodes
// as a type-3 header with a ~2650-dword count, which (a) made execute() skip
// ~10KB of real packets ([PM4_REJECT] 8200004A signature) and (b) made
// scanExtent stop at +40 and truncate every FMV window to the swap fence
// ([PM4_EXTENT] 229248 -> 40 bytes), discarding the movie frames — including
// the YUV pixel-shader IM_LOAD — wholesale. Segments are carved sequentially
// from the same buffer, so stepping over the 4 dwords continues into the next
// segment exactly where the chain points in the common in-order case.
bool pm4ChainSkipEnabled() {
  // iter-398d: baked default-ON (=0 keeps the A/B) after proving across
  // K-runs that tail-block recognition + the fence-budgeted chain drain
  // revive live FMV window walking (iter-397q evidence).
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_CHAINSKIP");
    return !e || e[0] != '0';
  }();
  return v;
}

static inline bool isSegmentChainTailBlock(const uint8_t* p) {
  const uint32_t d0 = readBE32(p);
  const uint32_t d1 = readBE32(p + 4);
  const uint32_t d2 = readBE32(p + 8);
  const uint32_t d3 = readBE32(p + 12);
  return (d0 >> 28) == 0xCu &&                 // WC-space continue pointer
         (d1 & 0xFFF00000u) == 0x82000000u &&  // size | marker
         d2 < 0x20000000u && (d2 & 3u) == 0 && // plausible dword-aligned phys
         d3 == 0xC0000000u;                    // park marker
}

// C1 (docs/RELEASE_COMPOSITION.md 2.2): scanExtent and drainSegmentChain belong to the legacy VdSwap window scan
// (SR_PM4_CP=0), compiled out of the release configuration (SR_ORACLE=0) with it.
#if SR_ORACLE
size_t Pm4Engine::scanExtent(const uint8_t* host, size_t bytes,
                             size_t* validEnd, ScanStop* stop) const {
  if (validEnd) *validEnd = 0;
  if (stop) *stop = ScanStop{};
  if (!host) return 0;
  size_t off = 0, lastFence = 0;
  auto note = [&](ScanStop::Reason r, size_t o, uint32_t h) {
    if (stop) { stop->reason = r; stop->offset = o; stop->header = h; }
  };
  while (off + 4 <= bytes) {
    const uint32_t hdr = readBE32(host + off);
    if (hdr == 0) { off += 4; continue; }          // NOP padding
    if (pm4ChainSkipEnabled() && off + 16 <= bytes &&
        isSegmentChainTailBlock(host + off)) {
      off += 16;
      if (validEnd) *validEnd = off;
      // The chain continues in ANOTHER segment (execute() follows the jump) —
      // the window bound must reach the tail block, so count it like a
      // trailing fence. Without this the fence-bound cuts the window at the
      // swap fence (+0x28) and execute() never sees the block at all.
      lastFence = off;
      continue;
    }
    const uint32_t type = (hdr >> 30) & 3u;
    if (type == 1u) { note(ScanStop::kTypeOne, off, hdr); break; }  // not PM4
    if (type == 2u) { off += 4; continue; }         // type-2 NOP
    const uint32_t cnt = ((hdr >> 16) & 0x3FFFu) + 1u;
    const size_t pktSz = (1 + static_cast<size_t>(cnt)) * 4;
    if (off + pktSz > bytes) {                      // overruns the window
      note(ScanStop::kOverrun, off, hdr);
      break;
    }
    // iter-369o: a type-3 header must carry an opcode this engine actually
    // implements. Header SHAPE alone is too weak — garbage that happens to look
    // well-formed extended the bound and kept ~150 spurious opcodes alive.
    // Every opcode listed here appears in execute()'s switch; the golden-trace
    // parity gate (3501 draws, CLEAN-EOF) proves the list is complete, since
    // omitting a real one would truncate the trace immediately.
    if (type == 3u) {
      const uint32_t op = (hdr >> 8) & 0xFFu;
      static const auto kKnown = [] {
        std::array<bool, 256> k{};
        for (uint32_t o : {0x01u, 0x21u, 0x22u, 0x23u, 0x27u, 0x2Bu, 0x2Du,
                           0x2Fu, 0x36u, 0x37u, 0x3Bu, 0x3Cu, 0x3Fu, 0x45u,
                           0x46u, 0x54u, 0x58u, 0x59u, 0x5Eu, 0x60u, 0x61u,
                           0x62u, 0x63u, 0x64u})
          k[o] = true;
        return k;
      }();
      if (!kKnown[op]) {                            // not a command stream here
        note(ScanStop::kUnknownOp, off, hdr);
        break;
      }
      if (op == 0x58u) lastFence = off + pktSz;
    }
    off += pktSz;
    if (validEnd) *validEnd = off;                  // last good packet boundary
  }
  return lastFence;
}

// iter-397q: THE PERSISTENT SEGMENT-CHAIN CONSUMER — our implementation of the
// console's software command processor (VdInterruptPumpWait ->
// GpuCommandBufferReplayDriver) for the jump-chain submission mode. Contract:
// after a segment's `size` dwords comes a 4-dword hole; when the NEXT segment
// closes, the producer fills it with a tail block naming that segment
// {size, phys}. So: consume segment -> park at segPhys+size*4 -> poll the hole
// -> follow when filled. The park cursor lives across windows (the console
// pump consumes each segment exactly once). Seeding: adopt the newest tail
// block the window scan recorded — at start, and again only if the cursor
// wedges (rotating-pool wrap / missed hole: >120 quiet polls ≈ 2 s).
void Pm4Engine::drainSegmentChain(uint32_t newSegments) {
  if (!pm4ChainSkipEnabled() || !physToHost) return;
  // iter-397q CYCLE BRAKE: the pool's holes retain STALE tail blocks from the
  // previous lap, which still pass the signature — an unbudgeted drain chased
  // them in a circle (live: seg#2049..16385 all re-consuming phys 0x0A3C8380).
  // The producer's own fence counter (dev+10780, +2 per closed segment —
  // iter-371) names exactly how many segments exist; the caller passes the
  // delta and we consume no more than the accumulated debt.
  chainSegDebt += newSegments;
  if (chainSegDebt > 512) chainSegDebt = 512;   // bound catch-up after gaps
  uint32_t nextPhys = 0, nextSize = 0;
  // Adopt the newest tail block the window scan recorded — EVERY time one
  // appears. The swap window carries the live edge (the swap stub's
  // freshly-filled hole); a cursor that follows only park-holes gets lapped by
  // the rotating pool and orbits stale history (live: seg#513..2049 all
  // re-consuming phys 0x0A220000). Segments still parked-in-flight when a new
  // seed arrives are skipped rather than replayed — the live edge wins.
  if (chainBlocksSkipped != chainSeedTailCount) {
    chainSeedTailCount = chainBlocksSkipped;
    nextPhys = lastChainTailPhys;
    nextSize = lastChainTailSize;
    chainParkPhys = 0;
    chainStuckPolls = 0;
  }
  for (uint32_t guard = 0; guard < 64 && chainSegDebt > 0; guard++) {
    if (!nextPhys) {
      if (!chainParkPhys) return;
      uint8_t* ph = physToHost(user, chainParkPhys);
      if (!ph || !isSegmentChainTailBlock(ph)) {
        chainStuckPolls++;   // hole not filled yet — stay parked, retry later
        return;
      }
      nextSize = readBE32(ph + 4) & 0xFFFFFu;
      nextPhys = readBE32(ph + 8);
    }
    if (nextSize == 0 || nextSize > 0x40000u) { chainParkPhys = 0; return; }
    uint8_t* sh = physToHost(user, nextPhys);
    if (!sh) { chainParkPhys = 0; return; }
    chainSegmentsConsumed++;
    static std::atomic<uint32_t> s_dLog{0};
    const uint32_t dn = s_dLog.fetch_add(1, std::memory_order_relaxed);
    if (dn < 8 || (dn & (dn - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_CHAIN] drain seg#%llu phys=0x%08X %u dwords",
                  (unsigned long long)chainSegmentsConsumed, nextPhys, nextSize);
    execute(sh, static_cast<size_t>(nextSize) * 4, 0);
    chainParkPhys = nextPhys + nextSize * 4u;
    chainStuckPolls = 0;
    if (chainSegDebt > 0) chainSegDebt--;
    nextPhys = 0;
    nextSize = 0;
  }
}
#endif  // SR_ORACLE (the legacy window scan's helpers)

void Pm4Engine::execute(const uint8_t* host, size_t bytes, int depth) {
  if (!host || depth > 4 || nativeRecordFailed) return;
  // NATIVE N0.2 (gfx/pm4_coverage.hpp, SR_PM4_COVERAGE / SR_NATIVE_BRACKETS, default
  // OFF): every packet this loop interprets is counted once, at its decision point.
  const bool cov = pm4cov::on();
  if (cov) pm4cov::windowBegin(host, bytes, depth);
  // Record consumption for the TOP-LEVEL window only (nested IB calls recurse).
  struct WindowTracker {
    Pm4Engine* e; int d; size_t bytes; const size_t* off; const size_t* fence;
    ~WindowTracker() {
      if (d != 0) return;
      e->lastWindowBytes = bytes;
      e->lastConsumed = *off;
      e->lastFenceEnd = *fence;
      if (*off < bytes) e->windowsShort++;
    }
  };
  // iter-369h: consecutive unknown type-3 opcodes within THIS window. Reset by
  // any known opcode; kUnknownRunLimit of them means we are off packet
  // boundaries and the rest of the window is abandoned (see the DESYNC BRAKE).
  constexpr uint32_t kUnknownRunLimit = 4;
  uint32_t unknownRun = 0;
  size_t off = 0;
  size_t fenceEnd = 0;
  WindowTracker wt{this, depth, bytes, &off, &fenceEnd};
  // The window a stalled CP wait reports (restored when a nested window returns).
  struct ExecCur {
    Pm4Engine* e; const uint8_t* h; size_t b, o; int d;
    ~ExecCur() { e->stallHost = h; e->stallBytes = b; e->stallOff = o; e->stallDepth = d; }
  } execCur{this, stallHost, stallBytes, stallOff, stallDepth};
  stallHost = host;
  stallBytes = bytes;
  stallDepth = depth;
  // iter-467 NATIVE METER (kernel/native_meter.hpp): every header this window decodes, added once
  // per window (nested IB windows add their own).
  struct MeterAdd {
    uint64_t n = 0;
    uint32_t k[sr::native::meter::kKinds] = {};
    ~MeterAdd() {
      if (n) sr::native::meter::add(sr::native::meter::kPm4, n);
      for (uint32_t i = 0; i < sr::native::meter::kKinds; i++) sr::native::meter::addKind(i, k[i]);
    }
  } meterPk;
  // Last successfully consumed type-3 opcode — the suspect if we exit early.
  uint32_t lastGoodOp = 0xFFFFFFFFu;
  size_t lastGoodOff = 0, firstUnknownOff = 0;
  auto blameLastOp = [&]() {
    if (lastGoodOp < 256) desyncPrevOp[lastGoodOp]++;
  };
  while (off + 4 <= bytes && !nativeRecordFailed) {
    uint32_t hdr = readBE32(host + off);
    uint32_t type = (hdr >> 30) & 0x3;
    stallOff = off;
    if (hdr == 0) {
      if (cov) pm4cov::packetOther(host + off, depth, pm4cov::kZeroDword);
      off += 4;
      continue;
    }
    meterPk.n++;
    // The meter's kinds (native_meter.hpp Kind). A native record marker ('NFLU') is not a PM4 packet.
    if (type == 0u) {
      meterPk.k[sr::native::meter::kT0]++;
    } else if (type == 1u) {
      meterPk.k[sr::native::meter::kT1]++;
    } else if (type == 2u) {
      meterPk.k[sr::native::meter::kT2Nop]++;
    } else {
      const uint32_t op3 = (hdr >> 8) & 0xFFu;
      const uint32_t b0 = off + 8u <= bytes ? readBE32(host + off + 4) : 0u;
      const uint32_t b1 = off + 12u <= bytes ? readBE32(host + off + 8) : 0u;
      if (!(op3 == 0x10u && b0 == sr::native::flush::kMarkerMagic))
        meterPk.k[sr::native::meter::kindOf3(op3, b0, b1)]++;
    }
    // iter-397q: segment-chain tail block (see isSegmentChainTailBlock above) —
    // 4 dwords of software-CP chain metadata, not PM4. Segments rotate through
    // a ~6MB pool during FMV (live chain: 0x0A29..., 0x0A55..., 0x0A61..., NOT
    // sequential), and each one's own tail block sits right after its `size`
    // dwords. The engine just records the latest {size, phys} and steps over;
    // the PERSISTENT chain consumer (null_renderer drainSegmentChain — a park
    // cursor that survives across windows, like the console's replay pump)
    // does the actual following. An in-window follow was tried first and
    // re-walked the chain from scratch every frame — wrong model.
    // Never in cpMode: the CP executes exactly what the ring publishes, and the
    // game's own pump thread (GpuCommandBufferReplayDriver_sub_82110840) is what
    // follows the chain and kicks each segment.
    if (!cpMode && pm4ChainSkipEnabled() && off + 16 <= bytes &&
        isSegmentChainTailBlock(host + off)) {
      const uint32_t nextSize = readBE32(host + off + 4) & 0xFFFFFu;
      const uint32_t nextPhys = readBE32(host + off + 8);
      chainBlocksSkipped++;
      if (cov) pm4cov::packetOther(host + off, depth, pm4cov::kChainTail);
      lastChainTailSize = nextSize;
      lastChainTailPhys = nextPhys;
      static std::atomic<uint32_t> s_csLog{0};
      const uint32_t csn = s_csLog.fetch_add(1, std::memory_order_relaxed);
      if (csn < 4 || (csn & (csn - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PM4_CHAIN] tail block at +0x%zX next={phys=0x%08X %u dwords}"
                    " total=%llu",
                    off, nextPhys, nextSize,
                    (unsigned long long)chainBlocksSkipped);
      off += 16;
      continue;
    }
    if (type == 3) {
      uint32_t op = (hdr >> 8) & 0xFF;
      uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1;
      size_t pktSz = (1 + static_cast<size_t>(cnt)) * 4;
      if (off + pktSz > bytes) {
        if (cov) pm4cov::packetOther(host + off, depth, pm4cov::kTruncated);
        blameLastOp();
        break;
      }
      const uint8_t* body = host + off + 4;
      // Bin predication (Xenia pm4_command_processor_implement.h:411-418): a
      // type-3 packet with header bit 0 set executes only while
      // (binSelect & binMask) != 0. Skipped packets are consumed, not run.
      const bool traced = cpMode && sr::gfx::livetrace::armed();
      if ((hdr & 1) && (op == 0x22u || op == 0x36u) && (binSelect & binMask) != 0) predDrawsRun++;
      curPredicated = (hdr & 1) && (op == 0x22u || op == 0x36u);
      if ((hdr & 1) && (binSelect & binMask) == 0) {
        predSkipped++;
        if (cov) pm4cov::packet3(host + off, depth, op, cnt, true);
        if (traced) {
          sr::gfx::livetrace::packetBegin(host + off, uint32_t(1u + cnt));
          sr::gfx::livetrace::packetEnd(0u, false);
        }
        off += pktSz;
        continue;
      }
      // NATIVE mode B 1a (gfx/native_device, SR_NATIVE_DRAW): a draw bracket the render thread
      // replaced by {NOP, 'NDRW', slot}. ndExecute puts the original packets back and runs the
      // draw from the native projection (or the original packets, its exact fallback).
      // NATIVE strand A (SR_NATIVE_CONST_DEDUP_CHECK): {NOP, 'DDCK'} announces that the next type-0
      // packet carries constants the dedup would have skipped.
      if (op == 0x10u && cnt >= 1u && readBE32(body) == sr::native::cdedup::kCheckMagic &&
          sr::native::cdedup::checkOn())
        sr::native::cdedup::cpNotePrediction();
      // NATIVE strand A S1 (kernel/native_flush, SR_NATIVE_FLUSH): {NOP, 'NFLU', pos} = a native
      // draw record in place of the XDK's packets; not a PM4 packet for the native meter.
      if (op == 0x10u && cnt >= 3u && readBE32(body) == sr::native::flush::kMarkerMagic) {
        sr::native::flush::cpExecute(*this, body, cnt);
        meterPk.n--;
        if (nativeRecordFailed) break;
        off += pktSz;
        continue;
      }
      if (op == 0x10u && cnt >= 2u && readBE32(body) == 0x4E445257u && sr::native::dev::ndOn()) {
        struct Ctx {
          Pm4Engine* e;
          int depth;
        } ctx{this, depth + 1};
        sr::native::dev::ndExecute(
            const_cast<uint8_t*>(host + off), readBE32(body + 4),
            [](void* c, const uint8_t* p, size_t n) {
              Ctx* x = static_cast<Ctx*>(c);
              x->e->execute(p, n, x->depth);
            },
            &ctx);
        off += pktSz;
        continue;
      }
      // iter-460 live trace (pm4_live_trace.hpp): Xenia records an
      // INDIRECT_BUFFER as 2 dwords and its packets individually.
      const uint64_t drawsBefore = draws;
      if (traced) sr::gfx::livetrace::packetBegin(host + off, op == 0x3Fu ? 2u : uint32_t(1u + cnt));
      opcodeHist[op]++;
      bool skip3 = false;  // NATIVE step 3: a SET_CONSTANT / LOAD_ALU_CONSTANT the projection carries
      if (cov) {
        // NATIVE N1 (gfx/native_device): the active shader pair a draw executes with.
        if (op == 0x22u || op == 0x36u)
          pm4cov::setDrawShaders(lastVsUcodeHash, lastPsUcodeHash, lastVsConstUse, lastPsConstUse,
                                 lastVsLoadAddr, lastVsLoadDwords);
        skip3 = pm4cov::packet3(host + off, depth, op, cnt, false);
      }
      const uint32_t unknownBefore = unknownRun;
      // NATIVE mode A' DRIVEN: the draw packet ran on the substituted registers; restore after.
      struct DriveRestore {
        bool on;
        ~DriveRestore() {
          if (on) pm4cov::afterDraw();
        }
      } driveRestore{cov && (op == 0x22u || op == 0x36u)};
      if (!skip3 && !(cpMode && cpPacket(op, body, cnt))) switch (op) {
        case 0x01: {  // SET_CONFIG_REG
          if (cnt >= 2) {
            uint32_t regAddr = 0x2000 + readBE32(body);
            for (uint32_t i = 1; i < cnt; i++)
              writeReg(regAddr + (i - 1), readBE32(body + i * 4));
          }
          break;
        }
        // Opcode table per the Xenia oracle (iter-358g):
        // reference/xenia-canary/src/xenia/gpu/xenos.h:1645-1659 +
        // pm4_command_processor_implement.h.
        case 0x21: {  // PM4_REG_RMW (Xenia pm4_command_processor_implement.h:758-782)
          if (cnt >= 3) applyRegRmw(readBE32(body), readBE32(body + 4), readBE32(body + 8));
          break;
        }
        case 0x22: {  // PM4_DRAW_INDX — [viz_token, initiator, (DMA_BASE, DMA_SIZE)]
          // Xenia ExecutePacketType3_DRAW_INDX (:1165-1180) + shared parser
          // (:1049-1148): source_select kDMA(0) carries the index buffer in the
          // packet; kAutoIndex(2) carries nothing further.
          if (cnt >= 2) {
            uint32_t initiator = readBE32(body + 4);
            uint32_t dmaBase = 0, dmaSize = 0;
            if (((initiator >> 6) & 3u) == 0u && cnt >= 4) {
              dmaBase = readBE32(body + 8);
              dmaSize = readBE32(body + 12);
            }
            emitDraw(initiator, dmaBase, dmaSize);
          }
          break;
        }
        case 0x36: {  // PM4_DRAW_INDX_2 — [initiator, ...]; kAutoIndex in practice
          if (cnt >= 1) {
            uint32_t initiator = readBE32(body);
            uint32_t dmaBase = 0, dmaSize = 0;
            if (((initiator >> 6) & 3u) == 0u && cnt >= 3) {
              dmaBase = readBE32(body + 4);
              dmaSize = readBE32(body + 8);
            }
            emitDraw(initiator, dmaBase, dmaSize);
          }
          break;
        }
        // NATIVE strand A P1 rows 1+2: the bodies of 0x2D / 0x2F / 0x27 / 0x2B / 0x46 / 0x60..0x63
        // live in Pm4Engine::apply* (above nativeRecord), shared with the native records.
        case 0x2D:  // PM4_SET_CONSTANT — THE register write path.
          if (cnt >= 1) applySetConstant(readBE32(body), body + 4, cnt - 1u);
          break;
        case 0x2F:  // PM4_LOAD_ALU_CONSTANT — load constants FROM MEMORY
          if (cnt >= 3) applyLoadAluConstant(readBE32(body), readBE32(body + 4), readBE32(body + 8));
          break;
        case 0x27:  // PM4_IM_LOAD — shader ucode, pointer to memory.
          if (cnt >= 2) applyImLoad(readBE32(body), readBE32(body + 4));
          break;
        case 0x2B:  // PM4_IM_LOAD_IMMEDIATE — shader ucode INLINE in the packet.
          if (cnt >= 2) applyImLoadImmediate(readBE32(body) & 0x3u, readBE32(body + 4), body + 8, cnt);
          break;
        case 0x3F:    // INDIRECT_BUFFER
        case 0x37: {  // INDIRECT_BUFFER_PFD (MS walker sub_82111798 recurses both)
          if (cnt >= 2) {
            uint32_t ibPhys = readBE32(body);
            uint32_t ibSizeDwords = readBE32(body + 4) & 0xFFFFF;
            uint8_t* ibHost = physToHost(user, ibPhys);
            // iter-371: BOUND THE RECURSION. This descriptor was trusted
            // outright, and its size field alone spans 0xFFFFF dwords (4 MB) —
            // so one garbage IB made execute() decode megabytes of unrelated
            // guest memory as PM4. Caught in the wild: run0 logged
            // "[PM4_DESYNC] 4 consecutive unknown opcodes at +0xCEBE0 of
            // 0x2CF900", a 2,947,840-byte window. The LIVE top-level path can
            // never produce one — null_renderer.cpp caps its window at
            // 0x100000 — so it could only have come from here.
            //
            // Same failure class as the iter-369d IM_LOAD payload bug (a
            // declared size is not evidence), which was fixed for IM_LOAD and
            // left unfixed for 0x3F/0x37. The golden trace is the completeness
            // gate: its IBs are real, so parity must stay draws=3501 CLEAN-EOF.
            constexpr uint32_t kMaxIbDwords = 0x100000u / 4u;   // 1 MB
            if (!ibHost || ibSizeDwords == 0 || ibSizeDwords > kMaxIbDwords) {
              badIndirectBuffers++;
              static std::atomic<uint32_t> s_ibLog{0};
              const uint32_t in = s_ibLog.fetch_add(1, std::memory_order_relaxed);
              if (in < 8 || (in & (in - 1)) == 0)
                SR_LOG_BOOT(sr::LogSeverity::Warn,
                            "[PM4_REJECT] INDIRECT_BUFFER descriptor rejected: "
                            "phys=0x%08X size=%u dwords (cap %u, host=%s); "
                            "total=%llu | op=0x%02X cnt=%u raw=[%08X %08X %08X %08X]",
                            ibPhys, ibSizeDwords, kMaxIbDwords,
                            ibHost ? "ok" : "UNMAPPED",
                            (unsigned long long)badIndirectBuffers,
                            op, cnt, cnt > 0 ? readBE32(body) : 0,
                            cnt > 1 ? readBE32(body + 4) : 0,
                            cnt > 2 ? readBE32(body + 8) : 0,
                            cnt > 3 ? readBE32(body + 12) : 0);
              // iter-397q: the reject repeats byte-identically every run, so it
              // is structure, not noise. Dump the surrounding stream once —
              // window identity + 16 dwords before the header + 48 after — so
              // the true packet framing can be reconstructed offline.
              if (in == 0) {
                SR_LOG_BOOT(sr::LogSeverity::Warn,
                            "[PM4_REJECT] ctx depth=%d off=+0x%zX window=0x%zX",
                            depth, off, bytes);
                const size_t from =
                    off >= 16 * 4 ? off - 16 * 4 : size_t(0);
                char line[420];
                int p = 0;
                int col = 0;
                for (size_t o = from; o + 4 <= bytes && o < off + 48 * 4;
                     o += 4) {
                  p += snprintf(line + p, sizeof(line) - size_t(p), "%08X ",
                                readBE32(host + o));
                  if (++col == 16) {
                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                                "[PM4_REJECT] dw@+0x%zX: %s", o - 15 * 4, line);
                    p = 0; col = 0;
                  }
                }
                if (col)
                  SR_LOG_BOOT(sr::LogSeverity::Warn,
                              "[PM4_REJECT] dw@+0x%zX: %s",
                              off + 48 * 4 - size_t(col) * 4, line);
              }
              break;
            }
            execute(ibHost, static_cast<size_t>(ibSizeDwords) * 4, depth + 1);
          }
          break;
        }
        case 0x58: {  // EVENT_WRITE_SHD (fence: GPU writes value to memory)
          if (cnt >= 3) {
            uint32_t physAddr = readBE32(body + 4) & 0xFFFFFFFC;
            uint32_t value = readBE32(body + 8);
            uint8_t* dest = physToHost(user, physAddr);
            if (dest) {
              dest[0] = (value >> 24) & 0xFF; dest[1] = (value >> 16) & 0xFF;
              dest[2] = (value >> 8) & 0xFF;  dest[3] = value & 0xFF;
            }
          }
          break;
        }
        case 0x59: case 0x5E: {  // EVENT_WRITE_EXT — resolve trigger; latch.
          resolveEvents++;
          resolveBaseAtLastEvent = copyDestBase ? copyDestBase
                                                : lastNonZeroCopyDestBase;
          break;
        }
        case 0x3B:  // PM4_INVALIDATE_STATE — state-pointer invalidation hint;
          break;    // Xenia discards the mask too (:1374-1380).
        case 0x3C:  // PM4_WAIT_REG_MEM — CP wait (memory-poll or register-poll).
          // Instant-consumption model: memory-mode conditions were satisfied by
          // the CPU before submission; register-mode COHER handshake is internal
          // to the CP. Never spin here (the parsing thread IS the CPU thread).
          break;
        case 0x45: {  // PM4_COND_WRITE — conditional write to memory or register.
          // Xenia ExecutePacketType3_COND_WRITE (pm4_command_processor_implement.h:823).
          // Poll a reg/memory value, mask + compare to ref via the wait_info
          // function code; on match, write write_data to a reg or memory. Our
          // parser IS the CPU thread, so the polled value is already coherent
          // (same instant-consumption model as WAIT_REG_MEM above).
          if (cnt >= 6) {
            if (cpMode && sr::gfx::nativeExecPendingEffects()) {  // executor sync
              sr::native::exec::syncReason(3);
              cpFlushTimed(cp.flush, cp.user, 4);
            }
            uint32_t waitInfo  = readBE32(body);
            uint32_t pollAddr  = readBE32(body + 4);
            uint32_t ref       = readBE32(body + 8);
            uint32_t mask      = readBE32(body + 12);
            uint32_t writeAddr = readBE32(body + 16);
            uint32_t writeData = readBE32(body + 20);
            uint32_t value;
            if (waitInfo & 0x10) {  // poll memory
              const uint8_t* src = physToHost(user, pollAddr & ~3u);
              if (cpMode && src) sr::gfx::livetrace::memRead(pollAddr & ~3u, src, 4u);  // iter-460 live trace
              value = src ? gpuSwap32(rawLE32(src), pollAddr & 3) : 0;
            } else {  // poll register
              value = getXenosRegisters().read(pollAddr & 0xFFFF);
            }
            // wait_info low 3 bits select the compare fn (Xenia MatchValueAndRef).
            const uint32_t v = value & mask;
            const uint32_t truth =
                (uint32_t(v <  ref) << 1) | (uint32_t(v <= ref) << 2) |
                (uint32_t(v == ref) << 3) | (uint32_t(v != ref) << 4) |
                (uint32_t(v >= ref) << 5) | (uint32_t(v >  ref) << 6) | (1u << 7);
            if ((truth >> (waitInfo & 7)) & 1) {  // matched -> write
              if (waitInfo & 0x100) {  // write memory (raw-LE store of GpuSwap'd data)
                uint8_t* dst = physToHost(user, writeAddr & ~3u);
                if (dst) {
                  const uint32_t out = gpuSwap32(writeData, writeAddr & 3);
                  dst[0] = out & 0xFF;         dst[1] = (out >> 8) & 0xFF;
                  dst[2] = (out >> 16) & 0xFF; dst[3] = (out >> 24) & 0xFF;
                }
              } else {  // write register
                writeReg(writeAddr & 0xFFFF, writeData);
              }
            }
          }
          break;
        }
        case 0x46:  // PM4_EVENT_WRITE — event that writes to memory on completion (applyEventWrite).
          if (cnt >= 1) applyEventWrite(readBE32(body));
          break;
        case 0x54: {  // PM4_INTERRUPT — CP interrupt to the guest ISR
          // (VdSetGraphicsInterruptCallback, source=1). Counted; ISR dispatch is
          // filed — kernel-side fence advancement covers liveness today.
          cpInterrupts++;
          // iter-397q: timestamped — does the driver emit these DURING movie
          // playback (replay kicks) or only at boot? Decides whether the
          // source-1 delivery gap is arm-timing or emission absence.
          static std::atomic<uint32_t> s_cpiLog{0};
          const uint32_t cn = s_cpiLog.fetch_add(1, std::memory_order_relaxed);
          if (cn < 24 || (cn & (cn - 1)) == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[PM4_CPINT] #%u depth=%d off=+0x%zX body0=%08X",
                        cn + 1, depth, off, cnt >= 1 ? readBE32(body) : 0);
          break;
        }
        case 0x60:  // PM4_SET_BIN_MASK_LO
        case 0x61:  // PM4_SET_BIN_MASK_HI
        case 0x62:  // PM4_SET_BIN_SELECT_LO
        case 0x63:  // PM4_SET_BIN_SELECT_HI
          if (cnt >= 1) applyBinReg(op - 0x60u, readBE32(body));
          break;
        default:
          opcodeUnknown[op] = true;
          if (unknownRun == 0) firstUnknownOff = off;
          unknownRun++;
          break;
      }
      if (traced)
        sr::gfx::livetrace::packetEnd(draws != drawsBefore ? uint32_t(draws) : 0u, op == 0x64u);
      // A known opcode executed => we are back on packet boundaries.
      if (unknownRun == unknownBefore) {
        unknownRun = 0;
        lastGoodOp = op;
        lastGoodOff = off;
        if (op == 0x58) fenceEnd = off + pktSz;   // trailing-fence candidate
      }
      // iter-369h DESYNC BRAKE. A real command stream contains isolated unknown
      // opcodes at worst — the entire golden trace has exactly ONE
      // (unknownOps=1, opcode 0x64). A RUN of them means we are no longer on
      // packet boundaries: the live scan was reaching 254 DISTINCT opcodes out
      // of 256 possible (a real stream uses ~20-30), i.e. it had wandered into
      // arbitrary guest memory — proven in 369d, where it decoded string tables
      // and PE headers as shader ucode.
      // Stop the window at that point instead of executing noise. Bounded so a
      // single unknown opcode (0x64) never truncates a legitimate frame.
      if (unknownRun >= kUnknownRunLimit) {
        desyncStops++;
        blameLastOp();
        static std::atomic<uint32_t> s_dsLog{0};
        const uint32_t dn = s_dsLog.fetch_add(1, std::memory_order_relaxed);
        if (dn < 6 || (dn & (dn - 1)) == 0) {
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[PM4_DESYNC] %u consecutive unknown opcodes at +0x%zX of "
                      "0x%zX — abandoning the rest of this window (total=%llu)",
                      unknownRun, off, bytes, (unsigned long long)desyncStops);
          // iter-397q: name the packet that broke framing. lastGoodOp = the
          // last opcode that parsed; the mis-sized culprit is AT or just after
          // its end. Dump 24 dwords straight after the last good packet.
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[PM4_DESYNC] lastGoodOp=0x%02X lastGoodOff=+0x%zX firstUnkOff=+0x%zX",
                      lastGoodOp, lastGoodOff, firstUnknownOff);
          const size_t dumpAt = lastGoodOff & ~size_t(3);
          char line[400];
          int p = 0;
          for (int i = 0; i < 24 && dumpAt + size_t(i) * 4 + 4 <= bytes; i++)
            p += snprintf(line + p, sizeof(line) - size_t(p), "%08X ",
                          readBE32(host + dumpAt + size_t(i) * 4));
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[PM4_DESYNC] dwords@+0x%zX: %s", dumpAt, line);
        }
        break;
      }
      off += pktSz;
    } else if (type == 0) {
      uint32_t startReg = hdr & 0xFFFF;
      uint32_t cnt0 = ((hdr >> 16) & 0x3FFF) + 1;
      size_t pktSz = (1 + static_cast<size_t>(cnt0)) * 4;
      if (off + pktSz > bytes) {
        if (cov) pm4cov::packetOther(host + off, depth, pm4cov::kTruncated);
        break;
      }
      // Xenia ExecutePacketType0: base index is 15 bits; bit 15 = write every
      // value to the SAME register. cpMode only (the default path keeps its
      // historical decode bit-identical).
      const bool oneReg = cpMode && ((hdr >> 15) & 1u);
      if (cpMode) startReg &= 0x7FFFu;
      if (cov && pm4cov::packet0(host + off, depth, startReg, cnt0, oneReg)) {
        off += pktSz;  // NATIVE step 3: a state run the native projection carries
        continue;
      }
      const bool traced0 = cpMode && sr::gfx::livetrace::armed();
      if (traced0) sr::gfx::livetrace::packetBegin(host + off, uint32_t(1u + cnt0));
      // NATIVE strand A (SR_NATIVE_CONST_DEDUP_CHECK): a group the dedup would have skipped must
      // already be in the register file when its packet executes (replays included).
      if (sr::native::cdedup::checkOn() && sr::native::cdedup::cpTakePrediction())
        sr::native::cdedup::cpCheck(host + off + 4, startReg, cnt0, getXenosRegisters().mutableData());
      // iter-465 perf lane: a run with no side-effect register is a bulk store (writeReg was
      // ~12% of the command processor in gameplay, most of it type-0 runs).
      if (!oneReg && cnt0 > 1 && bulkType0Enabled() && !regRangeHasEffects(startReg, cnt0)) {
        auto& xr = getXenosRegisters();
        // iter-466 cp lane (SR_CP_BULK_SSE): the run as one byte-swapping copy (pshufb) into
        // the register file - the same stores; registers past the file stay dropped. The
        // per-dword loop + readBE32 + write was ~11.5% of the CP (archive/v1_hfr/b4_g).
        static const bool s_bulkSse = cpsimd::gateLogged("SR_CP_BULK_SSE", true) && cpsimd::available();
        if (s_bulkSse) {
          const uint32_t inFile = startReg < XenosRegisterFile::kNumRegisters
                                      ? std::min<uint32_t>(cnt0, XenosRegisterFile::kNumRegisters - startReg)
                                      : 0u;
          if (inFile) {
            cpsimd::bswapCopy32(xr.mutableData() + startReg, host + off + 4, inFile);
            xr.noteWritten(startReg, inFile);
          }
        } else {
          for (uint32_t i = 0; i < cnt0; i++) xr.write(startReg + i, readBE32(host + off + 4 + i * 4));
        }
        regWrites += cnt0;
      } else {
        for (uint32_t i = 0; i < cnt0; i++)
          writeReg(oneReg ? startReg : startReg + i, readBE32(host + off + 4 + i * 4));
      }
      if (traced0) sr::gfx::livetrace::packetEnd(0u, false);
      off += pktSz;
    } else if (type == 1 && cpMode) {
      // Xenia ExecutePacketType1: two registers of data (3 dwords).
      if (off + 12 > bytes) {
        if (cov) pm4cov::packetOther(host + off, depth, pm4cov::kTruncated);
        break;
      }
      if (cov) pm4cov::packetOther(host + off, depth, pm4cov::kType1);
      const bool traced1 = sr::gfx::livetrace::armed();
      if (traced1) sr::gfx::livetrace::packetBegin(host + off, 3u);
      writeReg(hdr & 0x7FFu, readBE32(host + off + 4));
      writeReg((hdr >> 11) & 0x7FFu, readBE32(host + off + 8));
      if (traced1) sr::gfx::livetrace::packetEnd(0u, false);
      off += 12;
    } else {
      // the next dword of this window: the producer census's witness for 821075E0's alignment pad (pm4_coverage.hpp)
      if (cov) pm4cov::packetOther(host + off, depth, pm4cov::kType2, off + 8 <= bytes ? readBE32(host + off + 4) : 0u);
      off += 4;  // type 1/2: skip
    }
  }
  packets++;
}

// ---------------------------------------------------------------------------
// P2 async command processor: packet semantics that only make sense when this
// engine IS the console CP on its own thread. Every case follows the Xenia
// oracle (reference/xenia-canary/src/xenia/gpu/pm4_command_processor_implement.h,
// function named per case).
// ---------------------------------------------------------------------------

#if SR_ORACLE  // the SR_NATIVE_EXEC executor's store performer (dev-only since stage 0; docs/RELEASE_COMPOSITION.md 2.2)
// Memory store with the Xenos endian selector in the address's low 2 bits:
// Xenia `xe::store(TranslatePhysical(addr & ~3), GpuSwap(value, addr & 3))`.
// NATIVE strand A executor (kernel/native_exec.hpp): a store item, on the executor thread, after every
// item ahead of it - the same stores the synchronous handlers make (cpPacket 0x58 / 0x3D / 0x3E,
// writeReg's scratch write-back), from the values captured at their stream positions.
void Pm4Engine::execPerformStores(const GuestDrawCommand& c) {
  namespace ex = sr::native::exec;
  for (uint32_t i = 0; i < c.effectN && i < ex::kStoresPerItem; i++) {
    const uint32_t* q = c.drawParams + 4u * i;
    const uint32_t op = q[0], a = q[1], v = q[2], aux = q[3];
    if (op == ex::kOpEvShd) {
      // p2.zpd: the covering-fence check at the store's stream position (the executor drained the markers ahead).
      if (cp.zpdFence && sr::gfx::zpd::fenceNeedsFlush()) cp.zpdFence(cp.user);
      const uint32_t endian = a & 3u, address = a & ~3u;
      uint8_t* dst = physToHost(user, address);
      if (aux && cp.virtToHost) dst = cp.virtToHost(cp.user, aux);
      if (dst) {
        const uint32_t w = gpuSwap32(v, endian);
        dst[0] = uint8_t(w); dst[1] = uint8_t(w >> 8); dst[2] = uint8_t(w >> 16); dst[3] = uint8_t(w >> 24);
        cpMemWrites++;
      }
      if (sr::kernel::gpuHangWdIsFence(address)) sr::kernel::gpuHangWdNoteFenceWrite(0x58u, address, v, dst);
    } else if (op == ex::kOpMemWrite || op == ex::kOpRegToMem) {
      cpStoreMem(a, v);
      if (sr::kernel::gpuHangWdIsFence(a)) sr::kernel::gpuHangWdNoteFenceWrite(op, a, v, physToHost(user, a & ~3u));
    } else if (op >= ex::kOpScratch && op < ex::kOpScratch + 8u) {
      const bool reg1 = op == ex::kOpScratch + 1u;  // the vblank gate's pc+4 (writeReg's bracket, native/dreg-hang)
      if (reg1) sr::native::vblank::noteScratch1Begin(a, v);
      if (uint8_t* p = physToHost(user, a)) {
        p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
        cpScratchWrites++;
      }
      if (reg1) sr::native::vblank::noteScratch1End();
    }
  }
}
#endif  // SR_ORACLE

// iter-467 p2 divergence (3): Xenia's fake occlusion / extent query write-backs (cpPacket 0x5A / 0x5B).
bool Pm4Engine::cpZpdFakeOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CP_ZPD_FAKE");
    return e && e[0] == '1';
  }();
  return v;
}

void Pm4Engine::cpStoreMem(uint32_t addrWithEndian, uint32_t value) {
  uint8_t* p = physToHost(user, addrWithEndian & ~3u);
  if (!p) return;
  const uint32_t v = gpuSwap32(value, addrWithEndian & 3u);
  p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
  cpMemWrites++;
}

bool Pm4Engine::cpPacket(uint32_t op, const uint8_t* body, uint32_t cnt) {
  // NATIVE strand A drain census (SR_NATIVE_DRAIN_CENSUS, observe-only): the guest-visible effects.
  if (sr::native::dcensus::on() && (op == 0x58u || op == 0x3Du || op == 0x3Eu || op == 0x54u || op == 0x64u))
    sr::native::dcensus::noteEffect(op);
  switch (op) {
    case 0x10:  // PM4_NOP (ExecutePacketType3_NOP)
    case 0x48:  // PM4_ME_INIT (ExecutePacketType3_ME_INIT: micro-engine image)
      return true;
    case 0x3C:  // PM4_WAIT_REG_MEM
      if (cnt >= 5) cpWaitRegMem(body);
      return true;
    case 0x54: {  // PM4_INTERRUPT: guest ISR, source 1, per set cpu bit
      if (cnt >= 1) applyInterrupt(readBE32(body));
      return true;
    }
    case 0x64: {  // PM4_XE_SWAP (VdSwap's reserved block): {sig, fb, w, h}
      if (cnt >= 4 && readBE32(body) == 0x53574150u /* 'SWAP' */) {
        applySwap(readBE32(body + 4), readBE32(body + 8), readBE32(body + 12));
      } else {
        cpSwaps++;
        // NATIVE N0.2: this XE_SWAP ended its census frame at the packet and applies no present
        if (pm4cov::on()) pm4cov::swapWithoutPresent();
        if (sr::gfx::ledger::on()) sr::gfx::ledger::onSwap(cpSwaps);
      }
      return true;
    }
    case 0x58: {  // PM4_EVENT_WRITE_SHD: VS|PS done -> memory write
      if (cnt < 3) return true;
      applyEventWriteShd(readBE32(body), readBE32(body + 4), readBE32(body + 8));
      return true;
    }
    case 0x3D: {  // PM4_MEM_WRITE: addr, then cnt-1 dwords
      if (cnt < 1) return true;
      if (sr::native::exec::deferring() && !sr::native::exec::frontend()) {
        uint32_t q[4 * sr::native::exec::kStoresPerItem];
        uint32_t nq = 0, a = readBE32(body);
        for (uint32_t i = 1; i < cnt; i++, a += 4) {
          q[4 * nq + 0] = sr::native::exec::kOpMemWrite;
          q[4 * nq + 1] = a;
          q[4 * nq + 2] = readBE32(body + i * 4);
          q[4 * nq + 3] = 0u;
          if (++nq == sr::native::exec::kStoresPerItem) {
            sr::gfx::nativeExecPushStores(q, nq);
            nq = 0;
          }
        }
        if (nq) sr::gfx::nativeExecPushStores(q, nq);
        return true;
      }
      if (!sr::native::exec::frontend()) cpFlushTimed(cp.flush, cp.user, 4);  // FRONTEND: at parse
      uint32_t addr = readBE32(body);
      for (uint32_t i = 1; i < cnt; i++, addr += 4) {
        cpStoreMem(addr, readBE32(body + i * 4));
        if (sr::kernel::gpuHangWdIsFence(addr))   // iter-463 fence latch
          sr::kernel::gpuHangWdNoteFenceWrite(0x3Du, addr, readBE32(body + i * 4),
                                              physToHost(user, addr & ~3u));
      }
      return true;
    }
    case 0x3E: {  // PM4_REG_TO_MEM: register -> memory
      if (cnt < 2) return true;
      if (sr::native::exec::deferring()) {  // the register's value at its stream position
        const uint32_t q[4] = {sr::native::exec::kOpRegToMem, readBE32(body + 4),
                               getXenosRegisters().read(readBE32(body) & 0xFFFFu), 0u};
        sr::gfx::nativeExecPushStores(q, 1);
        return true;
      }
      cpFlushTimed(cp.flush, cp.user, 5);
      const uint32_t addr = readBE32(body + 4);
      const uint32_t value = getXenosRegisters().read(readBE32(body) & 0xFFFFu);
      cpStoreMem(addr, value);
      if (sr::kernel::gpuHangWdIsFence(addr))   // iter-463 fence latch
        sr::kernel::gpuHangWdNoteFenceWrite(0x3Eu, addr, value, physToHost(user, addr & ~3u));
      return true;
    }
    case 0x55: {  // PM4_SET_CONSTANT2: offset_type, then registers
      if (cnt < 1) return true;
      const uint32_t offsetType = readBE32(body);
      const uint32_t index = offsetType & 0xFFFFu;
      for (uint32_t i = 1; i < cnt; i++) writeReg(index + (i - 1), readBE32(body + i * 4));
      return true;
    }
    case 0x56: {  // PM4_SET_SHADER_CONSTANTS: offset_type, then registers
      if (cnt < 1) return true;
      const uint32_t index = readBE32(body) & 0xFFFFu;
      for (uint32_t i = 1; i < cnt; i++) writeReg(index + (i - 1), readBE32(body + i * 4));
      return true;
    }
    case 0x23: {  // PM4_VIZ_QUERY: end -> report "visible" (Xenia fakes it too)
      if (cnt < 1) return true;
      cpVizQueries++;
      const uint32_t d0 = readBE32(body);
      const uint32_t id = d0 & 0x3Fu;
      if (!(d0 & 0x100u)) {
        writeReg(0x21F9u, 7u);  // VIZQUERY_START
      } else {
        writeReg(0x21F9u, 8u);  // VIZQUERY_END
        const uint32_t reg = id < 32 ? 0x0C44u : 0x0C45u;
        getXenosRegisters().write(reg, getXenosRegisters().read(reg) | (1u << (id & 31u)));
      }
      return true;
    }
    case 0x5B: {  // PM4_EVENT_WRITE_ZPD: occlusion sample-count report
      // The initiator write-back is exact. iter-467 p2 divergence (3), SR_CP_ZPD_FAKE=1 (default OFF until
      // proven): Xenia's conventional fake fallback (pm4_command_processor_implement.h:957-1047, the
      // occlusion_query=fake path; cvars occlusion_query_fake_lower/upper_threshold = 80 / 100,
      // gpu_flags.cc:62-73; XenosZPDReport, xenos_zpd_report.h). We have no host occlusion queries (Xenia's
      // default 'fast' mode), so this is Xenia's fallback for exactly that case: the record at
      // RB_SAMPLE_COUNT_ADDR (reg 0x2325) & ~0x1F, if it still holds the D3D pending sentinel 0xFFFFFEED in
      // ZPass_A or ZFail_A (either byte order), gets a fake sample count (100, 99 .. 81, then 100 again) as
      // Total_A = ZPass_A = n and the other six counters 0, little-endian (xenos.h:1570
      // xe_gpu_depth_sample_counts). The title's QueryIssue 82102BC8 presets only the END record's first
      // four dwords, and QueryGetData 82102DC8 returns S_FALSE while they still hold the sentinel and
      // otherwise END.ZPass - BEGIN.ZPass summed over tiles.
      // p2.zpd (SR_NATIVE_ZPD, baked default ON; docs/NATIVE_ROADMAP.md P2.12): a host D3D12 occlusion query answers
      // it (the live CP that submits draws only; the fake below stays the interim for every other case) - the record
      // engine's applyZpdEvent, the one implementation the kRkZpd record reaches too.
      if (cnt >= 1 ? applyZpdEvent(readBE32(body)) : (zpd::noteSource(false), cpZpdEvents++, zpdNative())) return true;
      if (cpZpdFakeOn()) {
        const uint32_t rec = getXenosRegisters().read(0x2325u) & ~0x1Fu;
        const uint8_t* r = rec ? physToHost(user, rec) : nullptr;
        auto le32 = [](const uint8_t* p) {
          return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        };
        const bool pending = r && (le32(r + 16) == 0xEDFEFFFFu || le32(r + 16) == 0xFFFFFEEDu ||
                                   le32(r + 8) == 0xEDFEFFFFu || le32(r + 8) == 0xFFFFFEEDu);
        if (pending) {
          cpZpdFakeCount = cpZpdFakeCount <= 80u ? 100u : cpZpdFakeCount - 1u;
          // {Total_A, Total_B, ZFail_A, ZFail_B, ZPass_A, ZPass_B, StencilFail_A, StencilFail_B}; endian 0 = LE
          const uint32_t v[8] = {cpZpdFakeCount, 0u, 0u, 0u, cpZpdFakeCount, 0u, 0u, 0u};
          if (sr::native::exec::deferring() && !sr::native::exec::frontend()) {
            uint32_t q[4 * 8];
            for (uint32_t i = 0; i < 8; i++) {
              q[4 * i + 0] = sr::native::exec::kOpMemWrite;
              q[4 * i + 1] = rec + 4u * i;
              q[4 * i + 2] = v[i];
              q[4 * i + 3] = 0u;
            }
            for (uint32_t i = 0; i < 8; i += sr::native::exec::kStoresPerItem)
              sr::gfx::nativeExecPushStores(q + 4 * i, (8 - i) < sr::native::exec::kStoresPerItem
                                                           ? (8 - i) : sr::native::exec::kStoresPerItem);
          } else {
            if (!sr::native::exec::frontend()) cpFlushTimed(cp.flush, cp.user, 4);
            for (uint32_t i = 0; i < 8; i++) cpStoreMem(rec + 4u * i, v[i]);
          }
          cpZpdFakeWrites++;
        }
      }
      return true;
    }
    case 0x5A: {  // PM4_EVENT_WRITE_EXT: screen extent report {initiator, addr|endian}
      // iter-467 p2 divergence (3), SR_CP_ZPD_FAKE=1: Xenia ExecutePacketType3_EVENT_WRITE_EXT
      // (pm4_command_processor_implement.h:921-955) writes the initiator back and fakes the extent: six
      // big-endian u16 {min x 0, max x 8192 >> 3, min y 0, max y 8192 >> 3, min z 0, max z 1} at addr & ~3
      // (endian k8in16). The title's extent query (QueryIssue 82102BC8 type 10: 0xC0015A00 {26, phys|1})
      // presets the slot's first dword to 0xFFFFFEED; QueryGetData returns S_FALSE until it changes.
      if (cnt < 2) return true;
      writeReg(0x21F9u, readBE32(body) & 0x3Fu);
      cpExtEvents++;
      if (!cpZpdFakeOn()) return true;
      const uint32_t addr = readBE32(body + 4) & ~3u;
      // three dwords of two u16 each, stored big-endian (endian 2 = k8in32 swaps the whole dword)
      const uint32_t v[3] = {(0u << 16) | (8192u >> 3), (0u << 16) | (8192u >> 3), (0u << 16) | 1u};
      if (sr::native::exec::deferring() && !sr::native::exec::frontend()) {
        uint32_t q[4 * 3];
        for (uint32_t i = 0; i < 3; i++) {
          q[4 * i + 0] = sr::native::exec::kOpMemWrite;
          q[4 * i + 1] = (addr + 4u * i) | 2u;
          q[4 * i + 2] = v[i];
          q[4 * i + 3] = 0u;
        }
        sr::gfx::nativeExecPushStores(q, 3);
      } else {
        if (!sr::native::exec::frontend()) cpFlushTimed(cp.flush, cp.user, 4);
        for (uint32_t i = 0; i < 3; i++) cpStoreMem((addr + 4u * i) | 2u, v[i]);
      }
      return true;
    }
    default:
      return false;
  }
}

}  // namespace gfx
}  // namespace sr
