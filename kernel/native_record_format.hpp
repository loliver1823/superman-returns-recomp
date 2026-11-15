#pragma once

#include <cstdint>

// Native producer/consumer data ABI. The literal PM4 kind is retained for the
// development oracle; the independent native executor explicitly rejects it.
namespace sr::native::flush {

enum RecKind : uint32_t {
  kRkDraw = 0,       // initiator, dmaBase, dmaSize                                (n unused)
  kRkShaderPtr = 1,  // addrType, startSize                                        (IM_LOAD)
  kRkShaderImm = 2,  // shaderType, startSize, then n dwords of ucode (big-endian) (IM_LOAD_IMMEDIATE)
  kRkConstLoad = 3,  // srcPhys, offsetType, sizeDwords                            (LOAD_ALU_CONSTANT)
  kRkSetConst = 4,   // offsetType, then n big-endian values                       (SET_CONSTANT)
  kRkBinReg = 5,     // value; n = which: 0 mask lo, 1 mask hi, 2 select lo, 3 select hi
  kRkEvent = 6,      // initiator                                                  (EVENT_WRITE)
  // Strand B's kinds (NATIVE_ROADMAP "Record interface for strand B"): OPAQUE to the CP's own apply.
  // The payload is the owner's POD snapshot (edram::Regs, host order, n dwords); execution goes
  // through the handler the owner registers (setOpaqueHandler); a record whose kind has no handler,
  // or whose n the handler rejects, counts as bad.
  kRkResolve = 7,
  kRkClear = 8,
  kRkClearFull = 9,
  // P2-B transport kinds (SR_NATIVE_SEGMENTLESS, NATIVE_ROADMAP "P2-B BUILD PLAN"), applied by the CP:
  kRkPm4 = 10,         // n dwords of literal PM4 (reserved; the segment parts carry gap bytes directly)
  kRkFence = 11,       // fb4Addr|2, progress, fb0Addr|2, S  = the 10-dword fence tail of GpuBuildFenceWritePacket
  kRkInvalidate = 12,  // base, size, whole                 = the 11-dword COHER block of GpuBuildFlushMemoryPackets
  // p2.transport step 1 (native-a, 2026-10-11; the orchestrator moved the kind to native-a 2026-10-10): the block of
  // GpuEmitCpuCallbackPackets_820FDA80 {cb, arg, mask, flags, pcAddr} (5 words, host order) - applied by native_flush's
  // own opaque handler through the methods the 23 / 25 PM4 dwords reach (SR_NATIVE_CALLBACK).
  kRkCallback = 13,
  // P2-B item 5 (native-a, 2026-10-10): D3DDeviceEmitPm4Packet_82113010 - the per-tile constants the CALLER
  // fills after the call (Lion SetPerTileConstC100 827333E0). Payload {header, offsetType, bodyVA} (3 words,
  // host order): the CP reads the (header count) values at bodyVA when it APPLIES the record, as the PM4 path
  // reads them from the ring; the renderer writes the XDK bytes (header, offset, the body copied from
  // memory) for the dry CHECK and the ring path, where the span stays literal. Applied through
  // Pm4Engine::applySetConstant; handler + renderer registered by native_flush itself (an opaque kind).
  kRkSetConstMem = 14,
  // boot.device_create (native-a, 2026-10-10): the two packet forms the device-create producers write that no
  // kind covered - REG_RMW {rmwInfo, andMask, orMask} (D3DDeviceResetState_8210E520's SCLK_PWRMGT runs) and
  // EVENT_WRITE_SHD {initiator, addr|endian, value} (GpuCreateBlockInterruptThreads_82110EB8's per-CPU
  // interrupt word). Applied through Pm4Engine::applyRegRmw / applyEventWriteShd, the methods the PM4
  // packets reach; CPPM4 form = the 4-dword packet.
  kRkRegRmw = 15,
  kRkEventShd = 16,
  // Experimental present pacing command: argument, scratch root physical|2,
  // independently translated gate word physical|2 (SR_NATIVE_GATE).
  kRkPresentGate = 17,
  // Shader-constant coherency preamble: COHER_DEST_BASE_1. The other writes
  // are fixed and the SDK's status wait is already satisfied (bit 31 clear).
  kRkShaderConstSync = 18,
  // VdSwap's reserved block: hasFetch, physical fetch[6], fbPhys, width, height.
  // The surrounding 82112050 SDK body is a separate, unfinished replacement.
  kRkSwap = 19,
  // Fixed scaler-reset suffix of 82112050: reserved zero payload word.
  kRkSwapScalerReset = 20,
  // p2.transport BRACKET_END (SR_NATIVE_BRACKET, NATIVE_ROADMAP transport plan item 4 build note): the Z-pass kick
  // 82110C78's 23-dword callback block replaced by the consumer's own replay. Payload {generation lo, hi, step
  // count, cb, list-head view, cpu mask, flags, pc|2} (8 words, host order); the last five render the SDK's 23
  // dwords. The replay steps travel as the SEGMENT's parts after this record (native_flush.cpp cpSegment).
  kRkBracket = 21,
  // p2.transport RESIDUE (SR_NATIVE_LOCK_COHER, NATIVE_ROADMAP transport plan item 5 build note): the lock core
  // 820F3988's owner-thread COHER block when its COHER_STATUS_HOST (the caller's stack argument) is neither of the two
  // kRkInvalidate encodes (0x03000000 / 0x01000000) - in this title the texture lock's 0x02000000. Payload {status,
  // size, base} (3 words, host order); applied as kRkInvalidate is (COHER_STATUS_HOST / SIZE / BASE, then the
  // register-mode WAIT_REG_MEM {3, 0xA31, 0, 0x80000000, 8}), the status as given; PM4 form = the SDK's 11 dwords.
  kRkCoherHost = 22,
  // boot gaps (native/boot-gaps, 2026-10-16; SR_NATIVE_DISPLAY, kernel/native_display_emit.cpp): the display
  // controller's emitters - reached by the first Swap's persisted-display path and its scaler / gamma restore.
  // kRkGammaRamp = GpuRingEmitGammaRamp_821056E0 / GpuRingEmitGammaRampWide_821057C0. Payload {form, values} (host
  // order): form 0 = the 256-entry table, 256 DC_LUT_30_COLOR words (n 257; the SDK's 2309 dwords); form 1 = the PWL,
  // 128 x {r, g, b} DC_LUT_PWL_DATA words (n 385; 1413 dwords). Applied as the packets' register writes: RW_MODE 0x1921 =
  // form and RW_INDEX 0x1922 = 0, WRITE_EN_MASK 0x1927 = 7, then per entry the colour write(s) (0x1925, or 0x1924 three
  // times) and the COND_WRITE {7 = always, register} of RW_INDEX = entry + 1.
  kRkGammaRamp = 23,
  // GpuScalerEmitCommandBuffer_8210A788's block when the platform's VdInitializeScalerCommandBuffer returned no scaler
  // program (0 dwords: this port's display has no hardware scaler). Payload {0} (reserved); the SDK's 20 dwords: 0x1973
  // = 0x10000, 0x1964 = 1, 0x1973 = 0, WAIT_REG_MEM {3, 0x1973, 0, 1, 256}, 0x1852 = 0, 0x1838 = 1, 0x1973 = 0x10000,
  // 0x1973 = 0.
  kRkScaler = 24,
  // p2.zpd's bake (native/zpd-bake, 2026-10-16): EVENT_WRITE_ZPD {initiator} - the occlusion query's sample-count
  // report that GpuEmitTileSurfaceConfig_82101E08 ends with (D3DQueryIssue 82102BC8's BEGIN / END; RB_SAMPLE_COUNT_ADDR
  // travels before it as a register run). Applied through RecordEngine::applyZpdEvent, the method the 0x5B packet
  // reaches (gfx/native_zpd: a host occlusion query); PM4 form = {0xC0005B00, initiator}. A packet record, not opaque.
  kRkZpd = 25,
  kRkKinds
};
constexpr bool recOpaque(uint32_t kind) {
  return (kind >= kRkResolve && kind <= kRkClearFull) || kind == kRkCallback || kind == kRkSetConstMem ||
         kind == kRkPresentGate || kind == kRkShaderConstSync || kind == kRkSwap || kind == kRkSwapScalerReset ||
         kind == kRkBracket || kind == kRkCoherHost || kind == kRkGammaRamp || kind == kRkScaler;
}
constexpr uint32_t recTag(uint32_t kind, uint32_t n, uint32_t flags) {
  return 0x80000000u | (kind << 24) | ((n & 0xFFFFu) << 8) | flags;
}
// Payload words of a PACKET record with tag field n (0 = unknown kind).
constexpr uint32_t recWords(uint32_t kind, uint32_t n) {
  return kind == kRkDraw ? 3u : kind == kRkShaderPtr ? 2u : kind == kRkShaderImm ? 2u + n
       : kind == kRkConstLoad ? 3u : kind == kRkSetConst ? 1u + n : kind == kRkBinReg ? 1u
       : kind == kRkEvent ? 1u : kind == kRkZpd ? 1u : recOpaque(kind) ? n : kind == kRkPm4 ? n : kind == kRkFence ? 4u
       : kind == kRkInvalidate ? 3u : kind == kRkRegRmw ? 3u : kind == kRkEventShd ? 3u : 0u;
}

}  // namespace sr::native::flush
