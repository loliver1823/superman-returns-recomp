# Our build vs Xenia Canary (criterion C3)

RELEASE_PLAN phase 5, step 1: an honest, rerunnable side-by-side of our PC build and Xenia Canary
on the same PC, same disc, same scenes, measured the same way. Release criterion C3 says we must
beat Canary on frame rate, frame pacing, visuals and load times; C4 sets 60 fps minimum, 120
typical, 240+ uncapped as the stretch, and 4K. This page is the method and the baseline of record.
The README's "Compared with Xenia Canary" table comes from `readme_table.md` of a run made with
the tool below: since 2026-11-16, the 2026-10-21 rerun (section "Rerun 2026-10-21" at the end).

**The baseline in one paragraph (2026-10-09, dev f062cc17; details below).** Canary runs this game
at its own 30 fps (no setting or patch lifts it): 29-30 fps in gameplay at 1x with vsync off, 22-30
with vsync on, and 19-23 fps at 4K. Ours holds a locked 60 at 1x and 4K, and 123-168 fps uncapped at
1x (115-151 at 4K): 2-6.6x Canary. Pacing at 60 is far better than Canary's (p99 18-21 ms vs
35-68 ms), but at the console's 30 Canary with vsync off paces the opening movie and the street
steadier than ours, and our boot movie transitions sometimes freeze (up to 1.6 s). The picture is
equivalent at the same internal resolution. Warm boots reach the first frame in half Canary's time;
a cold boot loses 30 s to the shader-cache load. Ours uses less GPU but 2.3-3.7x the memory.

## Repro

```
# the instrument check (no game slot; builds tools/perf/present_selftest.cpp with clang++)
python tools/perf/canary_compare.py selftest --out archive/v1_canary/selftest

# the comparison (takes the game slot itself, --per-hold runs per hold, a gap between holds)
python tools/perf/canary_compare.py batch --out-root archive/v1_canary/<tag> --exe <tools/snapshot_exe.ps1 copy>
python tools/perf/canary_compare.py report --out-root archive/v1_canary/<tag>
#   -> report.md (every number), report.json, readme_table.md (the README table), sheets/*.jpg
```

