// NATIVE N4a (docs/NATIVE_ROADMAP.md): the resolve reads the SOURCE HOST RENDER TARGET
// directly - no EDRAM buffer. Included by gfx/d3d12_pm4_shmem.cpp (it needs that TU's
// buildResolve / XeResolveConsts and the shared-memory members).
//
// Each Xenia resolve compute shader (reference/xenia-canary src/xenia/gpu/shaders,
// resolve_{fast,full}_*.xesli, BSD-3, Ben Vanik / Triang3l) is reproduced with ONE change:
// where Xenia loads raw EDRAM words, this loads the host RT texel and packs it with the
// SAME math the EDRAM store pass uses (gfx/d3d12_pm4_edram.cpp kPm4EdramHlsl PackColor -
// copied below, keep in sync), i.e. exactly the words the EDRAM buffer would hold. MSAA
// keys render at 1x with replicated samples, so every sample read returns that texel and
// Xenia's sample sums are reproduced with the identical float additions. Everything else
// (unpack, exp bias, swap, pack, tiled destination address, endian) is Xenia's own code,
// compiled at run time from the embedded xesl sources (D3DCompile, cs_5_1), or loaded from the
// shipped blobs (SR_NB_AOT). Depth sources read the owner's planes (gNatDepth / the plane SRVs).
// release/emu-stage3: a rectangle whose origin lies past the first tile row / column reads the
// host RT at the true origin (gNatSrcFold adds the tiles buildResolve folded into the base);
// the memory path refused such resolves before (Astra round 1 P1).

#include "../third_party/xenia-shaders/xesl_embed.inc"

