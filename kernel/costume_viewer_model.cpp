// iter-465 viewer lane: COSTUME VIEWER model data (see costume_viewer_model.hpp).
// C++ port of the reading half of the costume lane's Python tools (rw4, sranim; the owner's asset
// tools, not published; formats measured there).
#include "costume_viewer_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>

namespace sr::kernel::cview {
namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
float bef(const uint8_t* p) { uint32_t v = be32(p); float f; std::memcpy(&f, &v, 4); return f; }
uint64_t le(const uint8_t* p, size_t n) { uint64_t v = 0; for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i); return v; }

// ── 4x4 helpers (row-major, column vectors) ───────────────────────────────────────────────
Mat4 ident() { Mat4 m{}; m[0] = m[5] = m[10] = m[15] = 1; return m; }
Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = s;
    }
  return r;
}
Mat4 transpose(const Mat4& m) { Mat4 r; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r[i * 4 + j] = m[j * 4 + i]; return r; }
bool invert(const Mat4& m, Mat4* out) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { a[r][c] = m[r * 4 + c]; a[r][4 + c] = r == c; }
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-12) return false;
    if (p != c) for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
    const double pv = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= pv;
    for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
  }
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) (*out)[r * 4 + c] = float(a[r][4 + c]);
  return true;
}
Mat4 fromTrs(const float* q, const float* t) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  Mat4 m = ident();
  m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
  m[4] = 2 * (x * y + z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z - x * w);
  m[8] = 2 * (x * z - y * w);     m[9] = 2 * (y * z + x * w);     m[10] = 1 - 2 * (x * x + y * y);
  m[3] = t[0]; m[7] = t[1]; m[11] = t[2];
  return m;
}
void toTrs(const Mat4& m, float* q, float* t) {
  t[0] = m[3]; t[1] = m[7]; t[2] = m[11];
  float c[3][3];
  for (int col = 0; col < 3; ++col) {
    const float s = std::sqrt(m[col] * m[col] + m[4 + col] * m[4 + col] + m[8 + col] * m[8 + col]);
    for (int r = 0; r < 3; ++r) c[r][col] = s > 0 ? m[r * 4 + col] / s : 0;
  }
  const float tr = c[0][0] + c[1][1] + c[2][2];
  if (tr > 0) {
    const float S = std::sqrt(tr + 1.0f) * 2;
    q[0] = (c[2][1] - c[1][2]) / S; q[1] = (c[0][2] - c[2][0]) / S; q[2] = (c[1][0] - c[0][1]) / S; q[3] = 0.25f * S;
  } else if (c[0][0] > c[1][1] && c[0][0] > c[2][2]) {
    const float S = std::sqrt(1.0f + c[0][0] - c[1][1] - c[2][2]) * 2;
    q[0] = 0.25f * S; q[1] = (c[0][1] + c[1][0]) / S; q[2] = (c[0][2] + c[2][0]) / S; q[3] = (c[2][1] - c[1][2]) / S;
  } else if (c[1][1] > c[2][2]) {
    const float S = std::sqrt(1.0f + c[1][1] - c[0][0] - c[2][2]) * 2;
    q[0] = (c[0][1] + c[1][0]) / S; q[1] = 0.25f * S; q[2] = (c[1][2] + c[2][1]) / S; q[3] = (c[0][2] - c[2][0]) / S;
  } else {
    const float S = std::sqrt(1.0f + c[2][2] - c[0][0] - c[1][1]) * 2;
    q[0] = (c[0][2] + c[2][0]) / S; q[1] = (c[1][2] + c[2][1]) / S; q[2] = 0.25f * S; q[3] = (c[1][0] - c[0][1]) / S;
  }
  const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  for (int i = 0; i < 4; ++i) q[i] = n > 0 ? q[i] / n : (i == 3);
}
void slerpLike(const float* a, const float* b, float t, float* o) {  // nlerp (short way)
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  const float s = d < 0 ? -1.0f : 1.0f;
  float n = 0;
  for (int i = 0; i < 4; ++i) { o[i] = a[i] * (1 - t) + s * b[i] * t; n += o[i] * o[i]; }
  n = std::sqrt(n);
  for (int i = 0; i < 4; ++i) o[i] = n > 0 ? o[i] / n : (i == 3);
}

// ── BGFA ───────────────────────────────────────────────────────────────────────────────────
struct BgfaHead { uint64_t count, toc, ntags, namelen; int wA, wId, wOff, wSize, wUnc, shift; };
bool bgfaHead(const uint8_t* h, size_t n, BgfaHead* o, std::string* err) {
  if (n < 0x40 || std::memcmp(h, "BGFA1.05", 8) != 0) { *err = "not a BGFA1.05 container"; return false; }
  o->count = le(h + 0x0C, 4); o->toc = le(h + 0x10, 4);
  o->wA = h[0x21]; o->wId = h[0x22]; o->wOff = h[0x23]; o->wSize = h[0x24]; o->wUnc = h[0x25]; o->shift = h[0x26];
  o->ntags = le(h + 0x28, 4); o->namelen = le(h + 0x2C, 4);
  if (o->wA > 8 || o->wId > 8 || o->wOff > 8 || o->wSize > 8 || o->wUnc > 8 || !o->wId || !o->wOff || !o->wSize ||
      o->shift > 31 || o->namelen > 4096 || o->count > 1000000) { *err = "BGFA header out of range"; return false; }
  return true;
}
struct BgfaRec { uint64_t id, off, size, unc; };
BgfaRec bgfaRec(const BgfaHead& h, const uint8_t* r) {
  const uint8_t* q = r + 1 + h.wA;
  BgfaRec o{};
  o.id = le(q, h.wId); q += h.wId;
  o.off = le(q, h.wOff) << h.shift; q += h.wOff;
  o.size = le(q, h.wSize); q += h.wSize;
  o.unc = h.wUnc ? le(q, h.wUnc) : o.size;
  return o;
}
bool maybeInflate(std::vector<uint8_t>* b) {
  if (b->size() >= 5 && (*b)[1] == 0xFB && ((*b)[0] & 0x3E) == 0x10) {
    std::vector<uint8_t> out;
    if (!refpackDecompress(b->data(), b->size(), &out)) return false;
    b->swap(out);
  }
  return true;
}
uint64_t wantId(const std::string& name) {
  if (!name.empty() && name[0] == '#') return std::strtoull(name.c_str() + 1, nullptr, 16);
  return fnv1a64Upper(name);
}

// ── RW4 ────────────────────────────────────────────────────────────────────────────────────
struct Entry { uint32_t off, size, type, abs; };
struct Arena {
  const std::vector<uint8_t>* d = nullptr;     // the arena, or (lazy) its header block
  uint64_t fullSize = 0;                       // the whole arena's size
  std::function<bool(uint64_t, size_t, uint8_t*)> read;   // lazy: reads the rest on demand
  mutable std::map<uint32_t, std::vector<uint8_t>> cache;
  uint32_t id = 0, count = 0, resBase = 0;
  std::vector<uint32_t> ext;
  std::vector<Entry> e;
  const uint8_t* blob(uint32_t i) const {
    const Entry& x = e[i];
    if (uint64_t(x.abs) + x.size <= d->size()) return d->data() + x.abs;
    auto it = cache.find(i);
    if (it == cache.end()) {
      std::vector<uint8_t> b(x.size, 0);
      if (read) read(x.abs, x.size, b.data());
      it = cache.emplace(i, std::move(b)).first;
    }
    return it->second.data();
  }
  bool ok(uint32_t i, uint32_t need) const { return i < e.size() && need <= e[i].size && uint64_t(e[i].abs) + need <= fullSize; }
};
bool parseArena(const std::vector<uint8_t>& d, Arena* a, std::string* err, uint64_t fullSize = 0) {
  static const uint8_t kMagic[8] = {0x89, 'R', 'W', '4', 'x', 'b', '2', 0};
  if (d.size() < 0xAC || std::memcmp(d.data(), kMagic, 8) != 0) { *err = "not an RW4 xb2 arena"; return false; }
  a->d = &d;
  a->fullSize = std::max<uint64_t>(fullSize, d.size());
  a->id = be32(&d[0x1C]);
  a->count = be32(&d[0x20]);
  const uint32_t dictOff = be32(&d[0x30]), man = be32(&d[0x34]);
  a->resBase = be32(&d[0x44]);
  if (uint64_t(dictOff) + 24ull * a->count > d.size() || man + 16 > d.size() || be32(&d[man]) != 0x10004) {
    *err = "RW4 dictionary out of range"; return false;
  }
  const uint32_t mc = be32(&d[man + 4]);
  for (uint32_t i = 0; i < mc && man + 12 + 4 * i + 4 <= d.size(); ++i) {
    const uint32_t so = man + be32(&d[man + 12 + 4 * i]);
    if (so + 12 > d.size()) continue;
    if (be32(&d[so]) == 0x10006) {
      const uint32_t ec = be32(&d[so + 4]);
      for (uint32_t k = 0; k < ec && so + 12 + 4 * k + 4 <= d.size(); ++k) a->ext.push_back(be32(&d[so + 12 + 4 * k]));
    }
  }
  a->e.resize(a->count);
  for (uint32_t i = 0; i < a->count; ++i) {
    const uint8_t* p = &d[dictOff + 24 * i];
    Entry& x = a->e[i];
    x.off = be32(p); x.size = be32(p + 8); x.type = be32(p + 20);
    x.abs = (x.type >= 0x10030 && x.type <= 0x1003F) ? x.off + a->resBase : x.off;
    if (uint64_t(x.abs) + x.size > a->fullSize) x.size = x.abs < a->fullSize ? uint32_t(a->fullSize - x.abs) : 0;
  }
  return true;
}

