// iter-464 PORT FEATURE: resolution scale + display configuration (gfx/res_scale.hpp,
// docs/RESOLUTION.md).
#include "res_scale.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "../app/log.hpp"
#include "surface_key.hpp"  // C1 stage 3: the surface identity (shipping)
#include "../kernel/xam_content.hpp"
#include "../kernel/host_automation.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sr {
namespace gfx {
bool pm4ShmEnabled();
bool pm4RtCacheEnabled();
bool pm4GpuEdramEnabled();

namespace resscale {
namespace {

Scale s_scale;            // 1x until configure()
bool s_configured = false;
bool s_frozen = false;

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}
bool parseSize(const char* s, uint32_t& w, uint32_t& h) {
  if (!s || !*s) return false;
  unsigned a = 0, b = 0;
  char x = 0;
  if (std::sscanf(s, "%u%c%u", &a, &x, &b) != 3 || (x != 'x' && x != 'X')) return false;
  if (a < 320 || b < 200 || a > 16384 || b > 16384) return false;
  w = a;
  h = b;
  return true;
}
bool parseMode(const std::string& v, DisplayMode& m) {
  const std::string s = lower(trim(v));
  if (s == "windowed" || s == "window") { m = DisplayMode::Windowed; return true; }
  if (s == "borderless" || s == "fullscreen" || s == "borderless_fullscreen") {
    m = DisplayMode::Borderless;
    return true;
  }
  if (s == "exclusive" || s == "exclusive_fullscreen") { m = DisplayMode::Exclusive; return true; }
  return false;
}

// iter-466 gfx: the primary monitor's desktop mode, and the highest refresh it offers at that size
// (a 144 Hz monitor left at 60 Hz on the desktop still starts at 144). Win32 (runs before SDL).
NativeMode detectPrimaryNative() {
  NativeMode m;
#ifdef _WIN32
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
    m.w = dm.dmPelsWidth;
    m.h = dm.dmPelsHeight;
    m.hz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
    for (DWORD i = 0;; ++i) {
      DEVMODEW e{};
      e.dmSize = sizeof e;
      if (!EnumDisplaySettingsW(nullptr, i, &e)) break;
      if (e.dmPelsWidth == m.w && e.dmPelsHeight == m.h && e.dmDisplayFrequency > m.hz &&
          e.dmDisplayFrequency <= 1000)
        m.hz = e.dmDisplayFrequency;
    }
  }
#endif
  return m;
}

Display load() {
  Display d;
  // settings/display.ini (the save root's settings folder, beside game_options.ini).
  std::filesystem::path path;
  try {
    path = sr::kernel::xamContentRoot() / "settings" / "display.ini";
  } catch (...) {
  }
  std::ifstream f(path);
  if (f) {
    std::string line;
    uint32_t ww = 0, wh = 0, ow = 0, oh = 0;
    while (std::getline(f, line)) {
      line = trim(line);
      if (line.empty() || line[0] == '#' || line[0] == ';') continue;
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = lower(trim(line.substr(0, eq)));
      const std::string v = trim(line.substr(eq + 1));
      if (k == "res_scale") {
        bool a = false;
        uint32_t x = 1, y = 1;
        if (parseScale(v.c_str(), a, x, y)) {
          d.scaleAuto = a;
          d.scaleX = x;
          d.scaleY = y;
          d.scaleSource = "display.ini";
        }
      } else if (k == "display_mode") {
        if (parseMode(v, d.mode)) d.modeSource = "display.ini";
      } else if (k == "window_width") {
        ww = uint32_t(std::strtoul(v.c_str(), nullptr, 10));
      } else if (k == "window_height") {
        wh = uint32_t(std::strtoul(v.c_str(), nullptr, 10));
      } else if (k == "output_width") {
        ow = uint32_t(std::strtoul(v.c_str(), nullptr, 10));
      } else if (k == "output_height") {
        oh = uint32_t(std::strtoul(v.c_str(), nullptr, 10));
      } else if (k == "display_index") {   // iter-466 gfx
        const long n = std::strtol(v.c_str(), nullptr, 10);
        if (n >= 0 && n < 16) d.displayIndex = int(n);
      } else if (k == "output_refresh") {  // iter-466 gfx
        const unsigned long n = std::strtoul(v.c_str(), nullptr, 10);
        if (n <= 1000) d.outputHz = uint32_t(n);
      }
    }
    if (ww >= 320 && wh >= 200 && ww <= 16384 && wh <= 16384) {
      d.windowW = ww;
      d.windowH = wh;
    }
    if (ow >= 320 && oh >= 200 && ow <= 16384 && oh <= 16384) {
      d.outputW = ow;
      d.outputH = oh;
    }
  } else if (!path.empty() && !(std::getenv("SR_INSTANCE_ID") && *std::getenv("SR_INSTANCE_ID"))) {
    // iter-466 gfx: a player's FIRST START - detect the monitor and write active settings
    // (borderless at its resolution, its refresh, auto render resolution, frame rate = refresh).
    const NativeMode m = detectPrimaryNative();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (std::ofstream t(path, std::ios::binary); t) {
      t << firstStartIni(m);
      d.firstStart = true;
    }
    if (m.w && m.h) {
      d.mode = DisplayMode::Borderless;
      d.modeSource = "first-start";
      d.outputW = m.w;
      d.outputH = m.h;
      d.outputHz = m.hz;
      d.scaleAuto = true;
      d.scaleSource = "first-start";
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[DISPLAY] first start: detected the primary monitor at %ux%u @ %u Hz -> wrote %s "
                "(borderless, output %ux%u, refresh %u, render auto, target_fps monitor)",
                m.w, m.h, m.hz, path.string().c_str(), m.w, m.h, m.hz);
  }
  // Environment overrides (rolls, A/B).
  if (const char* e = std::getenv("SR_RES_SCALE")) {
    bool a = false;
    uint32_t x = 1, y = 1;
    if (parseScale(e, a, x, y)) {
      d.scaleAuto = a;
      d.scaleX = x;
      d.scaleY = y;
      d.scaleSource = "SR_RES_SCALE";
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[RES_SCALE] SR_RES_SCALE=%s not understood (auto | 1..7 | XxY)", e);
    }
  }
  if (const char* e = std::getenv("SR_DISPLAY_MODE")) {
    if (parseMode(e, d.mode)) d.modeSource = "SR_DISPLAY_MODE";
  }
  uint32_t w = 0, h = 0;
  if (parseSize(std::getenv("SR_WINDOW_SIZE"), w, h)) {
    d.windowW = w;
    d.windowH = h;
  }
  if (parseSize(std::getenv("SR_OUTPUT_SIZE"), w, h)) {
    d.outputW = w;
    d.outputH = h;
  }
  // iter-465 player: the PORT FEATURE defaults (kernel/host_automation.hpp portFeaturesOn: a
  // player, or an automated roll with SR_PORT_FEATURES=1). Anything the player's display.ini or
  // an env var set above wins. A player gets the game full screen (borderless, the desktop's
  // size; Alt+Enter = windowed) at the internal resolution that covers it (auto: 4K -> 3x,
  // 1440p/1080p -> 2x). An automated roll stays windowed whatever this says (app/main.cpp).
  // Natural rolls (port features off) keep the console's 1x and the 1280x720 window.
  if (sr::kernel::automation::portFeaturesOn()) {
    if (d.scaleSource == "default") {
      d.scaleAuto = true;
      d.scaleSource = "port-feature default";
    }
    if (d.modeSource == "default") {
      d.mode = DisplayMode::Borderless;
      d.modeSource = "port-feature default";
    }
  }
  return d;
}

}  // namespace

