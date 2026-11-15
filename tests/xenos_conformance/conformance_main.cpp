// ============================================================================
// xenos_conformance - L0 conformance units for the Xenos native-replacement
// work (docs/NATIVE_ROADMAP.md section 6.2 "L0 conformance units"; roadmap N0
// deliverable 9, "formats and resolve first").
//
// WHAT IT DOES
//   1. Runs Xenia-ported REFERENCE functions (xenia_ref_*.hpp: BSD-3, ported
//      from reference/xenia-canary with a file:line citation per function)
//      over exhaustive or dense input sets, and compares their output with the
//      golden vector files in data/conformance/*.tsv. The goldens pin the
//      references: a drifted port, a compiler change or an FP-mode change
//      shows up as a golden mismatch.
//   2. Compares OUR header-only implementations - gfx/edram.hpp,
//      gfx/xenos_tiling.hpp, gfx/gamma_ramp.hpp - against those references and
//      reports every mismatch with exact inputs. It never modifies our code.
//
// RESULT CLASSES (harness.hpp)
//   PASS    identical to the Xenia reference.
//   FAIL    ours differs from Xenia (default configuration) -> exit code 1.
//   GAP     Xenia does something we have no implementation for.
//   WAIVED  a documented, intentional deviation of ours (reason printed).
//   INFO    facts about Xenia itself (its CPU and shader paths disagreeing, a
//           stale snapshot), census numbers, golden-file creation.
//   GAP and WAIVED fail the run only with --strict.
//
// BUILD (standalone; the game tree is not needed)
//   cmake -S tests/xenos_conformance -B <dir> -G Ninja -DCMAKE_CXX_COMPILER=clang++
//   cmake --build <dir> -j 2
//   In the root build the lead adds: add_subdirectory(tests/xenos_conformance)
//
// RUN
//   xenos_conformance [--data DIR] [--regen | --verify] [--exhaustive]
//                     [--threads N] [--strict] [--only SUBSTRING]
//   --data DIR     golden directory (default: <repo>/data/conformance, baked in
//                  at configure time as XC_DEFAULT_DATA_DIR)
//   (default)      a missing golden file is CREATED from the references (INFO
//                  line); an existing one must match byte for byte
//   --regen        rewrite every golden file from the references
//   --verify       a missing golden file is a FAIL (CI mode)
//   --exhaustive   every float32 bit pattern (2^32) instead of the dense grid
//                  (every 257th pattern plus edge values); takes minutes
//   --threads N    sweep worker threads (default 2)
//   --strict       GAP and WAIVED also fail
//   --only S       run only the groups whose name contains S
//                  (groups: f7e3 f20e4 depth edram_color pwl_gamma resolve
//                   tiling gamma_ramp)
//   Exit code: 0 no FAIL, 1 at least one FAIL, 2 usage error.
//
// GOLDEN REGENERATION
//   Golden files are written ONLY from the reference ports, never from our
//   code, so regenerating cannot hide a defect in gfx/. Regenerate (--regen)
//   only when a reference port changes on purpose (for example re-ported from
//   a newer Xenia), and review the diff of data/conformance/ in that commit.
//
// ATTRIBUTION
//   The reference ports derive from xenia-canary (BSD-3-Clause,
//   reference/xenia-canary/LICENSE: "Copyright (c) 2015, Ben Vanik. All rights
//   reserved."). Each xenia_ref_*.hpp names its exact Xenia source files and
//   their copyright lines; the full licence text is in xenia_ref_common.hpp.
//   This file and harness.hpp are original test code.
// ============================================================================

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "harness.hpp"
#include "xenia_ref_formats.hpp"
#include "xenia_ref_gamma_ramp.hpp"
#include "xenia_ref_resolve.hpp"
#include "xenia_ref_texture_address.hpp"

// OUR implementations under test (header-only, gfx/).
#include "edram.hpp"
#include "gamma_ramp.hpp"
#include "xenos_tiling.hpp"

#ifndef XC_DEFAULT_DATA_DIR
#define XC_DEFAULT_DATA_DIR "data/conformance"
#endif

