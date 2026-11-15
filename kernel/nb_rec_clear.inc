// nb_rec_clear.inc - included inside `namespace sr::native::recb { ... }` by native_rec_b.cpp (P1 row 9, kind kClearF).
//
// The XDK's D3DDeviceClearF and everything under it that writes the ring, the device or guest memory, ported from
// the asm (IDA, default.xex) block by block:
//   82101A58  D3DDeviceClearF                  -> portClearF
//   821013D0  D3DDeviceClearImpl               -> cl_clearImpl
//   82100F70  D3DDeviceClearSurface            -> cl_clearSurface
//   8210C090  SurfaceClearColorPackByFormat    -> cl_packColor = the Resolve port's rs_colorPack (+ 8210BEA0)
//   82100D20  D3DDeviceClearRectTileSplit      -> cl_split
//   82100A28  D3DDeviceEmitClearRectVertices   -> cl_emit
// Same packets in the same order, same device writes (q10 / q18 / q20, byte 0x2A39), same guest writes (the vertex
// allocation). The ring cursor stores (stw rX,0x28(dev)) belong to the framework (Prod's cursor model).
//
// Shared helpers (native_rec_b.cpp):
//   phaseC         ClearImpl 82101554..821016FC is instruction for instruction Resolve's flush at 8210CA58: the same
//                  q10 != 0 and q10 & 0x100 tests, 821072F8(dev, q10, [0x28C0], 0) with the OR-back into q10, then
//                  the 0x2200 / 0x2180 / 0x2100 / 0x2000 / 0x2280 / 0x2300 runs (82107520) with the same extldi masks
//                  (q10<<52; q10 bits 44..48; q18 bits 2..22, 23..38, 39..59; q20<<26), the same source offsets
//                  (0x28B4, 0x28A0, 0x284C, 0x2800, 0x28E4, 0x2938), order and mask clears.
//   hizWrite       821072F8 from ClearSurface 82101084 (r4 = 0, r6 = 0; ClearSurface discards the return value).
//   windowScissor  820F0208 from ClearImpl 82101968.
//   defaultBlock   8210C518 from ClearImpl 821017FC.
//
// Floating point follows the RECOMPILED guest body (the CHECK's oracle): lfs = double(float), stfs = float(double),
// fsubs / frsp = rounded through float, vmaddfp = mul then add (two roundings), vmaxfp / vminfp = _mm_max_ps /
// _mm_min_ps operand semantics, vrfin = nearest-even, vrfiz = trunc, vctsxs = simde_mm_vctsxs (NaN -> 0, saturating).
//
// Stops (the guest body runs instead):
//   - tiling active at entry: byte 0x2A38 & 0x30 or byte 0x2A3B & 0x20. This covers ClearF's per-tile split
//     (8210D400 SetPredication + ClearImpl per tile), ClearSurface's per-tile loop (0x2A38 & 0x20), 821072F8's tiled
//     0x2203 and 820F0208's tiled restore - the literal checks are kept below at their own sites as well;
//   - a NULL rect with neither RT0 nor DS bound (the XDK would read guest address 0x24);
//   - an unmapped guest read (surface +0x24, the rect, the colour, RT +0x1C) or write (the vertex allocation);
//   - the colour-as-depth packer's own stops (rs_colorPack: format 6, the spliced vpkd3d128 type 3; 8/9/11/13, the XDK
//     returns an uninitialised stack word; 3/12 with a non-finite R/G/B, the CRT ldexp special path);
//   - every XDK kick site that would kick in the dry CHECK (Prod::kick), the allocator off its carve path
//     (Prod::alloc), and the shared helpers' own stops (phaseC, hizWrite, windowScissor, defaultBlock);
//   - the .rdata ucode unmapped (never expected).

