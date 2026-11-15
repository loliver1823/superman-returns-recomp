// iter-464 controls lane: bindings, names, input.ini v2 (kernel/pc_controls.hpp,
// docs/PC_CONTROLS.md sections 1 and 7). Pure code: no SDL state, no guest access - the
// difftest --pc-controls-test drives it.
#include "pc_controls.hpp"
#include "host_automation.hpp"

#include <SDL3/SDL.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace sr::kernel::pccontrols {

namespace {
const char* const kActionNames[kActionCount] = {
    "MoveForward", "MoveBack", "MoveLeft", "MoveRight",
    "Attack", "Block", "Pickup", "Takeoff",
    "Superspeed", "TargetLock", "Superpower", "CameraHover",
    "Map", "Pause",
    "PowerUp", "PowerRight", "PowerLeft", "PowerDown", "PowerNext", "PowerPrev",
    "Dodge",
    "LookUp", "LookDown", "LookLeft", "LookRight",
    "MenuUp", "MenuDown", "MenuLeft", "MenuRight", "MenuConfirm", "MenuBack",
    "MenuX", "MenuY",
    "CycleTarget",
};

struct MouseName { const char* name; uint16_t code; };
const MouseName kMouseNames[] = {
    {"Mouse1", kInMouseLeft},   {"Mouse2", kInMouseRight}, {"Mouse3", kInMouseMiddle},
    {"Mouse4", kInMouseX1},     {"Mouse5", kInMouseX2},    {"WheelUp", kInWheelUp},
    {"WheelDown", kInWheelDown},
    // aliases (parse only)
    {"LeftMouse", kInMouseLeft}, {"RightMouse", kInMouseRight}, {"MiddleMouse", kInMouseMiddle},
};

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool ieq(const std::string& a, const char* b) {
  const size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

bool parseBool(const std::string& v, bool* out) {
  if (ieq(v, "1") || ieq(v, "on") || ieq(v, "true") || ieq(v, "yes")) { *out = true; return true; }
  if (ieq(v, "0") || ieq(v, "off") || ieq(v, "false") || ieq(v, "no")) { *out = false; return true; }
  return false;
}

void bind(Bindings& b, Action a, uint16_t i0, uint16_t i1 = kInNone) {
  b.in[a][0] = i0;
  b.in[a][1] = i1;
}

// RELEASE_PLAN step 3 (SDL3): input.ini v2 stores SDL key names. SDL3 kept every scancode below 258
// with SDL2's name (both tables compared, lane S1 NOTES 2026-10-18), named 20 keys SDL2 left
// unnamed (NonUSBackslash, International 1-9, Language 1-9, Front) and renumbered the media block
// (258+). A file written under SDL2 that names a media key loads as the same physical key: on
// Windows SDL2 mapped VK_MEDIA_* to these names, SDL3 maps the same keys to the codes below (the
// rest follow SDL's README-migration renames). SDL2's WWW / Mail / Calculator / Computer / App1 /
// App2 / Brightness* / DisplaySwitch / KBDIllum* keys have no SDL3 scancode: such a binding reads as
// an unknown key (reported, the default kept), as any unknown name does.
struct Sdl2KeyName {
  const char* name;
  SDL_Scancode sc;
};
constexpr Sdl2KeyName kSdl2KeyNames[] = {
    {"AudioNext", SDL_SCANCODE_MEDIA_NEXT_TRACK},  {"AudioPrev", SDL_SCANCODE_MEDIA_PREVIOUS_TRACK},
    {"AudioStop", SDL_SCANCODE_MEDIA_STOP},        {"AudioPlay", SDL_SCANCODE_MEDIA_PLAY_PAUSE},
    {"AudioMute", SDL_SCANCODE_MUTE},              {"AudioRewind", SDL_SCANCODE_MEDIA_REWIND},
    {"AudioFastForward", SDL_SCANCODE_MEDIA_FAST_FORWARD},
};
}  // namespace

bool scancodeFromName(const char* name, uint16_t* out) {
  if (!name || !*name) return false;
  // SDL's names ("Left Shift", "Space", "Return", "W", "1"). SDL_GetScancodeFromName is
  // case-insensitive and needs no initialised video subsystem.
  SDL_Scancode sc = SDL_GetScancodeFromName(name);
  if (sc == SDL_SCANCODE_UNKNOWN) {
    for (const auto& k : kSdl2KeyNames)
      if (ieq(name, k.name)) sc = k.sc;
  }
  if (sc == SDL_SCANCODE_UNKNOWN) return false;
  *out = static_cast<uint16_t>(sc);
  return true;
}

const char* actionName(Action a) { return a < kActionCount ? kActionNames[a] : "?"; }

bool parseInputName(const char* s, uint16_t* out) {
  if (!s) return false;
  const std::string t = trim(s);
  if (t.empty() || ieq(t, "none")) { *out = kInNone; return true; }
  for (const auto& m : kMouseNames)
    if (ieq(t, m.name)) { *out = m.code; return true; }
  return scancodeFromName(t.c_str(), out);
}

std::string inputName(uint16_t in) {
  if (in == kInNone) return "none";
  if (in >= kInMouseLeft) {
    for (const auto& m : kMouseNames)
      if (m.code == in) return m.name;
    return "none";
  }
  const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(in));
  return (n && *n) ? n : "none";
}

