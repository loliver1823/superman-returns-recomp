#pragma once

// Retained native diagnostics in BOTH configurations: atomics and reports only, no hardware model.
// iter-467 NATIVE METER (docs/NATIVE_ROADMAP.md "native meter"): the number we show the owner
// instead of claims. "Native" (CLAUDE.md, 100% NATIVE) means 0 PM4 packets interpreted, 0 EDRAM
// emulation and 0 XMA register emulation per frame in the gameplay window; this meter counts
// exactly those, always on (relaxed atomics, one add per PM4 execute window, a few loads per
// guest frame).
//
// Counters (totals since process start):
//   pm4         PM4 packets the command processor interpreted: every header the Pm4Engine
//               execute() loop decodes (type 0/1/2/3, chain tails; zero padding dwords excluded)
//               at every IB depth, plus the ring-level INDIRECT_BUFFER descriptors the CP thread
//               dispatches itself. Fed by gfx/pm4_engine.cpp + kernel/gpu_command_processor.cpp.
//   edram_res   resolves executed through the EDRAM model (not a native resolve).      strand B
//   edram_xfer  surface moves executed through the EDRAM word model (the EDRAM buffer's    strand B
//               store + load, or the CPU mirror), whatever decided them - the oracle unit's
//               work (release/emu-stage3; a move the direct passes take is not counted).
//   edram_load  EDRAM loads (EDRAM contents restored into a host render target).       strand B
//   nrec        native draw records the CP applied (kernel/native_flush) - native work, not
//               emulation; shown next to pm4, never part of the 'all zeros' verdict.
//   xma         XMA register accesses = AudioNativeCensus guest_mmio_fns + xma_regfile_writes
//               (kernel/audio_native_census.hpp, pulled; present once the audio B6 merge lands).
//   dispreg     display-register answers the host still gives by EMULATION (RELEASE_PLAN C1 lists
//               them beside PM4 / EDRAM / XMA; ledger `display.registers`): every scan-out register
//               write that answers the guest's vblank-gate reads (D1MODE_V_COUNTER + VIEWPORT_SIZE
//               at 0x7FC86530 / 0x7FC86584, two per vblank tick, SR_VD_SCANOUT_REGS), the system
//               command buffer descriptor fill (VdGetSystemCommandBuffer, SR_VD_SYSCMDBUF; one per
//               call), and the kernel vsync-enable MMIO bit 0x7FC86544 (once, at the ISR
//               registration). Kernel exports that a native port implements anyway (VdQueryVideoMode,
//               VdGetCurrentDisplayInformation, VdQueryVideoFlags, the gamma ramp) are NOT counted:
//               a kernel export answered by host code IS the native implementation (the UnleashedRecomp
//               standard CLAUDE.md names), not hardware-register emulation; only answers that stand in
//               for a register the console's display controller would have held are emulation. THE
//               LINE WAS DRAWN BEFORE ANY NUMBER WAS TAKEN (2026-10-09, confirmed by the orchestrator
//               with the ledger's `display.registers` record and NATIVE_ROADMAP "native meter"): it is
//               never moved to reach zero. Fed by kernel/stubs.cpp; part of the 'all zeros' verdict.
//               The native P2-B GATE item retires the scan-out answers.
//   shm         the shared-memory page model's work (ledger `mem.resources`, docs/RELEASE_COMPOSITION.md section 7;
//               release/mem-meter, 2026-10-15, the rule made one after Astra round 1). ONE RULE: a count per PAGE per
//               DECISION EVENT in which the model decides whether its copy of a 4 KiB guest page is current -
//               (a) a VALIDATION before a draw's vertex / index / texture fetch or a scan-out: decided by a hash
//                   (pm4ShmValidate: XXH3 over the page, an upload when it changed), by the write watch's clean state
//                   (pm4ShmRequest, SR_SHM_WRITE_TRACK), or by the range cache's proof that the range stayed clean since
//                   its last validation (SR_NB_SHM_RANGE_CACHE: one per page of the range);
//               (b) a RE-BASELINE after a GPU write: decided by a hash (pm4ShmRangeWrittenByGpu locked or unlocked,
//                   the pre-hash of pm4ShmResolve, the hash of the bytes a readback wrote back into guest memory) or
//                   by the write watch's kept generation (SR_SHM_WT_REBASE's keep).
//               A page hashed at a validation and re-baselined after a resolve in the same frame counts twice: two
//               decisions, two pieces of work the resource registry of mem.resources replaces. NOT counted: a same-epoch
//               dedup hit (the range memo, SR_CP_SHM_LEAN, the epoch stamps: the question was answered in this epoch),
//               a texture-prepass page the resolve mask covers (decided at the bind), the oracles' re-asking
//               (SR_SHM_WT_CHECK and SR_NB_SHM_RANGE_CHECK hashes, the live-trace recorder's hashes, the resolve-hash
//               debug), and hashes that are not page decisions (fetch-constant keys, the zero page, shader text). THE
//               RULE WAS DRAWN BEFORE ANY NUMBER WAS TAKEN and is never moved to reach zero; 0 = the GPU's memory reads
//               reached no page-model decision that frame, which is what mem.resources' registry must achieve (the
//               write-back of resolved bytes into guest memory stays - the guest reads it - but no hash of them does).
//               Fed by gfx/d3d12_pm4_shmem.cpp at every site tools/perf/test_shm_meter_wiring.py enumerates; part of
//               the 'all zeros' verdict.
// An id nobody has wired prints "-" (never a fake 0): a feeder calls wire(id) once at init.
//
// Log lines (tools/perf/native_meter.py sums the gameplay window from them):
//   [NATIVE_METER] w=<n> <secs>s frames=<f> | per_frame avg/max pm4=a/m ... | cum pm4=... ...
//       every 2 s of guest frames when SR_HIGHFPS_PACE=1 or SR_NATIVE_METER=1 (the pace window);
//   [NATIVE_METER_RUN] <why> frames=<f> pm4=<total> ... per_frame pm4=<avg> ...
//       every 30 s and at exit (the whole-run line). SR_NATIVE_METER=0 turns the meter off.

