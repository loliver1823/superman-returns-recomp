// iter-466 cp lane: differential test of gfx/cp_simd.cpp against the scalar loops it replaces
// (pm4_engine.cpp type-0 readBE32 loop; d3d12_pm4_edram.cpp pm4StageVsf raw() / remap()).
// Build + run:
//   clang++ -std=c++20 -O2 -msse4.1 -I. tests/perf/cp_simd_selftest.cpp gfx/cp_simd.cpp -o build/cp_simd_selftest.exe
//   build/cp_simd_selftest.exe          -> "cp_simd selftest: N cases, 0 failed"
// Mutation check: break one lane of remap4 (e.g. drop the clamp) and the test must fail.
#include "gfx/cp_simd.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace sr::gfx::cpsimd;

namespace {

uint32_t readBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// The pm4StageVsf scalar path, verbatim in effect.
uint32_t scalarRemap(uint32_t* dst, const uint8_t* ib, uint32_t n, const RemapParams& p) {
  auto raw = [&](uint32_t i) -> uint32_t {
    if (p.idx32) {
      uint32_t v;
      std::memcpy(&v, ib + size_t(i) * 4u, 4);
      if (p.endian == 2) v = __builtin_bswap32(v);
      else if (p.endian == 3) v = (v >> 16) | (v << 16);
      else if (p.endian == 1) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      return v;
    }
    uint16_t v;
    std::memcpy(&v, ib + size_t(i) * 2u, 2);
    if (p.endian == 1 || p.endian == 2) v = __builtin_bswap16(v);
    return v;
  };
  auto remap = [&](uint32_t r) -> uint32_t {
    uint32_t v = (r + p.offset) & 0xFFFFFFu;
    if (p.clamp) v = std::min(std::max(v, p.minV), p.maxV);
    return v;
  };
  uint32_t cnt = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t r = raw(i);
    dst[cnt++] = (p.cut && r == p.resetIndex) ? 0xFFFFFFFFu : remap(r);
  }
  return cnt;
}

}  // namespace

int main() {
  if (!available()) {
    std::printf("cp_simd selftest: SSE4.1 not available, skipped\n");
    return 0;
  }
  std::mt19937 rng(466);
  uint64_t cases = 0, failed = 0;
  // bswapCopy32
  for (uint32_t n = 0; n < 70; n++) {
    std::vector<uint8_t> src(n * 4 + 3);
    for (auto& b : src) b = uint8_t(rng());
    for (uint32_t mis = 0; mis < 4; mis++) {
      if (n * 4 + mis > src.size()) continue;
      std::vector<uint32_t> a(n + 1, 0xDEADBEEFu), b(n + 1, 0xDEADBEEFu);
      for (uint32_t i = 0; i < n; i++) a[i] = readBE32(src.data() + mis + i * 4);
      bswapCopy32(b.data(), src.data() + mis, n);
      cases++;
      if (a != b) failed++;
    }
  }
  // remapIndices
  for (int it = 0; it < 200000; it++) {
    RemapParams p{};
    p.idx32 = (rng() & 1) != 0;
    p.endian = rng() & 3;
    p.offset = (rng() & 3) == 0 ? 0u : uint32_t(rng());
    p.minV = rng() & 0xFFFF;
    p.maxV = (rng() & 1) ? 0xFFFFFFu : p.minV + (rng() & 0xFFFF);
    if ((rng() & 7) == 0) p.maxV = p.minV - 1;  // clamp off (max < min)
    p.clamp = p.maxV >= p.minV;
    p.cut = (rng() & 1) != 0;
    const uint32_t n = rng() % 67;
    const uint32_t bpi = p.idx32 ? 4u : 2u;
    std::vector<uint8_t> ib(n * bpi + 16);
    for (auto& b : ib) b = uint8_t(rng() & ((rng() & 1) ? 0xFF : 0x03));
    // a reset index that occurs in the data (as the swapped raw value) half the time
    if (n && (rng() & 1)) {
      RemapParams q = p;
      q.cut = false;
      q.offset = 0;
      q.clamp = false;
      std::vector<uint32_t> tmp(n);
      const uint32_t k = rng() % n;
      uint32_t r;
      if (p.idx32) {
        uint32_t v;
        std::memcpy(&v, ib.data() + k * 4, 4);
        if (p.endian == 2) v = __builtin_bswap32(v);
        else if (p.endian == 3) v = (v >> 16) | (v << 16);
        else if (p.endian == 1) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
        r = v;
      } else {
        uint16_t v;
        std::memcpy(&v, ib.data() + k * 2, 2);
        if (p.endian == 1 || p.endian == 2) v = __builtin_bswap16(v);
        r = v;
      }
      p.resetIndex = r & 0xFFFFFFu;
    } else {
      p.resetIndex = (rng() & 1) ? 0xFFFFu : (rng() & 0xFFFFFFu);
    }
    std::vector<uint32_t> a(n + 4, 0x12345678u), b(n + 4, 0x12345678u);
    const uint32_t ca = scalarRemap(a.data(), ib.data(), n, p);
    const uint32_t cb = remapIndices(b.data(), ib.data(), n, p);
    cases++;
    if (ca != cb || a != b) {
      if (failed < 5)
        std::printf("FAIL n=%u idx32=%d endian=%u off=%08X min=%u max=%u clamp=%d cut=%d reset=%08X\n", n,
                    p.idx32 ? 1 : 0, p.endian, p.offset, p.minV, p.maxV, p.clamp ? 1 : 0, p.cut ? 1 : 0,
                    p.resetIndex);
      failed++;
    }
  }
  std::printf("cp_simd selftest: %llu cases, %llu failed\n", (unsigned long long)cases,
              (unsigned long long)failed);
  return failed ? 1 : 0;
}
