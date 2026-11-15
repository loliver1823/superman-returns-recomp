// iter-464 input lane: the PC controller layer (kernel/host_input.{hpp,cpp}; docs/INPUT_GLYPHS.md).
//
// Run: build/difftest.exe --host-input-test     (exit = failed checks)
//
// What it proves, through the REAL module and SDL's own virtual joysticks (SDL3
// SDL_AttachVirtualJoystick - a device SDL treats like a plugged-in pad: it raises
// SDL_EVENT_GAMEPAD_ADDED/REMOVED, gets a gamepad mapping, and its VID/PID decide
// SDL_GetGamepadType):
//   P   keyboard prompts: the default prompt source names EXACTLY what the keyboard merge
//       maps (every named key, fed through the real kernel/host_automation.cpp mapKeysToPad,
//       presses that control; the controls it leaves unnamed - LT/RT/L3/R3/right stick - no
//       key presses); a registered source (the controls lane's) replaces it and bumps the
//       glyph generation; nullptr restores the default;
//   B   input.ini (the controls lane's file, format 2): only `glyphs =` is read, in any
//       section; other lines are ignored silently; a bad value is reported; the module never
//       writes the file; a player's pin applies at init, SR_GLYPHS wins over it;
//   H   hot-plug: an Xbox-type pad plugged in becomes player 1 (first device) and its
//       buttons/sticks/triggers read back in XInput's conventions; a PS4-type pad plugged in
//       second is NOT player 1 until used; pressing a button on it moves player 1 and the
//       glyph set; unplugging player 1's pad reports NOT CONNECTED until a device is used;
//       the keyboard reconnects player 1; plugging a pad back in while player 1 is lost
//       takes over; a Switch Pro-type pad maps to the Switch glyphs;
//   T   RELEASE_PLAN step 3 (SDL3): the device type -> glyph set for every family the glyphs
//       cover (360 / One, DS4, DualSense, Switch Pro, Joy-Con pair, a standard pad), each
//       through a real hot-plug + button press (last used device); a pad unplugged while a
//       button is HELD leaves nothing held (no stuck action) and the next pad reads clean;
//   R   rumble: setVibration + tick reach player 1's pad (the virtual pad's Rumble callback sees
//       the XInput left / right speeds as SDL's low / high motors) and 0 / 0 stops it; a move
//       to another device stops the old pad;
//   I   isolation: with SR_INSTANCE_ID set the module opens no controller - a plugged pad is
//       never player 1, readActivePad stays false, user 0 stays connected; glyphs off unless
//       SR_GLYPHS asks.
// Hermetic (SDL3): every real joystick backend is switched off by SDL hints at OVERRIDE priority
// (they outrank the module's SDL_SetHint calls; SDL3 snapshots the environment, so _putenv after
// SDL starts is not seen) - a pad on the machine, or another lane's ViGEm pad, cannot take part.
#include "kernel/host_input.hpp"
#include "kernel/host_automation.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace HI = sr::kernel::hostinput;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const std::string& name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name.c_str()); }
  else { g_fail++; std::printf("FAIL: %s\n", name.c_str()); }
}

void setEnv(const char* k, const char* v) {
#ifdef _WIN32
  _putenv_s(k, v ? v : "");
#else
  if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

// Every real-device joystick driver SDL3 has on Windows, plus the HIDAPI sub-drivers the module
// switches on by name (a sub-driver hint outranks the HIDAPI master hint).
const char* const kRealBackends[] = {
    SDL_HINT_XINPUT_ENABLED,       SDL_HINT_JOYSTICK_HIDAPI,      SDL_HINT_JOYSTICK_HIDAPI_PS3,
    SDL_HINT_JOYSTICK_HIDAPI_PS4,  SDL_HINT_JOYSTICK_HIDAPI_PS5,  SDL_HINT_JOYSTICK_HIDAPI_SWITCH,
    SDL_HINT_JOYSTICK_DIRECTINPUT, SDL_HINT_JOYSTICK_RAWINPUT,    SDL_HINT_JOYSTICK_WGI,
    SDL_HINT_JOYSTICK_GAMEINPUT,
};

// Pump SDL and feed every event to the module, as app/platform_sdl.cpp pumpEvents does.
int pump(int rounds = 4) {
  int n = 0;
  for (int i = 0; i < rounds; ++i) {
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e)) { HI::onEvent(e); ++n; }
    HI::tick();
    SDL_Delay(5);
  }
  return n;
}

