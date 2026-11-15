// CPU geometry proof against the pre-cut shader arithmetic. The companion
// source test binds the unchanged alias math to the production HLSL; this
// target itself does not execute a shader or prove rendered pixels.
#include "gfx/native_owner_rects.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace no = sr::gfx::native_owner;
namespace {
uint64_t checks = 0;
void require(bool ok, const char* what) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "[OWNER_RECTS_TEST] FAIL %s\n", what); std::exit(1); }
}
struct Input {
  uint32_t base, pitch, msaa, sx, sy;
  no::Rect domain;
  std::vector<no::Range> runs;
};
bool oldOwner(const Input& in, uint32_t x, uint32_t y) {
  const uint32_t sx = x << (in.msaa >= 2 ? 1 : 0);
  const uint32_t sy = y << (in.msaa >= 1 ? 1 : 0);
  const uint32_t tile = (in.base + (sy / (16 * in.sy)) * in.pitch + sx / (80 * in.sx)) & 2047;
  for (auto run : in.runs) if (tile >= run.begin && tile < run.end) return true;
  return false;
}
struct Pixel { uint32_t x, y; bool operator==(Pixel p) const { return x == p.x && y == p.y; } };
Pixel oldPixel(const Input& in, uint32_t x, uint32_t y, uint32_t ownerMsaa, bool depthSwap, bool is64) {
  uint32_t sx = x << (in.msaa >= 2 ? 1 : 0), sy = y << (in.msaa >= 1 ? 1 : 0);
  const uint32_t tw = (80 * in.sx) >> uint32_t(is64), th = 16 * in.sy;
  uint32_t lin = (sy / th) * in.pitch + sx / tw;
  if (lin >= 2048) {
    lin -= (lin / 2048) * 2048;
    sx = (lin % in.pitch) * tw + sx % tw;
    sy = (lin / in.pitch) * th + sy % th;
  }
  if (depthSwap) {
    const uint32_t half = tw >> 1;
    sx += sx % tw >= half ? uint32_t(-int32_t(half)) : half;
  }
  return {sx >> (ownerMsaa >= 2 ? 1 : 0), sy >> (ownerMsaa >= 1 ? 1 : 0)};
}
Pixel aliasCorrespondence(const Input& in, uint32_t x, uint32_t y,
                          uint32_t ownerMsaa, bool depthSwap, bool is64) {
  const uint32_t width = (is64 ? 40u : 80u) * in.sx, height = 16u * in.sy;
  const uint32_t sourceX = x * (in.msaa >= 2 ? 2u : 1u);
  const uint32_t sourceY = y * (in.msaa >= 1 ? 2u : 1u);
  uint32_t column = sourceX / width, row = sourceY / height;
  const uint32_t relative = row * in.pitch + column;
  if (relative >= 2048) {
    const uint32_t canonical = relative % 2048;
    column = canonical % in.pitch;
    row = canonical / in.pitch;
  }
  const uint32_t localX = sourceX % width;
  const uint32_t mappedX = depthSwap ? (localX + width / 2) % width : localX;
  return {(column * width + mappedX) / (ownerMsaa >= 2 ? 2u : 1u),
          (row * height + sourceY % height) / (ownerMsaa >= 1 ? 2u : 1u)};
}
void domains() {
  // Decode the pinned XeResolveGetInfo expressions, then enumerate every
  // NatRaw read made by each kernel's actual thread/pixel grouping.
  for (bool scaled : {false, true}) for (uint32_t sx = 1; sx <= 7; ++sx)
    for (uint32_t sy = 1; sy <= 7; ++sy) for (bool fill : {false, true}) {
      const uint32_t coord = 13u | (1u << 4) | (3u << 5) | (sx << 16) | (sy << 19);
      const uint32_t resolve = fill ? 1u << 29 : 0u;
      const uint32_t scaleX = scaled ? sx : 1u, scaleY = scaled ? sy : 1u;
      const uint32_t clampX = scaled && fill ? scaleX >> 1 : 0u;
      const uint32_t clampY = scaled && fill ? scaleY >> 1 : 0u;
      const uint32_t offsetX = ((coord >> 0) & 15u) * 8u * scaleX;
      const uint32_t offsetY = ((coord >> 4) & 1u) * 8u * scaleY;
      const uint32_t width = ((coord >> 5) & 2047u) * scaleX * 8u;
      const uint32_t foldX = 160u * scaleX, foldY = 48u * scaleY, groupsY = 2;
      no::Rect domain;
      require(no::dispatchDomain(coord, resolve, foldX, foldY, groupsY, scaled, domain), "packed domain accepted");
      require(domain.x0 == offsetX + foldX && domain.x1 == offsetX + foldX + width &&
              domain.y0 == offsetY + foldY + clampY &&
              domain.y1 == offsetY + foldY + groupsY * 8u, "decoded domain bounds");
      for (uint32_t kind = 0; kind < 7; ++kind) {
        const uint32_t lanes = kind == 0 || kind == 2 ? 8u : kind == 6 ? 2u : 4u;
        for (uint32_t y = 0; y < groupsY * 8u; ++y)
          for (uint32_t x = 0; x < width; x += lanes)
            for (uint32_t lane = 0; lane < lanes; ++lane) {
              const uint32_t pixelX = kind == 6 ? std::max(x, clampX) : x;
              const uint32_t readX = pixelX + lane + offsetX + foldX;
              const uint32_t readY = std::max(y, clampY) + offsetY + foldY;
              require(domain.contains(readX, readY), "every actual kernel read lies in domain");
            }
      }
    }
  no::Rect domain{1, 2, 3, 4};
  require(!no::dispatchDomain(0, 0, 0, 0, 1, false, domain), "zero encoded width refused");
  require(domain.x0 == 0 && domain.x1 == 0, "domain failure clears output");
  require(!no::dispatchDomain(1u << 5, 0, 0, 0, 1, true, domain), "zero encoded scale refused");
  require(!no::dispatchDomain(1u << 5, 0, 0, 0, 0, false, domain), "zero dispatch refused");
  require(!no::dispatchDomain(1u << 5, 0, UINT32_MAX, 0, 1, false, domain), "fold overflow refused");
  require(!no::dispatchDomain(1u << 5, 0, 0, 0, UINT32_MAX, false, domain), "dispatch overflow refused");
  require(no::dispatchDomain((2047u << 5) | (7u << 16) | (7u << 19), 1u << 29,
                            320u, 96u, 1u, true, domain), "largest packed width accepted");
  require(domain.x1 - domain.x0 == 2047u * 8u * 7u, "encoded width uses only eleven bits");
}
void exercise(const Input& in, std::mt19937& random) {
  no::Plan plan;
  require(no::build(in.base, in.pitch, in.msaa, in.sx, in.sy, in.runs.data(),
                   uint32_t(in.runs.size()), in.domain, plan), "plan accepted");
  auto point = [&](uint32_t x, uint32_t y) {
    const bool a = oldOwner(in, x, y), b = no::selected(plan, x, y);
    require(a == b, "owner matches original shader");
    for (bool is64 : {false, true}) for (bool depthSwap : {false, true})
      for (uint32_t ownerMsaa = 0; ownerMsaa < 4; ++ownerMsaa)
        require(oldPixel(in, x, y, ownerMsaa, depthSwap, is64) ==
                aliasCorrespondence(in, x, y, ownerMsaa, depthSwap, is64),
                "alias coordinates match independent grid correspondence");
  };
  const uint32_t cw = (80 * in.sx) >> (in.msaa >= 2 ? 1 : 0);
  const uint32_t rh = (16 * in.sy) >> (in.msaa >= 1 ? 1 : 0);
  for (uint32_t y = in.domain.y0; y < in.domain.y1; ++y) {
    if (y != in.domain.y0 && y + 1 != in.domain.y1 && y % rh != 0 && y % rh != rh - 1) continue;
    point(in.domain.x0, y); point(in.domain.x1 - 1, y);
    for (uint32_t x = in.domain.x0; x < in.domain.x1; ++x)
      if (x % cw == 0 || x % cw == cw - 1 || x % cw == cw / 2 || x % cw == cw / 2 - 1) point(x, y);
  }
  for (uint32_t i = 0; i < 100; ++i)
    point(in.domain.x0 + random() % (in.domain.x1 - in.domain.x0),
          in.domain.y0 + random() % (in.domain.y1 - in.domain.y0));
}
}

