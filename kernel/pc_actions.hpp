#pragma once
// iter-466 controls2 (PORT FEATURE, USER DECISION 2026-10-06, charter 2a): THE ACTION LAYER.
// docs/PC_CONTROLS.md section 2 is the design and verification record.
//
// "Any PC controller bindings should be a remapper, it should call the functions directly ...
// no virtual presses or mappings." The keyboard and the mouse no longer touch the pad report
// (XamInputGetState): a bound key IS the game's action. The game's input layer is its own
// InputMgr (*0x829A2C04): once per sim tick InputMgr_DispatchQueuedInput_sub_82721E68 evaluates
// player 1's input context, an array of InputCommand objects indexed by the game's event id
// (GAME_ACTION_ATTACK = 66, INTERFACE_A = 125 ...), each by its own class rule (EventButton edge /
// held / repeat, EventButtonRelease, EventAxisMapped dead zone + curve, EventCombo, EventAlternative,
// EventDualButton ...). Every consumer - the Lua CheckInputEvent, the state graphs' InputEvent
// conditions, locomotion, flight, the camera, power select, the pause / map tick, the menu router
// that posts the Apt keys - asks those commands (query sub_82181860 -> InputMgr_IsCommandActive
// sub_82722390 / the command's GetValue).
//
// So the layer hooks sub_82721E68: the game's own body runs first (the real controller, unchanged),
// then each command of player 1's current context is evaluated by a host mirror of ITS OWN class
// rule over the player's whole input - the controller's controls this tick (the InputState the
// game evaluated the context with) plus the controls the bound PC actions drive (the game's own
// control names: Action1-4 = A/B/X/Y, Shoulder1-4, Start, Select, D-pad, the sticks) - and the
// result goes to the fields the consumers read. With no PC input the mirror reproduces the game's
// own result exactly (it is seeded from the command's own state; difftest --pc-actions-test runs it
// against the recompiled class code). The game's own router, gates and options then act on it
// exactly as on a controller: the menu router calls AptAuxInputPostButton itself, the pause tick
// calls FlowMgr.ChangeInterfaceState itself, the inversion options flip the axis commands' own
// negate flags, DisableInputEvent suppresses, a context without the command ignores the key.
// Before the next evaluation the game's own values are put back, so the game's own state is the
// controller's alone.
//
// Gated: on for players (port features), off on natural rolls (automation::portFeaturesOn());
// SR_PC_ACTIONS=0 turns it off (developer A/B). Classified in docs/OVERRIDES.json (port-feature).
// Kernel-only header.
#include <cstdint>
#include <string>

