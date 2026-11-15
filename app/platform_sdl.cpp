#include "platform_sdl.hpp"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "../kernel/guest_context.hpp"  // iter-450S: host window focus flag
#include "../kernel/sr_oracle.hpp"
#if SR_ORACLE
#include "../gfx/hle_video.hpp"         // iter-450UI: operator trace hotkey
#endif
#include "../gfx/pm4_live_trace.hpp"    // iter-462: PM4 live trace hotkey
#include "../kernel/host_automation.hpp"  // iter-464 bgruns: background window + harness keys
#include "../kernel/pc_controls.hpp"      // iter-464 controls: mouse events + relative mouse-look
#include "../kernel/mouse_ui.hpp"       // iter-464: the in-game mouse cursor (platform input)
#include "../kernel/mod_library.hpp"    // iter-465: the mod library overlay (keyboard + text)
#include "../kernel/costume_viewer.hpp" // iter-465 viewer: mouse drag / wheel turn and zoom the suit
#include "log.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include "../kernel/host_input.hpp"     // iter-464 input lane: SDL controllers + hot-plug
#include "../gfx/res_scale.hpp"           // iter-465 polish: live display changes (DISPLAY OPTIONS)
#include "window_icon.hpp"                // iter-465 window: the game's own title icon
#include "../kernel/highfps.hpp"         // iter-466 hfr: the exclusive mode's refresh rate
#include "../kernel/pc_settings.hpp"     // iter-466 gfx: the display list the PC SETTINGS screen shows

#include <algorithm>
#include <vector>

// ── RELEASE_PLAN step 3: SDL2 display semantics on SDL3 (platform_sdl.hpp) ─────────────────────────
namespace sr::sdl3platform {

int sdl2RefreshHz(float rate) {
  if (!(rate > 0.0f)) return 0;
  const int n = int(std::lround(rate));
  // SDL3 rounds the rate to 1/100 Hz (SDL_FinalizeDisplayMode); its NTSC rates are n*1000/1001.
  if (n > 0 && std::fabs(double(rate) - double(n) * 1000.0 / 1001.0) < 0.006) return n - 1;
  return n;
}

int windowsRefreshHz(const SDL_DisplayMode& m, bool* exact) {
  if (exact) *exact = false;
#ifdef _WIN32
  if (m.internal) {
    const DEVMODEW* dm = reinterpret_cast<const DEVMODEW*>(m.internal);
    // dmSize is what EnumDisplaySettingsW wrote back (188 on Windows Server 2025: the DEVMODE up to and including
    // dmDisplayFrequency), never more than the struct SDL keeps.
    if (dm->dmSize >= offsetof(DEVMODEW, dmDisplayFrequency) + sizeof(dm->dmDisplayFrequency) &&
        dm->dmSize <= sizeof(DEVMODEW) && int(dm->dmPelsWidth) == m.w && int(dm->dmPelsHeight) == m.h &&
        dm->dmDisplayFrequency > 1) {
      if (exact) *exact = true;
      return int(dm->dmDisplayFrequency);
    }
  }
#endif
  return sdl2RefreshHz(m.refresh_rate);
}

int sdl2WheelStep(float amount, float* acc) {
  if (amount > 0.0f) {
    if (*acc < 0.0f) *acc = 0.0f;
  } else if (amount < 0.0f) {
    if (*acc > 0.0f) *acc = 0.0f;
  }
  *acc += amount;
  const int whole = *acc > 0.0f ? int(std::floor(*acc)) : *acc < 0.0f ? int(std::ceil(*acc)) : 0;
  *acc -= float(whole);
  return whole;
}

int closestModeSdl2(const ModeCandidate* modes, int n, int w, int h, int hz, int desktopHz,
                    uint32_t desktopFormat) {
  // src/video/SDL_video.c (2.32.10) SDL_GetClosestDisplayModeForDisplay, over SDL3's sorted list.
  const uint32_t targetFormat = desktopFormat;
  const int targetHz = hz ? hz : desktopHz;
  int match = -1;
  for (int i = 0; i < n; ++i) {
    const ModeCandidate& cur = modes[i];
    if (cur.w && cur.w < w) break;   // out of sorted modes large enough here
    if (cur.h && cur.h < h) {
      if (cur.w && cur.w == w) break;
      continue;   // wider, but not tall enough (another aspect ratio): closer modes may follow
    }
    if (match < 0 || cur.w < modes[match].w || cur.h < modes[match].h) {
      match = i;
      continue;
    }
    const ModeCandidate& m = modes[match];
    if (cur.format != m.format && m.format != targetFormat) {
      // sorted highest depth to lowest
      if (cur.format == targetFormat ||
          (SDL_BITSPERPIXEL(cur.format) >= SDL_BITSPERPIXEL(targetFormat) &&
           SDL_PIXELTYPE(cur.format) == SDL_PIXELTYPE(targetFormat)))
        match = i;
      continue;
    }
    if (cur.hz != m.hz && m.hz != targetHz) {
      // sorted highest refresh to lowest
      if (cur.hz >= targetHz) match = i;
    }
  }
  return match;
}

int displayIndexOr0(int want, int count) { return want >= 0 && want < count ? want : 0; }

}  // namespace sr::sdl3platform