The baseline below was made with (in the canary worktree, `Superman Returns (USA)` a junction to
dev's folder):

```
python tools/perf/canary_compare.py batch --out-root archive/v1_canary/baseline_20261009 \
    --configs xenia-1x,ours-1x-30,ours-1x-60,ours-1x-unc,xenia-3x,ours-3x-60,ours-3x-unc,xenia-1x-novsync \
    --per-hold 2 --exe <repo>/archive/exe_snapshots/12da45b9c7d5-canary/SupermanReturns.exe
python tools/perf/canary_compare.py batch --out-root archive/v1_canary/baseline_20261009 \
    --configs xenia-3x-novsync,xenia-3x,ours-3x-60 --per-hold 3 --exe <same>
python tools/perf/canary_compare.py report --out-root archive/v1_canary/baseline_20261009
```

(The first batch ran before 6cd06c0e and so shot only the intro, title and movie scenes; the
second batch's 4K repeats carry every scene's screenshot. The default `--configs` is now all nine.)

Needs: Python 3.11 with psutil, vgamepad (ViGEmBus driver), PIL; `py -3.13` with frida; the
owner's Xenia Canary and ISO (`--xenia`, `--iso`, or SR_XENIA_EXE / SR_GAME_ISO). Run from a
checkout that holds `Superman Returns (USA)` (a junction to the real folder is fine).

## Method

### One instrument for both programs

`tools/perf/present_probe.js` (frida, attached by `tools/perf/present_probe.py`) hooks the
win32u system-call stubs every DXGI present goes through on its way to the kernel graphics
driver (`NtGdiDdDDIPresent*`, `NtDxgkSubmitPresent[Blt]ToHwQueue`, ...). Each call writes a
QueryPerformanceCounter stamp into a ring buffer from a few lines of C (frida CModule, about
0.2 us per present). The same code, at the same layer, times our build and Xenia. On this PC
(hardware-accelerated GPU scheduling on) every present of all three programs measured (the
calibration app, our build, Xenia) is `NtDxgkSubmitPresentToHwQueue` on the presenting thread.

PresentMon would be the usual tool, but its ETW session needs an elevated process and the
automation here is not elevated (and must not raise UAC prompts on the owner's screen).

Calibration (`selftest`, `archive/v1_canary/selftest`): `tools/perf/present_selftest.cpp`
presents a known number of frames and logs its own QPC right before and after every
`IDXGISwapChain::Present`. The probe must see every present, each inside that window:

| mode | app presents | probe | inside the app's Present window | interval p50 app / probe |
|---|---|---|---|---|
| uncapped (sync 0, tearing) | 2000 | 2000 | 2000 | 0.226 / 0.225 ms |
| Sleep(3) paced, background, QoS opt-out | 600 | 600 | 600 | 3.517 ms |
| vsync | 300 | 300 | 300 | 16.666 / 16.663 ms |

On our build the probe is cross-checked against SR_PERF_STATS (the exe's own stamp per present):
see the calibration column of each run.

### What each number means

- **Present fps**: (presents - 1) / span over the scene window: how often the process hands a
  new image to the display path. With vsync the display shows each one; uncapped (tearing) the
  display shows parts of several per refresh.
- **p50 / p95 / p99 / max**: percentiles of the interval between two consecutive presents, ms.
  p99 is the "1% low" (1000 / p99 = the 1%-low fps).
- **std**: standard deviation of the intervals (pacing evenness).
- **>33.4 / >50**: intervals longer than two / three 60 Hz refreshes.
- **Hitches 1.5x / 2x / 4x**: intervals longer than that multiple of the median of the previous
  120 (the definition of `tools/perf/hitch_report.py` and the in-game recorder).
- **Game fps**: guest frames: the game's own `D3DDeviceSwap` (Xenia: a probe hook on the JIT'd
  guest function 0x82112050; ours: SR_PERF_STATS's guest swap stamp).
- **cores / other cores**: the program's CPU use in cores, and the rest of the machine's busy
  cores in the same window (the owner and other lanes share the PC; a contended run says so).
- **GPU %**: nvidia-smi whole-GPU utilisation (one game runs at a time).
- **Stalls/min**: intervals > 50 ms per minute: an absolute count that compares across frame-rate
  caps (the relative hitch counts do not: at 0.3 ms frames a 0.5 ms interval is a "1.5x hitch").
- **Load times**, from guest events on both sides: process creation -> the first game frame
  (the first `D3DDeviceSwap`); -> the guest's request of the title screen
  (`screens/bootflow/ScrPressStart`); the NEW GAME press -> the end of the opening movie (the
  movie pipeline's deleting destructor 0x82476760, our `[MPDTOR]`; the level loads behind the
  movie). Ours: the game log; Xenia: the probe's hooks on the same guest functions
  (AptLoadByPath 0x826D2690 = our `[APT_SCREEN]`). A Xenia hook can only attach after the call
  that compiles the function, so a function first called at the moment of interest cannot be
  used: the FE show-screen action that brings the HUD up (`[FE_SHOWSCREEN]`, "in control") is
  ours-only and stays in report.json.

### The scenes

Both sides: nothing is pressed until the title is requested (the intro movies play in full),
the title is shown for 10 s, START, then the popup the game shows (ours: LOAD A GAME? -> A =
START NEW GAME; Xenia has no profile, so the same screen asks to select a gamer profile -> DOWN A
= continue without one). The guest requests `screens/hud/main_hud` at that press. From it ONE
input tape plays on both sides (ours `SR_INPUT_REPLAY`, anchored in the request itself; Xenia a
ViGEm virtual pad, `tools/xenia_pad_tape.py`, started when the probe sees the request - about
0.55 s later, so route scenes are timed from each side's own tape start: the pause menu was
requested at +50.62 s on Xenia and +50.05 s on ours).

The windows were placed from a survey pair (`archive/v1_canary/survey`: a screenshot every 2.5 s
on both sides; `sheet_a..d.jpg`, `boot_sheet.jpg`), where both programs show the same picture at
the same offsets:

| scene | window | what is on screen |
|---|---|---|
| intro | first frame +8 .. +56 s | EA, DC, WB logos, legal, the game's intro movie |
| title | title request +2 .. +8.5 s | PRESS START over the 3D city |
| fmv | main_hud +4 .. +28 s | the opening movie (space, Earth, Metropolis, Clark becomes Superman) |
| street | tape +36 .. +48.5 s | gameplay just after the load: Superman standing in the street, traffic, pedestrians |
| on_foot | tape +56 .. +79.5 s | a tutorial overlay, a camera pan, running down the street |
| flight | tape +86 .. +96 s | flight through the streets at full speed |
| wall | tape +98.5 .. +122.5 s | still in flight mode, pushed against a building (a close-up dark wall) |
| pause | tape +126 .. +138 s | the pause menu over gameplay |

The route tape (`ROUTE` in the tool): A at +42 s, START +50 s (pause), A +54 s (resume), a camera
pan +58..70 s, run +70..80 s, Y held +80 s (take off), full stick + RT +83..113 s with yaw
pulses, neutral, START +123 s (pause). The flight runs into a building after ~10 s on both sides;
the wall window is kept and labelled as what it is.

A screenshot of each scene is taken 1.2 s after its window closes (PrintWindow through DWM, the
window behind other windows): the survey showed a capture perturbs the next ~1 s (ours p99 23.3 ms
in the second after a shot vs 18.5 ms elsewhere; one 133 ms interval on Xenia), so no capture
falls inside a measured window.

### Settings

| | Xenia Canary | ours |
|---|---|---|
| build | the owner's `canary_experimental` install on R: (sha256 in run.json) | dev `build/SupermanReturns.exe` at f062cc17, snapshot `12da45b9c7d5-canary` (sha256 in run.json) |
| config | the owner's `xenia-canary.config.toml`, untouched; differences are flags: `--mute=true --discord=false --fullscreen=false --content_root=<run>/content --log_file=<run>/xenia.log --draw_resolution_scale_x/y --vsync --framerate_limit=0` | natural (no port features, retail front end, `SR_MODS=0`), `SR_LOG=player` (a player's log), `SR_RES_SCALE`, `SR_WINDOW_SIZE`, `SR_HIGHFPS` (unset = the game's 30, `60`, `uncapped`), `SR_AUDIO_SINK=null`, `SR_FRAME_DUMP=0`, `SR_PERF_STATS=1` |
| output window | the owner's configured window, 1280x720 LOGICAL pixels: a 3840x2039 client on this 4K desktop at 300 % (menu bar excluded) | the same client size, 3840x2039 |
| 1x | 1280x720 internal | same |
| 4K | draw resolution scale 3 = 3840x2160 internal | `SR_RES_SCALE=3` |
| frame rate | the game's own cap: its render loop runs at 30 Hz. No Canary setting lifts it, and the Canary game-patch set has no patch for this title (checked 2026-10-09: 501 patch files in `xenia-canary/game-patches`, none for 454107ED). Vsync on (the owner's config); one run with it off | 30 (the game's own), 60 (cap, vsync), uncapped (tearing) |
| sound | XAudio2 with the voice volume at 0 (`--mute`) | null sink (a wall-clock consumer at the console rate) |
| saves | a fresh content root per run (no profile, no save) | a fresh per-process save root (SR_INSTANCE_ID) |
| shader caches | Xenia's own persistent cache (the owner's install) | the shipped AOT cache + the runtime translator's cache kept across runs in `archive/v1_canary/_shader_runtime_cache` |
| file cache | both programs had run before in the session: warm OS file cache (the cold case is reported separately) | same |
| window | windowed, behind other windows, never activated (`tools/bg_window.py`) | windowed, behind other windows (SR_INSTANCE_ID) |
| background throttling | opted out by this tool (ProcessPowerThrottling: no EcoQoS, timer resolution honoured) | the exe opts itself out (SR_BG_HIGH_QOS default) |
| input | a ViGEm virtual pad (the run refuses if a real XInput pad is connected: Xenia reads every pad) | the tape inside the game; real input ignored |

Why the opt-out matters: Windows 11 stops honouring a process's 1 ms timer request when all its
windows are occluded. The calibration app paced with Sleep(3) behind other windows ran at
15.5 ms per frame; with the opt-out applied from outside, 3.5 ms. Xenia paces its vblank and
frame limiter with timers, so without the opt-out a background Xenia would be measured slower
than a player sees it.

## Baseline 2026-10-09 (dev f062cc17, exe snapshot `12da45b9c7d5-canary`)

Evidence: `archive/v1_canary/baseline_20261009/` (one folder per run: `run.json`, `events.jsonl`,
`probe/`, `proc.csv`, `gpu.csv`, `shots/`, ours `perf/` + `drive/`, Xenia `xenia.log` + `pad.log`;
`report.md` has every number, `readme_table.md` the table below, `sheets/` the side-by-side
screenshots and the 1:1 4K crops). Pilot: `archive/v1_canary/pilot/`; survey pair:
`archive/v1_canary/survey/`; instrument check: `archive/v1_canary/selftest/`. Eleven runs on
2026-10-09 23:18 .. 2026-10-10 01:20 in five slot holds; one run per config, two for Xenia 3x
(vsync on) and ours 3x 60 (they agree within 1 fps in every scene). Every run is tagged natural
for our build except the frame-rate and resolution settings (`SR_HIGHFPS`, `SR_RES_SCALE`: port
features). Other processes kept 0.7-3.8 cores busy during the scenes (`other cores` in
report.md): the owner and other lanes shared the PC.

Calibration on the real targets: in every one of our runs the probe saw the same presents as
the exe's own SR_PERF_STATS stamps (6,655 of 6,656 up to 28,755 of 28,758 matched within 2 ms;
residual p50 3.6-6.7 us, p99 42-54 us). On Xenia the probe saw one present per guest
`D3DDeviceSwap` (6,370 presents, 6,369 guest swaps in xenia-1x). Every present on both sides was
`NtDxgkSubmitPresentToHwQueue`. 0 records lost in any run.

### The table for the README

Present fps and p99 of the present interval per scene; load times from guest events.

**1x (1280x720 internal)**

| Scene | Canary, vsync on | Canary, vsync off | ours, 30 cap | ours, 60 cap | ours, uncapped |
|---|---|---|---|---|---|
| Intro movies (EA, DC, WB, legal, the game's intro) | 30 fps, p99 50.5 ms | 30 fps, p99 38.7 ms | 30 fps, p99 49.8 ms | 31 fps, p99 40.0 ms | 63 fps, p99 34.6 ms |
| Title: PRESS START over the 3D city | 30 fps, p99 35.1 ms | 30 fps, p99 37.1 ms | 30 fps, p99 49.1 ms | 60 fps, p99 18.2 ms | 244 fps, p99 8.2 ms |
| Opening movie (FMV) | 30 fps, p99 35.0 ms | 30 fps, p99 38.5 ms | 30 fps, p99 51.5 ms | 35 fps, p99 35.4 ms | 110 fps, p99 34.8 ms |
| Gameplay start: standing in the street | 22 fps, p99 51.2 ms | 29 fps, p99 38.2 ms | 30 fps, p99 50.3 ms | 60 fps, p99 21.4 ms | 123 fps, p99 15.0 ms |
| On foot: tutorial overlay, camera pan, run | 27 fps, p99 50.8 ms | 30 fps, p99 37.5 ms | 30 fps, p99 36.1 ms | 59 fps, p99 19.9 ms | 126 fps, p99 11.0 ms |
| Flight through the streets (full speed) | 28 fps, p99 51.0 ms | 30 fps, p99 36.8 ms | 30 fps, p99 38.2 ms | 60 fps, p99 21.1 ms | 149 fps, p99 9.8 ms |
| Flight mode against a building (close-up wall) | 30 fps, p99 34.8 ms | 30 fps, p99 36.7 ms | 30 fps, p99 35.9 ms | 60 fps, p99 18.8 ms | 153 fps, p99 9.7 ms |
| Pause menu (over gameplay) | 30 fps, p99 49.0 ms | 30 fps, p99 38.1 ms | 30 fps, p99 35.4 ms | 60 fps, p99 19.7 ms | 168 fps, p99 8.1 ms |
| Launch to the first game frame | 14.46 s | 14.7 s | 7.32 s | 11.39 s | 8.36 s |
| Launch to the title (PRESS START requested) | 74.38 s | 74.74 s | 70.69 s | 75.55 s | 68.02 s |
| NEW GAME press to the end of the opening movie (the level loads behind it) | 31.1 s | 31.37 s | 31.09 s | 32.26 s | 31.03 s |

**4K (3x = 3840x2160 internal)**

| Scene | Canary, vsync on | Canary, vsync off | ours, 60 cap | ours, uncapped |
|---|---|---|---|---|
| Intro movies (EA, DC, WB, legal, the game's intro) | 30 fps, p99 35.1 ms | 30 fps, p99 38.7 ms | 33 fps, p99 35.1 ms | 45 fps, p99 34.8 ms |
| Title: PRESS START over the 3D city | 20 fps, p99 67.8 ms | 30 fps, p99 44.6 ms | 60 fps, p99 18.5 ms | 198 fps, p99 8.3 ms |
| Opening movie (FMV) | 30 fps, p99 35.1 ms | 30 fps, p99 38.7 ms | 35 fps, p99 46.7 ms | 65 fps, p99 35.2 ms |
| Gameplay start: standing in the street | 19 fps, p99 67.4 ms | 21 fps, p99 50.6 ms | 60 fps, p99 19.5 ms | 115 fps, p99 11.0 ms |
| On foot: tutorial overlay, camera pan, run | 19 fps, p99 67.7 ms | 21 fps, p99 51.7 ms | 60 fps, p99 20.1 ms | 122 fps, p99 12.0 ms |
| Flight through the streets (full speed) | 19 fps, p99 67.4 ms | 21 fps, p99 50.6 ms | 60 fps, p99 19.1 ms | 131 fps, p99 11.4 ms |
| Flight mode against a building (close-up wall) | 19 fps, p99 67.0 ms | 22 fps, p99 48.8 ms | 60 fps, p99 18.6 ms | 140 fps, p99 9.1 ms |
| Pause menu (over gameplay) | 20 fps, p99 67.1 ms | 23 fps, p99 46.1 ms | 60 fps, p99 18.1 ms | 151 fps, p99 8.6 ms |
| Launch to the first game frame | 19.48 s | 14.72 s | 7.23 s | 7.32 s |
| Launch to the title (PRESS START requested) | 79.42 s | 74.81 s | 81.14 s | 65.68 s |
| NEW GAME press to the end of the opening movie (the level loads behind it) | 31.15 s | 31.16 s | 32.37 s | 31.01 s |

How to read the movie rows: the movies are 30 fps content on both sides. Ours above the 30 cap
presents the held movie frame again (35 / 65 / 110 presents per second), so the movie rows say
nothing about smoothness beyond 30; their p99 does.

### Where we win, where we lose, by how much

**Frame rate (C3): a clear win in every gameplay scene and the menus.** Canary runs this game at
its own 30 Hz render loop and no Canary setting or patch lifts it. In gameplay (street, on foot,
flight, wall, pause) Canary's best setting (vsync off) gives 29-30 fps at 1x and 21-23 fps at 4K;
the owner's setting (vsync on) gives 22-30 fps at 1x and 19-20 fps at 4K. Ours: a locked 60 at the
60 cap at 1x and at 4K; uncapped 123-168 fps at 1x and 115-151 fps at 4K, which is 4.1-5.6x
Canary's best at 1x and 5.5-6.6x at 4K. Title: Canary 30 (1x) / 20-30 (4K); ours 244 / 198
uncapped.

**Frame pacing (C3): a win at 60 and above, NOT a win at the console's 30.** At the 60 cap our
gameplay p99 is 18-21 ms (target 16.7) and only two intervals in all ten gameplay windows exceed
50 ms (a 430 ms stall on foot in ours-1x-60, a 64 ms one in ours-3x-60); Canary's gameplay p99
is 35-51 ms at 1x and 46-68 ms at 4K, with 118-290 intervals over 50 ms per scene at 4K with
vsync on (0-76 with vsync off). But with both at 30 fps (ours
with no frame-rate feature), Canary with vsync off paces steadier than ours: street p99 38.2 ms
and 0 intervals over 50 ms (Canary) vs 50.3 ms and 6 (ours); the opening movie 38.5 ms and 0 vs
51.5 ms and 37 (ours alternates 33 / 50 ms intervals: frames missing a 60 Hz vblank). With vsync
on (the owner's setting) Canary is the worse of the two (22 fps in the street, p99 51 ms).

**Present freezes at boot (C3 pacing): a loss.** The intro movies' worst interval is 101-118 ms on
Canary in all 5 runs; ours 114-119 ms in 4 of 6 runs, but 524 ms and 1,565 ms in the other two.
Our log names the cause: the render tick waits on a guest lock while the next intro movie opens
(`[CS_WAIT] cs=05AE9A98 waiter_lr=82149590 ... holder_lr=8271EE0C`, 0.3-6.6 s waits in every run;
they freeze presents only sometimes).

**Load times (C3): a win warm, a loss cold, a tie on NEW GAME.**

| load | Canary (median, range) | ours (median, range) |
|---|---|---|
| launch -> first game frame, warm file cache | 14.7 s (13.5-19.5, 5 runs) | 7.3 s (6.5-11.4, 6 runs) |
| launch -> the title requested, warm | 74.7 s (72.9-79.4) | 69.4 s (65.7-81.1) |
| launch -> first frame / title, COLD (first run of the session, pilot) | 15.1 s / 75.2 s | 45.1 s / 105.1 s |
| NEW GAME press -> the opening movie ends | 31.15 s (31.1-31.4) | 31.07 s (31.0-32.4) |
| the movie's end -> the HUD up (ours only, `[FE_SHOWSCREEN]`) | not measurable by hook; in the survey Canary was already in gameplay 2.5 s after the movie | 0.8-1.0 s (a LOADING screen), 7.2 s in 1 of 6 runs |

The cold boot loss is the shader-cache load: `[DISPLAY] CP waited 40785.9 ms for the live backend`
until `[SHADER_CACHE] loaded 216 VS + 209 PS` (the 880-file AOT cache read from the HDD with a cold
OS cache; RELEASE_PLAN 6.2 "the boot shader-cache stall"). Warm, ours reaches the first frame in
half Canary's time; the title time is dominated by the intro movies, which play at the same speed
on both (in the runs without a transition freeze each intro movie ends within 0.2 s of Canary's,
relative to the first frame; MOVIE_PACE speed 0.993-0.999 at the 30 cap). The opening movie ran
at speed 0.995-1.000 in 6 of 8 of our runs and 0.966-0.983 (15-22 late frames) in two.

**Visuals (C3): equivalent at the same internal resolution; ours gets there at 60+.** The 1:1
centre crops at 4K (`sheets/*_4k_crop.png`) show the same geometry, textures, lighting and UI text
on both; the differences are animation timing (Superman's idle pose, the title fly-over camera)
and SAVE / LOAD enabled on ours vs greyed on Canary (Canary runs without a gamer profile). These
scenes do not cover the known open visual items (car shimmer / reflections, RELEASE_PLAN phase 6).

**Resources: ours uses less GPU but far more memory.** GPU (whole-GPU utilisation): ours 18-19 % at
1x 60 and 33-35 % at 4K 60 vs Canary 35-42 % at 1x 30 and 54-64 % at 4K 19-23 fps. CPU: both
5.4-6.4 cores (the game's spin-waiting kernel loops on both). Private bytes: ours 9.3 GB (1x) and
11.2 GB (4K) vs Canary 2.5 GB and 4.7-5.0 GB.

### Against C3 and C4

| criterion | status on this baseline |
|---|---|
| C3 frame rate | met: 2-6.6x Canary in gameplay, at 1x and 4K |
| C3 frame pacing | met at 60 and above; not met at 30 (Canary vsync off is steadier in the movie and the street); boot movie transitions freeze up to 1.6 s on ours |
| C3 visuals | equal at the same internal resolution (not better); better in practice only through frame rate |
| C3 load times | warm boot met (first frame 2x faster); cold boot not met (+30 s, shader cache); NEW GAME tied |
| C4 60 minimum | gameplay yes at 1x and 4K (60 cap and uncapped); not "everywhere": boot movie-transition freezes, one 430 ms stall on foot, the movies themselves are 30 fps content |
| C4 120 typical | 1x uncapped yes (123-168); 4K uncapped borderline (115-151: street 115) |
| C4 240+ uncapped | no (only the 1x title reaches 244) |
| C4 4K | yes: 60 locked at 3x |

### What the plan does not cover (found here)

- Boot movie-transition freezes (above): render tick vs the movie open on lock 05AE9A98.
- The cold-boot shader-cache stall costs 30 s against Canary (above); a player sees it on every
  first boot after a reboot.
- At the 30 fps setting ours alternates 33 / 50 ms intervals in the opening movie and the street
  (vblank misses); Canary vsync off does not.
- An occasional 7 s post-movie load (1 of 6 runs; `[ITEM_SPIN] item ... polled 200K+ times`).
- Memory: 2.3-3.7x Canary's private bytes.
- Test infrastructure inside every automated roll's numbers: the timed 120 / 240 / 360 s frame
  dumps (`SR_FRAME_DUMP`, automated rolls only) landed in the measured window of `perf_run.py` and
  other perf rolls; this tool turns them off, and so does `perf_run.py` since 2026-10-18 (step 4 item 7e:
  `SR_FRAME_DUMP=0` in every config, `--frame-dump` opts in and moves the BMPs into `<out>/frame_dump/`).
- Canary's own pacing depends on vsync: its vsync-on default (the owner's config) costs it 8 fps in
  the street at 1x. A README comparison must show both settings.

### What is unfair or not measured

- One sample per config (two for the 4K pair). Live rolls are not deterministic (traffic, pedestrians,
  load timing); boot times vary by ~8-15 s run to run on both sides.
- Present submission is measured, not the moment of display (no ETW without elevation). With vsync
  the two match; uncapped (tearing) the display shows parts of several frames per refresh.
- Our above-30 rates are a port feature (`SR_HIGHFPS`): the game's own 30 Hz simulation, the render
  loop interpolating between its frames (HIGH_FRAMERATE.md). Canary has no such option for this game.
- Canary runs without a gamer profile (its prompt differs from our LOAD A GAME?; SAVE / LOAD greyed);
  ours runs the natural front end without the custom front end players get.
- Canary's in-control moment cannot be hooked (a hook attaches only after the call that compiles the
  function), so the NEW GAME load compares the movie's end; ours' extra post-movie load is ours-only.
- Sound: Canary mixes into XAudio2 at volume 0, ours into a null sink; the cost difference is not
  measured.
- Xenia's window is sized in logical pixels by its config; ours was given the same physical client
  (3840x2039). The owner may run Canary maximised or full screen.
- The route flies into a building after ~10 s of flight on both sides (the "wall" scene); a better
  flight route is future work.
- The Warworld arena is not in the route: reaching it needs the dev-tool level skip on both sides
  (`tools/devtools/skip_run.py`, ~20 min of slot time per side); not run, to keep slot holds short.
- Cold-boot numbers come from one pilot run per side, before the scenes were final.

## Rerun 2026-10-21 (the step-4 close; release build of dev 2e07398e)

Lane D's 2e run at the step-4 close, on the close pair's release exe (`SR_RELEASE_COMPOSITION=ON`, exe
sha256 c1ec241a96f7...), the same tool and route as the baseline, `--per-hold` reruns where another lane's
work overlapped a window. Display: 1920x1080 at 60 Hz on the virtual adapter (the baseline ran on the native
3840x2160 60 Hz panel); the windows were 1280x720 clients on both sides. Evidence:
`archive/v1_canary/close_2e07398e1b2b_graded/` in the dev checkout (`report.md`, `readme_table.md`, the per-run
folders). This is the table the README carries.

**1x (1280x720 internal)**

| Scene | Canary, vsync on | Canary, vsync off | ours, 30 cap | ours, 60 cap | ours, uncapped |
|---|---|---|---|---|---|
| Intro movies (EA, DC, WB, legal, the game's intro) | 30 fps, p99 36.9 ms | 30 fps, p99 39.1 ms | 30 fps, p99 48.9 ms | 33 fps, p99 35.1 ms | 56 fps, p99 35.1 ms |
| Title: PRESS START over the 3D city | 27 fps, p99 66.8 ms | 30 fps, p99 40.6 ms | 30 fps, p99 37.2 ms | 60 fps, p99 24.6 ms | 147 fps, p99 14.4 ms |
| Opening movie (FMV) | 30 fps, p99 36.7 ms | 30 fps, p99 39.6 ms | 30 fps, p99 36.1 ms | 35 fps, p99 35.3 ms | 81 fps, p99 35.5 ms |
| Gameplay start: standing in the street | 24 fps, p99 51.4 ms | 30 fps, p99 37.2 ms | 30 fps, p99 36.1 ms | 59 fps, p99 30.0 ms | 67 fps, p99 28.4 ms |
| On foot: tutorial overlay, camera pan, run | 27 fps, p99 51.7 ms | 30 fps, p99 36.7 ms | 30 fps, p99 52.0 ms | 60 fps, p99 20.3 ms | 72 fps, p99 18.4 ms |
| Flight through the streets (full speed) | 29 fps, p99 50.1 ms | 30 fps, p99 36.5 ms | 30 fps, p99 52.5 ms | 60 fps, p99 21.2 ms | 76 fps, p99 21.6 ms |
| Flight mode against a building (close-up wall) | 30 fps, p99 50.4 ms | 30 fps, p99 41.7 ms | 30 fps, p99 50.7 ms | 60 fps, p99 20.4 ms | 79 fps, p99 15.9 ms |
| Pause menu (over gameplay) | 29 fps, p99 50.6 ms | 30 fps, p99 41.0 ms | 30 fps, p99 51.6 ms | 60 fps, p99 20.4 ms | 79 fps, p99 16.6 ms |
| Launch to the first game frame | 14.78 s | 13.6 s | 10.81 s | 10.8 s | 10.82 s |
| Launch to the title (PRESS START requested) | 75.04 s | 72.9 s | 68.96 s | 68.75 s | 68.8 s |
| NEW GAME press to the end of the opening movie (the level loads behind it) | 31.09 s | 31.05 s | - | - | - |

**4K (3x = 3840x2160 internal)**

| Scene | Canary, vsync on | Canary, vsync off | ours, 60 cap | ours, uncapped |
|---|---|---|---|---|
| Intro movies (EA, DC, WB, legal, the game's intro) | 30 fps, p99 35.2 ms | 30 fps, p99 39.0 ms | 31 fps, p99 36.2 ms | 42 fps, p99 35.3 ms |
| Title: PRESS START over the 3D city | 25 fps, p99 51.1 ms | 29 fps, p99 46.4 ms | 60 fps, p99 19.4 ms | 136 fps, p99 16.1 ms |
| Opening movie (FMV) | 30 fps, p99 35.1 ms | 30 fps, p99 38.7 ms | 30 fps, p99 35.2 ms | 59 fps, p99 35.6 ms |
| Gameplay start: standing in the street | 20 fps, p99 65.5 ms | 21 fps, p99 51.8 ms | 60 fps, p99 19.2 ms | 68 fps, p99 29.9 ms |
| On foot: tutorial overlay, camera pan, run | 19 fps, p99 67.7 ms | 22 fps, p99 51.2 ms | 60 fps, p99 22.5 ms | 70 fps, p99 19.3 ms |
| Flight through the streets (full speed) | 20 fps, p99 52.3 ms | 22 fps, p99 50.0 ms | 60 fps, p99 21.3 ms | 75 fps, p99 18.3 ms |
| Flight mode against a building (close-up wall) | 20 fps, p99 51.9 ms | 23 fps, p99 47.5 ms | 60 fps, p99 20.7 ms | 79 fps, p99 16.6 ms |
| Pause menu (over gameplay) | 20 fps, p99 51.7 ms | 23 fps, p99 46.4 ms | 60 fps, p99 19.4 ms | 78 fps, p99 15.7 ms |
| Launch to the first game frame | 13.99 s | 13.87 s | 10.84 s | 10.8 s |
| Launch to the title (PRESS START requested) | 73.19 s | 73.12 s | 68.94 s | 68.77 s |
| NEW GAME press to the end of the opening movie (the level loads behind it) | 31.13 s | 31.1 s | - | - |

Against the 2026-10-09 baseline:
- **C3 (beat Canary) holds** on every gameplay row: 2-2.6x Canary uncapped at 1x, ~3x at 4K; the 60 cap holds
  60; at the 30 cap the two tie.
- **The baseline clause FAILED.** Uncapped gameplay runs at about half the baseline (1x street / on foot /
  flight / wall / pause 67 / 72 / 76 / 79 / 79 against 123 / 126 / 149 / 153 / 168; 4K 68 / 70 / 75 / 79 / 78
  against 115 / 122 / 131 / 140 / 151), the title fell from 244 to 147 fps (1x) and 198 to 136 (4K), and the
  30-cap gameplay p99 rose from 35-38 to 51-53 ms. A same-day control (the 10-09 exe run on the same day)
  reproduced the baseline, so it is code, not the display. It entered between 2026-10-09 and 2026-10-13: the
  native draw path's per-frame CPU cost (render thread 12-13 ms per presented frame against 3.7 ms).
- Warm first frame: ours 10.8 s against Canary's 13.6-14.8 s.
- The NEW GAME row is empty for ours: its end marker (`[MPDTOR]`) is development-build only.

After this run, step 6's performance work (to 2026-10-25) measured its builds against the 10-09 exe, not against
Canary: the same route's street and on-foot scenes, several interleaved rolls per build. On the last measured
heads, uncapped gameplay ran at about 116-119 fps at 1x and 3x against the 10-09 exe's ~119-121 (1x passing,
3x marginal or short). No Canary rerun was made on those builds.
