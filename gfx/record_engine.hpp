#pragma once

// The record engine (C1, docs/RELEASE_COMPOSITION.md 2.1 and 3.2 stage 2a): the shipping half of the Xenos command
// processor. It owns the DEVICE-STATE SHADOW - XenosRegisterFile, the register-indexed store the XDK's setters write and
// the draw reads (the native record format's state runs are register-numbered: a register-indexed store is the record
// ABI, not PM4) - and applies what a native record means: a register run with the side effects of its registers (the CP
// scratch write-back 0x578..0x57F, the DC_LUT gamma ramp, the COHER_BASE_HOST accounting, the copy-destination latches),
// a shader bind or constant load, the bin predication, an event, a fence store, a coherency block, an interrupt, a swap,
// and a draw (emitDraw -> gfx/native_draw_builder -> the DrawCommandQueue of gfx/xenos_state.hpp). It is reached through
// the NativeRenderContext of gfx/record_context.cpp (nativeRecord). It decodes no packet header.
//
// gfx/pm4_engine.hpp derives Pm4Engine from it: the PM4 packet walker (execute, cpPacket, the WAIT_REG_MEM poll on a
// packet, the legacy window scan, the parse-health counters) - the development oracle. C1 stage 1
// (docs/RELEASE_COMPOSITION.md 9.9): the walker is out of the release link (CMakeLists.txt EMULATION_SOURCES); the
// release configuration's live instance is this class (gfx/null_renderer.cpp), the development one a Pm4Engine. The
// members below are the ones the record path uses, the walker's own stay in the derived class. The register / memory
// handshake (cpWaitRegMem) is the record path's own: the present gate's, the callback block's and strand B's op
// lists' waits are records and really wait (dcf2's default-build gate roll: 62 blocked waits).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "kernel/sr_oracle.hpp"
#include "native_record_executor.hpp"
#include "render_draw_command.hpp"
#include "xenos_ucode.hpp"

namespace sr {
namespace gfx {

struct GuestDrawCommand;  // render_draw_command.hpp

// Xenos GPU register shadow — the device state every register write (a record's state run, a PM4 type-0 packet in the
// development oracle) lands in. Registers 0x0000-0x7FFF cover config, ALU constants, fetch constants, etc.
// Fetch constants start at 0x4800.
class XenosRegisterFile {
public:
  static constexpr uint32_t kNumRegisters = 0x6000;

  // Fetch constant ranges (Xenia register_table.inc).
  static constexpr uint32_t kFetchConstBase = 0x4800;
  static constexpr uint32_t kFetchConstCount = 32 * 6;  // 32 groups × 6 dwords

  // ALU constant range.
  static constexpr uint32_t kAluConstBase = 0x4000;
  static constexpr uint32_t kAluConstCount = 0x800;     // 256 float4 constants = 1024 dwords

  void write(uint32_t reg, uint32_t value) {
    if (reg < kNumRegisters) regs_[reg] = value;
    if (reg - kAluConstBase < kAluConstCount) aluDirty_ |= 1u << ((reg - kAluConstBase) >> 6);
  }
  // iter-466 cp round 2 (SR_CP_CONST_TRACK): one bit per 64-dword (16 float4) block of the
  // ALU constants (bits 0..15 VS 0x4000.., 16..31 PS 0x4400..), set by every write since the
  // last takeAluDirty() - Xenia's cbuffer up_to_date invalidation in WriteRegister. A caller
  // that stores through mutableData() reports the range with noteWritten().
  void noteWritten(uint32_t startReg, uint32_t count) {
    if (!count) return;
    const uint64_t lo = std::max<uint64_t>(startReg, kAluConstBase);
    const uint64_t hi = std::min<uint64_t>(uint64_t(startReg) + count, kAluConstBase + kAluConstCount);
    if (lo >= hi) return;
    const uint32_t b0 = uint32_t(lo - kAluConstBase) >> 6, b1 = uint32_t(hi - 1u - kAluConstBase) >> 6;
    aluDirty_ |= (b1 >= 31u ? ~0u : ((1u << (b1 + 1u)) - 1u)) & ~((1u << b0) - 1u);
  }
  uint32_t takeAluDirty() {
    const uint32_t d = aluDirty_;
    aluDirty_ = 0;
    return d;
  }
  // iter-461: the whole file for bulk snapshots (a draw copies 2,248 dwords).
  const uint32_t* data() const { return &regs_[0]; }
  // iter-466 cp lane: the writable file for a bulk big-endian run (the record engine's register runs; the
  // caller clamps the count to kNumRegisters).
  uint32_t* mutableData() { return &regs_[0]; }