namespace {

const char kNativeResolveHlsl[] = R"HLSL(
#define SHADING_LANGUAGE_HLSL_XE 1
#include "endian.xesli"
#include "pixel_formats.xesli"
#include "sr_resolve.xesli"

byte_buffer_align16_wo_declare_xe(xe_resolve_dest, set=1, binding=0, u0, space0)
Texture2D<float4> gNatSrc : register(t1, space0);
// NATIVE present (SR_NATIVE_PRESENT=1): a fast 32bpp resolve also writes its destination
// dwords - the exact values the guest memory receives - into a LINEAR host buffer
// (pitch gNatFrontPitch pixels; 0 = off). The scan-out reads that buffer instead of the
// tiled shared memory.
RWByteAddressBuffer gNatFront : register(u1, space0);
cbuffer NatFrontC : register(b1, space0) { uint gNatFrontPitch; };
#if NAT_TEX
// iter-467 native-b B2 (SR_NB_RESOLVE_TEX): the resolve writes the TEXTURE that samples its
// destination (a native target, gfx/native_targets.inc) instead of the guest-memory buffer: each
// destination word goes through the texture loader's own transform (texture_load_{32,64}bpb: a
// 32-bit endian swap with the fetch's endian, then the UINT channel split of the direct load,
// gfx/tex_load_direct.inc), so the texel equals what loading the buffer would give. The texel
// position is the destination pixel: the base tile's origin (gNatTexOrigin, scaled) +
// dest_xy_offset_scaled + the pixel index, exactly as XeResolveDestPixelAddress places it.
// iter-467 native-b (SR_NB_TARGETS_1X): at 1x the kernels are built with NAT_DUAL - every texel goes to the texture
// AND to the guest-layout shared memory (xe_resolve_dest), so the memory stays exact for every reader and the
// texture is current without a reload.
RWTexture2DArray<uint4> gNatTex : register(u2, space0);
// iter-467 native-b (SR_NB_DEPTH_TARGET): a depth target's stencil bytes, which the depth texture's load
// (texture_load_depth_float: R32_FLOAT) drops - kept so the memory can be written back exactly.
RWTexture2DArray<uint> gNatTexStencil : register(u3, space0);
// iter-467 native-b (SR_NB_TEX_FANOUT): up to two more current targets of the same identity (the same shadow
// cascade fetched as a 2D texture and as a layer of stacked fetches) written by the same stores, instead of a
// CopyTextureRegion of the rectangle into each after the dispatch.
RWTexture2DArray<uint4> gNatTexB : register(u4, space0);
RWTexture2DArray<uint4> gNatTexC : register(u5, space0);
cbuffer NatTexC : register(b3, space0) {
  uint gNatTexEndian; uint gNatTexUnpack; uint2 gNatTexOrigin;
  uint2 gNatTexPad;  // (was the source fold: gNatSrcFold of NatDepthC carries it for both variants, release/emu-stage3)
  uint gNatTexMode;  // 0 colour; 1 k_24_8_FLOAT depth: texel = XeFloat20e4To32(word >> 8), stencil = word & 0xFF
  uint gNatTexFan;   // SR_NB_TEX_FANOUT: how many of gNatTexB / gNatTexC also receive every colour texel
};
void NatTexPut(uint3 p, uint4 v) {
  gNatTex[p] = v;
  [branch] if (gNatTexFan != 0u) {
    gNatTexB[p] = v;
    [branch] if (gNatTexFan >= 2u) gNatTexC[p] = v;
  }
}
void NatTexStore32(XeResolveInfo ri, uint2 pixel, uint4 guest) {  // 4 texels from 4 guest dwords
  uint4 w = XeEndianSwap32(guest, gNatTexEndian);
  uint2 t = gNatTexOrigin + ri.dest_xy_offset_scaled + pixel;
  [branch] if (gNatTexMode == 1u) {  // texture_load_depth_float's transform, the stencil byte aside
    [unroll] for (uint j = 0u; j < 4u; j++) {
      gNatTex[uint3(t.x + j, t.y, 0u)] = uint4(XeFloat20e4To32(w[j] >> 8u, false), 0u, 0u, 0u);
      gNatTexStencil[uint3(t.x + j, t.y, 0u)] = w[j] & 0xFFu;
    }
    return;
  }
  [unroll] for (uint j = 0u; j < 4u; j++) {
    uint v = w[j];
    uint4 c = gNatTexUnpack == 1u ? uint4(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24)
            : gNatTexUnpack == 2u ? uint4(v & 0xFFFFu, v >> 16, 0u, 0u) : uint4(v, 0u, 0u, 0u);
    NatTexPut(uint3(t.x + j, t.y, 0u), c);
  }
}
void NatTexStore16(XeResolveInfo ri, uint2 pixel, uint2 guest) {  // 4 texels from 2 guest dwords
  uint2 w = XeEndianSwap16(guest, gNatTexEndian);  // texture_load_16bpb's swap
  uint2 t = gNatTexOrigin + ri.dest_xy_offset_scaled + pixel;
  NatTexPut(uint3(t.x, t.y, 0u), uint4(w.x & 0xFFFFu, 0u, 0u, 0u));
  NatTexPut(uint3(t.x + 1u, t.y, 0u), uint4(w.x >> 16, 0u, 0u, 0u));
  NatTexPut(uint3(t.x + 2u, t.y, 0u), uint4(w.y & 0xFFFFu, 0u, 0u, 0u));
  NatTexPut(uint3(t.x + 3u, t.y, 0u), uint4(w.y >> 16, 0u, 0u, 0u));
}
void NatTexStore64(XeResolveInfo ri, uint2 pixel, uint4 guest) {  // 2 texels from 4 guest dwords
  uint4 w = XeEndianSwap32(guest, gNatTexEndian);
  uint2 t = gNatTexOrigin + ri.dest_xy_offset_scaled + pixel;
  NatTexPut(uint3(t.x, t.y, 0u), uint4(w.x & 0xFFFFu, w.x >> 16, w.y & 0xFFFFu, w.y >> 16));
  NatTexPut(uint3(t.x + 1u, t.y, 0u), uint4(w.z & 0xFFFFu, w.z >> 16, w.w & 0xFFFFu, w.w >> 16));
}
#endif
// Depth sources: the depth surface's two copy planes (depth, stencil) placed in a buffer,
// read exactly as the EDRAM store pass reads them (kPm4EdramHlsl CSStore).
ByteAddressBuffer gNatDepth : register(t2, space0);
// iter-467 native-b (SR_NB_DEPTH_SRV, gNatS1Bpt == 0): the depth owner's planes through SRVs - t3 the
// depth plane (R24_UNORM_X8 / R32_FLOAT_X8X24), t4 the stencil plane (.y) - instead of a copy of both
// whole planes into gNatDepth before every depth resolve (the depth RT spans the EDRAM height:
// 3840 x 6144 texels x 5 bytes at 3x). The bits come back exactly as the direct depth transfers read
// them (d3d12_pm4_edram.cpp SrcDepthDword): UNORM -> float -> UNORM is exact, NatUnorm24 = that
// round(f * (2^24 - 1)) in integer math (kPm4EdramHlsl Unorm24, verbatim).
Texture2D<float> gNatDepthTex : register(t3, space0);
Texture2D<uint4> gNatStencilTex : register(t4, space0);
uint NatUnorm24(float f) {
  uint b = asuint(f);
  if ((b & 0x80000000u) != 0u) return 0u;
  if (b >= 0x3F800000u) return 0xFFFFFFu;
  uint e = (b >> 23) & 0xFFu;
  if (e < 101u) return 0u;  // f < 2^-26
  uint mant = (b & 0x7FFFFFu) | 0x800000u;
  uint s = 150u - e;  // 24..49: p = (mant * 2^24 - mant) / 2^s
  uint lo1 = mant << 24;
  uint lo = lo1 - mant;
  uint hi = (mant >> 8) - (lo1 < mant ? 1u : 0u);
  if (s < 32u) {
    uint lo2 = lo + (1u << (s - 1u));
    uint hi2 = hi + (lo2 < lo ? 1u : 0u);
    return (lo2 >> s) | (hi2 << (32u - s));
  }
  if (s == 32u) { uint lo2 = lo + 0x80000000u; return hi + (lo2 < lo ? 1u : 0u); }
  return (hi + (1u << (s - 33u))) >> (s - 32u);
}
cbuffer NatDepthC : register(b2, space0) {
  uint gNatD0Off; uint gNatD0Pitch; uint gNatS1Off; uint gNatS1Pitch; uint gNatS1Bpt; uint gNatS1Byte;
  // The surface that OWNS the span (the source key itself, or a surface at the same EDRAM base,
  // pitch and bpp with another format or MSAA mode - roadmap Q5): its key format packs the EDRAM
  // words, and a source pixel maps to an owner pixel through the sample grid:
  // owner = (p << srcShift) >> ownerShift, shifts packed x | y << 4 | ox << 8 | oy << 12;
  // bit 16 = the owner is a depth surface (its planes are in gNatDepth), bit 17 = source and
  // owner differ in depth-ness: Xenos stores depth tiles with their two column halves swapped
  // (edram.xesli XeEdramOffsetBytes), so the sample column moves by half a tile.
  uint gNatOwnerFmt; uint gNatMsaaRemap;
  // A second owner has its own format/sample grid. Selection is described by
  // CPU-produced pixel rectangles in NatOwnerC; no source base or owner runs.
  uint gNatOwnerFmtB; uint gNatMsaaRemapB; uint gNatOwnerPad; uint gNatAliasPitch;
  uint4 gNatOwnerPadding[2];
  // iter-467 native-b (SR_NB_SCALED_REMAP): the draw resolution scale. Xenia's scaled EDRAM layout
  // (edram.xesli XeEdramOffsetBytes) is the 1x one in HOST samples with tiles of 80*sx x 16*sy samples, so the
  // owner remap above holds in host pixels; only the tile dimensions scale (0 = 1).
  uint gNatScaleX; uint gNatScaleY;
  // release/emu-stage3 (Astra round 1 P1): the source rectangle's origin past the first tile row / column.
  // buildResolve folds the whole EDRAM tiles before the origin into the base tile (XeResolveInfo keeps only the
  // in-tile 8-aligned offset, edram_offset_scaled), which the EDRAM read never sees - a host-RT read must add them
  // back: the folded pixels of the SOURCE surface, scaled (NatSrc; both variants, the NAT_TEX path carried it alone
  // before). The owner-selection tile math (NatOwnerSel) and the depth half-tile swap (NatOwnerPixel) then run on
  // the true sample coordinates, as the NAT_TEX path's already did.
  uint2 gNatSrcFold;
};
cbuffer NatOwnerC : register(b4, space0) {
  uint gNatRectCount; uint3 gNatRectPad;
  uint4 gNatRects[256];  // half-open source-pixel rectangles; native_owner::Plan
};

// ---- kPm4EdramHlsl PackColor (host RT texel -> EDRAM bits), verbatim ----
bool NatIsNan(float f) { return (asuint(f) & 0x7FFFFFFFu) > 0x7F800000u; }
uint NatFloatToHalf(float f) {
  uint x = asuint(f);
  uint sgn = (x >> 16) & 0x8000u;
  uint mant = x & 0x7FFFFFu;
  int ex = int((x >> 23) & 0xFFu);
  if (ex == 0xFF) return sgn | 0x7C00u | (mant != 0u ? 0x200u : 0u);
  int e = ex - 127 + 15;
  // iter-467 freezefx (gfx/pm4_half_gate.hpp): FXC types the hex literal as uint, so `e >= 0x1F`
  // was an unsigned compare and every e <= 0 (zero, |f| < 2^-14) packed as +-inf.
#ifdef SR_HALF_HEXCMP
  if (e >= 0x1F) return sgn | 0x7C00u;
#else
  if (e >= 31) return sgn | 0x7C00u;
#endif
  if (e <= 0) {
    if (e < -10) return sgn;
    mant |= 0x800000u;
    uint shift = uint(14 - e);
    uint h = mant >> shift;
    uint rem = mant & ((1u << shift) - 1u);
    uint halfway = 1u << (shift - 1u);
    if (rem > halfway || (rem == halfway && (h & 1u) != 0u)) h++;
    return sgn | h;
  }
  uint h2 = (uint(e) << 10) | (mant >> 13);
  uint rem2 = mant & 0x1FFFu;
  if (rem2 > 0x1000u || (rem2 == 0x1000u && (h2 & 1u) != 0u)) h2++;
  return sgn | h2;
}
uint NatFloatTo7e3(float v) {
  uint i = asuint(v);
  if (NatIsNan(v) || (i & 0x80000000u) != 0u) return 0u;
  if (i > 0x41FF73FFu) return 0x3FFu;
  if (i < 0x3E800000u) {
    uint shift = 125u - (i >> 23);
    i = shift >= 32u ? 0u : ((0x800000u | (i & 0x7FFFFFu)) >> shift);
  } else {
    i += 0xC2000000u;
  }
  return ((i + 0x7FFFu + ((i >> 16) & 1u)) >> 16) & 0x3FFu;
}
uint NatPackUNorm(float f, float s) { return uint(saturate(f) * s + 0.5); }
uint NatPackFixed16(float f) {
  f = NatIsNan(f) ? 0.0 : f;
  f = min(max(f, -32.0), 32.0);
  return uint(int(f * (32767.0 / 32.0))) & 0xFFFFu;
}
uint2 NatPackColor(uint fmt, float4 c) {
  if (fmt == 0u) return uint2(NatPackUNorm(c.r, 255.0) | (NatPackUNorm(c.g, 255.0) << 8) |
                              (NatPackUNorm(c.b, 255.0) << 16) | (NatPackUNorm(c.a, 255.0) << 24), 0u);
  if (fmt == 2u) return uint2(NatPackUNorm(c.r, 1023.0) | (NatPackUNorm(c.g, 1023.0) << 10) |
                              (NatPackUNorm(c.b, 1023.0) << 20) | (NatPackUNorm(c.a, 3.0) << 30), 0u);
  if (fmt == 3u) return uint2(NatFloatTo7e3(c.r) | (NatFloatTo7e3(c.g) << 10) |
                              (NatFloatTo7e3(c.b) << 20) | (NatPackUNorm(c.a, 3.0) << 30), 0u);
  if (fmt == 4u) return uint2(NatPackFixed16(c.r) | (NatPackFixed16(c.g) << 16), 0u);
  if (fmt == 5u) return uint2(NatPackFixed16(c.r) | (NatPackFixed16(c.g) << 16),
                              NatPackFixed16(c.b) | (NatPackFixed16(c.a) << 16));
  if (fmt == 6u) return uint2(NatFloatToHalf(c.r) | (NatFloatToHalf(c.g) << 16), 0u);
  if (fmt == 7u) return uint2(NatFloatToHalf(c.r) | (NatFloatToHalf(c.g) << 16),
                              NatFloatToHalf(c.b) | (NatFloatToHalf(c.a) << 16));
  if (fmt == 14u) return uint2(asuint(c.r), 0u);
  if (fmt == 15u) return uint2(asuint(c.r), asuint(c.g));
  return uint2(0u, 0u);
}

// The host RT holds the key's BASE format (Xenia GetBaseColorFormat: 10 -> 2, 12 -> 3); the
// resolve constants carry RB_COLOR_INFO's own format, which the unpack below still uses.
uint NatBaseFormat(uint f) { return f == 10u ? 2u : (f == 12u ? 3u : f); }
// kPm4EdramHlsl FloatTo20e4, verbatim.
uint NatFloatTo20e4(float f32) {
  uint u = asuint(f32);
  u = min((u <= 0x7FFFFFFFu) ? u : 0u, 0x3FFFFFF8u);
  uint shift = min(113u - min(113u, u >> 23), 24u);
  uint denorm = ((u & 0x7FFFFFu) | 0x800000u) >> shift;
  uint f24 = (u < 0x38800000u) ? denorm : (u + 0xC8000000u);
  f24 += 3u + ((f24 >> 3) & 1u);
  return (f24 >> 3) & 0xFFFFFFu;
}
// The alias table supplies ownership in source pixels before sample remapping.
uint NatOwnerSel(int2 p) {
  [loop] for (uint i = 0u; i < gNatRectCount; i++) {
    uint4 rect = gNatRects[i];
    if (all(p >= int2(rect.xy)) && all(p < int2(rect.zw))) return 1u;
  }
  return 0u;
}
int2 NatAliasPixel(XeResolveInfo ri, int2 p, uint r) {
  uint sx = uint(p.x) << (r & 15u);  // the source pixel's sample column
  uint sy = uint(p.y) << ((r >> 4) & 15u);
  // The EDRAM is a ring of 2048 tiles (release/emu-stage3, Astra re-review 1: the full-ring wrap): a source sample
  // whose tile index base + row * pitch + column passes 2048 - a rectangle origin folded past the surface's last
  // tile row, or a row that straddles the wrap - lands on the tile (index mod 2048), which the owner's host RT holds
  // at THAT tile's row / column (the owner shares the source's base and pitch). Canonicalise the sample before the
  // read; the model's EDRAM addressing wraps the same way (XeEdramOffsetBytes masks the tile index).
  {
    uint tileW = (80u * max(gNatScaleX, 1u)) >> ri.edram_format_ints_log2, tileH = 16u * max(gNatScaleY, 1u);
    uint pitch = gNatAliasPitch;
    uint lin = (sy / tileH) * pitch + sx / tileW;
    [branch] if (lin >= 2048u && pitch != 0u) {
      lin -= (lin / 2048u) * 2048u;
      sx = (lin % pitch) * tileW + sx % tileW;
      sy = (lin / pitch) * tileH + sy % tileH;
    }
  }
  [branch] if ((r & 0x20000u) != 0u) {
    uint tileW = (80u * max(gNatScaleX, 1u)) >> ri.edram_format_ints_log2, half = tileW >> 1u;
    uint inTile = sx % tileW;
    sx += inTile >= half ? uint(-int(half)) : half;
  }
  return int2(int(sx >> ((r >> 8) & 15u)), int(sy >> ((r >> 12) & 15u)));
}
uint2 NatRaw(XeResolveInfo ri, int2 p) {
  const bool b = NatOwnerSel(p) != 0u;
  const uint remap = b ? gNatMsaaRemapB : gNatMsaaRemap, fmt = b ? gNatOwnerFmtB : gNatOwnerFmt;
  p = NatAliasPixel(ri, p, remap);
  [branch] if ((remap & 0x10000u) != 0u) {
    [branch] if (gNatS1Bpt == 0u) {
      float dv = gNatDepthTex.Load(int3(p, 0));
      uint sv = gNatStencilTex.Load(int3(p, 0)).y & 0xFFu;
      uint dd = fmt == 1u ? NatFloatTo20e4(dv * 2.0) : NatUnorm24(dv);
      return uint2((dd << 8) | sv, 0u);
    }
    uint v0 = gNatDepth.Load(gNatD0Off + uint(p.y) * gNatD0Pitch + uint(p.x) * 4u);
    uint sa = gNatS1Off + uint(p.y) * gNatS1Pitch + uint(p.x) * gNatS1Bpt + gNatS1Byte;
    uint st = (gNatDepth.Load(sa & ~3u) >> ((sa & 3u) * 8u)) & 0xFFu;
    uint d24 = fmt == 1u ? NatFloatTo20e4(asfloat(v0) * 2.0) : (v0 & 0xFFFFFFu);
    return uint2((d24 << 8) | st, 0u);
  }
  return NatPackColor(fmt, gNatSrc.Load(int3(p, 0)));
}
// The source pixel a destination pixel reads: the in-tile offset of the rectangle's origin (edram_offset_scaled)
// plus the whole tiles buildResolve folded into the base (gNatSrcFold) - the rectangle's true origin in the source
// surface, which is where the host RT holds it. Both variants (release/emu-stage3: the memory path refused such
// origins before; the NAT_TEX path added them through its own constant).
int2 NatSrc(XeResolveInfo ri, uint2 pixel) {
  return int2(uint2(pixel.x, max(pixel.y, ri.half_pixel_offset_fill_source.y)) + ri.edram_offset_scaled +
              gNatSrcFold);
}

void NatSampleSum4(XeResolveInfo ri, inout float4 p0, inout float4 p1, inout float4 p2, inout float4 p3,
                   inout float exp_bias) {
  // Xenia adds the other samples' values; replicated samples are the same values.
  float4 m0 = p0, m1 = p1, m2 = p2, m3 = p3;
  [branch] if (ri.sample_select >= kXenosCopySampleSelect_01) {
    exp_bias *= 0.5f;
    p0 += m0; p1 += m1; p2 += m2; p3 += m3;
    [branch] if (ri.sample_select >= kXenosCopySampleSelect_0123) {
      exp_bias *= 0.5f;
      p0 += m0; p1 += m1; p2 += m2; p3 += m3;
      p0 += m0; p1 += m1; p2 += m2; p3 += m3;
    }
  }
}

void NatLoad4RGBAColors(XeResolveInfo ri, int2 s, out float4 p0, out float4 p1, out float4 p2, out float4 p3) {
  [branch] if (ri.edram_format_ints_log2 != 0u) {
    uint2 r0 = NatRaw(ri, s), r1 = NatRaw(ri, s + int2(1, 0)), r2 = NatRaw(ri, s + int2(2, 0)),
          r3 = NatRaw(ri, s + int2(3, 0));
    XeResolveUnpack64bpp4Samples(uint4(r0, r1), uint4(r2, r3), ri.edram_format, p0, p1, p2, p3);
  } else {
    XeResolveUnpack32bpp4Samples(uint4(NatRaw(ri, s).x, NatRaw(ri, s + int2(1, 0)).x, NatRaw(ri, s + int2(2, 0)).x,
                                       NatRaw(ri, s + int2(3, 0)).x),
                                 ri.edram_format, p0, p1, p2, p3);
  }
  float exp_bias = ri.dest_exp_bias_factor;
  NatSampleSum4(ri, p0, p1, p2, p3, exp_bias);
  p0 *= exp_bias; p1 *= exp_bias; p2 *= exp_bias; p3 *= exp_bias;
  [branch] if (ri.dest_swap) { p0 = p0.bgra; p1 = p1.bgra; p2 = p2.bgra; p3 = p3.bgra; }
}

void NatLoad2RGBAColors(XeResolveInfo ri, int2 s, out float4 p0, out float4 p1) {
  [branch] if (ri.edram_format_ints_log2 != 0u) {
    XeResolveUnpack64bpp2Samples(uint4(NatRaw(ri, s), NatRaw(ri, s + int2(1, 0))), ri.edram_format, p0, p1);
  } else {
    XeResolveUnpack32bpp2Samples(uint2(NatRaw(ri, s).x, NatRaw(ri, s + int2(1, 0)).x), ri.edram_format, p0, p1);
  }
  float exp_bias = ri.dest_exp_bias_factor;
  float4 d2 = p0, d3 = p1;
  NatSampleSum4(ri, p0, p1, d2, d3, exp_bias);
  p0 *= exp_bias; p1 *= exp_bias;
  [branch] if (ri.dest_swap) { p0 = p0.bgra; p1 = p1.bgra; }
}

void NatLoad8RedColors(XeResolveInfo ri, int2 s, out float4 p0123, out float4 p4567) {
  uint4 a, b;
  bool hi = ri.dest_swap && ri.edram_format_ints_log2 != 0u;  // Xenia: +4 bytes = the high dword
  uint2 r0 = NatRaw(ri, s), r1 = NatRaw(ri, s + int2(1, 0)), r2 = NatRaw(ri, s + int2(2, 0)),
        r3 = NatRaw(ri, s + int2(3, 0)), r4 = NatRaw(ri, s + int2(4, 0)), r5 = NatRaw(ri, s + int2(5, 0)),
        r6 = NatRaw(ri, s + int2(6, 0)), r7 = NatRaw(ri, s + int2(7, 0));
  a = hi ? uint4(r0.y, r1.y, r2.y, r3.y) : uint4(r0.x, r1.x, r2.x, r3.x);
  b = hi ? uint4(r4.y, r5.y, r6.y, r7.y) : uint4(r4.x, r5.x, r6.x, r7.x);
  [branch] if (ri.edram_format_ints_log2 != 0u) {
    XeResolveUnpack64bpp8RedUnswappedSamples(a, b, ri.edram_format, p0123, p4567);
  } else {
    XeResolveUnpack32bpp8RedSamples(a, b, ri.edram_format, ri.dest_swap, p0123, p4567);
  }
  float exp_bias = ri.dest_exp_bias_factor;
  float4 d2 = p0123, d3 = p4567;
  NatSampleSum4(ri, p0123, p4567, d2, d3, exp_bias);
  p0123 *= exp_bias; p4567 *= exp_bias;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  XeResolveInfo ri = XeResolveGetInfo(pass_push_consts_xe);
#if NAT_KIND == 0
  // resolve_fast_32bpp_{1x2x,4x}msaa
  if (tid.x >= ri.width_div_8_scaled) return;
  uint2 pixel_index = tid.xy << uint2(3u, 0u);
  int2 s = NatSrc(ri, pixel_index);
  uint4 pixels_0123 = uint4(NatRaw(ri, s).x, NatRaw(ri, s + int2(1, 0)).x, NatRaw(ri, s + int2(2, 0)).x,
                            NatRaw(ri, s + int2(3, 0)).x);
  uint4 pixels_4567 = uint4(NatRaw(ri, s + int2(4, 0)).x, NatRaw(ri, s + int2(5, 0)).x,
                            NatRaw(ri, s + int2(6, 0)).x, NatRaw(ri, s + int2(7, 0)).x);
  [branch] if (pixel_index.x == 0u && ri.half_pixel_offset_fill_source.x != 0u) {
    if (ri.half_pixel_offset_fill_source.x >= 2u) {
      if (ri.half_pixel_offset_fill_source.x >= 3u) pixels_0123.z = pixels_0123.w;
      pixels_0123.y = pixels_0123.z;
    }
    pixels_0123.x = pixels_0123.y;
  }
  XeResolveSwap8PixelsRedBlue32bpp(ri, pixels_0123, pixels_4567);
  [branch] if (gNatFrontPitch != 0u) {
    uint2 fp = pixel_index + ri.dest_xy_offset_scaled;
    uint fa = (fp.y * gNatFrontPitch + fp.x) * 4u;
    gNatFront.Store4(fa, XeEndianSwap32(pixels_0123, ri.dest_endian_128));
    gNatFront.Store4(fa + 16u, XeEndianSwap32(pixels_4567, ri.dest_endian_128));
  }
#if NAT_TEX
  NatTexStore32(ri, pixel_index, XeEndianSwap32(pixels_0123, ri.dest_endian_128));
  NatTexStore32(ri, pixel_index + uint2(4u, 0u), XeEndianSwap32(pixels_4567, ri.dest_endian_128));
#endif
#if !NAT_TEX || NAT_DUAL  // B2 at 1x (SR_NB_TARGETS_1X): the guest-layout memory too
  uint dest_address = XeResolveDestPixelAddress(ri, pixel_index, 2u);
  byte_buffer_align16_store16_xe(xe_resolve_dest, dest_address, XeEndianSwap32(pixels_0123, ri.dest_endian_128));
  dest_address += XeResolveLocalXAddressXor(4u, 2u);
  byte_buffer_align16_store16_xe(xe_resolve_dest, dest_address, XeEndianSwap32(pixels_4567, ri.dest_endian_128));
#endif
#elif NAT_KIND == 1
  // resolve_fast_64bpp_{1x2x,4x}msaa
  uint2 pixel_index = tid.xy << uint2(2u, 0u);
  if (pixel_index.x >= ri.width_div_8_scaled << 3u) return;
  int2 s = NatSrc(ri, pixel_index);
  uint4 pixels_01 = uint4(NatRaw(ri, s), NatRaw(ri, s + int2(1, 0)));
  uint4 pixels_23 = uint4(NatRaw(ri, s + int2(2, 0)), NatRaw(ri, s + int2(3, 0)));
  [branch] if (pixel_index.x == 0u && ri.half_pixel_offset_fill_source.x != 0u) {
    if (ri.half_pixel_offset_fill_source.x >= 2u) {
      if (ri.half_pixel_offset_fill_source.x >= 3u) pixels_23.xy = pixels_23.zw;
      pixels_01.zw = pixels_23.xy;
    }
    pixels_01.xy = pixels_01.zw;
  }
  XeResolveSwap4PixelsRedBlue64bpp(ri, pixels_01, pixels_23);
#if NAT_TEX
  NatTexStore64(ri, pixel_index, XeEndianSwap64(pixels_01, ri.dest_endian_128));
  NatTexStore64(ri, pixel_index + uint2(2u, 0u), XeEndianSwap64(pixels_23, ri.dest_endian_128));
#endif
#if !NAT_TEX || NAT_DUAL  // B2 at 1x (SR_NB_TARGETS_1X): the guest-layout memory too
  uint dest_address = XeResolveDestPixelAddress(ri, pixel_index, 3u);
  byte_buffer_align16_store16_xe(xe_resolve_dest, dest_address, XeEndianSwap64(pixels_01, ri.dest_endian_128));
  dest_address += XeResolveLocalXAddressXor(2u, 3u);
  byte_buffer_align16_store16_xe(xe_resolve_dest, dest_address, XeEndianSwap64(pixels_23, ri.dest_endian_128));
#endif
#elif NAT_KIND == 2
  // resolve_full_8bpp
  if (tid.x >= ri.width_div_8_scaled) return;
  uint2 pixel_index = tid.xy << uint2(3u, 0u);
  float4 pixels_0123, pixels_4567;
  NatLoad8RedColors(ri, NatSrc(ri, pixel_index), pixels_0123, pixels_4567);
  [branch] if (pixel_index.x == 0u && ri.half_pixel_offset_fill_source.x != 0u) {
    if (ri.half_pixel_offset_fill_source.x >= 2u) {
      if (ri.half_pixel_offset_fill_source.x >= 3u) pixels_0123.z = pixels_0123.w;
      pixels_0123.y = pixels_0123.z;
    }
    pixels_0123.x = pixels_0123.y;
  }
  byte_buffer_align8_store8_xe(xe_resolve_dest, XeResolveDestPixelAddress(ri, pixel_index, 0u),
                               uint2(XePackR8G8B8A8UNorm(pixels_0123), XePackR8G8B8A8UNorm(pixels_4567)));
#elif NAT_KIND == 3
  // resolve_full_16bpp
  uint2 pixel_index = tid.xy << uint2(2u, 0u);
  if (pixel_index.x >= ri.width_div_8_scaled << 3u) return;
  float4 pixel_0, pixel_1, pixel_2, pixel_3;
  NatLoad4RGBAColors(ri, NatSrc(ri, pixel_index), pixel_0, pixel_1, pixel_2, pixel_3);
  uint2 packed = XePack16bpp4PixelsInUInt2(pixel_0, pixel_1, pixel_2, pixel_3, ri.dest_format);
  [branch] if (pixel_index.x == 0u && ri.half_pixel_offset_fill_source.x != 0u) {
    if (ri.half_pixel_offset_fill_source.x >= 2u) {
      if (ri.half_pixel_offset_fill_source.x >= 3u) packed.y = (packed.y >> 16u) | (packed.y & 0xFFFF0000u);
      packed.x = (packed.x & 0xFFFFu) | (packed.y << 16u);
    }
    packed.x = (packed.x >> 16u) | (packed.x & 0xFFFF0000u);
  }
#if NAT_TEX
  NatTexStore16(ri, pixel_index, XeEndianSwap16(packed, ri.dest_endian_128));
#endif
#if !NAT_TEX || NAT_DUAL  // B2 at 1x (SR_NB_TARGETS_1X): the guest-layout memory too
  byte_buffer_align8_store8_xe(xe_resolve_dest, XeResolveDestPixelAddress(ri, pixel_index, 1u),
                               XeEndianSwap16(packed, ri.dest_endian_128));
#endif
#elif NAT_KIND == 4
  // resolve_full_32bpp
  uint2 pixel_index = tid.xy << uint2(2u, 0u);
  if (pixel_index.x >= ri.width_div_8_scaled << 3u) return;
  float4 pixel_0, pixel_1, pixel_2, pixel_3;
  NatLoad4RGBAColors(ri, NatSrc(ri, pixel_index), pixel_0, pixel_1, pixel_2, pixel_3);
  uint4 packed = XePack32bpp4Pixels(pixel_0, pixel_1, pixel_2, pixel_3, ri.dest_format);
  [branch] if (pixel_index.x == 0u && ri.half_pixel_offset_fill_source.x != 0u) {
    if (ri.half_pixel_offset_fill_source.x >= 2u) {
      if (ri.half_pixel_offset_fill_source.x >= 3u) packed.z = packed.w;
      packed.y = packed.z;
    }
    packed.x = packed.y;
  }
#if NAT_TEX
  NatTexStore32(ri, pixel_index, XeEndianSwap32(packed, ri.dest_endian_128));
#endif
#if !NAT_TEX || NAT_DUAL  // B2 at 1x (SR_NB_TARGETS_1X): the guest-layout memory too
  byte_buffer_align16_store16_xe(xe_resolve_dest, XeResolveDestPixelAddress(ri, pixel_index, 2u),
                                 XeEndianSwap32(packed, ri.dest_endian_128));
#endif
#elif NAT_KIND == 5
  // resolve_full_64bpp
  uint2 pixel_index = tid.xy << uint2(2u, 0u);
  if (pixel_index.x >= ri.width_div_8_scaled << 3u) return;
  float4 pixel_0, pixel_1, pixel_2, pixel_3;
  NatLoad4RGBAColors(ri, NatSrc(ri, pixel_index), pixel_0, pixel_1, pixel_2, pixel_3);
  uint4 packed_01, packed_23;
  XePack64bpp4Pixels(pixel_0, pixel_1, pixel_2, pixel_3, ri.dest_format, packed_01, packed_23);
  [branch] if (pixel_index.x == 0u && ri.half_pixel_offset_fill_source.x != 0u) {
    if (ri.half_pixel_offset_fill_source.x >= 2u) {
      if (ri.half_pixel_offset_fill_source.x >= 3u) packed_23.xy = packed_23.zw;
      packed_01.zw = packed_23.xy;
    }
    packed_01.xy = packed_01.zw;
  }
#if NAT_TEX
  NatTexStore64(ri, pixel_index, XeEndianSwap64(packed_01, ri.dest_endian_128));
  NatTexStore64(ri, pixel_index + uint2(2u, 0u), XeEndianSwap64(packed_23, ri.dest_endian_128));
#endif
#if !NAT_TEX || NAT_DUAL  // B2 at 1x (SR_NB_TARGETS_1X): the guest-layout memory too
  uint dest_address = XeResolveDestPixelAddress(ri, pixel_index, 3u);
  byte_buffer_align16_store16_xe(xe_resolve_dest, dest_address, XeEndianSwap64(packed_01, ri.dest_endian_128));
  dest_address += XeResolveLocalXAddressXor(2u, 3u);
  byte_buffer_align16_store16_xe(xe_resolve_dest, dest_address, XeEndianSwap64(packed_23, ri.dest_endian_128));
#endif
#else
  // resolve_full_128bpp
  uint2 pixel_index = tid.xy << uint2(1u, 0u);
  if (pixel_index.x >= ri.width_div_8_scaled << 3u) return;
  float4 pixel_0, pixel_1;
  NatLoad2RGBAColors(ri, NatSrc(ri, uint2(max(pixel_index.x, ri.half_pixel_offset_fill_source.x), pixel_index.y)),
                     pixel_0, pixel_1);
  [branch] if (pixel_index.x < ri.half_pixel_offset_fill_source.x) pixel_1 = pixel_0;
  uint dest_address = XeResolveDestPixelAddress(ri, pixel_index, 4u);
  byte_buffer_align16_store16_xe(xe_resolve_dest, dest_address,
                                 XeEndianSwap128(asuint(pixel_0), ri.dest_endian_128));
  dest_address += XeResolveLocalXAddressXor(1u, 4u);
  byte_buffer_align16_store16_xe(xe_resolve_dest, dest_address,
                                 XeEndianSwap128(asuint(pixel_1), ri.dest_endian_128));
#endif
}
)HLSL";