namespace {

// 82101744 / 82101748: r29 = 0x820B59DC (PS ucode), r4 = r29 + (0x820BA0D0 - 0x820B59DC) (VS ucode).
constexpr uint32_t cl_kVsUcodeVA = 0x820BA0D0u;
constexpr uint32_t cl_kVsUcodeDwords = 0x1Bu;  // 82101734 li r6,0x1B (size 27) / 82101740 li r5,0x6C (memcpy bytes)
constexpr uint32_t cl_kPsUcodeVA = 0x820B59DCu;
constexpr uint32_t cl_kPsUcodeDwords = 9u;     // 82101790 li r9,9 / 82101794 li r5,0x24

// .rdata constants (IDA read_memory_bytes, big-endian words as host values).
constexpr uint32_t cl_kFZero = 0x00000000u;                // flt_820BCB98  0.0f
constexpr uint32_t cl_kFHalf = 0x3F000000u;                // flt_820B5184  0.5f
constexpr uint64_t cl_kDSixteenth = 0x3FB0000000000000ull;  // dbl_820BD3A0  0.0625
constexpr uint64_t cl_kDDepthScale = 0x3E70000010000010ull; // dbl_820BD398  ~1/16777215

inline float cl_f(uint32_t b) { return __builtin_bit_cast(float, b); }
inline uint32_t cl_u(float f) { return __builtin_bit_cast(uint32_t, f); }
inline double cl_d(uint64_t b) { return __builtin_bit_cast(double, b); }
inline int32_t cl_add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }  // add (32-bit wrap)
// slwi rX,rY,{17|1} ; srawi rZ,rX,0x11 - a 15-bit field left-aligned, sign-extended.
inline uint32_t cl_sra17(uint32_t shifted) { return uint32_t(int32_t(shifted) >> 17); }
inline void cl_be32(uint8_t* d, uint32_t v) {
  d[0] = uint8_t(v >> 24);
  d[1] = uint8_t(v >> 16);
  d[2] = uint8_t(v >> 8);
  d[3] = uint8_t(v);
}

// lfs then stfs as the recompiled body runs them (double(float) then float(double), flush mode off): the bits come
// through unchanged except a signalling NaN comes out quiet (x64 cvtss2sd sets the quiet bit).
inline uint32_t cl_lfsStfs(uint32_t b) {
  return ((b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu)) ? (b | 0x00400000u) : b;
}

// The host bytes of an .rdata range (already big-endian); nullptr = not one contiguous mapped range.
const uint8_t* cl_rdata(uint32_t va, uint32_t bytes) {
  const uint8_t* h = ::sr::kernel::guestVAToHost(va);
  const uint8_t* e = ::sr::kernel::guestVAToHost(va + bytes - 1u);
  if (!h || !e || uint32_t(e - h) != bytes - 1u) return nullptr;
  return h;
}

// The colour argument as ClearImpl hands it down (r23 there, r10 into ClearSurface, r8 into the split, r6 into the
// emitter).
struct cl_Color {
  bool local;    // 821013FC..8210141C: pColor NULL -> ClearImpl's stack float4 (sp+0x60), every lane flt_820BCB98
  uint32_t va;   // else the caller's pointer
};

// Four colour words at va (for the local: the four 0.0f lanes). false = stopped.
bool cl_colorWords(Prod& p, const cl_Color& c, uint32_t va, uint32_t w[4]) {
  for (uint32_t k = 0; k < 4u; k++) {
    if (c.local) {
      w[k] = cl_kFZero;
      continue;
    }
    if (!p.g32(va + 4u * k, w[k])) {
      p.stop("clearF: colour unmapped");
      return false;
    }
  }
  return true;
}

// The byte 0x2A38 test ClearF (82101AA0..82101B64, 82101B80..82101C24), 821072F8 (82107338..821073DC) and 820F0208
// (820F0240..820F02E4) share: 0x10 -> tiled; else 0x20 -> tiled when each of RT0..RT3 / DS (0x2F88..0x2F98) equals
// its tiling slot (0x30A4..0x30B4) or is NULL; else not tiled.
bool cl_tiled(const Prod& p) {
  const uint8_t b = p.r8(0x2A38u);
  if (b & 0x10u) return true;
  if (!(b & 0x20u)) return false;
  for (uint32_t k = 0; k < 5u; k++) {
    const uint32_t s = p.r32(0x2F88u + 4u * k);
    if (p.r32(0x30A4u + 4u * k) != s && s != 0u) return false;
  }
  return true;
}