bool parseScale(const char* s, bool& isAuto, uint32_t& x, uint32_t& y) {
  if (!s) return false;
  const std::string v = lower(trim(s));
  if (v == "auto" || v == "a" || v == "0") {
    isAuto = true;
    x = y = 1;
    return true;
  }
  unsigned a = 0, b = 0;
  char sep = 0;
  int n = std::sscanf(v.c_str(), "%u%c%u", &a, &sep, &b);
  if (n == 3 && sep == 'x') {
    // "2x3"
  } else if (n >= 1 && (n == 1 || sep == 'x')) {
    if (n == 2) return false;  // "2x" without a y
    b = a;                      // "2" (or "2x" handled above) = both axes
  } else {
    return false;
  }
  if (a < 1 || b < 1 || a > kMaxScale || b > kMaxScale) return false;
  isAuto = false;
  x = a;
  y = b;
  return true;
}

const Display& display() {
  static const Display d = load();
  return d;
}

uint32_t autoScaleFor(uint32_t outW, uint32_t outH) {
  // iter-466 res4k: cover the 16:9 picture the present fits into the output, not the output
  // itself - an ultrawide or 32:9 screen pillarboxes the frame (3440x1440 shows a 2560x1440
  // picture: 2x, not 3x; 5120x1440: 2x, not 4x). 16:10 and 4:3 letterbox the same way.
  uint64_t fitW = outW, fitH = outH;
  if (uint64_t(outW) * 9u > uint64_t(outH) * 16u)
    fitW = (uint64_t(outH) * 16u + 8u) / 9u;  // wider than 16:9: pillarbox
  else
    fitH = (uint64_t(outW) * 9u + 15u) / 16u;  // taller than 16:9: letterbox
  uint32_t s = 1;
  while (s < kMaxScale && (720u * s < fitH || 1280u * s < fitW)) s++;
  // The user's target is native 4K (3x); higher automatic scales cost far more than
  // they show. An explicit setting can still ask for up to 7x.
  return std::min<uint32_t>(s, 4u);
}

