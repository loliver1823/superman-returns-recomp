# PC controls: keyboard + mouse layout, the action layer, mouse-look, Dodge, new air moves

Lanes `controls` (iter-464, `feat/pc-controls`) and `controls2` (iter-466, `feat/controls2`: the
action layer and the remapper, USER DECISION 2026-10-06). Code:
- `kernel/pc_actions.{hpp,cpp}`: THE ACTION LAYER (section 2): a key drives the game's own input
  commands; the keyboard and the mouse never touch the pad report.
- `kernel/pc_controls.hpp`, `kernel/pc_controls_bindings.cpp`: actions, inputs, defaults, input.ini.
- `kernel/pc_controls_map.{hpp,cpp}`: the per-poll mapper (pure): actions -> the frame of game
  controls the action layer takes (`controlFrameOf`), the stick look law, the Dodge chooser and macro.
- `kernel/pc_controls.cpp`: host glue (SDL mouse events, relative mouse, the poll merge, the
  harness read-back).
- `kernel/pc_controls_game.{hpp,cpp}`: the game readers and the three hooks (on-foot camera,
  ground and flight loco updates).
- `kernel/pc_moves.{hpp,cpp}`, `kernel/pc_moves_math.cpp`: the two new air moves (`SR_PC_MOVES`).
- Hooks in `kernel/stubs.cpp` (`mergeKeyboardPad`) and `app/platform_sdl.cpp` (events, relative
  mouse).

Tests: `build/difftest.exe --pc-controls-test`, `--pc-actions-test` (the action layer against the
recompiled command classes). Live: `tools/controls/harness_play.py` (our build,
through the harness channel), `tools/controls/move_probe.py` (a Xenia timeline),
`tools/xenia_pad.py` (now with held buttons, sticks, triggers and waits).

[V] = verified (bytes / IDA / run log / Xenia), [I] = inferred.

STATUS: being built - see the PROGRESS entry for what is verified live.

## 1. The default layout (USER 2026-10-02)

| Game action (the game's own name) | 360 control (Configuration 1) | PC default |
|---|---|---|
| Move / turn (ground), accelerate / strafe (air) | left stick | W A S D |
| Camera (ground), pitch / turn (air) | right stick | **mouse** (mouse-look, section 4) |
| ACTION (punch) - Action3 | X | Left mouse |
| BLOCK / DROP - Action2 | B | Right mouse |
| PICKUP / THROW - Action1 | A | E |
| TAKEOFF / LAND - Action4 (hold = power launch / power drop) | Y | Space |
| SUPERSPEED (SPRINT on the ground) - Shoulder2 | RB | Left Shift |
| TARGET LOCK - Shoulder3 | LT | Middle mouse, Tab |
| SUPERPOWER - Shoulder4 | RT | F |
| RESET CAMERA (ground) / AIRDRIFT - HOVER (air) - Shoulder1 | LB | Left Ctrl |
| MAP - Select | BACK | M |
| PAUSE - Start | START | Esc |
| SELECT POWERS: heat vision / freeze breath / super breath | D-pad up / right / left | 1 / 2 / 3, mouse wheel cycles the three |
| (D-pad down: no gameplay command reads it - x-ray vision has no input, iter-466) | D-pad down | unbound (bindable) |
| CYCLE TARGET while locked - GAME_LLOCK_CYCLE (iter-466) | R3 | Q |
| **DODGE** (new key, section 5) | B + left-stick flick | Left Alt |
| Menus | D-pad, A, B | arrows, Enter, Esc / Backspace; the mouse cursor (feat/mouse-cursor) |
| Menus: the screen's X / Y action (COSTUMES WEAR, the map's FORTRESS OF SOLITUDE, the controller diagram) | X, Y | E, F (iter-465, section 3.2) |

Where the 360 column comes from [V]:
- `text.AST` (INTERFACETEXT) `CntrlOpt_{Ground,Air}_Config{1..4}_*` names the logical buttons
  Action1-4, Shoulder1-4, Select, Start, DPad, LStick, RStick. The four configurations differ
  ONLY in the stick roles (air Config 2 = LStick ACCELERATE / TURN, RStick PITCH / STRAFE;
  Configs 3 and 4 swap the sticks); every button keeps its role in all four.
- Teaching text names the shoulders: `TeachingText_Throw` "pull Right Trigger [Shoulder4]",
  `TeachingText_LockMeteor` "pull and hold Left Trigger [Shoulder3]", `TeachingText_SuperSpeed`
  "hold Right Bumper [Shoulder2]", `KittenHint_18` "airdrift on the left bumper" (Shoulder1).
- The Apt `ComboDisplay.ParseCombo` maps `[Action1-4]` to A / B / X / Y (docs/INPUT_GLYPHS.md 2),
  and ENGINE_MAP 7 (flight facts) has hold Y = power launch (Action4 = TAKEOFF).
- The D-pad powers: `TeachingText_Heatvision_A` [Dpad_Up], `TeachingText_FreezeBreath_Bottom`
  [Dpad_Right], `TeachingText_BreathBlow_Bottom` [Dpad_Left]; x-ray vision is the fourth power
  (`PowerUp_SuperPower_4`).

Because every PC action presses the pad control the game itself assigns, the game's own
CONTROLLER OPTIONS still apply: the configuration (stick roles), X / Y INVERSION, SMART CAMERA
(auto-centre) and VIBRATION (pads only).

## 2. The game's input layer and THE ACTION LAYER (iter-466 controls2)

USER DECISION 2026-10-06 (charter 2a): "any PC controller bindings should be a remapper, it should
call the functions directly ... no virtual presses or mappings". Since iter-466 the keyboard and the
mouse never touch the pad report: a bound key IS the game's action. Code: `kernel/pc_actions.{hpp,cpp}`
(keys), `kernel/apt_call.{hpp,cpp}` + `kernel/mouse_ui.cpp` (the mouse cursor, docs/MOUSE_CURSOR.md).

### 2.1 How the game turns input into actions [V] (IDA, 2026-10-06)

- **One poll per sim tick.** `XamInputGetState` runs once per `FeMasterTick_821494C0` (FeMasterTick ->
  sub_82181810 -> `GameInputBuffered_Update_sub_8229F020` -> `GameInput_Update_sub_8229AD10`). The
  sim is a fixed 1/30 s (docs/HIGH_FRAMERATE.md); every timing below is in these ticks.
- **The input manager** `*0x829A2C04` (`sub_82721C88`): `+4` slot count, `+12` MapInfo[] (32 B:
  `+12` state, 0 = live; `+16` enabled; `+20` the slot's context). Player 1's slot =
  `*(*0x8296E8C8 + 12)`. `InputMgr_CapturePadStates_sub_82721CE8` turns each pad into an InputState
  (pad driver `InputXenonPad_Update_sub_825FFD40`: square dead zone per stick, left 7849 / right
  8689, then `i = trunc(2000 (raw + 32768) / 65535) - 1000`); `InputState_GetButton_sub_825F7B08` =
  bit i of the bitset at `+0x20` (i < `+0x14`) -> 0 / 255; `sub_825F7B78` = the int at `+0x28[i]`.
  The game's controls: buttons 0-15 = D-pad up / down / left / right, START, BACK, L3, R3, LB, RB, A,
  B, X, Y, LT, RT; axes 0-3 = left X, left Y, right X, right Y.
- **Contexts** (one per player slot; `IgcContextFactoryCreate_sub_821632B8` types 0-3 InGameConfigA-D,
  4 Interface, 5 Mim, 6 Reveal, 7 Debug, 8 Igc) are arrays of **InputCommand** objects indexed by the
  game's event id (the `INPUT_EVENT_*` table at `0x82912B68`; `sub_82722DA8` = `ctx+8[id]`). The
  binders build them with the game's constructors; an object can sit under two ids (flight yaw +
  camera yaw). The command classes (vtable, Update = slot 6, IsActive = slot 2, GetValue = slot 5):

