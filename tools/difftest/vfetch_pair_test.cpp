// iter-458 H-PAIR: known-answer tests for the guest D3D runtime's VS-input <->
// vertex-decl pairing rule (gfx/vfetch_pair_rule.hpp, from
// ShaderAssembleVertexFetches_sub_82107A30).
//
// Run: build/difftest.exe --vfetch-pair-test   (exit code = failed checks)
//
// The decls are the ones hwin4.srht records for the four dominant world VS
// (tools: scratchpad decl_by_vs.py on archive/iter458_hwin4.srht).
#include <cstdint>
#include <cstdio>

#include "gfx/vfetch_pair_rule.hpp"

namespace {

namespace P = sr::gfx::vfpair;
using sr::gfx::VfetchElement;

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL %s\n", name); }
}

// The backend's xeFormatBytes for the formats these tests use (0 = unknown).
uint32_t fmtBytes(uint32_t f) {
  switch (f) {
    case 6: case 7: case 25: case 33: case 36: return 4;   // 8888 2_10_10_10 16_16 32 32F
    case 26: case 37: return 8;                            // 16x4, 32_32F
    case 57: return 12;                                    // 32_32_32F
    case 38: return 16;                                    // 32x4F
    default: return 0;
  }
}

// An UNPATCHED sidecar entry: slot 95, offset/stride/format zero.
VfetchElement sc(uint32_t usage, uint32_t idx) {
  VfetchElement e{};
  e.slot = 95; e.usage = usage; e.usageIndex = idx;
  return e;
}

// Packed X360 decl type: low 6 bits format, bit 8 signed, bit 9 integer.
constexpr uint32_t T(uint32_t fmt, bool sgn = false, bool integer = false) {
  return fmt | (sgn ? 0x100u : 0u) | (integer ? 0x200u : 0u);
}

enum : uint32_t { POS = 0, BLENDW = 1, BLENDI = 2, NRM = 3, TC = 5, TAN = 6, BIN = 7, COL = 10 };

}  // namespace