// D3DDeviceEmitClearRectVertices 82100A28(dev, r4 n, r5 rects, r6 pColor, f1 Z, r8 surfInfo, r9 colorInfo,
// r10 depthControl, [caller sp+0x54] modeControl): one RECTLIST draw of n rects. false = stopped; an allocation
// failure skips this draw only (true).
bool cl_emit(Prod& p, uint32_t n, const int32_t (*rects)[4], const cl_Color& col, double z, uint32_t surfInfo,
             uint32_t colorInfo, uint32_t depthCtl, uint32_t mode) {
#pragma clang fp contract(off)
  if (n > 4u) {  // the split hands at most four (never expected)
    p.stop("clearF: emitter rect count > 4");
    return false;
  }
  // 82100A54..82100A78: the pixel-centre offset: dev+0x2940 bit 0 set -> flt_820BCB98 (0.0f), clear ->
  // flt_820B5184 (0.5f).
  const float o = cl_f((p.r32(0x2940u) & 1u) ? cl_kFZero : cl_kFHalf);
  // 82100A7C..82100A94: kick check.
  if (!p.kick()) return false;
  // 82100A98..82100AB8: GpuCmdBufAllocate 820FCDF0(dev, 21n, 4); 0 -> stw r30,0x28(dev) and return (this draw only).
  // A stopped allocator also returns 0.
  const uint32_t dw = n * 21u;  // mulli r27,r25,0x15
  const uint32_t va = p.alloc(dw, 4u);
  if (p.stopped) return false;
  if (!va) return true;
  // 82100ABC..82100AE0: the GPU address: (va & 0x1FFFFFFF) + (((va >> 20) + 0x200) & 0x1000).
  const uint32_t phys = (va & 0x1FFFFFFFu) + (((va >> 20) + 0x200u) & 0x1000u);
  // 82100AC0..82100AD4: lfs f0, f13, f12, f11 = pColor[0..3] (stored back with stfs below).
  uint32_t c[4];
  if (!cl_colorWords(p, col, col.va, c)) return false;
  for (uint32_t k = 0; k < 4u; k++) c[k] = cl_lfsStfs(c[k]);
  const uint32_t zb = cl_u(float(z));  // stfs f30
  // 82100AE8..82100BB0: per rect, three vertices {x, y, z, r, g, b, a}: (L,T), (R,T), (R,B), each coordinate
  // lwa ; fcfid ; frsp ; fsubs o.
  uint8_t buf[4u * 21u * 4u];
  for (uint32_t i = 0; i < n; i++) {
    const float fl = float(double(rects[i][0]));  // f8  (lwa -8(r11))
    const float ft = float(double(rects[i][1]));  // f10 (lwa -4(r11))
    const float fr = float(double(rects[i][2]));  // f9  (lwa 0(r11))
    const float fb = float(double(rects[i][3]));  // f7  (lwa 4(r11))
    const uint32_t xl = cl_u(float(double(fl) - double(o)));
    const uint32_t yt = cl_u(float(double(ft) - double(o)));
    const uint32_t xr = cl_u(float(double(fr) - double(o)));
    const uint32_t yb = cl_u(float(double(fb) - double(o)));
    const uint32_t w[21] = {xl, yt, zb, c[0], c[1], c[2], c[3],   // +0x00..+0x18
                            xr, yt, zb, c[0], c[1], c[2], c[3],   // +0x1C..+0x34
                            xr, yb, zb, c[0], c[1], c[2], c[3]};  // +0x38..+0x50
    for (uint32_t k = 0; k < 21u; k++) cl_be32(buf + 4u * (21u * i + k), w[k]);
  }
  p.gwBytes(va, buf, 4u * dw);
  if (p.stopped) return false;
  // 82100BB4..82100C30: vertex fetch constant 0 {00054800, addr|3, 0x10000002 | (21n << 2), 0, 0, 0, 0}
  // (insrwi r10,r27,24,6), then {00025000, 0, 0, 0}, then {00012000, surfInfo, colorInfo}.
  p.t0(0x4800u, {phys | 3u, 0x10000002u | ((dw & 0x00FFFFFFu) << 2), 0u, 0u, 0u, 0u});
  p.t0(0x5000u, {0u, 0u, 0u});
  p.t0(0x2000u, {surfInfo, colorInfo});
  // 82100C18 / 82100C40..82100C68: {00002301, MSAA (surfInfo bits 16..17): 0 -> 0, 1 -> 0x8001, else 0xC003}.
  const uint32_t msaa = (surfInfo >> 16) & 3u;  // extrwi. r10,r29,2,14
  p.t0(0x2301u, {msaa == 0u ? 0u : (msaa == 1u ? 0x8001u : 0xC003u)});
  // 82100C6C..82100CAC: {00002200, depthControl}, {00002208, modeControl}, DRAW_INDX_2 predicated (C0003601)
  // {(n * 0x30000 & 0xFFFF0000) | 0x88} (rlwimi r6,0x11,3,16,31: auto-index, RECTLIST, 3n vertices).
  p.t0(0x2200u, {depthCtl});
  p.t0(0x2208u, {mode});
  p.t3(0x36u, true, {((n * 0x30000u) & 0xFFFF0000u) | 0x88u});
  // 82100CB4..82100D08: dirty bits.
  uint64_t q10 = p.r64(0x10u);
  q10 |= 1ull << 43;  // extldi r12,1,64,43
  p.w64(0x10u, q10);
  uint64_t q18 = p.r64(0x18u);
  q18 |= 1ull << 38;  // extldi r12,1,64,38
  q18 |= 1ull << 37;  // extldi r12,1,64,37
  p.w64(0x18u, q18);
  p.w64(0x20u, p.r64(0x20u) | (1ull << 36));  // extldi r12,1,64,36
  q10 = p.r64(0x10u);
  q10 |= 0x800ull;  // ori 0x800
  q10 |= 0x8ull;    // ori 8
  p.w64(0x10u, q10);
  return true;
}

