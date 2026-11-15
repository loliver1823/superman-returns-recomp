#pragma once

#include <cstdint>
#include <string>

struct SDL_DisplayMode;   // SDL3 (SDL_video.h); kept out of this header

namespace sr {

class PlatformSDL {
public:
  // skipRenderer: when true, only create the window (no SDL renderer).
  // Use this when D3D12 or another backend will handle rendering.
  bool init(int windowWidth = 1280, int windowHeight = 720, bool skipRenderer = false);
  // iter-464 PORT FEATURE (docs/RESOLUTION.md): the window for a display mode
  // (gfx/res_scale.hpp DisplayMode: 0 windowed, 1 borderless fullscreen at the
  // desktop resolution, 2 exclusive fullscreen at outW x outH, 0 = desktop).
  // resizable: a windowed window can be resized / maximised (the PM4 present fits
  // the frame to any size). getWindowWidth/Height = the client size in PIXELS
  // (per-monitor DPI aware).
  bool initDisplay(int mode, int windowW, int windowH, int outW, int outH, bool resizable,
                   bool skipRenderer);
  // A size change since the last call (window resized, Alt+Enter); false = none.
  bool consumeResize(int& w, int& h);
  int displayMode() const { return m_mode; }
  void shutdown();

  bool pumpEvents();
  void present();
  // If hostFramebuffer is non-null, present it (X8R8G8B8 or similar); otherwise draw placeholder.
  void presentWithFramebuffer(const uint8_t* hostFramebuffer, int fbWidth, int fbHeight, int fbPitchBytes);
  // Present a "VdSwap active" indicator (used when VdSwap is called but no GPU content yet).
  void presentVdSwapActive(uint64_t frameCount);
  // Update window title.
  void setWindowTitle(const char* title);

  void* getWindow() const { return m_window; }
  // RELEASE_PLAN step 3 (SDL3): the window's native handle (SDL_PROP_WINDOW_WIN32_HWND_POINTER;
  // SDL2's SDL_GetWindowWMInfo is gone). nullptr = no window / not Windows.
  void* nativeWindowHandle() const;
  int getWindowWidth() const { return m_width; }
  int getWindowHeight() const { return m_height; }

private:
  void* m_window = nullptr;
  void* m_glContext = nullptr;
  int m_width = 0;
  int m_height = 0;
  int m_mode = 0;              // iter-464: the current display mode
  bool m_resizable = false;
  bool m_resized = false;      // a size change not yet consumed
  int m_winW = 1280, m_winH = 720;  // windowed size to return to from fullscreen
  void toggleFullscreen();
  void applyLiveDisplay();   // iter-465 polish: the DISPLAY OPTIONS page's queued change
  void reportLiveDisplay();  // iter-465 polish: tell the page what the window is now
  void refreshSize();
};

// ── RELEASE_PLAN step 3: SDL2 display semantics kept on SDL3 (pure, difftest --sdl3-platform-test) ──
namespace sdl3platform {
// The integer refresh rate SDL2 reported for a mode: Windows' own dmDisplayFrequency. SDL3 reports a
// float (29 / 59 / 119 faked as x*1000/1001, then refined by DXGI's closest matching mode), from which the
// integer cannot be recovered in general (a 75 Hz mode refined to 74.93 Hz looks like NTSC 74.93), so
// windowsRefreshHz reads the DEVMODE SDL3 enumerated the mode from (its private SDL_DisplayModeData,
// src/video/windows/SDL_windowsmodes.h 3.4.18: { DEVMODEW DeviceMode; }), recognised by a dmSize that holds
// dmDisplayFrequency and its own width / height; anything else (another platform, another layout) falls back
// to sdl2RefreshHz.
// The settings (display.ini output_refresh, the PC SETTINGS rows) and the frame-rate rule use it.
int windowsRefreshHz(const ::SDL_DisplayMode& mode, bool* exact = nullptr);
// The fallback: an NTSC-family rate is floored (59.94 -> 59, 119.88 -> 119), every other rate rounds
// (59.95 -> 60, 143.98 -> 144). Approximate where DXGI refined a rate near n*1000/1001.
int sdl2RefreshHz(float rate);
// SDL2's whole wheel ticks (src/events/SDL_mouse.c 2.32.10 SDL_SendMouseWheel): the fraction carries,
// but a direction reversal drops it. SDL3's integer_y keeps the fraction across a reversal (+0.75, -0.5,
// -0.5 = 0, 0, 0 instead of SDL2's 0, 0, -1), so the event pump rewrites integer_x / integer_y with this
// before any consumer sees the event. *acc = the axis's carry (one per axis, as SDL2's mouse kept).
int sdl2WheelStep(float amount, float* acc);
// One fullscreen mode as SDL3 lists it (SDL_GetFullscreenDisplayModes: largest first, then bit depth,
// then refresh rate, the same order SDL2 kept).
struct ModeCandidate {
  int w = 0, h = 0, hz = 0;   // hz = sdl2RefreshHz(refresh_rate)
  uint32_t format = 0;        // SDL_PixelFormat
};
// SDL2's SDL_GetClosestDisplayMode rule (src/video/SDL_video.c 2.32.10, ported): the smallest mode at
// least w x h; at equal size the desktop format, then the LOWEST refresh rate >= hz (0 = the desktop's),
// else the highest. SDL3's SDL_GetClosestFullscreenDisplayMode prefers the nearest rate instead, which
// would move the exclusive mode the frame-rate rule asks for (kernel/highfps.hpp exclusiveRefreshHz).
// Returns the index into modes, or -1 (nothing at least that large).
int closestModeSdl2(const ModeCandidate* modes, int n, int w, int h, int hz, int desktopHz,
                    uint32_t desktopFormat);
// The settings' display_index (SDL2: an index, 0 = the primary) -> SDL3's display list index. SDL3
// lists the primary first on Windows (WIN_AddDisplays, as SDL2 did): index i = SDL_GetDisplays()[i];
// out of range = 0.
int displayIndexOr0(int want, int count);
}  // namespace sdl3platform

}  // namespace sr
