// P1 (docs/RENDERER_PM4_HYBRID.md): known-answer tests for gfx/edram.hpp, the
// EDRAM model ported from Xenia (docs/XENIA_EDRAM_CONTRACT.md).
//
// Run: build/difftest.exe --edram-test   (exit code = failed checks)
//
// Answers come from the Xenia formulas worked by hand, and from register
// values the menu trace actually carries (resolve#7: the 1280x720 scene copy).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "gfx/edram.hpp"
#include "gfx/drain_reuse.hpp"          // step 6 R6 fix round 2: testDrainReuse
#include "gfx/render_draw_command.hpp"  // GuestDrawCommand (regsFromCommand / refreshNonKey)

namespace {

namespace E = sr::gfx::edram;
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL %s\n", name); }
}
bool near(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps; }

void testKeys() {
  check(E::pitchTiles32(1280, E::kMsaa1X) == 16, "pitch 1280@1x = 16 tiles");
  check(E::pitchTiles32(640, E::kMsaa4X) == 16, "pitch 640@4x = 16 tiles");
  check(E::pitchTiles32(320, E::kMsaa4X) == 8, "pitch 320@4x = 8 tiles");
  check(E::pitchTiles32(80, E::kMsaa1X) == 1, "pitch 80@1x = 1 tile");
  const E::RtKey k = E::RtKey::make(0x2D0, 16, E::kMsaa1X, false, 3);
  check(k.base() == 0x2D0 && k.pitchTiles32() == 16 && k.format() == 3 && !k.isDepth(),
        "RtKey fields round-trip");
  check(k.width() == 1280 && k.height() == 2048, "1280@1x host RT is 1280x2048 (contract example)");
  const E::RtKey k4 = E::RtKey::make(0, 16, E::kMsaa4X, false, 4);
  check(k4.width() == 640 && k4.height() == 1024, "640@4x host RT is 640x1024");
  check(k4.tileWidthPixels() == 40 && k4.tileHeightPixels() == 8, "4x tile = 40x8 px");
  const E::RtKey k64 = E::RtKey::make(0, 16, E::kMsaa1X, false, 7);
  check(k64.is64bpp() && k64.pitchTiles() == 32 && k64.width() == 1280 && k64.tileWidthPixels() == 40,
        "64bpp: twice the tiles, same width");
  check(E::colorStorageFormat(12) == 3 && E::colorStorageFormat(10) == 2, "storage aliases 12->3, 10->2");
  check(E::colorResourceFormat(1) == 0, "8888_GAMMA stores as 8888");
}

void testFloats() {
  check(E::floatToHalf(1.0f) == 0x3C00 && E::halfToFloat(0x3C00) == 1.0f, "half 1.0");
  check(E::floatToHalf(-2.5f) == 0xC100, "half -2.5");
  check(E::floatToHalf(65504.0f) == 0x7BFF, "half max");
  check(E::floatTo7e3(1.0f) == 0x180 && E::f7e3ToFloat(0x180) == 1.0f, "7e3 1.0 = 0x180");
  check(E::floatTo7e3(31.875f) == 0x3FF && E::floatTo7e3(100.0f) == 0x3FF, "7e3 saturates at 31.875");
  check(E::floatTo7e3(0.125f) == 0x40 && E::f7e3ToFloat(0x40) == 0.125f, "7e3 denormal 0.125 = 0x40");
  check(E::floatTo7e3(-1.0f) == 0 && E::floatTo7e3(0.0f) == 0, "7e3 negative/zero -> 0");
  bool rt = true;
  for (uint32_t b = 0; b < 0x400; b++) rt = rt && E::floatTo7e3(E::f7e3ToFloat(b)) == b;
  check(rt, "7e3 all 1024 codes round-trip");
  check(E::floatTo20e4(1.0f) == 0xF00000 && E::f20e4ToFloat(0xF00000) == 1.0f, "20e4 1.0 = 0xF00000");
  check(E::floatTo20e4(0.5f) == 0xE00000, "20e4 0.5 = 0xE00000");
  bool rt24 = true;
  for (uint32_t b = 0; b < 0x1000000; b += 4099) rt24 = rt24 && E::floatTo20e4(E::f20e4ToFloat(b)) == b;
  check(rt24, "20e4 codes round-trip (sampled)");
  check(E::packFixed16Edram(32.0f) == 0x7FFF && E::packFixed16Edram(-32.0f) == 0x8001,
        "16_16 edram fixed: +-32");
  check(near(E::unpackFixed16Edram(0x8001), -32.0f) && near(E::unpackFixed16Edram(0x7FFF), 32.0f),
        "16_16 edram fixed unpack");
  bool d = true;
  for (uint32_t n = 0; n < 0x1000000; n += 7919)
    d = d && (E::packDepth(0, E::unpackDepth(0, n << 8), 0) >> 8) == n;
  check(d, "D24S8 unorm24 round-trips through host float");
  bool df = true;
  for (uint32_t n = 0; n < 0x1000000; n += 7919)
    df = df && (E::packDepth(1, E::unpackDepth(1, n << 8), 0) >> 8) == n;
  check(df, "D24FS8 20e4 round-trips through host float (x0.5 convention)");
}