// D3DDeviceClearRectTileSplit 82100D20(dev, r4 L, r5 T, r6 R, r7 B, r8 pColor, r10 surfInfo, f1 Z,
// [caller sp+0x54] colorInfo, +0x5C depthControl1, +0x64 depthControl2, +0x6C mode1, +0x74 mode2). false = stopped.
bool cl_split(Prod& p, int32_t L, int32_t T, int32_t R, int32_t B, const cl_Color& col, uint32_t surfInfo, double z,
              uint32_t colorInfo, uint32_t dc1, uint32_t dc2, uint32_t mode1, uint32_t mode2) {
  // 82100D4C..82100D6C: r29 = the x shift (1 for 1X / 2X, 0 for 4X), r24 = the y shift (1 for 1X only).
  const uint32_t msaa = (surfInfo >> 16) & 3u;  // extrwi. r11,r25,2,14
  uint32_t sx = 1u, sy;
  if (msaa == 0u) {
    sy = 1u;
  } else {
    sy = 0u;
    if (msaa != 1u) sx = 0u;
  }
  // 82100D70..82100DBC: tile 0x28 << r29 by 8 << r24; the tile-aligned interior, unsigned divisions (divwu).
  const uint32_t tw = 0x28u << sx;
  const uint32_t th = 8u << sy;
  const uint32_t r30 = (uint32_t(R) / tw) * tw;              // right, floored
  const uint32_t r31 = ((tw + uint32_t(L) - 1u) / tw) * tw;  // left, ceiled
  const uint32_t r23 = (uint32_t(B) / th) * th;              // bottom, floored
  const uint32_t r22 = ((th + uint32_t(T) - 1u) / th) * th;  // top, ceiled
  // 82100DC0..82100DCC -> 82100F28..82100F60: no interior -> one draw of the whole rect (depthControl1 / mode1).
  if (int32_t(r31) >= int32_t(r30) || int32_t(r22) >= int32_t(r23)) {
    const int32_t one[1][4] = {{L, T, R, B}};
    return cl_emit(p, 1u, one, col, z, surfInfo, colorInfo, dc1, mode1);
  }
  // 82100DD0..82100E7C: up to four strips around the interior (signed compares).
  int32_t strips[4][4];
  uint32_t n = 0;
  if (int32_t(r31) > L) {  // left, full height
    strips[n][0] = L, strips[n][1] = T, strips[n][2] = int32_t(r31), strips[n][3] = B;
    n++;
  }
  if (int32_t(r30) < R) {  // right, full height
    strips[n][0] = int32_t(r30), strips[n][1] = T, strips[n][2] = R, strips[n][3] = B;
    n++;
  }
  if (int32_t(r22) > T) {  // top, interior width
    strips[n][0] = int32_t(r31), strips[n][1] = T, strips[n][2] = int32_t(r30), strips[n][3] = int32_t(r22);
    n++;
  }
  if (int32_t(r23) < B) {  // bottom, interior width
    strips[n][0] = int32_t(r31), strips[n][1] = int32_t(r23), strips[n][2] = int32_t(r30), strips[n][3] = B;
    n++;
  }
  // 82100E80..82100EB0: the strips in one draw (depthControl1 / mode1, surfInfo unchanged).
  if (n != 0u && !cl_emit(p, n, strips, col, z, surfInfo, colorInfo, dc1, mode1)) return false;
  // 82100EB4..82100F20: the interior through a 4X view: x >> r29, y >> r24 (sraw); RB_SURFACE_INFO MSAA = 2
  // (rlwimi r25,1,17,14,15), pitch (bits 0..13) >> r29 (clrlwi 18 ; srw ; rlwimi 0,0,17), the bits 18..31 field
  // >> r29 (srwi 18 ; srw ; insrwi 14,0); depthControl2 / mode2.
  const int32_t in[1][4] = {{int32_t(r31) >> sx, int32_t(r22) >> sy, int32_t(r30) >> sx, int32_t(r23) >> sy}};
  const uint32_t s = (surfInfo & ~0x00030000u) | 0x00020000u;
  uint32_t v = ((s & 0x3FFFu) >> sx) | (s & 0xFFFFC000u);
  const uint32_t hi = (v >> 18) >> sx;
  v = (v & 0x0003FFFFu) | ((hi << 18) & 0xFFFC0000u);
  return cl_emit(p, 1u, in, col, z, v, colorInfo, dc2, mode2);
}