namespace sr::kernel::pcactions {

// ── the game's controls (InputState indices, InputState_GetButton_sub_825F7B08 / sub_825F7B78) ─
// Buttons 0-15 as the game's binders use them (sub_8229B630): D-pad up/down/left/right, START,
// BACK, L3, R3, LB, RB, A, B, X, Y, LT, RT. Axes 0-3: left X, left Y, right X, right Y in the pad
// driver's units (InputXenonPad_Update_sub_825FFD40: -1000..1000).
enum Control : uint8_t {
  kCtlDUp = 0, kCtlDDown, kCtlDLeft, kCtlDRight, kCtlStart, kCtlBack, kCtlL3, kCtlR3,
  kCtlLB, kCtlRB, kCtlA, kCtlB, kCtlX, kCtlY, kCtlLT, kCtlRT, kCtlCount
};
// What the PC actions drive this tick (built by pc_controls from the bindings), or the controller's controls the
// game evaluated a context with (the latch).
struct ControlFrame {
  uint16_t buttons = 0;       // bit i = control i held
  int16_t axis[4] = {};       // -1000..1000 (pad driver units, after its dead zone)
  uint8_t axisDriven = 0;     // bit i = axis i is driven by the PC this tick
  // step4/pad-triggers: the controller's ANALOG controls. The pad driver (InputXenonPad_Update_sub_825FFD40) keeps
  // the triggers as bytes, not bits: InputState_GetButton_sub_825F7B08 answers an index in [digital count +0x14,
  // + analog count +0x18) with the byte at +0x24 (LT / RT = controls 14 / 15, 0..255). A control in analogMask
  // answers max(analog[i], 255 if its bit is held) - the PC's key on a trigger stays a full pull.
  uint16_t analogMask = 0;
  uint8_t analog[kCtlCount] = {};
  bool operator==(const ControlFrame& o) const;
};

// The action layer is on (port features on, SR_PC_ACTIONS not 0). Evaluated once.
bool enabled();
// The pad poll publishes the PC's frame for this sim tick (any thread; the dispatch hook reads it).
void publish(const ControlFrame& f);
// The game's event names (the INPUT_EVENT_* table at 0x82912B68), for logs. "" when unnamed.
const char* eventName(uint32_t id);

// ── the host mirror of the game's command classes (pure; difftest --pc-actions-test runs it
//    against the recompiled class code tick by tick) ─────────────────────────────────────────
enum class Kind : uint8_t {
  Unknown = 0, Button, Release, Mapped, Axis, AxisMapped, Alternative, Combo, Dual
};
Kind kindOfVtable(uint32_t vt);
const char* kindName(Kind k);

// One command's state, the fields the game's Update reads and writes. Leaves use the leaf part;
// composites keep their children as nested states.
struct CmdState {
  Kind kind = Kind::Unknown;
  // params (from the command: control index, repeat, threshold / dead zone, mode, negate ...)
  uint32_t ctl = 0, ctl2 = 0;       // control index (Dual: both)
  uint32_t repeat = 0;              // +8 (Dual +12, Combo +16)
  uint8_t thr = 0, thr2 = 0;        // button thresholds (+18; Dual +28 / +29)
  int16_t dz = 0;                   // axis dead zone (+18)
  uint32_t mode = 0;                // axis direction mode (0 +, 1 -, 2 either)
  uint32_t negate = 0;              // axis negate flag (the game's inversion option)
  uint32_t window = 0;              // Combo: frames both children must be within (+12)
  // state
  uint8_t prev = 0;                 // Button / Release / Mapped: last value (+17)
  uint32_t counter = 0;             // repeat counter (+12; Dual +16)
  bool fired = false;               // the IsActive result
  int16_t value = 0;                // Axis / AxisMapped: last value (+16)
  int32_t prevA = 0, prevB = 0;     // Dual: last values (+20 / +24)
  uint32_t sinceA = 0xFFFFFFFFu, sinceB = 0xFFFFFFFFu;   // Combo: frames since each child fired
  bool held = false;                // Combo: +22
  uint32_t countdown = 0;           // Combo: +32
  bool latchA = false, latchB = false;   // Alternative: +12 / +13
  CmdState* childA = nullptr;       // composites (owned by the layer's arena)
  CmdState* childB = nullptr;
};
// One tick of a command's own Update over a frame of controls.
void step(CmdState& s, const ControlFrame& f);
// The value the command reports for the consumers that read it (GetValue's input field):
// Button-like: the 0..255 value (+17); axes: the stored value (+16).
int32_t outputValue(const CmdState& s);

// ── the dispatch hook's two halves (the PPC_FUNC in pc_actions.cpp calls them around the game's
//    body; difftest --pc-actions-test drives them on a synthetic input manager) ───────────────
// Puts back the game's own command fields the last merge overwrote (same context only).
void beforeDispatch(uint8_t* base);
// The game evaluates `ctx` with the InputState `state` (InputContext_UpdateCommands hook):
// latches the controller's controls for the merge.
void noteContextUpdate(uint8_t* base, uint32_t ctx, uint32_t state);
// The flat guest model can be read fault-free (checked once).
bool readyFor(uint8_t* base);
// Evaluates player 1's live context by each command's own rule over the controller + the PC and
// writes the result to the fields the consumers read.
void afterDispatch(uint8_t* base);
// Forget every mirror and pending write (tests).
void resetForTest();

// Tests (difftest).
int selfTest();

}  // namespace sr::kernel::pcactions