void testColorPack() {
  const float c[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  float o[4];
  bool ok = true;
  for (uint32_t f : {0u, 2u, 3u, 7u}) {
    E::unpackColor(f, E::packColor(f, c), o);
    for (int i = 0; i < 4; i++) ok = ok && near(o[i], c[i], f == 2 ? 0.34f : 2e-3f);
  }
  check(ok, "8888 / 2_10_10_10 / 7e3 / 16x4F pack-unpack");
  check(E::packColor(0, c) == (64u | (128u << 8) | (191u << 16) | (255u << 24)), "8888 bit layout R low");
  const float big[4] = {-20.0f, 31.0f, 0, 0};
  E::unpackColor(4, E::packColor(4, big), o);
  check(near(o[0], -20.0f, 2e-3f) && near(o[1], 31.0f, 2e-3f), "16_16 keeps the [-32,32] range");
  const float f32[4] = {1234.5f, -7.25f, 0, 0};
  E::unpackColor(15, E::packColor(15, f32), o);
  check(o[0] == 1234.5f && o[1] == -7.25f, "32_32_FLOAT exact");
}

void testAddressing() {
  check(E::sampleOffsetBytes(0, 0, 0, 16, 0, false, 0, 0) == 0, "addr (0,0)");
  check(E::sampleOffsetBytes(1, 0, 0, 16, 0, false, 0, 0) == 4, "addr (1,0) = 4");
  check(E::sampleOffsetBytes(0, 1, 0, 16, 0, false, 0, 0) == 320, "addr (0,1) = one 80-sample row");
  check(E::sampleOffsetBytes(80, 0, 0, 16, 0, false, 0, 0) == 5120, "addr (80,0) = next tile");
  check(E::sampleOffsetBytes(0, 16, 0, 16, 0, false, 0, 0) == 16 * 5120, "addr (0,16) = next tile row");
  check(E::sampleOffsetBytes(0, 0, 0, 16, 0, true, 0, 0) == 160, "depth halves swapped: (0,0) -> sample 40");
  check(E::sampleOffsetBytes(40, 0, 0, 16, 0, true, 0, 0) == 0, "depth halves swapped: (40,0) -> sample 0");
  check(E::sampleOffsetBytes(1, 0, 0, 16, 2, false, 0, 0) == 8, "4x: pixel 1 sample 0 = sample column 2");
  check(E::sampleOffsetBytes(0, 0, 0, 16, 2, false, 0, 2) == 4, "4x: sample 2 = right column");
  check(E::sampleOffsetBytes(0, 0, 0, 16, 2, false, 0, 1) == 320, "4x: sample 1 = row below");
  check(E::sampleOffsetBytes(0, 1, 0, 16, 1, false, 0, 0) == 640, "2x: pixel row 1 = sample row 2");
  check(E::sampleOffsetBytes(0, 0, 2047, 1, 0, false, 0, 0) == 2047u * 5120u, "last tile");
  check(E::sampleOffsetBytes(0, 16, 2047, 1, 0, false, 0, 0) == 0, "EDRAM wraps mod 2048 tiles");
  check(E::sampleOffsetBytes(1, 0, 0, 32, 0, false, 1, 0) == 8, "64bpp: 8 bytes per sample");
}

void testOwnership() {
  E::Ownership own;
  std::vector<E::Transfer> t;
  const E::RtKey A = E::RtKey::make(0, 16, 0, false, 3), B = E::RtKey::make(0, 8, 2, true, 0);
  own.claim(A, 0, 720, t);
  check(t.empty(), "first claim transfers nothing");
  own.claim(B, 0, 360, t);
  check(t.size() == 1 && t[0].start == 0 && t[0].end == 360 && t[0].source == A, "overlap -> transfer from A");
  t.clear();
  own.claim(A, 0, 720, t);
  check(t.size() == 1 && t[0].start == 0 && t[0].end == 360 && t[0].source == B, "reclaim moves only B's tiles");
  t.clear();
  own.claim(A, 0, 720, t);
  check(t.empty(), "claim by the owner is free");
  const E::RtKey C = E::RtKey::make(2000, 1, 0, false, 0);
  own.claim(C, 2000, 100, t);
  check(t.size() == 1 && t[0].start == 0 && t[0].end == 52 && t[0].source == A, "wrap claim: [2000,2048)+[0,52)");
  std::vector<E::Transfer> o;
  own.owners(0, 2048, o);
  check(o.size() == 4 && o[0].source == C && o[1].source == A && o[1].end == 720 && o[2].source.empty() &&
            o[3].source == C,
        "owners(): C | A | empty | C");
}

void testRects() {
  const E::RtKey k = E::RtKey::make(0, 16, 0, false, 0);
  E::Rect r[3];
  int n = E::rangeRects(k, 0, 16, r);
  check(n == 1 && r[0].x == 0 && r[0].y == 0 && r[0].w == 1280 && r[0].h == 16, "one full tile row");
  n = E::rangeRects(k, 8, 40, r);
  check(n == 3 && r[0].x == 640 && r[0].w == 640 && r[1].y == 16 && r[1].w == 1280 && r[1].h == 16 &&
            r[2].y == 32 && r[2].w == 640,
        "partial / full / partial rows");
  const E::RtKey kb = E::RtKey::make(720, 16, 0, true, 0);
  n = E::rangeRects(kb, 720, 736, r);
  check(n == 1 && r[0].y == 0 && r[0].w == 1280, "rects are relative to the key's own base");
}

void testViewport() {
  E::Regs r;
  r.vteCntl = 0x43F;
  r.vpScale[0] = 640; r.vpScale[1] = -360; r.vpScale[2] = 1;
  r.vpOffset[0] = 640; r.vpOffset[1] = 360; r.vpOffset[2] = 0;
  r.clipCntl = 1u << 19;  // dx_clip_space_def
  const E::HostViewport v = E::computeViewport(r, false, false);
  check(v.x == 0 && v.y == 0 && v.w == 1280 && v.h == 720 && !v.empty, "VTE-on 1280x720 host viewport");
  check(near(v.ndcScale[0], 1) && near(v.ndcScale[1], 1), "ndc scale identity after Y flip");
  check(near(v.ndcOffset[0], 1.0f / 1280) && near(v.ndcOffset[1], -1.0f / 720), "ndc offset = half pixel");
  check(v.minZ == 0 && v.maxZ == 1, "dx z range [0,1]");
  E::Regs u;
  u.vteCntl = 0x300;
  u.clipCntl = 1u << 16;  // clip_disable: pixel-space (XYZRHW-style) draw
  const E::HostViewport w = E::computeViewport(u, false, false);
  // A vertex at pixel X lands at X + 0.5 through the 8192-wide viewport.
  const float ndcX = 100.0f * w.ndcScale[0] + w.ndcOffset[0];
  const float px = (ndcX + 1.0f) * 0.5f * w.w;
  const float ndcY = 50.0f * w.ndcScale[1] + w.ndcOffset[1];
  const float py = (1.0f - ndcY) * 0.5f * w.h;
  check(w.w == 8192 && near(px, 100.5f, 1e-3f) && near(py, 50.5f, 1e-3f), "clip-disabled pixel draw maps X -> X+0.5");
}

void testWindowOffset() {
  E::Regs r;
  r.surfaceInfo = 1280;
  r.windowOffset = 640u | (360u << 16);
  r.windowScissorTL = 0;  // bit31 clear: the window offset applies
  r.windowScissorBR = 640u | (360u << 16);
  r.screenScissorBR = 0x20002000u;
  const E::Scissor s = E::computeScissor(r, true);
  check(s.x == 640 && s.y == 360 && s.w == 640 && s.h == 360, "window offset shifts the scissor");
  r.windowScissorTL = 0x80000000u;
  const E::Scissor s2 = E::computeScissor(r, true);
  check(s2.x == 0 && s2.y == 0 && s2.w == 640, "window_offset_disable keeps the scissor unshifted");
  r.suScModeCntl = 1u << 16;  // vtx_window_offset_enable
  r.vteCntl = 0x43F;
  r.vpScale[0] = 320; r.vpOffset[0] = 320; r.vpScale[1] = -180; r.vpOffset[1] = 180;
  r.clipCntl = 1u << 19;
  const E::HostViewport v = E::computeViewport(r, false, false);
  check(v.x == 640 && v.y == 360 && v.w == 640 && v.h == 360, "vtx_window_offset_enable shifts the viewport");
  r.windowOffset = uint32_t(-8 & 0x7FFF) | (uint32_t(-16 & 0x7FFF) << 16);
  check(E::windowOffsetX(r) == -8 && E::windowOffsetY(r) == -16, "window offset is signed 15-bit");
}

void testDrawTargets() {
  E::Regs r;
  r.modeControl = 4;
  r.surfaceInfo = 1280;
  r.colorInfo[0] = (3u << 16) | 0;
  r.depthInfo = 0x2D0;
  r.colorMask = 0xF;
  r.depthControl = 0x2 | 0x4 | (3u << 4);
  r.windowScissorTL = 0x80000000u;
  r.windowScissorBR = 1280u | (720u << 16);
  r.screenScissorBR = 0x20002000u;
  r.clipCntl = 1u << 19;
  r.vteCntl = 0x43F;
  r.vpScale[1] = -360; r.vpOffset[1] = 360;
  E::DrawTargets t = E::computeDrawTargets(r, 1u, true);
  check(t.rasterize && t.colorWriteMask[0] == 0xF && t.color[0].format() == 3 && t.depthBound &&
            t.depth.base() == 0x2D0,
        "scene draw binds fmt3 color + depth@0x2D0");
  check(t.claimTiles[0] == 720 && t.claimTiles[4] == 720, "claims: 720 tiles each (1280x720@1x)");
  r.colorInfo[0] = (12u << 16);
  t = E::computeDrawTargets(r, 1u, true);
  check(t.color[0].format() == 3, "fmt12 shares fmt3 storage");
  r.depthInfo = 0;
  t = E::computeDrawTargets(r, 1u, true);
  check(!t.depthBound, "same-base: depth loses to color");
  r.modeControl = 5;
  r.depthInfo = 0x5A0;
  t = E::computeDrawTargets(r, 1u, true);
  check(!t.colorWriteMask[0] && t.depthBound, "mode 5 = depth only");
  r.modeControl = 4;
  t = E::computeDrawTargets(r, 0u, true);
  check(!t.colorWriteMask[0], "PS without oC0 binds no color");
  r.depthControl = (7u << 4) | 0x2;  // z enable, no write, ALWAYS
  t = E::computeDrawTargets(r, 1u, true);
  check(!t.depthBound, "z ALWAYS without write normalizes depth off");
}

void testResolve() {
  E::Regs r;
  r.surfaceInfo = 1280;
  r.colorInfo[0] = (3u << 16);
  r.depthInfo = 0x102D0;
  r.copyControl = 0x00100000;  // convert, color 0, sample 0
  r.copyDestBase = 0x1DB9C000;
  r.copyDestPitch = 1280u | (720u << 16);
  r.copyDestInfo = 0x0000F001;
  r.windowScissorTL = 0x80000000u;
  r.windowScissorBR = 1280u | (720u << 16);
  r.screenScissorBR = 0x20002000u;
  const float v[6] = {-0.5f, -0.5f, 1279.5f, -0.5f, 1279.5f, 719.5f};
  std::memcpy(r.resolveVerts, v, sizeof(v));
  r.resolveVertsValid = true;
  const E::ResolveInfo ri = E::computeResolve(r);
  check(ri.copy && ri.x0 == 0 && ri.y0 == 0 && ri.x1 == 1280 && ri.y1 == 720, "resolve#7 rect = 1280x720");
  check(ri.destFormat == 32 && ri.destEndian == 1 && ri.destPitch == 1280 && ri.destHeight == 736,
        "resolve#7 dest k_16_16_16_16_FLOAT, 8in16, 1280x736");
  check(ri.srcFormat == 3 && ri.srcKey.base() == 0 && !ri.isDepth, "resolve#7 source = fmt3 @0");
  E::Regs d = r;
  d.copyControl = 0x00000004;  // raw, depth
  const E::ResolveInfo rd = E::computeResolve(d);
  check(rd.isDepth && rd.raw && rd.destFormat == 23 && rd.srcBase == 0x2D0, "depth resolve -> k_24_8_FLOAT from 0x2D0");
  E::Regs sh = r;
  sh.copyDestInfo = 0x003B0C01;
  const E::ResolveInfo rs = E::computeResolve(sh);
  check(near(rs.expBiasFactor, 1.0f / 32.0f) && rs.destFormat == 24, "shadow resolve: k_16, exp bias -5");
}

void testResolveToGuest() {
  // 80-wide 1x 8888 surface at base 0; pixel (x,y) = (x, y, 128, 255).
  std::vector<uint8_t> mirror(E::kEdramBytes, 0);
  for (uint32_t y = 0; y < 8; y++)
    for (uint32_t x = 0; x < 8; x++) {
      const uint32_t v = x | (y << 8) | (128u << 16) | (255u << 24);
      std::memcpy(&mirror[E::sampleOffsetBytes(x, y, 0, 1, 0, false, 0, 0)], &v, 4);
    }
  E::Regs r;
  r.surfaceInfo = 80;
  r.colorInfo[0] = 0;
  r.copyControl = 0x00100000;
  r.copyDestPitch = 32u | (8u << 16);
  r.copyDestInfo = (6u << 7);  // k_8_8_8_8, no endian
  r.windowScissorTL = 0x80000000u;
  r.windowScissorBR = 80u | (8u << 16);
  r.screenScissorBR = 0x20002000u;
  const float v[6] = {-0.5f, -0.5f, 7.5f, -0.5f, 7.5f, 7.5f};
  std::memcpy(r.resolveVerts, v, sizeof(v));
  r.resolveVertsValid = true;
  E::ResolveInfo ri = E::computeResolve(r);
  std::vector<uint8_t> dest(32 * 32 * 4, 0xCD);
  const uint32_t n = E::resolveToGuest(mirror.data(), ri, dest.data(), dest.size());
  const int32_t o = sr::gfx::xenos_tiling::tiled2D(5, 3, 32, 2);
  check(n == 64 && dest[o] == 5 && dest[o + 1] == 3 && dest[o + 2] == 128 && dest[o + 3] == 255,
        "8888 convert lands at tiled (5,3), R in the low byte");
  r.copyDestInfo = (6u << 7) | 2u;  // 8in32
  ri = E::computeResolve(r);
  std::vector<uint8_t> dest2(32 * 32 * 4, 0xCD);
  E::resolveToGuest(mirror.data(), ri, dest2.data(), dest2.size());
  check(dest2[o] == 255 && dest2[o + 1] == 128 && dest2[o + 2] == 3 && dest2[o + 3] == 5, "8in32 reverses the dword");
  r.copyDestInfo = (6u << 7) | (1u << 24);  // swap R/B
  ri = E::computeResolve(r);
  std::vector<uint8_t> dest3(32 * 32 * 4, 0xCD);
  E::resolveToGuest(mirror.data(), ri, dest3.data(), dest3.size());
  check(dest3[o] == 128 && dest3[o + 2] == 5, "dest swap exchanges R and B");
  // 4x MSAA k0123 averages the four samples.
  std::vector<uint8_t> m4(E::kEdramBytes, 0);
  for (uint32_t y = 0; y < 8; y++)
    for (uint32_t x = 0; x < 8; x++)
      for (uint32_t s = 0; s < 4; s++) {
        const uint32_t val = (s * 60u) | (255u << 24);
        std::memcpy(&m4[E::sampleOffsetBytes(x, y, 0, 2, 2, false, 0, s)], &val, 4);
      }
  E::Regs r4 = r;
  r4.surfaceInfo = 80u | (2u << 16);
  r4.copyControl = 0x00100060;
  r4.copyDestInfo = (6u << 7);
  E::ResolveInfo r4i = E::computeResolve(r4);
  std::vector<uint8_t> dest4(32 * 32 * 4, 0xCD);
  E::resolveToGuest(m4.data(), r4i, dest4.data(), dest4.size());
  check(r4i.sampleSelect == 6 && dest4[o] == 90, "4x k0123 averages samples (0+60+120+180)/4 = 90");
}

}  // namespace

