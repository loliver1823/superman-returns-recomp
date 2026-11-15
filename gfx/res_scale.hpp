#pragma once
// iter-464 PORT FEATURE (docs/RESOLUTION.md): internal draw resolution scaling and the
// output (display) configuration.
//
// The draw resolution scale is Xenia's draw_resolution_scale_x / _y
// (reference/xenia-canary/src/xenia/gpu/texture_cache.cc:20-38, BSD-3): every PM4-path
// render target, the EDRAM buffer, every resolve and every texture a resolve wrote are
// `scale` times larger per axis; the guest still sees its own 1280x720 world (fetch
// constants, viewport registers, resolve rectangles are all guest pixels).
//
// Sources, highest first (all read once, before the backend starts):
//   SR_RES_SCALE        auto | 1..7 | <x>x<y>   (rolls, A/B; "=1" is the 1x control)
//   settings/display.ini  res_scale = auto | 1..7 | <x>x<y>
//                         display_mode = windowed | borderless | exclusive
//                         window_width / window_height (windowed client size)
//                         output_width / output_height (exclusive mode size)
//   (display.ini lives beside game_options.ini under the save root,
//    kernel/xam_content xamContentRoot(); an automated roll's root is fresh per process.)
//   SR_DISPLAY_MODE, SR_WINDOW_SIZE=<w>x<h>, SR_OUTPUT_SIZE=<w>x<h> override the file.
// Default while the feature proves itself: res_scale 1, windowed 1280x720 (docs/RESOLUTION.md
// "Defaults"). "auto" = the smallest integer scale whose internal height (720 * s) covers the
// output height: 4K -> 3x, 1440p -> 2x, 1080p -> 2x (downsampled), 720p -> 1x.
#include <cstdint>
#include <string>

namespace sr {
namespace gfx {
namespace resscale {

// Xenia kMaxDrawResolutionScaleAlongAxis (texture_cache.h:64): 3-bit shader fields, the
// resolve half-pixel fill handles <= 3 host pixels, a 7x7 EDRAM buffer still binds.
constexpr uint32_t kMaxScale = 7;

struct Scale {
  uint32_t x = 1, y = 1;
  bool scaled() const { return x > 1 || y > 1; }
  uint32_t area() const { return x * y; }
  uint32_t maxAxis() const { return x > y ? x : y; }
};

enum class DisplayMode : uint8_t { Windowed = 0, Borderless = 1, Exclusive = 2 };

struct Display {
  DisplayMode mode = DisplayMode::Windowed;
  uint32_t windowW = 1280, windowH = 720;  // windowed client area
  uint32_t outputW = 0, outputH = 0;       // exclusive mode size (0 = desktop)
  bool scaleAuto = false;                  // res_scale = auto
  uint32_t scaleX = 1, scaleY = 1;         // explicit scale (when !scaleAuto)
  std::string scaleSource = "default";     // env / display.ini / default (for the log)
  std::string modeSource = "default";
  // iter-466 gfx (PC SETTINGS > DISPLAY): the monitor (SDL display index) and the exclusive-mode
  // refresh rate (0 = the frame-rate rule's choice, kernel/highfps exclusiveRefreshHz).
  int displayIndex = 0;                    // display_index
  uint32_t outputHz = 0;                   // output_refresh
  bool firstStart = false;                 // display.ini was written by the first-start detection
};

// Read SR_* and settings/display.ini (once). Safe to call repeatedly.
const Display& display();

// Fix the scale for this process from the output size the window got (auto mode uses it).
// Called by app/main.cpp after the window exists and before the backend renders.
void configure(uint32_t outputW, uint32_t outputH);

// The device's limit (tiled resources + virtual address bits per resource, Xenia
// ClampDrawResolutionScaleToMaxSupported, texture_cache.cc:161-196) and the PM4 path's
// prerequisites (GPU EDRAM + shared memory). Called by D3D12Backend::init. Freezes the scale.
void clampToDevice(bool tiledResources, uint32_t vaBitsPerResource, uint32_t maxTextureDim);

// The draw resolution scale of this process. Frozen after clampToDevice (the first call
// after that never changes); before configure() it reports 1x.
const Scale& get();

// Auto rule: smallest integer s with 720 * s >= outH (and 1280 * s >= outW), clamped.
uint32_t autoScaleFor(uint32_t outW, uint32_t outH);

// Parse "auto" / "N" / "XxY". false on a malformed value.
bool parseScale(const char* s, bool& isAuto, uint32_t& x, uint32_t& y);

const char* modeName(DisplayMode m);

// iter-466 gfx: FIRST START (owner 2026-10-05: "detect it on first start"). With no display.ini, a
// player's launch detects the primary monitor's desktop resolution and the highest refresh rate it
// offers at that size, and writes display_mode = borderless, output = that size, output_refresh,
// res_scale = auto and target_fps = monitor (the display's refresh, the DISPLAY tab's "DISPLAY (N HZ)";
// it follows the display if its rate changes). Automated runs (SR_INSTANCE_ID) never write it.
struct NativeMode {
  uint32_t w = 0, h = 0, hz = 0;
};
// Pure: the display.ini text the first start writes for a detected mode.
std::string firstStartIni(const NativeMode& m);

// The settings-row hook for the settings / mod-menu UI: validate one display.ini key
// (res_scale, display_mode, window_width/_height, output_width/_height, display_index,
// output_refresh) and write it,
// keeping every other line. Takes effect on the next launch (the scale is frozen at
// start, as in Xenia). false = unknown key, malformed value or the file cannot be written.
bool setSetting(const std::string& key, const std::string& value);

// iter-465 polish: the stored value of one display.ini key ("" when absent), for the
// DISPLAY OPTIONS page (kernel/display_menu.cpp) - the page shows what the store holds.
std::string storedSetting(const std::string& key);

// iter-465 polish: LIVE window changes from the DISPLAY OPTIONS page. The page runs on a guest
// thread (fscommand), the window belongs to the SDL thread: the page queues a request and
// PlatformSDL::pumpEvents applies it (windowed size, borderless, exclusive at an output size;
// 0 x 0 = the desktop). The platform reports the window's state after every change (Alt+Enter
// included), so the page always shows what is on screen.
struct LiveDisplay {
  DisplayMode mode = DisplayMode::Windowed;
  uint32_t windowW = 1280, windowH = 720;
  uint32_t outputW = 0, outputH = 0;
  int display = -1;          // iter-466 gfx: the monitor (-1 = keep the current one)
  uint32_t refreshHz = 0;    // iter-466 gfx: exclusive refresh (0 = the frame-rate rule's choice)
};
void requestLiveDisplay(const LiveDisplay& d);
bool takeLiveDisplayRequest(LiveDisplay& d);
void noteLiveDisplay(const LiveDisplay& d);
LiveDisplay liveDisplay();
// The desktop size of the window's display (the platform records it at window creation).
void noteDesktopSize(uint32_t w, uint32_t h);
void desktopSize(uint32_t& w, uint32_t& h);

}  // namespace resscale
}  // namespace gfx
}  // namespace sr