bool natBuildOwnerPlan(const edram::ResolveInfo& ri, const uint32_t* runs, uint32_t count,
                       const XeResolveConsts& constants, uint32_t foldX, uint32_t foldY,
                       uint32_t groupsY, bool scaled, native_owner::Plan& plan) {
  if (count > 4) return false;
  native_owner::Range ranges[4]{};
  for (uint32_t i = 0; i < count; ++i) ranges[i] = {runs[2 * i], runs[2 * i + 1]};
  native_owner::Rect domain;
  if (!native_owner::dispatchDomain(constants.coordInfo, constants.edramInfo,
                                    foldX, foldY, groupsY, scaled, domain)) return false;
  return native_owner::build(ri.srcKey.base(), ri.srcKey.pitchTiles(), ri.msaa,
                             scaled ? (constants.coordInfo >> 16) & 7u : 1u,
                             scaled ? (constants.coordInfo >> 19) & 7u : 1u,
                             ranges, count, domain, plan);
}

// The shader slot buildResolve returns (0-3 fast 32/64bpp x 1x2x/4x, 4-8 full 8..128bpp)
// -> NAT_KIND.
int natKindForShader(int shader) {
  static const int k[9] = {0, 0, 1, 1, 2, 3, 4, 5, 6};
  return (shader >= 0 && shader < 9) ? k[shader] : -1;
}

