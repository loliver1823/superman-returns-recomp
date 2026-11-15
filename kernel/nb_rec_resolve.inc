// kernel/nb_rec_resolve.inc - iter-467 native-b, P1 row 9: the byte-exact port of D3DDeviceResolve (XDK, guest
// 8210C5F8) for the record interface of kernel/native_rec_b.hpp.
//
// #included by native_rec_b.cpp INSIDE `namespace sr::native::recb { ... }` (after ppc_context.h and
// kernel/guest_context.hpp). Defines portResolve(Prod&); every helper is rs_-prefixed in an anonymous namespace.
//
// SOURCE OF TRUTH: the guest asm (IDA, default.xex) of 8210C5F8 and its callees 8210BF48, 8210C440, 820FEFF8
// (+ 820FE9B0 / 820FE518 / 820FE370 / 820F3E48 / 820FEA48). 8210C090 + 8210BEA0 (the VMX colour pack) are ported
// from the RECOMPILED bodies that actually run (8210C090 is a spliced body, tools/regen/splice_bodies.json), because
// the CHECK compares with them: lanes in guest order, vmulfp/vaddfp/vmaddfp as mul-then-add with FTZ+DAZ, vrfin
// round-to-nearest-even, vrfiz truncate, vctsxs = cvttps with NaN -> 0 and +overflow -> INT_MAX. Host MXCSR is NOT
// trusted (a guest VMX op may have left FTZ/DAZ on): denormal handling is done explicitly below.
//
// ARGUMENTS (prologue: savegprlr_14 / stfd f30,f31 / stwu r1,-0x170 run AFTER entry, so the stack args are read
// from the CALLER's r1 = ctx->r1 at entry; the asm's 0x170+arg_NN(r1) == caller r1 + NN):
//   r3 dev, r4 Flags, r5 pSrcRect, r6 pDestTex, r7 pDestPoint (NULL -> .rdata 820BB3C0 {0,0}), r8 DestLevel,
//   r9 DestSlice, r10 pClearColor (NULL -> a stack {0.0 x4}), f1 ClearZ,
//   [r1+0x5C] ClearStencil (lwz: the FULL word is used - OR'd whole into RB_DEPTH_CLEAR on the unorm path),
//   [r1+0x64] pParameters (lwz; +0 colour D3DFORMAT, +4 exp bias (read, unused by 8210C090), +8 depth D3DFORMAT).
//   The body spills pDestTex into its home slot (stw r19,0x170+arg_2C = caller r1+0x2C): reproduced with gw32.
//
// p.stop() SITES (each returns immediately; the guest body then runs):
//   - any unmapped guest read (texture fetch constant / header, source surface descriptor, rect, dest point,
//     clear colour, pParameters, caller stack args, .rdata tables)                                 "resolve: unmapped"
//   - 820FEFF8 path: bpp 0 (the XDK's twllei divide trap at 8210C780 / 820FE590), or a DestLevel >= 2^31 (the
//     negative level counter reads the uninitialised var_A8 at 820FF3CC)
//   - 8210CC04: the predicated-tiling bin loop (byte 2A38 & 0x10, or & 0x20 with the five surfaces matching)
//   - colour clear (Flags & 0x100): caller r1 not 16-byte aligned (lvx/stvx stack layout of 8210C090 changes),
//     EDRAM colour format 6 (16_16_FLOAT: vpkd3d128 type 3 = FLOAT16_2, but the spliced 8210C090 packs it as
//     D3DCOLOR - see below), formats 8/9/11/13 (8210C420 reads an uninitialised stack word), formats 3/12 with a
//     non-finite R/G/B (the CRT ldexp 827A2FB0 takes its _handle_qnan/_except path)
//   - 8210D3C0: the 335C list segment is full (820FAC80 chains a new segment)
//   - the IM_LOAD_IMMEDIATE ucode (820B5374) has no host pointer
// Framework helpers (phaseC / hizWrite / windowScissor / kick / alloc) stop by returning false (alloc: 0).

namespace {

// ---- PPC integer semantics ------------------------------------------------------------------------------------
inline uint32_t rs_rotl32(uint32_t v, uint32_t n) {
  n &= 31u;
  return n ? ((v << n) | (v >> (32u - n))) : v;
}
inline uint32_t rs_clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32u; }  // cntlzw
// slw / srw: shift = low 6 bits of rB, >= 32 gives 0 (recomp: `n.u8 & 0x20 ? 0 : x << (n.u8 & 0x3F)`).
inline uint32_t rs_slw(uint32_t v, uint32_t n) { return (n & 0x20u) ? 0u : (v << (n & 0x1Fu)); }
inline uint32_t rs_srw(uint32_t v, uint32_t n) { return (n & 0x20u) ? 0u : (v >> (n & 0x1Fu)); }
inline int32_t rs_sx15lo(uint32_t v) { return (int32_t)(v << 17) >> 17; }  // slwi 17; srawi 17
inline int32_t rs_sx15hi(uint32_t v) { return (int32_t)(v << 1) >> 17; }   // slwi 1;  srawi 17
// The XDK's GPU physical address of a CPU VA: ((va >> 20) + 0x200) & 0x1000 + (va & 0x1FFFFFFF).
inline uint32_t rs_phys(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }
inline void rs_be32(uint8_t* d, uint32_t v) {
  d[0] = (uint8_t)(v >> 24);
  d[1] = (uint8_t)(v >> 16);
  d[2] = (uint8_t)(v >> 8);
  d[3] = (uint8_t)v;
}

// ---- float semantics, independent of the host MXCSR flush bits ------------------------------------------------
inline float rs_f(uint32_t b) { return __builtin_bit_cast(float, b); }
inline uint32_t rs_u(float f) { return __builtin_bit_cast(uint32_t, f); }

// lfs: single -> double, exact (a denormal single is a normal double; done by hand so host DAZ cannot flush it).
inline double rs_f32_to_f64(uint32_t b) {
  if ((b & 0x7F800000u) == 0 && (b & 0x007FFFFFu) != 0) {
    const double v = (double)(int32_t)(b & 0x007FFFFFu) * 0x1p-149;
    return (b & 0x80000000u) ? -v : v;
  }
  return (double)rs_f(b);
}
// frsp / stfs: double -> single bits, round-to-nearest-even, denormal results kept (FTZ off, as the recomp's scalar
// FP runs with disableFlushMode). The normal range uses the host conversion (FTZ cannot apply there).
inline uint32_t rs_f64_to_f32_bits(double d) {
  const uint64_t b = __builtin_bit_cast(uint64_t, d);
  const uint32_t s = (uint32_t)(b >> 32) & 0x80000000u;
  const int32_t e = (int32_t)((b >> 52) & 0x7FFu);
  if (e == 0x7FF || e == 0) return e == 0 ? s : rs_u((float)d);  // inf/NaN; zero or double-denormal -> +-0
  const int32_t ue = e - 1023;
  if (ue >= -126) return rs_u((float)d);  // a normal single (or overflow to inf)
  const uint64_t sig = (b & 0xFFFFFFFFFFFFFull) | (1ull << 52);  // value = sig * 2^(ue - 52)
  const int32_t shift = -97 - ue;  // in units of 2^-149 the value is sig >> shift; ue <= -127 -> shift >= 30
  if (shift >= 64) return s;
  uint64_t q = sig >> shift;
  const uint64_t rem = sig & ((1ull << shift) - 1u), half = 1ull << (shift - 1);
  if (rem > half || (rem == half && (q & 1u))) ++q;
  return s | (uint32_t)q;  // q == 0x800000 encodes the smallest normal, as it should
}
// fctidz as the recomp emits it: d > double(LLONG_MAX) -> LLONG_MAX, else cvttsd2si (NaN / out of range -> MIN).
inline int64_t rs_fctidz(double d) {
  if (d > 0x1p63) return (int64_t)0x7FFFFFFFFFFFFFFFull;
  if (d != d || d >= 0x1p63 || d < -0x1p63) return (int64_t)0x8000000000000000ull;
  return (int64_t)d;
}