| Class (vtable) | Fields | Rule |
|---|---|---|
| EventButton (0x8208CF34, `sub_82723A60`) | +4 control, +8 repeat, +12 counter, +16 fired, +17 value, +18 threshold | fires on the press, then every `repeat` ticks while held (0 = press only, 1 = every tick) |
| EventButtonRelease (0x8208D44C, `sub_82725C90`) | as EventButton | fires on the release (then every `repeat` ticks while released) |
| EventButtonMapped (0x8208D29C, `sub_827250B8`) | as EventButton + +19 max, +24 curve | as EventButton; the value is mapped (D-pad power select) |
| EventAxis (0x8208CE5C, `sub_82723518`) | +4 axis, +8 repeat, +16 value, +18 dead zone, +20 mode, +24 negate, +28 fired | fires on crossing the dead zone (mode 0 +, 1 -, 2 either), repeats |
| EventAxisMapped (0x8208D374, `sub_82725740`) | +16 value, +18 dz, +20 max, +24 mode, +28 negate, +32 fired, +36 curve | as EventAxis; GetValue = curve((abs(v) - dz) / (max - dz)) (`sub_82726220`) |
| EventAlternative (0x8208D1C4, `sub_82724A18`) | +4 / +8 children, +12 / +13 their IsActive | either child |
| EventCombo (0x8208D0EC, `sub_82724660`) | +4 / +8 children, +12 window, +16 repeat, +21 fired, +22 held, +24 / +28 ticks since each child fired, +32 countdown | both children within the window, then every `repeat` ticks |
| EventDualButton (0x8208D00C, `sub_82723D70`) | +4 / +8 controls, +12 repeat, +16 counter, +20 / +24 values, +28 / +29 thresholds, +30 fired | both held |

  The inversion options (CONTROLLER OPTIONS X / Y INVERSION) flip an axis command's negate flag
  (`sub_82725378`).
- **Each tick** `InputMgr_DispatchQueuedInput_sub_82721E68` evaluates every live context with its
  InputState (`InputContext_UpdateCommands_sub_82722DB8`: Update on each non-null entry; Reset when no
  pad is present). Then `InputMgr_DispatchActiveCommands_sub_82721F48` pushes each active command to
  `GameInput_OnActionEvent_sub_8229AB30` (the event bus: the press-start listener, the camera
  manager, the front end).
- **Every consumer asks the commands.** The query `sub_82181860(igcMgr, slot, eventId, consume,
  &value)` (named DispatchEventUnlessSuppressed in IDA; it is a query): skips ids in the
  `DisableInputEvent` list `*0x8296D888`, then answers from the 0.2 s buffered latch (65 GRAB, 66
  ATTACK, 68 JUMP, 77, 79; 58 SUPERSPEED) or `EventInputActionQuery_sub_8229AF18` ->
  `InputMgr_IsCommandActive_sub_82722390` + GetValue. Callers: Lua `CheckInputEvent` /
  `GetInputEventValue` and the state graphs' `InputEvent` conditions (attack, block, grab, jump /
  takeoff, superspeed ... are state-machine transitions on these events - there is no per-action
  function to call), locomotion, flight `sub_8220C5B8`, aim, power select `sub_82257A88`, the pause /
  map tick `PauseFlowStateTick_sub_82151298` (event 81 -> `FlowMgr.ChangeInterfaceState("PAUSEMENU")`
  after `FrontendUpdateGate_sub_8214D090`; 82 -> `PauseFlow_CanOpenPauseMap_sub_8214D188`, the
  objective check, `"PAUSEMAP"`), and the menu router `MenuButtonRepeatTick_sub_8214FC78` /
  `ProcessAnalogMenuNav_sub_8214E9D8`, which turns INTERFACE_* events into Apt key posts
  `AptAuxInputPostButton_sub_826D2C10(exec, slot, code, 1)` through the table `0x82911470` (113 ->
  301 START, 115 -> 300 SELECT, 125 -> 302 CROSS, 127 -> 304 TRIANGLE, 129 -> 305 SQUARE, 131 -> 303
  CIRCLE, 117 / 119 / 121 / 123 -> UP / DOWN / LEFT / RIGHT, 83 / 84 the analog nav) with its own
  repeat timing, `FmuiIsInputBlocked_sub_8214D228` and (states 2 / 4) `CheatMgr_FeedPadButton`.

What each control drives (the in-game shared binder `sub_8229B630` + config A `sub_8229E308`):
A -> 7, 65 GRAB, 69 THROW (alt), 75 / 77 / 78 / 80 combos, 187 FMV_SKIP (alt); B -> 70 BLOCK (held),
71 DROP, 76, 79 / 80; X -> 66 ATTACK, 67 ATTACK_HELD, 74, 77 / 78 / 80; Y -> 31 TOGGLE_MOVEMENT_MODE,
32 / 33, 68 JUMP, 79; LB -> 39 FLIGHT_HOVER, 42 MANEUVER, 43; RB -> 58 SUPERSPEED, 64 DASH, 72; LT ->
44-46 LLOCK; RT -> 59 POWER_ACTIVATE, 60, 69; START -> 81 PAUSE, 187; BACK -> 82 MAP_VIEW; R3 -> 47
LLOCK_CYCLE (every 10 ticks), 50 LLOCK_HALT_MOVEMENT; L3 + R3 -> 168 (dual); D-pad up / left / right
-> 61 / 62 / 63 power select (D-pad DOWN drives nothing: x-ray vision has a power definition in the
data but no input); left stick -> 27 / 28, 51 / 52 TARGETER, 53 / 54 LOCO; right stick -> 29 / 30,
48 / 49 LLOCK_CYCLEX/Y, 55 / 56 camera, 35-38 / 40 flight. Interface (4): 113-148. Igc (8, cinematics,
`sub_8229B578`): 186 = A, 168 = L3 + R3 - START is not bound there. Mim (5, `sub_8229D048`) and Reveal
(6, `sub_8229D5F0`, 149-169): covered by the same mechanism; the roll log's `[PC_ACTIONS] context`
lines list what each control drives in every context the run reached.

### 2.2 The action layer

`kernel/pc_actions.cpp` overrides `InputMgr_DispatchQueuedInput_sub_82721E68` (OVERRIDES.json
port-feature): the game's body runs first, exactly once and unchanged (the controller). Then every
command of player 1's live context is evaluated by a host mirror of ITS OWN class rule (the table
above, transcribed) over the player's whole input - the controller's controls this tick (the
InputState latched by an observe hook of `sub_82722DB8`) plus the controls the player's bound
actions drive - and the result goes to the fields the consumers read (IsActive's field, the value
field). Before the game's next evaluation every written byte is put back, so the game's own state is
the controller's alone. The mirror is seeded from the command's own state when first seen, so with
no PC input it reproduces the game's result exactly; every occurrence steps (the game updates an
object bound under two ids twice). Consequences:
- a key IS the action: no pad state, no XInput, no pad-button mapping; the binding names the game's
  control role (Action1-4, Shoulder1-4, Start, Select, D-pad, the sticks - the game's own CONTROLLER
  OPTIONS vocabulary), and every event the game binds to that control in the current context
  answers by its own rule (ATTACK, ATTACK_HELD, ATTACK_RELEASED, the combos ...);
- every condition stays the game's: a context without the command ignores the key; `DisableInputEvent`
  suppresses it; the menu router posts the Apt key itself with its own repeat timing and input
  blocks; the pause tick calls `ChangeInterfaceState` itself; the inversion options apply;
- a chord can be split across the controller and the keyboard (one input: buttons OR'd, an axis
  takes the larger magnitude, an analog trigger the larger pull).
- **the controller's triggers are analog** (step4/pad-triggers, 2026-10-20): the pad driver
  `InputXenonPad_Update_sub_825FFD40` gives each InputState 14 digital controls as bits (+0x20, count
  +0x14) and LT / RT as bytes (+0x24, count +0x18); `InputState_GetButton_sub_825F7B08` answers controls
  14 / 15 with that 0..255 byte. The latch reads both, and the mirror answers a trigger with the byte (a
  PC key on a trigger = 255). Before the fix the latch read the bits only, so with the layer on a
  controller's LT / RT were 0 to the mirror and the merge wrote "not fired" over the game's own LLOCK
  (44-46), POWER_ACTIVATE / RELEASE (59 / 60), THROW (69 / 75) and the LT / RT duals (107-109): lock-on
  and every superpower were dead on a pad with port features on (live trace, section 9).
