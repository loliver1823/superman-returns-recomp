# Input: SDL controllers, hot-plug, and device-matched button prompts (iter-464)

**PORT FEATURE**, always on for players, off in automated rolls unless asked for. Code:
`kernel/host_input.{hpp,cpp}` (devices, player 1, rumble, what keyboard prompts name),
`gfx/glyph_subst.{hpp,cpp}`
(prompt icons), hooks in `kernel/stubs.cpp` (XamInput*), `app/platform_sdl.cpp` (the event pump)
and `gfx/d3d12_backend.cpp` (`glyphNote` / `glyphRemap`). Data: `data/glyphs/` (manifest + the
CC0 Julio Cacko art subset, copied next to the exe by CMake). Tests: `difftest --host-input-test`,
`--glyph-test`, `--host-input-live`; tools: `tools/glyphs/`.

## 1. What the player gets

- **Any controller, hot-plugged.** SDL3's gamepad API (SDL 3.4.18; SDL2's GameController API,
  2.32.10, until RELEASE_PLAN step 3): Xbox 360 / One / Series, DualShock 3/4, DualSense, Switch
  Pro / Joy-Con pair (Switch 2 Joy-Cons too, new in SDL3) and anything in SDL's mapping
  database (plus an optional `gamecontrollerdb.txt` next to the exe). Plug a pad in mid-game and
  it works at once.
- **Player 1 = the device used last.** Pressing a button, or pushing a stick or trigger past half
  travel, makes that pad player 1; a key that drives the pad report, or the mouse (a click, the
  wheel, or a deliberate move), makes the keyboard player 1. Keys nothing is bound to (hotkeys,
  Alt+Tab, PrintScreen) never take player 1 from a pad. The keyboard is always merged into the
  report (as before).
- **Prompts follow the device.** Every controller prompt the game draws shows the active device's
  button: the game's own Xbox 360 art for Xbox and unknown pads, PlayStation 4 or 5 symbols,
  Switch letters (positional, see 4.3), or the key / mouse input bound to that control.
- **Unplugging player 1's pad** reports "controller not connected" to the game, as the console
  did, until any device is used again or a pad is plugged in; the game's own reaction applies
  (`GameInput_PauseOnActivePadDisconnect_sub_8229AC90` raises the pause menu when allowed).
- **Rumble** (`XamInputSetState`, was a generic stub) drives player 1's pad.
- **Prompt set pin**: `glyphs = auto | xbox | ps4 | ps5 | switch | keyboard` in
  `<save root>/settings/input.ini` (save root = `%LOCALAPPDATA%\SupermanRecomp\content`, or
  `<exe dir>\content` with `portable.txt`). `auto` (also when the line or the file is missing)
  follows the device.

### 1.1 Keys and rebinding: the controls lane's binding store (decision 2026-10-02)

The brief gave this lane the binding store; while it was down the `controls` lane
(`feat/pc-controls`, docs/PC_CONTROLS.md) built the PC layout and an ACTION-based store
(input.ini format 2: `[controls]` action = input[, input], mouse buttons and wheel included,
`[mouse]` look / sensitivity / invert). Two stores for one keyboard would disagree, and the
action-based one is the right unit for a PC layout (one action can need different 360 controls
in different contexts). **Decision: adopt it.** This lane's own format-1 table (one line per 360
control) is deleted; this lane keeps devices, hot-swap, player 1, rumble and glyphs, and asks a
PROMPT SOURCE which input a keyboard prompt names:

- `hostinput::promptInputFor(Control)` -> an input name in the controls lane's vocabulary (SDL key
  names, `Mouse1`..`Mouse5`, `WheelUp`, `WheelDown`, plus `MouseMove` for a control the mouse
  drives - mouse-look on the right stick), "" when nothing is bound.
- Default source (before the controls lane lands): the keymap dev actually has
  (`host_automation.cpp mapKeysToPad`, the iter-450S keys): Return = START, Escape (and Backspace)
  = BACK, arrows = D-pad, Space (and Z) = A, X = B, C = X, V = Y, Q / E = LB / RB, W A S D = left
  stick. LT, RT, L3, R3 and the right stick are not mapped by it, so their keyboard prompt is the
  blank key (`difftest --host-input-test` P1/P2 prove the source names exactly what the keymap
  presses, through the real keymap function).
