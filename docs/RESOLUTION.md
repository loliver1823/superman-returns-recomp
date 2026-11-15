# Resolution: internal draw scale and output (PORT FEATURE, iter-464)

**USER DECISION 2026-10-02:** "prioritise framerate and resolution ... target 4k but support all
resolutions." The Xbox 360 renders Superman Returns at 1280x720. This port renders it at an
integer multiple of that per axis (the *internal* resolution) and fits the result to any window
or screen (the *output*). 3x = 3840x2160 = native 4K.

This is a **port feature**: it changes how the host draws, never what the game decides. The
guest still sees its own 1280x720 world - fetch constants, viewport registers, resolve
rectangles and every byte it reads back are guest pixels. Nothing here writes guest state.

## Settings

| Key (`settings/display.ini`) | Env override | Values | Default |
|---|---|---|---|
| `res_scale` | `SR_RES_SCALE` | `auto`, `1`..`7`, or `<x>x<y>` (e.g. `2x3`) | see "Defaults" |
| `display_mode` | `SR_DISPLAY_MODE` | `windowed`, `borderless`, `exclusive` | see "Defaults" |
| `window_width` / `window_height` | `SR_WINDOW_SIZE=<w>x<h>` | windowed client size in physical pixels | 1280 x 720 |
| `output_width` / `output_height` | `SR_OUTPUT_SIZE=<w>x<h>` | exclusive-mode size (0 = desktop) | desktop |
| `upscale_filter` | `SR_GFX_UPSCALE` | `bilinear` (today's fit), `cas`, `fsr` - the present chain's *scale* stage | `bilinear` |
| `sharpness` | `SR_GFX_SHARPNESS` | `0`..`100` (0 = off) - the *sharpen* stage (CAS / RCAS) | `0` |

`upscale_filter` and `sharpness` (iter-466, agreed with the graphics-settings lane) are the res4k
lane's stages on the gfxset present-chain hook (`gfx/d3d12_present_chain.cpp`: post -> colour ->
scale -> sharpen -> fit); the PC SETTINGS screen (docs/GRAPHICS_SETTINGS.md) exposes them. The
settings UI shows real output resolutions (1920x1080, 2560x1440, 3840x2160, ...), never "1x/2x":
the internal scale is derived from the output (`auto`, below).

- `display.ini` lives beside `game_options.ini` in the save root's `settings/` folder
  (`kernel/xam_content` `xamContentRoot()`, normally `%LOCALAPPDATA%/SupermanRecomp/content`).
  iter-465 polish: the game's own menus set every key - MAIN MENU > SETTINGS > DISPLAY OPTIONS,
  or in game PAUSE > MORE OPTIONS > DISPLAY OPTIONS (docs/CUSTOM_FRONTEND.md 5.9); the ini stays
  the power-user surface (the same store). The scale is read once at start: a change takes
  effect on the next launch (as in Xenia) - the page marks it `- RESTART` and offers RESTART
  NOW. Display mode, window / output size, frame rate and vsync apply at once
  (`resscale::requestLiveDisplay` -> `PlatformSDL::applyLiveDisplay` on the window's thread;
  Alt+Enter writes `display_mode` too).
- **Auto** = the smallest integer scale whose internal 16:9 picture covers the picture the
  present fits into the output (iter-466: the fitted rectangle, not the raw output, so ultrawide
  and 32:9 screens do not pay for pillarbox bars): 2160p -> 3x, 1440p -> 2x, 1080p -> 2x
  (downsampled to 1080), 1366x768 / 1600x900 -> 2x, 2560x1080 -> 2x, 3440x1440 -> 2x (was 3x),
  5120x1440 -> 2x (was 4x), 720p -> 1x. Auto stops at 4x; an explicit value can ask for up to 7x.
- **Non-integer scales** (e.g. 1.5x = native 1080p) are not supported: Xenia's resolution scale is
  an integer per axis, and the scaled resolve memory and scaled texture loads address guest bytes
  x s^2 with s x s host texels per guest texel; resolve rectangles, viewports and scissors at odd
  guest coordinates have no exact 1.5x position. 1080p uses 2x and downsamples (exact area filter).
- **Alt+Enter** toggles windowed / borderless fullscreen. A windowed window can be resized or
  maximised; the frame is refitted on the next present.
- Automated runs (`SR_INSTANCE_ID`, every roll harness) are always windowed (unless
  `SR_DISPLAY_MODE` asks), never take the foreground (`kernel/host_automation.hpp`), and ignore
  Alt+Enter from the real keyboard.