// VMX (FTZ + DAZ, as enableFlushMode sets MXCSR before every vector float op in the recomp).
inline float rs_daz(float x) {
  const uint32_t b = rs_u(x);
  return ((b & 0x7F800000u) == 0) ? rs_f(b & 0x80000000u) : x;
}
inline float rs_vmul(float a, float b) { return rs_daz(rs_daz(a) * rs_daz(b)); }
inline float rs_vadd(float a, float b) { return rs_daz(rs_daz(a) + rs_daz(b)); }
inline float rs_vmax(float a, float b) {  // MAXPS(a, b): a > b ? a : b (NaN -> b)
  a = rs_daz(a);
  b = rs_daz(b);
  return (a > b) ? a : b;
}
inline float rs_vmin(float a, float b) {  // MINPS(a, b): a < b ? a : b (NaN -> b)
  a = rs_daz(a);
  b = rs_daz(b);
  return (a < b) ? a : b;
}
inline bool rs_vcmpgt(float a, float b) { return rs_daz(a) > rs_daz(b); }
inline float rs_vrfin(float x) {  // roundps nearest-even; sign of a zero result kept
  x = rs_daz(x);
  if (!(x < 8388608.0f && x > -8388608.0f)) return x;  // NaN, inf, already integral
  int32_t i = (int32_t)x;
  const float fr = x - (float)i;  // exact
  if (x >= 0.0f) {
    if (fr > 0.5f || (fr == 0.5f && (i & 1))) ++i;
  } else {
    if (fr < -0.5f || (fr == -0.5f && (i & 1))) --i;
  }
  if (i == 0) return (rs_u(x) & 0x80000000u) ? -0.0f : 0.0f;
  return (float)i;
}
inline float rs_vrfiz(float x) {  // roundps toward zero
  x = rs_daz(x);
  if (!(x < 8388608.0f && x > -8388608.0f)) return x;
  const int32_t i = (int32_t)x;
  if (i == 0) return (rs_u(x) & 0x80000000u) ? -0.0f : 0.0f;
  return (float)i;
}
inline int32_t rs_vctsxs(float x) {  // simde_mm_vctsxs (ppc_context.h)
  x = rs_daz(x);
  if (x != x) return 0;
  if (x >= 2147483648.0f) return 0x7FFFFFFF;
  if (x < -2147483648.0f) return (int32_t)0x80000000u;
  return (int32_t)x;
}

// ---- guest reads that stop on unmapped ------------------------------------------------------------------------
inline bool rs_g32(Prod& p, uint32_t va, uint32_t& v) {
  if (p.g32(va, v)) return true;
  p.stop("resolve: unmapped");
  return false;
}
inline bool rs_g16(Prod& p, uint32_t va, uint16_t& v) {
  if (p.g16(va, v)) return true;
  p.stop("resolve: unmapped");
  return false;
}
inline bool rs_g8(Prod& p, uint32_t va, uint8_t& v) {
  if (p.g8(va, v)) return true;
  p.stop("resolve: unmapped");
  return false;
}

constexpr uint32_t kRsFmtTable = 0x820B3890u;  // u16 per D3D format: bits 3..7 bytes/texel, low byte bits/texel

// The predicated-tiling test the XDK inlines at 8210CC04 / 8210CED0 (and 820F0208 / 821072F8): byte 2A38 & 0x10, or
// byte 2A38 & 0x20 with each of the five surface slots 2F88..2F98 equal to its twin at 30A4..30B4 or NULL.
bool rs_tiled(const Prod& p) {
  const uint8_t b = p.r8(0x2A38);
  if (b & 0x10u) return true;
  if (!(b & 0x20u)) return false;
  for (uint32_t i = 0; i < 5; ++i) {
    const uint32_t s = p.r32(0x2F88u + 4u * i), t = p.r32(0x30A4u + 4u * i);
    if (t != s && s != 0) return false;
  }
  return true;
}

// ---- 820FEFF8 TextureDescMipChainLayout and its helpers (pure) ------------------------------------------------
// 820FE370 TextureFormatBlockDims: (block width, block height) of a D3D texture format.
void rs_blockDims(uint32_t f, uint32_t& bw, uint32_t& bh) {
  if (f > 0x2Cu) {
    if (f == 0x31u) { bw = bh = 4; return; }
    if (f <= 0x32u) { bw = bh = 1; return; }
    if (f <= 0x35u) { bw = bh = 4; return; }
    if (f <= 0x39u) { bw = bh = 1; return; }
    if (f <= 0x3Du) { bw = bh = 4; return; }
    bw = bh = 1;
    return;
  }
  if (f == 0x2Cu) { bw = 4; bh = 1; return; }
  if (f < 0x0Bu) { bw = bh = 1; return; }
  if (f <= 0x0Cu) { bw = 2; bh = 1; return; }
  if (f <= 0x11u) { bw = bh = 1; return; }
  if (f <= 0x14u) { bw = bh = 4; return; }
  if (f == 0x27u) { bw = 4; bh = 1; return; }
  if (f == 0x28u) { bw = 2; bh = 1; return; }
  bw = bh = 1;
}

// 820FE9B0 TextureFetchConstantDimensions: (W, H, D) incl. the border, by dimension (fc5 bits 9..10).
void rs_fcDims(uint32_t fc2, uint32_t fc3, uint32_t fc5, uint32_t& W, uint32_t& H, uint32_t& D) {
  const uint32_t dim = (fc5 >> 9) & 3u;
  const uint32_t b = (rs_rotl32(fc3, 2) & 2u) + 1u;  // rlwinm r11,r9,2,30,30; addi 1
  if (dim == 0) {
    W = (fc2 & 0x00FFFFFFu) + b;
    H = 1;
    D = 1;
  } else if (dim == 2) {
    W = (fc2 & 0x7FFu) + b;
    H = ((fc2 >> 11) & 0x7FFu) + b;
    D = (fc2 >> 22) + b;
  } else {
    W = (fc2 & 0x1FFFu) + b;
    H = ((fc2 >> 13) & 0x1FFFu) + b;
    D = 1;
  }
}

// 820FE518 TextureSurfaceSize: aligns (W, H, D) in place (the byte size it returns is not used by 820FEFF8).
bool rs_surfSize(Prod& p, uint32_t& W, uint32_t& H, uint32_t& D, uint32_t bppBits, uint32_t fmt, uint32_t dim,
                 uint32_t tiled) {
  uint32_t r25 = 0x20u;
  const uint32_t r24 = (dim == 0 ? 0u : 31u) + 1u;  // subfic/subfe/clrlwi 27/addi 1
  const uint32_t r27 = (dim == 2) ? 4u : 1u;
  uint32_t bw, bh;
  rs_blockDims(fmt, bw, bh);
  if (tiled == 0) {  // 820FE578 cmpwi r23,0; bne
    const uint32_t bpb = (bw * bh * bppBits) >> 3;
    if (bpb == 0) {  // 820FE590 twllei
      p.stop("resolve: 820FE518 divide trap");
      return false;
    }
    const uint32_t q = 0x100u / bpb;
    if (q >= 0x20u) r25 = q;
  }
  const uint32_t aw = bw * r25, ah = bh * r24;
  W = (W + aw - 1u) & ~(aw - 1u);
  H = (H + ah - 1u) & ~(ah - 1u);
  D = (D + r27 - 1u) & ~(r27 - 1u);
  return true;
}