// What the virtual pad's motors were last told (SDL calls the driver only on a change).
struct RumbleLog {
  int calls = 0;
  uint16_t lo = 0, hi = 0;
};
bool SDLCALL onRumble(void* ud, Uint16 lo, Uint16 hi) {
  auto* r = static_cast<RumbleLog*>(ud);
  r->calls++;
  r->lo = lo;
  r->hi = hi;
  return true;
}

struct VPad {
  SDL_JoystickID id = 0;   // SDL3: 0 = none
  SDL_Joystick* joy = nullptr;
};

VPad attach(uint16_t vid, uint16_t pid, const char* name, RumbleLog* rumble = nullptr) {
  SDL_VirtualJoystickDesc d;
  SDL_INIT_INTERFACE(&d);
  d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  d.naxes = SDL_GAMEPAD_AXIS_COUNT;
  d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  d.vendor_id = vid;
  d.product_id = pid;
  d.name = name;
  d.userdata = rumble;
  d.Rumble = rumble ? onRumble : nullptr;
  VPad v;
  v.id = SDL_AttachVirtualJoystick(&d);
  if (v.id) v.joy = SDL_OpenJoystick(v.id);
  return v;
}

void detach(VPad& v) {
  if (!v.id) return;
  if (v.joy) SDL_CloseJoystick(v.joy);
  v.joy = nullptr;
  SDL_DetachVirtualJoystick(v.id);
  v.id = 0;
}

void press(VPad& v, SDL_GamepadButton b, bool down) {
  SDL_SetJoystickVirtualButton(v.joy, b, down);
}
void axis(VPad& v, SDL_GamepadAxis a, int16_t val) {
  SDL_SetJoystickVirtualAxis(v.joy, a, val);
}
}  // namespace

// --host-input-live <seconds>: the REAL devices (and ViGEm virtual pads) through the module for
// a while, printing every player-1 / connection change and the pad state while anything is
// held. Used with tools/glyphs/vpad_hotplug.py to check hot-plug without a game process.
// vpad = true (--host-input-vpad): as an automated roll with SR_HARNESS_VPAD=1 - only ViGEm
// virtual pads may be opened (kernel/vpad_filter.hpp), every other controller is rejected.
int runHostInputLive(int seconds, bool vpad) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec) / "sr_host_input_live";
  fs::create_directories(tmp, ec);
  setEnv("SR_CONTENT_DIR", tmp.string().c_str());
  setEnv("SR_INSTANCE_ID", vpad ? "vpadtest" : nullptr);
  setEnv("SR_HARNESS_VPAD", vpad ? "1" : nullptr);
  setEnv("SR_HARNESS_REAL_INPUT", nullptr);
  // HI::init() sets the joystick hints and THEN initialises the controller subsystem, as in
  // the game - initialising SDL first here would bypass the hints (JOYSTICK_THREAD).
  HI::resetForTest();
  HI::init();
  const Uint64 end = SDL_GetTicks() + Uint64(seconds) * 1000u;
  HI::Device lastDev = HI::Device::None;
  bool lastConn = true;
  uint16_t lastBtn = 0;
  while (SDL_GetTicks() < end) {
    pump(1);
    const HI::Device dv = HI::activeDevice();
    const bool conn = HI::player1Connected();
    HI::PadState ps;
    const bool pad = HI::readActivePad(&ps);
    if (dv != lastDev || conn != lastConn || (pad && ps.buttons != lastBtn)) {
      std::printf("[LIVE] t=%llu player1=%s connected=%d glyphs=%s pad=%d btn=0x%04X LT=%u RT=%u LX=%d LY=%d\n",
                  static_cast<unsigned long long>(SDL_GetTicks()), HI::deviceName(dv), conn ? 1 : 0,
                  HI::glyphSetName(HI::activeGlyphSet()), pad ? 1 : 0, pad ? ps.buttons : 0,
                  pad ? ps.lt : 0, pad ? ps.rt : 0, pad ? ps.lx : 0, pad ? ps.ly : 0);
      std::fflush(stdout);
      lastDev = dv; lastConn = conn; lastBtn = pad ? ps.buttons : 0;
    }
  }
  HI::shutdown();
  return 0;
}