bool natResolveEnabled() {
#if !SR_ORACLE
  return true;  // C1 stage 3 (3.1 rule 2): the =0 arm is the EDRAM-path resolve of the oracle unit - a fallback component
#else
  static const bool v = [] {
    // iter-467 native-b: default ON again at 1x (=0 off). The first 1x bake (354ad343) lost 13.7 fps uncapped (job15):
    // the native path hashed its rebaseline UNDER m_shmMu and the CP - the 1x wall - blocked on it (99d90b92 set it
    // OFF). With SR_NB_NATIVE_PREHASH + SR_NB_REBASE_FRESH_FIRST baked (93c67640) it is fps-neutral: job17 (exe fr18,
    // in-roll A/B, uncapped, two rolls) -0.8 +-2.6 fps, frame p50 -0.01 +-0.13 ms; and it removes the 1x EDRAM resolve
    // path (meter edram_res 26 -> 1.4, GPU busy -1.2 to -1.5 ms in l11; check roll cr3: 27 native vs EDRAM PASS).
    // At a scale SR_NATIVE_RESOLVE_SCALED rules.
    const char* e = std::getenv("SR_NATIVE_RESOLVE");
    return !e || e[0] != '0';
  }();
  static const int ab = sr::abt::reg("SR_NATIVE_RESOLVE");  // SR_AB_TOGGLE (read at the point of use)
  return v && (ab < 0 || sr::abt::on(ab));
#endif
}