// 820F3E48 TextureResourceTypeClassify.
bool rs_classify(Prod& p, uint32_t tex, uint32_t fc1, uint32_t fc5, uint32_t& out) {
  uint32_t h;
  if (!rs_g32(p, tex, h)) return false;
  const uint32_t t = h & 0xFu;
  out = t;
  if (t == 3) {
    const uint32_t dim = (fc5 >> 9) & 3u;
    if (dim == 3) out = 0x12;
    else if (dim == 2) out = 0x11;
    else if (dim == 0) out = 0x14;
    else if (fc1 & 0x400u) out = 0x13;
  } else if (t == 4) {
    if (h & 0x40000000u) {
      uint32_t q, v;
      if (!rs_g32(p, tex + 0x18u, q) || !rs_g32(p, q + 0x30u, v)) return false;
      if ((v & 0x600u) == 0x400u) out = 0x10;
    }
  }
  return true;
}

// 820FEA48 TexturePackedMipTailSurfaceSize: the byte offset of a level inside the packed mip tail.
bool rs_packedTail(Prod& p, uint32_t lvl, uint32_t W, uint32_t H, uint32_t D, uint32_t pitchB, uint32_t sliceB,
                   uint32_t fmt, uint32_t& off) {
  uint8_t bpp;  // r24
  if (!rs_g8(p, kRsFmtTable + 1u + fmt * 2u, bpp)) return false;
  uint32_t xy[2] = {0, 0};  // var_60 (x), var_5C (y)
  uint32_t z = 0;           // var_58
  uint32_t bw, bh;          // var_70, var_6C
  rs_blockDims(fmt, bw, bh);
  const uint32_t lw = 0x20u - rs_clz32(W - 1u), lh = 0x20u - rs_clz32(H - 1u), ld = 0x20u - rs_clz32(D - 1u);
  const uint32_t p2[2] = {rs_slw(1u, lw), rs_slw(1u, lh)};  // var_68, var_64
  uint32_t i = (lh < lw) ? 1u : 0u;                          // subfc/subfe/clrlwi 31 (then << 2)
  if (lvl < 3u) {
    xy[i] = (uint32_t)(0x10 >> lvl);  // sraw
  } else {
    i ^= 1u;
    const uint32_t v = rs_srw(p2[i], lvl - 2u);
    xy[i] = v;
    if (v < 4u) {
      uint32_t t = ld - lvl;
      if (t <= 1u) t = 1u;
      z = t << 2;
    }
  }
  off = ((bw * bpp * xy[0]) >> 3) + xy[1] * pitchB + z * sliceB;
  return true;
}

// 820FEFF8 TextureDescMipChainLayout - only the outputs D3DDeviceResolve reads: pitch in bytes (var_EC), width
// (var_FC), height (var_100) and base (var_F8). Called with slice = DestSlice & ~(depthFactor - 1).
bool rs_mipLayout(Prod& p, uint32_t tex, uint32_t slice, uint32_t level, uint32_t& oPitch, uint32_t& oW,
                  uint32_t& oH, uint32_t& oBase) {
  uint32_t fc0, fc1, fc2, fc3, fc5;
  if (!rs_g32(p, tex + 0x28u, fc3) || !rs_g32(p, tex + 0x20u, fc1) || !rs_g32(p, tex + 0x1Cu, fc0) ||
      !rs_g32(p, tex + 0x30u, fc5) || !rs_g32(p, tex + 0x24u, fc2))
    return false;
  const uint32_t border = fc3 >> 31;     // r31
  const uint32_t fmt = fc1 & 0x3Fu;      // r21
  const uint32_t tiled = fc0 >> 31;      // r18
  const uint32_t dim = (fc5 >> 9) & 3u;  // r20
  uint8_t bppBits;                       // r17
  if (!rs_g8(p, kRsFmtTable + 1u + fmt * 2u, bppBits)) return false;
  uint32_t vB0, vB4, vB8;  // 820FF070 820FE9B0(tex, &var_B0, &var_B4, &var_B8)
  rs_fcDims(fc2, fc3, fc5, vB0, vB4, vB8);
  uint32_t r14 = vB4;
  const uint32_t r8 = border << 1;
  const uint32_t r7 = vB0 - r8, r6 = vB4 - r8;
  const uint32_t r9 = rs_slw(1u, border - rs_clz32(r7 - 1u) + 0x20u);
  uint32_t r22 = rs_slw(1u, border - rs_clz32(r6 - 1u) + 0x20u);
  uint32_t r4 = (dim == 2) ? rs_slw(1u, border - rs_clz32(vB8 - r8 - 1u) + 0x20u) : 1u;
  const bool packed = (r9 <= 0x10u || r22 <= 0x10u) && (fc5 & 0x800u);  // 820FF0DC..F0F8 (r5)
  uint32_t r15 = (level == 0 || packed) ? (fc1 & ~0xFFFu) : (fc5 & ~0xFFFu);
  uint32_t r31 = 0;  // pitch in bytes
  if (level == 0 && !packed) {
    // 820FF124: level 0, not packed.
    uint32_t W = vB0, H = r14, D = vB8;  // var_B4, var_BC, var_C0
    if (!rs_surfSize(p, W, H, D, bppBits, fmt, dim, tiled)) return false;
    r22 = H;
    if (tiled == 0 && !(fc1 & 0x400u) && !(fc5 & 0x800u) && dim == 1) {
      uint32_t fc4;
      if (!rs_g32(p, tex + 0x2Cu, fc4)) return false;
      if (!(fc4 & 0x3C0u) && border == 0) {  // 820FF194 820FE370(fmt, &var_A8, &var_BC)
        uint32_t bw, bh;
        rs_blockDims(fmt, bw, bh);
        r22 = (bh + r14 - 1u) & ~(bh - 1u);
      }
    }
    r4 = D;
    if (dim == 0) r31 = (W * bppBits) >> 3;
    else r31 = ((((fc0 >> 22) & 0x1FFu) * bppBits) & 0x07FFFFFFu) << 2;  // extrwi 9,1; mullw; clrlslwi 5,2
  } else {
    // 820FF1E0: level > 0 or packed.
    uint32_t t = rs_srw(r7, level);
    if (t <= 1u) t = 1u;
    vB0 = t + r8;
    t = rs_srw(r6, level);
    if (t <= 1u) t = 1u;
    r14 = t + r8;
    if (dim == 2) {
      t = rs_srw(vB8 - r8, level);
      if (t <= 1u) t = 1u;
      vB8 = t + r8;
    } else {
      vB8 = 1;
    }
    uint32_t cls;  // 820FF240
    if (!rs_classify(p, tex, fc1, fc5, cls)) return false;
    uint32_t r19 = 1;
    if (cls == 0x12u || cls == 0x13u) {
      const uint32_t a = rs_slw(1u, rs_rotl32(fc1, 23) & 2u);
      r19 = ((fc2 >> 26) + a) & ~(a - 1u);
    }
    uint32_t r24 = 0x1Fu - rs_clz32(r9), r25 = 0x1Fu - rs_clz32(r22), r26 = 0x1Fu - rs_clz32(r4);
    int32_t r23 = (int32_t)level;
    if (packed) {
      ++r24;
      ++r25;
      ++r26;
    } else {
      r23 -= 1;
    }
    if (r23 < 0) {  // 820FF3CC lwz r31, var_A8 (never written on this path)
      p.stop("resolve: 820FEFF8 negative level counter");
      return false;
    }
    for (;;) {  // 820FF2B4
      if (r24) --r24;
      if (r25) --r25;
      if (r26) --r26;
      const uint32_t W0 = rs_slw(1u, r24), H0 = rs_slw(1u, r25);  // r28, r27
      uint32_t W = W0, H = H0, D = rs_slw(1u, r26);              // var_B4, var_BC, var_C0
      if (!rs_surfSize(p, W, H, D, bppBits, fmt, dim, tiled)) return false;
      r31 = (W * bppBits) >> 3;
      r22 = H;
      r4 = D;
      if ((W0 <= 0x10u || H0 <= 0x10u) && (fc5 & 0x800u)) {  // 820FF384: the packed tail
        uint32_t off;
        if (!rs_packedTail(p, (uint32_t)r23, rs_slw(1u, r24), rs_slw(1u, r25), rs_slw(1u, r26), r31, r31 * r22, fmt,
                           off))
          return false;
        r15 += off;
        break;
      }
      if (r23 > 0) {  // 820FF340 ble
        uint32_t sz;
        if (dim != 2) sz = ((r31 * r22 + 0xFFFu) & ~0xFFFu) * r4;
        else sz = (r31 * r4 * r22 + 0xFFFu) & ~0xFFFu;
        r15 += sz * r19;
      }
      if (--r23 < 0) break;  // addic. r23,r23,-1; bge
    }
  }
  // 820FF3D0
  oBase = (((r31 * r22 + 0xFFFu) & ~0xFFFu) * slice) + r15;
  oPitch = r31;
  oW = vB0;
  oH = r14;
  return true;
}

