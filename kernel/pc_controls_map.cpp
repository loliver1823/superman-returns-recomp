// iter-464 controls lane: the per-poll mapper (pure; difftest --pc-controls-test drives it).
// Keyboard + mouse state in, one Xbox 360 pad report out. docs/PC_CONTROLS.md sections 3-5.
#include "pc_controls_map.hpp"

#include <algorithm>
#include <cmath>

namespace sr::kernel::pccontrols {

namespace {
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020;
constexpr uint16_t kLB = 0x0100, kRB = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr float kPi = 3.14159265358979323846f;

int16_t clampStick(float v) {
  if (v > 32767.0f) return 32767;
  if (v < -32767.0f) return -32767;
  return static_cast<int16_t>(std::lround(v));
}

bool sharesInput(const Bindings& b, Action x, Action y) {
  for (int k = 0; k < kInputsPerAction; ++k)
    for (int j = 0; j < kInputsPerAction; ++j)
      if (b.in[x][k] != kInNone && b.in[x][k] == b.in[y][j]) return true;
  return false;
}
}  // namespace

int16_t* padAxis(Pad* p, Axis a) {
  switch (a) {
    case kAxLX: return &p->lx;
    case kAxLY: return &p->ly;
    case kAxRX: return &p->rx;
    default: return &p->ry;
  }
}

// [V] docs/PC_CONTROLS.md 2: InputCtx_BindConfigA-D. Ground: A/B move on the left stick, C/D
// on the right. Air: A = LY thrust, LX strafe, RX yaw, RY pitch; B = LY, RX, LX, RY; C = RY,
// RX, LX, LY; D = RY, LX, RX, LY. Config A uses the sine curve (3) for the look axes, B-D the
// square (1).
StickRoles stickRoles(int config, bool air) {
  switch (config) {
    case 1:
      return air ? StickRoles{kAxRX, kAxLY, kAxLX, kAxRY, 1} : StickRoles{kAxLX, kAxLY, kAxRX, kAxRY, 1};
    case 2:
      return air ? StickRoles{kAxRX, kAxRY, kAxLX, kAxLY, 1} : StickRoles{kAxRX, kAxRY, kAxLX, kAxLY, 1};
    case 3:
      return air ? StickRoles{kAxLX, kAxRY, kAxRX, kAxLY, 1} : StickRoles{kAxRX, kAxRY, kAxLX, kAxLY, 1};
    default:
      return StickRoles{kAxLX, kAxLY, kAxRX, kAxRY, 3};
  }
}

// ── the game's axis pipeline (section 4) ────────────────────────────────────────────────
float curveApply(uint8_t curve, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  switch (curve) {
    case 0: return t;                                   // sub_82725E58
    case 1: return t * t;                               // sub_82725EB0
    case 2: return std::sqrt(t);                        // sub_82725F18
    default: return 0.5f * (1.0f - std::cos(kPi * t));  // sub_82726048: 0.5 (1 + sin(pi (t - .5)))
  }
}

float curveInverse(uint8_t curve, float o) {
  o = std::clamp(o, 0.0f, 1.0f);
  switch (curve) {
    case 0: return o;
    case 1: return std::sqrt(o);
    case 2: return o * o;
    default: return std::acos(1.0f - 2.0f * o) / kPi;
  }
}

int axisIndexOf(int raw) {
  // InputXenonPad_Update_sub_825FFD40: i = trunc(2000 (s + 32768) / 65535) - 1000
  return static_cast<int>((2000LL * (raw + 32768)) / 65535) - 1000;
}

int rawForIndex(int i) {
  // the smallest raw with axisIndexOf(raw) == i (i >= 0)
  const long long num = static_cast<long long>(i + 1000) * 65535;
  int raw = static_cast<int>((num + 1999) / 2000) - 32768;
  return std::min(raw, 32767);
}

int16_t LookLaw::stickFor(float want, float* delivered) const {
  const float a = std::fabs(want);
  const int deadzone = rightStick ? 8689 : 7849;
  auto shape = [&](float y) { return flightShape ? 0.5f * y + 0.5f * y * y : y; };
  // the smallest stick that reads as non-zero: past the per-stick square deadzone AND past
  // the command's 250 cut
  int minIdx = 251;
  while (rawForIndex(minIdx) < deadzone) ++minIdx;
  const float minOut = shape(curveApply(curve, float(minIdx - 250) / 750.0f));
  if (a < 0.5f * minOut) { *delivered = 0.0f; return 0; }
  const float o = std::min(a, 1.0f);
  // undo the flight shape: o = 0.5 y + 0.5 y^2  ->  y = (sqrt(1 + 8 o) - 1) / 2
  const float y = flightShape ? 0.5f * (std::sqrt(1.0f + 8.0f * o) - 1.0f) : o;
  int idx = static_cast<int>(std::lround(250.0f + 750.0f * curveInverse(curve, y)));
  idx = std::clamp(idx, minIdx, 1000);
  const int raw = idx >= 1000 ? 32767 : rawForIndex(idx);
  const float got = shape(curveApply(curve, float(idx - 250) / 750.0f));
  *delivered = std::copysign(got, want);
  return static_cast<int16_t>(want < 0 ? -raw : raw);
}

// ── the mapper ───────────────────────────────────────────────────────────────────────────
bool Mapper::rawDown(const PollIn& in, uint16_t code) {
  if (code == kInNone) return false;
  if (code < kInMouseLeft) return in.keys && code < 512 && in.keys[code];
  if (code <= kInMouseX2) return (in.mouseButtons & (1u << (code - kInMouseLeft))) != 0;
  return false;
}

bool Mapper::held(const PollIn& in, const Bindings& b, Action a) const {
  for (int k = 0; k < kInputsPerAction; ++k) {
    const uint16_t code = b.in[a][k];
    if (code == kInNone || code > kInMouseX2 || suppress_[code]) continue;
    if (rawDown(in, code)) return true;
  }
  return false;
}

int Mapper::wheelPresses(const PollIn& in, const Bindings& b, Action a) {
  int n = 0;
  for (int k = 0; k < kInputsPerAction; ++k) {
    if (b.in[a][k] == kInWheelUp && in.wheel > 0) n += in.wheel;
    if (b.in[a][k] == kInWheelDown && in.wheel < 0) n += -in.wheel;
  }
  return n;
}

// The power cycle (wheel): heat vision (up) -> freeze breath (right) -> super breath (left), the
// three powers the game selects (sub_8229B630 binds D-pad up / right / left; down drives no
// gameplay command). The host keeps the index (the power keys set it), so the wheel always
// selects the neighbour of the last power the player chose.
uint16_t Mapper::powerBit(int idx) {
  static const uint16_t kRing[3] = {kDpadUp, kDpadRight, kDpadLeft};
  return kRing[((idx % 3) + 3) % 3];
}

void Mapper::poll(const PollIn& in, const Config& cfg, Pad* pad, PollOut* out) {
  const Bindings& b = cfg.bindings;
  // the context latch (see suppress_): release clears, a context change suppresses what is down
  for (uint16_t c = 1; c <= kInMouseX2; ++c)
    if (suppress_[c] && !rawDown(in, c)) suppress_[c] = false;
  if (in.ctx != prevCtx_) {
    // Not from Unknown (boot / reader not ready), and never the inputs that mean the same in
    // every context (movement = left stick, arrows = D-pad): holding W through a cinematic
    // keeps Superman running when gameplay resumes.
    if (prevCtx_ != Context::Unknown) {
      bool stable[kInWheelDown + 1] = {};
      const Action kStable[] = {kMoveForward, kMoveBack, kMoveLeft, kMoveRight,
                                kMenuUp, kMenuDown, kMenuLeft, kMenuRight};
      for (Action a : kStable)
        for (int k = 0; k < kInputsPerAction; ++k)
          if (b.in[a][k] <= kInWheelDown) stable[b.in[a][k]] = true;
      for (uint16_t c = 1; c <= kInMouseX2; ++c)
        if (!stable[c] && rawDown(in, c)) suppress_[c] = true;
    }
    prevCtx_ = in.ctx;
  }
  const bool game = in.ctx == Context::Gameplay || in.ctx == Context::MiniGame;
  const StickRoles roles = stickRoles(in.config, in.air);
  PollOut o;
  o.ctx = in.ctx;
  uint16_t btn = 0;

  // ── movement keys -> the move/strafe axes (full deflection, like the iter-450S merge) ──
  const int mx = (held(in, b, kMoveRight) ? 1 : 0) - (held(in, b, kMoveLeft) ? 1 : 0);
  const int my = (held(in, b, kMoveForward) ? 1 : 0) - (held(in, b, kMoveBack) ? 1 : 0);
  if (mx) *padAxis(pad, game ? roles.strafe : kAxLX) = static_cast<int16_t>(mx * 32767);
  if (my) *padAxis(pad, game ? roles.move : kAxLY) = static_cast<int16_t>(my * 32767);

  // ── arrows = D-pad everywhere (menus; in gameplay they also select powers) ─────────────
  if (held(in, b, kMenuUp)) btn |= kDpadUp;
  if (held(in, b, kMenuDown)) btn |= kDpadDown;
  if (held(in, b, kMenuLeft)) btn |= kDpadLeft;
  if (held(in, b, kMenuRight)) btn |= kDpadRight;

  if (!game) {
    // Menus and cinematics: Enter = A (the title takes START or CROSS), Backspace = B.
    // The default Esc is both Pause and MenuBack: in a MENU it is B (back / resume - the
    // pause menu's B closes it), in a CINEMATIC it is START (the game's pause / skip key).
    // E / F = X / Y: the menus' own X and Y actions (COSTUMES WEAR, the map's FORTRESS).
    if (held(in, b, kMenuConfirm)) btn |= kA;
    if (in.ctx != Context::Cinematic) {   // no X / Y action in a cinematic (START / A skip)
      if (held(in, b, kMenuX)) btn |= kX;
      if (held(in, b, kMenuY)) btn |= kY;
    }
    const bool escShared = sharesInput(b, kPause, kMenuBack);
    if (in.ctx == Context::Cinematic) {
      if (held(in, b, kPause)) btn |= kStart;
      if (held(in, b, kMenuBack) && !escShared) btn |= kB;
    } else {
      if (held(in, b, kMenuBack)) btn |= kB;
      if (!escShared && held(in, b, kPause)) btn |= kStart;
    }
    if (held(in, b, kMap)) btn |= kBack;
    lookCarryX_ = lookCarryY_ = 0.0f;
    dodge_.cancel();
    powerQueue_ = 0;
    pad->buttons |= btn;
    lastOut_ = o;
    if (out) *out = o;
    return;
  }

  // ── gameplay: the actions ───────────────────────────────────────────────────────────
  if (held(in, b, kAttack)) btn |= kX;
  if (held(in, b, kBlock)) btn |= kB;
  if (held(in, b, kPickup)) btn |= kA;
  if (held(in, b, kTakeoff)) btn |= kY;
  if (held(in, b, kSuperspeed)) btn |= kRB;
  if (held(in, b, kCameraHover)) btn |= kLB;
  if (held(in, b, kCycleTarget)) btn |= 0x0080;   // R3: GAME_LLOCK_CYCLE
  if (held(in, b, kMap)) btn |= kBack;
  if (held(in, b, kPause)) btn |= kStart;
  if (held(in, b, kTargetLock)) pad->lt = 255;
  if (held(in, b, kSuperpower)) pad->rt = 255;

  // powers: 1-4 hold their D-pad direction; the wheel queues single presses
  const Action pw[3] = {kPowerUp, kPowerRight, kPowerLeft};
  for (int i = 0; i < 3; ++i)
    if (held(in, b, pw[i])) { btn |= powerBit(i); powerIdx_ = i; }
  if (held(in, b, kPowerDown)) btn |= kDpadDown;   // bindable; no gameplay command reads it
  powerQueue_ += wheelPresses(in, b, kPowerNext) - wheelPresses(in, b, kPowerPrev);
  powerQueue_ = std::clamp(powerQueue_, -3, 3);
  // A queued step = PRESS for kPowerPressPolls, then RELEASE for kPowerGapPolls (the game
  // takes the D-pad on its press edge).
  if (powerPhase_ > 0) {
    btn |= powerBit(powerIdx_);
    if (--powerPhase_ == 0) powerGap_ = kPowerGapPolls;
  } else if (powerGap_ > 0) {
    --powerGap_;
  } else if (powerQueue_ != 0) {
    const int step = powerQueue_ > 0 ? 1 : -1;
    powerQueue_ -= step;
    powerIdx_ = (powerIdx_ + step + 3) % 3;
    powerPhase_ = kPowerPressPolls - 1;
    btn |= powerBit(powerIdx_);
    o.powerStep = static_cast<int8_t>(step);
  }

  // keyboard camera (unbound by default): full deflection on the look axes
  {
    const int kx = (held(in, b, kLookRight) ? 1 : 0) - (held(in, b, kLookLeft) ? 1 : 0);
    const int ky = (held(in, b, kLookUp) ? 1 : 0) - (held(in, b, kLookDown) ? 1 : 0);
    if (kx) *padAxis(pad, roles.yaw) = static_cast<int16_t>(kx * 32767);
    if (ky) *padAxis(pad, roles.pitch) = static_cast<int16_t>(ky * 32767);
  }

  // ── mouse-look -> the look axes (section 4) ────────────────────────────────────────────
  if (cfg.mouse.look && in.mouseLookActive && in.ctx == Context::Gameplay &&
      (in.lookUnitsPerCountX > 0.0f || in.lookUnitsPerCountY > 0.0f)) {
    LookLaw law;
    law.curve = roles.lookCurve;
    law.flightShape = in.air;
    law.rightStick = roles.yaw >= kAxRX;
    const float sx = cfg.mouse.invertX ? -1.0f : 1.0f;
    const float sy = cfg.mouse.invertY ? -1.0f : 1.0f;
    // counts -> wanted rotation this tick (full-deflection ticks), plus what earlier ticks
    // could not deliver (the stick saturates at 1.0)
    const float wantX =
        lookCarryX_ + sx * float(in.mouseDx) * in.lookUnitsPerCountX * cfg.mouse.sensitivity;
    // mouse +dy = moved toward the player = stick -Y, as a pad pulled back
    const float wantY =
        lookCarryY_ - sy * float(in.mouseDy) * in.lookUnitsPerCountY * cfg.mouse.sensitivity;
    float gotX = 0, gotY = 0;
    const int16_t vx = law.stickFor(wantX, &gotX);
    law.rightStick = roles.pitch >= kAxRX;
    const int16_t vy = law.stickFor(wantY, &gotY);
    lookCarryX_ = std::clamp(wantX - gotX, -law.maxCarry, law.maxCarry);
    lookCarryY_ = std::clamp(wantY - gotY, -law.maxCarry, law.maxCarry);
    int16_t* ax = padAxis(pad, roles.yaw);
    int16_t* ay = padAxis(pad, roles.pitch);
    if (vx) *ax = clampStick(float(*ax) + vx);
    if (vy) *ay = clampStick(float(*ay) + vy);
    if (vx || vy) o.mouseLookPolls = 1;
  } else {
    lookCarryX_ = lookCarryY_ = 0.0f;
  }

  // ── Dodge (section 5) ──────────────────────────────────────────────────────────────
  const bool dodgeHeld = held(in, b, kDodge);
  const bool dodgeEdge = dodgeHeld && !dodgeWasHeld_;
  dodgeWasHeld_ = dodgeHeld;
  o.dodgeRequested = dodgeEdge && in.ctx == Context::Gameplay;
  o.dodgeKeysX = static_cast<int8_t>(mx);
  o.dodgeKeysY = static_cast<int8_t>(my);
  if (o.dodgeRequested && in.air && in.airMoves) {
    o.dodgeAir = true;   // the air moves take it (pc_moves.cpp); no pad input
  } else if (o.dodgeRequested && !dodge_.active()) {
    // On the ground the game's evade; in the air (air moves off) the same input gives the
    // game's own response, the air block.
    float sx = 0, sy = 0;
    const DodgeKind k = chooseDodge(mx, my, in.dodgeFrame, &sx, &sy);
    dodge_.start(sx, sy);
    o.dodgeKind = static_cast<uint8_t>(k) + 1;
  }
  if (dodge_.active()) {
    const DodgeMacro::Frame f = dodge_.step();
    btn |= kB;   // Block held for the whole sequence
    *padAxis(pad, roles.strafe) = clampStick(f.x * 32767.0f);
    *padAxis(pad, roles.move) = clampStick(f.y * 32767.0f);
    o.dodgePhase = f.phase;
  }

  pad->buttons |= btn;
  lastOut_ = o;
  if (out) *out = o;
}

// ── the action layer's frame (iter-466 controls2) ────────────────────────────────────────
pcactions::ControlFrame controlFrameOf(const Pad& p) {
  pcactions::ControlFrame f;
  // XINPUT_GAMEPAD_* bit -> the game's control index (bits 0-9 keep their position; A/B/X/Y are
  // bits 12-15 in XInput and controls 10-13 in the game)
  for (int b = 0; b < 10; ++b)
    if (p.buttons & (1u << b)) f.buttons |= static_cast<uint16_t>(1u << b);
  for (int b = 0; b < 4; ++b)
    if (p.buttons & (0x1000u << b)) f.buttons |= static_cast<uint16_t>(1u << (pcactions::kCtlA + b));
  if (p.lt) f.buttons |= 1u << pcactions::kCtlLT;
  if (p.rt) f.buttons |= 1u << pcactions::kCtlRT;
  const int16_t raw[4] = {p.lx, p.ly, p.rx, p.ry};
  for (int stick = 0; stick < 2; ++stick) {
    const int dz = stick == 0 ? 7849 : 8689;
    const int x = raw[2 * stick], y = raw[2 * stick + 1];
    const bool inside = x > -dz && x < dz && y > -dz && y < dz;
    for (int k = 0; k < 2; ++k) {
      const int a = 2 * stick + k;
      if (!raw[a]) continue;
      f.axisDriven |= static_cast<uint8_t>(1u << a);
      f.axis[a] = inside ? 0 : static_cast<int16_t>(axisIndexOf(raw[a]));
    }
  }
  return f;
}

// ── the Dodge direction (section 5) ──────────────────────────────────────────────────────
DodgeKind chooseDodge(int kx, int ky, const DodgeFrame& f, float* sx, float* sy) {
  if (!f.valid) {
    // no frame: the camera-relative key direction (A/D first), back = stick down
    if (kx) { *sx = float(kx); *sy = 0.0f; return kx > 0 ? DodgeKind::Right : DodgeKind::Left; }
    *sx = 0.0f; *sy = -1.0f;
    return DodgeKind::Back;
  }
  // the intent in world (ground plane)
  float ix, iz;
  if (kx) { ix = kx * f.camRight[0]; iz = kx * f.camRight[1]; }
  else if (ky < 0) { ix = -f.camFwd[0]; iz = -f.camFwd[1]; }
  else { ix = -f.face[0]; iz = -f.face[1]; }
  // its angle from the facing, + = toward Superman's right
  const float fwd = ix * f.face[0] + iz * f.face[1];
  const float rgt = ix * f.faceRight[0] + iz * f.faceRight[1];
  const float a = std::atan2(rgt, fwd) * 57.29578f;   // (-180, 180]
  DodgeKind k;
  if (std::fabs(a) >= 135.0f || std::fabs(a) < 20.0f) k = DodgeKind::Back;   // forward -> back
  else k = a > 0 ? DodgeKind::Right : DodgeKind::Left;
  // the evade's centre direction in world, then camera-relative stick
  float wx, wz;
  if (k == DodgeKind::Back) { wx = -f.face[0]; wz = -f.face[1]; }
  else if (k == DodgeKind::Right) { wx = f.faceRight[0]; wz = f.faceRight[1]; }
  else { wx = -f.faceRight[0]; wz = -f.faceRight[1]; }
  float x = wx * f.camRight[0] + wz * f.camRight[1];
  float y = wx * f.camFwd[0] + wz * f.camFwd[1];
  const float n = std::sqrt(x * x + y * y);
  if (!(n > 1e-3f)) { x = 0.0f; y = -1.0f; }
  else { x /= n; y /= n; }
  *sx = x;
  *sy = y;
  return k;
}

// ── the Dodge macro ─────────────────────────────────────────────────────────────────────
void DodgeMacro::start(float sx, float sy) {
  const float m = std::sqrt(sx * sx + sy * sy);
  dirX_ = m > 0 ? sx / m : 0.0f;
  dirY_ = m > 0 ? sy / m : -1.0f;
  tick_ = 0;
  active_ = true;
}

DodgeMacro::Frame DodgeMacro::step() {
  Frame f;
  // phase 1: Block, stick centred (the detector needs the stick at rest first)
  // phase 2: Block, stick at full deflection in the direction (the flick)
  // phase 3: Block, stick centred again (a flick is a tap, not a hold)
  if (tick_ < kNeutralPolls) {
    f.phase = 1;
  } else if (tick_ < kNeutralPolls + kFlickPolls) {
    f.phase = 2;
    // full deflection along the exact direction (the game takes atan2 of the stick)
    f.x = dirX_;
    f.y = dirY_;
  } else {
    f.phase = 3;
  }
  if (++tick_ >= kNeutralPolls + kFlickPolls + kSettlePolls) active_ = false;
  return f;
}

}  // namespace sr::kernel::pccontrols