- **no controller = the game's reset** (Astra P2, 2026-10-20): while the InputState says no controller
  (+8 != 1) `sub_82722DB8` calls each command's Reset (vtable slot 7) instead of Update; the mirrors
  take the same reset instead of a step and their result is written as on any tick (transcribed per
  class: EventButton / ButtonMapped fired, value, counter; ButtonRelease fired only, so it keeps its
  value - the merged input's; the axes fired, value, counter; DualButton fired, both values, counter;
  Combo fired, held, both ages -1, countdown = repeat; Alternative both latches; a composite does not
  reset its children). A control still held when the controller comes back is a new press for both.
  Before this the layer wrote nothing and the mirrors kept their history (a held trigger's previous
  255), swallowed the edge the game fired on the reconnect and wrote "not fired" over it. A latch the
  pad driver never builds (analog bytes null with a non-zero count, or `n + na` wrapping in 32 bits)
  is rejected: the game's result stands and the mirrors reseed from it (Astra P3). Neither path runs
  while a controller is connected with the real layout, so the live heat-vision / lock-on proof (arm
  (c), section 9) stands for both.
Off on natural rolls (`automation::portFeaturesOn()`); `SR_PC_ACTIONS=0` turns it off (A/B).
`SR_PC_ACTIONS_TRACE=<file>`: per tick, the events the game's own evaluation fired (`game`), the
merged result (`out`), the PC frame (`pc`), wall-clock `ms`. The mapper (`pc_controls_map.cpp`, the
context table of section 3, the Dodge macro, the look law) builds the frame;
`controlFrameOf` converts it to the game's controls (sticks through the pad driver's law).

### 2.3 Proof

| What | How | Result |
|---|---|---|
| The mirror = the game's own class code | `difftest --pc-actions-test`: a synthetic input manager holding the commands built by the game's own constructors with the binders' arguments (every class, combos, an alternative, a dual, an object under two ids; since 2026-10-20 also the trigger commands exactly as `sub_8229B630` builds them: 44-46, 59 / 60, 69, 75, 107-109); 3000 ticks of every control (holds of 1-12 ticks, axis values on and around the dead zone): the recompiled Update with the controller vs (a) the controller with the layer on and (b) the same sequence as keys through the layer, on a 16-bit InputState AND on the real pad layout (14 bits + the 2 analog trigger bytes, trigger values 0 1 2 20 41 42 43 128 255 around both binder thresholds) | equal every tick for every event; the merge is put back before each evaluation |
| The pad-layout pass can fail | the layer before step4/pad-triggers (bits-only latch) | 10 failures, e.g. `t=5 ev=59 pad (1, 255) got (0, 0)` (2026-10-20, e3d32231 on the old layer) |
| Controller gone and back; controller + keys at once; a malformed layout | the same fixture with the game's Reset (slot 7) when the InputState has no controller, a presence sequence (no controller on the context's first 3 ticks, then gaps of 1-25 ticks every 40-200, with buttons and triggers held across gaps): controller with the layer on, keys through the layer; a second (PC) sequence with the controller's, the layer on both vs the game's own run on the merged input (buttons OR'd, the larger trigger pull, the PC's axis at magnitude >= the pad's), with and without gaps; analog bytes null with `na` = 2, and `n + na` wrapping | equal every tick for every event; the malformed latch writes nothing and the merge resumes after a reseed: 444 / 0 (VM, 2026-10-20). The layer before the P2 / P3 fix: 79 failures, 75 in the disconnect passes and the 4 malformed-layout checks; its connected passes pass |
| The test can fail | mutants: combo window `<=` -> `<`, axis negate dropped | 23 failures |
| Key = pad in the game | `tools/controls/play_roll.ps1` + `scripts/controls2_parity.play` / `controls2_parity2.play` (keys, then the same action on a ViGEm pad, `SR_HARNESS_VPAD=1`), `tools/controls/parity_trace.py <out>`: the events the game's commands fired in each window | EQUAL for attack, pickup, superspeed, a power SELECTION (freeze: D-pad right, events 6 / 63), move, takeoff, land (rolls parity1 + parity3, 2026-10-06; section 9). parity1's `superpower DIFFER key [59, 60, 69] pad []`: its trace's own `game` field has no 59 in the pad window either (SDL2's RAWINPUT read the virtual pad's triggers as 0, the 2f finding), and the trigger latch defect (2.2) would have dropped it in `out` too. parity4 (2026-10-20, the parity1 roll unchanged - `controls2_parity.play`, `SR_HARNESS_VPAD=1`, a ViGEm Xbox 360 pad, nothing reading real input - on the dev f25d0bd3 package 758027e1eeec, the pad-trigger fix merged): **superpower EQUAL key [59, 60, 69] pad [59, 60, 69]**, the game's own evaluation = `out` in the pad window ([59, 60, 69, 75] both); move, attack, pickup, superspeed, power-freeze, takeoff, land EQUAL (section 9) |
| The designed differences | pause / map: the pad's START / BACK, still held when the context changes, also fires INTERFACE_START (113) / INTERFACE_BACK (115) in the new menu context; a key does not carry over (the keyboard's menu context gives Esc its menu meaning) | neither event acts on the pause menu or the map in either roll |

## 3. Context: menus vs gameplay

The PC layer needs to know who owns the pad: Esc is PAUSE in gameplay but BACK in a menu, the mouse
buttons are actions in gameplay but the cursor's clicks in a menu, and the mouse is relative
(mouse-look) only in gameplay. The signal is the game's OWN input context [V]:
`*0x8296D884` = IgcContextManager, +28 slot count, +32 slots (20 B: +0 context, +4 current type,
+8 requested type, +12 last in-game config, +16 / +17 invert flags); player 1's slot =
`*0x8296E8C8 + 12`. Golden dumps: press-start screen = type 4 (sound_menu and nosound), gameplay
= type 0 (sound_gameplay).

| Requested type | Context | PC behaviour |
|---|---|---|
| 0-3 | Gameplay (config A-D) | actions, mouse-look (relative mouse), Dodge |
| 5 | MiniGame | actions (face buttons), no mouse-look |
| 8 | Cinematic | Enter = A (skip), Esc = START |
| 4, 6, 7 | Menu | Enter = A, Esc / Backspace = B, arrows = D-pad, the cursor |
| reader failed | Unknown | as Menu |

The title screen takes START or CROSS (A) (`ScrPressStart.HandleInput`), so Enter = A works there.

### 3.1 Menus opened over gameplay keep this right (iter-465, owner request 2026-10-05) [V]

OWNER 2026-10-05: "when navigating menus, shouldn't select be enter and back be escape". The
suspicion was that menus opened OVER gameplay keep the in-game type (0-3), so Enter would be dead
and Esc would be START there. A player-keymap roll REFUTES it: the game switches player 1's input
context to Interface (4) itself whenever one of its menus takes the pad, so the table above
already gives Enter = A and Esc = B there. The `[PC_CONTROLS] context` line carries the igc type
since then (`context menu (igc type 4)`).

Repro: worktree build, `archive/v1_menukeys/roll.ps1 -Tag <t> -Script <play>` (run_recomp_x +
`tools/controls/harness_play.py`, player keymap on the harness channel, `SR_PORT_FEATURES=1`,
`SR_DEVTOOLS=devmenu`, present dumps); `python archive/v1_menukeys/table.py <dir>` reads the
read-back pad bits while each key was held and the game's next reaction from the log.

Rolls: `archive/v1_menukeys/before1` (dev mapping, `SR_PC_MENU_OWNER=0`; its legacy-key
fallbacks fired where its owner-based markers did not exist, so only its direct hits count) and
`archive/v1_menukeys/after1` (this branch: + E / F). "Pad" = the read-back pad bits while the key
was held; the reaction = the game's next log line; frames in each roll's `frames/`.

