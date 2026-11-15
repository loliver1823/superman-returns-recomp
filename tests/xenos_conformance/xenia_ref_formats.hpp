#pragma once
// ============================================================================
// xenos_conformance - Xenia REFERENCE ports: pixel / depth formats, PWL gamma.
//
// The oracle for 7e3 (k_2_10_10_10_FLOAT), 20e4 (k_24_8_FLOAT / D24FS8),
// unorm24 (D24S8), 8_8_8_8_GAMMA PWL gamma and the EDRAM colour packs. Checked
// against our gfx/edram.hpp by conformance_main.cpp. Never edit a function
// here to match our code: a disagreement is a finding about OUR code.
//
// Ported from xenia-canary, snapshot reference/xenia-canary @ 0b4e6c44
// (2026-06-24), one function per Xenia function, cited file:line:
//   src/xenia/gpu/xenos.cc                 Copyright 2020 Ben Vanik. All rights reserved.
//   src/xenia/gpu/xenos.h                  Copyright 2020 Ben Vanik. All rights reserved.
//   src/xenia/gpu/shaders/pixel_formats.xesli
//                                          Copyright 2022 Ben Vanik. All rights reserved.
//   src/xenia/gpu/dxbc_shader_translator.cc
//                                          Copyright 2018 Ben Vanik. All rights reserved.
//   src/xenia/gpu/dxbc_shader_translator_om.cc
//                                          Copyright 2021 Ben Vanik. All rights reserved.
//   src/xenia/gpu/d3d12/d3d12_render_target_cache.cc
//                                          Copyright 2022 Ben Vanik. All rights reserved.
//   src/xenia/gpu/render_target_cache.cc   Copyright 2022 Ben Vanik. All rights reserved.
// plus one upstream fix not in the snapshot, applied to a SEPARATE function so
// both are visible: xenia-canary commit f3e42609 (2026-08-04, "[GPU] Fix
// mantissa placement in CPU Float7e3To32", present in
// reference/xenia-canary-oracle).
//
// Transcription rules for shader code (.xesli, and the C++ that EMITS DXBC):
// scalar lanes, D3D semantics from xenia_ref_common.hpp (d3d::max/min NaN
// rules, ftou/ftoi, round_ne/round_z, ubfe/bfi). DXBC `mad` is modelled
// unfused (D3D allows either; every call site here multiplies by a power of
// two or the result is re-rounded, see conformance_main.cpp INFO checks).
//
// Licence: BSD-3-Clause, reference/xenia-canary/LICENSE
//   "Copyright (c) 2015, Ben Vanik. All rights reserved." - the full licence
//   text (conditions + disclaimer) is reproduced in xenia_ref_common.hpp,
//   which this header includes; it applies to every function below.
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "xenia_ref_common.hpp"