- The controls lane registers its own source (below), and its keyboard + mouse then appear in the
  prompts: Left mouse for X (ACTION), Right mouse for B (BLOCK) in gameplay but Escape in menus,
  E for A (PICKUP) in gameplay but Return in menus, Space for Y, Left Shift for RB, Middle mouse
  for LT, F for RT, Left Ctrl for LB, M for BACK, Esc for START, 1-4 on the powers d-pad, W A S D
  and the mouse for the sticks. Every one of those inputs has art (`--glyph-test` G5).

### 1.2 THE CONTRACT with the controls lane (whoever merges second wires it)

1. **Prompt source.** At `pccontrols::init()`:
   ```cpp
   sr::kernel::hostinput::setKeyPromptSource([](HI::Control c) -> std::string {
     const bool gameplay = /* the last poll's context */ == Context::Gameplay;
     const Bindings& b = config().bindings;
     switch (c) {
       case HI::kLStickUp:    return first(b, kMoveForward);   // and Back / Left / Right
       case HI::kRStickUp: ... // gameplay && config().mouse.look ? "MouseMove" : first(b, kLookUp)
       case HI::kLThumb: case HI::kRThumb: return {};           // no action presses L3 / R3
       default: return inputName(primaryInputFor(
           c == HI::kLT ? kPadLT : c == HI::kRT ? kPadRT : HI::controlButtonBit(c), gameplay, b));
     }
   });
   ```
   The context must be cached by the poll (the render thread asks; it must not re-run the
   game readers). The backend re-asks on every prompt-icon bind, so a context change or a rebind
   shows on the next frame (each variant is built and uploaded once).
2. **Device note.** In `kernel/stubs.cpp mergeKeyboardPad`, the `PC::mergePoll` block calls
   `sr::kernel::hostinput::noteKeyboardUse()` when its `*anyHeld` is true (the legacy path below
   it already does, from the report change).
3. **The file.** input.ini is the controls lane's: it writes the defaults; this lane never writes
   it and reads only the `glyphs =` line (any section). Its `defaultIni()` should carry
   `glyphs = auto` with a one-line comment so players find the switch.

## 2. How the game draws a prompt (inventory, verified)

Investigation: `archive/v1_input/glyph_inventory/` (dumps of all 820 Apt movies, decoded icons,
scripts). [V] = verified from bytes / IDA / a run log.

- **Every** controller prompt is an Apt image (DXT5) drawn by the Apt UI; nothing is drawn by
  C++ or by a font (the fonts have no button glyphs) [V].
