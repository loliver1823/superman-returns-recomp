// iter-466 cp lane: SSE4.1 kernels for the command processor (see cp_simd.hpp). Compiled with
// -msse4.1 (CMakeLists.txt); callers reach them only when available() is true.
#include "cp_simd.hpp"

#include <intrin.h>
#include <smmintrin.h>
#include <tmmintrin.h>

#include <cstdlib>
#include <cstring>

namespace sr::gfx::cpsimd {

bool gate(const char* name, bool def) {
  const char* e = std::getenv(name);
  if (!e || !e[0]) return def;
  return e[0] != '0';
}

bool available() {
  static const bool v = [] {
    int r[4];
    __cpuid(r, 1);
    return (r[2] & (1 << 9)) != 0 && (r[2] & (1 << 19)) != 0;  // SSSE3, SSE4.1
  }();
  return v;
}

void bswapCopy32(uint32_t* dst, const uint8_t* srcBE, uint32_t n) {
  const __m128i shuf = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srcBE + size_t(i) * 4u));
    const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srcBE + size_t(i) * 4u + 16u));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_shuffle_epi8(a, shuf));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 4), _mm_shuffle_epi8(b, shuf));
  }
  for (; i < n; i++) {
    uint32_t v;
    std::memcpy(&v, srcBE + size_t(i) * 4u, 4);
    dst[i] = _byteswap_ulong(v);
  }
}

namespace {

inline uint32_t rawScalar(const uint8_t* ib, uint32_t i, const RemapParams& p) {
  if (p.idx32) {
    uint32_t v;
    std::memcpy(&v, ib + size_t(i) * 4u, 4);
    if (p.endian == 2) v = _byteswap_ulong(v);
    else if (p.endian == 3) v = (v >> 16) | (v << 16);
    else if (p.endian == 1) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return v;
  }
  uint16_t v;
  std::memcpy(&v, ib + size_t(i) * 2u, 2);
  if (p.endian == 1 || p.endian == 2) v = _byteswap_ushort(v);
  return v;
}

inline uint32_t remapScalar(uint32_t r, const RemapParams& p) {
  if (p.cut && r == p.resetIndex) return 0xFFFFFFFFu;
  uint32_t v = (r + p.offset) & 0xFFFFFFu;
  if (p.clamp) {
    if (v < p.minV) v = p.minV;
    if (v > p.maxV) v = p.maxV;
  }
  return v;
}

// Four guest indices (already swapped to host order, zero-extended) -> four host indices.
inline __m128i remap4(__m128i r, const RemapParams& p, __m128i off, __m128i m24, __m128i mn,
                      __m128i mx, __m128i reset) {
  __m128i v = _mm_and_si128(_mm_add_epi32(r, off), m24);
  if (p.clamp) v = _mm_min_epu32(_mm_max_epu32(v, mn), mx);
  if (p.cut) v = _mm_or_si128(v, _mm_cmpeq_epi32(r, reset));  // all-ones lanes = 0xFFFFFFFF
  return v;
}

}  // namespace

uint32_t remapIndices(uint32_t* dst, const uint8_t* ib, uint32_t n, const RemapParams& p) {
  const __m128i off = _mm_set1_epi32(int(p.offset)), m24 = _mm_set1_epi32(0xFFFFFF);
  const __m128i mn = _mm_set1_epi32(int(p.minV)), mx = _mm_set1_epi32(int(p.maxV));
  const __m128i reset = _mm_set1_epi32(int(p.resetIndex));
  const __m128i zero = _mm_setzero_si128();
  uint32_t i = 0;
  if (!p.idx32) {
    const bool swap = p.endian == 1 || p.endian == 2;
    for (; i + 8 <= n; i += 8) {
      __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ib + size_t(i) * 2u));
      if (swap) v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i),
                       remap4(_mm_unpacklo_epi16(v, zero), p, off, m24, mn, mx, reset));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 4),
                       remap4(_mm_unpackhi_epi16(v, zero), p, off, m24, mn, mx, reset));
    }
  } else {
    const __m128i bswap32 = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    for (; i + 4 <= n; i += 4) {
      __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ib + size_t(i) * 4u));
      if (p.endian == 2) v = _mm_shuffle_epi8(v, bswap32);
      else if (p.endian == 3) v = _mm_or_si128(_mm_srli_epi32(v, 16), _mm_slli_epi32(v, 16));
      else if (p.endian == 1) v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), remap4(v, p, off, m24, mn, mx, reset));
    }
  }
  for (; i < n; i++) dst[i] = remapScalar(rawScalar(ib, i, p), p);
  return n;
}

}  // namespace sr::gfx::cpsimd