// ---- 8210BF48 D3DDeviceSetDepthStencilClearValue ---------------------------------------------------------------
bool rs_depthClear(Prod& p, uint32_t pParams, double z, uint32_t stencil) {
  bool flt;
  if (pParams == 0) {
    flt = (p.r16(0x2808) & 1u) != 0;  // RB_DEPTH_INFO shadow: D24FS8
  } else {
    uint32_t f;
    uint16_t e;
    if (!rs_g32(p, pParams + 8u, f) || !rs_g16(p, kRsFmtTable + ((f & 0x3Fu) << 1), e)) return false;
    flt = (e & 0xF00u) != 0;  // rlwinm. 0,20,23
  }
  uint32_t r11, r10;
  if (!flt) {
    // fmuls (double(float(z * K))) + fctidz + stfiwx; K = 16777215.0f (820BD320), 2047.0f (820BD31C)
    const uint32_t d24 = (uint32_t)rs_fctidz(rs_f32_to_f64(rs_f64_to_f32_bits(z * 16777215.0)));
    r10 = (uint32_t)rs_fctidz(rs_f32_to_f64(rs_f64_to_f32_bits(z * 2047.0)));
    r11 = (d24 << 8) | stencil;
  } else {
    // 8210BFE4: the 20e4 float depth encode
    const uint32_t bits = rs_f64_to_f32_bits(z);  // stfs f1
    uint32_t v;
    if (bits & 0x80000000u) {
      v = 0;
    } else if ((bits >> 23) >= 0x71u) {
      v = ((bits & 0x07800000u) | (bits + 4u)) >> 3;
    } else {
      const uint32_t sh = 0x71u - ((bits >> 23) & 0xFFu);
      v = rs_srw((rs_slw(1u, sh - 1u) + (bits & 0x7FFFFFu)) | 0x00800000u, sh);
    }
    r11 = (rs_rotl32(v, 8) & 0xFFFFFF00u) | (stencil & 0xFFu);  // insrwi r11,r10,24,0 (r11 = stencil)
    r10 = r11 >> 21;
  }
  // 8210C04C
  const uint32_t b2 = p.r8(0x28C2), d28c0 = p.r32(0x28C0);
  p.w32(0x29AC, r11);  // RB_DEPTH_CLEAR
  const uint32_t r9 = rs_rotl32(rs_clz32(stencil - b2), 6) & 0x800u;
  const uint32_t r8 = rs_rotl32(~d28c0, 6) & 0x800u;
  p.w32(0x29A8, ((r9 ^ 0x800u) ^ r8) | r10);
  p.w64(0x20, p.r64(0x20) | 0x300u);
  return true;
}

// ---- 8210BEA0 VmxPiecewiseTruncSelectEval (the 8_8_8_8_GAMMA PWL), lanes x/y/z; w kept (vrlimi128 v13,v1,1,0) ----
void rs_gamma(float c[4]) {
  const float k = rs_f(0x3B808081u);                                                   // 820BB350.w  1/255
  const float t0 = rs_f(0x3D802008u), t1 = rs_f(0x3E002008u), t2 = rs_f(0x3F002008u);  // 820BB350.xyz
  const float o1 = rs_f(0x3E008081u), o2 = rs_f(0x3E808081u), o3 = rs_f(0x3F008081u);  // 820BB370.yzw
  for (int i = 0; i < 3; ++i) {
    const float x = c[i];
    const float v31 = rs_vmul(x, 1023.0f), m9 = rs_vmul(x, 511.0f);  // 820BB360 {1023, 511, 255, 127}
    const float m8 = rs_vmul(x, 255.0f), m0 = rs_vmul(x, 127.0f);
    const bool s13 = rs_vcmpgt(t0, x), s10 = rs_vcmpgt(t2, x), s11 = rs_vcmpgt(t1, x);
    const float v7 = rs_vmul(rs_vrfiz(v31), k);
    const float v9 = rs_vadd(rs_vmul(rs_vrfiz(m9), k), o1);  // vmaddfp as the recomp emits it: v9*v12 + v4
    const float v8 = rs_vadd(rs_vmul(rs_vrfiz(m8), k), o2);
    const float v0 = rs_vadd(rs_vmul(rs_vrfiz(m0), k), o3);
    const float v10 = s10 ? v8 : v0;
    const float v11 = s11 ? v9 : v10;
    c[i] = s13 ? v7 : v11;
  }
}

// The recomp's vpkd3d128 FLOAT16_4 lane (XenonRecomp recompiler.cpp case 5): truncating, |x| > 65504 / NaN -> 0x7FFF.
uint32_t rs_half(uint32_t u) {
  const float a = rs_f(u & 0x7FFFFFFFu);
  const uint32_t e = ((a != a) || (a > 65504.0f)) ? 0xFFu : ((u & 0x7F800000u) >> 23);
  const uint32_t m = (e != 0xFFu) ? ((u & 0x7FE000u) >> 13) : 0u;
  uint32_t h = (e != 0xFFu) ? (e > 0x70u ? (((e - 0x70u) << 10) + m) : ((0x71u - e) > 31u ? 0u : ((0x400u + m) >> (0x71u - e))))
                            : 0x7FFFu;
  h = (h & 0xFFFFu) | ((u & 0x80000000u) >> 16);
  return h & 0xFFFFu;
}