// docs/PC_CONTROLS.md section 1 - the user's working default layout (2026-10-02).
Bindings defaultBindings() {
  Bindings b;
  bind(b, kMoveForward, SDL_SCANCODE_W);
  bind(b, kMoveBack, SDL_SCANCODE_S);
  bind(b, kMoveLeft, SDL_SCANCODE_A);
  bind(b, kMoveRight, SDL_SCANCODE_D);
  bind(b, kAttack, kInMouseLeft);
  bind(b, kBlock, kInMouseRight);
  bind(b, kPickup, SDL_SCANCODE_E);
  bind(b, kTakeoff, SDL_SCANCODE_SPACE);
  bind(b, kSuperspeed, SDL_SCANCODE_LSHIFT);
  bind(b, kTargetLock, kInMouseMiddle, SDL_SCANCODE_TAB);
  bind(b, kSuperpower, SDL_SCANCODE_F);
  bind(b, kCameraHover, SDL_SCANCODE_LCTRL);
  bind(b, kMap, SDL_SCANCODE_M);
  bind(b, kPause, SDL_SCANCODE_ESCAPE);
  bind(b, kPowerUp, SDL_SCANCODE_1);
  bind(b, kPowerRight, SDL_SCANCODE_2);
  bind(b, kPowerLeft, SDL_SCANCODE_3);
  // iter-466 controls2: D-pad down drives no gameplay command (sub_8229B630 and the config binders
  // bind up / left / right only; x-ray vision has a power definition but no input): unbound.
  bind(b, kPowerNext, kInWheelDown);
  bind(b, kPowerPrev, kInWheelUp);
  bind(b, kDodge, SDL_SCANCODE_LALT);
  bind(b, kMenuUp, SDL_SCANCODE_UP);
  bind(b, kMenuDown, SDL_SCANCODE_DOWN);
  bind(b, kMenuLeft, SDL_SCANCODE_LEFT);
  bind(b, kMenuRight, SDL_SCANCODE_RIGHT);
  bind(b, kMenuConfirm, SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER);
  bind(b, kMenuBack, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE);
  // iter-465 menu keys: the menus' X / Y actions had no key (COSTUMES WEAR = X, the map's
  // FORTRESS OF SOLITUDE = Y). E = Equip / wear, F = Fortress; both are gameplay keys too
  // (PICKUP, SUPERPOWER) but a context never maps a key twice.
  bind(b, kMenuX, SDL_SCANCODE_E);
  bind(b, kMenuY, SDL_SCANCODE_F);
  // iter-466 controls2: R3 = GAME_LLOCK_CYCLE (sub_8229B630: EventButton(7, every 10 ticks)) - the
  // game's target cycle while locked had no key
  bind(b, kCycleTarget, SDL_SCANCODE_Q);
  return b;
}