// iter-466 res4k: at a draw resolution scale > 1 the resolve reads the source host RT (already
// s x s per guest pixel) and writes the scaled resolve memory with Xenia's *_scaled resolve
// addressing (XE_RESOLVE_RESOLUTION_SCALED) - the EDRAM store before every scaled resolve (the
// RT copied into the s^2-sized EDRAM buffer, then read back) is skipped. Same-shape single
// owners only; mixed spans and reinterpreting owners keep the EDRAM path.
// iter-467 native-b (SR_NB_SCALED_REMAP, baked ON after check roll cr4_nb; =0 off): at a draw resolution scale, native resolves (and native
// targets) also from an owner of another sample grid or depth-ness and from MIXED spans (one colour + one depth
// owner, per tile) - what 1x already does - with the scaled tile dimensions in the kernel (gNatScaleX / Y). Without
// it those resolves (the FMV-era traces eahd / t16 / t21 / t25: 5 a frame) take the EDRAM path.
bool natScaledRemapOn() {
#if !SR_ORACLE
  return true;  // C1 stage 3 (3.1 rule 2): the =0 arm is the EDRAM-path resolve of the oracle unit - a fallback component
#else
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_SCALED_REMAP");
    return !e || e[0] != '0';  // iter-467 native-b: baked ON after check roll cr4_nb (=0 A/B)
  }();
  static const int ab = sr::abt::reg("SR_NB_SCALED_REMAP");  // SR_AB_TOGGLE
  return v && (ab < 0 || sr::abt::on(ab));
