# Verification

> Drafted with AI assistance (Claude) for the project owner.

Nearly all code here was written by AI agents and most of it was not reviewed line by line by a
person. This page lists the checks that stand in for that review: what each one compares, how to
run it, its latest result and what it does not cover. Results are dated; "dev" means the
development branch the public repository is exported from. Development paused on 2026-10-25, so
nothing here is newer than that. "The step-4 close" (2026-10-21) is the project's whole-game
correctness milestone: one frozen build pair (development and release builds of one commit), every
closing run on it, each graded by a pass rule written before the run.

Several checks need data that cannot be published because it is captured from the game: Xenia GPU
traces, Xenia memory dumps and audio captures. The tools to capture your own copies are in this
repository; the captures are not.

## The reference: Xenia

[Xenia Canary](https://github.com/xenia-canary/xenia-canary) is the oracle. When our build and
Xenia disagree, our build is assumed wrong until a test shows otherwise. Three forms are used:

- **GPU traces** (`.xtr`): one frame's complete GPU command stream, textures and vertex data,
  captured from Xenia while the game runs. Xenia's `xenia-gpu-d3d12-trace-dump.exe` renders a trace
  to a PNG, deterministically.
- **Golden memory dumps**: full guest-memory minidumps taken from Xenia at known game points
  (main menu, gameplay), read by `tools/golden.py` without a debugger.
- **Live Xenia** under Frida, for audio capture and story state (`tools/XENIA_HARNESS.md`).

## Checks

### 1. Xenia trace oracle (renderer)

- **Compares:** our renderer's output for a captured frame against Xenia's render of the same capture.
- **Repro:** capture a trace from your own copy with Xenia built from `tools/xenia-trace/`
  (`tools/xenia_gpu_trace.ps1` at a fixed time, `tools/xenia_f4.ps1` at a scene you navigate to,
  `tools/XENIA_HARNESS.md`), then:
  ```
  xenia-gpu-d3d12-trace-dump.exe --async_shader_compilation=false --trace_dump_path=<out> <trace>.xtr
  .\tools\replay_ab.ps1 -Tag <name> -Trace <trace>.xtr        # ours, about 3 s, deterministic
  python tools/pm4_replay_score.py                            # all traces, mean absolute difference
  python tools/native_replay_check.py                         # ours against a previous build, byte for byte
  ```
- **Latest (2026-10-06, `docs/RESOLUTION.md`):** mean absolute difference on a 0-255 scale at 1x:
  four movie frames 0.00; press-start menu 2.06; two city frames 4.59 and 2.87; two frames of our
  own live gameplay 5.48 and 4.66. The same at 2x and 3x against Xenia's 2x and 3x.
  `native_replay_check.py`: 9 of 9 traces byte-identical between the current build and the build
  before each native change (2026-10-09).
- **Not covered:** gameplay frames are close, not pixel-exact. Nine frames cover the menu, the
  movies and two city views; arenas, interiors, weather and later chapters have no trace (the
  chapter matrix below compares them live instead).

### 1b. Chapter matrix against Xenia (whole game, sampled)

- **Compares:** chapter saves loaded in our build and in Xenia Canary: frames at fixed points after
  the load, and the audio over the first 120 s, chapter by chapter.
- **Repro:** `python tools/oracle/chapter_matrix.py xenia|ours|sheets|audio|report`, from chapter saves you
  make yourself; [`docs/TOOLS_INDEX.md`](TOOLS_INDEX.md) lists the arguments.
- **Latest (the step-4 close, 2026-10-21):** 11 chapter saves at four points each. The port defects found
  on 2026-10-18 in that scope were fixed (the Warworld arena floor, the city's window panes, heat vision),
  and no new port defect was found. Audio: 414 of 898 three-second windows matched in their own save; the
  rest were classed by matches in other saves and builds, by metro events and music cues, and for the
  arena fight by kind of sound.
- **Not covered:** saves 04, 06, 08, 09, 15, 16 and the one after Warworld were not in the matrix. A
  sound found in another run shows the port can play it, not that each scene plays it at the right moment.

### 2. Golden memory dumps (game state)

- **Compares:** values in our guest memory against the same addresses in a Xenia dump taken at the
  same game point.
- **Repro:** `python tools/golden.py --help` (reads, searches and diffs a dump).
- **Latest:** used per investigation, for example the audio mixer globals at `0x82976040..74` match
  the main-menu dump, and the story-state fields match Xenia at the Warworld checkpoints of a
  level-skip plan (`docs/DEVTOOLS.md`).
- **Not covered:** there is no automatic whole-memory comparison. Only the fields an investigation
  looked at are checked.

### 3. L6 differential tests (recompiler output)

- **Compares:** one recompiled function against an independent PowerPC implementation, on the same
  inputs: registers and every byte of memory it writes. Two oracles: Ghidra's PowerPC emulator for
  leaf functions, and a separate PowerPC/VMX128 interpreter (`scripts/difftest/golden_call/xenon_interp.py`,
  itself checked against QEMU) for whole call trees on golden memory.
- **Repro:** `scripts/difftest/README.md`, `docs/DIFFTEST_DESIGN.md`, `docs/TOOLKIT.md` 1.3 and 1.3b:
  ```
  cmake --build build --target difftest
  python scripts/difftest/compare.py --all
  python scripts/difftest/golden_call/compare_run.py <script> --mode recomp
  ```
- **Latest:** leaf suite 51 passed, 0 failed. Golden-call: 21,876 Havok collision cases (about
  1.29 billion guest instructions) bit-exact against the interpreter.
- **Not covered:** these test chosen functions, not the whole program. Most of the program is
  checked only indirectly, by the game behaving like it does in Xenia. A differential is run
  whenever a recompiled function is suspected; "miscompiled" is never claimed without one.

### 4. Audio parity

- **Compares:** our audio output against Xenia's at the same points, captured silently from both.
- **Repro (`docs/TOOLKIT.md` 1.15):**
  ```
  py -3.13 tools/xenia_audio_capture.py            # Xenia, the oracle
  powershell -File tools/audio_roll.ps1 -Out <dir>  # ours
  python tools/audio_oracle_compare.py compare ...  # also: eras, spec, info
  ```
- **Latest (2026-10-18, the SDL3 build):** intro and title: 83 of the three-second windows matched, 78 of
  them bit-exact (the SDL2 build: 83 and 77-78); the opening cinematic and Warworld: 58 matched, 32
  bit-exact, and one 115 s bit-exact stretch; the tutorial level -24.8 dB against Xenia's -24.9 dB; 5.1
  channel shares within 0.1-0.2 points. Offline, the SDL2 and SDL3 output paths wrote identical device
  bytes in 20 of 20 cases. Earlier (2026-10-07): gameplay music correlation 0.97-0.99 at 0.0 dB, and
  the sound-effect residual -39.7 dB against Xenia's own -39.6 dB.
- **Not covered:** pause-menu sounds were not compared separately; the full game was not compared
  end to end.

### 5. Flicker gate and scene floor (renderer, timing)

- **Compares:** frames of our own build against each other, in three ways:
  - every present of a gameplay span, scored for one-present outliers (A-B-A);
  - a frozen simulation (`SR_DIAG_SIM_FREEZE`): while the game's state is held still, consecutive
    presented frames must be pixel-identical; this is the exact part of the gate;
  - the scene floor: the 3D world must be drawn (median brightness of the centre region at least 8
    of 255 and at most 60 % near-black), because a black world under a working HUD passes the other
    two.
- **Repro:** `python tools/perf/flicker_gate.py --exe <exe> --out <dir>`, or
  `python tools/check_roll.py --tag <name> --flicker`. It is required for every change to rendering,
  draw timing, caching or threading.
- **Latest (the step-4 close, 2026-10-21):** all six gates PASS, on the development PC and on the ROG
  Xbox Ally X. One flagged block on the handheld (a car's tail-light moving into the frame edge) was
  viewed and ruled a detector false positive.
- **Not covered:** the gate runs one gameplay scenario. It measures our build's consistency, not
  correctness against Xenia; the scene floor is a floor, not an oracle.

### 6. Native meter and native ledger (emulation removed)

<a id="native-meter"></a>
- **Compares:** the meter counts the console-hardware emulation still running per frame: PM4
  packets interpreted, EDRAM emulation operations and XMA register accesses. Each native replacement
  is checked against the emulated path it replaces before it becomes the default:
  - a generation CHECK (the replacement's output against the bytes the console library would have
    produced),
  - an execution CHECK,
  - a per-present A/B under a frozen simulation, the replacement and the original alternating every
    frame, which must be pixel-identical.
- **Repro:** a gameplay roll with `python tools/perf/perf_run.py --scenario gameplay --config port
  --exe <exe> --out <dir>`, then `python tools/perf/native_meter.py <dir>` or
  `python tools/perf/meter_delta.py <dir A> <dir B>`. `docs/NATIVE_ROADMAP.md` lists each stage's
  CHECK gate.
- **Latest (the step-4 close, 2026-10-21):** every column 0 (PM4 packets, EDRAM resolves / transfers /
  loads, display-register answers, shared-memory page validations, XMA register accesses) over a
  whole-game run of the development build on the PC (43,706 frames) and of the release build on the
  handheld (78,567 frames). The native ledger: 26 of 26 obligations closed (2026-10-17; the README's
  generated block).
- **Not covered:** the meter counts the emulated work it was built to count. The ledger's evidence
  (run logs, captures) is in the private development repository, so a public clone cannot re-grade it.

### 7. No forcing (the game makes its own decisions)

- **Checks:** that no host code does the game's work for it. Every host override of a game function
  is classified in `docs/OVERRIDES.json` (observe, platform fix, port feature, developer tool,
  diagnostic, band-aid); diagnostics and band-aids sit behind one master switch,
  `SR_ALLOW_FORCING`, off by default ([`docs/NATURAL_PATH_CHARTER.md`](NATURAL_PATH_CHARTER.md)).
- **Repro:** `python scripts/audit_forcing.py` (fails on any unregistered or ungated site).
- **Latest (2026-10-25):** forcing number 0: no ungated site and no band-aid active in the default build.
  The registry has 950 entries: 787 observe, 80 platform fix, 29 diagnostic, 27 band-aid (each with a
  retirement condition, off by default), 12 native, 12 port feature, 3 developer tool. The generated code
  carries no hand edits: the old hand-edited bodies were converted to registered hooks on 2026-10-09.
- **Not covered:** the audit proves that every override is registered and gated, not that each
  "observe" entry is truly read-only; an observe entry seen writing is demoted for review
  ([`docs/BANDAIDS.md`](BANDAIDS.md)).

### 8. Whole-run checks

- **Checks:** many per-feature CHECK lines graded from one whole-run log; an entry without a pass
  pattern grades as a failure, never as a pass.
- **Repro:** `python tools/check_roll.py --tag <name>` (entries in `tools/check_roll.json`).
- **Latest (2026-10-25):** 41 enabled checks.
- **Not covered:** each check covers its own feature only.

### 9. How far the unmodified game gets

- **Checks:** with port features off, no mods and forcing off, how far the game goes on its own.
- **Repro and record:** [`docs/NATURAL_BASELINE.md`](NATURAL_BASELINE.md),
  [`docs/PLAYTHROUGH.md`](PLAYTHROUGH.md).
- **Latest (2026-10-05 and 2026-10-09):** NEW GAME to the ending movie and post-game free roam, 0
  crash markers. A developer tool passed each story objective (bosses after a hold of about 60 s),
  wrote the experience cap 15 times and locked one event during the Twister fight; loads,
  cutscenes and chapter advances are the game's own.
- **The step-4 close (2026-10-21), port features on:** six whole-game runs by chapter skip on both
  machines and both builds, each SUCCESS: 17 of 17 chapters and saves, 0 crash, device-removed, GPU-hang
  or fatal markers, the world drawn in every chapter, and on the two runs that ended by the game's own
  quit, a complete shutdown with no thread left stranded.
- **Not covered:** winning the fights by playing; a human playthrough.

### 10. Release contents

- **Checks:** a release package carries no game bytes (no file identical to a disc file, no 4 KiB
  block shared with a disc file, no game-derived data class) and no secrets; every dependency is
  credited and its licence text ships.
- **Repro:** `python tools/player/release_lint.py <package> --disc "<...>\Superman Returns (USA)"`,
  `python scripts/check_credits.py`.
- **Composition:** the `release` CMake preset (`SR_RELEASE_COMPOSITION=ON`) leaves the emulation layer
  out. `tools/player/composition_audit.py` reads what that build compiled, linked and staged (ninja's
  dependency records, the link lines, the PDB, the package) and classifies every item by
  `docs/release_composition.json`; an unclassified item fails. Latest: PASS on 2026-10-17 (481 linked
  units, no emulation unit, header, marker or link input). On a clean clone of the public repository
  (2026-11-16): 498 linked units, none of them emulation; 42 diagnostic units listed for review (an open
  owner decision); INCOMPLETE, because the package step needs the private shader cache.

## Second opinions

Visual milestones, and from 2026-10-14 each merge, also got one blind review from a second AI model
(OpenAI Codex) before they were recorded: it described the frames and their defects before it saw our interpretation, then compared
them with the Xenia frame. Where it disagreed, the measurement decided.

## What is not verified anywhere

- A full human playthrough on a release build.
- Hardware other than the development PC and the ROG Xbox Ally X.
- Real controllers, displays above 60 Hz on the PC, and audio-device changes.
- Bit-exact rendering in gameplay.
- The parts of the game no trace, roll or test reached.
