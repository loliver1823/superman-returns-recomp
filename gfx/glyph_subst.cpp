// iter-464 input lane: device-matched button prompts - contract in gfx/glyph_subst.hpp,
// design and evidence in docs/INPUT_GLYPHS.md.
#include "glyph_subst.hpp"

#include "cube_layout.hpp"   // iter-465 polish: the tiled slice size
#include "guest_image.hpp"   // iter-465 polish: de-tile a live (tiled) icon before hashing

#include "../app/log.hpp"
#include "../kernel/host_input.hpp"
#include "../kernel/pc_controls.hpp"   // scancodeFromName (SDL key names incl. SDL2's media names)

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace sr::gfx::glyphs {

namespace {

namespace HI = sr::kernel::hostinput;
constexpr int kSets = int(HI::GlyphSet::kCount);

struct TexId {
  std::string name;
  uint32_t fmt = 0, w = 0, h = 0, len = 0;
  uint64_t fnv = 0;
};

struct Recipe {
  enum Kind : uint8_t { Keep = 0, Blank, Png, Key, Cluster } kind = Keep;
  std::string png;
  int ctl[4] = {-1, -1, -1, -1};
  float scale = 1.0f, dx = 0.0f, dy = 0.0f;
};

struct Manifest {
  std::vector<TexId> tex;
  std::vector<std::array<Recipe, kSets>> use;   // per tex
  std::map<std::string, std::string> keyGlyph;  // canonical input name -> png
  std::string keyDefault;
  uint32_t canvas = 128;
};

struct State {
  std::mutex mu;
  bool tried = false, ok = false;
  std::string root;
  Manifest m;
  std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> pngCache;  // 128-ish RGBA
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> pngDims;
  // (icon, set) -> built image; keyboard images remember the input names they were built from.
  struct Built { std::unique_ptr<Image> img; uint64_t keySig = 0; };
  std::map<uint32_t, Built> built;
};

State& S() {
  static State s;
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(uint8_t(s[a]))) ++a;
  while (b > a && std::isspace(uint8_t(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

int setFromName(const std::string& n) {
  if (n == "xbox") return int(HI::GlyphSet::Xbox360);
  if (n == "ps4") return int(HI::GlyphSet::PS4);
  if (n == "ps5") return int(HI::GlyphSet::PS5);
  if (n == "switch") return int(HI::GlyphSet::Switch);
  if (n == "keyboard") return int(HI::GlyphSet::Keyboard);
  return -1;
}

// The canonical (lower-case) form of an input name in the controls lane's vocabulary: SDL key
// names in SDL's own spelling ("left shift", "return"), "mouse1".."mouse5", "wheelup",
// "wheeldown", "mousemove". "" for an unknown name.
std::string canonicalInput(std::string n) {
  n = trim(n);
  std::string low;
  for (char ch : n) low += char(std::tolower(uint8_t(ch)));
  static const char* const kPseudo[] = {"mouse1", "mouse2", "mouse3", "mouse4", "mouse5",
                                        "wheelup", "wheeldown", "wheel", "mousemove"};
  for (const char* p : kPseudo)
    if (low == p) return low;
  if (n.empty()) return {};
  uint16_t code = 0;
  if (!sr::kernel::pccontrols::scancodeFromName(n.c_str(), &code)) return {};
  std::string canon;
  for (const char* q = SDL_GetScancodeName(SDL_Scancode(code)); q && *q; ++q) canon += char(std::tolower(uint8_t(*q)));
  return canon;
}

int controlFromName(const std::string& n) {
  for (int c = 0; c < HI::kControlCount; ++c)
    if (n == HI::controlName(HI::Control(c))) return c;
  return -1;
}

int parse(const std::string& text, Manifest* out, std::string* errors) {
  Manifest m;
  int problems = 0;
  std::istringstream in(text);
  std::string line;
  int lineNo = 0;
  auto err = [&](const std::string& msg) {
    ++problems;
    if (errors) {
      if (!errors->empty()) *errors += "; ";
      *errors += "line " + std::to_string(lineNo) + ": " + msg;
    }
  };
  std::map<std::string, size_t> texIndex;
  while (std::getline(in, line)) {
    ++lineNo;
    if (const size_t h = line.find('#'); h != std::string::npos) line.resize(h);
    line = trim(line);
    if (line.empty()) continue;
    std::istringstream ls(line);
    std::string kw;
    ls >> kw;
    if (kw == "canvas") {
      uint32_t c = 0;
      if (!(ls >> c) || c < 16 || c > 1024) { err("bad canvas"); continue; }
      m.canvas = c;
    } else if (kw == "tex") {
      TexId t;
      std::string fnvHex;
      if (!(ls >> t.name >> t.fmt >> t.w >> t.h >> t.len >> fnvHex)) { err("bad tex"); continue; }
      t.fnv = std::strtoull(fnvHex.c_str(), nullptr, 16);
      if (texIndex.count(t.name)) { err("duplicate tex " + t.name); continue; }
      texIndex[t.name] = m.tex.size();
      m.tex.push_back(t);
      m.use.emplace_back();
    } else if (kw == "use") {
      std::string setName, texName, recipe;
      if (!(ls >> setName >> texName >> recipe)) { err("bad use"); continue; }
      const int set = setFromName(setName);
      auto it = texIndex.find(texName);
      if (set < 0) { err("unknown set " + setName); continue; }
      if (it == texIndex.end()) { err("use before tex " + texName); continue; }
      Recipe r;
      if (recipe == "keep") r.kind = Recipe::Keep;
      else if (recipe == "blank") r.kind = Recipe::Blank;
      else if (recipe.rfind("png:", 0) == 0) { r.kind = Recipe::Png; r.png = recipe.substr(4); }
      else if (recipe.rfind("key:", 0) == 0) {
        r.kind = Recipe::Key;
        r.ctl[0] = controlFromName(recipe.substr(4));
        if (r.ctl[0] < 0) { err("unknown control in " + recipe); continue; }
      } else if (recipe.rfind("cluster:", 0) == 0) {
        r.kind = Recipe::Cluster;
        std::string rest = recipe.substr(8);
        bool bad = false;
        for (int k = 0; k < 4; ++k) {
          const size_t c = rest.find(',');
          r.ctl[k] = controlFromName(rest.substr(0, c));
          if (r.ctl[k] < 0) bad = true;
          rest = c == std::string::npos ? std::string() : rest.substr(c + 1);
        }
        if (bad) { err("bad cluster " + recipe); continue; }
      } else {
        err("unknown recipe " + recipe);
        continue;
      }
      ls >> r.scale;
      ls >> r.dx >> r.dy;
      if (!(r.scale > 0.05f && r.scale < 4.0f)) { err("bad scale"); r.scale = 1.0f; }
      m.use[it->second][set] = r;
    } else if (kw == "keyglyph") {
      std::string key, png;
      if (!(ls >> key >> png)) { err("bad keyglyph"); continue; }
      std::replace(key.begin(), key.end(), '_', ' ');
      const std::string canon = canonicalInput(key);
      if (canon.empty()) { err("unknown input " + key); continue; }
      m.keyGlyph[canon] = png;
    } else if (kw == "keydefault") {
      std::string png;
      if (!(ls >> png)) { err("bad keydefault"); continue; }
      m.keyDefault = png;
    } else {
      err("unknown keyword " + kw);
    }
  }
  if (out) *out = std::move(m);
  return problems;
}

std::string exeDir() {
  std::string d;
  if (const char* base = SDL_GetBasePath()) d = base;   // SDL3: SDL owns the string (SDL2: SDL_free)
  return d;
}

// ── image helpers (straight alpha in, premultiplied float canvas, straight out) ──
struct Canvas {
  uint32_t n = 0;
  std::vector<float> px;  // premultiplied RGBA
  explicit Canvas(uint32_t size) : n(size), px(size_t(size) * size * 4, 0.0f) {}
};

// Bilinear sample of a straight-alpha RGBA8 image in premultiplied space, clamp to edge,
// transparent outside [0,1].
void samplePremul(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, float u, float v,
                  float out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0.0f;
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) return;
  const float x = u * float(w) - 0.5f, y = v * float(h) - 0.5f;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - float(x0), fy = y - float(y0);
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const int xi = std::clamp(x0 + i, 0, int(w) - 1), yi = std::clamp(y0 + j, 0, int(h) - 1);
      const uint8_t* p = &img[(size_t(yi) * w + xi) * 4];
      const float wgt = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
      const float a = p[3] / 255.0f;
      out[0] += wgt * a * (p[0] / 255.0f);
      out[1] += wgt * a * (p[1] / 255.0f);
      out[2] += wgt * a * (p[2] / 255.0f);
      out[3] += wgt * a;
    }
}

// Draws `img` (its whole frame) into the canvas square [x0,x0+size) x [y0,y0+size),
// supersampled 4x4 per canvas pixel, alpha-over.
// Draws `img` (its whole frame) into the canvas rectangle [x0,x0+dw) x [y0,y0+dh),
// supersampled 4x4 per canvas pixel, alpha-over.
void blit(Canvas& c, const std::vector<uint8_t>& img, uint32_t w, uint32_t h, float x0, float y0,
          float dw, float dh) {
  if (dw <= 0.0f || dh <= 0.0f) return;
  const int ix0 = std::max(0, int(std::floor(x0))), iy0 = std::max(0, int(std::floor(y0)));
  const int ix1 = std::min(int(c.n), int(std::ceil(x0 + dw)));
  const int iy1 = std::min(int(c.n), int(std::ceil(y0 + dh)));
  constexpr int SS = 4;
  for (int y = iy0; y < iy1; ++y)
    for (int x = ix0; x < ix1; ++x) {
      float acc[4] = {0, 0, 0, 0};
      for (int sy = 0; sy < SS; ++sy)
        for (int sx = 0; sx < SS; ++sx) {
          const float px = float(x) + (sx + 0.5f) / SS, py = float(y) + (sy + 0.5f) / SS;
          float s[4];
          samplePremul(img, w, h, (px - x0) / dw, (py - y0) / dh, s);
          for (int k = 0; k < 4; ++k) acc[k] += s[k];
        }
      float* d = &c.px[(size_t(y) * c.n + x) * 4];
      const float inv = 1.0f / (SS * SS);
      const float sa = acc[3] * inv;
      for (int k = 0; k < 4; ++k) d[k] = acc[k] * inv + d[k] * (1.0f - sa);
    }
}

// The opaque extent of an image (alpha > 32): pack glyphs carry different padding, the
// recipes size the VISIBLE glyph.
struct Box { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
Box opaqueBox(const std::vector<uint8_t>& img, uint32_t w, uint32_t h) {
  int x0 = int(w), y0 = int(h), x1 = -1, y1 = -1;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
      if (img[(size_t(y) * w + x) * 4 + 3] > 32) {
        x0 = std::min(x0, int(x)); y0 = std::min(y0, int(y));
        x1 = std::max(x1, int(x)); y1 = std::max(y1, int(y));
      }
  if (x1 < 0) return {0, 0, float(w), float(h)};
  return {float(x0), float(y0), float(x1 + 1), float(y1 + 1)};
}

// Fits the image's opaque box into a `box`-sized square centred on (cx, cy), aspect kept.
void blitFit(Canvas& c, const std::vector<uint8_t>& img, uint32_t w, uint32_t h, float cx,
             float cy, float box) {
  const Box b = opaqueBox(img, w, h);
  const float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
  const float s = box / std::max(1.0f, std::max(bw, bh));
  const float bcx = (b.x0 + b.x1) * 0.5f, bcy = (b.y0 + b.y1) * 0.5f;
  blit(c, img, w, h, cx - bcx * s, cy - bcy * s, float(w) * s, float(h) * s);
}

std::unique_ptr<Image> finish(const Canvas& c) {
  auto img = std::make_unique<Image>();
  img->size = c.n;
  std::vector<float> cur = c.px;
  uint32_t n = c.n;
  for (;;) {
    std::vector<uint8_t> lvl(size_t(n) * n * 4);
    for (size_t i = 0; i < size_t(n) * n; ++i) {
      const float a = cur[i * 4 + 3];
      for (int k = 0; k < 3; ++k) {
        const float v = a > 1e-6f ? cur[i * 4 + k] / a : 0.0f;
        lvl[i * 4 + k] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
      lvl[i * 4 + 3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    img->levels.push_back(std::move(lvl));
    if (n == 1) break;
    const uint32_t m = n / 2;
    std::vector<float> nxt(size_t(m) * m * 4);
    for (uint32_t y = 0; y < m; ++y)
      for (uint32_t x = 0; x < m; ++x)
        for (int k = 0; k < 4; ++k)
          nxt[(size_t(y) * m + x) * 4 + k] =
              0.25f * (cur[((size_t(2 * y) * n) + 2 * x) * 4 + k] +
                       cur[((size_t(2 * y) * n) + 2 * x + 1) * 4 + k] +
                       cur[((size_t(2 * y + 1) * n) + 2 * x) * 4 + k] +
                       cur[((size_t(2 * y + 1) * n) + 2 * x + 1) * 4 + k]);
    cur.swap(nxt);
    n = m;
  }
  return img;
}

// caller holds s.mu
const std::vector<uint8_t>* loadPng(State& s, const std::string& rel, uint32_t* w, uint32_t* h) {
  auto it = s.pngCache.find(rel);
  if (it == s.pngCache.end()) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    uint32_t pw = 0, ph = 0;
    const std::string path = s.root + "/" + rel;
    if (!decodePng(path, &pw, &ph, buf.get())) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[GLYPH] cannot decode %s", path.c_str());
      buf.reset();
    }
    s.pngDims[rel] = {pw, ph};
    it = s.pngCache.emplace(rel, buf).first;
  }
  if (!it->second) return nullptr;
  *w = s.pngDims[rel].first;
  *h = s.pngDims[rel].second;
  return it->second.get();
}

// The art for an input name (unbound or without art of its own -> keydefault).
std::string keyPng(const State& s, const std::string& input) {
  auto it = s.m.keyGlyph.find(canonicalInput(input));
  return it != s.m.keyGlyph.end() ? it->second : s.m.keyDefault;
}

// What a keyboard recipe shows now: the inputs the prompt source names for its controls.
struct KeyNames {
  std::string in[4];
  uint64_t sig = 0;  // 0 = not a keyboard recipe
};
KeyNames keyNames(const Recipe& r) {
  KeyNames k;
  if (r.kind != Recipe::Key && r.kind != Recipe::Cluster) return k;
  uint64_t sig = 1469598103934665603ull;
  for (int i = 0; i < 4; ++i) {
    if (r.ctl[i] < 0) continue;
    k.in[i] = canonicalInput(HI::promptInputFor(HI::Control(r.ctl[i])));
    for (char ch : k.in[i]) sig = (sig ^ uint8_t(ch)) * 1099511628211ull;
    sig = (sig ^ 0x1Fu) * 1099511628211ull;
  }
  k.sig = sig ? sig : 1;
  return k;
}

// caller holds s.mu
std::unique_ptr<Image> build(State& s, const Recipe& r, const KeyNames& kn) {
  const uint32_t n = s.m.canvas;
  Canvas c(n);
  // `scale` = the visible glyph's extent as a fraction of the canvas (match the original
  // icon's opaque extent), centred, offset by (dx, dy) canvas fractions.
  const float full = float(n) * r.scale;
  const float cx = float(n) * (0.5f + r.dx), cy = float(n) * (0.5f + r.dy);
  uint32_t w = 0, h = 0;
  switch (r.kind) {
    case Recipe::Blank:
      break;
    case Recipe::Png: {
      const auto* img = loadPng(s, r.png, &w, &h);
      if (!img) return nullptr;
      blitFit(c, *img, w, h, cx, cy, full);
      break;
    }
    case Recipe::Key: {
      const auto* img = loadPng(s, keyPng(s, kn.in[0]), &w, &h);
      if (!img) return nullptr;
      blitFit(c, *img, w, h, cx, cy, full);
      break;
    }
    case Recipe::Cluster: {
      // One input drives all four directions (the mouse on the right stick): one glyph.
      if (!kn.in[0].empty() && kn.in[0] == kn.in[1] && kn.in[0] == kn.in[2] &&
          kn.in[0] == kn.in[3]) {
        const auto* img = loadPng(s, keyPng(s, kn.in[0]), &w, &h);
        if (!img) return nullptr;
        blitFit(c, *img, w, h, cx, cy, full * 0.8f);
        break;
      }
      // A plus: up (top centre), left, down (bottom centre), right - the d-pad's geometry,
      // so the game's own direction highlight lands on the matching key.
      const float cell = full / 3.0f;
      const float pos[4][2] = {{0, -1}, {-1, 0}, {0, 1}, {1, 0}};
      for (int k = 0; k < 4; ++k) {
        const auto* img = loadPng(s, keyPng(s, kn.in[k]), &w, &h);
        if (!img) return nullptr;
        blitFit(c, *img, w, h, cx + pos[k][0] * cell, cy + pos[k][1] * cell, cell * 0.96f);
      }
      break;
    }
    default:
      return nullptr;
  }
  return finish(c);
}

}  // namespace

uint64_t fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

int parseManifestForTest(const std::string& text, std::string* errors) {
  return parse(text, nullptr, errors);
}

bool init() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.tried) return s.ok;
  s.tried = true;
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<std::string> roots;
  if (const char* e = std::getenv("SR_GLYPHS_DIR"); e && *e) roots.push_back(e);
  const std::string ed = exeDir();
  if (!ed.empty()) roots.push_back(ed + "data/glyphs");
  roots.push_back((fs::current_path(ec) / "data" / "glyphs").string());
  for (const auto& r : roots) {
    const std::string path = r + "/glyph_map.txt";
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string errors;
    const int problems = parse(ss.str(), &s.m, &errors);
    s.root = r;
    s.ok = !s.m.tex.empty();
    SR_LOG_BOOT(problems ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
                "[GLYPH] %zu prompt icons, %zu key glyphs from %s%s%s", s.m.tex.size(),
                s.m.keyGlyph.size(), path.c_str(), problems ? "; problems: " : "",
                errors.c_str());
    return s.ok;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[GLYPH] no data/glyphs/glyph_map.txt next to the exe or in the working folder: "
              "button prompts stay the game's own");
  return false;
}

bool loaded() { return S().ok; }
const std::string& root() { return S().root; }

bool candidate(uint32_t fmt, uint32_t w, uint32_t h) {
  const State& s = S();
  if (!s.ok) return false;
  for (const auto& t : s.m.tex)
    if (t.fmt == fmt && t.w == w && t.h == h) return true;
  return false;
}

uint16_t identify(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* src, size_t avail) {
  const State& s = S();
  if (!s.ok || !src) return 0;
  // Several identities can share a size: hash each distinct length once.
  uint32_t lastLen = 0;
  uint64_t lastHash = 0;
  for (size_t i = 0; i < s.m.tex.size(); ++i) {
    const TexId& t = s.m.tex[i];
    if (t.fmt != fmt || t.w != w || t.h != h || t.len > avail) continue;
    if (t.len != lastLen) {
      lastHash = fnv1a64(src, t.len);
      lastLen = t.len;
    }
    if (lastHash == t.fnv) return uint16_t(i + 1);
  }
  return 0;
}

uint32_t tiledSliceBytes(uint32_t w, uint32_t h, uint32_t pitchTexels, uint32_t blockW, uint32_t blockBytes) {
  uint32_t pb = 0;
  return sr::gfx::cube::detail::levelSliceStride(0, true, w, h, pitchTexels, blockW, blockW, blockBytes, true, &pb);
}

uint16_t identifyTiled(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* src, size_t avail,
                       uint32_t pitchTexels, uint32_t blockW, uint32_t blockBytes, uint64_t* detiledHash) {
  if (detiledHash) *detiledHash = 0;
  if (!src || !blockW || !blockBytes) return 0;
  sr::gfx::DecodedImage di;
  if (!sr::gfx::decodeGuestImage(src, avail, blockW, blockBytes, w, h, pitchTexels, true, 0u, di) || di.truncated)
    return 0;
  if (detiledHash) *detiledHash = fnv1a64(di.lin.data(), di.lin.size());
  return identify(fmt, w, h, di.lin.data(), di.lin.size());
}

const char* iconName(uint16_t icon) {
  const State& s = S();
  if (!icon || icon > s.m.tex.size()) return "?";
  return s.m.tex[icon - 1].name.c_str();
}

uint32_t iconCount() { return uint32_t(S().m.tex.size()); }

uint64_t variant(uint16_t icon, uint8_t set) {
  const State& s = S();
  if (!s.ok || !icon || icon > s.m.tex.size() || set >= kSets) return 0;
  return keyNames(s.m.use[icon - 1][set]).sig;
}

const Image* replacement(uint16_t icon, uint8_t set) {
  State& s = S();
  if (!s.ok || !icon || icon > s.m.tex.size() || set >= kSets) return nullptr;
  std::lock_guard<std::mutex> lk(s.mu);
  const Recipe& r = s.m.use[icon - 1][set];
  if (r.kind == Recipe::Keep) return nullptr;
  const uint32_t key = (uint32_t(icon) << 8) | set;
  const KeyNames kn = keyNames(r);
  const uint64_t sig = kn.sig;
  auto it = s.built.find(key);
  if (it != s.built.end() && it->second.keySig == sig) return it->second.img.get();
  auto img = build(s, r, kn);
  if (!img) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[GLYPH] %s/%s: replacement could not be built - "
                "the game's own icon stays", s.m.tex[icon - 1].name.c_str(),
                HI::glyphSetName(HI::GlyphSet(set)));
  }
  auto& slot = s.built[key];
  slot.img = std::move(img);
  slot.keySig = sig;
  return slot.img.get();
}