| Menu | igc type -> context | Enter | Esc | E / F |
|---|---|---|---|---|
| Title (PRESS START) | 4 -> menu | pad A -> the boot flow (`claim LoadCreate`) | - | - |
| MAIN MENU (custom front end) | 4 -> menu | pad A -> SETTINGS (`menu button=2`), NEW GAME (`answer ... button=0`) | pad B -> `menu key=304` (TRIANGLE); no cancel by design (canCancel false) | - |
| SETTINGS panel (front end) | 4 -> menu | - | pad B -> back to MAIN MENU (`menu key=304`) | - |
| Gameplay | 0 -> gameplay | nothing (not bound in gameplay) | pad START -> the pause menu (`pause install`) | PICKUP / SUPERPOWER |
| Pause menu | 4 -> menu | pad A -> MORE OPTIONS (ScrGenPop), CONTROLLER OPTIONS (`pause teardown`), AUDIO OPTIONS (before1), QUIT GAME's warning (before1, ScrGenPop) | pad B -> closes (`context gameplay (igc type 0)`), 5 / 5 | - |
| MORE OPTIONS panel | 4 -> menu | pad A -> DISPLAY OPTIONS (`menu show display`), DEV TOOLS (`menu show main`), MODS (before1: the mod library overlay, `open mods IG`) | pad B -> the pause menu (`pause panel button=-1`), 3 / 3 | - |
| DISPLAY OPTIONS page | 4 -> menu | not pressed (A changes a live display row) | pad B -> `menu close` | - |
| DEV TOOLS page | 4 -> menu | - | pad B -> `menu close` | - |
| CONTROLLER OPTIONS | 4 -> menu | - | pad B -> the pause menu (`pause install`) | F = pad Y -> AIR / GROUND CONTROLS diagram toggles; hint `GROUND CONTROLS [F] BACK [Esc]` |
| AUDIO OPTIONS (before1) | 4 -> menu | - | pad B -> `SR_SetSettings 'ScrAudioOpt PAUSEMENU ...'`, `pause install` | - |
| Map (M) | 4 -> menu | pad A -> nothing (no items) | pad B -> closes the map (`context gameplay`) | F = pad Y -> the game's "THE FORTRESS OF SOLITUDE IS NOT AVAILABLE DURING AN OBJECTIVE" popup; hint `FORTRESS OF SOLITUDE [F] BACK [Esc]` (before1: a blank key) |
| That popup | 4 -> menu | pad A -> OK closes it | pad B -> nothing: the popup has no cancel (its hint row shows only SELECT) | - |

Prompts follow: `SELECT [Enter] BACK [Esc]` on the pause menu, the front end and the port pages,
`[F]` for the menus' Y (after1 frames 380, 346).
The harness TEST keymap is unchanged (it never reads the context): `archive/v1_menukeys/menukeys_legacy.json`
through tools/sr_drive.ps1 on the after build (ENTER = START, A = Z, B = X, BACK = Backspace: front
end SETTINGS and back, NEW GAME, START to the pause menu, MORE OPTIONS, DEV TOOLS, B out, the map
and B) = `RESULT: SUCCESS after 1 attempt(s)`, 24 / 24 steps (`archive/v1_menukeys/legacy1`).

Earlier rolls agree for the menus this roll does not reach: the Fortress and its COSTUMES
(`archive/v1_fortress/rolls/wearW2`: `context menu` from ScrFortress through SR_CostumeBegin, both
from LOAD and from the in-game map) and the in-game save prompt with the HUD slot picker
(`archive/v1_save/rolls/m1`, `n1`, `p1`: `context menu` around `hud ShowSaveSlots`). A walk of
the game's own pad routing (`BistroFW_EventHandler.HandleEvent` over `gSM.mScreens`, the
mouse-cursor reader) during gameplay found no UI screen taking the pad except one 20 ms
Main_Hud frame as the HUD loads, so a screen-owner override was tried (54dc08f3) and removed
(63356672): it bought nothing and cost an Apt walk per pump.

Not a menu, so not covered: the boot movies before the title read type 0 (Esc = START skips,
Enter does nothing); in-game cinematics (type 8) keep Enter = A, Esc = START (skip / pause).

### 3.2 The menus' X and Y actions (iter-465)

What the roll did show: the menus' X (SQUARE) and Y (CIRCLE) actions had NO key. The map's hint
row read `FORTRESS OF SOLITUDE [blank] BACK [Esc]` (before1 frame 706); COSTUMES WEAR is X; the
controller options' air / ground diagram is Y. Two new input.ini actions, `MenuX = E` (wear /
equip) and `MenuY = F` (Fortress), press X / Y in menus only (not in a cinematic); in gameplay E
stays PICKUP (A) and F SUPERPOWER (RT). The prompt source names them, so the hints show E / F. An
input.ini written before them keeps these defaults (parseIni starts from the defaults).

The costume viewer (viewer lane, not on dev yet) swallows A and wears on X, so there Enter does
nothing and WEAR COSTUME shows the E key: for "Enter = select" on that page the page itself should
take CROSS (A) as WEAR too (its A has no other use) - a viewer-page change, not a key mapping.

## 4. Mouse-look

### 4.1 Common
- **Raw, unaccelerated, relative.** SDL relative mode (WM_INPUT raw deltas on Windows, no
  system acceleration: `SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE` off; SDL3: per window,
  `SDL_SetWindowRelativeMouseMode` on the game window, float deltas carried to whole counts) while the window is focused, the
  context is Gameplay and `look = on`; the OS cursor (and the menus' cursor lane) otherwise.
- **Frame-rate independent.** Counts accumulate between polls; each sim tick consumes exactly
  what arrived since the last one. Rendering faster (docs/HIGH_FRAMERATE.md) changes nothing: the
  camera is a sim object.
- **Scale.** 0.001 rad (0.057 deg) per count x `sensitivity` - a full turn is ~6300 counts (7.9 in
  at 800 DPI). Host `invert_x` / `invert_y` flip it on top of the game's options.

### 4.2 On foot: the camera's own angles (port feature, `SR_PC_MOUSELOOK`, default ON)
A synthetic right stick cannot be 1:1 on foot [V]: the orbit camera (`OnFootCamera_Tick_sub_821855B8`,
vt 0x82005DC8) turns at a capped rate (yaw 2.05 rad/s = 117.5 deg/s, pitch 1.2 rad/s) and ramps the
input up while held (`CameraAxisInputSmoothUpdate_sub_82160A70`: yaw x0.1 -> 1.0 over 10 ticks,
pitch x0.2 -> 1.0 over 7; any pause or reversal resets it). A 90 deg mouse flick would take most of
a second. So the mouse turns the camera the way the game's own input path does, minus the cap and
the ramp: before the camera tick, `+164 yaw += d`, `+160 pitch += d` clamped to its own limits
(+628 / +632, -34 / +46 deg), and the side effects of `sub_82160690` (idle timer +192 = 0, follow
reference +64 = +96, so SMART CAMERA's recentre waits until Superman moves 1 m, exactly as after a
stick input). The game's sense is kept: a stick pushed right turns yaw DOWN unless X INVERSION is
on; pushed up turns pitch DOWN unless Y INVERSION is on - the mouse follows the same options.
It writes only when the mouse moved this tick; automated runs have no real mouse.
`SR_PC_MOUSELOOK=0` turns it off (the mouse then does nothing on foot).

### 4.3 In flight: the stick, with the flight law inverted
In flight the stick turns Superman and the camera follows him [V] (`sub_8220C5B8` reads the axes;
yaw `sub_8220E0A0`, pitch `sub_8220D230`): `d = f(y) x R(speed) x dt`, `f(y) = 0.5|y| + 0.5 y^2`
(`sub_821EB308`), y = the curve output, R = lerp(rate, rateMax) over the move record's rotVel
thresholds (SupermanFlight default: 100 -> 70 deg/s over 0 -> 44.7 m/s; hover 90 -> 60; boost 90).
These caps are the game's designed turn rates, so the stick is the faithful path. Each tick the
mapper takes the mouse's angle, divides by R x dt, undoes f (y = (sqrt(1 + 8o) - 1) / 2) and the
curve, and finds the exact raw value (the quantiser's inverse; unit-tested to read back exactly).
What a saturated stick cannot deliver this tick carries to the next ones (at most 3 ticks).
The flight hook publishes R for the current record and speed every tick.

## 5. The Dodge key

### 5.1 What the game does [V]
Superman's state machines are Lua + binary-XML graphs in `objtiv.AST`. The evade:
- OnFoot -> Block on `CheckInputEvent(BLOCK)`; Block -> OnFootEvasion when
  `(LOCO_MOVE or LOCO_STRAFE) and 60 <= GetDesiredFaceDirDelta() <= 300 and BLOCK`.
- The delta is `(facing - (atan2(stick) + camera yaw)) mod 360`: the stick is camera-relative, the
  angle is against Superman's FACING. [135, 225) = Evade_Back, [60, 135) = Evade_Right,
  [225, 300] = Evade_Left; within 60 deg of facing nothing (there is NO forward evade).
- **There is no flick test**: any stick past 25% works, so a keyboard key is exactly as good as a
  pad. "Quantum Dash" is the game's own name for this evade (KittenHint_54 "Perform a Quantum Dash
  evasion by moving while blocking"); the moves list calls the input SPEEDING BULLET (HOLD
  Action2 + LStick Down/Left/Right). The RB tap is a different move (SuperDash, a lunge).
- The evade exists only ON FOOT: OnFootEvasion is referenced only from the OnFoot graph; in flight
  and hover, B is the air block and nothing reads the stick (Xenia check: section 5.4).

### 5.2 The key
Holding Block (right mouse) + a direction key already evades, as on a pad. The Dodge key (Left Alt)
makes it one key and makes the direction RELIABLE:
1. Intent (camera-relative): A / D held = that side; else S = camera-back; else straight back from
   Superman's facing.
2. The intent maps to the nearest evade the game has, relative to his facing (forward intent =
   back): `chooseDodge`, a pure function.
3. The stick is set to the EXACT centre of that evade's window (90 / 180 / 270 deg from facing),
   computed from the on-foot camera's forward (follow point - position; the camera yaw +164 is
   atan2(fwd.x, fwd.z)) and Superman's facing (actor quaternion x local +Z). [V] golden: camera yaw
   = facing = 51.57 deg with the camera behind him; the world is right-handed, y up, so a forward
   (fx, fz) has its right at (-fz, fx).
