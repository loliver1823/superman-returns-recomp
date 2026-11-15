# In-game mouse cursor (platform feature)

User request 2026-10-01: "an in game mouse cursor that works with all the menu options and is
styled after the ui or something, but gotta make sure it's not too close in style so it
doesn't just blend in with the menus". Requirement and classification: memory note
`project_mouse_cursor`. Code: `kernel/mouse_ui.{hpp,cpp}`, `gfx/d3d12_overlay.cpp`; tests:
`build/difftest.exe --mouse-ui-test`.

**Classification: PC platform input + host UI**, the same class as the keyboard merge
(`kernel/stubs.cpp` `mergeKeyboardPad`). The player drives it; the game's own `HandleInput`
decides every outcome. Always available, no player toggle (USER 2026-10-02, section 6). Not a mod, not
a dev tool, not forcing.

## 1. Status

| Part | State |
|---|---|
| Static investigation (Apt mouse support, menu inventory) | done, sections 2-4 |
| Host reader of the Apt menus (observe only) | built; C++ proven on a synthetic heap (difftest M4), the Python twin on all three Xenia goldens |
| Direct calls of the menus' own functions (hover / click / back / wheel / slider drag / toggles / icon menus / thumb grids; iter-466, replaces the pad injection) | built |
| Cursor overlay (after the frame dump) | built |
| Live rolls on every surface | see section 8 |
| Player setting | none by design: USER 2026-10-02 "the custom frontend and cursor shouldnt have a toggle" |

## 2. Does retail Apt still handle the mouse? (Phase 1, question 1)

Apt is EA's Flash player and it shipped on PC, so the question was whether the Xbox 360 build
kept its mouse path. **Answer: it kept the vocabulary and the readers, but the input handler is
compiled out and no movie uses the mouse.** Mechanism 1 ("feed the mouse into Apt's own mouse
path") is therefore not available. Evidence (IDA, the read-only worker of the session; Xenia
golden dumps via `tools/golden.py`):

- **The names survive.** The builtin-name perfect-hash table at `0x82937420` (`{char*, id}`
  pairs) still maps `onPress` 211, `onRelease` 212, `onReleaseOutside` 213, `onRollOut` 214,
  `onRollOver` 215, `onDragOver` 202, `onDragOut` 201, `onMouseDown` 208, `onMouseMove` 209,
  `onMouseUp` 210, `onMouseWheel` 218, `Mouse` 20, `_xmouse` 21, `_ymouse` 22, and the
  MovieClip methods `getBounds` 117, `hitTest` 118, `startDrag` 126, `localToGlobal` 127.
  The clip-event mask table `0x82085988` (used by `AptSetClipEventFlagByName_sub_82684988`
  and `AptAS2SetDisplayObjectProperty_sub_826BBDA0`) has the SWF clip-event bits for all of
  them (Press 0x400, Release 0x800, RollOver 0x2000, RollOut 0x4000, ...).
- **The readers survive.** `AptAS2GetMovieClipProperty_sub_826C0D30` cases 21/22 return
  `_xmouse/_ymouse` from the stage mouse position at `0x829A1788/0x829A178C` through the
  inverse of the clip's matrix chain; `hitTest` = `AptMovieClipHitTest_sub_82699C10`
  (bounds, or the shape callback `0x829A1AE8`), `getBounds` = `AptAS2GetBounds_sub_826A13B8`,
  `startDrag` = `AptAS2OpStartDrag_sub_826B4108` (reads the same mouse position).
