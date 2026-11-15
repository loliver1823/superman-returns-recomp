// iter-466 gfx (PORT FEATURE): THE PC SETTINGS MODEL. Contract: pc_settings.hpp,
// docs/GRAPHICS_SETTINGS.md.
//
// Charter: host settings only. This file never reads or writes guest memory and never calls guest
// code. It reads and writes the host stores (settings/display.ini, settings/input.ini) through the
// owning modules where they exist (gfx/res_scale, kernel/highfps, gfx/gfx_options, the controls and
// input lanes) and applies live changes through their own live paths.
#include "pc_settings.hpp"

#include "highfps.hpp"
#include "host_automation.hpp"
#include "host_input.hpp"
#include "pc_controls.hpp"
#include "cheats_menu.hpp"    // lane E L-b: the CHEATS tab (the game's own cheats)
#include "game_options.hpp"   // lane E L-b: the GAME tab (the game's own options)
#include "sr_oracle.hpp"      // lane E L-b: the DEV TOOLS row is a dev-build row (SR_ORACLE=0 in the release composition)
#include "xam_content.hpp"
#include "../app/log.hpp"
#include "../gfx/gfx_options.hpp"
#include "../gfx/res_scale.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace sr {
namespace kernel {
namespace pcsettings {

namespace {
namespace fs = std::filesystem;
namespace RS = sr::gfx::resscale;
namespace HF = sr::kernel::highfps;
namespace GO = sr::gfx::gfxopt;
namespace GOPT = sr::kernel::gameopts;

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
std::string fmt(const char* f, ...) {
  char b[256];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

// ── ini files (display.ini flat; input.ini with [sections]) ──────────────────────────────
fs::path settingsFile(const char* name) {
  try {
    return sr::kernel::xamContentRoot() / "settings" / name;
  } catch (...) {
    return {};
  }
}
std::string readText(const fs::path& p) {
  if (p.empty()) return {};
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
bool writeText(const fs::path& p, const std::string& text) {
  if (p.empty()) return false;
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.string() + ".pcs.tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(text.data(), std::streamsize(text.size()));
    if (!o) return false;
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}
// The value of `key` in `section` ("" = before any section header / a flat file). Last one wins.
std::string iniGet(const std::string& text, const std::string& section, const std::string& key) {
  std::istringstream in(text);
  std::string cur, v;
  bool found = false;
  for (std::string line; std::getline(in, line);) {
    std::string t = trim(line);
    if (const size_t h = t.find_first_of("#;"); h != std::string::npos) t = trim(t.substr(0, h));
    if (t.empty()) continue;
    if (t.front() == '[' && t.back() == ']') {
      cur = lower(trim(t.substr(1, t.size() - 2)));
      continue;
    }
    const size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    if (lower(trim(t.substr(0, eq))) == key && (section.empty() ? true : cur == section)) {
      v = trim(t.substr(eq + 1));
      found = true;
    }
  }
  return found ? v : std::string();
}
// `key = value` in `section` of an ini text; every other line kept byte for byte. A missing key
// goes at the end of its section (a missing section is appended); a section-less key goes before
// the first section header.
std::string iniSet(const std::string& text, const std::string& section, const std::string& key,
                   const std::string& value) {
  std::vector<std::string> lines;
  {
    size_t pos = 0;
    while (pos < text.size()) {
      const size_t nl = text.find('\n', pos);
      lines.push_back(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
  }
  const bool crlf = text.find("\r\n") != std::string::npos;
  auto mk = [&](const std::string& s) { return crlf ? s + "\r" : s; };
  std::string cur;
  int lastInSection = -1, firstHeader = -1, sectionHeader = -1;
  bool replaced = false;
  for (int i = 0; i < int(lines.size()); ++i) {
    std::string t = trim(lines[size_t(i)]);
    if (t.empty() || t[0] == '#' || t[0] == ';') {
      continue;
    }
    if (t.front() == '[' && t.back() == ']') {
      cur = lower(trim(t.substr(1, t.size() - 2)));
      if (firstHeader < 0) firstHeader = i;
      if (cur == section) sectionHeader = i;
      continue;
    }
    if (cur != section) continue;
    lastInSection = i;
    const size_t eq = t.find('=');
    if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == key) {
      lines[size_t(i)] = mk(key + " = " + value);
      replaced = true;
    }
  }
  if (!replaced) {
    if (section.empty()) {
      const int at = firstHeader >= 0 ? firstHeader : int(lines.size());
      lines.insert(lines.begin() + at, mk(key + " = " + value));
    } else if (sectionHeader >= 0) {
      const int at = (lastInSection >= 0 ? lastInSection : sectionHeader) + 1;
      lines.insert(lines.begin() + at, mk(key + " = " + value));
    } else {
      if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back(mk(""));
      lines.push_back(mk("[" + section + "]"));
      lines.push_back(mk(key + " = " + value));
    }
  }
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size() || (!text.empty() && text.back() == '\n') || !replaced) out += "\n";
  }
  return out;
}

// ── registry (other lanes) ─────────────────────────────────────────────────────────────
std::mutex g_regMu;
std::map<std::string, bool> g_avail;
std::map<std::string, std::function<void(const std::string&)>> g_live;
std::map<std::string, std::function<std::string()>> g_actions;

bool availableOf(const std::string& id, bool dflt) {
  std::lock_guard<std::mutex> lk(g_regMu);
  auto it = g_avail.find(id);
  return it == g_avail.end() ? dflt : it->second;
}
void runLiveHook(const std::string& id, const std::string& v) {
  std::function<void(const std::string&)> fn;
  {
    std::lock_guard<std::mutex> lk(g_regMu);
    auto it = g_live.find(id);
    if (it != g_live.end()) fn = it->second;
  }
  if (fn) fn(v);
}

// ── live previews (the screen shows them; APPLY commits) ─────────────────────────────────
std::mutex g_prevMu;
std::map<std::string, std::string> g_preview;

// ── displays ─────────────────────────────────────────────────────────────────────────────
std::mutex g_dispMu;
std::vector<DisplayInfo> g_displays;
int g_curDisplay = 0;

// ── host audio / overlay state read by other threads ─────────────────────────────────────
std::atomic<int> g_volume{-1};       // -1 = not loaded yet
std::atomic<int> g_bgMute{-1};
std::atomic<int> g_perfOverlay{-1};

// Keys stored in display.ini by this module (no other owner), with their defaults.
struct Plain {
  const char* key;
  const char* dflt;
};
const Plain kPlain[] = {
    {"perf_overlay", "off"},     {"audio_volume", "100"},       {"audio_device", "default"},
    {"audio_bg_mute", "off"},
};

// game_options.ini keys this screen writes for the density lane (integer percent).
bool isDensity(const std::string& id) { return id == "population_density" || id == "traffic_density"; }
const char* plainDefault(const std::string& id) {
  for (const Plain& p : kPlain)
    if (id == p.key) return p.dflt;
  return nullptr;
}

// input.ini: id -> (section, key, default)
struct InKey {
  const char* id;
  const char* section;
  const char* key;
  const char* dflt;
};
const InKey kInput[] = {
    {"glyphs", "", "glyphs", "auto"},
    {"mouse_look", "mouse", "look", "on"},
    {"mouse_sensitivity", "mouse", "sensitivity", "1.0"},
    {"mouse_invert_x", "mouse", "invert_x", "off"},
    {"mouse_invert_y", "mouse", "invert_y", "off"},
};
const InKey* inputKey(const std::string& id) {
  for (const InKey& k : kInput)
    if (id == k.id) return &k;
  return nullptr;
}

bool isGfxopt(const std::string& id) {
  for (const std::string& k : GO::keys())
    if (k == id) return true;
  return false;
}

std::string onOff(const std::string& v) {
  const std::string s = lower(trim(v));
  if (s == "1" || s == "on" || s == "true" || s == "yes") return "on";
  if (s == "0" || s == "off" || s == "false" || s == "no") return "off";
  return s;
}

// ── display helpers ──────────────────────────────────────────────────────────────────────
RS::DisplayMode modeOf(const std::string& v) {
  const std::string s = lower(v);
  if (s == "borderless") return RS::DisplayMode::Borderless;
  if (s == "exclusive") return RS::DisplayMode::Exclusive;
  return RS::DisplayMode::Windowed;
}
bool parseWxH(const std::string& v, uint32_t& w, uint32_t& h) {
  return std::sscanf(v.c_str(), "%ux%u", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 16384 && h <= 16384;
}
std::string wxh(uint32_t w, uint32_t h) { return std::to_string(w) + "x" + std::to_string(h); }

DisplayInfo displayFor(int idx) {
  std::lock_guard<std::mutex> lk(g_dispMu);
  if (idx >= 0 && idx < int(g_displays.size())) return g_displays[size_t(idx)];
  DisplayInfo d;
  uint32_t w = 0, h = 0;
  RS::desktopSize(w, h);
  d.desktop = {w, h, uint32_t(HF::hostDisplayHz() + 0.5)};
  if (w && h) d.modes.push_back(d.desktop);
  return d;
}

std::string storedOr(const std::string& key, const std::string& dflt) {
  const std::string v = RS::storedSetting(key);
  return v.empty() ? dflt : v;
}

// The output size the RESOLUTION row shows for a mode (windowed: the window; else the output keys,
// else the desktop of the chosen monitor).
std::string resolutionFor(RS::DisplayMode m) {
  if (m == RS::DisplayMode::Windowed) {
    const uint32_t w = uint32_t(std::strtoul(RS::storedSetting("window_width").c_str(), nullptr, 10));
    const uint32_t h = uint32_t(std::strtoul(RS::storedSetting("window_height").c_str(), nullptr, 10));
    if (w >= 320 && h >= 200) return wxh(w, h);
    const RS::LiveDisplay ld = RS::liveDisplay();
    return wxh(ld.windowW, ld.windowH);
  }
  const uint32_t w = uint32_t(std::strtoul(RS::storedSetting("output_width").c_str(), nullptr, 10));
  const uint32_t h = uint32_t(std::strtoul(RS::storedSetting("output_height").c_str(), nullptr, 10));
  if (w >= 320 && h >= 200) return wxh(w, h);
  const DisplayInfo d = displayFor(std::atoi(value("display_index").c_str()));
  return wxh(d.desktop.w, d.desktop.h);
}

std::string fpsNorm(std::string v) {
  v = lower(trim(v));
  if (v.empty()) v = HF::kPlayerDefault;
  if (v == "unlimited" || v == "max") v = "uncapped";
  if (v == "1" || v == "refresh" || v == "vsync") v = "monitor";
  return v;
}

// The live display request for the current (previewed / stored) display rows.
void requestDisplay() {
  RS::LiveDisplay r;
  r.mode = modeOf(value("display_mode"));
  uint32_t w = 0, h = 0;
  const std::string res = value("resolution");
  const RS::LiveDisplay ld = RS::liveDisplay();
  r.windowW = ld.windowW;
  r.windowH = ld.windowH;
  if (r.mode == RS::DisplayMode::Windowed) {
    if (parseWxH(res, w, h)) {
      r.windowW = w;
      r.windowH = h;
    }
  } else if (parseWxH(res, w, h)) {
    r.outputW = w;
    r.outputH = h;
  }
  r.display = std::atoi(value("display_index").c_str());
  r.refreshHz = uint32_t(std::strtoul(value("output_refresh").c_str(), nullptr, 10));
  RS::requestLiveDisplay(r);
}

void applyAudioFromValues() {
  g_volume.store(std::clamp(std::atoi(value("audio_volume").c_str()), 0, 100), std::memory_order_relaxed);
  g_bgMute.store(value("audio_bg_mute") == "on" ? 1 : 0, std::memory_order_relaxed);
}

}  // namespace

const char* tabName(Tab t) {
  switch (t) {
    case Tab::Game: return "GAME";
    case Tab::Cheats: return "CHEATS";
    case Tab::Display: return "DISPLAY";
    case Tab::Graphics: return "GRAPHICS";
    case Tab::World: return "WORLD";
    case Tab::Audio: return "AUDIO";
    case Tab::Controls: return "CONTROLS";
    default: return "?";
  }
}

std::string renderResolutionLabel(uint32_t scale, uint32_t outW, uint32_t outH) {
  const uint32_t w = 1280u * scale, h = 720u * scale;
  const char* tag = !outH ? "" : h > outH ? " (SUPERSAMPLED)" : h == outH ? " (NATIVE)" : " (PERFORMANCE)";
  (void)outW;
  return fmt("%u x %u%s", w, h, tag);
}

// ── the catalogue ───────────────────────────────────────────────────────────────────────
std::vector<Option> catalogue() {
  std::vector<Option> v;
  // a row another lane implements stays off the screen until that lane registers it
  auto add = [&](Option o) {
    if (!o.available && o.hideUnavailable) return;
    v.push_back(std::move(o));
  };
  const RS::DisplayMode mode = modeOf(value("display_mode"));
  const int dispIdx = std::atoi(value("display_index").c_str());
  const DisplayInfo disp = displayFor(dispIdx);
  uint32_t outW = 0, outH = 0;
  parseWxH(value("resolution"), outW, outH);

  // ═══ GAME ═══ (lane E L-b, STEP5_PLAN 10: the game's own options through its own getters / setters,
  // kernel/game_options.hpp; port features only)
  if (GOPT::enabled()) {
    const bool vol = GOPT::volumesAvailable();
    auto toggle = [&](const char* id, const char* section, const char* label, const char* desc, const char* xbox,
                      const char* dflt) {
      Option o;
      o.id = id;
      o.tab = Tab::Game;
      o.section = section;
      o.kind = Kind::Toggle;
      o.apply = Apply::Live;
      o.label = label;
      o.desc = desc;
      o.perf = "NONE";
      o.xbox = xbox;
      o.defaultValue = dflt;
      add(o);
    };
    toggle(GOPT::kInvertY, "GAMEPLAY", "INVERT LOOK (Y AXIS)",
           "Pushing the stick up looks down. The game's own GAMEPLAY OPTIONS setting, kept in your save.",
           "OFF", "off");
    toggle(GOPT::kAutoCenter, "GAMEPLAY", "AUTO-CENTRE CAMERA",
           "The camera swings back behind Superman on its own. The game's own GAMEPLAY OPTIONS setting.", "ON", "on");
    toggle(GOPT::kVibration, "GAMEPLAY", "VIBRATION",
           "The controller rumbles. The game's own GAMEPLAY OPTIONS setting.", "ON", "on");
    {
      Option o;
      o.id = GOPT::kScheme;
      o.tab = Tab::Game;
      o.section = "GAMEPLAY";
      o.apply = Apply::Live;
      o.label = "CONTROL SCHEME";
      o.desc = "Which buttons do what on the controller. The game's own CONTROLLER OPTIONS setting; VIEW PAD "
               "LAYOUT on the CONTROLS tab shows it.";
      o.perf = "NONE";
      o.xbox = "CONFIG 1";
      o.choices = {{"0", "CONFIG 1"}, {"1", "CONFIG 2"}, {"2", "CONFIG 3"}, {"3", "CONFIG 4"}};
      o.defaultValue = "0";
      add(o);
    }
    for (const auto& e : {std::make_tuple(GOPT::kMusic, "MUSIC VOLUME"), std::make_tuple(GOPT::kSfx, "SOUND EFFECTS VOLUME")}) {
      Option o;
      o.id = std::get<0>(e);
      o.tab = Tab::Game;
      o.section = "SOUND";
      o.kind = Kind::Slider;
      o.apply = Apply::Live;
      o.label = std::get<1>(e);
      o.desc = "The game's own AUDIO OPTIONS slider (MASTER VOLUME on the AUDIO tab sets everything at once).";
      o.perf = "NONE";
      o.xbox = "the game's own level";
      o.lo = 0;
      o.hi = 10;
      o.step = 1;
      // no defaultValue: the game's starting levels come from its audio data, so RESET TO DEFAULTS leaves them
      o.available = vol;
      o.unavailableWhy = "The game's own mixer is not running yet; this level can be set once it is (in game).";
      o.unavailableBadge = "NOT YET";
      add(o);
    }
    toggle(GOPT::kAutosave, "SAVING", "AUTOSAVE WITHOUT ASKING",
           "ON: the game saves at its own save points into your current slot without the prompt. OFF: the game "
           "asks, as on the console.",
           "OFF (the game asks)", "on");
// SR_ORACLE stands in for "dev build" until E11/E12 (SR_RELEASE_COMPOSITION reaches C++ only as SR_ORACLE=0).
#if SR_ORACLE
    {   // DEV BUILDS ONLY (the coordinator, L-b): the Dev Tools pages' entry, compiled out of the release composition
      Option o;
      o.id = "dev_tools";
      o.tab = Tab::Game;
      o.section = "DEVELOPER (DEV BUILD)";
      o.kind = Kind::Action;
      o.apply = Apply::Live;
      o.label = "DEV TOOLS";
      o.desc = "The developer tools (docs/DEVTOOLS.md). Not in the public build.";
      o.perf = "NONE";
      o.xbox = "-";
      o.choices = {{"open", "OPEN"}};
      add(o);
    }
#endif
  }

  // ═══ DISPLAY ═══
  {
    Option o;
    o.id = "display_mode";
    o.tab = Tab::Display;
    o.section = "SCREEN";
    o.apply = Apply::Confirm;
    o.label = "DISPLAY MODE";
    o.desc = "How the game fills the screen. Borderless covers the screen without changing the "
             "monitor's mode and switches instantly (Alt+Enter toggles windowed). Exclusive takes "
             "the display over at the resolution and refresh rate below. Windowed runs in a "
             "resizable window.";
    o.perf = "NONE";
    o.xbox = "Full screen on the TV";
    o.choices = {{"windowed", "WINDOWED"}, {"borderless", "BORDERLESS FULLSCREEN"}, {"exclusive", "EXCLUSIVE FULLSCREEN"}};
    o.defaultValue = "borderless";
    add(o);
  }
  {
    Option o;
    o.id = "display_index";
    o.tab = Tab::Display;
    o.section = "SCREEN";
    o.apply = Apply::Confirm;
    o.label = "MONITOR";
    o.desc = "The monitor the game runs on. The resolution and refresh rate lists follow it.";
    o.perf = "NONE";
    o.xbox = "-";
    std::vector<DisplayInfo> ds = displays();
    for (size_t i = 0; i < ds.size(); ++i)
      o.choices.push_back({std::to_string(i), fmt("%zu: %s (%u x %u)", i + 1,
                                                  ds[i].name.empty() ? "DISPLAY" : sr::kernel::pcsettings::upper(ds[i].name).c_str(),
                                                  ds[i].desktop.w, ds[i].desktop.h)});
    if (o.choices.empty()) o.choices.push_back({"0", "1: PRIMARY"});
    o.defaultValue = "0";
    if (ds.size() < 2) {   // one monitor: nothing to choose, so no row
      o.available = false;
      o.hideUnavailable = true;
    }
    add(o);
  }
  {
    Option o;
    o.id = "resolution";
    o.tab = Tab::Display;
    o.section = "SCREEN";
    o.apply = Apply::Confirm;
    o.label = mode == RS::DisplayMode::Windowed ? "WINDOW SIZE" : "RESOLUTION";
    o.perf = "HIGH";
    o.xbox = "1280 x 720 (720p)";
    std::vector<std::pair<uint32_t, uint32_t>> sizes;
    auto push = [&](uint32_t w, uint32_t h) {
      if (w < 640 || h < 480) return;
      if (std::find(sizes.begin(), sizes.end(), std::make_pair(w, h)) == sizes.end()) sizes.push_back({w, h});
    };
    if (mode == RS::DisplayMode::Windowed) {
      o.desc = "The window's size in pixels. The picture fits the window, keeping 16:9. With RENDER "
               "RESOLUTION on AUTO a bigger window also renders more pixels, which is where the cost "
               "comes from.";
      for (auto s : {std::make_pair(1280u, 720u), std::make_pair(1600u, 900u), std::make_pair(1920u, 1080u),
                     std::make_pair(2560u, 1440u), std::make_pair(3200u, 1800u), std::make_pair(3840u, 2160u)})
        if (!disp.desktop.w || (s.first <= disp.desktop.w && s.second <= disp.desktop.h)) push(s.first, s.second);
      if (outW && outH) push(outW, outH);
    } else {
      o.desc = "The output resolution: the monitor's own modes. The game picks the internal render "
               "resolution that covers it (2160p: 3840 x 2160, 1440p and 1080p: 2560 x 1440). In "
               "borderless the desktop keeps its mode and the picture is scaled to it.";
      for (const DisplayMode& m : disp.modes) push(m.w, m.h);
      if (sizes.empty()) push(disp.desktop.w, disp.desktop.h);
      if (outW && outH) push(outW, outH);
    }
    std::sort(sizes.begin(), sizes.end(), [](auto a, auto b) { return uint64_t(a.first) * a.second > uint64_t(b.first) * b.second; });
    for (auto s : sizes) {
      std::string label = fmt("%u x %u", s.first, s.second);
      if (s.first == disp.desktop.w && s.second == disp.desktop.h && mode != RS::DisplayMode::Windowed)
        label += "  (NATIVE)";
      o.choices.push_back({wxh(s.first, s.second), label});
    }
    o.defaultValue = disp.desktop.w ? wxh(disp.desktop.w, disp.desktop.h) : "1280x720";
    add(o);
  }
  {
    Option o;
    o.id = "output_refresh";
    o.tab = Tab::Display;
    o.section = "SCREEN";
    o.apply = Apply::Confirm;
    o.label = "REFRESH RATE";
    o.desc = "The display's refresh rate in exclusive fullscreen. Borderless and windowed use the "
             "desktop's rate. AUTO picks the rate that carries the frame-rate limit.";
    o.perf = "NONE";
    o.xbox = "60 HZ";
    o.choices.push_back({"0", "AUTO"});
    std::vector<uint32_t> hz;
    for (const DisplayMode& m : disp.modes)
      if ((!outW || (m.w == outW && m.h == outH)) && m.hz && std::find(hz.begin(), hz.end(), m.hz) == hz.end())
        hz.push_back(m.hz);
    std::sort(hz.rbegin(), hz.rend());
    for (uint32_t h : hz) o.choices.push_back({std::to_string(h), fmt("%u HZ", h)});
    o.defaultValue = "0";
    if (mode != RS::DisplayMode::Exclusive) {
      o.available = false;
      o.unavailableWhy = "Only in exclusive fullscreen: borderless and windowed use the desktop's rate.";
      o.unavailableBadge = "EXCLUSIVE FULLSCREEN ONLY";
    }
    add(o);
  }
  {
    Option o;
    o.id = "res_scale";
    o.tab = Tab::Display;
    o.section = "RENDERING";
    o.apply = Apply::Restart;
    o.label = "RENDER RESOLUTION";
    o.desc = "The resolution the game draws at, before it is fitted to the screen. AUTO covers the "
             "output resolution. Higher than the output supersamples (smoother edges and finer "
             "detail, at a large GPU cost); lower runs faster. Applies after a restart.";
    o.perf = "HIGH";
    o.xbox = "1280 x 720";
    const uint32_t a = RS::autoScaleFor(outW ? outW : disp.desktop.w, outH ? outH : disp.desktop.h);
    o.choices.push_back({"auto", "AUTO - " + renderResolutionLabel(a, outW, outH)});
    for (uint32_t s = 1; s <= RS::kMaxScale; ++s)
      o.choices.push_back({std::to_string(s), renderResolutionLabel(s, outW, outH)});
    o.defaultValue = "auto";
    o.presetManaged = true;
    add(o);
  }
  {
    Option o;
    o.id = "upscale_filter";
    o.tab = Tab::Display;
    o.section = "RENDERING";
    o.apply = Apply::Live;
    o.label = "UPSCALING";
    o.desc = "How the rendered picture is scaled to the screen. Bilinear is the plain fit. AMD "
             "FidelityFX CAS sharpens while it scales; FSR 1.0 reconstructs edges when upscaling "
             "(Xenia's implementations).";
    o.perf = "LOW";
    o.xbox = "Bilinear";
    o.choices = {{"bilinear", "BILINEAR"}, {"cas", "AMD FIDELITYFX CAS"}, {"fsr", "AMD FIDELITYFX FSR 1.0"}};
    o.defaultValue = "bilinear";
    o.presetManaged = true;
    o.available = availableOf("upscale_filter", false);
    o.hideUnavailable = true;
    add(o);
  }
  {
    Option o;
    o.id = "sharpness";
    o.tab = Tab::Display;
    o.section = "RENDERING";
    o.kind = Kind::Slider;
    o.apply = Apply::Live;
    o.label = "SHARPNESS";
    o.desc = "Extra sharpening on top of the upscaling filter (CAS / FSR). 0% = none.";
    o.perf = "LOW";
    o.xbox = "None";
    o.lo = 0;
    o.hi = 100;
    o.step = 5;
    o.unit = "%";
    o.defaultValue = "0";
    o.presetManaged = true;
    o.available = availableOf("sharpness", false);
    o.hideUnavailable = true;
    add(o);
  }
  {
    Option o;
    o.id = "target_fps";
    o.tab = Tab::Display;
    o.section = "FRAME RATE";
    o.apply = Apply::Live;
    o.label = "FRAME RATE LIMIT";
    o.desc = "The most frames the game draws per second. The game's simulation stays at its own "
             "30 per second; above 30 the engine shows in-between frames from its own animation "
             "data. 30 is the original console frame rate.";
    o.perf = "HIGH";
    o.xbox = "30";
    const double hz = HF::config().displayHz;
    o.choices = {{"30", "30 (ORIGINAL)"}, {"60", "60"},   {"120", "120"}, {"144", "144"}, {"165", "165"},
                 {"240", "240"},          {"monitor", fmt("DISPLAY (%.0f HZ)", hz)}, {"uncapped", "UNLIMITED"}};
    o.defaultValue = "monitor";
    o.presetManaged = true;
    add(o);
  }
  {
    Option o;
    o.id = "vsync";
    o.tab = Tab::Display;
    o.section = "FRAME RATE";
    o.kind = Kind::Toggle;
    o.apply = Apply::Live;
    o.label = "VSYNC";
    o.desc = "Wait for the display's refresh before showing a frame: no tearing, and the frame rate "
             "never goes above the refresh rate. Off shows frames at once (tearing where the "
             "display allows).";
    o.perf = "NONE";
    o.xbox = "On";
    o.defaultValue = "on";
    add(o);
  }
  {
    Option o;
    o.id = "brightness";
    o.tab = Tab::Display;
    o.section = "PICTURE";
    o.kind = Kind::Slider;
    o.apply = Apply::Live;
    o.label = "BRIGHTNESS";
    o.desc = "Brightens or darkens the whole picture, after the game's own display curve. 0 is the "
             "game's own look.";
    o.perf = "NONE";
    o.xbox = "0 (the TV's setting)";
    o.lo = -50;
    o.hi = 50;
    o.step = 1;
    o.defaultValue = "0";
    add(o);
  }
  {
    Option o;
    o.id = "gamma";
    o.tab = Tab::Display;
    o.section = "PICTURE";
    o.kind = Kind::Slider;
    o.apply = Apply::Live;
    o.label = "GAMMA";
    o.desc = "Lifts (above 1.00) or deepens (below 1.00) the mid-tones while black and white stay "
             "put, after the game's own display curve. 1.00 is the game's own look.";
    o.perf = "NONE";
    o.xbox = "1.00";
    o.lo = 0.5f;
    o.hi = 1.5f;
    o.step = 0.05f;
    o.decimals = 2;
    o.defaultValue = "1.00";
    add(o);
  }
  {
    Option o;
    o.id = "perf_overlay";
    o.tab = Tab::Display;
    o.section = "PICTURE";
    o.apply = Apply::Live;
    o.label = "PERFORMANCE OVERLAY";
    o.desc = "Shows the frame rate in a corner of the screen. FULL adds the frame time and the "
             "render and output resolutions.";
    o.perf = "NONE";
    o.xbox = "-";
    o.choices = {{"off", "OFF"}, {"fps", "FPS"}, {"full", "FULL"}};
    o.defaultValue = "off";
    add(o);
  }

  // ═══ GRAPHICS ═══
  {
    Option o;
    o.id = "preset";
    o.tab = Tab::Graphics;
    o.section = "QUALITY";
    o.apply = Apply::Live;
    o.label = "PRESET";
    o.desc = "Sets the render resolution, upscaling, frame rate, texture filtering and anti-"
             "aliasing together. ORIGINAL is the Xbox 360 look: 720p, 30 frames per second, the "
             "game's own filtering. Changing any of those settings makes the preset CUSTOM.";
    o.perf = "-";
    o.xbox = "ORIGINAL";
    for (const std::string& p : presetNames()) o.choices.push_back({p, p});
    o.defaultValue = "DEFAULT";
    add(o);
  }
  {
    Option o;
    o.id = "anisotropic";
    o.tab = Tab::Graphics;
    o.section = "TEXTURES";
    o.apply = Apply::Live;
    o.label = "ANISOTROPIC FILTERING";
    o.desc = "Keeps textures sharp on surfaces seen at an angle: streets, rooftops and walls "
             "below you in flight. OFF is the game's own filtering (trilinear).";
    o.perf = "LOW";
    o.xbox = "Off (trilinear)";
    o.choices = {{"off", "OFF (GAME'S OWN)"}, {"2", "2X"}, {"4", "4X"}, {"8", "8X"}, {"16", "16X"}};
    o.defaultValue = "off";
    o.presetManaged = true;
    add(o);
  }
  {
    Option o;
    o.id = "lod_bias";
    o.tab = Tab::Graphics;
    o.section = "TEXTURES";
    o.kind = Kind::Slider;
    o.apply = Apply::Live;
    o.label = "TEXTURE LOD BIAS";
    o.desc = "Which texture detail level is used at a distance. Negative values pick sharper levels "
             "(more shimmer); positive values pick softer ones. 0.00 is the game's own choice.";
    o.perf = "NONE";
    o.xbox = "0.00";
    o.lo = -2;
    o.hi = 2;
    o.step = 0.25f;
    o.decimals = 2;
    o.defaultValue = "0.00";
    add(o);
  }
  {
    Option o;
    o.id = "post_aa";
    o.tab = Tab::Graphics;
    o.section = "ANTI-ALIASING";
    o.apply = Apply::Live;
    o.label = "POST-PROCESS AA";
    o.desc = "Smooths jagged edges on the finished picture for a small cost; it can soften fine "
             "detail a little. "
             "FXAA EXTREME smooths more and softens more. A render resolution above the screen's is the "
             "sharper way to remove jaggies.";
    o.perf = "LOW";
    o.xbox = "Off";
    o.choices = {{"off", "OFF"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA EXTREME"}};
    o.defaultValue = "off";
    o.presetManaged = true;
    add(o);
  }
  {
    Option o;
    o.id = "edge_blur";
    o.tab = Tab::Graphics;
    o.section = "ANTI-ALIASING";
    o.kind = Kind::Toggle;
    o.apply = Apply::Live;
    o.label = "XBOX 360 EDGE BLUR";
    o.desc = "The game's own anti-aliasing: a light blur along high-contrast edges after the tone "
             "map. OFF gives a sharper picture; pair it with FXAA or a higher render resolution.";
    o.perf = "NONE";
    o.xbox = "On";
    o.defaultValue = "on";
    add(o);
  }
  {
    Option o;
    o.id = "bloom";
    o.tab = Tab::Graphics;
    o.section = "POST-PROCESSING";
    o.kind = Kind::Slider;
    o.apply = Apply::Live;
    o.label = "BLOOM";
    o.desc = "The glow around bright lights, the sky and the sun. 100% is the game's own strength; "
             "0% leaves the glow out. The game's tone map is otherwise unchanged.";
    o.perf = "NONE";
    o.xbox = "100%";
    o.lo = 0;
    o.hi = 200;
    o.step = 10;
    o.unit = "%";
    o.defaultValue = "100";
    add(o);
  }

  // ═══ WORLD ═══ (the density lane: the game's own tunables, patched as data on load; the keys
  // live in game_options.ini, integer percent, 25..kDensityMax step 25, default 100; the traffic
  // document is chosen at boot, so a change applies after a restart: kernel/mod_density.hpp)
  for (const auto& e : {std::make_tuple("population_density", "POPULATION DENSITY",
                                        "How many pedestrians walk Metropolis's streets."),
                        std::make_tuple("traffic_density", "TRAFFIC DENSITY",
                                        "How many cars drive and park on its roads.")}) {
    Option o;
    o.id = std::get<0>(e);
    o.tab = Tab::World;
    o.section = "METROPOLIS";
    o.kind = Kind::Slider;
    o.apply = Apply::Restart;
    o.label = std::get<1>(e);
    o.desc = std::string(std::get<2>(e)) + " 100% is the original game; more costs CPU and GPU "
             "time. Applies after a restart.";
    o.perf = "MEDIUM";
    o.xbox = "100%";
    o.lo = 25;
    o.hi = float(kDensityMax);
    o.step = 25;
    o.unit = "%";
    o.defaultValue = "100";
    o.available = availableOf(o.id, false);
    o.hideUnavailable = true;
    add(o);
  }

  // ═══ AUDIO ═══
  {
    Option o;
    o.id = "audio_volume";
    o.tab = Tab::Audio;
    o.section = "OUTPUT";
    o.kind = Kind::Slider;
    o.apply = Apply::Live;
    o.label = "MASTER VOLUME";
    o.desc = "The volume of everything the game plays. Music, effects and speech have their own "
             "sliders in the game's AUDIO OPTIONS.";
    o.perf = "NONE";
    o.xbox = "100%";
    o.lo = 0;
    o.hi = 100;
    o.step = 5;
    o.unit = "%";
    o.defaultValue = "100";
    add(o);
  }
  {
    Option o;
    o.id = "audio_bg_mute";
    o.tab = Tab::Audio;
    o.section = "OUTPUT";
    o.kind = Kind::Toggle;
    o.apply = Apply::Live;
    o.label = "MUTE IN BACKGROUND";
    o.desc = "Silences the game while its window is not the active one.";
    o.perf = "NONE";
    o.xbox = "-";
    o.defaultValue = "off";
    add(o);
  }
  {
    Option o;
    o.id = "audio_device";
    o.tab = Tab::Audio;
    o.section = "OUTPUT";
    o.apply = Apply::Restart;
    o.label = "OUTPUT DEVICE";
    o.desc = "The speakers or headphones the game plays through. DEFAULT follows Windows.";
    o.perf = "NONE";
    o.xbox = "-";
    o.choices.push_back({"default", "DEFAULT"});
    for (const std::string& d : audioDevices()) o.choices.push_back({d, sr::kernel::pcsettings::upper(d)});
    o.defaultValue = "default";
    add(o);
  }

  // ═══ CONTROLS ═══
  if (GOPT::enabled()) {   // lane E L-b: the game's own pad diagram (the retail CONTROLLER OPTIONS screen's)
    Option o;
    o.id = "view_pad_layout";
    o.tab = Tab::Controls;
    o.section = "CONTROLLER";
    o.kind = Kind::Action;
    o.apply = Apply::Live;
    o.label = "VIEW PAD LAYOUT";
    o.desc = "Shows the game's own controller diagrams (GROUND and AIR controls) for the current control scheme.";
    o.perf = "NONE";
    o.xbox = "CONTROLLER OPTIONS";
    o.choices = {{"open", "VIEW"}};
    add(o);
  }
  {
    Option o;
    o.id = "glyphs";
    o.tab = Tab::Controls;
    o.section = "BUTTON PROMPTS";
    o.apply = Apply::Live;
    o.label = "BUTTON PROMPTS";
    o.desc = "The buttons the game's prompts show. AUTO matches the device you used last.";
    o.perf = "NONE";
    o.xbox = "Xbox 360";
    o.choices = {{"auto", "AUTO (MATCH THE DEVICE)"}, {"xbox", "XBOX"},        {"ps4", "PLAYSTATION 4"},
                 {"ps5", "PLAYSTATION 5"},           {"switch", "NINTENDO SWITCH"}, {"keyboard", "KEYBOARD AND MOUSE"}};
    o.defaultValue = "auto";
    add(o);
  }
  {
    Option o;
    o.id = "mouse_look";
    o.tab = Tab::Controls;
    o.section = "MOUSE";
    o.kind = Kind::Toggle;
    o.apply = Apply::Live;
    o.label = "MOUSE LOOK";
    o.desc = "Turns the camera with the mouse in gameplay.";
    o.perf = "NONE";
    o.xbox = "-";
    o.defaultValue = "on";
    add(o);
  }
  {
    Option o;
    o.id = "mouse_sensitivity";
    o.tab = Tab::Controls;
    o.section = "MOUSE";
    o.kind = Kind::Slider;
    o.apply = Apply::Live;
    o.label = "MOUSE SENSITIVITY";
    o.desc = "How far the camera turns for a mouse movement. 1.00 is the standard rate.";
    o.perf = "NONE";
    o.xbox = "-";
    o.lo = 0.1f;
    o.hi = 5.0f;
    o.step = 0.05f;
    o.decimals = 2;
    o.defaultValue = "1.00";
    add(o);
  }
  for (const auto& e : {std::make_tuple("mouse_invert_x", "INVERT MOUSE X"), std::make_tuple("mouse_invert_y", "INVERT MOUSE Y")}) {
    Option o;
    o.id = std::get<0>(e);
    o.tab = Tab::Controls;
    o.section = "MOUSE";
    o.kind = Kind::Toggle;
    o.apply = Apply::Live;
    o.label = std::get<1>(e);
    o.desc = "Reverses the mouse on this axis, on top of the game's own camera inversion option.";
    o.perf = "NONE";
    o.xbox = "-";
    o.defaultValue = "off";
    add(o);
  }
  // iter-466 controls2: THE REMAPPER - one row per action (docs/PC_CONTROLS.md 7). A key IS the
  // game's action: it drives the game's own input commands (the action layer), never pad state.
  {
    namespace PC = sr::kernel::pccontrols;
    const PC::Bindings dflt = PC::defaultBindings();
    static const char* const kGroups[] = {"MOVEMENT", "ACTIONS", "POWERS", "CAMERA", "GAME", "MENUS"};
    for (const char* grp : kGroups)
      for (int a = 0; a < PC::kActionCount; ++a) {
        const PC::Action act = PC::Action(a);
        if (std::string(PC::actionGroup(act)) != grp) continue;
        Option o;
        o.id = std::string("bind_") + PC::actionName(act);
        o.tab = Tab::Controls;
        o.section = std::string("KEYS - ") + grp;
        o.kind = Kind::Binding;
        o.apply = Apply::Live;
        o.label = PC::actionLabel(act);
        o.desc = std::string(PC::actionDesc(act)) + " Up to four keys or mouse buttons.";
        o.perf = "NONE";
        o.xbox = PC::actionControl(act);
        o.defaultValue = PC::bindingText(dflt, act);
        add(o);
      }
  }
  // ═══ CHEATS ═══ (lane E L-b: the game's own cheats, kernel/cheats_menu.hpp; the screen lists the tab in
  // game only)
  if (GOPT::enabled())
    for (const CheatRow& c : cheatsMenuRows()) {
      Option o;
      o.id = c.id;
      o.tab = Tab::Cheats;
      o.section = c.section;
      o.kind = Kind::Action;
      o.apply = Apply::Live;
      o.label = c.label;
      o.desc = c.desc;
      o.perf = "NONE";
      o.xbox = "the button code";
      o.choices = {{"run", c.verb}};
      add(o);
    }
  return v;
}

int bindingAction(const std::string& id) {
  if (id.compare(0, 5, "bind_") != 0) return -1;
  namespace PC = sr::kernel::pccontrols;
  for (int a = 0; a < PC::kActionCount; ++a)
    if (id.compare(5, std::string::npos, PC::actionName(PC::Action(a))) == 0) return a;
  return -1;
}

const Option* find(const std::vector<Option>& cat, const std::string& id) {
  for (const Option& o : cat)
    if (o.id == id) return &o;
  return nullptr;
}

std::string upper(std::string s) {
  for (char& c : s) c = char((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
  return s;
}

// ── values ──────────────────────────────────────────────────────────────────────────────
std::string storedValue(const std::string& id) {
  if (id == "preset") return {};
  if (GOPT::isGameOption(id) || GOPT::isAutosave(id)) return GOPT::value(id);   // the game's live value
  if (isGfxopt(id)) return GO::formatKey(GO::fromIniText(readText(settingsFile("display.ini"))), id);
  if (id == "display_mode") {
    const std::string s = lower(RS::storedSetting("display_mode"));
    if (!s.empty()) return RS::modeName(modeOf(s == "fullscreen" || s == "borderless_fullscreen" ? "borderless" : s));
    return RS::modeName(RS::liveDisplay().mode);
  }
  if (id == "display_index") return storedOr("display_index", std::to_string(currentDisplay()));
  if (id == "resolution") return resolutionFor(modeOf(storedValue("display_mode")));
  if (id == "output_refresh") return storedOr("output_refresh", "0");
  if (id == "res_scale") {
    bool a = false;
    uint32_t x = 1, y = 1;
    const std::string s = RS::storedSetting("res_scale");
    if (!s.empty() && RS::parseScale(s.c_str(), a, x, y)) return a ? "auto" : std::to_string(std::max(x, y));
    const RS::Display& d = RS::display();
    return d.scaleAuto ? "auto" : std::to_string(std::max(d.scaleX, d.scaleY));
  }
  if (id == "target_fps") return fpsNorm(HF::playerTargetFps());
  if (id == "vsync") return HF::playerVsync() ? "on" : "off";
  if (const int a = bindingAction(id); a >= 0) {
    namespace PC = sr::kernel::pccontrols;
    const std::string v = iniGet(readText(settingsFile("input.ini")), "controls", PC::actionName(PC::Action(a)));
    PC::Bindings b = PC::defaultBindings();
    uint16_t ins[PC::kInputsPerAction];
    if (!v.empty() && PC::parseBindingText(v, ins))
      for (int k = 0; k < PC::kInputsPerAction; ++k) b.in[a][k] = ins[k];
    return PC::bindingText(b, PC::Action(a));
  }
  if (const InKey* k = inputKey(id)) {
    std::string v = iniGet(readText(settingsFile("input.ini")), k->section, k->key);
    if (v.empty()) return k->dflt;
    if (id == "glyphs") return lower(v);
    if (id == "mouse_sensitivity") return fmt("%.2f", std::atof(v.c_str()));
    return onOff(v);
  }
  if (isDensity(id)) {
    const std::string v = iniGet(readText(settingsFile("game_options.ini")), "", id);
    const int n = v.empty() ? 100 : std::atoi(v.c_str());
    return std::to_string(std::clamp(n, 25, kDensityMax));
  }
  if (const char* d = plainDefault(id)) {
    const std::string v = RS::storedSetting(id);
    if (v.empty()) return d;
    const std::string s = lower(v);
    return (std::string(d) == "on" || std::string(d) == "off") ? onOff(s) : s;
  }
  return {};
}

std::string value(const std::string& id) {
  {
    std::lock_guard<std::mutex> lk(g_prevMu);
    auto it = g_preview.find(id);
    if (it != g_preview.end()) return it->second;
  }
  if (id == "preset") return matchPreset([](const std::string& k) { return value(k); });
  return storedValue(id);
}

std::string bootValue(const std::string& id) {
  if (id == "res_scale") {
    const RS::Display& d = RS::display();
    return d.scaleAuto ? "auto" : std::to_string(std::max(d.scaleX, d.scaleY));
  }
  static std::map<std::string, std::string> s_boot;
  static std::mutex s_mu;
  std::lock_guard<std::mutex> lk(s_mu);
  auto it = s_boot.find(id);
  if (it == s_boot.end()) it = s_boot.emplace(id, storedValue(id)).first;
  return it->second;
}

namespace {
// Apply one value live (no store). For Live options (and Confirm ones on APPLY).
bool applyLive(const std::string& id, const std::string& v, std::string* err) {
  if (isGfxopt(id)) return GO::set(id, v, err, false);
  if (GOPT::isGameOption(id) || GOPT::isAutosave(id)) return GOPT::preview(id, v);   // the game's own setters
  if (id == "target_fps") return HF::applyLive(v.c_str(), value("vsync") == "on");
  if (id == "vsync") return HF::applyLive(value("target_fps").c_str(), v == "on");
  if (id == "audio_volume" || id == "audio_bg_mute") {
    applyAudioFromValues();
    return true;
  }
  if (id == "perf_overlay") {
    g_perfOverlay.store(v == "full" ? 2 : v == "fps" ? 1 : 0, std::memory_order_relaxed);
    return true;
  }
  if (id == "display_mode" || id == "resolution" || id == "display_index" || id == "output_refresh") {
    requestDisplay();
    return true;
  }
  runLiveHook(id, v);
  return true;
}

bool validate(const Option& o, const std::string& v) {
  if (o.kind == Kind::Choice) {
    for (const Choice& c : o.choices)
      if (c.value == v) return true;
    return o.id == "resolution" || o.id == "target_fps" || o.id == "display_index";   // free sizes / rates
  }
  if (o.kind == Kind::Toggle) return v == "on" || v == "off";
  if (o.kind == Kind::Binding) {
    uint16_t ins[sr::kernel::pccontrols::kInputsPerAction];
    return sr::kernel::pccontrols::parseBindingText(v, ins);
  }
  if (o.kind == Kind::Slider) {
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end && *end == '\0' && f >= o.lo - 1e-4f && f <= o.hi + 1e-4f;
  }
  return true;
}
}  // namespace

bool preview(const std::string& id, const std::string& v, std::string* err) {
  const std::vector<Option> cat = catalogue();
  const Option* o = find(cat, id);
  if (!o || o->kind == Kind::Action) return false;
  if (id == "preset") {
    for (const auto& kv : presetValues(v)) preview(kv.first, kv.second, err);
    return true;
  }
  if (!validate(*o, v)) {
    if (err) *err = "bad value";
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(g_prevMu);
    g_preview[id] = v;
  }
  if (o->apply == Apply::Live) applyLive(id, v, err);
  return true;
}

void clearPreview(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_prevMu);
  g_preview.erase(id);
}

bool commit(const std::string& id, const std::string& v, std::string* err) {
  const std::vector<Option> cat = catalogue();
  const Option* o = find(cat, id);
  if (!o) {
    if (err) *err = "unknown setting";
    return false;
  }
  if (o->kind == Kind::Action) return runAction(id, err);
  if (id == "preset") {
    bool ok = true;
    for (const auto& kv : presetValues(v)) ok = commit(kv.first, kv.second, err) && ok;
    return ok;
  }
  if (!validate(*o, v)) {
    if (err) *err = "bad value";
    return false;
  }
  bool ok = true;
  const bool store = sr::kernel::automation::portFeaturesOn();
  if (isGfxopt(id)) {
    ok = GO::set(id, v, err, true);
  } else if (GOPT::isGameOption(id) || GOPT::isAutosave(id)) {
    ok = GOPT::commit(id, v, store, err);   // the game's own setters, then its own capture into the store
  } else if (id == "display_mode" || id == "display_index" || id == "output_refresh" || id == "res_scale") {
    ok = !store || RS::setSetting(id, v);
  } else if (id == "resolution") {
    uint32_t w = 0, h = 0;
    ok = parseWxH(v, w, h);
    if (ok && store) {
      const bool win = modeOf(value("display_mode")) == RS::DisplayMode::Windowed;
      ok = RS::setSetting(win ? "window_width" : "output_width", std::to_string(w)) &&
           RS::setSetting(win ? "window_height" : "output_height", std::to_string(h));
    }
  } else if (id == "target_fps") {
    ok = !store || HF::setPlayerTargetFps(v.c_str());
  } else if (id == "vsync") {
    ok = !store || HF::setPlayerVsync(v == "on");
  } else if (const int ba = bindingAction(id); ba >= 0) {
    if (store) {
      const fs::path p = settingsFile("input.ini");
      std::string t = readText(p);
      if (t.empty()) t = sr::kernel::pccontrols::defaultIni();
      t = iniSet(t, "", "version", "3");
      ok = writeText(p, iniSet(t, "controls", sr::kernel::pccontrols::actionName(sr::kernel::pccontrols::Action(ba)), v));
    }
  } else if (const InKey* k = inputKey(id)) {
    if (store) {
      const fs::path p = settingsFile("input.ini");
      ok = writeText(p, iniSet(readText(p), k->section, k->key, v == "on" ? "true" : v == "off" ? "false" : v));
    }
  } else if (plainDefault(id)) {
    if (store) {
      const fs::path p = settingsFile("display.ini");
      ok = writeText(p, iniSet(readText(p), "", id, v));
    }
  } else if (isDensity(id)) {
    if (store) {
      const fs::path p = settingsFile("game_options.ini");
      ok = writeText(p, iniSet(readText(p), "", id, std::to_string(int(std::lround(std::atof(v.c_str()))))));
    }
  }
  if (!ok) {
    if (err && err->empty()) *err = "could not be saved";
    return false;
  }
  clearPreview(id);
  // apply: live options at once; display rows (Confirm) at once too; restart ones wait
  if (o->apply != Apply::Restart) applyLive(id, v, err);
  if (inputKey(id) || bindingAction(id) >= 0) {
    sr::kernel::pccontrols::reload();
    sr::kernel::hostinput::reloadGlyphPin();
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] %s = %s (%s%s)", id.c_str(), v.c_str(),
              o->apply == Apply::Restart ? "next start" : "applied", store ? ", stored" : ", not stored: port features off");
  return true;
}

void revertPreviews() {
  std::map<std::string, std::string> p;
  {
    std::lock_guard<std::mutex> lk(g_prevMu);
    p.swap(g_preview);
  }
  for (const auto& kv : p) {
    const std::vector<Option> cat = catalogue();
    const Option* o = find(cat, kv.first);
    if (o && o->apply == Apply::Live) applyLive(kv.first, storedValue(kv.first), nullptr);
  }
}

// ── presets ─────────────────────────────────────────────────────────────────────────────
const std::vector<std::string>& presetNames() {
  static const std::vector<std::string> v = {"ORIGINAL", "LOW", "MEDIUM", "HIGH", "ULTRA"};
  return v;
}

std::vector<std::pair<std::string, std::string>> presetValuesAll(const std::string& preset);

// A preset sets only the rows that exist: upscale_filter / sharpness wait for the res4k lane.
std::vector<std::pair<std::string, std::string>> presetValues(const std::string& preset) {
  std::vector<std::pair<std::string, std::string>> out;
  for (auto& kv : presetValuesAll(preset))
    if ((kv.first != "upscale_filter" && kv.first != "sharpness") || availableOf(kv.first, false))
      out.push_back(std::move(kv));
  return out;
}

std::vector<std::pair<std::string, std::string>> presetValuesAll(const std::string& preset) {
  uint32_t ow = 0, oh = 0;
  parseWxH(value("resolution"), ow, oh);
  const uint32_t a = RS::autoScaleFor(ow, oh);
  const std::string p = upper(preset);
  if (p == "ORIGINAL")
    return {{"res_scale", "1"}, {"upscale_filter", "bilinear"}, {"sharpness", "0"}, {"target_fps", "30"},
            {"anisotropic", "off"}, {"post_aa", "off"}};
  if (p == "LOW")
    return {{"res_scale", "1"}, {"upscale_filter", "bilinear"}, {"sharpness", "0"}, {"target_fps", "60"},
            {"anisotropic", "off"}, {"post_aa", "off"}};
  if (p == "MEDIUM")
    return {{"res_scale", std::to_string(std::min<uint32_t>(a, 2))}, {"upscale_filter", "bilinear"},
            {"sharpness", "0"}, {"target_fps", "monitor"}, {"anisotropic", "4"}, {"post_aa", "fxaa"}};
  if (p == "HIGH")
    return {{"res_scale", "auto"}, {"upscale_filter", "fsr"}, {"sharpness", "20"}, {"target_fps", "monitor"},
            {"anisotropic", "16"}, {"post_aa", "fxaa"}};
  if (p == "ULTRA")
    return {{"res_scale", std::to_string(std::min<uint32_t>(a + 1, RS::kMaxScale))},
            {"upscale_filter", "cas"}, {"sharpness", "30"}, {"target_fps", "monitor"}, {"anisotropic", "16"},
            {"post_aa", "fxaa_extreme"}};
  return {};
}

std::string matchPreset(const std::function<std::string(const std::string&)>& valueOf) {
  for (const std::string& p : presetNames()) {
    bool all = true;
    for (const auto& kv : presetValues(p))
      if (valueOf(kv.first) != kv.second) {
        all = false;
        break;
      }
    if (all) return p;
  }
  // every managed value at its default -> DEFAULT
  static const std::vector<std::pair<std::string, std::string>> dflt = {
      {"res_scale", "auto"}, {"upscale_filter", "bilinear"}, {"sharpness", "0"}, {"target_fps", "monitor"},
      {"anisotropic", "off"}, {"post_aa", "off"}};
  for (const auto& kv : dflt)
    if (valueOf(kv.first) != kv.second) return "CUSTOM";
  return "DEFAULT";
}

// ── registry ────────────────────────────────────────────────────────────────────────────
void setAvailable(const std::string& id, bool available) {
  std::lock_guard<std::mutex> lk(g_regMu);
  g_avail[id] = available;
}
void onLive(const std::string& id, std::function<void(const std::string&)> fn) {
  std::lock_guard<std::mutex> lk(g_regMu);
  g_live[id] = std::move(fn);
}
void onAction(const std::string& id, std::function<std::string()> fn) {
  std::lock_guard<std::mutex> lk(g_regMu);
  g_actions[id] = std::move(fn);
}
bool runAction(const std::string& id, std::string* msg) {
  // lane E L-b: the CHEATS tab's rows and VIEW PAD LAYOUT belong to their owners
  if (id.compare(0, 6, "cheat_") == 0 || id == "view_pad_layout" || id == "dev_tools") {
    std::string m;
    const bool ok = id == "view_pad_layout" ? GOPT::requestPadLayout(&m)
                    : id == "dev_tools"     ? GOPT::requestDevTools(&m)
                                            : cheatsMenuRun(id, &m);
    if (msg) *msg = m;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] action %s: %s", id.c_str(), m.c_str());
    return ok;
  }
  std::function<std::string()> fn;
  {
    std::lock_guard<std::mutex> lk(g_regMu);
    auto it = g_actions.find(id);
    if (it != g_actions.end()) fn = it->second;
  }
  if (!fn) {
    if (msg) *msg = "not available yet";
    return false;
  }
  const std::string m = fn();
  if (msg) *msg = m;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_SETTINGS] action %s: %s", id.c_str(), m.c_str());
  return true;
}

// ── displays / audio devices ─────────────────────────────────────────────────────────────
void noteDisplays(const std::vector<DisplayInfo>& ds, int current) {
  std::lock_guard<std::mutex> lk(g_dispMu);
  g_displays = ds;
  for (DisplayInfo& d : g_displays)
    std::sort(d.modes.begin(), d.modes.end(), [](const DisplayMode& a, const DisplayMode& b) {
      const uint64_t pa = uint64_t(a.w) * a.h, pb = uint64_t(b.w) * b.h;
      return pa != pb ? pa > pb : a.hz > b.hz;
    });
  g_curDisplay = current;
}
std::vector<DisplayInfo> displays() {
  std::lock_guard<std::mutex> lk(g_dispMu);
  return g_displays;
}
int currentDisplay() {
  std::lock_guard<std::mutex> lk(g_dispMu);
  return g_curDisplay;
}

namespace {
std::mutex g_audMu;
std::vector<std::string> g_audDevs;
}  // namespace
void noteAudioDevices(const std::vector<std::string>& names) {
  std::lock_guard<std::mutex> lk(g_audMu);
  g_audDevs = names;
}
std::vector<std::string> audioDevices() {
  std::lock_guard<std::mutex> lk(g_audMu);
  return g_audDevs;
}
std::string audioDeviceStored() {
  const std::string v = RS::storedSetting("audio_device");
  return v.empty() || lower(v) == "default" ? std::string() : v;
}
std::string audioDeviceSetting() {
  // Lane E E0: players only, like audioGain's stored volume. An automated run (SR_INSTANCE_ID) never opens the
  // stored device: a seeded SR_CONTENT_DIR can carry the owner's display.ini. Read fresh, not the cached
  // portFeaturesOn(), so the difftest drives both worlds in one process.
  if (!sr::kernel::automation::portFeaturesFor(std::getenv("SR_INSTANCE_ID"), std::getenv("SR_PORT_FEATURES")))
    return {};
  return audioDeviceStored();
}

float audioGain(bool windowFocused) {
  int vol = g_volume.load(std::memory_order_relaxed);
  int mute = g_bgMute.load(std::memory_order_relaxed);
  if (vol < 0 || mute < 0) {
    // first call: the stored values (players); an automated run keeps 100 / off
    if (sr::kernel::automation::portFeaturesOn()) {
      vol = std::clamp(std::atoi(storedValue("audio_volume").c_str()), 0, 100);
      mute = storedValue("audio_bg_mute") == "on" ? 1 : 0;
    } else {
      vol = 100;
      mute = 0;
    }
    g_volume.store(vol, std::memory_order_relaxed);
    g_bgMute.store(mute, std::memory_order_relaxed);
  }
  if (mute && !windowFocused) return 0.0f;
  return float(vol) / 100.0f;
}

int perfOverlayMode() {
  int m = g_perfOverlay.load(std::memory_order_relaxed);
  if (m < 0) {
    const std::string v = sr::kernel::automation::portFeaturesOn() ? storedValue("perf_overlay") : "off";
    m = v == "full" ? 2 : v == "fps" ? 1 : 0;
    g_perfOverlay.store(m, std::memory_order_relaxed);
  }
  return m;
}

// ── self test ───────────────────────────────────────────────────────────────────────────
int selfTest() {
  int fail = 0;
  auto check = [&](bool c, const char* what) {
    if (!c) {
      std::fprintf(stderr, "[pcsettings selftest] FAIL %s\n", what);
      fail++;
    }
  };
  check(renderResolutionLabel(3, 3840, 2160) == "3840 x 2160 (NATIVE)", "label native");
  check(renderResolutionLabel(4, 3840, 2160) == "5120 x 2880 (SUPERSAMPLED)", "label ss");
  check(renderResolutionLabel(2, 3840, 2160) == "2560 x 1440 (PERFORMANCE)", "label perf");
  check(iniGet("glyphs = ps5\n[mouse]\nlook = false\n", "mouse", "look") == "false", "iniGet section");
  check(iniGet("glyphs = ps5\n[mouse]\nlook = false\n", "", "glyphs") == "ps5", "iniGet flat");
  check(iniSet("version = 2\n[mouse]\nlook = true\n", "mouse", "look", "false") == "version = 2\n[mouse]\nlook = false\n",
        "iniSet replace in section");
  check(iniSet("version = 2\n[mouse]\nlook = true\n", "mouse", "invert_y", "true") ==
            "version = 2\n[mouse]\nlook = true\ninvert_y = true\n",
        "iniSet append in section");
  check(iniSet("version = 2\n[controls]\nJump = Space\n", "", "glyphs", "xbox") ==
            "version = 2\nglyphs = xbox\n[controls]\nJump = Space\n",
        "iniSet flat key before the first section");
  check(iniSet("a = 1\r\n", "mouse", "look", "false") == "a = 1\r\n\r\n[mouse]\r\nlook = false\r\n" ||
            iniSet("a = 1\r\n", "mouse", "look", "false").find("[mouse]") != std::string::npos,
        "iniSet new section");
  check(iniSet("", "", "audio_volume", "80") == "audio_volume = 80\n", "iniSet empty");
  return fail;
}

}  // namespace pcsettings
}  // namespace kernel
}  // namespace sr