struct VElem { uint16_t offset; uint32_t type; uint8_t usage, uidx; };
struct VBuf { uint32_t stride = 0, count = 0; const uint8_t* data = nullptr; size_t dataSize = 0; std::vector<VElem> el; };

float s10(uint32_t v) { v &= 0x3FF; return (v & 0x200 ? int(v) - 0x400 : int(v)) / 511.0f; }
float half(uint16_t h) {
  const int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = m / 1024.0f * std::ldexp(1.0f, -14);
  else if (e == 31) v = 65504.0f;
  else v = (1 + m / 1024.0f) * std::ldexp(1.0f, e - 15);
  return s ? -v : v;
}
// Decodes one element into up to 4 floats (memory order); returns the component count.
int decodeElem(uint32_t t, const uint8_t* p, float* o) {
  switch (t) {
    case 0x2A23B9: for (int i = 0; i < 3; ++i) o[i] = bef(p + 4 * i); return 3;              // FLOAT3
    case 0x2C23A5: for (int i = 0; i < 2; ++i) o[i] = bef(p + 4 * i); return 2;              // FLOAT2
    case 0x1A23A6: for (int i = 0; i < 4; ++i) o[i] = bef(p + 4 * i); return 4;              // FLOAT4
    case 0x2C83A4: o[0] = bef(p); return 1;                                                  // FLOAT1
    case 0x2A2187: { const uint32_t v = be32(p); o[0] = s10(v); o[1] = s10(v >> 10); o[2] = s10(v >> 20); return 3; }  // DEC3N
    case 0x2C2159: for (int i = 0; i < 2; ++i) o[i] = std::max(int16_t(be16(p + 2 * i)) / 32767.0f, -1.0f); return 2;  // SHORT2N
    case 0x2C2359: for (int i = 0; i < 2; ++i) o[i] = int16_t(be16(p + 2 * i)); return 2;    // SHORT2
    case 0x1A2286: case 0x182886: for (int i = 0; i < 4; ++i) o[i] = p[i]; return 4;          // UBYTE4 / D3DCOLOR
    case 0x1A2086: for (int i = 0; i < 4; ++i) o[i] = p[i] / 255.0f; return 4;                 // UBYTE4N
    case 0x1A205A: for (int i = 0; i < 4; ++i) o[i] = be16(p + 2 * i) / 65535.0f; return 4;   // USHORT4N
    case 0x2C2059: for (int i = 0; i < 2; ++i) o[i] = be16(p + 2 * i) / 65535.0f; return 2;   // USHORT2N
    case 0x1A215A: for (int i = 0; i < 4; ++i) o[i] = std::max(int16_t(be16(p + 2 * i)) / 32767.0f, -1.0f); return 4;
    case 0x2C235F: for (int i = 0; i < 2; ++i) o[i] = half(be16(p + 2 * i)); return 2;        // FLOAT16_2
    case 0x1A2360: for (int i = 0; i < 4; ++i) o[i] = half(be16(p + 2 * i)); return 4;        // FLOAT16_4
    default: return 0;
  }
}
bool loadVb(const Arena& a, uint32_t idx, VBuf* vb) {
  if (!a.ok(idx, 0x2C)) return false;
  const uint8_t* b = a.blob(idx);
  const uint32_t dataIdx = be32(b + 0x18) >> 2;
  vb->stride = be32(b + 0x20) >> 16;
  vb->count = be32(b + 0x24);
  const uint32_t descIdx = be32(b + 0x28);
  if (!a.ok(descIdx, 16) || dataIdx >= a.e.size()) return false;
  const uint8_t* dsc = a.blob(descIdx);
  const int n = dsc[8];
  if (!a.ok(descIdx, 16 + 12 * n)) return false;
  for (int i = 0; i < n; ++i) {
    const uint8_t* q = dsc + 16 + 12 * i;
    vb->el.push_back({be16(q + 2), be32(q + 4), q[9], q[10]});
  }
  vb->data = a.blob(dataIdx);
  vb->dataSize = a.e[dataIdx].size;
  return uint64_t(vb->stride) * vb->count <= vb->dataSize && vb->stride > 0;
}
const VElem* findElem(const VBuf& vb, uint8_t usage) {
  for (const auto& e : vb.el) if (e.usage == usage && e.uidx == 0) return &e;
  return nullptr;
}

// Xenos Tiled2D (Xenia texture_address.h) for a raster's base level.
uint32_t tiled2d(uint32_t x, uint32_t y, uint32_t pitchAl, uint32_t log2) {
  const uint32_t outer = (((y >> 5) * (pitchAl >> 5)) + (x >> 5)) << 6;
  const uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
  const uint32_t oib = (outer | inner) << log2;
  const uint32_t bank = (y >> 4) & 1;
  const uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
  return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 15) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