- `ButtonIcon.SetIcon(icon)` (movie BUTTONICON, copied into MAIN - MAIN's copy is the one that runs) loads
  `'../common/ButtonHints/' + GlobalAdapter.GetPlatform() + '/Hint' + icon + '.swf'`.
  GetPlatform is the native `sub_822A0138` and always returns `"XBox360"` (no other platform
  names exist in the binary) [V]. The loader drops the folder: `AptAS2ActionGetURL2LoadMovie_sub_826BB0F0`
  strips `.swf`, `PkgLoadedCbImplCtor_sub_826D56B8` keeps the text after the last `/`, and
  entries are keyed by FNV-1a-64 of the upper-cased name (`ResNameHashFnv1a64NoCase_sub_82702FC0`)
  [V]. **So swapping the platform string can never select other art** - the per-device prompt has
  to replace the icon IMAGE.
- Run-log evidence: `[APT_REQ] path='../common/ButtonHints/XBox360/HintA'` [V].
- The hint movies are resident: `RtamStorageTableLoadAll_sub_8214EC68` preloads 54 names from the
  table at `0x829116B0` (A, B, X, Y, Hint{A,B,X,Y,LT,RT,Select,Start,Shoulder1-4,Stick,LStick,
  RStick,Dpad,Dpad_*}) [V].

| Surface | Movie(s) | How the icon is chosen | Icons |
|---|---|---|---|
| Button-hint bar ("A SELECT / B BACK") | BUTTONHINTS via each screen | `ShowHints(label, icon, ...)` literals in the screen's AS | A, B, X, Y, Dpad |
| Pause menu, save/load, extras, trophies, video clips | SCRPAUSEMENU, SCRSAVELOAD, SAVELOADSLOTS, SCRCONCEPTART, SCRTROPHY, SCRVIDEOCLIPS | the bar | B, A |
| Options, stats, power-ups, costumes, controller options, moves list, credits | SCRAUDIOOPT, SCRGAMEPLAYOPT, SCRSTATISTICS, SCRPOWERUP, SCRCOSTUMES, SCRCONTROLLEROPT, SCRMOVESLIST, SCRCREDITS | the bar | B / A / X / Y / Dpad |
| Popups ("LOAD A GAME?") | SCRGENPOP | the bar | A |
| Map | SCRMAP | `mHints.*.SetIcon` | LStick, RStick, Y, B |
| Tutorial / HUD teaching text, moves list, power-ups | HUDMESSAGE, SCRMOVESLIST, SCRPOWERUP via COMBODISPLAY | `[Token]` in text.AST strings (135 strings: `Moves_`, `TeachingText_`, `Move_`, `ComboHint`, `ComboLevel*_Ctrl`); `ComboDisplay.ParseCombo` maps `[Action1-4]` to A/B/X/Y and passes any other token as the icon name (`[Shoulder4]` -> HintShoulder4) | all Hint* |
| Button-mash prompt | MAIN_HUD `SetHudMashButton` (Lua `SetHudMashButton`) | `'../common/buttonIcons/' + PLATFORM + '/' + name` | big animated A, Y |
| Mini-game prompts | BUTTONMASHMIM, MOMENTUMMIM | C++ table `0x8201D138` = {A, X, B, Y} | big A/B/X/Y |
| D-pad powers HUD | DPADPOWERS | baked image (char 5) | silver d-pad |
| Controls screen diagram | SCRCONTROLLEROPT char 4 (512x512 360 pad) | baked; labels from `CntrlOpt_*` text | **not replaced** (see 6) |

Dead in retail [V]: `Main_Hud.ShowHint` (its C++ callers are never called), the Jor-El controller
hints (`ShowJorElsController` is defined by no movie), the 128x128 "X360 stick" / LT / RT
placeholder movies, MIM_STICKS, HINTUP/DOWN/LEFT/RIGHT/QUESTIONMARK. `PRESS START` on the title
is plain text. Loading tips (`KittenHint_*`) are words only.

**Image format** [V]: each Apt image is a 128-byte header (u16 BE width, height, padded width and
height) and a DXT5 payload, 8in16-swapped, LINEAR block order; 32x32 hint icons = 1024 payload
bytes, 64x64 = 4096. Shared payloads: HintLT = HintShoulder3, HintRT = HintShoulder4, one d-pad
base for every HintDpad*, one stick body for HintStick/LStick/RStick, one shadow and one glow for
the big A/B/X/Y. Textures are straight (not premultiplied) alpha (the glow layer has RGB > A).

## 3. Mechanism: renderer-side icon substitution

Chosen because it covers **every** surface above with one hook, switches on the very next frame
when the device changes, needs no Apt or Lua edit, and writes nothing in the guest:

1. `glyphNote` (on every 2D texture (re)upload in `getOrCreateTexture2D`): if the format and size
   match a known icon (`candidate`), FNV-1a-64 of the first `len` guest bytes is compared with the
   identities in `data/glyphs/glyph_map.txt`; the slot is tagged with the icon and its base page.
   **iter-465 polish:** the live icon textures are TILED (`[TEX_CACHE] slot 285: fmt=20 32x32
   pitch=128 tiled=1 endian=1 base=0x1E430000`, player lane r5): the Apt renderer's texture holds
   the payload's blocks scattered over a 32x32-block macro tile, while the identities hash the
   LINEAR payload as ui.AST stores it. So no icon was ever identified live (0 `identified` lines
   in every iter-465 run) and prompts kept the Xbox art. `glyphs::identifyTiled` now de-tiles
   the base level first (Xenia 2D tiling, `gfx/guest_image.hpp`, no endian swap) and hashes
   that; every candidate logs one `[GLYPH] candidate #n ... detiled-fnv=... -> <icon>` line
   (the first 48). `difftest --glyph-test` G3c: the 19 payloads laid out tiled at pitch 128 are
   all identified; hashing the tiled bytes directly (the iter-464 path) finds 0.
2. `glyphRemap` (at both 2D bind sites): one vector read per bind; for a tagged slot whose fetch
   constant still points at the tagged base page, the active set's replacement slot is bound
   instead. Replacements are built on first use (WIC PNG decode, composition, full mip chain) into
   a 160-slot band reserved at init, never evicted. Identity SRV mapping: the replacement is true
   RGBA, which is what the guest fetch swizzle makes of the original.
3. Recipes (`glyph_map.txt`): `png:` (one pack PNG), `key:<Control>` (the input the prompt
   source names for it, 1.1), `cluster:` (four inputs in a plus, so the game's own d-pad
   direction highlight lands on the matching key; four times the same input - the mouse on the
   right stick - is drawn as ONE glyph), `blank`, `keep`. `extent` sizes the glyph's opaque box to
   the original icon's. `keyglyph <input> <png>` gives each key / mouse input its art (Mouse1-3 =
   the pack's left / right / middle mouse, Mouse4-5 = the plain mouse, WheelUp / WheelDown /
   Wheel, MouseMove = the four-arrow mouse). A texture pack can restyle prompts by editing it.
