// nb_rec_gpr.inc - included inside `namespace sr::native::recb { ... }` by native_rec_b.cpp (P1 row 9, kind kGpr).
//
// The XDK's SetShaderGPRAllocation (IDA: GpuRingEmitRegisterBlock_sub_820F5570) and its tail call
// D3DDeviceClearFullSurface_sub_82100488 (whose only caller is 820F5570), ported from the asm (IDA, default.xex),
// instruction block by instruction block. Same packets in the same order, same device writes; the ring cursor
// stores (stw rX,0x28(dev)) belong to the framework (Prod's cursor model).
//
// Arguments: r3 dev, r5 VS GPR count, r6 PS GPR count. r4 is never read (820F5570 does not touch it and
// ClearFullSurface overwrites it at 82100498 before any use).
//
// Stops (the guest body runs instead): every XDK kick site that would kick in the dry CHECK (Prod::kick), the
// allocator off its carve path (Prod::alloc), the shared flush's own stops (phaseC: tiled 0x2203, a run crossing
// the segment limit), the tiled restore scissor (windowScissor), the .rdata ucode unmapped (never expected), a
// live guest write that is unmapped (Prod::gwBytes).

namespace {

// 8210067C..82100688: r30 = 0x820B59DC (the PS ucode), r4 = r30 + (0x820BA0D0 - 0x820B59DC) (the VS ucode).
constexpr uint32_t gp_kVsUcodeVA = 0x820BA0D0u;
constexpr uint32_t gp_kVsUcodeDwords = 0x1Bu;  // li r6,0x1B (start 0, size 27) / li r5,0x6C (memcpy bytes)
constexpr uint32_t gp_kPsUcodeVA = 0x820B59DCu;
constexpr uint32_t gp_kPsUcodeDwords = 9u;  // li r9,9 (start 0, size 9) / li r5,0x24 (memcpy bytes)
constexpr uint32_t gp_kAllocDwords = 7u;    // 82100884 li r4,7
constexpr uint32_t gp_kAllocAlign = 4u;     // 82100880 li r5,4
constexpr uint8_t gp_kZero[4u * gp_kAllocDwords] = {};  // 82100898 memset(r26, 0, 0x1C)

// The host bytes of an .rdata range (already big-endian); nullptr = not one contiguous mapped range.
const uint8_t* gp_rdata(uint32_t va, uint32_t bytes) {
  const uint8_t* h = ::sr::kernel::guestVAToHost(va);
  const uint8_t* e = ::sr::kernel::guestVAToHost(va + bytes - 1u);
  if (!h || !e || uint32_t(e - h) != bytes - 1u) return nullptr;
  return h;
}

// slwi rX,rY,{17|1} ; srawi rZ,rX,0x11 - the 15-bit field the shift left-aligned, sign-extended (callers pass the
// already shifted word).
inline uint32_t gp_sra17(uint32_t shifted) { return uint32_t(int32_t(shifted) >> 17); }

// 821008AC..821008D8: the GPU address of the allocation, | 3 (the vertex fetch constant's type bits).
inline uint32_t gp_fetchAddr(uint32_t va) {
  const uint32_t lo = va & 0x1FFFFFFCu;               // rlwinm r9,r26,0,3,29
  const uint32_t hi = ((va >> 20) + 0x200u) & 0x1000u;  // srwi r11,r26,20 ; addi r11,r11,0x200 ; rlwinm r10,r11,0,19,19
  return (hi + lo) | 3u;                              // add r10,r10,r9 ; ori r10,r10,3
}

// D3DDeviceClearFullSurface 82100488 (r3 = dev).
void gp_clearFullSurface(Prod& p, const uint8_t* vsUcode, const uint8_t* psUcode) {
  // 82100498..8210063C: the dirty flush. Instruction for instruction the Resolve's 8210CA58..8210CC00 (registers
  // r29 for r31; r3 still holds dev at the 821072F8 call): the same tests, masks, register bases, source offsets,
  // order and mask clears, and the same 821072F8 OR-back -> the shared phaseC.
  if (!phaseC(p)) return;

  // 82100640: kick check.
  if (!p.kick()) return;
  // 8210065C..821006A4: INVALIDATE_STATE {0x300} (C0003B00), then IM_LOAD_IMMEDIATE (C01C2B00) vertex shader:
  // {type 0, start/size 0x1B} + 27 dwords memcpy'd from 0x820BA0D0.
  p.t3(0x3Bu, false, {0x300u});
  p.t3n(0x2Bu, false, {0u, gp_kVsUcodeDwords}, gp_kVsUcodeDwords, vsUcode);

  // 821006A8..821006B8: kick check.
  if (!p.kick()) return;
  // 821006C4..821006F0: IM_LOAD_IMMEDIATE (C00A2B00) pixel shader: {type 1, start/size 9} + 9 dwords from 0x820B59DC.
  p.t3n(0x2Bu, false, {1u, gp_kPsUcodeDwords}, gp_kPsUcodeDwords, psUcode);
  // 821006F4..82100714: {00012180, 0x1001000E, 0}.
  p.t0(0x2180u, {0x1001000Eu, 0u});

  // 82100718..82100728: kick check.
  if (!p.kick()) return;
  // 82100738: GpuDefaultStateBlockInit 8210C518 (56 bytes, its dirty bits; no kick check inside).
  defaultBlock(p);
  if (p.stopped) return;

  // 82100740..821007D4: seven separate type-0 packets (headers 00002200, 00002203, 00002208, 00002104, 00002280,
  // 00002302, 00022080).
  p.t0(0x2200u, {0u});
  p.t0(0x2203u, {0u});
  p.t0(0x2208u, {4u});
  p.t0(0x2104u, {0u});
  p.t0(0x2280u, {0x00080008u});
  p.t0(0x2302u, {4u});
  p.t0(0x2080u, {0u, 0u, 0x00100010u});

  // 821007DC..82100850: dirty bits (each a ld / or / std chain; the net effect, in the XDK's order).
  uint64_t q10 = p.r64(0x10);
  q10 |= 1ull << 51;  // extldi r12,1,64,51
  q10 |= 1ull << 52;  // extldi r12,1,64,52
  q10 |= 0x800ull;    // ori 0x800
  q10 |= 0x100ull;    // ori 0x100
  q10 |= 0x8ull;      // ori 8
  p.w64(0x10, q10);
  uint64_t q18 = p.r64(0x18);
  q18 |= 0x40000ull;  // oris r11,r11,4
  q18 |= 1ull << 59;  // extldi r12,1,64,59
  p.w64(0x18, q18);
  p.w64(0x20, p.r64(0x20) | (1ull << 35));  // extldi r12,1,64,35
  p.w64(0x10, p.r64(0x10) | (1ull << 43));  // extldi r12,1,64,43

  // 82100864..821009E4: two passes (r28 = 0, 1; cmplwi r28,2 ; blt).
  for (uint32_t pass = 0; pass < 2u; pass++) {
    // 82100864..8210087C: kick check.
    if (!p.kick()) return;
    // 82100880..82100894: GpuCmdBufAllocate 820FCDF0(dev, 7, 4); 0 -> 82100A20: stw r27,0x28(dev) and return
    // (no restore scissor). A stopped allocator also returns 0 here.
    const uint32_t va = p.alloc(gp_kAllocDwords, gp_kAllocAlign);
    if (!va || p.stopped) return;
    // 82100898..821008A4: memset(va, 0, 0x1C).
    p.gwBytes(va, gp_kZero, sizeof(gp_kZero));
    if (p.stopped) return;
    // 821008A8..82100930: {00054800, addr|3, 0x1000001E, 0, 0, 0, 0} (fetch constant 0x4800, 6 dwords).
    p.t0(0x4800u, {gp_fetchAddr(va), 0x1000001Eu, 0u, 0u, 0u, 0u});
    // 8210094C..82100964: {00025000, 0, 0, 0}.
    p.t0(0x5000u, {0u, 0u, 0u});
    // 8210096C..821009DC: 12 x DRAW_INDX_2 {C0003600, 0x00010081} (one auto-index point each).
    for (uint32_t k = 0; k < 12u; k++) p.t3(0x36u, false, {0x00010081u});
  }

  // 821009E8..82100A14: restore the window scissor from the shadow: D3DDeviceEmitScissorTileRects 820F0208(dev,
  // r4 = sext15(0x2844 bits 0..14), r5 = sext15(0x2844 bits 16..30), r6 = sext15(0x2848 bits 0..14),
  // r7 = sext15(0x2848 bits 16..30)).
  const uint32_t s44 = p.r32(0x2844u), s48 = p.r32(0x2848u);
  (void)windowScissor(p, gp_sra17(s44 << 17), gp_sra17(s44 << 1), gp_sra17(s48 << 17), gp_sra17(s48 << 1));
}

}  // namespace