// ---- 8210C090 SurfaceClearColorPackByFormat (spliced body): (EDRAM colour format, clear colour as four guest-order
// float bit patterns) -> lo (r10 -> *r5 -> 0x29B0) and hi (r11 -> *r6 -> 0x29B4). Needs a 16-aligned stack (checked
// by the caller).
bool rs_colorPack(Prod& p, uint32_t fmt, const uint32_t col[4], uint32_t& lo, uint32_t& hi) {
  switch (fmt) {
    case 0:
    case 1:
    case 2:
    case 10: {
      // 8210C0EC (fmt 1: bl 8210BEA0) / 8210C0F0: unorm (8_8_8_8, 2_10_10_10, AS_10_10_10_10)
      float c[4] = {rs_f(col[0]), rs_f(col[1]), rs_f(col[2]), rs_f(col[3])};
      if (fmt == 1) rs_gamma(c);
      const uint32_t f2 = (fmt == 10) ? 2u : fmt;
      uint8_t A, B;  // 820BB384[f] alpha bits, 820BB380[f] colour bits
      if (!rs_g8(p, 0x820BB384u + f2, A) || !rs_g8(p, 0x820BB380u + f2, B)) return false;
      // extsw/std/lfd/fcfid/frsp, then fsubs 1.0 (820BCBA4)
      const float fa = rs_f(rs_f64_to_f32_bits(rs_f32_to_f64(rs_f64_to_f32_bits((double)(int32_t)rs_slw(1u, A))) - 1.0));
      const float fb = rs_f(rs_f64_to_f32_bits(rs_f32_to_f64(rs_f64_to_f32_bits((double)(int32_t)rs_slw(1u, B))) - 1.0));
      const float scale[4] = {fb, fb, fb, fa};  // stfs 80/84/88 = fb, 92 = fa; lvx128 v12
      uint32_t e[4];
      for (int i = 0; i < 4; ++i) {
        float v = rs_vmul(c[i], scale[i]);  // vmulfp128 v0,v1,v12
        v = rs_vmax(0.0f, v);               // vmaxfp v0,v13(0),v0
        v = rs_vmin(scale[i], v);           // vminfp v0,v12,v0
        v = rs_vrfin(v);
        e[i] = (uint32_t)rs_vctsxs(v);
      }
      const uint32_t r9 = (uint32_t)B * 3u, r8 = rs_rotl32(B, 1);
      uint32_t r11 = rs_slw(e[1], B) | (rs_slw(e[3], r9) | rs_slw(e[2], r8));
      r11 |= e[0];  // 8210C1C8
      lo = hi = r11;
      return true;
    }
    case 3:
    case 12: {
      // 8210C2B4: 2_10_10_10_FLOAT (7e3): R/G/B through CRT ldexp(x, -124) + frsp + rebias, A = 2-bit unorm
      uint32_t t[3];
      for (int i = 0; i < 3; ++i) {
        const uint32_t b = col[i];
        if ((b & 0x7F800000u) == 0x7F800000u) {
          p.stop("resolve: 7e3 clear colour not finite (CRT ldexp special path)");
          return false;
        }
        const double y = rs_f32_to_f64(b) * 0x1p-124;  // ldexp: exact for a finite single (exponent stays normal)
        uint32_t r10 = rs_f64_to_f32_bits(y) + 0x8000u;  // frsp; stfsx; lwzx; addis 1; addi -0x8000
        if (r10 & 0x7C000000u) r10 = (rs_f(r10) < 0.0f) ? 0x3FFu : 0x1FFu;  // fcmpu vs f31 = 0.0 (820BCB98)
        else r10 = rs_rotl32(r10, 16) & 0x3FFu;
        t[i] = r10;
      }
      // alpha: fmuls by 3.0 (820BD210), clamp [0, 3], fadds 0.5 (820B5184), fctidz
      double f0 = rs_f32_to_f64(rs_f64_to_f32_bits(rs_f32_to_f64(col[3]) * 3.0));
      const double f13 = (f0 > 0.0) ? f0 : 0.0;
      if (!(f13 < 3.0)) f0 = 3.0;
      else if (!(f0 > 0.0)) f0 = 0.0;
      f0 = rs_f32_to_f64(rs_f64_to_f32_bits(f0 + 0.5));
      const uint32_t a = (uint32_t)rs_fctidz(f0);
      uint32_t r11 = rs_rotl32(a, 10) & 0xFFFFFC00u;
      r11 |= t[2];
      r11 = rs_rotl32(r11, 10) & 0xFFFFFC00u;
      r11 |= t[1];
      r11 = rs_rotl32(r11, 10) & 0xFFFFFC00u;
      r11 |= t[0];  // 8210C1C8
      lo = hi = r11;
      return true;
    }
    case 4:
    case 5: {
      // 8210C1D8: 16_16 / 16_16_16_16 (signed fixed, range +-32)
      uint32_t e[4];
      for (int i = 0; i < 4; ++i) {
        float v = rs_vmul(rs_f(col[i]), 0.03125f);  // 820BD300
        v = rs_vmax(-1.0f, v);                       // 820BD0D0
        v = rs_vmin(1.0f, v);                        // 820BCBA4
        v = rs_vadd(rs_vmul(v, 32767.0f), 32768.0f);  // vmaddfp v0,v0,v12(820BB390),v13(820BB3A0)
        v = rs_vrfin(v);
        e[i] = (uint32_t)rs_vctsxs(v) ^ 0x8000u;  // vxor 820BB3B0
      }
      uint32_t r11 = rs_rotl32(e[1], 16) & 0xFFFF0000u;
      r11 |= e[0];
      if (fmt == 4) {
        lo = hi = r11;
      } else {
        hi = r11;
        lo = (rs_rotl32(e[3], 16) & 0xFFFF0000u) | e[2];
      }
      return true;
    }
    case 7: {
      // 8210C3C8: 16_16_16_16_FLOAT, vpkd3d128 v0,v1,5,2,2 (FLOAT16_4) then halfwords 80/82/84/86
      const uint32_t h0 = rs_half(col[0]), h1 = rs_half(col[1]), h2 = rs_half(col[2]), h3 = rs_half(col[3]);
      hi = (h1 << 16) | h0;
      lo = (h3 << 16) | h2;
      return true;
    }
    case 14:  // 8210C40C: 32_FLOAT, the raw x
      lo = hi = col[0];
      return true;
    case 15:  // 8210C414: 32_32_FLOAT, r11 = x, r10 = y
      hi = col[0];
      lo = col[1];
      return true;
    case 6: {
      // 8210C3A4: vpkd3d128 v0,v1,3,1,3 is type 3 = FLOAT16_2 on hardware and in Xenia (word 0x180D0ED0: IMM 13 ->
      // type 3, shift 1, pack 3). XenonRecomp has no type-3 lowering; the hand splice (tools/regen/splice_bodies.json
      // sub_8210C090) packs Xenia's float_to_xenos_half pair (research/p2 d20953cd, L6 difftest 43/43 against Xenia;
      // baked) unless SR_SPLICE_VPK_F16=0 (the old D3DCOLOR body: every normal colour packs to 0). The port follows the
      // body that runs through THE SAME predicate the splice calls, sr::kernel::spliceVpkF16().
      // FLOAT16_2: packed element 3 = half(x) << 16 | half(y) (x = col[0], y = col[1]); D3DCOLOR: per host lane i
      // (= guest element 3 - i) clamp to [3.0, 0x404000FF] (a NaN passes), the low byte; temp = e3 << 24 | e2 |
      // e1 << 8 | e0 << 16. Then vspltw / stvewx r1+80 / lhz 82, lhz 80 / rlwimi swap the halfwords; 8210C1D0: lo = hi.
      uint32_t temp;
      if (::sr::kernel::spliceVpkF16()) {
        auto xenosHalf = [](uint32_t v) -> uint32_t {  // the splice's lambda, verbatim semantics
          const uint32_t a = v & 0x7FFFFFFFu;
          uint32_t r;
          if (a >= 0x47FFE000u) {
            r = 0x7FFFu;
          } else {
            r = a < 0x38800000u ? 0u : a + 0xC8000000u;
            r = (r >> 13) & 0x7FFFu;
          }
          return r | ((v & 0x80000000u) >> 16);
        };
        temp = (xenosHalf(col[0]) << 16) | xenosHalf(col[1]);
      } else {
        const float mx = rs_f(0x404000FFu);
        uint32_t bytes[4];
        for (int i = 0; i < 4; ++i) {
          const float f = rs_f(col[3 - i]);  // host lane i
          const float v = (f < 3.0f) ? 3.0f : ((f > mx) ? mx : f);
          bytes[i] = rs_u(v) & 0xFFu;
        }
        temp = (bytes[0] << 24) | bytes[1] | (bytes[2] << 8) | (bytes[3] << 16);
      }
      lo = hi = rs_rotl32(temp, 16);
      g_fmt6Packs.add();
      if ((col[0] | col[1] | col[2] | col[3]) & 0x7FFFFFFFu) g_fmt6NonZero.add();
      return true;
    }
    default:
      // 8, 9, 11, 13 (> 15 cannot occur: the format is a 4-bit field): 8210C420 reads the uninitialised var_50.
      p.stop("resolve: colour clear format reads uninitialised stack");
      return false;
  }
}