  uint32_t read(uint32_t reg) const {
    return (reg < kNumRegisters) ? regs_[reg] : 0;
  }

  void writeRange(uint32_t startReg, const uint32_t* data, uint32_t count) {
    for (uint32_t i = 0; i < count && (startReg + i) < kNumRegisters; i++)
      regs_[startReg + i] = data[i];
    noteWritten(startReg, count);
  }

  bool isFetchConst(uint32_t reg) const {
    return reg >= kFetchConstBase && reg < kFetchConstBase + kFetchConstCount;
  }

  // Xenos vertex fetch constant: 2 dwords per fetch slot.
  //   dword_0: type(2 bits) | address(30 bits) — address in dwords
  //   dword_1: endian(2 bits) | size(24 bits) — size in words
  using VertexFetch = NativeVertexFetch;

  VertexFetch getVertexFetch(uint32_t slotIndex) const {
    uint32_t base = kFetchConstBase + slotIndex * 2;
    if (base + 1 >= kNumRegisters) return {};
    uint32_t d0 = regs_[base];
    uint32_t d1 = regs_[base + 1];
    VertexFetch vf{};
    vf.type = d0 & 0x3;
    vf.addressDwords = (d0 >> 2) & 0x3FFFFFFF;
    vf.endian = d1 & 0x3;
    vf.sizeWords = (d1 >> 2) & 0x00FFFFFF;
    return vf;
  }

  // ── iter-456p: the TEXTURE fetch constant ───────────────────────────────
  // The oracle-first pass (director ruling 2026-08-31) needs, per draw, "the raw
  // fetch descriptor, address, format, dimensions, pitch, tiling/endian, and
  // level" for every sampled texture. The fetch-constant space is shared: one
  // 32-group array where a group holds EITHER three 2-dword vertex fetches OR
  // one 6-dword texture fetch. So texture slot N starts at group N.
  //
  // ⚠ RAW ONLY. Decoding belongs to kernel/tex_layout.hpp's
  // decodeFetchConstant(), which is the one canonical decoder in this tree and
  // already documents every bit field. Re-deriving the layout here would create
  // a second decoder to keep in sync, which is exactly how a "wrong texture is
  // bound" symptom gets a second, silent suspect.
  void getTextureFetchRaw(uint32_t slotIndex, uint32_t out[6]) const {
    const uint32_t base = kFetchConstBase + slotIndex * 6;
    for (uint32_t i = 0; i < 6; i++)
      out[i] = (base + i < kNumRegisters) ? regs_[base + i] : 0u;
  }

  // A texture fetch constant announces itself with type == 2 in dword 0; the
  // same two bits read 3 for a vertex fetch and 0 for an empty slot.
  bool textureFetchPresent(uint32_t slotIndex) const {
    uint32_t f[6];
    getTextureFetchRaw(slotIndex, f);
    return (f[0] & 3u) == 2u;
  }

  static constexpr uint32_t kTextureFetchSlots = 32;

  // Total vertex fetch slots: 96 (32 groups × 3 vertex fetches per group).
  static constexpr uint32_t kVertexFetchSlots = 96;

private:
  uint32_t regs_[kNumRegisters] = {};
  uint32_t aluDirty_ = ~0u;
};

// The one device-state shadow (gfx/record_engine.cpp).
XenosRegisterFile& getXenosRegisters();

struct RecordEngine {
  // GPU physical address -> host pointer (nullptr = unmapped: the operation is skipped).
  // Live: guest VA 0x80000000 + (phys & 0x1FFFFFFF); replay: its own arena.
  uint8_t* (*physToHost)(void* user, uint32_t phys) = nullptr;
  void* user = nullptr;

