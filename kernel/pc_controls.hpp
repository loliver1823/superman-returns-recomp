#pragma once
// iter-464 controls lane: THE PC CONTROL SCHEME (docs/PC_CONTROLS.md is the design and
// verification record). Kernel-only header: never include from guest/, never add to
// guest_context.hpp.
//
// WHAT IT IS - PC platform input, like the keyboard merge it replaces (kernel/stubs.cpp
// mergeKeyboardPad). The console handed the game one Xbox 360 pad; a PC player has a keyboard
// and a mouse. This module turns them into that pad, once per XamInputGetState poll:
//   * GAME ACTIONS, not pad buttons, are what the player binds (Attack = Left mouse, Block =
//     Right mouse, Takeoff = Space ...). Each action presses the pad control the game's own
//     controller configuration gives it (Action1-4 = A/B/X/Y, Shoulder1-4 = LB/RB/LT/RT,
//     docs/PC_CONTROLS.md section 2), so the game's own input mapper, combos and options run.
//   * MOUSE-LOOK becomes the right stick (section 4): relative mouse counts -> the stick value
//     that turns the game's camera by sensitivity x counts this tick (inverse of the camera's
//     own control law). The game's X/Y INVERSION options act on it as on a pad.
//   * DODGE is a macro (section 5): the exact Block + left-stick flick the game's evade
//     detector expects, in the current movement direction.
//   * The two NEW air moves (section 6) are a PORT FEATURE behind SR_PC_MOVES (default OFF;
//     docs/MODS.md port-feature tier): gameplay changes, never on in natural rolls.
// NOT FORCING: every output is pad state the player produced with a device. Nothing here
// writes guest memory or calls guest code (the air moves in pc_moves.cpp are the gated
// exception, declared in docs/OVERRIDES.json / MODS.md).
//
// AUTOMATED RUNS (kernel/host_automation.hpp): real keyboard/mouse only with realInput. The
// harness keyboard channel keeps the iter-450S TEST KEYMAP (Enter=START, Z=A, X=B, C=X, V=Y,
// Q/E=LB/RB, arrows, WASD, Esc/Backspace=BACK) that tools/sr_drive.ps1's key names rely on,
// unless the channel's keymap byte (or SR_PC_KEYMAP=player) selects the PLAYER keymap - the
// player-binding tests (tools/controls/harness_play.py) do that, and also feed the channel's
// mouse block.
#include <cstdint>
#include <string>
#include <vector>

union SDL_Event;