// ---- 8210C440 D3DDeviceSetClearColorValue (r5 = the source index, r4 = pParameters, v1 = the colour) -----------
bool rs_colorClear(Prod& p, uint32_t pParams, uint32_t idx, const uint32_t col[4]) {
  const uint32_t info = p.r32(idx == 0 ? 0x2804u : (idx + 0xA02u) * 4u);
  const uint32_t cfmt = (info >> 16) & 0xFu;  // extrwi r3,r11,4,12
  uint32_t lo, hi;
  if (pParams == 0) {
    if (!rs_colorPack(p, cfmt, col, lo, hi)) return false;
  } else {
    // 8210C490
    uint32_t p0, p1;
    uint8_t t33c, r31;
    uint16_t e;
    if (!rs_g32(p, pParams, p0) || !rs_g8(p, 0x820BB33Cu + cfmt, t33c)) return false;
    if (!rs_g8(p, kRsFmtTable + 1u + (uint32_t)t33c * 2u, r31)) return false;  // rotlwi r9,r9,1; lbzx 820B3891
    uint32_t r11 = p0 & 0x3Fu;
    if (r11 == 0x36u) r11 = 7;
    if (!rs_g32(p, pParams + 4u, p1)) return false;  // lwz r4,4(r4): the exp bias, unused by 8210C090
    if (!rs_g16(p, kRsFmtTable + r11 * 2u, e)) return false;
    const uint32_t r3 = ((uint32_t)e >> 8) & 0xFu, r30 = (uint32_t)e & 0xFFu;
    if (!rs_colorPack(p, r3, col, lo, hi)) return false;
    if ((uint32_t)r31 > r30) hi = lo;  // 8210C4E4
  }
  p.w32(0x29B0, lo);  // RB_COLOR_CLEAR
  p.w32(0x29B4, hi);  // RB_COLOR_CLEAR_LO
  p.w64(0x20, p.r64(0x20) | 0xC0u);
  return true;
}

}  // namespace