// P1 per-draw EDRAM oracle: decode one surface (RtKey) out of two whole-EDRAM
// dumps - Xenia's (trace-dump --trace_dump_edram_draws) and ours
// (SR_PM4_EDRAM_DUMP) - with the SAME decoder our resolves use, and report
// per-channel stats plus two BMPs on the reference's scale.
// Run: difftest.exe --edram-surface <xenia.bin> <ours.bin> <keyhex> <rows> <outprefix>
int runEdramSurface(int argc, char** argv) {
  if (argc < 7) {
    std::printf("usage: --edram-surface <xenia.bin> <ours.bin> <keyhex> <rows> <outprefix>\n");
    return 2;
  }
  auto load = [](const char* p, std::vector<uint8_t>& v) {
    FILE* f = std::fopen(p, "rb");
    if (!f) return false;
    v.resize(E::kEdramBytes);
    const size_t n = std::fread(v.data(), 1, v.size(), f);
    std::fclose(f);
    return n == v.size();
  };
  std::vector<uint8_t> xe, ours;
  if (!load(argv[2], xe) || !load(argv[3], ours)) {
    std::printf("cannot read both dumps\n");
    return 2;
  }
  const E::RtKey k{uint32_t(std::strtoul(argv[4], nullptr, 16))};
  const uint32_t w = k.width();
  const uint32_t h = std::min<uint32_t>(k.height(), uint32_t(std::strtoul(argv[5], nullptr, 10)));
  std::printf("key %08X base=%u pitchTiles=%u msaa=%u depth=%d fmt=%u -> %ux%u\n", k.v, k.base(),
              k.pitchTiles(), k.msaa(), k.isDepth() ? 1 : 0, k.format(), w, h);
  std::vector<float> fx(size_t(w) * h * 4), fo(size_t(w) * h * 4);
  auto decode = [&](const std::vector<uint8_t>& m, std::vector<float>& out) {
    for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++) {
        const uint32_t off = E::sampleOffsetBytes(x, y, k.base(), k.pitchTiles(), k.msaa(),
                                                  k.isDepth(), k.intsLog2(), 0);
        uint32_t lo, hi = 0;
        std::memcpy(&lo, m.data() + off, 4);
        if (k.intsLog2()) std::memcpy(&hi, m.data() + (off + 4) % E::kEdramBytes, 4);
        float* c = &out[(size_t(y) * w + x) * 4];
        if (k.isDepth()) {
          c[0] = E::unpackDepth(k.format(), lo);
          c[1] = float(lo & 0xFFu);
          c[2] = 0.0f;
          c[3] = 1.0f;
        } else {
          E::unpackColor(k.format(), uint64_t(lo) | (uint64_t(hi) << 32), c);
        }
      }
  };
  decode(xe, fx);
  decode(ours, fo);
  double sx[4] = {}, so[4] = {}, mad[4] = {};
  float mx = 0.0f;
  uint32_t differ = 0;
  for (size_t i = 0; i < size_t(w) * h; i++) {
    bool d = false;
    for (int c = 0; c < 4; c++) {
      sx[c] += fx[i * 4 + c];
      so[c] += fo[i * 4 + c];
      mad[c] += std::fabs(double(fx[i * 4 + c]) - fo[i * 4 + c]);
      d = d || fx[i * 4 + c] != fo[i * 4 + c];
      if (c < 3) mx = std::max(mx, std::fabs(fx[i * 4 + c]));
    }
    differ += d ? 1u : 0u;
  }
  const double n = double(w) * h;
  std::printf("differ %.1f%%\n", 100.0 * differ / n);
  std::printf("xenia mean (%.4f %.4f %.4f %.4f)\n", sx[0] / n, sx[1] / n, sx[2] / n, sx[3] / n);
  std::printf("ours  mean (%.4f %.4f %.4f %.4f)\n", so[0] / n, so[1] / n, so[2] / n, so[3] / n);
  std::printf("MAD        (%.4f %.4f %.4f %.4f)\n", mad[0] / n, mad[1] / n, mad[2] / n, mad[3] / n);
  const float s = mx > 1.0f ? 1.0f / mx : 1.0f;
  auto bmp = [&](const std::vector<float>& f, const char* tag) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; i++) {
      px[i * 4 + 0] = uint8_t(E::packUNorm(std::fabs(f[i * 4 + 2]) * s, 255.f));
      px[i * 4 + 1] = uint8_t(E::packUNorm(std::fabs(f[i * 4 + 1]) * s, 255.f));
      px[i * 4 + 2] = uint8_t(E::packUNorm(std::fabs(f[i * 4 + 0]) * s, 255.f));
      px[i * 4 + 3] = 255;
    }
    const std::string path = std::string(argv[6]) + "_" + tag + ".bmp";
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return;
    const uint32_t img = w * h * 4, file = 54 + img, off = 54, dib = 40, pb = 1u | (32u << 16);
    uint8_t hdr[54] = {'B', 'M'};
    const int32_t sw = int32_t(w), sh = -int32_t(h);
    std::memcpy(hdr + 2, &file, 4);
    std::memcpy(hdr + 10, &off, 4);
    std::memcpy(hdr + 14, &dib, 4);
    std::memcpy(hdr + 18, &sw, 4);
    std::memcpy(hdr + 22, &sh, 4);
    std::memcpy(hdr + 26, &pb, 4);
    std::memcpy(hdr + 34, &img, 4);
    std::fwrite(hdr, 1, 54, fp);
    std::fwrite(px.data(), 1, px.size(), fp);
    std::fclose(fp);
  };
  bmp(fx, "xenia");
  bmp(fo, "ours");
  std::printf("images: |value| scaled by 1/%.3f\n", mx > 1.0f ? mx : 1.0f);
  return 0;
}

