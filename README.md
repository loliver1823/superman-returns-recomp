# Superman Returns: Xbox 360 recompilation

I worked on this because *Superman Returns*, the last proper standalone Superman game, never got a decent
PC version, and I reckoned it deserved one. It got a fair way: the game runs natively on PC from start to
finish in automated tests, with high frame rates, 4K and a heap of PC extras. Then life got in the way, and
someone else's ReXGlue-based port of the same game is close to release anyway, so I'm putting this code up
as it is for anyone who wants to reuse bits of it.

> **Status: development is paused.** I've stepped back from the project for other life commitments. The code
> is published as-is, for anyone to use or build on. There is no prebuilt release, and issues and pull
> requests may not get an answer.

**What I got working:**
- **Native, not emulated.** The game's CPU code is recompiled ahead of time to x86-64, and its graphics and
  audio run on a native Direct3D 12 renderer and PC audio code. No Xbox 360 GPU or audio hardware is emulated
  on the default path ([Native status](#native-status)).
- **High frame rates with real frames.** The render loop runs above the game's 30 Hz, and every frame is drawn
  by the game's own renderer (no interpolation or frame generation). Plus 4K and resolution scaling, with FXAA,
  AMD CAS and FSR 1.
- **Keyboard and mouse done properly.** Bindings drive the game's own actions, every action can be rebound, and
  the mouse works in every menu.
- **Controllers** through SDL3 (Xbox, PlayStation and Switch pads), with matching button prompts.
- **A new main menu**, an ACHIEVEMENTS page, autosave, and one OPTIONS screen that includes the game's cheats.
- **Playable Bizarro** in free roam, built from the game's own Bizarro mission data.
- **A 3D costume viewer** in place of the Fortress of Solitude's costume screen.
- **A pause map that actually marks things:** the current objective, races, Bizarro rampages, kittens and city
  events, with your best times and scores.
- **A mod loader** with an in-game mod library (load order, conflicts). Suits added by mods show up in the
  costume viewer. I had a bunch of suits from another game running this way, but they're someone else's
  assets, so they aren't included.

**Tested on:** my PC (Windows 11, Ryzen 9 5950X, RTX 4070 Ti SUPER, 32 GB RAM, 4K 60 Hz screen) and an ASUS
ROG Xbox Ally X handheld. Nothing else, so other hardware is unknown.

**This project does not include any game assets. You must provide the files from your own legally acquired copy of the USA Xbox 360 disc of *Superman Returns* (2006) to build and run it.**

**AI use.** Nearly all of the code and documentation in this repository was written by AI agents, working under the direction of the project owner, Lachlan Oliver ([@loliver1823](https://github.com/loliver1823)). Most of it was written by Anthropic's Claude models, run through Claude Code; OpenAI's Codex wrote a smaller part (mainly integration, native-rendering and release-build work in October 2026) and gave second opinions on results. The owner set the goals and priorities, made the product and direction decisions, decided what ships, and judged the work by building, running and playing the game. He did not review most of the code line by line. Correctness is checked by machines instead: the Xenia emulator as a reference oracle, differential tests and the other checks in [`docs/VERIFICATION.md`](/docs/VERIFICATION.md). [`docs/HOW_IT_WAS_BUILT.md`](/docs/HOW_IT_WAS_BUILT.md) describes the process, including the mistakes.

> This README was drafted with AI assistance (Claude) for the project owner.

The game's PowerPC code is statically recompiled to x86-64 C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and runs on a hand-written host
platform (kernel, memory, threads, file system, input, audio, Direct3D 12 renderer). The game's
own code makes every decision; the host only provides what the console provided
([`docs/NATURAL_PATH_CHARTER.md`](docs/NATURAL_PATH_CHARTER.md)).

## Table of contents

- [Where it stopped](#where-it-stopped)
- [Getting it running](#getting-it-running)
- [System requirements](#system-requirements)
- [Features](#features)
- [Native status](#native-status)
- [Verification](#verification)
- [Compared with Xenia Canary](#compared-with-xenia-canary)
- [Known issues](#known-issues)
- [FAQ](#faq)
- [Contributing](#contributing)
- [Credits](#credits)
- [Licence](#licence)

## Where it stopped

Work paused on 2026-10-25. This repository is a snapshot of the development branch at that point. There is
no version number and no release build: nobody has played the game from start to finish on this code, which
was the project's own bar for calling anything a release.

What was reached, with the evidence in [`docs/VERIFICATION.md`](docs/VERIFICATION.md):
- **The whole game in automated runs.** A developer tool's chapter skip took one process from NEW GAME through
  every chapter to the ending and the post-game free roam, with the 3D world drawn in every chapter and no crash,
  hang or GPU device loss, on both test machines (2026-10-21). Loads, cutscenes and chapter advances were the
  game's own; the story objectives and fights were skipped, not played.
- **Native.** The game's graphics, audio and display calls run on native PC code: no Xbox 360 GPU commands,
  EDRAM or audio hardware are emulated on the default path (see [Native status](#native-status)).
- **Compared with Xenia.** Eleven chapter saves were compared with Xenia Canary at four points each (frames)
  and over 120 s of audio each; no new port defect was found in that sample.

Tested on two machines only:
- the development PC: Windows 11, AMD Ryzen 9 5950X, NVIDIA RTX 4070 Ti SUPER, 32 GB RAM, a 3840x2160 60 Hz display;
- an ASUS ROG Xbox Ally X handheld (AMD Radeon 890M graphics, Windows 11), for correctness runs, not timing.

The USA retail disc is the only supported edition.

What was still open when it paused (the plan's next steps): a person playing the whole game, winning the fights
by playing; getting the uncapped frame rate fully back to an earlier build's (see [Known issues](#known-issues));
bringing memory use under 6 GB; a 30-reload soak test; a documented mod API; real-controller and real-display
checks; and an installer.

## Getting it running

This is a **source-only** repository: there is no download. To play it you build it yourself, and you need a
USA disc of *Superman Returns* that you dumped yourself.

1. Extract your disc to a folder named `Superman Returns (USA)` that holds `default.xex` and `DATA/`
   ([`docs/DUMPING.md`](docs/DUMPING.md) has the steps and the hash to check).
2. Build from source with [`docs/BUILDING.md`](docs/BUILDING.md), including its one-off step for the
   point-sprite shader. The recompiled game code is generated from your own `default.xex` on your machine;
   nothing of the game is in this repository.
3. Run `build\SupermanReturns.exe` from the repository root. Saves and settings go to
   `%LOCALAPPDATA%\SupermanRecomp\content`.

The program reads the disc folder in place and never writes to it.

## System requirements

- Windows 10 or 11, 64-bit.
- An x86-64 CPU with SSSE3.
- A GPU with Direct3D 12 (feature level 11_0) and Shader Model 6.0.
- Memory: the release build commits about 7 GB at default settings (measured over a whole-game run, 2026-10-21).
- The USA Xbox 360 disc of *Superman Returns*, extracted ([`docs/DUMPING.md`](docs/DUMPING.md)).

These are what the code asks for, not measured minimums: only the two machines above were tested.

## Features

These are port features: on for players, off in the automated "natural" test runs that measure the
unmodified game.

### Front end, pause menu and autosave

A main menu (NEW GAME, LOAD GAME, OPTIONS, MODS, EXIT) built from the game's own menu screens: patched Apt
movies, applied to your disc's files at run time. The pause menu gains an ACHIEVEMENTS page (the game's own
18 achievements, kept with each save), an OPTIONS row and a QUIT TO TITLE / QUIT TO DESKTOP choice. Autosave
saves at each chapter advance through the game's own save prompt and save functions; it is the one feature
with an off switch ([`docs/CUSTOM_FRONTEND.md`](docs/CUSTOM_FRONTEND.md)).

### One OPTIONS screen

Tabs GAME, CONTROLS, DISPLAY, GRAPHICS, WORLD, AUDIO and, in game, CHEATS, with sliders, drop-downs and a
description of each option. GAME and CHEATS are the game's own options and cheats, changed through the game's
own functions ([`docs/GRAPHICS_SETTINGS.md`](docs/GRAPHICS_SETTINGS.md)).

### Keyboard and mouse

Keyboard and mouse bindings drive the game's own input commands, the objects its input manager
evaluates for actions such as ATTACK, instead of pretending to be a gamepad. Every condition the game applies
to an action still applies. OPTIONS > CONTROLS rebinds every action (up to four keys or mouse buttons each).
A mouse cursor works in every menu by calling the menus' own focus, select and slider functions, and a click
on the title screen is the game's own START ([`docs/PC_CONTROLS.md`](docs/PC_CONTROLS.md),
[`docs/MOUSE_CURSOR.md`](docs/MOUSE_CURSOR.md)).

### Controllers

Controllers through SDL3 (Xbox, PlayStation and Switch pads), hot-plug, rumble, and button prompts
that match the device in use ([`docs/INPUT_GLYPHS.md`](docs/INPUT_GLYPHS.md)).

### High frame rate

The game's simulation runs at a fixed 30 Hz; its separate render loop already draws from the two
nearest simulation states (the game's own EA::StateStream design). The port raises the render
loop's rate and removes two platform caps, so every displayed frame is a real render tick of the
game's own renderer. The port does not generate frames on the host. The default is
the display's refresh rate with VSYNC on; 30, 60, 120, any value and unlimited are settings
([`docs/HIGH_FRAMERATE.md`](docs/HIGH_FRAMERATE.md)).

### Resolution scaling

The internal resolution scales by whole multiples (Xenia's draw-resolution scaling): automatic 3x
for 2160p and 2x for 1440p and 1080p, up to 7x by hand, with bilinear, AMD FidelityFX CAS or FSR 1
upscaling and optional FXAA ([`docs/RESOLUTION.md`](docs/RESOLUTION.md),
[`docs/GRAPHICS_SETTINGS.md`](docs/GRAPHICS_SETTINGS.md)).

### Costumes, the costume viewer and playable Bizarro

The Fortress of Solitude's COSTUMES screen becomes a costume viewer that shows each suit and its cape as a 3D
model in an idle flight pose. Bizarro is a wearable character, built at start-up from your own disc's Bizarro
mission data, and the screen lists suits from mods next to the game's own ([`docs/MODS.md`](docs/MODS.md)).

### A useful pause map

The pause map marks the current objective, the races and Bizarro rampages (with par times, target scores and
your best), kittens and metro events, with a legend and filters; finished ones are dimmed
([`docs/MAP.md`](docs/MAP.md)).

### Mods

A `mods/` folder whose files override the game's own data, with an in-game mod library (search, categories,
load order, conflicts) under MAIN MENU > MODS ([`docs/MODS.md`](docs/MODS.md)). There is no documented mod API
yet: a mod replaces game files.

### Developer tools (off by default)

A level skip, a free camera and other developer tools, switched on with a `dev.txt` file. Runs and saves made
with them are tagged ([`docs/DEVTOOLS.md`](docs/DEVTOOLS.md)).

## Native status

The goal was a native port: no console hardware emulated at run time. The block below is generated by the
project's ledger tool from archived test evidence (it is copied from the private development repository, where
the ledger's evidence files live).

<!-- native-ledger:begin:readme -->
Computed by `tools/native_ledger.py` from the project's native obligation ledger (`docs/native_ledger.json`): every status below is derived from archived test evidence, never written by hand (digest `c82ec7dd0df34482`).

- **26 native obligations:** 26 closed, 0 in progress, 0 blocked, 0 open.
- **Still emulated per gameplay frame** (the default build, per in-game frame of a gameplay roll): 0.0 PM4 packets interpreted (the goal is 0); EDRAM-model operations 0.0 / 0.0 / 0.0 (resolves / transfers / loads); XMA register accesses 0.0; display-register answers 0.0; shared-memory page validations 0.0. Unmetered: none.

| Item | Status |
|---|---|
| Indexed draws go straight to native draw records (no PM4 encode for them) | closed |
| Shader binds and their constant loads are native records | closed |
| Begin/End and DrawVerticesUP draws are native records | closed |
| Tiled scissor / bin-mask writes inside the draw flush as native records | closed |
| Shared shader-constant SDK entry (default ON; bake proof pending) | closed |
| Viewport / scissor setters as native records | closed |
| Tile predication (SetPredication) as native records | closed |
| Resolves and clears as native records | closed |
| Draw-path leftovers: section fallbacks, split draws, declined resolves and clears | closed |
| The command ring is a host queue (the CP no longer parses the ring) | closed |
| Command-buffer segments as host records (no ring segments) | closed |
| Fences, swap, interrupts and the Z-pass replay without PM4 (default ON; bake proof pending) | closed |
| Per-tile shader constants (Warworld) without PM4 | closed |
| Occlusion queries (the HUD threat-marker glow) as native D3D12 queries | closed |
| Device creation without the boot PM4 block | closed |
| Display-register answers (vblank pacing) without emulated registers | closed |
| EDRAM clear transfers replaced by native clears | closed |
| Render targets are native D3D12 textures | closed |
| The presented frame comes from a native front target | closed |
| Scaled and 1x resolves run natively | closed |
| No EDRAM emulation in the shipping path | closed |
| Audio: no XMA hardware-register emulation | closed |
| Fixed: the 3D world sometimes drew black (a startup race) | closed |
| GPU memory reads as native resources (no shared-memory page model) | closed |
| Release binaries and package exclude all emulation components | closed |
| Release criterion C1: no emulation components and zero emulated hardware work in whole runs | closed |
<!-- native-ledger:end -->

**What is still compiled in.** The `default` CMake preset, the one [`docs/BUILDING.md`](docs/BUILDING.md) builds,
is the development build. It still contains the emulation layer that every native replacement was checked
against: the Xbox 360 GPU command (PM4) interpreter, the EDRAM model, the GPU shared-memory page model and the old
XMA audio path. On the default path that code does no work (the counters above are 0 per frame over whole-game
runs), but it is in the executable. The `release` preset (`cmake --preset release`, `SR_RELEASE_COMPOSITION=ON`)
leaves it out. `tools/player/composition_audit.py` checks a release build and its package against
[`docs/release_composition.json`](docs/release_composition.json); it passed on 2026-10-17 with no emulation unit
among 481 linked units. On a clean clone of this repository (2026-11-16) the release build links 498 units, none of
them emulation; the audit cannot finish there because the package step needs the private shader cache.

The release build is not free of unused code, though. It still links 42 diagnostic units that the audit lists
for review instead of failing: the native meter (the counter behind the numbers above), profilers, frame and
present dumps, a GPU-hang watchdog, log-only watches, and old forcing switches that do nothing unless
`SR_ALLOW_FORCING` is set. Most are off unless an `SR_*` environment variable turns them on. Whether they ship
was still an open decision when work paused.

## Verification

Each check is described in [`docs/VERIFICATION.md`](docs/VERIFICATION.md), with its repro command, the latest
result and what it does not cover.

| Check | What it compares | Latest result |
|---|---|---|
| Xenia trace oracle | Our render of a captured Xbox 360 GPU frame against Xenia's render of the same capture | Mean absolute difference (0-255): movies 0.00, menu 2.06, city gameplay 2.87-4.59 (close, not pixel-exact) |
| Chapter matrix | 11 chapter saves on our build and on Xenia Canary: frames at four points each and 120 s of audio | No new port defect in the sampled coverage (2026-10-21); see VERIFICATION for the audio window counts |
| L6 differential tests | One recompiled function against a PowerPC emulator, registers and memory | Every case passed on a clean clone of this repository; 21,876 Havok call cases bit-exact |
| Audio parity | Our audio output against Xenia's at the same points | Intro and title 78 of 83 three-second windows bit-exact; tutorial level -24.8 dB against Xenia's -24.9 (2026-10-18) |
| Flicker gate and scene floor | Every present for one-frame defects; frozen-simulation frames must be pixel-identical; the 3D world must be drawn | PASS on both test machines (2026-10-21) |
| Native meter and ledger | Emulated work per frame; each native replacement checked against the emulated path it replaces | 0 in every column over whole-game runs (see [Native status](#native-status)) |
| Release composition | What the release build compiled, linked and packaged | No emulation component (see [Native status](#native-status)) |
| No forcing | Host code may not do the game's work for it | `scripts/audit_forcing.py`: forcing number 0 |

## Compared with Xenia Canary

Measured on the development PC on 2026-10-21 (the release build of 2026-10-20, the project's step-4 close),
with one probe timing both programs' presents at the same driver call. Present fps and the 99th-percentile
present interval per scene; load times from the game's own events. Canary runs this game at its own 30 fps
render loop, and no Canary setting or patch lifts it. Method and caveats: [`docs/CANARY_COMPARISON.md`](docs/CANARY_COMPARISON.md).

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

How to read it:
- The movies are 30 fps content on both sides; above the 30 cap ours presents the held movie frame again, so
  the movie rows say nothing about smoothness beyond 30.
- At the console's 30 fps, Canary with vsync off paces as steadily as ours or better (lower p99 in most
  gameplay scenes); our win is the frame rate above 30.
- The uncapped numbers are about half those of an earlier build (2026-10-09, before the native draw path; its
  table is in [`docs/CANARY_COMPARISON.md`](docs/CANARY_COMPARISON.md)). Performance work after this run, up to the
  pause, brought uncapped gameplay (the same route's street and on-foot scenes) to about 116-119 fps at 1x and
  3x, against about 120 for the 2026-10-09 build. Those later builds were not compared with Canary.
- One run per configuration; live runs vary. The NEW GAME load time is not in the table: its end marker is
  only in the development build.

## Known issues

- **Nobody has played it through.** Every chapter was reached by the developer tool's chapter skip or a saved
  game. Winning the fights (Mongul, Metallo, Bizarro, the Twister and the rest) by playing is not verified.
- **Frame rate:** see the notes under the Canary table. The last builds ran uncapped gameplay at about
  116-119 fps on the development PC; 240 fps was not reached. Frame pacing at 120 Hz is unproven: the
  development PC has a 60 Hz display, and the handheld's 120 Hz panel was used for behaviour checks only.
- **Memory:** the release build commits about 7 GB, the development build more; the target was 6 GB.
  Quitting and reloading the same save grows memory by a few MB each time (about 4 MB per reload in a
  bounded test); the cause was not found.
- **Rarely, a load never finishes.** It happened once in 187 loads across the two machines (the 10th
  quit-and-reload of one save in one session) and in none of the 380 load screens watched since. If it happens, close the
  game with the window's X or Alt+F4 and load the same save again; in tests, closing during a load left the
  save files unchanged. The log (`superman_returns.log` next to the exe, replaced at each start) has
  `[LOAD_BEGIN]` / `[LOAD_DONE]` lines that show where a load stopped.
- **First start:** built from this repository, the game has no shader cache or pipeline list (both are made
  from the game's own shaders and cannot be published). It translates each shader the first time it appears,
  with a short hitch. The port's own point-sprite shader is not made by the build either: run
  `tools\build_point_gs.ps1` once ([`docs/BUILDING.md`](docs/BUILDING.md) step 5), or point-list draws are
  dropped.
- **Input and devices:** the controller triggers were checked with virtual Xbox 360 and DualShock 4 pads, not
  real ones. Real controllers, rumble, Alt+Enter, audio-device switching and 5.1 output were on the owner's
  hands-on checklist, which had not been done when work paused. Exclusive fullscreen was checked.
  D-pad down is unbound by default (nothing in gameplay reads it; it can be bound in CONTROLS). The mod
  library's search has no on-screen keyboard.
- **High frame rate:** water follows the game's 30 Hz look at high frame rates; arena combat effects and
  weather were not fully checked.
- **Rare render specks:** only on the ROG Xbox Ally X (Radeon 890M), a few pixels can change for one frame,
  rarely. The cause is not known.
- **Credits:** the credits screen is reached from the Fortress of Solitude's BONUS THEATER (tested on a
  post-game save).
- **Bugs of the original game** are kept. The faint line across the costume viewer's backdrop is one: Xenia
  draws the same frame the same way.

## FAQ

### Is this an emulator?

No. The game's CPU code is recompiled ahead of time to native x86-64, and its graphics, audio and display calls
run on native PC code. The emulation layer (the PM4 interpreter, EDRAM model and so on) is kept in the
development build only, as the test oracle, and the release build leaves it out (see
[Native status](#native-status)).

### Why only the USA disc?

The executable code is built from the USA `default.xex`. Other regions have different executables
and are not supported.

### Where are my saves and settings?

`%LOCALAPPDATA%\SupermanRecomp\content`, or `<game folder>\content` when a `portable.txt` file
sits next to the exe. Settings are `.ini` files in its `settings` folder
([`docs/SAVES.md`](docs/SAVES.md)).

### Where do I download it?

There is no download. Build it from source ([`docs/BUILDING.md`](docs/BUILDING.md)). Nothing from this project
is sold, and any site that offers a download of it is not this project.

### Does it run on Linux or the Steam Deck?

It was built and tested on Windows only. The renderer uses Direct3D 12.

### Will issues and pull requests be answered?

Maybe not: development is paused. The code is here to be used, forked and reused under its licence.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md), which covers AI use, the clean-room rules and the
verification a change needs. With development paused, a fork is the most reliable way to build on this.

## Credits

The port builds on [Xenia](https://github.com/xenia-canary/xenia-canary),
[XenonRecomp and XenosRecomp](https://github.com/hedge-dev) (hedge-dev),
[FFmpeg](https://github.com/has207/FFmpeg) (Xenia's XMA fork), [SDL3](https://www.libsdl.org/),
[Dear ImGui](https://github.com/ocornut/imgui), [xxHash](https://github.com/Cyan4973/xxHash),
[SIMDe](https://github.com/simd-everywhere/simde), [{fmt}](https://github.com/fmtlib/fmt),
NVIDIA FXAA, AMD FidelityFX CAS and FSR 1, the
[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler), Julio Cacko's
[FREE Input Prompts Pack](https://juliocacko.itch.io/free-input-prompts) (CC0) and the
[Saira](https://github.com/Omnibus-Type/Saira) font (OFL). [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp)
was a structural reference. The full list, with what is used from each project and its licence,
is in [`CREDITS.md`](CREDITS.md) and [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

**The authors of XenonRecomp, XenosRecomp, Xenia, Unleashed Recompiled and the other projects
credited here do not endorse this project and were not involved in it.**

*Superman Returns* was developed by EA Tiburon and published by Electronic Arts. Superman and all
related characters and elements are trademarks of DC Comics. This project is not affiliated with
or endorsed by Electronic Arts, Warner Bros. or DC Comics.

## Licence

The source code of this project is licensed under the MIT licence ([`LICENSE`](LICENSE)): take any of it
and use it for anything, open or closed, as long as the copyright notice goes with it. The third-party
components keep their own licences, listed in
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md#our-own-code) (FFmpeg's XMA decoder, for one, stays
LGPL-2.1 as a separate DLL). No licence to the game is granted or implied.
