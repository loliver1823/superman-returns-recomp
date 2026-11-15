// iter-464 controls lane: difftest --pc-controls-test (docs/PC_CONTROLS.md section 9).
// Hermetic: the pure binding / ini / mapper code, no SDL video, no guest.
#include "../../kernel/pc_controls.hpp"
#include "../../kernel/pc_controls_map.hpp"
#include "../../kernel/pc_moves.hpp"
#include "../../kernel/host_input.hpp"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace pc = sr::kernel::pccontrols;

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
  if (ok) { ++g_pass; return; }
  ++g_fail;
  std::printf("  FAIL: %s\n", what);
}

struct Keys {
  uint8_t k[512] = {};
  Keys& on(SDL_Scancode s) { k[s] = 1; return *this; }
};

pc::Pad run(pc::Mapper& m, const pc::PollIn& in, const pc::Config& c, pc::PollOut* o = nullptr) {
  pc::Pad p;
  m.poll(in, c, &p, o);
  return p;
}

void testNames() {
  uint16_t v = 0;
  check(pc::parseInputName("Mouse1", &v) && v == pc::kInMouseLeft, "Mouse1 parses");
  check(pc::parseInputName("wheeldown", &v) && v == pc::kInWheelDown, "WheelDown case-insensitive");
  check(pc::parseInputName("Left Shift", &v) && v == SDL_SCANCODE_LSHIFT, "Left Shift parses");
  check(pc::parseInputName("left alt", &v) && v == SDL_SCANCODE_LALT, "left alt parses");
  check(pc::parseInputName("none", &v) && v == pc::kInNone, "none parses");
  check(!pc::parseInputName("NotAKey", &v), "unknown name refused");
  check(pc::inputName(pc::kInMouseMiddle) == "Mouse3", "Mouse3 name");
  check(pc::inputName(SDL_SCANCODE_SPACE) == "Space", "Space name");
}

void testDefaultsAndIni() {
  const pc::Bindings d = pc::defaultBindings();
  check(d.in[pc::kAttack][0] == pc::kInMouseLeft, "Attack = Mouse1");
  check(d.in[pc::kBlock][0] == pc::kInMouseRight, "Block = Mouse2");
  check(d.in[pc::kPickup][0] == SDL_SCANCODE_E, "Pickup = E");
  check(d.in[pc::kTakeoff][0] == SDL_SCANCODE_SPACE, "Takeoff = Space");
  check(d.in[pc::kSuperspeed][0] == SDL_SCANCODE_LSHIFT, "Superspeed = Left Shift");
  check(d.in[pc::kTargetLock][0] == pc::kInMouseMiddle && d.in[pc::kTargetLock][1] == SDL_SCANCODE_TAB,
        "TargetLock = Mouse3, Tab");
  check(d.in[pc::kSuperpower][0] == SDL_SCANCODE_F, "Superpower = F");
  check(d.in[pc::kCameraHover][0] == SDL_SCANCODE_LCTRL, "CameraHover = Left Ctrl");
  check(d.in[pc::kMap][0] == SDL_SCANCODE_M, "Map = M");
  check(d.in[pc::kPause][0] == SDL_SCANCODE_ESCAPE, "Pause = Esc");
  check(d.in[pc::kDodge][0] == SDL_SCANCODE_LALT, "Dodge = Left Alt");
  check(d.in[pc::kPowerNext][0] == pc::kInWheelDown && d.in[pc::kPowerPrev][0] == pc::kInWheelUp,
        "wheel cycles powers");

  // the defaults file parses back to the defaults, with no errors
  std::string err;
  const pc::Config back = pc::parseIni(pc::defaultIni(), pc::Config{}, &err);
  check(err.empty(), "defaultIni parses without errors");
  check(std::memcmp(&back.bindings, &d, sizeof d) == 0, "defaultIni round-trips the bindings");

  err.clear();
  const pc::Config c = pc::parseIni(
      "[controls]\nAttack = F, Mouse4\nDodge = none\nBogus = W\nBlock = Nope\n"
      "[mouse]\nsensitivity = 2.5\ninvert_y = on\n[pad]\nLT = Q\n",
      pc::Config{}, &err);
  check(c.bindings.in[pc::kAttack][0] == SDL_SCANCODE_F && c.bindings.in[pc::kAttack][1] == pc::kInMouseX1,
        "rebind Attack = F, Mouse4");
  check(c.bindings.in[pc::kDodge][0] == pc::kInNone, "none unbinds Dodge");
  check(c.bindings.in[pc::kBlock][0] == pc::kInMouseRight, "a bad line keeps the old binding");
  check(std::fabs(c.mouse.sensitivity - 2.5f) < 1e-6f && c.mouse.invertY, "[mouse] keys");
  check(err.find("Bogus") != std::string::npos && err.find("Nope") != std::string::npos,
        "errors name the bad lines");
  check(err.find("LT") == std::string::npos, "[pad] lines are not ours (no error)");
  check(pc::defaultIni().find("glyphs = auto") != std::string::npos, "defaultIni carries glyphs = auto");
  check(pc::firstInput(d, pc::kTargetLock) == pc::kInMouseMiddle, "firstInput");
  check(pc::primaryInputFor(0x2000, true, d) == pc::kInMouseRight &&
            pc::primaryInputFor(0x2000, false, d) == SDL_SCANCODE_ESCAPE,
        "prompt source: B = Right mouse in gameplay, Escape in menus");
  check(pc::primaryInputFor(pc::kPadLT, true, d) == pc::kInMouseMiddle, "prompt source: LT = Mouse3");
  check(pc::primaryInputFor(0x4000, false, d) == SDL_SCANCODE_E &&
            pc::primaryInputFor(0x8000, false, d) == SDL_SCANCODE_F &&
            pc::primaryInputFor(0x4000, true, d) == pc::kInMouseLeft &&
            pc::primaryInputFor(0x8000, true, d) == SDL_SCANCODE_SPACE,
        "prompt source: menu X = E, Y = F (gameplay X = Mouse1, Y = Space)");
}