Config parseIni(const std::string& text, const Config& base, std::string* errors) {
  Config c = base;
  std::istringstream in(text);
  std::string raw, section;
  int lineNo = 0;
  auto err = [&](const std::string& m) {
    if (errors) *errors += "input.ini:" + std::to_string(lineNo) + ": " + m + "\n";
  };
  while (std::getline(in, raw)) {
    ++lineNo;
    const size_t hash = raw.find_first_of("#;");
    std::string line = trim(hash == std::string::npos ? raw : raw.substr(0, hash));
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) { err("not 'key = value': " + line); continue; }
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (ieq(key, "version") || ieq(key, "glyphs")) continue;   // glyphs: the input lane's key
    if (ieq(section, "mouse")) {
      bool bv = false;
      if (ieq(key, "look")) { if (parseBool(val, &bv)) c.mouse.look = bv; else err("bad bool"); }
      else if (ieq(key, "invert_x")) { if (parseBool(val, &bv)) c.mouse.invertX = bv; else err("bad bool"); }
      else if (ieq(key, "invert_y")) { if (parseBool(val, &bv)) c.mouse.invertY = bv; else err("bad bool"); }
      else if (ieq(key, "sensitivity")) {
        char* end = nullptr;
        const float f = std::strtof(val.c_str(), &end);
        if (end && *end == 0 && f > 0.0f && f <= 20.0f) c.mouse.sensitivity = f;
        else err("sensitivity must be a number in (0, 20]");
      } else err("unknown [mouse] key '" + key + "'");
      continue;
    }
    if (!section.empty() && !ieq(section, "controls")) {
      // other sections ([pad], [glyphs], ...) belong to other owners: ignored here
      continue;
    }
    int a = -1;
    for (int i = 0; i < kActionCount; ++i)
      if (ieq(key, kActionNames[i])) { a = i; break; }
    if (a < 0) { err("unknown action '" + key + "'"); continue; }
    uint16_t ins[kInputsPerAction] = {};
    int n = 0;
    bool ok = true;
    std::istringstream vs(val);
    std::string tok;
    while (std::getline(vs, tok, ',')) {
      uint16_t code = 0;
      if (!parseInputName(tok.c_str(), &code)) { err("unknown input '" + trim(tok) + "'"); ok = false; break; }
      if (code == kInNone) continue;
      if (n >= kInputsPerAction) { err("more than 4 inputs for " + key); ok = false; break; }
      ins[n++] = code;
    }
    if (!ok) continue;
    for (int k = 0; k < kInputsPerAction; ++k) c.bindings.in[a][k] = ins[k];
  }
  return c;
}

std::string defaultIni() {
  const Config c;
  std::string s =
      "# Superman Returns (recomp) - PC controls. docs/PC_CONTROLS.md explains every line.\n"
      "# action = input[, input ...]   (up to four; 'none' unbinds). PC SETTINGS > CONTROLS edits it.\n"
      "# A key IS the game's action: it drives the game's own input commands (docs/PC_CONTROLS.md 2).\n"
      "# inputs: SDL key names (W, Space, Left Shift, Left Ctrl, Left Alt, Tab, Return, 1 ...),\n"
      "#         Mouse1 (left) Mouse2 (right) Mouse3 (middle) Mouse4 Mouse5 WheelUp WheelDown\n"
      "# A controller always works too; the game's own CONTROLLER OPTIONS (configuration,\n"
      "# X/Y inversion, smart camera) apply to the mouse and keyboard as well.\n"
      "version = 3\n\n[controls]\n";
  for (int a = 0; a < kActionCount; ++a) {
    std::string v;
    for (int k = 0; k < kInputsPerAction; ++k) {
      const uint16_t in = c.bindings.in[a][k];
      if (in == kInNone) continue;
      if (!v.empty()) v += ", ";
      v += inputName(in);
    }
    s += std::string(kActionNames[a]) + " = " + (v.empty() ? "none" : v) + "\n";
  }
  s +=
      "\n[mouse]\n"
      "# mouse-look in gameplay (the menus get the mouse cursor)\n"
      "look = on\n"
      "# 1.0 = the default speed; 2.0 turns twice as far per mouse movement\n"
      "sensitivity = 1.0\n"
      "# extra inversion ON TOP of the game's own X / Y INVERSION options (normally off)\n"
      "invert_x = off\n"
      "invert_y = off\n"
      "\n[glyphs]\n"
      "# button prompts: auto (follow the device you last used) | xbox | ps4 | ps5 | switch | keyboard\n"
      "glyphs = auto\n";
  return s;
}