#endif
}

bool natResolveScaledEnabled() {
#if !SR_ORACLE
  return true;  // C1 stage 3 (3.1 rule 2): the =0 arm is the EDRAM-path resolve of the oracle unit - a fallback component
#else
  static const bool v = [] {
    // iter-466 res4k: baked default ON (replay byte-identical on 7 traces at 3x; live 3x whole
    // run, boss save and Fortress clean); =0 keeps the EDRAM-path A/B.
    const char* e = std::getenv("SR_NATIVE_RESOLVE_SCALED");
    return !e || e[0] != '0';
  }();
  return v;
#endif
}

// The Xenia include closure, embedded (third_party/xenia-shaders/xesl_embed.inc, generated by
// tools/gen_xesl_embed.py): the shipping exe needs no reference/ tree. Paths are resolved
// relative to the including file, as the xesl sources expect.
class NatXeslInclude final : public ID3DInclude {
 public:
  HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID parent, LPCVOID* data, UINT* bytes) override {
    std::string dir = "gpu/shaders";  // the main source's virtual directory
    for (const auto& o : opened_)
      if (o.first == parent) dir = o.second;
    std::vector<std::string> parts;
    auto push = [&](const std::string& path) {
      size_t i = 0;
      while (i <= path.size()) {
        const size_t j = std::min(path.find('/', i), path.size());
        const std::string seg = path.substr(i, j - i);
        if (seg == "..") {
          if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
          parts.push_back(seg);
        }
        i = j + 1;
      }
    };
    push(dir);
    push(name);
    std::string path;
    for (const auto& seg : parts) path += (path.empty() ? "" : "/") + seg;
    for (const auto& f : kXeslEmbed) {
      if (path != f.path) continue;
      *data = f.text;
      *bytes = UINT(std::strlen(f.text));
      opened_.emplace_back(f.text, path.substr(0, path.rfind('/')));
      return S_OK;
    }
    return E_FAIL;
  }
  HRESULT __stdcall Close(LPCVOID) override { return S_OK; }

 private:
  std::vector<std::pair<const void*, std::string>> opened_;
};

// iter-467 native-b (EDRAM zero, class A1: SR_NB_AOT, default ON; =0 compiles at run time as before): strand B's
// compute kernels (native resolves, NAT_TEX target resolves, materialize, direct loads) ship precompiled -
// data/shader_cache/nb/<name>-<key>.dxbc, key = XXH3 of the HLSL source, the defines, the embedded xesl closure and
// the target - as the guest shaders ship as DXIL. Compiled at run time (~25 s of D3DCompile on a worker) every run
// resolved through the EDRAM model until the pipelines were ready: 17-20K EDRAM resolves in the first ~25 s (check
// rolls cr4b / cr5, "warm="). A missing or stale blob (the source changed) compiles as before; SR_NB_AOT_EMIT=1 also
// writes the compiled blob. The set is checked in and listed by path in docs/release_composition.json (package):
// regenerate = one replay at 1x and at a scale under SR_NB_AOT_EMIT=1, delete the blobs it did not write (a key the
// exe never asks for is dead weight), list the new ones there (docs/RELEASE_COMPOSITION.md 8.12). SR_NB_AOT_DIR
// overrides the directory.
bool nbAotOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_AOT");
    return !e || e[0] != '0';
  }();
  return v;
}
bool nbAotEmit() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_NB_AOT_EMIT");
    return e && e[0] == '1';
  }();
  return v;
#else
  return false;  // C1 stage 3: the blobs are emitted by the dev build (docs/NATIVE_ROADMAP.md step 1)