void testLegacy() {
  Keys k;
  k.on(SDL_SCANCODE_RETURN).on(SDL_SCANCODE_Z).on(SDL_SCANCODE_X).on(SDL_SCANCODE_W).on(SDL_SCANCODE_A);
  pc::Pad p;
  const uint16_t b = pc::mapLegacyTestKeys(k.k, &p);
  check(b == (0x0010 | 0x1000 | 0x2000), "legacy: Enter=START Z=A X=B");
  check(p.ly == 32767 && p.lx == -32767, "legacy: W/A = left stick");
}

void testGameplayMap() {
  pc::Config c;
  pc::Mapper m;
  pc::PollIn in;
  in.ctx = pc::Context::Gameplay;
  Keys k;
  k.on(SDL_SCANCODE_E).on(SDL_SCANCODE_SPACE).on(SDL_SCANCODE_LSHIFT).on(SDL_SCANCODE_F)
      .on(SDL_SCANCODE_LCTRL).on(SDL_SCANCODE_M).on(SDL_SCANCODE_TAB);
  in.keys = k.k;
  in.mouseButtons = 1 | 2;   // left + right
  pc::Pad p = run(m, in, c);
  check((p.buttons & 0x4000) != 0, "gameplay: left mouse = X (ACTION)");
  check((p.buttons & 0x2000) != 0, "gameplay: right mouse = B (BLOCK)");
  check((p.buttons & 0x1000) != 0, "gameplay: E = A (PICKUP)");
  check((p.buttons & 0x8000) != 0, "gameplay: Space = Y (TAKEOFF)");
  check((p.buttons & 0x0200) != 0, "gameplay: Left Shift = RB (SUPERSPEED)");
  check((p.buttons & 0x0100) != 0, "gameplay: Left Ctrl = LB (RESET CAMERA / HOVER)");
  check((p.buttons & 0x0020) != 0, "gameplay: M = BACK (MAP)");
  check(p.lt == 255 && p.rt == 255, "gameplay: Tab = LT, F = RT");

  Keys esc;
  esc.on(SDL_SCANCODE_ESCAPE);
  in.keys = esc.k;
  in.mouseButtons = 0;
  p = run(m, in, c);
  check(p.buttons == 0x0010, "gameplay: Esc = START (PAUSE) only");

  // menus: Esc = B, Enter = A, the mouse buttons are not gameplay actions (a fresh Esc press:
  // the one held from gameplay is latched, testContextLatch)
  in.ctx = pc::Context::Menu;
  Keys none0;
  in.keys = none0.k;
  run(m, in, c);
  in.keys = esc.k;
  p = run(m, in, c);
  check(p.buttons == 0x2000, "menu: Esc = B");
  Keys ent;
  ent.on(SDL_SCANCODE_RETURN).on(SDL_SCANCODE_DOWN);
  in.keys = ent.k;
  in.mouseButtons = 1;
  p = run(m, in, c);
  check(p.buttons == (0x1000 | 0x0002), "menu: Enter = A, Down = D-pad down, left mouse ignored");
  in.ctx = pc::Context::Unknown;
  p = run(m, in, c);
  check((p.buttons & 0x4000) == 0, "unknown context behaves as menu (no gameplay mouse)");

  // iter-465 menu keys: the menus' X / Y actions (COSTUMES WEAR, the map's FORTRESS)
  in.ctx = pc::Context::Menu;
  in.mouseButtons = 0;
  Keys none1;
  in.keys = none1.k;
  run(m, in, c);
  Keys ef;
  ef.on(SDL_SCANCODE_E).on(SDL_SCANCODE_F);
  in.keys = ef.k;
  p = run(m, in, c);
  check(p.buttons == (0x4000 | 0x8000) && p.rt == 0, "menu: E = X, F = Y (not A / RT)");
  in.ctx = pc::Context::Cinematic;
  in.keys = none1.k;
  run(m, in, c);
  in.keys = ef.k;
  p = run(m, in, c);
  check(p.buttons == 0, "cinematic: E / F press nothing");
  in.ctx = pc::Context::Gameplay;
  in.keys = none1.k;
  run(m, in, c);
  in.keys = ef.k;
  p = run(m, in, c);
  check((p.buttons & 0x1000) != 0 && (p.buttons & 0xC000) == 0 && p.rt == 255,
        "gameplay: E = A (PICKUP), F = RT (SUPERPOWER), never X / Y");
}

void testPadPassThrough() {
  // a controller alone: with no key, button or mouse input the report is untouched, everywhere
  pc::Config c;
  bool ok = true;
  for (pc::Context ctx : {pc::Context::Unknown, pc::Context::Menu, pc::Context::Gameplay,
                          pc::Context::Cinematic, pc::Context::MiniGame})
    for (int cfg = 0; cfg < 4; ++cfg)
      for (int air = 0; air < 2; ++air) {
        pc::Mapper m;
        pc::PollIn in;
        in.ctx = ctx;
        in.config = cfg;
        in.air = air != 0;
        in.mouseLookActive = true;
        in.lookUnitsPerCountX = in.lookUnitsPerCountY = 0.02f;
        Keys none;
        in.keys = none.k;
        for (int t = 0; t < 3; ++t) {
          pc::Pad p;
          p.buttons = 0xF3FF; p.lt = 17; p.rt = 200;
          p.lx = -12345; p.ly = 20000; p.rx = 9000; p.ry = -31000;
          const pc::Pad before = p;
          m.poll(in, c, &p);
          if (std::memcmp(&p, &before, sizeof p) != 0) ok = false;
        }
      }
  check(ok, "pad pass-through: no keyboard/mouse input = the controller's report, unchanged");
}

