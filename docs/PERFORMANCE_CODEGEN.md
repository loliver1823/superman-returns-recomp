# Guest codegen performance (iter-464/465 codegen lane, branch `perf/codegen`)

Goal: faster recompiled guest code for the 60-120 fps target. Mechanism: XenonRecomp's
register-as-local options (`skip_lr`, `skip_msr`, `ctr_as_local`, `xer_as_local`,
`reserved_as_local`, `cr_as_local`, `non_argument_as_local`, `non_volatile_as_local`) applied
to every GENERATED function, hand bodies kept ctx-style, the two styles joined by solved ABI
bridges. Design and workflow: `tools/regen/README.md` "Register localization".
Companion (host side, perf lane): `docs/PERFORMANCE.md`.

## What is done

- `tools/regen/flag_transform.py`: textual application of the eight flags to a ctx-style
  regen. Equivalence proof against the recompiler's own flag handling: two raw regens of the
  XEX (`tools/regen/variants/base.toml` flags off, `variants/all.toml` flags on),
  `--equivalence` -> **0 differing functions of 34,511** (`archive/v1_codegen/equiv2.txt`).
- `tools/regen/abi_audit.py` + `tools/regen/abi_bridge.json`: whole-program dataflow over the
  deployed tree (hand bodies included). Result: 393 bridged functions (369 IN, 30 OUT), 301
  unbridged IN roots with no direct caller (log `[ABI_UNBRIDGED]` once if they run), 47
  no-return functions, excluded setjmp `sub_827A45E0` (`archive/v1_codegen/solve2.txt`).
- Policy `[localize]` in `tools/regen/superman_regen.toml` (all eight flags on) plus
  `keep_ctx_style = ["sub_822C6E30"]` (iter-465): dev's configure-time
  `tools/regen/cloth_capacity.py` copies the ctx-style body text of the cloth tangent
  function into `kernel/cloth_capacity.cpp` and hash-checks it, so that one generated
  function stays ctx-style (bridged like a hand body). Deployed tree: 30,228 generated
  bodies localized, 312 ctx-style bodies kept (311 splice bodies + the cloth function).
- Flags-off baseline artefacts for A/B: `archive/v1_codegen/base/SupermanReturnsGuest.lib`
  (ctx-style guest lib of the same generated code) and `tools/regen/relink_exe.py` (link the
  game against either guest lib with the SAME host objects: an identical-host A/B exe).
- `scripts/difftest/golden_call/run_script.py --watch <dword VA>` (iter-465): the clean-room
  oracle reports every guest store to a dword (pc, lr, address, value, registers) - how the
  ig_s divergence below was attributed in three 8 s runs.

## Correctness evidence (iter-465, on dev 31c522d6 merged: worktree `sr-wt-codegen`)

Localized build = `build/difftest.exe` / `build/SupermanReturns.exe` of the worktree; baseline =
dev's own ctx-style build (`<repo>/build`, same commit).

| Gate | Result |
|---|---|
| L6 differential, every captured golden (`scripts/difftest/compare.py --all`) | **54 pass, 0 fail** |
| golden_call vs the clean-room oracle (`compare_run.py --mode recomp`): types 241 cases, gold 6, aabb 344, ray_s 344 | **0 divergent** each |
| golden_call ig_s (hkCharacterProxy::integrate, 26 cases) | 4 divergent, all one class (below); the other 22 cases and every explicit dump (positions, velocities, contact arrays: ~900 lines) identical |
| difftest suites, all 48 `--*-test` entries of the merged `difftest_main.cpp`, same env/cwd (`archive/v1_codegen/suites_{base,local}/summary.txt`) | 47 equal exit codes (incl. the pre-existing non-zero ones: physalloc 4, physresolve-grant 11, tex-census 1, mod-overlay 1 (M18l, dev too), icall 0xC1CA11FA = the designed stop); `arena-wait-test` 3/3 rc 0 on both |
| `--xma-test` | 2 failures on the worktree exe ONLY because the worktree was configured without `SR_FFMPEG_XENIA_SRC` (XMA decoders 0/320); identical runtime DLLs; re-verified after reconfiguring with dev's cache values (see the status block) |
| `[ABI_UNBRIDGED]` during every golden_call / suite run | 0 |
| residual `ctx.<localized reg>` references inside localized bodies (`archive/v1_codegen/scratch/residual_ctx.py`) | 1: a diagnostic `ctx.lr` printf in `sub_82164898` (harmless) |