// ── iter-466 controls2: the remapper's vocabulary ───────────────────────────────────────────
namespace {
struct Meta { const char* label; const char* group; const char* control; const char* desc; uint8_t ctx; };
// ctx bits: where the mapper reads the action - 1 gameplay / mini-games, 2 menus, 4 cinematics
constexpr uint8_t G = 1, M = 2, C = 4;
const Meta kMeta[kActionCount] = {
    {"MOVE FORWARD", "MOVEMENT", "Left stick up (the configuration's move stick)", "Run on foot; thrust in flight. Menus: up.", G | M | C},
    {"MOVE BACK", "MOVEMENT", "Left stick down", "Run back; brake in flight. Menus: down.", G | M | C},
    {"MOVE LEFT", "MOVEMENT", "Left stick left", "Turn / strafe left. Menus: left.", G | M | C},
    {"MOVE RIGHT", "MOVEMENT", "Left stick right", "Turn / strafe right. Menus: right.", G | M | C},
    {"ATTACK", "ACTIONS", "X (Action3)", "ATTACK, ATTACK_HELD while held; with PICK UP the grab-and-attack combos.", G},
    {"BLOCK / DROP", "ACTIONS", "B (Action2)", "BLOCK while held, DROP on the press. Block + move = the Quantum Dash evade.", G},
    {"PICK UP / THROW", "ACTIONS", "A (Action1)", "GRAB, and THROW what you carry.", G},
    {"TAKE OFF / LAND", "ACTIONS", "Y (Action4)", "Take off and land; hold for a power launch / power drop.", G},
    {"SUPER SPEED", "ACTIONS", "RB (Shoulder2)", "SUPERSPEED while held; a tap on foot is the dash.", G},
    {"TARGET LOCK", "ACTIONS", "LT (Shoulder3)", "Lock on to a target (hold).", G},
    {"SUPERPOWER", "ACTIONS", "RT (Shoulder4)", "Use the selected superpower; also THROW.", G},
    {"RESET CAMERA / HOVER", "ACTIONS", "LB (Shoulder1)", "Reset the camera on foot; hover / airdrift in flight.", G},
    {"MAP", "GAME", "BACK (Select)", "Open the map.", G | M | C},
    {"PAUSE", "GAME", "START", "Pause; START in a cinematic.", G | M | C},
    {"HEAT VISION", "POWERS", "D-pad up", "Select heat vision (again: the strong version).", G},
    {"FREEZE BREATH", "POWERS", "D-pad right", "Select freeze breath.", G},
    {"SUPER BREATH", "POWERS", "D-pad left", "Select super breath.", G},
    {"D-PAD DOWN", "POWERS", "D-pad down", "No gameplay command reads it (x-ray vision has no input in the game).", G},
    {"NEXT POWER", "POWERS", "D-pad (one press)", "Step to the next superpower.", G},
    {"PREVIOUS POWER", "POWERS", "D-pad (one press)", "Step to the previous superpower.", G},
    {"DODGE", "MOVEMENT", "B held + a move-stick flick", "The Quantum Dash evade in the direction you move (on foot).", G},
    {"LOOK UP", "CAMERA", "Right stick up", "Turn the camera with keys (the mouse turns it too).", G},
    {"LOOK DOWN", "CAMERA", "Right stick down", "Turn the camera with keys.", G},
    {"LOOK LEFT", "CAMERA", "Right stick left", "Turn the camera with keys.", G},
    {"LOOK RIGHT", "CAMERA", "Right stick right", "Turn the camera with keys.", G},
    {"MENU UP", "MENUS", "D-pad up", "Menus: up. Gameplay: heat vision.", G | M | C},
    {"MENU DOWN", "MENUS", "D-pad down", "Menus: down.", G | M | C},
    {"MENU LEFT", "MENUS", "D-pad left", "Menus: left. Gameplay: super breath.", G | M | C},
    {"MENU RIGHT", "MENUS", "D-pad right", "Menus: right. Gameplay: freeze breath.", G | M | C},
    {"MENU SELECT", "MENUS", "A (CROSS)", "Menus: select. Cinematics: skip.", M | C},
    {"MENU BACK", "MENUS", "B (TRIANGLE)", "Menus: back.", M | C},
    {"MENU X ACTION", "MENUS", "X (SQUARE)", "Menus: the screen's X action (WEAR COSTUME).", M},
    {"MENU Y ACTION", "MENUS", "Y (CIRCLE)", "Menus: the screen's Y action (the map's FORTRESS OF SOLITUDE).", M},
    {"CYCLE TARGET", "ACTIONS", "R3 (right stick click)", "While locked on: the next target (repeats while held).", G},
};
bool sharedByDesign(Action a, Action b) {
  auto pair = [&](Action x, Action y) { return (a == x && b == y) || (a == y && b == x); };
  return pair(kPause, kMenuBack) || pair(kMenuUp, kPowerUp) || pair(kMenuRight, kPowerRight) ||
         pair(kMenuLeft, kPowerLeft) || pair(kMenuDown, kPowerDown) ||
         // a menu-only action and a gameplay-only one never meet (E = PICK UP and MENU X ...)
         false;
}
}  // namespace