void testContextLatch() {
  pc::Config c;
  pc::Mapper m;
  pc::PollIn in;
  Keys esc;
  esc.on(SDL_SCANCODE_ESCAPE);
  in.keys = esc.k;
  in.ctx = pc::Context::Gameplay;
  pc::Pad p = run(m, in, c);           // Unknown -> Gameplay: no latch
  check(p.buttons == 0x0010, "latch: none from Unknown (boot)");
  Keys none;
  in.keys = none.k;
  run(m, in, c);
  in.keys = esc.k;
  p = run(m, in, c);
  check(p.buttons == 0x0010, "latch: Esc in gameplay = START");
  in.ctx = pc::Context::Menu;          // the pause menu opened while Esc is still held
  p = run(m, in, c);
  check(p.buttons == 0, "latch: the same held Esc does NOT become B in the menu");
  in.keys = none.k;
  run(m, in, c);
  in.keys = esc.k;
  p = run(m, in, c);
  check(p.buttons == 0x2000, "latch: a NEW Esc press in the menu = B");
  // a left click held from a menu into gameplay is not an attack until re-pressed
  in.keys = nullptr;
  in.mouseButtons = 1;
  in.ctx = pc::Context::Gameplay;
  p = run(m, in, c);
  check((p.buttons & 0x4000) == 0, "latch: a click carried into gameplay is not ACTION");
  in.mouseButtons = 0;
  run(m, in, c);
  in.mouseButtons = 1;
  p = run(m, in, c);
  check((p.buttons & 0x4000) != 0, "latch: a new click in gameplay = ACTION");
  // W held through a cinematic keeps moving afterwards (movement is never latched)
  Keys w;
  w.on(SDL_SCANCODE_W);
  in.keys = w.k;
  in.mouseButtons = 0;
  run(m, in, c);
  in.ctx = pc::Context::Cinematic;
  run(m, in, c);
  in.ctx = pc::Context::Gameplay;
  p = run(m, in, c);
  check(p.ly == 32767, "latch: W held through a cinematic still moves");
}

void testPowerWheel() {
  pc::Config c;
  pc::Mapper m;
  pc::PollIn in;
  in.ctx = pc::Context::Gameplay;
  in.wheel = -1;   // wheel down = next
  pc::PollOut o;
  pc::Pad p = run(m, in, c, &o);
  check(o.powerStep == 1 && (p.buttons & 0x0008), "wheel down: next power (heat -> freeze = right)");
  in.wheel = 0;
  p = run(m, in, c);
  check((p.buttons & 0x0008) != 0, "the step holds its D-pad for a second poll");
  p = run(m, in, c);
  check((p.buttons & 0x000F) == 0, "then releases");
  Keys k3;
  k3.on(SDL_SCANCODE_3);
  in.keys = k3.k;
  p = run(m, in, c);
  check((p.buttons & 0x0004) != 0, "3 = D-pad left (super breath)");
  in.keys = nullptr;
  for (int i = 0; i < 4; ++i) run(m, in, c);
  in.wheel = 1;   // wheel up = previous: super breath -> freeze (right)
  p = run(m, in, c, &o);
  check(o.powerStep == -1 && (p.buttons & 0x0008), "wheel up after 3: previous = freeze (right)");
  in.wheel = -2;  // two steps on from freeze: super breath, then heat (the ring has 3, no D-pad down)
  run(m, in, c, &o);
  in.wheel = 0;
  bool sawDown = false;
  for (int i = 0; i < 12; ++i) { p = run(m, in, c); sawDown = sawDown || (p.buttons & 0x0002); }
  check(!sawDown, "the wheel ring never selects D-pad down (no gameplay command reads it)");
  pc::Bindings bc = pc::defaultBindings();
  check(pc::findConflicts(bc).empty(), "the default layout has no conflicts");
  bc.in[pc::kAttack][1] = SDL_SCANCODE_SPACE;   // Space is TAKE OFF too
  const auto cf = pc::findConflicts(bc);
  check(cf.size() == 1 && cf[0].input == SDL_SCANCODE_SPACE, "a key on two gameplay actions is a conflict");
  uint16_t ins[pc::kInputsPerAction];
  check(pc::parseBindingText("Mouse1, Tab, Q, Left Alt", ins) && ins[3] == SDL_SCANCODE_LALT, "four bindings parse");
  check(!pc::parseBindingText("Mouse1, Tab, Q, Left Alt, Z", ins), "a fifth is refused");
  check(pc::bindingText(pc::defaultBindings(), pc::kTargetLock) == "Mouse3, Tab", "binding text");
}

void testDodge() {
  pc::Config c;
  pc::Mapper m;
  pc::PollIn in;
  in.ctx = pc::Context::Gameplay;
  Keys k;
  k.on(SDL_SCANCODE_LALT).on(SDL_SCANCODE_D);
  in.keys = k.k;
  const int n = pc::DodgeMacro::kNeutralPolls, f = pc::DodgeMacro::kFlickPolls,
            s = pc::DodgeMacro::kSettlePolls;
  bool ok = true;
  for (int t = 0; t < n + f + s; ++t) {
    pc::PollOut o;
    const pc::Pad p = run(m, in, c, &o);
    if (!(p.buttons & 0x2000)) ok = false;                      // Block throughout
    if (t < n && (p.lx != 0 || p.ly != 0)) ok = false;          // rest
    if (t >= n && t < n + f && (p.lx != 32767 || p.ly != 0)) ok = false;   // flick right
    if (t >= n + f && (p.lx != 0 || p.ly != 0)) ok = false;     // rest again
  }
  check(ok, "Dodge + D: B held, stick rest -> full right -> rest");
  pc::Pad p = run(m, in, c);
  check((p.buttons & 0x2000) == 0 && p.lx == 32767, "after the macro: D moves again, no Block");
  // a held Dodge key does not repeat
  p = run(m, in, c);
  check((p.buttons & 0x2000) == 0, "holding Dodge does not re-trigger");
  // no movement key: backwards
  pc::Mapper m2;
  Keys k2;
  k2.on(SDL_SCANCODE_LALT);
  in.keys = k2.k;
  for (int t = 0; t < n; ++t) run(m2, in, c);
  p = run(m2, in, c);
  check(p.ly == -32767 && p.lx == 0, "Dodge alone flicks backwards");
}