  // iter-441 MIGRATION STAGE-A: whether emitDraw() submits GuestDrawCommands
  // to the backend queue. TRUE for the replay fixture (offline regression);
  // FALSE for a live instance that serves only as the command processor (fences, interrupts, health counters).
  bool submitDraws = true;

  // Counters. `packets` counts execute() calls in the development walker (one per trace packet record / one per live
  // scan window; the shader-load census reads it) — kept identical to the replay's proven meaning.
  uint64_t packets = 0;
  uint64_t draws = 0;
  uint64_t regWrites = 0;
  uint64_t vertexShaderLoads = 0;
  uint64_t pixelShaderLoads = 0;
  // The command census, numbered by the PM4 opcode a record stands for (a DRAW_INDX record counts under 0x22, a shader
  // bind under 0x27 / 0x2B, ...): the one histogram the record path and the development walker share.
  uint32_t opcodeHist[256] = {};

  // RB_COPY_DEST_* resolve latch (the copy-destination registers' side effect in writeReg).
  uint32_t copyDestBase = 0;
  uint32_t lastNonZeroCopyDestBase = 0;
  uint32_t copyDestPitch = 0;
  uint32_t copyDestInfo = 0;

  // Shader microcode captured from the shader binds (IM_LOAD / IM_LOAD_IMMEDIATE's records),
  // host-endian dwords. The vertex FORMAT lives in the VS vfetch instructions
  // (gfx/xenos_ucode.hpp) — nowhere else.
  std::vector<uint32_t> vertexUcode, pixelUcode;
  // iter-359c: XXH3-64 of the raw BE ucode bytes of the last loaded VS/PS —
  // the translated-shader cache key (gfx/shader_cache.hpp). Stamped into every
  // GuestDrawCommand.
  uint64_t lastVsUcodeHash = 0, lastPsUcodeHash = 0;
  // NATIVE N1 (record mode only): constant blocks the active shaders may read
  // (ucode::constantBlocksRead; bit 16 = relative addressing) and the VS load source.
  uint32_t lastVsConstUse = 0, lastPsConstUse = 0;
  uint32_t lastVsLoadAddr = 0, lastVsLoadDwords = 0;
  uint64_t xlatVsHits = 0, xlatPsHits = 0, xlatMisses = 0;
  // iter-369e: shader loads rejected as implausibly large. NON-ZERO MEANS A LOAD NAMED BYTES THAT ARE NOT A SHADER —
  // treat it as a health metric, not as noise. The golden trace replay must keep this at 0.
  uint64_t badShaderLoads = 0;
  // iter-370: loads rejected because they do not decode as Xenos control flow (see the STRUCTURE GATE in
  // onShaderLoad); iter-460: ucode found in the other host window.
  uint64_t imLoadWindowRescues = 0;
  uint64_t bogusShaderLoads = 0;
  // Parsed vfetch bindings of the LAST loaded vertex shader (= the currently
  // bound VS in a streaming model where the bind immediately precedes draws).
  // emitDraw uses these for the REAL fetch slot / stride / position layout.
  std::vector<ucode::VertexBinding> lastVsBindings;
  // iter-466 cp round 2 (SR_CP_SHLOAD_MEMO): what a ucode's bytes decode to, once per XXH3 of
  // the raw big-endian bytes (+ size): valid control flow (only valid ucode is stored), its
  // vfetch bindings and their formats. The game re-sends its shaders before most
  // draws; each load re-swapped, re-validated (twice for a pointer load), re-walked a 256-entry
  // census and re-parsed its bindings with allocations (~3% of the CP).
  struct ShLoadMemo {
    uint32_t dwords = 0;
    bool bindOk = false;
    std::vector<ucode::VertexBinding> bindings;
    std::vector<uint8_t> attrFormats;  // every binding's attributes, in order
  };
  std::unordered_map<uint64_t, ShLoadMemo> shLoadMemo;
  uint64_t lastVsBindingsHash = 0;  // the memo entry lastVsBindings was copied from (0 = none)
  // iter-466 cp round 2 (SR_CP_CONST_TRACK): the consumer's constant blocks as this producer
  // last sent them (cvValid: blocks sent at least once since the last resync), register writes
  // not yet compared (cvPending), and the tracked-command sequence.
  uint32_t cvShadow[2048] = {};
  uint32_t cvValid = 0, cvPending = 0, cvSeq = 0;
  uint64_t cvBlocksUsed = 0, cvBlocksSent = 0;

