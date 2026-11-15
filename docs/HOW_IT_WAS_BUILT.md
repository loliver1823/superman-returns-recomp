# How this port was built

> Drafted with AI assistance (Claude) for the project owner.

**AI use.** Nearly all of the code and documentation in this repository was written by AI agents, working under the direction of the project owner, Lachlan Oliver ([@loliver1823](https://github.com/loliver1823)). Most of it was written by Anthropic's Claude models, run through Claude Code; OpenAI's Codex wrote a smaller part (mainly integration, native-rendering and release-build work in October 2026) and gave second opinions on results. The owner set the goals and priorities, made the product and direction decisions, decided what ships, and judged the work by building, running and playing the game. He did not review most of the code line by line. Correctness is checked by machines instead: the Xenia emulator as a reference oracle, differential tests and the other checks in [`docs/VERIFICATION.md`](/docs/VERIFICATION.md). [`docs/HOW_IT_WAS_BUILT.md`](/docs/HOW_IT_WAS_BUILT.md) describes the process, including the mistakes.

This page is for engineers and the decompilation and recompilation community. It says what was
built, who did what, how results were checked, what is still unverified, and what went wrong.
Commit hashes below refer to the private development history; the public repository starts from a
fresh export of it. Development paused on 2026-10-25; this page describes the project at that point.

## 1. What this is

A static recompilation of the Xbox 360 game *Superman Returns* (EA Tiburon, 2006) to a Windows
x86-64 executable. It is not a decompilation: there is no hand-written C source of the game. The
game's PowerPC machine code is translated to C++ by a recompiler, and that C++ runs on a host
platform written for this project.

## 2. Who did what

### The owner

- Chose the project, its goals and their order. Recorded owner decisions, in date order:
  - 2026-09-29: the GPU command stream (PM4) draws, with Xenia's GPU code as the reference;
  - 2026-09-30: the shipping path must be fully native before mod work;
  - 2026-10-02: frame rate and resolution are priorities;
  - 2026-10-06: PC controls must act on the game's own functions, not on virtual button presses;
  - 2026-10-07: real frames only, no host frame generation;
  - 2026-10-08: an automated flicker gate before any rendering change ships;
  - 2026-10-09: the licence (GPL-3.0; GPL-3.0-only from 2026-10-23, then MIT from 2026-11-16 so anyone
    can reuse any part of it) and a public repository made by an allowlist export of the private one;
  - 2026-10-11: a written release plan with criteria, worked through in order, each milestone reviewed;
  - 2026-10-15: a mod API joins the release scope (not built when work paused);
  - 2026-10-18: routine approvals the agents would have asked him for go to a separate Claude session
    instead; publishing, his hardware and his own play testing stay his;
  - 2026-11: development paused, and the code published as it is.
- Made the product calls: which port features exist, what is on by default, the licence, what
  ships.
- Ran and watched builds and played them. Several defects were found this way, including the
  intro-movie noise regression and the black 3D world described below.
- Did not write most of the code and did not review most of it line by line. From 2026-09-29 the
  agents worked without per-change approval; the owner gave direction and decisions, not code
  review.

### The AI agents

- Claude models through Claude Code wrote most of the code, tools and documentation, ran the
  builds, the test runs and the comparisons, and recorded the results. Of 4,336 commits in the
  private history (2026-06-25 to 2026-10-25), 3,895 carry a Claude co-author trailer and 481 are
  merges. Models named in the trailers: Claude Opus 4.8, Opus 5, Opus 5.5, Fable 5 and Fable 5.1.
- Several agent sessions ran in parallel, each on its own git worktree and branch (a "lane"), and
  merged into one development branch. One game process ran at a time; a lock file serialised them.
- OpenAI's Codex was used four ways: as a reviewer of claimed results (2026-08-10 to 2026-09-25,
  retired because the review overhead was too high); for one integration pass of nine lanes
  (2026-10-03); for implementation sessions on the native renderer and the release build
  (2026-10-11 to 10-17: most of the roughly 120 commits of that week without a Claude trailer);
  and for a blind second opinion on visual milestones and merges.

## 3. The pipeline

### 3.1 CPU: static recompilation

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) translates every PowerPC function of the
  retail `default.xex` to C++ (`guest/ppc/`, generated, never committed). Its input is
  `guest/config/` (function boundaries, switch tables).