void testLook() {
  pc::Config c;
  pc::Mapper m;
  pc::PollIn in;
  in.ctx = pc::Context::Gameplay;
  in.mouseLookActive = true;
  in.lookUnitsPerCountX = in.lookUnitsPerCountY = 0.02f;
  in.mouseDx = 10;
  pc::Pad p = run(m, in, c);
  check(p.rx > 0 && p.ry == 0, "mouse right = right stick +X");
  in.mouseDx = 0;
  in.mouseDy = 10;
  pc::Mapper m2;   // fresh: no rounding remainder carried from the X move above
  p = run(m2, in, c);
  check(p.ry < 0 && p.rx == 0, "mouse down = right stick -Y (as a pad pulled back; the game's Y INVERSION applies after)");
  c.mouse.invertY = true;
  pc::Mapper m2b;
  p = run(m2b, in, c);
  check(p.ry > 0, "host invert_y flips it");
  c.mouse.invertY = false;
  // a flick bigger than one tick of full deflection is delivered over later ticks (carry)
  pc::Mapper m3;
  const float upc = 0.02f;
  in.mouseDy = 0;
  in.mouseDx = static_cast<int32_t>(2.5f / upc);
  p = run(m3, in, c);
  check(p.rx == 32767, "a big flick saturates this tick");
  in.mouseDx = 0;
  p = run(m3, in, c);
  check(p.rx == 32767, "and continues next tick (carry)");
  p = run(m3, in, c);
  check(p.rx > 0 && p.rx < 32767, "and finishes with the remainder");
  // the quantiser's rounding remainder may take one or two more tiny ticks, then it stops
  for (int t = 0; t < 2 && p.rx != 0; ++t) {
    p = run(m3, in, c);
    if (p.rx != 0 && p.rx > 9000) break;
  }
  p = run(m3, in, c);
  check(p.rx == 0, "then stops (after at most two rounding-remainder ticks)");
  in.mouseLookActive = false;
  in.mouseDx = 50;
  p = run(m, in, c);
  check(p.rx == 0, "no mouse-look while inactive");
}
void testPipeline() {
  // the quantiser and its inverse agree at every index
  bool ok = true;
  for (int i = 0; i <= 999; ++i)
    if (pc::axisIndexOf(pc::rawForIndex(i)) != i || pc::axisIndexOf(pc::rawForIndex(i) - 1) != i - 1) ok = false;
  check(ok, "rawForIndex is the smallest raw giving each index");
  check(pc::axisIndexOf(0) == 0 && pc::axisIndexOf(32767) == 1000 && pc::axisIndexOf(-32768) == -1000,
        "quantiser end points");
  ok = true;
  for (int c : {0, 1, 2, 3})
    for (int k = 0; k <= 100; ++k) {
      const float o = k / 100.0f;
      if (std::fabs(pc::curveApply(uint8_t(c), pc::curveInverse(uint8_t(c), o)) - o) > 1e-4f) ok = false;
    }
  check(ok, "curve inverses invert the game's curves");
  // stickFor: what the game reads back from the stick equals what stickFor says it delivers
  ok = true;
  for (int c : {1, 3})
    for (int k = 1; k <= 200; ++k) {
      pc::LookLaw law;
      law.curve = uint8_t(c);
      float got = 0;
      const float want = k / 200.0f;
      const int16_t raw = law.stickFor(want, &got);
      const int i = pc::axisIndexOf(raw);
      const float game = (raw >= 8689 && i > 250) ? pc::curveApply(uint8_t(c), (i - 250) / 750.0f) : 0.0f;
      if (std::fabs(game - got) > 1e-5f) ok = false;
      if (std::fabs(got - want) > 0.01f && want > 0.01f) ok = false;
    }
  check(ok, "stickFor: the game's read-back = delivered, within 0.01 of wanted");
  ok = true;
  for (int k = 1; k <= 200; ++k) {
    pc::LookLaw law;
    law.curve = 3;
    law.flightShape = true;
    float got = 0;
    const float want = k / 200.0f;
    const int16_t raw = law.stickFor(want, &got);
    const int i = pc::axisIndexOf(raw);
    const float y = (raw >= 8689 && i > 250) ? pc::curveApply(3, (i - 250) / 750.0f) : 0.0f;
    const float game = 0.5f * y + 0.5f * y * y;   // sub_821EB308
    if (std::fabs(game - got) > 1e-5f) ok = false;
    if (std::fabs(got - want) > 0.01f && want > 0.01f) ok = false;
  }
  check(ok, "flight shape: stickFor undoes 0.5|y| + 0.5y^2 exactly");
  // config C swaps the sticks: WASD on the right stick, the mouse on the left
  pc::Config c;
  pc::Mapper m;
  pc::PollIn in;
  in.ctx = pc::Context::Gameplay;
  in.config = 2;
  in.lookUnitsPerCountX = in.lookUnitsPerCountY = 0.02f;
  Keys k;
  k.on(SDL_SCANCODE_W);
  in.keys = k.k;
  in.mouseLookActive = true;
  in.mouseDx = 20;
  pc::Pad p = run(m, in, c);
  check(p.ry == 32767 && p.ly == 0 && p.lx > 0 && p.rx == 0, "config C: W = RY, mouse X = LX");
  in.air = true;
  in.config = 1;   // B air: thrust LY, strafe RX, yaw LX, pitch RY
  Keys kd;
  kd.on(SDL_SCANCODE_D);
  in.keys = kd.k;
  p = run(m, in, c);
  check(p.rx == 32767 && p.lx > 0, "config B air: D = RX (strafe), mouse X = LX (yaw)");
}
void testDodgeFrame() {
  namespace mv = sr::kernel::pcmoves;
  // camera looks along +z, right = +x; Superman faces +x (90 deg to the camera's right)
  pc::DodgeFrame f;
  f.valid = true;
  f.camFwd[0] = 0; f.camFwd[1] = 1;
  f.camRight[0] = 1; f.camRight[1] = 0;
  f.face[0] = 1; f.face[1] = 0;
  f.faceRight[0] = 0; f.faceRight[1] = -1;
  float x = 0, y = 0;
  pc::DodgeKind k = pc::chooseDodge(0, 0, f, &x, &y);
  check(k == pc::DodgeKind::Back && std::fabs(x + 1) < 1e-4f && std::fabs(y) < 1e-4f,
        "no keys: back from FACING (stick toward camera-left when he faces camera-right)");
  k = pc::chooseDodge(0, 1, f, &x, &y);
  check(k == pc::DodgeKind::Back, "W alone: back (the game has no forward evade)");
  k = pc::chooseDodge(-1, 0, f, &x, &y);
  check(k == pc::DodgeKind::Back, "A = camera-left = behind him when he faces camera-right: back");
  k = pc::chooseDodge(0, -1, f, &x, &y);
  check(k == pc::DodgeKind::Right && std::fabs(y + 1) < 1e-4f,
        "S = camera-back = his right: right evade, stick toward camera-back");
  // facing the camera's direction: the keys keep their camera meaning
  f.face[0] = 0; f.face[1] = 1; f.faceRight[0] = 1; f.faceRight[1] = 0;
  k = pc::chooseDodge(1, 1, f, &x, &y);
  check(k == pc::DodgeKind::Right && std::fabs(x - 1) < 1e-4f && std::fabs(y) < 1e-4f,
        "W+D facing away from the camera: right (D wins), stick exactly right");
  k = pc::chooseDodge(-1, 0, f, &x, &y);
  check(k == pc::DodgeKind::Left && std::fabs(x + 1) < 1e-4f, "A: left");
  // the evade centre lands inside the game's windows: delta = angle(facing -> stick dir)
  bool ok = true;
  for (int deg = 0; deg < 360; deg += 15) {
    const float a = deg * 0.0174533f;
    pc::DodgeFrame g = f;
    g.face[0] = std::sin(a); g.face[1] = std::cos(a);
    g.faceRight[0] = std::cos(a); g.faceRight[1] = -std::sin(a);
    for (int kx = -1; kx <= 1; ++kx)
      for (int ky = -1; ky <= 1; ++ky) {
        const pc::DodgeKind kk = pc::chooseDodge(kx, ky, g, &x, &y);
        // stick (camera frame = world here) vs facing: right component / forward component
        const float fw = x * g.face[0] + y * g.face[1];
        const float rt = x * g.faceRight[0] + y * g.faceRight[1];
        const float d = std::atan2(rt, fw) * 57.29578f;
        if (kk == pc::DodgeKind::Back && std::fabs(std::fabs(d) - 180.0f) > 1.0f) ok = false;
        if (kk == pc::DodgeKind::Right && std::fabs(d - 90.0f) > 1.0f) ok = false;
        if (kk == pc::DodgeKind::Left && std::fabs(d + 90.0f) > 1.0f) ok = false;
      }
  }
  check(ok, "every facing x every key combination: the stick hits the evade's centre (90/180 deg)");

  // air move geometry
  const float qid[4] = {0, 0, 0, 1};
  const float v[3] = {1, 2, 3};
  float o[3];
  mv::quatRotate(qid, v, o);
  check(o[0] == 1 && o[1] == 2 && o[2] == 3, "quatRotate: identity");
  const float q90[4] = {0, 0.70710678f, 0, 0.70710678f};   // +90 deg about +y
  const float z[3] = {0, 0, 1};
  mv::quatRotate(q90, z, o);
  check(std::fabs(o[0] - 1) < 1e-4f && std::fabs(o[2]) < 1e-4f, "quatRotate: +90 about y takes +z to +x");
  mv::Nudge n{};
  check(mv::planHoverDodge(qid, 0, 0, &n) && std::fabs(n.dir[2] + 1) < 1e-4f && n.dist == 6.0f,
        "hover dodge, no keys: 6 m straight back");
  check(mv::planHoverDodge(qid, 1, 0, &n) && std::fabs(std::fabs(n.dir[0]) - 1) < 1e-4f && n.dir[1] == 0,
        "hover dodge + D: level sideways");
  const float vel[3] = {0, 0, 40};   // 40 m/s forward
  check(mv::planStrafeDash(qid, vel, 1, &n), "strafe dash plans with D");
  // constant speed dist/dur along dir: forward component must stay 40 m/s
  const float spd = n.dist / n.dur;
  check(std::fabs(spd * n.dir[2] - 40.0f) < 0.01f && std::fabs(spd * std::fabs(n.dir[0]) - 40.0f) < 0.01f,
        "strafe dash keeps the forward 40 m/s and adds 40 m/s sideways");
  check(!mv::planStrafeDash(qid, vel, 0, &n), "strafe dash needs A or D");
}