- The process is per-monitor DPI aware, so window sizes are physical pixels: a 4K desktop at
  150% gets a true 3840x2160 swapchain in borderless mode, not a stretched 2560x1440 one.
- **SDL3 (RELEASE_PLAN step 3, 2026-10-18) keeps SDL2's display semantics** (`app/platform_sdl.hpp`
  `sdl3platform`, `difftest --sdl3-platform-test`): SDL3 is per-monitor-v2 aware by itself and its
  Windows window units are pixels; `display_index` stays an index (SDL3's display list, primary
  first, as SDL2's); refresh rates stay SDL2's integers: Windows' own `dmDisplayFrequency`, read from
  the DEVMODE SDL3 keeps with each mode (the build worker: 18 of 18 modes, cross-checked with
  `EnumDisplaySettingsW`), else SDL3's float by SDL2's rule (NTSC 59.94 reads 59, a DXGI-refined 59.95
  reads 60); the exclusive mode is chosen by SDL2's rule
  (the smallest mode at least the asked size, then the LOWEST rate >= the asked rate, else the
  highest) rather than SDL3's nearest-rate rule; no mode that large = exclusive at the desktop
  mode, as SDL2 did. Borderless = SDL3 fullscreen with no fullscreen mode; exclusive =
  `SDL_SetWindowFullscreenMode` with the chosen mode. Windows are created hidden, set up, then
  shown, so an exclusive start never flashes a window first.

## How it works (Xenia's draw resolution scale, BSD-3)

The port follows `reference/xenia-canary/src/xenia/gpu` `draw_resolution_scale_x/_y`
(`texture_cache.cc`, `render_target_cache.cc`, `draw_util.cc`, `d3d12_render_target_cache.cc`,
`d3d12_texture_cache.cc`, `d3d12_command_processor.cc`). Code: `gfx/res_scale.*`, the PM4 path in
`gfx/d3d12_pm4_edram.cpp` / `gfx/d3d12_pm4_shmem.cpp` / `gfx/d3d12_backend.cpp`.

| Stage | At scale s (per axis) | Xenia source |
|---|---|---|
| Scale choice | Read once; clamped to the device (tiled resources, VA bits per resource for 512 MiB x area, the 16384 texture limit) and to the GPU EDRAM + shared-memory path; frozen before the first draw | `texture_cache.cc:161-196`, `d3d12_command_processor.cc:841-858` |
| Render targets | Host RTs are s x the guest size; RT heights clamp to the host limit in scaled rows | `render_target_cache.cc` `GetRenderTargetHeight` |
| EDRAM | The EDRAM buffer is area x larger, with `edram.xesli`'s scaled tile layout in our store/load shader | `edram.xesli` |
| Viewport / scissor | Host pixels; limits computed in guest pixels / s | `draw_util.cc` `GetHostViewportInfo` |
| Depth bias | Slope-scaled bias x max(sx, sy) | `d3d12_command_processor.cc` |
| Point sprites | Diameter-to-NDC radius x s / extent | `d3d12_command_processor.cc:3983-3988` |
| Resolves | Xenia's `*_scaled` resolve compute shaders write a reserved 512 MiB x area buffer (16 MiB heaps mapped on use); the 1x shared memory is not written; pages are marked scaled and unmarked when the CPU rewrites them | `d3d12_render_target_cache.cc:1351-1416`, `d3d12_texture_cache.cc:926-1191`, `texture_cache.cc:1052-1098` |
| Textures | A tiled texture with a scaled loader whose pages a scaled resolve wrote is a separate s x resource loaded by `texture_load_*_scaled` (2D, arrays, cubes) | `texture_cache.cc:597-620` (`scaled_resolve` key), `d3d12_texture_cache.cc:1528-1771` |
| Pixel position | The Xenos param-gen pixel position is `floor(SV_Position) * (1 + bias)`, bias = 1/s - 1 (root constants b6 space4; `tools/xenosrecomp/0003` patch; exactly 8 cached pixel shaders changed) | Xenia's DXBC translator pixel position |
| Present | Pass 1: scan the front buffer out at the internal resolution (scaled addressing) into an output texture, through the gamma ramp. Then the present chain (`gfx/d3d12_present_chain.cpp`: post -> colour -> scale -> sharpen -> fit). Fit places the image in the backbuffer, aspect kept (letterbox / pillarbox on black): 1:1 copy, bilinear enlarge, or an exact area filter when shrinking | `presenter.cc` (letterbox, the guest-output effect flow) |
| Upscale / sharpen (iter-466, `upscale_filter`, `sharpness`) | Xenia's guest-output effects with its precompiled pixel shaders: CAS resample / sharpen, FSR 1 EASU (x2 per pass, up to 4 passes) + RCAS, bilinear for the rest. The table under "Upscale filter" says which runs when. Defaults (bilinear, 0) record nothing | `presenter.cc` `BuildGuestOutputPaintFlow`, `presenter.h` Cas/Fsr constants; AMD FidelityFX CAS / FSR (MIT) |
| Native scaled resolve (iter-466, `SR_NATIVE_RESOLVE_SCALED`, default ON; =0 keeps the A/B) | A resolve reads the source host RT (already s x s per guest pixel) and writes the scaled resolve memory with Xenia's `XE_RESOLVE_RESOLUTION_SCALED` addressing - no EDRAM store before it. Same-shape single owners whose rectangle starts in the first tile; mixed spans and later origins keep the EDRAM path | the N4a kernels (`gfx/native_resolve.inc`) built with Xenia's scaled resolve define |
| Direct transfer (iter-466, `SR_PM4_XFER_DIRECT`, default ON; =0 keeps the A/B) | A colour -> colour EDRAM ownership transfer computes the destination's EDRAM dwords from the source host RT with the store's own pack (one pass), instead of a store into the EDRAM buffer and a load back. Depth transfers keep the EDRAM path | `d3d12_render_target_cache.cc` host-RT transfers (one pass from the source RT) |
| Depth transfers direct (round 2, `SR_PM4_XFER_DEPTH`, default ON) | Transfers with a depth source and / or destination go direct too: a depth source's planes are read through SRVs (the unorm24 bits recovered exactly by an integer float -> UNORM conversion - no whole-plane copy, no EDRAM store); a depth destination in two steps - the dwords into a scratch R32_UINT once, then the load's depth pass + one pass per stencil bit read it | `d3d12_render_target_cache.cc` host depth transfers |
| Stencil-bit predication (round 2, `SR_XFER_DEPTH_PRED`, default ON) | The dword pass records which stencil bits occur in the move (8 predicate qwords); bit pass k runs under `SetPredication(EQUAL_ZERO)`, so a bit no pixel has is skipped (it stays the cleared 0 - exact). NVIDIA has no PS stencil-ref export | - |
| Texture sharing (round 2, `SR_TEX_SHARE`, default ON) | The cache keyed textures by all six fetch dwords, so one resolve target sampled with two sampler states / swizzles was two resources, each reloaded after every resolve. GPU-loaded textures now share one resource per identity (Xenia TextureKey: base, mips, format, size, pitch, tiling, endian, levels, scale); each key keeps its slot + SRV | `texture_cache.h` TextureKey |
| Direct scaled loads (round 2, `SR_TEX_LOAD_DIRECT`, default ON) | A scaled single-level 32 / 64bpp or depth texture (2D or array slice) is written by Xenia's scaled-loader addressing (`texture_load.xesli`, embedded) straight through a UINT view of its typeless family - no scratch buffer, barrier or copy. 16bpp (the shadow map) stays on the scratch path unless `SR_TEX_LOAD_DIRECT_16=1` (live it gained nothing: 8.39 / 8.35 ms with it vs 8.41 / 8.26 without, alternated, 2730 MHz) | `texture_load_*_scaled.cs.xesl` |

**What cannot scale** (Xenia: "no 1:1 mapping"): the CPU EDRAM mirror, the CPU resolve, the CPU
texture decode, the replay's EDRAM snapshot restore and the live-trace EDRAM capture. With any
of those paths selected (`SR_PM4_SHMEM=0`, `SR_PM4_EDRAM_GPU=0`, `SR_PM4_LIVE=0`) the scale
falls back to 1x with a `[RES_SCALE] clamped` line. As in Xenia, the guest CPU does not see the
contents of a scaled resolve in guest RAM.

## Upscale filter and sharpness (iter-466)

Xenia's presenter order and limits (`presenter.cc`): CAS and EASU work up to 2x per axis per
pass; past that, bilinear. RCAS is 1:1. The stages read the post / colour result at the internal
resolution and hand fit an image of the fitted rectangle's size (fit then copies it 1:1).

| `upscale_filter` | enlarge (output larger than internal) | 1:1 | shrink (supersampling) |
|---|---|---|---|
| `bilinear` | sharpness 0: fit's bilinear (today). > 0: Xenia's bilinear to the output size, then CAS sharpen | CAS sharpen if sharpness > 0 | fit's exact area filter, no sharpening |
| `cas` | CAS resample (<= 2x per axis), then bilinear for the rest | CAS sharpen | CAS resample (downsample) |
| `fsr` | EASU (doubling per pass, <= 4 passes) + RCAS | as `cas` | as `cas` |

`sharpness` 0..100 maps to Xenia's CAS additional sharpness 0..1 and to an RCAS sharpness
reduction of 2..0 stops (Xenia's FSR default, 0.2 stops, is sharpness 90). The case for an
upscale filter is a 2x (2560x1440) internal picture on a 4K screen (1.5x enlarge) - cheaper than
3x by the scaled passes' cost (GPU table below).

## Defaults

iter-465 (player lane, docs/PLAYER_BUILD.md): the defaults follow THE PORT-FEATURE SWITCH
(kernel/host_automation.hpp `portFeaturesOn`). A player (the exe started with no
`SR_INSTANCE_ID`) gets `res_scale = auto` and `display_mode = borderless` (the desktop's size;
Alt+Enter = windowed). Automated / natural rolls keep **1x** and a **windowed 1280x720**
window; a PORT FEATURE roll (`SR_PORT_FEATURES=1`) gets `auto` but stays windowed
(app/main.cpp never takes a roll full screen unless `SR_DISPLAY_MODE` asks). Anything set in
display.ini or by `SR_RES_SCALE` / `SR_DISPLAY_MODE` wins.

## Build requirement: the translator with patch 0003

The scaled pixel position (`floor(SV_Position) * (1 + bias)`, b6 space4) lives in the shader
TRANSLATOR (tools/xenosrecomp/0003-iter464-resscale-pixel-position.patch), not only in the 8
cached pixel shaders. The build embeds the clone named by `SR_XENOSRECOMP_DIR` (CMake cache;
default `<repo>/tools/XenosRecomp-clone`, gitignored) for the runtime translator. A clone
without 0003 builds and runs, but every runtime-translated pixel shader that reads the pixel
position is silently wrong at scale > 1. So:
- `python tools/xenosrecomp/verify_clone.py` must report `upstream 990d03b + superman.patch +
  0002 + 0003` and exit 0 (it checks `<repo>/tools/XenosRecomp-clone`);
- configure with `-DSR_XENOSRECOMP_DIR=<that clone>` (an existing cache entry is not changed
  by a reconfigure on its own); `build/generated/xenos_embed.cpp` line 1 names the clone and
  `kTranslatorSourceId` = `75c6643c...` for the 0003 series;
- ship `data/shader_cache/dxil` from the repo (git-tracked; the 8 scaled PS are
  264a28262b73c6c3 3a6876055fec1674 3b15e845131cc1d9 5703f997dd6479c0 9b16c6a8418ae1ab
  9bafe30bdf04f4c5 cb3736b2e52b6285 dbaef5bdc94ab47b). tools/player/make_player_build.ps1
  refuses a build whose clone lacks 0003 and records the hashes in its manifest.
- the clone's OFFLINE `build/XenosRecomp/XenosRecomp.exe` (build_xenos.bat) must be rebuilt
  from the patched sources too: a stale one makes `shader_xlat_test_run.py` T5 report exactly the
  8 scaled PS as differing, and any AOT regeneration would revert them.

## Measuring

- `[RES_SCALE]` (boot): the requested and the effective scale, the source (env / ini / default),
  the display mode and window size. `[DISPLAY]`: the window and any swapchain resize.
- `SR_PM4_GPU_TIME=1` (diagnostic, default OFF): D3D12 timestamp queries around every command
  list of each PM4 frame; every 120 frames one `[PM4_GPU_TIME]` line with the GPU busy time
  (sum of list times), span, lists per frame, the present interval and fps, the internal
  resolution and the backbuffer size.
- iter-466: the same switch also attributes the frame's GPU time per pass: one `[PM4_GPU_PASS]`
  line per 120 frames with ms per frame (and passes per frame) for the game's draws, the EDRAM
  store / load of ownership transfers, resolves (and the EDRAM store inside them), texture
  loads (the scaled ones, and the scratch -> texture copy), scan-out and the present chain.
  It runs on the replay too: `python tools/res_gpu_bench.py --tag <t> --scales 1,3 --traces gp1,lg1
  [--gates ...]` replays a golden frame N times (`SR_GPU_TRACE_REPLAY_LOOPS`) - a like-for-like
  GPU number for an A/B. Hold the game slot, and repeat: another GPU user (a build, another
  lane's process) inflates single runs 2-3x (seen on 2026-10-06). The replay loop is also light
  enough that the driver can hold the GPU in a low P-state for the whole run (2026-10-06: P5 at
  ~500-1400 MHz, every pass ~5x slower - gp1 1x read 30 ms); each row now prints the median
  graphics clock and P-states (nvidia-smi). Compare rows only at the same clock; the absolute
  numbers come from live rolls, where the loaded GPU boosts.
- Round 2 diagnostics (default OFF): `SR_RES_TEXLOG=<n>` logs why each scaled texture load happens
  (key, base, version) and why one took the scratch path; `SR_RES_XFERLOG=<n>` logs each ownership
  transfer's kind (C/D -> C/D) and path (direct / edram); `SR_XFER_DEPTH_NOSTENCIL=1` skips the
  stencil-bit passes (WRONG stencil - only to measure their cost). `res_gpu_bench.py --exe <snapshot>`
  benches another build in the same session.
- Replay at a scale: `python tools/pm4_replay_score.py --tag <t> --scale <s>` scores
  `SR_RES_SCALE=<s>` replays against Xenia's trace dump at the same scale
  (`python tools/xenia_scaled_oracles.py --scale <s> --out archive/v1_resscale/xe<s>`). Xenia does
  not restore the trace's EDRAM snapshot when scaled (`d3d12_render_target_cache.cc:1588`), and
  neither does our replay, so these oracles differ from the 1x goldens on the first frame.

## Evidence

### Replay (deterministic) - mean absolute difference vs Xenia at the same scale

| Trace | 1x (gate: must not move) | 2x vs Xenia 2x | 3x vs Xenia 3x |
|---|---|---|---|
| menu (press start) | 2.06 | 1.86 | 1.84 |
| FMV eahd / t16 / t21 / t25 | 0.00 | 0.00 | <= 0.01 |
| gp1 (city, Xenia trace) | 4.59 | 4.57 | 4.48 |
| gp2 (city, Xenia trace) | 2.87 | 2.73 | 2.74 |
| lg1 (our live gameplay trace) | 5.48 | 5.61 | 5.64 |
| lg2 (our live gameplay trace) | 4.66 | 4.68 | 4.71 |

- 1x: all 9 frames byte-identical (md5) to the pre-change build (`archive/pm4_score/rs_base0`
  vs `rs3_1x` in the lane worktree).
- At 2x/3x the residual is the 1x residual (the same known defects, e.g. lit window panes in
  gp1); a full-resolution crop of lg1 at 3x shows the same detail as Xenia's 3x render, where
  the 1x frame is visibly blocky (`archive/v1_resscale/lg1_3x_crop.png`).
- Before the RT height clamp, 3x scored 24.15 on the city (the 2048-tile EDRAM height x 3
  overflowed the 16384 limit).

### Live rolls (2026-10-06, iter-466, branch render/res4k)

All automated rolls are windowed (or borderless at the desktop size) and never take the foreground;
exclusive fullscreen was never rolled (it would take over the owner's TV). Frames are the GPU
present readback (`SR_PRESENT_DUMP`) at the backbuffer size; evidence under `archive/v1_res4k/`.
Exes: `res4k-f` (gates passed explicitly) and `res4k-g` / `res4k-h` (gates baked), snapshots in the
lane worktree's `archive/exe_snapshots/`.

**Per-era verdict at 3x (3840x2160 internal and output)**

| Era | Verdict | Frames |
|---|---|---|
| Intro: EA / DC / WB logos, legal, starfield, loading | renders | `w3x_on190954/drive/attempt1/frames` cl_001-008 |
| Title (PRESS START over the city) | renders | cl_009-015 |
| Main menu | renders | cl_016-021 |
| Opening movie (Krypton) | renders | cl_022-029 |
| Gameplay, night, HUD | renders | cl_030+ |
| Metallo save: boss intro (in-engine) + daylight gameplay | renders (combat itself not driven) | `fight_on193912/attempt1/frames` |
| Map, Fortress of Solitude menu, COSTUME VIEWER | renders; a faint horizontal line across the viewer's crystal backdrop at internal row 1080 -> 1081 (guest row 360, the half-height) - **not a scaling defect**: identical with both GPU gates off (`fv3_off`), and at 1x the strongest row step of the backdrop is guest row 359 -> 360 (`fv1`: 4.0 vs a 2.0 median; 3x: 4.0 vs 1.0). **The game's own content** (round 2, below): Xenia's render of the same frame has the identical step | `fortress3x` cl_329, `fv3_on`, `fv3_off`, `fv1` |

Repro (from the main root, holding the game slot as your owner):
`powershell -File tools/player/roll_player.ps1 -ExePath <exe> -Out <dir> -Scenario tools/scenarios/perf_player_gameplay.json -Gates "SR_DISPLAY_MODE=borderless,SR_PRESENT_DUMP=1" -DumpEvery 150 -DumpCap 600 -TimeoutSeconds 1200 -Owner <you> -SlotHeld`
(whole run, auto -> 3x on the 4K desktop); the boss save:
`powershell -File tools/sr_drive.ps1 -Scenario <worktree>/tools/scenarios/res_boss_fight.json -ExePath <exe> -Out <dir> -TimeoutSeconds 600 -Gates "SR_INSTANCE_ID=res4k,SR_PORT_FEATURES=1,SR_PM4_GPU_TIME=1,SR_WINDOW_SIZE=3840x2160,SR_RES_SCALE=3,SR_CONTENT_DIR=archive/player/boss_saves/metallo/content"`;
the viewer: the same with `tools/scenarios/res_fortress_viewer.json`.

**GPU per frame, live** (`SR_PM4_GPU_TIME=1`, median of the 120-frame windows, ms; a loaded GPU
boosts, so these are the authoritative numbers - see "Measuring" for the replay bench's clocks)

| Roll | busy | draw | EDRAM store | EDRAM load | resolve | tex load | present chain |
|---|---|---|---|---|---|---|---|
| title 3x, gates OFF (`pab_f_w3x_off`) | 12.33 | 1.27 | 1.77 | 1.32 | 4.49 | 3.37 | 0.06 |
| title 3x, gates ON (`pab_f_w3x_on`) | 11.00 | 1.33 | 1.47 | 1.54 | 2.52 | 3.62 | 0.06 |
| Metallo gameplay 3x, gates OFF (`fight_off193509`) | 15.32 | 2.99 | 1.83 | 1.38 | 4.70 | 4.26 | 0.06 |
| Metallo gameplay 3x, gates ON (`fight_on193912`) | **12.28** | 2.96 | 1.47 | 1.54 | 1.92 | 4.23 | 0.06 |
| whole run 3x, gates ON (`w3x_on190954`) | 12.29 | 2.88 | 1.47 | 1.54 | 1.90 | 4.24 | 0.06 |
| Fortress menus 3x, gates ON (`fortress3x`) | 12.89 | 2.14 | 1.63 | 1.54 | 2.55 | 4.92 | 0.06 |
| title 2x on a 4K window, bilinear (`up2x_bilinear0`) | 6.46 | 1.45 | 0.94 | 0.93 | 1.02 | 2.09 | 0.05 |
| same, `cas` sharpness 50 | 6.65 | | | | | | 0.15 |
| same, `fsr` sharpness 50 | 6.63 | | | | | | 0.32 |

- "gates" = `SR_NATIVE_RESOLVE_SCALED` + `SR_PM4_XFER_DIRECT`, baked default ON by this lane
  (=0 keeps the A/B). Gameplay at 3x: 15.3 -> 12.3 ms (resolve 4.7 -> 1.9).
- Frame rate in these rolls is 50-56 fps and is not GPU-bound (the guest tick and the CP are; see
  docs/PERFORMANCE.md). For 120 fps the GPU must reach 8.3 ms: 2x (6.5 ms) does today; 3x needs
  another ~4 ms - tex load (scaled load 2.7 + scratch copy 1.4-1.6) and the depth transfers (whole
  plane through EDRAM, ~1.5 store + 1.5 load) are the next targets.
- Replay byte identity of the baked exe (`archive/pm4_score/bk_1x`, `bk_3x` in the lane worktree):
  1x 4/4 and 3x 7/7 frames md5-identical to the gates-off build, MADs as in the table above.

**Outputs and window modes (2x auto unless noted, title + menus)**

| Output | Internal | Fit (from `[RES_SCALE] fit`) | Verdict |
|---|---|---|---|
| 1366x768 windowed | 2x | was (0,0) 1365x768 - a 1-px black column; now edge to edge (fitRect snaps within 2 px) | renders edge to edge after the snap (content x 0..1365; `fit_1366x768211124`, exe res4k-h) |
| 1600x900 windowed | 2x | 1600x900, area shrink | renders |
| 1920x1080 windowed | 2x | 1920x1080, area shrink (2x supersampled 1080p: clean text, no moire on window grids) | renders |
| 2560x1080 windowed | 2x | (320,0) 1920x1080 pillarbox | renders |
| 3440x1440 windowed | 2x | (440,0) 2560x1440 1:1 pillarbox | renders |
| 3840x2160 windowed / borderless | 3x (auto) | 1:1 | renders (the whole run above, borderless) |
| live resize 1600x900 -> 3844x2097 (maximised) | 2x | refit at (58,0) 3728x2097, bilinear enlarge, next present | renders |
| stepped resize 2560x1440 / 1280x720 / 1920x1080 / 1600x900 (`tools/res_window.py`, no activation) | 2x | 2560x1440 1:1, then area shrink at each size; refit on the next present | renders; every step's frames fill the new size (`resize212627`, exe res4k-i) |
| exclusive fullscreen, Alt+Enter | - | not rolled (owner's TV; Alt+Enter is real input only) | unverified live |

Repro: `powershell -File archive/v1_res4k/turn2.ps1 -Exe <exe> [-Skip sweep,resize,upscale] [-Sizes 1366x768,...]`
(holds nothing itself - acquire the slot first; scenario `tools/scenarios/mod_title_capture.json`).

**Upscale filter at 2x on a 4K window** (`up2x_*`, 1:1 crops of the title text): `bilinear` 0 is
soft; `cas` 50 visibly crisper; `fsr` 50 the cleanest letter edges, no ringing. Cost 0.05 / 0.15 /
0.32 ms.

**Astra (blind second opinion)** - replay crops vs Xenia 3x (2026-10-06): a qualified yes.
Live crops, first round: "The sampled live menu, gameplay and Fortress views render coherently at
3x; resolution-scale correctness remains unverified, including a possible Fortress background
seam." Second round (1:1 crops of the intro, title, menu, movie, night and day gameplay, viewer;
`archive/v1_res4k/astra_live/`): "'Mostly coherent 3x presentation, with a Fortress seam and
remaining oracle gaps' holds; 'artifact-free across all eras' does not." It found the viewer
line independently (crop y 381 = row 1081) and saw no seams, offsets or misaligned layers in the
other eras; it could not judge, without an oracle of the same frame, the soft movie / menu text,
the coarse HUD icons, jagged foliage, and the viewer's blocky dark foreground silhouette (the 1x
frame has the same shapes).

### GPU round 2 (2026-10-07): 3x from 12.3 to 8.4 ms

Live 3x, windowed 3840x2160, the Metallo save (`res_boss_fight.json`), `SR_PM4_GPU_TIME=1` medians,
GPU clock logged (nvidia-smi 1 s): every roll below at a 2730 MHz median.

| Roll | busy | draw | EDRAM store | transfers | resolve | tex load |
|---|---|---|---|---|---|---|
| round-2 gates all `=0` (`r9fight_off`) | 12.33 | 3.00 | 1.47 | 1.55 | 1.89 | 4.23 |
| + share + depth transfers (`r2fight_on`, round-1 exe) | 9.44 | 2.99 | 0.00 | 1.33 | 1.90 | 3.04 |
| + direct scaled loads (`r9fight_on`) | 8.51 | 2.97 | 0.00 | 1.33 | 1.90 | 2.11 |
| + stencil predication (`r10fight_pred`) - **the merged default** | **8.38** | 3.04 | 0.00 | 1.16 | 1.87 | 2.10 |
| whole run, merged default (`r10whole`, INTRO onward) | 8.32 | 2.83 | 0.00 | 1.16 | 1.88 | 2.11 |
| costume viewer, merged default (`r10viewer`) | 8.65 | 2.13 | 0.00 | 1.20 | 2.46 | 2.13 |

- Replay (gp1 / lg1, 2625 MHz): 8.8 / 8.35 ms with round 2 (from ~13 / 12.3 at round 1). Every
  gate and the merged default: replay md5-identical to the round-1 build at 1x (4/4) and 3x (7/7);
  activation witnesses: all 21 gp1 transfers direct (`SR_RES_XFERLOG`), 0 scaled loads on the
  scratch path except 16bpp (`SR_RES_TEXLOG`), `[TEX_SHARE]` / `[TEX_LOAD_DIRECT]` /
  `[PM4_XFER_DIRECT] depth on` / `stencil-bit passes predicated` lines.
- Frames: `r10whole` (intro logos, legal, starfield, logo movie, title, menu, Krypton movie,
  Metropolis intro, night gameplay + HUD) and `r10viewer` (pixel-identical to round 1's viewer).
- The replay bench's clock can sit in P5 (~400 MHz): only same-session, same-clock rows compare.
- What is left at 8.4 ms: the game's draws 3.0, resolves 1.9, scaled texture loads 2.1 (1.7 compute
  + 0.4 for the 16bpp shadow map's copy), transfers 1.2 (depth + 8 bit passes where bits occur).
- Repeat samples of the merged default (alternated with 16bpp direct, `r11p_a` / `r11p_b`): 8.41 and 8.26 ms - with `r10fight_pred`, three samples averaging **8.35 ms**: at the 8.3 target within the run-to-run spread, not clearly under it.

### GPU round 3 (2026-10-07): resolve write-through - REJECTED (byte-exact, no gain)

The idea: a texture whose identity exactly matches a scaled resolve's destination (base, pitch,
format, endian, tiling, scale) is written by the resolve kernel itself (a second UAV, one typed
store per texel next to the EDRAM-buffer store) and stamped current at the new memory version, so
its next bind skips the scaled load. Built as `SR_RESOLVE_WRITETHROUGH` (commits 423860c3, then
6d29a16f: only textures sampled since their last write-through), both reverted.

| Live 3x, Metallo fight, 2730 MHz, alternated | busy | resolve | tex load |
|---|---|---|---|
| default (`r15d_a`, `r15d_b`, `r16d_a`, `r16d_b`) | 8.42 / 8.28 / 8.41 / 8.33 | 1.88 | 2.10 |
| write-through, every match (`r15w_a`, `r15w_b`) | 8.54 / 8.45 | 2.73 | 1.39 |
| write-through, sampled-since-last only (`r16w_a`, `r16w_b`) | 8.43 / 8.46 | 2.63 | 1.52 |

- Replay md5-identical to the default: 1x 4/4, 3x 7/7 (`wt_1x`, `wt_3x`, `wtg_3x`); witness
  `[RESOLVE_WT]` lines (scaled loads per frame 39 -> 24).
- Why it loses: the resolve kernel's thread layout (64x8 strips, built for the EDRAM buffer)
  makes the extra texture stores as costly as the loads they replace, and a target is often
  resolved more than once per read. The saving the load measurement promised (up to 1.7 ms) is
  spent in the resolve.

**2x after round 2** (`r15_2x`, same fight, 4K window): busy 6.60 ms (draw 2.58, transfers 1.03,
resolve 1.44, tex load 1.37) at a **1365 MHz** median clock - the GPU did not boost at 2x, so the
time is at half the 3x clock; under the 8.3 ms target regardless.

## Still open

- **GPU at 3x: ~8.35 ms per gameplay frame (12.3 before round 2); 120 fps needs 8.3** - at the
  target within the run-to-run spread. Resolve write-through did not pay (round 3 above). Untried:
  a texture-shaped copy pass from the resolve output (one dispatch per write-through instead of
  stores inside the resolve), the 16bpp shadow map's 0.4 ms copy, the stencil bit passes. 2x: 6.60 ms.
- (Settled, round 2) The Fortress costume viewer's half-height line is the game's own content: a
  live trace of that frame (F9, `tools/scenarios/res_viewer_trace.json`, 1x) rendered by Xenia's
  trace dump shows the same strongest row step at 359 -> 360 (4.0 vs a 2.0 median) as our replay
  of it (MAD 0.01 vs Xenia). `archive/v1_res4k/vtrace1-xtr`, `vtrace1-xenia`.
- Exclusive fullscreen and Alt+Enter are unverified live (automation never takes the owner's
  screen); borderless at the desktop size and every windowed size are verified.
- Non-integer internal scales (1.5x) stay unsupported (Settings above).
- Ultrawide FOV is out of scope (the frame is pillarboxed at 16:9).
- The HLE draw path (`SR_PM4_LIVE=0`) stays 1x.

## Lane status (2026-10-06, iter-466 res4k)

- MERGED to `dev`: per-pass GPU timing + `tools/res_gpu_bench.py`;
  auto scale from the fitted 16:9 rectangle; native scaled resolve and direct colour transfers
  (baked ON, replay byte-identical: 1x 4/4, 3x 7/7 frames vs the gates-off build); the
  `upscale_filter` / `sharpness` stages (CAS, FSR 1; defaults byte-identical) registered as PC
  SETTINGS rows; the fit snap for near-16:9 outputs; scenarios `res_boss_fight`,
  `res_fortress_viewer`; `tools/res_window.py`.
- PROVEN LIVE: every era at 3x (table above, Astra second opinion); outputs 1280x720 to 3840x2160
  incl. 1366x768, 1600x900, 2560x1080, 3440x1440; stepped and maximised resizes; borderless 4K.
- NEXT: the two GPU items under "Still open"; then re-measure live at 3x.
