#pragma once

// iter-463: masked vertex-shader keys (header-only; no logging, no D3D).
//
// The XDK patches a vertex shader's vfetch instructions at bind time to match the
// vertex declaration (ENGINE_MAP section 6d): data format, stride and offset change,
// the program does not. The shader cache is keyed by XXH3-64 of the PATCHED ucode, so
// a known program bound with a new vertex layout misses it, and its draw is dropped
// ([XLAT_REJECT] gate=1: 16 Warworld VS, iter-463).
//
// The masked key (mkey) is XXH3-64 of the big-endian ucode with, in every vertex fetch
// the control-flow program executes, only these fields zeroed:
//   dword 1: format_comp_all (bit 12), num_format_all (13), format (16-21)
//   dword 2: stride (0-7), offset (8-30)
// i.e. each attribute's memory layout and data format - exactly the .vs.vfetch sidecar
// columns. Control flow, every ALU / texture-fetch instruction and the rest of each
// vfetch (registers, fetch-constant slot, destination swizzle, mini bit, prefetch,
// predicate bits, and the value transforms exp_adjust (bits 24-29, a 2^n scale) and
// signed_rf_mode_all (bit 14), which no sidecar column carries) stay in the key. tools/shader_mkey.py computes the same key offline (the <hash>.vs.mkey
// sidecars); tools/difftest/shader_mkey_test.cpp checks the two agree on real ucode.
//
// The control-flow walk is Xenia's (reference/xenia-canary/src/xenia/gpu/
// shader_translator.cc, Shader::AnalyzeUcode + GatherExecInformation, BSD-3): the CF
// program ends at the lowest exec target, the bound shrinks while scanning, and every
// exec-class CF names `count` instruction slots from `address`, sequence bit 2k = fetch.
// It is the same walk as tools/xtr_draws.py cf_exec_walk.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sr {
namespace gfx {
namespace mkey {

constexpr uint32_t kVfDw1Mask = 0x003F3000u;  // comp_all|num_all|format (exp_adjust, signed_rf stay)
constexpr uint32_t kVfDw2Mask = 0x7FFFFFFFu;  // stride|offset (bit 31 pred_condition stays)

inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline bool isExecOpcode(uint32_t op) {
  // kExec, kExecEnd, kCondExec, kCondExecEnd, kCondExecPred, kCondExecPredEnd,
  // kCondExecPredClean, kCondExecPredCleanEnd (ucode.h ControlFlowOpcode).
  return (op >= 1 && op <= 6) || op == 13 || op == 14;
}

// ucode.h UnpackControlFlowInstructions: two 48-bit CF instructions per 3 dwords.
inline void unpackCfPair(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t out[2][2]) {
  out[0][0] = d0;
  out[0][1] = d1 & 0xFFFFu;
  out[1][0] = (d1 >> 16) | (d2 << 16);
  out[1][1] = d2 >> 16;
}

// Every executed vertex-fetch instruction slot, in program order (each slot once).
// Returns false (no key) when the program has no exec or names a slot past the ucode.
inline bool vfetchSlots(const uint8_t* ucode, size_t bytes, std::vector<uint32_t>& out) {
  out.clear();
  const size_t n = bytes / 4;
  const size_t pairs = n / 3;
  if (!ucode || pairs == 0) return false;
  auto dw = [&](size_t i) { return be32(ucode + i * 4); };
  size_t bound = pairs;
  for (size_t i = 0; i < bound; i++) {
    uint32_t cf[2][2];
    unpackCfPair(dw(3 * i), dw(3 * i + 1), dw(3 * i + 2), cf);
    for (int j = 0; j < 2; j++)
      if (isExecOpcode((cf[j][1] >> 12) & 0xFu) && (cf[j][0] & 0xFFFu) < bound)
        bound = cf[j][0] & 0xFFFu;
  }
  bool sawExec = false;
  for (size_t i = 0; i < bound; i++) {
    uint32_t cf[2][2];
    unpackCfPair(dw(3 * i), dw(3 * i + 1), dw(3 * i + 2), cf);
    for (int j = 0; j < 2; j++) {
      if (!isExecOpcode((cf[j][1] >> 12) & 0xFu)) continue;
      sawExec = true;
      const uint32_t address = cf[j][0] & 0xFFFu;
      const uint32_t count = (cf[j][0] >> 12) & 0x7u;
      uint32_t seq = (cf[j][0] >> 16) & 0xFFFu;
      for (uint32_t k = 0; k < count; k++, seq >>= 2) {
        const uint32_t slot = address + k;
        if (size_t(slot) * 3 + 3 > n) return false;  // malformed: fail closed
        if (!(seq & 1u)) continue;                   // ALU slot
        if ((dw(size_t(slot) * 3) & 0x1Fu) != 0) continue;  // texture fetch
        bool seen = false;
        for (uint32_t s : out) seen |= (s == slot);
        if (!seen) out.push_back(slot);
      }
    }
  }
  return sawExec;
}

// The ucode with the bind-time layout fields of every executed vfetch zeroed (BE bytes).
inline bool maskedUcode(const uint8_t* ucode, size_t bytes, std::vector<uint8_t>& out) {
  std::vector<uint32_t> slots;
  if (!vfetchSlots(ucode, bytes, slots)) return false;
  out.assign(ucode, ucode + (bytes / 4) * 4);
  auto andBe = [&](size_t dwIndex, uint32_t keep) {
    uint8_t* p = out.data() + dwIndex * 4;
    const uint32_t v = be32(p) & keep;
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
  };
  for (uint32_t s : slots) {
    andBe(size_t(s) * 3 + 1, ~kVfDw1Mask);
    andBe(size_t(s) * 3 + 2, ~kVfDw2Mask);
  }
  return true;
}

// One row per vertex fetch that writes a component - exactly the rows XenosRecomp
// writes to <hash>.vs.vfetch (minus usage, which comes from the container's vertex
// elements): a mini fetch takes the slot and stride of the previous full fetch.
struct FetchRow {
  uint32_t instr = 0;         // instruction slot
  uint32_t slot = 0;          // vertex fetch constant (0..95)
  int32_t offsetDwords = 0;
  uint32_t strideDwords = 0;
  uint32_t format = 0;        // ucode VertexFormat
  bool isSigned = false;      // format_comp_all
  bool isInteger = false;     // num_format_all
  int32_t expAdjust = 0;
  bool mini = false;
};

inline bool deriveFetchRows(const uint8_t* ucode, size_t bytes, std::vector<FetchRow>& out) {
  out.clear();
  std::vector<uint32_t> slots;
  if (!vfetchSlots(ucode, bytes, slots)) return false;
  uint32_t lastSlot = 0, lastStride = 0;
  for (uint32_t s : slots) {
    const uint32_t d0 = be32(ucode + size_t(s) * 12);
    const uint32_t d1 = be32(ucode + size_t(s) * 12 + 4);
    const uint32_t d2 = be32(ucode + size_t(s) * 12 + 8);
    FetchRow r;
    r.instr = s;
    r.mini = ((d1 >> 30) & 1u) != 0;
    if (r.mini) {
      r.slot = lastSlot;
      r.strideDwords = lastStride;
    } else {
      r.slot = ((d0 >> 20) & 0x1Fu) * 3 + ((d0 >> 25) & 0x3u);
      r.strideDwords = d2 & 0xFFu;
      lastSlot = r.slot;
      lastStride = r.strideDwords;
    }
    int32_t off = int32_t((d2 >> 8) & 0x7FFFFFu);
    if (off & (1 << 22)) off -= (1 << 23);
    r.offsetDwords = off;
    r.format = (d1 >> 16) & 0x3Fu;
    r.isSigned = ((d1 >> 12) & 1u) != 0;
    r.isInteger = ((d1 >> 13) & 1u) != 0;
    int32_t ea = int32_t((d1 >> 24) & 0x3Fu);
    if (ea >= 32) ea -= 64;
    r.expAdjust = ea;
    bool writes = false;
    for (int c = 0; c < 4; c++) writes |= ((d1 >> (3 * c)) & 7u) <= 3u;
    if (writes) out.push_back(r);
  }
  return true;
}

}  // namespace mkey
}  // namespace gfx
}  // namespace sr