// Lane E E3: the host screens' keys and caps (pc_controls.hpp hostKeyAct / hostKeyFor / hostKeyCap).
void testHostKeys() {
  using HA = pc::HostAct;
  const pc::Bindings d = pc::defaultBindings();
  const pc::Keymap P = pc::Keymap::Player, L = pc::Keymap::LegacyTest;
  auto act = [&](SDL_Scancode s, bool shift, pc::Keymap km, const pc::Bindings& b) {
    return pc::hostKeyAct(uint16_t(s), shift, km, b);
  };
  // H1: the harness's test keymap is the iter-450S table, unchanged (tools/sr_drive.ps1 names rely on it).
  check(act(SDL_SCANCODE_Z, false, L, d) == HA::Accept && act(SDL_SCANCODE_X, false, L, d) == HA::Back &&
            act(SDL_SCANCODE_C, false, L, d) == HA::X && act(SDL_SCANCODE_V, false, L, d) == HA::Y &&
            act(SDL_SCANCODE_Q, false, L, d) == HA::LB && act(SDL_SCANCODE_E, false, L, d) == HA::RB &&
            act(SDL_SCANCODE_SPACE, false, L, d) == HA::X && act(SDL_SCANCODE_R, false, L, d) == HA::Y &&
            act(SDL_SCANCODE_TAB, false, L, d) == HA::RB && act(SDL_SCANCODE_TAB, true, L, d) == HA::LB &&
            act(SDL_SCANCODE_PAGEUP, false, L, d) == HA::LT && act(SDL_SCANCODE_PAGEDOWN, false, L, d) == HA::RT &&
            act(SDL_SCANCODE_RETURN, false, L, d) == HA::Accept && act(SDL_SCANCODE_BACKSPACE, false, L, d) == HA::Back,
        "E3 H1 host keys, test keymap: the iter-450S table (Z/X/C/V, Q/E, Space/R, Tab, PgUp/PgDn)");
  // H2: players follow the menu bindings (MenuX = E, MenuY = F), tabs on Tab / Shift+Tab, no other aliases.
  check(act(SDL_SCANCODE_RETURN, false, P, d) == HA::Accept && act(SDL_SCANCODE_KP_ENTER, false, P, d) == HA::Accept &&
            act(SDL_SCANCODE_ESCAPE, false, P, d) == HA::Back && act(SDL_SCANCODE_BACKSPACE, false, P, d) == HA::Back &&
            act(SDL_SCANCODE_E, false, P, d) == HA::X && act(SDL_SCANCODE_F, false, P, d) == HA::Y &&
            act(SDL_SCANCODE_TAB, false, P, d) == HA::RB && act(SDL_SCANCODE_TAB, true, P, d) == HA::LB &&
            act(SDL_SCANCODE_PAGEUP, false, P, d) == HA::LT && act(SDL_SCANCODE_PAGEDOWN, false, P, d) == HA::RT &&
            act(SDL_SCANCODE_UP, false, P, d) == HA::Up && act(SDL_SCANCODE_RIGHT, false, P, d) == HA::Right,
        "E3 H2 host keys, player defaults: Enter/Esc, the menu bindings (E = X, F = Y), Tab / Shift+Tab, PgUp / PgDn");
  check(act(SDL_SCANCODE_Z, false, P, d) == HA::None && act(SDL_SCANCODE_X, false, P, d) == HA::None &&
            act(SDL_SCANCODE_C, false, P, d) == HA::None && act(SDL_SCANCODE_V, false, P, d) == HA::None &&
            act(SDL_SCANCODE_Q, false, P, d) == HA::None && act(SDL_SCANCODE_SPACE, false, P, d) == HA::None &&
            act(SDL_SCANCODE_R, false, P, d) == HA::None,
        "E3 H2b player keymap: Z / X / C / V, Q, Space and R do nothing (no key acts that its cap does not show)");
  // H3: a rebind works and Esc stays the failsafe.
  pc::Bindings b = d;
  for (int k = 0; k < pc::kInputsPerAction; ++k) b.in[pc::kMenuBack][k] = pc::kInNone;
  b.in[pc::kMenuBack][0] = SDL_SCANCODE_B;
  check(act(SDL_SCANCODE_B, false, P, b) == HA::Back && act(SDL_SCANCODE_ESCAPE, false, P, b) == HA::Back &&
            act(SDL_SCANCODE_BACKSPACE, false, P, b) == HA::None && pc::hostKeyCap(HA::Back, P, b) == "B",
        "E3 H3 MenuBack rebound to B: B backs, Esc still backs (failsafe), Backspace no longer does; the cap says B");
  // H4: the failsafes win over a binding; a cap never names a key that does something else.
  pc::Bindings c = d;
  for (int k = 0; k < pc::kInputsPerAction; ++k) c.in[pc::kMenuX][k] = pc::kInNone;
  c.in[pc::kMenuX][0] = SDL_SCANCODE_RETURN;
  check(act(SDL_SCANCODE_RETURN, false, P, c) == HA::Accept && pc::hostKeyCap(HA::X, P, c).empty() &&
            act(SDL_SCANCODE_E, false, P, c) == HA::None,
        "E3 H4 MenuX bound only to Enter: Enter stays Accept, X has no key and no cap; E is free");
  // H5: a binding wins over a host-only key, and the host-only act then shows no cap.
  pc::Bindings t = d;
  t.in[pc::kMenuY][1] = SDL_SCANCODE_TAB;
  check(act(SDL_SCANCODE_TAB, false, P, t) == HA::Y && pc::hostKeyCap(HA::RB, P, t).empty() &&
            pc::hostKeyCap(HA::Y, P, t) == "F",
        "E3 H5 MenuY also on Tab: Tab = Y (the binding first), RB has no key left, the Y cap stays F (first binding)");
  // H6 / H7: the caps.
  check(pc::hostKeyCap(HA::Accept, P, d) == "Enter" && pc::hostKeyCap(HA::Back, P, d) == "Esc" &&
            pc::hostKeyCap(HA::X, P, d) == "E" && pc::hostKeyCap(HA::Y, P, d) == "F" &&
            pc::hostKeyCap(HA::LB, P, d) == "Shift+Tab" && pc::hostKeyCap(HA::RB, P, d) == "Tab" &&
            pc::hostKeyCap(HA::LT, P, d) == "PgUp" && pc::hostKeyCap(HA::RT, P, d) == "PgDn",
        "E3 H6 player caps: Enter / Esc / E / F / Shift+Tab / Tab / PgUp / PgDn");
  check(pc::hostKeyCap(HA::Accept, L, d) == "Enter" && pc::hostKeyCap(HA::Back, L, d) == "Esc" &&
            pc::hostKeyCap(HA::X, L, d) == "Space" && pc::hostKeyCap(HA::Y, L, d) == "R" &&
            pc::hostKeyCap(HA::LB, L, d) == "Q" && pc::hostKeyCap(HA::RB, L, d) == "E" &&
            pc::hostKeyCap(HA::LT, L, d) == "PgUp" && pc::hostKeyCap(HA::RT, L, d) == "PgDn",
        "E3 H7 test-keymap caps = the labels the host screens drew before E3");
  // H8: a mouse-only binding never becomes a key cap; the failsafe names the key.
  pc::Bindings m = d;
  for (int k = 0; k < pc::kInputsPerAction; ++k) m.in[pc::kMenuConfirm][k] = pc::kInNone;
  m.in[pc::kMenuConfirm][0] = pc::kInMouseLeft;
  check(pc::hostKeyCap(HA::Accept, P, m) == "Enter" &&
            pc::hostKeyAct(pc::kInMouseLeft, false, P, m) == HA::None,
        "E3 H8 MenuConfirm on Mouse1 only: the cap is Enter (the failsafe); a mouse input is never a host key");
  // H9: hostKeyFor gives the key and Shift for the scripted presses.
  bool shift = false;
  check(pc::hostKeyFor(HA::LB, P, d, &shift) == SDL_SCANCODE_TAB && shift &&
            pc::hostKeyFor(HA::RB, P, d, &shift) == SDL_SCANCODE_TAB && !shift &&
            pc::hostKeyFor(HA::X, L, d, &shift) == SDL_SCANCODE_SPACE && !shift &&
            pc::hostKeyFor(HA::None, P, d, &shift) == pc::kInNone,
        "E3 H9 hostKeyFor: LB = Shift+Tab, RB = Tab (player), X = Space (test keymap), None = none");
  check(pc::keyCapName(SDL_SCANCODE_PAGEUP) == "PgUp" && pc::keyCapName(SDL_SCANCODE_RETURN) == "Enter" &&
            pc::keyCapName(SDL_SCANCODE_LSHIFT) == "LShift" && pc::keyCapName(SDL_SCANCODE_E) == "E" &&
            pc::keyCapName(pc::kInNone).empty(),
        "E3 H10 key cap names (short forms, SDL names otherwise)");
  // H12: the pad's BACK / VIEW (PC SETTINGS: REVERT) follows MAP's key for players; the test keymap has none.
  check(act(SDL_SCANCODE_M, false, P, d) == HA::View && pc::hostKeyCap(HA::View, P, d) == "M" &&
            act(SDL_SCANCODE_M, false, L, d) == HA::None && pc::hostKeyCap(HA::View, L, d).empty(),
        "E3 H12 VIEW = MAP's key (M) for players; no key in the test keymap (the cap shows '-')");
  // H13 (Astra E3 r1): MOVE is the arrows picture only while the arrow keys move; else its cap names the keys that do.
  pc::Bindings mv = d;
  mv.in[pc::kMenuX][1] = SDL_SCANCODE_UP;   // the Up arrow now presses X
  pc::Bindings ws = d;
  ws.in[pc::kMenuUp][1] = SDL_SCANCODE_W;   // W also moves up; the arrows still move
  pc::Bindings mw = mv;
  for (int k = 0; k < pc::kInputsPerAction; ++k) mw.in[pc::kMenuUp][k] = pc::kInNone;
  mw.in[pc::kMenuUp][0] = SDL_SCANCODE_W;   // and Up is W alone
  check(pc::hostMoveCap(P, d).empty() && pc::hostMoveCap(L, d).empty() && pc::hostMoveCap(P, ws).empty() &&
            pc::hostMoveCap(P, mv) == "-/" + pc::keyCapName(SDL_SCANCODE_DOWN) &&
            pc::hostMoveCap(P, mw) == "W/" + pc::keyCapName(SDL_SCANCODE_DOWN),
        "E3 H13 MOVE cap: arrows while the arrows move; MenuX on Up -> '-/Down'; Up on W alone -> 'W/Down'");
  // H13b (Astra E3 r2): where MOVE also means LEFT / RIGHT (PC SETTINGS), all four arrows are checked and named.
  pc::Bindings lr = d;
  lr.in[pc::kMenuX][1] = SDL_SCANCODE_LEFT;   // the Left arrow now presses X
  check(pc::hostMoveCap(P, d, true).empty() && pc::hostMoveCap(P, lr, false).empty() &&
            pc::hostMoveCap(P, lr, true) == pc::keyCapName(SDL_SCANCODE_UP) + "/" + pc::keyCapName(SDL_SCANCODE_DOWN) +
                                               "/-/" + pc::keyCapName(SDL_SCANCODE_RIGHT),
        "E3 H13b MOVE cap, 4-way: defaults = arrows; MenuX on Left -> 'Up/Down/-/Right' (2-way MOVE unaffected)");
  // H14 (Astra E3 r1): hold-to-preview is RT's key under the same rule (a binding wins); P only in the test keymap.
  pc::Bindings pg = d;
  pg.in[pc::kMenuX][1] = SDL_SCANCODE_PAGEDOWN;   // PgDn now presses X
  check(pc::hostPreviewKey(SDL_SCANCODE_PAGEDOWN, false, P, d) && !pc::hostPreviewKey(SDL_SCANCODE_P, false, P, d) &&
            pc::hostPreviewKey(SDL_SCANCODE_PAGEDOWN, false, L, d) && pc::hostPreviewKey(SDL_SCANCODE_P, false, L, d) &&
            !pc::hostPreviewKey(SDL_SCANCODE_PAGEDOWN, false, P, pg) && act(SDL_SCANCODE_PAGEDOWN, false, P, pg) == HA::X &&
            pc::hostKeyCap(HA::RT, P, pg).empty(),
        "E3 H14 preview key: PgDn (player and test), P only in the test keymap; MenuX on PgDn -> X, no preview key");
  // H15 (Astra E3 r1): the harness channel's held Shift / Ctrl are its modifiers (Shift+Tab = LB there too).
  uint8_t hk[512] = {};
  const uint16_t none = pc::harnessKeyMods(hk);
  hk[SDL_SCANCODE_RSHIFT] = 1;
  hk[SDL_SCANCODE_TAB] = 1;
  const uint16_t sh = pc::harnessKeyMods(hk);
  hk[SDL_SCANCODE_RSHIFT] = 0;
  hk[SDL_SCANCODE_LCTRL] = 1;
  const uint16_t ct = pc::harnessKeyMods(hk);
  check(none == 0 && (sh & SDL_KMOD_SHIFT) && !(sh & SDL_KMOD_CTRL) && (ct & SDL_KMOD_CTRL) && !(ct & SDL_KMOD_SHIFT) &&
            act(SDL_SCANCODE_TAB, (sh & SDL_KMOD_SHIFT) != 0, L, d) == HA::LB && pc::harnessKeyMods(nullptr) == 0,
        "E3 H15 harness modifiers: RShift -> Shift (Shift+Tab = LB), LCtrl -> Ctrl, none -> 0");
}