const char* actionLabel(Action a) { return a < kActionCount ? kMeta[a].label : "?"; }
const char* actionGroup(Action a) { return a < kActionCount ? kMeta[a].group : "?"; }
const char* actionControl(Action a) { return a < kActionCount ? kMeta[a].control : "?"; }
const char* actionDesc(Action a) { return a < kActionCount ? kMeta[a].desc : "?"; }

std::string bindingText(const Bindings& b, Action a) {
  std::string v;
  if (a >= kActionCount) return "none";
  for (int k = 0; k < kInputsPerAction; ++k) {
    const uint16_t in = b.in[a][k];
    if (in == kInNone) continue;
    if (!v.empty()) v += ", ";
    v += inputName(in);
  }
  return v.empty() ? "none" : v;
}

bool parseBindingText(const std::string& text, uint16_t out[kInputsPerAction]) {
  for (int k = 0; k < kInputsPerAction; ++k) out[k] = kInNone;
  std::istringstream vs(text);
  std::string tok;
  int n = 0;
  while (std::getline(vs, tok, ',')) {
    uint16_t code = 0;
    if (!parseInputName(tok.c_str(), &code)) return false;
    if (code == kInNone) continue;
    bool dup = false;
    for (int k = 0; k < n; ++k) dup = dup || out[k] == code;
    if (dup) continue;
    if (n >= kInputsPerAction) return false;
    out[n++] = code;
  }
  return true;
}

std::vector<Conflict> findConflicts(const Bindings& b) {
  std::vector<Conflict> v;
  for (int a = 0; a < kActionCount; ++a)
    for (int c = a + 1; c < kActionCount; ++c) {
      if (!(kMeta[a].ctx & kMeta[c].ctx) || sharedByDesign(Action(a), Action(c))) continue;
      for (int i = 0; i < kInputsPerAction; ++i) {
        const uint16_t in = b.in[a][i];
        if (in == kInNone) continue;
        for (int j = 0; j < kInputsPerAction; ++j)
          if (b.in[c][j] == in) v.push_back({in, Action(a), Action(c)});
      }
    }
  return v;
}