4. Keyboard prompts have VARIANTS (the bound input differs by context and by rebind):
   `glyphs::variant()` is a signature of the names a recipe shows now, and the backend keys its
   uploaded replacements by (icon, set, variant) - each variant is built and uploaded once.

Charter: a port feature at the renderer (the platform the console provided was the GPU); the
game's own draws, geometry, blend and sampler are unchanged, only the sampled texture differs. No
guest memory write, no guest call, no override (`docs/OVERRIDES.json` untouched).

## 4. Decisions

### 4.1 Xbox: the game's own icons
Viewed side by side (`tools/glyphs/preview.py` sheet): the game's glossy 360 face buttons and
white LB/RB/LT/RT/START/BACK pills fit its UI; the pack's flat Xbox set does not. Xbox One /
Series pads keep them too (A/B/X/Y and the bumpers match; only START/BACK differ in shape).
Unknown pads use the Xbox art (they follow the Xbox layout in SDL's mapping).

### 4.2 PlayStation and keyboard styles
PlayStation face buttons use the pack's `_Color` symbols (the game's face-button prompts are
coloured); keyboard prompts use the **White** keys (the game's shoulder/trigger/START/BACK prompts
are white pills). The PS4 "OPTIONS"/"SHARE" glyphs carry small text that is hard to read at 32 px.