### The ig_s class: w-lane junk of hkVector4 temporaries (NOT a defect)

The 4 divergent cases (`ig golden vel3`, `ig 02289D00 0`, `ig 022B0AC0 0`, `ig 022782C0 0`)
differ from the oracle only in page hashes; dumping the pages (`scratch/make_ig4.py`) shows
exactly ONE dword per case (three in vel3), every one at offset `+0xC` of a 16-byte-aligned
vector: `008AFE1C/2C/3C`, `0290E6AC`, `027CEE6C`, `0284C66C`. The oracle's write watch names
the writers:

- `sub_821BC6F8` @ `0x821BC828`: `lvx128 v0 <- var_A0` (a stack slot the function never
  writes), `vrlimi128 v0, v13, 8/4/2` (x, y, z inserted), `stvx v0 -> [r31+0x10]`. The w lane
  is uninitialized stack by the game's own construction.
- `CloneFixedObject_sub_82363B58` @ `0x82363C00..C20`: `ld/std` copies of three 16-byte
  slots of a STACK temporary (`r31 = 70EFFB90`, above the frame) whose w lanes hold whatever
  an earlier frame left there (`8297945C`, the script's r6 `70000B00`); last written by a
  16-byte `stvx` in `sub_82584870` @ `0x82584BCC`.

The localized build changes what stale stack and register contents look like (no
`__savegprlr` stores, fresh zero locals), so the junk differs; nothing reads it (every
downstream value is identical). The oracle agrees with the ctx-style build only because it
models persistent registers the same way. A/B rule for this harness: ig_s may differ in
these page hashes only; any D/DARR line difference is a real divergence.

### Live roll defect found and fixed: indirect calls into the XMV (VC-1) decoder

The first live roll of the localized build (`archive/v1_codegen/rolls/local1`, the owner's
whole-run scenario, SUCCESS, 0 crashes, 0 ICALL failures) decoded the intro movies to
macroblock garbage (`rolls/sheet_local1_frames.png` frames 002-008 vs the clean
`sheet_r5_frames.png` and `sheet_base1_movies.png` of the ctx-style guest on the SAME host
objects); every in-engine era rendered correctly. `[ABI_UNBRIDGED]` named 15 entered roots:
`Vc1CodecDecodeStep_sub_824B2BE0` (reads v64-v127), the motion-compensation predictors
`sub_828FEE30/FF068/FF260/FF478` (v64, v65) and `sub_82901AE0..82902E38` (v69, v72) - all
installed into function tables (`VideoCodecInitMcPredictorTable_sub_82561C80`,
`VideoDecoderCtxInitTables_sub_82504D68`, `Vc1SetupDecodeContextTables_sub_8252DC88`) and
reached by `bctrl`, plus `sub_827BAAD0` (r11 pass-through into Crt_Free, benign). A callee
reached through a table loads its live-ins from ctx at entry; a localized caller flushed its
registers only at DIRECT bridged calls, so through `bctrl` the decoder read stale constants.
Fix (`flag_transform.py`, "indirect" spec field): at every indirect call site of a localized
function, keep/flush/restore the VMX non-volatiles (v14-31, v64-127) the function writes.
Sized from the audit data: v64+ account for 2226 of the entry-read (fn, reg) pairs and v14-31
for 171; 190 + 18 indirect-calling functions write them; the reverse direction (a register
read after an indirect call) occurs in 1 function (r-vol) and is left as is. Lesson recorded:
a scenario SUCCESS does not look at movie pixels - the contact sheet does.

**Second finding (roll `fixed1`, `rolls/sheet_fixed1_movies.png`): the indirect-site flush was
NOT enough** - the movies were still garbage, including the flat-black first frame (so the
decoder's constants, not only motion compensation). The entered table helpers write NO v64+
(consumers only); the constants are produced by other functions, pass through orchestrators
that never touch them, and are consumed at entry through `bctrl`: a register FILE shared
across functions, which no caller-side flush of written registers can carry and which the
solver follows only along direct call edges. (`skip_lr` was checked as the alternative cause
and refuted: the XEX has no `bl $+4; mflr` idiom; the "mflr as data" hits are SEH frame
prologues.) Remedy, correct by construction: `[localize] keep_ctx_style_regs = ["v64-v127"]`
- every generated function that references v64-v127 stays ctx-style (471 incl. the
`__savevmx_64+` helpers; `keep_by_regs` in flag_transform.py), so ctx IS that register file
whatever the call type. The game's own SIMD (Havok, animation) lives in v0-v63 and stays
localized. Verified by: (see the status block for the roll of that build).

### Gates re-run on the fixed build (indirect-site bridges, 42 TUs rebuilt)

L6 `compare.py --all`: 54 pass / 0 fail. golden_call vs oracle: types 241/0, gold 6/0,
aabb 344/0, ray_s 344/0, ig_s the same 4 w-lane cases. All 48 suites from the main tree
(`archive/v1_codegen/suites_fix1/`): 46 exit codes equal to dev's exe; the two others are
timing tests that flipped under the concurrent `build-v3` compile load and were re-run
interleaved on both exes: `arena-wait-test` 3/3 pass on both, `timebase-test` B4 (a host 1 ms
ticker, wants >= 99% of samples within +-1 ms) failed 2 of 5 runs on dev's exe (98.87%,
98.58%) and 1 of 5 on the fixed exe (98.99%) - load, not codegen. `[ABI_UNBRIDGED]` across
all suite runs: 0. Replays (`tools/replay_ab.ps1`, menu_pressstart + fmv_eahd traces):
frames pixel-identical to dev's exe (same PM4 summary: 3501 / 151 draws).

### Gates on the v64-world build (keep_ctx_style_regs, 53 TUs rebuilt, snapshot `a8474a145ff9-codegen-fix2`)

L6 54/0; golden_call types 241/0, gold 6/0, aabb 344/0, ray_s 344/0, ig_s the 4 w-lane
cases; all 48 suites from the main tree equal to dev's exe (`suites_fix2/`), 0
`[ABI_UNBRIDGED]`; residual `ctx.<localized reg>` references: the one diagnostic `ctx.lr`
printf. Spec: 357 bridged, 284 unbridged roots, 8 indirect-flush functions; 316 generated
functions ctx-style by the register rule + 311 splice bodies + the cloth function.

## Status block (2026-10-03, iter-465)

- DONE (merged to dev): localized tree + solved bridges + the v64-v127 world ctx-style; all
  gates above; rolls base1/local1/fixed1/fixed2. Roll `fixed2` (the merged code, rebased host,
  snapshot `c43fe9f22523-codegen-fix2r`): SUCCESS, 0 crashes, 0 ICALL failures, movies decode
  cleanly (`rolls/sheet_fixed2_movies.png`), `[ABI_UNBRIDGED]` only the benign `sub_827BAAD0`
  (r11 pass-through into Crt_Free), gameplay 10.8 fps (p50 92.0 ms, p95 101.4) vs the
  identical-host base 9.4 (p50 105.9), movie #7 20.5 fps (r5: 20.4).
- Open: `sub_827BAAD0`/Crt_Free's r11 entry read (classify or bridge; harmless so far); the
  ig_s w-lane class is accepted noise of the harness (page hashes), not a defect.
- NEXT levers: ThinLTO / PGO on the guest lib, the per-access `aliasGuestVA` fold (platform),
  `-ffp-contract=on` as a faithfulness decision; `-march=x86-64-v3` rejected (above).

## Next levers (measured facts, not yet done)

- **DONE iter-466 tick (docs/PERFORMANCE.md 2.8): ISO volatile.** clang's MSVC-target default
  `-fms-volatile` lowered every guest `volatile` load/store to an acquire/release atomic - a
  compiler barrier at every access (every live `ctx` field reloaded/re-stored around it). The
  guest lib now builds with `-fno-ms-volatile` (CMake SR_GUEST_ISO_VOLATILE, ON) and
  aliasGuestVASlow is `pure`. L6 54/0, golden_call byte-identical; ISO-only rolls b1/b2 +3%
  ingame fps / -4% tick. Ctx-style bodies (splices, the v64-v127 set) gain the most from it.
- **Plumbing only: guest PGO** (CMake SR_GUEST_PGO=gen | <profdata>; a gen build rewrites the
  profile every SR_PGO_DUMP_SECS because rolls end by killing the game). Not trained yet.

- Guest TUs compile with `-O3 -DNDEBUG -std=gnu++20` and nothing else (`build/compile_commands.json`
  for `ppc_recomp.10.cpp`): no `-march` (SSE2 baseline, no BMI/AVX2 for the rlwinm/rotate and
  simde VMX code), no LTO, no PGO.
- **TRIED, REJECTED (2026-10-03): `-march=x86-64-v3 -ffp-contract=off`** guest lib
  (`build-v3/`, relinked exe `archive/exe_snapshots/codegen-v3-relink`, harness
  `archive/v1_codegen/gc_v3`). golden_call best-of-3 vs the same tree at the default flags:
  types -5%, gold +3%, aabb +12%, ray_s -5%, ig_s -4% (noise, no win), and it DIVERGES from
  the oracle where the default build does not: ray_s 4 cases, ig_s 2 cases beyond the w-lane
  class (`gc_v3/out/*.diff.txt`) - AVX2/FMA-era simde lowerings change some VMX results even
  with contraction off. Not worth a CPU-compatibility cut. (`-ffp-contract=on` remains a
  faithfulness question: the real PPC fmadd IS fused.)
- Every guest load/store goes through `aliasGuestVA` (`kernel/guest_context.hpp`: folds the
  0xA0/0xC0/0xE0 windows onto 0x80) before the `volatile` access; a memory-mapping alias of
  those windows (Xenia maps them as views of the same pages) would make it the identity.
- ThinLTO across the 133 guest TUs (cross-TU inlining of small leaves) and PGO
  (`-fprofile-generate` on the guest lib + a gameplay roll) are the remaining compiler levers;
  each needs the same gates (L6, golden_call, suites, replay, a roll with the movie sheet).
- XenonRecomp has no further codegen options beyond the eight flags (all applied).

## Measurements

golden_call harness, best-of-3 wall ms (`archive/v1_codegen/gc_ab_timing.json`, flags-off
`base` vs localized `local`, same host objects): types 1253 -> 1007 (-20%), gold 621 -> 148
(-76%), aabb 1635 -> 1200 (-27%), ray_s 1825 -> 1210 (-34%), ig_s 253 -> 238 (-6%).

Live game, the owner's whole-run scenario (`tools/player/roll_player.ps1`, PORT FEATURE roll,
1080p output 2x internal, the same gates as PLAYER_BUILD r5), identical-host A/B: `base1` =
dev's ctx-style guest lib relinked against the worktree's host objects (`relink_exe.py`),
`local1` = the localized guest (before the indirect-call fix; the fix touches only the codec
drivers). Displayed fps = median of the 2 s `[HIGHFPS_PACE]` windows (`era_fps.py`); p50 =
host present interval = the guest frame in gameplay (the guest is the limiter):

| Era (child process after RESTART NOW) | base1 (ctx-style) | local1 (localized) | r5 (dev-branch build) |
|---|---|---|---|
| Boot (logos + movies, 30 Hz content) | 30.0 | 29.6 | 31.3 |
| Title | 15.5 (p50 63.9 ms) | 18.2 (p50 54.2 ms) | 16.4 |
| Custom MAIN MENU + MODS | 14.5 (p50 68.6) | 19.5 (p50 49.5) | 13.8 |
| **Gameplay (tutorial, 117 windows)** | **9.4 (p50 105.9 ms, p95 113.7)** | **11.1 (p50 89.6 ms, p95 97.5)** | 9.5 (p50 104.6) |
| GPU busy / render thread, gameplay | 34.4 ms / 26.1 ms | 29.5 ms / 28.1 ms | 31 / 25 |

Localized guest: +18% gameplay fps (-15% guest frame time), +17% title, +34% menu (menu has
5 windows: noisy). One roll each (`archive/v1_codegen/rolls/{base1,local1}`); the sim stays at
30.0 ticks/s in both. The guest frame is still ~90 ms: register localization alone does not
reach 60 fps; the remaining levers are below.