#endif
}
// render/shader-coverage (orchestrator 2026-10-17): SR_NB_AOT_DIR, else the exe's OWN folder's data/shader_cache/nb
// (the blobs are keyed by this build's kernel sources: a package started from any working folder reads its own set),
// else the working folder's (a dev build in build/ has none beside it). Before, the working folder came first, so a
// release started from an older checkout compiled 7 kernels at boot (lF_R2 / lF_R4: [GPU_HANG] H3 at 15.8 s).
// SR_NB_AOT_EMIT=1 regenerates the checkout's set, so it keeps the old order (the working folder first).
const std::filesystem::path& nbAotDir() {
  static const std::filesystem::path dir = [] {
    const std::filesystem::path d = ShaderCache::shippedDataDir("SR_NB_AOT_DIR", "data/shader_cache/nb", nbAotEmit());
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_AOT] blob folder %s", d.string().c_str());
    return d;
  }();
  return dir;
}
std::atomic<uint32_t> g_aotLoaded{0}, g_aotCompiled{0}, g_aotWritten{0};

// The bytecode of one kernel variant: the shipped blob, else (aotOnly = false) D3DCompile (cs_5_1, O3).
bool nbAotCode(const char* name, const char* src, size_t srcLen, const char* srcPath, const D3D_SHADER_MACRO* defs,
               bool aotOnly, std::vector<uint8_t>& out, std::string& err) {
  static const uint64_t s_xesl = [] {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (const auto& f : kXeslEmbed)
      h = (h ^ ShaderCache::hashUcode(f.text, std::strlen(f.text))) * 0x100000001B3ull ^
          ShaderCache::hashUcode(f.path, std::strlen(f.path));
    return h;
  }();
  std::string dk = "cs_5_1|O3|";
  for (const D3D_SHADER_MACRO* m = defs; m && m->Name; m++) dk += std::string(m->Name) + "=" + (m->Definition ? m->Definition : "") + ";";
  const uint64_t key = ShaderCache::hashUcode(src, srcLen) ^ (ShaderCache::hashUcode(dk.data(), dk.size()) * 0x9E3779B1ull) ^ s_xesl;
  char fn[96];
  std::snprintf(fn, sizeof fn, "%s-%016llx.dxbc", name, (unsigned long long)key);
  const std::filesystem::path file = nbAotDir() / fn;
  if (nbAotOn()) {
    std::ifstream in(file, std::ios::binary);
    if (in) {
      out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (out.size() > 32 && std::memcmp(out.data(), "DXBC", 4) == 0) {
        g_aotLoaded.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      out.clear();
    }
  }
  if (aotOnly) return false;
  NatXeslInclude inc;
  Microsoft::WRL::ComPtr<ID3DBlob> code, cerr;
  if (FAILED(D3DCompile(src, srcLen, srcPath, defs, &inc, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                        &cerr))) {
    err = cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "?";
    return false;
  }
  out.assign(static_cast<const uint8_t*>(code->GetBufferPointer()),
             static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
  const uint32_t nc = g_aotCompiled.fetch_add(1, std::memory_order_relaxed) + 1u;
  // A shipped build compiles nothing: a miss means the kernel source or its defines changed since the blobs were
  // emitted (regenerate: any replay at 1x and at a scale with SR_NB_AOT_EMIT=1, docs/NATIVE_ROADMAP.md step 1).
  if (nbAotOn() && !nbAotEmit() && nc <= 32u)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NB_AOT] compiled %s - no shipped blob %s", name, file.string().c_str());
#if SR_ORACLE
  if (nbAotEmit()) {
    std::error_code ec;
    std::filesystem::create_directories(nbAotDir(), ec);
    std::ofstream o(file, std::ios::binary);
    if (o && o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()))) {
      g_aotWritten.fetch_add(1, std::memory_order_relaxed);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_AOT] wrote %s (%zu bytes)", file.string().c_str(), out.size());
    }
  }
#endif
  return true;
}

bool natPresentEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NATIVE_PRESENT");
    return e && e[0] == '1';
  }();
  return v;
}

// A front-buffer candidate: the linear copy of one fast 32bpp resolve's destination.
struct NatFront {
  Microsoft::WRL::ComPtr<ID3D12Resource> buf;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  uint64_t bytes = 0, epoch = ~0ull, lastUse = 0;
  uint32_t dest = 0, span = 0, pitch = 0, rows = 0;
  bool valid = false;
};

// Why pm4NativeResolve did not take a resolve (release/emu-stage3, Astra round 1 P2: "not ready" told apart from
// "unsupported"): counted per reason, the last one named by nbNatStatus - the [NATIVE_RESOLVE_BAD] line of the
// release configuration and the [NB_EDRAM_RESOLVE] line of the dev build carry it.
enum NatRefusal : uint32_t {
  kNatRefuseWarm = 0,    // the pipelines are still compiling (the dev build only: the release waits for them)
  kNatRefuseFailed,      // the pipelines failed to build (the root signature, a compile, a PSO)
  kNatRefuseOff,         // SR_NATIVE_RESOLVE / _SCALED =0 (the development build's A/B), or not a copy
  kNatRefuseDepth,       // SR_NATIVE_RESOLVE_DEPTH=0, or a depth plane layout the kernel cannot read
  kNatRefuseScaled,      // scaled: the scaled resolve memory is not ready, or (SR_NB_SCALED_REMAP=0) another shape
  kNatRefuseShape,       // buildResolve took no shader for it (format / bpp / an empty extent)
  kNatRefuseKind,        // SR_NATIVE_RESOLVE_KINDS masks this kind (the development build's diagnostic)
  kNatRefuseNoRt,        // an owner's host RT or its SRV is missing
  kNatRefuseMemory,      // the scaled destination memory could not be ensured
  kNatRefuseArrayDest,   // copy_dest_array = 1 (an array / volume destination): implemented in no path - the dev
                         // build's CPU resolve drops it too (edram::resolveToGuest) - and never seen from the title
                         // (the 8.4 census of every replay trace and roll; Astra re-review 1 P2)
  kNatRefuseCount
};
constexpr const char* kNatRefusalName[kNatRefuseCount] = {"warm", "failed", "off", "depth", "scaled",
                                                           "shape", "kind", "no-rt", "memory", "array-dest"};

struct NatResolveState {
  std::atomic<bool> ready{false}, failed{false}, started{false};
  Microsoft::WRL::ComPtr<ID3D12RootSignature> rs;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso[7];
  uint64_t done = 0, fallbackDepth = 0, fallbackOther = 0, fallbackWarm = 0;
  // The refusals by reason (release/emu-stage3), and the census counters of the frame renderer's resolves the native
  // kernel did not take: declinedOwned = spans the kernel was asked for and refused (the development build's model took
  // them; the release stops), declinedOwners = spans the owner selection kept from the kernel (another base / pitch /
  // bpp, two colour owners, unowned tiles; the same in both builds up to SR_NB_RESOLVE_CLAIM).
  uint64_t refused[kNatRefuseCount] = {};
  uint32_t lastRefusal = kNatRefuseCount;  // kNatRefuseCount = none yet
  uint64_t declinedOwned = 0, declinedOwners = 0;
  NatFront front[4];
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> graveyard;  // replaced buffers (in flight)
  uint64_t frontUse = 0, presentsNative = 0, presentsShm = 0, depthDone = 0, scaledDone = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> depthScratch;  // the depth source's planes
  uint64_t depthScratchBytes = 0;
  D3D12_RESOURCE_STATES depthScratchState = D3D12_RESOURCE_STATE_COPY_DEST;
} g_nat;