// SurfaceClearColorPackByFormat 8210C090(r3 fmt, v1 = the colour as lvx128 loaded it): the 32-bit packed colour
// (r5 out; the r6 out is a dead stack word in ClearSurface). The Resolve port's rs_colorPack is the one implementation
// (nb_rec_resolve.inc: every format the spliced body packs, the VMX float semantics made independent of the host
// MXCSR); it stops on the formats it cannot pack (6, 8/9/11/13, a non-finite 7e3 colour). false = stopped.
bool cl_packColor(Prod& p, uint32_t fmt, const uint32_t cw[4], uint32_t& lo) {
  uint32_t hi = 0u;
  return rs_colorPack(p, fmt, cw, lo, hi);
}

// D3DDeviceClearSurface 82100F70(dev, r4 Flags, r5 idx (-1 = no colour target), r6 L, r7 T, r8 R, r9 B, r10 pColor,
// f1 Z, [caller sp+0x5C] Stencil). false = stopped.
bool cl_clearSurface(Prod& p, uint32_t flags, int32_t idx, int32_t L, int32_t T, int32_t R, int32_t B,
                     const cl_Color& col, double z, uint32_t stencil) {
#pragma clang fp contract(off)
  // 82100F98: r14 = the RB_SURFACE_INFO shadow (dev+0x2800).
  const uint32_t surfInfo = p.r32(0x2800u);
  // 82100FAC..82101004: idx -1 -> colorInfo 0, mode 5; else mode 4, colorInfo = [RT(idx) + 0x1C] with the format
  // field (bits 16..19) 0xA -> 2 (rlwimi r28,1,17,12,15) and 0xC -> 3 (insrwi r28,3,4,12).
  uint32_t colorInfo = 0u, mode1 = 5u;
  if (idx != -1) {
    mode1 = 4u;
    const uint32_t rt = p.r32(0x2F88u + 4u * uint32_t(idx));  // lwzx r11,(idx + 0xBE2) << 2,dev
    if (!p.g32(rt + 0x1Cu, colorInfo)) {
      p.stop("clearF: render target unmapped");
      return false;
    }
    if ((colorInfo & 0x000F0000u) == 0x000A0000u) colorInfo = (colorInfo & ~0x000F0000u) | 0x00020000u;
    if ((colorInfo & 0x000F0000u) == 0x000C0000u) colorInfo = (colorInfo & ~0x000F0000u) | 0x00030000u;
  }
  // 82101008..82101020: hiz = (dev+0x28C0 & ~0xF) | 1 if Z; depthControl1 = 0x76 if Z.
  uint32_t hiz = p.r32(0x28C0u) & ~0xFu;
  uint32_t dc1 = 0u;
  if (flags & 0x10u) {
    dc1 = 0x76u;
    hiz |= 1u;
  }
  // 82101024..82101070: Stencil: depthControl1 |= 0x8701, hiz |= 4; kick check; {0000210D, 0xFF0000 | Stencil & 0xFF}
  // (rlwimi r10,0xFF,16,0,23); q18 |= 0x200.
  if (flags & 0x20u) {
    dc1 |= 0x8701u;
    hiz |= 4u;
    if (!p.kick()) return false;  // 82101030..8210104C
    p.t0(0x210Du, {0x00FF0000u | (stencil & 0xFFu)});
    p.w64(0x18u, p.r64(0x18u) | 0x200ull);
  }
  // 82101074..82101084: 821072F8(dev, 0, hiz, 0) (its return value is discarded here).
  if (!hizWrite(p, hiz)) {
    p.stop("clearF: 821072F8 declined (tiled)");
    return false;
  }
  // 82101088..821010A0: kick check.
  if (!p.kick()) return false;
  // 821010A4..821010D4: {00022080, 0, 0, 0x20002000} (the screen scissor wide open).
  p.t0(0x2080u, {0u, 0u, 0x20002000u});
  uint32_t dc2 = dc1, mode2 = mode1;  // 821010C4 mr r26,r19 ; 821010C8 mr r27,r16
  // 821010B8..82101138: colour as depth - only when neither Z nor Stencil is cleared, the format (after the remap) is
  // not 5 / 7 / 15 (64bpp), and the rect is at least one tile: R-L >= 80/80/40, B-T >= 16/8/8 (1X/2X/4X), signed.
  if ((flags & 0x30u) == 0u) {
    const uint32_t fmt = (colorInfo >> 16) & 0xFu;  // extrwi r3,r28,4,12
    if (fmt != 5u && fmt != 7u && fmt != 0xFu) {
      const uint32_t msaa = (surfInfo >> 16) & 3u;  // extrwi. r11,r14,2,14
      uint32_t tw, th;
      if (msaa == 0u) {
        tw = 0x50u;
        th = 0x10u;
      } else {
        th = 8u;
        tw = (msaa == 1u) ? 0x50u : 0x28u;
      }
      if (int32_t(uint32_t(R) - uint32_t(L)) >= int32_t(tw) && int32_t(uint32_t(B) - uint32_t(T)) >= int32_t(th)) {
        // 8210113C..8210114C: lvx128 v1,r0,r17 (16-byte aligned: [pColor & ~0xF]) ; 8210C090(fmt, ...).
        uint32_t cw[4];
        if (!cl_colorWords(p, col, col.va & ~0xFu, cw)) return false;
        uint32_t lo = 0u;
        if (!cl_packColor(p, fmt, cw, lo)) return false;
        // 82101150..821011CC: {00002203, 0}, {00002002, colorInfo & 0xFFF}, {0000210D, 0xFF0000 | lo & 0xFF}.
        p.t0(0x2203u, {0u});
        p.t0(0x2002u, {colorInfo & 0xFFFu});
        p.t0(0x210Du, {0x00FF0000u | (lo & 0xFFu)});
        // 82101178..821011DC: Z = frsp((fcfid(lo >> 8) - dbl_820BD3A0) * dbl_820BD398) (fsub / fmul in double).
        double d = double(int64_t(uint64_t(lo >> 8)));
        d = d - cl_d(cl_kDSixteenth);
        d = d * cl_d(cl_kDDepthScale);
        z = double(float(d));
        dc2 = 0x8777u;  // ori r26,0,0x8777
        mode2 = 5u;     // li r27,5
        // 821011D0..821011F4: q10 |= 0x100; q18 |= 1 << 36, then 0x200.
        p.w64(0x10u, p.r64(0x10u) | 0x100ull);
        uint64_t q18 = p.r64(0x18u);
        q18 |= 1ull << 36;  // extldi r12,1,64,36
        q18 |= 0x200ull;
        p.w64(0x18u, q18);
      }
    }
  }
  // 821011F8..82101204: byte 0x2A38 & 0x20 -> the per-tile loop 82101244 (declined).
  if (p.r8(0x2A38u) & 0x20u) {
    p.stop("clearF: ClearSurface per-tile loop (0x2A38 & 0x20)");
    return false;
  }
  // 82101208..8210123C: 82100D20(dev, L, T, R, B, pColor, surfInfo, Z, colorInfo, dc1, dc2, mode1, mode2).
  return cl_split(p, L, T, R, B, col, surfInfo, z, colorInfo, dc1, dc2, mode1, mode2);
}

