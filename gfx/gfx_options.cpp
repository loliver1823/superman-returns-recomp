// iter-466 gfx (PORT FEATURE): the host graphics options. Contract: gfx_options.hpp,
// docs/GRAPHICS_SETTINGS.md.
#include "gfx_options.hpp"

#include "../app/log.hpp"
#include "../kernel/host_automation.hpp"
#include "../kernel/xam_content.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace sr {
namespace gfx {
namespace gfxopt {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  return s;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substr(a, b - a);
}
bool parseInt(const std::string& v, int lo, int hi, int& out) {
  if (v.empty()) return false;
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  if (!end || *end != '\0' || n < lo || n > hi) return false;
  out = int(n);
  return true;
}
bool parseFloat(const std::string& v, float lo, float hi, float& out) {
  if (v.empty()) return false;
  char* end = nullptr;
  const float f = std::strtof(v.c_str(), &end);
  if (!end || *end != '\0' || !(f >= lo && f <= hi)) return false;
  out = std::round(f * 100.0f) / 100.0f;   // the store keeps two decimals
  return true;
}

struct EnvKey {
  const char* key;
  const char* env;
};
const EnvKey kEnv[] = {
    {"post_aa", "SR_GFX_POST_AA"},
    {"brightness", "SR_GFX_BRIGHTNESS"},
    {"gamma", "SR_GFX_GAMMA"},
    {"upscale_filter", "SR_GFX_UPSCALE"},
    {"sharpness", "SR_GFX_SHARPNESS"},
    {"anisotropic", "SR_GFX_ANISO"},
    {"lod_bias", "SR_GFX_LOD_BIAS"},
    {"bloom", "SR_GFX_BLOOM"},
    {"edge_blur", "SR_GFX_EDGE_BLUR"},
};

std::filesystem::path iniPath() {
  try {
    return sr::kernel::xamContentRoot() / "settings" / "display.ini";
  } catch (...) {
    return {};
  }
}

std::string readFile(const std::filesystem::path& p) {
  if (p.empty()) return {};
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// `key = value` set in an ini text, keeping every other line byte for byte (the active line
// of the key replaced, or one appended). Same rule as kernel/highfps.cpp withIniKey.
std::string withKey(const std::string& text, const std::string& key, const std::string& value) {
  std::string out;
  bool replaced = false;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    const size_t end = nl == std::string::npos ? text.size() : nl + 1;
    std::string line = text.substr(pos, end - pos);
    const std::string t = trim(line);
    if (!t.empty() && t[0] != '#' && t[0] != ';') {
      const size_t eq = t.find('=');
      if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == key) {
        const bool crlf = line.size() >= 2 && line[line.size() - 2] == '\r';
        line = key + " = " + value + (nl == std::string::npos ? "" : (crlf ? "\r\n" : "\n"));
        replaced = true;
      }
    }
    out += line;
    pos = end;
  }
  if (!replaced) {
    if (!out.empty() && out.back() != '\n') out += "\n";
    out += key + " = " + value + "\n";
  }
  return out;
}