### 4.3 Switch: positional buttons
Positional face buttons (UnleashedRecomp's choice; SDL2 needed `SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0`,
SDL3's `SDL_GAMEPAD_BUTTON_SOUTH / EAST / WEST / NORTH` are positional by definition): the south
button is the 360's A whatever its label, so muscle memory and the game's layout agree; the prompt
shows the label printed on that button (B for the 360's A, A for B, Y for X, X for Y).

### 4.4 Sticks on PlayStation / Switch
The "LS"/"RS" labels over a generic stick body are kept (platform-neutral). Keyboard: the labels
become the bound clusters (W A S D on the left stick; the four-arrow mouse when the mouse drives
the right stick, a cluster of its look keys otherwise) and the shared stick body is blanked
(HintStick alone, which shows only the body, is used by no retail string).

### 4.5 Deadzones
None added: the 360 driver adds none, and the game applies its own square deadzones 7849 / 8689 in
`InputXenonPad_Update_sub_825FFD40` [V, IDA]. SDL values are mapped to XInput's conventions (Y
inverted with `~v`, triggers `v >> 7`, as Xenia's `sdl_input_driver.cc`). A pad becomes player 1
from a stick or trigger only past HALF travel, so a drifting idle pad cannot steal player 1.

### 4.6 Isolation and natural rolls
The background-runs automation policy decides (`automation::computePolicy`: `SR_INSTANCE_ID` =
automated; `realInput` off unless `SR_HARNESS_REAL_INPUT=1`). Without `realInput` the module opens
no controller and ignores real mouse events for device tracking; real keys never reach the report
(mergeKeyboardPad), so they cannot pick player 1 either. The harness keyboard channel counts as
the keyboard. A roll draws the game's own prompts (oracle-comparable) unless `SR_GLYPHS` asks for a
set, and never reads the player's input.ini (its content root is a scratch folder).

| Variable | Effect |
|---|---|
| `SR_SDL_PAD=0` | the pre-iter-464 XInput-slot-0 path (no hot-plug) - A/B only |
| `SR_HARNESS_REAL_INPUT=1` | an automated run opens real pads and tracks real devices - never for rolls: the owner may be using his pad while one runs |
| `SR_HARNESS_VPAD=1` | iter-466: an automated run opens ONLY ViGEm virtual pads (`tools/glyphs/vpad_hotplug.py`): SDL RAWINPUT only (no HIDAPI, no XInput polling), and a device is opened only when its device tree reaches the `ViGEmBus` service with a ViGEm target VID/PID (`kernel/vpad_filter.hpp`); every other controller is logged `[INPUT] vpad filter: REJECTED (not opened)` |
| `SR_GLYPHS=auto\|xbox\|ps4\|ps5\|switch\|keyboard\|0` | prompt set (automated runs: off unless set) |
| `SR_GLYPHS=cycle` (+`SR_GLYPHS_CYCLE_MS`) | TEST INSTRUMENT: every set in turn, 5 s each |
| `SR_GLYPHS_DIR` | another glyph folder (tests, texture packs) |

## 5. Verification

- `build/difftest.exe --host-input-test` (SDL3, 2026-10-18: the checks below plus each family's
  glyph set through a plug-in + press - Xbox One, DualSense, DS4, Joy-Con pair, a standard pad -,
  an unplug with A + LT held leaving nothing held, rumble values reaching the pad's motors and
  stopping on a device move; hermetic by SDL hints at OVERRIDE priority, since SDL3 snapshots the
  environment): 44 checks, 0 failed on the build worker (platform/sdl3 89a3f19a); one-line mutants of the
  SDL3 port's input paths (rumble motors swapped, a DualSense on the PS4 glyphs, unplug not reporting NOT
  CONNECTED) are each caught. SDL2-era record: 34 checks, 0 failed, through SDL virtual joysticks
  (hermetic: real backends off): the default prompt source names exactly what the keymap presses
  (each named key fed through the real `mapKeysToPad`; LT/RT/L3/R3/right stick unnamed and
  unpressable), a registered source replaces it, the `glyphs =` pin read from a format-2 file in
  any section and SR_GLYPHS winning over it, the module never writing input.ini, hot-plug order,
  player-1 rules, NOT CONNECTED on unplug, reconnect, Switch/PS4/Xbox type detection from VID/PID,
  rumble, isolation.
- `SR_GLYPH_PAYLOADS=... build/difftest.exe --glyph-test`: 9 checks, 0 failed: the shipped manifest,
  every recipe builds (67 images), the 19 icons' real guest bytes identified, flipped byte / wrong
  size refused; keyboard art follows the prompt source (A -> K changes image + variant; B -> Mouse2
  = the right-mouse art; the mouse on the right stick = one glyph, not a cluster); every input of
  the controls lane's default layout has art of its own.
- Mutation check (`archive/v1_input/mutation_check.py`, worktree archive): 9 injected defects
  (Y axis not inverted, unplug never loses player 1, no isolation, identify hashes one byte short,
  keyboard art ignoring the registered source, a default name the keymap does not press, the mouse
  stick drawn as a cluster, the glyph pin read only before the first section, the right-mouse art
  missing from the data) - 9/9 caught by the test named for each.
- Previews: `build/difftest.exe --glyph-preview <dir>` (+ `SR_GLYPH_PREVIEW_LAYOUT=pc` for the
  controls lane's layout) and `tools/glyphs/preview.py <dir> sheet.png`: every icon x every set on
  the dark menu panel and a light sky.
- Bench hot-plug with real system devices: `build/difftest.exe --host-input-live 40` +
  `python tools/glyphs/vpad_hotplug.py --plan quick` (ViGEm Xbox 360 + DualShock 4). Found and
  fixed: without `SDL_HINT_JOYSTICK_THREAD` a hot-plugged XInput pad was never enumerated.
  ⚠ ViGEm pads are system-wide: plug them only while holding the game slot (another lane's Xenia
  sees them too).
- Live rolls: see the PROGRESS entry (front-end hot-plug roll, glyph cycle rolls, keyboard play).

## 6. Not done / known gaps

- The controls-screen diagram (SCRCONTROLLEROPT, a 512x512 Xbox 360 pad) is not replaced: it is a
  reference picture with callout text, not a prompt; a per-device diagram needs new art.
- 13 text strings name controls in words (6 have a PS2 wording in text.AST, which the loader
  `sub_822B0C90` drops); they still say Xbox words on other devices.
- The ICONCONTROLLEROPT menu pictogram (a generic gamepad) is left as is.
- MAIN_HUD compares `getPlatform()` with `'Xbox360'` while the native returns `"XBox360"`, so its
  Xbox 360 layout branch probably never matches (HUD layout only; observed, not changed).
