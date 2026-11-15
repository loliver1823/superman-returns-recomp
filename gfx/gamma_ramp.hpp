#pragma once

// Xenos display gamma ramp (the DC_LUT registers) - the console's scan-out
// applies it to every presented pixel, so a port that presents the front
// buffer raw is brighter or darker than the game intends.
//
// Ported from xenia-canary (BSD):
//   defaults            src/xenia/gpu/command_processor.cc:163-183
//   register writes     src/xenia/gpu/command_processor.cc:562-680
//   application         src/xenia/gpu/shaders/apply_gamma_table.xesli,
//                       apply_gamma_pwl.xesli
//   table vs PWL        d3d12_command_processor.cc:2227-2236 (8_8_8_8 front
//                       buffer -> 256-entry table; 2_10_10_10 -> PWL)
//
// Pure state + arithmetic: no device, no guest memory. The trace replay fills
// it from the trace's kGammaRamp snapshot; register writes (PM4 type-0 or the
// MMIO aperture) go through onRegisterWrite.

#include <cstdint>

namespace sr::gfx::gamma {

constexpr uint32_t kRegRwMode = 0x1921;       // bit 0: 1 = PWL, 0 = 256-entry table
constexpr uint32_t kRegRwIndex = 0x1922;
constexpr uint32_t kRegSeqColor = 0x1923;
constexpr uint32_t kRegPwlData = 0x1924;
constexpr uint32_t kReg30Color = 0x1925;
constexpr uint32_t kRegWriteEnMask = 0x1927;  // bit 0 blue, 1 green, 2 red

struct Ramp {
  // DC_LUT_30_COLOR: blue bits 0:9, green 10:19, red 20:29.
  uint32_t table[256];
  // DC_LUT_PWL_DATA: base bits 0:15, delta bits 16:31; [segment][r, g, b].
  uint32_t pwl[128][3];
  // Component index (0 r, 1 g, 2 b) of the next SEQ_COLOR / PWL_DATA write.
  uint32_t rwComponent = 0;
  // Bumped on every change, so a presenter can cache an uploaded copy.
  uint64_t version = 0;

  Ramp() { reset(); }

  // Linear: what games set for the sRGB VdGetCurrentDisplayGamma return.
  void reset() {
    for (uint32_t i = 0; i < 256; i++) {
      const uint32_t v = i * 0x3FFu / 0xFFu;
      table[i] = v | (v << 10) | (v << 20);
    }
    for (uint32_t i = 0; i < 128; i++) {
      const uint32_t base = (i * 0xFFFFu / 0x7Fu) & ~0x3Fu;
      const uint32_t delta = i < 0x7Fu ? 0x200u : 0u;
      for (uint32_t j = 0; j < 3; j++) pwl[i][j] = base | (delta << 16);
    }
    rwComponent = 0;
    version++;
  }

  // The side effects of a register write, after the register file already
  // holds `value`. `Regs` provides read(reg) and write(reg, value) (the
  // RW_INDEX auto-increment is itself a register write).
  template <class Regs>
  void onRegisterWrite(uint32_t reg, uint32_t value, Regs& regs) {
    switch (reg) {
      case kRegRwIndex:
        rwComponent = 0;
        break;
      case kRegSeqColor: {
        const uint32_t index = regs.read(kRegRwIndex) & 0xFFu;
        // SEQ_COLOR is red, green, blue; the write-enable mask is blue, green, red.
        const bool en = (regs.read(kRegWriteEnMask) & (1u << (2u - rwComponent))) != 0;
        if (en) {
          const uint32_t c = (value & 0xFFFFu) >> 6;  // bits 0:5 hardwired zero
          const uint32_t shift = rwComponent == 0 ? 20u : rwComponent == 1 ? 10u : 0u;
          table[index] = (table[index] & ~(0x3FFu << shift)) | (c << shift);
          version++;
        }
        if (++rwComponent >= 3u) {
          const uint32_t ri = regs.read(kRegRwIndex);
          regs.write(kRegRwIndex, (ri & ~0xFFu) | ((ri + 1u) & 0xFFu));
          rwComponent = 0;
        }
        break;
      }
      case kRegPwlData: {
        const uint32_t ri = regs.read(kRegRwIndex);
        const uint32_t seg = ri & 0x7Fu;  // bit 7 is ignored for PWL
        const bool en = (regs.read(kRegWriteEnMask) & (1u << (2u - rwComponent))) != 0;
        if (en) {
          pwl[seg][rwComponent] = value & ~((0x3Fu << 16) | 0x3Fu);
          version++;
        }
        if (++rwComponent >= 3u) {
          regs.write(kRegRwIndex, (ri & ~0x7Fu) | ((seg + 1u) & 0x7Fu));
          rwComponent = 0;
        }
        break;
      }
      case kReg30Color: {
        const uint32_t ri = regs.read(kRegRwIndex);
        const uint32_t index = ri & 0xFFu;
        const uint32_t en = regs.read(kRegWriteEnMask) & 7u;
        if (en) {
          uint32_t e = table[index];
          if (en & 1u) e = (e & ~0x3FFu) | (value & 0x3FFu);
          if (en & 2u) e = (e & ~(0x3FFu << 10)) | (value & (0x3FFu << 10));
          if (en & 4u) e = (e & ~(0x3FFu << 20)) | (value & (0x3FFu << 20));
          table[index] = e;
          version++;
        }
        regs.write(kRegRwIndex, (ri & ~0xFFu) | ((ri + 1u) & 0xFFu));
        rwComponent = 0;
        break;
      }
      default:
        break;
    }
  }

  // 256-entry table, one 8-bit channel (0 r, 1 g, 2 b) -> 10-bit.
  uint32_t table10(uint32_t v8, uint32_t ch) const {
    const uint32_t e = table[v8 & 0xFFu];
    return ch == 0 ? (e >> 20) & 0x3FFu : ch == 1 ? (e >> 10) & 0x3FFu : e & 0x3FFu;
  }
  // 10-bit -> 8-bit the way a UNORM store rounds (x * 255 / 1023, nearest).
  static uint8_t to8(uint32_t v10) { return uint8_t((v10 * 255u + 511u) / 1023u); }

  // PWL, one 10-bit channel -> output in [0, 1] (apply_gamma_pwl.xesli: 128
  // segments of 8, output = base + (input & 7) * delta / 8, 16-bit scale).
  float pwl01(uint32_t v10, uint32_t ch) const {
    const uint32_t e = pwl[(v10 >> 3) & 0x7Fu][ch];
    const float base = float(e & 0xFFFFu), delta = float(e >> 16);
    const float o = (base + float(v10 & 7u) * delta * (1.0f / 8.0f)) * (1.0f / (64.0f * 1023.0f));
    return o < 0.0f ? 0.0f : o > 1.0f ? 1.0f : o;
  }
};

// The console has one display ramp.
inline Ramp& displayRamp() {
  static Ramp r;
  return r;
}

}  // namespace sr::gfx::gamma