bool writeKey(const std::string& key, const std::string& value) {
  try {
    namespace fs = std::filesystem;
    const fs::path p = iniPath();
    if (p.empty()) return false;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const std::string next = withKey(readFile(p), key, value);
    const fs::path tmp = p.parent_path() / "display.ini.gfx.tmp";
    {
      std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
      if (!o) return false;
      o.write(next.data(), std::streamsize(next.size()));
      if (!o) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::mutex s_mu;
bool s_init = false;
std::atomic<bool> s_ready{false};   // s_init, readable without the lock (version() on the draw path)
Settings s_cur;
std::atomic<uint64_t> s_version{1};

void ensureLocked() {
  if (s_init) return;
  s_init = true;
  Settings s;
  const char* src = "default";
  if (sr::kernel::automation::portFeaturesOn()) {
    const std::string text = readFile(iniPath());
    if (!text.empty()) {
      s = fromIniText(text);
      src = "display.ini";
    }
  } else {
    src = "default (port features off)";
  }
  bool anyEnv = false;
  for (const EnvKey& e : kEnv) {
    const char* v = std::getenv(e.env);
    if (!v || !*v) continue;
    if (applyKey(s, e.key, v)) {
      anyEnv = true;
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[GFX_OPT] %s=%s ignored (bad value)", e.env, v);
    }
  }
  s_cur = s;
  s_ready.store(true, std::memory_order_release);
  std::string line;
  for (const std::string& k : keys()) line += " " + k + "=" + formatKey(s, k);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[GFX_OPT] source=%s%s:%s", src, anyEnv ? " + env" : "",
              line.c_str());
}

}  // namespace

bool operator==(const Settings& a, const Settings& b) {
  return a.postAa == b.postAa && a.brightness == b.brightness && a.gamma == b.gamma &&
         a.upscale == b.upscale && a.sharpness == b.sharpness && a.anisotropic == b.anisotropic &&
         a.lodBias == b.lodBias && a.bloom == b.bloom && a.edgeBlur == b.edgeBlur;
}

const std::vector<std::string>& keys() {
  static const std::vector<std::string> k = {"post_aa",   "brightness", "gamma",  "upscale_filter",
                                             "sharpness", "anisotropic", "lod_bias",
                                             "bloom",     "edge_blur"};
  return k;
}

bool applyKey(Settings& s, const std::string& keyIn, const std::string& valueIn) {
  const std::string key = lower(trim(keyIn));
  const std::string v = lower(trim(valueIn));
  if (key == "post_aa") {
    if (v == "off" || v == "0" || v == "none") s.postAa = PostAa::Off;
    else if (v == "fxaa") s.postAa = PostAa::Fxaa;
    else if (v == "fxaa_extreme") s.postAa = PostAa::FxaaExtreme;
    else return false;
    return true;
  }
  if (key == "brightness") return parseInt(v, -50, 50, s.brightness);
  if (key == "gamma") return parseFloat(v, 0.5f, 1.5f, s.gamma);
  if (key == "upscale_filter") {
    if (v == "bilinear") s.upscale = Upscale::Bilinear;
    else if (v == "cas") s.upscale = Upscale::Cas;
    else if (v == "fsr") s.upscale = Upscale::Fsr;
    else return false;
    return true;
  }
  if (key == "sharpness") return parseInt(v, 0, 100, s.sharpness);
  if (key == "anisotropic") {
    if (v == "off" || v == "0" || v == "1") {
      s.anisotropic = 0;
      return true;
    }
    int n = 0;
    if (!parseInt(v == "2x" || v == "4x" || v == "8x" || v == "16x" ? v.substr(0, v.size() - 1) : v, 2, 16, n) ||
        (n & (n - 1)) != 0)
      return false;
    s.anisotropic = n;
    return true;
  }
  if (key == "lod_bias") {
    float f = 0;
    if (!parseFloat(v, -2.0f, 2.0f, f)) return false;
    s.lodBias = f == 0.0f ? 0.0f : f;   // never -0
    return true;
  }
  if (key == "bloom") {
    if (v == "on") s.bloom = 100;
    else if (v == "off") s.bloom = 0;
    else return parseInt(v.size() > 1 && v.back() == '%' ? v.substr(0, v.size() - 1) : v, 0, 200, s.bloom);
    return true;
  }
  if (key == "edge_blur") {
    if (v == "on" || v == "1" || v == "true") s.edgeBlur = true;
    else if (v == "off" || v == "0" || v == "false") s.edgeBlur = false;
    else return false;
    return true;
  }
  return false;
}

std::string formatKey(const Settings& s, const std::string& keyIn) {
  const std::string key = lower(trim(keyIn));
  char buf[32];
  if (key == "post_aa")
    return s.postAa == PostAa::Fxaa ? "fxaa" : s.postAa == PostAa::FxaaExtreme ? "fxaa_extreme" : "off";
  if (key == "brightness") return std::to_string(s.brightness);
  if (key == "gamma") {
    std::snprintf(buf, sizeof buf, "%.2f", double(s.gamma));
    return buf;
  }
  if (key == "upscale_filter")
    return s.upscale == Upscale::Cas ? "cas" : s.upscale == Upscale::Fsr ? "fsr" : "bilinear";
  if (key == "sharpness") return std::to_string(s.sharpness);
  if (key == "anisotropic") return s.anisotropic >= 2 ? std::to_string(s.anisotropic) : "off";
  if (key == "lod_bias") {
    std::snprintf(buf, sizeof buf, "%.2f", double(s.lodBias));
    return buf;
  }
  if (key == "bloom") return std::to_string(s.bloom);
  if (key == "edge_blur") return s.edgeBlur ? "on" : "off";
  return {};
}

Settings fromIniText(const std::string& text) {
  Settings s;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    const size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string v = trim(t.substr(eq + 1));
    if (const size_t h = v.find('#'); h != std::string::npos) v = trim(v.substr(0, h));
    applyKey(s, t.substr(0, eq), v);
  }
  return s;
}