- The build regenerates `guest/ppc/` at configure time from the builder's own `default.xex`
  (`tools/regen/regen.py`): the pinned upstream XenonRecomp plus local patches
  (`tools/regen/patches/`) that add instructions it did not implement, fix switch-table bounds and
  dispatch, add the CR6 update for two vector compare forms, give integer division PowerPC
  semantics and route raw guest addresses through the address translation.
- Deterministic transforms run after generation (`tools/regen/README.md`):
  - every raw guest memory access is wrapped in the guest-address translation (about 43,000 sites);
  - integer division gets PowerPC semantics for a zero divisor (976 sites);
  - dropped vector instructions are repaired;
  - guest registers become C++ locals where the call ABI allows (verified: 0 differing functions
    of 34,511 against the unlocalised tree).
- **The hook model.** The host changes or observes a game function by defining it in host code
  (`PPC_FUNC(sub_XXXXXXXX)`) and calling the recompiled original (`__imp__sub_XXXXXXXX`) from it.
  Every such override is registered and classified in `docs/OVERRIDES.json`; `scripts/audit_forcing.py`
  fails on an unregistered one.
- **No hand edits.** Until 2026-10-09 the tree spliced about 310 hand-edited function bodies into
  the generated code. They were converted to host hooks (mid-function hooks declared in
  `tools/regen/superman_regen.toml`, and function overrides), so the build runs XenonRecomp's own
  output and nothing generated is edited by hand (`tools/regen/README.md`).

### 3.2 The host platform

- `kernel/` (about 145,000 lines of C++) implements what the console provided:
  - the kernel and XAM imports (214 import thunks are routed);
  - threads, synchronisation, guest memory and physical-memory mapping;
  - file I/O against the player's disc;
  - content and profile storage for saves, and input.
- [Xenia](https://github.com/xenia-canary/xenia-canary) is the semantic reference for this layer.
  Where its code was ported, the file names its Xenia source (BSD-3-Clause; `CREDITS.md`).
- `app/` is the window and main loop on SDL3 (SDL2 until 2026-10-18).

### 3.3 Graphics

- **Where it started.** The game's Direct3D 9-style calls (the Xbox 360 D3D library is linked into
  the game) build Xbox 360 GPU command packets (PM4). Until October 2026 a command processor in this
  program interpreted them and drew with Direct3D 12 (`gfx/pm4_engine`, `gfx/d3d12_pm4_*`), with
  Xenia's GPU code as the reference for every semantic; deviations from it were treated as bugs.
- **Shaders.** Xenos shader microcode is translated to DXIL by
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), ahead of time into a cache and at run
  time for misses (DXC loaded at run time).
- **Becoming native.** That command stream is an emulation layer, and it was replaced piece by
  piece in two strands:
  - **Draw builder.** The console library's flush, bind and draw-building code is replaced with
    host code that records draws directly (`kernel/native_flush.cpp`); the ring buffer is
    replaced with a host queue (`kernel/gpu_ringless.cpp`).
  - **Render targets.** Native render targets and resolves replace the EDRAM emulation through a
    surface table (`gfx/nb_surfaces.hpp`).

  Each replacement was checked against the path it replaces before it became the default
  ([`docs/VERIFICATION.md`](VERIFICATION.md) section 6). PM4 packets interpreted per in-game frame
  went from 60,776 (2026-10-07) to 1,769 (2026-10-09) and to 0 in gameplay on 2026-10-16; on 2026-10-17 every
  counter of the native meter read 0 over whole runs of every era, and the 26 obligations of the
  native ledger closed ([`docs/NATIVE_ROADMAP.md`](NATIVE_ROADMAP.md)).
- **The release build.** The emulation layer stays in the development build as the test oracle. The
  `release` CMake preset compiles it out, and `tools/player/composition_audit.py` checks what that
  build compiled, linked and packaged against `docs/release_composition.json`.
- **Observe layer.** A layer at the D3D API boundary (`gfx/hle_video`, `kernel/hle_api_observe`)
  names resources and draws for tools and mods. It does not draw.

### 3.4 Audio

The game's XAudio and XMA library calls are served natively: a port of Xenia's audio system and XMA
decoder, decoding through Xenia's FFmpeg fork (`ffmpeg_xenia.dll`). No XMA hardware registers are
emulated (0 register accesses in a whole run).