#include <atomic>
#include <cstdint>

namespace sr::native::meter {

enum Id : uint32_t { kPm4 = 0, kEdramResolve, kEdramTransfer, kEdramLoad, kNativeRec, kDisplayReg, kShm, kIds };

extern std::atomic<uint64_t> g_count[kIds];

// Count n events of `id` (any thread). Cheap enough for per-packet-window use.
inline void add(Id id, uint64_t n = 1) noexcept {
  g_count[id].fetch_add(n, std::memory_order_relaxed);
}

// A feeder declares it counts `id` (once, any thread); unwired ids print "-".
void wire(Id id) noexcept;

// The columns as the log lines print them (the pushed ids in Id order, then the pulled xma):
// count, name, wired, and a snapshot of every column's total. For tools and the difftest
// (tools/difftest/native_meter_test.cpp): a column that exists here but not in a parser is a
// column the meter's verdict silently lacks.
int columnCount() noexcept;
const char* columnName(int c) noexcept;
bool columnWired(int c) noexcept;
void snapshotColumns(uint64_t* out, int n) noexcept;

// What the remaining pm4 packets are ("what is still being emulated", category by category as each
// falls to zero): every header the CP decodes (the pm4 column) by kind. Draws by VGT source select
// (DMA = indexed, auto = auto-index, imm = immediate data in the packet).
enum Kind : uint32_t {
  kT0,        // type-0 register runs (setters, non-native flushes)
  kT1,        // type-1
  kT2Nop,     // type-2 NOP padding
  kDrawDma,   // DRAW_INDX / DRAW_INDX_2, indexed (non-native: Z-pass, >0xFFFF, guest-body draws)
  kDrawAuto,  // auto-indexed draws (UP / Begin-End / non-native)
  kDrawImm,   // immediate-data draws
  kShader,    // IM_LOAD / IM_LOAD_IMMEDIATE (shader bind)
  kConst,     // SET_CONSTANT / SET_CONSTANT2 / LOAD_ALU_CONSTANT / SET_SHADER_CONSTANTS
  kIb,        // INDIRECT_BUFFER(_PFD): kicks / segment chain
  kSync,      // EVENT_WRITE_SHD / MEM_WRITE / REG_TO_MEM / INTERRUPT / WAIT_REG_MEM / COND_WRITE / XE_SWAP
  kEvent,     // EVENT_WRITE / EVENT_WRITE_EXT / ZPD: resolve and EDRAM events
  kBin,       // bin mask / select (predicated tiling)
  kNop3,      // type-3 NOP (not a native record marker)
  kOther,
  kKinds
};
extern std::atomic<uint64_t> g_kind[kKinds];
inline void addKind(uint32_t k, uint64_t n) noexcept {
  if (n) g_kind[k].fetch_add(n, std::memory_order_relaxed);
}
// The kind of a type-3 packet (op, first two body dwords; b1 only read for DRAW_INDX).
inline uint32_t kindOf3(uint32_t op, uint32_t b0, uint32_t b1) noexcept {
  switch (op) {
    case 0x22: return ((b1 >> 6) & 3u) == 0 ? kDrawDma : ((b1 >> 6) & 3u) == 1 ? kDrawImm : kDrawAuto;
    case 0x36: return ((b0 >> 6) & 3u) == 0 ? kDrawDma : ((b0 >> 6) & 3u) == 1 ? kDrawImm : kDrawAuto;
    case 0x27: case 0x2B: return kShader;
    case 0x2D: case 0x2E: case 0x2F: case 0x55: case 0x56: return kConst;
    case 0x3F: case 0x37: return kIb;
    case 0x58: case 0x3D: case 0x3E: case 0x54: case 0x3C: case 0x45: case 0x64: return kSync;
    case 0x46: case 0x59: case 0x5B: case 0x5E: return kEvent;
    case 0x60: case 0x61: case 0x62: case 0x63: return kBin;
    case 0x10: return kNop3;
    default: return kOther;
  }
}

// The guest presented a frame (VdSwap). Drives the per-frame max and the 2 s window line.
void onFrame() noexcept;

// The whole-run line now (also every 30 s and at exit).
void report(const char* why) noexcept;

}  // namespace sr::native::meter