// The constant block (16 float4 each) holding constant `reg` was uploaded for this draw.
bool blockUploaded(uint32_t use, uint32_t reg) {
  if ((use & 0x10000u) || (use & 0xFFFFu) == 0xFFFFu || (use & 0xFFFFu) == 0u) return true;
  return (use >> (reg / 16u)) & 1u;
}

void patchWith(const Settings& s, uint64_t psHash, float* c, uint32_t use) {
  if (psHash == kPsToneMapBloom && s.bloom != 100 && blockUploaded(use, 1))
    c[1 * 4 + 0] *= float(s.bloom) / 100.0f;
}

int selfTest() {
  int fail = 0;
  auto check = [&](bool c, const char* what) {
    if (!c) {
      std::fprintf(stderr, "[gfxopt selftest] FAIL %s\n", what);
      fail++;
    }
  };
  Settings d;
  for (const std::string& k : keys()) {
    Settings r;
    check(applyKey(r, k, formatKey(d, k)) && r == d, "default round trip");
  }
  Settings s;
  check(applyKey(s, "post_aa", "FXAA") && s.postAa == PostAa::Fxaa, "post_aa fxaa");
  check(!applyKey(s, "post_aa", "smaa") && s.postAa == PostAa::Fxaa, "post_aa bad keeps");
  check(applyKey(s, "brightness", "-50") && s.brightness == -50, "brightness lo");
  check(!applyKey(s, "brightness", "51") && s.brightness == -50, "brightness hi refused");
  check(!applyKey(s, "brightness", "5x"), "brightness junk refused");
  check(applyKey(s, "gamma", "1.234") && s.gamma == 1.23f, "gamma two decimals");
  check(!applyKey(s, "gamma", "0.4"), "gamma lo refused");
  check(applyKey(s, "upscale_filter", "fsr") && s.upscale == Upscale::Fsr, "upscale fsr");
  check(applyKey(s, "sharpness", "100") && s.sharpness == 100, "sharpness");
  check(!applyKey(s, "unknown", "1"), "unknown key");
  check(applyKey(s, "anisotropic", "16") && s.anisotropic == 16, "aniso 16");
  check(applyKey(s, "anisotropic", "4x") && s.anisotropic == 4, "aniso 4x");
  check(!applyKey(s, "anisotropic", "3") && s.anisotropic == 4, "aniso 3 refused");
  check(applyKey(s, "anisotropic", "off") && s.anisotropic == 0, "aniso off");
  check(applyKey(s, "lod_bias", "-0.5") && s.lodBias == -0.5f, "lod bias");
  check(!applyKey(s, "lod_bias", "2.5"), "lod bias range");
  check(applyKey(s, "lod_bias", "-0.00") && formatKey(s, "lod_bias") == "0.00", "lod bias -0");
  check(applyKey(s, "bloom", "50") && s.bloom == 50, "bloom 50");
  check(applyKey(s, "bloom", "off") && s.bloom == 0 && applyKey(s, "bloom", "on") && s.bloom == 100,
        "bloom on/off");
  check(!applyKey(s, "bloom", "201") && s.bloom == 100, "bloom range");
  check(applyKey(s, "edge_blur", "off") && !s.edgeBlur && formatKey(s, "edge_blur") == "off", "edge blur");
  {
    float c[256 * 4];
    for (int i = 0; i < 256 * 4; i++) c[i] = 1.5f;
    Settings b;
    b.bloom = 50;
    b.edgeBlur = false;
    patchWith(b, kPsToneMapBloom, c, 0x1FFFFu);
    check(c[4] == 0.75f && c[5] == 1.5f && c[255 * 4] == 1.5f, "bloom scales c1.x only");
    patchWith(b, kPsEdgeBlur, c, 0x1FFFFu);
    check(c[255 * 4] == 1.5f && c[4] == 0.75f, "edge blur: no constant written");
    c[4] = 1.5f;
    patchWith(b, kPsToneMapBloom, c, 0x0002u);
    check(c[4] == 1.5f, "block not uploaded: untouched");
    c[4] = 0.75f;
    patchWith(Settings{}, kPsToneMapBloom, c, 0x1FFFFu);
    check(c[4] == 0.75f, "defaults write nothing");
  }
  const Settings f = fromIniText(
      "# c\r\nres_scale = 2\r\npost_aa = fxaa_extreme # x\r\nbrightness=10\r\ngamma = bad\r\n");
  check(f.postAa == PostAa::FxaaExtreme && f.brightness == 10 && f.gamma == 1.0f, "fromIniText");
  check(withKey("a = 1\r\n# post_aa = fxaa\r\npost_aa = off\r\nb = 2", "post_aa", "fxaa") ==
            "a = 1\r\n# post_aa = fxaa\r\npost_aa = fxaa\r\nb = 2",
        "withKey replaces, keeps CRLF and comments");
  check(withKey("a = 1", "gamma", "1.10") == "a = 1\ngamma = 1.10\n", "withKey appends");
  check(withKey("", "gamma", "1.10") == "gamma = 1.10\n", "withKey empty");
  return fail;
}