uint16_t firstInput(const Bindings& b, Action a) {
  if (a >= kActionCount) return kInNone;
  for (int k = 0; k < kInputsPerAction; ++k)
    if (b.in[a][k] != kInNone) return b.in[a][k];
  return kInNone;
}

// ── lane E E3: the host screens' keys (pc_controls.hpp hostKeyAct) ───────────────────────────────────
namespace {
struct HostMenuRow { Action action; HostAct act; };
constexpr HostMenuRow kHostMenuRows[] = {
    {kMenuConfirm, HostAct::Accept}, {kMenuBack, HostAct::Back}, {kMenuX, HostAct::X},   {kMenuY, HostAct::Y},
    {kMenuUp, HostAct::Up},          {kMenuDown, HostAct::Down}, {kMenuLeft, HostAct::Left},
    {kMenuRight, HostAct::Right},
    {kMap, HostAct::View},   // the pad's BACK / VIEW in menus (primaryInputFor 0x0020) - PC SETTINGS: REVERT
};

HostAct legacyHostAct(uint16_t sc, bool shift) {
  switch (sc) {
    case SDL_SCANCODE_UP: return HostAct::Up;
    case SDL_SCANCODE_DOWN: return HostAct::Down;
    case SDL_SCANCODE_LEFT: return HostAct::Left;
    case SDL_SCANCODE_RIGHT: return HostAct::Right;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_Z: return HostAct::Accept;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_X: return HostAct::Back;
    case SDL_SCANCODE_SPACE: case SDL_SCANCODE_C: return HostAct::X;
    case SDL_SCANCODE_R: case SDL_SCANCODE_V: return HostAct::Y;
    case SDL_SCANCODE_Q: return HostAct::LB;
    case SDL_SCANCODE_E: return HostAct::RB;
    case SDL_SCANCODE_TAB: return shift ? HostAct::LB : HostAct::RB;
    case SDL_SCANCODE_PAGEUP: return HostAct::LT;
    case SDL_SCANCODE_PAGEDOWN: return HostAct::RT;
    default: return HostAct::None;
  }
}

// Player keymap step 3: the host-only keys (used only when no menu binding names the key). Coordinator ruling
// 2026-10-23: tabs are Tab / Shift+Tab; PgUp / PgDn stay LT / RT; no Q / E tab keys and no Space / R fallbacks (a key
// a cap does not show must not act).
HostAct hostOnlyAct(uint16_t sc, bool shift) {
  switch (sc) {
    case SDL_SCANCODE_UP: return HostAct::Up;
    case SDL_SCANCODE_DOWN: return HostAct::Down;
    case SDL_SCANCODE_LEFT: return HostAct::Left;
    case SDL_SCANCODE_RIGHT: return HostAct::Right;
    case SDL_SCANCODE_TAB: return shift ? HostAct::LB : HostAct::RB;
    case SDL_SCANCODE_PAGEUP: return HostAct::LT;
    case SDL_SCANCODE_PAGEDOWN: return HostAct::RT;
    default: return HostAct::None;
  }
}

// The fixed keys a cap may show for an act, in preference order (the first one that still does the act wins).
struct CapKey { uint16_t sc; bool shift; };
int fixedCapKeys(HostAct a, Keymap km, CapKey* out) {
  int n = 0;
  auto add = [&](uint16_t sc, bool shift) { out[n++] = CapKey{sc, shift}; };
  const bool legacy = km == Keymap::LegacyTest;
  switch (a) {
    case HostAct::Accept: add(SDL_SCANCODE_RETURN, false); add(SDL_SCANCODE_KP_ENTER, false); break;
    case HostAct::Back: add(SDL_SCANCODE_ESCAPE, false); add(SDL_SCANCODE_BACKSPACE, false); break;
    case HostAct::X: if (legacy) add(SDL_SCANCODE_SPACE, false); break;   // Player: MenuX's binding only
    case HostAct::Y: if (legacy) add(SDL_SCANCODE_R, false); break;       // Player: MenuY's binding only
    case HostAct::LB:
      if (legacy) add(SDL_SCANCODE_Q, false);
      add(SDL_SCANCODE_TAB, true);
      break;
    case HostAct::RB:
      if (legacy) add(SDL_SCANCODE_E, false);
      add(SDL_SCANCODE_TAB, false);
      break;
    case HostAct::LT: add(SDL_SCANCODE_PAGEUP, false); break;
    case HostAct::RT: add(SDL_SCANCODE_PAGEDOWN, false); break;
    case HostAct::Up: add(SDL_SCANCODE_UP, false); break;
    case HostAct::Down: add(SDL_SCANCODE_DOWN, false); break;
    case HostAct::Left: add(SDL_SCANCODE_LEFT, false); break;
    case HostAct::Right: add(SDL_SCANCODE_RIGHT, false); break;
    default: break;
  }
  return n;
}
}  // namespace