  // Bin predication state (Xenia command_processor.h:524-525, both init ~0):
  // a predicated record (or type-3 packet with header bit 0 set) is SKIPPED when
  // (binSelect & binMask) == 0 (pm4_command_processor_implement.h:411-418).
  // Set by the kRkBinReg record (PM4_SET_BIN_MASK_LO/HI 0x60/0x61, _BIN_SELECT_LO/HI 0x62/0x63).
  uint64_t binMask = ~0ull, binSelect = ~0ull;
  uint64_t predSkipped = 0;   // predicated records / packets skipped by the gate
  uint64_t resolveDraws = 0;  // NATIVE strand A tile census: emitDraw with RB_MODECONTROL edram mode 6
  uint64_t predDrawsRun = 0;  // tile census: predicated draws that passed the bin test
  uint64_t predDrawsBlend = 0;  // tile census: ... of them with RB_BLENDCONTROL0 != ONE/ZERO
  bool curPredicated = false;  // the draw being emitted came from a predicated record / packet
  uint64_t cpInterrupts = 0;  // interrupts delivered (the kRkCallback item's / PM4_INTERRUPT's ISR dispatch)

  // The command-processor services (kernel/gpu_command_processor.cpp). cpMode = this instance IS the
  // console CP, running on its own thread (Xenia CommandProcessor::WorkerThreadMain): a register handshake
  // really waits, an interrupt calls the guest ISR at the record, scratch registers write back to memory,
  // a swap presents. OFF for the replay fixture and a non-CP instance, which therefore stay bit-identical.
  bool cpMode = false;
  struct CpHooks {
    void* user = nullptr;
    // Guest ISR, source 1, once per set bit of cpu_mask, synchronously.
    void (*interrupt)(void* user, uint32_t cpuMask) = nullptr;
    // The swap (VdSwap's native item / PM4_XE_SWAP in the oracle): present the front buffer.
    void (*swap)(void* user, uint32_t fbPhys, uint32_t width, uint32_t height) = nullptr;
    // Queued draws must reach the host GPU before the CPU can observe progress
    // (fence/memory writes, interrupts, swaps, waits).
    void (*flush)(void* user) = nullptr;
    // false = abandon a wait (shutdown).
    bool (*running)(void* user) = nullptr;
    // Xenia counter_ (bumped per swap and per vblank): EVENT_WRITE_SHD bit 31.
    uint32_t (*counter)(void* user) = nullptr;
    // CPU-virtual address -> host (the 0x7F000000 writeback window).
    uint8_t* (*virtToHost)(void* user, uint32_t va) = nullptr;
    // iter-467 native-a P2 trace (observe): what 0 = EVENT_WRITE_SHD store {phys, value},
    // 1 = scratch write-back {phys, value}. Null unless SR_P2_TRACE is set.
    void (*trace)(void* user, uint32_t what, uint32_t addr, uint32_t value) = nullptr;
    // p2.zpd (SR_NATIVE_ZPD, gfx/native_zpd.hpp): the covering-fence check, called by whoever performs an
    // EVENT_WRITE_SHD store (the CP after its drain, or the executor's store item) while a drained END is
    // unwritten - strict mode renders the set so far and writes the slot words before the store.
    void (*zpdFence)(void* user) = nullptr;
  } cp;
  uint64_t cpWaits = 0, cpWaitsBlocked = 0, cpMemWrites = 0, cpScratchWrites = 0;
  uint64_t cpSwaps = 0;
  // Coherency requests (a COHER_BASE_HOST write: the kRkInvalidate record or the 11-dword block's PM4): all, the
  // whole-buffer ones, those whose range reaches 4 GB (base + size wraps; native/coher-sentinel: the all-ones
  // sentinel defect's signature, must stay 0) and the exact bogus {0xFFFFF000, 0x1000} page among them.
  uint64_t cpCoher = 0, cpCoherWhole = 0, cpCoherTop = 0, cpCoherPage = 0;