void rgb565(uint16_t c, uint8_t* o) {
  const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  o[0] = uint8_t((r << 3) | (r >> 2)); o[1] = uint8_t((g << 2) | (g >> 4)); o[2] = uint8_t((b << 3) | (b >> 2));
}
// One 4x4 colour block (little-endian bytes) into rgba (stride W).
void dxtColour(const uint8_t* blk, uint8_t* out, int W, int H, int px, int py, bool dxt1) {
  const uint16_t c0 = uint16_t(blk[0] | (blk[1] << 8)), c1 = uint16_t(blk[2] | (blk[3] << 8));
  const uint32_t bits = uint32_t(blk[4]) | (uint32_t(blk[5]) << 8) | (uint32_t(blk[6]) << 16) | (uint32_t(blk[7]) << 24);
  uint8_t pal[4][4];
  rgb565(c0, pal[0]); rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || !dxt1) {
    for (int k = 0; k < 3; ++k) { pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3); }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) { pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
    pal[2][3] = 255; pal[3][3] = 0;
  }
  for (int i = 0; i < 16; ++i) {
    const int x = px + (i & 3), y = py + i / 4;
    if (x >= W || y >= H) continue;
    std::memcpy(out + (size_t(y) * W + x) * 4, pal[(bits >> (2 * i)) & 3], 4);
  }
}
void alpha5(const uint8_t* blk, uint8_t vals[16]) {
  const int a0 = blk[0], a1 = blk[1];
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
  int pal[8] = {a0, a1};
  if (a0 > a1) for (int i = 0; i < 6; ++i) pal[2 + i] = ((6 - i) * a0 + (i + 1) * a1) / 7;
  else { for (int i = 0; i < 4; ++i) pal[2 + i] = ((4 - i) * a0 + (i + 1) * a1) / 5; pal[6] = 0; pal[7] = 255; }
  for (int i = 0; i < 16; ++i) vals[i] = uint8_t(pal[(bits >> (3 * i)) & 7]);
}
// Linear little-endian blocks -> RGBA. fmt: 18 DXT1, 19 DXT2/3, 20 DXT4/5, 6 8888 (A8R8G8B8 LE).
bool decodeBlocks(int fmt, const uint8_t* lin, int bw, int bh, int W, int H, Texture* t) {
  t->w = W; t->h = H;
  t->rgba.assign(size_t(W) * H * 4, 255);
  if (fmt == 6) {
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
      const uint8_t* s = lin + (size_t(y) * bw + x) * 4;
      uint8_t* d = &t->rgba[(size_t(y) * W + x) * 4];
      d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
    }
    return true;
  }
  const int bpb = fmt == 18 ? 8 : 16;
  if (fmt != 18 && fmt != 19 && fmt != 20) return false;
  for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
    const uint8_t* blk = lin + (size_t(by) * bw + bx) * bpb;
    if (fmt == 18) { dxtColour(blk, t->rgba.data(), W, H, bx * 4, by * 4, true); continue; }
    dxtColour(blk + 8, t->rgba.data(), W, H, bx * 4, by * 4, false);
    uint8_t al[16];
    if (fmt == 19) { for (int i = 0; i < 16; ++i) al[i] = uint8_t(((blk[i / 2] >> ((i & 1) * 4)) & 15) * 17); }
    else alpha5(blk, al);
    for (int i = 0; i < 16; ++i) {
      const int x = bx * 4 + (i & 3), y = by * 4 + i / 4;
      if (x < W && y < H) t->rgba[(size_t(y) * W + x) * 4 + 3] = al[i];
    }
  }
  return true;
}
struct Raster { int format = 0, endian = 0, w = 0, h = 0, dim = 0, pitch = 0; bool tiled = false; uint32_t base = 0; };
bool parseRaster(const Arena& a, uint32_t idx, Raster* r) {
  if (!a.ok(idx, 0x34) || a.e[idx].type != 0x20003) return false;
  const uint8_t* b = a.blob(idx);
  uint32_t f[6];
  for (int i = 0; i < 6; ++i) f[i] = be32(b + 0x1C + 4 * i);
  r->tiled = (f[0] >> 31) != 0;
  r->pitch = int((f[0] >> 22) & 0x1FF) * 32;
  r->format = int(f[1] & 0x3F);
  r->endian = int((f[1] >> 6) & 3);
  r->base = f[1] >> 12;
  r->w = int(f[2] & 0x1FFF) + 1;
  r->h = int((f[2] >> 13) & 0x1FFF) + 1;
  r->dim = int((f[5] >> 9) & 3);
  return true;
}
bool decodeRaster(const Arena& a, const Raster& r, Texture* t) {
  int bw = 4, bpb = 0;
  switch (r.format) { case 18: bpb = 8; break; case 19: case 20: bpb = 16; break; case 6: bw = 1; bpb = 4; break; default: return false; }
  if (r.base >= a.e.size()) return false;
  const uint8_t* pix = a.blob(r.base);
  const size_t pixSize = a.e[r.base].size;
  const int blocksW = (r.w + bw - 1) / bw, blocksH = (r.h + bw - 1) / bw;
  const int pitchBlocks = std::max(r.pitch / bw, blocksW);
  const uint32_t pitchAl = uint32_t(pitchBlocks + 31) & ~31u;
  const uint32_t log2 = bpb == 4 ? 2 : bpb == 8 ? 3 : 4;
  std::vector<uint8_t> lin(size_t(blocksW) * blocksH * bpb);
  for (int y = 0; y < blocksH; ++y) for (int x = 0; x < blocksW; ++x) {
    const size_t src = r.tiled ? tiled2d(uint32_t(x), uint32_t(y), pitchAl, log2) : (size_t(y) * pitchBlocks + x) * bpb;
    if (src + bpb > pixSize) return false;
    std::memcpy(&lin[(size_t(y) * blocksW + x) * bpb], pix + src, bpb);
  }
  if (r.endian == 1) for (size_t i = 0; i + 1 < lin.size(); i += 2) std::swap(lin[i], lin[i + 1]);
  else if (r.endian == 2) for (size_t i = 0; i + 3 < lin.size(); i += 4) { std::swap(lin[i], lin[i + 3]); std::swap(lin[i + 1], lin[i + 2]); }
  else if (r.endian == 3) for (size_t i = 0; i + 3 < lin.size(); i += 4) { std::swap(lin[i], lin[i + 2]); std::swap(lin[i + 1], lin[i + 3]); }
  return decodeBlocks(r.format, lin.data(), blocksW, blocksH, r.w, r.h, t);
}

int lodOf(const std::string& name) {
  for (size_t i = 0; i + 3 < name.size(); ++i)
    if (name[i] == 'L' && (name[i + 1] == 'O' || name[i + 1] == '0') && name[i + 2] == 'D' && std::isdigit(uint8_t(name[i + 3])))
      return name[i + 3] - '0';
  return 0;
}

}  // namespace

int Skeleton::find(uint32_t h) const {
  for (int i = 0; i < n; ++i) if (hash[size_t(i)] == h) return i;
  return -1;
}

uint32_t fnv1_32(const std::string& s) {
  uint32_t h = 0x811C9DC5u;
  for (unsigned char c : s) { h *= 0x01000193u; h ^= c; }
  return h;
}
uint64_t fnv1a64Upper(const std::string& s) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : s) { if (c >= 'a' && c <= 'z') c = uint8_t(c - 32); h = (h ^ c) * 0x100000001B3ull; }
  return h;
}

bool refpackDecompress(const uint8_t* p, size_t n, std::vector<uint8_t>* out) {
  if (n < 5 || p[1] != 0xFB || (p[0] & 0x3E) != 0x10) return false;
  const size_t w = (p[0] & 0x80) ? 4 : 3;
  size_t i = 2 + ((p[0] & 1) ? w : 0);
  if (n < i + w) return false;
  uint64_t size = 0;
  for (size_t k = 0; k < w; ++k) size = (size << 8) | p[i + k];
  i += w;
  if (size > (512ull << 20)) return false;
  out->clear();
  out->reserve(size_t(size));
  auto lit = [&](size_t c) { if (i + c > n) return false; out->insert(out->end(), p + i, p + i + c); i += c; return true; };
  auto copy = [&](size_t off, size_t c) {
    if (off == 0 || off > out->size()) return false;
    const size_t s = out->size() - off;
    for (size_t k = 0; k < c; ++k) out->push_back((*out)[s + k]);
    return true;
  };
  while (i < n) {
    const uint8_t b0 = p[i];
    if (b0 < 0x80) {
      if (i + 2 > n) return false;
      const uint8_t b1 = p[i + 1]; i += 2;
      if (!lit(b0 & 3) || !copy(((b0 & 0x60u) << 3) + b1 + 1, ((b0 >> 2) & 7) + 3)) return false;
    } else if (b0 < 0xC0) {
      if (i + 3 > n) return false;
      const uint8_t b1 = p[i + 1], b2 = p[i + 2]; i += 3;
      if (!lit((b1 >> 6) & 3) || !copy(((b1 & 0x3Fu) << 8) + b2 + 1, (b0 & 0x3F) + 4)) return false;
    } else if (b0 < 0xE0) {
      if (i + 4 > n) return false;
      const uint8_t b1 = p[i + 1], b2 = p[i + 2], b3 = p[i + 3]; i += 4;
      if (!lit(b0 & 3) || !copy(((b0 & 0x10u) << 12) + (uint32_t(b1) << 8) + b2 + 1, ((b0 & 0x0Cu) << 6) + b3 + 5)) return false;
    } else if (b0 < 0xFC) {
      i += 1;
      if (!lit(((b0 & 0x1Fu) << 2) + 4)) return false;
    } else {
      i += 1;
      if (!lit(b0 & 3)) return false;
      break;
    }
  }
  return out->size() == size;
}

bool astReadFileEntry(const std::string& path, const std::string& name, std::vector<uint8_t>* out, std::string* err) {
#ifdef _WIN32
  std::FILE* f = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
  std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
  if (!f) { *err = "cannot open " + path; return false; }
  uint8_t h[0x40];
  BgfaHead hd{};
  bool ok = std::fread(h, 1, 0x40, f) == 0x40 && bgfaHead(h, 0x40, &hd, err);
  if (ok) {
    const size_t rec = 1u + hd.wA + hd.wId + hd.wOff + hd.wSize + hd.wUnc + hd.namelen;
    std::vector<uint8_t> toc(size_t(4 * hd.ntags + rec * hd.count));
    _fseeki64(f, int64_t(hd.toc), SEEK_SET);
    ok = std::fread(toc.data(), 1, toc.size(), f) == toc.size();
    if (!ok) *err = "TOC truncated";
    const uint64_t id = wantId(name);
    const uint64_t mask = hd.wId >= 8 ? ~0ull : ((1ull << (8 * hd.wId)) - 1);
    bool found = false;
    for (uint64_t i = 0; ok && i < hd.count; ++i) {
      const BgfaRec r = bgfaRec(hd, &toc[size_t(4 * hd.ntags + rec * i)]);
      if (r.id != (id & mask)) continue;
      out->resize(size_t(r.size));
      _fseeki64(f, int64_t(r.off), SEEK_SET);
      ok = std::fread(out->data(), 1, out->size(), f) == out->size();
      if (!ok) *err = "entry truncated";
      found = true;
      break;
    }
    if (ok && !found) { ok = false; *err = name + " not in " + path; }
  }
  std::fclose(f);
  if (ok && !maybeInflate(out)) { *err = "RefPack stream invalid"; ok = false; }
  return ok;
}