namespace {
// One snapshot per settings version for the draw thread (the post draws run once a frame).
const Settings& drawSnapshot() {
  static std::atomic<uint64_t> s_seen{0};
  static thread_local uint64_t t_ver = 0;
  static thread_local Settings t_s;
  const uint64_t v = version();
  if (v != t_ver) {
    t_s = current();
    t_ver = v;
    if (s_seen.exchange(v) != v && (t_s.bloom != 100 || !t_s.edgeBlur))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[GFX_OPT] game post: bloom %d%% (ps %016llX c1.x), edge blur %s (ps %016llX)",
                  t_s.bloom, (unsigned long long)kPsToneMapBloom, t_s.edgeBlur ? "on" : "off (draw left out)",
                  (unsigned long long)kPsEdgeBlur);
  }
  return t_s;
}
}  // namespace

void patchPsConstants(uint64_t psHash, float* c, uint32_t use) {
  if (psHash != kPsToneMapBloom) return;
  patchWith(drawSnapshot(), psHash, c, use);
}

bool skipDraw(uint64_t psHash) {
  if (psHash != kPsEdgeBlur) return false;
  return !drawSnapshot().edgeBlur;
}

Settings current() {
  std::lock_guard<std::mutex> lk(s_mu);
  ensureLocked();
  return s_cur;
}

uint64_t version() {
  if (!s_ready.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lk(s_mu);
    ensureLocked();
  }
  return s_version.load(std::memory_order_acquire);
}

bool set(const std::string& keyIn, const std::string& value, std::string* err, bool persist) {
  const std::string key = lower(trim(keyIn));
  std::lock_guard<std::mutex> lk(s_mu);
  ensureLocked();
  Settings next = s_cur;
  if (!applyKey(next, key, value)) {
    if (err) *err = "bad value '" + value + "' for " + key;
    return false;
  }
  const std::string text = formatKey(next, key);
  const bool changed = next != s_cur;
  s_cur = next;
  if (changed) s_version.fetch_add(1, std::memory_order_acq_rel);
  bool wrote = false;
  if (persist && sr::kernel::automation::portFeaturesOn()) wrote = writeKey(key, text);
  if (persist || changed)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[GFX_OPT] set %s = %s (live%s)", key.c_str(), text.c_str(),
                wrote ? ", display.ini" : persist ? ", not stored" : ", preview");
  return true;
}

std::string get(const std::string& key) { return formatKey(current(), key); }

std::string defaultValue(const std::string& key) { return formatKey(Settings{}, key); }

}  // namespace gfxopt
}  // namespace gfx
}  // namespace sr
