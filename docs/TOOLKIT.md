# Dev Toolkit & Pipelines

Operational reference for the recomp debugging toolkit (the **L-series** tools, built in iter-215)
and the **pipelines** that chain them. This is the "how do I actually run it" doc.

- For the *strategy/rationale* behind each tool: [`TOOLING_PLAN.md`](TOOLING_PLAN.md).
- For the *reference/ground-truth* tools (Xenia, XDK, RAG, the MCP servers): [`TOOLS.md`](TOOLS.md).
- Per-tool confidence + residual limits: the memory note `project_tooling_confidence`.

Everything below is **verified working** as of iter-215 unless marked otherwise. All script output is
ASCII-only (the consoles here are Windows cp1252 — non-ASCII in a `print()` is a crash, not a cosmetic).

---

## 0. Quick reference

Where every tool lives, and the one canonical entry point per job (roll `tools/sr_roll.ps1`,
press `tools/sr_press.py`, package smoke `tools/player/smoke_pkg.ps1`, sheets
`tools/frame_sheet.py`, worktree hygiene `tools/worktree_audit.py`, asset pipeline
`tools/assets/`): [`TOOLS_INDEX.md`](TOOLS_INDEX.md).

| Tool | What it does | Entry point | Confidence |
|---|---|---|---|
| **L2 smoke** | Regression gate: run the game, classify CRASH/HANG/LOW-PROGRESS/PASS | `scripts/smoke_test.py` | High |
| **L4 call tracer** | Env-gated runtime trace of indirect/vtable/computed calls | `SR_TRACE_CALLS=1` → `[CALLTRACE]` | High |
| **L5 refresh-ghidra** | 7-step pipeline: full fn coverage, xrefs, non-leaf decompile | `scripts/ghidra/*.java` | High (VMX128 scope) |
| **L6 difftest** | Differential CPU test: recomp vs Ghidra-emulator oracle | `scripts/difftest/` | High |
| **L7 bandaid registry** | Cross-check `BANDAIDS.md` ↔ code, CI gate | `scripts/bandaid_registry.py` | High |
| **L8 grind loop** | Autonomous crash-fix loop (pattern + agent tiers) behind L2 | `scripts/grind_loop.py` | High (pattern/endianness) |
| **L9-A Xenia oracle** | Instrumented Xenia → golden guest-execution trace | `tools/xenia-trace/` | Built + capturing |
| **L9-B recomp emitter** | Our side's trace, same format | `SR_GUEST_TRACE=1` → `sr_boot.trace` | High |
| **L9-C bisector** | First guest-execution divergence (golden vs ours) | `scripts/l9/divergence_bisect.py` | High |
| **Xenia harness** | Drive the working reference emulator headlessly: screenshot (vision) + virtual Xbox pad + Frida memory | `tools/xenia_shot.ps1`, `tools/xenia_pad.py` ([`XENIA_HARNESS.md`](../tools/XENIA_HARNESS.md)) | Working (iter-236+) |
| **Live-Xenia watchpoint oracle** | HW watchpoint on a guest VA in RUNNING Xenia — names who writes a predicate at the flip (§1.7) | `frida_attach target=xenia_canary` + `frida_watchpoint` | Working (iter-357) |
| **GPU trace replay** | Replay a golden Xenia .xtr GPU trace through our PM4→D3D12 — renderer work w/o the game wall (§1.6) | capture `tools/xenia_gpu_trace.ps1`; replay `SR_GPU_TRACE_REPLAY=<xtr>` | Built (iter-357) |
| **HLE command replay** | THE renderer loop for the API-HLE rebuild: replay a recorded command stream + its real plane pixels through the SAME `hle::execute()` the render thread runs — no game boot. ~110 ms vs ~200 s. Payload hashes re-checked (rc=2 = did not round-trip, pixel verdicts void). Recorder ARMS on movie activity, so a no-movie roll writes no trace instead of a misleading empty one. **iter-450h: traces carry a STATE PREAMBLE** — while recording is enabled-but-unarmed the recorder shadows the last TextureUpload/TextureUnlock payload per key + the last SetTexture per sampler, and writes them at trace open, so pre-window textures/bindings replay REAL instead of as white dummies (the iter-450f blind spot that manufactured the "replay paints / live doesn't" texture theory). Traces recorded before iter-450h still have the blind spot — treat their texture VALUES as artifacts | `tools/hle_replay.ps1 -Record` / `tools/hle_replay.ps1` (`SR_HLE_TRACE_REC` / `SR_HLE_TRACE_REPLAY`, FE-era window via `SR_HLE_TRACE_ARM_GEOM=<n>`) | Working (iter-442 S5; preamble iter-450h) |
| **Menu loop harness** | Default menu-blocker driver: /loop + K parallel runs (beat roll-dependence) + always-fan-out analysis agent-team | `tools/run_recomp_x.ps1`, `.claude/workflows/menu-analysis.js` ([`MENU_LOOP_HARNESS.md`](MENU_LOOP_HARNESS.md)) | Working (iter-245) |
| **Background runs** | Automated runs never take the foreground or read the user's input: game window shown without activation at the z-order bottom, harness keyboard channel for `sr_drive`, background Xenia launch + `PrintWindow` capture, foreground watch probe (§1.8a) | `kernel/host_automation.*`, `tools/bg_window.ps1` / `.py`, `sr_drive.ps1 [-ForegroundInput]` | Rolled (iter-464) |
| **Havok snapshot** | Havok world census (bodies/layers/shapes/MOPP CRC/filter/Superman's proxy) from a Xenia OR our full dump through ONE reader; set-diff two snapshots (§1.10) | `tools/havok_snapshot.py snap|diff` | Built + golden-validated (iter-463) |
| **objstate** | Objective / world-progress state (flows, DAS progress bits, chapter, XP, metro, powerups, save projection, OBJ\|TRIGGERS + ported availability verdict) from a Xenia OR our full dump OR a `.snap` of just the touched pages through ONE reader, plus the flow-FSM state, CurrentXP cap and trigger input event; diff two snapshots (§1.11) | `tools/objstate.py snap` / `diff`; `tools/guest_snap.py capture` | Built + golden-validated; dev-HUD calibration open (iter-463) |
| **Level-skip planner + runs** | Plan a skip to an objective / chapter along the story spine from an objstate snapshot (refuses what it cannot plan); run a plan on Xenia or ours in a tagged `devtool_*` dir; collect settle-point `.snap`s; compare ours vs the Xenia oracle (§1.12). DEV-TOOL-ASSISTED, never baseline evidence | `tools/devtools/skipplan.py plan|parse`; `tools/devtools/skip_run.py run|collect|compare`; executor `tools/devtools/xenia_skip.py` + `frida_skip.js` | Built, offline-tested (the executor differential-tested vs `skipexec.py` / objstate on 3 goldens and live run 1's snapshots); live run 2 (DEVTOOLS_DESIGN 6.8): the whole Warworld plan uninterrupted on Xenia, and 3/3 on our build with the kernel executor, story state EQUAL to the oracle at every settle point (`tools/devtools/xo_report_diff.py`) (iter-463) |
| **In-exe dev tools** | Never-default gate (`dev.txt` / `SR_DEVTOOLS`), boot tags + save taint, the KERNEL level-skip executor on the game's own tick, `[OBJSTATE]` dumps (§1.13). DEV-TOOL-ASSISTED, never baseline evidence | `SR_DEVTOOLS=levelskip SR_DEVTOOLS_CMDS=P.txt`; `tools/devtools/skip_run.py run --side ours --executor kernel`; `tools/devtools/kernel_skip_check.py` | Built, offline differential-tested; not yet run in gameplay (our build does not reach P0) (iter-463) |
| log-sqlite / va-to-source / ghidra / frida / windbg | MCP servers (query logs, VA→source, decompile/emulate, live mem, dumps) | MCP (deferred) | — |
| RAG / crash parser | Past-fix search; structured crash parse | `scripts/query_rag.py`, `scripts/parse_crash_log.py` | — |
| **C1 split receipts** | Preprocessed-token identity of a change (an `#if SR_ORACLE` split) per affected unit, base vs tree, with the build's own compile commands; exit 0 only when every affected unit is EQUAL, 1 on any DIFF (no exception class; integer-only differences are judged by review), 2 outside its stated scope (§1.8j) | `tools/pp_receipts.py` (on the cloud worker) | Used for release/oracle-mixed-a |
| **C1 removed-gate sweep** | The other readers of a gate an `#if SR_ORACLE` split removed: in the source (clang's -E of every unit the release exe links, default and release build; its resources, embeds, DLLs and prebuilt inputs listed UNCOVERED) and in the release exe (its C strings); survivors listed for review, exe names outside `--allow` fail | `tools/player/gate_sweep.py`, `tools/player/exe_sweep.py` (§1.8k) | High for names in literal values; a name code assembles is seen only by the exe scan |

---

## 1. Pipelines

The tools are designed to chain. The five pipelines below are the main workflows.

### 1.1 refresh-ghidra — bring Ghidra in line with the recompiler
**When:** after any XenonRecomp regen, or once per fresh Ghidra DB. **Full detail:**
[`scripts/ghidra/README.md`](../scripts/ghidra/README.md).

Prereq: Ghidra running with `GHIDRA_MCP_ALLOW_SCRIPTS=1`; the `ppc_savres_millicode` call-fixup present
in the install cspec (mirrored at `scripts/ghidra/cspec/ppc_64_32.cspec`); copy the scripts to
`~/ghidra_scripts`. Run each via `mcp__ghidra__run_ghidra_script` on program `default.xex`, in order:

1. `seed_functions.java` — create every `ppc_func_mapping` function (→ ~34,7xx)
2. `recover_disasm.java` — disassemble undefined-in-body bytes (~5.9M)
3. `run_constref.java` — const-ref analyzer, **dual-instruction refs ON** (lis/addi → xrefs)
4. `define_handler_table.java` + `scan_table_refs.java` — type the RTAM dispatch table
5. **(restart Ghidra)** then `apply_savres_redisasm.java` — save/restore millicode call-fixup so
   non-leaf functions decompile fully
6. `clean_overreach.java` — clear data-overreach bad-instruction noise
7. `seed_missing.java` — recover functions whose entry Ghidra mis-tagged as data (false strings)

Result: ~34,794 functions, scalar decompile + xrefs work. **Scope limit:** ~44K VMX128 vector
instructions stock Ghidra can't decode → vector-heavy fns decompile partially (use `va_to_source`).

### 1.2 smoke / regression gate (L2)
**When:** before trusting any code change; as the gate inside L8.

```bash
python scripts/smoke_test.py                 # run the game, PASS/CRASH/HANG/LOW-PROGRESS verdict
python scripts/smoke_test.py --self-test     # detection unit test (fixtures)
python scripts/smoke_test.py --check-log FILE # classify an existing log, no run
```
Crash markers are single-sourced in `scripts/crash_markers.py` (shared with `parse_crash_log.py`, so
they can't drift). Baseline floor in `scripts/smoke_baseline.json`. Note: a self-healing
`[WALKER_WATCHDOG]` is only a HANG if progress *also* stays below floor.

`python scripts/parse_crash_log.py FILE` also recognizes terminal `[ICALL_FAIL]`
markers in both the long boot-log and short stderr formats (iter-464). Its
`icall_fail` record retains `guest_call_site` and `indirect_call_target`; the
nonzero call site is an `investigation_targets` entry. Look up that instruction
VA directly: it is not a captured LR, and the parser does not invent one or
classify the indirect target as an AV fault address. `[ICALL_UNRESOLVED]` alone
is diagnostic, not a fatal receipt (the legacy forcing path can continue).
Fixture-only regression: `python scripts/test_parse_crash_log.py`.

### 1.3 differential CPU test (L6) — is a recompiled function correct?
**When:** you suspect a lowering bug (endianness, carry, shift, sign, a store). **Full detail:**
[`scripts/difftest/README.md`](../scripts/difftest/README.md) + [`DIFFTEST_DESIGN.md`](DIFFTEST_DESIGN.md).

The oracle is **Ghidra's PowerPC emulator** (an independent ISA impl) via the GhidraMCP HTTP endpoint —
so you need Ghidra running with the plugin (see §2.2). The recomp side is `difftest.exe`.

```bash
# 0. find pure-leaf candidates (and store leaves with --allow-mem)
python scripts/difftest/scan_candidates.py [--allow-mem]
# 1. build the recomp runner
cmake --build build --target difftest
# 2. generate the golden from the machine oracle (live)
python scripts/difftest/golden_capture.py --cases cases/sub_XXXX.cases.json --fn sub_XXXX --emulate
#    (--hand pins a hand-derived delta when no oracle; --emit/--ingest = offline MCP path)
# 3. diff recomp vs golden
python scripts/difftest/compare.py --all          # or --fn sub_XXXX
```
Gates registers (`r3,r4`, r3 always recorded) **and** memory (`deltas.mem`, byte-for-byte). difftest is
SEH-guarded: pointed at a non-pure function it returns `{"status":"faulted"}`, never a crash. A case
carries `regs`, optional `mem` (input seed), `watch` (output regions). Current set: 9 leaves + 2 stores
+ 1 negative, `compare.py --all` = 51/0, oracle fully machine-generated.

### 1.3b golden-call differential — whole call trees on golden memory (iter-463)
**When:** codegen is a suspect for a NON-leaf path (vtables, VMX128, allocators, game callbacks) and
you want recomp vs an independent PPC implementation on real game state. `scripts/difftest/golden_call/`.

- **Recomp side:** `golden_call_harness.exe` links the (pinned copy of the) guest lib + the difftest
  kernel objects, loads a golden-dump blob, fills the dispatch table and runs a script
  (`W va hex`, `CALL va rN= fN=`, `PR`, `D`, `DARR`, `LOG`, `TLS`, `CS`, `HEAP`, `END`). `END` prints every
  4 KiB page outside the scratch window `0x70000000..0x70FFFFFF` that changed (FNV hash) and restores
  memory (write-watch), so side effects are compared too. A harness collector (vtable slot 1 ->
  `0x8290F500`) records every `addCdPoint`.
- **Oracle side:** `xenon_interp.py` — a clean-room PPC64/VMX/VMX128 interpreter decoding the machine
  words in the blob (ISA for integer/branch/FPU, Xenia `ppc_emit_altivec.cc` + x64 sequences for VMX).
  NOT derived from `guest/ppc`. `run_script.py` executes the same script. Arithmetic modes:
  `xenia` (reference: fused vmaddfp, Xenia dot products, MulAdd-in-double fmadds, 32-bit CR0),
  `recomp` (XenonRecomp's conventions: unfused vmaddfp, simde's PORTABLE dp_ps — the lib has no
  -msse4.1 — maxps/minps, the pinned `roundevenf`), `isa` (exact single rounding, 64-bit CR0).
  **Bit-exact agreement in `recomp` mode proves every other lowering matches; the residue in `xenia`
  mode is then attributable to the named arithmetic conventions only.**
- `test_interp_vs_qemu.py` cross-checks the interpreter against QEMU (Unicorn PPC32) per instruction:
  63 integer/FPU forms, 0 mismatches (Unicorn's PPC64 target ignores MSR[FP] writes; its AltiVec
  faults inside the library, so VMX follows Xenia).
- Harness ABI facts: FP args consume GPR slots (`getAabb(this r3, xf r4, tol f1, out r6)`); import
  thunks KeTlsGet/SetValue use a harness TLS table on both sides; Rtl*CriticalSection succeed.
- `HEAP lo hi` (iter-467): until `END`, RtlAllocateHeap / XMemAllocPhysical (sub_8246CA80 /
  sub_82468008) take 16-aligned blocks from a bump arena `lo..hi` (put it in the scratch window)
  and RtlFreeHeap / XMemFreePhysical (sub_8246D368 / sub_824680A8) succeed without freeing, on both
  sides. The golden dump has no process heap (its LocalAlloc returns 0x8), so code that allocates
  (libjpeg, libpng, the HLSL back end) needs it.

```bash
python scripts/difftest/golden_call/build_harness.py        # -> archive/golden_call/golden_call_harness.exe
python scripts/difftest/golden_call/export_blob.py           # sound_gameplay image + low heap -> golden_mem.bin
python scripts/difftest/havok/gen_cases.py types archive/golden_call/types.script   # then: aabb, gold, mq, ray, lc, cp, ct, ig
python scripts/difftest/golden_call/compare_run.py archive/golden_call/types.script --mode recomp
python scripts/difftest/golden_call/compare_run.py <script> --mode xenia --recomp-out <prior .recomp.out>
python scripts/difftest/golden_call/classify.py  <recomp.out> <oracle.out>     # ULP vs STRUCT (hit-set) classes
python scripts/difftest/havok/classify_ig.py <recomp.out> <oracle.out> ig.script   # proxy integrate outcomes
python scripts/difftest/havok/coverage.py --mode xenia gold mq ray lc cp ig      # functions/branches/overrides hit
python scripts/difftest/golden_call/compare_run.py <script> --mode recomp --recomp-out <f> --mutate VA:hex  # mutation test
```
First result (Havok collision, H-B "the recomp computes building collision wrongly"): 21,876 cases,
~1.29 B guest instructions, recomp == oracle bit-exact in `recomp` mode; vs Xenia arithmetic only
float residue (max 0.49 mm on the proxy position) and 4/4624 near-tie cast reorderings — H-B refuted
for the query layer and `hkCharacterProxy::integrate`. The golden manifold contact reproduces
bit-exactly from the golden state.

### 1.4 autonomous crash-grind loop (L8)
**When:** a reproducible crash you want fixed mechanically. Structurally **cannot** touch the menu
/absence/race class — it escalates those (that's L9's job).

```bash
python scripts/grind_loop.py --dry-run                 # default: propose, don't apply
python scripts/grind_loop.py --go --no-commit          # full pipeline, stop before commit (inspect)
python scripts/grind_loop.py --go                      # apply→build→L2 gate→commit on green
python scripts/grind_loop.py --go --allow-agent        # add the claude -p fallback tier
```
Flow: `parse_crash_log` → `propose_fix` (RAG-replay → pattern tier `grind_patternfix` for the 6 CLAUDE.md
bug classes → agent tier `grind_agentfix`) → `git apply` → `cmake --build` → L2 smoke gate →
revert-on-fail (byte-exact) / commit-on-green. `grind_state` tracks signatures + oscillation. Logs which
tier solved each crash (mechanical-vs-investigative). **Limits:** crash-site resolution is recomp-only
(maps to generated `ppc_recomp.*.cpp`, not hand-written kernel stubs); run the first real `--go` supervised.

### 1.5 menu-divergence pipeline (L9) — A → C ← B
**When:** the menu/absence bug, where "what should happen" exists in a working Xenia boot but ours
diverges with no crash. **Full detail:** [`L9_DIFFERENTIAL_ORACLE.md`](L9_DIFFERENTIAL_ORACLE.md),
[`tools/xenia-trace/README.md`](../tools/xenia-trace/README.md), [`scripts/l9/trace_format.md`](../scripts/l9/trace_format.md).

```
A: instrumented Xenia  --(XENIA_GUEST_TRACE=1)-->  xenia_boot_rich.trace  (golden)
                                                            \
                                                             C: divergence_bisect  -->  first divergence VA
                                                            /
B: our recomp  --(SR_GUEST_TRACE=1)-->  sr_boot.trace      (ours)
```
- **A** — build the instrumented Xenia (see the xenia-trace README; needs the VS toolchain), run it
  against the **sound-stripped** ISO with `XENIA_GUEST_TRACE=1`. The JIT-prolog hook captures every
  guest function entry (2,223 distinct PCs; the trace is large, ~100 MB/s).
- **B** — run our recomp with `SR_GUEST_TRACE=1` (`SR_GUEST_TRACE_PATH` to override the path).
- **C** — diff them:
  ```bash
  python scripts/l9/divergence_bisect.py --golden xenia_boot_rich.trace --ours sr_boot.trace \
         --stream --resolve            # --stream: O(window) RAM, required for the multi-GB golden
  #   --strict for matched granularity; --window N tunes the subsequence look-ahead
  ```
  Reports the first VA where ours takes a path golden never does → feed it to Ghidra / the L4 tracer.
  Granularity-robust by default (ours is a subset of golden's entries); see the trace-format doc.

**Not yet built:** B's record-replay *scheduler* (deterministic boot) — the genuine multi-week piece.

### 1.6d SCORING TRAP - meanAbsDiff alone rewards rendering NOTHING [iter-414]

On a mostly-dark golden (the FMV fixture is a black frame with a white logo) a
near-black output scores BETTER than a bright wrong one, because most of the
target is black. Measured, iter-414, on the FMV fixture:

| config | meanAbsDiff | distinct colours | what it actually is |
|---|---|---|---|
| default | 59.25 | 2570 | structured garbage, but it HAS content |
| `SR_RESOLVE_NOWRITE=1` | **28.72** | 3 | a near-black frame - renders almost nothing |
| `SR_RESOLVE_TILED=1` | **28.72** | 3 | same |

The two 'better' numbers are the metric being gamed by blankness. ALWAYS report
distinct-colour count (or non-black coverage) next to meanAbsDiff, and ALWAYS
look at the image before believing a score. This is the same family as the
iter-408b non-zero-BYTES trap and the k_8 dumper: a measurement a blank surface
passes cannot show the surface is right.

Standing rule (TOOLKIT 1.6): never A/B two configs that are BOTH broken - here
the default is broken by garbage and the nowrite arm is broken by emptiness, so
neither is a baseline for the other.

### 1.6c THE FMV ORACLE - capture a golden for the scene you are ACTUALLY debugging [iter-412]

`tools/xenia_gpu_trace.ps1` takes `-WaitSeconds`, so it can capture ANY moment,
not just the title screen. Until iter-412 every renderer measurement of the movie
was compared against `menu_pressstart_golden.png` - a completely different scene -
which is why movie debugging felt like guessing. It was guessing.

```
\tools\xenia_gpu_trace.ps1 -WaitSeconds 18   # the EA HD logo FMV
\tools\xenia_gpu_trace.ps1 -WaitSeconds 45   # the legal/copyright screen
\tools\xenia_gpu_trace.ps1 -WaitSeconds 110  # press-start title (original golden)
\tools\replay_ab.ps1 -Tag fmv -Trace data\gpu_traces\fmv_eahd_454107ED_1124.xtr
```

The script saves BOTH a `.xtr` and `tools/xenia_trace_capture.png` (what Xenia
actually showed), so every capture is a trace plus its own pixel-exact target.

**Banked FMV oracle:** `data/gpu_traces/fmv_eahd_454107ED_1124.xtr` (2 MB, one
frame of the EA HD logo movie) + `fmv_eahd_golden.png`. 1955 packets, 151 draws,
55 vsLoads, 29 psLoads - two orders of magnitude smaller than the menu trace, so
it is also the fastest render fixture we have (~1 s).

**Baseline (iter-412): meanAbsDiff 59.25**, and our output is structured garbage -
vertical striping and blocky tiled noise, NOT the flat frame the live boot path
shows. Structure means the quad IS drawn and textured and the ADDRESSING is wrong,
a completely different bug from 'nothing renders'.

Gap it exposed immediately: `[TEX_CACHE] unsupported fmt=23 1280x720` -> fail() ->
WHITE. Format 23 is k_24_8_FLOAT (depth sampled as a texture); fmt 22 (k_24_8) is
missing from the switch too. (Fixed iter-413; pixel-neutral on both fixtures.)

**`unknownOps=1` is BENIGN - do not chase it.** The one unhandled opcode is 0x64 =
`PM4_XE_SWAP`, which xenos.h:1669 documents as *"Xenia only: VdSwap uses this to
trigger a swap"* - a synthetic packet Xenia injects into its own trace, not a real
Xenos command. Our replay drives the frame end from the trace's `kSwap` EVENT record
instead (`gfx/gpu_trace_replay.cpp:442` -> `onSwapEvent`), so the packet is redundant
by design. Expect `unknownOps=1` on every trace; only a HIGHER count is a finding.

**Fetch constant 0 is the SWAP TEXTURE slot, by convention** (iter-358j, matching
Xenia's `D3D12TextureCache::RequestSwapTexture`): VdSwap's type-0 packet writes the
front-buffer descriptor into fetch slot 0 immediately before the swap packet. So slot
0 holding something unrelated to a draw's own textures is EXPECTED, not corruption -
see `gfx/gpu_trace_replay.cpp:238-240`.

### 1.6b Replay is BIT-EXACT deterministic + how to read a cached shader [iter-411]

**Determinism proven (iter-411):** the same build + same gates produces a
byte-identical `scratch_replay_<tag>.png` (md5 verified across repeat runs). So a
pixel diff against `data/gpu_traces/menu_pressstart_golden.png` is real signal,
not noise, and any A/B that moves the image moved it for a reason.

```
# score a config against the golden (mean abs difference, lower = closer)
.\tools\replay_ab.ps1 -Tag <name> -Gates "SR_FOO=1"
# then diff scratch_replay_<name>.png vs data/gpu_traces/menu_pressstart_golden.png
```

⚠️ **An "off" switch must be verified by HASH against the pre-change binary.**
iter-411's first attempt had `SR_XLAT_SAMPLERS=0` writing sampler index 0, which
in the new table is POINT+wrap while the old default was LINEAR+wrap — so "off"
silently changed behaviour and every comparison against it was meaningless. The
fixed version reproduces the pre-change output bit-for-bit. If your off switch
does not hash-match, you do not have a baseline.

**Reading a cached shader (the translator is inspectable, not a black box):**
```
tools\XenosRecomp-clone\thirdparty\dxc-bin\bin\dxc.exe -dumpbin     data\shader_cache\dxil\<hash>.ps.dxil
```
DXIL is bitcode, so `strings` finds nothing — that is NOT evidence the shader is
empty. `-dumpbin` gives the signature, resource bindings and full disassembly.
Used in iter-411 to confirm the movie's YUV shader issues three `dx.op.sample`
calls plus the YUV->RGB matrix loads, i.e. the translation was never at fault.

**Shader ABI ground truth** (XenosRecomp `shader_recompiler.cpp:1275-1296`,
`shader_common.h`): `cbuffer SharedConstants : register(b2, space4)` laid out as
c0-c3 Texture2D descriptor indices, c4-c7 Texture3D, c8-c11 TextureCube,
**c12-c15 SAMPLER descriptor indices**, c16+ the shared constants
(`g_Booleans` c16.x, `g_SwappedTexcoords` c16.y, `g_HalfPixelOffset` c16.z,
`g_AlphaThreshold` c17.x, `g_NdcScaleOffset` c18). Texture heaps are unbounded
arrays at `t0 space0/1/2`, samplers at `s0 space3`.

### 1.6 GPU trace replay — renderer work without the game-progression wall
**When:** ANY PM4→D3D12 work (HARD RULE, CLAUDE.md): develop/regress the render path against a
golden Xenia GPU trace instead of waiting for the game to reach the scene. Idea imported from
Dolphin FifoCI via RecompCore (OTHER_RECOMPS_LESSONS §9, finding #7).

```
capture (once):  .\tools\xenia_gpu_trace.ps1              # boots ISO in instrumented Xenia,
                                                          # F4 single-frame .xtr at ~110s (title)
                 .\tools\xenia_gpu_trace.ps1 -PressStart  # + virtual pad @START first, to trace
                                                          # the MENU instead of the title screen
                 .\tools\xenia_gpu_trace.ps1 -Stream      # full-boot stream trace (big)
replay (each iteration):
                 .\tools\replay_ab.ps1 -Tag <name> -Gates "SR_FOO=1,SR_BAR=0"   # <- USE THIS
                 # ~3s: runs the replay, banks scratch_replay_<name>.png, prints the
                 # SUMMARY line and the pure-white %. -Gates is a comma string, NOT a
                 # hashtable (a hashtable is stringified across `powershell -File`).
raw form:        $env:SR_GPU_TRACE_REPLAY="<path>.xtr"; .\build\SupermanReturns.exe
                 # -> [GPU:REPLAY] frame/draw/reg logs + SUMMARY + replay_frame_N.bmp dumps
```

**This is the render iteration loop — ~3 seconds, deterministic, no game boot.** Use it in
preference to K-runs for anything touching PM4/D3D12: the natural deep-roll rate that reaches
the menu was measured at ~1/6 (iter-368), so boot-driven render iteration is a coin flip.
Diff the banked PNG against `data/gpu_traces/menu_pressstart_golden.png`.

⚠️ **Never A/B two configs that are both broken.** iter-368 recorded `SR_XLAT_RTT` as
"pixel-neutral" because RTT-on and RTT-off were byte-identical — both were white, for two
unrelated reasons. Fix one side, re-measure, then compare.

- Replayer: `gfx/gpu_trace_replay.cpp` — self-contained (own flat guest memory, no XEX, no guest
  threads); feeds the SAME `XenosRegisterFile`/`DrawCommandQueue`/D3D12 backend as the live path.
  Knobs: `SR_GPU_TRACE_REPLAY_HOLD=1` (keep window up), `_MAXFRAMES`, `_DUMP_EVERY`.
- The `[GPU:REPLAY] SUMMARY` line + per-opcode histogram is the iteration metric: UNHANDLED
  opcodes are the to-do list; frames/draws/regWrites regressions are failures.
- Capture prereq: the trace writer is compiled OUT of stock release Xenia; our instrumented build
  carries `tools/xenia-trace/xenia_gpu_trace_release.patch` (one line) to force it on. Trace format
  ground truth: `reference/xenia-canary/src/xenia/gpu/trace_protocol.h` + `trace_reader.cc`.
- Traces land in `reference/xenia-canary/build/bin/Windows/Release/scratch/gpu/<TITLEID>_*.xtr`.
- **Canonical golden (captured 2026-07-16, iter-357):** `data/gpu_traces/menu_pressstart_454107ED_9415.xtr`
  (71 MB, ONE frame of the PRESS-START title screen) + `menu_pressstart_golden.png` (what Xenia showed).
  First replay: CLEAN-EOF, 63,054 packets, 3,868 draws, 1.74M reg writes, 123 MB mem prefill,
  snappyFail=0. Unhandled-opcode to-do: 0x2B(IM_LOAD_IMMEDIATE) 0x2F 0x36(DRAW_INDX_2!) 0x3B 0x3C
  0x46 0x54 0x60 0x61 0x62 0x63 0x64 — note the live parser also mis-treats 0x27 (IM_LOAD, a shader
  upload) as a draw; fix during replay-driven renderer iterations.
- Capture caveat: send F4 via PostMessage to the Xenia HWND (the script does) — SendKeys/foreground
  tricks silently land in the wrong window from a background session.

### 1.6f iter-460 kit - live PM4 traces, per-draw bisects, the memory-window diagnostics

- **Live trace writer** (`gfx/pm4_live_trace.cpp`): `SR_PM4_TRACE_SWAP=<swap>` writes
  `live_454107ED_<swap>.xtr` (Xenia .xtr format) into `SR_PM4_TRACE_DIR` (default
  `archive/live_xtr`); `SR_PM4_TRACE_EVERY=<n>` adds up to 8 more traces n swaps apart. The
  trace holds the registers, gamma ramp, EDRAM, every GPU-written shared-memory page, every
  packet, and every byte the renderer/CP read (vertex, index, texture, ucode, resolve rects,
  WAIT_REG_MEM polls, LOAD_ALU_CONSTANT sources). Render it with
  `xenia-gpu-d3d12-trace-dump.exe` (Xenia draws what we consumed) and with our replay
  (`pm4_replay_score.py --xtr X --oracle Y --name Z`); scratchpad `mtr_eval.py <archive dir>`
  does both for every trace in a dir and makes a sheet (Xenia | live frame | our replay).
  Reading: our replay of a live trace matches the live frame; where Xenia's render differs,
  the data the CP fed the GPU is wrong (or the trace is missing a record - check
  `unmappedReads` in `xtr_draws.py --list`).
- **Trace hotkey (iter-462)**: `SR_PM4_TRACE_HOTKEY=1` - F9 in the game window (or the sr_drive
  key `"F9"`, no pad bit) records the frame after the next swap as `live_454107ED_<swap>.xtr`, up to 32
  per run, so a scene reached by playing can be traced (armed all run: the render thread stays off).
  Opening a trace re-validates every bound texture (`pm4TraceInit` clears the bind fast path) so the
  shared-memory path records all of them. Open issue: Xenia still renders our live gameplay traces
  dark while our replay of them is right, with `unmappedReads=0` (archive/v1_met2/eval_sheet.png).
- **Complete traces (iter-463, `SR_PM4_TRACE_FULL`, default ON; `=0` = renderer reads only)**: a
  traced frame also records, per draw and BEFORE the shader gates, the IB, every vertex fetch
  range and the full base + mip backing of every texture fetch constant (Xenia
  GetGuestTextureLayout), plus pages validated before the trace opened. Each page comes from the
  window validation serves it from (physwin owner: commandBufferWindow, then gpuWindowRange /
  gpuWindow). Pages resolved earlier in the frame are never recorded from the CPU copy; a
  GPU-written page is recorded only once the CPU rewrote it since the resolve. Check a trace with
  `python tools/xtr_draws.py TRACE --coverage`. It is TEMPORAL, as Xenia's TracePlayer plays a
  trace (a MemoryRead applies when reached, a draw runs at its PacketEnd): per kind it reports
  MISSING (no record anywhere) and LATE (supplied only after a draw that needs it - the old
  traces' render-time pre-pass records land in the swap packet, after every draw); either gives
  exit 5, exit 0 = COMPLETE. PS-only slots of non-kColorDepth draws do not count (Xenia runs them
  without a PS). Controls: the Xenia goldens menu_pressstart_9415 and fmv_eahd_1124 are
  COMPLETE. Every pre-iter-463 live trace fails it: the arena traces on missing-VS draws'
  IB/VB/textures, cube mips and late textures; the tutorial 19296 on late textures (156 draws,
  e.g. draws 26-38 and Superman's cube at 193-200) and cube mips.
- **Replaying a pre-iter-463 live trace (`SR_REPLAY_MIPS_SUPPLIED`, default ON)**: those traces
  carry no cube / stacked mip bytes, and `SR_PM4_ARRAY_MIPS` (default ON) now loads array mip
  chains. The replay therefore marks the pages the trace supplied, and an array whose mip region
  was never supplied (nor resolved) loads its base level only (`[TEX_ARRAY_MIPS] replay: ... not
  in the trace` in the log). `SR_REPLAY_MIPS_SUPPLIED=0` loads the zero mips anyway;
  `SR_PM4_ARRAY_MIPS=0` is the full pre-iter-463 array path.
- **Routes into the game**: `tools/scenarios/frontend_navigate_pm4_long.json` (300 s dwell: the street
  intro and its idle orbit camera, no input) and `tools/scenarios/gameplay_meteor_pm4.json` (the
  user-played route of p2_reach1: A dismisses the idle orbit, Y taps/holds launch, then the tutorial -
  NAVIGATE, the cold meteor, heat vision, superbreath - with F9 trace steps; `archive/v1_met2`).
- **Per-draw EDRAM bisect on a live trace**: `tools/edram_bisect.py --trace <live.xtr> --key
  <RtKey hex> --draws a,b,c --tag t --gates ""` (Xenia vs ours at each draw; the mean jumps name
  the draw). RtKey = base | pitchTiles32<<11 | msaa<<19 | depth<<21 | fmt<<22 (the scene
  target is 00C08000, its fmt12 alias renders to the same key; depth 006082D0).
- **Live per-batch RT0 dumps of ONE frame**: `SR_PM4_DUMP_FRAME=<frame epoch>` +
  `SR_PM4_DUMP_FROM/TO=<batch>` + `SR_PM4_DUMP_EVERY=<n>` (+ `SR_PM4_FRAMELOG=<epoch>` for the
  per-batch shader/texture lines). The epoch equals the present count in the dump names.
- **Index-buffer diagnostics** (`d3d12_pm4_edram.cpp`): `[IB_ZERO]` (all-zero index buffer)
  and `[IB_RANGE]` (indices past every vertex buffer the VS fetches) log the window decision
  (win / grant flags / page owner / command-buffer window) and whether the OTHER window's
  bytes would have been in range. Xenia's own traces: 0 hits. `SR_PM4_VSF_TRACE=<vs hash>`
  prints the raw indices (`[VSF_TRACE]`).
- **Driving past the title**: `tools/sr_drive.ps1 -Scenario tools/scenarios/frontend_navigate_pm4.json
  -Out archive/<dir> -Gates "..."` reaches the street (era witness = the guest's main_hud
  APT request; the HLE-era black-loading-screen gate never fires under PM4).
- **Frame census (perf)**: `[PM4_BIND_CENSUS]` every 100 frames (with `[PM4_FRAME]` and
  `[PM4_SHM_FRAME]`): bind ms by texture path (2D / 3D / array, with counts), the drain by
  prep / vsfFetch / vsfIb / decode, the resolve by store / shm / inval / clear / cpu;
  `[PM4_SHM_FRAME]` adds pages requested / lookupMs / hashMs and `[PM4_SWAP_TIME]` the
  per-present phases plus the CP's stalls on the render thread (`thirdWait` = waiting for
  an arena third's frame to complete on the GPU, `submitWait` = waiting for the render
  thread to take the previous frame; `[render thread]` marks the mode). The title CITY era
  (after `ScrPressStart`) is the one to read; the intro movies are ~55 batches a frame.
  RENDERER_PM4_HYBRID.md section 9 has the cuts these named and section 10 the render thread.
- **IDA worker pool (iter-462)**: the master database (GUI IDA, port 13337) answers one request
  at a time, so parallel agents queued on it. `tools/ida_pool.ps1 -Action start [-Count 8]`
  starts minimized GUI IDA workers (not idat: it spins a core each) on fresh copies of `ida/default.xex.i64` (`ida/pool/w<N>/`,
  ~290 MB RAM each) on ports 13341.. (`IDA_RPC_PORT`, a local patch of the plugin
  `%APPDATA%\Hex-Rays\IDA Pro\plugins\ida_rpc_bridge.py`, which also adds `save_database`;
  backup of the unpatched file in `archive/ida_map/`). Agents call a worker with
  `python tools/ida_rpc.py --port 1334N decompile_function 0x...` (any plugin method), or
  after a Claude Code reload through the `ida-w1..ida-w8` MCP servers in `.mcp.json`.
  Workers are READ-ONLY by convention (their copies are deleted on `-Action stop`): mapping
  waves (workflow `ida-map-wave`) write proposals + an independent verifier's verdicts to
  `archive/ida_map/<wave>/`, and `python tools/ida_apply_names.py <wave dir>` is the single
  writer to the master (verified, >= 0.65 confidence, never over an existing name);
  `--export data/ida_names.tsv` commits the whole name map and `--contracts data/ida_contracts.tsv`
  the verified one-line contracts (grep an address there before decompiling it). Renames in the master live in
  memory until it saves (File > Save, or `ida_rpc.py save_database` once the master runs the
  patched plugin).
- **Guest sampling profiler (perf)**: `SR_GUEST_PROFILE_MS=<interval>` (e.g. 4) samples every
  registered guest thread's lr and PPC back-chain (`kernel/guest_profile.cpp`) and writes
  self + inclusive histograms per 30 s INTERVAL to `archive/guest_profile/prof_<pid>_<n>.txt`
  (the histograms reset after each; `[GUEST_PROFILE] interval n` in the log maps n to the
  era) and the last partial interval at exit;
  `python tools/guest_profile.py <file> [--by-function] [--tid T]` symbolizes them with
  `guest/ppc/ppc_func_mapping.cpp` (IDA names for the top entries by hand). A sampled lr is
  the INNERMOST call site, so a `self` row names the caller of the running code. Use it when
  `[GPU_CP_TIME]` shows the CP idle (the guest is the limiter), not for renderer costs.
  Related gates (iter-461, all default OFF): `SR_STORE_WATCH=1` arms the PPC_STORE
  watchpoints (module globals, vtable area, the dynamic render-object / FMUI-queue / Apt-env
  / world-pointer hunts) - since iter-465 only in a build configured with
  `-DSR_STORE_WATCHPOINTS=ON` (they are compiled out of normal builds: the sub_826F92E0
  observer armed them in every game) - `SR_TRACE_CALLS=1` the indirect-call tracer, `SR_LOG_VERBOSE=1`
  restores the uncapped high-volume tags (PM4_RING_EXEC, DOGMA_GRANT rewinds, NAME_LOOKUP,
  FETCH0_WRITE, TRICOPY, CLIP_CTOR) - without it they keep their first 200 and a 1-in-64
  sample (the city wrote ~1,100 lines/s on the guest's own threads).

### 1.6g Runtime shader translation - knobs and the replay proofs [iter-463]

A VS/PS missing from `data/shader_cache/dxil` is translated in-process on its first IM_LOAD
(XenosRecomp linked in + `dxcompiler.dll`), persisted, and admitted; its draws are dropped only
until then. Design: `docs/RENDERER_PM4_HYBRID.md` section 11; toolchain + licences:
`tools/xenosrecomp/README.md`.

- **Gates.** `SR_SHADER_RUNTIME_XLAT=0` (off; default ON),
  `SR_SHADER_RUNTIME_XLAT_WAIT=1` (the CP thread waits for each job: deterministic replays),
  `SR_SHADER_RUNTIME_DIR=<dir>` (cache root; otherwise a per-process `%TEMP%` dir under
  `SR_INSTANCE_ID`, else `%LOCALAPPDATA%/SupermanRecomp/shader_cache`),
  `SR_SHADER_CONTAINER_DIRS=<dir|corpus.txt;...>` (containers for a replay, which has no
  CreateShader calls), `SR_DXC_DIR`.
- **Log.** `[SHADER_XLAT] on|off|DISABLED` once at cache load; `request`, `admitted <path>
  after N ms (translate M ms), K draws dropped meanwhile` + the provenance lines; `REFUSED` /
  `FAILED` / `WAITING` with the reason; `persisted entry skipped (<why>)`; `pruned N`.
- **Replay proofs** (the 16 arena VS; `<sc>` = a copy of `data/shader_cache/dxil` without the
  16 `archive/v1_arena/stage/missing_vs/manifest.txt` STAGED-OK hashes):
  ```powershell
  $t = "archive\v1_tape3\traces\live_454107ED_23799.xtr"; $c = "SR_SHADER_CONTAINER_DIRS=archive/v1_arena/stage/missing_vs/corpus.txt"
  .\tools\replay_ab.ps1 -Tag base -Trace $t -Gates "SR_SHADER_RUNTIME_XLAT=0"
  .\tools\replay_ab.ps1 -Tag A -Trace $t -Gates "SR_SHADER_RUNTIME_XLAT=1,SR_SHADER_RUNTIME_XLAT_WAIT=1,SR_SHADER_CACHE_DIR=<sc>,SR_SHADER_RUNTIME_DIR=<rt>,$c"   # == base
  .\tools\replay_ab.ps1 -Tag B -Trace $t -Gates "SR_SHADER_RUNTIME_XLAT=1,SR_GPU_TRACE_REPLAY_LOOPS=2,SR_SHADER_CACHE_DIR=<sc>,SR_SHADER_RUNTIME_DIR=<rt2>,$c"
  .\tools\replay_ab.ps1 -Tag C -Trace $t -Gates "SR_SHADER_RUNTIME_XLAT=1,SR_SHADER_CACHE_DIR=<sc>,SR_SHADER_RUNTIME_DIR=<rt>"   # 16 admitted at load, 0 requests, == base
  ```
  Between `SR_GPU_TRACE_REPLAY_LOOPS` passes the replay waits for the translator to go idle
  (`rtxlat::waitIdle`), so pass 2 deterministically sees every pass-1 admission. Compare pass 2
  with a full-cache `LOOPS=2` control, NOT with the single-pass base: passes are not steady
  state (the full-cache control's pass 2 differs from its pass 1 in 36% of pixels, pass 3 from
  pass 2 in 47%). `replay_ab.ps1` keeps only `replay_frame_1.bmp`; copy `replay_frame_2.bmp`
  yourself.
- **Unit + mutation proof** (no game process needed): `python tools/difftest/shader_xlat_test_run.py
  --work <scratch> [--reuse-offline]` (T1-T5 + mutants; the clean run uses the default, the
  OFF run `=0`).
- **Live check** (a VS no trace holds, e.g. the tutorial's 40078E73): the `sr_drive.ps1`
  tutorial scenario in RENDERER_PM4_HYBRID.md section 11 - `run_recomp_x.ps1` alone never
  leaves the title (nothing presses START). Grep the log for `[SHADER_XLAT] (admitted|WAITING|REFUSED|FAILED)`.
- **Exit.** The GPU replay calls `rtxlat::stop()` before returning from main: a job left
  running into `exit()` blocked it for the whole 30 s drain once (a replay that exits while
  translating: 32 s before the fix, 7 s after, exit code 0).

### 1.6e xtr_draws.py - the .xtr trace as a per-draw DATA oracle [TOOL-1 r1 CONFIRMED; TOOL-2A schema /3 + r1 CONFIRMED; TOOL-2B schema /4 + r1 CONFIRMED; TOOL-3A schema /5 CONFIRMED; TOOL-3B schema /6 CONFIRMED; TOOL-3C schema /7 + r1 UNVERIFIED]
**When:** a mechanism question about what the guest handed the GPU for a specific draw -
shader pair, ALU/fetch constants, render-target/depth/viewport registers, vertex bytes,
resolve targets - so OUR fixture can be diffed field by field against the console's own
state for the same shader pair. It renders nothing; the pixel oracle stays
`xenia-gpu-d3d12-trace-dump` (1.6). Sources: `tools/xtr_draws.py`, unit suite
`tools/test_xtr_draws.py`, mutation battery `tools/mutate_xtr_draws.py`.

```powershell
python tools/xtr_draws.py data/gpu_traces/menu_pressstart_454107ED_9415.xtr            # summary
python tools/xtr_draws.py TRACE --json archive/xtr_draws/title.json                     # + machine summary
python tools/xtr_draws.py TRACE --list [--frame N]                                       # one row per draw packet
python tools/xtr_draws.py TRACE --draw 1234 --consts [--crange 0:32] --verts 8 --textures   # static bindings
python tools/xtr_draws.py TRACE --shaders | --resolves | --vfetch-census
python tools/xtr_draws.py TRACE --coverage                                               # iter-463: exit 5 if a needed IB/VB/texture byte is absent or late
python tools/xtr_draws.py TRACE --draw N --dump-ucode PREFIX --dump-verts FILE
python tools/xtr_draws.py TRACE --frames 1 --no-shader-hash                              # hashes become null, never 0
python tools/test_xtr_draws.py            # verdict line: XTR_DRAWS TESTS: total= passed= failed= exceptions= skipped=
python tools/mutate_xtr_draws.py          # temp-copy battery; exit 0 iff every mutant is killed by an ASSERTION
```

**Exit codes:** 0 ok | 1 usage | 2 malformed trace | 3 missing dependency (`xxhash`, `cramjam`)
| 4 unsupported version/encoding/tag/event. Every refusal names the byte offset of the
record and the reason; `--json` still writes a summary with `status: refused` and the
same offset. EVERY supplied payload is decoded and length-checked through one
routine (`decode_payload`) before it is used or discarded: memory reads AND memory
writes against their declared decoded length (a validated write is then discarded -
it never becomes input memory), register snapshots against `count*4` and the
0x5003-entry file, EDRAM snapshots against Xenia's `kEdramSizeBytes` (10,485,760 bytes)
and gamma ramps against `(256 + 3*128) * 4` = 2,560 bytes (those two records carry no
decoded length, so the exact size IS the contract). Unknown encoding exits 4, missing
`cramjam` exits 3, and a raw-length mismatch, corrupt compression or decompressed-length
mismatch exits 2, always at the record offset. A read of bytes the trace never supplied
is counted in `unmapped_reads`/`unmapped_bytes` (byte-granular, not page-granular) and
warned by name. r1 (2026-09-12) closed the gap the TOOL-1 verdict found: before it,
memory-write, EDRAM and gamma payloads were skipped by encoded length without decoding.

**Semantics that differ from the r1 script (each pinned in the file header, Xenia
`0b4e6c440e91f118b42a5e9e32a6b629ce4c387c`):**
- **Resolve = a draw packet issued while `RB_MODECONTROL.edram_mode == kCopy (6)`**
  (`d3d12_command_processor.cc` L2515-L2517 -> `IssueCopy`). `EVENT_WRITE_EXT` (0x5A) is a
  screen-extent event, counted separately; `EVENT_WRITE_CFL` (0x59) and `CONTEXT_UPDATE`
  (0x5E) are not resolves either. The r1 rule (0x59/0x5E) reported ZERO resolves on
  traces that contain 26. Every draw packet is classified: `draws` (mode 4/5),
  `resolves` (6), `noop_draws` (0), `unknown_mode_draws`; `draw_packets` is their sum and
  equals Xenia's `TraceDump: draw packets=` count.
- **The resolve SOURCE is the selected register, not RT0** (`draw_util.cc`
  `GetResolveInfo` L1082-L1083, L1256-L1259): `copy_src_select` 0..3 reads
  `RB_COLOR_INFO::rt_register_indices[select]` = 0x2001 / 0x2003 / 0x2004 / 0x2005
  (0x2002 in between is `RB_DEPTH_INFO`); 4..7 reads `RB_DEPTH_INFO`. `--resolves` and
  the JSON `resolve_details[].source` give kind (`color`/`depth`), index, register, raw
  value, 12-bit tile base, format name and, for colour, the SIGNED 6-bit exponent bias;
  all four `RB_COLOR*_INFO` raws and `RB_DEPTH_INFO` are kept alongside. Title anchors:
  draw 3423 -> `COLOR1 RB_COLOR1_INFO=00000658 base=658`, draw 3404 -> `DEPTH(src4)
  RB_DEPTH_INFO=000102D0 base=2D0 fmt=kD24FS8(1)`. The `--shaders` view labels its
  render-target column `rt0=` because that is what it shows.
- **An unhashed shader is not an identity.** `distinct_shaders.pairs` counts only draws
  with BOTH hashes; `distinct_shaders.unidentified_draws` counts the rest (all of them
  under `--no-shader-hash`), and the `--shaders` view says how many were excluded.
- **TOOL-2A (schema /3): every trace-resident `GetResolveInfo` input is captured
  losslessly per resolve, nothing is derived.** Each `resolve_details[]` row carries
  `get_resolve_info_inputs`: `vertex_fetch_0` (registers 0x4800/0x4801 per
  `register_file.h` L59-L67; `xenos.h` L1158-L1172: type 2 bits, address 30 bits IN
  DWORDS, endian 2 bits, size 24 bits IN DWORDS - both raw dwords plus the decoded
  fields, address also in bytes); `vertex_payload` = the exact 24 bytes (six big-endian
  dwords) taken ONLY from decoded `MemoryRead` records that lie inside that draw's own
  PacketStart/PacketEnd interval - pinned Xenia reads them at `draw_util.cc` L959-L962
  and its writer records them as an UNCACHED `MemoryRead` (`trace_writer.cc` L142-L148)
  exactly there, so a reused address with new contents is captured per draw. **r1
  (2026-09-12):** the proof is packet-local. The parser keeps a per-packet coverage store
  (`PacketLocalReads`) separate from the trace-wide sparse image: reset at every
  PacketStart, fed only by `MemoryRead` records decoded while that packet is pending,
  discarded when an INDIRECT_BUFFER dispatch packet is consumed, and handed only to that
  PacketEnd's execution. Split and overlapping current reads are joined (a later record
  wins byte for byte); the trace-wide image, the packet body's own write and
  `MemoryWrite` records never satisfy the proof, so a reused address whose current draw
  lacks all or part of its own read is `incomplete` (exact `missing_bytes`), never a
  stale or mixed payload; `registers`
  = the raw values at that draw of `PA_SU_VTX_CNTL` 0x2302, `PA_SU_SC_MODE_CNTL` 0x2205,
  `PA_SC_WINDOW_OFFSET` 0x2080, window scissor TL/BR 0x2081/0x2082, screen scissor
  TL/BR 0x200E/0x200F (`draw_util.cc` L963-L1016; `GetScissor` L551-L724) and
  `RB_DEPTH_CLEAR`/`RB_COLOR_CLEAR`/`RB_COLOR_CLEAR_LO` 0x231D/0x231E/0x231F
  (L1301-L1303), numbers per `register_table.inc` L1245-L1250, L1303, L1336,
  L1363-L1365; `draw_packet_record_offset` = provenance (the draw's PacketStart
  record offset). **Status is fail closed:** `rejected` when the fetch is not
  `kVertex` or not 6 dwords (L954-L958: `GetResolveInfo` returns false and Xenia
  performs no copy; the `rejection` string names the bad field; no payload is read);
  `incomplete` when the fetch is valid but its draw's own packet interval did not supply
  all 24 bytes (`missing_bytes` = the exact count of target bytes no current read
  covered; `bytes_hex`/`dwords_be` null; if the bytes are absent from the trace-wide
  image too the unmapped read is counted and warned as before, if they exist only in
  the image a `resolve_payload_not_in_packet` warning names the row; the later join must
  refuse such a row); `complete` only when the current interval supplied all 24 bytes.
  Nothing is zero-filled and nothing is taken from an earlier draw. `not_in_trace` names the host parameters no
  `.xtr` record carries and that are therefore NEVER invented: `draw_resolution_scale_x`,
  `draw_resolution_scale_y`, the cvars `resolve_resolution_scale_fill_half_pixel_offset`
  and `half_pixel_offset` (read inside `GetResolveInfo`, L1230-L1233) and the
  `fixed_rg16/rgba16_truncated_to_minus_1_to_1` flags (`d3d12_render_target_cache.cc`
  L1307-L1312). The summary's `resolve_inputs` block conserves
  `complete + rejected + incomplete == resolves`. The capture itself derives nothing:
  the resolve rectangle, the adjusted destination base and the tiled extent are derived
  from exactly these captured inputs by TOOL-2B (next bullet), and raw destination base
  plus declared pitch/height remains an insufficient join key, because pinned Xenia
  derives the written range from these inputs (L1132-L1198; on the title trace the tiled
  extent differs from the declared linear span for 19 of the 26 rows).
- **TOOL-2B (schema /4): every resolve row carries `guest_destination_range`, the pinned
  `GetResolveInfo` range path (`draw_util.cc` L926-L1206) replayed bit for bit from the
  /3 inputs and the /2 copy registers - nothing else enters.** One
  configuration-independent guest destination range per complete row; the six
  `not_in_trace` host parameters stay unknown and are provably not inputs: the manifest's
  `guest_range_host_parameter_dependencies` is the empty list (`draw_util.cc` L1076-L1079
  only stores the scales; L1228-L1233 and L1272-L1281 feed fields this derivation does
  not produce). Stages, in arithmetic order, each a Python port of the pinned code (the
  top-level `derivation_manifest` carries the same list as full-SHA line-range
  permalinks, 49 links over 23 stages, each cited once; rows carry only
  `derivation_version` = `xtr_draws-resolve-range/1`): copy command kRaw/kConvert only
  (L939-L947, checked BEFORE the fetch); fetch validity (L953-L958, from /3); the 24
  payload bytes read little-endian and `GpuSwap`ped per the fetch endian - all four
  modes (`xenos.h` L1092-L1124); +0.5 when `PA_SU_VTX_CNTL.pix_center` == kD3DZero
  (L963-L968); `FloatToD3D11Fixed16p8` (`ui/graphics_util.cc` L17-L60: NaN and
  |x| < 1/512 -> 0, saturation at 32768-1/256 and -32768, hidden bit, two's complement,
  nearest-even, arithmetic shift); min/max over the three vertices then `(v+127)>>8`
  (L969-L991); signed 15-bit window offset when
  `PA_SU_SC_MODE_CNTL.vtx_window_offset_enable` (L993-L1001; `registers.h` L610-L620);
  `GetScissor<false>` - 14-bit window scissor, `window_offset_disable` bit 31, signed
  15-bit screen scissor, clamp to >= 0 and br >= tl (L1003-L1016, L636-L724); x0/y0 &= ~7
  and x1/y1 aligned up to 8 (L1024-L1027); MSAA > 4x rejected (L1029-L1038); surface
  pitch clamp on the 8-aligned `RB_SURFACE_INFO` pitch (L1040-L1051); height clamp to
  kMaxResolveSize 16376 (L1053-L1059; `xenos.h` L1027-L1029); empty rectangle returned
  separately (L1065-L1069); width/height div 8 (L1071-L1074); destination format = the
  raw `RB_COPY_DEST_INFO` format for colour, the `RB_DEPTH_INFO` format -> k_24_8 /
  k_24_8_FLOAT for depth (L1082-L1083, L1098-L1108; `texture_info.h` L170-L181); the
  64-row FormatInfo table (`texture_info_formats.inl`) with `is_depth || kResolvable`
  gating the tiled path; pitch/height aligned to 32 (`texture_address.h` L65-L67); base
  granularity `7 - min(2, bpp_log2 + is_3d)` / `is_3d ? 5 : 7 - min(2, bpp_log2)`
  (`xenos.h` L1211-L1218); `Tiled2D`/`Tiled3D` (the pinned function names) in int32/int64
  (`texture_address.h` L172-L238) for the adjusted base and the lower/upper bounds
  (`texture_util.h` L294-L312, `texture_util.cc` L450-L510, including the pin's
  `front & ~kTextureTileDepth` mask and the per-bpp upper-bound addends); a non-resolvable
  format produces the pinned ZERO extent (L1195-L1200); the L1201-L1206 invariants are
  evaluated and reported. C++ widths are emulated (uint32 addresses/extents, int32 2D,
  int64 3D); no linear `pitch*height*bpp` shortcut exists in the path. **Statuses**
  (`guest_destination_ranges` conserves their sum == `resolves`): `complete`;
  `rejected_copy_command` and `rejected_msaa` (named like Xenia, no range);
  `inputs_rejected` / `inputs_incomplete` (the /3 status propagated with NO arithmetic);
  `empty_rectangle` (width_div_8 == 0, no range); `non_resolvable_format` (zero extent,
  named, unusable). `join_usable` is true only for a `complete` row with a nonempty extent
  whose invariants hold. A complete row exposes the swapped vertex bits, the half-pixel
  offset, the 16.8 values, the rectangle after every stage, the scissor, MSAA, the
  aligned surface pitch, `width_div_8`/`height_div_8`, `destination` (is_depth, format,
  name, type, bpp, aligned pitch/height and their /32), `tiling` (2D/3D, bpp_log2, slice,
  granularities, base coordinates, offset/8; null when non-resolvable), `range` (raw
  base, adjusted base, extent start, length, exclusive end, invariants) and
  `raw_vs_derived` (adjusted == raw, start == raw, the differences, the declared linear
  span `pitch*height*bpp/8` and whether the tiled extent equals it). No host-dependent
  field (exponent, fill offset, scale) exists. **Differential oracle (T2B2):** an
  independent C++17 transcription of the same pinned lines (`archive/tool2b/oracle.cpp`,
  git-ignored; source SHA-256
  `55f67cff0c4d67838381bc679462cdfa9713776eb420e8d31be53c96cd624a37`; compiled with
  `"C:/Program Files/LLVM/bin/clang++.exe" -std=c++17 -O1 -fwrapv archive/tool2b/oracle.cpp -o archive/tool2b/oracle.exe`;
  the archived executable's identity is SHA-256
  `da78f9a01e9e1d67cc25ad9f384a2c0acb9912791dfeb4035a3e9e7fa928b6db` - PE timestamp
  metadata means a fresh rebuild may carry a different executable hash, so the
  reproducible evidence is the oracle source hash, the exact compiler command, the
  corpus and its status histogram, and the zero mismatches, not the executable hash)
  imports no Python; the 4,336-vector boundary corpus (`archive/tool2b/diff_corpus.py`:
  four endian modes x float classes, NaN/inf/subnormal/max patterns per slot, signed
  window offsets with enable/disable, scissors, pitch/MSAA, tall, empties, command x
  source x depth format, all 64 formats in 2D and 3D, geometry/slice/base including
  0xFFFFF000 and 0x80000000, bpp classes crossing tiles, 3,000 seeded randoms) agrees on
  every defined output field: 0 mismatches (status histogram complete 1,424 / rejected
  command 1,464 / rejected MSAA 399 / empty 736 / non-resolvable 313). 49 of those
  vectors are retained verbatim in the unit suite (`FIXED_ORACLE_VECTORS`) together with
  the oracle's view of the 36 trace-driven configurations. **Five traces (2026-09-12,
  this tree):** 26/26 rows `complete` and `join_usable` in each; the normalized 26-row
  derived list hashes `c0f9e7c1986e178f540f1b21bf54223356b345edc8fd2d9401971e30ef960eb3`
  in all five (T2B4: 0 differing rows); every title rectangle starts at (0,0), so the
  adjusted base and the extent start equal the raw base for 26/26 rows, while the tiled
  extent differs from the declared linear span for 19/26 (e.g. #3501 1280x720
  k_8_8_8_8: 3,768,320 B tiled vs 3,686,400 B linear; #3435 8x8 k_32_FLOAT: 4,096 vs
  512) - T2B3 is refuted on length, not on base, for this title. The remaining seven
  rows are the six 640x640 k_16 rows #33, #35, #102, #104, #877 and #879 (819,200 bytes
  each) and the one 64x64 k_32_FLOAT row #3431 (16,384 bytes). For these seven captured
  rows only, raw base plus declared pitch, height and effective bits per pixel happens
  to produce the same contiguous interval as TOOL-2B's conservative tiled extent. That
  is a producer-side numerical coincidence: it does not make the raw formula
  sufficient, it carries no consumer meaning, and a later join must use the derived
  `join_usable` range uniformly, without special-casing these rows. Producer-side
  numbers only; no consumer meaning is attached to any of them.
- **TOOL-3A (schema /5): every NORMAL draw carries `static_texture_bindings[]` - the
  texture-fetch slots its VS and PS ucode statically name, with the raw and fully decoded
  fetch constant of every referenced slot. These are STATIC BINDING OPPORTUNITIES, not
  executed samples.** Normal = kColorDepth/kDepth mode (the 26 kCopy resolves are not
  texture consumers in this census; noop/unknown modes are listed as excluded with their
  counts). The walk is the pinned one and is SHARED with the vertex-fetch census
  (`cf_exec_walk`): `Shader::AnalyzeUcode` (`shader_translator.cc` L53-L64, bound shrinks
  while scanning), `GatherExecInformation` (L349-L372: every exec family kExec, kExecEnd,
  kCondExec, kCondExecEnd, kCondExecPred, kCondExecPredEnd, kCondExecPredClean,
  kCondExecPredCleanEnd names `count` slots from `address`; sequence bit 0 = fetch, bit 1 =
  serialize; a fetch slot whose opcode is not kVertexFetch goes to the texture path),
  `GatherTextureFetchInformation` (L435-L470: setTexLOD / setGradientH / setGradientV
  "don't use bindings" and are recorded as non-binding; tfetch, getBCF, getCompTexLOD,
  getGradients and getWeights bind their fetch constant; the slot mask is deduplicated only
  at the end, so both stages' references to one slot and repeated references inside one
  stage are all kept with opcode, cf index, slot and stage provenance). An unknown fetch
  opcode hits `assert_unhandled_case` in `ParseTextureFetchInstruction` (L1090-L1092) and
  makes the stage `incomplete`; so do a malformed exec (slot beyond the ucode), a program
  with no exec, an empty program and partly unmapped ucode; a stage with no shader loaded
  is `missing`. The tool NEVER falls back to "every type-2 fetch constant". Instruction
  fields come from `ucode.h` `TextureFetchInstruction` (L835-L879; signed 7-bit LOD bias,
  signed 5-bit offsets, `FetchOpDimension` k1D/k2D/k3DOrStacked/kCube). **The
  fetch-constant decoder is repaired** from `xenos.h` `xe_gpu_texture_fetch_t` (L1229-L1330):
  dword 0 type/sign x-w/clamp x-z/pitch (pixels >> 5)/tiled; dword 1 format, endianness,
  request_size, stacked, nearest_clamp_policy, base_address (>> 12); dword 2 the size union
  (1D width:24 | 2D-or-stacked width:13, height:13, stack_depth:6 | 3D width:11, height:11,
  depth:10 - the row reports all three views plus the dimension-selected size, cube depth
  6); dword 3 num_format, swizzle (bit 1), SIGNED exp_adjust (6), mag/min/mip filters,
  aniso_filter, arbitrary_filter, border_size; dword 4 vol_mag/vol_min filters,
  mip_min/max_level, aniso walks, SIGNED lod_bias (10), SIGNED grad_exp_adjust_h/v (5);
  dword 5 border_color, force_bc_w_to_max, tri_clamp, SIGNED aniso_bias (4), dimension,
  packed_mips, mip_address (>> 12). The r1-era decoder read swizzle and the filters from
  dword 4 and the mip bounds from dword 5; those fields were wrong in every earlier
  `--textures` print (TOOL-2B did not consume the decoder). **Row:** draw ordinal, frame,
  mode, VS/PS hash, per-stage {status, reason, ucode hash, referenced instructions (cf
  index, cf opcode, slot, opcode, name, fetch constant, dimension, binding flag, writes
  flag, predicated), binding slots, mask, instruction counts}, `analysis_status`
  (complete only when both stages are), `union_slots`, `overlap_slots`, `bindings[]` (slot,
  stages, per-stage instruction counts, fetch-constant type and name, `is_texture_constant`,
  raw six dwords, the complete decode), `type2_slots`, `unused_type2_slots`,
  `referenced_non_texture_slots`. `static_texture_shader_analyses` (hash -> the full
  analysis with every instruction field) is written once per hashed shader. **Census**
  (`static_texture_binding_census`, every denominator explicit): normal draws and the
  excluded kinds; per-stage complete/incomplete/missing and draws with both complete;
  referenced instructions per stage (binding / non-binding / unknown, by opcode name, over
  all normal draws); referenced slots per stage, union and overlap and by fetch-constant
  type (over draws with both analyses complete); type-2 slots, unused type-2 slots and
  draws with any; draws referencing a non-texture constant; draws with / without a static
  binding / not complete, conserving to the total; T3A1 counts; distinct shaders analysed.
  **Differential oracle (T3A2 / T3A3):** `archive/tool3a/oracle.cpp` (git-ignored) is
  compiled AGAINST THE PINNED HEADERS - it includes `xenia/gpu/ucode.h` and
  `xenia/gpu/xenos.h` from the vendored checkout and decodes with Xenia's own bitfield
  structs and accessors (`UnpackControlFlowInstructions`, `IsControlFlowOpcodeExec`,
  `TextureFetchInstruction`, `xe_gpu_texture_fetch_t`), transcribing only the walk logic
  of L53-L64 / L349-L372 / L435-L470 / L1058-L1092; it imports nothing from Python. Build:
  `"C:/Program Files/LLVM/bin/clang++.exe" -std=c++17 -O1 -D_CRT_SECURE_NO_WARNINGS -I reference/xenia-canary/src -I reference/xenia-canary/third_party/fmt/include archive/tool3a/oracle.cpp -o archive/tool3a/oracle.exe`
  (clang 22.1.6); source SHA-256
  `8cfb227f1997668e558badccd2d04166e2b3388927fb3b6c1d278f82481f5e27`; the archived
  executable's identity is SHA-256
  `60ae9c5a8a7e13aa901d7fce761d6831ba0f367bdd9f4b5926793d73a883c1de` (PE timestamps make
  a rebuild's hash vary; the source hash, command and zero-mismatch corpus are the
  reproducible proof). `archive/tool3a/diff_corpus.py` compared, line for line and field
  for field: all 112 distinct VS/PS ucodes bound at any draw of the five traces (55 VS, 57
  PS, all complete), 748 synthetic programs (every exec family, every non-exec CF opcode,
  seven sequence patterns, all 32 fetch opcodes, binding opcodes x four dimensions x dst
  swizzles, duplicates, vertex-only, setters-only, shrinking and zero bounds, out-of-range
  execs, empty and truncated programs, 600 seeded randoms; 158 complete / 590 incomplete)
  and 3,255 fetch-constant vectors x 48 fields (every one of the 192 bits in isolation,
  all-ones, zero, six boundary values of each of the five signed fields, seven size
  patterns x four dimensions, three realistic constants, 3,000 seeded randoms): 0
  mismatches in all three. The suite retains the oracle's lines for 80 programs and the
  oracle's fields for 66 full vectors plus the 192-bit isolation table. **Five traces
  (2026-09-12, this tree):** every normal draw has both analyses complete (title
  3,475/3,475; FMV 125/125 each); the VS stage references no texture anywhere; the title
  PS stage names 22,636 binding instructions (tfetch2D 11,122 / tfetch3D 9,709 /
  tfetchCube 1,805) and 408 setTexLOD non-binding ones over 14,577 referenced slots (all
  type 2, no overlap); 3,124 draws have a static binding, 351 none, 0 not complete
  (conserved). **T3A1 REFUTED:** on the title 0 of 3,475 complete draws have a type-2 set
  equal to the referenced set - 38,120 type-2 slots are present but 23,543 of them are
  referenced by neither shader, every draw has at least one unused type-2 slot, and no
  draw references a non-type-2 slot; FMV 4/125 (t16, eahd) and 0/125 (t21, t25). Printing
  type-2 constants was never consumer evidence. **T3A2 / T3A3 NOT KILLED** within the
  same-pin boundary above. **T3A4 REFUTED as a five-way equality:** the normalized rows
  (draw ordinal and frame removed) hash to
  `9a65644a84081c7abfb1c1abdae9e2254fb3b2d45eca8a2e670148798648ffdd` (title, 3,475 rows),
  `0cb788c6608fb67376a906edb510c961e93597a9a191bb1373c750f08d11dc03` (t16 and eahd) and
  `95b48b30099a9e886c704a4f0d89c36ecce5b51a1883d430ac58aeb1a86c008c` (t21 and t25); t16
  and t21 differ in all 125 rows but ONLY in `type2_slots` / `unused_type2_slots` (t21/t25
  carry an extra type-2 constant in slot 15 that no shader references); with those two
  fields removed all four FMV row lists are identical, and the 25 shader analyses the FMV
  traces share with the title are identical to the title's (2 PS are FMV-only). The
  post-translation mask Xenia requests at draw time
  (`dxbc_shader_translator.cc` PostTranslation, formed from the translator's bindings) is
  NOT reproduced here; this is the analysis-stage static set, labelled as such. No
  consumer memory range, producer match, ownership, contents or pixel claim is made.
- **TOOL-3B (schema /6, gated two-phase): Xenia's ACTUAL post-translation request mask
  first, then the exact requested consumer backing ranges, with the /5 bulk normalized
  losslessly.** *Phase 1 (T3B1 gate).* The tool cannot run `DxbcShaderTranslator`; it
  CONSUMES a table recorded by running the real one. `archive/tool3b/xenia_oracle.cc`
  (git-ignored; source SHA-256
  `25504dcf9b3493ecd0739cd4a0b47fd758e6952f5ea108bd093c829e98e53dc7`) is a host program
  compiled with the pinned checkout's own MSVC toolchain, defines and include paths
  (`archive/tool3b/build_oracle.cmd`) and LINKED AGAINST THE PINNED CHECKOUT'S OWN RELEASE
  OBJECTS (`build/obj/Windows/Release/xenia-gpu.lib`, `xenia-base.lib`, `xenia-ui.lib`,
  `xenia-core.lib`, `dxbc.lib`, `fmt.lib`, ...; `ninja -n xenia-gpu` = "no work to do", so
  the library is current with the pinned sources; the linker map lists the pinned
  `dxbc_shader_translator*.cc`, `shader*.cc`, `texture_cache.cc`, `texture_util.cc`,
  `texture_info_formats.cc` objects and NONE of the checkout's 12 locally modified
  translation units, which are all outside the translator/layout/base paths - `git diff
  --quiet` on those paths is clean). It constructs `DxbcShader(type, 0, ucode, count,
  big)`, `AnalyzeUcode`, `DxbcShaderTranslator(GpuVendorID(0), bindless=false,
  edram_rov=false)` with the default modification (as `xenia-gpu-shader-compiler`;
  `dxbc_shader.h` L52-L58: bindings do not depend on modification bits),
  `GetOrCreateTranslation` + `TranslateAnalyzedShader`, and prints translation status,
  `GetUsedTextureMaskAfterTranslation()` and `GetTextureBindingsAfterTranslation()` per
  shader; three `xe::cpu::MMIOHandler` members that xenia-core's `memory.cc` references
  are stubbed with `std::abort()` (unreachable on the static paths). The archived
  executable's identity is SHA-256
  `bd635246f93025a6e877ae2821a8fb82e27a0ff0f0fde332705fbc74cf9a68c7` (PE timestamps make a
  rebuild's hash vary; the source hash, build script, linked-library hashes and the
  zero-delta corpus are the reproducible proof). `archive/tool3b/mask_oracle.py` ran it on
  all 112 distinct stage/ucode pairs bound at any draw of the five traces (55 VS, 57 PS),
  bound the evidence to the pin, the checkout's local-modification list, the SHA-256 and
  git blob of the pinned sources, the linked libraries and the oracle, and wrote
  `archive/tool3b/masks.json`. **Provenance correction (Sol, TOOL-3B verdict; repaired at the
  start of TOOL-3C):** the accepted sidecar (SHA-256
  `aa36783a232d97779aea26fce5a2edcde4a4338a9f3bada8b88fa848fcce38e5`) listed 23 source
  paths, not 24 - `gpu_flags.cc`, whose object supplies the pinned cvar default the layout
  oracle uses, was in the linker map but not in the identity list - and hashed nine
  libraries although `build_oracle.cmd` links a tenth, `xenia-core.lib` (its pinned
  `memory.cc` object is in the map). The regenerated sidecar (SHA-256
  `a52ebbafb4c26500962219e58352516eda4275e1f9834ad612337b4b24a60bad`) records 24 source
  identities (`gpu_flags.cc` blob `88b55444d9e857b873483633be54207300983f22`), ten library
  hashes (`xenia-core.lib` SHA-256 `2ef6fe8108099c73...`) and the `ninja -n` dry runs of
  `xenia-gpu`, `xenia-core`, `xenia-base` and `xenia-ui` (all "no work to do"); its 112
  shader rows and its summary block are IDENTICAL to the accepted sidecar's (only the four
  identity fields changed), so no mask, status or trace result moved. Under the accepted
  run: 112/112 translate
  valid, the post-translation mask EQUALS the TOOL-3A analysis mask for 112/112 and Xenia's
  own analysis-stage mask for 112/112 (delta 0; 196 post-binding slots, 430 binding entries
  of which 215 signed - the translator splits one slot by signedness). **T3B1 NOT KILLED;
  the gate passed and phase 2 was entered.** The tool takes the table through
  `--post-masks PATH`, refuses a table whose `identity.xenia_pin` is not the tool pin
  (every stage `not_in_table`), indexes by (stage, exact ucode SHA-256) and marks every
  stage `translated` / `translation_invalid` / `analysis_mismatch` (Xenia's analysis mask
  differs from this tool's) / `not_in_table` / `no_table` / `ucode_unmapped` / `missing`,
  fail-closed: a draw is post-complete only when BOTH stages are `translated`. Slots in the
  union of the two post masks are **requested binding opportunities** - what
  `d3d12_command_processor.cc` L2664-L2669 requests from the texture cache - still static:
  no predicate, branch, coordinate or sample is proven executed. *Normalization (T3B2).*
  `static_texture_shaders` (one entry per exact ucode content, keyed by its SHA-256 with
  `-unmapped` appended for truncated programs, carrying the trace's XXH3-64 identity, the
  stages it was bound to, the full analysis and the per-stage post-translation status /
  mask / bindings once) and `texture_fetch_constants` (one entry per exact 24-byte raw
  constant, keyed by its 48 hex digits - never a short hash - with the raw dwords, the
  complete decode, `requested` and the backing) replace the per-draw repetition; a row
  keeps draw ordinal, frame, mode, VS/PS hashes, per-stage {status, reason, ucode hash,
  analysis key, mask}, union/overlap, the type-2 sets, `bindings[]` {slot, stages,
  constant key, is_texture_constant, requested_stages}, `post_translation` {per-stage
  status + mask, status, requested_union, requested_overlap,
  requested_equals_analysis_union, requested_minus_analysis, analysis_minus_requested} and
  `requested_binding_opportunities[]` {slot, stages, constant key, backing_status,
  join_usable}. `expand_static_bindings_v5` / `--expand-v5 FILE` rebuilds the accepted /5
  projection by COPYING from the two tables (binding slots from the mask, per-stage
  provenance counts from binding instructions, decode and raw from the constant table);
  `archive/tool3b/check_expand_v5.py` compared it field for field with the accepted /5
  outputs of all five traces: 0 differing paths in `static_texture_shader_analyses`,
  `static_texture_bindings` and `static_texture_binding_census` (title 110 analyses, 3,475
  rows, 14,577 bindings, 23,044 instruction records; FMV 27 / 125 / 31 / 158 each), for
  the `--expand-v5` file and for an in-process expansion of the /6 file. **T3B2 NOT
  KILLED.** Sizes: title 42,585,152 -> 18,999,823 bytes (44.6%, a 55% reduction; the
  reconstructed /5 file is 42,453,839), FMV 521,713 -> 705,241 (t16; t21/t25 523,949 ->
  707,477; eahd 521,698 -> 705,226 - the FMV files GROW because the per-constant layout
  records, 14 level records + 14 offsets each, outweigh 31 binding occurrences); distinct
  definitions title 110 shaders / 833 constants (14,577 occurrences), FMV 27 / 27 (31).
  The 75% estimate was not reached: the 3,475 rows (post block, bindings, opportunities,
  type-2 lists at `indent=1`) still hold ~12 MB, the constant table ~6 MB. *Phase 2 -
  backing (T3B3).* Only for constants a post-mask requests, `derive_texture_backing`
  ports, in uint32, the pinned request-to-backing path: `TextureCache::
  BindingInfoFromFetchConstant` (`texture_cache.cc` L871-L965: kTexture accepted;
  kInvalidTexture accepted ONLY because the pinned DEFAULT of
  `gpu_allow_invalid_fetch_constants` is true (`gpu_flags.cc` L29-L34; the row records
  `allowed_by_cvar` so the other setting is a filter, not a recomputation); kInvalidVertex /
  kVertex `invalid_type`; base and mip pages both 0 `no_data`; a 1D texture >= 8192 wide,
  tiled or with packed mips `invalid_1d`, every defect named; `GetBaseFormat`
  (`texture_info.h` L27-L60) remaps the eleven resampling/gamma formats on the key, the raw
  format is kept), `texture_util::GetSubresourcesFromFetchConstant` (L23-L114: the size
  view per dimension - stacked depth, 3D depth in the longest axis, cube 6; 17-bit
  physical 4 KB pages `& 0x1FFFF`; mip page 0 forces 0..0, min/max clamped to
  `log2_floor(longest)`, max raised to min, base 0 raises min to 1, a range starting
  above 0 DROPS the base page, max 0 DROPS the mip page), `FormatInfo` block geometry
  (`texture_info_formats.inl`, 64 rows: 2x1 REP formats, 4x4 DXT/DXN/CTX1/DXT3A/DXT5A,
  4x1 `k_32_AS_8` and `k_32_AS_8_INTERLACED`, 2x1 `k_32_AS_8_8`; `k_1` / `k_1_REVERSE`
  have 0 bytes per block and are `unsupported_format` because the pin's `log2_floor(0)` is
  undefined), `GetPackedMipLevel` (`texture_util.h` L41-L44), `GetPackedMipOffset`
  (L116-L206) and `GetGuestTextureLayout` (L208-L448: base pitch from the fetch pitch,
  mip pitch/rows from `next_pow2 >> level`, blocks tile-aligned to 32, linear mip rows
  aligned to 256 B, 3D slices aligned to 4 depth, array slice stride aligned to 4 KB, cube
  array 6 / stacked array = depth, packed tail extents as the max over the tail levels,
  tiled extents from the same `GetTiledAddressUpperBound2D/3D` TOOL-2B uses (3D truncated
  to uint32), linear extent `z_stride*(z-1) + row_pitch*(y-1) + bpb*x`, mip offsets
  accumulating `stride*array_size`, `mips_total_extent_bytes` as the max end; fields the
  pin never writes - `mip_offsets_bytes[0]` unless the packed tail is level 0 with mips,
  `mips[0]` when the packed tail is level 0 without mips - are `null` and named in
  `undefined_in_pin`). The result is the TextureKey, the normalized subresources, the
  layout and TWO SEPARATE guest backing intervals exactly as `Texture` watches them
  (`texture_cache.h` L247-L252): base `[base_page << 12, + base.level_data_extent_bytes)`
  and mips `[mip_page << 12, + mips_total_extent_bytes)`; `join_usable` only for
  `complete` with a non-empty interval; every other status carries no interval. These are
  the bytes pinned Xenia would keep resident for the texture, NOT bytes proven sampled.
  **Differential oracle:** the same `xenia_oracle.exe layout` mode CALLS the pinned
  `GetSubresourcesFromFetchConstant`, `BindingInfoFromFetchConstant` and
  `TextureKey::GetGuestLayout()` (through a `TextureCache` subclass, since `TextureKey` is
  protected) on each stdin constant; `archive/tool3b/layout_corpus.py` compared status /
  key validity, the 7 subresource fields, the 12 key fields, array size, max level,
  packed level, mips total, base/mips sizes, all 14 level records x 8 fields and the 14
  offsets for 116,075 constants: every distinct non-zero constant in any slot of any
  normal draw of the five traces (2,580: 898 complete, 1,680 kVertex/kInvalidVertex, 2
  no-data) plus 113,495 synthetic vectors (all four dimensions, linear/tiled, stacked
  depths 1/6/64, packed/unpacked, eight mip configurations incl. separate mip bases and
  base page 0, 15 sizes incl. 1x1, 17x17, 8192x8192 and 7x3000, natural and +1 pitches,
  15 formats, all 64 formats at four geometries, the type / no-data / 17-bit page / pitch
  0-1-2-511 edges, 4,000 seeded randoms): 108,839 layouts compared, 0 mismatches
  (statuses: 108,839 complete, 3,191 no_data, 2,250 invalid_1d, 1,684 invalid_type, 111
  unsupported_format; 102,369 `mip_offsets[0]` and 3,531 `mips[0]` skipped as
  pin-undefined and cross-checked against `undefined_in_pin`). The first run found THREE
  wrong rows in a hand-typed block table (`k_32_AS_8`, `k_32_AS_8_8`,
  `k_32_AS_8_INTERLACED` are 4x1 / 2x1 / 4x1 blocks) - 7,145 mismatches - fixed from the
  pinned `.inl`; the suite retains 140 oracle vectors. **T3B3 NOT KILLED** within the
  same-pin boundary. **Five traces (2026-09-12, this tree, `--post-masks
  archive/tool3b/masks.json`):** every normal draw is post-complete (title 3,475/3,475,
  FMV 125/125; 54 VS + 56 PS distinct title shaders translated); the requested union
  equals the analysis union on every draw (title 14,577 requested slots, all PS, overlap 0;
  0 slots either way); 833 distinct requested title constants (FMV 27), ALL `complete` and
  join-usable (14,577 occurrences); 3,124 draws with every request usable, 351 with no
  request, 0 with an unusable one (conserved). Requested title classes: DXT4_5 tiled
  unpacked base-only 315, DXT1 tiled packed base+mips 243, DXN 150, DXT4_5 packed 41, DXT1
  unpacked 21, k_8_8_8_8 17, k_32_FLOAT 6, cube k_8_8_8_8 6, DXT2_3 6, 3D k_8_8_8_8 5,
  k_16_16_16_16_FLOAT 4, ...; FMV: k_8_8_8_8 10, k_32_FLOAT 6, k_16_16_16_16_FLOAT 4, k_8
  linear planes 3, k_24_8_FLOAT 2, k_16_16_FLOAT 1, k_16 1 (no mips anywhere in the FMV
  set). /4: every `resolve_details` row and every other top-level key byte-identical to
  the accepted /4 outputs; core counters unchanged. **T3B4 NOT KILLED:** the normalized
  requested slot + range rows (post-complete draws, ordinal/frame removed; slot, stages,
  status, key geometry, base/mip intervals, join_usable) hash to
  `eb250fcefbcc2a98f47989157d4553da8b2f48a8d7ac5f5ac1216b364366c9c6` for ALL FOUR FMV
  traces (125 rows each) and
  `54270e856e2e8b5841da741e7bbb8856ef129797e6192946f001dd374070d4ac` for the title (3,475);
  the four FMV requested-constant sets and shader sets are identical; unused constants
  (the t21/t25 slot-15 extra of T3A4) are excluded by construction. Suite 140 tests
  (all pass); battery 365/365 killed, 0 survived, 0 crashed, 0 anchor errors (73 new `t3b-*` mutants: mask boundary
  12, normalization/expansion 16, binding-info gate 9, subresources 9, layout 21, census 6;
  the first full run left four survivors - the two table-sharing mutants and the layout's own
  max-level clamp were unobservable through the fixtures, and the JSON backing wiring mutant
  raised instead of failing - each was answered with an assertion; none was a tool defect). Not done: no TOOL-3C join, no producer/consumer
  match, no ownership, contents, HLE comparison, renderer, build, roll, E1-E8 or pixel
  claim.
- **TOOL-3C (schema /7): the TEMPORAL, COMPONENT-LEVEL, HALF-OPEN interval relation between
  resolve destination ranges and later requested texture backing intervals - the sixth and
  final planned tool slice; it closes the offline producer-to-consumer relation WITHOUT
  selecting an owner or inferring execution, identity, contents or format compatibility.**
  Producers are the `resolve_details` rows whose TOOL-2B range is `join_usable` with
  `invariants_hold`, a positive length and monotonic half-open endpoints with `end - start ==
  length` (every failed check is named per excluded row and counted; nothing wrapped, empty
  or inconsistent is coerced); the producer interval is `[extent_start,
  extent_end_exclusive)`. Consumers are the normal draws whose two post-translation stages
  are `translated`; each requested binding OCCURRENCE (not each distinct constant - the
  833 title constants occur 14,577 times) whose TOOL-3B backing is `join_usable` after the
  cvar filter contributes its `base_interval` and its `mips_interval` as SEPARATE
  components (a missing component is counted `absent`, a defective one by its defect). A
  producer is compared only with a STRICTLY LATER draw of the same trace
  (`consumer_draw_ord > producer_draw_ord`); the pair is exactly one of `equal`,
  `producer_contains_consumer`, `consumer_contains_producer`, `partial_overlap`,
  `disjoint` (endpoint contact is disjoint; `[max(starts), min(ends))` is kept for every
  non-disjoint pair); every overlapping earlier resolve is kept - no latest or preferred
  owner. Detail rows exist ONLY for non-disjoint pairs (producer resolve index / draw /
  interval / raw and adjusted base, consumer draw / stages / slot / exact 48-hex constant
  key / component / interval, relation, intersection, ordinal distance); the destination
  and key metadata are observations kept once on `producers.rows[]` and on the constant
  table and referenced by index and key, never called compatible. Every normal draw
  carries `resolve_relation` (eligible earlier producers, occurrences and exclusions,
  components by kind, candidate pairs, the five relation counts, non-disjoint pairs, the
  overlapping resolve draw ordinals). The top-level `resolve_consumer_relation` conserves
  `candidate_pairs == equal + producer_contains_consumer + consumer_contains_producer +
  partial_overlap + disjoint` and `detail_rows == the four non-disjoint buckets`, and
  reports resolves with zero later overlap, occurrences and components with zero overlap,
  the producers-per-component and components-per-producer multiplicity histograms, the
  T3C2 sets (raw producer base == component start, and adjusted extent start == component
  start, each with false positives = equal-but-disjoint pairs and false negatives =
  non-disjoint-but-unequal pairs over explicit denominators), and the cvar matrix (`true`
  = pinned default = the canonical detail table; `false` excludes occurrences whose backing
  has `allowed_by_cvar=true` without recomputing any layout; `same_as_default`, changed
  counts and affected occurrence keys when they differ). `--relations` lists producers,
  excluded rows and every non-disjoint pair; `--draw N --textures` prints the draw's
  relation summary and its rows; the summary prints the `relation:` line. **Classifier
  oracle:** the suite compares `classify_intervals` with an exhaustive brute-force SET
  oracle over all 441 pairs of the 21 half-open intervals inside [0, 7) (relation and exact
  intersection; every relation occurs) plus hand-derived near-uint32 vectors (top-of-space
  containment and equality, endpoint contact below the last byte, one-byte overlaps on both
  sides, shared-start and shared-end containment) and the three interval defects; the
  trace-shaped fixtures cover one base_A resolve between a draw before it and two after it
  (equal 1,888,256 B; producer-contains 4,096 B; consumer-contains 4,194,304 B; right and
  left partial 53,248 / 61,440 B; a ONE-BYTE partial [1F12F000, 1F130001); both endpoint
  contacts disjoint; a base-disjoint / mips-inside 18,184 B pair; the earlier draw gets no
  pair), five resolves with two excluded by every failed check (`rejected_msaa`:
  not_join_usable + no_range; a wrapped base: not_join_usable + invariants_violated +
  wrapped_interval), two base_A producers both kept on one component, a type-0 constant
  filtered only under cvar=false (matrix differs, affected occurrence retained), a no-data
  constant, a post-incomplete draw, the 3D slice-3 producer whose extent START lies past
  its raw (= adjusted) base (raw-base equality is neither necessary nor sufficient),
  direct injection of invariants / length / empty / wrapped defects with `join_usable`
  untouched, and the JSON/CLI wiring. **Five traces (2026-09-12, this tree):** every
  resolve is an eligible producer (26/26 per trace); every normal draw is post-complete;
  the title has 14,577 eligible occurrences giving 14,577 base and 8,401 mip components
  (6,176 occurrences define no mip interval), each FMV 31 base components and no mips.
  Title: 136,741 candidate pairs = equal 28 + producer_contains_consumer 0 +
  consumer_contains_producer 10,497 + partial_overlap 0 + disjoint 126,216 (conserved;
  10,525 detail rows); 23 resolves overlap a later component, 3 never do (draws 3426,
  3451, 3501 - the last is the swap-frontbuffer resolve); 12,802 occurrences and 21,203
  components overlap nothing. Each FMV: 479 pairs = 28 equal + 9 consumer_contains_producer
  + 442 disjoint (37 detail rows; 22 resolves overlap, 4 never: draws 34, 99, 124, 151).
  **T3C1 REFUTED** in every trace (10,525 / 37 non-disjoint pairs). **T3C3 REFUTED:** the
  title's producers-per-component histogram is {0: 21,203; 1: 21; 2: 4; 3: 1; 5: 1; 6:
  1,748}. The 10,497 `consumer_contains_producer` rows partition exactly (r1 correction of
  the 8316758 wording, which conflated the two parts): 1,748 LATER base components of the
  decoded `k2DOrStacked` four-slice 640x640 k_16 array backing each contain all six
  640x640 k_16 resolves (resolve draws #33, #35, #102, #104, #877, #879 - the rows
  TOOL-2B-r1 documented as the linear-span coincidence), 1,748 x 6 = 10,488 rows; three
  EARLIER base components of the same class relate only to the resolves already issued
  before their draw - consumer draw 34 to resolve draw 33 (1 row), consumer draw 103 to
  33/35/102 (3 rows), consumer draw 878 to 33/35/102/104/877 (5 rows) - adding 1 + 3 + 5 =
  9 rows. All 10,497 are `consumer_contains_producer`, k_16 into k_16, in the decoded
  `k2DOrStacked` four-slice class; only 10,488 are the six-resolves-per-component cohort.
  This is an address-interval relation between an issued resolve command and a later
  requested binding of a stacked-array (`k2DOrStacked`) backing; it is not cube evidence,
  not 3D-volume evidence and not proof of execution, contents or sampling. The FMV
  histogram is {0: 4; 1: 21; 2: 4; 3: 1; 5: 1}. **T3C2 REFUTED:** raw-base equality finds 3,529 title pairs (all
  non-disjoint, 0 false positives) but misses 6,996 non-disjoint pairs (FMV: 33 found, 4
  missed); adjusted-start equality gives the same sets because every producer here has
  `adjusted == raw` and `start == raw` - the synthetic scissor-cut and 3D fixtures show
  both a false positive and a false negative for the raw rule. **T3C4 NOT KILLED:** the
  normalized relation objects (details, per-draw summaries, census, T3C2, producers,
  consumers, matrix; input path removed) of the four FMV traces hash identically
  (`4e544c4be698ec150c66e8433f23217ded269ee5436526e73e941162f0093d99`, 103,354 bytes); the
  title differs in every content section. **T3C5 REFUTED (as Sol predicted):** every
  requested occurrence is type 2, the false filter excludes 0 occurrences in all five
  traces, both projections have identical counts and hashes (`same_as_default=true`,
  title hash `a0786df3b3c14eb9...`, FMV `8a663e28069666db...`); the false path is exercised
  by the synthetic type-0 fixture only. Equal-relation classes on file: k_8_8_8_8 12,
  k_16_16_16_16_FLOAT 7, k_32_FLOAT 6, k_24_8_FLOAT 2, k_16_16_FLOAT 1 per trace (title and
  FMV alike: the title's 28 equal pairs are the same FMV-era resolves), all at ordinal
  distance 1-36. Sizes: title 18,999,823 -> 30,946,352 bytes (the relation object is 8,341,561 bytes for
  10,525 detail rows plus 3,475 per-draw summaries), FMV 705,241 -> 856,237 (t16; t21/t25
  707,477 -> 858,473; eahd 705,226 -> 856,222; 45,212-byte relation objects). /4 rows, the lossless /5 expansion, every /6 table
  (shaders, constants, rows minus the new summary, manifest, requested census minus the
  sidecar identity) and all accepted core counters are unchanged on the five traces; the
  ONE expected /6 field change is `requested_binding_census.post_translation_table.
  file_sha256`: `aa36783a...` -> `a52ebbaf...` (the repaired sidecar). Suite 145 tests
  (all pass); battery 411/411 killed, 0 survived, 0 crashed, 0 anchor errors (46 new `t3c-*` mutants: classifier 7,
  temporal 4, component identity 6, filters 10, census 13, wiring 6; the first full run left the
  TOOL-3B shared-constant mutant alive because the relation code raised on a never-derived
  backing instead of failing an assertion - a fail-closed None guard restored the kill). Not done: no owner or latest producer selected, no
  execution, texture identity, contents or format compatibility inferred, no HLE
  comparison, renderer/director/frozen/cube/parked change, build, roll, E1-E8 or pixel
  claim.
- **Vertex fetches come from the control-flow walk** (`ucode.h` + `AnalyzeUcode`): CF pairs
  2-per-3-dwords up to the SHRINKING exec-address bound, exec `sequence` bits name the
  fetch slots, `FetchOpcode==0` is a vfetch, a mini fetch inherits constant/stride, a
  fetch whose dst swizzle writes nothing binds nothing (since /5 the enumeration is the
  shared `cf_exec_walk`; the accepted census numbers are unchanged). The r1 "every third dword"
  scan is kept only as `parse_vs_vfetch_naive` for `--vfetch-census`, which reports how
  often it disagrees (title trace: 49/54 distinct VS, 116 phantom fetches, all ALU slots).
- **INDIRECT_BUFFER (0x3F) is recorded as a two-dword marker** and its inner packets
  follow as their own records inside an IndirectBufferStart/End span; the dispatch is
  consumed once (`ib_dispatch_records`), never executed from memory. PFD (0x37) is
  recorded in full and takes the same path. A trace that opens or closes inside an IB
  (every title/FMV trace does) yields `leading_ib_ends`, `ib_wrapper_end_missing` and
  `ib_unbalanced` warnings, not refusals.
- `packets_executed` excludes the dispatch markers (title: 63,106 records, 63,054
  executed, 52 dispatches), matching `gfx/gpu_trace_replay.cpp`'s `packets=`.

**JSON (`schema: xtr_draws-summary/7`):** `input_path/bytes/sha256`, `xenia_build_sha`,
`title_id`, `frames`, `stopped_at_frame_limit`, `records` (per tag), `packet_records`,
`packets_executed`, `ib_dispatch_records`, `ib_spans`, `leading_ib_ends`,
`ib_wrapper_ends`, `draw_packets`, `draws`, `resolves`, `noop_draws`,
`unknown_mode_draws`, `zero_index_draws`, `screen_extent_events`, `event_writes`,
`memory_read_records/bytes`, `memory_write_records/bytes`, `memory_pages` (64 KB pages
touched by memory-read AND packet records), `unmapped_reads/bytes`, `registers_records`,
`register_values`, `edram_snapshots`, `gamma_ramps`, `shader_loads`, `distinct_shaders`
(`vs`, `ps`, `pairs` over identified draws only, `unidentified_draws`),
`resolve_details[]` (per kCopy draw: raw copy/dest/surface registers, decoded copy and
dest fields, `rt_info[4]`, `depth_info`, `source` = kind/index/register/raw/base/
fmt/fmt_name/exp_bias, and since `/3` `get_resolve_info_inputs` =
draw_packet_record_offset / vertex_fetch_0{raw[2], type, type_name, address_dwords,
address_bytes, size_dwords, size_bytes, endian} / vertex_payload{status, bytes_hex,
dwords_be[6], missing_bytes} / registers{the ten names above} / status / rejection /
not_in_trace[6], and since `/4` `guest_destination_range` = derivation_version / status /
reason / join_usable / copy_command, then only when arithmetic ran: vertices_swapped_bits /
half_pixel_offset / vertices_fixed_16_8 / rect_pixels / window_offset /
rect_after_window_offset / scissor / rect_after_scissor / rect_aligned_8 / msaa_samples /
surface_pitch_aligned / rect_after_pitch_clamp / rect_final / width_div_8 / height_div_8 /
destination / tiling / range / raw_vs_derived), `resolve_inputs` (complete, rejected,
incomplete, total, distinct_vertex_payloads, distinct_vertex_addresses, not_in_trace),
`derivation_manifest` (derivation_version, xenia_repository, xenia_commit, input_schema,
output, guest_range_host_parameter_dependencies = [], host_parameters_not_in_trace[6],
host_parameter_note, integer_model, stages{23 names -> permalinks}),
`guest_destination_ranges` (the seven status counts, total, join_usable,
adjusted_base_differs_from_raw, extent_start_differs_from_raw_base,
extent_differs_from_declared_linear_span), and since `/5` `static_texture_shader_analyses`
(ucode hash -> analysis_version / status / reason / ucode_dwords / cf_pair_bound /
exec_instructions / instruction_slots / fetch_slots / vertex_fetch_slots / instructions[]
with every TextureFetchInstruction field / binding_slots / binding_mask / binding,
non-binding and unknown-opcode instruction counts), `static_texture_bindings[]` (per
normal draw: draw_ord, frame, mode, vs, ps, analysis_status, stages{vs, ps: status,
reason, ucode_hash, binding_slots, binding_mask, instructions[], counts}, union_slots,
overlap_slots, bindings[]{slot, stages, instructions_per_stage, fetch_constant_type,
fetch_constant_type_name, is_texture_constant, raw[6], decoded{every
xe_gpu_texture_fetch_t field, size_1d/size_2d/size_3d, type_name, format_name,
dimension_name, base_address_bytes, mip_address_bytes, pitch_pixels, size}},
type2_slots, unused_type2_slots, referenced_non_texture_slots) and
`static_texture_binding_census` (the census above) - since `/6` the per-draw rows are
NORMALIZED: `static_texture_shaders` (ucode SHA-256 [+`-unmapped`] -> analysis_key,
ucode_sha256, ucode_xxh3_64, ucode_bytes, unmapped_bytes, stages_seen, analysis (the /5
analysis above), post_translation{vs/ps: status, reason, mask, binding_slots,
bindings[]{fetch_constant, dimension, dimension_name, is_signed}}),
`texture_fetch_constants` (48-hex raw -> key, raw[6], decoded (the /5 decode), requested,
backing{backing_version, status, reason, join_usable, fetch_constant_type[_name],
allowed_by_cvar, subresources, key, format, layout{array_size, max_level, packed_level,
base, mips[14], mip_offsets_bytes[14], mips_total_extent_bytes, undefined_in_pin},
base_interval, mips_interval} | null, backing_reason), `static_texture_bindings[]` rows as
described in the TOOL-3B bullet (stages{status, reason, ucode_hash, analysis_key,
binding_mask}, bindings[]{slot, stages, fetch_constant_key, is_texture_constant,
requested_stages}, post_translation, requested_binding_opportunities[]),
`texture_backing_manifest` (backing version, pin, host_defaults_assumed, integer model,
12 stages of permalinks) and `requested_binding_census` (table identity incl. the
table's SHA-256 and pin match; per-stage post status counts; draws post-complete;
requested slots per stage / union / overlap; requested-vs-analysis deltas; distinct
shaders translated; distinct and requested constants; backing statuses over distinct
constants and over per-draw occurrences with conservation; join-usable counts; draws
all-usable / some-unusable / no-request, conserved); `--expand-v5 FILE` writes the /5
projection rebuilt from the tables; since `/7` each row also carries `resolve_relation`
(relation_version, post_status, eligible_earlier_producers, requested_occurrences,
occurrences_eligible, occurrences_excluded{cvar_excluded, backing_not_join_usable},
components{base, mips}, components_excluded{absent, wrapped_interval, inconsistent_length,
empty_interval}, candidate_pairs, relations{5}, non_disjoint_pairs,
overlapping_producer_draw_ords) and the top level carries `resolve_consumer_relation`
(relation_version, meaning, definitions, canonical_cvar, producers{resolves_total,
eligible, excluded_rows_total, excluded_checks, excluded_rows[]{resolve_index, draw_ord,
status, reasons}, rows[]{resolve_index, draw_ord, frame, start, end_exclusive, length,
raw_base, adjusted_base, destination_observed}}, consumers{normal_draws,
draws_post_complete/incomplete, requested_occurrences, occurrences_eligible,
occurrences_excluded, components{base, mips, total}, components_excluded},
census{candidate_pairs, relations, conservation, conservation_holds, non_disjoint_pairs,
detail_rows, detail_rows_equal_non_disjoint, producers_with_later_overlap,
producers_with_zero_later_overlap{count, draw_ords}, occurrences_with_zero_overlap,
components_with_zero_overlap, multiplicity{producers_per_component,
components_per_producer, max_producers_per_component,
components_with_multiple_producers}, t3c1_any_non_disjoint_pair,
t3c3_every_component_at_most_one_producer}, t3c2{claim, denominators, raw_base_equal,
adjusted_start_equal each {pairs, and_non_disjoint, false_positives_equal_but_disjoint,
false_negatives_non_disjoint_not_equal, equals_full_relation}}, details[]{relation,
producer{resolve_index, draw_ord, start, end_exclusive, length, raw_base, adjusted_base},
consumer{draw_ord, slot, stages, fetch_constant_key, component, start, end_exclusive,
length}, intersection{start, end_exclusive, length}, ordinal_distance}, cvar_matrix{true,
false each {allow_invalid_fetch_constants, occurrences_excluded_by_cvar,
occurrences_eligible, components, candidate_pairs, relations, detail_rows,
normalized_hash}, same_as_default, differences}). `opcode_histogram`,
`unknown_opcodes`, `vfetch_census` (loads, parsed, failed, bindings, attrs, per_format,
naive_* divergences), `warning_counts`, `warnings` (first 64), `status`, `refusal`.
Shader hashes are XXH3-64 of the big-endian ucode (`ShaderCache::hashUcode`); `null`
when unhashed, never 0. Schema history: `/1` (commit 80e7c83) counted a `(null, null)`
pair as one identity and had no `resolve_details`; `/2` (f86803c, the RC-1 census
pin) added `resolve_details` and the selected source; `/3` (9f32be9, r1 69c154f) added
`get_resolve_info_inputs` and `resolve_inputs`; `/4` (TOOL-2B) adds
`guest_destination_range` per row plus `derivation_manifest` and
`guest_destination_ranges` at the top level and changes nothing else (every /3 field and
every core counter byte-identical on the five traces); `/5` (TOOL-3A) adds
`static_texture_shader_analyses`, `static_texture_bindings` and
`static_texture_binding_census` and changes nothing else (every /4 `resolve_details` row,
the manifest and every other top-level key byte-identical on the five traces); `/6`
(TOOL-3B) replaces the /5 per-draw repetition by the two normalized tables (the /5
projection is reconstructed exactly by `--expand-v5` on all five traces), adds
`post_translation` and `requested_binding_opportunities` per row,
`texture_backing_manifest` and `requested_binding_census`, and changes nothing else
(every /4 row and every other top-level key byte-identical on the five traces); `/7`
(TOOL-3C) adds `resolve_relation` per normal-draw row and the top-level
`resolve_consumer_relation`, and changes nothing else except the repaired sidecar's
SHA-256 inside `requested_binding_census.post_translation_table` (every /4 row, the /5
expansion and every /6 table byte-identical on the five traces). The RC-1 census document
stays pinned to `/2` at f86803c and is not rewritten.

**Cross-checks on file (2026-09-11, this tree):**

| number | xtr_draws v2 | independent source |
|---|---|---|
| title packet records / IB spans | 63,106 / 52 | MCLA `xtr_dump_validator` @ 5aa19696 (compiled from the pin) |
| title packets executed | 63,054 | `gfx/gpu_trace_replay.cpp` SUMMARY `packets=63054` |
| title draw packets | 3,501 | `xenia-gpu-d3d12-trace-dump` "draw packets=3501"; C++ replay `draws=3501` |
| title draws by mode 4/5/6 | 1,850 / 1,625 / 26 | engine-free register scan (scratch, type-0 + SET_CONSTANT writes to 0x2208) |
| title resolve sources (r1) | draw 3423 COLOR1 base 658; draw 3404 DEPTH base 2D0 fmt 1 | Sol's TOOL-1 review anchors (`--draw 3423`: RT0=000005A0, RT1=00000658) |
| last kCopy draw #3501 dest | 1F130000 1280x720 | XE_SWAP body: frontbuffer 1F130000, 0x500 x 0x2D0 |
| title memory reads / writes | 4,450 / 24 | MCLA validator; C++ replay `memReads=4450` |
| title memory pages | 2,540 | independent page counter: 2,466 memory-read pages + 78 packet pages, union 2,540 |
| title VS/PS loads | 512 / 263 | C++ replay `vsLoads=512 psLoads=263` |
| title vfetch census | 602 bindings / 1,665 attrs, per-format identical | C++ `[VFETCH] SUMMARY`; per-VS join 54/54 agree with `gfx/xenos_ucode.cpp` |
| FMV t16/t21/t25/eahd draw packets | 151 each (125 draws + 26 resolves) | Xenia trace-dump "draw packets=151" x4 |
| FMV packet records | 2,694 / 1,967 / 1,967 / 1,967 | MCLA validator |

The readiness observation's "zero resolves" for the title trace is WITHDRAWN and
corrected to 26 (kCopy-mode draws #3404..#3501: the bloom/downsample chain, two
1280x720 convert copies and the final clear+copy into the swap frontbuffer). "One
frame, 3,501 draws, 2,540 memory pages" are RETAINED.

**Evidence limits:** every trace on file is ONE frame; counts are per trace, not per
title. `get_resolve_info_inputs` is a CAPTURE; `guest_destination_range` is the
PRODUCER-side derivation of it and stops at the written guest range: no consumer
relationship, texture ownership or contents, HLE-fixture comparison or pixel claim is
computed or claimed, and the six `not_in_trace` host parameters are unknowable from a
trace and enter no range. The C++ oracle is an independent transcription of the same
pinned source, not a Xenia-side dump of resolve ranges, so its agreement is between two
readings of the pin; the title's 26 rectangles all start at (0,0), so the adjusted-base
path is exercised only by the synthetic corpus and the unit fixtures, not by a real
trace on file. `static_texture_bindings` is the analysis-stage STATIC binding set of the
bound ucode, not proof that a sample executes (predicates, branches, coordinates and LOD
are unproven) and not Xenia's post-translation request mask; the fetch constants it
decodes are the register values at the draw, not a memory layout - no producer/consumer
join, no ownership or contents claim is made. The TOOL-3A oracle uses Xenia's own header
structs, so its agreement proves the Python decode matches the pinned layout, not that
the layout matches hardware. Since `/6`, `requested_binding_opportunities` are the slots
Xenia's ACTUAL pinned translator leaves in the post-translation mask, recorded by one run
of that translator (`archive/tool3b/masks.json`) and consumed by the tool - a table for a
different pin is refused, a shader absent from it is `not_in_table`, and the tool still
proves no sample executes (predicates, branches, coordinates, LOD unproven). The
base/mip intervals are what pinned Xenia would watch for the texture under the pinned
default `gpu_allow_invalid_fetch_constants=true`; they are not bytes proven sampled, not a
producer match and not a consumer join; the layout oracle CALLS the pinned functions, so
its agreement proves the port matches the pin, not that the pin matches hardware; the
title exercises no 1D, no `unsupported_format` and no `invalid_1d` constant, so those
branches are proven by the synthetic corpus and the suite only. Since `/7`, a
`resolve_consumer_relation` row says that a resolve-command destination interval and a
later requested backing interval stand in a byte-interval relation - both remain
OPPORTUNITIES (a copy command issued, a texture requested), so no row proves that the
copy or a sample executed, that the bytes were read, that the two describe one resource,
or that their formats are compatible (the destination and key metadata are observations);
when several earlier resolves overlap one component all are listed and none is called the
owner or the latest writer; the relation is per capture and per trace (no cross-trace
address identity is asserted), and the cvar filter is inert on these five captures by
construction, so its false path is proven by a synthetic fixture only. "Resolve" is Xenia's kCopy-draw definition; no Xenia log line names resolves, so
the cross-check is the register scan plus the swap pointer, not a second decoder; the
source-register selection is cross-checked against the pinned source and the two review
anchors, not against a Xenia-side dump of resolve sources. EDRAM and gamma payloads are
size- and decode-validated but their CONTENTS are not interpreted. Shader
identity is by ucode hash; no Xenia-side per-shader dump exists at the pin, so distinct
VS/PS counts (54/56) have no external check. Register state printed for a draw is the
trace's, not ours; `in_ib` is unknown for packets before a mid-IB opening. Not a pixel
oracle; nothing here is an E1-E8 verdict or a pixel-progress claim.

**Provenance / reuse:** Xenia pin above (`trace_protocol.h`,
`pm4_command_processor_implement.h`, `xenos.h`, `registers.h`, `register_table.inc`,
`register_file.h`, `ucode.h`, `shader_translator.cc`, `trace_reader.cc`,
`trace_player.cc`, `d3d12/d3d12_command_processor.cc`). MCLA
(`3bdull4h2008/mcla-pc` @ `5aa1969604b13fe44b5acc81afbed2943926a4fa`, reused under the
2026-09-11 author-permission ruling): `src/renderer/xtr_dump_validator.cpp` shaped the
per-record byte accounting, the exit-nonzero-with-reason contract and the synthetic
self-test builder, and was compiled from the pin as the independent record-level
cross-check; `src/renderer/xenos_microcode.cpp` was consulted and NOT reused for
vertex-fetch decode (its word-1 layout disagrees with `ucode.h`). No third-party code
from that repository's `third_party/` is used.

### 1.7 Live-Xenia oracle — watch the predicate flip on the working reference
**When:** any divergence where "what should happen" exists in a Xenia boot (HARD RULE: never
theorize about a divergence you haven't observed on the working reference). This is the LIVE
upgrade over golden-dump diffing: the dump shows the healthy END state; a live watchpoint names
WHO writes the predicate and WHEN.

```
1. python tools/xenia_pad.py            # virtual pad first (Xenia enumerates on launch)
2. & "R:\...\Xenia Canary\xenia_canary.exe" "<iso>"     # stock menu-reaching build
3. mcp: frida_attach target=xenia_canary                # base auto-falls-back to 0x100000000
4. mcp: frida_watchpoint guest_va=0x829761E4 size=4 mode=w   # e.g. SM state (SM+4)
5. drive with tools/pad_cmd.txt; screenshot via tools/xenia_shot.ps1
6. mcp: frida_get_hits                  # hit backtraces = the writer at the moment it flips
```

`tools/frida_mcp.py` resolves the guest base from our harness's `g_debugGuestBase` export and,
when absent (attached to Xenia), falls back to Xenia's fixed membase `0x100000000` (validated by
the MZ header at `+0x82000000`). Guest memory is BIG-ENDIAN in Xenia (`82 08 6f f0`, not our
recomp's LE) — see `tools/XENIA_HARNESS.md` for byte-order traps, JIT-fault register recovery,
and MemoryAccessMonitor gotchas. Restart the frida-superman MCP server after pulling changes.

### 1.8 sr_drive — unattended UI automation (reach an era without a human) [iter-450V]
**When:** the era you need is only reachable through real input (anything past PRESS START).
Before this, each acquisition burned a whole 300 s roll on a human being at the keyboard at the
right second, and iter-450UK caught a press *silently discarded* because the window had lost
focus at that moment.

```powershell
.\tools\sr_drive.ps1 -Scenario tools\scenarios\title_to_frontend.json `
    -Out archive\<name> -Gates "SR_PRESENT_DUMP_DIR=archive/<name>/frames" -TimeoutSeconds 420
```

**It is HOST TEST INFRASTRUCTURE, and stays outside production behaviour.** It holds keys the way
a human would, so the guest still receives input through the normal `mergeKeyboardPad` →
`XamInputGetState` path. It never writes guest memory, calls a guest function, fabricates state,
or adds autoplay. **Since iter-464 the keys go through the HARNESS KEYBOARD CHANNEL by default**
(§1.8a): the game window stays in the background and needs no focus. `-ForegroundInput` restores
the old black-box path (`SetForegroundWindow` + `SendInput`, window in front, real keyboard read)
for a test that must prove the OS input path itself.

Scenarios are JSON; each step names a precondition (`await`), an action (`key`/`holdMs`), a
delivery confirmation (`confirm`, auto-derived from the key's pad mask), an expected observation
(`expect`), a timeout, and an optional `dwellMs` capture window. A LAST step with
`expectExitCode: N` (+ `exitTimeoutMs`, default 10000) makes the process exit the pass
condition: only that exit code within the timeout passes (a crash code, an unreadable code or a
process still running fails); `femenu_load_exit.json` uses it for `SR_Quit`.
`expectFresh: true` counts only expect hits that appear after the step starts. Use it when the
marker can already be in the log (a screen path requested at the title and again in game).
`onlyIfObserved: "<regex>"` (2026-10-14) runs the step only when the line the latest `expect` observed
matches; otherwise the step is skipped and logged (`NOT run - the latest observation does not match`).
perf_run --from-save uses it: `load-lands` accepts the METROPOLIS / FORTRESS carousel or the HUD, and
A = METROPOLIS runs only after a carousel landing.
`expectSince: "<earlier step>"` (2026-10-15) counts only expect hits that arrived after that earlier step was
entered (its boundary is armed at its entry, before its action). Use it when the marker can be in the log from
before the action AND can follow the previous step's observation within one log read, where `expectFresh` would
already count it as old (the HUD landing is logged 11 ms after the load event). The named step must come earlier
and must not be skippable (`cutsceneSkip` / `onlyIfObserved`); a boundary that was never armed fails the step.
perf_run --from-save arms it at `load-slot`: `load-lands` and `in-game` (the gameplay input context) count only
after the slot's load press, and `sim-runs` then waits for a fresh `[HIGHFPS_PACE]` window with the guest sim
advancing before the capture window opens. `tools/sr_drive_sim.ps1` runs a scenario's steps over a simulated
log with the driver's own decision functions (`tools/perf/test_from_save_landing.py`).

**Scenario contract (iter-463):** every scenario is validated before anything else happens,
`-ValidateOnly` included (`Assert-SrScenario` in `sr_drive_core.ps1`; exit 2 on a defect).
- It checks canonical field names (an unknown or re-cased field is refused), keys from the
  driver's own table, regexes that compile, positive integer times, booleans, and fields that
  need another field (`holdMs` without `key`, `expectFresh` without `expect`). It also requires
  `expectExitCode` to be the last step.
- A top-level `"requires": {"contentSeed": true}` makes a real run refuse, before any directory,
  env or process action, unless the seed exists and holds a
  `<xuid>\454107ED\00000001\<package>` folder. The seed is the one the run would use:
  scenario gates, then `-Gates`, then the environment.
- Tests: `tools/test_sr_drive_scenarios.ps1`.
- **Cutscene skipping (iter-467, owner 2026-10-11).** Rolls skip cutscenes: pressing the game's skip
  input is a natural player action, not forcing.
  - **The input.** A skippable FMV polls event 187 (FMV_SKIP: A (alt) and START in the gameplay
    contexts). An in-engine cinematic runs in the Igc context (`[PC_CONTROLS] context cinematic (igc
    type 8)`), where A = 186. So the skip is pad A: the drive's `A` key, the harness channel's `Z`.
  - **The confirmation.** A skipped movie logs no EOM record. The new game's opening FMV, killed by
    pad A, logs no `[EV187] ... ACCEPTED-KILL` either (forcing gate/merge_ready, 2026-10-11: that edge is
    the title card's at boot). Its end is the player getting control: the first `[PC_CONTROLS] context
    gameplay ... mode <a mode>` after the HUD request (the HUD request's own line says `mode -`). The
    gate's in-game start (`perf_report.MOVIE_SKIP`) and the skip step take that line, an EV187 accept
    or the movie's EOM. A cinematic ends when the context returns to `gameplay`.
  - **Scenarios.** A step with `"cutsceneSkip": true` runs only while skipping is ON.
    `natural_new`, `femenu_new`, `perf_player_gameplay` and `perf_tutorial` carry
    `skip-opening-movie`: A pulses, `holdUntilConfirmed`, until the player has control (or an accept,
    or the movie's own EOM).
    It is optional, because a missed skip only costs the 30 s movie.
  - **ON by default** (gate, proof, A/B and planner rolls).
  - **OFF** when any of these holds:
    - the scenario has `"coverMovies": true`;
    - its name or file says `full` or `whole_run`;
    - the run passes `sr_drive -SkipCutscenes off`;
    - `SR_DRIVE_SKIP_CUTSCENES=0` is set in the environment, the scenario gates or `-Gates`.
    Use OFF for whole-run meter coverage (C1 counts movies) and movie-stall measurements.
    `-SkipCutscenes on|off` beats everything else. The drive logs `cutscene skipping ON|OFF (why)`.
  - **The gate's in-game start** takes the accept after the HUD request
    (`tools/perf/perf_report.py` `MOVIE_SKIP`; fixture `skipped_movie` in `test_flicker_gate.py`).
  - **Planner rolls.** `tools/devtools/skip_run.py run --executor kernel --skip-cutscenes
    auto|on|off`: auto follows `SR_DRIVE_SKIP_CUTSCENES`. It taps A every 2.5 s inside each
    `await_natural` window (from its `await_start` event to the step's next event) and logs to
    `<run>/cutscene_skip.jsonl`. It never taps during the linger.
  - **Tapes** skip a cinematic with the existing idiom
    `tap_until Z 300 2500 20 \[PC_CONTROLS\] context gameplay`.
- **Retail LOAD path:** `natural_load.json` (with `-Gates SR_MODS=0,SR_CONTENT_DIR=<seed>`) and
  `natural_load_empty.json` (no save).
  - Their markers are `[PATH_RESOLVE] wrapper site ... name='ShowSlotSelector'`, `'FillSlotInfo'`
    and `'LoadComplete'`. They are always logged, one per native-to-AS call by name.
  - DEVTOOLS_DESIGN section 8 "Built".

**Screen markers (iter-463):** `[APT_SCREEN] path='<movie>' seen=K req=N` is logged for each Apt
load-by-path request: the first 16 per distinct path, then at most one per path per 30 s
(`kernel/fe_flow_watch.cpp` `logAptScreen`). Use it to wait for a screen opening after gameplay
starts; `[APT_REQ]` stops after its 24th request for the whole run. Its lines are count-unsafe;
`[GAUGE] aptReq` is the uncapped count.

- **Every pulse is confirmed, never assumed.** `SR_KB_PAD_OBS=1` makes `[KBPAD_OBS] mappedMask=`
  report whether the guest actually sampled the key, so a discarded press is a *named* failure.
  Every nonzero mask is logged unthrottled (`kernel/stubs.cpp`), so absence is real evidence.
- **Use `holdUntilConfirmed: true` for anything past the title.** The front end polls input at
  ~1 Hz during its load, where a fixed 180 ms press has ~1-in-5 odds — measured twice. Holding
  until observed (then releasing immediately) turned 1/5 into 1/1.
- **The log is read incrementally.** A run is ~0.6M lines; `Select-String` polling cannot keep up
  and silently misses transitions. All patterns are registered up front and counted from byte 0.
- Key-up is guaranteed in a `finally`; the HWND is re-validated before every action; artifacts are
  retained attempt-tagged under `-Out` on success **and** failure.

`python tools/sr_ratecheck.py <log> --bucket 20 [--count REGEX]` reads the responsiveness census
off monotonic counters (`[KBPAD_OBS] poll=`, `[D3D_DIAG] present #`, `[RES_REQ] #`), so rates stay
exact even where the lines themselves are throttled.

⚠ **iter-450V lesson:** a diagnostic in `shimResRequest` spawned one detached OS thread *per*
world-flow request — 29,912 in one run — and `[FSM_WALK]` logged 205,620 unGATED lines through the
global log spinlock. Both are now default-OFF (`SR_FLOWREC_PROBE`, `SR_RES_REQ_TRACE`,
`SR_FSM_WALK_TRACE`). Check for this shape before trusting any timing in a hot era.
⛔ **And the meta-lesson (iter-450W director verdict):** I reported that gating them "changed no
guest metric", on the strength of a **cross-roll** A/B. Under DOGMA that is unattributable **in
either direction** — a null result across two different rolls establishes no more than a positive
one would. The honest statement is "no measurable difference was observed between these two rolls;
causality remains undetermined." The gating is justified as hygiene, not as a performance fix.
Within-fixture replay A/Bs are the only attributable kind; see §1.6.

### 1.8a Automated runs stay in the background [iter-464]
**Why:** the user works on this PC while rolls run, and every roll used to take the foreground:
`Start-Process` gave the console-subsystem game a new console (on Windows 11 a Windows Terminal
window that held the foreground ~2.5 s at launch - measured, `archive/v1_bgruns/rollA/fgwatch.log`),
SDL activated the game window on creation, `sr_drive.ps1` called `SetForegroundWindow` before every
key (the keyboard merge was focus-gated), and `xenia_shot.ps1` restored + foregrounded Xenia.
**The rule now: an automated run never activates or raises a window, and never reads the user's
keyboard or controller.**

| Piece | What it does |
|---|---|
| Game policy (`kernel/host_automation.{hpp,cpp}`, one `[AUTOMATION]` boot line) | `SR_INSTANCE_ID` set (every roll harness sets it; a player never does) = automated. Automated (or a trace replay) => the window is shown WITHOUT activation (SDL3 `SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN=0`, + `ACTIVATE_WHEN_RAISED=0`; SDL2 was `NO_ACTIVATION_WHEN_SHOWN`) and parked at `HWND_BOTTOM` (`SWP_NOACTIVATE`); nothing raises it later. Rendering and the `SR_PRESENT_DUMP` readback do not depend on visibility (it is occluded, never minimized). |
| Background QoS | A background window must not mean a slower game: Windows 11 stops honouring the 1 ms timer request of an occluded process. Measured `Sleep(1)` with the window in the background: **15.3 ms** with the OS default (`P_noqos1`) vs **1.7 ms** after the opt-out (`P_qos1`, `SR_BG_PROBE=1` lines). The automated process therefore opts out of power throttling (`SetProcessInformation(ProcessPowerThrottling)`, execution speed + timer resolution). `SR_BG_HIGH_QOS=0` = OS default (A/B). |
| Harness keyboard channel | Named file mapping `Local\SR_HARNESS_INPUT_<SR_INSTANCE_ID>` (4 KiB; +0 magic `SRHI`, +4 version 1, +8 game PID, +0x0C merges that read it, +0x10 last mapped pad mask, +0x40 512 SDL-scancode bytes, nonzero = held). The game creates it before its window; `mergeKeyboardPad` maps the bytes through the SAME iter-450S table as a physical keyboard (`mapKeysToPad`), so a harness key reaches the guest exactly as that key would - no focus. The F9 trace hotkey works through it too (rising edge). Harness side: `Open-SrHarnessInput` & co. in `tools/sr_drive_core.ps1` (refuses a channel stamped with another PID). |
| Input isolation | An automated run ignores the real keyboard (incl. F9) and does not poll XInput, so the user's typing or controller never reaches a roll (`[KBPAD_OBS] ... REJECTED:real-input-isolated` / `src=harness`). `SR_HARNESS_REAL_INPUT=1` lets real input back in (never for rolls). Pad tests use `SR_HARNESS_VPAD=1`: only a ViGEm virtual pad is opened (`kernel/vpad_filter.hpp`, `[INPUT] vpad filter: ACCEPTED/REJECTED`). |
| `sr_drive.ps1` | Harness channel by default (no `SetForegroundWindow`, no `SendInput`; release proofs read the channel back). `-ForegroundInput` = the old black-box path (sets `SR_WINDOW_BACKGROUND=0 SR_HARNESS_INPUT=0 SR_HARNESS_REAL_INPUT=1`, restores the caller's values at the end). The run summary prints the game's merge count for the channel. |
| Game launchers | `Start-SrGameProcess` (`tools/bg_window.ps1`): `UseShellExecute=false` + `CreateNoWindow`, so no console window; optional stdout/stderr files. Used by `run_recomp_x`, `sr_drive`, `replay_ab`, `hle_replay`, `run-30s`, `run-test-capture-log`, `verify_present_window`. Python launchers (`subprocess.run`) inherit the caller's console and open none. |
| Xenia | `powershell -File tools\bg_window.ps1 -BgLaunch <xenia_canary.exe> "<iso>"` (each trailing argument is quoted for the child, so a path with spaces stays one argument) (or `Start-SrBackgroundProcess` + `Move-SrWindowToBottom` + `Start-SrWindowGuard`; Python: `tools/bg_window.py` `popen_background` + `guard_background`): `STARTUPINFO` `SW_SHOWNOACTIVATE`, windows to `HWND_BOTTOM`, then a **lifetime guard** (a hidden process, no inherited handles; `-BgNoGuard` skips it, `-BgGuardLog <f>` records each event): whenever the app takes the foreground WITHOUT a user gesture (mouse button / Alt / Win key down in the last 0.6 s) the foreground goes straight back to the window that had it and the app goes to the bottom; a user who clicks or Alt+Tabs to it keeps it. Needed because the stock Xenia activates itself ~35 s after launch, which gets through while the user has the Start/Search flyout open (measured once, `v2_xenia3`); a self-activating test window is handed back within ~70 ms per steal (PowerShell and Python guards). Used by `xenia_gpu_trace.ps1`, `xenia-trace/run_oracle_trace.ps1`, `xenia_collision_route.py`. `xenia_shot.ps1` captures with `PrintWindow(PW_RENDERFULLCONTENT)` (works behind other windows; `-Foreground` = old path). The virtual pad (`xenia_pad.py`, ViGEm) needs no focus: `v2_xenia5` captured PRESS START and, after a pad `@START`, the SELECT A GAMER PROFILE popup with Xenia never in the foreground. ⚠ Xenia still reads every XInput pad, so a user's real controller reaches a harness Xenia (no isolation on that side). |
| Verify | `powershell -File tools\bg_window.ps1 -BgWatch <out.log> -BgSeconds N [-BgFlag SupermanReturns,xenia_canary,WindowsTerminal,conhost,OpenConsole]`: the foreground owner (name + PID) every 0.5 s; exit 1 if a flagged process ever held it. Attribute by PID: during this lane's rolls other lanes' replays of the pre-iter-464 exe took the foreground (e.g. 30 samples in `v2_xenia5`), never the roll's own process. |

Env: `SR_WINDOW_BACKGROUND=1/0` (default: automated or trace replay), `SR_HARNESS_INPUT=1/0`
(default: automated), `SR_HARNESS_REAL_INPUT=1/0` (default: NOT automated), `SR_BG_HIGH_QOS=0`,
`SR_BG_PROBE=1` (observe-only `[BG_PROBE]` sleep-resolution/foreground lines, every 10 s for 3 min).
**Rolls (iter-464, `archive/v1_bgruns/`, the user working in Cursor/Chrome throughout; the
foreground watcher ran for each roll):**
- `v2_recomp1` - `run_recomp_x -K 1 -TimeoutSeconds 120`: 295 samples, the game never in the
  foreground; `[AUTOMATION] ... window=background qos=high`; `[BG_PROBE]` `Sleep(1)` 1.8-1.9 ms with
  `foregroundIsUs=0`.
- `v2_femenu3` - `sr_drive femenu_new.json -Gates SR_MODS=custom_frontend` on the harness channel:
  **SUCCESS**, the same outcome as the foreground baselines `archive/v1_frontend/menu/new1,new2`
  (press-start pulses delivered with `focus=0 sdl=0 src=harness`, `claim LoadCreate`, NEW GAME,
  `screens/hud/main_hud`); 647 samples, 0 of the game's; the custom MAIN MENU and the intro
  cinematic captured by `SR_PRESENT_DUMP` with the window in the background.
  Two earlier attempts failed for game-side reasons, not attributable to this change (one roll
  each): `v2_femenu1` - the `start-bootflow` press fell inside a 2.3 s `[CS_WAIT]` stall, the
  documented harness-miss class; `v2_femenu2` - a boot-time spin (`[FC108]` 274M calls) during a
  concurrent guest build. Neither showed the game in the foreground.
- `v2_xenia5` - Xenia via `-BgLaunch` + the virtual pad: 431 samples, 0 of Xenia's; the
  `PrintWindow` shots show PRESS START and, after `@START`, SELECT A GAMER PROFILE.

Players set none of these and see no change. Tests: `build/difftest.exe --host-automation-test`
(policy table, channel name, mapping table key by key, the real channel opened by name),
`tools/test_sr_drive_harness.ps1` (+ every `tools/test_sr_drive_*.ps1`).

Because the window no longer takes the foreground, `tools/game_slot.py acquire` no longer waits for
the user to be idle by default (`--user-idle-min` defaults to 0; pass a value or set
`SR_SLOT_USER_IDLE_MIN` to wait anyway). Exception: `verify_present_window.ps1` still makes the
window TOPMOST (its gdigrab capture reads the desktop) - never activated, but it covers that screen
region, so run it when the user is away.

⛔ **New code:** any new REAL input source (mouse, another key path) must honour
`automation::policy().realInput`; nothing may call `SetForegroundWindow`/`SDL_RaiseWindow`/a plain
`ShowWindow` on an automated run; new launchers use `Start-SrGameProcess` / `bg_window`.

### 1.8c Combined correctness roll: several lanes in one roll [iter-467]

The game slot is the bottleneck (one game process at a time, four lanes). Correctness checks
(CHECK gates, censuses, the native meter, whole-run frames) do not interfere with each other, so
one whole run of ONE exe can carry all of them:

    python tools/check_roll.py --exe <snapshot exe> --tag <name> [--only lane1,lane2] [--dry-run]
    python tools/check_roll.py --grade archive/v1_checkroll/<tag>

- `tools/check_roll.json`: each lane adds entries {lane, name, enabled, gates, pass, fail}.
  `pass` must match at least once, `fail` never. Keep `enabled=false` until the code is on the
  exe under test. Two entries that set one variable differently stop the roll (gate conflict).
- The roll is `tools/player/roll_player.ps1` (holds the slot as owner `check-roll`, background
  window, harness input). Report: `<out>/check_report.json` + one line per check.
- For this to pay off, lanes merge gated-OFF work to dev as soon as it builds and passes replay
  9/9 with the gate off, so one dev exe carries every lane's checks.
- NOT for perf: CHECK modes slow the game. Perf comparisons use in-roll toggling (alternate a
  feature on/off by window inside one roll, 1.8d), never a check roll's fps.

### 1.8d In-roll A/B: several perf features in one roll [iter-467]

A classic A/B costs 6 rolls (3 OFF / 3 ON) and live-roll noise (+-0.4 ms tick) hides gains under
~0.5 ms. Instead, ONE uncapped roll flips each feature per [HIGHFPS_PACE] window (~2 s):

    perf_run.py ... --gates "SR_HIGHFPS=uncapped,SR_HIGHFPS_PACE=1,SR_FOO=1,SR_BAR=1,SR_AB_TOGGLE=SR_FOO;SR_BAR"
    python tools/perf/ab_windows.py <roll> [<roll> ...] [--pool]

- `kernel/ab_toggle.hpp`: a feature joins by reading its flag at the point of use
  (`return v && (ab < 0 || sr::abt::on(ab));` with `ab = sr::abt::reg("SR_FOO")`), never a startup
  latch. The toggle only switches OFF what the feature's env gate enabled, so set `SR_FOO=1` for a
  default-OFF feature. Only exact features (CHECK-proven) may be toggled.
- Schedule: block-paired (in every window pair each feature is ON in exactly one, chosen per
  feature by a seeded hash, `SR_AB_SEED`), so features toggled together stay separable. The pace
  line ends with ` | ab=0x<mask>` (bit i = the i-th name; the `[AB_TOGGLE]` line names the bits).
- `ab_windows.py`: over the ingame era's windows, per feature ON minus OFF of the render tick
  (`tick_cost_ms render`), the frame interval median (`frame_p50`), fps and thread CPU
  (perf/threads.csv), by OLS and by paired-difference OLS over the window pairs (the tighter one:
  +-0.20 ms per feature from two rolls with seven features). `--pool` fits several rolls.
- A feature that cannot flip at runtime (a codegen / compiler change) needs a classic interleaved
  A/B on two exe snapshots.
- **The frozen-pass A/B (`SR_AB_PRESENT=1`, iter-467 native-a P1):** every listed feature flips at
  EVERY guest present instead (VdSwap: even presents ON, odd OFF; `[AB_PRESENT] swap=<n> ab=0x..`
  logged for the first 16 and while the sim is held). With the dense sim freeze (1.8f) consecutive
  dumped presents then come from alternate paths, and `freeze_diff --exact` compares them directly:
  an exact freeze = both paths pixel-identical. It is the EXECUTION check of a native-record row
  (NATIVE_ROADMAP rule 3); run it under the segment STRESS too:
  `SR_NATIVE_SEG_STRESS=<bytes>` makes the native draw path take the XDK's own kick (820FD8C0, at
  the XDK's own check sites) once that many bytes went into the segment, so the segment switch
  inside a bind / reserve - where the flicker lived - fires on a large share of draws (1024 =
  roughly every other draw in gameplay). The periodic `[NATIVE_FLUSH]` line reports `segStress=`,
  `reserveFlush=` (switches inside the immediate VS load) and `epochStale=` (switches after which the
  dedup mirror still held groups although dev+13508 moved; must stay 0).

### 1.8e Batch traps: exe snapshots and the game slot [iter-467]

- **Snapshot with `tools/snapshot_exe.ps1`, never a plain copy of the exe.** The snapshot folder
  needs the runtime DLLs (SDL3, dxcompiler, dxil, ffmpeg_xenia), the pdb, `data/` and the
  port-feature recipes. A bare `SupermanReturns.exe` copy fails to load: replays print
  `NO FRAME PRODUCED` in under a second and a perf_run roll waits for a log that never appears
  until its HUD timeout (iter-467 guestcode lost a batch to this).
- **`tools/replay_batch.py run` releases the game slot when it finishes**, even when the calling
  batch acquired it. A batch that runs replays and then rolls either does the replays last or
  runs the jobs without touching the slot (`archive/v1_gc3/replay_nolock.py`: imports
  replay_batch and calls `run_job` per job). Otherwise another lane's reservation takes the slot
  between the replays and the rolls.
- `game_slot.py status` prints `next: none` when nothing is reserved; one reservation at a time,
  and the orchestrator may `--force` over yours.
- **Build configs:** dev builds (`cmake --preset default`, `build/`) have ThinLTO OFF; the player
  package is built by `tools/player/make_player_build.ps1` from `cmake --preset player`
  (`build-player/`, `SR_THINLTO=ON`, PERFORMANCE 2.18). A codegen A/B snapshots both builds with
  `snapshot_exe.ps1` (run it with the build dir in place: it copies `build/`).

### 1.8f The flicker gate: every present of a span + sim-freeze exact [iter-467]

One-present defects (an object vanishing for one present, a draw executed with another draw's
state) are invisible to sparse sheets and to CHECKs that compare what was GENERATED (the S1 native
flush CHECK proved 9.96M records byte-exact while every gameplay freeze flickered: the defect was past
generation). Two instruments measure them directly; both run perf_run's `gameplay` scenario at
`SR_HIGHFPS=uncapped` with `SR_INSTANCE_ID` set:

    python tools/perf/flicker_gate.py --exe <exe> --out <dir> [--gates "SR_X=1"] [--only span|freeze]
    python tools/check_roll.py --exe <exe> --tag <t> --flicker        # the gate beside the check roll
    python tools/check_roll.py --exe <exe> --tag <t> --flicker-only   # the gate alone (lane 'flicker')

- **The verdict rests on the freeze.** The span is a coarse screen (`--span-max 5`: fail at 6 or more
  counted presents): a clean exe scores 0-1 in a calm scene and up to 5 where a car passes close or a
  speed-blur pulses (trunk_span_off, S1 off), while the S1 regression scored 3 guarded - a busy scene
  and a defect overlap on the span; the dense freeze separated them 14/15 against 0/12 every time.
- **span** (`tools/perf/flicker_diff.py <roll dir> --worst <jpg>`): 360 consecutive presents from the gate
  tape's DUMP MARK (iter-467 hooks, below; an exe without the mark: p20000-20359, or the re-placed window when
  that misses play)
  (`SR_PRESENT_DUMP=1,SR_PRESENT_DUMP_EVERY=1,SR_PRESENT_DUMP_FROM=20000,SR_PRESENT_DUMP_CAP=360,SR_PRESENT_DUMP_FROM_MARK=1`)
  scored for A-B-A outlier blocks (a present that differs from BOTH neighbours where they agree).
  `FLICKER PASS/FAIL: n of 358 presents` (fail at 2). Five clean dev exes scored 0/358, the S1
  regression 5-21/358 (archive/v1_native/batch87, HANDOFF_FLICKER). The span is placed by present
  NUMBER, so a config that runs at a different speed lands in a different scene: compare gates with
  the in-roll A/B (1.8d) or with the freeze, never span against span across configs. The metric's
  domain is CONTINUOUS gameplay motion: the title fly-through (a fast rotating camera) and the movies
  change every pixel between presents at the dump-slowed capture rate and score 11-16/358 with the
  regression fixed and with S1 off alike (span_menu / span_fmv vs their _off controls) - for those
  eras read ON against an OFF control on the same span, never against zero; the freeze is the exact
  oracle everywhere the sim can be held. `--motion 0.25` (default): a present whose two neighbours
  disagree on more than a quarter of the frame is a whole-frame camera / cut event, listed as
  `motion` and not counted - the defect class leaves the neighbours agreeing (the dev gate's run
  landed in a sidewalk sprint ending in the pause menu: 3 such events, 0 defects).
- **freeze** (`tools/perf/freeze_diff.py <run.log> <frames> --out <dir> --exact --allow-maxdiff 1`):
  `SR_DIAG_SIM_FREEZE=8:2000:80,SR_PRESENT_DUMP_FREEZE=2,SR_PRESENT_DUMP_FREEZE_CAP=40` holds the sim
  2 s every 8 s and dumps every present from 400 ms into each freeze (up to 40). The frozen game
  draws the same picture every pass, so consecutive presents must be pixel-identical. Noise floor
  measured with the regression off (fz4, mit2): maxdiff 1 on <= 39 px, 9 of 12 gameplay freezes
  exactly identical over all 39 pairs; the regression: maxdiff up to 245 on thousands of px in every
  gameplay freeze (fz3, s1on). Freezes that begin before the ingame era are movie frames on the media
  clock and differ legitimately; flicker_gate excludes them through perf_report's era markers.
  The post-cutscene LOADING screen is the same class and lies AFTER perf_report's ingame marker
  (the movie's end): its row of dots animates on the render clock while the sim is held, so a
  freeze landing there differs in every pair (native-a roll p1b_gate freeze 4: 1,902 px at maxdiff
  45 in the strip 531,641-749,661; crop archive/v1_native/p1b_gate_freeze4_crop.png). Since
  iter-467 native-a the gate's freeze era starts at the first `[PC_CONTROLS] context gameplay` line
  after the ingame marker (within 30 s of it), i.e. when the player has control.
  Sheets `freeze_exact_NNN.png` (prev | cur | changed px in red) name the object.
- **Attribution (`SR_DRAW_LEDGER=1`, `gfx/draw_ledger.cpp`):** with the freeze on, the CP records every
  draw of a present (args, shaders, bin predication, the S1 record it came from, a hash of the
  register subset a draw consumes with vertex-fetch addresses masked, and a hash of the vertex data
  the bound slots point at - dynamic vertex buffers move every present) and compares each frozen
  present with the previous one at XE_SWAP: `[DRAW_LEDGER]` names a missing draw or the candidate
  draw and its differing registers / data. 12 presents in full per freeze. It has a BASELINE: ~15-23
  candidate draws per frozen gameplay present even with the defect absent (registers 0x4194-0x419F,
  0x4594-0x459F, 0x4030, 0x444C change every pass without changing pixels), so read it as the
  difference between a defect roll and a control roll of the same instrument (the S1 regression added
  88 registers - the VS transform group 0x4010-0x401B and PS 0x4404-0x444E - that the control never
  showed), not against zero.
- **Windowed ledger (`SR_DRAW_LEDGER_WINDOW=<seconds>`, iter-467 forcing, diagnostic, default OFF):** no freeze
  needed. It opens at the first CP swap after the harness DUMP MARK (`mark dump`, the mark
  `SR_PRESENT_DUMP_FROM_MARK` uses) and, for that long, logs one `[DRAW_LEDGER_W] swap=` line per present: the
  constant file 0x4000-0x497F aggregated over the selected draws (`SR_DRAW_LEDGER_WINDOW_PS=<hex>[+<hex>]`, default
  every draw; '*' = not uniform across them), the values of `SR_DRAW_LEDGER_WINDOW_REGS=<hexreg>:<dwords>[+...]`
  at the first selected draw, and an `ABA` line for registers whose middle-present value differs from two agreeing
  neighbours. Present p of the every-present dump is CP swap p+1 (both open at the same mark). The colour-flash
  roll (archive/v1_forcing/ledger13b.env, ledger_check.py) found the world-material light rig written guest-side.
- **The world must be drawn** (iter-467 ledger; `tools/perf/world_floor.py`, the one rule the gate and the
  ledger share). A black 3D scene under a working HUD is perfectly stable, so an exact freeze and a quiet span
  both pass it, and so does the pause menu over a black world (its panel alone reads luma ~17). Eight archived
  rolls cited as passes had graded black frames. So:
  - **freeze:** at least `--min-world` (8) GRADED freezes must show the world: the middle dumped present clears the
    scene floor (central luma >= 8, near-black <= 0.6) - or, under it, has a drawn scene's STRUCTURE (2026-10-16,
    tools/world-floor: the edge share of the same central region, pixels whose luma differs from the right or
    lower neighbour by >= 2 on the 160x90 downscale, reaches 0.08 - the black worlds' tails read 0.000-0.043, the
    dark drawn corridor 0.131-0.303, lit scenes >= 0.22; a frame whose BMP is gone and whose sidecar predates the
    measurement is never rescued) - and either the player context there (`[PC_CONTROLS] context`) is `gameplay`,
    or - the pause menu - the side bands its panel leaves uncovered show the world (luma >= 5, near-black <= 0.9:
    the dimmed world reads 10-15 there, a black world 0-2). Else `WORLD NOT DRAWN` = FAIL. The structure test
    exists because the luma floor alone read a DRAWN dark scene as no world - the opening corridor the cinematic
    leaves Superman in (s3m_gate 2026-10-16: world 6 of 15 on a merged release exe whose three predecessors had
    passed 15 / 15; qdl1 and s3check3's dev gate: dark [3, 8]; scm2 / scm2b 2026-10-13: world 4-5, "SCENE-CONFOUNDED")
    - while a black world (the startup race, job65's w65_3 / w65_5: their tails' edge share 0.000-0.043) and the
    pause menu over black still fail: `tools/perf/world_floor_validate.py` grades every archived witness before /
    after (a roll whose BMPs are gone and whose sidecar predates the measurement grades exactly as before),
    `tools/perf/test_world_floor_structure.py` the synthetic cases. The world rule's docstring in `world_floor.py`
    has the measurement; the sidecar's `rule` header names its gradient level, and a stored structure measured
    under another level is never served.
  - **The package must be the exe's** (roll health, 2026-10-16): a `[NB_AOT] compiled <kernel> - no shipped blob
    <file>` line in the roll's log means the staged `data/shader_cache/nb` does not hold this exe's resolve-kernel
    keys and the exe compiled them at boot - in the release configuration inline on the CP's first command, which
    on a slower machine is the H3 watchdog (s3m_gate2 on the Ally: two compiles, `[GPU_HANG] H3` at 15.79 s with
    the CP 5.1 s into its first IB, two more; the Ally's checkout 054a577a predated the exe's blobs). The gate
    FAILS such a roll as `ROLL FAIL: PACKAGE: n resolve kernel(s) compiled at boot ...`, names the kernels, and
    names a GPU_HANG whose trigger falls inside the compile window (`review_kernel_compiles`,
    `grade_roll_health`; test_flicker_gate `kernel_compile_health`). Exes before render/shader-coverage
    (2026-10-17) look for the blobs in the working directory's `data/shader_cache/nb`, else beside the exe: a gate
    run from a checkout stages that checkout's blobs, so the checkout must carry the exe's set
    (release/emu-s3-blobs lists it). Since then the exe looks beside itself FIRST and in the working directory only
    when it has no set (`ShaderCache::shippedDataDir`; the same order for `data/shader_cache/dxil` and the
    point-sprite GS; `SR_NB_AOT_DIR` / `SR_SHADER_CACHE_DIR` override; `SR_DATA_CWD_FIRST=1` = the old order;
    `SR_NB_AOT_EMIT=1` keeps the old order so the checkout's set is what gets regenerated).
  - **span:** at least `--span-in-play` (0.8) of its presents must be IN PLAY - dumped at or after the run's own
    in-game era start (the freeze era's start) in the gameplay context - and its median central region must clear
    the scene floor.
  - **The span is placed IN THE ROLL (iter-467 hooks).** Both gate rolls play `tools/perf/flicker_play.txt`: the
    player tape's actions with NO pause (the player tape's pause round trip put the menu over the world: its 150 ms
    resume tap landed inside a 2 s sim freeze, never reached the game, and the menu stayed up - "menu over world 10"
    in postmerge_sj / postmerge_hooks gate2 / flicker_stale / flicker_wave2; PC_CONTROLS.md 8.1), started once
    Superman is on foot after the opening movie, with `mark dump` there: the harness channel's DUMP MARK (flags bit 2)
    opens the window at the next present (`SR_PRESENT_DUMP_FROM_MARK=1`; the log says `[PRESENT_DUMP] window opened
    by the harness dump mark at present N`). A present NUMBER is not a place in the game: over the 64 archived gates
    the HUD came at p9,767-p21,251, so a count taken from one roll lands anywhere in the next - postmerge_sj's span,
    re-placed at p21,391 (40 s into play in its first roll), opened 1.3 s after the HUD request of the re-roll, on
    the opening movie (luma 7.3). The earlier offline check ("a window opened 40 s in is fully in play in all 33")
    measured each window on the roll it was computed from, which is not what the re-roll does. A mark-placed span
    that misses play is a finding and is not re-rolled. An exe without the mark keeps the old path: from p20000, and
    when that misses play, RE-PLACED once `--replace-settle` (40) s after that roll's in-game start, moved past any
    pause / menu stretch its log shows, and rolled again (the miss kept as `span_try1/`; `span_attempts` in the
    JSON) - a cross-boot guess, and the attempt record says so. `--no-replace` turns it off.
  - **The in-game start is the opening movie's END OF MEDIA** (`perf_report.INGAME_MOVIE`, EOM only, iter-467
    hooks). A stall in the FMV splits it into a `GAP` record (the movie pace logger's 2 s rule) and the `EOM` record
    of its rest; the GAP opened the in-game era on the movie, where the context already reads `gameplay` (the HUD
    is requested before the FMV), and the span counted movie presents as play: postmerge_hooks gate2's span PASSED
    on 360 presents of the opening FMV (luma 108). Every opening FMV in the 64 archived gates ends EOM. Re-grade of
    all 64 (read-only, old rule vs EOM rule; `archive/d3/pmfix/regrade_table.md` in the hooks worktree): 61
    identical in every verdict and number; postmerge_hooks gate2 PASS -> FAIL (span in play 360 -> 0: the movie);
    postmerge_sj and fg_discrestore stay FAIL (span in play 98 -> 0, 70 -> 0); nothing moved to PASS.
  - **A tape that dies FAILS the gate:** harness_play exits non-zero when an `await` times out or a `hold_ctx`
    never sees its context change; perf_run writes `<roll>/play_failed.txt` and prints `HARNESS_PLAY FAILED`.
  - `--grade <dir> --reuse` re-grades an archived gate from its stored freeze_diff / flicker_diff outputs (the
    pre-world verdict is kept as `flicker_gate.pre_world.json`). Re-grade of every archived gate (2026-10-09,
    35 dirs; `archive/v1_ledger/regrade_world.md`): 6 verdicts changed - p2d_gate, fg54 (black freeze rolls),
    fg67 (span on the title), p2vpk and p2vpk3 (the vpk splice bake's gate: black spans, luma 0.1 / 0.3), fg49
    (a crashed freeze roll: NO DATA -> FAIL); every other healthy gate, the phase-0 gate fg_phase0 included, PASSes.
  - **Frames are scratch; their measurements are kept** (2026-10-14). Raw dumps go to the J: frame scratch
    (`tools/sr_artifacts.ps1`: `<attempt>\frames` is a junction; the GC deletes a folder 48 h after its roll, sooner
    over 200 GB). Before it deletes one, the GC measures every BMP with `python tools/perf/world_floor.py sidecar
    <frames>` into `<attempt>\frames_measured.json` - each frame's center and sides measurement with its size and
    mtime, a provenance header, never a verdict - and KEEPS the folder (a warning, retried next roll) unless its
    own enumeration of the folder succeeds, the measurer exits 0 reporting exactly that many BMPs, and the written
    sidecar reads back as exactly this folder (`world_floor.py covers --exact`: it applies, and holds every file
    there as THAT version - same name, size and mtime_ns - and nothing else; an unreadable folder is never an empty
    one: Astra 2026-10-14). frames_deleted.txt then records the byte total of that check.
    THE INVENTORY IS FIXED AT WRITE TIME AND CHECKED AGAINST THE DELETION RECORD (Astra round 4; schema
    `sr-frames-measured/2`, format in `world_floor.py`'s docstring): the sidecar header lists every file the folder
    held (each BMP with size and mtime_ns, other files with their sizes, the byte total; for a recovery,
    outcome.json's frame list) and, off disk, every frame run.log / outcome.json name that was not there. The
    reader rejects the sidecar - its frames grade `unmeasured` - unless (a) that total equals the bytes
    frames_deleted.txt records (exactly one note), (b) every frame run.log `[PRESENT_DUMP]`, outcome.json or the
    frames folder names is in it with no two claiming one present, and every source agrees on each frame's
    version (outcome.json's byte count; a BMP on disk needs the recorded size AND mtime_ns; an unreadable
    outcome.json agrees with nothing). Grading always uses the fixed inventory as the expected set, so a truncated
    log or a lost outcome.json cannot shrink a span sample, a freeze's middle frame or a meter / span denominator
    (an inventory frame no log line names counts as unmeasured / not in play). An ARCHIVED roll (frames_deleted.txt
    or a sidecar exists) is graded from its applicable sidecar only: a rejected sidecar, or a deletion record with no
    sidecar, makes EVERY frame of the roll unmeasured - never the BMPs that happen to be on disk (Astra round 5).
    Archival is decided by the marker FILES existing (`marker_exists`), never by what they parse to: a
    frames_measured.json holding null, true, 0, "", a list or the wrong schema is a sidecar that does not apply
    (round 6); outcome.json is parsed strictly the same way (a falsy "frames" value is unreadable, not "no frames").
    Every share, span sample and freeze middle has ONE ROW PER PRESENT of the inventory (`present_index`), never one
    per log line: a present two frames claim, or whose frame the log names twice, is ambiguous and unmeasured, and a
    freeze whose `[SIM_FREEZE_DUMP]` lines repeat a present has no middle. The GC itself fails closed (`$ErrorActionPreference` Stop, any error keeps the folder): it
    deletes only when the folder's `.source.txt` reads cleanly and names an attempt frames path that is a junction
    here (measured first), a junction elsewhere (re-allocated: superseded), or a plain folder beside a sidecar that
    covers these frames; a missing / empty / unreadable back-reference, a gone (deleted or MOVED) attempt, or a
    plain folder without covering measurements is KEPT with a warning - clean those by hand. The world rule reads
    the sidecar where a BMP is absent, over the EXPECTED inventory (the fixed inventory; with no sidecar, the BMPs +
    every frame the run.log `[PRESENT_DUMP]` lines and outcome.json name), so missing evidence stays missing: a frame with no stored
    measurement grades `unmeasured`, never a class; a freeze's middle logged dump is never replaced by a readable
    neighbour; a meter's logged dumps all stay in its denominator; a span samples the same 40 names and is
    `scene UNMEASURED` (FAIL) when any has no measurement, when its frames are gone, or when a BMP cannot be decoded
    because Pillow is missing (stored measurements need no Pillow) - before, each of these skipped the floor or
    graded a substitute. A measurement must be a finite pair in 0..255 / 0..1 (else the sidecar is rejected, a
    cached value re-measured); the cache records its `MEASURE_VERSION` and one of another version is never served
    nor overwritten. A sidecar written for another run.log (the folder re-run), frames folder or `MEASURE_VERSION`
    is ignored. Frames
    deleted before this: `tools/perf/recover_frame_measurements.py` rebuilds the sidecar from the measurement cache,
    taking an entry only as proof of THAT file version: the frame's exact path (or the path its `[PRESENT_DUMP]`
    line / frames_deleted.txt names), its size, a file the run logged exactly once (a second line = a rewrite the
    cache may not have seen), an mtime within 50 ms of that line (the recovered ones sit at -2.0..+0.9 ms; median
    over 256,449 BMPs on disk 0.0 ms), and one file version over center and sides together (a second candidate
    version of either -> neither is used); anything else stays unrecovered, with the reason.
    Tests: `python tools/perf/test_frame_measurements.py`; the GC's: `tools/test_sr_drive_artifacts.ps1` section S.
    Recovery run 2026-10-14 over `_worktrees/native/archive` + `_worktrees/dev/archive`: 67 attempts with
    frames_deleted.txt, 27 of them never dumped a frame (0 bytes deleted: no sidecar); 40 sidecars, 18,572 frames
    inventoried, 1,139 centers + 53 sides recovered - every frame the world rule reads in those rolls (each graded
    freeze's middle frame, each span's 40-frame scene sample, each meter's in-play sparse frames) except the two
    uncited `cr1_meter0` rolls (never graded, so never cached). 29 ledger items changed: 26 FAIL -> PASS, the
    `evidence_deleted.callback_gate_void` witness no longer fails (its roll grades world 14 now), and
    `gate_full_live_gate` (pixels: freeze 1) and `swap_cppm4` (world 5 of 15, bad [1, 6, 7]) still FAIL on their
    own recorded reasons; p1.row3, p1.rows12, p1.rows467 close. The recovered grades match the counts the ledger
    notes quote from when the BMPs existed (e.g. cr1_cppm4 "gameplay 12, menu over world 2, dark 1"). Re-run with
    the tightened identity rule (50 ms, logged once, one version over center + sides) and the fail-closed
    expected-inventory rule (Astra rounds 2-3): the same 1,139 + 53 measurements, the same 29 items, ledger digest
    unchanged (9b179fb3a1efb89b). The same archives graded by the current code with the sidecars hidden keep every
    status of the original before-run, with the stricter reasons (the deleted spans now FAIL `scene UNMEASURED`
    instead of passing without a floor; the cr1_meter1 meters read 0 of 72 / 0 of 70 instead of 0 of 0).
    Round 4 (the fixed inventory): the 40 sidecars rewritten as `sr-frames-measured/2` all reconcile with their
    frames_deleted.txt (byte totals equal, RUN_CLASS.json as the 123-byte residual in 9 of them), the same 1,139 + 53
    measurements, the same 29 items and statuses; two meters' denominators gain the one frame each whose
    `[PRESENT_DUMP]` line the killed run never flushed (cr1_meter1 66 of 71, r9m_meter0 38 of 42: still PASS), ledger
    digest 595cffc84e5e3c21. A probe of every route that could shrink or swap a real sidecar's inventory (a cut /
    forged log, outcome.json hidden / unreadable / altered, the deletion record removed / altered, a same-named BMP of
    another version back on disk, edits to the sidecar that keep its header consistent) is rejected or leaves the
    inventory intact; what stays out of reach is a forger who rewrites the sidecar itself (in-range values; an
    unlogged frame dropped while outcome.json is deleted too) - the sidecar is unsigned, like the BMPs it replaces.
    Round 5 (archived rolls, one row per present): Astra's r3e_gate/span probe (a one-dump log, outcome.json hidden,
    one bright BMP on disk) now FAILS `scene UNMEASURED` 40 of 40, in play 1 of 360 (it passed 1 of 1); a 100x
    repeated `[PRESENT_DUMP]` line leaves one unmeasured row. The ledger and the recovery are unchanged (digest
    595cffc84e5e3c21): no ledger-cited roll logs a frame twice, gives a present two names, repeats a freeze dump or
    holds BMPs once archived. Round 6 (a sidecar holding JSON null read as "no sidecar", so the BMPs beside it
    graded): the same probe with the sidecar read as null / true / 0 / "" / [] / {} FAILS each time; ledger and
    recovery unchanged.
  - `python tools/perf/test_flicker_gate.py`: synthetic fixtures (black world, pause over black, pause over world,
    healthy, span on the title, the re-placement, `main()` end to end with perf_run mocked; iter-467 hooks: the
    split opening movie (the in-game start at the EOM, a span on the movie not in play), the mark-placed span (one
    roll, PASS; missed play FAILS without a re-roll), a dead tape FAILS, the old-exe re-placement past a pause) and
    the archived fixtures p1d_abp / p3a_abp (FAIL), p1a_gate2 (PASS), fg67's span (FAIL, re-placed into play),
    postmerge_sj's and postmerge_hooks gate2's spans (on the split movie: not in play).
- Every merge candidate that touches rendering, timing or caching passes the gate (both parts) before
  it merges.

### 1.8g The P2 trace: the fence / ring / answer baseline of the XDK ring path [iter-467 native-a]

`SR_P2_TRACE=<file>` (`=1` -> `archive/p2_trace_<pid>.bin`), default OFF, observe-only
(`kernel/p2_trace.cpp`; NATIVE_ROADMAP "P2.7 (d)" item 1). Every event of the ring protocol becomes a
40-byte record `{qpc, present, thread, kind, aux, S = dev+0x2A1C, C = fb+0, F, a, b}`: closes
(820FD580: the S written, the cursor), the IB descriptors each doorbell publishes (820FD218) and the
pump's bin selects (82110140), the answers the guest gets (IsFencePending 820FDD90, BlockOnFence
820FD9A8 begin / end with the reason, QueryGetData 82102DC8, the in-place patch decision 82108520,
InsertFence 820FDD30, the ring waits 820FCB30 / 820FD150, the kick 820FD8C0 with the cursor), the
consumer's writes (fb+0 COMPLETE, fb+4 PROGRESS, the scratch write-backs pc+4n), the ISR calls and the
presents (dev+0x34D0 throttle, fb+0x3C read index). One host thread writes the file every 200 ms; a
`[P2_TRACE]` census line every 10 s. The table entries record through `XdkScope`'s hook table
(`sr::native::setScopeHook`, one bool test per shimmed call when off); 820FD580 / 82110140 have their
own observe overrides.

`python tools/perf/p2_trace.py <bin> [--presents A-B] [--ab] [--compare other.bin] [--json out]`:
per-present counts per kind, the P2.7 invariants (S odd and +2 per close, C monotone and <= S, every
COMPLETE a closed S in order, IsFencePending answers against `(S-F) < (S-C)` with a "racy" bucket when
C passed F between the call and the read), waits by reason (p50 / p99 / max), BS values, IB sizes,
scratch registers, ISR masks, present pacing. `--ab` compares consecutive presents' `{IB dwords, BS
values, closes}` signatures (the P2-A identity under the dense sim freeze with
`SR_NATIVE_SEGMENTLESS=0,SR_AB_TOGGLE=SR_NATIVE_RINGLESS,SR_AB_PRESENT=1` - since the 2026-10-13 segmentless
re-bake the ringless toggle is ignored, with an error line, unless segmentless is off); `--compare` does it present by present between
two files. Baseline roll: `archive/v1_native/p2a_trace*` (p2_batch1.sh).

### 1.8h The row-9 dry CHECK: a native port against the guest body, per call [iter-467 native-b]

`SR_NB_REC_CHECK=1` (diagnostic, default OFF; `kernel/native_rec_b.cpp`). Per call of Resolve 8210C5F8, ClearF
82101A58 and SetShaderGPRAllocation 820F5570 (+ ClearFullSurface), the native port runs on a COPY of the device
first. It writes no guest memory: its writes are recorded, and the ring bytes it would write are rendered. Then
the guest body runs, and the CHECK compares four things:
- the ring LEN from the entry's cursor;
- the ring BYTES;
- every DEVICE dword except the async fence / pump block (`dev+0x2A18..0x2BB3` minus 0x2A38, plus 0x2C50);
- the recorded guest writes (MEM).
A port stop (a path not modelled, a kick site that would kick) makes the call a skip, counted per cause.
- `[NB_REC] check <entry>: calls compared ok bad: len bytes device mem guest-kick | stopped=N:<cause=count...>`
  every 16,384 calls (whitelisted in the player log profile). `[NB_REC_BAD]` examples (first 48) give the
  offset and the native / guest values. A DEVICE BAD adds a detail line with the first 4 differing dwords.
- `python tools/perf/nb_check_eval.py <run.log> [--gate]` gives the last line per entry and a
  `NB_CHECK_VERDICT PASS / FAIL`. The batch scripts stop before the record-mode rolls on FAIL.
- ⛔ **TRAP (job45-47): never find "what the port wrote" by diffing two copies of racing memory.** The first
  CHECK took `before` from the live device, then `copy` from `before`, and diffed them for the async fields the
  port wrote. LLVM's memcpy-to-memcpy forwarding rewrites `memcpy(copy, before)` into `memcpy(copy, live)`, a
  second read that races the pump / ISR writers. The result was ~0.1% "port-written" async fields the ports never
  touch: before 0 / native 0x40000000 at +0x2A64, the pump's replay state. Track writes at the write
  (`Prod::asyncWrites`), or exclude the block unconditionally as strand A's flush CHECK does.

### 1.8i The native obligation ledger: computed status, never written [iter-467 ledger]

`docs/native_ledger.json` holds one record per native obligation (each P1 row, the S1 flush, the P2 transport
items, device creation, the display registers, every strand-B stage and EDRAM zero, XMA, the black-world fix and
release criterion C1). A record cites evidence; `tools/native_ledger.py` COMPUTES its status from that evidence.
Nobody writes a status: a `status` / `closed` key in a record is a schema error.

    python tools/native_ledger.py                       # the table; exit 2 on schema errors
    python tools/native_ledger.py -v --id p1.row3       # every evidence item's result, the contradictions
    python tools/native_ledger.py --strict              # exit 1 on a docs-vs-evidence mismatch (below)
    python tools/native_ledger.py --write-status docs/NATIVE_ROADMAP.md   # regenerate a doc's status block
    python tools/native_ledger.py --markdown readme     # print the README's native-status section
    python tools/native_ledger.py sheet <freeze roll> <out.jpg>   # LOOK at what a freeze grade passed: one frame
                                                        # per graded freeze, labelled gameplay / menu_world / dark /
                                                        # menu_black

- **Closed** = every evidence item passes; for every guard (`{observes, min_authority}`) a passing item observes
  that thing at that authority or higher; no residual unknown or contradiction is open; every `depends_on` record
  is closed. Otherwise **blocked** (a dependency is not closed), **in progress** (some evidence passes) or **open**.
  Authority, low to high: static (source, a commit on dev) < generation (dry CHECK, L6) < replay (Xenia traces; no
  guest code runs) < live-observe (counters, meter windows, a span's floor) < live-exact (CPPM4 + dedup CHECK, VERIFY
  modes, the dense freeze) < frozen-ab (the per-present A/B against the guest / PM4 path) < xenia. A draw-path row
  needs `pixels >= frozen-ab`: a packed-word or dry CHECK proves generation, never frames.
- **Fail-closed.** A missing or unparsable item fails; an archive path absent on this machine is `unavailable`,
  which never closes anything. Evidence paths use the `umbrella` root (`<umbrella folder>`):
  dev's `archive\` and the lane worktrees' archives.
- **Kinds:** `source` (a regex in a tracked file: default-ON gates, doc corrections), `git_ancestor` (a commit on
  HEAD), `text` (select lines, match a pattern: any / all / last / min_count), `json` (field checks),
  `replay_cmp` (`N/M byte-identical`), `check_report` (check_roll's JSON; named entries PASS, scale-only
  NO-EVIDENCE allowed by pattern), `freeze_grade` (a stored `<roll>_freeze.txt` graded by the flicker gate's own
  `grade_freeze`, read-only: pixels + the world rule; `ab_present_min` demands the toggle witness), `flicker_gate`
  (the gate's own `grade()` on the stored diff outputs, read-only - never the verdict an older gate JSON recorded),
  `meter` / `meter_delta` (meter_delta.py's ingame windows; bounds or a required drop).
- **The world must be drawn** - the flicker gate's world rule (TOOLKIT 1.8f, `tools/perf/world_floor.py`), applied
  by calling the gate itself, so a ledger status and a gate verdict cannot disagree: 8 graded freezes showing the
  world (`min_lit`; the pause menu counts only with the world around its panel - p3a_abp: 10 of 12 freezes were the
  menu over black at luma 17), a gate's span 80% in play (fg67's span on the title: 0 of 360). A meter item with
  `world_min` needs that share of its ingame sparse frames (from the in-game era start) showing the world
  (p3a_meter1: 0 of 46). Frame measurements are cached in `%LOCALAPPDATA%\sr_native_ledger_frames.json` (keyed by
  path, size and mtime; `SR_WORLD_FLOOR_CACHE=0` off). The `scene_floor.json` sidecars written on 2026-10-09 for the
  pre-floor gates are inert now: every gate is graded from its frames - or, once the frame-scratch GC deleted
  them, from the attempt's `frames_measured.json` (TOOLKIT 1.8f "Frames are scratch").
- **Docs vs evidence.** `claimed` is the strongest claim the docs make (`none` / `planned` / `built` /
  `in-progress` / `shipping-unproven` = default ON and the docs SAY the proof is incomplete / `done` / `baked`),
  and `claim_refs` quote the doc text that makes it. A `contradiction` names a doc claim the evidence contradicts:
  its `witness` items show it (`"expect": "fail"` inverts a read result: "this freeze grade fails the world floor"),
  and it stays open until its `resolved_by` items pass (normally a `source` check that the doc now says the truth).
  `--strict` fails when a record the docs call done / baked is not closed, a claim quote is no longer in its doc,
  or a `status_docs` block differs from what the ledger computes now.
- **Status blocks.** A doc carries `<!-- native-ledger:begin:roadmap -->` (or `:readme`) ... `<!-- native-ledger:end
  -->`; `--write-status` fills it (the totals, the meter from the ledger's `meter` section, one row per record).
  `--write-status` refuses on a machine where any evidence is unavailable. NATIVE_ROADMAP's top block is generated
  and checked this way; the README's native section (branch `release/docs`) takes the `readme` style.
- **Adding or closing an obligation:** add the record (or the new evidence: the re-run's roll dirs) to the JSON,
  run the tool, correct any doc the result contradicts, `--write-status docs/NATIVE_ROADMAP.md`, `--strict`, and
  commit them together. Bump a record's `version` when its meaning changes. Never delete failing evidence to close a
  record: supersede it with a passing re-run and keep the void one as a contradiction witness.

### 1.8j C1 oracle split receipts: preprocessed-token identity, fail-closed [release/oracle-mixed-a]

**Scope (the whole claim).** `tools/pp_receipts.py` exits 0 only when, for one ninja build dir, every C/C++ compile
unit that can read a file changed since `--base` yields the SAME preprocessed token stream (`-E -P`, the unit's own
compile command) with `--base`'s files as with this tree's. Code generation, link, packaging, the compile commands
themselves (both sides use the AFTER build's), a generator's undeclared inputs and a compiler hidden inside a script
are not compared. Everything outside that scope, or not established, is exit 2 (INCOMPLETE) - never 0. Exit 1 = some
unit's streams differ (DIFF) or its source is new. There is no exception class: a difference that looks harmless is
DIFF, with an advisory note, and is judged by review.

An `#if SR_ORACLE` split (kernel/sr_oracle.hpp, C1) should leave the default build (SR_ORACLE=1, dev and difftest)
unchanged and change the release build (SR_ORACLE=0) only by the removed oracle code. The tool is the project's one
receipt tool for that (any base ref, any ninja build dir); it runs on the cloud worker, in a synced tree whose build
dir is built, and never compiles an object:

    python tools/cloud/sr_cloud.py run --name <tree> -- python tools/pp_receipts.py --build-dir build     --base <ref> --out build/pp_default --assume-no-codegen "docs/*.md=documentation"
    python tools/cloud/sr_cloud.py run --name <tree> -- python tools/pp_receipts.py --build-dir build-rel --base <ref> --out build-rel/pp_rel --assume-no-codegen "docs/*.md=documentation"
    python tools/cloud/sr_cloud.py fetch --name <tree> build/pp_default/summary.tsv archive/<lane>/pp_default/
    python tools/pp_receipts.py --self-test [--cc <clang++>]   # the scope checks and the attacks tried on it; --cc adds the compiler-backed ones

- **Compile edges.** A compile-database entry whose command starts with a GNU-style C/C++ driver (clang, clang++,
  gcc, g++, cc, c++, optional target prefix / version suffix, the name taken after resolving the path so 8.3 names
  count; one ccache / sccache launcher skipped) and has `-c`. Each must have exactly one `-c` and one separate `-o`, a
  source ending `.c .cc .cpp .cxx`, `-x` only c / c++ / c-header / c++-header, no response file, no module flags, no
  long-form dependency option (`--write-user-dependencies`, ...), no forwarded or driver-level option and its own
  object. Forwarded means `-Wp,...`, `-Xpreprocessor`, any `-X...` other than the accepted `-Xclang` forms,
  `/clang:...`, `--config...`, `--driver-mode`, `-ccc-...`: such an option can change preprocessing or the dependency
  output behind the visible flags (`-MD -Wp,-MMD,b.d` omits system headers; Astra round 4). Accepted: `-Xclang`
  followed by `-emit-pch` or `--dependent-lib=...`, and `-Xclang -include` / `-include-pch -Xclang <path>` (the forms
  this build uses for its precompiled header). At most one, non-empty, `-include-pch` (clang uses the last of several:
  `-include-pch x.pch -include-pch ""` loads none). No option that shapes the `-E` output (`-P`, `-C`, `-CC`, `-dM` /
  `-dD` / `-dI` / `-dN` / `-dU` / `-dE`, `-fdirectives-only`, `-frewrite-includes` / `-imports`): the token pass and
  the line-marker witness below would not show the unit. Exit 2, never skipped: any other compile edge, an MSVC-style
  compiler (cl, clang-cl), a C/C++ source built without `-c`, a C/C++ source built by a command that does not start
  with a compiler (a `cmd /C`
  wrapper, a script). Commands are split as the platform does: on Windows by the C runtime's rules (`\"` is a literal
  quote; `""` inside quotes, on which runtimes disagree, is refused), elsewhere by POSIX sh quoting with every shell
  metacharacter refused.
- **The change.** `git diff --no-renames -z <base>` of tracked files, git run with fsmonitor off. Exit 2: an
  untracked, not ignored file; an assume-unchanged / skip-worktree entry; a submodule or symlink entry; two changed
  paths that differ only in case; a run outside the top level; a changed file that differs from HEAD (an interrupted
  run). Every changed file must be read by a unit or be named by `--assume-no-codegen 'PATTERN=REASON'` (the reason
  lands in the summary; the flag on a file a unit reads is exit 2). A changed file that a non-compile build edge
  consumes - a declared input in build.ninja (followed through phony aliases), or a file in that edge's own deps
  record (the resource compiler's, for one) - is exit 2 even when units read it, flag or not; so is a changed file
  inside a precompiled header a unit uses. Ignored files (generated inputs such as `guest/ppc`) are the same on both
  sides.
- **Evidence.** A unit's read set is its `ninja -t deps` record only when the record is VALID, complete, current
  (nothing newer than the object), names the source, was made with `-MD` (a `-MMD` / `-MM` record, or a command
  whose deps mode does not show, omits system headers), and clang's own `-###` for the unit's command shows the
  effective mode: its one cc1 job, read positionally, carries the driver's dependency group `-dependency-file F -MT T
  [-MT T ...] -sys-header-deps`. F and the targets are consumed as values, so an argument value cannot pass for the
  flag (`-I -sys-header-deps`, `-MT -sys-header-deps`: Astra round 5). A dependency token anywhere else in the job,
  a clang configuration file in use (a `Configuration file:` line - clang loads `clang.cfg` / `<triple>-clang++.cfg`
  on its own, or `--config`), a job whose precompiled header is not exactly the command's (several, an empty one, or
  one the driver adds for an `-include x.h` next to an `x.h.pch`), or a job line that cannot be parsed is exit 2; a
  group without the flag sends the unit to `-M`; another driver's records are never trusted. Otherwise the
  compiler's full `-M`, which must name the source (on clang, its own `-M` command must classify as full the same
  way). No read set for any unit is exit 2.
- **The read-set cross-check.** The AFTER preprocessing writes its own dependency list (`-MD`); a unit whose list
  differs from its build's deps record is FAILED (the receipt's environment resolves includes differently from the
  build's) - with one explained exception, exactly the cause found in real use (SupermanReturns'
  native_const_dedup.cpp and pm4_native_record_adapter.cpp after a header change). clang's builtin `stddef.h` /
  `stdarg.h` probe the next header of their name on line 22 (`#if defined(__MVS__) &&
  __has_include_next(<stddef.h>)`) without entering it; a PCH's input files are the files it ENTERED, so the probed
  UCRT `stddef.h` / MSVC `stdarg.h` is in the PCH unit's record but not in a consumer's `-c` record, while `-E` runs
  the PCH's header again and records the probe. A file is excused only if ALL of these hold:
  1. the unit's precompiled header's trusted build record (current, full `-MD`) lists it;
  2. no line marker of the unit's plain `-E` pass names it (`<name>.markers.txt` in `after/`; clang writes one on
     entering every file, an empty one too; the pass drops every output-shaping option from its own command and
     counts only when its markers name the unit's own source - zero markers are no answer);
  3. it lies under a system include directory of the unit's effective cc1 job (`-internal-isystem` / `-isystem` /
     `-internal-externc-isystem` as `-###` reports them);
  4. a header the unit DID enter, under the cc1 job's `-resource-dir` (a clang builtin header), contains
     `__has_include_next(<NAME>)` where NAME is the file's include name (its path under that system directory).
  Excused files are named in the `read_set` column; everything else is FAILED - a file the unit enters, a user
  header's `__has_include` probe, a file in neither record, a recorded file the preprocessing did not read, a PCH
  whose read set came from `-M`, no answer from the marker pass (Astra, receipts-fix2 rounds 1-3). Every unit's
  marker pass runs with clang's cc1 `-dE` (`-Xclang -dE` - the driver's `-dE` is dropped silently): clang then prints
  each `#embed` as a canonical `#embed` line instead of expanding it, so its own lexer decides and no spelling hides
  (`#/**/embed`, `/**/ #embed`, `%:/**/embed`, vertical tab, form feed: Astra, receipts-fix2 round 4). An embedded
  input is read without a line marker, so a unit with any such line is FAILED, and so is one without a `-dE` answer
  (a driver that cannot show embeds - probed once per driver - or an unreadable output). A text scan of the entered
  files for `#embed` is kept as an extra check only. (A changed file in the PCH's read set, probed ones included, is
  exit 2 before any comparison.)
- **Environment and new / deleted files.** Driver environment variables (CPATH, C/CPLUS_INCLUDE_PATH, INCLUDE,
  CCC_OVERRIDE_OPTIONS, CL, CLANG_NO_DEFAULT_CONFIG, ...) set for the tool are exit 2. A NEW file is moved aside for
  BEFORE; a DELETED one is written back and its readers come from every unit's BEFORE-side `-M`; with either, each
  compiler must list a file found only by `__has_include` in its `-M` output (probed; else exit 2).
- **Tokens.** String / char literals (u8 / u / U / L, raw `R"d(...)d"` with any legal delimiter, exact contents) with
  their user-defined suffixes are single tokens; digraphs are tokens (`<::` as the standard splits it); whitespace
  outside literals is not a token; a `#pragma` line keeps its end of line. Exit 2 (FAILED): a non-ASCII character
  outside a literal, a line marker, a directive other than `#pragma` (an `#embed` left by `-E` would hide the file's
  bytes), a literal the lexer does not recognise. Undecodable bytes stay distinct inside literals.
- **No `__LINE__` exception.** The generated `PPC_STORE_*` macros pass `__FILE__, __LINE__` to the store-watch hooks
  (inside a constant-false `if`), so an added `#if` line above them changes integer literals in the stream. That is
  DIFF: the note says "N token(s) differ, all of them integer literals; DIFF - judge by review", and the reviewer reads
  `diff_<name>.txt`. Values preprocessing does not fix (`__builtin_LINE`) are not compared; `__DATE__` / `__TIME__` /
  `__TIMESTAMP__` make a unit DIFF, never EQUAL.
- **Outputs.** `summary.tsv`: `#` lines (base, build dir, every changed file with kind and readers or reason,
  coverage counts), then per unit tu, object, tokens before / after, result (EQUAL / DIFF / NEW / FAILED), read set
  (ninja deps cross-checked, or compiler `-M` and why), note. `diff_<name>.txt` per DIFF unit (a display: blank lines
  dropped); `before/`, `after/` hold `<name>.i.txt` and its `.d`. `<name>` = the object's file name + a hash of its
  path, checked for collisions (exit 2).
- **Self-test.** `--self-test` runs the lexer, command splitting, classification and coverage checks plus every attack
  tried on the tool (odd commands, response files, wrapped compiles, forced includes / PCH, duplicate objects, path
  spellings, BOM / non-ASCII / undecodable bytes, `#embed`, generator and phony consumers, resource-compiler deps,
  `-MMD` records, forwarded options such as `-MD -Wp,-MMD,b.d` and `-Xclang -dependency-file`, a visible `-MD`
  whose effective mode omits system headers, `-###` jobs where a value imitates `-sys-header-deps`, configuration
  files, environment variables, receipt-name collisions); `--cc` adds the
  compiler-backed ones (`__LINE__`, raw literals, macro spacing, `-E -MD` read sets, the `__has_include` probe, a
  define with `\"` inside quotes, clang `-###` showing full deps for `-MD` and `-M` but not for `-MMD` or
  `-MD -Wp,-MMD,b.d`, an implicit config modelled by prepended flags with `-I -sys-header-deps` /
  `-MT -sys-header-deps`, a real `--config` file).
- A shim that gives a gate a SECOND declaration (`#else constexpr bool g_x = false;`) is token-safe but not
  audit-safe: `scripts/audit_forcing.py` reads source text, takes the weaker candidate and can report a gated write as
  UNGATED. Keep one declaration per gate; A/B the forcing audit with the base files swapped in.

### 1.8k C1 removed-gate sweep: the other readers of a removed gate [release/oracle-backend]

When an `#if SR_ORACLE` split compiles an oracle, a CHECK or an A/B out of the release, its environment variable must
read as unset everywhere in the release, or a set value still steers the shipped binary. Astra's P2 on the C1 backend
split: the replay modes left `app/main.cpp`, but `kernel/host_automation.cpp` and `kernel/host_input.cpp` still read
`SR_GPU_TRACE_REPLAY` / `SR_HLE_TRACE_REPLAY`, and any value picked the background window policy. The receipts
(§1.8j) cannot see that - both readers were unchanged, token-EQUAL units. Two tools find such readers, both run on
the worker where the tree's default (`build`) and release (`build-rel`, SR_RELEASE_COMPOSITION=ON) configurations
are built:

    python tools/cloud/sr_cloud.py run --name <tree> -- python tools/player/gate_sweep.py --build build `
        --build-rel build-rel --uncovered-ok tools/player/gate_sweep_uncovered.txt `
        --json archive/<lane>/removed.json > archive/<lane>/gate_sweep.txt
    python tools/cloud/sr_cloud.py run --name <tree> -- python tools/player/exe_sweep.py --removed <removed.json> `
        --release build-rel/SupermanReturns.exe --default build/SupermanReturns.exe --allow <explained names>
    python tools/player/gate_sweep.py --advisory      # anywhere, no build: the lexer's quick pre-check, never the answer

- **Source (gate_sweep): clang's views, the authority.** Astra's review rounds 1-7 kept finding forms a lexer that
  emulates the preprocessor misreads (the last: an object-like macro supplying a call and its `(` - `#define CALL F(`
  + `#if CALL X)`), so the views are clang's own. The release is what the exe links (round 8: round 7's two hard-coded
  targets missed sr_imgui and xenos_xlat): in each build's own graph (build.ninja, read by composition_audit's
  Graph.closure) SupermanReturns.exe's link closure is its objects and every static library it links -
  SupermanReturnsGuest, sr_imgui, xenos_xlat. Every C / C++ unit of it must have its compile-database entry, for the
  same source - an empty or partial database, an included ninja file holding edges, or a closure without
  SupermanReturns or SupermanReturnsGuest units is exit 2 -, and a unit the default links and the release graph does
  not (the 25 ORACLE_SOURCES units) has an empty release view because the graph says so, never because an entry is
  missing. The link commands are read too, against a canonical-template allowlist (rounds 9-12, Astra: a response
  file's content, quoted or not, a LINK_LIBRARIES the rule or the file binds instead of the edge, a file-level
  assignment ninja freezes when it reads it, ninja's own `compdb -x` - an unbounded substring swap of `@rspfile`, so
  `@x.rsp.more` read x.rsp -, a response file's `&&` taken for a shell separator, a copy between two links: each
  dressed a link command so it could hand the linker an input the closure never reads, so round 12 ends the class
  rather than the instances). Each linker edge of the exe must have one of the command shapes the tree's four link
  edges use (`LINK_TEMPLATES`: CMake's clang++ executable rule through a response file, its llvm-ar rule with $in or a
  response file - its rule's text exactly a template's, the program paths aside), every value the template reads is
  ninja's own for that edge (`ninja -t compdb` on a scratch copy of the graph whose link rules' command records $in,
  $rspfile, $rspfile_content and each template variable), the template filled with them must be the command ninja
  gives the edge (`ninja -t compdb`, never -x), and `ninja -t query` must read the edge's rule and inputs as the
  closure did. $PRE_LINK must be `cd .`, $POST_BUILD `cd .` or - the exe's - CMake's `cmd.exe /C "..."` of `cd /D` and
  `cmake -E copy* / make_directory` of absolute paths writing no file a linker edge reads or writes, no other value on
  the command line may hold a character cmd.exe reads, the linker's one @file must be the edge's rspfile exactly
  (quoted or not), the response file is split only as linker arguments (MSVC's rules, blanks and newlines separating)
  with no shell operator and no nested @file, and every token a linker or archiver gets must be an input the edge
  lists, an output, a token of the edge's own link bindings (what the closure reads) or a known flag. Round 13 (Astra
  round 12) closes the file-name level under it: every name the link reads - its command, its response file, its
  edge's inputs and outputs - must be ASCII (a response file's leading BOM is one clang drops and the closure keeps,
  so the linker read an object the closure never did), no step or link argument may name a path through a Windows
  alias (`\\?\`, `\\.\`, `\??\`, UNC, an 8.3 name, an alternate data stream, a trailing dot or blank - a post-build
  copy to `\\?\<the exe>` overwrote it unseen), and a post-build copy's destination is compared with the linker edges'
  files by resolved identity (a junction, a hard link). Anything else - another rule shape, a step, a second link, an
  unexpected token, an edge ninja cannot answer for - is exit 2 until a template is added: soundness over generality,
  with exe_sweep the backstop. Each unit is preprocessed with its own compile command (`-E -Xclang -dE`, outputs
  stripped, as pp_receipts runs a unit; a driver -include made cc1's textual one; a PCH replaced by its text - the
  forced includes and source of the database entry that emits it, a prebuilt .pch being exit 2 -, and that text must
  read in the unit as it read in the PCH: round 9 preprocesses it with -E -P -dD under the producer's command (its
  source the main file) and as the substitution runs it (the unit's command, that source force-included), and the two
  must print the same text (compared exactly, or as tokens read with the -E reader when only blanks differ - round 11:
  a line-marker regex had deleted a raw string's `# 1 ...` line), so a producer -D the unit lacks (Astra's -DSWITCH=1,
  which a PCH carries into the unit), an include path that resolves a header elsewhere or text that depends on its
  place (round 10, Astra: __INCLUDE_LEVEL__, 0 in the producer and 1 force-included) is exit 2), once from `build`
  (SR_ORACLE=1) and once from `build-rel` (SR_ORACLE=0); the default database's other units under the source roots
  (tools/difftest, ...) are preprocessed in the default view (NOT-REL). A name is any SR_* run in a literal's VALUE in
  clang's output (round 8: `"SR_" "LIVE"`, `"SR_" #x` and `"SR_\x4cIVE"` are all SR_LIVE): adjacent literals
  concatenated across line markers and #pragma lines, escapes decoded (octal, hex, \u / \U, the delimited and named
  forms), prefixes and raw pieces, a ud-suffix as a whole identifier (round 9: `"SR_"_é "LIVE"` still concatenates),
  and a comma-separated run of character / integer literals (a character array, a generated byte table) read as
  characters; a lone quote or an escape it cannot decode is exit 2. A value is attributed by clang's line markers to
  the file it starts in - a header through the units that include it; a build-directory file is `<build>/...` in both
  views. A name of a file's default view that its release view lacks is removed; one some file's release view still
  has survives [REL] (or a difftest file [NOT-REL]). A file no processed unit enters is reported unreachable with the
  literals the lexer reads in it. Exit 2 when a build graph or compile database is missing, a linked unit cannot be
  run, fails to preprocess or holds a literal that cannot be read, or an UNCOVERED input is not acknowledged.
- **UNCOVERED (gate_sweep).** What the exe takes in besides its C / C++ units' literals is listed, never skipped
  (round 8): a resource script, an #embed (clang's -dE prints the directive, not its bytes) or an .incbin string, a
  DLL it links or delay-loads, a link input the graph does not build, a library a unit asks for itself - `#pragma
  comment(lib)` (through _Pragma too) or `--dependent-lib` (round 9, Astra: kernel/vpad_filter.cpp's cfgmgr32.lib and
  setupapi.lib and kernel/image_write_watchdog.cpp's psapi.lib were missing; one the graph builds counts as covered
  when the exe links it and is exit 2 when it does not) -, a `#pragma comment(linker)` directive. A pre-link or
  post-build step outside CMake's known forms is no longer an item (rounds 9-11 listed it as `step ...`): it is exit 2
  (round 12). exe_sweep on the built binary is the authority for them - the release exe holds the resource, the
  manifest and the static libraries' code; a DLL is its own file (`exe_sweep.py --release <the dll>`). The tree has 31
  (2026-10-14): app/app_icon.rc (one ICON, app_icon.ico), ffmpeg_xenia.dll (Xenia's FFmpeg fork, 161 C units,
  delay-loaded), vcpkg's SDL2.lib and SDL2main.lib (since RELEASE_PLAN step 3 one SDL3.lib: SDL3's main is header-only, so 30
  now), app/windows.manifest, 25 Windows SDK / Visual C++ import libraries
  (20 on the link line, some also asked for by #pragma; cfgmgr32, setupapi and psapi by the tree's units, imm32 by
  imgui, msvcprt by the STL's use_ansi.h) and the UCRT wchar.h's `/alternatename` for its AVX2 wmem switch; no #embed,
  no .incbin (the exe's POST_BUILD only copies DLLs, data and licences next to it, in CMake's form).
  tools/player/gate_sweep_uncovered.txt lists them with the reasons; `--uncovered-ok` takes it, any item not in it is
  exit 2 (the report and the JSON are still written), and an item no longer uncovered is reported.
- **Advisory (gate_sweep --advisory, --view, --calls; the comparison line of a clang run).** Each file under app/
  kernel/ gfx/ guest/ (not guest/ppc) host/ tools/difftest is first read as translation phases 2 and 3 read it:
  backslash-newlines spliced, each comment (`/* */`, `//`) one space, literals (raw strings too) kept whole. A
  directive is then a logical line - a continued one read whole, a comment inside one read through - and text in a
  comment or a raw string is never a directive (a raw string's delimiter, `"` allowed, and body are read on the
  unspliced text, as [lex.pptoken] reverts its splices). A directive is split into pp-tokens, blanks being boundaries.
  Each file is read as SR_ORACLE=1 and as SR_ORACLE=0 sees it (only conditionals whose tokens are exactly `#if
  SR_ORACLE` / `#if ! SR_ORACLE` are evaluated, with their `#else`; an ORACLE_SOURCES unit's release view is empty).
  Every string literal starting `"SR_` outside comments is a name - getenv, env(), gateOn(), tables and log text
  alike. A name in a file's SR_ORACLE=1 view and not in its SR_ORACLE=0 view is removed; a removed name that another
  SR_ORACLE=0 view still names survives and is listed with where that file goes (REL / NOT-REL / OTHER). Any other use
  of the identifier SR_ORACLE stops the sweep (exit 2: the view would be wrong) - another conditional form (`#if
  SR_ORACLE /* c */ || 1`, a continued `#if X \ || !SR_ORACLE`, `#if SR_ORACLE and 1`, `not`, `defined`, `#ifdef`,
  parentheses), another directive (`#define ORA SR_ORACLE`, `#undef`), code (`if constexpr (SR_ORACLE)`) -,
  kernel/sr_oracle.hpp's default definition aside; so do an `#elif` in a split, an unbalanced one and an unterminated
  comment or unterminated / ill-formed raw string. The source sweep is an aid and the release binary (exe_sweep) the
  ground truth, so after Astra's round 3 the reader fails closed broadly instead of modelling more: it uses only its
  lexer's own tokens and literal spans, and is exit 2 on a raw string literal in any directive, on a string or
  character literal in any directive naming a macro some #if expression in the tree names (a `__has_include("x")`
  operand is a header-name, not a literal), and on any non-ASCII character outside comments and string / character
  literals, identifiers included. Header-names are lexed before comments and literals, where clang lexes them (Astra
  round 4: in `#if __has_include(<no//such>) || SR_ORACLE` the `//` had eaten the real condition): the operand of
  #include / #import / #embed, and of an `(` in an #if-family directive, spelled out or through a __has_include alias
  such as xxhash's XXH_HAS_INCLUDE; a quoted operand that a header-name and a string literal would read differently is
  exit 2 (clang itself reads `__has_include("no\")` as an unterminated string). In an #if line `(<` can also be an
  ordinary macro's argument (Astra round 5: `#define F(x) (0 x 0)` + `#if F(< SR_ORACLE >)` is `(0 < SR_ORACLE > 0)`),
  so there it is a header-name only as a plain path ([A-Za-z0-9_./+-], non-empty) followed by `)` - xxhash's
  `XXH_HAS_INCLUDE(<arm_neon.h>)` -, any other `(<` operand is exit 2, and so is any header-name whose text holds
  SR_ORACLE or a macro an #if expression names. Astra round 6 found the class behind these variants - an ordinary
  macro's argument can carry a token past the reader (`#if F(<X>)`, `#if F(X)` with X defined per SR_ORACLE) - so it
  is closed: an #if-family line may invoke (identifier then `(`) only the forms this tree uses, from an explicit
  allowlist in gate_sweep.py - `defined`, `__has_include`, and xxhash's XXH_HAS_ATTRIBUTE / XXH_HAS_BUILTIN /
  XXH_HAS_CPP_ATTRIBUTE / XXH_HAS_C_ATTRIBUTE / XXH_HAS_INCLUDE, each alias verified against every #define of it in
  the files (its builtin, or a constant fallback that ignores its argument). Any other invocation is exit 2; a new
  form is added to the list deliberately. `--calls <file>` writes the allowlist with every use and every alias
  definition.
- **Tests.** `python tools/player/test_gate_sweep.py [--cc <clang++>]` (exit 0 = every check passed): comments and
  continuations in directives, directive look-alikes in comments, raw strings and after a `/*` inside a string or a
  `//` comment, every unmodelled form, names, a scratch tree's removed / surviving sets and the exit codes, and a
  fixture table whose every row states what clang -E keeps under -DSR_ORACLE=0 / 1 - the sweep must name exactly that
  or exit 2. `--cc` re-confirms each row on a real clang (on the worker: `sr_cloud.py run --name <tree> -- python
  tools/player/test_gate_sweep.py --cc clang++`). Round 8 adds 44 checks - literal values and the escapes, the link
  closure and its database checks, the PCH, UNCOVERED and its acknowledgement; 33 run without a compiler - and
  0f351e61's reader (Astra round 7) fails 42, all but the run itself and clang's own confirmation; its value rule (a
  name anywhere in a literal) also moved four raw-string rows of the table, which 0f351e61's lexer and every earlier
  reader fail. Round 9 (Astra round 8 on fb8bab8a) adds 19 - #pragma comment collected and reconciled, link rules read
  (Astra's response-file repro and six variants), non-ASCII and UCN literal suffixes, PCH contexts (Astra's -DSWITCH=1
  repro, a shadowing include path), post-build steps; 11 without a compiler - and fb8bab8a fails 18 of them (it passes
  only the check that a harmless post-build copy is not reported). Round 10 (Astra round 9 on 0957d16f) adds 7 - a
  quoted object in a response file (Astra's repro) and in the command, a LINK_LIBRARIES bound by the file (Astra's
  repro) or the rule, an unknown flag, a file-level PRE_LINK, a PCH source using __INCLUDE_LEVEL__; 5 without a
  compiler - and 0957d16f fails 6 of them (it already caught a quoted object in the command itself, not in a response
  file). Round 11 (Astra round 10 on 43f9cf03) makes the scratch graphs ones ninja loads and adds 5 - Astra's EXTRA
  repro (`EXTRA = extra.obj`, `LINK_LIBRARIES = $EXTRA`, `EXTRA =`) and the same end to end through exe_sweep (the
  sweep must stop rather than hand exe_sweep a removed list without SR_HIDDEN for a binary holding it), a linker input
  spelled through a variable (ninja and the closure read the edge differently), a $in_newline response file
  (accepted), Astra's raw-string PCH repro; 3 without a compiler - and 43f9cf03 fails all 5 (and the 3 step checks,
  whose items are now named before / after the link). Round 12 (Astra round 11 on a2db69e4) adds 14, all without a
  compiler - Astra's three repros (the linker reading @exe.rsp.lib while ninja writes exe.rsp, `&&cd hidden.obj` in a
  response file, a copy between two links) and their CMake-template forms (a link flag naming @<rspfile>.more, `&&` in
  the response file, a static library's post-build copy over an object the exe links, a post-build step that links
  again), an edge's own rspfile and rspfile_content, a post-build step that is not a copy or copies over the exe and a
  file-level PRE_LINK (each exit 2 now, acknowledged or not; the last three were --cc step checks), and a quoted
  response-file argument ("@$RSP_FILE", @"$RSP_FILE") naming the edge's rspfile, accepted - and a2db69e4 fails 12 of
  them: it accepted Astra's three repros, the static library's post-build copy and the exe's second link outright,
  made the other three steps acknowledgeable items, rejected both quoted arguments, and rejected the template's `&&`
  and an edge's rspfile only for another reason (the `-Lx` before the `&&`, an @file it could not expand). Round 13
  (Astra round 12 on 839b0359) adds 15 - a post-build copy naming the exe through `\\?\` (Astra's repro), `\\.\`,
  `\??\`, a UNC share, an 8.3 name, a trailing dot or an alternate data stream, a link argument through `\\?\`, a copy
  through a directory junction and through a hard link, a BOM before the response file's first input (Astra's repro)
  and a non-ASCII input name, the command line's exit 2 when its message quotes such a name, and with --cc both repros
  end to end through exe_sweep; 13 without a compiler - and 839b0359 fails all 15, in the console's code page and
  under `python -X utf8` alike: it let every alias but the trailing dot through, and both end-to-end runs passed a
  binary holding SR_HIDDEN (the BOM one under -X utf8; in the code page it stopped on another reading of the name).
  Before that, of the 92 unit and fixture checks, 6cbf45f9's reader (Astra round 6) fails 4 - macro arguments in #if
  lines (`F(<X>)`, `F(X)`, an alias that passes its argument through) -, 46f81044's reader (Astra round 5) fails 9 -
  it read every `(<...>` in an #if as a header-name, so `#if F(< SR_ORACLE >)` kept both bodies -, 24317e77's reader
  (Astra round 4) fails 10 - header-name operands with `//`, `/*` or `\` inside, spelled out or through an alias -,
  39084c40's reader (Astra round 3) fails 19, among them its three repros: a raw string in a #define's macro arguments
  hid SR_ORACLE, a regex over the rebuilt line took `"R"` for a raw opener, an ASCII-only identifier made `eR"(` (a
  non-ASCII e) a raw string - and 94a2e527's reader (Astra round 2) fails 30, among them the 11 of round 2: it read
  `#if SR_ORACLE and 1` as `SR_ORACLEand1` (both bodies kept), let `not` / `defined` / aliases through, searched a raw
  string's closing delimiter after splicing and refused `"` as a delimiter character. The 610fac98 reader truncated a
  directive at `/*` (so `#if SR_ORACLE /* c */ || 1` read as a split), read a continued directive's first line only
  (so `#if X \ || !SR_ORACLE` kept both branches) and took a `/*` inside a `//` comment for a block comment (it hid
  1,911 lines of kernel/stubs.cpp and 363 of tools/difftest/difftest_main.cpp from the name scan); 51 of the 92 fail
  on it.
- **Binary (exe_sweep).** Every removed name counted as a C string in the release exe, and in the default exe as the
  control (a name in neither proves nothing). getenv takes a literal, so a name absent from the release exe has no
  reader there, however the source spells the read. A present name must be explained and listed in `--allow`; any
  other is exit 1.
- **Judging a survivor.** A survivor is a question, not a defect. On the combined C1 branch (2026-10-14: 193 removed,
  16 survivors) they were names only difftest reads; a name table no release code calls; a read that is constant false
  under SR_ORACLE=0; and 5 names the release exe keeps because a retained unit owns them, none of them a control of a
  removed oracle: SR_FAKE_SPLASH (existing forcing, master-gated by SR_ALLOW_FORCING), SR_PRESENT_DUMP (present dumps)
  and SR_HIGHFPS_PACE (pacing telemetry) - retained diagnostics -, SR_NATIVE_RESOLVE_SYNC (a retained developer
  setting: the native resolve pipelines built synchronously) and SR_VB_ALIAS_PROBE (=0 turns off a default-on platform
  accommodation). clang's views on dev 465c06ce (2026-10-14, the worker: 327 default / 302 release / 290 NOT-REL
  units, 1,353 files entered, 593 s) give the same 193 removed and 16 surviving names; one survivor place is gone -
  gfx/tex_ownership.hpp's arm-name table (SR_HLE_TRACE_HOTKEY, SR_HLE_TRACE_ARM_*), which the lexer listed as a REL
  survivor because it reads every header as release code: no release unit includes that header
  (kernel/hle_api_observe.cpp includes it under `#if SR_ORACLE`, its other includers are ORACLE_SOURCES units), so
  those six names survive only in difftest files. Unreachable there: guest/pgo_dump.cpp and two tools/difftest shader
  tests no target builds in this configuration. Round 8 on the same sources (dev f431b7f6, the worker): 335 default /
  310 release / 288 NOT-REL units (sr_imgui's 5 and xenos_xlat's 4 units are release now; gfx/xenos_bridge.cpp and
  gfx/shader_xlat_core.cpp move out of NOT-REL), the same 193 removed names and 19 survivors. The three new ones, and
  the new places, come from the value rule - a name anywhere in a literal, so a message that mentions a gate counts:
  SR_HLE_ARENA_WAIT and SR_TEX_OWN_CENSUS in difftest messages [NOT-REL]; SR_HLE_OWN_PRESENT in app/main.cpp's release
  boot log ("SR_HLE_OWN_PRESENT=0 restores host-driven present" - a variable the release no longer reads) [REL];
  SR_AB_TOGGLE also in kernel/gpu_ringless.cpp's error log [REL], a branch the release never takes (its sr::abt::reg()
  is a constant -1). The closure's new units and the PCH's text added no name; the advisory lexer, on the same rule,
  agrees name for name. Round 9 (the same sources): the same 335 / 310 / 288 units, 193 removed and 19 surviving
  names; the link rules of the exe, SupermanReturnsGuest, sr_imgui and xenos_xlat are accepted; the PCH's text reads
  the same in its 4 consumer contexts (2 per build: 2 units add -I<build>/generated, which resolves nothing
  differently); 6 more UNCOVERED inputs, from #pragma comment (above). Round 10 (the same sources): the same units and
  names (193 removed, 19 surviving) and the same 31 UNCOVERED inputs; the four link commands split as the linker reads
  them pass token for token, and the PCH's text reads the same force-included as in its producer in all 4 contexts.
  Round 11 (the same sources): unchanged again - the four linker edges read the same in ninja (-t query) as in the
  closure, their commands as ninja expands them (-t compdb -x, the response files inlined) pass token for token, and
  the 4 PCH contexts read the same under -E -P. Round 12 (the same sources): unchanged - the four link edges are
  CMake's templates (the exe's clang++ rule and SupermanReturnsGuest's llvm-ar rule through a response file,
  sr_imgui's and xenos_xlat's llvm-ar rule with $in), their commands are the templates filled with ninja's own values,
  the exe's POST_BUILD is CMake's copies of DLLs, data and licences, and every token passes. Round 13 (the same
  sources): unchanged - every name the four link edges read is ASCII (both response files too), no step or link
  argument is a path alias, and the exe's post-build copies write none of the linker edges' files by resolved
  identity.
- **Limits.** A name code assembles - at run time, by a constexpr computation, or as characters through casts - is
  in no literal value, so in no source view (the exe scan sees what the binary holds; a UTF-16 one neither). What
  each configuration compiles is still proven by the receipts (§1.8j).

### 1.8l Rolls from the SSD: `--stage` puts the game's read set on J: [tools/roll-stage]

In about a quarter of rolls the boot load took 6-100 s: the installed shader cache (856 files), the DXC DLLs and the
game image are read from D:, a 7200-rpm disk every worktree shares (devcreate-boot `archive/dcb/FINDINGS.md`). Owner
2026-10-14: "okay do them on j just be wary about space and resources" (J: = NVMe SSD).

    python tools/perf/flicker_gate.py --exe <snapshot> --out <bank> --stage J:\sr_stage            # or perf_run.py /
    python tools/perf/native_proof.py --plan <p> --exe <snapshot> --out <bank> --stage J:\sr_stage # sr_drive.ps1 -Stage
    python tools/perf/stage_roll.py check <perf_run out>      # per-scope witnesses: PASS / FAIL / INSUFFICIENT EVIDENCE
    python tools/perf/stage_roll.py status|clean --stage J:\sr_stage

What the game reads (from the code) and where the stage puts it - 2.42 GB for a snapshot exe today:

| Read | Size | The game's lookup | Staged as |
|---|---|---|---|
| `Superman Returns (USA)/` (the disc: DATA/*.AST, default.xex + .img, $SystemUpdate) | 2.17 GB, 15 files | content root = the exe folder, its parent, the working directory (`kernel/file_io.cpp`); image `<exe>/../<disc>/default.xex` first (`guest_harness.cpp`) | `content-<k>/` with the exe folder inside it |
| the exe folder: exe 156 MB, pdb 65 MB (dbghelp), SDL3 / dxil / dxcompiler (19 MB) / ffmpeg_xenia, `port_features/`, `data/fonts`, `data/glyphs` | 245 MB | DLLs and DXC from the exe folder; port features, fonts and glyphs beside the exe first | `content-<k>/exe-<sha12>-<k>/` |
| `data/shader_cache/dxil` + `point_sprite.gs.dxil` | 5.4 MB, 857 files | `SR_SHADER_CACHE_DIR`, else the working directory's, else the exe folder's; the GS from `<cache>/..` or the working directory (`gfx/shader_cache.cpp`, `d3d12_backend.cpp`) | `shader-<k>/` with the GS beside it, named by `SR_SHADER_CACHE_DIR` in every case |

- **Where outputs go.** The working directory stays the repo root, so the log, frames, `SR_PERF_DIR`, saves and
  settings (`SR_CONTENT_DIR`, per attempt) and dumps land where they do without `--stage`. Two exceptions: the game's
  generated port-feature layers (`<content root>/mods/.cache`, ~20 MB, regenerated on the first staged roll) move to J:
  with the content root, and the runtime translator's per-process scratch stays in `%TEMP%` on C: (pruned per roll).
- **Content-addressed.** Entries are named by sha256 over (path, size, sha256 of the bytes); a copy is hashed as it
  is written and refused unless its bytes are the digest's. The retail disc's source digests are cached in
  `<stage>/digests.json` by path + size + mtime (read-only; a disc file edited in place under the same size and mtime
  would keep its digest - the one blind spot); the mutable sources - the exe folder, its DLLs, port features, the
  shader cache (~250 MB) - are re-hashed on D: by every sr_drive stage (`--verify full`), which also re-hashes every
  staged file (2.4 GB from the NVMe, about 2 s) and re-copies any that differ. Stray files are removed (the game loads
  every file in the cache folder). A second roll with the same exe and data copies nothing. The first staged roll
  hashes and copies 2.4 GB from D: before its launch: perf_run stages (`--verify quick`: cached digests, manifest size
  + mtime) before it starts the play tape (harness_play gives the game 60 s to open its input channel).
- **Leases.** sr_drive stages under a lease (`<stage>/leases/<id>.json`: the exe folder, the disc and the shader cache)
  written inside the stage lock; each game it launches is added as a second holder (`lease-add`), the lease lives while
  any holder runs (pid + creation time) and at most 12 h, and sr_drive's `finally` releases it on every exit path
  (`-KeepAlive` keeps it for the game). `clean` and retention never remove a leased entry. perf_run's pre-stage holds
  its own until it exits. Residual gap: a driver killed between a launch and its `lease-add`.
- **Space.** Each stage prints what it copies and J:'s free space, and refuses below 50 GiB free (+256 MiB reserve for
  the game's writes): before anything is created, before every file it copies, and after copying (no `STAGE_OK`, no
  lease); sr_drive re-checks before every launch (a retry too). Retention keeps 3 exe folders, 3 shader caches and the
  current disc. `clean` removes every unleased entry, then (still holding the lock) the marker, then the folder.
- **Safety.** The stage lock is an OS byte-range lock on `stage.lock` (msvcrt.locking): the OS drops it when its holder
  exits, so there is no stale-lock recovery. Every metadata / temporary file is created with O_EXCL after its folder is
  checked (no write through an existing name, no truncation). Deletion goes file by file without following links:
  any reparse point stops it, each file's resolved path is re-checked inside the stage just before its unlink, and only
  emptied folders are removed. Residual race: a folder swapped for a junction in the microseconds between that check
  and the unlink; only this tool's processes write under the stage, so it is not a realistic threat here.
- **Refused:** `SR_MODS` (mods/ is read from the content root) and `SR_DEVTOOLS` (plans / devtools_out live under it);
  the `title` / `arena` perf_run scenarios (other harnesses); a source disc without default.xex, its image or one of
  the 12 retail DATA/*.AST files (the game would fall back to D:); a junction / symlink inside any source, in the stage,
  in its root or an ancestor (nothing is written or deleted through a link; `clean` refuses before removing anything);
  a non-empty folder without `SR_STAGE_ROOT.txt`. `--stage-source <dir>` stages the disc from a folder that is not
  beside the repo, so a lane worktree can roll its own tools.
- **Confirming it** (`stage_roll.py check <out>`, exit 0 PASS / 1 FAIL / 2 NOT STAGED / 3 INSUFFICIENT EVIDENCE):
  each scope needs a positive witness inside the stage and none outside - exe image (provenance exePath + sha256,
  `perf/meta.txt`, the perf memmap), shader cache (`[SHADER_CACHE] loaded ... from`), overlays and content root
  (`[MOD] overlay adds`, `Content root:`), disc opens (a SUCCESSFUL `NtCreateFile ... -> handle=`; FILE_NOT_FOUND and
  alias lines are no witness, the alias target's own open is), game image (`xex_loader: loaded ... from`), DXC (the
  perf memmap's `dxcompiler.dll` path), glyphs (`[GLYPH] ... from`); any path line naming a disc file outside the stage
  FAILs. The player log (`--config port`) witnesses only exe, shader cache, overlays and content root; the dev log
  (`perf_run --scenario gameplay --config port --gates SR_LOG=dev` - not `--config portdev`, which the gameplay
  scenario refuses) with perf_run's SR_PERF_STATS witnesses everything but fonts (smoke roll 2026-10-14,
  roll-stage `archive/stage/smoke_gp`: 8 scopes PASS from J:). No log line names the font folder in any profile, so
  fonts always read INSUFFICIENT EVIDENCE (staged beside the exe, which `fontDir()` searches first).

### 1.8m Graceful quit: whole-run counters from a terminal snapshot [tools/graceful-quit]

Live rolls ended with sr_drive's `Stop-Process -Force`, so no exit code ran and every census had a blind tail of up to
one reporting period: `[PM4_COV]` / `[PM4_COVMAP]` (256 frames or 20 s), the `[GPU_CP]` census (120 swaps) with the
transport / ringless / devcreate lines beside it, `[NATIVE_FLUSH]` (2^18 draws per thread), `[NATIVE_METER_RUN]` and
`[AUDIO_NATIVE]` (30 s). Even a window close ends without them: `hostExitNow` closes the log before `quick_exit`, and
the meter's exit line is an `atexit` handler `quick_exit` never runs. A whole-run claim (C1: 0 emulated work across the
run) needs a terminal, quiesced measurement. The graceful quit gives one.

    powershell -File tools\sr_drive.ps1 -Scenario <s> -Out <o> -GracefulQuit   # or SR_DRIVE_GRACEFUL_QUIT=1; sr_roll.ps1 -GracefulQuit passes it on
    python tools/perf/terminal_snapshot.py <roll or run.log> [--require]        # did the roll end on a complete terminal snapshot?
    python tools/perf/quit_set_grade.py <set or roll> ... [--require-ring N]   # every quit of a set complete, every stranded thread backed
    python tools/perf/owner_census.py --base <r0> --target <r1> --require-terminal   # grade frames 1..N, no tail
    python tools/perf/residue_census.py <base> <target> --require-terminal

**The protocol.** Opt-in on the harness side; the game side exists only in automated runs (SR_INSTANCE_ID set; a
player never has it; `SR_QUIT_CHANNEL=0` turns it off).
- *Game* (`kernel/quit.cpp`): at boot it creates two manual-reset named events, `Local\SR_QUIT_<id>_<pid>` (request)
  and `Local\SR_QUIT_ACK_<id>_<pid>` (ack). `<id>` is sanitised like the harness keyboard channel (`[A-Za-z0-9_.-]`, at
  most 64, `default` when empty) and `<pid>` is the game's own PID, so a harness can only ever signal the process it
  launched. One `[QUIT] channel ...` boot line. A watcher thread blocks on the request.
- *Harness* (`tools/sr_quit.ps1` `Invoke-SrGracefulQuit`, used by sr_drive's finalization in place of the
  Stop-Process): it opens BOTH events by name before signalling (an exited game's events vanish with it), sets the
  request, waits for the ack (5 s), then for the process to exit (45 s), and falls back to the old `Stop-Process -Force`
  whenever a step fails. Outcomes: `complete` (ack, exit 0), `failed` (ack, exit 81: the game printed `[QUIT] terminal
  snapshot FAILED` and exited without a snapshot), `deadline` (exit 82), `exit-other` (ack, any other exit code),
  `no-channel` (the request event does not exist: an older exe, the channel off - immediate fallback), `no-ack`
  (fallback after the ack timeout), `exit-timeout` (fallback after the exit timeout), `already-exited`, `bad-args` (a
  timeout outside 1..600000 ms - `-1` would wait forever - or no PID: no request, immediate fallback) and
  `harness-error` (anything the harness side threw: fallback, the error recorded). It never throws. sr_drive reads
  `SR_DRIVE_QUIT_ACK_MS` / `SR_DRIVE_QUIT_EXIT_MS` through the same bounds (digits in range, else the default and a
  WARN line), and writes `<attempt>/quit.json` (schema `sr-quit/1`) in a `finally` for EVERY ending, plus one `quit:`
  journal line; sr_roll copies the last attempt's receipt beside its `run.log`, run_recomp_x writes
  `superman_returns_run<i>.quit.json`.
- *Exit evidence* (Astra round 1, P1-3). The game prints the complete line and only then closes its log and exits; its
  hard deadline (exit 82) or a harness fallback can still end it after that line. So the graders count a snapshot as
  complete only with BOTH the `[QUIT] terminal snapshot complete` line and a receipt saying result `complete`,
  graceful, no fallback, exit code 0. The complete line without a receipt is `unconfirmed`; with any other receipt,
  `rejected`. Neither grades as whole-run.

**The safe point.** Seven ordered stages on the watcher thread, every wait bounded:
1. *Claim.* An atomic state RUNNING -> QUITTING. A game already exiting (a window close or SR_Quit reached
   `hostExitNow` first, which claims EXITING) is left alone: no ack. A window close during a quit waits for the quit
   (`hostExitNow` never exits under it). The ack is set; `[QUIT] request received ...`.
2. *Ring cut (Q1).* `quit::g_ringCut` is written under the CP's publish mutex `c.mu` (`gpuCpCutRing` ->
   `quit::cutRingUnder`), and `gpuCpDoorbell` tests it under the same mutex, in the critical section that publishes
   the write index (`quit::publishUnlessCut`). So a doorbell either published before the cut or publishes nothing: no
   in-flight window, no count to wait for. The doorbell (the guest's own GpuRingUpdateWritePointer 820FD218, ring or
   ringless) is the only way work reaches the CP: an INDIRECT_BUFFER descriptor, a ringless item or a native segment
   becomes consumable only when a doorbell publishes its index space. When `cutRingUnder` returns, the CP's write
   index is final. A doorbell after the cut releases the mutex, then parks its caller - unless the caller holds a
   tracked guest lock (the SDK's 820FD800 publishes under the spinlock dev+0x2A78) or is an exempt loop: then the
   publish is dropped (counted, `doorbell_drops=`) and the thread runs on to its release (see Q3).
3. *CP drain (Q2).* The CP thread is woken with a drain request. It consumes the ring up to that final write index
   (ring packets, ringless items, native segments, the ISRs it runs at INTERRUPT packets), then `cpFlushDraws()` (the
   native executor / drain worker perform everything queued), then parks itself at its idle point and hands over rptr,
   wptr, swaps and the items left unconsumed (a bin select or segment pushed for index space no doorbell published -
   built, never submitted; `rl_pending=`). Guest threads still run here, since a WAIT_REG_MEM may wait for a CPU write
   and a fence the CP writes releases a waiting render thread (which then parks at the doorbell).
   `SR_QUIT_DRAIN_MS` (10000). No CP thread (before the first doorbell, `SR_PM4_CP=0`): nothing to drain.
4. *Thread cut (Q3).* `quit::g_threadCut`. Every kernel import (`guest/ppc_import_glue.cpp` `callKernel`) passes an
   ENTRY gate before the export runs and an EXIT gate right after it returns - before the import trace and before any
   guest APC it delivers - and marks the thread "inside <import>" in between (its gate slot). At the cut a thread
   parks at its next gate: before an import runs, or, if it was already inside one (a blocking wait), when that import
   returns, before it runs a single guest instruction. Release imports (RtlLeaveCriticalSection,
   KeReleaseSpinLockFromRaisedIrql, KfReleaseSpinLock, KfLowerIrql, KeLeaveCriticalRegion, KeUnlockL2) pass their
   entry and park at their exit, after the release. **The locks** (Astra round 1 note b, then smoke1): the seven lock
   thunks keep a per-thread count - KfAcquireSpinLock, KeAcquireSpinLockAtRaisedIrql, RtlEnterCriticalSection and
   RtlTryEnterCriticalSection count BEFORE their import (a failed try uncounts), the three releases uncount BEFORE
   theirs. WHILE THE CP DRAINS (ring cut to thread cut) no thread parks holding one: the ISRs the CP runs may need it,
   so a holder's doorbell is dropped (`doorbell_drops=`) and no other gate is armed yet. AT THE THREAD CUT - the CP has
   drained and parked - every thread parks at its next gate, lock holders and exempt loops included. Round 1 let a
   holder pass every gate instead; the first smoke roll (gq/smoke1) showed the cost: the render thread, holding a
   critical section across its frame, passed every gate and drew a whole frame after the cut (vdswaps 5615->5616, the
   native flush activity +3210, two ringless items queued), so the counters never settled. Nothing the quit still
   needs can wait on what a parked holder keeps: a thread that wants the lock blocks inside its acquire import - it can
   never get it, and if it did, that import's exit gate would park it. The ringless producers (the IB / segment
   publish in GpuRingUpdateWritePointer's override, the bin select) test the ring cut before they queue an item, so
   none is queued for the drained CP either (smoke1: `rl_pending` 0->2). Host loops: the exempt ones park at a safe
   point right before their guest call (VD_ISR after its vblank wait, before the ISR and its delivery lease; the audio
   worker after its pacing wait, before the credit and the client lock; the legacy render loop at its top, where the
   callback follows), and every GuestCaller host loop (RTAM, the frontend watchers) parks at its next
   `GuestCaller::call` (`quit::guestCallEnter`). A parked thread blocks forever; the process exits around it.
5. *Settle (Q3 + Q4): acknowledgements, not a quiet interval* (Astra round 1, P1-1). Every 50 ms the coordinator
   examines every thread that can run guest code - every registered guest-running thread (`g_debugThreadRegistry`)
   and every exempt or GuestCaller loop - and classifies it: **parked** (it registered itself at a park point: its
   acknowledgement), **waiting** (inside an import using < 1 ms of CPU over the window - blocked in a wait, a
   contended critical section, host I/O - or spinning in KfAcquireSpinLock / KeAcquireSpinLockAtRaisedIrql /
   RtlEnterCriticalSection: whatever it holds, when that import returns its exit gate parks it before any guest
   code), **host-idle** (a GuestCaller loop between guest calls) or **unacknowledged** (running guest code, busy inside
   an import - in flight, it parks at the exit -, an exempt loop in host code not yet at its safe point, a thread that
   appeared during the window). A thread waiting while holding a lock (smoke1: tid 24256, the audio DAC thread, in
   KeWaitForSingleObject with a critical section) is waiting like any other: its wait can end only by a timeout or a
   signal from a host thread (every guest thread is parked or blocked, the CP and the vblank / audio loops parked), and
   then it parks at the exit with the lock still held - no guest code runs, so no counter it could move moves, and
   the lock is never released to anyone. The stage needs every thread
   acknowledged AND a digest of the producer counters (the CP's doorbells, packets, swaps, ISRs, rptr, wptr and pending
   items; every native meter column; the guest's VdSwap calls; the native flush and ringless producer totals)
   unchanged, both over `SR_QUIT_SETTLE_MS` (200), within `SR_QUIT_SETTLE_MAX_MS` (3000); else FAILED
   `threads-not-quiescent` (naming the first thread: `tid= name= running guest code (no gate reached)`, `busy in
   <import>`, ...) or `producers-not-quiesced` (naming the fields). A silent spinner is caught though no counter
   moves. The `[QUIT] stage settle:` line carries `threads= parked= waiting= host_idle= stranded= untracked=
   holding_locks= doorbell_drops=`, and one `[QUIT] thread tid=.. name=.. state=waiting|host-idle|stranded|unacked
   import=.. locks=.. cpu_ms=..` line per thread not parked.
   **Stranded (native/quit-drain, 2026-10-16).** The drop rule of step 4 has a consequence the first live quits found
   (memres1/mrc2; s3check2 rel_gate span and rel_gate_seed03 freeze - all FAILED `threads-not-quiescent`, the render
   thread `running guest code (no gate reached)` with 3 locks): the render thread, holding the device locks, publishes
   a segment after the ring cut - dropped, so the fence that segment carried is never written - and then calls
   GpuBlockOnFence on it. That wait spins in GpuRingWaitProgress 820F33E8: guest code, no import, no gate (its hang
   trap counts KTHREAD+0x58, which never advances here: docs/NATIVE_ROADMAP.md P2.10). Nothing can end it once the CP
   has parked, and nothing it does moves a counter.
   *What the cut dropped (round 2, Astra round 1 P1).* A segment close carries exactly ONE fence (GpuBuildFenceWritePacket
   820FD580: `EVENT_WRITE_SHD {3, mangled(fb)|2, F}` at the segment's tail, F = `dev+0x2A1C`, then +2; the segmentless
   close's kRkFence record S). The drop branches record `{dev, F}` - only once the ring is cut, so nothing before the
   request: ringless `updateWritePointer` reads the dropped IB's own tail (`quit::recordDroppedIb` / `ibTailFence`: the
   close's segment at `dev+0x34C4`, the packet's address checked), native `publishSegment` records the S its close handed
   it (`quit::recordDroppedFence`). The cut also marks the global wait sequence (`cutRingUnder`). The legacy CP_RB_WPTR
   doorbell and chain mode record nothing: a wait on their fences stays FAILED (fail closed). Round 3 (Astra round 2
   P1): the tail is read from a PLAIN IB only (`native::flush::cutPublicationKind` 0). A native segmentless
   publication's guest bytes are not its packets (record mode leaves the bytes under a native footprint untouched, so a
   reused segment can still end in an older close's `EVENT_WRITE_SHD ... F`): the close's own hand-off records its S
   (publishSegment), a pump replay of a deferred entry or a hand-off pending for another block records nothing.
   *The spin phase (round 2, P2).* The three XDK GPU-wait strong-defs (kernel/gpu_hang_wd.cpp) mark the thread's gate
   slot while it is inside the wait (`quit::gpuWaitEnter` / `gpuWaitLeave`: kind, dev, target, a process-wide residence
   sequence number under a seqlock; a nested wait's leave resumes the outer as a NEW residence - fresh number, PRE - so an
   unchanged number means an uninterrupted residence). GpuRingWaitProgress 820F33E8 has a pass-through strong-def (it
   delegates exactly once, unconditionally, returns the body's r3, writes no guest state; docs/OVERRIDES.json observe):
   a call from GpuBlockOnFence's spin loop (return address 0x820FDA44, its one call site there) promotes the fence wait
   from PRE (the fence test, the `F == next` + `dev+0x3290` early return, the preliminary flush) to SPIN with a fresh spin
   number and counts the turn; a 0 return (the no-GPU mode, or its own hang trap) marks it EXITED.
   *Which call is the spin (native/quit-ringspace, 2026-10-17).* Both waits build a 0x90-byte frame and pass
   GpuRingWaitProgress the wait record at `r1+0x50` - the caller's r1 - 0x40 (IDA 820FD9A8 / 820FCB30), which the mark
   stores at entry (`kWaitRecBelowSp`). `quit::gpuSpinSite(lr, r3)` decides: a fence wait's spin call = return address
   0x820FDA44 AND its own record (the hardening: a stale lr can never promote a PRE wait through a foreign record);
   GpuRingWaitForSpace's = its own record alone, because that body is LOCALIZED (`skip_lr`): its call leaves `ctx.lr`
   stale (the hang dump's callerLR 8271EE0C in s3m_gate), and inside its mark the thread runs only the generated 820FCB30
   body, whose callees are 820F33E8 (the one site, r3 = that record) and, after the loop, GpuTimingEventReport (no
   820F33E8 call). The ring mark also stores the generation r5. GpuRingWaitReadPtrWrap's mark has no record: its spin is
   never recognised. Every other GpuRingWaitProgress call costs a thread-local load and a relaxed load.
   *Stranded.* The census acknowledges a thread as **stranded** only when ALL of: (1) the CP drain is over; (2) it is in
   GpuBlockOnFence's SPIN loop; (3) the same residence and the same spin as when the window began; (4) that spin began
   after the ring cut (a wait already spinning at the request - a fence lost earlier - is a hang the quit did not cause);
   (5) the loop turned since the previous check (every check consumes that baseline, so every check follows a sleep:
   the check that ends the stable window is itself the record, and so is the deadline's - round 3, Astra round 2 P2; a
   waiter sharing one CPU with the coordinator runs only while it sleeps); (6) GpuBlockOnFence's own test still false, `(next - F) < (next -
   completed)` with next = `*(dev+0x2A1C)`, completed = `*(*(dev+0x2A10))`, and the no-GPU exit closed (byte
   `dev+0x2A39 & 2` clear); (7) `{dev, F}` was in a publish the cut dropped. Anything else stays FAILED, the
   unacknowledged line naming the first condition that failed (`running guest code (no gate reached) in GpuBlockOnFence
   (not stranded: <why>)`).
   *Stranded in GpuRingWaitForSpace (native/quit-ringspace).* The first live quits after native/quit-drain found the
   other face of the same drop (mrt8 ch18, mrt10 nb: 2 of 7 graceful quits on 2026-10-17, and s3m_gate on 10-16, all
   FAILED `threads-not-quiescent`, the render thread `running guest code (no gate reached) in GpuRingWaitForSpace
   locks=3`): holding the device locks through the drain, the render thread keeps closing segments, every close is
   dropped, and it laps the command buffer until its reservation in GpuRingWaitForSpace 820FCB30(dev, end r4,
   generation r5) waits for the GPU's PROGRESS WORD `*(fb+4)` - `(gen & 3) | cursor`, written only by the CP executing
   the first EVENT_WRITE_SHD of a segment's fence tail (GpuBuildFenceWritePacket 820FD580; the native close's kRkFence
   record) - to leave `[.., end)` of the previous generation (its own test, `quit::ringSpaceFree`: `(gen - P) & 3` is 0,
   or 1 with `end <= (P & ~3)`). The drop branches also record each dropped close's progress word (`quit::
   recordDroppedProgress`, the same per-device tables, after the cut only, once per value, the shared overflow; the PM4
   tail parse reads it from the packet right before the fence packet, its header, event and `mangled(fb+4)|2` checked -
   anything else records the fence alone; native `publishSegment` takes it from `segmentClose`, the coherency block
   none). The rule: conditions (1)-(5) unchanged (the ring wait's own spin loop), the no-GPU exit closed, and (6') its
   own test still false on the progress word the parked CP last wrote, (7') some recorded dropped progress word passes
   that test - had the CP executed that publish, the wait would have ended. (The dropped closes follow the last
   executed one and stay within one lap of it - the writer cannot reserve past the frozen word - so the mod-4 test cannot
   alias.) The stranded line reads `wait=GpuRingWaitForSpace target=<end> completed=<the frozen progress word>
   next=<generation>`; the settle line adds `dropped_progress=N`. GpuRingWaitReadPtrWrap keeps no rule (marked, named,
   never stranded).
   *Never silent.* Each stranded thread gets a `[QUIT] stranded tid=.. wait=GpuBlockOnFence dev=.. target=.. completed=..
   next=.. seq=.. spin=.. locks=..` line (or `wait=GpuRingWaitForSpace`) right after the settle line (which carries
   `stranded=N`, `dropped_fences=N` and `dropped_progress=N`),
   the count is `stranded=N` LAST on the complete line, the verify compares the stranded set identity by identity (a
   swapped thread with the same count FAILS), more than 64 stranded threads FAIL the settle (`stranded-overflow`, never a
   silent cap), and the harness copies both into quit.json (`stranded`, `strandedThreads`; `Add-SrQuitTerminalInfo`) and
   the journal line. Graders: `terminal_snapshot.py` refuses a count that differs from its records; owner_census /
   residue_census carry the count, the records and an explicit `stranded_policy` (`allow`: the counts close at the ring
   cut, the stranded thread's dropped work is after it; `forbid`: not complete; `--stranded-policy`); native_proof's plan
   key `stranded_policy` (default allow, recorded in the proof and in each terminal item's note). Where a stranded thread
   must still fail: `SR_QUIT_STRANDED=0` on the game, `--forbid-stranded` / `stranded_policy forbid`. Proof:
   `build/difftest.exe --quit-test` section F (the REAL generated GpuBlockOnFence on a device in guest memory: the bug,
   the fix, a hang that began before the request staying FAILED, each condition failing alone, nine threads), RW
   (native/quit-ringspace: the REAL generated GpuRingWaitForSpace reserving a lapping buffer - the bug through the body
   alone, the fix through the strong-def with `ctx.lr` stale at 0x820FDA44, the forbid switch; RW2 the native close's
   progress word, RW3 a broken progress packet, RW4 a foreign record never promoting; RS0-RS9 each ring condition failing
   alone against a control on s3m_gate's numbers), T (the drop record), PT (the pass-through against the bare body, ten
   states), S21 / S22 (identity verify, the cap), W9-W12 (the wiring) and `tools/difftest/quit_drain_mutation_run.py`
   (49 compiled mutants).
6. *Terminal snapshot.* Each family's own printer, with the why token `terminal` (same formats as the periodic lines),
   its lines also captured (`Log::setThreadCapture`, the coordinator thread only):

| Family | Lines | Printer |
|---|---|---|
| CP census | `[GPU_CP_TIME]`, `[GPU_CP]`, `[GPU_COHER]`, `[RINGLESS]`, `[NATIVE_TRANSPORT]`, `[NATIVE_DEVCREATE]` (each under its own gate) | `gpuCpCensusLine` (the CP's `census`) |
| PM4 coverage | `[PM4_COV] terminal tail` + the tail's F line (below), `[NATIVE_CPTIME]`, `[PM4_COV] CUM`, `[PM4_COVMAP]` owner / inner / kind / internalOrigin, `[NATIVE_BRACKETS]`, `[NATIVE_REC]` / `[NATIVE_DRAW]` | `pm4cov::terminal` |
| native flush | every `[NATIVE_FLUSH] <why> ...` line of the periodic report (draws, literal, CHECK, residue, bracket, ...), and once the segment transport ran its segment lines `[NATIVE_FLUSH] terminal seg closed=..` (idMiss, mismatch, capDrop, ...) and `[NATIVE_FLUSH] terminal seg tile segs=..` (gap, overlap, order, dup, reorder; SR_NATIVE_SEG_TILE) - the periodic ones keep their exact format, no why token (Astra round 1, P1-2) | `flush::reportLine` (`segReport(why)`) |
| native executor | `[NATIVE_EXEC]` | `exec::report` |
| native meter | `[NATIVE_METER_RUN]`, `[NATIVE_METER_KINDS] run` | `meter::report` |
| audio | `[AUDIO_NATIVE] terminal ...` (logged directly, not through the audio log pump) | `audioNativeCensusLine` |
| P2 trace, A/B drain, gauges | `[P2_TRACE]`, `[AB_DRAIN] terminal stats ..` (SR_AB_PRESENT_DRAINED), `[GAUGE]` (under their gates) | `p2trace::finalLine`, `abt::drainStatsLine`, `gaugeCensus` |

   Not covered: the `[NATIVE_FLUSH] split snapshot` (branch native/flush-residue, not on dev: it registers with
   `quit::addFamily` when it lands), `[CONST_DEDUP]`, `[NATIVE_FRAMESPLIT]` CUM and the window-delta censuses
   (`[NATIVE_DRAIN_CENSUS]`, `[HITCH_SUM]`).
7. *Verify and exit (Q5)* (Astra round 1, P1-1: the digest must cover everything printed). `SR_QUIT_SETTLE_MS` after
   the snapshot every thread must still be acknowledged (else FAILED `verify` `threads-not-quiescent`), and a
   capture-only re-render of every family (never logged) must equal the snapshot on EVERY integer field it printed:
   `quit::snapshotDiff` pairs lines by shape (digits blanked) and occurrence and compares each `key=N` (N followed by a
   space, `,`, `;`, `|`, `)` or the end); floats, keys ending in us / ms / ns (any case of the first letter) and
   `*_TIME` / `*_TIMING` lines are times, not counters; a line only the re-render has is a difference, a line only the
   snapshot has is not (the one-shot tail frame). Any difference, or the digest moving: FAILED `verify`
   `counters-moved-after-snapshot` naming `[TAG] key(a->b)`; no integer field at all: FAILED `snapshot-empty` (a broken
   capture never passes). Then `[QUIT] terminal snapshot complete frames=N swaps=S vdswaps=V tail=T rl_pending=R
   threads=.. parked=.. waiting=.. host_idle=.. families=K fields=F elapsed_ms=..` (`fields` = the integer fields
   verified), and the same exit as a window close: input tape closed, log closed (fclose flushes it), C stdio flushed,
   `std::quick_exit(0)`.

A hard deadline guards the whole sequence: a thread started at the request terminates the process with exit 82 after
`SR_QUIT_HARD_MS` (30000), writing one FAILED line only if it can take the log lock without waiting. Every
`SR_QUIT_*_MS` is digits only within its range (HARD 1000..600000, DRAIN 100..120000, SETTLE 10..60000, SETTLE_MAX
100..120000); anything else (`-1`, a sign, a unit, overflow) is the default, named on the channel line (P2-6). A DIAGNOSTIC, never in an evidence run: `SR_QUIT_DIAG_CUT_DELAY_MS` (0..5000, default 0 = off;
native/quit-ringspace) holds the thread cut back that long after the CP drain (`[QUIT] stage diag-cut-delay: ...`, and
named on the channel line), so a guest-lock holder runs on dropping its publishes against the parked CP - the
stranded states provoked on demand (a fence wait on a dropped fence, a ring-space wait lapping the frozen progress
word). Nothing is forced, but guest-side counters (vdswaps) keep moving until the thread cut.

**What "quiesced" means:** Q1 (no doorbell can publish: the cut is under the publish mutex), Q2 (the CP consumed and
performed everything published, then parked), Q3 (every thread that can run guest code is acknowledged: parked, or
blocked inside an import (idle, or spinning for a guest lock) - it parks at that import's exit, whatever it holds,
if the import ever returns - or a host loop between guest calls), Q4 (the producer digest unchanged over the settle window) and Q5 (settleMs after the
snapshot, every integer field it printed unchanged, every thread still acknowledged). The complete line requires all
five. The bound of the claim: guest code runs nowhere after the snapshot; the only code that can still run is the
host side of a wait import whose wait ends in the microseconds between the verify and the exit, before its exit gate
parks it - after the measured run. **Frames:** `frames=N` = the XE_SWAP
packets the CP executed (completed GPU frames; with no CP, the guest's VdSwap calls); `vdswaps` = the guest's VdSwap
calls (the meter's frame count). **The tail:** segments are kicked several times a frame, so the drain usually ends
inside frame N+1. Nothing is dropped: pm4 coverage prints that partial frame as `[PM4_COV] terminal tail frame=N+1
pk=..` followed by its F line, so the F lines and the terminal CUM still reconcile (owner_census `frames_match_census`),
and the complete line says `tail=1`. A per-frame claim covers frames 1..N plus that disclosed tail.

**Failure modes.** No ack: an exe without the channel, `SR_QUIT_CHANNEL=0`, a game already exiting, a wedged watcher
-> the harness falls back after the ack timeout; the log has no `[QUIT] request` line. A hang during the drain (a
WAIT_REG_MEM no one will satisfy, an untracked guest lock a parked thread holds needed by an ISR on the CP thread):
FAILED `drain-timeout` with rptr, wptr and the CP phase, exit 81, no terminal family line. A thread that never
reaches a gate (a guest spin with no import, a thread blocked in host code outside an import): FAILED
`threads-not-quiescent` naming it. Producers that never settle: FAILED
`producers-not-quiesced`. A counter or thread that moves after the snapshot: FAILED `verify` (the terminal lines are
in the log, but no complete line follows them). A printer blocked on a lock a parked thread holds: the hard deadline,
exit 82. A game that dies mid-quit (crash, `[NATIVE_FLUSH_BAD]` `_Exit(3)`): `exit-other`, no complete line. In every
case the process ends, the graders see no complete line or no clean-exit receipt, and nothing grades as whole-run by
accident.

**Why it cannot deadlock what it ends** (Astra round 1, note b). (1) The coordinator never waits for a guest lock or
for a guest thread to do anything but stop: the locks it takes are the CP's publish mutex (`cutRingUnder`; a doorbell
never parks holding it - `publishUnlessCut` releases it before `doorbellCut`), the CP's item mutex (`gpuCpRlPending`;
the CP parks after releasing it), the parked-thread table (held for bookkeeping, never across a wait), the family
table, the log spinlock (a thread never parks inside a log call: every park point is a gate outside one) and the
meter's mutex by `try_lock`. (2) While the CP drains - the one stage that needs other threads to make progress, since
its ISRs may take a guest lock - no thread parks holding a tracked guest lock (kernel spinlocks and critical sections,
the seven lock thunks): only the doorbell parks then, and a holder's doorbell is dropped. (3) After the drain the
CP is parked; a holder parking then can only stall a thread that wants its lock, and that thread is blocked inside its
acquire import - acknowledged, and parked by that import's exit gate if it ever got the lock (an exempt loop's ISR or
callback included: the gates arm for every thread at the thread cut). (4) Every wait of the sequence is bounded (the
drain, the settle window, the verify gap) and the deadline thread terminates the process whatever any lock does. What
stays untracked - a guest-inline spinlock (lwarx / stwcx), a mutant or semaphore used as a lock, a raised IRQL - can
stall the CP drain behind a doorbell caller parked holding it (`drain-timeout`) or leave a guest thread spinning in
guest code behind a parked holder (`threads-not-quiescent`), visibly and bounded, never a hang and never a false
complete. A printer blocked on a HOST mutex a parked thread holds (one held across a guest import by an override)
ends at the hard deadline (exit 82).

**The cost when no quit is requested** (Astra round 1, note a; `--quit-test` B, measured on the cloud worker, 20M
iterations, two runs - the shared VM's noise is the spread): the doorbell's cut test is ONE relaxed load inside the
publish mutex it takes anyway - no RMW, no fence (+0.14 / +0.46 ns over a 10.5 / 19.4 ns uncontended lock: noise).
The import gate: a relaxed load of the cut flag at entry and at exit, and this thread's "inside <import>" mark (a
thread-local slot pointer and a plain store each way, no RMW): 0.7-2.3 ns per import, entry and exit together - on a
path that already makes several thread-local stores per import (the guest LR, the extra args, r13, the call ring).
The lock thunks: a load and a store of the owner-written count, no RMW (1.0-3.3 ns per acquire / release pair).
`GuestCaller::call`: the same pair around each host-loop guest call. Plus one thread blocked on an event.

**Tools.** `tools/perf/terminal_snapshot.py` is the shared reader: `read(log)` -> the complete or FAILED line, frames,
tail, the terminal lines of each family, and the exit evidence (`find_receipt`: `<log stem>.quit.json`, `quit.json`
beside the log, else `drive/attempt<N>/quit.json`; status `complete` only with a clean-exit receipt, else
`unconfirmed` / `rejected`); `last_line(lines, regex, require_terminal, snap)` picks a family's terminal line inside a
complete snapshot or, without the requirement, today's last line. A log without `[QUIT]` lines (every roll before
this lane, every roll without `-GracefulQuit`) grades exactly as before; `--require-terminal` turns its absence into a
failure. `owner_census --require-terminal` grades the TERMINAL rows only: `[PM4_COVMAP] terminal inner=` lines strictly
between the request and the complete line; a snapshot whose rows are labelled periodic, a row or CUM line after the
complete line, or a producer with only periodic rows fails `*_census_terminal` (P2-4; `coverage.terminal_rows`,
`rows_outside_snapshot`, `producers_without_terminal_row`). `native_proof --graceful-quit` adds, per roll and gate
roll, `<prefix>.<tag>.terminal` (the complete line), `<prefix>.<tag>.quit_receipt` (a json item on quit.json: result
complete, graceful, no fallback, exit 0) and, for EVERY original text item, its terminal twin `<item id>.terminal`:
the same pattern / which / forbid on the snapshot's line of the same family (`terminal_select`: the why token after
the tag becomes `terminal`, or is inserted - `\[NATIVE_FLUSH\] seg tile segs=` -> `\[NATIVE_FLUSH\] terminal seg tile
segs=`; per-frame `[PM4_COV] F` records are whole-run by themselves), so a terminal `BAD bytes=1` fails even when the
last periodic line said 0 (P2-5). Items and parsers that read a why token as `\w+` / `\S+` take the terminal line as
their last line on a graceful-quit roll; items pinned to `periodic` or `census` keep reading the last periodic line.
No pre-registered item changes.

**Default.** Opt-in today (`-GracefulQuit`, `SR_DRIVE_GRACEFUL_QUIT=1`). The case for making it sr_drive's default once
the smoke roll and a few lane rolls pass: an old exe costs nothing (`no-channel`, immediate fallback), a failed quiesce
costs at most the ack timeout or the hard deadline before the same Stop-Process, and every roll then carries its own
whole-run evidence and a flushed log tail (the last 200 ms of the log are lost to a kill today).

**Tests.** `build/difftest.exe --quit-test` (76 checks: names; the SR_QUIT_*_MS bounds; `snapshotDiff`; the
coordinator's order and every failure path through injected hooks - a thread never acknowledged with every counter
still, a printed field outside the digest moving after the snapshot, a thread waking after it, an empty capture; a
fake quit against real producer threads and the production thread census: an import loop, a doorbell, an exempt
loop, a GuestCaller loop, and smoke1's cases - a render thread holding a critical section across its frame loop
(parked at its next gate after the cut, still holding it, no frame after the cut), a thread blocked acquiring that
lock, a thread blocked in KeWaitForSingleObject and one waiting while holding a lock (both acknowledged; released
after the snapshot, each parks at the import's exit with no guest code run); a registered spinner failing the quit;
the ring cut under the mutex (a holder's doorbell dropped during the drain, parked after the thread cut); every gate;
the real named-event round trip; the production printers re-rendered field-for-field equal; the wiring by source
(lock thunks, callKernel's gates, the doorbell, the ringless producers' cut test); the measured hot-path cost); `powershell -File tools\test_sr_quit.ps1` (45: every outcome
through simulated probes including bad bounds, throwing probes and the receipt for every ending, the live Win32 events
against a stand-in child process, sr_drive / sr_roll / run_recomp_x wiring by AST); `python -m unittest discover -s
tools/perf -p test_terminal_snapshot.py` (19: the exit evidence, the owner rows, the twins). Each Astra round 1
finding has a mutant of its fix that these tests fail (the lane's report lists them).

**Smoke roll** (orchestrator; one game, the slot held). From dev once merged: `powershell -NoProfile -File
tools\sr_roll.ps1 -Owner gq -Tag gq_smoke1 -Scenario tools\scenarios\frontend_long_observe.json -GracefulQuit -Gates
"SR_NATIVE_BRACKETS=1" -TimeoutSeconds 300`, then `python tools/perf/terminal_snapshot.py <out> --require`. Before the
merge, from the lane worktree (no disc beside it, so the read set is staged, 1.8l): `sr_drive.ps1 -Scenario
tools\scenarios\frontend_long_observe.json -Out archive\gq\smoke1 -ExePath <lane exe> -Gates
"SR_INSTANCE_ID=gq,SR_NATIVE_BRACKETS=1" -GracefulQuit -Stage J:\sr_stage -StageSource <dev checkout> -TimeoutSeconds
300`. Expected: the journal line `quit: complete ack=<ms>ms exit=<ms>ms code=0 fellBack=False` and `<attempt>\quit.json`
(result complete, exitCode 0); in run.log `[QUIT] channel ... ready`, `[QUIT] request received`, the four `[QUIT]
stage` lines (`cp-drain: drained rptr=X wptr=X`; `settle: every thread acknowledged ... threads= parked= waiting=
host_idle=`), one `[QUIT] thread ... state=waiting|host-idle` line per thread not parked, the terminal family lines
(`[GPU_CP] terminal`, `[PM4_COV] CUM terminal`, `[PM4_COVMAP] terminal inner=`, `[NATIVE_FLUSH] terminal`,
`[NATIVE_FLUSH] terminal seg closed=`, `[NATIVE_METER_RUN] terminal`, `[AUDIO_NATIVE] terminal`, `[GAUGE] terminal`;
`[PM4_COV] terminal tail` when the drain ended inside a frame) and, as the last [QUIT] line, `[QUIT] terminal snapshot
complete frames=N ... fields=F ...`; `terminal_snapshot.py <out> --require` exits 0 and `owner_census.py <its run.log>
--require-terminal` finds the census terminal with 0 tail frames. Watch: a FAILED line's stage and the thread it
names (`threads-not-quiescent`: a thread blocked in host code outside an import, or a guest spin without imports,
would show here first - the design cannot see those as acknowledged), `counters-moved-after-snapshot` (a host thread
still counting), `untracked=`, `holding_locks=` and `doorbell_drops=` (expected small). smoke1 (2026-10-14, round 1's
exe): ack 11 ms, FAILED settle `threads-not-quiescent` after 3 s - a waiter holding a critical section, and a render
thread holding one that passed every gate and drew a frame after the cut; both fixed in the round-2 rule above.


### 1.10 Havok world snapshot - ours vs Xenia as SETS (collision) [iter-463]

`tools/havok_snapshot.py` reads the Havok 3.2.0 world out of a full-memory dump. The same
code reads a Xenia dump and a dump of our process (tool verification protocol: the two sides
never go through different readers). Built for the building pass-through investigation
(`archive/v1_invest/collision/`: H-A missing bodies / H-F filter / H-B MOPP / memory-failure exit).

```
python tools/havok_snapshot.py snap --image xenia:sound_gameplay            # golden name or a .DMP path
python tools/havok_snapshot.py snap --image ours:archive/<run>/ours.dmp [--base 0x2A00000000]
      [--label NAME] [--near=x,y,z] [--radius 150]    # default near = Superman's proxy position
python tools/havok_snapshot.py diff A.json B.json [--near=a|b|x,y,z] [--radius R]
python tools/test_havok_snapshot.py                    # 24 tests incl. 14 constant mutants + 7 diff-robustness
```
Write `--near=...` with `=` (a negative x otherwise parses as an option). Output:
`archive/v1_invest/collision/snapshots/<label>.json` (full) + `.txt` (summary).

- **What it walks:** `hkWorld = *(*(0x8296F244)+12)`. Fixed island (+0x30), active (+0x38) and
  inactive (+0x44) island arrays, and every entity in each island (+0x3C). Per body: vtable and
  class, name (+0x10), filterInfo (+0x30) decoded as layer (bits 0-4), subsystem (5-9),
  don't-collide subsystem (10-14) and system group (16-31) the way the filter's
  isCollisionEnabled `sub_82578A68` decodes it; shape (+0x14) with class; for MOPP the
  hkMoppCode, its size and a CRC32 of the bytecode; for list shapes (and a MOPP's list child)
  the child count, classes and the multiset of per-child filterInfo words (`child_filter`: the
  narrowphase shape-key filter `sub_82578CB0` tests each child's word, not the body's; all 0 in
  both goldens); the transform (+0x1C), whose translation is the position; the motion (+0x50) class
  (`hkFixedRigidMotion` / `hkKeyframedRigidMotion` ...); the island; the broadphase AABB
  decoded from the hk3AxisSweep node the collidable's handle names. It also records the filter
  (+0x7C, the 32-word table at +0x1C), the phantoms (+0xCC), hkMemory+4 (the memory-failure
  flag), the loop step mode, period and +0x84, hkSimulation+8/+0x10/+0x14, every
  hkCharacterProxy and Superman's.
- **Every offset cites the decompile that reads or writes it.** The table is at the top of the
  tool. Class names come from the image's own hkClass/vtable registry triples in `.data`
  (name*, fn, vt), not from a hand list.
- **Superman's proxy:** hkCharacterProxy (vt triple 0x82072C94/C80/C6C, dtor `sub_825EC390`;
  +0x30 phantom, +0x74 manifold, +0x8C bodies). Its owner is a PhysCharProxyObj (vt 0x82009018,
  +4 = proxy, `PhysCharProxy_Create_sub_821B9A18`), held in the actor's child slot +0x70 (vt
  0x82009798, `ScreenActorCtor8_sub_821D0DD0`). The actor's handler (+0x84) has the FSM name at
  +0x24: `SUPERMANFSM_...` versus `CITIZENFSM_...`. The actor, owner and proxy objects are
  located by a single vtable scan.
- **Structural checks (reported, never hidden):** these vtables must lie in `.rdata` (from the
  image's own PE header): entity, shape, motion and phantom. Entity back-pointers must match:
  world (+8) and island (+0x54). The rotation must be orthonormal, and the position finite and
  inside the +-25000 broadphase border. The hkMoppCode vtable is checked. Every broadphase
  endpoint must point back to its node, and the node to the collidable's handle. The filter
  table is checked for symmetry and diffed against the table the game's filter ctor
  `sub_822C49F8` builds (ported). A failing body keeps its row with a `problems` list, and the
  summary counts them.
- **The diff:** it keys static bodies (fixed island or fixed motion) by (layer, shape class,
  position rounded to 0.1) as multisets. Within a key group, identical signatures pair first
  (the goldens have 11 key groups of 2 co-located bodies). Leftovers re-pair with the NEAREST
  same layer+shape body within 0.25 per axis, so bucket-edge or 1-ulp drift is not a miss.
  Positions are world-space (collidable.m_motion is the motion's world transform; a
  shape-local offset lives inside hkConvexTranslate/hkTransform shapes), so no parent
  composition is needed. The output lists:
  - bodies only in A and only in B (a body without a finite position gets its own bucket
    and is reported, never crashes the diff);
  - matched bodies whose signature differs, per field: filterInfo, name, entity/motion class,
    fixed flag, broadphase type, MOPP CRC/size/child, list count/children, problem count;
  - static<->moving status changes (a building that became a dynamic body is not "missing");
  - bodies with structural problems on each side;
  - moving-body counts, reported separately (cars and people differ between runs by design);
  - filter-table words, global flags, the world lock counts (+0x8C, +0x90), the
    block-pending byte (+0x94) and pending-op count (+0x88) - a stuck lock leaves every
    later addEntity queued, not in the world (H-A) - and Superman's filterInfo, layer,
    group and manifold.
  `--near` restricts the output to bodies whose AABB is within R; matching runs on the full
  sets first, so a pair straddling the radius edge is still a match.
- **Independent verification (2026-10-01, iter-463):** a second agent re-derived the world,
  filter (+0x7C, table +0x1C, row = A's layer, bit = B's layer), filterInfo (collidable+0x1C),
  broadphase (+0x64, handle +0x28), island (+0x30/+0x38/+0x44, entities +0x3C, world +0x1C,
  active +0x28), entity (motion +0x50, island +0x54, fixed +0x91), MOPP (+0x0C/+0x10, code
  +0x20/+0x24), list (+0x10 stride 8, +0x14), proxy (+0x30/+0x74/+0x8C/+0x98), hkMemory+4 and
  hkSimulation offsets from the decompiles without the tool's citations: all agree. Six
  golden bodies hand-read through `golden.py` alone (fixed island entries 1/100/500/1000/1400
  plus active island 0) matched the tool field for field. Planted mutations (layer bits,
  group/subsystem bits, removed body, filter-bit flip, entity and shape vtable garbage, MOPP
  byte, list count, broadphase backref, static->moving, NaN / null transform) are all caught
  by the diff; before the fix the diff crashed on NaN/null positions and missed entity-vtable,
  broadphase-backref and list-count changes.
- **Validation (2026-10-01), no failing bodies in either golden:**

  | | `sound_gameplay` (HUD after the intro) | `nosound` |
  |---|---|---|
  | Bodies | 1642: fixed 1537, moving 105 | 1434: fixed 1334, moving 100 |
  | Layer 7 | 578 = 365 list / 183 MOPP / 30 box (the documented census) | 677 = 463 / 182 / 32 |
  | MOPP bytecode | 818/818 CRC'd | all CRC'd |
  | Filter table | identical to the ctor table in both dumps | |
  | Memory-failure flag, step mode, period, dt | 0, 0, 33333334 ns, 1/30 | same |
  | Superman's proxy | filterInfo 0x00010002 (layer 2, group 1), capsule, in world | same |
  | Other proxies | 60 citizen proxies with filterInfo 0x00000002 | same |

  - `sound_gameplay`: Superman is at (-2267.4, 1178.3, -411.7), standing on the terrain body
    0x022ADF40 (1 manifold point).
  - Diffing the two dumps, the layer-7 set near Superman's `sound_gameplay` position is
    identical: 0 buildings missing either way. Layer-3 props there are loaded only in
    `sound_gameplay`, because bodies stream around the player. The MOPP CRCs of all 544 matched
    static bodies are equal.
  - The `ours:` path was checked on
    `archive/iter458_hr1ph_stop_508c_*.dmp` (8.7 GB, front end):
    - base 0x2A00000000 read from the `g_debugGuestBase` export;
    - self-check passed and the world was found, at 0x85E2BF40, which is the Xenia address
      rebased;
    - that world had only its fixed body.

    First gameplay dumps of ours (2026-10-01, build 9af2245, natural except for sound: the
    then-ungated SR_CFG_SOUND_OFF left the gameconfig Sound flag 0, so they hold no sound heap,
    sound allocators, EntAudioRep or sound Rtam refs, iter-464):
    - `ours_start` (`tools/scenarios/gameplay_idle_capture.json`, no input, [RECSET] quiet 4 min):
      Superman at (-2267.26, 1178.27, -412.36), 0.7 units from the golden. Diff vs
      `xenia_sound_gameplay`, all bodies: 1537/1537 static matched, 0 only-in-either side,
      0 field differences (MOPP CRCs included); filter table, filterInfo 0x00010002, lock and
      pending fields, hkMemory+4, step mode/period/dt all equal; he stands on the same terrain
      body (1 manifold point, normal +Y). H-A and H-F are refuted at the start area.
    - `ours_midtown` (tutorial.inpr replay, meteor tutorial at HUD+205 s): Superman at
      (-2349.2, 1059.9, -840.7) against the terrain (2 manifold points on the L6 'Pristine'
      MOPP bodies). All 90 layer-7 bodies within 500 units are present (golden: 90); the
      227 buildings streamed out are all >= 779 units away. Snapshots and diffs:
      `archive/v1_invest/collision/snapshots/{ours_start,ours_midtown,diff_*}`.
- **Capture procedure, ours** (one game process at a time; no Xenia running):
  1. Run the build to the spot, e.g. MIDTOWN at the pass-through building. Stand still, or hold
     Superman against the wall.
  2. Wait until streaming is quiet: no `[RECSET]` lines for >= 30 s. Bodies stream per load
     unit, so a snapshot taken mid-stream shows false "missing" bodies.
  3. `$p = (Get-Process SupermanReturns).Id` and
     `& "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -pv -p $p -c ".dump /ma /o <repo>\archive\v1_invest\collision\ours_<tag>.dmp; qd"`
     This is noninvasive, and the process is suspended while it writes. The dump is about 9 GB
     (4 GiB guest + 2.2 GB mirror + host), so write it to `archive/` on D:, never to C:.
  4. `python tools/havok_snapshot.py snap --image ours:archive/v1_invest/collision/ours_<tag>.dmp --label ours_<tag>`
     takes about 4 min cold. The base is found from the exe's `g_debugGuestBase` export
     (guest_context.cpp) through the dump's module list and PE export table. If that fails, the
     tool falls back to the harness's fixed preferred base 0x2A00000000
     (`allocGuestMemory`, logged as `Guest harness: allocated at base=`). An explicit `--base`
     is still self-checked. The self-check needs MZ at 0x82000000, the registry string at
     0x829289E8 and an hkWorld vtable. The stale startup mirror at base-4 GB passes the first
     two and fails the third.
- **Capture procedure, Xenia:** go to the same spot in the same state. Take the dump with the
  same cdb line on the xenia_canary pid (`tools/XENIA_HARNESS.md`: detach Frida first; about
  13.5 GB). Then run `snap --image xenia:<path>`.
- **Known limits:**
  - One snapshot is one instant. The world streams, so compare the same spot after streaming is
    quiet.
  - The 0.1 key can split a body that moved by exactly a rounding edge; the 0.25 re-pair covers
    ulp drift, not real moves.
  - The AABB is the broadphase's 16-bit quantized box (about +-0.4 units), not the shape's
    exact extent.
  - Some layouts are data-derived but checked on every image: the node and endpoint layout,
    the broadphase offset and scale, the actor handler+0x24 name, and the manifold
    collidable fields.
  - Shape classes outside the registry print as `unregistered_<vt>`, e.g. the game's terrain
    shape 0x82006AB0.
  - The tool does not walk MOPP trees or list children geometrically, so a body can be
    present with a correct CRC and still fail its queries (H-B). That needs the L6 difftest.
  - A full scan of our 4 GiB guest takes minutes; Xenia dumps take about 25 s.
- **Companion: `tools/recomp_fn_profile.py`** profiles recompiled functions from
  `guest/ppc/ppc_recomp.*.cpp`: the counts of risky instruction classes (VMX compares, estimates,
  fused ops, dot products, conversions, rounding, mftb), known-bug forms (`vcmpgtuh.`/`vcmpgtub.`
  without CR6, `vrfin`), direct callees, indirect calls and jump tables. `--closure --depth N`
  walks the direct-call graph. The Superman-proxy -> building query closure (228 functions,
  23.7k instructions, from `hkCharacterProxy_integrate_sub_825EC5A8` through the MOPP VMs and
  the capsule/convex agents) has **0 known-bug forms** (iter-463).

### 1.10b Collision watch - live pass-through evidence, default ON (iter-463)

The user reported "some buildings didn't have collision" (build 60a90bf, Midtown, flying). It has
never been reproduced on HEAD. `kernel/collision_watch.cpp` puts the evidence in EVERY run's log
(the user's own play is the best reproducer), and `tools/collision_route_analyze.py` finds and
classifies pass-throughs offline.

- **Gate:** `SR_COLLISION_WATCH` is default ON; `=0` turns it off. It is observe-only: 8 strong
  defs, each with one unconditional `__imp__` call, all `observe` in `docs/OVERRIDES.json`.
  Hooks: MoveTowardIdealPos, actor SetPosition/SetTransform, PRXY SetPhysRepMode/Deactivate/
  SetFilterInfo, `hkWorld::addEntity`/`removeEntity`.
- **Fault-free by construction.** Every read goes through `Mem` (`kernel/collision_watch.hpp`).
  A read happens only inside guest `[0x10000, 0xA0000000)`, where `aliasGuestVA` is the
  identity. The watch enables only after a one-time VirtualQuery walk confirms that range is
  committed, readable and has no guard page. It refuses under `SR_PHYS_UNIFY`. The walk is logged
  in the `[COLL_WATCH]` banner.
- **Overhead, measured** on the golden-call harness (ours_midtown image, 100k calls,
  5 interleaved reps, medians):

  | Call | Added cost |
  |---|---|
  | Non-Superman mover call | +0.008 us (noise) |
  | Superman tick | +0.49 to +0.52 us |
  | One `[COLL_PATH]` log line | 9.3 us (difftest B1) |

  Fast flight at 30 Hz therefore costs about 0.3 ms per wall second (0.03 % of a core), and the
  log grows by about 9 KB/s. With the gate at 0 and at 1, the harness outputs are byte-identical
  (fly/mv/gate_midtown), and they match the prep's recomp outputs.
- **Log tags:** all are capped; `audit_log_gating.py` reports CAPPED. Take counts from the `1s`
  summary fields, not from line counts.
  - `[COLL_PATH]`: one line per Superman mover tick above 20 m/s, plus the first slow tick after.
    Fields: path, phantom layer, in/out, velocity, manifold and overlap-list body addresses.
  - `[COLL_BODY]`: the first sighting of a body, with its translation, layer and shape vt.
  - `[COLL_WORLD]`: ADD/REMOVE for every layer-7 body, and for any body within 300 m of Superman.
    Fields: `st=added|queued|removed`, the world lock, distance and speed.
  - `[COLL_MOVER]`: TELEPORT, SETPOS, SETXFORM, SET_FILTERINFO, SET_PHYSREP_MODE, DEACTIVATE,
    NO-INTEGRATE, and the `1s` summary (uncapped counters, `dropped=`, `selfUs=`).
  - `[COLL_ROUTE]`: the prep's fixed-route target lines. Opt-in with `SR_COLL_ROUTE=1` or
    `SR_COLL_ROUTE_TARGET`. `SR_COLLISION_MOVER_WATCH=1` still works as an alias, so the
    collision_route kit commands are unchanged.
- **When the user hits a pass-through:** copy `superman_returns.log` before relaunching, because
  the next launch overwrites it. Then run:
  ```
  python tools/collision_route_analyze.py <copied log> [--json out.json] [--layers 7] [--threshold 4]
  python tools/collision_route_analyze.py --xenia archive/v1_invest/collision/route/xenia_*/xenia_route.jsonl
  python tools/collision_route_analyze.py --build-wall-depth     # per-body wall insets from archive/golden_call_mover
  python tools/test_collision_route_analyze.py                   # 27 tests
  python tools/collision_route_verdict.py ...                    # fixed-route PASS/FAIL (route mode)
  ```
- **What counts as a flag:** the capsule centre is more than 4 m inside a layer-7 AABB, or more
  than (measured wall depth + 1 m) for the 16 bodies the golden-call measured. AABBs come from
  the havok_snapshot JSONs, matched by translation, and the body must not be in the manifold.
  A flag between 4 and 6 m on a body with no measured wall depth is LOW confidence, because
  walls can sit up to about 7 m inside an AABB.
- **Classes, first match wins:**
  1. STREAMING: the body was removed early, added late, or never added.
  2. WRITER: a teleport or physrep change around the flag, or phantom layer != 2.
  3. GATE: the path was not integrate.
  4. BROADPHASE: the body is in the world but not in the overlap list.
  5. MOVER: the body is in the overlap list and the path was integrate. Dump and golden-call that
     tick; only after that is H-D a candidate.
  6. UNKNOWN: anything else.
- **Limits:**
  - Only bodies present in a snapshot have an AABB. The report counts the layer-7 world bodies it
    could not map.
  - Only ticks above 20 m/s are logged.
  - The Xenia side cannot establish "never added", because Frida attaches after the first call.
- **Confirm every flag against the building's own shape before believing it** (iter-463 rolls,
  ENGINE_MAP 6j.1: ALL 30 flags, 17 of them HIGH, were AABB slack or contact - a setback tower
  flagged 31.8 m "deep" had no geometry anywhere on the path):
  ```
  python tools/collision_route_analyze.py <log> --json <dir>/analysis.json
  python tools/collision_flag_rays.py <log> <dir>/analysis.json <dir>   # castRay per travelled tick, golden shapes
  python tools/test_collision_flag_rays.py                              # 8 tests
  ```
  Each travelled tick segment (t0-8..t1+8; probes at the feet +0.05, +0.9 and +1.75 m - the actor
  position is his FEET, ENGINE_MAP 6j.1) is ray-cast against the
  flagged body's shape on the golden Xenia image through the golden-call harness (recomp and the
  clean-room interpreter; `divergent=0` = they agree). Verdicts: CLEAR (no geometry on the path),
  CONTACT (crossed only on ticks with a manifold contact that tick or the next: roof skims,
  landings), GRAZE (crossed with an empty manifold but < 0.5 m past the face and contact within
  +-3 ticks: an edge clipped while sliding off), CONFIRMED (anything deeper / with no contact
  nearby: a real miss - golden-call that tick and run the Xenia route). Exit code 1 on CONFIRMED.
- **Known AABB false-flag sources:** the main-menu background flight is the SAME Superman actor
  on a scripted loop (flags before the `main_hud` APT request are not player flight); setback
  towers (Hull00424 golden 0x022D09C0 is ~30 m narrower per side at 254 m altitude than its
  AABB); list-shape gaps; roof spires (roof skims read 4-8 m "inside" the AABB top); the ground
  slab `Pristine` on landing.
- **Live overhead** (iter-463, 5 rolls, 6005 summary seconds; game thread, wall clock including
  the synchronous logger): mean 0.83 ms per wall second (0.08 % of a core); 137 seconds above
  5 ms; worst 102 ms in one second, while the game was running at 10-20 Hz (logger stalls, not
  watch compute). That is about 3x the harness estimate above. It is still small, but the
  per-second `selfUs` is the number to watch.
- **Rolling it on purpose:** `tools/sr_drive.ps1 -Scenario tools/scenarios/tutorial_tape_full.json
  -Gates "SR_INPUT_REPLAY=<tape>"`. The tape that flies fast at building height is
  `archive/v1_invest/collision/route/dive.inpr`, made by
  `python tools/gen_collision_route_tape.py <out> --mode dive --minutes 9`. The flight controls
  it relies on are in ENGINE_MAP 6j.1. The power-launch `sweep.inpr` climbs above the rooftops,
  and the tap-launch `hop.inpr` only hovers at up to 10 m/s.

### 1.11 objstate - objective / world-progress state from a memory image [iter-463]

`tools/objstate.py` reads the story state out of a memory image with ONE code path for the
Xenia golden and for our own minidumps (the level-skip comparison set, devtools critique B4).
It is offline and observe-only: no guest writes, no guest calls, no exe changes. Every lookup
is a Python port of the retail function that does it, cited by address in the source, so it
reads what the game itself would read. Memory it cannot read, or that fails a structural
check, is reported as a failed check; it is never turned into a plausible number.

```
python tools/objstate.py snap --image xenia:sound_gameplay --label golden_gameplay   # < 1 s
python tools/objstate.py snap --image ours:archive/<full .dump /ma>.dmp --label ours_x
python tools/objstate.py diff archive/v1_devtools/objstate/A.json archive/v1_devtools/objstate/B.json
python tools/test_objstate.py        # 63 tests (synthetic image + 4 golden), incl. 11 code mutants
python tools/objstate.py snap --image xenia:sound_gameplay --ledger L.json   # + touched page ranges
python tools/objstate.py snap --image xenia:sound_gameplay --write-snap G.snap  # + the touched pages
python tools/objstate.py snap --image snap:G.snap          # the same reader on the .snap
python tools/guest_snap.py capture --image xenia:sound_gameplay --out G.snap  # objstate+save_records,
                                     # self-verifying (exit 1 unless the snap reproduces both)
python tools/test_guest_snap.py      # 13 tests: format round trip, corruptions, no-zero rule, mutants
```

- **Images**: the image layer (Xenia dumps, and our dumps with guest-base detection) is
  imported from `tools/havok_snapshot.py` (§1.10). `snap:<file>` (below) is served by the same
  layer. If that module is missing, the fallback
  serves Xenia dumps only, through `tools/golden.py`. `ours:` needs a FULL dump (a
  Memory64ListStream); `archive/v1_tape1/freeze_58272.dmp` is not one.
  - Output: `archive/v1_devtools/objstate/<label>.json|.txt` (schema `objstate/1`).
  - The exit code is 1 when any check fails.
- **What it reads, and the code it ports**
  - The current flow: `FindEntryByNameIn8Slot_sub_821F68E8` on `*0x8296DF74`.
    - Manager: +8 defs, +0x20 eight slots, +0x2C current def.
    - Flow: +0xC name, +0x4C hash31(name), +0x68 DAS handle.
  - The flow's DAS: every node, named through the game's own atom map
    (`AptStringAtomFindValue_sub_826CE950`).
    - Lookups are `AptResolvePathString_sub_826CD770`: FNV-1 (multiply, then xor) of the
      name, then the binary search `SortedPtrVec_FindByKey_sub_821947D8`.
    - A node is {key, value, flags}. Its type is `(flags>>17)&0x7F`: 0 int, 1 float,
      5 bitfield, 6 atom string, 3 vector.
    - The saved subset is the type-bit-16 vector at handle+0x58 (`sub_82181E28`,
      `DasDocHandle_GetNodesByTypeBit_sub_821E6D68`).
  - Progress fields from that DAS:
    - objectiveStatus0..8 and objectiveFound0..8 (`WorldFlow_IsObjectiveStatusBitSet_sub_82224EE0`);
    - currentHeroPointChapter, heroPointChapterNNRequirements, GoToNextChapter;
    - triggersAvailable, waitingForLoad, objectiveActivated, triggerEngaged, and the engage
      scratch attributes;
    - MaxMetropolisHealth, MaxMetroHealthBoost, ME01..ME50Weight, and
      min/maxTimeBetweenTriggerSpawns.
  - Objective manager bytes at `*0x8296DF5C`: +0 replay, +1 quit-to-FE (read by the
    availability check), +2 engage, +4 mini-events.
  - The metro object `*0x8296DF50`: +4 health (u32), +8 max, +0x10 percent
    (`ScriptCmd_GetMetropolisHealth_sub_821E86B8`, `ScriptCmd_SetMaxMetropolisHealth_sub_821F9730`).
  - Stats `*0x8296DF70` {elems, count}: name at +0x1C, flags byte at +0x22 (bit 4 = saved,
    `SaveRecords_WriteSavedStats_sub_82165198`), value through vt+40.
    - Every retail stat class shares the getter 0x8223FD70 = `lfd f1,8(r3); blr`.
    - The tool decodes the getter from the image and refuses any other shape.
  - Powerups `*0x8296DF6C` {+4 elems, +8 count}: name at +8, and **byte +0x23 = purchased**.
    - That byte is the save's "entity byte +35" (`SaveRecords_WritePowerupBytes_sub_82165250`,
      `ScriptCmd_IsPowerupPurchased_sub_82203F50`).
    - Byte +0x22 (the level within the family) is [data].
  - The save projection: what `SaveMgr_CaptureAutoSave` (`sub_8218E3A8`) would write now.
    - It covers the saved stats, the powerup bytes, the persistent DAS nodes (value in the
      high word) and the metro record: `max_u32(health, fctidz(f32(max) x f32 bits of the
      current flow's MetroHealthSaveThreshold))` (`SaveRecords_WriteMetroHealth_sub_82186F70`,
      `sub_82182020`, `AptResolveObjectPathValue_sub_82177660`; 0.2 on every golden, so
      25000/25000 saves 25000). `diff` compares all four.
    - objectiveStatus4 is repacked as `PackObjectiveStatusBits_sub_82165638` does it: bits
      141/146/148 mirror objectives 34/12/44.
    - The options block is not included.
  - The OBJ|TRIGGERS members, found the way the game finds them
    (`Objective_FindTriggerActrByIndex_sub_822102F0`):
    - `AptBuildMaskFromNameList_sub_821B3C18` + `HashTableLookup_sub_821DC218`, hashing with
      `sub_8263C550` (signed chars);
    - `FindTriggerFilterPair_sub_821CC160`: FNV-1 over the 32-byte mask, then a full 32-byte
      compare;
    - the member's vt+36('ACTR'). The getter 0x821E4B78 is decoded from the image; it returns
      the member itself for ACTR and for GAME.
  - Per trigger, the tool reads:
    - its DAS (actor+0x80);
    - its group membership bits (actor+0x30), including ENGAGED;
    - its FSM state, through actor+0x84 component (whose +0x3C is the actor), then the +0x1C
      machine, then *machine. Those three offsets are [data], so the tool guards them. The
      state's vtable must be 0x82017790, whose slots are `TriggerState_EnterAvailability_sub_822737D0`
      and `TriggerState_UpdateEngage_sub_82273B40`, and state +0x14 must equal triggerDistance.
      State +4 bit 0 = disabled (unavailable or engaged).
    - its TriggerGate power, when the actor has a power manager (+0x68). The golden's triggers
      have none.
  - **The flow FSMs (phase 1b, iter-463).** Every loaded flow's FSM is `flow+0x6C`, ticked
    through vt[1] by `WorldFlow_Update_sub_82225E60`; class vt `0x8200E5E4` ([data], its
    vt[1] = `FlowFsm_Tick_sub_82293108` is fingerprinted).
    - fsm +4 start index, **+8 current index** (-1 = not running), +0xC previous index
      (`FsmSetState_sub_8278C1C0` stores both), +0x18 count, +0x1C state* array.
    - State +8 transition count, +0xC transition* array; a transition's +4 is the destination
      INDEX (`FsmWalkTransitions_sub_8278C448`).
    - Index -> state id: the parse (`FsmParseElementCb_sub_82293398`) pushes each `<state>`
      id and gives it index count-1, so the index is the document order of the graph's
      `<state>` elements, which `tools/worldflow_fsm.py` decodes. The id vector (fsm+0x30)
      is gone after the parse, so the mapping is **proven on each image** instead: every live
      state's transition list must equal the decoded graph's, ids mapped to indices (44/44 on
      all three goldens). Without a proven mapping the tool reports the index only.
    - `--ast` (default: retail `baseworl.AST`) supplies the BaseWorldFlow graph; objective
      flows (`objtiv.AST`) are not decoded, so their FSMs report indices only.
  - **The CurrentXP cap.** A stat's cap is decoded from its class's SetStatCap slot, vt+20,
    the call target of `ScriptCmd_SetStatCap_sub_821F2688`.
    - CurrentXP is the only retail stat class with a cap (vt `0x8200E900`; its vt+20 is
      `0x821F1288`: `lfd f0,8(r3); stfd f1,0x28(r3); fcmpu; blelr; stfd f1,8(r3)`, so the cap
      is the f64 at **+0x28** and setting it lowers a larger value to it). Every other class
      has `blr` there.
    - Its Increment/Decrement/Add/Sub slots (vt+4..+16, `0x82204400..0x822044A0`) add, then
      clamp the value at +0x28, gated by flags bit 0 (active). A second check confirms the
      AddToStat slot clamps at the decoded offset.
    - `SetStatValueByName` (`ScriptCmd_SetStatValueByName_sub_821F1E88`) calls vt+32 =
      `0x8223FD58` = `stfd f1,8(r3); blr` for every class: a plain store, no clamp, no
      notify, no flag gate. This settles the store semantics DEVTOOLS_DESIGN 5.3 asked for.
    - Triangulation with the game's own Lua: BaseWorldFlow state 105 always ends with
      `SetStatCap("CurrentXP", heroPointChapter<current+1>Requirements)`, so a nonzero cap
      must equal that requirement. Gameplay golden: cap 7500 = chapter 2's requirement;
      press-start goldens: cap 0 (state 105 has not run).
  - **The trigger's input event.** Trigger FSM state +0x18 is linked to `triggerEvent`: the
    enter action stores `sub_8229A568(triggerEvent)` when the string is non-empty, else 0
    (r23). `sub_8229A568` hashes the name with FNV-1 (`sub_825F43E0`) and walks the 191-row
    table `{id, name*, FNV-1}` at `0x82912B68` (the `INPUT_EVENT_*` names, 166 rows before the
    id-0 terminator). The tool ports it and checks state+0x18 against the attribute on every
    live trigger (both golden triggers: empty, 0).
  - The **availability verdict**: `availability()` is a pure port of the verdict part of
    `sub_822737D0`. It runs on every live trigger and on the static trigger docs
    (`archive/v1_devtools/{baseworl,metropol}_das_docs.json`: 59 docs parsed from the AST
    files). These semantics are carried over exactly:
    - The dependency tokenizer (`Str_NextIntToken_sub_8224DF00` + `Str_AtoiLenient_sub_825F5380`)
      stops at a comma at the string start and reads an empty token as 0.
    - The XP compare is unsigned: fctidz, then the low 32 bits.
    - The requirement is read by the current flow's NAME (`WorldFlow_GetDasIntByFlowName_sub_82218CC8`).
    - The CurrentXP stat is found by a strncmp PREFIX match (`FindElementByNameHash_sub_821F16B8`).
    - Metro health is loaded as a u32 (lwz/std/fcfid), so a "negative" health never counts as
      <= 0. The threshold is flt_820013B8 = 0.0.
    - Where the game's own code would be undefined, the verdict is `None`: a null CurrentXP
      stat, or no current flow.
- **Checks run on every image**
  - the retail-code fingerprint (vtable slots and function prologues);
  - the DAS containers are sorted (the binary-search precondition);
  - every DAS key resolves to an atom whose FNV-1 equals the key;
  - the persistent vector equals the nodes flagged 0x10000000;
  - the ported bit test agrees with the raw status words;
  - the flow name hashes;
  - metro max equals the DAS MaxMetropolisHealth;
  - each trigger FSM's distance equals its triggerDistance attribute, and its +0x18 equals
    `sub_8229A568(triggerEvent)`;
  - each flow FSM has the flow-FSM vtable and an index in [-1, count); for a decoded graph,
    every live transition list equals the graph and the start index is the graph's start
    state;
  - the CurrentXP cap equals the next chapter's requirement (when nonzero), and the AddToStat
    slot clamps at the cap's offset;
  - **triangulation**: every live trigger judged unavailable must carry the game's own
    `objectiveAvailable == 0`, which sub_822737D0 writes (it never writes 1).
- **Validation (2026-10-01): no check failed on any image**

  | image | checks | state |
  |---|---|---|
  | golden `sound_gameplay` | 34 (46 with phase 1b) | Metropolis, MSTobj01 running; BaseWorldFlow FSM index 3 = state **107** (InObjective), previous 39 = 2023575322 |
  | golden `sound_menu` | 26 (33) | press start; FSM index 17 = state 1793378647, previous 31 (attract) |
  | golden `nosound` | 26 (33) | press start; FSM index 17, previous 13 |
  | ours `archive/iter458_hr1ph_stop_...81d0.dmp` (8.7 GB) | 24 | iter-458 menu era (not re-run with phase 1b) |

  - The gameplay golden:
    - flows BaseWorldFlow, MetropolisWorldFlow, SupermenActorWorldFlow,
      MeteorStormParentWorldFlow and MSTobj01; chapter 1/20;
    - won {147}, found {0, 147};
    - triggersAvailable 1, objectiveActivated 1, objectiveWorldFlowName MSTobj01;
    - CurrentXP 0; metro 25000/25000; chapter requirements 0, 0, 7500, 19000, ... 5000000.
  - Its two live triggers:
    - idx 0 MSTobj01: available, in ENGAGED, FSM bit 0 set (it is the running objective);
    - idx 127 CinematicMxyOutro: unavailable, because dependencies 22 and 11 are not won and
      CurrentXP 0 < 140000 (chapter 9). The game's own objectiveAvailable is 0, so the two agree.
  - Against the static docs, objectives 0 and 120 (x2) are available.
  - `diff` menu -> gameplay shows:
    - 147 won;
    - objective 0 becomes available and 147 becomes unavailable;
    - the loaded flows change;
    - videosUnlocked goes 0 -> 6.
- **Findings from building it**
  - `sub_82186F70` writes metro health, not play time. It is renamed in IDA to
    `SaveRecords_WriteMetroHealth`. Play time is the saved stat TotalPlayTime.
  - PreviousXP has flags 1, so it is NOT saved; only CurrentXP is.
  - The press-start goldens already hold the full base-flow state, objective manager
    included. So the critique's C3 guess that it is null there is wrong. It is still
    pre-game state: chapter 1, nothing won.
- **Protocol status** (`memory/feedback_tool_verification_protocol.md`)
  - Step 1 (port, then a differential) is partial.
    - The lookups are ports of the retail functions, not re-inventions.
    - A live differential against the guest getters is impossible offline.
    - The substitutes are the triangulations above and the 29 synthetic tests. The tests
      include 6 code mutants and 5 verdict-clause mutants, and every mutant is killed.
  - Step 3 (dev/retail layout) is done for the READ side (independent verifier, below): the
    20 retail functions the reader ports are address-normalized identical to their dev twins.
    The save writers differ from dev only in the save-manager object layout, not in the
    object fields they read.
  - Step 4 (triangulation) is done for availability, metro max, the persistent set and the FSM,
    and (phase 1b) for the flow-FSM index (live transition lists = the decoded graph) and the
    CurrentXP cap (= the state-105 Lua's requirement).
  - Step 5 (oracle): the Xenia side is done. Our side needs a full dump of our build at the
    same scene.
  - Step 6 (observe-only, cost) holds: offline, about 30k reads, under 1 s.
  - Until step 2 passes, every field other than the verdict and the triangulated ones is
    "our artifacts" evidence.
- **Independent verification (iter-463, a second agent; scripts not committed, results here)**
  - Six offset groups re-derived from IDA without the tool's citations (stats list and
    +0x1C/+0x22/vt+0x28 from `sub_82165198`/`sub_821F1858`/`sub_821F17E0`; powerups
    +4/+8/+8/+0x23 from `sub_82165250`/`sub_821F08E8`/`sub_821F0DE0`; metro +4/+8 from
    `sub_82186F70`; flow manager +8/+0x20/+0x2C and flow +0xC/+0x4C/+0x68 from
    `sub_821F68E8`/`sub_821F6850`/`sub_822101B0`; trigger state +4/+0x14/+0x18/+0x20 and
    actor +0x80 from `sub_822737D0`; objective manager +1): all equal the tool's constants.
  - Five values hand-read from the `sound_gameplay` golden with `golden.py read` hops and a
    linear (not binary-search) DAS scan: metro 25000/25000, objectiveStatus4 0x80000
    (bit 147), objectiveFound0/4 = 1/0x80000, chapter 1/20 with triggersAvailable 1 and
    objectiveActivated 1, CurrentXP 0.0 (flags 7) and PreviousXP (flags 1), the five
    LevelOne powerups purchased. All equal the tool's output.
  - 17 planted image mutations (u32 health 0x80000000, dependency "-1", ",2", "2,", 288,
    repeatable 2, chapter requirement -3 and 99, quit byte 0x80, unloaded or switched current
    flow, XP 1e10 / 2^32+5 / -1.0, repack pairs, non-int persistent node) all behave as the
    disassembly says. One crashed the tool (a non-int MaxMetropolisHealth node raised
    TypeError out of `metro()`); fixed, it is now a failed check. 8 verifier code mutants:
    4 survived the 29-test suite (requirement read from BaseWorldFlow instead of the current
    flow, `triggerIsRepeatable != 1`, unsigned chapter-required test, swapped repack pairs);
    the added `VerifierRuleTest` class kills all 8.
  - Read ledger: the gameplay golden touches 166 guest pages (95 ranges, 664 KiB). Heap
    addresses differ run to run, so a ledger from one image does not transfer to another run
    (DEVTOOLS_DESIGN 6.2 corrected).
- **`snap:` images (DEVTOOLS_DESIGN 6.2 step 5; `tools/guest_snap.py`, iter-463 phase 1b)**
  - A `.snap` holds only the guest pages a reader touched, in guest VAs (the E-view +0x1000
    host offset is applied by the source image). Format: magic `SRSNAP\0\1`, a JSON meta block
    (schema `guest_snap/1`, source `describe()`, label, missing pages), a strictly increasing
    page table {page, valid length}, the page bytes, a CRC-32. Partial pages keep the length
    the source returned, so reads stop exactly where they stopped on the source.
  - It is served by `havok_snapshot.open_image` (`SnapImage` subclasses its `Image`), so
    `objstate.py`, `save_records.py` and `objective_table.py` all accept `snap:<file>`
    unchanged. The objstate fallback also accepts it.
  - **No-zero rule:** a read outside the captured pages comes back SHORT; objstate turns it
    into a failed check. `SnapImage.misses` counts such reads.
  - Two writers: `objstate.py snap --write-snap` (objstate's own ledger) and
    `guest_snap.py capture` (runs objstate + save_records through a page-recording wrapper,
    writes the union, re-runs both on the snap and exits 1 unless both outputs are identical
    and no read fell outside).
  - Results (archive/v1_devtools/snap/): gameplay 256 pages / 1.05 MB for both readers
    (objstate alone: 175 pages, 719 KB); sound_menu 690 KB; nosound 595 KB. On all three,
    both readers on the snap equal the full golden, 0 reads outside, and `objstate diff`
    golden vs snap reports no differences.
  - Scanning readers (havok_snapshot's class map uses `iter_chunks` over the whole space)
    are out of scope: the recording wrapper passes scans through unrecorded.
- **Limits**
  - Only spawned triggers have a live DAS. Every other objective is judged from the static
    docs (source `static:`), which assumes the runtime attributes equal the asset docs.
    Check that assumption on the spawned triggers by comparing their `attrs` with the docs.
  - The static docs live in the gitignored `archive/`. Without them the tool evaluates the
    live triggers only.

### 1.12 Level skip: planner, command language, tagged runs [iter-463]

Design: `docs/DEVTOOLS_DESIGN.md` sections 3.3, 5.3, 5.4 and 6. Every run is
**dev-tool-assisted**; `verify_run.py` refuses its frames (exit 3).

```
python tools/devtools/skipplan.py plan --target 5 --image xenia:sound_gameplay --out P.txt --json P.json
python tools/devtools/skipplan.py plan --target chapter:4 --snapshot S.json --carrier auto   # see the carrier note
python tools/devtools/skipplan.py parse P.txt --expand          # validate; canonical skipcmds/1 JSON
python tools/devtools/skip_run.py run --side xenia --plan P.txt --dry-run   # the tagged dir + commands, nothing launched
python tools/devtools/skip_run.py collect archive/v1_devtools/skip/devtool_<stamp>_xenia
python tools/devtools/skip_run.py compare --oracle <xenia run> --ours <ours run> [<ours run> ...]
```

- **Planner.** Target = objective index or `chapter:N`; inputs = the committed
  `data/devtools/objective_table_retail.tsv` + `objective_flows_retail.tsv` and an objstate
  snapshot (`--snapshot` JSON, or `--image` to read one the way `objstate.py snap` does). The
  plan has explicit guards on every step and a `snapshot` step at every settle point. Exit codes:
  0 plan, 2 refused (the reason is printed), 3 truncated by `--xp-policy stop`.
- **Golden result (after live run 1, open item 15).** From `sound_gameplay` (MSTobj01 running, 147
  won), target 5 gives the auto-engage chain with no forced write and no `reach`:
  - `abort_when objective_failed`;
  - snapshot P0, then `pass 0`;
  - snapshot MSTobj02 engaged (after `won 0` and `won 144`), then `pass 1`;
  - snapshot MSTobj03 engaged, then `pass 2` and `pad DOWN A` (guarded by `won 2`);
  - snapshot WARobj01 engaged (after `won 143`).

  Every step after P0 has an active-tick `timeout` and a wall-clock `deadline`. The artifact is
  `archive/v1_devtools/phase2/warworld_from_golden_autoengage.txt`. Triggers with `triggerDistance
  >= 20000` (a new table column) engage by themselves and get no `reach`; DEVTOOLS_DESIGN 5.3
  "Revised after live run 1" has the details. The save-prompt answer (`DOWN A` = Continue Without Saving;
  B does nothing there) is decoded from the image by
  `python tools/devtools/save_prompt.py [--image xenia:<golden>] [--json]`. It also reports
  whether the prompt blocks the flow (it does: no hooked tick runs while it is up). Tests:
  `tools/devtools/test_save_prompt.py` (DEVTOOLS_DESIGN 5.3). `chapter:4` is refused without `--carrier auto` (the XP
  gap needs a `105` pass in chapter 3 that no story step provides; DEVTOOLS_DESIGN 5.3 "Built").
- **Runs.** `skip_run.py run` makes `archive/v1_devtools/skip/devtool_<stamp>_<side>/` with
  `RUN_CLASS.json`, `plan.txt`, `plan.json` (+ `plan_projection.json` when `P.json` sits next to
  `P.txt`) before any launch; ours reaches P0 through `sr_drive.ps1` (`natural_new.json`,
  `-KeepAlive`, `SR_MODS=0,SR_DEVTOOLS=0`). The executor (phase 1b item 4) attaches only when
  `check_sidecar` passes. `collect` runs the unchanged `objstate.py snap` / `save_records.py
  from-image` over every `snap:<label>.snap`; `compare` reports the first divergence per run,
  excluded stats separately, and the planner-projection model check.
- **Tests.** `tools/devtools/test_skipplan.py` (48, incl. 13 planted planner mutants, the live
  golden, the tie-break and the auto-engage cases), `tools/devtools/test_skip_run.py` (13, incl. a real `.snap` collect), and
  `tools/test_verify_run.py` (7). The command-language corpus shared with the JS / C++ parsers:
  `tools/devtools/testdata/cmdlang_corpus.json`.
- **Executor (phase 1b item 4).** `tools/devtools/xenia_skip.py` loads
  `tools/devtools/frida_skip.js` into the target (needs the `frida` module: `py -3.13`;
  `skip_run.py` picks it). Reference semantics: `tools/devtools/skipexec.py` (ticks, `stable`,
  timeouts, the reach restore, `await_natural` phases, `pad_prompt` = run state 4, refusal
  codes: DEVTOOLS_DESIGN 5.5 "Built"). `tools/devtools/jshost.py` runs the JS offline under node
  or Frida's QuickJS over any image. Tests: `py -3.13 tools/devtools/test_frida_skip.py` (30, incl. the idle-pad path, the hash ports, 30 JS mutants, and `LiveRun1Plan` on live run 1's `.snap` files when `archive/v1_devtools/skip/live1` or `SR_LIVE1_DIR` has them).
- **A live run (Xenia side; nothing else may hold the Xenia slot).** Live run 1 was done
  (DEVTOOLS_DESIGN 6.7): the old planner's Warworld plan stalled at k=2. The tools for open item 15
  are now built: the planner emits the auto-engage shape, and the executor has `deadline`, `poll`,
  `abort_when` and active-tick timeouts. The uninterrupted P0 -> Warworld run with the
  regenerated plan has not been done yet. A step with a `deadline` is bounded by it, not by
  `--stall-seconds`.
  0. Unattended: mute first with `powershell -File tools/audio_endpoint.ps1 mute -State
     <run dir>/audio_endpoint_state.json`, and afterwards run `... restore -State <same>`. It uses
     Core Audio, needs no admin, and refuses to record twice. Also pass `--mute=true` to Xenia on
     the command line, which zeroes only the voice volume, so timing is unchanged. Never edit the
     user's config.
  1. `python tools/xenia_pad.py` (background, before Xenia); launch `xenia_canary.exe` on
     `Superman Returns (USA).iso` with `--log_file=archive/v1_devtools/skip/xenia_<stamp>.log`;
     at PRESS START (~110 s) `@START`, then `DOWN A` (no profile), then the opening cinematic
     (about 75 s). The orbiting Superman scene after it **is already gameplay** with the idle
     camera: `A` does nothing there. `START` shows the pause menu, and `A` (Resume) shows
     Metropolis, the HUD and MSTobj01 running (XENIA_HARNESS.md).
     - To watch from outside the executor:
       `py -3.13 tools/devtools/xenia_peek.py --pid <pid> --brief [--json f]`. It is read-only,
       uses its own Frida session, and is non-atomic, so it is a diagnostic and never a settle
       point.
  2. Regenerate the plan first (an archived plan can predate a planner fix; the ones made
     before item 7 answered the save prompt with `pad B`, which does nothing):
     `py -3.13 tools/devtools/skipplan.py plan --target 5 --image xenia:sound_gameplay --out archive/v1_devtools/skip/plans/warworld_from_golden.txt --json archive/v1_devtools/skip/plans/warworld_from_golden.json`
     (check it has `pad DOWN A`), then
     `py -3.13 tools/devtools/skip_run.py run --side xenia --plan archive/v1_devtools/skip/plans/warworld_from_golden.txt --max-minutes 30`
     (one `xenia_canary.exe` running, or `--pid`). It tags the run directory, attaches, and
     runs `collect` at the end. `--max-minutes` matters: a `pass` after a `reach` and the settle
     snapshots have no timeout, so a reach that never engages waits for it (DEVTOOLS_DESIGN
     open item 14, which lists the other first-run risks).
  3. Expect in `run.log`: `init {'ok': True, 'membase': '0x100000000'}`, `hooked active`, then
     `step 0 snapshot ... P0` about 30 active ticks after attach, `snapshot P0: ok`, the pass of
     MSTobj01, and each later step; `steps.jsonl` carries every write (VA, old, new).
  4. Abort (Ctrl-C, or it aborts itself): a `refused` / `timeout` event (exit 2, the plan
     stops there), `stall` (no tick for 300 s), `max_minutes` (240), a snapshot that fails its
     self-check, or Xenia visibly broken. A pending reach poke is restored at shutdown.
  5. Then `python tools/devtools/skip_run.py compare --oracle <that run> --ours <ours run>` once
     an ours run exists (our build must reach P0 first).
- **Protocol status.** Live on the Xenia side (live run 1, DEVTOOLS_DESIGN 6.7):
  - verified live: the hooks, the blocking snapshots (readers equal on the .snap, 0 reads
    outside) and the pass stores; P0 equals the golden twice;
  - the chain to WARobj01 engaged matched the planner's projection;
  - the planner's plan itself failed from a design error (open item 15);
  - the X/O comparison (step 5) waits for our build to reach P0; the carrier assumption is
    open (open item 10).

### 1.13 In-exe dev tools: the gate, the kernel level-skip executor, `[OBJSTATE]` [iter-463]

User guide: `docs/DEVTOOLS.md`. Design: `docs/DEVTOOLS_DESIGN.md` sections 1-3 and 10 ("Built").
Registry: `docs/DEVTOOLS.json` (= `kernel/devtools.cpp` `kTools[]`, audited). Never on by
default; a run with a devtool-class tool is **dev-tool-assisted** and is never baseline evidence.

```
# switch on (an automated roll, SR_INSTANCE_ID set, ignores <content root>/dev.txt)
SR_DEVTOOLS=objstate                                   # observe: [OBJSTATE] dumps, run = natural+hud
SR_DEVTOOLS=levelskip SR_DEVTOOLS_CMDS=P.txt SR_DEVTOOLS_OUT=<dir>   # the kernel executor
# the sanctioned ours-side run (tagged devtool_* dir, sr_drive to P0, waits, kernel_skip_check)
python tools/devtools/skip_run.py run --side ours --executor kernel --plan P.txt --max-minutes 30
python tools/devtools/kernel_skip_check.py <SR_DEVTOOLS_OUT dir> [--plan P.txt]   # vs skipexec.py on the run's own bytes
# tests (all must say 0 failed / OK)
build/difftest.exe --devtools-test            # gate grammar, predicates, tags, closed set
build/difftest.exe --xam-content-test         # TDV a-f: save taint through the real XamContent wrappers
py -3.13 tools/devtools/test_skipcore_cpp.py  # C++ core == skipplan.parse / skipexec (corpus, fuzz, goldens, live run 1, mutants)
python scripts/test_audit_devtools.py         # the DEVTOOL audit rules, planted violations
```

- **The executor** (`kernel/devtools_levelskip.cpp`) is a strong def of `sub_8223EB08` (writes)
  plus `sub_8223ECC0` / `sub_8223F230` (observe only), each an exact pass-through when
  `devToolHookNeeded_sub_X()` is false (`docs/OVERRIDES.json` class `devtool`). Semantics are
  `kernel/devtools_skip_core.cpp`, the third implementation of `skipexec.py`. Reads go through
  `hostRangeReadableCached`, each write through a fresh `VirtualQuery`; no `__try`.
- **Outputs** in `SR_DEVTOOLS_OUT`: `steps.jsonl`, `<label>.snap/.report.json` per snapshot step,
  `k<k>_<prim>_<event>.snap/.event.json` per write (the pre-write pages that tick read),
  `RUN_CLASS.json`. Log tags (uncapped, `audit_log_gating.py` registered): `[RUN_CLASS]`,
  `[DEVTOOL]`, `[OBJSTATE]`; every `[GAUGE]` line ends `devtools=<mask> devtoolActs=<n>`.
- **Pad:** the exe has a virtual pad (`devToolsVirtualPadButtons`, ORed into
  `stub_XamInputGetState`'s report; 0 unless levelskip queued a press). The prompt is seen by the
  idle-pad path, `Executor::idlePad` (the third twin of `skipexec.Executor.idle_pad` and
  `frida_skip.js` `Executor.prototype.idlePad`; 5 idle scenarios + 3 mutants per side in the
  differentials), called from the 10 s `[GAUGE]` poll when no tick ran since the previous poll.
- **Limits:** `play_objective` refuses `unsupported`; a `deadline` is also checked by the 10 s
  `[GAUGE]` thread, so it can fire up to 10 s late while no tick runs; `reach` is unproven live.
- **X/O comparison:** `py -3.13 tools/devtools/xo_report_diff.py --oracle <xenia run> --ours <kernel run>`:
  our `<label>.report.json` against `skipexec.report` on Xenia's `.snap`, heap addresses dropped
  (the kernel `.snap` holds only the report's pages, so `skip_run.py compare`'s objstate read is
  INCOMPLETE on it). Fields the oracle snap cannot read are UNVERIFIED. Tests:
  `test_xo_report_diff.py` (6 mutants).
- **Protocol status:** offline differential done (parser, lookups, write lists, step machine,
  idle pad); live run 2 (DEVTOOLS_DESIGN 6.8): the Warworld plan from a new game, 3/3 rolls on our
  build, `kernel_skip_check.py` 7/7 each, EQUAL to the uninterrupted Xenia run at P0, S01, S02, S03;
  the virtual pad answered the MSTobj03 save prompt 3/3. Not exercised live: `deadline` /
  `abort_when` firing, `reach`, a signed-in Xenia prompt.

### 1.14 Native N0 instruments: packet counter, XDK brackets, frame split, conformance [iter-464]

Plan: `docs/NATIVE_ROADMAP.md` N0; results: ENGINE_MAP §6d "NATIVE N0". All observe-only and
default OFF; the replays stay md5-identical with every gate on.

```
SR_PM4_COVERAGE=1        # [PM4_COV] F per frame + CUM: every PM4 packet the CP interprets (THE native metric)
SR_NATIVE_BRACKETS=1     # + owner of every packet (XDK entry bracket) -> [PM4_COVMAP], [PM4_COV_UNSTAMPED], [NATIVE_BRACKETS]
SR_NATIVE_FRAMESPLIT=1   # [NATIVE_FRAMESPLIT] WIN/CUM/THREAD: XDK vs fence-wait vs kernel-import vs rest per presented frame
python tools/pm4_coverage_report.py <log> --tag T                 # -> archive/native_cov/T.{json,tsv}
python tools/native_replay_check.py --exe-a A.exe --exe-b B.exe --tag T [--gates-b "..."] [--traces menu,...,gp1,lg1]
python scripts/gen_native_brackets.py                             # after editing the entry TSVs or an XDK-range override
cmake --build build/ --target xenos_conformance && build/tests/xenos_conformance/xenos_conformance.exe --verify
python tools/native_census_memexport.py ; python tools/native_pwl_vs_srgb.py
```

- Read COUNTER VALUES from the last `[PM4_COV] CUM` / `[NATIVE_FRAMESPLIT] CUM` line; never count
  lines (CUM lines are periodic; F lines carry `frame=` indices, the report checks gaps).
- A census frame ends at every present the CP applies: at the `XE_SWAP` packet, or (native/composed-census,
  2026-10-15) at a present with no packet - `SR_NATIVE_SWAP_BLOCK`'s kRkSwap record (`pm4cov::presented`, from
  `Pm4Engine::applySwap`). Every report (CUM, `[PM4_COVMAP]`, `[NATIVE_BRACKETS]`) is printed from the frame end, so
  before that fix a composed roll counted packets and printed nothing (tcmp2 census1: 5,640 CP presents, zero lines).
  Positive witnesses: `[PM4_COV] armed brackets=N ...` once at the counter's first hook (again as `armed terminal` in
  the graceful-quit snapshot) and `[PM4_COV] ENDS <why> frames= xeSwap= present= presents=` after every CUM. A census
  is usable only when (`tools/perf/transport_census.py` COVERAGE WITNESS) its present frame ends (F lines less terminal
  tail frames) EQUAL the CP's own `[GPU_CP] <why> ... swaps=N` count at every such line, every ENDS line accounts for
  its frame ends (one pending XE_SWAP present at most; an exe without ENDS only with SWAP_BLOCK off), and the graded
  interval ends at the last report a reconciled CP sample follows; else DARK_CENSUS / NO_CENSUS, exit 2 - silence is
  never 0.
- A new strong-def of an XDK-range function that is in `data/native/xdk_entry_classes.tsv` /
  `xdk_table_setters.tsv` must start with `SR_NATIVE_SCOPE(0xVA);` (or `SR_NATIVE_SCOPE_SYM(sub_X)`
  inside a macro); `gen_native_brackets.py` fails and lists any that do not.
- The replay has no XDK calls: there every packet is UNSTAMPED by construction; brackets need a
  live roll.

### 1.15 Audio A/B against live Xenia, silently [iter-467 sound lane]

Both sides record the SAME capture point - every mixed frame the game hands to
`XAudioSubmitRenderDriverFrame` (256 samples x 6 ch planar big-endian float, 48 kHz) - in one
record format (`AFR1`: u32 magic, u32 seq, u64 epoch us, 6144 B frame). Nothing reaches the
owner's speakers on either side.

```
# Xenia oracle (hold the game slot; owner's build + config: xma_decoder new, dedicated thread)
py -3.11 tools/xenia_pad_tape.py --tape <t.inpr> --anchor-file <X>/anchor.txt --cmd-file <X>/pad_cmd.txt   # optional pad (start FIRST)
py -3.13 tools/xenia_audio_capture.py --out <X> --seconds 600 --extra=--content_root=<X>/content
#   -> <X>/xenia.afr, markers.jsonl (screens/* AptLoadByPath = era markers), anchor.txt on main_hud
#   front end without a profile: PRESS START ~110 s -> @START, then DOWN A -> main_hud at once
# ours (sr_drive; null sink = wall-clock consumer, or -Sink sdl -Device "Steam Streaming Speakers")
powershell -File tools/audio_roll.ps1 -Out <O> [-Scenario tools/scenarios/audio_intro.json -Tape none] [-Gates "SR_LOG=player"]
#   -> <O>_cap/ours.afr (SR_AUDIO_FRAMES), ours.wav (SR_AUDIO_WAV), roll.txt
python tools/audio_oracle_compare.py compare --oracle <X>/xenia.afr --ours <O>_cap/ours.afr [--from S --to S --win 4 --hop 2]
python tools/audio_oracle_compare.py info <afr>            # pacing gaps, level, clicks, seams, zero-frame dropouts, channels
python tools/audio_oracle_compare.py eras <afr> <markers.jsonl | run.log>
python tools/audio_oracle_compare.py spec <X>.afr --vs <O>.afr --from 0 1.0 --to 30 --png f.png
python tools/audio_roll_health.py <run.log>                 # cumulative counters only (AUDIO_OUT_SUM, XMA_SUM, RWAC_Q...)
# second level + front-end SFX oracle: bash tools/xenia_audio_warworld.sh <X> <xenia plan> (Warworld via skip_run,
# the pause menu START DOWN DOWN UP UP A -> <X>/pad_times.txt); the sound each press makes:
# ours = tools/scenarios/audio_pausemenu.json through audio_roll.ps1
python tools/audio_events.py compare --oracle <X>/xenia.afr --xenia-pad <X>/pad_times.txt --ours <O>_cap/ours.afr --sr-drive <O>_stdout.txt
python tools/audio_channels.py --cmp <dir>/compare.json      # 3D positioning: 5.1 share of the whole signal and of the SFX/VO
```
- The N6 native-audio proof is the last `[AUDIO_NATIVE]` line of a whole run (kept in the player
  log): `guest_mmio_fns=0` (no entry into any function that touches the XMA register block),
  `xma_regfile_writes=0` (the XMA* exports kick / lock / clear natively), `reg600=native:<PA>`,
  `xma_hal_voices=0 xma_hal_rest=0` (the XDK XMA HAL never runs).

- `compare` rows: `wcorr` = median 2 s waveform correlation at the matched place (1.000 = the same
  samples), `gaindB` = ours/oracle, `lag_ms` = local lag spread (a drift = a rate error),
  `UNMATCHED` = an energetic oracle window with no envelope match in ours (a missing sound, or a
  different era/input). Gameplay with the same tape still diverges in timing: judge the music and
  the level there, and the deterministic eras (intro movies, title, attract demo) bit for bit.
- **Timing instruments perturb what they measure.** Any file write or log line on the render-callback
  or SDL-callback thread is an audio gap waiting to happen: the dev log profile (SR_INSTANCE_ID
  rolls) flushes 1 MB inside a global lock, and the pre-iter-467 SR_AUDIO_WAV / first SR_AUDIO_FRAMES
  wrote synchronously (caught by SR_AUDIO_CB_PROBE: the render thread 70 ms inside WriteFile).
  The dumps and the periodic audio log lines are asynchronous now; measure output health in the
  PLAYER log profile (`SR_LOG=player`), where `[AUDIO_TIMING]` / `[AUDIO_OUT_SUM]` / `[AUDIO_DEV]`
  are kept.
- **SDL3 (lane sdl3-audio, RELEASE_PLAN step 3):** the device is an SDL3 device stream; SDL3 reads
  `SDL_AUDIO_DRIVER` / `SDL_AUDIO_DISK_OUTPUT_FILE` (`SDL_DISKAUDIODELAY` became `SDL_AUDIO_DISK_TIMESCALE`). Every
  harness launch sets both spellings (`tools/sdl_audio_env.py`); the exe maps the SDL2 disk-file name itself and logs
  `[AUDIO_ENV]` (WARN = SDL2 names used, ERROR = a harness run with no audio driver = the real speakers). The output
  path offline: `difftest --audio-sdl-replay --in <afr> --out <dir> [--ch 2|6|4|8]` (exit 0 = the device played the
  frames bit for bit); a set over the eras: `tools/audio_sdl_replay_set.py`. `SDL_AUDIO_DEVICE_SAMPLE_FRAMES` (default
  480 here = 10 ms, SDL2's WASAPI period; the disk driver then runs at exactly 48 kHz) sets the device buffer.
- Diagnostics: `SR_AUDIO_CB_PROBE=1` logs the host call stacks of every guest thread
  (`[AUDIO_CB_SLOW_STACKS]`, PDB-symbolized `sub_XXXXXXXX` frames, kernel/native_stack_probe.cpp)
  when one render callback runs > 60 ms. The Xenia hook's own JS lock can delay Xenia's audio
  worker, so the oracle's submit gaps are not a timing reference - only its content is.

---

## 2. Setup & prerequisites

### 2.1 MCP servers (load schemas via ToolSearch before first use)
Live: **ghidra** (decompile/xrefs/`emulate_function`), **log-sqlite** (`log_index`/`log_query`/
`log_around`), **va-to-source** (`va_to_source`/`source_to_va`/`build_va_index`), **frida-superman**
(`frida_guest_regs`/`frida_guest_backtrace`/`frida_read_memory`/`frida_watchpoint` + iter-295g
`frida_threads`/`frida_thread_backtrace` — ALL guest-running threads, not just main), **mcp-windbg**
(crash dumps / TTD). Trigger rules + load commands: [`TOOLS.md`](TOOLS.md), `CLAUDE.md`.

### 2.1b All-threads guest visibility (iter-295g)
The harness keeps a process-global registry of every guest-running thread
(`g_debugThreadRegistry`, 64 slots × 64 B, exported): main, FM workers, and host driver
threads (e.g. `FE_TICK`). `setCurrentGuestContext` auto-registers; `setGuestThreadName`
labels. Three consumers:
- **`[THREAD_SAMPLER]`** (host, gate `SR_THREAD_SAMPLER`, default ON, 20 s cadence) — logs each
  registered thread's live `lr/r1/r3/ctr`. **Any hang names its own location in the log — no
  attach needed.** Reads are fault-free (VirtualQuery-validated; the harness VEH makes any
  host AV fatal, so the sampler must never fault).
- **`frida_threads`** — enumerate all registered threads live (lr symbolized to `sub_XXXX`).
- **`frida_thread_backtrace`** — guest back-chain stack of ANY registered thread (by slot/tid).
MCP servers run old code until reconnected — restart the frida-superman server after pulling.

### 2.2 GhidraMCP plugin (powers L5 scripting + L6 emulate oracle)
Source: `<ghidra-mcp>` (its own git repo). HTTP server at `127.0.0.1:8089`. To rebuild
+ deploy after a Java change:
```bash
cd <ghidra-mcp>
GHIDRA_INSTALL_DIR="<workspace>/tools/ghidra_12.0.4_PUBLIC" ./gradlew buildExtension
cp build/libs/GhidraMCP-5.9.1.jar  "C:/Users/<you>/AppData/Roaming/ghidra/ghidra_12.0.4_PUBLIC/Extensions/GhidraMCP/lib/"
cp build/distributions/GhidraMCP-5.9.1.zip "$GHIDRA_INSTALL_DIR/Extensions/Ghidra/"
# then RESTART Ghidra (classes are loaded at startup; a program reopen is not enough)
```
The plugin carries the iter-215 fix that makes `emulate_function` work on PowerPC (arch-generic stack
pointer + link-register return + endianness + real register dump) plus the `read_memory` readback.
The `~/ghidra_scripts` directory holds the L5 pipeline `.java` files.

### 2.3 Xenia (L9-A)
Built instrumented exe at `reference/xenia-canary/build/bin/Windows/Release/` (the clone + build dir are
gitignored). Patches: `tools/xenia-trace/xenia_trace.patch` (Call-site hook) +
`xenia_trace_jitprolog.patch` (the rich per-entry hook). Build via CMake under `vcvars64` with
Ninja Multi-Config (xenia-build's VS auto-detect is broken here). Full recipe in the xenia-trace README.

---

## 3. Runtime env-var gates

Tool/diagnostic knobs (set in the environment before launching the recomp / Xenia):

| Gate | Effect |
|---|---|
| `SR_TRACE_CALLS=1` (+ `SR_TRACE_LO`/`SR_TRACE_HI`) | L4: log indirect/computed call targets as `[CALLTRACE]` |
| `SR_GUEST_TRACE=1` (+ `SR_GUEST_TRACE_PATH`) | L9-B: emit the binary guest-execution trace (`sr_boot.trace`) |
| `XENIA_GUEST_TRACE=1` (+ `XENIA_GUEST_TRACE_FILE`) | L9-A: instrumented Xenia emits its golden trace |
| `SR_LOG_BOOT`, `SR_LOG_KERNEL_IMPORT`, `SR_LOG_KERNEL_FILE` | extra logging channels |
| `SR_COLLISION_WATCH` (default ON, `=0` off) | collision pass-through evidence `[COLL_PATH]`/`[COLL_WORLD]`/`[COLL_BODY]`/`[COLL_MOVER]` (§1.10b); `SR_COLL_ROUTE=1` adds the fixed-route `[COLL_ROUTE]` lines |
| `SR_LOG_QUEUE_KB` (default 16384) | the log writer's queue for ordinary lines, plus a 64 KB reserve for `[LOG_STALL]` lines (§3.2.4) |
| `SR_LOG_INJECT_STALL=<ms>[@<s>]` (default off) | named diagnostic: once, `<s>` s after `Log::init`, the log writer sleeps `<ms>` inside one timed write (`[LOG_STALL] write ... injected=1`). It is the slow-disk case of the live plan `log_async` (§3.2.6). |
| `SR_GPU_HANG_DUMP=1` (default off) | the hang watchdog's PssCaptureSnapshot minidump. Its VA clone pauses the whole process for seconds, so never set it on a timing or proof roll; the `.bin` sidecar is always written (§3.2.5). |

Investigation gates (menu-blocker research, off by default — see `_attic/APT_INSTANTIATION_BLOCKER.md`):
`SR_ENABLE_RTAM`, `SR_ENABLE_SCRIPT`, `SR_DRIVE_FMUI_CTOR`, `SR_DRIVE_ACTIVATE_UI`, `SR_DRIVE_APT_TICK`,
`SR_FORCE_GAMECONFIG`, `SR_FORCE_UI_CFG`, `SR_INPUT_RECORD`/`SR_INPUT_REPLAY`, `SR_CRITSEC_FAKE_SUCCESS`,
`SR_DOGMA_HOST_ALLOC`, `SR_NO_MODGLOB_POLLER`.

---

## 3.1 ⛔ LOG TAGS ARE CAPPED OR SAMPLED BY DEFAULT — counting lines is not measuring

**Run `python tools/audit_log_gating.py --tag <TAG>` BEFORE quoting any number
derived from log lines.** It exits 1 when the tag is count-unsafe.

Figures below are DERIVED by the tool, not typed. Over the emitter sites it can
RECOGNIZE in `kernel/`, `gfx/` and `guest/ppc` (SR_LOG_BOOT / logInit2Progress
call sites carrying a `"[TAG]"` literal): **674 tags, 1,311 emitter sites,
506 count-unsafe** (CAPPED 410, SAMPLED 6, CAPPED+SAMPLED 61, CHANGE-TRIGGERED
29), plus **168 UNPROVED** (COUNTED-UNCAPPED? 44, UNGATED? 124).

⚠ **UNPROVED is neither safe nor unsafe.** "No gate found" is not "no gate
exists", and a bare counter is a lead rather than a guarantee. Never fold those
168 into a "safe" figure. Other loggers, runtime-built tag strings and emitters
outside those three trees are not seen by the tool at all, so this is a screen
over what it recognizes — not a census of all logging.

The four gating classes the auditor distinguishes, **all four count-unsafe**:

| Class | Shape | What a line count means |
|---|---|---|
| `CAPPED` | `if (n <= 24)` | you are measuring **24**, whatever the truth is |
| `SAMPLED` | `if ((n & 0x3FF) == 0)` | lines x stride ≈ population, never the population |
| `CAPPED+SAMPLED` | `if (n <= 64 \|\| (n & 0x3FF) == 0)` | both traps at once |
| `CHANGE-TRIGGERED` | `if (v != t_last)`, `if (s.insert(k).second)` | count is TRANSITIONS or DISTINCT KEYS, not events — but **silence is real evidence** that the value did not change |

**Counters EXPLICITLY VERIFIED uncapped** are the six gauges in
`kernel/natural_gauges.hpp`, read off the `[GAUGE]` census line
(`tools/audit_log_gating.py --safe`): `feTick`, `aptPump`, `clipCtorAll`,
`clipCtorReal`, `defExec`, `aptReq`. That list is **verified-safe, not
exclusive** — other safe counters may exist that the tool has not proved.
Everything else needs the auditor first.

**This trap has now cost the project at least seven times.**
`kernel/fe_flow_watch.cpp:177` carries the warning in a comment directly above
the cap it describes — *"a reader counting [APT_REQ] lines would report the cap
as the measurement (the sampling trap, 6th occurrence). Read [GAUGE] aptReq."*
iter-454d then reported **48 `[APT_REQ]` lines as 48 requests**: the emitter caps
at 24 and prints two lines per request, and the uncapped gauge says the true
figure is **34**. The same packet quoted `[APT_ADV]` line counts as clip
advances, when that emitter is `n <= 64 || (n & 0x3FF) == 0`.

**Exit codes — `--tag` is FAIL-CLOSED and never returns 0:**

| code | meaning |
|---|---|
| `1` | COUNT-UNSAFE — a gate was found; counting lines measures the gate |
| `2` | UNPROVED — no gate found, or a bare counter. **NOT ESTABLISHED** |
| `3` | NOT RECOGNIZED — no emitter for this tag in the trees scanned |

There is deliberately **no success code** for `--tag`. The tool can prove a tag
is count-unsafe; it can never prove one is count-safe, because "no gate found"
is not "no gate exists". An earlier revision returned 0 for UNPROVED tags, which
automation could read as permission to count — that is why exit 0 is not
offered at all. The only counts established safe are the six gauges under
`--safe`, and those are counters, not tags.

Three practical rules:
1. **Never state a count from `Select-String … | Measure-Object`** without
   running the auditor on that tag first.
2. **A gated emitter usually prints its own `#N` ordinal** — the highest `#N`
   observed is a far better lower bound than the line count. `[APT_ADV] #2411520`
   is worth more than "2,419 lines".
3. **When no uncapped counter exists, say "log lines"** explicitly and treat the
   figure as a lower bound. Do not silently upgrade it to an event count.

⚠ The auditor is a **screen, not a proof**: it is regex over the lines preceding
each emit, so it can miss an exotic gate. `UNGATED?` means "no gate found",
never "no gate exists" — it always prints `file:line` so the source is one click
away.

---

## 3.2 Log I/O off the game threads: the asynchronous log writer [app/log-async]

This section is the design note of branch `app/log-async` (base dev d71946e2). Its subject is the
logger (`app/log.cpp`) and every other host file writer that can run on a game thread.

### 3.2.1 Why: measured stalls

- **tcmp1 census0** (`_worktrees/native/archive/v1_native/tcmp1/tcmp_census0/drive/attempt1/run.log`
  lines 7270-7440; diagnosis by the transport-composed lane in
  `_worktrees/transport-composed/archive/tcmp/diag_tcmp1/stall_*.txt`). The CP thread spent ~4.85 s in
  the kernel while it wrote its per-frame `[PM4_COV] F` line. That line exists only under
  `SR_NATIVE_BRACKETS=1` (or coverage): `gfx/pm4_coverage.cpp:29`, emitted at `frameEnd`. The CP held the
  global log lock, and the flush rule sent the CRT buffer to the file from the CP's own thread. Evidence:
  - a `[PERF]` line stamped 25.571 sits after one stamped 26.235 (line 7279), because a line's stamp is taken
    before the lock;
  - the watchdog took 1.73 s between its first two lines;
  - the CP had 0 context switches in the stall sample.

  The host trigger: available RAM fell from 7089 to 5833 MB within a second while the game's working set stayed
  flat. Something else on the machine allocated or wrote, and writes to D:, a busy 7200-rpm HDD, stalled. The hang
  watchdog fired (3 s of flat fences). Its process-snapshot dump then froze the whole process a second time:
  every thread was silent from 28.04 to 32.24, the sampler included, and the dump failed (lines 7383 and 7435).
  `native_proof` aborted the plan on the trigger.
- **Earlier rolls:**
  - rcen1 census1: the CP waited 0.72 s on the log lock;
  - rcen1 census0: four in-game windows over 500 ms;
  - brk2 tbr_probe (+137.8 s): the same ~1 s signature, on a roll without the census;
  - bat2 on1: the main thread stalled 0.91 s between two log lines that only write a function table
    (`kernel/fe_flow_watch.cpp:1969-1978`), probably the same mechanism.
- **Consequence:**
  - disk stalls in the log pollute timing evidence (boot pairs, pacing soaks) and hang triggers;
  - a player on a slow disk can see hitches caused by logging.

### 3.2.2 Today's logging path (d71946e2), file:line

| Piece | Where | What it does |
|---|---|---|
| Profiles | `app/log.cpp:38-63`, `:149-161`, `:173-177` | `SR_LOG=dev\|player`; unset = dev when `SR_INSTANCE_ID` is set (every automated roll), player otherwise. Player keeps the tags in `kMilestoneTags` (`:69-111`) plus the first 4 Warn / 64 Error lines of each other call site, and drops the rest BEFORE formatting (`Log::write`, `:253-259`). |
| Formatting | `:261-290`, on the calling thread | The timestamp is read AT THE CALL (`system_clock`, date part cached per thread per second, `:264-281`). `vsnprintf` into 4096 bytes, then `"[%s.%03d] [%s] [%s] %s\n"` into 4224 bytes. A longer line is cut at 4223 bytes and so loses its `\n`. |
| **The global log lock** | `g_logSpinLock` `:26`; `LogSpinGuard` `:183-195`; taken at `:292` | Every logging thread takes it and holds it across four steps. (a) The 64-line crash-tail ring copy (`:293-301`). (b) The stderr echo: a synchronous `WriteFile` per line (`:303-315`). Since iter-465 `app/stdio_guard.cpp` puts a private 1 MB pipe in front of a console or pipe sink, but not a disk file. (c) `fwrite` into a 1 MB fully-buffered CRT stream opened `"w"`, i.e. TEXT mode, so each `\n` becomes `\r\n` (`:232-235`, `:317`). (d) `fflush`, a `WriteFile` of up to 1 MB into the OS cache (`:332-338`). |
| Flush policy | `:318-338` | `fflush` on an Error line, on every 256th line, or when 200 ms have passed since the last flush. The time rule is only checked on the next call: a quiet tail stays in the CRT buffer indefinitely. The flush runs on whichever thread crossed the threshold (CP, render, guest, watchdog), and every other logging thread spins on the lock meanwhile. One stalled `WriteFile` therefore stalls every logger. |
| Rotation | `app/main.cpp:297-307` | None. One file per process, truncated at `Log::init`: `superman_returns[_<SR_INSTANCE_ID>[_r<gen>]].log`. |
| Shutdown | `:245-251` (`fclose`) | `hostExitNow` (`app/main.cpp:183-195`) calls `Log::shutdown`, then `fflush(nullptr)`, `stdio_guard::flush(500)` and `quick_exit`. The fatal paths call `Log::shutdown` before `_Exit` / `abort` / `TerminateProcess`: `kernel/gpu_command_processor.cpp:172/455/521`, `kernel/indirect_call.cpp:121`, `kernel/native_flush.cpp:5592`. |
| Error-then-exit paths without `Log::shutdown` | `kernel/stubs.cpp:4669/4711/4729/4755/4925`, `gfx/d3d12_backend.cpp:346/11699` (`std::_Exit(3)`); `guest/guest_harness.cpp:709-710/886` (`fflush(nullptr)` + `ExitProcess(3)`); `gfx/hle_video.cpp:4930-4933` (`fflush(nullptr)` + `TerminateProcess`) | Their last Error line reached the OS only through the Error flush rule. |
| Crash handler | `app/crash_handler_win.cpp:27-63` | Writes a minidump of the live process, logs an Error `[CRASH]` line (flushed by the Error rule), then copies the tail ring to `superman_returns_crash.log`. There is no explicit log flush; `writeThreadFatalMinidump` (`:71-89`) also relies on the Error rule. |
| graceful quit (`tools/graceful-quit`, merged in dev 95e221f5, after this note was written) | `kernel/quit.cpp` `deadlineThread` / `pExit` | Its `Log::tryWriteLine(sev, text)` was try-lock + `fwrite` + `fflush` (never waits); the hard-deadline thread uses it before `TerminateProcess`. Its quit path calls `Log::shutdown` + `fflush(nullptr)` + `quick_exit`. Merged onto the writer in 3.2.4. |
| Stop-Process -Force (how rolls end) | - | Nothing in the process runs. Lost: whatever sits in the 1 MB CRT buffer, up to 255 lines, held for an unbounded time when no later call triggers the 200 ms rule. |

### 3.2.3 The other host file writers that can run on a game thread

This is a census of `app/`, `kernel/`, `gfx/` and hand-written `guest/*.cpp` (2026-10-14). It lists every
`fopen` / `CreateFile` / `WriteFile` / `MiniDumpWriteDump` / `PssCaptureSnapshot` / BMP / CSV writer and the
thread that performs it. Hot threads:

- the CP (`cpMain`, `kernel/gpu_command_processor.cpp:580`);
- the render thread (`SR PM4 render`, `gfx/d3d12_pm4_edram.cpp:3460`, where `dumpThenOverlay` runs at `:3611`).
  With `SR_PM4_RENDER_THREAD=0` its writers run on the CP instead;
- the guest threads.

No writer calls `FlushFileBuffers`. "Synchronous" means CRT `fwrite` / `fclose` (or `fflush`) on that thread: a
`WriteFile` into the OS cache, which blocks when the cache manager throttles writes to a busy disk (the tcmp1
mechanism).

**On a hot thread, and on in rolls or for players:**

| Writer | Thread | Gate (default) | Blocks that thread on disk | Disposition |
|---|---|---|---|---|
| The logger, `app/log.cpp` (3.2.2) | every logging thread | always | yes: the lock + the flush on whichever thread crosses the threshold | **FIXED**, 3.2.4 |
| The logger's stderr echo when stderr is a disk file. `app/stdio_guard.cpp:127-133` guards only pipe and console sinks. | every logging thread | `SR_LOG_CONSOLE` (on) | yes | **FIXED**: the echo moves to the writer thread |
| Present dump, `D3D12Backend::dumpBackBuffer` (`gfx/d3d12_backend.cpp:11843`). It waits on a readback fence (`~:12059`, the GPU, not the disk), converts and scans the pixels, then writes `RUN_CLASS.json` (`:12130`) and the BMP (`writeBmp32` `:12144` → `:3155-3177`): 8 MB per file at 1080p, 33 MB at 4K. | render | `SR_PRESENT_DUMP` (off). On in `tools/sr_roll.ps1 -DumpEvery` and in 78 of the 92 `tools/scenarios`; `native_proof` turns it off on rolls that grade no pixels (`NO_SPARSE`). | yes, on every file-sampled present | **INSTRUMENTED**: an `[IO_STALL]` line when the BMP write takes more than 100 ms. **PLAN:** the converted pixels go to a bounded BMP-writer thread (the render thread waits, reported, only when that queue is full). The `[PRESENT_DUMP] <file>` line is printed by that thread once the file exists. Not done here, because this is the flicker gate's own instrument and its graders pair `[PRESENT_DUMP]` with `[PRESENT_SIG]` and read the files after a `Stop-Process`. It needs the flicker gate and a grader audit (the async version can lose a file still queued at a kill). |
| Saves (game I/O by design). `exp_NtWriteFile` `fwrite` + `fflush` (`kernel/xam_content.cpp:1043-1044`); `writeWholeFile` (`:354-366`; header `:460`, thumbnail `:887`). | the saving guest thread | `SR_XAM_CONTENT_FS` (on; player too) | yes, as the console's own `NtWriteFile` is synchronous | **INSTRUMENTED**: `[IO_STALL]` on an `NtWriteFile` over 100 ms. Kept synchronous: the guest reads its status and byte count from the call. |

**On a hot thread, user actions (menus, not gameplay frames):**

| Writer | Thread | Disposition |
|---|---|---|
| PC SETTINGS apply: small `.ini` files written as temp + rename (`kernel/pc_settings.cpp:76-87, 1065`; `gfx/res_scale.cpp:399`; `kernel/highfps.cpp:344-349`; `gfx/gfx_options.cpp:123-128`) | render (`gfx/settings_ui.cpp:404/430`) | LISTED. Plan: hand the text to a small-file writer thread; the menu shows "saved" when that thread confirms. |
| FSCommand menus (`kernel/mod_bridge.cpp:391/476`, `kernel/mod_manager.cpp:808`, `kernel/mod_menu.cpp:520`, `kernel/devtools_menu.cpp:201`) | guest UI thread | LISTED, same plan |
| Mod manager boot extraction (`kernel/mod_manager.cpp:213-224, 432-440, 746`; `kernel/mod_zip.cpp:425-436`) | the first guest `NtCreateFile` (`kernel/stubs.cpp:2236` → `kernel/mod_overlay.cpp:185`) | LISTED. It runs once at boot, before any timing window, and only when mods are present. |

**On a hot thread, diagnostics that are off by default:**

| Writer | Thread | Gate | Disposition |
|---|---|---|---|
| Live PM4 trace `.xtr` / `.nrt`: a whole frame (`gfx/pm4_live_trace.cpp:107, 149, 223-227`, from `packetEnd` `:283`) | CP | `SR_PM4_TRACE_SWAP` / `_HOTKEY` / `_EVERY` | LISTED: a capture frame is not a timing sample. Plan: the frame buffer goes to a writer thread. |
| Missed-shader dump `gfx/pm4_engine.cpp:83-85`; draw-hash oracle `:992-1001` (`fflush` per draw, replay only) | CP | `SR_SHADER_DUMP_MISSES`, `SR_REPLAY_DRAW_HASH` | LISTED |
| Texture dumps `gfx/d3d12_backend.cpp:7052, 7126, 7714` | CP / render | `SR_TEX_DUMP` | LISTED |
| PM4 batch / EDRAM / oracle BMPs (`gfx/d3d12_pm4_edram.cpp:4266, 3758, 2917`); HDR async probe (`:2062-2067`); mod-library shots (`gfx/d3d12_overlay.cpp:440`) | render | `SR_PM4_DUMP_*`, `SR_PM4_EDRAM_DUMP`, `SR_PM4_ORACLE_DIR`, `SR_HDR_ASYNC_PROBE`, `SR_MOD_LIBRARY_SHOTS` | LISTED |
| HLE-path dumps: SRHT trace (`gfx/hle_video.cpp:3444-3777`), composite and resolve BMPs (`gfx/d3d12_backend.cpp:10659, 11563, 3423, 11134`), `dumpGuestFramebufferTagged` (`kernel/guest_context.cpp:2323-2333`) | HLE render / main | `SR_PM4_LIVE=0` paths only | LISTED (not reachable on the PM4 path) |
| Input recorder (`kernel/input_recorder.cpp:174-176`, `fflush` every 1024 polls); level skip / OBJSTATE (`kernel/devtools_levelskip.cpp`, `fflush` per event); guest trace (`kernel/guest_context.cpp:2638-2649`, every guest function entry, under a mutex); movie pace CSV (`kernel/cs_timeout_underflow_fix.cpp:6640-6697`); `kernel/shader_container_dump.cpp:134-144`; `kernel/hle_api_observe.cpp:754-762`; `kernel/pc_actions.cpp:473/637/640`; `kernel/tex_census.cpp:424-430`; physwin samples (`kernel/phys_window.cpp:285, 324, 375`) | guest | each its own `SR_*` (off) | LISTED |
| phys_window sticky file: `fseek` + `fwrite` + `fflush` (`kernel/phys_window.cpp:557-561`) from `registerGrant` (`:918`), **under `g_grantMu`** | guest (allocator) | `SR_PHYS_UNIQ_STICKY_FILE` | LISTED. Plan: publish under the lock, write after `lk.unlock()`. |
| Ungated: shader-container dumps (`kernel/cs_timeout_underflow_fix.cpp:3784-3911`; only when `shader_containers/` or `shader_rebuilt/` exists in the working folder; first 64) and failed-FX HLSL (`:4249-4251`, first 8 failures, `archive/fx_fail_*.hlsl`) | guest | none | LISTED. Plan: put them behind a gate. |
| Ungated stderr lines: `kernel/stubs.cpp:2099` (first 20 APC wakes per guest thread) and allocator refusals (`kernel/guest_context.cpp:523, 1168, 1243, 1586`) | guest | none | LISTED. They block only when stderr is a disk file (otherwise the stdio guard's pipe takes them). Plan: route them through the logger. |
| Legacy CP-stall minidump of the LIVE process, which suspends every thread (`kernel/gpu_command_processor.cpp:656`) | VD_ISR (`gpuCpVblank`) | only with `SR_GPU_HANG_WD=0` | LISTED |

**Already off the game threads (no change):**

- **P2 trace (`SR_P2_TRACE`).** Producers push under `g_mu`, with no I/O. Its writer thread swaps the buffer
  under the lock and writes and flushes every 200 ms outside it (`kernel/p2_trace.cpp:89-109`). Past 1M unwritten
  records it drops and counts the drops (`:115-120`).
- **Perf telemetry CSVs (`SR_PERF_STATS`, `perf_run`).** `statsMain` writes them (`kernel/perf_stats.cpp:351-480`);
  producers use a lock-free ring.
- **Profilers.** The host profiler (`kernel/host_profile.cpp`) and the guest profiler (`kernel/guest_profile.cpp`).
- **Timed frame dumps (`SR_FRAME_DUMP`, on in rolls).** Its own thread reads guest memory and writes three BMPs
  (`kernel/guest_context.cpp:2251-2295`); no render-thread readback. perf_run (since 2026-10-18, step 4 item 7e),
  native_proof, era_matrix and canary_compare set `SR_FRAME_DUMP=0`; `perf_run --frame-dump` opts back in.
- **Shader cache persist.** The `SR shader xlat` worker (`gfx/shader_runtime_xlat.cpp:276-323`); the CP only
  enqueues.
- **Audio.** The audio dump writer (`kernel/apu/audio_system.cpp:204-233`).
- **stdio_guard forwarders.**
- **The hang watchdog's dumps.** 3.2.5 moves them onto a helper thread.
- **At exit.** The exit handlers (`at_quick_exit`: perf memmap, profilers) and the crash handler's minidump run on
  a thread that is already leaving.

### 3.2.4 The new design

**Producers format and enqueue; one writer thread writes.**

- **Producer, on the calling thread.** The profile check, timestamp and formatting are unchanged, so the line
  bytes are unchanged. Under the existing spinlock the producer copies one record (`u32 len` + the line) into a
  byte ring and updates the crash-tail ring. That is all the lock covers: no syscall, no I/O. It then wakes the
  writer (`WakeByAddressSingle`, only when the writer is idle). The ring position is the global sequence: records
  enter in lock order and are written in ring order, whole, by one thread, so lines are never interleaved
  mid-line. Timestamps are still taken before the lock, so a line can carry an earlier stamp than its
  predecessor, exactly as today.
- **Writer: one thread, `SR log writer`, above-normal priority.** It wakes on an enqueue or every 100 ms. Each
  pass it takes whole records, up to 256 KB of line bytes, translates `\n` to `\r\n` (the CRT text-mode bytes),
  and hands them to the OS with one `WriteFile` on a Win32 handle (`CREATE_ALWAYS`, shared read / write like
  `fopen("w")`). Then it echoes the untranslated bytes to stderr (`SR_LOG_CONSOLE`, as today). There is no CRT
  stream and no `FlushFileBuffers`: once `WriteFile` returns, the bytes are in the OS cache and survive any
  process death (only an OS crash loses them, as today). The flush latency is one wake, normally under 1 ms;
  there is no N-line or N-ms batching.
- **Bounded memory, no loss.** Ordinary lines may occupy `SR_LOG_QUEUE_KB` of the ring (default 16384 = 16 MB,
  minimum 16, which is more than one record). A further 64 KB is a reserve that only the logger's own
  `[LOG_STALL]` lines and crash / abort lines (the emergency path, below) may use.
  Logging runs at ~8.5 KB/s in a player-profile census roll and
  ~580 lines/s in dev gameplay, so 16 MB rides out a disk stall of tens of seconds. When the ring is full, the
  producer WAITS in 100 ms slices (`WaitOnAddress` on the writer's progress, lock released) until its record
  fits. It never drops while a writer exists or can be started. The wait is reported (next item) and counted.
  This is the brief's policy; no other policy is used. The one exception: when there is no writer and none can be
  started (`CreateThread` fails, which happens only while the process exits), nothing will ever make room, so the
  line goes to stderr and counts in the census's `lost=` instead of hanging the thread.
- **`[LOG_STALL]` witnesses.** The tag is in `kMilestoneTags`; the writer's own lines bypass the profile filter
  anyway. All of them are WARN, category BOOT, in the normal line format:
  - `[LOG_STALL] producer wait tid=<os tid> name=<thread description> ms=<waited> slices=<n> bytes=<record>
    queued=<bytes> cap=<bytes>`: enqueued by the producer right after its own line, in the same critical section.
    It may use the reserve.
  - `[LOG_STALL] write tid=<writer> name=SR log writer op=file|stderr bytes=<n> ms=<t> queued=<bytes>
    count=<n> [injected=1]`: when a write takes more than 100 ms. These lines are aggregated: at most one per op
    per second. `ms` is the slowest write since that op's previous line, `bytes` is its size, and `count` is how
    many writes were over 100 ms. The first slow write is reported at once. A slow write gets no line when its
    batch held only the logger's own lines, unless it is the injected stall. Without that rule each witness line
    would make its own successor on a disk that stays slow, and `Log::shutdown` would never end (Astra round 1).
    The census's `slow=` still counts every slow write.
  - `[LOG_STALL] write FAILED tid=<writer> gle=<error> lines=<n> bytes=<n> tries=3`: a batch that could not be
    written after its retries (below). The line goes to stderr at once and into the file once the disk takes
    writes again. A batch of only the logger's own lines is reported on stderr only, so a dead disk cannot keep
    the queue busy.
  - `[LOG_STALL] census periodic|final t=<s> lines=<n> bytes=<raw bytes enqueued before this line>
    written=<raw bytes> writes=<n> slow=<n> max_write_ms=<t> waits=<n> unwitnessed=<n> wait_ms=<t>
    max_wait_ms=<t> peak=<bytes> cap=<bytes> lost=<n>`. Emitted every 10 s in an automated run
    (`SR_INSTANCE_ID`), every 5 min in a plain player launch (a player log stays small), plus a `final` one at
    `Log::shutdown`. It is the activation witness: a roll without these lines ran an exe without this writer. It
    is also the completeness check: the file bytes before the census line, minus the count of `\n` among them,
    must equal `bytes=`. `waits` counts every producer wait. `unwitnessed` counts the waits whose witness line
    found no room even in the reserve, so `waits` = producer-wait lines + `unwitnessed`.
  - The writer never waits for its own queue (that would deadlock), and it tries the log lock for at most 20 ms
    for its own lines. If even the reserve is full, it keeps the counts and retries after its next write.
- **Same bytes.** For the same sequence of log calls the file is byte-identical to today's except for the
  `[LOG_STALL]` lines: same text, same truncation, same `\r\n`, same order rule. The stderr echo carries the same
  bytes per line, but they are now written by the writer thread, so how they interleave with code that writes
  stderr directly can differ.
- **Pre-init and post-shutdown** keep today's behaviour: no file; the tail ring plus a synchronous stderr echo,
  outside the lock.
- **`[IO_STALL]` for the writers that stay synchronous on a hot thread** (3.2.3: the present dump's BMP and the
  saves' `NtWriteFile`). `Log::noteIoStall(what, bytes, ms)` logs `[IO_STALL] what=<writer> tid=<os tid>
  name=<thread> bytes=<n> ms=<t>` when one write took more than 100 ms. The tag is in `kMilestoneTags`. The write
  itself is unchanged; what the instrument adds is that the stall becomes visible and attributable.

**One owner, every write journaled** (Astra round 1 found two recovery drains that could overlap and a recovery
loop without a journal). Exactly one thread writes the file at a time:

- **The writer thread.** A writer that ended is replaced by whichever thread notices first (`ensureWriter`).
  Only the test seam ends a writer; nothing in the game does. The replacement is one `CreateThread` behind a
  compare-and-swap flag, so there is one replacement at a time, and the noticing thread does no file I/O. The
  replacement's first act is to finish the ended writer's batch.
- **The writer thread itself in its crash filter or SIGABRT handler** (`Log::flush` on the writer thread). It is
  the owner, and nobody else would write.
- **The exit drain** (the TLS process-detach callback), after the OS has ended every other thread.

A producer or a flusher never writes the file. A producer waits on the writer's progress; a flusher waits too,
but only until its own deadline. This also fixes the other P1: dead-writer recovery used to put synchronous
`WriteFile`s on game threads and ignore `flush` deadlines.

Every batch goes through one journal: `inflight = 1` while `stage[0, stagedLen)` is being written at file offset
`inflightAt`. The commit stores `tail` (the batch leaves the ring) before `inflight = 0`. Whichever owner comes
next finishes an interrupted batch at the file's real position (`SetFilePointerEx`), whether it is a replacement
writer, the crash drain or the exit drain. So nothing is lost and nothing is written twice. A journal found
already committed (`tail == inflightEnd`) is only closed. This holds across several owners: a writer killed
mid-write, then its replacement killed mid-resume, then the exit drain (test X1).

**Write failures** (Astra P2: a failed write still let `flush()` succeed):

- A failed `WriteFile` is retried from where it stopped: 3 tries, 50 ms apart. A partial write is not
  rewritten.
- A batch still not written after that is lost. Its lines count in the census's `lost=`, and the
  `[LOG_STALL] write FAILED` line says so.
- From the first loss on, `flush()` never returns true again, because a line enqueued before the call is not in
  the file.
- The next write starts with `\r\n`, so the piece of a line the lost batch left in the file ends there and is not
  glued to the next record.
- The stderr echo is best effort and never counts as a loss.

**Crash / abort context (`Log::EmergencyScope`; Astra P1: crash logging could wait forever).** The crash
filter, the SIGABRT handler and `writeThreadFatalMinidump` open a scope before their first log call. While it is
open, that thread's lines take the emergency path:

- the log lock is tried for at most 50 ms;
- the line may use the 64 KB reserve and never waits for room;
- a line that cannot be queued goes to stderr and counts in `lost=` (`failed()` gives the scope's count).

A thread that logs while it already holds the log lock takes the same path even without a scope. That happens
when a fault inside the logger reaches a VEH or `__except` filter that logs; the lock owner's thread id is kept
for this check. So nothing spins on a lock its own thread holds. `getTailLines` (the crash tail file) waits at
most 50 ms for the lock and otherwise reads the slots as they are. The writer's own lines use the same bounded
try.

**What is guaranteed, and what is not:**

- **Guaranteed:**
  - Order, and no loss, while a writer runs and the disk accepts writes.
  - No game thread ever performs a log file write. A logging thread can wait only on the log lock (a record copy),
    on a full queue (reported), or in a `flush` it asked for, bounded by its timeout.
  - The crash filter returns within about 2 s + the minidump + 3 s, even with a full queue and a stuck writer
    (C6), and even when the fault is inside the logger (C7).
- **Not guaranteed:**
  - A write in progress is not interrupted. While the writer is stuck inside `WriteFile` on a dead disk, the
    queued lines stay queued. If the process then crashes, they are lost once the crash flushes time out; if it
    is killed, they are lost outright. No other thread could write them: a stuck synchronous write holds the file
    object.
  - The owner's drain checks its deadline only between batches (each at most 256 KB).
  - The exit drain has no deadline. It runs when every other thread is dead, and a disk that never returns hangs
    the exit, as the old `fclose` did.
  - A thread that faults inside the logger leaves the log lock held. Other threads' lines then wait on that lock
    until the process ends; only the faulting thread's own filters go on.
  - The resume assumes the log is a disk file, which it always is. On a pipe the file position is unknown and a
    resumed batch would be rewritten whole.

**Drain points** (every line enqueued before them reaches the OS):

- **`Log::flush(timeoutMs = 2000)` (new).** It returns true once every line enqueued by any thread before the
  call is in the file. It returns false if the writer did not get there in time: those lines stay queued and are
  still written. It also returns false, from then on, once any line of the log was lost.
  - It waits on the writer's progress only, never on a lock a game thread holds, and its deadline holds
    throughout.
  - A dead writer is replaced, never stood in for: `flush` does no file I/O.
  - The exception is the writer thread itself (its crash filter / SIGABRT handler). There, `flush` drains as the
    owner, checking the deadline between batches.
- **`Log::shutdown()`.** Flushes first, so the writer's own `[LOG_STALL] write` lines about that drain land
  too. Then it enqueues the `final` census and stops accepting lines (later ones take the stderr-only path, as
  after the old `fclose`). The writer writes what is queued and ends (waited up to 10 s), and the file is
  closed. It is idempotent, and two concurrent calls are safe (X2).
  - A writer that ended early is replaced and the replacement drains; `shutdown` itself never writes.
  - A writer stuck in a write keeps the handle, and the exit drain finishes it.
- **The crash filter** (`unhandledExceptionFilter`) opens a `Log::EmergencyScope` first. It calls
  `Log::flush(2000)` before its live minidump, which can take seconds or hang, and `Log::flush(3000)` after its
  `[CRASH]` line. The minidump waits at most 2 s for the DbgHelp lock (3.2.5). If the lock stays busy, the dump is
  skipped and a `[CRASH] minidump skipped` line says so. `writeThreadFatalMinidump` opens the same scope and calls
  `Log::flush(2000)` after its `[THREAD_FATAL]` line.
- **`abort()` / `std::terminate`:** `Log::init` installs a `SIGABRT` handler. It opens a `Log::EmergencyScope`,
  flushes for 3 s and then chains any previous handler. The UCRT's `abort` raises `SIGABRT` before its
  `__fastfail` when a handler is installed.
- **Process exit** (`ExitProcess`, `_Exit`, `exit`, `quick_exit`, `main` returning): a TLS callback on
  `DLL_PROCESS_DETACH` runs after the OS has terminated every other thread.
  - If the log is still open, the callback becomes the owner and writes what is left: the open journal first,
    at the file's actual position, so nothing is lost or written twice; then the queue.
  - A producer killed mid-copy never published its record.
  - A line logged after this drain takes the stderr-only path.
  - This covers every Error-then-`_Exit` path above without touching them.
- **`TerminateProcess(GetCurrentProcess())`:** no callback runs, so those sites flush first.
  `kernel/indirect_call.cpp:121` already calls `Log::shutdown`; `gfx/hle_video.cpp:4931` gets a `Log::flush`.
- **Stop-Process -Force:** no code runs. Lost: the records enqueued but not yet handed to the OS. Normally that
  is what arrived since the writer's last wake (well under a millisecond's worth). During a disk stall it is
  everything queued since the stall began, at most the queue. Nobody else can write those lines: a stuck
  synchronous `WriteFile` holds the file object, and any other write would queue behind it. The same applies
  when a crash flush times out while the writer is stuck.
- **graceful quit (merged from dev 95e221f5).**
  - `Log::tryWriteLine`, used by the hard-deadline thread just before `TerminateProcess`, does three things: it
    formats the line; it tries the lock for at most 20 ms (never when its thread holds it) and enqueues only if
    there is room, so it never waits; then it calls `Log::flush(1000)`. It returns true only when `flush` does.
    Tested by Q1-Q2: the line and the 300 before it survive the `TerminateProcess`; under a 3 s stall it returns
    false after about 1 s and the line still lands.
  - The quit's normal exit (`pExit`) calls `Log::shutdown`, which drains before the final census and closes the
    file, so the terminal snapshot is in the file before `quick_exit`.
  - `Log::setThreadCapture` (the terminal snapshot's capture) is unchanged in `Log::write`, before the profile
    filter.
  - The writer is a host thread the quit never parks, and no guest thread can park holding the log lock: the lock
    covers a copy only, never a kernel import.

**Seams.** Each is read once at `Log::init`.

- `SR_LOG_QUEUE_KB`: the queue size.
- `SR_LOG_INJECT_STALL=<ms>[@<s>]`: a named diagnostic, default off. Once, `<s>` seconds after init, the writer
  sleeps `<ms>` inside one timed write. The write line carries `injected=1`. It is meant for live proof rolls.
- `Log::testSeams(...)`: difftest only, every field off by default. The fields:
  - `exitMidWriteMask`: the writer writes half of the chosen file writes and ends its thread;
  - `failWrites`: partial writes that fail;
  - `slowWriteMs`: every write sleeps first;
  - `respawnLimit`: how many ended writers may be replaced;
  - `faultInLock`: a fault inside the logger with the lock held;
  - `onFileWrite`: reports the thread of every file write.

  They drive the recovery, failure, sustained-latency and re-entry tests.

### 3.2.5 The hang watchdog's process-snapshot dump

Today's code: `writeDump` (`kernel/gpu_hang_wd.cpp:669-792`), called from the watchdog's own `tick` (`:1145`,
`:1166`). It writes the `.bin` sidecar of guest ranges (`:725-734`), then takes a `PssCaptureSnapshot` VA clone
(`:763`) and runs `MiniDumpWriteDump` on it (`:779`). At most 2 dumps per process.

**Decision: the minidump does not run by default, in proof rolls or anywhere.**

- **New gate `SR_GPU_HANG_DUMP`, default 0.** `=1` restores the dump for a hang investigation.
- **Never on the watchdog's thread.** When the dump is on, it runs on a one-shot helper thread.
- **The `.bin` sidecar stays on by default** (the guest ranges, 85 KB in tcmp1). The watchdog copies the ranges
  into memory, which is only memory reads, as today; the helper thread writes the file and logs the usual
  `[GPU_HANG] dump #n (...) -> ...` line. With the dump off, that line says `minidump off (SR_GPU_HANG_DUMP=1)`.
- **Unchanged:** the `[GPU_HANG]` block itself, now never stalled by the disk; the one-dump-per-trigger accounting;
  the gate `SR_GPU_HANG_WD`.

Why:

1. **It froze the whole process for ~4.2 s** (tcmp1 census0). Every thread was silent from 28.04 to 32.24: the
   census sampler's 1-s sample stretched to 4.4 s, and the `GPU_HANG_WD` thread spent 4.16 s in the kernel. The
   VA clone of the ~8 GB address space is the expensive step; after a clone, game threads also take
   copy-on-write faults while the dump runs.
2. **It failed.** Flag set 0 returned 87 (as in iter-463 E3), and `MiniDumpWriteDump` on the set-1 clone
   returned FAILED (line 7435).
3. **On the watchdog thread it stops the watchdog's own clock.** No tick runs while the dump does, so the "flat
   for N s" windows, H2's consecutive-tick rule and the +30 s / +120 s re-reports measure the dump, not the game.
4. **A proof roll gains nothing from it.** The trigger already ends the plan (`native_proof` ABORT), and the
   evidence is the `[GPU_HANG]` block, logged before the dump. A freeze after it only pollutes the later lines.
   Thread state is available live (frida `frida_threads` / `frida_thread_backtrace`), in the block's
   `[GPU_HANG_THREADS]` / `[GPU_HANG_CS]` lines, or from a deliberate `SR_GPU_HANG_DUMP=1` roll.

Even with `=1`, the clone still pauses the whole process. The gate's documentation says so: never set it on a
timing or proof roll.

**One DbgHelp call at a time** (Astra round 1, P2). The watchdog's hang trigger and its double-book trigger each
start their own helper thread, so with `SR_GPU_HANG_DUMP=1` both could be inside `MiniDumpWriteDump` at once.
DbgHelp is single-threaded, and before this change the watchdog thread had serialized the two dumps. Now
`sr::DbgHelpLock` (`app/crash_handler.hpp`) is one process-wide lock that every DbgHelp call in the host holds:

| Caller | Wait for the lock |
|---|---|
| hang-dump helper (`PssCaptureSnapshot` + `MiniDumpWriteDump`; dumps run one after the other, the waiting is on the helper thread) | unbounded |
| crash filter | 2 s, then the minidump is skipped and logged |
| thread-fatal dump | 5 s |
| diagnostic dump (`writeDiagnosticMinidump`) | 10 s |
| image-write VEH symbolizer | 1 s |
| native stack probe (it replaces that file's own mutex) | 5 s |

The lock is an owner word with no CRT lock and no heap, so it is safe in a crash filter. It refuses re-entry by
its holder, which happens when a fault inside DbgHelp reaches a filter that would call DbgHelp again.
`tools/test_log_async_wiring.py` W7 checks every DbgHelp call site in `app/ kernel/ gfx/ ui/ guest/*.cpp`.

### 3.2.6 Proof

**Offline, on the cloud worker; no game ran.**

`build/difftest.exe --log-async-test` (`tools/difftest/log_async_test.cpp`): **34 passed, 0 failed** after
Astra round 1; the first version had 21. Each case runs in a child process (a kill-on-close job), so the profile,
queue size, injection and crash mode start fresh.

| Case | What it shows |
|---|---|
| O1 order | 8 threads pass a token, so the enqueue order is fixed by the program. The file holds seq 0..11999 in that order. |
| S1 stress | 16 threads x 4000 lines, 64 KB queue, 300 ms writer stall. Every line exactly once, byte-exact, in its thread's order. 4209 `producer wait` lines, each right after a line of its own thread, equal to the census `waits` (+ `unwitnessed` 0). Census `bytes=` exact. |
| D1 slow disk | The writer sleeps 5 s inside its first write. 4 producers log 8000 lines in 4.9 ms (max `Log::write` 0.23 ms) while none of them is on disk; then all 8000 land with one `injected=1` write line of >= 5000 ms and no producer wait. |
| W1 full queue | A 64 KB queue behind a 1.5 s stall: the producer waited 1.4 s, said so, and dropped nothing. |
| C1 crash | An access violation through the game's own `installCrashHandler` filter, with an 800 ms writer stall pending: every line and the `[CRASH]` line are in the file. |
| C2 abort | `std::abort()` (the SIGABRT handler, then the UCRT `__fastfail`, rc 0xC0000409), stall pending: every line. |
| C3 exit | `std::_Exit(7)`, stall pending: the TLS process-detach drain writes every line. |
| C4 / C5 mid-batch | The writer ends its thread with half of write 3 on disk. C4: the `_Exit` drain resumes it. C5: the replacement writer `Log::flush` starts resumes it. Either way 720 lines appear exactly once, in order, with no fragment. |
| C6 / C6b crash, full queue (Astra P1) | The crash comes while a producer waits on a full 16 KB queue behind a writer stuck in a 120 s write. The crash filter's lines take the emergency path, its flushes time out, and the process ends after 6.1 s; it used to wait for the writer. With a 4 s write (C6b), the `[CRASH]` line, queued in the reserve, is in the file once the writer is back. |
| C7 fault inside the logger (Astra P1) | A fault with the log lock held. A VEH that logs (the ordinary path) and the crash filter (the emergency path) both finish instead of spinning: both lines reach stderr, the crash tail file is written, and the 100 lines before are in the file. |
| X1 exit during takeover (Astra P1) | The writer dies in write 3, its replacement dies resuming that batch (write 4), and no further replacement is allowed; then `_Exit`. The exit drain finishes the same journal: 720 lines exactly once, no fragment. |
| X2 concurrent takeover and shutdown (Astra P1) | The writer dies in write 3 while 3 producers log, 4 threads flush in a loop and 2 call `Log::shutdown` at once. 17 file writes by 2 writer threads (the first and its one replacement), 0 on a game thread; 1200 lines once in their threads' order; one final census, `bytes=` exact. |
| R1 / R2 recovery (Astra P1) | A dead writer, a 1 s-per-write disk, and a producer pushing 63 KB through a 16 KB queue. The waiting producer (R1) or a `flush(200)` (R2) finds the writer dead first. A replacement does every file write (0 on a game thread), `flush(200)` returns false after 206 / 213 ms, and every line lands once, in order. |
| F2 retry (Astra P2) | Two partial failed writes. The retry continues where the write stopped: 100 lines once, no fragment, `lost=0`, census exact. |
| F3 lost (Astra P2) | A batch that fails all 3 tries. Census `lost=1`; `[LOG_STALL] write FAILED` on stderr and in the file; the torn piece is ended, so nothing is glued to it; later lines land; `flush()` is false after the loss, after a later line and after shutdown. |
| S2 sustained slow disk (Astra P2) | Every write takes 150 ms. With nothing logged for 3 s, no witness follows its own write (1 line at 2.5 s, 1 at 5.5 s). Then 3 s of steady logging gives 5 `[LOG_STALL] write` lines covering 22 slow writes, at most one a second. `Log::shutdown` ends in 581 ms with its final census; before the fix it never ended. |
| H2 DbgHelp lock (Astra P2) | Exclusive across 4 threads (8000 sections, never 2 inside). Re-entry is refused at once, a 60 ms timeout is honoured (59 ms), and the lock is granted once released. |
| K1 kill | A `TerminateProcess` 100 ms after 500 lines were logged to an idle writer: all 500 are there. |
| F1 flush | `flush()` is true with the line in the file; false within 200 ms under a 1.5 s stall (the line not yet there); true after it; true after shutdown. `noteIoStall` logs a 150 ms write and not a 50 ms one. |
| H1 hang dump | `writeDump` as a trigger takes it returned in 64 ms. The dump line came from the helper thread (another tid). The GHW1 sidecar was written; there was no minidump. |
| P1 / P2 | The player profile keeps `[IO_STALL]` and the census. An automated run gets a census every 10 s with `bytes=` exact while two threads log multi-line records. |
| B dev / player | The d71946e2 logger (`tools/difftest/log_legacy_ref.cpp`, verbatim) and this writer run the same calls (severity x category, the Warn / Error caps, a 5000-char message, embedded `\n` / `\r\n` / `\r`, UTF-8, `%c` 0, writeV, 2000 lines). The log file (123,756 / 121,494 bytes) and stderr are identical once timestamps are masked and the `[LOG_STALL]` lines removed. |

**Mutants** (`tools/difftest/log_async_run.py`, applied in place on the worker tree): **28 of 28 fail the suite**
(the final run, against the final test file: clean 34 / 0 before and after). M1-M15 come from the first version. M16-M28 were added for Astra round 1, one per fix:

| Mutant | The defect it plants | Caught by |
|---|---|---|
| M16 | a replacement writer skips the journal | C5, X1, X2 |
| M17 | a waiting producer writes the file for a dead writer | R1 |
| M18 | a flusher writes the file for a dead writer | R2, X1, X2 |
| M19 | crash lines wait for room like any producer | C6 (the child hangs to its 30 s timeout) |
| M20 | re-entry not detected on the ordinary path | C7 |
| M21 | emergency lines without the reserve | C6b |
| M22 | a failed batch counted as written | F3 |
| M23 | a retry restarts the batch | F2 |
| M24 | `flush()` ignores the loss | F3 |
| M25 | the torn line is not ended | F3 |
| M26 | a witness of only internal lines raises another | S2 |
| M27 | witnesses not rate-limited | S2 |
| M28 | the DbgHelp lock not exclusive | H2 |

In the first run M19 and M21 SURVIVED. C6's filler wrote 215-byte lines, and a filler that waits for room can
leave a gap of up to ~218 bytes. The ~100-byte `[CRASH]` line fitted into that gap through the ordinary path, so
neither the emergency path nor the reserve was needed. The filler now writes lines shorter than the `[CRASH]`
line; C6b also checks that the `[CRASH]` line comes before the waiting producer's line. Both mutants are then
KILLED.

The final test file also deletes the pid-named hang-dump files before H1 runs. A `.dmp` that the M15 mutant had
written under a pid that a later child reused had made H1 fail once on the clean build.

The first version's mutants:

| Mutant | The defect it plants |
|---|---|
| M1 | per-thread batching (order) |
| M2 | no lock |
| M3 | drop when full |
| M4 | the wait witness not written |
| M5 | the wait not counted |
| M6 | the crash filter does not flush |
| M7 | no TLS drain |
| M8 | no SIGABRT handler |
| M9 | the resume ignores the file position |
| M10 | `flush()` returns at once |
| M11 | no `\r\n` |
| M12 | the writer ignores wakes. It first SURVIVED, because K1 logged before the writer went idle; K1 was fixed, then KILLED. |
| M13 | producers wait for the disk |
| M14 | the hang-dump files are written on the calling thread |
| M15 | the minidump is on by default |

**Wiring and evaluator tests:**
- `tools/test_log_async_wiring.py` reads the source and passes with 0 failed. It covers:
  - every self-`TerminateProcess` (whole-surface);
  - the crash-filter drains;
  - the watchdog's thread split and gate;
  - the `[IO_STALL]` call sites;
  - the milestone tags;
  - the TLS / SIGABRT registration.

  Since Astra round 1 it also covers:
  - W2: the crash / abort paths open `Log::EmergencyScope` before their first log call;
  - W6: one owner. `drainAsOwner` is called only from the exit drain and from flush on the writer thread;
    `emitLine`, `emitEmergency`, `tryWriteLine`, `shutdown` and `ensureWriter` write no file; every batch is
    journaled;
  - W7: every DbgHelp call in the host trees comes after a `DbgHelpLock`. This is whole-surface: 11 sites.

  24 of 24 wiring mutants (`archive/logq/mutate_wiring.py`) fail it. Among them: a shutdown, producer or flusher
  that writes for a dead writer; an unjournaled drain; each DbgHelp site without the lock; each crash path
  without the scope.
- `tools/perf/test_log_stall_eval.py` passes 23 of 23. Six of these tests are new, and all six fail against the
  evaluator as it was before Astra round 1: Astra's two reproductions, a census only late in the roll, census
  waits that disagree with the file, `lost=2`, and an injected roll whose census counts a wait.

**Every suite's exit code against the base.** The runner covers every difftest `--*-test`, every standalone
`build\*_test.exe`, and every tracked `tools/` and `scripts/` `test_*.py` / `test_*.ps1`, run in a built worker
tree.

- **After Astra round 1 and the merge of dev 95e221f5** (base: dev 95e221f5 in its own tree `logq_base`, run
  first, then the lane, never both at once): the base has 206 suites and all 206 give the same exit code in the
  lane. The lane adds three, all exit 0: `--log-async-test` 34 / 0, `test_log_stall_eval.py` 23 / 23,
  `test_log_async_wiring.py` 0 failed. `--quit-test` (graceful quit, merged) gives 76 passed, 0 failed.
- The first version (base dev d71946e2) is kept below for the record.
- 200 suites in the base; the lane has the same 200 plus 3 new ones (`--log-async-test`,
  `test_log_stall_eval.py`, `test_log_async_wiring.py`, all exit 0). 199 of the 200 have the same exit code.
- The one that differs is `test_sr_drive_stage.ps1` (0 -> 13). It refuses to stage below its 50 GB free-space
  floor, and the worker's C: had fallen to 34 GB. The BASE tree's copy, run at the same disk state, gives the
  same 13 (29 passed, 13 failed). That is the environment, not this change.
- Two lane runs needed a second look:
  - An earlier lane run had `--audio-out-test` at 2 (A5a pacing: 129 vs 185 callbacks/s) while another build
    loaded the worker; the final run gives the base's 1.
  - The standalone exes failed to start ("the file or directory is corrupted and unreadable") after the worker's
    hard reset; they were relinked and give the base's codes.
- `tools/test_hrr_invalidation_surface.py` caught a `VirtualFree` of the log ring (4 -> 5). The ring is now never
  released, and the result is the base's 4.

**The forcing audit is unchanged** (`scripts/audit_forcing.py`, base and lane trees on the worker). After Astra
round 1, against dev 95e221f5:
- `--count` 0 (UNGATED) in both;
- the 788 guest-touching sites of `--json` are identical apart from line numbers;
- `gen_override_manifest.py` regenerates `docs/OVERRIDES.json` with no diff in either tree (925 overrides, the
  same classes);
- the only line of the summary that differs, with line numbers masked, is the launch-site count: 99 -> 101 with
  FAIL 0. The two new sites are INERT, consistent with the two new host threads: the log writer (`startWriter`)
  and the hang-dump helper.

The first version against d71946e2 had 779 sites, 924 overrides, and launch sites 96 -> 98, with the same two
INERT sites.

**The release build builds.** `build-rel` with `SR_RELEASE_COMPOSITION=ON` exited 0, in 146 s after Astra round 1
(154 s for the first version), and the exe carries the writer.

**Staged** (`C:\sr\vm_stage.ps1 -Name logq -Out C:\sr\out\logq`, after Astra round 1 and the merge of dev
95e221f5): `SupermanReturns.exe` SHA256 `5858BCE65E0AF28222F042E6A9FE19381038C71D3EBEDB7D9C7B18350E1EA70E`,
`difftest.exe` `F8449FDAAE5493477160CD531529AA9B21578A86102AACB57F76344D64ADAE0F`. The first live set (lsa1)
ran on the first version's exe `693892A0C99F...`.

**Not provable offline:**
- A real disk stall on the owner's HDD. The writer's injected sleep stands in for it; the live plan injects one
  in gameplay.
- The CP's frame interval in a real roll.
- The present dump's `[IO_STALL]` and the saves' `[IO_STALL]` firing on a real slow write (only their wiring is
  tested).
- That the game's exits other than those simulated (`ExitProcess` from guest code, the fatal `_Exit` paths) take
  the TLS drain: the mechanism is tested with `_Exit`, not each call site.
- The stderr echo at a crash. It is best effort: the exit drain writes the file only, because the stdio guard's
  forwarders may be gone.
- A real failing disk: write failures are simulated through `Log::testSeams` (partial writes that return an
  error), not a full or removed volume.
- Two hang dumps racing in a real roll with `SR_GPU_HANG_DUMP=1`. The lock is tested directly (H2) and its wiring
  at every DbgHelp site (W7); the PSS snapshot path is not run offline.

**Live plan** (the orchestrator runs it; nothing here launched it):
`python tools/perf/native_proof.py --plan log_async --exe <snapshot> --out <dir> [--stage J:\sr_stage]`, then
`python tools/perf/log_stall_eval.py --proof <dir> --prefix lsa [--json <dir>/log_stall_eval.json]`. Three
perf_run gameplay rolls (the dry run is clean, with and without `--stage`):

1. `census0`: `SR_NATIVE_BRACKETS=1`, so the CP logs a `[PM4_COV] F` line per frame (the tcmp1 stall's line);
2. `plain`: the default path;
3. `inject`: `census0` plus `SR_LOG_INJECT_STALL=5000@120`, so the writer sleeps 5 s inside one write in gameplay.

The evaluator's rule (its docstring, registered with the plan; tightened after Astra round 1, which reproduced
two false passes - a 240 s roll with only a final census, and a census reporting `waits=1 unwitnessed=1
max_wait_ms=4850.0` with no wait line):
- **witness:** the first census within 25 s of the log's start, then census lines at most 25 s apart (an exe
  without the writer FAILs, and so does a log with only the final census);
- **complete:** every census `bytes=` exact and `lost=0`, the last census within 25 s of the end, F frame
  numbers +1;
- **waits:** no producer wait line over 100 ms, and every census agrees with the file: `max_wait_ms` <= 100 (it
  covers waits whose witness line found no room) and `waits=` = the wait lines before it + `unwitnessed=`;
- **cp** (census rolls): after +60 s, the largest F-line interval under 1000 ms (the old logger's signature was
  0.7-4.9 s);
- **pace:** the `[HIGHFPS_PACE]` max under 1000 ms after +60 s;
- **inject:** exactly one `injected=1` write of >= 5000 ms, F lines logged by the CP while it slept, and no
  producer wait (no wait line, and `waits=0` in the last census). This check is the live proof that the CP no
  longer waits for the log's disk.

---

## 4. Known limits (honest)

- **Ghidra VMX128** — ~44K Xenon vector instructions aren't decoded by stock Ghidra; vector-heavy
  functions decompile partially. `va_to_source` (the real executing C++) is authoritative
  **only while its freshness check passes** — see the staleness note below.

**⛔ `va_to_source` is authoritative ONLY IF FRESH (iter-455i).** The index silently served a
five-week-old mapping (`.cache/va_index.db` 2026-05-15 vs generated PPC 2026-08-01) and
mis-attributed *every* line-level lookup tested — in both directions, with no restart needed
for it to keep doing so. It nearly caused a correct finding to be retracted. Since iter-455i
the db stores a `source_fingerprint` of the `guest/ppc/ppc_recomp.*.cpp` inventory and BOTH
`lookup` and `reverse_lookup` **fail closed** (return `stale: true` and refuse) when it does
not match, including for any pre-repair db that has no fingerprint at all. Rebuilds go to a
temp db and are swapped atomically, so an interrupted build can never leave a half-index that
still answers.
  - verify: `python scripts/va_to_source.py --selftest` (proves both directions refuse on a
    modified fingerprint and that a rebuild restores exact bidirectional mapping)
  - repair: `python scripts/va_to_source.py --build` (never commit `.cache/`)
  - if it ever refuses, the CHECKED-IN generated source is the authority: the nearest
    preceding `PPC_FUNC_IMPL(__imp__sub_XXXXXXXX)` marker. Never let a cached mapping
    overrule direct source.
- **L8 resolution is recomp-only** — crash sites map to generated `ppc_recomp.*.cpp`, not kernel stubs;
  only the pattern/endianness tier is proven through a live build.
- **L9 trace size** — the rich golden boot is ~11 GB; always diff with `divergence_bisect.py --stream`.
  Reaching the visible `ScrPressStart` just means capturing longer.
- **L9-B scheduler** — the deterministic record-replay scheduler (byte-identical boot) is designed but
  not built; it's the remaining multi-week piece.
- **Regenerated files** — the L4 hook lives in gitignored `guest/ppc/ppc_context.h`; re-apply from
  `patches/recomp_patches.md` after a regen. Re-run refresh-ghidra after a regen too.