int main() {
  domains();
  std::mt19937 random(0xC1A11A5u);
  auto draw = [&](uint32_t bound) { return uint32_t(random() % bound); };
  // Row 102 crosses the absolute base and the relative ring. Include columns
  // beyond the RT width: rangeRects' host clipping must not discard them.
  exercise({1536, 20, 0, 1, 1, {0, 101 * 16, 21 * 80, 104 * 16}, {{1528, 1548}}}, random);
  exercise({2040, 20, 0, 1, 1, {18 * 80, 0, 24 * 80, 3 * 16}, {{0, 12}, {2044, 2048}}}, random);
  exercise({17, 7, 2, 3, 2, {2047 * 120, 48, 2051 * 120, 112}, {{0, 23}, {2000, 2048}}}, random);
  // Source fold and NatSrc's half-pixel row clamp: domain begins after the
  // clamp, not at zero or the destination rectangle's origin.
  for (uint32_t sx = 1; sx <= 3; ++sx) for (uint32_t sy = 1; sy <= 3; ++sy)
    for (uint32_t msaa = 0; msaa < 4; ++msaa) {
      const uint32_t foldX = 160 * sx, foldY = 48 * sy, fillY = sy / 2;
      exercise({2024, 20, msaa, sx, sy,
                {foldX, foldY + fillY, foldX + 128 * sx, foldY + 32 * sy},
                {{0, 16}, {2000, 2048}}}, random);
    }
  for (unsigned n = 0; n < 300; ++n) {
    const uint32_t x = draw(180000), y = draw(18000);
    Input in{draw(2048), 1 + draw(80), draw(4),
             1 + draw(3), 1 + draw(3), {x, y, x + 16 + draw(512), y + 16 + draw(128)}, {}};
    for (unsigned i = 0; i < 4; ++i) {
      const uint32_t begin = draw(2048);
      in.runs.push_back({begin, begin + 1 + draw(2048 - begin)});
    }
    exercise(in, random);
  }
  no::Plan plan;
  const no::Range run{0, 1};
  require(!no::build(0, 1, 0, 1, 1, &run, 1, {0, 0, 80, 16}, plan, 0), "capacity refuses instead of truncating");
  require(plan.count == 0, "failure publishes no partial plan");
  require(!no::build(0, 0, 0, 1, 1, &run, 1, {0, 0, 80, 16}, plan), "zero pitch refused");
  require(!no::build(0, 1, 0, 0, 1, &run, 1, {0, 0, 80, 16}, plan), "zero scale refused");
  const no::Range invalid{2047, 2049};
  require(!no::build(0, 1, 0, 1, 1, &invalid, 1, {0, 0, 80, 16}, plan), "invalid run refused");
  require(no::build(0, 1, 0, 1, 1, &run, 1, {0, 0, 80, 16}, plan), "control plan");
  require(no::selected(plan, 0, 0), "control detects omitted owner");
  no::Plan omitted;
  require(no::selected(plan, 0, 0) != no::selected(omitted, 0, 0), "omission mutant detected");
  require(!no::selected(plan, 80, 0) && !no::selected(plan, 0, 16), "half-open rectangle bounds");
  std::printf("[OWNER_RECTS_TEST] PASS checks=%llu\n", static_cast<unsigned long long>(checks));
  return 0;
}
