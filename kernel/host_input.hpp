#pragma once
// iter-464 input lane: THE PC CONTROLLER LAYER (SDL3 gamepads, hot-plug, player 1 =
// the device used last, rumble, the active prompt-glyph set and what keyboard prompts name).
// docs/INPUT_GLYPHS.md is the design + verification record. Kernel-only header (never
// include from guest/, never add to guest_context.hpp).
//
// THE CONSOLE CONTRACT WE IMPLEMENT (platform, not forcing): the 360 hands the game one
// XInput pad per user index through XamInputGetState / XamInputGetCapabilities /
// XamInputSetState (kernel/stubs.cpp). The PC has many devices instead, so all of them
// collapse onto user 0 ("player 1"):
//   - player 1's pad is whichever SDL game controller was used LAST (a button, or a stick /
//     trigger pushed past half travel); the keyboard (and mouse) is a device too, OR-merged
//     into the report by stubs.cpp mergeKeyboardPad (the keymap is the controls lane's; a
//     poll where the keyboard/mouse changed the report calls noteKeyboardUse());
//   - hot-plug: SDL_EVENT_GAMEPAD_ADDED opens a pad (usable at once; it becomes player 1
//     when nobody is yet, or when player 1's pad had been unplugged), REMOVED closes it;
//   - unplugging player 1's ACTIVE pad reports ERROR_DEVICE_NOT_CONNECTED for user 0 - the
//     console's "controller removed" - until any device is used again or a pad is plugged
//     in. The game's own response to that (InputXenonPad_Update_sub_825FFD40 marks the pad
//     disconnected, GameInput_PauseOnActivePadDisconnect_sub_8229AC90 raises PAUSEMENU) is
//     what the player sees; we only report the state;
//   - XamInputSetState vibration drives SDL rumble on player 1's pad.
// Values are XInput's: buttons = XINPUT_GAMEPAD_* bits, sticks -32768..32767 with +Y up,
// triggers 0..255. No deadzone is added - the 360 driver adds none either; the game applies
// its own (docs/INPUT_GLYPHS.md "deadzones").
//
// INPUT ISOLATION (automated runs): the background-runs automation policy decides
// (kernel/host_automation.hpp computePolicy: SR_INSTANCE_ID = automated, realInput off unless
// SR_HARNESS_REAL_INPUT=1). Without realInput the module opens NO controller and ignores real
// keyboard/mouse events for device tracking; the harness keyboard still counts as keyboard use
// (stubs.cpp mergeKeyboardPad -> noteKeyboardUse). Players set none of these.
//
// THE GLYPH SET: prompts follow the active device (Xbox -> the game's own 360 icons,
// PlayStation 4/5, Switch, keyboard + mouse: the input bound to each control, from the prompt
// source below). Players: always on (input.ini `glyphs = auto`, or pin one set). Automated
// runs: OFF (the game's own icons, oracle-comparable) unless SR_GLYPHS
// (auto|xbox|ps4|ps5|switch|keyboard|cycle) asks for it.
//
// SR_SDL_PAD=0 restores the pre-iter-464 XInputGetState(0) path in stubs.cpp (A/B).
#include <cstdint>
#include <string>

union SDL_Event;