  // [VFETCH] census — decode validation (logging capped, counting uncapped).
  uint32_t vfetchOk = 0, vfetchFail = 0, vfetchBindings = 0, vfetchAttrs = 0;
  uint32_t vfetchFormatHist[64] = {};

  // The window the development walker is in and the packet it is at (observe-only; set by execute(), read by the
  // [CP_WAIT] window / [SCRATCH_CFG] diagnostics of the handshake and the scratch write-back). C1 stage 1
  // (docs/RELEASE_COMPOSITION.md 9.9): the walker is not linked in the release configuration, so neither are these.
#if SR_ORACLE
  const uint8_t* stallHost = nullptr;
  size_t stallBytes = 0, stallOff = 0;
  int stallDepth = 0;
#endif

  // Single register write with its side effects (also used by the replay's kRegisters records).
  void writeReg(uint32_t reg, uint32_t value);

  // NATIVE strand A S1 (kernel/native_flush.hpp, SR_NATIVE_FLUSH): the effect of one register run / one draw,
  // applied from a native draw record - no PM4 header decode. nativeRun = a bulk store when no register in the
  // run has a writeReg side effect, else writeReg per register; nativeDraw = the bin predication when
  // `predicated`, then emitDraw.
  void nativeRun(uint32_t startReg, uint32_t count, const uint8_t* beValues);
  void nativeDraw(bool predicated, uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);
  // NATIVE strand A P1 rows 1+2 (kernel/native_record_format.hpp RecKind): the shader bind and constant
  // load as ONE implementation each - nativeRecord() calls them for the record, the development walker for the
  // packet (Xenia pm4_command_processor_implement.h semantics, nothing PM4-shaped inside).
  void applyImLoad(uint32_t addrType, uint32_t startSize);                                // kRkShaderPtr  (PM4_IM_LOAD 0x27)
  void applyImLoadImmediate(uint32_t shaderType, uint32_t startSize, const uint8_t* ucodeBE,
                            uint32_t cnt);                                                // kRkShaderImm  (PM4_IM_LOAD_IMMEDIATE 0x2B; cnt = body dwords incl. the 2 header words)
  void applyLoadAluConstant(uint32_t srcPhys, uint32_t offsetType, uint32_t sizeDwords);  // kRkConstLoad  (PM4_LOAD_ALU_CONSTANT 0x2F)
  void applySetConstant(uint32_t offsetType, const uint8_t* valuesBE, uint32_t n);        // kRkSetConst   (PM4_SET_CONSTANT 0x2D)
  void applyBinReg(uint32_t which, uint32_t value);                                       // kRkBinReg     (0x60..0x63: bin mask lo/hi, bin select lo/hi)
  // The register / memory handshake from a record (strand B's resolve op list; the coherency block's own wait): the
  // body as the packet carries it (5 big-endian dwords: waitInfo, pollAddr, ref, mask, wait) - the same handshake /
  // test the development walker runs for WAIT_REG_MEM (cpMode only; a no-op otherwise, the instant-consumption model).
  void applyWaitRegMem(const uint8_t* bodyBE, uint32_t cnt);
  // The fence word store (kRkFence / kRkEventShd; EVENT_WRITE_SHD 0x58's effect): VGT_EVENT_INITIATOR, then the
  // store of `value` (or the counter when initiator bit 31) at `addrWithEndian`, in stream order (flush first).
  void applyEventWriteShd(uint32_t initiator, uint32_t addrWithEndian, uint32_t value);
  void applyRegRmw(uint32_t rmwInfo, uint32_t andMask, uint32_t orMask);  // kRkRegRmw (PM4_REG_RMW 0x21)
  // GpuBuildFlushMemoryPackets' COHER block as registers: COHER_STATUS_HOST 0xA31 = 0x03000000 (the
  // deferred CPU-write range) or 0x01000000 (the whole command buffer), COHER_SIZE 0xA2F, COHER_BASE
  // 0xA30; its WAIT_REG_MEM is the CP's own register handshake (instant here). The kRkInvalidate record.
  void applyCoherency(uint32_t base, uint32_t size, bool whole);
  // The interrupt's effect (kRkCallback's item): flush, then the guest ISR (source 1) once per set bit of
  // cpuMask, synchronously (the CP hook). cpMode only.
  void applyInterrupt(uint32_t cpuMask);
  // Present in stream order, after queued draws reach the GPU (the kRkSwap item; the development
  // PM4 oracle's XE_SWAP), with no packet decoding.
  void applySwap(uint32_t fbPhys, uint32_t width, uint32_t height);
  void applyEventWrite(uint32_t initiator);                                               // kRkEvent (PM4_EVENT_WRITE 0x46)
  // p2.zpd (SR_NATIVE_ZPD, baked default ON since native/zpd-bake): EVENT_WRITE_ZPD 0x5B's effect - the kRkZpd record
  // (GpuEmitTileSurfaceConfig_82101E08's port) and the development walker's packet reach this one implementation:
  // cpZpdEvents, VGT_EVENT_INITIATOR, then (zpdNative) the host occlusion query. true = the native path took it.
  // record: the event came as the kRkZpd record (the port), not the literal packet - the frame stamp's form
  // (gfx/native_zpd noteSource; diagnostic, counted only while the freeze dump runs).
  bool applyZpdEvent(uint32_t initiator, bool record = false);                            // kRkZpd (PM4_EVENT_WRITE_ZPD 0x5B)
  // The native path (the live CP that submits draws, the mode not off): applyZpd at RB_SAMPLE_COUNT_ADDR (0x2325).
  bool zpdNative();
  // RB_SAMPLE_COUNT_ADDR = sampleCountAddr: classifies BEGIN / END (gfx/native_zpd) and queues the marker at its stream
  // position (the draw queue is FIFO: the render loop opens / closes the host query there).
  void applyZpd(uint32_t sampleCountAddr);
  uint64_t cpZpdEvents = 0;  // EVENT_WRITE_ZPDs applied (record or packet)
  uint64_t cpZpdNative = 0;  // ... of them the native path took (SR_NATIVE_ZPD)
  // A whole native record stream [w, e) (kernel/native_record_format.hpp words) through the NativeRenderContext of
  // gfx/record_context.cpp: every record validated before it mutates anything.
  using NativeRecordStats = sr::gfx::NativeRecordStats;
  // A rejected record may follow a completed prefix. Latch it; neither later
  // records nor the development packet oracle may replay this engine's stream.
  bool nativeRecordFailed = false;
  bool nativeRecord(const uint32_t* w, const uint32_t* e, NativeRecordStats& st, bool timing);