### 3.5 Port features

The custom front end, autosave, keyboard and mouse, controllers, high frame rate, resolution
scaling, mods and developer tools are described in the [README](/README.md#features). Each feature
is on for players and off in the natural test runs, through one switch (`portFeaturesOn()` in
`kernel/host_automation.hpp`).

## 4. How results were checked

### The no-forcing rule

Early in the project the agents made the game progress by doing its work for it: calling game
functions the game had not called, writing state it had not written, faking return values. Metrics
moved and the game did not actually work. The charter
([`docs/NATURAL_PATH_CHARTER.md`](NATURAL_PATH_CHARTER.md)) now separates two kinds of change:

- a **platform fix** implements what the console provided, and is always allowed;
- **forcing** does the game's work for it, and is allowed only as a named diagnostic behind
  `SR_ALLOW_FORCING`, which is off by default.

The progress record ([`docs/NATURAL_BASELINE.md`](NATURAL_BASELINE.md)) counts only what the game
does on its own, with a repro command per entry.

### Oracles over judgement

- **Xenia is the reference.** "Better than our previous output" is not accepted as "correct".
  Renderer changes are first tested on Xenia GPU traces replayed deterministically and scored
  against Xenia's own render of the same trace. Live runs come second.
- **The recompiler is a suspect like any other code.** A function is not called "miscompiled"
  without a differential test against an independent PowerPC implementation (the L6 test).
- **Counts from logs** are checked against the log's own sampling and caps before they are quoted
  (`tools/audit_log_gating.py`).
- **Whole runs.** A visual result is recorded only after frames from every era of a whole run were
  looked at, because a change judged on title-screen frames once broke every movie frame.

[`docs/VERIFICATION.md`](VERIFICATION.md) lists every check with its latest result.

## 5. Verified and not verified

Verified, with the evidence in [`docs/VERIFICATION.md`](VERIFICATION.md):
- the game runs from NEW GAME to its ending and the post-game, without crashing, in automated runs
  where a developer tool skipped each chapter, on both test machines;
- audio matches Xenia at the compared points;
- the renderer is close to Xenia on the captured frames, and eleven chapter saves showed no new port
  defect against Xenia at the sampled points;
- no console hardware is emulated per frame on the default path (the native meter reads 0 over whole
  runs), and the release build contains no emulation component;
- the recompiled functions that were tested match an independent implementation.

Not verified:
- a human playthrough on a release build;
- winning the boss fights by playing;
- bit-exact gameplay rendering;
- other hardware;
- the parts of the game no test reached;
- real controllers, displays above 60 Hz and audio-device changes (left to a hands-on checklist that
  was not done).

## 6. Mistakes made and caught

These are the larger ones. Each changed how the project works.

- **Months of progress that were partly forced** (before 2026-07-13). The harness and many
  band-aids drove the game through sequences it was not running itself. At iter-294, after about 100 iterations of
  menu forcing, a check showed the harness set the game's quit flag and ran its teardown as the
  "tick". The no-forcing charter (2026-07-13), the override registry and the forcing audit came
  from this. Later burndowns removed about 5,500 lines of forcing (iter-378) and showed that an
  earlier claimed gameplay reach had been forced.
- **A title screen called "pristine"** (iter-450T). Its press-start text was white placeholder
  boxes. It had been judged against our own earlier output, while a Xenia frame of the same screen
  sat unused. Since then results are judged against Xenia, never against our own old output.
- **Movie frames broken by a title-screen check** (iter-450N, found and fixed in iter-450Q,
  `745d9a67`). A change was verified on title frames only and broke every movie frame. Since then
  visual results need frames from the whole run.
- **A substitution the registry could not see** (iter-395, `cfd2e01e`). A guest-side allocator
  substitution with an out-of-bounds bug caused a wall of effect failures. The override scan only
  covered `kernel/`; it now covers the guest code too.
- **The recompiler dropped instructions silently** (iter-434e, `743168e9`). 59 sites of two vector
  instructions (`vsel128`, `vandc`) produced no code, which caused an infinite recursion in a camera
  sweep. An earlier note had called the recompiler "instruction-complete". Fixed by a post-regen
  repair and a recompiler patch.
- **Intro movies rendered as noise** (`a3647e9`, fixed by `f4837e7c`). A renderer change made the
  movies noise for about two hours of commits; the owner noticed. The intro era is now checked on
  every renderer change.
- **A heap-corruption fix that was only correlation** (`89f4afd`). The real cause was a stubbed
  `ObReferenceObjectByHandle`, which led to a use-after-free write into an allocator free list
  (fixed by `60a90bf`: 130 corruption events to 0).
- **An audio command-buffer overrun** (D-AUDIO-CMDBUF). The cause was a signed `p % 16` alignment
  meeting our allocation addresses above `0x80000000`. Fix proven 2 of 2 rolls failing without it,
  0 of 3 with it.
- **The black world misattributed** (2026-10-09).
  - The 3D world sometimes drew black under a working HUD. A merged shader-splice fix was blamed
    and switched off (`0729341b`); that commit admits nobody had looked at the gameplay frames.
  - The same day it was retracted (`6725dd95`): the black world reproduced with the fix off, and
    the on/off tally over about ten runs had been chance.
  - The cause was a startup race (`41572437`). The command processor started before the Direct3D 12
    backend was ready, its queue saturated, and a NaN poisoned the game's eye-adaptation history.
  - Fixes: the command processor now waits for the backend, and the flicker gate got a scene floor,
    because a stable black world had passed both of its other checks.
- **One-present flicker** (2026-10-08, merged in `740f0768`). A native replacement stored a dedup
  cache entry while planning a section that could later abort, so a later draw ran with stale
  constants for one frame. The replacement's own check compared generated bytes and missed it; the
  owner saw it. A mutation test showed the store order was the whole difference. The flicker gate
  became mandatory for rendering, timing, caching and threading changes.
- **Overstated summaries.** The agents' own summaries overstated results more than once (for
  example "59 instruction classes" for 59 sites of two instructions, and "a year of forcing" with
  no source for the length). The development log
  carries a warning that older entries are testimony, not settled fact. Results are now recorded
  with their evidence, a repro command and what is still broken.
