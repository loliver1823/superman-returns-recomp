#pragma once
// ============================================================================
// xenos_conformance - Xenia REFERENCE ports: resolve (copy) setup and math.
//
// The oracle for the resolve rectangle (fixed-point vertices, window offset,
// scissor, 8-pixel alignment, pitch / height clamps), copy_sample_select
// sanitizing, the destination exponent bias and MSAA averaging factor, the
// clear-on-resolve predicates, the destination endian swaps and the set of
// resolvable destination formats. Checked against our gfx/edram.hpp by
// conformance_main.cpp. Never edit a function here to match our code.
//
// Ported from xenia-canary, snapshot reference/xenia-canary @ 0b4e6c44
// (2026-06-24), cited file:line:
//   src/xenia/gpu/draw_util.cc             Copyright 2023 Ben Vanik. All rights reserved.
//   src/xenia/gpu/draw_util.h              Copyright 2022 Ben Vanik. All rights reserved.
//   src/xenia/ui/graphics_util.cc          Copyright 2021 Ben Vanik. All rights reserved.
//   src/xenia/gpu/shaders/resolve.xesli    Copyright 2022 Ben Vanik. All rights reserved.
//   src/xenia/gpu/shaders/endian.xesli     Copyright 2022 Ben Vanik. All rights reserved.
//   src/xenia/gpu/shaders/edram.xesli      Copyright 2022 Ben Vanik. All rights reserved.
//   src/xenia/gpu/registers.h              Copyright 2025 Ben Vanik. All rights reserved.
//   src/xenia/gpu/xenos.h                  Copyright 2020 Ben Vanik. All rights reserved.
//   src/xenia/gpu/texture_info_formats.inl (included by texture_info_formats.cc,
//                                          Copyright 2014 Ben Vanik. All rights reserved.)
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
namespace resolve {

// xenos.h:1022-1029
constexpr uint32_t kResolveAlignmentPixelsLog2 = 3;
constexpr uint32_t kResolveAlignmentPixels = 1 << kResolveAlignmentPixelsLog2;
constexpr uint32_t kResolveSizeBits = 14;
constexpr uint32_t kMaxResolveSize =
    (1 << kResolveSizeBits) - kResolveAlignmentPixels;
constexpr uint32_t kMaxColorRenderTargets = 4;

// xenos.h:1039-1047 CopySampleSelect; MsaaSamples k1X=0, k2X=1, k4X=2.
enum : uint32_t { k0, k1, k2, k3, k01, k23, k0123 };
enum : uint32_t { kMsaa1X = 0, kMsaa2X = 1, kMsaa4X = 2 };

// xenos.h:465-476 GetSurfacePitchTiles (kEdramTileWidthSamples = 80).
constexpr uint32_t GetSurfacePitchTiles(uint32_t pitch_pixels,
                                        uint32_t msaa_samples, bool is_64bpp) {
  uint32_t pitch_samples = pitch_pixels << uint32_t(msaa_samples >= kMsaa4X);
  uint32_t pitch_tiles = (pitch_samples + (80 - 1)) / 80;
  if (is_64bpp) {
    pitch_tiles <<= 1;
  }
  return pitch_tiles;
}

// draw_util.cc:839-880 SanitizeCopySampleSelect
inline uint32_t SanitizeCopySampleSelect(uint32_t copy_sample_select,
                                         uint32_t msaa_samples, bool is_depth) {
  if (msaa_samples >= kMsaa4X) {
    if (copy_sample_select > k0123) {
      copy_sample_select = k0123;
    }
    if (is_depth) {
      switch (copy_sample_select) {
        case k01:
        case k0123:
          copy_sample_select = k0;
          break;
        case k23:
          copy_sample_select = k2;
          break;
        default:
          break;
      }
    }
  } else if (msaa_samples >= kMsaa2X) {
    switch (copy_sample_select) {
      case k2:
        copy_sample_select = k0;
        break;
      case k3:
        copy_sample_select = k1;
        break;
      default:
        if (copy_sample_select > k01) {
          copy_sample_select = k01;
        }
    }
    if (is_depth && copy_sample_select == k01) {
      copy_sample_select = k0;
    }
  } else {
    copy_sample_select = k0;
  }
  return copy_sample_select;
}

// ui/graphics_util.cc:17-60 FloatToD3D11Fixed16p8
inline int32_t FloatToD3D11Fixed16p8(float f32) {
  if (!(std::abs(f32) >= 1.0f / 512.0f)) {
    return 0;
  }
  if (f32 >= 32768.0f - 1.0f / 256.0f) {
    return (1 << 23) - 1;
  }
  if (f32 <= -32768.0f) {
    return -32768 * 256;
  }
  uint32_t f32_bits = AsU32(f32);
  union {
    int32_t s;
    uint32_t u;
  } result;
  result.u = (f32_bits & ((1 << 23) - 1)) | (1 << 23);
  if ((f32_bits >> 31) != 0) {
    result.s = -result.s;
  }
  int32_t exponent = int32_t((f32_bits >> 23) & 255) - 127;
  uint32_t extra_bits = uint32_t(15 - exponent);
  if (extra_bits) {
    result.u += (1 << (extra_bits - 1)) - 1 + ((result.u >> extra_bits) & 1);
    result.s >>= extra_bits;
  }
  return result.s;
}

// Register field decoding, registers.h bit-field layouts.
inline int32_t SignExtend(uint32_t v, uint32_t bits) {
  return int32_t(v << (32 - bits)) >> (32 - bits);
}
struct ScissorRegs {
  uint32_t pa_sc_window_scissor_tl = 0;  // registers.h:621-633 (14-bit, bit 31 window_offset_disable)
  uint32_t pa_sc_window_scissor_br = 0;  // registers.h:635-645 (14-bit)
  uint32_t pa_sc_window_offset = 0;      // registers.h:609-619 (signed 15-bit)
  uint32_t pa_sc_screen_scissor_tl = 0;  // registers.h:585-595 (signed 15-bit)
  uint32_t pa_sc_screen_scissor_br = 0;  // registers.h:597-607 (signed 15-bit)
  uint32_t rb_surface_info = 0;          // registers.h:670-677
};
struct Scissor {
  uint32_t offset[2];
  uint32_t extent[2];
};

// draw_util.cc:640-716 GetScissorTmpl - the portable (#else) branch; the
// AMD64 SSE branch (draw_util.cc:553-639) computes the same values.
inline void GetScissor(const ScissorRegs& regs, Scissor& scissor_out,
                       bool clamp_to_surface_pitch) {
  uint32_t surface_pitch = 0;
  if (clamp_to_surface_pitch) {
    surface_pitch = regs.rb_surface_info & 0x3FFF;
  }
  uint32_t pa_sc_window_scissor_tl_tl_x = regs.pa_sc_window_scissor_tl & 0x3FFF,
           pa_sc_window_scissor_tl_tl_y =
               (regs.pa_sc_window_scissor_tl >> 16) & 0x3FFF,
           pa_sc_window_scissor_br_br_x = regs.pa_sc_window_scissor_br & 0x3FFF,
           pa_sc_window_scissor_br_br_y =
               (regs.pa_sc_window_scissor_br >> 16) & 0x3FFF,
           pa_sc_window_offset_window_x_offset =
               uint32_t(SignExtend(regs.pa_sc_window_offset & 0x7FFF, 15)),
           pa_sc_window_offset_window_y_offset = uint32_t(
               SignExtend((regs.pa_sc_window_offset >> 16) & 0x7FFF, 15)),
           pa_sc_screen_scissor_tl_tl_x =
               uint32_t(SignExtend(regs.pa_sc_screen_scissor_tl & 0x7FFF, 15)),
           pa_sc_screen_scissor_tl_tl_y = uint32_t(
               SignExtend((regs.pa_sc_screen_scissor_tl >> 16) & 0x7FFF, 15)),
           pa_sc_screen_scissor_br_br_x =
               uint32_t(SignExtend(regs.pa_sc_screen_scissor_br & 0x7FFF, 15)),
           pa_sc_screen_scissor_br_br_y = uint32_t(
               SignExtend((regs.pa_sc_screen_scissor_br >> 16) & 0x7FFF, 15));

  int32_t tl_x = int32_t(pa_sc_window_scissor_tl_tl_x);
  int32_t tl_y = int32_t(pa_sc_window_scissor_tl_tl_y);
  int32_t br_x = int32_t(pa_sc_window_scissor_br_br_x);
  int32_t br_y = int32_t(pa_sc_window_scissor_br_br_y);

  int32_t window_offset_disable_mask =
      ~(static_cast<int32_t>(regs.pa_sc_window_scissor_tl) >> 31);
  tl_x += pa_sc_window_offset_window_x_offset & window_offset_disable_mask;
  tl_y += pa_sc_window_offset_window_y_offset & window_offset_disable_mask;
  br_x += pa_sc_window_offset_window_x_offset & window_offset_disable_mask;
  br_y += pa_sc_window_offset_window_y_offset & window_offset_disable_mask;

  tl_x = std::max(tl_x, int32_t(pa_sc_screen_scissor_tl_tl_x));
  tl_y = std::max(tl_y, int32_t(pa_sc_screen_scissor_tl_tl_y));
  br_x = std::min(br_x, int32_t(pa_sc_screen_scissor_br_br_x));
  br_y = std::min(br_y, int32_t(pa_sc_screen_scissor_br_br_y));
  if (clamp_to_surface_pitch) {
    tl_x = std::min(tl_x, int32_t(surface_pitch));
    br_x = std::min(br_x, int32_t(surface_pitch));
  }
  tl_x = std::max(tl_x, int32_t(0));
  tl_y = std::max(tl_y, int32_t(0));
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  scissor_out.offset[0] = uint32_t(tl_x);
  scissor_out.offset[1] = uint32_t(tl_y);
  scissor_out.extent[0] = uint32_t(br_x - tl_x);
  scissor_out.extent[1] = uint32_t(br_y - tl_y);
}

// Inputs of the rectangle part of GetResolveInfo.
struct ResolveRectIn {
  uint32_t rb_copy_control = 0;    // registers.h:853-867
  uint32_t pa_su_vtx_cntl = 0;     // registers.h:493-500, pix_center bit 0 (0 = kD3DZero)
  uint32_t pa_su_sc_mode_cntl = 0; // registers.h:475, vtx_window_offset_enable bit 16
  ScissorRegs scissor;             // includes PA_SC_WINDOW_OFFSET, RB_SURFACE_INFO
  float vertices[6] = {};          // vf0, already byte-swapped (GpuSwap)
};
struct ResolveRectOut {
  bool ok = false;     // GetResolveInfo's return value
  bool empty = true;   // width_div_8 == 0 || height_div_8 == 0
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// draw_util.cc:926-1074 GetResolveInfo, the part that computes the copied
// rectangle (the vertex-fetch type/size check at :952-957 is assumed to pass).
inline ResolveRectOut GetResolveRect(const ResolveRectIn& in) {
  ResolveRectOut out;
  const uint32_t copy_command = (in.rb_copy_control >> 20) & 3;
  if (copy_command != 0 /* kRaw */ && copy_command != 1 /* kConvert */) {
    return out;  // :940-947
  }
  float half_pixel_offset = (in.pa_su_vtx_cntl & 1) == 0 ? 0.5f : 0.0f;
  int32_t vertices_fixed[6];
  for (size_t i = 0; i < 6; ++i) {
    vertices_fixed[i] = FloatToD3D11Fixed16p8(in.vertices[i] + half_pixel_offset);
  }
  int32_t x0 = std::min(std::min(vertices_fixed[0], vertices_fixed[2]),
                        vertices_fixed[4]);
  int32_t y0 = std::min(std::min(vertices_fixed[1], vertices_fixed[3]),
                        vertices_fixed[5]);
  int32_t x1 = std::max(std::max(vertices_fixed[0], vertices_fixed[2]),
                        vertices_fixed[4]);
  int32_t y1 = std::max(std::max(vertices_fixed[1], vertices_fixed[3]),
                        vertices_fixed[5]);
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;

  if ((in.pa_su_sc_mode_cntl >> 16) & 1) {
    const int32_t wx = SignExtend(in.scissor.pa_sc_window_offset & 0x7FFF, 15);
    const int32_t wy =
        SignExtend((in.scissor.pa_sc_window_offset >> 16) & 0x7FFF, 15);
    x0 += wx;
    y0 += wy;
    x1 += wx;
    y1 += wy;
  }

  Scissor scissor;
  GetScissor(in.scissor, scissor, false);
  int32_t scissor_right = int32_t(scissor.offset[0] + scissor.extent[0]);
  int32_t scissor_bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), scissor_right);
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), scissor_bottom);
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), scissor_right);
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), scissor_bottom);

  x0 &= ~int32_t(kResolveAlignmentPixels - 1);
  y0 &= ~int32_t(kResolveAlignmentPixels - 1);
  x1 = xref::align(x1, int32_t(kResolveAlignmentPixels));
  y1 = xref::align(y1, int32_t(kResolveAlignmentPixels));

  const uint32_t msaa_samples = (in.scissor.rb_surface_info >> 16) & 3;
  if (msaa_samples > kMsaa4X) {
    return out;  // :1018-1027
  }
  int32_t surface_pitch_aligned =
      int32_t((in.scissor.rb_surface_info & 0x3FFF) &
              ~uint32_t(kResolveAlignmentPixels - 1));
  if (x1 > surface_pitch_aligned) {
    x0 = std::min(x0, surface_pitch_aligned);
    x1 = std::min(x1, surface_pitch_aligned);
  }
  if (y1 - y0 > int32_t(kMaxResolveSize)) {
    y1 = y0 + int32_t(kMaxResolveSize);
  }
  out.ok = true;
  out.x0 = x0;
  out.y0 = y0;
  out.x1 = x1;
  out.y1 = y1;
  if (x0 >= x1 || y0 >= y1) {
    out.empty = true;  // :1065-1069
    return out;
  }
  out.empty = false;
  return out;
}