bool astLocateNested(const std::string& path, const std::string& top, const std::string& nested, uint64_t* offset,
                     uint64_t* size, std::string* err) {
#ifdef _WIN32
  std::FILE* f = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
  std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
  if (!f) { *err = "cannot open " + path; return false; }
  auto find = [&](uint64_t base, const std::string& name, BgfaRec* out) {
    uint8_t h[0x40];
    BgfaHead hd{};
    _fseeki64(f, int64_t(base), SEEK_SET);
    if (std::fread(h, 1, 0x40, f) != 0x40 || !bgfaHead(h, 0x40, &hd, err)) return false;
    const size_t rec = 1u + hd.wA + hd.wId + hd.wOff + hd.wSize + hd.wUnc + hd.namelen;
    std::vector<uint8_t> toc(size_t(4 * hd.ntags + rec * hd.count));
    _fseeki64(f, int64_t(base + hd.toc), SEEK_SET);
    if (std::fread(toc.data(), 1, toc.size(), f) != toc.size()) { *err = "TOC truncated"; return false; }
    const uint64_t id = wantId(name), mask = hd.wId >= 8 ? ~0ull : ((1ull << (8 * hd.wId)) - 1);
    for (uint64_t i = 0; i < hd.count; ++i) {
      const BgfaRec r = bgfaRec(hd, &toc[size_t(4 * hd.ntags + rec * i)]);
      if (r.id != (id & mask)) continue;
      if (r.unc != r.size) { *err = name + " is compressed"; return false; }
      *out = r;
      return true;
    }
    *err = name + " missing";
    return false;
  };
  BgfaRec t{}, n{};
  const bool ok = find(0, top, &t) && find(t.off, nested, &n);
  std::fclose(f);
  if (!ok) return false;
  *offset = t.off + n.off;
  *size = n.size;
  return true;
}

bool openLazyArena(const std::string& path, const std::string& top, const std::string& nested, LazyArena* out,
                   std::string* err) {
  uint64_t off = 0, size = 0;
  if (!astLocateNested(path, top, nested, &off, &size, err)) return false;
  auto reader = [path, off, size](uint64_t at, size_t n, uint8_t* dst) {
    if (at > size || n > size - at) return false;
#ifdef _WIN32
    std::FILE* f = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
    std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    _fseeki64(f, int64_t(off + at), SEEK_SET);
    const bool ok = std::fread(dst, 1, n, f) == n;
    std::fclose(f);
    return ok;
  };
  uint8_t h[0x48];
  if (size < 0xAC || !reader(0, sizeof h, h)) { *err = nested + ": unreadable"; return false; }
  const uint32_t resBase = be32(h + 0x44);
  if (resBase < 0xAC || resBase > size || resBase > (64u << 20)) { *err = nested + ": header block size"; return false; }
  out->head.resize(resBase);
  if (!reader(0, resBase, out->head.data())) { *err = nested + ": header block unreadable"; return false; }
  out->size = size;
  out->read = reader;
  return true;
}

bool astForEachNested(const std::vector<uint8_t>& b, const std::function<bool(const std::vector<uint8_t>&)>& fn,
                      std::string* err) {
  BgfaHead hd{};
  if (!bgfaHead(b.data(), b.size(), &hd, err)) return false;
  const size_t rec = 1u + hd.wA + hd.wId + hd.wOff + hd.wSize + hd.wUnc + hd.namelen;
  if (hd.toc + 4 * hd.ntags + rec * hd.count > b.size()) { *err = "nested TOC out of range"; return false; }
  for (uint64_t i = 0; i < hd.count; ++i) {
    const BgfaRec r = bgfaRec(hd, &b[size_t(hd.toc + 4 * hd.ntags + rec * i)]);
    if (r.off > b.size() || r.size > b.size() - r.off) continue;
    std::vector<uint8_t> payload(b.begin() + ptrdiff_t(r.off), b.begin() + ptrdiff_t(r.off + r.size));
    if (!maybeInflate(&payload)) continue;
    if (!fn(payload)) break;
  }
  return true;
}

bool astReadNested(const std::vector<uint8_t>& b, const std::string& name, std::vector<uint8_t>* out, std::string* err) {
  BgfaHead hd{};
  if (!bgfaHead(b.data(), b.size(), &hd, err)) return false;
  const size_t rec = 1u + hd.wA + hd.wId + hd.wOff + hd.wSize + hd.wUnc + hd.namelen;
  if (hd.toc + 4 * hd.ntags + rec * hd.count > b.size()) { *err = "nested TOC out of range"; return false; }
  const uint64_t id = wantId(name);
  const uint64_t mask = hd.wId >= 8 ? ~0ull : ((1ull << (8 * hd.wId)) - 1);
  for (uint64_t i = 0; i < hd.count; ++i) {
    const BgfaRec r = bgfaRec(hd, &b[size_t(hd.toc + 4 * hd.ntags + rec * i)]);
    if (r.id != (id & mask)) continue;
    if (r.off > b.size() || r.size > b.size() - r.off) { *err = "nested payload out of range"; return false; }
    out->assign(b.begin() + ptrdiff_t(r.off), b.begin() + ptrdiff_t(r.off + r.size));
    if (!maybeInflate(out)) { *err = "RefPack stream invalid"; return false; }
    return true;
  }
  *err = name + " missing";
  return false;
}