int runVfetchPairTest() {
  std::printf("[VFETCH_PAIR_TEST] guest pairing rule (ShaderAssembleVertexFetches_sub_82107A30)\n");
  g_pass = g_fail = 0;
  P::How how{};

  {
    // K1 - VS 6A9C49D90E5F61F0 on its dominant 36-byte decl: NORMAL0 is
    // 2_10_10_10 (signed) at +12. OFFPAIR on the unpatched sidecar read +0.
    const VfetchElement s[2] = {sc(POS, 0), sc(NRM, 0)};
    const P::DeclEl d[2] = {{0, 0, T(57), POS, 0}, {12, 0, T(7, true), NRM, 0}};
    check(P::sidecarUnpatched(s, 2) && P::anySemanticMatch(s, 2, d, 2),
          "K1pre sidecar unpatched and non-empty");
    const VfetchElement a = P::pairGuestRule(s[0], d, 2, 0, 36, fmtBytes, &how);
    check(a.offsetDwords == 0 && a.format == 57 && how == P::How::Matched,
          "K1a POSITION0 <- +0 32_32_32_FLOAT");
    const VfetchElement b = P::pairGuestRule(s[1], d, 2, 0, 36, fmtBytes, &how);
    check(b.offsetDwords == 3 && b.format == 7 && b.isSigned && !b.isInteger &&
              how == P::How::Matched,
          "K1b NORMAL0 <- +12 2_10_10_10 signed (the offset OFFPAIR lost)");
  }
  {
    // K2 - VS 7AF01C0866CB8F14 (skinned), 40-byte decl: BLENDINDICES0 8_8_8_8
    // at +28 and BLENDWEIGHT0 16_16_16_16 at +32 -- not the position floats.
    const P::DeclEl d[7] = {{0, 0, T(57), POS, 0},     {12, 0, T(7, true), NRM, 0},
                            {16, 0, T(7, true), TAN, 0}, {20, 0, T(7, true), BIN, 0},
                            {24, 0, T(25, true), TC, 0}, {28, 0, T(6, false, true), BLENDI, 0},
                            {32, 0, T(26), BLENDW, 0}};
    const VfetchElement bi = P::pairGuestRule(sc(BLENDI, 0), d, 7, 0, 40, fmtBytes, &how);
    check(bi.offsetDwords == 7 && bi.format == 6 && bi.isInteger && how == P::How::Matched,
          "K2a BLENDINDICES0 <- +28 8_8_8_8 integer");
    const VfetchElement bw = P::pairGuestRule(sc(BLENDW, 0), d, 7, 0, 40, fmtBytes, &how);
    check(bw.offsetDwords == 8 && bw.format == 26 && how == P::How::Matched,
          "K2b BLENDWEIGHT0 <- +32 16_16_16_16");
    const VfetchElement tc = P::pairGuestRule(sc(TC, 0), d, 7, 0, 40, fmtBytes, &how);
    check(tc.offsetDwords == 6 && tc.format == 25 && tc.isSigned, "K2c TEXCOORD0 <- +24 16_16 signed");
  }
  {
    // K3 - sidecar order differs from decl order (u 0,3,6,5 vs 0,3,5,6): each
    // input takes ITS OWN semantic, never its position.
    const P::DeclEl d[4] = {{0, 0, T(57), POS, 0}, {12, 0, T(7, true), NRM, 0},
                            {16, 0, T(37), TC, 0}, {24, 0, T(7, true), TAN, 0}};
    const VfetchElement t = P::pairGuestRule(sc(TAN, 0), d, 4, 0, 28, fmtBytes, &how);
    const VfetchElement c = P::pairGuestRule(sc(TC, 0), d, 4, 0, 28, fmtBytes, &how);
    check(t.offsetDwords == 6 && t.format == 7 && c.offsetDwords == 4 && c.format == 37,
          "K3 input 2 (TANGENT) <- +24 and input 3 (TEXCOORD) <- +16 regardless of order");
  }
  {
    // K4 - usageIndex discriminates: TEXCOORD1 is not TEXCOORD0.
    const P::DeclEl d[2] = {{24, 0, T(37), TC, 0}, {32, 0, T(25, true), TC, 1}};
    const VfetchElement t1 = P::pairGuestRule(sc(TC, 1), d, 2, 0, 36, fmtBytes, &how);
    check(t1.offsetDwords == 8 && t1.format == 25 && how == P::How::Matched,
          "K4 TEXCOORD1 <- +32 16_16, not TEXCOORD0");
  }
  {
    // D1 - duplicate (usage, usageIndex): the FIRST in decl order wins, on any
    // stream (the guest loop breaks on the first hit).
    const P::DeclEl sameStream[2] = {{12, 0, T(57), POS, 0}, {0, 0, T(57), POS, 0}};
    const VfetchElement a = P::pairGuestRule(sc(POS, 0), sameStream, 2, 0, 36, fmtBytes, &how);
    check(a.offsetDwords == 3 && how == P::How::Matched, "D1a same-stream duplicate: first (+12) wins");
    const P::DeclEl crossStream[2] = {{0, 1, T(33), POS, 0}, {0, 0, T(57), POS, 0}};
    P::pairGuestRule(sc(POS, 0), crossStream, 2, 0, 36, fmtBytes, &how);
    check(how == P::How::DummyForeign,
          "D1b cross-stream duplicate: the first (stream 1) wins -> foreign dummy, not the later s0 element");
  }
  {
    // F1 - VS 592004AEBC573BC6's POSITION1 lives on stream 1 (4-byte k_32):
    // the producer snapshots stream 0 only -> bounded dummy at +0.
    const P::DeclEl d[2] = {{0, 0, T(57), POS, 0}, {0, 1, T(33), POS, 1}};
    const VfetchElement p1 = P::pairGuestRule(sc(POS, 1), d, 2, 0, 36, fmtBytes, &how);
    check(how == P::How::DummyForeign && p1.offsetDwords == 0 && p1.format == 33,
          "F1 POSITION1 on stream 1 -> DummyForeign at +0, own format");
  }
  {
    // U1 - no decl element carries the semantic: the guest default (slot 95,
    // offset 0, k_8_8_8_8, unsigned, normalized), never a positional fallback.
    const P::DeclEl d[2] = {{0, 0, T(57), POS, 0}, {12, 0, T(7, true), NRM, 0}};
    const VfetchElement c = P::pairGuestRule(sc(COL, 0), d, 2, 0, 36, fmtBytes, &how);
    check(how == P::How::Default && c.offsetDwords == 0 && c.format == 6 && !c.isSigned &&
              !c.isInteger && !c.bgra,
          "U1 unmatched COLOR0 -> Default +0 k_8_8_8_8 unsigned normalized");
    P::pairGuestRule(sc(COL, 0), d, 2, 1, 36, fmtBytes, &how);
    check(how == P::How::DummyForeign, "U2 default resolves to stream 0; gathered stream 1 -> foreign dummy");
  }
  {
    // B1 - a matched element outside the record, or an undecodable format.
    const P::DeclEl d[2] = {{28, 0, T(57), POS, 0}, {12, 0, T(16), NRM, 0}};
    P::pairGuestRule(sc(POS, 0), d, 2, 0, 36, fmtBytes, &how);
    check(how == P::How::DummyBad, "B1 +28 + 12 bytes > stride 36 -> DummyBad");
    const VfetchElement n = P::pairGuestRule(sc(NRM, 0), d, 2, 0, 36, fmtBytes, &how);
    check(how == P::How::DummyBad && n.format == 38 && n.offsetDwords == 0,
          "B2 undecodable format 16 -> DummyBad with the OFFPAIR fallback format");
  }
  {
    // P1 - provenance predicate: patched or empty sidecars are NOT taken.
    VfetchElement s[2] = {sc(POS, 0), sc(NRM, 0)};
    check(P::sidecarUnpatched(s, 2), "P1a all-zero sidecar -> unpatched");
    s[1].strideDwords = 9;
    check(!P::sidecarUnpatched(s, 2), "P1b a stride -> patched (keeps OFFPAIR)");
    s[1].strideDwords = 0; s[1].offsetDwords = 3;
    check(!P::sidecarUnpatched(s, 2), "P1c an offset -> patched");
    s[1].offsetDwords = 0; s[1].format = 7;
    check(!P::sidecarUnpatched(s, 2), "P1d a format -> patched");
    check(!P::sidecarUnpatched(s, 0), "P1e empty sidecar -> not taken");
    const VfetchElement u[1] = {sc(COL, 0)};
    const P::DeclEl d[1] = {{0, 0, T(57), POS, 0}};
    check(!P::anySemanticMatch(u, 1, d, 1), "P2 no semantic in common -> not taken");
  }

  std::printf("[VFETCH_PAIR_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