- **Review overhead.** For about seven weeks every claim went through a written review by a second
  model with a ledger entry per claim. Roughly a third of commits went to that process (a rough
  count; not measured precisely). It was retired on 2026-09-29 in favour of mechanical oracles and
  one blind second opinion per visual milestone.

## 7. References and clean-room rules

- **Xenia** (BSD-3-Clause): the behavioural reference for the platform, GPU and audio layers.
  Ported code names its source file; `CREDITS.md` lists every ported file.
- **Unleashed Recompiled** (GPL-3.0): read for structure (D3D12 host, render thread, pipeline cache,
  hook surface). A line-level scan on 2026-10-02 found none of its code in this tree.
- **OpenSAGE** (GPL-3.0): documentation of EA's Apt UI format. No code used.
- **The game itself**: analysed with IDA Pro and Ghidra. Function names in the code are names given
  during that analysis, with the address kept in the name (`..._sub_82721E68`).
- **The October 2006 prototype** of the game published by Hidden Palace was used only as a read-only
  reference: to name functions in the retail executable, and as the description of a few developer
  tools (a free-camera switch and some cheat commands), which were rebuilt on the retail game's own
  functions. None of its code or data is in this repository.
- **Not used:** no Xbox 360 SDK files are in this repository. The console's D3D and audio library
  code was studied from the game's own executable and from Xenia's reimplementation.
- **Not in this repository:** the game's code, data, shaders, GPU captures, memory dumps and
  screenshots. They stay in the private development repository or are produced from your own disc
  at build time.

## 8. Numbers

| Item | Value (2026-10-25, when work paused) |
|---|---|
| Commits in the private history | 4,336 (from 2026-06-25; earlier work was in a previous repository) |
| Commits with a Claude co-author trailer | 3,895 (481 of all commits are merges) |
| Host code | `kernel/` 296 files, about 145,000 lines; `gfx/` 146 files, about 99,000 lines |
| Functions compared in the register-localisation check | 34,511 (2026-10-09) |
| Override registry entries | 950 (787 observe, 80 platform fix, 29 diagnostic, 27 band-aid, 12 native, 12 port feature, 3 developer tool) |
| PM4 packets interpreted per in-game frame | 0 (60,776 on 2026-10-07) |