  // The opaque record kinds (kernel/native_record_format.hpp recOpaque: strand B's op lists, the transport's blocks)
  // are applied by the handler their owner registered (kernel/native_flush.hpp setOpaqueHandler). That registry's
  // handler takes the CP's engine (kernel/native_flush.hpp CpEngine: the development walker, or this class in the
  // release configuration), so the engine's owner installs the lookup and the dispatch here: Pm4Engine's constructor
  // (gfx/pm4_engine.cpp) in the development configuration, the live engine's construction (gfx/null_renderer.cpp) in the
  // release one (C1 stage 1). Null = no opaque kind applies: such a record is rejected.
  static bool (*opaqueKnown)(uint32_t kind);
  static bool (*opaqueApply)(RecordEngine& eng, uint32_t kind, const uint32_t* w, uint32_t n);

 protected:
  void onShaderLoad(uint32_t shaderType, uint32_t addr, const uint8_t* ucodeHost,
                    uint32_t sizeDwords, bool inlineUcode = false);
  void emitDraw(uint32_t vgtDrawInitiator, uint32_t dmaBasePhys, uint32_t dmaSizeReg);
  // The handshake (cpMode): poll a register or a memory word until (value & mask) matches ref under the compare
  // function; applyWaitRegMem's body and the development walker's WAIT_REG_MEM.
  void cpWaitRegMem(const uint8_t* body);

 private:
  uint32_t vsLogN_ = 0, psLogN_ = 0, vfetchVerboseN_ = 0;
};

}  // namespace gfx
}  // namespace sr