// Lane E E5c: the cursor's title click is the game's own START - the START control on the published frame for a
// fixed number of pad polls (pccontrols::pulseStart), never the pad report
void testStartPulse() {
  pc::queueStartPulseForTest(3);
  const uint16_t a = pc::takeStartPulse(), b = pc::takeStartPulse(), c = pc::takeStartPulse(), d = pc::takeStartPulse();
  check(a == 0x0010 && b == 0x0010 && c == 0x0010 && d == 0,
        "E5c P1 a 3-poll pulse: START (0x0010) on 3 polls, then nothing");
  pc::queueStartPulseForTest(2);
  const uint16_t e = pc::takeStartPulse();
  pc::queueStartPulseForTest(2);   // a second click restarts the pulse, never adds to it
  const uint16_t f = pc::takeStartPulse(), g = pc::takeStartPulse(), h = pc::takeStartPulse();
  check(e == 0x0010 && f == 0x0010 && g == 0x0010 && h == 0, "E5c P2 a new pulse restarts the count (2 polls), never adds");
  pc::Pad p;
  p.buttons = 0x0010;
  const auto fr = pc::controlFrameOf(p);
  check(fr.buttons == (1u << 4), "E5c P3 the pulse's bit is the game's START control (control 4, kCtlStart)");
  check(!pc::pulseStart(0) && !pc::pulseStart(-1) && pc::takeStartPulse() == 0 && pc::kStartPulsePolls == 6,
        "E5c P4 no pulse for 0 / negative polls; the title click holds START for 6 polls");
  // E5c r2 (review P2): the pulse is bound to its click - a deadline that has passed drops what is left (a stalled
  // poll never carries START into the next screen), and a cancel (the title is gone) drops it at once
  pc::queueStartPulseForTest(6, -1);
  const uint16_t late = pc::takeStartPulse(), late2 = pc::takeStartPulse();
  pc::queueStartPulseForTest(6);
  const uint16_t first = pc::takeStartPulse();
  pc::cancelStartPulse();
  const uint16_t afterCancel = pc::takeStartPulse();
  check(late == 0 && late2 == 0 && first == 0x0010 && afterCancel == 0 && pc::kStartPulseMaxMs == 250,
        "E5c P5 a pulse past its deadline (250 ms) gives nothing; a cancel drops the rest of a running pulse");
  pc::queueStartPulseForTest(0);
}

}  // namespace