// ── the body ──────────────────────────────────────────────────────────────────────────────
bool loadBody(const std::vector<std::vector<uint8_t>>& arenaBytes, Model* model, std::string* err,
              const std::vector<LazyArena>& lazy) {
  if (arenaBytes.empty()) { *err = "no arena"; return false; }
  std::vector<Arena> arenas(arenaBytes.size() + lazy.size());
  for (size_t i = 0; i < arenaBytes.size(); ++i)
    if (!parseArena(arenaBytes[i], &arenas[i], err)) { if (i == 0) return false; arenas[i].d = nullptr; }
  for (size_t i = 0; i < lazy.size(); ++i) {
    Arena& la = arenas[arenaBytes.size() + i];
    std::string e2;
    if (!parseArena(lazy[i].head, &la, &e2, lazy[i].size)) { la.d = nullptr; continue; }
    la.read = lazy[i].read;
  }
  const Arena& a = arenas[0];
  auto resolve = [&](uint32_t v, const Arena** ar, uint32_t* idx) {
    const uint32_t k = v >> 22, i = v & 0x3FFFFF;
    if (k == 0) { *ar = &a; *idx = i; return i < a.e.size(); }
    if (k < 3 || k >= a.ext.size()) return false;
    for (const auto& o : arenas) if (o.d && o.id == a.ext[k]) { *ar = &o; *idx = i; return i < o.e.size(); }
    return false;
  };
  // skeleton + inverse binds (the first SkinsInK)
  int sik = -1;
  for (uint32_t i = 0; i < a.count; ++i) if (a.e[i].type == 0x7000C) { sik = int(i); break; }
  if (sik < 0 || !a.ok(uint32_t(sik), 0x14)) { *err = "no SkinsInK"; return false; }
  const uint32_t skIdx = be32(a.blob(uint32_t(sik)) + 12), askIdx = be32(a.blob(uint32_t(sik)) + 16);
  if (!a.ok(skIdx, 20) || !a.ok(askIdx, 8)) { *err = "skeleton refs out of range"; return false; }
  const std::vector<uint8_t>& d = *a.d;
  {
    const uint8_t* b = a.blob(skIdx);
    const uint32_t pf = be32(b), pp = be32(b + 4), ph = be32(b + 8), n = be32(b + 12);
    (void)pf;
    if (n == 0 || n > 512 || pp + 4ull * n > d.size() || ph + 4ull * n > d.size()) { *err = "skeleton out of range"; return false; }
    Skeleton& s = model->skel;
    s.n = int(n);
    s.parent.resize(n); s.hash.resize(n); s.invBind.resize(n); s.bindLocal.resize(n);
    for (uint32_t i = 0; i < n; ++i) { s.parent[i] = int32_t(be32(&d[pp + 4 * i])); s.hash[i] = be32(&d[ph + 4 * i]); }
    const uint8_t* ab = a.blob(askIdx);
    const uint32_t p = be32(ab), an = be32(ab + 4);
    if (an < n || p < a.e[askIdx].abs || p + 64ull * n > d.size()) { *err = "inverse binds out of range"; return false; }
    std::vector<Mat4> world(n);
    for (uint32_t i = 0; i < n; ++i) {
      Mat4 rowv;
      for (int k = 0; k < 16; ++k) rowv[size_t(k)] = bef(&d[p + 64 * i + 4 * k]);
      rowv[3] = rowv[7] = rowv[11] = 0; rowv[15] = 1;
      s.invBind[i] = transpose(rowv);
      if (!invert(s.invBind[i], &world[i])) world[i] = ident();
    }
    for (uint32_t i = 0; i < n; ++i) {
      const int par = s.parent[i];
      if (par < 0 || par >= int(n)) { s.bindLocal[i] = world[i]; continue; }
      Mat4 inv;
      if (!invert(world[size_t(par)], &inv)) inv = ident();
      s.bindLocal[i] = mul(inv, world[i]);
    }
  }
  // COB mesh names (1:1 with the RenderObjects in dictionary order)
  std::vector<std::string> names;
  for (uint32_t i = 0; i < a.count && names.empty(); ++i) {
    if (a.e[i].type != 0x10002 || !a.ok(i, 12)) continue;
    const uint8_t* b = a.blob(i);
    if (std::memcmp(b, "COB", 3) != 0) continue;
    const uint32_t t0 = be32(b + 4), t1 = be32(b + 8), sz = a.e[i].size;
    for (uint32_t o = t0; o + 4 <= t1 && o + 4 <= sz; o += 4) {
      const uint32_t so = be32(b + o);
      std::string s;
      for (uint32_t k = so; k < sz && b[k]; ++k) s.push_back(char(b[k]));
      names.push_back(s);
    }
  }
  std::vector<uint32_t> robjs;
  for (uint32_t i = 0; i < a.count; ++i) if (a.e[i].type == 0x2000C) robjs.push_back(i);
  std::map<uint64_t, int> texCache;  // (arena ptr, idx) -> texture
  auto texture = [&](uint32_t ref) -> int {
    const Arena* ar = nullptr; uint32_t ri = 0;
    if (!resolve(ref, &ar, &ri)) return -1;
    const uint64_t key = (uint64_t(ar->id) << 32) | ri;
    auto it = texCache.find(key);
    if (it != texCache.end()) return it->second;
    Raster r; Texture t;
    int out = -1;
    if (parseRaster(*ar, ri, &r) && decodeRaster(*ar, r, &t)) { model->textures.push_back(std::move(t)); out = int(model->textures.size() - 1); }
    texCache[key] = out;
    return out;
  };
  auto rasterInfo = [&](uint32_t ref, Raster* r) {
    const Arena* ar = nullptr; uint32_t ri = 0;
    return resolve(ref, &ar, &ri) && parseRaster(*ar, ri, r);
  };
  for (size_t ro = 0; ro < robjs.size(); ++ro) {
    const std::string mname = names.size() == robjs.size() ? names[ro] : "ro" + std::to_string(robjs[ro]);
    if (lodOf(mname) != 0) continue;
    const uint32_t ri = robjs[ro];
    const uint8_t* b = a.blob(ri);
    const uint32_t sz = a.e[ri].size;
    if (sz < 0x80) continue;
    const uint32_t nblocks = be32(b + 4);
    std::map<int, int> palette;
    std::vector<uint32_t> bound;
    uint32_t o = 0x80;
    for (uint32_t bi = 0; bi < nblocks; ++bi) {
      if (o + 0x20 > sz) break;
      uint32_t h[8];
      for (int k = 0; k < 8; ++k) h[k] = be32(b + o + 4 * k);
      const uint32_t draw = o + h[7];
      if (draw + 24 > sz) break;
      uint32_t dr[6];
      for (int k = 0; k < 6; ++k) dr[k] = be32(b + draw + 4 * k);
      uint32_t p = o + 0x20 + ((h[0] & 0x40000000u) ? 0x40 : 0);
      bool hasSkinHdr = false;
      while (p + 12 <= sz) {
        const uint32_t k = be32(b + p), cls = k >> 16, f = be32(b + p + 4);
        if ((cls >= 1 && cls <= 4) && (f == 0x200 || f == 0x400)) { if (cls == 4) hasSkinHdr = true; p += 12; }
        else break;
      }
      uint32_t palTable = 0x00400000u;
      if (hasSkinHdr && p + 16 <= sz) {
        const uint32_t tsize = be32(b + p + 4), tidx = be32(b + p + 8);
        if (tidx != 0x00400000u && tsize != 0xFFFFFFFFu) palTable = tidx;
        p += 16;
      }
      // rasters: trailing dictionary refs before the VS patch region / the draw record
      const uint32_t end = o + (h[6] ? h[6] : h[7]);
      std::vector<uint32_t> rasters;
      for (uint32_t q = end - 4; q > p && q + 4 <= sz; q -= 4) {
        const uint32_t v = be32(b + q);
        Raster r;
        // an external reference into an arena we did not load still counts (export_gltf
        // is_raster): the scan must reach the resolvable rasters before it
        const uint32_t k = v >> 22;
        if (!rasterInfo(v, &r) && !(k >= 3 && k < a.ext.size() && (v & 0x3FFFFF) < 0x100000)) break;
        rasters.insert(rasters.begin(), v);
      }
      if (palTable != 0x00400000u && palTable < a.e.size()) {
        const uint8_t* pt = a.blob(palTable);
        for (uint32_t k = 0; k + 2 < a.e[palTable].size; k += 3)
          for (int c = 0; c < pt[k]; ++c) palette[pt[k + 1] + c] = pt[k + 2] + c;
      }
      if (bound.empty()) bound = rasters;
      else for (uint32_t rr : rasters) {
        Raster nr; if (!rasterInfo(rr, &nr)) continue;
        for (auto& cur : bound) {
          Raster cr; if (!rasterInfo(cur, &cr)) continue;
          if ((cr.format == 49) == (nr.format == 49) && cr.dim == nr.dim && cr.w >= 128) { cur = rr; break; }
        }
      }
      o = (draw + 24 + 15) & ~15u;
      VBuf vb;
      if (!loadVb(a, dr[5], &vb) || dr[4] >= a.e.size()) continue;
      const VElem *ePos = findElem(vb, 0), *eNrm = findElem(vb, 3), *eUv = findElem(vb, 5),
                  *eBi = findElem(vb, 2), *eBw = findElem(vb, 1);
      if (!ePos || !eUv) continue;   // depth / shadow proxies carry no TEXCOORD
      const uint8_t* ibb = a.blob(dr[4]);
      const uint32_t icount = be32(ibb + 0x20), ibData = be32(ibb + 0x18);
      if (ibData >= a.e.size() || 2ull * icount > a.e[ibData].size) continue;
      const uint8_t* ib = a.blob(ibData);
      const uint32_t start = dr[2], count = dr[3];
      if (uint64_t(start) + count > icount) continue;
      // diffuse: the first 2D raster >= 128 wide that is DXT1/3/5 or 8888 (export_gltf heuristic)
      int tex = -1;
      if (std::getenv("SR_COSTUME_VIEWER_DEBUG"))
        for (uint32_t rref : bound) {
          Raster r; const bool ok = rasterInfo(rref, &r);
          std::fprintf(stderr, "  [%s] raster ref %08X ok=%d fmt=%d %dx%d dim=%d\n", mname.c_str(), rref, ok, r.format, r.w, r.h, r.dim);
        }
      for (uint32_t rref : bound) {
        Raster r; if (!rasterInfo(rref, &r)) continue;
        if (r.dim != 1 || r.w < 128) continue;
        if (r.format == 18 || r.format == 19 || r.format == 20 || r.format == 6) { tex = texture(rref); break; }
      }
      Mesh m;
      m.name = mname;
      m.texture = tex;
      std::map<uint32_t, uint32_t> remap;
      auto vert = [&](uint32_t v) -> uint32_t {
        auto it = remap.find(v);
        if (it != remap.end()) return it->second;
        const uint32_t ni = uint32_t(remap.size());
        remap[v] = ni;
        const uint8_t* vp = vb.data + size_t(v) * vb.stride;
        float f[4] = {0, 0, 0, 0};
        decodeElem(ePos->type, vp + ePos->offset, f);
        m.pos.insert(m.pos.end(), {f[0], f[1], f[2]});
        float nn[4] = {0, 1, 0, 0};
        if (eNrm) decodeElem(eNrm->type, vp + eNrm->offset, nn);
        m.nrm.insert(m.nrm.end(), {nn[0], nn[1], nn[2]});
        float uv[4] = {0, 0, 0, 0};
        decodeElem(eUv->type, vp + eUv->offset, uv);
        m.uv.insert(m.uv.end(), {uv[0], uv[1] + 1.0f});   // stored v in [-1,0]; GPU WRAP
        float bi[4] = {0, 0, 0, 0}, bw[4] = {0, 0, 0, 0};
        if (eBi && eBw) { decodeElem(eBi->type, vp + eBi->offset, bi); decodeElem(eBw->type, vp + eBw->offset, bw); }
        else bw[0] = 1;
        float ws = 0;
        uint16_t js[4] = {0, 0, 0, 0};
        float wv[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
          const int reg = eBi ? int(bi[3 - k]) : 0;
          const float w = bw[k];
          if (w <= 0) continue;
          if (eBi && (reg % 3 || !palette.count(reg / 3))) continue;
          js[k] = uint16_t(eBi ? palette[reg / 3] : 0);
          wv[k] = w; ws += w;
        }
        for (int k = 0; k < 4; ++k) { m.joint.push_back(js[k]); m.weight.push_back(ws > 0 ? wv[k] / ws : (k == 0)); }
        return ni;
      };
      // strips with 0xFFFF restarts -> triangles (stored winding)
      std::vector<uint32_t> run;
      auto flush = [&]() {
        for (size_t k = 0; k + 2 < run.size(); ++k) {
          const uint32_t x = run[k], y = run[k + 1], z = run[k + 2];
          if (x == y || y == z || x == z) continue;
          if (std::max({x, y, z}) >= vb.count) continue;
          const uint32_t tri[3] = {k % 2 ? y : x, k % 2 ? x : y, z};
          for (uint32_t v : tri) m.idx.push_back(vert(v));
        }
        run.clear();
      };
      for (uint32_t k = 0; k < count; ++k) {
        const uint16_t v = be16(ib + 2 * (start + k));
        if (v == 0xFFFF) flush(); else run.push_back(v);
      }
      flush();
      if (!m.idx.empty()) model->meshes.push_back(std::move(m));
    }
  }
  if (model->meshes.empty()) { *err = "no LOD0 textured meshes"; return false; }
  return true;
}