void configure(uint32_t outputW, uint32_t outputH) {
  if (s_frozen) return;
  const Display& d = display();
  Scale s;
  if (d.scaleAuto) {
    s.x = s.y = autoScaleFor(outputW, outputH);
  } else {
    s.x = d.scaleX;
    s.y = d.scaleY;
  }
  s_scale = s;
  s_configured = true;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[RES_SCALE] requested %s%ux%u (source %s) for output %ux%u; display mode %s (%s), "
              "window %ux%u",
              d.scaleAuto ? "auto -> " : "", s.x, s.y, d.scaleSource.c_str(), outputW, outputH,
              modeName(d.mode), d.modeSource.c_str(), d.windowW, d.windowH);
}

void clampToDevice(bool tiledResources, uint32_t vaBitsPerResource, uint32_t maxTextureDim) {
  if (s_frozen) return;
  if (!s_configured) configure(1280, 720);
  Scale s = s_scale;
  const char* why = nullptr;
  if (s.scaled() && !(pm4RtCacheEnabled() && pm4GpuEdramEnabled() && pm4ShmEnabled())) {
    // The scaled path is the GPU EDRAM + shared-memory path; the CPU mirror, the CPU
    // resolve and the CPU texture decode are 1:1 with guest memory.
    why = "the PM4 GPU EDRAM / shared-memory path is off";
    s = Scale{};
  }
  if (s.scaled() && !tiledResources) {
    why = "no tiled resources (the scaled resolve memory is a reserved buffer)";
    s = Scale{};
  }
  // texture_cache.cc:161-196: the scaled resolve address space (512 MiB * area) must fit
  // the per-resource virtual address bits.
  while (s.scaled() && vaBitsPerResource &&
         vaBitsPerResource < 64 &&
         ((uint64_t(1) << 29) * s.area() - 1u) >> vaBitsPerResource) {
    if (s.x >= s.y) s.x--; else s.y--;
    why = "virtual address bits per resource";
  }
  // A 1280-wide surface and the 2048-tile EDRAM height must fit the host texture limit.
  while (s.scaled() && maxTextureDim && (1280u * s.x > maxTextureDim || 2048u * s.y > maxTextureDim)) {
    if (1280u * s.x > maxTextureDim) s.x--;
    if (2048u * s.y > maxTextureDim) s.y--;
    why = "texture size limit";
  }
  if (why && (s.x != s_scale.x || s.y != s_scale.y))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[RES_SCALE] clamped %ux%u -> %ux%u: %s", s_scale.x,
                s_scale.y, s.x, s.y, why);
  s_scale = s;
  s_frozen = true;
  edram::g_rtHeightScaleY = s.y;  // RT heights clamp to the host limit in scaled rows
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[RES_SCALE] draw resolution scale %ux%u (internal 1280x720 -> %ux%u)%s", s.x, s.y,
              1280u * s.x, 720u * s.y, s.scaled() ? "" : " - native 1x path");
}

const Scale& get() { return s_scale; }

std::string firstStartIni(const NativeMode& m) {
  std::string s =
      "# Superman Returns recomp - display settings (docs/RESOLUTION.md, docs/GRAPHICS_SETTINGS.md).\n"
      "# Written at the first start from the detected monitor. The game's PC SETTINGS screen\n"
      "# (MAIN MENU > OPTIONS, or in game PAUSE > OPTIONS) sets every\n"
      "# key; a hand edit is the same thing.\n";
  if (!m.w || !m.h) return s + "# (no monitor detected: the defaults apply)\n";
  char b[256];
  std::snprintf(b, sizeof b,
                "display_mode = borderless\noutput_width = %u\noutput_height = %u\noutput_refresh = %u\n"
                "res_scale = auto\ntarget_fps = monitor\n",
                m.w, m.h, m.hz);
  return s + b;
}

