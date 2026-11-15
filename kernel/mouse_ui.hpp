#pragma once
// iter-464 PLATFORM FEATURE: the in-game mouse cursor (docs/MOUSE_CURSOR.md).
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
//
// What it is: PC platform input. The player's mouse drives the game's own menus by calling the
// menus' own functions (iter-466 controls2, USER DECISION 2026-10-06, charter 2a):
//   * the host READS the Apt state (screen stack, focus managers, item geometry) on the guest
//     thread right after the Apt pump (fault-free reads);
//   * on the same thread it then calls, through the game's own AS invoke sequence
//     (kernel/apt_call.hpp): hover = the surface's own focus step (CafeFW_FocusManager
//     selectNextItem / selectPreviousItem, IconMenu Next / Previous, ThumbViewer MoveHighlight),
//     one step per pump until the game's own selection is the hovered item; left click = focus,
//     then the game's input router gEH.HandleEvent(CROSS) - what its Key listener calls; right
//     click = HandleEvent(TRIANGLE); middle = SQUARE; sliders / toggles = their own
//     increaseValue / decreaseValue / nextValue / prevValue. No pad state is touched.
//   * the cursor itself is a host overlay drawn on the present path AFTER the frame dump
//     (gfx/d3d12_overlay.cpp), so captures and replay scores never contain it.
// Why not Apt's own mouse path: the retail Apt runtime keeps the readers (_xmouse/_ymouse,
// hitTest, the onPress..onRollOver names) but its mouse INPUT handler is compiled out
// (executor command type 1, sub_8268D118, has an empty body) and no movie defines a mouse
// handler (docs/MOUSE_CURSOR.md section 2).
//
// Always available (USER 2026-10-02: no toggle), in player runs and automated rolls alike.
// Developer A/B gate: SR_MOUSE_CURSOR=0 disables everything (no SDL use, no Apt reads, no pad
// bits, no overlay). Input isolation (kernel/host_automation.hpp, bgruns): the REAL mouse
// counts only where automation::policy().realInput allows (players always; automated rolls
// only with SR_HARNESS_REAL_INPUT=1); rolls drive it through the harness feed. Inert until the
// mouse moves inside the focused window: an untouched mouse reads nothing and presses nothing.
//
// HAND-OFF with mouse-look (feat/pc-controls, docs/PC_CONTROLS.md 4): while SDL relative mouse
// mode is on, the mouse is the camera's and the cursor ignores it entirely; with relative mode
// off, mouse buttons in a menu are the cursor's. docs/MOUSE_CURSOR.md 6.
#include <cstdint>

union SDL_Event;
struct PPCContext;

namespace sr {
namespace kernel {

// Feature on? (SR_MOUSE_CURSOR=0 turns it off; evaluated once.)
bool mouseUiEnabled();
// The mouse-look owner reports SDL relative mode here when it switches it (the cursor also
// notices it on every mouse event). on = the cursor sleeps and drops queued clicks; off = the
// next motion (SDL's pointer restore) is not taken as a move.
void mouseUiSetRelativeMode(bool on);

// ── main thread (app/platform_sdl.cpp pumpEvents) ───────────────────────────────────────
// Every SDL event goes through here (cheap: motion/button/wheel only). winW/winH = the
// window's client size in SDL units.
void mouseUiOnSdlEvent(const SDL_Event& e, int winW, int winH);
// True while the host OS cursor should be hidden over the window (our cursor replaces it).
bool mouseUiWantHideOsCursor();
// HARNESS ONLY (SR_MOUSE_FEED=<file>, default unset): automated rolls (tools/sr_drive.ps1
// `mouse` steps) append mouse commands to that file instead of moving the desktop cursor,
// so a roll never takes the user's mouse. Called once per pump after the SDL events; reads
// the new complete lines and turns each into the SDL event a real mouse would produce
// (same code path). While a feed is set, real mouse/window events are ignored and the
// window counts as focused for the mouse. Lines (stage coords, 1280x720):
//   move <x> <y> | down left|right|middle | up left|right|middle | wheel <n>
void mouseUiPollFeed(int winW, int winH);
// Parses one feed line (pure; unit-tested). kind: 1 move (x,y), 2 down (button), 3 up
// (button), 4 wheel (n in x). button: 1 left, 2 middle, 3 right (SDL numbering).
struct MouseFeedCmd { int kind = 0; float x = 0, y = 0; int button = 0; };
bool mouseUiParseFeedLine(const char* line, MouseFeedCmd* out);

// ── guest thread (kernel/apt_parse_trace.cpp, after the Apt executor pump) ─────────────
// The snapshot of the Apt menu state (fault-free reads), then at most one call of a menu's own
// function (above). ctx = the pump hook's context (a frame is built below its r1, restored).
void mouseUiAfterAptPump(PPCContext& ctx, uint8_t* base);

// ── pad poll (kernel/stubs.cpp stub_XamInputGetState, user 0) ─────────────────────────
// active = the controller or the keyboard was used this poll: the cursor sleeps (hides, drops
// what it queued) until the mouse moves again.
void mouseUiNoteOtherInput(bool active);

// ── render thread (gfx/d3d12_overlay.cpp) ─────────────────────────────────────────────
struct CursorOverlayState {
  bool visible = false;
  float x = 0, y = 0;        // tip position in backbuffer pixels
  float scale = 1.0f;        // 1.0 = the 720p design size
  float hover = 0.0f;        // 0..1 over an interactive item
  float pulse = -1.0f;       // seconds since the last click (<0 = none)
  float pressed = 0.0f;      // 1 while a mouse button is held
  float timeSec = 0.0f;      // free-running clock (glint animation)
};
// bbW/bbH = the backbuffer being presented.
CursorOverlayState mouseUiOverlayState(int bbW, int bbH);

// ── pure helpers (unit-tested by difftest --mouse-ui-test) ────────────────────────────
// Window client coords -> Apt stage coords (1280x720). The window shows the backbuffer
// stretched to the client area (DXGI flip model default) and Apt draws its 1280x720 stage
// onto the whole backbuffer.
// With an output that is not 16:9 the game image is the centred 16:9 fit rectangle
// (render/resscale's present fit), so both directions go through mouseUiFitRect.
void mouseUiWindowToStage(float wx, float wy, int winW, int winH, float* sx, float* sy);
void mouseUiStageToWindow(float sx, float sy, int winW, int winH, float* wx, float* wy);
// The largest centred 16:9 rectangle in an outW x outH output.
void mouseUiFitRect(int outW, int outH, float* x, float* y, float* w, float* h);
// 16-bit case-insensitive FNV-1a, the Apt property-table key (AptStringHashFNV1a16CI_sub_82681C40).
uint16_t mouseUiAptHash16(const char* s);
// Slider: the number of steps (signed: + = RIGHT) that move `value` to the value under
// stage x `px` on a track [x0, x1] for a range [vmin, vmax] in `step` increments.
int mouseUiSliderSteps(float px, float x0, float x1, float vmin, float vmax, float step,
                       float value);
// Runs the pure-helper self test; returns the number of failures (0 = pass).
int mouseUiSelfTest();

}  // namespace kernel
}  // namespace sr