// step 6 R6 CHECK (Astra round 1 P2): sameRegs is the CHECK's Regs compare - every field, made stale, must show BAD.
void testSameRegs() {
  E::Regs a;
  uint32_t v = 0x1000;
  auto u = [&]() { return v += 0x11; };
  a.surfaceInfo = u();
  for (auto& x : a.colorInfo) x = u();
  a.depthInfo = u(); a.colorMask = u(); a.depthControl = u(); a.stencilRefMask = u(); a.stencilRefMaskBF = u();
  a.modeControl = u(); a.windowOffset = u(); a.windowScissorTL = u(); a.windowScissorBR = u();
  a.screenScissorTL = u(); a.screenScissorBR = u(); a.suScModeCntl = u(); a.clipCntl = u(); a.vteCntl = u();
  a.vtxCntl = u();
  for (int i = 0; i < 3; i++) { a.vpScale[i] = 0.5f + i; a.vpOffset[i] = 100.0f + i; }
  a.copyControl = u(); a.copyDestBase = u(); a.copyDestPitch = u(); a.copyDestInfo = u();
  a.depthClear = u(); a.colorClear = u(); a.colorClearLo = u();
  for (int i = 0; i < 6; i++) a.resolveVerts[i] = 10.0f * i;
  a.resolveVertsValid = true;
  const E::Regs b = a;
  check(E::sameRegs(a, a) && E::sameRegs(a, b), "sameRegs: a snapshot equals itself and its copy");
  using M = std::pair<const char*, std::function<void(E::Regs&)>>;
  std::vector<M> m = {
      {"surfaceInfo", [](E::Regs& r) { r.surfaceInfo ^= 1; }},
      {"depthInfo", [](E::Regs& r) { r.depthInfo ^= 1; }},
      {"colorMask", [](E::Regs& r) { r.colorMask ^= 1; }},
      {"depthControl", [](E::Regs& r) { r.depthControl ^= 1; }},
      {"stencilRefMask", [](E::Regs& r) { r.stencilRefMask ^= 1; }},
      {"stencilRefMaskBF", [](E::Regs& r) { r.stencilRefMaskBF ^= 1; }},
      {"modeControl", [](E::Regs& r) { r.modeControl ^= 1; }},
      {"windowOffset", [](E::Regs& r) { r.windowOffset ^= 1; }},
      {"windowScissorTL", [](E::Regs& r) { r.windowScissorTL ^= 1; }},
      {"windowScissorBR", [](E::Regs& r) { r.windowScissorBR ^= 1; }},
      {"screenScissorTL", [](E::Regs& r) { r.screenScissorTL ^= 1; }},
      {"screenScissorBR", [](E::Regs& r) { r.screenScissorBR ^= 1; }},
      {"suScModeCntl", [](E::Regs& r) { r.suScModeCntl ^= 1; }},
      {"clipCntl", [](E::Regs& r) { r.clipCntl ^= 1; }},
      {"vteCntl", [](E::Regs& r) { r.vteCntl ^= 1; }},
      {"vtxCntl", [](E::Regs& r) { r.vtxCntl ^= 1; }},
      {"copyControl", [](E::Regs& r) { r.copyControl ^= 1; }},
      {"copyDestBase", [](E::Regs& r) { r.copyDestBase ^= 1; }},
      {"copyDestPitch", [](E::Regs& r) { r.copyDestPitch ^= 1; }},
      {"copyDestInfo", [](E::Regs& r) { r.copyDestInfo ^= 1; }},
      {"depthClear", [](E::Regs& r) { r.depthClear ^= 1; }},
      {"colorClear", [](E::Regs& r) { r.colorClear ^= 1; }},
      {"colorClearLo", [](E::Regs& r) { r.colorClearLo ^= 1; }},
      {"resolveVertsValid", [](E::Regs& r) { r.resolveVertsValid = !r.resolveVertsValid; }},
  };
  for (int i = 0; i < 4; i++) m.push_back({"colorInfo[i]", [i](E::Regs& r) { r.colorInfo[i] ^= 1; }});
  for (int i = 0; i < 3; i++) {
    m.push_back({"vpScale[i]", [i](E::Regs& r) { r.vpScale[i] = std::nextafter(r.vpScale[i], 1e9f); }});
    m.push_back({"vpOffset[i]", [i](E::Regs& r) { r.vpOffset[i] = std::nextafter(r.vpOffset[i], 1e9f); }});
  }
  for (int i = 0; i < 6; i++)
    m.push_back({"resolveVerts[i]", [i](E::Regs& r) { r.resolveVerts[i] = std::nextafter(r.resolveVerts[i], 1e9f); }});
  int caught = 0;
  for (const auto& x : m) {
    E::Regs s = a;
    x.second(s);
    if (!E::sameRegs(a, s)) caught++;
    else std::printf("  stale field NOT caught: %s\n", x.first);
  }
  check(m.size() == 40 && caught == int(m.size()), "sameRegs: every one of the 40 fields, made stale, shows BAD");
  E::Regs z = a, nz = a;
  z.vpOffset[0] = 0.0f;
  nz.vpOffset[0] = -0.0f;
  check(!E::sameRegs(z, nz), "sameRegs: +0 and -0 differ (bitwise floats)");
  E::Regs n1 = a, n2 = a;
  n1.vpScale[1] = n2.vpScale[1] = std::nanf("");
  check(E::sameRegs(n1, n2), "sameRegs: the same NaN bits compare equal (bitwise floats)");
}