int runPcControlsTest() {
  std::printf("[pc-controls-test]\n");
  testNames();
  testDefaultsAndIni();
  testLegacy();
  testGameplayMap();
  testContextLatch();
  testPadPassThrough();
  testPowerWheel();
  testDodge();
  testLook();
  testPipeline();
  testDodgeFrame();
  testHostKeys();
  testStartPulse();
  // Exercise the real registration; pure binding tests cannot detect missing wiring.
  namespace HI = sr::kernel::hostinput;
  _putenv_s("SR_INSTANCE_ID", "pc-prompt-test");
  _putenv_s("SR_PC_KEYMAP", "player");
  HI::setKeyPromptSource(nullptr);
  check(HI::promptInputFor(HI::kA) == "Space", "unwired source reproduces legacy menu prompt");
  pc::init();
  check(HI::promptInputFor(HI::kA) == "Return" && HI::promptInputFor(HI::kB) == "Escape",
        "controls init registers the actual PC menu bindings with glyphs");
  check(HI::promptInputFor(HI::kLStickUp) == "W" && HI::promptInputFor(HI::kLStickRight) == "D",
        "registered movement prompts use the controls bindings");
  check(HI::promptInputFor(HI::kLThumb).empty() && HI::promptInputFor(HI::kRT).empty(),
        "registered source leaves unbound menu controls unnamed");
  // Lane E E3: the host screens' keymap follows SR_PC_KEYMAP like the pad merge (read per call).
  check(pc::hostKeymap(false) == pc::Keymap::Player && pc::hostKeymap(true) == pc::Keymap::Player,
        "E3 H11 SR_PC_KEYMAP=player: both keyboard sources use the player keys on the host screens");
  _putenv_s("SR_PC_KEYMAP", "legacy");
  check(pc::hostKeymap(false) == pc::Keymap::LegacyTest && pc::hostKeymap(true) == pc::Keymap::LegacyTest,
        "E3 H11b SR_PC_KEYMAP=legacy: both use the test keys");
  _putenv_s("SR_PC_KEYMAP", "player");
  std::printf("[pc-controls-test] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