// ── the cape (RSS) ─────────────────────────────────────────────────────────────────────────
bool decodeDds(const std::vector<uint8_t>& d, Texture* out, std::string* err) {
  if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0) { *err = "not a DDS"; return false; }
  auto u = [&](size_t o) { return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24); };
  const int H = int(u(12)), W = int(u(16));
  const uint32_t pfFlags = u(80), fourcc = u(84), bits = u(88);
  if (W <= 0 || H <= 0 || W > 8192 || H > 8192) { *err = "DDS size"; return false; }
  int fmt = 0;
  if (pfFlags & 4) {
    if (fourcc == 0x31545844u) fmt = 18; else if (fourcc == 0x33545844u || fourcc == 0x32545844u) fmt = 19;
    else if (fourcc == 0x35545844u || fourcc == 0x34545844u) fmt = 20;
  } else if (bits == 32) fmt = 6;
  if (!fmt) { *err = "DDS format unsupported"; return false; }
  const int bw = fmt == 6 ? W : (W + 3) / 4, bh = fmt == 6 ? H : (H + 3) / 4;
  const size_t need = size_t(bw) * bh * (fmt == 18 ? 8 : fmt == 6 ? 4 : 16);
  if (128 + need > d.size()) { *err = "DDS truncated"; return false; }
  return decodeBlocks(fmt, d.data() + 128, bw, bh, W, H, out) || (*err = "DDS decode", false);
}

bool loadCape(const std::vector<uint8_t>& d, const std::vector<uint8_t>& dds, Model* model, std::string* err) {
  static const uint8_t kHead[8] = {'R', 'S', 'S', 0, 1, 0, 0, 0};
  if (d.size() < 16 || std::memcmp(d.data(), kHead, 8) != 0) { *err = "not an RSS surface"; return false; }
  size_t cur = 8;
  struct Arr { uint32_t count = 0, width = 0; size_t at = 0; };
  auto arr = [&](uint32_t width, uint32_t elem, Arr* o) {
    if (cur + 8 > d.size()) return false;
    o->count = be32(&d[cur]); o->width = be32(&d[cur + 4]); cur += 8;
    if (o->width != width || o->count == 0 || cur + size_t(o->count) * width * elem > d.size()) return false;
    o->at = cur; cur += size_t(o->count) * width * elem;
    return true;
  };
  Arr pos, uv, idx, wt, jt, nm;
  if (!arr(4, 4, &pos) || !arr(2, 4, &uv) || !arr(1, 2, &idx) || !arr(4, 4, &wt) || !arr(4, 4, &jt)) { *err = "RSS arrays"; return false; }
  if (cur + 8 > d.size()) { *err = "RSS names"; return false; }
  nm.count = be32(&d[cur]); nm.width = be32(&d[cur + 4]); cur += 8;
  if (nm.width != 64 || cur + 64ull * nm.count > d.size()) { *err = "RSS names"; return false; }
  std::vector<int> jmap(nm.count, -1);
  for (uint32_t i = 0; i < nm.count; ++i) {
    std::string s;
    for (size_t k = cur + 64ull * i; k < cur + 64ull * (i + 1) && d[k]; ++k) s.push_back(char(d[k]));
    jmap[i] = model->skel.find(fnv1_32(s));
  }
  if (uv.count != pos.count || wt.count != pos.count || jt.count != pos.count) { *err = "RSS counts"; return false; }
  Mesh m;
  m.name = "cape";
  m.cape = true;
  for (uint32_t v = 0; v < pos.count; ++v) {
    for (int k = 0; k < 3; ++k) m.pos.push_back(bef(&d[pos.at + 16ull * v + 4 * k]));
    m.nrm.insert(m.nrm.end(), {0, 0, 1});
    m.uv.push_back(bef(&d[uv.at + 8ull * v]));
    m.uv.push_back(bef(&d[uv.at + 8ull * v + 4]));
    float ws = 0, w[4];
    uint16_t j[4];
    for (int k = 0; k < 4; ++k) {
      w[k] = bef(&d[wt.at + 16ull * v + 4 * k]);
      const int ji = int(bef(&d[jt.at + 16ull * v + 4 * k]));
      const int mj = ji >= 0 && ji < int(jmap.size()) ? jmap[size_t(ji)] : -1;
      if (mj < 0 || w[k] <= 0) { w[k] = 0; j[k] = 0; } else j[k] = uint16_t(mj);
      ws += w[k];
    }
    for (int k = 0; k < 4; ++k) { m.joint.push_back(j[k]); m.weight.push_back(ws > 0 ? w[k] / ws : (k == 0)); }
  }
  // four-index cloth faces (quads); a quad with a repeated index is a triangle
  for (uint32_t q = 0; q + 3 < idx.count; q += 4) {
    uint32_t v[4];
    for (int k = 0; k < 4; ++k) v[k] = be16(&d[idx.at + 2ull * (q + k)]);
    if (std::max({v[0], v[1], v[2], v[3]}) >= pos.count) continue;
    m.idx.insert(m.idx.end(), {v[0], v[1], v[2]});
    if (v[3] != v[2] && v[3] != v[0]) m.idx.insert(m.idx.end(), {v[0], v[2], v[3]});
  }
  // smooth normals from the faces (bind pose)
  std::fill(m.nrm.begin(), m.nrm.end(), 0.0f);
  for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
    const float* a = &m.pos[3ull * m.idx[t]]; const float* b = &m.pos[3ull * m.idx[t + 1]]; const float* c = &m.pos[3ull * m.idx[t + 2]];
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    for (int k = 0; k < 3; ++k) for (int c3 = 0; c3 < 3; ++c3) m.nrm[3ull * m.idx[t + k] + c3] += n[c3];
  }
  if (!dds.empty()) {
    Texture t;
    std::string e2;
    if (decodeDds(dds, &t, &e2)) { model->textures.push_back(std::move(t)); m.texture = int(model->textures.size() - 1); }
  }
  if (m.idx.empty()) { *err = "RSS has no faces"; return false; }
  // cape chain depth per joint (for the procedural sway): joints named *Cape_<k>
  model->capeDepth.assign(size_t(model->skel.n), 0);
  for (uint32_t i = 0; i < nm.count; ++i) {
    const int mj = jmap[i];
    if (mj < 0) continue;
    std::string s;
    for (size_t k = cur + 64ull * i; k < cur + 64ull * (i + 1) && d[k]; ++k) s.push_back(char(d[k]));
    const size_t us = s.rfind('_');
    if (s.find("Cape") != std::string::npos && us != std::string::npos && us + 1 < s.size() && std::isdigit(uint8_t(s[us + 1])))
      model->capeDepth[size_t(mj)] = uint8_t(1 + std::atoi(s.c_str() + us + 1));
  }
  model->meshes.push_back(std::move(m));
  return true;
}