int runHostInputTest() {
  std::printf("[HOST_INPUT_TEST] SDL controllers, hot-plug, player 1, prompts, isolation\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec) / "sr_host_input_test";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  setEnv("SR_CONTENT_DIR", tmp.string().c_str());  // input.ini goes here, never the player's
  setEnv("SR_GLYPHS", nullptr);
  setEnv("SR_HARNESS_REAL_INPUT", nullptr);

  // ── P: keyboard prompts ───────────────────────────────────────────────────
  {
    // Every control the default source names: that key, alone, through the real keymap.
    bool exact = true;
    int named = 0;
    for (int c = 0; c < HI::kControlCount; ++c) {
      const HI::Control ctl = HI::Control(c);
      const std::string nm = HI::defaultPromptInputFor(ctl);
      if (nm.empty()) continue;
      ++named;
      const SDL_Scancode sc = SDL_GetScancodeFromName(nm.c_str());
      uint8_t keys[SDL_SCANCODE_COUNT] = {};
      if (sc != SDL_SCANCODE_UNKNOWN) keys[sc] = 1;
      int16_t lx = 0, ly = 0;
      const uint16_t m = sr::kernel::automation::mapKeysToPad(keys, &lx, &ly);
      bool ok = sc != SDL_SCANCODE_UNKNOWN;
      switch (ctl) {
        case HI::kLStickUp: ok = ok && ly > 0 && m == 0; break;
        case HI::kLStickDown: ok = ok && ly < 0 && m == 0; break;
        case HI::kLStickLeft: ok = ok && lx < 0 && m == 0; break;
        case HI::kLStickRight: ok = ok && lx > 0 && m == 0; break;
        default: ok = ok && m == HI::controlButtonBit(ctl) && m != 0;
      }
      if (!ok) { exact = false; std::printf("    %s -> '%s' mask=0x%04X lx=%d ly=%d\n", HI::controlName(ctl), nm.c_str(), m, lx, ly); }
    }
    // The unnamed controls: no single key presses them.
    bool unmapped = true;
    for (int sc = 1; sc < SDL_SCANCODE_COUNT; ++sc) {
      uint8_t keys[SDL_SCANCODE_COUNT] = {};
      keys[sc] = 1;
      int16_t lx = 0, ly = 0;
      const uint16_t m = sr::kernel::automation::mapKeysToPad(keys, &lx, &ly);
      if (m & (0x0040 | 0x0080)) unmapped = false;  // L3 / R3 (the keymap has no trigger/RS output)
    }
    for (HI::Control c : {HI::kLT, HI::kRT, HI::kLThumb, HI::kRThumb, HI::kRStickUp, HI::kRStickDown,
                          HI::kRStickLeft, HI::kRStickRight})
      if (!HI::defaultPromptInputFor(c).empty()) unmapped = false;
    check(exact && named == 16, "P1 the default prompt source names exactly what the keymap maps (" +
                                    std::to_string(named) + " controls, each key presses its control)");
    check(unmapped, "P2 LT/RT/L3/R3/right stick: unnamed, and no key presses them");
    check(HI::promptInputFor(HI::kA) == "Space" && HI::promptInputFor(HI::kStart) == "Return",
          "P3 no source registered: prompts use the default (A = Space, START = Return)");
    const uint32_t g0 = HI::glyphGeneration();
    HI::setKeyPromptSource([](HI::Control c) -> std::string {
      return c == HI::kB ? "Mouse2" : c == HI::kRStickUp ? "MouseMove" : "";
    });
    check(HI::promptInputFor(HI::kB) == "Mouse2" && HI::promptInputFor(HI::kRStickUp) == "MouseMove" &&
              HI::promptInputFor(HI::kA).empty() && HI::glyphGeneration() != g0,
          "P4 a registered source (the controls lane's) answers every prompt; the generation bumps");
    HI::setKeyPromptSource(nullptr);
    check(HI::promptInputFor(HI::kB) == "X", "P5 nullptr restores the default source");
  }

  // ── B: input.ini is the controls lane's file; the glyph pin is the one key read here ──
  {
    HI::GlyphSet pin = HI::GlyphSet::Xbox360;
    bool isAuto = true;
    std::string err;
    const std::string v2 =
        "\xEF\xBB\xBFversion = 2\r\nglyphs = PS5   # pin\r\n\r\n[controls]\r\nAttack = Mouse1\r\n"
        "TargetLock = Mouse3, Tab\r\n[mouse]\r\nlook = on\r\n";
    check(HI::readGlyphPin(v2, &pin, &isAuto, &err) && !isAuto && pin == HI::GlyphSet::PS5 && err.empty(),
          "B1 format-2 file: glyphs = PS5 pins PS5 (BOM, CRLF, comment); controls lines ignored silently");
    pin = HI::GlyphSet::Xbox360; isAuto = false; err.clear();
    check(HI::readGlyphPin("[controls]\nAttack = Mouse1\n[prompts]\nglyphs = auto\n", &pin, &isAuto, &err) && isAuto,
          "B2 glyphs = auto in any section");
    err.clear();
    check(!HI::readGlyphPin("version = 2\n[controls]\nAttack = Mouse1\n", &pin, &isAuto, &err) && err.empty(),
          "B3 no glyphs line: nothing found, nothing reported");
    check(!HI::readGlyphPin("glyphs = gamecube\n", &pin, &isAuto, &err) && err.find("gamecube") != std::string::npos,
          "B4 a bad glyphs value is reported");
  }

  // ── H: hot-plug through SDL virtual joysticks ─────────────────────────────
  for (const char* h : kRealBackends) SDL_SetHintWithPriority(h, "0", SDL_HINT_OVERRIDE);
  setEnv("SR_INSTANCE_ID", nullptr);
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    std::printf("FAIL: SDL_InitSubSystem(GAMEPAD): %s\n", SDL_GetError());
    return ++g_fail;
  }
  HI::resetForTest();
  HI::init();
  pump();
  check(!fs::exists(tmp / "settings" / "input.ini", ec),
        "H0 the input lane never writes settings/input.ini (the controls lane's file)");
  check(HI::glyphsEnabled() && HI::activeDevice() == HI::Device::None && HI::player1Connected(),
        "H1 player: glyphs on, no device yet, user 0 connected");

  RumbleLog xbRumble;
  VPad xb = attach(0x045E, 0x028E, "Virtual Xbox 360 pad", &xbRumble);
  pump();
  check(xb.id != 0, "H2 virtual Xbox pad attached");
  check(HI::activeDevice() == HI::Device::Xbox, "H3 first pad plugged in = player 1 (type xbox)");
  check(HI::activeGlyphSet() == HI::GlyphSet::Xbox360, "H4 xbox -> the game's own 360 prompts");
  press(xb, SDL_GAMEPAD_BUTTON_SOUTH, true);
  axis(xb, SDL_GAMEPAD_AXIS_LEFTY, -32768);   // SDL: up = negative
  axis(xb, SDL_GAMEPAD_AXIS_RIGHTX, 12345);
  axis(xb, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
  pump();
  HI::PadState ps;
  bool got = HI::readActivePad(&ps);
  check(got && (ps.buttons & 0x1000) && ps.ly == 32767 && ps.rx == 12345 && ps.rt == 255 && ps.lt == 0,
        "H5 A held, stick up = +32767 (XInput +Y up), RX passes through, RT 0..255");
  press(xb, SDL_GAMEPAD_BUTTON_SOUTH, false);
  axis(xb, SDL_GAMEPAD_AXIS_LEFTY, 0);
  axis(xb, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
  pump();

  // ── R: rumble reaches player 1's pad ──────────────────────────────────────
  HI::setVibration(30000, 12000);
  pump();
  check(xbRumble.calls > 0 && xbRumble.lo == 30000 && xbRumble.hi == 12000,
        "R1 XamInputSetState speeds reach player 1's pad (left -> low motor 30000, right -> high 12000)");
  HI::setVibration(0, 0);
  pump();
  check(xbRumble.lo == 0 && xbRumble.hi == 0, "R2 0 / 0 stops both motors");
  HI::setVibration(20000, 20000);
  pump();

  VPad ds4 = attach(0x054C, 0x09CC, "Virtual DualShock 4");
  pump();
  check(ds4.id != 0 && HI::activeDevice() == HI::Device::Xbox,
        "H6 a second pad plugged in is usable but not player 1 until used");
  press(ds4, SDL_GAMEPAD_BUTTON_EAST, true);
  pump();
  check(HI::activeDevice() == HI::Device::PS4 && HI::activeGlyphSet() == HI::GlyphSet::PS4,
        "H7 a button on the DualShock 4 makes it player 1 -> PS4 prompts");
  check(xbRumble.lo == 0 && xbRumble.hi == 0,
        "R3 player 1 moved to another pad: the old pad's motors are stopped");
  HI::setVibration(0, 0);
  got = HI::readActivePad(&ps);
  check(got && (ps.buttons & 0x2000) && !(ps.buttons & 0x1000), "H8 player 1 now reads the DS4 (B held)");
  press(ds4, SDL_GAMEPAD_BUTTON_EAST, false);
  axis(xb, SDL_GAMEPAD_AXIS_LEFTX, 9000);  // below half travel: no takeover
  pump();
  check(HI::activeDevice() == HI::Device::PS4, "H9 a small stick nudge on the idle pad does not steal player 1");
  axis(xb, SDL_GAMEPAD_AXIS_LEFTX, 30000);  // past half travel: takeover
  pump();
  check(HI::activeDevice() == HI::Device::Xbox, "H10 a full stick push on the Xbox pad takes player 1 back");
  axis(xb, SDL_GAMEPAD_AXIS_LEFTX, 0);
  press(ds4, SDL_GAMEPAD_BUTTON_START, true);
  pump();
  press(ds4, SDL_GAMEPAD_BUTTON_START, false);
  pump();
  check(HI::activeDevice() == HI::Device::PS4, "H11 back to the DS4");
  detach(ds4);
  pump();
  check(!HI::player1Connected() && !HI::readActivePad(&ps),
        "H12 unplugging player 1's pad: user 0 NOT CONNECTED, no pad state");
  HI::noteKeyboardUse();
  check(HI::player1Connected() && HI::activeDevice() == HI::Device::Keyboard &&
            HI::activeGlyphSet() == HI::GlyphSet::Keyboard && !HI::readActivePad(&ps),
        "H13 the keyboard reconnects player 1 -> keyboard prompts, no pad state");
  press(xb, SDL_GAMEPAD_BUTTON_WEST, true);
  pump();
  press(xb, SDL_GAMEPAD_BUTTON_WEST, false);
  pump();
  check(HI::activeDevice() == HI::Device::Xbox, "H14 a pad button takes player 1 back from the keyboard");
  detach(xb);
  pump();
  check(!HI::player1Connected(), "H15 unplug the Xbox pad (player 1) -> NOT CONNECTED");
  VPad sw = attach(0x057E, 0x2009, "Virtual Switch Pro");
  pump();
  check(HI::player1Connected() && HI::activeDevice() == HI::Device::Switch &&
            HI::activeGlyphSet() == HI::GlyphSet::Switch,
        "H16 plugging a pad in while player 1 is lost: it takes over (Switch Pro -> Switch prompts)");
  const uint32_t g0 = HI::glyphGeneration();
  HI::noteKeyboardUse();
  check(HI::glyphGeneration() != g0, "H17 a device change bumps the glyph generation");
  press(sw, SDL_GAMEPAD_BUTTON_SOUTH, true);
  pump();
  press(sw, SDL_GAMEPAD_BUTTON_SOUTH, false);
  pump();
  check(HI::activeDevice() == HI::Device::Switch, "H18 back to the Switch Pro pad");

  // ── T: each family's glyph set through a real plug-in + press (last used device) ──
  struct Family {
    uint16_t vid, pid;
    const char* name;
    HI::Device dev;
    HI::GlyphSet glyphs;
  };
  const Family fams[] = {
      {0x045E, 0x02EA, "Virtual Xbox One pad", HI::Device::Xbox, HI::GlyphSet::Xbox360},
      {0x054C, 0x0CE6, "Virtual DualSense", HI::Device::PS5, HI::GlyphSet::PS5},
      {0x054C, 0x05C4, "Virtual DualShock 4 (v1)", HI::Device::PS4, HI::GlyphSet::PS4},
      {0x057E, 0x2008, "Virtual Joy-Con pair", HI::Device::Switch, HI::GlyphSet::Switch},
      {0x1234, 0x5678, "Virtual standard pad", HI::Device::Generic, HI::GlyphSet::Xbox360},
  };
  for (const Family& f : fams) {
    VPad v = attach(f.vid, f.pid, f.name);
    pump();
    const bool notYet = HI::activeDevice() == HI::Device::Switch;   // plugged in, not used yet
    press(v, SDL_GAMEPAD_BUTTON_NORTH, true);
    pump();
    got = HI::readActivePad(&ps);
    check(v.id != 0 && notYet && HI::activeDevice() == f.dev && HI::activeGlyphSet() == f.glyphs &&
              got && (ps.buttons & 0x8000),
          std::string("T ") + f.name + " (" + HI::deviceName(f.dev) + "): plugged in it waits; a press makes it player 1 -> " +
              HI::glyphSetName(f.glyphs) + " prompts, Y held");
    press(v, SDL_GAMEPAD_BUTTON_NORTH, false);
    pump();
    // back to the Switch pad so the next family starts from the same state
    press(sw, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump();
    press(sw, SDL_GAMEPAD_BUTTON_SOUTH, false);
    pump();
    detach(v);
    pump();
  }
  check(HI::activeDevice() == HI::Device::Switch && HI::player1Connected(),
        "T6 unplugging pads that are not player 1 leaves player 1 alone");
  {
    // Unplug WHILE HELD: nothing stays pressed (RELEASE_PLAN 14: a disconnect releases its action).
    VPad held = attach(0x045E, 0x028E, "Virtual Xbox 360 pad (held)");
    pump();
    press(held, SDL_GAMEPAD_BUTTON_SOUTH, true);
    axis(held, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    pump();
    const bool wasHeld = HI::readActivePad(&ps) && (ps.buttons & 0x1000) && ps.lt == 255;
    detach(held);
    pump();
    const bool none = !HI::readActivePad(&ps) && !HI::player1Connected();
    VPad next = attach(0x045E, 0x028E, "Virtual Xbox 360 pad (next)");
    pump();
    got = HI::readActivePad(&ps);
    check(wasHeld && none && got && ps.buttons == 0 && ps.lt == 0 && HI::activeDevice() == HI::Device::Xbox,
          "T7 unplugging player 1's pad with A + LT held: no pad state, NOT CONNECTED; the next pad reads clean");
    detach(next);
    pump();
  }
  detach(sw);
  pump();

  // ── I: isolation on automated runs ────────────────────────────────────────
  setEnv("SR_INSTANCE_ID", "host-input-test");
  HI::resetForTest();
  HI::init();
  pump();
  check(!HI::glyphsEnabled(), "I1 automated run: glyph substitution off by default");
  VPad iso = attach(0x045E, 0x028E, "Virtual Xbox 360 pad (isolated)");
  pump();
  press(iso, SDL_GAMEPAD_BUTTON_SOUTH, true);
  pump();
  check(HI::activeDevice() == HI::Device::None && !HI::readActivePad(&ps) && HI::player1Connected() &&
            !HI::realInputAllowed(),
        "I2 automated run: a plugged pad is never opened, never player 1; user 0 stays connected");
  detach(iso);
  pump();
  setEnv("SR_GLYPHS", "ps5");
  HI::resetForTest();
  HI::init();
  check(HI::glyphsEnabled() && HI::activeGlyphSet() == HI::GlyphSet::PS5,
        "I3 SR_GLYPHS=ps5 turns glyphs on and pins PS5 in an automated run");
  setEnv("SR_GLYPHS", nullptr);
  setEnv("SR_INSTANCE_ID", "host-input-test");
  setEnv("SR_HARNESS_REAL_INPUT", "1");
  HI::resetForTest();
  HI::init();
  VPad opt = attach(0x045E, 0x028E, "Virtual Xbox 360 pad (opt-in)");
  pump();
  check(HI::activeDevice() == HI::Device::Xbox, "I4 SR_HARNESS_REAL_INPUT=1 opts an automated run back in");
  detach(opt);
  pump();
  setEnv("SR_HARNESS_REAL_INPUT", nullptr);
  setEnv("SR_INSTANCE_ID", nullptr);

  // ── B5/B6: a player's glyph pin at init; SR_GLYPHS wins ────────────────────
  fs::create_directories(tmp / "settings", ec);
  {
    std::ofstream o(tmp / "settings" / "input.ini", std::ios::binary);
    o << "version = 2\nglyphs = switch\n[controls]\nAttack = Mouse1\n";
  }
  HI::resetForTest();
  HI::init();
  check(HI::glyphsEnabled() && HI::activeGlyphSet() == HI::GlyphSet::Switch,
        "B5 player run: input.ini glyphs = switch pins the Switch prompts");
  setEnv("SR_GLYPHS", "ps4");
  HI::resetForTest();
  HI::init();
  check(HI::activeGlyphSet() == HI::GlyphSet::PS4, "B6 SR_GLYPHS=ps4 wins over the file");
  setEnv("SR_GLYPHS", nullptr);
  HI::resetForTest();
  for (const char* h : kRealBackends) SDL_ResetHint(h);

  fs::remove_all(tmp, ec);
  std::printf("[HOST_INPUT_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