namespace {
namespace S3 = sr::sdl3platform;

// SDL3 names a display by an SDL_DisplayID; the settings and the PC SETTINGS rows keep SDL2's index.
std::vector<SDL_DisplayID> displayList() {
  std::vector<SDL_DisplayID> out;
  int n = 0;
  if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {
    out.assign(ids, ids + n);
    SDL_free(ids);
  }
  return out;
}

// The display a setting asks for, if it exists (else the primary).
SDL_DisplayID displayFor(int want) {
  const std::vector<SDL_DisplayID> ids = displayList();
  if (ids.empty()) return SDL_GetPrimaryDisplay();
  return ids[size_t(S3::displayIndexOr0(want, int(ids.size())))];
}

// The index of a display in the list (-1 = unknown).
int displayIndexOf(SDL_DisplayID id) {
  const std::vector<SDL_DisplayID> ids = displayList();
  for (size_t i = 0; i < ids.size(); ++i)
    if (ids[i] == id) return int(i);
  return -1;
}

int windowDisplayIndex(SDL_Window* win) {
  const int i = win ? displayIndexOf(SDL_GetDisplayForWindow(win)) : -1;
  return i < 0 ? 0 : i;
}

bool desktopMode(SDL_DisplayID disp, SDL_DisplayMode* out) {
  const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(disp);
  if (!dm) return false;
  *out = *dm;
  return true;
}

// The exclusive mode for w x h @ hz (0 = the desktop's rate) on disp by SDL2's rule; false = none at
// least that large (SDL2 then fell back to the desktop mode: the caller does too).
bool closestExclusiveMode(SDL_DisplayID disp, int w, int h, int hz, SDL_DisplayMode* out) {
  int n = 0;
  SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(disp, &n);
  if (!modes) return false;
  std::vector<S3::ModeCandidate> cand(size_t(std::max(n, 0)));
  for (int i = 0; i < n; ++i)
    cand[size_t(i)] = {modes[i]->w, modes[i]->h, S3::windowsRefreshHz(*modes[i]), uint32_t(modes[i]->format)};
  SDL_DisplayMode desk{};
  const bool haveDesk = desktopMode(disp, &desk);
  const int k = S3::closestModeSdl2(cand.data(), n, w, h, hz, haveDesk ? S3::windowsRefreshHz(desk) : 0,
                                    haveDesk ? uint32_t(desk.format) : 0u);
  if (k >= 0) *out = *modes[k];
  SDL_free(modes);
  return k >= 0;
}

// iter-466 gfx: every display's name, desktop mode and modes, for the PC SETTINGS screen's MONITOR /
// RESOLUTION / REFRESH RATE rows (kernel/pc_settings.hpp). Main thread (SDL video calls).
void publishDisplays(SDL_Window* win) {
  std::vector<sr::kernel::pcsettings::DisplayInfo> out;
  const std::vector<SDL_DisplayID> ids = displayList();
  for (size_t i = 0; i < ids.size() && i < 16; ++i) {
    sr::kernel::pcsettings::DisplayInfo d;
    const char* name = SDL_GetDisplayName(ids[i]);
    d.name = name ? name : "";
    SDL_DisplayMode dm{};
    if (desktopMode(ids[i], &dm))
      d.desktop = {uint32_t(dm.w), uint32_t(dm.h), uint32_t(S3::windowsRefreshHz(dm))};
    int nm = 0;
    if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(ids[i], &nm)) {
      for (int k = 0; k < nm; ++k) {
        const SDL_DisplayMode& m = *modes[k];
        if (m.w < 640 || m.h < 480) continue;
        const sr::kernel::pcsettings::DisplayMode v{uint32_t(m.w), uint32_t(m.h), uint32_t(S3::windowsRefreshHz(m))};
        if (std::none_of(d.modes.begin(), d.modes.end(), [&](const auto& e) { return e.w == v.w && e.h == v.h && e.hz == v.hz; }))
          d.modes.push_back(v);
      }
      SDL_free(modes);
    }
    out.push_back(std::move(d));
  }
  sr::kernel::pcsettings::noteDisplays(out, windowDisplayIndex(win));
}