// The rect argument of ClearImpl: the caller's pointer (read at 82101444..821014A4) or ClearF's stack rect.
struct cl_RectSrc {
  bool guest = false;
  uint32_t va = 0;
  int32_t v[4] = {0, 0, 0, 0};
};

// D3DDeviceClearImpl 821013D0(dev, r4 Flags, r5 pRect, r6 pColor, f1 Z, r8 Stencil, r9 &{ox, oy}).
void cl_clearImpl(Prod& p, uint32_t flags, const cl_RectSrc& rs, const cl_Color& col, double z, uint32_t stencil,
                  int32_t ox, int32_t oy, const uint8_t* vsUcode, const uint8_t* psUcode) {
  // 82101420..82101430: no DS bound -> Flags &= ~0x30 (rlwinm. 0,28,25); nothing left -> return. Only the low word
  // matters (the recompiled cr0 is the 32-bit compare).
  if (p.r32(0x2F98u) == 0u) {
    flags &= ~0x30u;
    if (flags == 0u) return;
  }
  // 82101434..82101440: memcpy(sp+0x70, dev+0x3058, 0x18) - the viewport X, Y, W, H (the last two dwords unread).
  const int32_t vpX = int32_t(p.r32(0x3058u)), vpY = int32_t(p.r32(0x305Cu));
  const int32_t vpW = int32_t(p.r32(0x3060u)), vpH = int32_t(p.r32(0x3064u));
  // 82101444..821014B8: the rect clamped to the viewport moved by {ox, oy} (signed compares).
  int32_t rect[4];
  if (rs.guest) {
    for (uint32_t k = 0; k < 4u; k++) {
      uint32_t u = 0u;
      if (!p.g32(rs.va + 4u * k, u)) {
        p.stop("clearF: rect unmapped");
        return;
      }
      rect[k] = int32_t(u);
    }
  } else {
    for (uint32_t k = 0; k < 4u; k++) rect[k] = rs.v[k];
  }
  const int32_t x0 = cl_add(ox, vpX), y0 = cl_add(oy, vpY);
  const int32_t x1 = cl_add(vpW, x0), y1 = cl_add(vpH, y0);
  int32_t L = (rect[0] > x0) ? rect[0] : x0;  // cmpw ; bgt
  int32_t T = (rect[1] > y0) ? rect[1] : y0;
  int32_t R = (rect[2] < x1) ? rect[2] : x1;  // cmpw ; blt
  int32_t B = (rect[3] < y1) ? rect[3] : y1;
  // 821014BC..82101540: dev+0x2D40 != 0 (cmpwi) -> also clamp to the scissor rect at dev+0x3070, moved by {ox, oy}.
  if (int32_t(p.r32(0x2D40u)) != 0) {
    const int32_t sL = cl_add(ox, int32_t(p.r32(0x3070u))), sT = cl_add(oy, int32_t(p.r32(0x3074u)));
    const int32_t sR = cl_add(ox, int32_t(p.r32(0x3078u))), sB = cl_add(oy, int32_t(p.r32(0x307Cu)));
    if (!(L > sL)) L = sL;
    if (!(T > sT)) T = sT;
    if (!(R < sR)) R = sR;
    if (!(B < sB)) B = sB;
  }
  // 82101544..82101550: empty -> return (nothing written).
  if (R <= L || B <= T) return;

  // 82101554..821016FC: the dirty flush (= Resolve's 8210CA58 sequence).
  if (!phaseC(p)) {
    p.stop("clearF: phaseC declined");
    return;
  }
  // 82101700..82101718: kick check.
  if (!p.kick()) return;
  // 8210171C..82101764: INVALIDATE_STATE (C0003B00) {0x300}; IM_LOAD_IMMEDIATE (C01C2B00) {0, 0x1B} + the 27 VS
  // dwords memcpy'd from 0x820BA0D0.
  p.t3(0x3Bu, false, {0x300u});
  p.t3n(0x2Bu, false, {0u, cl_kVsUcodeDwords}, cl_kVsUcodeDwords, vsUcode);
  // 82101768..82101780: kick check.
  if (!p.kick()) return;
  // 82101784..821017B0: IM_LOAD_IMMEDIATE (C00A2B00) {1, 9} + the 9 PS dwords memcpy'd from 0x820B59DC.
  p.t3n(0x2Bu, false, {1u, cl_kPsUcodeDwords}, cl_kPsUcodeDwords, psUcode);
  // 821017B4..821017D4: {00012180, 0x10010001, 0}.
  p.t0(0x2180u, {0x10010001u, 0u});
  // 821017D8..821017F0: kick check.
  if (!p.kick()) return;
  // 821017F4..821017FC: GpuDefaultStateBlockInit 8210C518 (no kick check inside).
  defaultBlock(p);
  if (p.stopped) return;
  // 82101800..82101824: q10 |= 1 << 51, then 1 << 52.
  uint64_t q10 = p.r64(0x10u);
  q10 |= 1ull << 51;  // extldi r12,1,64,51
  q10 |= 1ull << 52;  // extldi r12,1,64,52
  p.w64(0x10u, q10);

  if ((flags & 0xFu) == 0u) {
    // 82101834..82101860: no colour target requested -> ClearSurface(dev, Flags, -1, L, T, R, B, pColor, Z, Stencil).
    if (!cl_clearSurface(p, flags, -1, L, T, R, B, col, z, stencil)) return;
  } else {
    // 82101864..82101878: kick check.
    if (!p.kick()) return;
    // 8210187C..821018BC: {00002104, 0xFFFF}, {00002202, 0}, {00002201, 0x10001}.
    p.t0(0x2104u, {0xFFFFu});
    p.t0(0x2202u, {0u});
    p.t0(0x2201u, {0x00010001u});
    // 821018C0..821018DC: q18 |= 0x40000 (oris 4); q10 |= 0x200, then 0x400.
    p.w64(0x18u, p.r64(0x18u) | 0x40000ull);
    q10 = p.r64(0x10u);
    q10 |= 0x200ull;
    q10 |= 0x400ull;
    p.w64(0x10u, q10);
    // 821018E0..82101938: targets 0..3 requested and bound (dev+0x2F88 + 4i) -> ClearSurface(i); the depth / stencil
    // clear rides on the first one (Flags &= ~0x30 after it, rlwinm r25,r25,0,28,25).
    for (uint32_t i = 0; i < 4u; i++) {
      if (!(flags & (1u << i))) continue;
      if (p.r32(0x2F88u + 4u * i) == 0u) continue;
      if (!cl_clearSurface(p, flags, int32_t(i), L, T, R, B, col, z, stencil)) return;
      flags &= ~0x30u;
    }
  }

  // 8210193C..82101968: restore the window scissor from the shadow: 820F0208(dev, sext15(0x2844 bits 0..14),
  // sext15(0x2844 bits 16..30), sext15(0x2848 bits 0..14), sext15(0x2848 bits 16..30)). Its tiled branch (the 2A38
  // test, after its kick check) is declined here, before the kick.
  if (cl_tiled(p)) {
    p.stop("clearF: 820F0208 tiled restore");
    return;
  }
  const uint32_t s44 = p.r32(0x2844u), s48 = p.r32(0x2848u);
  if (!windowScissor(p, cl_sra17(s44 << 17), cl_sra17(s44 << 1), cl_sra17(s48 << 17), cl_sra17(s48 << 1))) {
    p.stop("clearF: 820F0208 declined");
    return;
  }
  // 8210196C..82101988: byte 0x2A39 |= 4; q10 |= 1 << 49.
  p.w8(0x2A39u, uint8_t(p.r8(0x2A39u) | 4u));
  p.w64(0x10u, p.r64(0x10u) | (1ull << 49));  // extldi r12,1,64,49
}

}  // namespace