namespace xref {

// ---------------------------------------------------------------------------
// src/xenia/gpu/xenos.cc (CPU reference; used by Xenia for clear values and
// viewport depth, e.g. d3d12_render_target_cache.cc:5381/5412/5428,
// draw_util.cc:527-528).
namespace xenos_cc {

// xenos.cc:23-64
inline float PWLGammaToLinear(float gamma) {
  gamma = xref::saturate(gamma);
  float scale, offset;
  if (gamma >= 96.0f / 255.0f) {
    if (gamma >= 192.0f / 255.0f) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else {
    if (gamma >= 64.0f / 255.0f) {
      scale = 2.0f / 1024.0f;
      offset = -64.0f;
    } else {
      scale = 1.0f / 1024.0f;
      offset = 0.0f;
    }
  }
  float linear = gamma * ((255.0f * 1024.0f) * scale) + offset;
  linear += std::trunc(linear * scale);
  linear *= 1.0f / 1023.0f;
  return linear;
}

// xenos.cc:66-100
inline float LinearToPWLGamma(float linear) {
  linear = xref::saturate(linear);
  float scale, offset;
  if (linear >= 128.0f / 1023.0f) {
    if (linear >= 512.0f / 1023.0f) {
      scale = 1023.0f / 8.0f;
      offset = 128.0f / 255.0f;
    } else {
      scale = 1023.0f / 4.0f;
      offset = 64.0f / 255.0f;
    }
  } else {
    if (linear >= 64.0f / 1023.0f) {
      scale = 1023.0f / 2.0f;
      offset = 32.0f / 255.0f;
    } else {
      scale = 1023.0f;
      offset = 0.0f;
    }
  }
  return std::trunc(linear * scale) * (1.0f / 255.0f) + offset;
}

// xenos.cc:104-120 EXACTLY as shipped in the 0b4e6c44 snapshot. Line 119 places
// the 7-bit mantissa at `<< 3` (copied from the 20e4 decoder) - a bug fixed
// upstream by f3e42609. Kept only so the suite can show the snapshot is stale.
inline float Float7e3To32_AsShipped(uint32_t f10) {
  f10 &= 0x3FF;
  if (!f10) {
    return 0.0f;
  }
  uint32_t mantissa = f10 & 0x7F;
  uint32_t exponent = f10 >> 7;
  if (!exponent) {
    uint32_t mantissa_lzcnt = xref::lzcnt(mantissa) - (32 - 8);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7F;
  }
  return AsF32(uint32_t(((exponent + 124) << 23) | (mantissa << 3)));
}

// xenos.cc:104-120 with upstream fix f3e42609 (mantissa << 16).
inline float Float7e3To32(uint32_t f10) {
  f10 &= 0x3FF;
  if (!f10) {
    return 0.0f;
  }
  uint32_t mantissa = f10 & 0x7F;
  uint32_t exponent = f10 >> 7;
  if (!exponent) {
    uint32_t mantissa_lzcnt = xref::lzcnt(mantissa) - (32 - 8);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7F;
  }
  return AsF32(uint32_t(((exponent + 124) << 23) | (mantissa << 16)));
}

// xenos.cc:125-149
inline uint32_t Float32To20e4(float f32, bool round_to_nearest_even) {
  if (!(f32 > 0.0f)) {
    return 0;
  }
  auto f32u32 = AsU32(f32);
  if (f32u32 >= 0x3FFFFFF8) {
    return 0xFFFFFF;
  }
  if (f32u32 < 0x38800000) {
    uint32_t shift = std::min(uint32_t(113 - (f32u32 >> 23)), uint32_t(24));
    f32u32 = (0x800000 | (f32u32 & 0x7FFFFF)) >> shift;
  } else {
    f32u32 += 0xC8000000u;
  }
  if (round_to_nearest_even) {
    f32u32 += 3 + ((f32u32 >> 3) & 1);
  }
  return (f32u32 >> 3) & 0xFFFFFF;
}

// xenos.cc:150-167
inline float Float20e4To32(uint32_t f24) {
  f24 &= 0xFFFFFF;
  if (!f24) {
    return 0.0f;
  }
  uint32_t mantissa = f24 & 0xFFFFF;
  uint32_t exponent = f24 >> 20;
  if (!exponent) {
    uint32_t mantissa_lzcnt = xref::lzcnt(mantissa) - (32 - 21);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFF;
  }
  return AsF32(uint32_t(((exponent + 112) << 23) | (mantissa << 3)));
}

}  // namespace xenos_cc

// src/xenia/gpu/xenos.h:430-436
namespace xenos_h {
inline float UNorm24To32(uint32_t n24) {
  return float(n24 + (n24 >> 23)) * (1.0f / float(1 << 24));
}
}  // namespace xenos_h

// ---------------------------------------------------------------------------
// src/xenia/gpu/shaders/pixel_formats.xesli (shader reference: what Xenia's
// resolve / EDRAM / texture-load shaders compute), one scalar lane.
namespace xesli {

// pixel_formats.xesli:113-116 XePackR8G8B8A8UNorm
inline uint32_t PackR8G8B8A8UNorm(const float f[4]) {
  uint32_t n[4];
  for (int i = 0; i < 4; ++i) n[i] = d3d::ftou(d3d::sat(f[i]) * 255.0f + 0.5f);
  return n[0] | (n[1] << 8) | (n[2] << 16) | (n[3] << 24);
}
// pixel_formats.xesli:118-121 XePackR10G10B10A2UNorm
inline uint32_t PackR10G10B10A2UNorm(const float f[4]) {
  uint32_t n[4];
  for (int i = 0; i < 4; ++i) {
    n[i] = d3d::ftou(d3d::sat(f[i]) * (i == 3 ? 3.0f : 1023.0f) + 0.5f);
  }
  return n[0] | (n[1] << 10) | (n[2] << 20) | (n[3] << 30);
}
// pixel_formats.xesli:98-148 - the remaining resolve-destination packers
// (Direct3D 11.3 UNORM rules: saturate, scale, + 0.5, truncate).
inline uint32_t UNormN(float f, float scale) {
  return d3d::ftou(d3d::sat(f) * scale + 0.5f);
}
inline uint32_t PackR5G5B5A1UNorm(const float f[4]) {  // :98-101
  return UNormN(f[0], 31.0f) | (UNormN(f[1], 31.0f) << 5) |
         (UNormN(f[2], 31.0f) << 10) | (UNormN(f[3], 1.0f) << 15);
}
inline uint32_t PackR5G6B5UNorm(const float f[4]) {  // :103-106
  return UNormN(f[0], 31.0f) | (UNormN(f[1], 63.0f) << 5) |
         (UNormN(f[2], 31.0f) << 11);
}
inline uint32_t PackR5G5B6UNorm(const float f[4]) {  // :108-111
  return UNormN(f[0], 31.0f) | (UNormN(f[1], 31.0f) << 5) |
         (UNormN(f[2], 63.0f) << 10);
}
inline uint32_t PackR4G4B4A4UNorm(const float f[4]) {  // :123-126
  return UNormN(f[0], 15.0f) | (UNormN(f[1], 15.0f) << 4) |
         (UNormN(f[2], 15.0f) << 8) | (UNormN(f[3], 15.0f) << 12);
}
inline uint32_t PackR11G11B10UNorm(const float f[4]) {  // :128-132
  return UNormN(f[0], 2047.0f) | (UNormN(f[1], 2047.0f) << 11) |
         (UNormN(f[2], 1023.0f) << 22);
}
inline uint32_t PackR10G11B11UNorm(const float f[4]) {  // :134-138
  return UNormN(f[0], 1023.0f) | (UNormN(f[1], 2047.0f) << 10) |
         (UNormN(f[2], 2047.0f) << 21);
}
inline uint32_t PackR16G16UNorm(float r, float g) {  // :140-143
  return UNormN(r, 65535.0f) | (UNormN(g, 65535.0f) << 16);
}

// One resolved pixel packed into a destination TextureFormat, as the full
// resolve shaders do it (little-endian GPU value, before the endian swap):
//   8bpp   resolve_full_8bpp.xesli:59-60 (XePackR8G8B8A8UNorm of 4 pixels' red)
//   16bpp  pixel_formats.xesli:163-207 XePack16bpp4PixelsInUInt2
//   32bpp  pixel_formats.xesli:209-266 XePack32bpp4Pixels
//   64bpp  pixel_formats.xesli:268-298 XePack64bpp4Pixels
//   128bpp resolve_full_128bpp.xesli (raw float bits)
// The kXenosFormat_* numbers are pixel_formats.xesli:15-65 (= TextureFormat).
// Returns the byte count, or 0 for the half-float branches (pack_half_2x16_xe
// is the host GPU's f32tof16, not modelled here) and non-resolvable formats.
inline uint32_t PackDestPixel(uint32_t format, const float c[4], uint8_t out[16]) {
  auto put = [&](uint64_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) out[i] = uint8_t(v >> (8 * i));
    return n;
  };
  switch (format) {
    // 8bpp (k_8, k_8_A, k_8_B).
    case 2: case 8: case 9:
      return put(UNormN(c[0], 255.0f), 1);
    // 16bpp.
    case 3: return put(PackR5G5B5A1UNorm(c), 2);
    case 4: return put(PackR5G6B5UNorm(c), 2);
    case 5: return put(PackR5G5B6UNorm(c), 2);
    case 10: return put(UNormN(c[0], 255.0f) | (UNormN(c[1], 255.0f) << 8), 2);
    case 15: return put(PackR4G4B4A4UNorm(c), 2);
    case 24: return put(UNormN(c[0], 65535.0f), 2);
    case 30: return 0;  // k_16_FLOAT: half
    // 32bpp.
    case 6: case 14: case 50: return put(PackR8G8B8A8UNorm(c), 4);
    case 7: case 54: return put(PackR10G10B10A2UNorm(c), 4);
    case 16: case 55: return put(PackR11G11B10UNorm(c), 4);
    case 17: case 56: return put(PackR10G11B11UNorm(c), 4);
    case 13: case 25: return put(PackR16G16UNorm(c[0], c[1]), 4);
    case 31: return 0;  // k_16_16_FLOAT: half
    case 36: return put(AsU32(c[0]), 4);  // default: treat as 32_FLOAT
    // 64bpp.
    case 21: case 26:
      return put(uint64_t(PackR16G16UNorm(c[0], c[1])) |
                     (uint64_t(PackR16G16UNorm(c[2], c[3])) << 32),
                 8);
    case 32: return 0;  // k_16_16_16_16_FLOAT: half
    case 37:  // default: treat as 32_32_FLOAT
      return put(uint64_t(AsU32(c[0])) | (uint64_t(AsU32(c[1])) << 32), 8);
    // 128bpp.
    case 38:
      for (int i = 0; i < 4; ++i) {
        const uint32_t v = AsU32(c[i]);
        for (int b = 0; b < 4; ++b) out[4 * i + b] = uint8_t(v >> (8 * b));
      }
      return 16;
    default:
      return 0;
  }
}

// pixel_formats.xesli:150-154 XePackR16G16Edram (one 16-bit lane)
inline uint32_t PackR16Edram(float f) {
  f = d3d::min(d3d::max(f, -32.0f), 32.0f);
  int32_t n = d3d::ftoi(f * (32767.0f / 32.0f));
  return uint32_t(n & 0xFFFF);
}
inline uint32_t PackR16G16Edram(float r, float g) {
  return PackR16Edram(r) | (PackR16Edram(g) << 16u);
}
// pixel_formats.xesli:304-307 XeUnpackR8G8B8A8UNorm
inline void UnpackR8G8B8A8UNorm(uint32_t p, float out[4]) {
  for (int i = 0; i < 4; ++i) {
    out[i] = float((p >> (8 * i)) & 255u) * (1.0f / 255.0f);
  }
}
// pixel_formats.xesli:313-317 XeUnpackR10G10B10A2UNorm
inline void UnpackR10G10B10A2UNorm(uint32_t p, float out[4]) {
  for (int i = 0; i < 3; ++i) {
    out[i] = float((p >> (10 * i)) & 1023u) * (1.0f / 1023.0f);
  }
  out[3] = float((p >> 30) & 3u) * (1.0f / 3.0f);
}
// pixel_formats.xesli:319-337 XeUnpackR10FloatX4 (one lane)
inline float UnpackR10Float(uint32_t p) {
  uint32_t f10u32 = p & 0x3FFu;
  uint32_t mantissa = f10u32 & 0x7Fu;
  uint32_t exponent = f10u32 >> 7u;
  bool is_denormalized = exponent == 0u;
  uint32_t mantissa_lzcnt = 7u - d3d::firstbithigh(mantissa);
  exponent = is_denormalized ? (1u - mantissa_lzcnt) : exponent;
  mantissa = is_denormalized ? ((mantissa << (mantissa_lzcnt & 31u)) & 0x7Fu)
                             : mantissa;
  return AsF32(f10u32 == 0u ? 0u
                            : (((exponent + 124u) << 23u) | (mantissa << 16u)));
}
// pixel_formats.xesli:339-358 XeUnpackR10G10B10A2Float
inline void UnpackR10G10B10A2Float(uint32_t p, float out[4]) {
  for (int i = 0; i < 3; ++i) out[i] = UnpackR10Float(p >> (10 * i));
  out[3] = float(p >> 30u) * (1.0f / 3.0f);
}
// pixel_formats.xesli:380-388 XeUnpackR16G16Edram
inline void UnpackR16G16Edram(uint32_t p, float out[2]) {
  out[0] = d3d::max(-32.0f, float(int32_t(p << 16) >> 16) * (32.0f / 32767.0f));
  out[1] = d3d::max(-32.0f, float(int32_t(p) >> 16) * (32.0f / 32767.0f));
}
// pixel_formats.xesli:602-613 XeFloat32To20e4
inline uint32_t Float32To20e4(uint32_t f32u32, bool round_to_nearest_even) {
  f32u32 = std::min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  uint32_t denormalized =
      ((f32u32 & 0x7FFFFFu) | 0x800000u) >>
      (std::min(113u - (f32u32 >> 23u), 24u) & 31u);
  uint32_t f24u32 =
      (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24u32 += 3u + ((f24u32 >> 3u) & 1u);
  }
  return (f24u32 >> 3u) & 0xFFFFFFu;
}
// pixel_formats.xesli:615-632 XeFloat20e4To32 (scalar, with the range remap)
inline uint32_t Float20e4To32(uint32_t f24u32, bool remap_to_0_to_0_5) {
  uint32_t mantissa = f24u32 & 0xFFFFFu;
  uint32_t exponent = f24u32 >> 20u;
  bool is_denormalized = exponent == 0u;
  uint32_t mantissa_lzcnt = 20u - d3d::firstbithigh(mantissa);
  exponent = is_denormalized ? (1u - mantissa_lzcnt) : exponent;
  mantissa = is_denormalized
                 ? ((mantissa << (mantissa_lzcnt & 31u)) & 0xFFFFFu)
                 : mantissa;
  return (f24u32 != 0u)
             ? ((exponent + (112u - uint32_t(remap_to_0_to_0_5))) << 23u) |
                   (mantissa << 3u)
             : 0u;
}
// pixel_formats.xesli:655-661 XeUNorm24To32
inline float UNorm24To32(uint32_t n24) {
  return float(n24 + (n24 >> 23u)) * (1.0f / 16777216.0f);
}

}  // namespace xesli

// ---------------------------------------------------------------------------
// The C++ in dxbc_shader_translator(_om).cc that EMITS DXBC, transcribed op by
// op (this is what Xenia's D3D12 backend runs on the GPU).
namespace dxbc {

// dxbc_shader_translator_om.cc:3403-3460 PreClampedFloat32To7e3
inline uint32_t PreClampedFloat32To7e3(uint32_t f32) {
  uint32_t f10;
  if (f32 < 0x3E800000u) {                      // OpULT ... OpIf
    uint32_t temp = f32 >> 23;                  // OpUShR
    temp = uint32_t(125 - int32_t(temp));       // OpIAdd(125, -temp)
    temp = std::min(temp, 24u);                 // OpUMin
    f10 = d3d::bfi(9, 23, 1, f32);              // (f32 & 0x7FFFFF) | 0x800000
    f10 = d3d::ushr(f10, temp);
  } else {
    f10 = f32 + 0xC2000000u;                    // OpIAdd(-124 << 23)
  }
  uint32_t temp = d3d::ubfe(1, 16, f10);
  f10 = f10 + 0x7FFFu;
  f10 = f10 + temp;
  return d3d::ubfe(10, 16, f10);
}
// dxbc_shader_translator_om.cc:3462-3476 UnclampedFloat32To7e3
inline uint32_t UnclampedFloat32To7e3(float f) {
  float c = d3d::max(f, 0.0f);                  // OpMax(f32, 0)
  c = d3d::min(c, 31.875f);                     // OpMin(., 31.875)
  return PreClampedFloat32To7e3(AsU32(c));
}
// dxbc_shader_translator_om.cc:3478-3553 Float7e3To32
inline float Float7e3To32(uint32_t f10_word, uint32_t f10_shift = 0) {
  uint32_t exponent = d3d::ubfe(3, f10_shift + 7, f10_word);
  uint32_t mantissa = d3d::ubfe(7, f10_shift, f10_word);
  if (!exponent) {
    if (mantissa) {
      exponent = d3d::firstbit_hi(mantissa);
      exponent = uint32_t(int32_t(exponent) + (7 - 31));
      mantissa = d3d::ishl(mantissa, exponent);
      exponent = uint32_t(1 - int32_t(exponent));
    } else {
      exponent = uint32_t(-124);
    }
  }
  exponent = uint32_t(int32_t(exponent) * (1 << 23) + (124 << 23));  // OpIMAd
  return AsF32(d3d::bfi(7, 23 - 7, mantissa, exponent));
}
// dxbc_shader_translator_om.cc:3555-3623 PreClampedDepthTo20e4
inline uint32_t PreClampedDepthTo20e4(uint32_t f32, bool round_to_nearest_even,
                                      bool remap_from_0_to_0_5) {
  const uint32_t remap_bias = uint32_t(remap_from_0_to_0_5);
  uint32_t f24;
  if (f32 < 0x38800000u - (remap_bias << 23)) {
    uint32_t temp = f32 >> 23;
    temp = uint32_t(int32_t(113 - remap_bias) - int32_t(temp));
    temp = std::min(temp, 24u);
    f24 = d3d::bfi(9, 23, 1, f32);
    f24 = d3d::ushr(f24, temp);
  } else {
    f24 = f32 + (0xC8000000u + (remap_bias << 23));
  }
  if (round_to_nearest_even) {
    uint32_t temp = d3d::ubfe(1, 3, f24);
    f24 = f24 + 3u;
    f24 = f24 + temp;
  }
  return d3d::ubfe(24, 3, f24);
}
// dxbc_shader_translator_om.cc:3625-3704 Depth20e4To32
inline float Depth20e4To32(uint32_t f24_word, bool remap_to_0_to_0_5,
                           uint32_t f24_shift = 0) {
  const uint32_t remap_bias = uint32_t(remap_to_0_to_0_5);
  uint32_t exponent = d3d::ubfe(4, f24_shift + 20, f24_word);
  uint32_t mantissa = d3d::ubfe(20, f24_shift, f24_word);
  if (!exponent) {
    if (mantissa) {
      exponent = d3d::firstbit_hi(mantissa);
      exponent = uint32_t(int32_t(exponent) + (20 - 31));
      mantissa = d3d::ishl(mantissa, exponent);
      exponent = uint32_t(1 - int32_t(exponent));
    } else {
      exponent = uint32_t(-int32_t(112 - remap_bias));
    }
  }
  exponent = uint32_t(int32_t(exponent) * (1 << 23) +
                      int32_t((112 - remap_bias) << 23));
  return AsF32(d3d::bfi(20, 23 - 20, mantissa, exponent));
}
// dxbc_shader_translator_om.cc:3726-3738 (ROV_DepthTo24Bit, unorm24 branch);
// identical to the dump pipeline's D24S8 pack,
// d3d12_render_target_cache.cc:6239-6249: ftou(round_ne(d * float(0xFFFFFF))).
inline uint32_t DepthToUNorm24(float d32) {
  float d24 = d32 * float(0xFFFFFF);
  d24 = d3d::round_ne(d24);
  return d3d::ftou(d24);
}
// dxbc_shader_translator.cc:225-286 PWLGammaToLinear
inline float PWLGammaToLinear(float source, bool source_pre_saturated) {
  float scale, offset;
  if (source >= 96.0f / 255.0f) {
    const bool hi = source >= 192.0f / 255.0f;
    scale = hi ? 8.0f / 1024.0f : 4.0f / 1024.0f;
    offset = hi ? -1024.0f : -256.0f;
  } else {
    const bool hi = source >= 64.0f / 255.0f;
    scale = hi ? 2.0f / 1024.0f : 1.0f / 1024.0f;
    offset = hi ? -64.0f : 0.0f;
  }
  float target = source_pre_saturated ? source : d3d::sat(source);
  target = target * (255.0f * 1024.0f);
  target = target * scale + offset;             // OpMAd (unfused)
  float t1 = target * scale;
  t1 = d3d::round_z(t1);
  target = target + t1;
  return target * (1.0f / 1023.0f);
}
// dxbc_shader_translator.cc:288-344 PreSaturatedLinearToPWLGamma
inline float PreSaturatedLinearToPWLGamma(float source, bool fused_mad = false) {
  float scale, offset;
  if (source >= 128.0f / 1023.0f) {
    const bool hi = source >= 512.0f / 1023.0f;
    scale = hi ? 1023.0f / 8.0f : 1023.0f / 4.0f;
    offset = hi ? 128.0f / 255.0f : 64.0f / 255.0f;
  } else {
    const bool hi = source >= 64.0f / 1023.0f;
    scale = hi ? 1023.0f / 2.0f : 1023.0f;
    offset = hi ? 32.0f / 255.0f : 0.0f;
  }
  float target = source * scale;
  target = d3d::round_z(target);
  return fused_mad ? std::fma(target, 1.0f / 255.0f, offset)
                   : target * (1.0f / 255.0f) + offset;  // OpMAd
}

}  // namespace dxbc

// ---------------------------------------------------------------------------
// Xenia's D3D12 host-render-target -> EDRAM "dump" packing
// (D3D12RenderTargetCache::GetOrCreateDumpPipeline): the exact analogue of our
// edram::packColor / packDepth (host RT value -> guest EDRAM bits).
namespace dump {

// render_target_cache.cc:40-41 and :105-106 - cvar defaults.
constexpr bool kDepthFloat24Round = false;
constexpr bool kDepthFloat24ConvertInPixelShader = false;

// d3d12_render_target_cache.cc:6250-6256 (kD24FS8): host depth in [0, 1)
// float32 (guest * 0.5) -> 20e4, truncating with the default cvars.
inline uint32_t PackD24FS8(float host_depth,
                           bool depth_float24_round = kDepthFloat24Round) {
  return dxbc::PreClampedDepthTo20e4(
      AsU32(host_depth),
      !kDepthFloat24ConvertInPixelShader && depth_float24_round, true);
}
// d3d12_render_target_cache.cc:6240-6249 (kD24S8).
inline uint32_t PackD24S8(float host_depth) {
  return dxbc::DepthToUNorm24(host_depth);
}
// d3d12_render_target_cache.cc:6268-6279 (k_8_8_8_8, UNORM host source in
// [0, 1]; no saturate - mad + ftou).
inline uint32_t PackColor8888(const float c[4]) {
  uint32_t n[4];
  for (int i = 0; i < 4; ++i) n[i] = d3d::ftou(c[i] * 255.0f + 0.5f);
  uint32_t r = n[0];
  for (uint32_t i = 1; i < 4; ++i) r = d3d::bfi(8, i * 8, n[i], r);
  return r;
}
// d3d12_render_target_cache.cc:6280-6294 (k_8_8_8_8_GAMMA): the host RT holds
// LINEAR values (R16G16B16A16_UNORM); RGB are PWL-encoded on the way to EDRAM.
inline uint32_t PackColor8888Gamma(const float c[4]) {
  float g[4];
  for (int i = 0; i < 3; ++i) g[i] = dxbc::PreSaturatedLinearToPWLGamma(c[i]);
  g[3] = c[3];
  return PackColor8888(g);
}
// d3d12_render_target_cache.cc:6295-6306 (k_2_10_10_10).
inline uint32_t PackColor2101010(const float c[4]) {
  uint32_t n[4];
  for (int i = 0; i < 4; ++i) {
    n[i] = d3d::ftou(c[i] * (i == 3 ? 3.0f : 1023.0f) + 0.5f);
  }
  uint32_t r = n[0];
  for (uint32_t i = 1; i < 4; ++i) r = d3d::bfi(i == 3 ? 2 : 10, i * 10, n[i], r);
  return r;
}
// d3d12_render_target_cache.cc:6307-6333 (k_2_10_10_10_FLOAT): 7e3 RGB from
// the float16 host RT, alpha saturated to 2 bits.
inline uint32_t PackColor2101010Float(const float c[4]) {
  uint32_t r = dxbc::UnclampedFloat32To7e3(c[0]);
  for (uint32_t i = 1; i < 3; ++i) {
    r = d3d::bfi(10, i * 10, dxbc::UnclampedFloat32To7e3(c[i]), r);
  }
  float a = d3d::sat(c[3]);
  a = a * 3.0f + 0.5f;
  return d3d::bfi(2, 30, d3d::ftou(a), r);
}

}  // namespace dump

// ---------------------------------------------------------------------------
// Xenia's ROV (rasterizer-ordered view) output path: the only Xenia path that
// turns a TRUE-range k_16_16 / k_16_16_16_16 colour (-32..32) into EDRAM
// fixed-point bits, like our edram::packColor does from its RG16F/RGBA16F host
// copy. (pixel_formats.xesli:150-161 XePackR16G16Edram truncates instead, but
// has no caller anywhere in the snapshot.)
namespace rov {
// Clamp: dxbc_shader_translator_om.cc:2338-2345 with the k_16_16 bounds from
// render_target_cache.cc:248-256 (GetPSIColorFormatInfo: -32..32).
// Pack: dxbc_shader_translator_om.cc:1428-1455 (ROV_PackPreClampedColor):
// mad(c, 32767/32, c >= 0 ? 0.5 : -0.5), ftoi, low 16 bits.
inline uint32_t PackFixed16(float c) {
  c = d3d::max(c, -32.0f);
  c = d3d::min(c, 32.0f);
  const float half = c >= 0.0f ? 0.5f : -0.5f;
  const float t = c * (32767.0f / 32.0f) + half;
  return uint32_t(d3d::ftoi(t)) & 0xFFFFu;
}
}  // namespace rov

}  // namespace xref