// step 6 R6 fix round 2 (Astra round 2 P2b): every DrawTargets field, made stale, shows BAD in the new sameTargets;
// the former compare (kept here verbatim) was blind to fullRect, rectExact, clearValid, clearStencil and clearColor.
bool formerSameTargets(const E::DrawTargets& a, const E::DrawTargets& b) {
  if (a.rasterize != b.rasterize || a.depthBound != b.depthBound || a.zEnable != b.zEnable || a.zWrite != b.zWrite ||
      a.stencilEnable != b.stencilEnable || a.zFunc != b.zFunc || a.fullWrite != b.fullWrite)
    return false;
  if (std::memcmp(a.colorWriteMask, b.colorWriteMask, sizeof(a.colorWriteMask)) != 0) return false;
  if (std::memcmp(a.claimTiles, b.claimTiles, sizeof(a.claimTiles)) != 0) return false;
  for (int i = 0; i < 4; i++)
    if (std::memcmp(&a.color[i], &b.color[i], sizeof(a.color[i])) != 0) return false;
  return std::memcmp(&a.depth, &b.depth, sizeof(a.depth)) == 0;
}
void testSameTargets() {
  E::DrawTargets a;
  a.rasterize = true;
  for (int i = 0; i < 4; i++) { a.color[i] = E::RtKey(0x100u + i); a.colorWriteMask[i] = uint8_t(0xF - i); }
  a.depth = E::RtKey(0x200u);
  a.depthBound = a.zEnable = a.zWrite = a.stencilEnable = true;
  a.zFunc = 3;
  for (int i = 0; i < 5; i++) a.claimTiles[i] = 40u + i;
  a.fullWrite = 0x11;
  for (int i = 0; i < 4; i++) { a.fullRect[i] = 7 * (i + 1); a.clearColor[i] = 0.25f * (i + 1); }
  a.rectExact = 1; a.clearValid = 0x11; a.clearStencil = 0x80;
  const E::DrawTargets b = a;
  check(E::sameTargets(a, a) && E::sameTargets(a, b), "sameTargets: a target set equals itself and its copy");
  using M = std::pair<const char*, std::function<void(E::DrawTargets&)>>;
  std::vector<M> m = {
      {"rasterize", [](E::DrawTargets& t) { t.rasterize = !t.rasterize; }},
      {"depth", [](E::DrawTargets& t) { t.depth = E::RtKey(t.depth.v ^ 1u); }},
      {"depthBound", [](E::DrawTargets& t) { t.depthBound = !t.depthBound; }},
      {"zEnable", [](E::DrawTargets& t) { t.zEnable = !t.zEnable; }},
      {"zWrite", [](E::DrawTargets& t) { t.zWrite = !t.zWrite; }},
      {"stencilEnable", [](E::DrawTargets& t) { t.stencilEnable = !t.stencilEnable; }},
      {"zFunc", [](E::DrawTargets& t) { t.zFunc ^= 1; }},
      {"fullWrite", [](E::DrawTargets& t) { t.fullWrite ^= 1; }},
      {"rectExact", [](E::DrawTargets& t) { t.rectExact ^= 1; }},
      {"clearValid", [](E::DrawTargets& t) { t.clearValid ^= 1; }},
      {"clearStencil", [](E::DrawTargets& t) { t.clearStencil ^= 1; }},
  };
  for (int i = 0; i < 4; i++) {
    m.push_back({"color[i]", [i](E::DrawTargets& t) { t.color[i] = E::RtKey(t.color[i].v ^ 1u); }});
    m.push_back({"colorWriteMask[i]", [i](E::DrawTargets& t) { t.colorWriteMask[i] ^= 1; }});
    m.push_back({"fullRect[i]", [i](E::DrawTargets& t) { t.fullRect[i] ^= 1; }});
    m.push_back({"clearColor[i]", [i](E::DrawTargets& t) { t.clearColor[i] = std::nextafter(t.clearColor[i], 1e9f); }});
  }
  for (int i = 0; i < 5; i++) m.push_back({"claimTiles[i]", [i](E::DrawTargets& t) { t.claimTiles[i] ^= 1; }});
  int caught = 0, formerBlind = 0;
  for (const auto& x : m) {
    E::DrawTargets s = a;
    x.second(s);
    if (!E::sameTargets(a, s)) caught++;
    else std::printf("  stale field NOT caught: %s\n", x.first);
    if (formerSameTargets(a, s)) formerBlind++;
  }
  check(m.size() == 32 && caught == int(m.size()), "sameTargets: every one of the 32 fields, made stale, shows BAD");
  // fullRect x4, rectExact, clearValid, clearStencil, clearColor x4: the 11 the former compare could not see
  check(formerBlind == 11, "sameTargets: the former compare was blind to exactly the 11 clear / rect fields (Astra r2 P2b)");
  E::DrawTargets z = a, nz = a;
  z.clearColor[2] = 0.0f;
  nz.clearColor[2] = -0.0f;
  check(!E::sameTargets(z, nz), "sameTargets: +0 and -0 differ (bitwise floats)");
}