namespace sr::kernel::pccontrols {

// ── the player's vocabulary ──────────────────────────────────────────────────────────────
enum Action : uint8_t {
  kMoveForward = 0, kMoveBack, kMoveLeft, kMoveRight,   // left stick
  kAttack,       // Action3 = X: ACTION (punch)
  kBlock,        // Action2 = B: BLOCK / DROP
  kPickup,       // Action1 = A: PICKUP / THROW
  kTakeoff,      // Action4 = Y: TAKEOFF / LAND (hold = power launch / power drop)
  kSuperspeed,   // Shoulder2 = RB: SUPERSPEED (SPRINT on the ground)
  kTargetLock,   // Shoulder3 = LT: TARGET LOCK
  kSuperpower,   // Shoulder4 = RT: SUPERPOWER
  kCameraHover,  // Shoulder1 = LB: RESET CAMERA (ground) / AIRDRIFT-HOVER (air)
  kMap,          // Select = BACK: MAP
  kPause,        // START: PAUSE
  kPowerUp,      // D-pad up:    heat vision
  kPowerRight,   // D-pad right: freeze breath
  kPowerLeft,    // D-pad left:  super breath
  kPowerDown,    // D-pad down: no gameplay command reads it (x-ray has no input in retail)
  kPowerNext,    // cycles the four (wheel)
  kPowerPrev,
  kDodge,        // macro: Block + left-stick flick (section 5); air moves with SR_PC_MOVES
  kLookUp, kLookDown, kLookLeft, kLookRight,   // keyboard camera (right stick), unbound
  kMenuUp, kMenuDown, kMenuLeft, kMenuRight,   // D-pad (menus; in gameplay = power select)
  kMenuConfirm,  // A in menus
  kMenuBack,     // B in menus
  kMenuX,        // X (SQUARE) in menus: COSTUMES WEAR (iter-465 menu keys, docs/PC_CONTROLS.md 3.2)
  kMenuY,        // Y (CIRCLE) in menus: the map's FORTRESS OF SOLITUDE, the controller diagram
  kCycleTarget,  // R3 (iter-466 controls2): GAME_LLOCK_CYCLE - the next target while locked
  kActionCount
};
const char* actionName(Action a);   // the input.ini key ("Attack", "MoveForward", ...)

// ── bindable inputs ──────────────────────────────────────────────────────────────────────
// 0 = none; 1..511 = SDL scancode; then the mouse.
enum : uint16_t {
  kInNone = 0,
  kInMouseLeft = 0x200, kInMouseRight, kInMouseMiddle, kInMouseX1, kInMouseX2,
  kInWheelUp, kInWheelDown,
};
// "W", "Left Shift", "Space", "Mouse1".."Mouse5", "WheelUp", "WheelDown", "none" -> code.
// Returns false for an unknown name.
bool parseInputName(const char* s, uint16_t* out);
// An SDL key name -> its SDL scancode; SDL2's media-key names (AudioNext, AudioPlay, ...) load as
// the SDL3 key they name (RELEASE_PLAN step 3). False = unknown.
bool scancodeFromName(const char* name, uint16_t* out);
std::string inputName(uint16_t in);   // the canonical name (SDL key name, "Mouse1", ...)

constexpr int kInputsPerAction = 4;   // input.ini v3 (v2: 2)
struct Bindings {
  uint16_t in[kActionCount][kInputsPerAction] = {};
};
Bindings defaultBindings();   // docs/PC_CONTROLS.md section 1 (the user's 2026-10-02 layout)

struct MouseSettings {
  bool look = true;           // mouse-look in gameplay
  float sensitivity = 1.0f;   // x the base rate (section 4)
  bool invertX = false;       // host-side, ON TOP of the game's own X INVERSION option
  bool invertY = false;       // host-side, ON TOP of the game's own Y INVERSION option
};

struct Config {
  Bindings bindings = defaultBindings();
  MouseSettings mouse;
};
// ── iter-466 controls2: the remapper's vocabulary (the PC SETTINGS CONTROLS tab) ─────────────
const char* actionLabel(Action a);     // "ATTACK", "TAKE OFF / LAND" ...
const char* actionGroup(Action a);     // the CONTROLS tab section: MOVEMENT, ACTIONS ...
const char* actionControl(Action a);   // the game control it drives ("X (Action3)" ...)
const char* actionDesc(Action a);      // one line: what the game does with it
// "Mouse1, Tab" <-> the action's inputs (an unknown name -> false; "none" / "" = unbound).
std::string bindingText(const Bindings& b, Action a);
bool parseBindingText(const std::string& text, uint16_t out[kInputsPerAction]);
// Two actions bound to one input that both act in one context (gameplay / menus / cinematics),
// except the designed shares (Esc = Pause and MenuBack; a D-pad direction for a menu move and a
// power in gameplay).
struct Conflict { uint16_t input; Action a, b; };
std::vector<Conflict> findConflicts(const Bindings& b);

// Parses input.ini v2 / v3 text over `base` (docs/PC_CONTROLS.md section 7). Unknown keys / bad
// names are reported in *errors (one per line) and skipped.
Config parseIni(const std::string& text, const Config& base, std::string* errors);
std::string defaultIni();     // written for players when the file is missing
const Config& config();       // the active config (defaults until init() read the file)

// ── the per-poll mapping (pure; unit-tested) ─────────────────────────────────────────────
struct Pad {                  // XInput conventions: +Y up, triggers 0..255
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
// Which keymap a keyboard source uses.
enum class Keymap : uint8_t { Player = 0, LegacyTest = 1 };
// The iter-450S test keymap (sr_drive's key names): ORs buttons, sets the left stick.
uint16_t mapLegacyTestKeys(const uint8_t* keys, Pad* pad);

// For button prompts (the input lane's keyboard glyphs): the first input bound to the action
// that presses this 360 control in gameplay or in menus. `xinputBit` = an XINPUT_GAMEPAD_* bit,
// or kPadLT / kPadRT for the triggers. kInNone when nothing presses it.
constexpr uint32_t kPadLT = 0x10000u, kPadRT = 0x20000u;
uint16_t primaryInputFor(uint32_t xinputBit, bool gameplay, const Bindings& b);
// The first input bound to an action (kInNone when unbound).
uint16_t firstInput(const Bindings& b, Action a);

// ── lane E E3 (RELEASE_PLAN step 5): the host screens' keys (PC SETTINGS, the mod library) ─────────
// The host overlays draw their own menus and map the keyboard themselves; this is their one rule, so a key does
// what its cap says. The acts are the pad controls the host screens use.
enum class HostAct : uint8_t { None, Up, Down, Left, Right, Accept, Back, X, Y, LB, RB, LT, RT, View };
// LegacyTest keymap (the harness channel's default, tools/sr_drive.ps1 key names): the fixed iter-450S table -
//   arrows; Enter / KP Enter / Z = Accept; Esc / Backspace / X = Back; Space / C = X; R / V = Y; Q / E = LB / RB;
//   Tab = RB (Shift+Tab = LB); PgUp / PgDn = LT / RT.
// Player keymap, in this order (the first that names the key wins):
//   1. the failsafes: Enter / KP Enter = Accept, Esc = Back (whatever is rebound; owner ruling F3, 2026-10-22);
//   2. the menu bindings: MenuConfirm / MenuBack / MenuX / MenuY / MenuUp..MenuRight, and Map (the pad's BACK button
//      in menus, primaryInputFor) = View (keyboard inputs only);
//   3. the host-only keys no menu binding uses: arrows; Tab = RB (Shift+Tab = LB); PgUp / PgDn = LT / RT.
// So with the defaults (MenuX = E, MenuY = F) E is X, F is Y and the tabs are Tab / Shift+Tab; Z / X / C / V, Q,
// Space and R do nothing for players unless a menu action is bound to them (coordinator ruling 2026-10-23: no key
// acts that its cap does not show).
HostAct hostKeyAct(uint16_t scancode, bool shift, Keymap km, const Bindings& b);
// The key that does `a` (kInNone = none): the first, in the act's order (Player: its menu bindings, then the fixed
// keys; LegacyTest: the fixed keys), that hostKeyAct maps to `a`; *shift = it needs Shift (Shift+Tab).
uint16_t hostKeyFor(HostAct a, Keymap km, const Bindings& b, bool* shift = nullptr);
// The key cap a host screen draws for `a` under the keymap: the first key that hostKeyAct maps to it, in the act's
// order (Player: its menu bindings, then the fixed keys; LegacyTest: the fixed keys), as short cap text ("Enter",
// "Esc", "PgUp", "Shift+Tab", "E" ...); "" when no key does `a`.
std::string hostKeyCap(HostAct a, Keymap km, const Bindings& b);
// Short cap text for a keyboard key ("Return" -> "Enter", "Escape" -> "Esc", "PageUp" -> "PgUp" ...).
std::string keyCapName(uint16_t scancode);
// The MOVE prompt's cap (the D-pad on a keyboard): "" while the arrow keys move in their directions under the keymap
// (the host screens draw the arrows picture), else the keys that do, "<up>/<down>" ("-" for a direction no key
// moves; Astra E3 r1: MenuX bound to Up made the advertised arrows press X). leftRight = MOVE also means LEFT /
// RIGHT on that screen (PC SETTINGS): all four arrows are checked and named, "<up>/<down>/<left>/<right>" (r2).
std::string hostMoveCap(Keymap km, const Bindings& b, bool leftRight = false);
// PC SETTINGS' hold-to-preview key: the key the rule above gives RT (PgDn unless a menu binding takes it - a binding
// wins, in a modal too; the "Hold: preview" cap names RT's key), and P in the LegacyTest keymap, where P does
// nothing else (the iter-450S key). Astra E3 r1.
bool hostPreviewKey(uint16_t scancode, bool shift, Keymap km, const Bindings& b);
// The modifiers (SDL_Keymod bits: Shift / Ctrl / Alt) held on a harness key array (512 entries by SDL scancode), so
// the harness channel's Shift+Tab is LB and Ctrl+F searches, as on a real keyboard (Astra E3 r1). 0 for null.
uint16_t harnessKeyMods(const uint8_t* keys);

// The game context the LAST pad poll saw (cached; any thread - the prompt glyphs ask from the
// render thread and must not re-run the game readers). True = gameplay (or a mini-game).
bool lastPollWasGameplay();

// ── host glue (pc_controls.cpp) ───────────────────────────────────────────────────────────
// SR_PC_CONTROLS (default ON); =0 restores the iter-450S keyboard merge exactly (A/B).
bool enabled();
// Lane E E3: the keymap the host screens use for a keyboard source, decided as mergePoll decides it: SR_PC_KEYMAP
// if set; the harness channel = LegacyTest unless its flags select the player keymap; the real keyboard = Player
// for players, LegacyTest in automated runs. SR_PC_CONTROLS=0 = LegacyTest (the iter-450S keys).
Keymap hostKeymap(bool harnessChannel);
// The keymap the host screens' key caps follow (the one the prompts use: Player when any source maps by it).
Keymap hostCapKeymap();
// Once, at startup: players read / create <content root>/settings/input.ini; rolls use defaults.
void init();
// iter-466 gfx: re-read input.ini after the PC SETTINGS screen changed it (players only; the
// CONTROLS tab's APPLY). Host-only: the bindings and mouse options the host input path uses.
void reload();
// Main thread (app/platform_sdl.cpp pumpEvents): every SDL event; then once per pump pass the
// relative-mouse switch (on in gameplay with mouse-look, focused, real input allowed).
void onSdlEvent(const SDL_Event& e);
void mainThreadTick(void* sdlWindow);
// RELEASE_PLAN step 3 (SDL3): relative mouse mode is per window; true = it is on for the game's
// window (the sdlWindow mainThreadTick gets). The one arbiter of mouse-look vs the cursor
// (docs/MOUSE_CURSOR.md 6). Any thread: a copy mainThreadTick publishes (SDL3's query is main-thread only).
bool relativeMouseOn();
// The pad poll (kernel/stubs.cpp mergeKeyboardPad): maps the harness keys and the real keys
// (null when unavailable) plus the mouse into *pad. Returns the buttons it added; *anyHeld =
// the keyboard/mouse changed the report (the input lane's "keyboard is the active device").
uint16_t mergePoll(Pad* pad, const uint8_t* harnessKeys, const uint8_t* realKeys,
                   bool realFocused, bool* anyHeld);

// Lane E E5c (step 5): the mouse cursor's title click is the game's own START - the START control held for `polls`
// pad polls through the action layer (OR'ed into the frame mergePoll publishes, as a bound key held that long), so
// the game's input layer raises its own INTERFACE_START (113): ActivePadListener (dword_8296EDB4) arms the front
// end's pad / keyboard menu input (+16, which MenuButtonRepeatTick_sub_8214FC78 waits for), sets the active pad and,
// on the first menu tick, ProfileMgr_SetActiveUser. A direct gEH.HandleEvent(301) does none of that (the r5 PC roll:
// UP after a mouse title click never reached the menu). The pulse is not the player's keyboard: it never counts as
// keyboard use (the cursor stays awake, the prompt glyphs keep their device). false = the action layer is off
// (SR_PC_CONTROLS=0, SR_PC_ACTIONS=0, port features off): nothing is queued and the caller uses its fallback.
// E5c r2 (review P2): the pulse is bound to its click - at most kStartPulsePolls polls AND kStartPulseMaxMs of wall
// time from pulseStart (a stalled poll never carries START into the next screen), and cancelStartPulse() (the cursor,
// once the title is no longer the screen) drops whatever is left.
constexpr int kStartPulsePolls = 6;
constexpr int kStartPulseMaxMs = 250;
bool pulseStart(int polls);
void cancelStartPulse();
// The pulse's share of this poll's frame: the START bit (XINPUT_GAMEPAD_START, 0x0010) while polls are left and the
// deadline has not passed, else 0; each call uses one poll (mergePoll calls it once per poll). Any thread.
uint16_t takeStartPulse();
// Test seam (difftest --pc-controls-test): queue a pulse whatever the action layer's state; its deadline is
// `deadlineMs` from now (a negative one has already passed).
void queueStartPulseForTest(int polls, int deadlineMs = kStartPulseMaxMs);

}  // namespace sr::kernel::pccontrols