// ── the clip ───────────────────────────────────────────────────────────────────────────────
namespace {
constexpr float kAng = 9.58738019107841e-05f;  // pi / 32768
constexpr float kTrn = 1.0f / 1024.0f;
void quatFromEuler(float A, float B, float C, float* q) {
  A *= 0.5f; B *= 0.5f; C *= 0.5f;
  const float sA = std::sin(A), cA = std::cos(A), sB = std::sin(B), cB = std::cos(B), sC = std::sin(C), cC = std::cos(C);
  q[0] = cA * sB * cC - sA * cB * sC;
  q[1] = sA * cB * cC + cA * sB * sC;
  q[2] = cA * cB * sC - sA * sB * cC;
  q[3] = cA * cB * cC + sA * sB * sC;
}
struct VarLen {
  const uint8_t* b; size_t p, n; int bit = 7;
  uint32_t bits(int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; ++i) {
      v = (v << 1) | (p < n ? ((b[p] >> bit) & 1u) : 0u);
      if (--bit < 0) { bit = 7; ++p; }
    }
    return v;
  }
  uint32_t read() { const uint32_t pre = bits(2); return pre == 0 ? bits(12) : pre == 1 ? bits(8) : pre == 2 ? bits(4) : 0; }
};
uint32_t bitsetMode(const uint8_t* b, size_t base, uint32_t idx) {
  const uint8_t* w = b + base + (idx >> 5) * 8;
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | w[i];
  return uint32_t(v >> (2 * (idx & 31))) & 3;
}
size_t nbits(uint32_t n) { return n == 0 ? 0 : size_t(((n - 1) >> 5) + 1) * 8; }
size_t al16(size_t x) { return (x + 15) & ~size_t(15); }
}  // namespace

bool loadClip(const std::vector<uint8_t>& cct, const std::vector<uint8_t>& bank, const std::string& clipName,
              Model* model, std::string* err) {
  const Skeleton& ms = model->skel;
  if (ms.n == 0) { *err = "load the body first"; return false; }
  // CCT: bones (hash per index), anim sets, bind (inverse, row-vector)
  if (cct.size() < 0x70) { *err = "CCT too small"; return false; }
  const uint8_t* c = cct.data();
  const uint32_t cn = be32(c + 0x60), pa = be32(c + 0x64), pm = be32(c + 0x68), ph = be32(c + 0x6C);
  if (cn == 0 || cn > 512 || pa + 16ull * cn > cct.size() || pm + 64ull * cn > cct.size() || ph + 20ull * cn > cct.size()) {
    *err = "CCT out of range"; return false;
  }
  std::vector<uint32_t> chash(cn);
  std::vector<int> cpar(cn);
  for (uint32_t i = 0; i < cn; ++i) {
    cpar[i] = int32_t(be32(c + pa + 16 * i));
    const uint32_t idx = be32(c + ph + 20 * i + 16);
    if (idx < cn) chash[idx] = be32(c + ph + 20 * i);
  }
  // CCT bind locals (fallback for bones outside the clip's anim set)
  std::vector<Mat4> cworld(cn), clocal(cn);
  for (uint32_t i = 0; i < cn; ++i) {
    Mat4 rowv;
    for (int k = 0; k < 16; ++k) rowv[size_t(k)] = bef(c + pm + 64 * i + 4 * k);
    Mat4 inv = transpose(rowv);
    if (!invert(inv, &cworld[i])) cworld[i] = ident();
  }
  for (uint32_t i = 0; i < cn; ++i) {
    const int p = cpar[i];
    Mat4 ip;
    clocal[i] = (p >= 0 && p < int(cn) && invert(cworld[size_t(p)], &ip)) ? mul(ip, cworld[i]) : cworld[i];
  }
  std::map<uint32_t, std::vector<uint32_t>> sets;
  {
    const uint32_t ns = be32(c + 0x20), ps = be32(c + 0x24);
    for (uint32_t k = 0; k < ns && ps + 8ull * k + 8 <= cct.size(); ++k) {
      const uint32_t sid = be32(c + ps + 8 * k), sp = be32(c + ps + 8 * k + 4);
      if (sp + 8 > cct.size()) continue;
      const uint32_t q = be32(c + sp), cnt = be32(c + sp + 4);
      if (q + (cnt + 7) / 8 > cct.size()) continue;
      std::vector<uint32_t> bones;
      for (uint32_t i = 0; i < cnt; ++i) if ((c[q + (i >> 3)] >> (i & 7)) & 1) bones.push_back(i);
      sets[sid] = bones;
    }
  }
  // bank: the clip by name -> its first leaf's block
  const uint8_t* b = bank.data();
  const size_t bn = bank.size();
  if (bn < 0x44 || be32(b + 4) != 9) { *err = "not an animation bank"; return false; }
  const uint32_t nclip = be32(b + 0x24), pc = be32(b + 0x28), nd = be32(b + 0x2C), pd = be32(b + 0x30);
  if (nclip > 100000 || nd > 100000 || pc + 4ull * nclip > bn || pd + 4ull * nd > bn) { *err = "bank tables"; return false; }
  uint32_t leaf = 0;
  for (uint32_t i = 0; i < nclip && !leaf; ++i) {
    const uint32_t cp = be32(b + pc + 4 * i);
    if (cp + 8 > bn) continue;
    const uint32_t np = be32(b + cp);
    std::string name;
    for (uint32_t k = np; k < bn && b[k]; ++k) name.push_back(char(b[k]));
    if (_stricmp(name.c_str(), clipName.c_str()) != 0) continue;
    std::vector<uint32_t> stack{be32(b + cp + 4)};
    int guard = 0;
    while (!stack.empty() && guard++ < 256 && !leaf) {
      const uint32_t n = stack.back(); stack.pop_back();
      if (!n || n + 16 > bn) continue;
      if (b[n] == 0x16) { leaf = n; break; }
      const int kids = std::max(1, int(b[n + 1]));
      for (int k = kids - 1; k >= 0; --k) if (n + 12 + 4 * k + 4 <= bn) stack.push_back(be32(b + n + 12 + 4 * k));
    }
  }
  if (!leaf) { *err = "clip " + clipName + " not in bank"; return false; }
  const uint32_t lp = be32(b + leaf + 4), blockOff = be32(b + leaf + 12);
  float spacing = 4.0f;
  if (lp && lp + 12 <= bn) { const float s = bef(b + lp + 8); if (std::isfinite(s) && s > 0) spacing = s; }
  if (blockOff + 0xB0 > bn || be32(b + blockOff + 4) != 8) { *err = "clip block invalid"; return false; }
  const uint32_t frames = be32(b + blockOff + 0x18), setId = be32(b + blockOff + 0x14);
  const size_t D = blockOff + 0x30;
  if (be32(b + D) != 1 || frames == 0 || frames > 20000) { *err = "clip codec unsupported"; return false; }
  auto h16 = [&](size_t o) { return be16(b + D + o); };
  auto h32 = [&](size_t o) { return be32(b + D + o); };
  const uint32_t szConst = h32(0x04), szRaw = h32(0x08), szStatic = h32(0x0C), szRotBase = h32(0x14);
  const uint32_t nCodes = h16(0x18), szCodes = h16(0x1A), szU16Base = h16(0x1C), szU8Base = h16(0x1E), szU16 = h16(0x20),
                 szU8 = h16(0x22), szOff = h16(0x26), streamQ = h16(0x28), nPack = h16(0x2A), nRaw = h16(0x30),
                 nU16 = h16(0x32), nU8 = h16(0x34), strideU16 = h16(0x36), strideU8 = h16(0x38), padBits = h16(0x3A);
  if (nCodes > 256) { *err = "clip header"; return false; }
  size_t p = D + 0x80;
  const size_t codes = p;
  p = al16(p + szCodes);
  const size_t bitsPack = p; p += nbits(nPack);
  const size_t bitsU16 = p; p += nbits(nU16);
  const size_t bitsU8 = p; p += nbits(nU8);
  p += padBits;
  const size_t constRot = p; p += szConst;
  const size_t rotBase = p; p += szRotBase;
  const size_t rawTrans = p; p += szRaw;
  const size_t u16Data = p; p += szU16;
  const size_t u8Data = p; p += szU8;
  const size_t staticT = p; p += szStatic;
  const size_t u16Base = p; p += szU16Base;
  const size_t u8Base = p; p += szU8Base;
  const size_t offTab = p; p += szOff;
  const size_t stream = p;
  const bool wide = (uint32_t(streamQ) << 2) > 0x10000;
  if (stream + streamQ * 4ull > bn || offTab + szOff > bn) { *err = "clip regions out of range"; return false; }
  std::vector<uint32_t> bones;
  auto it = sets.find(setId);
  if (it != sets.end()) bones = it->second; else for (uint32_t i = 0; i < cn; ++i) bones.push_back(i);
  if (bones.size() != nCodes) { *err = "clip channels != anim set"; return false; }
  // CCT bone -> model joint
  std::vector<int> toModel(cn, -1);
  for (uint32_t i = 0; i < cn; ++i) toModel[i] = ms.find(chash[i]);
  Clip& clip = model->clip;
  clip.frames = int(frames);
  clip.keySpacing = spacing;
  clip.trs.assign(size_t(frames) * ms.n * 7, 0.0f);
  clip.driven.assign(size_t(ms.n), 0);
  // defaults: model bind locals; CCT bind locals for the CCT bones (the clip's rest)
  std::vector<float> rest(size_t(ms.n) * 7);
  for (int j = 0; j < ms.n; ++j) toTrs(ms.bindLocal[size_t(j)], &rest[size_t(j) * 7], &rest[size_t(j) * 7 + 4]);
  for (uint32_t i = 0; i < cn; ++i) if (toModel[i] >= 0) toTrs(clocal[i], &rest[size_t(toModel[i]) * 7], &rest[size_t(toModel[i]) * 7 + 4]);
  for (uint32_t f = 0; f < frames; ++f) std::memcpy(&clip.trs[size_t(f) * ms.n * 7], rest.data(), rest.size() * 4);
  for (uint32_t f = 0; f < frames; ++f) {
    const uint32_t fo = wide ? be32(b + offTab + 4ull * f) : be16(b + offTab + 2ull * f);
    VarLen vl{b, stream + fo, bn};
    uint32_t iConst = 0, iPack = 0, iStatic = 0, iRaw = 0, iU16 = 0, iU8 = 0;
    size_t p16 = u16Data + size_t(f) * strideU16 * 2, p8 = u8Data + size_t(f) * strideU8, praw = rawTrans + size_t(f) * nRaw * 12;
    for (uint32_t ch = 0; ch < nCodes; ++ch) {
      const uint8_t cc = b[codes + ch];
      float q[4] = {0, 0, 0, 1}, t[3] = {0, 0, 0};
      bool hasT = true;
      if (cc & 1) {
        const size_t o = constRot + 6ull * iConst++;
        const float e0 = -float(be16(b + o)) * kAng, e1 = -float(be16(b + o + 2)) * kAng, e2 = -float(be16(b + o + 4)) * kAng;
        quatFromEuler(e1, e2, e0, q);
      } else if (cc & 2) {
        const uint32_t m = bitsetMode(b, bitsPack, iPack);
        const size_t o = rotBase + 6ull * iPack++;
        const uint32_t base[3] = {be16(b + o), be16(b + o + 2), be16(b + o + 4)};
        uint32_t val[3] = {0, 0, 0};
        bool anim[3] = {false, false, false};
        if (m == 0) { for (int k = 0; k < 3; ++k) { val[k] = vl.read(); anim[k] = true; } }
        else if (m == 1) { val[0] = vl.read(); anim[0] = true; }
        else if (m == 2) { val[2] = vl.read(); anim[2] = true; }
        else { val[1] = vl.read(); anim[1] = true; }
        float e[3];
        for (int k = 0; k < 3; ++k)
          e[k] = anim[k] ? float((val[k] << 4) & 0xFFFF0u) * kAng - float(base[k]) * kAng : -float(base[k]) * kAng;
        quatFromEuler(e[1], e[2], e[0], q);
      }
      if ((cc & 0x1C) == 0) {
        if (cc & 0x20) hasT = false;   // the bank's translation table: keep the rest translation
        else { for (int k = 0; k < 3; ++k) t[k] = bef(b + staticT + 12ull * iStatic + 4 * k); ++iStatic; }
      } else if (cc & 0x10) {
        const uint32_t m = bitsetMode(b, bitsU8, iU8);
        for (int k = 0; k < 3; ++k) t[k] = bef(b + u8Base + 12ull * iU8 + 4 * k);
        if (m == 0) { for (int k = 0; k < 3; ++k) t[k] += float(b[p8 + k]) * kTrn; p8 += 3; }
        else { const int k = m == 1 ? 0 : m == 2 ? 1 : 2; t[k] += float(b[p8]) * kTrn; p8 += 1; }
        ++iU8;
      } else if (cc & 0x08) {
        const uint32_t m = bitsetMode(b, bitsU16, iU16);
        for (int k = 0; k < 3; ++k) t[k] = bef(b + u16Base + 12ull * iU16 + 4 * k);
        if (m == 0) { for (int k = 0; k < 3; ++k) t[k] += float(be16(b + p16 + 2 * k)) * kTrn; p16 += 6; }
        else { const int k = m == 1 ? 0 : m == 2 ? 1 : 2; t[k] += float(be16(b + p16)) * kTrn; p16 += 2; }
        ++iU16;
      } else {
        for (int k = 0; k < 3; ++k) t[k] = bef(b + praw + 12ull * iRaw + 4 * k);
        ++iRaw;
      }
      const int mj = toModel[bones[ch]];
      if (mj < 0) continue;
      float* dst = &clip.trs[(size_t(f) * ms.n + size_t(mj)) * 7];
      std::memcpy(dst, q, 16);
      if (hasT) std::memcpy(dst + 4, t, 12);
      clip.driven[size_t(mj)] = 1;
    }
  }
  return true;
}