namespace sr::kernel::hostinput {

// The kind of device the player used last.
enum class Device : uint8_t {
  None = 0,   // nothing used or connected yet
  Keyboard,   // keyboard + mouse
  Xbox,       // Xbox 360 / One / Series, XInput-class pads
  PS4,        // DualShock 4 (and DualShock 3)
  PS5,        // DualSense
  Switch,     // Switch Pro controller, Joy-Con pair
  Generic,    // any other SDL game controller (Xbox-style prompts)
};
const char* deviceName(Device d);

// Which prompt artwork is shown.
enum class GlyphSet : uint8_t { Xbox360 = 0, PS4, PS5, Switch, Keyboard, kCount };
const char* glyphSetName(GlyphSet g);
GlyphSet glyphSetForDevice(Device d);
// "auto" -> false; a known name -> true + *out. Accepts xbox|xbox360|ps4|playstation4|ps5|
// playstation5|switch|nintendo|keyboard|kbm.
bool parseGlyphSet(const char* s, GlyphSet* out, bool* isAuto);

// The 360 pad's controls, the unit prompt lookups are expressed in.
enum Control : uint8_t {
  kDpadUp = 0, kDpadDown, kDpadLeft, kDpadRight, kStart, kBack, kLThumb, kRThumb,
  kLB, kRB, kA, kB, kX, kY, kLT, kRT,
  kLStickUp, kLStickDown, kLStickLeft, kLStickRight,
  kRStickUp, kRStickDown, kRStickLeft, kRStickRight,
  kControlCount
};
const char* controlName(Control c);   // "A", "LT", "LStickUp", ... (glyph_map.txt recipes)
uint16_t controlButtonBit(Control c); // XINPUT_GAMEPAD_* bit, 0 for analog controls

struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// ── keyboard prompts: which key or mouse input a keyboard prompt names ──────
// THE BINDING STORE IS THE CONTROLS LANE'S (docs/PC_CONTROLS.md section 7: action-based
// input.ini format 2, kernel/pc_controls.hpp). This module does not map keys and does not own
// a binding table; it asks a PROMPT SOURCE which input a prompt for a 360 control should name.
// Names are the controls lane's vocabulary: SDL key names ("W", "Left Shift", "Space",
// "Return"), "Mouse1".."Mouse5" (left, right, middle, X1, X2), "WheelUp", "WheelDown", plus
// "MouseMove" (the mouse drives the control, e.g. mouse-look on the right stick). "" = nothing
// is bound to that control. The source decides the context (gameplay vs menus) itself.
using KeyPromptSource = std::string (*)(Control c);
// Installs the source (nullptr = the default below). The controls lane registers its
// pccontrols::primaryInputFor adapter at init (docs/INPUT_GLYPHS.md "contract").
void setKeyPromptSource(KeyPromptSource fn);
// What a keyboard prompt for `c` names now (the registered source, else the default).
std::string promptInputFor(Control c);
// The default source: the keyboard merge the game has without the controls lane - the
// iter-450S keymap (kernel/host_automation.cpp mapKeysToPad: Return=START, Escape=BACK,
// arrows=D-pad, Q/E=LB/RB, Space/Z=A, X=B, C=X, V=Y, W A S D=left stick). Controls it does
// not map (LT, RT, L3, R3, the right stick) name nothing.
std::string defaultPromptInputFor(Control c);
// input.ini: this module reads ONE key from it, `glyphs = auto|xbox|ps4|ps5|switch|keyboard`
// (any section; every other line belongs to the controls lane and is ignored here). Returns
// true when the text had a valid glyphs line (*isAuto / *pin set). Bad values -> *errors.
bool readGlyphPin(const std::string& iniText, GlyphSet* pin, bool* isAuto, std::string* errors);

// ── main thread (the SDL event pump, app/platform_sdl.cpp) ───────────────────
void init();                       // once, right after SDL_Init(SDL_INIT_VIDEO)
void onEvent(const SDL_Event& e);  // every event the pump polls
void tick();                       // once per pump pass (applies rumble)
void shutdown();

// ── any thread ────────────────────────────────────────────────────────────────
bool sdlPadEnabled();              // SR_SDL_PAD (default ON)
bool realInputAllowed();           // false on automated runs (isolation rule above)
// User 0 for XamInputGetState/Capabilities: false only while player 1's pad is unplugged
// and nothing has been used since.
bool player1Connected();
// Player 1's pad state when the active device is a pad; false otherwise (keyboard active,
// no pad, isolated run).
bool readActivePad(PadState* out);
// XamInputSetState(user 0): the motor speeds the game asked for (applied by tick()).
void setVibration(uint16_t left, uint16_t right);
// The keyboard / mouse just drove the report (mergeKeyboardPad: a mapped key or mouse input
// changed it). Makes the keyboard the active device; the harness keyboard counts, which is
// how automated runs see keyboard prompts.
void noteKeyboardUse();
Device activeDevice();
// The set prompts show now: the pin, else the active device's. Callers check glyphsEnabled().
GlyphSet activeGlyphSet();
bool glyphsEnabled();
// iter-466 gfx: re-read the player's glyph pin from input.ini (the PC SETTINGS screen changed it).
void reloadGlyphPin();
// Bumps whenever activeGlyphSet() may have changed (the renderer re-selects on change).
uint32_t glyphGeneration();

// Test seam (difftest --host-input-test): reset all state and re-read the environment.
void resetForTest();

}  // namespace sr::kernel::hostinput