// SDL3 creates a window at a position through properties (SDL_CreateWindow takes none). Hidden: the
// caller finishes the fullscreen setup, then shows it (beginBackgroundShow's hint applies there).
SDL_Window* createHiddenWindow(const char* title, int x, int y, int w, int h, SDL_WindowFlags flags) {
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, Sint64(flags | SDL_WINDOW_HIDDEN));
  SDL_Window* win = SDL_CreateWindowWithProperties(props);
  SDL_DestroyProperties(props);
  return win;
}

// The placeholder renderer (only when no D3D12 backend presents: skipRenderer=false).
SDL_Renderer* createFallbackRenderer(SDL_Window* win) {
  SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
  if (ren) {
    SDL_SetRenderVSync(ren, 1);
  } else {
    ren = SDL_CreateRenderer(win, SDL_SOFTWARE_RENDERER);
  }
  return ren;
}

#ifdef _WIN32
HWND hwndOf(SDL_Window* win) {
  if (!win) return nullptr;
  return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}
#endif
}  // namespace

namespace sr {

namespace {
// iter-450UI / iter-462: the F9 operator hotkey - one place, two key sources.
void fireF9Hotkey() {
#if SR_ORACLE
  static const bool s_hotkey = [] {
    const char* v = std::getenv("SR_HLE_TRACE_HOTKEY");
    return v && v[0] == '1';
  }();
  if (s_hotkey) sr::gfx::hle::operatorArmRequest();
#endif  // SR_ORACLE=0: the HLE recorder is compiled out (gfx/hle_video.cpp); SR_HLE_TRACE_HOTKEY reads as unset
  // iter-462: the same key records the next PM4 frame as a Xenia .xtr
  // (SR_PM4_TRACE_HOTKEY=1; no-op otherwise). Host-only, like the above.
  sr::gfx::livetrace::requestCapture();
}
}  // namespace

namespace {
// ── iter-464 bgruns: AUTOMATED RUNS NEVER TAKE THE FOREGROUND ──────────────
// (kernel/host_automation.hpp). The window is shown WITHOUT activation and
// parked at the bottom of the z-order; nothing raises it later. It renders
// exactly as before (D3D12 flip-model Present and the SR_PRESENT_DUMP GPU
// readback do not depend on visibility). Players keep the activating show.
// iter-464 resscale: shared by init() (replay, fixed 1280x720) and initDisplay()
// (the game's window, display modes) - both are created through these two calls.
// SDL3: SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN=0 replaces SDL2's NO_ACTIVATION_WHEN_SHOWN=1 (WIN_ShowWindow
// then shows with SWP_NOACTIVATE); ACTIVATE_WHEN_RAISED=0 too (SDL3 raises the window after a message box).
struct BackgroundShow {
  bool background = false;
#ifdef _WIN32
  HWND prevForeground = nullptr;
#endif
};

BackgroundShow beginBackgroundShow() {
  BackgroundShow b;
  b.background = sr::kernel::automation::policy().windowBackground;
#ifdef _WIN32
  b.prevForeground = GetForegroundWindow();
#endif
  if (b.background) {
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
  }
  return b;
}

void finishBackgroundShow(const BackgroundShow& b, SDL_Window* win) {
#ifdef _WIN32
  if (!b.background) return;
  HWND hwnd = hwndOf(win);
  if (!hwnd) return;
  SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
               SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
  // Belt and braces: if the show activated us anyway (a launcher's
  // STARTUPINFO can override the first ShowWindow), the foreground is
  // ours right now, so handing it straight back is allowed.
  const bool stole = GetForegroundWindow() == hwnd;
  if (stole && b.prevForeground && b.prevForeground != hwnd && IsWindow(b.prevForeground))
    SetForegroundWindow(b.prevForeground);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[AUTOMATION] window shown without activation, z-order bottom%s "
              "(SR_WINDOW_BACKGROUND=0 restores the foreground window)",
              stole ? " - it was activated anyway; foreground handed back" : "");
#else
  (void)b;
  (void)win;
#endif
}

void logSdlVersionOnce() {
  static bool s_done = false;
  if (s_done) return;
  s_done = true;
  const int v = SDL_GetVersion();
  const char* drv = SDL_GetCurrentVideoDriver();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[SDL] SDL %d.%d.%d (built against %d.%d.%d), video driver %s",
              SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_MAJOR_VERSION,
              SDL_MINOR_VERSION, SDL_MICRO_VERSION, drv ? drv : "?");
}
}  // namespace