const char* modeName(DisplayMode m) {
  switch (m) {
    case DisplayMode::Borderless: return "borderless";
    case DisplayMode::Exclusive: return "exclusive";
    default: return "windowed";
  }
}

bool setSetting(const std::string& key, const std::string& value) {
  const std::string k = lower(trim(key));
  const std::string v = trim(value);
  bool ok = false;
  if (k == "res_scale") {
    bool a = false;
    uint32_t x = 1, y = 1;
    ok = parseScale(v.c_str(), a, x, y);
  } else if (k == "display_mode") {
    DisplayMode m{};
    ok = parseMode(v, m);
  } else if (k == "display_index") {   // iter-466 gfx
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    ok = !v.empty() && end && *end == '\0' && n >= 0 && n < 16;
  } else if (k == "output_refresh") {  // iter-466 gfx
    char* end = nullptr;
    const unsigned long n = std::strtoul(v.c_str(), &end, 10);
    ok = !v.empty() && end && *end == '\0' && n <= 1000;
  } else if (k == "window_width" || k == "window_height" || k == "output_width" ||
             k == "output_height") {
    char* end = nullptr;
    const unsigned long n = std::strtoul(v.c_str(), &end, 10);
    ok = !v.empty() && end && *end == '\0' && (n == 0 || (n >= 200 && n <= 16384));
  }
  if (!ok) return false;
  std::filesystem::path path;
  try {
    path = sr::kernel::xamContentRoot() / "settings" / "display.ini";
  } catch (...) {
    return false;
  }
  // Keep every other line (comments, other keys); replace the key's active line or append.
  std::vector<std::string> lines;
  if (std::ifstream f(path); f)
    for (std::string line; std::getline(f, line);) lines.push_back(line);
  bool replaced = false;
  for (std::string& line : lines) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    const size_t eq = t.find('=');
    if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == k) {
      line = k + " = " + v;
      replaced = true;
    }
  }
  if (!replaced) lines.push_back(k + " = " + v);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream o(path, std::ios::trunc);
  if (!o) return false;
  for (const std::string& line : lines) o << line << "\n";
  SR_LOG_BOOT(sr::LogSeverity::Info, "[RES_SCALE] setting %s = %s written (next launch)", k.c_str(),
              v.c_str());
  return true;
}

std::string storedSetting(const std::string& key) {
  const std::string k = lower(trim(key));
  std::filesystem::path path;
  try {
    path = sr::kernel::xamContentRoot() / "settings" / "display.ini";
  } catch (...) {
    return {};
  }
  std::string value;
  if (std::ifstream f(path); f)
    for (std::string line; std::getline(f, line);) {
      const std::string t = trim(line);
      if (t.empty() || t[0] == '#' || t[0] == ';') continue;
      const size_t eq = t.find('=');
      if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == k) value = trim(t.substr(eq + 1));
    }
  return value;
}

namespace {
std::mutex s_liveMu;
bool s_liveInit = false;
LiveDisplay s_live;
bool s_reqPending = false;
LiveDisplay s_req;
uint32_t s_deskW = 0, s_deskH = 0;

void ensureLiveLocked() {
  if (s_liveInit) return;
  const Display& d = display();
  s_live.mode = d.mode;
  s_live.windowW = d.windowW;
  s_live.windowH = d.windowH;
  s_live.outputW = d.outputW;
  s_live.outputH = d.outputH;
  s_liveInit = true;
}
}  // namespace

void requestLiveDisplay(const LiveDisplay& d) {
  std::lock_guard<std::mutex> lk(s_liveMu);
  s_req = d;
  s_reqPending = true;
}

bool takeLiveDisplayRequest(LiveDisplay& d) {
  std::lock_guard<std::mutex> lk(s_liveMu);
  if (!s_reqPending) return false;
  s_reqPending = false;
  d = s_req;
  return true;
}

void noteLiveDisplay(const LiveDisplay& d) {
  std::lock_guard<std::mutex> lk(s_liveMu);
  s_live = d;
  s_liveInit = true;
}

LiveDisplay liveDisplay() {
  std::lock_guard<std::mutex> lk(s_liveMu);
  ensureLiveLocked();
  return s_live;
}

void noteDesktopSize(uint32_t w, uint32_t h) {
  std::lock_guard<std::mutex> lk(s_liveMu);
  s_deskW = w;
  s_deskH = h;
}

void desktopSize(uint32_t& w, uint32_t& h) {
  std::lock_guard<std::mutex> lk(s_liveMu);
  w = s_deskW;
  h = s_deskH;
}

}  // namespace resscale
}  // namespace gfx
}  // namespace sr
