#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace sr::gfx::native_owner {

// Half-open rectangles in the coordinates passed to NatRaw, before the
// owner's sample-grid remap. No clipping to the host RT: out-of-bounds reads
// and coordinates beyond a wrapped alias row must retain their old owner.
struct Rect {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool contains(uint32_t x, uint32_t y) const {
    return x >= x0 && x < x1 && y >= y0 && y < y1;
  }
};
struct Range { uint32_t begin = 0, end = 0; };
constexpr uint32_t kMaxRects = 256;
struct Plan {
  uint32_t count = 0;
  uint32_t padding[3]{};  // HLSL cbuffer array starts at the next 16-byte row.
  std::array<Rect, kMaxRects> rects{};
};
static_assert(sizeof(Plan) == 16 + 16 * kMaxRects);

// A bounding source domain of every production NatSrc/NatRaw call: packed resolve
// constants, the actual dispatch group count, and the already-scaled fold.
// X invocations outside width return before NatRaw; Y has no such early-out.
// Kind 6 may skip the first few X samples for half-pixel fill; including those
// unused points is harmless and keeps the shared domain independent of kind.
inline bool dispatchDomain(uint32_t coordInfo, uint32_t resolveInfo,
                           uint32_t foldX, uint32_t foldY, uint32_t groupsY,
                           bool scaled, Rect& output) {
  output = {};
  const uint32_t sx = scaled ? (coordInfo >> 16) & 7u : 1u;
  const uint32_t sy = scaled ? (coordInfo >> 19) & 7u : 1u;
  const uint64_t width = uint64_t((coordInfo >> 5) & 2047u) * 8u * sx;
  if (!sx || !sy || !width || !groupsY) return false;
  const uint64_t x = uint64_t(coordInfo & 15u) * 8u * sx + foldX;
  const uint64_t y = uint64_t((coordInfo >> 4) & 1u) * 8u * sy + foldY;
  const uint64_t fill = scaled && (resolveInfo & (1u << 29)) ? sy / 2u : 0u;
  const uint64_t endY = y + std::max(uint64_t(groupsY) * 8u - 1u, fill) + 1u;
  if (x + width > uint32_t(std::numeric_limits<int32_t>::max()) / 2 ||
      endY > uint32_t(std::numeric_limits<int32_t>::max()) / 2) return false;
  output = {uint32_t(x), uint32_t(y + fill), uint32_t(x + width), uint32_t(endY)};
  return true;
}

// Convert absolute alias runs to source-pixel rectangles over the actual
// dispatch domain. Selection uses base+row*pitch+column modulo the alias ring;
// the later owner-pixel canonicalization deliberately remains a separate
// operation. A 64-bit source still selects with the original 80-sample column
// width: changing this to the source's storage width would change the shader.
// No span is truncated on capacity/invalid geometry. The caller must refuse
// the resolve before publishing GPU work when false is returned.
inline bool build(uint32_t base, uint32_t pitch, uint32_t sourceMsaa,
                  uint32_t scaleX, uint32_t scaleY, const Range* ranges,
                  uint32_t rangeCount, Rect domain, Plan& output,
                  uint32_t capacity = kMaxRects) {
  output = {};
  if (base >= 2048 || !pitch || pitch > 1023 || sourceMsaa > 3 ||
      !scaleX || scaleX > 7 || !scaleY || scaleY > 7 ||
      rangeCount > 4 || (rangeCount && !ranges) || capacity > kMaxRects ||
      domain.x0 >= domain.x1 || domain.y0 >= domain.y1 ||
      domain.x1 > uint32_t(std::numeric_limits<int32_t>::max()) / 2 ||
      domain.y1 > uint32_t(std::numeric_limits<int32_t>::max()) / 2)
    return false;
  for (uint32_t i = 0; i < rangeCount; ++i)
    if (ranges[i].begin >= ranges[i].end || ranges[i].end > 2048) return false;
  if (!rangeCount) return true;

  const uint32_t columnPixels = (80u * scaleX) >> (sourceMsaa >= 2 ? 1u : 0u);
  const uint32_t rowPixels = (16u * scaleY) >> (sourceMsaa >= 1 ? 1u : 0u);
  const uint32_t firstColumn = domain.x0 / columnPixels;
  const uint32_t lastColumn = (domain.x1 - 1) / columnPixels;
  const uint32_t firstRow = domain.y0 / rowPixels;
  const uint32_t lastRow = (domain.y1 - 1) / rowPixels;
  // Input is a bounded resolve dispatch, not arbitrary unbounded geometry.
  if (lastRow - firstRow > 16384 || lastColumn - firstColumn > 16384) return false;

  Plan result;
  std::vector<uint32_t> previous, current;
  std::vector<Range> spans, merged;
  for (uint32_t row = firstRow; row <= lastRow; ++row) {
    const uint32_t head = uint32_t((uint64_t(base) + uint64_t(row) * pitch) % 2048);
    const uint32_t y0 = std::max(domain.y0, row * rowPixels);
    const uint32_t y1 = std::min(domain.y1, (row + 1) * rowPixels);
    spans.clear();
    // Each run repeats along an unbounded source row every 2048 columns.
    // Include the previous cycle because its tail may cross column zero.
    const int64_t firstCycle = int64_t(firstColumn / 2048) - 1;
    const int64_t lastCycle = int64_t(lastColumn / 2048);
    for (uint32_t i = 0; i < rangeCount; ++i) {
      const int64_t start = (ranges[i].begin + 2048 - head) % 2048;
      const int64_t length = ranges[i].end - ranges[i].begin;
      for (int64_t cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        const int64_t a = std::max<int64_t>(firstColumn, start + cycle * 2048);
        const int64_t b = std::min<int64_t>(uint64_t(lastColumn) + 1, start + length + cycle * 2048);
        if (a < b) spans.push_back({uint32_t(a), uint32_t(b)});
      }
    }
    std::sort(spans.begin(), spans.end(), [](Range a, Range b) { return a.begin < b.begin; });
    merged.clear();
    for (Range span : spans) {
      if (!merged.empty() && span.begin <= merged.back().end)
        merged.back().end = std::max(merged.back().end, span.end);
      else
        merged.push_back(span);
    }
    current.clear();
    for (Range span : merged) {
      const uint32_t x0 = std::max(domain.x0, span.begin * columnPixels);
      const uint32_t x1 = std::min(domain.x1, span.end * columnPixels);
      uint32_t index = result.count;
      for (uint32_t prior : previous) {
        const Rect& r = result.rects[prior];
        if (r.x0 == x0 && r.x1 == x1 && r.y1 == y0) { index = prior; break; }
      }
      if (index == result.count) {
        if (result.count == capacity) return false;
        result.rects[result.count++] = {x0, y0, x1, y1};
      } else {
        result.rects[index].y1 = y1;
      }
      current.push_back(index);
    }
    previous.swap(current);
  }
  output = result;
  return true;
}

inline bool selected(const Plan& plan, uint32_t x, uint32_t y) {
  for (uint32_t i = 0; i < plan.count; ++i)
    if (plan.rects[i].contains(x, y)) return true;
  return false;
}

}  // namespace sr::gfx::native_owner