void* PlatformSDL::nativeWindowHandle() const {
#ifdef _WIN32
  return hwndOf(static_cast<SDL_Window*>(m_window));
#else
  return nullptr;
#endif
}

bool PlatformSDL::init(int windowWidth, int windowHeight, bool skipRenderer) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    return false;
  }
  logSdlVersionOnce();
  // iter-464 input lane: game controllers (hot-plug arrives through this pump).
  sr::kernel::hostinput::init();
  m_width = windowWidth;
  m_height = windowHeight;

  const BackgroundShow bg = beginBackgroundShow();

  SDL_Window* win = createHiddenWindow("Superman Returns - placeholder (guest ran, waiting for game init)",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, m_width, m_height, 0);
  if (!win) {
    SDL_Quit();
    return false;
  }
  SDL_ShowWindow(win);
  m_window = win;
  finishBackgroundShow(bg, win);

  if (!skipRenderer) {
    SDL_Renderer* ren = createFallbackRenderer(win);
    if (!ren) {
      SDL_DestroyWindow(win);
      SDL_Quit();
      m_window = nullptr;
      return false;
    }
    m_glContext = ren;
  }

  return true;
}

// iter-464 PORT FEATURE (docs/RESOLUTION.md): display modes. The window's client
// area is the swapchain size; the PM4 present fits the internal-resolution frame
// into it (aspect kept, letterbox/pillarbox).
bool PlatformSDL::initDisplay(int mode, int windowW, int windowH, int outW, int outH,
                              bool resizable, bool skipRenderer) {
  // Physical pixels: without DPI awareness Windows would stretch a 150% desktop's
  // "2560x1440" window to 4K and the swapchain would never be native. SDL3 declares the process
  // per-monitor-v2 DPI aware by itself (WIN_InitDPIAwareness; SDL2 needed the hint), and on Windows
  // its window coordinates are pixels (docs/README-highdpi.md), as SDL2's were with that hint.
  if (!SDL_Init(SDL_INIT_VIDEO)) return false;
  logSdlVersionOnce();
  sr::kernel::hostinput::init();
  m_mode = mode;
  m_resizable = resizable;
  m_winW = windowW > 0 ? windowW : 1280;
  m_winH = windowH > 0 ? windowH : 720;
  SDL_WindowFlags flags = 0;
  int w = m_winW, h = m_winH;
  // iter-466 gfx: the monitor the settings chose (display_index; 0 = the primary).
  const SDL_DisplayID disp = displayFor(sr::gfx::resscale::display().displayIndex);
  SDL_DisplayMode desk{};
  const bool haveDesk = desktopMode(disp, &desk);
  if (mode == 1) {
    flags |= SDL_WINDOW_FULLSCREEN;   // SDL3: fullscreen with no fullscreen mode = borderless desktop
    if (haveDesk) { w = desk.w; h = desk.h; }
  } else if (mode == 2) {
    w = outW > 0 ? outW : (haveDesk ? desk.w : m_winW);
    h = outH > 0 ? outH : (haveDesk ? desk.h : m_winH);
  } else if (resizable) {
    flags |= SDL_WINDOW_RESIZABLE;
  }
  // An automated run's window never takes the foreground (bgruns; main.cpp also keeps
  // it windowed unless SR_DISPLAY_MODE asks).
  const BackgroundShow bg = beginBackgroundShow();
  SDL_Window* win = createHiddenWindow("Superman Returns - placeholder (guest ran, waiting for game init)",
                                       int(SDL_WINDOWPOS_CENTERED_DISPLAY(disp)), int(SDL_WINDOWPOS_CENTERED_DISPLAY(disp)), w, h, flags);
  if (!win) {
    SDL_Quit();
    return false;
  }
  if (mode == 2) {
    // Exclusive: the display mode closest to the requested output size.
    // iter-466 hfr: a refresh rate that carries the FRAME RATE target (0 = the desktop's rate
    // - a 120 fps target on a 60 Hz desktop of a 120 Hz monitor would otherwise stay at 60 Hz).
    // SDL2's rule picks the lowest mode rate >= this, else the highest (closestModeSdl2).
    int wantHz = sr::kernel::highfps::exclusiveRefreshHz(haveDesk ? S3::windowsRefreshHz(desk) : 0);
    // iter-466 gfx: a refresh rate the settings chose (output_refresh) wins over the rule.
    if (sr::gfx::resscale::display().outputHz) wantHz = int(sr::gfx::resscale::display().outputHz);
    SDL_DisplayMode got{};
    // No mode that large: SDL2 went exclusive at the desktop mode (SDL_GetWindowDisplayMode).
    if (!closestExclusiveMode(disp, w, h, wantHz, &got) && haveDesk) got = desk;
    SDL_SetWindowFullscreenMode(win, &got);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[DISPLAY] exclusive %dx%d @ %d Hz (asked %d Hz)", got.w, got.h,
                S3::windowsRefreshHz(got), wantHz);
    SDL_SetWindowFullscreen(win, true);
  }
  SDL_ShowWindow(win);
  SDL_SyncWindow(win);
  m_window = win;
  finishBackgroundShow(bg, win);
  refreshSize();
  m_resized = false;
  if (haveDesk) sr::gfx::resscale::noteDesktopSize(uint32_t(desk.w), uint32_t(desk.h));
  reportLiveDisplay();
  publishDisplays(win);   // iter-466 gfx
  if (!skipRenderer) {
    SDL_Renderer* ren = createFallbackRenderer(win);
    if (!ren) {
      SDL_DestroyWindow(win);
      SDL_Quit();
      m_window = nullptr;
      return false;
    }
    m_glContext = ren;
  }
  return true;
}