4. The pad sequence (`DodgeMacro`): B held throughout; 2 ticks stick at rest (OnFoot -> Block),
   3 ticks stick at full deflection along the chosen direction (Block -> OnFootEvasion; the evade's
   enter script reads the delta while the stick is still deflected), 2 ticks at rest (no chained
   second evade), then release. 7 ticks = 233 ms; the evade itself (animation root motion) runs on.
Without a frame (camera not found) the stick is the camera-relative key direction, back = down.

### 5.3 Evade attack, combos
SURPRISE EXCHANGE (Speeding Bullet, then X): press Left mouse during the evade. Because the Dodge
macro only holds B for 233 ms, the evade's own cancel windows decide the timing as on a pad.

### 5.4 Does the ORIGINAL evade in the air? (Xenia)
Probe: `tools/controls/move_probe.py` + `archive/v1_controls/xenia/evade_air.pad` (ground evade as
the positive control, then hover and flight). Result: see section 10.

## 6. The new air moves (PORT FEATURE, `SR_PC_MOVES`, default OFF)

Neither exists in the original. They are built from the game's own systems so they look native:
- **The move is the game's nudge** [V]: `Vec3ComputeNormalizedDir_sub_821EAA18(mover, duration,
  distance, &worldDir, stopOnCollide, relative, profile)` - what the Lua `ApplyNudge`
  (`ScriptCmd_ApplyNudge_sub_8221B118` -> `sub_8220A350`) uses for the ram-attack recoil and Down To
  Earth. `MoveToPointUpdate_sub_821FABC0`, the last step of `LocoFly_Update_sub_82224A90`, then sets
  the character proxy's velocity to distance / duration toward the target each tick, so the
  game's own collision (`hkCharacterProxy_integrate`) runs; stopOnCollide ends it on contact.
  Afterwards the game's integrator decays the extra speed by itself.
- **No new animation**: the hover / flight blend reads the proxy's local velocity
  (CurLocalVelocityX/Y/Z), so the burst leans Superman as a fast strafe does. The retail data has
  no air evade / roll animation [V].
- **Trigger**: the Dodge key while airborne (the LocoFly hook saw Superman's flight mover last
  tick). The key then sends NO pad input: a host request, consumed by Superman's next
  `LocoFly_Update` before the game's body runs. With `SR_PC_MOVES` off the Dodge key in the air sends
  the same Block + stick input as on the ground, i.e. the game's own response (the air block).

| Move | When | What |
|---|---|---|
| HOVER DODGE | hover slot (LB held), or default flight below 22.4 m/s | 6 m in 0.2 s (30 m/s, ~1.9x hover top speed), level, sideways (A / D, with S diagonally back) or straight back |
| AIR STRAFE DASH | default flight above 22.4 m/s, A or D held | 10 m sideways in 0.25 s (+40 m/s) while the forward velocity is kept: the nudge goes along `v T + side L`, so its constant speed holds the forward component exactly |

Guards (refused moves log `[PC_MOVES] refused: <why>`): the mover's def is SupermanFlight or
SupermanFlightLock (not carrying, AirBrake, dashes, attacks); no nudge already running; the proxy
active and not keyframed; 0.5 s cooldown; not in superspeed (the game removes strafe there by
design) or brake / land.

### 6.3 Frames and signs [V]
Golden `sound_gameplay`: Superman actor 0x00719ED0, quaternion (0, 0.435, 0, 0.900) = yaw 51.57 deg;
local +Z rotates to (0.78, 0, 0.62) = his facing (the camera's forward too, yaw +164 = 0.900 rad);
local +X rotates to (0.62, 0, -0.78) = minus his right-handed right. `g_localRightSign = -1`.

### 6.4 Classification
A port feature, not forcing in the charter's sense: a gameplay addition a player opts into, like
the custom front end, never on in natural rolls (runs with it are not natural). The flight hook
(`sub_82224A90`) is registered as `port-feature` in docs/OVERRIDES.json: it always delegates to the
game's body once, and calls the game's own nudge start only with `SR_PC_MOVES=1` and a Dodge
request.

## 7. Rebinding: THE REMAPPER (input.ini format 3, PC SETTINGS > CONTROLS)

iter-466 controls2: PC SETTINGS > CONTROLS lists every action (sections KEYS - MOVEMENT / ACTIONS /
POWERS / CAMERA / GAME / MENUS; ids `bind_<Action>`, `kernel/pc_settings.cpp`, drawn by
`gfx/settings_ui.cpp`). A row shows up to FOUR keys or mouse buttons as chips. Click a chip (or
focus it and press Enter / A) and press the new key or mouse button - it replaces that chip; the `+`
chip adds one. Esc or a controller's B cancels; 8 s without input cancels. X (or the pad's X)
clears the focused chip. An input that another action of the same context also uses is drawn red
and listed in the pane (`pccontrols::findConflicts`: gameplay / menus / cinematics; the designed
shares - Esc = PAUSE and MENU BACK, a D-pad key for a menu move and a power - are not conflicts).
APPLY writes input.ini `[controls]` (`version = 3`) and reloads the bindings; REVERT / RESET work as
on the other tabs. A controller keeps its own buttons (the game's CONTROLLER OPTIONS): the remapper
binds keyboard and mouse only.

Format 3 = format 2 with up to four inputs per action (a format-2 file reads unchanged).

File: `<content root>/settings/input.ini` (the save root: `%LOCALAPPDATA%\SupermanRecomp\content`,
or `<exe dir>\content` with `portable.txt`; docs/SAVES.md). Written with the defaults on first
run. Automated rolls (SR_INSTANCE_ID) use a fresh scratch root and START on the defaults; an APPLY
in the roll reloads what the screen just wrote there, so a roll proves a rebind end to end (section 9).

```
version = 3

