#pragma once
// ============================================================================
// xenos_conformance - Xenia REFERENCE ports: the display gamma ramp (DC_LUT).
//
// The oracle for the scan-out gamma ramp: default (linear) ramp, the DC_LUT
// register-write side effects (sequential table / PWL writes, RW_INDEX
// auto-increment, write-enable masks) and the two application shaders (the
// 256-entry table for 8_8_8_8 front buffers, the 128-segment PWL for
// 2_10_10_10). Checked against our gfx/gamma_ramp.hpp by conformance_main.cpp.
// Never edit a function here to match our code.
//
// Ported from xenia-canary, snapshot reference/xenia-canary @ 0b4e6c44
// (2026-06-24), cited file:line:
//   src/xenia/gpu/command_processor.cc     Copyright 2022 Ben Vanik. All rights reserved.
//     Initialize gamma defaults (:163-183), HandleSpecialRegisterWrite DC_LUT
//     cases (:562-680), WriteRegister dispatch (:684-713).
//   src/xenia/gpu/registers.h              Copyright 2025 Ben Vanik. All rights reserved.
//     DC_LUT_RW_INDEX / SEQ_COLOR / PWL_DATA / 30_COLOR layouts (:912-962).
//   src/xenia/gpu/register_table.inc       (register indices 0x1921-0x1927, :1061-1099)
//   src/xenia/gpu/shaders/apply_gamma_pwl.xesli
//                                          Copyright 2022 Ben Vanik. All rights reserved.
//     XeApplyPWLGamma (:12-23), input quantization and ramp indexing (:74-93).
//   src/xenia/gpu/shaders/apply_gamma_table.xesli
//                                          Copyright 2022 Ben Vanik. All rights reserved.
//     input quantization and channel selection (:58-71).
//   src/xenia/gpu/d3d12/d3d12_command_processor.cc
//                                          Copyright 2022 Ben Vanik. All rights reserved.
//     ramp upload (:2243-2279: PWL entries uploaded as R16G16 = base, delta).
//
// Licence: BSD-3-Clause, reference/xenia-canary/LICENSE
//   "Copyright (c) 2015, Ben Vanik. All rights reserved." - the full licence
//   text (conditions + disclaimer) is reproduced in xenia_ref_common.hpp,
//   which this header includes; it applies to every function below.
// ============================================================================

#include <cstdint>

#include "xenia_ref_common.hpp"

namespace xref {
namespace gamma_ramp {

// register_table.inc:1061-1099
constexpr uint32_t XE_GPU_REG_DC_LUT_RW_MODE = 0x1921;
constexpr uint32_t XE_GPU_REG_DC_LUT_RW_INDEX = 0x1922;
constexpr uint32_t XE_GPU_REG_DC_LUT_SEQ_COLOR = 0x1923;
constexpr uint32_t XE_GPU_REG_DC_LUT_PWL_DATA = 0x1924;
constexpr uint32_t XE_GPU_REG_DC_LUT_30_COLOR = 0x1925;
constexpr uint32_t XE_GPU_REG_DC_LUT_WRITE_EN_MASK = 0x1927;

// The command-processor state that the DC_LUT registers drive.
struct CommandProcessorGamma {
  // DC_LUT_30_COLOR words: blue 0:9, green 10:19, red 20:29 (registers.h:952-961).
  uint32_t gamma_ramp_256_entry_table_[256];
  // DC_LUT_PWL_DATA words: base 0:15, delta 16:31 (registers.h:935-949).
  uint32_t gamma_ramp_pwl_rgb_[128][3];
  uint32_t gamma_ramp_rw_component_ = 0;
  // The slice of the register file these handlers touch, indexed reg-0x1920.
  uint32_t regs[8] = {};

  uint32_t& reg(uint32_t index) { return regs[index - 0x1920]; }

  // command_processor.cc:163-183 (CommandProcessor::Initialize).
  CommandProcessorGamma() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t value = i * 0x3FF / 0xFF;
      gamma_ramp_256_entry_table_[i] = value | (value << 10) | (value << 20);
    }
    for (uint32_t i = 0; i < 128; ++i) {
      uint32_t base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
      uint32_t delta = i < 0x7F ? 0x200 : 0;
      for (uint32_t j = 0; j < 3; ++j) {
        gamma_ramp_pwl_rgb_[i][j] = (base & 0xFFFF) | (delta << 16);
      }
    }
  }

  // command_processor.cc:684-713 WriteRegister (the DC_LUT dispatch only).
  void WriteRegister(uint32_t index, uint32_t value) {
    reg(index) = value;
    if (index - XE_GPU_REG_DC_LUT_RW_INDEX <=
        XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX) {
      HandleSpecialRegisterWrite(index, value);
    }
  }