void PlatformSDL::refreshSize() {
  if (!m_window) return;
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(m_window), &w, &h);
  if (w <= 0 || h <= 0) return;  // minimised: keep the last size
  if (w != m_width || h != m_height) {
    m_width = w;
    m_height = h;
    m_resized = true;
  }
}

bool PlatformSDL::consumeResize(int& w, int& h) {
  if (!m_resized) return false;
  m_resized = false;
  w = m_width;
  h = m_height;
  return true;
}

// Alt+Enter: windowed <-> borderless fullscreen (only for a resizable window,
// i.e. the PM4 present that fits any size).
void PlatformSDL::toggleFullscreen() {
  if (!m_window || !m_resizable) return;
  SDL_Window* win = static_cast<SDL_Window*>(m_window);
  if (m_mode == 0) {
    m_winW = m_width;
    m_winH = m_height;
    SDL_SetWindowFullscreenMode(win, nullptr);   // borderless desktop
    SDL_SetWindowFullscreen(win, true);
    m_mode = 1;
  } else {
    SDL_SetWindowFullscreen(win, false);
    SDL_SetWindowResizable(win, true);
    SDL_SetWindowSize(win, m_winW, m_winH);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    m_mode = 0;
  }
  SDL_SyncWindow(win);
  refreshSize();
  reportLiveDisplay();
  // iter-465 polish: Alt+Enter is a display setting too - the DISPLAY OPTIONS page and the next
  // launch follow it (players only: Alt+Enter is real input, which automated runs ignore).
  if (sr::kernel::automation::portFeaturesOn())
    sr::gfx::resscale::setSetting("display_mode", m_mode == 0 ? "windowed" : "borderless");
}

void PlatformSDL::reportLiveDisplay() {
  sr::gfx::resscale::LiveDisplay d = sr::gfx::resscale::liveDisplay();
  d.mode = sr::gfx::resscale::DisplayMode(m_mode);
  if (m_mode == 0 && m_width > 0 && m_height > 0) {
    d.windowW = uint32_t(m_width);
    d.windowH = uint32_t(m_height);
  } else {
    d.windowW = uint32_t(m_winW);
    d.windowH = uint32_t(m_winH);
  }
  sr::gfx::resscale::noteLiveDisplay(d);
}

