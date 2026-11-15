#pragma once
// ============================================================================
// xenos_conformance - shared helpers for the Xenia REFERENCE ports.
//
// Part of tests/xenos_conformance (docs/NATIVE_ROADMAP.md section 6.2 "L0
// conformance units"). The xenia_ref_*.hpp headers are the ORACLE the suite
// checks our gfx/ code against. A disagreement is a finding about OUR code,
// never a reason to edit a reference port.
//
// Ported from xenia-canary, snapshot reference/xenia-canary @ 0b4e6c44
// (2026-06-24):
//   src/xenia/base/math.h  - Copyright 2019 Ben Vanik. All rights reserved.
//     clamp_float / saturate (math.h:75-92), align (math.h:63-65),
//     lzcnt(uint32_t) (math.h:191, __lzcnt semantics: 32 for 0).
//
// The d3d:: namespace below is NOT Xenia code: it models the Direct3D shader
// model semantics needed to transcribe Xenia's .xesli / DXBC reference code
// into scalar C++ (min/max NaN rules, ftou/ftoi, round_ne, bit-field ops).
//
// ---------------------------------------------------------------------------
// Xenia licence (reference/xenia-canary/LICENSE), BSD-3-Clause:
//
// Copyright (c) 2015, Ben Vanik.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the project nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// ============================================================================

#include <bit>
#include <cmath>
#include <cstdint>

namespace xref {

inline uint32_t AsU32(float f) { return std::bit_cast<uint32_t>(f); }
inline float AsF32(uint32_t u) { return std::bit_cast<float>(u); }

// base/math.h:75-85. For NaN, returns min_value; at a zero boundary the result
// takes the boundary's sign.
template <typename T>
T clamp_float(T value, T min_value, T max_value) {
  float clamped_to_min = std::isgreater(value, min_value) ? value : min_value;
  return std::isless(clamped_to_min, max_value) ? clamped_to_min : max_value;
}
// base/math.h:87-92 - "the same conventions as in shading languages, returning
// 0 for NaN. 0 is always returned as positive."
template <typename T>
T saturate(T value) {
  return clamp_float(value, static_cast<T>(0.0f), static_cast<T>(1.0f));
}
// base/math.h:63-65.
template <typename T>
constexpr T align(T value, T alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
// base/math.h:191 (__lzcnt: 32 for 0).
inline uint32_t lzcnt(uint32_t v) { return uint32_t(std::countl_zero(v)); }

// ---------------------------------------------------------------------------
// Direct3D shader-model semantics (not Xenia code; used to transcribe .xesli
// and DXBC-emitting reference code op by op).
namespace d3d {
// D3D10+ min/max: when exactly one operand is NaN, the other one is returned
// (IEEE 754-2008 minNum/maxNum). For (-0, +0) the second operand is returned,
// so max(-0, 0) = +0 (the Xenia call sites always clamp against a literal).
inline float max(float a, float b) {
  if (a != a) return b;
  if (b != b) return a;
  return a > b ? a : b;
}
inline float min(float a, float b) {
  if (a != a) return b;
  if (b != b) return a;
  return a < b ? a : b;
}
// The `_sat` modifier / HLSL saturate: NaN -> 0 (same as xref::saturate).
inline float sat(float f) { return saturate(f); }
// ftou: NaN and negatives -> 0, >= 2^32 -> 0xFFFFFFFF, else round toward zero.
inline uint32_t ftou(float f) {
  if (!(f > 0.0f)) return 0u;
  if (f >= 4294967296.0f) return 0xFFFFFFFFu;
  return uint32_t(f);
}
// ftoi: NaN -> 0, saturating, else round toward zero.
inline int32_t ftoi(float f) {
  if (f != f) return 0;
  if (f >= 2147483648.0f) return INT32_MAX;
  if (f <= -2147483648.0f) return INT32_MIN;
  return int32_t(f);
}
// round_ne: round to nearest even (default FP environment).
inline float round_ne(float f) { return std::nearbyint(f); }
// round_z: round toward zero.
inline float round_z(float f) { return std::trunc(f); }
// DXBC ushr / ishl use the low 5 bits of the shift amount.
inline uint32_t ushr(uint32_t v, uint32_t s) { return v >> (s & 31u); }
inline uint32_t ishl(uint32_t v, uint32_t s) { return v << (s & 31u); }
// DXBC ubfe / bfi (width < 32 at every call site here).
inline uint32_t ubfe(uint32_t width, uint32_t offset, uint32_t v) {
  return (v >> offset) & ((1u << width) - 1u);
}
inline uint32_t bfi(uint32_t width, uint32_t offset, uint32_t src,
                    uint32_t base) {
  const uint32_t mask = ((1u << width) - 1u) << offset;
  return (base & ~mask) | ((src << offset) & mask);
}
// DXBC firstbit_hi: bit index counted from the MSB, 0xFFFFFFFF for 0.
inline uint32_t firstbit_hi(uint32_t v) { return v ? lzcnt(v) : 0xFFFFFFFFu; }
// HLSL firstbithigh (xesli first_one_bit_high_xe): index from the LSB, -1 for 0.
inline uint32_t firstbithigh(uint32_t v) { return v ? 31u - lzcnt(v) : 0xFFFFFFFFu; }
}  // namespace d3d

}  // namespace xref