// step 6 R6 fix round 2 (Astra round 2 P1 / P2a): the drain reuse's adjacency and its Regs refresh.
void testDrainReuse() {
  namespace R = sr::gfx::drainreuse;
  // adjacency: an entry stored by command s is reusable by command s + 1 only
  R::Seq q;
  q.drainEntry();
  q.command();
  const uint64_t stored = q.v;  // the drain's first command stores
  q.command();
  check(R::adjacent(stored, q.v), "drain reuse: the immediately next command of the same drain may reuse");
  q.command();
  check(!R::adjacent(stored, q.v), "drain reuse: an intervening command makes the entry ineligible");
  // the last command of drain A stores; drain B's first command must not reuse it - also when B follows an
  // early-returning drain (its entry bump only) and whatever any drain counter says
  R::Seq s;
  s.drainEntry(); s.command(); s.command();
  const uint64_t lastOfA = s.v;
  s.drainEntry(); s.command();
  check(!R::adjacent(lastOfA, s.v), "drain reuse: the first command of the next drain does not reuse the previous drain's entry");
  R::Seq e;
  e.drainEntry(); e.command();
  const uint64_t lastOfA2 = e.v;
  e.drainEntry();  // an early-returning drain (empty / skip-empty / map failure): the entry bump only
  e.drainEntry(); e.command();
  check(!R::adjacent(lastOfA2, e.v), "drain reuse: an early-return drain followed by a matching first command gives no reuse");
  // refresh: two commands equal in every keyed field, different in every unkeyed copy / clear / resolve field
  sr::gfx::GuestDrawCommand c1{}, c2{};
  c1.surfaceInfo = c2.surfaceInfo = 0x1234; c1.colorInfo = c2.colorInfo = 0x55; c1.colorMask = c2.colorMask = 0xF;
  c1.copyControl = 1; c2.copyControl = 2;
  c1.copyDestBase = 0x1000; c2.copyDestBase = 0x2000;
  c1.copyDestPitch = 3; c2.copyDestPitch = 4;
  c1.copyDestInfo = 5; c2.copyDestInfo = 6;
  c1.depthClear = 7; c2.depthClear = 8;
  c1.colorClear = 9; c2.colorClear = 10;
  c1.colorClearLo = 11; c2.colorClearLo = 12;
  for (int i = 0; i < 6; i++) { c1.resolveVerts[i] = float(i); c2.resolveVerts[i] = float(i) + 0.5f; }
  c1.resolveVertsValid = false; c2.resolveVertsValid = true;
  const E::Regs fresh = R::regsFromCommand(c2);
  E::Regs reused = R::regsFromCommand(c1);  // the entry the previous command stored
  check(!E::sameRegs(fresh, reused), "drain reuse: a skipped refresh leaves stale copy / clear / resolve fields (BAD)");
  R::refreshNonKey(reused, c2);
  check(E::sameRegs(fresh, reused), "drain reuse: the refresh makes the reused Regs equal a fresh regsFromCommand");
  // fix rounds 3 + 4 (Astra r3 P2, r4 P2): only a COMPLETED prepare is cached - the stand-in for an admission /
  // acquisition harness (a missing alpha-test variant fails a prepare, a pipeline acquisition can fail; either can
  // succeed on the next identical-key command, which must then prepare afresh rather than reuse the failure)
  check(R::cacheable(true, true, false, false), "drain reuse: a completed prepare with no mark is cached");
  check(R::cacheable(true, true, true, true), "drain reuse: a completed prepare with its mark is cached");
  check(!R::cacheable(false, true, false, false), "drain reuse: a failed prepare is never cached");
  check(!R::cacheable(false, false, false, false), "drain reuse: a failed prepare without a PSO is never cached");
  check(!R::cacheable(true, false, false, false), "drain reuse: a prepare whose main PSO acquisition failed is never cached");
  check(!R::cacheable(true, true, true, false), "drain reuse: a prepare whose required mark PSO acquisition failed is never cached");
  check(!R::cacheable(true, false, true, true), "drain reuse: a main-PSO failure is not cached even with a mark");
  // fail-then-recover: the failed acquisition empties the entry; the recovered prepare on the next command is cached
  bool cached = R::cacheable(true, true, true, false);
  check(!cached, "drain reuse: the failed mark acquisition leaves no entry");
  cached = R::cacheable(true, true, true, true);
  check(cached, "drain reuse: the recovered prepare is cached afresh");
  // Astra r5 P3: all 16 eligibility combinations ...
  int agree = 0;
  for (int m = 0; m < 16; m++) {
    const bool p = (m & 1) != 0, ps = (m & 2) != 0, mw = (m & 4) != 0, mk = (m & 8) != 0;
    agree += R::cacheable(p, ps, mw, mk) == (p && ps && (!mw || mk)) ? 1 : 0;
  }
  check(agree == 16, "drain reuse: cacheable() is prepared && pso && (!markWanted || mark) in all 16 combinations");
  // ... and a fail-then-recover per acquisition through a model of the drain's store rule (d3d12_backend.cpp's miss
  // path: store when cacheable, else empty the entry - n = 0, vpValid = false; a hit needs adjacency and n == rn)
  struct Entry { uint32_t n = 0; bool vpValid = false; uint64_t seq = 0; };
  auto store = [](Entry& e, bool ok, uint32_t rn, uint64_t seq) {
    if (ok) { e.n = rn; e.seq = seq; } else { e.n = 0; }
    e.vpValid = false;
  };
  auto hits = [](const Entry& e, uint32_t rn, uint64_t now) { return R::adjacent(e.seq, now) && e.n == rn; };
  for (int which = 0; which < 2; which++) {  // 0: the main PSO acquisition fails; 1: the required mark PSO's fails
    const bool markWanted = which == 1;
    Entry e;
    e.n = 42; e.seq = 5; e.vpValid = true;  // a valid entry stored by command 5
    store(e, R::cacheable(true, which != 0, markWanted, false), 42, 6);  // command 6: the acquisition fails
    check(e.n == 0 && !e.vpValid, which ? "drain reuse: a failed mark acquisition empties the entry"
                                        : "drain reuse: a failed main-PSO acquisition empties the entry");
    check(!hits(e, 42, 7), which ? "drain reuse: after a failed mark acquisition the next identical draw prepares afresh"
                                 : "drain reuse: after a failed main-PSO acquisition the next identical draw prepares afresh");
    store(e, R::cacheable(true, true, markWanted, true), 42, 7);  // command 7: the acquisition recovers
    check(e.n == 42 && e.seq == 7 && !e.vpValid, which ? "drain reuse: the recovered prepare (mark) is stored"
                                                       : "drain reuse: the recovered prepare (main PSO) is stored");
    check(hits(e, 42, 8), which ? "drain reuse: the next identical draw reuses the recovered prepare (mark)"
                                : "drain reuse: the next identical draw reuses the recovered prepare (main PSO)");
  }
}

int runEdramTest() {
  std::printf("edram (gfx/edram.hpp) known-answer tests\n");
  testKeys();
  testFloats();
  testColorPack();
  testAddressing();
  testOwnership();
  testRects();
  testViewport();
  testWindowOffset();
  testDrawTargets();
  testResolve();
  testResolveToGuest();
  testSameRegs();
  testSameTargets();
  testDrainReuse();
  std::printf("EDRAM TESTS: total=%d passed=%d failed=%d\n", g_pass + g_fail, g_pass, g_fail);
  return g_fail;
}
