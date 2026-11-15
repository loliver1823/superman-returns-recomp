#pragma once
// iter-464 controls lane: the per-poll keyboard + mouse -> pad mapper (pure, unit-tested).
// docs/PC_CONTROLS.md sections 3-5. Kernel-only header.
#include "pc_controls.hpp"
#include "pc_actions.hpp"

namespace sr::kernel::pccontrols {

// What owns the pad, from the game's own input context (section 3): the IgcContextManager
// slot's requested context type (IgcContextFactoryCreate_sub_821632B8: 0-3 InGameConfigA-D,
// 4 Interface, 5 Mim, 6 Reveal, 7 Debug, 8 Igc).
enum class Context : uint8_t {
  Unknown = 0,    // no signal (boot, reader failed): treated as Menu
  Menu = 1,       // Interface / Reveal / Debug contexts: front end, pause, popups
  Gameplay = 2,   // InGameConfig A-D: the player controls Superman
  Cinematic = 3,  // Igc: in-game cinematics (START / A skip)
  MiniGame = 4,   // Mim: the button-mash / momentum mini-games (face buttons)
};

// Pad axes.
enum Axis : uint8_t { kAxLX = 0, kAxLY = 1, kAxRX = 2, kAxRY = 3 };

// Which stick axis carries each role in a controller configuration (the game's own binders
// InputCtx_BindConfigA-D, sub_8229E308 / E638 / E968 / EC98), ground and air.
struct StickRoles {
  Axis strafe, move, yaw, pitch;   // move = LOCO_MOVE / FLIGHT_THRUST
  uint8_t lookCurve;               // the yaw/pitch response curve id (0 lin, 1 sq, 3 sine)
};
StickRoles stickRoles(int config, bool air);

// The game's axis pipeline for one look axis (section 4), inverted exactly:
//   raw -> square pre-deadzone (left 7849, right 8689; InputXenonPad_Update_sub_825FFD40)
//       -> i = trunc(2000 (raw + 32768) / 65535) - 1000
//       -> |i| <= 250: 0; y = curve((|i| - 250) / 750)  (EventAxisMapped + 0x82726220)
//       -> flight only: o = 0.5 |y| + 0.5 y^2           (sub_821EB308, the flight yaw/pitch shape)
// The game turns by o x (its full-deflection rate) per tick. `want` is in units of one
// full-deflection tick; stickFor returns the raw stick value that delivers it (saturating at
// 1.0) and *delivered = what the game will actually turn by, in the same units.
struct LookLaw {
  uint8_t curve = 3;
  bool rightStick = true;
  bool flightShape = false;
  float maxCarry = 3.0f;         // at most 3 ticks of saturated turn queued (fast flicks)
  int16_t stickFor(float want, float* delivered) const;
};
// The game's curves and their inverses (unit-tested against each other).
float curveApply(uint8_t curve, float t);
float curveInverse(uint8_t curve, float out);
// The game's raw -> i quantiser and the smallest raw that gives index i (>= 0).
int axisIndexOf(int raw);
int rawForIndex(int i);

// The Dodge key's pad sequence (section 5): Block held throughout; the left stick rests, then
// flicks to full deflection in the direction, then rests. Counted in pad polls (= sim ticks).
class DodgeMacro {
 public:
  static constexpr int kNeutralPolls = 2;
  static constexpr int kFlickPolls = 3;
  static constexpr int kSettlePolls = 2;
  struct Frame { uint8_t phase = 0; float x = 0, y = 0; };
  void start(float sx, float sy);   // the stick direction of the flick (camera-relative)
  Frame step();
  bool active() const { return active_; }
  void cancel() { active_ = false; }
 private:
  float dirX_ = 0, dirY_ = -1;
  int tick_ = 0;
  bool active_ = false;
};

// Where the Dodge evade should go (section 5). The game's evade [V] (Superman's Lua FSM in
// objtiv.AST, Block -> OnFootEvasion): Block held + the move stick past 25% in a direction
// 60-300 deg away from Superman's FACING; the delta picks Evade_Right [60,135), Evade_Back
// [135,225), Evade_Left [225,300]; nothing within 60 deg of facing (no forward evade). The stick
// is camera-relative. DodgeFrame carries the world frame that converts between the two.
struct DodgeFrame {
  bool valid = false;
  float camFwd[2] = {0, 1};    // camera forward on the ground plane (world x, z), unit
  float camRight[2] = {1, 0};  // camera right (stick +X) on the ground plane, unit
  float face[2] = {0, 1};      // Superman's facing on the ground plane, unit
  float faceRight[2] = {1, 0}; // Superman's right on the ground plane, unit
};
enum class DodgeKind : uint8_t { Back = 0, Left = 1, Right = 2 };
// Intent: a held A/D wins (camera left/right), else S (camera back), else straight back from
// Superman's facing. The intent maps to the nearest evade the game has (forward intent = back);
// the returned stick (-1..1, camera-relative) points exactly at that evade's centre. Without a
// valid frame: the camera-relative key direction, back = stick down.
DodgeKind chooseDodge(int keysX, int keysY, const DodgeFrame& f, float* stickX, float* stickY);

struct PollIn {
  const uint8_t* keys = nullptr;   // SDL-scancode-indexed key state (player keymap), or null
  uint32_t mouseButtons = 0;       // bit0 left, bit1 right, bit2 middle, bit3 X1, bit4 X2
  int32_t mouseDx = 0, mouseDy = 0;  // relative counts since the last poll (+x right, +y down)
  int32_t wheel = 0;               // wheel steps since the last poll (+ = up)
  bool mouseLookActive = false;    // relative mouse is live (gameplay, focused, look on)
  Context ctx = Context::Unknown;
  int config = 0;                  // the controller configuration 0-3 (A-D)
  bool air = false;                // Superman is airborne (flight / hover)
  // The stick look path (flight): full-deflection ticks per mouse count, per axis, at
  // sensitivity 1 (= radPerCount / (the game's full-deflection rate x dt)); 0 = no stick look
  // (on foot the camera angles take the mouse directly, pc_controls_game.cpp).
  float lookUnitsPerCountX = 0.0f, lookUnitsPerCountY = 0.0f;
  DodgeFrame dodgeFrame;           // the ground evade's frame (pc_controls_game.cpp)
  bool airMoves = false;           // SR_PC_MOVES: Dodge in the air requests a new air move
};

struct PollOut {
  Context ctx = Context::Unknown;
  uint8_t dodgePhase = 0;     // 0 idle, 1-3 the macro's phases
  bool dodgeRequested = false;  // the Dodge key's press edge this poll
  bool dodgeAir = false;      // ... while airborne with the air moves on (section 6)
  int8_t dodgeKeysX = 0, dodgeKeysY = 0;  // the movement keys at the press (air moves)
  uint8_t dodgeKind = 0;      // DodgeKind + 1 of the ground macro started this poll (0 none)
  int8_t powerStep = 0;       // a wheel step started this poll (+1 / -1)
  uint32_t mouseLookPolls = 0;  // 1 when mouse-look produced stick this poll
};

class Mapper {
 public:
  // ORs buttons into *pad and overrides the sticks/triggers the keyboard/mouse drive
  // (an untouched keyboard leaves the controller's values alone).
  void poll(const PollIn& in, const Config& cfg, Pad* pad, PollOut* out = nullptr);
  const PollOut& last() const { return lastOut_; }

