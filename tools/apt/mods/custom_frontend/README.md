# tools/apt/mods/custom_frontend - patch sources for the custom front end

Current player packaging is the built-in `data/port_features/custom_frontend`
SRPATCH1 recipe, applied to the player's disc at boot. The form-A instructions
below describe the authoring/debug build, not a mod the player must enable.

`50_costumes.patch` adds a native `ScrCostumes` extension. `costumes_data.aptasm`
splices after the retail getter; `srcostumes.aptasm` collects host additions and
binds reply callbacks only during each request; `costumes_input.aptasm` routes
X/WEAR only for rows carrying a stable `SRId`. Retail rows and their input remain
unchanged. `ExitScreen` deletes the helper object before unload. This path is
still runtime-unverified and lists nothing unless `SR_NATIVE_COSTUMES=1`.

Tracked sources of the `custom_frontend` content mod (`docs/CUSTOM_FRONTEND.md` §3-5, §12
steps 4-5). `tools/apt/build_custom_frontend.py` applies every `*.patch` here, in sorted
file-name order, to the user's own `DATA/ui.AST`. It writes the result to
`<content root>/mods/custom_frontend/DATA/ui.AST`, which is gitignored (form A, chosen by
experiment E1). The kernel overlay serves it in place of the disc's ui.AST while the mod is
enabled: add `custom_frontend` to `<content root>/mods/enabled.txt`, or build with `--enable`
(`docs/MODS.md`). Its `fscommand("SR_...")` calls go to the host bridge
(`kernel/mod_bridge.cpp`: EXIT, the settings store). The bridge is always compiled in and inert
for retail content (0 FSCommands); enabling the mod only makes its content issue commands.