[controls]
# action = input[, input ...]   (up to four; 'none' unbinds)
MoveForward = W
Attack = Mouse1
TargetLock = Mouse3, Tab
...
[mouse]
look = on
sensitivity = 1.0
invert_x = off
invert_y = off
```

- Inputs: SDL key names (`W`, `Space`, `Left Shift`, `Left Ctrl`, `Left Alt`, `Tab`, `Return`,
  `1` ...; SDL_GetScancodeFromName, case-insensitive), `Mouse1` (left) `Mouse2` (right) `Mouse3`
  (middle) `Mouse4` `Mouse5`, `WheelUp`, `WheelDown`, `none`.
- The action names are `kernel/pc_controls_bindings.cpp kActionNames` (the defaults file lists
  them all).
- Other sections belong to other owners and are ignored here: `[pad]` (raw 360-control key
  bindings, the input lane's format 1 lines) and `glyphs = ...` (the input lane's prompt set).
- Bad lines are reported in the log (`[PC_CONTROLS] input.ini:<line>: ...`) and skipped; the
  rest of the file still applies.

THE CONTRACT WITH THE INPUT LANE (feat/input-glyphs; adopted there, docs/INPUT_GLYPHS.md 1.1-1.2):
the binding store is this ACTION-based one (the input lane's per-360-control format 1 is
retired); that lane keeps devices, hot-swap, player 1, rumble and glyphs. Whichever lane merges
second wires: (1) the prompt source - `hostinput::setKeyPromptSource` answering with
`pccontrols::inputName(primaryInputFor(<360 bit or kPadLT/kPadRT>, lastPollWasGameplay(),
config().bindings))`, `firstInput(b, kMove*)` for the left-stick directions and "MouseMove" for the
look stick while `config().mouse.look`; (2) `hostinput::noteKeyboardUse()` from the
`PC::mergePoll` block in `kernel/stubs.cpp` when its `*anyHeld` is true. `lastPollWasGameplay()`
is cached by the poll, so the render thread never re-runs the game readers. input.ini belongs to
this lane (it writes the defaults, incl. `glyphs = auto` under `[glyphs]`); the input lane reads
only the `glyphs =` line.

### 7.1 The host screens follow the menu bindings (lane E E3, RELEASE_PLAN step 5, 2026-10-23)

PC SETTINGS and the MODS library draw their own menus, so they map the keyboard themselves. One rule
(`pccontrols::hostKeyAct`, `kernel/pc_controls_bindings.cpp`) decides what a key does there, and the same rule
names the key caps (`hostKeyCap`, drawn by `gfx/overlay_kit.cpp kbLabel`), so a rebind shows at once and no key
acts that its cap does not show. For a player, in order (the first that names the key wins):
1. the failsafes: Enter / Keypad Enter = A (accept), Esc = B (back), whatever is rebound (coordinator ruling F3);
2. the MENUS bindings: MenuConfirm = A, MenuBack = B, MenuX = X, MenuY = Y, MenuUp..MenuRight = the D-pad
   (keyboard keys only; a mouse button bound there is not a host key);
3. the host-only keys no menu binding uses: the arrows, Tab = RB / Shift+Tab = LB (the tabs), PgUp / PgDn = LT / RT.

With the defaults that is Enter, Esc / Backspace, E = X, F = Y, Tab / Shift+Tab, PgUp / PgDn, and MAP's key (M) =
the pad's BACK / VIEW (PC SETTINGS: REVERT; MODS: close). The screens' own extras: F12 = screenshot (both), `/` =
search and Ctrl+F = search (MODS; Ctrl+F before the rule, so it searches although F is MenuY), and hold-to-preview
on PC SETTINGS = RT's key under the rule (`hostPreviewKey`: PgDn unless a binding takes it, the key the "Hold:
preview" cap names; P too in the test keymap; the key that starts a preview holds it until its own release). The
MOVE cap is the arrows picture only while the arrow keys move (all four on PC SETTINGS, where LEFT / RIGHT adjust);
otherwise it names the keys that do (`hostMoveCap`, e.g. "W/Down"). A modal button shows A when it is focused and
B when B triggers it (both when both); a disabled RESTART NOW never takes the focus. Ctrl+F searches from the
search box too, and never types an "f".

**Changed for players (changelog):** the host screens no longer take the undocumented test aliases Z / X / C / V,
Q / E no longer switch tabs (E is the MENUS X action: COSTUMES WEAR on the game's menus, and now X on the host
screens too; the tabs are Tab / Shift+Tab), and Space / R no longer act as X / Y.

The harness channel (and the real keyboard in automated runs) keeps the iter-450S test keymap on the host screens
(arrows, Enter / Z = A, Esc / Backspace / X = B, Space / C = X, R / V = Y, Q / E / Tab = LB / RB, PgUp / PgDn), as the
pad merge does (section 8), so tools/sr_drive.ps1's key names drive them unchanged; `SR_PC_KEYMAP` or the channel's
player-keymap flag switches both to the player rule (`pccontrols::hostKeymap`). The caps follow the keymap the
prompts use (`hostCapKeymap`). The offscreen renders (difftest `--settings-render` / `--mod-library-render`) press
the player's key for a pad name (`hostKeyFor`, with its Shift for LB, also for `khold`); the harness channel's own
held Shift / Ctrl / Alt are its modifiers (`harnessKeyMods`). Tests: `difftest --pc-controls-test` E3 H1-H15 (H13b).

## 8. Automated runs and the harness channel

- iter-466: the PLAYER keymap goes through the action layer, a port feature: a roll that drives
  it needs `SR_PORT_FEATURES=1` (natural rolls ignore it: `[PC_CONTROLS] player-keymap input
  ignored`). The TEST keymap (sr_drive) is test tooling and still writes the pad report. The
  read-back reports the controller / test-keymap report with the action frame on top.
- `tools/controls/play_roll.ps1` runs one roll of a worktree build with harness_play (the player
  keymap, the cursor feed `cursor` / `cclick` / `chover` / `cclickitem <index>`, ViGEm pad commands
  `vplug` / `vpress` / `vtrigger` / `vstick`), `SR_HARNESS_VPAD=1`, the action trace and frames.
  `vstick` ramps (half, then full, 60 ms apart): SDL sends no `SDL_CONTROLLERAXISMOTION` for an
  axis's FIRST change after the pad opens, and player 1 moves to a pad on that event
  (`host_input.cpp`), so an instant full deflection after a key press left the keyboard as player 1
  and the stick never reached the game (parity1). `vtrigger` reads 0 under `SR_HARNESS_VPAD=1`
  (RAWINPUT without XInput correlation; section 9). The PC SETTINGS screen and the cursor are drawn
  after the present-dump point: their proof is the log (`[PC_SETTINGS]`, `[MOUSE_UI]`).

- Real keyboard / mouse count only where `automation::policy().realInput` allows (players
  always; rolls only with `SR_HARNESS_REAL_INPUT=1`).
- The harness keyboard channel (`Local\SR_HARNESS_INPUT_<instance>`, kernel/host_automation.hpp
  layout v1) keeps the iter-450S TEST keymap by default - tools/sr_drive.ps1's key names are 360
  buttons delivered through it (`A` = the Z key, `B` = X, `ENTER` = START). The controls
  extension (additive; the version stays 1):

| Offset | Type | Writer | Meaning |
|---|---|---|---|
| 0x14 | u32 flags | harness | bit0 = PLAYER keymap for the channel's keys; bit1 = the mouse block is live; bit2 = the DUMP MARK (iter-467: with `SR_PRESENT_DUMP_FROM_MARK=1` the every-present dump window opens at the next present) |
| 0x300 | i32 dxTotal, i32 dyTotal | harness | cumulative relative mouse counts (the game consumes the difference) |
| 0x308 | i32 wheelTotal | harness | cumulative wheel steps, + = up |
| 0x30C | u32 buttons | harness | bit0 left, bit1 right, bit2 middle, bit3 X1, bit4 X2 |
| 0x320 | 24 B read-back | game | `<BBBBHBBhhhhII`: version, context, dodge phase, moves state, the pad report (buttons, LT, RT, LX, LY, RX, RY), a sequence counter, mouse-look polls |

- `SR_PC_KEYMAP=player|legacy` overrides the keymap for every keyboard source (A/B).
- `tools/controls/harness_play.py` drives the channel with a small script language (key holds,
  taps, relative mouse moves, clicks, wheel, log awaits, read-back samples; iter-467: `hold_ctx`
  holds a key until the read-back context changes, `mark dump` sets the dump mark).

### 8.1 The PAUSE MENU in automated rolls: the tape opens it (resolved, iter-467 hooks lane)

Nothing opens it by itself. The flicker gate and the perf gameplay rolls play
`tools/player/gameplay_play.txt`, and that tape has a deliberate pause round trip about 53 s after
`main_hud` (Esc = START in gameplay, then Esc = B to resume, 4 s later) - the proof that Esc
reaches the game's pause. Evidence (Frida, `archive/d3/rolls/pause_probe5`-`7` in the hooks
worktree; dev gates `archive/v1_orch/postmerge_sj`, `postmerge_hooks/gate2`):

- **The trigger.** The tape's Escape goes through the harness channel with the PLAYER keymap, so it
  never enters the XInput report (`XamInputGetState` and `[KBPAD_OBS]` show nothing - which is why
  the first look saw "no input"). The mapper turns it into START and the action layer
  (`kernel/pc_actions.cpp`, section 2.2) writes the gameplay context's START command (event 81,
  EventButton ctl 4) fired byte after the game's own evaluation. `PauseFlowStateTick_sub_82151298`
  (gameplay state 3) reads action 81 true and requests `PAUSEMENU`. pause_probe7: action 81 true at
  10:57:40.603, `ScrPauseMenu` at .604, the tape's `tap Escape 150` ending at .739 (begun about
  .589); the game's own `EventButton::Update` never fired (the press is not in the pad report). The
  resume is the tape's second Escape (action 127, B in the menu context): probe5 46.7 s.
- **Not involved:** `GameInput_PauseOnActivePadDisconnect_sub_8229AC90` -> `sub_822A0348` (never
  called; the active pad is MapInfo 0 = port 0, mode 1, throughout), the XNotify system-UI path
  (`PauseFlowStateTick`'s +83 stays 0; no `XNotifyGetNext` delivery near the pause), the profile
  sign-out watcher, and the iter-467 D3 stale-guard deletion (`sub_8246A1D8` walks thread-notify
  routines, not XNotify; the freeze rolls before it, `flicker_wave2`, show the same stuck menu).
- **Why the menu sometimes stayed up** ("menu over world 10" in the freeze roll): the resume tap
  is 150 ms, and the freeze roll's `SR_DIAG_SIM_FREEZE` holds the sim 2 s every 10 s; the pad is
  not polled during a hold, so a tap inside one never reaches the game. postmerge_sj: tap
  06:22:15.97-16.12 inside freeze #11 (14.225-16.227); postmerge_hooks gate2: 01:38:41.59-.74
  inside freeze #8; flicker_stale and flicker_wave2: inside freeze #6. When the OPEN tap is the one
  lost (flicker_wave #10, flicker_sj #20), no menu opens at all. The read-back shows it: a lost tap
  reads buttons 0x0000, a delivered one 0x0010 / 0x2000.
- **Console / Xenia parity, players.** A START press in gameplay opens PAUSEMENU through the game's
  own code on any platform; nothing here differs from the console. A player opens it only by
  pressing Esc / START. The sim freeze is a diagnostic (never on for players); a press shorter than
  a sim stall is dropped on the console too (the pad is read once per tick).
- **The fix (iter-467 hooks):** `harness_play.py hold_ctx <key> <ctx>` holds a key until the
  read-back context changes (2 -> 1 opens, 1 -> 2 resumes), presses again if it did not, and the
  tape DIES if it never does (perf_run writes `play_failed.txt`; the flicker gate FAILS on it). The
  player tape's round trip uses it. The flicker gate plays its own tape,
  `tools/perf/flicker_play.txt`: the same actions with no pause at all, started once Superman is on
  foot after the opening movie, and the span's dump mark (bit 2 below).

## 9. Verification

| What | How | Where |
|---|---|---|
| Mapping, ini, latch, Dodge chooser, look law, air-move geometry | `build/difftest.exe --pc-controls-test` (hermetic) | tools/difftest/pc_controls_test.cpp |
| The tests can fail | mutation check: 11 mutants (latch off, quantiser +1, flight-shape / sine inverses, dodge back = forward, no B in the macro, Attack = A, menu Esc = START, config C unswapped, strafe dash without the forward term, Dodge unbound) | archive/v1_controls/mutation_check.py |
| A controller alone is untouched | `testPadPassThrough`: every context x configuration x air, no key / mouse input = the report unchanged | difftest |
| Our build, live | `archive/v1_controls/ours/roll.ps1` (run_recomp_x + `tools/controls/harness_play.py` script + `tools/controls/move_probe.py --ours`), then `tools/controls/analyze_timeline.py probe.jsonl --play play.jsonl` | archive/v1_controls/ours/<tag>/ |
| The original game (Xenia) | `tools/xenia_pad.py` (held state + sticks) + `move_probe.py` with a pad script, `analyze_timeline.py` | archive/v1_controls/xenia/ |
| Menu keys (iter-465): Enter / Esc / E / F per menu, Esc = pause in gameplay, the test keymap unchanged | `archive/v1_menukeys/roll.ps1` + `menukeys2.play`, `table.py`; `menukeys_legacy.json` through tools/sr_drive.ps1 | archive/v1_menukeys/ |
| The action layer = the game's class code | `build/difftest.exe --pc-actions-test` (the commands built by the game's constructors, 3000 ticks; since 2026-10-20 also the real pad layout with the analog triggers, the controller gone and back, controller + keys at once, malformed layouts; section 2.3) | 444 / 0 (2026-10-20; 92 / 92 before the pad-layout passes); mutants 23 failures |
| Key = native pad, live | `tools/controls/play_roll.ps1 -Owner <slot owner> -Tag <t> -ExePath <exe> -Script tools/controls/scripts/controls2_parity2.play` (`SR_PORT_FEATURES=1`, `SR_HARNESS_VPAD=1`, the cursor feed), then `python tools/controls/parity_trace.py archive/v1_controls2/<t>` | parity1: attack, pickup, superspeed, power-freeze, land EQUAL; parity3: attack (K after the rebind), move, takeoff, land EQUAL; 0 ICALL_FAIL. parity4 (2026-10-20, the parity1 roll unchanged - `controls2_parity.play`, `SR_HARNESS_VPAD=1`, a ViGEm Xbox 360 pad, nothing reading real input - on the dev f25d0bd3 package 758027e1eeec, the pad-trigger fix merged): **superpower EQUAL key [59, 60, 69] pad [59, 60, 69]**, the game's own evaluation = `out` in the pad window ([59, 60, 69, 75] both); move, attack, pickup, superspeed, power-freeze, takeoff, land EQUAL (parity1: move / takeoff / superpower DIFFER). Its pause / map rows moved: a cinematic (`[PC_CONTROLS] context cinematic (igc type 8)`, 12.5 s, 2.5 s after pad-back; none in parity1) took key-pause2's Esc, so each later Esc / M / BACK / B landed one menu state off (back2 = the pause, M = INTERFACE_BACK in the pause menu, pad BACK = 2 / 82 in gameplay with `game` = `out` but no ScrMap request within 10 s - one roll, not chased); pause DIFFER as designed (113). `dev/archive/v1_controls2/parity4_padfix`. parity5 (`controls2_parity_v2.play`: `await_ctx 2` + 1 s before each menu press): the gameplay pairs EQUAL again, superpower included; the menu rows still not clean - the cinematic is a tutorial beat on the clock (101.4 s after the HUD request; parity4 106.0 s), it began 1 s after the await for pad-pause (START lost), the first Esc after it opened no pause, and the roll ended in the pause menu (await_ctx timeout) before the map pairs. The map check (pad BACK -> 82 -> ScrMap) stays open: parity4's 82 came in gameplay (`context gameplay (igc type 0) ... mode on-foot camera`, 3.9 s after a pause teardown), `game` = `out`, no ScrMap request. `parity5_padfix_v2`. **parity6** (`controls2_parity_v3.play`: v2 + the cinematic waited out before the menu section - a non-fatal `expect ... context cinematic 45`, `await_ctx 2 60`, 8 s): all 9 gameplay pairs EQUAL (superpower key [59, 60, 69] pad [59, 60, 69]) and the menu rows exactly parity1's: pause DIFFER key [1, 81, 187] pad [1, 81, 113, 187] (designed), pause2 [1, 81, 187] / back2 [], map DIFFER key [2, 82] pad [2, 82, 115] (designed), map-back EQUAL. **Pad BACK opens the map like M**: ScrMap on M (11:03:42.0) and on pad BACK (11:03:51.4). parity4's no-map was the game's own gate after the out-of-order sequence, not the port: `PauseFlowStateTick_sub_82151298` (run state 3) takes 81 first, then opens PAUSEMAP on 82 only when `FrontendUpdateGate_sub_8214D090` and `PauseFlow_CanOpenPauseMap_sub_8214D188` hold - the pause flow's map-enable byte +0x51 (set by `SetGameRunState_sub_8214F348(3)` and the HUD's `ClearMimLoaded`, cleared by run states 1 / 2 / 4 and `SetMimLoaded`), `SetAllowPause` (world flow +6), and NOT (the AUI state 8 with a render-client state 6) - and shows `Map_UnavailableInArea` instead while the world flow's objectiveBitIndex is 5 or 6; with `game` = `out` the layer delivered 82 either way. **Dodge** (`key only`): the script has no pad dodge (the Dodge key is PC-only, section 5; the pad's evade is B + a stick direction, never sent); the key's macro fired the same [8, 28, 35, 52, 53, 70, 71, 76] in parity4 / 5 / 6 - parity4's `[]` was `parity_trace.py`'s window (built from the `watch` samples, which began 646 ms after the key-down; the macro is over in 233 ms), fixed: a press window now opens at the key's own down / up edges (re-read: parity1's map-back = key [127] pad [], its samples began 2.9 s late; parity1's dodge `[]` stands - that build traced no Dodge macro). `parity6_padfix_v3` |
| The remapper, live | parity3: MAIN MENU > SETTINGS > DISPLAY OPTIONS by mouse clicks, E x4 = CONTROLS, ATTACK's first chip captured as K (`[PC_SETTINGS] bind bind_Attack slot 0 = K`), R = APPLY (`[PC_CONTROLS] reloaded ...`), then K attacks in gameplay with the pad's X events | archive/v1_controls2/parity3 |
| Mouse by direct calls, live | parity1 / parity3: hover = the focus manager's `selectNextItem` / `selectPreviousItem`; clicks on SETTINGS, DISPLAY OPTIONS, NEW GAME and the pause menu's RESUME = `gEH.HandleEvent(302)`; right-click = `gEH.HandleEvent(304)` (closes a menu, the pause menu and the map) | `[MOUSE_UI] call ...` lines |
| The triggers (SUPERPOWER, TARGET LOCK) on a controller | CORRECTED 2026-10-20: this row said the virtual pad's SDL RAWINPUT triggers (0 for an Xbox 360 HID pad) were the only gap and the difftest covered the trigger commands. The difftest's InputState had 16 digital bits and no analog slot, so it never saw the real layout, and the layer DID drop a controller's triggers (2.2). Live, 976b35b97d7d, the heat-vision tape (`SR_INPUT_REPLAY` writes RT = 255 into XINPUT_STATE +7, no SDL in the path), `SR_PC_ACTIONS_TRACE`: the game's own evaluation fires `[59,255]` on 120 ticks (the 4 s hold) and THROW 69 / POWER_RELEASE 60, while `out` has none of them and a spurious 75 on every tick; retail (Xenia) fires heat vision 3.5 s into the same hold, ours 0 of 4 runs | `s4-oracle/archive/step4_claude_20261018/oracle/ally_heat_a`; fixed by step4/pad-triggers (b74c495b). Live after the fix (arm (c), Ally, the fixed package f3105dc0957a (b74c495b), the heat_lock tape (flight_powers + LT 255 at +52..+54 s), `SR_PC_ACTIONS_TRACE`: `out` == `game` on all 8212 ticks (the bug's run: 121 ticks differ); 59 POWER_ACTIVATE 120 / 120 ticks from +40.04 s, 60 / 69 1 / 1, 75 150 / 150 (the spurious 75 gone); the LT lock-on 44 LLOCK_TOGGLE 1 / 1 (+52.04 s), 45 LLOCK_HOLD 60 / 60, 46 LLOCK_RELEASE 1 / 1; heat vision fires (beam, the wall on fire, the screen yellow / orange from the first dense frame +42.9 s to +44.1 s, the RT release at +44.04 s; retail orange to +44.4 s)): `ally_heat_c`. Through a real input path (SDL3, a ViGEm Xbox 360 pad's RT): parity4 superpower EQUAL key [59, 60, 69] pad [59, 60, 69], `game` = `out` (row "Key = native pad, live"). The SDL2-era RAWINPUT zero is gone with SDL3 (2f, 2026-10-18: LT 255 / RT 254 on the virtual Xbox 360 pad) |
| Nothing else changed | the level-skip plan EQUAL to its Xenia oracle with `SR_PC_MOVES` off (`skip_run.py run --side ours --executor kernel`, `xo_report_diff.py`, `kernel_skip_check.py`) | archive/v1_controls/skip/ |


## Release guest-memory admission (2026-10-16)

The R3 chapter-17 route pilot exposed a prerequisite failure before route input:
release package `6d11b644e78927394ff1037a11c5dfeb5910684a00d6ff34622fef393ec72c71`
reported both PC control/action readers OFF because their old whole-arena
`flatModelReadable` check encountered the mandatory `PAGE_NOACCESS` hardware
aperture `[0x7FC80000, 0x80000000)`. The game reached HUD, but the control-context
prerequisite timed out; no route ticket or ending was observed. Driver exit 5 and
play failure remain failures despite graceful game exit 0, zero stranded threads,
and an empty owned process tree. The private save remained chapter 17/time 885.5.
Full receipts and source attribution are retained in dev's
`archive/milestone2_20261016/final_native_acceptance_r3_route2/assessment.json`;
the 375,896,064-byte disk-audio file remains on ROG with SHA-256
`b478e2a2a622e143fede573d08871f98bf6ff2076bae52b500a7727823fd586f`.
This pilot does not validate the ending route or native-zero acceptance.

`kernel/pc_guest_access.hpp` now gives retained release features a scoped
`guest_protect::ArenaLease`. The supplied base must match the registered, flat
arena. Each actual read/write/copy checks its entire extent and rejects declared
foreign protection; member/index arithmetic is widened before addition. Failed
reads cannot publish a valid zero-valued gameplay context. Camera writes, action
byte/halfword publication and restoration, move scratch writes, and Apt host
accesses use this admission. Feature-initiated guest calls retain the lease.
No feature changes page protection. Arena registration, retirement and the
protectors' declare-before-protect ordering remain the existing transport
contract; `guest_protect_census.py` must be run against the built release inputs.
This is memory-access admission, not synchronization with mutable guest objects.

Oracle retains the old whole-arena admission for control/action enablement and
its normal valid-input behavior. Collision diagnostics still require the strict
whole-arena readable proof and therefore refuse release's protected aperture;
their guest delegation is unchanged. There is no blanket readable override.

The standalone `sr_pc_guest_access_test` target compiles the real control,
action, move and Apt host implementations with `SR_ORACLE=0`, alongside the real
hardware protector. Only guest-call ABI/dispatch and arena registration are test
doubles. It exercises a real Windows no-access aperture, protected/crossing/
wrapped addresses, foreign/retired arenas, direct camera/action/move writes and
Apt restoration. `tools/difftest/test_pc_guest_access.py --exe <fixture.exe>
--out <fresh-archive>` retains actual positive and two named defect controls.
Run this fixture on the scheduled VM; it reserves/commits the normal 4 GiB arena.
The full release build, existing Oracle control/action tests, protection census,
and a new bounded route acquisition remain required before gameplay claims.