// D3DDeviceClearF 82101A58 (r3 dev, r4 Flags, r5 pRect, r6 pColor, f1 Z, r8 Stencil; r7 is never read).
void portClearF(Prod& p) {
  // The IM_LOAD_IMMEDIATE ucode (.rdata; the XDK memcpy's it). Checked before anything is written.
  const uint8_t* vsUcode = cl_rdata(cl_kVsUcodeVA, 4u * cl_kVsUcodeDwords);
  const uint8_t* psUcode = cl_rdata(cl_kPsUcodeVA, 4u * cl_kPsUcodeDwords);
  if (!vsUcode || !psUcode) {
    p.stop("clearF: ucode unmapped");
    return;
  }

  // 82101A68..82101A84: r22 = Flags, f31 = Z, r26 = pRect, r31 = dev, r21 = pColor, r20 = Stencil,
  // r19 = Flags & 0xF.
  const uint32_t flags = p.ctx->r4.u32;
  const uint32_t pRect = p.ctx->r5.u32;
  const uint32_t pColor = p.ctx->r6.u32;
  const double z = p.ctx->f1.f64;
  const uint32_t stencil = p.ctx->r8.u32;
  const uint32_t r19 = flags & 0xFu;

  // Tiling (declined): ClearF's per-tile split (82101C2C..82101DC4), ClearSurface's per-tile loop (82101200),
  // 821072F8's tiled 0x2203 and 820F0208's tiled restore all need byte 0x2A38 & 0x30 (the 0x2A3B & 0x20 tests only
  // matter together with cl_tiled, which needs 0x2A38 & 0x30) -> stop before anything is written (no kick for a call
  // the guest body will redo). The literal checks stay at their own sites below.
  if (p.r8(0x2A38u) & 0x30u) {
    p.stop("clearF: tiling active (0x2A38 & 0x30)");
    return;
  }

  cl_RectSrc rs;
  if (pRect == 0u) {
    // 82101A90..82101A9C: the surface: RT0 (dev+0x2F88), else DS (dev+0x2F98).
    const uint32_t rt0 = p.r32(0x2F88u);
    const uint32_t surf = rt0 ? rt0 : p.r32(0x2F98u);
    if (surf == 0u) {
      p.stop("clearF: NULL rect with no RT0 and no DS");
      return;
    }
    uint32_t s24 = 0u;
    if (!p.g32(surf + 0x24u, s24)) {
      p.stop("clearF: surface unmapped");
      return;
    }
    // 82101AA4..82101AC4: {0, 0, (s24 >> 18) + 1, ((s24 >> 3) & 0x7FFF) + 1} (srwi 18 ; extrwi 15,14).
    rs.v[0] = 0;
    rs.v[1] = 0;
    rs.v[2] = int32_t((s24 >> 18) + 1u);
    rs.v[3] = int32_t(((s24 >> 3) & 0x7FFFu) + 1u);
    // 82101AC8..82101B78: tiled -> R, B from dev+0x3264 / dev+0x3268.
    if (cl_tiled(p)) {
      rs.v[2] = int32_t(p.r32(0x3264u));
      rs.v[3] = int32_t(p.r32(0x3268u));
    }
  } else {
    rs.guest = true;
    rs.va = pRect;
  }

  // 82101B80..82101C3C: tiled and byte 0x2A3B & 0x20 and a colour target requested -> the per-tile split (declined).
  if (cl_tiled(p) && (p.r8(0x2A3Bu) & 0x20u) && r19 != 0u) {
    p.stop("clearF: per-tile split");
    return;
  }

  // 82101DD0..82101DF8: ClearImpl(dev, Flags, pRect, pColor, Z, Stencil, &{0, 0}).
  const cl_Color col{pColor == 0u, pColor};
  cl_clearImpl(p, flags, rs, col, z, stencil, 0, 0, vsUcode, psUcode);
}
