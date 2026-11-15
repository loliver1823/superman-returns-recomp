# Graphics settings and the PC SETTINGS screen (iter-466, gfxset lane; PORT FEATURE)

Owner, 2026-10-05: "I want to go all out on the graphics settings, which may mean the UI gets a
redesign with sliders and checkboxes and drop-downs for a true native PC look."

Every implemented option here is a **host** setting: it changes how the host draws, presents, sounds or reads
input. None reads or writes guest memory, calls guest code or changes a game decision. **The two exceptions are
the GAME and CHEATS tabs** (lane E L-b, step 5; section 1.1): they hold the game's own options and cheats and
change them only through the game's own functions, on the game's thread, never from the screen. Each
option's default reproduces the pre-feature output exactly (the code path it controls records
nothing at its default). Natural rolls run without any of it (port features off: the defaults,
display.ini not read).

The deferred expansion in section 4.1 is a plan, not implemented functionality. Its camera / LOD candidates
would be explicit port features affecting game decisions, outside the current host-only settings contract.

## 1. The screen

**OPTIONS** (lane E L-b, step 5: the owner's one options screen, mock-up q14d; it was PC SETTINGS): MAIN MENU >
OPTIONS and in game PAUSE > MORE OPTIONS > DISPLAY OPTIONS (until the 9-row pause menu, L-c, gives it its own
OPTIONS row) open it, a host overlay drawn after the game's frame (`gfx/settings_ui.cpp`) with the
shared overlay kit (`gfx/overlay_kit.*`, moved out of the MODS library unchanged: the game's
deep-blue glass, crystal chrome, Saira type, red/gold focus). It replaces the Apt DISPLAY OPTIONS
page (`kernel/display_menu.cpp`; `SR_PC_SETTINGS=0` restores that page for a developer A/B). The
overlay routing (open request, keyboard / mouse queue, the neutral pad the game reads while it is
open, the release latch) is the MODS library's (`kernel/mod_library.hpp`, `OverlayPage::Settings`).

- **Tabs** GAME, CONTROLS, DISPLAY, GRAPHICS, WORLD, AUDIO, CHEATS (LB / RB, Tab / Shift+Tab, or click; every open
  starts on the first one shown). CHEATS is listed in game only. The keyboard keys follow the PC controls
  (docs/PC_CONTROLS.md 7.1).
- **Rows** are real controls: drop-downs (A / Enter / click opens the list; LEFT / RIGHT step in
  place), sliders with their value (LEFT / RIGHT step, drag, click on the track), checkboxes, and
  buttons. Restart-only rows carry a RESTART badge (red when a change is waiting for one);
  no dead controls: a row another lane implements is not listed until that lane registers it
  (`pcsettings::setAvailable`; a tab with no rows is not shown), and a row unavailable for a
  reason of the moment (one monitor; refresh rate outside exclusive fullscreen) is shown
  disabled with that reason.
- **Description pane**: what the option does, its performance cost (bars), when it applies (at
  once as a preview / on APPLY with keep-or-revert / after a restart / next Metropolis load), what
  the Xbox 360 did, the default, and the applied value while a change is pending.
- **APPLY** (Y / R / START / button) stores every changed row; **REVERT** (VIEW / button) drops
  them; **RESET TO DEFAULTS** (X / Space / button, confirmed) sets the current tab's defaults
  (APPLY keeps them). Live rows preview at once; leaving with unapplied changes asks APPLY /
  DISCARD / CANCEL.
- **Display changes** (mode, monitor, resolution, refresh rate) apply on APPLY behind a 15 s
  **KEEP THESE DISPLAY SETTINGS?** countdown; no answer reverts them.
- **Restart-only** changes end with RESTART NOW / LATER (the MODS menu's relaunch).
- **Hold to preview** (RT, Page Down - RT's key, see `docs/PC_CONTROLS.md` 7.1 - or hold the eye button;
  P in automated runs' test keymap): the screen fades so the game shows.
- Prompts follow the device (the input lane's glyph set; `SR_MOD_LIBRARY_PROMPTS` pins one).

Offscreen look checks (no game process): `build/difftest.exe --settings-render <test root> <out
dir> 1920x1080 <background png|-> "<script>"` (the mod library harness; the test root becomes
`SR_CONTENT_DIR`, so its settings files are written there). Unit tests: `build/difftest.exe
--gfx-settings-test`.

### 1.1 The GAME and CHEATS tabs (lane E L-b; port features only)

- **GAME** = the game's own options: INVERT LOOK (Y AXIS), AUTO-CENTRE CAMERA, VIBRATION, CONTROL SCHEME (the retail
  GAMEPLAY / CONTROLLER OPTIONS rows), MUSIC VOLUME, SOUND EFFECTS VOLUME (AUDIO OPTIONS, 0-10), plus AUTOSAVE WITHOUT
  ASKING (the port's mod option, `settings/mod_options.ini`), and in DEV BUILDS ONLY a DEV TOOLS row (compiled out of
  the release composition with `SR_ORACLE`; it opens the Dev Tools pages from the opener, back to the MAIN MENU or the
  pause menu's MORE OPTIONS panel). `kernel/game_options.hpp`:
  - **The values are the game's.** The movie that opens the screen (`srfrontend.aptasm` OpenOptions,
    `srpause.aptasm` OpenDisplay) first sends what the game's own SettingsAdapter getters return
    (`fscommand("SR_GameOptionsLive", "<FE|IG> <packed>")`, packed as `kernel/mod_bridge.hpp`), then the open request.
    The volumes are the game's only while its audio mixer exists; before it the two volume rows are shown disabled.
  - **A change goes through the game's own setters.** On the next Apt pump (the game's thread, the interpreter idle,
    as the mouse cursor's calls) the host calls the opener's `SR_GameOptions(packed, stored)`
    (`game_options_fn.aptasm`), which runs the custom front end's ApplyStore: the game's own setters, only where the
    live value differs (logged `apply why=3 store=... now=... match=...`). A live preview changes the game's
    options at once; APPLY first stores them in the universal store `settings/game_options.ini` with the same
    function an options screen's capture uses (`modSettingsSavePacked`), so the next NEW GAME / load re-applies
    them; REVERT and a close without APPLY send the stored value back. Logged `[GAME_OPTIONS]`.
  - The retail GAMEPLAY / CONTROLLER / AUDIO OPTIONS screens stay reachable from the pause menu until L-c; both
    paths change the same values (the screens' exit capture keeps the store and the GAME tab in step).
- **CONTROLS** gains **VIEW PAD LAYOUT**: the game's own controller diagrams (the retail CONTROLLER OPTIONS
  screen's); the screen closes and the opener shows them (`pad_layout_fn.aptasm`).
- **CHEATS** (in game only) = the game's own cheats, the rows of the pause menu's CHEATS page
  (`kernel/cheats_menu.hpp`: UNLOCK ALL SUITS, the ON / OFF switches, DISGUISE). A row's button queues the cheat;
  the next UI tick applies it through the game's own functions, or the game refuses it (between missions, after
  Warworld) and the rows' description says so. E15 fills this tab with the full list.

## 2. The options

| Tab | Option | Key (store) | Values | Default | Applies | Owner |
|---|---|---|---|---|---|---|
| DISPLAY | DISPLAY MODE | `display_mode` (display.ini) | windowed / borderless / exclusive | borderless (players) | keep-or-revert | existing |
| | MONITOR (listed with 2+ monitors) | `display_index` | 0..n-1 | 0 | keep-or-revert | gfxset |
| | RESOLUTION / WINDOW SIZE | `output_width/_height` / `window_width/_height` | the monitor's modes / window sizes | first start: the desktop | keep-or-revert | existing keys |
| | REFRESH RATE | `output_refresh` | AUTO (0) or the mode's rates | 0 / first start: the monitor's max | keep-or-revert (exclusive) | gfxset |
| | RENDER RESOLUTION | `res_scale` | AUTO, or pixel sizes 1280x720 .. 8960x5040 | auto | restart | res4k |
| | UPSCALING | `upscale_filter` | bilinear / cas / fsr (FSR 1.0) | bilinear | live | res4k (stage scale) |
| | SHARPNESS | `sharpness` | 0..100 | 0 | live | res4k (stage sharpen) |
| | FRAME RATE LIMIT | `target_fps` | 30 (ORIGINAL), 60, 120, 144, 165, 240, display, unlimited | monitor | live | highfps |
| | VSYNC | `vsync` | on / off | on | live | highfps |
| | BRIGHTNESS | `brightness` | -50..50 | 0 | live | gfxset (stage colour) |
| | GAMMA | `gamma` | 0.50..1.50 | 1.00 | live | gfxset (stage colour) |
| | PERFORMANCE OVERLAY | `perf_overlay` | off / fps / full | off | live | gfxset |
| GRAPHICS | PRESET | (computed) | ORIGINAL, LOW, MEDIUM, HIGH, ULTRA (+DEFAULT, CUSTOM) | DEFAULT | per row | gfxset |
| | ANISOTROPIC FILTERING | `anisotropic` | off, 2, 4, 8, 16 | off (the game's own) | live | gfxset |
| | TEXTURE LOD BIAS | `lod_bias` | -2.00..2.00 | 0.00 | live | gfxset |
| | POST-PROCESS AA | `post_aa` | off / fxaa / fxaa_extreme | off | live | gfxset (stage post) |
| | XBOX 360 EDGE BLUR | `edge_blur` | on / off | on | live | gfxset (game draw left out) |
| | BLOOM | `bloom` | 0..200 % step 10 | 100 | live | gfxset (game constant) |
| WORLD | POPULATION / TRAFFIC DENSITY | `population_density`, `traffic_density` (game_options.ini) | 25..`kDensityMax` step 25 % | 100 | next Metropolis load or restart | density |
| AUDIO | MASTER VOLUME | `audio_volume` | 0..100 % | 100 | live | gfxset |
| | MUTE IN BACKGROUND | `audio_bg_mute` | on / off | off | live | gfxset |
| | OUTPUT DEVICE | `audio_device` | default / the SDL devices | default | restart | gfxset |
| CONTROLS | BUTTON PROMPTS | `glyphs` (input.ini) | auto, xbox, ps4, ps5, switch, keyboard | auto | on APPLY (reload) | input lane's key |
| | MOUSE LOOK / SENSITIVITY / INVERT X / Y | `[mouse] look, sensitivity, invert_x, invert_y` | | on / 1.00 / off / off | on APPLY (reload) | controls lane's keys |

Every `display.ini` graphics key has an `SR_GFX_*` env override for replays and rolls
(`SR_GFX_POST_AA`, `SR_GFX_BRIGHTNESS`, `SR_GFX_GAMMA`, `SR_GFX_UPSCALE`, `SR_GFX_SHARPNESS`,
`SR_GFX_ANISO`, `SR_GFX_LOD_BIAS`, `SR_GFX_BLOOM`, `SR_GFX_EDGE_BLUR`). Lanes that implement a row register it:
`pcsettings::setAvailable(id, true)`, `onLive(id, fn)`, `onAction(id, fn)` (`kernel/pc_settings.hpp`).

**Presets** (managed rows only; upscaling / sharpness only once the res4k lane registers them; brightness, gamma, LOD bias, density, audio and controls are
never touched):

| | ORIGINAL | LOW | MEDIUM | HIGH | ULTRA |
|---|---|---|---|---|---|
| render resolution | 1280x720 | 1280x720 | auto, at most 2560x1440 | auto | one step above auto |
| upscaling / sharpness | bilinear / 0 | bilinear / 0 | bilinear / 0 | FSR / 20 | CAS / 30 |
| frame rate | 30 | 60 | display | display | display |
| anisotropic | off | off | 4x | 16x | 16x |
| post AA | off | off | FXAA | FXAA | FXAA extreme |

**First start** (owner 2026-10-05): with no display.ini a player's launch detects the primary
monitor (Win32: its desktop mode, and the highest refresh it offers at that size) and writes
`display_mode = borderless`, `output_width/_height`, `output_refresh`, `res_scale = auto`,
`target_fps = monitor` (the display's refresh) (log line `[DISPLAY] first start: detected ...`). Automated runs
(`SR_INSTANCE_ID`) never write it. An old display.ini with `res_scale = N` keeps working; the
screen shows N as its pixel size.

## 3. How each host option works

- **The present chain** (`gfx/d3d12_present_chain.cpp`): ordered host stages between the scan-out
  (the front buffer at the internal resolution through the game's display gamma ramp) and the
  swap chain, in Xenia's presenter order: **post** (internal res) -> **colour** (internal res) ->
  **scale** (output rectangle) -> **sharpen** -> **fit** (letterbox, the pre-chain pass 2). A stage
  at its default records nothing and fit reads the scan-out, so the presented pixels are the
  pre-chain ones. Helpers for stage authors: chain targets (RTV + SRV + optional UAV), a PSO
  helper on the chain root signature (b0 16 constants, t0, s0 linear / s1 point clamp), a
  one-pass helper. The replay dump runs the internal-res stages, so they are A/B-able on replay.
- **Post AA**: Xenia's FXAA as Xenia runs it - a luma pass (`apply_gamma_*_fxaa_luma`: luma =
  dot(rgb, .299/.587/.114) into an R16G16B16A16_UNORM alpha) then Xenia's precompiled
  `fxaa_cs` / `fxaa_extreme_cs` with Xenia's root signature. NVIDIA FXAA 3.11 (BSD-3), credited.
- **Brightness / gamma**: after the game's own ramp, out = in^(1/g), then blend toward white
  (k > 0) or scale toward black (k < 0).
- **Anisotropic / LOD bias**: the PM4 path's sampler heap holds 8 blocks of the 16-sampler table;
  block 0 is the iter-411 table byte for byte (the game's own filtering); an option change writes
  the next block of a 1..7 ring (never the one in use) and every draw binds `xlatSamplerTable()`.
  OFF is the Xbox 360 look: the gameplay trace (`454107ED_25023`, `xtr_draws --textures`) asks for
  trilinear with `aniso_filter = 0` on its main-scene textures.
- **Bloom / edge blur**: the gameplay post chain is identical in every gameplay trace (gp1, gp2,
  lg1, lg2, 16099): luminance reduction at pitch 80, bright pass and blur at 320 and 160, then the
  tone map `ps 2679C172F83DE85D` and an edge blur `ps DB6A2996BA148383`. Their ucode
  (`xtr_draws.py --dump-ucode` + `xenia-gpu-shader-compiler --shader_output_type=ucode`):
  tone map `colour = scene + tf2 * c1.x` (tf2 = the 160x90 bloom) then Reinhard on the 1x1
  luminance; edge blur `colour = centre + c255.x * w * (weighted 3x3 mean - centre)`.
  BLOOM scales c1.x in the host copy of the tone map draw's pixel constants
  (`gfxopt::patchPsConstants`, right after the upload); the guest's registers are untouched and
  at 100 % nothing is written. The bloom passes still run (their cost is the game's).
  EDGE BLUR off leaves the edge blur draw out (`gfxopt::skipDraw`, colour draws only). Its c255 is
  a literal of the shader's definition table, which the translator compiles in, so there is no
  constant to scale (a first version zeroed c255.x in the buffer and the replay showed no change).
  In gp1 the tone map #3843 writes base000, resolve #3844 copies it to 1D804000, the edge blur
  #3845 samples 1D804000 and rewrites all of base000, resolve #3846 copies it back: without #3845
  base000 keeps the tone map's output, which is exactly strength 0.
- **Master volume / mute**: a gain on the SDL output after the guest's mix (1.0 = untouched).

## 4. Not possible, and not built yet

- **FOV / ultrawide, draw distance, LOD distances**: game decisions (camera, CPU-side culling);
  no retail setting exposes them; a host change would have to write guest state. Not offered.
- **Depth of field, film grain**: the game has neither (its post plugins are Bloom, MotionBlur,
  Bulge, FastTint, FlexibleTint, Gloom).
- **The game's own MSAA**: the main scene is 1X; the 640x640x4 16_16 pass (likely the shadow
  cascades, 1062 draws / frame in gp1) asks for 4X and our PM4 path draws guest MSAA at 1X.
  Real MSAA requires native multisampled render targets and correct resolves before exposing a player setting.
- **Shadow-map resolution**: the render resolution already scales every render target, the
  shadow array included; a separate per-target scale is not safe.
- **Motion blur / screen distortion toggles**: the same method as bloom or edge blur (find the
  plugin's composite draw, scale its weight or leave the draw out), but no trace holds them yet
  (fast flight, heat vision), so they are not offered. A trace of those moments is the next step.
- **Shader precompile / background compiling / clear cache**: the stutter lane found that PSO
  creation is not a stutter cause and added no such settings, so the screen has none.

### 4.1 Deferred modern graphics expansion (owner approval, 2026-10-11)

After native conversion, correctness, existing-feature compatibility and menu navigation are proven,
the owner wants MSAA, ambient occlusion, DLSS and modern FSR. See RELEASE_PLAN phase 6B for sequencing and
acceptance. Section 4 describes current limitations; it does not rule out new, opt-in port features.

- **AA:** retain FXAA; add real MSAA where compatible native passes support it. Temporal AA / native-resolution
  DLAA is a candidate once temporal inputs are correct. Offer supported AA / upscaling combinations.
- **Ambient occlusion:** OFF / SSAO or a modern method such as GTAO / CACAO, plus quality and strength.
  SSAO is an AO technique, not a second effect to stack. Apply it to the world without darkening UI or previews.
- **Upscaling:** distinguish existing spatial FSR 1.0 from planned temporal FSR and DLSS Super Resolution.
  Temporal work needs valid scene depth, camera / object motion vectors, jitter, exposure and transparency masks;
  fast flight, animated capes, water and particles are required quality checks. Handle HUD / movies separately,
  isolate costume-preview history and reset histories when scenes or relevant settings change.
- **Further candidates:** dynamic resolution, HDR output calibration, independent shadow quality, motion-blur /
  distortion controls, FOV / ultrawide and view-distance / LOD options. XeSS, reflection and latency controls
  need feasibility review; ray tracing remains separate research. Refine existing bloom, sharpening and
  anisotropic controls rather than duplicating them.

All enhancements use the native renderer; the release executable excludes emulation components. The original
look remains available, existing PC features remain compatible, and unsupported modes explain their requirements.
Keep APPLY / REVERT, persistence, clickable actions and adaptive input glyphs consistent across all new controls.
Measure actual rendered-frame performance and VRAM; frame generation remains excluded. No settings ship as stubs.

Primary references for the eventual design: [NVIDIA DLSS](https://developer.nvidia.com/rtx/dlss),
[AMD temporal FSR integration](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)
and [AMD CACAO](https://gpuopen.com/fidelityfx-cacao/). Select SDK versions and hardware coverage during design.

## 5. Proof

### 5.1 Replay (deterministic; `tools/replay_batch.py`, evidence in the worktree's `archive/gfxset/r1`)

Exes: `base` = dev b33d266e, `hook` = 1db302f7, `f4` = 405bd32b (every option and the screen),
`f5` = 3b44b5fa. Each job replays the traces through `tools/replay_ab.ps1` (`pm4_replay_score.py`
table); `compare` checks byte identity and writes side-by-sides with a x8 diff map
(`<a>_vs_<b>_<trace>.png`). Default dump = the replay's frame; `out` = `SR_REPLAY_DUMP_OUTPUT=1`,
the scan-out dump path that runs the internal-resolution chain stages.

Defaults (every option at its default must reproduce dev):

| A vs B | traces | result |
|---|---|---|
| base vs hook | menu, eahd, t16, t21, t25, gp1, gp2, lg1, lg2 | 9/9 byte-identical |
| baseout vs hookout | same 9 | 9/9 byte-identical |
| base vs f4 | same 9 | 9/9 byte-identical |
| baseout vs f4out | same 9 | 9/9 byte-identical |
| base vs f5 / baseout vs f5out | same 9 | 9/9 / 9/9 byte-identical |
| merged dev (t7 = 92017f6d + the M19 fix: tick, stutter, density, gfxset) vs f5 and vs base, both modes | same 9 | 9/9 each (4 x 9/9); bloom 0 % and FXAA on gp1 identical to f5's |

Each option against the default (must change the picture, and only as described):

| option (env) | vs | traces | MAD (0..255) | pixels changed |
|---|---|---|---|---|
| anisotropic 16 (`SR_GFX_ANISO=16`) | f4 | menu, gp1, gp2, lg1, lg2 | 2.11, 1.91, 1.33, 0.60, 0.65 | 53..74 % |
| anisotropic 4 | f4 | gp1, lg1 | 1.86, 0.59 | 71, 55 % |
| LOD bias -1.00 / +1.00 | f4 | gp1, lg1 | 2.02, 0.70 / 1.98, 0.85 | 56..73 % |
| FXAA (`SR_GFX_POST_AA=fxaa`) | f4out | menu, gp1, gp2, lg1, lg2, eahd | 0.70, 0.24, 0.37, 0.05, 0.10, 0.09 | 1..10 % (edges) |
| FXAA extreme | f4out | menu, gp1, lg1 | 1.16, 0.49, 0.13 | 5..20 % (edges) |
| brightness +20 | f4out | menu, gp1, lg1 | 27.4, 40.3, 44.9 | 99 % |
| gamma 1.20 | f4out | menu, gp1, lg1 | 13.1, 14.4, 11.4 | 98..100 % |
| gamma 0.80 + brightness -10 | f4out | gp1 | 19.4 | 100 % |
| bloom 0 % (`SR_GFX_BLOOM=0`) | f4 | gp1, gp2, lg1, lg2, menu | 19.8, 20.2, 4.6, 4.5, 46.8 | 92..100 % |
| bloom 200 % | f4 | gp1, lg1 | 20.2, 4.7 | 99.7..100 % |
| edge blur off, first version (c255.x zeroed) | f4 | gp1, gp2, lg1, lg2 | 0 | 0 % - no effect, rebuilt as a left-out draw |
| edge blur off (`SR_GFX_EDGE_BLUR=off`, draw left out) | f5 | gp1, gp2, lg1, lg2, menu | 0.54, 0.62, 0.28, 0.50, 0.18 | 2..9 % (edges) |

Looked at, not only measured: anisotropic 16x sharpens the street-level wall and pavement textures
at a grazing angle (gp1); FXAA smooths the building and hair silhouettes (gp1, 2x crop); bloom 0 %
removes the game's haze (the menu city loses its glow, the S shield and text are drawn after the
post chain and do not change); edge blur off leaves the building edges and the ear outline
stair-stepped and crisp (gp1, 2x crop).

### 5.2 Live (background PORT FEATURE rolls; evidence in the worktree's `archive/gfxset/`)

Repro: `archive/gfxset/live_batch3.ps1` = `tools/sr_roll.ps1 -Owner gfxset -Scenario
tools/scenarios/gfxset_settings.json -PortFeatures 1 -Gates "SR_KB_PAD_OBS=1,SR_PM4_GPU_TIME=1,
SR_PRESENT_CHAIN_TIME=1,SR_HARNESS_VPAD=1,SR_MOD_LIBRARY_SHOTS=<dir>,SR_WINDOW_SIZE=1920x1080"
-DumpEvery 30` with `python tools/glyphs/vpad_hotplug.py --plan gfxset --log <run log>` beside it.

**Roll 3 (`live3`, exe = 5aeecd0e build, dev 92017f6d + the gfxset follow-ups): SUCCESS.**
- `[AUTOMATION] automated=1 ... realInput=0`; window shown without activation at the bottom of the
  z-order. The pad tour's ViGEm Xbox 360 pad: `[INPUT] vpad filter: ACCEPTED ... <- ROOT\SYSTEM\0007
  [ViGEmBus]`; no other controller was opened.
- MAIN MENU > SETTINGS > DISPLAY OPTIONS -> `[PC_SETTINGS] shown (FE, 1920x1080)`. Keyboard tour
  (key-cap prompts): WINDOW SIZE list, size change + APPLY -> KEEP THESE DISPLAY SETTINGS? countdown
  -> REVERT; GRAPHICS previews (anisotropic, FXAA) with the unsaved banner, APPLY, RESET TO DEFAULTS?
  (opens on CANCEL); WORLD (the density lane's two sliders); AUDIO. Mouse tour (the port's cursor):
  click opens the anisotropic list and picks 2X, LOD drag, APPLY button, tab clicks, wheel. Pad tour:
  player 1 = xbox, Xbox prompts, RB tabs, D-pad rows, A list, Y apply, B close.
- NEW GAME -> HUD; PAUSE > MORE OPTIONS > DISPLAY OPTIONS -> `[PC_SETTINGS] shown (IG, 1920x1080)`;
  every option held for its cost window; B -> DISCARD -> `revert`, `close`.
- No `[DISPLAY] first start` line (an automated run never writes display.ini). 0 `[ICALL_UNRESOLVED]`
  / `[ICALL_FAIL]` lines (ICALL_FAIL ends the process; the run ended at its scenario's end).
  `scripts/audit_forcing.py`: UNGATED 2 (unchanged).
- Frames: `live3_ui_sheet.png` (the 26 composited F12 shots), `live3_conditions.png` (the present dump
  of every in-game condition, full frame + 2x crop), `live3_verify/` (whole run by `tools/verify_run.py`:
  menu, settings, NEW GAME, the opening FMV, gameplay with the HUD, the pause menu - all viewed).

Rolls 1 and 2 were stopped by the scenario, not the screen: roll 1 expected exclusive fullscreen
after one RIGHT from the roll's windowed start (a roll now stays windowed and proves keep-or-revert on
the window size; roll 1 showed the 15 s countdown reverting by itself); roll 2's pad started on the
tab's remembered row (the pad plan now goes to the top row first). Roll 1's slot session also ran the
merged-trunk replay (5.1).

**GPU cost, live** (roll 3, in game behind the pause menu: the paused scene at the AUTO render
resolution 2560x1440 on a 1920x1080 window; `[PM4_GPU_TIME]` = the whole frame's GPU time,
`[PRESENT_CHAIN_TIME]` = the host chain stages; averages over the windows' 120-present lines that do
not straddle a change):

| condition | frame GPU ms | chain stage ms |
|---|---|---|
| post AA off / FXAA / FXAA extreme | 7.61 / 7.68 / 7.69 | post 0.000 / 0.068 / 0.077 |
| brightness +15, gamma 1.20 | 7.64, 7.63 | colour 0.017 |
| anisotropic off / 2x / 8x | 7.67 / 7.70 / 7.71 | - (sampler state) |
| LOD bias -2.00 / -1.00 | 7.63 / 7.62 | - |
| bloom 0 % / 100 % | 7.60 / 7.67 | - (a constant) |
| edge blur off / on (2 and 1 samples) | 7.45 / 7.62 | - (one draw left out) |
| fit (letterbox, always) | | 0.025 - 0.029 |

Every option costs under 0.1 ms; run-to-run noise of the frame figure is about 0.05 ms.

**Live effect on the picture** (the same roll's present dumps; the paused 3D scene left and right
of the pause panel, which does not move while paused; MAD on 0..255, % of pixels > 2 apart):

| A vs B | MAD | changed |
|---|---|---|
| null control: two windows with the same settings | 0.005 | 0.0 % |
| anisotropic off vs 8x | 0.17 | 1.1 % |
| FXAA vs off / extreme vs FXAA | 0.03 / 0.05 | 0.2 / 0.3 % |
| edge blur off vs on | 0.16 | 1.9 % |
| bloom 0 % vs 100 % | 1.34 | 11.7 % |
| LOD bias -2.00 vs -1.00 | 0.09 | 0.6 % |
| brightness +15 vs 0 | 36.0 | 100 % |
| gamma 1.20 vs 1.00 | 7.8 | 99 % |

### 5.3 Second opinion

Astra, blind, on ten live frames of roll 2 (keyboard, mouse, pad): "visually cohesive ... no obvious
text clipping, overlapping labels or off-screen controls", not ready to ship until the input guidance
was fixed. Fixed in 5aeecd0e (seen in roll 3 and offscreen `archive/gfxset/ui7`): the countdown no
longer names a pad button on the keyboard; the prompt bar stays bright above a modal; D-pad = MOVE;
APPLY is prompted only when something is pending; HOLD TO PREVIEW only on DISPLAY and GRAPHICS; the
reset confirmation opens on CANCEL; restart rows read APPLY SAVES IT - AFTER A RESTART; the drop-down
is opaque; FXAA / window size / audio texts say what the player gains and pays. Kept on purpose: the
APPLY / RESET buttons and their prompts (buttons for the mouse, prompts for keys and pads). Open (not
this lane): the mouse cursor stays drawn after a pad press (the mouse lane's cursor overlay).

### 5.4 Still open
- Motion blur and screen distortion toggles need a trace of those moments (section 4).
- UPSCALING / SHARPNESS appear when the res4k lane registers them.
- The live cost table is one roll on one GPU with the game paused; gameplay costs scale with the
  render resolution (the chain stages run at the internal resolution).