bool keyArtFor(const std::string& input, std::string* png) {
  const State& s = S();
  auto it = s.m.keyGlyph.find(canonicalInput(input));
  const bool own = it != s.m.keyGlyph.end();
  if (png) *png = own ? it->second : s.m.keyDefault;
  return own;
}

bool decodePng(const std::string& path, uint32_t* w, uint32_t* h, std::vector<uint8_t>* rgba) {
#ifdef _WIN32
  using Microsoft::WRL::ComPtr;
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninit = SUCCEEDED(co);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> f;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&f)))) {
      const std::wstring wpath = std::filesystem::path(path).wstring();
      ComPtr<IWICBitmapDecoder> dec;
      ComPtr<IWICBitmapFrameDecode> frame;
      ComPtr<IWICFormatConverter> conv;
      UINT pw = 0, ph = 0;
      if (SUCCEEDED(f->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) &&
          SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
          SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) &&
          SUCCEEDED(conv->GetSize(&pw, &ph)) && pw && ph && pw <= 4096 && ph <= 4096) {
        rgba->resize(size_t(pw) * ph * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, pw * 4, UINT(rgba->size()), rgba->data()));
        *w = pw;
        *h = ph;
      }
    }
  }
  if (uninit) CoUninitialize();
  return ok;
#else
  (void)path; (void)w; (void)h; (void)rgba;
  return false;
#endif
}

}  // namespace sr::gfx::glyphs
