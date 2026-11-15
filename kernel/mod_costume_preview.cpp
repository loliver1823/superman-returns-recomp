#include "mod_costume_preview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace sr::kernel {
namespace {
constexpr uint32_t kTagTexture = 0x264923D3u;  // nested Apt container: image texture

uint64_t le(const uint8_t* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
  return v;
}

uint32_t fnv1a32Lower(const std::string& s) {
  uint32_t h = 0x811C9DC5u;
  for (unsigned char c : s) {
    h ^= static_cast<uint8_t>(std::tolower(c));
    h *= 0x01000193u;
  }
  return h;
}

std::string hex8(uint32_t v) {
  char b[9];
  std::snprintf(b, sizeof b, "%08x", v);
  return b;
}

uint16_t to565(int r, int g, int b) {
  return static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}
void from565(uint16_t c, int* r, int* g, int* b) {
  *r = (c >> 11 & 31) * 255 / 31;
  *g = (c >> 5 & 63) * 255 / 63;
  *b = (c & 31) * 255 / 31;
}

void encodeBlock(const uint8_t px[16][4], uint8_t out[16]) {
  // alpha: 8-value interpolation between the block's extremes (a0 > a1 selects it)
  int amin = 255, amax = 0;
  for (int i = 0; i < 16; ++i) {
    amin = std::min<int>(amin, px[i][3]);
    amax = std::max<int>(amax, px[i][3]);
  }
  uint64_t abits = 0;
  if (amax == amin) {
    out[0] = out[1] = static_cast<uint8_t>(amax);
  } else {
    out[0] = static_cast<uint8_t>(amax);
    out[1] = static_cast<uint8_t>(amin);
    int pal[8] = {amax, amin};
    for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * amax + i * amin) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0, bd = 1 << 30;
      for (int k = 0; k < 8; ++k) {
        const int d = std::abs(pal[k] - px[i][3]);
        if (d < bd) { bd = d; best = k; }
      }
      abits |= uint64_t(best) << (3 * i);
    }
  }
  for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>(abits >> (8 * i));

  // colour: inset bounding box of the visible texels (fully transparent ones do not count)
  int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}, seen = 0;
  for (int i = 0; i < 16; ++i) {
    if (px[i][3] < 8) continue;
    ++seen;
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min<int>(lo[c], px[i][c]);
      hi[c] = std::max<int>(hi[c], px[i][c]);
    }
  }
  if (!seen) for (int c = 0; c < 3; ++c) lo[c] = hi[c] = 0;
  for (int c = 0; c < 3; ++c) {
    const int inset = (hi[c] - lo[c]) / 16;
    lo[c] += inset;
    hi[c] -= inset;
  }
  uint16_t c0 = to565(hi[0], hi[1], hi[2]), c1 = to565(lo[0], lo[1], lo[2]);
  if (c0 < c1) std::swap(c0, c1);
  int pal[4][3];
  from565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
  from565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
  for (int c = 0; c < 3; ++c) {
    pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
    pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
  }
  uint32_t cbits = 0;
  if (c0 != c1) {
    for (int i = 0; i < 16; ++i) {
      int best = 0, bd = 1 << 30;
      for (int k = 0; k < 4; ++k) {
        int d = 0;
        for (int c = 0; c < 3; ++c) d += (pal[k][c] - px[i][c]) * (pal[k][c] - px[i][c]);
        if (d < bd) { bd = d; best = k; }
      }
      cbits |= uint32_t(best) << (2 * i);
    }
  }
  out[8] = static_cast<uint8_t>(c0);
  out[9] = static_cast<uint8_t>(c0 >> 8);
  out[10] = static_cast<uint8_t>(c1);
  out[11] = static_cast<uint8_t>(c1 >> 8);
  for (int i = 0; i < 4; ++i) out[12 + i] = static_cast<uint8_t>(cbits >> (8 * i));
}

// coverage helpers for the procedural card (unit square coordinates)
bool inRoundRect(double x, double y, double x0, double y0, double x1, double y1, double r) {
  if (x < x0 || x > x1 || y < y0 || y > y1) return false;
  const double cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}
double diamond(double x, double y, double cx, double cy, double hw, double hh) {
  return std::abs(x - cx) / hw + std::abs(y - cy) / hh;  // <= 1 inside
}
}  // namespace