// edram.xesli:23-58 XeEdramOffsetBytes (resolution scale 1).
inline uint32_t EdramOffsetBytes(uint32_t pixel_x, uint32_t pixel_y,
                                 uint32_t base_tiles, bool wrap,
                                 uint32_t pitch_tiles, uint32_t msaa_samples,
                                 bool is_depth, uint32_t format_ints_log2,
                                 uint32_t pixel_sample_index) {
  uint32_t rt_sample_x = pixel_x << uint32_t(msaa_samples >= kMsaa4X);
  uint32_t rt_sample_y = pixel_y << uint32_t(msaa_samples >= kMsaa2X);
  rt_sample_x += (pixel_sample_index >> 1u) & 1u;
  rt_sample_y += (pixel_sample_index >> 0u) & 1u;
  const uint32_t tile_size_at_32bpp_x = 80u, tile_size_at_32bpp_y = 16u;
  const uint32_t tile_size_samples_x = tile_size_at_32bpp_x >> format_ints_log2;
  const uint32_t tile_size_samples_y = tile_size_at_32bpp_y;
  const uint32_t tile_offset_x = rt_sample_x / tile_size_samples_x;
  const uint32_t tile_offset_y = rt_sample_y / tile_size_samples_y;
  base_tiles += tile_offset_y * pitch_tiles + tile_offset_x;
  rt_sample_x -= tile_offset_x * tile_size_samples_x;
  rt_sample_y -= tile_offset_y * tile_size_samples_y;
  if (is_depth) {
    uint32_t tile_width_half = tile_size_samples_x >> 1u;
    rt_sample_x = uint32_t(int32_t(rt_sample_x) +
                           ((rt_sample_x >= tile_width_half)
                                ? -int32_t(tile_width_half)
                                : int32_t(tile_width_half)));
  }
  uint32_t address =
      base_tiles * (tile_size_at_32bpp_x * tile_size_at_32bpp_y) +
      ((rt_sample_y * tile_size_samples_x + rt_sample_x) << format_ints_log2);
  if (wrap) {
    address %= tile_size_at_32bpp_x * tile_size_at_32bpp_y * 2048u;
  }
  return address << 2;
}