HostAct hostKeyAct(uint16_t sc, bool shift, Keymap km, const Bindings& b) {
  if (sc == kInNone || sc >= kInMouseLeft) return HostAct::None;
  if (km == Keymap::LegacyTest) return legacyHostAct(sc, shift);
  if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) return HostAct::Accept;   // 1. failsafes
  if (sc == SDL_SCANCODE_ESCAPE) return HostAct::Back;
  for (const HostMenuRow& r : kHostMenuRows)                                                // 2. menu bindings
    for (int k = 0; k < kInputsPerAction; ++k)
      if (b.in[r.action][k] == sc) return r.act;
  return hostOnlyAct(sc, shift);                                                            // 3. host-only keys
}

std::string keyCapName(uint16_t sc) {
  switch (sc) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "Enter";
    case SDL_SCANCODE_ESCAPE: return "Esc";
    case SDL_SCANCODE_BACKSPACE: return "Bksp";
    case SDL_SCANCODE_PAGEUP: return "PgUp";
    case SDL_SCANCODE_PAGEDOWN: return "PgDn";
    case SDL_SCANCODE_DELETE: return "Del";
    case SDL_SCANCODE_INSERT: return "Ins";
    case SDL_SCANCODE_LSHIFT: return "LShift";
    case SDL_SCANCODE_RSHIFT: return "RShift";
    case SDL_SCANCODE_LCTRL: return "LCtrl";
    case SDL_SCANCODE_RCTRL: return "RCtrl";
    case SDL_SCANCODE_LALT: return "LAlt";
    case SDL_SCANCODE_RALT: return "RAlt";
    default: break;
  }
  const std::string n = inputName(sc);
  return n == "none" ? std::string() : n;
}

uint16_t hostKeyFor(HostAct a, Keymap km, const Bindings& b, bool* shift) {
  if (shift) *shift = false;
  if (a == HostAct::None) return kInNone;
  CapKey cand[12];
  int n = 0;
  if (km == Keymap::Player)
    for (const HostMenuRow& r : kHostMenuRows)
      if (r.act == a)
        for (int k = 0; k < kInputsPerAction && n < 8; ++k)
          if (b.in[r.action][k] != kInNone && b.in[r.action][k] < kInMouseLeft)
            cand[n++] = CapKey{b.in[r.action][k], false};
  n += fixedCapKeys(a, km, cand + n);
  for (int i = 0; i < n; ++i)
    if (hostKeyAct(cand[i].sc, cand[i].shift, km, b) == a) {
      if (shift) *shift = cand[i].shift;
      return cand[i].sc;
    }
  return kInNone;
}