// iter-465 polish: a change queued by the DISPLAY OPTIONS page (kernel/display_menu.cpp),
// applied here on the window's own thread. Same SDL calls as initDisplay / Alt+Enter. An
// automated run never goes full screen unless SR_DISPLAY_MODE asked for it at launch (the
// user can see windows on this PC; app/main.cpp keeps rolls windowed for the same reason):
// the request is stored by the page and logged here, and the window stays windowed.
void PlatformSDL::applyLiveDisplay() {
  sr::gfx::resscale::LiveDisplay r;
  if (!sr::gfx::resscale::takeLiveDisplayRequest(r) || !m_window) return;
  SDL_Window* win = static_cast<SDL_Window*>(m_window);
  if (!m_resizable) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[DISPLAY] live change refused: this present path has a fixed 1280x720 window "
                "(the setting applies at the next launch)");
    return;
  }
  int mode = int(r.mode);
  const char* inst = std::getenv("SR_INSTANCE_ID");
  const char* envMode = std::getenv("SR_DISPLAY_MODE");
  if (mode != 0 && inst && *inst && !(envMode && *envMode)) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[DISPLAY] live: %s requested; an automated run stays windowed (stored for a player launch)",
                sr::gfx::resscale::modeName(r.mode));
    mode = 0;
  }
  const int ww = r.windowW >= 320 ? int(r.windowW) : m_winW;
  const int wh = r.windowH >= 200 ? int(r.windowH) : m_winH;
  // iter-466 gfx: the monitor (r.display >= 0 asks to move): leave full screen, move, re-enter.
  const SDL_DisplayID curDisp = displayFor(windowDisplayIndex(win));
  const SDL_DisplayID disp = r.display >= 0 ? displayFor(r.display) : curDisp;
  if (disp != curDisp) {
    if (m_mode != 0) SDL_SetWindowFullscreen(win, false);
    SDL_SetWindowPosition(win, int(SDL_WINDOWPOS_CENTERED_DISPLAY(disp)), int(SDL_WINDOWPOS_CENTERED_DISPLAY(disp)));
    if (m_mode == 1 && mode == 1) {
      SDL_SetWindowFullscreenMode(win, nullptr);
      SDL_SetWindowFullscreen(win, true);
    }
    SDL_DisplayMode dd{};
    if (desktopMode(disp, &dd)) sr::gfx::resscale::noteDesktopSize(uint32_t(dd.w), uint32_t(dd.h));
  }
  if (mode == 0) {
    SDL_SetWindowFullscreen(win, false);
    SDL_SetWindowResizable(win, true);
    SDL_SetWindowSize(win, ww, wh);
    SDL_SetWindowPosition(win, int(SDL_WINDOWPOS_CENTERED_DISPLAY(disp)), int(SDL_WINDOWPOS_CENTERED_DISPLAY(disp)));
    m_winW = ww;
    m_winH = wh;
  } else if (mode == 1) {
    if (m_mode == 0) {
      m_winW = ww;
      m_winH = wh;
    }
    if (m_mode == 2) SDL_SetWindowFullscreen(win, false);
    SDL_SetWindowFullscreenMode(win, nullptr);   // borderless desktop
    SDL_SetWindowFullscreen(win, true);
  } else {
    SDL_DisplayMode desk{};
    const bool haveDesk = desktopMode(disp, &desk);
    const int wantW = r.outputW > 0 ? int(r.outputW) : (haveDesk ? desk.w : ww);
    const int wantH = r.outputH > 0 ? int(r.outputH) : (haveDesk ? desk.h : wh);
    int wantHz = sr::kernel::highfps::exclusiveRefreshHz(haveDesk ? S3::windowsRefreshHz(desk) : 0);   // iter-466 hfr
    if (r.refreshHz) wantHz = int(r.refreshHz);   // iter-466 gfx: REFRESH RATE
    SDL_DisplayMode got{};
    if (m_mode != 0) SDL_SetWindowFullscreen(win, false);
    if (!closestExclusiveMode(disp, wantW, wantH, wantHz, &got) && haveDesk) got = desk;
    SDL_SetWindowFullscreenMode(win, &got);
    SDL_SetWindowSize(win, wantW, wantH);
    SDL_SetWindowFullscreen(win, true);
  }
  SDL_SyncWindow(win);
  m_mode = mode;
  refreshSize();
  reportLiveDisplay();
  publishDisplays(win);   // iter-466 gfx
  SR_LOG_BOOT(sr::LogSeverity::Info, "[DISPLAY] live (settings menu): %s, window %dx%d, client %dx%d",
              sr::gfx::resscale::modeName(sr::gfx::resscale::DisplayMode(mode)), m_winW, m_winH, m_width,
              m_height);
}

void PlatformSDL::shutdown() {
  sr::kernel::hostinput::shutdown();
  if (m_glContext) {
    SDL_DestroyRenderer(static_cast<SDL_Renderer*>(m_glContext));
    m_glContext = nullptr;
  }
  if (m_window) {
    SDL_DestroyWindow(static_cast<SDL_Window*>(m_window));
    m_window = nullptr;
  }
  SDL_Quit();
  m_width = 0;
  m_height = 0;
}