// ================================================================================================================
// D3DDeviceResolve 8210C5F8
void portResolve(Prod& p) {
  PPCContext& c = *p.ctx;
  const uint32_t sp = c.r1.u32;  // the CALLER's r1 (the body's stwu has not run)
  const uint32_t flags0 = c.r4.u32;
  const uint32_t pRect = c.r5.u32;
  const uint32_t tex = c.r6.u32;
  uint32_t pPoint = c.r7.u32;
  const uint32_t level = c.r8.u32;
  const uint32_t slice = c.r9.u32;
  const uint32_t pColor = c.r10.u32;
  const double clearZ = c.f1.f64;

  // 8210C628 stw r19,0x170+arg_2C(r1): pDestTex into its home slot in the caller's frame.
  p.gw32(sp + 0x2Cu, tex);

  // 8210C618..8210C688: the MSAA fixup of Flags when (Flags & 0x70) == 0, from the source surface's +0x18.
  const uint32_t idx = flags0 & 7u;  // var_F4
  uint32_t flags = flags0;           // r25
  if ((flags & 0x70u) == 0) {
    const uint32_t surf = p.r32(idx == 4 ? 0x2F98u : (idx + 0xBE2u) * 4u);
    uint16_t ms;
    if (!rs_g16(p, surf + 0x18u, ms)) return;
    const uint32_t m = ms & 3u;
    flags |= (m == 0) ? 0x10u : (m == 1) ? 0x50u : 0x70u;
  }

  // 8210C6B0..8210C728: the destination fetch constant (tex+0x1C..0x30).
  uint32_t fc0, fc1, fc2, fc3, fc5;
  if (!rs_g32(p, tex + 0x30u, fc5) || !rs_g32(p, tex + 0x28u, fc3) || !rs_g32(p, tex + 0x24u, fc2) ||
      !rs_g32(p, tex + 0x20u, fc1) || !rs_g32(p, tex + 0x1Cu, fc0))
    return;
  const uint32_t dim = (fc5 >> 9) & 3u;                  // r23
  const uint32_t bord = (rs_rotl32(fc3, 2) & 2u) + 1u;  // 1 + 2*border
  uint32_t wm1, hm1, depthF;                             // depthF = r20 / var_F0
  if (dim == 2) {
    wm1 = fc2 & 0x7FFu;
    depthF = 8;
    hm1 = (fc2 >> 11) & 0x7FFu;
  } else {
    wm1 = fc2 & 0x1FFFu;
    depthF = 1;
    hm1 = (fc2 >> 13) & 0x1FFFu;
  }
  uint32_t height = hm1 + bord;  // r3 / var_100
  uint32_t width = wm1 + bord;   // var_FC
  const uint32_t fmt = fc1 & 0x3Fu;  // r29
  uint16_t fe;
  if (!rs_g16(p, kRsFmtTable + fmt * 2u, fe)) return;
  uint32_t base = fc1 & ~0xFFFu;                     // var_F8
  uint32_t pitch = rs_rotl32(fc0, 15) & 0x3FE0u;     // r18: rlwinm r18,r30,15,18,26
  const uint32_t bpp = ((uint32_t)fe >> 3) & 0x1Fu;  // r15: extrwi r15,r11,5,24

  // 8210C730..8210C788: a mip level or slice -> 820FEFF8.
  if (level != 0 || slice != 0) {
    uint32_t pb, w2, h2, b2;
    if (!rs_mipLayout(p, tex, slice & ~(depthF - 1u), level, pb, w2, h2, b2)) return;
    width = w2;
    height = h2;
    base = b2;
    if (bpp == 0) {  // 8210C780 twllei 6,r15,0
      p.stop("resolve: bpp 0 divide trap");
      return;
    }
    pitch = pb / bpp;  // divwu r18,r11,r15
  }

  // 8210C78C..8210C7C0: the source rect (NULL -> {0, 0, width, height} on the stack) and the dest point.
  uint32_t rect[4];
  if (pRect == 0) {
    rect[0] = 0;
    rect[1] = 0;
    rect[2] = width;
    rect[3] = height;
  } else {
    for (uint32_t i = 0; i < 4; ++i)
      if (!rs_g32(p, pRect + 4u * i, rect[i])) return;
  }
  if (pPoint == 0) pPoint = 0x820BB3C0u;
  uint32_t dx, dy;
  if (!rs_g32(p, pPoint, dx) || !rs_g32(p, pPoint + 4u, dy)) return;

  // 8210C7C4..8210C824: RB_COPY_DEST_BASE.
  const uint32_t signX = (fc0 >> 2) & 3u;  // r19
  const uint32_t r14 = height - dy;
  const uint32_t destBase = ((dy - rect[1]) * pitch + ((dx - rect[0]) << 5)) * bpp + (((base >> 20) + 0x200u) & 0x1000u) +
                            (base & 0x1FFFFFFFu);  // r17

  // 8210C820..8210C91C: RB_COPY_DEST_INFO / RB_COPY_DEST_PITCH.
  uint32_t f = fmt;
  switch (fmt) {
    case 0x36: f = 7; break;
    case 0x37: f = 0x10; break;
    case 0x38: f = 0x11; break;
    case 0x1B: f = 0x1E; break;
    case 0x1C: f = 0x1F; break;
    case 0x1D: f = 0x20; break;
    case 0x16:
    case 0x17: f = 6; break;
    default: break;
  }
  uint32_t num = (signX == 1) ? 1u : 0u;  // addi -1; cntlzw; extrwi 1,26
  if ((fc3 & 1u) == 1u) num += 2u;
  if (f == 0x1Eu || f == 0x1Fu || f == 0x20u || f == 0x24u || f == 0x25u || f == 0x26u || f == 0x39u || f == 0x3Fu)
    num = 7;
  const uint32_t swap = ((fc3 & 0xEu) != 0) ? 1u : 0u;              // cntlzw; extrwi 1,26; xori 1
  const uint32_t r6 = ((flags >> 26) & ~0x100u) | (swap << 8);       // insrwi r6,r4,1,23
  const uint32_t r10 = (num & 7u) | (rs_rotl32(r6 & 0x13Fu, 3) & 0xFFFFFFF8u);  // insrwi r10,r9,29,0
  uint32_t info = ((f & 0x3Fu) | (rs_rotl32(r10, 6) & 0xFFFFFFC0u)) << 7;      // insrwi r11,r10,26,0; slwi 7
  info |= (fc1 >> 6) & 3u;                                                      // endian
  const uint32_t destPitch = ((r14 & 0x3FFFu) << 16) | (pitch & 0x3FFFu);

  // 8210C920..8210C948: a 3D destination.
  if (dim == 2) {
    p.w32(0x29C4, pitch * height);  // RB_COPY_SURFACE_SLICE
    info |= ((slice & (depthF - 1u)) & 7u) << 4;
    p.w64(0x20, p.r64(0x20) | 4u);
    info |= 8u;
  }

  // 8210C94C..8210C9A0: RB_COPY_CONTROL and the four shadows, q20 |= 0x3C00.
  uint32_t cmd = 1;
  if (flags & 0x2000u) cmd = 3;
  else if (idx == 4) cmd = 0;
  p.w32(0x29A4, info);       // RB_COPY_DEST_INFO
  p.w32(0x299C, destBase);   // RB_COPY_DEST_BASE
  p.w32(0x29A0, destPitch);  // RB_COPY_DEST_PITCH
  const uint32_t copyCtl = ((cmd & 3u) << 20) | ((flags & 0x377u) - 0x10u);
  p.w32(0x2998, copyCtl);  // RB_COPY_CONTROL
  p.w64(0x20, p.r64(0x20) | 0x3C00u);

  // 8210C988 lwz r4,0x170+arg_64(r1): pParameters (loaded unconditionally).
  uint32_t pParams;
  if (!rs_g32(p, sp + 0x64u, pParams)) return;
  // 8210C9A4: Flags & 0x200 -> 8210BF48 (r6 = lwz 0x170+arg_5C, f1 = ClearZ).
  if (flags & 0x200u) {
    uint32_t stencil;
    if (!rs_g32(p, sp + 0x5Cu, stencil)) return;
    if (!rs_depthClear(p, pParams, clearZ, stencil)) return;
  }
  // 8210C9B8: Flags & 0x100 -> 8210C440 (r5 = RB_COPY_CONTROL & 7, v1 = lvx128 of pClearColor & ~0xF).
  if (flags & 0x100u) {
    if (sp & 0xFu) {
      p.stop("resolve: colour clear with a caller r1 not 16-byte aligned");
      return;
    }
    uint32_t col[4] = {0, 0, 0, 0};  // NULL: four stfs of f31 = 0.0f (820BCB98) at an aligned stack slot
    if (pColor != 0) {
      const uint32_t a = pColor & ~0xFu;
      for (uint32_t i = 0; i < 4; ++i)
        if (!rs_g32(p, a + 4u * i, col[i])) return;
    }
    if (!rs_colorClear(p, pParams, copyCtl & 7u, col)) return;
  }

  // 8210C9D0..8210CA54 (B0): the rect rounded up to 8; outside the window scissor -> 820F0208 with it.
  const int32_t left = (int32_t)rect[0], top = (int32_t)rect[1];
  const int32_t R8 = (int32_t)((rect[2] + 7u) & ~7u), B8 = (int32_t)((rect[3] + 7u) & ~7u);
  bool b0 = false;  // r20
  {
    const uint32_t tl = p.r32(0x2844), br = p.r32(0x2848);
    if (left < rs_sx15lo(tl) || top < rs_sx15hi(tl) || R8 > rs_sx15lo(br) || B8 > rs_sx15hi(br)) {
      if (!windowScissor(p, (uint32_t)left, (uint32_t)top, (uint32_t)R8, (uint32_t)B8)) return;
      b0 = true;
    }
  }

  // 8210CA58..8210CC00 (B1/B2): the dirty flush.
  if (!phaseC(p)) return;

  // 8210CC04..8210CD9C: predicated tiling -> per-bin SET_BIN_MASK / predicated RB_COPY_DEST_BASE loop. Not modelled.
  if (rs_tiled(p)) {
    p.stop("resolve: predicated tiling bin loop (8210CCB0)");
    return;
  }

  // 8210CDA0: kick.
  if (!p.kick()) return;

  // 8210CDBC..8210CFBC (B3).
  if (p.r8(0x2A3A) & 8u) {
    p.t3(0x46, false, {6u});
    p.t3(0x46, false, {6u});
    p.t3(0x46, false, {6u});
    p.w8(0x2A3A, (uint8_t)(p.r8(0x2A3A) & 0xF7u));
  }
  p.t3(0x3B, false, {0x100u});
  const uint8_t* ucode = sr::kernel::guestVAToHost(0x820B5374u);
  if (!ucode) {
    p.stop("resolve: no host pointer for the 820B5374 ucode");
    return;
  }
  p.t3n(0x2B, false, {0u, 15u}, 15, ucode);  // IM_LOAD_IMMEDIATE (0xC0102B00), 15 dwords memcpy'd from 820B5374
  p.t0(0x2180, {0x00010002u, 0u});
  p.t0(0x2208, {6u});
  p.t0(0x2200, {0u});
  uint32_t hiz = 0;  // r5
  if (flags & 0x200u) {
    // 8210CEA4..8210CF9C
    const uint8_t b3b = p.r8(0x2A3B), b3a = p.r8(0x2A3A);
    const uint32_t r9 = p.r32(0x28C0) & ~0xFu;
    const uint32_t r8 = ((uint32_t)b3a & ~8u) | (rs_rotl32(b3a, 30) & 8u);  // rlwimi r8,r10,30,28,28
    hiz = (((r8 >> 3) & 7u) & 0xFFFFFFFDu) | r9;                           // extrwi 3,26; rlwinm 0,31,29; or
    if ((b3b & 0x20u) && rs_tiled(p)) hiz &= ~1u;                          // 8210CF7C clrrwi r5,r5,1
    p.w8(0x2A39, (uint8_t)(p.r8(0x2A39) | 4u));
    p.w64(0x10, p.r64(0x10) | (1ull << 49));
  }
  p.t0(0x05C8, {0x20000u});
  // 8210CFC0: 821072F8(dev, 0, hiz, 1) - its return (0 & ~0x100) is discarded.
  if (!hizWrite(p, hiz)) return;
  // 8210CFC4: kick.
  if (!p.kick()) return;
  // 8210CFE4 (B5): 8210C518.
  defaultBlock(p);

  // 8210CFEC..8210D088 (B6): an sRGB (signX == 3) colour destination re-sends the source RB_COLOR*_INFO.
  if (signX == 3 && idx != 4) {
    const uint32_t r30 = (idx != 0) ? idx + 1u : 0u;
    const uint32_t ci = p.r32((r30 + 0xA01u) * 4u);
    if ((ci & 0x000F0000u) == 0x10000u) {
      if (!p.kick()) return;  // 8210D030
      p.t0(0x2001u + r30, {ci & 0x03F00FFFu});
      const uint32_t sh = (r30 + 0x1Au) & 0xFFu;  // (r30 + 0x3011A) & 0xFF; dword (r30 + 0x3011A) >> 16 << 3 = 0x18
      p.w64(0x18, p.r64(0x18) | (0x8000000000000000ull >> sh));
    }
  }

  // 8210D08C: kick, then 820FCDF0(dev, 6, 4); failure -> the XDK puts the cursor back and returns.
  if (!p.kick()) return;
  const uint32_t va = p.alloc(6, 4);
  if (va == 0) return;

  // 8210D0C8..8210D1AC (B7): the three copy vertices in the allocation (big-endian f32).
  const double o = (p.r32(0x2940) & 1u) ? 0.0 : 0.5;  // f31 (820BCB98) : 820B5184
  const double fl = (double)(float)(double)(int64_t)left;                                // fcfid; frsp
  const double ft = (double)(float)(double)(int64_t)top;
  const double fw = (double)(float)(double)(int64_t)(int32_t)((uint32_t)R8 - (uint32_t)left);
  const double fh = (double)(float)(double)(int64_t)(int32_t)((uint32_t)B8 - (uint32_t)top);
  const float x0 = (float)(fl - o);  // fsubs
  const float y0 = (float)(ft - o);
  const float x1 = (float)(fw + (double)x0);  // fadds
  const float y1 = (float)(fh + (double)y0);
  {
    uint8_t vb[24];
    rs_be32(vb + 0x00, rs_u(x0));
    rs_be32(vb + 0x04, rs_u(y0));
    rs_be32(vb + 0x08, rs_u(x1));
    rs_be32(vb + 0x0C, rs_u(y0));
    rs_be32(vb + 0x10, rs_u(x1));
    rs_be32(vb + 0x14, rs_u(y1));
    p.gwBytes(va, vb, 24);
  }
  // 8210D1B0..8210D1F8: vertex fetch constant 0, three zero constants at 0x5000, DRAW_INDX_2 (predicated).
  p.t0(0x4800, {rs_phys(va) | 3u, 0x1000001Au, 0u, 0u, 0u, 0u});
  p.t0(0x5000, {0u, 0u, 0u});
  p.t3(0x36, true, {0x00030088u});
  // 8210D1FC: kick.
  if (!p.kick()) return;
  // 8210D218..8210D234 (B8)
  p.t3(0x46, false, {6u});
  p.t0dev(0x2007, 1, 0x281C);
  // 8210D23C..8210D26C (B9): restore the window scissor from the shadow if B0 changed it.
  if (b0) {
    const uint32_t br = p.r32(0x2848), tl = p.r32(0x2844);
    if (!windowScissor(p, (uint32_t)rs_sx15lo(tl), (uint32_t)rs_sx15hi(tl), (uint32_t)rs_sx15lo(br),
                       (uint32_t)rs_sx15hi(br)))
      return;
  }
  // 8210D270..8210D2D4: the copy size, tex+8 = the device fence, the dirty bits.
  const uint32_t size = r14 * pitch * bpp * depthF;
  p.gw32(tex + 8u, p.r32(0x2A1C));
  p.w64(0x10, p.r64(0x10) | (1ull << 51) | 8u | (1ull << 43) | (3ull << 47) | 0x800u | 0x100u);
  // 8210D2D8: kick.
  if (!p.kick()) return;
  // 8210D2F4..8210D380 (B10): the coherency flush of the destination and the copy registers' reset.
  p.t0(0x0A31, {0x03000100u});
  p.t0(0x0A2F, {(size + 0x1FFFu) & ~0xFFFu, destBase & ~0xFFFu});
  p.t3(0x3C, false, {3u, 0xA31u, 0u, 0x80000000u, 8u});  // WAIT_REG_MEM
  p.w32(0x2998, 0);
  p.w32(0x29A4, 0);
  p.t0(0x2318, {0u});
  p.t0(0x231B, {0u});

  // 8210D384..8210D3EC: the destination's fences, or an entry {0x40000000 | tex >> 2, -1} on the 335C list.
  const uint32_t s = p.r32(0x2A1C);
  // p1.flush_residue: a pre-pass past a transparent kick reads S from before the kick; the live S is S + 2k (a close adds
  // 2), so the branch below is the same only while S is odd (fences are odd). An even S declines (never expected).
  if (p.kicked && (s & 1u) == 0u) {
    p.stop("resolve: even fence S after a transparent kick");
    return;
  }
  if (s != 0) {
    p.gw32(tex + 8u, s);
    p.gw32(tex + 0xCu, s);
    return;
  }
  uint32_t th;
  if (!rs_g32(p, tex, th)) return;
  if ((p.r32(0x2A20) & th) == 0) return;
  const uint32_t cur = p.r32(0x335C), lim = p.r32(0x3360);
  if (cur >= lim) {  // 8210D3C4 820FAC80
    p.stop("resolve: 335C list segment full (820FAC80)");
    return;
  }
  // lwz r11,var_D0 is the high word of `std extsw(left)` at 8210D10C: inslwi keeps its top two bits.
  const uint32_t entry = ((left < 0) ? 0xC0000000u : 0x40000000u) | (tex >> 2);
  uint8_t eb[8];
  rs_be32(eb, entry);
  rs_be32(eb + 4, 0xFFFFFFFFu);
  p.gwBytes(cur, eb, 8);
  p.w32(0x335C, cur + 8u);
}