  static constexpr int kPowerPressPolls = 2;
  static constexpr int kPowerGapPolls = 2;
  // An action is held when any bound input is down and NOT suppressed (see suppress_).
  bool held(const PollIn& in, const Bindings& b, Action a) const;
  static bool rawDown(const PollIn& in, uint16_t code);
  static int wheelPresses(const PollIn& in, const Bindings& b, Action a);
  static uint16_t powerBit(int idx);

 private:
  float lookCarryX_ = 0, lookCarryY_ = 0;
  DodgeMacro dodge_;
  bool dodgeWasHeld_ = false;
  int powerIdx_ = 0, powerQueue_ = 0, powerPhase_ = 0, powerGap_ = 0;
  PollOut lastOut_;
  // CONTEXT LATCH: an input still held when the context changes (Esc = START opens the pause
  // menu, where Esc = B; a menu click carried into gameplay) is ignored until it is released,
  // so one physical press never acts in two contexts. Index = input code (scancode / mouse).
  Context prevCtx_ = Context::Unknown;
  bool suppress_[kInWheelDown + 1] = {};
};

int16_t* padAxis(Pad* p, Axis a);

// iter-466 controls2: the mapper's frame (360-control vocabulary) as the game's own controls for
// the action layer: XInput bits -> the game's button indices (sub_8229B630's), LT / RT -> 14 / 15,
// the sticks through the pad driver's law (InputXenonPad_Update_sub_825FFD40: the square dead
// zone per stick, then the -1000..1000 quantiser). An axis the frame leaves at 0 is not driven.
pcactions::ControlFrame controlFrameOf(const Pad& p);

}  // namespace sr::kernel::pccontrols