bool modReadDdsDxt5(const std::string& hostPath, int dim, std::vector<uint8_t>* blocks, std::string* err) {
  std::ifstream in(std::filesystem::u8path(hostPath), std::ios::binary);
  if (!in) { *err = "cannot open preview " + hostPath; return false; }
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const size_t need = static_cast<size_t>(dim) * static_cast<size_t>(dim);
  if (b.size() < 128 || std::memcmp(b.data(), "DDS ", 4) != 0 || le(b.data() + 4, 4) != 124) {
    *err = "preview is not a DDS file";
    return false;
  }
  const uint32_t h = static_cast<uint32_t>(le(b.data() + 12, 4)), w = static_cast<uint32_t>(le(b.data() + 16, 4));
  const uint32_t pfFlags = static_cast<uint32_t>(le(b.data() + 80, 4));
  if (!(pfFlags & 0x4) || std::memcmp(b.data() + 84, "DXT5", 4) != 0) {
    *err = "preview DDS must be DXT5 (tools/srmods/costume_preview.py makes one)";
    return false;
  }
  if (w != static_cast<uint32_t>(dim) || h != static_cast<uint32_t>(dim)) {
    *err = "preview DDS must be " + std::to_string(dim) + "x" + std::to_string(dim);
    return false;
  }
  if (b.size() < 128 + need) { *err = "preview DDS is truncated"; return false; }
  blocks->assign(b.begin() + 128, b.begin() + 128 + static_cast<ptrdiff_t>(need));
  return true;
}

void modEncodeDxt5(const uint8_t* rgba, int dim, std::vector<uint8_t>* blocks) {
  blocks->assign(static_cast<size_t>(dim) * static_cast<size_t>(dim), 0);
  uint8_t px[16][4];
  size_t o = 0;
  for (int by = 0; by < dim; by += 4)
    for (int bx = 0; bx < dim; bx += 4) {
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
          std::memcpy(px[y * 4 + x], rgba + (static_cast<size_t>(by + y) * dim + bx + x) * 4, 4);
      encodeBlock(px, blocks->data() + o);
      o += 16;
    }
}

void modCostumeFallbackRgba(int dim, std::vector<uint8_t>* rgba) {
  // A deep-blue card, a gold diamond emblem and a caption bar: our own generic art, drawn with
  // 4x4 supersampling so the 128 and 512 versions are clean at their native sizes.
  rgba->assign(static_cast<size_t>(dim) * static_cast<size_t>(dim) * 4, 0);
  const double edge = 1.5 / dim;
  for (int py = 0; py < dim; ++py)
    for (int px = 0; px < dim; ++px) {
      double acc[4] = {0, 0, 0, 0};
      for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) {
          const double x = (px + (sx + 0.5) / 4.0) / dim, y = (py + (sy + 0.5) / 4.0) / dim;
          double r = 0, g = 0, b = 0, a = 0;
          if (inRoundRect(x, y, 0.16, 0.05, 0.84, 0.95, 0.07)) {
            const double t = (y - 0.05) / 0.9;
            r = 22 - 14 * t; g = 48 - 30 * t; b = 120 - 70 * t; a = 240;
            if (!inRoundRect(x, y, 0.16 + 3 * edge, 0.05 + 3 * edge, 0.84 - 3 * edge, 0.95 - 3 * edge, 0.065)) {
              r = 150; g = 172; b = 214;  // border
            }
            const double d = diamond(x, y, 0.5, 0.42, 0.22, 0.20);
            if (d <= 1.0 && d >= 0.80) { r = 236; g = 194; b = 64; }      // gold outline
            else if (d < 0.80) {
              const double inner = diamond(x, y, 0.5, 0.42, 0.22, 0.20);
              const double glow = 1.0 - inner / 0.8;
              r += 40 * glow; g += 60 * glow; b += 90 * glow;
            }
            if (diamond(x, y, 0.5, 0.42, 0.09, 0.08) <= 1.0) { r = 236; g = 194; b = 64; }  // inner emblem
            if (inRoundRect(x, y, 0.28, 0.72, 0.72, 0.77, 0.02)) { r = 200; g = 210; b = 230; }      // caption
            if (inRoundRect(x, y, 0.35, 0.81, 0.65, 0.85, 0.017)) { r = 120; g = 140; b = 185; }
          }
          acc[0] += r * a; acc[1] += g * a; acc[2] += b * a; acc[3] += a;
        }
      uint8_t* p = rgba->data() + (static_cast<size_t>(py) * dim + px) * 4;
      if (acc[3] > 0) {
        for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(std::clamp(acc[c] / acc[3], 0.0, 255.0));
      }
      p[3] = static_cast<uint8_t>(std::clamp(acc[3] / 16.0, 0.0, 255.0));
    }
}