// resolve.xesli:224-234 XeResolveFirstSampleIndex - the first sample read;
// the averaging then adds the next sample row (+1) and, for k0123, the next
// sample column (+2, +3) (resolve.xesli:593-626).
inline uint32_t ResolveFirstSampleIndex(uint32_t sample_select) {
  uint32_t sample_index;
  if (sample_select <= k3) {
    sample_index = sample_select;
  } else if (sample_select == k23) {
    sample_index = 2u;
  } else {
    sample_index = 0u;
  }
  return sample_index;
}

// draw_util.h:669-694 (rb_copy_control predicates; registers.h:853-867).
inline bool IsCopyingDepth(uint32_t rb_copy_control) {
  return (rb_copy_control & 7) >= kMaxColorRenderTargets;
}
inline bool IsClearingDepth(uint32_t rb_copy_control) {
  return ((rb_copy_control >> 9) & 1) != 0;
}
inline bool IsClearingColor(uint32_t rb_copy_control) {
  return !IsCopyingDepth(rb_copy_control) && ((rb_copy_control >> 8) & 1) != 0;
}

// draw_util.cc:1234 - the color exponent bias (copy_dest_exp_bias, signed
// 6-bit at bit 16, registers.h:878); depth resolves use 0. (The +5 at
// draw_util.cc:1281 applies only with the -1..1 k_16_16 host hack, off here.)
inline int32_t CopyDestExpBias(uint32_t rb_copy_dest_info, bool is_depth) {
  return is_depth ? 0 : SignExtend((rb_copy_dest_info >> 16) & 0x3F, 6);
}
// resolve.xesli:135-137 - dest_exp_bias_factor from the packed dest_info
// (the same layout: exp bias in bits 16:21).
inline float DestExpBiasFactor(uint32_t dest_info) {
  return AsF32(uint32_t(
      ((int32_t(dest_info) << (32 - (16 + 6)) >> (32 - 6)) * (1 << 23)) +
      int32_t(AsU32(1.0f))));
}
// resolve.xesli:593-631 (and :649-700, :724-764): the factor applied to the
// SUM of the selected samples - halved for 2 samples, quartered for 4.
inline float ResolveSampleFactor(float dest_exp_bias_factor,
                                 uint32_t sample_select) {
  float exp_bias = dest_exp_bias_factor;
  if (sample_select >= k01) {
    exp_bias *= 0.5f;
    if (sample_select >= k0123) {
      exp_bias *= 0.5f;
    }
  }
  return exp_bias;
}