  // command_processor.cc:562-680 (the DC_LUT cases).
  void HandleSpecialRegisterWrite(uint32_t index, uint32_t /*value*/) {
    switch (index) {
      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        uint32_t gamma_ramp_rw_index = reg(XE_GPU_REG_DC_LUT_RW_INDEX);
        uint32_t rw_index = gamma_ramp_rw_index & 0xFF;
        bool write_gamma_ramp_component =
            (reg(XE_GPU_REG_DC_LUT_WRITE_EN_MASK) &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          uint32_t& gamma_ramp_entry = gamma_ramp_256_entry_table_[rw_index];
          uint32_t gamma_ramp_seq_color =
              (reg(XE_GPU_REG_DC_LUT_SEQ_COLOR) & 0xFFFF) >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:  // color_10_red
              gamma_ramp_entry = (gamma_ramp_entry & ~(0x3FFu << 20)) |
                                 (gamma_ramp_seq_color << 20);
              break;
            case 1:  // color_10_green
              gamma_ramp_entry = (gamma_ramp_entry & ~(0x3FFu << 10)) |
                                 (gamma_ramp_seq_color << 10);
              break;
            case 2:  // color_10_blue
              gamma_ramp_entry =
                  (gamma_ramp_entry & ~0x3FFu) | gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          uint32_t new_gamma_ramp_rw_index =
              (gamma_ramp_rw_index & ~0xFFu) | ((rw_index + 1) & 0xFF);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX, new_gamma_ramp_rw_index);
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        uint32_t gamma_ramp_rw_index = reg(XE_GPU_REG_DC_LUT_RW_INDEX);
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index & 0x7F;
        bool write_gamma_ramp_component =
            (reg(XE_GPU_REG_DC_LUT_WRITE_EN_MASK) &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          uint32_t& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl]
                                 [gamma_ramp_rw_component_];
          uint32_t gamma_ramp_value = reg(XE_GPU_REG_DC_LUT_PWL_DATA);
          uint32_t base = (gamma_ramp_value & 0xFFFF) & ~UINT32_C(0x3F);
          uint32_t delta = (gamma_ramp_value >> 16) & ~UINT32_C(0x3F);
          gamma_ramp_entry = base | (delta << 16);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          uint32_t rw_index = gamma_ramp_rw_index & 0xFF;
          uint32_t new_rw_index = (rw_index & ~UINT32_C(0x7F)) |
                                  ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        (gamma_ramp_rw_index & ~0xFFu) | new_rw_index);
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        uint32_t gamma_ramp_rw_index = reg(XE_GPU_REG_DC_LUT_RW_INDEX);
        uint32_t rw_index = gamma_ramp_rw_index & 0xFF;
        uint32_t gamma_ramp_write_enable_mask =
            reg(XE_GPU_REG_DC_LUT_WRITE_EN_MASK) & 0b111;
        if (gamma_ramp_write_enable_mask) {
          uint32_t& gamma_ramp_entry = gamma_ramp_256_entry_table_[rw_index];
          uint32_t gamma_ramp_value = reg(XE_GPU_REG_DC_LUT_30_COLOR);
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry =
                (gamma_ramp_entry & ~0x3FFu) | (gamma_ramp_value & 0x3FFu);
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry = (gamma_ramp_entry & ~(0x3FFu << 10)) |
                               (gamma_ramp_value & (0x3FFu << 10));
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry = (gamma_ramp_entry & ~(0x3FFu << 20)) |
                               (gamma_ramp_value & (0x3FFu << 20));
          }
        }
        gamma_ramp_rw_component_ = 0;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      (gamma_ramp_rw_index & ~0xFFu) | ((rw_index + 1) & 0xFF));
      } break;
    }
  }
};

// apply_gamma_pwl.xesli:12-23 XeApplyPWLGamma; ramp_value = (base, delta) as
// uploaded (d3d12_command_processor.cc:2272-2279, R16G16 on little-endian).
inline float ApplyPWLGamma(uint32_t input_value, uint32_t ramp_base,
                           uint32_t ramp_delta) {
  return xref::saturate(
      (float(ramp_base) + float((input_value & 7u) * ramp_delta) * (1.0f / 8.0f)) *
      (1.0f / (64.0f * 1023.0f)));
}
// apply_gamma_pwl.xesli:74-93 for one channel (0 r, 1 g, 2 b) of a 10-bit
// front-buffer value: entry (input >> 3) * 3 + channel.
inline float ApplyPWLGammaChannel(const CommandProcessorGamma& cp,
                                  uint32_t input10, uint32_t channel) {
  const uint32_t e = cp.gamma_ramp_pwl_rgb_[(input10 >> 3) & 0x7F][channel];
  return ApplyPWLGamma(input10, e & 0xFFFF, e >> 16);
}
// apply_gamma_table.xesli:58-71 for one channel of an 8-bit front-buffer
// value: the R10G10B10A2 ramp buffer's .b (bits 20:29) is red, .g green, .r
// (bits 0:9) blue. Returns the 10-bit UNORM the shader reads (it sees /1023).
inline uint32_t ApplyTableChannel10(const CommandProcessorGamma& cp,
                                    uint32_t input8, uint32_t channel) {
  const uint32_t e = cp.gamma_ramp_256_entry_table_[input8 & 0xFF];
  return channel == 0 ? (e >> 20) & 0x3FF
                      : channel == 1 ? (e >> 10) & 0x3FF : e & 0x3FF;
}

}  // namespace gamma_ramp
}  // namespace xref