bool PlatformSDL::pumpEvents() {
  SDL_Event e;
  // iter-464 controls lane (kernel/pc_controls.hpp): input.ini once (idempotent).
  sr::kernel::pccontrols::init();
  int winW = m_width, winH = m_height;
  if (m_window) SDL_GetWindowSize(static_cast<SDL_Window*>(m_window), &winW, &winH);
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_QUIT) {
      return false;
    }
    // SDL2's whole wheel ticks (sdl3platform::sdl2WheelStep): every consumer reads integer_x / integer_y.
    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
      static float s_wheelX = 0.0f, s_wheelY = 0.0f;
      e.wheel.integer_x = S3::sdl2WheelStep(e.wheel.x, &s_wheelX);
      e.wheel.integer_y = S3::sdl2WheelStep(e.wheel.y, &s_wheelY);
    }
    // iter-464 controls lane: mouse motion (relative, mouse-look), buttons and wheel.
    sr::kernel::pccontrols::onSdlEvent(e);
    // iter-464 (docs/MOUSE_CURSOR.md): mouse motion / buttons / wheel feed the in-game
    // cursor (platform input; inert unless the mouse moves inside the focused window).
    sr::kernel::mouseUiOnSdlEvent(e, winW, winH);
    // iter-464 input lane: controller hot-plug, player 1 = the device used last.
    sr::kernel::hostinput::onEvent(e);
    // iter-465 modlib: the mod library overlay's keyboard + text (only while it is open).
    sr::kernel::modlib::onSdlEvent(e);
    sr::kernel::costumeViewerOnSdlEvent(&e);
    // iter-450S: keyboard-as-gamepad is focus-gated (guest_context.hpp) so
    // background/unattended rolls stay input-neutral by construction.
    // SDL3: window events are top-level event types (SDL2's SDL_WINDOWEVENT sub-events);
    // PIXEL_SIZE_CHANGED is SDL2's SIZE_CHANGED (every size change, programmatic ones included).
    switch (e.type) {
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        sr::kernel::setHostWindowFocused(true);
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        sr::kernel::setHostWindowFocused(false);
        break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_MAXIMIZED:
      case SDL_EVENT_WINDOW_RESTORED:
        refreshSize();  // iter-464: the renderer resizes its swapchain to match
        reportLiveDisplay();
        break;
      default:
        break;
    }
    // iter-464 PORT FEATURE: Alt+Enter toggles windowed / borderless fullscreen
    // (real keyboard only: an automated run ignores the user's keys - bgruns).
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_RETURN &&
        (e.key.mod & SDL_KMOD_ALT) && sr::kernel::automation::policy().realInput)
      toggleFullscreen();
    // ── iter-450UI: THE OPERATOR TRACE HOTKEY (SR_HLE_TRACE_HOTKEY, default
    // OFF). F9 marks "the era under test is on screen RIGHT NOW" — the one
    // fact no guest-state or atlas-version signal can supply for this era
    // (contract + rationale in gfx/hle_video.hpp).
    // ⛔ HOST-DIAGNOSTIC ONLY, and deliberately NOT routed anywhere near the
    // pad merge a few lines' worth of code away in stub_XamInputGetState: F9
    // never enters guest pad state, never writes guest memory, never calls
    // guest code and changes no rendering. It sets one atomic.
    // ⚠ keydown with `repeat == 0` so auto-repeat cannot produce a second
    // edge; the request itself is additionally CAS-latched on the other side.
    // iter-464 bgruns: a real F9 counts only where real input is allowed (an
    // automated run ignores the user's keyboard - host_automation.hpp).
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F9 &&
        !e.key.repeat && sr::kernel::automation::policy().realInput) {
      fireF9Hotkey();
    }
  }
  // iter-464 bgruns: the harness keyboard's F9 - the rising edge of its byte,
  // the same edge a physical keydown (repeat == 0) gives.
  if (const uint8_t* hk = sr::kernel::automation::harnessKeys()) {
    static bool s_f9WasDown = false;
    const bool down = hk[SDL_SCANCODE_F9] != 0;
    if (down && !s_f9WasDown) fireF9Hotkey();
    s_f9WasDown = down;
  }
  // iter-464 controls lane: relative mouse (mouse-look) on in gameplay, off in menus / unfocused.
  sr::kernel::pccontrols::mainThreadTick(m_window);
  // iter-464: harness mouse feed (SR_MOUSE_FEED, automated rolls only; inert when unset).
  sr::kernel::mouseUiPollFeed(winW, winH);
  // iter-464: our cursor replaces the OS one over the window while it is shown (SDL
  // hides the system cursor only inside the window; must run on this, the video thread).
  // In relative mode (mouse-look) SDL owns the pointer's visibility: hands off.
  if (!sr::kernel::pccontrols::relativeMouseOn()) {
    static int s_shown = -1;
    const int want = sr::kernel::mouseUiWantHideOsCursor() ? 0 : 1;
    if (want != s_shown) {
      if (want) SDL_ShowCursor();
      else SDL_HideCursor();
      s_shown = want;
    }
  }
  // iter-465 modlib: text events for the library's search box. SDL2 started text input at video
  // init on desktop and left it on; SDL3 starts it per window only when asked, so it runs exactly
  // while the library is open (no IME / text events during play).
  if (m_window) {
    SDL_Window* win = static_cast<SDL_Window*>(m_window);
    const bool wantText = sr::kernel::modlib::isOpen();
    if (wantText != SDL_TextInputActive(win)) {
      if (wantText) SDL_StartTextInput(win);
      else SDL_StopTextInput(win);
    }
  }
  sr::kernel::hostinput::tick();  // iter-464: rumble -> player 1's pad
  applyLiveDisplay();             // iter-465 polish: DISPLAY OPTIONS window changes
  windowIconTick(m_window);       // iter-465 window: the title icon once the XEX has loaded
  return true;
}