- **The writer is gone.** The only writers of `0x829A1788/0x829A178C` reset them to 0
  (`sub_826830F8`, the exec-context reset; `AptEngineReset_sub_826B3068`).
  `AptHitTestPointInBounds_sub_82690030` (point-in-world-bounds against that position) has no
  caller (its only reference is `.pdata`). The executor's input command queue (exec+0x84,
  drained by `AptExecutorCmdDrain_sub_826D2558`) has three command types: 0 = key
  (`AptContextQueueEventKey_sub_8268D160`, posted by executor vt slot 20
  `AptEnqueueAuxInput_sub_826D2928`), 2 = analog stick (`AptScopedAnimTargetStore_sub_82691CC8`
  -> `sub_8268A538`), and **1 = a two-int command (vt slot 21 `sub_826D29E0`, the shape of a
  mouse position) whose handler `sub_8268D118` has an empty body**: it publishes and restores
  the Apt context and does nothing else. No code loads the event ids 211-215 as immediates
  (whole-binary search of the recompiled sources' disassembly), so nothing dispatches
  onPress / onRelease / onRollOver.
- **No movie uses the mouse.** A census of all 820 movies in ui.AST (`tools/apt` model):
  0 Button characters (character type 4 does not occur), all 235 PlaceObject clip actions
  are `Construct` (flag 0x40000), and no `.const` pool contains `onPress`, `onRelease`,
  `onRollOver`, `onMouse*`, `Mouse`, `_xmouse`, `hitTest` or `getBounds`.
- **How the menus take input instead.** `BistroFW_EventHandler` (MAIN 0xe89f) is a `Key`
  listener. `onKeyDown` -> `HandleEvent(keyCode, controllerId, ascii)` (MAIN 0xe94e): drop
  the input while `mInputLockMgr` holds a lock (0xeab6), run a global handler
  `mHandledInputs['KEY_'+code]` if any, then walk `gSM.GetScreenStack()` **from the end
  down to index 1** calling `GetScreenRef().HandleInput(...)` and **stop at the first screen
  that returns false** (0xeb22-0xec09). The key state the `Key` natives read is
  `dword_829A19F8` = `keyCode<<17 | controller<<2 | 1`, set by the screen pump
  (`AptScreenPump_826BF268`) from the queued events. Key codes: UP 38, DOWN 40, LEFT 37,
  RIGHT 39, SELECT 300, START 301, CROSS 302 (pad A), CIRCLE 303 (pad Y), TRIANGLE 304
  (pad B), SQUARE 305 (pad X).

Consequence: **mechanism 2** (host hit-test of the Apt display list, then move focus and press
the game's own confirm / back / slider inputs). Restoring the compiled-out writer alone would
make `_xmouse` valid but would still drive nothing, because no movie has a mouse handler.

## 3. The Apt object model the host reads

All of it verified on the three Xenia golden dumps (`tools/golden.py --dump nosound |
sound_menu | sound_gameplay`) with a Python twin of the reader
(session scratch `asobj.py`, `cls_check2.py`), and the C++ reader on a synthetic heap
(difftest M4).

| Thing | Layout |
|---|---|
| value object | `+0` vtable, `+4` flags: low 7 bits = type, bit 27 = live. Types: 1 string (`+8` -> string object, chars at `+8`), 5 bool (`+8` byte), 6 number (`+8` f32), 7 int (`+8` i32), 12 MovieClip wrapper (vt `0x82086FF0`), 14 Array, 17 `_global`, 19 Object, 34/35 functions, 3 undefined |
| property table | plain objects, arrays, functions: `obj+8` (vt[2] `sub_826819C0` = `addi r3,r3,8`); a MovieClip wrapper: `*(*(w+0x20)+12)` (vt[2] `sub_82680448`). `{+0 cap (power of 2), +4 entries [{name*, value*}], +8 __proto__, +12 prototype}`; `AsPropertyGet_sub_8268C228` special-cases those two names. A name is a string object: `+6` u16 hash (`AptStringHashFNV1a16CI_sub_82681C40`: FNV-1a 32 over lower-cased chars, low 16 bits, 0 -> 17767; cached lazily, 0 = not yet), `+8` chars. Empty slot 0, deleted slot `0x829311B4` (`AsPropertyProbe_sub_82684768`) |
| Array (type 14) | `+0x20` element array, `+0x24` capacity, `+0x28` count |
| AS `_global` | `*(0x829A1588)` (type 17). `*(0x829A1584)` is the native adapters object (GlobalAdapter, SettingsAdapter, ...) |
| MovieClip wrapper | `+0x18` next sibling, `+0x1C` parent wrapper, `+0x20` display instance D |
| display instance D | `D+4` state S, `D+8` flags (bits 26-31 = character type: 5 sprite, 9 movie, 2 text, 1/10 shape; `0x3C000000` = skip), `D+12` property table, `D+28` display list (pointer to the head wrapper) |
| state S | `S+4` character definition, `S+8` matrix pointer (6 floats a, b, c, d, tx, ty; null = identity), `S+12` colour transform (alpha = first byte), `S+22` u16 >= 0x8000 = placed, `S+24` bit 31 = `_visible` (getter case 8), `S+80` text bounds; shape bounds at chardef+16 |
| matrices | `AptMatrixConcat_sub_82684F38(A, B)` = A·B with B applied first; point = (a x + c y + tx, b x + d y + ty) |
| world bounds | `AptComputeCharBoundsRecurse_sub_8268E548`: world(child) = world(parent)·M(child); sprites recurse over the display list, text uses S+80, shapes chardef+16 |

Golden checks: the title screen (sound_menu) gives `mcLogo` (380,100)-(892,484), `mcTitle`
(314,430)-(952,519), `mcStartText` (0,561)-(1282,594), `mCopyright` (384,604)-(896,668); the
LOAD A GAME? popup (nosound) gives its two rows at (339.1,369.8)-(940.9,409.8) and
(339.1,401.8)-(940.9,441.8), matching the inventory's x 339.1 / width 600 / row pitch 32.
Screen stacks: nosound `[ScrBackground_IG L20, ScrPressStart L50, ScrForeground_IG L75,
ScrGenPop L900]`; sound_gameplay `[ScrBackground_IG L20, ScrForeground_IG L75, Main_Hud L10]`
(load order, not level order). Input locks: `gSM.mInputLockMgr.mLocks` (Array) is empty in
all three.

## 4. Menu surfaces (Phase 1, question 2)

From the AS bytecode of every screen (dumps `archive/v1_mouse/dumps/*.txt`, pseudo-decompiled
`*.dec.txt`; MOVIE:offset citations). Pad buttons, from the hint letter each screen shows
against the code it handles: CROSS = A, TRIANGLE = B, CIRCLE = Y, SQUARE = X.

**Framework (MAIN).**
- `CafeFW_FocusManager` (0x6c4b): `m_arrItems`, `m_selectedItemIdx` (an index into it),
  `m_nInputs`. `selectNextItem` / `selectPreviousItem` (0x70a2 / 0x72a2) step +-1, **wrap**,
  and skip items where `!(isEnabled() && isVisible())`, then `setSelected` + FOCUS sound.
  `_global.gFM` (pause, options, ScrSaveLoad) adds items with `addItemAtIndex(item,
  m_tabIndex)` and is never cleared, so it keeps stale entries of earlier screens.
- Controls (`Cafe2_Imp_BaseControl`, 0x405f): `setFocusManager(fm)` (0x42c6) binds UP to
  `fm.selectPreviousItem` and DOWN to `fm.selectNextItem`; that is the only navigation.
  Confirm is CROSS, read by the screen (`getSelectedItem`, then a branch on the item).
- Slider (0x10e44): RIGHT `increaseValue`, LEFT `decreaseValue`; `mCurrentValue`,
  `mMinValue`, `mMaxValue`, `mStepValue`; 10 sections `mSection1..10` at x = 489.8 + 17(i-1)
  in slider space; section i is full iff i <= (value-min)*10/(max-min) (`UpdateValue`
  0x11832); arrows `mLeftArrow` / `mRightArrow`.
- Toggle (0xc4dd): RIGHT next, LEFT previous (all retail toggles wrap); `mLeftArrow`,
  `mRightArrow`, `mToggleField`.
- IconMenu: `mIconHolder.icon_<i>`, `mSelectedIndex`, `mVisibleIcons`; LEFT / RIGHT, no wrap
  (0x2aa6 / 0x2b65).
- ThumbViewer: `mThumbsHolder.ThumbClip<k>`, `mActiveThumb`, grid of `ThumbViewer.mColumns`
  (a class static, 0x1e80); 4-way `MoveHighlight`, gated by a 300 ms tween (0x3145).

| Surface (movie) | Items | Nav | Confirm | Back |
|---|---|---|---|---|
| Title SCRPRESSSTART | none: START or CROSS -> `StartBootFlow` once `readyForInput` (0x4586) | - | START / CROSS | - |
| Title / in-game save slots (`Screen.mSlots`, SaveLoadSlots; MAIN_HUD) | private `mFM`: btn1..3 | UP/DOWN | CROSS | TRIANGLE |
| Every popup, ScrGenPop (LOAD A GAME?, the save prompt `PromptInGameSave_*`, QUIT/RESTART warnings, map "fortress unavailable", controller unplugged, the custom front end's MAIN MENU and SETTINGS panels) | `Screen.mFM`: PopupBtn0..n-1, x 339.1, width 600, pitch 32 | UP/DOWN | CROSS | TRIANGLE when `canCancel` |
| Pause SCRPAUSEMENU | `gFM`: btn1..btn9 (btn2/btn3 may be disabled) | UP/DOWN | CROSS | START / TRIANGLE |
| Audio options SCRAUDIOOPT | `gFM`: Music, SoundFX sliders 0..10 step 1 | UP/DOWN, LEFT/RIGHT value | - | TRIANGLE |
| Gameplay options SCRGAMEPLAYOPT | `gFM`: YInversion, AutoCenter, Vibration toggles | UP/DOWN, LEFT/RIGHT | - | TRIANGLE (in pause) |
| Controller options SCRCONTROLLEROPT | `gFM`: Config toggle (4 options); CIRCLE = air/ground diagram | LEFT/RIGHT | - | TRIANGLE |
| SCRSAVELOAD | `gFM`: btn1 Save, btn2 Load; then `mSlots` | UP/DOWN | CROSS | TRIANGLE |
| Fortress SCRFORTRESS, Main menu SCRMAINMENU, Bonus SCRBONUS, Control room SCRCONTROLROOM | IconMenu | LEFT/RIGHT | CROSS | TRIANGLE |
| Costumes / Trophies / Concept art / Video clips | ThumbViewer grid | 4-way | CROSS (full size); SQUARE = wear costume | TRIANGLE |
| Powers SCRPOWERUP, Moves list, Statistics, Credits | none (scrolling lists) | UP/DOWN scroll | - | TRIANGLE |
| Map SCRMAP | none (pan / zoom are sticks, handled in C++) | - | - | TRIANGLE / SELECT; CIRCLE = Fortress |

Reachability notes from the inventory: SCRREPLAY and SCRONLINEOPT have no movie in ui.AST;
SCRCONTROLROOM is not linked from the Fortress icons (inferred). The custom front end's pages
are ScrGenPop popups, so they are covered by the popup surface.

**Port pages (other lanes, 2026-10-02).** Every page the port adds is built from the game's
own widgets, so the same readers cover them:

| Page (lane) | Built as | Cursor |
|---|---|---|
| Custom front end MAIN MENU / SETTINGS, AUTOSAVE WITHOUT ASKING (fe, `srfrontend.aptasm`) | ScrGenPop panels, `mParams.context` = `SRFrontEnd` (no `kinds`) | popup rows: hover + click = A (the autosave row flips on A) |
| Dev Tools pages (fe/settings-devtools, `devtools_ui.aptasm`, host model `kernel/devtools_menu.cpp`) | ScrGenPop, `mParams.context` = `_global.SRDevTools`, whose `kinds[row]` = 0 action, 1 toggle, 2 slider, 3 readout | **value rows** (kinds 1/2): click on the left half = LEFT, right half = RIGHT (`SRDevTools.PopupKey` turns them into `adjust -1/+1`); the wheel over a value row = RIGHT (up) / LEFT (down). Other rows: click = A |
| Pause MORE OPTIONS (fe, `40_pause.patch` until lane E L-c) | gone: the 9-row pause menu re-purposes the nine retail `gFM` items (no clone; btn8 hidden and disabled until PHOTO MODE) | the rows read as before (the reader follows `m_arrItems` order, not names) |
| A mod menu, port settings rows (resolution / frame rate / controls) | not built yet; the fe lane's plan is the same host-page popup | covered by the contract below, no cursor change needed |

**The contract for host pages:** a page shown with `FlowMgr.ShowGenPop` whose callback context
carries `kinds` (an Array indexed by the row's focus-manager index; 1 = toggle, 2 = slider)
gets value rows; LEFT / RIGHT on those rows must adjust the value in place (as
`SRDevTools.PopupKey` does). A page without `kinds` is a plain popup (click = A). The reader is
observe-only and tolerates a sparse `kinds` (numeric keys stored as named members).

## 5. The mechanism (built)

**Which screen takes the input** (the game's own rule, `HandleEvent`): walk `gSM.mScreens`
from the end down to index 1, skip the pass-through screens (`ScrBackground_IG`,
`ScrForeground_IG`, `ScrVideoClips`, and `Main_Hud` unless its save slots are up; identified
by their prototype `_global.<Class>` property-table +12), the first other screen is the
target. Gameplay and cutscenes have no target, so the cursor is hidden there.

**What it shows** (first match on the target screen): its visible `mSlots.mFM` (save slots),
its `mFM` (popups), an IconMenu (`mIconMenu` / `mMenu` / `mIcons`), `mThumbViewer`, the
`gFM` items that are children of this screen (stale entries of other screens are filtered
out), the title (`readyForInput`: a click anywhere = START), else a scroll / back-only
screen (lists, credits, the map: the cursor shows, wheel and right click work).

**Input mapping** (iter-466 controls2, USER DECISION 2026-10-06 / charter 2a: the cursor calls the
menus' own functions; no pad state). On the guest thread right after the Apt pump, at most one navigation /
action call per pump (plus, for the hover glow below, at most one cosmetic `SetFontColor` beside it), through the game's own AS invoke sequence (`kernel/apt_call.cpp`: a transcription of
`AsInvokeByPath_sub_826B8DC0` inside `AptAS2InvokeByPathCtxRaw_sub_826BB990`'s context publish):

| Mouse | Call | Notes |
|---|---|---|
| hover an item | the surface's own focus step toward it, one per pump, re-reading the game's own selection: `CafeFW_FocusManager.selectNextItem` / `selectPreviousItem` (lists; they skip disabled items and play FOCUS, MAIN 0x70a2 / 0x72a2), `IconMenu.Next` / `Previous` (its own LEFT / RIGHT handlers), `ThumbViewer.MoveHighlight("left" / "right" / "up" / "down")` (what the screens' HandleInput call) | lists use the shorter way round (they wrap); thumb grids column first |
| left click on an item | focus it, then `gEH.HandleEvent(302 CROSS, controller)` - the game's input router, as its Key listener's `onKeyDown` calls it (locks, the screen stack walk, the screen's own HandleInput) | |
| left click on a toggle | focus it, then its own `nextValue()` (arrows: `prevValue` / `nextValue`) | every retail toggle wraps |
| left click / drag on a slider | focus it, then its own `increaseValue` / `decreaseValue` until its own `mCurrentValue` is the one under the cursor | closed loop on the value |
| left click on the title | the game's own START: the START control held for 6 pad polls through the PC action layer (`pccontrols::pulseStart`); `gEH.HandleEvent(301 START)` only when the action layer is off | lane E E5c (step 5): the game's input layer then raises its own INTERFACE_START (113), so ActivePadListener (`dword_8296EDB4`) arms the front end's pad / keyboard menu input (+16, which `MenuButtonRepeatTick_sub_8214FC78` waits for), sets the active pad and, on the first menu tick, `ProfileMgr_SetActiveUser`. A bare HandleEvent(301) started the boot flow but left that input unarmed (the r5 PC roll of 2026-10-23: keyboard UP after a mouse title click never reached the menu). The pulse is not keyboard use (the cursor stays awake, the prompt glyphs keep their device), and it is bound to its click (E5c r2): at most 6 polls and 250 ms, and cancelled once the title is no longer the screen, so START never reaches the next screen. Logged: `[MOUSE_UI] press START (title click = START: ...; front-end input armed=N)` (N before the click: 0 = the listener is there and not armed, -1 = no listener). The live proof (`tools/scenarios/step5_title_click.json`) is armed=0 at the click, then the harness keyboard's DOWN reaching the menu (`menu key=40`) and A opening SETTINGS. The one-shot `[MOUSE_UI] front-end pad / keyboard input armed (...)` line is not part of it: the menu tick that first sees +16 destroys the listener and clears `dword_8296EDB4` (`PressStartListener_Destroy_sub_822A3300`) in the same game tick, so a pump practically never sees it set (the E5c r2 Ally roll: never logged) |
| hover a button hint | its label takes the cursor's gold through the game's own `PartText.SetFontColor` with a value only the cursor passes (16777218), and its own colour comes back the same way (16777217) when the cursor leaves it, the mouse sleeps or loses focus, or the mod library opens | lane E E5b (owner Q1 (a)). The GUEST owns the glow: the custom front end's `66_parttext_glow.patch` (on the class inside MAIN since E5b r13: MAIN registers PartText first, the PARTTEXT movie's copy never runs) makes 16777218 keep the label's own colour on the label itself (`srOwn`, once) and 16777217 put back exactly what that object recorded (nothing without a record); any other value (the game's own call) voids the record. So a restore can only undo the cursor's own glow on that very object: a label the game recoloured, or another object at the same address, is left as it is. The host remembers no colour; before each call it finds the label as a hint label of some screen of the stack on a path read with no refused read (else it waits; a label no screen holds any more is forgotten with no call). While it stays hovered, the label must still carry the guest's record: a new object at the same address, or one the game recoloured (the record voided), gets the on again under the same rules, without fighting the game: re-sent ons in a row wait 2, 4, ... at most 64 pumps, and the wait starts over only once the record has held for 64 pumps in a row (a game that recolours the label, even every other pump, costs one call every 64 pumps at most). The two values are above every 24-bit colour, so no game call can mean them. A pump makes at most two calls: the glow's restore before the pump's main call; an on only in a pump with no other call and no input lock. The state changes only after a call succeeded: a refused restore keeps the glow and is retried after 2, 4, ... at most 64 pumps until it is done or the label is gone (never given up). Only hints with a recorded button (`srIcon`, the same port feature) are hovered. The first time a hovered label's record reads present the log says `[MOUSE_UI] glow record seen label=...` once: the live receipt that the patched method ran (`tools/main_receipt.py --live e5b`, E5b r13). Pad / keyboard players never see it; port features only |
| left click on a button hint ("A SELECT", "B BACK", its icon or its label) | `gEH.HandleEvent(<that button's key>)`: A 302 CROSS, B 304 TRIANGLE, X 305 SQUARE, Y 303 CIRCLE, START 301, SELECT 300 | lane E E5 (step 5). Once per click; nothing under the hint is clicked too. Needs the custom front end: `65_buttonicon_id.patch` keeps each ButtonIcon's name (`srIcon`), which the retail icon never stores, so with port features off no hint is clickable. The patch is on the class inside MAIN (E3-E6 r3: MAIN registers ButtonIcon first; the BUTTONICON movie's copy never runs). Hints come only from a snapshot with no refused read; while they are unknown (logged: `hints unknown`) a click acts on the rows alone, as before E5. ButtonHints bars only (section 9). The snapshot logs each one (`[MOUSE_UI]   hint i key=K rect=...`). A hint's world bounds span its label field's whole AUTHORED width (366 stage px each on the popup bar), so they overlap (the PC mouse roll of 2026-10-23: a click on BACK's centre landed on SELECT). The clickable rect is therefore the game's own layout (ButtonHints.AddHint: the label left-anchored, Icon._x = LabelText.GetWidth() + 5, the hint Icon._x + 32 wide): the hint's bounds in its own space, cut at that hint-local x, then put through the hint's world matrix (E3-E6 r4, kernel/mouse_ui.cpp hintExtent; cut before the transform since r5, so a rotated or reflected hint is cut on the right axis and side); a hint whose Icon is not a readable clip is not clickable, and two clickable rects that still overlap are both dropped (never trimmed). The census keeps the game's own bounds |
| right click | `gEH.HandleEvent(304 TRIANGLE)` | the game's back |
| wheel | one focus step per notch (as hover) | |
| middle click | `gEH.HandleEvent(305 SQUARE)` | e.g. wear a costume |
| left click / wheel on a host page's value row (Dev Tools) | focus it, then `gEH.HandleEvent(37 LEFT / 39 RIGHT)` (the page adjusts in place) | |

- **Hover is edge-triggered and motion-only**: it acts only when a real mouse motion changes
  the hovered item, so a still cursor never fights the keyboard or the pad, and a menu that
  opens under a resting cursor keeps the game's own default selection. Synthetic motion
  (window enter / focus gain) is ignored, and waking the cursor needs 6 px of motion. Any real
  pad or keyboard input puts the cursor to sleep AT ONCE (iter-466: `mouseUiNoteOtherInput` from
  the pad poll, so the cursor is hidden from the next drawn frame): nothing queued is called until
  the mouse moves again.
- Nothing is called while an input lock is held (`gSM.mInputLockMgr.mLocks` non-empty; every
  overlay load holds one until 500 ms after `ScreenLoaded`); the intent is kept. The one exception
  is the hover glow's restore, which only puts back the game's own label colour (no new glow
  starts under a lock).
- An untouched mouse costs nothing: the Apt snapshot runs only after a mouse event.

**Charter.** A PORT FEATURE (charter 2a; docs/OVERRIDES.json `sub_826D2718` port-feature): it acts
only on the player's explicit mouse input, calls the menus' own functions with the game's own
invoke protocol, and is off on natural rolls (`automation::portFeaturesOn()`; `apt_call` refuses
otherwise). The reads stay fault-free.

## 6. Coordinates, rendering, on/off

**Coordinates.** Apt draws its 1280x720 stage over the whole game frame (every screen movie
and MAIN are 1280x720; the 550x400 / 854x480 headers of components are authoring stages only).
The game frame is 16:9 at any internal resolution, and the present fits it into the output
with its aspect kept (render/resscale `pm4FitToBackbuffer` / `fitRect`: the largest centred
16:9 rectangle, bars on the other axis). So stage = (window - fit origin) * 1280 / fit width
(`mouseUiFitRect`, `mouseUiWindowToStage`, and the inverse `mouseUiStageToWindow` for the
harness feed). In a 16:9 window the fit is the whole client area (today's fixed 1280x720
window: identity); a 16:10 or 21:9 window (resscale's resizable / borderless modes) maps
through the bars, and a click in a bar lands outside the stage (nothing is hit). The overlay
draws at the real mouse position, window * (bbW / clientW, bbH / clientH), with the
backbuffer size read from the swapchain buffer itself (`rt->GetDesc()`, so a resized or 4K
swapchain is drawn at its true size), and **scales with the game image**: scale = fit height
/ 720 (1080p x1.5, 1440p x2, 4K x3). World bounds include every ancestor's matrix up to the
root, so a scaled root is already accounted for in stage space.

**Rendering.** `D3D12Backend::drawHostOverlay` runs after `dumpBackBuffer` and before Present
at all four present sites (three in `presentFramebuffer`, one in `pm4PresentFrame`), in its own
command list on the same queue, and records nothing while the cursor is hidden. So
`SR_PRESENT_DUMP` captures, replay scores and golden comparisons never contain it
(`D3D12Backend::dumpThenOverlay`; `SR_PRESENT_DUMP_WITH_CURSOR=1`, default OFF, swaps the
order for review captures only). The cursor
is procedural (one quad, a signed-distance-field arrow in the pixel shader, root constants
only), crisp at any size (scale = backbuffer height / 720). Style, per the user's "styled after
the UI but distinct":

- a crisp arrow with a faceted **crystal tip** (white to pale gold, two facets and a slow
  glint), the game's Fortress-of-Solitude motif;
- a **Superman-red body with a gold rim** (cape / S-shield) instead of the menus' blue and
  silver chrome;
- a **dark outline**, a soft gold glow and a **drop shadow**, so it reads on the bright title
  art and on the dark pause scrim;
- **hover**: a gold ring around the tip; **click**: a quick expanding gold-white pulse;
  pressed: a brighter rim.

**On/off.** USER 2026-10-02: "the custom frontend and cursor shouldnt have a toggle". The
cursor is always available to the player and has no setting: it shows itself when the mouse
moves over a menu, hides on pad or keyboard use and outside menus (gameplay, cutscenes,
movies), and works on the plain retail menus as well as the custom front end. In a menu our
cursor replaces the OS one; outside menus the OS cursor hides after 2 s without motion and
returns when the mouse moves. Developer gate only: `SR_MOUSE_CURSOR=0` (A/B; off means
nothing at all: no SDL use, no Apt reads, no pad bits, no overlay, the OS cursor left alone).
`SR_MOUSE_UI_LOG=1` logs snapshot changes and presses (`[MOUSE_UI]`; change-triggered, so the
lines are not event counts).

**Automated runs (bgruns input isolation, `kernel/host_automation.hpp`).** Rolls run the
player configuration (the cursor is on), but the REAL mouse counts only where
`automation::policy().realInput` allows: players always, automated runs (`SR_INSTANCE_ID`)
only with `SR_HARNESS_REAL_INPUT=1` (sr_drive `-ForegroundInput`). So a hand on the desk never
steers a background roll, the OS cursor is never hidden by one, and a roll without `mouse`
steps gets no mouse event at all: zero Apt reads, zero pad bits, no overlay (zero impact by
construction). Mouse steps arrive through the harness feed (section 7).

**Hand-off with mouse-look (feat/pc-controls, docs/PC_CONTROLS.md 3-4).** One owner at a
time, decided by SDL relative mouse mode, which the controls lane switches on only in the
game's Gameplay input context (window focused, look on):

| SDL relative mode | Who owns the mouse | Cursor |
|---|---|---|
| ON (gameplay, mouse-look) | the camera (raw deltas); its buttons are actions | sleeps: every mouse event is ignored, queued clicks are dropped, nothing is drawn, the OS cursor is left to SDL |
| OFF, an Apt menu takes input | the cursor: hover / click / wheel drive the menu (the controls lane maps no mouse button in a Menu context) | shown on mouse motion |
| OFF, no menu (cutscenes, movies, gameplay with look off) | nobody acts through the cursor | hidden; the OS cursor hides after 2 s still |

The cursor notices relative mode on every mouse event (SDL3: `pccontrols::relativeMouseOn()`, the
game window's `SDL_GetWindowRelativeMouseMode` as the controls lane's main-thread tick last read it - SDL3's
query is per window and main-thread only; SDL2's mode was global), and the
mouse-look owner may also report its switch with `mouseUiSetRelativeMode(bool)`; leaving
relative mode skips the next motion (SDL's pointer restore is not a move). The menu signal
differs by design: the cursor follows the Apt screen stack (which screen takes input), the
controls lane follows the game's input context; relative mode is the single arbiter, so the
two cannot both act on one click.

## 7. Tests

`build/difftest.exe --mouse-ui-test` (exit = failed checks): the hash, the mapping (identity,
1080p, the 16:9 fit in 16:10 and 21:9 outputs, the stage -> window inverse, 4K), the slider
formula against the retail Slider geometry, list / row / grid navigation, overlapping popup
rows, and the reader on a synthetic Apt heap laid out as the runtime lays it out (deleted and
hash-not-yet-cached slots, prototypes, arrays, a 2x-scaled parent): it picks the popup, the
title, nothing in gameplay, reports an input lock, and marks a host page's value rows from
`mParams.context.kinds` (dense and sparse) while a context without `kinds` stays a plain
popup. Mutation check: reversing the matrix order or dropping the `Main_Hud` pass-through
turns 2 checks red.

Live: `tools/sr_drive.ps1` steps take a `mouse` script (`move x,y`, `click x,y`, `rclick`,
`mclick`, `wheel +-n`, `drag x,y x,y`, `wait ms`; points in 1280x720 stage space; `moveitem i` /
`clickitem i` aim at the game's item i of the last `[MOUSE_UI]` snapshot, and `movehint BTN` /
`clickhint BTN` - lane E E5 - at the centre of button BTN's hint there: A, B, X, Y, START or SELECT). By default
the commands go through the exe's **harness mouse feed** (`SR_MOUSE_FEED=<attempt>/mouse_feed.txt`,
set by sr_drive; `mouseUiPollFeed`): lines `move x y`, `down|up left|right|middle`, `wheel n`,
read every pump and turned into the SDL events a real mouse produces, through the same
handler. The desktop cursor never moves and the window needs no focus for the mouse (the
user may be at the desk); while a feed is set, real mouse and window events are ignored.
`SR_MOUSE_FEED=desktop` in the gates uses the real cursor instead (SetCursorPos + SendInput,
sent only while the game is foreground and under the cursor, and stopped the moment a human
moves the mouse); it is real input, so it needs `-ForegroundInput` (sr_drive refuses it
otherwise, exit 3). Scenarios: `tools/scenarios/mouse_natural.json`
(retail title, popup, pause, the three options screens, slider drag, toggles, map) and
`tools/scenarios/mouse_femenu.json` (the custom front end: NEW / LOAD / SETTINGS / EXIT and
its settings pages, with a stored-value check).

## 8. Live verification

(Phase 3, filled in after the rolls.)

## 9. Rejected / not done

- Mechanism 1 (Apt's own mouse path): compiled out, and no movie has a mouse handler
  (section 2). Restoring the writer of `0x829A1788` would be a platform fix with no effect.
- Calling the focus manager's own `selectItemByIdx` from the host: rejected for the charter
  (a host call into guest code the game did not make); D-pad steps through the pad merge do
  the same through the game's own input path.
- Not mapped: the map's pan / zoom (sticks, handled in C++); a CIRCLE (Y) action has no mouse button (keyboard V
  still works) but is clickable where a hint bar shows Y (lane E E5). The hint bars drawn by ButtonHints (directly
  or through a screen's DisplayWell: the "A SELECT  B BACK" rows) are clickable since E5, with the custom front end
  on and only from a snapshot with no refused read. NOT covered: the map's own hint row (SCRMAP `mHints.*`, its own
  ButtonIcons outside a ButtonHints bar) and the moves list / tutorial combo text (ComboDisplay): their icons carry
  `srIcon` too, but nothing reads those layouts yet.
