// iter-467 p2: OBSERVE-ONLY census of the two vpkd3d128 FLOAT16_4 (type 5) sites.
//
// XenonRecomp's type-5 lowering truncates the mantissa, keeps half denormals and
// saturates above 65504; Xenia's EmulateFLOAT16_4_RoundToEven rounds to nearest
// even, flushes denormals and saturates from 0x47FFE000 (L6 golden-call: 18/44 of
// synthetic edge vectors agree). Whether that is a LIVE divergence depends on the
// production inputs, which these overrides record:
//   sub_823AE810(dst, src, n)  - the float -> half loop under TexEncode_Float16x1 /
//                                _Float16x2 (the D3DX surface codec: CreateTexture
//                                FromImageEx / LoadSurfaceFromSurface -> Texture2D
//                                FillTiledFromMemory -> FilterSurfaceDispatch);
//   sub_8210C090(fmt, .., v1)  - SurfaceClearColorPackByFormat, fmt 7
//                                (k_16_16_16_16_FLOAT) = the other type-5 site.
// Per value: exact (truncation == round-to-even, no denormal / overflow), tie or
// inexact (RNE differs from truncation), denormal, > 65504, NaN / inf. Summary
// lines [VPK_F16_4] at calls 1, 2, 4, 8, ... and at exit. Both delegate to the
// recompiled body unchanged (docs/OVERRIDES.json: observe).
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

extern "C" void __imp__sub_823AE810(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8210C090(PPCContext& __restrict ctx, uint8_t* base);

namespace {

struct Census {
  const char* name;
  std::atomic<uint64_t> calls{0}, values{0}, exact{0}, rneDiff{0}, denorm{0}, over{0}, nanInf{0}, zero{0};
  std::atomic<uint32_t> maxAbs{0};
  uint32_t lr[8] = {};
  std::atomic<unsigned> nLr{0};
  std::mutex lrMu;
  explicit Census(const char* n) : name(n) {}

  void value(uint32_t u) {
    values.fetch_add(1, std::memory_order_relaxed);
    const uint32_t a = u & 0x7FFFFFFFu;
    uint32_t prev = maxAbs.load(std::memory_order_relaxed);
    while (a > prev && !maxAbs.compare_exchange_weak(prev, a, std::memory_order_relaxed)) {}
    if (a == 0) { zero.fetch_add(1, std::memory_order_relaxed); exact.fetch_add(1, std::memory_order_relaxed); return; }
    if (a >= 0x7F800000u) { nanInf.fetch_add(1, std::memory_order_relaxed); return; }
    if (a > 0x477FE000u) { over.fetch_add(1, std::memory_order_relaxed); return; }   // > 65504: XenonRecomp 0x7FFF, Xenia keeps up to 0x47FFE000
    if (a < 0x38800000u) { denorm.fetch_add(1, std::memory_order_relaxed); return; } // half denormal: XenonRecomp keeps, Xenia -> 0
    const uint32_t r = a + 0xC8000000u;                      // Xenia float_to_xenos_half: rebias
    const uint32_t trunc = (r >> 13) & 0x7FFFu;
    const uint32_t rne = ((r + 0xFFFu + ((r >> 13) & 1u)) >> 13) & 0x7FFFu;
    (trunc == rne ? exact : rneDiff).fetch_add(1, std::memory_order_relaxed);
  }

  void call(uint32_t callerLr) {
    const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    {
      std::lock_guard<std::mutex> g(lrMu);
      const unsigned k = nLr.load(std::memory_order_relaxed);
      bool seen = false;
      for (unsigned i = 0; i < k; ++i) seen |= lr[i] == callerLr;
      if (!seen && k < 8) { lr[k] = callerLr; nLr.store(k + 1, std::memory_order_relaxed); }
    }
    if ((n & (n - 1)) == 0) report("call");
  }

  void report(const char* why) {
    char lrs[8 * 10] = {};
    std::lock_guard<std::mutex> g(lrMu);
    const unsigned k = nLr.load(std::memory_order_relaxed);
    size_t p = 0;
    for (unsigned i = 0; i < k && p + 10 < sizeof(lrs); ++i) p += (size_t)std::snprintf(lrs + p, sizeof(lrs) - p, "%s%08X", i ? "," : "", lr[i]);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[VPK_F16_4] %s %s: calls=%llu values=%llu exact=%llu rneDiff=%llu denorm=%llu over65504=%llu nanInf=%llu zero=%llu maxAbs=%08X lr=[%s]",
                name, why, (unsigned long long)calls.load(), (unsigned long long)values.load(),
                (unsigned long long)exact.load(), (unsigned long long)rneDiff.load(), (unsigned long long)denorm.load(),
                (unsigned long long)over.load(), (unsigned long long)nanInf.load(), (unsigned long long)zero.load(),
                maxAbs.load(), lrs);
  }
};

Census g_texEncode("sub_823AE810(TexEncode_Float16)");
Census g_clearPack7("sub_8210C090(fmt7)");
std::atomic<uint64_t> g_clearPackByFmt[16];
std::once_flag g_exitOnce;

void atExit() {
  g_texEncode.report("exit");
  g_clearPack7.report("exit");
  char fm[16 * 24] = {};
  size_t p = 0;
  for (int f = 0; f < 16; ++f) {
    const uint64_t n = g_clearPackByFmt[f].load();
    if (n) p += (size_t)std::snprintf(fm + p, sizeof(fm) - p, "%s%d:%llu", p ? " " : "", f, (unsigned long long)n);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[VPK_F16_4] sub_8210C090 calls by format at exit: %s", fm);
}

void armExit() { std::call_once(g_exitOnce, [] { std::atexit(atExit); }); }

}  // namespace

// sub_823AE810(dst r3, src r4, n r5): n float32 (guest BE) -> n half
PPC_FUNC(sub_823AE810) {
  armExit();
  const uint32_t src = ctx.r4.u32, n = ctx.r5.u32;
  if (n && n <= 0x100000u) {
    for (uint32_t i = 0; i < n; ++i) g_texEncode.value(sr::kernel::readGuestU32BE(src + 4 * i));
  }
  g_texEncode.call((uint32_t)ctx.lr);
  __imp__sub_823AE810(ctx, base);
}

// sub_8210C090(fmt r3, ..., v1 = the colour): fmt 7 runs the FLOAT16_4 pack
PPC_FUNC(sub_8210C090) {
  armExit();
  const uint32_t fmt = ctx.r3.u32 & 0xFu;
  g_clearPackByFmt[fmt].fetch_add(1, std::memory_order_relaxed);
  if (fmt == 7) {
    for (int i = 0; i < 4; ++i) g_clearPack7.value(ctx.v1.u32[i]);
    g_clearPack7.call((uint32_t)ctx.lr);
  }
  __imp__sub_8210C090(ctx, base);
}
