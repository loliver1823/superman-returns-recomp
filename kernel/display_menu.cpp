// iter-465 polish: DISPLAY OPTIONS - the host half. Contract: display_menu.hpp,
// docs/CUSTOM_FRONTEND.md 5.9, docs/RESOLUTION.md, docs/HIGH_FRAMERATE.md 9.
//
// Charter: PORT FEATURE UI. This file never reads or writes guest memory and never calls guest
// code: it reads and writes settings/display.ini through the two features' own setters
// (gfx/res_scale.hpp setSetting, kernel/highfps.hpp setPlayerTargetFps / setPlayerVsync) and
// queues their live application (resscale::requestLiveDisplay for the window, applied on the
// SDL thread; highfps::applyLive for the frame rate).
#include "display_menu.hpp"

#include "highfps.hpp"
#include "host_automation.hpp"
#include "host_pages.hpp"
#include "mod_menu.hpp"
#include "mod_library.hpp"   // iter-466 gfx: the PC SETTINGS overlay replaces these pages
#include "game_options.hpp"  // lane E L-b: the GAME tab calls its opener
#include "../app/log.hpp"
#include "../gfx/res_scale.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

namespace {

using Row = HostPageRow;
using Page = HostPage;
using Out = HostPageOut;
namespace RS = sr::gfx::resscale;
namespace HF = sr::kernel::highfps;

std::string g_ctx = "FE";   // "FE" (front end) | "IG" (pause menu)

std::string lower(std::string s) {
  for (char& c : s) c = char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  return s;
}

int cycle(int i, int n, int delta) { return ((i + delta) % n + n) % n; }

// ---- RESOLUTION ----
const char* const kScales[] = {"auto", "1", "2", "3", "4"};
constexpr int kScaleN = 5;

// The value the store holds, normalised to one of kScales ("" = absent).
std::string storedScale() {
  bool isAuto = false;
  uint32_t x = 1, y = 1;
  const std::string v = RS::storedSetting("res_scale");
  if (v.empty() || !RS::parseScale(v.c_str(), isAuto, x, y)) return {};
  if (isAuto) return "auto";
  return std::to_string(std::min<uint32_t>(std::max(x, y), 4u));
}

// What this process started with (the env / ini / port-feature default, before the clamp).
std::string bootScale() {
  const RS::Display& d = RS::display();
  if (d.scaleAuto) return "auto";
  return std::to_string(std::min<uint32_t>(std::max(d.scaleX, d.scaleY), 4u));
}

// The setting shown and cycled: the store's, else the start-up one.
std::string scaleSetting() {
  const std::string s = storedScale();
  return s.empty() ? bootScale() : s;
}

// The scale a restart would give (auto: for the current output size).
uint32_t scaleAfterRestart(const std::string& s) {
  if (s != "auto") return uint32_t(std::atoi(s.c_str()));
  const RS::LiveDisplay ld = RS::liveDisplay();
  uint32_t w = ld.windowW, h = ld.windowH;
  if (ld.mode != RS::DisplayMode::Windowed) {
    RS::desktopSize(w, h);
    if (ld.mode == RS::DisplayMode::Exclusive && ld.outputW && ld.outputH) {
      w = ld.outputW;
      h = ld.outputH;
    }
  }
  return RS::autoScaleFor(w, h);
}

// A restart is needed when the setting differs from the one this process started with AND it
// would give another scale (AUTO -> 1X on a 720p window changes nothing on screen).
bool scalePending() {
  const std::string s = scaleSetting();
  return s != bootScale() && scaleAfterRestart(s) != RS::get().maxAxis();
}

std::string scaleLabel() {
  const std::string s = scaleSetting();
  const bool pend = scalePending();
  const std::string head = s == "auto" ? std::string("RESOLUTION: AUTO") : "RESOLUTION: " + s + "X";
  if (pend) return head + " - RESTART";
  const uint32_t n = RS::get().maxAxis();   // in effect now (after the device clamp)
  char b[64];
  std::snprintf(b, sizeof b, "%s (%uX%u)", head.c_str(), 1280u * n, 720u * n);
  return b;
}

// ---- DISPLAY (mode) ----
RS::DisplayMode modeSetting() {
  const std::string v = lower(RS::storedSetting("display_mode"));
  if (v == "windowed" || v == "window") return RS::DisplayMode::Windowed;
  if (v == "borderless" || v == "fullscreen" || v == "borderless_fullscreen") return RS::DisplayMode::Borderless;
  if (v == "exclusive" || v == "exclusive_fullscreen") return RS::DisplayMode::Exclusive;
  return RS::liveDisplay().mode;
}

const char* modeLabel(RS::DisplayMode m) {
  switch (m) {
    case RS::DisplayMode::Borderless: return "DISPLAY: BORDERLESS FULLSCREEN";
    case RS::DisplayMode::Exclusive: return "DISPLAY: EXCLUSIVE FULLSCREEN";
    default: return "DISPLAY: WINDOWED";
  }
}

// ---- WINDOW SIZE / OUTPUT ----
struct Size {
  uint32_t w, h;
};
const Size kSizes[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};

std::vector<Size> sizesThatFit(bool withDesktop) {
  uint32_t dw = 0, dh = 0;
  RS::desktopSize(dw, dh);
  std::vector<Size> v;
  if (withDesktop) v.push_back({0, 0});
  for (const Size& s : kSizes)
    if (!dw || !dh || (s.w <= dw && s.h <= dh)) v.push_back(s);
  if (v.empty()) v.push_back(kSizes[0]);
  return v;
}

Size windowSetting() {
  const uint32_t w = uint32_t(std::strtoul(RS::storedSetting("window_width").c_str(), nullptr, 10));
  const uint32_t h = uint32_t(std::strtoul(RS::storedSetting("window_height").c_str(), nullptr, 10));
  if (w >= 320 && h >= 200) return {w, h};
  const RS::LiveDisplay ld = RS::liveDisplay();
  return {ld.windowW, ld.windowH};
}

Size outputSetting() {
  const uint32_t w = uint32_t(std::strtoul(RS::storedSetting("output_width").c_str(), nullptr, 10));
  const uint32_t h = uint32_t(std::strtoul(RS::storedSetting("output_height").c_str(), nullptr, 10));
  if (w >= 320 && h >= 200) return {w, h};
  return {0, 0};
}

std::string sizeText(Size s) {
  if (!s.w || !s.h) {
    uint32_t dw = 0, dh = 0;
    RS::desktopSize(dw, dh);
    char b[48];
    if (dw && dh) std::snprintf(b, sizeof b, "DESKTOP (%uX%u)", dw, dh);
    else std::snprintf(b, sizeof b, "DESKTOP");
    return b;
  }
  char b[32];
  std::snprintf(b, sizeof b, "%uX%u", s.w, s.h);
  return b;
}

// ---- FRAME RATE / VSYNC ----
const char* const kFps[] = {"30", "60", "120", "monitor", "uncapped"};
constexpr int kFpsN = 5;

std::string fpsSetting() {
  std::string v = lower(HF::playerTargetFps());
  if (v.empty()) v = HF::kPlayerDefault;
  if (v == "unlimited" || v == "max") v = "uncapped";
  if (v == "1" || v == "refresh" || v == "vsync") v = "monitor";
  return v;
}

std::string fpsLabel(const std::string& v) {
  char b[64];
  if (v == "monitor")
    std::snprintf(b, sizeof b, "FRAME RATE: DISPLAY (%.0f HZ)", HF::config().displayHz);
  else if (v == "uncapped")
    std::snprintf(b, sizeof b, "FRAME RATE: UNLIMITED");
  else if (v == "30")
    std::snprintf(b, sizeof b, "FRAME RATE: 30 (CONSOLE)");
  else
    std::snprintf(b, sizeof b, "FRAME RATE: %s", v.c_str());
  return b;
}

void applyWindow(RS::DisplayMode m, Size win, Size out) {
  RS::LiveDisplay r;
  r.mode = m;
  r.windowW = win.w;
  r.windowH = win.h;
  r.outputW = out.w;
  r.outputH = out.h;
  RS::requestLiveDisplay(r);
}

void logChange(const char* what, const std::string& v) {
  SR_LOG_BOOT(sr::LogSeverity::Info, "[DISPLAY_MENU] %s -> %s (%s)", what, v.c_str(), g_ctx.c_str());
}

// ---- the pages ----
Page buildPage(const std::string& id);

Page pageDisplay() {
  Page p;
  p.id = "display";
  p.title = "DISPLAY OPTIONS";
  std::string body = "LEFT and RIGHT change a setting at once.";
  if (scalePending()) body += "\nThe new RESOLUTION needs a restart: press A on it.";
  else body += "\nRESOLUTION applies after a restart.";
  if (fpsSetting() != "30" && fpsSetting() != "monitor" && HF::playerVsync())
    body += "\nVSYNC ON caps the frame rate at the display's refresh.";
  p.body = body;

  // 0 RESOLUTION
  Row res;
  res.label = scaleLabel();
  res.kind = DevMenuKind::Toggle;
  auto resAdjust = [](Out& o, int delta) {
    const std::string cur = scaleSetting();
    int i = 0;
    for (int k = 0; k < kScaleN; ++k)
      if (cur == kScales[k]) i = k;
    const std::string next = kScales[cycle(i, kScaleN, delta)];
    const bool wasPending = scalePending();
    RS::setSetting("res_scale", next);
    logChange("res_scale", next);
    o.page = "display";
    o.focus = 0;
    o.action = wasPending != scalePending() ? 1 : 2;   // the body names a pending restart
  };
  res.adjust = resAdjust;
  res.press = [resAdjust](Out& o) {
    if (scalePending()) {
      o.page = "displayrestart";
      o.focus = 0;
      return;
    }
    resAdjust(o, 1);
  };
  p.rows.push_back(res);

  // 1 DISPLAY mode
  Row mode;
  mode.label = modeLabel(modeSetting());
  mode.kind = DevMenuKind::Toggle;
  mode.adjust = [](Out& o, int delta) {
    const RS::DisplayMode m = RS::DisplayMode(cycle(int(modeSetting()), 3, delta));
    RS::setSetting("display_mode", RS::modeName(m));
    logChange("display_mode", RS::modeName(m));
    applyWindow(m, windowSetting(), outputSetting());
    o.page = "display";
    o.focus = 1;
    o.action = 2;
  };
  mode.press = [adj = mode.adjust](Out& o) { adj(o, 1); };
  p.rows.push_back(mode);

  // 2 FRAME RATE
  Row fps;
  fps.label = fpsLabel(fpsSetting());
  fps.kind = DevMenuKind::Toggle;
  fps.adjust = [](Out& o, int delta) {
    const std::string cur = fpsSetting();
    int i = 3;   // unknown values (e.g. 144 typed by hand) step from DISPLAY
    for (int k = 0; k < kFpsN; ++k)
      if (cur == kFps[k]) i = k;
    const std::string next = kFps[cycle(i, kFpsN, delta)];
    HF::setPlayerTargetFps(next.c_str());
    HF::applyLive(next.c_str(), HF::playerVsync());
    logChange("target_fps", next);
    o.page = "display";
    o.focus = 2;
    o.action = 1;   // the body's VSYNC note depends on it
  };
  fps.press = [adj = fps.adjust](Out& o) { adj(o, 1); };
  p.rows.push_back(fps);

  // 3 VSYNC
  Row vs;
  vs.label = HF::playerVsync() ? "VSYNC: ON" : "VSYNC: OFF";
  vs.kind = DevMenuKind::Toggle;
  vs.adjust = [](Out& o, int) {
    const bool on = !HF::playerVsync();
    HF::setPlayerVsync(on);
    HF::applyLive(fpsSetting().c_str(), on);
    logChange("vsync", on ? "on" : "off");
    o.page = "display";
    o.focus = 3;
    o.action = 1;
  };
  vs.press = [adj = vs.adjust](Out& o) { adj(o, 1); };
  p.rows.push_back(vs);

  // 4 WINDOW SIZE (windowed) / OUTPUT (exclusive) / the desktop (borderless, a readout)
  Row sz;
  const RS::DisplayMode m = modeSetting();
  if (m == RS::DisplayMode::Borderless) {
    sz.label = "OUTPUT: " + sizeText({0, 0});
    sz.kind = DevMenuKind::Readout;
    sz.press = [](Out& o) {
      o.page = "display";
      o.focus = 4;
      o.action = 2;
    };
  } else {
    const bool excl = m == RS::DisplayMode::Exclusive;
    const Size cur = excl ? outputSetting() : windowSetting();
    sz.label = std::string(excl ? "OUTPUT: " : "WINDOW SIZE: ") + sizeText(cur);
    sz.kind = DevMenuKind::Toggle;
    sz.adjust = [excl](Out& o, int delta) {
      const std::vector<Size> v = sizesThatFit(excl);
      const Size cur = excl ? outputSetting() : windowSetting();
      // the preset at or just past the current size, then step
      int i = -1;
      for (int k = 0; k < int(v.size()); ++k)
        if (v[k].w == cur.w && v[k].h == cur.h) i = k;
      int next;
      if (i >= 0) {
        next = cycle(i, int(v.size()), delta);
      } else {
        next = delta > 0 ? int(v.size()) - 1 : 0;
        for (int k = 0; k < int(v.size()); ++k)
          if (uint64_t(v[k].w) * v[k].h > uint64_t(cur.w) * cur.h) {
            next = delta > 0 ? k : std::max(0, k - 1);
            break;
          }
      }
      const Size s = v[size_t(next)];
      if (excl) {
        RS::setSetting("output_width", std::to_string(s.w));
        RS::setSetting("output_height", std::to_string(s.h));
        logChange("output", sizeText(s));
        applyWindow(RS::DisplayMode::Exclusive, windowSetting(), s);
      } else {
        RS::setSetting("window_width", std::to_string(s.w));
        RS::setSetting("window_height", std::to_string(s.h));
        logChange("window", sizeText(s));
        applyWindow(RS::DisplayMode::Windowed, s, outputSetting());
      }
      o.page = "display";
      o.focus = 4;
      o.action = 2;
    };
    sz.press = [adj = sz.adjust](Out& o) { adj(o, 1); };
  }
  p.rows.push_back(sz);
  p.back = [](Out& o) { o.action = 0; };
  return p;
}

Page pageRestart() {
  Page p;
  p.id = "displayrestart";
  p.title = "RESTART";
  p.body = g_ctx == "IG"
               ? "The game restarts now to apply the new resolution. Progress since your last save is lost."
               : "The game restarts now to apply the new resolution.";
  Row now;
  now.label = "RESTART NOW";
  now.press = [](Out& o) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[DISPLAY_MENU] RESTART NOW (res_scale %s -> %s)", bootScale().c_str(),
                scaleSetting().c_str());
    modMenuRequestRestart();
    o.action = 0;
  };
  Row later;
  later.label = "NOT NOW";
  later.press = [](Out& o) {
    o.page = "display";
    o.focus = 0;
  };
  p.rows = {now, later};
  p.back = [](Out& o) {
    o.page = "display";
    o.focus = 0;
  };
  return p;
}