void PlatformSDL::present() {
  presentWithFramebuffer(nullptr, 0, 0, 0);
}

void PlatformSDL::presentWithFramebuffer(const uint8_t* hostFramebuffer, int fbWidth, int fbHeight, int fbPitchBytes) {
  if (!m_glContext || !m_window) return;
  SDL_Renderer* ren = static_cast<SDL_Renderer*>(m_glContext);

  if (hostFramebuffer && fbWidth > 0 && fbHeight > 0 && fbPitchBytes >= fbWidth * 4) {
    // Guest provided a framebuffer: create/update texture and draw it (stretch to window).
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, fbWidth, fbHeight);
    if (tex) {
      void* pixels = nullptr;
      int pitch = 0;
      if (SDL_LockTexture(tex, nullptr, &pixels, &pitch)) {
        const uint8_t* src = hostFramebuffer;
        uint8_t* dst = static_cast<uint8_t*>(pixels);
        for (int y = 0; y < fbHeight; ++y) {
          std::memcpy(dst, src, static_cast<size_t>(fbWidth) * 4);
          src += fbPitchBytes;
          dst += pitch;
        }
        SDL_UnlockTexture(tex);
      }
      SDL_RenderClear(ren);
      SDL_RenderTexture(ren, tex, nullptr, nullptr);
      SDL_DestroyTexture(tex);
    }
  } else {
    // Placeholder: dark blue background until game provides a framebuffer.
    SDL_SetRenderDrawColor(ren, 10, 10, 26, 255);
    SDL_RenderClear(ren);
    const SDL_FRect bar = {0.0f, float(m_height / 2 - 4), float(m_width), 8.0f};
    SDL_SetRenderDrawColor(ren, 60, 80, 180, 255);
    SDL_RenderFillRect(ren, &bar);
  }
  SDL_RenderPresent(ren);
}

void PlatformSDL::presentVdSwapActive(uint64_t frameCount) {
  if (!m_glContext || !m_window) return;
  SDL_Renderer* ren = static_cast<SDL_Renderer*>(m_glContext);

  // Dark background with a green band to indicate "VdSwap active, no GPU content yet".
  SDL_SetRenderDrawColor(ren, 5, 12, 5, 255);
  SDL_RenderClear(ren);

  // Green progress bar (width proportional to frame count mod 240, cycling every ~4s at 60fps).
  const int barWidth = static_cast<int>((frameCount % 240) * m_width / 240);
  const SDL_FRect bar = {0.0f, float(m_height / 2 - 8), float(barWidth), 16.0f};
  SDL_SetRenderDrawColor(ren, 30, 200, 60, 255);
  SDL_RenderFillRect(ren, &bar);

  // Small white tick marks at 1/4 intervals.
  SDL_SetRenderDrawColor(ren, 100, 100, 100, 255);
  for (int i = 1; i <= 3; i++) {
    const SDL_FRect tick = {float(m_width * i / 4 - 1), float(m_height / 2 - 12), 2.0f, 24.0f};
    SDL_RenderFillRect(ren, &tick);
  }

  SDL_RenderPresent(ren);
}

void PlatformSDL::setWindowTitle(const char* title) {
  if (m_window) {
    SDL_SetWindowTitle(static_cast<SDL_Window*>(m_window), title);
  }
}

}  // namespace sr