// endian.xesli:25-75 XeEndianSwap32 / XeEndianSwap64 / XeEndianSwap128, on a
// 16-byte block held as four little-endian dwords. Endian128: 0 none,
// 1 8in16, 2 8in32, 3 16in32, 4 8in64, 5 8in128 (xenos.h:198-205).
inline uint32_t EndianSwap32(uint32_t value, uint32_t endian) {
  if (endian == 1 || endian == 2) {
    value = ((value & 0x00FF00FFu) << 8u) | ((value & 0xFF00FF00u) >> 8u);
  }
  if (endian == 2 || endian == 3) {
    value = (value << 16u) | (value >> 16u);
  }
  return value;
}
inline void EndianSwap128(uint32_t v[4], uint32_t endian) {
  if (endian == 5) {  // 8in128: .wzyx, then 8in32
    std::swap(v[0], v[3]);
    std::swap(v[1], v[2]);
    endian = 2;
  } else if (endian == 4) {  // 8in64: .yxwz, then 8in32
    std::swap(v[0], v[1]);
    std::swap(v[2], v[3]);
    endian = 2;
  }
  for (int i = 0; i < 4; ++i) v[i] = EndianSwap32(v[i], endian);
}

// texture_info_formats.inl:1-64 (line N is TextureFormat N-1): bits per pixel
// of every kResolvable format; 0 = not a resolve destination. (Depth resolves
// use k_24_8 / k_24_8_FLOAT, draw_util.cc:1105-1106.)
inline uint32_t ResolvableBitsPerPixel(uint32_t texture_format) {
  switch (texture_format) {
    case 2:  // k_8
    case 8:  // k_8_A
    case 9:  // k_8_B
      return 8;
    case 3:   // k_1_5_5_5
    case 4:   // k_5_6_5
    case 5:   // k_6_5_5
    case 10:  // k_8_8
    case 15:  // k_4_4_4_4
    case 24:  // k_16
    case 30:  // k_16_FLOAT
      return 16;
    case 6:   // k_8_8_8_8
    case 7:   // k_2_10_10_10
    case 14:  // k_8_8_8_8_A
    case 16:  // k_10_11_11
    case 17:  // k_11_11_10
    case 25:  // k_16_16
    case 31:  // k_16_16_FLOAT
    case 36:  // k_32_FLOAT
    case 50:  // k_8_8_8_8_AS_16_16_16_16
    case 54:  // k_2_10_10_10_AS_16_16_16_16
    case 55:  // k_10_11_11_AS_16_16_16_16
    case 56:  // k_11_11_10_AS_16_16_16_16
      return 32;
    case 26:  // k_16_16_16_16
    case 32:  // k_16_16_16_16_FLOAT
    case 37:  // k_32_32_FLOAT
      return 64;
    case 38:  // k_32_32_32_32_FLOAT
      return 128;
    default:
      return 0;
  }
}

}  // namespace resolve
}  // namespace xref