// SetShaderGPRAllocation 820F5570 (r3 = dev, r5 = VS GPRs, r6 = PS GPRs).
void portGpr(Prod& p) {
  // The IM_LOAD_IMMEDIATE ucode (.rdata; the XDK memcpy's it). Checked before anything is written.
  const uint8_t* vsUcode = gp_rdata(gp_kVsUcodeVA, 4u * gp_kVsUcodeDwords);
  const uint8_t* psUcode = gp_rdata(gp_kPsUcodeVA, 4u * gp_kPsUcodeDwords);
  if (!vsUcode || !psUcode) {
    p.stop("gpr: ucode unmapped");
    return;
  }

  // 820F5584: insrwi r6,r5,7,17 (= rlwimi r6,r5,8,17,23): r5's low 7 bits into r6 bits 8..14.
  const uint32_t r6 = (p.ctx->r6.u32 & ~0x7F00u) | ((p.ctx->r5.u32 & 0x7Fu) << 8);
  // 820F558C: clrlslwi r31,r6,17,4 (= rlwinm r31,r6,4,13,27).
  uint32_t v = (r6 & 0x7FFFu) << 4;
  // 820F5590: rlwinm. r31,r31,0,21,19 - clears bit 0x800 (r6 bit 7). cr0 is the 32-bit result (the recompiled
  // body's compare<int32_t>, and Xenia's zero-extended rlwinm agree), so 0 -> 820F5598 lis r31,4 ; ori 0x400.
  v &= ~0x800u;
  if (v == 0u) v = 0x00040400u;
  // 820F55A8: stw r31,0x2A28(dev) (before the kick check, as the asm orders it).
  p.w32(0x2A28u, v);
  // 820F55A0..820F55B8: kick check.
  if (!p.kick()) return;
  // 820F55BC..820F55F8: {000005C8, 0x20000}, then 8 x {00000D00, v | 1}, then {00000D00, v}.
  p.t0(0x05C8u, {0x00020000u});
  for (uint32_t k = 0; k < 8u; k++) p.t0(0x0D00u, {v | 1u});
  p.t0(0x0D00u, {v});
  // 820F5600: bl D3DDeviceClearFullSurface 82100488 (r3 = dev), then return.
  gp_clearFullSurface(p, vsUcode, psUcode);
}