// ── evaluation ─────────────────────────────────────────────────────────────────────────────
void evalSkin(const Model& model, double seconds, float sway, std::vector<float>* out) {
  const Skeleton& s = model.skel;
  const Clip& c = model.clip;
  std::vector<Mat4> world(size_t(s.n));
  float fk = 0;
  int k0 = 0, k1 = 0;
  if (c.frames > 1) {
    const double keys = seconds * 30.0 / c.keySpacing;
    const double span = double(c.frames - 1);
    const double kk = std::fmod(keys, span);
    k0 = int(kk); k1 = std::min(k0 + 1, c.frames - 1); fk = float(kk - k0);
  }
  for (int j = 0; j < s.n; ++j) {
    Mat4 local;
    if (c.frames > 0) {
      const float* a = &c.trs[(size_t(k0) * s.n + size_t(j)) * 7];
      const float* b = &c.trs[(size_t(k1) * s.n + size_t(j)) * 7];
      float q[4], t[3];
      slerpLike(a, b, fk, q);
      for (int k = 0; k < 3; ++k) t[k] = a[4 + k] * (1 - fk) + b[4 + k] * fk;
      local = fromTrs(q, t);
    } else {
      local = s.bindLocal[size_t(j)];
    }
    if (sway != 0 && j < int(model.capeDepth.size()) && model.capeDepth[size_t(j)]) {
      // a slow swing about the joint's local X, growing down the chain
      const float d = float(model.capeDepth[size_t(j)]);
      const float ang = sway * (0.35f + 0.12f * d) * std::sin(float(seconds) * 1.7f - 0.55f * d);
      const float h = 0.5f * ang;
      const float qs[4] = {std::sin(h), 0, 0, std::cos(h)}, t0[3] = {0, 0, 0};
      local = mul(local, fromTrs(qs, t0));
    }
    const int p = s.parent[size_t(j)];
    world[size_t(j)] = (p >= 0 && p < j) ? mul(world[size_t(p)], local) : local;
  }
  out->resize(size_t(s.n) * 12);
  for (int j = 0; j < s.n; ++j) {
    const Mat4 m = mul(world[size_t(j)], s.invBind[size_t(j)]);
    std::memcpy(&(*out)[size_t(j) * 12], m.data(), 12 * sizeof(float));
  }
}

void skinMesh(const Mesh& m, const std::vector<float>& sk, std::vector<float>* out) {
  const size_t nv = m.pos.size() / 3;
  out->resize(nv * 8);
  const size_t nj = sk.size() / 12;
  for (size_t v = 0; v < nv; ++v) {
    float M[12] = {0};
    for (int k = 0; k < 4; ++k) {
      const float w = m.weight[v * 4 + k];
      const size_t j = m.joint[v * 4 + k];
      if (w <= 0 || j >= nj) continue;
      for (int e = 0; e < 12; ++e) M[e] += w * sk[j * 12 + e];
    }
    const float* p = &m.pos[v * 3]; const float* n = &m.nrm[v * 3];
    float* o = &(*out)[v * 8];
    for (int r = 0; r < 3; ++r) {
      o[r] = M[r * 4] * p[0] + M[r * 4 + 1] * p[1] + M[r * 4 + 2] * p[2] + M[r * 4 + 3];
      o[3 + r] = M[r * 4] * n[0] + M[r * 4 + 1] * n[1] + M[r * 4 + 2] * n[2];
    }
    const float l = std::sqrt(o[3] * o[3] + o[4] * o[4] + o[5] * o[5]);
    if (l > 0) for (int r = 0; r < 3; ++r) o[3 + r] /= l;
    o[6] = m.uv[v * 2]; o[7] = m.uv[v * 2 + 1];
  }
}

void posedBounds(const Model& model, const std::vector<float>& sk, float mn[3], float mx[3]) {
  for (int k = 0; k < 3; ++k) { mn[k] = 1e30f; mx[k] = -1e30f; }
  std::vector<float> v;
  for (const auto& m : model.meshes) {
    skinMesh(m, sk, &v);
    for (size_t i = 0; i + 7 < v.size(); i += 8)
      for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], v[i + k]); mx[k] = std::max(mx[k], v[i + k]); }
  }
}

}  // namespace sr::kernel::cview