| File | What it is |
|---|---|
| `10_menu.patch` | the step-4 patch: three code ops on SCRPRESSSTART (+ the step-7 LoadComplete op, + iter-464 the SRDevTools InitAction) |
| `srfrontend.aptasm` | appended to ScrPressStart's class InitAction (sprite 61): `_global.SRFrontEnd` (Install / Uninstall, the `FlowMgr.CppShowPopup` hook, IsLoadCreate, ShowMenu, OnReady, the ScrGenPop input wrapper, OnMenu, ShowExitConfirm, OnExit, Defer / RunDeferred) |
| `initgui_tail.aptasm` | appended to `ScrPressStart.InitGui`: `SRFrontEnd.Install(this)`, and re-show the menu when back from SETTINGS |
| `exitscreen_head.aptasm` | prepended to `ScrPressStart.ExitScreen`: `SRFrontEnd.Uninstall()`, then delete every SRFrontEnd method (the lifetime rule; the build checks the list against `srfrontend.aptasm`) |
| `loadcomplete_head.aptasm` | prepended to `ScrPressStart.LoadComplete`: re-apply the universal options after NEW GAME / LOAD (step 7) |
| `20_settings.patch` | step 7: the three options screens (`options_input_head.aptasm` = B/Esc/START back to the menu in the front end; `options_exit_tail.aptasm` = capture into the store) and `ScrSaveLoad.LoadComplete` (`saveload_loadcomplete_head.aptasm`, an in-game load) |
| `apply_store_fn.aptasm`, `host_settings_fn.aptasm`, `snapshot_expr.aptasm` | `.include` bodies shared by several movies: ApplyStore, the host reply method, the packed snapshot of the SettingsAdapter getters |
| `modopts_methods.aptasm` | iter-464, `.include`d by SRFrontEnd, SRPause and SRAutoSave: AutosaveOn / AutosaveLabel / FetchModOptions / OnModOptions / ToggleAutosave over `fscommand("SR_GetModOptions" / "SR_SetModOption")` (`_global.SRModOptions`) |
| `devtools_ui.aptasm` | iter-464, an InitAction in SCRPRESSSTART (`10_menu.patch`) and SCRPAUSEMENU (`40_pause.patch`): `_global.SRDevTools`, the renderer of the Dev Tools pages (ScrGenPop) over `fscommand("SR_DevMenu")`; the page model is `kernel/devtools_menu.cpp` (docs/DEVTOOLS_DESIGN.md 13) |
| `30_autosave.patch` + `srautosave.aptasm`, `hud_exitscreen_head.aptasm` | iter-464, MAIN_HUD + SCRSAVELOAD: `_global.SRAutoSave`, "Autosave without asking" (docs/CUSTOM_FRONTEND.md 5.6): answers the game's own save popups with its own buttons and drives the HUD's slot picker hidden; `SR_SaveEvent` at save / load |
| `40_pause.patch` + `srpause.aptasm`, `pause_exitscreen_head.aptasm` | lane E L-c, SCRPAUSEMENU: the 9-row pause menu (the nine retail buttons re-labelled / re-purposed, `edit-place` only: ACHIEVEMENTS, STATISTICS, MOVES LIST, OPTIONS, the PHOTO MODE slot hidden until unit P) and the two-choice QUIT GAME (QUIT TO TITLE / QUIT TO DESKTOP); `SRPause` (OnRow, OpenOptions, ShowQuit / OnQuit, Back, OpenDevTools, the GAME tab calls); the iter-464 MORE OPTIONS row and panel are gone (docs/CUSTOM_FRONTEND.md 5.14) |
| `60_genpop_hints.patch` | lane E E4 (step 5; F1, owner Q4 (b)), class ScrGenPop **in movie MAIN** (E3-E6 r3: MAIN registers the class first, so SCRGENPOP's own copy never runs - `patch_movie.py` THE FIRST-REGISTRAR RULE): every popup that B cancels shows "A SELECT  B BACK" (the game loads Generic_BACK and never shows it); an OK-only popup (one button, not cancellable) takes B as OK and shows the same bar; other non-cancellable popups (the save prompt, YES / NO) unchanged. With it the front end's MAIN MENU is cancellable: B asks QUIT TO DESKTOP? (`srfrontend.aptasm` ShowMenu / OnMenu) |
| `65_buttonicon_id.patch` | lane E E5 (step 5; F2: Apt-side), class ButtonIcon **in movie MAIN** (E3-E6 r3, as above): `ButtonIcon.SetIcon(icon)` also keeps `this.srIcon = icon` (it otherwise hands the name only to loadMovie), so `kernel/mouse_ui.cpp` knows each hint's button: a click on a hint (icon or label) sends that button's key, once; the menu census names it (`icon=`) |
| `66_parttext_glow.patch` | lane E E5b (step 5; owner Q1 (a); F2: Apt-side), class PartText **in movie MAIN** (E5b r13: MAIN registers the class first, so the PARTTEXT movie's own copy never runs - `patch_movie.py` THE FIRST-REGISTRAR RULE): `PartText.SetFontColor` gains two values only the cursor passes, both above every colour: 16777218 keeps the label's own colour on the label itself (`this.srOwn`, once) and shows the cursor's gold 0xFFC83C; 16777217 puts back exactly what THAT object recorded and forgets it (no record: nothing); any other value (the game's own call) voids the record. So the hover glow's restore can only undo the cursor's own glow on that very object (`kernel/mouse_ui.cpp` glowStep; a label the game recoloured, or another object at the same address, is left as it is) |
| `@autosave_without_asking/` (built, not a source) | iter-464: the build writes `mods/<mod>/@autosave_without_asking/DATA/baseworl.AST`, the disc's file with the chapter-advance debug print replaced by `PromptUserSaveGame()` (same length); an option folder (docs/MODS.md), mounted only while the option is 1 at boot |

What it does in the game: after PRESS START the game raises its own LOAD A GAME? popup; the
hook claims it and shows MAIN MENU (NEW GAME / LOAD GAME / SETTINGS / EXIT) in the same
ScrGenPop chrome. NEW GAME and LOAD GAME answer the game's popup with its own button indices
(0 and 1) through `GlobalAdapter.CppPopupComplete`; EXIT asks WARNING / QUIT TO DESKTOP? and
YES sends `fscommand("SR_Quit")`. SETTINGS opens a panel of the game's three options screens
(as an overlay over ScrPressStart); their values are the universal options: stored in
`settings/game_options.ini` under the SAVE root (next to the saves: `%LOCALAPPDATA%\SupermanRecomp\content`
by default, `<exe dir>\content` with `portable.txt`; NOT the content root that holds `mods/`;
docs/MODS.md "Host bridge"), re-applied after NEW GAME and loads, captured in
the pause menu too (docs/CUSTOM_FRONTEND.md section 0 and 5.4). Enter selects and Esc backs out in the mod's popups. Evidence: CUSTOM_FRONTEND §5.3.

- `*.patch` files use the patch language documented in `tools/apt/patch_movie.py`
  (`movie`, `replace-const`, `prepend-to` / `append-to` `function` / `action`,
  `add-initaction`, `add-doaction`).
- Code bodies are `aptasm` source (`tools/apt/aptasm.py`). A body goes inline or in a
  `*.aptasm` file that a patch names with `file=`. The build warns about an `.aptasm` file
  that no patch names.
- The build renumbers every patched movie's `.const` into the runtime's unload-walk order
  (`apt_graph.renumber_walk_order`) and fails unless every output movie survives a simulated
  unload + reload (`tools/apt/apt_unload.py`; the QUIT GAME soft-lock, docs/CUSTOM_FRONTEND.md
  5.4 item 4).
- The one register warning the build prints for `srfrontend.aptasm` is expected: register 1
  at the InitAction's top level is written after the class definition that used it (nested
  function bodies have their own registers and are not checked against the host).