namespace {

using xc::Fmt;
using xc::Mismatches;
using xc::Status;
using xref::AsF32;
using xref::AsU32;
namespace ed = sr::gfx::edram;
namespace tl = sr::gfx::xenos_tiling;
namespace gm = sr::gfx::gamma;
namespace rs = xref::resolve;

struct Ctx {
  xc::Results res;
  std::filesystem::path data = XC_DEFAULT_DATA_DIR;
  xc::GoldenMode mode = xc::GoldenMode::CreateMissing;
  bool exhaustive = false;
  bool strict = false;
  unsigned threads = 2;
  std::string only;
};

constexpr uint64_t kDenseStep = 257;
constexpr uint32_t kSampleRows = 1024;

uint64_t Step(const Ctx& c) { return c.exhaustive ? 1 : kDenseStep; }
std::string GridName(const Ctx& c) {
  return c.exhaustive ? " [exhaustive]" : " [dense grid: every 257th bit pattern + edges]";
}

// Edge values for float sweeps (bit patterns).
const std::vector<uint32_t>& Specials() {
  static const std::vector<uint32_t> v = {
      0x00000000, 0x80000000, 0x00000001, 0x007FFFFF, 0x00800000, 0x80800000,
      0x3E7FFFFF, 0x3E800000, 0x3E800001,              // 0.25: 7e3 denormal edge
      0x41FF0000, 0x41FF73FF, 0x41FF7400, 0x41FF8000,  // 31.875: 7e3 saturation
      0x42000000, 0x387FFFFF, 0x38800000, 0x38800001,  // 2^-14: 20e4 denormal edge
      0x37FFFFFF, 0x38000000, 0x38000001,              // 2^-15 (host, remapped)
      0x3FFFFFF7, 0x3FFFFFF8, 0x3FFFFFFB, 0x3FFFFFFC, 0x40000000,  // 20e4 saturation
      0x3EFFFFFF, 0x3F000000, 0x3F000001, 0x3F7FFFFF, 0x3F800000, 0x3F800001,
      0xBF800000, 0x3B000000, 0x3AFFFFFF, 0x3B800000, 0xBB000000,  // 1/512, 1/256
      0x46FFFE00, 0x46FFFFFF, 0x47000000, 0xC7000000, 0xC7000001,  // +-32768
      0x42000001, 0xC2000000, 0xC2000001, 0x3A800000, 0x3A800001,  // +-32
      0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0x7FFFFFFF,
      0xFFFFFFFF, 0x3F7FBFC0, 0x3B808081,                           // 255/256, 1/255
  };
  return v;
}
// Deterministic broad sample of bit patterns for golden rows.
uint32_t SampleBits(uint32_t i) { return i * 0x9E3779B1u + 0x01234567u; }

// Compare over float32 bit patterns in [lo, hi] (grid + edge values).
template <class F>
Mismatches SweepBits(const Ctx& c, uint32_t lo, uint32_t hi, const F& fn) {
  Mismatches m = xc::ParallelSweep(lo, uint64_t(hi) + 1, Step(c), c.threads,
                                   [&](uint64_t u, Mismatches& l) { fn(uint32_t(u), l); });
  Mismatches s;
  for (uint32_t u : Specials()) {
    if (u >= lo && u <= hi) fn(u, s);
  }
  m.Merge(s);
  return m;
}

void Compare(Ctx& c, const std::string& id, const Mismatches& m,
             Status onMismatch, const std::string& what,
             const std::string& note = "") {
  if (!m.count) {
    c.res.Report(Status::Pass, id, what + ": " + m.Summary());
  } else {
    c.res.Report(onMismatch, id,
                 what + ": " + m.Summary() +
                     (note.empty() ? "" : "\n           note: " + note));
  }
}
// For agreement between Xenia's own code paths: PASS when they agree, INFO
// (never FAIL - it is not our code) when they do not.
void RefAgree(Ctx& c, const std::string& id, const Mismatches& m,
              const std::string& what, const std::string& note = "") {
  Compare(c, id, m, Status::Info, what, note);
}

// ===========================================================================
// f7e3 - k_2_10_10_10_FLOAT colour channels.
void TestF7e3(Ctx& c) {
  using namespace xref;
  {
    xc::Golden g("f7e3_decode",
                 "7e3 (k_2_10_10_10_FLOAT channel: 7-bit mantissa, 3-bit exponent) -> float32, all 1024 codes");
    g.Comment("sources: pixel_formats.xesli:319-337 XeUnpackR10FloatX4 | dxbc_shader_translator_om.cc:3478-3553 Float7e3To32 |");
    g.Comment("         xenos.cc:104-120 Float7e3To32 with upstream fix f3e42609 | the same as shipped in snapshot 0b4e6c44");
    g.Comment("columns: f10  xesli  dxbc  xenos_cc_fixed  xenos_cc_as_shipped   (float32 bits)");
    Mismatches internal, shipped, ours;
    for (uint32_t f10 = 0; f10 < 1024; ++f10) {
      const uint32_t a = AsU32(xesli::UnpackR10Float(f10));
      const uint32_t b = AsU32(dxbc::Float7e3To32(f10));
      const uint32_t cc = AsU32(xenos_cc::Float7e3To32(f10));
      const uint32_t d = AsU32(xenos_cc::Float7e3To32_AsShipped(f10));
      g.Row(Fmt("%03X\t%08X\t%08X\t%08X\t%08X", f10, a, b, cc, d));
      internal.Check();
      if (a != b || a != cc) {
        internal.Add(Fmt("f10=%03X xesli=%08X dxbc=%08X xenos.cc=%08X", f10, a, b, cc));
      }
      shipped.Check();
      if (d != cc) shipped.Add(Fmt("f10=%03X as_shipped=%08X fixed=%08X", f10, d, cc));
      ours.Check();
      const uint32_t o = AsU32(ed::f7e3ToFloat(f10));
      if (o != a) ours.Add(Fmt("f10=%03X ours=%08X xenia=%08X", f10, o, a));
    }
    g.Finish(c.res, c.data, c.mode);
    RefAgree(c, "ref.f7e3.decode.paths_agree", internal,
             "xesli XeUnpackR10FloatX4 == DXBC Float7e3To32 == xenos.cc Float7e3To32 (+f3e42609)");
    if (shipped.count) {
      c.res.Report(Status::Info, "ref.f7e3.decode.snapshot_stale",
                   "reference/xenia-canary @0b4e6c44 xenos.cc:119 Float7e3To32 places the mantissa at <<3 "
                   "(fixed upstream by f3e42609, present in reference/xenia-canary-oracle); Xenia uses it for "
                   "k_2_10_10_10_FLOAT clear colours (d3d12_render_target_cache.cc:5428). Port 7e3 from the "
                   "oracle tree or the shader code, not that CPU function: " +
                       shipped.Summary());
    }
    Compare(c, "ours.edram.f7e3ToFloat", ours, Status::Fail,
            "gfx/edram.hpp:511 vs pixel_formats.xesli:319 XeUnpackR10FloatX4, all 1024 codes");
  }
  {
    xc::Golden g("f7e3_encode",
                 "float32 -> 7e3 (UnclampedFloat32To7e3: clamp to [0, 31.875], round to nearest even)");
    g.Comment("source: dxbc_shader_translator_om.cc:3403-3476 PreClampedFloat32To7e3 / UnclampedFloat32To7e3");
    g.Comment("columns: f32_bits  f10");
    xc::Fnv h;
    uint64_t n = 0;
    for (uint64_t u = 0; u <= 0xFFFFFFFFull; u += kDenseStep, ++n) {
      h.U32(dxbc::UnclampedFloat32To7e3(AsF32(uint32_t(u))));
    }
    g.Digest("dense_step257_f10", n, h.h);
    for (uint32_t u : Specials()) g.Row(Fmt("%08X\t%03X", u, dxbc::UnclampedFloat32To7e3(AsF32(u))));
    for (uint32_t i = 0; i < kSampleRows; ++i) {
      const uint32_t u = SampleBits(i);
      g.Row(Fmt("%08X\t%03X", u, dxbc::UnclampedFloat32To7e3(AsF32(u))));
    }
    g.Finish(c.res, c.data, c.mode);
    Mismatches m = SweepBits(c, 0, 0xFFFFFFFFu, [](uint32_t u, Mismatches& l) {
      l.Check();
      const float f = AsF32(u);
      const uint32_t o = ed::floatTo7e3(f), r = dxbc::UnclampedFloat32To7e3(f);
      if (o != r) l.Add(Fmt("f32=%08X ours=%03X xenia=%03X", u, o, r));
    });
    Compare(c, "ours.edram.floatTo7e3", m, Status::Fail,
            "gfx/edram.hpp:498 vs dxbc_shader_translator_om.cc:3462 UnclampedFloat32To7e3" + GridName(c));
  }
}

// ===========================================================================
// f20e4 - k_24_8_FLOAT depth codes.
void TestF20e4(Ctx& c) {
  using namespace xref;
  {
    xc::Golden g("f20e4_decode", "20e4 (k_24_8_FLOAT depth: 20-bit mantissa, 4-bit exponent, [0, 2)) -> float32");
    g.Comment("sources: xenos.cc:150-167 Float20e4To32 | pixel_formats.xesli:615-632 XeFloat20e4To32 (remap 0 / 1)");
    g.Comment("columns: f24  xenos_cc  xesli_remap0  xesli_remap1   (float32 bits); digests over all 2^24 codes");
    xc::Fnv h0, h1, h2;
    Mismatches internal, ours;
    for (uint32_t f24 = 0; f24 < (1u << 24); ++f24) {
      const uint32_t a = AsU32(xenos_cc::Float20e4To32(f24));
      const uint32_t b = xesli::Float20e4To32(f24, false);
      const uint32_t b1 = xesli::Float20e4To32(f24, true);
      const uint32_t d0 = AsU32(dxbc::Depth20e4To32(f24, false));
      const uint32_t d1 = AsU32(dxbc::Depth20e4To32(f24, true));
      h0.U32(a);
      h1.U32(b);
      h2.U32(b1);
      internal.Check();
      if (a != b || a != d0 || b1 != d1) {
        internal.Add(Fmt("f24=%06X xenos.cc=%08X xesli=%08X dxbc=%08X | remap xesli=%08X dxbc=%08X",
                         f24, a, b, d0, b1, d1));
      }
      ours.Check();
      const uint32_t o = AsU32(ed::f20e4ToFloat(f24));
      if (o != a) ours.Add(Fmt("f24=%06X ours=%08X xenia=%08X", f24, o, a));
    }
    g.Digest("all_2^24_xenos_cc", 1u << 24, h0.h);
    g.Digest("all_2^24_xesli_remap0", 1u << 24, h1.h);
    g.Digest("all_2^24_xesli_remap1", 1u << 24, h2.h);
    const uint32_t edges[] = {0, 1, 2, 0x7FFFF, 0x80000, 0xFFFFF, 0x100000, 0x100001,
                              0x7FFFFF, 0x800000, 0xEFFFFF, 0xF00000, 0xFFFFFF};
    for (uint32_t f24 : edges) {
      g.Row(Fmt("%06X\t%08X\t%08X\t%08X", f24, AsU32(xenos_cc::Float20e4To32(f24)),
                xesli::Float20e4To32(f24, false), xesli::Float20e4To32(f24, true)));
    }
    for (uint32_t i = 0; i < kSampleRows; ++i) {
      const uint32_t f24 = SampleBits(i) & 0xFFFFFF;
      g.Row(Fmt("%06X\t%08X\t%08X\t%08X", f24, AsU32(xenos_cc::Float20e4To32(f24)),
                xesli::Float20e4To32(f24, false), xesli::Float20e4To32(f24, true)));
    }
    g.Finish(c.res, c.data, c.mode);
    RefAgree(c, "ref.f20e4.decode.paths_agree", internal,
             "xenos.cc Float20e4To32 == xesli XeFloat20e4To32 == DXBC Depth20e4To32 (both remaps), all 2^24 codes");
    Compare(c, "ours.edram.f20e4ToFloat", ours, Status::Fail,
            "gfx/edram.hpp:536 vs xenos.cc:150 Float20e4To32, all 2^24 codes");
  }
  {
    xc::Golden g("f20e4_encode", "float32 -> 20e4, round to nearest even and truncating");
    g.Comment("sources: xenos.cc:125-149 Float32To20e4 (CPU) | pixel_formats.xesli:602-613 XeFloat32To20e4 (shader)");
    g.Comment("columns: f32_bits  cpu_rne  cpu_trunc  xesli_rne  xesli_trunc");
    xc::Fnv h[4];
    uint64_t n = 0;
    for (uint64_t u = 0; u <= 0xFFFFFFFFull; u += kDenseStep, ++n) {
      const float f = AsF32(uint32_t(u));
      h[0].U32(xenos_cc::Float32To20e4(f, true));
      h[1].U32(xenos_cc::Float32To20e4(f, false));
      h[2].U32(xesli::Float32To20e4(uint32_t(u), true));
      h[3].U32(xesli::Float32To20e4(uint32_t(u), false));
    }
    g.Digest("dense_step257_cpu_rne", n, h[0].h);
    g.Digest("dense_step257_cpu_trunc", n, h[1].h);
    g.Digest("dense_step257_xesli_rne", n, h[2].h);
    g.Digest("dense_step257_xesli_trunc", n, h[3].h);
    auto row = [&](uint32_t u) {
      const float f = AsF32(u);
      g.Row(Fmt("%08X\t%06X\t%06X\t%06X\t%06X", u, xenos_cc::Float32To20e4(f, true),
                xenos_cc::Float32To20e4(f, false), xesli::Float32To20e4(u, true),
                xesli::Float32To20e4(u, false)));
    };
    for (uint32_t u : Specials()) row(u);
    for (uint32_t i = 0; i < kSampleRows; ++i) row(SampleBits(i));
    g.Finish(c.res, c.data, c.mode);

    Mismatches internal = SweepBits(c, 0, 0xFFFFFFFFu, [](uint32_t u, Mismatches& l) {
      l.Check();
      const float f = AsF32(u);
      for (int rne = 0; rne < 2; ++rne) {
        const uint32_t a = xenos_cc::Float32To20e4(f, rne != 0), b = xesli::Float32To20e4(u, rne != 0);
        if (a != b) {
          l.Add(Fmt("f32=%08X %s cpu=%06X xesli=%06X", u, rne ? "rne" : "trunc", a, b));
          break;
        }
      }
    });
    RefAgree(c, "ref.f20e4.encode.cpu_vs_shader", internal,
             "xenos.cc Float32To20e4 vs xesli XeFloat32To20e4" + GridName(c),
             "Xenia's CPU encoder returns 0 for +NaN (`!(f32 > 0)`), the shader one saturates +NaN to 0xFFFFFF");
    Mismatches m = SweepBits(c, 0, 0xFFFFFFFFu, [](uint32_t u, Mismatches& l) {
      l.Check();
      const uint32_t o = ed::floatTo20e4(AsF32(u)), r = xesli::Float32To20e4(u, true);
      if (o != r) l.Add(Fmt("f32=%08X ours=%06X xenia=%06X", u, o, r));
    });
    Compare(c, "ours.edram.floatTo20e4", m, Status::Fail,
            "gfx/edram.hpp:527 vs pixel_formats.xesli:602 XeFloat32To20e4(rne)" + GridName(c));
  }
}

// ===========================================================================
// depth - EDRAM depth pack / unpack (host D32F <-> guest D24S8 / D24FS8).
void TestDepth(Ctx& c) {
  using namespace xref;
  // Unpack: all 2^24 codes.
  {
    Mismatches fs8, s8;
    for (uint32_t d24 = 0; d24 < (1u << 24); ++d24) {
      fs8.Check();
      const uint32_t o = AsU32(ed::unpackDepth(1, (d24 << 8) | 0x5A));
      const uint32_t r = AsU32(dxbc::Depth20e4To32(d24, true));
      if (o != r) fs8.Add(Fmt("d24=%06X ours=%08X xenia=%08X", d24, o, r));
      s8.Check();
      const uint32_t o2 = AsU32(ed::unpackDepth(0, (d24 << 8) | 0xA5));
      const uint32_t r2 = AsU32(xenos_h::UNorm24To32(d24));
      if (o2 != r2) s8.Add(Fmt("d24=%06X ours=%08X xenia=%08X", d24, o2, r2));
    }
    Compare(c, "ours.edram.unpackDepth.D24FS8", fs8, Status::Fail,
            "gfx/edram.hpp:648 vs dxbc_shader_translator_om.cc:3625 Depth20e4To32(remap to 0..0.5), all 2^24 codes");
    Compare(c, "ours.edram.unpackDepth.D24S8", s8, Status::Fail,
            "gfx/edram.hpp:649 vs xenos.h:430 UNorm24To32, all 2^24 codes");
  }
  // Pack D24FS8: host depth in [0, 0.5] (our viewport halves Z for float24,
  // gfx/edram.hpp:795, as Xenia's draw_util.cc:517-537 does).
  {
    auto packFs8 = [](uint32_t u, Mismatches& l) {
      l.Check();
      const uint32_t o = ed::packDepth(1, AsF32(u), 0) >> 8;
      const uint32_t r = dump::PackD24FS8(AsF32(u));
      if (o != r) l.Add(Fmt("host=%08X(%.9g) ours=%06X xenia=%06X", u, double(AsF32(u)), o, r));
    };
    // Normal 20e4 range first (host >= 2^-15) so the examples are realistic
    // depths, then everything down to 0 for the total.
    Mismatches normal = SweepBits(c, 0x38000000u, 0x3F000000u, packFs8);
    Mismatches all = SweepBits(c, 0, 0x3F000000u, packFs8);
    Compare(c, "ours.edram.packDepth.D24FS8", normal, Status::Fail,
            "gfx/edram.hpp:637 (and the HLSL CSStore, gfx/d3d12_pm4_edram.cpp:2500) vs Xenia's EDRAM dump "
            "d3d12_render_target_cache.cc:6250-6256, host depth in [2^-15, 0.5]" + GridName(c),
            Fmt("Xenia's default (render_target_cache.cc:40-41 depth_float24_round=false) TRUNCATES the host "
                "float to 20e4; ours rounds to nearest even (1 ULP of 20e4 apart). Whole [0, 0.5]: %" PRIu64
                "/%" PRIu64 " differ.",
                all.count, all.checked));
    Mismatches vsRound = SweepBits(c, 0, 0x3F000000u, [](uint32_t u, Mismatches& l) {
      l.Check();
      const uint32_t o = ed::packDepth(1, AsF32(u), 0) >> 8;
      const uint32_t r = dump::PackD24FS8(AsF32(u), true);
      if (o != r) l.Add(Fmt("host=%08X ours=%06X xenia_rne=%06X", u, o, r));
    });
    RefAgree(c, "info.packDepth.D24FS8.vs_round_cvar", vsRound,
             "ours vs the same Xenia dump with depth_float24_round=true (non-default)");
  }
  // Pack D24S8: host depth in [0, 1].
  {
    Mismatches m = SweepBits(c, 0, 0x3F800000u, [](uint32_t u, Mismatches& l) {
      l.Check();
      const uint32_t o = ed::packDepth(0, AsF32(u), 0) >> 8;
      const uint32_t r = dump::PackD24S8(AsF32(u));
      if (o != r) l.Add(Fmt("host=%08X(%.9g) ours=%06X xenia=%06X", u, double(AsF32(u)), o, r));
    });
    Compare(c, "ours.edram.packDepth.D24S8", m, Status::Waived,
            "gfx/edram.hpp:638-642 vs Xenia dump d3d12_render_target_cache.cc:6240-6249 "
            "(round_ne(d * 16777215.0f)), host depth in [0, 1]" + GridName(c),
            "intentional: ours is the exact inverse of UNorm24To32 so load->store->load is bit-exact "
            "(edram.hpp:639-640); Xenia keeps exactness via its separate host-depth store instead");
  }
  // Round trips (all 2^24 codes).
  {
    Mismatches oursS8, oursFS8, xeS8, xeFS8;
    for (uint32_t d24 = 0; d24 < (1u << 24); ++d24) {
      oursS8.Check();
      const uint32_t a = ed::packDepth(0, ed::unpackDepth(0, d24 << 8), 0) >> 8;
      if (a != d24) oursS8.Add(Fmt("d24=%06X -> %06X", d24, a));
      oursFS8.Check();
      const uint32_t b = ed::packDepth(1, ed::unpackDepth(1, d24 << 8), 0) >> 8;
      if (b != d24) oursFS8.Add(Fmt("d24=%06X -> %06X", d24, b));
      xeS8.Check();
      const uint32_t x = dump::PackD24S8(xenos_h::UNorm24To32(d24));
      if (x != d24) xeS8.Add(Fmt("d24=%06X -> %06X", d24, x));
      xeFS8.Check();
      const uint32_t y = dump::PackD24FS8(dxbc::Depth20e4To32(d24, true));
      if (y != d24) xeFS8.Add(Fmt("d24=%06X -> %06X", d24, y));
    }
    Compare(c, "ours.edram.depth_roundtrip.D24S8", oursS8, Status::Fail,
            "packDepth(unpackDepth(d24)) == d24, all 2^24 codes");
    Compare(c, "ours.edram.depth_roundtrip.D24FS8", oursFS8, Status::Fail,
            "packDepth(unpackDepth(d24)) == d24, all 2^24 codes");
    RefAgree(c, "info.xenia_depth_roundtrip.D24S8", xeS8,
             "Xenia dump(UNorm24To32(d24)) == d24 (Xenia relies on its host-depth store for exactness)");
    RefAgree(c, "info.xenia_depth_roundtrip.D24FS8", xeFS8, "Xenia dump(Depth20e4To32(d24)) == d24");
  }
  // Golden.
  {
    xc::Golden g("depth24", "host depth (float32) -> guest 24-bit EDRAM depth, Xenia's dump pipeline");
    g.Comment("sources: d3d12_render_target_cache.cc:6239-6256 + dxbc_shader_translator_om.cc:3555-3623 / 3726-3738");
    g.Comment("columns: host_f32  d24fs8_trunc(default cvars)  d24fs8_rne  d24s8");
    xc::Fnv h[3];
    uint64_t n = 0;
    for (uint64_t u = 0; u <= 0x3F800000ull; u += kDenseStep, ++n) {
      const float f = AsF32(uint32_t(u));
      h[0].U32(dump::PackD24FS8(f));
      h[1].U32(dump::PackD24FS8(f, true));
      h[2].U32(dump::PackD24S8(f));
    }
    g.Digest("dense_0_to_1_d24fs8_trunc", n, h[0].h);
    g.Digest("dense_0_to_1_d24fs8_rne", n, h[1].h);
    g.Digest("dense_0_to_1_d24s8", n, h[2].h);
    auto row = [&](uint32_t u) {
      const float f = AsF32(u);
      g.Row(Fmt("%08X\t%06X\t%06X\t%06X", u, dump::PackD24FS8(f), dump::PackD24FS8(f, true),
                dump::PackD24S8(f)));
    };
    for (uint32_t u : Specials()) {
      if (u <= 0x3F800000u) row(u);
    }
    for (uint32_t i = 0; i < kSampleRows; ++i) row(SampleBits(i) % 0x3F800001u);
    g.Finish(c.res, c.data, c.mode);
  }
}

// ===========================================================================
// edram_color - EDRAM colour pack / unpack (edram::packColor / unpackColor).
float RandomColorChannel(xc::Rng& r) {
  switch (r.Below(10)) {
    case 0: return AsF32(Specials()[r.Below(uint32_t(Specials().size()))]);
    case 1: return float(r.Below(256)) / 255.0f;
    case 2: return float(r.Below(1024)) / 1023.0f;
    case 3: return r.Uniform(-40.0f, 40.0f);
    case 4: return AsF32(r.U32());
    case 5: return float(int32_t(r.Below(65536)) - 32768) * (32.0f / 32767.0f);
    default: return r.Uniform(-0.25f, 1.25f);
  }
}
void TestEdramColor(Ctx& c) {
  using namespace xref;
  // Unpack, dense over the 32-bit packed word.
  {
    struct U {
      const char* id;
      uint32_t fmt;
      const char* what;
    };
    const U cases[] = {
        {"ours.edram.unpackColor.k_8_8_8_8", 0, "gfx/edram.hpp:595 vs pixel_formats.xesli:304 XeUnpackR8G8B8A8UNorm"},
        {"ours.edram.unpackColor.k_2_10_10_10", 2, "gfx/edram.hpp:598 vs pixel_formats.xesli:313 XeUnpackR10G10B10A2UNorm"},
        {"ours.edram.unpackColor.k_2_10_10_10_FLOAT", 3, "gfx/edram.hpp:602 vs pixel_formats.xesli:339 XeUnpackR10G10B10A2Float"},
        {"ours.edram.unpackColor.k_16_16", 4, "gfx/edram.hpp:606 vs pixel_formats.xesli:380 XeUnpackR16G16Edram"},
        {"ours.edram.unpackColor.k_16_16_16_16", 5, "gfx/edram.hpp:610 vs pixel_formats.xesli:380 XeUnpackR16G16Edram (both halves)"},
    };
    for (const auto& k : cases) {
      Mismatches m = SweepBits(c, 0, 0xFFFFFFFFu, [&](uint32_t p, Mismatches& l) {
        l.Check();
        float o[4], r[4] = {0, 0, 0, 1};
        const uint32_t hi = p ^ 0x5A5A5A5Au;
        ed::unpackColor(k.fmt, uint64_t(p) | (uint64_t(hi) << 32), o);
        int nc = 4;
        switch (k.fmt) {
          case 0: xesli::UnpackR8G8B8A8UNorm(p, r); break;
          case 2: xesli::UnpackR10G10B10A2UNorm(p, r); break;
          case 3: xesli::UnpackR10G10B10A2Float(p, r); break;
          case 4: xesli::UnpackR16G16Edram(p, r); nc = 2; break;
          case 5: xesli::UnpackR16G16Edram(p, r); xesli::UnpackR16G16Edram(hi, r + 2); break;
        }
        for (int i = 0; i < nc; ++i) {
          if (AsU32(o[i]) != AsU32(r[i])) {
            l.Add(Fmt("word=%08X ch%d ours=%08X xenia=%08X", p, i, AsU32(o[i]), AsU32(r[i])));
            break;
          }
        }
      });
      Compare(c, k.id, m, Status::Fail, std::string(k.what) + GridName(c));
    }
  }
  // Pack, random float4 + edge values.
  {
    const uint32_t kN = c.exhaustive ? 16000000u : 2000000u;
    Mismatches m8888, m8888dump, m2101010, m7e3, m16, m16xesli;
    xc::Rng rng(0xC0FFEE);
    for (uint32_t i = 0; i < kN; ++i) {
      float v[4];
      for (float& f : v) f = RandomColorChannel(rng);
      // k_8_8_8_8 vs the shader packer (any float) and the dump (UNORM host source).
      m8888.Check();
      const uint32_t o8 = uint32_t(ed::packColor(0, v)), r8 = xesli::PackR8G8B8A8UNorm(v);
      if (o8 != r8) m8888.Add(Fmt("in=(%08X %08X %08X %08X) ours=%08X xenia=%08X", AsU32(v[0]), AsU32(v[1]), AsU32(v[2]), AsU32(v[3]), o8, r8));
      float u[4];
      for (int k = 0; k < 4; ++k) u[k] = xref::saturate(v[k]);
      m8888dump.Check();
      const uint32_t od = uint32_t(ed::packColor(0, u)), rd = dump::PackColor8888(u);
      if (od != rd) m8888dump.Add(Fmt("in=(%08X %08X %08X %08X) ours=%08X xenia=%08X", AsU32(u[0]), AsU32(u[1]), AsU32(u[2]), AsU32(u[3]), od, rd));
      // k_2_10_10_10.
      m2101010.Check();
      const uint32_t o10 = uint32_t(ed::packColor(2, v)), r10 = xesli::PackR10G10B10A2UNorm(v);
      if (o10 != r10) m2101010.Add(Fmt("in=(%08X %08X %08X %08X) ours=%08X xenia=%08X", AsU32(v[0]), AsU32(v[1]), AsU32(v[2]), AsU32(v[3]), o10, r10));
      // k_2_10_10_10_FLOAT vs the dump (7e3 RGB, saturated 2-bit alpha).
      m7e3.Check();
      const uint32_t of = uint32_t(ed::packColor(3, v)), rf = dump::PackColor2101010Float(v);
      if (of != rf) m7e3.Add(Fmt("in=(%08X %08X %08X %08X) ours=%08X xenia=%08X", AsU32(v[0]), AsU32(v[1]), AsU32(v[2]), AsU32(v[3]), of, rf));
      // k_16_16_16_16 (covers k_16_16's two lanes) vs the ROV fixed-point pack.
      m16.Check();
      const uint64_t o16 = ed::packColor(5, v);
      const uint64_t r16 = uint64_t(rov::PackFixed16(v[0]) | (rov::PackFixed16(v[1]) << 16)) |
                           (uint64_t(rov::PackFixed16(v[2]) | (rov::PackFixed16(v[3]) << 16)) << 32);
      if (o16 != r16) {
        for (int k = 0; k < 4; ++k) {
          const uint32_t ok = uint32_t(o16 >> (16 * k)) & 0xFFFF, rk = uint32_t(r16 >> (16 * k)) & 0xFFFF;
          if (ok != rk) {
            m16.Add(Fmt("ch=%08X(%.9g) ours=%04X xenia=%04X", AsU32(v[k]), double(v[k]), ok, rk));
            break;
          }
        }
      }
      m16xesli.Check();
      const uint32_t x16 = xesli::PackR16G16Edram(v[0], v[1]);
      if (x16 != uint32_t(ed::packColor(4, v))) m16xesli.Add("");
    }
    Compare(c, "ours.edram.packColor.k_8_8_8_8", m8888, Status::Fail,
            "gfx/edram.hpp:563 vs pixel_formats.xesli:113 XePackR8G8B8A8UNorm, random float4 + edges");
    Compare(c, "ours.edram.packColor.k_8_8_8_8.dump", m8888dump, Status::Fail,
            "gfx/edram.hpp:563 vs Xenia dump d3d12_render_target_cache.cc:6268 (UNORM host values)");
    Compare(c, "ours.edram.packColor.k_2_10_10_10", m2101010, Status::Fail,
            "gfx/edram.hpp:566 vs pixel_formats.xesli:118 XePackR10G10B10A2UNorm");
    Compare(c, "ours.edram.packColor.k_2_10_10_10_FLOAT", m7e3, Status::Fail,
            "gfx/edram.hpp:569 vs Xenia dump d3d12_render_target_cache.cc:6307-6333");
    Compare(c, "ours.edram.packColor.k_16_16(_16_16)", m16, Status::Fail,
            "gfx/edram.hpp:551 packFixed16Edram vs Xenia ROV pack dxbc_shader_translator_om.cc:1428-1455 "
            "(clamp -32..32, render_target_cache.cc:248-256)",
            "ours TRUNCATES toward zero (`int32_t(f * 32767/32)`); Xenia's ROV path rounds half away from "
            "zero (mad with +-0.5, then ftoi) and its RTV path uses D3D float->SNORM (round to nearest). "
            "NaN: ROV clamps max(NaN, -32) = -32 -> 0x8001, ours -> 0. The truncating xesli "
            "XePackR16G16Edram that edram.hpp:551 cites has no caller in Xenia.");
    if (m16xesli.count == 0) {
      c.res.Report(Status::Info, "info.packColor.k_16_16.vs_unused_xesli",
                   Fmt("ours == the (unused) truncating xesli XePackR16G16Edram on %" PRIu64 " inputs",
                       m16xesli.checked));
    } else {
      c.res.Report(Status::Info, "info.packColor.k_16_16.vs_unused_xesli",
                   Fmt("ours != the (unused) truncating xesli XePackR16G16Edram on %" PRIu64 "/%" PRIu64
                       " inputs (NaN: max(NaN,-32) = -32 there, 0 in ours)",
                       m16xesli.count, m16xesli.checked));
    }
  }
  // k_8_8_8_8_GAMMA: our PM4 EDRAM model stores it as plain k_8_8_8_8.
  {
    const uint32_t res = ed::colorResourceFormat(1);
    uint32_t diffStore = 0, maxStore = 0, diffLoad = 0;
    float maxLoad = 0.0f;
    for (uint32_t k = 0; k <= 4095; ++k) {
      const float l = float(k) / 4095.0f;
      const float v[4] = {l, l, l, 1.0f};
      const uint32_t ours = uint32_t(ed::packColor(res, v)) & 0xFF;
      const uint32_t xe = xref::dump::PackColor8888Gamma(v) & 0xFF;
      if (ours != xe) {
        ++diffStore;
        maxStore = std::max(maxStore, ours > xe ? ours - xe : xe - ours);
      }
    }
    for (uint32_t b = 0; b < 256; ++b) {
      float o[4];
      ed::unpackColor(res, b | (b << 8) | (b << 16), o);
      const float xe = xref::dxbc::PWLGammaToLinear(float(b) / 255.0f, true);
      if (AsU32(o[0]) != AsU32(xe)) {
        ++diffLoad;
        maxLoad = std::max(maxLoad, std::fabs(o[0] - xe));
      }
    }
    c.res.Report(diffStore || diffLoad ? Status::Gap : Status::Pass, "ours.edram.k_8_8_8_8_GAMMA",
                 Fmt("colorResourceFormat(k_8_8_8_8_GAMMA)=%u: our PM4 path keeps gamma RTs as plain 8888 "
                     "(gfx/edram.hpp:85-90, gfx/d3d12_pm4_edram.cpp:19) while Xenia PWL-encodes RGB on the way "
                     "to EDRAM (d3d12_render_target_cache.cc:6280-6294) and decodes with PWLGammaToLinear: "
                     "store %u/4096 linear levels land on a different byte (max |delta| %u), load %u/256 "
                     "bytes decode differently (max |delta| %.4f linear)",
                     res, diffStore, maxStore, diffLoad, double(maxLoad)));
  }
  // Golden: reference packers on seeded inputs.
  {
    xc::Golden g("edram_color", "EDRAM colour packing references on seeded random float4 inputs");
    g.Comment("sources: pixel_formats.xesli:113-121 | d3d12_render_target_cache.cc:6307-6333 (7e3 dump) | "
              "dxbc_shader_translator_om.cc:1428-1455 (ROV 16_16) | d3d12_render_target_cache.cc:6280-6294 (gamma)");
    g.Comment("columns: r g b a (f32 bits)  pack8888  pack2101010  dump2101010float  rov16_16(rg)  dump8888gamma");
    xc::Rng rng(0xBADC0DE);
    for (uint32_t i = 0; i < kSampleRows; ++i) {
      float v[4];
      for (float& f : v) f = RandomColorChannel(rng);
      float s[4];
      for (int k = 0; k < 4; ++k) s[k] = xref::saturate(v[k]);
      g.Row(Fmt("%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X", AsU32(v[0]), AsU32(v[1]),
                AsU32(v[2]), AsU32(v[3]), xesli::PackR8G8B8A8UNorm(v), xesli::PackR10G10B10A2UNorm(v),
                dump::PackColor2101010Float(v), rov::PackFixed16(v[0]) | (rov::PackFixed16(v[1]) << 16),
                dump::PackColor8888Gamma(s)));
    }
    g.Finish(c.res, c.data, c.mode);
  }
}

// ===========================================================================
// pwl_gamma - the 8_8_8_8_GAMMA piecewise-linear gamma (reference + census).
double SrgbToLinear(double s) {
  return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}
double LinearToSrgb(double l) {
  return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}
void TestPwlGamma(Ctx& c) {
  using namespace xref;
  {
    xc::Golden g("pwl_gamma", "Xenos PWL gamma (8_8_8_8_GAMMA): gamma->linear for the 256 bytes, linear->gamma for 1024 levels");
    g.Comment("sources: xenos.cc:23-64 PWLGammaToLinear, xenos.cc:66-100 LinearToPWLGamma");
    g.Comment("rows 'G': byte  PWLGammaToLinear(byte/255) bits;  rows 'L': level  LinearToPWLGamma(level/1023) bits  byte");
    xc::Fnv h0, h1;
    uint64_t n = 0;
    for (uint64_t u = 0; u <= 0x3F800000ull; u += kDenseStep, ++n) {
      h0.U32(AsU32(xenos_cc::PWLGammaToLinear(AsF32(uint32_t(u)))));
      h1.U32(AsU32(xenos_cc::LinearToPWLGamma(AsF32(uint32_t(u)))));
    }
    g.Digest("dense_0_to_1_gamma_to_linear", n, h0.h);
    g.Digest("dense_0_to_1_linear_to_gamma", n, h1.h);
    for (uint32_t b = 0; b < 256; ++b) {
      g.Row(Fmt("G\t%02X\t%08X", b, AsU32(xenos_cc::PWLGammaToLinear(float(b) / 255.0f))));
    }
    for (uint32_t l = 0; l < 1024; ++l) {
      const float gv = xenos_cc::LinearToPWLGamma(float(l) / 1023.0f);
      g.Row(Fmt("L\t%03X\t%08X\t%02X", l, AsU32(gv), d3d::ftou(gv * 255.0f + 0.5f)));
    }
    g.Finish(c.res, c.data, c.mode);
  }
  // Xenia CPU vs DXBC, fused vs unfused mad, round trip.
  {
    Mismatches toLin = SweepBits(c, 0, 0xFFFFFFFFu, [](uint32_t u, Mismatches& l) {
      l.Check();
      const float f = AsF32(u);
      const uint32_t a = AsU32(xenos_cc::PWLGammaToLinear(f)), b = AsU32(dxbc::PWLGammaToLinear(f, false));
      if (a != b) l.Add(Fmt("in=%08X cpu=%08X dxbc=%08X", u, a, b));
    });
    RefAgree(c, "ref.pwl_gamma.to_linear.cpu_vs_dxbc", toLin,
             "xenos.cc PWLGammaToLinear == dxbc_shader_translator.cc:225 PWLGammaToLinear" + GridName(c));
    Mismatches toGam = SweepBits(c, 0, 0x3F800000u, [](uint32_t u, Mismatches& l) {
      l.Check();
      const float f = AsF32(u);
      const uint32_t a = AsU32(xenos_cc::LinearToPWLGamma(f)), b = AsU32(dxbc::PreSaturatedLinearToPWLGamma(f));
      if (a != b) l.Add(Fmt("in=%08X cpu=%08X dxbc=%08X", u, a, b));
    });
    RefAgree(c, "ref.pwl_gamma.to_gamma.cpu_vs_dxbc", toGam,
             "xenos.cc LinearToPWLGamma == dxbc_shader_translator.cc:288 PreSaturatedLinearToPWLGamma, [0,1]" + GridName(c));
    Mismatches fused = SweepBits(c, 0, 0x3F800000u, [](uint32_t u, Mismatches& l) {
      l.Check();
      const float f = AsF32(u);
      const uint32_t a = d3d::ftou(dxbc::PreSaturatedLinearToPWLGamma(f, false) * 255.0f + 0.5f);
      const uint32_t b = d3d::ftou(dxbc::PreSaturatedLinearToPWLGamma(f, true) * 255.0f + 0.5f);
      if (a != b) l.Add(Fmt("in=%08X unfused=%02X fused=%02X", u, a, b));
    });
    RefAgree(c, "ref.pwl_gamma.mad_fusion", fused,
             "8-bit gamma byte identical whether the GPU fuses DXBC mad or not, [0,1]" + GridName(c));
    Mismatches rt;
    for (uint32_t b = 0; b < 256; ++b) {
      rt.Check();
      const float lin = xenos_cc::PWLGammaToLinear(float(b) / 255.0f);
      const uint32_t back = d3d::ftou(xenos_cc::LinearToPWLGamma(lin) * 255.0f + 0.5f);
      if (back != b) rt.Add(Fmt("byte=%02X -> %02X", b, back));
    }
    RefAgree(c, "ref.pwl_gamma.byte_roundtrip", rt, "byte -> linear -> byte through Xenia's PWL pair");
  }
  // Census: DXGI _SRGB (used by the HLE path for k_8_8_8_8_GAMMA,
  // gfx/d3d12_backend.cpp:613) vs the Xenos PWL curve.
  {
    uint32_t dec = 0;
    double decMax = 0.0;
    for (uint32_t b = 0; b < 256; ++b) {
      const double pwl = xref::xenos_cc::PWLGammaToLinear(float(b) / 255.0f);
      const double srgb = SrgbToLinear(b / 255.0);
      const double d = std::fabs(pwl - srgb) * 255.0;
      decMax = std::max(decMax, d);
      if (std::lround(pwl * 255.0) != std::lround(srgb * 255.0)) ++dec;
    }
    uint32_t enc = 0, encMax = 0;
    for (uint32_t k = 0; k <= 4095; ++k) {
      const float l = float(k) / 4095.0f;
      const uint32_t pwl = d3d::ftou(xref::xenos_cc::LinearToPWLGamma(l) * 255.0f + 0.5f);
      const uint32_t srgb = uint32_t(std::lround(LinearToSrgb(l) * 255.0));
      if (pwl != srgb) {
        ++enc;
        encMax = std::max(encMax, pwl > srgb ? pwl - srgb : srgb - pwl);
      }
    }
    c.res.Report(Status::Info, "census.pwl_vs_srgb",
                 Fmt("DXGI *_SRGB is not the Xenos PWL curve: decode - %u/256 bytes give a different 8-bit "
                     "linear value (max |delta| %.2f/255); encode - %u/4096 linear levels give a different "
                     "byte (max |delta| %u)",
                     dec, decMax, enc, encMax));
  }
}

// ===========================================================================
// resolve
ed::Regs OurRegs(const rs::ResolveRectIn& in) {
  ed::Regs r;
  r.surfaceInfo = in.scissor.rb_surface_info;
  r.copyControl = in.rb_copy_control;
  r.windowOffset = in.scissor.pa_sc_window_offset;
  r.windowScissorTL = in.scissor.pa_sc_window_scissor_tl;
  r.windowScissorBR = in.scissor.pa_sc_window_scissor_br;
  r.screenScissorTL = in.scissor.pa_sc_screen_scissor_tl;
  r.screenScissorBR = in.scissor.pa_sc_screen_scissor_br;
  r.suScModeCntl = in.pa_su_sc_mode_cntl;
  r.vtxCntl = in.pa_su_vtx_cntl;
  std::memcpy(r.resolveVerts, in.vertices, sizeof(r.resolveVerts));
  r.resolveVertsValid = true;
  r.copyDestInfo = 6u << 7;  // k_8_8_8_8
  r.copyDestPitch = 1280u | (720u << 16);
  return r;
}
float RandomCoord(xc::Rng& r, float base) {
  switch (r.Below(12)) {
    case 0: return AsF32(Specials()[r.Below(uint32_t(Specials().size()))]);
    case 1: return base + 0.5f;
    case 2: return base + 0.498046875f;  // .128/256 - the top-left rule edge
    case 3: return base + 0.50390625f;   // .129/256
    case 4: return base - 0.5f;
    case 5: return r.Uniform(-200.0f, 9000.0f);
    case 6: return base + r.Uniform(-1.0f, 1.0f);
    default: return base;
  }
}
rs::ResolveRectIn RandomRect(xc::Rng& r) {
  rs::ResolveRectIn in;
  static const uint32_t kPitches[] = {1280, 640, 1024, 720, 256, 64, 8, 0, 100, 1283, 16383, 4096};
  const uint32_t pitch = r.Below(6) == 0 ? r.Below(16384) : kPitches[r.Below(12)];
  const uint32_t msaa = r.Below(10) == 0 ? 3 : r.Below(3);
  in.scissor.rb_surface_info = pitch | (msaa << 16);
  const uint32_t cmd = r.Below(12) == 0 ? 2 + r.Below(2) : r.Below(2);
  in.rb_copy_control = r.Below(8) | (r.Below(8) << 4) | (r.Below(2) << 8) | (r.Below(2) << 9) | (cmd << 20);
  in.pa_su_vtx_cntl = r.Below(2);
  in.pa_su_sc_mode_cntl = r.Below(2) << 16;
  auto off = [&]() { return r.Below(4) == 0 ? r.Below(0x8000) : uint32_t(int32_t(r.Below(81)) - 40) & 0x7FFF; };
  in.scissor.pa_sc_window_offset = off() | (off() << 16);
  const uint32_t tlx = r.Below(3) ? 0 : r.Below(400), tly = r.Below(3) ? 0 : r.Below(400);
  const uint32_t brx = r.Below(3) ? 8192 : r.Below(0x4000), bry = r.Below(3) ? 8192 : r.Below(0x4000);
  in.scissor.pa_sc_window_scissor_tl = tlx | (tly << 16) | (r.Below(2) << 31);
  in.scissor.pa_sc_window_scissor_br = brx | (bry << 16);
  if (r.Below(5) == 0) {
    in.scissor.pa_sc_screen_scissor_tl = r.Below(0x8000) | (r.Below(0x8000) << 16);
    in.scissor.pa_sc_screen_scissor_br = r.Below(0x8000) | (r.Below(0x8000) << 16);
  } else {
    in.scissor.pa_sc_screen_scissor_tl = 0;
    in.scissor.pa_sc_screen_scissor_br = 8192 | (8192 << 16);
  }
  const float x0 = float(r.Below(1400)) - 40.0f, y0 = float(r.Below(800)) - 40.0f;
  const float x1 = x0 + float(r.Below(1400)), y1 = y0 + float(r.Below(800));
  if (r.Below(8) == 0) {
    for (float& v : in.vertices) v = RandomCoord(r, float(r.Below(1300)));
  } else {
    in.vertices[0] = RandomCoord(r, x0); in.vertices[1] = RandomCoord(r, y0);
    in.vertices[2] = RandomCoord(r, x1); in.vertices[3] = RandomCoord(r, y0);
    in.vertices[4] = RandomCoord(r, x0); in.vertices[5] = RandomCoord(r, y1);
  }
  return in;
}

void TestResolve(Ctx& c) {
  // copy_sample_select sanitizing: every select x MSAA x depth.
  {
    xc::Golden g("resolve_sample_select", "copy_sample_select sanitizing, every select x MSAA mode x color/depth");
    g.Comment("source: draw_util.cc:839-880 SanitizeCopySampleSelect");
    g.Comment("columns: select  msaa(0=1x 1=2x 2=4x 3=invalid)  is_depth  sanitized");
    Mismatches m;
    for (uint32_t msaa = 0; msaa < 4; ++msaa) {
      for (uint32_t depth = 0; depth < 2; ++depth) {
        for (uint32_t sel = 0; sel < 8; ++sel) {
          const uint32_t r = rs::SanitizeCopySampleSelect(sel, msaa, depth != 0);
          g.Row(Fmt("%u\t%u\t%u\t%u", sel, msaa, depth, r));
          m.Check();
          const uint32_t o = ed::sanitizeSampleSelect(sel, msaa, depth != 0);
          if (o != r) m.Add(Fmt("sel=%u msaa=%u depth=%u ours=%u xenia=%u", sel, msaa, depth, o, r));
        }
      }
    }
    g.Finish(c.res, c.data, c.mode);
    Compare(c, "ours.edram.sanitizeSampleSelect", m, Status::Fail,
            "gfx/edram.hpp:839 vs draw_util.cc:839, all 64 combinations");
  }
  // Clear-on-resolve / depth-copy predicates.
  {
    Mismatches m;
    for (uint32_t msaa = 0; msaa < 3; ++msaa) {
      for (uint32_t cc = 0; cc < (1u << 10); ++cc) {
        if (cc & 0x88) continue;  // pad bits
        ed::Regs r;
        r.surfaceInfo = 1280 | (msaa << 16);
        r.copyControl = cc | (1u << 20);
        const ed::ResolveInfo ri = ed::computeResolve(r);
        m.Check();
        if (ri.isDepth != rs::IsCopyingDepth(cc) || ri.clearDepth != rs::IsClearingDepth(cc) ||
            ri.clearColor != rs::IsClearingColor(cc)) {
          m.Add(Fmt("copy_control=%03X ours(depth,clrD,clrC)=%d%d%d xenia=%d%d%d", cc, ri.isDepth,
                    ri.clearDepth, ri.clearColor, rs::IsCopyingDepth(cc), rs::IsClearingDepth(cc),
                    rs::IsClearingColor(cc)));
        }
      }
    }
    Compare(c, "ours.edram.resolve_clear_flags", m, Status::Fail,
            "gfx/edram.hpp:868-869 vs draw_util.h:669-694 (0x100 color clear, 0x200 depth clear, src>=4 depth)");
  }
  // Exponent bias factor.
  {
    xc::Golden g("resolve_exp_bias", "RB_COPY_DEST_INFO exponent bias -> factor, and the MSAA averaging factor");
    g.Comment("sources: resolve.xesli:135-137 dest_exp_bias_factor | resolve.xesli:593-631 averaging (x0.5 per halving)");
    g.Comment("columns: bias_field  factor_bits  x(k01|k23)  x(k0123)");
    Mismatches m;
    for (uint32_t field = 0; field < 64; ++field) {
      const float f = rs::DestExpBiasFactor(field << 16);
      g.Row(Fmt("%02X\t%08X\t%08X\t%08X", field, AsU32(f), AsU32(rs::ResolveSampleFactor(f, rs::k01)),
                AsU32(rs::ResolveSampleFactor(f, rs::k0123))));
      for (uint32_t src : {0u, 4u}) {
        rs::ResolveRectIn in;
        in.rb_copy_control = src | (1u << 20);
        in.pa_su_vtx_cntl = 1;
        in.scissor.rb_surface_info = 1280;
        in.scissor.pa_sc_window_scissor_br = 8192 | (8192 << 16);
        in.scissor.pa_sc_screen_scissor_br = 8192 | (8192 << 16);
        const float verts[6] = {0, 0, 64, 0, 0, 64};
        std::memcpy(in.vertices, verts, sizeof(verts));
        ed::Regs r = OurRegs(in);
        r.copyDestInfo = (6u << 7) | (field << 16);
        const ed::ResolveInfo ri = ed::computeResolve(r);
        const bool depth = src >= 4;
        const float ref = rs::DestExpBiasFactor(uint32_t(rs::CopyDestExpBias(r.copyDestInfo, depth) & 0x3F) << 16);
        m.Check();
        if (!ri.copy || AsU32(ri.expBiasFactor) != AsU32(ref)) {
          m.Add(Fmt("bias_field=%02X %s ours=%08X(copy=%d) xenia=%08X", field, depth ? "depth" : "color",
                    AsU32(ri.expBiasFactor), ri.copy, AsU32(ref)));
        }
      }
    }
    g.Finish(c.res, c.data, c.mode);
    Compare(c, "ours.edram.resolve_exp_bias", m, Status::Fail,
            "gfx/edram.hpp:920-928 vs draw_util.cc:1234 + resolve.xesli:135, all 64 biases, color and depth");
  }
  // FloatToD3D11Fixed16p8.
  {
    Mismatches m = SweepBits(c, 0, 0xFFFFFFFFu, [](uint32_t u, Mismatches& l) {
      l.Check();
      const int32_t o = ed::floatToFixed16p8(AsF32(u)), r = rs::FloatToD3D11Fixed16p8(AsF32(u));
      if (o != r) l.Add(Fmt("f32=%08X ours=%d xenia=%d", u, o, r));
    });
    Compare(c, "ours.edram.floatToFixed16p8", m, Status::Fail,
            "gfx/edram.hpp:803 vs ui/graphics_util.cc:17 FloatToD3D11Fixed16p8" + GridName(c));
  }
  // Resolve rectangle fuzz.
  {
    const uint32_t kCases = c.exhaustive ? 4000000u : 400000u, kGoldenCases = 100000u, kGoldenRows = 256u;
    xc::Golden g("resolve_rect", "resolve rectangle (draw_util.cc GetResolveInfo) on seeded random register states");
    g.Comment("sources: draw_util.cc:926-1074 GetResolveInfo (rectangle part), draw_util.cc:640-716 GetScissorTmpl, "
              "ui/graphics_util.cc:17-60");
    g.Comment("generator: splitmix64 seed 0x5EED0001, RandomRect() in conformance_main.cpp");
    g.Comment("columns: copy_control surface_info vtx_cntl sc_mode_cntl window_offset wsc_tl wsc_br ssc_tl ssc_br "
              "v0..v5(f32 bits) | ok empty x0 y0 x1 y1");
    xc::Rng rng(0x5EED0001);
    xc::Fnv h;
    Mismatches m;
    uint32_t copies = 0;
    for (uint32_t i = 0; i < kCases; ++i) {
      const rs::ResolveRectIn in = RandomRect(rng);
      const rs::ResolveRectOut ref = rs::GetResolveRect(in);
      copies += (ref.ok && !ref.empty) ? 1u : 0u;
      if (i < kGoldenCases) {
        h.U32(ref.ok | (ref.empty << 1));
        h.U32(uint32_t(ref.x0)); h.U32(uint32_t(ref.y0)); h.U32(uint32_t(ref.x1)); h.U32(uint32_t(ref.y1));
      }
      if (i < kGoldenRows) {
        std::string row = Fmt("%08X\t%08X\t%X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X", in.rb_copy_control,
                              in.scissor.rb_surface_info, in.pa_su_vtx_cntl, in.pa_su_sc_mode_cntl,
                              in.scissor.pa_sc_window_offset, in.scissor.pa_sc_window_scissor_tl,
                              in.scissor.pa_sc_window_scissor_br, in.scissor.pa_sc_screen_scissor_tl,
                              in.scissor.pa_sc_screen_scissor_br);
        for (float v : in.vertices) row += Fmt("\t%08X", AsU32(v));
        row += Fmt("\t|\t%d\t%d\t%d\t%d\t%d\t%d", ref.ok, ref.empty, ref.x0, ref.y0, ref.x1, ref.y1);
        g.Row(row);
      }
      const ed::ResolveInfo ri = ed::computeResolve(OurRegs(in));
      const bool refCopy = ref.ok && !ref.empty;
      m.Check();
      if (ri.copy != refCopy ||
          (refCopy && (int32_t(ri.x0) != ref.x0 || int32_t(ri.y0) != ref.y0 || int32_t(ri.x1) != ref.x1 ||
                       int32_t(ri.y1) != ref.y1))) {
        m.Add(Fmt("case %u cc=%08X si=%08X wo=%08X wtl=%08X wbr=%08X v=(%g,%g,%g,%g,%g,%g): ours copy=%d "
                  "[%u,%u)-[%u,%u) xenia copy=%d [%d,%d)-[%d,%d)",
                  i, in.rb_copy_control, in.scissor.rb_surface_info, in.scissor.pa_sc_window_offset,
                  in.scissor.pa_sc_window_scissor_tl, in.scissor.pa_sc_window_scissor_br,
                  double(in.vertices[0]), double(in.vertices[1]), double(in.vertices[2]),
                  double(in.vertices[3]), double(in.vertices[4]), double(in.vertices[5]), ri.copy, ri.x0,
                  ri.y0, ri.x1, ri.y1, refCopy, ref.x0, ref.y0, ref.x1, ref.y1));
      }
    }
    g.Digest("first_100000_cases", kGoldenCases, h.h);
    g.Finish(c.res, c.data, c.mode);
    Compare(c, "ours.edram.computeResolve.rect", m, Status::Fail,
            Fmt("gfx/edram.hpp:884-911 vs draw_util.cc:926-1074 (fixed-point vertices, window offset, scissor, "
                "8-px alignment, pitch / height clamp), seeded fuzz, %u/%u cases copy a non-empty rect",
                copies, kCases));
  }
  // EDRAM sample addressing.
  {
    Mismatches m;
    for (uint32_t msaa = 0; msaa < 3; ++msaa)
      for (uint32_t depth = 0; depth < 2; ++depth)
        for (uint32_t ints = 0; ints < 2; ++ints)
          for (uint32_t base : {0u, 1u, 1000u, 2047u})
            for (uint32_t pitchTiles : {1u, 16u, 32u, 160u})
              for (uint32_t py = 0; py < 720; py += 3)
                for (uint32_t px = 0; px < 1280; px += 7)
                  for (uint32_t s = 0; s < 4; ++s) {
                    m.Check();
                    const uint32_t o = ed::sampleOffsetBytes(px, py, base, pitchTiles, msaa, depth != 0, ints, s);
                    const uint32_t r = rs::EdramOffsetBytes(px, py, base, true, pitchTiles, msaa, depth != 0, ints, s);
                    if (o != r) {
                      m.Add(Fmt("px=%u py=%u base=%u pitch=%u msaa=%u depth=%u ints=%u s=%u ours=%08X xenia=%08X",
                                px, py, base, pitchTiles, msaa, depth, ints, s, o, r));
                    }
                  }
    Compare(c, "ours.edram.sampleOffsetBytes", m, Status::Fail,
            "gfx/edram.hpp:435 vs edram.xesli:23-58 XeEdramOffsetBytes (wrap)");
  }
  // End to end: resolveToGuest on a synthetic EDRAM (k_32_FLOAT RT -> k_32_FLOAT
  // texture): sample selection, averaging, exponent bias, destination tiling
  // and endian, with the samples placed by Xenia's EDRAM addressing.
  {
    std::vector<uint8_t> edram(ed::kEdramBytes);
    std::vector<uint8_t> dest(32 * 32 * 4);
    Mismatches m;
    const uint32_t kBase = 5, kPitch = 64;
    for (uint32_t msaa = 0; msaa < 3; ++msaa) {
      std::fill(edram.begin(), edram.end(), uint8_t(0));
      const uint32_t pitchTiles = rs::GetSurfacePitchTiles(kPitch, msaa, false);
      const uint32_t nsamp = 1u << msaa;
      for (uint32_t py = 0; py < 8; ++py)
        for (uint32_t px = 0; px < 8; ++px)
          for (uint32_t s = 0; s < nsamp; ++s) {
            const float v = float((1u << s) * (1u + px + 8u * py));
            const uint32_t off = rs::EdramOffsetBytes(px, py, kBase, true, pitchTiles, msaa, false, 0, s);
            std::memcpy(&edram[off], &v, 4);
          }
      for (uint32_t sel = 0; sel < 8; ++sel)
        for (int32_t bias : {-32, -7, -1, 0, 1, 5, 31})
          for (uint32_t endian : {0u, 2u}) {
            rs::ResolveRectIn in;
            in.rb_copy_control = 0 | (sel << 4) | (1u << 20);
            in.pa_su_vtx_cntl = 1;
            in.scissor.rb_surface_info = kPitch | (msaa << 16);
            in.scissor.pa_sc_window_scissor_br = 8192 | (8192 << 16);
            in.scissor.pa_sc_screen_scissor_br = 8192 | (8192 << 16);
            const float verts[6] = {0, 0, 8, 0, 0, 8};
            std::memcpy(in.vertices, verts, sizeof(verts));
            ed::Regs r = OurRegs(in);
            r.colorInfo[0] = kBase | (14u << 16);  // k_32_FLOAT
            r.copyDestInfo = endian | (36u << 7) | ((uint32_t(bias) & 0x3F) << 16);
            r.copyDestPitch = 32 | (32u << 16);
            const ed::ResolveInfo ri = ed::computeResolve(r);
            std::fill(dest.begin(), dest.end(), uint8_t(0));
            ed::resolveToGuest(edram.data(), ri, dest.data(), dest.size());
            const uint32_t s2 = rs::SanitizeCopySampleSelect(sel, msaa, false);
            const uint32_t first = rs::ResolveFirstSampleIndex(s2);
            const float factor = rs::ResolveSampleFactor(rs::DestExpBiasFactor((uint32_t(bias) & 0x3F) << 16), s2);
            for (uint32_t py = 0; py < 8; ++py)
              for (uint32_t px = 0; px < 8; ++px) {
                const float unit = float(1u + px + 8u * py);
                float sum = float(1u << first) * unit;
                if (s2 >= rs::k01) {
                  sum += float(1u << (first + 1)) * unit;
                  if (s2 >= rs::k0123) {
                    sum += float(1u << (first + 2)) * unit;
                    sum += float(1u << (first + 3)) * unit;
                  }
                }
                const uint32_t want = xref::resolve::EndianSwap32(AsU32(sum * factor), endian);
                const int32_t off = xref::texture_address::Tiled2D(int32_t(px), int32_t(py), 32, 2);
                uint32_t got;
                std::memcpy(&got, &dest[size_t(off)], 4);
                m.Check();
                if (got != want) {
                  m.Add(Fmt("msaa=%u sel=%u bias=%d endian=%u px=%u py=%u ours=%08X xenia=%08X", msaa, sel, bias,
                            endian, px, py, got, want));
                }
              }
          }
    }
    Compare(c, "ours.edram.resolveToGuest.k_32_FLOAT", m, Status::Fail,
            "gfx/edram.hpp:1029 end to end vs resolve.xesli:224-234 + 593-631, edram.xesli:23-58, "
            "texture_address.h:190, endian.xesli (1x/2x/4x MSAA, every select, 7 biases, endian none/8in32)");
  }
  // Endian swaps.
  {
    Mismatches m;
    xc::Rng rng(0xE11D1A);
    for (uint32_t endian = 0; endian < 8; ++endian) {
      for (int t = 0; t < 4096; ++t) {
        uint32_t v[4];
        for (uint32_t& w : v) w = rng.U32();
        uint8_t ours[16];
        std::memcpy(ours, v, 16);
        const uint32_t unit = std::max(ed::endianUnitBytes(endian), 1u);
        for (uint32_t o = 0; o < 16; o += unit) ed::endianSwapUnit(endian, ours + o);
        uint32_t ref[4] = {v[0], v[1], v[2], v[3]};
        xref::resolve::EndianSwap128(ref, endian);
        m.Check();
        if (std::memcmp(ours, ref, 16) != 0) {
          uint32_t o4[4];
          std::memcpy(o4, ours, 16);
          m.Add(Fmt("endian=%u in=%08X%08X%08X%08X ours=%08X%08X%08X%08X xenia=%08X%08X%08X%08X", endian, v[0],
                    v[1], v[2], v[3], o4[0], o4[1], o4[2], o4[3], ref[0], ref[1], ref[2], ref[3]));
        }
      }
    }
    Compare(c, "ours.edram.endianSwapUnit", m, Status::Fail,
            "gfx/edram.hpp:1012 vs endian.xesli:25-75, every 3-bit Endian128 value on 16-byte blocks");
  }
  // Destination formats: coverage, then per-pixel packing.
  {
    std::string gaps, extra;
    Mismatches size;
    for (uint32_t fmt = 0; fmt < 64; ++fmt) {
      const uint32_t ref = rs::ResolvableBitsPerPixel(fmt), ours = ed::destBytesPerTexel(fmt) * 8;
      if (ref && !ours) gaps += Fmt(" %u", fmt);
      if (!ref && ours) extra += Fmt(" %u", fmt);
      if (ref && ours) {
        size.Check();
        if (ref != ours) size.Add(Fmt("fmt=%u ours=%ubpp xenia=%ubpp", fmt, ours, ref));
      }
    }
    Compare(c, "ours.edram.destBytesPerTexel", size, Status::Fail,
            "gfx/edram.hpp:934 vs texture_info_formats.inl (kResolvable bits per pixel)");
    if (!gaps.empty()) {
      c.res.Report(Status::Gap, "ours.edram.resolve_dest_formats",
                   "Xenia-resolvable destination formats our resolveToGuest skips (destBytesPerTexel == 0, no "
                   "copy):" + gaps +
                       "  (8/9 k_8_A/k_8_B, 14 k_8_8_8_8_A, 50/54/55/56 *_AS_16_16_16_16)");
    }
    if (!extra.empty()) {
      c.res.Report(Status::Info, "ours.edram.resolve_dest_formats.extra",
                   "formats ours accepts that Xenia's table does not list as resolvable:" + extra +
                       "  (22/23 are the depth destinations Xenia substitutes at draw_util.cc:1105)");
    }
    Mismatches m;
    std::string skipped;
    xc::Rng rng(0xDE57);
    for (uint32_t fmt = 0; fmt < 64; ++fmt) {
      if (!ed::destBytesPerTexel(fmt) || !rs::ResolvableBitsPerPixel(fmt)) continue;
      uint8_t probe[16];
      const float zero[4] = {0, 0, 0, 0};
      if (!xref::xesli::PackDestPixel(fmt, zero, probe)) {
        skipped += Fmt(" %u", fmt);
        continue;
      }
      for (int t = 0; t < 200000; ++t) {
        float v[4];
        for (float& f : v) f = RandomColorChannel(rng);
        uint8_t o[16] = {}, r[16] = {};
        const uint32_t on = ed::packDest(fmt, v, o), rn = xref::xesli::PackDestPixel(fmt, v, r);
        m.Check();
        if (on != rn || std::memcmp(o, r, rn) != 0) {
          uint64_t ov = 0, rv = 0;
          std::memcpy(&ov, o, std::min(on, 8u));
          std::memcpy(&rv, r, std::min(rn, 8u));
          m.Add(Fmt("fmt=%u in=(%08X %08X %08X %08X) ours=%u:%llX xenia=%u:%llX", fmt, AsU32(v[0]), AsU32(v[1]),
                    AsU32(v[2]), AsU32(v[3]), on, (unsigned long long)ov, rn, (unsigned long long)rv));
        }
      }
    }
    Compare(c, "ours.edram.packDest", m, Status::Fail,
            "gfx/edram.hpp:950 vs pixel_formats.xesli:98-298 XePack*bpp + resolve_full_8bpp/128bpp, every "
            "resolvable format ours supports (random float4 + edges)");
    if (!skipped.empty()) {
      c.res.Report(Status::Info, "ours.edram.packDest.half_formats_unchecked",
                   "half-float destinations not checked (Xenia uses the host GPU's f32tof16):" + skipped);
    }
  }
}

// ===========================================================================
// tiling - 2D / 3D tiled addressing.
void TestTiling(Ctx& c) {
  namespace ta = xref::texture_address;
  static const uint32_t kPitches[] = {32, 64, 96, 128, 160, 256, 640, 1280, 2048, 8192};
  {
    xc::Golden g("tiled_address", "Xenos tiled texture addressing (byte offsets)");
    g.Comment("sources: texture_address.h:173-184 TiledCombine, :190-208 Tiled2D, :213-238 Tiled3D");
    g.Comment("digests: 2D over x < min(pitch, 2048), y < 1024 per (pitch, bpb_log2); 3D over x < pitch, "
              "y < height, z < 16");
    g.Comment("rows: 2D x y pitch bpb_log2 offset | 3D x y z pitch height bpb_log2 offset");
    Mismatches m2;
    for (uint32_t pitch : kPitches) {
      for (unsigned bpb = 0; bpb <= 4; ++bpb) {
        xc::Fnv h;
        uint64_t n = 0;
        for (int32_t y = -64; y < 1024; ++y) {
          for (int32_t x = -64; x < int32_t(std::min(pitch, 2048u)); ++x) {
            const int32_t r = ta::Tiled2D(x, y, pitch, bpb);
            if (x >= 0 && y >= 0) {
              h.U32(uint32_t(r));
              ++n;
            }
            m2.Check();
            const int32_t o = tl::tiled2D(x, y, pitch, bpb);
            if (o != r) m2.Add(Fmt("x=%d y=%d pitch=%u bpb_log2=%u ours=%d xenia=%d", x, y, pitch, bpb, o, r));
          }
        }
        g.Digest(Fmt("2d_pitch%u_bpb%u", pitch, bpb), n, h.h);
      }
    }
    Compare(c, "ours.tiling.tiled2D", m2, Status::Fail,
            "gfx/xenos_tiling.hpp:35 vs texture_address.h:190 Tiled2D (10 pitches x 5 bpb, incl. negative coords)");
    Mismatches m3;
    for (uint32_t pitch : {32u, 64u, 256u}) {
      for (uint32_t height : {32u, 64u, 128u}) {
        for (unsigned bpb = 0; bpb <= 4; ++bpb) {
          xc::Fnv h;
          uint64_t n = 0;
          for (int32_t z = 0; z < 16; ++z)
            for (int32_t y = 0; y < int32_t(height); ++y)
              for (int32_t x = 0; x < int32_t(pitch); ++x) {
                const int64_t r = ta::Tiled3D(x, y, z, pitch, height, bpb);
                h.U64(uint64_t(r));
                ++n;
                m3.Check();
                const int64_t o = tl::tiled3D(x, y, z, pitch, height, bpb);
                if (o != r) {
                  m3.Add(Fmt("x=%d y=%d z=%d pitch=%u height=%u bpb_log2=%u ours=%lld xenia=%lld", x, y, z, pitch,
                             height, bpb, (long long)o, (long long)r));
                }
              }
          g.Digest(Fmt("3d_pitch%u_height%u_bpb%u", pitch, height, bpb), n, h.h);
        }
      }
    }
    Compare(c, "ours.tiling.tiled3D", m3, Status::Fail, "gfx/xenos_tiling.hpp:68 vs texture_address.h:213 Tiled3D");
    xc::Rng rng(0x711E);
    for (uint32_t i = 0; i < kSampleRows / 2; ++i) {
      const uint32_t pitch = kPitches[rng.Below(10)], bpb = rng.Below(5);
      const int32_t x = int32_t(rng.Below(std::min(pitch, 2048u))), y = int32_t(rng.Below(1024));
      g.Row(Fmt("2D\t%d\t%d\t%u\t%u\t%d", x, y, pitch, bpb, ta::Tiled2D(x, y, pitch, bpb)));
    }
    for (uint32_t i = 0; i < kSampleRows / 2; ++i) {
      const uint32_t pitch = 32u << rng.Below(4), height = 32u << rng.Below(3), bpb = rng.Below(5);
      const int32_t x = int32_t(rng.Below(pitch)), y = int32_t(rng.Below(height)), z = int32_t(rng.Below(32));
      g.Row(Fmt("3D\t%d\t%d\t%d\t%u\t%u\t%u\t%lld", x, y, z, pitch, height, bpb,
                (long long)ta::Tiled3D(x, y, z, pitch, height, bpb)));
    }
    g.Finish(c.res, c.data, c.mode);
  }
  // untile32 (front-buffer / CPU readback untile) vs a Tiled2D-based untile.
  {
    struct S {
      uint32_t w, h, pitch;
    };
    const S sizes[] = {{1280, 720, 1280}, {100, 50, 128}, {33, 17, 64}, {640, 480, 640}, {64, 64, 32}};
    Mismatches m;
    xc::Rng rng(0x0711E);
    for (const S& s : sizes) {
      const size_t span = tl::tiledSpanBytes32(s.pitch, s.h);
      std::vector<uint8_t> src(span), ours(size_t(s.w) * s.h * 4), ref(size_t(s.w) * s.h * 4);
      for (auto& b : src) b = uint8_t(rng.U32());
      tl::untile32(ours.data(), src.data(), s.pitch, s.w, s.h);
      const uint32_t wEff = std::min(s.w, s.pitch);
      for (uint32_t y = 0; y < s.h; ++y)
        for (uint32_t x = 0; x < wEff; ++x) {
          const int32_t off = ta::Tiled2D(int32_t(x), int32_t(y), s.pitch, 2);
          std::memcpy(&ref[(size_t(y) * s.w + x) * 4], &src[size_t(off)], 4);
        }
      m.Check();
      if (ours != ref) {
        size_t i = 0;
        while (ours[i] == ref[i]) ++i;
        m.Add(Fmt("w=%u h=%u pitch=%u first diff at texel (%zu,%zu)", s.w, s.h, s.pitch, (i / 4) % s.w,
                  (i / 4) / s.w));
      }
    }
    Compare(c, "ours.tiling.untile32", m, Status::Fail,
            "gfx/xenos_tiling.hpp:94 vs a texture_address.h:190 Tiled2D untile (5 surface sizes)");
  }
}

// ===========================================================================
// gamma_ramp - the display gamma ramp (DC_LUT registers + scan-out shaders).
struct OurRegFile {
  uint32_t v[8] = {};
  uint32_t read(uint32_t r) { return v[r - 0x1920]; }
  void write(uint32_t r, uint32_t x) { v[r - 0x1920] = x; }
};
void TestGammaRamp(Ctx& c) {
  namespace gr = xref::gamma_ramp;
  // Defaults.
  {
    gm::Ramp ours;
    gr::CommandProcessorGamma ref;
    Mismatches m;
    for (uint32_t i = 0; i < 256; ++i) {
      m.Check();
      if (ours.table[i] != ref.gamma_ramp_256_entry_table_[i]) {
        m.Add(Fmt("table[%u] ours=%08X xenia=%08X", i, ours.table[i], ref.gamma_ramp_256_entry_table_[i]));
      }
    }
    for (uint32_t i = 0; i < 128; ++i)
      for (uint32_t j = 0; j < 3; ++j) {
        m.Check();
        if (ours.pwl[i][j] != ref.gamma_ramp_pwl_rgb_[i][j]) {
          m.Add(Fmt("pwl[%u][%u] ours=%08X xenia=%08X", i, j, ours.pwl[i][j], ref.gamma_ramp_pwl_rgb_[i][j]));
        }
      }
    Compare(c, "ours.gamma.Ramp.reset", m, Status::Fail,
            "gfx/gamma_ramp.hpp:43 vs command_processor.cc:163-183 (linear defaults)");
  }
  // Application: default ramp + random ramps.
  {
    Mismatches pwl, table;
    xc::Rng rng(0x6A33A);
    for (int t = 0; t < 65; ++t) {
      gm::Ramp ours;
      gr::CommandProcessorGamma ref;
      if (t > 0) {
        for (uint32_t i = 0; i < 128; ++i)
          for (uint32_t j = 0; j < 3; ++j) {
            const uint32_t e = rng.U32() & ~((0x3Fu << 16) | 0x3Fu);
            ours.pwl[i][j] = e;
            ref.gamma_ramp_pwl_rgb_[i][j] = e;
          }
        for (uint32_t i = 0; i < 256; ++i) {
          const uint32_t e = rng.U32() & 0x3FFFFFFF;
          ours.table[i] = e;
          ref.gamma_ramp_256_entry_table_[i] = e;
        }
      }
      for (uint32_t ch = 0; ch < 3; ++ch) {
        for (uint32_t v = 0; v < 1024; ++v) {
          pwl.Check();
          const uint32_t o = AsU32(ours.pwl01(v, ch)), r = AsU32(gr::ApplyPWLGammaChannel(ref, v, ch));
          if (o != r) pwl.Add(Fmt("ramp#%d ch=%u v10=%u ours=%08X xenia=%08X", t, ch, v, o, r));
        }
        for (uint32_t v = 0; v < 256; ++v) {
          table.Check();
          const uint32_t o = ours.table10(v, ch), r = gr::ApplyTableChannel10(ref, v, ch);
          if (o != r) table.Add(Fmt("ramp#%d ch=%u v8=%u ours=%03X xenia=%03X", t, ch, v, o, r));
        }
      }
    }
    Compare(c, "ours.gamma.Ramp.pwl01", pwl, Status::Fail,
            "gfx/gamma_ramp.hpp:128 vs apply_gamma_pwl.xesli:12-23 + 74-93 (default + 64 random ramps)");
    Compare(c, "ours.gamma.Ramp.table10", table, Status::Fail,
            "gfx/gamma_ramp.hpp:119 vs apply_gamma_table.xesli:58-71 (default + 64 random tables)");
  }
  // Register-write side effects: random DC_LUT write sequences.
  {
    xc::Golden g("gamma_ramp", "display gamma ramp: default PWL output and state after seeded DC_LUT write sequences");
    g.Comment("sources: command_processor.cc:163-183, :562-713 | apply_gamma_pwl.xesli:12-23");
    g.Comment("rows 'P': v10  PWL output bits (default ramp, channel 0);  rows 'S': sequence  state digest");
    {
      gr::CommandProcessorGamma ref;
      for (uint32_t v = 0; v < 1024; v += 4) g.Row(Fmt("P\t%03X\t%08X", v, AsU32(gr::ApplyPWLGammaChannel(ref, v, 0))));
    }
    Mismatches m;
    xc::Rng rng(0xD0C1);
    for (int seq = 0; seq < 300; ++seq) {
      gm::Ramp ours;
      OurRegFile regs;
      gr::CommandProcessorGamma ref;
      for (int w = 0; w < 800; ++w) {
        uint32_t reg, val;
        switch (rng.Below(10)) {
          case 0: reg = 0x1921; val = rng.Below(2); break;
          case 1: reg = 0x1922; val = rng.Below(4) == 0 ? rng.U32() : rng.Below(256); break;
          case 2: reg = 0x1927; val = rng.Below(4) == 0 ? rng.U32() : rng.Below(8); break;
          case 3: case 4: reg = 0x1923; val = rng.U32(); break;
          case 5: case 6: reg = 0x1924; val = rng.U32(); break;
          case 7: case 8: reg = 0x1925; val = rng.U32(); break;
          default: reg = 0x1926; val = rng.U32(); break;
        }
        regs.write(reg, val);
        if (reg >= gm::kRegRwMode && reg <= gm::kRegWriteEnMask) ours.onRegisterWrite(reg, val, regs);
        ref.WriteRegister(reg, val);
      }
      xc::Fnv h;
      for (uint32_t i = 0; i < 256; ++i) h.U32(ref.gamma_ramp_256_entry_table_[i]);
      for (uint32_t i = 0; i < 128; ++i)
        for (uint32_t j = 0; j < 3; ++j) h.U32(ref.gamma_ramp_pwl_rgb_[i][j]);
      h.U32(ref.gamma_ramp_rw_component_);
      h.U32(ref.reg(gr::XE_GPU_REG_DC_LUT_RW_INDEX));
      g.Row(Fmt("S\t%03d\t%016llX", seq, (unsigned long long)h.h));
      m.Check();
      std::string diff;
      for (uint32_t i = 0; i < 256 && diff.empty(); ++i) {
        if (ours.table[i] != ref.gamma_ramp_256_entry_table_[i]) {
          diff = Fmt("table[%u] ours=%08X xenia=%08X", i, ours.table[i], ref.gamma_ramp_256_entry_table_[i]);
        }
      }
      for (uint32_t i = 0; i < 128 && diff.empty(); ++i)
        for (uint32_t j = 0; j < 3 && diff.empty(); ++j) {
          if (ours.pwl[i][j] != ref.gamma_ramp_pwl_rgb_[i][j]) {
            diff = Fmt("pwl[%u][%u] ours=%08X xenia=%08X", i, j, ours.pwl[i][j], ref.gamma_ramp_pwl_rgb_[i][j]);
          }
        }
      if (diff.empty() && ours.rwComponent != ref.gamma_ramp_rw_component_) {
        diff = Fmt("rw_component ours=%u xenia=%u", ours.rwComponent, ref.gamma_ramp_rw_component_);
      }
      if (diff.empty() && regs.read(0x1922) != ref.reg(gr::XE_GPU_REG_DC_LUT_RW_INDEX)) {
        diff = Fmt("RW_INDEX ours=%08X xenia=%08X", regs.read(0x1922), ref.reg(gr::XE_GPU_REG_DC_LUT_RW_INDEX));
      }
      if (!diff.empty()) m.Add(Fmt("sequence %d: ", seq) + diff);
    }
    g.Finish(c.res, c.data, c.mode);
    Compare(c, "ours.gamma.Ramp.onRegisterWrite", m, Status::Fail,
            "gfx/gamma_ramp.hpp:61 vs command_processor.cc:562-713 (300 seeded sequences x 800 DC_LUT writes)");
  }
}

int Usage() {
  std::fprintf(stderr,
               "usage: xenos_conformance [--data DIR] [--regen | --verify] [--exhaustive] [--threads N]\n"
               "                         [--strict] [--only SUBSTRING]\n"
               "see the comment block at the top of tests/xenos_conformance/conformance_main.cpp\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  Ctx c;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--data" && i + 1 < argc) {
      c.data = argv[++i];
    } else if (a == "--regen") {
      c.mode = xc::GoldenMode::Regen;
    } else if (a == "--verify") {
      c.mode = xc::GoldenMode::Verify;
    } else if (a == "--exhaustive") {
      c.exhaustive = true;
    } else if (a == "--strict") {
      c.strict = true;
    } else if (a == "--threads" && i + 1 < argc) {
      c.threads = unsigned(std::max(1, std::atoi(argv[++i])));
    } else if (a == "--only" && i + 1 < argc) {
      c.only = argv[++i];
    } else {
      return Usage();
    }
  }
  std::printf("xenos_conformance: goldens in %s, %s, %u thread(s)\n", c.data.generic_string().c_str(),
              c.exhaustive ? "exhaustive" : "dense grid", c.threads);
  struct Group {
    const char* name;
    void (*fn)(Ctx&);
  };
  const Group groups[] = {
      {"f7e3", TestF7e3},         {"f20e4", TestF20e4},     {"depth", TestDepth},
      {"edram_color", TestEdramColor}, {"pwl_gamma", TestPwlGamma}, {"resolve", TestResolve},
      {"tiling", TestTiling},     {"gamma_ramp", TestGammaRamp},
  };
  for (const Group& g : groups) {
    if (!c.only.empty() && std::string(g.name).find(c.only) == std::string::npos) continue;
    std::printf("\n== %s ==\n", g.name);
    const auto t0 = std::chrono::steady_clock::now();
    g.fn(c);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("   (%s: %.1f s)\n", g.name, s);
  }
  const int* n = c.res.counts;
  std::printf("\n== summary ==\nPASS %d  FAIL %d  GAP %d  WAIVED %d  INFO %d\n", n[int(Status::Pass)],
              n[int(Status::Fail)], n[int(Status::Gap)], n[int(Status::Waived)], n[int(Status::Info)]);
  for (const auto& l : c.res.notable) {
    const std::string first = l.substr(0, l.find('\n'));
    std::printf("  %s\n", first.c_str());
  }
  const bool fail = n[int(Status::Fail)] > 0 ||
                    (c.strict && (n[int(Status::Gap)] > 0 || n[int(Status::Waived)] > 0));
  return fail ? 1 : 0;
}