Page buildPage(const std::string& id) {
  if (id == "displayrestart") return pageRestart();
  return pageDisplay();
}

}  // namespace

bool displayMenuOwnsPage(const std::string& page) {
  return page == "display" || page == "displayrestart";
}

void displayMenuCommand(const std::string& args, std::vector<ModReply>* out) {
  // iter-466 gfx (PORT FEATURE, docs/GRAPHICS_SETTINGS.md): the DISPLAY OPTIONS entries (MAIN MENU >
  // OPTIONS, pause > OPTIONS) open the PC SETTINGS screen, a host overlay
  // (gfx/settings_ui.cpp), instead of these Apt pages - the same hand-off as MODS -> the mod
  // library: the Apt menu closes back to its caller at once (SR_DTEnd action 0) and the overlay
  // draws over it. SR_PC_SETTINGS=0 restores the Apt pages (developer A/B).
  {
    const char* e = std::getenv("SR_PC_SETTINGS");
    std::istringstream ss(args);
    std::string verb, page, ctx;
    ss >> verb >> page >> ctx;
    if (!(e && e[0] == '0') && verb == "open" && page == "display") {
      if (gameopts::enabled()) gameopts::noteOpen(ctx.empty() ? std::string("FE") : ctx);
      modlib::requestOpenPage(modlib::OverlayPage::Settings, ctx.empty() ? std::string("FE") : ctx);
      // lane E L-b: action 3 = handed to the overlay - the Apt menu (devtools_ui.aptasm Cmd) neither shows a page
      // nor calls its caller back now; once the overlay is gone the opener's SR_OptionsClosed brings its menu back
      // (kernel/game_options.hpp), so no popup of the opener stays up under the screen (VIEW PAD LAYOUT and DEV TOOLS
      // then open from a clean menu). Only where that callback is delivered: the GAME tab's pump runs with port
      // features on; without them (a front end mounted by SR_MODS in an automated roll) action 0, the menu closes
      // back to its caller at once, as before (the review's P1-1)
      out->push_back({"SR_DTEnd", {ModArg::Int(0), ModArg::Int(0), ModArg::Int(gameopts::enabled() ? 3 : 0)}});
      SR_LOG_BOOT(sr::LogSeverity::Info, "[DISPLAY_MENU] %s -> the PC SETTINGS overlay", args.c_str());
      return;
    }
  }
  // One command at a time: the Apt UI's fscommand runs on the game's UI thread.
  hostPagesCommand(args, [](const std::string& id) { return buildPage(id); }, out, &g_ctx, "DISPLAY_MENU");
}

}  // namespace kernel
}  // namespace sr