// Root signature + the 7 resolve pipelines (worker thread or, for the replay, inline). aotOnly (SR_NB_AOT): only from
// the shipped blobs, false (nothing built, nothing failed) when one is missing - the caller then compiles.
bool natBuildPipelines(ID3D12Device* dev, bool aotOnly = false) {
  {
    auto fail = [&](const char* what, const char* detail) {
      g_nat.failed.store(true, std::memory_order_release);
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_RESOLVE] init failed: %s %s", what, detail ? detail : "");
      return false;
    };
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 1;
    D3D12_DESCRIPTOR_RANGE rangeD{};  // iter-467 native-b: t3/t4, the depth owner's plane SRVs
    rangeD.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangeD.NumDescriptors = 2;
    rangeD.BaseShaderRegister = 3;
    D3D12_ROOT_PARAMETER p[9]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 5;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1;
    p[2].DescriptorTable.pDescriptorRanges = &range;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u1 gNatFront
    p[3].Descriptor.ShaderRegister = 1;
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1 gNatFrontPitch
    p[4].Constants.ShaderRegister = 1;
    p[4].Constants.Num32BitValues = 1;
    p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t2 gNatDepth
    p[5].Descriptor.ShaderRegister = 2;
    p[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b2 depth plane layout
    p[6].Constants.ShaderRegister = 2;
    p[6].Constants.Num32BitValues = 24;
    p[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[7].DescriptorTable.NumDescriptorRanges = 1;
    p[7].DescriptorTable.pDescriptorRanges = &rangeD;
    p[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[8].Descriptor.ShaderRegister = 4;
    for (auto& x : p) x.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 9;
    rd.pParameters = p;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
      return fail("root signature", err ? static_cast<const char*>(err->GetBufferPointer()) : nullptr);
    if (FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&g_nat.rs))))
      return fail("root signature (create)", nullptr);
    std::vector<uint8_t> codes[7];
    for (int k = 0; k < 7; k++) {
      char kv[4];
      std::snprintf(kv, sizeof(kv), "%d", k);
      // iter-466 res4k: the scaled variant (resolve.xesli XE_RESOLVE_RESOLUTION_SCALED: 4 push
      // constants, destination relative to the bound scaled-memory address).
      const bool scaledSet = resscale::get().scaled();
      // iter-467 freezefx: SR_PM4_HALF_SIGNED=0 compiles the old unsigned half-overflow compare.
      const bool hexCmp = !pm4HalfSignedOn();
      // iter-467 native-b: packed with no hole - a {nullptr, nullptr} entry ENDS a D3D_SHADER_MACRO list, so at 1x
      // (no XE_RESOLVE_RESOLUTION_SCALED) a define listed after it was dropped.
      D3D_SHADER_MACRO defs[4] = {{"NAT_KIND", kv}, {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr}};
      int nd = 1;
      if (scaledSet) defs[nd++] = {"XE_RESOLVE_RESOLUTION_SCALED", "1"};
      if (hexCmp) defs[nd++] = {"SR_HALF_HEXCMP", "1"};
      std::string cerr;
      if (!nbAotCode("nat_resolve", kNativeResolveHlsl, sizeof(kNativeResolveHlsl) - 1,
                     "gpu/shaders/sr_native_resolve.cs.hlsl", defs, aotOnly, codes[k], cerr)) {
        if (aotOnly) return false;
        return fail("compile", cerr.c_str());
      }
    }
    for (int k = 0; k < 7; k++) {
      char kv[4];
      std::snprintf(kv, sizeof(kv), "%d", k);
      D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
      cd.pRootSignature = g_nat.rs.Get();
      cd.CS = {codes[k].data(), codes[k].size()};
      if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&g_nat.pso[k]))))
        return fail("pso", kv);
    }
    g_nat.ready.store(true, std::memory_order_release);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_RESOLVE] ready: 7 resolve CS %s (embedded Xenia xesl sources)%s",
                aotOnly ? "from the shipped blobs (SR_NB_AOT)" : "compiled", resscale::get().scaled() ? ", resolution-scaled" : "");
  }
  return true;
}

// A refusal: counted by reason, the reason kept for the status line; always false.
bool natRefuse(NatRefusal r) {
  g_nat.refused[r]++;
  g_nat.lastRefusal = r;
  switch (r) {
    case kNatRefuseWarm: g_nat.fallbackWarm++; break;
    case kNatRefuseDepth: g_nat.fallbackDepth++; break;
    default: g_nat.fallbackOther++; break;
  }
  return false;
}

// The pipelines' build, started once (release/emu-stage3, Astra round 1 P2): from the shipped blobs inline
// (SR_NB_AOT; no compile), else compiled - in the dev build on a worker thread (every resolve takes the EDRAM path
// until they are ready; SR_NATIVE_RESOLVE_SYNC=1, which the replay sets, compiles inline), in the release
// configuration inline, since nothing there can take a resolve in the meantime (3.1 rule 2: the thread + the
// EDRAM-path warm-up is the oracle build's arm). pm4ShmInit calls it first, so a missing / stale blob set compiles
// at init ([NB_AOT] compiled, ~25 s once) instead of at the first resolve. wait: block until ready or failed (a
// build another thread started; the release configuration's resolve path).
void natStartPipelines(ID3D12Device* devRaw, bool wait) {
  if (g_nat.ready.load(std::memory_order_acquire) || g_nat.failed.load(std::memory_order_acquire)) return;
  if (!g_nat.started.exchange(true)) {
    Microsoft::WRL::ComPtr<ID3D12Device> dev = devRaw;
    if (nbAotOn() && natBuildPipelines(dev.Get(), /*aotOnly=*/true)) return;  // the shipped blobs, inline
#if SR_ORACLE
    const char* e = std::getenv("SR_NATIVE_RESOLVE_SYNC");
    if (e && e[0] == '1') {
      natBuildPipelines(dev.Get());
    } else {
      std::thread([dev] { natBuildPipelines(dev.Get()); }).detach();
    }
#else
    natBuildPipelines(dev.Get());  // C1 stage 3: inline - the release configuration has no path to take the warm-up
#endif
  }
  if (wait)
    while (!g_nat.ready.load(std::memory_order_acquire) && !g_nat.failed.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void natFrontInvalidate(uint32_t start, uint32_t len) {
  start &= 0x1FFFFFFFu;
  for (NatFront& f : g_nat.front)
    if (f.valid && start < f.dest + f.span && f.dest < start + len) f.valid = false;
}

}  // namespace

#if SR_DIFFTEST
// Compile the real shipping source and embedded closure without consulting
// the AOT cache. These are all combinations the two production builders emit.
int runNativeResolveCompileTest() {
  uint32_t compiled = 0;
  for (bool tex : {false, true}) for (bool scaled : {false, true})
    for (bool hexCompare : {false, true}) for (int kind = 0; kind < 7; ++kind) {
      if (tex && (kind == 2 || kind == 6)) continue;
      char kindText[4];
      std::snprintf(kindText, sizeof kindText, "%d", kind);
      D3D_SHADER_MACRO defs[6]{};
      uint32_t n = 0;
      defs[n++] = {"NAT_KIND", kindText};
      if (tex) defs[n++] = {"NAT_TEX", "1"};
      if (scaled) defs[n++] = {"XE_RESOLVE_RESOLUTION_SCALED", "1"};
      else if (tex) defs[n++] = {"NAT_DUAL", "1"};
      if (hexCompare) defs[n++] = {"SR_HALF_HEXCMP", "1"};
      NatXeslInclude include;
      Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
      const HRESULT hr = D3DCompile(kNativeResolveHlsl, sizeof(kNativeResolveHlsl) - 1,
          "gpu/shaders/sr_native_resolve.cs.hlsl", defs, &include, "main", "cs_5_1",
          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
      if (FAILED(hr) || !code || code->GetBufferSize() <= 32) {
        std::fprintf(stderr, "[NATIVE_RESOLVE_COMPILE] FAIL kind=%d tex=%d scaled=%d hex=%d hr=%08X %s\n",
                     kind, int(tex), int(scaled), int(hexCompare), unsigned(hr),
                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no bytecode");
        return 1;
      }
      ++compiled;
    }
  std::printf("[NATIVE_RESOLVE_COMPILE] PASS variants=%u cache=unused\n", compiled);
  return compiled == 48 ? 0 : 1;
}
#endif