bool modComposeCostumePreviewMovie(const std::vector<uint8_t>& t, int dim, const std::vector<uint8_t>& blocks,
                                   uint32_t innerTag, std::vector<uint8_t>* out, std::string* err) {
  auto bad = [&](const char* why) { *err = why; return false; };
  const size_t need = static_cast<size_t>(dim) * static_cast<size_t>(dim);
  if (blocks.size() != need) return bad("preview payload has the wrong size");
  if (t.size() < 64 || std::memcmp(t.data(), "BGFA1.05", 8) != 0) return bad("preview template is not a BGFA1.05 movie");
  const uint64_t count = le(t.data() + 12, 4), toc = le(t.data() + 16, 4), tocLen = le(t.data() + 24, 4);
  const uint64_t ntags = le(t.data() + 40, 4), names = le(t.data() + 44, 4);
  const size_t mode = t[32], wa = t[33], wi = t[34], wo = t[35], ws = t[36], wu = t[37], shift = t[38];
  if (mode != 1 || wa > 8 || wi != 8 || !wo || wo > 8 || !ws || ws > 8 || wu > 8 || shift > 31 || names > 4096)
    return bad("preview template has an unexpected record layout");
  const uint64_t record = 1 + wa + wi + wo + ws + wu + names;
  if (toc > t.size() || tocLen > t.size() - toc || 4 * ntags > tocLen || count == 0 || count > 16 ||
      count > (tocLen - 4 * ntags) / record)
    return bad("preview template TOC out of range");
  *out = t;
  bool image = false;
  for (uint64_t i = 0; i < count; ++i) {
    uint8_t* r = out->data() + toc + ntags * 4 + i * record + 1;
    const uint64_t a = le(r, wa);
    const uint32_t tag = a < ntags ? static_cast<uint32_t>(le(t.data() + toc + 4 * a, 4)) : 0;
    uint8_t* id = r + wa;
    const uint64_t off = le(id + wi, wo) << shift, size = le(id + wi + wo, ws);
    if (off > t.size() || size > t.size() - off) return bad("preview template payload out of range");
    // id = <record index:4 LE><movie tag:4 LE>; keep the index, give the movie its own tag
    for (int k = 0; k < 4; ++k) id[4 + k] = static_cast<uint8_t>(innerTag >> (8 * k));
    if (tag != kTagTexture) continue;
    if (image || size != 128 + need) return bad("preview template image is not the expected DXT5 size");
    const uint8_t* hdr = t.data() + off;
    if ((hdr[0] << 8 | hdr[1]) != dim || (hdr[2] << 8 | hdr[3]) != dim) return bad("preview template image has other dimensions");
    uint8_t* dst = out->data() + off + 128;
    for (size_t k = 0; k + 1 < need; k += 2) {  // 8in16: the retail payload byte order
      dst[k] = blocks[k + 1];
      dst[k + 1] = blocks[k];
    }
    image = true;
  }
  if (!image) return bad("preview template has no image");
  return true;
}

std::string modCostumeThumbStem(const std::string& costumeId) { return "SRCostumeThumb_" + hex8(fnv1a32Lower(costumeId)); }
std::string modCostumeFullStem(const std::string& costumeId) { return "SRCostumeFull_" + hex8(fnv1a32Lower(costumeId)); }
std::string modCostumeThumbPath(const std::string& stem) { return "artassets/Costumes/thumbs/" + stem + ".swf"; }
std::string modCostumeFullPath(const std::string& stem) { return "artassets/Costumes/fullsize/" + stem + ".swf"; }

}  // namespace sr::kernel
