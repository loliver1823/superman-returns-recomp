# Custom front end: NEW GAME / LOAD GAME / SETTINGS / EXIT

Status: BUILT, opt-in content mod (2026-09-30). Steps 0-8 done: 0, 1, 2 (overlay + E1: form
A chosen), 3, 4 (the menu), 5 (the SR_Quit bridge + a clean exit path), 6 (real saves), 7
(SETTINGS + the universal options, section 0 and 5.4) and 8 (whole-run verification through
the player's switch + the mod-off control + one blind Astra review, 5.5). The QUIT GAME
soft-lock with the mod ON (5.4 review fix 4) is **fixed and live-verified** (2026-09-30,
femenu_quit 2/2, 5.4 "Live verification"). The mod is still default OFF in
`mods/enabled.txt`; the recommendation is to switch it on (5.4 "Default-enable
recommendation"), but that file is changed only by a separate decision. Still to do: 9. Design evidence
comes from the five read-only investigation angles under `archive/v1_frontend/`
(`apt_format/NOTES.txt`, `movies/notes.txt`, `engine_overlay/notes.txt`,
`xam_storage/notes.md`, `visuals/notes.txt`), plus the IDA pool and the run logs cited
inline. Anything marked *not checked* is exactly that.

User request: *"a menu that shows new game, load game, settings and exit, recreated to suit
the existing UI."*

**Turning it on (players; no environment variables, no launcher).** From the repo root:

1. `python tools/apt/build_custom_frontend.py --enable` - builds
   `mods/custom_frontend/DATA/ui.AST` from your own `Superman Returns (USA)/DATA/ui.AST`
   (the game folder is only read) and adds `custom_frontend` to `mods/enabled.txt`.
2. Start `build/SupermanReturns.exe` as usual. After PRESS START the game shows MAIN MENU:
   NEW GAME / LOAD GAME / OPTIONS / MODS / EXIT (keyboard: arrows, Enter or A, Esc or B; OPTIONS is one
   screen since lane E L-b, section 5.12).
3. To switch it off, put `#` in front of `custom_frontend` in `mods/enabled.txt` (or delete
   the file). The next launch is the unmodified game. Details: `docs/MODS.md`.

Settings you change (in the menu or in the pause menu) are kept in
`settings/game_options.ini` next to the saves (`%LOCALAPPDATA%/SupermanRecomp/content`, or
`<exe dir>/content` with `portable.txt`) and apply live. Known limits: the AUDIO OPTIONS
sliders read 0 and do nothing (this build has no audio mixer, mod on or off); LOAD GAME has
been run only with no saves; the SETTINGS panel shows no BACK hint (B works); the labels are
English only.

---

## 0. User requirement: universal settings (2026-09-30)

"Settings will be universal: changes made carry into the save you load, and settings changed
in game sync to the main menu settings." **Built (step 7, 5.4); what the IDA work changed in
the design:**

- **Where the game keeps its options** (IDA, this session): the SettingsAdapter natives
  (registered by sub_822AF3E8) read and write two places. (1) The per-controller input
  records `*dword_8296EDB0` (16 B each: +0 scheme, +4 displayed scheme, +8 X inversion, +9 Y
  inversion, +0xA vibration, +0xB auto-center; setters sub_822A2D40/2D98/2DF8/2E58/2EB8/2F18,
  getters sub_822A7060/70D8/7100/7128/7150). (2) The audio mixer (music `sub_822AD450` /
  `sub_822AD1D0`, SFX `sub_822AD610` / `sub_822AD310`, via `dword_82976054` +
  `dword_82976070`). The options screens apply on change (volumes, vibration) or in their
  ExitScreen (Y inversion, auto-center, scheme): that is "live, no reload" already, by the
  game's own code.
- **`ProfileSettings_applyOptions_sub_822A45D0` is NOT where changes are applied.** It is the
  opposite direction: profile -> game, at sign-in (sub_822B4B18, sub_822B4BC8) and at NEW GAME
  (sub_822B32B8 button 0). It reads 4 dashboard settings through
  `XamUserReadProfileSettings` (0x10040023 auto-center, 0x10040024 movement control,
  0x10040002 Y inversion, 0x10040003 vibration; results at +72/+112/+152) and overwrites
  scheme (0 or 2), Y inversion and vibration, and forces auto-center on. So menu changes made
  before NEW GAME were lost at NEW GAME - the store must be re-applied AFTER it.
- **Save files DO carry options (the open question, answered by IDA).** The save's record
  table writes music, SFX and per-slot scheme / X / Y / vibration
  (`sub_82165088`, vtable 0x820C08C0) and a load restores them through the same setters
  (`ScriptCmd_FillRecordArray_sub_821652E0`, called from `AptDisplayListBuildTable_sub_8218E4A0`
  when the LOAD popup is answered, sub_822B32B8 +72 / PromptIGLoad sub_822B4ED8), right before
  `LoadComplete(true)`. So re-applying the store after a load overrides console behaviour: it
  is the named mod option **`apply_to_loaded_saves`** (store key, default 1 in the custom
  front end; 0 = the save's own options stay). Nothing of it runs with the mod off.
- **The store**: `settings/game_options.ini` under the SAVE root (`xamContentRoot()`, next to
  the saves: `%LOCALAPPDATA%/SupermanRecomp/content` by default, `<exe dir>/content` with
  `portable.txt`, `SR_CONTENT_DIR` when set; section 8), NOT the content root that holds
  `mods/` (key=value, kernel/mod_bridge.cpp; docs/MODS.md "Host bridge"). Written through `fscommand("SR_SetSettings")` whenever an options
  screen closes (front end AND pause menu, so in-game changes reach the menu); read once per
  process through `fscommand("SR_GetSettings")`. The AS copy is `_global.SRSettings` (packed
  int, kernel/mod_bridge.hpp); the host's reply is its only writer (5.4 review fixes).
- **Applying it** (`ApplyStore`, AS, through the game's own SettingsAdapter setters, only where
  the live value differs): at the menu claim (after the sign-in applyOptions), at
  `ScrPressStart.LoadComplete(true)` (after NEW GAME's applyOptions / after a load's restore)
  and at `ScrSaveLoad.LoadComplete(true)` (an in-game load).
- **`XamUserReadProfileSettings`**: a platform fix of its own (kernel/xam_profile.cpp,
  `SR_XAM_PROFILE` default ON), a Xenia port returning Xenia's profile defaults (vibration 3,
  others 0). The generic stub filled nothing, so the natural game read a zero-size buffer
  (vibration OFF). It is NOT served from the store: the title never writes the profile (no
  `XamUserWriteProfileSettings` import), applyOptions can only express scheme 0/2 and forces
  auto-center, and the store is re-applied after it anyway.
- **PC settings** (resolution scale etc.) are not built; they would ride the same
  `SR_Get/SetSettings` channel (9.3).

## 1. Decision in one paragraph

Build the menu **inside the game's own EA Apt UI**, as a content mod delivered through an
**asset overlay**. The game's Apt VM runs it, it draws with the game's own popup chrome
(ScrGenPop / DisplayWell / BasicButton / SupermanBodyFont), so it matches the look by
construction. It appears where the game already asks its only New-or-Load question: the C++
**"LOAD A GAME?" (LoadCreate) popup** raised after PRESS START. A patched **ScrPressStart**
claims that popup and shows a 4-button panel in the same chrome instead. NEW GAME and LOAD
GAME answer the game's own popup with its own button indices (0 and 1), so the native
new-game and load paths run unchanged. SETTINGS opens the game's own three options screens.
EXIT goes through a new **FSCommand → host bridge** that quits the process. The feature sits
behind **`SR_CUSTOM_FRONTEND=1` (default OFF)**; when it is off, no overlay is mounted, no
bridge is installed and the file layer resolves exactly as today.

The menu is only usable once the separate **XAM storage platform fix** lands (always on,
Xenia-faithful). Today the natural front end loops on "NO STORAGE DEVICE SELECTED". That fix
is the first build step, and it moves the natural baseline on its own.

A host-drawn overlay is **not** needed. Section 11 gives the evidence that the Apt route is
feasible and the conditions under which we would fall back.

---

## 2. What the game does today (ground truth)

### 2.1 The natural front-end flow

| Step | What happens | Evidence |
|---|---|---|
| 1 | `ChangeInterfaceState("FE")` loads BACKGROUND_IG + **ScrPressStart** + FOREGROUND_IG | `movies/MainLib.as` 0x187C-0x19F4 |
| 2 | ScrPressStart timeline frame 74 calls `SetReadyForInput(true)` and `SetStartText()` | `movies/ScrPressStart.as` @0x4A08 |
| 3 | START or A hides the logo, title and start text, sets `mInBootflow=true`, then calls `SaveLoadAdapter.StartBootFlow()` | ScrPressStart.as 0x45E4-0x467A |
| 4 | Native `StartBootFlow` (sub_822B57A8) runs the sign-in gate (sub_822B4B18): signed in means `ProfileSettings_applyOptions`, then `ProfileMgr_BeginSaveDeviceSelect_sub_822A7CC8`, which runs save enumeration and then `XamShowDeviceSelectorUI` | ida_contracts.tsv; `archive/v1_align2/attempt1/run.log` L94074-94131 |
| 5 | **Today:** the selector "fails", so the storage library posts SELECT, and sub_822B3880 raises the `No_Device_Warning` CppShowPopup (buttons SELECT A DEVICE / CONTINUE). A re-runs the selector, which loops | run.log L94085 / L94117 / L94138; frames `archive/v1_align2/attempt1/frames` 018-036 (`movies/align2_tail_sheet.png`) |
| 6 | After the device step, the C++ raises the **LoadCreate popup**: title key `LoadCreate_Profile_Title`, body `LoadCreate_Profile_Body`, 2 buttons `off_829137E4 = [LoadCreate_Create_Btn, LoadCreate_Load_Btn]`, colour 29153. The id is stored at saveFlow(`*0x8296EE8C`)+16 | IDA sub_822B37E0 decompile (this session); SaveFlow_ShowLoadCreateProfilePopup_sub_822B4C50; tape1 frame 024 shows it rendered |
| 7 | Button 0 "START NEW GAME": sub_822B32B8 writes the gamertag (or `#NO_PROFILE#`) into `dword_8296F21C+1`, calls `ProfileSettings_applyOptions`, then `sub_822A0C00(adapter,1)`, which invokes AS `listener.LoadComplete(true)` | engine_overlay / movies findings; IDA sub_822B32B8, sub_822A0C00 |
| 8 | `ScrPressStart.LoadComplete(true)`: `IsFirstHour() ? ChangeInterfaceState("IG") : SwitchScreen(SCREEN.MAINMENU)`. `"IG"` from FE loads MAINHUD and calls `StateAdapter.StartGame()`, which is sub_822A73E8, which calls `SetGameRunState(uiMgr,3)` | ScrPressStart.as 0x47AB-0x480A; MainLib.as 0x1AC0-0x1B66 |
| 9 | Button 1 "LOAD GAME": native calls AS `ShowSlotSelector(1)`, then `mSlots.Start(1)` (SaveLoadSlots in MODE_LOAD), then `SaveLoadAdapter.LoadTask(slot)` (sub_822B5748, then sub_822B4ED8). B in the slots calls `BootFlowLoadCanceled` (sub_822B37E0), which **re-raises the LoadCreate popup** | ScrPressStart.as 0x4684-0x4731, 0x477C; IDA sub_822B37E0 |

The LoadCreate popup has three raise sites: the guide-close tick `sub_822B5910`
(+0x100 1→2), `sub_822B6070` (only when `*(g_FeFlowConfig+144) == 1`), and
`BootFlowLoadCanceled sub_822B37E0` (IDA `get_xrefs_to 0x822B4C50`, this session).

### 2.2 What the retail UI has, and does not have

- **The real ScrMainMenu is not a New/Load menu.** It is a 2-icon IconMenu carousel
  (METROPOLIS → `ChangeInterfaceState("IG")`, FORTRESS → the Fortress hub). The game shows it
  only after a load when `IsFirstHour()` is false, and it was never requested in any archived
  run (`movies/ScrMainMenu.as`; the APT_REQ lists in `archive/v1_tapeplay1/attempt1/run.log`
  and `archive/v1_tape2/run.log`).
- **Settings** are three separate screens driven by `SettingsAdapter` natives:
  ScrGameplayOpt (Y inversion, auto-centre, rumble), ScrControllerOpt (control scheme) and
  ScrAudioOpt (music and SFX volume). Their Back and START handling is hard-wired to the pause
  menu or the Control Room (`movies/ScrAudioOpt.as` 0x06F2-0x0812; ScrGameplayOpt.as
  0x082A-0x08E2 has no Back at all outside `PAUSEMODE_MENU`).
- **Nothing exits the process.** `StateAdapter.QuitGame` (sub_822A0FE0) only sets
  `*(*0x8296DF5C+1)=1`. Lua `QuitGameUI` (sub_822A25E0) returns to the front end. ui.AST
  contains 0 `fscommand` / GetURL uses. `ICON.EXIT_GAME` and its art ship but no screen uses
  them (MainLib.as 0x148E).
- **Strings:** text.AST has `LoadCreate_Create_Btn` "Start New Game", `LoadCreate_Load_Btn`
  "Load Game", `SaveLoad_LoadGame_Btn` "LOAD GAME", `Label_ScrGameplayOpt` /
  `Label_ScrControllerOpt` / `Label_ScrAudioOpt` "… OPTIONS", `Generic_WARNING`,
  `Generic_YES` and `Generic_NO`. It has **no** "NEW GAME", "SETTINGS" or "EXIT" id
  (`visuals/uitext_all.tsv`, 1896 entries).

### 2.3 Two corrections to earlier records

1. **Every carved file name in `data/ui_ast_extracted/` is wrong.** Carve *n* holds the
   movie named by TOC record *n+2*, which holds for all 820 files. `0038_ICONMENU` is **MAIN**,
   `0040_MAIN` is PARTHIGHLIGHTBAR, `0045_SAVELOADSLOTS` is **SCRPRESSSTART**, and
   `kernel/main_apt_blob.h` ("MAIN") is PARTHIGHLIGHTBAR's .apt. Evidence: the TOC record MAIN
   is at ui.AST 835968 and its nested .apt at +0x68 = 836072, which is index.json's offset
   for 0038; the loose root `main.AST` equals `0038_ICONMENU.apt`; 55/55 self-naming movies
   check out (`visuals/apt_identity.txt`). ENGINE_MAP §262 ("MAIN = `0040_MAIN` … TRUE length
   0x1b51") describes PARTHIGHLIGHTBAR. **Select movies by TOC name through
   `bgfa.py movies()`, never by carve number.**
2. **The storage prompt is not caused by the `XamContentGetDevice*` stubs.** They are never
   called in any front-end run: there are zero `[STUB_CENSUS] XamContent*` lines in
   align2/tape2/tapeplay1, while the census is on and does log
   `XamUserReadProfileSettings`. The real cause is `stub_XamShowDeviceSelectorUI`
   (kernel/stubs.cpp:5511). It returns 0 instead of `ERROR_IO_PENDING` (997), and
   `DeviceSelectorTask_invokeXam_sub_82645D20` treats every return other than 997 as failure.
   It also writes the device id through **r8**, which is the XOVERLAPPED at mgr+0x33C,
   instead of **r7** (mgr+0x31C). The stub's comment assumes a u64 takes a register pair,
   which is false on PPC64. Details are in §8 and `xam_storage/notes.md` §0-2.

---

## 3. What the player sees

```
intro movies ─► PRESS START screen (unchanged, matches Xenia golden menu_pressstart_golden.png)
   │ START
   ▼
[invisible, native]  sign-in gate ─► applyOptions ─► save enumeration ─► device selector
                     (headless after the XAM fix: device 1, no popup)
   │ C++ raises the LoadCreate popup ──► claimed by the mod
   ▼
┌──────────────── DisplayWell, 620 px, GENPOP blue 0x0071E1 ────────────────┐
│                          SUPERMAN RETURNS                                  │
│   ▌NEW GAME▐        (PartHighlightBar on the focused row)                  │
│    LOAD GAME                                                               │
│    SETTINGS                                                                │
│    EXIT                                                     SELECT (A)     │
└────────────────────────────────────────────────────────────────────────────┘
      over the live Metropolis flyby, letterbox bars and EA copyright (unchanged)
```

Target look: `archive/v1_frontend/visuals/mock_menu_panel_vs_real.png`, a composite made
from our real tape1 frame 020 and the game's own font. It is **not** a game render. The panel
is the same size as the 2-button boot-flow popups (ScrGenPop: WIDTH 620, PADDING 32,
FRAME_OFFSET 64, 32 px button pitch, MAXNUMBUTTONS 5; `movies/ScrGenPop.as` lines 42-58 and
142-164).

With the gate **off** and the XAM fix in, the same point shows the natural 2-button
"LOAD A GAME?" popup.

---

## 4. The four items: exact game actions

All navigation goes through the game's own AS globals and adapters. The mod calls no native
function that the game's own UI does not call from AS.

| Item | Label (v1) | Action | What runs next (natural code) |
|---|---|---|---|
| **NEW GAME** | literal `"NEW GAME"` | `_global.GlobalAdapter.CppPopupComplete(pendingId, 0)` | GlobalAdapter native `sub_822AEF88` → owner vtable 0x8201D804 slot 1 → thunk sub_822B9C90 → `sub_822B32B8` button 0: profile name into `dword_8296F21C+1`, `ProfileSettings_applyOptions`, `LoadComplete(true)` → `ChangeInterfaceState("IG")` or `SwitchScreen(MAINMENU)` → `StartGame` → `SetGameRunState(3)` |
| **LOAD GAME** | literal `"LOAD GAME"` (the same text as `SaveLoad_LoadGame_Btn`) | `CppPopupComplete(pendingId, 1)` | `sub_822B32B8` button 1 → executor vt+32 `"ShowSlotSelector"(1)` → `ScrPressStart.ShowSlotSelector(1)` → `mSlots.Start(1)` (SaveLoadSlots MODE_LOAD, 3 slots, title "LOAD A GAME") → A: `SaveLoadAdapter.LoadTask(slot)` → `LoadComplete`. B: `BootFlowLoadCanceled` → the popup is re-raised → the mod shows the menu again |
| **SETTINGS** | literal `"SETTINGS"` | a second panel in the same chrome: `[Label_ScrGameplayOpt, Label_ScrControllerOpt, Label_ScrAudioOpt]` (localized), B = back **As built (5.4):** `SetFortressDisplayWell(true)` + `FlowMgr.SwitchLevel("NONE", SCREEN.GAMEPLAY_OPT \| CONTROLLER_OPT \| AUDIO_OPT, 60)`, the screen as an overlay over the still-loaded ScrPressStart (the design's `SwitchScreen`, what ScrPauseMenu does at 0x0CCC-0x0D72, left a reloaded ScrPressStart without InitGui). B, Esc and START in those screens return to the menu (§5.2). **Later:** a 4th row "PC SETTINGS" through the bridge (§9.3) |
| **EXIT** | literal `"EXIT"` | confirm panel: title `Generic_WARNING`, body literal `"QUIT TO DESKTOP?"`, buttons `[Generic_NO, Generic_YES]` with NO as default (copied from `ScrPauseMenu.showWarning` 0x0E8C-0x1048) | YES: `fscommand("SR_Quit", "")` → host bridge → `SDL_PushEvent(SDL_QUIT)`, the same as closing the window. NO or B: back to the menu |

Why answer the popup instead of calling `ChangeInterfaceState("IG")` directly: the native
button-0 path also stamps the save's profile name and applies profile options. The save flow
later compares that name (`SaveFailedWrongProfile`, sub_822B32B8 0x822b36a4-0x822b3710). An
AS-only New Game would skip both steps (`movies/notes.txt`).

Labels are literals in v1 because ScrGenPop passes `buttonLabels[i]` straight to
`SetText` (`movies/ScrGenPop.as` line 68) and PartText upper-cases the text. Localized ids
(`SR_FE_*` added to text.AST INTERFACETEXT through the same overlay; the BGFA writer
rebuilds text.AST byte-exact) are a follow-up, §12 step 9.

---

## 5. Which movies change, and how

*5.1 and 5.2 are the original design sketch. What was built differs where 5.3 and 5.4 say
so (notably: no BackFromSettings / SwitchScreen; the options screens are an overlay).*

Select each movie by its **TOC name** in `DATA/ui.AST`. MAIN, ScrGenPop, SaveLoadSlots and
text.AST stay byte-identical.

| TOC name | Misnamed carve | Change |
|---|---|---|
| `SCRPRESSSTART` (TOC #47, ui.AST +0x133C70) | `0045_SAVELOADSLOTS` | add the `_global.SRFrontEnd` object (a new InitAction), and append 2 calls to `InitGui` |
| `SCRGAMEPLAYOPT` | `0802_HINTLSTICK_ND` | prepend a 6-line FE-return check to `HandleInput` |
| `SCRCONTROLLEROPT` | `0814_SCRAUDIOOPT` | same |
| `SCRAUDIOOPT` | `0812_SCRPAUSEMENU` | same |

### 5.1 ScrPressStart patch (pseudo-AS; the source of truth is the `.aptasm` in §7)

```actionscript
// NEW InitAction, guarded exactly like the game's own class definitions
if (_global.SRFrontEnd == undefined) {
  _global.SRFrontEnd = new Object();
  var fe = _global.SRFrontEnd;
  fe.pendingId = undefined;          // C++ id of the LoadCreate popup we are answering
  fe.inSettings = false;

  fe.Install = function () {         // called from ScrPressStart.InitGui
    if (_global.FlowMgr.SR_OrigCppShowPopup == undefined) {
      _global.FlowMgr.SR_OrigCppShowPopup = _global.FlowMgr.CppShowPopup;
      _global.FlowMgr.CppShowPopup    = _global.SRFrontEnd.CppShowPopupHook;
    }
  };
  // same 10-arg signature as FlowMgr.CppShowPopup (MainLib.as 0x2345)
  fe.CppShowPopupHook = function (t, b, n, def, opts, can, close, col, cppId, delay) {
    if (n == 2 && _global.SRFrontEnd.IsLoadCreate(t, opts)) {
      _global.SRFrontEnd.pendingId = cppId;
      _global.SRFrontEnd.ShowMenu();
      return;
    }
    _global.FlowMgr.SR_OrigCppShowPopup(t, b, n, def, opts, can, close, col, cppId, delay);
  };
  fe.IsLoadCreate = function (t, opts) {   // structural marker: the game's own text keys
    var L = _global.LocalizationAdapter;
    // title AND first button (as built; PromptIGLoad_Title also reads "LOAD A GAME?")
    return (t == "LoadCreate_Profile_Title" || t == L.GetLocalizedText("LoadCreate_Profile_Title"))
        && (opts[0] == "LoadCreate_Create_Btn" || opts[0] == L.GetLocalizedText("LoadCreate_Create_Btn"));
  };
  fe.ShowMenu = function () {       // FlowMgr.ShowGenPop = the game's own popup helper
    _global.FlowMgr.ShowGenPop("", "SUPERMAN RETURNS", 4, 0,
        ["NEW GAME", "LOAD GAME", "SETTINGS", "EXIT"],
        _global.SRFrontEnd.OnMenu, undefined, _global.SRFrontEnd,
        false /*canCancel*/, true /*closeOnPress*/, 29153);
  };
  fe.OnMenu = function (popup, button) {
    if (button == 0 || button == 1) {
      var id = this.pendingId; this.pendingId = undefined;
      _global.GlobalAdapter.CppPopupComplete(id, button);   // the game's own answer path
    } else if (button == 2) this.ShowSettings();
    else if (button == 3) this.ShowExitConfirm();
  };
  fe.ShowSettings = function () { /* ShowGenPop 3 localized Label_Scr*Opt rows, canCancel=true, cb OnSettings */ };
  fe.OnSettings = function (popup, button) {
    if (button < 0) { this.ShowMenu(); return; }
    this.inSettings = true;
    _global.FlowMgr.SwitchScreen([_global.SCREEN.GAMEPLAY_OPT, _global.SCREEN.CONTROLLER_OPT,
                                  _global.SCREEN.AUDIO_OPT][button]);   // BUILT: SwitchLevel overlay, 5.4
  };
  fe.BackFromSettings = function () {   // NOT BUILT (5.4): the options patch unloads the overlay
    this.inSettings = false;
    _global.FlowMgr.SwitchScreen(_global.SCREEN.PRESS_START);  // InitGui re-shows the menu
  };
  fe.ShowExitConfirm = function () { /* Generic_WARNING / "QUIT TO DESKTOP?" / [Generic_NO, Generic_YES], cb OnExit */ };
  fe.OnExit = function (popup, button) {
    if (button == 1) fscommand("SR_Quit", "");   // GetURL "FSCommand:SR_Quit" → §9
    else this.ShowMenu();
  };
}

// appended to ScrPressStart.InitGui (after RegisterListener(this), so LoadComplete and
// ShowSlotSelector keep reaching the live instance):
_global.SRFrontEnd.Install();
if (_global.SRFrontEnd.pendingId != undefined) {           // back from a settings screen
  this.mcStartText._visible = false; this.mcLogo._visible = false; this.mcTitle._visible = false;
  this.mInBootflow = true;
  _global.SRFrontEnd.ShowMenu();
}
```

Design notes and the evidence behind them:

- **The hook replaces `FlowMgr.CppShowPopup` by member assignment and does not edit MAIN.**
  MAIN defines that function by `SetMember` on `_global.FlowMgr` (MainLib.as 0x2345), and the
  C++ reaches it by **name** each time through the executor (`vt+28`/`+32` →
  `AptAS2InvokeByPathCtx`, engine_overlay finding 3). A reassigned member is therefore what
  the C++ calls. Every other CppShowPopup call (No_Device, profile prompts, save errors) passes
  through unchanged.
- **Functions defined by a movie outlive that movie.** The game depends on this itself: every
  screen movie defines `_global.BistroBaseScreen` behind `if (!!_global.BistroBaseScreen)`, so
  later screens run the copy from whichever movie loaded first after that movie was unloaded
  (ScrPressStart.as 0x40B5, ScrPauseMenu.as 0x04ED). `_global.SRFrontEnd` follows the same
  pattern, which lets it survive the ScrPressStart ⇄ options screen swaps.
- **State across screen swaps lives in `_global`.** This is the pattern the game uses for
  `_global.PauseSelectedItem` (ScrPauseMenu.as 0x09B9).
- **Why `pendingId` is the right id to answer with.** `FlowMgr.CppCallback` reports
  `gPopupMgr.GetPopupId(popup)`, which is the `id` passed to `ShowGenPop`, which is `cppId`
  (MainLib.as 0x24C2, 0x2684). The native compare is against saveFlow+16, the value
  sub_822B15F8 returned (sub_822B37E0 decompile). That `cppId` equals that return value is
  *inferred*, not traced. Step 4 logs both sides to confirm it.
- **Callbacks are invoked as `selectedCallback.call(context, popup, index)`, with index −1 on
  cancel** (ScrGenPop.as 0x101C, 0x10E0). `context = _global.SRFrontEnd` makes `this` work
  inside `OnMenu`.
- Only constructs retail bytecode already uses: DefineFunction2, SetMember, CallMethod,
  NewObject, InitArray, If and Jump. No closures, `arguments` or Try.

### 5.3 As built (step 4, 2026-09-30) and what the game showed

Sources: `tools/apt/mods/custom_frontend/10_menu.patch` + `srfrontend.aptasm`,
`initgui_tail.aptasm`, `exitscreen_head.aptasm` (one movie patched: SCRPRESSSTART; the other
827 ui.AST entries byte-identical). Build: `python tools/apt/build_custom_frontend.py`
(`--enable` to switch it on for the player). Differences from the §5.1 sketch, each for a
reason found while building it:

- **No new InitAction.** The definitions are appended to ScrPressStart's own class
  InitAction (sprite 61, after its guard), so they run on every load of the movie and no
  new sprite id is needed (critique: the id must be an existing exported sprite).
- **Hook lifetime = ScrPressStart's lifetime.** `Install` (end of `InitGui`) swaps
  `FlowMgr.CppShowPopup`; `Uninstall` (start of `ExitScreen`) restores it, restores
  `ScrGenPop.prototype.HandleInput` and cancels a pending deferred popup, so nothing outside
  the loaded movie keeps calling code the movie owns. The game's own ExitScreen does the
  same for its class: `Object.registerClass("ScrPressStart", undefined)` (ScrPressStart.as
  0x04560-0x04567) and then `delete _global.ScrPressStart` (0x04569-0x04577: `Push reg2`
  (= `_global`, flags 0x129 preload this=r1, _global=r2), `'ScrPressStart'`, `Delete`
  0x3A, `Pop`). So on every reload the class InitAction's guard
  `if (!!_global.ScrPressStart)` (0x041F5) finds no class and runs the class body again;
  the mod's code, appended after that guarded block, runs on every load either way.
  (Re-checked 2026-09-30 against the bytecode after a review claimed the global survives:
  the review read the registerClass call and missed the Delete that follows it.)
  Functions are re-bound on every load; only plain state (`pendingId`, `inSettings`)
  lives on in `_global.SRFrontEnd`.
- **Stale-state guard.** `Install` drops `pendingId` unless the player left for SETTINGS
  (`inSettings`), and `OnMenu` with no pending id re-shows the menu instead of answering,
  so a front end reached again from the pause menu (QuitGameUI) never answers an old id.
  The step-7 options patch must also check `FlowMgr.CurInterfaceState == "FE"` before
  treating Back as "back to the menu" (the pause menu opens the same screens).
- **Chained popups are deferred** (`Defer` -> `setInterval(this, "RunDeferred", 20)`, the
  game's own `ShowPopupDelayed` pattern): ScrGenPop closes its popup AFTER the callback, and
  a popup shown inside the callback never appeared (roll loadexit1).
- **Keyboard:** the host maps Enter to pad START and Esc/Backspace to pad BACK
  (`INPUTCODE.SELECT` = 300). For the mod's own popups only, `GenPopInputHook` (a wrapper on
  `ScrGenPop.prototype.HandleInput`, installed from the first popup's readyCallBack) turns
  START into CROSS once the panel has been up 400 ms (the Enter that started the boot flow
  must not pick NEW GAME) and SELECT into TRIANGLE. Natural popups are untouched.
- **Strings** are constant-pool entries pushed with `EA_PushConstByte`, as retail code does;
  the only static strings are the `GetURL` operands. The labels stay literals (§4).
- **Title/body:** "MAIN MENU" / "SUPERMAN RETURNS: THE VIDEOGAME" (an empty body left a gap).
- **Diagnostics:** `fscommand("SR_Log", ...)` lines (`install`, `claim LoadCreate cppId=`,
  `menu key=`, `menu button=`, `answer cppId= button=`, `uninstall`) appear as `[SR_FSCMD]`
  in the log, each with the native `saveFlow+16`.

Live evidence (sr_drive, `-Gates SR_MODS=custom_frontend`, fresh empty save root per roll;
scenarios `tools/scenarios/femenu_new.json`, `femenu_load_exit.json`; artifacts
`archive/v1_frontend/menu/<roll>/attempt1/`):

| Roll | What happened | Frames looked at |
|---|---|---|
| new1, new2 | START -> `claim LoadCreate cppId=1012091577` (saveFlow+16 still 0: the C++ stores the id after the AS call returns) -> A -> `answer cppId=1012091577 button=0` with saveFlow+16 = 1012091577 (§13 Q1: equal) -> `APT_REQ #17 screens/hud/main_hud` 0.1 s later -> intro cinematic -> gameplay with the HUD | new1 042 (the panel, same chrome as the natural popup: `png/menu_crop_full.png` vs `png/natural_loadcreate_ref.png`), 058/069 intro; new2 `png/sheet.png` (050 panel with the body line, 070 Clark, 080 HUD on the street) |
| loadexit1 | LOAD: `answer ... button=1` -> the game's LOAD A GAME slot picker, 3 EMPTY SLOT, SELECT/BACK hints; B -> the game re-raised the popup (same cppId) and the menu came back; DOWN x3 + Enter -> `menu key=301`, `menu button=3` -> **no confirm** (the callback problem above) | `png/sheet.png` |
| loadexit2 | as loadexit1 with Defer: WARNING / QUIT TO DESKTOP? / NO / YES; Esc -> `menu key=300` -> back to the menu with EXIT focused; A, DOWN, A -> `SR_Quit` -> **process exited 0xC0000005 3.4 s later** (CRT teardown under running guest threads; the window-close path is the same code) | - |
| loadexit3, loadexit5 | same sequence with `hostExitNow` (SR_FAST_EXIT): `[EXIT] event pump ended` 6 ms after `SR_Quit`; loadexit5 journal: **`process exited (code 0)`** 0.17 s after SR_Quit; `parse_crash_log.py`: 0 crashes | - |
| loadexit4 | the A after the Esc never reached the guest (no KBPAD_OBS line; the game kept rendering the menu with EXIT focused, `png/tail_sheet.png`); harness input delivery, not seen in loadexit3/5 | tail sheet |
| off2 (gate off, `mod_title_capture.json`) | the natural LOAD A GAME? popup, 0 `[SR_FSCMD]`, 0 `[MOD]` lines, APT_REQ identical to overlay_fix/alias_probe (off1 stalled after `main` loaded, before the first screen: a boot flake with the mod off) | `png/off_popup.png` |

**Review fixes (2026-09-30, after d375f12).** Six should-fix findings; five fixed, one rejected:

1. `IsLoadCreate` now needs the LoadCreate title AND `opts[0]` = `LoadCreate_Create_Btn`
   (raw or localized): `PromptIGLoad_Title` (sub_822B4ED8, buttons [Load, Cancel]) also reads
   "LOAD A GAME?", and a title-only match would have answered it with the wrong buttons.
2. The FSCommand bridge handles `SR_*` whatever mod issued it (not only `custom_frontend`):
   a renamed copy would otherwise reach the game's body, which null-derefs the registry miss.
3. `femenu_load_exit.json` ends in an `expectExitCode: 0` step (2 s) - the pass condition;
   `femenu_new.json` requires `main_hud`. sr_drive opens the process handle at launch so the
   exit code is never read as `$null` -> 0.
4. `hostExitNow` leaves through `std::quick_exit` (the guest profiler's final dump is also
   registered with `at_quick_exit`) and the `SR_HLE_OWN_PRESENT=0` loop uses it too.
5. `build_custom_frontend.py` uses `patch_movie.refuse_game_folder` for every output, before
   any work (realpath: symlinks/junctions); a custom `--ui X/DATA/ui.AST` protects `X/`.
   `rt_patch.py` 24/24.
6. REJECTED: "ExitScreen only unregisters the class, `_global.ScrPressStart` survives". The
   bytecode (0x04569-0x04577) deletes `_global.ScrPressStart` right after the registerClass
   call; the §5.3 text now cites both.

| Roll (`archive/v1_frontend/fix/`) | Result |
|---|---|
| loadexit6 | SUCCESS: claim (x2, title+button marker), LOAD -> slot picker, B -> menu, EXIT confirm, Esc, YES -> `SR_Quit` -> `[EXIT] ... std::quick_exit(0)` 8 ms later, **exit code 0** within 0.13 s (`tail_sheet.png`: slot picker, menu, confirm) |
| loadexit7_fastexit0 | negative control, `SR_FAST_EXIT=0`: **FAILURE** "still running 2000 ms later" - the scenario now tells the old exit from the clean one |
| new3 | SUCCESS with `main_hud` required: `answer ... button=0` -> `uninstall` -> `APT_REQ #17 main_hud` 70 ms later (`sheet.png`: menu, intro, Clark, the shirt reveal) |
| off3 (`SR_MODS=0`, mod_title_capture) | natural LOAD A GAME? popup (`last.png`), 0 `[SR_FSCMD]`, 0 `[MOD]` |

Tests: `difftest --mod-overlay-test` 43 passed, 0 failed (M9a/b: `SR_*` with no mod and under
`my_menu`); sr_drive finalization 46/46, wiring 22/22, dwell 20/20; `audit_forcing.py` pass.

Not done / not verified: SETTINGS is a placeholder (error sound, menu again); LOAD was
verified only on an empty save root (the NEW GAME rolls made no XamContentCreateEx call in
their 60 s of play, so no save exists to seed); the look was judged against our own natural
frame, not a Xenia capture; no Astra review yet (step 8).

### 5.4 As built (step 7, 2026-09-30): SETTINGS and the universal options

Sources: `tools/apt/mods/custom_frontend/` - `srfrontend.aptasm` (ShowSettings / OnSettings /
GoSettings / ApplyStore / FlowMgr.SR_OnHostSettings, the SR_GetSettings fetch in Install),
`20_settings.patch` + `options_input_head.aptasm` / `options_exit_tail.aptasm` (the three
options screens), `loadcomplete_head.aptasm` (ScrPressStart), `saveload_loadcomplete_head.aptasm`
(ScrSaveLoad), shared `.include` bodies `apply_store_fn.aptasm`, `host_settings_fn.aptasm`,
`snapshot_expr.aptasm` (the patch language gained `.include`, tools/apt/patch_movie.py).
Host: `kernel/mod_bridge.cpp` (SR_SetSettings / SR_GetSettings, the store, the reply
channel), `kernel/xam_profile.cpp` (platform fix). Five movies patched, 823/823 other
ui.AST entries byte-identical.

- **SETTINGS** opens a second panel in the same chrome: SETTINGS / [GAMEPLAY OPTIONS,
  CONTROLLER OPTIONS, AUDIO OPTIONS] (the game's own `Label_Scr*Opt` text); B returns to the
  4-button menu.
- **The options screens open as an OVERLAY at depth 60** (`FlowMgr.SwitchLevel("NONE", id,
  60)` -> `gSM.LoadOverlayScreen`), not `SwitchScreen` at MAIN as the pause menu does.
  Rolls set1, set2 and back1 (`archive/v1_frontend/settings/`) showed that a ScrPressStart
  reloaded in the same session runs its `onLoad` (log) but never reaches `InitGui` (no
  `initgui`/`install`, its SaveLoadSlots left visible, input locked); re-running the class
  InitAction was not the cause (it did run: `initaction` logged on the reload, and keeping the
  class alive across the unload changed nothing). **Cause found later (item 4 below): the
  toolchain's `.const` item order broke the runtime's unload renumbering.** The overlay keeps
  ScrPressStart loaded, so nothing reloads; it stays until a live roll shows `SwitchScreen`
  back to ScrPressStart working with the fixed build. The screen manager hands input
  to the top of its stack first and the options screens return false, so ScrPressStart does
  not see it (MainLib 0x0EB31-0x0EC10).
- **Back**: B / Esc / START in an options screen, only while `SRFrontEnd.inSettings` and
  `FlowMgr.CurInterfaceState == "FE"` (the pause menu opens the same screens and keeps its
  own handling): `SetFortressDisplayWell(false)`, `ClearHints()`, `SwitchLevel(id, "NONE", 60)`
  (runs the screen's ExitScreen: the game applies its values, the capture stores them), then
  the menu comes back (deferred `ShowMenu(2)`).
- **Capture** (ExitScreen tail, all three screens, front end and pause menu): snapshot the
  SettingsAdapter getters into `_global.SRSettings`, `fscommand("SR_SetSettings",
  "<screen> <state> <packed>")`; the host replies with the value as stored.
- **Host reply channel**: the host calls `FlowMgr.SR_OnHostSettings(packed)` through the
  AptExecutor (vt+0x20, target path "FlowMgr") - the call the game's own natives make for
  `FlowMgr.CppShowPopup`. The first version targeted the SaveLoadAdapter listener
  (`0x8296EB00`), which points at an unloaded ScrPressStart while an in-game options screen
  is up; the FlowMgr path is always loaded (MAIN). Each movie binds its own copy of the
  method right before sending (options screens delete it after), so the host never calls
  into an unloaded movie.
- **Audio mixer**: in this build `dword_82976054` / `dword_82976070` are 0 in the front end
  (every `SR_SetSettings` log line carries `audioMixer=`), while the golden Xenia dump at the
  main menu has both set (0x03672F40 / 0x0385E718). So in our front end GetMusicVol/GetSFXVol
  return a cached 0 and SetMusicVol/SetSFXVol reach no mixer: the AUDIO OPTIONS sliders show
  0 and do nothing there. A platform gap (audio init), not fixed here. The store therefore
  takes volumes only while the mixer exists (else keeps the file's, or leaves them unknown;
  bit 15), and ApplyStore sets volumes only when they are known.

Live evidence (sr_drive, `-Gates SR_MODS=custom_frontend`, `archive/v1_frontend/settings/<roll>/attempt1/`;
scenarios `tools/scenarios/femenu_settings.json`, `femenu_settings_reboot.json`,
`femenu_settings_back.json`):

| Roll | What happened | Frames looked at |
|---|---|---|
| set1, set2, back1 | SETTINGS panel, AUDIO OPTIONS opened with `SwitchScreen`; back -> ScrPressStart reloaded: `onload` logged, no `initgui`/`install`, menu never came back (set2: keeping the class alive changed nothing). Host reply via the listener path delivered at boot (`host settings -1`). `audioMixer=00000000/00000000` on every capture | set1 `tail_sheet.png`: panel, AUDIO OPTIONS with its DisplayWell, then the reloaded screen with the slot picker visible; `audio_slider.png`: MUSIC slider empty (0) before and after LEFT x3 |
| set3 | options as an overlay (depth 60): SETTINGS -> AUDIO -> back -> menu -> SETTINGS -> GAMEPLAY -> RIGHT (`invY=1` captured) -> Esc -> menu -> NEW GAME -> `apply why=1 store=30720 now=14336` (Y inversion set again after the game's applyOptions) -> intro -> gameplay. Failed only on its own `main_hud` APT_REQ expect: `[APT_REQ]` is capped at 24 lines (audit_log_gating) and the settings trips used them | `sheet.png`: menu, SETTINGS panel, AUDIO OPTIONS, menu back, GAMEPLAY OPTIONS rows over the flyby (no well), menu, intro, Clark, the shirt reveal, HUD gameplay |
| set4 | SUCCESS: as set3, plus in game START -> pause menu -> DOWN x7 = AUDIO OPTIONS -> B: `SR_SetSettings 'ScrAudioOpt PAUSEMENU 14336'` (invY=1 in gameplay; the pause menu's own Back ran, the mod's FE handling stayed out) | `tail_sheet.png`: HUD gameplay, PAUSE MENU, AUDIO OPTIONS in pause (sliders 0: no mixer in game either), back to the pause menu |
| set5 | SUCCESS: as set4 but in game GAMEPLAY OPTIONS, DOWN, RIGHT = auto-center off: `SR_SetSettings 'ScrGameplayOpt PAUSEMENU 10240'` -> store `invert_y=1 auto_center=0` | - |
| reboot1 | SUCCESS, a new process seeded with set5's content dir: `SR_GetSettings -> 26624` at the first ScrPressStart load, `host settings 26624`, at the claim `apply why=0 store=26624 now=10240` (after the sign-in applyOptions reset them); SETTINGS -> GAMEPLAY OPTIONS; B; EXIT -> YES -> `process exited (code 0)` | `gameplay_opts_crop.png` (frame 134): **Y INVERSION: INVERTED, SMART CAMERA: OFF, VIBRATION: ON** - the menu change of process 1 and its pause-menu change, both shown by the game's own screen in process 2 |
| natural1 (`SR_MODS=0`, `tools/scenarios/natural_new.json`) | SUCCESS: natural title -> LOAD A GAME? -> START NEW GAME -> `APT_REQ #17 main_hud`; `[XAM_PROFILE] #1`/`#2` = the sign-in and the NEW GAME reads (count 4, size 168, 997); 0 `[SR_FSCMD]`, 0 `[MOD]` lines | `sheet.png`: title, the natural popup, the intro |

Not done / not verified:
- **Volumes**: no audio mixer in our build (front end AND gameplay), so the MUSIC / SOUND
  EFFECTS sliders read 0 and change nothing, and the store keeps volumes "unknown". Xenia has
  the mixer at the main menu (golden dump). A platform gap for the audio track; the store
  and ApplyStore are ready for it (bit 15).
- **LOAD with a real save** (the `apply why=2` path after a save's options were restored,
  `apply_to_loaded_saves`) and the in-game SAVE/LOAD copy in ScrSaveLoad: built, not run -
  no roll has a save yet.
- **Controller options**: opened and captured only through the shared code; no roll changed
  the scheme.
- **The ScrPressStart reload failure** (onLoad without InitGui): explained and fixed offline
  (item 4 below); not yet confirmed by a live roll.
- GAMEPLAY OPTIONS in the front end has no DisplayWell panel (retail never shows it there).
  **Fixed by the review below (finding 5).**
- No Xenia capture or Astra review of these screens (step 8).

**Review fixes (2026-09-30, after 24caf17).** Seven should-fix findings; five fixed, one
confirmed and upgraded to a blocker (not fixed), one process finding recorded:

1. `docs/OVERRIDES.json` `sub_826D39A0` now lists the reply channel's guest writes and calls
   (stack frame + two strings, `AptAS2AllocIntObj`, the executor invoke, the mod AS that
   assigns `_global.SRSettings`) and cites M10a-l; the `audit_forcing.py` allowlist reason
   names both replying commands.
2. `_global.SRSettings` has ONE writer, the host reply: the ExitScreen capture sends the
   snapshot without assigning it. A failed store write still replies with the value the file
   would hold (the capture + the ini's `apply_to_loaded_saves` + kept volumes), logged
   `NOT STORED (file write failed; kept for this session)`; an invalid capture (`NaN` from an
   undefined getter, out of range) gets no reply, so the last good value stays. Tests M10e
   (no reply), M10k (write blocked by a directory at the temp name: file kept, reply keeps
   `apply_to_loaded_saves=0`), M10l (canonical == read-back); a mutation restoring the old
   no-reply-on-failure behaviour fails M10k. `difftest --mod-overlay-test` 60 passed, 0 failed.
3. Store location corrected everywhere (section 0, docs/MODS.md, the mod README): the SAVE
   root (`%LOCALAPPDATA%/SupermanRecomp/content`, `<exe dir>/content` with `portable.txt`),
   not the content root that holds `mods/`. The stale design text in §4, §5.1, §5.2 and §6
   (SwitchScreen / BackFromSettings, "exactly two things", a bridge installed only under the
   gate) is marked or rewritten to match what was built.
4. **QUIT GAME with the mod ON soft-locks: FIXED 2026-09-30 (root cause and live rolls below,
   "Item 4"); the text of this item is the history.** Mod off (`quit_off1`,
   `natural_quit.json`): QUIT GAME -> title -> START -> the natural popup (SUCCESS). Mod on
   (`quit_on1`, `femenu_quit.json`): the reloaded ScrPressStart never runs onLoad/InitGui (slot
   boxes and the well over the title, input dead, later the attract loop). Bisected to the
   STRUCTURE of the edited SCRPRESSSTART (the append fails even when jumped over, `quit_v6`);
   two fixes on the AS side were tried and reverted (ENGINE_MAP "A ScrPressStart reloaded
   ... never reaches InitGui"). Per the 3-attempt rule the mod stays opt-in (default OFF) and
   this is a blocker for step 8. The same defect is why SETTINGS uses an overlay.
5. SETTINGS -> GAMEPLAY OPTIONS now shows the game's DisplayWell panel and its title:
   `GoSettings` calls the game's own `SetFortressDisplayWell(true)` before `SwitchLevel`
   (ScrGameplayOpt's InitGui never does; Audio/Controller do it themselves). Frames
   `set6/attempt1/gameplay_opts_086.png` (panel, title, INVERTED/ON/ON, BACK), reboot2 120-129.
6. The scenarios check VALUES: ApplyStore's log line ends `match=true|false` (live options ==
   the store over the bits it knows: 8-13, plus 0-7 when bit 15 says volumes are known);
   `femenu_settings.json` expects `invY=1` in the FE capture, `apply why=1 ... match=true`
   and `invY=1 autoCenter=0` in the PAUSEMENU capture; `femenu_settings_reboot.json` expects
   the seed `invY=1 autoCenter=0` from SR_GetSettings, `apply why=0 ... match=true` and the
   same values in its capture. Mutation roll `mut_noop_apply` (ApplyStore jumps over its
   setters): `apply why=1 store=30720 now=12288 match=false` -> scenario FAILURE at new-game.
7. Process: two `cmake --build` runs linked `build/SupermanReturns.exe` while another
   session's roll `archive/iter463_xlat_live3` ran (10:52-11:14). Its logs and frames stand;
   host-address / pdb triage of it must use the exe hash in its provenance, not `build/`
   (caveat added to RENDERER_PM4_HYBRID §11). Every roll of this fix waited for the slot
   (`tasklist` before each launch) and linking happened only with no game running.

| Roll (`archive/v1_frontend/settings_fix/`) | Result |
|---|---|
| set6 | FE part all values right (`apply why=1 store=30720 now=14336 match=true`); in game the pause never opened (a tutorial prompt took the START; frames 146-150), so the PAUSEMENU expect failed - timing, not the feature |
| set7 | SUCCESS: FE capture `invY=1`, `apply why=1 ... match=true`, in game `SR_SetSettings 'ScrGameplayOpt PAUSEMENU 10240' -> stored (... invY=1 autoCenter=0 ...)`; every capture answered by `reply FlowMgr.SR_OnHostSettings(...) delivered` |
| reboot2 (seed = set7's content) | SUCCESS: `SR_GetSettings -> 26624 (... invY=1 autoCenter=0 ...)`, `apply why=0 store=26624 now=10240 match=true`, the capture keeps both, EXIT -> `process exited (code 0)`; frames 120-129 GAMEPLAY OPTIONS in its panel: INVERTED / OFF / ON |
| mut_noop_apply | FAILURE as intended (`match=false`) |
| quit_off1 (`SR_MODS=0`) | SUCCESS: QUIT GAME -> title -> START -> `[XAM_PROFILE] #3` -> LOAD A GAME? |
| quit_on1, quit_on2, quit_diag1, quit_v1_nolc, quit_v3_iaonly, quit_v5_noreg1, quit_v6_skipped | the mod-on reload soft-lock and its bisection (item 4) |
| quit_diagonly1 (SR_Log diagnostics only) | SUCCESS: an edited SCRPRESSSTART with small edits reloads fine |

**Item 4, QUIT GAME soft-lock: ROOT CAUSE FOUND, fixed offline, LIVE-VERIFIED
(2026-09-30; rolls under "Live verification" at the end of this item).** Full contract:
ENGINE_MAP "Apt writer invariants" (unload-walk order).

- **Mechanism (IDA, re-read by an investigation and a skeptic).** On unload the Apt runtime
  does not restore the `.const` indices it replaced at load: `AptActionBytecodeRelocate_sub_8269F3A0`
  (a3 == 0, 0x8269F6BC) writes `slot = counter++` into every ConstantPool (0x88) / Push (0x96)
  slot, in the walk order of `AptMovieUnrelocate_sub_826A3180` -> `sub_826A0758` (chars in
  index order, types 5/9, imports skipped -> frames -> items: Action +4, PlaceObject clip
  actions +8, InitAction +8 -> a linear bytecode scan to the first End). The QUIT GAME reload
  reuses the resident payload (only the ui.AST header and TOC are read again), so it
  relocates those counters. Retail stores slot k = item k with one item per slot (820/820
  movies), so the rewrite is an identity there.
- **What the mod did wrong.** The toolchain appended new items at the end (`aptasm`,
  `patch_movie._grow_pool`, `apt_graph`), and `Graph.gc()` kept that order. The 5 patched
  movies had 439 / 225 / 183 / 130 / 199 slots out of walk order (SCRPRESSSTART / SCRSAVELOAD /
  SCRGAMEPLAYOPT / SCRAUDIOOPT / SCRCONTROLLEROPT). On the reload, InitAction 59's
  `Object.registerClass('ScrPressStart', ...)` pool resolved to `[reg2, reg2, reg2]`: the class
  was never bound, so onLoad and InitGui never ran. This fits every bisection roll: any variant
  that adds items fails, even `quit_v6_skipped` (the unload walk is a static scan, not
  execution); `quit_diagonly1` adds no items and passes. The set1 / set2 / back1 SwitchScreen
  failure is the same cause (the same cached blob was reused).
- **Fix (toolchain, no engine change, no forcing).** `apt_graph.Graph.renumber_walk_order()`
  runs after `gc()` and before `emit()` for every patched movie (`patch_movie.Patcher.finish`):
  slot k gets index k, item k is the constant it named (an index array used twice is copied, a
  constant used by two slots becomes two items). `tools/apt/apt_unload.py` is the independent
  byte-level checker: `patch_movie.verify()` (check 5) and `build_custom_frontend.py` (every
  output movie, `--check` included) fail unless index == counter for every slot, slots ==
  items, every slot is walked, every slot resolves the constant it named before the
  renumbering, and a reload after a simulated unload resolves the same constants.
- **Second defect, signature A (medium confidence).** `quit_on1` / `quit_v3_iaonly` showed NO
  ScrPressStart load request after YES (aptReq +2, no n=41 package, no 0x36C parse) while
  function objects of the unloaded movie survived in `_global.SRFrontEnd`; `quit_on2` (delete
  the object in ExitScreen) removed signature A and failed only on the defect above. This build does NOT restore
  quit_on2 itself: it applies the investigation's alternative, never rolled live, which keeps
  the object and its plain state (the live plan separates the two: aptReq +3 and the n=41
  package after YES): `exitscreen_head.aptasm` now calls `Uninstall()` and then deletes
  the 16 METHODS of `_global.SRFrontEnd` (plain state - fetched, inSettings, pendingId,
  lastAnswer - stays; the class InitAction binds the methods again on every load). Deleted
  in ExitScreen, not inside Uninstall, so a running function never deletes itself. The build
  checks that the deleted list equals the methods `srfrontend.aptasm` binds (lifetime rule;
  a mutation dropping one delete fails `--check`). The same rule for ScrSaveLoad:
  `_global.SR_ApplyStore` is deleted right after the in-game load calls it. Nothing calls an
  SRFrontEnd method after ExitScreen: ScrPressStart.LoadComplete (ApplyStore) runs before it
  (roll final/on1: `apply why=1 ... match=true` then `uninstall`); the options screens read
  only `inSettings`. Not verified for LOAD GAME with a real save (no roll has one).
- **Offline proofs (this commit).** `apt_unload.py`: retail 820/820, rebuilt mod 820/820, the
  pre-fix mod build 815/820 (exactly the 5 patched movies). The investigation's
  `archive/v1_invest/quit_apt_loader/sim_unload_renumber.py`: index != counter 0 in all 5; the
  skeptic's `archive/v1_invest/quit_skeptic/all_movies.py`: mod 820/820. Pre-fix vs fixed build
  for the three movies whose sources did not change: every slot resolves the same constant, 0
  .apt bytes changed outside slot fields, reload-after-unload resolves the original. `rt_patch.py`
  32/32 (section Q: retail, the real build, and mutations - no renumbering 5/5 rejected with
  the `[reg2]x3` reload reproduced, `verify_container` rejecting the un-renumbered container,
  swapped items, swapped slots, a dropped item, the copy/duplicate path); `rt_graph.py`
  (renumbering is a no-op on 820/820 retail movies), `rt_test.py`, `rt_asm.py` all pass. Mod
  rebuilt: ui.AST 89614348 B, sha256 78dba966...; `mods/enabled.txt` untouched (custom_frontend
  stays commented out).
- **Live verification (2026-09-30, DONE; natural, no forcing gate; the mod's own AS runs).**
  Exe from f7877f2 (the `[APT_SCREEN]` marker), mod ui.AST 89614348 B sha256 78dba966...
  (`build_custom_frontend.py --check` clean). The scenarios now wait on the screens' own
  requests instead of timing: the pause step pulses START every 10 s (up to 18) until a fresh
  `[APT_SCREEN] path='screens/misc/ScrPauseMenu'`, QUIT GAME's A until a fresh
  `ScrGenPop`, YES until a fresh `screens/bootflow/ScrPressStart` (sr_drive `expectFresh`), and
  every key step uses `holdUntilConfirmed` (roll `femenu_quit1` lost the title START pulse to
  a 1.5 s game-thread stall, `[CS_WAIT] waited=1501ms`: a 250 ms press was never sampled; a
  harness miss, not a game result). Artifacts `archive/v1_frontend/quit_verify/<roll>/attempt1/`
  (run.log, steps.log, frames/, sheets/ = contact sheets of every frame, all viewed).

  | Roll | Gates | Result | Evidence |
  |---|---|---|---|
  | femenu_quit2 | SR_MODS=custom_frontend | SUCCESS, exit 0 | ScrPauseMenu seen=1 req=35 (2nd START; the 1st reached the guest, the opening ignored it) -> ScrGenPop seen=2 req=36 -> YES -> ScrPressStart seen=2 req=37, `PKG#37 n=41`, `PARSE#36 b14=0000036C`, `SR_Log 'install' saveFlow+16=1012091577`, HintA/HintB (aptReq +3) -> START -> `claim LoadCreate` -> custom MAIN MENU -> EXIT -> `menu button=3` -> QUIT TO DESKTOP? YES -> `SR_Quit`, code 0 |
  | femenu_quit3 | SR_MODS=custom_frontend | SUCCESS, exit 0 | the same sequence: req=35/36/37, `PKG#37 n=41`, `PARSE#36 b14=0000036C`, `install`, code 0 (this roll played the scripted Clark Kent opening first) |
  | natural_quit1 | SR_MODS=0 | SUCCESS | ScrPauseMenu req=33 -> ScrGenPop req=34 -> ScrPressStart req=35, `PKG#35 n=41`, `PARSE#34 b14=0000036C` -> START -> `[XAM_PROFILE] #3` -> the stock LOAD A GAME? popup (frames 133-138) |
  | femenu_settings_audio2_2 | SR_MODS=custom_frontend | SUCCESS | AUDIO OPTIONS opened (`settings button=2` #13), backed out (`ScrAudioOpt FE 12288 -> stored` #16), opened again (#23) and backed out again (#26, stored); GAMEPLAY OPTIONS invY=1 stored; NEW GAME `apply why=1 ... match=true`, `uninstall`; pause ScrPauseMenu req=50; in-game GAMEPLAY OPTIONS shows INVERTED |
  | femenu_new1, femenu_load_exit1 | SR_MODS=custom_frontend | SUCCESS (load_exit: exit 0) | NEW GAME -> intro -> HUD; LOAD GAME -> three EMPTY SLOT -> B -> menu -> EXIT -> YES |
  | femenu_quit1 | SR_MODS=custom_frontend | harness miss | title START not sampled (above); no quit reached |
  | femenu_settings_audio2_1 | SR_MODS=custom_frontend | boot freeze | run.log stops 3 s after launch (last line `[EXTHREAD_PATH] F2C8_LONG_post`, no `[THREAD_SAMPLER]` afterwards), 0 frames, before any input; not attributable to the mod from this evidence |

  Frames, femenu_quit2/3: the pause menu with QUIT GAME highlighted, the game's WARNING
  popup with YES highlighted, LOADING, the title (no slot boxes, no stray DisplayWell), the
  custom MAIN MENU over the title city, the cursor moving to EXIT, QUIT TO DESKTOP? YES.
  Signature A did not appear: every quit roll shows the ScrPressStart request about 0.8 s
  after YES, so ExitScreen deleting the 16 SRFrontEnd methods (keeping the plain state) works
  live. Still not verified: LOAD with a real save; the quit path after a LOAD (not rolled).
- **Default-enable recommendation (2026-09-30): YES, switch it on for players** by
  uncommenting `custom_frontend` in `mods/enabled.txt` (`build_custom_frontend.py --enable`).
  Not done here; the file is unchanged. Basis: the last blocker (QUIT GAME) is gone, and NEW,
  LOAD (no saves), SETTINGS (audio twice, gameplay, carried into the game), QUIT -> front end
  and EXIT -> code 0 all pass live (the rolls above plus 5.5 on1/off1). Open issues that do
  not block it: the volume sliders read 0 (no mixer; the same with the mod off); LOAD with a
  real save and QUIT after a LOAD not rolled; the SETTINGS panel has no BACK hint; the labels
  are English only (step 9). Each path rests on one or two rolls. Roll LOAD with a real save
  before or right after switching it on.


### 5.5 Final verification (step 8, 2026-09-30)

Both rolls use the **player's switch** (`mods/enabled.txt`), not `SR_MODS=<name>`: rolls set
`SR_INSTANCE_ID` and therefore ignore that file, so the explicit opt-in
`SR_MODS=@enabled.txt` (kernel/mod_overlay.cpp, difftest M6h/M6i) makes a roll read it
exactly as a direct launch does. Exe built from this commit's tree; the mod built with
`python tools/apt/build_custom_frontend.py --enable` (ui.AST 89613884 B, sha256 901c3c82...,
5 movies patched, 823/823 other entries byte-identical). Artifacts:
`archive/v1_frontend/final/<roll>/attempt1/` (run.log, steps.log, frames/, sheets/, png/).

```
python tools/apt/build_custom_frontend.py --enable             # mods/enabled.txt: custom_frontend
powershell -File tools/sr_drive.ps1 -Scenario tools/scenarios/femenu_settings.json -Out archive/v1_frontend/final/on1 -TimeoutSeconds 420 -Gates "SR_MODS=@enabled.txt"
# switch off: "# custom_frontend" in mods/enabled.txt
powershell -File tools/sr_drive.ps1 -Scenario tools/scenarios/natural_new.json -Out archive/v1_frontend/final/off1 -TimeoutSeconds 360 -Gates "SR_MODS=@enabled.txt,SR_PRESENT_DUMP_SPARSE_EVERY=20"
```

| Roll | Tag | Result | Log | Frames (all viewed, contact sheets `sheets/`) |
|---|---|---|---|---|
| on1 (`femenu_settings.json`) | mod:custom_frontend | SUCCESS, every step passed | `[MOD] enabled ... custom_frontend [source: mods/enabled.txt (SR_MODS=@enabled.txt)]`; claim; SETTINGS -> AUDIO -> back (`SR_SetSettings 'ScrAudioOpt FE ..' -> stored`); SETTINGS -> GAMEPLAY -> RIGHT -> Esc (`... invY=1`); NEW GAME -> `apply why=1 ... match=true` -> `uninstall`; in game pause -> GAMEPLAY OPTIONS -> auto-center off -> `SR_SetSettings 'ScrGameplayOpt PAUSEMENU 10240' -> stored (... invY=1 autoCenter=0 ...)` | 158 frames: 000-039 intro movies (EA HD, EA, DC, WB, legal, starfield); 043-051 loading; 052-054 PRESS START; 055-059 MAIN MENU panel; 060-063 SETTINGS panel; 064-072 AUDIO OPTIONS; 073-077 menu again; 078-079 SETTINGS; 080-085 GAMEPLAY OPTIONS (084-085: Y INVERSION INVERTED); 086-090 menu, NEW GAME; 091-131 opening cinematic (space, Earth, Metropolis, Clark, the shirt reveal); 133 loading; 134-142 gameplay with the HUD; 143-157 PAUSE MENU and its GAMEPLAY OPTIONS reading INVERTED (the menu change carried into the game) |
| off1 (`natural_new.json`, `custom_frontend` commented out) | natural | SUCCESS | 0 `[MOD]`, 0 `[SR_FSCMD]` lines; `APT_REQ #17 main_hud` | 102 frames: the same intro movies, loading and PRESS START; 056-058 the natural LOAD A GAME? popup; the same cinematic; 101 loading (the scenario ends 45 s after START NEW GAME, so no HUD frame; `main_hud` was requested) |

The first 16 APT_REQ entries (main ... ScrGenPop) are identical in on1 and off1 (MD5
`4b1b047c043fc6581dab25a7b91502ad`, the same as e1_off and alias_probe); after that on1 loads
the settings screens. Best frames: panel
`archive/v1_frontend/final/on1/attempt1/png/present_cl_056_p001120_1280x720.png`, SETTINGS
`.../png/present_cl_060_p001200_1280x720.png`, GAMEPLAY OPTIONS
`.../png/present_cl_085_p001700_1280x720.png`.

**Astra (blind, one call; `archive/v1_frontend/final/astra.txt`)** was given the panel, the
SETTINGS panel, GAMEPLAY OPTIONS, our natural LOAD A GAME? popup and the Xenia golden
press-start frame: "The claim 'the menu is recreated to suit the existing UI' holds
visually" (same chrome, fill, font, selection strip, A prompt). Its one inconsistency: **the
SETTINGS panel shows no BACK/B hint** although B works there (follow-up: show the game's
BACK hint with that popup). Its other notes are on the game's own screens and backdrop
(GAMEPLAY OPTIONS transparency and footer, city texture streaks), not the mod. No Xenia
frame of the mod exists (retail has no such menu); the chrome is judged against the game's
own popup.

Still broken / not verified (unchanged by step 8; the QUIT GAME soft-lock listed here
first was fixed and live-verified later the same day, 5.4 fix 4): no audio mixer, so the
volume sliders read 0; LOAD with a real save not run;
the controller scheme never changed in a roll; English-only labels (step 9).

### 5.6 Autosave without asking (iter-464, fe lane; tag PORT FEATURE)

User decision 2026-10-01: "I think it's game design. It should go in the settings but default
on." The save prompt the user saw is the game's own (the save-prompt investigation,
`docs/SAVES.md`): MSTobj03's end raises **SAVE GAME BEFORE CONTINUING?**
(`PromptUserSaveGame_ShowPopupIfSignedIn_sub_822B5070` -> popup type 16 PromptInGameSave). Retail
has **no disk autosave**: the Lua `AutoSaveGame()` the chapter advance calls only captures the
state in memory. So "autosave without asking" is a port feature built as content (user decision
2026-10-02: the custom front end and this setting are PORT FEATURES, always on for players;
the modship lane ships them as baked-in clean patches generated from the recipes in
`tools/apt/mods/custom_frontend/`), and the kernel never suppresses
the prompt.

**The settings tree (custom front end + pause menu):**

```
MAIN MENU (custom front end, 5.3; five rows = ScrGenPop's MAXNUMBUTTONS)
├─ NEW GAME · LOAD GAME
├─ OPTIONS ─► the OPTIONS screen (lane E L-b, 5.12; the SETTINGS panel below is no longer reached)
│  ├─ GAME      the game's own options (invert look, auto-centre, vibration, control scheme, music / SFX)
│  │            + AUTOSAVE WITHOUT ASKING (+ DEV TOOLS in a dev build)
│  ├─ CONTROLS  + VIEW PAD LAYOUT ─► the game's own CONTROLLER OPTIONS screen (its pad diagrams)
│  └─ DISPLAY · GRAPHICS · WORLD · AUDIO   (the PC options, docs/GRAPHICS_SETTINGS.md)
├─ MODS ─► the MODS pages (5.8; iter-464 modmenu)
└─ EXIT

(before L-b: SETTINGS = GAMEPLAY / CONTROLLER / AUDIO OPTIONS, DISPLAY OPTIONS, MORE OPTIONS [AUTOSAVE, DEV TOOLS])

PAUSE MENU (the game's rows + one; the 9-row layout is lane E L-c)
├─ RESUME ... (the game's rows; its option screens write the same store, 5.4)
├─ MORE OPTIONS  (new 10th row, above QUIT GAME)
│  ├─ AUTOSAVE WITHOUT ASKING: ON|OFF   (the same host option)
│  ├─ DISPLAY OPTIONS ─► the OPTIONS screen (5.12; with CHEATS, in game only)
│  ├─ MODS ─► the MODS pages (5.8)
│  └─ DEV TOOLS ─► the Dev Tools pages (5.7)
└─ QUIT GAME

DISPLAY OPTIONS (5.10; host-built, kernel/display_menu.cpp; LEFT/RIGHT or A change a row)
├─ RESOLUTION: AUTO | 1X | 2X | 3X | 4X        (applies on restart: A offers RESTART NOW)
├─ DISPLAY: WINDOWED | BORDERLESS FULLSCREEN | EXCLUSIVE FULLSCREEN   (live)
├─ FRAME RATE: 30 (CONSOLE) | 60 | 120 | DISPLAY (<refresh> HZ) | UNLIMITED   (live)
├─ VSYNC: ON | OFF                             (live)
└─ WINDOW SIZE (windowed) | OUTPUT (exclusive) | OUTPUT: DESKTOP (borderless)   (live)

MODS pages (5.8; host-built, kernel/mod_menu.cpp)
├─ the list: <NAME>: ON|OFF [- CONFLICT | - FAILED]  (3 per page; LEFT/RIGHT on/off, A details)
│  ├─ MORE MODS
│  └─ HOW TO INSTALL MODS | RESTART TO APPLY ─► RESTART NOW / NOT NOW
├─ a mod: ENABLED · LOAD ORDER n OF m · WHAT IT CHANGES · CONFLICTS · OPTIONS (extension point)
├─ no mod installed: short help + OPEN MODS FOLDER / RESTART TO APPLY / HOW TO INSTALL MODS
└─ (no COSTUMES entry: what Superman wears is chosen in the game's own Fortress of Solitude >
   COSTUMES screen, 5.9; the MODS pages only install packs)
```

**The setting.** Host option `autosave_without_asking` in `<save root>/settings/mod_options.ini`
(next to `game_options.ini`, section 0; written atomically; default 1 when absent).
`fscommand("SR_GetModOptions")` / `("SR_SetModOption", "autosave_without_asking 0|1")` read and
write it (docs/MODS.md); `_global.SRModOptions` is the AS copy (bit 0 autosave, bit 1 notice
acknowledged, bit 8 a dev tool on, bit 9 the option folder was mounted at boot, bit 10 a level
skip armed or running).

**ON = a real autosave, through the game's own save code** (two parts):

1. **When:** at every chapter advance. `mods/custom_frontend/@autosave_without_asking/DATA/baseworl.AST`
   is an *option folder* (docs/MODS.md): mounted above the mod only when the option is 1 at boot.
   Its Lua differs from the disc's in one line: BaseWorldFlow state 105's chapter-advance block
   calls `AutoSaveGame()` and then, instead of the debug print
   `LuaDebugPrint("[base] you advanced to chapter " .. currentHeroPointChapter + 1)`, the game's
   own `PromptUserSaveGame()` (same length, the rest of the file byte-identical;
   `build_custom_frontend.py` refuses to build unless the print occurs exactly once, right after
   `AutoSaveGame()`). MSTobj03's prompt is the game's own and stays.
2. **Answering:** `SRAutoSave` (an InitAction in MAIN_HUD, `srautosave.aptasm`) wraps
   `FlowMgr.CppShowPopup` while the HUD lives (owner protocol shared with the front end:
   `FlowMgr.SR_OrigCppShowPopup` / `SR_PopupHookOwner`). For the game's own save popups it
   answers with the game's own button, deferred one interval as the front end does:
   - **SAVE GAME BEFORE CONTINUING?** (`PromptInGameSave_Title`) -> **Save Game** (button 0).
     Before answering, the hook prepares the HUD's picker (`PrepSlots`, iter-465): instance
     members over the SaveLoadSlots prototype on `Main_Hud.mSlots` - `Show` -> the game's Show,
     then hidden; `FillSlotInfo` -> `FillHook`. The game then opens its picker
     (`Main_Hud.ShowSaveSlots`, from C++) and it stays invisible;
   - the picker's own `FillSlotInfo` -> the slot: the **current slot** (the one loaded from or
     last saved to, tracked by the host: `fscommand("SR_SaveEvent")` at NEW GAME, LOAD and
     SAVE, which reads `saveFlow(*0x8296EE8C)+0x54`, the slot SaveTask/LoadTask use), else the
     **first empty slot**; then the picker's own CROSS -> `SaveTask(slot)`;
   - an in-use slot -> the game's **overwrite** question (`SaveLoad_SaveMessage_Title`) ->
     **Yes** (0); the host latches the slot here, because a confirmed overwrite leaves +0x54 = 3;
   - **SAVE SUCCESSFUL** (`SaveSuccessful_Title`, k 3) -> the tracker learns the slot, the
     autosave ends, **OK** -> `SaveComplete(true)`.
   - **CancelInGameSave** (k 4) while an autosave runs (a failed save: `SaveComplete(false)` ->
     `HudAdapter.CanceledSaveGame`) -> **Continue** (button 0).
   - No current slot and no empty slot: the autosave cancels through the HUD's own B path
     (`HideSaveSlots` + `HudAdapter.CanceledSaveGame`) and answers the game's CancelInGameSave
     **Continue**: it never overwrites a slot it cannot attribute to this playthrough, and never
     leaves a picker up (at a chapter advance it would sit under the loading screen, out of
     reach - the iter-465 hang). The player can still save from the pause menu.
   - While a level skip is armed or running (bit 10) the prompt is answered **Continue Without
     Saving** (button 1): a skip never writes a slot by itself.

**Live status (iter-465 save lane, background rolls `archive/v1_save/rolls/`).** The reported
hang ("after Save Game the slot picker is called but never completes; LOADING forever",
fortress lane, `archive/v1_fortress/rolls/ng_hold2`) was this feature's flow, not the platform:
the chapter prompt is raised under the arena->Metropolis loading screen, and the old hidden
picker was installed from a prepend on `Main_Hud.ShowSaveSlots`, a function the C++ calls; a
call from such a function into `SRAutoSave` never ran (rolls p1, p2, with logs), so the game's
own picker waited, invisible and unreachable. The hook now prepares the picker itself.
- p3 (`tools/scenarios/save_autosave_mstobj03.json`): MSTobj03 prompt -> slot 0 written ->
  SAVE SUCCESSFUL -> tracker slot 0 -> the game goes on. SUCCESS.
- p4 (`save_autosave_chapter.json`): the chapter advance after WARobj02 writes slot 0 (chapter 2,
  objectives 0-2, 5, 6 won) and the load carries on; ~90 s later the separate arena-return
  crash (`[PHYS_CAP]` refuses the 234 MB block, physmem lane) ends the process.
- l1 (`save_load_portfeat.json`, seeded with p4's save): MAIN MENU > LOAD GAME > slot 0 ->
  tracker slot 0 -> `ScrMainMenu`, the post-first-hour METROPOLIS / FORTRESS carousel
  (`rolls/l1/sheet_load.png`). This is the save the Fortress needs.
- Prompt count: per playthrough the player is asked at most once, at MSTobj03 - retail
  behaviour - and with the setting ON not even that (the chapter prompts are answered too).
- Open: the HUD's ExitScreen teardown calls `SRAutoSave.Teardown()` from a function the game may
  call from C++; no roll has quit from the HUD yet, so whether the hook is restored there is
  unverified (the front end re-installs over a stale hook either way).

**OFF = the console's behaviour.** The option folder is not mounted (the disc's `baseworl.AST`,
so no chapter prompt) and the hook passes every popup to the game: MSTobj03 shows SAVE GAME
BEFORE CONTINUING? and waits for the player. The answer follows the setting at once; the
chapter-advance prompt follows the setting **at launch** (the folder is mounted at boot): ON ->
OFF mid-session shows the chapter prompt to the player until the next launch, OFF -> ON adds no
chapter prompt until then. **Port features off** (automated natural rolls: `SR_INSTANCE_ID`
-> `SR_PORT_FEATURES=0` once modship merges; today `SR_MODS=0`) run none of this: natural rolls
(`docs/NATURAL_BASELINE.md`) have zero `[MOD]` / `[SR_FSCMD]` lines. **For the packaging:** the
chapter prompt is the option folder `@autosave_without_asking/DATA/baseworl.AST`, which must
stay CONDITIONAL on the host option at boot (kernel/mod_overlay.cpp option folders); the
answering half lives in ui.AST and reads the option at run time.

**Profile name (a platform fix found on the way, always on).** A save stores the profile name
(`sub_82164F08` copies up to 16 bytes from `XamUserGetName`), and `LoadTask` (`sub_822B4ED8`)
compares it; a save made under another name is refused. `XamUserGetName` now follows Xenia
(`xam_user.cc`): user 0 -> the gamertag from `<save root>/settings/profile.ini` (`gamertag=`,
1..15 printable ASCII, default `Player`), other users -> `X_ERROR_NO_SUCH_USER` with byte 0
cleared (`SR_XAM_USERNAME=0` = the old stub). To load a save made in Xenia under another
gamertag, put that gamertag in `profile.ini`.

### 5.7 Dev Tools pages and MORE OPTIONS (iter-464, fe lane; tag PORT FEATURE / DEV-TOOL)

> **Superseded in game by 5.14 (lane E L-c):** the pause menu has no MORE OPTIONS row any more; its rows are in OPTIONS
> (AUTOSAVE and, in a dev build, DEV TOOLS on the GAME tab; DISPLAY / GRAPHICS / WORLD; CHEATS) and MODS is on the MAIN
> MENU only. The Dev Tools pages themselves are unchanged.

User decision 2026-10-01: a Dev Tools entry in SETTINGS and in the pause menu, built "as real
game style menu pages", never on by default. Design and the DT-2 extension point:
`docs/DEVTOOLS_DESIGN.md` 13; player guide: `docs/DEVTOOLS.md` 0b.

- **The pages are ScrGenPop**, the game's popup (title, body, up to 5 buttons, the A/B hints),
  shown with `FlowMgr.ShowGenPop` as the front end's own panels are. `devtools_ui.aptasm`
  (`SRDevTools`, an InitAction in SCRPRESSSTART and SCRPAUSEMENU) is a renderer only: every
  A / B / LEFT / RIGHT is one `fscommand("SR_DevMenu", ...)`, and the host's replies
  (`SR_DTBegin` / `SR_DTItem` / `SR_DTEnd`) say what to show. A toggle or slider changed with
  LEFT/RIGHT is relabelled in place (`PopupBtn<i>.SetText`); a page change re-shows the popup.
- **Front end:** SETTINGS gains rows 4 (AUTOSAVE WITHOUT ASKING) and 5 (DEV TOOLS) (iter-465:
  both moved into SETTINGS > MORE OPTIONS, 5.10); B on a Dev
  Tools page goes back one page, and from the top page back to SETTINGS (focus on DEV TOOLS).
- **Pause menu:** `40_pause.patch` clones the 8th pause button (`patch_movie.py clone-place`)
  as `btn10` "MORE OPTIONS" at y 382, moves QUIT GAME (`btn9`) to y 440 and renumbers the focus
  order (`m_tabIndex` 8, 9), so UP from RESUME reaches QUIT GAME then MORE OPTIONS. A on it
  opens a ScrGenPop panel (`SRPause`): AUTOSAVE WITHOUT ASKING, DEV TOOLS; B returns to the
  pause menu with its hints restored.
- **Opt-in:** the pages are part of the port's front end, but every tool behind them is a
  devtool-class tool, off at every launch, switched on only by the player (the notice, the
  tagging of DEVTOOLS_DESIGN 3).
- **Lifetime** (the rule of 5.4 fix 4: no function object of a movie outlives it): each new
  object has a `Teardown()` that the movie's `ExitScreen` calls first; `build_custom_frontend.py
  --check` verifies every added method is deleted on the way out (SRFrontEnd 22, SRDevTools 13
  x2, SRPause 14, SRAutoSave 19).

### 5.8 The MODS pages (iter-464, lane modmenu; tag PORT FEATURE)

USER 2026-10-02: "we're going to need a mod menu". Player how-to, page list, data model and
the agreement with the modcore lane: `docs/MODS.md` "The MODS menu". What changed in the
front end:

- **MAIN MENU** has five rows: NEW GAME / LOAD GAME / SETTINGS / **MODS** / EXIT
  (`srfrontend.aptasm` `ShowMenu`, `OnMenu` button 3 -> `OpenMods` = `SRDevTools.Open("mods",
  "FE", fe, "ShowMenu", 3)`; EXIT is button 4 and its confirm returns to row 4). B on the first
  MODS page returns to the menu with MODS focused. Scenarios that pressed DOWN x3 for EXIT press
  it x4 now (femenu_load_exit, femenu_quit, femenu_settings_reboot).
- **Pause > MORE OPTIONS** shows AUTOSAVE WITHOUT ASKING / **MODS** / DEV TOOLS
  (`srpause.aptasm` `OpenMods` = `Open("mods", "IG", sp, "ShowPanel", 1)`; DEV TOOLS returns to
  row 2). In game the restart page warns that progress since the last save is lost.
- **The renderer is shared**: the Dev Tools renderer (`devtools_ui.aptasm`, still
  `_global.SRDevTools`) renders every host-built page; its command is now `SR_Menu`
  (`SR_DevMenu` accepted), its log lines `menu show <page>` / `menu <page> button=<b>` /
  `menu close`. The host routes pages starting `mod` / `costume` to the MODS menu and the rest
  to the Dev Tools menu (`kernel/host_pages.cpp`).
- **Lifetime**: `OpenMods` is deleted on unload with the other methods (SRFrontEnd 23 methods,
  SRPause 15; `build_custom_frontend.py --check`).
- **The port feature's manifest**: `tools/apt/mods/custom_frontend/mod.json` (`"port_feature":
  true`) is copied into the built mod folder, so a custom front end that still lives in `mods/`
  (before the modship packaging) is never listed as a player mod.

### 5.9 Fortress COSTUMES: mod suits in the game's own screen (iter-465, fortress lane; PORT FEATURE)

The suit picker is the game's OWN Fortress of Solitude > COSTUMES screen
(`screens/misc/ScrCostumes`, ui.AST SCRCOSTUMES). The MODS pages (5.8) choose which packs are
installed; they never choose what Superman wears. Player reference: docs/MODS.md "Fortress
costumes"; retail ground truth and the switch contract: docs/ENGINE_MAP.md "Fortress COSTUMES
and the costume switch".

- **Patch** (`tools/apt/mods/custom_frontend/50_costumes.patch` -> `SCRCOSTUMES.srpatch`):
  `srcostumes.aptasm` builds `SRCostumes` (owned by the movie, deleted on ExitScreen) whose
  `Append(retail)` asks the host for the included mod suits (`SR_ModCostumes`, synchronous
  FlowMgr callbacks) and returns `retail.concat(additions)`; the retail ThumbViewer pages rows
  of three itself. With additions present, retail record 0 ("Superman Returns") is made
  wearable (`Locked = false`) so a player can always return to the default suit.
  `costumes_input.aptasm` (prepended to `HandleInput`): X on an appended record calls
  `SRCostumes.Wear(SRId)` -> `SR_CostumeWear`; X on an unlocked retail record calls
  `SRCostumes.Retail(index)` -> `SR_CostumeWorn <index>`, and continues into the retail
  `SetActiveCostume` only when the host answers that the switch can happen now.
- **Host** (`kernel/mod_bridge.cpp`): `SR_CostumeWear` writes `SupermenActorWorldFlow`
  `SetCostumeNum` through the game's DAS setter (as retail `SetActiveCostume` does) and raises
  the retail "NEW COSTUME SELECTED!" popup (`sub_822B18E0`, the retail strings) when the suit is
  of this session's animation family; otherwise it only saves the choice and the description
  field says the suit is worn from the next start. Both commands save the worn suit
  (`<save root>/settings/costume.ini`, `worn=`).
- **Gate**: `SR_NATIVE_COSTUMES`; unset, the PORT-FEATURE switch (on for players, off for natural
  rolls). Without it the screen is retail.

### 5.10 DISPLAY OPTIONS (iter-465 polish lane; tag PORT FEATURE)

Owner report (docs/PLAYER_BUILD.md 7): the player had to edit `display.ini` for resolution,
display mode and frame rate. Now they are a page of the game's own popup (ScrGenPop), built
by the host like the Dev Tools and MODS pages (`kernel/display_menu.cpp`, routed by page id in
`kernel/host_pages.cpp`, rendered by `devtools_ui.aptasm`):

- **Where:** MAIN MENU > SETTINGS > DISPLAY OPTIONS (SETTINGS row 3) and in game PAUSE > MORE
  OPTIONS > DISPLAY OPTIONS (panel row 1). SETTINGS has five rows (ScrGenPop's maximum), so
  its AUTOSAVE / DEV TOOLS rows moved into a MORE OPTIONS panel - the same two rows the pause
  menu's MORE OPTIONS has (`srfrontend.aptasm` ShowMore / OnMore; OpenDevTools now returns to
  MORE OPTIONS row 1). Pause MORE OPTIONS = AUTOSAVE / DISPLAY OPTIONS / MODS / DEV TOOLS.
- **Rows** (LEFT / RIGHT change a row in place, A steps it forward):
  RESOLUTION (AUTO, 1X-4X: the internal draw scale, frozen at start as in Xenia - a change
  shows `- RESTART`, and A on it opens RESTART NOW / NOT NOW, the MODS menu's relaunch),
  DISPLAY (windowed / borderless / exclusive, applied at once on the window's thread:
  `resscale::requestLiveDisplay` -> `PlatformSDL::applyLiveDisplay`), FRAME RATE (30 / 60 /
  120 / the display refresh / unlimited: `highfps::applyLive` - the host present sync, the
  emulated vblank rate and the render loop rate through the game's own
  `GameKernelMgr_SetTickRate_sub_82720418`, docs/HIGH_FRAMERATE.md 9), VSYNC (on / off, live),
  WINDOW SIZE (windowed; 1280x720 .. 3840x2160, the sizes that fit the desktop) or OUTPUT
  (exclusive; DESKTOP or a preset).
- **One store, universal:** `<save root>/settings/display.ini` (beside `game_options.ini` and
  `mod_options.ini`); the page reads it every time it is built, so the front end and the pause
  menu always show the same values, and a hand edit (power users) is the same thing. Alt+Enter
  writes `display_mode` too.
- **Automated rolls** (SR_INSTANCE_ID) store a full-screen choice but keep the window
  windowed (the owner can see windows on this PC), as app/main.cpp does at launch.
- Scenario `tools/scenarios/polish_display_glyphs.json` (`tools/player/roll_polish.ps1`).

### 5.11 The useful pause map (iter-466, map lane; tag PORT FEATURE)

A separate port feature, `pause_map` (`data/port_features/pause_map`, recipe `tools/apt/mods/pause_map`),
built with the same Apt toolchain (`build_custom_frontend.py --mod pause_map`, `clone-place ... with-char=`,
`as2asm`) and the same FSCommand bridge (`SR_Map`). It patches SCRMAP only: the map screen marks the current
objective and the next story mission, races and Bizarro statues (unlocked / completed / best), kittens
(found / not), metro events, with a legend and per-type filters. Always on for players, no toggle; off in
automated rolls unless `SR_PORT_FEATURES=1`. Everything about it: docs/MAP.md; for mods: docs/MODS.md
"Map markers and maps for mods".

### 5.12 One OPTIONS screen (lane E L-b, RELEASE_PLAN step 5; STEP5_PLAN 10, the owner's q14d; tag PORT FEATURE)

The MAIN MENU's third row is **OPTIONS** and opens the OPTIONS screen at once (the PC SETTINGS host screen
renamed, docs/GRAPHICS_SETTINGS.md 1 / 1.1): tabs GAME / CONTROLS / DISPLAY / GRAPHICS / WORLD / AUDIO / CHEATS, GAME
first, CHEATS in game only. In game the pause menu's own OPTIONS row opens it (5.14, lane E L-c).

- **The GAME tab is the game's own options.** The opener sends the game's own getters first
  (`fscommand("SR_GameOptionsLive", "<FE|IG> " + <snapshot_expr>)`: `srfrontend.aptasm` OpenOptions,
  `srpause.aptasm` OpenDisplay), then the open request. A change goes back through the game's own setters: the host
  calls the opener's `SR_GameOptions(p, s)` on the Apt pump (`game_options_fn.aptasm`): `_global.SRSettings = p`,
  `this.ApplyStore(3)` (only where the live value differs; logged `apply why=3 ... match=`), then
  `_global.SRSettings` = the value APPLY stored (s), or back to what it was for a preview (s = -1). APPLY stores
  with the same function as an options screen's capture (`modSettingsSavePacked`), so NEW GAME and loads re-apply
  it (5.4). `kernel/game_options.hpp`; logged `[GAME_OPTIONS]`.
- **The open hands over; the close calls back.** `kernel/display_menu.cpp` answers the open with
  `SR_DTEnd(0, 0, 3)`: action 3 = handed to the overlay, so the Dev Tools renderer neither shows a page nor calls
  its caller back, and no popup of the opener stays up under the screen. Once the overlay is gone (closed, or an
  open request no frame picked up) the host calls the opener's `SR_OptionsClosed(what)`:
  0 = its menu again (`ShowMenu(2)`, OPTIONS focused / `ShowPanel(1)`, DISPLAY OPTIONS focused);
  1 = VIEW PAD LAYOUT: the game's own CONTROLLER OPTIONS screen - its pad diagram for the current scheme
  (`showConfigGraphic`; Y = AIR / GROUND CONTROLS) - the front end through `GoSettings(1)` (the overlay at depth 60,
  its back re-shows the menu, 5.2), the pause menu as its own row opens it (`PauseSelectedItem = "btn10"`,
  `FlowMgr.SwitchScreen(SCREEN.CONTROLLER_OPT)`; its own B returns to the pause menu);
  2 = the Dev Tools pages (a development build's DEV TOOLS row; back to the MAIN MENU / the MORE OPTIONS panel).
  With `SR_PC_SETTINGS=0` (the developer A/B) the Apt DISPLAY OPTIONS page opens instead and its B calls the caller
  back as before.
- **Not reached any more:** the front end's SETTINGS panel (`ShowSettings` / `OnSettings` / `ShowMore` / `OnMore` /
  `OpenDisplay` stay in `srfrontend.aptasm` until the MAIN MENU rework, lane E L-d, removes them).
- **Rolls:** `tools/scenarios/step5_options_game.json` (the GAME tab in the front end and in game, CHEATS, VIEW PAD
  LAYOUT from the pause menu); the `femenu_settings*` set (the GAME tab, CONTROLLER OPTIONS through VIEW PAD LAYOUT)
  and `femenu_devtools` (the DEV TOOLS row).

### 5.13 The ACHIEVEMENTS page (lane E L-a, RELEASE_PLAN step 5; STEP5_PLAN 9, the owner's q14 (b); tag PORT FEATURE)

This save's achievements over the paused game, in the pause panel's look (`gfx/achievements_ui.hpp`; the model
`kernel/achievements_page.hpp`, from the achievements core's read API, `kernel/achievements.hpp`, E14a):
the title ACHIEVEMENTS; `N OF 18 UNLOCKED · X / 1000 G · THIS SAVE (SLOT n)` (`NOT SAVED YET` before the first
SAVE or LOAD, `SLOT n: ITS RECORD COULD NOT BE READ` for a record the core ignored); six rows at a time in the table's
own order: the title's own icon (its XDBF image, `sr::ach::iconPng`, decoded on the overlay kit's image worker; grey
while locked), the name, the unlocked description or the game's own how-to text, the gamerscore, LOCKED / UNLOCKED.
A locked secret reads SECRET ACHIEVEMENT / "Hidden until it is unlocked." with a "?" for its icon. The range line
(`1-6 OF 18`) with two clickable arrows, and BACK, always there.

- **Open:** `fscommand("SR_Achievements", "[IG|FE]")` (`kernel/mod_bridge.cpp`; the pause menu's ACHIEVEMENTS row
  sends it once the 9-row pause menu lands, lane E L-c) -> the mod library's routing, `OverlayPage::Achievements`
  (the game reads a neutral pad while it is up; the release latch on close). Drawn through the OPTIONS screen's slot
  (`gfx/settings_ui.cpp` render hands its frame to the page), so the present path is unchanged. No reply, no
  callback: the pause menu under the page stays as it was, ACHIEVEMENTS focused.
- **Input:** UP / DOWN move the focus (the list follows), LB / RB and PgUp / PgDn a page, Home / End; the wheel and
  the arrows scroll; the mouse focuses the row it points at; B / Esc / a right click / a click on BACK close it.
  No toast (the console's unlock pop-up was the system's, not the game's).
- **Log:** `[ACH_PAGE] shown (IG, WxH): 18 rows, <header>`, `[ACH_PAGE] view 1-6 OF 18, focus 2 <NAME>`,
  `[ACH_PAGE] icons: 18 of 18 decoded from the title's images, ...` (the icons' live proof), `[ACH_PAGE] close (...)`,
  `[ACH_PAGE] hidden`.
- **Opened from the pause menu's own ACHIEVEMENTS row** (5.14; L-a's developer aid `SR_ACH_PAGE_ON_PAUSE` went with it).
- **Tests:** `difftest --achievements-page-test` (the page model and its view, the XDBF images, the kit's decode from
  memory - scaled, grey - and SR_Achievements; `SR_ACH_XDBF=<the title's XDBF>` adds the real 18 icons);
  `difftest --achievements-render` (the page's frames through the offscreen harness, `mod_library_render.cpp`).

### 5.14 The 9-row pause menu and the two-choice QUIT (lane E L-c, RELEASE_PLAN step 5; STEP5_PLAN 10, the owner's q14d; tag PORT FEATURE)

With port features on, `SCRPAUSEMENU` reads RESUME GAME, RESTART OBJECTIVE, SAVE / LOAD, **ACHIEVEMENTS**, STATISTICS,
MOVES LIST, **OPTIONS**, (PHOTO MODE: unit P), QUIT GAME (`40_pause.patch`, `srpause.aptasm`). Port features off =
the retail nine rows, untouched.

- **No clone, no new tab order.** The nine retail buttons are re-labelled and re-purposed: btn4 ACHIEVEMENTS (was
  STATISTICS), btn5 STATISTICS (was MOVES LIST), btn6 MOVES LIST (was GAMEPLAY OPTIONS), btn7 OPTIONS (was CONTROLLER
  OPTIONS), btn8 PHOTO MODE's slot (was AUDIO OPTIONS; hidden - `disable()` and `_visible = false` - until unit P gives
  it its action), btn9 QUIT GAME. `m_tabIndex` stays 0-8, and the game's own by-name logic (InitGui disables RESTART /
  SAVE / LOAD, restores `PauseSelectedItem`) keeps its shape. STATISTICS / MOVES LIST keep the game's own text
  (`Label_ScrStatistics`, `Label_ScrMovesList` through its LocalizationAdapter); ACHIEVEMENTS / OPTIONS are literals.
- **Layout:** `edit-place` only: rows 1-8 on one 38-unit pitch from y 68 (ACHIEVEMENTS fills the retail gap after
  SAVE / LOAD), QUIT GAME at 392 (retail 58 .. 344, 402); the panel, its bevels and the hint bar do not move.
- **A on a re-purposed row** first does what the game's own rows do (`PauseSelectedItem` := the row, the button's own
  press, the OPEN sound), then `SRPause.OnRow`: ACHIEVEMENTS -> `fscommand("SR_Achievements", "IG")` (the host page,
  5.13; the pause menu stays under it with ACHIEVEMENTS focused); STATISTICS / MOVES LIST -> the game's own screens
  (`FlowMgr.SwitchScreen`; their own back returns with the row focused); OPTIONS -> the OPTIONS screen (5.12: the
  game's getters first, then the open; on close `SR_OptionsClosed` -> the pause menu with OPTIONS focused, VIEW PAD
  LAYOUT -> the game's own CONTROLLER OPTIONS with `PauseSelectedItem = "btn7"`, DEV TOOLS in a dev build -> the Dev
  Tools pages, back to the pause menu); QUIT GAME -> the two-choice popup.
- **QUIT GAME:** the game's own popup shape (`ScrPauseMenu.showWarning`: title `Generic_WARNING`, body
  `Generic_AREYOUSUREQUITGAME`) with **QUIT TO TITLE** (the default: the game's own quit, exactly its
  quitCallBack's YES - the CLOSE sound, `StateAdapter.QuitGame`, `FlowMgr.ChangeInterfaceState("IG")`,
  `PauseSelectedItem = "btn1"`; `QuitGame` only raises the request, the interface change takes the pause screen down,
  and the next pause opens on RESUME; like the game's own YES it lands on the PRESS START title - START, then the MAIN
  MENU; the Ally rolls lb2 434 and lc2 480 / 482 show it, so the choice says TITLE, not MAIN MENU) and **QUIT TO DESKTOP** (`fscommand("SR_Quit", "")`, the front end's EXIT path,
  9.2); B cancels back to the pause menu with QUIT GAME focused. Logged `SR_Log 'pause quit button=N'`.
- **Checks:** `tools/apt/test_pause_patch.py` builds the recipe on the player's ui.AST and reads the bodies it names:
  the rows' positions, labels and the hidden slot; HandleInput's head routes A on btn4/5/6/7/9 (only) to `OnRow` and
  returns false, the game's own HandleInput follows unchanged; the popup's ordered arguments; each answer's action, the
  main-menu one compared with the disc's quitCallBack. Its mutant tests (no return after `OnRow`, btn8 routed,
  `QuitGame` misnamed, the labels reversed, the interface change dropped) must fail.
- **MORE OPTIONS is gone** (iter-464 `btn10`, 5.7): AUTOSAVE WITHOUT ASKING and (dev builds) DEV TOOLS are rows of the
  OPTIONS GAME tab, DISPLAY OPTIONS is the OPTIONS screen, CHEATS its CHEATS tab, MODS only on the MAIN MENU.
- **Rolls:** `tools/scenarios/step5_pause_rows.json` (every row from the pause menu, the two QUIT choices).

### 5.2 Options screens patch (prepended to `HandleInput` in all three)

*Design sketch. As built (`options_input_head.aptasm`, 5.4): also requires
`FlowMgr.CurInterfaceState == "FE"`, also takes Esc (`INPUTCODE.SELECT`), and instead of
`BackFromSettings` hides the well, clears the hints and unloads the overlay with
`SwitchLevel(settingsScreen, "NONE", 60)`, then re-shows the menu (deferred).*

```actionscript
if (_global.SRFrontEnd.inSettings) {
  if (keyCode == _global.INPUTCODE.TRIANGLE || keyCode == _global.INPUTCODE.START) {
    _global.playSound(_global.SOUND.CLOSE);
    _global.SRFrontEnd.BackFromSettings();
    return false;
  }
}
// ...original HandleInput continues unchanged
```

This is required, not cosmetic. Outside pause mode, Audio and Controller send Back to
`SCREEN.CONTROL_ROOM`, and Gameplay has no Back. START in pause mode calls
`ChangeInterfaceState("IG")`, and from FE that calls **StartGame** (MainLib.as 0x1AC0). The
patch catches both before the original code runs. `INPUTCODE.TRIANGLE` is the pad's B button:
ScrPauseMenu shows hint "B" for it (ScrPauseMenu.as 0x0A71-0x0AB8, 0x0DE6).

The options screens will render their **non-pause** look: `InitGui` calls
`_global.SetFortressDisplayWell(true)` when not in `PAUSEMODE_MENU` (ScrAudioOpt.as 0x0506).
That is the retail Control Room look. How it renders in the FE state is *not checked*
(§11 R4).

---

## 6. The gate and the natural path

**The switch (built, step 2).** The user enables the mod without environment variables
by listing `custom_frontend` in `<content root>/mods/enabled.txt` (or running
`python tools/apt/build_custom_frontend.py --enable`); the exe reads that file once at boot.
`SR_CUSTOM_FRONTEND=1` / `=0` and `SR_MODS=` are the overrides for rolls, which ignore
`enabled.txt` (`SR_INSTANCE_ID` set); every other automated launcher, and difftest, sets
`SR_MODS=0` (docs/MODS.md "Automated runs"). Full rules: `docs/MODS.md`.

Enabling `custom_frontend` turns on one thing, read once at boot: the **read-only asset
overlay** rooted at `<content root>/mods/custom_frontend/` (§7). Its patched movies then issue
`fscommand("SR_*")` calls, which the **FSCommand bridge** (§9) handles. As built the bridge
is a strong def of `sub_826D39A0`, always linked and not tied to a mod name (§9.2 "As built");
it is inert for retail content (0 FSCommands), so with the mod off it is never reached. The
settings store lives with the saves (section 0), not under `mods/`. (The design below
planned a dispatch-table patch installed only under the gate; superseded.)

When it is **unset or 0**:

- `resolveGuestPath` / `stub_NtCreateFile` / `stub_NtQueryDirectoryFile` take their current
  code path, with no extra `stat` or probe per open. The A/B is byte-for-byte the same
  kernel file behaviour.
- `sub_826D39A0` is reached by nothing (retail issues 0 FSCommands; checked by 0 `[SR_FSCMD]` lines).
- There is no `[MOD]` log line. A test for this: gate-off runs must show the same APT_REQ
  order as the archived runs (main, ScrForeground_IG, ScrPressStart, … ScrGenPop), and zero
  `[MOD]`/`[SR_FSCMD]` lines.

If the gate is on and `mods/custom_frontend/` is missing, log one error line and run the
natural path. Do not crash.

**Charter classification.** This is a **content mod**, not a platform fix and not
forcing. The game's own Apt VM runs it, and every game action goes through the game's own
adapters. It changes UI behaviour, so it is opt-in, runs with it are tagged
`mod:custom_frontend`, and they are **never** counted in `docs/NATURAL_BASELINE.md`. It does
not go under `SR_ALLOW_FORCING`, because it writes no guest state the game didn't write and
calls no guest function the game's UI doesn't call. The generalisation for later is
`SR_MODS=a;b`, one overlay root per mod, with custom_frontend as the first client.

---

## 7. Asset overlay and Apt tooling

### 7.1 Kernel file-layer overlay (new; ~0.5 day)

**Built 2026-09-30 (step 2), as a general mod framework: `docs/MODS.md`.** `kernel/mod_overlay.{hpp,cpp}`;
hooks in `stub_NtCreateFile` (NtOpenFile delegates) and `stub_NtQueryDirectoryFile`;
any number of mods from `mods/enabled.txt`, earlier lines win; a `[MOD]` line per shadowed
path. Differences from the plan below: `resolveGuestPath` is unchanged (the overlay sits
after it in NtCreateFile, and the disc-name aliases `sound1`/`objtiv1`/`fmv1..5` resolved
their target through it too, until the aliases were deleted 2026-10-20); `NtQueryFullAttributesFile`
was **not covered** then - the natural run makes 1 call per boot (lr 0x8246975C,
`GetFileAttributesA_sub_82469720` from the shader include/compile paths) and the generic stub returned
success without filling the result, so every path "existed". Since step4/audit batch 3 (2026-10-20) it
answers by real existence through the overlay (`modOverlayResolveFile` / `modOverlayHasDir`), as the
plan's requirement below asks (docs/MODS.md). `/mods/` was already gitignored. Test:
`build/difftest.exe --mod-overlay-test` 37 passed, 0 failed. The original plan:

Add a **union overlay** that mirrors the disc tree. With the gate on:

- `stub_NtCreateFile` (kernel/stubs.cpp:1688; NtOpenFile delegates to it) and
  `resolveGuestPath` (kernel/file_io.cpp:162) work as follows. Once a guest path maps to
  `Superman Returns (USA)/<rel>`, check `<content root>/mods/custom_frontend/<rel>` first,
  with a case-insensitive match. If it exists, open that file **read-only**. Log
  `[MOD] overlay <guest path> -> <host path>` once per file.
- `stub_NtQueryDirectoryFile` (stubs.cpp:2140) appends the overlay directory's entries, and
  the overlay wins on a name clash, with its size.
- `NtQueryInformationFile` already takes sizes from the open `FILE*`, so opened handles stay
  consistent (engine_overlay finding 10). `NtQueryFullAttributesFile` by path must use the
  overlay path when it applies.

The content root is the parent of `build/` (file_io.cpp:19-40), so the repo root when run as
documented. `/mods/` gets a root-anchored `.gitignore` entry **in the same commit**: the
generated files are game-derived bytes and are never committed.

### 7.2 Two delivery forms, chosen by experiment E1

| Form | File in the overlay | For it | Against it |
|---|---|---|---|
| **A: whole container** | `DATA/ui.AST` (89,595,420 B, rebuilt) | Certain consumer: the natural path opens `DATA/ui.AST` (align2 run.log NtCreateFile summary, handle 33). The writer rebuilds ui.AST **byte-identical** and keeps untouched entries identical (827/827 in the demo) | An 89.6 MB generated copy; mods do not compose (the last one wins) |
| **B: engine loose mount** | `DATA/_workinprogress/SCRPRESSSTART.ast` (plus the 3 options `.ast`) | This is the engine's own override mechanism. `MountLocalizedBigFiles_sub_8218D128` always mounts a LooseFileStorage on `d:\DATA\_workinprogress` at priority **0x7D0**, above ui (0x32). The runtime extension list includes `ast` (golden dump 0x82911D00). The loose name grammar is `<name>.<ext>` (AptResNameParseToDescriptor_sub_826EA798). Per-movie files, and mods compose | **Static evidence only.** The folder is enumerated at boot (run.log:6286-6312) but has never served a file. That Apt package requests resolve through it, and that the higher priority wins, is *not checked* |

**E1**, the first experiment after the overlay lands. It needs 3 short rolls, each a
separate process:

1. **Form A identity.** Overlay `DATA/ui.AST` rebuilt byte-identical. Expect the `[MOD]
   overlay` line and the same APT_REQ order and frames as gate-off. This proves the overlay
   plumbing.
2. **Form A edit.** In SCRPRESSSTART's .const, change `LOCALIZED_PRESS_START`'s value
   `"SUP_PRESS_START"` (ScrPressStart.as 0x4954) to `"Generic_QUITGAME"`. Expect the press-start
   text to read **QUIT GAME**. This proves an edited movie runs in the game.
3. **Form B.** Revert ui.AST, and place the same edited package as
   `DATA/_workinprogress/SCRPRESSSTART.ast`. Expect NtCreateFile of that path near
   `[APT_REQ] screens/bootflow/ScrPressStart` and the QUIT GAME text.

If B works, the build tool emits form B by default. If it doesn't, it emits form A, and B is
recorded as REJECTED in ENGINE_MAP and the RAG. Either way the kernel overlay is the same
code.

**E1 result (2026-09-30, docs/MODS.md "Experiment E1"): FORM A.** (1) identity: the
`[MOD] overlay shadows 'DATA/ui.AST'` line, ui.AST opened from `mods/custom_frontend/`, the
APT_REQ list identical to the gate-off roll, the title and LOAD A GAME? frames unchanged.
(2) edit: the title reads **QUIT GAME** (2/2 rolls), APT_REQ identical, the popup unchanged.
(3) form B: `DATA/_workinprogress/SCRPRESSSTART.ast` was listed by the boot enumeration and
never opened; the title still read PRESS START. Form B is rejected for that name only;
`__ASSET__<16 hex id>.ast` and the hash key are untested (docs/MODS.md "Experiment E1").
Answers §13 Q3 (no, as tried). §11's fall-back condition (an edited movie cannot run) did not occur.

**2026-10-02: the form-B verdict (3) is VOID** (iter-464 modcore; the owner's private mod project
`research/modsystem/DESIGN.md` finding 2). The host's NtQueryDirectoryFile packed
`GAMECONFIG`, `__ASSET__GAMECONFIG` and `SCRPRESSSTART.ast` into ONE 244-byte answer
(e1_form_b run.log 6274-6279) and the game converts only the first record of each call, so it
never saw SCRPRESSSTART.ast. The listing is fixed (`SR_NTQDIR_XENIA`, docs/MODS.md section 12)
and the custom front end now ships as a PORT FEATURE delivered through exactly this route
(docs/MODS.md section 7): its 5 movies are generated from the player's ui.AST by SRPATCH1
patches into `DATA/_workinprogress/custom_frontend/<MOVIE>.ast`.

### 7.3 Apt toolchain (`tools/apt/`)

Promote `archive/v1_frontend/apt_format/*` as follows:

- `bgfa.py`: BGFA1.05 read/write. Outer TOC ids are FNV-1a-64(UPPER name) little-endian,
  828/828. The field widths and shift come from the header, not constants. The writer is
  byte-exact on ui.AST, 821/821 nested containers and text.AST.
- `apt_model.py`: .apt/.const walker. The pointer map is the game's own relocation set
  (AptParseReloc_826AE6D0, AptSpriteFramesRelocate_sub_826A39A0,
  AptActionBytecodeRelocate_sub_8269F3A0).
- `apt_asm.py` (disassembler), `apt_graph.py` (editable graph and emitter), `apt2txt.py`.
- Regression tests `rt_test.py` and `rt_graph.py` must print 820/820 byte-exact.
- **New: `aptasm.py`, a text assembler** that accepts the disassembler's syntax, plus
  symbolic labels, `.const "str"`, `.pool` and `.func name(params) … .end`. Round-trip gate:
  disassemble → assemble must be byte-identical for all 820 movies. The mod source is then
  readable `.aptasm` diffs, not Python.
- **New: `patch_movie.py`**, which applies a patch file with `prepend-to <function>`,
  `append-to <function>` and `add-initaction` to a movie's graph. It also needs orphan-block GC
  and string dedupe in `Graph.emit`.
- **New: `build_custom_frontend.py`**, which reads the user's own
  `Superman Returns (USA)/DATA/ui.AST`, applies `tools/apt/mods/custom_frontend/*.aptasm`
  (tracked sources), re-parses each output (0 uncovered non-zero bytes) and writes form A or
  form B into `mods/custom_frontend/`.

Writer invariants, all from `apt_format/NOTES.txt`:

- every block is 4-aligned;
- every Push and ConstantPool has **its own** index array (12,992/12,992 unshared, because the
  loader rewrites them in place);
- branch offsets (`next_pc + i32`) and DefineFunction(2) body sizes are recomputed after any
  insert;
- `.const` strings stay in item order, 4-aligned;
- nested TOC ids are sorted ascending, because the lookup is a bsearch (sub_827066D8);
- the TOC field widths are respected (nested offset 2 bytes <<3; some entries have a 2-byte
  size field). Widen the header width bytes if an entry outgrows them; the reader honours
  them (sub_82706170).

`fscommand("SR_Quit","")` assembles to ActionGetURL (0x83) with operand
`{url* → "FSCommand:SR_Quit", target* → ""}`. The strings live in the .apt, and the operand
is 4-aligned (AptAS2OpGetURL_sub_826BADE0, this session).

**Step 3 status (2026-09-30): built and verified at the format level only. Nothing has run
in the game.** `aptasm.py`, `patch_movie.py` and `build_custom_frontend.py` are in
`tools/apt/`. Their tests are `rt_asm.py` (8 checks) and `rt_patch.py` (19 checks). A
mutation check caught 12/12 injected defects. Where the build differs from the plan above:

- **No string/item dedupe.** The retail corpus has every `.const` item referenced by exactly
  one array element (19,812/19,812), and the loader rewrites arrays in place. So new code gets
  a fresh item per element, as retail does, and dedupe would add a sharing pattern retail
  never contains. `Graph.gc()` drops orphan blocks and unreferenced items. An empty
  DefineFunction params pointer is a weak reference there, and it is kept non-null.
- **`.const NAME = value` is an assembler-time alias** (`$NAME`), not a free-standing
  `.const` item, for the same reason.
- **Patches are `*.patch` files.** `.aptasm` files hold code bodies that a patch names with
  `file=`. Ops: `replace-const`, `prepend-to` / `append-to` (`function NAME [in CLASS]` or
  `action SEL`), `add-initaction`, `add-doaction`. Pool names in a snippet resolve in the
  host block's pool, and missing names are appended to it.
- **`add-initaction` works at the format level.** It is tested on SCRPRESSSTART and
  SCRGAMEPLAYOPT: first/last placement, auto and explicit spriteId, and the edited movie
  relocates. No reason was found to move the new code elsewhere. IDA shows `+4` is only the
  run-once sign flag (ENGINE_MAP, frame-item types), but that was not proven exhaustive.
  **E1 must still show that an edited movie runs.**
- The E1 step-2 edit is one patch line:
  `replace-const 'SUP_PRESS_START' -> 'Generic_QUITGAME' count=1` under
  `movie SCRPRESSSTART`. It builds a form-A ui.AST in which the other 827 entries are
  byte-identical.

---

## 8. XAM storage platform fix: separate and always on

Platform fix per the charter: XAM and the kernel file system were the console's platform,
and the guest makes every decision. Gate `SR_XAM_CONTENT`, **default ON**, with `=0`
restoring the old stubs for A/B. The reference is Xenia `reference/xenia-canary/src/xenia/
kernel/xam/{xam_ui.cc, xam_content.cc, xam_content_device.cc, content_manager.cc}`, ported
with attribution (BSD-3). The full spec is `archive/v1_frontend/xam_storage/notes.md` §4-6.

**Part A (fixes the prompt loop):**

- `stub_XamShowDeviceSelectorUI` (stubs.cpp:5511): `r6` = u64 total (one GPR), **`r7` =
  pdwDeviceId, `r8` = pOverlapped**. `r8==0` returns 0x57. Bad user or flags
  (`&0x83F00008`) or `!r7` set ovl+0x18=0x57 and return 0x57. Otherwise:
  `xnEnqueue(9,1)`; `*r7 = BE 1`; complete the overlapped (+0 InternalLow 0, +4 0, +0x18
  ExtErr 0); schedule `xnEnqueue(9,0)` about 100 ms later from a host timer; **return 997**.
  This matches Xenia xam_ui.cc:514-571 and 142-161, kernel_state.cc:1128-1170.
- A shared XOVERLAPPED completion helper: result, length and ext-error big-endian; signal the
  hEvent at +0xC if it is set.
- A dummy HDD device table: id 1, type 1, name "Hard Drive" (10 chars or fewer, to avoid guest
  truncation), with total and free from `std::filesystem::space(save root)`.
- `XamContentGetDeviceState`, `GetDeviceData` (0x50 bytes: id, type, u64 total, **u64 free at
  +0x10**, UTF-16BE name) and `GetDeviceName`. Unknown ids return 0x48F.

**Part B (real saving and loading) - BUILT iter-463, `kernel/xam_content.{hpp,cpp}`:**

Gate `SR_XAM_CONTENT_FS`, **default ON**; `=0` leaves every old stub registered (exact
parity). Sub-gate `SR_NTSTATUS_DOSERR` (**default ON since 2026-10-18**, `=0` disables;
effective only with the main gate) switches `RtlNtStatusToDosError` from the generic stub
(returns 0, so every guest `GetLastError` after a failed Nt* call read 0) to Xenia's table.
It was OFF by default until then because it changes `GetLastError` title-wide (disc
FindFirst/FindNext and optional-file misses at boot), not just for saves; it was baked ON
with step4/audit fix batch 1 after natural A/B rolls showed no regression (docs/LOGIC_AUDIT.md
2.0b / 2.0d). The save and load success paths the difftest
drives do not depend on it (CreateFileA sets the success GLE 183 / 0 itself); the failure
paths map to a save error either way (1 instead of 9). A natural save+load roll must still
confirm no other guest branch on GetLastError in the save flow. Port of xenia-canary
@0b4e6c4 `xam_content.cc`, `content_manager.cc`, `xam_enum.cc`, `xenumerator.cc`,
`xfile.cc`, `virtual_file_system.cc`, `xboxkrnl_io.cc`, `xboxkrnl_error.cc` (BSD-3).

- Exports replaced by `registerXamContentExports(reg)` (the LAST line of `registerStubs()`;
  each keeps the stub it replaced and forwards to it while the test seam turns the gate
  off): `XamContentCreateEx` (Xenia switch on flags&0xF: 1 CREATE_NEW, 2 CREATE_ALWAYS,
  3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING; `*pDisp` 1 created / 2 opened;
  root already mounted -> 0x57; 9th argument pOverlapped read at `[r1+0x54]`), `Close`
  (force-closes every file handle under the root, then unmounts; unknown -> 2), `Flush`,
  `Delete` (open -> 5, else remove the package; the `.header` stays, as Xenia),
  `GetCreator` (saved game -> creator = user 0's XUID; missing -> 3), `SetThumbnail`
  (`__thumbnail.png`), `CreateEnumerator` + `XamEnumerate` (snapshot at create from the
  headers, dir-name fallback; exhaustion 0x12; unknown handle 6, synchronously, the
  overlapped untouched, as Xenia), `NtWriteFile`,
  `NtFlushBuffersFile`, `NtQueryFullAttributesFile`, `RtlNtStatusToDosError`.
- One-line hooks at the top of `stub_NtCreateFile`, `stub_NtOpenFile`,
  `stub_NtSetInformationFile` (classes 13 delete-on-close, 14, 19, 20, 4) and
  `stub_NtQueryDirectoryFile` (ANSI mask in r10, one entry per call, real FILETIMEs,
  non-empty mask restarts and misses with 0xC000000F, exhaustion 0x80000006), plus
  `xamContentOnNtClose` in `stub_NtClose`. They claim only paths `<mounted root>:\...` and
  handles they opened; everything else runs the old stub unchanged (the disc stays
  read-only). NtCreateFile follows Xenia VFS `OpenFile` (missing parent 0xC000003A,
  OPEN/OVERWRITE of a missing file 0xC0000034, CREATE collision 0xC0000035, directory with
  NON_DIRECTORY 0xC00000BA, IOSB.Information 0 superseded / 1 opened / 2 created /
  3 overwritten). A known root that is not mounted gives 0xC000003A.
- Beyond Xenia, for host safety: path components and package names must be plain names
  (no `.`/`..`, separators, `:` streams, wildcards, control chars, trailing space/dot or DOS
  device names) -> 0xC0000033 / 0x7B. CREATE_ALWAYS and TRUNCATE_EXISTING move the old
  package to `<root>/.backup/...` (one generation, outside the enumerated tree) instead of
  deleting it. The game never reads `.backup`: it is a MANUAL recovery copy (after a crash
  mid-save, LOAD GAME shows an error while the previous save sits in `.backup`; copy it
  back by hand). The move goes to a staging name first and the previous backup is dropped
  only after it succeeded; if the move fails (a file in the package held open by another
  process) nothing is deleted and the create fails with 0xB7.
- Headers read back from disk are validated before the guest sees them (Xenia trusts any
  file of >= 0x148 bytes): exactly 0x148 or 0x14C (+ licence word) bytes, display name
  NUL-terminated within 128 wchars, file name == the folder name, type == the listed type.
  Otherwise the folder-name fallback is listed. Reason: the boot-time enumeration
  (sub_8263FEE0) escapes the display name with Str_WideToAsciiEscaped_sub_82642388 (no
  bound; 6 bytes per wchar >= 0x100) into a 32-byte stack buffer, so a garbage header
  would smash the guest stack on every boot. Folders whose names cannot round-trip through
  XCONTENT_DATA (> 42 chars, or not a plain name) are skipped.
- `XamContentGetDeviceData` reports free/total space for the volume holding the content
  root (`xamContentSpace`).

**Host save folder** (Xenia's layout, so a Xenia content folder drops in):

```
<root> = SR_CONTENT_DIR
       | %TEMP%/SupermanRecomp/content-<id>-<pid>   if SR_INSTANCE_ID is set (automated
                                                   roll: fresh, empty, per process)
       | <exe dir>/content                 if <exe dir>/portable.txt exists (UnleashedRecomp)
       | %LOCALAPPDATA%/SupermanRecomp/content
  0009000000000001/                 XUID of user 0 (kXamUser0Xuid = stub_XamUserGetXUID)
    454107ED/                       title id (kXamContentTitleId)
      00000001/Superman Returns/    the package; 'rmcsave:\' maps here
        Superman Returns            the save file (88 + 96*N bytes, N inferred)
      Headers/00000001/Superman Returns.header   0x148 B XCONTENT_AGGREGATE_DATA (BE):
                                    XCONTENT_DATA 0x134, pad, u64 xuid @0x138,
                                    u32 title_id @0x140, pad (Xenia's exact layout)
  .backup/<XUID>/<TITLE>/<TYPE>/<name>/           host-only previous package (manual recovery)
```

**Automated rolls are hermetic.** The natural game enumerates saves at boot and a found
save changes the front-end path (SaveContentFindFirst returns 11 instead of 12; SAVE takes
the overwrite prompt), so a roll must never see the player's saves or an earlier roll's.
Every harness sets its own root: `tools/run_recomp_x.ps1` gives each instance
`archive/content_rolls/<stamp>/run<i>` (listed in `last_run_x.json` as `contentDirs`),
`tools/sr_drive.ps1` gives each attempt `<attempt dir>/content` (journal line `saves:`,
and provenance's SR_* snapshot), and `run-30s.ps1`, `run-test-capture-log.ps1` and
`verify_present_window.ps1` use a fresh scratch root. Any other launcher that sets
`SR_INSTANCE_ID` gets the exe's own fresh per-process root. A caller-supplied
`SR_CONTENT_DIR` (env or `-Gates`) is a SEED: copied into each run's root, never written.
Record the seed (or "empty") in every NATURAL_BASELINE repro.

The header is load-bearing: both the save and the load flow match an existing save by
DISPLAY NAME (`wcsncmp(..., 42)`, sub_827A5998) against `L"Superman Returns: The
Videogame"`, and the header-less fallback would enumerate the directory name instead.
A Xenia save uses Xenia's profile XUID, so its XUID folder must be renamed to
`0009000000000001`. Do NOT move it under `0000000000000000` (common content): enumeration
lists it, but XamContentCreateEx and GetCreator resolve the package under the caller's
XUID, so LOAD GAME would show it and then fail with error 9 (Xenia behaves the same).

Test: `build/difftest.exe --xam-content-test` drives the real guest wrappers
(sub_828A8218, CreateFileA, WriteFile, ReadFile, FindFirstFileA, DeleteFileA,
SaveContentFindFirst) against a fresh `SR_CONTENT_DIR`: 85 passed, 0 failed (the test
checks SR_NTSTATUS_DOSERR's default - ON since the 2026-10-18 bake, 92 checks today - then
keeps it on through the seam for the GetLastError checks). `SR_XAM_CONTENT_FS=0` gives 17 passed, 67 failed (the old stubs);
`SR_NTSTATUS_DOSERR=0` gives 77 passed, 7 failed (every GetLastError check). A code
mutation restoring the pre-review behaviour (trusted headers, backup dropped before the
move, XamEnumerate through the overlapped, no SR_INSTANCE_ID scratch root) fails T16a-d/f,
T17c, T18a-b and T19.

Still open: no natural save+load roll yet, and no byte comparison against a Xenia save of
the same slot. The RtlNtStatusToDosError port (default OFF) changes the boot path when on
(at least 32 calls with 0x80000006 / 0xC0000034 / 0xC0000002 return 0x12 / 2 / 1
instead of 0); it needs a natural A/B roll pair before it is baked ON.

---

## 9. AS → host bridge (Exit now, PC settings later)

### 9.1 The retail channel

AS2 `fscommand(cmd, args)` compiles to ActionGetURL. `AptAS2OpGetURL_sub_826BADE0` compares
the url against the prefix `off_829313C4` ("FSCommand:"). On a match it calls
`sub_82682DF8(ctx, url, target)`, which calls the pointer at `*0x829A1A90` with
`(url + strlen(prefix), target)`. That pointer is **`sub_826D39A0(cmd, args)`**, installed by
AptGfxRendererDriverInstall_sub_826D4520. It does
`AptNamedRegistryLookup_sub_826D3780(*0x829A23E0, cmd)->vt[3](args)` with **no null check**,
so an unknown command is a fatal host AV under our VEH. All of this was checked by decompile
this session. Retail content issues **0** FSCommands (case-insensitive scan of ui.AST).

### 9.2 The shim

- Only under `SR_CUSTOM_FRONTEND=1`, patch the dispatch-table slot
  `PPC_LOOKUP_FUNC(gb, 0x826D39A0)` with a host function, saving the original. This is the
  same technique as kernel/fe_flow_watch.cpp:1872-1881 (`PPC_LOOKUP_FUNC(gb, VA) = shim`). A
  table patch only sees indirect calls, and `sub_826D39A0` is reached only through
  `*0x829A1A90`, so the patch sees every call.
- Read `r3` (cmd) and `r4` (args) as guest C strings, bounds-checked with fault-free reads.
- For `cmd` starting with `SR_`: handle it on the host and **never** delegate. Log
  `[SR_FSCMD] <cmd> <args>` (count-safe: one line per command). An unknown `SR_*` is logged
  and ignored.
  - `SR_Quit` calls `SDL_PushEvent(SDL_QUIT)`. `PlatformSDL::pumpEvents` returns false
    (app/platform_sdl.cpp:65) and the main loop in app/main.cpp:380-394 returns 0, exactly as
    when the user closes the window.
- Anything else is delegated to the original, so natural behaviour is unchanged.
- Register it in `docs/OVERRIDES.json` as `platform-fix`, with notes "Apt host fscommand
  handler (mod API); inert for retail content; installed only under SR_CUSTOM_FRONTEND".
  Then run `python scripts/gen_override_manifest.py` and `scripts/audit_forcing.py`.

**As built (step 5):** a strong def of `sub_826D39A0` in `kernel/mod_bridge.cpp` instead
of a dispatch-table patch (the registry sees it; OVERRIDES.json `platform-fix`). It handles
`SR_*` whichever mod issued it (review finding 2: tying it to the custom_frontend name made a
renamed copy crash at the title, because the game's own body null-derefs the registry miss);
retail content issues 0 FSCommands. Commands: `SR_Quit`, `SR_Log`, unknown `SR_*` ignored
(docs/MODS.md "Host bridge"). The window-close path itself crashed on exit (0xC0000005,
roll loadexit2), so the exit now goes through `hostExitNow` in app/main.cpp
(`SR_FAST_EXIT`, default ON): flush the input tape, the log and stdio, then
`std::quick_exit(0)` (the reference host's `App::Exit` uses `std::_Exit`; quick_exit adds the
`at_quick_exit` handlers, where the guest profiler's final dump is registered). Both loops
that run the game use it (the default event pump and `SR_HLE_OWN_PRESENT=0`); the replay
modes keep `return rc` (no guest threads, and replay_ab reads rc). It also covers the
window-close path with the mod off: a platform fix, not a mod feature. Guest saves are flushed on every NtWriteFile, so
nothing host-side is mid-write; a guest save in progress when the window is closed is cut
short as on a console power-off (xam_content keeps a `.backup`). Verified: exit code 0,
0.17 s after `SR_Quit` (loadexit5). Tests: difftest `--mod-overlay-test` M9a-f.

### 9.3 PC settings later (design only)

Under the no-launcher decision (`project_no_launcher_ux.md`):

- A **PC SETTINGS** row in the Settings panel opens an authored Apt panel, again ScrGenPop
  chrome or a BasicButton list cloned from ScrPauseMenu.
- AS calls `fscommand("SR_GetSettings","")` and `fscommand("SR_Set","key=value")`.
- The host answers by invoking a listener such as `_global.SRFrontEnd.OnHostSettings(str)`
  through the AptExecutor vtable `+0x20` (`sub_826D0D30`, which invokes by path with AS-value
  args). That is the same path the game's natives use to send `LoadComplete` and
  `SaveComplete`, so it already runs on the guest thread inside the interpreter.
- Values persist in a host settings file. The first keys are resolution scale, window mode
  and vsync, which are the remaster features the HLE layer hosts.

---

## 10. Verification per step

General rules (CLAUDE.md):

- one game process at a time;
- live rolls are samples, so repeat them sequentially;
- frames come from `SR_PRESENT_DUMP=1`;
- run `tools/audit_log_gating.py --tag X` before quoting any count;
- input comes from tapes (`tools/gen_input_tape.py` + `SR_INPUT_REPLAY`). Add a `femenu`
  pattern: the START era, then DOWN×n and A with fixed spacing.

Replay (`tools/replay_ab.ps1`) does not cover this work, because nothing here touches the
PM4 path. The one renderer-side check is that the press-start frame stays consistent with
`data/gpu_traces/menu_pressstart_golden.png` with the gate both off and on.

| Step | Pass evidence | Compared against |
|---|---|---|
| XAM-A | Log: `XamShowDeviceSelectorUI … rc=997`, XNotifyGetNext id=9 param=1 then param=0, and **no** No_Device CppShowPopup. Frida: `*(*0x8299FB18+0x31C)==1`, `+0x360==1`, `*0x8292DFFC==0x7E`, `E014==E018==0`. Frames show whatever follows the device step | Golden dump `data/xenia_canary.DMP` (the values above, `xam_storage` finding 3). **Live Xenia** through press start with an empty content dir (tools/XENIA_HARNESS.md: shot + pad). This is the first oracle frame of the boot-flow popups, and it confirms the LoadCreate popup is what follows. `SR_XAM_CONTENT=0` reproduces the loop |
| Overlay + E1 | The 3 E1 outcomes in §7.2; gate-off run with zero `[MOD]` lines and an unchanged APT_REQ order | Archived APT_REQ lists (tape1/tape2); golden press-start frame |
| Toolchain | `rt_test.py`, `rt_graph.py`, and aptasm round-trip all 820/820 byte-exact; patched movies re-parse with 0 uncovered non-zero bytes | — |
| Menu v1 (New/Load) | Frames: the 4-row panel in DisplayWell chrome. Log: the hook's `pendingId` and the native `+16` compare agree. **New Game** gives the same downstream as natural button 0 (`APT_REQ screens/hud/main_hud`, run state 3). **Load Game** shows the SaveLoadSlots picker, and B brings the menu back | tape1 frames 020/024 (same chrome, width, colour 0x0071E1, font) and `mock_menu_panel_vs_real.png`. A gate-off natural run pressing START NEW GAME / LOAD GAME must have the same APT_REQ/log sequence after the choice |
| Exit | YES: `[SR_FSCMD] SR_Quit`, then the process exits with code 0 within 2 s. NO or B: the menu comes back. Gate off: the shim is not installed (log) | Window-close exit path |
| XAM-B | New Game → first save → files under `saves/0009000000000001/454107ED/00000001/`. Restart → Load Game lists the slot → it loads | Optional: our save loads in Xenia, and a Xenia save loads in ours |
| Settings | Each options screen opens from the menu. Values change (SettingsAdapter getters show the new values when reopened). B and START return to the menu; START does **not** start the game (no main_hud APT_REQ) | The same 3 screens reached from the in-game pause menu in a natural run (reference look), and R4 |
| Milestone | Whole-run frames (`tools/record_fullrun.ps1` + `tools/verify_run.py`) for every era with the gate on; one blind Astra second opinion; PROGRESS.md entry tagged **mod**, with repro command and what is still broken | Gate-off run of the same tape |

---

## 11. Why Apt-native is feasible, and when we would fall back

Feasible today at the format level:

- The container, the .apt/.const format and the bytecode are fully mapped, with 100% of
  non-zero bytes accounted for in all 820 movies.
- The writer round-trips byte-exact 820/820, including through the editable graph.
- A real edit (a code insert inside a DefineFunction2 inside an If, with a grown ConstantPool)
  re-links and re-packs with the other 827 entries byte-identical
  (`apt_format/out/ui_demo.AST`).
- The menu reuses only components the natural front end already loads (APT_REQ #5-#16:
  DisplayWell, BasicButton, PartText, PartHighlightBar, ButtonHints, HintA, ScrGenPop), so it
  needs **no new art and no new movie**.

**Fall back to a host-drawn overlay only if** E1 step 2 shows that an edited movie cannot run
in the game, meaning the overlay serves the file but the Apt loader rejects or ignores the
edit, and one attempt to find the reason fails. Examples of a reason: a checksum we missed, or
the nested-id hash. Even then, keep New and Load wired to the game's own popup answer through
a host call to `CppPopupComplete`, and record the Apt rejection as evidence.

### Risks

- **R1.** The loose `_workinprogress` route may not serve Apt packages. Fall back to form A
  (whole ui.AST); E1 decides.
- **R2.** The meanings of ui.AST header +8 (0x537C69D6) and the nested-id high dword are
  unknown. Edits keep both unchanged, and header +8 is shared by containers with the same TOC
  shape, so it is not a content checksum. This only matters if we ever add a new movie.
- **R3.** Is the LoadCreate popup always raised? It has three raise sites, one conditional on
  `*(g_FeFlowConfig+144)==1`. It is seen in our build (tape1 frame 024), but not yet with a
  working device selector and an empty save folder. **Contingency:** show the menu from
  ScrPressStart's START handler instead, remember the choice, call `StartBootFlow()`, and
  answer the LoadCreate popup with the remembered index if it arrives. Settings would then run
  before `applyOptions`, so values could be overwritten; that is why this is not the primary
  design.
- **R4.** The options screens in FE state render their non-pause look
  (`SetFortressDisplayWell(true)`), which has not been seen in our build. If the backdrop is
  wrong, set up the background the way `ChangeInterfaceState("PAUSEMENU")` does before
  switching, or clone the pause look. **Seen (roll settings/set3):** AUDIO OPTIONS renders its
  DisplayWell panel over the flyby; GAMEPLAY OPTIONS has no panel in the front end (its
  InitGui never calls SetFortressDisplayWell - retail only opens it from the pause menu), so
  its rows float over the city. Not judged against Xenia (the retail game never shows it
  there).
- **R5.** A native popup stays pending while the player is in Settings. This is the same
  native state as idling on "LOAD A GAME?". As built there is no ScrPressStart swap: the
  options screens are an overlay (5.4).
- **R6.** The **known menu heap-lock freeze** (iter-462, the 0x3846B33D bogus grant) can stop
  live rolls in the menus. That is a separate defect: A/B any freeze with the gate off before
  blaming the mod.
- **R7.** Settings persistence across sessions: built on the host store instead
  (section 0, 5.4); `XamUserReadProfileSettings` got its own Xenia platform fix
  (kernel/xam_profile.cpp). The title imports no `XamUserWriteProfileSettings`.
- **R8.** The `FlowMgr.CppShowPopup` passes `CppCallBack` but defines `CppCallback`.
  Natural popups answer anyway: 6 selector re-runs after A presses in align2, so member lookup
  is effectively case-tolerant here. The mod uses exact case everywhere.

---

## 12. Ordered build plan

| # | Step | Files | Effort |
|---|---|---|---|
| 0 | Correct the records: ENGINE_MAP §262 carve names, imports vs exports naming and frame-item types; note `main_apt_blob.h` = PARTHIGHLIGHTBAR. Promote the toolchain to `tools/apt/` with `rt_test.py` / `rt_graph.py` as regression tests | docs/ENGINE_MAP.md, tools/apt/* | 0.5 d |
| 1 | XAM-A: selector (r7/r8, 997, overlapped, XN_SYS_UI 9,1/9,0), device table, GetDeviceState/Data/Name. Natural roll + live-Xenia oracle capture. NATURAL_BASELINE entry if the natural game gets past the prompt | kernel/stubs.cpp (+ new kernel/xam_content.cpp), docs/OVERRIDES.json via manifest | 0.5 d + 0.25 d oracle |
| 2 | Asset overlay (gate, NtCreateFile / NtQueryDirectoryFile union) + experiment E1; `.gitignore` `/mods/`. **Done 2026-09-30**: general `mods/` framework + `mods/enabled.txt` switch (docs/MODS.md); E1 chose form A | kernel/mod_overlay.cpp, kernel/stubs.cpp, kernel/file_io.cpp | 1 d |
| 3 | `aptasm.py` + `patch_movie.py` + orphan GC / dedupe; 820/820 round-trip. **Done at the format level 2026-09-30** (no dedupe, by evidence; see 7.3) | tools/apt/ | 1-1.5 d |
| 4 | Menu v1: SRFrontEnd + hook + NEW GAME / LOAD GAME + EXIT confirm; `build_custom_frontend.py`; `femenu` tape pattern. **Done 2026-09-30** (§5.3, incl. the review fixes; sr_drive scenarios `femenu_new.json` / `femenu_load_exit.json` instead of a tape) | tools/apt/mods/custom_frontend/*.aptasm, tools/apt/build_custom_frontend.py, tools/gen_input_tape.py | 1.5 d |
| 5 | FSCommand bridge (`SR_Quit`) + OVERRIDES registration. **Done 2026-09-30** (§9.2 "As built", plus the clean exit path) | kernel/ (new mod_bridge.cpp), docs/OVERRIDES.json | 0.5 d |
| 6 | XAM-B: content packages, enumerator from host, rmcsave mount, write-capable NtCreateFile / NtWriteFile / NtSetInformationFile / attributes; `saves/` layout + `SR_SAVE_DIR`; `.gitignore` `/saves/`. **Done 2026-09-30** (5780fbb, `SR_XAM_CONTENT_FS` default ON; section 8) | kernel/xam_content.cpp, kernel/file_io.cpp, kernel/stubs.cpp | 1.5-2 d |
| 7 | Settings panel + the 3 options-screen patches. **Done 2026-09-30** (5.4): SETTINGS panel, options screens as an overlay, universal store + ApplyStore, XamUserReadProfileSettings platform fix | tools/apt/mods/custom_frontend/, kernel/mod_bridge.cpp, kernel/xam_profile.cpp | 1 d |
| 8 | Milestone: whole-run frames, Astra blind review, PROGRESS.md (tagged mod), ENGINE_MAP contracts (FlowMgr.CppShowPopup hook point, FSCommand path, loose-mount result), `scripts/index_fix.py`. **Done 2026-09-30** (5.5): on1 + off1 rolls, every era viewed, Astra agrees; the ENGINE_MAP contracts were recorded by steps 2-7; `index_fix.py` not used (it indexes crash+fix pairs; no crash here). The QUIT GAME soft-lock was fixed after this step (5.4 item 4, live-verified 2026-09-30) | docs/ | 0.5 d |
| 9 | Later: localized `SR_FE_*` ids through a text.AST overlay; PC SETTINGS panel + `SR_Get/Set` bridge; `SR_MODS` generalisation | — | 2-3 d |

Steps 0-8 take about **8.5-10 days**. Step 1 goes first: it is small, it is a natural-path
fix on its own, and New/Load depend on it. Steps 2-3 de-risk delivery before any menu code is
written.

---

## 13. Open questions (*not checked*; each has a step that answers it)

1. Does the C++ `cppId` passed to AS equal the value stored at saveFlow+16? **Yes**: step 4
   logged 1012091577 on both sides; saveFlow+16 is written after the AS call returns.
2. What exactly follows a successful device selection on an empty save folder? Step 1, live
   Xenia.
3. Do Apt package requests resolve through the `_workinprogress` loose storage, and does
   priority 0x7D0 beat ui.AST? **Answered by E1: not with `<TOC NAME>.ast`** - listed at boot,
   never opened. What that storage serves at all is still *not checked*.
4. Does `sub_822B15F8` pass text keys or localized strings to `CppShowPopup`? The hook
   accepts both.
5. What does `LocalizationAdapter.GetLocalizedText` return for an unknown id? This only
   matters for the step-9 localized labels.
6. Who consumes `StateAdapter.QuitGame`'s flag, and is there a natural writer of the
   app+0x18 quit flag? The search was not exhaustive. Irrelevant for Exit, which uses the
   bridge.
7. The file names inside the `rmcsave` package, and whether the save path uses
   SetFileAttributes / SetEndOfFile. Step 6.