std::string hostKeyCap(HostAct a, Keymap km, const Bindings& b) {
  bool shift = false;
  const uint16_t sc = hostKeyFor(a, km, b, &shift);
  if (sc == kInNone) return {};
  return shift ? "Shift+" + keyCapName(sc) : keyCapName(sc);
}

std::string hostMoveCap(Keymap km, const Bindings& b, bool leftRight) {
  auto moves = [&](uint16_t sc, HostAct a) { return hostKeyAct(sc, false, km, b) == a; };
  const bool arrows = moves(SDL_SCANCODE_UP, HostAct::Up) && moves(SDL_SCANCODE_DOWN, HostAct::Down) &&
                      (!leftRight || (moves(SDL_SCANCODE_LEFT, HostAct::Left) && moves(SDL_SCANCODE_RIGHT, HostAct::Right)));
  if (arrows) return {};
  auto cap = [&](HostAct a) {
    const std::string c = hostKeyCap(a, km, b);
    return c.empty() ? std::string("-") : c;
  };
  std::string s = cap(HostAct::Up) + "/" + cap(HostAct::Down);
  if (leftRight) s += "/" + cap(HostAct::Left) + "/" + cap(HostAct::Right);
  return s;
}

bool hostPreviewKey(uint16_t sc, bool shift, Keymap km, const Bindings& b) {
  const HostAct a = hostKeyAct(sc, shift, km, b);
  return a == HostAct::RT || (km == Keymap::LegacyTest && a == HostAct::None && sc == SDL_SCANCODE_P);
}

uint16_t harnessKeyMods(const uint8_t* k) {
  if (!k) return 0;
  uint16_t m = 0;
  if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) m |= SDL_KMOD_SHIFT;
  if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]) m |= SDL_KMOD_CTRL;
  if (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]) m |= SDL_KMOD_ALT;
  return m;
}

uint16_t primaryInputFor(uint32_t bit, bool gameplay, const Bindings& b) {
  struct Row { uint32_t bit; Action game; Action menu; };
  static const Row kRows[] = {
      {0x4000, kAttack, kMenuX},                // X
      {0x2000, kBlock, kMenuBack},              // B
      {0x1000, kPickup, kMenuConfirm},          // A
      {0x8000, kTakeoff, kMenuY},               // Y
      {0x0200, kSuperspeed, kActionCount},      // RB
      {0x0100, kCameraHover, kActionCount},     // LB
      {0x0080, kCycleTarget, kActionCount},     // R3
      {0x0020, kMap, kMap},                     // BACK
      {0x0010, kPause, kPause},                 // START
      {0x0001, kPowerUp, kMenuUp},
      {0x0008, kPowerRight, kMenuRight},
      {0x0004, kPowerLeft, kMenuLeft},
      {0x0002, kPowerDown, kMenuDown},
      {kPadLT, kTargetLock, kActionCount},
      {kPadRT, kSuperpower, kActionCount},
  };
  for (const Row& r : kRows) {
    if (r.bit != bit) continue;
    const Action a = gameplay ? r.game : r.menu;
    if (a >= kActionCount) return kInNone;
    for (int k = 0; k < kInputsPerAction; ++k)
      if (b.in[a][k] != kInNone) return b.in[a][k];
    return kInNone;
  }
  return kInNone;
}

uint16_t mapLegacyTestKeys(const uint8_t* k, Pad* pad) {
  // One source of truth: the bgruns table (kernel/host_automation.cpp mapKeysToPad).
  if (!k) return 0;
  int16_t lx = pad ? pad->lx : 0, ly = pad ? pad->ly : 0, rx = pad ? pad->rx : 0, ry = pad ? pad->ry : 0;
  const uint16_t b = automation::mapKeysToPad(k, &lx, &ly, &rx, &ry);
  if (pad) {
    pad->buttons |= b;
    pad->lx = lx;
    pad->ly = ly;
    pad->rx = rx;
    pad->ry = ry;
  }
  return b;
}

}  // namespace sr::kernel::pccontrols
